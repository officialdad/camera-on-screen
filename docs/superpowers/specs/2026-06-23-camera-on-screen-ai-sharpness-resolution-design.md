# Camera-on-Screen — Design Spec: AI Sharpness & Resolution (Phase 1)

**Date:** 2026-06-23
**Status:** Approved (design); pending implementation plan
**Depends on:** M3 (AI Green Screen) + M4 (Eye Contact) — both merged to `main`. Reuses the
worker-thread effect chain (`aigs.{h,cpp}` pattern), the capability probe, and the live-param push.
**Parent spec:** `docs/superpowers/specs/2026-06-20-camera-on-screen-design.md`.
**Phase 2 (separate, deferred):** fps interpolation — GitHub issue #13.

## 1. Goal

Make the overlay image sharper and higher-resolution than the camera hardware delivers, using
NVIDIA Maxine AI. Two distinct user-visible wins:

1. **Cleaner image** — remove the MJPG/H.264 compression artifacts the webcam emits at
   1080p30 (the source of the "softer than the native camera app" complaint).
2. **More effective resolution** — AI-upscale the source so the overlay stays sharp when shown
   large, not just at widget size.

The overlay display size **varies** (small widget ↔ near-fullscreen), so the design must hold up
at any scale: clean + sharpen at the source for small sizes, real upscaled detail for large sizes.

## 2. Scope

**In scope (Phase 1):**
- **Artifact Reduction** (Maxine VFX `NVVFX_FX_ARTIFACT_REDUCTION`) — removes compression
  artifacts, in-place at source resolution, no dimension change.
- **Super Resolution** (Maxine VFX `NVVFX_FX_SUPER_RES`) — AI upscale, off / 1.5× / 2×, writes a
  larger output buffer.
- **Mipmapped downscale** in the overlay present path — fixes bilinear-minification softness when
  the overlay is shown smaller than the source. Independent of Maxine; ships first.
- **Real fps counter** — replace the hardcoded `cos_get_status` `30.0` stub with a measured value
  so the user can see each effect's cost.
- Independent on/off toggles per effect; capability gating; live-param push; JSON persistence.

**Out of scope:**
- **fps interpolation (Phase 2)** — NVIDIA Optical Flow FRUC, separate SDK, co-version risk.
  GitHub issue #13. Gated on a standalone co-version smoke test before any integration.
- **Automatic GPU-budget management** — no scheduler, no mutual exclusion, no auto-drop. The user
  toggles effects and watches the fps readout (explicit decision).
- **Maxine Video Noise Removal / Upscale (non-AI) / relighting** — not needed for this goal.
- **3×/4× Super Res** — capped at 2× (see §7).

## 3. Why this is low-risk: no new co-version

Super Resolution and Artifact Reduction live in the **same Maxine VideoFX SDK** already integrated
for green screen (VFX 1.2.0.0). Same `NVVideoEffects.dll` dispatcher, same per-feature-DLL model,
**same TensorRT 10.9 / CUDA 12.x runtime**. Adding them is **not** a new co-version — unlike Phase 2's
Optical Flow SDK. They compile behind the existing `COS_HAS_MAXINE` flag; without the SDK they build
as passthrough stubs, exactly like green screen.

## 4. Worker-thread effect chain

`capture.cpp WorkerLoop`, per frame (each stage gated on its atomic enable flag, lazy Start/Stop
identical to the current green-screen block):

```
decode (RGB32 BGRA)
  → [Artifact Reduction]   in-place, source res
  → Eye Contact            (existing)
  → Green Screen           (existing)
  → [Super Resolution]     → LARGER output buffer, new w/h
  → publish frame (w/h = SR output size)
```

**Order rationale:**
- Artifact Reduction runs **first** so the AI effects downstream consume a clean frame (better
  matte and landmarks).
- Super Resolution runs **last** so the expensive upscale happens once on the final composite, and
  the green-screen matte + gaze landmarks compute at native 1080p (cheaper, and the models are
  tuned for that resolution).

