using System.Numerics;
using System.Runtime.InteropServices;
using SharpGen.Runtime;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DirectComposition;
using Vortice.DXGI;
using static CameraOnScreen.App.Overlay.Interop;

namespace CameraOnScreen.App.Overlay;

public sealed class OverlayWindow : IDisposable
{
    private readonly IntPtr _hwnd;
    private readonly ID3D11Device _device;
    private readonly ID3D11DeviceContext _context;
    private readonly ID3D11DeviceContext1 _context1; // ClearView (sub-region fill, used for the drag handle) lives here.
    private readonly IDXGISwapChain1 _swapChain;
    private readonly IDCompositionDevice _dcomp;
    private readonly IDCompositionTarget _target;
    private readonly IDCompositionVisual _visual;
    private static WndProc? _proc; // keep delegate alive

    // Single-overlay design: the static window proc routes messages to the one live instance.
    private static OverlayWindow? _instance;

    // Cached upload texture (Dynamic, CPU-writable) for the incoming BGRA frame.
    private ID3D11Texture2D? _frameTex;
    private int _texW, _texH;
    // Swap-chain (back-buffer) size. CRITICAL: this is ALWAYS the camera frame resolution so the
    // 1:1 CopyResource stays valid. It is DECOUPLED from the window size — the DComp visual scale
    // transform stretches the frame-res content to fill whatever size the window currently is.
    private int _bufW, _bufH;

    // Interaction state (Task 13).
    private bool _locked;     // when true: no drag/resize and no chrome (clean capture).
    private bool _clickThrough; // mirrors WS_EX_TRANSPARENT; gates wheel-resize (Task: wheel-resize).
    private bool _handleVisible; // host (hook hover) toggles this; gates the drawn handle.

    // Drag handle: a small "+" crosshair at the overlay CENTER. The HIT area (fixed SCREEN pixels,
    // generous so it's easy to grab) is centered via the live window rect (zoom-independent); the
    // crosshair is DRAWN into the back buffer (see DrawHandle), so content zoom>1 shifts the drawn
    // glyph vs the fixed hit rect (v1 caveat) — mirror is fine (the glyph is symmetric).
    private const int HandleHitW = 44, HandleHitH = 44;       // hit-test square, screen px (generous grab)
    private const int CrossArmPx = 6, CrossThickPx = 2;       // visible "+" half-arm + thickness, screen px

    // Presentation transform state (Bucket 2). Mirror flips horizontally about the window centre;
    // zoom (1.0–3.0) scales about the centre so the window edges crop the overflow = tighter framing.
    // Both fold into the single DComp visual transform in UpdateScale — the swap chain is untouched.
    private bool _mirror;
    private double _zoom = 1.0;

    // Visibility state for ToggleVisible (Task 14). Starts true: the window is shown at startup.
    private bool _visible = true;
    private bool _disposed;

    /// <summary>
    /// User finished a drag/resize (WM_EXITSIZEMOVE). Fires ONCE per interaction — the cue to
    /// persist geometry, debounced (it does not fire continuously during the gesture).
    /// </summary>
    public event Action? InteractionEnded;
    /// <summary>
    /// A registered global hotkey fired (WM_HOTKEY). Carries the registration id (wParam). The
    /// host routes this into <c>GlobalHotkeyService.OnHotkeyMessage</c> to decouple the service
    /// from the window proc.
    /// </summary>
    public event Action<int>? HotkeyPressed;

    public IntPtr Hwnd => _hwnd;
    public IntPtr D3DDevicePtr => _device.NativePointer;

