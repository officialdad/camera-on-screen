# StatusError Panel Binding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a native effect error visible in the Avalonia panel, labelled with the effect that produced it.

**Architecture:** `cos_get_status` already merges four effect error strings into one `CosStatus.error` field; it gains a source label at the merge point (`"AI Green Screen: NvVFX_Run failed"`). The Avalonia panel binds `MainViewModel.StatusError` raw in the AI Effects card — no sticky latch, no priority logic against `CameraError`, because the native side owns the string's lifetime and clears it on effect-off, on the next good frame, and in `Capture::Stop`.

**Tech Stack:** C++17 (native shim, CMake + `-Wall -Wextra -Werror`), C# .NET 8 (Core + Avalonia panel), xUnit.

**Spec:** `docs/superpowers/specs/2026-08-06-status-error-binding-design.md`

## Global Constraints

- Work in the current worktree (`.claude/worktrees/fix+59-bind-status-error`). Never `cd` to the main checkout.
- `dotnet` is not on `PATH`. Use the absolute path `/home/ariff/.dotnet/dotnet`. `cmake` is `/home/ariff/.local/bin/cmake`.
- Builds and tests must be **pristine — 0 warnings**. Native is `-Wall -Wextra -Werror`; C# is `TreatWarningsAsErrors`.
- **Do not touch `src/CameraOnScreen.App/`** (WinUI). It cannot be compile-checked on this box and the Windows half of #59 is blocked on #38.
- Effect labels are verbatim the Avalonia panel's control captions: `AI Green Screen`, `Eye Contact`, `AI Sharpness`, `Smooth 60 fps (AI)`.
- No new files in `src/` or `native/`. Every change is an edit to an existing file.

---

### Task 1: Label the merged effect error in the native shim

`cos_get_status` collapses four effect error sources into one field and throws away which one won, so the panel would show bare strings like `out of memory`. The merge is the only place that knows the source.

**Files:**
- Modify: `native/shim/shim.cpp:85-102` (`cos_get_status`)

**Interfaces:**
- Consumes: `Capture::GreenScreenError()`, `EyeContactError()`, `SuperResError()`, `FrameInterpError()` — all `std::string`, empty when no error (declared `native/shim/capture.h`).
- Produces: `CosStatus.error` now carries `"<Effect caption>: <native message>"` when non-empty, still `""` when there is no error. No ABI change — the field is the same `char error[256]`. C# sees it through `PInvokeShim.cs:120`, which maps empty to `null`.

- [ ] **Step 1: Read the current merge block**

Open `native/shim/shim.cpp` and confirm `cos_get_status` currently reads:

```cpp
    std::string err = g_capture.GreenScreenError();
    if (err.empty()) err = g_capture.EyeContactError();
    if (err.empty()) err = g_capture.SuperResError();
    if (err.empty()) err = g_capture.FrameInterpError();
    if (!err.empty()) {
```

- [ ] **Step 2: Add the source label**

Replace those first four lines with the labelled form. Leave the `memcpy`/truncation block below it untouched:

```cpp
    // Each source string is bare ("NvVFX_Run failed", "out of memory"), and the merge below is the
    // only place that still knows which effect produced it — so name it here rather than in either
    // panel. Labels are verbatim the panel's control captions so the message points at a control
    // the user can see (#59).
    std::string err = g_capture.GreenScreenError();
    const char* who = "AI Green Screen";
    if (err.empty()) { err = g_capture.EyeContactError();  who = "Eye Contact"; }
    if (err.empty()) { err = g_capture.SuperResError();    who = "AI Sharpness"; }
    if (err.empty()) { err = g_capture.FrameInterpError(); who = "Smooth 60 fps (AI)"; }
    if (!err.empty()) err = std::string(who) + ": " + err;
```

- [ ] **Step 3: Build the shim**

Run:

```bash
/home/ariff/.local/bin/cmake -S native/shim -B native/shim/build
/home/ariff/.local/bin/cmake --build native/shim/build
```

