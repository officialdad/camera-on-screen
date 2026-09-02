#include "gpu_mem.h"
#include "paths.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// Minimal driver-API surface, typed by hand so no cuda.h is needed at build time (mirrors
// the stable 64-bit ABI; the *_v2 names are the 64-bit entry points, plain = legacy).
using CUresult  = int;
using CUdevice  = int;
using CUcontext = struct CUctx_st*;
constexpr CUresult kCudaSuccess = 0;
constexpr CUresult kCudaErrorOutOfMemory = 2;

struct Driver {
    CUresult (*cuInit)(unsigned) = nullptr;
    CUresult (*cuDeviceGet)(CUdevice*, int) = nullptr;
    CUresult (*cuDevicePrimaryCtxRetain)(CUcontext*, CUdevice) = nullptr;
    CUresult (*cuCtxSetCurrent)(CUcontext) = nullptr;
    CUresult (*cuMemGetInfo)(size_t*, size_t*) = nullptr;
    bool     tried = false;
    bool     ok = false;
    std::string err;
    // Retained once for the process lifetime. The primary context is what cudart (Maxine) and
    // FRUC bind to anyway; holding a reference just means our query never tears it down and
    // rebuilds it between effects (~0.5 s and ~300 MiB each time).
    CUcontext ctx = nullptr;
    CUdevice  dev = 0;
};

Driver g_drv;
std::mutex g_mtx;

void* Sym(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

// Loads the driver library and resolves the five entry points. Under g_mtx.
bool LoadDriverLocked() {
    if (g_drv.tried) return g_drv.ok;
    g_drv.tried = true;
#ifdef _WIN32
    void* lib = reinterpret_cast<void*>(LoadLibraryW(L"nvcuda.dll"));
    const char* libName = "nvcuda.dll";
#else
    void* lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    const char* libName = "libcuda.so.1";
#endif
    if (!lib) { g_drv.err = std::string(libName) + " not found (no NVIDIA driver)"; return false; }
    g_drv.cuInit                   = reinterpret_cast<CUresult(*)(unsigned)>(Sym(lib, "cuInit"));
    g_drv.cuDeviceGet              = reinterpret_cast<CUresult(*)(CUdevice*, int)>(Sym(lib, "cuDeviceGet"));
    g_drv.cuDevicePrimaryCtxRetain = reinterpret_cast<CUresult(*)(CUcontext*, CUdevice)>(Sym(lib, "cuDevicePrimaryCtxRetain"));
    g_drv.cuCtxSetCurrent          = reinterpret_cast<CUresult(*)(CUcontext)>(Sym(lib, "cuCtxSetCurrent"));
    g_drv.cuMemGetInfo             = reinterpret_cast<CUresult(*)(size_t*, size_t*)>(Sym(lib, "cuMemGetInfo_v2"));
    if (!g_drv.cuInit || !g_drv.cuDeviceGet || !g_drv.cuDevicePrimaryCtxRetain
        || !g_drv.cuCtxSetCurrent || !g_drv.cuMemGetInfo) {
        g_drv.err = std::string(libName) + " is missing driver API symbols";
        return false;
    }
    g_drv.ok = true;
    return true;
}

} // namespace

namespace gpumem {

size_t MinFreeMiB() {
    std::string v = EnvVar("COS_GPU_MIN_FREE_MIB");
    if (!v.empty()) {
        char* end = nullptr;
        unsigned long long n = std::strtoull(v.c_str(), &end, 10);
        if (end && *end == '\0' && n > 0) return static_cast<size_t>(n);
    }
    return kDefaultMinFreeMiB;
}

bool Query(Info& out, std::string& err) {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!LoadDriverLocked()) { err = g_drv.err; return false; }

    char buf[128];
    if (!g_drv.ctx) {
        CUresult r = g_drv.cuInit(0);
        if (r != kCudaSuccess) {
            std::snprintf(buf, sizeof(buf), "cuInit failed (CUDA error %d)", r);
            err = buf; return false;
        }
        r = g_drv.cuDeviceGet(&g_drv.dev, 0);
        if (r != kCudaSuccess) {
            std::snprintf(buf, sizeof(buf), "no CUDA device (error %d)", r);
            err = buf; return false;
        }
        CUcontext ctx = nullptr;
        r = g_drv.cuDevicePrimaryCtxRetain(&ctx, g_drv.dev);
        if (r != kCudaSuccess || !ctx) {
            std::snprintf(buf, sizeof(buf), "CUDA context creation failed (error %d%s)", r,
                          r == kCudaErrorOutOfMemory ? ", GPU memory exhausted" : "");
            err = buf; return false;
        }
        g_drv.ctx = ctx;
    }
    // The context must be current on THIS thread for cuMemGetInfo; the caller (probe thread or
    // capture worker) is about to run cudart on the same primary context anyway.
    CUresult r = g_drv.cuCtxSetCurrent(g_drv.ctx);
    if (r != kCudaSuccess) {
        std::snprintf(buf, sizeof(buf), "cuCtxSetCurrent failed (error %d)", r);
        err = buf; return false;
    }
    size_t freeB = 0, totalB = 0;
    r = g_drv.cuMemGetInfo(&freeB, &totalB);
    if (r != kCudaSuccess) {
        std::snprintf(buf, sizeof(buf), "cuMemGetInfo failed (error %d)", r);
        err = buf; return false;
    }
    out.freeMiB  = freeB  >> 20;
    out.totalMiB = totalB >> 20;
    return true;
}

bool CanLoad(std::string& reason) {
    Info info; std::string err;
    if (!Query(info, err)) {
        // No driver / no device: nothing to gate on, let the SDK explain itself. A context
        // failure is different — if we can't stand up CUDA, neither can Maxine, and its
        // loaders don't fail safely.
        if (err.find("context") == std::string::npos) return true;
        reason = err;
        return false;
    }
    const size_t need = MinFreeMiB();
    if (info.freeMiB >= need) return true;
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "GPU memory low: %llu MiB free of %llu, AI effects need %llu MiB. Close other GPU apps (LLMs, image generators)",
                  static_cast<unsigned long long>(info.freeMiB),
                  static_cast<unsigned long long>(info.totalMiB),
                  static_cast<unsigned long long>(need));
    reason = buf;
    return false;
}

} // namespace gpumem
