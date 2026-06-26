using System.ComponentModel;
using System.Runtime.InteropServices;
using CameraOnScreen.App.Composition;
using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.ViewModels;
using Microsoft.UI.Xaml;

namespace CameraOnScreen.App;

public sealed partial class MainWindow : Window, INotifyPropertyChanged
{
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);

    public MainViewModel Vm { get; }

    private readonly Overlay.OverlayWindow _overlay;
    private readonly Hotkeys.GlobalHotkeyService _hotkeys;
    private readonly JsonSettingsStore _store = new(JsonSettingsStore.DefaultPath());
    // Pre-sized to 4K (3840x2160) BGRA so Super Resolution (up to 2x of 1080p) fits without a
    // resize. TryGetFrame writes the actual size; cos_get_frame rejects frames larger than this.
    private readonly byte[] _frameBuf = new byte[3840 * 2160 * 4];
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _timer;
    private int _topmostTick; // re-assert HWND_TOPMOST every ~1s (30 ticks @ 33ms) — see EnsureTopmost
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _saveTimer; // debounces persist after wheel-resize
    private Overlay.OverlayMouseHook? _mouseHook;
    private Microsoft.UI.Windowing.AppWindow? _appWindow; // this control-panel window, for size restore/persist
    // Last control-panel size (physical px) loaded from config; 0 => first launch (size to content).
    private readonly double _savedPanelW, _savedPanelH;
    // Hook-driven drag state (all touched on the UI thread inside the hook callback).
    private bool _dragging;
    // Cursor-minus-origin offset captured at drag start. The drag MOVE is driven by the frame-pump
    // timer (polling GetCursorPos), NOT by hook move events — calling SetBounds inside a WM_MOUSEMOVE
    // hook makes the window-move emit synthesized moves that re-enter the hook = a feedback loop that
    // pins the overlay (confirmed via instrumentation). Polling on the timer decouples it.
    private int _dragGrabDX, _dragGrabDY;

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
        _savedPanelW = config.PanelWidth;
        _savedPanelH = config.PanelHeight;

        // Build the overlay BEFORE the VM so its D3D device pointer exists when shim.Init runs.
        _overlay = new Overlay.OverlayWindow(x, y, w, h);
        _overlay.Show();
        Vm = Services.BuildViewModel(_overlay);
        Vm.PropertyChanged += OnVmPropertyChanged;
        // Apply the initial state loaded from config to the overlay.
        _overlay.SetMirror(Vm.Mirror);
        // ponytail: overlay stays always-interactive (no SetLocked/SetClickThrough) and unzoomed
        // (no SetZoom) — Lock/ClickThrough/Zoom were removed from the panel.

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
        // Default Windows title bar (system chrome). Cache the AppWindow for size restore/persist,
        // and persist (debounced) whenever the user resizes the panel.
        var panelHwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        _appWindow = Microsoft.UI.Windowing.AppWindow.GetFromWindowId(
            Microsoft.UI.Win32Interop.GetWindowIdFromWindow(panelHwnd));
        _appWindow.Changed += OnAppWindowChanged;
        RightSizePanel();

        // ~30 Hz frame pump on the WinUI UI thread: pull the latest BGRA frame from the shim and
        // blit it into the overlay, then refresh status. All D3D work happens here on the UI thread.
        _timer = DispatcherQueue.CreateTimer();
        _timer.Interval = TimeSpan.FromMilliseconds(33);
        _timer.Tick += (_, _) =>
        {
            if (Vm.IsRunning && Vm.ShimRef.TryGetFrame(_frameBuf, out int w, out int h) && w > 0)
                _overlay.PresentFrame(_frameBuf, w, h);
            // Handle drag is polled here (not in the mouse hook) to avoid a synthesized-move feedback
            // loop: follow the live cursor at the captured grab offset, preserving the current size.
            if (_dragging && Overlay.Interop.GetCursorPos(out var cur))
            {
                var (_, _, ow, oh) = _overlay.GetBounds();
                _overlay.SetBounds(cur.x - _dragGrabDX, cur.y - _dragGrabDY, ow, oh);
            }
            Vm.PollStatusTick();
            // Re-float topmost ~1 Hz so a fullscreen app (game/video/slideshow) that z-demotes the
            // overlay doesn't keep it sunk until an alt-tab. Cheap; SWP_NOACTIVATE never steals focus.
            if (++_topmostTick >= 30) { _topmostTick = 0; _overlay.EnsureTopmost(); }
        };
        _timer.Start();

        // Debounced persist for wheel-resize: the wheel has no "gesture end" event (unlike
        // WM_EXITSIZEMOVE), so coalesce rapid notches into a single Save() ~400ms after the last one.
        _saveTimer = DispatcherQueue.CreateTimer();
        _saveTimer.Interval = TimeSpan.FromMilliseconds(400);
        _saveTimer.IsRepeating = false;
        _saveTimer.Tick += (_, _) => Save();

        // Global mouse hook: resize the overlay on wheel, drag it by the handle on left-button.
        // The callback runs on the UI thread (the hook is installed here), so it touches the overlay
        // directly. Returning true swallows the event so the app under the cursor doesn't also see it.
        _mouseHook = new Overlay.OverlayMouseHook(OnMouse);

        // Probe effect availability OFF the UI thread (the real probe does a ~1s TensorRT model
        // load — running it in the ctor froze startup). Until it completes, EffectsAvailable is false
        // (toggles disabled) and the note shows "Checking effect availability…". `await` resumes on
        // the UI dispatcher, so the resulting OneWay binding updates happen on the UI thread.
        _ = Vm.ProbeCapabilitiesAsync();
    }

    // Restore the last panel size (physical px) so it reopens as the user left it. First launch
    // (no saved size): size to content once so every control is visible. ponytail: stored/restored
    // in physical pixels — a cross-DPI restore (different-scaling monitor between sessions) is
    // approximate; the user can resize and it re-persists. ScrollViewer is the shrink fallback.
    private void RightSizePanel()
    {
        if (_savedPanelW > 0 && _savedPanelH > 0)
        {
            _appWindow!.Resize(new Windows.Graphics.SizeInt32((int)_savedPanelW, (int)_savedPanelH));
            return;
        }
        RootGrid.Loaded += (_, _) =>
        {
            RootGrid.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
            var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
            double scale = GetDpiForWindow(hwnd) / 96.0;
            int w = (int)(RootGrid.DesiredSize.Width * scale);
            int h = (int)(RootGrid.DesiredSize.Height * scale);
            if (w > 0 && h > 0)
                _appWindow!.Resize(new Windows.Graphics.SizeInt32(w, h));
        };
    }

    // Persist the panel size when the user resizes it (debounced via _saveTimer, same as wheel-resize).
    // _saveTimer is created later in the ctor, so the startup Resize's notification no-ops via `?.`.
    private void OnAppWindowChanged(Microsoft.UI.Windowing.AppWindow sender,
        Microsoft.UI.Windowing.AppWindowChangedEventArgs args)
    {
        if (args.DidSizeChange) { _saveTimer?.Stop(); _saveTimer?.Start(); }
    }

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        // Stop the timer first so no WM_EXITSIZEMOVE-driven InteractionEnded can race with Save().
        _timer?.Stop();
        _timer = null;
        // Unhook the global mouse hook and flush any pending debounced save before teardown, so no
        // hook callback fires into a disposed overlay.
        _mouseHook?.Dispose();
        _mouseHook = null;
        _saveTimer?.Stop();
        _saveTimer = null;
        // Unsubscribe both event sources BEFORE saving so a late WM_EXITSIZEMOVE cannot trigger a
        // double-save while teardown is in progress.
        _overlay.InteractionEnded -= Save;
        _overlay.HotkeyPressed -= _hotkeys.OnHotkeyMessage;
        if (_appWindow != null) _appWindow.Changed -= OnAppWindowChanged;
        Save(); // final persist with the closing geometry/state + panel size
        _hotkeys.Dispose(); // unregister all hotkeys
        Vm.PropertyChanged -= OnVmPropertyChanged;
        Vm.Dispose();
        _overlay.Dispose();
    }

    // Choose startup geometry: restore saved bounds only when the saved size is usable (>0);
    // otherwise fall back to defaults. Win32 ctor wants ints, so cast the double settings down.
    private static (int x, int y, int w, int h) ResolveStartupBounds(OverlaySettings s)
    {
        if (s.Width <= 0 || s.Height <= 0)
            return (DefaultX, DefaultY, DefaultW, DefaultH);
        int x = (int)s.X, y = (int)s.Y, w = (int)s.Width, h = (int)s.Height;
        // The saved display may be off/disconnected now, leaving the rect on no live monitor — the
        // overlay would restore off-screen (invisible until you reconnect that display). If the rect
        // intersects no monitor, drop the dead position back to defaults (on primary), keeping size.
        var rc = new Overlay.Interop.RECT { left = x, top = y, right = x + w, bottom = y + h };
        if (Overlay.Interop.MonitorFromRect(ref rc, Overlay.Interop.MONITOR_DEFAULTTONULL) == IntPtr.Zero)
            return (DefaultX, DefaultY, w, h);
        return (x, y, w, h);
    }

    // Map a global hotkey to a behavior. Hotkey messages can arrive off the UI thread, and these
    // toggles touch UI-bound VM observable props, so marshal onto the dispatcher.
    private void OnHotkeyAction(HotkeyAction action)
    {
        DispatcherQueue.TryEnqueue(() =>
        {
            switch (action)
            {
                case HotkeyAction.ToggleOverlayVisible: _overlay.ToggleVisible(); break;
                case HotkeyAction.ToggleRunning:
                    if (Vm.IsRunning) Vm.StopCommand?.Execute(null); else Vm.StartCommand?.Execute(null);
                    break;
                // ToggleLock/ToggleClickThrough: no-ops now (overlay always interactive); enum kept.
            }
        });
    }

    // Runs on the UI thread for every hooked mouse event. Returns true to swallow (so the app under
    // the cursor doesn't also get it). Wheel = resize; left-button on the handle = drag the overlay.
    private bool OnMouse(Overlay.MouseEventKind kind, Overlay.Interop.POINT pt, int notches)
    {
        switch (kind)
        {
            case Overlay.MouseEventKind.Wheel:
                if (_dragging) return true; // ignore+swallow wheel while a handle drag is in progress (avoids origin snap)
                if (!_overlay.IsInteractive) return false;
                var (wx, wy, ww, wh) = _overlay.GetBounds();
                bool inside = pt.x >= wx && pt.x < wx + ww && pt.y >= wy && pt.y < wy + wh;
                if (!inside) return false;
                var next = CameraOnScreen.Core.Overlay.OverlaySizer.Resize(
                    new CameraOnScreen.Core.Overlay.Rect(wx, wy, ww, wh), notches, GetWorkArea(_overlay.Hwnd));
                _overlay.SetBounds(next.X, next.Y, next.W, next.H);
                _saveTimer?.Stop(); _saveTimer?.Start();
                return true;

            case Overlay.MouseEventKind.LeftDown:
                if (_overlay.IsInteractive && _overlay.HitHandle(pt))
                {
                    _dragging = true;
                    var (ox, oy, _, _) = _overlay.GetBounds();
                    _dragGrabDX = pt.x - ox; _dragGrabDY = pt.y - oy;
                    return true; // swallow: begin handle drag (the frame-pump timer performs the move)
                }
                return false;

            case Overlay.MouseEventKind.Move:
                // Drag move is timer-driven; do NOT SetBounds in the hook (feedback loop). Just let
                // the cursor move freely (return false). When not dragging, drive handle visibility.
                if (_dragging) return false;
                // Handle visibility: show on hover over the overlay when interactive (hook-driven,
                // because window mouse messages are unreliable over the MPO plane).
                var b = _overlay.GetBounds();
                bool over = _overlay.IsInteractive
                    && pt.x >= b.x && pt.x < b.x + b.w && pt.y >= b.y && pt.y < b.y + b.h;
                _overlay.SetHandleVisible(over);
                return false;

            case Overlay.MouseEventKind.LeftUp:
                if (_dragging)
                {
                    _dragging = false;
                    _saveTimer?.Stop(); _saveTimer?.Start(); // debounced persist of new position
                    return true;
                }
                return false;
        }
        return false;
    }

    // Work area (monitor minus taskbar) of the monitor the overlay is on, as an OverlaySizer.Rect.
    private static CameraOnScreen.Core.Overlay.Rect GetWorkArea(IntPtr hwnd)
    {
        IntPtr mon = Overlay.Interop.MonitorFromWindow(hwnd, Overlay.Interop.MONITOR_DEFAULTTONEAREST);
        var mi = new Overlay.Interop.MONITORINFO { cbSize = Marshal.SizeOf<Overlay.Interop.MONITORINFO>() };
        if (!Overlay.Interop.GetMonitorInfo(mon, ref mi))
            // GetMonitorInfo failure (unreachable for a valid HWND) -> un-clamped fallback, never a 0x0 collapse.
            return new CameraOnScreen.Core.Overlay.Rect(0, 0, int.MaxValue, int.MaxValue);
        var r = mi.rcWork;
        return new CameraOnScreen.Core.Overlay.Rect(r.left, r.top, r.right - r.left, r.bottom - r.top);
    }

    // Persist current VM state + live overlay geometry. Called on drag/resize END and on close —
    // NOT on every WM_MOVE/WM_SIZE, which would thrash the disk during a gesture.
    private void Save()
    {
        var (x, y, w, h) = _overlay.GetBounds();
        var cfg = Vm.ToAppConfig(x, y, w, h);
        // Also persist the control-panel window size (physical px) so it restores on next launch.
        var size = _appWindow?.Size ?? default;
        if (size.Width > 0 && size.Height > 0)
            cfg = cfg with { PanelWidth = size.Width, PanelHeight = size.Height };
        _store.Save(cfg);
    }

    public Visibility NotAvailableVisibility =>
        Vm.EffectsAvailable ? Visibility.Collapsed : Visibility.Visible;

    public Visibility EyeContactNotAvailableVisibility =>
        Vm.EyeContactAvailable ? Visibility.Collapsed : Visibility.Visible;

    // SR sub-controls gate on the probe AND the chosen mode. x:Bind re-runs these when either
    // argument (SuperResAvailable / SuperResModeIndex) raises PropertyChanged. Mode 0 = Off, 1 = Upscale.
    public static bool QualityEnabled(bool available, int modeIndex) => available && modeIndex != 0;

    // Exposure slider is live only when the camera supports manual exposure AND the lock is on.
    // x:Bind re-runs this when either ExposureSupported or ExposureLock raises PropertyChanged.
    public static bool ExposureSliderEnabled(bool supported, bool locked) => supported && locked;

    public string StatusLine =>
        Vm.IsRunning ? $"Running — {Vm.Fps:F0} fps" : "Stopped";

    // Required NVIDIA Maxine SDK attribution (Supplement §3.1), shown in the panel footer.
    public string MaxineAttribution => CameraOnScreen.Core.AppInfo.MaxineAttribution;

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
        else if (e.PropertyName == nameof(MainViewModel.Mirror))
            _overlay.SetMirror(Vm.Mirror);
        // Locked/ClickThrough/Zoom branches removed — those props no longer exist.
    }
}
