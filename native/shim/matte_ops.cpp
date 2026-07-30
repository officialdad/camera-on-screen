#include "matte_ops.h"
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace matte {

int RadiusFromAmount(double amount, int maxRadius) {
    if (amount <= 0.0) return 0;
    if (amount >= 1.0) return maxRadius;
    return static_cast<int>(std::lround(amount * maxRadius));
}

void Dilate(uint8_t* work, uint8_t* tmp, int w, int h, int r) {
    if (r <= 0) return;
    for (int y = 0; y < h; ++y) {       // horizontal
        const uint8_t* srow = work + static_cast<size_t>(w) * y;
        uint8_t* trow = tmp + static_cast<size_t>(w) * y;
        for (int x = 0; x < w; ++x) {
            uint8_t mx = 0;
            const int x0 = std::max(0, x - r), x1 = std::min(w - 1, x + r);
            for (int k = x0; k <= x1; ++k) mx = std::max(mx, srow[k]);
            trow[x] = mx;
        }
    }
    for (int x = 0; x < w; ++x) {       // vertical
        for (int y = 0; y < h; ++y) {
            uint8_t mx = 0;
            const int y0 = std::max(0, y - r), y1 = std::min(h - 1, y + r);
            for (int k = y0; k <= y1; ++k) mx = std::max(mx, tmp[static_cast<size_t>(w) * k + x]);
            work[static_cast<size_t>(w) * y + x] = mx;
        }
    }
}

void Feather(uint8_t* work, uint8_t* tmp, int w, int h, int r) {
    if (r <= 0) return;
    const int win = 2 * r + 1;
    for (int y = 0; y < h; ++y) {       // horizontal
        const uint8_t* srow = work + static_cast<size_t>(w) * y;
        uint8_t* trow = tmp + static_cast<size_t>(w) * y;
        for (int x = 0; x < w; ++x) {
            int sum = 0;
            const int x0 = std::max(0, x - r), x1 = std::min(w - 1, x + r);
            for (int k = x0; k <= x1; ++k) sum += srow[k];
            trow[x] = static_cast<uint8_t>(sum / win);
        }
    }
    for (int x = 0; x < w; ++x) {       // vertical
        for (int y = 0; y < h; ++y) {
            int sum = 0;
            const int y0 = std::max(0, y - r), y1 = std::min(h - 1, y + r);
            for (int k = y0; k <= y1; ++k) sum += tmp[static_cast<size_t>(w) * k + x];
            work[static_cast<size_t>(w) * y + x] = static_cast<uint8_t>(sum / win);
        }
    }
}

void CompositePremultiplied(uint8_t* bgra, const uint8_t* matteBuf, int w, int h) {
    for (int y = 0; y < h; ++y) {
        const uint8_t* mrow = matteBuf + static_cast<size_t>(w) * y;
        uint8_t* prow = bgra + static_cast<size_t>(w) * 4 * y;
        for (int x = 0; x < w; ++x) {
            const unsigned a = mrow[x];
            uint8_t* px = prow + x * 4;
            px[0] = static_cast<uint8_t>((px[0] * a) / 255); // B
            px[1] = static_cast<uint8_t>((px[1] * a) / 255); // G
            px[2] = static_cast<uint8_t>((px[2] * a) / 255); // R
            px[3] = static_cast<uint8_t>(a);                 // A = matte
        }
    }
}

} // namespace matte
