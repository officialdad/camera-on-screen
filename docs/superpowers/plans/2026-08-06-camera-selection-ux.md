# Camera Selection UX Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user change camera while running, see a live device list and the negotiated resolution, and get told when a camera disappears instead of a frozen frame.

**Architecture:** All four features land in C# — the native shim is untouched, because it already exposes everything needed (`Capture::Start` restarts internally, `cos_get_frame` returns width/height, `LatestFrame` clears its new-frame flag so a `true` return *is* the liveness signal). Logic lives in shared `Core` (`MainViewModel`) where it is unit-testable; the Avalonia panel only wires events. The WinUI panel is deliberately not touched.

**Tech Stack:** .NET 8, CommunityToolkit.Mvvm source generators (`[ObservableProperty]`, `[RelayCommand]`), Avalonia 11, xUnit.

**Spec:** `docs/superpowers/specs/2026-08-06-camera-selection-ux-design.md`
**Issue:** #57

## Global Constraints

- **Builds and tests must be pristine — 0 warnings.** CI enforces `TreatWarningsAsErrors`.
- **Do not touch `src/CameraOnScreen.App/`** (the WinUI panel) — spec §9. Changes to shared `Core` are fine and must stay inert on Windows.
- **Do not touch `native/shim/`.** No C-ABI change; `CosStatus`/`CosParams`/`CosCaps` struct parity stays as-is.
- **`dotnet` is not on PATH on this box.** Use `~/.dotnet/dotnet` for every command.
- **`CameraInfo` is a `readonly record struct`** (`Core/Native/Contracts.cs:3`), so `CameraInfo?` is `Nullable<CameraInfo>` — access via `.Value.Id`.
- Build: `~/.dotnet/dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj`
- Test: `~/.dotnet/dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
- Single test: append `--filter "FullyQualifiedName~TestName"`

---

### Task 1: Hot-swap on camera selection change

**Files:**
- Modify: `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs` (add field near line 22; `Start`/`Stop` at 214-226; new partial near 172)
- Modify: `src/CameraOnScreen.Core/Native/FakeShim.cs:17` (add `StartCount` so tests can assert a restart happened)
- Test: `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`

**Interfaces:**
- Consumes: `Orchestrator.Start(ShimParams)` (`Orchestration/Orchestrator.cs:76`) — calls `ApplyParams` then `_shim.Start()`. Native `Capture::Start` calls `StopLocked()` first, so calling it again while running is a clean restart.
- Produces: `MainViewModel._activeCameraId` (private `string?`) — the camera capture is actually running on. Task 4 resets liveness state alongside it.

- [ ] **Step 1: Add `StartCount` to the test double**

In `src/CameraOnScreen.Core/Native/FakeShim.cs`, replace line 17:

```csharp
    public void Start() => _running = true;
```

with:

```csharp
    public int StartCount { get; private set; }
    public void Start() { _running = true; StartCount++; }
```

- [ ] **Step 2: Write the failing tests**

Add to `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`. Note the existing private `Build` helper seeds exactly one camera; these tests need two, so they construct their own shim.

```csharp
    private static MainViewModel BuildWithCameras(out FakeShim shim, params string[] ids)
    {
        shim = new FakeShim();
        foreach (var id in ids) shim.Cameras.Add(new CameraInfo(id, id));
        var vm = new MainViewModel(new Orchestrator(shim, GpuTier.Rtx), shim);
        foreach (var cam in shim.Cameras) vm.Cameras.Add(cam);
        return vm;
    }

    [Fact]
    public void Selecting_a_different_camera_while_running_restarts_capture()
    {
        // The whole point of #57: no Stop/Start round-trip. Native Capture::Start tears the
        // old session down first, so a second Start IS the swap.
        var vm = BuildWithCameras(out var shim, "a", "b");
        vm.SelectedCamera = vm.Cameras[0];
        vm.StartCommand.Execute(null);
        Assert.Equal(1, shim.StartCount);

        vm.SelectedCamera = vm.Cameras[1];

        Assert.Equal(2, shim.StartCount);
        Assert.Equal("b", shim.LastParams?.CameraId);
        Assert.True(vm.IsRunning); // IsRunning must NOT flip, or the overlay would close and reopen
    }

    [Fact]
    public void Selecting_a_camera_while_stopped_does_not_start_capture()
    {
        var vm = BuildWithCameras(out var shim, "a", "b");
        vm.SelectedCamera = vm.Cameras[0];
        vm.SelectedCamera = vm.Cameras[1];
        Assert.Equal(0, shim.StartCount);
        Assert.False(vm.IsRunning);
    }

    [Fact]
    public void Reselecting_the_active_camera_does_not_restart_capture()
    {
        // A restart is a full worker teardown + device reopen + Maxine re-init. Not for a no-op.
        var vm = BuildWithCameras(out var shim, "a", "b");
        vm.SelectedCamera = vm.Cameras[0];
        vm.StartCommand.Execute(null);
        vm.SelectedCamera = vm.Cameras[0];
        Assert.Equal(1, shim.StartCount);
    }

    [Fact]
    public void Clearing_the_selection_while_running_does_not_restart_capture()
    {
        // RefreshCameras (Task 2) can drop the selected camera, which sets this to null.
        var vm = BuildWithCameras(out var shim, "a", "b");
        vm.SelectedCamera = vm.Cameras[0];
        vm.StartCommand.Execute(null);
        vm.SelectedCamera = null;
        Assert.Equal(1, shim.StartCount);
    }
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `~/.dotnet/dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~camera"`

