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

    // Retained across LoadFrom → ToAppConfig so Save never discards loaded/custom hotkeys. The VM
    // has no UI for editing hotkeys, so it must round-trip them verbatim. Initialised to the
    // defaults so ToAppConfig is valid even if LoadFrom is never called.
    private IReadOnlyList<HotkeyBinding> _hotkeys = AppConfig.DefaultHotkeys();

    // Shared shim instance — the frame pump (Task 12) pulls frames via ShimRef.TryGetFrame.
    // MUST be the same instance the Orchestrator drives, so Start/Stop and frame production agree.
    public INativeShim ShimRef { get; }

    public MainViewModel(Orchestrator orchestrator, INativeShim shim)
    {
        _orchestrator = orchestrator;
        ShimRef = shim;
        // Mirror the orchestrator's pre-probe state: effects gated off, "checking" placeholder note.
        // ProbeCapabilitiesAsync publishes the real values once the (deferred) probe completes.
        EffectsAvailable = orchestrator.EffectsAvailable;
        CapabilityDetail = orchestrator.CapabilityDetail;
        EyeContactAvailable = orchestrator.EyeContactAvailable;
        EyeContactDetail = orchestrator.EyeContactDetail;
        _statusHandler = (_, s) => OnStatus(s);
        _orchestrator.StatusChanged += _statusHandler;
    }

    // Driven by the UI-thread frame pump each tick to refresh status (fps/gaze/error).
    public void PollStatusTick() => _orchestrator.PollStatus();

    /// <summary>Runs the native capability probe off the UI thread, then publishes the result to the
    /// observable props. The probe does a ~1s TensorRT model load, so it must not block startup. In
    /// the app `await` resumes on the UI dispatcher (so the XAML bindings update on the UI thread);
    /// in unit tests (no SynchronizationContext) it resumes on the thread pool.</summary>
    public async Task ProbeCapabilitiesAsync()
    {
        await Task.Run(_orchestrator.ProbeCapabilities);
        EffectsAvailable = _orchestrator.EffectsAvailable;
        CapabilityDetail = _orchestrator.CapabilityDetail;
        EyeContactAvailable = _orchestrator.EyeContactAvailable;
        EyeContactDetail = _orchestrator.EyeContactDetail;
    }

    public ObservableCollection<CameraInfo> Cameras { get; } = new();

    [ObservableProperty] private CameraInfo? selectedCamera;
    [ObservableProperty] private bool greenScreenEnabled = true;
    [ObservableProperty] private double greenScreenExpand;
    [ObservableProperty] private double greenScreenFeather;
    [ObservableProperty] private bool eyeContactEnabled;
    [ObservableProperty] private double eyeContactSensitivity = 0.5;
    [ObservableProperty] private double eyeContactLookAwayRange = 0.5;
    [ObservableProperty] private bool effectsAvailable;
    [ObservableProperty] private string capabilityDetail = "Checking effect availability…";
    [ObservableProperty] private bool eyeContactAvailable;
    [ObservableProperty] private string eyeContactDetail = "Checking effect availability…";
    [ObservableProperty] private bool isRunning;
    [ObservableProperty] private bool locked;
    [ObservableProperty] private bool clickThrough;
    [ObservableProperty] private bool mirror;
    [ObservableProperty] private double zoom = 1.0;
    [ObservableProperty] private double fps;
    [ObservableProperty] private string? statusError;
    [ObservableProperty] private GazeState gaze;

    public void LoadFrom(AppConfig config)
    {
        GreenScreenEnabled = config.Effects.GreenScreenEnabled;
        GreenScreenExpand = config.Effects.GreenScreenExpand;
        GreenScreenFeather = config.Effects.GreenScreenFeather;
        EyeContactEnabled = config.Effects.EyeContactEnabled;
        EyeContactSensitivity = config.Effects.EyeContactSensitivity;
        EyeContactLookAwayRange = config.Effects.EyeContactLookAwayRange;
        Locked = config.Overlay.Locked;
        ClickThrough = config.Overlay.ClickThrough;
        Mirror = config.Overlay.Mirror;
        Zoom = config.Overlay.Zoom;
        _hotkeys = config.Hotkeys;
        if (config.CameraId is not null)
            SelectedCamera = Cameras.FirstOrDefault(c => c.Id == config.CameraId);
    }

    // Capture current VM + overlay state into a persistable AppConfig. Geometry is passed in by the
    // caller (read from the live overlay window). Locked/ClickThrough are the existing observable
    // props (Task 13) — used here, not redeclared.
    public AppConfig ToAppConfig(double x, double y, double w, double h) => new()
    {
        CameraId = SelectedCamera?.Id,
        Overlay = new OverlaySettings
        {
            X = x, Y = y, Width = w, Height = h,
            Locked = Locked, ClickThrough = ClickThrough,
            Mirror = Mirror, Zoom = Zoom
        },
        Effects = new EffectSettings
        {
            GreenScreenEnabled = GreenScreenEnabled, GreenScreenExpand = GreenScreenExpand,
            GreenScreenFeather = GreenScreenFeather,
            EyeContactEnabled = EyeContactEnabled, EyeContactSensitivity = EyeContactSensitivity,
            EyeContactLookAwayRange = EyeContactLookAwayRange
        },
        Hotkeys = _hotkeys
    };

    // Live param push: when the user flips an effect toggle or moves a slider WHILE running, send the
    // fresh params to the shim immediately. Without this, SetParams only fired at Start, so toggling
    // green screen did nothing until the next Stop→Start. Gated on IsRunning so config load (LoadFrom)
    // and pre-start changes don't drive a not-yet-started shim — Start sends the initial params.
    // The MVVM-toolkit source generator calls these On…Changed partials after each property setter.
    partial void OnGreenScreenEnabledChanged(bool value) => ApplyLiveParams();
    partial void OnGreenScreenExpandChanged(double value) => ApplyLiveParams();
    partial void OnGreenScreenFeatherChanged(double value) => ApplyLiveParams();
    partial void OnEyeContactEnabledChanged(bool value) => ApplyLiveParams();
    partial void OnEyeContactSensitivityChanged(double value) => ApplyLiveParams();
    partial void OnEyeContactLookAwayRangeChanged(double value) => ApplyLiveParams();

    private void ApplyLiveParams()
    {
        if (IsRunning) _orchestrator.ApplyParams(BuildParams());
    }

    public ShimParams BuildParams() => new(
        CameraId: SelectedCamera?.Id,
        GreenScreenEnabled: GreenScreenEnabled,
        GreenScreenExpand: GreenScreenExpand,
        GreenScreenFeather: GreenScreenFeather,
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
