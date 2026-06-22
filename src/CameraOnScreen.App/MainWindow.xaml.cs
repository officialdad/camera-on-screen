using System.ComponentModel;
using System.Runtime.InteropServices;
using CameraOnScreen.App.Composition;
using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.ViewModels;
using Microsoft.UI.Xaml;

namespace CameraOnScreen.App;

public sealed partial class MainWindow : Window, INotifyPropertyChanged
{
    // Compact control-panel size (in DIPs) — the panel only holds a few Auto-height rows, so the
    // WinUI default (~1100×700) leaves it mostly empty. Scaled by the window DPI before Resize,
    // which wants physical pixels. No layout redesign — sensible defaults only.
    private const int PanelWidthDip = 400, PanelHeightDip = 720;

    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);

    public MainViewModel Vm { get; }

    private readonly Overlay.OverlayWindow _overlay;
    private readonly Hotkeys.GlobalHotkeyService _hotkeys;
    private readonly JsonSettingsStore _store = new(JsonSettingsStore.DefaultPath());
    // Big enough for 1920x1080 BGRA; the test camera is 640x480. TryGetFrame writes the actual size.
    private readonly byte[] _frameBuf = new byte[1920 * 1080 * 4];
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _timer;
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _saveTimer; // debounces persist after wheel-resize
    private Overlay.MouseWheelHook? _wheelHook;

    // Default overlay geometry, used when the saved config has no usable (non-zero) bounds.
    private const int DefaultX = 200, DefaultY = 200, DefaultW = 320, DefaultH = 240;

    public event PropertyChangedEventHandler? PropertyChanged;

    public MainWindow()
    {
        // Read the persisted config BEFORE creating the overlay so we can restore geometry into the
        // ctor. Only restore when the saved size is sane (non-zero); otherwise fall back to defaults
        // (e.g. first run, corrupt config, or a config written before geometry was persisted).
        var config = _store.Load();
        var (x, y, w, h) = ResolveStartupBounds(config.Overlay);

        // Build the overlay BEFORE the VM so its D3D device pointer exists when shim.Init runs.
        _overlay = new Overlay.OverlayWindow(x, y, w, h);
        _overlay.Show();
        Vm = Services.BuildViewModel(_overlay);
        Vm.PropertyChanged += OnVmPropertyChanged;
        // Apply the initial lock / click-through state loaded from config to the overlay.
        _overlay.SetLocked(Vm.Locked);
        _overlay.SetClickThrough(Vm.ClickThrough);
        _overlay.SetMirror(Vm.Mirror);
        _overlay.SetZoom(Vm.Zoom);

        // Global hotkeys. RegisterHotKey targets the overlay HWND, so WM_HOTKEY arrives at the
        // overlay proc and is forwarded here via HotkeyPressed → the service. Action handling is
        // marshalled onto the UI thread because it toggles UI-bound VM observable props.
        _hotkeys = new Hotkeys.GlobalHotkeyService(_overlay.Hwnd);
        _overlay.HotkeyPressed += _hotkeys.OnHotkeyMessage;
        _hotkeys.Register(config.Hotkeys, OnHotkeyAction);

        // Persist on drag/resize END (one save per gesture, not per pixel) and on window close.
        _overlay.InteractionEnded += Save;
        this.Closed += OnWindowClosed;
        InitializeComponent();
        RightSizePanel();

        // ~30 Hz frame pump on the WinUI UI thread: pull the latest BGRA frame from the shim and
        // blit it into the overlay, then refresh status. All D3D work happens here on the UI thread.
        _timer = DispatcherQueue.CreateTimer();
        _timer.Interval = TimeSpan.FromMilliseconds(33);
        _timer.Tick += (_, _) =>
        {
            if (Vm.IsRunning && Vm.ShimRef.TryGetFrame(_frameBuf, out int w, out int h) && w > 0)
                _overlay.PresentFrame(_frameBuf, w, h);
            Vm.PollStatusTick();
        };
        _timer.Start();

        // Debounced persist for wheel-resize: the wheel has no "gesture end" event (unlike
        // WM_EXITSIZEMOVE), so coalesce rapid notches into a single Save() ~400ms after the last one.
        _saveTimer = DispatcherQueue.CreateTimer();
        _saveTimer.Interval = TimeSpan.FromMilliseconds(400);
        _saveTimer.IsRepeating = false;
        _saveTimer.Tick += (_, _) => Save();

        // Global wheel hook: resize the overlay when the cursor is over it and it is interactive.
        // The callback runs on the UI thread (the hook is installed here), so it touches the overlay
        // directly. Returning true swallows the wheel so the app under the cursor doesn't scroll.
        _wheelHook = new Overlay.MouseWheelHook(OnWheelOverScreen);

        // Probe effect availability OFF the UI thread (the real probe does a ~1s TensorRT model
        // load — running it in the ctor froze startup). Until it completes, EffectsAvailable is false
        // (toggles disabled) and the note shows "Checking effect availability…". `await` resumes on
        // the UI dispatcher, so the resulting OneWay binding updates happen on the UI thread.
        _ = Vm.ProbeCapabilitiesAsync();
    }

    // Shrink the control panel from the WinUI default to a compact size that fits its rows.
    // AppWindow.Resize takes physical pixels, so scale the DIP target by the window's DPI.
    private void RightSizePanel()
    {
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        double scale = GetDpiForWindow(hwnd) / 96.0;
        var id = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(hwnd);
        var appWindow = Microsoft.UI.Windowing.AppWindow.GetFromWindowId(id);
        appWindow.Resize(new Windows.Graphics.SizeInt32(
            (int)(PanelWidthDip * scale), (int)(PanelHeightDip * scale)));
    }

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        // Stop the timer first so no WM_EXITSIZEMOVE-driven InteractionEnded can race with Save().
        _timer?.Stop();
        _timer = null;
        // Unhook the global wheel hook and flush any pending debounced save before teardown, so no
        // hook callback fires into a disposed overlay.
        _wheelHook?.Dispose();
        _wheelHook = null;
        _saveTimer?.Stop();
        _saveTimer = null;
        // Unsubscribe both event sources BEFORE saving so a late WM_EXITSIZEMOVE cannot trigger a
        // double-save while teardown is in progress.
        _overlay.InteractionEnded -= Save;
        _overlay.HotkeyPressed -= _hotkeys.OnHotkeyMessage;
        Save(); // final persist with the closing geometry/state
        _hotkeys.Dispose(); // unregister all hotkeys
        Vm.PropertyChanged -= OnVmPropertyChanged;
        Vm.Dispose();
        _overlay.Dispose();
    }

    // Choose startup geometry: restore saved bounds only when the saved size is usable (>0);
    // otherwise fall back to defaults. Win32 ctor wants ints, so cast the double settings down.
    private static (int x, int y, int w, int h) ResolveStartupBounds(OverlaySettings s) =>
        s.Width > 0 && s.Height > 0
            ? ((int)s.X, (int)s.Y, (int)s.Width, (int)s.Height)
            : (DefaultX, DefaultY, DefaultW, DefaultH);

    // Map a global hotkey to a behavior. Hotkey messages can arrive off the UI thread, and these
    // toggles touch UI-bound VM observable props, so marshal onto the dispatcher.
    private void OnHotkeyAction(HotkeyAction action)
    {
        DispatcherQueue.TryEnqueue(() =>
        {
            switch (action)
            {
                case HotkeyAction.ToggleLock: Vm.Locked = !Vm.Locked; break;
                case HotkeyAction.ToggleClickThrough: Vm.ClickThrough = !Vm.ClickThrough; break;
                case HotkeyAction.ToggleOverlayVisible: _overlay.ToggleVisible(); break;
                case HotkeyAction.ToggleRunning:
                    if (Vm.IsRunning) Vm.StopCommand?.Execute(null); else Vm.StartCommand?.Execute(null);
                    break;
            }
        });
    }

    // Called on the UI thread for every WM_MOUSEWHEEL. Resize only when the cursor is inside the
    // overlay AND the overlay is interactive (not locked / not click-through). Returns true when we
    // handled the wheel so MouseWheelHook swallows it.
    private bool OnWheelOverScreen(Overlay.Interop.POINT pt, int notches)
    {
        if (!_overlay.IsInteractive) return false;

        var (x, y, w, h) = _overlay.GetBounds();
        bool inside = pt.x >= x && pt.x < x + w && pt.y >= y && pt.y < y + h;
        if (!inside) return false;

        var work = GetWorkArea(_overlay.Hwnd);
        var next = CameraOnScreen.Core.Overlay.OverlaySizer.Resize(
            new CameraOnScreen.Core.Overlay.Rect(x, y, w, h), notches, work);
        _overlay.SetBounds(next.X, next.Y, next.W, next.H);

        // Debounce the disk write: restart the one-shot timer on every notch.
        _saveTimer?.Stop();
        _saveTimer?.Start();
        return true;
    }

    // Work area (monitor minus taskbar) of the monitor the overlay is on, as an OverlaySizer.Rect.
    private static CameraOnScreen.Core.Overlay.Rect GetWorkArea(IntPtr hwnd)
    {
        IntPtr mon = Overlay.Interop.MonitorFromWindow(hwnd, Overlay.Interop.MONITOR_DEFAULTTONEAREST);
        var mi = new Overlay.Interop.MONITORINFO { cbSize = Marshal.SizeOf<Overlay.Interop.MONITORINFO>() };
        Overlay.Interop.GetMonitorInfo(mon, ref mi);
        var r = mi.rcWork;
        return new CameraOnScreen.Core.Overlay.Rect(r.left, r.top, r.right - r.left, r.bottom - r.top);
    }

    // Persist current VM state + live overlay geometry. Called on drag/resize END and on close —
    // NOT on every WM_MOVE/WM_SIZE, which would thrash the disk during a gesture.
    private void Save()
    {
        var (x, y, w, h) = _overlay.GetBounds();
        _store.Save(Vm.ToAppConfig(x, y, w, h));
    }

    public Visibility NotAvailableVisibility =>
        Vm.EffectsAvailable ? Visibility.Collapsed : Visibility.Visible;

    public Visibility EyeContactNotAvailableVisibility =>
        Vm.EyeContactAvailable ? Visibility.Collapsed : Visibility.Visible;

    public string StatusLine =>
        Vm.IsRunning ? $"Running — {Vm.Fps:F0} fps" : "Stopped";

    private void OnVmPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(MainViewModel.IsRunning) or nameof(MainViewModel.Fps))
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(StatusLine)));
        else if (e.PropertyName == nameof(MainViewModel.EffectsAvailable))
            // The note's Visibility is derived from EffectsAvailable; re-evaluate it when the probe
            // result lands (Text is bound directly to Vm.CapabilityDetail and updates on its own).
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(NotAvailableVisibility)));
        else if (e.PropertyName == nameof(MainViewModel.EyeContactAvailable))
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(EyeContactNotAvailableVisibility)));
        else if (e.PropertyName == nameof(MainViewModel.Locked))
            _overlay.SetLocked(Vm.Locked);
        else if (e.PropertyName == nameof(MainViewModel.ClickThrough))
            _overlay.SetClickThrough(Vm.ClickThrough);
        else if (e.PropertyName == nameof(MainViewModel.Mirror))
            _overlay.SetMirror(Vm.Mirror);
        else if (e.PropertyName == nameof(MainViewModel.Zoom))
            _overlay.SetZoom(Vm.Zoom);
    }
}
