#include "capture.h"
#include "aigs.h"

#include <atomic>
#include <mutex>
#include <thread>

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlwapi.h>

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
    std::atomic<bool>     greenScreenActive{false};  // set by worker
    std::mutex            gsErrMtx;
    std::string           gsError;                   // guarded by gsErrMtx
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

    // Request RGB32 output on the first video stream.
    IMFMediaType* outType = nullptr;
    if (FAILED(MFCreateMediaType(&outType))) { SafeRelease(reader); return false; }
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = reader->SetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outType);
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

    std::vector<uint8_t> scratch;

    while (!g_state.stopRequested.load(std::memory_order_acquire)) {
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
                // Lazily start/stop AIGS to match the enabled flag.
                const bool want = g_state.greenScreenEnabled.load(std::memory_order_acquire);
                if (want && !aigs.IsReady()) {
                    if (!aigs.Start()) {
                        std::lock_guard<std::mutex> e(g_state.gsErrMtx);
                        g_state.gsError = aigs.LastError();
                    }
                } else if (!want && aigs.IsReady()) {
                    aigs.Stop();
                }

                bool applied = false;
                if (want && aigs.IsReady()) {
                    applied = aigs.ProcessFrame(scratch.data(), width, height);
                    if (!applied) {
                        std::lock_guard<std::mutex> e(g_state.gsErrMtx);
                        g_state.gsError = aigs.LastError();
                    }
                }
                g_state.greenScreenActive.store(applied, std::memory_order_release);

                std::lock_guard<std::mutex> lock(g_state.mtx);
                g_state.frame.swap(scratch);
                g_state.width = width;
                g_state.height = height;
                g_state.hasNewFrame = true;
            }
            SafeRelease(sample);
        } else {
            // No sample but no error (e.g. stream tick / format change with no data).
            // Yield so a transient burst of empty reads can't busy-spin the CPU.
            std::this_thread::yield();
        }
    }

    // Tear down AIGS before releasing the reader (thread-affinity requirement).
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

bool Capture::GreenScreenActive() const {
    return g_state.greenScreenActive.load(std::memory_order_acquire);
}

std::string Capture::GreenScreenError() const {
    std::lock_guard<std::mutex> e(g_state.gsErrMtx);
    return g_state.gsError;
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
