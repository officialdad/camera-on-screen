// Standalone capture diagnostic — runs WITHOUT the GUI.
// Opens the first video device, lists native types, then measures actual delivery fps for:
//   (A) RGB32 output with MF video processing (what the app uses), pinned to the best native rate.
//   (B) the native MJPG type directly (no conversion) — the raw camera ceiling.
// Distinguishes "camera/MF negotiates low" from "RGB32 conversion is the CPU cap".
//
// Build (from a VS x64 Developer prompt):
//   cl /EHsc /DUNICODE /D_UNICODE capture_probe.cpp ^
//      mfplat.lib mf.lib mfreadwrite.lib mfuuid.lib ole32.lib /Fe:capture_probe.exe
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <chrono>
#include <cstdio>
#include <string>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

template <typename T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

static std::string FourCC(const GUID& g) {
    // MF video subtypes embed a FOURCC in Data1.
    char c[5] = {0};
    memcpy(c, &g.Data1, 4);
    for (int i = 0; i < 4; ++i) if (c[i] < 32 || c[i] > 126) c[i] = '?';
    return std::string(c, 4);
}

static IMFMediaSource* OpenFirstDevice() {
    IMFAttributes* a = nullptr;
    MFCreateAttributes(&a, 1);
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr; UINT32 n = 0;
    MFEnumDeviceSources(a, &devs, &n);
    Rel(a);
    IMFMediaSource* src = nullptr;
    if (n > 0) {
        WCHAR name[256]; UINT32 len = 0;
        devs[0]->GetString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, name, 256, &len);
        wprintf(L"Device: %s\n", name);
        devs[0]->ActivateObject(IID_PPV_ARGS(&src));
    }
    for (UINT32 i = 0; i < n; ++i) Rel(devs[i]);
    if (devs) CoTaskMemFree(devs);
    return src;
}

static double MeasureFps(IMFSourceReader* r, int frames) {
    using clk = std::chrono::steady_clock;
    // warm up
    for (int i = 0; i < 5; ++i) {
        DWORD si = 0, fl = 0; LONGLONG ts = 0; IMFSample* s = nullptr;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &si, &fl, &ts, &s))) return -1;
        Rel(s);
    }
    auto t0 = clk::now();
    int got = 0;
    for (int i = 0; i < frames; ++i) {
        DWORD si = 0, fl = 0; LONGLONG ts = 0; IMFSample* s = nullptr;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &si, &fl, &ts, &s))) break;
        if (s) got++;
        Rel(s);
        if (fl & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    double sec = std::chrono::duration<double>(clk::now() - t0).count();
    return sec > 0 ? got / sec : -1;
}

static IMFSourceReader* MakeReader(IMFMediaSource* src, bool videoProcessing) {
    IMFAttributes* ra = nullptr; MFCreateAttributes(&ra, 1);
    if (videoProcessing) ra->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    IMFSourceReader* r = nullptr;
    MFCreateSourceReaderFromMediaSource(src, ra, &r);
    Rel(ra);
    return r;
}

int main() {
    if (FAILED(MFStartup(MF_VERSION))) { printf("MFStartup failed\n"); return 1; }

    // ---- list native types + pick best (fps, then res, cap 1080) ----
    IMFMediaSource* src = OpenFirstDevice();
    if (!src) { printf("no device\n"); MFShutdown(); return 1; }
    IMFSourceReader* lister = MakeReader(src, false);
    printf("\n-- native types --\n");
    UINT32 bestW=0,bestH=0,bestNum=0,bestDen=1; double bestScore=-1;
    for (DWORD i = 0; ; ++i) {
        IMFMediaType* t = nullptr;
        if (FAILED(lister->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t))) break;
        UINT32 w=0,h=0,num=0,den=0; GUID sub = {};
        MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &num, &den);
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        double fps = den ? (double)num/den : 0;
        if (w && h && den && fps >= 29.0 && w<=1920 && h<=1080)
            printf("  [%2lu] %4ux%-4u %5.1ffps  %s\n", i, w, h, fps, FourCC(sub).c_str());
        if (w && h && den && w<=1920 && h<=1080) {
            double score = fps*1e8 + (double)w*h;
            if (score > bestScore) { bestScore=score; bestW=w; bestH=h; bestNum=num; bestDen=den; }
        }
        Rel(t);
    }
    printf("  (only >=29fps shown; many lower-fps modes omitted)\n");
    printf("BEST pick: %ux%u @ %.1ffps\n", bestW, bestH, bestDen?(double)bestNum/bestDen:0);
    Rel(lister); Rel(src);

    // ---- TEST A: RGB32 + video processing, pinned to best rate (the app's path) ----
    src = OpenFirstDevice();
    IMFSourceReader* rA = MakeReader(src, true);
    IMFMediaType* o = nullptr; MFCreateMediaType(&o);
    o->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    o->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(o, MF_MT_FRAME_SIZE, bestW, bestH);
    MFSetAttributeRatio(o, MF_MT_FRAME_RATE, bestNum, bestDen);
    HRESULT hrA = rA->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, o);
    bool pinned = SUCCEEDED(hrA);
    if (!pinned) { // fallback unconstrained
        Rel(o); MFCreateMediaType(&o);
        o->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        o->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hrA = rA->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, o);
    }
    Rel(o);
    IMFMediaType* cur = nullptr; UINT32 nw=0,nh=0,nnum=0,nden=1;
    rA->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
    if (cur) { MFGetAttributeSize(cur,MF_MT_FRAME_SIZE,&nw,&nh); MFGetAttributeRatio(cur,MF_MT_FRAME_RATE,&nnum,&nden); }
    Rel(cur);
    printf("\n-- TEST A: RGB32 + video processing (app path) --\n");
    printf("  constrained-pin %s; negotiated %ux%u @ %.1ffps\n",
           pinned?"OK":"REJECTED(fell back)", nw, nh, nden?(double)nnum/nden:0);
    printf("  MEASURED delivery: %.1f fps\n", MeasureFps(rA, 90));
    Rel(rA); Rel(src);

    // ---- TEST B: native MJPG direct (no RGB32 conversion) — raw camera ceiling ----
    src = OpenFirstDevice();
    IMFSourceReader* rB = MakeReader(src, false);
    IMFMediaType* mj = nullptr; MFCreateMediaType(&mj);
    mj->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mj->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_MJPG);
    MFSetAttributeSize(mj, MF_MT_FRAME_SIZE, bestW, bestH);
    MFSetAttributeRatio(mj, MF_MT_FRAME_RATE, bestNum, bestDen);
    HRESULT hrB = rB->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mj);
    Rel(mj);
    printf("\n-- TEST B: native MJPG direct (raw camera, no conversion) --\n");
    if (SUCCEEDED(hrB)) printf("  MEASURED delivery: %.1f fps\n", MeasureFps(rB, 90));
    else                printf("  MJPG set failed hr=0x%08lX (camera may not expose MJPG at %ux%u)\n", hrB, bestW, bestH);
    Rel(rB); Rel(src);

    MFShutdown();
    return 0;
}
