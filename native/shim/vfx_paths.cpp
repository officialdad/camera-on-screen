#include "vfx_paths.h"
#ifdef COS_HAS_MAXINE
#include <windows.h>
#include "paths.h"

// The proxy stub (nvVideoEffectsProxy.cpp) declares this extern; we own the single
// definition here (moved out of aigs.cpp). Non-null => dir holding NVVideoEffects.dll,
// which the proxy passes to SetDllDirectory before LoadLibrary.
char* g_nvVFXSDKPath = nullptr;

namespace vfx {
bool ResolveSdkPaths(std::string& binDir, std::string& modelDir, std::string& err) {
    char buf[1024] = {0};
    DWORD n = GetEnvironmentVariableA("COS_VFX_RUNTIME_DIR", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        std::string root(buf, n);
        if (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
        binDir = root; modelDir = root + "\\models"; return true;
    }
    n = GetEnvironmentVariableA("COS_VFX_SDK_DIR", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        std::string root(buf, n);
        if (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
        binDir = root + "\\bin"; modelDir = root + "\\bin\\models"; return true;
    }
    std::string appDir = ShimModuleDir();
    if (!appDir.empty()) {
        std::string maxine = appDir + "\\maxine";
        if (DirExists(maxine)) { binDir = maxine; modelDir = maxine + "\\models\\vfx"; return true; }
    }
    err = "VFX runtime not found: set COS_VFX_RUNTIME_DIR or bundle maxine\\ beside the app";
    return false;
}
void PointProxiesAt(const std::string& binDir) {
    static std::string s_bin;
    s_bin = binDir;
    g_nvVFXSDKPath = const_cast<char*>(s_bin.c_str());
}
}
#else
namespace vfx {
bool ResolveSdkPaths(std::string&, std::string&, std::string& err) { err = "Maxine SDK not built in"; return false; }
void PointProxiesAt(const std::string&) {}
}
#endif
