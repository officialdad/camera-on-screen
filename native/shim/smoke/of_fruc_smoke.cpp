// Issue #13 HARD-GATE smoke: can NVIDIA Optical Flow FRUC (NvOFFRUC.dll) coexist in ONE
// process with the app's pinned Maxine VFX 1.2.0.0 + AR 1.1.1.0 runtime (CUDA 12.x / TRT 10.9)?
//
// Per CLAUDE.md CO-VERSION: two TRT/CUDA runtimes with the SAME dll name can't coexist
// (first LoadLibrary wins, loser's load fails with cudaErrorNoKernelImageForDevice). FRUC's
// closure is cudart64_110.dll + nvcuda.dll (NO TensorRT, NO NVCVImage) -- distinct names from
// VFX/AR's cudart64_12.dll/nvinfer_10.dll, so the hazard should NOT trigger. This proves it
// empirically: load VFX+AR FIRST (they grab their runtime), THEN load + create + run FRUC.
//
// GATE = NvOFFRUCCreate returns NvOFFRUC_SUCCESS (Create inits the CUDA-11 runtime + NVOFA
// engine + fatbin kernels -- exactly where a cross-runtime CUDA conflict surfaces). Process is
// a best-effort end-to-end confirmation (constant frames => frame-repetition is fine).
//
// Build: native\shim\smoke\build_of_fruc_smoke.bat <out.exe>   (see that file for env vars)
// Run with COS_VFX_RUNTIME_DIR + COS_AR_RUNTIME_DIR -> the co-versioned maxine stage, and the
// FRUC bin\win64 dir on PATH (or pass NvOFFRUC.dll's full path as argv[1]).
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <windows.h>
#include <psapi.h>
#include <cuda.h>            // CUDA driver API (cuda.lib) -- version-agnostic, via nvcuda.dll
#include "../aigs.h"
#include "../eyecontact.h"
#include "../superres.h"
#include "NvOFFRUC.h"        // OF SDK: NvOFFRUC\Interface (via /I)

