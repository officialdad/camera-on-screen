#define COS_EXPORTS
#include "shim.h"
#include <atomic>
#include <cstring>

namespace {
    std::atomic<bool> g_running{false};
    CosParams g_params{};
}

COS_API int cos_init(void* /*d3d11_device*/) { return 1; }

COS_API int cos_enumerate_cameras(char* /*ids*/, char* /*names*/, int /*max*/) {
    return 0; // Task 10 fills this from Media Foundation.
}

COS_API void cos_set_params(const CosParams* p) { if (p) g_params = *p; }
COS_API void cos_start(void) { g_running = true; }
COS_API void cos_stop(void) { g_running = false; }

COS_API void cos_get_status(CosStatus* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->running = g_running ? 1 : 0;
    out->fps = g_running ? 30.0 : 0.0;
}

COS_API void cos_shutdown(void) { g_running = false; }
