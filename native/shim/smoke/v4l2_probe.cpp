// Linux ABI smoke: init -> enumerate -> query_capabilities -> optional 3s capture -> shutdown.
// Zero cameras is a PASS (exit 0) so CI runners without a webcam can gate on it; a present
// camera exercises the full frame path and reports measured fps.
#include "../shim.h"
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    if (!cos_init(nullptr)) { std::printf("FAIL cos_init\n"); return 1; }

    char ids[16 * 128] = {};
    char names[16 * 128] = {};
    const int n = cos_enumerate_cameras(ids, names, 16);
    std::printf("cameras: %d\n", n);
    for (int i = 0; i < n; ++i)
        std::printf("  [%d] id=%s name=%s\n", i, ids + i * 128, names + i * 128);

    CosCaps caps{};
    cos_query_capabilities(&caps);
    std::printf("caps: green_screen=%d (%s) eye_contact=%d (%s) super_res=%d frame_interp=%d (%s)\n",
                caps.green_screen_available, caps.detail,
                caps.eye_contact_available, caps.ec_detail,
                caps.super_res_available, caps.frame_interp_available, caps.fi_detail);

    if (n > 0) {
        CosParams p{};
        p.camera_id = ids; // first camera
        cos_set_params(&p);
        cos_start();
        std::vector<uint8_t> buf(static_cast<size_t>(1920) * 1080 * 4);
        int frames = 0, w = 0, h = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < until) {
            if (cos_get_frame(buf.data(), &w, &h, static_cast<int>(buf.size()))) ++frames;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CosStatus st{};
        cos_get_status(&st);
        cos_stop();
        std::printf("capture: %d frames in 3s at %dx%d, fps=%.1f\n", frames, w, h, st.fps);
        if (frames == 0) { std::printf("FAIL no frames from present camera\n"); cos_shutdown(); return 1; }
    }

    cos_shutdown();
    std::printf("PASS\n");
    return 0;
}
