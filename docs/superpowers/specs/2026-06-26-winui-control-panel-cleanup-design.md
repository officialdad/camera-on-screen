# WinUI Control-Panel Cleanup — Design

**Date:** 2026-06-26
**Branch:** `worktree-feature+winui-panel-cleanup` (worktree, from origin/main)

## Context

The WinUI control panel (`MainWindow.xaml`) is a single flat `ScrollViewer → StackPanel`:
camera, AI-effect toggles/sliders, overlay-behavior toggles, Start/Stop, and the status line
are all jammed together with no grouping or hierarchy. The window uses the default WinUI title
bar and a hardcoded 400×720 size that leaves the panel cramped or empty depending on DPI. Two
controls (Lock overlay, Click-through) and one (Zoom) are not meaningful for this app's intent —
a draggable, always-on-top camera overlay. The non-obvious overlay gestures (scroll-to-resize,
drag-the-handle) are undiscoverable.

Goal: streamline the panel for **clarity and accessibility** — group controls by concern, add a
custom branded title bar, surface the hidden gestures, drop the controls that don't belong, and
size the window to its content instead of a magic number.

## Scope

UI-layer change. C# shim, capture pipeline, and Maxine code are untouched.

### Removed
- **Lock overlay** toggle + VM `Locked` property + config load/save + `OnVmPropertyChanged` branch + `SetLocked` call.
- **Click-through** toggle + VM `ClickThrough` property + config load/save + branch + `SetClickThrough` call + the `ToggleClickThrough` default hotkey binding.
- **Zoom** slider + VM `Zoom` property + branch + `SetZoom` call.

Removing `Locked` and `ClickThrough` makes `OverlayWindow.IsInteractive` (`!_locked && !_clickThrough`)
always `true` — the overlay is permanently draggable/resizable, matching intent. `SetZoom` is no
longer called, so `UpdateScale`'s `Matrix3x2` uses zoom = 1 (mirror-only transform).

**Deliberately kept (ponytail):** `OverlaySettings.Locked/ClickThrough/Zoom` fields and the
`HotkeyAction.ToggleLock/ToggleClickThrough` enum members stay (unused), marked with a `ponytail:`
comment. Removing them would cascade into config-versioning + 3 test files for no user benefit.
`OverlayWindow.SetLocked/SetClickThrough/SetZoom` stay on the type (now unused by the app); they're
harmless and the overlay defaults (`_locked=false`, `_clickThrough=false`, zoom=1) give the desired
always-interactive, no-zoom behavior with no call needed.

### Kept / Moved
- **Mirror** → folded into the CAMERA group.
- **Lock exposure + Exposure slider** → CAMERA group (camera-related, not AI).
- Camera combo, Start/Stop, status line → CAMERA group.
- Green Screen (+Expand/Feather), Eye Contact (+note), AI Sharpness (+Quality, +note) → AI EFFECTS group.

## Layout

```
🎥 Camera on Screen                         ─ □ ✕   ← custom title bar
─────────────────────────────────────────────────
 ⓘ  Scroll over the overlay to resize ·            ← InfoBar (Informational, IsClosable=False)
    drag the  ＋  handle to move it.

 CAMERA                                             ← section header (BodyStrongTextBlockStyle)
   ┌───────────────────────────────────────────┐   ← card: Border, CardBackgroundFillColorDefaultBrush
   │ Camera             [Logitech ▾]           │
   │ [ ▶ Start ]  [ ⏹ Stop ]     • Running 30fps│
   │ Mirror (selfie view)            [ON ●]     │
   │ Lock exposure (steady FPS)      [OFF]      │
   │ Exposure           ──●────                 │
   └───────────────────────────────────────────┘

 AI EFFECTS
   ┌───────────────────────────────────────────┐
   │ AI Green Screen                 [ON ●]     │
   │   Expand  ──●──      Feather  ──●──        │
   │ Eye Contact                     [OFF]      │
   │   (caution note when unavailable)          │
   │ AI Sharpness [Denoise ▾]  Quality [High ▾] │
   │   (caution note when unavailable)          │
   └───────────────────────────────────────────┘
─────────────────────────────────────────────────
 NVIDIA Maxine attribution (pinned footer — unchanged)
```

