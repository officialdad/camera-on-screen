# Camera-on-Screen — Design Spec: ONNX Green-Screen Fallback

**Date:** 2026-07-16
**Status:** Approved (brainstorm 2026-07-16)
**Depends on:** existing frame pump + `cos_get_frame` single-consumer rule, `HandInference`
pump-tap pattern, MVVM live-param pattern, capability probe (`ProbeCapabilitiesAsync`).
**No shim changes. No new NuGet packages. No Maxine/NVIDIA dependency for this path.**
**Parent spec:** `docs/superpowers/specs/2026-06-20-camera-on-screen-design.md`.

## 1. Goal

Green screen for **non-NVIDIA / non-RTX Windows users**: when the Maxine probe fails, an
open-source person-segmentation model (MediaPipe Selfie Segmentation via ONNX Runtime, CPU)
supplies the matte instead, behind the **same** green-screen toggle and expand/feather sliders.
Converts the app from "effects need RTX" to "green screen on any GPU, best on RTX" — the
biggest addressable-audience win short of a Linux rewrite.

## 2. Decisions (brainstorm 2026-07-16)

1. **Silent fallback.** One "Green screen" toggle as today. Probe order: Maxine first, ONNX
   second, unavailable last. The capability note names the winning backend
   (e.g. *"Green screen: basic (CPU) — Maxine unavailable"*). No user-facing backend picker,
   nothing new persisted.
2. **Dev override:** `COS_GS_BACKEND=onnx` forces the ONNX path even when Maxine is available
   (maintainer has RTX hardware; natural fallback is also reachable by unsetting
   `COS_VFX_RUNTIME_DIR` on a dev build). Matches the repo's `COS_*` env-var culture.
3. **CPU-first.** Existing `Microsoft.ML.OnnxRuntime` (CPU) package. DirectML
   (`Microsoft.ML.OnnxRuntime.DirectML`, a superset — hand-grab unaffected) is the pre-agreed
   escape hatch **only if** a real machine misses the performance budget (§5).
4. **C# App side** (over a C++ shim backend). Follows the `HandInference` precedent: zero
   shim/ABI/vcxproj/CI/bundler changes, matte math unit-testable in Core. Accepted trade-off:
   matte lags the frame by ≤1 frame — invisible for a seated presenter.

## 3. Backend selection

One decision at probe time, in `ProbeCapabilitiesAsync` after the existing shim caps call:

| Backend | Condition |
|---|---|
| Maxine | `caps.green_screen_available == 1` **and** `COS_GS_BACKEND` ≠ `onnx` |
| ONNX | otherwise, if `OnnxSegmenter.TryCreate()` succeeds (model file present + session loads) |
| Unavailable | otherwise — toggle greyed + detail, exactly as today |

When ONNX wins, `BuildParams` sends `green_screen_enabled = 0` to the shim **always** (Maxine
GS never starts); the toggle and expand/feather sliders drive the C# path instead, via the same
`On…Changed` live-param partials. Backend is fixed for the app session (re-probe = restart, same
as Maxine today).

## 4. Architecture

Two new units plus one hook, split along the existing Core/App boundary:

- **`CameraOnScreen.Core` — `GreenScreen/MatteOps` (pure, fully unit-tested, no ORT/Win32
  types):** dilate (expand) and box-blur (feather) on the **low-res** matte (model resolution,
  cheap), then bilinear-upscale-and-apply into the BGRA frame: **A = matte, RGB premultiplied**
  — byte-identical semantics to the Aigs contract (`aigs.h`), so the overlay's premultiplied
  swap chain needs no changes. Slider semantics preserved: matte ops order = dilate → feather →
  premultiply.
- **`CameraOnScreen.App` — `Effects/OnnxSegmenter`:** structural mirror of `HandInference`:
  static `TryCreate(out detail)` probe; `Start`/`Stop`; `PublishFrame` tap (the UI pump remains
  the **sole** `cos_get_frame` consumer — consume-on-read, never add a second caller);
  own inference thread running per newest published frame (camera rate, ~30 Hz); output =
  latest low-res matte in a lock-guarded versioned slot, tagged with the source frame dims;
  `Failed` event on thread death (never silent).
