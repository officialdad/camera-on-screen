# M5 Bundler Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A publish-time PowerShell step that produces `<output>\maxine\` containing the *minimal verified load-closure* of the green-screen (VFX 0.7.6) and gaze (AR 0.8.7) effects, co-versioned on shared TensorRT 10.4, Ampere models only.

**Architecture:** A C++ trace tool loads both effects and runs a synthetic frame, then enumerates the DLLs the runtime actually loaded from `maxine\`. That list seeds a checked-in manifest. A PowerShell bundler reads the manifest and copies exactly those files (plus curated Ampere models + license notices) from the two SDK runtimes into `<output>\maxine\`. The same trace tool, run against the *produced* pruned bundle with no env vars, is the verify gate.

**Tech Stack:** C++17 (MSVC v143, built via `vcvars64` + `cl`, like the existing smoke tools), PowerShell 7 + Pester, the existing shim sources (`aigs.cpp`, `eyecontact.cpp`, `paths.cpp`) compiled into the tool.

## Global Constraints

- **Co-version invariant (hard):** VFX **0.7.6** + AR **0.8.7**, both **TensorRT 10.4.0.26 / CUDA 12.1**. Every **shared** runtime DLL is copied from the **VFX runtime only** (byte-identical to AR's copy); only `nvARPose.dll` + the AR models come from the AR runtime.
- **No C-ABI change.** `CosStatus`/`CosParams`/`CosCaps` and `PInvokeShim` are untouched. Core tests stay green.
- **Ampere (`_86`) only** this milestone. All model globs are arch-tagged (`*_86*`) so adding `_75/_89/_120` later = appending globs, no layout change (universal-installer-ready).
- **Builds pristine — 0 warnings.** The smoke/trace tools are dev/CI binaries, not the shipped shim; keep them warning-clean.
- **The bundler is an explicit publish-time step, NOT wired into `dotnet build`** (no 2 GB copy per build). No csproj change.
- **License notices ride along:** `NVIDIA Maxine EULA.pdf` + `ThirdPartyLicenses.txt` land in `maxine\`. (Partial compliance; full review is issue #3.)
- **Size target:** `maxine\` ≈ **1.1–1.4 GB** (from the 2.1 GB verbatim copy). Validate, don't assert.
- Spec: `docs/superpowers/specs/2026-06-21-camera-on-screen-m5-bundler-design.md`. Branch: `feat/m5-bundler`.

## File Structure

- `native/shim/smoke/trace_closure.cpp` (new) — loads both effects, runs a synthetic frame, prints loaded modules under `maxine\`; exit 0 iff both effects loaded. Trace tool **and** verify gate.
- `native/shim/smoke/build_trace_closure.bat` (new) — `cl` build, mirrors `build_bundle_probe.bat` + links `Psapi.lib`.
- `native/shim/bundle/maxine-manifest.psd1` (new) — the checked-in allow-list (DLLs from the trace, curated model globs, license files).
- `scripts/bundle-maxine.ps1` (new) — reads the manifest + the two runtimes, copies into `<OutDir>\maxine\`, reports size.
- `scripts/Bundle-Maxine.Tests.ps1` (new) — Pester test of the bundler's copy + missing-file logic against fake source dirs (no SDK, no GPU; fast).
- `docs/superpowers/verification/2026-06-21-m5-bundler.md` (new) — records the end-to-end RTX gate result + measured size.

---

### Task 1: Closure trace tool

**Files:**
- Create: `native/shim/smoke/trace_closure.cpp`
- Create: `native/shim/smoke/build_trace_closure.bat`
- Consumes: `Aigs` (`native/shim/aigs.h`) — `bool Start()`, `bool ProcessFrame(uint8_t*,int,int,double,double)`, `void Stop()`; `EyeContact` (`native/shim/eyecontact.h`) — `bool Start()`, `bool ProcessFrame(uint8_t*,int,int)`, `void Stop()`; `ShimModuleDir()` (`native/shim/paths.h`) — returns the EXE's own dir (UTF-8, no trailing slash) when statically linked into the tool.
- Produces: a binary that (a) prints `maxine\`-relative loaded-module paths to stdout, (b) exits `0` iff both effects' `Start()` returned true. Used as the trace source in Task 2 and the verify gate in Task 4.

- [ ] **Step 1: Write the trace tool**

`native/shim/smoke/trace_closure.cpp`:

```cpp
// Dev/CI tool. Loads BOTH Maxine effects and runs one synthetic frame through each
// (so every DLL + model the runtime touches at load AND run time is pulled in), then
// enumerates every module loaded from <exe>\maxine and prints it relative to that dir.
//
// Two uses:
//   (1) TRACE  — run against a FULL (verbatim) <exe>\maxine to discover the DLL closure;
//                its stdout list seeds maxine-manifest.psd1's SharedDlls.
//   (2) GATE   — run against the PRODUCED (pruned) <exe>\maxine; exit 0 iff BOTH effects
//                loaded, proving the prune didn't drop a required DLL/model.
//
// Build INTO the App output dir so ShimModuleDir()==this exe's dir resolves <exe>\maxine.
// Run with all COS_* env vars UNSET to exercise the app-relative tier.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <windows.h>
#include <psapi.h>
#include "../aigs.h"
#include "../eyecontact.h"
#include "../paths.h"

