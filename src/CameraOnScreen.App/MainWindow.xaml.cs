using System.ComponentModel;
using CameraOnScreen.App.Composition;
using CameraOnScreen.Core.ViewModels;
using Microsoft.UI.Xaml;

namespace CameraOnScreen.App;

public sealed partial class MainWindow : Window, INotifyPropertyChanged
{
    public MainViewModel Vm { get; }

    private readonly Overlay.OverlayWindow _overlay;

    public event PropertyChangedEventHandler? PropertyChanged;

    public MainWindow()
    {
        Vm = Services.BuildViewModel();
        Vm.PropertyChanged += OnVmPropertyChanged;
        this.Closed += OnWindowClosed;
        InitializeComponent();
        _overlay = new Overlay.OverlayWindow(200, 200, 320, 240);
        _overlay.Show();
        _overlay.PresentTestPattern();
    }

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
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
