# Verification: minimize to system tray (Linux panel)

Window state, native tray menus, and StatusNotifierItem cannot be asserted from a unit test —
this is a human gate, run on the KDE/KWin dev box.

Build and run: `dotnet run --project src/CameraOnScreen.App.Avalonia`

| # | Step | Expected |
|---|---|---|
| 1 | Launch the app | Camera-on-Screen icon in the system tray; hovering it reads "Camera-on-Screen — stopped" |
| 2 | Start the camera, then close the panel with X | Panel disappears; overlay stays on screen; process still alive (`pgrep -f CameraOnScreen.App.Avalonia`) |
| 3 | With the panel hidden, capture the screen in OBS | The overlay is still live video, not a frozen frame |
| 4 | Open the tray menu | Reads `Show Panel` / `Stop Camera` / `Quit`; tooltip reads "— running" |
| 5 | Click `Stop Camera` | Capture stops, the overlay closes, the item flips to `Start Camera` |
| 6 | Left-click the tray icon | Panel reappears, focused, and its status line matches the real state |
| 7 | Press the panel's minimize button | Panel hides to the tray (no taskbar entry); restoring it comes back as a normal window, not minimized |
| 8 | Tray `Quit` | Process exits; `pgrep -f CameraOnScreen.App.Avalonia` prints nothing; no abort dialog on stderr |
| 9 | Relaunch, turn "Minimize to tray" off, click X | App exits as it did before the feature |
| 10 | Relaunch | The toggle is still off (persisted), and X still exits |
| 11 | #62 arm gate (instrumented, not an organic repro — a launch-and-quit loop is a negative observation: it would also pass on the broken `_opened` latch if the transient `_NET_WM_STATE_HIDDEN` this box happens to hit just didn't fire that run). In `MainWindow`'s constructor, temporarily add `DispatcherTimer.RunOnce(() => WindowState = WindowState.Minimized, TimeSpan.FromMilliseconds(200));` right after the existing `Opened`/`PropertyChanged` subscriptions, rebuild, and launch. Then change the delay to `TimeSpan.FromSeconds(2)`, rebuild, and launch again | At 200 ms the panel **stays visible** — that lands well inside the 1 s grace window, so `_armed` is still false and the old bug (hiding on this transient state) is proven not to reproduce. At 2 s the panel **hides to the tray** — `_armed` is true by then, proving the fix did not simply disable minimize-to-tray outright. Both directions matter: a row that only proved the panel stays visible would also pass on code that broke minimize-to-tray entirely. Revert the instrumentation and rebuild afterwards |
| 12 | Left-click the tray icon (as opposed to using its menu) | Either the panel restores or the menu opens — both are acceptable host behaviors; note which one your desktop does |
| 13 | Tray `Quit` while the panel is hidden and has never been shown this run (launch, hide via minimize or X, then Quit from the tray without ever restoring the panel) | Clean exit: `pgrep -f CameraOnScreen.App.Avalonia` prints nothing, no abort message on stderr. Proves Avalonia raises `Closing`/`Closed` for a window that was never shown — if it does not, `cos_shutdown` never runs and the process aborts on exit |
| 14 | Start the camera and open the overlay, hide the panel (minimize or X), then Quit from the tray | Overlay closes before native teardown, clean stderr, no orphan PID |
| 15 | No-tray-host rehearsal: on a session with no StatusNotifierItem host, hide the panel, then walk the README recovery end to end (`pkill -f CameraOnScreen.App.Avalonia`, edit `"MinimizeToTray": false` in config.json, relaunch) | Panel reappears and X now exits normally, matching the README steps exactly |
| 16 | Session logout or reboot with the panel open | The app does not block the logout/reboot and does not strand the capture worker (no hung process, no dialog blocking session end) |
| 17 | Hide the panel, then `kill -9` the process, then relaunch | Config file is valid JSON on relaunch and the overlay reopens at its last-known geometry |
| 18 | #61 scroll gate (instrumented, not an organic repro — `StatusError` is only ever set from the 250 ms `PollStatusTick` -> native status poll, independent of the startup capability probe, so a literal `Text=` override on the TextBlock never goes visible and proves nothing). In `MainViewModel.OnStatus`, temporarily comment out the `StatusError = s.Error;` line so the injected value below isn't overwritten 250 ms later — without this the line vanishes almost as soon as it appears, and the tester ends up judging the scroll against content that is simultaneously collapsing. In `MainWindow`'s constructor, temporarily add `DispatcherTimer.RunOnce(() => _services.Vm.StatusError = "TEST ERROR", TimeSpan.FromSeconds(5));` after the existing subscriptions, rebuild, launch **without starting the camera** (so the real native status stays error-free and doesn't race the injected value), scroll the panel to the top, and watch through the 5 s mark | The panel auto-scrolls the red error line into view when the timer fires, and the line stays put (not overwritten, since `OnStatus` no longer touches `StatusError`) so the scroll is unambiguous. Revert both instrumentation edits — the `MainWindow` timer and the commented-out `OnStatus` line — and rebuild afterwards |

Result: ____ (date / tester / notes)
