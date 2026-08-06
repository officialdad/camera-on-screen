# Panel fixes and user-facing README — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop the Avalonia panel hiding itself at startup (#62), scroll the native effect-error line into view when it appears (#61), and turn the README into a user-facing document with a one-command install.

**Architecture:** Two small changes in the Avalonia UI layer (`MainWindow.axaml` / `MainWindow.axaml.cs`), one new install script, one line in the publish script, and a README rewrite. Nothing in `Core` and nothing in the native shim changes, so the existing unit-test suite is a regression guard rather than the target of new tests.

**Tech Stack:** C# .NET 8, Avalonia 11.2.1, bash, GNU tar + zstd.

**Spec:** `docs/superpowers/specs/2026-08-07-panel-fixes-and-user-readme-design.md`

## Global Constraints

- Builds and tests must be **pristine — 0 warnings**. CI enforces `/warnaserror` + `TreatWarningsAsErrors`.
- No new unit tests. Both code changes are Avalonia UI-layer behaviour (window state, scroll viewer) which the repo gates with written human checklists, not xUnit.
- `Core` must stay free of UI-framework types. Nothing in this plan touches `Core`.
- Windows / WinUI is out of scope — `MinimizeToTray` is Linux-only, parked on #38.
- Avalonia version is pinned at **11.2.1**. `DispatcherTimer.RunOnce(Action, TimeSpan)` exists there; do not reach for APIs from a later Avalonia.
- The release asset name is `CameraOnScreen-<tag>-linux-x64.tar.zst` — it carries the version, so there is **no** fixed `/releases/latest/download/` URL.
- The release tarball is packed with `tar -C "$OUT" -cf "$PKG" .`, so it extracts **flat** (no top-level directory).
- README must stay user-facing: no SDK names, versions, CUDA, TensorRT, or build instructions. The NVIDIA trademark/attribution line in the License section **stays** — that is a Maxine SDK License obligation.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs` | Modify | `_opened` → `_armed` latch (#62); `StatusError` scroll-into-view subscription (#61) |
| `src/CameraOnScreen.App.Avalonia/MainWindow.axaml` | Modify | `x:Name` on the `StatusError` `TextBlock` |
| `CLAUDE.md` | Modify | Minimize-to-tray paragraph: correct the startup-latch clause |
| `docs/superpowers/verification/2026-08-06-minimize-to-tray.md` | Modify | Row 11 restated; new row for the #61 scroll gate |
| `scripts/install.sh` | Create | One-command install: download latest release, extract, write a desktop entry |
| `scripts/publish-linux.sh` | Modify | Copy `cos.png` into the output tree so the desktop entry has an icon |
| `README.md` | Rewrite | User-facing only; technical half replaced by links |

Tasks 1–2 both touch `MainWindow.axaml.cs` and must run **in order, in the same working tree**. Tasks 3–5 touch disjoint files and are independent of 1–2.

---

### Task 1: #62 — replace the no-op `_opened` latch with `_armed`

**Files:**
- Modify: `src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs` (the `_opened` field, the `Opened` handler, the `PropertyChanged` handler)
- Modify: `CLAUDE.md` (minimize-to-tray paragraph)
- Modify: `docs/superpowers/verification/2026-08-06-minimize-to-tray.md` (row 11)

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: a private `bool _armed` field on `MainWindow`. Task 2 adds an unrelated subscription to the same constructor and must not disturb it.

**Why:** `Window.ShowCore` raises `OnOpened` synchronously right after `PlatformImpl.Show(...)`, and `X11Window.Show` is just `XMapWindow` + `XFlush` with no event pump in between. Every `WindowState` transition arrives later, from a `PropertyNotify` on `_NET_WM_STATE`. So `_opened` is always already `true` by the time a `Minimized` transition can be observed — the latch blocks nothing, and a transient post-map `_NET_WM_STATE_HIDDEN` still hides the panel before the user ever sees it.

- [ ] **Step 1: Replace the `_opened` field and its comment**

Find this block near the top of the class:

```csharp
    // Latched true by the Opened event, never reset. Guards the minimize-to-tray handler
    // below: without it, a transient WindowState == Minimized during X11 map (or a WM
    // restoring a previously iconified state) fires Hide() before the window has ever been
    // shown to the user — the unattributed "tray icon but no panel window" launch.
    private bool _opened;
