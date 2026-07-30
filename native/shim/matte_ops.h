#pragma once
#include <cstdint>

// Matte post-processing shared by the green-screen engines (Maxine AIGS and ONNX —
// issue #24). All buffers are tightly packed (pitch == w). Dilate/Feather run in
// place on 'work', using 'tmp' as scratch; both must be w*h bytes.
namespace matte {

// Max amount of dilate/blur at slider value 1.0, in pixels.
constexpr int kMaxDilateRadius = 16;
constexpr int kMaxBlurRadius   = 16;

// Slider amount 0..1 -> pixel radius 0..maxRadius.
int RadiusFromAmount(double amount, int maxRadius);

// Separable morphological dilate (max filter).
void Dilate(uint8_t* work, uint8_t* tmp, int w, int h, int r);

// Separable box blur.
void Feather(uint8_t* work, uint8_t* tmp, int w, int h, int r);

// Apply the packed matte to BGRA in place: A = matte, RGB premultiplied by matte/255.
void CompositePremultiplied(uint8_t* bgra, const uint8_t* matteBuf, int w, int h);

} // namespace matte
