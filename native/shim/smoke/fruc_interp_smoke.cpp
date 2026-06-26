// Task 3: GPU integration smoke -- drives Fruc::Submit over an 8-frame sequence
// of a smoothly translated 2D sinusoidal texture, dumps BMP files, and reports
// byte/mean-abs-diff statistics to confirm genuine optical-flow interpolation.
//
// Frame layout (1280x720 BGRA):
//   Frame i: grayscale texture, B=G=R=intensity(x,y,dx), A=255
//   where dx = i*6 (6 px/frame horizontal translation),
//   intensity(x,y,dx) =
//     clamp(128 + 80*sin((x+dx)*0.04)*cos(y*0.04) + 40*sin((x+dx)*0.011+y*0.017), 0,255)
//   Small sub-threshold translation: smooth, well-posed for optical flow.
//   Frames 0..7 submitted sequentially via Fruc::Submit (streaming API).
//   Mid between frame6 and frame7 is the analysis target (warmed up by 6 prior pairs).
//
// Build: native\shim\smoke\build_fruc_interp_smoke.bat
// Run:
//   $env:COS_FRUC_RUNTIME_DIR="<OF_SDK>\NvOFFRUC\NvOFFRUCSample\bin\win64"
//   native\shim\smoke\fruc_interp_smoke.exe
//
// Output:
//   f6.bmp / mid67.bmp / f7.bmp in the session scratchpad dir (paths printed to stdout)
//   Submit avg latency in ms (excluding first/warm-up call)
//   % bytes identical mid-vs-f6 and mid-vs-f7
//   meanAbsDiff mid-f6, mid-f7, f6-f7
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>
#include "../fruc.h"

// ---------------------------------------------------------------------------
// Minimal inline BMP writer: 32-bit BGRA (stored on disk as B G R A per pixel,
// which is standard for 32-bpp BMPs with BI_RGB/BI_BITFIELDS).
// ---------------------------------------------------------------------------
static bool WriteBmp(const char* path, const uint8_t* bgra, int width, int height) {
    const int rowBytes  = width * 4;
    const int imageSize = rowBytes * height;

#pragma pack(push, 1)
    struct BmpFileHeader {
        uint16_t bfType;
        uint32_t bfSize;
        uint16_t bfReserved1;
        uint16_t bfReserved2;
        uint32_t bfOffBits;
    };
    struct BmpInfoHeader {
        uint32_t biSize;
        int32_t  biWidth;
        int32_t  biHeight;     // negative = top-down stored
        uint16_t biPlanes;
        uint16_t biBitCount;
        uint32_t biCompression; // BI_RGB = 0
        uint32_t biSizeImage;
        int32_t  biXPelsPerMeter;
        int32_t  biYPelsPerMeter;
        uint32_t biClrUsed;
        uint32_t biClrImportant;
    };
#pragma pack(pop)

    BmpFileHeader fh{};
    fh.bfType    = 0x4D42; // 'BM'
    fh.bfOffBits = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    fh.bfSize    = fh.bfOffBits + (uint32_t)imageSize;

    BmpInfoHeader ih{};
    ih.biSize        = sizeof(BmpInfoHeader);
    ih.biWidth       = width;
    ih.biHeight      = -height; // negative = top-down (no row flip needed)
    ih.biPlanes      = 1;
    ih.biBitCount    = 32;
    ih.biCompression = 0;       // BI_RGB
    ih.biSizeImage   = (uint32_t)imageSize;

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bgra, 1, (size_t)imageSize, f);
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Compare two same-size BGRA buffers; return fraction [0,1] of bytes that match.
// ---------------------------------------------------------------------------
static double ByteMatchFraction(const uint8_t* a, const uint8_t* b, size_t bytes) {
    size_t same = 0;
    for (size_t i = 0; i < bytes; ++i) same += (a[i] == b[i]) ? 1u : 0u;
    return (double)same / (double)bytes;
}

// ---------------------------------------------------------------------------
// Mean absolute difference (per byte, 0-255 scale) between two same-size buffers.
// ---------------------------------------------------------------------------
static double MeanAbsDiff(const uint8_t* a, const uint8_t* b, size_t bytes) {
    double sum = 0.0;
    for (size_t i = 0; i < bytes; ++i) {
        const int d = (int)a[i] - (int)b[i];
        sum += d < 0 ? -d : d;
    }
    return sum / (double)bytes;
}

