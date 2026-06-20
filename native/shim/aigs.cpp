#include "aigs.h"

#ifdef COS_HAS_MAXINE
#include <windows.h>
#include <cstdlib>
#include <new>
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
    NvCVImage srcGpu{};   // BGR  u8 chunky, GPU  (AIGS input)
    NvCVImage matteGpu{}; // A    u8 chunky, GPU  (AIGS output)
    NvCVImage matteCpu{}; // A    u8 chunky, CPU  (downloaded)
    NvCVImage stage{};    // BGRA u8 chunky, GPU  (transfer staging; matches the CPU src)
    int  w = 0, h = 0;
    bool loaded = false;
};

Aigs::Aigs() = default;
Aigs::~Aigs() { Stop(); }

bool Aigs::Probe(std::string& detail) {
    std::string binDir, modelDir, err;
    if (!ResolveSdkPaths(binDir, modelDir, err)) { detail = err; return false; }
    PointProxiesAt(binDir);

    // Probe uses the SDK's default CUDA stream (no explicit CudaStreamCreate); sufficient for a load-only check.
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
    AigsImpl* impl = new (std::nothrow) AigsImpl();
    if (!impl) { lastError_ = "out of memory"; return false; }

    std::string binDir, err;
    if (!ResolveSdkPaths(binDir, impl->modelDir, err)) {
        lastError_ = err; delete impl; return false;
    }
    PointProxiesAt(binDir);

    if (NvVFX_CudaStreamCreate(&impl->stream) != NVCV_SUCCESS) {
        lastError_ = "NvVFX_CudaStreamCreate failed"; delete impl; return false;
    }
    if (NvVFX_CreateEffect(NVVFX_FX_GREEN_SCREEN, &impl->effect) != NVCV_SUCCESS || !impl->effect) {
        lastError_ = "NvVFX_CreateEffect failed"; delete impl; return false;
    }
    NvVFX_SetString(impl->effect, NVVFX_MODEL_DIRECTORY, impl->modelDir.c_str());
    NvVFX_SetCudaStream(impl->effect, NVVFX_CUDA_STREAM, impl->stream);
    NvVFX_SetU32(impl->effect, NVVFX_MODE,     0u); // mode 0 = best quality; models m0 confirmed present
    NvVFX_SetU32(impl->effect, NVVFX_TEMPORAL, 1u); // video: reduce matte flicker
    // NvVFX_Load is called after SetImage in Task 3 (requires known input dimensions).
    impl_ = impl;    // assign only on full success
    ready_ = true;   // "configured"; model load completes on first frame (Task 3)
    lastError_.clear();
    return true;
}

void Aigs::Stop() {
    auto* impl = static_cast<AigsImpl*>(impl_);
    if (!impl) { ready_ = false; return; }
    NvCVImage_Dealloc(&impl->srcGpu);
    NvCVImage_Dealloc(&impl->matteGpu);
    NvCVImage_Dealloc(&impl->matteCpu);
    NvCVImage_Dealloc(&impl->stage);
    if (impl->effect) NvVFX_DestroyEffect(impl->effect);
    if (impl->stream) NvVFX_CudaStreamDestroy(impl->stream);
    delete impl;
    impl_ = nullptr;
    ready_ = false;
}

