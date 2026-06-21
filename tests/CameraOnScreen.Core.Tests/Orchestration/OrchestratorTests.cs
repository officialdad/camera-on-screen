using CameraOnScreen.Core.Native;
using CameraOnScreen.Core.Orchestration;
using Xunit;

namespace CameraOnScreen.Core.Tests.Orchestration;

public class OrchestratorTests
{
    private static ShimParams Requested() =>
        new(CameraId: "cam", GreenScreenEnabled: true, GreenScreenExpand: 0.5, GreenScreenFeather: 0.3,
            EyeContactEnabled: true, EyeContactSensitivity: 0.5, EyeContactLookAwayRange: 0.5);

    [Fact]
    public void Probe_available_passes_effects_through()
    {
        // When the shim reports effects available the orchestrator must forward them unchanged.
        // GpuTier is irrelevant to the gate; Rtx is used here only as a realistic construction arg.
        // Capabilities are probed lazily now (not in the ctor), so probe before Start.
        var shim = new FakeShim { GreenScreenAvailable = true };
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        orch.ProbeCapabilities();
        orch.Start(Requested());
        Assert.Equal(OrchestratorState.Running, orch.State);
        Assert.True(shim.LastParams!.GreenScreenEnabled);
        Assert.True(shim.LastParams!.EyeContactEnabled);
    }

    [Fact]
    public void Probe_unavailable_forces_effects_off_regardless_of_tier()
    {
        // When the shim reports effects unavailable, the orchestrator must strip them — even on RTX.
        var shim = new FakeShim { GreenScreenAvailable = false };
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        orch.ProbeCapabilities();
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

    // --- Task 7: probe-based gate ---

    [Fact]
    public void Effects_Disabled_When_Shim_Reports_Unavailable()
    {
        var shim = new FakeShim { GreenScreenAvailable = false };
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        orch.ProbeCapabilities();
        Assert.False(orch.EffectsAvailable);
    }

    [Fact]
    public void Effects_Enabled_When_Shim_Reports_Available()
    {
        var shim = new FakeShim { GreenScreenAvailable = true };
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        orch.ProbeCapabilities();
        Assert.True(orch.EffectsAvailable);
    }

    // --- Deferred (lazy/async) probe: ctor must NOT call the native probe ---

    [Fact]
    public void Effects_Gated_Off_Before_Probe_Runs()
    {
        // The real probe does a ~1s TensorRT model load; it must not run in the ctor. Until
        // ProbeCapabilities() is called, effects stay gated OFF regardless of what the shim reports.
        var shim = new FakeShim { GreenScreenAvailable = true };
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        Assert.False(orch.EffectsAvailable);
        orch.ProbeCapabilities();
        Assert.True(orch.EffectsAvailable);
    }

    [Fact]
    public void ProbeCapabilities_Records_Detail_From_Shim()
    {
        var shim = new FakeShim { GreenScreenAvailable = false };
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        orch.ProbeCapabilities();
        Assert.Equal("fake: unavailable", orch.CapabilityDetail);
    }
}