Maxine effects remain worker-thread-local (CUDA/NvVFX thread affinity); the UI only flips atomic
enable flags; status crosses threads via atomics + leaf-lock (never nested under `g_state.mtx` /
`g_lifecycleMtx`) — unchanged invariants.

## 5. New shim effect modules

Two files each, lifecycle shape copied from `aigs.{h,cpp}`
(`Start()/Stop()/IsReady()/ProcessFrame()/LastError()`, worker-thread-local):

- `native/shim/artifactreduction.{h,cpp}` — `NVVFX_FX_ARTIFACT_REDUCTION`, mode 1
  (strong / for-compressed). Operates in place on the BGRA scratch buffer at source resolution.
  **No dimension change.**
- `native/shim/superres.{h,cpp}` — `NVVFX_FX_SUPER_RES`, mode 1, upscale factor from params
  (1.5× or 2×). Allocates/owns a **larger** output buffer; `ProcessFrame` returns the new width and
  height to the worker, which publishes them.

Both behind `COS_HAS_MAXINE` → CI-safe passthrough stub when the SDK is absent. The exact
feature-DLL names and Maxine property keys (`NVVFX_STRENGTH`, `NVVFX_MODE`, the upscale-factor key)
are verified against the VFX 1.2.0.0 headers at implementation time.

## 6. Dimension-change plumbing (the only structurally new part)

Super Res makes published frames larger than 1080p, which ripples through the consumer:

- **`MainWindow.xaml.cs:25` `_frameBuf`** is a fixed `1920*1080*4` array — would overflow.
  **Pre-size it to 4K** (`3840*2160*4` ≈ 33 MB). One constant change; no dynamic realloc logic.
  *(ponytail: pre-size beats a resize-on-demand path; 33 MB is negligible.)* This is the hard cap
  on SR output and is the reason SR is capped at 2× of 1080p.
- **Swap chain** — `OverlayWindow.PresentFrame` **already** re-pins the back buffer to the incoming
  frame size (`if (_bufW != width || _bufH != height)`). SR's larger frame re-pins once on first
  arrival. No change needed.
- **Frame C-ABI** already returns dynamic `w`/`h` from `get_frame`. No contract change for size.

## 7. Super Res factor cap

Capped at **2×** (1080p → 2160p). Rationale: 2× of 1080p hits exactly 4K, matching the pre-sized
`_frameBuf`; 3×/4× would need a bigger buffer and far more GPU for diminishing perceptual gain at
overlay sizes. Exposed as off / 1.5× / 2×. Higher factors are a later change if ever needed.

## 8. C-ABI parity (load-bearing — x64 byte-for-byte)

Both `shim.h` (C) and `PInvokeShim` (`[StructLayout(Sequential)]` mirror) change together:

- **`CosParams`** — add `int artifactReductionEnabled`, `int superResEnabled`, `int superResScale`
  (`0` = off, else factor ×10: `15` = 1.5×, `20` = 2×).
