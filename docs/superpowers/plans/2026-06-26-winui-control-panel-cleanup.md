# WinUI Control-Panel Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Streamline the WinUI control panel — grouped cards, custom title bar, a gesture-help InfoBar, drop Lock/Click-through/Zoom, and size the window to its content.

**Architecture:** UI-layer only. Core VM loses three observable props (Lock/ClickThrough/Zoom); `MainWindow.xaml` is regrouped into two `Border` cards under section headers with a custom title bar; the capture/shim/overlay pipeline is untouched. Removed config fields and enum members are kept (unused) to avoid config-version churn.

**Tech Stack:** C# .NET 8, WinUI 3 (Windows App SDK 1.8), CommunityToolkit.Mvvm, xUnit.

## Global Constraints

- **Pristine build:** 0 warnings (CI enforces `/warnaserror` + `TreatWarningsAsErrors`). No `#pragma warning disable`.
- **No new NuGet dependency** — native `Border`/`TextBlock`/`InfoBar` only, not the SettingsControls toolkit.
- **System theme only** — no brand-accent brushes; bind to `ThemeResource` keys.
- **Core has no WinUI/Win32 types** — VM changes stay pure .NET.
- **Keep, do not remove:** `OverlaySettings.{Locked,ClickThrough,Zoom}` fields; `HotkeyAction.{ToggleLock,ToggleClickThrough}` enum members; `OverlayWindow.{SetLocked,SetClickThrough,SetZoom}` methods. Mark deliberate keeps with a `// ponytail:` comment.
- **Maxine attribution footer stays** (SDK License Supplement §3.1) — pinned, always visible.
- **WinUI API verification:** before coding the title bar and min-size (Task 4/5), consult Context7 MCP (`resolve-library-id` → `query-docs`) for the current Windows App SDK 1.8 `AppWindowTitleBar` / `OverlappedPresenter` surface, per the global Context7 rule. Code below is the expected shape; confirm signatures before relying on them.
- **App-project verification is a CI/human gate:** the App can't build on this host (needs the shim DLL + RTX). Tasks 3–5 are verified by the self-hosted RTX CI build (0 warnings) plus a visual human check; only Tasks 1–2 (Core) run locally via `dotnet test`.

---

### Task 1: Remove Lock/ClickThrough/Zoom from the view-model

**Files:**
- Modify: `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs`
- Test: `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`

**Interfaces:**
- Consumes: `AppConfig`, `OverlaySettings` (unchanged — fields stay).
- Produces: `MainViewModel` with no `Locked`/`ClickThrough`/`Zoom` properties; `Mirror` kept. `ToAppConfig` still returns a fully-populated `AppConfig` (removed fields default: `Locked=false`, `ClickThrough=false`, `Zoom=1.0`).

- [ ] **Step 1: Update the VM tests to the new contract**

In `MainViewModelTests.cs`, **replace** the test `LoadFrom_propagates_locked_and_clickthrough` (currently ~lines 149-160) and `LoadFrom_propagates_mirror_and_zoom` (~196-207) with a single mirror-only test, and **edit** `ToAppConfig_captures_geometry_and_effects` (~162-176) and `ToAppConfig_captures_mirror_and_zoom` (~209-218) to drop Lock/Zoom.

Replace the two `LoadFrom_*` tests with:

```csharp
    [Fact]
    public void LoadFrom_propagates_mirror()
    {
        var vm = Build(GpuTier.Rtx, out _);
        var config = new AppConfig
        {
            Overlay = new OverlaySettings { Mirror = true }
        };
        vm.LoadFrom(config);
        Assert.True(vm.Mirror);
    }
```

Replace `ToAppConfig_captures_geometry_and_effects` body's Lock lines — set it to:

```csharp
    [Fact]
    public void ToAppConfig_captures_geometry_and_effects()
    {
        var vm = Build(GpuTier.Rtx, out _);
        vm.GreenScreenEnabled = true;
        var cfg = vm.ToAppConfig(10, 20, 300, 400);
        Assert.Equal(10, cfg.Overlay.X);
        Assert.Equal(20, cfg.Overlay.Y);
        Assert.Equal(300, cfg.Overlay.Width);
        Assert.Equal(400, cfg.Overlay.Height);
        Assert.True(cfg.Effects.GreenScreenEnabled);
        Assert.Null(cfg.CameraId);
    }
```

Replace `ToAppConfig_captures_mirror_and_zoom` with mirror-only:

