# M3: AI Green Screen (CPU-copy) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run NVIDIA Maxine AI Green Screen in the native shim so the webcam subject appears on a transparent background in the existing overlay, captured live by any screen recorder.

**Architecture:** All new work is native. The capture worker, after Media Foundation produces a BGRA frame, runs the Maxine GreenScreen effect on the GPU via a **CPU-copy** round-trip (upload BGRA→GPU, run, download matte→CPU), then composites the matte into the same BGRA buffer as **premultiplied alpha**. The existing `cos_get_frame` → managed dynamic-texture → DirectComposition overlay path is unchanged because alpha is just the 4th byte of the same buffer. A new `cos_query_capabilities` export becomes the real effect gate, replacing the RTX-substring heuristic.

**Tech Stack:** C++17 native shim (Media Foundation + Maxine VFX SDK, proxy-stub linking, no import lib), C# .NET 8 Core (xUnit), WinUI 3 App (P/Invoke). Maxine VFX SDK `nvvfxgreenscreen` 1.2.0.0, compute capability 86 (RTX 3090).

## Global Constraints

- **Build is pristine: 0 warnings** in both the shim (MSBuild) and `dotnet build`/`dotnet test`. Warnings are findings.
- **Build order:** native shim FIRST via Build Tools MSBuild run from **PowerShell** (`& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64`), THEN `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild`, THEN `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`. The `.vcxproj` is NOT in the `.sln`.
- **C-ABI struct byte-parity (x64) is load-bearing.** `CosCaps` (C) and its `[StructLayout(LayoutKind.Sequential)]` managed mirror must match byte-for-byte. The 128-byte enumeration stride convention and UTF-8 string handling already in the shim are unchanged.
- **The shim never renders and never creates a window.** C# owns 100% of windowing/compositing. M3 adds no D3D/DComp work to the shim.
- **CPU-copy only.** No CUDA↔D3D11 interop, no `OpenSharedResource`, no use of the `d3d11_device` passed to `cos_init` in M3. The `D3DDevicePtr` stays unused (reserved for the future GPU path).
- **Output is premultiplied BGRA.** The overlay swap chain is `AlphaMode.Premultiplied`; the shim emits `RGB *= alpha/255`, `A = matte`.
- **SDK location via `COS_VFX_SDK_DIR`** (env var) / `CosVfxSdkDir` (MSBuild property). **No absolute SDK paths committed to the repo. No SDK DLLs or models committed to git or copied into build output.**
- **Effect gate = `cos_query_capabilities`**, not the RTX-substring heuristic. `GpuTierDetector` is retained ONLY for the GPU-name display string.
- **Green screen is on/off only** in M3. `CosParams.green_screen_strength` is accepted but not consumed.
- **Core tests pass with NO SDK present** (FakeShim reports effects unavailable). The shim must also *build* with no SDK present (passthrough stub), so CI and SDK-less machines stay green.
- Spec: `docs/superpowers/specs/2026-06-21-camera-on-screen-m3-aigs-design.md`. Parent: `docs/superpowers/specs/2026-06-20-camera-on-screen-design.md`.

## File Structure

- `native/shim/aigs.h` — **create.** `Aigs` class interface: `Probe`, `Start`, `Stop`, `ProcessFrame`, `IsReady`, `LastError`.
- `native/shim/aigs.cpp` — **create.** Maxine GreenScreen wrapper. Real implementation behind `#ifdef COS_HAS_MAXINE`; passthrough stub otherwise. Defines the proxy globals (`g_nvVFXSDKPath`, `g_nvCVImageSDKPath`).
- `native/shim/shim.vcxproj` — **modify.** Add SDK include dirs, compile the two proxy `.cpp` stubs, define `COS_HAS_MAXINE` when `CosVfxSdkDir` is set, add `aigs.cpp`.
- `native/shim/shim.h` — **modify.** Add `CosCaps` struct + `cos_query_capabilities` export.
- `native/shim/shim.cpp` — **modify.** Implement `cos_query_capabilities`; set `green_screen_active`/`error` in `cos_get_status`; pass the green-screen-enabled flag to capture.
- `native/shim/capture.h` / `capture.cpp` — **modify.** Run AIGS in the worker loop; own the `Aigs` lifecycle on the worker thread; expose a setter for the enabled flag.
- `native/shim/smoke/aigs_smoke.cpp` — **create.** Standalone headless smoke harness (built ad hoc, not part of the DLL) to verify init + a non-degenerate matte on the target machine.
- `src/CameraOnScreen.Core/Shim/INativeShim.cs` — **modify.** Add `QueryCapabilities()`.
- `src/CameraOnScreen.Core/Shim/ShimCapabilities.cs` — **create.** Managed capability record.
- `src/CameraOnScreen.Core/Shim/FakeShim.cs` — **modify.** Implement `QueryCapabilities` (configurable).
- `src/CameraOnScreen.Core/Orchestrator.cs` — **modify.** Gate effects on `QueryCapabilities`.
- `src/CameraOnScreen.App/Native/PInvokeShim.cs` — **modify.** Marshal `cos_query_capabilities`; add `CosCaps` mirror.
- `tests/CameraOnScreen.Core.Tests/*` — **modify.** Capability + gating tests.
- `docs/superpowers/verification/2026-06-20-recorder-capture.md` — **modify.** Append the M3 manual gate.

> **Note on existing names:** Before Task 6/7, the implementer must open `INativeShim.cs`, `FakeShim.cs`, `Orchestrator.cs`, and `PInvokeShim.cs` to confirm the exact existing member names (e.g. how `EffectsAvailable` / the RTX tier currently flow). The signatures below are the contract to add; match the file's existing style and namespaces.

---

### Task 1: Native build wiring for the Maxine SDK (CI-safe)

Set up the build so the shim compiles **with** the SDK (real Maxine) and **without** it (passthrough stub), before writing any effect logic. This isolates the highest build-system risk first.

