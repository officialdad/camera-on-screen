# QoL Bucket 2 — Overlay Mirror + Zoom Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a horizontal mirror toggle and a center-only zoom slider (1.0×–3.0×) to the webcam overlay, both applied live and persisted.

**Architecture:** Pure presentation-side change. Mirror and zoom compose into the single `Matrix3x2` already passed to the DirectComposition visual in `OverlayWindow.UpdateScale()`. Window edges clip zoom overflow = tighter center framing. The swap chain stays pinned to camera frame resolution (1:1 `CopyResource` unchanged). No shim, C-ABI, or `ShimParams` changes. State flows panel → `MainViewModel` props → host `OnVmPropertyChanged` → `OverlayWindow.SetMirror/SetZoom`, exactly like the existing `Locked`/`ClickThrough` props.

**Tech Stack:** C# .NET 8, WinUI 3, CommunityToolkit.Mvvm (source-generated `[ObservableProperty]`), System.Numerics `Matrix3x2`, Vortice DirectComposition, xUnit.

## Global Constraints

- Platform: Windows + NVIDIA RTX only; x64.
- Builds and tests must be **pristine — 0 warnings** (warnings are findings).
- Mirror/zoom are **presentation state, NOT shim params** — they must not flow through `BuildParams`/`SetParams`/`ShimParams`.
- Persistence saves only on `WM_EXITSIZEMOVE` / window close (existing path) — never per tick.
- `MainViewModel.ToAppConfig` builds a **fresh** `AppConfig`; any field not copied reverts to default — every persisted field must be copied.
- Core project has no WinUI/Win32 types; the overlay is reached only via the host's `OnVmPropertyChanged` routing.
- Zoom is clamped to **1.0–3.0**; default **1.0**. Mirror default **false**.
- Build the App with `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild`. Run Core tests with `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`.

---

### Task 1: Add `Zoom` to `OverlaySettings` config

**Files:**
- Modify: `src/CameraOnScreen.Core/Config/Models.cs:10-21` (`OverlaySettings` record)
- Test: `tests/CameraOnScreen.Core.Tests/Config/JsonSettingsStoreTests.cs`

**Interfaces:**
- Consumes: nothing (leaf config change).
- Produces: `OverlaySettings.Zoom` (`double`, default `1.0`). `OverlaySettings.Mirror` (`bool`) already exists.

- [ ] **Step 1: Write the failing test**

Add this test method to `JsonSettingsStoreTests` (class already exists; follow the existing save→load round-trip pattern used for `GreenScreenStrength`):

```csharp
[Fact]
public void Save_then_load_round_trips_overlay_mirror_and_zoom()
{
    var path = Path.Combine(Path.GetTempPath(), $"cos-cfg-{Guid.NewGuid():N}.json");
    try
    {
        var store = new JsonSettingsStore(path);
        store.Save(new AppConfig
        {
            Overlay = new OverlaySettings { Mirror = true, Zoom = 2.5 }
        });

        var loaded = store.Load();

        Assert.True(loaded.Overlay.Mirror);
        Assert.Equal(2.5, loaded.Overlay.Zoom);
    }
    finally
    {
        if (File.Exists(path)) File.Delete(path);
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~Save_then_load_round_trips_overlay_mirror_and_zoom"`
Expected: FAIL to compile — `OverlaySettings` has no `Zoom` member.

- [ ] **Step 3: Add the `Zoom` property**

In `src/CameraOnScreen.Core/Config/Models.cs`, add to `OverlaySettings` (next to the existing `Mirror`):

```csharp
public double Zoom { get; init; } = 1.0;
```

