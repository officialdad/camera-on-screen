using CameraOnScreen.Core.Native;

namespace CameraOnScreen.Core.Orchestration;

public enum GpuTier { Rtx, NonRtx }
public enum OrchestratorState { Idle, Running, Faulted }

public sealed class Orchestrator
{
    private readonly INativeShim _shim;
    private readonly GpuTier _tier;

    public Orchestrator(INativeShim shim, GpuTier tier)
    {
        _shim = shim;
        _tier = tier;
    }

    public OrchestratorState State { get; private set; } = OrchestratorState.Idle;
    public bool EffectsAvailable => _tier == GpuTier.Rtx;
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
        if (status.Error is not null) State = OrchestratorState.Faulted;
        StatusChanged?.Invoke(this, status);
    }
}
