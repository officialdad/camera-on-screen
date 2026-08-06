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
| 11 | Launch and quit the app five times in a row | The panel window appears every time (an occasional icon-but-no-window launch was seen once during development and never attributed — report it if it recurs) |
| 12 | Left-click the tray icon (as opposed to using its menu) | Either the panel restores or the menu opens — both are acceptable host behaviors; note which one your desktop does |

Result: ____ (date / tester / notes)
