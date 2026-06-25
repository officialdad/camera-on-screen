#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct CameraDesc { std::string id; std::string name; };

// Media Foundation capture wrapper.
//
// Threading model: Start() spawns a worker thread that pulls frames synchronously
// from an IMFSourceReader and writes the latest decoded BGRA frame into a
// mutex-guarded buffer. LatestFrame() copies that buffer out under the same mutex
// and returns true only when a new frame has been produced since the last call.
class Capture {
public:
    Capture() = default;
    ~Capture();

    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // symbolicLink == device id (symbolic link). Empty selects the first device.
    bool Start(const std::string& symbolicLink);
    void Stop();

    // Returns true and fills out (tightly packed BGRA, width*height*4) + dims
    // when a frame newer than the previous call is available; false otherwise.
    bool LatestFrame(std::vector<uint8_t>& out, int& w, int& h);

    static std::vector<CameraDesc> Enumerate();

    // Toggles AIGS for subsequent frames. Thread-safe; the worker owns the Aigs object.
    void SetGreenScreen(bool enabled);
    // Sets matte post-process amounts (0..1). Thread-safe; worker reads per frame.
    void SetMatteParams(double expand, double feather);
    // Snapshot for status polling. Thread-safe.
    bool GreenScreenActive() const;       // true only while AIGS is transforming frames
    std::string GreenScreenError() const; // empty when none

    // Toggles Eye Contact for subsequent frames. Thread-safe; the worker owns the object.
    void SetEyeContact(bool enabled);
    bool EyeContactActive() const;        // true only while gaze redirection is transforming frames
    std::string EyeContactError() const;  // empty when none

    // Toggles Super Resolution + VSR QualityLevel (mode) + scale (15=1.5x, 20=2x; upscale modes
    // only). Thread-safe; worker owns the object.
    void SetSuperRes(bool enabled, int qualityLevel, int scaleX10);
    bool SuperResActive() const;
    std::string SuperResError() const;

    // Measured frames-per-second over a rolling window (status polling). Thread-safe to
    // call from the single status-poll thread; 0 until the first interval elapses.
    double MeasuredFps() const;

private:
    void WorkerLoop();
};
