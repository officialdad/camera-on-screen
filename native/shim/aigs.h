#pragma once
#include <cstdint>
#include <string>

// Wraps the Maxine GreenScreen effect. CPU-copy: a BGRA frame is uploaded to the
// GPU, the matte is computed, downloaded, and composited into the same BGRA buffer
// as premultiplied alpha. All methods are no-throw; failure is reported via IsReady()
// + LastError(). When built without the SDK (COS_HAS_MAXINE undefined) this is a
// stub that is never ready, so the shim degrades to opaque passthrough.
class Aigs {
public:
    Aigs();
    ~Aigs();

    // One-shot probe: can the SDK load and the GreenScreen effect be created+loaded?
    // Does not retain the effect. Call before starting capture (e.g. from the
    // orchestrator). Not designed for concurrent calls from multiple threads. Fills 'detail'.
    static bool Probe(std::string& detail);

    // Create the effect, CUDA stream, and (lazily) the GPU images. Call on the
    // capture worker thread. Returns true on success; on failure IsReady()==false.
    bool Start();

    // Destroy the effect, stream, and images. Call on the worker thread.
    void Stop();

    // Run GreenScreen on a tightly-packed BGRA buffer (width*height*4) in place:
    // A = matte, RGB premultiplied by matte/255. Returns true if the matte was
    // applied; false leaves 'bgra' untouched (caller keeps opaque passthrough).
    bool ProcessFrame(uint8_t* bgra, int width, int height);

    bool IsReady() const { return ready_; }
    const std::string& LastError() const { return lastError_; }

private:
    bool ready_ = false;
    std::string lastError_;
    void* impl_ = nullptr; // opaque; real fields live in aigs.cpp behind COS_HAS_MAXINE
};
