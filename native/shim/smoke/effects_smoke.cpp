// Dev-box smoke (needs VFX SDK + RTX GPU). Build with the shim's include/lib setup.
// Verifies ArtifactReduction and SuperRes can start and process synthetic frames.
//
// Build (Developer PowerShell for VS 2022, from repo root):
//   $env:COS_VFX_SDK_DIR = "C:\path\to\VideoFX"
//   cl /EHsc /std:c++17 ^
//      /I "%COS_VFX_SDK_DIR%\nvvfx\include" ^
//      /DCOS_HAS_MAXINE ^
//      native\shim\smoke\effects_smoke.cpp ^
//      native\shim\artifactreduction.cpp ^
//      native\shim\superres.cpp ^
//      native\shim\vfx_paths.cpp native\shim\paths.cpp ^
//      "%COS_VFX_SDK_DIR%\nvvfx\src\nvVideoEffectsProxy.cpp" ^
//      "%COS_VFX_SDK_DIR%\nvvfx\src\nvCVImageProxy.cpp"
//   .\effects_smoke.exe
//
// Expected output (RTX GPU with models installed):
//   AR ProcessFrame: ok ()
//   effects_smoke AR OK
//   SR ProcessFrame: ok -> 1280x960   (requires nvvfxvideosuperres NGC feature + models)
// Without GPU/models, Probe prints the reason and exits 0 (deferred to human gate).
#include <cassert>
#include <cstdio>
#include <vector>
#include "../artifactreduction.h"
#include "../superres.h"

int main() {
    {
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
    }
    {
        std::string d;
        if (SuperRes::Probe(d)) {
            SuperRes sr; assert(sr.Start(20));
            std::vector<uint8_t> in(640 * 480 * 4, 64), out; int ow = 0, oh = 0;
            bool ok = sr.ProcessFrame(in.data(), 640, 480, out, ow, oh);
            std::printf("SR ProcessFrame: %s -> %dx%d\n", ok ? "ok" : "fail", ow, oh);
            assert(ok && ow == 1280 && oh == 960);
            sr.Stop();
        } else { std::printf("SR unavailable: %s\n", d.c_str()); }
    }
    return 0;
}
