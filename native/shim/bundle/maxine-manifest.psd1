@{
    # Maxine bundle allow-list. Verified SDK pair: VFX 0.7.6 + AR 0.8.7
    # (TensorRT 10.4.0.26 / CUDA 12.1). The DLL lists were PRODUCED by running
    # native/shim/smoke/trace_closure on the RTX 3090 (commit 9567dfa) and enumerating the
    # modules actually loaded from maxine\. Re-run the trace and update these lists when the
    # SDK versions bump.

    # Shared CUDA/TensorRT runtime DLLs that load AND exist in the VFX 0.7.6 runtime
    # (byte-identical to AR's copy — the co-version invariant). Sourced from -VfxRuntime.
    SharedDlls = @(
        'NVCVImage.dll'
        'cublasLt64_12.dll'
        'cudart64_12.dll'
        'libcrypto-3-x64.dll'
        'nppc64_12.dll'
        'nppial64_12.dll'
        'nppidei64_12.dll'
        'nppig64_12.dll'
        'nppim64_12.dll'
        'nppist64_12.dll'
        'nppitc64_12.dll'
        'nvinfer_10.dll'
        'nvonnxparser_10.dll'
    )

    # Shared runtime DLLs that load but exist ONLY in the AR 0.8.7 runtime (gaze pulls FFT +
    # the nppif image primitive; VFX 0.7.6 ships neither). Same CUDA 12.1 generation as the
    # VFX-sourced cudart/cublas, so co-version-safe. Sourced from -ArRuntime.
    ArSharedDlls = @(
        'cufft64_11.dll'
        'nppif64_12.dll'
    )

    VfxEffectDll = 'NVVideoEffects.dll'   # green-screen effect DLL — from -VfxRuntime
    ArEffectDll  = 'nvARPose.dll'         # gaze effect DLL        — from -ArRuntime

    # Ampere (_86) models only this milestone. Globs are arch-tagged for forward-compat.
    VfxModelGlobs = @(
        'AIGS_*_86_*.engine.trtpkg'
    )
    ArModelGlobs = @(
        'gazeredir_*_86*.engine.trtpkg'
        'face_detection_86*.engine.trtpkg'
        'faceland_*_86*.engine.trtpkg'
    )

    # License notices that MUST travel with the redistributable (required copy — the bundler
    # throws if any is missing). Maps source filename -> destination filename in maxine\.
    # The Maxine EULA is byte-identical across SDKs (one VFX copy covers the bundle). Each
    # SDK's ThirdPartyLicenses.txt has DIFFERENT content (its own OSS load-closure: VFX ~1.4 MB,
    # AR ~466 KB incl. cufft/nppif/nvARPose deps), so both are carried under distinct names —
    # copying only one would drop the other SDK's required third-party notices.
    VfxLicense = @{
        'NVIDIA Maxine EULA.pdf' = 'NVIDIA Maxine EULA.pdf'
        'ThirdPartyLicenses.txt' = 'ThirdPartyLicenses-VFX.txt'
    }
    ArLicense = @{
        'ThirdPartyLicenses.txt' = 'ThirdPartyLicenses-AR.txt'
    }
}
