#define COS_EXPORTS
#include "shim.h"
#include "capture.h"
#include "aigs.h"
#include "eyecontact.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <mfapi.h>

namespace {
    std::atomic<bool> g_running{false};
    std::atomic<bool> g_mfStarted{false};
    CosParams   g_params{};
    Capture     g_capture;
    std::string g_cameraId;
}

COS_API int cos_init(void* /*d3d11_device*/) {
    // MFStartup exactly once. cos_init runs before enumerate/start (the App calls
    // Init() first), so this guarantees MF is live for every later MF call.
    bool expected = false;
    if (g_mfStarted.compare_exchange_strong(expected, true)) {
        if (FAILED(MFStartup(MF_VERSION))) {
            g_mfStarted.store(false);
            return 0;
        }
    }
    return 1;
}

namespace {
    // Copies a UTF-8 string into a fixed 128-byte slot: up to 127 bytes + NUL,
    // zero-filling the rest. The 128 stride MUST stay in sync with
    // PInvokeShim.ReadUtf8 (a prior review flagged this as a cross-file sync risk).
    void CopyToSlot(char* slot, const std::string& s) {
        const size_t cap = 127;
        size_t n = s.size() < cap ? s.size() : cap;
        std::memcpy(slot, s.data(), n);
        std::memset(slot + n, 0, 128 - n);
    }
}

COS_API int cos_enumerate_cameras(char* ids, char* names, int max) {
    if (!ids || !names || max <= 0) return 0;
    auto cams = Capture::Enumerate();
    int n = static_cast<int>(cams.size());
    if (n > max) n = max;
    for (int i = 0; i < n; ++i) {
        CopyToSlot(ids   + static_cast<size_t>(i) * 128, cams[i].id);
        CopyToSlot(names + static_cast<size_t>(i) * 128, cams[i].name);
    }
    return n;
}

COS_API void cos_set_params(const CosParams* p) {
    if (!p) return;
    g_params = *p;
    g_cameraId = p->camera_id ? p->camera_id : "";
    g_capture.SetGreenScreen(p->green_screen_enabled != 0);
    g_capture.SetMatteParams(p->green_screen_expand, p->green_screen_feather);
    g_capture.SetEyeContact(p->eye_contact_enabled != 0);
}

COS_API void cos_start(void) { g_capture.Start(g_cameraId); g_running = true; }
COS_API void cos_stop(void)  { g_capture.Stop(); g_running = false; }

COS_API void cos_get_status(CosStatus* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->running = g_running ? 1 : 0;
    out->fps = g_running ? g_capture.MeasuredFps() : 0.0;
    out->green_screen_active = g_capture.GreenScreenActive() ? 1 : 0;
    out->eye_contact_active = g_capture.EyeContactActive() ? 1 : 0;
    std::string err = g_capture.GreenScreenError();
    if (err.empty()) err = g_capture.EyeContactError();
    if (!err.empty()) {
        size_t n = err.size() < 255 ? err.size() : 255;
        std::memcpy(out->error, err.data(), n);
        out->error[n] = '\0';
    }
}

COS_API int cos_get_frame(uint8_t* dst, int* width, int* height, int dst_capacity) {
    if (!dst || dst_capacity <= 0) return 0;
    std::vector<uint8_t> frame;
    int w = 0, h = 0;
    if (!g_capture.LatestFrame(frame, w, h)) return 0;
    if (static_cast<int>(frame.size()) > dst_capacity) return 0;
    std::memcpy(dst, frame.data(), frame.size());
    if (width)  *width  = w;
    if (height) *height = h;
    return 1;
}

COS_API int cos_query_capabilities(CosCaps* out) {
    if (!out) return 0;
    std::memset(out, 0, sizeof(*out));

    std::string gsDetail;
    bool gsOk = Aigs::Probe(gsDetail);
    out->green_screen_available = gsOk ? 1 : 0;
    size_t gn = gsDetail.size() < 255 ? gsDetail.size() : 255;
    std::memcpy(out->detail, gsDetail.data(), gn);
    out->detail[gn] = '\0';

    std::string ecDetail;
    bool ecOk = EyeContact::Probe(ecDetail);
    out->eye_contact_available = ecOk ? 1 : 0;
    size_t en = ecDetail.size() < 255 ? ecDetail.size() : 255;
    std::memcpy(out->ec_detail, ecDetail.data(), en);
    out->ec_detail[en] = '\0';

    // Return 1 if either effect is available (the managed side reads the per-gate ints).
    return (gsOk || ecOk) ? 1 : 0;
}

COS_API void cos_shutdown(void) {
    g_capture.Stop();
    g_running = false;
    bool expected = true;
    if (g_mfStarted.compare_exchange_strong(expected, false)) {
        MFShutdown();
    }
}