static std::string ToLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

int main() {
    // 1. Load + run both effects (same thread = CUDA affinity satisfied).
    Aigs gs;
    EyeContact ec;
    const bool gsOk = gs.Start();
    const bool ecOk = ec.Start();
    const int W = 1280, H = 720;
    std::vector<uint8_t> frame((size_t)W * H * 4, (uint8_t)128);
    if (gsOk) gs.ProcessFrame(frame.data(), W, H, 0.0, 0.0);
    if (ecOk) ec.ProcessFrame(frame.data(), W, H);
    std::printf("# green-screen Start=%d  eye-contact Start=%d\n", gsOk ? 1 : 0, ecOk ? 1 : 0);
    if (!gsOk) std::printf("# GS error: %s\n", gs.LastError().c_str());
    if (!ecOk) std::printf("# EC error: %s\n", ec.LastError().c_str());

    // 2. Enumerate modules loaded from <exe>\maxine\ .
    const std::string root = ToLower(ShimModuleDir()) + "\\maxine\\";
    std::vector<HMODULE> mods(2048);
    DWORD needed = 0;
    if (!EnumProcessModulesEx(GetCurrentProcess(), mods.data(),
                              (DWORD)(mods.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL)) {
        std::printf("EnumProcessModulesEx failed: %lu\n", GetLastError());
        return 2;
    }
    const int n = (int)(needed / sizeof(HMODULE));
    std::vector<std::string> hits;
    for (int i = 0; i < n && i < (int)mods.size(); ++i) {
        wchar_t pathW[MAX_PATH];
        if (!GetModuleFileNameW(mods[i], pathW, MAX_PATH)) continue;
        char path[MAX_PATH * 2];
        if (WideCharToMultiByte(CP_UTF8, 0, pathW, -1, path, sizeof(path), nullptr, nullptr) <= 0) continue;
        const std::string p = ToLower(path);
        if (p.rfind(root, 0) == 0) hits.push_back(p.substr(root.size()));
    }
    std::sort(hits.begin(), hits.end());
    std::printf("# %zu modules loaded from maxine\\:\n", hits.size());
    for (const auto& h : hits) std::printf("%s\n", h.c_str());

    // 3. Clean up so worker objects release the CUDA stream before exit.
    gs.Stop();
    ec.Stop();
    return (gsOk && ecOk) ? 0 : 1;
}
```

- [ ] **Step 2: Write the build script**

`native/shim/smoke/build_trace_closure.bat` (mirrors `build_bundle_probe.bat`, adds `Psapi.lib`; `%~1` = output exe path). The `C:\dev\...` paths are the committed scrubbed placeholders — set them to the real VFX/AR **build-source** trees on your machine before running (same as the existing smoke build):

```bat
@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set VFX=C:\dev\VideoFX
set AR=C:\dev\Maxine-AR-SDK
set OUT=%~1
cl /nologo /EHsc /std:c++17 /DCOS_HAS_MAXINE /DCOS_HAS_MAXINE_AR /I "%VFX%\nvvfx\include" /I "%VFX%\features\nvvfxgreenscreen\include" /I "%AR%\nvar\include" native\shim\smoke\trace_closure.cpp native\shim\aigs.cpp native\shim\eyecontact.cpp native\shim\paths.cpp "%VFX%\nvvfx\src\nvVideoEffectsProxy.cpp" "%VFX%\nvvfx\src\nvCVImageProxy.cpp" "%AR%\nvar\src\nvARProxy.cpp" Psapi.lib /Fo"native\shim\smoke\\" /Fe"%OUT%"
exit /b %ERRORLEVEL%
```

- [ ] **Step 3: Build the tool (PowerShell, from repo root)**

Run:
```powershell
$exe = "src\CameraOnScreen.App\bin\Debug\net8.0-windows10.0.19041.0\win-x64\trace_closure.exe"
cmd /c "native\shim\smoke\build_trace_closure.bat $exe"
```
Expected: `cl` compiles with **no warnings**, `trace_closure.exe` written next to the App's `maxine\`. (Requires the App to have been built once so the output dir + a verbatim `maxine\` exist — the M5-part-1 staged bundle is already there.)

- [ ] **Step 4: Run the trace against the FULL bundle (RTX gate)**

Run with all `COS_*` unset, from the App output dir:
```powershell
$out = "src\CameraOnScreen.App\bin\Debug\net8.0-windows10.0.19041.0\win-x64"
Remove-Item Env:COS_VFX_RUNTIME_DIR,Env:COS_VFX_SDK_DIR,Env:COS_AR_RUNTIME_DIR -ErrorAction SilentlyContinue
Push-Location $out; .\trace_closure.exe; $code = $LASTEXITCODE; Pop-Location
"exit=$code"
```
Expected: `# green-screen Start=1  eye-contact Start=1`, then a sorted list of `.dll` paths (incl. `nvinfer_10.dll`, `nvcvimage.dll`, `nvvideoeffects.dll`, `nvarpose.dll`, cuda/cublas/npp entries), `exit=0`. **Save this stdout** — it is the DLL closure for Task 2. Note which big DLLs are **absent** (expect `cufft*`, `nvvpi2.dll` to NOT appear).

- [ ] **Step 5: Commit**

```bash
git add native/shim/smoke/trace_closure.cpp native/shim/smoke/build_trace_closure.bat
git commit -m "feat(m5): closure trace tool (loads both effects, lists loaded maxine DLLs)"
```

---

### Task 2: The bundle manifest

**Files:**
- Create: `native/shim/bundle/maxine-manifest.psd1`
- Consumes: the Task 1 trace stdout (the actual loaded-DLL list).
- Produces: a `.psd1` importable via `Import-PowerShellDataFile` with keys `SharedDlls` (string[]), `VfxEffectDll` (string), `ArEffectDll` (string), `VfxModelGlobs` (string[]), `ArModelGlobs` (string[]), `License` (string[]). Consumed by Task 3.

- [ ] **Step 1: Write the manifest, seeding `SharedDlls` from the Task 1 trace**

Replace the `SharedDlls` list below with the **exact** `.dll` names the trace printed (Task 1 Step 4) — drop any DLL the trace did **not** list. The starter list reflects the VFX 0.7.6 runtime contents; the trace prunes it to the real closure.

`native/shim/bundle/maxine-manifest.psd1`:

```powershell
@{
    # ----------------------------------------------------------------------------------
    # Maxine bundle allow-list. Verified SDK pair: VFX 0.7.6 + AR 0.8.7
    # (TensorRT 10.4.0.26 / CUDA 12.1). Shared runtime DLLs are byte-identical between the
    # two SDKs (the co-version invariant), so they are sourced from the VFX runtime ONLY.
    # SharedDlls is PRODUCED by native/shim/smoke/trace_closure (loaded-module enumeration);
    # re-run it and update this list when the SDK versions bump.
    # ----------------------------------------------------------------------------------

    # Shared CUDA/TensorRT runtime — copied from -VfxRuntime. <<< paste trace output here >>>
    SharedDlls = @(
        'NVCVImage.dll'
        'cudart64_12.dll'
        'cublas64_12.dll'
        'cublasLt64_12.dll'
        'nvinfer_10.dll'
        'nvinfer_plugin_10.dll'
        'nvonnxparser_10.dll'
        'nvrtc64_120_0.dll'
        'nvrtc-builtins64_121.dll'
        'libcrypto-3-x64.dll'
        'nppc64_12.dll'
        'nppial64_12.dll'
        'nppidei64_12.dll'
        'nppig64_12.dll'
        'nppim64_12.dll'
        'nppist64_12.dll'
        # NOTE: cufft64_10/11, cufftw*, nvvpi2 expected ABSENT from the trace -> not listed.
    )

    VfxEffectDll = 'NVVideoEffects.dll'   # green-screen effect DLL — from -VfxRuntime
    ArEffectDll  = 'nvARPose.dll'         # gaze effect DLL        — from -ArRuntime

    # Ampere (_86) models only this milestone. Globs are arch-tagged for forward-compat.
    VfxModelGlobs = @(
        'AIGS_*_86_*.engine.trtpkg'
    )
    ArModelGlobs = @(
        'gazeredir_*_86*.engine.trtpkg'
        'face_detection_86*.engine.trtpkg'
        'faceland_*_86*.engine.trtpkg'
    )

    # License notices that must travel with the redistributable.
    License = @(
        'NVIDIA Maxine EULA.pdf'
        'ThirdPartyLicenses.txt'
    )
}
```

> The `ArModelGlobs` list is the curated gaze dependency set (gaze redirector + face detect + landmarks); bodypose / peoplenet / fullupperbody are excluded. If the Task 4 gate later reveals gaze needs another model, add its `*_86*` glob here.

- [ ] **Step 2: Verify the manifest parses and has every key**

Run:
```powershell
$m = Import-PowerShellDataFile native\shim\bundle\maxine-manifest.psd1
@('SharedDlls','VfxEffectDll','ArEffectDll','VfxModelGlobs','ArModelGlobs','License') |
  ForEach-Object { if ($null -eq $m.$_) { throw "manifest missing key: $_" } }
"keys OK; SharedDlls=$($m.SharedDlls.Count)"
```
Expected: prints `keys OK; SharedDlls=<n>` with no throw.

- [ ] **Step 3: Commit**

```bash
git add native/shim/bundle/maxine-manifest.psd1
git commit -m "feat(m5): bundle manifest (traced DLL allow-list + curated Ampere models)"
```

---

### Task 3: The bundler script (+ Pester test)

**Files:**
- Create: `scripts/bundle-maxine.ps1`
- Create: `scripts/Bundle-Maxine.Tests.ps1`
- Consumes: the manifest from Task 2; two runtime source dirs.
- Produces: `<OutDir>\maxine\` with `SharedDlls` + `VfxEffectDll` + `License` (from `-VfxRuntime`) and `ArEffectDll` (from `-ArRuntime`) in the root, `VfxModelGlobs` in `models\vfx`, `ArModelGlobs` in `models\ar`. Throws on any missing required file or zero-match glob.

- [ ] **Step 1: Write the failing Pester test**

`scripts/Bundle-Maxine.Tests.ps1`:

```powershell
Describe 'bundle-maxine' {
    BeforeAll {
        $script:root = Join-Path ([IO.Path]::GetTempPath()) ("bm_" + [guid]::NewGuid())
        $script:vfx  = Join-Path $root 'vfx'
        $script:ar   = Join-Path $root 'ar'
        $script:out  = Join-Path $root 'out'
        New-Item -ItemType Directory -Force -Path (Join-Path $vfx 'models'),(Join-Path $ar 'models'),$out | Out-Null
        'x' | Set-Content (Join-Path $vfx 'nvinfer_10.dll')
        'x' | Set-Content (Join-Path $vfx 'NVVideoEffects.dll')
        'x' | Set-Content (Join-Path $vfx 'NVIDIA Maxine EULA.pdf')
        'x' | Set-Content (Join-Path $vfx 'ThirdPartyLicenses.txt')
        'x' | Set-Content (Join-Path $ar  'nvARPose.dll')
        'x' | Set-Content (Join-Path $vfx 'models\AIGS_288x512_86_m0.engine.trtpkg')
        'x' | Set-Content (Join-Path $ar  'models\gazeredir_encoder_fp16_86.engine.trtpkg')
        'x' | Set-Content (Join-Path $ar  'models\face_detection_86.engine.trtpkg')
        'x' | Set-Content (Join-Path $ar  'models\faceland_fp16_rcn_mode0_86.engine.trtpkg')
        $script:manifest = Join-Path $root 'm.psd1'
        @'
@{
  SharedDlls = @('nvinfer_10.dll')
  VfxEffectDll = 'NVVideoEffects.dll'
  ArEffectDll = 'nvARPose.dll'
  VfxModelGlobs = @('AIGS_*_86_*.engine.trtpkg')
  ArModelGlobs = @('gazeredir_*_86*.engine.trtpkg','face_detection_86*.engine.trtpkg','faceland_*_86*.engine.trtpkg')
  License = @('NVIDIA Maxine EULA.pdf','ThirdPartyLicenses.txt')
}
'@ | Set-Content -LiteralPath $manifest
        $script:script = Join-Path $PSScriptRoot 'bundle-maxine.ps1'
    }
    AfterAll { Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue }

    It 'produces the maxine layout from the manifest' {
        & $script -OutDir $out -VfxRuntime $vfx -ArRuntime $ar -ManifestPath $manifest
        Test-Path (Join-Path $out 'maxine\nvinfer_10.dll')          | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\NVVideoEffects.dll')      | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\nvARPose.dll')            | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\NVIDIA Maxine EULA.pdf')  | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\models\vfx\AIGS_288x512_86_m0.engine.trtpkg') | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\models\ar\gazeredir_encoder_fp16_86.engine.trtpkg') | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\models\ar\face_detection_86.engine.trtpkg')   | Should -BeTrue
    }
    It 'throws when a required DLL is missing from the source' {
        Remove-Item (Join-Path $vfx 'nvinfer_10.dll')
        { & $script -OutDir $out -VfxRuntime $vfx -ArRuntime $ar -ManifestPath $manifest } | Should -Throw
    }
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run (install Pester once if absent: `Install-Module Pester -Scope CurrentUser -Force`):
```powershell
Invoke-Pester scripts\Bundle-Maxine.Tests.ps1
```
Expected: FAIL — `bundle-maxine.ps1` does not exist yet (`CommandNotFoundException` / cannot find path).

