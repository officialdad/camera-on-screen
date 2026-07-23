using Avalonia.Controls;
using CameraOnScreen.Core.Native;
using CameraOnScreen.Core.Orchestration;
using CameraOnScreen.Core.ViewModels;

namespace CameraOnScreen.App.Avalonia;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        DataContext = BuildDemoViewModel();
    }

    // ponytail: Phase 1 is control-panel UI parity only. The real native shim (Media Foundation on
    // Windows, V4L2 on Linux) + overlay/pump are Phase 2+, so a fully-available FakeShim stands in
    // so every control renders enabled and bindings are exercised. Real shim wiring replaces this.
    private static MainViewModel BuildDemoViewModel()
    {
        var shim = new FakeShim
        {
            GreenScreenAvailable = true,
            EyeContactAvailable = true,
            SuperResAvailable = true,
            FrameInterpAvailable = true,
        };
        var orchestrator = new Orchestrator(shim, GpuTier.Rtx);
        var vm = new MainViewModel(orchestrator, shim);

        vm.Cameras.Add(new CameraInfo("cam0", "Integrated Webcam"));
        vm.Cameras.Add(new CameraInfo("cam1", "USB Capture HDMI"));
        vm.SelectedCamera = vm.Cameras[0];

        // The App sets finger-control availability after ORT init (independent of the Maxine probe).
        vm.FingerControlAvailable = true;
        vm.FingerControlDetail = "";

        // Same deferred capability probe the WinUI app fires from its ctor; FakeShim reports all
        // effects available, so the toggles ungrey once it lands.
        _ = vm.ProbeCapabilitiesAsync();
        return vm;
    }
}
