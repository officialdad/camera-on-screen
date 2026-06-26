# FRUC fps Interpolation (30→60) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add optional AI frame-rate up-conversion (camera 30 fps → 60 fps overlay) via the NVIDIA Optical Flow FRUC library (`NvOFFRUC.dll`), gated + toggled like the existing Maxine effects.

**Architecture:** A worker-thread-local `Fruc` wrapper (mirrors `SuperRes`) runs **last** in the capture pipeline on the final composited BGRA frame. It holds the previous composite; when a new composite arrives it synthesises the mid frame and the worker **double-publishes** (mid, then new) paced ~half a camera interval apart. The C# frame pump drops from 33 ms to 16 ms while interpolation is active so both frames reach the swap chain. FRUC uses its **own** CUDA-11 context and a CPU round-trip (BGRA→device→FRUC→device→BGRA), so it never shares device pointers with the Maxine (CUDA-12/TRT) effects — co-version already proven (`docs/superpowers/specs/2026-06-26-camera-on-screen-13-fruc-coversion-findings.md`).

**Tech Stack:** C++17 shim (Media Foundation + CUDA driver API + NvOFFRUC), C# .NET 8 / WinUI 3 app, xUnit Core tests, PowerShell bundler.

## REVISION 2026-06-26 (post Task-3 gate)

Task 3's smoke proved the original `Interpolate(prev, cur)` "prime-every-call" model is **wrong**: it resets timestamps each call, but `NvOFFRUC` is a **stateful streaming** pipeline on a monotonic timeline, so re-priming triggers frame-repetition (`mid == prev`). **Authoritative API is now streaming feed-once:**

```cpp
// Feed each frame ONCE; FRUC holds the previous frame internally. Output = midpoint between the
// PREVIOUS submitted frame and this one. hasMid=false on the first call (no previous frame yet).
bool Fruc::Submit(const uint8_t* curBgra, int width, int height, std::vector<uint8_t>& outMid, bool& hasMid);
```

Timestamp convention (verbatim from the SDK sample, `FrameGenerator.cpp`): input ts is monotonic with `interval = 1.0`; output (interpolated) ts = `inputTs - interval*0.5` (midpoint, `DEFAULT_FRUC_SPEED = 0.5`). Ping-pong the two render buffers (`buf[1]`/`buf[2]`) per call so FRUC's previous input stays valid. This **also ~halves latency** (one `Process`/call, not two) and **simplifies Task 5** (the worker no longer keeps `prevComposite` — FRUC owns the history; it just calls `Submit(cur)` and publishes `mid` then `cur`). Tasks 1, 2, 3, 5 below are superseded by this where they say `Interpolate(prev,cur)`.

## Global Constraints

- **Windows + NVIDIA RTX only.** Without FRUC available the feature greys out; the app must run unchanged (passthrough).
- **Pristine builds: 0 warnings.** CI enforces `/warnaserror` + `TreatWarningsAsErrors`.
- **Build shim SDK config LAST before running** (deploy-the-right-shim); verify exports with `dumpbin`.
- **Shim ABI struct parity is load-bearing.** `CosParams`/`CosCaps` (C, `shim.h`) and their `[StructLayout(Sequential)]` mirrors in `PInvokeShim.cs` must match byte-for-byte on x64. **Append new fields at the END of each struct, in the SAME order on both sides.**
- **Maxine/FRUC effects run on the capture worker thread only** (CUDA affinity). UI flips an atomic flag; status crosses threads via atomics + a leaf-lock, never nested under `g_state.mtx`/`g_lifecycleMtx`.
- **Effect compiled behind `COS_HAS_MAXINE`**; FRUC behind a new `COS_HAS_FRUC`. Without it the wrapper is a never-ready passthrough stub (CI builds the stub).
- **CPU round-trip contract:** every effect takes/returns tightly-packed BGRA8 `width*height*4`. FRUC must honour this.
- **FRUC SDK (build):** headers at `<OF_SDK>\NvOFFRUC\Interface\NvOFFRUC.h`; runtime `NvOFFRUC.dll` + `cudart64_110.dll` from `<OF_SDK>\NvOFFRUC\NvOFFRUCSample\bin\win64`. Gate the SDK build path on a new `COS_FRUC_SDK_DIR` env var.
- **FRUC CUDA API (verified in the smoke):** CUDA mode, `pDevice=NULL` (uses current ctx), `ARGBSurface`, `CudaResourceCuDevicePtr`; min 3 registered `CUdeviceptr` buffers (1 interpolate + 2 render); `pFrame = &CUdeviceptr`; `nCuSurfacePitch = width*4`. CUDA 13.x toolkit remaps `cuCtxCreate`→4-arg `_v4` — use `cuDevicePrimaryCtxRetain`.

---

## File Structure

**Native (`native/shim/`):**
- Create `fruc.h` / `fruc.cpp` — the FRUC wrapper (mirrors `superres.{h,cpp}`). One responsibility: take previous + current BGRA, return interpolated BGRA.
- Modify `shim.h` — append `frame_interp_enabled` to `CosParams`, `frame_interp_available` + `fi_detail[256]` to `CosCaps`.
- Modify `shim.cpp` — wire `cos_set_params`, `cos_query_capabilities`.
- Modify `capture.h` / `capture.cpp` — `SetFrameInterp`, worker hold-frame + double-publish, new atomics + leaf-lock.
- Modify `native/shim/shim.vcxproj` — compile `fruc.cpp`, add `COS_HAS_FRUC` + include/lib when `COS_FRUC_SDK_DIR` set.

