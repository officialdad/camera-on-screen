using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Threading;
using CameraOnScreen.App.Avalonia.Composition;
using CameraOnScreen.Core.ViewModels;

namespace CameraOnScreen.App.Avalonia;

public partial class MainWindow : Window
{
    private readonly Services.AppServices _services;
    private readonly DispatcherTimer _statusTimer;
    private readonly Tray.TrayController _tray;
    // Set only by the tray's Quit item: tells Closing to let the close through instead of
    // cancelling it into a Hide().
    private bool _quitting;
    private Overlay.OverlayWindow? _overlay;
    // Last-known overlay geometry: seeded from config, refreshed every time the overlay
    // closes, written back to config on panel exit.
    private (double X, double Y, double W, double H) _overlayBounds;
    // Gates the minimize-to-tray handler below. NOT latched on Opened (#62): Window.ShowCore
    // raises Opened synchronously right after PlatformImpl.Show, and X11Window.Show is only
    // XMapWindow + XFlush, so every WindowState transition arrives later from a PropertyNotify
    // on _NET_WM_STATE — an Opened latch is already true before any state change can be seen and
    // blocks nothing. Set instead by whichever comes first: a transition to a non-Minimized
    // state (the WM clearing its transient _NET_WM_STATE_HIDDEN, i.e. the window is genuinely
    // up), or a 1 s grace timer from Opened for the quiet launch that emits no transition at all.
    private bool _armed;

    public MainWindow()
    {
        InitializeComponent();
        _services = Services.Build();
        DataContext = _services.Vm;

        var o = _services.Loaded.Overlay;
        _overlayBounds = (o.X, o.Y, o.Width, o.Height);

        _tray = new Tray.TrayController(_services.Vm, ShowPanel, Quit);

        Opened += (_, _) => DispatcherTimer.RunOnce(() => _armed = true, TimeSpan.FromSeconds(1));

        // Minimize means "get out of the way", same as close: hide to the tray instead. Do NOT
        // also write WindowState = Normal here — that write races the WM's own async deiconify
        // and wins, so the window bounces back visible and cannot be minimized at all (measured
        // on KDE/XWayland). ShowPanel() sets WindowState = Normal on the restore side, which is
        // where it belongs. Gated on _armed so a transient _NET_WM_STATE_HIDDEN during X11 map
        // cannot hide the panel before the user has ever seen it (#62).
        PropertyChanged += (_, e) =>
        {
            if (e.Property != WindowStateProperty) return;
            // Any non-minimized state means the window is genuinely up — arm immediately rather
            // than waiting out the grace timer.
            if (WindowState != WindowState.Minimized) { _armed = true; return; }
            if (_armed && _services.Vm.MinimizeToTray) Hide();
        };

        // Status is polled, never pushed (repo contract). The overlay runs its own 33 ms
        // frame pump; this panel-side timer only refreshes fps/error/running at 4 Hz.
        _statusTimer = new DispatcherTimer(TimeSpan.FromMilliseconds(250),
            DispatcherPriority.Background, (_, _) => _services.Vm.PollStatusTick());
        _statusTimer.Start();

        _services.Vm.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(MainViewModel.IsRunning)) SyncOverlay();
            else if (e.PropertyName == nameof(MainViewModel.Mirror)) _overlay?.SetMirror(_services.Vm.Mirror);
            else if (e.PropertyName is nameof(MainViewModel.FrameInterpEnabled) or nameof(MainViewModel.FrameInterpAvailable))
                _overlay?.SetFrameInterp(_services.Vm.FrameInterpEnabled && _services.Vm.FrameInterpAvailable);
        };

        Closing += (_, e) =>
        {
            CaptureOverlayBounds();
            var b = _overlayBounds;
            // Save on the way to the tray too, not just on a real exit: an app killed while
            // sitting in the tray then loses nothing it would not already have lost.
            _services.Store.Save(_services.Vm.ToAppConfig(b.X, b.Y, b.W, b.H));
            // Only an ordinary window close (the X button / our own Close() call) becomes a
            // hide. OwnerWindowClosing/ApplicationShutdown/OSShutdown mean the session or the
            // process is going away regardless of what we do here — cancelling those into a
            // Hide() would strand the capture worker (cos_shutdown never runs) instead of
            // tearing down cleanly.
            if (_services.Vm.MinimizeToTray && !_quitting && e.CloseReason == WindowCloseReason.WindowClosing)
            {
                e.Cancel = true;
                Hide();
            }
        };
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
    }

    // Opening the dropdown is exactly the moment the list needs to be true, and it costs no
    // extra chrome. Enumeration opens every /dev/video* node — see the manual gate in the plan
    // for the check that this does not perturb a live capture.
    private void CameraCombo_DropDownOpened(object? sender, EventArgs e) =>
        _services.Vm.RefreshCameras();

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
