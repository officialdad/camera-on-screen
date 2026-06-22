// Dev/CI tool. Loads BOTH Maxine effects and runs one synthetic frame through each
// (so every DLL + model the runtime touches at load AND run time is pulled in), then
// enumerates every module loaded from <exe>\maxine and prints it relative to that dir.
//
// Two uses:
//   (1) TRACE  — run against a FULL (verbatim) <exe>\maxine to discover the DLL closure;
//                its stdout list seeds maxine-manifest.psd1's SharedDlls.
//   (2) GATE   — run against the PRODUCED (pruned) <exe>\maxine; exit 0 iff BOTH effects
//                loaded, proving the prune didn't drop a required DLL/model.
//
// Build INTO the App output dir so ShimModuleDir()==this exe's dir resolves <exe>\maxine.
// Run with all COS_* env vars UNSET to exercise the app-relative tier.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <windows.h>
#include <psapi.h>
#include "../aigs.h"
#include "../eyecontact.h"
#include "../paths.h"

static std::string ToLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

int main() {
    // 1. Load + run both effects (same thread = CUDA affinity satisfied).
    Aigs gs;
    EyeContact ec;
    const bool gsOk = gs.Start();
    const bool ecOk = ec.Start();
    const int W = 1280, H = 720;
    std::vector<uint8_t> frame((size_t)W * H * 4, (uint8_t)128);
    if (gsOk) gs.ProcessFrame(frame.data(), W, H, 0.0, 0.0);
    if (ecOk) ec.ProcessFrame(frame.data(), W, H);
    std::printf("# green-screen Start=%d  eye-contact Start=%d\n", gsOk ? 1 : 0, ecOk ? 1 : 0);
    if (!gsOk) std::printf("# GS error: %s\n", gs.LastError().c_str());
    if (!ecOk) std::printf("# EC error: %s\n", ec.LastError().c_str());

    // 2. Enumerate modules loaded from <exe>\maxine\ .
    const std::string root = ToLower(ShimModuleDir()) + "\\maxine\\";
    std::vector<HMODULE> mods(2048);
    DWORD needed = 0;
    if (!EnumProcessModulesEx(GetCurrentProcess(), mods.data(),
                              (DWORD)(mods.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL)) {
        std::printf("EnumProcessModulesEx failed: %lu\n", GetLastError());
        return 2;
    }
    const int n = (int)(needed / sizeof(HMODULE));
    std::vector<std::string> hits;
    for (int i = 0; i < n && i < (int)mods.size(); ++i) {
        wchar_t pathW[MAX_PATH];
        if (!GetModuleFileNameW(mods[i], pathW, MAX_PATH)) continue;
        char path[MAX_PATH * 2];
        if (WideCharToMultiByte(CP_UTF8, 0, pathW, -1, path, sizeof(path), nullptr, nullptr) <= 0) continue;
        const std::string p = ToLower(path);
        if (p.rfind(root, 0) == 0) hits.push_back(p.substr(root.size()));
    }
    std::sort(hits.begin(), hits.end());
    std::printf("# %zu modules loaded from maxine\\:\n", hits.size());
    for (const auto& h : hits) std::printf("%s\n", h.c_str());

    // 3. Clean up so worker objects release the CUDA stream before exit.
    gs.Stop();
    ec.Stop();
    return (gsOk && ecOk) ? 0 : 1;
}
