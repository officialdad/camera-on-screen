Describe 'bundle-maxine' {
    BeforeEach {
        $script:root = Join-Path ([IO.Path]::GetTempPath()) ("bm_" + [guid]::NewGuid())
        $script:vfx  = Join-Path $root 'vfx'
        $script:ar   = Join-Path $root 'ar'
        $script:out  = Join-Path $root 'out'
        New-Item -ItemType Directory -Force -Path (Join-Path $vfx 'models'),(Join-Path $ar 'models'),$out | Out-Null
        'x' | Set-Content (Join-Path $vfx 'nvinfer_10.dll')
        'x' | Set-Content (Join-Path $vfx 'NVVideoEffects.dll')
        'x' | Set-Content (Join-Path $vfx 'NVIDIA Maxine EULA.pdf')
        'vfx-notices' | Set-Content (Join-Path $vfx 'ThirdPartyLicenses.txt')
        'ar-notices'  | Set-Content (Join-Path $ar  'ThirdPartyLicenses.txt')
        'x' | Set-Content (Join-Path $ar  'nvARPose.dll')
        'x' | Set-Content (Join-Path $ar  'cufft64_11.dll')
        'x' | Set-Content (Join-Path $vfx 'models\AIGS_288x512_86_m0.engine.trtpkg')
        'x' | Set-Content (Join-Path $ar  'models\gazeredir_encoder_fp16_86.engine.trtpkg')
        'x' | Set-Content (Join-Path $ar  'models\face_detection_86.engine.trtpkg')
        'x' | Set-Content (Join-Path $ar  'models\faceland_fp16_rcn_mode0_86.engine.trtpkg')
        $script:manifest = Join-Path $root 'm.psd1'
        @'
@{
  SharedDlls = @('nvinfer_10.dll')
  ArSharedDlls = @('cufft64_11.dll')
  VfxEffectDll = 'NVVideoEffects.dll'
  ArEffectDll = 'nvARPose.dll'
  VfxModelGlobs = @('AIGS_*_86_*.engine.trtpkg')
  ArModelGlobs = @('gazeredir_*_86*.engine.trtpkg','face_detection_86*.engine.trtpkg','faceland_*_86*.engine.trtpkg')
  VfxLicense = @{ 'NVIDIA Maxine EULA.pdf' = 'NVIDIA Maxine EULA.pdf'; 'ThirdPartyLicenses.txt' = 'ThirdPartyLicenses-VFX.txt' }
  ArLicense  = @{ 'ThirdPartyLicenses.txt' = 'ThirdPartyLicenses-AR.txt' }
}
'@ | Set-Content -LiteralPath $manifest
        $script:script = Join-Path $PSScriptRoot 'bundle-maxine.ps1'
    }
    AfterEach { Remove-Item -Recurse -Force $script:root -ErrorAction SilentlyContinue }

    It 'produces the maxine layout from the manifest' {
        & $script -OutDir $out -VfxRuntime $vfx -ArRuntime $ar -ManifestPath $manifest
        Test-Path (Join-Path $out 'maxine\nvinfer_10.dll')          | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\cufft64_11.dll')          | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\NVVideoEffects.dll')      | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\nvARPose.dll')            | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\NVIDIA Maxine EULA.pdf')  | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\models\vfx\AIGS_288x512_86_m0.engine.trtpkg') | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\models\ar\gazeredir_encoder_fp16_86.engine.trtpkg') | Should -BeTrue
        Test-Path (Join-Path $out 'maxine\models\ar\face_detection_86.engine.trtpkg')   | Should -BeTrue
    }
    It 'carries BOTH SDKs ThirdPartyLicenses under distinct names (co-version notices)' {
        & $script -OutDir $out -VfxRuntime $vfx -ArRuntime $ar -ManifestPath $manifest
        $vfxOut = Join-Path $out 'maxine\ThirdPartyLicenses-VFX.txt'
        $arOut  = Join-Path $out 'maxine\ThirdPartyLicenses-AR.txt'
        Test-Path $vfxOut | Should -BeTrue
        Test-Path $arOut  | Should -BeTrue
        (Get-Content -LiteralPath $vfxOut -Raw).Trim() | Should -Be 'vfx-notices'
        (Get-Content -LiteralPath $arOut  -Raw).Trim() | Should -Be 'ar-notices'
        # the colliding source name must NOT survive verbatim (it would clobber one SDK's notices)
        Test-Path (Join-Path $out 'maxine\ThirdPartyLicenses.txt') | Should -BeFalse
    }
    It 'throws when the AR ThirdPartyLicenses.txt is missing (license copy is required, not best-effort)' {
        Remove-Item (Join-Path $ar 'ThirdPartyLicenses.txt')
        { & $script -OutDir $out -VfxRuntime $vfx -ArRuntime $ar -ManifestPath $manifest } | Should -Throw
    }
    It 'throws when a required VFX DLL is missing from the source' {
        Remove-Item (Join-Path $vfx 'nvinfer_10.dll')
        { & $script -OutDir $out -VfxRuntime $vfx -ArRuntime $ar -ManifestPath $manifest } | Should -Throw
    }
    It 'throws when an AR-only shared DLL is missing from the AR source' {
        Remove-Item (Join-Path $ar 'cufft64_11.dll')
        { & $script -OutDir $out -VfxRuntime $vfx -ArRuntime $ar -ManifestPath $manifest } | Should -Throw
    }
    It 'throws when a models source dir is missing' {
        Remove-Item -Recurse -Force (Join-Path $vfx 'models')
        { & $script -OutDir $out -VfxRuntime $vfx -ArRuntime $ar -ManifestPath $manifest } | Should -Throw
    }
}