Expected: `Selecting_a_different_camera_while_running_restarts_capture` FAILS with `Assert.Equal() Failure: Expected: 2, Actual: 1`. The other three pass already (nothing fires today) — that is fine, they are regression guards for the code added next.

- [ ] **Step 4: Add the active-camera field and stamp it in Start/Stop**

In `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs`, add after the `_teleportModifiers` field (line 22):

```csharp
    // The camera capture is actually running on — distinct from SelectedCamera, which is whatever
    // the combo shows. Guards the hot-swap against a transient null during a list refresh and
    // against re-selecting the camera already running (a restart is a full worker teardown).
    private string? _activeCameraId;
```

Replace the `Start` and `Stop` commands (lines 214-226):

```csharp
    [RelayCommand]
    private void Start()
    {
        _orchestrator.Start(BuildParams());
        _activeCameraId = SelectedCamera?.Id;
        IsRunning = true;
    }

    [RelayCommand]
    private void Stop()
    {
        _orchestrator.Stop();
        _activeCameraId = null;
        IsRunning = false;
    }
```

- [ ] **Step 5: Add the hot-swap partial**

In the same file, add after `OnExposureValueChanged` (line 172), alongside the other `On…Changed` partials:

```csharp
    // The camera equivalent of the live param push above. Native Capture::Start opens with
    // StopLocked() (capture_v4l2.cpp:591, capture.cpp:602), so calling Start again is a clean
    // restart on the new device. IsRunning deliberately does NOT change: SyncOverlay keys off it
    // alone, so leaving it true keeps the overlay window, its geometry, and its mirror state.
    partial void OnSelectedCameraChanged(CameraInfo? value)
    {
        if (!IsRunning || value is null || value.Value.Id == _activeCameraId) return;
        _orchestrator.Start(BuildParams());
        _activeCameraId = value.Value.Id;
    }
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `~/.dotnet/dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`

Expected: PASS, all tests (75 existing + 4 new = 79).

- [ ] **Step 7: Commit**

```bash
git add src/CameraOnScreen.Core/ViewModels/MainViewModel.cs src/CameraOnScreen.Core/Native/FakeShim.cs tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs
git commit -m "feat: hot-swap camera while running (#57)

SelectedCamera was the only user-facing property without an On...Changed
partial, so changing camera did nothing until the next Stop/Start. The native
side already supported it: both Capture::Start implementations call
StopLocked() first, so re-Starting on a new device is a clean restart.

Guarded by _activeCameraId, which suppresses a restart on a transient null
selection (a list refresh can cause one) and on re-selecting the camera
already running. IsRunning never flips, so the overlay keeps its geometry."
```

---

### Task 2: Live camera list on dropdown open

**Files:**
- Modify: `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs` (add `RefreshCameras` near `Cameras`, line 66)
- Modify: `src/CameraOnScreen.App.Avalonia/MainWindow.axaml:45-53` (name the ComboBox, hook `DropDownOpened`)
- Modify: `src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs` (handler)
- Test: `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`

**Interfaces:**
- Consumes: `INativeShim.EnumerateCameras()` → `IReadOnlyList<CameraInfo>` (`Native/Contracts.cs:44`), reachable as `ShimRef.EnumerateCameras()`.
- Produces: `MainViewModel.RefreshCameras()` — `public void`, no args. Task 5's manual gate exercises it.

- [ ] **Step 1: Write the failing tests**

Add to `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`:

```csharp
    [Fact]
    public void RefreshCameras_adds_devices_that_appeared()
    {
        // A v4l2loopback node only announces VIDEO_CAPTURE while a writer is attached, so
        // starting scrcpy after the app is exactly this case.
        var vm = BuildWithCameras(out var shim, "a");
        shim.Cameras.Add(new CameraInfo("b", "b"));

        vm.RefreshCameras();

        Assert.Equal(2, vm.Cameras.Count);
        Assert.Contains(vm.Cameras, c => c.Id == "b");
    }

    [Fact]
    public void RefreshCameras_removes_devices_that_vanished()
    {
        var vm = BuildWithCameras(out var shim, "a", "b");
        shim.Cameras.RemoveAll(c => c.Id == "b");

        vm.RefreshCameras();

        Assert.Single(vm.Cameras);
        Assert.Equal("a", vm.Cameras[0].Id);
    }

    [Fact]
    public void RefreshCameras_leaves_an_unchanged_selection_alone()
    {
        // Diff, not clear-and-refill: a Clear() would null the ComboBox selection and fire
        // OnSelectedCameraChanged twice for what the user sees as no change.
        var vm = BuildWithCameras(out var shim, "a", "b");
        vm.SelectedCamera = vm.Cameras[1];
        vm.StartCommand.Execute(null);

        vm.RefreshCameras();

        Assert.Equal("b", vm.SelectedCamera?.Id);
        Assert.Equal(1, shim.StartCount); // no spurious restart
    }
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `~/.dotnet/dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~RefreshCameras"`