namespace {
// (Re)allocates the GPU/CPU images for a w*h frame and loads the model. Returns
// NVCV_SUCCESS on success. Called when size changes or on first frame.
NvCV_Status EnsureImages(AigsImpl* impl, int w, int h) {
    if (impl->loaded && impl->w == w && impl->h == h) return NVCV_SUCCESS;

    NvCVImage_Dealloc(&impl->srcGpu);
    NvCVImage_Dealloc(&impl->matteGpu);
    NvCVImage_Dealloc(&impl->matteCpu);
    NvCVImage_Dealloc(&impl->stage);

    NvCV_Status s;
    s = NvCVImage_Alloc(&impl->srcGpu,   w, h, NVCV_BGR,  NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->matteGpu, w, h, NVCV_A,    NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->matteCpu, w, h, NVCV_A,    NVCV_U8, NVCV_CHUNKY, NVCV_CPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->stage,    w, h, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;

    NvVFX_SetImage(impl->effect, NVVFX_INPUT_IMAGE,  &impl->srcGpu);
    NvVFX_SetImage(impl->effect, NVVFX_OUTPUT_IMAGE, &impl->matteGpu);
    // Set max input dimensions before Load (required by some SDK versions).
    NvVFX_SetU32(impl->effect, NVVFX_MAX_INPUT_WIDTH,  static_cast<unsigned>(w));
    NvVFX_SetU32(impl->effect, NVVFX_MAX_INPUT_HEIGHT, static_cast<unsigned>(h));
    s = NvVFX_Load(impl->effect); // builds/loads the engine for this input size
    if (s != NVCV_SUCCESS) return s;

    impl->w = w; impl->h = h; impl->loaded = true;
    return NVCV_SUCCESS;
}

// SEAM 1 (upload): CPU BGRA -> GPU BGR, via a GPU staging buffer that matches the CPU src.
NvCV_Status Upload(AigsImpl* impl, uint8_t* bgra, int w, int h) {
    NvCVImage src{};
    NvCVImage_Init(&src, w, h, w * 4, bgra, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
    return NvCVImage_Transfer(&src, &impl->srcGpu, 1.0f, impl->stream, &impl->stage);
}

// SEAM 2 (download): GPU matte -> CPU matte (same format; tmp not needed).
NvCV_Status Download(AigsImpl* impl) {
    return NvCVImage_Transfer(&impl->matteGpu, &impl->matteCpu, 1.0f, impl->stream, nullptr);
}

// SEAM 3 (composite): apply matte to the BGRA buffer in place, premultiplied.
void Composite(AigsImpl* impl, uint8_t* bgra, int w, int h) {
    const uint8_t* m = static_cast<const uint8_t*>(impl->matteCpu.pixels);
    const int mpitch = impl->matteCpu.pitch; // bytes per matte row (>= w)
    for (int y = 0; y < h; ++y) {
        const uint8_t* mrow = m + static_cast<size_t>(mpitch) * y;
        uint8_t* prow = bgra + static_cast<size_t>(w) * 4 * y;
        for (int x = 0; x < w; ++x) {
            const unsigned a = mrow[x];
            uint8_t* px = prow + x * 4;
            px[0] = static_cast<uint8_t>((px[0] * a) / 255); // B
            px[1] = static_cast<uint8_t>((px[1] * a) / 255); // G
            px[2] = static_cast<uint8_t>((px[2] * a) / 255); // R
            px[3] = static_cast<uint8_t>(a);                 // A = matte
        }
    }
}
} // namespace

bool Aigs::ProcessFrame(uint8_t* bgra, int w, int h) {
    auto* impl = static_cast<AigsImpl*>(impl_);
    if (!impl || !impl->effect || !bgra || w <= 0 || h <= 0) return false;

    if (EnsureImages(impl, w, h) != NVCV_SUCCESS) { lastError_ = "EnsureImages/Load failed"; ready_ = false; return false; }
    if (Upload(impl, bgra, w, h) != NVCV_SUCCESS) { lastError_ = "Upload (Transfer) failed"; return false; }
    if (NvVFX_Run(impl->effect, 0) != NVCV_SUCCESS) { lastError_ = "NvVFX_Run failed"; return false; }
    if (Download(impl) != NVCV_SUCCESS) { lastError_ = "Download (Transfer) failed"; return false; }
    if (NvVFX_CudaStreamSynchronize(impl->stream) != NVCV_SUCCESS) { lastError_ = "stream sync failed"; return false; }

    Composite(impl, bgra, w, h);
    return true;
}

#else
// ---- Passthrough stub: built when no SDK is configured. ----
Aigs::Aigs() = default;
Aigs::~Aigs() = default;
bool Aigs::Probe(std::string& detail) { detail = "Maxine SDK not built in"; return false; }
bool Aigs::Start() { lastError_ = "Maxine SDK not built in"; ready_ = false; return false; }
void Aigs::Stop() { ready_ = false; }
bool Aigs::ProcessFrame(uint8_t*, int, int) { return false; }
#endif
