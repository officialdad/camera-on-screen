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

private:
    void WorkerLoop();
};
