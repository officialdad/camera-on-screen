using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.Native;
using CameraOnScreen.Core.Orchestration;
using CameraOnScreen.Core.ViewModels;

namespace CameraOnScreen.App.Composition;

public static class Services
{
    // Task 9 replaces FakeShim with PInvokeShim and GpuTier with GpuTierDetector.Detect().
    public static MainViewModel BuildViewModel()
    {
        var store = new JsonSettingsStore(JsonSettingsStore.DefaultPath());
        var config = store.Load();
        INativeShim shim = new FakeShim();
        var orchestrator = new Orchestrator(shim, GpuTier.NonRtx);
        var vm = new MainViewModel(orchestrator);
        vm.LoadFrom(config);
        return vm;
    }
}
