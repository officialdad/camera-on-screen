#include "seg_onnx.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

#include "matte_ops.h"
#include "paths.h"
#include "third_party/onnxruntime/onnxruntime_c_api.h"

#ifdef _WIN32
  #define NOMINMAX
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace {

constexpr int kNetSize = 256; // model I/O is 256x256 (NHWC float, RGB 0..1 in, confidence out)
constexpr const char* kModelFile = "selfie_segmentation.onnx";

// ---- runtime resolution + dynamic loading -------------------------------------------

// COS_SEG_RUNTIME_DIR (dev override) -> <shim>/onnx (bundled tier). Same pattern as
// the Maxine resolvers in vfx_paths.cpp.
std::string ResolveRuntimeDir(std::string& err) {
    std::string dir = EnvVar("COS_SEG_RUNTIME_DIR");
    if (!dir.empty()) {
        if (DirExists(dir)) return dir;
        err = "COS_SEG_RUNTIME_DIR set but not a directory: " + dir;
        return std::string();
    }
    const std::string mod = ShimModuleDir();
    if (!mod.empty()) {
        dir = mod + "/onnx";
        if (DirExists(dir)) return dir;
    }
    err = "ONNX runtime not found (set COS_SEG_RUNTIME_DIR or ship <shim>/onnx)";
    return std::string();
}

void* LoadLib(const std::string& path) {
#ifdef _WIN32
    int wn = MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), nullptr, 0);
    if (wn <= 0) return nullptr;
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), &w[0], wn);
    return LoadLibraryW(w.c_str());
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* GetSym(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

// Process-wide ORT function table + the dir it was loaded from. Loaded once, never
// unloaded (in-process re-dlopen of ORT is unsupported; matches the Maxine preload).
// Not designed for concurrent Probe()/Start() calls (caller-sequenced), same as the
// Maxine preload in maxine_linux.cpp.
const OrtApi* g_ort = nullptr;
std::string   g_runtimeDir;

const OrtApi* EnsureOrt(std::string& err) {
    if (g_ort) return g_ort;
    const std::string dir = ResolveRuntimeDir(err);
    if (dir.empty()) return nullptr;
#ifdef _WIN32
    void* lib = LoadLib(dir + "\\onnxruntime.dll");
    if (!lib) { err = "failed to load ONNX Runtime from " + dir; return nullptr; }
#else
    void* lib = LoadLib(dir + "/libonnxruntime.so.1");
    if (!lib) {
        const char* dlerr = dlerror(); // cleared on read; capture before it's gone
        err = "failed to load ONNX Runtime from " + dir + ": " + (dlerr ? dlerr : "unknown error");
        return nullptr;
    }
#endif
    using GetBaseFn = const OrtApiBase*(ORT_API_CALL*)(void);
    auto getBase = reinterpret_cast<GetBaseFn>(GetSym(lib, "OrtGetApiBase"));
    if (!getBase) { err = "OrtGetApiBase export missing (not an ONNX Runtime library?)"; return nullptr; }
    const OrtApi* api = getBase()->GetApi(ORT_API_VERSION);
    if (!api) { err = "ONNX Runtime does not support C API version " + std::to_string(ORT_API_VERSION); return nullptr; }
    g_ort = api;
    g_runtimeDir = dir;
    return g_ort;
}

// OrtStatus -> bool + error string.
bool Check(const OrtApi* ort, OrtStatus* st, const char* what, std::string& err) {
    if (!st) return true;
    err = std::string(what) + ": " + ort->GetErrorMessage(st);
    ort->ReleaseStatus(st);
    return false;
}

// Validates a session input/output is a float32 tensor whose shape is compatible with
// `expected` (fixed dims must match exactly; a dynamic dim, encoded as -1, passes
// unconditionally). Guards against a wrong or incompatible model (different resolution,
// the 2-channel "Meet" segmentation variant, ...) silently loading and then heap-overreading
// in DownscaleToNet/UpscaleMatte, which trust the shape blindly. `typeInfo` stays owned by
// the caller (ReleaseTypeInfo); the OrtTensorTypeAndShapeInfo* obtained here is a view into
// it and must not be released separately (see CastTypeInfoToTensorInfo's doc comment).
bool CheckTensorShape(const OrtApi* ort, OrtTypeInfo* typeInfo, const int64_t* expected,
                       size_t expectedCount, const char* label, std::string& err) {
    const OrtTensorTypeAndShapeInfo* info = nullptr;
    if (!Check(ort, ort->CastTypeInfoToTensorInfo(typeInfo, &info), "CastTypeInfoToTensorInfo", err))
        return false;
    if (!info) { err = std::string(label) + ": not a tensor"; return false; }

    ONNXTensorElementDataType elemType;
    if (!Check(ort, ort->GetTensorElementType(info, &elemType), "GetTensorElementType", err)) return false;
    if (elemType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        err = std::string(label) + ": expected float32 element type, got " +
              std::to_string(static_cast<int>(elemType));
        return false;
    }

    size_t dimCount = 0;
    if (!Check(ort, ort->GetDimensionsCount(info, &dimCount), "GetDimensionsCount", err)) return false;
    if (dimCount != expectedCount) {
        err = std::string(label) + ": expected " + std::to_string(expectedCount) +
              " dims, got " + std::to_string(dimCount);
        return false;
    }

    std::vector<int64_t> dims(dimCount);
    if (!Check(ort, ort->GetDimensions(info, dims.data(), dimCount), "GetDimensions", err)) return false;
    for (size_t i = 0; i < expectedCount; ++i) {
        if (dims[i] != -1 && dims[i] != expected[i]) {
            err = std::string(label) + ": shape mismatch at dim " + std::to_string(i) +
                  " (expected " + std::to_string(expected[i]) + ", got " + std::to_string(dims[i]) + ")";
            return false;
        }
    }
    return true;
}

// ---- session ------------------------------------------------------------------------

struct SegImpl {
    OrtEnv*            env = nullptr;
    OrtSessionOptions* so = nullptr;
    OrtSession*        session = nullptr;
    OrtMemoryInfo*     memInfo = nullptr;
    std::string inName, outName;
    std::vector<float>   input;              // 1*256*256*3 NHWC RGB
    std::vector<uint8_t> matteWork, matteTmp; // packed w*h
};

void DestroySession(const OrtApi* ort, SegImpl* impl) {
    if (!impl) return;
    if (impl->memInfo) ort->ReleaseMemoryInfo(impl->memInfo);
    if (impl->session) ort->ReleaseSession(impl->session);
    if (impl->so)      ort->ReleaseSessionOptions(impl->so);
    if (impl->env)     ort->ReleaseEnv(impl->env);
    delete impl;
}

// Create env/options/session + resolve tensor names. On failure fills err and
// returns nullptr (everything created so far is released).
SegImpl* CreateSession(const OrtApi* ort, std::string& err) {
    SegImpl* impl = new (std::nothrow) SegImpl();
    if (!impl) { err = "out of memory"; return nullptr; }

    if (!Check(ort, ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "cos-seg", &impl->env), "CreateEnv", err) ||
        !Check(ort, ort->CreateSessionOptions(&impl->so), "CreateSessionOptions", err) ||
        // 2 threads: enough to parallelize the small 256x256 conv net without contending
        // with the capture worker thread's own per-frame work on this box's core count.
        !Check(ort, ort->SetIntraOpNumThreads(impl->so, 2), "SetIntraOpNumThreads", err) ||
        !Check(ort, ort->SetSessionGraphOptimizationLevel(impl->so, ORT_ENABLE_ALL), "SetGraphOptimizationLevel", err)) {
        DestroySession(ort, impl);
        return nullptr;
    }

    const std::string modelPath = g_runtimeDir +
#ifdef _WIN32
        "\\" + kModelFile;
    int wn = MultiByteToWideChar(CP_UTF8, 0, modelPath.data(), (int)modelPath.size(), nullptr, 0);
    std::wstring wpath((size_t)(wn > 0 ? wn : 0), L'\0');
    if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, modelPath.data(), (int)modelPath.size(), &wpath[0], wn);
    OrtStatus* cs = ort->CreateSession(impl->env, wpath.c_str(), impl->so, &impl->session);