    public OverlayWindow(int x, int y, int width, int height)
    {
        _proc = WndProcImpl;
        var wc = new WNDCLASSEX
        {
            cbSize = Marshal.SizeOf<WNDCLASSEX>(),
            lpfnWndProc = Marshal.GetFunctionPointerForDelegate(_proc),
            hInstance = GetModuleHandle(null),
            lpszClassName = "CosOverlay"
        };
        // Single shared class name "CosOverlay" — idempotent: 1410 (ERROR_CLASS_ALREADY_EXISTS)
        // is not an error; it means a prior instance already registered the class, which is fine.
        ushort atom = RegisterClassEx(ref wc);
        if (atom == 0)
        {
            int err = Marshal.GetLastWin32Error();
            if (err != 1410) // ERROR_CLASS_ALREADY_EXISTS
                throw new InvalidOperationException($"RegisterClassEx failed with Win32 error {err}.");
        }

        _hwnd = CreateWindowEx(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
            "CosOverlay", "CameraOnScreen Overlay", WS_POPUP,
            x, y, width, height, IntPtr.Zero, IntPtr.Zero, wc.hInstance, IntPtr.Zero);
        if (_hwnd == IntPtr.Zero)
            throw new InvalidOperationException($"CreateWindowEx failed with Win32 error {Marshal.GetLastWin32Error()}.");

        D3D11.D3D11CreateDevice(null, DriverType.Hardware, DeviceCreationFlags.BgraSupport,
            null!, out _device!, out _context!).CheckError();
        // ClearView (sub-region RTV fill, used for the drag handle) is on ID3D11DeviceContext1.
        _context1 = _context.QueryInterface<ID3D11DeviceContext1>();

        using var dxgi = _device.QueryInterface<IDXGIDevice>();
        using var factory = DXGI.CreateDXGIFactory2<IDXGIFactory2>(false);
        var desc = new SwapChainDescription1
        {
            Width = (uint)width, Height = (uint)height, Format = Format.B8G8R8A8_UNorm,
            BufferCount = 2, BufferUsage = Usage.RenderTargetOutput,
            SwapEffect = SwapEffect.FlipSequential, AlphaMode = AlphaMode.Premultiplied,
            SampleDescription = new SampleDescription(1, 0)
        };
        // Vortice 3.8.3: IDXGIFactory2.CreateSwapChainForComposition returns IDXGISwapChain1
        // directly and throws a SharpGenException on HRESULT failure — no CheckError() needed.
        _swapChain = factory.CreateSwapChainForComposition(_device, desc);
        _bufW = width; _bufH = height;

        DComp.DCompositionCreateDevice(dxgi, out _dcomp!).CheckError();
        _dcomp.CreateTargetForHwnd(_hwnd, topmost: true, out _target!).CheckError();
        _visual = _dcomp.CreateVisual();
        _visual.SetContent(_swapChain);
        _target.SetRoot(_visual);
        _dcomp.Commit();

        // Route the static proc to this instance LAST, after all DComp fields are initialised,
        // so any early WM_SIZE that arrives before construction is complete cannot
        // route into WndProcImpl before _visual/_dcomp exist (single-overlay design).
        _instance = this;
    }

    public void Show() => ShowWindow(_hwnd, SW_SHOWNOACTIVATE);