- [ ] **Step 3: Write the bundler**

`scripts/bundle-maxine.ps1`:

```powershell
#Requires -Version 7
<#
.SYNOPSIS
  Copy the minimal co-versioned Maxine runtime into <OutDir>\maxine\ per the manifest.
  Explicit publish-time step (NOT wired into dotnet build). See the M5 bundler spec.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutDir,
    [string]$VfxRuntime = $env:COS_VFX_RUNTIME_DIR,
    [string]$ArRuntime  = $(if ($env:COS_AR_RUNTIME_DIR) { $env:COS_AR_RUNTIME_DIR }
                           else { Join-Path $env:ProgramFiles 'NVIDIA Corporation\NVIDIA AR SDK' }),
    [string]$ManifestPath = (Join-Path $PSScriptRoot '..\native\shim\bundle\maxine-manifest.psd1')
)
$ErrorActionPreference = 'Stop'

function Copy-Required {
    param([string]$Src, [string]$Dst, [string]$Name)
    $s = Join-Path $Src $Name
    if (-not (Test-Path -LiteralPath $s)) { throw "missing required file in '$Src': $Name" }
    Copy-Item -LiteralPath $s -Destination $Dst -Force
}
function Copy-Glob {
    param([string]$Src, [string]$Dst, [string]$Glob)
    $files = @(Get-ChildItem -LiteralPath $Src -Filter $Glob -File -ErrorAction SilentlyContinue)
    if ($files.Count -eq 0) { throw "no files matched '$Glob' in '$Src'" }
    foreach ($f in $files) { Copy-Item -LiteralPath $f.FullName -Destination $Dst -Force }
}

if (-not $VfxRuntime -or -not (Test-Path -LiteralPath $VfxRuntime)) { throw "VFX runtime not found: '$VfxRuntime'" }
if (-not (Test-Path -LiteralPath $ArRuntime)) { throw "AR runtime not found: '$ArRuntime'" }
$m = Import-PowerShellDataFile -LiteralPath $ManifestPath

$maxine = Join-Path $OutDir 'maxine'
$mVfx   = Join-Path $maxine 'models\vfx'
$mAr    = Join-Path $maxine 'models\ar'
New-Item -ItemType Directory -Force -Path $maxine, $mVfx, $mAr | Out-Null

foreach ($d in $m.SharedDlls) { Copy-Required $VfxRuntime $maxine $d }
Copy-Required $VfxRuntime $maxine $m.VfxEffectDll
Copy-Required $ArRuntime  $maxine $m.ArEffectDll
foreach ($g in $m.VfxModelGlobs) { Copy-Glob (Join-Path $VfxRuntime 'models') $mVfx $g }
foreach ($g in $m.ArModelGlobs)  { Copy-Glob (Join-Path $ArRuntime  'models') $mAr  $g }
foreach ($l in $m.License) {
    $s = Join-Path $VfxRuntime $l
    if (Test-Path -LiteralPath $s) { Copy-Item -LiteralPath $s -Destination $maxine -Force }
}

$bytes = (Get-ChildItem -LiteralPath $maxine -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("bundled -> {0}" -f $maxine)
Write-Host ("  total : {0:N0} bytes  ({1:N2} GB)" -f $bytes, ($bytes / 1GB))
```

