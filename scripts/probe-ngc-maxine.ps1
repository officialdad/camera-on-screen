<#
.SYNOPSIS
  READ-ONLY probe of the NGC nvidia/maxine model registry. Lists every Maxine model, each
  version, and which GPU arches (smNN) it ships Windows engines for — to answer issue #2
  (multi-GPU): does NGC host non-Ampere (sm75/89/100/120) engine packages CO-VERSIONED with
  our shipped runtimes (VFX 0.7.x green screen + AR 0.8.x gaze, TensorRT 10.4)?

  Downloads NOTHING. Only GETs version metadata. Mirrors install_feature.ps1's auth +
  endpoints so the same NGC_CLI_API_KEY works. Run from anywhere; PS 5.1 compatible.

.NOTES
  Key resolution (same as install_feature.ps1): $env:NGC_CLI_API_KEY, else ~/.ngc/config.
  Personal keys (nvapi-...) are used as-is; org keys are exchanged for a bearer token.
#>
[CmdletBinding()]
param(
    [string]$NgcOrg  = 'nvidia',
    [string]$NgcTeam = 'maxine',
    # Optional name-substring filter. Empty (default) = probe every curated candidate.
    [string[]]$Focus = @()
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
    if ($k -match '^nvapi-') { return $k }   # personal key: use directly
    # org key -> bearer token
    $auth = '$oauthtoken:' + $k
    $enc  = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes($auth))
    $url  = "https://authn.nvidia.com/token?service=ngc&scope=group/ngc:$NgcOrg&group/ngc:$NgcOrg/$NgcTeam"
    $r = Invoke-RestMethod -Uri $url -Headers @{ Accept = 'application/json'; Authorization = "Basic $enc" } -Method Get
    if (-not $r.token) { throw 'NGC auth failed (no token).' }
    return $r.token
}

$key = Get-NgcApiKey
$H = @{ Authorization = "Bearer $key"; 'Content-Type' = 'application/json' }
$base = "https://api.ngc.nvidia.com/v2/org/$NgcOrg/team/$NgcTeam"

# Personal keys often lack team-LIST scope (HTTP 403) but CAN read specific models/versions
# (exactly what install_feature.ps1 does). So: try collection discovery (best-effort), then
# UNION with a curated candidate list, then query each model's /versions directly.
$names = New-Object System.Collections.Generic.HashSet[string]

# (a) best-effort discovery via collections (same endpoint install_feature uses)
foreach ($col in @('maxine_vfx_sdk', 'vfx_sdk', 'maxine_ar_sdk', 'ar_sdk', 'maxine')) {
    try {
        $u = "$base/collections/$col/artifacts/models"
        $r = Invoke-RestMethod -Uri $u -Headers $H -Method Get -ErrorAction Stop
        foreach ($a in $r.artifacts) { [void]$names.Add($a.name.ToLower()) }
        Write-Host "  collection '$col': +$($r.artifacts.Count) models" -ForegroundColor DarkGray
    } catch { Write-Host "  collection '$col': n/a" -ForegroundColor DarkGray }
}

# (b) curated candidates — VFX (known) + AR gaze/face guesses (NGC naming unknown, probe many)
$candidates = @(
    'nvvfxgreenscreen','nvvfxvideosuperres','nvvfxdenoising','nvvfxupscale',
    'nvvfxbackgroundblur','nvvfxrelighting','nvvfxaigsrelighting','nvvfxartifactreduction',
    # AR gaze / eye-contact candidates
    'nvargazeredirection','nvargazeredirect','nvar_gaze','nvargaze','gazeredirection',
    'nvareyecontact','eyecontact','nvarfacedetection','nvarfacelandmark','nvarfacelandmarks',
    'nvarfaceexpression','nvarfaceexpressions','nvarface3d','nvarbodypose','nvarsdk',
    'maxine_ar_gaze','maxine_eye_contact'
)
foreach ($c in $candidates) { [void]$names.Add($c.ToLower()) }
$models = $names | ForEach-Object { [pscustomobject]@{ name = $_ } }
Write-Host ("  probing {0} candidate model names" -f $models.Count) -ForegroundColor Green

# 2. For focused models, pull versions and parse versionId -> (baseVersion, platform, arch).
function Parse-VersionId {
    param([string]$id)
    # e.g. 0.7.6.0_models_windows_sm75 | 1.2.0.0_lib_windows | <v>_models_linux_sm86
    if ($id -match '^(\d+\.\d+\.\d+\.\d+)_(.+)$') {
        $v = $matches[1]; $rest = $matches[2]
        $arch = ''
        if ($rest -match 'sm(\d+)') { $arch = $matches[1] }
        $kind = if ($rest -match 'lib') { 'lib' } elseif ($rest -match 'models') { 'models' } else { 'other' }
        $os   = if ($rest -match 'windows') { 'win' } elseif ($rest -match 'linux') { 'linux' } else { '?' }
        return [pscustomobject]@{ v = $v; kind = $kind; os = $os; arch = $arch; raw = $id }
    }
    return [pscustomobject]@{ v = $id; kind = 'other'; os = '?'; arch = ''; raw = $id }
}

$focused = if ($Focus -and ($Focus -join '') -ne '') {
    $models | Where-Object { $n = $_.name.ToLower(); ($Focus | Where-Object { $n -like "*$_*" }).Count -gt 0 }
} else { $models }

Write-Host ("`nFocused models ({0}):" -f $focused.Count) -ForegroundColor Cyan
foreach ($m in $focused) { Write-Host "  - $($m.name)" }

Write-Host "`n==== VERSION x ARCH MATRIX (Windows model packages) ====" -ForegroundColor Cyan
foreach ($m in ($focused | Sort-Object name)) {
    $name = $m.name
    try {
        $vr = Invoke-RestMethod -Uri "$base/models/$name/versions" -Headers $H -Method Get
    } catch {
        Write-Host "`n[$name] versions query FAILED: $_" -ForegroundColor Red
        continue
    }
    $parsed = @($vr.modelVersions | ForEach-Object { Parse-VersionId $_.versionId })
    $winModels = $parsed | Where-Object { $_.os -eq 'win' -and $_.kind -eq 'models' }
    Write-Host "`n[$name]" -ForegroundColor Yellow
    if (-not $winModels) {
        $libs = ($parsed | Where-Object { $_.kind -eq 'lib' } | Select-Object -Expand v -Unique) -join ', '
        Write-Host "  (no windows model packages; lib versions: $libs)" -ForegroundColor DarkGray
        continue
    }
    $byVer = $winModels | Group-Object v | Sort-Object { [version]$_.Name }
    foreach ($g in $byVer) {
        $arches = ($g.Group | Select-Object -Expand arch -Unique | Where-Object { $_ } | Sort-Object) -join ', '
        $flag = if ($g.Name -like '0.7.*' -or $g.Name -like '0.8.*') { '  <-- co-version candidate' } else { '' }
        Write-Host ("  {0,-12} arches: {1}{2}" -f $g.Name, $arches, $flag)
    }
}

Write-Host "`nDone. Co-version target: green screen 0.7.x + gaze/AR 0.8.x must offer sm75/89/100 to unblock option A." -ForegroundColor Cyan
