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

    // Same round-trip rule as _hotkeys: config-only knob (#53, no UI) — retain the loaded value so
    // ToAppConfig's fresh OverlaySettings doesn't reset a customised chord to the default.
    private HotkeyModifiers _teleportModifiers = new OverlaySettings().TeleportModifiers;

    // The camera capture is actually running on — distinct from SelectedCamera, which is whatever
    // the combo shows. Guards the hot-swap against a transient null during a list refresh and
    // against re-selecting the camera already running (a restart is a full worker teardown).
    private string? _activeCameraId;

    // Set by OnFrameReceived. Task 4's watchdog reads it; null means no frame has arrived yet
    // since the last (re)start.
    private long? _lastFrameMs;

    // Silence thresholds. The no-first-frame window is the longer of the two: Start can
    // legitimately take seconds while a Maxine effect loads its TensorRT engine.
    private const long DisconnectMs = 2000;
    private const long NoFirstFrameMs = 5000;

    // Anchors the no-first-frame window. Set on the first CheckLiveness after a (re)start rather
    // than at Start, so the VM never reads a clock itself and the tests stay deterministic.
    private long? _livenessAnchorMs;

    // Shared shim instance — the frame pump (Task 12) pulls frames via ShimRef.TryGetFrame.
    // MUST be the same instance the Orchestrator drives, so Start/Stop and frame production agree.
    public INativeShim ShimRef { get; }

    /// <summary>True only while something is pumping frames and reporting them via
    /// <see cref="OnFrameReceived"/> — i.e. while the Avalonia overlay window is open. The
    /// watchdog is off otherwise, so "nobody is reporting frames" means disabled, not dead.
    /// The WinUI panel never sets this (spec §9), so Windows behaviour is unchanged.</summary>
    public bool FrameReportingActive { get; set; }

    public MainViewModel(Orchestrator orchestrator, INativeShim shim)
    {
        _orchestrator = orchestrator;
        ShimRef = shim;
        // Mirror the orchestrator's pre-probe state: effects gated off, "checking" placeholder note.
        // ProbeCapabilitiesAsync publishes the real values once the (deferred) probe completes.
        EffectsAvailable = orchestrator.EffectsAvailable;
        CapabilityDetail = orchestrator.CapabilityDetail;
        GreenScreenMaxineAvailable = orchestrator.GreenScreenMaxineAvailable;
        GreenScreenOnnxAvailable = orchestrator.GreenScreenOnnxAvailable;
        EyeContactAvailable = orchestrator.EyeContactAvailable;
        EyeContactDetail = orchestrator.EyeContactDetail;
        SuperResAvailable = orchestrator.SuperResAvailable;
        FrameInterpAvailable = orchestrator.FrameInterpAvailable;
        _statusHandler = (_, s) => OnStatus(s);
        _orchestrator.StatusChanged += _statusHandler;
    }

    // Driven by the UI-thread status timer each tick to refresh status (fps/gaze/error) and to
    // run the camera liveness watchdog.
    public void PollStatusTick()
    {
        _orchestrator.PollStatus();
        CheckLiveness(Environment.TickCount64);
    }

    /// <summary>Called by the overlay's frame pump for every frame it successfully pulls.
    /// TryGetFrame returning true IS the liveness signal — Capture::LatestFrame clears its
    /// new-frame flag on read, so a true return means a genuinely new frame arrived.
    /// <paramref name="nowMs"/> is passed in rather than read from the clock so the watchdog
    /// (CheckLiveness) is unit-testable without a clock abstraction; callers pass
    /// Environment.TickCount64.</summary>
    public void OnFrameReceived(int width, int height, long nowMs)
    {
        _lastFrameMs = nowMs;
        FrameWidth = width;
        FrameHeight = height;
    }

    /// <summary>Auto-stop when the selected camera goes silent. Deliberately a timeout rather
    /// than native error detection: when scrcpy dies, v4l2loopback does not report an error — it
    /// just goes quiet, select() times out forever, and the capture worker's break paths never
    /// fire. A timeout catches that, genuine ioctl failures, and the Windows equivalents with one
    /// implementation. A frozen stale frame silently baked into a recording is worse than the
    /// overlay visibly closing.</summary>
    public void CheckLiveness(long nowMs)
    {
        // Disarmed: clear _lastFrameMs too, not just the anchor. Otherwise a stale stamp from
        // before the pump went away (e.g. Alt+F4 on the overlay) sits armed indefinitely, and
        // the next FrameReportingActive false->true (reopening the overlay) would see a stale
        // _lastFrameMs, apply the short DisconnectMs window instead of the no-first-frame grace,
        // and fire "Camera disconnected" on the very first tick. A freshly-armed pump has
        // reported no frame, so the 5s window is the correct one for it.
        if (!IsRunning || !FrameReportingActive) { _lastFrameMs = null; _livenessAnchorMs = null; return; }
        _livenessAnchorMs ??= nowMs;

        long since = nowMs - (_lastFrameMs ?? _livenessAnchorMs.Value);
        if (since < (_lastFrameMs is null ? NoFirstFrameMs : DisconnectMs)) return;

        CameraError = _lastFrameMs is null ? "No frames from camera" : "Camera disconnected";
        Stop();
    }

    // Clears the frame history so a (re)started camera gets its own grace period, instead of
    // being condemned by the previous camera's last-frame stamp.
    private void ResetLiveness()
    {
        _lastFrameMs = null;
        _livenessAnchorMs = null;
        CameraError = null;
    }

    // Clears the negotiated frame size at the two session-start points (Start, hot-swap restart).
    // Deliberately NOT folded into ResetLiveness: that method also runs from
    // ResetLivenessIfRunning() on every discrete effect toggle, and blanking the size there would
    // flicker the status line to 0x0 on each toggle for no reason — a restart is the only case
    // where the OLD camera's resolution is actually stale. Without this, a hot-swap keeps
    // rendering the previous camera's resolution for the whole restart (worker join + device open
    // + possible Maxine engine load — seconds), and indefinitely if the new camera never delivers
    // a frame, until the watchdog trips.
    private void ResetFrameSize()
    {
        FrameWidth = 0;
        FrameHeight = 0;
    }

    /// <summary>Runs the native capability probe off the UI thread, then publishes the result to the
    /// observable props. The probe does a ~1s TensorRT model load, so it must not block startup. In
    /// the app `await` resumes on the UI dispatcher (so the XAML bindings update on the UI thread);
    /// in unit tests (no SynchronizationContext) it resumes on the thread pool.</summary>
    public async Task ProbeCapabilitiesAsync()
    {
        await Task.Run(_orchestrator.ProbeCapabilities);
        EffectsAvailable = _orchestrator.EffectsAvailable;
        CapabilityDetail = _orchestrator.CapabilityDetail;
        GreenScreenMaxineAvailable = _orchestrator.GreenScreenMaxineAvailable;
        GreenScreenOnnxAvailable = _orchestrator.GreenScreenOnnxAvailable;
        EyeContactAvailable = _orchestrator.EyeContactAvailable;
        EyeContactDetail = _orchestrator.EyeContactDetail;
        SuperResAvailable = _orchestrator.SuperResAvailable;
        FrameInterpAvailable = _orchestrator.FrameInterpAvailable;
    }

    public ObservableCollection<CameraInfo> Cameras { get; } = new();

    /// <summary>Re-enumerate devices and diff into <see cref="Cameras"/> by Id: drop what's gone,
    /// add what's new, leave existing entries alone. Diff rather than clear-and-refill so the
    /// ComboBox selection never churns — a Clear() nulls SelectedItem, and the re-add would fire
    /// OnSelectedCameraChanged twice for what the user experiences as no change.
    /// Called when the camera dropdown opens: a v4l2loopback device (scrcpy, OBS) only announces
    /// itself while its writer is attached, so the startup enumeration goes stale.</summary>
    public void RefreshCameras()
    {
        var live = ShimRef.EnumerateCameras();
        for (int i = Cameras.Count - 1; i >= 0; i--)
            if (!live.Any(c => c.Id == Cameras[i].Id))
                Cameras.RemoveAt(i);
        foreach (var cam in live)
            if (!Cameras.Any(c => c.Id == cam.Id))
                Cameras.Add(cam);
    }

    [ObservableProperty] private CameraInfo? selectedCamera;
    [ObservableProperty] private bool greenScreenEnabled = true;
    [ObservableProperty] private double greenScreenExpand;
    [ObservableProperty] private double greenScreenFeather;
    [ObservableProperty] private int greenScreenBackendIndex;
    [ObservableProperty] private bool eyeContactEnabled;
    [ObservableProperty] private double eyeContactSensitivity = 0.5;
    [ObservableProperty] private double eyeContactLookAwayRange = 0.5;
    [ObservableProperty] private int superResModeIndex;      // 0=Off, 1=Denoise, 2=Deblur
    [ObservableProperty] private int superResQualityIndex;   // 0=Low, 1=Med, 2=High, 3=Ultra
    [ObservableProperty] private bool frameInterpEnabled;
    [ObservableProperty] private bool effectsAvailable;
    [ObservableProperty] private string capabilityDetail = "Checking effect availability…";
    [ObservableProperty] private bool greenScreenMaxineAvailable;
    [ObservableProperty] private bool greenScreenOnnxAvailable;
    [ObservableProperty] private bool eyeContactAvailable;
    [ObservableProperty] private string eyeContactDetail = "Checking effect availability…";
    [ObservableProperty] private bool superResAvailable;
    [ObservableProperty] private bool frameInterpAvailable;
    [ObservableProperty] private int frameWidth;    // negotiated capture size; 0 until the first frame
    [ObservableProperty] private int frameHeight;
    [ObservableProperty] private bool exposureLock;             // #16: lock exposure to hold fps
    [ObservableProperty] private double exposureValue = 0.5;    // 0..1 normalized; only meaningful when locked
    [ObservableProperty] private bool exposureSupported;        // camera exposes manual exposure (status poll, while running)
    [ObservableProperty] private bool isRunning;
    [ObservableProperty] private bool mirror;
    [ObservableProperty] private double fps;
    [ObservableProperty] private string? statusError;
    [ObservableProperty] private GazeState gaze;
    [ObservableProperty] private string? cameraError;   // sticky: OnStatus must not clear it

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
        GreenScreenBackendIndex = config.Effects.GreenScreenBackend is >= 0 and <= 2 ? config.Effects.GreenScreenBackend : 0;
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
        _teleportModifiers = config.Overlay.TeleportModifiers;
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
            Mirror = Mirror,
            TeleportModifiers = _teleportModifiers
        },
        Effects = new EffectSettings
        {
            GreenScreenEnabled = GreenScreenEnabled, GreenScreenExpand = GreenScreenExpand,
            GreenScreenFeather = GreenScreenFeather, GreenScreenBackend = GreenScreenBackendIndex,
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
    //
    // The six discrete-selection partials below also call ResetLivenessIfRunning(): flipping one of
    // these brings the capture worker's engine up inline on the worker thread (NvAR_Load /
    // NvVFX_Load — eyecontact.cpp/aigs.cpp), which publishes no frames for the load duration. With
    // _lastFrameMs already set from before the toggle, CheckLiveness would otherwise apply the short
    // 2s DisconnectMs window and auto-stop a healthy capture mid-engine-load. Re-arming gives that
    // load the same 5s no-first-frame grace Start gets. NOT applied to the continuous sliders or
    // ExposureLock: none of them can trigger an engine load, and re-arming on every slider tick would
    // let a real disconnect hide behind idle UI fiddling for 5s at a time.
    partial void OnGreenScreenEnabledChanged(bool value) { ApplyLiveParams(); ResetLivenessIfRunning(); }
    partial void OnGreenScreenExpandChanged(double value) => ApplyLiveParams();
    partial void OnGreenScreenFeatherChanged(double value) => ApplyLiveParams();
    partial void OnGreenScreenBackendIndexChanged(int value) { ApplyLiveParams(); ResetLivenessIfRunning(); }
    partial void OnEyeContactEnabledChanged(bool value) { ApplyLiveParams(); ResetLivenessIfRunning(); }
    partial void OnEyeContactSensitivityChanged(double value) => ApplyLiveParams();
    partial void OnEyeContactLookAwayRangeChanged(double value) => ApplyLiveParams();
    partial void OnSuperResModeIndexChanged(int value) { ApplyLiveParams(); ResetLivenessIfRunning(); }
    partial void OnSuperResQualityIndexChanged(int value) { ApplyLiveParams(); ResetLivenessIfRunning(); }
    partial void OnFrameInterpEnabledChanged(bool value) { ApplyLiveParams(); ResetLivenessIfRunning(); }
    partial void OnExposureLockChanged(bool value) => ApplyLiveParams();
    partial void OnExposureValueChanged(double value) => ApplyLiveParams();

    // The camera equivalent of the live param push above. Native Capture::Start opens with
    // StopLocked() (capture_v4l2.cpp:591, capture.cpp:602), so calling Start again is a clean
    // restart on the new device. IsRunning deliberately does NOT change: SyncOverlay keys off it
    // alone, so leaving it true keeps the overlay window, its geometry, and its mirror state.
    partial void OnSelectedCameraChanged(CameraInfo? value)
    {
        if (!IsRunning || value is null || value.Value.Id == _activeCameraId) return;
        _orchestrator.Start(BuildParams());
        _activeCameraId = value.Value.Id;
        ResetLiveness();
        ResetFrameSize();
    }

    private void ApplyLiveParams()
    {
        if (IsRunning) _orchestrator.ApplyParams(BuildParams());
    }

    // Gated on IsRunning the same way ApplyLiveParams is, so a config load (LoadFrom) or a
    // pre-Start change doesn't touch watchdog state.
    private void ResetLivenessIfRunning()
    {
        if (IsRunning) ResetLiveness();
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
        FrameInterpEnabled: FrameInterpEnabled,
        GreenScreenBackend: GreenScreenBackendIndex);

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
        _activeCameraId = SelectedCamera?.Id;
        ResetLiveness();
        ResetFrameSize();
        IsRunning = true;
    }

    [RelayCommand]
    private void Stop()
    {
        _orchestrator.Stop();
        _activeCameraId = null;
        IsRunning = false;
    }
}
