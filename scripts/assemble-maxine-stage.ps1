<#
.SYNOPSIS
  Assemble a co-versioned Maxine runtime "stage" from the VFX 1.2.0.0 + AR 1.1.1.0 SDK trees +
  NGC-fetched multi-arch engines + the 2025 license set. The output stage feeds bundle-maxine.ps1
  (-MaxineStage), which prunes it to the verified load closure.

  Co-version is enforced HERE, physically: shared CUDA/TensorRT DLLs are taken from the VFX SDK
  bin (the AR SDK's copies are byte-identical in body -- same TRT 10.9 build -- but VFX is the
  single source so one runtime wins). Only the AR-unique DLLs (dispatcher + feature libs) come
  from the AR side. Validity is proven downstream by bundle_probe / trace_closure.

  Both SDKs use a dispatcher + per-feature-DLL model: NVVideoEffects/nvARPose are dispatchers;
  nvVFXGreenScreen + the three nvAR* feature DLLs are the real effects. nvARFaceExpressions is
  intentionally excluded (not in the gaze closure; emotion recognition disallowed by the AI
  Product-Specific Terms s8.17).

.EXAMPLE
  ./assemble-maxine-stage.ps1 -OutStage C:\maxine-stage `
    -VfxSdk C:\dev\VideoFX -ArSdk C:\dev\Maxine-AR-SDK-1.1.1.0 -ArFeatureLibs C:\dev\ar-feature-libs
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutStage,
    [string]$VfxSdk  = $env:COS_VFX_SDK_DIR,
    [string]$ArSdk   = $env:COS_AR_SDK_DIR,
    # Root holding the four per-feature lib packages, each as <root>\<name>\<name>\{bin,license}.
    [Parameter(Mandatory)][string]$ArFeatureLibs,
    # Optional: Optical Flow SDK root (COS_FRUC_SDK_DIR). Provides NvOFFRUC.dll + cudart64_110.dll
    # for frame-rate upscaling (issue #13). If unset or path missing, a warning is emitted and the
    # FRUC DLLs are skipped -- the stage will lack them and bundle-maxine.ps1 will fail if they are
    # listed in the manifest. Required on the release host; optional for VFX/AR-only dev staging.
    [string]$FrucSdk = $env:COS_FRUC_SDK_DIR,
    [int[]]$Arches = @(75, 86, 89, 100),
    [switch]$SkipEngineFetch,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'

foreach ($pair in @(@{n = 'VfxSdk'; v = $VfxSdk }, @{n = 'ArSdk'; v = $ArSdk }, @{n = 'ArFeatureLibs'; v = $ArFeatureLibs })) {
    if (-not $pair.v -or -not (Test-Path -LiteralPath $pair.v)) { throw "$($pair.n) not found: '$($pair.v)'" }
}

# AR feature: <name> -> shipped DLL filename. Gaze + its two deps only (no FaceExpressions).
$arFeatures = @(
    @{ name = 'nvargazeredirection';   dll = 'nvARGazeRedirection.dll' }
    @{ name = 'nvarfaceboxdetection';  dll = 'nvARFaceBoxDetection.dll' }
    @{ name = 'nvarlandmarkdetection'; dll = 'nvARLandmarkDetection.dll' }
)
function Feature-Dir([string]$name) { Join-Path $ArFeatureLibs (Join-Path $name $name) }
$gazeLicenseDir = Join-Path (Feature-Dir 'nvargazeredirection') 'license'

# License set (the six required redistribution notices). Source -> dest filename in the stage root.
$licenses = @(
    @{ src = Join-Path $VfxSdk 'license\NVIDIA-Software-License-Agreement-2025.05.05.pdf';        dst = 'NVIDIA-Software-License-Agreement-2025.05.05.pdf' }
    @{ src = Join-Path $VfxSdk 'license\product-specific-terms-for-nvidia-ai-products-2025.05.05.pdf'; dst = 'product-specific-terms-for-nvidia-ai-products-2025.05.05.pdf' }
    @{ src = Join-Path $VfxSdk 'license\NVIDIA-Open-Model-License-Agreements-24-10-2025.pdf';     dst = 'NVIDIA-Open-Model-License-Agreements-24-10-2025.pdf' }
    @{ src = Join-Path $gazeLicenseDir 'NVIDIA-Models-Community-License-2025-04-15.pdf';          dst = 'NVIDIA-Models-Community-License-2025-04-15.pdf' }
    @{ src = Join-Path $VfxSdk 'bin\ThirdPartyLicenses.txt';                                       dst = 'ThirdPartyLicenses-VFX.txt' }
    @{ src = Join-Path $ArSdk  'bin\ThirdPartyLicenses.txt';                                       dst = 'ThirdPartyLicenses-AR.txt' }
)

if ($DryRun) {
    Write-Host "DRY RUN — assemble stage at $OutStage"
    Write-Host "  VFX bin DLLs (shared + NVVideoEffects dispatcher) <- $VfxSdk\bin"
    Write-Host "  nvVFXGreenScreen.dll <- $VfxSdk\features\nvvfxgreenscreen\bin"
    Write-Host "  nvVFXVideoSuperRes.dll + nvngx_vsr.dll <- $VfxSdk\features\nvvfxvideosuperres\bin"
    Write-Host "  nvARPose.dll <- $ArSdk\bin"
    foreach ($f in $arFeatures) { Write-Host "  $($f.dll) <- $(Feature-Dir $f.name)\bin" }
    foreach ($l in $licenses) { Write-Host "  license: $($l.dst) <- $($l.src)" }
    $frucBin = if ($FrucSdk) { "$FrucSdk\NvOFFRUC\NvOFFRUCSample\bin\win64" } else { '(COS_FRUC_SDK_DIR unset -- FRUC DLLs will be skipped)' }
    Write-Host "  NvOFFRUC.dll + cudart64_110.dll <- $frucBin"
    Write-Host "  engines: fetch arches $($Arches -join ',') -> models\{vfx,ar}  (SkipEngineFetch=$SkipEngineFetch)"
    return
}

$modelsVfx = Join-Path $OutStage 'models\vfx'
$modelsAr = Join-Path $OutStage 'models\ar'
New-Item -ItemType Directory -Force -Path $OutStage, $modelsVfx, $modelsAr | Out-Null

function Copy-One([string]$src, [string]$dst) {
    if (-not (Test-Path -LiteralPath $src)) { throw "missing source: $src" }
    Copy-Item -LiteralPath $src -Destination $dst -Force
}

# 1. Shared CUDA/TensorRT + NVVideoEffects dispatcher: ALL of VFX bin (the bundler prunes later).
Get-ChildItem (Join-Path $VfxSdk 'bin') -Filter *.dll -File | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $OutStage -Force }
# 2. VFX green-screen feature DLL.
Copy-One (Join-Path $VfxSdk 'features\nvvfxgreenscreen\bin\nvVFXGreenScreen.dll') $OutStage
# 2b. VFX video-super-resolution feature DLL + its NGX runtime. VSR is NGX (model baked into
# nvngx_vsr.dll) -- no per-arch engine to fetch, runs on every RTX arch.
Copy-One (Join-Path $VfxSdk 'features\nvvfxvideosuperres\bin\nvVFXVideoSuperRes.dll') $OutStage
Copy-One (Join-Path $VfxSdk 'features\nvvfxvideosuperres\bin\nvngx_vsr.dll') $OutStage
# 3. AR dispatcher + the three gaze feature DLLs (AR-unique only -- shared DLLs already from VFX).
Copy-One (Join-Path $ArSdk 'bin\nvARPose.dll') $OutStage
foreach ($f in $arFeatures) { Copy-One (Join-Path (Feature-Dir $f.name) (Join-Path 'bin' $f.dll)) $OutStage }
# 3b. FRUC frame-rate upscaler DLLs (issue #13). Optional: warn + skip if COS_FRUC_SDK_DIR unset.
#     NvOFFRUC.dll uses CUDA 11 (cudart64_110.dll), distinct from cudart64_12.dll -- both coexist.
#     LOAD_WITH_ALTERED_SEARCH_PATH in fruc.cpp resolves cudart64_110.dll beside NvOFFRUC.dll.
if (-not $FrucSdk) {
    Write-Warning "COS_FRUC_SDK_DIR not set -- skipping FRUC DLLs (NvOFFRUC.dll, cudart64_110.dll). bundle-maxine.ps1 will fail if they are listed in the manifest."
} elseif (-not (Test-Path -LiteralPath $FrucSdk)) {
    Write-Warning "COS_FRUC_SDK_DIR path not found: '$FrucSdk' -- skipping FRUC DLLs. bundle-maxine.ps1 will fail if they are listed in the manifest."
} else {
    $frucBin = Join-Path $FrucSdk 'NvOFFRUC\NvOFFRUCSample\bin\win64'
    Copy-One (Join-Path $frucBin 'NvOFFRUC.dll')      $OutStage
    Copy-One (Join-Path $frucBin 'cudart64_110.dll')   $OutStage
}
# 4. License notices.
foreach ($l in $licenses) { Copy-One $l.src (Join-Path $OutStage $l.dst) }
# 5. Multi-arch engines (NGC). Skip if already staged (e.g. air-gapped runner pre-seeded).
if (-not $SkipEngineFetch) {
    & (Join-Path $PSScriptRoot 'fetch-maxine-engines.ps1') -VfxModelsOut $modelsVfx -ArModelsOut $modelsAr -Arches $Arches
    if ($LASTEXITCODE) { throw "engine fetch failed ($LASTEXITCODE)" }
}

$dllN = (Get-ChildItem $OutStage -Filter *.dll -File).Count
$engN = (Get-ChildItem $modelsVfx, $modelsAr -Filter *.engine.trtpkg -File).Count
Write-Host ("staged -> {0}" -f $OutStage)
Write-Host ("  dlls: {0}  engines: {1}  licenses: {2}" -f $dllN, $engN, $licenses.Count)
