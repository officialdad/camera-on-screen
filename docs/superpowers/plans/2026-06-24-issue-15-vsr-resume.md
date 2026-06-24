# Issue #15 Resume — VSR-only AI sharpness (amends the parked Phase-1 plan)

> **For agentic workers:** Use superpowers:subagent-driven-development or superpowers:executing-plans.
> **When implementing any App/XAML task (T7), invoke the WinUI skills first:** `winui-design`
> (XAML correctness, Fluent layout, theming, accessibility review) and `winui-dev-workflow`
> (build/run). `winui-app` for broader WinUI guidance if needed.

**Issue:** #15 — Parked AI sharpness effects.
**Supersedes the effect tasks of:** `docs/superpowers/plans/2026-06-24-ai-sharpness-resolution-phase1.md`
(parked branch `feat/ai-sharpness-resolution`). **Spec:** `docs/superpowers/specs/2026-06-23-camera-on-screen-ai-sharpness-resolution-design.md` — needs a spec update to drop Artifact Reduction and re-scope Super Resolution to VSR-only (see T0).

## Decision (user, 2026-06-24)
**VSR-only + mode combo.** One effect (Video Super Resolution), expose its `QualityLevel` as a
mode selector covering **Upscale** *and* **Denoise/Deblur** — so it also covers the image-cleanup
goal the (dead) Artifact Reduction was meant to serve. **No second effect.** (`nvvfxdenoising` is
real + 4-arch on NGC and is the only viable stack-on-top add, but only needed if denoise must run
*in the same frame as* upscale — not wanted now.)

## Grounded facts (verified 2026-06-24, see memory `vfx-vsr-ngc-grounded`)
- **Real VFX 1.2.0.0 catalog** (live NGC, collection `maxine_vfx_sdk`, 8 models): aigsrelighting,
  backgroundblur, denoising, greenscreen, relighting, transfer, upscale, **videosuperres**.
- **Artifact Reduction is DEAD** — absent on disk, only a help-text typo in `install_feature.ps1`,
  and live NGC returns `PAYMENT_REQUIRED`/"key lacks permission" for `nvvfxartifactreduction`
  while all 8 real features return data.
- **Real VSR API** (`features/nvvfxvideosuperres/include/nvVFXVideoSuperRes.h`): selector
  `NVVFX_FX_VIDEO_SUPER_RES "VideoSuperRes"`, param `NVVFX_QUALITY_LEVEL "QualityLevel"`. Image
  in+out = **BGRA/RGBA u8 GPU** (matches our pipeline — no BGR convert). QualityLevel values:
  0 Bicubic, 1-4 VSR Low/Med/High/Ultra (upscale), 8-11 Denoise, 12-15 Deblur, 16-19 HighBitrate.
  **Denoise/Deblur do NOT upscale (out must == in).**
- **VSR ships NO per-arch engine** — model baked into `nvngx_vsr.dll` (NGX, 45MB) + dispatcher
  `nvVFXVideoSuperRes.dll`. **Runs on every RTX arch with zero per-arch deserialize risk** —
  sidesteps the non-Ampere verify gate that greys out green screen / gaze.
- **T1 PROBE PASSED on the 3090** — `CreateEffect("VideoSuperRes")` → `SetU32(QualityLevel)` →
  `Load` → `Run` all `0 No error` for quality=1 (720→1440 2x upscale) and quality=8 (denoise,
  out==in). Confirms: QualityLevel takes the values directly (single param, no separate Mode);
  scale inferred from out/in dims; `nvngx_vsr.dll` resolves from a flat dir via
  `g_nvVFXSDKPath`→SetDllDirectory. Probe sources in session scratchpad
  (`vsr_probe.cpp` + `build_run_vsr_probe.bat`).

## Reuse from parked branch (do NOT rebuild)
`vfx_paths.{h,cpp}` (green screen adopted it); C-ABI effect slots + capture-worker wiring
(lazy start/stop, leaf-lock errors, dynamic-resolution publish); managed Orchestrator gating,
MVVM scale combo, persistence, 4K frame buffer, App UI scaffold, 57 xUnit tests.

