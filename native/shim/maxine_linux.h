#pragma once
#ifndef _WIN32
#include <string>
// Loads every non-Triton .so under sdkRoot (lib/, external/, features/) by absolute path
// with RTLD_GLOBAL, iterating to a fixpoint (the SDK .so set has no RUNPATH; SONAME reuse
// then resolves later plain-name dlopens and DT_NEEDED). Safe to call repeatedly and for
// both SDK roots.
void PreloadMaxineClosure(const std::string& sdkRoot);
#endif
