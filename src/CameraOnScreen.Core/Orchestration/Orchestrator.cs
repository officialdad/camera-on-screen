using CameraOnScreen.Core.Native;

namespace CameraOnScreen.Core.Orchestration;

public enum GpuTier { Rtx, NonRtx }
public enum OrchestratorState { Idle, Running, Faulted }

public sealed class Orchestrator
{
    private readonly INativeShim _shim;
    // GpuTier is retained for GPU-name / tier display only. It no longer gates effects.
    private readonly GpuTier _tier;

    public Orchestrator(INativeShim shim, GpuTier tier)
    {
        _shim = shim;
        _tier = tier;
        // No probe here: QueryCapabilities does a ~1s TensorRT model load, so running it in the ctor
        // froze the UI thread at startup. Capabilities are probed lazily via ProbeCapabilities()
        // (callers run it off the UI thread). Until then effects stay gated OFF (safe default).
    }

    public OrchestratorState State { get; private set; } = OrchestratorState.Idle;

    /// <summary>True when the native shim reports Green Screen can actually run. False until
    /// <see cref="ProbeCapabilities"/> has run.</summary>
    public bool EffectsAvailable { get; private set; }

    /// <summary>Human-readable reason string from the shim probe (e.g. "SDK not found"). Shows a
    /// "checking" placeholder until <see cref="ProbeCapabilities"/> has run.</summary>
    public string CapabilityDetail { get; private set; } = "Checking effect availability…";

    /// <summary>True when the shim reports Eye Contact can run. False until <see cref="ProbeCapabilities"/>.</summary>
    public bool EyeContactAvailable { get; private set; }

    /// <summary>Human-readable reason from the eye-contact probe. "Checking…" until probed.</summary>
    public string EyeContactDetail { get; private set; } = "Checking effect availability…";

    /// <summary>True when the shim reports Super Resolution can run. False until <see cref="ProbeCapabilities"/>.</summary>
    public bool SuperResAvailable { get; private set; }

    /// <summary>True when the shim reports Frame Interpolation (FRUC) can run. False until <see cref="ProbeCapabilities"/>.</summary>
    public bool FrameInterpAvailable { get; private set; }

    /// <summary>Runs the (blocking) native capability probe and records the result. Run this OFF the
    /// UI thread — the real probe does a ~1s TensorRT model load. The tier (RTX heuristic) is kept
    /// only for display; this probe is the authoritative effect gate.</summary>
    public void ProbeCapabilities()
    {
        var caps = _shim.QueryCapabilities();
        EffectsAvailable = caps.GreenScreenAvailable;
        CapabilityDetail = caps.Detail;
        EyeContactAvailable = caps.EyeContactAvailable;
        EyeContactDetail = caps.EyeContactDetail;
        SuperResAvailable = caps.SuperResAvailable;
        FrameInterpAvailable = caps.FrameInterpAvailable;
    }

    /// <summary>The detected GPU tier — used for display only, not for effect gating.</summary>
    public GpuTier GpuTier => _tier;

    public event EventHandler<ShimStatus>? StatusChanged;

    public void Start(ShimParams requested)
    {
        ApplyParams(requested);
        _shim.Start();
        State = OrchestratorState.Running;
    }

    /// <summary>Push effect params to the shim — at Start and on every live toggle/slider change
    /// while running. Applies the effect gate: when effects are unavailable they are forced off
    /// (so a UI toggle can never enable an effect the probe said can't run).</summary>
    public void ApplyParams(ShimParams requested)
    {
        var effective = requested with
        {
            GreenScreenEnabled = requested.GreenScreenEnabled && EffectsAvailable,
            EyeContactEnabled = requested.EyeContactEnabled && EyeContactAvailable,
            SuperResEnabled = requested.SuperResEnabled && SuperResAvailable,
            FrameInterpEnabled = requested.FrameInterpEnabled && FrameInterpAvailable,
        };
        _shim.SetParams(effective);
    }

    public void Stop()
    {
        _shim.Stop();
        State = OrchestratorState.Idle;
    }

    public void PollStatus()
    {
        var status = _shim.GetStatus();
        if (State is OrchestratorState.Running or OrchestratorState.Faulted)
            State = status.Error is not null ? OrchestratorState.Faulted : OrchestratorState.Running;
        StatusChanged?.Invoke(this, status);
    }
}