**Files:**
- Modify: `native/shim/shim.vcxproj`
- Create: `native/shim/aigs.h`
- Create: `native/shim/aigs.cpp`

**Interfaces:**
- Produces: the `Aigs` class (declared here, implemented across Tasks 2–3) and the `COS_HAS_MAXINE` compile switch. Consumed by `capture.cpp` (Task 5) and `shim.cpp` (Task 4).

- [ ] **Step 1: Create `native/shim/aigs.h`**

```cpp
#pragma once
#include <cstdint>
#include <string>

// Wraps the Maxine GreenScreen effect. CPU-copy: a BGRA frame is uploaded to the
// GPU, the matte is computed, downloaded, and composited into the same BGRA buffer
// as premultiplied alpha. All methods are no-throw; failure is reported via IsReady()
// + LastError(). When built without the SDK (COS_HAS_MAXINE undefined) this is a
// stub that is never ready, so the shim degrades to opaque passthrough.
class Aigs {
public:
    Aigs();
    ~Aigs();

    // One-shot probe: can the SDK load and the GreenScreen effect be created+loaded?
    // Does not retain the effect. Safe to call from any thread. Fills 'detail'.
    static bool Probe(std::string& detail);

    // Create the effect, CUDA stream, and (lazily) the GPU images. Call on the
    // capture worker thread. Returns true on success; on failure IsReady()==false.
    bool Start();

    // Destroy the effect, stream, and images. Call on the worker thread.
    void Stop();

    // Run GreenScreen on a tightly-packed BGRA buffer (width*height*4) in place:
    // A = matte, RGB premultiplied by matte/255. Returns true if the matte was
    // applied; false leaves 'bgra' untouched (caller keeps opaque passthrough).
    bool ProcessFrame(uint8_t* bgra, int width, int height);

    bool IsReady() const { return ready_; }
    const std::string& LastError() const { return lastError_; }

private:
    bool ready_ = false;
    std::string lastError_;
    void* impl_ = nullptr; // opaque; real fields live in aigs.cpp behind COS_HAS_MAXINE
};
```

- [ ] **Step 2: Create `native/shim/aigs.cpp` with the stub path only (real path added in Tasks 2–3)**

```cpp
#include "aigs.h"

#ifdef COS_HAS_MAXINE
// Real implementation is added in Tasks 2 and 3.
#else
// ---- Passthrough stub: built when no SDK is configured. ----
Aigs::Aigs() = default;
Aigs::~Aigs() = default;
bool Aigs::Probe(std::string& detail) { detail = "Maxine SDK not built in"; return false; }
bool Aigs::Start() { lastError_ = "Maxine SDK not built in"; ready_ = false; return false; }
void Aigs::Stop() { ready_ = false; }
bool Aigs::ProcessFrame(uint8_t*, int, int) { return false; }
#endif
```

- [ ] **Step 3: Wire the SDK into `shim.vcxproj`**

