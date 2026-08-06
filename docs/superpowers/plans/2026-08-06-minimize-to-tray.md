# Minimize to System Tray Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Closing or minimizing the Linux (Avalonia) control panel sends it to the system tray with capture and the overlay still running, enabled by default.

**Architecture:** One new `AppConfig.MinimizeToTray` flag (default `true`) mirrored onto `MainViewModel`; a new `TrayController` in the Avalonia app that owns an `Avalonia.Controls.TrayIcon` plus its native menu and routes Start/Stop to the view model's existing commands; `MainWindow` cancels its own close (and swallows minimize) to `Hide()` instead, with the app switched to `ShutdownMode.OnExplicitShutdown` so a hidden panel does not end the process.

**Tech Stack:** C# .NET 8, Avalonia 11.2.1 (`TrayIcon` / `NativeMenu`, StatusNotifierItem on Linux), CommunityToolkit.Mvvm source-generated properties and commands, xUnit.

**Spec:** `docs/superpowers/specs/2026-08-06-minimize-to-tray-design.md`

## Global Constraints

- Builds and tests must be **pristine — 0 warnings** (CI enforces `TreatWarningsAsErrors`). A warning is a failure.
- **No new NuGet dependencies.** `TrayIcon` ships in the already-referenced `Avalonia` 11.2.1 package.
- **Linux / Avalonia panel only.** Do not touch `src/CameraOnScreen.App` (WinUI) — Windows is parked on issue #38.
- **Do not touch `native/shim` or `src/CameraOnScreen.App.Avalonia/Overlay/`.** This feature is panel-side only.
- `CameraOnScreen.Core` stays free of Avalonia/WinUI/Win32 types — the config flag and the view-model property are plain .NET.
- Assembly name for `avares://` URIs is `CameraOnScreen.App.Avalonia` (the csproj sets no `AssemblyName`).
- Build commands, run from the repo root:
  - Core tests: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
  - Avalonia app: `dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj`
  - Run it: `dotnet run --project src/CameraOnScreen.App.Avalonia`
- Commit after each task; use the repo's `feat:` / `docs:` / `test:` prefixes.

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `src/CameraOnScreen.Core/Config/Models.cs` | Add `AppConfig.MinimizeToTray` (default `true`) + equality | 1 |
| `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs` | Observable `MinimizeToTray`, read in `LoadFrom`, written in `ToAppConfig` | 1 |
| `tests/CameraOnScreen.Core.Tests/Config/ModelsTests.cs` | Config default / round-trip / equality tests | 1 |
| `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs` | VM round-trip test | 1 |
| `src/CameraOnScreen.App.Avalonia/Tray/TrayController.cs` | **New.** Owns the `TrayIcon`, its `NativeMenu`, and the `IsRunning` → header/tooltip sync | 2 |
| `src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj` | Ship `cos.png` as an `AvaloniaResource` at `Assets/cos.png` | 2 |
| `src/CameraOnScreen.App.Avalonia/App.axaml.cs` | `ShutdownMode.OnExplicitShutdown` | 2 |
| `src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs` | Hide-on-close, hide-on-minimize, show, quit, tray lifetime | 2 |
| `src/CameraOnScreen.App.Avalonia/MainWindow.axaml` | Window icon + "Minimize to tray" toggle | 2 |
| `README.md`, `CLAUDE.md` | User-facing behavior + the no-tray recovery knob | 3 |
| `docs/superpowers/verification/2026-08-06-minimize-to-tray.md` | **New.** Human verification gate | 3 |

---

### Task 1: Config and view-model plumbing