**Managed (`src/`):**
- Modify `CameraOnScreen.Core/Native/Contracts.cs` — `ShimParams.FrameInterpEnabled`, `ShimCapabilities.FrameInterpAvailable`.
- Modify `CameraOnScreen.App/Native/PInvokeShim.cs` — struct mirrors + marshalling.
- Modify `CameraOnScreen.Core/ViewModels/MainViewModel.cs` — `FrameInterpEnabled` observable prop + `OnFrameInterpEnabledChanged` → `ApplyLiveParams`; gate.
- Modify `CameraOnScreen.Core/Orchestration/Orchestrator.cs` — carry the flag through `BuildParams`/capability mapping.
- Modify `CameraOnScreen.Core/Native/FakeShim.cs` — honour the new field (test double).
- Modify `CameraOnScreen.App/MainWindow.xaml(.cs)` — toggle + 16 ms pump when active.

**Bundling (`scripts/`, `native/shim/bundle/`):**
- Modify `maxine-manifest.psd1` — add FRUC DLLs.
- Modify `assemble-maxine-stage.ps1` — copy FRUC DLLs into the stage from `COS_FRUC_SDK_DIR`.
- Modify `bundle-maxine.ps1` / closure trace as needed.

---

## Task 1: `Fruc` wrapper — header + passthrough stub

Establishes the interface and the CI-safe stub (no SDK). Mirrors `SuperRes`.

**Files:**
- Create: `native/shim/fruc.h`
- Create: `native/shim/fruc.cpp`
- Test: `native/shim/smoke/fruc_unit_smoke.cpp` (build-only stub check; the real GPU test is Task 3)

**Interfaces:**
- Produces: `class Fruc { static bool Probe(std::string&); bool Start(int w,int h); void Stop(); bool Interpolate(const uint8_t* prevBgra, const uint8_t* curBgra, int w, int h, std::vector<uint8_t>& outMid); bool IsReady() const; const std::string& LastError() const; };`

- [ ] **Step 1: Write `fruc.h`**

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Wraps NVIDIA Optical Flow FRUC (NvOFFRUC.dll) frame interpolation. CPU round-trip:
// BGRA in -> FRUC's own CUDA-11 context (ARGB devptr) -> BGRA mid frame out. Holds NO state
// across calls except the FRUC pipeline; the caller owns prev/cur frame history. Without
// COS_HAS_FRUC this is a never-ready passthrough stub (Probe returns false). Co-version with
// the Maxine CUDA-12/TRT effects is proven (separate cudart64_110.dll, no TensorRT).
class Fruc {
public:
    Fruc();
    ~Fruc();
    static bool Probe(std::string& detail);     // tries to LoadLibrary+Create FRUC
    bool Start(int width, int height);          // create FRUC + register CUDA buffers
    void Stop();
    // Synthesises the temporal-midpoint frame between prevBgra and curBgra (both w*h BGRA8).
    // Writes w*h BGRA8 into outMid. Returns false on failure (outMid untouched). If FRUC
    // reports frame repetition (no usable motion), outMid is a copy of curBgra and returns true.
    bool Interpolate(const uint8_t* prevBgra, const uint8_t* curBgra, int width, int height,
                     std::vector<uint8_t>& outMid);
    bool IsReady() const { return ready_; }
    const std::string& LastError() const { return lastError_; }
private:
    bool ready_ = false;
    int  width_ = 0, height_ = 0;
    std::string lastError_;
    void* impl_ = nullptr;  // owns CUcontext + NvOFFRUCHandle + device buffers
};
```

- [ ] **Step 2: Write the stub `fruc.cpp` (no SDK path)**

```cpp
#include "fruc.h"
#include <cstring>

#ifndef COS_HAS_FRUC
Fruc::Fruc() {}
Fruc::~Fruc() {}
bool Fruc::Probe(std::string& detail) { detail = "FRUC not built in (COS_HAS_FRUC unset)"; return false; }
bool Fruc::Start(int, int) { lastError_ = "FRUC not built in"; return false; }
void Fruc::Stop() { ready_ = false; }
bool Fruc::Interpolate(const uint8_t*, const uint8_t*, int, int, std::vector<uint8_t>&) {
    lastError_ = "FRUC not built in"; return false;
}
#endif // !COS_HAS_FRUC
```
(The `COS_HAS_FRUC` implementation lands in Task 2.)

- [ ] **Step 3: Build the shim stub (no FRUC SDK) and confirm it compiles + exports unchanged**

Run (PowerShell, repo root):
```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:CosVfxSdkDir= /p:CosArSdkDir=
```
Expected: build succeeds, 0 warnings. (`fruc.cpp` added to the vcxproj in this step — see Task 5 for the vcxproj edit; for now add `<ClCompile Include="fruc.cpp" />`.)

- [ ] **Step 4: Commit**

```bash
git add native/shim/fruc.h native/shim/fruc.cpp native/shim/shim.vcxproj
git commit -m "feat(#13): Fruc wrapper interface + passthrough stub"
```

---

## Task 2: `Fruc` real implementation (COS_HAS_FRUC)

The GPU path, lifted verbatim from the proven smoke (`native/shim/smoke/of_fruc_smoke.cpp`).

**Files:**
- Modify: `native/shim/fruc.cpp`

**Interfaces:**
- Consumes: NvOFFRUC.h API; CUDA driver API (`cuda.lib`).
- Produces: working `Fruc::Probe/Start/Interpolate/Stop`.

- [ ] **Step 1: Implement the `COS_HAS_FRUC` block in `fruc.cpp`**

```cpp
#ifdef COS_HAS_FRUC
#include <windows.h>
#include <cuda.h>
#include "NvOFFRUC.h"