```

Replace it with:

```csharp
    // Gates the minimize-to-tray handler below. NOT latched on Opened (#62): Window.ShowCore
    // raises Opened synchronously right after PlatformImpl.Show, and X11Window.Show is only
    // XMapWindow + XFlush, so every WindowState transition arrives later from a PropertyNotify
    // on _NET_WM_STATE — an Opened latch is already true before any state change can be seen and
    // blocks nothing. Set instead by whichever comes first: a transition to a non-Minimized
    // state (the WM clearing its transient _NET_WM_STATE_HIDDEN, i.e. the window is genuinely
    // up), or a 1 s grace timer from Opened for the quiet launch that emits no transition at all.
    private bool _armed;
```

- [ ] **Step 2: Replace the `Opened` handler with the grace timer**

Find:

```csharp
        Opened += (_, _) => _opened = true;
```

Replace with:

```csharp
        Opened += (_, _) => DispatcherTimer.RunOnce(() => _armed = true, TimeSpan.FromSeconds(1));
```

`Avalonia.Threading` is already imported at the top of the file — do not add a using.

- [ ] **Step 3: Rewrite the minimize handler and its comment**

Find:

```csharp
        // Minimize means "get out of the way", same as close: hide to the tray instead. Do NOT
        // also write WindowState = Normal here — that write races the WM's own async iconify
        // and wins, so the window never actually unmaps (measured on KDE/XWayland: the window
        // never left the mapped state, because the WindowState = Normal write beat the WM's
        // async deiconify). ShowPanel() already resets WindowState to Normal on restore, so a
        // later restore still comes back as a normal window, not a minimized one. Gated on
        // _opened: without that latch, a transient WindowState == Minimized during X11 map (or
        // a WM restoring a previously iconified state) hides the window before it was ever shown.
        PropertyChanged += (_, e) =>
        {
            if (e.Property == WindowStateProperty
                && WindowState == WindowState.Minimized
                && _opened
                && _services.Vm.MinimizeToTray)
            {
                Hide();
            }
        };
```

Replace with:

```csharp
        // Minimize means "get out of the way", same as close: hide to the tray instead. Do NOT
        // also write WindowState = Normal here — that write races the WM's own async deiconify
        // and wins, so the window bounces back visible and cannot be minimized at all (measured
        // on KDE/XWayland). ShowPanel() sets WindowState = Normal on the restore side, which is
        // where it belongs. Gated on _armed so a transient _NET_WM_STATE_HIDDEN during X11 map
        // cannot hide the panel before the user has ever seen it (#62).
        PropertyChanged += (_, e) =>
        {
            if (e.Property != WindowStateProperty) return;
            // Any non-minimized state means the window is genuinely up — arm immediately rather
            // than waiting out the grace timer.
            if (WindowState != WindowState.Minimized) { _armed = true; return; }
            if (_armed && _services.Vm.MinimizeToTray) Hide();
        };
