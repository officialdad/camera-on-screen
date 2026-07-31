using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.Native;
using CameraOnScreen.Core.Orchestration;
using CameraOnScreen.Core.ViewModels;

namespace CameraOnScreen.App.Avalonia.Composition;

public static class Services
{
    public sealed record AppServices(MainViewModel Vm, JsonSettingsStore Store, AppConfig Loaded);

    // Mirrors the WinUI Services.BuildViewModel: real shim + orchestrator + config-loaded VM.
    // Falls back to the FakeShim demo VM when the native library is absent (e.g. the shim
    // hasn't been built yet) so the panel still opens instead of crashing — the WinUI app
    // has no such fallback because its shim is deployed by its own build.
    public static AppServices Build()
    {
        var store = new JsonSettingsStore(JsonSettingsStore.DefaultPath());
        var config = store.Load();

        INativeShim shim;
        try
        {
            shim = new PInvokeShim();
            shim.Init(IntPtr.Zero); // no shared D3D device on Linux; first P/Invoke = load probe
        }
        catch (DllNotFoundException)
        {
            shim = new FakeShim();
        }

        var orchestrator = new Orchestrator(shim, Native.GpuTierDetector.Detect());
        var vm = new MainViewModel(orchestrator, shim);
        foreach (var cam in shim.EnumerateCameras()) vm.Cameras.Add(cam);
        vm.LoadFrom(config);
        if (vm.SelectedCamera is null && vm.Cameras.Count > 0) vm.SelectedCamera = vm.Cameras[0];

        // ORT hand-inference is not wired on Linux yet (Phase 3+): grey the toggle honestly.
        vm.FingerControlAvailable = false;
        vm.FingerControlDetail = "Finger control not available on Linux yet";

        _ = vm.ProbeCapabilitiesAsync();
        return new AppServices(vm, store, config);
    }
}
