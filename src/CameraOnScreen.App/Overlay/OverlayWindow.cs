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
    private readonly IDXGISwapChain1 _swapChain;
    private readonly IDCompositionDevice _dcomp;
    private readonly IDCompositionTarget _target;
    private readonly IDCompositionVisual _visual;
    private static WndProc? _proc; // keep delegate alive

    // Cached upload texture (Dynamic, CPU-writable) for the incoming BGRA frame.
    private ID3D11Texture2D? _frameTex;
    private int _texW, _texH;
    // Current back-buffer (swap chain) size; starts at the ctor dimensions.
    private int _bufW, _bufH;

    public IntPtr D3DDevicePtr => _device.NativePointer;

    public OverlayWindow(int x, int y, int width, int height)
    {
        _proc = (h, m, w, l) => DefWindowProc(h, m, w, l);
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
    }

    public void Show() => ShowWindow(_hwnd, SW_SHOWNOACTIVATE);

    public void PresentTestPattern()
    {
        using var back = _swapChain.GetBuffer<ID3D11Texture2D>(0);
        using var rtv = _device.CreateRenderTargetView(back);
        _context.ClearRenderTargetView(rtv, new Vortice.Mathematics.Color4(0f, 0.4f, 1f, 0.5f));
        _swapChain.Present(1, PresentFlags.None);
    }

    /// <summary>
    /// Uploads a tightly-packed (width*4 per row) BGRA frame and blits it 1:1 into the back buffer.
    /// </summary>
    /// <remarks>
    /// CORRECTNESS: <see cref="ID3D11DeviceContext.CopyResource"/> requires identical dimensions and
    /// format for source and destination — it cannot scale. The overlay is created at a placeholder
    /// size (e.g. 320x240) but the first real camera frame may be a different native resolution
    /// (e.g. 640x480). So before the copy, if the back-buffer size != the incoming frame size we
    /// resize the swap chain (ResizeBuffers) AND the host window (SetWindowPos) to the frame's native
    /// resolution, making the back buffer match the frame 1:1. The first real frame therefore
    /// establishes the overlay at the camera's native resolution — acceptable for M2 passthrough.
    ///
    /// TASK 13 MUST INTRODUCE SCALING: when the user is allowed to resize the overlay away from the
    /// camera's native resolution (window size != frame size), a straight CopyResource is no longer
    /// valid. Task 13 must draw the frame through a textured quad (or apply a DComp visual transform)
    /// so the frame can be scaled into an arbitrarily-sized back buffer. Do NOT extend this method to
    /// "handle" arbitrary scale by snapping the window to native — that defeats user resize.
    /// </remarks>
    public void PresentFrame(byte[] bgra, int width, int height)
    {
        if (width <= 0 || height <= 0) return;

        // Match the back buffer (and window) to the frame so CopyResource is a valid same-size copy.
        if (_bufW != width || _bufH != height)
        {
            _swapChain.ResizeBuffers(0, (uint)width, (uint)height, Format.Unknown, SwapChainFlags.None);
            SetWindowPos(_hwnd, IntPtr.Zero, 0, 0, width, height,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            _bufW = width; _bufH = height;
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
        _swapChain.Present(1, PresentFlags.None);
    }

    public void Dispose()
    {
        _frameTex?.Dispose();
        _visual.Dispose(); _target.Dispose(); _dcomp.Dispose(); _swapChain.Dispose();
        _context.Dispose(); _device.Dispose();
        GC.SuppressFinalize(this); // CA1816: suppress finalizer even though none exists (sealed)
    }
}
