#pragma once
#include <cstdint>
#include <string>

// Wraps the Maxine VFX Artifact Reduction effect. CPU-copy: a BGRA frame is uploaded
// to the GPU as BGR, cleaned, downloaded, and written back over the same BGRA buffer
// (RGB replaced, alpha left untouched). No size change. All methods no-throw; failure
// via IsReady()/LastError(). Without COS_HAS_MAXINE this is a never-ready stub.
class ArtifactReduction {
public:
    ArtifactReduction();
    ~ArtifactReduction();
    static bool Probe(std::string& detail);
    bool Start();
    void Stop();
    bool ProcessFrame(uint8_t* bgra, int width, int height);
    bool IsReady() const { return ready_; }
    const std::string& LastError() const { return lastError_; }
private:
    bool ready_ = false;
    std::string lastError_;
    void* impl_ = nullptr;
};
