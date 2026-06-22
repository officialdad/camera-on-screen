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
    It 'names the output CameraOnScreen-Setup-<ver>-x64' { $script:text | Should -Match 'OutputBaseFilename=CameraOnScreen-Setup-.*-x64' }
    It 'recursively copies the staged SourceDir' {
        $script:text | Should -Match '\{#SourceDir\}'
        $script:text | Should -Match 'recursesubdirs'
    }
    It 'creates a Start Menu shortcut to the app exe' { $script:text | Should -Match 'CameraOnScreen\.App\.exe' }
    It 'has a soft NVIDIA preflight in [Code]' {
        $script:text | Should -Match '\[Code\]'
        $script:text | Should -Match 'NVIDIA'
    }
    It 'preflight warns but never aborts the install (info-only dialog, no Yes/No)' {
        # The only abort vector is a cancellable prompt; a warn-only MB_OK/mbInformation
        # dialog cannot abort. (HasNvidiaGpu legitimately uses Result:=False as its
        # "no NVIDIA" return, so a whole-file Result:=False grep would be wrong here.)
        $script:text | Should -Not -Match 'MB_YESNO'
        $script:text | Should -Not -Match 'MB_OKCANCEL'
        $script:text | Should -Match 'mbInformation'
        $script:text | Should -Match 'MB_OK'
    }
    It 'presents a combined end-user license, not MIT-only (NVIDIA Maxine flow-down, §1.2(v))' {
        # The license page the user accepts must cover the bundled NVIDIA Maxine components,
        # whose redistribution terms require end-user terms consistent with NVIDIA's license.
        $script:text | Should -Match 'LicenseFile=COMBINED-LICENSE\.txt'
        $script:text | Should -Not -Match 'LicenseFile=\.\.\\LICENSE\b'
        $combined = Join-Path $PSScriptRoot '..\installer\COMBINED-LICENSE.txt'
        Test-Path $combined | Should -BeTrue
        $ctext = Get-Content -LiteralPath $combined -Raw
        $ctext | Should -Match 'MIT'
        $ctext | Should -Match 'NVIDIA Maxine'
    }
    It 'ships the third-party notices into the install directory' {
        $script:text | Should -Match 'THIRD-PARTY-NOTICES\.md'
    }
}

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
    It 'dry-run prints the build/bundle/compile plan stamped with the version' {
        $dummy = Join-Path ([IO.Path]::GetTempPath()) ("iscc_" + [guid]::NewGuid() + ".exe")
        'x' | Set-Content -LiteralPath $dummy
        try {
            $stage = Join-Path ([IO.Path]::GetTempPath()) ("stg_" + [guid]::NewGuid())
            $out = & $script:s -Version '9.9.9' -IsccPath $dummy -StagingDir $stage -DryRun 6>&1 | Out-String
            $out | Should -Match 'dotnet build'
            $out | Should -Match 'SelfContained'
            $out | Should -Match 'bundle-maxine\.ps1'
            $out | Should -Match 'ISCC'
            $out | Should -Match '9\.9\.9'
        }
        finally { Remove-Item -LiteralPath $dummy -Force -ErrorAction SilentlyContinue }
    }
}
