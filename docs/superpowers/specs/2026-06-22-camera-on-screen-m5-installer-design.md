# Camera-on-Screen M5 (installer) — Inno Setup packaging of the bundled app

Date: 2026-06-22
Status: Design approved; implementation plan next.
Tracks: GitHub issue #1.

## Purpose

The M5 **bundler** (`scripts/bundle-maxine.ps1`, spec
`2026-06-21-camera-on-screen-m5-bundler-design.md`) produces `<output>\maxine\` — the minimal
co-versioned Maxine runtime — beside a published `CameraOnScreen.App.exe` +
`CameraOnScreen.Shim.dll`. This spec packages that whole tree into a **single distributable
installer**: one `.exe` an end user double-clicks to get a working app on a clean RTX (Ampere)
machine with no env vars, no NVIDIA account, effects functional.

This is the last build-tooling piece before a public release. A tag-triggered `release.yml`
that runs this pipeline in CI and uploads the artifact is **out of scope** (issue #4); this
spec defines the installer script and the local build orchestrator it consumes.

## Decisions (from brainstorming)

- **Tech: Inno Setup 6.** Free, single-file `ISCC.exe` compiler (scriptable in CI for #4),
  LZMA2/ultra64 solid compression to hit the ~700 MB download target on the ~1.3 GB payload,
  and a natural fit for an **unpackaged self-contained** app (lay down a folder + Start Menu
  shortcut + uninstaller + Add/Remove Programs entry). Rejected: **WiX/MSI** (heavier authoring,
  clunky at 1.3 GB, enterprise features unused by a consumer overlay), **MSIX** (no MSIX tooling
  on this host and the app is deliberately unpackaged), **7-Zip SFX** (no uninstaller / Start
  Menu / Add-Remove — too crude to ship).
- **Per-user install, no admin.** `PrivilegesRequired=lowest` → installs to
  `{localappdata}\Programs\CameraOnScreen`. Friendlier for a consumer tool; avoids a UAC prompt.
- **Unsigned for now.** No code-signing cert exists. Ship unsigned and document the Windows
  **SmartScreen** click-through ("More info → Run anyway") in the README. Optional signing is
  wired into `release.yml` (#4) later if a cert is acquired — it does **not** block #1, and the
  installer functions identically signed or not.
- **Soft RTX preflight.** The app already degrades to a plain passthrough overlay when no Maxine
  runtime / RTX GPU is present (effects gate off, no crash). The installer therefore **warns**
  but never hard-blocks when no NVIDIA GPU is detected.
- **One orchestrator script** (`scripts/build-installer.ps1`) chains publish → bundle → compile,
  matching the repo ethos where each native/packaging step is an explicit script outside the
  sln (the shim build and the bundler are both this way).

## Inputs / sources

- **Published app staging dir** — produced by `dotnet publish` of
  `src/CameraOnScreen.App/CameraOnScreen.App.csproj` (`-c Release -r win-x64 --self-contained`).
  Self-contained WinApp SDK: the folder holds `CameraOnScreen.App.exe`,
  `CameraOnScreen.Shim.dll`, and all the WinApp SDK / Vortice runtime DLLs. The shim DLL is
  pulled in by the csproj `<None>` item, so the **SDK config of the shim must be built last**
  before publish (the CLAUDE.md DEPLOY-THE-RIGHT-SHIM gotcha — a passthrough shim here would
  silently ship effects-disabled).
- **`maxine\` runtime** — produced into the same staging dir by `bundle-maxine.ps1 -OutDir
  <staging>`, using the existing `COS_VFX_RUNTIME_DIR` / AR runtime sources (no new config).
- **License files** — repo `LICENSE` (MIT) shown on the installer license page;
  `THIRD-PARTY-NOTICES.md` plus the NVIDIA Maxine EULA + `ThirdPartyLicenses.txt` already
  travel inside `maxine\` (bundler ride-along).

## Components

### 1. Installer script — `installer\CameraOnScreen.iss`

An Inno Setup script, parameterized by preprocessor defines passed from the orchestrator:
`/DSourceDir=<staging>` (the publish+bundle output) and `/DAppVersion=<x.y.z>`.

- **`[Setup]`**
  - `AppId={{<fixed GUID>}}` — a single stable GUID (generated once, checked in) so upgrades and
    uninstall track one product across versions.
  - `AppName=Camera on Screen`, `AppVersion={#AppVersion}`,
    `AppPublisher`, `AppPublisherURL` = the public repo.
  - `PrivilegesRequired=lowest`, `DefaultDirName={localappdata}\Programs\CameraOnScreen`,
    `DefaultGroupName=Camera on Screen`.
  - `ArchitecturesAllowed=x64compatible`, `ArchitecturesInstallIn64BitMode=x64compatible`.
  - `Compression=lzma2/ultra64`, `SolidCompression=yes`, `LZMANumBlockThreads` left default.
  - `OutputDir=..\dist`, `OutputBaseFilename=CameraOnScreen-Setup-{#AppVersion}-x64`.
  - `WizardStyle=modern`, `LicenseFile=..\LICENSE`, `UninstallDisplayIcon={app}\CameraOnScreen.App.exe`.
  - `DisableProgramGroupPage=yes` (single app, no need to choose a group).
- **`[Files]`** — one recursing entry copying the entire staging tree:
  `Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion`.
  This carries `CameraOnScreen.App.exe`, `CameraOnScreen.Shim.dll`, every self-contained runtime
  DLL, and the full `maxine\` subtree (models + co-versioned DLLs).
- **`[Icons]`**
  - `Name: "{group}\Camera on Screen"; Filename: "{app}\CameraOnScreen.App.exe"`.
  - `Name: "{userdesktop}\Camera on Screen"; Filename: "{app}\CameraOnScreen.App.exe"; Tasks: desktopicon`.
- **`[Tasks]`** — `desktopicon` (unchecked by default).
- **`[Run]`** — `Filename: "{app}\CameraOnScreen.App.exe"; Description: "Launch Camera on Screen";
  Flags: nowait postinstall skipifsilent` (launch-on-finish checkbox).
- **`[Code]`** — soft RTX preflight in `InitializeSetup` (or `NextButtonClick` on the welcome
  page): query installed display adapters (registry under
  `HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-...}\` `DriverDesc`, or `Get-WMIObject`
  equivalent via an enumerated registry walk — no admin needed to read). If no adapter string
  contains "NVIDIA", show a non-blocking warning: *"No NVIDIA GPU detected. The AI effects (green
  screen, eye contact) require an NVIDIA RTX GPU; the app will still run as a plain webcam
  overlay. Continue anyway?"* Default Yes. Never abort the install on GPU grounds.

### 2. Build orchestrator — `scripts/build-installer.ps1`

```
build-installer.ps1
  -Version <x.y.z>          # default '0.0.0-dev'; stamped into the .iss and the output name
  -Configuration Release    # default Release; passed to dotnet publish
  -StagingDir <dir>         # default a clean temp/obj path; publish + bundle land here
  [-SkipShimBuild]          # opt out of the shim rebuild if the caller already built it
  [-VfxRuntime] [-ArRuntime]  # forwarded to bundle-maxine.ps1 (default the COS_* env vars)
