<#
.SYNOPSIS
  Runtime verify gate for a produced Maxine bundle: compile the headless bundle_probe into the
  staging dir (beside maxine\) and run it with the dev COS_*_RUNTIME_DIR overrides cleared, so it
  exercises the app-relative <exeDir>\maxine tier. Exit 0 = both effects load from the bundle
  (proves models present + TRT/CUDA co-version holds). Consumed by release.yml; see the M5
  bundler spec + docs/superpowers/verification/2026-06-21-m5-bundler.md.
#>
[CmdletBinding()]
param(
    [string]$StagingDir,
    [string]$VfxSdkDir = $env:COS_VFX_SDK_DIR,
    [string]$ArSdkDir  = $env:COS_AR_SDK_DIR,
    [string]$ProbeBat,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $StagingDir) { $StagingDir = Join-Path $repo 'dist\stage' }
if (-not $ProbeBat)   { $ProbeBat   = Join-Path $repo 'native\shim\smoke\build_bundle_probe.bat' }
$maxine   = Join-Path $StagingDir 'maxine'
$probeExe = Join-Path $StagingDir 'bundle_probe.exe'

if ($DryRun) {
    Write-Host "DRY RUN — bundle verify plan:"
    Write-Host "  1. compile $ProbeBat -> $probeExe   (VfxSdkDir=$VfxSdkDir ArSdkDir=$ArSdkDir)"
    Write-Host "  2. run $probeExe from $StagingDir with COS_*_RUNTIME_DIR unset (app-relative maxine\ tier)"
    Write-Host "  3. require exit 0 = both Maxine effects load from the bundled maxine\ ($maxine)"
    return
}

if (-not (Test-Path -LiteralPath $maxine)) {
    throw "no maxine\ in staging '$StagingDir' — run the bundler/installer first; nothing to verify"
}

# 1. Build the probe INTO the staging dir so ShimModuleDir() (the probe exe's own dir) resolves
#    <staging>\maxine. Forward explicit SDK-source dirs to the (env-aware) batch via the env.
if ($VfxSdkDir) { $env:COS_VFX_SDK_DIR = $VfxSdkDir }
if ($ArSdkDir)  { $env:COS_AR_SDK_DIR  = $ArSdkDir }
& cmd /c "`"$ProbeBat`" `"$probeExe`""
if ($LASTEXITCODE -ne 0) { throw "bundle_probe build failed ($LASTEXITCODE)" }
if (-not (Test-Path -LiteralPath $probeExe)) { throw "probe exe not produced: $probeExe" }

# 2. Run with the dev runtime overrides cleared, exercising the bundled app-relative tier.
$env:COS_VFX_RUNTIME_DIR = $null
$env:COS_AR_RUNTIME_DIR  = $null
Push-Location $StagingDir
try { & $probeExe; $rc = $LASTEXITCODE } finally { Pop-Location }
if ($rc -ne 0) {
    throw "bundle verify FAILED ($rc): effects did not load from the bundled maxine\ with COS_* unset"
}
Write-Host "bundle verify OK — both Maxine effects load from $maxine"