- **Pump hook** (frame pump in `MainWindow`): after `cos_get_frame` → publish the **raw** frame
  to BOTH taps (segmenter + existing `HandInference`) **before** matte apply — hand detection
  must never see premultiplied-black backgrounds; if GS-on **and** a matte exists **and** its source dims match
  the current frame → `MatteOps.Apply` before upload. No matte yet, or dims mismatch (camera
  switch) → present opaque (alpha=255 passthrough, today's behavior). Frames are never
  half-processed.

## 5. Data flow & performance budget

```
camera → shim worker (eye contact → [Maxine GS off] → FRUC) → cos_get_frame
  → pump: publish frame ──→ OnnxSegmenter thread (model-res matte, versioned slot)
  → pump: MatteOps.Apply(latest matte) ←┘
  → upload → DComp present (premultiplied; mirror/zoom via visual transform, matte follows free)
```

- Effect order preserved: eye contact (shim) before green screen (C#), matching the Maxine chain.
- FRUC interplay: pump presents at ~60 Hz, matte computes at camera rate; the latest matte is
  applied to every presented frame (rare combo anyway — FRUC needs NVIDIA hardware).
- **Budget:** inference ~1–3 ms (mobile-class model on desktop CPU); `MatteOps.Apply` **< 5 ms
  at 1080p** (Span-based; vectorize only if needed) to fit the 16 ms FRUC-active pump tick.
  Measured during implementation on real hardware; miss → DirectML escape hatch (§2.3).

## 6. Model (plan gate: verify contract BEFORE coding)

**MediaPipe Selfie Segmentation** — Google-maintained, Apache-2.0, mobile-class (~100K params).
Candidate exports: PINTO0309 model zoo (same source as the hand models) — general 256×256
square, or landscape 144×256 (better fit for 16:9 webcams). **Gate, same lesson as hand-grab:
download the actual export and record input/output tensor names, shapes, layout (NCHW/NHWC),
value range, mask output semantics (sigmoid already applied?), and stretch-vs-letterbox
preprocessing in `Assets/models/segmentation/README.md` BEFORE writing pre/post-processing
code.** The README is the recorded contract; the spec deliberately does not guess it. Variant
choice (general vs landscape) is decided at that gate.

## 7. Error handling & status

Same contract as hand-grab (finger-control spec §7): `TryCreate` failure → backend unavailable
+ reason in the capability note. Inference-thread death → `Failed` event → toggle forced off +
note shows reason. Mid-run per-frame inference errors kill the loop (surface, don't limp).
`green_screen_active` for the ONNX backend is VM/pump-side truth; shim status stays
authoritative for Maxine only. `Dispose` order: segmenter stops before the shim (mirrors
`HandInference` handling in `MainViewModel.Dispose`).

## 8. Packaging & licensing

Ships like the hand models: `Assets/models/segmentation/` (`.onnx` + `LICENSE.txt` + contract
README) → csproj `<None>` copy to `<out>\models\segmentation\`. Apache-2.0 → new
THIRD-PARTY-NOTICES entry. Installer/bundler/CI: **zero changes** (installer stages the App
build output, which now contains the model; Maxine bundling untouched).
**Rejected on license:** Robust Video Matting (GPL-3.0 — would contaminate the MIT bundle).

## 9. Testing & verification

- **Unit (CI):** `MatteOps` — premultiply math, bilinear upscale coordinate mapping,
  dilate/feather bounds and ordering, dims-mismatch skip. Backend-selection rule (pure function
  of caps + env + probe result) unit-tested in Core.
- **Manual (human gate, per `docs/superpowers/verification/`), on the RTX box:**
  1. `COS_GS_BACKEND=onnx` — forced fallback with Maxine present; visual matte quality.
  2. `COS_VFX_RUNTIME_DIR` unset — natural fallback path end-to-end.
  3. Maxine path regression (no override, runtime present — behavior unchanged).
  4. OBS/recorder capture of the ONNX-matted overlay (transparency correct).
  5. FRUC + ONNX GS together (60 Hz apply budget holds).
  6. Expand/feather sliders live-drive the ONNX matte.

## 10. Out of scope (deliberate)

- **Eye contact / FRUC open-source replacements** — no production-grade OSS gaze redirection;
  real-time OSS interpolation unrealistic on CPU/DirectML. RTX-only they stay.
- **DirectML execution provider** — escape hatch only, not v1.
- **User-facing backend picker / persistence** — env override covers the real (dev) need.
- **Linux** — separate app regardless of this feature (see brainstorm discussion 2026-07-16).