```

Note the wording is now consistently **deiconify** — the old comment said "iconify" in one sentence and "deiconify" in the next, and the race is against the deiconify.

- [ ] **Step 4: Build and confirm zero warnings**

Run: `dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj`
Expected: `Build succeeded.` with `0 Warning(s)` and `0 Error(s)`.

- [ ] **Step 5: Run the Core test suite as a regression guard**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
Expected: all tests pass, 0 failed. Nothing in this task touches `Core`, so any failure means something unrelated is broken — stop and report rather than pressing on.

- [ ] **Step 6: Correct the CLAUDE.md minimize-to-tray paragraph**

In `CLAUDE.md`, find these three lines inside the **Minimize to tray:** paragraph:

```
which is where it belongs. The minimize handler is also latched on an `_opened` flag (set from
the `Opened` event, never reset) so a transient `WindowState == Minimized` during X11 map, or a
WM restoring a previously-iconified state, can't `Hide()` the window before it was ever shown.
```

Replace with:

```
which is where it belongs. The minimize handler is gated on an `_armed` flag so a transient
`_NET_WM_STATE_HIDDEN` during X11 map can't `Hide()` the panel before the user has ever seen it
(#62). **An `Opened` latch does not work here** — `Window.ShowCore` raises `Opened`
*synchronously* after `PlatformImpl.Show`, and `X11Window.Show` is only `XMapWindow` + `XFlush`,
so every `WindowState` transition arrives later from a `PropertyNotify` and the latch is already
true. `_armed` is set by whichever lands first: a transition to a non-`Minimized` state (the WM
clearing the transient), or a 1 s `DispatcherTimer.RunOnce` grace from `Opened`. Cost: a genuine
minimize inside that first second leaves an ordinary minimized window instead of a tray hide.
```

- [ ] **Step 7: Restate verification row 11**

In `docs/superpowers/verification/2026-08-06-minimize-to-tray.md`, replace row 11 with:

```
| 11 | Launch and quit the app five times in a row | The panel window appears every time. #62 replaced the no-op `_opened` latch with `_armed` (armed by the first non-`Minimized` `WindowState` transition, or a 1 s grace timer from `Opened`, whichever lands first) — this row is the gate on that fix |
```

- [ ] **Step 8: Commit**

```bash
git add src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs CLAUDE.md docs/superpowers/verification/2026-08-06-minimize-to-tray.md
git commit -m "fix: gate minimize-to-tray on a real arm signal, not the no-op Opened latch (#62)"
```

---

### Task 2: #61 — scroll the native effect-error line into view

**Files:**
- Modify: `src/CameraOnScreen.App.Avalonia/MainWindow.axaml` (the `StatusError` `TextBlock`, around line 164)
- Modify: `src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs` (constructor)
- Modify: `docs/superpowers/verification/2026-08-06-minimize-to-tray.md` (new row)

**Interfaces:**
- Consumes: `MainWindow` constructor from Task 1. Add the new subscription **after** the existing `_services.Vm.PropertyChanged` block and **before** the `Closing` handler; do not touch the `_armed` handler.
- Produces: nothing consumed by later tasks.

**Scope note, do not widen:** only `StatusError` gets this. `CapabilityDetail` and `EyeContactDetail` are deliberately excluded — they become visible at startup when the async capability probe lands, so auto-scrolling them would open the panel pre-scrolled to the bottom of the effects card on every launch on any non-RTX machine. That is a worse defect than the one being fixed.

- [ ] **Step 1: Name the `StatusError` TextBlock**

In `MainWindow.axaml`, find:

```xml
                            <TextBlock Text="{Binding StatusError}" TextWrapping="Wrap"
                                       Foreground="{StaticResource Danger}"
                                       IsVisible="{Binding StatusError, Converter={x:Static ObjectConverters.IsNotNull}}" />
```

Replace with:

```xml
                            <TextBlock x:Name="StatusErrorText"
                                       Text="{Binding StatusError}" TextWrapping="Wrap"
                                       Foreground="{StaticResource Danger}"
                                       IsVisible="{Binding StatusError, Converter={x:Static ObjectConverters.IsNotNull}}" />
```

- [ ] **Step 2: Subscribe to the visibility edge in the constructor**

In `MainWindow.axaml.cs`, immediately after the closing `};` of the `_services.Vm.PropertyChanged += (_, e) => { ... };` block and before `Closing += ...`, insert:

```csharp
        // #61: the panel is 440x720 and its ScrollViewer content overflows, so this error line —
        // the last child of the effects card — can appear below the fold. Scroll it in when it
        // shows. Posted at Loaded priority, not called inline: the IsVisible change fires before
        // layout has measured the newly-visible element, so an inline BringIntoView() would
        // scroll to a stale rect. Deliberately NOT applied to CapabilityDetail/EyeContactDetail:
        // those go visible when the startup capability probe lands, so scrolling them would open
        // the panel pre-scrolled to the bottom on every launch on non-RTX hardware.
        StatusErrorText.PropertyChanged += (_, e) =>
        {
            if (e.Property == IsVisibleProperty && StatusErrorText.IsVisible)
                Dispatcher.UIThread.Post(() => StatusErrorText.BringIntoView(), DispatcherPriority.Loaded);
        };