Expected: FAIL to compile — `'MainViewModel' does not contain a definition for 'RefreshCameras'`.

- [ ] **Step 3: Implement `RefreshCameras`**

In `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs`, add directly after the `Cameras` property (line 66):

```csharp
    /// <summary>Re-enumerate devices and diff into <see cref="Cameras"/> by Id: drop what's gone,
    /// add what's new, leave existing entries alone. Diff rather than clear-and-refill so the
    /// ComboBox selection never churns — a Clear() nulls SelectedItem, and the re-add would fire
    /// OnSelectedCameraChanged twice for what the user experiences as no change.
    /// Called when the camera dropdown opens: a v4l2loopback device (scrcpy, OBS) only announces
    /// itself while its writer is attached, so the startup enumeration goes stale.</summary>
    public void RefreshCameras()
    {
        var live = ShimRef.EnumerateCameras();
        for (int i = Cameras.Count - 1; i >= 0; i--)
            if (!live.Any(c => c.Id == Cameras[i].Id))
                Cameras.RemoveAt(i);
        foreach (var cam in live)
            if (!Cameras.Any(c => c.Id == cam.Id))
                Cameras.Add(cam);
    }
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `~/.dotnet/dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`

Expected: PASS (82 tests).

- [ ] **Step 5: Wire the dropdown**

In `src/CameraOnScreen.App.Avalonia/MainWindow.axaml`, replace the ComboBox opening tag (lines 45-47):

```xml
                                <ComboBox HorizontalAlignment="Stretch"
                                          ItemsSource="{Binding Cameras}"
                                          SelectedItem="{Binding SelectedCamera, Mode=TwoWay}">
```

with:

```xml
                                <ComboBox HorizontalAlignment="Stretch"
                                          ItemsSource="{Binding Cameras}"
                                          SelectedItem="{Binding SelectedCamera, Mode=TwoWay}"
                                          DropDownOpened="CameraCombo_DropDownOpened">
```

In `src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs`, add before `SyncOverlay` (line 57):

```csharp
    // Opening the dropdown is exactly the moment the list needs to be true, and it costs no
    // extra chrome. Enumeration opens every /dev/video* node — see the manual gate in the plan
    // for the check that this does not perturb a live capture.
    private void CameraCombo_DropDownOpened(object? sender, EventArgs e) =>
        _services.Vm.RefreshCameras();
```

- [ ] **Step 6: Build and verify no warnings**

Run: `~/.dotnet/dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj`

Expected: `Build succeeded.` with `0 Warning(s)`.

- [ ] **Step 7: Verify a refresh does not disturb a live capture (spec §4 risk)**

This is the one assumption the spec flagged for verification rather than assertion. `Capture::Enumerate` opens all 64 `/dev/video*` nodes `O_RDWR`, and during #55 debugging that was implicated in flipping a v4l2loopback node's advertised caps between `VIDEO_CAPTURE` and `VIDEO_OUTPUT`.

```bash
# Terminal 1: feed a virtual camera
ffmpeg -re -f lavfi -i testsrc=size=1280x720:rate=30 -pix_fmt yuv420p -f v4l2 /dev/video3

# Terminal 2: run the panel, Start on the Brio, then open the camera dropdown
#             ~10 times while watching the overlay
~/.dotnet/dotnet run --project src/CameraOnScreen.App.Avalonia
```

Expected: the overlay keeps rendering throughout, fps does not drop to 0, and the virtual camera appears in the list.

If the capture DOES stutter or die, apply the spec's fallback — skip the active device during refresh, since it is by definition present:

```csharp
    public void RefreshCameras()
    {
        var live = ShimRef.EnumerateCameras();
        for (int i = Cameras.Count - 1; i >= 0; i--)
            if (!live.Any(c => c.Id == Cameras[i].Id) && Cameras[i].Id != _activeCameraId)
                Cameras.RemoveAt(i);
        foreach (var cam in live)
            if (!Cameras.Any(c => c.Id == cam.Id))
                Cameras.Add(cam);
    }
```

Note that fallback only protects the *list entry*; if enumeration itself is what disturbs capture, the real fix is a native change to skip the open on the active device — record that as a new issue rather than expanding this task.

- [ ] **Step 8: Commit**

```bash
git add src/CameraOnScreen.Core/ViewModels/MainViewModel.cs src/CameraOnScreen.App.Avalonia/MainWindow.axaml src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs
git commit -m "feat: refresh the camera list when the dropdown opens (#57)

The list was built once at composition (Services.cs:34) and never again, so a
v4l2loopback device that appeared later — scrcpy started after the app — stayed
invisible until a full app restart.

