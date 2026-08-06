# Design: startup self-hide fix, error-line visibility, and a user-facing README

Date: 2026-08-07
Issues: [#62](https://github.com/officialdad/camera-on-screen/issues/62),
[#61](https://github.com/officialdad/camera-on-screen/issues/61)
Branch: `fix/61-62-readme` (off `main` @ `f5b555c`, which carries both #60 and #63)

Three changes ship together because they are all panel-facing polish on the freshly merged
minimize-to-tray work, and because the README rewrite depends on the install story that lands
here.

## 1. #62 — the panel can hide itself at startup

### Problem

`MainWindow`'s minimize-to-tray handler hides the window on any `WindowState == Minimized`
transition. The `_opened` latch meant to gate it is a no-op on X11:

- `Window.ShowCore` raises `OnOpened` **synchronously**, right after `PlatformImpl.Show(...)`.
- `X11Window.Show` is `XMapWindow` + `XFlush`; no event pump runs in between.
- Every `WindowState` transition therefore arrives **later**, from a `PropertyNotify` on
  `_NET_WM_STATE` (`_NET_WM_STATE_HIDDEN` maps to `Minimized`).

So `_opened` is already `true` before any state change can be observed. A transient post-map
`_NET_WM_STATE_HIDDEN` still hides the panel. `MinimizeToTray` defaults to `true`, so this is
armed for every Linux user; on a desktop with no StatusNotifierItem host the result is a live
process with no window **and** no tray icon, recoverable only by `pkill`.

### Design

Replace `_opened` with `_armed`, set from whichever of two signals arrives first:

- **A `WindowState` transition to any non-`Minimized` value.** That is the window manager
  clearing its transient `_NET_WM_STATE_HIDDEN` — proof the window is genuinely up. This is the
  fast path.
- **A 1 s grace timer from `Opened`**, via `DispatcherTimer.RunOnce` (present in Avalonia
  11.2.1). Covers the quiet launch where the WM emits no state transition at all.

The `Minimized` branch hides only once `_armed`.

```csharp
private bool _armed;

Opened += (_, _) => DispatcherTimer.RunOnce(() => _armed = true, TimeSpan.FromSeconds(1));

PropertyChanged += (_, e) =>
{
    if (e.Property != WindowStateProperty) return;
    if (WindowState != WindowState.Minimized) { _armed = true; return; }
    if (_armed && _services.Vm.MinimizeToTray) Hide();
};
```

### Accepted behaviour cost

A user who genuinely minimizes within the first second of launch gets an ordinary minimized
window instead of a tray hide. Recoverable in one click, and strictly better than the failure it
replaces. If a WM emits a transient `HIDDEN` it never clears, the panel sits minimized in the
taskbar rather than vanishing — also recoverable.

`ShowPanel()` writes `WindowState = Normal`, which sets `_armed` again. Harmless: it is already
set by then.

### Documentation corrected in the same change

These currently describe a protection the code does not provide:

- `src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs` — the `_opened` field comment and the
  handler comment.
- The same handler comment says "races the WM's own async **iconify**" and then "beat the WM's
  async **deiconify**". The race is against the **deiconify**; the wording settles on that.
- `CLAUDE.md`, minimize-to-tray paragraph — the startup-latch clause.
- `docs/superpowers/verification/2026-08-06-minimize-to-tray.md` row 11 — restate what the row
  now proves.

## 2. #61 — the error line can sit below the fold

### Problem

The panel is 440x720 with a `ScrollViewer` whose content overflows by several hundred pixels.
Three explanatory lines are the last children of their cards, so they can be off-screen when they
appear: `StatusError` (red, native effect errors), `CapabilityDetail` and `EyeContactDetail`
(both amber availability notes).

### Design — `StatusError` only

When `StatusError`'s `TextBlock` goes from hidden to visible, scroll it into view. The two amber
notes are deliberately **out of scope**, and this is the load-bearing decision:

`CapabilityDetail` and `EyeContactDetail` become visible at **startup**, the moment the async
capability probe lands. Auto-scrolling them would make the panel open pre-scrolled to the bottom
of the effects card on every launch on any non-RTX machine — a worse defect than the one being
fixed. #61's own "mitigating factor" paragraph already argues these two are tolerable where they
are.

`StatusError` is different in kind: transient, event-driven, and rare, so a scroll responds to
something that just happened rather than becoming a permanent layout side effect.

Implementation: `x:Name` on the `StatusError` `TextBlock`; subscribe to its `IsVisibleProperty`
change in code-behind; on the false-to-true edge call `BringIntoView()`.

`BringIntoView()` must be **posted at `DispatcherPriority.Loaded`**, not called inline. The
property change fires before layout measures the newly-visible element, so an inline call scrolls
to a stale rect.

#59's placement decision stands — the error stays next to the control it belongs to. Nothing
moves out of the `ScrollViewer`.

### Verification

`StatusError` has no convenient on-demand trigger; it comes from a real native effect failure via
`cos_get_status`. The gate is an explicitly instrumented check: temporarily bind the `TextBlock`
to a constant string, confirm the panel scrolls it into view from a scrolled-to-top state, then
revert. This is stated as instrumented rather than presented as an organic reproduction.

## 3. `scripts/install.sh` and the README rewrite

### Install script

New `scripts/install.sh`, no sudo, no PATH edits:

1. Check `curl`, `tar` (>= 1.31, for `--zstd`) and `zstd` are present; fail with a clear message
   naming what is missing.
2. Refuse to run if the app is live (`pgrep -f CameraOnScreen.App.Avalonia`) — overwriting a
   running install is a silent half-broken upgrade.
3. Resolve the latest release asset through the GitHub API (the release asset name carries the
   version, so there is no fixed download URL).
4. Download and extract to `~/.local/share/camera-on-screen`, replacing any existing install.
   That replacement *is* the upgrade path.
5. Write `~/.local/share/applications/camera-on-screen.desktop` with an absolute `Icon=` path
   into the install directory.
6. Print the run command and the uninstall command.

It must read nothing from stdin: under `curl | bash` the script *is* stdin.

`COS_INSTALL_DIR` overrides the destination, which is also the script's runnable check — it can
be smoke-run into a temporary directory without touching a real install.

### Icon

`dist/linux` carries no image; `cos.png` is an embedded `AvaloniaResource`, not a file on disk,
so the `.desktop` entry has nothing to point at. `scripts/publish-linux.sh` copies `cos.png` into
`dist/linux`, so the tarball carries it. This also fixes the plain download-and-extract path, not
just the script path.

### Accepted risk, documented not hidden

The README's headline install is `curl -fsSL .../main/scripts/install.sh | bash`. That executes
remote code before the user can read it, and it is served from `main`'s tip rather than a pinned
tag, so a bad commit to `main` breaks installs immediately. Mitigations: the script stays small
and readable, and the README shows the download-inspect-run form directly beneath the one-liner.
No shellcheck CI job — that is YAGNI until the script actually breaks.

### README

The README becomes user-facing only.

**Keeps:** hero, what it is, features in plain language, Install (Linux one-liner + Windows),
Using it, Contributing, License.

**Cuts:** the entire `# Technical details` half — architecture, Maxine SDK detail, the ONNX CPU
engine, the Optical Flow SDK, Build, CI/Release. Replaced by a short *Under the hood* line
linking `CLAUDE.md`, `CONTRIBUTING.md` and `THIRD-PARTY-NOTICES.md`.

**NVIDIA in the user half** reduces to one plain sentence: AI Eye Contact and Smooth 60 fps need
an NVIDIA RTX graphics card, and AI Green Screen works on any computer. No SDK names, versions,
CUDA or TensorRT. The License section **keeps** the NVIDIA trademark and attribution line — the
Maxine SDK License Supplement requires attribution, so that is a legal obligation rather than a
technical detail.

**Also removed:** the *Hand grab* bullet under "Using it". That feature was retired in PR #52 and
the bullet is stale.

The existing tray-recovery paragraph stays, still accurate: with no StatusNotifierItem host the
panel can hide with no way back, and the recovery is `pkill` then `"MinimizeToTray": false` in
`config.json`.

## Testing

No new unit tests. Both code changes sit in the Avalonia UI layer, where window state and
scroll-viewer behaviour need a real desktop session — the repo already holds this line (Core is
unit-tested; the UI layer is gated by written human checklists).

- Core's existing test suite must stay green, and the Avalonia app must build with 0 warnings.
- **#62 gate:** re-run row 11 of `docs/superpowers/verification/2026-08-06-minimize-to-tray.md`
  — launch and quit five times, panel appears every time.
- **#61 gate:** the instrumented scroll check described above, added as a new row to the same
  verification file.
- **install.sh gate:** run with `COS_INSTALL_DIR` pointed at a temp directory; confirm it
  downloads, extracts a runnable tree, writes a valid `.desktop`, and that the running-process
  guard fires when the app is up.

## Out of scope

- Windows / WinUI. `MinimizeToTray` is Linux-only, parked on #38.
- Auto-scrolling the two amber availability notes — see the #61 section for why this is a
  deliberate exclusion rather than an omission.
- Pinning the install script to a release tag.
