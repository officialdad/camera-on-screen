using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;
using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.Native;
using System.Runtime.InteropServices;
using CoreRect = CameraOnScreen.Core.Overlay.Rect;

namespace CameraOnScreen.App.Avalonia.Overlay;

// Linux overlay (Phase 3, #31): Spike B winner (a) — Avalonia transparent Topmost window
// presenting shim BGRA frames via a premultiplied WriteableBitmap. A separate window from
// the control panel, mirroring the Windows OverlayWindow split. The panel owns the shim;
// this window only reads frames. Drag = BeginMoveDrag (normal composited ARGB surface —
// the Windows MPO-plane hit-test failure does not apply). Wheel = OverlaySizer resize.
public sealed class OverlayWindow : Window
{
    private const int PumpMs = 33;     // camera rate
    private const int PumpFrucMs = 16; // ~60 Hz while FRUC double-publishes (mid + real frame)

    private readonly INativeShim _shim;
    private readonly Image _image = new() { Stretch = Stretch.Uniform };
    private readonly byte[] _buffer = new byte[1920 * 1080 * 4];
    private readonly DispatcherTimer _pump;
    private WriteableBitmap? _bitmap;

    public OverlayWindow(INativeShim shim, double x, double y, double w, double h, bool mirror,
                         HotkeyModifiers teleportChord = HotkeyModifiers.Control)
    {
        _shim = shim;
        _teleportMask = ToXModMask(teleportChord);
        Log($"overlay open chord={teleportChord} mask=0x{_teleportMask:X4}");
        SystemDecorations = SystemDecorations.None;
        Topmost = true;
        ShowInTaskbar = false;
        TransparencyLevelHint = new[] { WindowTransparencyLevel.Transparent };
        Background = Brushes.Transparent;
        Position = new PixelPoint((int)x, (int)y);
        Width = w;
        Height = h;
        Content = _image;
        SetMirror(mirror);

        _pump = new DispatcherTimer(TimeSpan.FromMilliseconds(PumpMs),
            DispatcherPriority.Render, (_, _) => { PollTeleport(); if (_dragging) PollDrag(); Present(); });
        _pump.Start();
        Closed += (_, _) => { _pump.Stop(); if (_dpy != IntPtr.Zero) { XCloseDisplay(_dpy); _dpy = IntPtr.Zero; } };

        // #40: KWin stacks a focused fullscreen window (slide share, fullscreen video) in a
        // layer ABOVE _NET_WM_STATE_ABOVE, so Topmost alone sinks on click. The KDE
        // critical-notification window type is the one client-settable layer above active
        // fullscreen. Set before AND after map (KWin reads the type at manage time and
        // tracks changes). Non-KDE WMs ignore the unknown atom and fall back to NORMAL,
        // where Topmost keeps today's behavior.
        SetAboveFullscreen();
        Opened += (_, _) => SetAboveFullscreen();
    }

    private void SetAboveFullscreen()
    {
        if (!OperatingSystem.IsLinux()) return;
        var handle = TryGetPlatformHandle();
        if (handle is null) return;
        IntPtr dpy = XOpenDisplay(IntPtr.Zero);
        if (dpy == IntPtr.Zero) return;
        try
        {
            var atoms = new[]
            {
                XInternAtom(dpy, "_KDE_NET_WM_WINDOW_TYPE_CRITICAL_NOTIFICATION", false),
                XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", false),
            };
            XChangeProperty(dpy, handle.Handle,
                XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", false),
                XInternAtom(dpy, "ATOM", false), 32, PropModeReplace, atoms, atoms.Length);
        }
        finally { XCloseDisplay(dpy); }
    }

