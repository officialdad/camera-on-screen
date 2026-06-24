#pragma once
#include <string>

// Shared VFX-SDK runtime discovery for ALL VFX effects (Green Screen, Artifact
// Reduction, Super Resolution). Single source of truth — aigs.cpp uses these too.
// Model subdir is models\vfx in the bundled layout.
namespace vfx {
// Fills binDir (holds NVVideoEffects.dll) and modelDir. Order: COS_VFX_RUNTIME_DIR
// (flat) -> COS_VFX_SDK_DIR (legacy \bin) -> <shimDir>\maxine (bundled). false if none.
bool ResolveSdkPaths(std::string& binDir, std::string& modelDir, std::string& err);
// Points the VFX proxy global (g_nvVFXSDKPath, defined in vfx_paths.cpp) at binDir.
void PointProxiesAt(const std::string& binDir);
}
