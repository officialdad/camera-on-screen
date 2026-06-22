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
    It 'names the output CameraOnScreen-Setup-<ver>-x64' { $script:text | Should -Match 'OutputBaseFilename=CameraOnScreen-Setup-' }
    It 'recursively copies the staged SourceDir' {
        $script:text | Should -Match '\{#SourceDir\}'
        $script:text | Should -Match 'recursesubdirs'
    }
    It 'creates a Start Menu shortcut to the app exe' { $script:text | Should -Match 'CameraOnScreen\.App\.exe' }
    It 'has a soft NVIDIA preflight in [Code]' {
        $script:text | Should -Match '\[Code\]'
        $script:text | Should -Match 'NVIDIA'
    }
}
