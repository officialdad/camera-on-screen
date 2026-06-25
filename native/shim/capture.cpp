#include "capture.h"
#include "aigs.h"
#include "eyecontact.h"
#include "superres.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include "fps_counter.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlwapi.h>
#include <dshow.h>  // IAMCameraControl, CameraControl_Exposure, CameraControl_Flags_Manual/_Auto

// COM smart-pointer-free RAII via a tiny helper to avoid pulling in <wrl> / <comdef>.
namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// State shared between the worker thread and the calling thread. Guarded by mtx.
struct CaptureState {
    std::mutex            mtx;
    std::vector<uint8_t>  frame;        // tightly packed BGRA, width*height*4
    int                   width  = 0;
    int                   height = 0;
    bool                  hasNewFrame = false;

    std::atomic<bool>     stopRequested{false};
    std::thread           worker;

    std::string           symbolicLink;

    std::atomic<bool>     greenScreenEnabled{false}; // set by UI thread, read by worker
    std::atomic<double>   matteExpand{0.0};  // set by UI thread, read by worker
    std::atomic<double>   matteFeather{0.0}; // set by UI thread, read by worker
    std::atomic<bool>     greenScreenActive{false};  // set by worker
    std::mutex            gsErrMtx;
    std::string           gsError;                   // guarded by gsErrMtx

    std::atomic<bool>     eyeContactEnabled{false}; // set by UI thread, read by worker
    std::atomic<bool>     eyeContactActive{false};  // set by worker
    std::mutex            ecErrMtx;                  // leaf lock, never nested under mtx/lifecycle
    std::string           ecError;                   // guarded by ecErrMtx

    std::atomic<bool>     superResEnabled{false};
    std::atomic<int>      superResScale{20};   // 15 or 20
    std::atomic<int>      superResQuality{1};  // VSR QualityLevel (mode): 1-4 / 8-11 / 12-15
    std::atomic<bool>     superResActive{false};
    std::mutex            srErrMtx;            // leaf lock
    std::string           srError;

    std::atomic<bool>     exposureLockEnabled{false}; // set by UI thread, read by worker
    std::atomic<double>   exposureValue{0.0};         // 0..1 normalized, set by UI thread
    std::atomic<bool>     exposureSupported{false};   // set by worker once camera range is known

    std::atomic<uint64_t> framesProduced{0}; // bumped by worker after each published frame
    FpsCounter            fpsCounter;         // read only by MeasuredFps (status-poll thread)
};

// Module-level singleton state. Capture is used as a single global instance by the
// shim (one camera at a time), so a single state block keeps the Capture object
// trivially copyable-free and avoids leaking MF types into capture.h.
CaptureState g_state;

// Serializes Start()/Stop() so the worker handle and stopRequested are never mutated
// concurrently (Task 12 drives capture from a render/timer thread). DISTINCT from
// g_state.mtx: holding this during join() must not block LatestFrame().
std::mutex g_lifecycleMtx;

// Tears down the worker thread. MUST be called with g_lifecycleMtx held. Does not
// take the lifecycle lock itself so Start() and Stop() can both reuse it without
// recursive locking / self-deadlock.
void StopLocked() {
    g_state.stopRequested.store(true, std::memory_order_release);
    if (g_state.worker.joinable()) {
        g_state.worker.join();
    }
}