```csharp
    [Fact]
    public void ToAppConfig_captures_mirror()
    {
        var vm = Build(GpuTier.Rtx, out _);
        vm.Mirror = true;
        var cfg = vm.ToAppConfig(10, 20, 300, 400);
        Assert.True(cfg.Overlay.Mirror);
    }
```

- [ ] **Step 2: Run the tests to verify they fail to compile**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
Expected: BUILD FAILURE — tests still reference `vm.Locked`/`vm.Zoom` nowhere now, but the VM still *defines* them; compile actually still succeeds and tests PASS. (This is a deletion refactor — the suite is the regression guard, not a red-first test.) Proceed to Step 3.

- [ ] **Step 3: Remove the three observable properties**

In `MainViewModel.cs`, delete these three `[ObservableProperty]` lines (~76-79), keeping `mirror`:

```csharp
    [ObservableProperty] private bool locked;          // DELETE
    [ObservableProperty] private bool clickThrough;    // DELETE
    [ObservableProperty] private double zoom = 1.0;    // DELETE
```

Keep:
```csharp
    [ObservableProperty] private bool mirror;
```

- [ ] **Step 4: Remove the load/save lines**

In `LoadFrom` (~106-109), delete the `Locked`, `ClickThrough`, and `Zoom` lines; keep `Mirror`:

```csharp
        // DELETE: Locked = config.Overlay.Locked;
        // DELETE: ClickThrough = config.Overlay.ClickThrough;
        Mirror = config.Overlay.Mirror;
        // DELETE: Zoom = config.Overlay.Zoom;
```

In `ToAppConfig`'s `OverlaySettings` initializer (~121-126), drop `Locked`/`ClickThrough`/`Zoom` so they take their record defaults:

```csharp
        Overlay = new OverlaySettings
        {
            X = x, Y = y, Width = w, Height = h,
            // ponytail: Locked/ClickThrough/Zoom intentionally omitted — fields kept on the record
            // (default false/false/1.0) so config stays schema-stable; the overlay is always
            // interactive and unzoomed now.
            Mirror = Mirror
        },
```

Also update the stale comment above `ToAppConfig` (~116-117) that says "Locked/ClickThrough are the existing observable props" — replace with: `// Mirror is the kept observable prop; Lock/ClickThrough/Zoom were removed (overlay is always interactive).`

- [ ] **Step 5: Run the tests to verify green**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
Expected: PASS — 58 tests, 0 failures (was 59; net -1 after merging two load tests into one).

- [ ] **Step 6: Commit**

```bash
git add src/CameraOnScreen.Core/ViewModels/MainViewModel.cs tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs
git commit -m "refactor(vm): drop Lock/ClickThrough/Zoom from view-model"
```

---

### Task 2: Drop the dead default hotkeys

**Files:**
- Modify: `src/CameraOnScreen.Core/Config/Models.cs:53-59`
- Test: `tests/CameraOnScreen.Core.Tests/Hotkeys/HotkeyValidatorTests.cs` (no change expected — verify)

**Interfaces:**
- Consumes: nothing new.
- Produces: `AppConfig.DefaultHotkeys()` returns 2 bindings (`ToggleOverlayVisible`, `ToggleRunning`). The `HotkeyAction.ToggleLock`/`ToggleClickThrough` enum members remain (still referenced by `HotkeyValidatorTests`).

- [ ] **Step 1: Remove the two dead default bindings**

In `Models.cs`, `DefaultHotkeys()` — delete the `ToggleLock` and `ToggleClickThrough` entries (~55-56), keeping the other two:

```csharp
    // VK codes: F10=0x79, F11=0x7A. (ToggleLock/ToggleClickThrough default bindings removed —
    // ponytail: enum members kept for config compatibility, but no UI/behavior toggles them now.)
    public static IReadOnlyList<HotkeyBinding> DefaultHotkeys() => Array.AsReadOnly(new[]
    {
        new HotkeyBinding { Action = HotkeyAction.ToggleOverlayVisible,Modifiers = HotkeyModifiers.Control | HotkeyModifiers.Alt, VirtualKey = 0x79 },
        new HotkeyBinding { Action = HotkeyAction.ToggleRunning,       Modifiers = HotkeyModifiers.Control | HotkeyModifiers.Alt, VirtualKey = 0x7A },
    });
```

- [ ] **Step 2: Run the Core tests**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
Expected: PASS, 0 failures. `HotkeyValidatorTests.DefaultHotkeys_has_no_conflict` still holds (two bindings, distinct keys); the conflict test still compiles (`ToggleLock` enum member intact).

