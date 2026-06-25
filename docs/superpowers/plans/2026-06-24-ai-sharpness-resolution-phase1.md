# AI Sharpness & Resolution (Phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Maxine VFX **Artifact Reduction** + **Super Resolution** effects and a **real fps counter** to the overlay pipeline, giving a sharper/higher-resolution image than the 1080p30 webcam delivers.

**Architecture:** Two new VFX effects run on the existing capture worker thread, mirroring `aigs.cpp` (green screen): Artifact Reduction first (cleans MJPG artifacts, no size change), Super Resolution last (upscales, larger output buffer). Both live in the same VFX 1.2.0.0 runtime already used for green screen — **no new co-version**. The C ABI grows three `CosParams` fields + two `CosCaps` gates; the managed MVVM chain gains toggles, capability gates, persistence, and live-param push, exactly like the green-screen props.

**Tech Stack:** C++ (Media Foundation + Maxine VFX, `COS_HAS_MAXINE`), C# .NET 8 (Core + WinUI 3 App), CommunityToolkit.Mvvm, xUnit.

**Spec:** `docs/superpowers/specs/2026-06-23-camera-on-screen-ai-sharpness-resolution-design.md`. Phase 2 (fps interpolation) = GitHub issue #13. Present-path minification fix is **deferred** (spec §11) — not in this plan.

## Global Constraints

- **Builds & tests must be pristine — 0 warnings** (CI enforces `/warnaserror` + `TreatWarningsAsErrors`).
- **Build the native shim BEFORE the App** (App copies `native/shim/x64/$(Configuration)/CameraOnScreen.Shim.dll`). Build the **SDK config last** before running (deploy-the-right-shim).
- **C ABI struct parity is byte-for-byte on x64.** `CosStatus`/`CosParams`/`CosCaps` in `shim.h` and their `[StructLayout(Sequential)]` mirrors in `PInvokeShim` must match. After ABI change verify exports: `dumpbin /exports` must still list exactly the **9** `cos_*` functions.
- **Maxine code is behind `COS_HAS_MAXINE`**; without the SDK the effect compiles as a passthrough stub (never ready). New VFX effects use the **same VFX runtime** as green screen — do NOT introduce a new TensorRT/CUDA runtime.
- **Effects run on the capture worker thread only.** The UI flips atomic enable flags; status crosses threads via atomics + **leaf locks** (never nested under `g_state.mtx` / `g_lifecycleMtx`).
- **Super Res output is capped at 2×** → max frame 3840×2160. The managed `_frameBuf` is pre-sized to that.
- Shim build (PowerShell): `& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64`
- App build: `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild`
- Core tests: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj` (single: append `--filter "FullyQualifiedName~Name"`).

---

## File Structure

**New files:**
- `native/shim/fps_counter.h` — header-only rolling FPS estimator (no Win32/Maxine deps; unit-testable).
- `native/shim/vfx_paths.h` / `vfx_paths.cpp` — shared VFX SDK path resolver + proxy pointer, reused by the two new effects (green-screen `aigs.cpp` keeps its own copy untouched).
- `native/shim/artifactreduction.h` / `artifactreduction.cpp` — `ArtifactReduction` effect (VFX `NVVFX_FX_ARTIFACT_REDUCTION`), in-place, no size change.
- `native/shim/superres.h` / `superres.cpp` — `SuperRes` effect (VFX `NVVFX_FX_SUPER_RES`), larger output buffer.
- `native/shim/smoke/fps_smoke.cpp` — asserts on the FPS estimator (no GPU).
- `native/shim/smoke/effects_smoke.cpp` — dev-box smoke: AR + SR `Start`+`ProcessFrame` on a synthetic frame (needs SDK+GPU).

**Modified files:**
- `native/shim/capture.h` / `capture.cpp` — fps counter, AR/SR enable atomics + Set methods + active/error state, worker-chain insertion.
- `native/shim/shim.h` — `CosParams` +3 fields, `CosCaps` +2 gates.
- `native/shim/shim.cpp` — real fps in `cos_get_status`; wire AR/SR in `cos_set_params` + `cos_query_capabilities`.
- `native/shim/shim.vcxproj` — compile the four new `.cpp` files.
- `src/CameraOnScreen.Core/Native/Contracts.cs` — `ShimParams` +3, `ShimCapabilities` +2.
- `src/CameraOnScreen.Core/Native/FakeShim.cs` — new capability gates.
- `src/CameraOnScreen.Core/Config/Models.cs` — `EffectSettings` +3.
- `src/CameraOnScreen.Core/Orchestration/Orchestrator.cs` — AR/SR availability + gating in `ApplyParams`.
- `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs` — props, `BuildParams`, `LoadFrom`, `ToAppConfig`, live-push partials.
- `src/CameraOnScreen.App/Native/PInvokeShim.cs` — struct mirrors + `SetParams`/`QueryCapabilities` mapping.
- `src/CameraOnScreen.App/MainWindow.xaml` — two toggles + scale combo.
- `src/CameraOnScreen.App/MainWindow.xaml.cs` — `_frameBuf` pre-size to 4K.
- `scripts/assemble-maxine-stage.ps1` + `native/shim/bundle/maxine-manifest.psd1` — bundle AR/SR feature DLLs + models.

---

## Task 1: Real fps counter (native)

Replaces the hardcoded `30.0` stub so the user can see effect cost. Fully isolated: no ABI change, no managed change (the managed side already plumbs `Fps`).

**Files:**
- Create: `native/shim/fps_counter.h`
- Create: `native/shim/smoke/fps_smoke.cpp`
- Modify: `native/shim/capture.h`, `native/shim/capture.cpp`, `native/shim/shim.cpp`

**Interfaces:**
- Produces: `class FpsCounter { double Sample(double nowSec, uint64_t totalFrames); void Reset(); }` and `double Capture::MeasuredFps() const;`

- [ ] **Step 1: Write the failing test** — `native/shim/smoke/fps_smoke.cpp`

```cpp
// Standalone smoke for FpsCounter. Build: cl /EHsc /I.. fps_smoke.cpp
#include <cassert>
#include <cmath>
#include <cstdio>
#include "../fps_counter.h"

static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

