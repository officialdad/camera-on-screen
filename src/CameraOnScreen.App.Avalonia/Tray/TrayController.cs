using System.ComponentModel;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Platform;
using CameraOnScreen.Core.ViewModels;

namespace CameraOnScreen.App.Avalonia.Tray;

/// <summary>
/// Owns the system-tray icon and its native menu. On Linux this is backed by the freedesktop
/// StatusNotifierItem D-Bus interface, which Avalonia speaks for us.
///
/// ponytail: no availability probe. If the session runs no StatusNotifierItem host the icon
/// silently never appears, and a hidden panel is unreachable — the escape hatches are the
/// "Minimize to tray" toggle and "MinimizeToTray": false in config.json (both documented in the
/// README). Upgrade path if that bites: query D-Bus for org.kde.StatusNotifierWatcher at startup
/// and force MinimizeToTray off when it is missing.
/// </summary>
public sealed class TrayController : IDisposable
{
    private readonly MainViewModel _vm;
    private readonly TrayIcon _icon;
    private readonly NativeMenuItem _runItem;

    public TrayController(MainViewModel vm, Action showPanel, Action quit)
    {
        _vm = vm;

        // Start/Stop routes to the panel's own commands — no second copy of the start logic.
        _runItem = new NativeMenuItem();
        _runItem.Click += (_, _) =>
        {
            if (_vm.IsRunning) _vm.StopCommand.Execute(null);
            else _vm.StartCommand.Execute(null);
        };

        var showItem = new NativeMenuItem("Show Panel");
        showItem.Click += (_, _) => showPanel();

        var quitItem = new NativeMenuItem("Quit");
        quitItem.Click += (_, _) => quit();

        var menu = new NativeMenu();
        menu.Add(showItem);
        menu.Add(_runItem);
        menu.Add(new NativeMenuItemSeparator());
        menu.Add(quitItem);

        _icon = new TrayIcon
        {
            Icon = new WindowIcon(AssetLoader.Open(
                new Uri("avares://CameraOnScreen.App.Avalonia/Assets/cos.png"))),
            Menu = menu,
            IsVisible = true,
        };
        _icon.Clicked += (_, _) => showPanel();

        // The attached property is Avalonia's registration hook — a TrayIcon that is not in
        // Application's TrayIcons collection is never shown.
        TrayIcon.SetIcons(Application.Current!, new TrayIcons { _icon });

        _vm.PropertyChanged += OnVmPropertyChanged;
        SyncRunState();
    }

    private void OnVmPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(MainViewModel.IsRunning)) SyncRunState();
    }

    private void SyncRunState()
    {
        _runItem.Header = _vm.IsRunning ? "Stop Camera" : "Start Camera";
        _icon.ToolTipText = _vm.IsRunning ? "Camera-on-Screen — running" : "Camera-on-Screen — stopped";
    }

    public void Dispose()
    {
        _vm.PropertyChanged -= OnVmPropertyChanged;
        TrayIcon.SetIcons(Application.Current!, new TrayIcons());
        _icon.Dispose();
    }
}
