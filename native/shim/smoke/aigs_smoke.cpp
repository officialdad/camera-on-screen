// Headless smoke test for AIGS init + ProcessFrame. Build ad hoc (not part of the DLL).
// Run from a Developer PowerShell for VS 2022 with COS_VFX_SDK_DIR set, e.g.:
//
//   $env:COS_VFX_SDK_DIR = "C:\path\to\VideoFX"
//   cl /EHsc /std:c++17 /I "%COS_VFX_SDK_DIR%\nvvfx\include" /I "%COS_VFX_SDK_DIR%\features\nvvfxgreenscreen\include" /DCOS_HAS_MAXINE ^
//      native\shim\smoke\aigs_smoke.cpp native\shim\aigs.cpp ^
//      "%COS_VFX_SDK_DIR%\nvvfx\src\nvVideoEffectsProxy.cpp" "%COS_VFX_SDK_DIR%\nvvfx\src\nvCVImageProxy.cpp"
//   .\aigs_smoke.exe
//
// Expected output:
//   Probe: AVAILABLE (GreenScreen available)
//   Start: OK ()
//   ProcessFrame: OK ()
#include <cstdio>
#include <cstdint>
#include <string>
#include "../aigs.h"

int main() {
    std::string detail;
    bool ok = Aigs::Probe(detail);
    std::printf("Probe: %s (%s)\n", ok ? "AVAILABLE" : "unavailable", detail.c_str());
    if (!ok) return 1;

    Aigs a;
    bool started = a.Start();
    std::printf("Start: %s (%s)\n", started ? "OK" : "FAIL", a.LastError().c_str());
    if (!started) return 2;

    // Minimal: a flat gray frame just exercises the pipeline end-to-end.
    int W = 640, H = 480;
    std::string buf(static_cast<size_t>(W) * H * 4, (char)128);
    bool ran = a.ProcessFrame(reinterpret_cast<uint8_t*>(&buf[0]), W, H);
    std::printf("ProcessFrame: %s (%s)\n", ran ? "OK" : "FAIL", a.LastError().c_str());

    a.Stop();
    return ran ? 0 : 3;
}