## Branch
After PR #14 (`feat/fps-and-capture-mode` = fps counter + highest-fps capture) merges, branch
`feat/ai-sharpness-vsr` off `main`; rebase the parked branch's effect commits on (the fps/capture
patches dedupe to empty since #14 carries them). The parked branch is the substrate.

## Tasks

- [x] **T1 — Probe VSR (DONE, 2026-06-24).** Verified the real API on the 3090 (above).

- [ ] **T0 — Spec update.** Edit the design spec: drop Artifact Reduction entirely; re-scope SR to
  VSR-only with a mode selector. Keep the generic-plumbing sections.

- [ ] **T2 — Rewrite `superres.{h,cpp}`** against the real API (simpler than parked):
  - selector `"VideoSuperRes"`, include the real header.
  - **BGRA u8 GPU in+out** — delete the BGR conversion and the per-pixel alpha-repack loop.
  - `Start(int qualityLevel, int scaleX10)`; set `NVVFX_QUALITY_LEVEL` (not `NVVFX_MODE`).
  - Upscale modes 1-4 → out = in×scale; Denoise 8-11 / Deblur 12-15 → **out = in** (enforce).
  - NGX path via `vfx_paths` (dir holding `nvngx_vsr.dll`).

- [ ] **T3 — Delete Artifact Reduction** everywhere: `artifactreduction.{h,cpp}` + vcxproj entry,
  CosParams flag, CosCaps gate, capture-worker call, MVVM toggle/gate/persistence, AR xUnit tests,
  AR UI, AR bundle/manifest entry.

- [ ] **T4 — C ABI.** SR enable + **mode (QualityLevel)** + scale. Update `CosParams`/`CosCaps`
  (byte-parity x64) + `[StructLayout]` mirrors in `PInvokeShim` + `FakeShim` + `Contracts`.
  Verify `dumpbin /exports` still lists exactly the **9** `cos_*` functions.

- [ ] **T5 — Capture worker.** SR is now the only effect (AR gone), runs last; reuse the existing
  dynamic output-resolution publish.

- [ ] **T6 — Managed + Core + tests.** Orchestrator gate from the VSR cap-probe; MVVM mode + quality
  + scale props with live push + persistence. Update the 57 xUnit tests (remove AR, add mode).

- [ ] **T7 — App UI (INVOKE `winui-design` + `winui-dev-workflow` FIRST).**
  1. **Add VSR controls:** mode combo (Off / Upscale / Denoise / Deblur), quality (Low/Med/High/
     Ultra → QualityLevel), scale combo (1.5x / 2x — upscale modes only, disable otherwise).
  2. **Restructure the window for overflow + a pinned footer** (`MainWindow.xaml`): wrap the
     controls in a `ScrollViewer` and pin the **"Powered by Maxine" attribution footer OUTSIDE**
     it (Grid: row 0 `*` = ScrollViewer of controls, row 1 `Auto` = footer).
     **Fixes a current bug:** the footer sits in a `*` spacer row pinned `Bottom`; once the stacked
     toggles fill the 400×720 window the spacer collapses and the footer drops below the fold (only
     visible after manually growing the window). Adding the VSR controls makes it worse. The
     attribution is license-required (SDK Supplement §3.1) so it must always be visible.
     Keep all existing `x:Bind` bindings. 4K frame buffer already present.

- [ ] **T8 — Bundler.** Stage `nvVFXVideoSuperRes.dll` + `nvngx_vsr.dll` into the flat `maxine\`;
  **no per-arch engines** for VSR. **Re-run `trace_closure`** — the NGX runtime may pull DLLs
  outside the current 19-DLL closure (bundle size unknown until traced). Re-check the VSR model
  license (`NVIDIA-Open-Model-License-Agreements-...pdf` in the feature dir) covers redistribution
  like green screen.

- [ ] **T9 — Verify (human gate, 3090).** Build the SDK shim config **last**, export-verify, run:
  upscale 1080→2160 visibly sharper; denoise mode cleans; toggles gate correctly off when probe
  unavailable.

## Open risks (both grounded)
1. **NGX DLL closure unknown** until `trace_closure` re-runs against the VSR bundle (T8).
2. **VSR model license** needs a one-line redistribution re-check — it's a different model than
   green screen (T8).
