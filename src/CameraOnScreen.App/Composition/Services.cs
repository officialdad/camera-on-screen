using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.Native;
using CameraOnScreen.Core.Orchestration;
using CameraOnScreen.Core.ViewModels;

namespace CameraOnScreen.App.Composition;

public static class Services
{
    public static MainViewModel BuildViewModel()
    {
        var store = new JsonSettingsStore(JsonSettingsStore.DefaultPath());
        var config = store.Load();
        INativeShim shim = new CameraOnScreen.App.Native.PInvokeShim();
        shim.Init(IntPtr.Zero); // real D3D device passed in Task 11
        var tier = CameraOnScreen.App.Native.GpuTierDetector.Detect();
        var orchestrator = new Orchestrator(shim, tier);
        var vm = new MainViewModel(orchestrator);
        foreach (var cam in shim.EnumerateCameras()) vm.Cameras.Add(cam);
        vm.LoadFrom(config);
        return vm;
    }
}
