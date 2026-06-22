# M5 Installer — human verification (RTX 3090)

Date: 2026-06-22
Issue: #1 (installer)
Plan: `docs/superpowers/plans/2026-06-22-camera-on-screen-m5-installer.md`

## Result: PASS

The Inno Setup installer builds, installs per-user with no admin prompt, and the installed app
launches from the Start Menu with both Maxine effects working — no environment variables.

## Artifact

- `dist\CameraOnScreen-Setup-0.1.0-x64.exe` — **632,393,562 bytes (0.59 GB)**.
- Staged from `dotnet build -c Release -r win-x64 -p:SelfContained=true -p:Platform=x64`
  (self-contained .NET runtime) + the bundler-produced `maxine\` (~1.3 GB on disk → 0.59 GB
  compressed by LZMA2/ultra64 solid, beating the ~700 MB target).

## Steps verified

1. **Build** — `scripts\build-installer.ps1 -Version 0.1.0` (with `COS_VFX_SDK_DIR` /
   `COS_AR_SDK_DIR` / `COS_VFX_RUNTIME_DIR` set): shim built SDK config, export-verify passed
   (GreenScreen + GazeRedirection, not stub), staged, bundled, ISCC compiled. Exit 0.
2. **Install** — per-user to `%LOCALAPPDATA%\Programs\CameraOnScreen`, no UAC/admin prompt.
   No SmartScreen prompt (locally built `.exe` has no Mark-of-the-Web; a *downloaded* unsigned
   build will show the "More info → Run anyway" click-through).
3. **Launch** — from the Start Menu shortcut, no `COS_*` env vars: overlay shows the webcam;
   **green screen** works, **eye contact** works, both together. Effects resolve from
   `<app>\maxine\`.

## Bug found and fixed during this gate

The **first** installer installed fine but the app crashed on launch (post-install and from the
Start Menu). Root cause (via `%LOCALAPPDATA%\CameraOnScreen\startup-error.log`):
`Microsoft.UI.Xaml.Markup.XamlParseException` (HRESULT `0x802B000A`) at
`MainWindow.InitializeComponent`. `dotnet publish` for this unpackaged WinUI 3 app **drops the
app PRI + compiled XAML** (`CameraOnScreen.App.pri`, `App.xbf`, `MainWindow.xbf`) — present in
the `dotnet build` output, absent in the publish output. Fix (commit `a6b3ca9`): the orchestrator
stages via `dotnet build -p:SelfContained=true` (the repo's proven run path) instead of
`dotnet publish`; this bundles the .NET runtime **and** keeps the XAML resources. Recorded as a
gotcha in `CLAUDE.md`. The rebuilt installer passed all steps above.

## Notes / scope

- Effects are **Ampere (`_86`) only** this build; non-RTX machines install + run as a plain
  overlay (effects gated off, no crash).
- Handing the `.exe` to a private tester is acceptable (NVIDIA notices ride along in `maxine\`);
  **public** redistribution still gates on the full license review (issue #3).
