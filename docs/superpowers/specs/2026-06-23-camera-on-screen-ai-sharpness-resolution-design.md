# Camera-on-Screen — Design Spec: AI Sharpness & Resolution (Phase 1)

> **⚠️ AMENDED 2026-06-24 (issue #15) — re-scoped to VSR-only; implemented.** Two design
> assumptions below are **superseded** by what was verified against the live NGC catalog + the
> real VFX 1.2.0.0 headers (memory `vfx-vsr-ngc-grounded`):
> 1. **Artifact Reduction is DROPPED — it does not exist.** `NVVFX_FX_ARTIFACT_REDUCTION` is absent
>    from the VFX 1.2.0.0 catalog (only a help-text typo in `install_feature.ps1`); NGC returns
>    `PAYMENT_REQUIRED` for it. All `artifactreduction.{h,cpp}` / CosParams / CosCaps / UI / test
>    references were removed.
> 2. **Super Resolution = NGX VSR, not the old `SuperRes` effect.** Real selector
>    `NVVFX_FX_VIDEO_SUPER_RES "VideoSuperRes"`, param `NVVFX_QUALITY_LEVEL "QualityLevel"` (NOT
>    `NVVFX_MODE`/`NVVFX_STRENGTH`/a scale param). BGRA u8 GPU in+out. **QualityLevel is the mode
>    selector**: 1-4 upscale (Low/Med/High/Ultra, scaled by 1.5×/2×), 8-11 denoise, 12-15 deblur —
>    so VSR's clean modes cover the image-cleanup goal the dead Artifact Reduction was meant to
>    serve. VSR is NGX (model baked into `nvngx_vsr.dll`) → **no per-arch TRT engine**, runs on
>    every RTX arch. Bundle delta = 3 DLLs (`nvVFXVideoSuperRes.dll`, `nvngxruntime.dll`,
>    `nvngx_vsr.dll`), confirmed by `trace_closure` on the 3090 (22 modules).
>
> The **generic plumbing** below (worker-chain placement, dynamic output-resolution publish, 4K
> `_frameBuf`, factor cap, present-path ripple, single-effect MVVM/persistence pattern) still holds.
> Authoritative current scope + task state: `docs/superpowers/plans/2026-06-24-issue-15-vsr-resume.md`.
> Treat any `NVVFX_FX_SUPER_RES` / `NVVFX_MODE` / Artifact-Reduction wording below as historical.

**Date:** 2026-06-23
**Status:** AMENDED + implemented 2026-06-24 (issue #15, VSR-only) — see banner above
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

The overlay display size **varies** (small widget ↔ near-fullscreen). Phase 1 cleans compression
artifacts (helps all sizes) and adds AI-upscaled detail (helps large sizes, where there is little
or no downscale). The remaining **small-size softness** — bilinear minification in the present path
— is a separate present-path redesign and is **deferred** (see §2 and §11), not solved by the
Maxine effects here.

## 2. Scope

**In scope (Phase 1):**
- **Artifact Reduction** (Maxine VFX `NVVFX_FX_ARTIFACT_REDUCTION`) — removes compression
  artifacts, in-place at source resolution, no dimension change.
- **Super Resolution** (Maxine VFX `NVVFX_FX_SUPER_RES`) — AI upscale, off / 1.5× / 2×, writes a
  larger output buffer.
- **Real fps counter** — replace the hardcoded `cos_get_status` `30.0` stub with a measured value
  so the user can see each effect's cost.
- Independent on/off toggles per effect; capability gating; live-param push; JSON persistence.

**Out of scope:**
- **Present-path minification fix (deferred — see §11).** The small-size bilinear-minification
  softness needs a present-path redesign (window-sized back buffer + mipped shader scale), which
  collides with load-bearing invariants (pinned-to-frame-res back buffer, mirror/zoom in the DComp
  transform, the MPO hit-test gotcha). Separated from the Maxine effects to keep Phase 1 low-risk.
  To be filed as its own issue if pursued.
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
- **`CosCaps`** — add `int artifactReductionAvailable`, `int superResAvailable` gates. The struct
  grows from two `int` gates + `char detail[512]` = **520 bytes** to **four** `int` gates +
  `detail[512]` = **528 bytes**; update the size comment in `shim.h` and the `[StructLayout]` C#
  mirror together.
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

## 11. Present-path minification softness (DEFERRED — not built in Phase 1)

Recorded here so the analysis is not lost; this is **out of scope** for Phase 1 (§2).

**Root cause (confirmed in code):** the back buffer is pinned to the **frame** resolution
(`PresentFrame` — `CopyResource(back, _frameTex)` is 1:1, `_frameTex` is `MipLevels = 1`), and the
frame-res → window scaling is done by the **DComp visual transform** (`UpdateScale` → `SetTransform`),
which samples **bilinear** with no mip control. A 1080p → ~480px widget downscale therefore
undersamples → softness/aliasing.

**Why it is not a drop-in blit swap:** putting mips on `_frameTex` and drawing a quad into the
*current* frame-res back buffer changes nothing — DComp still does the final minification afterward
and ignores our mips. A correct fix requires moving the scale into our own D3D draw:
- back buffer becomes **window-sized** (drops the pinned-to-frame-res / `CopyResource` 1:1 invariant),
- draw a fullscreen quad sampling a **mipped** `_frameTex` (`GENERATE_MIPS` + `GenerateMips()`) with
  a **trilinear** sampler,
- DComp transform becomes **identity**, and **mirror + center-zoom** move out of the DComp
  `Matrix3x2` into the quad's transform,
- re-verify `WS_EX_NOREDIRECTIONBITMAP` clean capture and the size-dependent **MPO hit-test** gotcha
  still hold with a window-sized back buffer.

This touches load-bearing present-path invariants, so it is its own milestone. Note: Super Res does
**not** substitute for it — the upscaled detail is also thrown away by the DComp bilinear downscale
at small sizes.

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

1. **Real fps counter** — needed to tune everything after.
2. **Artifact Reduction** — clean source, no dimension change (low risk).
3. **Super Resolution** + 4K `_frameBuf` pre-size — the dimension-change step, landed last.

## 16. Open questions / verify-at-impl

- Exact VFX 1.2.0.0 feature-DLL filenames + property keys for SuperRes / ArtifactReduction.
- Whether Super Res mode 1 (compressed) or mode 0 looks better on this webcam's MJPG output —
  decide visually during the human gate.
