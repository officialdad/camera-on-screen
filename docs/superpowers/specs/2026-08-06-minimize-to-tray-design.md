# Minimize to system tray (Linux control panel)

Date: 2026-08-06
Status: approved, ready for implementation plan

## Problem

Camera-on-Screen is a set-and-forget overlay: once the webcam overlay sits where you want it,
the control panel is dead weight on the taskbar. Closing the panel today quits the process and
tears down capture (`MainWindow.Closed` → `Vm.Dispose()` → `cos_shutdown`), so the only way to
get the panel out of the way is a normal taskbar minimize.

Wanted: send the panel to the system tray, keep the overlay and capture running, and make that
the default behavior.

## Scope

- **Linux / Avalonia control panel only** (`src/CameraOnScreen.App.Avalonia`). The WinUI panel
  stays unwired — Windows work is parked on #38, same as the #57 camera-selection features.
- The overlay window (`Overlay/OverlayWindow.cs`) is untouched. Hiding the panel must not
  disturb capture, overlay geometry, or the liveness watchdog.
- No new NuGet dependencies. `Avalonia.Controls.TrayIcon` ships in the `Avalonia` package
  already referenced (11.2.1); on Linux it is backed by the freedesktop
  StatusNotifierItem D-Bus interface.

## Behavior

| Gesture | Result |
|---|---|
| Window close (X) | Panel hides to tray. Capture and overlay keep running. |
| Window minimize | Panel hides to tray (not a taskbar minimize). |
| Tray icon left-click | Panel shows and activates. |
| Tray menu `Show Panel` | Same as left-click. |
| Tray menu `Start Camera` / `Stop Camera` | Invokes the panel's existing `StartCommand` / `StopCommand`. Header reflects the current state. |
| Tray menu `Quit` | Real exit: saves config, closes the overlay, joins the native capture worker, shuts the app down. |

With `MinimizeToTray = false`, close and minimize revert to today's behavior (X quits).

## Configuration

`AppConfig` gains one top-level property, beside `PanelWidth`/`PanelHeight`:

```csharp
// Close/minimize send the control panel to the system tray instead of quitting/taskbar-minimizing.
// Linux (Avalonia) panel only; the WinUI panel ignores it (#38).
public bool MinimizeToTray { get; init; } = true;
```

It participates in `AppConfig.Equals` and `GetHashCode` like every other field. An existing
`config.json` written before this change has no `MinimizeToTray` key, so
`System.Text.Json` leaves the property at its initializer value — existing users get the
feature enabled on upgrade, which is the intent ("enable it by default").

`MainViewModel` gains `[ObservableProperty] private bool minimizeToTray = true;`, read in
`LoadFrom` and written in `ToAppConfig`. No `On…Changed` partial: the flag drives window
behavior only, never native params, so it must NOT call `ApplyLiveParams` or
`ResetLivenessIfRunning`.

## Components

### `Tray/TrayController.cs` (new, Avalonia app)

One class owning the tray icon and its menu. Constructed by `MainWindow` (which already holds
both the VM and the window handle), disposed on real quit.

- Registers via `TrayIcon.SetIcons(Application.Current, new TrayIcons { icon })` — the attached
  property is Avalonia's registration + lifetime hook for tray icons.
- `Icon`: repo-root `cos.png`, added to the Avalonia csproj as an `AvaloniaResource` linked to
  `Assets/cos.png` and loaded with `AssetLoader.Open(new Uri("avares://…/Assets/cos.png"))`.
  The same resource is set as the panel `Window.Icon` — the panel has no icon today, and this
  costs nothing extra.
- `ToolTipText`: `"Camera-on-Screen — running"` while `IsRunning`, `"Camera-on-Screen — stopped"`
  otherwise.
- `Menu`: a `NativeMenu` with `Show Panel`, a run-toggle item, a separator, and `Quit`.
- Subscribes to `Vm.PropertyChanged` for `IsRunning` to swap the toggle item's `Header`
  between `"Start Camera"` and `"Stop Camera"` and refresh the tooltip. The item's `Command`
  routes to `Vm.StartCommand` / `Vm.StopCommand` — no duplicate start/stop logic.
- Exposes callbacks for show and quit; `MainWindow` supplies them.

### `App.axaml.cs`

Sets `desktop.ShutdownMode = ShutdownMode.OnExplicitShutdown`.

This is load-bearing, not cosmetic. Under the default `OnLastWindowClose`, a hidden panel with
no overlay open leaves the app with no open windows and Avalonia exits — the tray icon would
vanish seconds after the user hid the panel. With explicit shutdown, the only exit path is the
tray `Quit` item calling `desktop.Shutdown()`.

