using System;
using System.Runtime.InteropServices;
using static CameraOnScreen.App.Overlay.Interop;

namespace CameraOnScreen.App.Overlay;

internal enum MouseEventKind { Wheel, LeftDown, Move, LeftUp }

/// <summary>
/// Global low-level mouse hook installed on the UI thread (its callback runs on the UI thread).
/// Surfaces wheel + left-button + move events to a single callback that returns true to SWALLOW the
/// event (so the app under the cursor does not also receive it). The overlay is a no-focus topmost
/// window whose video can sit on an MPO plane, so window hit-testing is unreliable; this hook sees
/// input before the OS routes it, which is how drag-over-video works.
/// </summary>
internal sealed class OverlayMouseHook : IDisposable
{
    // (kind, screenPoint, wheelNotches) => handled. wheelNotches is 0 for non-wheel events.
    private readonly Func<MouseEventKind, POINT, int, bool> _onMouse;
    private readonly LowLevelMouseProc _proc;
    private IntPtr _hook;
    private bool _disposed;

    public OverlayMouseHook(Func<MouseEventKind, POINT, int, bool> onMouse)
    {
        ArgumentNullException.ThrowIfNull(onMouse);
        _onMouse = onMouse;
        _proc = HookProc;
        _hook = SetWindowsHookEx(WH_MOUSE_LL, _proc, GetModuleHandle(null), 0);
        if (_hook == IntPtr.Zero)
            throw new InvalidOperationException(
                $"SetWindowsHookEx(WH_MOUSE_LL) failed with Win32 error {Marshal.GetLastWin32Error()}.");
    }

    private IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode == HC_ACTION)
        {
            var data = Marshal.PtrToStructure<MSLLHOOKSTRUCT>(lParam);
            switch ((int)wParam)
            {
                case 0x020A: // WM_MOUSEWHEEL
                    int notches = (short)(data.mouseData >> 16) / WHEEL_DELTA;
                    if (notches != 0 && _onMouse(MouseEventKind.Wheel, data.pt, notches)) return (IntPtr)1;
                    break;
                case 0x0201: // WM_LBUTTONDOWN
                    if (_onMouse(MouseEventKind.LeftDown, data.pt, 0)) return (IntPtr)1;
                    break;
                case 0x0200: // WM_MOUSEMOVE
                    if (_onMouse(MouseEventKind.Move, data.pt, 0)) return (IntPtr)1;
                    break;
                case 0x0202: // WM_LBUTTONUP
                    if (_onMouse(MouseEventKind.LeftUp, data.pt, 0)) return (IntPtr)1;
                    break;
            }
        }
        return CallNextHookEx(_hook, nCode, wParam, lParam);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        if (_hook != IntPtr.Zero) { UnhookWindowsHookEx(_hook); _hook = IntPtr.Zero; }
        GC.SuppressFinalize(this);
    }
}
