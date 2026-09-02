#include "aigs.h"
#include "gpu_mem.h"

#ifdef COS_HAS_MAXINE
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include "nvCVStatus.h"
#include "nvCVImage.h"
#include "nvVideoEffects.h"
#include "nvVFXGreenScreen.h" // defines NVVFX_FX_GREEN_SCREEN "GreenScreen"
#include "paths.h"
#include "vfx_paths.h"
#include "matte_ops.h"

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
    std::string  gateErr;           // free-VRAM gate reason from EnsureImages (NVCV_ERR_MEMORY)
    std::vector<uint8_t> matteWork; // packed w*h, post-processed matte (pitch == w)
    std::vector<uint8_t> matteTmp;  // packed w*h, separable-pass scratch
};

Aigs::Aigs() = default;
Aigs::~Aigs() { Stop(); }

bool Aigs::Probe(std::string& detail) {
    std::string binDir, modelDir, err;
    if (!vfx::ResolveSdkPaths(binDir, modelDir, err)) { detail = err; return false; }
    vfx::PointProxiesAt(binDir);

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
    // Free-VRAM gate (gpu_mem.h) — NvVFX_Load crashes rather than fails when the card is full.
    // In Auto backend a false here is what hands green screen to the ONNX CPU engine.
    if (std::string why; !gpumem::CanLoad(why)) { lastError_ = why + " and retry."; ready_ = false; return false; }
    AigsImpl* impl = new (std::nothrow) AigsImpl();
    if (!impl) { lastError_ = "out of memory"; return false; }

    std::string binDir, err;
    if (!vfx::ResolveSdkPaths(binDir, impl->modelDir, err)) {
        lastError_ = err; delete impl; return false;
    }
    vfx::PointProxiesAt(binDir);

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

    s = NvVFX_SetImage(impl->effect, NVVFX_INPUT_IMAGE,  &impl->srcGpu);   if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetImage(impl->effect, NVVFX_OUTPUT_IMAGE, &impl->matteGpu); if (s != NVCV_SUCCESS) return s;
    // Set max input dimensions before Load (required by some SDK versions).
    s = NvVFX_SetU32(impl->effect, NVVFX_MAX_INPUT_WIDTH,  static_cast<unsigned>(w)); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetU32(impl->effect, NVVFX_MAX_INPUT_HEIGHT, static_cast<unsigned>(h)); if (s != NVCV_SUCCESS) return s;
    // Re-check right before the load: memory may have moved since Start (this runs on the
    // first frame, and again on a resolution change).
    if (!gpumem::CanLoad(impl->gateErr)) { impl->gateErr += " and retry."; return NVCV_ERR_MEMORY; }
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

// Copy the (possibly padded) CPU matte into a packed w*h buffer.
static void PackMatte(AigsImpl* impl, int w, int h) {
    impl->matteWork.resize(static_cast<size_t>(w) * h);
    impl->matteTmp.resize(static_cast<size_t>(w) * h);
    const uint8_t* m = static_cast<const uint8_t*>(impl->matteCpu.pixels);
    const int mpitch = impl->matteCpu.pitch;
    for (int y = 0; y < h; ++y)
        std::memcpy(impl->matteWork.data() + static_cast<size_t>(w) * y,
                    m + static_cast<size_t>(mpitch) * y, static_cast<size_t>(w));
}

} // namespace

bool Aigs::ProcessFrame(uint8_t* bgra, int w, int h, double expand, double feather) {
    auto* impl = static_cast<AigsImpl*>(impl_);
    if (!impl || !impl->effect || !bgra || w <= 0 || h <= 0) return false;

    if (NvCV_Status es = EnsureImages(impl, w, h); es != NVCV_SUCCESS) {
        lastError_ = (es == NVCV_ERR_MEMORY && !impl->gateErr.empty())
            ? impl->gateErr
            : std::string("EnsureImages/Load failed: ") + NvCV_GetErrorStringFromCode(es);
        ready_ = false; return false;
    }
    if (Upload(impl, bgra, w, h) != NVCV_SUCCESS) { lastError_ = "Upload (Transfer) failed"; return false; }
    if (NvVFX_Run(impl->effect, 0) != NVCV_SUCCESS) { lastError_ = "NvVFX_Run failed"; return false; }
    if (Download(impl) != NVCV_SUCCESS) { lastError_ = "Download (Transfer) failed"; return false; }
    if (NvVFX_CudaStreamSynchronize(impl->stream) != NVCV_SUCCESS) { lastError_ = "stream sync failed"; return false; }

    PackMatte(impl, w, h);
    matte::Dilate(impl->matteWork.data(), impl->matteTmp.data(), w, h,
                  matte::RadiusFromAmount(expand, matte::kMaxDilateRadius));
    matte::Feather(impl->matteWork.data(), impl->matteTmp.data(), w, h,
                   matte::RadiusFromAmount(feather, matte::kMaxBlurRadius));
    matte::CompositePremultiplied(bgra, impl->matteWork.data(), w, h);
    return true;
}

#else
// ---- Passthrough stub: built when no SDK is configured. ----
Aigs::Aigs() = default;
Aigs::~Aigs() = default;
bool Aigs::Probe(std::string& detail) { detail = "Maxine SDK not built in"; return false; }
bool Aigs::Start() { lastError_ = "Maxine SDK not built in"; ready_ = false; return false; }
void Aigs::Stop() { ready_ = false; }
bool Aigs::ProcessFrame(uint8_t*, int, int, double, double) { return false; }
#endif