- [ ] **Step 3: Commit**

```bash
git add src/CameraOnScreen.Core/Config/Models.cs
git commit -m "refactor(config): drop dead Lock/ClickThrough default hotkeys"
```

---

### Task 3: Strip removed wiring from the window code-behind

**Files:**
- Modify: `src/CameraOnScreen.App/MainWindow.xaml.cs`

**Interfaces:**
- Consumes: `MainViewModel` without `Locked`/`ClickThrough`/`Zoom` (Task 1).
- Produces: a code-behind that references only `Mirror` among the removed props; the overlay is left at its always-interactive defaults (`_locked=false`, `_clickThrough=false`, `_zoom=1.0` — no setter calls).

> Not locally buildable (App needs shim + RTX). Verified at CI build + visual gate.

- [ ] **Step 1: Remove the SetLocked/SetClickThrough/SetZoom ctor calls**

In the ctor (~57-61), delete the `SetLocked`, `SetClickThrough`, and `SetZoom` lines; keep `SetMirror`:

```csharp
        // Apply the initial state loaded from config to the overlay.
        _overlay.SetMirror(Vm.Mirror);
        // ponytail: overlay stays always-interactive (no SetLocked/SetClickThrough) and unzoomed
        // (no SetZoom) — Lock/ClickThrough/Zoom were removed from the panel.
```

- [ ] **Step 2: Remove the hotkey switch cases for Lock/ClickThrough**

In `OnHotkeyAction` (~173-181), delete the `ToggleLock` and `ToggleClickThrough` cases (they referenced the removed `Vm.Locked`/`Vm.ClickThrough`), keeping the other two:

```csharp
            switch (action)
            {
                case HotkeyAction.ToggleOverlayVisible: _overlay.ToggleVisible(); break;
                case HotkeyAction.ToggleRunning:
                    if (Vm.IsRunning) Vm.StopCommand?.Execute(null); else Vm.StartCommand?.Execute(null);
                    break;
                // ToggleLock/ToggleClickThrough: no-ops now (overlay always interactive); enum kept.
            }
```

- [ ] **Step 3: Remove the OnVmPropertyChanged branches for Locked/ClickThrough/Zoom**

In `OnVmPropertyChanged` (~287-294), delete the three `else if` branches for `Locked`, `ClickThrough`, and `Zoom`; keep the `Mirror` branch:

```csharp
        else if (e.PropertyName == nameof(MainViewModel.Mirror))
            _overlay.SetMirror(Vm.Mirror);
        // Locked/ClickThrough/Zoom branches removed — those props no longer exist.
```

- [ ] **Step 4: Commit**

```bash
git add src/CameraOnScreen.App/MainWindow.xaml.cs
git commit -m "refactor(app): remove Lock/ClickThrough/Zoom overlay wiring"
```

---

### Task 4: Custom title bar + dynamic sizing in code-behind

**Files:**
- Modify: `src/CameraOnScreen.App/MainWindow.xaml.cs`

**Interfaces:**
- Consumes: an `x:Name="AppTitleBar"` element added in Task 5's XAML, and `x:Name="RootGrid"` (the window root).
- Produces: `ExtendsContentIntoTitleBar=true` + `SetTitleBar(AppTitleBar)`; window sized to content on first layout instead of the hardcoded 400×720.

> **Before coding:** Context7 `query-docs` for "Windows App SDK 1.8 ExtendsContentIntoTitleBar SetTitleBar AppWindow Resize OverlappedPresenter minimum size". Confirm `SetTitleBar(UIElement)` and whether `OverlappedPresenter.PreferredMinimumWidth/Height` exist in 1.8. The code below is the expected shape.

- [ ] **Step 1: Replace `RightSizePanel` with size-to-content**

Delete the `PanelWidthDip`/`PanelHeightDip` consts (~15) and rewrite `RightSizePanel` (~119-127) to measure the laid-out content and resize to it. Call it from the existing ctor call site (`RightSizePanel();` ~74) but defer to first layout via `RootGrid.Loaded`:

