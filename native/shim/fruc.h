#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Wraps NVIDIA Optical Flow FRUC (NvOFFRUC.dll) frame interpolation. CPU round-trip:
// BGRA in -> FRUC's own CUDA-11 context (ARGB devptr) -> BGRA mid frame out. Holds NO state
// across calls except the FRUC pipeline; the caller owns prev/cur frame history. Without
// COS_HAS_FRUC this is a never-ready passthrough stub (Probe returns false). Co-version with
// the Maxine CUDA-12/TRT effects is proven (separate cudart64_110.dll, no TensorRT).
class Fruc {
public:
    Fruc();
    ~Fruc();
    static bool Probe(std::string& detail);     // tries to LoadLibrary+Create FRUC
    bool Start(int width, int height);          // create FRUC + register CUDA buffers
    void Stop();
    // Synthesises the temporal-midpoint frame between prevBgra and curBgra (both w*h BGRA8).
    // Writes w*h BGRA8 into outMid. Returns false on failure (outMid untouched). If FRUC
    // reports frame repetition (no usable motion), outMid is a copy of curBgra and returns true.
    bool Interpolate(const uint8_t* prevBgra, const uint8_t* curBgra, int width, int height,
                     std::vector<uint8_t>& outMid);
    bool IsReady() const { return ready_; }
    const std::string& LastError() const { return lastError_; }
private:
    bool ready_ = false;
    int  width_ = 0, height_ = 0;
    std::string lastError_;
    void* impl_ = nullptr;  // owns CUcontext + NvOFFRUCHandle + device buffers
};