```

`IsVisibleProperty` resolves from `Avalonia.Visual` (the `Avalonia` using is already present); `Dispatcher` and `DispatcherPriority` come from the existing `Avalonia.Threading` using. Add no new usings.

If the build reports that `StatusErrorText` does not exist, the XAML in Step 1 was not saved — the generated field comes from `x:Name`.

- [ ] **Step 3: Build and confirm zero warnings**

Run: `dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj -t:Rebuild`
Expected: `Build succeeded.` with `0 Warning(s)` and `0 Error(s)`. `-t:Rebuild` is used here because the XAML changed.

- [ ] **Step 4: Add the verification row**

In `docs/superpowers/verification/2026-08-06-minimize-to-tray.md`, add a row 18 after row 17:

```
| 18 | #61 scroll gate (instrumented, not an organic repro): temporarily change the `StatusErrorText` binding to a literal `Text="TEST ERROR"`, rebuild, launch, scroll the panel to the top, then let the panel finish its capability probe | The panel scrolls the red error line into view without the user scrolling. Revert the literal binding afterwards and rebuild |
```

- [ ] **Step 5: Commit**

```bash
git add src/CameraOnScreen.App.Avalonia/MainWindow.axaml src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs docs/superpowers/verification/2026-08-06-minimize-to-tray.md
git commit -m "fix: scroll the native effect-error line into view when it appears (#61)"
```

---

### Task 3: ship `cos.png` in the Linux output tree

**Files:**
- Modify: `scripts/publish-linux.sh` (near the existing `cp THIRD-PARTY-NOTICES.md "$OUT/"` line)

**Interfaces:**
- Consumes: nothing.
- Produces: `cos.png` at the root of `dist/linux` and therefore at the root of the release tarball. Task 4's `.desktop` entry points `Icon=` at it.

**Why:** `cos.png` is linked into the app as an `AvaloniaResource`, so it is embedded in the assembly and there is no image file on disk. A `.desktop` entry needs a real path.

- [ ] **Step 1: Copy the icon into the output tree**

In `scripts/publish-linux.sh`, find:

```bash
cp THIRD-PARTY-NOTICES.md "$OUT/"
```

Replace with:

```bash
cp THIRD-PARTY-NOTICES.md "$OUT/"
# Desktop-entry icon: cos.png is an AvaloniaResource (embedded in the assembly), so the tree
# would otherwise carry no image file for scripts/install.sh's .desktop Icon= to point at.
cp cos.png "$OUT/"
```

- [ ] **Step 2: Verify the script still parses**

Run: `bash -n scripts/publish-linux.sh`
Expected: no output, exit 0.

A full `publish-linux.sh` run needs the Maxine SDK trees and takes minutes; it is not required for this task. The icon's presence is confirmed by Task 4's smoke check, which extracts a real tarball.

- [ ] **Step 3: Commit**

```bash
git add scripts/publish-linux.sh
git commit -m "build: ship cos.png in the Linux tree so the desktop entry has an icon"
```

---

### Task 4: `scripts/install.sh`

**Files:**
- Create: `scripts/install.sh`

**Interfaces:**
- Consumes: `cos.png` at the tarball root (Task 3).
- Produces: the install command the README quotes in Task 5:
  `curl -fsSL https://raw.githubusercontent.com/officialdad/camera-on-screen/main/scripts/install.sh | bash`

**Constraints specific to this task:**
- The script must read **nothing** from stdin. Under `curl | bash` the script itself is stdin, so any `read` would consume the script's own remaining bytes.
- No `sudo`, no PATH edits, no writes outside `$DEST` and the applications directory.

- [ ] **Step 1: Write the script**

Create `scripts/install.sh` with exactly this content:

```bash
#!/usr/bin/env bash
# One-command installer for the Camera-on-Screen Linux build.
#
#   curl -fsSL https://raw.githubusercontent.com/officialdad/camera-on-screen/main/scripts/install.sh | bash
#
# Re-running replaces an existing install — that is also the upgrade path.
# No sudo, no PATH changes; everything lands under $HOME.
#
# Reads NOTHING from stdin: under `curl | bash` this script IS stdin, so a stray `read` would
# eat the rest of itself.
#
# Env overrides (used by the smoke check, not by end users):
#   COS_INSTALL_DIR      destination instead of ~/.local/share/camera-on-screen
#   COS_INSTALL_TARBALL  install from a local tarball instead of downloading a release
set -euo pipefail

REPO="officialdad/camera-on-screen"
EXE="CameraOnScreen.App.Avalonia"
DEST="${COS_INSTALL_DIR:-$HOME/.local/share/camera-on-screen}"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"

for tool in curl tar zstd; do
  command -v "$tool" >/dev/null 2>&1 \
    || { echo "ERROR: '$tool' is required but not installed." >&2; exit 1; }
done
# --zstd landed in GNU tar 1.31; older tars fail mid-extract instead of up front.
tar --help 2>/dev/null | grep -q -- '--zstd' \
  || { echo "ERROR: your tar does not support --zstd (needs GNU tar 1.31 or newer)." >&2; exit 1; }

# Overwriting a running install leaves a half-swapped tree behind the running process.
if pgrep -f "$EXE" >/dev/null 2>&1; then
  echo "ERROR: Camera-on-Screen is running. Quit it (tray icon -> Quit), then re-run this." >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ -n "${COS_INSTALL_TARBALL:-}" ]; then
  TARBALL="$COS_INSTALL_TARBALL"
  echo "Installing from $TARBALL"
else
  echo "Finding the latest release..."
  # The asset name carries the version, so there is no fixed /releases/latest/download/ URL.
  URL="$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" \
    | grep -o '"browser_download_url": *"[^"]*linux-x64\.tar\.zst"' \
    | head -1 | cut -d'"' -f4)"
  [ -n "$URL" ] || { echo "ERROR: no Linux release asset found for $REPO." >&2; exit 1; }
  echo "Downloading $(basename "$URL") — about 1.9 GB, this takes a while..."
  TARBALL="$TMP/cos.tar.zst"
  curl -fL --progress-bar "$URL" -o "$TARBALL"
fi

echo "Installing to $DEST ..."
rm -rf "$DEST"
mkdir -p "$DEST"
# The release tarball is packed flat (tar -C "$OUT" -cf "$PKG" .), so there is no top dir.
tar -C "$DEST" --zstd -xf "$TARBALL"
[ -f "$DEST/$EXE" ] || { echo "ERROR: '$EXE' missing from the archive." >&2; exit 1; }
chmod +x "$DEST/$EXE"

mkdir -p "$APPS"
cat > "$APPS/camera-on-screen.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Camera-on-Screen
Comment=Always-on-top webcam overlay for recording and screen sharing
Exec=$DEST/$EXE
Icon=$DEST/cos.png
Terminal=false
Categories=AudioVideo;Video;
EOF

cat <<EOF

Done. Launch "Camera-on-Screen" from your application menu, or run:
  $DEST/$EXE

To uninstall:
  rm -rf "$DEST" "$APPS/camera-on-screen.desktop"
EOF
```

- [ ] **Step 2: Make it executable and syntax-check it**

```bash
chmod +x scripts/install.sh
bash -n scripts/install.sh
```

Expected: no output, exit 0.

- [ ] **Step 3: Verify the running-process guard fires**

With the app running (`dotnet run --project src/CameraOnScreen.App.Avalonia &` or an existing install), run:

```bash
COS_INSTALL_DIR=$(mktemp -d) bash scripts/install.sh
```

Expected: exits non-zero with `ERROR: Camera-on-Screen is running.` and nothing is written. Then quit the app before continuing.

- [ ] **Step 4: Smoke-test a real install into a temp directory**

If a locally built tarball exists (`ls dist/CameraOnScreen-*-linux-x64.tar.zst`), use it — it avoids a 1.9 GB download:

```bash
TESTDIR=$(mktemp -d)
COS_INSTALL_DIR="$TESTDIR/app" \
  COS_INSTALL_TARBALL=$(ls dist/CameraOnScreen-*-linux-x64.tar.zst | head -1) \
  XDG_DATA_HOME="$TESTDIR/share" \
  bash scripts/install.sh
```

Otherwise drop `COS_INSTALL_TARBALL` and let it download the real release.

Expected: it prints `Done.`, and all three of these hold:

```bash
test -x "$TESTDIR/app/CameraOnScreen.App.Avalonia" && echo "exe OK"
test -f "$TESTDIR/app/cos.png" && echo "icon OK"
grep -q "^Exec=$TESTDIR/app/CameraOnScreen.App.Avalonia$" "$TESTDIR/share/applications/camera-on-screen.desktop" && echo "desktop OK"
```

The `icon OK` line only passes against a tarball built **after** Task 3. If the available tarball predates it, note that in the task report rather than editing the script to hide it.

Clean up: `rm -rf "$TESTDIR"`

- [ ] **Step 5: Commit**

```bash
git add scripts/install.sh
git commit -m "feat: one-command Linux installer with a desktop entry"
```

---

### Task 5: rewrite the README as a user-facing document

**Files:**
- Modify: `README.md` (full rewrite below the badges)

**Interfaces:**
- Consumes: the install one-liner from Task 4.
- Produces: nothing consumed by later tasks.

**Why:** the current README is half user guide and half engineering reference — Maxine SDK version pins, CUDA/TensorRT co-versioning, build commands, CI topology. All of that already lives in `CLAUDE.md` and `CONTRIBUTING.md`.

- [ ] **Step 1: Replace the whole README**

Write `README.md` with exactly this content:

````markdown
<p align="center">
  <img src="cos.png" alt="Camera-on-Screen" width="200">
</p>

<h1 align="center">Camera-on-Screen</h1>

<p align="center">
  I made this because I'm too lazy to do any post editing.
</p>

<p align="center">
  <a href="https://github.com/officialdad/camera-on-screen/actions/workflows/ci-linux.yml"><img src="https://github.com/officialdad/camera-on-screen/actions/workflows/ci-linux.yml/badge.svg" alt="ci-linux"></a>
  <a href="https://github.com/officialdad/camera-on-screen/actions/workflows/ci.yml"><img src="https://github.com/officialdad/camera-on-screen/actions/workflows/ci.yml/badge.svg" alt="ci"></a>
</p>

---

An always-on-top webcam overlay for **Linux and Windows**. It floats your live
camera feed over everything else, so any screen recorder or screen sharing
session captures you and your screen together in real time — no post editing.
Useful for teaching too.

AI features:

- **AI Green Screen** — removes your background
- **AI Eye Contact** — gently redirects your gaze toward the camera
- **Smooth 60 fps** — doubles the overlay's frame rate

> **What you need:** AI Eye Contact and Smooth 60 fps need an **NVIDIA RTX
> graphics card**. **AI Green Screen works on any computer.** Without an RTX
> card everything else still runs — the unavailable options simply grey out.

## Install

### Linux

One command. It downloads the latest release, installs it under your home
folder, and adds Camera-on-Screen to your application menu. No admin rights.

```bash
curl -fsSL https://raw.githubusercontent.com/officialdad/camera-on-screen/main/scripts/install.sh | bash
```

Re-run the same command later to upgrade.

Piping a script straight into `bash` runs it before you can read it. To read it
first:

```bash
curl -fsSL -O https://raw.githubusercontent.com/officialdad/camera-on-screen/main/scripts/install.sh
less install.sh     # read it
bash install.sh
```

Needs `curl`, `tar` and `zstd`, which nearly every desktop Linux already has.
The download is about 1.9 GB — everything is bundled, so there is nothing else
to install.

To uninstall:

```bash
rm -rf ~/.local/share/camera-on-screen ~/.local/share/applications/camera-on-screen.desktop
```

### Windows