int main() {
    FpsCounter c;
    assert(near(c.Sample(0.0, 0), 0.0));        // first sample primes, no estimate yet
    assert(near(c.Sample(1.0, 30), 30.0));      // 30 frames in 1.0s -> 30 fps
    assert(near(c.Sample(1.2, 45), 30.0));      // 0.2s < min interval -> keep last estimate
    assert(near(c.Sample(2.0, 90), 56.25));     // 45 frames over 0.8s (since last accepted at t=1.0)
    c.Reset();
    assert(near(c.Sample(5.0, 999), 0.0));      // reset re-primes
    std::puts("fps_smoke OK");
    return 0;
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cl /EHsc /I native/shim native/shim/smoke/fps_smoke.cpp /Fe:build/fps_smoke.exe` (from a VS Developer prompt)
Expected: FAIL to compile — `fps_counter.h` does not exist.

- [ ] **Step 3: Implement `native/shim/fps_counter.h`**

```cpp
#pragma once
#include <cstdint>

// Rolling FPS estimate. SINGLE-CONSUMER (the status poll thread) — not internally
// synchronized. Feed a monotonically increasing cumulative frame count plus a
// monotonic clock reading in seconds. Recomputes over the elapsed window, but only
// refreshes once at least kMinInterval has passed so a fast poll can't divide by ~0.
class FpsCounter {
public:
    double Sample(double nowSec, uint64_t totalFrames) {
        if (!started_) { started_ = true; lastSec_ = nowSec; lastFrames_ = totalFrames; return fps_; }
        const double dt = nowSec - lastSec_;
        if (dt < kMinInterval) return fps_;     // too soon: keep the last estimate
        fps_ = static_cast<double>(totalFrames - lastFrames_) / dt;
        lastSec_ = nowSec;
        lastFrames_ = totalFrames;
        return fps_;
    }
    void Reset() { started_ = false; fps_ = 0.0; lastSec_ = 0.0; lastFrames_ = 0; }
private:
    static constexpr double kMinInterval = 0.5; // refresh at most ~2x/sec
    bool     started_   = false;
    double   lastSec_   = 0.0;
    double   fps_       = 0.0;
    uint64_t lastFrames_ = 0;
};
```

- [ ] **Step 4: Run the smoke to verify it passes**

Run: `cl /EHsc /I native/shim native/shim/smoke/fps_smoke.cpp /Fe:build/fps_smoke.exe && build\fps_smoke.exe`
Expected: prints `fps_smoke OK`, exit 0.

- [ ] **Step 5: Wire the counter into capture** — `native/shim/capture.h`

Add to the public section of `class Capture`:

```cpp
    // Measured frames-per-second over a rolling window (status polling). Thread-safe to
    // call from the single status-poll thread; 0 until the first interval elapses.
    double MeasuredFps() const;
```

- [ ] **Step 6: Implement counter state + increment** — `native/shim/capture.cpp`

At the top includes add:

```cpp
#include <chrono>
#include "fps_counter.h"
```

In `struct CaptureState` add:

```cpp
    std::atomic<uint64_t> framesProduced{0}; // bumped by worker after each published frame
    FpsCounter            fpsCounter;         // read only by MeasuredFps (status-poll thread)
```

In `WorkerLoop`, immediately after `g_state.hasNewFrame = true;` (the line at the end of the publish block, ~line 362) add:

```cpp
                g_state.framesProduced.fetch_add(1, std::memory_order_release);
```

In `Capture::Start`, inside the `g_state.mtx` block that resets frame state (next to `g_state.hasNewFrame = false;`) add:

```cpp
        g_state.framesProduced.store(0, std::memory_order_release);
        g_state.fpsCounter.Reset();
```

At the end of the file add the method:

```cpp
double Capture::MeasuredFps() const {
    using clock = std::chrono::steady_clock;
    const double nowSec =
        std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    const uint64_t frames = g_state.framesProduced.load(std::memory_order_acquire);
    return g_state.fpsCounter.Sample(nowSec, frames);
}
```

- [ ] **Step 7: Use it in `cos_get_status`** — `native/shim/shim.cpp`

Replace:

```cpp
    out->fps = g_running ? 30.0 : 0.0; // still a stub count (documented)
```

with:

```cpp
    out->fps = g_running ? g_capture.MeasuredFps() : 0.0;
```

- [ ] **Step 8: Build the shim and verify clean + exports unchanged**

Run (PowerShell):
```
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
dumpbin /exports native/shim/x64/Debug/CameraOnScreen.Shim.dll | Select-String cos_
```
Expected: 0 warnings; exactly 9 `cos_*` exports.

- [ ] **Step 9: Commit**

```bash
git add native/shim/fps_counter.h native/shim/smoke/fps_smoke.cpp native/shim/capture.h native/shim/capture.cpp native/shim/shim.cpp
git commit -m "feat(shim): real measured fps in cos_get_status (replaces 30.0 stub)"
```

---

## Task 2: Shared VFX path resolver (single source of truth)

Extract the VFX SDK path + proxy-pointer logic into one module that **green screen, Artifact Reduction, and Super Resolution all use**. `g_nvVFXSDKPath` ownership **moves into `vfx_paths.cpp`**; `aigs.cpp` drops its private copies and calls the shared helpers. This is the fully-DRY choice (user decision) — it edits the proven green-screen path, so it carries a **green-screen re-verify gate**.

**Files:**
- Create: `native/shim/vfx_paths.h`, `native/shim/vfx_paths.cpp`
- Modify: `native/shim/aigs.cpp` (use shared helpers; remove its private `ResolveSdkPaths`/`PointProxiesAt` + its `g_nvVFXSDKPath` definition), `native/shim/shim.vcxproj`
- Verify (human gate): green screen still loads + mattes after the refactor.

**Interfaces:**
- Produces: `namespace vfx { bool ResolveSdkPaths(std::string& binDir, std::string& modelDir, std::string& err); void PointProxiesAt(const std::string& binDir); }`. `g_nvVFXSDKPath` is now **defined** in `vfx_paths.cpp` (was `aigs.cpp`).

- [ ] **Step 1: Create `native/shim/vfx_paths.h`**

```cpp
#pragma once
#include <string>

// Shared VFX-SDK runtime discovery for ALL VFX effects (Green Screen, Artifact
// Reduction, Super Resolution). Single source of truth — aigs.cpp uses these too.
// Model subdir is models\vfx in the bundled layout.
namespace vfx {
// Fills binDir (holds NVVideoEffects.dll) and modelDir. Order: COS_VFX_RUNTIME_DIR
// (flat) -> COS_VFX_SDK_DIR (legacy \bin) -> <shimDir>\maxine (bundled). false if none.
bool ResolveSdkPaths(std::string& binDir, std::string& modelDir, std::string& err);
// Points the VFX proxy global (g_nvVFXSDKPath, defined in vfx_paths.cpp) at binDir.
void PointProxiesAt(const std::string& binDir);
}
```

- [ ] **Step 2: Create `native/shim/vfx_paths.cpp`** (owns `g_nvVFXSDKPath`; logic moved from `aigs.cpp`)

```cpp
#include "vfx_paths.h"
#ifdef COS_HAS_MAXINE
#include <windows.h>
#include "paths.h"

// The proxy stub (nvVideoEffectsProxy.cpp) declares this extern; we own the single
// definition here (moved out of aigs.cpp). Non-null => dir holding NVVideoEffects.dll,
// which the proxy passes to SetDllDirectory before LoadLibrary.
char* g_nvVFXSDKPath = nullptr;

namespace vfx {
bool ResolveSdkPaths(std::string& binDir, std::string& modelDir, std::string& err) {
    char buf[1024] = {0};
    DWORD n = GetEnvironmentVariableA("COS_VFX_RUNTIME_DIR", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        std::string root(buf, n);
        if (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
        binDir = root; modelDir = root + "\\models"; return true;
    }
    n = GetEnvironmentVariableA("COS_VFX_SDK_DIR", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        std::string root(buf, n);
        if (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
        binDir = root + "\\bin"; modelDir = root + "\\bin\\models"; return true;
    }
    std::string appDir = ShimModuleDir();
    if (!appDir.empty()) {
        std::string maxine = appDir + "\\maxine";
        if (DirExists(maxine)) { binDir = maxine; modelDir = maxine + "\\models\\vfx"; return true; }
    }
    err = "VFX runtime not found: set COS_VFX_RUNTIME_DIR or bundle maxine\\ beside the app";
    return false;
}
void PointProxiesAt(const std::string& binDir) {
    static std::string s_bin;
    s_bin = binDir;
    g_nvVFXSDKPath = const_cast<char*>(s_bin.c_str());
}
}
#else
namespace vfx {
bool ResolveSdkPaths(std::string&, std::string&, std::string& err) { err = "Maxine SDK not built in"; return false; }
void PointProxiesAt(const std::string&) {}
}
#endif
```

- [ ] **Step 3: Refactor `native/shim/aigs.cpp` to use the shared module**

In the `COS_HAS_MAXINE` block:
1. Add `#include "vfx_paths.h"` next to the other includes.
2. **Delete** the `g_nvVFXSDKPath` definition (the `char* g_nvVFXSDKPath = nullptr;` line and its comment) — it now lives in `vfx_paths.cpp`.
3. **Delete** the anonymous-namespace `ResolveSdkPaths(...)` and `PointProxiesAt(...)` functions (the whole `namespace { ... }` block that defines them, lines ~26–76).
4. In `Aigs::Probe`, replace the resolver call:

```cpp
    std::string binDir, modelDir, err;
    if (!vfx::ResolveSdkPaths(binDir, modelDir, err)) { detail = err; return false; }
    vfx::PointProxiesAt(binDir);
```

5. In `Aigs::Start`, replace likewise:

```cpp
    std::string binDir, err;
    if (!vfx::ResolveSdkPaths(binDir, impl->modelDir, err)) {
        lastError_ = err; delete impl; return false;
    }
    vfx::PointProxiesAt(binDir);
```

(Both call sites already use these exact local variable names; only the function namespace changes from the file-local helpers to `vfx::`.)

- [ ] **Step 4: Add to `native/shim/shim.vcxproj`**

Inside the `<ItemGroup>` that holds the other `<ClCompile>` entries (e.g. `aigs.cpp`), add:

```xml
    <ClCompile Include="vfx_paths.cpp" />
```

- [ ] **Step 5: Build the shim (SDK config) — clean, single `g_nvVFXSDKPath` definition**

Run the shim build with `COS_VFX_SDK_DIR` set. Expected: 0 warnings, links cleanly (exactly one definition of `g_nvVFXSDKPath` — a duplicate-symbol linker error here means the old definition was not removed from `aigs.cpp`).

- [ ] **Step 6: Commit**

```bash
git add native/shim/vfx_paths.h native/shim/vfx_paths.cpp native/shim/aigs.cpp native/shim/shim.vcxproj
git commit -m "refactor(shim): shared VFX SDK path resolver (green screen + new effects)"
```

> **Human gate (after build):** run the app with green screen ON and confirm the matte still works — this task edited the proven green-screen load path. Tasks 3–4 reuse `vfx::` so a green-screen regression here would surface everywhere.

---

## Task 3: Artifact Reduction effect

VFX effect that removes compression artifacts in place at source resolution. Mirrors `aigs.cpp` but the output is a full BGR image, not a matte. No dimension change.

**Files:**
- Create: `native/shim/artifactreduction.h`, `native/shim/artifactreduction.cpp`
- Create/extend: `native/shim/smoke/effects_smoke.cpp`
- Modify: `native/shim/shim.vcxproj`

**Interfaces:**
- Consumes: `vfx::ResolveSdkPaths`, `vfx::PointProxiesAt` (Task 2).
- Produces: `class ArtifactReduction { static bool Probe(std::string&); bool Start(); void Stop(); bool ProcessFrame(uint8_t* bgra, int w, int h); bool IsReady() const; const std::string& LastError() const; }`

> **Verify at impl:** confirm the effect selector macro `NVVFX_FX_ARTIFACT_REDUCTION` and that AR input/output images are `NVCV_BGR`/`NVCV_U8`/`NVCV_CHUNKY` against the VFX 1.2.0.0 headers (`nvVideoEffects.h`). Mode key is `NVVFX_MODE` (0 or 1). These match the green-screen pattern; only the selector, the output format (full BGR, not `NVCV_A`), and the write-back differ.

- [ ] **Step 1: Create `native/shim/artifactreduction.h`**

```cpp
#pragma once
#include <cstdint>
#include <string>

// Wraps the Maxine VFX Artifact Reduction effect. CPU-copy: a BGRA frame is uploaded
// to the GPU as BGR, cleaned, downloaded, and written back over the same BGRA buffer
// (RGB replaced, alpha left untouched). No size change. All methods no-throw; failure
// via IsReady()/LastError(). Without COS_HAS_MAXINE this is a never-ready stub.
class ArtifactReduction {
public:
    ArtifactReduction();
    ~ArtifactReduction();
    static bool Probe(std::string& detail);
    bool Start();
    void Stop();
    bool ProcessFrame(uint8_t* bgra, int width, int height);
    bool IsReady() const { return ready_; }
    const std::string& LastError() const { return lastError_; }
private:
    bool ready_ = false;
    std::string lastError_;
    void* impl_ = nullptr;
};
```

- [ ] **Step 2: Create `native/shim/artifactreduction.cpp`**

```cpp
#include "artifactreduction.h"

#ifdef COS_HAS_MAXINE
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <new>
#include <string>
#include "nvCVStatus.h"
#include "nvCVImage.h"
#include "nvVideoEffects.h"  // NVVFX_FX_ARTIFACT_REDUCTION (verify symbol)
#include "vfx_paths.h"

struct ArImpl {
    NvVFX_Handle effect = nullptr;
    CUstream     stream = nullptr;
    std::string  modelDir;
    NvCVImage srcGpu{};  // BGR u8 GPU (input)
    NvCVImage dstGpu{};  // BGR u8 GPU (output)
    NvCVImage dstCpu{};  // BGR u8 CPU (downloaded)
    NvCVImage stage{};   // BGRA u8 GPU (transfer staging)
    int w = 0, h = 0;
    bool loaded = false;
};

ArtifactReduction::ArtifactReduction() = default;
ArtifactReduction::~ArtifactReduction() { Stop(); }

bool ArtifactReduction::Probe(std::string& detail) {
    std::string binDir, modelDir, err;
    if (!vfx::ResolveSdkPaths(binDir, modelDir, err)) { detail = err; return false; }
    vfx::PointProxiesAt(binDir);
    NvVFX_Handle eff = nullptr;
    if (NvVFX_CreateEffect(NVVFX_FX_ARTIFACT_REDUCTION, &eff) != NVCV_SUCCESS || !eff) {
        detail = "NvVFX_CreateEffect(ArtifactReduction) failed (DLL/SDK load?)"; return false;
    }
    NvVFX_SetString(eff, NVVFX_MODEL_DIRECTORY, modelDir.c_str());
    NvVFX_SetU32(eff, NVVFX_MODE, 0u);
    NvVFX_SetU32(eff, NVVFX_MAX_INPUT_WIDTH,  1920u);
    NvVFX_SetU32(eff, NVVFX_MAX_INPUT_HEIGHT, 1080u);
    NvCV_Status load = NvVFX_Load(eff);
    NvVFX_DestroyEffect(eff);
    if (load != NVCV_SUCCESS) { detail = "NvVFX_Load(ArtifactReduction) failed (models missing?)"; return false; }
    detail = "ArtifactReduction available";
    return true;
}

bool ArtifactReduction::Start() {
    Stop();
    ArImpl* impl = new (std::nothrow) ArImpl();
    if (!impl) { lastError_ = "out of memory"; return false; }
    std::string binDir, err;
    if (!vfx::ResolveSdkPaths(binDir, impl->modelDir, err)) { lastError_ = err; delete impl; return false; }
    vfx::PointProxiesAt(binDir);
    if (NvVFX_CudaStreamCreate(&impl->stream) != NVCV_SUCCESS) { lastError_ = "CudaStreamCreate failed"; delete impl; return false; }
    if (NvVFX_CreateEffect(NVVFX_FX_ARTIFACT_REDUCTION, &impl->effect) != NVCV_SUCCESS || !impl->effect) {
        lastError_ = "CreateEffect failed"; delete impl; return false;
    }
    NvVFX_SetString(impl->effect, NVVFX_MODEL_DIRECTORY, impl->modelDir.c_str());
    NvVFX_SetCudaStream(impl->effect, NVVFX_CUDA_STREAM, impl->stream);
    NvVFX_SetU32(impl->effect, NVVFX_MODE, 1u); // mode 1 = stronger / for compressed input
    impl_ = impl; ready_ = true; lastError_.clear();
    return true;
}

void ArtifactReduction::Stop() {
    auto* impl = static_cast<ArImpl*>(impl_);
    if (!impl) { ready_ = false; return; }
    NvCVImage_Dealloc(&impl->srcGpu);
    NvCVImage_Dealloc(&impl->dstGpu);
    NvCVImage_Dealloc(&impl->dstCpu);
    NvCVImage_Dealloc(&impl->stage);
    if (impl->effect) NvVFX_DestroyEffect(impl->effect);
    if (impl->stream) NvVFX_CudaStreamDestroy(impl->stream);
    delete impl; impl_ = nullptr; ready_ = false;
}

namespace {
NvCV_Status EnsureImages(ArImpl* impl, int w, int h) {
    if (impl->loaded && impl->w == w && impl->h == h) return NVCV_SUCCESS;
    NvCVImage_Dealloc(&impl->srcGpu);
    NvCVImage_Dealloc(&impl->dstGpu);
    NvCVImage_Dealloc(&impl->dstCpu);
    NvCVImage_Dealloc(&impl->stage);
    NvCV_Status s;
    s = NvCVImage_Alloc(&impl->srcGpu, w, h, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->dstGpu, w, h, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->dstCpu, w, h, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_CPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->stage,  w, h, NVCV_BGRA,NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetImage(impl->effect, NVVFX_INPUT_IMAGE,  &impl->srcGpu); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetImage(impl->effect, NVVFX_OUTPUT_IMAGE, &impl->dstGpu); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetU32(impl->effect, NVVFX_MAX_INPUT_WIDTH,  static_cast<unsigned>(w)); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetU32(impl->effect, NVVFX_MAX_INPUT_HEIGHT, static_cast<unsigned>(h)); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_Load(impl->effect); if (s != NVCV_SUCCESS) return s;
    impl->w = w; impl->h = h; impl->loaded = true;
    return NVCV_SUCCESS;
}
} // namespace

bool ArtifactReduction::ProcessFrame(uint8_t* bgra, int w, int h) {
    auto* impl = static_cast<ArImpl*>(impl_);
    if (!impl || !impl->effect || !bgra || w <= 0 || h <= 0) return false;
    if (NvCV_Status es = EnsureImages(impl, w, h); es != NVCV_SUCCESS) {
        lastError_ = std::string("EnsureImages/Load failed: ") + NvCV_GetErrorStringFromCode(es); ready_ = false; return false;
    }
    NvCVImage src{};
    NvCVImage_Init(&src, w, h, w * 4, bgra, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
    if (NvCVImage_Transfer(&src, &impl->srcGpu, 1.0f, impl->stream, &impl->stage) != NVCV_SUCCESS) { lastError_ = "upload failed"; return false; }
    if (NvVFX_Run(impl->effect, 0) != NVCV_SUCCESS) { lastError_ = "NvVFX_Run failed"; return false; }
    if (NvCVImage_Transfer(&impl->dstGpu, &impl->dstCpu, 1.0f, impl->stream, nullptr) != NVCV_SUCCESS) { lastError_ = "download failed"; return false; }
    if (NvVFX_CudaStreamSynchronize(impl->stream) != NVCV_SUCCESS) { lastError_ = "stream sync failed"; return false; }
    // Write cleaned BGR back over the BGRA buffer; leave alpha untouched (forced 0xFF upstream).
    const uint8_t* d = static_cast<const uint8_t*>(impl->dstCpu.pixels);
    const int dpitch = impl->dstCpu.pitch;
    for (int y = 0; y < h; ++y) {
        const uint8_t* drow = d + static_cast<size_t>(dpitch) * y;
        uint8_t* prow = bgra + static_cast<size_t>(w) * 4 * y;
        for (int x = 0; x < w; ++x) {
            prow[x * 4 + 0] = drow[x * 3 + 0];
            prow[x * 4 + 1] = drow[x * 3 + 1];
            prow[x * 4 + 2] = drow[x * 3 + 2];
        }
    }
    return true;
}

#else
ArtifactReduction::ArtifactReduction() = default;
ArtifactReduction::~ArtifactReduction() = default;
bool ArtifactReduction::Probe(std::string& detail) { detail = "Maxine SDK not built in"; return false; }
bool ArtifactReduction::Start() { lastError_ = "Maxine SDK not built in"; ready_ = false; return false; }
void ArtifactReduction::Stop() { ready_ = false; }
bool ArtifactReduction::ProcessFrame(uint8_t*, int, int) { return false; }
#endif
```

- [ ] **Step 3: Add `artifactreduction.cpp` to `native/shim/shim.vcxproj`**

```xml
    <ClCompile Include="artifactreduction.cpp" />
```

- [ ] **Step 4: Add an AR smoke** — append to `native/shim/smoke/effects_smoke.cpp` (create it)

```cpp
// Dev-box smoke (needs VFX SDK + RTX GPU). Build with the shim's include/lib setup.
// Verifies AR can start and process a synthetic 1280x720 frame.
#include <cassert>
#include <cstdio>
#include <vector>
#include "../artifactreduction.h"

int main() {
    std::string detail;
    if (!ArtifactReduction::Probe(detail)) { std::printf("AR unavailable: %s\n", detail.c_str()); return 0; }
    ArtifactReduction ar;
    assert(ar.Start());
    std::vector<uint8_t> frame(1280 * 720 * 4, 128);
    bool ok = ar.ProcessFrame(frame.data(), 1280, 720);
    std::printf("AR ProcessFrame: %s (%s)\n", ok ? "ok" : "fail", ar.LastError().c_str());
    assert(ok);
    ar.Stop();
    std::puts("effects_smoke AR OK");
    return 0;
}
```

- [ ] **Step 5: Build the shim (clean) and run the AR smoke on the dev box (SDK config)**

Run the shim build with `COS_VFX_SDK_DIR` set (SDK config). Expected: 0 warnings. Then build+run `effects_smoke.cpp` (dev box): prints `effects_smoke AR OK`. (If no SDK/GPU, `Probe` returns the stub message and the smoke exits 0 — acceptable on CI-style hosts.)

- [ ] **Step 6: Commit**

```bash
git add native/shim/artifactreduction.h native/shim/artifactreduction.cpp native/shim/smoke/effects_smoke.cpp native/shim/shim.vcxproj
git commit -m "feat(shim): Maxine VFX Artifact Reduction effect"
```

---

## Task 4: Super Resolution effect

VFX effect that upscales the frame (1.5× or 2×) into a **larger output buffer**. Runs last in the chain.

**Files:**
- Create: `native/shim/superres.h`, `native/shim/superres.cpp`
- Modify: `native/shim/smoke/effects_smoke.cpp`, `native/shim/shim.vcxproj`

**Interfaces:**
- Consumes: `vfx::ResolveSdkPaths`, `vfx::PointProxiesAt`.
- Produces: `class SuperRes { static bool Probe(std::string&); bool Start(int scaleX10); void Stop(); bool ProcessFrame(const uint8_t* bgra, int w, int h, std::vector<uint8_t>& out, int& outW, int& outH); bool IsReady() const; const std::string& LastError() const; }` where `scaleX10` ∈ {15, 20}.

> **Verify at impl:** confirm `NVVFX_FX_SUPER_RES`, and how the upscale factor is selected — the VFX SuperRes effect infers scale from the output image size relative to the input (set both images), plus `NVVFX_MODE` (0/1). If a dedicated scale property (`NVVFX_SCALE`) is required by 1.2.0.0, set it. Output dims must be one of the SDK-supported factors of the input.

- [ ] **Step 1: Create `native/shim/superres.h`**

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Wraps the Maxine VFX Super Resolution effect. Upscales a BGRA frame by 1.5x or 2x
// into a freshly sized BGRA output buffer (alpha = 0xFF). Without COS_HAS_MAXINE this
// is a never-ready stub. scaleX10: 15 => 1.5x, 20 => 2x.
class SuperRes {
public:
    SuperRes();
    ~SuperRes();
    static bool Probe(std::string& detail);
    bool Start(int scaleX10);
    void Stop();
    // Reads w*h BGRA from 'bgra', writes outW*outH BGRA into 'out'. Returns false on failure
    // (out untouched). outW/outH are w*scale, h*scale.
    bool ProcessFrame(const uint8_t* bgra, int w, int h, std::vector<uint8_t>& out, int& outW, int& outH);
    bool IsReady() const { return ready_; }
    const std::string& LastError() const { return lastError_; }
private:
    bool ready_ = false;
    int  scaleX10_ = 20;
    std::string lastError_;
    void* impl_ = nullptr;
};
```

- [ ] **Step 2: Create `native/shim/superres.cpp`**

```cpp
#include "superres.h"

#ifdef COS_HAS_MAXINE
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <new>
#include <string>
#include "nvCVStatus.h"
#include "nvCVImage.h"
#include "nvVideoEffects.h"  // NVVFX_FX_SUPER_RES (verify symbol)
#include "vfx_paths.h"

struct SrImpl {
    NvVFX_Handle effect = nullptr;
    CUstream     stream = nullptr;
    std::string  modelDir;
    NvCVImage srcGpu{};  // BGR u8 GPU (input, w x h)
    NvCVImage dstGpu{};  // BGR u8 GPU (output, outW x outH)
    NvCVImage dstCpu{};  // BGR u8 CPU (downloaded)
    NvCVImage stage{};   // BGRA u8 GPU (upload staging, w x h)
    int w = 0, h = 0, ow = 0, oh = 0;
    bool loaded = false;
};

static void ScaledDims(int w, int h, int scaleX10, int& ow, int& oh) {
    ow = (w * scaleX10) / 10;
    oh = (h * scaleX10) / 10;
}

SuperRes::SuperRes() = default;
SuperRes::~SuperRes() { Stop(); }

bool SuperRes::Probe(std::string& detail) {
    std::string binDir, modelDir, err;
    if (!vfx::ResolveSdkPaths(binDir, modelDir, err)) { detail = err; return false; }
    vfx::PointProxiesAt(binDir);
    NvVFX_Handle eff = nullptr;
    if (NvVFX_CreateEffect(NVVFX_FX_SUPER_RES, &eff) != NVCV_SUCCESS || !eff) {
        detail = "NvVFX_CreateEffect(SuperRes) failed (DLL/SDK load?)"; return false;
    }
    NvVFX_SetString(eff, NVVFX_MODEL_DIRECTORY, modelDir.c_str());
    NvVFX_SetU32(eff, NVVFX_MODE, 1u);
    NvVFX_SetU32(eff, NVVFX_MAX_INPUT_WIDTH,  1920u);
    NvVFX_SetU32(eff, NVVFX_MAX_INPUT_HEIGHT, 1080u);
    NvCV_Status load = NvVFX_Load(eff);
    NvVFX_DestroyEffect(eff);
    if (load != NVCV_SUCCESS) { detail = "NvVFX_Load(SuperRes) failed (models missing?)"; return false; }
    detail = "SuperRes available";
    return true;
}

bool SuperRes::Start(int scaleX10) {
    Stop();
    scaleX10_ = (scaleX10 == 15) ? 15 : 20; // only 1.5x / 2x supported here
    SrImpl* impl = new (std::nothrow) SrImpl();
    if (!impl) { lastError_ = "out of memory"; return false; }
    std::string binDir, err;
    if (!vfx::ResolveSdkPaths(binDir, impl->modelDir, err)) { lastError_ = err; delete impl; return false; }
    vfx::PointProxiesAt(binDir);
    if (NvVFX_CudaStreamCreate(&impl->stream) != NVCV_SUCCESS) { lastError_ = "CudaStreamCreate failed"; delete impl; return false; }
    if (NvVFX_CreateEffect(NVVFX_FX_SUPER_RES, &impl->effect) != NVCV_SUCCESS || !impl->effect) {
        lastError_ = "CreateEffect failed"; delete impl; return false;
    }
    NvVFX_SetString(impl->effect, NVVFX_MODEL_DIRECTORY, impl->modelDir.c_str());
    NvVFX_SetCudaStream(impl->effect, NVVFX_CUDA_STREAM, impl->stream);
    NvVFX_SetU32(impl->effect, NVVFX_MODE, 1u);
    impl_ = impl; ready_ = true; lastError_.clear();
    return true;
}

void SuperRes::Stop() {
    auto* impl = static_cast<SrImpl*>(impl_);
    if (!impl) { ready_ = false; return; }
    NvCVImage_Dealloc(&impl->srcGpu);
    NvCVImage_Dealloc(&impl->dstGpu);
    NvCVImage_Dealloc(&impl->dstCpu);
    NvCVImage_Dealloc(&impl->stage);
    if (impl->effect) NvVFX_DestroyEffect(impl->effect);
    if (impl->stream) NvVFX_CudaStreamDestroy(impl->stream);
    delete impl; impl_ = nullptr; ready_ = false;
}

namespace {
NvCV_Status EnsureImages(SrImpl* impl, int w, int h, int ow, int oh) {
    if (impl->loaded && impl->w == w && impl->h == h && impl->ow == ow && impl->oh == oh) return NVCV_SUCCESS;
    NvCVImage_Dealloc(&impl->srcGpu);
    NvCVImage_Dealloc(&impl->dstGpu);
    NvCVImage_Dealloc(&impl->dstCpu);
    NvCVImage_Dealloc(&impl->stage);
    NvCV_Status s;
    s = NvCVImage_Alloc(&impl->srcGpu, w,  h,  NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->dstGpu, ow, oh, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->dstCpu, ow, oh, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_CPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->stage,  w,  h,  NVCV_BGRA,NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetImage(impl->effect, NVVFX_INPUT_IMAGE,  &impl->srcGpu); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_SetImage(impl->effect, NVVFX_OUTPUT_IMAGE, &impl->dstGpu); if (s != NVCV_SUCCESS) return s;
    s = NvVFX_Load(impl->effect); if (s != NVCV_SUCCESS) return s;
    impl->w = w; impl->h = h; impl->ow = ow; impl->oh = oh; impl->loaded = true;
    return NVCV_SUCCESS;
}
} // namespace

bool SuperRes::ProcessFrame(const uint8_t* bgra, int w, int h, std::vector<uint8_t>& out, int& outW, int& outH) {
    auto* impl = static_cast<SrImpl*>(impl_);
    if (!impl || !impl->effect || !bgra || w <= 0 || h <= 0) return false;
    int ow, oh; ScaledDims(w, h, scaleX10_, ow, oh);
    if (NvCV_Status es = EnsureImages(impl, w, h, ow, oh); es != NVCV_SUCCESS) {
        lastError_ = std::string("EnsureImages/Load failed: ") + NvCV_GetErrorStringFromCode(es); ready_ = false; return false;
    }
    NvCVImage src{};
    NvCVImage_Init(&src, w, h, w * 4, const_cast<uint8_t*>(bgra), NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
    if (NvCVImage_Transfer(&src, &impl->srcGpu, 1.0f, impl->stream, &impl->stage) != NVCV_SUCCESS) { lastError_ = "upload failed"; return false; }
    if (NvVFX_Run(impl->effect, 0) != NVCV_SUCCESS) { lastError_ = "NvVFX_Run failed"; return false; }
    if (NvCVImage_Transfer(&impl->dstGpu, &impl->dstCpu, 1.0f, impl->stream, nullptr) != NVCV_SUCCESS) { lastError_ = "download failed"; return false; }
    if (NvVFX_CudaStreamSynchronize(impl->stream) != NVCV_SUCCESS) { lastError_ = "stream sync failed"; return false; }
    out.assign(static_cast<size_t>(ow) * oh * 4, 0xFF);
    const uint8_t* d = static_cast<const uint8_t*>(impl->dstCpu.pixels);
    const int dpitch = impl->dstCpu.pitch;
    for (int y = 0; y < oh; ++y) {
        const uint8_t* drow = d + static_cast<size_t>(dpitch) * y;
        uint8_t* prow = out.data() + static_cast<size_t>(ow) * 4 * y;
        for (int x = 0; x < ow; ++x) {
            prow[x * 4 + 0] = drow[x * 3 + 0];
            prow[x * 4 + 1] = drow[x * 3 + 1];
            prow[x * 4 + 2] = drow[x * 3 + 2];
            prow[x * 4 + 3] = 0xFF;
        }
    }
    outW = ow; outH = oh;
    return true;
}

#else
SuperRes::SuperRes() = default;
SuperRes::~SuperRes() = default;
bool SuperRes::Probe(std::string& detail) { detail = "Maxine SDK not built in"; return false; }
bool SuperRes::Start(int) { lastError_ = "Maxine SDK not built in"; ready_ = false; return false; }
void SuperRes::Stop() { ready_ = false; }
bool SuperRes::ProcessFrame(const uint8_t*, int, int, std::vector<uint8_t>&, int&, int&) { return false; }
#endif
```

- [ ] **Step 3: Add `superres.cpp` to `native/shim/shim.vcxproj`**

```xml
    <ClCompile Include="superres.cpp" />
```

- [ ] **Step 4: Extend the smoke** — append to `native/shim/smoke/effects_smoke.cpp` a SuperRes check (inside `main`, before the final puts):

```cpp
    {
        std::string d;
        if (SuperRes::Probe(d)) {
            SuperRes sr; assert(sr.Start(20));
            std::vector<uint8_t> in(640 * 480 * 4, 64), out; int ow = 0, oh = 0;
            bool ok = sr.ProcessFrame(in.data(), 640, 480, out, ow, oh);
            std::printf("SR ProcessFrame: %s -> %dx%d\n", ok ? "ok" : "fail", ow, oh);
            assert(ok && ow == 1280 && oh == 960);
            sr.Stop();
        } else { std::printf("SR unavailable: %s\n", d.c_str()); }
    }
```
Add `#include "../superres.h"` at the top.

- [ ] **Step 5: Build the shim (clean) + run the smoke (dev box)**

Expected: 0 warnings; smoke prints `SR ProcessFrame: ok -> 1280x960`.

- [ ] **Step 6: Commit**

```bash
git add native/shim/superres.h native/shim/superres.cpp native/shim/smoke/effects_smoke.cpp native/shim/shim.vcxproj
git commit -m "feat(shim): Maxine VFX Super Resolution effect (1.5x/2x)"
```

---

## Task 5: C ABI — params + capability gates

Grow the contract once for both effects. After this task the shim accepts the new params and reports the new gates, but the worker does not yet run the effects (Task 6).

**Files:**
- Modify: `native/shim/shim.h`, `native/shim/shim.cpp`

**Interfaces:**
- Produces (C): `CosParams` gains `int artifact_reduction_enabled; int super_res_enabled; int super_res_scale;`. `CosCaps` gains `int artifact_reduction_available; int super_res_available;`.

- [ ] **Step 1: Update `CosParams` + `CosCaps` in `native/shim/shim.h`**

Replace the `CosParams` struct with:

```cpp
typedef struct {
    const char* camera_id;   // UTF-8, may be null
    int    green_screen_enabled;
    double green_screen_expand;
    double green_screen_feather;
    int    eye_contact_enabled;
    double eye_contact_sensitivity;
    double eye_contact_look_away_range;
    int    artifact_reduction_enabled;
    int    super_res_enabled;
    int    super_res_scale;           // 0=off, 15=1.5x, 20=2x
} CosParams;
```

Replace the `CosCaps` struct with (total 528 bytes on x64):

```cpp
typedef struct {
    int  green_screen_available;
    char detail[256];
    int  eye_contact_available;
    char ec_detail[256];
    int  artifact_reduction_available; // 1 if Maxine ArtifactReduction can run
    int  super_res_available;          // 1 if Maxine SuperRes can run
} CosCaps;
```

- [ ] **Step 2: Wire `cos_set_params`** — `native/shim/shim.cpp`

Add the includes near the top:

```cpp
#include "artifactreduction.h"
#include "superres.h"
```

In `cos_set_params`, after the existing `g_capture.SetEyeContact(...)` line add:

```cpp
    g_capture.SetArtifactReduction(p->artifact_reduction_enabled != 0);
    g_capture.SetSuperRes(p->super_res_enabled != 0, p->super_res_scale);
```

- [ ] **Step 3: Wire `cos_query_capabilities`** — `native/shim/shim.cpp`

Before the final `return` of `cos_query_capabilities`, add:

```cpp
    std::string arDetail, srDetail;
    out->artifact_reduction_available = ArtifactReduction::Probe(arDetail) ? 1 : 0;
    out->super_res_available          = SuperRes::Probe(srDetail) ? 1 : 0;
```

And change the final return to also surface the new gates:

```cpp
    return (gsOk || ecOk || out->artifact_reduction_available || out->super_res_available) ? 1 : 0;
```

- [ ] **Step 4: Build the shim and verify exports + struct sizes**

Run the shim build. Expected: 0 warnings; `dumpbin /exports` still shows exactly 9 `cos_*`. (Capture `Set*` methods are added in Task 6 — if the build fails on missing `SetArtifactReduction`/`SetSuperRes`, do Task 6 Step 1–3 before re-building; the two tasks share the ABI boundary. Recommended: implement Task 6 immediately after this step, then build once.)

- [ ] **Step 5: Commit** (after Task 6 builds clean, or together)

```bash
git add native/shim/shim.h native/shim/shim.cpp
git commit -m "feat(shim): ABI — AR/SR params + capability gates (CosParams/CosCaps 528B)"
```

---

## Task 6: Capture worker — run AR first, SR last

**Files:**
- Modify: `native/shim/capture.h`, `native/shim/capture.cpp`

**Interfaces:**
- Consumes: `ArtifactReduction`, `SuperRes` (Tasks 3–4); `cos_set_params` (Task 5).
- Produces: `Capture::SetArtifactReduction(bool)`, `Capture::SetSuperRes(bool, int scaleX10)`, `Capture::ArtifactReductionActive()/SuperResActive()` + error getters.

- [ ] **Step 1: Declare the new methods in `native/shim/capture.h`**

After the Eye Contact block add:

```cpp
    // Toggles Artifact Reduction for subsequent frames. Thread-safe; worker owns the object.
    void SetArtifactReduction(bool enabled);
    bool ArtifactReductionActive() const;
    std::string ArtifactReductionError() const;

    // Toggles Super Resolution + scale (15=1.5x, 20=2x). Thread-safe; worker owns the object.
    void SetSuperRes(bool enabled, int scaleX10);
    bool SuperResActive() const;
    std::string SuperResError() const;
```

- [ ] **Step 2: Add shared state** — `native/shim/capture.cpp`, in `struct CaptureState`

```cpp
    std::atomic<bool>     artifactReductionEnabled{false};
    std::atomic<bool>     artifactReductionActive{false};
    std::mutex            arErrMtx;            // leaf lock
    std::string           arError;

    std::atomic<bool>     superResEnabled{false};
    std::atomic<int>      superResScale{20};   // 15 or 20
    std::atomic<bool>     superResActive{false};
    std::mutex            srErrMtx;            // leaf lock
    std::string           srError;
```

Add the include at the top: `#include "superres.h"` (alongside the existing `#include "aigs.h"` / `#include "eyecontact.h"`; `artifactreduction.h` is needed too):

```cpp
#include "artifactreduction.h"
#include "superres.h"
```

- [ ] **Step 3: Implement the setters/getters** — `native/shim/capture.cpp` (near the existing `SetEyeContact` etc.)

```cpp
void Capture::SetArtifactReduction(bool enabled) {
    g_state.artifactReductionEnabled.store(enabled, std::memory_order_release);
}
bool Capture::ArtifactReductionActive() const {
    return g_state.artifactReductionActive.load(std::memory_order_acquire);
}
std::string Capture::ArtifactReductionError() const {
    std::lock_guard<std::mutex> e(g_state.arErrMtx); return g_state.arError;
}
void Capture::SetSuperRes(bool enabled, int scaleX10) {
    g_state.superResScale.store(scaleX10 == 15 ? 15 : 20, std::memory_order_release);
    g_state.superResEnabled.store(enabled, std::memory_order_release);
}
bool Capture::SuperResActive() const {
    return g_state.superResActive.load(std::memory_order_acquire);
}
std::string Capture::SuperResError() const {
    std::lock_guard<std::mutex> e(g_state.srErrMtx); return g_state.srError;
}
```

- [ ] **Step 4: Insert AR + SR into `WorkerLoop`**

Declare the effect objects next to the existing `Aigs aigs;` / `EyeContact eyeContact;` (worker-thread-local):

```cpp
    ArtifactReduction artifactReduction;
    SuperRes superRes;
```

**Artifact Reduction — before Eye Contact.** Inside `if (CopyFrame(...))`, immediately BEFORE the existing `const bool ecWant = ...` line, add:

```cpp
                // Artifact Reduction runs FIRST so the AI effects downstream get a clean frame.
                const bool arWant = g_state.artifactReductionEnabled.load(std::memory_order_acquire);
                if (arWant && !artifactReduction.IsReady()) {
                    if (!artifactReduction.Start()) {
                        std::lock_guard<std::mutex> e(g_state.arErrMtx);
                        const std::string& ne = artifactReduction.LastError();
                        if (g_state.arError != ne) g_state.arError = ne;
                    }
                } else if (!arWant && artifactReduction.IsReady()) {
                    artifactReduction.Stop();
                    std::lock_guard<std::mutex> e(g_state.arErrMtx);
                    if (!g_state.arError.empty()) g_state.arError.clear();
                }
                bool arApplied = false;
                if (arWant && artifactReduction.IsReady()) {
                    arApplied = artifactReduction.ProcessFrame(scratch.data(), width, height);
                    std::lock_guard<std::mutex> e(g_state.arErrMtx);
                    if (!arApplied) {
                        const std::string& ne = artifactReduction.LastError();
                        if (g_state.arError != ne) g_state.arError = ne;
                    } else if (!g_state.arError.empty()) { g_state.arError.clear(); }
                }
                g_state.artifactReductionActive.store(arApplied, std::memory_order_release);
```

**Super Resolution — after Green Screen, before publish.** The publish block currently does `g_state.frame.swap(scratch); g_state.width = width; g_state.height = height;`. Replace that publish block (the four lines under `std::lock_guard<std::mutex> lock(g_state.mtx);`) with SR-aware publish:

```cpp
                // Super Resolution runs LAST; it changes the frame dimensions.
                const bool srWant = g_state.superResEnabled.load(std::memory_order_acquire);
                const int  srScale = g_state.superResScale.load(std::memory_order_acquire);
                if (srWant && !superRes.IsReady()) {
                    if (!superRes.Start(srScale)) {
                        std::lock_guard<std::mutex> e(g_state.srErrMtx);
                        const std::string& ne = superRes.LastError();
                        if (g_state.srError != ne) g_state.srError = ne;
                    }
                } else if (!srWant && superRes.IsReady()) {
                    superRes.Stop();
                    std::lock_guard<std::mutex> e(g_state.srErrMtx);
                    if (!g_state.srError.empty()) g_state.srError.clear();
                }

                int pubW = width, pubH = height;
                std::vector<uint8_t> srOut;
                bool srApplied = false;
                if (srWant && superRes.IsReady()) {
                    srApplied = superRes.ProcessFrame(scratch.data(), width, height, srOut, pubW, pubH);
                    std::lock_guard<std::mutex> e(g_state.srErrMtx);
                    if (!srApplied) {
                        const std::string& ne = superRes.LastError();
                        if (g_state.srError != ne) g_state.srError = ne;
                    } else if (!g_state.srError.empty()) { g_state.srError.clear(); }
                }
                g_state.superResActive.store(srApplied, std::memory_order_release);

                {
                    std::lock_guard<std::mutex> lock(g_state.mtx);
                    if (srApplied) g_state.frame.swap(srOut);
                    else           g_state.frame.swap(scratch);
                    g_state.width = pubW;
                    g_state.height = pubH;
                    g_state.hasNewFrame = true;
                    g_state.framesProduced.fetch_add(1, std::memory_order_release);
                }
```

> Note: this replaces the Task 1 Step 6 increment location — the increment now lives inside the new publish block. Remove the standalone increment line added in Task 1 if it is now duplicated.

Add teardown next to the existing `eyeContact.Stop(); aigs.Stop();` (after the loop):

```cpp
    artifactReduction.Stop();
    superRes.Stop();
```

- [ ] **Step 5: Reset active flags + clear errors in `Capture::Stop`**

After the eye-contact reset block add:

```cpp
    g_state.artifactReductionActive.store(false, std::memory_order_release);
    { std::lock_guard<std::mutex> e(g_state.arErrMtx); g_state.arError.clear(); }
    g_state.superResActive.store(false, std::memory_order_release);
    { std::lock_guard<std::mutex> e(g_state.srErrMtx); g_state.srError.clear(); }
```

- [ ] **Step 6: Surface AR/SR errors in `cos_get_status`** — `native/shim/shim.cpp`

In `cos_get_status`, extend the error fallback chain:

```cpp
    std::string err = g_capture.GreenScreenError();
    if (err.empty()) err = g_capture.EyeContactError();
    if (err.empty()) err = g_capture.ArtifactReductionError();
    if (err.empty()) err = g_capture.SuperResError();
```

- [ ] **Step 7: Build the shim (SDK config) — clean, exports intact**

Run the shim build. Expected: 0 warnings; 9 `cos_*` exports. Verify the deployed DLL still exports `GreenScreen` and `GazeRedirection` and lacks `not built in` (grep the DLL).

- [ ] **Step 8: Commit**

```bash
git add native/shim/capture.h native/shim/capture.cpp native/shim/shim.h native/shim/shim.cpp
git commit -m "feat(shim): run Artifact Reduction (first) + Super Resolution (last) on the worker"
```

---

## Task 7: Managed contracts + Core logic (xUnit)

This is the automated-test task. All changes are in `CameraOnScreen.Core` + tests (run in CI), plus the `PInvokeShim` mirror in the App (build-verified).

**Files:**
- Modify: `src/CameraOnScreen.Core/Native/Contracts.cs`, `FakeShim.cs`, `Config/Models.cs`, `Orchestration/Orchestrator.cs`, `ViewModels/MainViewModel.cs`
- Modify: `src/CameraOnScreen.App/Native/PInvokeShim.cs`
- Test: `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`, `tests/CameraOnScreen.Core.Tests/Orchestration/OrchestratorTests.cs`

**Interfaces:**
- Produces: `ShimParams` gains `bool ArtifactReductionEnabled = false, bool SuperResEnabled = false, int SuperResScale = 0` (defaulted — existing call sites keep compiling). `ShimCapabilities` gains `bool ArtifactReductionAvailable = false, bool SuperResAvailable = false`. `MainViewModel` gains `ArtifactReductionEnabled`, `SuperResEnabled`, `SuperResScaleIndex` (0=off,1=1.5x,2=2x), `ArtifactReductionAvailable`, `SuperResAvailable`.

- [ ] **Step 1: Write the failing test** — append to `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`

```csharp
[Fact]
public void BuildParams_includes_artifact_reduction_and_superres()
{
    var shim = new FakeShim { GreenScreenAvailable = true, EyeContactAvailable = true,
                              ArtifactReductionAvailable = true, SuperResAvailable = true };
    var orch = new Orchestrator(shim, GpuTier.Rtx);
    orch.ProbeCapabilities();
    var vm = new MainViewModel(orch, shim);

    vm.ArtifactReductionEnabled = true;
    vm.SuperResEnabled = true;
    vm.SuperResScaleIndex = 2; // 2x

    var p = vm.BuildParams();
    Assert.True(p.ArtifactReductionEnabled);
    Assert.True(p.SuperResEnabled);
    Assert.Equal(20, p.SuperResScale);
}

[Fact]
public void ToAppConfig_roundtrips_new_effects()
{
    var shim = new FakeShim();
    var vm = new MainViewModel(new Orchestrator(shim, GpuTier.Rtx), shim)
    {
        ArtifactReductionEnabled = true, SuperResEnabled = true, SuperResScaleIndex = 1
    };
    var cfg = vm.ToAppConfig(0, 0, 320, 240);
    Assert.True(cfg.Effects.ArtifactReductionEnabled);
    Assert.True(cfg.Effects.SuperResEnabled);
    Assert.Equal(15, cfg.Effects.SuperResScale);

    var vm2 = new MainViewModel(new Orchestrator(shim, GpuTier.Rtx), shim);
    vm2.LoadFrom(cfg);
    Assert.True(vm2.ArtifactReductionEnabled);
    Assert.Equal(1, vm2.SuperResScaleIndex);
}
```

Append to `tests/CameraOnScreen.Core.Tests/Orchestration/OrchestratorTests.cs`:

```csharp
[Fact]
public void ApplyParams_forces_effects_off_when_unavailable()
{
    var shim = new FakeShim { GreenScreenAvailable = false, ArtifactReductionAvailable = false, SuperResAvailable = false };
    var orch = new Orchestrator(shim, GpuTier.Rtx);
    orch.ProbeCapabilities();
    orch.ApplyParams(new ShimParams("cam", true, 0, 0, false, 0.5, 0.5,
        ArtifactReductionEnabled: true, SuperResEnabled: true, SuperResScale: 20));
    Assert.False(shim.LastParams!.ArtifactReductionEnabled);
    Assert.False(shim.LastParams!.SuperResEnabled);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~artifact OR FullyQualifiedName~SuperRes OR FullyQualifiedName~forces_effects_off"`
Expected: FAIL to compile — new members do not exist.

- [ ] **Step 3: Extend `ShimParams` + `ShimCapabilities`** — `src/CameraOnScreen.Core/Native/Contracts.cs`

```csharp
public sealed record ShimParams(
    string? CameraId,
    bool GreenScreenEnabled,
    double GreenScreenExpand,
    double GreenScreenFeather,
    bool EyeContactEnabled,
    double EyeContactSensitivity,
    double EyeContactLookAwayRange,
    bool ArtifactReductionEnabled = false,
    bool SuperResEnabled = false,
    int SuperResScale = 0);            // 0=off, 15=1.5x, 20=2x
```

```csharp
public sealed record ShimCapabilities(
    bool GreenScreenAvailable, string Detail,
    bool EyeContactAvailable = false, string EyeContactDetail = "",
    bool ArtifactReductionAvailable = false, bool SuperResAvailable = false);
```

- [ ] **Step 4: Extend `EffectSettings`** — `src/CameraOnScreen.Core/Config/Models.cs`

Add to the record:

```csharp
    public bool ArtifactReductionEnabled { get; init; }
    public bool SuperResEnabled { get; init; }
    public int SuperResScale { get; init; } // 0=off, 15=1.5x, 20=2x
```

- [ ] **Step 5: Extend `FakeShim`** — `src/CameraOnScreen.Core/Native/FakeShim.cs`

Add properties:

```csharp
    public bool ArtifactReductionAvailable { get; set; }
    public bool SuperResAvailable { get; set; }
```

Update `QueryCapabilities`:

```csharp
    public ShimCapabilities QueryCapabilities() =>
        new(GreenScreenAvailable,
            GreenScreenAvailable ? "fake: available" : "fake: unavailable",
            EyeContactAvailable,
            EyeContactAvailable ? "fake: ec available" : "fake: ec unavailable",
            ArtifactReductionAvailable, SuperResAvailable);
```

- [ ] **Step 6: Extend `Orchestrator`** — `src/CameraOnScreen.Core/Orchestration/Orchestrator.cs`

Add properties:

```csharp
    public bool ArtifactReductionAvailable { get; private set; }
    public bool SuperResAvailable { get; private set; }
```

In `ProbeCapabilities` add:

```csharp
        ArtifactReductionAvailable = caps.ArtifactReductionAvailable;
        SuperResAvailable = caps.SuperResAvailable;
```

In `ApplyParams`, extend the `with`:

```csharp
        var effective = requested with
        {
            GreenScreenEnabled = requested.GreenScreenEnabled && EffectsAvailable,
            EyeContactEnabled = requested.EyeContactEnabled && EyeContactAvailable,
            ArtifactReductionEnabled = requested.ArtifactReductionEnabled && ArtifactReductionAvailable,
            SuperResEnabled = requested.SuperResEnabled && SuperResAvailable,
        };
```

- [ ] **Step 7: Extend `MainViewModel`** — `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs`

Add observable props (next to the green-screen ones):

```csharp
    [ObservableProperty] private bool artifactReductionEnabled;
    [ObservableProperty] private bool superResEnabled;
    [ObservableProperty] private int superResScaleIndex; // 0=off, 1=1.5x, 2=2x
    [ObservableProperty] private bool artifactReductionAvailable;
    [ObservableProperty] private bool superResAvailable;
```

Map index→scale once:

```csharp
    // SuperResScaleIndex 0/1/2 -> shim scale 0/15/20.
    private static int ScaleFromIndex(int i) => i switch { 1 => 15, 2 => 20, _ => 0 };
    private static int IndexFromScale(int s) => s switch { 15 => 1, 20 => 2, _ => 0 };
```

In the ctor (after the existing `EyeContact*` mirroring) add:

```csharp
        ArtifactReductionAvailable = orchestrator.ArtifactReductionAvailable;
        SuperResAvailable = orchestrator.SuperResAvailable;
```

In `ProbeCapabilitiesAsync` (after the existing assignments) add:

```csharp
        ArtifactReductionAvailable = _orchestrator.ArtifactReductionAvailable;
        SuperResAvailable = _orchestrator.SuperResAvailable;
```

Add live-push partials:

```csharp
    partial void OnArtifactReductionEnabledChanged(bool value) => ApplyLiveParams();
    partial void OnSuperResEnabledChanged(bool value) => ApplyLiveParams();
    partial void OnSuperResScaleIndexChanged(int value) => ApplyLiveParams();
```

Extend `BuildParams`:

```csharp
    public ShimParams BuildParams() => new(
        CameraId: SelectedCamera?.Id,
        GreenScreenEnabled: GreenScreenEnabled,
        GreenScreenExpand: GreenScreenExpand,
        GreenScreenFeather: GreenScreenFeather,
        EyeContactEnabled: EyeContactEnabled,
        EyeContactSensitivity: EyeContactSensitivity,
        EyeContactLookAwayRange: EyeContactLookAwayRange,
        ArtifactReductionEnabled: ArtifactReductionEnabled,
        SuperResEnabled: SuperResEnabled,
        SuperResScale: ScaleFromIndex(SuperResScaleIndex));
```

Extend `LoadFrom` (after the eye-contact lines):

```csharp
        ArtifactReductionEnabled = config.Effects.ArtifactReductionEnabled;
        SuperResEnabled = config.Effects.SuperResEnabled;
        SuperResScaleIndex = IndexFromScale(config.Effects.SuperResScale);
```

Extend `ToAppConfig`'s `Effects = new EffectSettings { ... }`:

```csharp
            ArtifactReductionEnabled = ArtifactReductionEnabled,
            SuperResEnabled = SuperResEnabled,
            SuperResScale = ScaleFromIndex(SuperResScaleIndex),
```

- [ ] **Step 8: Run the new tests to verify they pass**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~artifact OR FullyQualifiedName~SuperRes OR FullyQualifiedName~forces_effects_off"`
Expected: PASS.

- [ ] **Step 9: Mirror the ABI in `PInvokeShim`** — `src/CameraOnScreen.App/Native/PInvokeShim.cs`

Extend `CosParams`:

```csharp
    [StructLayout(LayoutKind.Sequential)]
    private struct CosParams
    {
        public IntPtr camera_id;
        public int green_screen_enabled; public double green_screen_expand;
        public double green_screen_feather;
        public int eye_contact_enabled; public double eye_contact_sensitivity;
        public double eye_contact_look_away_range;
        public int artifact_reduction_enabled;
        public int super_res_enabled;
        public int super_res_scale;
    }
```

Extend `CosCaps`:

```csharp
    [StructLayout(LayoutKind.Sequential)]
    private struct CosCaps
    {
        public int GreenScreenAvailable;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)] public byte[] Detail;
        public int EyeContactAvailable;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)] public byte[] EcDetail;
        public int ArtifactReductionAvailable;
        public int SuperResAvailable;
    }
```

Extend `SetParams`' `new CosParams { ... }`:

```csharp
                artifact_reduction_enabled = p.ArtifactReductionEnabled ? 1 : 0,
                super_res_enabled = p.SuperResEnabled ? 1 : 0,
                super_res_scale = p.SuperResScale,
```

Extend `QueryCapabilities`' returned `ShimCapabilities`:

```csharp
        return new ShimCapabilities(
            caps.GreenScreenAvailable != 0, ReadUtf8(caps.Detail, 0, 256),
            caps.EyeContactAvailable != 0, ReadUtf8(caps.EcDetail, 0, 256),
            caps.ArtifactReductionAvailable != 0, caps.SuperResAvailable != 0);
```

- [ ] **Step 10: Run the full Core test suite (no regressions)**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
Expected: all PASS, 0 warnings.

- [ ] **Step 11: Commit**

```bash
git add src/CameraOnScreen.Core src/CameraOnScreen.App/Native/PInvokeShim.cs tests/CameraOnScreen.Core.Tests
git commit -m "feat(core): AR/SR params, capability gates, persistence, live push"
```

---

## Task 8: App UI — toggles, scale combo, 4K frame buffer

**Files:**
- Modify: `src/CameraOnScreen.App/MainWindow.xaml`, `src/CameraOnScreen.App/MainWindow.xaml.cs`

**Interfaces:**
- Consumes: `MainViewModel.ArtifactReductionEnabled/SuperResEnabled/SuperResScaleIndex/ArtifactReductionAvailable/SuperResAvailable` (Task 7).

- [ ] **Step 1: Add controls** — `src/CameraOnScreen.App/MainWindow.xaml`

After the Eye Contact toggle + its note (`</TextBlock>` ending the eye-contact detail), add:

```xml
            <ToggleSwitch Header="Artifact Reduction"
                          IsEnabled="{x:Bind Vm.ArtifactReductionAvailable, Mode=OneWay}"
                          IsOn="{x:Bind Vm.ArtifactReductionEnabled, Mode=TwoWay}"/>
            <ToggleSwitch Header="Super Resolution"
                          IsEnabled="{x:Bind Vm.SuperResAvailable, Mode=OneWay}"
                          IsOn="{x:Bind Vm.SuperResEnabled, Mode=TwoWay}"/>
            <ComboBox Header="Super-res scale"
                      IsEnabled="{x:Bind Vm.SuperResEnabled, Mode=OneWay}"
                      SelectedIndex="{x:Bind Vm.SuperResScaleIndex, Mode=TwoWay}">
                <ComboBoxItem Content="Off"/>
                <ComboBoxItem Content="1.5x"/>
                <ComboBoxItem Content="2x"/>
            </ComboBox>
```

- [ ] **Step 2: Pre-size the frame buffer to 4K** — `src/CameraOnScreen.App/MainWindow.xaml.cs:25`

Replace:

```csharp
    // Big enough for 1920x1080 BGRA; the test camera is 640x480. TryGetFrame writes the actual size.
    private readonly byte[] _frameBuf = new byte[1920 * 1080 * 4];
```

with:

```csharp
    // Pre-sized to 4K (3840x2160) BGRA so Super Resolution (up to 2x of 1080p) fits without a
    // resize. TryGetFrame writes the actual size; cos_get_frame rejects frames larger than this.
    private readonly byte[] _frameBuf = new byte[3840 * 2160 * 4];
```

- [ ] **Step 3: Build the App (shim built SDK-config last) — clean**

Run:
```
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild
```
Expected: 0 warnings, build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/CameraOnScreen.App/MainWindow.xaml src/CameraOnScreen.App/MainWindow.xaml.cs
git commit -m "feat(app): Artifact Reduction + Super Resolution UI, 4K frame buffer"
```

---

## Task 9: Bundle the new feature DLLs + models

**Files:**
- Modify: `native/shim/bundle/maxine-manifest.psd1`, `scripts/assemble-maxine-stage.ps1`

**Interfaces:**
- Consumes: the produced bundle is verified by `trace_closure` / `verify-bundle.ps1`.

> **Verify at impl:** confirm the exact VFX feature-DLL filenames for Artifact Reduction and Super Resolution in the VFX 1.2.0.0 SDK `bin` tree (alongside `nvVFXGreenScreen.dll`), and the model file globs under `models` for each effect.

- [ ] **Step 1: Add the feature DLLs to the manifest** — `native/shim/bundle/maxine-manifest.psd1`

Add the AR + SR feature DLL names to the `Dlls` list (next to the green-screen feature DLL), e.g.:

```powershell
    'nvVFXArtifactReduction.dll'
    'nvVFXSuperRes.dll'
```

- [ ] **Step 2: Ensure the model globs cover the new effects** — `scripts/assemble-maxine-stage.ps1`

In the VFX model copy step (the one that stages `models\vfx`), confirm the glob includes the AR + SR model files (they live under the same VFX `models` tree as green screen; if the script copies the whole `models\vfx` dir, no change is needed — verify it is not filtering to green-screen-only model names).

- [ ] **Step 3: Re-stage + re-run the load-closure trace (dev box)**

Run `scripts/assemble-maxine-stage.ps1` then `scripts/bundle-maxine.ps1 -OutDir <out> -MaxineStage <stage>`, then run `native/shim/smoke/trace_closure` (or `bundle_probe`) against the produced bundle with `COS_*` unset.
Expected: AR + SR both load (Probe success); the trace lists the new feature DLLs in the closure; no `not built in`.

- [ ] **Step 4: Update the manifest `Dlls` list from the trace output** if the trace surfaced additional dependency DLLs for the new effects, then re-run the trace to confirm a stable closure.

- [ ] **Step 5: Commit**

```bash
git add native/shim/bundle/maxine-manifest.psd1 scripts/assemble-maxine-stage.ps1
git commit -m "build(bundle): stage Artifact Reduction + Super Resolution DLLs + models"
```

---

## Final verification (human gate)

- [ ] Build shim **SDK config last**, build App, run. Confirm:
  - Real fps shows in the status line and drops as effects stack (Task 1).
  - Artifact Reduction toggle visibly removes MJPG blockiness on the webcam.
  - Super Resolution at 2× visibly adds detail when the overlay is shown large; the overlay re-pins to the larger frame without artifacts.
  - Toggles grey out correctly when the probe reports an effect unavailable.
- [ ] Visual/recorder confirmation via Windows.Graphics.Capture / OBS (inherent human gate, `docs/superpowers/verification/`).

> Reminder (spec §13): stacking AR + Eye Contact + Green Screen + Super Res will not sustain high fps on an RTX 3090 — the fps readout is the tuning signal; no automatic management by design.

---

## Self-Review

**Spec coverage:** Artifact Reduction (Tasks 3,5,6,7,8,9) ✓; Super Resolution incl. 2× cap + 4K buffer (Tasks 4,5,6,7,8,9) ✓; real fps counter (Task 1) ✓; independent toggles + capability gates + live push + JSON persistence (Task 7) ✓; worker order AR-first/SR-last (Task 6) ✓; ABI parity 528-byte CosCaps + 3 CosParams fields (Tasks 5,7) ✓; bundler (Task 9) ✓; no-auto-management / fps-as-tuning-signal (final gate note) ✓; minification fix correctly **absent** (deferred, spec §11) ✓.

**Placeholder scan:** no TBD/TODO; "verify at impl" notes name the exact symbol to confirm (selector macros, feature-DLL filenames) — these are real verification steps against headers, not gaps. Every code step shows complete code.

**Type consistency:** `ShimParams`/`CosParams` field names and order match across `shim.h`, `PInvokeShim`, `Contracts.cs`, `BuildParams`. `SuperResScale` is the shim-facing int (0/15/20); `SuperResScaleIndex` is the VM/combo int (0/1/2) with `ScaleFromIndex`/`IndexFromScale` as the single conversion point. `ArtifactReductionAvailable`/`SuperResAvailable` consistent across `CosCaps`, `ShimCapabilities`, `FakeShim`, `Orchestrator`, `MainViewModel`. Effect class APIs (`Probe`/`Start`/`Stop`/`ProcessFrame`/`IsReady`/`LastError`) consistent with the `capture.cpp` call sites in Task 6.
