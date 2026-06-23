#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Wraps the Maxine VFX Super Resolution effect. Upscales a BGRA frame by 1.5x or 2x
// into a freshly sized BGRA output buffer (alpha = 0xFF). Without COS_HAS_MAXINE this
// is a never-ready stub. scaleX10: 15 => 1.5x, 20 => 2x.
class SuperRes {
public:
    SuperRes();
    ~SuperRes();
    static bool Probe(std::string& detail);
    bool Start(int scaleX10);
    void Stop();
    // Reads w*h BGRA from 'bgra', writes outW*outH BGRA into 'out'. Returns false on failure
    // (out untouched). outW/outH are w*scale, h*scale.
    bool ProcessFrame(const uint8_t* bgra, int w, int h, std::vector<uint8_t>& out, int& outW, int& outH);
    bool IsReady() const { return ready_; }
    const std::string& LastError() const { return lastError_; }
private:
    bool ready_ = false;
    int  scaleX10_ = 20;
    std::string lastError_;
    void* impl_ = nullptr;
};
