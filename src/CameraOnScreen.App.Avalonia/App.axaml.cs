using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;

namespace CameraOnScreen.App.Avalonia;

public partial class App : Application
{
    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            // Spike B (#31): `--overlay-spike` opens the transparent overlay probe instead
            // of the control panel.
            desktop.MainWindow = desktop.Args?.Contains("--overlay-spike") == true
                ? new OverlaySpikeWindow()
                : new MainWindow();
        }

        base.OnFrameworkInitializationCompleted();
    }
}