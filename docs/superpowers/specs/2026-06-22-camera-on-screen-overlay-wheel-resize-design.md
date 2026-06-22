# Camera-on-Screen — Overlay wheel-resize design

Date: 2026-06-22
Status: approved (brainstorm), pending implementation
Scope: small QoL feature. Add **mouse-wheel resize** of the on-screen overlay: hover the
overlay and scroll to grow/shrink its footprint live. Presentation/windowing-side only — no
shim, no C ABI, no MVVM, no config-schema change. One independent unit of work with its own
implementation plan + review checkpoint.

## Goal & core decision

Today the overlay can only be resized by dragging the 16px bottom-right grip
(`OverlayWindow.OnHitTest` → `HTBOTTOMRIGHT`). In practice that grip is undiscoverable/hard
to hit, so the overlay feels non-resizable — only the WinUI control panel (a normal
titled/bordered window) resizes. The user wants the **intuitive, live** gesture: put the
cursor over the overlay and scroll the wheel to resize it, the way you'd resize a
picture-in-picture.

The core decision driving the design: the overlay is a no-focus topmost layered window, so
the wheel can't reach it the normal way — we accept a **low-level global mouse hook** as the
delivery mechanism, and that hook's cursor-in-rect test doubles as the "only when hovered"
gate the user requires.

Non-goals (explicitly out of scope): a panel **Size slider** (decided against — wheel-only),
binding the wheel to the existing **content Zoom** (1.0–3.0, unchanged), zoom-to-cursor
anchoring, overlay shape/border, and any change to capture, effects, or the shim.

## Decisions (from brainstorm)

- **Wheel changes the window footprint**, not content zoom. The on-screen camera bubble gets
  bigger/smaller; aspect ratio is locked.
- **Center-anchored**: the window center stays put; edges expand/contract outward.
- **Active only when interactive** — disabled when the overlay is **Locked** or
  **Click-through** (Lock means "freeze everything for clean capture", consistent with
  drag-resize today; Click-through means the overlay takes no pointer input by design).
- **Wheel-only** — no panel slider added.
- **Step** ≈ 8% per wheel notch (one `WHEEL_DELTA` = 120). Up = grow, down = shrink.
- When the overlay handles a wheel tick, the event is **swallowed** so the application
  underneath does not also scroll.

## Why a low-level mouse hook

The overlay is created `WS_POPUP | WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP`
and shown with `SW_SHOWNOACTIVATE` — it never takes focus. Windows routes `WM_MOUSEWHEEL` to
the **focused** window, so the overlay's own wndproc cannot rely on receiving it. (The Win10
"scroll inactive windows on hover" setting would route to the window under the cursor, but
it is a user-toggleable setting — too fragile to depend on.)

So the app installs a **`WH_MOUSE_LL`** hook on the **UI thread** (which owns a message loop,
required for low-level hooks). On each `WM_MOUSEWHEEL` the callback reads the screen cursor
point and wheel delta from `MSLLHOOKSTRUCT`, tests whether the cursor is inside the overlay's
window rect, and — if so and the overlay is interactive — performs the resize and returns
non-zero to **consume** the event. The hook callback runs on the UI thread, so it may touch
the overlay window and Win32 geometry directly with no marshalling. Cost: one global hook
doing a cheap rect test per mouse event — acceptable; the app already runs a global hotkey
path.

## Units (isolation)

1. **`OverlaySizer`** — Core, **pure, unit-tested**. Owns all geometry math; no Win32 types.
   - `readonly record struct Rect(int X, int Y, int W, int H)`.
   - `static Rect Resize(Rect current, int notches, Rect workArea)`:
     - factor = `(1 + Step)^notches` (notches may be negative); `Step = 0.08`.
     - scale both `W` and `H` by the same factor → **aspect preserved** (within rounding).
     - **min**: height floor `MinHeight = 120` px (scale up to the floor, keeping aspect, if
       a shrink would go under it).
     - **max**: clamp to `workArea.W`/`workArea.H` (scale down to fit, keeping aspect).
     - **re-center**: keep the current center (`cx = X + W/2`, `cy = Y + H/2`); new
       `X = cx - newW/2`, `Y = cy - newH/2`. Position is left as-is otherwise (off-screen
       clamping is not required — drag-move already lets the window sit partly off-screen).
   - No-op when `notches == 0` or the result equals the input.

