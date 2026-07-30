# Hardware-Agnostic Green Screen (ONNX Runtime) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a second, user-selectable green-screen engine (`SegOnnx`) that runs the MediaPipe selfie-segmentation model in ONNX Runtime on the CPU — works on AMD/Intel/non-RTX/no-GPU boxes, Linux + Windows — alongside the untouched NVIDIA Maxine engine.

**Architecture:** The capture worker owns both engines (`Aigs` + new `SegOnnx`, identical method contract) and dispatches per a new `green_screen_backend` param (0 Auto / 1 Maxine / 2 ONNX). ONNX Runtime is dlopen'd at runtime (no build-time SDK, no `COS_HAS_*` flag — always compiled, probe gates availability). Shared matte post-processing (dilate→feather→premultiply) is extracted from `aigs.cpp` into `matte_ops.{h,cpp}` so both engines feed the same slider-driven chain.

**Tech Stack:** C++17 (CMake Linux / vcxproj Windows), ONNX Runtime 1.28.0 C API (dlopen, `OrtGetApiBase`), MediaPipe selfie-segmentation ONNX (PINTO zoo #109), .NET 8 + Avalonia, xUnit.

**Spec:** `docs/superpowers/specs/2026-07-30-hw-agnostic-green-screen-design.md`

## Global Constraints

- Builds and tests pristine: **0 warnings** (`-Wall -Wextra -Werror` native; `/p:TreatWarningsAsErrors=true` dotnet).
- C ABI struct parity is load-bearing: `shim.h` structs and BOTH `PInvokeShim.cs` mirrors (Avalonia + WinUI) change **in the same commit**; new fields are **appended** so existing offsets never move.
- The Maxine path (`aigs.cpp` behavior, probe, runtime resolution) must be behaviorally unchanged.
- Maxine effects + SegOnnx run **only on the capture worker thread**; UI crosses via atomics; errors via the `gsErrMtx` leaf lock. Never nest leaf locks under `g_state.mtx`/`g_lifecycleMtx`.
- Windows-only code (`capture.cpp`, `shim.vcxproj`, WinUI `PInvokeShim.cs`) cannot be compiled on this box — keep those edits exact mirrors of the Linux versions; they get compile-verified when #38 re-homes Windows CI.
- Pinned artifacts (do not re-resolve):
  - ORT: `https://github.com/microsoft/onnxruntime/releases/download/v1.28.0/onnxruntime-linux-x64-1.28.0.tgz`, sha256 `a3e1b79d7bb1bf09696ce675f49e4064e6c81f6202b8225624fff0e93f8d6407`, lib at `onnxruntime-linux-x64-1.28.0/lib/libonnxruntime.so.1.28.0` (MIT).
  - Model: `https://s3.ap-northeast-2.wasabisys.com/pinto-model-zoo/109_Selfie_Segmentation/resources.tar.gz`, file `saved_model_tflite_tfjs_tftrt_onnx_coreml/model_float32.onnx`, sha256 `de212dabbc6266f0047711d1dfae80900f7b596b9ed5f7665f3d1cf68c5443ee`, 447,658 bytes (Apache-2.0). Input `input_1:0` `[1,256,256,3]` NHWC float32 RGB 0..1; output `activation_10` `[1,256,256,1]` sigmoid confidence; opset 11.
  - Vendored header `ORT_API_VERSION` = 28.
- Dev-box conveniences: user-local cmake/dotnet (no sudo). A scratch copy of both artifacts already sits in this session's scratchpad; re-download is fine too.

---

### Task 0: Branch

**Files:** none

- [ ] **Step 1: Create the feature branch**

```bash
git checkout -b feat/onnx-green-screen
```

---

### Task 1: Extract shared matte ops from aigs.cpp

**Files:**
- Create: `native/shim/matte_ops.h`, `native/shim/matte_ops.cpp`
- Modify: `native/shim/aigs.cpp` (delete moved functions, call `matte::`), `native/shim/CMakeLists.txt:20-29` (add source), `native/shim/shim.vcxproj` (add ClCompile/ClInclude)

**Interfaces:**
- Produces: `namespace matte` — `int RadiusFromAmount(double amount, int maxRadius)`, `void Dilate(uint8_t* work, uint8_t* tmp, int w, int h, int r)`, `void Feather(uint8_t* work, uint8_t* tmp, int w, int h, int r)`, `void CompositePremultiplied(uint8_t* bgra, const uint8_t* matteBuf, int w, int h)`, `constexpr int kMaxDilateRadius = 16`, `constexpr int kMaxBlurRadius = 16`. Task 3's `SegOnnx` consumes these.

- [ ] **Step 1: Write `native/shim/matte_ops.h`**

```cpp
#pragma once
#include <cstdint>

// Matte post-processing shared by the green-screen engines (Maxine AIGS and ONNX —
// issue #24). All buffers are tightly packed (pitch == w). Dilate/Feather run in
// place on 'work', using 'tmp' as scratch; both must be w*h bytes.
namespace matte {

// Max amount of dilate/blur at slider value 1.0, in pixels.
constexpr int kMaxDilateRadius = 16;
constexpr int kMaxBlurRadius   = 16;

// Slider amount 0..1 -> pixel radius 0..maxRadius.
int RadiusFromAmount(double amount, int maxRadius);

// Separable morphological dilate (max filter).
void Dilate(uint8_t* work, uint8_t* tmp, int w, int h, int r);

// Separable box blur.
void Feather(uint8_t* work, uint8_t* tmp, int w, int h, int r);

// Apply the packed matte to BGRA in place: A = matte, RGB premultiplied by matte/255.
void CompositePremultiplied(uint8_t* bgra, const uint8_t* matteBuf, int w, int h);

} // namespace matte
```

- [ ] **Step 2: Write `native/shim/matte_ops.cpp`** — the bodies are moves of `aigs.cpp:152-239` (`RadiusFromAmount`, `DilateMatte`, `FeatherMatte`, `Composite`) with `AigsImpl*` replaced by raw buffer parameters:

```cpp
#include "matte_ops.h"
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace matte {

int RadiusFromAmount(double amount, int maxRadius) {
    if (amount <= 0.0) return 0;
    if (amount >= 1.0) return maxRadius;
    return static_cast<int>(std::lround(amount * maxRadius));
}

void Dilate(uint8_t* work, uint8_t* tmp, int w, int h, int r) {
    if (r <= 0) return;
    for (int y = 0; y < h; ++y) {       // horizontal
        const uint8_t* srow = work + static_cast<size_t>(w) * y;
        uint8_t* trow = tmp + static_cast<size_t>(w) * y;
        for (int x = 0; x < w; ++x) {
            uint8_t mx = 0;
            const int x0 = std::max(0, x - r), x1 = std::min(w - 1, x + r);
            for (int k = x0; k <= x1; ++k) mx = std::max(mx, srow[k]);
            trow[x] = mx;
        }
    }
    for (int x = 0; x < w; ++x) {       // vertical
        for (int y = 0; y < h; ++y) {
            uint8_t mx = 0;
            const int y0 = std::max(0, y - r), y1 = std::min(h - 1, y + r);
            for (int k = y0; k <= y1; ++k) mx = std::max(mx, tmp[static_cast<size_t>(w) * k + x]);
            work[static_cast<size_t>(w) * y + x] = mx;
        }
    }
}

void Feather(uint8_t* work, uint8_t* tmp, int w, int h, int r) {
    if (r <= 0) return;
    const int win = 2 * r + 1;
    for (int y = 0; y < h; ++y) {       // horizontal
        const uint8_t* srow = work + static_cast<size_t>(w) * y;
        uint8_t* trow = tmp + static_cast<size_t>(w) * y;
        for (int x = 0; x < w; ++x) {
            int sum = 0;
            const int x0 = std::max(0, x - r), x1 = std::min(w - 1, x + r);
            for (int k = x0; k <= x1; ++k) sum += srow[k];
            trow[x] = static_cast<uint8_t>(sum / win);
        }
    }
    for (int x = 0; x < w; ++x) {       // vertical
        for (int y = 0; y < h; ++y) {
            int sum = 0;
            const int y0 = std::max(0, y - r), y1 = std::min(h - 1, y + r);
            for (int k = y0; k <= y1; ++k) sum += tmp[static_cast<size_t>(w) * k + x];
            work[static_cast<size_t>(w) * y + x] = static_cast<uint8_t>(sum / win);
        }
    }
}

void CompositePremultiplied(uint8_t* bgra, const uint8_t* matteBuf, int w, int h) {
    for (int y = 0; y < h; ++y) {
        const uint8_t* mrow = matteBuf + static_cast<size_t>(w) * y;
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

} // namespace matte
```

- [ ] **Step 3: Update `aigs.cpp`** — add `#include "matte_ops.h"` (inside the `COS_HAS_MAXINE` block, after `#include "vfx_paths.h"`); delete `kMaxDilateRadius`/`kMaxBlurRadius`/`RadiusFromAmount`/`DilateMatte`/`FeatherMatte`/`Composite` from the anonymous namespace (keep `EnsureImages`, `Upload`, `Download`, `PackMatte`); rewrite the tail of `Aigs::ProcessFrame` to:

```cpp
    PackMatte(impl, w, h);
    matte::Dilate(impl->matteWork.data(), impl->matteTmp.data(), w, h,
                  matte::RadiusFromAmount(expand, matte::kMaxDilateRadius));
    matte::Feather(impl->matteWork.data(), impl->matteTmp.data(), w, h,
                   matte::RadiusFromAmount(feather, matte::kMaxBlurRadius));
    matte::CompositePremultiplied(bgra, impl->matteWork.data(), w, h);
    return true;
```

- [ ] **Step 4: Add `matte_ops.cpp` to both builds** — CMake: append `matte_ops.cpp` to the `add_library(shim SHARED …)` list. vcxproj: add `<ClCompile Include="matte_ops.cpp" />` next to the existing `<ClCompile Include="aigs.cpp" />` and `<ClInclude Include="matte_ops.h" />` next to `aigs.h` (text edit only — not buildable here).

- [ ] **Step 5: Verify the Linux build (stub config — aigs.cpp's matte usage is inside `COS_HAS_MAXINE`, so also do an SDK-config compile check)**

```bash
cmake -S native/shim -B native/shim/build && cmake --build native/shim/build
COS_VFX_SDK_DIR=~/dev/VideoFX-linux/VideoFX COS_AR_SDK_DIR=~/dev/ARSDK-linux/ARSDK \
  COS_FRUC_SDK_DIR=~/dev/Optical_Flow_SDK_5.0.7 \
  cmake -S native/shim -B native/shim/build-sdk && cmake --build native/shim/build-sdk
```
Expected: both builds succeed, 0 warnings.

- [ ] **Step 6: Commit**

```bash
git add native/shim/matte_ops.h native/shim/matte_ops.cpp native/shim/aigs.cpp \
        native/shim/CMakeLists.txt native/shim/shim.vcxproj
git commit -m "refactor(shim): extract shared matte ops from aigs (#24)"
```

---

### Task 2: C ABI + C# contract extension (backend param, ONNX capability gate)

**Files:**
- Modify: `native/shim/shim.h:23-47` (CosParams/CosCaps), `src/CameraOnScreen.Core/Native/Contracts.cs`, `src/CameraOnScreen.Core/Native/FakeShim.cs`, `src/CameraOnScreen.App.Avalonia/Native/PInvokeShim.cs`, `src/CameraOnScreen.App/Native/PInvokeShim.cs`
- Test: `tests/CameraOnScreen.Core.Tests/Native/` (existing dir; extend or add `ContractsTests.cs`)

**Interfaces:**
- Produces (C): `CosParams.green_screen_backend` (int, appended: 0 Auto, 1 Maxine, 2 ONNX CPU); `CosCaps.gs_onnx_available` (int) + `CosCaps.gs_onnx_detail[256]` (appended).
- Produces (C#): `ShimParams.GreenScreenBackend` (int, default 0, appended positional param); `ShimCapabilities.GreenScreenOnnxAvailable` (bool, default false) + `ShimCapabilities.GreenScreenOnnxDetail` (string, default "") appended; `FakeShim.GreenScreenOnnxAvailable { get; set; }`.

- [ ] **Step 1: Write the failing test** — in `tests/CameraOnScreen.Core.Tests/Native/` (new file `ContractsTests.cs` if none fits):

```csharp
using CameraOnScreen.Core.Native;
using Xunit;

namespace CameraOnScreen.Core.Tests.Native;

public class ContractsTests
{
    [Fact]
    public void ShimParams_CarriesGreenScreenBackend()
    {
        var p = new ShimParams(null, true, 0, 0, false, 0.5, 0.5, GreenScreenBackend: 2);
        Assert.Equal(2, p.GreenScreenBackend);
        // Default stays Auto so existing call sites keep their behavior.
        var d = new ShimParams(null, true, 0, 0, false, 0.5, 0.5);
        Assert.Equal(0, d.GreenScreenBackend);
    }

    [Fact]
    public void FakeShim_ReportsOnnxAvailability()
    {
        var shim = new FakeShim { GreenScreenOnnxAvailable = true };
        var caps = shim.QueryCapabilities();
        Assert.True(caps.GreenScreenOnnxAvailable);
        Assert.False(caps.GreenScreenAvailable);
    }
}
```

- [ ] **Step 2: Run to verify failure**

```bash
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~ContractsTests"
```
Expected: compile FAIL (`GreenScreenBackend`/`GreenScreenOnnxAvailable` not defined).

- [ ] **Step 3: Extend the C structs in `shim.h`** — append to `CosParams` (after `frame_interp_enabled`):

```c
    int    green_screen_backend;      // 0 Auto (Maxine, else ONNX), 1 Maxine, 2 ONNX CPU (#24)
```

Append to `CosCaps` (after `fi_detail`):

```c
    int  gs_onnx_available;      // 1 if the ONNX CPU green-screen engine can run (#24)
    char gs_onnx_detail[256];    // ONNX engine status/error (UTF-8, NUL-terminated)
```

- [ ] **Step 4: Extend `Contracts.cs`** — `ShimParams` gains a final positional param `int GreenScreenBackend = 0`; `ShimCapabilities` gains `bool GreenScreenOnnxAvailable = false, string GreenScreenOnnxDetail = ""` appended after `FrameInterpAvailable`.

- [ ] **Step 5: Extend `FakeShim.cs`** — add `public bool GreenScreenOnnxAvailable { get; set; }`; extend `QueryCapabilities()`:

```csharp
    public ShimCapabilities QueryCapabilities() =>
        new(GreenScreenAvailable,
            GreenScreenAvailable ? "fake: available" : "fake: unavailable",
            EyeContactAvailable,
            EyeContactAvailable ? "fake: ec available" : "fake: ec unavailable",
            SuperResAvailable,
            FrameInterpAvailable,
            GreenScreenOnnxAvailable,
            GreenScreenOnnxAvailable ? "fake: onnx available" : "fake: onnx unavailable");
```

- [ ] **Step 6: Extend BOTH `PInvokeShim.cs` mirrors (Avalonia and WinUI — identical edits, same commit)** — `CosParams` struct gains `public int green_screen_backend;` after `frame_interp_enabled`; `CosCaps` gains after `FiDetail`:

```csharp
        public int GsOnnxAvailable;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)] public byte[] GsOnnxDetail;
```

`SetParams` gains `green_screen_backend = p.GreenScreenBackend,`; `QueryCapabilities` initializes `GsOnnxDetail = new byte[256]` in the `CosCaps` ctor call and appends to the returned record:

```csharp
            caps.GsOnnxAvailable != 0, ReadUtf8(caps.GsOnnxDetail, 0, 256));
```

- [ ] **Step 7: Run tests + full suite**

```bash
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj
```
Expected: PASS, 0 warnings. (Native side not consuming the new fields yet — memset-zero in `cos_query_capabilities` keeps `gs_onnx_available` 0 until Task 4.)

- [ ] **Step 8: Commit**

```bash
git add native/shim/shim.h src/CameraOnScreen.Core/Native/Contracts.cs \
        src/CameraOnScreen.Core/Native/FakeShim.cs \
        src/CameraOnScreen.App.Avalonia/Native/PInvokeShim.cs \
        src/CameraOnScreen.App/Native/PInvokeShim.cs \
        tests/CameraOnScreen.Core.Tests/Native/ContractsTests.cs
git commit -m "feat(abi): green_screen_backend param + ONNX capability gate (#24)"
```

---

### Task 3: SegOnnx engine + vendored ORT headers + seg_probe smoke

**Files:**
- Create: `native/shim/seg_onnx.h`, `native/shim/seg_onnx.cpp`, `native/shim/third_party/onnxruntime/` (vendored headers + LICENSE), `native/shim/smoke/seg_probe.cpp`
- Modify: `native/shim/CMakeLists.txt` (shim sources + seg_probe target), `native/shim/shim.vcxproj` (ClCompile/ClInclude + AdditionalIncludeDirectories for `third_party`)

**Interfaces:**
- Consumes: `matte::` (Task 1), `ShimModuleDir()/DirExists()/EnvVar()` from `paths.h`.
- Produces: `class SegOnnx` — `static bool Probe(std::string& detail)`, `bool Start()`, `void Stop()`, `bool ProcessFrame(uint8_t* bgra, int width, int height, double expand, double feather)`, `bool IsReady() const`, `const std::string& LastError() const`. Task 4 consumes it in both capture backends and `shim.cpp`.

- [ ] **Step 1: Vendor the ORT C headers** (MIT — include the license):

```bash
mkdir -p native/shim/third_party/onnxruntime
S=/tmp/claude-1000/-home-ariff-officialdad-camera-on-screen/093d13a6-7168-47ba-9eb7-ca8567b7240b/scratchpad
# Re-download if the scratchpad copy is gone (pinned URL + sha in Global Constraints).
tar -xzf "$S/ort.tgz" -C /tmp onnxruntime-linux-x64-1.28.0/include onnxruntime-linux-x64-1.28.0/LICENSE
cp /tmp/onnxruntime-linux-x64-1.28.0/include/onnxruntime_c_api.h \
   /tmp/onnxruntime-linux-x64-1.28.0/include/onnxruntime_error_code.h \
   /tmp/onnxruntime-linux-x64-1.28.0/include/onnxruntime_ep_c_api.h \
   /tmp/onnxruntime-linux-x64-1.28.0/LICENSE \
   native/shim/third_party/onnxruntime/
```
If the shim compile in Step 6 demands another transitively-included ORT header, copy that too (bounded set, all MIT).

- [ ] **Step 2: Write `native/shim/seg_onnx.h`**

```cpp
#pragma once
#include <cstdint>
#include <string>

// Hardware-agnostic green-screen engine (issue #24): the MediaPipe selfie-segmentation
// model (256x256, Apache-2.0) run in ONNX Runtime on the CPU. Same contract as Aigs:
// processes a tightly-packed BGRA buffer in place — A = matte, RGB premultiplied.
// ONNX Runtime is dlopen'd at runtime from COS_SEG_RUNTIME_DIR or <shim>/onnx/
// (libonnxruntime.so.1 / onnxruntime.dll + selfie_segmentation.onnx). Absent runtime
// means Probe()/Start() fail with a detail string — no hard dependency, the shim
// still loads everywhere. All methods are no-throw; failure via IsReady()+LastError().
class SegOnnx {
public:
    SegOnnx();
    ~SegOnnx();

    // One-shot probe: can ORT load and a session be created on the model? Does not
    // retain the session. Not designed for concurrent calls. Fills 'detail'.
    static bool Probe(std::string& detail);

    // Load ORT + create the inference session. Call on the capture worker thread.
    bool Start();

    // Destroy the session. Call on the worker thread. The ORT library itself stays
    // loaded process-wide (one runtime per process, like the Maxine preload).
    void Stop();

    // Run segmentation on a tightly-packed BGRA buffer (width*height*4) in place:
    // A = matte, RGB premultiplied. expand/feather (0..1) dilate then blur the matte.
    // Returns true if applied; false leaves 'bgra' untouched.
    bool ProcessFrame(uint8_t* bgra, int width, int height, double expand, double feather);

    bool IsReady() const { return ready_; }
    const std::string& LastError() const { return lastError_; }

private:
    bool ready_ = false;
    std::string lastError_;
    void* impl_ = nullptr; // opaque; real fields live in seg_onnx.cpp
};
```

- [ ] **Step 3: Write `native/shim/seg_onnx.cpp`**

```cpp
#include "seg_onnx.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

#include "matte_ops.h"
#include "paths.h"
#include "third_party/onnxruntime/onnxruntime_c_api.h"

#ifdef _WIN32
  #define NOMINMAX
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace {

constexpr int kNetSize = 256; // model I/O is 256x256 (NHWC float, RGB 0..1 in, confidence out)
constexpr const char* kModelFile = "selfie_segmentation.onnx";

// ---- runtime resolution + dynamic loading -------------------------------------------

// COS_SEG_RUNTIME_DIR (dev override) -> <shim>/onnx (bundled tier). Same pattern as
// the Maxine resolvers in vfx_paths.cpp.
std::string ResolveRuntimeDir(std::string& err) {
    std::string dir = EnvVar("COS_SEG_RUNTIME_DIR");
    if (!dir.empty()) {
        if (DirExists(dir)) return dir;
        err = "COS_SEG_RUNTIME_DIR set but not a directory: " + dir;
        return std::string();
    }
    const std::string mod = ShimModuleDir();
    if (!mod.empty()) {
        dir = mod + "/onnx";
        if (DirExists(dir)) return dir;
    }
    err = "ONNX runtime not found (set COS_SEG_RUNTIME_DIR or ship <shim>/onnx)";
    return std::string();
}

void* LoadLib(const std::string& path) {
#ifdef _WIN32
    int wn = MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), nullptr, 0);
    if (wn <= 0) return nullptr;
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), &w[0], wn);
    return LoadLibraryW(w.c_str());
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* GetSym(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

// Process-wide ORT function table + the dir it was loaded from. Loaded once, never
// unloaded (in-process re-dlopen of ORT is unsupported; matches the Maxine preload).
const OrtApi* g_ort = nullptr;
std::string   g_runtimeDir;

const OrtApi* EnsureOrt(std::string& err) {
    if (g_ort) return g_ort;
    const std::string dir = ResolveRuntimeDir(err);
    if (dir.empty()) return nullptr;
#ifdef _WIN32
    void* lib = LoadLib(dir + "\\onnxruntime.dll");
#else
    void* lib = LoadLib(dir + "/libonnxruntime.so.1");
#endif
    if (!lib) { err = "failed to load ONNX Runtime from " + dir; return nullptr; }
    using GetBaseFn = const OrtApiBase*(ORT_API_CALL*)(void);
    auto getBase = reinterpret_cast<GetBaseFn>(GetSym(lib, "OrtGetApiBase"));
    if (!getBase) { err = "OrtGetApiBase export missing (not an ONNX Runtime library?)"; return nullptr; }
    const OrtApi* api = getBase()->GetApi(ORT_API_VERSION);
    if (!api) { err = "ONNX Runtime does not support C API version " + std::to_string(ORT_API_VERSION); return nullptr; }
    g_ort = api;
    g_runtimeDir = dir;
    return g_ort;
}

// OrtStatus -> bool + error string.
bool Check(const OrtApi* ort, OrtStatus* st, const char* what, std::string& err) {
    if (!st) return true;
    err = std::string(what) + ": " + ort->GetErrorMessage(st);
    ort->ReleaseStatus(st);
    return false;
}

// ---- session ------------------------------------------------------------------------

struct SegImpl {
    OrtEnv*            env = nullptr;
    OrtSessionOptions* so = nullptr;
    OrtSession*        session = nullptr;
    OrtMemoryInfo*     memInfo = nullptr;
    std::string inName, outName;
    std::vector<float>   input;              // 1*256*256*3 NHWC RGB
    std::vector<uint8_t> matteWork, matteTmp; // packed w*h
};

void DestroySession(const OrtApi* ort, SegImpl* impl) {
    if (!impl) return;
    if (impl->memInfo) ort->ReleaseMemoryInfo(impl->memInfo);
    if (impl->session) ort->ReleaseSession(impl->session);
    if (impl->so)      ort->ReleaseSessionOptions(impl->so);
    if (impl->env)     ort->ReleaseEnv(impl->env);
    delete impl;
}

// Create env/options/session + resolve tensor names. On failure fills err and
// returns nullptr (everything created so far is released).
SegImpl* CreateSession(const OrtApi* ort, std::string& err) {
    SegImpl* impl = new (std::nothrow) SegImpl();
    if (!impl) { err = "out of memory"; return nullptr; }

    if (!Check(ort, ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "cos-seg", &impl->env), "CreateEnv", err) ||
        !Check(ort, ort->CreateSessionOptions(&impl->so), "CreateSessionOptions", err) ||
        !Check(ort, ort->SetIntraOpNumThreads(impl->so, 2), "SetIntraOpNumThreads", err) ||
        !Check(ort, ort->SetSessionGraphOptimizationLevel(impl->so, ORT_ENABLE_ALL), "SetGraphOptimizationLevel", err)) {
        DestroySession(ort, impl);
        return nullptr;
    }

    const std::string modelPath = g_runtimeDir +
#ifdef _WIN32
        "\\" + kModelFile;
    int wn = MultiByteToWideChar(CP_UTF8, 0, modelPath.data(), (int)modelPath.size(), nullptr, 0);
    std::wstring wpath((size_t)(wn > 0 ? wn : 0), L'\0');
    if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, modelPath.data(), (int)modelPath.size(), &wpath[0], wn);
    OrtStatus* cs = ort->CreateSession(impl->env, wpath.c_str(), impl->so, &impl->session);
#else
        "/" + kModelFile;
    OrtStatus* cs = ort->CreateSession(impl->env, modelPath.c_str(), impl->so, &impl->session);
#endif
    if (!Check(ort, cs, "CreateSession (model missing/corrupt?)", err)) { DestroySession(ort, impl); return nullptr; }

    OrtAllocator* alloc = nullptr;
    char* name = nullptr;
    if (!Check(ort, ort->GetAllocatorWithDefaultOptions(&alloc), "GetAllocator", err) ||
        !Check(ort, ort->SessionGetInputName(impl->session, 0, alloc, &name), "GetInputName", err)) {
        DestroySession(ort, impl);
        return nullptr;
    }
    impl->inName = name;
    ort->AllocatorFree(alloc, name);
    name = nullptr;
    if (!Check(ort, ort->SessionGetOutputName(impl->session, 0, alloc, &name), "GetOutputName", err)) {
        DestroySession(ort, impl);
        return nullptr;
    }
    impl->outName = name;
    ort->AllocatorFree(alloc, name);

    if (!Check(ort, ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &impl->memInfo), "CreateCpuMemoryInfo", err)) {
        DestroySession(ort, impl);
        return nullptr;
    }
    impl->input.resize(static_cast<size_t>(kNetSize) * kNetSize * 3);
    return impl;
}

// ---- resampling ----------------------------------------------------------------------

// Bilinear BGRA(w,h) -> float RGB (kNetSize^2, NHWC, 0..1).
void DownscaleToNet(const uint8_t* bgra, int w, int h, float* dst) {
    const float sx = static_cast<float>(w) / kNetSize, sy = static_cast<float>(h) / kNetSize;
    for (int y = 0; y < kNetSize; ++y) {
        const float fy = std::max(0.0f, (y + 0.5f) * sy - 0.5f);
        const int y0 = std::min(static_cast<int>(fy), h - 1), y1 = std::min(y0 + 1, h - 1);
        const float wy = fy - static_cast<float>(y0);
        for (int x = 0; x < kNetSize; ++x) {
            const float fx = std::max(0.0f, (x + 0.5f) * sx - 0.5f);
            const int x0 = std::min(static_cast<int>(fx), w - 1), x1 = std::min(x0 + 1, w - 1);
            const float wx = fx - static_cast<float>(x0);
            float* out = dst + (static_cast<size_t>(y) * kNetSize + x) * 3;
            for (int c = 0; c < 3; ++c) {
                const int sc = 2 - c; // dst RGB <- src BGRA
                const float p00 = bgra[(static_cast<size_t>(y0) * w + x0) * 4 + sc];
                const float p10 = bgra[(static_cast<size_t>(y0) * w + x1) * 4 + sc];
                const float p01 = bgra[(static_cast<size_t>(y1) * w + x0) * 4 + sc];
                const float p11 = bgra[(static_cast<size_t>(y1) * w + x1) * 4 + sc];
                const float top = p00 + (p10 - p00) * wx;
                const float bot = p01 + (p11 - p01) * wx;
                out[c] = (top + (bot - top) * wy) / 255.0f;
            }
        }
    }
}

// Bilinear float confidence (kNetSize^2, 0..1) -> packed u8 matte (w,h).
void UpscaleMatte(const float* net, int w, int h, uint8_t* matteBuf) {
    const float sx = static_cast<float>(kNetSize) / w, sy = static_cast<float>(kNetSize) / h;
    for (int y = 0; y < h; ++y) {
        const float fy = std::max(0.0f, (y + 0.5f) * sy - 0.5f);
        const int y0 = std::min(static_cast<int>(fy), kNetSize - 1), y1 = std::min(y0 + 1, kNetSize - 1);
        const float wy = fy - static_cast<float>(y0);
        for (int x = 0; x < w; ++x) {
            const float fx = std::max(0.0f, (x + 0.5f) * sx - 0.5f);
            const int x0 = std::min(static_cast<int>(fx), kNetSize - 1), x1 = std::min(x0 + 1, kNetSize - 1);
            const float wx = fx - static_cast<float>(x0);
            const float p00 = net[static_cast<size_t>(y0) * kNetSize + x0];
            const float p10 = net[static_cast<size_t>(y0) * kNetSize + x1];
            const float p01 = net[static_cast<size_t>(y1) * kNetSize + x0];
            const float p11 = net[static_cast<size_t>(y1) * kNetSize + x1];
            const float top = p00 + (p10 - p00) * wx;
            const float v = std::clamp(top + ((p01 + (p11 - p01) * wx) - top) * wy, 0.0f, 1.0f);
            matteBuf[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(v * 255.0f + 0.5f);
        }
    }
}

} // namespace

// ---- public API -----------------------------------------------------------------------

SegOnnx::SegOnnx() = default;
SegOnnx::~SegOnnx() { Stop(); }

bool SegOnnx::Probe(std::string& detail) {
    const OrtApi* ort = EnsureOrt(detail);
    if (!ort) return false;
    SegImpl* impl = CreateSession(ort, detail);
    if (!impl) return false;
    DestroySession(ort, impl);
    detail = "ONNX CPU green screen available";
    return true;
}

bool SegOnnx::Start() {
    Stop();
    const OrtApi* ort = EnsureOrt(lastError_);
    if (!ort) return false;
    SegImpl* impl = CreateSession(ort, lastError_);
    if (!impl) return false;
    impl_ = impl;
    ready_ = true;
    lastError_.clear();
    return true;
}

void SegOnnx::Stop() {
    auto* impl = static_cast<SegImpl*>(impl_);
    if (impl) DestroySession(g_ort, impl);
    impl_ = nullptr;
    ready_ = false;
}

bool SegOnnx::ProcessFrame(uint8_t* bgra, int width, int height, double expand, double feather) {
    auto* impl = static_cast<SegImpl*>(impl_);
    if (!impl || !impl->session || !bgra || width <= 0 || height <= 0) return false;
    const OrtApi* ort = g_ort;

    DownscaleToNet(bgra, width, height, impl->input.data());

    const int64_t inShape[4] = {1, kNetSize, kNetSize, 3};
    OrtValue* inVal = nullptr;
    if (!Check(ort, ort->CreateTensorWithDataAsOrtValue(
            impl->memInfo, impl->input.data(), impl->input.size() * sizeof(float),
            inShape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVal),
            "CreateTensor", lastError_)) return false;

    const char* inNames[]  = {impl->inName.c_str()};
    const char* outNames[] = {impl->outName.c_str()};
    OrtValue* outVal = nullptr;
    OrtStatus* rs = ort->Run(impl->session, nullptr, inNames,
                             const_cast<const OrtValue* const*>(&inVal), 1, outNames, 1, &outVal);
    ort->ReleaseValue(inVal);
    if (!Check(ort, rs, "Run", lastError_)) return false;

    void* net = nullptr;
    if (!Check(ort, ort->GetTensorMutableData(outVal, &net), "GetTensorData", lastError_)) {
        ort->ReleaseValue(outVal);
        return false;
    }
    impl->matteWork.resize(static_cast<size_t>(width) * height);
    impl->matteTmp.resize(static_cast<size_t>(width) * height);
    UpscaleMatte(static_cast<const float*>(net), width, height, impl->matteWork.data());
    ort->ReleaseValue(outVal);

    matte::Dilate(impl->matteWork.data(), impl->matteTmp.data(), width, height,
                  matte::RadiusFromAmount(expand, matte::kMaxDilateRadius));
    matte::Feather(impl->matteWork.data(), impl->matteTmp.data(), width, height,
                   matte::RadiusFromAmount(feather, matte::kMaxBlurRadius));
    matte::CompositePremultiplied(bgra, impl->matteWork.data(), width, height);
    return true;
}
```

(If the exact `Run` signature differs from the vendored header — e.g. the input-values
parameter is `const OrtValue* const* input_values` vs `const OrtValue* const*` cast —
adjust the cast to match the header; the header is the source of truth.)

- [ ] **Step 4: Write `native/shim/smoke/seg_probe.cpp`**

```cpp
// Standalone smoke for the ONNX green-screen engine (issue #24). Needs the ORT runtime
// + model (COS_SEG_RUNTIME_DIR or <exe>/onnx). Feeds a synthetic frame, asserts the
// engine writes a plausible matte, prints per-frame latency. Exit 0 = pass.
//
// Built by CMake as target seg_probe (compiles seg_onnx.cpp directly — the shim's
// class symbols are hidden-visibility, so linking the .so is not an option).
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include "../seg_onnx.h"

int main() {
    std::string detail;
    if (!SegOnnx::Probe(detail)) {
        std::fprintf(stderr, "PROBE FAIL: %s\n", detail.c_str());
        return 1;
    }
    std::printf("probe: %s\n", detail.c_str());

    SegOnnx seg;
    if (!seg.Start()) {
        std::fprintf(stderr, "START FAIL: %s\n", seg.LastError().c_str());
        return 1;
    }

    // Synthetic 640x480 frame: dark background, bright centered "torso" blob — enough
    // structure that the model produces a non-constant confidence map.
    const int w = 640, h = 480;
    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* px = bgra.data() + (static_cast<size_t>(y) * w + x) * 4;
            const bool blob = (x > w / 3 && x < 2 * w / 3 && y > h / 4);
            px[0] = blob ? 200 : 30; px[1] = blob ? 180 : 40;
            px[2] = blob ? 160 : 50; px[3] = 255;
        }

    // Warm-up + timed runs.
    std::vector<uint8_t> work = bgra;
    if (!seg.ProcessFrame(work.data(), w, h, 0.0, 0.0)) {
        std::fprintf(stderr, "PROCESS FAIL: %s\n", seg.LastError().c_str());
        return 1;
    }
    const int kRuns = 30;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kRuns; ++i) {
        work = bgra;
        if (!seg.ProcessFrame(work.data(), w, h, 0.2, 0.2)) {
            std::fprintf(stderr, "PROCESS FAIL (run %d): %s\n", i, seg.LastError().c_str());
            return 1;
        }
    }
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kRuns;

    // Matte plausibility: alpha must vary (not all-255 passthrough, not all-0).
    uint64_t histLow = 0, histHigh = 0;
    for (size_t i = 3; i < work.size(); i += 4) {
        if (work[i] < 64) ++histLow;
        if (work[i] > 192) ++histHigh;
    }
    std::printf("avg %.2f ms/frame; alpha<64: %llu px, alpha>192: %llu px\n",
                ms, (unsigned long long)histLow, (unsigned long long)histHigh);
    if (histLow == 0 && histHigh == static_cast<uint64_t>(w) * h) {
        std::fprintf(stderr, "FAIL: matte is all-opaque (engine did not run?)\n");
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
```

- [ ] **Step 5: CMake wiring** — add `seg_onnx.cpp` and (already, from Task 1) `matte_ops.cpp` to the shim's source list; append at the bottom of `CMakeLists.txt`:

```cmake
# ONNX green-screen smoke (issue #24): compiles the engine directly (shim symbols are
# hidden). Needs COS_SEG_RUNTIME_DIR (ORT lib + model) at run time, not build time.
add_executable(seg_probe smoke/seg_probe.cpp seg_onnx.cpp matte_ops.cpp paths.cpp)
target_compile_options(seg_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(seg_probe PRIVATE ${CMAKE_DL_LIBS})
```

vcxproj: add `<ClCompile Include="seg_onnx.cpp" />`, `<ClInclude Include="seg_onnx.h" />` (text-only edit).

- [ ] **Step 6: Build + assemble a local runtime dir + run seg_probe**

```bash
cmake -S native/shim -B native/shim/build && cmake --build native/shim/build
S=/tmp/claude-1000/-home-ariff-officialdad-camera-on-screen/093d13a6-7168-47ba-9eb7-ca8567b7240b/scratchpad
mkdir -p /tmp/seg-runtime
tar -xzf "$S/ort.tgz" -C /tmp onnxruntime-linux-x64-1.28.0/lib/libonnxruntime.so.1.28.0
cp /tmp/onnxruntime-linux-x64-1.28.0/lib/libonnxruntime.so.1.28.0 /tmp/seg-runtime/libonnxruntime.so.1
cp "$S/saved_model_tflite_tfjs_tftrt_onnx_coreml/model_float32.onnx" /tmp/seg-runtime/selfie_segmentation.onnx
COS_SEG_RUNTIME_DIR=/tmp/seg-runtime ./native/shim/build/seg_probe
```
Expected: `probe: ONNX CPU green screen available`, avg latency printed (expect < 15 ms/frame on the dev box), `OK`, exit 0. Also verify the missing-runtime path: `COS_SEG_RUNTIME_DIR= ./native/shim/build/seg_probe` run from a dir without `onnx/` → `PROBE FAIL: ONNX runtime not found …`, exit 1.

- [ ] **Step 7: Commit**

```bash
git add native/shim/seg_onnx.h native/shim/seg_onnx.cpp native/shim/third_party \
        native/shim/smoke/seg_probe.cpp native/shim/CMakeLists.txt native/shim/shim.vcxproj
git commit -m "feat(shim): SegOnnx CPU green-screen engine + seg_probe smoke (#24)"
```

---

### Task 4: Capture-worker dispatch + shim.cpp wiring (both platforms)

**Files:**
- Modify: `native/shim/capture.h:32-38`, `native/shim/capture_v4l2.cpp` (g_state, worker loop ~343-441, SetGreenScreen ~538), `native/shim/capture.cpp` (g_state ~42, worker ~322/479-509, SetGreenScreen ~632), `native/shim/shim.cpp` (includes, cos_set_params, cos_query_capabilities)

**Interfaces:**
- Consumes: `SegOnnx` (Task 3), `CosParams.green_screen_backend` / `CosCaps.gs_onnx_*` (Task 2).
- Produces: `Capture::SetGreenScreen(bool enabled, int backend)` (was 1-arg); worker honors backend semantics 0/1/2; `cos_query_capabilities` fills `gs_onnx_available`/`gs_onnx_detail` and returns 1 if ANY effect (incl. ONNX) is available.

- [ ] **Step 1: `capture.h`** — change the declaration to:

```cpp
    // Toggles green screen for subsequent frames and selects the engine
    // (0 Auto = Maxine else ONNX, 1 Maxine, 2 ONNX CPU — issue #24).
    // Thread-safe; the worker owns both engine objects.
    void SetGreenScreen(bool enabled, int backend);
```

- [ ] **Step 2: `capture_v4l2.cpp` state + setter** — in `CaptureState`, under `greenScreenEnabled`, add:

```cpp
    std::atomic<int>      greenScreenBackend{0}; // 0 Auto, 1 Maxine, 2 ONNX (#24)
```

Change the setter:

```cpp
void Capture::SetGreenScreen(bool enabled, int backend) {
    g_state.greenScreenEnabled.store(enabled, std::memory_order_release);
    g_state.greenScreenBackend.store(backend, std::memory_order_release);
}
```

- [ ] **Step 3: `capture_v4l2.cpp` worker** — add `#include "seg_onnx.h"` next to `#include "aigs.h"`. Next to `Aigs aigs;` (line ~353) declare:

```cpp
    SegOnnx seg; int prevGsBackend = 0;
```

Replace the green-screen block (currently lines ~415-441, from `const bool gsWant = …` through `g_state.greenScreenActive.store(gsApplied, …);`) with:

```cpp
            // Green screen composites last and authors the premultiplied alpha matte.
            // Two engines (issue #24): Maxine AIGS (RTX) and SegOnnx (CPU, any hardware).
            // Backend 0 = Auto (Maxine first, ONNX fallback), 1 = Maxine, 2 = ONNX.
            const bool gsWant = g_state.greenScreenEnabled.load(std::memory_order_acquire);
            const int  gsBackend = g_state.greenScreenBackend.load(std::memory_order_acquire);
            if (gsBackend != prevGsBackend) { // selection changed: re-resolve from scratch
                if (aigs.IsReady()) aigs.Stop();
                if (seg.IsReady())  seg.Stop();
                prevGsBackend = gsBackend;
            }
            if (!gsWant) {
                if (aigs.IsReady()) aigs.Stop();
                if (seg.IsReady())  seg.Stop();
                std::lock_guard<std::mutex> e(g_state.gsErrMtx);
                if (!g_state.gsError.empty()) g_state.gsError.clear();
            } else {
                // Bring up the selected engine. Auto tries Maxine, then falls back to
                // ONNX; once a fallback engine is up we stop retrying Maxine (stable,
                // no per-frame churn). Explicit selections retry their engine only.
                if ((gsBackend == 0 || gsBackend == 1) && !aigs.IsReady() && !seg.IsReady()) {
                    if (!aigs.Start() && gsBackend == 1) {
                        std::lock_guard<std::mutex> e(g_state.gsErrMtx);
                        if (g_state.gsError != aigs.LastError()) g_state.gsError = aigs.LastError();
                    }
                }
                if ((gsBackend == 0 || gsBackend == 2) && !aigs.IsReady() && !seg.IsReady()) {
                    if (!seg.Start()) {
                        std::lock_guard<std::mutex> e(g_state.gsErrMtx);
                        if (g_state.gsError != seg.LastError()) g_state.gsError = seg.LastError();
                    }
                }
            }
            bool gsApplied = false;
            if (gsWant && (aigs.IsReady() || seg.IsReady())) {
                const double expand  = g_state.matteExpand.load(std::memory_order_acquire);
                const double feather = g_state.matteFeather.load(std::memory_order_acquire);
                const std::string& engineErr = aigs.IsReady() ? aigs.LastError() : seg.LastError();
                gsApplied = aigs.IsReady()
                    ? aigs.ProcessFrame(bgra.data(), width, height, expand, feather)
                    : seg.ProcessFrame(bgra.data(), width, height, expand, feather);
                std::lock_guard<std::mutex> e(g_state.gsErrMtx);
                if (!gsApplied) {
                    if (g_state.gsError != engineErr) g_state.gsError = engineErr;
                } else if (!g_state.gsError.empty()) {
                    g_state.gsError.clear();
                }
            }
            g_state.greenScreenActive.store(gsApplied, std::memory_order_release);
```

(Note the `engineErr` reference is taken BEFORE ProcessFrame so a failing call that
flips `IsReady()` still reports the right engine's error.)

At worker teardown (line ~489, `aigs.Stop();`) add `seg.Stop();` beside it.

- [ ] **Step 4: `capture.cpp` (Windows — exact mirror, text-only)** — same three edits: `#include "seg_onnx.h"` next to `#include "aigs.h"`; `std::atomic<int> greenScreenBackend{0};` under `greenScreenEnabled` in its `CaptureState` (~line 42); `SegOnnx seg; int prevGsBackend = 0;` next to `Aigs aigs;` (~line 322); replace its green-screen block (~479-509, the `want`/`applied` version operating on `cur.data(), curW, curH`) with the same dispatch code as Step 3 — substituting `cur.data(), curW, curH` for `bgra.data(), width, height` and keeping its local naming (`want` → `gsWant` etc. per the new block); add `seg.Stop();` at its teardown (~line 569); update `Capture::SetGreenScreen` (~632) to the 2-arg version.

- [ ] **Step 5: `shim.cpp`** — add `#include "seg_onnx.h"`; in `cos_set_params`:

```cpp
    g_capture.SetGreenScreen(p->green_screen_enabled != 0, p->green_screen_backend);
```

In `cos_query_capabilities`, after the `fiDetail` block:

```cpp
    std::string soDetail;
    bool soOk = SegOnnx::Probe(soDetail);
    out->gs_onnx_available = soOk ? 1 : 0;
    size_t sn = soDetail.size() < 255 ? soDetail.size() : 255;
    std::memcpy(out->gs_onnx_detail, soDetail.data(), sn);
    out->gs_onnx_detail[sn] = '\0';
```

and extend the return:

```cpp
    return (gsOk || ecOk || soOk || out->super_res_available || out->frame_interp_available) ? 1 : 0;
```

- [ ] **Step 6: Build + smoke on Linux (stub + SDK configs)**

```bash
cmake --build native/shim/build && ./native/shim/build/v4l2_probe
cmake --build native/shim/build-sdk
```
Expected: clean builds; `v4l2_probe` passes.

- [ ] **Step 7: Commit**

```bash
git add native/shim/capture.h native/shim/capture_v4l2.cpp native/shim/capture.cpp native/shim/shim.cpp
git commit -m "feat(shim): green-screen engine dispatch in both capture workers (#24)"
```

---

### Task 5: Orchestrator — ONNX gate, backend clamp

**Files:**
- Modify: `src/CameraOnScreen.Core/Orchestration/Orchestrator.cs`
- Test: `tests/CameraOnScreen.Core.Tests/Orchestration/OrchestratorTests.cs`

**Interfaces:**
- Consumes: `ShimCapabilities.GreenScreenOnnxAvailable/GreenScreenOnnxDetail`, `ShimParams.GreenScreenBackend`, `FakeShim` (Task 2).
- Produces: `Orchestrator.GreenScreenMaxineAvailable` (bool), `Orchestrator.GreenScreenOnnxAvailable` (bool); **semantic change:** `EffectsAvailable` = Maxine OR ONNX; `ApplyParams` clamps an unavailable explicit backend to 0 (Auto). Task 6's VM consumes all three props.

- [ ] **Step 1: Write the failing tests** — append to `OrchestratorTests.cs` (match the file's existing test style):

```csharp
    [Fact]
    public void ProbeCapabilities_OnnxOnly_EnablesGreenScreen()
    {
        var shim = new FakeShim { GreenScreenAvailable = false, GreenScreenOnnxAvailable = true };
        var orch = new Orchestrator(shim, GpuTier.NonRtx);
        orch.ProbeCapabilities();
        Assert.True(orch.EffectsAvailable);
        Assert.False(orch.GreenScreenMaxineAvailable);
        Assert.True(orch.GreenScreenOnnxAvailable);
    }

    [Fact]
    public void ApplyParams_ClampsUnavailableExplicitBackendToAuto()
    {
        var shim = new FakeShim { GreenScreenAvailable = false, GreenScreenOnnxAvailable = true };
        var orch = new Orchestrator(shim, GpuTier.NonRtx);
        orch.ProbeCapabilities();
        orch.ApplyParams(new ShimParams(null, true, 0, 0, false, 0.5, 0.5, GreenScreenBackend: 1));
        Assert.Equal(0, shim.LastParams!.GreenScreenBackend);   // Maxine unavailable -> Auto
        Assert.True(shim.LastParams!.GreenScreenEnabled);       // ONNX carries the effect
    }

    [Fact]
    public void ApplyParams_PassesAvailableBackendThrough()
    {
        var shim = new FakeShim { GreenScreenAvailable = true, GreenScreenOnnxAvailable = true };
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        orch.ProbeCapabilities();
        orch.ApplyParams(new ShimParams(null, true, 0, 0, false, 0.5, 0.5, GreenScreenBackend: 2));
        Assert.Equal(2, shim.LastParams!.GreenScreenBackend);
    }
```

- [ ] **Step 2: Run to verify failure**

```bash
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~OrchestratorTests"
```
Expected: compile FAIL (`GreenScreenMaxineAvailable` not defined).

- [ ] **Step 3: Implement** — in `Orchestrator.cs` add the props:

```csharp
    /// <summary>True when the Maxine (RTX) green-screen engine can run. False until probed.</summary>
    public bool GreenScreenMaxineAvailable { get; private set; }

    /// <summary>True when the ONNX CPU green-screen engine can run (issue #24). False until probed.</summary>
    public bool GreenScreenOnnxAvailable { get; private set; }
```

In `ProbeCapabilities()` replace the two green-screen lines:

```csharp
        GreenScreenMaxineAvailable = caps.GreenScreenAvailable;
        GreenScreenOnnxAvailable = caps.GreenScreenOnnxAvailable;
        // Green screen is usable when EITHER engine can run (#24); the detail note
        // (shown only while unavailable) then carries both reasons.
        EffectsAvailable = caps.GreenScreenAvailable || caps.GreenScreenOnnxAvailable;
        CapabilityDetail = EffectsAvailable
            ? (caps.GreenScreenAvailable ? caps.Detail : caps.GreenScreenOnnxDetail)
            : $"{caps.Detail} · {caps.GreenScreenOnnxDetail}";
```

In `ApplyParams` extend the `with`:

```csharp
            GreenScreenEnabled = requested.GreenScreenEnabled && EffectsAvailable,
            GreenScreenBackend = requested.GreenScreenBackend switch
            {
                1 when !GreenScreenMaxineAvailable => 0, // engine gone -> Auto resolves
                2 when !GreenScreenOnnxAvailable => 0,
                var b => b,
            },
```

- [ ] **Step 4: Run tests**

```bash
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj
```
Expected: PASS (full suite — existing EffectsAvailable tests may assert the old Maxine-only semantics; update any that do to the new either-engine semantics, they are the intended behavior change).

- [ ] **Step 5: Commit**

```bash
git add src/CameraOnScreen.Core/Orchestration/Orchestrator.cs \
        tests/CameraOnScreen.Core.Tests/Orchestration/OrchestratorTests.cs
git commit -m "feat(core): green-screen backend gate + clamp in Orchestrator (#24)"
```

---

### Task 6: MainViewModel + config persistence

**Files:**
- Modify: `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs`, `src/CameraOnScreen.Core/Config/Models.cs` (EffectSettings)
- Test: `tests/CameraOnScreen.Core.Tests/ViewModels/` (existing MainViewModel test file)

**Interfaces:**
- Consumes: `Orchestrator.GreenScreenMaxineAvailable/GreenScreenOnnxAvailable` (Task 5), `ShimParams.GreenScreenBackend` (Task 2).
- Produces: `MainViewModel.GreenScreenBackendIndex` (int observable, 0 Auto/1 Maxine/2 ONNX), `MainViewModel.GreenScreenMaxineAvailable`/`GreenScreenOnnxAvailable` (bool observables), `EffectSettings.GreenScreenBackend` (int, default 0). Task 7's XAML binds all three.

- [ ] **Step 1: Write the failing tests** — in the existing MainViewModel test file, following its construction pattern (FakeShim + Orchestrator):

```csharp
    [Fact]
    public void GreenScreenBackend_RoundTripsThroughConfig()
    {
        var shim = new FakeShim();
        var vm = new MainViewModel(new Orchestrator(shim, GpuTier.Rtx), shim);
        vm.GreenScreenBackendIndex = 2;
        var config = vm.ToAppConfig(0, 0, 100, 100);
        Assert.Equal(2, config.Effects.GreenScreenBackend);

        var vm2 = new MainViewModel(new Orchestrator(shim, GpuTier.Rtx), shim);
        vm2.LoadFrom(config);
        Assert.Equal(2, vm2.GreenScreenBackendIndex);
    }

    [Fact]
    public void LoadFrom_ClampsBackendIndex()
    {
        var shim = new FakeShim();
        var vm = new MainViewModel(new Orchestrator(shim, GpuTier.Rtx), shim);
        vm.LoadFrom(new AppConfig { Effects = new EffectSettings { GreenScreenBackend = 9 } });
        Assert.Equal(0, vm.GreenScreenBackendIndex); // out-of-range -> Auto (combo crash guard)
    }

    [Fact]
    public void BuildParams_CarriesBackend()
    {
        var shim = new FakeShim();
        var vm = new MainViewModel(new Orchestrator(shim, GpuTier.Rtx), shim);
        vm.GreenScreenBackendIndex = 1;
        Assert.Equal(1, vm.BuildParams().GreenScreenBackend);
    }
```

- [ ] **Step 2: Run to verify failure** — same `dotnet test --filter` pattern; expected compile FAIL.

- [ ] **Step 3: Implement**
  - `Models.cs` `EffectSettings`: add `public int GreenScreenBackend { get; init; }` (0 = Auto default) after `GreenScreenFeather`.
  - `MainViewModel.cs`:
    - observables (near `greenScreenFeather`): `[ObservableProperty] private int greenScreenBackendIndex;` and (near `effectsAvailable`) `[ObservableProperty] private bool greenScreenMaxineAvailable;` + `[ObservableProperty] private bool greenScreenOnnxAvailable;`
    - ctor + `ProbeCapabilitiesAsync()`: mirror `orchestrator.GreenScreenMaxineAvailable`/`GreenScreenOnnxAvailable` into the two new observables (both places, same as the existing props).
    - `LoadFrom`: `GreenScreenBackendIndex = config.Effects.GreenScreenBackend is >= 0 and <= 2 ? config.Effects.GreenScreenBackend : 0;` — out-of-range → 0, same SelectedIndex-crash guard as SuperRes (see the clamp comment at `MainViewModel.cs:104`).
    - `ToAppConfig` Effects block: `GreenScreenBackend = GreenScreenBackendIndex,`
    - live push partial (next to the other green-screen ones): `partial void OnGreenScreenBackendIndexChanged(int value) => ApplyLiveParams();`
    - `BuildParams()`: `GreenScreenBackend: GreenScreenBackendIndex);` (append; keep the existing arg order).

- [ ] **Step 4: Run the full suite**

```bash
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj
```
Expected: PASS, 0 warnings.

- [ ] **Step 5: Commit**

```bash
git add src/CameraOnScreen.Core/ViewModels/MainViewModel.cs src/CameraOnScreen.Core/Config/Models.cs \
        tests/CameraOnScreen.Core.Tests/ViewModels/
git commit -m "feat(core): green-screen engine selection in VM + config (#24)"
```

---

### Task 7: Avalonia UI — engine dropdown

**Files:**
- Modify: `src/CameraOnScreen.App.Avalonia/MainWindow.axaml` (AI Effects card, after the Feather slider StackPanel, ~line 106)

**Interfaces:**
- Consumes: `GreenScreenBackendIndex`, `GreenScreenMaxineAvailable`, `GreenScreenOnnxAvailable`, `EffectsAvailable` (Task 6).

- [ ] **Step 1: Add the dropdown** — insert after the Feather `</StackPanel>`:

```xml
                            <StackPanel Spacing="4">
                                <TextBlock Text="Green-screen Engine" />
                                <ComboBox HorizontalAlignment="Stretch" IsEnabled="{Binding EffectsAvailable}"
                                          SelectedIndex="{Binding GreenScreenBackendIndex, Mode=TwoWay}">
                                    <ComboBoxItem Content="Auto" />
                                    <ComboBoxItem Content="NVIDIA Maxine (GPU)"
                                                  IsEnabled="{Binding GreenScreenMaxineAvailable}" />
                                    <ComboBoxItem Content="ONNX (CPU)"
                                                  IsEnabled="{Binding GreenScreenOnnxAvailable}" />
                                </ComboBox>
                            </StackPanel>
```

- [ ] **Step 2: Build + run against the real shim (RTX dev box, camera attached)**

```bash
dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj -c Release /p:TreatWarningsAsErrors=true
# Stage the ONNX runtime beside the built shim so the probe finds the bundled tier:
BIN=$(dirname "$(find src/CameraOnScreen.App.Avalonia/bin/Release -name libCameraOnScreen.Shim.so | head -1)")
mkdir -p "$BIN/onnx" && cp /tmp/seg-runtime/* "$BIN/onnx/"
dotnet run --project src/CameraOnScreen.App.Avalonia -c Release
```

Manual A/B check (the human visual gate — `docs/superpowers/verification/`):
1. Start camera, enable green screen. Dropdown enabled; on the RTX box all three items selectable.
2. Auto → Maxine matte (crisp edges). Switch to "ONNX (CPU)" live → matte visibly softer, effect continues within a frame or two, fps holds ~30.
3. Expand/Feather sliders act on BOTH engines identically.
4. Rename the shim-side `onnx/` dir → restart → ONNX item greys, Maxine still works; restore.
Report findings; screenshots optional.

- [ ] **Step 3: Commit**

```bash
git add src/CameraOnScreen.App.Avalonia/MainWindow.axaml
git commit -m "feat(ui): green-screen engine dropdown (Auto/Maxine/ONNX) (#24)"
```

---

### Task 8: Packaging (publish-linux.sh) + third-party notices + docs

**Files:**
- Modify: `scripts/publish-linux.sh`, `THIRD-PARTY-NOTICES.md`, `CLAUDE.md`, `README.md`

- [ ] **Step 1: Stage the ONNX runtime in `publish-linux.sh`** — insert after the `bundle-maxine-linux.sh` call (line ~34), before the stub-shim check:

```bash
# ONNX green-screen runtime (issue #24): ORT + selfie-segmentation model -> $OUT/onnx
# (the shim's bundled tier; COS_SEG_RUNTIME_DIR overrides in dev). Pinned + sha-verified;
# cached under ~/.cache so rebuilds cost nothing.
ORT_VER="1.28.0"
ORT_SHA="a3e1b79d7bb1bf09696ce675f49e4064e6c81f6202b8225624fff0e93f8d6407"
MODEL_SHA="de212dabbc6266f0047711d1dfae80900f7b596b9ed5f7665f3d1cf68c5443ee"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/camera-on-screen"
mkdir -p "$CACHE" "$OUT/onnx"
[ -f "$CACHE/ort-$ORT_VER.tgz" ] || curl -fsSL -o "$CACHE/ort-$ORT_VER.tgz" \
  "https://github.com/microsoft/onnxruntime/releases/download/v$ORT_VER/onnxruntime-linux-x64-$ORT_VER.tgz"
echo "$ORT_SHA  $CACHE/ort-$ORT_VER.tgz" | sha256sum -c - >/dev/null
[ -f "$CACHE/pinto109.tar.gz" ] || curl -fsSL -o "$CACHE/pinto109.tar.gz" \
  "https://s3.ap-northeast-2.wasabisys.com/pinto-model-zoo/109_Selfie_Segmentation/resources.tar.gz"
tar -xzf "$CACHE/ort-$ORT_VER.tgz" -C "$CACHE" \
  "onnxruntime-linux-x64-$ORT_VER/lib/libonnxruntime.so.$ORT_VER" \
  "onnxruntime-linux-x64-$ORT_VER/LICENSE" "onnxruntime-linux-x64-$ORT_VER/ThirdPartyNotices.txt"
tar -xzf "$CACHE/pinto109.tar.gz" -C "$CACHE" saved_model_tflite_tfjs_tftrt_onnx_coreml/model_float32.onnx
echo "$MODEL_SHA  $CACHE/saved_model_tflite_tfjs_tftrt_onnx_coreml/model_float32.onnx" | sha256sum -c - >/dev/null
cp "$CACHE/onnxruntime-linux-x64-$ORT_VER/lib/libonnxruntime.so.$ORT_VER" "$OUT/onnx/libonnxruntime.so.1"
cp "$CACHE/onnxruntime-linux-x64-$ORT_VER/LICENSE" "$OUT/onnx/ONNXRUNTIME-LICENSE"
cp "$CACHE/onnxruntime-linux-x64-$ORT_VER/ThirdPartyNotices.txt" "$OUT/onnx/ONNXRUNTIME-ThirdPartyNotices.txt"
cp "$CACHE/saved_model_tflite_tfjs_tftrt_onnx_coreml/model_float32.onnx" "$OUT/onnx/selfie_segmentation.onnx"
```

- [ ] **Step 2: `THIRD-PARTY-NOTICES.md`** — append a section following the file's existing format:

```markdown
## ONNX Runtime

Bundled as `onnx/libonnxruntime.so.1` (Linux). © Microsoft Corporation.
Licensed under the MIT License — see `onnx/ONNXRUNTIME-LICENSE` in the
distribution and https://github.com/microsoft/onnxruntime. Additional
attributions: `onnx/ONNXRUNTIME-ThirdPartyNotices.txt`.

## MediaPipe Selfie Segmentation model

Bundled as `onnx/selfie_segmentation.onnx` — the Google MediaPipe selfie-
segmentation model (© Google LLC), ONNX conversion from the PINTO model zoo
(© Katsuya Hyodo), both licensed under the Apache License 2.0.
https://github.com/google-ai-edge/mediapipe · https://github.com/PINTO0309/PINTO_model_zoo
```

- [ ] **Step 3: Docs**
  - `CLAUDE.md`: in the Linux-build section after the FRUC paragraph, add (adjust to fit surrounding prose):

    > **ONNX green screen (issue #24, 2026-07-30):** second green-screen engine `seg_onnx.{h,cpp}` — MediaPipe selfie-segmentation (256×256, Apache-2.0) in ONNX Runtime 1.28 **CPU EP**, works on any hardware, both OSes. No build flag: ORT is dlopen'd (`OrtGetApiBase`) from `COS_SEG_RUNTIME_DIR` else `<shim>/onnx/` (`libonnxruntime.so.1`/`onnxruntime.dll` + `selfie_segmentation.onnx`); vendored MIT C headers in `native/shim/third_party/onnxruntime/`. Backend select: `CosParams.green_screen_backend` 0 Auto/1 Maxine/2 ONNX; worker dispatch in both capture backends; shared matte chain `matte_ops.{h,cpp}`. Smoke: `seg_probe` (runs REAL inference on hosted CI, no GPU).
  - `README.md`: read its effects/requirements wording first, then update the green-screen line to say it runs on NVIDIA Maxine (RTX, best quality) **or** a built-in CPU engine on any hardware, and trim any "RTX required for green screen" claim accordingly (RTX stays required for eye contact / super-res / smooth 60).

- [ ] **Step 4: Full publish verify (RTX box)**

```bash
scripts/publish-linux.sh
COS_VFX_RUNTIME_DIR= COS_AR_RUNTIME_DIR= COS_FRUC_RUNTIME_DIR= COS_SEG_RUNTIME_DIR= \
  dist/linux/CameraOnScreen.App.Avalonia &
```
Expected: app runs from the bundle with NO env vars; green-screen dropdown offers Maxine AND ONNX (both bundled tiers resolve). Close the app. Also refresh the desktop launcher build (memory: launcher runs `dist/linux`, stale otherwise) — this publish already did.

- [ ] **Step 5: Commit**

```bash
git add scripts/publish-linux.sh THIRD-PARTY-NOTICES.md CLAUDE.md README.md
git commit -m "feat(dist): bundle ONNX runtime + model; notices + docs (#24)"
```

---

### Task 9: CI — hosted job runs real inference

**Files:**
- Modify: `.github/workflows/ci-linux.yml` (hosted `linux` job, after the v4l2_probe step)

- [ ] **Step 1: Add the smoke step**

```yaml
      - name: ONNX green-screen smoke (real CPU inference — no GPU needed)
        run: |
          curl -fsSL -o /tmp/ort.tgz https://github.com/microsoft/onnxruntime/releases/download/v1.28.0/onnxruntime-linux-x64-1.28.0.tgz
          echo "a3e1b79d7bb1bf09696ce675f49e4064e6c81f6202b8225624fff0e93f8d6407  /tmp/ort.tgz" | sha256sum -c -
          curl -fsSL -o /tmp/pinto109.tar.gz "https://s3.ap-northeast-2.wasabisys.com/pinto-model-zoo/109_Selfie_Segmentation/resources.tar.gz"
          mkdir -p /tmp/seg-runtime
          tar -xzf /tmp/ort.tgz -C /tmp onnxruntime-linux-x64-1.28.0/lib/libonnxruntime.so.1.28.0
          cp /tmp/onnxruntime-linux-x64-1.28.0/lib/libonnxruntime.so.1.28.0 /tmp/seg-runtime/libonnxruntime.so.1
          tar -xzf /tmp/pinto109.tar.gz -C /tmp saved_model_tflite_tfjs_tftrt_onnx_coreml/model_float32.onnx
          echo "de212dabbc6266f0047711d1dfae80900f7b596b9ed5f7665f3d1cf68c5443ee  /tmp/saved_model_tflite_tfjs_tftrt_onnx_coreml/model_float32.onnx" | sha256sum -c -
          cp /tmp/saved_model_tflite_tfjs_tftrt_onnx_coreml/model_float32.onnx /tmp/seg-runtime/selfie_segmentation.onnx
          COS_SEG_RUNTIME_DIR=/tmp/seg-runtime ./native/shim/build/seg_probe
```

- [ ] **Step 2: Local rehearsal of exactly that script block** (copy-paste it into a shell from the repo root, using the already-built `native/shim/build/seg_probe`). Expected: `OK`, exit 0.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci-linux.yml
git commit -m "ci: run ONNX green-screen inference smoke on hosted Linux (#24)"
```

---

### Task 10: Full verify, PR, issues

**Files:** none (process)

- [ ] **Step 1: Full local gate**

```bash
cmake --build native/shim/build && ./native/shim/build/v4l2_probe
COS_SEG_RUNTIME_DIR=/tmp/seg-runtime ./native/shim/build/seg_probe
dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj -c Release /p:TreatWarningsAsErrors=true --nologo
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj -c Release /p:TreatWarningsAsErrors=true --nologo
```
Expected: all green, 0 warnings.

- [ ] **Step 2: Push + PR** (repo rule: merges go through PRs; the PR run also exercises the new CI smoke + the maxine-rtx job)

```bash
git push -u origin feat/onnx-green-screen
gh pr create --title "feat: hardware-agnostic green screen via ONNX Runtime (closes #24)" --body "$(cat <<'EOF'
Second, user-selectable green-screen engine: MediaPipe selfie-segmentation
(Apache-2.0) in ONNX Runtime 1.28 CPU EP — runs on AMD/Intel/non-RTX/no-GPU,
Linux + Windows. Maxine stays the RTX quality tier; new Engine dropdown
(Auto / Maxine / ONNX) lets RTX users A/B GPU vs CPU mattes live.

Spec: docs/superpowers/specs/2026-07-30-hw-agnostic-green-screen-design.md
Plan: docs/superpowers/plans/2026-07-30-hw-agnostic-green-screen.md

- shim: SegOnnx engine (dlopen'd ORT, no build flag), shared matte_ops,
  worker dispatch, ABI: green_screen_backend + gs_onnx_available
- app: Orchestrator gate/clamp, VM + config persistence, Avalonia dropdown
- dist: publish-linux.sh stages onnx/ (pinned, sha-verified); notices updated
- ci: hosted job now runs REAL segmentation inference (first fully
  CI-verifiable AI effect — no GPU needed)

Closes #24.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 3: Update the issues**

```bash
gh issue edit 24 --title "Hardware-agnostic AI Green Screen (ONNX Runtime CPU engine — supersedes DirectML framing)"
gh issue comment 24 --body "Superseded the AMD/DirectML framing: DirectML is Windows-only and in maintenance mode; the implemented approach is the MediaPipe selfie-segmentation model in ONNX Runtime (CPU EP) — hardware-agnostic (AMD/Intel/non-RTX/no-GPU), Linux + Windows, user-selectable next to Maxine. Design: docs/superpowers/specs/2026-07-30-hw-agnostic-green-screen-design.md. Closes with the linked PR."
gh issue comment 25 --body "Research note while superseding #24 (2026-07-30): no hardware-AGNOSTIC frame-interpolation path exists. MediaPipe has nothing for FRC. Only cross-vendor option: RIFE on ncnn-Vulkan (MIT) — real-time 720p on midrange discrete GPUs (any vendor), 1080p needs ~RTX-3070 class; CPU-only interpolation is not viable (block-matching quality unacceptable for webcam content). AMD AMF FRC remains the AMD-specific option if this issue stays vendor-scoped; a RIFE/ncnn backend would be the cross-vendor alternative — either way it stays a capable-GPU-gated effect, never a CPU-floor one."
```

- [ ] **Step 4: Merge gate** — wait for CI green; merging is the user's call (superpowers:finishing-a-development-branch).

---

## Self-Review Notes

- Spec §3 backend semantics (auto/explicit, live switch, worker affinity) → Task 4; §4 SegOnnx internals → Task 3; §5 ABI → Task 2; §6 C#/UI → Tasks 5-7 (WinUI XAML deferred per spec); §7 errors → Tasks 3-4 (mirrors Aigs); §8 packaging/licenses → Task 8; §9 testing → Tasks 3 (seg_probe), 5-6 (xUnit), 9 (CI), 7 (visual gate); §10 issues → Task 10. No gaps.
- Windows parity edits (capture.cpp, vcxproj, WinUI PInvokeShim) are text-exact mirrors, flagged not-compilable-here per Global Constraints.
- Type names cross-checked: `GreenScreenBackend`/`GreenScreenOnnxAvailable`/`GreenScreenOnnxDetail`/`GreenScreenMaxineAvailable`/`GreenScreenBackendIndex`/`gs_onnx_available`/`gs_onnx_detail`/`green_screen_backend` used consistently across Tasks 2-7.
