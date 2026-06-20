#include "aigs.h"

#ifdef COS_HAS_MAXINE
#include <windows.h>
#include <cstdlib>
#include <string>
#include "nvCVStatus.h"
#include "nvCVImage.h"
#include "nvVideoEffects.h"
#include "nvVFXGreenScreen.h" // defines NVVFX_FX_GREEN_SCREEN "GreenScreen"

// The proxy stub (nvVideoEffectsProxy.cpp) declares this as extern; we define it here.
// When non-null it points at the dir that holds NVVideoEffects.dll, which the proxy
// passes to SetDllDirectory before LoadLibrary.
// (Defined in Task 1 — NOT redefined here.)
char* g_nvVFXSDKPath = nullptr;

namespace {
// Resolves "<COS_VFX_SDK_DIR>\bin" (DLLs) and "<COS_VFX_SDK_DIR>\bin\models" once.
// Returns false if the env var is unset/empty. Stores results in out-params so
// callers can keep them alive as needed.
bool ResolveSdkPaths(std::string& binDir, std::string& modelDir, std::string& err) {
    char buf[1024] = {0};
    DWORD n = GetEnvironmentVariableA("COS_VFX_SDK_DIR", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) { err = "COS_VFX_SDK_DIR not set"; return false; }
    std::string root(buf, n);
    if (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
    binDir   = root + "\\bin";
    modelDir = root + "\\bin\\models";
    return true;
}

// Points the VFX proxy global at the bin dir so NVVideoEffects.dll is found.
// NVCVImage.dll resolves from the same search path that VFX proxy sets.
// Idempotent; s_bin must outlive every Maxine call.
void PointProxiesAt(const std::string& binDir) {
    static std::string s_bin;
    s_bin = binDir;
    g_nvVFXSDKPath = const_cast<char*>(s_bin.c_str());
}
} // namespace

// Real per-effect state, hidden behind the opaque impl_ pointer.
struct AigsImpl {
    NvVFX_Handle effect = nullptr;
    CUstream     stream = nullptr;
    std::string  modelDir;
    // GPU/CPU images are added in Task 3.
};

Aigs::Aigs() = default;
Aigs::~Aigs() { Stop(); }

bool Aigs::Probe(std::string& detail) {
    std::string binDir, modelDir, err;
    if (!ResolveSdkPaths(binDir, modelDir, err)) { detail = err; return false; }
    PointProxiesAt(binDir);

    NvVFX_Handle eff = nullptr;
    if (NvVFX_CreateEffect(NVVFX_FX_GREEN_SCREEN, &eff) != NVCV_SUCCESS || !eff) {
        detail = "NvVFX_CreateEffect(GreenScreen) failed (DLL/SDK load?)";
        return false;
    }
    NvVFX_SetString(eff, NVVFX_MODEL_DIRECTORY, modelDir.c_str());
    // Set mode 0 (best quality) for the probe load — models AIGS_288x512_86_m0*.engine.trtpkg
    // are confirmed present. Mode 1 is the runtime preference (set in Start); probe just
    // needs any successful load to confirm the SDK is functional.
    NvVFX_SetU32(eff, NVVFX_MODE, 0u);
    // Set max input dimensions so NvVFX_Load can build the TensorRT engine.
    NvVFX_SetU32(eff, NVVFX_MAX_INPUT_WIDTH,  1920u);
    NvVFX_SetU32(eff, NVVFX_MAX_INPUT_HEIGHT, 1080u);
    NvCV_Status load = NvVFX_Load(eff);
    NvVFX_DestroyEffect(eff);
    if (load != NVCV_SUCCESS) {
        detail = "NvVFX_Load failed (models missing or GPU incompatible?)";
        return false;
    }
    detail = "GreenScreen available";
    return true;
}

bool Aigs::Start() {
    Stop();
    auto* impl = new AigsImpl();
    impl_ = impl;

    std::string binDir, err;
    if (!ResolveSdkPaths(binDir, impl->modelDir, err)) { lastError_ = err; return false; }
    PointProxiesAt(binDir);

    if (NvVFX_CudaStreamCreate(&impl->stream) != NVCV_SUCCESS) {
        lastError_ = "NvVFX_CudaStreamCreate failed"; return false;
    }
    if (NvVFX_CreateEffect(NVVFX_FX_GREEN_SCREEN, &impl->effect) != NVCV_SUCCESS || !impl->effect) {
        lastError_ = "NvVFX_CreateEffect failed"; return false;
    }
    NvVFX_SetString(impl->effect, NVVFX_MODEL_DIRECTORY, impl->modelDir.c_str());
    NvVFX_SetCudaStream(impl->effect, NVVFX_CUDA_STREAM, impl->stream);
    NvVFX_SetU32(impl->effect, NVVFX_MODE,     0u); // mode 0 = best quality; models m0 confirmed present
    NvVFX_SetU32(impl->effect, NVVFX_TEMPORAL, 1u); // video: reduce matte flicker
    // NvVFX_Load is called after SetImage in Task 3 (requires known input dimensions).
    ready_ = true; // "configured"; model load completes on first frame (Task 3)
    lastError_.clear();
    return true;
}

void Aigs::Stop() {
    auto* impl = static_cast<AigsImpl*>(impl_);
    if (!impl) { ready_ = false; return; }
    if (impl->effect) NvVFX_DestroyEffect(impl->effect);
    if (impl->stream) NvVFX_CudaStreamDestroy(impl->stream);
    delete impl;
    impl_ = nullptr;
    ready_ = false;
}

bool Aigs::ProcessFrame(uint8_t*, int, int) { return false; } // implemented in Task 3

#else
// ---- Passthrough stub: built when no SDK is configured. ----
Aigs::Aigs() = default;
Aigs::~Aigs() = default;
bool Aigs::Probe(std::string& detail) { detail = "Maxine SDK not built in"; return false; }
bool Aigs::Start() { lastError_ = "Maxine SDK not built in"; ready_ = false; return false; }
void Aigs::Stop() { ready_ = false; }
bool Aigs::ProcessFrame(uint8_t*, int, int) { return false; }
#endif