// Reads a frame's pixels out of an IMFMediaBuffer (RGB32 / BGRX) into a tightly
// packed BGRA destination, honoring the source stride and forcing alpha = 0xFF.
// defaultStride is the contiguous stride (width*4); the actual stride is queried
// from the 2D buffer when available, else falls back to defaultStride.
bool CopyFrame(IMFSample* sample, int width, int height,
               LONG defaultStride, std::vector<uint8_t>& dst) {
    IMFMediaBuffer* buffer = nullptr;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) {
        return false;
    }

    const int destStride = width * 4;
    dst.assign(static_cast<size_t>(destStride) * height, 0);

    bool ok = false;

    // Prefer the 2D buffer interface: it exposes the true row pitch (which may be
    // padded or negative for bottom-up images).
    IMF2DBuffer* buffer2d = nullptr;
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d))) && buffer2d) {
        BYTE* scanline0 = nullptr;
        LONG  pitch = 0;
        if (SUCCEEDED(buffer2d->Lock2D(&scanline0, &pitch))) {
            // scanline0 points at the first row in top-down order; pitch may be
            // negative when the underlying layout is bottom-up.
            for (int y = 0; y < height; ++y) {
                const BYTE* src = scanline0 + static_cast<ptrdiff_t>(pitch) * y;
                uint8_t* row = dst.data() + static_cast<size_t>(destStride) * y;
                std::memcpy(row, src, destStride);
                for (int x = 3; x < destStride; x += 4) row[x] = 0xFF; // force opaque
            }
            buffer2d->Unlock2D();
            ok = true;
        }
        SafeRelease(buffer2d);
    } else {
        // Fall back to the 1D buffer. Use the media-type stride; if it is negative
        // the image is bottom-up and the buffer points at the last row.
        // defaultStride == 0 means orientation could not be determined -> fail rather
        // than silently assuming top-down (which would render a bottom-up source
        // upside-down).
        if (defaultStride == 0) {
            SafeRelease(buffer);
            return false;
        }
        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (SUCCEEDED(buffer->Lock(&data, &maxLen, &curLen))) {
            LONG stride = defaultStride;
            const BYTE* begin = data;
            if (stride < 0) {
                // Bottom-up: first source row is the last image row.
                begin = data + static_cast<ptrdiff_t>(-stride) * (height - 1);
            }
            for (int y = 0; y < height; ++y) {
                const BYTE* src = begin + static_cast<ptrdiff_t>(stride) * y;
                uint8_t* row = dst.data() + static_cast<size_t>(destStride) * y;
                std::memcpy(row, src, destStride);
                for (int x = 3; x < destStride; x += 4) row[x] = 0xFF; // force opaque
            }
            buffer->Unlock();
            ok = true;
        }
    }

    SafeRelease(buffer);
    return ok;
}

// Computes the SIGNED stride for an RGB32 media type. The sign encodes orientation:
// positive = top-down, negative = bottom-up. Prefers MF_MT_DEFAULT_STRIDE (already
// signed); when absent, derives it from the subtype via
// MFGetStrideForBitmapInfoHeader (which also returns a signed value). Returns 0 when
// the stride/orientation cannot be determined so callers can fail rather than
// silently assuming top-down.
LONG GetDefaultStride(IMFMediaType* type, UINT32 width) {
    LONG stride = 0;
    if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE,
                                  reinterpret_cast<UINT32*>(&stride))) && stride != 0) {
        return stride;
    }

    // Attribute absent: derive a signed stride from the format's FOURCC. This honors
    // a genuinely bottom-up source (negative result) instead of assuming width*4.
    GUID subtype = GUID_NULL;
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
        return 0;
    }
    LONG signedStride = 0;
    if (SUCCEEDED(MFGetStrideForBitmapInfoHeader(subtype.Data1, width, &signedStride))
        && signedStride != 0) {
        return signedStride;
    }
    return 0; // orientation undeterminable
}