int main() {
    const int W = 1280, H = 720;
    const size_t kBytes  = (size_t)W * H * 4;
    const int    kFrames = 8;

    const char* scratchDir =
        "C:\\Temp\\claude\\C--Users-opari-OneDrive-Desktop-claude-code-camera-on-screen"
        "\\ee2d499c-5476-45b3-9689-71ae0faf6005\\scratchpad";

    char f6Path[MAX_PATH], midPath[MAX_PATH], f7Path[MAX_PATH];
    _snprintf_s(f6Path,  sizeof(f6Path),  _TRUNCATE, "%s\\f6.bmp",    scratchDir);
    _snprintf_s(midPath, sizeof(midPath), _TRUNCATE, "%s\\mid67.bmp", scratchDir);
    _snprintf_s(f7Path,  sizeof(f7Path),  _TRUNCATE, "%s\\f7.bmp",    scratchDir);

    // -----------------------------------------------------------------------
    // Generate 8 frames: smooth sinusoidal 2D texture translated 6 px/frame.
    // B=G=R=intensity(x,y,dx), A=255. dx = frameIndex * 6.
    // intensity(x,y,dx) = clamp(128 + 80*sin((x+dx)*0.04)*cos(y*0.04)
    //                               + 40*sin((x+dx)*0.011 + y*0.017), 0, 255)
    // -----------------------------------------------------------------------
    std::vector<std::vector<uint8_t>> frames(kFrames, std::vector<uint8_t>(kBytes, 0u));
    for (int fi = 0; fi < kFrames; ++fi) {
        const int dx = fi * 6;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const int idx = (y * W + x) * 4;
                const double raw = 128.0
                    + 80.0 * std::sin((x + dx) * 0.04) * std::cos(y * 0.04)
                    + 40.0 * std::sin((x + dx) * 0.011 + y * 0.017);
                const int iv = (int)raw;
                const uint8_t intensity = (uint8_t)(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
                frames[fi][idx + 0] = intensity; // B
                frames[fi][idx + 1] = intensity; // G
                frames[fi][idx + 2] = intensity; // R
                frames[fi][idx + 3] = 255;       // A
            }
        }
    }

    // -----------------------------------------------------------------------
    // Start FRUC.
    // -----------------------------------------------------------------------
    Fruc fruc;
    if (!fruc.Start(W, H)) {
        std::printf("Start failed: %s\n", fruc.LastError().c_str());
        return 1;
    }
    std::printf("Start ok\n");

    // -----------------------------------------------------------------------
    // Submit frames 0..7. Time each call; exclude first (i==0, warm-up).
    // Mid between frame6 and frame7 is produced when submitting frame7 (i==7).
    // -----------------------------------------------------------------------
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);

    double totalMs    = 0.0;
    int    timedCalls = 0;
    std::vector<uint8_t> mid67;

    for (int i = 0; i < kFrames; ++i) {
        std::vector<uint8_t> outMid;
        bool hasMid = false;

        QueryPerformanceCounter(&t0);
        const bool ok = fruc.Submit(frames[i].data(), W, H, outMid, hasMid);
        QueryPerformanceCounter(&t1);

        if (!ok) {
            std::printf("Submit frame %d failed: %s\n", i, fruc.LastError().c_str());
            fruc.Stop();
            return 1;
        }

        const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;

        if (i == 0) {
            std::printf("Frame 0: hasMid=%s (warm-up, not timed)\n",
                        hasMid ? "true" : "false");
        } else {
            totalMs += ms;
            ++timedCalls;
            std::printf("Frame %d: hasMid=%s, %.3f ms\n", i,
                        hasMid ? "true" : "false", ms);
        }

        // Mid between frame6 and frame7 is produced when we submit frame7 (i==7)
        if (i == 7 && hasMid && !outMid.empty()) {
            mid67 = outMid;
            std::printf("  -> mid67 captured (%zu bytes)\n", mid67.size());
        }
    }

    const double avgMs = timedCalls > 0 ? totalMs / (double)timedCalls : 0.0;
    std::printf("\nSubmit avg (frames 1-7): %.2f ms\n", avgMs);

    // -----------------------------------------------------------------------
    // Analyse mid67.
    // -----------------------------------------------------------------------
    if (mid67.empty()) {
        std::printf("WARNING: mid67 not captured (hasMid was false when submitting frame7)\n");
    } else {
        // 1. % bytes identical vs frame6 and frame7
        const double midVsF6 = ByteMatchFraction(mid67.data(), frames[6].data(), kBytes) * 100.0;
        const double midVsF7 = ByteMatchFraction(mid67.data(), frames[7].data(), kBytes) * 100.0;
        std::printf("%% bytes identical: mid vs f6 = %.2f%%\n", midVsF6);
        std::printf("%% bytes identical: mid vs f7 = %.2f%%\n", midVsF7);

        // 2. Mean absolute per-byte difference (0-255 scale).
        //    A repeated frame => one of mid-fX is ~0. True interpolation => both
        //    small-but-nonzero and each roughly half of f6-f7.
        const double madMidF6 = MeanAbsDiff(mid67.data(), frames[6].data(), kBytes);
        const double madMidF7 = MeanAbsDiff(mid67.data(), frames[7].data(), kBytes);
        const double madF6F7  = MeanAbsDiff(frames[6].data(), frames[7].data(), kBytes);
        std::printf("meanAbsDiff mid-f6 = %.3f\n", madMidF6);
        std::printf("meanAbsDiff mid-f7 = %.3f\n", madMidF7);
        std::printf("meanAbsDiff f6-f7  = %.3f  (full motion; mid-to-each should be ~half)\n",
                    madF6F7);
    }

    // -----------------------------------------------------------------------
    // Write BMPs: f6.bmp, mid67.bmp, f7.bmp.
    // -----------------------------------------------------------------------
    const bool f6Ok  = WriteBmp(f6Path,  frames[6].data(), W, H);
    const bool midOk = !mid67.empty() && WriteBmp(midPath, mid67.data(), W, H);
    const bool f7Ok  = WriteBmp(f7Path,  frames[7].data(), W, H);

    std::printf("\nBMP paths:\n");
    std::printf("  f6:    %s (%s)\n", f6Path,  f6Ok  ? "ok" : "WRITE FAILED");
    std::printf("  mid67: %s (%s)\n", midPath,
                midOk ? "ok" : (mid67.empty() ? "no mid data" : "WRITE FAILED"));
    std::printf("  f7:    %s (%s)\n", f7Path,  f7Ok  ? "ok" : "WRITE FAILED");

    fruc.Stop();
    std::printf("\nDone. Exit 0.\n");
    return 0;
}