- [ ] **Step 4: Run test to verify it passes**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~Save_then_load_round_trips_overlay_mirror_and_zoom"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/CameraOnScreen.Core/Config/Models.cs tests/CameraOnScreen.Core.Tests/Config/JsonSettingsStoreTests.cs
git commit -m "feat(config): add OverlaySettings.Zoom (mirror already present)"
```

---

### Task 2: VM mirror/zoom props + load/save round-trip

**Files:**
- Modify: `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs` (props ~line 52-65; `LoadFrom` ~67-79; `ToAppConfig` ~84-99)
- Test: `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`

**Interfaces:**
- Consumes: `OverlaySettings.Mirror`, `OverlaySettings.Zoom` (Task 1).
- Produces: `MainViewModel.Mirror` (`bool`), `MainViewModel.Zoom` (`double`). These raise `PropertyChanged` via the MVVM source generator. They are NOT added to `BuildParams`/`ShimParams`.

- [ ] **Step 1: Write the failing tests**

Add to `MainViewModelTests`:

```csharp
[Fact]
public void LoadFrom_propagates_mirror_and_zoom()
{
    var vm = Build(GpuTier.Rtx, out _);
    var config = new AppConfig
    {
        Overlay = new OverlaySettings { Mirror = true, Zoom = 2.0 }
    };
    vm.LoadFrom(config);
    Assert.True(vm.Mirror);
    Assert.Equal(2.0, vm.Zoom);
}