**Files:**
- Modify: `src/CameraOnScreen.Core/Config/Models.cs` (the `AppConfig` record — property list, `Equals`, `GetHashCode`)
- Modify: `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs` (observable property block ~line 195, `LoadFrom`, `ToAppConfig`)
- Test: `tests/CameraOnScreen.Core.Tests/Config/ModelsTests.cs`
- Test: `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `bool CameraOnScreen.Core.Config.AppConfig.MinimizeToTray { get; init; }` — default `true`.
  - `bool CameraOnScreen.Core.ViewModels.MainViewModel.MinimizeToTray { get; set; }` — source-generated from a `minimizeToTray` field, default `true`, raises `PropertyChanged`. Task 2 reads this property and binds to it.

- [ ] **Step 1: Write the failing config tests**

Append to `tests/CameraOnScreen.Core.Tests/Config/ModelsTests.cs`, inside the existing `ModelsTests` class:

```csharp
    [Fact]
    public void MinimizeToTray_defaults_on_and_round_trips_when_disabled()
    {
        Assert.True(new AppConfig().MinimizeToTray);

        var json = ConfigSerializer.Serialize(new AppConfig { MinimizeToTray = false });
        Assert.False(ConfigSerializer.Deserialize(json).MinimizeToTray);
    }

    [Fact]
    public void MinimizeToTray_absent_from_json_stays_enabled()
    {
        // A config.json written before the feature existed has no key at all: System.Text.Json
        // leaves the property at its initializer, so upgrading users get the tray by default.
        var back = ConfigSerializer.Deserialize("{ \"CameraId\": \"cam\" }");
        Assert.True(back.MinimizeToTray);
    }

    [Fact]
    public void MinimizeToTray_participates_in_equality()
    {
        // AppConfig.Equals is hand-written — a field left out of it silently makes the
        // "did anything change?" comparison lie.
        Assert.NotEqual(new AppConfig(), new AppConfig { MinimizeToTray = false });
    }
```

- [ ] **Step 2: Run the config tests to verify they fail**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~MinimizeToTray"`

Expected: build FAILS with `error CS0117: 'AppConfig' does not contain a definition for 'MinimizeToTray'`.

- [ ] **Step 3: Add the config property**

In `src/CameraOnScreen.Core/Config/Models.cs`, inside `public sealed record AppConfig`, directly after the `PanelHeight` property:

```csharp
    // Close/minimize send the control panel to the system tray instead of quitting /
    // taskbar-minimizing, leaving capture and the overlay running. Linux (Avalonia) panel only;
    // the WinUI panel ignores it (#38). Default on — absent from an older config.json, the
    // initializer wins, so existing users get it on upgrade.
    public bool MinimizeToTray { get; init; } = true;
```

In the same record, extend the hand-written equality members — add the `MinimizeToTray` clause to `Equals`:

```csharp
    public bool Equals(AppConfig? other) => other != null
        && CameraId == other.CameraId
        && Overlay == other.Overlay
        && Effects == other.Effects
        && PanelWidth == other.PanelWidth
        && PanelHeight == other.PanelHeight
        && MinimizeToTray == other.MinimizeToTray
        && Hotkeys.SequenceEqual(other.Hotkeys);
```

…and to `GetHashCode`:

```csharp
    public override int GetHashCode()
    {
        var hk = Hotkeys.Aggregate(0, (h, b) => HashCode.Combine(h, b));
        return HashCode.Combine(CameraId, Overlay, Effects, hk, PanelWidth, PanelHeight, MinimizeToTray);
    }
```

- [ ] **Step 4: Run the config tests to verify they pass**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~MinimizeToTray"`

Expected: PASS, 3 tests.

- [ ] **Step 5: Write the failing view-model test**

Append to `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`, inside the existing `MainViewModelTests` class (it already has the private `Build` helper):

```csharp
    [Fact]
    public void MinimizeToTray_round_trips_through_the_view_model()
    {
        var vm = Build(GpuTier.Rtx, out _);
        Assert.True(vm.MinimizeToTray);   // default on, before any config load

        vm.LoadFrom(new AppConfig { MinimizeToTray = false });
        Assert.False(vm.MinimizeToTray);
        Assert.False(vm.ToAppConfig(0, 0, 320, 240).MinimizeToTray);

        vm.LoadFrom(new AppConfig { MinimizeToTray = true });
        Assert.True(vm.ToAppConfig(0, 0, 320, 240).MinimizeToTray);
    }
```

- [ ] **Step 6: Run the view-model test to verify it fails**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~MinimizeToTray_round_trips"`

Expected: build FAILS with `error CS1061: 'MainViewModel' does not contain a definition for 'MinimizeToTray'`.

- [ ] **Step 7: Add the view-model property and wire it into load/save**

In `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs`, in the observable-property block (next to `private bool mirror;`):

