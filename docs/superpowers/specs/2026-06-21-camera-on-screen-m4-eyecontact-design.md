# Camera-on-Screen M4 — Eye Contact (AI gaze redirection)

Date: 2026-06-21
Status: Design approved; implementation plan next.

## Purpose

Add NVIDIA Maxine **Eye Contact** (gaze redirection) to the overlay as a second live
effect alongside the M3 AI Green Screen. When enabled, the user's gaze is redirected
toward the camera in real time so any screen recorder captures it live, one pass — same
"no post-edit" intent as the rest of the app.

Eye Contact lives in the Maxine **AR SDK** (NvAR), a **separate** dependency from the
green screen's VFX SDK (NvVFX). M4 introduces that SDK, a second native effect wrapper, a
second capability gate, and the UI wiring for the already-stubbed Eye Contact toggle.

## Scope (YAGNI)

- **Eye Contact = a single toggle.** No exposed parameters (temporal filtering, eye-size
  sensitivity, head-pose threshold) — SDK defaults only. Sliders can be added later if the
  defaults look wrong on screen.
- **Coexists with Green Screen.** Each effect toggles independently. When both are on the
  worker runs `EyeContact → GreenScreen` per frame.
- **No zero-copy.** CPU-copy interop, same as M3 (frames round-trip CPU↔GPU around the
  effect). Zero-copy stays deferred.
- **Dev + verify target is the installed RTX 3090 (Ampere, compute 86) AR variant.**
  Multi-GPU model variants are a ship-time concern (see Deferred).

## Architecture — the pipeline

New `native/shim/eyecontact.{h,cpp}` wraps `NvAR_Feature_GazeRedirection`. Per the
official `samples/GazeRedirect/GazeEngine.cpp`, GazeRedirection is **one NvAR feature that
internally performs face detection + landmark tracking + gaze redirection** — the runtime
path (`acquireGazeRedirection`) is just `Transfer(in) → NvAR_Run(handle) → Transfer(out)`.
(The sample also declares separate `faceDetectHandle` / `landmarkDetectHandle` for its
diagnostic side-by-side visualization; those are **not** needed for redirection and the
shim omits them.) Its output is a **full redirected frame** (not a matte), so the composite
is a straight frame replacement.

**Exact NvAR sequence (from `GazeEngine.cpp`, no OpenCV — we wrap NvCVImage directly):**
- **Create:** `NvAR_CudaStreamCreate(&stream)`;
  `NvAR_Create(NvAR_Feature_GazeRedirection, &h)`.
- **Config (then `NvAR_Load(h)`):** `NvAR_SetString(h, NvAR_Parameter_Config(ModelDir),
  modelDir)`; `NvAR_SetU32(Landmarks_Size, 126)`; `NvAR_SetU32(Temporal, 0xFFFFFFFF)` (all
  filtering on); `NvAR_SetU32(GazeRedirect, 1)`; `NvAR_SetCudaStream(CUDAStream, stream)`;
  `NvAR_SetU32(UseCudaGraph, 1)`; `NvAR_SetU32(EyeSizeSensitivity, 3)`.
- **I/O bind (once, on first frame when size known):** input `inputImageBuffer` and output
  `outputImageBuffer` are **`NVCV_BGR`, `NVCV_U8`, `NVCV_CHUNKY`, `NVCV_GPU`** (note: BGR,
  *not* BGRA). `NvAR_SetObject(Input(Image))`, `NvAR_SetObject(Output(Image))`,
  `NvAR_SetS32(Input(Width/Height))`. The SDK **also requires these output buffers bound**
  even though the shim ignores them: `Output(Landmarks)` (NvAR_Point2f ×Landmarks_Size),
  `Output(GazeOutputLandmarks)` (×12), `Output(LandmarksConfidence)` (f32 ×Landmarks_Size),
  `Output(OutputGazeVector)` (f32 ×2), `Output(OutputHeadTranslation)` (f32 ×3),
  `Output(HeadPose)` (NvAR_Quaternion), `Output(GazeDirection)` (NvAR_Point3f ×2),
  `Output(BoundingBoxes)` (NvAR_BBoxes). Skipping any returns a parameter error from Load/Run.
