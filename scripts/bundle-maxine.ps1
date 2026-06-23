<#
.SYNOPSIS
  Prune a pre-assembled, co-versioned Maxine runtime "stage" into <OutDir>\maxine\ per the
  manifest: the allow-listed DLLs + model globs + required license files. Explicit publish-time
  step (NOT wired into dotnet build). See the M5 bundler spec + the issue #2 multi-GPU migration
  findings (VFX 1.2.0.0 + AR 1.1.1.0 / TRT 10.9).

  -MaxineStage is the manual co-version curation (shared DLLs sourced from VFX, AR-only from AR,
  both SDKs' dispatcher + per-feature DLLs, NGC-fetched multi-arch engines under models\{vfx,ar},
  and the license files at its root). Its validity is proven by bundle_probe / trace_closure --
  the bundler only prunes it to the verified load closure; it does not enforce co-version itself.

.EXAMPLE
  ./bundle-maxine.ps1 -OutDir dist\stage -MaxineStage C:\maxine-stage
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutDir,
    [Parameter(Mandatory)][string]$MaxineStage,
    [string]$ManifestPath = (Join-Path $PSScriptRoot '..\native\shim\bundle\maxine-manifest.psd1')
)
$ErrorActionPreference = 'Stop'

function Copy-Required {
    param([string]$Src, [string]$Dst, [string]$Name, [string]$DestName)
    $s = Join-Path $Src $Name
    if (-not (Test-Path -LiteralPath $s)) { throw "missing required file in stage '$Src': $Name" }
    $target = if ($DestName) { Join-Path $Dst $DestName } else { Join-Path $Dst $Name }
    Copy-Item -LiteralPath $s -Destination $target -Force
}
function Copy-Glob {
    param([string]$Src, [string]$Dst, [string]$Glob)
    if (-not (Test-Path -LiteralPath $Src)) { throw "models source dir not found: '$Src'" }
    $files = @(Get-ChildItem -LiteralPath $Src -Filter $Glob -File -ErrorAction SilentlyContinue)
    if ($files.Count -eq 0) { throw "no files matched '$Glob' in '$Src'" }
    foreach ($f in $files) { Copy-Item -LiteralPath $f.FullName -Destination $Dst -Force }
}

if (-not (Test-Path -LiteralPath $MaxineStage)) { throw "stage not found: '$MaxineStage'" }
$m = Import-PowerShellDataFile -LiteralPath $ManifestPath

$maxine = Join-Path $OutDir 'maxine'
$mVfx = Join-Path $maxine 'models\vfx'
$mAr = Join-Path $maxine 'models\ar'
New-Item -ItemType Directory -Force -Path $maxine, $mVfx, $mAr | Out-Null

foreach ($d in $m.Dlls) { Copy-Required $MaxineStage $maxine $d }
foreach ($g in $m.VfxModelGlobs) { Copy-Glob (Join-Path $MaxineStage 'models\vfx') $mVfx $g }
foreach ($g in $m.ArModelGlobs) { Copy-Glob (Join-Path $MaxineStage 'models\ar')  $mAr $g }
# License notices are REQUIRED, not best-effort -- a shipped bundle must never lack them.
foreach ($l in $m.LicenseFiles) { Copy-Required $MaxineStage $maxine $l }

$bytes = (Get-ChildItem -LiteralPath $maxine -Recurse -File | Measure-Object -Property Length -Sum).Sum
$arches = (Get-ChildItem -LiteralPath $mVfx, $mAr -Filter *.engine.trtpkg |
    ForEach-Object { if ($_.Name -match '_(75|86|89|100)(_|\.)') { $matches[1] } } | Sort-Object -Unique) -join ', '
Write-Host ("bundled -> {0}" -f $maxine)
Write-Host ("  total  : {0:N0} bytes  ({1:N2} GB)" -f $bytes, ($bytes / 1GB))
Write-Host ("  arches : {0}" -f $arches)