- [ ] **Step 4: Run the test to verify it passes**

Run:
```powershell
Invoke-Pester scripts\Bundle-Maxine.Tests.ps1
```
Expected: PASS — both `It` blocks green (layout produced; missing-DLL throws).

- [ ] **Step 5: Commit**

```bash
git add scripts/bundle-maxine.ps1 scripts/Bundle-Maxine.Tests.ps1
git commit -m "feat(m5): bundle-maxine.ps1 + Pester test (manifest-driven copy, missing-file guard)"
```

---

### Task 4: End-to-end bundle + verify gate

**Files:**
- Create: `docs/superpowers/verification/2026-06-21-m5-bundler.md`
- Consumes: Task 1 `trace_closure.exe` (as gate), Task 3 `bundle-maxine.ps1`, Task 2 manifest.
- Produces: a real pruned `maxine\` in the App publish output + a recorded RTX verification.

- [ ] **Step 1: Publish the App and run the bundler against the real runtimes**

Run (point `-VfxRuntime`/`-ArRuntime` at your co-versioned **0.7.6 / 0.8.7** runtimes):
```powershell
$pub = "src\CameraOnScreen.App\bin\Debug\net8.0-windows10.0.19041.0\win-x64"
Remove-Item -Recurse -Force (Join-Path $pub 'maxine') -ErrorAction SilentlyContinue
.\scripts\bundle-maxine.ps1 -OutDir $pub `
    -VfxRuntime "C:\dev\VideoFX-0.7.6" `
    -ArRuntime  "$env:ProgramFiles\NVIDIA Corporation\NVIDIA AR SDK"
