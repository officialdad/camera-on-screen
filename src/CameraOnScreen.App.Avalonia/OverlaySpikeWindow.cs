using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;
using CameraOnScreen.Core.Native;
using System.Runtime.InteropServices;

namespace CameraOnScreen.App.Avalonia;

// Spike B (#31): minimal transparent Topmost window showing live shim frames, design
// candidate (a) from the Linux spec §5.2. Throwaway — launched via `--overlay-spike`.
// Human gate: OBS screen capture must show the video live with the margin transparent
// (desktop visible, not black). Prints present-fps to stdout for the handoff-cost check.
public sealed class OverlaySpikeWindow : Window
{
    private const int TransparentRing = 60; // transparent ring around the video = the transparency probe

    private readonly Image _image = new() { Stretch = Stretch.None,
        HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
    private readonly byte[] _buffer = new byte[1920 * 1080 * 4];
    private readonly DispatcherTimer _pump;
    private INativeShim? _shim;
    private WriteableBitmap? _bitmap;
    private int _frames;
    private long _lastFpsTick = Environment.TickCount64;

    public OverlaySpikeWindow()
    {
        SystemDecorations = SystemDecorations.None;
        Topmost = true;
        CanResize = false;
        ShowInTaskbar = false;
        TransparencyLevelHint = new[] { WindowTransparencyLevel.Transparent };
        Background = Brushes.Transparent;
        SizeToContent = SizeToContent.WidthAndHeight;
        Content = _image;

        try
        {
            var shim = new Native.PInvokeShim();
            shim.Init(IntPtr.Zero);
            var cams = shim.EnumerateCameras();
            if (cams.Count == 0) throw new InvalidOperationException("no camera");
            shim.SetParams(new ShimParams(cams[0].Id, false, 0, 0, false, 0, 0));
            shim.Start();
            _shim = shim;
        }
        catch (Exception e) when (e is DllNotFoundException or InvalidOperationException)
        {
            Console.WriteLine($"overlay-spike: no live camera ({e.Message}) — animated test pattern instead");
        }

        _pump = new DispatcherTimer(TimeSpan.FromMilliseconds(33), DispatcherPriority.Render, (_, _) => Present());
        _pump.Start();
        Closed += (_, _) => { _pump.Stop(); _shim?.Dispose(); };
    }

    private void Present()
    {
        int w, h;
        if (_shim is not null)
        {
            if (!_shim.TryGetFrame(_buffer, out w, out h)) return;
        }
        else
        {
            (w, h) = (640, 480);
            TestPattern(w, h);
        }

        if (_bitmap is null || _bitmap.PixelSize.Width != w || _bitmap.PixelSize.Height != h)
        {
            _bitmap = new WriteableBitmap(new PixelSize(w, h), new Vector(96, 96),
                PixelFormat.Bgra8888, AlphaFormat.Premul);
            _image.Source = _bitmap;
            _image.Margin = new Thickness(TransparentRing);
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

        _frames++;
        var now = Environment.TickCount64;
        if (now - _lastFpsTick >= 2000)
        {
            Console.WriteLine($"overlay-spike: present {_frames * 1000.0 / (now - _lastFpsTick):F1} fps");
            _frames = 0;
            _lastFpsTick = now;
        }
    }

    // Opaque moving color bands — motion proves "live", opaque alpha matches passthrough frames.
    private void TestPattern(int w, int h)
    {
        int t = (int)(Environment.TickCount64 / 16 % w);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int i = (y * w + x) * 4;
                byte v = (byte)((x + t) * 255 / w);
                _buffer[i] = v; _buffer[i + 1] = (byte)(255 - v); _buffer[i + 2] = 128; _buffer[i + 3] = 255;
            }
    }
}
