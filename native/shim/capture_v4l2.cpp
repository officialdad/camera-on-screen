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

// Serializes Start()/Stop(); DISTINCT from g_state.mtx so join() never blocks LatestFrame().
std::mutex g_lifecycleMtx;

// Tears down the worker thread. MUST be called with g_lifecycleMtx held.
void StopLocked() {
    g_state.stopRequested.store(true, std::memory_order_release);
    if (g_state.worker.joinable()) g_state.worker.join();
}

int Xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

// ---- pixel conversions -> tightly packed BGRA (alpha forced 0xFF; passthrough is opaque
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

    v4l2_fmtdesc fmtDesc{};
    fmtDesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (fmtDesc.index = 0; Xioctl(fd, VIDIOC_ENUM_FMT, &fmtDesc) == 0; ++fmtDesc.index) {
        if (!IsSupported(fmtDesc.pixelformat)) continue;
        v4l2_frmsizeenum fs{};
        fs.pixel_format = fmtDesc.pixelformat;
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
            fi.pixel_format = fmtDesc.pixelformat;
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
                    bestScore = score; bestFmt = fmtDesc.pixelformat;
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
        for (int v : {originalAuto, static_cast<int>(V4L2_EXPOSURE_APERTURE_PRIORITY),
                      static_cast<int>(V4L2_EXPOSURE_AUTO)}) {
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
    g_state.hasNewFrame = false; // consumed; next call returns false until a new frame.
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