```

Steps (fail loudly on any non-zero exit):
1. **(unless `-SkipShimBuild`)** Build the shim **SDK config** via Build Tools MSBuild
   (`shim.vcxproj /p:Configuration=Release /p:Platform=x64`) so the deployed DLL exports the
   real effects, not the stub.
2. Clean `StagingDir`; `dotnet publish src/CameraOnScreen.App/...csproj -c $Configuration
   -r win-x64 --self-contained -o $StagingDir`.
3. **Export-verify** the deployed shim (the CI gotcha, reused): assert
   `$StagingDir\CameraOnScreen.Shim.dll` exports `GreenScreen` **and** `GazeRedirection` and
   lacks `not built in` (via `dumpbin /exports` or a string scan). Abort if it's the stub.
4. `bundle-maxine.ps1 -OutDir $StagingDir [...]` → adds `$StagingDir\maxine\`; abort if missing.
5. Resolve `ISCC.exe` (PATH, then the default `Program Files (x86)\Inno Setup 6\ISCC.exe`); fail
   with an install hint (`winget install JRSoftware.InnoSetup`) if absent.
6. `ISCC.exe installer\CameraOnScreen.iss /DSourceDir=$StagingDir /DAppVersion=$Version`.
7. Report the produced `dist\CameraOnScreen-Setup-$Version-x64.exe` path and its size.

### 3. Uninstall behavior

Inno's generated uninstaller removes everything under `{app}` (the app, the shim, all runtime
DLLs, and the entire `maxine\` tree) and the Start Menu / desktop shortcuts and the Add/Remove
entry. It **does not** touch the user's config at `%LOCALAPPDATA%\CameraOnScreen\config.json`
(a separate dir, not under `{app}`) — standard "don't delete user data on uninstall" behavior.
No `[UninstallDelete]` of the config dir.

## Prerequisites (host + CI runner)

- **Inno Setup 6** must be installed where the installer is built (dev host and the self-hosted
  RTX CI runner for #4): `winget install JRSoftware.InnoSetup`, or the JRSoftware download.
  `ISCC.exe` is the only binary the orchestrator invokes. Document this in CLAUDE.md alongside
  the existing toolchain notes.
- The existing shim-build + Maxine-runtime prerequisites are unchanged (Build Tools MSBuild,
  the `COS_*` runtime sources the bundler already needs).

## Testing & verification

- **No Core unit tests.** This is a PowerShell orchestrator + an Inno Setup script — no managed
  surface, no C-ABI change. Existing Core tests and C-ABI parity are untouched and must stay
  green. (Mirrors the bundler and M4, which added no Core tests for native/packaging steps.)
- **Pester test — `scripts/Build-Installer.Tests.ps1`** (mirrors `Bundle-Maxine.Tests.ps1`),
  pure static checks that need no RTX / no Inno install:
  - `installer\CameraOnScreen.iss` exists and declares the load-bearing directives
    (`PrivilegesRequired=lowest`, `ArchitecturesAllowed`, `Compression=lzma2`,
    `OutputBaseFilename`, a `[Files]` recurse of `{#SourceDir}`, the `AppId` GUID present).
  - `build-installer.ps1` parses, exposes the documented params, and fails fast with a clear
    message when `ISCC.exe` cannot be resolved (mock/PATH-shadow the lookup).
- **Native build stays pristine (0 warnings)** — unchanged; this spec adds no native code.
- **Human gate (the real functional verification).** On the RTX 3090 box:
  1. `scripts/build-installer.ps1 -Version <v>` → produces `dist\CameraOnScreen-Setup-<v>-x64.exe`;
     record its size (validate against the ~700 MB compressed target).
  2. Install it (per-user, no admin), ideally on a **clean** machine / fresh user profile with
     **all `COS_*` env vars unset**.
  3. Launch from the Start Menu shortcut; confirm green screen **and** eye contact work alone and
     together; confirm uninstall removes the app but preserves `config.json`.
  4. Log results in `docs/superpowers/verification/`.

## Out of scope (deferred)

- **`release.yml` (#4)** — tag-triggered CI that runs `build-installer.ps1` on the self-hosted
  runner and uploads the `.exe` to a GitHub Release. This spec makes that a thin wrapper (call
  the orchestrator, attach the artifact); it adds no new packaging logic.
- **Code signing** — optional, additive in #4 when a cert exists; documented (SmartScreen
  click-through) but not implemented here.
- **Multi-GPU models (#2)** — the universal-installer model set. The bundler layout is already
  forward-compatible (append `_75`/`_89`/`_120` globs); the installer recurses `maxine\`
  wholesale, so a larger model set needs **no installer change** — only a payload-size note.
- **Full license review (#3)** — unchanged; the installer surfaces the MIT `LICENSE` and carries
  the bundled NVIDIA notices, but the full EULA-compliance review remains its own item.