```csharp
    // Size the window to its content (DPI-scaled) instead of a magic number, so every control is
    // visible at open. ScrollViewer is the fallback if the user shrinks the window.
    private void RightSizePanel()
    {
        RootGrid.Loaded += (_, _) =>
        {
            RootGrid.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
            var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
            double scale = GetDpiForWindow(hwnd) / 96.0;
            var id = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(hwnd);
            var appWindow = Microsoft.UI.Windowing.AppWindow.GetFromWindowId(id);
            int w = (int)(RootGrid.DesiredSize.Width * scale);
            int h = (int)(RootGrid.DesiredSize.Height * scale);
            appWindow.Resize(new Windows.Graphics.SizeInt32(w, h));
            // ponytail: best-effort min-size lock if the presenter supports it; else ScrollViewer
            // covers shrink. Guarded so a missing API in 1.8 doesn't break the build.
            if (appWindow.Presenter is Microsoft.UI.Windowing.OverlappedPresenter p)
            {
                p.PreferredMinimumWidth = w;
                p.PreferredMinimumHeight = h;
            }
        };
    }
```

> If Context7 confirms `PreferredMinimumWidth/Height` do **not** exist in 1.8, delete the `if (appWindow.Presenter ...)` block (size-to-content alone satisfies the requirement; ScrollViewer is the shrink fallback). Do not add a WM_GETMINMAXINFO subclass — out of scope.

- [ ] **Step 2: Wire the custom title bar in the ctor**

After `InitializeComponent();` (~73, before `RightSizePanel();`), extend into the title bar and set the drag region:

```csharp
        InitializeComponent();
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        RightSizePanel();
```

- [ ] **Step 3: Commit**

```bash
git add src/CameraOnScreen.App/MainWindow.xaml.cs
git commit -m "feat(app): custom title bar + size-to-content window"
```

---

### Task 5: Regroup the panel XAML

**Files:**
- Modify: `src/CameraOnScreen.App/MainWindow.xaml`

**Interfaces:**
- Consumes: `Vm` without `Zoom` (the Zoom `Slider` is deleted); the static helpers `QualityEnabled`, `ExposureSliderEnabled` (unchanged); `StatusLine`, `MaxineAttribution`, `NotAvailableVisibility`, `EyeContactNotAvailableVisibility` (unchanged).
- Produces: a window with `x:Name="RootGrid"`, an `x:Name="AppTitleBar"` title bar row, two grouped cards, and a help `InfoBar`.

> Not locally buildable. Verified at CI build + visual gate. Keep every binding path byte-identical to today's (only grouping/containers change) so no code-behind helper goes missing.

- [ ] **Step 1: Rewrite `MainWindow.xaml`**

Replace the whole file with the grouped layout. Title-bar row (row 0), scrollable body (row 1), pinned attribution (row 2):

