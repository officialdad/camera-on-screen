# Camera-on-Screen M5 Installer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Package the published self-contained app + bundler-produced `maxine\` runtime into one per-user Inno Setup installer (`dist\CameraOnScreen-Setup-<ver>-x64.exe`) that works on a clean RTX (Ampere) machine with no env vars.

**Architecture:** A PowerShell orchestrator (`scripts\build-installer.ps1`) chains: build shim SDK config → `dotnet publish` the App **.NET-self-contained** → export-verify the deployed shim → run `bundle-maxine.ps1` → compile `installer\CameraOnScreen.iss` with Inno Setup 6's `ISCC.exe`. The `.iss` lays down the staged tree, a Start Menu shortcut, an uninstaller, and a soft (non-blocking) NVIDIA-GPU preflight. Static behavior is covered by Pester; the functional gate is human-run on the RTX box.

**Tech Stack:** Inno Setup 6 (`ISCC.exe`), PowerShell 7, Pester 5.4, `dotnet publish`, MSVC `dumpbin` (export-verify), existing `scripts\bundle-maxine.ps1`.

## Global Constraints

- **Inno Setup 6 required** on the build host + CI runner — `winget install JRSoftware.InnoSetup`. `ISCC.exe` is the only binary invoked.
- **Publish must be `--self-contained true`.** The current `dotnet build` output is framework-dependent (`Microsoft.NETCore.App 8.0.0`); a consumer machine would otherwise need the .NET 8 Desktop Runtime. Self-contained → zero prereq.
- **Deploy-the-right-shim (CLAUDE.md gotcha).** Build the shim **SDK config last**, then export-verify the *deployed* DLL exports `GreenScreen` **and** `GazeRedirection` and lacks `not built in`, **before** packaging — else the installer silently ships effects-disabled.
- **Per-user, no admin** (`PrivilegesRequired=lowest`); **unsigned** (document the SmartScreen click-through); **x64 only**.
- **Effects are Ampere (`_86`) only** this build — the installer carries whatever `maxine\` the bundler produced; non-Ampere GPUs install + run as a plain overlay. State this in the README.
- **Fixed AppId GUID:** `{{6C6D5E07-D334-456C-9E31-2D0C3069BA89}` (double-brace is the Inno literal-`{` escape). Generated once; never change it (upgrade/uninstall identity).
- Pester tests must run **without** an RTX GPU, the Maxine SDK, or Inno installed (pure static / dry-run checks).

---

### Task 1: Inno Setup script — `installer\CameraOnScreen.iss`

**Files:**
- Create: `installer\CameraOnScreen.iss`
- Test: `scripts\Build-Installer.Tests.ps1` (new; add the `installer-iss` Describe block)

**Interfaces:**
- Consumes: preprocessor defines `SourceDir` (staged publish+bundle dir) and `AppVersion`, passed by the orchestrator as `/DSourceDir=… /DAppVersion=…`.
- Produces: `dist\CameraOnScreen-Setup-<AppVersion>-x64.exe` when compiled by `ISCC.exe`.

- [ ] **Step 1: Write the failing test**

Create `scripts\Build-Installer.Tests.ps1`:

```powershell
Describe 'installer-iss' {
    BeforeAll {
        $script:iss = Join-Path $PSScriptRoot '..\installer\CameraOnScreen.iss'
        $script:text = if (Test-Path $script:iss) { Get-Content -LiteralPath $script:iss -Raw } else { '' }
    }
    It 'the .iss file exists' { Test-Path $script:iss | Should -BeTrue }
    It 'pins the fixed AppId GUID' { $script:text | Should -Match '6C6D5E07-D334-456C-9E31-2D0C3069BA89' }
    It 'installs per-user (no admin)' { $script:text | Should -Match 'PrivilegesRequired=lowest' }
    It 'is x64-only' { $script:text | Should -Match 'ArchitecturesAllowed=x64compatible' }
    It 'uses LZMA2 solid compression' {
        $script:text | Should -Match 'Compression=lzma2'
        $script:text | Should -Match 'SolidCompression=yes'
    }
    It 'names the output CameraOnScreen-Setup-<ver>-x64' { $script:text | Should -Match 'OutputBaseFilename=CameraOnScreen-Setup-' }
    It 'recursively copies the staged SourceDir' {
        $script:text | Should -Match '\{#SourceDir\}'
        $script:text | Should -Match 'recursesubdirs'
    }
    It 'creates a Start Menu shortcut to the app exe' { $script:text | Should -Match 'CameraOnScreen\.App\.exe' }
    It 'has a soft NVIDIA preflight in [Code]' {
        $script:text | Should -Match '\[Code\]'
        $script:text | Should -Match 'NVIDIA'
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pwsh -NoProfile -Command "Invoke-Pester -Path scripts\Build-Installer.Tests.ps1 -Output Detailed"`
Expected: FAIL — `the .iss file exists` is false; the match assertions fail (empty text).

- [ ] **Step 3: Write the `.iss`**

Create `installer\CameraOnScreen.iss`:

```iss
; CameraOnScreen.iss — Inno Setup 6 script. Compiled by scripts\build-installer.ps1.
; The orchestrator passes /DSourceDir=<publish+bundle staging dir> and /DAppVersion=<x.y.z>.
; Defaults below let the script be opened/checked standalone.

#ifndef SourceDir
  #define SourceDir "..\dist\stage"
#endif
#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif

[Setup]
AppId={{6C6D5E07-D334-456C-9E31-2D0C3069BA89}
AppName=Camera on Screen
AppVersion={#AppVersion}
AppPublisher=officialdad
AppPublisherURL=https://github.com/officialdad/camera-on-screen
DefaultDirName={localappdata}\Programs\CameraOnScreen
DefaultGroupName=Camera on Screen
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2/ultra64
SolidCompression=yes
OutputDir=..\dist
OutputBaseFilename=CameraOnScreen-Setup-{#AppVersion}-x64
WizardStyle=modern
LicenseFile=..\LICENSE
UninstallDisplayIcon={app}\CameraOnScreen.App.exe
UninstallDisplayName=Camera on Screen

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\Camera on Screen"; Filename: "{app}\CameraOnScreen.App.exe"
Name: "{group}\Uninstall Camera on Screen"; Filename: "{uninstallexe}"
Name: "{userdesktop}\Camera on Screen"; Filename: "{app}\CameraOnScreen.App.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\CameraOnScreen.App.exe"; Description: "{cm:LaunchProgram,Camera on Screen}"; Flags: nowait postinstall skipifsilent

[Code]
// Soft preflight: warn (do not block) when no NVIDIA display adapter is present.
// The app degrades to a plain overlay without RTX, so installation must still proceed.
function HasNvidiaGpu(): Boolean;
var
  Names: TArrayOfString;
  I: Integer;
  Desc: String;
  Key: String;
begin
  Result := False;
  Key := 'SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}';
  if RegGetSubkeyNames(HKLM, Key, Names) then
  begin
    for I := 0 to GetArrayLength(Names) - 1 do
    begin
      if RegQueryStringValue(HKLM, Key + '\' + Names[I], 'DriverDesc', Desc) then
      begin
        if Pos('NVIDIA', Uppercase(Desc)) > 0 then
        begin
          Result := True;
          Exit;
        end;
      end;
    end;
  end;
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not HasNvidiaGpu() then
  begin
    if MsgBox('No NVIDIA GPU was detected.' + #13#10 + #13#10 +
              'The AI effects (green screen, eye contact) require an NVIDIA RTX GPU. ' +
              'The app will still install and run as a plain webcam overlay.' + #13#10 + #13#10 +
              'Continue anyway?', mbConfirmation, MB_YESNO) = IDNO then
      Result := False;
  end;
end;
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pwsh -NoProfile -Command "Invoke-Pester -Path scripts\Build-Installer.Tests.ps1 -Output Detailed"`
Expected: PASS — all `installer-iss` assertions green.

- [ ] **Step 5: Commit**

```bash
git add installer/CameraOnScreen.iss scripts/Build-Installer.Tests.ps1
git commit -m "feat(m5): installer .iss (Inno Setup, per-user, soft NVIDIA preflight) + Pester checks"
```

---

### Task 2: Build orchestrator — `scripts\build-installer.ps1`

**Files:**
- Create: `scripts\build-installer.ps1`
- Modify: `scripts\Build-Installer.Tests.ps1` (add the `build-installer` Describe block)

**Interfaces:**
- Consumes: `installer\CameraOnScreen.iss` (Task 1), `scripts\bundle-maxine.ps1` (existing), `src\CameraOnScreen.App\CameraOnScreen.App.csproj`, `native\shim\shim.vcxproj`.
- Produces: `dist\CameraOnScreen-Setup-<Version>-x64.exe`.
- Params: `-Version <str='0.0.0-dev'>`, `-Configuration <str='Release'>`, `-StagingDir <dir>`, `-VfxRuntime <dir>`, `-ArRuntime <dir>`, `-IsccPath <file>`, `-SkipShimBuild` (switch), `-DryRun` (switch).

- [ ] **Step 1: Write the failing test**

Append to `scripts\Build-Installer.Tests.ps1`:

```powershell
Describe 'build-installer' {
    BeforeAll { $script:s = Join-Path $PSScriptRoot 'build-installer.ps1' }

    It 'exposes the documented parameters' {
        $p = (Get-Command $script:s).Parameters.Keys
        foreach ($name in 'Version','Configuration','StagingDir','VfxRuntime','ArRuntime','IsccPath','SkipShimBuild','DryRun') {
            $p | Should -Contain $name
        }
    }
    It 'throws a clear, actionable error when ISCC cannot be resolved' {
        { & $script:s -IsccPath 'X:\nope\ISCC.exe' -DryRun } | Should -Throw '*Inno Setup*'
    }
    It 'dry-run prints the publish/bundle/compile plan stamped with the version' {
        $dummy = Join-Path ([IO.Path]::GetTempPath()) ("iscc_" + [guid]::NewGuid() + ".exe")
        'x' | Set-Content -LiteralPath $dummy
        try {
            $stage = Join-Path ([IO.Path]::GetTempPath()) ("stg_" + [guid]::NewGuid())
            $out = & $script:s -Version '9.9.9' -IsccPath $dummy -StagingDir $stage -DryRun 6>&1 | Out-String
            $out | Should -Match 'dotnet publish'
            $out | Should -Match 'self-contained'
            $out | Should -Match 'bundle-maxine\.ps1'
            $out | Should -Match 'ISCC'
            $out | Should -Match '9\.9\.9'
        }
        finally { Remove-Item -LiteralPath $dummy -Force -ErrorAction SilentlyContinue }
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pwsh -NoProfile -Command "Invoke-Pester -Path scripts\Build-Installer.Tests.ps1 -Output Detailed"`
Expected: FAIL — `build-installer` block errors because `build-installer.ps1` does not exist (`Get-Command` throws / `&` fails).

- [ ] **Step 3: Write the orchestrator**

Create `scripts\build-installer.ps1`:

```powershell
#Requires -Version 7
<#
.SYNOPSIS
  Build the distributable installer: publish the .NET-self-contained App, bundle the Maxine
  runtime beside it, then compile installer\CameraOnScreen.iss with Inno Setup 6.
  Explicit publish-time step (NOT wired into dotnet build). See the M5 installer spec.
#>
[CmdletBinding()]
param(
    [string]$Version = '0.0.0-dev',
    [string]$Configuration = 'Release',
    [string]$StagingDir,
    [string]$VfxRuntime = $env:COS_VFX_RUNTIME_DIR,
    [string]$ArRuntime  = $env:COS_AR_RUNTIME_DIR,
    [string]$IsccPath,
    [switch]$SkipShimBuild,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'

$repo     = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$iss      = Join-Path $repo 'installer\CameraOnScreen.iss'
$bundler  = Join-Path $PSScriptRoot 'bundle-maxine.ps1'
$appProj  = Join-Path $repo 'src\CameraOnScreen.App\CameraOnScreen.App.csproj'
$shimProj = Join-Path $repo 'native\shim\shim.vcxproj'
if (-not $StagingDir) { $StagingDir = Join-Path $repo 'dist\stage' }
$output   = Join-Path $repo "dist\CameraOnScreen-Setup-$Version-x64.exe"

function Resolve-Iscc {
    param([string]$Explicit)
    if ($Explicit) {
        if (Test-Path -LiteralPath $Explicit) { return $Explicit }
        throw "ISCC.exe not found at -IsccPath '$Explicit'. Install Inno Setup 6: winget install JRSoftware.InnoSetup"
    }
    $cmd = Get-Command 'ISCC.exe' -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $default = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'
    if (Test-Path -LiteralPath $default) { return $default }
    throw "ISCC.exe not found (PATH or '$default'). Install Inno Setup 6: winget install JRSoftware.InnoSetup"
}

function Assert-ShimHasEffects {
    param([string]$Dll)
    if (-not (Test-Path -LiteralPath $Dll)) { throw "shim DLL missing in staging: $Dll" }
    $dumpbin = (Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC" -Directory |
        Sort-Object Name -Descending | Select-Object -First 1).FullName + '\bin\Hostx64\x64\dumpbin.exe'
    $exports = if (Test-Path $dumpbin) { & $dumpbin /exports $Dll | Out-String } else { '' }
    $strings = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($Dll))
    $hasGS   = $exports -match 'GreenScreen'    -or $strings -match 'GreenScreen'
    $hasGaze = $exports -match 'GazeRedirection' -or $strings -match 'GazeRedirection'
    $isStub  = $strings -match 'not built in'
    Write-Host "shim check: GreenScreen=$hasGS GazeRedirection=$hasGaze stub=$isStub"
    if (-not $hasGS)   { throw "deployed shim lacks GreenScreen — green-screen effect not built in (built the stub last?)" }
    if (-not $hasGaze) { throw "deployed shim lacks GazeRedirection — eye-contact effect not built in" }
    if ($isStub)       { throw "deployed shim is the passthrough STUB ('not built in' present)" }
}

$isccExe = Resolve-Iscc -Explicit $IsccPath

if ($DryRun) {
    Write-Host "DRY RUN — installer build plan (version $Version):"
    Write-Host "  1. MSBuild $shimProj /p:Configuration=$Configuration /p:Platform=x64   (SkipShimBuild=$SkipShimBuild)"
    Write-Host "  2. dotnet publish $appProj -c $Configuration -r win-x64 --self-contained true -o $StagingDir"
    Write-Host "  3. export-verify $StagingDir\CameraOnScreen.Shim.dll (GreenScreen + GazeRedirection, not stub)"
    Write-Host "  4. bundle-maxine.ps1 -OutDir $StagingDir (VfxRuntime=$VfxRuntime ArRuntime=$ArRuntime)"
    Write-Host "  5. ISCC '$isccExe' '$iss' /DSourceDir=$StagingDir /DAppVersion=$Version"
    Write-Host "  -> $output"
    return
}

# 1. Native shim, SDK config, LAST (deploy-the-right-shim gotcha).
if (-not $SkipShimBuild) {
    $msbuild = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
    & $msbuild $shimProj /p:Configuration=$Configuration /p:Platform=x64 /warnaserror /nologo
    if ($LASTEXITCODE -ne 0) { throw "shim build failed ($LASTEXITCODE)" }
}

# 2. Publish App, .NET self-contained (no runtime prereq on the target machine).
if (Test-Path -LiteralPath $StagingDir) { Remove-Item -Recurse -Force -LiteralPath $StagingDir }
New-Item -ItemType Directory -Force -Path $StagingDir | Out-Null
dotnet publish $appProj -c $Configuration -r win-x64 --self-contained true -o $StagingDir --nologo
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed ($LASTEXITCODE)" }

# 3. Export-verify the deployed shim BEFORE packaging.
Assert-ShimHasEffects -Dll (Join-Path $StagingDir 'CameraOnScreen.Shim.dll')

# 4. Bundle the Maxine runtime into <staging>\maxine\.
& $bundler -OutDir $StagingDir -VfxRuntime $VfxRuntime -ArRuntime $ArRuntime
if (-not (Test-Path -LiteralPath (Join-Path $StagingDir 'maxine'))) { throw "bundler did not produce maxine\ in $StagingDir" }

# 5. Compile the installer.
New-Item -ItemType Directory -Force -Path (Join-Path $repo 'dist') | Out-Null
& $isccExe $iss "/DSourceDir=$StagingDir" "/DAppVersion=$Version"
if ($LASTEXITCODE -ne 0) { throw "ISCC compile failed ($LASTEXITCODE)" }

$size = (Get-Item -LiteralPath $output).Length
Write-Host ("installer -> {0}" -f $output)
Write-Host ("  size : {0:N0} bytes  ({1:N2} GB)" -f $size, ($size / 1GB))
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pwsh -NoProfile -Command "Invoke-Pester -Path scripts\Build-Installer.Tests.ps1 -Output Detailed"`
Expected: PASS — both `installer-iss` and `build-installer` blocks green (dry-run path needs no SDK/RTX/Inno).

- [ ] **Step 5: Commit**

```bash
git add scripts/build-installer.ps1 scripts/Build-Installer.Tests.ps1
git commit -m "feat(m5): build-installer.ps1 orchestrator (publish self-contained -> bundle -> ISCC) + dry-run tests"
```

---

### Task 3: Documentation — README Install section + CLAUDE.md prereq

**Files:**
- Modify: `README.md` (insert an `## Install` section after the Requirements blockquote, before `## What it is`)
- Modify: `CLAUDE.md` (add Inno Setup to the toolchain note + an Installer paragraph after the Bundler paragraph)

**Interfaces:** none (docs only).

- [ ] **Step 1: Add the README Install section**

In `README.md`, insert this block immediately after line 12 (the end of the `> **Requirements:** …` blockquote) and before `## What it is`:

```markdown

## Install

Download `CameraOnScreen-Setup-<ver>-x64.exe` (built by `scripts\build-installer.ps1`;
GitHub Releases once the release workflow lands) and run it. It installs **per-user**
(no admin) to `%LOCALAPPDATA%\Programs\CameraOnScreen`, adds a Start Menu shortcut, and
registers an uninstaller. The .NET runtime is bundled — no prerequisite install.

The installer is **unsigned**, so Windows SmartScreen may warn on first run: click
**More info → Run anyway**. Uninstalling removes the app but keeps your settings
(`%LOCALAPPDATA%\CameraOnScreen\config.json`).

> **AI effects in this build are for RTX **30-series (Ampere)** GPUs.** On other GPUs the
> app installs and runs as a plain webcam overlay with the effects disabled.
```

- [ ] **Step 2: Add the CLAUDE.md toolchain note**

In `CLAUDE.md`, in the `## Toolchain (host-specific, non-obvious)` list, add a bullet:

```markdown
- **Inno Setup 6** (`ISCC.exe`) is required to build the installer (issue #1):
  `winget install JRSoftware.InnoSetup`. Not needed for normal build/test.
```

- [ ] **Step 3: Add the CLAUDE.md Installer paragraph**

In `CLAUDE.md`, in the `## Maxine SDKs …` section, immediately after the `- **Bundler** …` bullet, add:

```markdown
- **Installer** (`scripts/bundle-maxine.ps1` consumer): `scripts/build-installer.ps1`
  publishes the App **.NET-self-contained**, export-verifies the deployed shim, runs the
  bundler, then compiles `installer/CameraOnScreen.iss` with Inno Setup 6 →
  `dist/CameraOnScreen-Setup-<ver>-x64.exe` (per-user, unsigned, x64). Effects are
  **Ampere-only** this build; non-RTX installs run as a plain overlay. Build the shim SDK
  config **last** before running (deploy-the-right-shim). `-DryRun` prints the plan with no
  SDK/RTX/Inno needed.
```

- [ ] **Step 4: Verify docs render (no broken anchors)**

Run: `pwsh -NoProfile -Command "Select-String -Path README.md,CLAUDE.md -Pattern 'build-installer|Inno Setup|SmartScreen|Ampere' | Select-Object -First 10"`
Expected: matches in both files; eyeball that the README section sits between Requirements and `## What it is`.

- [ ] **Step 5: Commit**

```bash
git add README.md CLAUDE.md
git commit -m "docs(m5): installer usage (README Install + CLAUDE.md Inno Setup prereq)"
```

---

### Task 4: Human verification gate (RTX 3090 box)

**Files:**
- Create: `docs/superpowers/verification/2026-06-22-m5-installer.md`

**Interfaces:** Consumes the produced `dist\CameraOnScreen-Setup-<ver>-x64.exe`.

This task is **manual** — it is the real functional verification (no automated substitute; the layered-overlay + Maxine effects are an inherent human gate per `docs/superpowers/verification/`).

- [ ] **Step 1: Build the installer**

Run (PowerShell, with the `COS_*` runtime env vars set as for a normal effects build):

```powershell
$env:COS_VFX_RUNTIME_DIR = "<VFX 0.7.6 runtime>"
$env:COS_AR_RUNTIME_DIR  = "<AR 0.8.7 runtime>"
.\scripts\build-installer.ps1 -Version 0.1.0
```

Expected: ends with `installer -> …\dist\CameraOnScreen-Setup-0.1.0-x64.exe` and a size line. Record the size (validate against the ~700 MB compressed target).

- [ ] **Step 2: Install**

Double-click the produced `.exe` (or run it). Confirm: no admin/UAC prompt; installs to `%LOCALAPPDATA%\Programs\CameraOnScreen`; Start Menu shortcut created; SmartScreen click-through is the only friction (unsigned).

- [ ] **Step 3: Run with no env vars**

Open a **fresh** shell (or log into a clean profile) with **all `COS_*` unset**, launch from the Start Menu shortcut. Confirm: app starts; toggles are enabled; **green screen** works; **eye contact** works; **both together** work. (Effects resolve from `<app>\maxine\` with no env vars.)

- [ ] **Step 4: Uninstall check**

Uninstall via Add/Remove Programs. Confirm the app dir (incl. `maxine\`) is removed **and** `%LOCALAPPDATA%\CameraOnScreen\config.json` is **preserved**.

- [ ] **Step 5: Record + commit the verification log**

Write `docs/superpowers/verification/2026-06-22-m5-installer.md` capturing: produced filename + size, install/launch/effects/uninstall results, GPU + driver, any deviations.

```bash
git add docs/superpowers/verification/2026-06-22-m5-installer.md
git commit -m "docs(m5): installer human-verification log (RTX 3090)"
```

---

## Self-Review

**Spec coverage:** Inno Setup per-user unsigned (Task 1 `.iss` + Global Constraints); LZMA2 ~700 MB (Task 1 `Compression=lzma2/ultra64`, size recorded Task 4); orchestrator publish→bundle→compile (Task 2); export-verify deploy-the-right-shim (Task 2 `Assert-ShimHasEffects`); soft RTX preflight (Task 1 `[Code]`); uninstall preserves config (Task 1 — config outside `{app}`; verified Task 4); Pester static tests (Tasks 1–2); Inno Setup prereq (Task 3); human gate (Task 4). The spec's `--self-contained` zero-prereq refinement is captured in Global Constraints + Task 2. Out-of-scope items (#4 release.yml, signing, multi-GPU, #3 license) intentionally absent.

**Placeholder scan:** `<ver>` / `<VFX 0.7.6 runtime>` in Task 4 are real human-supplied values at run time, not plan gaps; every code/iss/test block is complete.

**Type/name consistency:** `CameraOnScreen.iss`, `build-installer.ps1`, `Build-Installer.Tests.ps1`, Describe blocks `installer-iss` / `build-installer`, function names `Resolve-Iscc` / `Assert-ShimHasEffects`, and param names match across Tasks 1–2 and the tests.
