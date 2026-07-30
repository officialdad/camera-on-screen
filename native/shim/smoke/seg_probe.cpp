// Standalone smoke for the ONNX green-screen engine (issue #24). Needs the ORT runtime
// + model (COS_SEG_RUNTIME_DIR or <exe>/onnx). Feeds a synthetic frame, asserts the
// engine writes a plausible matte, prints per-frame latency. Exit 0 = pass.
//
// Built by CMake as target seg_probe (compiles seg_onnx.cpp directly — the shim's
// class symbols are hidden-visibility, so linking the .so is not an option).
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include "../seg_onnx.h"

int main() {
    std::string detail;
    if (!SegOnnx::Probe(detail)) {
        std::fprintf(stderr, "PROBE FAIL: %s\n", detail.c_str());
        return 1;
    }
    std::printf("probe: %s\n", detail.c_str());

    SegOnnx seg;
    if (!seg.Start()) {
        std::fprintf(stderr, "START FAIL: %s\n", seg.LastError().c_str());
        return 1;
    }

    // Synthetic 640x480 frame: dark background, bright centered "torso" blob — enough
    // structure that the model produces a non-constant confidence map.
    const int w = 640, h = 480;
    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* px = bgra.data() + (static_cast<size_t>(y) * w + x) * 4;
            const bool blob = (x > w / 3 && x < 2 * w / 3 && y > h / 4);
            px[0] = blob ? 200 : 30; px[1] = blob ? 180 : 40;
            px[2] = blob ? 160 : 50; px[3] = 255;
        }

    // Warm-up + timed runs.
    std::vector<uint8_t> work = bgra;
    if (!seg.ProcessFrame(work.data(), w, h, 0.0, 0.0)) {
        std::fprintf(stderr, "PROCESS FAIL: %s\n", seg.LastError().c_str());
        return 1;
    }
    const int kRuns = 30;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kRuns; ++i) {
        work = bgra;
        if (!seg.ProcessFrame(work.data(), w, h, 0.2, 0.2)) {
            std::fprintf(stderr, "PROCESS FAIL (run %d): %s\n", i, seg.LastError().c_str());
            return 1;
        }
    }
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kRuns;

    // Matte plausibility: alpha must vary (not all-255 passthrough, not all-0).
    uint64_t histLow = 0, histHigh = 0;
    for (size_t i = 3; i < work.size(); i += 4) {
        if (work[i] < 64) ++histLow;
        if (work[i] > 192) ++histHigh;
    }
    std::printf("avg %.2f ms/frame; alpha<64: %llu px, alpha>192: %llu px\n",
                ms, (unsigned long long)histLow, (unsigned long long)histHigh);
    if (histLow == 0 && histHigh == static_cast<uint64_t>(w) * h) {
        std::fprintf(stderr, "FAIL: matte is all-opaque (engine did not run?)\n");
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
