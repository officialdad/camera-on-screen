using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.Native;
using CameraOnScreen.Core.Orchestration;
using CameraOnScreen.Core.ViewModels;
using Xunit;

public class MainViewModelTests
{
    private static MainViewModel Build(GpuTier tier, out FakeShim shim)
    {
        shim = new FakeShim { Cameras = { new CameraInfo("cam", "Cam") } };
        return new MainViewModel(new Orchestrator(shim, tier));
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
}