#else
        "/" + kModelFile;
    OrtStatus* cs = ort->CreateSession(impl->env, modelPath.c_str(), impl->so, &impl->session);
#endif
    if (!Check(ort, cs, "CreateSession (model missing/corrupt?)", err)) { DestroySession(ort, impl); return nullptr; }

    // Reject a wrong-shaped model up front (per-session, not per-frame) — see
    // CheckTensorShape's comment for why DownscaleToNet/UpscaleMatte can't defend themselves.
    OrtTypeInfo* inType = nullptr;
    if (!Check(ort, ort->SessionGetInputTypeInfo(impl->session, 0, &inType), "GetInputTypeInfo", err)) {
        DestroySession(ort, impl);
        return nullptr;
    }
    const int64_t kExpectedIn[4] = {1, kNetSize, kNetSize, 3};
    const bool inOk = CheckTensorShape(ort, inType, kExpectedIn, 4, "input tensor", err);
    ort->ReleaseTypeInfo(inType);
    if (!inOk) { DestroySession(ort, impl); return nullptr; }

    OrtTypeInfo* outType = nullptr;
    if (!Check(ort, ort->SessionGetOutputTypeInfo(impl->session, 0, &outType), "GetOutputTypeInfo", err)) {
        DestroySession(ort, impl);
        return nullptr;
    }
    const int64_t kExpectedOut[4] = {1, kNetSize, kNetSize, 1};
    const bool outOk = CheckTensorShape(ort, outType, kExpectedOut, 4, "output tensor", err);
    ort->ReleaseTypeInfo(outType);
    if (!outOk) { DestroySession(ort, impl); return nullptr; }

    OrtAllocator* alloc = nullptr;
    char* name = nullptr;
    if (!Check(ort, ort->GetAllocatorWithDefaultOptions(&alloc), "GetAllocator", err) ||
        !Check(ort, ort->SessionGetInputName(impl->session, 0, alloc, &name), "GetInputName", err)) {
        DestroySession(ort, impl);
        return nullptr;
    }
    impl->inName = name;
    bool freedIn = Check(ort, ort->AllocatorFree(alloc, name), "AllocatorFree(inputName)", err);
    name = nullptr;
    if (!freedIn ||
        !Check(ort, ort->SessionGetOutputName(impl->session, 0, alloc, &name), "GetOutputName", err)) {
        DestroySession(ort, impl);
        return nullptr;
    }
    impl->outName = name;
    if (!Check(ort, ort->AllocatorFree(alloc, name), "AllocatorFree(outputName)", err)) {
        DestroySession(ort, impl);
        return nullptr;
    }

    if (!Check(ort, ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &impl->memInfo), "CreateCpuMemoryInfo", err)) {
        DestroySession(ort, impl);
        return nullptr;
    }
    impl->input.resize(static_cast<size_t>(kNetSize) * kNetSize * 3);
    return impl;
}

