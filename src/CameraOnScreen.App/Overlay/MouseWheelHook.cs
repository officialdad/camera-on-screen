using System;
using System.Runtime.InteropServices;
using static CameraOnScreen.App.Overlay.Interop;

namespace CameraOnScreen.App.Overlay;

/// <summary>
/// Global low-level mouse hook that surfaces wheel events to a callback. Installed on the UI thread
/// (which owns the message loop a WH_MOUSE_LL hook requires), so the callback runs on the UI thread
/// and may touch UI/overlay state directly. The callback receives the screen cursor point and the
/// signed notch count (one WHEEL_DELTA = one notch) and returns true to SWALLOW the wheel event
/// (so the app under the cursor does not also scroll).
/// </summary>
internal sealed class MouseWheelHook : IDisposable
{
    private readonly Func<POINT, int, bool> _onWheel;
    private readonly LowLevelMouseProc _proc; // keep the delegate alive for the hook's lifetime
    private IntPtr _hook;
    private bool _disposed;

    public MouseWheelHook(Func<POINT, int, bool> onWheel)
    {
        ArgumentNullException.ThrowIfNull(onWheel);
        _onWheel = onWheel;
        _proc = HookProc;
        // hMod = the .exe module handle; dwThreadId = 0 => global hook.
        _hook = SetWindowsHookEx(WH_MOUSE_LL, _proc, GetModuleHandle(null), 0);
        if (_hook == IntPtr.Zero)
            throw new InvalidOperationException(
                $"SetWindowsHookEx(WH_MOUSE_LL) failed with Win32 error {Marshal.GetLastWin32Error()}.");
    }

    private IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode == HC_ACTION && (int)wParam == (int)WM_MOUSEWHEEL)
        {
            var data = Marshal.PtrToStructure<MSLLHOOKSTRUCT>(lParam);
            int delta = (short)(data.mouseData >> 16); // HIWORD, signed
            int notches = delta / WHEEL_DELTA;
            if (notches != 0 && _onWheel(data.pt, notches))
                return (IntPtr)1; // handled — swallow so the window under the cursor doesn't scroll
        }
        return CallNextHookEx(_hook, nCode, wParam, lParam);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        if (_hook != IntPtr.Zero)
        {
            UnhookWindowsHookEx(_hook);
            _hook = IntPtr.Zero;
        }
        GC.SuppressFinalize(this);
    }
}
