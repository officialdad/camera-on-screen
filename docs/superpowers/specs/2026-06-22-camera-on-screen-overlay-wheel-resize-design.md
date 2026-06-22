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