[Fact]
public void ToAppConfig_captures_mirror_and_zoom()
{
    var vm = Build(GpuTier.Rtx, out _);
    vm.Mirror = true;
    vm.Zoom = 1.5;
    var cfg = vm.ToAppConfig(10, 20, 300, 400);
    Assert.True(cfg.Overlay.Mirror);
    Assert.Equal(1.5, cfg.Overlay.Zoom);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~_mirror_and_zoom"`
Expected: FAIL to compile — `MainViewModel` has no `Mirror`/`Zoom`.

- [ ] **Step 3: Add the observable properties**

In `MainViewModel.cs`, add alongside the other `[ObservableProperty]` fields (after `clickThrough`, ~line 62):

```csharp
[ObservableProperty] private bool mirror;
[ObservableProperty] private double zoom = 1.0;
```

- [ ] **Step 4: Load them in `LoadFrom`**

In `LoadFrom`, alongside `Locked`/`ClickThrough` (~line 74-75), add:

```csharp
Mirror = config.Overlay.Mirror;
Zoom = config.Overlay.Zoom;
```

- [ ] **Step 5: Persist them in `ToAppConfig`**

In `ToAppConfig`, extend the `OverlaySettings` initializer (~line 87-91) to:

```csharp
Overlay = new OverlaySettings
{
    X = x, Y = y, Width = w, Height = h,
    Locked = Locked, ClickThrough = ClickThrough,
    Mirror = Mirror, Zoom = Zoom
},
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~_mirror_and_zoom"`
Expected: PASS (both).

- [ ] **Step 7: Run the full Core suite (no regressions)**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
Expected: PASS, 0 warnings.

- [ ] **Step 8: Commit**

```bash
git add src/CameraOnScreen.Core/ViewModels/MainViewModel.cs tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs
git commit -m "feat(vm): mirror/zoom observable props with load/save round-trip"
```

---

### Task 3: `OverlayWindow` mirror/zoom transform

**Files:**
- Modify: `src/CameraOnScreen.App/Overlay/OverlayWindow.cs` (fields ~line 27-45; `UpdateScale` ~236-243; add setters)

**Interfaces:**
- Consumes: nothing managed-side; reads its own `_mirror`/`_zoom`.
- Produces: `OverlayWindow.SetMirror(bool)`, `OverlayWindow.SetZoom(double)` — called by the host (Task 4). Both re-run `UpdateScale` against the current client rect and commit, so changes apply live.

**Background (no test — native D3D/DComp has no unit harness here; this is a visual gate):** `System.Numerics` is already imported (line 1). `Matrix3x2.CreateScale(float scale, Vector2 center)` and `CreateScale(float x, float y, Vector2 center)` apply a scale about a center point. For row-vector matrices, `A * B` applies A then B, so `Fit * Mirror * Zoom` = fit the frame to the window, then flip about center, then zoom about center.

- [ ] **Step 1: Add the state fields**

In `OverlayWindow.cs`, after the interaction-state fields (after `_trackingMouse`, ~line 38), add:

```csharp
// Presentation transform state (Bucket 2). Mirror flips horizontally about the window centre;
// zoom (1.0–3.0) scales about the centre so the window edges crop the overflow = tighter framing.
// Both fold into the single DComp visual transform in UpdateScale — the swap chain is untouched.
private bool _mirror;
private double _zoom = 1.0;
```

- [ ] **Step 2: Fold mirror/zoom into `UpdateScale`**

Replace the body of `UpdateScale` (currently lines ~236-243) with:

```csharp
private void UpdateScale(int clientW, int clientH)
{
    if (_bufW <= 0 || _bufH <= 0 || clientW <= 0 || clientH <= 0) return;
    float sx = clientW / (float)_bufW;
    float sy = clientH / (float)_bufH;
    var center = new Vector2(clientW / 2f, clientH / 2f);
    // Fit frame-res content to the window, then flip and zoom about the window centre.
    var m = Matrix3x2.CreateScale(sx, sy);
    if (_mirror)
        m *= Matrix3x2.CreateScale(-1f, 1f, center);
    if (_zoom != 1.0)
        m *= Matrix3x2.CreateScale((float)_zoom, center);
    _visual.SetTransform(m);
    _dcomp.Commit();
}
```

- [ ] **Step 3: Add the live setters**

Add these public methods next to `SetLocked`/`SetClickThrough` (in the "Task 13 public API" region, ~line 137):

```csharp
/// <summary>Horizontal mirror (selfie view). Presentation-only; re-applies the visual transform live.</summary>
public void SetMirror(bool on)
{
    if (_disposed || _mirror == on) return;
    _mirror = on;
    GetClientRect(_hwnd, out RECT rc);
    UpdateScale(rc.right - rc.left, rc.bottom - rc.top);
}

/// <summary>Center zoom, clamped 1.0–3.0. Window edges crop the overflow = tighter framing. Live.</summary>
public void SetZoom(double zoom)
{
    zoom = Math.Clamp(zoom, 1.0, 3.0);
    if (_disposed || _zoom == zoom) return;
    _zoom = zoom;
    GetClientRect(_hwnd, out RECT rc);
    UpdateScale(rc.right - rc.left, rc.bottom - rc.top);
}
```

- [ ] **Step 4: Build the App (compile check — no managed test for native window)**

Run: `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild`
Expected: Build succeeded, 0 warnings.

- [ ] **Step 5: Commit**

```bash
git add src/CameraOnScreen.App/Overlay/OverlayWindow.cs
git commit -m "feat(overlay): mirror + center-zoom in the DComp visual transform"
```

---

### Task 4: Host wiring — route VM props to the overlay + initial apply

**Files:**
- Modify: `src/CameraOnScreen.App/MainWindow.xaml.cs` (initial-apply block ~line 39-40; `OnVmPropertyChanged` ~128-140)

**Interfaces:**
- Consumes: `MainViewModel.Mirror`/`Zoom` (Task 2); `OverlayWindow.SetMirror`/`SetZoom` (Task 3).
- Produces: nothing new — completes the live + startup application path.

- [ ] **Step 1: Apply the loaded mirror/zoom at startup**

In the `MainWindow` constructor, after the existing initial-apply lines (`_overlay.SetLocked(Vm.Locked);` / `_overlay.SetClickThrough(Vm.ClickThrough);`, ~line 39-40), add:

```csharp
_overlay.SetMirror(Vm.Mirror);
_overlay.SetZoom(Vm.Zoom);
```

(`Vm.LoadFrom` already ran inside `Services.BuildViewModel`, so `Vm.Mirror`/`Vm.Zoom` hold the persisted values here.)

- [ ] **Step 2: Route live changes in `OnVmPropertyChanged`**

In `OnVmPropertyChanged`, add two branches after the `ClickThrough` branch (~line 138-139):

```csharp
else if (e.PropertyName == nameof(MainViewModel.Mirror))
    _overlay.SetMirror(Vm.Mirror);
else if (e.PropertyName == nameof(MainViewModel.Zoom))
    _overlay.SetZoom(Vm.Zoom);
```

- [ ] **Step 3: Build the App**

Run: `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild`
Expected: Build succeeded, 0 warnings.

- [ ] **Step 4: Commit**

```bash
git add src/CameraOnScreen.App/MainWindow.xaml.cs
git commit -m "feat(app): wire mirror/zoom VM props to the overlay (live + startup)"
```

---

### Task 5: Panel UI — Mirror toggle + Zoom slider

**Files:**
- Modify: `src/CameraOnScreen.App/MainWindow.xaml` (the `Grid.Row="1"` StackPanel, lines 19-31)

**Interfaces:**
- Consumes: `MainViewModel.Mirror`/`Zoom` (Task 2). Two-way bindings drive the VM props, which Task 4 routes to the overlay.
- Produces: nothing.

- [ ] **Step 1: Add the controls**

In `MainWindow.xaml`, inside the `Grid.Row="1"` `StackPanel`, after the `Click-through` `ToggleSwitch` (line 30), add:

```xml
<ToggleSwitch Header="Mirror (selfie view)"
              IsOn="{x:Bind Vm.Mirror, Mode=TwoWay}"/>
<Slider Header="Zoom" Minimum="1" Maximum="3" StepFrequency="0.1"
        Value="{x:Bind Vm.Zoom, Mode=TwoWay}"/>
```

- [ ] **Step 2: Build the App**

Run: `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild`
Expected: Build succeeded, 0 warnings.

- [ ] **Step 3: Commit**

```bash
git add src/CameraOnScreen.App/MainWindow.xaml
git commit -m "feat(ui): mirror toggle + zoom slider in the control panel"
```

---

### Task 6: Full verification + human visual gate

**Files:** none (verification only).

- [ ] **Step 1: Full Core test suite**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
Expected: PASS, 0 warnings.

- [ ] **Step 2: Clean App build**

Run: `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild`
Expected: Build succeeded, 0 warnings.

- [ ] **Step 3: Human visual gate (run the app)**

Per CLAUDE.md the overlay cannot be GDI-screenshotted — confirm visually. Ensure the SDK shim is the deployed DLL (so the app runs normally), then:

```powershell
$env:COS_VFX_SDK_DIR = "C:\Users\opari\OneDrive\Desktop\claude-code\VideoFX"
src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.App.exe
```

Confirm, with the camera Started:
- Mirror toggle flips the image left↔right immediately.
- Zoom slider (1.0→3.0) scales in toward the center; content stays centered and the window crops the overflow (no stretching, no black bars).
- Both survive an app restart (persisted): set mirror on + zoom ~1.5, close, relaunch — overlay returns mirrored and zoomed.

- [ ] **Step 4: Checkpoint**

Stop here and report results to the user for the Bucket 2 review checkpoint before starting Bucket 3.

---

## Self-Review

**Spec coverage (Bucket 2 section of the QoL spec):**
- Mirror toggle → Tasks 2 (prop), 3 (transform), 4 (wire), 5 (UI). ✓
- Center-only zoom 1.0–3.0 → Tasks 1 (config), 2 (prop), 3 (clamp + transform), 5 (slider). ✓
- Presentation-side only, no shim/ABI/ShimParams change → no task touches the shim, `BuildParams`, or `ShimParams`. ✓
- Live application → Task 3 setters re-run `UpdateScale`; Task 4 routes `PropertyChanged`. ✓
- Persisted on EXITSIZEMOVE/close, `ToAppConfig` copies fields → Tasks 1, 2. ✓
- Swap chain pinned / `CopyResource` 1:1 unchanged → Task 3 touches only `UpdateScale`/setters, never `PresentFrame`/`ResizeBuffers`. ✓
- Mirror already in config, only wired now → Task 1 note + Task 2 LoadFrom. ✓
- Core unit tests for config + VM round-trip → Tasks 1, 2. Visual result is a human gate → Task 6. ✓

**Placeholder scan:** No TBD/TODO; every code step shows complete code. ✓

**Type consistency:** `SetMirror(bool)`/`SetZoom(double)` defined in Task 3, consumed in Task 4. `Mirror`/`Zoom` props defined in Task 2, consumed in Tasks 4 (host), 5 (XAML). `OverlaySettings.Zoom` defined Task 1, consumed Task 2. `Matrix3x2`/`Vector2` from `System.Numerics` (already imported). Names consistent throughout. ✓
