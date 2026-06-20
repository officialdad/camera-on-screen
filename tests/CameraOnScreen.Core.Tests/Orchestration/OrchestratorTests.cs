using CameraOnScreen.Core.Native;
using CameraOnScreen.Core.Orchestration;
using Xunit;

namespace CameraOnScreen.Core.Tests.Orchestration;

public class OrchestratorTests
{
    private static ShimParams Requested() =>
        new(CameraId: "cam", GreenScreenEnabled: true, GreenScreenStrength: 1.0,
            EyeContactEnabled: true, EyeContactSensitivity: 0.5, EyeContactLookAwayRange: 0.5);

    [Fact]
    public void Rtx_tier_passes_effects_through()
    {
        var shim = new FakeShim();
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        orch.Start(Requested());
        Assert.Equal(OrchestratorState.Running, orch.State);
        Assert.True(shim.LastParams!.GreenScreenEnabled);
        Assert.True(shim.LastParams!.EyeContactEnabled);
    }

    [Fact]
    public void NonRtx_tier_forces_effects_off()
    {
        var shim = new FakeShim();
        var orch = new Orchestrator(shim, GpuTier.NonRtx);
        Assert.False(orch.EffectsAvailable);
        orch.Start(Requested());
        Assert.False(shim.LastParams!.GreenScreenEnabled);
        Assert.False(shim.LastParams!.EyeContactEnabled);
    }

    [Fact]
    public void PollStatus_raises_event_and_faults_on_error()
    {
        var shim = new ErroringShim();
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        ShimStatus? seen = null;
        orch.StatusChanged += (_, s) => seen = s;
        orch.Start(Requested());
        orch.PollStatus();
        Assert.NotNull(seen);
        Assert.Equal(OrchestratorState.Faulted, orch.State);
    }

    [Fact]
    public void Fault_clears_when_status_recovers()
    {
        var shim = new RecoverableShim();
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        orch.Start(Requested());

        // First poll: error → Faulted
        shim.HasError = true;
        orch.PollStatus();
        Assert.Equal(OrchestratorState.Faulted, orch.State);

        // Second poll: no error → back to Running
        shim.HasError = false;
        orch.PollStatus();
        Assert.Equal(OrchestratorState.Running, orch.State);
    }

    private sealed class ErroringShim : INativeShim
    {
        public bool Init(System.IntPtr d) => true;
        public System.Collections.Generic.IReadOnlyList<CameraInfo> EnumerateCameras() => System.Array.Empty<CameraInfo>();
        public void SetParams(ShimParams p) { }
        public void Start() { }
        public void Stop() { }
        public ShimStatus GetStatus() => new(true, 0, GazeState.Unknown, false, false, "boom");
        public bool TryGetFrame(byte[] buffer, out int width, out int height) { width = 0; height = 0; return false; }
        public ShimCapabilities QueryCapabilities() => new(false, "test");
        public void Dispose() { }
    }

    private sealed class RecoverableShim : INativeShim
    {
        public bool HasError { get; set; }
        public bool Init(System.IntPtr d) => true;
        public System.Collections.Generic.IReadOnlyList<CameraInfo> EnumerateCameras() => System.Array.Empty<CameraInfo>();
        public void SetParams(ShimParams p) { }
        public void Start() { }
        public void Stop() { }
        public ShimStatus GetStatus() => new(true, 30, GazeState.Unknown, false, false, HasError ? "transient error" : null);
        public bool TryGetFrame(byte[] buffer, out int width, out int height) { width = 0; height = 0; return false; }
        public ShimCapabilities QueryCapabilities() => new(false, "test");
        public void Dispose() { }
    }
}