Consequence to honor: closing the overlay alone (Alt+F4, already a supported gesture) must not
exit either, and it does not — the existing `OverlayWindow.Closed` handler only clears
`FrameReportingActive`.

### `MainWindow.axaml.cs`

- Holds the `TrayController` and a `_quitting` flag.
- `Closing`: when `Vm.MinimizeToTray && !_quitting`, save config exactly as today (overlay
  bounds captured first — hiding must not lose geometry), then `e.Cancel = true; Hide();`.
  Otherwise fall through to the existing save path.
- `WindowState` change to `Minimized` while `Vm.MinimizeToTray`: set `WindowState = Normal`
  then `Hide()`, so a later restore comes back as a normal window rather than a minimized one.
- Show: `Show()`, `WindowState = Normal`, `Activate()`.
- Quit: `_quitting = true; Close();` — which runs the existing `Closing` save and `Closed`
  teardown (stop `_statusTimer`, close the overlay, `Vm.Dispose()` → `cos_shutdown` joins the
  native capture worker) — then `desktop.Shutdown()`. Skipping `Vm.Dispose()` here would
  destroy the global `std::thread` joinable at process exit → `std::terminate`.

### `MainWindow.axaml`

A `Minimize to tray` `CheckBox` bound `TwoWay` to `MinimizeToTray`, in the camera section
beside the Start/Stop row.

## Interaction with existing contracts

- **Status polling.** `_statusTimer` is a `DispatcherTimer`, not a window-render-driven timer,
  so it keeps ticking at 4 Hz while the panel is hidden. `PollStatusTick` → `CheckLiveness`
  therefore stays armed: a camera that goes silent while the panel is in the tray still
  auto-stops with a sticky `CameraError`, visible when the panel is next shown.
- **`FrameReportingActive`** tracks the *overlay* window's lifetime, never the panel's. Hiding
  the panel does not touch it, so the watchdog neither disarms nor fires spuriously.
- **Config save timing.** Today config is written on panel `Closing` only. Hiding now takes
  that same path, so hiding still persists overlay geometry and effect state — an app killed
  while sitting in the tray loses nothing that it would not already have lost.

## Failure mode: no tray host

If the desktop session runs no StatusNotifierItem host, Avalonia's `TrayIcon` silently does
nothing: the icon never appears, and a hidden panel becomes unreachable with no way to quit
short of `kill`. Avalonia exposes no availability probe, and writing one (D-Bus name query for
`org.kde.StatusNotifierWatcher`) is more machinery than the failure justifies.

Mitigations, in order of what a user reaches for:

1. The `Minimize to tray` checkbox — turn it off before it bites.
2. `"MinimizeToTray": false` in `$XDG_CONFIG_HOME/CameraOnScreen/config.json`, documented in
   the README's Linux section.

The code carries a `ponytail:` comment naming the ceiling (no SNI-availability probe) and the
upgrade path (D-Bus watcher query, auto-disable) so the shortcut is tracked rather than
forgotten.

## Testing

**Core unit tests** (`tests/CameraOnScreen.Core.Tests`, existing xUnit style):

- Config round-trip: a serialized `AppConfig` with `MinimizeToTray = false` deserializes to
  `false`; JSON with no `MinimizeToTray` key deserializes to `true`.
- `AppConfig` equality: two configs differing only in `MinimizeToTray` are not equal (guards
  against the field being forgotten in the hand-written `Equals`).
- VM round-trip: `LoadFrom(config).ToAppConfig(…)` preserves `MinimizeToTray` in both states.

**Human verification gate** (`docs/superpowers/verification/`, KDE/KWin dev box) — window and
tray behavior cannot be asserted from a unit test:

1. Tray icon appears with the app icon and a tooltip.
2. Start the camera, close the panel with X → panel disappears, overlay stays on screen, and
   OBS still captures live video from it.
3. Tray menu reads `Stop Camera` while running; clicking it stops capture and the overlay
   closes; the item flips to `Start Camera`.
4. Tray left-click restores the panel, and the status line still reflects the true state.
5. Minimize button hides to tray the same way; restore comes back as a normal window.
6. `Quit` exits with no leftover process (`pgrep -f CameraOnScreen`) and no abort dialog.
7. With `MinimizeToTray` unchecked, X quits the app as it does today.

## Out of scope

- WinUI panel parity (#38).
- Start-hidden / autostart-to-tray.
- Tray-menu control of overlay visibility, mirror, or effects — the panel is one click away.
- Any SNI-availability probe (see failure mode above).
