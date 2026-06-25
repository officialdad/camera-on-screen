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
    Fruc(const Fruc&) = delete;
    Fruc& operator=(const Fruc&) = delete;
    Fruc(Fruc&&) = delete;
    Fruc& operator=(Fruc&&) = delete;
    static bool Probe(std::string& detail);     // tries to LoadLibrary+Create FRUC
    bool Start(int width, int height);          // create FRUC + register CUDA buffers
    void Stop();
    // Feeds the next w*h BGRA frame into the streaming pipeline. Writes the temporal-midpoint
    // frame between the PREVIOUSLY submitted frame and this one into outMid (w*h BGRA) and sets
    // hasMid=true. On the FIRST call after Start (no previous frame) hasMid=false, outMid
    // untouched, returns true (not an error). On FRUC frame-repetition outMid holds the repeated
    // frame, hasMid=true. Returns false only on a real error (outMid untouched, hasMid=false).
    bool Submit(const uint8_t* curBgra, int width, int height, std::vector<uint8_t>& outMid, bool& hasMid);
    bool IsReady() const { return ready_; }
    const std::string& LastError() const { return lastError_; }
private:
    bool ready_ = false;
    int  width_ = 0, height_ = 0;
    std::string lastError_;
    void* impl_ = nullptr;  // owns CUcontext + NvOFFRUCHandle + device buffers
};
