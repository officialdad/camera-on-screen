# Camera-on-Screen — Design Spec: Finger-Pointing Overlay Control

> **⚠️ AMENDED 2026-07-09 (implemented on `feat/finger-control`).** Three §4 details below are
> **superseded** by what implementation + the final whole-branch review established:
> 1. **HandInference does NOT pull frames from the shim.** `cos_get_frame` is consume-on-read
>    (each frame is delivered to exactly ONE caller), so a second consumer steals frames from the
>    present pump → overlay judder. The UI frame pump is the **sole** shim consumer and republishes
>    each frame into `HandInference.PublishFrame` (lock-guarded latest-frame slot + version
>    counter; the inference loop copies out then releases before running ONNX). The "§4
>    `TryGetFrame` is thread-safe by design" line conflated thread-safety with multi-consumer
>    correctness — do not add a second `TryGetFrame` caller, ever.
> 2. **Clamp is the current monitor's work area (≥48 px of overlay kept visible), not the virtual
>    screen** — deliberate v1 simplification (`ponytail:` comment in `OnFingerNudge`); finger
>    control does not cross monitors.
> 3. **Palm detection needed no anchor decode** — the vendored export is post-processed
>    (`[N,8]` boxes baked into the graph); `PalmDecoder` is a plain argmax. Landmark presence
>    score is used raw (source thresholds it directly, no sigmoid). Ground truth:
>    `src/CameraOnScreen.App/Assets/models/hand/README.md`.
> §7's "disable the feature, set the note" is implemented via a `Failed` event →
> `FingerControlAvailable=false` + detail note; `Tracker.Reset()` on Stop guarantees clean
> re-arm across camera stop/start.

**Date:** 2026-07-08
**Status:** Approved (brainstorm 2026-07-08); defaults chosen by maintainer ("proceed with sensible defaults"); amended 2026-07-09 — see banner
**Depends on:** existing frame pump + `cos_get_frame` (thread-safe), overlay `SetBounds` path,
MVVM live-param pattern. **No shim changes. No Maxine/NVIDIA dependency.**
**Parent spec:** `docs/superpowers/specs/2026-06-20-camera-on-screen-design.md`.

## 1. Goal

Hands-free overlay repositioning for showcasing/teaching: raise a pointing finger (☝) at the
webcam, **nudge** it in a direction, and the overlay moves with the nudge. Hold the finger still —
overlay stops (still pointing ≠ still moving). Relax the hand — control disarms. No keyboard, no
mouse, works mid-presentation.

## 2. Interaction model (decided in brainstorm)

- **Fingertip-velocity drive ("air-trackpad"), not absolute mapping.** Per tracked frame the
  index-fingertip delta (normalized camera coords) scales to a screen-pixel delta applied to the
  overlay position. Finger still → delta ≈ 0 → overlay holds. No camera-to-screen calibration
  problem, no runaway glide.
- **Pointing-pose arming.** Only ☝ (index extended, middle/ring/pinky curled; thumb ignored)
  arms the control. Open palm, fist, talking gestures are ignored. Hysteresis: pose seen
  **3 consecutive inference frames → armed**; pose lost **5 consecutive frames → disarmed**
  (numbers are tunable constants in Core).
- **Manual drag wins.** While the mouse-hook drag is active, finger deltas are ignored.

## 3. Technology choice (verified 2026-07-08)

**MediaPipe Hands via ONNX Runtime (CPU), all in C#.** Verified before design (memory rule):

- **NVIDIA has no fitting feature.** Maxine AR SDK full catalog = face detect / landmarks /
  3D body pose / eye contact / expressions / lipsync / active speaker — **no hand features**;
  body pose's 34 keypoints stop at the wrist, so it cannot distinguish ☝ from ✋
  (fails the arming model). TAO GestureNet is a legacy static-gesture *classifier* (no fingertip
  coords, needs its own hand detector, TensorRT deployment → re-enters the M4/M5 TRT co-version
  hazard). `trt_pose_hand` is unmaintained Jetson research code with per-arch TRT engines.
- **MediaPipe Hands** (BlazePalm palm detector + 21-landmark model) is the de-facto standard:
  real-time on CPU, Google-maintained, Apache-2.0, models are a few MB. 21 landmarks give the
  index fingertip **and** enough joints to classify the pointing pose.
- **Model source:** Qualcomm AI Hub ONNX exports (`qualcomm/MediaPipe-Hand-Detection` on
  Hugging Face) as primary; PINTO model zoo conversions as fallback. **Plan gate: download and
  verify actual tensor I/O shapes/dtypes BEFORE coding the pre/post-processing** (same lesson as
  "verify SDK features before building").

Consequences: zero shim changes, zero NVIDIA licensing/co-version exposure, and this is the first
smart feature that also works on **non-RTX** machines (gate = model files + ONNX Runtime init,
not the Maxine probe).

