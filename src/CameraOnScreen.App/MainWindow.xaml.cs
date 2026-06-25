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
    // Pre-sized to 4K (3840x2160) BGRA so Super Resolution (up to 2x of 1080p) fits without a
    // resize. TryGetFrame writes the actual size; cos_get_frame rejects frames larger than this.
    private readonly byte[] _frameBuf = new byte[3840 * 2160 * 4];
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _timer;
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _saveTimer; // debounces persist after wheel-resize
    private Overlay.OverlayMouseHook? _mouseHook;
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
            // Handle drag is polled here (not in the mouse hook) to avoid a synthesized-move feedback
            // loop: follow the live cursor at the captured grab offset, preserving the current size.
            if (_dragging && Overlay.Interop.GetCursorPos(out var cur))
            {
                var (_, _, ow, oh) = _overlay.GetBounds();
                _overlay.SetBounds(cur.x - _dragGrabDX, cur.y - _dragGrabDY, ow, oh);
            }
            Vm.PollStatusTick();
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
        _store.Save(Vm.ToAppConfig(x, y, w, h));
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
