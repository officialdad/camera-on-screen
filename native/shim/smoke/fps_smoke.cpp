// Standalone smoke for FpsCounter. Build: cl /EHsc /I.. fps_smoke.cpp
#include <cassert>
#include <cmath>
#include <cstdio>
#include "../fps_counter.h"

static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

int main() {
    FpsCounter c;
    assert(near(c.Sample(0.0, 0), 0.0));        // first sample primes, no estimate yet
    assert(near(c.Sample(1.0, 30), 30.0));      // 30 frames in 1.0s -> 30 fps
    assert(near(c.Sample(1.2, 45), 30.0));      // 0.2s < min interval -> keep last estimate
    assert(near(c.Sample(2.0, 90), 60.0));      // window 1.0->2.0: 60 frames / 1.0s (the 1.2 sub-interval call did not move the baseline)
    c.Reset();
    assert(near(c.Sample(5.0, 999), 0.0));      // reset re-primes
    std::puts("fps_smoke OK");
    return 0;
}
