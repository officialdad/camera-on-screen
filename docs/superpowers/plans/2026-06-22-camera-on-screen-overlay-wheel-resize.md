# Overlay Wheel-Resize Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user hover the on-screen overlay and scroll the mouse wheel to resize its footprint live (aspect-locked, center-anchored), active only when the overlay is interactive.

**Architecture:** A pure geometry helper in Core (`OverlaySizer`) owns the resize math and is unit-tested. A `WH_MOUSE_LL` low-level mouse hook in the App delivers wheel events to the no-focus overlay; its cursor-in-rect test is the hover gate. `MainWindow` wires the hook to `OverlaySizer` + a new `OverlayWindow.SetBounds`, then persists the new size on a debounce via the existing `Save()`. No shim, C-ABI, MVVM, or config-schema change.

**Tech Stack:** C# .NET 8, WinUI 3 (unpackaged), Win32 P/Invoke (`user32.dll`), xUnit (Core tests). Spec: `docs/superpowers/specs/2026-06-22-camera-on-screen-overlay-wheel-resize-design.md`.

## Global Constraints

- **Pristine build:** 0 warnings across App and Core. CI enforces `/warnaserror` + `TreatWarningsAsErrors` — any warning is a finding.
- **No shim / C-ABI / MVVM / config-schema change.** `OverlaySettings.Width/Height` already persist and restore via `ResolveStartupBounds`; do not add fields.
- **Overlay is no-focus + topmost.** Any window-positioning call must use `SWP_NOACTIVATE`; never steal focus or change Z-order intent.
- **Core stays Win32-free.** `CameraOnScreen.Core` must not reference WinUI/Win32 types. `OverlaySizer` uses only its own `Rect` struct.
- **App build needs the shim DLL present** at `native/shim/x64/Debug/CameraOnScreen.Shim.dll` (the App copies it via a `<None>` item). It already exists from prior builds; if missing, build it per CLAUDE.md "Build & test". These tasks do **not** modify the shim.
- App-side pieces (hook, window positioning, wheel delivery) cannot be GDI-screenshotted or unit-tested — they are confirmed by a **human visual gate** on the RTX 3090 (Task 4).

---

### Task 1: `OverlaySizer` — pure resize math (Core, TDD)

**Files:**
- Create: `src/CameraOnScreen.Core/Overlay/OverlaySizer.cs`
- Test: `tests/CameraOnScreen.Core.Tests/Overlay/OverlaySizerTests.cs`

**Interfaces:**
- Produces:
  - `public readonly record struct Rect(int X, int Y, int W, int H)` in namespace `CameraOnScreen.Core.Overlay`.
  - `public static class OverlaySizer` with:
    - `public const double StepPerNotch = 0.08;`
    - `public const int MinHeight = 120;`
    - `public static Rect Resize(Rect current, int notches, Rect workArea)` — scales by `(1+StepPerNotch)^notches`, preserves aspect (driven by height), floors height at `MinHeight`, clamps to `workArea`, and keeps the current center.

- [ ] **Step 1: Write the failing tests**

Create `tests/CameraOnScreen.Core.Tests/Overlay/OverlaySizerTests.cs`:

