// Standalone smoke for the ONNX green-screen engine (issue #24). Needs the ORT runtime
// + model (COS_SEG_RUNTIME_DIR or <exe>/onnx). Feeds a synthetic frame, asserts the
// engine writes a plausible matte, prints per-frame latency. Exit 0 = pass.
//
// Built by CMake as target seg_probe (compiles seg_onnx.cpp directly — the shim's
// class symbols are hidden-visibility, so linking the .so is not an option).
#include <algorithm>
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

    // Synthetic 640x480 "selfie" portrait: dark background gradient (simple lighting),
    // a skin-tone head ellipse (radially shaded for roundness) with eye/mouth blots for
    // edge contrast, and broad shoulders/torso below in a different clothing color —
    // close, selfie-style framing the segmentation model can plausibly recognize as a
    // person. A flat-color blob (this smoke's first version) produced zero confidence
    // everywhere on this model; this shape was found by iterating on the dev box
    // against the real model until it wasn't degenerate in either direction.
    const int w = 640, h = 480;
    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
    const int cx = 320, cy = 190, rx = 150, ry = 180;                    // head ellipse
    const int shirtX0 = 10, shirtX1 = 630, shirtY0 = 300, shirtY1 = 480; // shoulders/torso
    for (int y = 0; y < h; ++y) {
        const float t = static_cast<float>(y) / h; // vertical lighting gradient
        for (int x = 0; x < w; ++x) {
            uint8_t* px = bgra.data() + (static_cast<size_t>(y) * w + x) * 4;
            const double dx = (x - cx) / static_cast<double>(rx);
            const double dy = (y - cy) / static_cast<double>(ry);
            const double r2 = dx * dx + dy * dy;
            if (r2 <= 1.0) {
                const float shade = 1.0f - 0.35f * static_cast<float>(r2); // head roundness
                px[0] = static_cast<uint8_t>(140 * shade); // skin tone, BGR
                px[1] = static_cast<uint8_t>(180 * shade);
                px[2] = static_cast<uint8_t>(220 * shade);
            } else if (x >= shirtX0 && x <= shirtX1 && y >= shirtY0 && y <= shirtY1) {
                px[0] = 40; px[1] = 40; px[2] = 150; // dark red shirt, BGR
            } else {
                px[0] = static_cast<uint8_t>(20 + t * 35); // dark background, BGR
                px[1] = static_cast<uint8_t>(18 + t * 30);
                px[2] = static_cast<uint8_t>(16 + t * 25);
            }
            px[3] = 255;
        }
    }
    auto blot = [&](int ex, int ey, int erx, int ery, uint8_t b, uint8_t g, uint8_t r) {
        for (int y = std::max(0, ey - ery); y < std::min(h, ey + ery); ++y)
            for (int x = std::max(0, ex - erx); x < std::min(w, ex + erx); ++x) {
                const double ddx = (x - ex) / static_cast<double>(erx);
                const double ddy = (y - ey) / static_cast<double>(ery);
                if (ddx * ddx + ddy * ddy <= 1.0) {
                    uint8_t* px = bgra.data() + (static_cast<size_t>(y) * w + x) * 4;
                    px[0] = b; px[1] = g; px[2] = r;
                }
            }
    };
    blot(cx - 40, cy - 10, 12, 8, 30, 30, 30); // left eye
    blot(cx + 40, cy - 10, 12, 8, 30, 30, 30); // right eye
    blot(cx, cy + 50, 25, 6, 90, 60, 120);     // mouth

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

    // Matte plausibility: alpha must vary (not all-255 passthrough, not all-0 either —
    // the latter would mean the model saw no person at all, which is just as much a
    // sign of a broken pipeline as a stuck-opaque passthrough).
    uint64_t histLow = 0, histHigh = 0;
    for (size_t i = 3; i < work.size(); i += 4) {
        if (work[i] < 64) ++histLow;
        if (work[i] > 192) ++histHigh;
    }
    const uint64_t total = static_cast<uint64_t>(w) * h;
    std::printf("avg %.2f ms/frame; alpha<64: %llu px, alpha>192: %llu px\n",
                ms, (unsigned long long)histLow, (unsigned long long)histHigh);
    if (histLow == 0 && histHigh == total) {
        std::fprintf(stderr, "FAIL: matte is all-opaque (engine did not run?)\n");
        return 1;
    }
    if (histHigh == 0 && histLow == total) {
        std::fprintf(stderr, "FAIL: matte is all-transparent (no person detected in synthetic frame?)\n");
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
