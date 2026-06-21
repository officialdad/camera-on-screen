#pragma once
#include <string>

// Directory containing the module whose compiled code calls this function — i.e. the shim
// DLL at runtime (CWD-independent; correct under Explorer double-click). UTF-8, no trailing
// slash. Returns "" on failure.
std::string ShimModuleDir();

// True iff `path` exists and is a directory.
bool DirExists(const std::string& path);
