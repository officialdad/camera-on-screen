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
        SuperResAvailable = orchestrator.SuperResAvailable;
        FrameInterpAvailable = orchestrator.FrameInterpAvailable;
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
        SuperResAvailable = _orchestrator.SuperResAvailable;
        FrameInterpAvailable = _orchestrator.FrameInterpAvailable;
    }

    public ObservableCollection<CameraInfo> Cameras { get; } = new();

    [ObservableProperty] private CameraInfo? selectedCamera;
    [ObservableProperty] private bool greenScreenEnabled = true;
    [ObservableProperty] private double greenScreenExpand;
    [ObservableProperty] private double greenScreenFeather;
    [ObservableProperty] private bool eyeContactEnabled;
    [ObservableProperty] private double eyeContactSensitivity = 0.5;
    [ObservableProperty] private double eyeContactLookAwayRange = 0.5;
    [ObservableProperty] private int superResModeIndex;      // 0=Off, 1=Denoise, 2=Deblur
    [ObservableProperty] private int superResQualityIndex;   // 0=Low, 1=Med, 2=High, 3=Ultra
    [ObservableProperty] private bool frameInterpEnabled;
    [ObservableProperty] private bool effectsAvailable;
    [ObservableProperty] private string capabilityDetail = "Checking effect availability…";
    [ObservableProperty] private bool eyeContactAvailable;
    [ObservableProperty] private string eyeContactDetail = "Checking effect availability…";
    [ObservableProperty] private bool superResAvailable;
    [ObservableProperty] private bool frameInterpAvailable;
    [ObservableProperty] private bool exposureLock;             // #16: lock exposure to hold fps
    [ObservableProperty] private double exposureValue = 0.5;    // 0..1 normalized; only meaningful when locked
    [ObservableProperty] private bool exposureSupported;        // camera exposes manual exposure (status poll, while running)
    [ObservableProperty] private bool isRunning;
    [ObservableProperty] private bool mirror;
    [ObservableProperty] private double fps;
    [ObservableProperty] private string? statusError;
    [ObservableProperty] private GazeState gaze;

    // VSR QualityLevel base per mode (Denoise=8, Deblur=12) + quality 0..3. Off => 0.
    // Upscale (1-4) dropped: wasted on a downscaled overlay.
    private static int QualityLevelFor(int mode, int quality) => mode switch
    {
        1 => 8 + quality, 2 => 12 + quality, _ => 0,
    };

    public void LoadFrom(AppConfig config)
    {
        GreenScreenEnabled = config.Effects.GreenScreenEnabled;
        GreenScreenExpand = config.Effects.GreenScreenExpand;
        GreenScreenFeather = config.Effects.GreenScreenFeather;
        EyeContactEnabled = config.Effects.EyeContactEnabled;
        EyeContactSensitivity = config.Effects.EyeContactSensitivity;
        EyeContactLookAwayRange = config.Effects.EyeContactLookAwayRange;
        // Clamp to the live 3-mode range (0=Off,1=Denoise,2=Deblur): a config saved by an older
        // build (4-mode, included Upscale) can carry an index past the combo's item count, which
        // throws ArgumentException when the binding sets SelectedIndex → startup crash.
        SuperResModeIndex = Math.Clamp(config.Effects.SuperResMode, 0, 2);
        SuperResQualityIndex = Math.Clamp(config.Effects.SuperResQuality, 0, 3);
        ExposureLock = config.Effects.ExposureLock;
        ExposureValue = Math.Clamp(config.Effects.ExposureValue, 0.0, 1.0);
        FrameInterpEnabled = config.Effects.FrameInterpEnabled;
        Mirror = config.Overlay.Mirror;
        _hotkeys = config.Hotkeys;
        if (config.CameraId is not null)
            SelectedCamera = Cameras.FirstOrDefault(c => c.Id == config.CameraId);
    }

    // Capture current VM + overlay state into a persistable AppConfig. Geometry is passed in by the
    // caller (read from the live overlay window). Mirror is the kept observable prop; Lock/ClickThrough/Zoom were removed (overlay is always interactive).
    public AppConfig ToAppConfig(double x, double y, double w, double h) => new()
    {
        CameraId = SelectedCamera?.Id,
        Overlay = new OverlaySettings
        {
            X = x, Y = y, Width = w, Height = h,
            // ponytail: Locked/ClickThrough/Zoom intentionally omitted — fields kept on the record
            // (default false/false/1.0) so config stays schema-stable; the overlay is always
            // interactive and unzoomed now.
            Mirror = Mirror
        },
        Effects = new EffectSettings
        {
            GreenScreenEnabled = GreenScreenEnabled, GreenScreenExpand = GreenScreenExpand,
            GreenScreenFeather = GreenScreenFeather,
            EyeContactEnabled = EyeContactEnabled, EyeContactSensitivity = EyeContactSensitivity,
            EyeContactLookAwayRange = EyeContactLookAwayRange,
            SuperResMode = SuperResModeIndex,
            SuperResQuality = SuperResQualityIndex,
            ExposureLock = ExposureLock,
            ExposureValue = ExposureValue,
            FrameInterpEnabled = FrameInterpEnabled,
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
    partial void OnSuperResModeIndexChanged(int value) => ApplyLiveParams();
    partial void OnSuperResQualityIndexChanged(int value) => ApplyLiveParams();
    partial void OnFrameInterpEnabledChanged(bool value) => ApplyLiveParams();
    partial void OnExposureLockChanged(bool value) => ApplyLiveParams();
    partial void OnExposureValueChanged(double value) => ApplyLiveParams();

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
        EyeContactLookAwayRange: EyeContactLookAwayRange,
        SuperResEnabled: SuperResModeIndex != 0,
        SuperResScale: 0,  // denoise/deblur: out == in, no upscale
        SuperResQualityLevel: QualityLevelFor(SuperResModeIndex, SuperResQualityIndex),
        ExposureLockEnabled: ExposureLock,
        ExposureValue: ExposureValue,
        FrameInterpEnabled: FrameInterpEnabled);

    public void OnStatus(ShimStatus s)
    {
        IsRunning = s.Running;
        Fps = s.Fps;
        Gaze = s.Gaze;
        StatusError = s.Error;
        ExposureSupported = s.ExposureSupported;
    }

    public void Dispose()
    {
        _orchestrator.StatusChanged -= _statusHandler;
        // Dispose the shim: PInvokeShim.Dispose -> cos_shutdown -> Capture::Stop joins the native
        // capture worker thread. Without this the worker is never stopped, so at process exit the
        // global std::thread (CaptureState::worker) is destroyed while still joinable, which calls
        // std::terminate()/abort() — the debug-build "Debug Error … abort() has been called" dialog.
        ShimRef.Dispose();
    }

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
