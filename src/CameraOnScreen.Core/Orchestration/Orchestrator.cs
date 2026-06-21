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

        // Probe the shim to determine real effect availability.
        // The tier (RTX heuristic) is kept only for display; the probe is authoritative.
        var caps = _shim.QueryCapabilities();
        EffectsAvailable = caps.GreenScreenAvailable;
        CapabilityDetail = caps.Detail;
    }

    public OrchestratorState State { get; private set; } = OrchestratorState.Idle;

    /// <summary>True when the native shim reports Green Screen can actually run.</summary>
    public bool EffectsAvailable { get; }

    /// <summary>Human-readable reason string from the shim probe (e.g. "SDK not found").</summary>
    public string CapabilityDetail { get; }

    /// <summary>The detected GPU tier — used for display only, not for effect gating.</summary>
    public GpuTier GpuTier => _tier;

    public event EventHandler<ShimStatus>? StatusChanged;

    public void Start(ShimParams requested)
    {
        var effective = EffectsAvailable
            ? requested
            : requested with { GreenScreenEnabled = false, EyeContactEnabled = false };
        _shim.SetParams(effective);
        _shim.Start();
        State = OrchestratorState.Running;
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
