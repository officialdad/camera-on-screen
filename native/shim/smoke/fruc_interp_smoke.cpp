// Task 3: GPU integration smoke -- drives Fruc::Submit over a 6-frame sequence
// of a moving red bar, dumps BMP files, and reports byte/color statistics.
//
// Frame layout (1280x720 BGRA):
//   Frame i: solid RED bar (B=0,G=0,R=255,A=255) at x=[200+i*80, 200+i*80+80),
//            all other pixels black (0,0,0,255).
//   Frames 0..5 submitted sequentially via Fruc::Submit (streaming API).
//   Mid between frame3 and frame4 is the analysis target.
//
// Build: native\shim\smoke\build_fruc_interp_smoke.bat
// Run:
//   $env:COS_FRUC_RUNTIME_DIR="<OF_SDK>\NvOFFRUC\NvOFFRUCSample\bin\win64"
//   native\shim\smoke\fruc_interp_smoke.exe
//
// Output:
//   f3.bmp / mid34.bmp / f4.bmp in the session scratchpad dir (paths printed to stdout)
//   Submit avg latency in ms (excluding first/warm-up call)
//   Byte-identical %, non-black avg R/G/B, mid bar x-range
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

int main() {
    const int W = 1280, H = 720;
    const size_t kBytes  = (size_t)W * H * 4;
    const int    kFrames = 6;

    const char* scratchDir =
        "C:\\Temp\\claude\\C--Users-opari-OneDrive-Desktop-claude-code-camera-on-screen"
        "\\ee2d499c-5476-45b3-9689-71ae0faf6005\\scratchpad";

    char f3Path[MAX_PATH], midPath[MAX_PATH], f4Path[MAX_PATH];
    _snprintf_s(f3Path,  sizeof(f3Path),  _TRUNCATE, "%s\\f3.bmp",    scratchDir);
    _snprintf_s(midPath, sizeof(midPath), _TRUNCATE, "%s\\mid34.bmp", scratchDir);
    _snprintf_s(f4Path,  sizeof(f4Path),  _TRUNCATE, "%s\\f4.bmp",    scratchDir);

    // -----------------------------------------------------------------------
    // Generate 6 frames: solid RED bar (B=0, G=0, R=255, A=255) on black
    // (B=0, G=0, R=0, A=255). BGRA byte order in memory.
    // Frame i: bar at x = [200 + i*80, 200 + i*80 + 80).
    //   Frame 0: x[200, 280)   Frame 3: x[440, 520)
    //   Frame 1: x[280, 360)   Frame 4: x[520, 600)
    //   Frame 2: x[360, 440)   Frame 5: x[600, 680)
    // -----------------------------------------------------------------------
    std::vector<std::vector<uint8_t>> frames(kFrames, std::vector<uint8_t>(kBytes, 0u));
    for (int fi = 0; fi < kFrames; ++fi) {
        const int barX0 = 200 + fi * 80;
        const int barX1 = barX0 + 80;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const int idx = (y * W + x) * 4;
                if (x >= barX0 && x < barX1) {
                    frames[fi][idx + 0] = 0;    // B
                    frames[fi][idx + 1] = 0;    // G
                    frames[fi][idx + 2] = 255;  // R
                    frames[fi][idx + 3] = 255;  // A
                } else {
                    frames[fi][idx + 0] = 0;    // B
                    frames[fi][idx + 1] = 0;    // G
                    frames[fi][idx + 2] = 0;    // R
                    frames[fi][idx + 3] = 255;  // A
                }
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
    // Submit frames 0..5. Time each call; exclude first (i==0, warm-up).
    // Mid between frame3 and frame4 is produced when submitting frame4 (i==4).
    // -----------------------------------------------------------------------
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);

    double totalMs    = 0.0;
    int    timedCalls = 0;
    std::vector<uint8_t> mid34;

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

        // Mid between frame3 and frame4 is produced when we submit frame4 (i==4)
        if (i == 4 && hasMid && !outMid.empty()) {
            mid34 = outMid;
            std::printf("  -> mid34 captured (%zu bytes)\n", mid34.size());
        }
    }

    const double avgMs = timedCalls > 0 ? totalMs / (double)timedCalls : 0.0;
    std::printf("\nSubmit avg (frames 1-5): %.2f ms\n", avgMs);

    // -----------------------------------------------------------------------
    // Analyse mid34.
    // -----------------------------------------------------------------------
    if (mid34.empty()) {
        std::printf("WARNING: mid34 not captured (hasMid was false when submitting frame4)\n");
    } else {
        // 1. % bytes identical vs frame3 and frame4
        const double midVsF3 = ByteMatchFraction(mid34.data(), frames[3].data(), kBytes) * 100.0;
        const double midVsF4 = ByteMatchFraction(mid34.data(), frames[4].data(), kBytes) * 100.0;
        std::printf("%% bytes identical: mid vs frame3 = %.2f%%\n", midVsF3);
        std::printf("%% bytes identical: mid vs frame4 = %.2f%%\n", midVsF4);

        // 2. Color-order check: avg R/G/B over non-black pixels (max(B,G,R) > 30).
        //    BGRA layout: idx+0=B, idx+1=G, idx+2=R, idx+3=A.
        //    Red preserved => R high, B~0.  Swapped => B high, R~0.
        double sumR = 0.0, sumG = 0.0, sumB = 0.0;
        size_t nonBlack = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const int     idx = (y * W + x) * 4;
                const uint8_t b   = mid34[idx + 0];
                const uint8_t g   = mid34[idx + 1];
                const uint8_t r   = mid34[idx + 2];
                const uint8_t mx  = (b > g ? b : g);
                const uint8_t mx2 = (mx > r ? mx : r);
                if (mx2 > 30) {
                    sumB += b;
                    sumG += g;
                    sumR += r;
                    ++nonBlack;
                }
            }
        }
        if (nonBlack > 0) {
            std::printf("mid non-black avg: R=%.1f G=%.1f B=%.1f (over %zu pixels)\n",
                        sumR / (double)nonBlack,
                        sumG / (double)nonBlack,
                        sumB / (double)nonBlack,
                        nonBlack);
        } else {
            std::printf("mid non-black avg: no non-black pixels found\n");
        }

        // 3. x-range of columns containing non-black pixels in mid.
        //    Frame3 bar = x[440,520), frame4 bar = x[520,600).
        //    Midpoint interpolation should show energy around x[480,560) or spanning both bars.
        int minX = W, maxX = -1;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const int     idx = (y * W + x) * 4;
                const uint8_t b   = mid34[idx + 0];
                const uint8_t g   = mid34[idx + 1];
                const uint8_t r   = mid34[idx + 2];
                const uint8_t mx  = (b > g ? b : g);
                const uint8_t mx2 = (mx > r ? mx : r);
                if (mx2 > 30) {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                }
            }
        }
        if (maxX >= 0) {
            std::printf("mid bar x-range: x[%d, %d] "
                        "(frame3 bar=[440,520), frame4 bar=[520,600))\n",
                        minX, maxX);
        } else {
            std::printf("mid bar x-range: no non-black columns found\n");
        }
    }

    // -----------------------------------------------------------------------
    // Write BMPs: f3.bmp, mid34.bmp, f4.bmp.
    // -----------------------------------------------------------------------
    const bool f3Ok  = WriteBmp(f3Path,  frames[3].data(), W, H);
    const bool midOk = !mid34.empty() && WriteBmp(midPath, mid34.data(), W, H);
    const bool f4Ok  = WriteBmp(f4Path,  frames[4].data(), W, H);

    std::printf("\nBMP paths:\n");
    std::printf("  f3:    %s (%s)\n", f3Path,  f3Ok  ? "ok" : "WRITE FAILED");
    std::printf("  mid34: %s (%s)\n", midPath,
                midOk ? "ok" : (mid34.empty() ? "no mid data" : "WRITE FAILED"));
    std::printf("  f4:    %s (%s)\n", f4Path,  f4Ok  ? "ok" : "WRITE FAILED");

    fruc.Stop();
    std::printf("\nDone. Exit 0.\n");
    return 0;
}