- **Per frame:** wrap the CPU BGRA buffer as a source `NvCVImage` →
  `NvCVImage_Transfer(src → inputImageBuffer, 1.0, stream, &tmpImage)` (converts BGRA→BGR);
  `NvAR_Run(h)`; `NvCVImage_Transfer(outputImageBuffer → dst, 1.0, stream, &tmpImage)`
  (converts BGR→BGRA, alpha set to 255). `tmpImage` is the staging buffer for the
  format/layout conversion (analogous to AIGS's `stage`).
- **Low-confidence:** when no face is found / `LandmarksConfidence` is low, `NvAR_Run` still
  writes `outputImageBuffer` (redirection ≈ identity), so the shim always uses the output —
  no special-casing. (The sample returns an error here only for its UI; the frame is valid.)

The `EyeContact` object is **worker-thread-local** (NvAR/CUDA stream has thread affinity),
exactly like the M3 `Aigs` object. `cos_set_params` (UI thread) only flips an atomic
enable flag; status crosses threads via atomics + a leaf-lock (never nested under the
existing state/lifecycle mutexes), mirroring `gsErrMtx`.

### Per-frame worker chain (capture WorkerLoop)

```
capture BGRA
   → [EyeContact ON?]  gaze-redirect → full RGB        (NvAR, runs first: needs real eyes/landmarks)
   → [GreenScreen ON?] matte + premultiply alpha       (NvVFX, unchanged from M3)
   → overlay (CPU buffer → PresentFrame)
```

- **Eye Contact runs first**, on the raw frame, because it needs the real eyes and face
  landmarks before the background is removed.
- **Green Screen runs on Eye Contact's output**, unchanged from M3 (matte = A,
  RGB *= matte/255 premultiplied, honoring matte pitch + dilate/feather).
- Each stage is gated by its own atomic flag. Neither on → passthrough (alpha forced 255,
  as today).
- **CPU-copy seam preserved:** Upload (CPU BGRA→GPU) / Run / Download (GPU→CPU) are the
  same swappable seam as M3, kept deliberately for a future zero-copy path.

### Two Maxine runtimes side by side

The VFX SDK (green screen, located via `COS_VFX_SDK_DIR`) and the **AR SDK** (eye contact,
located via a new `COS_AR_SDK_DIR`) load independently. Each can be present or missing on
its own, so each gets its **own capability probe and its own gate**. Both effect objects
live on the same capture worker thread.

**Coexistence risk: low (verified).** Both SDKs are the same runtime generation — each ships
`cudart64_12`, `nvinfer_10`/`nvinfer_plugin_10`/`nvonnxparser_10` (CUDA 12 / TensorRT 10) and
`NVCVImage.dll`. The shared `NVCVImage.dll` (whichever loads first wins; same API version) is
compatible, so running both Maxine pipelines in one process has no version clash. The
remaining concern is purely throughput (two effects per frame) — covered by the perf gate.
`SetDllDirectory` is per-process and single-valued; each proxy sets it before its own first
DLL load and the OS caches loaded modules by name, so the green-screen (VFX bin) and
eye-contact (AR root) DLLs each resolve correctly despite the shared setting.

## C ABI — changes

- **No `CosParams` change.** The entire Eye Contact param set was already plumbed
  end-to-end at M1 (over-stubbed) across all 5 parity sites — `CosParams` (`shim.h`),
  `PInvokeShim`, Core `Contracts.cs`, `BuildParams`, and the config model:
  `eye_contact_enabled`, `eye_contact_sensitivity` (double), `eye_contact_look_away_range`
  (double). `CosStatus` already has `eye_contact_active`. M4 wires these existing fields to
  the new effect; `cos_set_params` flips `eye_contact_enabled` like the green-screen flag
  (atomic, UI-thread-safe). The two doubles are **kept** (the native side maps
  sensitivity → eye-size sensitivity and look-away-range → head-pose disengage threshold,
  starting at their defaults); the UI just doesn't expose sliders yet (toggle-only is a UI
  decision, not an ABI one), so they remain available for a later slider pass with no
  further ABI churn.
- **`CosCaps` grows (Option A — one probe, one struct).** Extend:
  ```c
  struct CosCaps {
      int  green_screen_available;
      char detail[256];          // green screen reason
      int  eye_contact_available; // NEW
      char ec_detail[256];        // NEW — eye contact reason
  };
  ```
  One `cos_query_capabilities` call returns both gates. The
  `[StructLayout(LayoutKind.Sequential)]` mirror in `PInvokeShim` must match byte-for-byte
  on x64 (the existing 260-byte struct becomes 524 bytes: 4 + 256 + 4 + 256). `detail` /
  `ec_detail` are UTF-8. Verify with `dumpbin /exports` + a struct-parity check after the
  change.
- **No new exports** (9 stays 9; the second gate rides in the existing
  `cos_query_capabilities`).

## Build guard + SDK discovery