```xml
<Window
    x:Class="CameraOnScreen.App.MainWindow"
    xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
    xmlns:muxc="using:Microsoft.UI.Xaml.Controls"
    xmlns:local="using:CameraOnScreen.App">
    <Grid x:Name="RootGrid">
        <Grid.RowDefinitions>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="*"/>
            <RowDefinition Height="Auto"/>
        </Grid.RowDefinitions>

        <!-- Custom title bar (drag region set via SetTitleBar in code-behind). -->
        <Grid x:Name="AppTitleBar" Grid.Row="0" Height="36" ColumnSpacing="8" Padding="12,0,0,0">
            <Grid.ColumnDefinitions>
                <ColumnDefinition Width="Auto"/>
                <ColumnDefinition Width="*"/>
            </Grid.ColumnDefinitions>
            <Image Grid.Column="0" Source="ms-appx:///Assets/cos.ico" Width="16" Height="16"
                   VerticalAlignment="Center"/>
            <TextBlock Grid.Column="1" Text="Camera on Screen" VerticalAlignment="Center"
                       Style="{ThemeResource CaptionTextBlockStyle}"/>
        </Grid>

        <!-- Controls scroll; the license-required footer (row 2) stays pinned outside. -->
        <ScrollViewer Grid.Row="1" Padding="16">
            <StackPanel Spacing="16">

                <muxc:InfoBar Severity="Informational" IsOpen="True" IsClosable="False"
                              Title="Overlay tips"
                              Message="Scroll over the overlay to resize it · drag the + handle to move it."/>

                <!-- CAMERA -->
                <StackPanel Spacing="8">
                    <TextBlock Text="Camera" Style="{ThemeResource BodyStrongTextBlockStyle}"/>
                    <Border Background="{ThemeResource CardBackgroundFillColorDefaultBrush}"
                            BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}"
                            BorderThickness="1" CornerRadius="8" Padding="12">
                        <StackPanel Spacing="12">
                            <ComboBox Header="Camera"
                                      ItemsSource="{x:Bind Vm.Cameras}"
                                      SelectedItem="{x:Bind Vm.SelectedCamera, Mode=TwoWay}"
                                      DisplayMemberPath="Name" HorizontalAlignment="Stretch"/>
                            <StackPanel Orientation="Horizontal" Spacing="8">
                                <Button Content="Start" Command="{x:Bind Vm.StartCommand}"
                                        AutomationProperties.Name="Start capture"/>
                                <Button Content="Stop" Command="{x:Bind Vm.StopCommand}"
                                        AutomationProperties.Name="Stop capture"/>
                                <TextBlock Text="{x:Bind StatusLine, Mode=OneWay}"
                                           VerticalAlignment="Center"
                                           AutomationProperties.LiveSetting="Polite"/>
                            </StackPanel>
                            <ToggleSwitch Header="Mirror (selfie view)"
                                          IsOn="{x:Bind Vm.Mirror, Mode=TwoWay}"/>
                            <ToggleSwitch Header="Lock exposure (steady FPS)"
                                          IsEnabled="{x:Bind Vm.ExposureSupported, Mode=OneWay}"
                                          IsOn="{x:Bind Vm.ExposureLock, Mode=TwoWay}"/>
                            <Slider Header="Exposure" Minimum="0" Maximum="1" StepFrequency="0.05"
                                    IsEnabled="{x:Bind local:MainWindow.ExposureSliderEnabled(Vm.ExposureSupported, Vm.ExposureLock), Mode=OneWay}"
                                    Value="{x:Bind Vm.ExposureValue, Mode=TwoWay}"/>
                        </StackPanel>
                    </Border>
                </StackPanel>

                <!-- AI EFFECTS -->
                <StackPanel Spacing="8">
                    <TextBlock Text="AI Effects" Style="{ThemeResource BodyStrongTextBlockStyle}"/>
                    <Border Background="{ThemeResource CardBackgroundFillColorDefaultBrush}"
                            BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}"
                            BorderThickness="1" CornerRadius="8" Padding="12">
                        <StackPanel Spacing="12">
                            <ToggleSwitch Header="AI Green Screen" IsEnabled="{x:Bind Vm.EffectsAvailable, Mode=OneWay}"
                                          IsOn="{x:Bind Vm.GreenScreenEnabled, Mode=TwoWay}"/>
                            <Slider Header="Green-screen Expand" Minimum="0" Maximum="1" StepFrequency="0.05"
                                    IsEnabled="{x:Bind Vm.GreenScreenEnabled, Mode=OneWay}"
                                    Value="{x:Bind Vm.GreenScreenExpand, Mode=TwoWay}"/>
                            <Slider Header="Green-screen Feather" Minimum="0" Maximum="1" StepFrequency="0.05"
                                    IsEnabled="{x:Bind Vm.GreenScreenEnabled, Mode=OneWay}"
                                    Value="{x:Bind Vm.GreenScreenFeather, Mode=TwoWay}"/>
                            <ToggleSwitch Header="Eye Contact" IsEnabled="{x:Bind Vm.EyeContactAvailable, Mode=OneWay}"
                                          IsOn="{x:Bind Vm.EyeContactEnabled, Mode=TwoWay}"/>
                            <TextBlock Text="{x:Bind Vm.EyeContactDetail, Mode=OneWay}"
                                       Visibility="{x:Bind EyeContactNotAvailableVisibility, Mode=OneWay}"
                                       TextWrapping="Wrap"
                                       Foreground="{ThemeResource SystemFillColorCautionBrush}"/>
                            <ComboBox Header="AI Sharpness"
                                      IsEnabled="{x:Bind Vm.SuperResAvailable, Mode=OneWay}"
                                      SelectedIndex="{x:Bind Vm.SuperResModeIndex, Mode=TwoWay}">
                                <ComboBoxItem Content="Off"/>
                                <ComboBoxItem Content="Denoise"/>
                                <ComboBoxItem Content="Deblur"/>
                            </ComboBox>
                            <ComboBox Header="Quality"
                                      IsEnabled="{x:Bind local:MainWindow.QualityEnabled(Vm.SuperResAvailable, Vm.SuperResModeIndex), Mode=OneWay}"
                                      SelectedIndex="{x:Bind Vm.SuperResQualityIndex, Mode=TwoWay}">
                                <ComboBoxItem Content="Low"/>
                                <ComboBoxItem Content="Medium"/>
                                <ComboBoxItem Content="High"/>
                                <ComboBoxItem Content="Ultra"/>
                            </ComboBox>
                            <TextBlock Text="{x:Bind Vm.CapabilityDetail, Mode=OneWay}"
                                       Visibility="{x:Bind NotAvailableVisibility, Mode=OneWay}"
                                       TextWrapping="Wrap"
                                       Foreground="{ThemeResource SystemFillColorCautionBrush}"/>
                        </StackPanel>
                    </Border>
                </StackPanel>
            </StackPanel>
        </ScrollViewer>

        <!-- Required NVIDIA Maxine attribution (SDK License Supplement §3.1) — always visible. -->
        <TextBlock Grid.Row="2" Margin="16,8,16,12"
                   Text="{x:Bind MaxineAttribution}"
                   Style="{ThemeResource CaptionTextBlockStyle}"
                   Foreground="{ThemeResource TextFillColorSecondaryBrush}"/>
    </Grid>
</Window>
```