// Builds an IMFSourceReader for the given symbolic link (or first device when
// empty), negotiated to deliver RGB32 with video processing enabled so MF converts
// from the camera's native NV12/YUY2/MJPG. Out params receive the reader and its
// negotiated width/height/stride. Returns true on success.
bool CreateReader(const std::string& symbolicLink, IMFSourceReader*& outReader,
                  int& outW, int& outH, LONG& outStride) {
    outReader = nullptr;

    IMFAttributes* devAttrs = nullptr;
    if (FAILED(MFCreateAttributes(&devAttrs, 1))) return false;
    devAttrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFMediaSource* source = nullptr;

    if (!symbolicLink.empty()) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, symbolicLink.c_str(), -1, nullptr, 0);
        std::wstring wlink(wlen > 0 ? wlen - 1 : 0, L'\0');
        if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, symbolicLink.c_str(), -1, &wlink[0], wlen);
        devAttrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                            wlink.c_str());
        HRESULT hr = MFCreateDeviceSource(devAttrs, &source);
        SafeRelease(devAttrs);
        if (FAILED(hr) || !source) return false;
    } else {
        // Enumerate and pick the first device.
        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFEnumDeviceSources(devAttrs, &devices, &count);
        SafeRelease(devAttrs);
        if (FAILED(hr) || count == 0) {
            if (devices) CoTaskMemFree(devices);
            return false;
        }
        hr = devices[0]->ActivateObject(IID_PPV_ARGS(&source));
        for (UINT32 i = 0; i < count; ++i) SafeRelease(devices[i]);
        CoTaskMemFree(devices);
        if (FAILED(hr) || !source) return false;
    }

    // Reader attributes: enable hardware video processing so MF converts native
    // camera formats to RGB32 for us.
    IMFAttributes* readerAttrs = nullptr;
    if (FAILED(MFCreateAttributes(&readerAttrs, 1))) { SafeRelease(source); return false; }
    readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    IMFSourceReader* reader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromMediaSource(source, readerAttrs, &reader);
    SafeRelease(readerAttrs);
    SafeRelease(source);
    if (FAILED(hr) || !reader) return false;

    // Pick the camera's best native type up front: HIGHEST frame rate, then highest
    // resolution, capped at 1080p (the overlay downscales; _frameBuf is sized for 4K).
    // Without this MF picks the camera's *default* native type, which on common webcams
    // (e.g. Logitech Brio 100) is a ~15fps mode rather than the MJPG 1080p30 path — so
    // the live overlay is choppy at half the camera's real capability. fps dominates the
    // score; resolution is only the tiebreaker.
    UINT32 bestW = 0, bestH = 0, bestNum = 0, bestDen = 1;
    double bestScore = -1.0;
    for (DWORD i = 0; ; ++i) {
        IMFMediaType* nt = nullptr;
        if (FAILED(reader->GetNativeMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &nt))) break;
        UINT32 w = 0, h = 0, num = 0, den = 0;
        if (SUCCEEDED(MFGetAttributeSize(nt, MF_MT_FRAME_SIZE, &w, &h)) &&
            SUCCEEDED(MFGetAttributeRatio(nt, MF_MT_FRAME_RATE, &num, &den)) &&
            w > 0 && h > 0 && den > 0 && w <= 1920 && h <= 1080) {
            const double fps = static_cast<double>(num) / den;
            const double score = fps * 1e8 + static_cast<double>(w) * h; // fps wins, res breaks ties
            if (score > bestScore) {
                bestScore = score; bestW = w; bestH = h; bestNum = num; bestDen = den;
            }
        }
        SafeRelease(nt);
    }

    // Request RGB32 output on the first video stream. When a best native type was found,
    // pin the output to its frame size + rate so MF selects that (high-fps) native path
    // and converts it; if MF rejects the constrained request, retry unconstrained
    // (previous behavior — MF picks the default).
    IMFMediaType* outType = nullptr;
    if (FAILED(MFCreateMediaType(&outType))) { SafeRelease(reader); return false; }
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (bestScore >= 0.0) {
        MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, bestW, bestH);
        MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, bestNum, bestDen);
    }
    hr = reader->SetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outType);
    if (FAILED(hr) && bestScore >= 0.0) {
        // Constrained request rejected — fall back to letting MF choose the native type.
        SafeRelease(outType);
        if (FAILED(MFCreateMediaType(&outType))) { SafeRelease(reader); return false; }
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = reader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outType);
    }
    SafeRelease(outType);
    if (FAILED(hr)) { SafeRelease(reader); return false; }

    // Read back the negotiated type to get dimensions + stride.
    IMFMediaType* current = nullptr;
    hr = reader->GetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current);
    if (FAILED(hr) || !current) { SafeRelease(reader); return false; }

    UINT32 w = 0, h = 0;
    if (FAILED(MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0) {
        SafeRelease(current);
        SafeRelease(reader);
        return false;
    }
    outStride = GetDefaultStride(current, w);
    SafeRelease(current);

    outW = static_cast<int>(w);
    outH = static_cast<int>(h);
    outReader = reader;
    return true;
}

} // namespace

