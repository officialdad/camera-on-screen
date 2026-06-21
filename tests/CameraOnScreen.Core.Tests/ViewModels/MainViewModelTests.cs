using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.Native;
using CameraOnScreen.Core.Orchestration;
using CameraOnScreen.Core.ViewModels;
using Xunit;

namespace CameraOnScreen.Core.Tests.ViewModels;

public class MainViewModelTests
{
    private static MainViewModel Build(GpuTier tier, out FakeShim shim,
        bool greenScreenAvailable = false)
    {
        shim = new FakeShim
        {
            Cameras = { new CameraInfo("cam", "Cam") },
            GreenScreenAvailable = greenScreenAvailable
        };
        // Same shim instance drives the orchestrator AND is exposed via VM.ShimRef.
        return new MainViewModel(new Orchestrator(shim, tier), shim);
    }

    [Fact]
    public void Effects_gated_off_before_probe_runs()
    {
        // Probe is async now — until ProbeCapabilitiesAsync completes, effects stay gated off and
        // the note shows the "checking" placeholder (so the toggles aren't briefly enabled).
        var vm = Build(GpuTier.Rtx, out _, greenScreenAvailable: true);
        Assert.False(vm.EffectsAvailable);
        Assert.False(string.IsNullOrWhiteSpace(vm.CapabilityDetail));
    }

    [Fact]
    public async Task Probe_unavailable_disables_effects_and_surfaces_detail()
    {
        // Explicitly set GreenScreenAvailable = false; tier is irrelevant. After the async probe the
        // VM must keep effects off AND surface the shim's real reason string (not a static message).
        var vm = Build(GpuTier.NonRtx, out _, greenScreenAvailable: false);
        await vm.ProbeCapabilitiesAsync();
        Assert.False(vm.EffectsAvailable);
        Assert.Equal("fake: unavailable", vm.CapabilityDetail);
    }

    [Fact]
    public async Task NonRtx_tier_does_not_gate_effects_when_probe_says_available()
    {
        // Non-RTX tier must NOT disable effects when the shim probe reports available.
        var vm = Build(GpuTier.NonRtx, out _, greenScreenAvailable: true);
        await vm.ProbeCapabilitiesAsync();
        Assert.True(vm.EffectsAvailable);
        Assert.Equal("fake: available", vm.CapabilityDetail);
    }

    [Fact]
    public async Task Toggling_effect_while_running_pushes_params_live_to_shim()
    {
        // Bug: SetParams only fired at Start, so toggling green screen while running was a no-op
        // until the next Stop→Start. Live toggles must push fresh params to the shim immediately.
        var vm = Build(GpuTier.Rtx, out var shim, greenScreenAvailable: true);
        await vm.ProbeCapabilitiesAsync();
        vm.SelectedCamera = new CameraInfo("cam", "Cam");
        vm.GreenScreenEnabled = true;
        vm.StartCommand.Execute(null);
        Assert.True(shim.LastParams!.GreenScreenEnabled);

        vm.GreenScreenEnabled = false;      // toggle OFF while running
        Assert.False(shim.LastParams!.GreenScreenEnabled);

        vm.GreenScreenEnabled = true;       // toggle back ON while running
        Assert.True(shim.LastParams!.GreenScreenEnabled);
    }

    [Fact]
    public void Toggling_effect_before_start_does_not_push_to_shim()
    {
        // Before Start the shim isn't running; live-push must be gated on IsRunning so config load
        // (LoadFrom) and pre-start fiddling don't drive a not-yet-started shim. Start sends params.
        var vm = Build(GpuTier.Rtx, out var shim, greenScreenAvailable: true);
        vm.GreenScreenEnabled = false;
        vm.GreenScreenEnabled = true;
        Assert.Null(shim.LastParams);
    }

    [Fact]
    public async Task Live_toggle_respects_probe_gate()
    {
        // When effects are unavailable, a live toggle ON must still be forced OFF (same gate as Start).
        var vm = Build(GpuTier.Rtx, out var shim, greenScreenAvailable: false);
        await vm.ProbeCapabilitiesAsync();
        vm.SelectedCamera = new CameraInfo("cam", "Cam");
        vm.StartCommand.Execute(null);
        vm.GreenScreenEnabled = true;
        Assert.False(shim.LastParams!.GreenScreenEnabled);
    }