## 4. Architecture

Two new units, split along the existing Core/App boundary:

- **`CameraOnScreen.Core` — `FingerControl` (pure logic, fully unit-tested, no ORT/Win32 types):**
  - `HandPoseClassifier`: 21 normalized landmarks → `Pointing | Other | NoHand`. Ratio-based:
    index extended = tip(8) farther from wrist(0) than pip(6); curled for middle(12)/ring(16)/
    pinky(20) = tip closer to wrist than pip. Thumb ignored. Confidence inputs (palm score,
    landmark presence score) gate to `NoHand` below thresholds (defaults 0.7 / 0.6).
  - `FingerNudgeTracker`: state machine (Disarmed → Arming(3) → Armed → Losing(5) → Disarmed) +
    per-tick fingertip delta → EMA smoothing (α = 0.5) → deadzone (2 px/tick after gain) →
    `MoveDelta(dxPx, dyPx)` output. Gain default **1.5** (a full camera-width sweep ≈ 1.5×
    screen width), user slider 0.5–3.0. Screen-space sign: `dxScreen = −dxCam` (camera frames are
    unmirrored; the display Mirror toggle is a DComp transform and does NOT affect the buffer —
    the sign is constant; verify empirically on first run, flip the constant if wrong).
- **`CameraOnScreen.App` — `HandInference`:**
  - Own background thread + own frame buffer; pulls frames via `ShimRef.TryGetFrame` (the shim's
    `LatestFrame` is mutex-guarded — thread-safe by design). Loop targets **~15 Hz** (66 ms
    cadence, skip-if-busy); independent of the 30/60 Hz present pump.
  - Preprocess in C# (no new imaging dep): BGRA → letterboxed RGB float tensor at each model's
    input size; two-stage MediaPipe plumbing (palm ROI → landmark model → map landmarks back to
    frame space).
  - `Microsoft.ML.OnnxRuntime` NuGet, **CPU EP only** (models are mobile-class; DirectML is a
    deferred optimization, not needed at 15 Hz).
  - Feeds classifier + tracker; posts `MoveDelta` to the UI thread via `DispatcherQueue`, which
    applies it through the existing `SetBounds` path (same as hook-drag), clamped to the virtual
    screen.

## 5. UI + persistence

- Control panel: **"Finger control"** toggle + **sensitivity** slider (0.5–3.0, default 1.5) in
  the effects card group. Live-gated on `IsRunning` like other params.
- Availability: `FingerControlAvailable` = model files present beside exe **and** ORT session
  init succeeded. Unavailable → toggle greyed + note (mirrors `EffectsAvailable` pattern, but
  independent of the Maxine probe — non-RTX machines get this feature).
- Persistence: `FingerControlEnabled` + `FingerControlSensitivity` in `AppConfig`
  (remember `ToAppConfig` copies-or-defaults rule).
- **Armed indicator:** tint/accent the existing centre "+" handle while armed, so the presenter
  knows the overlay is listening. If touching the handle rendering path risks the DComp/MPO
  invariants, cut the indicator from v1 (log-only) and file a follow-up.

## 6. Models + licensing + packaging

- Two ONNX files (palm detection + hand landmark, ~5–10 MB total, Apache-2.0) **committed to the
  repo** under `src/CameraOnScreen.App/Assets/models/hand/` with their license file;
  `CopyToOutputDirectory` via csproj. Committed (not fetch-scripted) because they are small,
  freely redistributable, and it keeps CI/installer offline-safe.
- `THIRD-PARTY-NOTICES.md` gains a MediaPipe/Apache-2.0 section.
- Installer needs no change beyond picking up the new build-output files (verify in plan).

## 7. Error handling

- Models missing / ORT init failure → `FingerControlAvailable = false`, note text, app runs
  normally (matches Maxine graceful-degradation philosophy).
- Any exception on the inference thread → disable the feature, set the note, log; never crash
  the app or stall the pump.
- Camera stop → inference loop idles (no frames); re-arms on next start automatically.

## 8. Testing

- **Core (xUnit):** classifier truth table from synthetic landmark sets (☝ / ✋ / ✊ / low
  confidence / missing hand); arming hysteresis counts; EMA + deadzone + gain math;
  sign-mapping unit (camera→screen); manual-drag-suppression flag.
- **App:** optional integration smoke that runs one inference on a synthetic frame, skipped when
  model files are absent (keeps `dotnet test` green everywhere).
- **Human gate (inherent):** pointing at the real webcam and watching the overlay follow —
  visual verification per `docs/superpowers/verification/`.

## 9. Out of scope (deferred)

- Grab/pinch gestures, two-hand gestures, resize-by-gesture.
- Absolute finger-as-cursor mapping.
- DirectML / GPU inference; ROI-tracking optimization (skip palm det when landmarks valid).
- Multi-monitor pointing semantics beyond virtual-screen clamping.