2. **`MouseWheelHook`** — App, pure Win32 plumbing. Installs/uninstalls `WH_MOUSE_LL`
   (`SetWindowsHookEx`/`UnhookWindowsHookEx`/`CallNextHookEx`). Keeps the callback delegate
   alive (GC) like `OverlayWindow._proc`. Exposes a single injected callback
   `Func<POINT, int, bool>` invoked per `WM_MOUSEWHEEL` as `(screenPoint, notches) →
   handled`; returns non-zero from the hook to swallow when `handled` is true, else
   `CallNextHookEx`. `IDisposable` → unhooks.

3. **`OverlayWindow`** additions:
   - `SetBounds(int x, int y, int w, int h)` — `SetWindowPos(..., SWP_NOACTIVATE |
     SWP_NOZORDER)` (preserves topmost; never steals focus). The resulting `WM_SIZE` already
     routes to `UpdateScale`, which re-stretches the frame-res content to the new window size
     — so no extra redraw code is needed.
   - `bool IsInteractive => !_locked && !_clickThrough`. Add a `_clickThrough` field set in
     `SetClickThrough` (today only the window style is toggled; track the bool too).
   - `GetBounds()` already exists (returns current screen rect).

4. **`MainWindow`** wiring:
   - Construct/own a `MouseWheelHook`; dispose it in `OnWindowClosed`.
   - Hook callback: if `_overlay.IsInteractive` and the cursor point is inside
     `_overlay.GetBounds()` → compute `workArea` from the overlay's monitor
     (`MonitorFromWindow` + `GetMonitorInfo`) → `OverlaySizer.Resize(...)` →
     `_overlay.SetBounds(...)` → restart a debounce timer → return `true` (consume). Else
     return `false`.
   - **Debounced persist**: a `DispatcherQueueTimer` (~400 ms), restarted on each resize,
     fires the existing `Save()` once after the wheel settles. (`Save()` already reads live
     geometry via `_overlay.GetBounds()` and writes `Overlay.Width/Height` — no new save
     path.) Stop/null this timer in `OnWindowClosed` alongside the frame-pump timer.

## Data flow

`WH_MOUSE_LL` → `MouseWheelHook` callback → `MainWindow` (interactive gate + cursor-in-rect
test + monitor work area) → `OverlaySizer.Resize` → `OverlayWindow.SetBounds` → `WM_SIZE` →
`UpdateScale` (DComp visual re-stretches content to fill) → debounce → `Save()` →
`config.Overlay.Width/Height`.

## No changes to

- The shim, the C ABI, or any `Cos*` struct / `PInvokeShim`.
- `AppConfig` / `OverlaySettings` schema — `Width`/`Height` already persist and restore
  (`ResolveStartupBounds`).
- `MainViewModel` observable props / `BuildParams` — wheel-resize is windowing state, not
  shim params and not an MVVM-bound value.
- The existing **Zoom** (content crop, 1.0–3.0) and **Mirror** transforms in `UpdateScale`.

## Testing

- **Core unit tests** for `OverlaySizer.Resize`: aspect ratio preserved across grow/shrink;
  `MinHeight` floor honored; `workArea` max clamp honored; center stays fixed; positive vs
  negative notches; zero-notch no-op. Same xUnit pattern as existing Core tests.
- **Hook + actual on-screen resize = inherent human/visual gate** — the overlay is not
  GDI-screenshottable by design (`WS_EX_NOREDIRECTIONBITMAP` + DComp flip-model), so wheel
  delivery, the hover gate, swallow behavior, and the Lock/Click-through gating are confirmed
  by hand on the RTX 3090 (per `docs/superpowers/verification/`).

## Cross-cutting

- **Pristine build:** 0 warnings across App and Core (warnings are findings; CI enforces
  `/warnaserror`).
- **Hook lifetime/safety:** keep the hook delegate rooted for the hook's lifetime; unhook in
  `OnWindowClosed` (before `Vm.Dispose()`/overlay dispose) so no callback fires into a
  torn-down overlay.
- **No focus theft:** `SetBounds` uses `SWP_NOACTIVATE | SWP_NOZORDER`; the overlay stays
  topmost and unfocused, preserving the clean-capture design.

---

## Addendum (2026-06-22): drag-handle redesign + remove BR resize

**What the human gate found.** Wheel-resize works, but at larger sizes (the user hit it at
~973×720) **dragging the overlay fails over the video region** — clicks on the body/face
pass straight through to the app beneath, while clicks on uncovered window area still drag.
It reproduces in raw passthrough (fully opaque image), so it is **not** an alpha/green-screen
issue.

