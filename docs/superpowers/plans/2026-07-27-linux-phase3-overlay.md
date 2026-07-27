# Linux Phase 3 — Overlay + Present Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Linux transparent always-on-top overlay window presenting live shim frames, with drag, wheel-resize, live mirror, and geometry persistence — issue #31, Spike B winner design (a).

**Architecture:** A new `OverlayWindow` (Avalonia, code-only, no XAML) evolves the proven `OverlaySpikeWindow`: frameless Topmost transparent window blitting shim BGRA frames into a premultiplied `WriteableBitmap` on its own 33 ms `DispatcherTimer` pump. It is a **separate window** from the control panel (mirrors the Windows `OverlayWindow` split). The panel (`MainWindow.axaml.cs`) opens it on `IsRunning` true, closes it on false, pushes `Mirror` live, and saves its geometry to config on exit. Drag = Avalonia `BeginMoveDrag` (no MPO-plane problem on Linux — the window is a normal composited ARGB surface, toolkit hit-testing works). Wheel-resize reuses the already-tested `Core.Overlay.OverlaySizer`.

**Tech Stack:** Avalonia 11.2.1 (X11/XWayland), .NET 8, existing `PInvokeShim`/`libCameraOnScreen.Shim.so`. No new dependencies.

## Global Constraints

- Builds must be pristine: `/p:TreatWarningsAsErrors=true`, 0 warnings (CI enforces).
- `dotnet`/`cmake` are user-local on this box: `export DOTNET_ROOT="$HOME/.dotnet" PATH="$HOME/.dotnet:$PATH"` before any dotnet command. GUI runs need `DISPLAY=:0` and `XAUTHORITY` (see memory `linux-dev-box-toolchain`).
- Never modify `src/CameraOnScreen.App` (WinUI) on this box — not compile-checkable here; Windows overlay stays untouched.
- The shim is owned by the panel's composition (`Services.Build()`); the overlay only **reads** frames via `INativeShim.TryGetFrame(byte[], out int, out int)`. Never a second `cos_init`/`cos_shutdown` from the overlay.
- Status is polled, never pushed. Effect params push live only via the VM (unchanged).
- Premultiplied alpha: `WriteableBitmap` stays `AlphaFormat.Premul` — passthrough frames are opaque today; Phase 4 green-screen mattes land with no present-path change.
- No new Core logic in this phase → no new unit tests; the deliverable gates are 0-warning builds, the 90 existing Core tests, and manual run checks (UI-headless testing would need a new package — YAGNI).
- `Core.Overlay.Rect` collides with `Avalonia.Rect` — alias it (`using CoreRect = CameraOnScreen.Core.Overlay.Rect;`).
- DPI: Avalonia `Position` is physical pixels, `Width`/`Height` logical units. This box runs scale 1.0; treat them as equal (ponytail ceiling — revisit if a HiDPI report lands).

---

### Task 1: `OverlayWindow` (replaces the spike)

**Files:**
- Create: `src/CameraOnScreen.App.Avalonia/Overlay/OverlayWindow.cs`
- Delete: `src/CameraOnScreen.App.Avalonia/OverlaySpikeWindow.cs`
- Modify: `src/CameraOnScreen.App.Avalonia/App.axaml.cs` (remove the `--overlay-spike` switch)

**Interfaces:**
- Consumes: `INativeShim.TryGetFrame(byte[] buffer, out int width, out int height)` (Core); `OverlaySizer.Resize(CoreRect current, int notches, CoreRect workArea)` (Core, already unit-tested).
- Produces: `OverlayWindow(INativeShim shim, double x, double y, double w, double h, bool mirror)` ctor and `void SetMirror(bool mirror)` — Task 2 relies on both, plus inherited `Position` (`PixelPoint`) / `Width` / `Height` for geometry capture.

- [ ] **Step 1: Create `Overlay/OverlayWindow.cs`**

