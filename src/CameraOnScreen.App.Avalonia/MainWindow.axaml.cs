using Avalonia.Controls;
using Avalonia.Threading;
using CameraOnScreen.App.Avalonia.Composition;

namespace CameraOnScreen.App.Avalonia;

public partial class MainWindow : Window
{
    private readonly Services.AppServices _services;
    private readonly DispatcherTimer _statusTimer;

    public MainWindow()
    {
        InitializeComponent();
        _services = Services.Build();
        DataContext = _services.Vm;

        // Status is polled, never pushed (repo contract). No frame pump yet — the Linux
        // overlay is Phase 3 — so 4 Hz keeps fps/error/running fresh without burning CPU.
        _statusTimer = new DispatcherTimer(TimeSpan.FromMilliseconds(250),
            DispatcherPriority.Background, (_, _) => _services.Vm.PollStatusTick());
        _statusTimer.Start();

        Closing += (_, _) =>
        {
            // Overlay geometry passes through from the loaded config until the Linux
            // overlay (Phase 3) exists to supply live values.
            var o = _services.Loaded.Overlay;
            _services.Store.Save(_services.Vm.ToAppConfig(o.X, o.Y, o.Width, o.Height));
        };
        Closed += (_, _) =>
        {
            _statusTimer.Stop();
            // Joins the native capture worker (cos_shutdown) — without this the global
            // std::thread is destroyed joinable at process exit -> std::terminate.
            _services.Vm.Dispose();
        };
    }
}