- **`CosCaps`** — add `int artifactReductionAvailable`, `int superResAvailable` gates. Update the
  documented struct size (currently two `int` gates + `char detail[512]` = 520 bytes; each new gate
  adds 4 bytes — keep the comment and the C#-side size in sync).
- **`cos_query_capabilities`** — extend the probe to create + load each new effect, exactly as it
  already does for green screen and gaze, and set the new gates from the result.

## 9. Real fps counter

Replace the hardcoded `30.0` in `cos_get_status`:
- The capture worker increments an atomic frame count and stamps `std::chrono::steady_clock` at
  publish.
- `cos_get_status` computes fps over a rolling window (or simple EWMA) under the existing status
  leaf-lock — never nested under `g_state.mtx`.
- The existing C# `Fps` property (already polled every pump tick → `PollStatus` → `GetStatus`) now
  shows the real value. No new plumbing on the managed side.

## 10. C# / MVVM (Core + App)

- **`MainViewModel`** — new observable props `ArtifactReductionEnabled`, `SuperResEnabled`,
  `SuperResScale`. Their `On…Changed` partials call `ApplyLiveParams()` →
  `Orchestrator.ApplyParams(BuildParams())` → `shim.SetParams`, gated on `IsRunning` +
  `EffectsAvailable`, identical to the green-screen props. `ApplyParams` already forces effects off
  when `EffectsAvailable` is false — the new flags inherit that.
- **`BuildParams()`** — copy the three new fields into `ShimParams`/`CosParams`.
- **`ToAppConfig` / `AppConfig`** — persist the three fields in `config.json` (anything not copied
  reverts to default — must add explicitly).
- **XAML** — two toggles + a Super Res factor combo, bound `Mode=OneWay` to the availability gates
  (greyed until the deferred probe lands). The status line shows the now-real fps.

## 11. Mip downscale (present path — ships first, standalone)

Replace the `CopyResource` blit in `OverlayWindow.PresentFrame` (`OverlayWindow.cs:328`) with a
textured fullscreen-quad draw:
- frame texture created with `D3D11_RESOURCE_MISC_GENERATE_MIPS`,
- `GenerateMips()` after upload,
- draw a fullscreen quad into the back buffer with a **trilinear** sampler.

Mip/trilinear minification averages source texels properly → sharp at any shrink ratio, fixing the
bilinear-undersampling softness. Independent of the Maxine work; lands first and is independently
valuable. (Upscale magnification blur at large sizes is what Super Res addresses — the two are
complementary.)

## 12. Bundler / installer

- `scripts/assemble-maxine-stage.ps1` + `native/shim/bundle/maxine-manifest.psd1` — add the
  SuperRes + ArtifactReduction feature DLLs and their model files to the curated stage. Same VFX
  runtime, so **no new TRT/CUDA DLLs** — only the two feature DLLs + models.
- Re-run `native/shim/smoke/trace_closure.cpp` against the produced bundle to capture the new
  load closure; update the manifest `Dlls` list from its output.
- `verify-bundle.ps1` runtime probe (`COS_*` unset) must still load all effects.

## 13. Performance stance

Independent toggles, real fps readout, **no auto-management** (explicit user decision). Stacking
four AI inferences (Artifact Reduction + Eye Contact + Green Screen + Super Res) will not sustain
high fps on an RTX 3090; the user watches the fps readout and disables to taste. Document this in
the UI note + CLAUDE.md. No scheduler, no mutual exclusion (YAGNI).

## 14. Testing

- **Core unit tests** — `BuildParams` / `ToAppConfig` round-trip the three new fields;
  `ApplyParams` forces them off when `EffectsAvailable == false` (extend existing green-screen tests).
- **Native smoke** — extend `cos_query_capabilities` probe coverage; an AR/SR `Start` +
  `ProcessFrame` smoke on a synthetic frame returns success and (for SR) a larger output size
  (mirrors `aigs_smoke.cpp`).
- **fps counter self-check** — feed N frames over a known elapsed time, assert reported fps ≈ N/elapsed.
- **Human gate** — visual sharpness confirmation via a real recorder (Windows.Graphics.Capture /
  OBS), the inherent verification gate (`docs/superpowers/verification/`).

## 15. Incremental landing order

Each step is independently shippable and independently valuable:

1. **Mip downscale** (present path) — standalone sharpness win, no Maxine dependency.
2. **Real fps counter** — needed to tune everything after.
3. **Artifact Reduction** — clean source, no dimension change (low risk).
4. **Super Resolution** + 4K `_frameBuf` pre-size — the dimension-change step, landed last.

## 16. Open questions / verify-at-impl

- Exact VFX 1.2.0.0 feature-DLL filenames + property keys for SuperRes / ArtifactReduction.
- Whether Super Res mode 1 (compressed) or mode 0 looks better on this webcam's MJPG output —
  decide visually during the human gate.
- Confirm trilinear-quad present path keeps the `WS_EX_NOREDIRECTIONBITMAP` clean-capture behavior
  (it should — still a DComp flip-model swap chain, only the blit changes).
