# Camera-on-Screen — QoL Polish design

Date: 2026-06-21
Status: approved (brainstorm), pending implementation
Scope: small quality-of-life polish between M3 (done) and the next milestone (M4 Eye
Contact / M5 ship-time). Three independent buckets, each built and reviewed on its own
checkpoint.

## Goal & core decision

The app works (M1–M3 verified on RTX 3090), but day-to-day use has friction. This effort
is **polish, not a new milestone**: make the existing build pleasant to use before taking
on M4/M5. The core decision it drives: *is the current build good enough to use as the
daily driver, or does using it surface deeper design gaps?* Three concrete irritations,
ordered by user priority.

Build order: **2 → 3 → 1**. Each bucket is independent (no shared state, no sequential
dependency) and gets its own implementation plan + review checkpoint.

Non-goals (deferred to "future", explicitly out of scope here): overlay shape masks
(round/rounded-rect), border, drop shadow, zoom **pan** (center-only zoom only), and any
M4/M5 work.

---

## Bucket 2 — Overlay mirror + zoom (first priority)

**What:** a horizontal **mirror** (selfie view) toggle and a center-only **zoom** slider
(1.0×–3.0×) for the overlay, so the user can flip and tighten the framing of their face.

**Where it lives — presentation-side only, no shim change.** Both are pure
`Matrix3x2` math in `OverlayWindow.UpdateScale()`. The overlay already decouples window
size from the frame-res swap chain: the DirectComposition **visual transform** scales the
frame-res content to fill the window, and the layered window clips anything outside its
bounds. The swap chain stays pinned to the camera's native resolution so the
`CopyResource` in `PresentFrame` remains a valid 1:1 copy. We extend the existing transform
— we do **not** touch the swap chain, the shim, or the C ABI.

**Transform composition** (maps frame-res content → window): start from the existing fit
scale (`sx = clientW/bufW`, `sy = clientH/bufH`), then:
- **Zoom** — scale by `Z` about the content center. `Z > 1` makes content larger than the
  window; the window edges crop the overflow = tighter center framing. (`Z = 1` is current
  behavior.)
- **Mirror** — negate the X scale about the center (flip horizontally) when enabled.

Both compose into the single `Matrix3x2` already passed to `_visual.SetTransform(...)`,
followed by `_dcomp.Commit()`. The matrix algebra (center-anchored scale/flip) is an
implementation detail for the plan.

**State & flow:**
- `OverlaySettings.Mirror` (`bool`) **already exists in config** but is not yet wired to
  the overlay — bucket 2 wires it.
- Add `OverlaySettings.Zoom` (`double`, default `1.0`, clamped 1.0–3.0).
- New `OverlayWindow` fields `_mirror`/`_zoom` with `SetMirror(bool)`/`SetZoom(double)`
  setters that re-run `UpdateScale` + commit, so changes apply **live** while running.
- Panel control → `MainViewModel` props → host calls `OverlayWindow.SetMirror/SetZoom`.
  Mirror/zoom are presentation state, not shim params, so they do **not** flow through
  `BuildParams`/`SetParams`.
- Persisted with the rest of overlay geometry on `WM_EXITSIZEMOVE` / window close (no
  per-tick disk writes).

**Panel UI:** a Mirror toggle and a Zoom slider (1.0–3.0) in the control panel.

**Tests:** Core unit tests for the `OverlaySettings.Zoom` config round-trip and the
`MainViewModel` mirror/zoom props (same pattern as existing overlay-settings tests). The
visual result (flip correct, zoom crops to center) is a human visual gate — the overlay
cannot be GDI-screenshotted by design.

---

## Bucket 3 — Green-screen Expand + Feather

**What:** two sliders to fix the green-screen matte cutting into the user's body:
- **Expand** (0–1) — morphologically **dilates** the matte, growing coverage outward so
  the edge stops eating into the body. This is the primary fix for "cuts part of me".
