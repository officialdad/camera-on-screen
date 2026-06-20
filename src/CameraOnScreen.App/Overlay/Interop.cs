using System.Runtime.InteropServices;

namespace CameraOnScreen.App.Overlay;

internal static class Interop
{
    public const int WS_POPUP = unchecked((int)0x80000000);
    public const int WS_EX_LAYERED = 0x00080000;
    public const int WS_EX_TOPMOST = 0x00000008;
    public const int WS_EX_NOREDIRECTIONBITMAP = 0x00200000;
    public const int WS_EX_TRANSPARENT = 0x00000020;

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

    // Window messages handled by the overlay proc (Task 13).
    public const uint WM_MOVE = 0x0003;
    public const uint WM_SIZE = 0x0005;
    public const uint WM_NCHITTEST = 0x0084;
    public const uint WM_MOUSEMOVE = 0x0200;
    public const uint WM_MOUSELEAVE = 0x02A3;
    // WM_EXITSIZEMOVE fires ONCE when the user finishes a drag/resize (Task 14): the cue to
    // persist geometry without thrashing the disk on every live WM_MOVE/WM_SIZE pixel update.
    public const uint WM_EXITSIZEMOVE = 0x0232;
    // WM_HOTKEY (Task 14): RegisterHotKey targets the overlay HWND, so the hotkey message arrives
    // at this proc; wParam carries the registration id.
    public const uint WM_HOTKEY = 0x0312;

    // WM_NCHITTEST return codes: HTCLIENT (no drag), HTCAPTION (drag-anywhere),
    // HTBOTTOMRIGHT (bottom-right resize grip).
    public const int HTCLIENT = 1;
    public const int HTCAPTION = 2;
    public const int HTBOTTOMRIGHT = 17;

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int left, top, right, bottom; }

    // TrackMouseEvent flags + struct: request a WM_MOUSELEAVE once the pointer exits the window.
    public const uint TME_LEAVE = 0x00000002;

    [StructLayout(LayoutKind.Sequential)]
    public struct TRACKMOUSEEVENT
    {
        public int cbSize;
        public uint dwFlags;
        public IntPtr hwndTrack;
        public uint dwHoverTime;
    }

    [DllImport("user32.dll")] public static extern bool TrackMouseEvent(ref TRACKMOUSEEVENT lpEventTrack);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr hWnd, ref POINT lpPoint);

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
    // SetWindowPos flags: keep current position/Z-order/activation, only change size.
    public const uint SWP_NOMOVE = 0x0002;
    public const uint SWP_NOZORDER = 0x0004;
    public const uint SWP_NOACTIVATE = 0x0010;
    [DllImport("kernel32.dll")] public static extern IntPtr GetModuleHandle(string? name);
}