**Root cause.** The overlay is `WS_EX_LAYERED` and its DirectComposition flip-model swap
chain is **promoted to a hardware MPO (multiplane overlay) plane** at larger sizes on the RTX
GPU (the same property CLAUDE.md notes makes GDI screenshots return black). When the video is
on an MPO plane, DWM no longer has the per-pixel alpha it uses to hit-test the layered window
over that region, so the OS routes the click through. MPO promotion is size/occlusion
dependent → the "inconsistent, only at certain sizes" symptom. This is a **pre-existing**
latent issue (any large size would trigger it) that wheel-resize merely made easy to reach.
`WM_NCHITTEST`-based drag-anywhere is therefore fundamentally unreliable on the video.

**Decision — stop relying on window hit-testing for drag; use the global hook + a visible
handle.** The `WH_MOUSE_LL` hook already installed for the wheel sees mouse input **before**
the OS routes it to a window, so it catches presses on the video region even when the window
cannot. Two changes:

1. **Remove the bottom-right resize grip** (the `HTBOTTOMRIGHT` hit-zone, `DrawGrip`,
   `GripSize`) and **remove drag-anywhere** (`HTCAPTION`). The wheel is the only resize
   mechanism; `WM_NCHITTEST` reverts to default (`HTCLIENT`). This also removes the
   non-aspect-locked free resize the user disliked.
2. **Add a visible drag handle, dragged via the hook.** A **move-icon pill at the top-center**
   of the overlay, shown **on hover when unlocked** (hidden when Locked or Click-through, and
   in clean-capture). Dragging is driven entirely by the low-level hook: left-button-down
   inside the handle's screen rect (and `IsInteractive`) begins a drag; mouse-move calls
   `SetBounds` to follow (size unchanged); button-up ends it and persists (debounced `Save`).
   The hook **swallows** these events so the app beneath does not also receive them.

**Handle geometry & rendering (v1).** The handle is a fixed **screen-pixel** size (≈110×28)
centered on the overlay's top edge. Hit-testing uses the live window rect (fixed screen
position, independent of zoom). The pill is drawn into the back buffer via `ClearView` (the
same mechanism the old grip used), sized from the current client→buffer scale so it maps to
the intended screen size at the default transform. **Known v1 caveat:** because it is drawn
in the content back buffer, content **zoom > 1** shifts the drawn pill away from the fixed
hit rect (mirror is fine — the pill is symmetric). Zoom defaults to 1.0; if zoom+handle
alignment matters later, promote the handle to its own DComp visual. Tracked as a follow-up,
not a v1 blocker.

**Hook generalization.** `MouseWheelHook` becomes a general low-level mouse event source
surfacing button-down / mouse-move / button-up **and** wheel to the host, each returning a
"handled → swallow" bool. The drag state machine (offsets, in-progress flag) lives in
`MainWindow` (UI-thread, where the hook callback runs), not in the hook.

**Unchanged:** wheel-resize math (`OverlaySizer`), `SetBounds`/`IsInteractive`, persistence
path, Lock/Click-through semantics, no shim/C-ABI/MVVM/config-schema change. Lock and
Click-through both disable handle dragging via `IsInteractive`.

### As-built (revised during the human gate)

Two deviations from the above, found while validating on the RTX 3090:

- **Handle is a small "+" crosshair at the overlay CENTER**, not a top-center pill (user
  preference: smaller, no background box, centered). Hit area is a generous fixed 44×44 screen
  px (centered via the live window rect, zoom-independent); the crosshair glyph (~12 px, 0.45
  alpha) is drawn into the back buffer (the zoom>1 caveat above still applies).
- **Drag MOVE is polled on the 30 Hz frame-pump timer via `GetCursorPos`, not driven by the
  hook's `WM_MOUSEMOVE`.** Calling `SetBounds` inside the low-level mouse-move hook moves the
  window under the cursor, which makes the OS emit synthesized mouse-moves that re-enter the
  hook → a feedback loop that floods at ~800 Hz and pins the overlay (it jitters ±1 px instead
  of following the cursor; confirmed via instrumentation). The hook now only **starts** the
  drag (left-down on the handle, capturing the cursor→origin offset) and **ends** it
  (left-up); the timer does `SetBounds(GetCursorPos − offset, same size)` each tick. The hook
  still swallows the down/up so the app beneath never sees the click.