```csharp
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;
using CameraOnScreen.Core.Native;
using System.Runtime.InteropServices;
using CoreRect = CameraOnScreen.Core.Overlay.Rect;

namespace CameraOnScreen.App.Avalonia.Overlay;

// Linux overlay (Phase 3, #31): Spike B winner (a) — Avalonia transparent Topmost window
// presenting shim BGRA frames via a premultiplied WriteableBitmap. A separate window from
// the control panel, mirroring the Windows OverlayWindow split. The panel owns the shim;
// this window only reads frames. Drag = BeginMoveDrag (normal composited ARGB surface —
// the Windows MPO-plane hit-test failure does not apply). Wheel = OverlaySizer resize.
public sealed class OverlayWindow : Window
{
    private const int PumpMs = 33; // camera rate; Phase 4 FRUC drops this to 16 (60 Hz)

    private readonly INativeShim _shim;
    private readonly Image _image = new() { Stretch = Stretch.Uniform };
    private readonly byte[] _buffer = new byte[1920 * 1080 * 4];
    private readonly DispatcherTimer _pump;
    private WriteableBitmap? _bitmap;

    public OverlayWindow(INativeShim shim, double x, double y, double w, double h, bool mirror)
    {
        _shim = shim;
        SystemDecorations = SystemDecorations.None;
        Topmost = true;
        ShowInTaskbar = false;
        TransparencyLevelHint = new[] { WindowTransparencyLevel.Transparent };
        Background = Brushes.Transparent;
        Position = new PixelPoint((int)x, (int)y);
        Width = w;
        Height = h;
        Content = _image;
        SetMirror(mirror);

        _pump = new DispatcherTimer(TimeSpan.FromMilliseconds(PumpMs),
            DispatcherPriority.Render, (_, _) => Present());
        _pump.Start();
        Closed += (_, _) => _pump.Stop();
    }

    public void SetMirror(bool mirror) =>
        _image.RenderTransform = mirror ? new ScaleTransform(-1, 1) : null;

    private void Present()
    {
        if (!_shim.TryGetFrame(_buffer, out int w, out int h) || w <= 0) return;

        if (_bitmap is null || _bitmap.PixelSize.Width != w || _bitmap.PixelSize.Height != h)
        {
            _bitmap = new WriteableBitmap(new PixelSize(w, h), new Vector(96, 96),
                PixelFormat.Bgra8888, AlphaFormat.Premul);
            _image.Source = _bitmap;
        }

        using (var fb = _bitmap.Lock())
        {
            if (fb.RowBytes == w * 4)
                Marshal.Copy(_buffer, 0, fb.Address, w * h * 4);
            else
                for (int y = 0; y < h; y++)
                    Marshal.Copy(_buffer, y * w * 4, fb.Address + y * fb.RowBytes, w * 4);
        }
        _image.InvalidateVisual();
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        if (e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
            BeginMoveDrag(e);
    }

    protected override void OnPointerWheelChanged(PointerWheelEventArgs e)
    {
        base.OnPointerWheelChanged(e);
        int notches = (int)Math.Round(e.Delta.Y);
        if (notches == 0) return;
        var wa = Screens.ScreenFromWindow(this)?.WorkingArea
                 ?? Screens.Primary?.WorkingArea
                 ?? new PixelRect(0, 0, 1920, 1080);
        var next = CameraOnScreen.Core.Overlay.OverlaySizer.Resize(
            new CoreRect(Position.X, Position.Y, (int)Width, (int)Height), notches,
            new CoreRect(wa.X, wa.Y, wa.Width, wa.Height));
        Position = new PixelPoint(next.X, next.Y);
        Width = next.W;
        Height = next.H;
        e.Handled = true;
    }
}
```

- [ ] **Step 2: Delete the spike and its launch switch**

Delete `src/CameraOnScreen.App.Avalonia/OverlaySpikeWindow.cs`. In `App.axaml.cs`, restore the plain panel launch:

```csharp
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.MainWindow = new MainWindow();
        }
```