// ---- resampling ----------------------------------------------------------------------

// Bilinear BGRA(w,h) -> float RGB (kNetSize^2, NHWC, 0..1).
void DownscaleToNet(const uint8_t* bgra, int w, int h, float* dst) {
    const float sx = static_cast<float>(w) / kNetSize, sy = static_cast<float>(h) / kNetSize;
    for (int y = 0; y < kNetSize; ++y) {
        const float fy = std::max(0.0f, (y + 0.5f) * sy - 0.5f);
        const int y0 = std::min(static_cast<int>(fy), h - 1), y1 = std::min(y0 + 1, h - 1);
        const float wy = fy - static_cast<float>(y0);
        for (int x = 0; x < kNetSize; ++x) {
            const float fx = std::max(0.0f, (x + 0.5f) * sx - 0.5f);
            const int x0 = std::min(static_cast<int>(fx), w - 1), x1 = std::min(x0 + 1, w - 1);
            const float wx = fx - static_cast<float>(x0);
            float* out = dst + (static_cast<size_t>(y) * kNetSize + x) * 3;
            for (int c = 0; c < 3; ++c) {
                const int sc = 2 - c; // dst RGB <- src BGRA
                const float p00 = bgra[(static_cast<size_t>(y0) * w + x0) * 4 + sc];
                const float p10 = bgra[(static_cast<size_t>(y0) * w + x1) * 4 + sc];
                const float p01 = bgra[(static_cast<size_t>(y1) * w + x0) * 4 + sc];
                const float p11 = bgra[(static_cast<size_t>(y1) * w + x1) * 4 + sc];
                const float top = p00 + (p10 - p00) * wx;
                const float bot = p01 + (p11 - p01) * wx;
                out[c] = (top + (bot - top) * wy) / 255.0f;
            }
        }
    }
}

