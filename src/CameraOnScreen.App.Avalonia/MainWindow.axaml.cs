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
            else if (e.PropertyName is nameof(MainViewModel.FrameInterpEnabled) or nameof(MainViewModel.FrameInterpAvailable))
                _overlay?.SetFrameInterp(_services.Vm.FrameInterpEnabled && _services.Vm.FrameInterpAvailable);
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
            var w = new Overlay.OverlayWindow(_services.Vm.ShimRef, b.X, b.Y, b.W, b.H, _services.Vm.Mirror,
                _services.Loaded.Overlay.TeleportModifiers);
            // Alt+F4 on the frameless overlay closes it directly: remember where it was and
            // let capture keep running; the next Stop/Start round-trip reopens it.
            w.Closed += (_, _) => { if (ReferenceEquals(_overlay, w)) { _overlayBounds = BoundsOf(w); _overlay = null; } };
            w.SetFrameInterp(_services.Vm.FrameInterpEnabled && _services.Vm.FrameInterpAvailable);
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