void Capture::WorkerLoop() {
    // MF + COM must be initialized per-thread for the reader to behave.
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Aigs must live entirely on the worker thread: CUDA stream/effect have thread
    // affinity. Declare here (after CoInitializeEx) so ctor/dtor run on this thread.
    Aigs aigs;
    EyeContact eyeContact;
    SuperRes superRes;
    int srAppliedQuality = 0, srAppliedScale = 0; // last (quality,scale) Start ran with; for live restart

    IMFSourceReader* reader = nullptr;
    int width = 0, height = 0;
    LONG stride = 0;

    std::string link;
    {
        std::lock_guard<std::mutex> lock(g_state.mtx);
        link = g_state.symbolicLink;
    }

    if (!CreateReader(link, reader, width, height, stride)) {
        if (SUCCEEDED(coHr)) CoUninitialize();
        return;
    }

    // Camera exposure control (issue #16). The device source behind the reader may expose
    // IAMCameraControl; if it supports Manual exposure we can lock it to a fixed short value
    // so framerate holds steady in low light. Held worker-local (COM apartment affinity),
    // released before the reader. camControl stays null when unsupported -> feature greys out.
    IAMCameraControl* camControl = nullptr;
    long expMin = 0, expMax = 0, expStep = 1, expDef = 0, expCaps = 0;
    {
        IMFMediaSource* src = nullptr;
        if (SUCCEEDED(reader->GetServiceForStream(
                static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), GUID_NULL, IID_PPV_ARGS(&src)))
            && src) {
            if (SUCCEEDED(src->QueryInterface(IID_PPV_ARGS(&camControl))) && camControl) {
                if (SUCCEEDED(camControl->GetRange(CameraControl_Exposure,
                        &expMin, &expMax, &expStep, &expDef, &expCaps))
                    && (expCaps & CameraControl_Flags_Manual) && expStep > 0 && expMax > expMin) {
                    g_state.exposureSupported.store(true, std::memory_order_release);
                } else {
                    SafeRelease(camControl); // no usable manual-exposure range
                }
            }
            SafeRelease(src);
        }
    }
    bool   expAppliedEnabled = false; // last state pushed to the camera (starts in Auto)
    double expAppliedValue   = -1.0;  // sentinel forces the first manual push

    std::vector<uint8_t> scratch;

    while (!g_state.stopRequested.load(std::memory_order_acquire)) {
        // Apply exposure lock when the request changed (only touches the camera on change).
        if (camControl) {
            const bool   expWant = g_state.exposureLockEnabled.load(std::memory_order_acquire);
            const double expVal  = g_state.exposureValue.load(std::memory_order_acquire);
            if (expWant != expAppliedEnabled || (expWant && expVal != expAppliedValue)) {
                if (expWant) {
                    const double t = expVal < 0.0 ? 0.0 : (expVal > 1.0 ? 1.0 : expVal);
                    long raw = expMin + static_cast<long>(t * (expMax - expMin) + 0.5);
                    raw = expMin + ((raw - expMin) / expStep) * expStep; // snap to stepping delta
                    camControl->Set(CameraControl_Exposure, raw, CameraControl_Flags_Manual);
                } else {
                    camControl->Set(CameraControl_Exposure, expDef, CameraControl_Flags_Auto);
                }
                expAppliedEnabled = expWant;
                expAppliedValue   = expVal;
            }
        }

        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* sample = nullptr;

        HRESULT hr = reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            0, &streamIndex, &flags, &timestamp, &sample);

        if (FAILED(hr)) break;

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            SafeRelease(sample);
            break;
        }
        if (flags & MF_SOURCE_READERF_ERROR) {
            SafeRelease(sample);
            break;
        }

        if (sample) {
            if (CopyFrame(sample, width, height, stride, scratch)) {
                // Eye Contact runs first, on the raw frame (needs real eyes/landmarks).
                const bool ecWant = g_state.eyeContactEnabled.load(std::memory_order_acquire);
                if (ecWant && !eyeContact.IsReady()) {
                    if (!eyeContact.Start()) {
                        std::lock_guard<std::mutex> e(g_state.ecErrMtx);
                        const std::string& newErr = eyeContact.LastError();
                        if (g_state.ecError != newErr) g_state.ecError = newErr;
                    }
                } else if (!ecWant && eyeContact.IsReady()) {
                    eyeContact.Stop();
                    std::lock_guard<std::mutex> e(g_state.ecErrMtx);
                    if (!g_state.ecError.empty()) g_state.ecError.clear();
                }

                bool ecApplied = false;
                if (ecWant && eyeContact.IsReady()) {
                    ecApplied = eyeContact.ProcessFrame(scratch.data(), width, height);
                    std::lock_guard<std::mutex> e(g_state.ecErrMtx);
                    if (!ecApplied) {
                        const std::string& newErr = eyeContact.LastError();
                        if (g_state.ecError != newErr) g_state.ecError = newErr;
                    } else if (!g_state.ecError.empty()) {
                        g_state.ecError.clear();
                    }
                }
                g_state.eyeContactActive.store(ecApplied, std::memory_order_release);

                // Super Resolution runs BEFORE green screen: it sharpens/upscales the raw RGB,
                // then green screen authors the final alpha matte on the result. (Running SR
                // last clobbered the matte — NGX VSR does not preserve alpha — so a green-
                // screened overlay went opaque/black.) SR may change the frame dimensions.
                const bool srWant = g_state.superResEnabled.load(std::memory_order_acquire);
                const int  srScale = g_state.superResScale.load(std::memory_order_acquire);
                const int  srQuality = g_state.superResQuality.load(std::memory_order_acquire);
                // QualityLevel + scale are baked at Start; restart the effect if either changed live.
                if (srWant && superRes.IsReady() && (srQuality != srAppliedQuality || srScale != srAppliedScale))
                    superRes.Stop();
                if (srWant && !superRes.IsReady()) {
                    if (!superRes.Start(srQuality, srScale)) {
                        std::lock_guard<std::mutex> e(g_state.srErrMtx);
                        const std::string& ne = superRes.LastError();
                        if (g_state.srError != ne) g_state.srError = ne;
                    } else {
                        srAppliedQuality = srQuality; srAppliedScale = srScale;
                    }
                } else if (!srWant && superRes.IsReady()) {
                    superRes.Stop();
                    std::lock_guard<std::mutex> e(g_state.srErrMtx);
                    if (!g_state.srError.empty()) g_state.srError.clear();
                }

                int curW = width, curH = height;
                std::vector<uint8_t> srOut;
                bool srApplied = false;
                if (srWant && superRes.IsReady()) {
                    srApplied = superRes.ProcessFrame(scratch.data(), width, height, srOut, curW, curH);
                    std::lock_guard<std::mutex> e(g_state.srErrMtx);
                    if (!srApplied) {
                        const std::string& ne = superRes.LastError();
                        if (g_state.srError != ne) g_state.srError = ne;
                    } else if (!g_state.srError.empty()) { g_state.srError.clear(); }
                }
                g_state.superResActive.store(srApplied, std::memory_order_release);

                // The "current" frame is the SR output if it ran, else the raw capture.
                // Green screen composites onto it and writes the final premultiplied alpha.
                std::vector<uint8_t>& cur = srApplied ? srOut : scratch;

                // Lazily start/stop AIGS to match the enabled flag.
                const bool want = g_state.greenScreenEnabled.load(std::memory_order_acquire);
                if (want && !aigs.IsReady()) {
                    if (!aigs.Start()) {
                        std::lock_guard<std::mutex> e(g_state.gsErrMtx);
                        const std::string& newErr = aigs.LastError();
                        if (g_state.gsError != newErr) g_state.gsError = newErr;
                    }
                } else if (!want && aigs.IsReady()) {
                    aigs.Stop();
                    // Clear any stale error: green screen is off, error is no longer relevant.
                    std::lock_guard<std::mutex> e(g_state.gsErrMtx);
                    if (!g_state.gsError.empty()) g_state.gsError.clear();
                }

                bool applied = false;
                if (want && aigs.IsReady()) {
                    const double expand  = g_state.matteExpand.load(std::memory_order_acquire);
                    const double feather = g_state.matteFeather.load(std::memory_order_acquire);
                    applied = aigs.ProcessFrame(cur.data(), curW, curH, expand, feather);
                    if (!applied) {
                        std::lock_guard<std::mutex> e(g_state.gsErrMtx);
                        const std::string& newErr = aigs.LastError();
                        if (g_state.gsError != newErr) g_state.gsError = newErr;
                    } else {
                        // Frame succeeded — clear any stale error so status is consistent.
                        std::lock_guard<std::mutex> e(g_state.gsErrMtx);
                        if (!g_state.gsError.empty()) g_state.gsError.clear();
                    }
                }
                g_state.greenScreenActive.store(applied, std::memory_order_release);

                {
                    std::lock_guard<std::mutex> lock(g_state.mtx);
                    g_state.frame.swap(cur);
                    g_state.width = curW;
                    g_state.height = curH;
                    g_state.hasNewFrame = true;
                    g_state.framesProduced.fetch_add(1, std::memory_order_release);
                }
            }
            SafeRelease(sample);
        } else {
            // No sample but no error (e.g. stream tick / format change with no data).
            // Yield so a transient burst of empty reads can't busy-spin the CPU.
            std::this_thread::yield();
        }
    }

    // Restore auto-exposure on shutdown so the camera isn't left pinned to manual for
    // other apps, then release the control before the reader.
    if (camControl) {
        camControl->Set(CameraControl_Exposure, expDef, CameraControl_Flags_Auto);
        SafeRelease(camControl);
    }

    // Tear down all effects before releasing the reader (thread-affinity requirement).
    superRes.Stop();
    eyeContact.Stop();
    aigs.Stop();
    SafeRelease(reader);
    if (SUCCEEDED(coHr)) CoUninitialize();
}