- **Feather** (0–1) — **blurs** the matte transition for a soft edge (cosmetic).

**Where it lives — shim, matte post-processing in `aigs.cpp`.** Both operate on the
downloaded CPU matte inside `Composite()`, **before** the premultiplied alpha is applied.
Order: **dilate → feather → composite**. The ops honor the matte's existing `pitch` and run
entirely worker-thread-local (the `Aigs` object already lives on the capture worker thread
for CUDA affinity) — no threading change. Params are pushed live exactly like the existing
enable flag (UI thread flips values; worker reads them per frame).

**C ABI — one new field, full parity.** The reserved-but-unused `green_screen_strength` is
repurposed as **`green_screen_expand`**; a new **`green_screen_feather`** field is added.
Both are `double`. This touches every parity site and must stay byte-for-byte on x64:
- `native/shim/shim.h` — `CosParams` (rename + add field)
- `src/CameraOnScreen.App/Native/PInvokeShim.cs` — `[StructLayout(Sequential)]` mirror
- `src/CameraOnScreen.Core/Native/Contracts.cs` — `ShimParams` record
- `src/CameraOnScreen.Core/Config/Models.cs` — `EffectSettings` (rename
  `GreenScreenStrength` → `GreenScreenExpand`, add `GreenScreenFeather`)
- `MainViewModel` — props + `BuildParams` + live `On…Changed` partials
- App XAML — two sliders

**Matte ops (native):** dilate and feather are simple separable passes over the
single-channel (A8) matte buffer. Radius scales from the 0–1 slider value. These are a
deliberate seam in `Composite()`; the existing premultiply step is unchanged. Keep the
build **pristine (0 warnings)** and preserve the leaf-lock / atomic discipline already in
`aigs.cpp` (no new locks nested under `g_state.mtx`).

**Panel UI:** two sliders under the green-screen toggle, enabled only when green screen is
on (same enable-gating as the existing effect controls).

**Tests:** Core unit tests for the renamed/added `EffectSettings` fields and
`BuildParams`/`SetParams` round-trip (same pattern as the existing `GreenScreenStrength`
tests, which are updated to the new names). Verify the C ABI after the shim change with
`dumpbin /exports` and the `grep -a GreenScreen` deploy check. The matte visual result
(no body-cutting, soft edge) is a human visual gate on the RTX 3090.

**Build-deploy reminder:** rebuild the SDK config (`COS_HAS_MAXINE`) **last** before
running, then `-t:Rebuild` the App, or the passthrough stub deploys (greyed toggles). See
CLAUDE.md "Build & test".

---

## Bucket 1 — Control panel right-size

**What:** the control panel opens at the WinUI default (~1100×700) while it only shows a
few `Auto`-height rows of controls — mostly empty, disruptive. Right-size it.

**How:** set `AppWindow.Resize(...)` to a compact size that fits the controls in the
`MainWindow` constructor (sensible default, roughly the width of the existing 280px camera
combo plus margins, height to fit the rows). Optionally set a minimum size. **No redesign**
of the panel layout — sensible defaults only, per the user's intent ("just not be
disruptive").

**Tests:** none meaningful (window chrome sizing) — visual confirmation only.

---

## Cross-cutting

- **Pristine build:** 0 warnings across shim, App, and Core (warnings are findings here).
- **Persistence:** new overlay state (`Zoom`; `Mirror` wiring) saves through the existing
  `WM_EXITSIZEMOVE`/close path; `MainViewModel.ToAppConfig` must copy the new fields (it
  builds a fresh `AppConfig`, so anything not copied reverts to default).
- **ABI parity (bucket 3 only):** the `CosParams` struct change must be mirrored
  byte-for-byte; verify with `dumpbin /exports` and a deploy `grep` check.
- **Human visual gates:** overlay flip/zoom and matte edge quality are confirmed visually
  on the RTX 3090 — the overlay is not GDI-capturable by design.
