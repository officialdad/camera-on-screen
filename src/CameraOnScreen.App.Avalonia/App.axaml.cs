using Avalonia;
using Avalonia.Controls;
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
            // Load-bearing for minimize-to-tray: under the default OnLastWindowClose, a panel
            // hidden to the tray with no overlay open leaves zero open windows and Avalonia
            // exits — the tray icon would vanish seconds after the user hid the panel. The only
            // exit path now is MainWindow's Closed handler calling Shutdown().
            desktop.ShutdownMode = ShutdownMode.OnExplicitShutdown;
            desktop.MainWindow = new MainWindow();
        }

        base.OnFrameworkInitializationCompleted();
    }
}