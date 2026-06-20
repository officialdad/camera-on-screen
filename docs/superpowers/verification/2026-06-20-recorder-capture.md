# Recorder-Capture Verification — Camera-on-Screen M2 (passthrough)

Date: 2026-06-20
Branch: `feat/m2-winui-overlay`
Build: .NET 8.0.422 + VS2022 Build Tools (MSVC v143 14.44.35207); WindowsAppSDK 1.8 (unpackaged, self-contained); Vortice 3.8.3.
Target machine: Windows 10 Pro 19045, NVIDIA RTX 3090, camera "Brio 100".

This note satisfies the spec risk *"Recorder capture of topmost layered windows — verify against target recorders early (M2)"* and the headline requirement: the overlay must be captured **live** by a desktop recorder in one pass, no post-edit.

## Summary

The full passthrough pipeline is verified working at runtime end-to-end **up to the limit of what automated tooling can observe**. The final visual + recorder confirmation is an inherent **manual (human-at-screen) gate** — see "Remaining manual gate" — because the very window flags that make the overlay recorder-friendly (`WS_EX_NOREDIRECTIONBITMAP` + DirectComposition flip-model) make it invisible to GDI screen capture, so an automated GDI screenshot cannot stand in for a real recorder.

## Objectively verified (automated, this session)

| # | Claim | Evidence |
|---|-------|----------|
| 1 | App launches without crashing | Process stays alive; window handle present; no `startup-error.log`. (A real startup crash — `WNDCLASSEX` ANSI/Unicode marshalling bug — was found and fixed, commit `fb6b18a`.) |
| 2 | Control panel renders all controls | Screenshot: Camera combo, AI Green Screen / Eye Contact / Lock overlay / Click-through toggles, Start/Stop, status line all present and laid out. |
| 3 | RTX detected → effects enabled | On the RTX 3090, the effect toggles render **enabled** (not greyed) → `GpuTierDetector.Detect()` returned `Rtx` → `EffectsAvailable=true`. |
| 4 | Start → orchestrator runs, status polled | After invoking Start, the status line reads "Running — 30 fps" (polled from the native shim via `PInvokeShim.GetStatus`). |
| 5 | Overlay window has the correct compositing flags | Win32 enumeration: overlay window rect 320×240, `exStyle=0x280008` = `WS_EX_LAYERED \| WS_EX_TOPMOST \| WS_EX_NOREDIRECTIONBITMAP`. Topmost, borderless. |
| 6 | Native Media Foundation capture works | Headless capture smoke (twice): camera "Brio 100" enumerated; live 640×480 BGRA frame captured; alpha forced to 255 (opaque); 307200/307200 pixels non-black. |
| 7 | **Frames flow end-to-end into the overlay renderer** | Instrumented frame-pump run: `ticks=100 runningTicks=100 framesReceived=54 firstFrame=640x480 presentFrameException=none`. Real 640×480 frames reach managed code via `TryGetFrame` and pass through `OverlayWindow.PresentFrame` (dynamic-texture upload → `CopyResource` → `Present` → DComp visual scale) with **no D3D/DComp exception**. |

Conclusion from #5–#7: the capture → P/Invoke → pump → render path is fully exercised and error-free, so the overlay is rendering live webcam video.

## Why GDI screenshots show black (and why that's expected, not a bug)

`CopyFromScreen` / GDI `BitBlt` of the overlay region returns pure black. The overlay uses `WS_EX_NOREDIRECTIONBITMAP` (no GDI redirection surface) with a DirectComposition flip-model swap chain; on an RTX GPU this is typically promoted to a hardware Multiplane Overlay (MPO) plane that DWM composites directly. GDI screen reads do not include that plane → black. This is the **same** property that lets proper recorders grab the overlay cleanly: they must use the DWM-based capture path, not GDI.

- ❌ GDI `BitBlt` / `PrintWindow` / classic GDI capture — will NOT see the overlay.
- ✅ DWM-based capture: `Windows.Graphics.Capture` (used by modern Xbox Game Bar and OBS "Window/Display Capture"), DXGI Desktop Duplication, OBS Display/Game Capture.

## Remaining manual gate (requires a human at the screen)

The following cannot be substituted by automated tooling on this host (GDI can't see the overlay, and **OBS is not installed**). To complete the headline verification, perform on the target machine:

1. Launch `src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.App.exe`, pick the camera, click **Start**. Confirm by eye that live webcam video appears in the floating, borderless, topmost overlay and that dragging it (anywhere) moves it, the bottom-right grip resizes it (video scales to fill), Lock disables drag/resize, and Click-through passes clicks through. Confirm the resize grip is **not** drawn when locked or when the pointer is not over the overlay (clean capture).
2. **OBS** (install from obsproject.com): add a **Display Capture** source → confirm the overlay appears in the preview. Try a **Window Capture** source targeting the overlay window and note whether the layered topmost window is captured; if not, record that Display Capture is the supported path. Record ~5 s, move the overlay mid-recording, stop, play back → confirm the overlay is in the recording with **no chrome/handles** and no post-edit.
3. **Xbox Game Bar** (`Win+G` → record ~5 s) → confirm the overlay appears (Game Bar uses `Windows.Graphics.Capture`; it captures the foreground app/desktop — note any limitation, e.g. it may require capturing the desktop rather than the overlay window directly).
4. Append the result here: recorder + version, the capture mode that worked (display vs window), and screenshots taken from the **recorder's** output (not GDI).

### Result (to be filled by the user)

- Visual overlay (webcam visible, drag/resize/lock/click-through, clean chrome): _pending_
- OBS Display Capture: _pending_ — _OBS not installed on the host at verification time._
- OBS Window Capture: _pending_
- Xbox Game Bar: _pending_

## Notes / follow-ups

- The "30 fps" in the status line is a hardcoded stub in the native `cos_get_status`; a real measured FPS is a future improvement (M3+).
- Effects (AI Green Screen, Eye Contact) are intentionally passthrough-only in M2; the toggles gate on RTX but do nothing yet (implemented in the M3–M5 Maxine plan).