Add a property that resolves the SDK directory (env var by default), the include dirs, the `COS_HAS_MAXINE` define, the two proxy stubs, and `aigs.cpp`. Insert near the existing `<ItemGroup>`/`<PropertyGroup>` entries (match the file's existing layout):

```xml
<PropertyGroup>
  <!-- SDK dir: MSBuild property overrides the COS_VFX_SDK_DIR env var. Empty => stub build. -->
  <CosVfxSdkDir Condition="'$(CosVfxSdkDir)' == ''">$(COS_VFX_SDK_DIR)</CosVfxSdkDir>
</PropertyGroup>

<ItemDefinitionGroup Condition="'$(CosVfxSdkDir)' != ''">
  <ClCompile>
    <PreprocessorDefinitions>COS_HAS_MAXINE;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    <AdditionalIncludeDirectories>$(CosVfxSdkDir)\nvvfx\include;$(CosVfxSdkDir)\features\nvvfxgreenscreen\include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
  </ClCompile>
</ItemDefinitionGroup>

<ItemGroup>
  <ClCompile Include="aigs.cpp" />
</ItemGroup>

<!-- Proxy stubs provide the Maxine API by LoadLibrary'ing the runtime DLLs (no import .lib). -->
<ItemGroup Condition="'$(CosVfxSdkDir)' != ''">
  <ClCompile Include="$(CosVfxSdkDir)\nvvfx\src\nvVideoEffectsProxy.cpp" />
  <ClCompile Include="$(CosVfxSdkDir)\nvvfx\src\nvCVImageProxy.cpp" />
</ItemGroup>
```

Also add `aigs.h` to the existing `<ItemGroup>` of headers if the project lists them.

- [ ] **Step 4: Build the stub configuration (no SDK) and verify 0 warnings**

Run (PowerShell), with `CosVfxSdkDir` forced empty:
```
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:CosVfxSdkDir=
```
Expected: `Build succeeded. 0 Warning(s) 0 Error(s)`. `COS_HAS_MAXINE` undefined → stub compiled.

- [ ] **Step 5: Build the SDK configuration and verify 0 warnings**

Run (PowerShell), with the SDK present and `COS_VFX_SDK_DIR` set in the environment (or pass `/p:CosVfxSdkDir=...`):
```
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
```
Expected: `Build succeeded. 0 Warning(s) 0 Error(s)`. The proxy stubs + `aigs.cpp` real path (after Tasks 2–3) compile against the SDK headers. At this task (stub body only) it still builds clean because `aigs.cpp`'s `#ifdef` branch is empty of real code yet.

- [ ] **Step 6: Verify the C ABI is unchanged**

Run (PowerShell), using the MSVC `dumpbin`:
```
& "$(Get-ChildItem 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC' -Directory | Select-Object -Last 1 -ExpandProperty FullName)\bin\Hostx64\x64\dumpbin.exe" /exports native/shim/x64/Debug/CameraOnScreen.Shim.dll
```
Expected: the existing 8 exports (`cos_init`, `cos_enumerate_cameras`, `cos_set_params`, `cos_start`, `cos_stop`, `cos_get_status`, `cos_get_frame`, `cos_shutdown`) — no change yet.

- [ ] **Step 7: Commit**

```bash
git add native/shim/aigs.h native/shim/aigs.cpp native/shim/shim.vcxproj
git commit -m "build(m3): wire Maxine VFX SDK into shim with CI-safe stub fallback"
```

---

### Task 2: AIGS init + availability probe

Implement the real `Aigs::Start`/`Stop`/`Probe` (effect creation, model load, SDK path hookup). No per-frame processing yet.

**Files:**
- Modify: `native/shim/aigs.cpp`
- Create: `native/shim/smoke/aigs_smoke.cpp`

**Interfaces:**
- Consumes: `Aigs` from Task 1.
- Produces: a working `Aigs::Start()` (sets `ready_`), `Aigs::Probe()`, and the proxy globals. `ProcessFrame` still returns false until Task 3.

- [ ] **Step 1: Add the real implementation block in `aigs.cpp` (inside `#ifdef COS_HAS_MAXINE`)**

```cpp
#include <windows.h>
#include <cstdlib>
#include <string>
#include "nvCVStatus.h"
#include "nvCVImage.h"
#include "nvVideoEffects.h"
#include "nvVFXGreenScreen.h" // defines NVVFX_FX_GREEN_SCREEN "GreenScreen"

// The proxy stubs (nvVideoEffectsProxy.cpp / nvCVImageProxy.cpp) declare these as
// extern; the consumer must define them. When non-null they point at the dir that
// holds NVVideoEffects.dll / NVCVImage.dll, which the proxies pass to SetDllDirectory.
char* g_nvVFXSDKPath = nullptr;
char* g_nvCVImageSDKPath = nullptr;

namespace {
// Resolves "<COS_VFX_SDK_DIR>\bin" (DLLs) and "<COS_VFX_SDK_DIR>\bin\models" once.
// Returns false if the env var is unset/empty. Stores results in static strings so
// the char* globals stay valid for the process lifetime.
bool ResolveSdkPaths(std::string& binDir, std::string& modelDir, std::string& err) {
    char buf[1024] = {0};
    DWORD n = GetEnvironmentVariableA("COS_VFX_SDK_DIR", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) { err = "COS_VFX_SDK_DIR not set"; return false; }
    std::string root(buf, n);
    if (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
    binDir   = root + "\\bin";
    modelDir = root + "\\bin\\models";
    return true;
}

// Holds the SDK dir string for the lifetime of the process and points the proxy
// globals at it. Idempotent.
void PointProxiesAt(const std::string& binDir) {
    static std::string s_bin;        // must outlive every Maxine call
    s_bin = binDir;
    g_nvVFXSDKPath = const_cast<char*>(s_bin.c_str());
    g_nvCVImageSDKPath = const_cast<char*>(s_bin.c_str());
}
} // namespace

// Real per-effect state, hidden behind the opaque impl_ pointer.
struct AigsImpl {
    NvVFX_Handle effect = nullptr;
    CUstream     stream = nullptr;
    std::string  modelDir;
    // GPU/CPU images are added in Task 3.
};
```

- [ ] **Step 2: Implement `Probe`, `Start`, `Stop` (still inside `#ifdef COS_HAS_MAXINE`)**

```cpp
Aigs::Aigs() = default;
Aigs::~Aigs() { Stop(); }

bool Aigs::Probe(std::string& detail) {
    std::string binDir, modelDir, err;
    if (!ResolveSdkPaths(binDir, modelDir, err)) { detail = err; return false; }
    PointProxiesAt(binDir);

    NvVFX_Handle eff = nullptr;
    if (NvVFX_CreateEffect(NVVFX_FX_GREEN_SCREEN, &eff) != NVCV_SUCCESS || !eff) {
        detail = "NvVFX_CreateEffect(GreenScreen) failed (DLL/SDK load?)";
        return false;
    }
    NvVFX_SetString(eff, NVVFX_MODEL_DIRECTORY, modelDir.c_str());
    NvCV_Status load = NvVFX_Load(eff);
    NvVFX_DestroyEffect(eff);
    if (load != NVCV_SUCCESS) { detail = "NvVFX_Load failed (models for this GPU?)"; return false; }
    detail = "GreenScreen available";
    return true;
}

bool Aigs::Start() {
    Stop();
    auto* impl = new AigsImpl();
    impl_ = impl;

    std::string binDir, err;
    if (!ResolveSdkPaths(binDir, impl->modelDir, err)) { lastError_ = err; return false; }
    PointProxiesAt(binDir);

    if (NvVFX_CudaStreamCreate(&impl->stream) != NVCV_SUCCESS) {
        lastError_ = "NvVFX_CudaStreamCreate failed"; return false;
    }
    if (NvVFX_CreateEffect(NVVFX_FX_GREEN_SCREEN, &impl->effect) != NVCV_SUCCESS || !impl->effect) {
        lastError_ = "NvVFX_CreateEffect failed"; return false;
    }
    NvVFX_SetString(impl->effect, NVVFX_MODEL_DIRECTORY, impl->modelDir.c_str());
    NvVFX_SetCudaStream(impl->effect, NVVFX_CUDA_STREAM, impl->stream);
    NvVFX_SetU32(impl->effect, NVVFX_MODE, 1u);     // 0=best quality, 1=fastest; confirm against docs
    NvVFX_SetU32(impl->effect, NVVFX_TEMPORAL, 1u); // video: reduce matte flicker
    // Images are set per-frame in Task 3; NvVFX_Load happens after the first SetImage there.
    ready_ = true; // "configured"; full readiness (model loaded) is confirmed on first frame
    lastError_.clear();
    return true;
}

void Aigs::Stop() {
    auto* impl = static_cast<AigsImpl*>(impl_);
    if (!impl) { ready_ = false; return; }
    if (impl->effect) NvVFX_DestroyEffect(impl->effect);
    if (impl->stream) NvVFX_CudaStreamDestroy(impl->stream);
    delete impl;
    impl_ = nullptr;
    ready_ = false;
}

bool Aigs::ProcessFrame(uint8_t*, int, int) { return false; } // implemented in Task 3
```

> **Note:** AIGS requires `NvVFX_Load` *after* input/output image dimensions are known (it builds the engine for the input size). Task 3 calls `NvVFX_Load` after the first `NvVFX_SetImage`. Keep the `Start()` `ready_=true` meaning "effect configured"; Task 3 sets a separate "model loaded" guard.

- [ ] **Step 3: Create the smoke harness `native/shim/smoke/aigs_smoke.cpp`**

```cpp
// Headless smoke test for AIGS init. Build ad hoc (not part of the DLL):
//   cl /EHsc /std:c++17 /I "%COS_VFX_SDK_DIR%\nvvfx\include" /I "%COS_VFX_SDK_DIR%\features\nvvfxgreenscreen\include" /DCOS_HAS_MAXINE ^
//      native\shim\smoke\aigs_smoke.cpp native\shim\aigs.cpp ^
//      "%COS_VFX_SDK_DIR%\nvvfx\src\nvVideoEffectsProxy.cpp" "%COS_VFX_SDK_DIR%\nvvfx\src\nvCVImageProxy.cpp"
#include <cstdio>
#include <string>
#include "../aigs.h"

int main() {
    std::string detail;
    bool ok = Aigs::Probe(detail);
    std::printf("Probe: %s (%s)\n", ok ? "AVAILABLE" : "unavailable", detail.c_str());
    if (!ok) return 1;

    Aigs a;
    bool started = a.Start();
    std::printf("Start: %s (%s)\n", started ? "OK" : "FAIL", a.LastError().c_str());
    a.Stop();
    return started ? 0 : 2;
}
```

- [ ] **Step 4: Build the shim (SDK config) and verify 0 warnings**

Run (PowerShell, `COS_VFX_SDK_DIR` set):
```
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
```
Expected: `0 Warning(s) 0 Error(s)`.

- [ ] **Step 5: Build + run the smoke harness on the target machine**

From a Developer PowerShell (so `cl` is on PATH), with `COS_VFX_SDK_DIR` set, run the `cl` command in the file header comment, then:
```
.\aigs_smoke.exe
```
Expected:
```
Probe: AVAILABLE (GreenScreen available)
Start: OK ()
```
If it fails, the detail/error string says which step (DLL load vs model load) — fix before proceeding.

- [ ] **Step 6: Commit**

```bash
git add native/shim/aigs.cpp native/shim/smoke/aigs_smoke.cpp
git commit -m "feat(m3): AIGS effect init + availability probe (no per-frame yet)"
```

---

### Task 3: AIGS ProcessFrame — upload, run, download, composite

Implement the per-frame transform with the **swappable seam**: `Upload`, `Download`, `Composite` as separate functions so the future GPU/zero-copy path replaces only these.

**Files:**
- Modify: `native/shim/aigs.cpp`
- Modify: `native/shim/smoke/aigs_smoke.cpp`

**Interfaces:**
- Consumes: `AigsImpl` (Task 2).
- Produces: a working `Aigs::ProcessFrame(uint8_t* bgra, int w, int h)` that writes premultiplied BGRA with `A = matte`.

- [ ] **Step 1: Extend `AigsImpl` with the persistent images and a model-loaded guard**

Add these fields to `struct AigsImpl` (Task 2):
```cpp
    NvCVImage srcGpu{};   // BGR  u8 chunky, GPU  (AIGS input)
    NvCVImage matteGpu{}; // A    u8 chunky, GPU  (AIGS output)
    NvCVImage matteCpu{}; // A    u8 chunky, CPU  (downloaded)
    NvCVImage stage{};    // BGRA u8 chunky, GPU  (transfer staging; matches the CPU src)
    int  w = 0, h = 0;
    bool loaded = false;
```

- [ ] **Step 2: Implement (re)allocation of the images for a given size**

```cpp
namespace {
// (Re)allocates the GPU/CPU images for a w*h frame and loads the model. Returns
// NVCV_SUCCESS on success. Called when size changes or on first frame.
NvCV_Status EnsureImages(AigsImpl* impl, int w, int h) {
    if (impl->loaded && impl->w == w && impl->h == h) return NVCV_SUCCESS;

    NvCVImage_Dealloc(&impl->srcGpu);
    NvCVImage_Dealloc(&impl->matteGpu);
    NvCVImage_Dealloc(&impl->matteCpu);
    NvCVImage_Dealloc(&impl->stage);

    NvCV_Status s;
    s = NvCVImage_Alloc(&impl->srcGpu,   w, h, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->matteGpu, w, h, NVCV_A,   NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->matteCpu, w, h, NVCV_A,   NVCV_U8, NVCV_CHUNKY, NVCV_CPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->stage,    w, h, NVCV_BGRA,NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;

    NvVFX_SetImage(impl->effect, NVVFX_INPUT_IMAGE,  &impl->srcGpu);
    NvVFX_SetImage(impl->effect, NVVFX_OUTPUT_IMAGE, &impl->matteGpu);
    s = NvVFX_Load(impl->effect); // builds/loads the engine for this input size
    if (s != NVCV_SUCCESS) return s;

    impl->w = w; impl->h = h; impl->loaded = true;
    return NVCV_SUCCESS;
}
} // namespace
```

- [ ] **Step 3: Implement the swappable seam functions + `ProcessFrame`**

```cpp
namespace {
// SEAM 1 (upload): CPU BGRA -> GPU BGR, via a GPU staging buffer that matches the CPU src.
NvCV_Status Upload(AigsImpl* impl, uint8_t* bgra, int w, int h) {
    NvCVImage src{};
    NvCVImage_Init(&src, w, h, w * 4, bgra, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
    return NvCVImage_Transfer(&src, &impl->srcGpu, 1.0f, impl->stream, &impl->stage);
}
// SEAM 2 (download): GPU matte -> CPU matte (same format; tmp not needed).
NvCV_Status Download(AigsImpl* impl) {
    return NvCVImage_Transfer(&impl->matteGpu, &impl->matteCpu, 1.0f, impl->stream, nullptr);
}
// SEAM 3 (composite): apply matte to the BGRA buffer in place, premultiplied.
void Composite(AigsImpl* impl, uint8_t* bgra, int w, int h) {
    const uint8_t* m = static_cast<const uint8_t*>(impl->matteCpu.pixels);
    const int mpitch = impl->matteCpu.pitch; // bytes per matte row (>= w)
    for (int y = 0; y < h; ++y) {
        const uint8_t* mrow = m + static_cast<size_t>(mpitch) * y;
        uint8_t* prow = bgra + static_cast<size_t>(w) * 4 * y;
        for (int x = 0; x < w; ++x) {
            const unsigned a = mrow[x];
            uint8_t* px = prow + x * 4;
            px[0] = static_cast<uint8_t>((px[0] * a) / 255); // B
            px[1] = static_cast<uint8_t>((px[1] * a) / 255); // G
            px[2] = static_cast<uint8_t>((px[2] * a) / 255); // R
            px[3] = static_cast<uint8_t>(a);                 // A = matte
        }
    }
}
} // namespace

bool Aigs::ProcessFrame(uint8_t* bgra, int w, int h) {
    auto* impl = static_cast<AigsImpl*>(impl_);
    if (!impl || !impl->effect || !bgra || w <= 0 || h <= 0) return false;

    if (EnsureImages(impl, w, h) != NVCV_SUCCESS) { lastError_ = "EnsureImages/Load failed"; ready_ = false; return false; }
    if (Upload(impl, bgra, w, h) != NVCV_SUCCESS) { lastError_ = "Upload (Transfer) failed"; return false; }
    if (NvVFX_Run(impl->effect, 0) != NVCV_SUCCESS) { lastError_ = "NvVFX_Run failed"; return false; }
    if (Download(impl) != NVCV_SUCCESS) { lastError_ = "Download (Transfer) failed"; return false; }
    if (NvVFX_CudaStreamSynchronize(impl->stream) != NVCV_SUCCESS) { lastError_ = "stream sync failed"; return false; }

    Composite(impl, bgra, w, h);
    return true;
}
```

> The matte pitch may exceed `w` (alignment); `Composite` honors `matteCpu.pitch`. The BGRA buffer is tightly packed (`w*4`) per the existing capture contract.

- [ ] **Step 4: In `aigs_smoke.cpp`, add a synthetic-frame matte check**

After `Start()` succeeds, feed a frame and assert the matte is non-degenerate (contains both bright and dark regions when fed a real camera frame; for a synthetic flat frame just assert `ProcessFrame` returns true and alpha is written). Append:
```cpp
    // Minimal: a flat gray frame just exercises the pipeline end-to-end.
    int W = 640, H = 480;
    std::string buf(static_cast<size_t>(W) * H * 4, (char)128);
    bool ran = a.ProcessFrame(reinterpret_cast<uint8_t*>(&buf[0]), W, H);
    std::printf("ProcessFrame: %s (%s)\n", ran ? "OK" : "FAIL", a.LastError().c_str());
```
Expected output line: `ProcessFrame: OK ()`.

- [ ] **Step 5: Build the shim (SDK config), verify 0 warnings, run the smoke harness**

Build per Task 2 Step 4. Rebuild + run `aigs_smoke.exe`. Expected:
```
Probe: AVAILABLE (GreenScreen available)
Start: OK ()
ProcessFrame: OK ()
```

- [ ] **Step 6: Commit**

```bash
git add native/shim/aigs.cpp native/shim/smoke/aigs_smoke.cpp
git commit -m "feat(m3): AIGS per-frame upload/run/download/composite (premultiplied matte)"
```

---

### Task 4: `cos_query_capabilities` export + `CosCaps` struct

Add the native side of the real availability probe.

**Files:**
- Modify: `native/shim/shim.h`
- Modify: `native/shim/shim.cpp`

**Interfaces:**
- Produces: `CosCaps` struct + `int cos_query_capabilities(CosCaps* out)`. Consumed by `PInvokeShim` (Task 6).

- [ ] **Step 1: Add `CosCaps` + the export to `shim.h`**

After the `CosParams` struct:
```cpp
typedef struct {
    int  green_screen_available; // 1 if Maxine GreenScreen can run, else 0
    char detail[256];            // human-readable status/error (UTF-8, NUL-terminated)
} CosCaps;
```
With the other declarations:
```cpp
// Probes whether AI Green Screen is available (SDK loads + effect creates + model loads).
// Fills *out. Returns 1 if available, 0 otherwise. Safe to call before cos_start.
COS_API int cos_query_capabilities(CosCaps* out);
```

- [ ] **Step 2: Implement it in `shim.cpp`**

Add `#include "aigs.h"` near the top, then:
```cpp
COS_API int cos_query_capabilities(CosCaps* out) {
    if (!out) return 0;
    std::memset(out, 0, sizeof(*out));
    std::string detail;
    bool ok = Aigs::Probe(detail);
    out->green_screen_available = ok ? 1 : 0;
    // Copy detail into the fixed slot (truncate to 255 + NUL).
    size_t n = detail.size() < 255 ? detail.size() : 255;
    std::memcpy(out->detail, detail.data(), n);
    out->detail[n] = '\0';
    return ok ? 1 : 0;
}
```

- [ ] **Step 3: Build the shim (both configs) and verify 0 warnings**

Build the SDK config (Task 2 Step 4) AND the stub config (`/p:CosVfxSdkDir=`). Both: `0 Warning(s) 0 Error(s)`. In the stub config, `Aigs::Probe` returns false with detail "Maxine SDK not built in".

- [ ] **Step 4: Verify the new export**

Run the `dumpbin /exports` command from Task 1 Step 6. Expected: now **9** exports, including `cos_query_capabilities`.

- [ ] **Step 5: Commit**

```bash
git add native/shim/shim.h native/shim/shim.cpp
git commit -m "feat(m3): cos_query_capabilities export + CosCaps struct"
```

---

### Task 5: Wire AIGS into the capture worker + status

Run AIGS in the worker loop when green screen is enabled, own its lifecycle on the worker thread, and report `green_screen_active` / `error`.

**Files:**
- Modify: `native/shim/capture.h`
- Modify: `native/shim/capture.cpp`
- Modify: `native/shim/shim.cpp`

**Interfaces:**
- Consumes: `Aigs` (Tasks 2–3), `CosStatus` fields (`green_screen_active`, `error`).
- Produces: green-screen-enabled wiring through `Capture`; status reflects the live effect.

- [ ] **Step 1: Add an enabled flag + status accessors to `capture.h`**

Add to the `Capture` class public interface:
```cpp
    // Toggles AIGS for subsequent frames. Thread-safe; the worker owns the Aigs object.
    void SetGreenScreen(bool enabled);
    // Snapshot for status polling. Thread-safe.
    bool GreenScreenActive() const;       // true only while AIGS is transforming frames
    std::string GreenScreenError() const; // empty when none
```

- [ ] **Step 2: Add the worker-side state in `capture.cpp`**

Add to `CaptureState` (the module-level singleton):
```cpp
    std::atomic<bool>     greenScreenEnabled{false}; // set by UI thread, read by worker
    std::atomic<bool>     greenScreenActive{false};  // set by worker
    std::mutex            gsErrMtx;
    std::string           gsError;                   // guarded by gsErrMtx
```
Add `#include "aigs.h"` to `capture.cpp`.

- [ ] **Step 3: Run AIGS inside `WorkerLoop`**

In `WorkerLoop`, create an `Aigs` on the worker thread (so the CUDA context/stream has the right thread affinity), start it lazily when enabled, and process frames after `CopyFrame` fills `scratch`. Replace the existing `if (CopyFrame(...))` block body with:

```cpp
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
            std::this_thread::yield();
        }
```

Declare `Aigs aigs;` as a local at the top of `WorkerLoop` (after `CoInitializeEx`), and call `aigs.Stop();` before the loop's `SafeRelease(reader)` teardown. This keeps the entire `Aigs` lifetime on the worker thread.

- [ ] **Step 4: Implement the new `Capture` members in `capture.cpp`**

```cpp
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
```

On `Stop()` (after `StopLocked()`), reset the active flag: `g_state.greenScreenActive.store(false, std::memory_order_release);`.

- [ ] **Step 5: Drive it from `shim.cpp`**

In `cos_set_params`, after storing params, push the flag down:
```cpp
    g_capture.SetGreenScreen(p->green_screen_enabled != 0);
```
Rewrite `cos_get_status` to report the live effect:
```cpp
COS_API void cos_get_status(CosStatus* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->running = g_running ? 1 : 0;
    out->fps = g_running ? 30.0 : 0.0; // still a stub count (documented)
    out->green_screen_active = g_capture.GreenScreenActive() ? 1 : 0;
    std::string err = g_capture.GreenScreenError();
    if (!err.empty()) {
        size_t n = err.size() < 255 ? err.size() : 255;
        std::memcpy(out->error, err.data(), n);
        out->error[n] = '\0';
    }
}
```

- [ ] **Step 6: Build the shim (SDK config) — verify 0 warnings**

Build per Task 2 Step 4. Expected `0 Warning(s) 0 Error(s)`.

- [ ] **Step 7: Manual runtime check (target machine)**

Build the App (Global Constraints order) and run it. With a camera started and green screen toggled ON, instrument or observe that frames carry varying alpha (subject opaque, background transparent). This is exercised fully in Task 8; here just confirm no crash and `green_screen_active` flips to 1 in the status line.

- [ ] **Step 8: Commit**

```bash
git add native/shim/capture.h native/shim/capture.cpp native/shim/shim.cpp
git commit -m "feat(m3): run AIGS in capture worker, drive enable + status"
```

---

### Task 6: Managed capability contract + P/Invoke

Add the managed half of the probe: `ShimCapabilities`, `INativeShim.QueryCapabilities`, the `FakeShim` and `PInvokeShim` implementations. Pure Core is TDD; the P/Invoke marshalling mirrors `CosCaps`.

**Files:**
- Create: `src/CameraOnScreen.Core/Shim/ShimCapabilities.cs`
- Modify: `src/CameraOnScreen.Core/Shim/INativeShim.cs`
- Modify: `src/CameraOnScreen.Core/Shim/FakeShim.cs`
- Modify: `src/CameraOnScreen.App/Native/PInvokeShim.cs`
- Modify: `tests/CameraOnScreen.Core.Tests/FakeShimTests.cs` (or the existing shim test file)

**Interfaces:**
- Consumes: `cos_query_capabilities` / `CosCaps` (Task 4).
- Produces: `ShimCapabilities QueryCapabilities()` on `INativeShim`. Consumed by `Orchestrator` (Task 7).

> Confirm the existing namespace and folder (`Shim/` vs another) by opening `INativeShim.cs` first; match it. Paths below assume the existing `CameraOnScreen.Core.Shim` namespace — adjust if different.

- [ ] **Step 1: Write the failing test**

In the Core test project (e.g. `FakeShimTests.cs`):
```csharp
[Fact]
public void FakeShim_QueryCapabilities_ReportsConfiguredValue()
{
    var shim = new FakeShim { GreenScreenAvailable = true };
    var caps = shim.QueryCapabilities();
    Assert.True(caps.GreenScreenAvailable);
    Assert.False(string.IsNullOrEmpty(caps.Detail));
}

[Fact]
public void FakeShim_QueryCapabilities_DefaultsUnavailable()
{
    var caps = new FakeShim().QueryCapabilities();
    Assert.False(caps.GreenScreenAvailable);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~QueryCapabilities"`
Expected: FAIL — `ShimCapabilities` / `QueryCapabilities` / `GreenScreenAvailable` not defined.

- [ ] **Step 3: Create `ShimCapabilities.cs`**

```csharp
namespace CameraOnScreen.Core.Shim;

/// <summary>Result of probing the native shim for effect availability.</summary>
public sealed record ShimCapabilities(bool GreenScreenAvailable, string Detail);
```

- [ ] **Step 4: Add the method to `INativeShim.cs`**

```csharp
    /// <summary>Probes whether AI Green Screen can run (SDK present + model loads).</summary>
    ShimCapabilities QueryCapabilities();
```

- [ ] **Step 5: Implement in `FakeShim.cs`**

Add a settable flag and the method:
```csharp
    public bool GreenScreenAvailable { get; set; }

    public ShimCapabilities QueryCapabilities() =>
        new(GreenScreenAvailable,
            GreenScreenAvailable ? "fake: available" : "fake: unavailable");
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~QueryCapabilities"`
Expected: PASS (2 tests).

- [ ] **Step 7: Implement `PInvokeShim.QueryCapabilities` + `CosCaps` mirror**

In `PInvokeShim.cs`, add the struct mirror (byte-parity with Task 4: `int` + 256-byte char buffer) and the call:
```csharp
[StructLayout(LayoutKind.Sequential)]
private struct CosCaps
{
    public int GreenScreenAvailable;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)]
    public byte[] Detail;
}

[DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
private static extern int cos_query_capabilities(ref CosCaps caps);

public ShimCapabilities QueryCapabilities()
{
    var caps = new CosCaps { Detail = new byte[256] };
    int ok = cos_query_capabilities(ref caps);
    string detail = ReadUtf8(caps.Detail); // reuse the existing UTF-8 helper (NUL-terminated)
    return new ShimCapabilities(ok != 0, detail);
}
```
> Confirm `DllName`, `CallingConvention`, and the exact existing `ReadUtf8` signature in `PInvokeShim.cs`; if `ReadUtf8` takes a different shape (e.g. an offset/stride for the 128-byte enumeration), add a small `byte[]`→string overload that stops at the first NUL. Do not change the existing 128-byte enumeration code.

- [ ] **Step 8: Build the App to verify the P/Invoke compiles, 0 warnings**

Run: `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild`
Expected: `Build succeeded. 0 Warning(s)`.

- [ ] **Step 9: Commit**

```bash
git add src/CameraOnScreen.Core/Shim/ShimCapabilities.cs src/CameraOnScreen.Core/Shim/INativeShim.cs src/CameraOnScreen.Core/Shim/FakeShim.cs src/CameraOnScreen.App/Native/PInvokeShim.cs tests/CameraOnScreen.Core.Tests/FakeShimTests.cs
git commit -m "feat(m3): managed capability contract + cos_query_capabilities P/Invoke"
```

---

### Task 7: Orchestrator effect gate → QueryCapabilities

Switch the effect gate from the RTX-substring `GpuTierDetector` to the real probe. Keep `GpuTierDetector` for the GPU-name display only.

**Files:**
- Modify: `src/CameraOnScreen.Core/Orchestrator.cs`
- Modify: `tests/CameraOnScreen.Core.Tests/OrchestratorTests.cs`

**Interfaces:**
- Consumes: `INativeShim.QueryCapabilities()` (Task 6).
- Produces: the orchestrator's effect-availability decision now comes from the shim probe.

> Open `Orchestrator.cs` and `OrchestratorTests.cs` first to confirm how effects are currently gated (the property name exposed to the VM, e.g. `EffectsAvailable`, and how `GpuTierDetector`/the tier is injected). The steps below adapt to that; preserve the existing public property name the VM binds to.

- [ ] **Step 1: Write the failing test**

```csharp
[Fact]
public void Effects_Disabled_When_Shim_Reports_Unavailable()
{
    var shim = new FakeShim { GreenScreenAvailable = false };
    var orch = NewOrchestrator(shim); // existing test helper; pass the fake shim
    orch.Initialize();                // or the existing init entry point
    Assert.False(orch.EffectsAvailable);
}

[Fact]
public void Effects_Enabled_When_Shim_Reports_Available()
{
    var shim = new FakeShim { GreenScreenAvailable = true };
    var orch = NewOrchestrator(shim);
    orch.Initialize();
    Assert.True(orch.EffectsAvailable);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~Effects_"`
Expected: FAIL — effects still gated on the RTX tier, not the shim probe (one or both asserts wrong).

- [ ] **Step 3: Change the gate in `Orchestrator.cs`**

Replace the RTX-tier gate with the probe. Keep the tier for display:
```csharp
// Effects are available iff the native shim can actually run Green Screen.
var caps = _shim.QueryCapabilities();
EffectsAvailable = caps.GreenScreenAvailable;
CapabilityDetail = caps.Detail;          // surfaced to the VM/status (add if not present)
// GpuTierDetector result is retained only for the GPU-name display string:
GpuName = _gpuTier.Describe();           // keep whatever the existing display call is
```
Remove any code path where a non-RTX tier alone disables effects; the probe is now authoritative. If the VM still needs a "requires RTX GPU"-style note, derive it from `CapabilityDetail`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~Effects_"`
Expected: PASS.

- [ ] **Step 5: Run the full Core suite — no regressions**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
Expected: all green (update any existing test that assumed RTX-tier gating to set `FakeShim.GreenScreenAvailable` instead).

- [ ] **Step 6: Commit**

```bash
git add src/CameraOnScreen.Core/Orchestrator.cs tests/CameraOnScreen.Core.Tests/OrchestratorTests.cs
git commit -m "feat(m3): gate effects on cos_query_capabilities, keep GpuTier for display"
```

---

### Task 8: Full integration build + manual verification

Build the whole app against the SDK, confirm the live green-screen pipeline, and record the manual recorder gate.

**Files:**
- Modify: `docs/superpowers/verification/2026-06-20-recorder-capture.md`

**Interfaces:**
- Consumes: everything from Tasks 1–7.
- Produces: a recorded M3 verification result.

- [ ] **Step 1: Clean build, full chain**

Run in order (PowerShell, `COS_VFX_SDK_DIR` set):
```
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj
```
Expected: shim `0 Warning(s)`, App `0 Warning(s)`, all Core tests pass.

- [ ] **Step 2: Confirm the runtime DLL chain is reachable**

The shim loads `NVVideoEffects.dll` + CUDA/TensorRT from `%COS_VFX_SDK_DIR%\bin` via the proxy globals. Confirm `COS_VFX_SDK_DIR` is set in the environment the app runs under (launch the exe from the same PowerShell session). If green screen reports unavailable at runtime, check the status line `error`/`detail` text.

- [ ] **Step 3: Manual visual verification (human at screen)**

Launch `src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.App.exe`. Pick the camera, Start. Confirm:
- Toggle AI Green Screen ON → the background becomes **transparent** in the overlay (subject only); status shows `green_screen_active`.
- Toggle OFF → the overlay returns to the opaque rectangle (passthrough).
- Drag / resize / lock / click-through still behave (no M2 regression).

- [ ] **Step 4: Recorder capture gate**

With green screen ON, capture with a DWM-based recorder (OBS Display Capture, or Xbox Game Bar `Win+G`) for ~5 s, move the overlay mid-recording, play back. Confirm the subject appears on a transparent background over the desktop, no chrome/handles, no post-edit. (GDI screenshots will still show black — by design.)

- [ ] **Step 5: Record the result**

Append an M3 section to `docs/superpowers/verification/2026-06-20-recorder-capture.md` with: build versions, that green screen toggles matte/opaque, the recorder + capture mode that worked, and screenshots taken from the recorder's output. Note the matte quality and any flicker.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/verification/2026-06-20-recorder-capture.md
git commit -m "test(m3): verify AI Green Screen live + recorder capture"
```

---

## Self-Review

**Spec coverage (M3 design spec):**
- Maxine VFX SDK integration in the shim (link, init, model load, error) → Tasks 1, 2. ✓
- GreenScreen effect run per frame on GPU → Tasks 2, 3. ✓
- CPU-copy interop (CPU→GPU→CPU), no D3D11 interop → Task 3 (swappable seam). ✓
- Per-pixel alpha (matte) through the existing CPU buffer to the overlay, premultiplied → Task 3 Composite + Task 5 (unchanged `cos_get_frame` path). ✓
- Real availability probe replacing RTX-substring → Tasks 4 (native), 6 (managed), 7 (gate). ✓
- Green screen on/off; strength unused → Tasks 5, 6 (only `green_screen_enabled` consumed). ✓
- `cos_query_capabilities` + `CosCaps` byte-parity → Tasks 4, 6. ✓
- `green_screen_active` / `error` status → Task 5. ✓
- `COS_VFX_SDK_DIR` path; no SDK in git; no absolute paths → Tasks 1, 2 (env-var resolution, MSBuild property). ✓
- Builds with no SDK (CI-safe stub); Core tests pass without SDK → Tasks 1 (stub), 6 (FakeShim). ✓
- Crash safety / degrade to passthrough → Tasks 3, 5 (ProcessFrame returns false → opaque), 2 (no-throw). ✓
- Testing: Core unit (6,7), native smoke (2,3), manual gate (8). ✓
- Out of scope (Eye Contact, GPU zero-copy, strength slider, bundling/license) → not in any task, by design. ✓

**Placeholder scan:** No "TBD"/"TODO"/"handle edge cases". Two deliberately deferred specifics carry explicit verification steps, not placeholders: the exact `NVVFX_MODE` value (Task 2 Step 2, "confirm against docs" — a smoke-verified default of 1, not an empty blank) and the exact existing managed member names (flagged "confirm by opening the file" because they are pre-existing and must be matched, not invented).

**Type consistency:** `Aigs` methods (`Probe`/`Start`/`Stop`/`ProcessFrame`/`IsReady`/`LastError`) are identical across Tasks 1–5. `CosCaps` is `int green_screen_available` + `char detail[256]` in both Task 4 (C) and Task 6 (managed `int` + `byte[256]`). `ShimCapabilities(bool GreenScreenAvailable, string Detail)` is consistent across Tasks 6–7. `QueryCapabilities()` returns `ShimCapabilities` everywhere. `SetGreenScreen`/`GreenScreenActive`/`GreenScreenError` match between `capture.h` (Task 1/5) and `capture.cpp` (Task 5) and their callers in `shim.cpp` (Task 5).

---

## Execution Handoff

Two execution options:

1. **Subagent-Driven (recommended)** — fresh subagent per task, task review (spec + quality) between tasks, broad review at the end. Same flow as M1/M2.
2. **Inline Execution** — execute in this session with checkpoints.

Note: Tasks 2, 3, 5, 8 require the **target machine with the SDK + a webcam** (runtime smoke + manual gate); Tasks 1, 4, 6, 7 are SDK-optional (build/CI-safe). Sequence is linear (each builds on the prior).