```csharp
using CameraOnScreen.Core.Overlay;
using Xunit;

namespace CameraOnScreen.Core.Tests.Overlay;

public class OverlaySizerTests
{
    // A monitor large enough that no clamp engages, for the basic grow/shrink cases.
    private static readonly Rect BigWorkArea = new(0, 0, 3840, 2160);

    [Fact]
    public void Zero_notches_returns_current_unchanged()
    {
        var cur = new Rect(200, 200, 320, 240);
        Assert.Equal(cur, OverlaySizer.Resize(cur, 0, BigWorkArea));
    }

    [Fact]
    public void One_notch_up_grows_about_8_percent_and_keeps_center()
    {
        // 320x240 @ (200,200): center (360,320). factor 1.08 -> H=round(259.2)=259,
        // W=round(259*4/3)=345. newX=360-345/2=188, newY=320-259/2=191.
        var result = OverlaySizer.Resize(new Rect(200, 200, 320, 240), 1, BigWorkArea);
        Assert.Equal(345, result.W);
        Assert.Equal(259, result.H);
        Assert.Equal(188, result.X);
        Assert.Equal(191, result.Y);
    }

    [Fact]
    public void One_notch_down_shrinks()
    {
        // factor 1/1.08=0.9259 -> H=round(222.2)=222, W=round(222*4/3)=296.
        var result = OverlaySizer.Resize(new Rect(0, 0, 320, 240), -1, BigWorkArea);
        Assert.Equal(296, result.W);
        Assert.Equal(222, result.H);
    }

    [Fact]
    public void Aspect_ratio_is_preserved_within_one_pixel()
    {
        var result = OverlaySizer.Resize(new Rect(0, 0, 320, 240), 3, BigWorkArea);
        Assert.True(System.Math.Abs((double)result.W / result.H - 320.0 / 240.0) < 0.02);
    }

    [Fact]
    public void Shrinking_past_min_height_floors_at_min_keeping_aspect()
    {
        // Huge negative notch count collapses toward zero; floored to MinHeight=120,
        // W=round(120*4/3)=160.
        var result = OverlaySizer.Resize(new Rect(0, 0, 320, 240), -50, BigWorkArea);
        Assert.Equal(OverlaySizer.MinHeight, result.H);
        Assert.Equal(160, result.W);
    }

    [Fact]
    public void Growth_is_clamped_to_work_area_height()
    {
        // 320x240 in a 400x300 work area, big zoom: height capped at 300, W=round(300*4/3)=400.
        var result = OverlaySizer.Resize(new Rect(0, 0, 320, 240), 10, new Rect(0, 0, 400, 300));
        Assert.Equal(300, result.H);
        Assert.Equal(400, result.W);
    }

    [Fact]
    public void Growth_is_clamped_to_work_area_width_for_wide_overlays()
    {
        // 400x200 (2:1) in a 500x500 work area: width caps at 500, H=round(500/2)=250.
        var result = OverlaySizer.Resize(new Rect(0, 0, 400, 200), 10, new Rect(0, 0, 500, 500));
        Assert.Equal(500, result.W);
        Assert.Equal(250, result.H);
    }
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~OverlaySizer"`
Expected: FAIL — build error, `OverlaySizer` / `Rect` do not exist (`CS0246`).

- [ ] **Step 3: Write the implementation**

Create `src/CameraOnScreen.Core/Overlay/OverlaySizer.cs`:

```csharp
using System;

namespace CameraOnScreen.Core.Overlay;

/// <summary>Screen rectangle in pixels. Win32-free so it stays unit-testable in Core.</summary>
public readonly record struct Rect(int X, int Y, int W, int H);

/// <summary>
/// Pure geometry for mouse-wheel overlay resize. One wheel notch scales the overlay by
/// <see cref="StepPerNotch"/>; aspect ratio is preserved (driven by height), height is floored at
/// <see cref="MinHeight"/>, the result is clamped to the monitor work area, and the overlay's
/// center stays fixed (center-anchored resize).
/// </summary>
public static class OverlaySizer
{
    /// <summary>Fractional size change per wheel notch (one WHEEL_DELTA = 120 units).</summary>
    public const double StepPerNotch = 0.08;

    /// <summary>Smallest overlay height (px); a shrink never goes below this.</summary>
    public const int MinHeight = 120;

    public static Rect Resize(Rect current, int notches, Rect workArea)
    {
        if (notches == 0) return current;

        double aspect = current.W / (double)current.H;
        double factor = Math.Pow(1.0 + StepPerNotch, notches);

        int newH = (int)Math.Round(current.H * factor);
        if (newH < MinHeight) newH = MinHeight;
        if (newH > workArea.H) newH = workArea.H;

        int newW = (int)Math.Round(newH * aspect);
        if (newW > workArea.W)
        {
            newW = workArea.W;
            newH = (int)Math.Round(newW / aspect);
        }

        // Center-anchored: keep the current center point fixed.
        int cx = current.X + current.W / 2;
        int cy = current.Y + current.H / 2;
        return new Rect(cx - newW / 2, cy - newH / 2, newW, newH);
    }
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~OverlaySizer"`
Expected: PASS — 7 passed.