    private const int PropModeReplace = 0;
    [DllImport("libX11.so.6")] private static extern IntPtr XOpenDisplay(IntPtr display);
    [DllImport("libX11.so.6")] private static extern IntPtr XInternAtom(IntPtr display, string name, bool onlyIfExists);
    [DllImport("libX11.so.6")] private static extern int XChangeProperty(IntPtr display, IntPtr window,
        IntPtr property, IntPtr type, int format, int mode, IntPtr[] data, int nelements);
    [DllImport("libX11.so.6")] private static extern int XCloseDisplay(IntPtr display);
    [DllImport("libX11.so.6")] private static extern IntPtr XDefaultRootWindow(IntPtr display);
    [DllImport("libX11.so.6")] private static extern bool XQueryPointer(IntPtr display, IntPtr window,
        out IntPtr root, out IntPtr child, out int rootX, out int rootY,
        out int winX, out int winY, out uint mask);

    public void SetMirror(bool mirror) =>
        _image.RenderTransform = mirror ? new ScaleTransform(-1, 1) : null;

    public void SetFrameInterp(bool on) =>
        _pump.Interval = TimeSpan.FromMilliseconds(on ? PumpFrucMs : PumpMs);

    private void Present()
    {
        if (!_shim.TryGetFrame(_buffer, out int w, out int h) || w <= 0) return;

        if (_bitmap is null || _bitmap.PixelSize.Width != w || _bitmap.PixelSize.Height != h)
        {
            _bitmap = new WriteableBitmap(new PixelSize(w, h), new Vector(96, 96),
                PixelFormat.Bgra8888, AlphaFormat.Premul);
            _image.Source = _bitmap;
        }

        using (var fb = _bitmap.Lock())
        {
            if (fb.RowBytes == w * 4)
                Marshal.Copy(_buffer, 0, fb.Address, w * h * 4);
            else
                for (int y = 0; y < h; y++)
                    Marshal.Copy(_buffer, y * w * 4, fb.Address + y * fb.RowBytes, w * 4);
        }
        _image.InvalidateVisual();
    }

