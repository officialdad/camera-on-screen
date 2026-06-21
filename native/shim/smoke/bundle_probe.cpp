// Headless bundled-discovery probe: confirms BOTH Maxine effect resolvers find a runtime
// bundled at <exeDir>\maxine with NO COS_* env vars set, and that NvVFX_Load / NvAR_Load
// succeed against the co-versioned bundle (proves models present + TRT/CUDA co-version holds).
//
// Build into the App output dir so ShimModuleDir() (== this exe's dir) resolves <exeDir>\maxine.
// Run with all COS_* env vars UNSET — exercises the app-relative tier specifically.
//
// Expected output (exit 0):
//   VFX  Probe: AVAILABLE (GreenScreen available)
//   AR   Probe: AVAILABLE (...)
#include <cstdio>
#include <string>
#include "../aigs.h"
#include "../eyecontact.h"

int main() {
    std::string d1, d2;
    bool vfx = Aigs::Probe(d1);
    std::printf("VFX  Probe: %s (%s)\n", vfx ? "AVAILABLE" : "UNAVAILABLE", d1.c_str());
    bool ar = EyeContact::Probe(d2);
    std::printf("AR   Probe: %s (%s)\n", ar ? "AVAILABLE" : "UNAVAILABLE", d2.c_str());
    return (vfx && ar) ? 0 : 1;
}
