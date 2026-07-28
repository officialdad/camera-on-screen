// Linux effects smoke (needs RTX GPU + camera + Maxine runtimes): drives the shim C ABI
// exactly like the app — init -> query_capabilities -> set_params(both effects on) ->
// start -> poll status/frames. Proved decisive for the Phase 4 loader debugging; also the
// runtime verify gate for a produced Linux bundle (point it at a dir whose <shimdir>/maxine
// tiers resolve, COS_* unset — the Linux twin of bundle_probe).
//
// Build (repo root; the -L dir's libCameraOnScreen.Shim.so is what gets exercised):
//   g++ -std=c++17 -o effects_drive native/shim/smoke/effects_drive_linux.cpp \
//       -L native/shim/build -lCameraOnScreen.Shim -Wl,-rpath,$PWD/native/shim/build
// Run:
//   export COS_VFX_RUNTIME_DIR=... COS_AR_RUNTIME_DIR=...   # or unset -> bundled maxine/ tier
//   ./effects_drive
// Expect: probe gs=1 ec=1, then gs_active=1 ec_active=1 alphaVaried=1 (early-exits on success;
// first run engine-load can take ~30-60 s). -DSKIP_PROBE reproduces the raw worker-only
// ordering trap (first NvVFX_Load must precede AR/alloc use — see CLAUDE.md) and MUST fail.
#include <cstdio>
#include <vector>
#include <thread>
#include <chrono>
#include "../shim.h"

int main() {
    if (!cos_init(nullptr)) { std::printf("init failed\n"); return 1; }
#ifndef SKIP_PROBE
    CosCaps caps{};
    cos_query_capabilities(&caps);
    std::printf("probe: gs=%d (%s) ec=%d (%s)\n", caps.green_screen_available, caps.detail,
                caps.eye_contact_available, caps.ec_detail);
#endif
    CosParams p{};
    p.green_screen_enabled = 1;
    p.eye_contact_enabled = 1;
    cos_set_params(&p);
    cos_start();
    std::vector<uint8_t> buf(1920 * 1080 * 4);
    int frames = 0, alphaVaried = 0;
    bool pass = false;
    for (int s = 0; s < 90; ++s) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        int w = 0, h = 0;
        while (cos_get_frame(buf.data(), &w, &h, (int)buf.size())) {
            ++frames;
            // any non-opaque alpha = the green-screen matte was authored
            for (int i = 3; i < w * h * 4; i += 4)
                if (buf[i] != 0xFF) { alphaVaried = 1; break; }
        }
        CosStatus st{};
        cos_get_status(&st);
        std::printf("[%2ds] frames=%d gs_active=%d ec_active=%d alphaVaried=%d err='%s'\n",
                    s + 1, frames, st.green_screen_active, st.eye_contact_active, alphaVaried, st.error);
        std::fflush(stdout);
        if (st.green_screen_active && st.eye_contact_active && alphaVaried) { pass = true; break; }
    }
    cos_stop();
    cos_shutdown();
    std::printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