- [ ] **Step 5: Commit**

```bash
git add src/CameraOnScreen.Core/Overlay/OverlaySizer.cs tests/CameraOnScreen.Core.Tests/Overlay/OverlaySizerTests.cs
git commit -m "feat(overlay): OverlaySizer wheel-resize math (Core, unit-tested)"
```

---

### Task 2: `OverlayWindow.SetBounds` + interactive gate (App)

**Files:**
- Modify: `src/CameraOnScreen.App/Overlay/OverlayWindow.cs`

**Interfaces:**
- Consumes: existing `Interop.SetWindowPos`, `SWP_NOACTIVATE`, `SWP_NOZORDER` (already declared in `Interop.cs`).
- Produces:
  - `public void SetBounds(int x, int y, int w, int h)` — moves+resizes the overlay HWND without activating or reordering; the resulting `WM_SIZE` re-runs `UpdateScale` automatically.
  - `public bool IsInteractive => !_locked && !_clickThrough` — true only when neither Lock nor Click-through is active.
  - `private bool _clickThrough` field, kept in sync inside `SetClickThrough`.

> No automated test — this is Win32 windowing. Verified by the App build (0 warnings) here and the human visual gate in Task 4.

- [ ] **Step 1: Track click-through state**

In `src/CameraOnScreen.App/Overlay/OverlayWindow.cs`, add a field next to `_locked` (near line 36):

```csharp
    private bool _locked;     // when true: no drag/resize and no chrome (clean capture).
    private bool _clickThrough; // mirrors WS_EX_TRANSPARENT; gates wheel-resize (Task: wheel-resize).
```

In `SetClickThrough` (near line 146), record the flag at the top of the method:

```csharp
    public void SetClickThrough(bool on)
    {
        _clickThrough = on;
        int ex = GetWindowLong(_hwnd, GWL_EXSTYLE);
```

- [ ] **Step 2: Add `IsInteractive` and `SetBounds`**

Add these members near the other public Task-13 API (just after `SetZoom`, around line 178):

```csharp
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
```

- [ ] **Step 3: Build the App to verify 0 warnings**

Run: `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild`
Expected: `Build succeeded.` with `0 Warning(s)` and `0 Error(s)`.

- [ ] **Step 4: Commit**

```bash
git add src/CameraOnScreen.App/Overlay/OverlayWindow.cs
git commit -m "feat(overlay): SetBounds + IsInteractive gate for wheel-resize"
```

---

### Task 3: `MouseWheelHook` — WH_MOUSE_LL delivery (App)

**Files:**
- Modify: `src/CameraOnScreen.App/Overlay/Interop.cs`
- Create: `src/CameraOnScreen.App/Overlay/MouseWheelHook.cs`

**Interfaces:**
- Consumes: existing `Interop.POINT`, `Interop.GetModuleHandle`.
- Produces:
  - Interop additions: `WH_MOUSE_LL`, `WM_MOUSEWHEEL`, `HC_ACTION`, `WHEEL_DELTA`, `MSLLHOOKSTRUCT`, `LowLevelMouseProc` delegate, `SetWindowsHookEx`/`UnhookWindowsHookEx`/`CallNextHookEx`.
  - `public sealed class MouseWheelHook : IDisposable` — ctor takes `Func<Interop.POINT, int, bool> onWheel` (returns `true` when the wheel was handled, so the hook swallows it). Installs the hook on the calling thread; `Dispose()` unhooks.

> No automated test — global OS hook. Verified by the App build here and the human visual gate in Task 4.

- [ ] **Step 1: Add hook P/Invoke to `Interop.cs`**

Append inside the `Interop` class in `src/CameraOnScreen.App/Overlay/Interop.cs` (before the closing brace, after the `GetModuleHandle` import at line 91):

```csharp
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

    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr SetWindowsHookEx(int idHook, LowLevelMouseProc lpfn, IntPtr hMod, uint dwThreadId);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool UnhookWindowsHookEx(IntPtr hhk);

    [DllImport("user32.dll")]
    public static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);
```

- [ ] **Step 2: Create `MouseWheelHook.cs`**

