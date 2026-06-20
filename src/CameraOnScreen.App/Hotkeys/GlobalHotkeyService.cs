using System.Runtime.InteropServices;
using CameraOnScreen.Core.Config;

namespace CameraOnScreen.App.Hotkeys;

/// <summary>
/// Registers process-global hotkeys against an existing HWND (the overlay window) via
/// <c>RegisterHotKey</c>. The hotkey message (WM_HOTKEY) arrives at that window's proc; the host
/// forwards it here through <see cref="OnHotkeyMessage"/>, keeping this service decoupled from the
/// window proc.
/// </summary>
public sealed class GlobalHotkeyService : IDisposable
{
    [DllImport("user32.dll")] private static extern bool RegisterHotKey(IntPtr hWnd, int id, uint mods, uint vk);
    [DllImport("user32.dll")] private static extern bool UnregisterHotKey(IntPtr hWnd, int id);

    private readonly IntPtr _hwnd;                 // the overlay window's HWND
    private readonly List<int> _ids = new();       // successfully registered ids (to unregister on dispose)
    private Action<HotkeyAction>? _onPressed;
    private IReadOnlyList<HotkeyBinding> _bindings = Array.Empty<HotkeyBinding>();

    // Bindings whose RegisterHotKey call failed (e.g. another app already owns the combination).
    // Surfaced so a taken hotkey is not silently dropped — see Register's return value.
    private readonly List<HotkeyAction> _failed = new();

    /// <summary>Actions whose hotkey could not be registered (already owned by another process).</summary>
    public IReadOnlyList<HotkeyAction> FailedRegistrations => _failed;

    public GlobalHotkeyService(IntPtr messageWindowHwnd) => _hwnd = messageWindowHwnd;

    /// <summary>
    /// Register each binding. A binding whose <c>RegisterHotKey</c> fails is recorded in
    /// <see cref="FailedRegistrations"/> rather than throwing — one taken hotkey must not crash the
    /// app or abort the remaining registrations. Returns the list of actions that failed.
    /// </summary>
    public IReadOnlyList<HotkeyAction> Register(IReadOnlyList<HotkeyBinding> bindings, Action<HotkeyAction> onPressed)
    {
        _bindings = bindings; _onPressed = onPressed;
        _failed.Clear();
        for (int i = 0; i < bindings.Count; i++)
        {
            // Use the binding index as the registration id so OnHotkeyMessage(id) maps back to it.
            // _ids holds only ids that registered, so Dispose unregisters exactly what we own.
            if (RegisterHotKey(_hwnd, i, (uint)bindings[i].Modifiers, bindings[i].VirtualKey))
                _ids.Add(i);
            else
                _failed.Add(bindings[i].Action);
        }
        return _failed;
    }

    // Call from the message window's WndProc on WM_HOTKEY (0x0312); wParam = id.
    public void OnHotkeyMessage(int id)
    {
        if (id >= 0 && id < _bindings.Count) _onPressed?.Invoke(_bindings[id].Action);
    }

    public void Dispose() { foreach (var id in _ids) UnregisterHotKey(_hwnd, id); }
}
