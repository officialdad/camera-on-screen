@{
    # Maxine bundle allow-list. Verified SDK pair: VFX 1.2.0.0 + AR 1.1.1.0
    # (TensorRT 10.9 / CUDA 12.x). Migrated from VFX 0.7.6 + AR 0.8.7 (TRT 10.4) for multi-GPU
    # model coverage (issue #2). Both SDKs moved to a dispatcher + per-feature-DLL model.
    #
    # The Dlls list was PRODUCED by running native/shim/smoke/trace_closure against the assembled
    # co-versioned stage on an RTX 3090 (sm86) and enumerating the modules actually loaded from
    # maxine\ (all four effects Start=1, 22 mandatory modules; FRUC DLLs in OptionalDlls, best-effort). Re-run the trace and update this list on any
    # SDK bump. See docs/superpowers/specs/2026-06-23-camera-on-screen-m5-multigpu-findings.md.
    #
    # VSR (issue #15) added 3 DLLs (static-import closure of nvVFXVideoSuperRes.dll via dumpbin):
    # nvVFXVideoSuperRes.dll (feature) -> nvngxruntime.dll (NGX loader, from VFX\bin) + nvngx_vsr.dll
    # (NGX model, 45 MB). VSR is NGX not TensorRT, so it pulls NO nvinfer/engine; nvcuda.dll is the
    # driver DLL (System32) and is never bundled. NVCVImage.dll is shared (already listed).
    #
    # SOURCE MODEL (bundle-maxine.ps1): the bundler PRUNES a pre-assembled, co-versioned flat
    # "stage" dir (-MaxineStage) down to this allow-list + model globs + license set. The stage is
    # the manual co-version curation (VFX-sourced shared DLLs, AR-only DLLs from AR, both SDKs'
    # per-feature DLLs, NGC-fetched multi-arch engines under models\{vfx,ar}, license files at
    # root) whose validity is proven by bundle_probe / trace_closure (single NVCVImage.dll, both
    # TRT-10.9 effects load). Physical co-version is enforced at assembly time, not by the bundler.

    # Every DLL in the gaze + green-screen + VSR load closure (trace_closure, sm86). All live in
    # the stage root. Dispatcher + per-feature DLLs are explicit (NVVideoEffects/nvARPose are the
    # dispatchers; nvVFXGreenScreen + the three nvAR* feature DLLs are loaded by them at runtime).
    # nvARFaceExpressions is intentionally absent (not in the gaze closure; emotion recognition is
    # also disallowed by the AI Product-Specific Terms s8.17).
    Dlls = @(
        'cudart64_12.dll'
        'libcrypto-3-x64.dll'
        'nppc64_12.dll'
        'nppial64_12.dll'
        'nppidei64_12.dll'
        'nppif64_12.dll'
        'nppig64_12.dll'
        'nppim64_12.dll'
        'nppist64_12.dll'
        'nppitc64_12.dll'
        'NVCVImage.dll'
        'nvinfer_10.dll'
        'nvonnxparser_10.dll'
        'NVVideoEffects.dll'        # VFX dispatcher
        'nvVFXGreenScreen.dll'      # VFX green-screen feature
        'nvVFXVideoSuperRes.dll'    # VFX video-super-res feature (NGX)
        'nvngxruntime.dll'          # NGX loader (pulled by nvVFXVideoSuperRes.dll)
        'nvngx_vsr.dll'             # NGX VSR model (no per-arch engine)
        'nvARPose.dll'              # AR dispatcher
        'nvARGazeRedirection.dll'   # AR gaze feature
        'nvARFaceBoxDetection.dll'  # AR gaze dep
        'nvARLandmarkDetection.dll' # AR gaze dep
    )

    # FRUC (frame-rate upscaling, issue #13): copied best-effort only when staged by
    # assemble-maxine-stage.ps1 with COS_FRUC_SDK_DIR set. If absent from the stage the bundle
    # proceeds without them and the app runs as a plain overlay (FRUC toggle unavailable).
    # Redistribution pending legal gate. NvOFFRUC requires CUDA 11 -- cudart64_110.dll is a
    # distinct name from cudart64_12.dll (CUDA 12) and they coexist without conflict.
    OptionalDlls = @(
        'NvOFFRUC.dll'              # FRUC frame-rate upscaler (issue #13)
        'cudart64_110.dll'          # CUDA 11 runtime required by NvOFFRUC (coexists with cudart64_12.dll)
    )

    # Model engines, copied from the stage's models\{vfx,ar}. Broad per-effect globs so the bundle
    # ships whatever arches are staged (sm75/86/89/100) -- arch selection happens at fetch time
    # (scripts/fetch-maxine-engines.ps1 -Arches). VFX -> models\vfx, AR -> models\ar.
    # Green-screen MODE is hardcoded to 0 in aigs.cpp (Probe + Start) -> only the m0 engines
    # (m0 + its m0_1_4_8 batch variant) ever load. Shipping m1/m2/m3 was ~73 MB/arch of dead
    # weight (~290 MB across the 4 arches). Widen this glob back to 'AIGS_*' if a quality-mode
    # selector is ever exposed in the UI -> shim.
    VfxModelGlobs = @(
        'AIGS_*_m0*.engine.trtpkg'
    )
    ArModelGlobs = @(
        'gazeredir_*.engine.trtpkg'
        'face_detection_*.engine.trtpkg'
        'faceland_*.engine.trtpkg'
    )

    # License notices that MUST travel with the redistributable (required copy -- the bundler
    # throws if any is missing). All staged at the maxine\ root. Redistribution basis + per-file
    # rationale: docs/superpowers/specs/2026-06-22-camera-on-screen-m5-license-compliance.md
    # (new-pair addendum). Summary:
    #   - Software License Agreement + Product-Specific Terms govern the runtime DLLs/SDK.
    #   - Open Model License governs the green-screen (AIGS) engines.
    #   - Community Model License governs the gaze/face engines; its s1.2(i) RTX exception permits
    #     no-charge redistribution for RTX-desktop, single-user use (our app) and REQUIRES a copy
    #     of the agreement to travel with the models.
    #   - Two ThirdPartyLicenses.txt (VFX + AR OSS load-closures incl. OpenSSL / libcrypto) carried
    #     under distinct names so neither SDK's third-party notices are dropped.
    LicenseFiles = @(
        'NVIDIA-Software-License-Agreement-2025.05.05.pdf'
        'product-specific-terms-for-nvidia-ai-products-2025.05.05.pdf'
        'NVIDIA-Open-Model-License-Agreements-24-10-2025.pdf'
        'NVIDIA-Models-Community-License-2025-04-15.pdf'
        'ThirdPartyLicenses-VFX.txt'
        'ThirdPartyLicenses-AR.txt'
    )
}
