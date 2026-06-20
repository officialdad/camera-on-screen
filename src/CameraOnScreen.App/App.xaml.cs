using System;
using System.IO;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;

namespace CameraOnScreen.App;

public partial class App : Application
{
    // Top-level crash log lives next to the app's config in %LOCALAPPDATA%\CameraOnScreen so a
    // startup crash that never reaches a window still leaves a diagnosable trail.
    private static readonly string LogDir =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CameraOnScreen");
    private static readonly string LogPath = Path.Combine(LogDir, "startup-error.log");

    public App()
    {
        // Wire the belt-and-suspenders handlers BEFORE InitializeComponent so a failure during
        // application resource load is still captured.
        this.UnhandledException += OnXamlUnhandledException;
        AppDomain.CurrentDomain.UnhandledException += OnDomainUnhandledException;
        TaskScheduler.UnobservedTaskException += OnUnobservedTaskException;
        InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            var window = new MainWindow();
            window.Activate();
        }
        catch (Exception ex)
        {
            LogException("OnLaunched", ex);
            throw;
        }
    }

    private void OnXamlUnhandledException(object sender, Microsoft.UI.Xaml.UnhandledExceptionEventArgs e)
        => LogException("Xaml.UnhandledException", e.Exception);

    private void OnDomainUnhandledException(object sender, System.UnhandledExceptionEventArgs e)
        => LogException("AppDomain.UnhandledException", e.ExceptionObject as Exception);

    private void OnUnobservedTaskException(object? sender, UnobservedTaskExceptionEventArgs e)
        => LogException("TaskScheduler.UnobservedTaskException", e.Exception);

    // Best-effort, never-throw logger. Appends the full exception (ToString includes the
    // InnerException chain) plus the HResult so WER-bucketed crashes become diagnosable.
    private static void LogException(string source, Exception? ex)
    {
        try
        {
            Directory.CreateDirectory(LogDir);
            var hr = ex is null ? 0 : ex.HResult;
            File.AppendAllText(LogPath,
                $"[{DateTimeOffset.Now:O}] {source} (HRESULT=0x{hr:X8}):{Environment.NewLine}" +
                $"{ex?.ToString() ?? "<null exception>"}{Environment.NewLine}{Environment.NewLine}");
        }
        catch
        {
            // Never let the logger mask the original failure.
        }
    }
}