Expected: builds to completion, **zero warnings** (the build is `-Werror`, so any warning is a hard failure). This is the only automated gate for this task — the native tree has no unit-test harness, and reaching this code with a real error needs a running capture plus a failing effect. Task 3 exercises the label visually.

- [ ] **Step 4: Commit**

```bash
git add native/shim/shim.cpp
git commit -m "fix: label the merged effect error with its source effect (#59)

cos_get_status merges four bare effect error strings into one field and
discarded which one won, so a panel line would read 'out of memory' with no
hint what failed. Label at the merge point, which is the last place that
knows. Labels are verbatim the panel's control captions.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Bind StatusError in the Avalonia panel

**Files:**
- Modify: `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs` (add one test after `CameraError_survives_a_status_poll`, which ends at line 670)
- Modify: `src/CameraOnScreen.App.Avalonia/MainWindow.axaml` (resources at 19-23; `CameraError` block at 70-72; AI Effects card `StackPanel` ending at 157)
- Modify: `CLAUDE.md` (Architecture section, after the `**Status is polled, never pushed.**` bullet)

**Interfaces:**
- Consumes: `MainViewModel.StatusError` (`string?`, `[ObservableProperty]` at `MainViewModel.cs:201`, assigned in `OnStatus` at `:338`); `ShimStatus(bool Running, double Fps, GazeState Gaze, bool GreenScreenActive, bool EyeContactActive, string? Error, bool ExposureSupported = false)` from `src/CameraOnScreen.Core/Native/Contracts.cs:16`; the private test helper `Build(GpuTier tier, out FakeShim shim, bool greenScreenAvailable = false)` at the top of `MainViewModelTests.cs`.
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Write the test**

Add after `CameraError_survives_a_status_poll` (the test that ends at line 670). `vm.OnStatus` is public, so no fake plumbing is needed — `FakeShim` hardcodes `Error: null` and does not need changing.

```csharp
    [Fact]
    public void StatusError_round_trips_the_native_error_and_clears_when_it_goes_away()
    {
        // #59: the panel binds StatusError raw, with no sticky latch of its own, because the
        // native side owns the string's lifetime — both capture backends clear all four effect
        // error strings on effect-off, on the next successful frame, and in Capture::Stop. That
        // non-sticky round-trip is what makes a plain always-visible-when-set binding correct,
        // and what keeps CameraError (sticky until the next Start) from needing priority logic
        // against it. If StatusError were ever latched like CameraError, the panel would grow a
        // red line that never goes away and nothing else in this suite would notice.
        var vm = Build(GpuTier.Rtx, out _);

        vm.OnStatus(new ShimStatus(Running: true, Fps: 30, Gaze: GazeState.Unknown,
            GreenScreenActive: false, EyeContactActive: false,
            Error: "AI Green Screen: NvVFX_Run failed"));
        Assert.Equal("AI Green Screen: NvVFX_Run failed", vm.StatusError);

        vm.OnStatus(new ShimStatus(Running: true, Fps: 30, Gaze: GazeState.Unknown,
            GreenScreenActive: true, EyeContactActive: false, Error: null));
        Assert.Null(vm.StatusError);
    }
```

- [ ] **Step 2: Run the test — expect PASS, then prove it is not vacuous**

Run:

```bash
/home/ariff/.dotnet/dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~StatusError_round_trips"
```

Expected: **PASS**. `OnStatus` already assigns `StatusError`, so this is a regression guard on an existing contract, not red-then-green TDD — a green first run is correct here, but a green first run also cannot tell you the test exercises anything. Prove it does: comment out `StatusError = s.Error;` at `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs:338`, re-run the same command, and confirm it **FAILS** with `Assert.Equal() Failure` (expected `AI Green Screen: NvVFX_Run failed`, actual `null`). Then restore the line and re-run to confirm PASS again. Do not commit with that line commented out.

- [ ] **Step 3: Add the `Danger` brush resource**

`#E06C75` is currently hardcoded on the `CameraError` TextBlock and is about to have a second consumer. In `src/CameraOnScreen.App.Avalonia/MainWindow.axaml`, add it below the existing `Caution` brush (line 23), inside `<Window.Resources>`:

