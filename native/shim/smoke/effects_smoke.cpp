// Dev-box smoke (needs VFX SDK + RTX GPU). Build with the shim's include/lib setup.
// Verifies ArtifactReduction can start and process a synthetic 1280x720 frame.
//
// Build (Developer PowerShell for VS 2022, from repo root):
//   $env:COS_VFX_SDK_DIR = "C:\path\to\VideoFX"
//   cl /EHsc /std:c++17 ^
//      /I "%COS_VFX_SDK_DIR%\nvvfx\include" ^
//      /DCOS_HAS_MAXINE ^
//      native\shim\smoke\effects_smoke.cpp ^
//      native\shim\artifactreduction.cpp ^
//      native\shim\vfx_paths.cpp native\shim\paths.cpp ^
//      "%COS_VFX_SDK_DIR%\nvvfx\src\nvVideoEffectsProxy.cpp" ^
//      "%COS_VFX_SDK_DIR%\nvvfx\src\nvCVImageProxy.cpp"
//   .\effects_smoke.exe
//
// Expected output (RTX GPU with AR models installed):
//   AR ProcessFrame: ok ()
//   effects_smoke AR OK
// Without GPU/models, Probe prints the reason and exits 0 (deferred to human gate).
#include <cassert>
#include <cstdio>
#include <vector>
#include "../artifactreduction.h"

int main() {
    std::string detail;
    if (!ArtifactReduction::Probe(detail)) { std::printf("AR unavailable: %s\n", detail.c_str()); return 0; }
    ArtifactReduction ar;
    assert(ar.Start());
    std::vector<uint8_t> frame(1280 * 720 * 4, 128);
    bool ok = ar.ProcessFrame(frame.data(), 1280, 720);
    std::printf("AR ProcessFrame: %s (%s)\n", ok ? "ok" : "fail", ar.LastError().c_str());
    assert(ok);
    ar.Stop();
    std::puts("effects_smoke AR OK");
    return 0;
}
