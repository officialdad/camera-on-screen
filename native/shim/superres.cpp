#include "superres.h"

#ifdef COS_HAS_MAXINE
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <new>
#include <string>
#include "nvCVStatus.h"
#include "nvCVImage.h"
#include "nvVideoEffects.h"
#include "vfx_paths.h"

// VFX 1.2.0.0 ships the super-res selector only in its per-feature header (NGC feature
// nvvfxvideosuperres). Define the documented literal if absent. NVIDIA VideoFX selector
// "SuperRes" confirmed by grepping the VFX 1.2.0.0 SDK doc tree.
#ifndef NVVFX_FX_SUPER_RES
#define NVVFX_FX_SUPER_RES "SuperRes"
#endif

struct SrImpl {
    NvVFX_Handle effect = nullptr;
    CUstream     stream = nullptr;
    std::string  modelDir;
    NvCVImage srcGpu{};  // BGR u8 chunky GPU (input, w x h)
    NvCVImage dstGpu{};  // BGR u8 chunky GPU (output, outW x outH)
    NvCVImage dstCpu{};  // BGR u8 chunky CPU (downloaded)
    NvCVImage stage{};   // BGRA u8 chunky GPU (upload staging, w x h)
    int w = 0, h = 0, ow = 0, oh = 0;
    bool loaded = false;
};

static void ScaledDims(int w, int h, int scaleX10, int& ow, int& oh) {
    ow = (w * scaleX10) / 10;
    oh = (h * scaleX10) / 10;
}

SuperRes::SuperRes() = default;
SuperRes::~SuperRes() { Stop(); }

// Probe loads the effect using mode 1 (VSR_Low), matching the mode Start() uses.
// This avoids a false-positive probe where mode 0 (bicubic) succeeds but mode 1 fails.
bool SuperRes::Probe(std::string& detail) {
    std::string binDir, modelDir, err;
    if (!vfx::ResolveSdkPaths(binDir, modelDir, err)) { detail = err; return false; }
    vfx::PointProxiesAt(binDir);
    NvVFX_Handle eff = nullptr;
    if (NvVFX_CreateEffect(NVVFX_FX_SUPER_RES, &eff) != NVCV_SUCCESS || !eff) {
        detail = "NvVFX_CreateEffect(SuperRes) failed (DLL/SDK load?)"; return false;
    }
    NvVFX_SetString(eff, NVVFX_MODEL_DIRECTORY, modelDir.c_str());
    NvVFX_SetU32(eff, NVVFX_MODE, 1u);  // VSR_Low; same mode as Start() to avoid false-positive
    NvVFX_SetU32(eff, NVVFX_MAX_INPUT_WIDTH,  1920u);
    NvVFX_SetU32(eff, NVVFX_MAX_INPUT_HEIGHT, 1080u);
    NvCV_Status load = NvVFX_Load(eff);
    NvVFX_DestroyEffect(eff);
    if (load != NVCV_SUCCESS) { detail = "NvVFX_Load(SuperRes) failed (models missing?)"; return false; }
    detail = "SuperRes available";
    return true;
}

bool SuperRes::Start(int scaleX10) {
    Stop();
    scaleX10_ = (scaleX10 == 15) ? 15 : 20; // only 1.5x / 2x supported here
    SrImpl* impl = new (std::nothrow) SrImpl();
    if (!impl) { lastError_ = "out of memory"; return false; }
    std::string binDir, err;
    if (!vfx::ResolveSdkPaths(binDir, impl->modelDir, err)) { lastError_ = err; delete impl; return false; }
    vfx::PointProxiesAt(binDir);
    if (NvVFX_CudaStreamCreate(&impl->stream) != NVCV_SUCCESS) { lastError_ = "CudaStreamCreate failed"; delete impl; return false; }
    if (NvVFX_CreateEffect(NVVFX_FX_SUPER_RES, &impl->effect) != NVCV_SUCCESS || !impl->effect) {
        lastError_ = "CreateEffect failed"; NvVFX_CudaStreamDestroy(impl->stream); delete impl; return false;
    }
    NvVFX_SetString(impl->effect, NVVFX_MODEL_DIRECTORY, impl->modelDir.c_str());
    NvVFX_SetCudaStream(impl->effect, NVVFX_CUDA_STREAM, impl->stream);
    NvVFX_SetU32(impl->effect, NVVFX_MODE, 1u);  // VSR_Low: fast, good quality for live webcam
    // Scale is inferred by the effect from the ratio of output to input image dimensions;
    // no explicit NvVFX_SetF32(NVVFX_SCALE, ...) call is required.
    impl_ = impl; ready_ = true; lastError_.clear();
    return true;
}

