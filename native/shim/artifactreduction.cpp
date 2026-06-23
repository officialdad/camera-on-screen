#include "artifactreduction.h"

#ifdef COS_HAS_MAXINE
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <new>
#include <string>
#include "nvCVStatus.h"
#include "nvCVImage.h"
#include "nvVideoEffects.h"
#include "nvVFXArtifactReduction.h" // defines NVVFX_FX_ARTIFACT_REDUCTION "ArtifactReduction"
#include "vfx_paths.h"

struct ArImpl {
    NvVFX_Handle effect = nullptr;
    CUstream     stream = nullptr;
    std::string  modelDir;
    NvCVImage srcGpu{};  // BGR u8 chunky GPU  (input)
    NvCVImage dstGpu{};  // BGR u8 chunky GPU  (output)
    NvCVImage dstCpu{};  // BGR u8 chunky CPU  (downloaded)
    NvCVImage stage{};   // BGRA u8 chunky GPU (transfer staging)
    int w = 0, h = 0;
    bool loaded = false;
};

ArtifactReduction::ArtifactReduction() = default;
ArtifactReduction::~ArtifactReduction() { Stop(); }

bool ArtifactReduction::Probe(std::string& detail) {
    std::string binDir, modelDir, err;
    if (!vfx::ResolveSdkPaths(binDir, modelDir, err)) { detail = err; return false; }
    vfx::PointProxiesAt(binDir);
    NvVFX_Handle eff = nullptr;
    if (NvVFX_CreateEffect(NVVFX_FX_ARTIFACT_REDUCTION, &eff) != NVCV_SUCCESS || !eff) {
        detail = "NvVFX_CreateEffect(ArtifactReduction) failed (DLL/SDK load?)"; return false;
    }
    NvVFX_SetString(eff, NVVFX_MODEL_DIRECTORY, modelDir.c_str());
    NvVFX_SetU32(eff, NVVFX_MODE, 0u);
    NvVFX_SetU32(eff, NVVFX_MAX_INPUT_WIDTH,  1920u);
    NvVFX_SetU32(eff, NVVFX_MAX_INPUT_HEIGHT, 1080u);
    NvCV_Status load = NvVFX_Load(eff);
    NvVFX_DestroyEffect(eff);
    if (load != NVCV_SUCCESS) { detail = "NvVFX_Load(ArtifactReduction) failed (models missing?)"; return false; }
    detail = "ArtifactReduction available";
    return true;
}

bool ArtifactReduction::Start() {
    Stop();
    ArImpl* impl = new (std::nothrow) ArImpl();
    if (!impl) { lastError_ = "out of memory"; return false; }
    std::string binDir, err;
    if (!vfx::ResolveSdkPaths(binDir, impl->modelDir, err)) { lastError_ = err; delete impl; return false; }
    vfx::PointProxiesAt(binDir);
    if (NvVFX_CudaStreamCreate(&impl->stream) != NVCV_SUCCESS) { lastError_ = "CudaStreamCreate failed"; delete impl; return false; }
    if (NvVFX_CreateEffect(NVVFX_FX_ARTIFACT_REDUCTION, &impl->effect) != NVCV_SUCCESS || !impl->effect) {
        lastError_ = "CreateEffect failed"; delete impl; return false;
    }
    NvVFX_SetString(impl->effect, NVVFX_MODEL_DIRECTORY, impl->modelDir.c_str());
    NvVFX_SetCudaStream(impl->effect, NVVFX_CUDA_STREAM, impl->stream);
    NvVFX_SetU32(impl->effect, NVVFX_MODE, 1u); // mode 1 = stronger / for compressed input
    impl_ = impl; ready_ = true; lastError_.clear();
    return true;
}

void ArtifactReduction::Stop() {
    auto* impl = static_cast<ArImpl*>(impl_);
    if (!impl) { ready_ = false; return; }
    NvCVImage_Dealloc(&impl->srcGpu);
    NvCVImage_Dealloc(&impl->dstGpu);
    NvCVImage_Dealloc(&impl->dstCpu);
    NvCVImage_Dealloc(&impl->stage);
    if (impl->effect) NvVFX_DestroyEffect(impl->effect);
    if (impl->stream) NvVFX_CudaStreamDestroy(impl->stream);
    delete impl; impl_ = nullptr; ready_ = false;
}

