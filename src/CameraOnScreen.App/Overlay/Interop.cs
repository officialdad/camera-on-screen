using System.Runtime.InteropServices;

namespace CameraOnScreen.App.Overlay;

internal static class Interop
{
    public const int WS_POPUP = unchecked((int)0x80000000);
    public const int WS_EX_LAYERED = 0x00080000;
    public const int WS_EX_TOPMOST = 0x00000008;
    public const int WS_EX_NOREDIRECTIONBITMAP = 0x00200000;
    public const int WS_EX_TRANSPARENT = 0x00000020;
    // Tool window: keeps the overlay out of the alt-tab switcher and taskbar (it's chrome, not an app).
    public const int WS_EX_TOOLWINDOW = 0x00000080;

    // CharSet.Unicode is REQUIRED here: RegisterClassEx/CreateWindowEx are declared CharSet.Unicode
    // (-> the W exports), but DllImport's CharSet does NOT control how a by-ref struct's string
    // fields marshal. Without this, lpszClassName defaults to ANSI marshalling, so RegisterClassExW
    // reads a mismatched name and CreateWindowExW then fails with Win32 1407 (CANNOT_FIND_WND_CLASS).
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct WNDCLASSEX
    {
        public int cbSize; public int style; public IntPtr lpfnWndProc;
        public int cbClsExtra; public int cbWndExtra; public IntPtr hInstance;
        public IntPtr hIcon; public IntPtr hCursor; public IntPtr hbrBackground;
        public string? lpszMenuName; public string lpszClassName; public IntPtr hIconSm;
    }

    public delegate IntPtr WndProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    // Window messages handled by the overlay proc.
    public const uint WM_SIZE = 0x0005;
    // WM_EXITSIZEMOVE fires ONCE when the user finishes a drag/resize: the cue to
    // persist geometry without thrashing the disk on every live WM_SIZE pixel update.
    public const uint WM_EXITSIZEMOVE = 0x0232;
    // WM_HOTKEY: RegisterHotKey targets the overlay HWND, so the hotkey message arrives
    // at this proc; wParam carries the registration id.
    public const uint WM_HOTKEY = 0x0312;

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int left, top, right, bottom; }

    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int x, y; }

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern ushort RegisterClassEx(ref WNDCLASSEX lpwcx);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateWindowEx(int exStyle, string className, string windowName,
        int style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);

    [DllImport("user32.dll")] public static extern IntPtr DefWindowProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int idx);
    [DllImport("user32.dll")] public static extern int SetWindowLong(IntPtr h, int idx, int val);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter,
        int x, int y, int cx, int cy, uint flags);
    public const int GWL_EXSTYLE = -20;
    public const int SW_HIDE = 0;
    public const int SW_SHOWNOACTIVATE = 4;
    // SetWindowPos flags: keep current Z-order/activation, only change position+size.
    public const uint SWP_NOSIZE     = 0x0001;
    public const uint SWP_NOMOVE     = 0x0002;
    public const uint SWP_NOZORDER   = 0x0004;
    public const uint SWP_NOACTIVATE = 0x0010;
    // Re-float a window that keeps WS_EX_TOPMOST style but was z-demoted (fullscreen app, etc.).
    public static readonly IntPtr HWND_TOPMOST = new(-1);
    [DllImport("kernel32.dll")] public static extern IntPtr GetModuleHandle(string? name);

    // ---- Low-level mouse hook (wheel-over-overlay resize) ------------------------------------
    // The overlay is SW_SHOWNOACTIVATE (never focused), so WM_MOUSEWHEEL never reaches its wndproc
    // — Windows routes the wheel to the FOCUSED window. A WH_MOUSE_LL hook sees every mouse event
    // globally before routing, so we use it to detect wheel-over-overlay and (optionally) swallow it.
    public const int WH_MOUSE_LL = 14;
    public const int HC_ACTION = 0;
    public const uint WM_MOUSEWHEEL = 0x020A;
    public const int WHEEL_DELTA = 120;

    [StructLayout(LayoutKind.Sequential)]
    public struct MSLLHOOKSTRUCT
    {
        public POINT pt;          // screen coordinates of the cursor
        public uint mouseData;    // for WM_MOUSEWHEEL: HIWORD is the signed wheel delta
        public uint flags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    public delegate IntPtr LowLevelMouseProc(int nCode, IntPtr wParam, IntPtr lParam);

    // Current cursor position in SCREEN pixels. Used to drive handle-drag from the frame-pump timer
    // (polling) instead of from the hook's move events — moving the window inside a WM_MOUSEMOVE hook
    // creates a synthesized-move feedback loop, so the drag is decoupled onto the timer.
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT lpPoint);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr SetWindowsHookEx(int idHook, LowLevelMouseProc lpfn, IntPtr hMod, uint dwThreadId);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool UnhookWindowsHookEx(IntPtr hhk);

    [DllImport("user32.dll")]
    public static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);

    // ---- Monitor work area (clamp wheel-resize to the overlay's monitor) ---------------------
    public const uint MONITOR_DEFAULTTONEAREST = 0x00000002;

    [StructLayout(LayoutKind.Sequential)]
    public struct MONITORINFO
    {
        public int cbSize;
        public RECT rcMonitor;
        public RECT rcWork;   // monitor area minus the taskbar — what we clamp to
        public uint dwFlags;
    }

    [DllImport("user32.dll")]
    public static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint dwFlags);

    // Returns the monitor with the largest intersection with rc, or NULL (DEFAULTTONULL) if rc
    // intersects no live monitor — used to detect saved geometry on a now-off/disconnected display.
    [DllImport("user32.dll")]
    public static extern IntPtr MonitorFromRect(ref RECT lprc, uint dwFlags);
    public const uint MONITOR_DEFAULTTONULL = 0;

    [DllImport("user32.dll")]
    public static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFO lpmi);
}