```
Expected: no throw; prints `total : … (~1.1–1.4 GB)`. If it throws "no files matched", fix the offending glob in the manifest (Task 2) and re-run.

- [ ] **Step 2: Run the verify gate against the PRODUCED pruned bundle**

Run with `COS_*` unset (the trace tool doubles as the gate):
```powershell
Remove-Item Env:COS_VFX_RUNTIME_DIR,Env:COS_VFX_SDK_DIR,Env:COS_AR_RUNTIME_DIR -ErrorAction SilentlyContinue
Push-Location $pub; .\trace_closure.exe; $code = $LASTEXITCODE; Pop-Location
"gate exit=$code"
```
Expected: `# green-screen Start=1  eye-contact Start=1`, `gate exit=0`. **If exit≠0**, the prune dropped a required file — read the `GS error:`/`EC error:` line, add the missing DLL to `SharedDlls` or the missing model glob to the manifest, re-bundle (Step 1), re-gate.

- [ ] **Step 3: Human end-to-end run on the RTX 3090 (definitive gate)**

Launch the published app from `$pub` with `COS_*` unset (double-click or run the exe). Confirm:
- Both capability probes pass; both toggles enable.
- Green screen works; eye contact works; both together.

This is the only check that exercises gaze's runtime face pipeline on real video — it catches a pruned run-time-only model the engine-load gate could miss.