- **New guard `COS_HAS_MAXINE_AR`**, defined when `COS_AR_SDK_DIR` is set at build time
  (independent of `COS_HAS_MAXINE` for green screen). Without it the eye-contact path is a
  passthrough **stub** that reports `"AR SDK not built in"`, so CI / no-SDK machines still
  build and Core tests still pass. The two guards are orthogonal: a machine can have one
  SDK, both, or neither.
- **Two-location SDK (differs from VFX).** Unlike the VFX SDK (one tree with headers, src,
  and `bin/` runtime), the AR SDK splits into:
  1. **Source** — the GitHub repo clone `https://github.com/NVIDIA-Maxine/Maxine-AR-SDK`
     (provides `nvar/include/*.h` and the proxy stubs `nvar/src/nvARProxy.cpp` +
     `nvar/src/nvCVImageProxy.cpp`). **Build-time only.** `COS_AR_SDK_DIR` points here.
  2. **Runtime** — the installer output at `%ProgramFiles%\NVIDIA Corporation\NVIDIA AR
     SDK\` (the DLLs `nvARPose.dll`, `NVCVImage.dll`, CUDA 12 + TensorRT 10 chain, directly
     in the root — **not** a `bin/` subdir) and `…\NVIDIA AR SDK\models\` (the
     `gazeredir_*_86`, `face_detection_86`, `faceland_*_86` engines — compute **86** = RTX
     3090). **Not** in the repo; comes from the NGC/installer download.
- **Link via proxy stubs** (no import `.lib`, same idea as VFX): compile
  `$(CosArSdkDir)\nvar\src\nvARProxy.cpp` into the shim. **Do NOT also compile the AR
  tree's `nvCVImageProxy.cpp`** — the VFX build already compiles `nvCVImageProxy.cpp` and a
  second copy is a duplicate-symbol link error (`NvCVImage_*`). The single VFX-provided
  `nvCVImageProxy.cpp` serves both SDKs (same NvCVImage API + shared `NVCVImage.dll`). When
  the shim is built **AR-only** (VFX guard off), compile the AR tree's `nvCVImageProxy.cpp`
  instead — exactly one copy must be in the build. (vcxproj conditions handle this; see plan.)
- **Runtime DLL discovery.** The proxy global `extern char* g_nvARSDKPath;` (defined in
  `eyecontact.cpp`, mirroring `g_nvVFXSDKPath`) is `SetDllDirectory`'d before loading
  `nvARPose.dll`. The proxy **auto-falls back** to `%ProgramFiles%\NVIDIA Corporation\NVIDIA
  AR SDK\` when the global is left null — so for a default install no path wiring is needed.
  The shim sets it explicitly from `COS_AR_RUNTIME_DIR` (env var, **optional**, defaults to
  that Program Files path) to support non-default installs and ship-time relocation.
- **ModelDir** passed to `NvAR_Parameter_Config(ModelDir)` = `<runtime>\models` (i.e.
  `%COS_AR_RUNTIME_DIR%\models`, default `%ProgramFiles%\NVIDIA Corporation\NVIDIA AR
  SDK\models`). **Not** under the repo / `COS_AR_SDK_DIR`.
- **Env summary:** `COS_AR_SDK_DIR` (build, = repo clone, required for a non-stub build);
  `COS_AR_RUNTIME_DIR` (runtime, optional, defaults to the Program Files install). The app
  needs neither in its launch env for a default install (proxy auto-resolves) — set
  `COS_AR_RUNTIME_DIR` only for a relocated install.
- **Deploy gotcha (same shape as M3):** the AR-SDK build and the CI stub write the **same**
  shim DLL path. Build the SDK config **last**, then `-t:Rebuild` the App, or it silently
  deploys the stub (eye-contact toggle greyed). Verify the deployed DLL: `GazeRedirection`
  present, `AR SDK not built in` absent (`grep -a`).

## VM / UI / gating

- **Split the capability gate.** Today one `EffectsAvailable` gates both toggles. Add a
  second VM flag `EyeContactAvailable` driven by `CosCaps.eye_contact_available`. The
  green-screen toggle keeps `EffectsAvailable`; the eye-contact toggle binds
  `EyeContactAvailable`. Each toggle gets its **own** disabled-state note bound to its own
  reason string (green-screen `CapabilityDetail` + a new eye-contact detail), so a machine
  with the VFX SDK but no AR SDK shows green screen enabled and eye contact greyed with its
  real reason.
- **Live push (M3 pattern).** `MainViewModel.OnEyeContactEnabledChanged` →
  `ApplyLiveParams()` → `Orchestrator.ApplyParams(BuildParams())` → `shim.SetParams`, gated
  on `IsRunning`. `eye_contact_enabled` is already in `BuildParams`.
- **Orchestrator.** `ProbeCapabilities()` sets **both** `EffectsAvailable` and
  `EyeContactAvailable` from the single probe. `ApplyParams` forces `eye_contact_enabled`
  off when `EyeContactAvailable` is false — the same gate green screen already gets.
- **Persistence — already done.** `EyeContactEnabled` (+ `EyeContactSensitivity`,
  `EyeContactLookAwayRange`) already round-trip through `ToAppConfig` / `LoadFrom` and have
  `On…Changed` live-push partials wired. No persistence work for the toggle. (If the
  unexposed sensitivity/look-away props prove distracting, they may stay at defaults — they
  do not need UI.)
- **No new sliders** — toggle-only UI. The stubbed `OnEyeContactEnabledChanged` live-push
  partial already exists; it just needs the `IsRunning` gate + the same effect gate
  treatment green screen got (M3 fix) if not already applied.

## Testing

**Core unit tests (xUnit, net8.0 — no SDK required):**
- Split-gate logic: `EyeContactAvailable` false → `ApplyParams` forces
  `eye_contact_enabled` off; true → honors the VM toggle. Green-screen gate unaffected.
- `BuildParams` carries `EyeContactEnabled`.
- Persistence round-trip for `EyeContactEnabled`.
- `FakeShim` extended with an **independent** eye-contact capability flag + param echo, so
  the split gate is testable without either SDK.
- The native NvAR path is **not** unit-tested (guarded native code, same as M3 `aigs`).

**Build verification:**
- `dumpbin /exports` + struct-parity check after the `CosCaps` change (524 bytes x64).
- Pristine builds: 0 warnings across shim (SDK + stub), App, Core.
- Deployed-DLL grep check (above).

## Human visual gate (RTX 3090, like M3)

The overlay is not GDI-capturable by design (DComp flip-model + `NOREDIRECTIONBITMAP`), so
this is an inherent human gate:
- Gaze redirect visible on screen — eyes look at the camera.
- Live toggle on/off works in real time (validates the live-param push).
- **Both effects on** = gaze-fixed **and** background-removed simultaneously.
- A recorder (NVIDIA ShadowPlay / DWM-hardware path) captures the overlay correctly.
- **Perf check:** holds ~30 fps with both Maxine pipelines (AR + VFX) on. If the 3090
  can't hold it, fall back to mutually-exclusive effects (decision deferred to the verify
  step — design for both-at-once first).
- **Beta quality:** note the head-pose range where gaze redirect disengages / looks
  uncanny (Eye Contact is a beta feature).

Results recorded in `docs/superpowers/verification/2026-06-20-recorder-capture.md` (new M4
section).

## Deferred (ship-time / M5)

- **Multi-GPU AR model variants.** The AR SDK ships GPU-generation-specific variants
  (Turing / Ampere / Ada / Blackwell). The installed variant matches the 3090 (Ampere). To
  ship to other RTX GPUs, choose one: **bundle all variants** (select by detected GPU gen,
  large installer) / **ship generic ONNX and let TensorRT build the engine on first run**
  (small install, slow first launch) / **post-install fetch** the matching variant from
  NGC (smallest base, needs network). Same class of problem as the M3 compute-86-only VFX
  models.
- **App-relative SDK discovery.** Drop `COS_AR_SDK_DIR` in favor of AR runtime + models
  bundled beside the exe (joins the VFX bundling work in M5).
- **License review.** AR SDK redistribution governed by NVIDIA terms + bundled-model
  license — read/comply before publishing (joins the VFX license review).

## Documentation gotcha (recorded for future setup)

The official AR SDK System Guide install section
(`https://docs.nvidia.com/deeplearning/maxine/ar-sdk-system-guide/index.html#install-sdk-assoc-sw`)
links to a **non-existent GitHub repository** — the old `github.com/NVIDIA/MAXINE-AR-SDK`
returns **HTTP 404** (confirmed via the GitHub API during this design). The correct, live
repo is **`https://github.com/NVIDIA-Maxine/Maxine-AR-SDK`** (default branch `main`). Note
this when documenting setup so the next person isn't sent to a dead link.

## References

- AR SDK System Guide: https://docs.nvidia.com/deeplearning/maxine/ar-sdk-system-guide/index.html
- AR SDK repo (correct): https://github.com/NVIDIA-Maxine/Maxine-AR-SDK
- Eye Contact blog: https://developer.nvidia.com/blog/improve-human-connection-in-video-conferences-with-nvidia-maxine-eye-contact/
- M3 design (green screen, pattern this mirrors): `docs/superpowers/specs/2026-06-21-camera-on-screen-m3-aigs-design.md`
- Overall design: `docs/superpowers/specs/2026-06-20-camera-on-screen-design.md`