namespace {
struct FrucImpl {
    HMODULE dll = nullptr;
    CUcontext ctx = nullptr; CUdevice dev = 0;
    NvOFFRUCHandle h = nullptr;
    CUdeviceptr buf[3] = {0,0,0};   // [0]=interpolate, [1..2]=render (sample's GetResource order)
    int toggle = 0;                 // which render buffer holds "current"
    PtrToFuncNvOFFRUCCreate   pCreate   = nullptr;
    PtrToFuncNvOFFRUCRegisterResource   pReg = nullptr;
    PtrToFuncNvOFFRUCUnregisterResource pUnreg = nullptr;
    PtrToFuncNvOFFRUCProcess  pProcess  = nullptr;
    PtrToFuncNvOFFRUCDestroy  pDestroy  = nullptr;
};
// Resolves NvOFFRUC.dll: COS_FRUC_RUNTIME_DIR, else <shimDir>\maxine, else PATH. (Mirror the
// VFX/AR app-relative resolver in vfx_paths.cpp — add a fruc tier there in Task 5.)
HMODULE LoadFruc(std::string& err);  // implement alongside vfx_paths
} // namespace

Fruc::Fruc() {}
Fruc::~Fruc() { Stop(); }

bool Fruc::Probe(std::string& detail) {
    Fruc f;
    if (!f.Start(1280, 720)) { detail = f.LastError(); return false; }
    detail = "FRUC available"; f.Stop(); return true;
}

bool Fruc::Start(int width, int height) {
    auto* impl = new FrucImpl();
    impl->dll = LoadFruc(lastError_);
    if (!impl->dll) { delete impl; return false; }
    impl->pCreate = (PtrToFuncNvOFFRUCCreate)GetProcAddress(impl->dll, CreateProcName);
    impl->pReg    = (PtrToFuncNvOFFRUCRegisterResource)GetProcAddress(impl->dll, RegisterResourceProcName);
    impl->pUnreg  = (PtrToFuncNvOFFRUCUnregisterResource)GetProcAddress(impl->dll, UnregisterResourceProcName);
    impl->pProcess= (PtrToFuncNvOFFRUCProcess)GetProcAddress(impl->dll, ProcessProcName);
    impl->pDestroy= (PtrToFuncNvOFFRUCDestroy)GetProcAddress(impl->dll, DestroyProcName);
    if (!impl->pCreate || !impl->pReg || !impl->pUnreg || !impl->pProcess || !impl->pDestroy) {
        lastError_ = "NvOFFRUC exports missing"; delete impl; return false;
    }
    if (cuInit(0) != CUDA_SUCCESS || cuDeviceGet(&impl->dev, 0) != CUDA_SUCCESS ||
        cuDevicePrimaryCtxRetain(&impl->ctx, impl->dev) != CUDA_SUCCESS ||
        cuCtxSetCurrent(impl->ctx) != CUDA_SUCCESS) {
        lastError_ = "CUDA ctx setup failed"; delete impl; return false;
    }
    NvOFFRUC_CREATE_PARAM cp{};
    cp.uiWidth = width; cp.uiHeight = height; cp.pDevice = nullptr;
    cp.eResourceType = CudaResource; cp.eSurfaceFormat = ARGBSurface;
    cp.eCUDAResourceType = CudaResourceCuDevicePtr;
    if (impl->pCreate(&cp, &impl->h) != NvOFFRUC_SUCCESS) {
        lastError_ = "NvOFFRUCCreate failed"; cuDevicePrimaryCtxRelease(impl->dev); delete impl; return false;
    }
    const size_t bytes = (size_t)width * height * 4;
    for (int i = 0; i < 3; ++i)
        if (cuMemAlloc(&impl->buf[i], bytes) != CUDA_SUCCESS) {
            lastError_ = "cuMemAlloc failed"; impl->pDestroy(impl->h);
            cuDevicePrimaryCtxRelease(impl->dev); delete impl; return false;
        }
    NvOFFRUC_REGISTER_RESOURCE_PARAM reg{};
    reg.pArrResource[0] = &impl->buf[0]; reg.pArrResource[1] = &impl->buf[1];
    reg.pArrResource[2] = &impl->buf[2]; reg.uiCount = 3; reg.pD3D11FenceObj = nullptr;
    if (impl->pReg(impl->h, &reg) != NvOFFRUC_SUCCESS) {
        lastError_ = "RegisterResource failed";
        for (auto& b : impl->buf) cuMemFree(b);
        impl->pDestroy(impl->h); cuDevicePrimaryCtxRelease(impl->dev); delete impl; return false;
    }
    impl_ = impl; width_ = width; height_ = height; ready_ = true; return true;
}
```

- [ ] **Step 2: Implement `Interpolate` (the per-frame hot path)**

```cpp
bool Fruc::Interpolate(const uint8_t* prevBgra, const uint8_t* curBgra, int width, int height,
                       std::vector<uint8_t>& outMid) {
    if (!ready_) { lastError_ = "FRUC not started"; return false; }
    if (width != width_ || height != height_) { lastError_ = "frame size changed"; return false; }
    auto* impl = static_cast<FrucImpl*>(impl_);
    cuCtxSetCurrent(impl->ctx);
    const size_t bytes = (size_t)width * height * 4;
    // ponytail: BGRA uploaded as-is to FRUC ARGBSurface. Byte order is verified in Task 3's
    // human visual gate; if colours swap, add a B<->R swizzle here (or use NV12Surface).
    // Upload prev->render[0], cur->render[1]; FRUC interpolates into interpolate buf.
    NvOFFRUC_PROCESS_IN_PARAMS  in{};
    NvOFFRUC_PROCESS_OUT_PARAMS out{};
    bool rep = false;
    // Prime with prev, then process cur to get the midpoint. FRUC is stateful across calls.
    cuMemcpyHtoD(impl->buf[1], prevBgra, bytes);
    in.stFrameDataInput.pFrame = &impl->buf[1];
    in.stFrameDataInput.nTimeStamp = 0.0;
    in.stFrameDataInput.nCuSurfacePitch = (size_t)width * 4;
    out.stFrameDataOutput.pFrame = &impl->buf[0];
    out.stFrameDataOutput.nTimeStamp = 0.5;
    out.stFrameDataOutput.nCuSurfacePitch = (size_t)width * 4;
    out.stFrameDataOutput.bHasFrameRepetitionOccurred = &rep;
    if (impl->pProcess(impl->h, &in, &out) != NvOFFRUC_SUCCESS) { lastError_ = "Process(prime) failed"; return false; }

    cuMemcpyHtoD(impl->buf[2], curBgra, bytes);
    in.stFrameDataInput.pFrame = &impl->buf[2];
    in.stFrameDataInput.nTimeStamp = 1.0;
    out.stFrameDataOutput.nTimeStamp = 0.5;
    if (impl->pProcess(impl->h, &in, &out) != NvOFFRUC_SUCCESS) { lastError_ = "Process(mid) failed"; return false; }

    outMid.resize(bytes);
    if (cuMemcpyDtoH(outMid.data(), impl->buf[0], bytes) != CUDA_SUCCESS) { lastError_ = "download failed"; return false; }
    return true;
}