```xml
        <SolidColorBrush x:Key="Danger" Color="#E06C75" />
```

- [ ] **Step 4: Repoint the existing CameraError TextBlock at it**

Same file, lines 70-72. Replace:

```xml
                            <TextBlock Text="{Binding CameraError}" TextWrapping="Wrap"
                                       Foreground="#E06C75"
                                       IsVisible="{Binding CameraError, Converter={x:Static ObjectConverters.IsNotNull}}" />
```

with:

```xml
                            <TextBlock Text="{Binding CameraError}" TextWrapping="Wrap"
                                       Foreground="{StaticResource Danger}"
                                       IsVisible="{Binding CameraError, Converter={x:Static ObjectConverters.IsNotNull}}" />
```

- [ ] **Step 5: Bind StatusError as the last child of the AI Effects card**

Same file. The AI Effects card's inner `StackPanel` currently ends with the `CapabilityDetail` TextBlock (lines 155-156). Add the new block immediately after it, still inside that `StackPanel`:

```xml
                            <TextBlock Text="{Binding CapabilityDetail}" IsVisible="{Binding !EffectsAvailable}"
                                       TextWrapping="Wrap" Foreground="{StaticResource Caution}" />
                            <!-- #59: native effect errors (green screen / eye contact / sharpness /
                                 FRUC), polled every 250 ms. Bound raw and unlatched — cos_get_status
                                 supplies the effect label and the capture backends clear the string
                                 themselves, so there is nothing to format or reset here. Lives in
                                 this card, not next to CameraError, because CosStatus.error only
                                 ever carries effect failures. -->
                            <TextBlock Text="{Binding StatusError}" TextWrapping="Wrap"
                                       Foreground="{StaticResource Danger}"
                                       IsVisible="{Binding StatusError, Converter={x:Static ObjectConverters.IsNotNull}}" />
```

No `StringFormat`: Task 1 put the label in the native string.

- [ ] **Step 6: Build the panel and run the full test suite**

Run:

```bash
/home/ariff/.dotnet/dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj
/home/ariff/.dotnet/dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj
```

Expected: build succeeds with **0 warnings** (this catches a misspelled `StaticResource` key); tests report **93 passed, 0 failed** (92 before this task). Note the build cannot catch a wrong `{Binding}` path — Avalonia reflection bindings fail silently at runtime, which is why Task 3 exists.

- [ ] **Step 7: Record the two-error-channel contract in CLAUDE.md**

The "should they stack or take priority?" question in #59 has a non-obvious answer that someone will otherwise re-derive. In `CLAUDE.md`, in the `## Architecture — contracts that span files` list, add a bullet immediately after the `- **Status is polled, never pushed.**` bullet:

```markdown
- **Two error channels, different owners and lifetimes.** `CameraError` is Core-owned, sticky until the next `Start`, and carries only the two watchdog strings. `StatusError` mirrors `CosStatus.error` verbatim on every poll and is **native-owned**: `cos_get_status` labels the winning source (`"AI Green Screen: NvVFX_Run failed"`) and both capture backends clear all four effect strings on effect-off, on the next good frame, and in `Capture::Stop`. So the Avalonia panel binds `StatusError` raw in the AI Effects card (it only ever carries effect failures) and `CameraError` in the Camera card, with **no priority or stacking logic** — a watchdog stop clears the native strings, so the two can only overlap for one 250 ms tick (#59). The WinUI panel binds neither (#38).
```

- [ ] **Step 8: Commit**