Windows installers are paused for now — the latest one is
[**v0.6.0**](https://github.com/officialdad/camera-on-screen/releases/tag/v0.6.0)
(`CameraOnScreen-Setup-0.6.0-x64.exe`). It installs **per-user** (no admin) and
bundles everything it needs. Windows SmartScreen will warn that it is unsigned:
click **More info → Run anyway**. Uninstall from **Settings → Apps**.

## Using it

- **Move it** — drag the centre **+** handle (Windows) or drag anywhere (Linux).
- **Resize it** — scroll the mouse wheel over the overlay.
- **Mirror / zoom** — toggle in the control panel.
- **Pick a camera** — switch cameras from the control panel while it is running.
- **Minimize to tray** (Linux) — closing or minimizing the control panel sends
  it to the system tray while the overlay keeps running. Click the tray icon, or
  use its **Show Panel** menu item, to bring the panel back; the tray menu can
  also start/stop the camera and quit. Turn it off with the **Minimize to tray**
  toggle in the control panel.
- **AI Green Screen** — removes your background, with adjustable edge expand and
  feather.
- **AI Eye Contact** — gently redirects your gaze toward the camera.
- **Smooth 60 fps** — doubles the overlay's frame rate.

Then record with OBS, NVIDIA ShadowPlay or Game Bar, or just share your screen —
the overlay is live in the capture, in real time.

## Settings and troubleshooting

Your preferences live in `~/.config/CameraOnScreen/config.json` on Linux and
`%LOCALAPPDATA%\CameraOnScreen\config.json` on Windows.

**The panel disappeared and there is no tray icon.** A few Linux desktops have
no system tray at all, so "minimize to tray" hides the panel with no way back.
Recover with:

```bash
pkill -f CameraOnScreen.App.Avalonia
```

then set `"MinimizeToTray": false` in `~/.config/CameraOnScreen/config.json` and
launch again. Edit the file only while the app is closed — a running app never
re-reads it.

## Under the hood

Curious how it works, or want to build it yourself? See
[`CLAUDE.md`](CLAUDE.md) for the architecture, build and runtime details, and
[`CONTRIBUTING.md`](CONTRIBUTING.md) for how to get set up and the bar pull
requests need to clear.

## Contributing

Issues and pull requests are welcome — start with
[`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

[MIT](LICENSE). The bundled NVIDIA Maxine runtime is governed separately under
the NVIDIA Maxine SDK License — see
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md). NVIDIA, Maxine, and RTX are
trademarks of NVIDIA Corporation; this project is not affiliated with NVIDIA.
````

- [ ] **Step 2: Check every relative link resolves**

```bash
for f in cos.png LICENSE CLAUDE.md CONTRIBUTING.md THIRD-PARTY-NOTICES.md; do
  test -e "$f" && echo "OK $f" || echo "MISSING $f"
done
```

Expected: five `OK` lines. If `CONTRIBUTING.md` is missing, stop and report — do not silently drop the link.

- [ ] **Step 3: Confirm the technical content is actually gone**

```bash
grep -niE 'tensorrt|cuda|vcxproj|msbuild|cmake|COS_[A-Z_]+|dotnet (build|test|run)|self-hosted' README.md
```

Expected: **no matches**. A match means engineering detail survived the rewrite.

- [ ] **Step 4: Confirm the attribution survived**

```bash
grep -q 'trademarks of NVIDIA Corporation' README.md && echo "attribution OK"
```

Expected: `attribution OK`. This line is a Maxine SDK License obligation and must not be dropped.

- [ ] **Step 5: Commit**

```bash
git add README.md
git commit -m "docs: rewrite the README for users, with a one-command Linux install"
```

---

## Final gates before the PR

Run these in the worktree once every task is committed.

- [ ] `dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj -t:Rebuild` — 0 warnings, 0 errors
- [ ] `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj` — all green
- [ ] `bash -n scripts/install.sh && bash -n scripts/publish-linux.sh` — both parse
- [ ] **Human gate #62:** row 11 of `docs/superpowers/verification/2026-08-06-minimize-to-tray.md` — launch and quit five times, panel appears every time
- [ ] **Human gate #61:** row 18 of the same file — the instrumented scroll check
- [ ] PR body states plainly which human gates were run and which were not. Do not claim a gate that was not actually executed.
