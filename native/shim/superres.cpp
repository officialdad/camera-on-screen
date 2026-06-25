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
#include "nvVFXVideoSuperRes.h"  // real header: NVVFX_FX_VIDEO_SUPER_RES + NVVFX_QUALITY_LEVEL
#include "vfx_paths.h"

struct SrImpl {
    NvVFX_Handle effect = nullptr;
    CUstream     stream = nullptr;
    std::string  modelDir;
    NvCVImage srcGpu{};  // BGRA u8 chunky GPU (input,  w x h)
    NvCVImage dstGpu{};  // BGRA u8 chunky GPU (output, ow x oh)
    NvCVImage dstCpu{};  // BGRA u8 chunky CPU (downloaded)
    NvCVImage stage{};   // BGRA u8 chunky GPU (upload staging, w x h)
    int w = 0, h = 0, ow = 0, oh = 0;
    bool loaded = false;
};

// QualityLevel 1-4 are the upscale modes; 8-15 (denoise/deblur) keep input size.
static bool IsUpscale(int q) { return q >= 1 && q <= 4; }

static void OutDims(int w, int h, int q, int scaleX10, int& ow, int& oh) {
    if (IsUpscale(q)) { ow = (w * scaleX10) / 10; oh = (h * scaleX10) / 10; }
    else              { ow = w; oh = h; }  // denoise/deblur do not upscale (out == in)
}

SuperRes::SuperRes() = default;
SuperRes::~SuperRes() { Stop(); }

// Probe mirrors the verified Start sequence (create + quality + images + load) so a load-only
// capability check matches the runtime path. False here greys the effect off for all users.
bool SuperRes::Probe(std::string& detail) {
    std::string binDir, modelDir, err;
    if (!vfx::ResolveSdkPaths(binDir, modelDir, err)) { detail = err; return false; }
    vfx::PointProxiesAt(binDir);
    NvVFX_Handle eff = nullptr;
    if (NvVFX_CreateEffect(NVVFX_FX_VIDEO_SUPER_RES, &eff) != NVCV_SUCCESS || !eff) {
        detail = "NvVFX_CreateEffect(VideoSuperRes) failed (DLL/SDK load?)"; return false;
    }
    CUstream stream = nullptr;
    NvVFX_CudaStreamCreate(&stream);
    NvVFX_SetString(eff, NVVFX_MODEL_DIRECTORY, modelDir.c_str());  // harmless for NGX
    NvVFX_SetCudaStream(eff, NVVFX_CUDA_STREAM, stream);
    NvVFX_SetU32(eff, NVVFX_QUALITY_LEVEL, 1u);  // VSR_Low (upscale); same family Start uses
    NvCVImage src{}, dst{};
    NvCV_Status load = NVCV_ERR_GENERAL;
    if (NvCVImage_Alloc(&src, 640, 360,  NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1) == NVCV_SUCCESS &&
        NvCVImage_Alloc(&dst, 1280, 720, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1) == NVCV_SUCCESS) {
        NvVFX_SetImage(eff, NVVFX_INPUT_IMAGE,  &src);
        NvVFX_SetImage(eff, NVVFX_OUTPUT_IMAGE, &dst);
        load = NvVFX_Load(eff);
    }
    NvCVImage_Dealloc(&src); NvCVImage_Dealloc(&dst);
    NvVFX_DestroyEffect(eff);
    if (stream) NvVFX_CudaStreamDestroy(stream);
    if (load != NVCV_SUCCESS) { detail = "NvVFX_Load(VideoSuperRes) failed (models missing or GPU incompatible?)"; return false; }
    detail = "VideoSuperRes available";
    return true;
}