- [ ] **Step 2: Verify the title-bar icon asset path**

Confirm `cos.ico` is reachable at `ms-appx:///Assets/cos.ico`. The csproj sets `<ApplicationIcon>..\..\cos.ico</ApplicationIcon>` (the exe icon) — that is NOT an `ms-appx` Content asset. Check whether an `Assets/` folder with the icon exists in the App project:

Run: `ls src/CameraOnScreen.App/Assets/ 2>/dev/null; grep -n "cos.ico\|Assets" src/CameraOnScreen.App/CameraOnScreen.App.csproj`

If no `ms-appx`-published icon exists, add the icon as Content in the csproj:

```xml
  <ItemGroup>
    <Content Include="..\..\cos.ico"><Link>Assets\cos.ico</Link><CopyToOutputDirectory>PreserveNewest</CopyToOutputDirectory></Content>
  </ItemGroup>
```

…and keep the `ms-appx:///Assets/cos.ico` source. If publishing the icon proves fragid for the unpackaged app, fall back to dropping the `<Image>` and showing the title text only (the title bar still works) — note this in the commit.

- [ ] **Step 3: Commit**

```bash
git add src/CameraOnScreen.App/MainWindow.xaml src/CameraOnScreen.App/CameraOnScreen.App.csproj
git commit -m "feat(app): grouped control panel + gesture-help InfoBar"
```

---

### Task 6: Verify

- [ ] **Step 1: Core tests pass locally**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
Expected: PASS, 0 failures (~58 tests).

- [ ] **Step 2: CI build gate (self-hosted RTX)**

Open the PR; CI builds the shim SDK config → export-verify → App build (`/warnaserror`) → Core tests. Expected: green, 0 warnings. This is the only place the App project compiles.

- [ ] **Step 3: Visual human gate**

On the RTX host, run the App and confirm:
- Custom title bar shows the icon + "Camera on Screen"; min/max/close work; the bar is draggable.
- Two cards: **Camera** (camera combo, Start/Stop + status, Mirror, Lock exposure, Exposure) and **AI Effects** (green screen + sliders, eye contact + note, sharpness + quality + note).
- The **InfoBar** at top states the scroll-to-resize / drag-the-+-handle gestures.
- No Lock overlay, Click-through, or Zoom controls anywhere.
- Window opens sized to fit all controls (no empty space / no clipping); shrinking it scrolls.
- Overlay is draggable and wheel-resizable (always interactive).

---

## Self-Review

- **Spec coverage:** custom title bar (T4/T5) ✓; grouped cards (T5) ✓; InfoBar help (T5) ✓; remove Lock/ClickThrough/Zoom (T1/T2/T3) ✓; Exposure→Camera, Mirror→Camera (T5) ✓; dynamic sizing (T4) ✓; system theme / no dep (Global Constraints + T5 ThemeResource) ✓; kept fields/enums/setters (T1/T2/T3 ponytail comments) ✓; attribution footer preserved (T5) ✓.
- **Placeholder scan:** none — every code step shows full code; the one uncertainty (min-size API) has an explicit fallback.
- **Type consistency:** `RootGrid`/`AppTitleBar` names match between T4 (code-behind) and T5 (XAML); `Mirror` kept consistently; removed props removed everywhere they were referenced (VM, code-behind branches, hotkey cases, XAML Zoom slider).
- **Note:** Step 2 of Task 1 is honestly labeled — this is a deletion refactor guarded by the existing suite, not a red-green-first test.
