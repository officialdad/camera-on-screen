using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.Native;
using CameraOnScreen.Core.Orchestration;

namespace CameraOnScreen.Core.ViewModels;

public sealed partial class MainViewModel : ObservableObject, IDisposable
{
    private readonly Orchestrator _orchestrator;
    private readonly EventHandler<ShimStatus> _statusHandler;

    // Shared shim instance — the frame pump (Task 12) pulls frames via ShimRef.TryGetFrame.
    // MUST be the same instance the Orchestrator drives, so Start/Stop and frame production agree.
    public INativeShim ShimRef { get; }

    public MainViewModel(Orchestrator orchestrator, INativeShim shim)
    {
        _orchestrator = orchestrator;
        ShimRef = shim;
        EffectsAvailable = orchestrator.EffectsAvailable;
        _statusHandler = (_, s) => OnStatus(s);
        _orchestrator.StatusChanged += _statusHandler;
    }

    // Driven by the UI-thread frame pump each tick to refresh status (fps/gaze/error).
    public void PollStatusTick() => _orchestrator.PollStatus();

    public ObservableCollection<CameraInfo> Cameras { get; } = new();

    [ObservableProperty] private CameraInfo? selectedCamera;
    [ObservableProperty] private bool greenScreenEnabled = true;
    [ObservableProperty] private double greenScreenStrength = 1.0;
    [ObservableProperty] private bool eyeContactEnabled;
    [ObservableProperty] private double eyeContactSensitivity = 0.5;
    [ObservableProperty] private double eyeContactLookAwayRange = 0.5;
    [ObservableProperty] private bool effectsAvailable;
    [ObservableProperty] private bool isRunning;
    [ObservableProperty] private double fps;
    [ObservableProperty] private string? statusError;
    [ObservableProperty] private GazeState gaze;

    public void LoadFrom(AppConfig config)
    {
        GreenScreenEnabled = config.Effects.GreenScreenEnabled;
        GreenScreenStrength = config.Effects.GreenScreenStrength;
        EyeContactEnabled = config.Effects.EyeContactEnabled;
        EyeContactSensitivity = config.Effects.EyeContactSensitivity;
        EyeContactLookAwayRange = config.Effects.EyeContactLookAwayRange;
        if (config.CameraId is not null)
            SelectedCamera = Cameras.FirstOrDefault(c => c.Id == config.CameraId);
    }

    public ShimParams BuildParams() => new(
        CameraId: SelectedCamera?.Id,
        GreenScreenEnabled: GreenScreenEnabled,
        GreenScreenStrength: GreenScreenStrength,
        EyeContactEnabled: EyeContactEnabled,
        EyeContactSensitivity: EyeContactSensitivity,
        EyeContactLookAwayRange: EyeContactLookAwayRange);

    public void OnStatus(ShimStatus s)
    {
        IsRunning = s.Running;
        Fps = s.Fps;
        Gaze = s.Gaze;
        StatusError = s.Error;
    }

    public void Dispose() => _orchestrator.StatusChanged -= _statusHandler;

    [RelayCommand]
    private void Start()
    {
        _orchestrator.Start(BuildParams());
        IsRunning = true;
    }

    [RelayCommand]
    private void Stop()
    {
        _orchestrator.Stop();
        IsRunning = false;
    }
}