- [ ] **Step 4: Record the verification + measured size**

`docs/superpowers/verification/2026-06-21-m5-bundler.md` — record: produced `maxine\` total size + per-section breakdown (`du`/`Get-ChildItem`), the final `SharedDlls` count, which big DLLs were pruned (`cufft*`, `nvvpi2`, etc.), the gate exit code, and the human-run result (both effects working). Mirror the format of existing files in that dir.

- [ ] **Step 5: Update project docs**

Mark the bundler done and link the deferred issues in `CLAUDE.md` (M5 status + follow-up #7 "Remaining") and the auto-memory `MEMORY.md` pointer: bundler DONE (`scripts/bundle-maxine.ps1` + manifest + trace gate, Ampere-only, ~<measured> GB); remaining M5 = installer (#1), multi-GPU (#2), license review (#3), release.yml (#4).

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/verification/2026-06-21-m5-bundler.md CLAUDE.md
git commit -m "docs(m5): bundler verified on RTX 3090 — maxine\\ pruned to <measured> GB"
```

---

## Self-Review

**Spec coverage:** trace tool → Task 1; manifest (DLL allow-list + curated models + license) → Task 2; PowerShell bundler not wired into build → Task 3; verify gate (probe against produced bundle) + human gate + size record → Task 4; co-version-from-one-source → enforced in Task 3 (`SharedDlls`/`VfxEffectDll` from `-VfxRuntime`, `ArEffectDll` from `-ArRuntime`); Ampere-only + arch-tagged globs → Task 2; license ride-along → manifest `License` + bundler copy. Out-of-scope items (installer, multi-GPU, full license, release.yml) → GH issues #1–#4, not tasks. No gaps.

**Placeholder scan:** `SharedDlls` is intentionally trace-seeded — Task 1 Step 4 produces the real list and Task 2 Step 1 says to paste it; not a placeholder but a documented data-dependency. All code blocks are complete. No TBD/TODO in steps.

**Type/name consistency:** manifest keys `SharedDlls / VfxEffectDll / ArEffectDll / VfxModelGlobs / ArModelGlobs / License` are identical across Task 2 (definition), Task 3 script + test, and the bundler reads. `trace_closure.exe` / `bundle-maxine.ps1` names consistent across Tasks 1, 3, 4. `Aigs`/`EyeContact` method signatures match `aigs.h`/`eyecontact.h`.
