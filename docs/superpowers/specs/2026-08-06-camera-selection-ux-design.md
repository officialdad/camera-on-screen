# Camera Selection UX — Design

**Date:** 2026-08-06
**Issue:** #57
**Follows:** #55 / PR #56 (virtual cameras capture nothing on Linux)

## 1. Goal

Make picking a camera work the way the effect toggles already do — live, while
running. Four gaps, all surfaced by using a phone-as-webcam (`scrcpy
--v4l2-sink`) alongside the Brio:

1. Changing camera requires Stop → select → Start.
2. The camera list is built once at startup, so a virtual camera that appears
   later is invisible until the app restarts.
3. No resolution readout, though negotiated resolution varies wildly by source.
4. A camera disappearing mid-capture leaves the overlay frozen on a stale
   frame with no signal.

Non-goal: changing what capture does. This is entirely about selecting and
observing it.

## 2. Why no C-ABI change

The C ABI (`CosStatus`/`CosParams`/`CosCaps` and their `[StructLayout]`
mirrors) is byte-for-byte parity-critical per the repo contract. None of the
four gaps needs it:

- **Hot-swap** already works natively. Both backends' `Capture::Start` open
  with `StopLocked()` (`capture_v4l2.cpp:591`, `capture.cpp:602`), so calling
  Start again on a different device is a clean, serialized restart. The
  capability exists and is simply never invoked.
- **Resolution** is already crossing the boundary — `cos_get_frame` writes
  `width`/`height` on every call, which the overlay pump consumes and discards.
- **Liveness** is already crossing it too: `Capture::LatestFrame` clears
  `hasNewFrame` on read (`capture_v4l2.cpp:627`), so `TryGetFrame` returning
  true *is* the "a new frame arrived" signal.

So all four land in C# — shared `Core` where the logic is testable, per-panel
only for event wiring.

## 3. Hot-swap on selection change

`MainViewModel` gains the `On…Changed` partial that `SelectedCamera` is
conspicuously missing:

```csharp
partial void OnSelectedCameraChanged(CameraInfo? value)
{
    if (!IsRunning || value is null || value.Value.Id == _activeCameraId) return;
    _orchestrator.Start(BuildParams());   // native Start tears down first
    _activeCameraId = value.Value.Id;
}
```

`_activeCameraId` — the camera capture is actually running on, stamped in
`Start()` and here — guards two cases that would otherwise cause a spurious
restart:

- **Transient null.** A list refresh can momentarily clear the selection.
- **Re-selecting the same camera.** Harmless but wasteful: a full worker
  teardown, device reopen, and Maxine re-init for nothing.

`IsRunning` never changes, so `SyncOverlay` (which keys off it alone) leaves
the overlay window untouched — position, size, and mirror all survive a swap.
The overlay's `WriteableBitmap` is already size-adaptive
(`OverlayWindow.cs:107`), so swapping between sources of different resolution
needs nothing extra.

## 4. Live camera list

`MainViewModel.RefreshCameras()` in Core, diffing the live enumeration against
`Cameras` by `Id`: remove entries that are gone, add ones that are new, leave
existing entries alone.

Diff rather than clear-and-refill specifically to avoid selection churn — a
`Cameras.Clear()` drops `SelectedItem` to null and the re-add would fire
`OnSelectedCameraChanged` twice for what the user experiences as no change.
The `_activeCameraId` guard would absorb that, but not creating the churn is
simpler than tolerating it.

Wired to the ComboBox's `DropDownOpened` in both panels (Avalonia and WinUI
both expose it). Opening the dropdown is exactly the moment the list needs to
be true, and it costs no extra chrome.

**Risk to verify before shipping:** `Capture::Enumerate` opens all 64
`/dev/video*` nodes `O_RDWR`. During #55 debugging, opening a v4l2loopback
node was implicated in flipping its advertised caps between `VIDEO_CAPTURE`
and `VIDEO_OUTPUT`. A refresh must not perturb a live capture. If it does,
the fallback is skipping the currently-active device during refresh — it is
by definition present, so nothing is lost.

## 5. One frame callback, two consumers

`OverlayWindow` takes a `Action<int,int>? onFrame` ctor param, invoked from
`Present()` when `TryGetFrame` succeeds. `MainWindow` passes
`Vm.OnFrameReceived`.

This single signal feeds both remaining features, which is why it is one
callback and not two:

