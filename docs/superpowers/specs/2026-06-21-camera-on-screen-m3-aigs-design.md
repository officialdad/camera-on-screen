# Camera-on-Screen — M3 Design Spec: AI Green Screen (CPU-copy)

**Date:** 2026-06-21
**Status:** Approved (design); pending implementation plan
**Depends on:** M1 (Core) + M2 (App + shim + overlay passthrough), both merged to `main`.
**Parent spec:** `docs/superpowers/specs/2026-06-20-camera-on-screen-design.md` (§3 architecture, §6 data flow, §8 milestone M3).

## 1. Goal

Show the webcam subject on a **transparent background**, live, through the existing
DirectComposition overlay, using NVIDIA Maxine **AI Green Screen** (AIGS). The matte is
produced in the native shim; its per-pixel alpha flows unchanged into the overlay (which
already composites premultiplied alpha). A screen recorder then captures the subject with a
transparent background in one pass — the headline product value, with no physical green
screen and no post-edit.

This is the MVP-defining increment: M2 ships raw webcam on an opaque rectangle; M3 makes
the background disappear.

## 2. Scope

**In scope (M3):**
- Maxine VFX SDK integration in the shim (link, init, model load, error handling).
- The **GreenScreen** effect run per frame on the GPU.
- **CPU-copy** interop: frames travel CPU→GPU→CPU around the effect (parent spec §3.3 "v1
  fallback"). No CUDA↔D3D11 zero-copy.
- Per-pixel alpha (the matte) carried through the existing CPU frame buffer to the overlay.
- A real Maxine **availability probe** replacing the RTX-substring heuristic as the effect
  gate.
- Green screen **on/off** (toggle already exists in the control panel).

**Out of scope (deferred, logged):**
- **Eye Contact (M4).** Finding during M3 design: Eye Contact is **not** in the VFX SDK —
  it lives in the separate **Maxine AR SDK** (not installed). The parent spec's assumption
  that "one native pipeline covers both" via the VFX SDK is wrong for Eye Contact. M4 will
  need the AR SDK as a separate dependency.
- **GPU/D3D11 zero-copy interop.** A later optimization (see §9). It buys no user-visible
  performance at the current resolution on the target GPU; it is an architectural cleanup.
- **Green-screen strength slider**, denoise, relighting, auto-framing.
- **Deployment/bundling** of the CUDA/TensorRT runtime + models, and the Maxine
  redistribution **license review** — a ship-time concern (parent spec Risks).

## 3. Hardware / SDK facts (as installed)

- SDK: **Maxine VFX SDK**, GreenScreen feature `nvvfxgreenscreen` 1.2.0.0, installed to a
  user-chosen directory (target machine: `…/claude-code/VideoFX`). Models built for compute
  capability **86** (RTX 3090, Ampere) — `bin/models/AIGS_288x512_86_m0..m3.engine.trtpkg`.
- **Headers:** `nvvfx/include/` (`nvVideoEffects.h`, `nvCVImage.h`, `nvCVStatus.h`,
  `nvTransferD3D11.h`) and `features/nvvfxgreenscreen/include/nvVFXGreenScreen.h` (defines
  `NVVFX_FX_GREEN_SCREEN "GreenScreen"`).
- **No import `.lib`.** The SDK ships **proxy stubs** `nvvfx/src/nvVideoEffectsProxy.cpp`
  and `nvCVImageProxy.cpp`, compiled into the consumer; they `LoadLibrary` the runtime DLLs.
  This is the standard Maxine linking pattern.
- **Runtime DLLs** (`bin/`): `NVVideoEffects.dll`, `NVCVImage.dll`, plus a heavy dependency
  chain — CUDA (`cudart64_12`, `cublas*`, `npp*`), TensorRT (`nvinfer_10`,
  `nvonnxparser_10`), `nvngxruntime`, `nvrtc`, and `features/nvvfxgreenscreen/bin/
  nvVFXGreenScreen.dll`. All must be loadable at runtime.
- **Effect API** (from `nvVideoEffects.h`): `NvVFX_CreateEffect`, `NvVFX_SetString`
  (`NVVFX_MODEL_DIRECTORY`), `NvVFX_SetCudaStream` (`NVVFX_CUDA_STREAM`), `NvVFX_SetU32`
  (`NVVFX_MODE`, `NVVFX_TEMPORAL`), `NvVFX_SetImage` (`NVVFX_INPUT_IMAGE` /
  `NVVFX_OUTPUT_IMAGE`), `NvVFX_Load`, `NvVFX_Run`, `NvVFX_DestroyEffect`. CUDA stream via
  `NvVFX_CudaStreamCreate/Destroy`. `NvCVImage_*` (alloc, transfer) for the images.

## 4. Architecture

All new native work lives in the shim. **C# windowing, compositing, and the overlay render
path are unchanged.** This is the key payoff of the CPU-copy choice: the existing
`cos_get_frame` → managed dynamic-texture upload → overlay present path carries the matte
with no change, because alpha is just the fourth byte of the same BGRA buffer.

### 4.1 New native module: `aigs.{h,cpp}`

A thin wrapper around the Maxine GreenScreen effect, owning:
- the effect handle, the CUDA stream, and the persistent GPU `NvCVImage`s (input + matte);
- `Initialize(modelDir)` → create effect, set model dir / stream / mode / temporal, `Load`;
  returns success/failure (no throw across the C ABI);
- `ProcessFrame(bgra, width, height)` → run the effect and composite the matte into the
  BGRA buffer in place (alpha = matte, premultiply RGB);
- `Shutdown()` → destroy effect, stream, images;
- `IsReady()` and a last-error string.

The effect handle, stream, and GPU images are created **once** (on first frame or at start),
reused across frames, and destroyed on stop/shutdown — never per frame.

### 4.2 Where it runs

Inside the existing capture worker thread (`capture.cpp` `WorkerLoop`), after Media
Foundation produces the BGRA frame and **before** it is published to the shared frame buffer.
Capture remains the single producer; AIGS is an in-line transform on the same thread. No new
thread, no change to the capture/lifecycle mutex design.

### 4.3 The swappable seam (forward-compat with the M-future GPU path)

`aigs.cpp` isolates the three pieces that the future GPU/zero-copy path (§9) will replace:
1. **upload** — CPU BGRA → GPU input image;
2. **download** — GPU matte → CPU;
3. **composite** — apply matte to the BGRA buffer (CPU).

These are separate, named functions behind the `ProcessFrame` boundary so the GPU path later
swaps the plumbing (~30% of the frame path) without touching the Maxine core (effect
lifecycle, model load, `Run`, the probe) — which is identical in both designs.

## 5. Per-frame pipeline (CPU-copy)

```
Media Foundation RGB32 → CPU BGRA frame (capture.cpp, as today)
   │
   ├─ green screen OFF, or AIGS not ready:
   │     existing behavior — force alpha = 0xFF (opaque passthrough)   [M2 path]
   │
   └─ green screen ON and AIGS ready:
         upload:    CPU BGRA → GPU NvCVImage (BGR u8)  via NvCVImage_Transfer(stream)
         run:       NvVFX_Run(GreenScreen, async=0)     → GPU matte (single-channel A8)
         download:  GPU matte → CPU                      via NvCVImage_Transfer(stream)
         composite: for each pixel: A = matte;  RGB = RGB * matte / 255  (premultiplied)
   │
   ▼
shared frame buffer  →  cos_get_frame  →  C# dynamic texture  →  DComp overlay (unchanged)
```

- **Matte semantics:** single-channel 8-bit, `0` = background, `255` = foreground; same
  width/height as the frame.
- **Premultiplied alpha** is required because the overlay swap chain is
  `AlphaMode.Premultiplied` (parent spec §4, M2). The shim emits premultiplied BGRA; the
  overlay is untouched.
- **Temporal mode** `NVVFX_TEMPORAL = 1` (video) to reduce frame-to-frame matte flicker.
- **Mode** `NVVFX_MODE` is fixed in M3 to a single sensible quality tier; the exact constant
  and the AIGS input pixel format (BGR vs BGRA, chunky/planar) are verified against
  `nvCVImage.h` enums and the AIGS sample during implementation. `green_screen_strength` is
  **not** consumed in M3 (on/off only).
- **Staging:** `NvCVImage_Transfer` format/layout conversion may require a persistent staging
  GPU image; if so it is allocated once alongside the input/matte images, not per frame.

## 6. Contract changes

### 6.1 Native C ABI (`shim.h`)

- `CosParams.green_screen_enabled` — **now acted on**: gates whether `ProcessFrame` runs the
  effect (already in the struct since M1).
- `CosParams.green_screen_strength` — accepted but **unused in M3** (reserved).
- `CosStatus.green_screen_active` — set to `1` only while AIGS is actually transforming
  frames; `0` in passthrough.
- `CosStatus.error[256]` — carries the Maxine failure message when init/load/run fails.
- **New export:** `int cos_query_capabilities(CosCaps* out)` — the real availability probe
  (§7). New `CosCaps` struct with at minimum `int green_screen_available` and a
  `char detail[256]`. A `[StructLayout(LayoutKind.Sequential)]` mirror is added on the
  managed side; **byte-for-byte x64 parity** is load-bearing (parent spec / CLAUDE.md rule).

### 6.2 Managed (`CameraOnScreen.Core` + `App`)

- `INativeShim` gains `QueryCapabilities()` returning a small managed record (available +
  detail). `FakeShim` returns **unavailable** so Core tests need no SDK and CI stays green.
  `PInvokeShim` calls `cos_query_capabilities`.
- **Effect gate moves** from `GpuTierDetector` (RTX-substring) to `QueryCapabilities`. The
  orchestrator enables effect toggles iff `green_screen_available`. `GpuTierDetector` is
  retained only to display the GPU name/tier string; it no longer gates effects.
- No change to the overlay, the present loop, `cos_get_frame`, or the frame-pump timer.

## 7. Availability probe, error handling, GPU gating

- **Probe** (at init / first start): read `COS_VFX_SDK_DIR`; if set, `SetDllDirectoryW(<dir>/
  bin)` so the proxy stubs can load the runtime DLLs; then create the GreenScreen effect with
  `NVVFX_MODEL_DIRECTORY = <dir>/bin/models` and call `NvVFX_Load`. Success of all steps →
  `green_screen_available = 1`.
- **Any failure** — env var unset, DLL not loadable, model load fails, GPU unsupported →
  `green_screen_available = 0`, the control panel disables the toggle with a clear note, and
  capture runs **opaque passthrough** (the M2 behavior). The failure detail is surfaced via
  `CosStatus.error` / `CosCaps.detail`.
- **Crash safety:** all Maxine calls are inside the shim and guarded; a Maxine fault degrades
  to passthrough and reports an error — it never takes down the UI process. P/Invoke calls
  remain guarded as today.
- **Behavior on the target machine:** RTX 3090 + SDK installed + `COS_VFX_SDK_DIR` set →
  effects available. On any machine without the SDK (including CI), the app runs exactly as
  M2.

## 8. Build, runtime, deployment

- **Build:** `native/shim/shim.vcxproj` adds the two SDK include directories and compiles the
  two proxy stubs (`nvVideoEffectsProxy.cpp`, `nvCVImageProxy.cpp`) into the shim. **No
  `.lib`** is referenced. The SDK location is supplied to MSBuild via a property/environment
  variable (default to `COS_VFX_SDK_DIR`) so no absolute path is hardcoded in the repo. The
  shim still builds without the SDK present only if the include path is satisfied; when the
  SDK is absent the shim should still compile to the M2 passthrough (guard the Maxine code so
  a missing SDK is a runtime "unavailable", not a build break) — verified during
  implementation.
- **Runtime (dev):** set `COS_VFX_SDK_DIR=…\VideoFX`. The shim adds its `bin` to the DLL
  search path. No multi-GB SDK DLLs enter git or the build output.
- **Deploy (ship):** bundling the CUDA/TensorRT DLLs + models, validating on a clean machine,
  and the Maxine **license review** (`NVIDIA-Open-Model-License`, `NVIDIA-Software-License`
  PDFs shipped in the feature dir) are **out of scope for M3**, logged for ship time.

## 9. Future: GPU/D3D11 zero-copy (not M3)

The eventual optimization (parent spec §3.3 end-goal): the shim writes the AIGS output
straight into the C#-owned D3D11 texture via `nvTransferD3D11` on the shared device, removing
the CPU round-trips and the managed dynamic-texture upload. It activates the shared-device
contract (the `D3DDevicePtr` already passed to `cos_init` but unused), CUDA↔D3D11 resource
mapping, and cross-API synchronization (CUDA stream on the worker thread vs the D3D context
on the present/UI thread). Because §4.3 isolates upload/download/composite, this replaces
plumbing only; the Maxine core built in M3 is reused unchanged. Deferred because it buys no
user-visible performance at the current resolution on the target GPU and carries materially
higher interop risk that is better taken on against a proven matte.

## 10. Testing

- **Core unit tests (no SDK, CI-safe):** orchestrator gates effects on `QueryCapabilities` —
  FakeShim "unavailable" → toggles disabled, passthrough; "available" → toggles enabled.
  Params round-trip `green_screen_enabled`. Status maps `green_screen_active` / `error`.
- **Native smoke (target machine, SDK present):** create the effect, run AIGS on a captured
  (or synthetic) frame, assert the matte contains both background (`0`) and foreground
  (`255`) regions and matches frame dimensions — proves a real, non-degenerate matte.
- **Manual gate (human at screen):** with the SDK installed and `COS_VFX_SDK_DIR` set, the
  overlay shows the subject on a transparent background; toggling green screen on/off switches
  between matte and opaque; a screen recorder captures the transparent-background subject
  cleanly. Extends `docs/superpowers/verification/2026-06-20-recorder-capture.md`.

## 11. Open implementation details (resolved during build, not blocking design)

- Exact `NVVFX_MODE` constant and whether to expose mode selection later.
- AIGS input pixel format and whether a staging `NvCVImage` is required for the
  `NvCVImage_Transfer` conversions.
- Whether to run the probe in `cos_init` vs lazily on first `cos_start`.
- Whether the shim guards Maxine includes behind a build flag so the vcxproj builds with no
  SDK at all (CI), versus requiring only the headers at build time.