void Fruc::Stop() {
    if (!impl_) { ready_ = false; return; }
    auto* impl = static_cast<FrucImpl*>(impl_);
    if (impl->h) { NvOFFRUC_UNREGISTER_RESOURCE_PARAM u{};
        u.pArrResource[0]=&impl->buf[0]; u.pArrResource[1]=&impl->buf[1]; u.pArrResource[2]=&impl->buf[2];
        u.uiCount=3; impl->pUnreg(impl->h, &u); impl->pDestroy(impl->h); }
    for (auto& b : impl->buf) if (b) cuMemFree(b);
    if (impl->ctx) cuDevicePrimaryCtxRelease(impl->dev);
    if (impl->dll) FreeLibrary(impl->dll);
    delete impl; impl_ = nullptr; ready_ = false;
}
#endif // COS_HAS_FRUC
```
> **Design note for the implementer:** the prime/process-twice per call is the simple, stateless-to-the-caller model. FRUC is internally stateful (it keeps the last input), so a future optimisation is to prime once and feed one new frame per call. Keep the simple version until Task 3's perf gate says otherwise. `// ponytail: prime-every-call; feed-once if Process latency matters.`

- [ ] **Step 3: Commit**

```bash
git add native/shim/fruc.cpp
git commit -m "feat(#13): Fruc GPU implementation (NvOFFRUC, CUDA-11 ctx)"
```

---

## Task 3: GPU integration smoke — color + latency gate (HUMAN)

Proves `Fruc::Interpolate` produces a correct-colour mid frame and measures per-call latency. This is the gate that retires the two real unknowns (ARGB byte order, Process cost).

**Files:**
- Modify: `native/shim/smoke/of_fruc_smoke.cpp` (extend) OR create `native/shim/smoke/fruc_interp_smoke.cpp`
- Test artifact: writes `prev.bmp`, `mid.bmp`, `cur.bmp` for visual inspection.

- [ ] **Step 1: Write a smoke that feeds two distinct gradient frames through `Fruc::Interpolate` and dumps BMPs**

```cpp
// Build via build_of_fruc_smoke.bat (add fruc.cpp + /DCOS_HAS_FRUC). Make prev = left-to-right
// red gradient, cur = the same shifted +40px right. The mid frame should show ~+20px shift and
// preserve red (NOT blue). Dump all three to BMP.
```
(Use the existing `Bitmap.h` writer from the OF SDK sample, or a 10-line BMP writer.)

- [ ] **Step 2: Run and inspect**

Run:
```powershell
$env:COS_FRUC_RUNTIME_DIR="C:\actions-runner\_sdk\Optical_Flow_SDK_5.0.7\Optical_Flow_SDK_5.0.7\NvOFFRUC\NvOFFRUCSample\bin\win64"
native\shim\smoke\fruc_interp_smoke.exe
```
Expected: stdout prints `Interpolate ok, avg N ms`; `mid.bmp` shows the shifted gradient, **red preserved**.

- [ ] **Step 3: HUMAN GATE — confirm colour + motion**

