# Linux Phase 2 — Shim `.so` + V4L2 Capture + Avalonia Real-Shim Wiring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Phase 2 of the Linux migration (#27): build the C-ABI shim as `libCameraOnScreen.Shim.so` with a V4L2 passthrough capture backend, and wire the Avalonia control panel to the real shim + JSON config persistence (#29), on the new CachyOS Linux dev box.

**Architecture:** The existing `Capture` class interface (`capture.h`) gets a second, Linux-only implementation file (`capture_v4l2.cpp`) compiled by a new CMake build; Windows keeps the untouched `vcxproj` + `capture.cpp`. Effects stay the existing `COS_HAS_*` passthrough stubs (Phase 4 adds Maxine Linux runtimes). The Avalonia app gains its own app-layer `PInvokeShim` (the WinUI project is NOT touched — it cannot even be compile-checked on Linux; consolidation is deferred).

**Tech Stack:** C++17 + V4L2 (mmap streaming) + CMake ≥3.20; .NET 8 + Avalonia 11.2.1; xUnit.

## Global Constraints

- **0 warnings everywhere** (repo standard): native build uses `-Wall -Wextra -Werror`; `dotnet build` must stay warning-free.
- **The shim never creates a window and never renders** (parent spec boundary).
- **C ABI parity is load-bearing:** 9 exports, `CosStatus`/`CosParams`/`CosCaps` byte-layouts unchanged, 128-byte enumeration stride, UTF-8 strings.
- **Do not modify `src/CameraOnScreen.App` (WinUI) or `native/shim/shim.vcxproj`** — not buildable/verifiable on this host (XAML compiler is Windows-only; verified 2026-07-27).
- **Run under X11/XWayland** on Linux (`DISPLAY=:0`); never enable content protection (spec §3.1).
- Toolchain on this box: `dotnet` at `~/.dotnet/dotnet` (8.0.4xx, user-local), gcc 16, cmake user-local at `~/.local/cmake/bin` if not system-installed.
- Effect stub detail strings must keep the exact substrings CI greps for: green screen / gaze stubs say `"not built in"`; the FRUC stub must NOT contain that substring.

---

### Task 1: Portable C ABI (`shim.h` + `shim.cpp` build on Linux)

**Files:**
- Modify: `native/shim/shim.h:1-7` (COS_API macro)
- Modify: `native/shim/shim.cpp:14-15,25-36,139-146` (windows/MF includes + init/shutdown guards)

**Interfaces:**
- Consumes: nothing new.
- Produces: `shim.h`/`shim.cpp` that compile with gcc on Linux; `COS_API` = `extern "C" __attribute__((visibility("default")))` on non-Windows. Same 9 exports.

- [ ] **Step 1: Make the export macro portable in `shim.h`**

Replace lines 1–7 with:

```c
#pragma once
#include <stdint.h>
#ifdef _WIN32
  #ifdef COS_EXPORTS
  #define COS_API extern "C" __declspec(dllexport)
  #else
  #define COS_API extern "C" __declspec(dllimport)
  #endif
#else
  #define COS_API extern "C" __attribute__((visibility("default")))
#endif
```

- [ ] **Step 2: Guard the Media Foundation pieces in `shim.cpp`**

Replace `#include <windows.h>` / `#include <mfapi.h>` (lines 14–15) with:

```cpp
#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#endif
```

In `cos_init`, wrap the MF body so non-Windows just returns 1 (keep `g_mfStarted` inside the guard):

```cpp
COS_API int cos_init(void* /*d3d11_device*/) {
#ifdef _WIN32
    // MFStartup exactly once. cos_init runs before enumerate/start (the App calls
    // Init() first), so this guarantees MF is live for every later MF call.
    bool expected = false;
    if (g_mfStarted.compare_exchange_strong(expected, true)) {
        if (FAILED(MFStartup(MF_VERSION))) {
            g_mfStarted.store(false);
            return 0;
        }
    }
#endif
    return 1;
}
```

And in `cos_shutdown` wrap the `MFShutdown` block in `#ifdef _WIN32`. Move the `g_mfStarted` global declaration (line 19) inside an `#ifdef _WIN32` too so `-Werror` doesn't flag an unused variable on Linux.

- [ ] **Step 3: Syntax-check on Linux**

Run: `g++ -std=c++17 -fsyntax-only -Wall -Wextra native/shim/shim.cpp`
Expected: exit 0, no output. (Headers `capture.h`/`aigs.h`/`eyecontact.h`/`superres.h`/`fruc.h` are already platform-clean.)

- [ ] **Step 4: Commit**

```bash
git add native/shim/shim.h native/shim/shim.cpp
git commit -m "feat(linux): portable COS_API export macro + MF guards in shim.cpp"
```

---

### Task 2: V4L2 capture backend + CMake build → `libCameraOnScreen.Shim.so`

**Files:**
- Create: `native/shim/capture_v4l2.cpp`
- Create: `native/shim/CMakeLists.txt`
- Modify: `.gitignore` (add `native/shim/build/`)

**Interfaces:**
- Consumes: `capture.h` (class `Capture`, `CameraDesc`), `fps_counter.h`, effect stub headers.
- Produces: `native/shim/build/libCameraOnScreen.Shim.so` exporting the 9 `cos_*` symbols; `Capture::Enumerate()` returns `/dev/videoN` ids + card names; `Start/LatestFrame/Stop` deliver tightly packed opaque BGRA frames from V4L2 mmap streaming. Effect setters store state; worker is passthrough (Phase 4 marker).

- [ ] **Step 1: Ensure cmake exists (user-local fallback)**

```bash
command -v cmake || {
  curl -sSL https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz \
    | tar xz -C ~/.local && ln -sf ~/.local/cmake-3.31.6-linux-x86_64/bin/cmake ~/.local/bin/cmake; }
cmake --version
```

