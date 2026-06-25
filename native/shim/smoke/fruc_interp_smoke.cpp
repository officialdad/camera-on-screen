// Task 3: GPU integration smoke -- drives Fruc::Interpolate on two gradient frames,
// dumps BMP files, and measures per-call latency.
//
// Frame layout (1280x720 BGRA):
//   prev = horizontal red gradient: pixel at (x,y) has B=0, G=0, R=x*255/(W-1), A=255
//   cur  = same gradient shifted +40 px right (wraps, so col x maps to R=(x+40)%W*255/(W-1))
//   mid  = Fruc::Interpolate(prev, cur) -- should show ~+20px shift if interpolation works
//
// Build: native\shim\smoke\build_fruc_interp_smoke.bat
// Run:
//   $env:COS_FRUC_RUNTIME_DIR="<OF_SDK>\NvOFFRUC\NvOFFRUCSample\bin\win64"
//   native\shim\smoke\fruc_interp_smoke.exe
//
// Output:
//   prev.bmp / mid.bmp / cur.bmp in the session scratchpad dir (printed to stdout)
//   Interpolate avg latency in ms
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
        uint16_t bfType;       // 'BM'
        uint32_t bfSize;
        uint16_t bfReserved1;
        uint16_t bfReserved2;
        uint32_t bfOffBits;
    };
    struct BmpInfoHeader {
        uint32_t biSize;
        int32_t  biWidth;
        int32_t  biHeight;     // positive = bottom-up stored
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
    fh.bfType      = 0x4D42; // 'BM'
    fh.bfOffBits   = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    fh.bfSize      = fh.bfOffBits + (uint32_t)imageSize;

    BmpInfoHeader ih{};
    ih.biSize       = sizeof(BmpInfoHeader);
    ih.biWidth      = width;
    ih.biHeight     = -height;  // negative = top-down (no row flip needed)
    ih.biPlanes     = 1;
    ih.biBitCount   = 32;
    ih.biCompression = 0;       // BI_RGB
    ih.biSizeImage  = (uint32_t)imageSize;

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
    for (size_t i = 0; i < bytes; ++i) same += (a[i] == b[i]) ? 1 : 0;
    return (double)same / (double)bytes;
}

int main() {
    const int W = 1280, H = 720;
    const size_t kBytes = (size_t)W * H * 4;

    const char* scratchDir =
        "C:\\Temp\\claude\\C--Users-opari-OneDrive-Desktop-claude-code-camera-on-screen"
        "\\ee2d499c-5476-45b3-9689-71ae0faf6005\\scratchpad";

    // Build output paths
    char prevPath[MAX_PATH], midPath[MAX_PATH], curPath[MAX_PATH];
    _snprintf_s(prevPath, sizeof(prevPath), _TRUNCATE, "%s\\prev.bmp", scratchDir);
    _snprintf_s(midPath,  sizeof(midPath),  _TRUNCATE, "%s\\mid.bmp",  scratchDir);
    _snprintf_s(curPath,  sizeof(curPath),  _TRUNCATE, "%s\\cur.bmp",  scratchDir);

    // -----------------------------------------------------------------------
    // Build gradient frames.
    // prev: R = x*255/(W-1), B=G=0, A=255 (pure red gradient left-to-right)
    // cur:  same gradient shifted +40 px right (wraps at W)
    // -----------------------------------------------------------------------
    std::vector<uint8_t> prev(kBytes), cur(kBytes);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const uint8_t rPrev = (uint8_t)(x * 255 / (W - 1));
            const uint8_t rCur  = (uint8_t)(((x + 40) % W) * 255 / (W - 1));
            const int idx = (y * W + x) * 4;
            // BGRA order in memory
            prev[idx + 0] = 0;       // B
            prev[idx + 1] = 0;       // G
            prev[idx + 2] = rPrev;   // R
            prev[idx + 3] = 255;     // A

            cur[idx + 0] = 0;
            cur[idx + 1] = 0;
            cur[idx + 2] = rCur;
            cur[idx + 3] = 255;
        }
    }

    // -----------------------------------------------------------------------
    // Start FRUC.
    // -----------------------------------------------------------------------
    Fruc fruc;
    if (!fruc.Start(W, H)) {
        std::printf("BLOCKED: Start failed: %s\n", fruc.LastError().c_str());
        return 1;
    }
    std::printf("Start ok\n");

    // -----------------------------------------------------------------------
    // Warm-up run (not timed).
    // -----------------------------------------------------------------------
    std::vector<uint8_t> midWarmup;
    if (!fruc.Interpolate(prev.data(), cur.data(), W, H, midWarmup)) {
        std::printf("BLOCKED: Interpolate (warm-up) failed: %s\n", fruc.LastError().c_str());
        fruc.Stop();
        return 1;
    }

    // -----------------------------------------------------------------------
    // Timed runs (30 iterations; avg).
    // -----------------------------------------------------------------------
    const int kRuns = 30;
    std::vector<uint8_t> midOut;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    double totalMs = 0.0;
    bool interpOk = true;

    for (int i = 0; i < kRuns; ++i) {
        QueryPerformanceCounter(&t0);
        const bool ok = fruc.Interpolate(prev.data(), cur.data(), W, H, midOut);
        QueryPerformanceCounter(&t1);
        if (!ok) {
            std::printf("BLOCKED: Interpolate run %d failed: %s\n", i, fruc.LastError().c_str());
            interpOk = false;
            break;
        }
        totalMs += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    }

    if (!interpOk) {
        fruc.Stop();
        return 1;
    }

    const double avgMs = totalMs / kRuns;
    std::printf("Interpolate ok, avg %.2f ms\n", avgMs);

    // -----------------------------------------------------------------------
    // Write BMPs.
    // -----------------------------------------------------------------------
    const bool prevOk = WriteBmp(prevPath, prev.data(), W, H);
    const bool midOk  = WriteBmp(midPath,  midOut.data(), W, H);
    const bool curOk  = WriteBmp(curPath,  cur.data(), W, H);

    std::printf("BMP paths:\n");
    std::printf("  prev: %s (%s)\n", prevPath, prevOk ? "ok" : "WRITE FAILED");
    std::printf("  mid:  %s (%s)\n", midPath,  midOk  ? "ok" : "WRITE FAILED");
    std::printf("  cur:  %s (%s)\n", curPath,  curOk  ? "ok" : "WRITE FAILED");

    // -----------------------------------------------------------------------
    // Byte-stat comparison: mid vs prev, mid vs cur.
    // -----------------------------------------------------------------------
    if (!midOut.empty()) {
        const double midVsPrev = ByteMatchFraction(midOut.data(), prev.data(), kBytes) * 100.0;
        const double midVsCur  = ByteMatchFraction(midOut.data(), cur.data(),  kBytes) * 100.0;
        std::printf("Byte stats: mid vs prev = %.2f%% identical, mid vs cur = %.2f%% identical\n",
                    midVsPrev, midVsCur);
        if (midVsPrev > 99.0)
            std::printf("  NOTE: mid is effectively identical to prev (frame repetition / no-op?)\n");
        else if (midVsCur > 99.0)
            std::printf("  NOTE: mid is effectively identical to cur (frame repetition?)\n");
        else
            std::printf("  NOTE: mid differs from both inputs (interpolation produced new content)\n");
    }

    fruc.Stop();
    std::printf("Done. Exit 0.\n");
    return 0;
}