void SuperRes::Stop() {
    auto* impl = static_cast<SrImpl*>(impl_);
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
// (Re)allocates GPU/CPU images for input w*h and output ow*oh, then loads the model.
// Scale is inferred by the effect from the ratio dstGpu.width/srcGpu.width.
// Returns NVCV_SUCCESS on success; called on first frame and on resolution change.
NvCV_Status EnsureImages(SrImpl* impl, int w, int h, int ow, int oh) {
    if (impl->loaded && impl->w == w && impl->h == h && impl->ow == ow && impl->oh == oh) return NVCV_SUCCESS;
    NvCVImage_Dealloc(&impl->srcGpu);
    NvCVImage_Dealloc(&impl->dstGpu);
    NvCVImage_Dealloc(&impl->dstCpu);
    NvCVImage_Dealloc(&impl->stage);
    NvCV_Status s;
    s = NvCVImage_Alloc(&impl->srcGpu, w,  h,  NVCV_BGR,  NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->dstGpu, ow, oh, NVCV_BGR,  NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->dstCpu, ow, oh, NVCV_BGR,  NVCV_U8, NVCV_CHUNKY, NVCV_CPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->stage,  w,  h,  NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetImage(impl->effect, NVVFX_INPUT_IMAGE,  &impl->srcGpu); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetImage(impl->effect, NVVFX_OUTPUT_IMAGE, &impl->dstGpu); if (s != NVCV_SUCCESS) return s;
    // Set max input dimensions to pre-allocate internal buffers and enable load.
    s = NvVFX_SetU32(impl->effect, NVVFX_MAX_INPUT_WIDTH,  static_cast<unsigned>(w)); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetU32(impl->effect, NVVFX_MAX_INPUT_HEIGHT, static_cast<unsigned>(h)); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_Load(impl->effect); if (s != NVCV_SUCCESS) return s;
    impl->w = w; impl->h = h; impl->ow = ow; impl->oh = oh; impl->loaded = true;
    return NVCV_SUCCESS;
}
} // namespace

bool SuperRes::ProcessFrame(const uint8_t* bgra, int w, int h, std::vector<uint8_t>& out, int& outW, int& outH) {
    auto* impl = static_cast<SrImpl*>(impl_);
    if (!impl || !impl->effect || !bgra || w <= 0 || h <= 0) return false;
    int ow, oh; ScaledDims(w, h, scaleX10_, ow, oh);
    if (NvCV_Status es = EnsureImages(impl, w, h, ow, oh); es != NVCV_SUCCESS) {
        lastError_ = std::string("EnsureImages/Load failed: ") + NvCV_GetErrorStringFromCode(es); ready_ = false; return false;
    }
    // Upload: CPU BGRA -> GPU BGR via staging buffer (NvCVImage_Transfer handles format conversion).
    NvCVImage src{};
    NvCVImage_Init(&src, w, h, w * 4, const_cast<uint8_t*>(bgra), NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
    if (NvCVImage_Transfer(&src, &impl->srcGpu, 1.0f, impl->stream, &impl->stage) != NVCV_SUCCESS) { lastError_ = "upload failed"; return false; }
    if (NvVFX_Run(impl->effect, 0) != NVCV_SUCCESS) { lastError_ = "NvVFX_Run failed"; return false; }
    // Download: GPU BGR (ow x oh) -> CPU BGR.
    if (NvCVImage_Transfer(&impl->dstGpu, &impl->dstCpu, 1.0f, impl->stream, nullptr) != NVCV_SUCCESS) { lastError_ = "download failed"; return false; }
    if (NvVFX_CudaStreamSynchronize(impl->stream) != NVCV_SUCCESS) { lastError_ = "stream sync failed"; return false; }
    // Write-back: BGR CPU -> BGRA output (alpha = 0xFF; fresh upscaled buffer, opaque).
    out.assign(static_cast<size_t>(ow) * oh * 4, 0xFF);
    const uint8_t* d = static_cast<const uint8_t*>(impl->dstCpu.pixels);
    const int dpitch = impl->dstCpu.pitch;
    for (int y = 0; y < oh; ++y) {
        const uint8_t* drow = d + static_cast<size_t>(dpitch) * y;
        uint8_t* prow = out.data() + static_cast<size_t>(ow) * 4 * y;
        for (int x = 0; x < ow; ++x) {
            prow[x * 4 + 0] = drow[x * 3 + 0]; // B
            prow[x * 4 + 1] = drow[x * 3 + 1]; // G
            prow[x * 4 + 2] = drow[x * 3 + 2]; // R
            // Alpha already 0xFF from the out.assign() pre-fill (opaque upscaled buffer).
        }
    }
    outW = ow; outH = oh;
    return true;
}

#else
// ---- Passthrough stub: built when no SDK is configured. ----
SuperRes::SuperRes() = default;
SuperRes::~SuperRes() = default;
bool SuperRes::Probe(std::string& detail) { detail = "Maxine SDK not built in"; return false; }
bool SuperRes::Start(int) { lastError_ = "Maxine SDK not built in"; ready_ = false; return false; }
void SuperRes::Stop() { ready_ = false; }
bool SuperRes::ProcessFrame(const uint8_t*, int, int, std::vector<uint8_t>&, int&, int&) { return false; }
#endif
