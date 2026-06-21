#include "eyecontact.h"

#ifdef COS_HAS_MAXINE_AR
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include "nvCVStatus.h"
#include "nvCVImage.h"
#include "nvAR.h"
#include "nvAR_defs.h"

// The AR proxy stub (nvARProxy.cpp) declares this extern; define it here. When non-null it
// is SetDllDirectory'd before LoadLibrary(nvARPose.dll). When null the proxy auto-falls back
// to "%ProgramFiles%\NVIDIA Corporation\NVIDIA AR SDK\".
char* g_nvARSDKPath = nullptr;

namespace {
// Resolves the AR runtime root (holds nvARPose.dll) and its models dir. Prefers
// COS_AR_RUNTIME_DIR; otherwise defaults to the Program Files install.
bool ResolveArPaths(std::string& runtimeDir, std::string& modelDir, std::string& err) {
    char buf[1024] = {0};
    DWORD n = GetEnvironmentVariableA("COS_AR_RUNTIME_DIR", buf, sizeof(buf));
    std::string root;
    if (n > 0 && n < sizeof(buf)) {
        root.assign(buf, n);
    } else {
        char pf[1024] = {0};
        DWORD m = GetEnvironmentVariableA("ProgramFiles", pf, sizeof(pf));
        if (m == 0 || m >= sizeof(pf)) { err = "ProgramFiles not set"; return false; }
        root.assign(pf, m);
        root += "\\NVIDIA Corporation\\NVIDIA AR SDK";
    }
    if (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
    runtimeDir = root;
    modelDir   = root + "\\models";
    return true;
}

// Points the AR proxy at the runtime root so nvARPose.dll + deps are found. Idempotent;
// s_root must outlive every NvAR call.
void PointProxyAt(const std::string& runtimeDir) {
    static std::string s_root;
    s_root = runtimeDir;
    g_nvARSDKPath = const_cast<char*>(s_root.c_str());
}

constexpr unsigned kNumLandmarks = 126;       // GazeEngine LANDMARKS_INFO[1].numPoints
constexpr unsigned kNumGazeOutLandmarks = 12; // GazeEngine num_output_landmarks
constexpr unsigned kEyeSizeSensitivity = 3;   // GazeEngine default
} // namespace

// Real per-effect state, hidden behind the opaque impl_ pointer.
struct EyeContactImpl {
    NvAR_FeatureHandle handle = nullptr;
    CUstream stream = nullptr;
    std::string modelDir;
    NvCVImage inGpu{};   // BGR u8 chunky, GPU  (gaze input)
    NvCVImage outGpu{};  // BGR u8 chunky, GPU  (gaze redirected output)
    NvCVImage tmp{};     // staging for NvCVImage_Transfer (managed by the SDK)
    int w = 0, h = 0;
    bool bound = false;
    // Output buffers the SDK requires bound even though we ignore them.
    std::vector<NvAR_Point2f> landmarks;
    std::vector<NvAR_Point2f> gazeOutLandmarks;
    std::vector<float> landmarksConfidence;
    float gazeVector[2] = {0.f};
    float headTranslation[3] = {0.f};
    NvAR_Point3f gazeDirection[2] = {{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}};
    NvAR_Quaternion headPose{};
    std::vector<NvAR_Rect> bboxData;
    NvAR_BBoxes bboxes{};
};

EyeContact::EyeContact() = default;
EyeContact::~EyeContact() { Stop(); }

bool EyeContact::Probe(std::string& detail) {
    std::string runtimeDir, modelDir, err;
    if (!ResolveArPaths(runtimeDir, modelDir, err)) { detail = err; return false; }
    PointProxyAt(runtimeDir);

    CUstream stream = nullptr;
    if (NvAR_CudaStreamCreate(&stream) != NVCV_SUCCESS) {
        detail = "NvAR_CudaStreamCreate failed (CUDA/driver?)";
        return false;
    }
    NvAR_FeatureHandle h = nullptr;
    if (NvAR_Create(NvAR_Feature_GazeRedirection, &h) != NVCV_SUCCESS || !h) {
        NvAR_CudaStreamDestroy(stream);
        detail = "NvAR_Create(GazeRedirection) failed (DLL/SDK load?)";
        return false;
    }
    NvAR_SetString(h, NvAR_Parameter_Config(ModelDir), modelDir.c_str());
    NvAR_SetU32(h, NvAR_Parameter_Config(Landmarks_Size), kNumLandmarks);
    NvAR_SetU32(h, NvAR_Parameter_Config(Temporal), 0xFFFFFFFFu);
    NvAR_SetU32(h, NvAR_Parameter_Config(GazeRedirect), 1u);
    NvAR_SetCudaStream(h, NvAR_Parameter_Config(CUDAStream), stream);
    NvAR_SetU32(h, NvAR_Parameter_Config(EyeSizeSensitivity), kEyeSizeSensitivity);
    NvCV_Status load = NvAR_Load(h);
    NvAR_Destroy(h);
    NvAR_CudaStreamDestroy(stream);
    if (load != NVCV_SUCCESS) {
        detail = "NvAR_Load(GazeRedirection) failed (models missing or GPU incompatible?)";
        return false;
    }
    detail = "GazeRedirection available";
    return true;
}

bool EyeContact::Start() {
    Stop();
    EyeContactImpl* impl = new (std::nothrow) EyeContactImpl();
    if (!impl) { lastError_ = "out of memory"; return false; }

    std::string runtimeDir, err;
    if (!ResolveArPaths(runtimeDir, impl->modelDir, err)) { lastError_ = err; delete impl; return false; }
    PointProxyAt(runtimeDir);

    if (NvAR_CudaStreamCreate(&impl->stream) != NVCV_SUCCESS) {
        lastError_ = "NvAR_CudaStreamCreate failed"; delete impl; return false;
    }
    if (NvAR_Create(NvAR_Feature_GazeRedirection, &impl->handle) != NVCV_SUCCESS || !impl->handle) {
        lastError_ = "NvAR_Create failed"; delete impl; return false;
    }
    NvAR_SetString(impl->handle, NvAR_Parameter_Config(ModelDir), impl->modelDir.c_str());
    NvAR_SetU32(impl->handle, NvAR_Parameter_Config(Landmarks_Size), kNumLandmarks);
    NvAR_SetU32(impl->handle, NvAR_Parameter_Config(Temporal), 0xFFFFFFFFu);
    NvAR_SetU32(impl->handle, NvAR_Parameter_Config(GazeRedirect), 1u);
    NvAR_SetCudaStream(impl->handle, NvAR_Parameter_Config(CUDAStream), impl->stream);
    NvAR_SetU32(impl->handle, NvAR_Parameter_Config(EyeSizeSensitivity), kEyeSizeSensitivity);
    if (NvAR_Load(impl->handle) != NVCV_SUCCESS) {
        lastError_ = "NvAR_Load failed";
        NvAR_Destroy(impl->handle);
        NvAR_CudaStreamDestroy(impl->stream);
        delete impl; return false;
    }
    impl_ = impl;
    ready_ = true;
    lastError_.clear();
    return true;
}

void EyeContact::Stop() {
    auto* impl = static_cast<EyeContactImpl*>(impl_);
    if (!impl) { ready_ = false; return; }
    NvCVImage_Dealloc(&impl->inGpu);
    NvCVImage_Dealloc(&impl->outGpu);
    NvCVImage_Dealloc(&impl->tmp);
    if (impl->handle) NvAR_Destroy(impl->handle);
    if (impl->stream) NvAR_CudaStreamDestroy(impl->stream);
    delete impl;
    impl_ = nullptr;
    ready_ = false;
}

namespace {
// (Re)allocates GPU images and binds all input/output params for a w*h frame. Returns
// NVCV_SUCCESS on success. Called on first frame or when the size changes. The SDK requires
// every output below to be bound even though the shim consumes only the output image.
NvCV_Status BindIO(EyeContactImpl* impl, int w, int h) {
    if (impl->bound && impl->w == w && impl->h == h) return NVCV_SUCCESS;

    NvCVImage_Dealloc(&impl->inGpu);
    NvCVImage_Dealloc(&impl->outGpu);

    NvCV_Status s;
    s = NvCVImage_Alloc(&impl->inGpu,  w, h, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->outGpu, w, h, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;

    s = NvAR_SetObject(impl->handle, NvAR_Parameter_Input(Image),  &impl->inGpu,  (unsigned long)sizeof(NvCVImage)); if (s != NVCV_SUCCESS) return s;
    s = NvAR_SetObject(impl->handle, NvAR_Parameter_Output(Image), &impl->outGpu, (unsigned long)sizeof(NvCVImage)); if (s != NVCV_SUCCESS) return s;
    s = NvAR_SetS32(impl->handle, NvAR_Parameter_Input(Width),  w); if (s != NVCV_SUCCESS) return s;
    s = NvAR_SetS32(impl->handle, NvAR_Parameter_Input(Height), h); if (s != NVCV_SUCCESS) return s;

    unsigned kpts = kNumLandmarks;
    NvAR_GetU32(impl->handle, NvAR_Parameter_Config(Landmarks_Size), &kpts);

    impl->landmarks.assign(kpts, {0.f, 0.f});
    s = NvAR_SetObject(impl->handle, NvAR_Parameter_Output(Landmarks), impl->landmarks.data(), (unsigned long)sizeof(NvAR_Point2f)); if (s != NVCV_SUCCESS) return s;

    impl->gazeOutLandmarks.assign(kNumGazeOutLandmarks, {0.f, 0.f});
    s = NvAR_SetObject(impl->handle, NvAR_Parameter_Output(GazeOutputLandmarks), impl->gazeOutLandmarks.data(), (unsigned long)sizeof(NvAR_Point2f)); if (s != NVCV_SUCCESS) return s;

    impl->landmarksConfidence.assign(kpts, 0.f);
    s = NvAR_SetF32Array(impl->handle, NvAR_Parameter_Output(LandmarksConfidence), impl->landmarksConfidence.data(), kpts); if (s != NVCV_SUCCESS) return s;

    s = NvAR_SetF32Array(impl->handle, NvAR_Parameter_Output(OutputGazeVector), impl->gazeVector, 2); if (s != NVCV_SUCCESS) return s;
    s = NvAR_SetF32Array(impl->handle, NvAR_Parameter_Output(OutputHeadTranslation), impl->headTranslation, 3); if (s != NVCV_SUCCESS) return s;
    s = NvAR_SetObject(impl->handle, NvAR_Parameter_Output(HeadPose), &impl->headPose, (unsigned long)sizeof(NvAR_Quaternion)); if (s != NVCV_SUCCESS) return s;
    s = NvAR_SetObject(impl->handle, NvAR_Parameter_Output(GazeDirection), &impl->gazeDirection, (unsigned long)sizeof(NvAR_Point3f)); if (s != NVCV_SUCCESS) return s;

    impl->bboxData.assign(1, {0.f, 0.f, 0.f, 0.f});
    impl->bboxes.boxes = impl->bboxData.data();
    impl->bboxes.max_boxes = 1;
    impl->bboxes.num_boxes = 1;
    s = NvAR_SetObject(impl->handle, NvAR_Parameter_Output(BoundingBoxes), &impl->bboxes, (unsigned long)sizeof(NvAR_BBoxes)); if (s != NVCV_SUCCESS) return s;

    impl->w = w; impl->h = h; impl->bound = true;
    return NVCV_SUCCESS;
}

// SEAM 1 (upload): CPU BGRA -> GPU BGR, via the SDK-managed tmp staging buffer.
NvCV_Status Upload(EyeContactImpl* impl, uint8_t* bgra, int w, int h) {
    NvCVImage src{};
    NvCVImage_Init(&src, w, h, w * 4, bgra, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
    return NvCVImage_Transfer(&src, &impl->inGpu, 1.0f, impl->stream, &impl->tmp);
}

// SEAM 3 (download/composite): GPU BGR redirected output -> CPU BGRA in place. The BGR->BGRA
// transfer sets alpha; force it opaque afterward so passthrough alpha semantics hold (green
// screen, if enabled, overwrites alpha with the matte downstream).
NvCV_Status Download(EyeContactImpl* impl, uint8_t* bgra, int w, int h) {
    NvCVImage dst{};
    NvCVImage_Init(&dst, w, h, w * 4, bgra, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
    NvCV_Status s = NvCVImage_Transfer(&impl->outGpu, &dst, 1.0f, impl->stream, &impl->tmp);
    if (s != NVCV_SUCCESS) return s;
    const int stride = w * 4;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = bgra + static_cast<size_t>(stride) * y;
        for (int x = 3; x < stride; x += 4) row[x] = 0xFF;
    }
    return NVCV_SUCCESS;
}
} // namespace

bool EyeContact::ProcessFrame(uint8_t* bgra, int w, int h) {
    auto* impl = static_cast<EyeContactImpl*>(impl_);
    if (!impl || !impl->handle || !bgra || w <= 0 || h <= 0) return false;

    if (BindIO(impl, w, h) != NVCV_SUCCESS) { lastError_ = "BindIO failed"; ready_ = false; return false; }
    if (Upload(impl, bgra, w, h) != NVCV_SUCCESS) { lastError_ = "Upload (Transfer) failed"; return false; }
    if (NvAR_Run(impl->handle) != NVCV_SUCCESS) { lastError_ = "NvAR_Run failed"; return false; }
    // No explicit stream sync: NvAR has no CudaStreamSynchronize, and the GPU->CPU
    // NvCVImage_Transfer in Download is host-blocking (as in the GazeRedirect sample).
    if (Download(impl, bgra, w, h) != NVCV_SUCCESS) { lastError_ = "Download (Transfer) failed"; return false; }
    return true;
}

#else
// ---- Passthrough stub: built when the AR SDK is not configured. ----
EyeContact::EyeContact() = default;
EyeContact::~EyeContact() = default;
bool EyeContact::Probe(std::string& detail) { detail = "AR SDK not built in"; return false; }
bool EyeContact::Start() { lastError_ = "AR SDK not built in"; ready_ = false; return false; }
void EyeContact::Stop() { ready_ = false; }
bool EyeContact::ProcessFrame(uint8_t*, int, int) { return false; }
#endif