    [Fact]
    public void BuildParams_reflects_vm_state()
    {
        var vm = Build(GpuTier.Rtx, out _);
        vm.GreenScreenEnabled = true;
        vm.GreenScreenStrength = 0.7;
        vm.SelectedCamera = new CameraInfo("cam", "Cam");
        var p = vm.BuildParams();
        Assert.Equal("cam", p.CameraId);
        Assert.Equal(0.7, p.GreenScreenStrength);
    }

    [Fact]
    public void Start_sets_running_and_OnStatus_updates_fps()
    {
        var vm = Build(GpuTier.Rtx, out _);
        vm.SelectedCamera = new CameraInfo("cam", "Cam");
        vm.StartCommand.Execute(null);
        Assert.True(vm.IsRunning);
        vm.OnStatus(new ShimStatus(true, 42, GazeState.OnCamera, true, false, null));
        Assert.Equal(42, vm.Fps);
        Assert.Equal(GazeState.OnCamera, vm.Gaze);
    }

    [Fact]
    public void LoadFrom_propagates_locked_and_clickthrough()
    {
        var vm = Build(GpuTier.Rtx, out _);
        var config = new AppConfig
        {
            Overlay = new OverlaySettings { Locked = true, ClickThrough = true }
        };
        vm.LoadFrom(config);
        Assert.True(vm.Locked);
        Assert.True(vm.ClickThrough);
    }

    [Fact]
    public void ToAppConfig_captures_geometry_and_effects()
    {
        var vm = Build(GpuTier.Rtx, out _);
        vm.GreenScreenEnabled = true; vm.Locked = true;
        var cfg = vm.ToAppConfig(10, 20, 300, 400);
        Assert.Equal(10, cfg.Overlay.X);
        Assert.Equal(20, cfg.Overlay.Y);
        Assert.Equal(300, cfg.Overlay.Width);
        Assert.Equal(400, cfg.Overlay.Height);
        Assert.True(cfg.Overlay.Locked);
        Assert.False(cfg.Overlay.ClickThrough);
        Assert.True(cfg.Effects.GreenScreenEnabled);
        Assert.Null(cfg.CameraId);
    }

    [Fact]
    public void ToAppConfig_preserves_loaded_hotkeys()
    {
        var vm = Build(GpuTier.Rtx, out _);
        // Start from the defaults but change one binding's VirtualKey so the loaded list is
        // provably different from DefaultHotkeys() — guards against ToAppConfig silently
        // reverting hotkeys to the defaults on Save.
        var custom = AppConfig.DefaultHotkeys()
            .Select((b, i) => i == 0 ? b with { VirtualKey = 0x42 } : b)
            .ToArray();
        vm.LoadFrom(new AppConfig { Hotkeys = custom });

        var cfg = vm.ToAppConfig(10, 20, 300, 400);

        Assert.True(cfg.Hotkeys.SequenceEqual(custom));
        Assert.False(cfg.Hotkeys.SequenceEqual(AppConfig.DefaultHotkeys()));
    }

    [Fact]
    public void Dispose_unsubscribes_from_status()
    {
        var shim = new ControllableFpsShim { FpsValue = 10 };
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        var vm = new MainViewModel(orch, shim);

        // Start then poll to confirm subscription is live
        orch.Start(new ShimParams("cam", false, 1.0, false, 0.5, 0.5));
        orch.PollStatus();
        Assert.Equal(10, vm.Fps);

        // Dispose then change fps and poll again — vm.Fps must NOT update
        vm.Dispose();
        shim.FpsValue = 99;
        orch.PollStatus();
        Assert.Equal(10, vm.Fps);
    }

    private sealed class ControllableFpsShim : INativeShim
    {
        public double FpsValue { get; set; }
        private bool _running;
        public bool Init(IntPtr d) => true;
        public IReadOnlyList<CameraInfo> EnumerateCameras() => Array.Empty<CameraInfo>();
        public void SetParams(ShimParams p) { }
        public void Start() => _running = true;
        public void Stop() => _running = false;
        public ShimStatus GetStatus() => new(_running, FpsValue, GazeState.Unknown, false, false, null);
        public bool TryGetFrame(byte[] buffer, out int width, out int height) { width = 0; height = 0; return false; }
        public ShimCapabilities QueryCapabilities() => new(false, "test");
        public void Dispose() { }
    }
}