- **Resolution readout.** `FrameWidth`/`FrameHeight` observables; the status
  line converter renders `Running · 30.0 fps · 2560×1440`. Blank until the
  first frame.
- **Liveness.** Stamps `_lastFrameMs`.

To keep it off the hot path, the VM only does real work when the size changes;
the liveness stamp is a single field write per frame.

## 6. Disconnect watchdog

`PollStatusTick` calls `CheckLiveness(nowMs)`. Two windows, one mechanism:

| Condition | Threshold | Result |
|---|---|---|
| Frames were flowing, then stopped | 2 s | `"Camera disconnected"`, auto-Stop |
| Never delivered a first frame after Start | 5 s | `"No frames from camera"`, auto-Stop |

Auto-Stop sets `IsRunning` false, which closes the overlay through the
existing `SyncOverlay` path. A frozen stale frame silently baked into a
recording is worse than the overlay visibly vanishing.

The second window is deliberately longer: Start can legitimately take seconds
when a Maxine effect is initializing on first use.

**Why Core-side timeout rather than native error detection:** when scrcpy
dies, v4l2loopback does not report an error. It goes quiet — `select` times
out forever and the worker's `break` paths never fire. A timeout catches that
case, genuine ioctl failures, and the Windows equivalents, with one
implementation and no native change.

`CheckLiveness` takes `nowMs` as a parameter rather than reading the clock, so
it is unit-testable without injecting a clock abstraction. Callers pass
`Environment.TickCount64`.

**The watchdog is opt-in** (`Vm.FrameReportingActive`), owned by the Avalonia
panel and true only while an overlay window exists. `PollStatusTick` is shared
Core, but only the Avalonia overlay pump reports frames, so the flag makes
"nobody is reporting frames" mean *disabled* rather than *dead*. It covers two
cases, which is why it tracks the overlay's lifetime rather than being set once
at composition:

- **Windows** (§9) never sets it, so its capture is unaffected.
- **Alt+F4 on the overlay** closes it while capture keeps running (an existing
  behaviour — `MainWindow.axaml.cs:66`). The pump dies with the window, so
  without the flag the watchdog would read that silence as a dead camera and
  stop a healthy capture.

## 7. Testing

Core unit tests (added to the existing 75):

- Swap while running restarts; swap while stopped does not.
- Re-selecting the same camera does not restart.
- Null selection during refresh does not restart.
- `RefreshCameras` adds new, removes gone, preserves the selected instance.
- Both watchdog windows fire at their thresholds; neither fires early.
- A frame arriving resets the disconnect window.

Manual gate (visual confirmation is an inherent human gate in this repo):

- Swap Brio ↔ scrcpy while recording; overlay keeps geometry, video switches.
- Start scrcpy after the app; open the dropdown; the device appears.
- Refresh the dropdown during a live capture; capture is undisturbed (§4 risk).
- Kill scrcpy mid-capture; overlay closes, panel shows "Camera disconnected".

## 8. Files

| File | Change |
|---|---|
| `Core/ViewModels/MainViewModel.cs` | `OnSelectedCameraChanged`, `_activeCameraId`, `RefreshCameras`, `OnFrameReceived`, `CheckLiveness`, frame-size observables |
| `App.Avalonia/Overlay/OverlayWindow.cs` | `onFrame` ctor param, invoked in `Present()` |
| `App.Avalonia/MainWindow.axaml{,.cs}` | `DropDownOpened` wiring, pass `OnFrameReceived`, status line |
| `App.Avalonia/MainWindow.axaml.cs` | Own `FrameReportingActive` across the overlay's lifetime |
| `Core.Tests/` | Cases above |

Native shim: unchanged. WinUI panel: unchanged (§9).

## 9. Windows deferred

The WinUI panel is not touched. It cannot be built on this box (no WinUI
workload — see CLAUDE.md), so any change there would ship verified only by a
compile in CI, and #38 has not yet restored a Windows RTX environment to run
it on.

Every change here is additive to `Core`, so Windows behavior is unchanged:
the new observables go unread by its XAML, `RefreshCameras` is never called
without the `DropDownOpened` wiring, and the watchdog stays off via §6's flag.
Hot-swap is the one feature Windows would get for free — `OnSelectedCameraChanged`
is in shared Core and `capture.cpp:602` already restarts correctly — but it
goes unverified there until #38 lands.

Follow-up for when Windows returns: wire `DropDownOpened`, pass
`OnFrameReceived` from the WinUI pump, add the resolution to its status line,
and set the watchdog flag.
