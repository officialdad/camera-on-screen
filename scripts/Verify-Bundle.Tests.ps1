Describe 'verify-bundle' {
    BeforeAll { $script:s = Join-Path $PSScriptRoot 'verify-bundle.ps1' }

    It 'exposes the documented parameters' {
        $p = (Get-Command $script:s).Parameters.Keys
        foreach ($name in 'StagingDir','VfxSdkDir','ArSdkDir','ProbeBat','DryRun') {
            $p | Should -Contain $name
        }
    }

    It 'dry-run prints the probe build/run plan (bundled maxine, COS_* unset)' {
        $stage = Join-Path ([IO.Path]::GetTempPath()) ("vb_" + [guid]::NewGuid())
        $out = & $script:s -StagingDir $stage -DryRun 6>&1 | Out-String
        $out | Should -Match 'bundle_probe'
        $out | Should -Match 'maxine'
        $out | Should -Match 'COS_'
    }

    It 'throws when the staging dir has no maxine\ to probe' {
        $stage = Join-Path ([IO.Path]::GetTempPath()) ("vb_" + [guid]::NewGuid())
        New-Item -ItemType Directory -Force -Path $stage | Out-Null
        try { { & $script:s -StagingDir $stage } | Should -Throw }
        finally { Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue }
    }
}
