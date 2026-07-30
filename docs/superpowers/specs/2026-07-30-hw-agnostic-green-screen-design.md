# Hardware-agnostic AI Green Screen (ONNX Runtime backend) — Design

**Date:** 2026-07-30
**Supersedes:** issue #24 (AMD-specific DirectML framing)
**Related:** issue #25 (kept open, research comment added — see §10)

## 1. Goal

AI Green Screen currently requires NVIDIA Maxine (RTX-only). Add a second,
hardware-agnostic green-screen engine that runs on **any hardware — AMD, Intel,
non-RTX NVIDIA, and CPU-only boxes — on both Linux and Windows**, and make the
engine **user-selectable** so RTX users can A/B the GPU (Maxine) and CPU (ONNX)
paths side by side.

Non-goal: replacing Maxine. Maxine remains the quality tier on RTX.

## 2. Research summary (why ONNX Runtime, why not MediaPipe-the-framework)

- **MediaPipe framework: rejected.** Its Tasks C API does cover ImageSegmenter,
  but the framework is Bazel-only, ships zero official prebuilt desktop
  binaries (through v1.0.0, 2026-07-28), has no Windows `.dll` build target and
  no Windows GPU delegate. Unmaintainable inside a CMake + vcxproj shim.
- **MediaPipe's model without MediaPipe: adopted.** `selfie_segmentation`
  (256×256, 106K params, ~450 KB, Apache-2.0) is a plain network with no
  custom ops; ONNX conversions exist (PINTO model zoo #109). Real-time on a
  plain desktop CPU — a few ms per frame.
- **Runtime: ONNX Runtime (MIT).** Official prebuilt C libraries for
  Linux x64 (~9 MB) and Windows x64; stable C API reachable through a single
  export (`OrtGetApiBase`), ideal for dlopen. CPU EP is the baseline and is
  sufficient for this model.
- **Production precedent:** `obs-backgroundremoval` (GPL-3.0, 4.4k stars) runs
  this exact stack — C++ plugin + ONNX Runtime + these very models, CPU
  inference, Windows/Linux/macOS. Cloned to `~/dev/obs-backgroundremoval` as a
  **read-only reference**; its code is GPL and must not be copied into this MIT
  repo. Its model files are Apache-2.0 (SPDX sidecars) but we fetch models from
  upstream pinned URLs, not from that repo.
- **DirectML (issue #24's proposal): rejected as the primary path.**
  Windows-only and in maintenance mode (`Microsoft.ML.OnnxRuntime.DirectML`
  frozen at 1.24.4 while core ORT is at 1.28). GPU EPs (DirectML, WebGPU) can
  be added later behind the same backend if CPU inference ever proves
  insufficient — for this model it does not.
- **Frame interpolation (issue #25): no hardware-agnostic path exists.** The
  only cross-vendor option is RIFE on ncnn-Vulkan (MIT): real-time at 720p on
  midrange discrete GPUs, 1080p needs ~RTX-3070 class, CPU not viable.
  Out of scope here; findings go to #25.

## 3. Architecture

Two engines behind one contract, selected by the user:

- `Aigs` (existing Maxine engine) — **untouched**.
- `SegOnnx` (new, `native/shim/seg_onnx.{h,cpp}`) — same method shape:
  `Probe(detail)`, `Start()`, `Stop()`,
  `ProcessFrame(bgra, w, h, expand, feather)`, `IsReady()`, `LastError()`.

The capture worker owns both objects and resolves the requested backend each
loop iteration:

- `green_screen_backend` param: **0 = Auto** (Maxine if its probe passed, else
  ONNX), **1 = Maxine**, **2 = ONNX CPU**.
- Selecting an unavailable backend behaves like green screen unavailable
  (passthrough + detail text); the UI greys those options out anyway.
- Changing the backend while running stops the old engine and starts the new
  one on the worker thread (one-frame hiccup accepted). Same worker-thread
  affinity rule as Maxine: the UI only writes atomics; engine objects are
  worker-thread-local.

**Shared matte ops.** The dilate → feather → premultiply chain moves out of
`aigs.cpp` into `matte_ops.{h,cpp}`, used by both engines, so the
expand/feather sliders behave identically — which is what makes the A/B toggle
a fair comparison.

## 4. SegOnnx internals

- **Loading:** `dlopen`/`LoadLibraryW` of the ONNX Runtime shared library at
  runtime; resolve `OrtGetApiBase`, use the C API function table. No import
  library, no build-time SDK, no `COS_HAS_*` flag — the code always compiles
  on both platforms; the probe decides availability at runtime. ORT's MIT C
  header is vendored into the repo.
- **Runtime resolution (first hit wins):** `COS_SEG_RUNTIME_DIR` (dev
  override) → `<shim>/onnx/` (bundled: ORT lib + model). Neither present →
  probe fails with a clear detail string.
- **Per frame (in place, on the worker thread):**
  1. BGRA frame → bilinear downscale to 256×256, BGR→RGB, normalize to float.
  2. Single session run (CPU EP; a few ms on a modern x64 core).
  3. Output confidence mask → bilinear upscale to frame size.
  4. Shared matte ops: dilate(expand) → blur(feather) → A = matte, RGB
     premultiplied. Identical contract to Aigs: returns `true` if applied,
     `false` leaves the buffer untouched.
- **Model:** `selfie_segmentation` 256×256 ONNX. Exact upstream URL + sha256
  pinned at plan stage (PINTO zoo #109 / Google storage). Output-channel
  semantics verified at implementation (the Meet-variant outputs a 2-channel
  float mask; selfie_segmentation outputs 1 — the smoke tool asserts this).

## 5. C ABI changes (struct parity is load-bearing)

- `CosParams` gains `int green_screen_backend;` (appended).
- `CosCaps` gains `int green_screen_onnx_available; char gs_onnx_detail[256];`
  (appended).
- The C structs (`shim.h`) and the `[StructLayout(Sequential)]` mirrors in
  `PInvokeShim` (both apps' copies) are updated together, same commit.
- `cos_query_capabilities` additionally runs `SegOnnx::Probe` and fills the new
  gate + detail. Existing Maxine gates unchanged.

## 6. C# side

- **Core:** contract mirrors; `Orchestrator` exposes ONNX availability +
  passes `GreenScreenBackend` through `BuildParams`; capability text includes
  the provider (e.g. "NVIDIA Maxine" / "ONNX CPU").
- **Avalonia panel:** backend dropdown — Auto / NVIDIA Maxine / ONNX CPU —
  visible when green screen is available at all, individual options greyed by
  availability. On an RTX box both are live: that is the A/B comparison
  control. Live push via the existing `On…Changed` → `ApplyLiveParams` path
  (gated on `IsRunning` as today). Selection persists to config —
  `ToAppConfig` must copy it (known revert-to-defaults gotcha).
- **WinUI panel:** native + Core changes are cross-platform now; the XAML
  dropdown is deferred to the Windows re-home (#38) since it cannot be built
  or verified on this box.

## 7. Error handling

Mirrors Aigs exactly: `Probe`/`Start` failures produce a human-readable detail
("ONNX Runtime library not found", "model file missing", "session create
failed: …"); `ProcessFrame` failure returns `false`, the frame passes through
opaque, and the error surfaces via status polling. No new mechanisms.

## 8. Packaging & licensing

- `scripts/publish-linux.sh` downloads the pinned ORT release tarball and the
  model (both sha256-verified, cached) and stages `dist/linux/onnx/`
  (~10 MB total). No binaries in git.
- Windows installer staging: same layout under `<app>\onnx\`, wired when #38
  re-homes Windows releases.
- `THIRD-PARTY-NOTICES.md` += ONNX Runtime (MIT) + MediaPipe Selfie
  Segmentation model (Apache-2.0). Both licenses are redistribution-friendly —
  simpler than every NVIDIA SDK already shipped.

## 9. Testing

- **Core xUnit:** backend gating (auto-resolution, unavailable-backend
  behavior), params passthrough, config persistence.
- **Native smoke:** `native/shim/smoke/seg_probe` — loads the bundled runtime,
  feeds a synthetic frame, asserts a plausible matte + prints per-frame
  timing. Modeled on `v4l2_probe`/`effects_drive`.
- **CI:** the hosted Linux job downloads ORT + model and runs `seg_probe` —
  CPU-only inference means **the first AI effect fully verifiable on hosted
  CI, no GPU required**. The RTX-side A/B of both backends is the human
  visual gate below, not a CI job.
- **Visual gate:** overlay + OBS capture, human check per
  `docs/superpowers/verification/` (matte quality is a judgment call).

## 10. Issue handling

- **#24:** retitle + comment to hardware-agnostic ONNX framing (this design);
  closed by this work when it ships.
- **#25:** research comment added (no hardware-agnostic FRC exists;
  RIFE-ncnn-Vulkan is the only cross-vendor option, midrange-dGPU-bound);
  stays open, unscoped here.

## 11. Out of scope

- Eye contact / super-res on non-NVIDIA hardware (no comparable open models).
- Frame interpolation backends (see #25).
- GPU execution providers (DirectML/WebGPU) — add behind `SegOnnx` only if CPU
  inference proves insufficient on real hardware.
- Multi-model menu (RVM, PP-HumanSeg, …) — single pinned model until quality
  feedback demands options. RVM specifically is GPL-3.0 and cannot ship with
  this MIT app.

## 12. Acceptance criteria

- On RTX: Maxine still works; dropdown offers Maxine + ONNX; switching live
  visibly changes matte character (the A/B).
- On non-NVIDIA / CPU-only Linux: green screen enables via ONNX, ~30 fps
  sustained at 720p.
- Runtime absent: toggle greyed with clear reason; no crash, no startup cost.
- Builds pristine (0 warnings) on both build systems; hosted CI runs
  `seg_probe` green with real inference.
