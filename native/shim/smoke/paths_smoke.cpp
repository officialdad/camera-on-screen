// Headless smoke test for paths.cpp (ShimModuleDir + DirExists). Pure Win32, no SDK.
// Build ad hoc from a Developer PowerShell for VS 2022:
//
//   cl /EHsc /std:c++17 native\shim\smoke\paths_smoke.cpp native\shim\paths.cpp
//   .\paths_smoke.exe
//
// Expected output (exit 0):
//   ShimModuleDir: <some existing absolute dir>
//   DirExists(self): PASS
//   DirExists(self\__nope__): PASS (correctly false)
#include <cstdio>
#include <string>
#include "../paths.h"

int main() {
    std::string dir = ShimModuleDir();
    std::printf("ShimModuleDir: %s\n", dir.c_str());
    if (dir.empty()) { std::printf("FAIL: empty dir\n"); return 1; }
    if (!DirExists(dir)) { std::printf("FAIL: own dir not found\n"); return 2; }
    std::printf("DirExists(self): PASS\n");
    if (DirExists(dir + "\\__nope__")) { std::printf("FAIL: ghost dir reported present\n"); return 3; }
    std::printf("DirExists(self\\__nope__): PASS (correctly false)\n");
    return 0;
}
