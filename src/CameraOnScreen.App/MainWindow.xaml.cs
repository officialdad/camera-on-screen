using System.ComponentModel;
using CameraOnScreen.App.Composition;
using CameraOnScreen.Core.ViewModels;
using Microsoft.UI.Xaml;

namespace CameraOnScreen.App;

public sealed partial class MainWindow : Window, INotifyPropertyChanged
{
    public MainViewModel Vm { get; }

    private readonly Overlay.OverlayWindow _overlay;
    // Big enough for 1920x1080 BGRA; the test camera is 640x480. TryGetFrame writes the actual size.
    private readonly byte[] _frameBuf = new byte[1920 * 1080 * 4];
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _timer;

    public event PropertyChangedEventHandler? PropertyChanged;

    public MainWindow()
    {
        // Build the overlay BEFORE the VM so its D3D device pointer exists when shim.Init runs.
        _overlay = new Overlay.OverlayWindow(200, 200, 320, 240);
        _overlay.Show();
        Vm = Services.BuildViewModel(_overlay);
        Vm.PropertyChanged += OnVmPropertyChanged;
        this.Closed += OnWindowClosed;
        InitializeComponent();

        // ~30 Hz frame pump on the WinUI UI thread: pull the latest BGRA frame from the shim and
        // blit it into the overlay, then refresh status. All D3D work happens here on the UI thread.
        _timer = DispatcherQueue.CreateTimer();
        _timer.Interval = TimeSpan.FromMilliseconds(33);
        _timer.Tick += (_, _) =>
        {
            if (Vm.IsRunning && Vm.ShimRef.TryGetFrame(_frameBuf, out int w, out int h) && w > 0)
                _overlay.PresentFrame(_frameBuf, w, h);
            Vm.PollStatusTick();
        };
        _timer.Start();
    }

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        _timer?.Stop();
        _timer = null;
        Vm.PropertyChanged -= OnVmPropertyChanged;
        Vm.Dispose();
        _overlay.Dispose();
    }

    public Visibility NotAvailableVisibility =>
        Vm.EffectsAvailable ? Visibility.Collapsed : Visibility.Visible;

    public string StatusLine =>
        Vm.IsRunning ? $"Running — {Vm.Fps:F0} fps" : "Stopped";

    private void OnVmPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(MainViewModel.IsRunning) or nameof(MainViewModel.Fps))
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(StatusLine)));
    }
}