// Bilinear float confidence (kNetSize^2, 0..1) -> packed u8 matte (w,h).
void UpscaleMatte(const float* net, int w, int h, uint8_t* matteBuf) {
    const float sx = static_cast<float>(kNetSize) / w, sy = static_cast<float>(kNetSize) / h;
    for (int y = 0; y < h; ++y) {
        const float fy = std::max(0.0f, (y + 0.5f) * sy - 0.5f);
        const int y0 = std::min(static_cast<int>(fy), kNetSize - 1), y1 = std::min(y0 + 1, kNetSize - 1);
        const float wy = fy - static_cast<float>(y0);
        for (int x = 0; x < w; ++x) {
            const float fx = std::max(0.0f, (x + 0.5f) * sx - 0.5f);
            const int x0 = std::min(static_cast<int>(fx), kNetSize - 1), x1 = std::min(x0 + 1, kNetSize - 1);
            const float wx = fx - static_cast<float>(x0);
            const float p00 = net[static_cast<size_t>(y0) * kNetSize + x0];
            const float p10 = net[static_cast<size_t>(y0) * kNetSize + x1];
            const float p01 = net[static_cast<size_t>(y1) * kNetSize + x0];
            const float p11 = net[static_cast<size_t>(y1) * kNetSize + x1];
            const float top = p00 + (p10 - p00) * wx;
            const float v = std::clamp(top + ((p01 + (p11 - p01) * wx) - top) * wy, 0.0f, 1.0f);
            matteBuf[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(v * 255.0f + 0.5f);
        }
    }
}

} // namespace

// ---- public API -----------------------------------------------------------------------

SegOnnx::SegOnnx() = default;
SegOnnx::~SegOnnx() { Stop(); }

bool SegOnnx::Probe(std::string& detail) {
    const OrtApi* ort = EnsureOrt(detail);
    if (!ort) return false;
    SegImpl* impl = CreateSession(ort, detail);
    if (!impl) return false;
    DestroySession(ort, impl);
    detail = "ONNX CPU green screen available";
    return true;
}

bool SegOnnx::Start() {
    Stop();
    const OrtApi* ort = EnsureOrt(lastError_);
    if (!ort) return false;
    SegImpl* impl = CreateSession(ort, lastError_);
    if (!impl) return false;
    impl_ = impl;
    ready_ = true;
    lastError_.clear();
    return true;
}

void SegOnnx::Stop() {
    auto* impl = static_cast<SegImpl*>(impl_);
    if (impl) DestroySession(g_ort, impl);
    impl_ = nullptr;
    ready_ = false;
}

bool SegOnnx::ProcessFrame(uint8_t* bgra, int width, int height, double expand, double feather) {
    auto* impl = static_cast<SegImpl*>(impl_);
    if (!impl || !impl->session || !bgra || width <= 0 || height <= 0) return false;
    const OrtApi* ort = g_ort;

    DownscaleToNet(bgra, width, height, impl->input.data());

    const int64_t inShape[4] = {1, kNetSize, kNetSize, 3};
    OrtValue* inVal = nullptr;
    if (!Check(ort, ort->CreateTensorWithDataAsOrtValue(
            impl->memInfo, impl->input.data(), impl->input.size() * sizeof(float),
            inShape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVal),
            "CreateTensor", lastError_)) return false;

    const char* inNames[]  = {impl->inName.c_str()};
    const char* outNames[] = {impl->outName.c_str()};
    OrtValue* outVal = nullptr;
    OrtStatus* rs = ort->Run(impl->session, nullptr, inNames,
                             const_cast<const OrtValue* const*>(&inVal), 1, outNames, 1, &outVal);
    ort->ReleaseValue(inVal);
    if (!Check(ort, rs, "Run", lastError_)) return false;

    void* net = nullptr;
    if (!Check(ort, ort->GetTensorMutableData(outVal, &net), "GetTensorData", lastError_)) {
        ort->ReleaseValue(outVal);
        return false;
    }
    impl->matteWork.resize(static_cast<size_t>(width) * height);
    impl->matteTmp.resize(static_cast<size_t>(width) * height);
    UpscaleMatte(static_cast<const float*>(net), width, height, impl->matteWork.data());
    ort->ReleaseValue(outVal);

    matte::Dilate(impl->matteWork.data(), impl->matteTmp.data(), width, height,
                  matte::RadiusFromAmount(expand, matte::kMaxDilateRadius));
    matte::Feather(impl->matteWork.data(), impl->matteTmp.data(), width, height,
                   matte::RadiusFromAmount(feather, matte::kMaxBlurRadius));
    matte::CompositePremultiplied(bgra, impl->matteWork.data(), width, height);
    return true;
}