    // App-side drag, not BeginMoveDrag: KWin refuses _NET_WM_MOVERESIZE on the
    // critical-notification layer (SetAboveFullscreen), so the WM won't move us — we move
    // ourselves. Same lesson as the Windows overlay (see CLAUDE.md): the drag MOVE is
    // polled on the frame-pump timer from ROOT pointer coords (XQueryPointer), never
    // derived from window-relative event coords — Position updates async from the WM, and
    // feeding its stale value back into the delta makes the window glide behind the
    // cursor. Pointer events only start/stop the drag.
    private bool _dragging;
    private int _grabDX, _grabDY;      // pointer(root) - window origin at press

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        if (e.GetCurrentPoint(this).Properties.IsLeftButtonPressed
            && TryGetRootPointer(out int rx, out int ry, out uint mask))
        {
            // Chord + click over the overlay itself is a teleport (#53), not a drag start —
            // letting both run would have PollDrag and PollTeleport fight over Position.
            if (_teleportMask != 0 && (mask & _teleportMask) == _teleportMask) return;
            _grabDX = rx - Position.X;
            _grabDY = ry - Position.Y;
            _dragging = true;
            e.Pointer.Capture(this);
        }
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        base.OnPointerReleased(e);
        _dragging = false;
    }

    private void PollDrag()
    {
        if (!TryGetRootPointer(out int rx, out int ry, out _)) { _dragging = false; return; }
        Position = new PixelPoint(rx - _grabDX, ry - _grabDY);
    }

    // Teleport (#53): hold the chord + left-click anywhere on screen and the overlay recenters on
    // the click point. XQueryPointer's mask already carries modifiers + buttons, so this rides the
    // existing pump poll — no new event plumbing. Latch = one jump per press, not one per tick.
    // ponytail: click is NOT swallowed (the app under the cursor also sees it); add a passive
    // XGrabButton on the root window if that ever bites.
    private readonly uint _teleportMask;   // X11 modifier mask of the chord; 0 = teleport disabled
    private bool _btn1Held;

    private const uint Button1Mask = 1u << 8;

    // Diagnostic log beside config.json (launcher runs with no terminal). One line per click
    // edge, not per tick — cheap enough to leave on.
    private static readonly string LogPath = System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "CameraOnScreen", "teleport.log");

    private static void Log(string msg)
    {
        try { System.IO.File.AppendAllText(LogPath, $"{DateTime.Now:HH:mm:ss.fff} {msg}\n"); }
        catch { /* logging must never break the pump */ }
    }

    private static uint ToXModMask(HotkeyModifiers m) =>
        (m.HasFlag(HotkeyModifiers.Shift) ? 1u : 0)            // ShiftMask
        | (m.HasFlag(HotkeyModifiers.Control) ? 1u << 2 : 0)   // ControlMask
        | (m.HasFlag(HotkeyModifiers.Alt) ? 1u << 3 : 0)       // Mod1Mask
        | (m.HasFlag(HotkeyModifiers.Win) ? 1u << 6 : 0);      // Mod4Mask

    private void PollTeleport()
    {
        if (!TryGetRootPointer(out int rx, out int ry, out uint mask)) { return; }
        bool btn1 = (mask & Button1Mask) != 0;
        bool chord = _teleportMask != 0 && (mask & _teleportMask) == _teleportMask;
        if (btn1 && !_btn1Held)
        {
            Log($"press root=({rx},{ry}) mask=0x{mask:X4} chordMask=0x{_teleportMask:X4} chord={chord} dragging={_dragging}");
            if (chord && !_dragging)
            {
                var wa = (Screens.ScreenFromPoint(new PixelPoint(rx, ry)) ?? Screens.Primary)?.WorkingArea
                         ?? new PixelRect(0, 0, 1920, 1080);
                var next = CameraOnScreen.Core.Overlay.OverlaySizer.CenterOn(
                    new CoreRect(Position.X, Position.Y, (int)Width, (int)Height), rx, ry,
                    new CoreRect(wa.X, wa.Y, wa.Width, wa.Height));
                Log($"teleport ({Position.X},{Position.Y}) -> ({next.X},{next.Y}) wa=({wa.X},{wa.Y},{wa.Width},{wa.Height})");
                Position = new PixelPoint(next.X, next.Y);
            }
        }
        _btn1Held = btn1;
    }

    private IntPtr _dpy;   // persistent Xlib connection for pointer polling; closed with the window

    private bool TryGetRootPointer(out int rootX, out int rootY, out uint mask)
    {
        rootX = rootY = 0;
        mask = 0;
        if (!OperatingSystem.IsLinux()) return false;
        if (_dpy == IntPtr.Zero) _dpy = XOpenDisplay(IntPtr.Zero);
        if (_dpy == IntPtr.Zero) return false;
        return XQueryPointer(_dpy, XDefaultRootWindow(_dpy), out _, out _,
                             out rootX, out rootY, out _, out _, out mask);
    }

    private double _wheelAccum;

    protected override void OnPointerWheelChanged(PointerWheelEventArgs e)
    {
        base.OnPointerWheelChanged(e);
        // Hi-res wheels (libinput smooth scrolling) deliver fractional deltas well below 1
        // per tick — accumulate until a whole notch lands instead of rounding each event.
        _wheelAccum += e.Delta.Y;
        int notches = (int)_wheelAccum;
        if (notches == 0) return;
        _wheelAccum -= notches;
        var wa = Screens.ScreenFromWindow(this)?.WorkingArea
                 ?? Screens.Primary?.WorkingArea
                 ?? new PixelRect(0, 0, 1920, 1080);
        var next = CameraOnScreen.Core.Overlay.OverlaySizer.Resize(
            new CoreRect(Position.X, Position.Y, (int)Width, (int)Height), notches,
            new CoreRect(wa.X, wa.Y, wa.Width, wa.Height));
        Position = new PixelPoint(next.X, next.Y);
        Width = next.W;
        Height = next.H;
        e.Handled = true;
    }
}
