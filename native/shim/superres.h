#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Wraps the Maxine VFX Video Super Resolution effect (NGX, selector "VideoSuperRes").
// QualityLevel doubles as the mode selector:
//   1-4   = upscale (VSR Low/Med/High/Ultra)  -> output = input * scale (scaleX10: 15=1.5x, 20=2x)
//   8-11  = denoise, 12-15 = deblur           -> output = input size (no upscale)
// In and out are BGRA u8 on the GPU (matches the capture pipeline; no BGR conversion). Alpha
// flows through VSR untouched. Without COS_HAS_MAXINE this is a never-ready passthrough stub.
class SuperRes {
public:
    SuperRes();
    ~SuperRes();
    static bool Probe(std::string& detail);
    bool Start(int qualityLevel, int scaleX10);
    void Stop();
    // Reads w*h BGRA from 'bgra', writes outW*outH BGRA into 'out'. Returns false on failure
    // (out untouched). For upscale modes outW/outH are w*scale,h*scale; otherwise w,h.
    bool ProcessFrame(const uint8_t* bgra, int w, int h, std::vector<uint8_t>& out, int& outW, int& outH);
    bool IsReady() const { return ready_; }
    const std::string& LastError() const { return lastError_; }
private:
    bool ready_ = false;
    int  qualityLevel_ = 1;
    int  scaleX10_ = 20;
    std::string lastError_;
    void* impl_ = nullptr;
};