Create `src/CameraOnScreen.App/Overlay/MouseWheelHook.cs`:

```csharp
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
public sealed class MouseWheelHook : IDisposable
{
    private readonly Func<POINT, int, bool> _onWheel;
    private readonly LowLevelMouseProc _proc; // keep the delegate alive for the hook's lifetime
    private IntPtr _hook;
    private bool _disposed;

    public MouseWheelHook(Func<POINT, int, bool> onWheel)
    {
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
```

- [ ] **Step 3: Build the App to verify 0 warnings**

Run: `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild`
Expected: `Build succeeded.` with `0 Warning(s)` and `0 Error(s)`.

- [ ] **Step 4: Commit**

```bash
git add src/CameraOnScreen.App/Overlay/Interop.cs src/CameraOnScreen.App/Overlay/MouseWheelHook.cs
git commit -m "feat(overlay): WH_MOUSE_LL MouseWheelHook for wheel delivery"
```

---

### Task 4: Wire the hook in `MainWindow` + debounced persist (App)

**Files:**
- Modify: `src/CameraOnScreen.App/Overlay/Interop.cs` (monitor work-area APIs)
- Modify: `src/CameraOnScreen.App/MainWindow.xaml.cs`

**Interfaces:**
- Consumes: `OverlaySizer.Resize` + `Rect` (Task 1); `OverlayWindow.SetBounds`, `OverlayWindow.IsInteractive`, `OverlayWindow.GetBounds` (Task 2); `MouseWheelHook` (Task 3).
- Produces: end-to-end wheel-resize. No new public surface.

> No automated test — integration of OS hook + windowing. Verified by the App build (0 warnings) and the **human visual gate** (Step 7).

- [ ] **Step 1: Add monitor work-area P/Invoke to `Interop.cs`**

Append inside the `Interop` class in `src/CameraOnScreen.App/Overlay/Interop.cs` (after the hook block from Task 3):

```csharp
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

    [DllImport("user32.dll")]
    public static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFO lpmi);
```

- [ ] **Step 2: Add the hook + save-timer fields to `MainWindow`**

In `src/CameraOnScreen.App/MainWindow.xaml.cs`, add fields near `_timer` (around line 26):

```csharp
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _timer;
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _saveTimer; // debounces persist after wheel-resize
    private Overlay.MouseWheelHook? _wheelHook;
```

- [ ] **Step 3: Install the hook + debounce timer in the constructor**

In the `MainWindow` constructor, after the frame-pump timer block (`_timer.Start();`, around line 75) and before the `ProbeCapabilitiesAsync` call, add:

```csharp
        // Debounced persist for wheel-resize: the wheel has no "gesture end" event (unlike
        // WM_EXITSIZEMOVE), so coalesce rapid notches into a single Save() ~400ms after the last one.
        _saveTimer = DispatcherQueue.CreateTimer();
        _saveTimer.Interval = TimeSpan.FromMilliseconds(400);
        _saveTimer.IsRepeating = false;
        _saveTimer.Tick += (_, _) => Save();

        // Global wheel hook: resize the overlay when the cursor is over it and it is interactive.
        // The callback runs on the UI thread (the hook is installed here), so it touches the overlay
        // directly. Returning true swallows the wheel so the app under the cursor doesn't scroll.
        _wheelHook = new Overlay.MouseWheelHook(OnWheelOverScreen);
```

- [ ] **Step 4: Add the wheel callback method**

Add this method to `MainWindow` (e.g. just after `OnHotkeyAction`, around line 135):