## Components

### 1. Custom title bar
- `ExtendsContentIntoTitleBar = true` (in `MainWindow` ctor) + `SetTitleBar(<grid>)`.
- Title bar = a `Grid` (row 0 of the window) holding the app icon (cos.ico, 16×16 `Image`) +
  "Camera on Screen" `TextBlock` (`CaptionTextBlockStyle`), left-aligned, height 32, with the
  draggable region excluding the caption-button inset.
- System caption buttons (min/max/close) and system theme — no custom button drawing.
- Verify the exact API surface (`AppWindowTitleBar`, drag-region, RTL inset) via context7 / winui-design before coding.

### 2. Grouped cards (native, no new dependency)
- Each group: a `TextBlock` header (`BodyStrongTextBlockStyle`) + a `Border`
  (`Background={ThemeResource CardBackgroundFillColorDefaultBrush}`, `CornerRadius=8`,
  `BorderBrush={ThemeResource CardStrokeColorDefaultBrush}`, padding 12) wrapping a `StackPanel`
  of the existing controls. No `Expander` (small panel, everything stays visible = clarity).
- Controls keep their existing `Header` attribute — that is the accessibility name. Add
  `AutomationProperties.Name` only where a control has no `Header` (e.g. the Start/Stop buttons get
  explicit names; the status `TextBlock` is `AutomationProperties.LiveSetting=Polite`).
- Decision: native cards over `CommunityToolkit.WinUI.Controls.SettingsControls` — only 2 groups,
  a NuGet dep isn't justified.

### 3. Help InfoBar
- `InfoBar Severity="Informational" IsOpen="True" IsClosable="False"` at the top of the scroll body.
- Message states both hidden gestures: "Scroll over the overlay to resize it · drag the ＋ handle to move it."

### 4. Dynamic sizing
- Drop the hardcoded `PanelWidthDip/HeightDip = 400/720`.
- After first layout, measure the root content's `DesiredSize` and `AppWindow.Resize` to it
  (DPI-scaled, as `RightSizePanel` already does). Set a **minimum** window size to that measured
  size so every control stays reachable; window remains user-resizable, `ScrollViewer` is the
  fallback when shrunk.
- Min-size enforcement: use `OverlappedPresenter.PreferredMinimumWidth/Height` if available in
  WinAppSDK 1.8; else hook `WM_GETMINMAXINFO`. Confirm the API via context7 before coding.

## Data flow / contracts

No contract changes. `BuildParams()` still feeds the shim; removing Zoom/Lock/ClickThrough only
removes their `On…Changed` → `ApplyLiveParams` / overlay-setter wiring. `ToAppConfig` writes the
kept config fields with the removed ones at their defaults.

## Error handling

Unchanged. Capability probe still gates AI toggles OneWay; caution notes still bind to
`CapabilityDetail` / `EyeContactDetail` visibility.

## Testing

- **Core unit tests** (`dotnet test`) must stay green. `MainViewModelTests` references
  `Locked/ClickThrough/Zoom` — update those assertions to the removed-property reality (the props
  are gone from the VM; config round-trip keeps the fields at default). This is the one runnable
  check that fails if the VM surgery is wrong.
- **App build** must be pristine (0 warnings) — but the App project can't be built on a host
  without the shim DLL + Windows App SDK runtime; CI (self-hosted RTX) is the build gate.
- **Visual confirmation is a human gate** (per repo convention): the panel layout, title bar, and
  InfoBar can only be eyeballed by running the app on the RTX host. Document what to look for.

## Out of scope
- No theme/brand-accent color (system theme only).
- No SettingsControls toolkit.
- No removal of the kept-but-unused config fields / enum members / overlay setters.
- No overlay-window (capture) changes.