bool SuperRes::Start(int qualityLevel, int scaleX10) {
    Stop();
    qualityLevel_ = qualityLevel;
    scaleX10_ = (scaleX10 == 15) ? 15 : 20;  // only 1.5x / 2x supported (upscale modes)
    SrImpl* impl = new (std::nothrow) SrImpl();
    if (!impl) { lastError_ = "out of memory"; return false; }
    std::string binDir, err;
    if (!vfx::ResolveSdkPaths(binDir, impl->modelDir, err)) { lastError_ = err; delete impl; return false; }
    vfx::PointProxiesAt(binDir);
    if (NvVFX_CudaStreamCreate(&impl->stream) != NVCV_SUCCESS) { lastError_ = "CudaStreamCreate failed"; delete impl; return false; }
    if (NvVFX_CreateEffect(NVVFX_FX_VIDEO_SUPER_RES, &impl->effect) != NVCV_SUCCESS || !impl->effect) {
        lastError_ = "CreateEffect failed"; NvVFX_CudaStreamDestroy(impl->stream); delete impl; return false;
    }
    NvVFX_SetString(impl->effect, NVVFX_MODEL_DIRECTORY, impl->modelDir.c_str());
    NvVFX_SetCudaStream(impl->effect, NVVFX_CUDA_STREAM, impl->stream);
    NvVFX_SetU32(impl->effect, NVVFX_QUALITY_LEVEL, static_cast<unsigned>(qualityLevel_));
    // Scale is inferred by the effect from the output/input dimension ratio; no scale param.
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
// (Re)allocates GPU/CPU images for input w*h and output ow*oh, then loads the model. Scale is
// inferred by the effect from dstGpu.width/srcGpu.width. Called on first frame and on size change.
NvCV_Status EnsureImages(SrImpl* impl, int w, int h, int ow, int oh) {
    if (impl->loaded && impl->w == w && impl->h == h && impl->ow == ow && impl->oh == oh) return NVCV_SUCCESS;
    NvCVImage_Dealloc(&impl->srcGpu);
    NvCVImage_Dealloc(&impl->dstGpu);
    NvCVImage_Dealloc(&impl->dstCpu);
    NvCVImage_Dealloc(&impl->stage);
    NvCV_Status s;
    s = NvCVImage_Alloc(&impl->srcGpu, w,  h,  NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->dstGpu, ow, oh, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->dstCpu, ow, oh, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->stage,  w,  h,  NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetImage(impl->effect, NVVFX_INPUT_IMAGE,  &impl->srcGpu); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetImage(impl->effect, NVVFX_OUTPUT_IMAGE, &impl->dstGpu); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_Load(impl->effect); if (s != NVCV_SUCCESS) return s;
    impl->w = w; impl->h = h; impl->ow = ow; impl->oh = oh; impl->loaded = true;
    return NVCV_SUCCESS;
}
} // namespace

bool SuperRes::ProcessFrame(const uint8_t* bgra, int w, int h, std::vector<uint8_t>& out, int& outW, int& outH) {
    auto* impl = static_cast<SrImpl*>(impl_);
    if (!impl || !impl->effect || !bgra || w <= 0 || h <= 0) return false;
    int ow, oh; OutDims(w, h, qualityLevel_, scaleX10_, ow, oh);
    if (NvCV_Status es = EnsureImages(impl, w, h, ow, oh); es != NVCV_SUCCESS) {
        lastError_ = std::string("EnsureImages/Load failed: ") + NvCV_GetErrorStringFromCode(es); ready_ = false; return false;
    }
    // Upload: CPU BGRA -> GPU BGRA via staging buffer (same format, no conversion).
    NvCVImage src{};
    NvCVImage_Init(&src, w, h, w * 4, const_cast<uint8_t*>(bgra), NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
    if (NvCVImage_Transfer(&src, &impl->srcGpu, 1.0f, impl->stream, &impl->stage) != NVCV_SUCCESS) { lastError_ = "upload failed"; return false; }
    if (NvVFX_Run(impl->effect, 0) != NVCV_SUCCESS) { lastError_ = "NvVFX_Run failed"; return false; }
    // Download: GPU BGRA (ow x oh) -> CPU BGRA.
    if (NvCVImage_Transfer(&impl->dstGpu, &impl->dstCpu, 1.0f, impl->stream, nullptr) != NVCV_SUCCESS) { lastError_ = "download failed"; return false; }
    if (NvVFX_CudaStreamSynchronize(impl->stream) != NVCV_SUCCESS) { lastError_ = "stream sync failed"; return false; }
    // Write-back: BGRA CPU -> packed BGRA out, honoring source pitch. Force alpha opaque: VSR
    // does not preserve the alpha channel, and SR now runs on a pre-matte frame (green screen
    // authors the real matte AFTER SR). Opaque matches CopyFrame's passthrough invariant.
    out.resize(static_cast<size_t>(ow) * oh * 4);
    const uint8_t* d = static_cast<const uint8_t*>(impl->dstCpu.pixels);
    const int dpitch = impl->dstCpu.pitch;
    const int rowBytes = ow * 4;
    for (int y = 0; y < oh; ++y) {
        uint8_t* orow = out.data() + static_cast<size_t>(rowBytes) * y;
        std::memcpy(orow, d + static_cast<size_t>(dpitch) * y, static_cast<size_t>(rowBytes));
        for (int x = 3; x < rowBytes; x += 4) orow[x] = 0xFF;
    }
    outW = ow; outH = oh;
    return true;
}

#else
// ---- Passthrough stub: built when no SDK is configured. ----
SuperRes::SuperRes() = default;
SuperRes::~SuperRes() = default;
bool SuperRes::Probe(std::string& detail) { detail = "Maxine SDK not built in"; return false; }
bool SuperRes::Start(int, int) { lastError_ = "Maxine SDK not built in"; ready_ = false; return false; }
void SuperRes::Stop() { ready_ = false; }
bool SuperRes::ProcessFrame(const uint8_t*, int, int, std::vector<uint8_t>&, int&, int&) { return false; }
#endif