```csharp
    // Called on the UI thread for every WM_MOUSEWHEEL. Resize only when the cursor is inside the
    // overlay AND the overlay is interactive (not locked / not click-through). Returns true when we
    // handled the wheel so MouseWheelHook swallows it.
    private bool OnWheelOverScreen(Interop.POINT pt, int notches)
    {
        if (!_overlay.IsInteractive) return false;

        var (x, y, w, h) = _overlay.GetBounds();
        bool inside = pt.x >= x && pt.x < x + w && pt.y >= y && pt.y < y + h;
        if (!inside) return false;

        var work = GetWorkArea(_overlay.Hwnd);
        var next = CameraOnScreen.Core.Overlay.OverlaySizer.Resize(
            new CameraOnScreen.Core.Overlay.Rect(x, y, w, h), notches, work);
        _overlay.SetBounds(next.X, next.Y, next.W, next.H);

        // Debounce the disk write: restart the one-shot timer on every notch.
        _saveTimer?.Stop();
        _saveTimer?.Start();
        return true;
    }

    // Work area (monitor minus taskbar) of the monitor the overlay is on, as an OverlaySizer.Rect.
    private static CameraOnScreen.Core.Overlay.Rect GetWorkArea(IntPtr hwnd)
    {
        IntPtr mon = Interop.MonitorFromWindow(hwnd, Interop.MONITOR_DEFAULTTONEAREST);
        var mi = new Interop.MONITORINFO { cbSize = Marshal.SizeOf<Interop.MONITORINFO>() };
        Interop.GetMonitorInfo(mon, ref mi);
        var r = mi.rcWork;
        return new CameraOnScreen.Core.Overlay.Rect(r.left, r.top, r.right - r.left, r.bottom - r.top);
    }
```

> `Marshal` is already imported (`using System.Runtime.InteropServices;` at the top of `MainWindow.xaml.cs`).

- [ ] **Step 5: Tear down the hook + save timer on close**

In `OnWindowClosed` (around line 96), stop the save timer and dispose the hook **before** disposing the VM/overlay. Update the start of the method:

```csharp
    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        // Stop the timer first so no WM_EXITSIZEMOVE-driven InteractionEnded can race with Save().
        _timer?.Stop();
        _timer = null;
        // Unhook the global wheel hook and flush any pending debounced save before teardown, so no
        // hook callback fires into a disposed overlay.
        _wheelHook?.Dispose();
        _wheelHook = null;
        _saveTimer?.Stop();
        _saveTimer = null;
```

(The rest of `OnWindowClosed` — unsubscribe, `Save()`, dispose — is unchanged.)

- [ ] **Step 6: Build the App to verify 0 warnings**

Run: `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild`
Expected: `Build succeeded.` with `0 Warning(s)` and `0 Error(s)`.

- [ ] **Step 7: Human visual gate (run the app)**

Run the App (per CLAUDE.md "Run with effects", or plain passthrough). Confirm by hand on the RTX 3090:
- Hover the overlay, scroll up → overlay grows from its center; scroll down → shrinks. Aspect ratio holds.
- Scrolling while the cursor is over the **control panel or desktop** does NOT resize the overlay.
- While resizing over the overlay, the window/app underneath does **not** also scroll (event swallowed).
- Shrinking stops at a sensible minimum; growing stops at the screen work area.
- Toggle **Lock** (or **Click-through**) → wheel no longer resizes the overlay.
- Resize via wheel, wait ~½s, close + reopen the app → the new size is restored (debounced `Save()` persisted `Overlay.Width/Height`).

- [ ] **Step 8: Commit**

```bash
git add src/CameraOnScreen.App/Overlay/Interop.cs src/CameraOnScreen.App/MainWindow.xaml.cs
git commit -m "feat(overlay): wire wheel-resize hook + debounced persist in MainWindow"
```

---

## Notes for the implementer

- **Why a hook and not `WM_MOUSEWHEEL` in the overlay proc:** the overlay never takes focus (`SW_SHOWNOACTIVATE`), so wheel messages go to the focused window, not the overlay. The global `WH_MOUSE_LL` hook is the only reliable delivery, and its cursor-in-rect test is exactly the "only when hovered" gate the user asked for.
- **Thread safety:** the hook is installed on the UI thread and its callback runs there, so `OnWheelOverScreen` may touch `_overlay` and the dispatcher timer directly — no marshalling needed.
- **Persistence reuses the existing path:** `Save()` already reads live geometry via `_overlay.GetBounds()` and writes `Overlay.Width/Height`; the only new piece is the debounce timer that calls it after the wheel settles.
- **Do not** change the swap chain, `UpdateScale`, Zoom, or Mirror — `SetBounds` triggers `WM_SIZE` → `UpdateScale`, which already re-stretches content to the new size.
```
