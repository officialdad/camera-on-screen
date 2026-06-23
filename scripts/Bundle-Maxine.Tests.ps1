Describe 'bundle-maxine' {
    BeforeEach {
        $script:root  = Join-Path ([IO.Path]::GetTempPath()) ("bm_" + [guid]::NewGuid())
        $script:stage = Join-Path $root 'stage'
        $script:out   = Join-Path $root 'out'
        New-Item -ItemType Directory -Force -Path (Join-Path $stage 'models\vfx'), (Join-Path $stage 'models\ar'), $out | Out-Null
        # stage root: allow-listed DLLs + license files all live flat here.
        'x' | Set-Content (Join-Path $stage 'nvinfer_10.dll')
        'x' | Set-Content (Join-Path $stage 'NVVideoEffects.dll')
        'x' | Set-Content (Join-Path $stage 'nvVFXGreenScreen.dll')
        'x' | Set-Content (Join-Path $stage 'nvARPose.dll')
        'x' | Set-Content (Join-Path $stage 'nvARGazeRedirection.dll')
        'sla'       | Set-Content (Join-Path $stage 'NVIDIA-Software-License-Agreement-2025.05.05.pdf')
        'community' | Set-Content (Join-Path $stage 'NVIDIA-Models-Community-License-2025-04-15.pdf')
        'vfx-notices' | Set-Content (Join-Path $stage 'ThirdPartyLicenses-VFX.txt')
        'ar-notices'  | Set-Content (Join-Path $stage 'ThirdPartyLicenses-AR.txt')
        'x' | Set-Content (Join-Path $stage 'models\vfx\AIGS_288x512_86_m0.engine.trtpkg')
        'x' | Set-Content (Join-Path $stage 'models\vfx\AIGS_288x512_75_m0.engine.trtpkg')
        'x' | Set-Content (Join-Path $stage 'models\ar\gazeredir_encoder_fp16_86.engine.trtpkg')
        'x' | Set-Content (Join-Path $stage 'models\ar\face_detection_86.engine.trtpkg')
        'x' | Set-Content (Join-Path $stage 'models\ar\faceland_fp16_rcn_mode0_86.engine.trtpkg')
        $script:manifest = Join-Path $root 'm.psd1'
        @'
@{
  Dlls = @('nvinfer_10.dll','NVVideoEffects.dll','nvVFXGreenScreen.dll','nvARPose.dll','nvARGazeRedirection.dll')
  VfxModelGlobs = @('AIGS_*.engine.trtpkg')
  ArModelGlobs = @('gazeredir_*.engine.trtpkg','face_detection_*.engine.trtpkg','faceland_*.engine.trtpkg')
  LicenseFiles = @('NVIDIA-Software-License-Agreement-2025.05.05.pdf','NVIDIA-Models-Community-License-2025-04-15.pdf','ThirdPartyLicenses-VFX.txt','ThirdPartyLicenses-AR.txt')
}
'@ | Set-Content -LiteralPath $manifest
        $script:script = Join-Path $PSScriptRoot 'bundle-maxine.ps1'
    }
    AfterEach { Remove-Item -Recurse -Force $script:root -ErrorAction SilentlyContinue }

    It 'prunes the stage into the maxine layout per the manifest' {
        & $script -OutDir $out -MaxineStage $stage -ManifestPath $manifest
        Test-Path (Join-Path $out 'maxine\nvinfer_10.dll')          | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\nvVFXGreenScreen.dll')    | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\nvARGazeRedirection.dll') | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\models\vfx\AIGS_288x512_86_m0.engine.trtpkg') | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\models\ar\gazeredir_encoder_fp16_86.engine.trtpkg') | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\models\ar\face_detection_86.engine.trtpkg')   | Should -BeTrue
    }
    It 'ships every staged arch (broad globs, multi-GPU)' {
        & $script -OutDir $out -MaxineStage $stage -ManifestPath $manifest
        Test-Path (Join-Path $out 'maxine\models\vfx\AIGS_288x512_86_m0.engine.trtpkg') | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\models\vfx\AIGS_288x512_75_m0.engine.trtpkg') | Should -BeTrue
    }
    It 'carries all required license files' {
        & $script -OutDir $out -MaxineStage $stage -ManifestPath $manifest
        Test-Path (Join-Path $out 'maxine\NVIDIA-Software-License-Agreement-2025.05.05.pdf') | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\NVIDIA-Models-Community-License-2025-04-15.pdf')   | Should -BeTrue
        $vfxOut = Join-Path $out 'maxine\ThirdPartyLicenses-VFX.txt'
        $arOut  = Join-Path $out 'maxine\ThirdPartyLicenses-AR.txt'
        (Get-Content -LiteralPath $vfxOut -Raw).Trim() | Should -Be 'vfx-notices'
        (Get-Content -LiteralPath $arOut  -Raw).Trim() | Should -Be 'ar-notices'
    }
    It 'throws when a required license file is missing (license copy is required, not best-effort)' {
        Remove-Item (Join-Path $stage 'NVIDIA-Models-Community-License-2025-04-15.pdf')
        { & $script -OutDir $out -MaxineStage $stage -ManifestPath $manifest } | Should -Throw
    }
    It 'throws when a required DLL is missing from the stage' {
        Remove-Item (Join-Path $stage 'nvARGazeRedirection.dll')
        { & $script -OutDir $out -MaxineStage $stage -ManifestPath $manifest } | Should -Throw
    }
    It 'throws when a models source dir is missing' {
        Remove-Item -Recurse -Force (Join-Path $stage 'models\vfx')
        { & $script -OutDir $out -MaxineStage $stage -ManifestPath $manifest } | Should -Throw
    }
}
