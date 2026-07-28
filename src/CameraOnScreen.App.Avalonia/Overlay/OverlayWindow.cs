using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;
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

    public OverlayWindow(INativeShim shim, double x, double y, double w, double h, bool mirror)
    {
        _shim = shim;
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
            DispatcherPriority.Render, (_, _) => Present());
        _pump.Start();
        Closed += (_, _) => _pump.Stop();
    }

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

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        if (e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
            BeginMoveDrag(e);
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