    /// <summary>
    /// Toggle overlay visibility (Task 14, ToggleOverlayVisible hotkey). Hides via SW_HIDE, shows
    /// via SW_SHOWNOACTIVATE so showing never steals focus from the foreground app.
    /// </summary>
    public void ToggleVisible()
    {
        if (_disposed) return;
        _visible = !_visible;
        ShowWindow(_hwnd, _visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    }

    // ---- Task 13 public API ---------------------------------------------------------------

    /// <summary>Lock disables drag/resize and hides chrome.</summary>
    public void SetLocked(bool locked) => _locked = locked;

    /// <summary>Toggle WS_EX_TRANSPARENT so mouse input passes through to windows beneath.</summary>
    public void SetClickThrough(bool on)
    {
        _clickThrough = on;
        int ex = GetWindowLong(_hwnd, GWL_EXSTYLE);
        ex = on ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
        SetWindowLong(_hwnd, GWL_EXSTYLE, ex);
        // When click-through is enabled, clear the handle so no chrome is drawn while the overlay
        // is click-through.
        if (on)
        {
            _handleVisible = false;
        }
    }

    /// <summary>Horizontal mirror (selfie view). Presentation-only; re-applies the visual transform live.</summary>
    public void SetMirror(bool on)
    {
        if (_disposed || _mirror == on) return;
        _mirror = on;
        GetClientRect(_hwnd, out RECT rc);
        UpdateScale(rc.right - rc.left, rc.bottom - rc.top);
    }

    /// <summary>Center zoom, clamped 1.0–3.0. Window edges crop the overflow = tighter framing. Live.</summary>
    public void SetZoom(double zoom)
    {
        zoom = Math.Clamp(zoom, 1.0, 3.0);
        if (_disposed || _zoom == zoom) return;
        _zoom = zoom;
        GetClientRect(_hwnd, out RECT rc);
        UpdateScale(rc.right - rc.left, rc.bottom - rc.top);
    }

    /// <summary>
    /// True only when the overlay accepts size gestures: not locked and not click-through.
    /// Wheel-resize is gated on this (Lock freezes the overlay for clean capture; click-through
    /// means the overlay takes no pointer input by design).
    /// </summary>
    public bool IsInteractive => !_locked && !_clickThrough;

    /// <summary>
    /// Move+resize the overlay window. Uses SWP_NOACTIVATE | SWP_NOZORDER so the overlay never
    /// steals focus and stays topmost. The resulting WM_SIZE routes to UpdateScale, which
    /// re-stretches the frame-res content to the new window size — no extra redraw needed.
    /// </summary>
    public void SetBounds(int x, int y, int w, int h)
    {
        if (_disposed) return;
        SetWindowPos(_hwnd, IntPtr.Zero, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER);
    }

    /// <summary>Current window rectangle (screen coords) as (x, y, width, height).</summary>
    public (int x, int y, int w, int h) GetBounds()
    {
        GetWindowRect(_hwnd, out RECT r);
        return (r.left, r.top, r.right - r.left, r.bottom - r.top);
    }

    /// <summary>Host toggles handle visibility (driven by the hook's hover detection).</summary>
    public void SetHandleVisible(bool visible) => _handleVisible = visible;

    /// <summary>True if a SCREEN point is inside the drag handle's fixed centered hit rect.</summary>
    internal bool HitHandle(POINT screenPt)
    {
        if (_disposed) return false;
        GetWindowRect(_hwnd, out RECT w);
        int winW = w.right - w.left, winH = w.bottom - w.top;
        int hx = w.left + (winW - HandleHitW) / 2;
        int hy = w.top + (winH - HandleHitH) / 2;
        return screenPt.x >= hx && screenPt.x < hx + HandleHitW
            && screenPt.y >= hy && screenPt.y < hy + HandleHitH;
    }

    // ---- Window proc ----------------------------------------------------------------------

    private static IntPtr WndProcImpl(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam)
    {
        var self = _instance;
        if (self is not null && self._hwnd == hwnd)
        {
            switch (msg)
            {
                case WM_SIZE:
                    self.OnSize(LoWord(lParam), HiWord(lParam));
                    break;
                case WM_EXITSIZEMOVE:
                    // Drag/resize finished — single, debounced cue for the host to persist geometry.
                    self.InteractionEnded?.Invoke();
                    break;
                case WM_HOTKEY:
                    // RegisterHotKey targets this HWND, so global hotkeys land here. wParam is the
                    // registration id; the host forwards it to GlobalHotkeyService.OnHotkeyMessage.
                    self.HotkeyPressed?.Invoke((int)wParam);
                    break;
            }
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    // WM_SIZE: window size is decoupled from the frame-res back buffer. Scale the DComp visual so
    // native-res content fills the new client area. The swap chain is NEVER resized here.
    private void OnSize(int clientW, int clientH) => UpdateScale(clientW, clientH);

    // Apply scaleX = clientW/frameW, scaleY = clientH/frameH to the visual, then commit.
    private void UpdateScale(int clientW, int clientH)
    {
        if (_bufW <= 0 || _bufH <= 0 || clientW <= 0 || clientH <= 0) return;
        float sx = clientW / (float)_bufW;
        float sy = clientH / (float)_bufH;
        var center = new Vector2(clientW / 2f, clientH / 2f);
        // Fit frame-res content to the window, then flip and zoom about the window centre.
        var m = Matrix3x2.CreateScale(sx, sy);
        if (_mirror)
            m *= Matrix3x2.CreateScale(-1f, 1f, center);
        if (_zoom != 1.0)
            m *= Matrix3x2.CreateScale((float)_zoom, center);
        _visual.SetTransform(m);
        _dcomp.Commit();
    }

    /// <summary>
    /// Uploads a tightly-packed (width*4 per row) BGRA frame and blits it 1:1 into the back buffer.
    /// </summary>
    /// <remarks>
    /// CORRECTNESS: <see cref="ID3D11DeviceContext.CopyResource"/> requires identical dimensions and
    /// format for source and destination — it cannot scale. The swap chain is therefore PINNED to the
    /// camera's native frame resolution: the first real frame (re)sizes the back buffer to the frame
    /// once, and from then on CopyResource(back, frameTex) is always a valid same-size copy.
    ///
    /// USER RESIZE (wheel) and MOVE (top-center drag handle) do NOT change the swap chain. Wheel
    /// resize changes only the HWND size; WM_SIZE then applies a DirectComposition visual SCALE
    /// transform that stretches the frame-res content to fill the window. So window size and
    /// swap-chain size are decoupled and the 1:1 CopyResource remains valid at any window size —
    /// see <see cref="UpdateScale"/>.
    /// </remarks>
    public void PresentFrame(byte[] bgra, int width, int height)
    {
        if (width <= 0 || height <= 0) return;

        // Pin the back buffer to the FRAME size so CopyResource is a valid same-size copy. This only
        // ever fires when the camera's native resolution changes (typically once). It does NOT track
        // the window size — user resize is handled by the DComp scale transform.
        if (_bufW != width || _bufH != height)
        {
            _swapChain.ResizeBuffers(0, (uint)width, (uint)height, Format.Unknown, SwapChainFlags.None);
            _bufW = width; _bufH = height;
            // Re-establish the scale for the current window size against the new frame size.
            GetClientRect(_hwnd, out RECT rc);
            UpdateScale(rc.right - rc.left, rc.bottom - rc.top);
        }

        // (Re)create the CPU-writable upload texture when the frame size changes.
        if (_frameTex is null || _texW != width || _texH != height)
        {
            _frameTex?.Dispose();
            _frameTex = _device.CreateTexture2D(new Texture2DDescription
            {
                Width = (uint)width, Height = (uint)height, MipLevels = 1, ArraySize = 1,
                Format = Format.B8G8R8A8_UNorm, SampleDescription = new SampleDescription(1, 0),
                Usage = ResourceUsage.Dynamic, BindFlags = BindFlags.ShaderResource,
                CPUAccessFlags = CpuAccessFlags.Write
            });
            _texW = width; _texH = height;
        }

        // Upload row-by-row honoring the texture's RowPitch (may exceed width*4); the source rows
        // are tightly packed at width*4 (native side already de-strided).
        var map = _context.Map(_frameTex, 0, MapMode.WriteDiscard, Vortice.Direct3D11.MapFlags.None);
        for (int row = 0; row < height; row++)
            Marshal.Copy(bgra, row * width * 4, map.DataPointer + row * (int)map.RowPitch, width * 4);
        _context.Unmap(_frameTex, 0);

        using var back = _swapChain.GetBuffer<ID3D11Texture2D>(0);
        _context.CopyResource(back, _frameTex); // valid: back buffer == frame size (see remarks)

        // Clean-capture chrome: draw the drag handle ONLY when handle is visible AND unlocked.
        // When locked or handle is hidden, nothing is drawn so recordings stay clean.
        if (_handleVisible && !_locked)
            DrawHandle(back, width, height);

        _swapChain.Present(1, PresentFlags.None);
    }

    // Draw the drag handle (semi-opaque pill + move-cross) into the frame-res back buffer, sized
    // from the current client->buffer scale so it lands at ~HandleScreenW×HandleScreenH on screen at
    // the default transform. Premultiplied alpha: RGB is pre-scaled by A.
    private void DrawHandle(ID3D11Texture2D back, int frameW, int frameH)
    {
        GetClientRect(_hwnd, out RECT rc);
        int clientW = rc.right, clientH = rc.bottom;
        if (clientW <= 0 || clientH <= 0) return;
        float ux = frameW / (float)clientW, uy = frameH / (float)clientH; // frame px per screen px
        int cx = frameW / 2, cy = frameH / 2;                             // overlay centre, back-buffer px
        int armX = Math.Max(2, (int)(CrossArmPx * ux)), armY = Math.Max(2, (int)(CrossArmPx * uy));
        int tX = Math.Max(1, (int)(CrossThickPx * ux)), tY = Math.Max(1, (int)(CrossThickPx * uy));
        using var rtv = _device.CreateRenderTargetView(back);
        // White "+" crosshair @ 0.45 alpha, premultiplied -> (0.45,0.45,0.45,0.45). No background box.
        var white = new Vortice.Mathematics.Color4(0.45f, 0.45f, 0.45f, 0.45f);
        // Vertical bar (half-width tX, half-height armY) + horizontal bar (half-width armX, half-height tY).
        _context1.ClearView(rtv, white, new[] { new Vortice.RawRect(cx - tX, cy - armY, cx + tX, cy + armY) });
        _context1.ClearView(rtv, white, new[] { new Vortice.RawRect(cx - armX, cy - tY, cx + armX, cy + tY) });
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        if (_instance == this) _instance = null;
        _frameTex?.Dispose();
        _visual.Dispose(); _target.Dispose(); _dcomp.Dispose(); _swapChain.Dispose();
        _context1.Dispose(); _context.Dispose(); _device.Dispose();
        GC.SuppressFinalize(this); // CA1816: suppress finalizer even though none exists (sealed)
    }

    // lParam packs two 16-bit signed coords (LOWORD=x, HIWORD=y). Use signed casts so points on
    // multi-monitor setups with negative coordinates convert correctly.
    private static int LoWord(IntPtr v) => unchecked((short)(long)v);
    private static int HiWord(IntPtr v) => unchecked((short)((long)v >> 16));
}