RefreshCameras diffs by Id rather than clearing and refilling, so the ComboBox
selection never churns."
```

---

### Task 3: Report frame size from the overlay pump

**Files:**
- Modify: `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs` (observables + `OnFrameReceived`)
- Modify: `src/CameraOnScreen.App.Avalonia/Overlay/OverlayWindow.cs:33-34` (ctor param), `:105` (invoke)
- Modify: `src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs:62-63` (pass the callback)
- Modify: `src/CameraOnScreen.App.Avalonia/Converters.cs:13-21` (status line)
- Modify: `src/CameraOnScreen.App.Avalonia/MainWindow.axaml:60-64` (bind the new values)
- Test: `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`

**Interfaces:**
- Consumes: `INativeShim.TryGetFrame(byte[], out int, out int)` — already called by `OverlayWindow.Present()` at line 105; returns false until a *new* frame lands, because `Capture::LatestFrame` clears `hasNewFrame` on read.
- Produces:
  - `MainViewModel.OnFrameReceived(int width, int height, long nowMs)` — `public void`. `nowMs` is a parameter rather than a clock read so Task 4's watchdog tests stay deterministic.
  - `MainViewModel.FrameWidth` / `FrameHeight` — `int` observables, 0 until the first frame.
  - `OverlayWindow` ctor gains a trailing `Action<int,int>? onFrame = null` parameter.

- [ ] **Step 1: Write the failing test**

Add to `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`:

```csharp
    [Fact]
    public void OnFrameReceived_publishes_the_negotiated_frame_size()
    {
        // Resolution varies a lot by source — a Brio 100 falls back to 640x480 YUYV while a
        // scrcpy loopback runs 2560x1440 — and the panel had no way to show which you got.
        var vm = BuildWithCameras(out _, "a");
        Assert.Equal(0, vm.FrameWidth);

        vm.OnFrameReceived(2560, 1440, nowMs: 1000);

        Assert.Equal(2560, vm.FrameWidth);
        Assert.Equal(1440, vm.FrameHeight);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `~/.dotnet/dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~OnFrameReceived"`

Expected: FAIL to compile — `'MainViewModel' does not contain a definition for 'FrameWidth'`.

- [ ] **Step 3: Add the observables and the callback**

In `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs`, add to the observable block after `frameInterpAvailable` (line 86):

```csharp
    [ObservableProperty] private int frameWidth;    // negotiated capture size; 0 until the first frame
    [ObservableProperty] private int frameHeight;
```

Add the field next to `_activeCameraId`:

```csharp
    // Set by OnFrameReceived. Task 4's watchdog reads it; null means no frame has arrived yet
    // since the last (re)start.
    private long? _lastFrameMs;
```

Add the method after `PollStatusTick` (line 47):

```csharp
    /// <summary>Called by the overlay's frame pump for every frame it successfully pulls.
    /// TryGetFrame returning true IS the liveness signal — Capture::LatestFrame clears its
    /// new-frame flag on read, so a true return means a genuinely new frame arrived.
    /// <paramref name="nowMs"/> is passed in rather than read from the clock so the watchdog
    /// (CheckLiveness) is unit-testable without a clock abstraction; callers pass
    /// Environment.TickCount64.</summary>
    public void OnFrameReceived(int width, int height, long nowMs)
    {
        _lastFrameMs = nowMs;
        if (width == FrameWidth && height == FrameHeight) return; // hot path: usually unchanged
        FrameWidth = width;
        FrameHeight = height;
    }
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `~/.dotnet/dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`

Expected: PASS (83 tests).

- [ ] **Step 5: Add the ctor param to the overlay and invoke it**

In `src/CameraOnScreen.App.Avalonia/Overlay/OverlayWindow.cs`, replace the ctor signature (lines 33-34):

```csharp
    public OverlayWindow(INativeShim shim, double x, double y, double w, double h, bool mirror,
                         HotkeyModifiers teleportChord = HotkeyModifiers.Control)
    {
        _shim = shim;
```

with:

```csharp
    public OverlayWindow(INativeShim shim, double x, double y, double w, double h, bool mirror,
                         HotkeyModifiers teleportChord = HotkeyModifiers.Control,
                         Action<int, int>? onFrame = null)
    {
        _shim = shim;
        _onFrame = onFrame;
```

Add the field next to `_bitmap` (line 31):

```csharp
    private readonly Action<int, int>? _onFrame;
```

In `Present()`, replace line 105:

```csharp
        if (!_shim.TryGetFrame(_buffer, out int w, out int h) || w <= 0) return;
```

with:

```csharp
        if (!_shim.TryGetFrame(_buffer, out int w, out int h) || w <= 0) return;
        _onFrame?.Invoke(w, h);
```

- [ ] **Step 6: Pass the callback from the panel**

In `src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs`, replace the overlay construction (lines 62-63):

```csharp
            var w = new Overlay.OverlayWindow(_services.Vm.ShimRef, b.X, b.Y, b.W, b.H, _services.Vm.Mirror,
                _services.Loaded.Overlay.TeleportModifiers);
```

with:

```csharp
            var w = new Overlay.OverlayWindow(_services.Vm.ShimRef, b.X, b.Y, b.W, b.H, _services.Vm.Mirror,
                _services.Loaded.Overlay.TeleportModifiers,
                (fw, fh) => _services.Vm.OnFrameReceived(fw, fh, Environment.TickCount64));
```

- [ ] **Step 7: Show the resolution in the status line**

In `src/CameraOnScreen.App.Avalonia/Converters.cs`, replace the `StatusLineConverter.Convert` body (lines 15-20):

```csharp
    public object Convert(IList<object?> values, Type targetType, object? parameter, CultureInfo culture)
    {
        bool running = values.Count > 0 && values[0] is bool b && b;
        double fps = values.Count > 1 && values[1] is double d ? d : 0;
        return running ? $"Running — {fps:F0} fps" : "Stopped";
    }
```

with:

```csharp
    public object Convert(IList<object?> values, Type targetType, object? parameter, CultureInfo culture)
    {
        bool running = values.Count > 0 && values[0] is bool b && b;
        double fps = values.Count > 1 && values[1] is double d ? d : 0;
        int w = values.Count > 2 && values[2] is int iw ? iw : 0;
        int h = values.Count > 3 && values[3] is int ih ? ih : 0;
        if (!running) return "Stopped";
        // Size stays hidden until the first frame lands, so it never shows a stale 0×0.
        return w > 0 && h > 0 ? $"Running — {fps:F0} fps — {w}×{h}" : $"Running — {fps:F0} fps";
    }
```

Update the doc comment on line 12 to match:

```csharp
/// <summary>[IsRunning(bool), Fps(double), FrameWidth(int), FrameHeight(int)] -> status string.</summary>
```

In `src/CameraOnScreen.App.Avalonia/MainWindow.axaml`, replace the MultiBinding (lines 60-63):

```xml
                                        <MultiBinding Converter="{StaticResource StatusLine}">
                                            <Binding Path="IsRunning" />
                                            <Binding Path="Fps" />
                                        </MultiBinding>
```

with:

```xml
                                        <MultiBinding Converter="{StaticResource StatusLine}">
                                            <Binding Path="IsRunning" />
                                            <Binding Path="Fps" />
                                            <Binding Path="FrameWidth" />
                                            <Binding Path="FrameHeight" />
                                        </MultiBinding>
```

- [ ] **Step 8: Build and run the full suite**

Run:
```bash
~/.dotnet/dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj
~/.dotnet/dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj
```

Expected: `0 Warning(s)`, all tests pass.

- [ ] **Step 9: Commit**

```bash
git add src/CameraOnScreen.Core/ViewModels/MainViewModel.cs src/CameraOnScreen.App.Avalonia/ tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs
git commit -m "feat: show the negotiated capture resolution in the status line (#57)

The overlay pump already received width/height from every cos_get_frame call
and discarded them. It now reports them to the VM through one callback, which
Task 4 also uses as its liveness signal.

No CosStatus change — struct parity is untouched."
```

---

### Task 4: Disconnect watchdog

**Files:**
- Modify: `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs` (`CheckLiveness`, `CameraError`, `FrameReportingActive`, `PollStatusTick`, `Start`, hot-swap partial)
- Modify: `src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs` (`SyncOverlay`, overlay `Closed` handler)
- Modify: `src/CameraOnScreen.App.Avalonia/MainWindow.axaml` (show `CameraError`)
- Test: `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`

**Interfaces:**
- Consumes: `MainViewModel.OnFrameReceived(int, int, long)` and `_lastFrameMs` from Task 3; `_activeCameraId` from Task 1.
- Produces:
  - `MainViewModel.CheckLiveness(long nowMs)` — `public void`, called from `PollStatusTick`.
  - `MainViewModel.FrameReportingActive` — `public bool { get; set; }`, owned by the panel and true only while an overlay pump exists.
  - `MainViewModel.CameraError` — `string?` observable, sticky (not overwritten by the status poll).

- [ ] **Step 1: Write the failing tests**

Add to `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`:

```csharp
    // Drives the watchdog with an explicit clock. The first CheckLiveness call after a (re)start
    // anchors the window, mirroring what the 4 Hz status timer does in the app.
    private static MainViewModel BuildRunningWithWatchdog(out FakeShim shim, long anchorMs)
    {
        var vm = BuildWithCameras(out shim, "a");
        vm.SelectedCamera = vm.Cameras[0];
        vm.StartCommand.Execute(null);
        vm.FrameReportingActive = true;
        vm.CheckLiveness(anchorMs);
        return vm;
    }

    [Fact]
    public void Watchdog_stops_capture_when_frames_stop_arriving()
    {
        // Killing scrcpy does not make v4l2loopback error — it just goes quiet. select() times
        // out forever and the worker's break paths never fire, so only a timeout catches this.
        var vm = BuildRunningWithWatchdog(out _, anchorMs: 1000);
        vm.OnFrameReceived(1280, 720, nowMs: 1000);

        vm.CheckLiveness(2999);
        Assert.True(vm.IsRunning);      // 1999 ms of silence — not yet

        vm.CheckLiveness(3001);
        Assert.False(vm.IsRunning);     // 2001 ms — overlay closes via SyncOverlay
        Assert.Equal("Camera disconnected", vm.CameraError);
    }

    [Fact]
    public void Watchdog_stops_capture_when_no_first_frame_ever_arrives()
    {
        // The #55 symptom: device opens, negotiation fails, worker returns, nothing is surfaced.
        var vm = BuildRunningWithWatchdog(out _, anchorMs: 1000);

        vm.CheckLiveness(5999);
        Assert.True(vm.IsRunning);      // 4999 ms — Start can legitimately take seconds
                                        // while a Maxine effect initializes
        vm.CheckLiveness(6001);
        Assert.False(vm.IsRunning);
        Assert.Equal("No frames from camera", vm.CameraError);
    }

    [Fact]
    public void Watchdog_does_not_fire_while_frames_keep_arriving()
    {
        var vm = BuildRunningWithWatchdog(out _, anchorMs: 1000);
        for (long t = 1000; t <= 20_000; t += 500)
        {
            vm.OnFrameReceived(1280, 720, t);
            vm.CheckLiveness(t);
        }
        Assert.True(vm.IsRunning);
        Assert.Null(vm.CameraError);
    }

    [Fact]
    public void Watchdog_is_off_when_nothing_reports_frames()
    {
        // PollStatusTick is shared Core but only the Avalonia overlay reports frames. With the
        // WinUI panel unwired (spec §9), an always-on watchdog would read Windows' silence as a
        // dead camera and stop a healthy capture.
        var vm = BuildWithCameras(out _, "a");
        vm.SelectedCamera = vm.Cameras[0];
        vm.StartCommand.Execute(null);
        // FrameReportingActive left false
        vm.CheckLiveness(1000);
        vm.CheckLiveness(999_000);
        Assert.True(vm.IsRunning);
        Assert.Null(vm.CameraError);
    }

    [Fact]
    public void Hot_swap_resets_the_watchdog()
    {
        // A swap restarts capture on a new device, which gets its own grace period — otherwise
        // the old camera's last-frame stamp would condemn the new one.
        var vm = BuildWithCameras(out var shim, "a", "b");
        vm.SelectedCamera = vm.Cameras[0];
        vm.StartCommand.Execute(null);
        vm.FrameReportingActive = true;
        vm.CheckLiveness(1000);
        vm.OnFrameReceived(640, 480, nowMs: 1000);

        vm.SelectedCamera = vm.Cameras[1];   // swap at an unknown wall-clock instant
        vm.CheckLiveness(1500);              // anchors the new window here

        vm.CheckLiveness(3400);
        Assert.True(vm.IsRunning);           // 1900 ms into the 5000 ms no-first-frame window
        Assert.Equal(2, shim.StartCount);
    }
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `~/.dotnet/dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~Watchdog"`

Expected: FAIL to compile — `'MainViewModel' does not contain a definition for 'FrameReportingActive'`.

- [ ] **Step 3: Add the watchdog state**

In `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs`, add next to `_lastFrameMs`:

```csharp
    // Silence thresholds. The no-first-frame window is the longer of the two: Start can
    // legitimately take seconds while a Maxine effect loads its TensorRT engine.
    private const long DisconnectMs = 2000;
    private const long NoFirstFrameMs = 5000;

    // Anchors the no-first-frame window. Set on the first CheckLiveness after a (re)start rather
    // than at Start, so the VM never reads a clock itself and the tests stay deterministic.
    private long? _livenessAnchorMs;
```

Add the observable next to the others:

```csharp
    [ObservableProperty] private string? cameraError;   // sticky: OnStatus must not clear it
```

Add the plain property near `ShimRef` (line 26):

```csharp
    /// <summary>True only while something is pumping frames and reporting them via
    /// <see cref="OnFrameReceived"/> — i.e. while the Avalonia overlay window is open. The
    /// watchdog is off otherwise, so "nobody is reporting frames" means disabled, not dead.
    /// The WinUI panel never sets this (spec §9), so Windows behaviour is unchanged.</summary>
    public bool FrameReportingActive { get; set; }
```

- [ ] **Step 4: Implement `CheckLiveness` and reset points**

In the same file, add after `OnFrameReceived`:

```csharp
    /// <summary>Auto-stop when the selected camera goes silent. Deliberately a timeout rather
    /// than native error detection: when scrcpy dies, v4l2loopback does not report an error — it
    /// just goes quiet, select() times out forever, and the capture worker's break paths never
    /// fire. A timeout catches that, genuine ioctl failures, and the Windows equivalents with one
    /// implementation. A frozen stale frame silently baked into a recording is worse than the
    /// overlay visibly closing.</summary>
    public void CheckLiveness(long nowMs)
    {
        if (!IsRunning || !FrameReportingActive) { _livenessAnchorMs = null; return; }
        _livenessAnchorMs ??= nowMs;

        long since = nowMs - (_lastFrameMs ?? _livenessAnchorMs.Value);
        if (since < (_lastFrameMs is null ? NoFirstFrameMs : DisconnectMs)) return;

        CameraError = _lastFrameMs is null ? "No frames from camera" : "Camera disconnected";
        Stop();
    }

    // Clears the frame history so a (re)started camera gets its own grace period, instead of
    // being condemned by the previous camera's last-frame stamp.
    private void ResetLiveness()
    {
        _lastFrameMs = null;
        _livenessAnchorMs = null;
        CameraError = null;
    }
```

Replace `PollStatusTick` (line 47) — order matters, `PollStatus` raises `OnStatus` which writes `StatusError`, and `CameraError` is a separate sticky field precisely so the poll cannot wipe the watchdog's message:

```csharp
    // Driven by the UI-thread status timer each tick to refresh status (fps/gaze/error) and to
    // run the camera liveness watchdog.
    public void PollStatusTick()
    {
        _orchestrator.PollStatus();
        CheckLiveness(Environment.TickCount64);
    }
```

Add `ResetLiveness()` to `Start` and to the hot-swap partial:

```csharp
    [RelayCommand]
    private void Start()
    {
        _orchestrator.Start(BuildParams());
        _activeCameraId = SelectedCamera?.Id;
        ResetLiveness();
        IsRunning = true;
    }
```

```csharp
    partial void OnSelectedCameraChanged(CameraInfo? value)
    {
        if (!IsRunning || value is null || value.Value.Id == _activeCameraId) return;
        _orchestrator.Start(BuildParams());
        _activeCameraId = value.Value.Id;
        ResetLiveness();
    }
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `~/.dotnet/dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`

Expected: PASS (88 tests).

- [ ] **Step 6: Own `FrameReportingActive` from the panel**

In `src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs`, replace `SyncOverlay` (lines 57-78) in full:

```csharp
    private void SyncOverlay()
    {
        if (_services.Vm.IsRunning && _overlay is null)
        {
            var b = ClampToAScreen(_overlayBounds);
            var w = new Overlay.OverlayWindow(_services.Vm.ShimRef, b.X, b.Y, b.W, b.H, _services.Vm.Mirror,
                _services.Loaded.Overlay.TeleportModifiers,
                (fw, fh) => _services.Vm.OnFrameReceived(fw, fh, Environment.TickCount64));
            // Alt+F4 on the frameless overlay closes it directly: remember where it was and
            // let capture keep running; the next Stop/Start round-trip reopens it. Frame
            // reporting stops with it, so the watchdog must stand down or it would read the
            // closed pump's silence as a dead camera.
            w.Closed += (_, _) =>
            {
                if (!ReferenceEquals(_overlay, w)) return;
                _overlayBounds = BoundsOf(w);
                _overlay = null;
                _services.Vm.FrameReportingActive = false;
            };
            w.SetFrameInterp(_services.Vm.FrameInterpEnabled && _services.Vm.FrameInterpAvailable);
            _overlay = w;
            _services.Vm.FrameReportingActive = true;
            w.Show();
        }
        else if (!_services.Vm.IsRunning && _overlay is not null)
        {
            var w = _overlay;
            _overlay = null;
            _services.Vm.FrameReportingActive = false;
            _overlayBounds = BoundsOf(w);
            w.Close();
        }
    }
```

- [ ] **Step 7: Surface the error in the panel**

In `src/CameraOnScreen.App.Avalonia/MainWindow.axaml`, add directly after the closing `</StackPanel>` of the Start/Stop row (after line 66):

```xml
                            <TextBlock Text="{Binding CameraError}" TextWrapping="Wrap"
                                       Foreground="#E06C75"
                                       IsVisible="{Binding CameraError, Converter={x:Static ObjectConverters.IsNotNull}}" />
```

`ObjectConverters` lives in `Avalonia.Data.Converters`, which the default XAML
namespace already maps — no extra `xmlns` needed. If it fails to resolve
anyway, drop the `IsVisible` binding entirely: an empty `TextBlock` renders as
nothing, so visibility is cosmetic here (it only avoids a blank line of
padding).

- [ ] **Step 8: Build and run the full suite**

Run:
```bash
~/.dotnet/dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj
~/.dotnet/dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj
```

Expected: `0 Warning(s)`, all tests pass.

- [ ] **Step 9: Commit**

```bash
git add src/CameraOnScreen.Core/ViewModels/MainViewModel.cs src/CameraOnScreen.App.Avalonia/ tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs
git commit -m "feat: auto-stop and report when a camera goes silent (#57)

Killing scrcpy mid-capture left the overlay frozen on the last frame with
IsRunning still true. v4l2loopback does not error when its writer dies — it
goes quiet, so select() times out forever and the worker's break paths never
fire. Only a timeout catches it.

Two windows, one mechanism: 2s of silence after frames were flowing, or 5s
without a first frame ever arriving. The longer second window covers a Maxine
effect loading its TensorRT engine at Start.

Opt-in via FrameReportingActive, owned by the overlay's lifetime: the WinUI
panel does not report frames (spec §9), and the Avalonia overlay can be closed
with Alt+F4 while capture keeps running. In both cases silence means the pump
is gone, not the camera."
```

---

### Task 5: End-to-end verification and documentation

**Files:**
- Modify: `CLAUDE.md` (Linux section — the camera-selection behaviours a future session needs to know)

**Interfaces:**
- Consumes: everything from Tasks 1-4.
- Produces: nothing code-facing.

- [ ] **Step 1: Run the full manual gate**

Visual confirmation is an inherent human gate in this repo (`docs/superpowers/verification/`). Requires a phone with scrcpy, or ffmpeg feeding a loopback as a stand-in.

```bash
# Virtual camera stand-in, if no phone is to hand:
ffmpeg -re -f lavfi -i testsrc=size=2560x1440:rate=30 -pix_fmt yuv420p -f v4l2 /dev/video3
~/.dotnet/dotnet run --project src/CameraOnScreen.App.Avalonia
```

Check each, and record the result in the commit message:

1. **Hot-swap.** Start on the Brio. Select the virtual camera. Video switches; the overlay keeps its exact position, size, and mirror state; it does not flicker closed and reopen.
2. **Swap back.** Select the Brio again. Switches back; resolution readout changes with it.
3. **Late device.** Quit ffmpeg, restart the app, then start ffmpeg. Open the dropdown — the device appears without an app restart.
4. **Refresh during capture** (spec §4 risk, already checked in Task 2 Step 7 — confirm it still holds with the watchdog live, which would auto-stop on a 2 s stall).
5. **Resolution readout.** Status line reads `Running — 30 fps — 2560×1440` on the virtual camera and the Brio's actual negotiated size on the Brio.
6. **Disconnect.** With the virtual camera running, quit ffmpeg. Within ~2 s the overlay closes and the panel shows "Camera disconnected".
7. **Alt+F4 false positive.** Start capture, close the overlay with Alt+F4, wait 10 s. Capture must keep running and NO "Camera disconnected" may appear — this is the `FrameReportingActive` guard.

- [ ] **Step 2: Document the behaviours in CLAUDE.md**

In `CLAUDE.md`, in the Linux paragraph that covers the Avalonia panel (the "Phase 3 overlay" sentence), append:

```
**Camera selection (#57):** changing `SelectedCamera` while running hot-swaps —
`MainViewModel.OnSelectedCameraChanged` calls `Orchestrator.Start` again, which is a clean
restart because both `Capture::Start` implementations open with `StopLocked()`. `IsRunning`
deliberately does NOT flip (`SyncOverlay` keys off it alone), so the overlay keeps its
geometry; `_activeCameraId` suppresses a restart on a transient null selection or a re-select.
The camera list re-enumerates on ComboBox `DropDownOpened` (`RefreshCameras`, diff-by-Id so
the selection never churns) because a v4l2loopback node only announces `V4L2_CAP_VIDEO_CAPTURE`
while its writer is attached. The overlay pump reports each frame to
`Vm.OnFrameReceived(w, h, nowMs)`, feeding both the status-line resolution readout and a
liveness watchdog (`CheckLiveness`, run from `PollStatusTick`): 2 s of silence after frames
were flowing, or 5 s with no first frame, auto-stops with a sticky `CameraError`. The watchdog
is a Core-side TIMEOUT, not native error detection — a dead v4l2loopback writer produces no
error, just silence. It is gated on `FrameReportingActive`, which tracks the overlay window's
lifetime: the WinUI panel never sets it (#57 left Windows unwired), and Alt+F4 on the overlay
stops the pump while capture continues, so in both cases silence means the pump is gone rather
than the camera.
```

- [ ] **Step 3: Commit and open the PR**

```bash
git add CLAUDE.md
git commit -m "docs: record the camera-selection contracts in CLAUDE.md (#57)"
git push -u origin feat/57-camera-selection-ux
```

Then open the PR. The body must contain, in this order: `Closes #57.`; one
section per feature (hot-swap, live list, resolution readout, watchdog) stating
what was broken and what changed; the **result of each of the seven manual
checks from Step 1**, naming any that were skipped and why; and a line
confirming the WinUI panel was not touched (spec §9). State test counts and
warning counts as actual observed numbers, not expectations.

---

## Notes for the implementer

- **`SelectedCamera` may end up null** after a refresh drops the running camera. That is intentional: the hot-swap guard ignores null, and the watchdog stops capture ~2 s later with "Camera disconnected". Do not add a "re-select something" fallback — the spec explicitly rejected auto-falling-back to another camera as a mid-recording surprise.
- **Do not move the watchdog into the shim.** It is Core-side on purpose (spec §6); a native implementation would need a timer thread per backend and would still miss the silent-loopback case unless it were also a timeout.
- **Task order matters.** Task 4 consumes `OnFrameReceived` from Task 3 and `_activeCameraId` from Task 1.
