#pragma once
#include <cstdint>

// Rolling FPS estimate. SINGLE-CONSUMER (the status poll thread) — not internally
// synchronized. Feed a monotonically increasing cumulative frame count plus a
// monotonic clock reading in seconds. Recomputes over the elapsed window, but only
// refreshes once at least kMinInterval has passed so a fast poll can't divide by ~0.
class FpsCounter {
public:
    double Sample(double nowSec, uint64_t totalFrames) {
        if (!started_) { started_ = true; lastSec_ = nowSec; lastFrames_ = totalFrames; return fps_; }
        const double dt      = nowSec      - lastSec_;
        const auto   dframes = totalFrames - lastFrames_;
        lastSec_    = nowSec;       // always advance baseline so next window starts here
        lastFrames_ = totalFrames;
        if (dt < kMinInterval) return fps_;     // too soon: keep the last estimate
        fps_ = static_cast<double>(dframes) / dt;
        return fps_;
    }
    void Reset() { started_ = false; fps_ = 0.0; lastSec_ = 0.0; lastFrames_ = 0; }
private:
    static constexpr double kMinInterval = 0.5; // refresh at most ~2x/sec
    bool     started_   = false;
    double   lastSec_   = 0.0;
    double   fps_       = 0.0;
    uint64_t lastFrames_ = 0;
};