```bash
git add tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs src/CameraOnScreen.App.Avalonia/MainWindow.axaml CLAUDE.md
git commit -m "fix: surface native effect errors in the Avalonia panel (#59)

StatusError was assigned on every 250 ms poll and bound nowhere, so a failing
effect reported nothing beyond its toggle appearing dead. Bind it raw at the
bottom of the AI Effects card — the field only ever carries effect errors, and
the native side clears it, so no latch and no priority logic against the sticky
CameraError. Hoist the shared error colour into a Danger brush.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Visual verification with a throwaway forced error

An Avalonia `{Binding}` typo fails silently, so the binding needs to be seen rendering. The app deliberately makes a real post-probe effect failure unreachable — unavailable engines are `IsEnabled="False"` in the combo, `EffectsAvailable` gates the toggles, and `Orchestrator.ApplyParams` coerces green-screen backend `2 → 0` when ONNX is unavailable — so no `COS_*` env-var combination produces an error string in a running session. Force one instead. Forcing it inside `cos_get_status` (rather than in the capture worker) needs no camera and no running capture, and exercises the Task 1 label path.

**Files:**
- Temporarily modify, then revert: `native/shim/shim.cpp`

- [ ] **Step 1: Add the scratch line**

In `cos_get_status`, immediately **before** the `if (!err.empty()) err = std::string(who) + ": " + err;` line added in Task 1, insert:

```cpp
    err = "NvVFX_Run failed"; who = "AI Green Screen"; // SCRATCH #59 verify — REVERT
```

- [ ] **Step 2: Build and run the panel**

```bash
/home/ariff/.local/bin/cmake --build native/shim/build
/home/ariff/.dotnet/dotnet run --project src/CameraOnScreen.App.Avalonia
```

`dotnet build` copies the built `.so` next to the app, so no publish is needed. The status timer runs from panel startup, so the line appears without pressing Start.

- [ ] **Step 3: Confirm with your human partner**

This is an inherent human gate — ask them to confirm, in the **AI Effects** card and nowhere else:

- a red line reading exactly `AI Green Screen: NvVFX_Run failed`
- below the `Smooth 60 fps (AI)` toggle and the amber availability note
- wrapping rather than clipping when the window is narrowed

If it does not appear, the binding path or the resource key is wrong — fix it in `MainWindow.axaml` and repeat this step. Do not proceed until they confirm.

- [ ] **Step 4: Revert the scratch line and rebuild**

```bash
git checkout -- native/shim/shim.cpp
/home/ariff/.local/bin/cmake --build native/shim/build
git status --short
```

Expected: `git status --short` prints **nothing**. The rebuild matters — it stops a shim with a hardcoded error being left in `src/CameraOnScreen.App.Avalonia/bin/`.

- [ ] **Step 5: No commit**

This task produces no commit by design. If `git status` is not clean, the scratch line survived — remove it before finishing.

---

### Task 4: Final gates and PR

**Files:** none modified.

- [ ] **Step 1: Run every gate from a clean tree**

```bash
/home/ariff/.local/bin/cmake --build native/shim/build
/home/ariff/.dotnet/dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj
/home/ariff/.dotnet/dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj
```

Expected: native builds `-Werror` clean; app builds with 0 warnings; **93 passed, 0 failed**.

- [ ] **Step 2: Confirm nothing Windows-side was touched**

```bash
git diff --name-only main...HEAD
```

Expected: exactly `CLAUDE.md`, `docs/superpowers/plans/2026-08-06-status-error-binding.md`, `docs/superpowers/specs/2026-08-06-status-error-binding-design.md`, `native/shim/shim.cpp`, `src/CameraOnScreen.App.Avalonia/MainWindow.axaml`, `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`. No path under `src/CameraOnScreen.App/`.

- [ ] **Step 3: Open the PR**

Use the `superpowers:finishing-a-development-branch` skill. The PR body must state that the visual gate (Task 3) was confirmed by a human against a forced error, that the Windows panel is deliberately untouched (#38), and `Closes #59`.