- [ ] **Step 3: Build, 0 warnings**

Run: `dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj /p:TreatWarningsAsErrors=true --nologo`
Expected: `0 Error(s)`, 0 warnings. (The overlay is not reachable yet — Task 2 wires it; build-only gate here.)

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat(avalonia): OverlayWindow — Linux present path from Spike B winner (#31)"
```

---

### Task 2: Panel wiring — open/close with capture, live mirror, geometry persistence

**Files:**
- Modify: `src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs`

**Interfaces:**
- Consumes: `OverlayWindow` ctor + `SetMirror` (Task 1); `MainViewModel.IsRunning` / `.Mirror` / `.ShimRef` (observable, raise `PropertyChanged` by those names); `MainViewModel.ToAppConfig(double x, double y, double w, double h)`; `Services.AppServices` record (`Vm`, `Store`, `Loaded`).
- Produces: nothing consumed later — terminal wiring.

- [ ] **Step 1: Rewrite `MainWindow.axaml.cs`**

```csharp
using Avalonia;
using Avalonia.Controls;
using Avalonia.Threading;
using CameraOnScreen.App.Avalonia.Composition;
using CameraOnScreen.Core.ViewModels;

namespace CameraOnScreen.App.Avalonia;

public partial class MainWindow : Window
{
    private readonly Services.AppServices _services;
    private readonly DispatcherTimer _statusTimer;
    private Overlay.OverlayWindow? _overlay;
    // Last-known overlay geometry: seeded from config, refreshed every time the overlay
    // closes, written back to config on panel exit.
    private (double X, double Y, double W, double H) _overlayBounds;

    public MainWindow()
    {
        InitializeComponent();
        _services = Services.Build();
        DataContext = _services.Vm;

        var o = _services.Loaded.Overlay;
        _overlayBounds = (o.X, o.Y, o.Width, o.Height);

        // Status is polled, never pushed (repo contract). The overlay runs its own 33 ms
        // frame pump; this panel-side timer only refreshes fps/error/running at 4 Hz.
        _statusTimer = new DispatcherTimer(TimeSpan.FromMilliseconds(250),
            DispatcherPriority.Background, (_, _) => _services.Vm.PollStatusTick());
        _statusTimer.Start();

        _services.Vm.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(MainViewModel.IsRunning)) SyncOverlay();
            else if (e.PropertyName == nameof(MainViewModel.Mirror)) _overlay?.SetMirror(_services.Vm.Mirror);
        };

        Closing += (_, _) =>
        {
            CaptureOverlayBounds();
            var b = _overlayBounds;
            _services.Store.Save(_services.Vm.ToAppConfig(b.X, b.Y, b.W, b.H));
        };
        Closed += (_, _) =>
        {
            _statusTimer.Stop();
            _overlay?.Close();
            // Joins the native capture worker (cos_shutdown) — without this the global
            // std::thread is destroyed joinable at process exit -> std::terminate.
            _services.Vm.Dispose();
        };
    }

    private void SyncOverlay()
    {
        if (_services.Vm.IsRunning && _overlay is null)
        {
            var b = ClampToAScreen(_overlayBounds);
            var w = new Overlay.OverlayWindow(_services.Vm.ShimRef, b.X, b.Y, b.W, b.H, _services.Vm.Mirror);
            // Alt+F4 on the frameless overlay closes it directly: remember where it was and
            // let capture keep running; the next Stop/Start round-trip reopens it.
            w.Closed += (_, _) => { if (ReferenceEquals(_overlay, w)) { _overlayBounds = BoundsOf(w); _overlay = null; } };
            _overlay = w;
            w.Show();
        }
        else if (!_services.Vm.IsRunning && _overlay is not null)
        {
            var w = _overlay;
            _overlay = null;
            _overlayBounds = BoundsOf(w);
            w.Close();
        }
    }

    private void CaptureOverlayBounds()
    {
        if (_overlay is not null) _overlayBounds = BoundsOf(_overlay);
    }

    private static (double, double, double, double) BoundsOf(Overlay.OverlayWindow w)
        => (w.Position.X, w.Position.Y, w.Width, w.Height);

    // A stale config can point at a monitor that no longer exists; falling back to the
    // OverlaySettings defaults keeps the overlay reachable (Windows twin: ResolveStartupBounds).
    private (double X, double Y, double W, double H) ClampToAScreen((double X, double Y, double W, double H) b)
    {
        foreach (var s in Screens.All)
            if (s.Bounds.Contains(new PixelPoint((int)b.X, (int)b.Y)))
                return b;
        return (100, 100, b.W, b.H);
    }
}
```

- [ ] **Step 2: Build + Core tests, 0 warnings**

Run:
```bash
dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj /p:TreatWarningsAsErrors=true --nologo
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --nologo
```
Expected: 0 warnings, 90/90 tests pass.

- [ ] **Step 3: Manual run gate (needs camera + display)**

Run: `dotnet run --project src/CameraOnScreen.App.Avalonia` (with `DISPLAY`/`XAUTHORITY`/`DOTNET_ROOT` set). Verify:
1. Start → frameless overlay opens at the configured position with live video.
2. Left-drag anywhere on the video moves it; wheel grows/shrinks it (center-anchored, aspect kept, ≥120 px high).
3. Mirror toggle in the panel flips the video live.
4. Stop → overlay closes; Start → it reopens where it was.
5. Close the panel → `~/.config/CameraOnScreen/config.json` `Overlay.X/Y/Width/Height` match where the overlay last sat.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat(avalonia): panel wires overlay to Start/Stop, live mirror, geometry persistence (#31)"
```