```csharp
    // Panel-window behavior only — deliberately has no On…Changed partial: it drives no native
    // params, so it must not call ApplyLiveParams or ResetLivenessIfRunning.
    [ObservableProperty] private bool minimizeToTray = true;
```

In `LoadFrom`, next to the `Mirror = config.Overlay.Mirror;` line:

```csharp
        MinimizeToTray = config.MinimizeToTray;
```

In `ToAppConfig`, as a top-level initializer beside `CameraId`:

```csharp
        MinimizeToTray = MinimizeToTray,
```

- [ ] **Step 8: Run the full Core suite**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`

Expected: PASS — 96 tests (92 baseline + 4 new), 0 failures, 0 warnings.

- [ ] **Step 9: Commit**

```bash
git add src/CameraOnScreen.Core/Config/Models.cs \
        src/CameraOnScreen.Core/ViewModels/MainViewModel.cs \
        tests/CameraOnScreen.Core.Tests/Config/ModelsTests.cs \
        tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs
git commit -m "feat: add MinimizeToTray config flag, on by default (#tray)"
```

---

### Task 2: Tray icon and panel hide/restore/quit lifecycle

**Files:**
- Create: `src/CameraOnScreen.App.Avalonia/Tray/TrayController.cs`
- Modify: `src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj`
- Modify: `src/CameraOnScreen.App.Avalonia/App.axaml.cs`
- Modify: `src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs`
- Modify: `src/CameraOnScreen.App.Avalonia/MainWindow.axaml`
- Test: none automatable — Avalonia window state, native tray menus, and StatusNotifierItem need a real desktop session. The gate is the build plus the human checklist in Task 3.

**Interfaces:**
- Consumes: `MainViewModel.MinimizeToTray` (bool, gettable/settable/bindable), `MainViewModel.IsRunning` (bool, raises `PropertyChanged`), `MainViewModel.StartCommand` / `MainViewModel.StopCommand` (source-generated `IRelayCommand`, invoked as `Execute(null)`) — all from Task 1 and existing code.
- Produces: `CameraOnScreen.App.Avalonia.Tray.TrayController`, constructed as `new TrayController(MainViewModel vm, Action showPanel, Action quit)` and implementing `IDisposable`. Nothing after Task 2 consumes it.

- [ ] **Step 1: Ship the app icon as an Avalonia resource**

`cos.png` (344 KB) already sits at the repo root and is referenced by `README.md` and the Windows installer. Link it into the Avalonia app rather than copying the binary — add this `ItemGroup` to `src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj`, right before the `PackageReference` group:

```xml
  <!-- Repo-root app icon, surfaced to Avalonia as avares://CameraOnScreen.App.Avalonia/Assets/cos.png
       (window icon + system-tray icon). Linked, not copied — one binary in the repo. -->
  <ItemGroup>
    <AvaloniaResource Include="$(MSBuildThisFileDirectory)..\..\cos.png" Link="Assets/cos.png" />
  </ItemGroup>
```

- [ ] **Step 2: Verify the resource URI actually resolves**

Run: `dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj`

Then confirm the resource landed under the linked path:

Run: `strings src/CameraOnScreen.App.Avalonia/bin/Debug/net8.0/CameraOnScreen.App.Avalonia.dll | grep -i 'assets/cos.png'`

Expected: a match containing `Assets/cos.png`.

If there is **no match**, the `Link` metadata did not drive the `avares://` path. Fall back to a real copy — `cp cos.png src/CameraOnScreen.App.Avalonia/Assets/cos.png`, `git add` it, and replace the item with `<AvaloniaResource Include="Assets/cos.png" />`. Re-run this step. Do not proceed until the string is present; a wrong `avares://` URI throws at window construction.

- [ ] **Step 3: Write the tray controller**

Create `src/CameraOnScreen.App.Avalonia/Tray/TrayController.cs`:

```csharp
using System.ComponentModel;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Platform;
using CameraOnScreen.Core.ViewModels;

namespace CameraOnScreen.App.Avalonia.Tray;

/// <summary>
/// Owns the system-tray icon and its native menu. On Linux this is backed by the freedesktop
/// StatusNotifierItem D-Bus interface, which Avalonia speaks for us.
///
/// ponytail: no availability probe. If the session runs no StatusNotifierItem host the icon
/// silently never appears, and a hidden panel is unreachable — the escape hatches are the
/// "Minimize to tray" toggle and "MinimizeToTray": false in config.json (both documented in the
/// README). Upgrade path if that bites: query D-Bus for org.kde.StatusNotifierWatcher at startup
/// and force MinimizeToTray off when it is missing.
/// </summary>
public sealed class TrayController : IDisposable
{
    private readonly MainViewModel _vm;
    private readonly TrayIcon _icon;
    private readonly NativeMenuItem _runItem;

    public TrayController(MainViewModel vm, Action showPanel, Action quit)
    {
        _vm = vm;

        // Start/Stop routes to the panel's own commands — no second copy of the start logic.
        _runItem = new NativeMenuItem();
        _runItem.Click += (_, _) =>
        {
            if (_vm.IsRunning) _vm.StopCommand.Execute(null);
            else _vm.StartCommand.Execute(null);
        };

        var showItem = new NativeMenuItem("Show Panel");
        showItem.Click += (_, _) => showPanel();

        var quitItem = new NativeMenuItem("Quit");
        quitItem.Click += (_, _) => quit();

        var menu = new NativeMenu();
        menu.Add(showItem);
        menu.Add(_runItem);
        menu.Add(new NativeMenuItemSeparator());
        menu.Add(quitItem);

        _icon = new TrayIcon
        {
            Icon = new WindowIcon(AssetLoader.Open(
                new Uri("avares://CameraOnScreen.App.Avalonia/Assets/cos.png"))),
            Menu = menu,
            IsVisible = true,
        };
        _icon.Clicked += (_, _) => showPanel();

        // The attached property is Avalonia's registration hook — a TrayIcon that is not in
        // Application's TrayIcons collection is never shown.
        TrayIcon.SetIcons(Application.Current!, new TrayIcons { _icon });

        _vm.PropertyChanged += OnVmPropertyChanged;
        SyncRunState();
    }

    private void OnVmPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(MainViewModel.IsRunning)) SyncRunState();
    }

    private void SyncRunState()
    {
        _runItem.Header = _vm.IsRunning ? "Stop Camera" : "Start Camera";
        _icon.ToolTipText = _vm.IsRunning ? "Camera-on-Screen — running" : "Camera-on-Screen — stopped";
    }

    public void Dispose()
    {
        _vm.PropertyChanged -= OnVmPropertyChanged;
        TrayIcon.SetIcons(Application.Current!, new TrayIcons());
        _icon.Dispose();
    }
}
```

- [ ] **Step 4: Switch the app to explicit shutdown**

In `src/CameraOnScreen.App.Avalonia/App.axaml.cs`, replace the body of the `if` in `OnFrameworkInitializationCompleted`:

```csharp
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            // Load-bearing for minimize-to-tray: under the default OnLastWindowClose, a panel
            // hidden to the tray with no overlay open leaves zero open windows and Avalonia
            // exits — the tray icon would vanish seconds after the user hid the panel. The only
            // exit path now is MainWindow's Closed handler calling Shutdown().
            desktop.ShutdownMode = ShutdownMode.OnExplicitShutdown;
            desktop.MainWindow = new MainWindow();
        }
```

- [ ] **Step 5: Wire the window — hide on close, hide on minimize, show, quit**

In `src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs`:

Add to the usings:

```csharp
using Avalonia.Controls.ApplicationLifetimes;
```

Add two fields beside `_overlay`:

```csharp
    private readonly Tray.TrayController _tray;
    // Set only by the tray's Quit item: tells Closing to let the close through instead of
    // cancelling it into a Hide().
    private bool _quitting;
```

In the constructor, immediately after `_overlayBounds = (o.X, o.Y, o.Width, o.Height);`:

```csharp
        _tray = new Tray.TrayController(_services.Vm, ShowPanel, Quit);

        // Minimize means "get out of the way", same as close. Bounce the state back to Normal
        // before hiding so a later restore comes back as a normal window, not a minimized one.
        PropertyChanged += (_, e) =>
        {
            if (e.Property == WindowStateProperty
                && WindowState == WindowState.Minimized
                && _services.Vm.MinimizeToTray)
            {
                WindowState = WindowState.Normal;
                Hide();
            }
        };
```

