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

        // ponytail: FakeShim + Orchestrator stand in for the native shim so this Windows scaffold
        // proves Core/VM reuse with zero native code. Real shim wiring lands in Phase 1.
        var shim = new FakeShim { GreenScreenAvailable = true };
        var orchestrator = new Orchestrator(shim, GpuTier.NonRtx);
        DataContext = new MainViewModel(orchestrator, shim);
    }
}
