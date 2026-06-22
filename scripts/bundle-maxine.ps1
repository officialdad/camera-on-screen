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
    param([string]$Src, [string]$Dst, [string]$Name, [string]$DestName)
    $s = Join-Path $Src $Name
    if (-not (Test-Path -LiteralPath $s)) { throw "missing required file in '$Src': $Name" }
    $target = if ($DestName) { Join-Path $Dst $DestName } else { $Dst }
    Copy-Item -LiteralPath $s -Destination $target -Force
}
function Copy-Glob {
    param([string]$Src, [string]$Dst, [string]$Glob)
    if (-not (Test-Path -LiteralPath $Src)) { throw "models source dir not found: '$Src'" }
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

foreach ($d in $m.SharedDlls)   { Copy-Required $VfxRuntime $maxine $d }
foreach ($d in $m.ArSharedDlls) { Copy-Required $ArRuntime  $maxine $d }
Copy-Required $VfxRuntime $maxine $m.VfxEffectDll
Copy-Required $ArRuntime  $maxine $m.ArEffectDll
foreach ($g in $m.VfxModelGlobs) { Copy-Glob (Join-Path $VfxRuntime 'models') $mVfx $g }
foreach ($g in $m.ArModelGlobs)  { Copy-Glob (Join-Path $ArRuntime  'models') $mAr  $g }
# License notices are REQUIRED, not best-effort (a shipped bundle must never lack them).
# Both SDKs ship a same-named ThirdPartyLicenses.txt with DIFFERENT content (the VFX vs AR
# OSS load-closures), so each is copied to a distinct name to avoid clobbering. The Maxine
# EULA is identical across SDKs, so one copy (from VFX) covers the bundle.
foreach ($k in $m.VfxLicense.Keys) { Copy-Required $VfxRuntime $maxine $k -DestName $m.VfxLicense[$k] }
foreach ($k in $m.ArLicense.Keys)  { Copy-Required $ArRuntime  $maxine $k -DestName $m.ArLicense[$k] }

$bytes = (Get-ChildItem -LiteralPath $maxine -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("bundled -> {0}" -f $maxine)
Write-Host ("  total : {0:N0} bytes  ({1:N2} GB)" -f $bytes, ($bytes / 1GB))