Replace the existing `Closing` handler with:

```csharp
        Closing += (_, e) =>
        {
            CaptureOverlayBounds();
            var b = _overlayBounds;
            // Save on the way to the tray too, not just on a real exit: an app killed while
            // sitting in the tray then loses nothing it would not already have lost.
            _services.Store.Save(_services.Vm.ToAppConfig(b.X, b.Y, b.W, b.H));
            if (_services.Vm.MinimizeToTray && !_quitting)
            {
                e.Cancel = true;
                Hide();
            }
        };
```

Replace the existing `Closed` handler with:

```csharp
        Closed += (_, _) =>
        {
            _statusTimer.Stop();
            _tray.Dispose();
            _overlay?.Close();
            // Joins the native capture worker (cos_shutdown) — without this the global
            // std::thread is destroyed joinable at process exit -> std::terminate.
            _services.Vm.Dispose();
            // Closed only fires when Closing did not cancel, so reaching here always means a
            // real exit — whether via the tray's Quit or via X with MinimizeToTray off. Under
            // ShutdownMode.OnExplicitShutdown nothing else ends the process.
            if (Application.Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime d)
                d.Shutdown();
        };
```

Add both methods next to `SyncOverlay`:

```csharp
    private void ShowPanel()
    {
        Show();
        WindowState = WindowState.Normal;
        Activate();
    }

    private void Quit()
    {
        _quitting = true;
        Close();   // Closing saves, Closed tears down and shuts the app down.
    }
```

- [ ] **Step 6: Add the window icon and the opt-out toggle**

In `src/CameraOnScreen.App.Avalonia/MainWindow.axaml`, add to the root `<Window …>` element's attributes:

```xml
        Icon="avares://CameraOnScreen.App.Avalonia/Assets/cos.png"
```

In the CAMERA card, immediately after the `Lock exposure (steady FPS)` `ToggleSwitch`:

```xml
                            <ToggleSwitch Content="Minimize to tray"
                                          IsChecked="{Binding MinimizeToTray, Mode=TwoWay}" />
```

- [ ] **Step 7: Build clean**

Run: `dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj -t:Rebuild`

Expected: `Build succeeded.` with **0 Warning(s), 0 Error(s)**. A warning here is a failure — fix it before continuing.

- [ ] **Step 8: Re-run the Core suite (nothing should have moved)**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`

Expected: PASS, 96 tests, 0 failures.

- [ ] **Step 9: Smoke-run the app**

Run: `dotnet run --project src/CameraOnScreen.App.Avalonia`

Confirm three things, then close it from the tray's `Quit`:
1. The panel opens with no exception on stdout (a bad `avares://` URI throws at window construction — if it does, go back to Step 2).
2. A Camera-on-Screen icon is present in the system tray.
3. Clicking the window's X hides the panel and the process is still alive (`pgrep -f CameraOnScreen.App.Avalonia` prints a PID); a tray left-click brings the panel back.

Full behavior verification is the Task 3 checklist — this step only proves the app runs.

- [ ] **Step 10: Commit**

```bash
git add src/CameraOnScreen.App.Avalonia/
git commit -m "feat: minimize the Linux control panel to the system tray"
```

---

### Task 3: Docs and the human verification gate

**Files:**
- Create: `docs/superpowers/verification/2026-08-06-minimize-to-tray.md`
- Modify: `README.md` (the "Using it" list, and the Linux config-path paragraph around line 45)
- Modify: `CLAUDE.md` (the Linux build/behavior paragraph in the "Linux build (Phase 2+, issue #27)" section)

**Interfaces:**
- Consumes: the behavior shipped in Tasks 1 and 2.
- Produces: nothing consumed by code.

- [ ] **Step 1: Write the verification document**

Create `docs/superpowers/verification/2026-08-06-minimize-to-tray.md`:

```markdown
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

Result: ____ (date / tester / notes)
```

- [ ] **Step 2: Update the README**

In `README.md`, in the "Using it" bullet list, after the `**Resize it**` bullet:

```markdown
- **Minimize to tray** (Linux) - closing or minimizing the control panel sends it
  to the system tray while the overlay keeps running. Left-click the tray icon to
  bring the panel back; the tray menu can start/stop the camera and quit. Turn it
  off with the **Minimize to tray** toggle in the control panel.
```

Then, in the Linux paragraph that names the config path (around line 45), append this sentence:

```markdown
If your desktop has no system-tray host, the panel can hide with no way back —
set `"MinimizeToTray": false` in that file to restore the plain close-to-quit
behavior.
```

- [ ] **Step 3: Update CLAUDE.md**

In `CLAUDE.md`, at the end of the long "Linux build (Phase 2+, issue #27…)" paragraph — right after the `Camera selection (#57)` material and before the fenced build commands — add:

```markdown
**Minimize to tray:** `AppConfig.MinimizeToTray` (default **true**, Avalonia panel only —
the WinUI panel ignores it, #38) makes the panel's close *and* minimize buttons `Hide()` it
behind an `Avalonia.Controls.TrayIcon` (`Tray/TrayController.cs`, StatusNotifierItem on Linux)
while capture and the overlay keep running. `App.axaml.cs` therefore sets
`ShutdownMode.OnExplicitShutdown` — under the default `OnLastWindowClose` a hidden panel with
no overlay open leaves zero windows and Avalonia exits. The single exit path is `MainWindow`'s
`Closed` handler calling `desktop.Shutdown()`; `Closed` only fires when `Closing` did not
cancel, so it covers both the tray's Quit and X-with-the-toggle-off. `Closing` saves config on
the way to the tray as well as on exit, so hiding still persists overlay geometry. The 250 ms
`_statusTimer` is a `DispatcherTimer` and keeps ticking while hidden, so the liveness watchdog
stays armed; `FrameReportingActive` tracks the overlay, never the panel. There is deliberately
no StatusNotifierItem-availability probe — on a desktop with no tray host the icon silently
never appears, and the recovery is the panel toggle or `"MinimizeToTray": false` in config.json.
```

- [ ] **Step 4: Run the human verification checklist**

Work through every row of `docs/superpowers/verification/2026-08-06-minimize-to-tray.md` on the dev box and fill in the `Result:` line. Report any row that fails instead of marking the task done.

- [ ] **Step 5: Commit**

```bash
git add README.md CLAUDE.md docs/superpowers/verification/2026-08-06-minimize-to-tray.md
git commit -m "docs: document minimize-to-tray and its verification gate"
```

---

## Self-Review

Checked against `docs/superpowers/specs/2026-08-06-minimize-to-tray-design.md`:

| Spec section | Covered by |
|---|---|
| Scope (Avalonia only, overlay untouched, no new deps) | Global Constraints |
| Behavior table (close, minimize, left-click, menu items, quit) | Task 2 Steps 3, 5; verified Task 3 Step 4 |
| Configuration (`AppConfig.MinimizeToTray`, VM mirror, no `On…Changed` partial) | Task 1 Steps 3, 7 |
| `TrayController` (icon asset, tooltip text, menu, `IsRunning` sync) | Task 2 Steps 1–3 |
| `App.axaml.cs` explicit shutdown | Task 2 Step 4 |
| `MainWindow` hide/minimize/show/quit | Task 2 Step 5 |
| Opt-out control in the panel | Task 2 Step 6 (a `ToggleSwitch`, matching the panel's existing controls, rather than the spec's word "checkbox") |
| Interaction with polling / `FrameReportingActive` / save timing | Comments in Task 2 Step 5; CLAUDE.md text in Task 3 Step 3 |
| No-tray failure mode + `ponytail:` comment | Task 2 Step 3 doc comment; README recovery note in Task 3 Step 2 |
| Core unit tests (default, round-trip, equality, VM round-trip) | Task 1 Steps 1, 5 |
| Human verification gate (7 spec items) | Task 3 Step 1 (10 rows — the spec's 7 plus persistence and a tooltip check) |
| Out of scope (WinUI, autostart, extra menu items, SNI probe) | Nothing in the plan touches them |

No gaps. No placeholders. Names used across tasks are consistent: `MinimizeToTray` (config +
VM), `TrayController(vm, showPanel, quit)`, `ShowPanel()`, `Quit()`, `_quitting`, `_tray`.
