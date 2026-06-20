using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.Native;
using CameraOnScreen.Core.Orchestration;
using CameraOnScreen.Core.ViewModels;
using Xunit;

namespace CameraOnScreen.Core.Tests.ViewModels;

public class MainViewModelTests
{
    private static MainViewModel Build(GpuTier tier, out FakeShim shim)
    {
        shim = new FakeShim { Cameras = { new CameraInfo("cam", "Cam") } };
        // Same shim instance drives the orchestrator AND is exposed via VM.ShimRef.
        return new MainViewModel(new Orchestrator(shim, tier), shim);
    }

    [Fact]
    public void NonRtx_disables_effects_in_vm()
    {
        var vm = Build(GpuTier.NonRtx, out _);
        Assert.False(vm.EffectsAvailable);
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
        public void Dispose() { }
    }
}
