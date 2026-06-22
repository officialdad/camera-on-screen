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
}