namespace {
NvCV_Status EnsureImages(ArImpl* impl, int w, int h) {
    if (impl->loaded && impl->w == w && impl->h == h) return NVCV_SUCCESS;
    NvCVImage_Dealloc(&impl->srcGpu);
    NvCVImage_Dealloc(&impl->dstGpu);
    NvCVImage_Dealloc(&impl->dstCpu);
    NvCVImage_Dealloc(&impl->stage);
    NvCV_Status s;
    s = NvCVImage_Alloc(&impl->srcGpu, w, h, NVCV_BGR,  NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->dstGpu, w, h, NVCV_BGR,  NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->dstCpu, w, h, NVCV_BGR,  NVCV_U8, NVCV_CHUNKY, NVCV_CPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->stage,  w, h, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetImage(impl->effect, NVVFX_INPUT_IMAGE,  &impl->srcGpu); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetImage(impl->effect, NVVFX_OUTPUT_IMAGE, &impl->dstGpu); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetU32(impl->effect, NVVFX_MAX_INPUT_WIDTH,  static_cast<unsigned>(w)); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetU32(impl->effect, NVVFX_MAX_INPUT_HEIGHT, static_cast<unsigned>(h)); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_Load(impl->effect); if (s != NVCV_SUCCESS) return s;
    impl->w = w; impl->h = h; impl->loaded = true;
    return NVCV_SUCCESS;
}
} // namespace

bool ArtifactReduction::ProcessFrame(uint8_t* bgra, int w, int h) {
    auto* impl = static_cast<ArImpl*>(impl_);
    if (!impl || !impl->effect || !bgra || w <= 0 || h <= 0) return false;
    if (NvCV_Status es = EnsureImages(impl, w, h); es != NVCV_SUCCESS) {
        lastError_ = std::string("EnsureImages/Load failed: ") + NvCV_GetErrorStringFromCode(es); ready_ = false; return false;
    }
    NvCVImage src{};
    NvCVImage_Init(&src, w, h, w * 4, bgra, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
    if (NvCVImage_Transfer(&src, &impl->srcGpu, 1.0f, impl->stream, &impl->stage) != NVCV_SUCCESS) { lastError_ = "upload failed"; return false; }
    if (NvVFX_Run(impl->effect, 0) != NVCV_SUCCESS) { lastError_ = "NvVFX_Run failed"; return false; }
    if (NvCVImage_Transfer(&impl->dstGpu, &impl->dstCpu, 1.0f, impl->stream, nullptr) != NVCV_SUCCESS) { lastError_ = "download failed"; return false; }
    if (NvVFX_CudaStreamSynchronize(impl->stream) != NVCV_SUCCESS) { lastError_ = "stream sync failed"; return false; }
    // Write cleaned BGR back over the BGRA buffer; leave alpha untouched (forced 0xFF upstream).
    const uint8_t* d = static_cast<const uint8_t*>(impl->dstCpu.pixels);
    const int dpitch = impl->dstCpu.pitch;
    for (int y = 0; y < h; ++y) {
        const uint8_t* drow = d + static_cast<size_t>(dpitch) * y;
        uint8_t* prow = bgra + static_cast<size_t>(w) * 4 * y;
        for (int x = 0; x < w; ++x) {
            prow[x * 4 + 0] = drow[x * 3 + 0];
            prow[x * 4 + 1] = drow[x * 3 + 1];
            prow[x * 4 + 2] = drow[x * 3 + 2];
        }
    }
    return true;
}

#else
// ---- Passthrough stub: built when no SDK is configured. ----
ArtifactReduction::ArtifactReduction() = default;
ArtifactReduction::~ArtifactReduction() = default;
bool ArtifactReduction::Probe(std::string& detail) { detail = "Maxine SDK not built in"; return false; }
bool ArtifactReduction::Start() { lastError_ = "Maxine SDK not built in"; ready_ = false; return false; }
void ArtifactReduction::Stop() { ready_ = false; }
bool ArtifactReduction::ProcessFrame(uint8_t*, int, int) { return false; }
#endif
