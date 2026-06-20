#include "aigs.h"

#ifdef COS_HAS_MAXINE
// Defined here so the compiled proxy stub (nvVideoEffectsProxy.cpp) links.
// The proxy points SetDllDirectory at this path; it is populated in Task 2.
char* g_nvVFXSDKPath = nullptr;
// (Real Aigs method bodies are added in Tasks 2 and 3.)
#else
// ---- Passthrough stub: built when no SDK is configured. ----
Aigs::Aigs() = default;
Aigs::~Aigs() = default;
bool Aigs::Probe(std::string& detail) { detail = "Maxine SDK not built in"; return false; }
bool Aigs::Start() { lastError_ = "Maxine SDK not built in"; ready_ = false; return false; }
void Aigs::Stop() { ready_ = false; }
bool Aigs::ProcessFrame(uint8_t*, int, int) { return false; }
#endif