static std::string ToLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Print every loaded module whose name contains one of the runtime markers -- visual proof that
// both CUDA runtimes (12 + 110) + the driver are co-resident with no name clash.
static void DumpRuntimeModules() {
    const char* markers[] = { "cudart64_12", "cudart64_110", "cudart64_11", "nvinfer_10",
                              "nvcuda", "nvoffruc", "nvcvimage", "nvvfx", "nvar" };
    std::vector<HMODULE> mods(2048);
    DWORD needed = 0;
    if (!EnumProcessModulesEx(GetCurrentProcess(), mods.data(),
                              (DWORD)(mods.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL))
        return;
    const int n = (int)(needed / sizeof(HMODULE));
    std::printf("# co-resident runtime modules:\n");
    for (int i = 0; i < n && i < (int)mods.size(); ++i) {
        wchar_t pathW[MAX_PATH];
        if (!GetModuleFileNameW(mods[i], pathW, MAX_PATH)) continue;
        char path[MAX_PATH * 2];
        if (WideCharToMultiByte(CP_UTF8, 0, pathW, -1, path, sizeof(path), nullptr, nullptr) <= 0) continue;
        const std::string lp = ToLower(path);
        for (const char* m : markers) {
            if (lp.find(m) != std::string::npos) { std::printf("    %s\n", path); break; }
        }
    }
}

int main(int argc, char** argv) {
    const int W = 1280, H = 720;
    const size_t kFrameBytes = (size_t)W * H * 4;  // ARGB

    // -------- Phase 1: bring up VFX (green screen) + AR (gaze) -- CUDA 12 / TRT 10.9 resident.
    Aigs gs; EyeContact ec; SuperRes sr;
    const bool gsOk = gs.Start();
    const bool ecOk = ec.Start();
    const bool srOk = sr.Start(1, 20);
    std::vector<uint8_t> frame(kFrameBytes, (uint8_t)128);
    if (gsOk) gs.ProcessFrame(frame.data(), W, H, 0.0, 0.0);
    if (ecOk) ec.ProcessFrame(frame.data(), W, H);
    if (srOk) { std::vector<uint8_t> out; int ow = 0, oh = 0; sr.ProcessFrame(frame.data(), W, H, out, ow, oh); }
    std::printf("# Phase1 VFX+AR resident: greenScreen=%d eyeContact=%d superRes=%d\n",
                gsOk ? 1 : 0, ecOk ? 1 : 0, srOk ? 1 : 0);
    if (!gsOk) std::printf("#   GS error: %s\n", gs.LastError().c_str());
    if (!ecOk) std::printf("#   EC error: %s\n", ec.LastError().c_str());
    if (!srOk) std::printf("#   SR error: %s\n", sr.LastError().c_str());
    if (!gsOk && !ecOk) std::printf("# WARN: no Maxine effect loaded -- co-version not actually exercised "
                                    "(set COS_VFX_RUNTIME_DIR/COS_AR_RUNTIME_DIR to the maxine stage)\n");

    // -------- Phase 2: load FRUC into the SAME process. Full path => its own dir resolves deps
    // (cudart64_110.dll sits beside it). LOAD_WITH_ALTERED_SEARCH_PATH per the SDK's loader.
    const wchar_t* dllArg = (argc > 1) ? nullptr : L"NvOFFRUC.dll";
    HMODULE h = nullptr;
    if (argc > 1) {
        wchar_t wp[MAX_PATH * 2];
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, wp, MAX_PATH * 2);
        h = LoadLibraryExW(wp, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    } else {
        h = LoadLibraryExW(dllArg, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    }
    if (!h) { std::printf("# FAIL: LoadLibrary NvOFFRUC.dll failed (err=%lu). Pass full dll path as argv[1].\n",
                          GetLastError()); DumpRuntimeModules(); return 2; }

    auto pCreate   = (PtrToFuncNvOFFRUCCreate)GetProcAddress(h, CreateProcName);
    auto pRegister = (PtrToFuncNvOFFRUCRegisterResource)GetProcAddress(h, RegisterResourceProcName);
    auto pUnreg    = (PtrToFuncNvOFFRUCUnregisterResource)GetProcAddress(h, UnregisterResourceProcName);
    auto pProcess  = (PtrToFuncNvOFFRUCProcess)GetProcAddress(h, ProcessProcName);
    auto pDestroy  = (PtrToFuncNvOFFRUCDestroy)GetProcAddress(h, DestroyProcName);
    if (!pCreate || !pRegister || !pUnreg || !pProcess || !pDestroy) {
        std::printf("# FAIL: NvOFFRUC exports missing\n"); DumpRuntimeModules(); return 2;
    }

    // A current CUDA context for FRUC's CUDA-mode buffers (pDevice=NULL => FRUC uses current ctx).
    CUdevice dev; CUcontext ctx;
    if (cuInit(0) != CUDA_SUCCESS || cuDeviceGet(&dev, 0) != CUDA_SUCCESS ||
        cuDevicePrimaryCtxRetain(&ctx, dev) != CUDA_SUCCESS || cuCtxSetCurrent(ctx) != CUDA_SUCCESS) {
        std::printf("# FAIL: CUDA driver-API context setup failed\n"); DumpRuntimeModules(); return 2;
    }

    NvOFFRUC_CREATE_PARAM cp{};
    cp.uiWidth = W; cp.uiHeight = H; cp.pDevice = nullptr;
    cp.eResourceType = CudaResource; cp.eSurfaceFormat = ARGBSurface;
    cp.eCUDAResourceType = CudaResourceCuDevicePtr;
    NvOFFRUCHandle hFRUC = nullptr;
    NvOFFRUC_STATUS st = pCreate(&cp, &hFRUC);
    std::printf("\n#### GATE: NvOFFRUCCreate -> %d (%s)\n", (int)st,
                st == NvOFFRUC_SUCCESS ? "SUCCESS -- co-version OK" : "FAIL");
    if (st != NvOFFRUC_SUCCESS) { DumpRuntimeModules(); return 1; }

    // -------- Phase 3 (best-effort): register 1 interpolate + 2 render buffers, run 2 frames.
    bool processOk = false;
    CUdeviceptr buf[3] = {0, 0, 0};  // [0]=interpolate, [1..2]=render (sample's GetResource order)
    bool allocOk = true;
    for (int i = 0; i < 3; ++i)
        if (cuMemAlloc(&buf[i], kFrameBytes) != CUDA_SUCCESS) allocOk = false;
        else cuMemsetD8(buf[i], (unsigned char)(64 + i * 32), kFrameBytes);
    if (allocOk) {
        NvOFFRUC_REGISTER_RESOURCE_PARAM reg{};
        reg.pArrResource[0] = &buf[0];
        reg.pArrResource[1] = &buf[1];
        reg.pArrResource[2] = &buf[2];
        reg.uiCount = 3; reg.pD3D11FenceObj = nullptr;
        NvOFFRUC_STATUS rst = pRegister(hFRUC, &reg);
        std::printf("# RegisterResource -> %d\n", (int)rst);
        if (rst == NvOFFRUC_SUCCESS) {
            NvOFFRUC_STATUS last = NvOFFRUC_SUCCESS;
            for (int f = 0; f < 2; ++f) {
                bool rep = false;
                NvOFFRUC_PROCESS_IN_PARAMS  in{};
                NvOFFRUC_PROCESS_OUT_PARAMS out{};
                in.stFrameDataInput.pFrame = &buf[1 + (f % 2)];  // render buffer
                in.stFrameDataInput.nTimeStamp = (double)f;
                in.stFrameDataInput.nCuSurfacePitch = (size_t)W * 4;
                out.stFrameDataOutput.pFrame = &buf[0];           // interpolate buffer
                out.stFrameDataOutput.nTimeStamp = (double)f + 0.5;
                out.stFrameDataOutput.nCuSurfacePitch = (size_t)W * 4;
                out.stFrameDataOutput.bHasFrameRepetitionOccurred = &rep;
                last = pProcess(hFRUC, &in, &out);
                std::printf("#   Process frame %d -> %d (repeat=%d)\n", f, (int)last, rep ? 1 : 0);
            }
            processOk = (last == NvOFFRUC_SUCCESS);
            NvOFFRUC_UNREGISTER_RESOURCE_PARAM unreg{};
            unreg.pArrResource[0] = &buf[0]; unreg.pArrResource[1] = &buf[1]; unreg.pArrResource[2] = &buf[2];
            unreg.uiCount = 3;
            pUnreg(hFRUC, &unreg);
        }
    }
    std::printf("# Phase3 Process end-to-end: %s\n", processOk ? "ok" : "best-effort/skipped");

    DumpRuntimeModules();

    // -------- Cleanup.
    for (int i = 0; i < 3; ++i) if (buf[i]) cuMemFree(buf[i]);
    pDestroy(hFRUC);
    cuDevicePrimaryCtxRelease(dev);
    gs.Stop(); ec.Stop(); sr.Stop();

    std::printf("\n#### VERDICT: FRUC co-version GATE PASSED (Create ok in-process with VFX+AR).%s\n",
                processOk ? " Process produced output too." : "");
    return 0;
}