bool Capture::Start(const std::string& symbolicLink) {
    // Hold the lifecycle lock for the whole start sequence so a concurrent Start/Stop
    // cannot race on the worker handle or stopRequested.
    std::lock_guard<std::mutex> life(g_lifecycleMtx);

    StopLocked(); // idempotent: tear down any previous session first (already locked).

    {
        std::lock_guard<std::mutex> lock(g_state.mtx);
        g_state.symbolicLink = symbolicLink;
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
    // Reset the active flag after the worker has joined: the worker may have set it
    // true just before stopping, so clear it here to avoid a stale status read.
    g_state.greenScreenActive.store(false, std::memory_order_release);
    // Clear the error so the next session starts clean (no stale error from prior run).
    {
        std::lock_guard<std::mutex> e(g_state.gsErrMtx);
        g_state.gsError.clear();
    }
    // Eye contact: reset active flag and clear error in its own separate lock scope
    // (ecErrMtx is a leaf lock — never nested under gsErrMtx).
    g_state.eyeContactActive.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> e(g_state.ecErrMtx);
        g_state.ecError.clear();
    }
    g_state.superResActive.store(false, std::memory_order_release);
    { std::lock_guard<std::mutex> e(g_state.srErrMtx); g_state.srError.clear(); }
    // Exposure support is per open camera; clear it so a stopped session reads "unsupported".
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
    std::lock_guard<std::mutex> e(g_state.srErrMtx); return g_state.srError;
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

    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 1))) return result;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attrs, &devices, &count);
    SafeRelease(attrs);
    if (FAILED(hr)) return result;

    for (UINT32 i = 0; i < count; ++i) {
        CameraDesc desc;

        WCHAR* link = nullptr;
        UINT32 linkLen = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &linkLen))
            && link) {
            int bytes = WideCharToMultiByte(CP_UTF8, 0, link, -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 0) {
                desc.id.resize(bytes - 1);
                WideCharToMultiByte(CP_UTF8, 0, link, -1, &desc.id[0], bytes, nullptr, nullptr);
            }
            CoTaskMemFree(link);
        }

        WCHAR* name = nullptr;
        UINT32 nameLen = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen))
            && name) {
            int bytes = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 0) {
                desc.name.resize(bytes - 1);
                WideCharToMultiByte(CP_UTF8, 0, name, -1, &desc.name[0], bytes, nullptr, nullptr);
            }
            CoTaskMemFree(name);
        }

        result.push_back(std::move(desc));
        SafeRelease(devices[i]);
    }

    if (devices) CoTaskMemFree(devices);
    return result;
}

double Capture::MeasuredFps() const {
    using clock = std::chrono::steady_clock;
    const double nowSec =
        std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    const uint64_t frames = g_state.framesProduced.load(std::memory_order_acquire);
    return g_state.fpsCounter.Sample(nowSec, frames);
}