Open the three BMPs. **STOP and report to the user:**
- Is `mid.bmp` red (correct) or blue (→ add B↔R swizzle in `Interpolate`)?
- Is the gradient shifted halfway between prev and cur (interpolation working)?
- Record avg ms (feeds the latency budget; >16 ms means interpolation can't sustain 60 fps and the design must reconsider feed-once).

Do not proceed until the user confirms. Fix swizzle inline if needed, re-run.

- [ ] **Step 4: Commit**

```bash
git add native/shim/smoke/fruc_interp_smoke.cpp native/shim/smoke/build_of_fruc_smoke.bat
git commit -m "test(#13): FRUC interpolation color+latency smoke (human gate passed)"
```

---

## Task 4: Shim ABI — params, caps, probe

Append the two ABI fields and wire the probe. Mirrors `super_res_*`.

**Files:**
- Modify: `native/shim/shim.h:19-40`
- Modify: `native/shim/shim.cpp:61-70` (set_params), `:105-128` (query_capabilities)

**Interfaces:**
- Produces: `CosParams.frame_interp_enabled`, `CosCaps.frame_interp_available` + `fi_detail[256]`.

- [ ] **Step 1: Append fields in `shim.h`**

In `CosParams` (after `exposure_value`):
```c
    int    frame_interp_enabled;      // 1 = FRUC 30->60 interpolation on
```
In `CosCaps` (after `super_res_available`):
```c
    int  frame_interp_available;      // 1 if FRUC can run
    char fi_detail[256];              // FRUC status/error (UTF-8, NUL-terminated)
```

- [ ] **Step 2: Wire `cos_set_params` and `cos_query_capabilities` in `shim.cpp`**

In `cos_set_params`, after the `SetSuperRes` line:
```cpp
    g_capture.SetFrameInterp(p->frame_interp_enabled != 0);
```
In `cos_query_capabilities`, after the SuperRes probe, before the `return`:
```cpp
    std::string fiDetail;
    out->frame_interp_available = Fruc::Probe(fiDetail) ? 1 : 0;
    size_t fn = fiDetail.size() < 255 ? fiDetail.size() : 255;
    std::memcpy(out->fi_detail, fiDetail.data(), fn);
    out->fi_detail[fn] = '\0';
```
Add `#include "fruc.h"` at the top, and include `out->frame_interp_available` in the final OR:
```cpp
    return (gsOk || ecOk || out->super_res_available || out->frame_interp_available) ? 1 : 0;
```

- [ ] **Step 3: Build shim (stub config), expect 0 warnings**

Run: the MSBuild stub command from Task 1 Step 3.

- [ ] **Step 4: Commit**

```bash
git add native/shim/shim.h native/shim/shim.cpp
git commit -m "feat(#13): CosParams/CosCaps frame-interp ABI + probe"
```

---

## Task 5: Capture worker — hold-frame, interpolate, double-publish

The core pipeline change. FRUC runs **after** green screen on the final composite.

**Files:**
- Modify: `native/shim/capture.h:44-49` (add methods)
- Modify: `native/shim/capture.cpp` — g_state atomics (~line 25-70), worker loop publish (~455-519), `Set*`/error accessors (~620), `Stop` clear (~575)
- Modify: `native/shim/vfx_paths.cpp` — add `LoadFruc` resolver (FRUC tier)
- Modify: `native/shim/shim.vcxproj` — `COS_HAS_FRUC` + include/lib when `COS_FRUC_SDK_DIR` set

**Interfaces:**
- Consumes: `Fruc` (Task 1-2).
- Produces: `Capture::SetFrameInterp(bool)`, `bool FrameInterpActive() const`, `std::string FrameInterpError() const`.

- [ ] **Step 1: Add atomics + leaf-lock to the `g_state` struct (capture.cpp ~line 25-70)**

```cpp
    std::atomic<bool>     frameInterpEnabled{false};
    std::atomic<bool>     frameInterpActive{false};
    std::mutex            fiErrMtx;   // leaf lock, never nested under mtx/lifecycle
    std::string           fiError;
```

- [ ] **Step 2: Declare the methods in `capture.h` (after the SuperRes block)**

```cpp
    // Toggles FRUC 30->60 frame interpolation. Thread-safe; worker owns the object.
    void SetFrameInterp(bool enabled);
    bool FrameInterpActive() const;
    std::string FrameInterpError() const;
```

- [ ] **Step 3: Worker loop — replace the single publish block (capture.cpp ~504-511) with hold-frame + double-publish**

Add, in `WorkerLoop()` before the loop (near `std::vector<uint8_t> scratch;`):
```cpp
    Fruc fruc;
    std::vector<uint8_t> prevComposite;   // last published real composite (for interpolation)
    int  prevW = 0, prevH = 0;
```
Replace the publish block with:
```cpp
                // FRUC runs LAST, on the final composite (cur, curW x curH). Lazily start/stop.
                const bool fiWant = g_state.frameInterpEnabled.load(std::memory_order_acquire);
                if (fiWant && !fruc.IsReady()) {
                    if (!fruc.Start(curW, curH)) {
                        std::lock_guard<std::mutex> e(g_state.fiErrMtx);
                        if (g_state.fiError != fruc.LastError()) g_state.fiError = fruc.LastError();
                    }
                } else if ((!fiWant && fruc.IsReady()) || (fruc.IsReady() && (curW != prevW || curH != prevH))) {
                    fruc.Stop(); prevComposite.clear();
                    std::lock_guard<std::mutex> e(g_state.fiErrMtx);
                    if (!g_state.fiError.empty()) g_state.fiError.clear();
                }

                auto publish = [&](std::vector<uint8_t>& f, int w, int h) {
                    std::lock_guard<std::mutex> lock(g_state.mtx);
                    g_state.frame = f; g_state.width = w; g_state.height = h;
                    g_state.hasNewFrame = true;
                    g_state.framesProduced.fetch_add(1, std::memory_order_release);
                };

                bool fiApplied = false;
                if (fiWant && fruc.IsReady() && !prevComposite.empty()
                    && prevW == curW && prevH == curH) {
                    std::vector<uint8_t> mid;
                    if (fruc.Interpolate(prevComposite.data(), cur.data(), curW, curH, mid)) {
                        publish(mid, curW, curH);                       // N+0.5 first
                        std::this_thread::sleep_for(std::chrono::milliseconds(8)); // pace toward 60Hz
                        fiApplied = true;
                    } else {
                        std::lock_guard<std::mutex> e(g_state.fiErrMtx);
                        if (g_state.fiError != fruc.LastError()) g_state.fiError = fruc.LastError();
                    }
                }
                g_state.frameInterpActive.store(fiApplied, std::memory_order_release);

                publish(cur, curW, curH);                                // then the real frame N+1
                if (fiWant && fruc.IsReady()) { prevComposite = cur; prevW = curW; prevH = curH; }
```
> **ponytail:** `sleep_for(8ms)` is a crude pacer — the worker also blocks ~33 ms in `ReadSample`, so net output ≈ 60 Hz at 30 fps capture. If timing jitters visibly, replace the single latest-frame buffer with a 2-slot FIFO drained one-per-`LatestFrame` and pump C# at a fixed 16 ms. Don't build the FIFO until the human gate (Task 8) shows jitter.

- [ ] **Step 4: Add `fruc.Stop()` in the worker teardown (capture.cpp ~529) and the `Set*`/error accessors (~620)**

Teardown (before `superRes.Stop();`): `fruc.Stop();`
Accessors (after the SuperRes ones):
```cpp
void Capture::SetFrameInterp(bool enabled) {
    g_state.frameInterpEnabled.store(enabled, std::memory_order_release);
}
bool Capture::FrameInterpActive() const {
    return g_state.frameInterpActive.load(std::memory_order_acquire);
}
std::string Capture::FrameInterpError() const {
    std::lock_guard<std::mutex> e(g_state.fiErrMtx); return g_state.fiError;
}
```
In `Capture::Stop` (~575), add: `{ std::lock_guard<std::mutex> e(g_state.fiErrMtx); g_state.fiError.clear(); }`
Add `#include "fruc.h"` and `#include <chrono>` to capture.cpp.

- [ ] **Step 5: Surface the error in `cos_get_status` (shim.cpp ~85)**

After the SuperRes error line:
```cpp
    if (err.empty()) err = g_capture.FrameInterpError();
```

- [ ] **Step 6: Add the `LoadFruc` resolver + `COS_HAS_FRUC` to the vcxproj**

In `vfx_paths.cpp`, add `LoadFruc(std::string& err)`: try `COS_FRUC_RUNTIME_DIR`, then `<ShimModuleDir()>\maxine\NvOFFRUC.dll`, then bare `LoadLibraryExW(L"NvOFFRUC.dll", ..., LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`. Use `LOAD_WITH_ALTERED_SEARCH_PATH` for full paths so `cudart64_110.dll` resolves beside it.
In `shim.vcxproj`: add a `COS_HAS_FRUC` define + `$(CosFrucSdkDir)\NvOFFRUC\Interface` include + `cuda.lib` link, conditioned on `CosFrucSdkDir` (mirror the `CosVfxSdkDir` blocks).

- [ ] **Step 7: Build the SDK config LAST, export-verify**

```powershell
$env:COS_VFX_SDK_DIR="C:\actions-runner\_sdk\VideoFX"; $env:COS_AR_SDK_DIR="C:\actions-runner\_sdk\Maxine-AR-SDK-1.1.1.0"
$env:COS_FRUC_SDK_DIR="C:\actions-runner\_sdk\Optical_Flow_SDK_5.0.7\Optical_Flow_SDK_5.0.7"
& "C:/.../MSBuild.exe" native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64 `
  /p:CosVfxSdkDir=$env:COS_VFX_SDK_DIR /p:CosArSdkDir=$env:COS_AR_SDK_DIR /p:CosFrucSdkDir=$env:COS_FRUC_SDK_DIR
dumpbin /exports native\shim\x64\Debug\CameraOnScreen.Shim.dll | Select-String "GreenScreen|GazeRedirection"
```
Expected: `GreenScreen` + `GazeRedirection` present, no "not built in".

- [ ] **Step 8: Commit**

```bash
git add native/shim/capture.h native/shim/capture.cpp native/shim/shim.cpp native/shim/vfx_paths.cpp native/shim/shim.vcxproj
git commit -m "feat(#13): worker hold-frame + double-publish FRUC interpolation"
```

---

## Task 6: Managed contracts + PInvoke mirror

Carry the flag/gate through the Core contracts and the P/Invoke structs. TDD via Core tests.

**Files:**
- Modify: `src/CameraOnScreen.Core/Native/Contracts.cs:7-10,23-35`
- Modify: `src/CameraOnScreen.App/Native/PInvokeShim.cs:21-42,87-89,112-122`
- Modify: `src/CameraOnScreen.Core/Native/FakeShim.cs`
- Test: `tests/CameraOnScreen.Core.Tests/Orchestration/OrchestratorTests.cs`

**Interfaces:**
- Produces: `ShimParams.FrameInterpEnabled`, `ShimCapabilities.FrameInterpAvailable`.

- [ ] **Step 1: Write a failing Orchestrator test**

```csharp
[Fact]
public void BuildParams_carries_frame_interp_flag()
{
    var vm = NewViewModel(); // existing helper
    vm.FrameInterpEnabled = true;
    var p = Orchestrator.BuildParams(vm); // or the equivalent existing path
    Assert.True(p.FrameInterpEnabled);
}
```

- [ ] **Step 2: Run it, expect FAIL (no `FrameInterpEnabled`)**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~frame_interp"`
Expected: compile error / FAIL.

- [ ] **Step 3: Add the fields**

`Contracts.cs` — `ShimCapabilities`: add `, bool FrameInterpAvailable = false`. `ShimParams`: add `, bool FrameInterpEnabled = false` (at the end, default keeps call sites compiling).

- [ ] **Step 4: Mirror the P/Invoke structs (`PInvokeShim.cs`)**

`CosParams` struct: append `public int frame_interp_enabled;`. `CosCaps`: append `public int FrameInterpAvailable;` then `[MarshalAs(UnmanagedType.ByValTStr, SizeConst=256)] public string FiDetail;` (match `fi_detail[256]`). In the `ToCosParams` builder add `frame_interp_enabled = p.FrameInterpEnabled ? 1 : 0,`. In `QueryCapabilities` mapping add `caps.FrameInterpAvailable != 0` as the new ctor arg.

- [ ] **Step 5: Honour the field in `FakeShim.cs`** (store last params; expose for tests).

- [ ] **Step 6: Run tests, expect PASS**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
Expected: PASS, 0 warnings.

- [ ] **Step 7: Commit**

```bash
git add src/CameraOnScreen.Core/Native/Contracts.cs src/CameraOnScreen.App/Native/PInvokeShim.cs src/CameraOnScreen.Core/Native/FakeShim.cs tests/CameraOnScreen.Core.Tests/Orchestration/OrchestratorTests.cs
git commit -m "feat(#13): frame-interp through Core contracts + PInvoke mirror"
```

---

## Task 7: ViewModel toggle + capability gate

`FrameInterpEnabled` observable property, live push, and gate off when unavailable. Mirrors `SuperResEnabled`.

**Files:**
- Modify: `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs`
- Modify: `src/CameraOnScreen.Core/Orchestration/Orchestrator.cs` (capability → VM mapping)
- Test: `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`

- [ ] **Step 1: Write a failing VM test**

```csharp
[Fact]
public void FrameInterp_pushes_params_when_running_and_available()
{
    var (vm, fake) = NewRunningViewModel(frameInterpAvailable: true);
    vm.FrameInterpEnabled = true;
    Assert.True(fake.LastParams.FrameInterpEnabled);
}

[Fact]
public void FrameInterp_forced_off_when_unavailable()
{
    var (vm, fake) = NewRunningViewModel(frameInterpAvailable: false);
    vm.FrameInterpEnabled = true;          // user toggles
    Assert.False(fake.LastParams.FrameInterpEnabled); // ApplyParams forces off
}
```

- [ ] **Step 2: Run, expect FAIL.**

Run: `dotnet test ... --filter "FullyQualifiedName~FrameInterp"` → FAIL.

- [ ] **Step 3: Add the observable property + change handler (MainViewModel.cs, mirroring `SuperResEnabled`)**

```csharp
[ObservableProperty] private bool _frameInterpEnabled;
partial void OnFrameInterpEnabledChanged(bool value) => ApplyLiveParams();
```
Add `FrameInterpAvailable` (set from the probe, like `SuperResAvailable`) and include `FrameInterpEnabled` in the `BuildParams()` / `ToShimParams()` construction. In `ApplyParams`, force `FrameInterpEnabled=false` when `!FrameInterpAvailable` (mirror the existing `EffectsAvailable` forcing).

- [ ] **Step 4: Map the capability in `Orchestrator.cs`** — set `vm.FrameInterpAvailable = caps.FrameInterpAvailable` where `SuperResAvailable` is mapped.

- [ ] **Step 5: Run tests, expect PASS.** `dotnet test ...` → PASS, 0 warnings.

- [ ] **Step 6: Commit**

```bash
git add src/CameraOnScreen.Core/ViewModels/MainViewModel.cs src/CameraOnScreen.Core/Orchestration/Orchestrator.cs tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs
git commit -m "feat(#13): frame-interp VM toggle + capability gate"
```

---

## Task 8: XAML toggle + 16 ms pump — end-to-end (HUMAN)

UI toggle + the pump-rate bump that actually delivers 60 fps. Visual confirmation gate.

**Files:**
- Modify: `src/CameraOnScreen.App/MainWindow.xaml` (toggle, mirror the SuperRes ToggleSwitch)
- Modify: `src/CameraOnScreen.App/MainWindow.xaml.cs:75-92` (pump interval)

- [ ] **Step 1: Add the ToggleSwitch in `MainWindow.xaml`**

Mirror the SuperRes toggle: `IsOn="{x:Bind Vm.FrameInterpEnabled, Mode=TwoWay}"`, `IsEnabled` bound to `Vm.FrameInterpAvailable` (OneWay), label "Smooth 60 fps (AI)".

- [ ] **Step 2: Make the frame pump 16 ms while interpolation is active (MainWindow.xaml.cs)**

In the timer tick, after presenting, adjust the interval:
```csharp
var want = (Vm.IsRunning && Vm.FrameInterpEnabled && Vm.FrameInterpAvailable)
    ? TimeSpan.FromMilliseconds(16) : TimeSpan.FromMilliseconds(33);
if (_timer!.Interval != want) _timer.Interval = want;
```
> **ponytail:** reuses the one pump timer; no second timer. The 16 ms only matters while FRUC is on.

- [ ] **Step 3: Build shim SDK config LAST, then the App**

```powershell
# (shim SDK build from Task 5 Step 7 first)
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild
```
Expected: 0 warnings; shim DLL copied next to the exe.

- [ ] **Step 4: HUMAN GATE — run and observe**

Run the app with the maxine runtime + FRUC dll resolvable (`<exe>\maxine` or `COS_FRUC_RUNTIME_DIR`). Toggle "Smooth 60 fps".
**STOP and ask the user to confirm:**
- fps readout (#14 counter) roughly doubles (~60).
- Motion looks smoother, no severe smearing on hand/face motion (artifact check).
- No stutter/jitter (if jittery → implement the 2-slot FIFO noted in Task 5 Step 3).
- Toggle greys out on a non-RTX / FRUC-unavailable machine.

Do not proceed until confirmed.

- [ ] **Step 5: Commit**

```bash
git add src/CameraOnScreen.App/MainWindow.xaml src/CameraOnScreen.App/MainWindow.xaml.cs
git commit -m "feat(#13): frame-interp UI toggle + 60Hz pump (human gate passed)"
```

---

## Task 9: Bundle FRUC into the maxine runtime

Ship `NvOFFRUC.dll` + `cudart64_110.dll` so end users need only an RTX GPU.

**Files:**
- Modify: `native/shim/bundle/maxine-manifest.psd1` (add FRUC DLLs to the closure list)
- Modify: `scripts/assemble-maxine-stage.ps1` (copy FRUC DLLs from `COS_FRUC_SDK_DIR`)
- Modify: `scripts/bundle-maxine.ps1` if the prune list is explicit

- [ ] **Step 1: Add FRUC DLLs to the manifest** — append `NvOFFRUC.dll`, `cudart64_110.dll` to the `Dlls` list in `maxine-manifest.psd1`.

- [ ] **Step 2: Copy FRUC into the stage** — in `assemble-maxine-stage.ps1`, copy `$env:COS_FRUC_SDK_DIR\NvOFFRUC\NvOFFRUCSample\bin\win64\{NvOFFRUC.dll,cudart64_110.dll}` into the flat stage root. (FRUC has no per-arch engine — its kernels are an embedded fatbin; no model glob needed.)

- [ ] **Step 3: Re-run the closure trace / bundle probe against the produced bundle**

```powershell
# assemble stage, bundle, then verify all THREE effect families + FRUC load with COS_* unset
scripts\assemble-maxine-stage.ps1 ...
scripts\bundle-maxine.ps1 -OutDir <out> -MaxineStage <stage>
# trace_closure / bundle_probe should still report greenScreen+eyeContact+superRes=1
# and the new fruc_interp_smoke (run against <out>\maxine) should pass with COS_* unset.
```
Expected: all load from `<exe>\maxine`, exit 0.

- [ ] **Step 4: Commit**

```bash
git add native/shim/bundle/maxine-manifest.psd1 scripts/assemble-maxine-stage.ps1 scripts/bundle-maxine.ps1
git commit -m "feat(#13): bundle NvOFFRUC + cudart64_110 into maxine runtime"
```

---

## Task 10: Docs + licensing

- [ ] **Step 1: Update `CLAUDE.md`** — add FRUC to the effects list, the `COS_FRUC_SDK_DIR` / `COS_FRUC_RUNTIME_DIR` env vars, and a one-line CO-VERSION note (FRUC = separate cudart64_110, no TRT, distinct name → safe). Keep it lean (doc-maintenance preference).

- [ ] **Step 2: Add the Optical Flow SDK to `THIRD-PARTY-NOTICES.md`** — FRUC ships under the NVIDIA Optical Flow SDK EULA; verify redistribution terms for `NvOFFRUC.dll` + `cudart64_110.dll` (this is a **human legal gate**, like the Maxine license — flag it, don't assume).

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md THIRD-PARTY-NOTICES.md
git commit -m "docs(#13): document FRUC effect + Optical Flow SDK notice"
```

---

## Self-Review

**Spec coverage** (vs the issue + findings doc):
- Co-version gate → already passed (prerequisite, not a task). ✓
- Runs on worker thread, after the effect chain → Task 5. ✓
- Holds frame N, emits N then N+0.5 when N+1 arrives → Task 5 (double-publish). ✓
- New `CosParams` enable flag + `CosCaps` availability gate → Tasks 4, 6, 7. ✓
- Independent toggle + doubled fps readout → Tasks 7, 8. ✓
- +1 frame latency, motion artifacts → Task 3 (latency gate) + Task 8 (artifact human gate). ✓
- New runtime bundled + co-versioned → Task 9. ✓
- Depends on Phase 1 sharpness plumbing → satisfied (SuperRes shipped; this plan mirrors it). ✓

**Open risks flagged in-plan (not placeholders — verification gates):**
- ARGB byte order (Task 3 human gate; swizzle fix path given).
- Interpolate latency vs 16 ms budget (Task 3 measures; feed-once optimisation path given).
- Pacing jitter (Task 8 human gate; FIFO upgrade path given in Task 5).
- Optical Flow SDK redistribution terms (Task 10 human legal gate).

**Type consistency:** `FrameInterpEnabled` (bool) / `FrameInterpAvailable` (bool) used consistently across Contracts, PInvoke, VM, Orchestrator, tests. C side: `frame_interp_enabled` / `frame_interp_available` / `fi_detail`. `Fruc::Interpolate(prev,cur,w,h,out)` signature consistent between Tasks 1, 2, 5.
