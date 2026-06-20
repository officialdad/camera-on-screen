using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.Native;
using CameraOnScreen.Core.Orchestration;
using CameraOnScreen.Core.ViewModels;

namespace CameraOnScreen.App.Composition;

public static class Services
{
    public static MainViewModel BuildViewModel(CameraOnScreen.App.Overlay.OverlayWindow overlay)
    {
        var store = new JsonSettingsStore(JsonSettingsStore.DefaultPath());
        var config = store.Load();
        INativeShim shim = new CameraOnScreen.App.Native.PInvokeShim();
        // Shared device: hand the overlay's D3D device to the shim. In M2 the shim still returns a
        // CPU BGRA buffer and does not use the device for D3D, but the shared-device contract is
        // established now (M3 will move frame production onto it, CUDA<->D3D interop).
        shim.Init(overlay.D3DDevicePtr);
        var tier = CameraOnScreen.App.Native.GpuTierDetector.Detect();
        var orchestrator = new Orchestrator(shim, tier);
        // Same shim instance drives the orchestrator AND is exposed via VM.ShimRef for the pump.
        var vm = new MainViewModel(orchestrator, shim);
        foreach (var cam in shim.EnumerateCameras()) vm.Cameras.Add(cam);
        vm.LoadFrom(config);
        return vm;
    }
}
