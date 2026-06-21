#pragma once
#include <cstdint>
#include <string>

// Wraps the Maxine AR SDK GazeRedirection (Eye Contact) effect. CPU-copy: a tightly-
// packed BGRA frame is uploaded to the GPU (converted to BGR), gaze-redirected, and the
// redirected image is downloaded back into the same BGRA buffer (alpha forced opaque).
// All methods are no-throw; failure is reported via IsReady() + LastError(). When built
// without the SDK (COS_HAS_MAXINE_AR undefined) this is a stub that is never ready, so the
// shim degrades to passthrough. Mirrors the Aigs (green-screen) wrapper.
class EyeContact {
public:
    EyeContact();
    ~EyeContact();

    // One-shot probe: can the AR SDK load and the GazeRedirection feature create+load?
    // Does not retain the feature. Fills 'detail'. Call before starting capture.
    static bool Probe(std::string& detail);

    // Create the feature + CUDA stream and configure+load it. Call on the capture worker
    // thread (NvAR/CUDA thread affinity). Returns true on success; on failure IsReady()==false.
    bool Start();

    // Destroy the feature, stream, and images. Call on the worker thread.
    void Stop();

    // Gaze-redirect a tightly-packed BGRA buffer (width*height*4) in place. Returns true if
    // applied; false leaves 'bgra' untouched. Eye-size sensitivity / look-away params are
    // fixed at SDK defaults for M4 (toggle-only UI); the ABI doubles are reserved/ignored.
    bool ProcessFrame(uint8_t* bgra, int width, int height);

    bool IsReady() const { return ready_; }
    const std::string& LastError() const { return lastError_; }

private:
    bool ready_ = false;
    std::string lastError_;
    void* impl_ = nullptr; // opaque; real fields live in eyecontact.cpp behind COS_HAS_MAXINE_AR
};
