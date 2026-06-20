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

    public IntPtr D3DDevicePtr => _device.NativePointer;

    public OverlayWindow(int x, int y, int width, int height)
    {
        _proc = (h, m, w, l) => DefWindowProc(h, m, w, l);
        var wc = new WNDCLASSEX
        {
            cbSize = System.Runtime.InteropServices.Marshal.SizeOf<WNDCLASSEX>(),
            lpfnWndProc = System.Runtime.InteropServices.Marshal.GetFunctionPointerForDelegate(_proc),
            hInstance = GetModuleHandle(null),
            lpszClassName = "CosOverlay"
        };
        RegisterClassEx(ref wc);

        _hwnd = CreateWindowEx(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
            "CosOverlay", "CameraOnScreen Overlay", WS_POPUP,
            x, y, width, height, IntPtr.Zero, IntPtr.Zero, wc.hInstance, IntPtr.Zero);

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
        _swapChain = factory.CreateSwapChainForComposition(_device, desc);

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

    public void Dispose()
    {
        _visual.Dispose(); _target.Dispose(); _dcomp.Dispose(); _swapChain.Dispose();
        _context.Dispose(); _device.Dispose();
    }
}
