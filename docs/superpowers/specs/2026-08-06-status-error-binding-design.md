# Surfacing native effect errors in the panel (issue #59)

Date: 2026-08-06
Issue: [#59](https://github.com/officialdad/camera-on-screen/issues/59)
Scope: Linux / Avalonia panel + the shared native shim. Windows panel is out of scope (#38).

## Problem

`MainViewModel.StatusError` is assigned on every 250 ms status poll
(`MainViewModel.cs:338`, from `ShimStatus.Error`) and bound nowhere in
`src/CameraOnScreen.App.Avalonia/MainWindow.axaml`. A native effect failure is polled, stored,
and never shown. Since #57 `CameraError` is the panel's only visible error channel, and it only
ever carries the two watchdog strings. An effect that fails to load reports nothing to the user
beyond its toggle appearing not to work.

## What `StatusError` actually carries

Established before designing:

- `cos_get_status` fills `CosStatus.error` with the **first non-empty** of
  `GreenScreenError() | EyeContactError() | SuperResError() | FrameInterpError()`
  (`native/shim/shim.cpp:93-101`). So `StatusError` carries **effect errors only** — never a
  capture error. That is what puts it in the AI Effects card rather than the Camera card.
- It is **not sticky**. Both capture backends clear all four strings when the effect is switched
  off, when a frame succeeds, and in `Capture::Stop` (`capture_v4l2.cpp:496-532,611-623`;
  `capture.cpp:425-544,628-638`). So after a watchdog auto-stop `StatusError` goes null on the
  next poll, and `CameraError` (sticky until the next `Start`) plus `StatusError` can only
  overlap for one 250 ms tick. **No priority or stacking logic is needed.**
- `PInvokeShim` maps an empty native string to `null` (`PInvokeShim.cs:120`), so
  `ObjectConverters.IsNotNull` is the correct visibility test.
- The individual native strings are bare — `"NvVFX_Run failed"`, `"out of memory"`,
  `"Upload (Transfer) failed"`, `"Maxine SDK not built in"` — and the merge in `cos_get_status`
  discards which of the four sources won. Unlabelled, a line reading `out of memory` under the
  effect toggles tells the user nothing.

## Design

### 1. Label the merged error at its source

`native/shim/shim.cpp`, in `cos_get_status`: the merge already knows which source won, so name it
there rather than in either panel. One file, shared by both OSes, no ABI change.

```cpp
std::string err = g_capture.GreenScreenError();
const char* who = "AI Green Screen";
if (err.empty()) { err = g_capture.EyeContactError();  who = "Eye Contact"; }
if (err.empty()) { err = g_capture.SuperResError();    who = "AI Sharpness"; }
if (err.empty()) { err = g_capture.FrameInterpError(); who = "Smooth 60 fps (AI)"; }
if (!err.empty()) err = std::string(who) + ": " + err;
```

Labels are verbatim the panel's own control captions (`MainWindow.axaml:99,124,129,153`) so the
message points at a control the user can see — e.g. `AI Green Screen: NvVFX_Run failed`. The
existing 255-char truncation into `out->error` is unchanged; native strings are short enough that
the prefix cannot push a real message into truncation.

### 2. Bind it in the Avalonia panel

`src/CameraOnScreen.App.Avalonia/MainWindow.axaml`: a new `TextBlock` as the last child of the
AI Effects card's `StackPanel`, after the `CapabilityDetail` block.

```xml
<TextBlock Text="{Binding StatusError}" TextWrapping="Wrap"
           Foreground="{StaticResource Danger}"
           IsVisible="{Binding StatusError, Converter={x:Static ObjectConverters.IsNotNull}}" />
```

Same shape as the existing `CameraError` block (`MainWindow.axaml:70-72`). No `StringFormat` —
native supplies the label. It sits in the AI Effects card because `StatusError` is by
construction an effect error; that also means it never shares a line with `CameraError`, which
disposes of the issue's stack/priority/share question.

### 3. One `Danger` brush, two consumers

`#E06C75` is currently hardcoded on the `CameraError` TextBlock. Add
`<SolidColorBrush x:Key="Danger" Color="#E06C75" />` beside the existing `CardBg`/`CardStroke`/
`Caution` resources and repoint both error blocks at it. Net +1 line, keeps the two error lines
from drifting apart.

### 4. Test

One xUnit test in `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`, next to
`CameraError_survives_a_status_poll`: `OnStatus` publishes `ShimStatus.Error` to `StatusError`,
and a subsequent status with `Error: null` clears it. The non-sticky round-trip is the contract
the whole design rests on — if someone later makes `StatusError` sticky the way `CameraError` is,
the panel grows a red line that never goes away, and nothing else in the suite would catch it.

## Non-goals

- **Windows panel.** `src/CameraOnScreen.App/MainWindow.xaml` binds neither `CameraError` nor
  `StatusError`; #57 deliberately left the WinUI side unwired and it cannot be compile-checked on
  this box. The shim change (§1) benefits it for free once #38 wires it up.
- **Flicker suppression.** A per-frame `ProcessFrame` failure that alternates with successes can
  toggle the line at up to 4 Hz. No debouncing until that is observed in practice.
- **Per-effect error fields.** One merged, labelled line is enough; splitting `CosStatus.error`
  into four would be an ABI change for a display nicety.

## Verification

Durable gates:

- `cmake --build native/shim/build` — clean under `-Wall -Wextra -Werror`.
- `dotnet test tests/CameraOnScreen.Core.Tests/…` — new test plus the existing 92, 0 warnings.
- `dotnet build src/CameraOnScreen.App.Avalonia/…` — 0 warnings (catches an XAML resource typo;
  a bad `{Binding}` path stays silent, hence the visual check below).

Visual check is a throwaway, because **the app deliberately makes a post-probe effect failure
unreachable**: an unavailable engine is `IsEnabled="False"` in the combo, `EffectsAvailable`
gates the toggles, and `Orchestrator.ApplyParams` coerces backend `2 → 0` when ONNX is
unavailable — so no combination of `COS_*` env vars produces an error string in a running
session. Procedure: add one scratch line in `cos_get_status` (`shim.cpp`) forcing `err`/`who`
before the label is applied, `cmake --build native/shim/build`, then `dotnet run --project
src/CameraOnScreen.App.Avalonia` — no camera, no running capture needed, since the status timer
starts polling from panel startup and the scratch line is on the merge path itself. Confirm the
labelled red line renders in the AI Effects card and wraps, then `git checkout -- native/shim/shim.cpp`
and rebuild. Confirm the scratch line is gone before committing.
