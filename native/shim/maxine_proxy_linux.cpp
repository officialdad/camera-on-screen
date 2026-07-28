// Linux replacement for NVIDIA's proxy stubs, which do not compile on Linux (the VFX
// NVVideoEffectsProxy.cpp/nvCVImageProxy.cpp are "#warning not ported"; nvARProxy.cpp's
// getNvARLib() uses unguarded TCHAR/SetDllDirectory). PreloadMaxineClosure has already
// dlopen'd the runtime with RTLD_GLOBAL, so every symbol is reachable via RTLD_DEFAULT.
// The forwarders below are NOT exported (empty NvVFX_API/NvAR_API/NvCV_API + hidden
// visibility preset), so dlsym can never resolve back to them and recurse.
// decltype on the header declarations keeps every signature exact — drift = compile error.
#if !defined(_WIN32) && (defined(COS_HAS_MAXINE) || defined(COS_HAS_MAXINE_AR))
#include <dlfcn.h>
#include "nvCVStatus.h"
#include "nvCVImage.h"

namespace {
template <typename Fn>
Fn* Sym(const char* name) { return reinterpret_cast<Fn*>(dlsym(RTLD_DEFAULT, name)); }
}

#define COS_FWD(fn, ...)                                   \
    static auto* f = Sym<decltype(fn)>(#fn);               \
    if (!f) return NVCV_ERR_LIBRARY;                       \
    return f(__VA_ARGS__)

// ---- NvCVImage_* + status strings (libNVCVImage.so) — needed by both effects ----

NvCV_Status NvCVImage_Alloc(NvCVImage* im, unsigned width, unsigned height, NvCVImage_PixelFormat format, NvCVImage_ComponentType type, unsigned layout, unsigned memSpace, unsigned alignment) { COS_FWD(NvCVImage_Alloc, im, width, height, format, type, layout, memSpace, alignment); }
void NvCVImage_Dealloc(NvCVImage* im) { static auto* f = Sym<decltype(NvCVImage_Dealloc)>("NvCVImage_Dealloc"); if (f) f(im); }
NvCV_Status NvCVImage_Init(NvCVImage* im, unsigned width, unsigned height, int pitch, void* pixels, NvCVImage_PixelFormat format, NvCVImage_ComponentType type, unsigned layout, unsigned memSpace) { COS_FWD(NvCVImage_Init, im, width, height, pitch, pixels, format, type, layout, memSpace); }
NvCV_Status NvCVImage_Transfer(const NvCVImage* src, NvCVImage* dst, float scale, struct CUstream_st* stream, NvCVImage* tmp) { COS_FWD(NvCVImage_Transfer, src, dst, scale, stream, tmp); }
const char* NvCV_GetErrorStringFromCode(NvCV_Status code) { static auto* f = Sym<decltype(NvCV_GetErrorStringFromCode)>("NvCV_GetErrorStringFromCode"); return f ? f(code) : "NvCV library not loaded"; }

#ifdef COS_HAS_MAXINE
// ---- NvVFX_* (libVideoFX.so) ----
#include "nvVideoEffects.h"

NvCV_Status NvVFX_CreateEffect(NvVFX_EffectSelector code, NvVFX_Handle* effect) { COS_FWD(NvVFX_CreateEffect, code, effect); }
void NvVFX_DestroyEffect(NvVFX_Handle effect) { static auto* f = Sym<decltype(NvVFX_DestroyEffect)>("NvVFX_DestroyEffect"); if (f) f(effect); }
NvCV_Status NvVFX_SetString(NvVFX_Handle effect, NvVFX_ParameterSelector paramName, const char* str) { COS_FWD(NvVFX_SetString, effect, paramName, str); }
NvCV_Status NvVFX_SetU32(NvVFX_Handle effect, NvVFX_ParameterSelector paramName, unsigned int val) { COS_FWD(NvVFX_SetU32, effect, paramName, val); }
NvCV_Status NvVFX_SetImage(NvVFX_Handle effect, NvVFX_ParameterSelector paramName, NvCVImage* im) { COS_FWD(NvVFX_SetImage, effect, paramName, im); }
NvCV_Status NvVFX_SetCudaStream(NvVFX_Handle effect, NvVFX_ParameterSelector paramName, CUstream stream) { COS_FWD(NvVFX_SetCudaStream, effect, paramName, stream); }
NvCV_Status NvVFX_CudaStreamCreate(CUstream* stream) { COS_FWD(NvVFX_CudaStreamCreate, stream); }
NvCV_Status NvVFX_CudaStreamDestroy(CUstream stream) { COS_FWD(NvVFX_CudaStreamDestroy, stream); }
NvCV_Status NvVFX_CudaStreamSynchronize(CUstream stream) { COS_FWD(NvVFX_CudaStreamSynchronize, stream); }
NvCV_Status NvVFX_Load(NvVFX_Handle effect) { COS_FWD(NvVFX_Load, effect); }
NvCV_Status NvVFX_Run(NvVFX_Handle effect, int async) { COS_FWD(NvVFX_Run, effect, async); }
#endif // COS_HAS_MAXINE

#ifdef COS_HAS_MAXINE_AR
// ---- NvAR_* (libnvARPose.so) ----
#include "nvAR.h"
#include "nvAR_defs.h"

NvCV_Status NvAR_Create(NvAR_FeatureID featureID, NvAR_FeatureHandle* handle) { COS_FWD(NvAR_Create, featureID, handle); }
NvCV_Status NvAR_Destroy(NvAR_FeatureHandle handle) { COS_FWD(NvAR_Destroy, handle); }
NvCV_Status NvAR_Load(NvAR_FeatureHandle handle) { COS_FWD(NvAR_Load, handle); }
NvCV_Status NvAR_Run(NvAR_FeatureHandle handle) { COS_FWD(NvAR_Run, handle); }
NvCV_Status NvAR_SetU32(NvAR_FeatureHandle handle, const char* name, unsigned int val) { COS_FWD(NvAR_SetU32, handle, name, val); }
NvCV_Status NvAR_SetS32(NvAR_FeatureHandle handle, const char* name, int val) { COS_FWD(NvAR_SetS32, handle, name, val); }
NvCV_Status NvAR_SetString(NvAR_FeatureHandle handle, const char* name, const char* str) { COS_FWD(NvAR_SetString, handle, name, str); }
NvCV_Status NvAR_SetCudaStream(NvAR_FeatureHandle handle, const char* name, CUstream stream) { COS_FWD(NvAR_SetCudaStream, handle, name, stream); }
NvCV_Status NvAR_SetObject(NvAR_FeatureHandle handle, const char* name, void* ptr, unsigned long typeSize) { COS_FWD(NvAR_SetObject, handle, name, ptr, typeSize); }
NvCV_Status NvAR_SetF32Array(NvAR_FeatureHandle handle, const char* name, float* vals, int count) { COS_FWD(NvAR_SetF32Array, handle, name, vals, count); }
NvCV_Status NvAR_GetU32(NvAR_FeatureHandle handle, const char* name, unsigned int* val) { COS_FWD(NvAR_GetU32, handle, name, val); }
NvCV_Status NvAR_CudaStreamCreate(CUstream* stream) { COS_FWD(NvAR_CudaStreamCreate, stream); }
NvCV_Status NvAR_CudaStreamDestroy(CUstream stream) { COS_FWD(NvAR_CudaStreamDestroy, stream); }
#endif // COS_HAS_MAXINE_AR

#endif // !_WIN32 && (COS_HAS_MAXINE || COS_HAS_MAXINE_AR)
