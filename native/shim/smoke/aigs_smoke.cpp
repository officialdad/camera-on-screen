// Headless smoke test for AIGS init. Build ad hoc (not part of the DLL).
// Run from a Developer PowerShell for VS 2022 with COS_VFX_SDK_DIR set, e.g.:
//
//   $env:COS_VFX_SDK_DIR = "C:\Users\opari\OneDrive\Desktop\claude-code\VideoFX"
//   cl /EHsc /std:c++17 /I "%COS_VFX_SDK_DIR%\nvvfx\include" /I "%COS_VFX_SDK_DIR%\features\nvvfxgreenscreen\include" /DCOS_HAS_MAXINE ^
//      native\shim\smoke\aigs_smoke.cpp native\shim\aigs.cpp ^
//      "%COS_VFX_SDK_DIR%\nvvfx\src\nvVideoEffectsProxy.cpp" "%COS_VFX_SDK_DIR%\nvvfx\src\nvCVImageProxy.cpp"
//   .\aigs_smoke.exe
//
// Expected output:
//   Probe: AVAILABLE (GreenScreen available)
//   Start: OK ()
#include <cstdio>
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
    a.Stop();
    return started ? 0 : 2;
}
