#include "fruc.h"
#include <cstring>

#ifdef COS_HAS_FRUC
#include <windows.h>
#include <cuda.h>
#include "NvOFFRUC.h"
#include "paths.h"

namespace {
struct FrucImpl {
    HMODULE dll = nullptr;
    CUcontext ctx = nullptr; CUdevice dev = 0;
    NvOFFRUCHandle h = nullptr;
    CUdeviceptr buf[3] = {0,0,0};   // [0]=interpolate, [1..2]=render (sample's GetResource order)
    PtrToFuncNvOFFRUCCreate   pCreate   = nullptr;
    PtrToFuncNvOFFRUCRegisterResource   pReg = nullptr;
    PtrToFuncNvOFFRUCUnregisterResource pUnreg = nullptr;
    PtrToFuncNvOFFRUCProcess  pProcess  = nullptr;
    PtrToFuncNvOFFRUCDestroy  pDestroy  = nullptr;
    int renderIdx = 1;   // ping-pong: (renderIdx+1)%2 -> 0 or 1 -> buf[1] or buf[2]
    double lastTs = 0.0;
    bool primed = false;
};

static const double kInterval = 1.0;

// Resolves NvOFFRUC.dll: COS_FRUC_RUNTIME_DIR, else <shimDir>\maxine, else system PATH.
// LOAD_WITH_ALTERED_SEARCH_PATH on full paths so cudart64_110.dll resolves beside the dll.
HMODULE LoadFruc(std::string& err) {
    // (a) COS_FRUC_RUNTIME_DIR env override
    {
        wchar_t envBuf[2048] = {};
        DWORD n = GetEnvironmentVariableW(L"COS_FRUC_RUNTIME_DIR", envBuf, 2048);
        if (n > 0 && n < 2048) {
            std::wstring dll(envBuf, n);
            if (!dll.empty() && (dll.back() == L'\\' || dll.back() == L'/')) dll.pop_back();
            dll += L"\\NvOFFRUC.dll";
            HMODULE h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (h) return h;
        }
    }
    // (b) <ShimModuleDir()>\maxine\NvOFFRUC.dll — app-relative bundled path
    {
        std::string shimDir = ShimModuleDir();
        if (!shimDir.empty()) {
            std::string path = shimDir + "\\maxine\\NvOFFRUC.dll";
            wchar_t wpath[4096];
            int wn = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath, 4096);
            if (wn > 0) {
                HMODULE h = LoadLibraryExW(wpath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                if (h) return h;
            }
        }
    }
    // (c) fallback: system PATH / default search dirs
    {
        HMODULE h = LoadLibraryExW(L"NvOFFRUC.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (h) return h;
    }
    err = "NvOFFRUC.dll not found: set COS_FRUC_RUNTIME_DIR or bundle maxine\\ beside the app";
    return nullptr;
}
} // namespace

Fruc::Fruc() {}
Fruc::~Fruc() { Stop(); }

bool Fruc::Probe(std::string& detail) {
    Fruc f;
    if (!f.Start(1280, 720)) { detail = f.LastError(); return false; }
    detail = "FRUC available"; f.Stop(); return true;
}

bool Fruc::Start(int width, int height) {
    auto* impl = new FrucImpl();
    impl->dll = LoadFruc(lastError_);
    if (!impl->dll) { delete impl; return false; }
    impl->pCreate = (PtrToFuncNvOFFRUCCreate)GetProcAddress(impl->dll, CreateProcName);
    impl->pReg    = (PtrToFuncNvOFFRUCRegisterResource)GetProcAddress(impl->dll, RegisterResourceProcName);
    impl->pUnreg  = (PtrToFuncNvOFFRUCUnregisterResource)GetProcAddress(impl->dll, UnregisterResourceProcName);
    impl->pProcess= (PtrToFuncNvOFFRUCProcess)GetProcAddress(impl->dll, ProcessProcName);
    impl->pDestroy= (PtrToFuncNvOFFRUCDestroy)GetProcAddress(impl->dll, DestroyProcName);
    if (!impl->pCreate || !impl->pReg || !impl->pUnreg || !impl->pProcess || !impl->pDestroy) {
        lastError_ = "NvOFFRUC exports missing"; FreeLibrary(impl->dll); delete impl; return false;
    }
    // CUDA ctx setup: track retain separately so a cuCtxSetCurrent failure releases the ctx.
    if (cuInit(0) != CUDA_SUCCESS || cuDeviceGet(&impl->dev, 0) != CUDA_SUCCESS) {
        lastError_ = "CUDA ctx setup failed"; FreeLibrary(impl->dll); delete impl; return false;
    }
    bool ctxRetained = (cuDevicePrimaryCtxRetain(&impl->ctx, impl->dev) == CUDA_SUCCESS);
    if (!ctxRetained || cuCtxSetCurrent(impl->ctx) != CUDA_SUCCESS) {
        lastError_ = "CUDA ctx setup failed";
        if (ctxRetained) cuDevicePrimaryCtxRelease(impl->dev);
        FreeLibrary(impl->dll); delete impl; return false;
    }
    NvOFFRUC_CREATE_PARAM cp{};
    cp.uiWidth = (uint32_t)width; cp.uiHeight = (uint32_t)height; cp.pDevice = nullptr;
    cp.eResourceType = CudaResource; cp.eSurfaceFormat = ARGBSurface;
    cp.eCUDAResourceType = CudaResourceCuDevicePtr;
    if (impl->pCreate(&cp, &impl->h) != NvOFFRUC_SUCCESS) {
        lastError_ = "NvOFFRUCCreate failed";
        cuDevicePrimaryCtxRelease(impl->dev); FreeLibrary(impl->dll); delete impl; return false;
    }
    const size_t bytes = (size_t)width * height * 4;
    for (int i = 0; i < 3; ++i) {
        if (cuMemAlloc(&impl->buf[i], bytes) != CUDA_SUCCESS) {
            lastError_ = "cuMemAlloc failed";
            for (int j = 0; j < i; ++j) cuMemFree(impl->buf[j]);
            impl->pDestroy(impl->h);
            cuDevicePrimaryCtxRelease(impl->dev); FreeLibrary(impl->dll); delete impl; return false;
        }
    }
    NvOFFRUC_REGISTER_RESOURCE_PARAM reg{};
    reg.pArrResource[0] = &impl->buf[0]; reg.pArrResource[1] = &impl->buf[1];
    reg.pArrResource[2] = &impl->buf[2]; reg.uiCount = 3; reg.pD3D11FenceObj = nullptr;
    if (impl->pReg(impl->h, &reg) != NvOFFRUC_SUCCESS) {
        lastError_ = "RegisterResource failed";
        for (auto& b : impl->buf) cuMemFree(b);
        impl->pDestroy(impl->h); cuDevicePrimaryCtxRelease(impl->dev); FreeLibrary(impl->dll); delete impl; return false;
    }
    impl_ = impl; width_ = width; height_ = height; ready_ = true; return true;
}

bool Fruc::Submit(const uint8_t* curBgra, int width, int height,
                  std::vector<uint8_t>& outMid, bool& hasMid) {
    hasMid = false;
    if (!ready_) { lastError_ = "FRUC not started"; return false; }
    if (width != width_ || height != height_) { lastError_ = "frame size changed"; return false; }
    auto* impl = static_cast<FrucImpl*>(impl_);
    if (cuCtxSetCurrent(impl->ctx) != CUDA_SUCCESS) { lastError_ = "cuCtxSetCurrent failed"; return false; }
    const size_t bytes = (size_t)width * height * 4;
    impl->renderIdx = (impl->renderIdx + 1) % 2;          // ping-pong buf[1]/buf[2]
    const int rb = 1 + impl->renderIdx;
    if (cuMemcpyHtoD(impl->buf[rb], curBgra, bytes) != CUDA_SUCCESS) { lastError_ = "upload failed"; return false; }
    const double renderTs = impl->lastTs + kInterval;
    NvOFFRUC_PROCESS_IN_PARAMS  in{};
    NvOFFRUC_PROCESS_OUT_PARAMS out{};
    bool rep = false;
    in.stFrameDataInput.pFrame = &impl->buf[rb];
    in.stFrameDataInput.nTimeStamp = renderTs;
    in.stFrameDataInput.nCuSurfacePitch = (size_t)width * 4;
    out.stFrameDataOutput.pFrame = &impl->buf[0];
    out.stFrameDataOutput.nTimeStamp = renderTs - kInterval * 0.5;   // midpoint
    out.stFrameDataOutput.nCuSurfacePitch = (size_t)width * 4;
    out.stFrameDataOutput.bHasFrameRepetitionOccurred = &rep;
    const NvOFFRUC_STATUS st = impl->pProcess(impl->h, &in, &out);
    impl->lastTs = renderTs;
    const bool first = !impl->primed;
    impl->primed = true;
    if (st != NvOFFRUC_SUCCESS) { lastError_ = "Process failed"; return false; }
    if (first) return true;                       // no previous frame yet -> hasMid stays false
    std::vector<uint8_t> tmp(bytes);
    if (cuMemcpyDtoH(tmp.data(), impl->buf[0], bytes) != CUDA_SUCCESS) { lastError_ = "download failed"; return false; }
    outMid.swap(tmp);
    hasMid = true;
    return true;
}

void Fruc::Stop() {
    if (!impl_) { ready_ = false; return; }
    auto* impl = static_cast<FrucImpl*>(impl_);
    if (impl->h) {
        NvOFFRUC_UNREGISTER_RESOURCE_PARAM u{};
        u.pArrResource[0]=&impl->buf[0]; u.pArrResource[1]=&impl->buf[1]; u.pArrResource[2]=&impl->buf[2];
        u.uiCount=3; impl->pUnreg(impl->h, &u);
    }
    for (auto& b : impl->buf) if (b) cuMemFree(b);
    if (impl->h) impl->pDestroy(impl->h);
    if (impl->ctx) cuDevicePrimaryCtxRelease(impl->dev);
    if (impl->dll) FreeLibrary(impl->dll);
    delete impl; impl_ = nullptr; ready_ = false;
}
#endif // COS_HAS_FRUC

#ifndef COS_HAS_FRUC
// FRUC is OPTIONAL (legal gate open; ships as an optional bundler tier). These stub messages must
// NOT contain the substring "not built in" -- the CI export-verify uses that phrase to detect a
// stubbed REQUIRED effect (green screen / gaze), and an optional-FRUC stub must not false-trip it.
Fruc::Fruc() {}
Fruc::~Fruc() {}
bool Fruc::Probe(std::string& detail) { detail = "FRUC unavailable (COS_HAS_FRUC unset)"; return false; }
bool Fruc::Start(int, int) { lastError_ = "FRUC unavailable (COS_HAS_FRUC unset)"; return false; }
void Fruc::Stop() { ready_ = false; }
bool Fruc::Submit(const uint8_t*, int, int, std::vector<uint8_t>&, bool& hasMid) {
    hasMid = false; lastError_ = "FRUC unavailable (COS_HAS_FRUC unset)"; return false;
}
#endif // !COS_HAS_FRUC
