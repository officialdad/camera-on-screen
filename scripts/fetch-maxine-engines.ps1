<#
.SYNOPSIS
  Fetch the multi-arch Maxine TensorRT engine packages (issue #2 universal installer) from the
  NGC nvidia/maxine model registry into a staged models dir, sha256-verified. The bundler then
  copies these into maxine\models\{vfx,ar} via the manifest's arch globs.

  Engines only; NOT the runtime DLLs or SDKs. Target version line = the migrated co-versioned
  pair: green screen nvvfxgreenscreen 1.2.0.0, gaze nvargazeredirection / nvarfaceboxdetection /
  nvarlandmarkdetection 1.1.1.0 (TensorRT 10.9). faceexpressions is NOT fetched (not in the gaze
  load closure per trace_closure). Arch 100 == Blackwell/sm120 on Windows.

  Auth mirrors install_feature.ps1 / probe-ngc-maxine.ps1: $env:NGC_CLI_API_KEY or ~/.ngc/config.
  Personal nvapi- keys are used as-is for the metadata GET; the presigned download needs no auth.
  PS 5.1 compatible.

.EXAMPLE
  ./fetch-maxine-engines.ps1 -VfxModelsOut out\vfx -ArModelsOut out\ar
  ./fetch-maxine-engines.ps1 -VfxModelsOut out\vfx -ArModelsOut out\ar -Arches 86 -DryRun
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$VfxModelsOut,
    [Parameter(Mandatory)][string]$ArModelsOut,
    [int[]]$Arches = @(75, 86, 89, 100),
    [string]$NgcOrg = 'nvidia',
    [string]$NgcTeam = 'maxine',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'

function Get-NgcApiKey {
    $k = $env:NGC_CLI_API_KEY
    if (-not $k) {
        $cfg = Join-Path $env:USERPROFILE '.ngc\config'
        if (Test-Path $cfg) {
            $c = Get-Content $cfg -Raw
            if ($c -match 'apikey\s*=\s*([^\s]+)') { $k = $matches[1] }
        }
    }
    if (-not $k) { throw 'NGC API key not found. Set $env:NGC_CLI_API_KEY or ~/.ngc/config first.' }
    if ($k -match '^nvapi-') { return $k }
    $auth = '$oauthtoken:' + $k
    $enc = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes($auth))
    $url = "https://authn.nvidia.com/token?service=ngc&scope=group/ngc:$NgcOrg&group/ngc:$NgcOrg/$NgcTeam"
    $r = Invoke-RestMethod -Uri $url -Headers @{ Accept = 'application/json'; Authorization = "Basic $enc" } -Method Get
    if (-not $r.token) { throw 'NGC auth failed (no token).' }
    return $r.token
}

# Model -> (version line, destination). VFX green screen -> vfx; AR gaze + its two feature deps -> ar.
$models = @(
    @{ name = 'nvvfxgreenscreen';     ver = '1.2.0.0'; dst = $VfxModelsOut }
    @{ name = 'nvargazeredirection';  ver = '1.1.1.0'; dst = $ArModelsOut }
    @{ name = 'nvarfaceboxdetection'; ver = '1.1.1.0'; dst = $ArModelsOut }
    @{ name = 'nvarlandmarkdetection';ver = '1.1.1.0'; dst = $ArModelsOut }
)

$key = Get-NgcApiKey
$hdr = @{ Authorization = "Bearer $key"; 'Content-Type' = 'application/json' }
$base = "https://api.ngc.nvidia.com/v2/org/$NgcOrg/team/$NgcTeam"
$sha = [Security.Cryptography.SHA256]::Create()
# NB: PowerShell variables are case-insensitive -- never name a local $h next to $hdr/$H.
function Get-Sha256B64([string]$p) { [Convert]::ToBase64String($sha.ComputeHash([IO.File]::ReadAllBytes($p))) }

if (-not $DryRun) { New-Item -ItemType Directory -Force -Path $VfxModelsOut, $ArModelsOut | Out-Null }
$ProgressPreference = 'SilentlyContinue'
$downloaded = 0; $skipped = 0; $failed = 0; $bytes = 0

foreach ($m in $models) {
    foreach ($arch in $Arches) {
        $vid = "$($m.ver)_models_windows_sm$arch"
        try {
            $r = Invoke-RestMethod -Uri "$base/models/$($m.name)/$vid/files" -Headers $hdr -Method Get -ErrorAction Stop
        } catch {
            Write-Host "[$($m.name) sm$arch] versions/files query FAILED: $_" -ForegroundColor Red
            $failed++; continue
        }
        for ($i = 0; $i -lt $r.filepath.Count; $i++) {
            $fn = $r.filepath[$i]; $url = $r.urls[$i]; $want = $r.sha256_base64[$i]
            if ($DryRun) { Write-Host "  would fetch $($m.name) sm$arch -> $fn"; continue }
            $out = Join-Path $m.dst $fn
            if ((Test-Path $out) -and $want -and (Get-Sha256B64 $out) -eq $want) { $skipped++; continue }
            try { Invoke-WebRequest -Uri $url -OutFile $out -ErrorAction Stop }
            catch { Write-Host "  DOWNLOAD FAILED $fn : $_" -ForegroundColor Red; $failed++; continue }
            if ($want -and (Get-Sha256B64 $out) -ne $want) {
                Write-Host "  SHA256 MISMATCH (corrupt download): $fn" -ForegroundColor Red
                Remove-Item $out -Force; $failed++; continue
            }
            $bytes += (Get-Item $out).Length; $downloaded++
        }
    }
}

if ($DryRun) { Write-Host "`nDry run only; nothing downloaded." -ForegroundColor Cyan; return }
Write-Host ("`nfetched={0} skipped(already-verified)={1} failed={2}  new={3:N1} MB" -f $downloaded, $skipped, $failed, ($bytes / 1MB)) -ForegroundColor Cyan
if ($failed -gt 0) { throw "$failed engine file(s) failed to fetch/verify." }
