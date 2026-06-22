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
    $msvcRoot = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC"
    $dumpbin = if (Test-Path $msvcRoot) {
        $ver = Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($ver) { Join-Path $ver.FullName 'bin\Hostx64\x64\dumpbin.exe' } else { '' }
    } else { '' }
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
if (Test-Path -LiteralPath $StagingDir) {
    $existing = @(Get-ChildItem -LiteralPath $StagingDir -Force)
    if ($existing.Count -gt 0 -and -not (Test-Path -LiteralPath (Join-Path $StagingDir 'CameraOnScreen.App.exe'))) {
        throw "refusing to delete non-empty -StagingDir '$StagingDir': it does not look like a prior build stage (no CameraOnScreen.App.exe). Use an empty or dedicated directory."
    }
    Remove-Item -Recurse -Force -LiteralPath $StagingDir
}
New-Item -ItemType Directory -Force -Path $StagingDir | Out-Null
dotnet publish $appProj -c $Configuration -r win-x64 --self-contained true -o $StagingDir --nologo
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed ($LASTEXITCODE)" }

# 3. Export-verify the deployed shim BEFORE packaging.
Assert-ShimHasEffects -Dll (Join-Path $StagingDir 'CameraOnScreen.Shim.dll')

# 4. Bundle the Maxine runtime into <staging>\maxine\.
& $bundler -OutDir $StagingDir -VfxRuntime $VfxRuntime -ArRuntime $ArRuntime
if (-not (Test-Path -LiteralPath (Join-Path $StagingDir 'maxine'))) { throw "bundler did not produce maxine\ in $StagingDir" }

# 5. Compile the installer.
$stagedExe = Join-Path $StagingDir 'CameraOnScreen.App.exe'
if (-not (Test-Path -LiteralPath $stagedExe)) { throw "staging is missing CameraOnScreen.App.exe — publish incomplete; refusing to package" }
New-Item -ItemType Directory -Force -Path (Join-Path $repo 'dist') | Out-Null
& $isccExe $iss "/DSourceDir=$StagingDir" "/DAppVersion=$Version"
if ($LASTEXITCODE -ne 0) { throw "ISCC compile failed ($LASTEXITCODE)" }

$size = (Get-Item -LiteralPath $output).Length
Write-Host ("installer -> {0}" -f $output)
Write-Host ("  size : {0:N0} bytes  ({1:N2} GB)" -f $size, ($size / 1GB))