- [ ] **Step 2: Write `native/shim/capture_v4l2.cpp`**

Full file (mirror of `capture.cpp`'s threading/locking contract — `CaptureState` + separate `g_lifecycleMtx`, leaf error locks; V4L2 replaces MF):

```cpp
// Linux V4L2 implementation of the Capture interface (capture.h). Compiled ONLY by the
// CMake Linux build; Windows compiles capture.cpp (Media Foundation) via the vcxproj.
// Threading contract is identical to capture.cpp: a worker thread fills a mutex-guarded
// frame buffer; Start/Stop are serialized by a separate lifecycle mutex so join() never
// blocks LatestFrame(); error strings sit behind leaf locks never nested under g_state.mtx.
#include "capture.h"
#include "aigs.h"
#include "eyecontact.h"
#include "superres.h"
#include "fruc.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include "fps_counter.h"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>

namespace {

struct CaptureState {
    std::mutex            mtx;
    std::vector<uint8_t>  frame;        // tightly packed BGRA, width*height*4
    int                   width  = 0;
    int                   height = 0;
    bool                  hasNewFrame = false;

    std::atomic<bool>     stopRequested{false};
    std::thread           worker;

    std::string           deviceId;     // "/dev/videoN"; empty selects the first device

    std::atomic<bool>     greenScreenEnabled{false};
    std::atomic<double>   matteExpand{0.0};
    std::atomic<double>   matteFeather{0.0};
    std::atomic<bool>     greenScreenActive{false};
    std::mutex            gsErrMtx;     // leaf lock
    std::string           gsError;

    std::atomic<bool>     eyeContactEnabled{false};
    std::atomic<bool>     eyeContactActive{false};
    std::mutex            ecErrMtx;     // leaf lock
    std::string           ecError;

    std::atomic<bool>     superResEnabled{false};
    std::atomic<int>      superResScale{20};
    std::atomic<int>      superResQuality{1};
    std::atomic<bool>     superResActive{false};
    std::mutex            srErrMtx;     // leaf lock
    std::string           srError;

    std::atomic<bool>     frameInterpEnabled{false};
    std::atomic<bool>     frameInterpActive{false};
    std::mutex            fiErrMtx;     // leaf lock
    std::string           fiError;

    std::atomic<bool>     exposureLockEnabled{false};
    std::atomic<double>   exposureValue{0.0};
    std::atomic<bool>     exposureSupported{false};

    std::atomic<uint64_t> framesProduced{0};
    FpsCounter            fpsCounter;
};

CaptureState g_state;
std::mutex g_lifecycleMtx;

void StopLocked() {
    g_state.stopRequested.store(true, std::memory_order_release);
    if (g_state.worker.joinable()) g_state.worker.join();
}

int Xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

// ---- pixel conversions → tightly packed BGRA (alpha forced 0xFF; passthrough is opaque
// by contract — the premultiplied overlay renders alpha 0 as transparent). All honor the
// source stride (bytesperline).

uint8_t ClampU8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

void YuyvToBgra(const uint8_t* src, int stride, int w, int h, uint8_t* dst) {
    // BT.601 integer approximation, 2 pixels per YUYV macropixel.
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + static_cast<size_t>(stride) * y;
        uint8_t* out = dst + static_cast<size_t>(w) * 4 * y;
        for (int x = 0; x < w; x += 2) {
            const int y0 = row[0], u = row[1] - 128, y1 = row[2], v = row[3] - 128;
            const int rc = (91881 * v) >> 16;
            const int gc = (22554 * u + 46802 * v) >> 16;
            const int bc = (116130 * u) >> 16;
            out[0] = ClampU8(y0 + bc); out[1] = ClampU8(y0 - gc);
            out[2] = ClampU8(y0 + rc); out[3] = 0xFF;
            if (x + 1 < w) {
                out[4] = ClampU8(y1 + bc); out[5] = ClampU8(y1 - gc);
                out[6] = ClampU8(y1 + rc); out[7] = 0xFF;
            }
            row += 4; out += 8;
        }
    }
}

void Bgr24ToBgra(const uint8_t* src, int stride, int w, int h, uint8_t* dst) {
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + static_cast<size_t>(stride) * y;
        uint8_t* out = dst + static_cast<size_t>(w) * 4 * y;
        for (int x = 0; x < w; ++x) {
            out[0] = row[0]; out[1] = row[1]; out[2] = row[2]; out[3] = 0xFF;
            row += 3; out += 4;
        }
    }
}

void Rgb24ToBgra(const uint8_t* src, int stride, int w, int h, uint8_t* dst) {
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + static_cast<size_t>(stride) * y;
        uint8_t* out = dst + static_cast<size_t>(w) * 4 * y;
        for (int x = 0; x < w; ++x) {
            out[0] = row[2]; out[1] = row[1]; out[2] = row[0]; out[3] = 0xFF;
            row += 3; out += 4;
        }
    }
}

void Bgrx32ToBgra(const uint8_t* src, int stride, int w, int h, uint8_t* dst) {
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + static_cast<size_t>(stride) * y;
        uint8_t* out = dst + static_cast<size_t>(w) * 4 * y;
        std::memcpy(out, row, static_cast<size_t>(w) * 4);
        for (int x = 3; x < w * 4; x += 4) out[x] = 0xFF;
    }
}

// Formats we can convert, in preference order (cheapest conversion first).
const uint32_t kSupportedFormats[] = {
    V4L2_PIX_FMT_XBGR32, V4L2_PIX_FMT_BGR24, V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_YUYV,
};

bool IsSupported(uint32_t fmt) {
    for (uint32_t f : kSupportedFormats) if (f == fmt) return true;
    return false;
}

// Picks the best (format, size, interval) the device offers among the convertible
// formats: highest fps wins, resolution breaks ties, capped at 1080p — the same
// scoring the Windows backend uses (fps*1e8 + w*h).
bool NegotiateFormat(int fd, v4l2_format& outFmt, v4l2_fract& outInterval) {
    uint32_t bestFmt = 0; int bestW = 0, bestH = 0;
    v4l2_fract bestIv{0, 1};
    double bestScore = -1.0;

    v4l2_fmtdesc fd_desc{};
    fd_desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (fd_desc.index = 0; Xioctl(fd, VIDIOC_ENUM_FMT, &fd_desc) == 0; ++fd_desc.index) {
        if (!IsSupported(fd_desc.pixelformat)) continue;
        v4l2_frmsizeenum fs{};
        fs.pixel_format = fd_desc.pixelformat;
        for (fs.index = 0; Xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0; ++fs.index) {
            int w, h;
            if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                w = static_cast<int>(fs.discrete.width);
                h = static_cast<int>(fs.discrete.height);
            } else { // stepwise/continuous: take a safe common size within bounds
                w = 1280; h = 720;
            }
            if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
                if (fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) break;
                continue;
            }
            v4l2_frmivalenum fi{};
            fi.pixel_format = fd_desc.pixelformat;
            fi.width = static_cast<uint32_t>(w);
            fi.height = static_cast<uint32_t>(h);
            for (fi.index = 0; Xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fi) == 0; ++fi.index) {
                double fps = 0.0; v4l2_fract iv{0, 1};
                if (fi.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                    iv = fi.discrete;
                    if (iv.numerator > 0)
                        fps = static_cast<double>(iv.denominator) / iv.numerator;
                } else {
                    iv = fi.stepwise.min; // smallest interval = highest fps
                    if (iv.numerator > 0)
                        fps = static_cast<double>(iv.denominator) / iv.numerator;
                }
                const double score = fps * 1e8 + static_cast<double>(w) * h;
                if (score > bestScore) {
                    bestScore = score; bestFmt = fd_desc.pixelformat;
                    bestW = w; bestH = h; bestIv = iv;
                }
                if (fi.type != V4L2_FRMIVAL_TYPE_DISCRETE) break;
            }
            if (fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) break;
        }
    }
    if (bestScore < 0.0) return false;

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = static_cast<uint32_t>(bestW);
    fmt.fmt.pix.height = static_cast<uint32_t>(bestH);
    fmt.fmt.pix.pixelformat = bestFmt;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (Xioctl(fd, VIDIOC_S_FMT, &fmt) == -1) return false;
    if (!IsSupported(fmt.fmt.pix.pixelformat)) return false; // driver substituted something else

    if (bestIv.numerator > 0) {
        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe = bestIv;
        Xioctl(fd, VIDIOC_S_PARM, &parm); // best-effort; not all drivers honor it
    }
    outFmt = fmt;
    outInterval = bestIv;
    return true;
}

struct MappedBuffer { void* start = nullptr; size_t length = 0; };

// Camera exposure lock via V4L2 controls (parity with the Windows IAMCameraControl path).
struct ExposureCtl {
    bool supported = false;
    int  fd = -1;
    long minv = 0, maxv = 0, step = 1;
    int  originalAuto = -1;   // V4L2_CID_EXPOSURE_AUTO value at open; restored on teardown
    bool appliedEnabled = false;
    double appliedValue = -1.0; // sentinel forces the first manual push

    void Init(int deviceFd) {
        fd = deviceFd;
        v4l2_queryctrl qa{}; qa.id = V4L2_CID_EXPOSURE_AUTO;
        v4l2_queryctrl qe{}; qe.id = V4L2_CID_EXPOSURE_ABSOLUTE;
        if (Xioctl(fd, VIDIOC_QUERYCTRL, &qa) == -1 || (qa.flags & V4L2_CTRL_FLAG_DISABLED)) return;
        if (Xioctl(fd, VIDIOC_QUERYCTRL, &qe) == -1 || (qe.flags & V4L2_CTRL_FLAG_DISABLED)) return;
        if (qe.maximum <= qe.minimum || qe.step <= 0) return;
        minv = qe.minimum; maxv = qe.maximum; step = qe.step;
        v4l2_control cur{}; cur.id = V4L2_CID_EXPOSURE_AUTO;
        if (Xioctl(fd, VIDIOC_G_CTRL, &cur) == 0) originalAuto = cur.value;
        supported = true;
        g_state.exposureSupported.store(true, std::memory_order_release);
    }

    void Set(uint32_t id, int value) {
        v4l2_control c{}; c.id = id; c.value = value;
        Xioctl(fd, VIDIOC_S_CTRL, &c);
    }

    void SetAuto() {
        // UVC maps "auto" to APERTURE_PRIORITY (3); fall through AUTO (0) for other drivers.
        for (int v : {originalAuto, V4L2_EXPOSURE_APERTURE_PRIORITY, V4L2_EXPOSURE_AUTO}) {
            if (v < 0 || v == V4L2_EXPOSURE_MANUAL) continue;
            v4l2_control c{}; c.id = V4L2_CID_EXPOSURE_AUTO; c.value = v;
            if (Xioctl(fd, VIDIOC_S_CTRL, &c) == 0) return;
        }
    }

    // Applies the requested lock state when it changed (only touches the camera on change).
    void Apply(bool want, double value) {
        if (!supported) return;
        if (want == appliedEnabled && (!want || value == appliedValue)) return;
        if (want) {
            const double t = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
            long raw = minv + static_cast<long>(t * (maxv - minv) + 0.5);
            raw = minv + ((raw - minv) / step) * step; // snap to stepping delta
            Set(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
            Set(V4L2_CID_EXPOSURE_ABSOLUTE, static_cast<int>(raw));
        } else {
            SetAuto();
        }
        appliedEnabled = want;
        appliedValue = value;
    }

    void Restore() { if (supported) SetAuto(); }
};

} // namespace

void Capture::WorkerLoop() {
    std::string device;
    {
        std::lock_guard<std::mutex> lock(g_state.mtx);
        device = g_state.deviceId;
    }
    if (device.empty()) {
        auto cams = Capture::Enumerate();
        if (cams.empty()) return;
        device = cams.front().id;
    }

    const int fd = open(device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd == -1) return;

    v4l2_format fmt{};
    v4l2_fract interval{};
    if (!NegotiateFormat(fd, fmt, interval)) { close(fd); return; }
    const int width  = static_cast<int>(fmt.fmt.pix.width);
    const int height = static_cast<int>(fmt.fmt.pix.height);
    const int stride = static_cast<int>(fmt.fmt.pix.bytesperline);
    const uint32_t pixfmt = fmt.fmt.pix.pixelformat;

    ExposureCtl exposure;
    exposure.Init(fd);

    // mmap streaming: 4 driver buffers, all queued up front.
    v4l2_requestbuffers req{};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (Xioctl(fd, VIDIOC_REQBUFS, &req) == -1 || req.count < 2) { close(fd); return; }

    std::vector<MappedBuffer> buffers(req.count);
    bool mapOk = true;
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (Xioctl(fd, VIDIOC_QUERYBUF, &b) == -1) { mapOk = false; break; }
        buffers[i].length = b.length;
        buffers[i].start = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (buffers[i].start == MAP_FAILED) { buffers[i].start = nullptr; mapOk = false; break; }
        if (Xioctl(fd, VIDIOC_QBUF, &b) == -1) { mapOk = false; break; }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (!mapOk || Xioctl(fd, VIDIOC_STREAMON, &type) == -1) {
        for (auto& m : buffers) if (m.start) munmap(m.start, m.length);
        close(fd);
        return;
    }

    // Phase 4 (Maxine on Linux) inserts the effect chain here, mirroring capture.cpp:
    // eye contact -> super res -> green screen -> FRUC, worker-thread-local objects.
    // Until then this backend publishes opaque passthrough frames only.
    std::vector<uint8_t> bgra;

    while (!g_state.stopRequested.load(std::memory_order_acquire)) {
        exposure.Apply(g_state.exposureLockEnabled.load(std::memory_order_acquire),
                       g_state.exposureValue.load(std::memory_order_acquire));

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        timeval tv{0, 100 * 1000}; // 100ms slice so stopRequested is honored promptly
        const int r = select(fd + 1, &fds, nullptr, nullptr, &tv);
        if (r == -1 && errno != EINTR) break;
        if (r <= 0) continue;

        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (Xioctl(fd, VIDIOC_DQBUF, &b) == -1) {
            if (errno == EAGAIN) continue;
            break;
        }

        if (b.index < buffers.size() && buffers[b.index].start && b.bytesused > 0) {
            const uint8_t* src = static_cast<const uint8_t*>(buffers[b.index].start);
            bgra.assign(static_cast<size_t>(width) * height * 4, 0);
            switch (pixfmt) {
                case V4L2_PIX_FMT_XBGR32: Bgrx32ToBgra(src, stride, width, height, bgra.data()); break;
                case V4L2_PIX_FMT_BGR24:  Bgr24ToBgra(src, stride, width, height, bgra.data()); break;
                case V4L2_PIX_FMT_RGB24:  Rgb24ToBgra(src, stride, width, height, bgra.data()); break;
                case V4L2_PIX_FMT_YUYV:   YuyvToBgra(src, stride, width, height, bgra.data()); break;
                default: break;
            }
            {
                std::lock_guard<std::mutex> lock(g_state.mtx);
                g_state.frame.swap(bgra);
                g_state.width = width;
                g_state.height = height;
                g_state.hasNewFrame = true;
                g_state.framesProduced.fetch_add(1, std::memory_order_release);
            }
        }
        Xioctl(fd, VIDIOC_QBUF, &b);
    }

    exposure.Restore(); // leave the camera in auto-exposure for other apps
    Xioctl(fd, VIDIOC_STREAMOFF, &type);
    for (auto& m : buffers) if (m.start) munmap(m.start, m.length);
    close(fd);
}

bool Capture::Start(const std::string& symbolicLink) {
    std::lock_guard<std::mutex> life(g_lifecycleMtx);
    StopLocked();
    {
        std::lock_guard<std::mutex> lock(g_state.mtx);
        g_state.deviceId = symbolicLink;
        g_state.frame.clear();
        g_state.width = 0;
        g_state.height = 0;
        g_state.hasNewFrame = false;
        g_state.framesProduced.store(0, std::memory_order_release);
        g_state.fpsCounter.Reset();
    }
    g_state.stopRequested.store(false, std::memory_order_release);
    g_state.worker = std::thread(&Capture::WorkerLoop, this);
    return true;
}

void Capture::Stop() {
    std::lock_guard<std::mutex> life(g_lifecycleMtx);
    StopLocked();
    g_state.greenScreenActive.store(false, std::memory_order_release);
    { std::lock_guard<std::mutex> e(g_state.gsErrMtx); g_state.gsError.clear(); }
    g_state.eyeContactActive.store(false, std::memory_order_release);
    { std::lock_guard<std::mutex> e(g_state.ecErrMtx); g_state.ecError.clear(); }
    g_state.superResActive.store(false, std::memory_order_release);
    { std::lock_guard<std::mutex> e(g_state.srErrMtx); g_state.srError.clear(); }
    g_state.frameInterpActive.store(false, std::memory_order_release);
    { std::lock_guard<std::mutex> e(g_state.fiErrMtx); g_state.fiError.clear(); }
    g_state.exposureSupported.store(false, std::memory_order_release);
}

bool Capture::LatestFrame(std::vector<uint8_t>& out, int& w, int& h) {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    if (!g_state.hasNewFrame || g_state.frame.empty()) return false;
    out = g_state.frame;
    w = g_state.width;
    h = g_state.height;
    g_state.hasNewFrame = false;
    return true;
}

void Capture::SetGreenScreen(bool enabled) {
    g_state.greenScreenEnabled.store(enabled, std::memory_order_release);
}
void Capture::SetMatteParams(double expand, double feather) {
    g_state.matteExpand.store(expand, std::memory_order_release);
    g_state.matteFeather.store(feather, std::memory_order_release);
}
bool Capture::GreenScreenActive() const {
    return g_state.greenScreenActive.load(std::memory_order_acquire);
}
std::string Capture::GreenScreenError() const {
    std::lock_guard<std::mutex> e(g_state.gsErrMtx);
    return g_state.gsError;
}
void Capture::SetEyeContact(bool enabled) {
    g_state.eyeContactEnabled.store(enabled, std::memory_order_release);
}
bool Capture::EyeContactActive() const {
    return g_state.eyeContactActive.load(std::memory_order_acquire);
}
std::string Capture::EyeContactError() const {
    std::lock_guard<std::mutex> e(g_state.ecErrMtx);
    return g_state.ecError;
}
void Capture::SetSuperRes(bool enabled, int qualityLevel, int scaleX10) {
    g_state.superResScale.store(scaleX10 == 15 ? 15 : 20, std::memory_order_release);
    g_state.superResQuality.store(qualityLevel, std::memory_order_release);
    g_state.superResEnabled.store(enabled, std::memory_order_release);
}
bool Capture::SuperResActive() const {
    return g_state.superResActive.load(std::memory_order_acquire);
}
std::string Capture::SuperResError() const {
    std::lock_guard<std::mutex> e(g_state.srErrMtx);
    return g_state.srError;
}
void Capture::SetFrameInterp(bool enabled) {
    g_state.frameInterpEnabled.store(enabled, std::memory_order_release);
}
bool Capture::FrameInterpActive() const {
    return g_state.frameInterpActive.load(std::memory_order_acquire);
}
std::string Capture::FrameInterpError() const {
    std::lock_guard<std::mutex> e(g_state.fiErrMtx);
    return g_state.fiError;
}
void Capture::SetExposureLock(bool enabled, double value) {
    g_state.exposureValue.store(value, std::memory_order_release);
    g_state.exposureLockEnabled.store(enabled, std::memory_order_release);
}
bool Capture::ExposureSupported() const {
    return g_state.exposureSupported.load(std::memory_order_acquire);
}

Capture::~Capture() {
    Stop();
}

std::vector<CameraDesc> Capture::Enumerate() {
    std::vector<CameraDesc> result;
    // /dev/video* nodes: capture devices carry V4L2_CAP_VIDEO_CAPTURE|STREAMING in
    // device_caps; metadata/output nodes don't, which filters the extra nodes UVC
    // cameras expose. 64 covers any realistic index range.
    for (int i = 0; i < 64; ++i) {
        const std::string path = "/dev/video" + std::to_string(i);
        const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd == -1) continue;
        v4l2_capability cap{};
        if (Xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                ? cap.device_caps : cap.capabilities;
            if ((caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING)) {
                CameraDesc desc;
                desc.id = path;
                desc.name = reinterpret_cast<const char*>(cap.card);
                if (desc.name.empty()) desc.name = path;
                result.push_back(std::move(desc));
            }
        }
        close(fd);
    }
    return result;
}

double Capture::MeasuredFps() const {
    using clock = std::chrono::steady_clock;
    const double nowSec =
        std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    const uint64_t frames = g_state.framesProduced.load(std::memory_order_acquire);
    return g_state.fpsCounter.Sample(nowSec, frames);
}
```

- [ ] **Step 3: Write `native/shim/CMakeLists.txt`**

```cmake
# Linux build of the C-ABI shim -> libCameraOnScreen.Shim.so (issue #27 Phase 2).
# Windows keeps shim.vcxproj (deliberately outside the .sln); this file is Linux-only.
# Effects compile as passthrough stubs until the Maxine Linux runtimes land (Phase 4):
# COS_HAS_MAXINE / COS_HAS_MAXINE_AR / COS_HAS_FRUC are intentionally not defined here yet.
cmake_minimum_required(VERSION 3.20)
project(CameraOnScreenShim CXX)

if(NOT UNIX OR APPLE)
  message(FATAL_ERROR "This CMake build targets Linux only; build shim.vcxproj on Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_library(shim SHARED
  shim.cpp
  capture_v4l2.cpp
  aigs.cpp
  eyecontact.cpp
  superres.cpp
  fruc.cpp
)
set_target_properties(shim PROPERTIES
  OUTPUT_NAME "CameraOnScreen.Shim"   # -> libCameraOnScreen.Shim.so
  CXX_VISIBILITY_PRESET hidden
)
target_compile_options(shim PRIVATE -Wall -Wextra -Werror)
target_link_libraries(shim PRIVATE Threads::Threads)

add_executable(v4l2_probe smoke/v4l2_probe.cpp)
target_compile_options(v4l2_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(v4l2_probe PRIVATE shim)
```

(`smoke/v4l2_probe.cpp` is Task 3; write it before the first configure, or comment those
three lines out until Task 3 and restore them there.)

- [ ] **Step 4: Add the build dir to `.gitignore`**

Append `native/shim/build/` under the existing build-artifact entries.

- [ ] **Step 5: Build clean**

Run: `cmake -S native/shim -B native/shim/build && cmake --build native/shim/build`
Expected: 0 warnings, produces `native/shim/build/libCameraOnScreen.Shim.so`.

- [ ] **Step 6: Verify the 9 exports**

Run: `nm -D --defined-only native/shim/build/libCameraOnScreen.Shim.so | grep cos_`
Expected: exactly `cos_init cos_enumerate_cameras cos_set_params cos_start cos_stop cos_get_status cos_get_frame cos_query_capabilities cos_shutdown`.

- [ ] **Step 7: Commit**

```bash
git add native/shim/capture_v4l2.cpp native/shim/CMakeLists.txt .gitignore
git commit -m "feat(linux): V4L2 capture backend + CMake build -> libCameraOnScreen.Shim.so"
```

---

### Task 3: Linux smoke CLI (`v4l2_probe`)

**Files:**
- Create: `native/shim/smoke/v4l2_probe.cpp`

**Interfaces:**
- Consumes: the 9 `cos_*` exports via `shim.h` (linked against the `shim` target).
- Produces: a CLI that exercises init → enumerate → query_capabilities → (if a camera exists) start/frames/stop → shutdown. Exit 0 = ABI works, even with zero cameras.

- [ ] **Step 1: Write `native/shim/smoke/v4l2_probe.cpp`**

```cpp
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
```

- [ ] **Step 2: Build + run**

Run: `cmake --build native/shim/build && ./native/shim/build/v4l2_probe`
Expected on this box (no webcam attached): `cameras: 0`, all caps 0 with stub details (`"Maxine SDK not built in"`, `"AR SDK not built in"`, FRUC `"unavailable"`), `PASS`, exit 0.

- [ ] **Step 3: Commit**

```bash
git add native/shim/smoke/v4l2_probe.cpp
git commit -m "test(linux): v4l2_probe ABI smoke (enumerate/caps/optional capture)"
```

---

### Task 4: XDG-aware config path in Core (TDD)

**Files:**
- Modify: `src/CameraOnScreen.Core/Config/JsonSettingsStore.cs:11-13`
- Test: `tests/CameraOnScreen.Core.Tests/JsonSettingsStoreTests.cs` (add to the existing test file if one exists; else create)

**Interfaces:**
- Consumes: nothing new.
- Produces: `JsonSettingsStore.DefaultPath()` → `%LOCALAPPDATA%\CameraOnScreen\config.json` on Windows (unchanged; the WinUI app keeps calling it), `$XDG_CONFIG_HOME/CameraOnScreen/config.json` (default `~/.config/...`) elsewhere.

- [ ] **Step 1: Write the failing test**

```csharp
[Fact]
public void DefaultPath_UsesPlatformConfigRoot()
{
    var path = JsonSettingsStore.DefaultPath();
    var expectedRoot = OperatingSystem.IsWindows()
        ? Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData)
        : Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData); // XDG_CONFIG_HOME / ~/.config
    Assert.Equal(Path.Combine(expectedRoot, "CameraOnScreen", "config.json"), path);
}
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~DefaultPath_UsesPlatformConfigRoot"`
Expected: FAIL on Linux (current code returns `~/.local/share/...` via LocalApplicationData).

- [ ] **Step 3: Implement**

```csharp
// Windows: %LOCALAPPDATA% (existing contract, WinUI app unchanged). Elsewhere:
// SpecialFolder.ApplicationData maps to $XDG_CONFIG_HOME (default ~/.config) on Unix,
// which is where Linux config belongs (issue #29).
public static string DefaultPath() => Path.Combine(
    Environment.GetFolderPath(OperatingSystem.IsWindows()
        ? Environment.SpecialFolder.LocalApplicationData
        : Environment.SpecialFolder.ApplicationData),
    "CameraOnScreen", "config.json");
```

- [ ] **Step 4: Run the full test suite**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
Expected: all pass (90 total).

- [ ] **Step 5: Commit**

```bash
git add src/CameraOnScreen.Core/Config/JsonSettingsStore.cs tests/CameraOnScreen.Core.Tests/JsonSettingsStoreTests.cs
git commit -m "feat(linux): XDG config path for JsonSettingsStore.DefaultPath (#29)"
```

---

### Task 5: Avalonia-side `PInvokeShim` + GPU tier detection

**Files:**
- Create: `src/CameraOnScreen.App.Avalonia/Native/PInvokeShim.cs`
- Create: `src/CameraOnScreen.App.Avalonia/Native/GpuTierDetector.cs`

**Interfaces:**
- Consumes: `CameraOnScreen.Core.Native.INativeShim` + contract records; `libCameraOnScreen.Shim.so` beside the app (or `CameraOnScreen.Shim.dll` on Windows).
- Produces: `CameraOnScreen.App.Avalonia.Native.PInvokeShim : INativeShim` (library name `"CameraOnScreen.Shim"` — .NET probing appends `.dll` on Windows and tries `lib…so` on Linux, so ONE name serves both); `GpuTierDetector.Detect()` → `GpuTier`.

- [ ] **Step 1: Create `Native/PInvokeShim.cs`**

Copy `src/CameraOnScreen.App/Native/PInvokeShim.cs` verbatim except:
- namespace `CameraOnScreen.App.Avalonia.Native;`
- `private const string Dll = "CameraOnScreen.Shim";` (no `.dll` suffix)
- add this header comment:

```csharp
// Cross-platform twin of the WinUI app's PInvokeShim (same C ABI, same struct mirrors).
// Duplicated rather than shared because the WinUI project cannot be compile-checked on the
// Linux dev box (Windows-only XAML compiler); consolidation is tracked in #29. The extension-
// less library name lets .NET probing resolve CameraOnScreen.Shim.dll (Windows) and
// libCameraOnScreen.Shim.so (Linux) from the app directory.
```

The struct mirrors (`CosStatus`/`CosParams`/`CosCaps`), the 9 `DllImport`s, `ReadUtf8`, and every method body stay byte-for-byte identical to the WinUI copy — layout parity with `shim.h` is load-bearing.

- [ ] **Step 2: Create `Native/GpuTierDetector.cs`**

```csharp
using CameraOnScreen.Core.Orchestration;

namespace CameraOnScreen.App.Avalonia.Native;

public static class GpuTierDetector
{
    // Display-only heuristic (the native capability probe is the real effect gate).
    // Linux: the NVIDIA kernel driver exposes /proc/driver/nvidia when loaded.
    public static GpuTier Detect() =>
        File.Exists("/proc/driver/nvidia/version") ? GpuTier.Rtx : GpuTier.NonRtx;
}
```

- [ ] **Step 3: Build clean**

Run: `dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj`
Expected: 0 warnings, 0 errors.

- [ ] **Step 4: Commit**

```bash
git add src/CameraOnScreen.App.Avalonia/Native/
git commit -m "feat(avalonia): PInvokeShim (cross-platform lib name) + Linux GPU tier detect"
```

---

### Task 6: Wire the Avalonia panel to the real shim + config persistence

**Files:**
- Create: `src/CameraOnScreen.App.Avalonia/Composition/Services.cs`
- Modify: `src/CameraOnScreen.App.Avalonia/MainWindow.axaml.cs` (replace `BuildDemoViewModel`)
- Modify: `src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj` (copy the `.so` when present)

**Interfaces:**
- Consumes: `PInvokeShim`, `FakeShim`, `Orchestrator`, `MainViewModel` (`LoadFrom`, `ToAppConfig`, `PollStatusTick`, `ProbeCapabilitiesAsync`, `Dispose`), `JsonSettingsStore`, `GpuTierDetector.Detect()`.
- Produces: `Services.Build()` → `(MainViewModel Vm, JsonSettingsStore Store, AppConfig Loaded)`; MainWindow polls status at 4 Hz, saves config on close, disposes the VM (→ `cos_shutdown` joins the native worker — the WinUI Dispose gotcha applies identically here).

- [ ] **Step 1: Create `Composition/Services.cs`**

```csharp
using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.Native;
using CameraOnScreen.Core.Orchestration;
using CameraOnScreen.Core.ViewModels;

namespace CameraOnScreen.App.Avalonia.Composition;

public static class Services
{
    public sealed record AppServices(MainViewModel Vm, JsonSettingsStore Store, AppConfig Loaded);

    // Mirrors the WinUI Services.BuildViewModel: real shim + orchestrator + config-loaded VM.
    // Falls back to the FakeShim demo VM when the native library is absent (e.g. the shim
    // hasn't been built yet) so the panel still opens instead of crashing — the WinUI app
    // has no such fallback because its shim is deployed by its own build.
    public static AppServices Build()
    {
        var store = new JsonSettingsStore(JsonSettingsStore.DefaultPath());
        var config = store.Load();

        INativeShim shim;
        try
        {
            shim = new Native.PInvokeShim();
            shim.Init(IntPtr.Zero); // no shared D3D device on Linux; first P/Invoke = load probe
        }
        catch (DllNotFoundException)
        {
            shim = new FakeShim();
        }

        var orchestrator = new Orchestrator(shim, Native.GpuTierDetector.Detect());
        var vm = new MainViewModel(orchestrator, shim);
        foreach (var cam in shim.EnumerateCameras()) vm.Cameras.Add(cam);
        vm.LoadFrom(config);
        if (vm.SelectedCamera is null && vm.Cameras.Count > 0) vm.SelectedCamera = vm.Cameras[0];

        // ORT hand-inference is not wired on Linux yet (Phase 3+): grey the toggle honestly.
        vm.FingerControlAvailable = false;
        vm.FingerControlDetail = "Finger control not available on Linux yet";

        _ = vm.ProbeCapabilitiesAsync();
        return new AppServices(vm, store, config);
    }
}
```

- [ ] **Step 2: Rewrite `MainWindow.axaml.cs`**

```csharp
using Avalonia.Controls;
using Avalonia.Threading;
using CameraOnScreen.App.Avalonia.Composition;
using CameraOnScreen.Core.ViewModels;

namespace CameraOnScreen.App.Avalonia;

public partial class MainWindow : Window
{
    private readonly Services.AppServices _services;
    private readonly DispatcherTimer _statusTimer;

    public MainWindow()
    {
        InitializeComponent();
        _services = Services.Build();
        DataContext = _services.Vm;

        // Status is polled, never pushed (repo contract). No frame pump yet — the Linux
        // overlay is Phase 3 — so 4 Hz keeps fps/error/running fresh without burning CPU.
        _statusTimer = new DispatcherTimer(TimeSpan.FromMilliseconds(250),
            DispatcherPriority.Background, (_, _) => _services.Vm.PollStatusTick());
        _statusTimer.Start();

        Closing += (_, _) =>
        {
            // Overlay geometry passes through from the loaded config until the Linux
            // overlay (Phase 3) exists to supply live values.
            var o = _services.Loaded.Overlay;
            _services.Store.Save(_services.Vm.ToAppConfig(o.X, o.Y, o.Width, o.Height));
        };
        Closed += (_, _) =>
        {
            _statusTimer.Stop();
            // Joins the native capture worker (cos_shutdown) — without this the global
            // std::thread is destroyed joinable at process exit -> std::terminate.
            _services.Vm.Dispose();
        };
    }
}
```

(Check `OverlaySettings` property names in `src/CameraOnScreen.Core/Config/Models.cs` — use the actual `X/Y/Width/Height` casing found there.)

- [ ] **Step 3: Copy the shim beside the app in the csproj**

Add to `CameraOnScreen.App.Avalonia.csproj`:

```xml
<!-- Deploy the Linux shim when it has been built (cmake -S native/shim -B native/shim/build).
     Absent -> the app falls back to the FakeShim demo VM instead of crashing. -->
<ItemGroup Condition="Exists('$(MSBuildThisFileDirectory)..\..\native\shim\build\libCameraOnScreen.Shim.so')">
  <None Include="$(MSBuildThisFileDirectory)..\..\native\shim\build\libCameraOnScreen.Shim.so"
        Link="libCameraOnScreen.Shim.so" CopyToOutputDirectory="PreserveNewest" />
</ItemGroup>
```

- [ ] **Step 4: Build + tests + launch check**

Run: `dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj` (expect 0 warnings; verify `libCameraOnScreen.Shim.so` lands in `bin/Debug/net8.0/`), then `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj` (all pass), then launch `dotnet run --project src/CameraOnScreen.App.Avalonia` for ~5s in the background and confirm the process stays alive and `~/.config/CameraOnScreen/config.json` is written after close (send SIGTERM… if the window can't be closed programmatically, verify the config write by invoking the Closing path via a manual run later — record as human-gate item).
Expected: panel opens; effect toggles grey out with the stub details ("Maxine SDK not built in") once the probe lands — honest Linux passthrough state.

- [ ] **Step 5: Commit**

```bash
git add src/CameraOnScreen.App.Avalonia/
git commit -m "feat(avalonia): real shim wiring + config load/save + status poll (#29)"
```

---

### Task 7: Hosted Linux CI

**Files:**
- Create: `.github/workflows/ci-linux.yml`

**Interfaces:**
- Consumes: everything above; `ubuntu-latest` (cmake + gcc preinstalled, no GPU/camera needed — stub shim + `v4l2_probe` passing with 0 cameras is the design).
- Produces: PR-gating Linux job. (The existing `ci.yml` self-hosted Windows runner went away with the Windows box; its jobs will show pending/queued on PRs until the runner question is resolved in Phase 5 — do not edit `ci.yml` in this plan.)

- [ ] **Step 1: Write `.github/workflows/ci-linux.yml`**

```yaml
name: CI (Linux)

on:
  pull_request:

jobs:
  linux:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Build shim (.so, stub effects)
        run: |
          cmake -S native/shim -B native/shim/build
          cmake --build native/shim/build

      - name: Verify exports
        run: |
          nm -D --defined-only native/shim/build/libCameraOnScreen.Shim.so | grep -q cos_query_capabilities
          nm -D --defined-only native/shim/build/libCameraOnScreen.Shim.so | grep -q cos_get_frame

      - name: ABI smoke (v4l2_probe, zero cameras is a pass)
        run: ./native/shim/build/v4l2_probe

      - uses: actions/setup-dotnet@v4
        with:
          dotnet-version: 8.0.x

      - name: Avalonia app (warnings as errors)
        run: dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj -warnaserror -p:TreatWarningsAsErrors=true

      - name: Core tests
        run: dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj -warnaserror -p:TreatWarningsAsErrors=true
```

- [ ] **Step 2: Sanity-check the workflow locally**

Run the same commands from a clean `git stash`-free tree in order; all must pass (this is exactly what the runner will do).

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci-linux.yml
git commit -m "ci: hosted Linux job — shim .so build, ABI smoke, Avalonia build, Core tests"
```

---

### Task 8: Docs + issue bookkeeping + PR

**Files:**
- Modify: `CLAUDE.md` (add a "Linux build" subsection under Build & test)
- Modify: GitHub issues #27 / #29 (checkboxes + status comments), open PR

- [ ] **Step 1: Add a Linux build section to `CLAUDE.md`** (after the Windows build block)

```markdown
### Linux build (Phase 2+, issue #27)

The same shim sources build as `libCameraOnScreen.Shim.so` via CMake with a V4L2 capture
backend (`capture_v4l2.cpp`; Windows keeps `capture.cpp` + the vcxproj). Effects are the
CI-safe passthrough stubs until Phase 4 wires the Maxine Linux runtimes.

```bash
cmake -S native/shim -B native/shim/build && cmake --build native/shim/build
./native/shim/build/v4l2_probe   # ABI smoke: enumerate + caps (+3s capture when a camera exists)
dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj  # auto-copies the .so
dotnet run --project src/CameraOnScreen.App.Avalonia   # X11/XWayland; real shim, FakeShim fallback if .so absent
```

Config: `$XDG_CONFIG_HOME/CameraOnScreen/config.json` (Windows keeps `%LOCALAPPDATA%`).
Hosted Linux CI: `.github/workflows/ci-linux.yml` (no GPU/camera needed — stub build).
The old `[self-hosted, windows, rtx]` runner died with the Windows box; `ci.yml` jobs stay
queued on PRs until Phase 5 re-homes them (Linux+RTX runner).
```

- [ ] **Step 2: Push branch + open PR**

```bash
git push -u origin feat/linux-phase2-shim-v4l2
gh pr create --title "feat: Linux Phase 2 — shim .so (CMake + V4L2) + Avalonia real-shim wiring" --body "<summary; closes nothing; refs #27 #29>"
```

- [ ] **Step 3: Update issues**

- #27: tick the Phase 2 checkbox (via `gh issue edit 27 --body-file` with the edited body), comment with: dev box migrated (CachyOS + RTX 3090 + driver 610.43, Wayland session with XWayland), first Linux smoke test of the Avalonia panel PASSED, Phase 2 delivered in the PR, spikes A/C now unblocked but need the Maxine Linux SDK downloads (NGC), Spike B needs OBS + human gate, CI runner note.
- #29: tick "No config load/save" and "DataContext is a fully-available FakeShim demo VM" (now real-shim-with-fallback), note the remaining cosmetic items stay open.

---

## Verification gates that need a human

1. **Visual panel check on the box** — panel opens on X11/XWayland with camera list (once a webcam is plugged in) and honest greyed-out effect toggles.
2. **Real-camera capture** — no `/dev/video*` device is currently attached to the box; plug in a UVC webcam and run `./native/shim/build/v4l2_probe` — expect `capture: N frames … fps≈30`.
