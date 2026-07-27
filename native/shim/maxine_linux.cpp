// Linux-only Maxine runtime loading (issue #27 Phase 4). Two jobs:
//   1. PreloadMaxineClosure — the SDK .so set ships with no RUNPATH, so nothing resolves
//      by search path; dlopen everything by absolute path with RTLD_GLOBAL to a fixpoint.
//   2. GetOSInfo interposer — libVideoFXLocal.so segfaults parsing /etc/lsb-release on
//      non-Ubuntu distros; exporting the same mangled symbol from an earlier-loaded module
//      overrides it (verified fix, spec §6 Spike A).
#ifndef _WIN32
#include "maxine_linux.h"
#include <dirent.h>
#include <dlfcn.h>
#include <string>
#include <vector>

namespace {
void CollectSos(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string full = dir + "/" + name;
        if (e->d_type == DT_DIR) { CollectSos(full, out); continue; }
        // Skip *Triton* (server variant; pulls deps deliberately not shipped).
        if (name.find(".so") != std::string::npos && name.find("Triton") == std::string::npos)
            out.push_back(full);
    }
    closedir(d);
}
} // namespace

void PreloadMaxineClosure(const std::string& sdkRoot) {
    // .NET dlopens the shim RTLD_LOCAL; promote it to the global scope FIRST so the
    // exported GetOSInfo interposer below wins over libVideoFXLocal's own copy.
    Dl_info self{};
    if (dladdr(reinterpret_cast<void*>(&PreloadMaxineClosure), &self) && self.dli_fname)
        dlopen(self.dli_fname, RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);

    std::vector<std::string> libs;
    CollectSos(sdkRoot + "/lib", libs);
    CollectSos(sdkRoot + "/external", libs);
    CollectSos(sdkRoot + "/features", libs);
    // Fixpoint: a pass that loads at least one new lib may unblock others.
    std::vector<bool> loaded(libs.size(), false);
    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t i = 0; i < libs.size(); ++i) {
            if (loaded[i]) continue;
            if (dlopen(libs[i].c_str(), RTLD_NOW | RTLD_GLOBAL)) { loaded[i] = true; progress = true; }
        }
    }
    // Leftovers (unresolvable deps) are fine: the effect Probe reports the precise failure.
}

// libVideoFXLocal.so's exported GetOSInfo(key, out) parses /etc/lsb-release and SEGFAULTS
// on non-Ubuntu distros (CachyOS: DISTRIB_RELEASE="rolling"). The shim sits before the SDK
// libs in the dlopen scope, so exporting the same mangled symbol interposes it. Claim
// Ubuntu 22.04 (a supported OS); rc=0 = success. Used only for model-compatibility checks.
__attribute__((visibility("default")))
int GetOSInfo(const char* key, std::string& out) {
    const std::string k = key ? key : "";
    if (k == "ID") out = "ubuntu";
    else if (k.find("RELEASE") != std::string::npos || k.find("VERSION") != std::string::npos) out = "22.04";
    else out = "Ubuntu";
    return 0;
}
#endif // !_WIN32