---

### Task 3: Docs, issue, PR

**Files:**
- Modify: `CLAUDE.md` (Linux build section)
- Modify: GitHub issue #31 checklist; PR

**Interfaces:** none — bookkeeping.

- [ ] **Step 1: Document the overlay in CLAUDE.md's Linux section**

Append to the "### Linux build" section paragraph (after the FakeShim sentence):

```
Phase 3 overlay: `Overlay/OverlayWindow.cs` — frameless transparent Topmost Avalonia window
(Spike B winner (a), OBS-verified on KWin/XWayland), own 33 ms pump reading `TryGetFrame`;
opens/closes with Start/Stop; drag = `BeginMoveDrag`, wheel-resize = `OverlaySizer`,
geometry persists to config on panel close. Premultiplied alpha ready for Phase 4 mattes.
```

- [ ] **Step 2: Commit docs**

```bash
git add CLAUDE.md
git commit -m "docs: Linux Phase 3 overlay notes"
```

- [ ] **Step 3: Push, PR, issue bookkeeping**

```bash
git push -u origin feat/linux-phase3-overlay-spike
gh pr create --base feat/linux-phase2-shim-v4l2 --title "feat: Linux Phase 3 — overlay + present (#31)" --body "..."
```
(Stacked on the Phase 2 branch while PR #30 is open; retarget to `main` after #30 merges.)
Tick the remaining #31 checkboxes that are now true (overlay window, present path, drag+resize, geometry persist, panel wiring) and comment with the manual-gate results.

---

## Self-Review

- **Spec coverage:** #31 checklist → Task 1 (window, present, premul alpha), Task 2 (drag/resize, persist, wiring), Task 3 (docs). Frame-rate measurement was done in the spike (present-fps log); the overlay drops the counter (YAGNI — status panel shows shim fps).
- **Placeholders:** none — all code inline. PR body "..." is authored at execution time from the actual results (not a code placeholder).
- **Type consistency:** `SetMirror(bool)`, ctor `(INativeShim, double, double, double, double, bool)`, `BoundsOf` tuple, `ToAppConfig(double,double,double,double)` — checked against Core sources this session; `ShimRef`/`IsRunning`/`Mirror` exist on `MainViewModel` (used identically by the WinUI twin).
