# M4 — Eye Contact (AI gaze redirection) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add NVIDIA Maxine AR-SDK Eye Contact (gaze redirection) as a second live overlay effect that coexists with the M3 green screen, gated by its own capability probe, exposed as a single panel toggle.

**Architecture:** A new worker-thread-local `EyeContact` C++ wrapper around `NvAR_Feature_GazeRedirection` (CPU-copy: upload BGRA→GPU BGR, `NvAR_Run`, download→BGRA) runs in the capture worker **before** the green-screen matte. The C ABI gains a second capability field (`CosCaps` grows); `cos_set_params` flips an atomic enable flag. Managed: the single `EffectsAvailable` gate splits into `EffectsAvailable` (green screen) + `EyeContactAvailable` (eye contact), each with its own probe-derived reason note.

**Tech Stack:** C++17 (MSVC v143, native shim), Maxine AR SDK (NvAR, build via cloned repo proxy stubs), C# .NET 8 + CommunityToolkit.Mvvm (Core), WinUI 3 (App), xUnit (Core tests).

## Global Constraints

- **Pristine builds: 0 warnings** across shim (SDK + stub), App, Core. Warnings are findings.
- **C ABI struct parity is load-bearing.** `CosCaps` (C in `shim.h`) and its `[StructLayout(LayoutKind.Sequential)]` mirror in `PInvokeShim` must match byte-for-byte on x64. After this plan `CosCaps` = `int green_screen_available; char detail[256]; int eye_contact_available; char ec_detail[256]` = **524 bytes**. `detail`/`ec_detail` are UTF-8.
- **9 C exports stay 9.** No new exports — the second gate rides in the existing `cos_query_capabilities`.
- **Native effects are worker-thread-local** (CUDA/NvAR thread affinity). `cos_set_params` (UI thread) only flips atomics. Status crosses threads via atomics + a leaf-lock mutex never nested under `g_state.mtx`/`g_lifecycleMtx`.
- **Build guards are orthogonal.** `COS_HAS_MAXINE` (green screen, VFX) and `COS_HAS_MAXINE_AR` (eye contact, AR) are independent; the shim must build and Core tests must pass with neither, either, or both. Without a guard the corresponding effect is a passthrough stub.
- **Two-location AR SDK.** `COS_AR_SDK_DIR` = a clone of `https://github.com/NVIDIA-Maxine/Maxine-AR-SDK` (build-time headers `nvar/include` + proxy stub `nvar/src/nvARProxy.cpp`). Runtime DLLs + models = the installer at `%ProgramFiles%\NVIDIA Corporation\NVIDIA AR SDK\` (DLLs in root, models in `…\models`); the proxy auto-resolves this when `g_nvARSDKPath` is null. `COS_AR_RUNTIME_DIR` (optional) overrides the runtime root.
- **Deploy gotcha:** the SDK build and CI stub write the same `native/shim/x64/Debug/CameraOnScreen.Shim.dll`. Build the SDK config **last**, then `-t:Rebuild` the App, else the stub deploys (toggle greyed). Verify deployed DLL: contains `GazeRedirection`, not `AR SDK not built in`.
- Spec: `docs/superpowers/specs/2026-06-21-camera-on-screen-m4-eyecontact-design.md`. Mirror the M3 green-screen pattern (`aigs.{h,cpp}`, capture worker, `Orchestrator`/`MainViewModel`) throughout.

---

### Task 1: Split the capability gate (managed Core)

Extend `ShimCapabilities`, `FakeShim`, and `Orchestrator` so eye contact has its own availability gate independent of green screen. Pure .NET, fully unit-tested with `FakeShim` (no SDK).

**Files:**
- Modify: `src/CameraOnScreen.Core/Native/Contracts.cs:6`
- Modify: `src/CameraOnScreen.Core/Native/FakeShim.cs`
- Modify: `src/CameraOnScreen.Core/Orchestration/Orchestrator.cs`
- Test: `tests/CameraOnScreen.Core.Tests/Orchestration/OrchestratorTests.cs`

**Interfaces:**
- Produces: `ShimCapabilities(bool GreenScreenAvailable, string Detail, bool EyeContactAvailable = false, string EyeContactDetail = "")`; `Orchestrator.EyeContactAvailable` (bool), `Orchestrator.EyeContactDetail` (string); `FakeShim.EyeContactAvailable` (settable bool).
- Consumes: existing `INativeShim.QueryCapabilities()`, `Orchestrator.ApplyParams`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/CameraOnScreen.Core.Tests/Orchestration/OrchestratorTests.cs` (before the final closing `}`):

```csharp
    // --- M4: split eye-contact gate ---

    [Fact]
    public void EyeContact_available_independent_of_greenscreen()
    {
        // Green screen unavailable, eye contact available: GS stripped, EC forwarded.
        var shim = new FakeShim { GreenScreenAvailable = false, EyeContactAvailable = true };
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        orch.ProbeCapabilities();
        Assert.False(orch.EffectsAvailable);
        Assert.True(orch.EyeContactAvailable);
        orch.Start(Requested());
        Assert.False(shim.LastParams!.GreenScreenEnabled);
        Assert.True(shim.LastParams!.EyeContactEnabled);
    }

    [Fact]
    public void EyeContact_unavailable_forces_eye_contact_off()
    {
        // Eye contact unavailable, green screen available: EC stripped, GS forwarded.
        var shim = new FakeShim { GreenScreenAvailable = true, EyeContactAvailable = false };
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        orch.ProbeCapabilities();
        Assert.True(orch.EffectsAvailable);
        Assert.False(orch.EyeContactAvailable);
        orch.Start(Requested());
        Assert.True(shim.LastParams!.GreenScreenEnabled);
        Assert.False(shim.LastParams!.EyeContactEnabled);
    }

    [Fact]
    public void EyeContact_gated_off_before_probe_runs()
    {
        var shim = new FakeShim { GreenScreenAvailable = true, EyeContactAvailable = true };
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        Assert.False(orch.EyeContactAvailable);
        orch.ProbeCapabilities();
        Assert.True(orch.EyeContactAvailable);
    }

    [Fact]
    public void ProbeCapabilities_records_eye_contact_detail()
    {
        var shim = new FakeShim { EyeContactAvailable = false };
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        orch.ProbeCapabilities();
        Assert.Equal("fake: ec unavailable", orch.EyeContactDetail);
    }
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~OrchestratorTests"`
Expected: FAIL — `ShimCapabilities` has no `EyeContactAvailable`; `FakeShim`/`Orchestrator` lack the members (compile errors).

- [ ] **Step 3: Extend `ShimCapabilities`**

In `src/CameraOnScreen.Core/Native/Contracts.cs`, replace line 6:

```csharp
/// <summary>Result of probing the native shim for effect availability. Eye-contact fields default
/// so existing 2-arg call sites keep compiling.</summary>
public sealed record ShimCapabilities(
    bool GreenScreenAvailable, string Detail,
    bool EyeContactAvailable = false, string EyeContactDetail = "");
```

- [ ] **Step 4: Extend `FakeShim`**

In `src/CameraOnScreen.Core/Native/FakeShim.cs`, add the property after `GreenScreenAvailable` (line 7) and update `QueryCapabilities`:

```csharp
    public bool EyeContactAvailable { get; set; }
```

```csharp
    public ShimCapabilities QueryCapabilities() =>
        new(GreenScreenAvailable,
            GreenScreenAvailable ? "fake: available" : "fake: unavailable",
            EyeContactAvailable,
            EyeContactAvailable ? "fake: ec available" : "fake: ec unavailable");
```

- [ ] **Step 5: Split the gate in `Orchestrator`**

In `src/CameraOnScreen.Core/Orchestration/Orchestrator.cs`, add two properties after `CapabilityDetail` (line 31):

```csharp
    /// <summary>True when the shim reports Eye Contact can run. False until <see cref="ProbeCapabilities"/>.</summary>
    public bool EyeContactAvailable { get; private set; }

    /// <summary>Human-readable reason from the eye-contact probe. "Checking…" until probed.</summary>
    public string EyeContactDetail { get; private set; } = "Checking effect availability…";
```

Replace `ProbeCapabilities` (lines 36-41) to record both gates:

```csharp
    public void ProbeCapabilities()
    {
        var caps = _shim.QueryCapabilities();
        EffectsAvailable = caps.GreenScreenAvailable;
        CapabilityDetail = caps.Detail;
        EyeContactAvailable = caps.EyeContactAvailable;
        EyeContactDetail = caps.EyeContactDetail;
    }
```

Replace `ApplyParams` (lines 58-64) so each effect is gated by its own flag:

```csharp
    public void ApplyParams(ShimParams requested)
    {
        var effective = requested with
        {
            GreenScreenEnabled = requested.GreenScreenEnabled && EffectsAvailable,
            EyeContactEnabled = requested.EyeContactEnabled && EyeContactAvailable,
        };
        _shim.SetParams(effective);
    }
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~OrchestratorTests"`
Expected: PASS (all, including the four new tests and the existing gate tests).

- [ ] **Step 7: Commit**

```bash
git add src/CameraOnScreen.Core/Native/Contracts.cs src/CameraOnScreen.Core/Native/FakeShim.cs src/CameraOnScreen.Core/Orchestration/Orchestrator.cs tests/CameraOnScreen.Core.Tests/Orchestration/OrchestratorTests.cs
git commit -m "feat(core): split eye-contact capability gate from green screen"
```

---

### Task 2: VM split gate + probe publish (managed Core)

Surface `EyeContactAvailable`/`EyeContactDetail` as observable VM props mirrored from the orchestrator, published by the async probe. The live-push partial `OnEyeContactEnabledChanged` already exists (line 117) — verify it drives `ApplyLiveParams`.

**Files:**
- Modify: `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs`
- Test: `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`

**Interfaces:**
- Consumes: `Orchestrator.EyeContactAvailable`, `Orchestrator.EyeContactDetail` (Task 1).
- Produces: `MainViewModel.EyeContactAvailable` (bool observable), `MainViewModel.EyeContactDetail` (string observable).

- [ ] **Step 1: Write the failing tests**

Append to `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs` (before the final `}`):

```csharp
    [Fact]
    public async Task ProbeCapabilitiesAsync_publishes_eye_contact_gate_to_vm()
    {
        var shim = new FakeShim { GreenScreenAvailable = false, EyeContactAvailable = true };
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        var vm = new MainViewModel(orch, shim);
        Assert.False(vm.EyeContactAvailable);          // gated off pre-probe
        await vm.ProbeCapabilitiesAsync();
        Assert.True(vm.EyeContactAvailable);
        Assert.False(vm.EffectsAvailable);             // independent of green screen
        Assert.Equal("fake: ec available", vm.EyeContactDetail);
    }

    [Fact]
    public void Toggling_eye_contact_while_running_pushes_params()
    {
        var shim = new FakeShim { GreenScreenAvailable = true, EyeContactAvailable = true };
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        orch.ProbeCapabilities();
        var vm = new MainViewModel(orch, shim);
        vm.StartCommand.Execute(null);                 // running → live push enabled
        vm.EyeContactEnabled = true;
        Assert.True(shim.LastParams!.EyeContactEnabled);
        vm.EyeContactEnabled = false;
        Assert.False(shim.LastParams!.EyeContactEnabled);
    }
```

(If `MainViewModelTests.cs` lacks `using` lines for `CameraOnScreen.Core.Native;`/`CameraOnScreen.Core.Orchestration;`, they are already present from existing tests — confirm before running.)

- [ ] **Step 2: Run the tests to verify they fail**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~MainViewModelTests"`
Expected: FAIL — `MainViewModel` has no `EyeContactAvailable`/`EyeContactDetail`.

- [ ] **Step 3: Add the observable props**

In `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs`, add after `capabilityDetail` (line 60):

```csharp
    [ObservableProperty] private bool eyeContactAvailable;
    [ObservableProperty] private string eyeContactDetail = "Checking effect availability…";
```

In the constructor, after line 31 (`CapabilityDetail = orchestrator.CapabilityDetail;`), mirror the eye-contact gate:

```csharp
        EyeContactAvailable = orchestrator.EyeContactAvailable;
        EyeContactDetail = orchestrator.EyeContactDetail;
```

In `ProbeCapabilitiesAsync`, after line 47 (`CapabilityDetail = _orchestrator.CapabilityDetail;`), publish the eye-contact result:

```csharp
        EyeContactAvailable = _orchestrator.EyeContactAvailable;
        EyeContactDetail = _orchestrator.EyeContactDetail;
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~MainViewModelTests"`
Expected: PASS.

- [ ] **Step 5: Run the full Core suite**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
Expected: PASS, 0 warnings.

- [ ] **Step 6: Commit**

```bash
git add src/CameraOnScreen.Core/ViewModels/MainViewModel.cs tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs
git commit -m "feat(core): publish eye-contact gate to MainViewModel"
```

---

### Task 3: Native `EyeContact` wrapper (eyecontact.h/.cpp) + vcxproj

Create the NvAR GazeRedirection wrapper (real behind `COS_HAS_MAXINE_AR`, else passthrough stub) and wire the AR SDK into the vcxproj. Mirrors `aigs.{h,cpp}` exactly. **Build-verified** (no unit test — native, guarded). Cross-check every NvAR call against the on-disk `$(COS_AR_SDK_DIR)\samples\GazeRedirect\GazeEngine.cpp`.

**Files:**
- Create: `native/shim/eyecontact.h`
- Create: `native/shim/eyecontact.cpp`
- Modify: `native/shim/shim.vcxproj`

**Interfaces:**
- Produces: class `EyeContact` with `static bool Probe(std::string&)`, `bool Start()`, `void Stop()`, `bool ProcessFrame(uint8_t* bgra, int w, int h)`, `bool IsReady() const`, `const std::string& LastError() const`.

- [ ] **Step 1: Create `native/shim/eyecontact.h`**

```cpp
#pragma once
#include <cstdint>
#include <string>

// Wraps the Maxine AR SDK GazeRedirection (Eye Contact) effect. CPU-copy: a tightly-
// packed BGRA frame is uploaded to the GPU (converted to BGR), gaze-redirected, and the
// redirected image is downloaded back into the same BGRA buffer (alpha forced opaque).
// All methods are no-throw; failure is reported via IsReady() + LastError(). When built
// without the SDK (COS_HAS_MAXINE_AR undefined) this is a stub that is never ready, so the
// shim degrades to passthrough. Mirrors the Aigs (green-screen) wrapper.
class EyeContact {
public:
    EyeContact();
    ~EyeContact();

    // One-shot probe: can the AR SDK load and the GazeRedirection feature create+load?
    // Does not retain the feature. Fills 'detail'. Call before starting capture.
    static bool Probe(std::string& detail);

    // Create the feature + CUDA stream and configure+load it. Call on the capture worker
    // thread (NvAR/CUDA thread affinity). Returns true on success; on failure IsReady()==false.
    bool Start();

    // Destroy the feature, stream, and images. Call on the worker thread.
    void Stop();

    // Gaze-redirect a tightly-packed BGRA buffer (width*height*4) in place. Returns true if
    // applied; false leaves 'bgra' untouched. Eye-size sensitivity / look-away params are
    // fixed at SDK defaults for M4 (toggle-only UI); the ABI doubles are reserved/ignored.
    bool ProcessFrame(uint8_t* bgra, int width, int height);

    bool IsReady() const { return ready_; }
    const std::string& LastError() const { return lastError_; }

private:
    bool ready_ = false;
    std::string lastError_;
    void* impl_ = nullptr; // opaque; real fields live in eyecontact.cpp behind COS_HAS_MAXINE_AR
};
```

- [ ] **Step 2: Create `native/shim/eyecontact.cpp`**

```cpp
#include "eyecontact.h"

#ifdef COS_HAS_MAXINE_AR
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include "nvCVStatus.h"
#include "nvCVImage.h"
#include "nvAR.h"
#include "nvAR_defs.h"

// The AR proxy stub (nvARProxy.cpp) declares this extern; define it here. When non-null it
// is SetDllDirectory'd before LoadLibrary(nvARPose.dll). When null the proxy auto-falls back
// to "%ProgramFiles%\NVIDIA Corporation\NVIDIA AR SDK\".
char* g_nvARSDKPath = nullptr;

namespace {
// Resolves the AR runtime root (holds nvARPose.dll) and its models dir. Prefers
// COS_AR_RUNTIME_DIR; otherwise defaults to the Program Files install.
bool ResolveArPaths(std::string& runtimeDir, std::string& modelDir, std::string& err) {
    char buf[1024] = {0};
    DWORD n = GetEnvironmentVariableA("COS_AR_RUNTIME_DIR", buf, sizeof(buf));
    std::string root;
    if (n > 0 && n < sizeof(buf)) {
        root.assign(buf, n);
    } else {
        char pf[1024] = {0};
        DWORD m = GetEnvironmentVariableA("ProgramFiles", pf, sizeof(pf));
        if (m == 0 || m >= sizeof(pf)) { err = "ProgramFiles not set"; return false; }
        root.assign(pf, m);
        root += "\\NVIDIA Corporation\\NVIDIA AR SDK";
    }
    if (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
    runtimeDir = root;
    modelDir   = root + "\\models";
    return true;
}

// Points the AR proxy at the runtime root so nvARPose.dll + deps are found. Idempotent;
// s_root must outlive every NvAR call.
void PointProxyAt(const std::string& runtimeDir) {
    static std::string s_root;
    s_root = runtimeDir;
    g_nvARSDKPath = const_cast<char*>(s_root.c_str());
}

constexpr unsigned kNumLandmarks = 126;       // GazeEngine LANDMARKS_INFO[1].numPoints
constexpr unsigned kNumGazeOutLandmarks = 12; // GazeEngine num_output_landmarks
constexpr unsigned kEyeSizeSensitivity = 3;   // GazeEngine default
} // namespace

// Real per-effect state, hidden behind the opaque impl_ pointer.
struct EyeContactImpl {
    NvAR_FeatureHandle handle = nullptr;
    CUstream stream = nullptr;
    std::string modelDir;
    NvCVImage inGpu{};   // BGR u8 chunky, GPU  (gaze input)
    NvCVImage outGpu{};  // BGR u8 chunky, GPU  (gaze redirected output)
    NvCVImage tmp{};     // staging for NvCVImage_Transfer (managed by the SDK)
    int w = 0, h = 0;
    bool bound = false;
    // Output buffers the SDK requires bound even though we ignore them.
    std::vector<NvAR_Point2f> landmarks;
    std::vector<NvAR_Point2f> gazeOutLandmarks;
    std::vector<float> landmarksConfidence;
    float gazeVector[2] = {0.f};
    float headTranslation[3] = {0.f};
    NvAR_Point3f gazeDirection[2] = {{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}};
    NvAR_Quaternion headPose{};
    std::vector<NvAR_Rect> bboxData;
    NvAR_BBoxes bboxes{};
};

EyeContact::EyeContact() = default;
EyeContact::~EyeContact() { Stop(); }

bool EyeContact::Probe(std::string& detail) {
    std::string runtimeDir, modelDir, err;
    if (!ResolveArPaths(runtimeDir, modelDir, err)) { detail = err; return false; }
    PointProxyAt(runtimeDir);

    CUstream stream = nullptr;
    if (NvAR_CudaStreamCreate(&stream) != NVCV_SUCCESS) {
        detail = "NvAR_CudaStreamCreate failed (CUDA/driver?)";
        return false;
    }
    NvAR_FeatureHandle h = nullptr;
    if (NvAR_Create(NvAR_Feature_GazeRedirection, &h) != NVCV_SUCCESS || !h) {
        NvAR_CudaStreamDestroy(stream);
        detail = "NvAR_Create(GazeRedirection) failed (DLL/SDK load?)";
        return false;
    }
    NvAR_SetString(h, NvAR_Parameter_Config(ModelDir), modelDir.c_str());
    NvAR_SetU32(h, NvAR_Parameter_Config(Landmarks_Size), kNumLandmarks);
    NvAR_SetU32(h, NvAR_Parameter_Config(Temporal), 0xFFFFFFFFu);
    NvAR_SetU32(h, NvAR_Parameter_Config(GazeRedirect), 1u);
    NvAR_SetCudaStream(h, NvAR_Parameter_Config(CUDAStream), stream);
    NvAR_SetU32(h, NvAR_Parameter_Config(EyeSizeSensitivity), kEyeSizeSensitivity);
    NvCV_Status load = NvAR_Load(h);
    NvAR_Destroy(h);
    NvAR_CudaStreamDestroy(stream);
    if (load != NVCV_SUCCESS) {
        detail = "NvAR_Load(GazeRedirection) failed (models missing or GPU incompatible?)";
        return false;
    }
    detail = "GazeRedirection available";
    return true;
}

bool EyeContact::Start() {
    Stop();
    EyeContactImpl* impl = new (std::nothrow) EyeContactImpl();
    if (!impl) { lastError_ = "out of memory"; return false; }

    std::string runtimeDir, err;
    if (!ResolveArPaths(runtimeDir, impl->modelDir, err)) { lastError_ = err; delete impl; return false; }
    PointProxyAt(runtimeDir);

    if (NvAR_CudaStreamCreate(&impl->stream) != NVCV_SUCCESS) {
        lastError_ = "NvAR_CudaStreamCreate failed"; delete impl; return false;
    }
    if (NvAR_Create(NvAR_Feature_GazeRedirection, &impl->handle) != NVCV_SUCCESS || !impl->handle) {
        lastError_ = "NvAR_Create failed"; delete impl; return false;
    }
    NvAR_SetString(impl->handle, NvAR_Parameter_Config(ModelDir), impl->modelDir.c_str());
    NvAR_SetU32(impl->handle, NvAR_Parameter_Config(Landmarks_Size), kNumLandmarks);
    NvAR_SetU32(impl->handle, NvAR_Parameter_Config(Temporal), 0xFFFFFFFFu);
    NvAR_SetU32(impl->handle, NvAR_Parameter_Config(GazeRedirect), 1u);
    NvAR_SetCudaStream(impl->handle, NvAR_Parameter_Config(CUDAStream), impl->stream);
    NvAR_SetU32(impl->handle, NvAR_Parameter_Config(EyeSizeSensitivity), kEyeSizeSensitivity);
    if (NvAR_Load(impl->handle) != NVCV_SUCCESS) {
        lastError_ = "NvAR_Load failed";
        NvAR_Destroy(impl->handle);
        NvAR_CudaStreamDestroy(impl->stream);
        delete impl; return false;
    }
    impl_ = impl;
    ready_ = true;
    lastError_.clear();
    return true;
}

void EyeContact::Stop() {
    auto* impl = static_cast<EyeContactImpl*>(impl_);
    if (!impl) { ready_ = false; return; }
    NvCVImage_Dealloc(&impl->inGpu);
    NvCVImage_Dealloc(&impl->outGpu);
    NvCVImage_Dealloc(&impl->tmp);
    if (impl->handle) NvAR_Destroy(impl->handle);
    if (impl->stream) NvAR_CudaStreamDestroy(impl->stream);
    delete impl;
    impl_ = nullptr;
    ready_ = false;
}

namespace {
// (Re)allocates GPU images and binds all input/output params for a w*h frame. Returns
// NVCV_SUCCESS on success. Called on first frame or when the size changes. The SDK requires
// every output below to be bound even though the shim consumes only the output image.
NvCV_Status BindIO(EyeContactImpl* impl, int w, int h) {
    if (impl->bound && impl->w == w && impl->h == h) return NVCV_SUCCESS;

    NvCVImage_Dealloc(&impl->inGpu);
    NvCVImage_Dealloc(&impl->outGpu);

    NvCV_Status s;
    s = NvCVImage_Alloc(&impl->inGpu,  w, h, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;
    s = NvCVImage_Alloc(&impl->outGpu, w, h, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1); if (s != NVCV_SUCCESS) return s;

    s = NvAR_SetObject(impl->handle, NvAR_Parameter_Input(Image),  &impl->inGpu,  sizeof(NvCVImage)); if (s != NVCV_SUCCESS) return s;
    s = NvAR_SetObject(impl->handle, NvAR_Parameter_Output(Image), &impl->outGpu, sizeof(NvCVImage)); if (s != NVCV_SUCCESS) return s;
    s = NvAR_SetS32(impl->handle, NvAR_Parameter_Input(Width),  w); if (s != NVCV_SUCCESS) return s;
    s = NvAR_SetS32(impl->handle, NvAR_Parameter_Input(Height), h); if (s != NVCV_SUCCESS) return s;

    unsigned kpts = kNumLandmarks;
    NvAR_GetU32(impl->handle, NvAR_Parameter_Config(Landmarks_Size), &kpts);

    impl->landmarks.assign(kpts, {0.f, 0.f});
    s = NvAR_SetObject(impl->handle, NvAR_Parameter_Output(Landmarks), impl->landmarks.data(), sizeof(NvAR_Point2f)); if (s != NVCV_SUCCESS) return s;

    impl->gazeOutLandmarks.assign(kNumGazeOutLandmarks, {0.f, 0.f});
    s = NvAR_SetObject(impl->handle, NvAR_Parameter_Output(GazeOutputLandmarks), impl->gazeOutLandmarks.data(), sizeof(NvAR_Point2f)); if (s != NVCV_SUCCESS) return s;

    impl->landmarksConfidence.assign(kpts, 0.f);
    s = NvAR_SetF32Array(impl->handle, NvAR_Parameter_Output(LandmarksConfidence), impl->landmarksConfidence.data(), kpts); if (s != NVCV_SUCCESS) return s;

    s = NvAR_SetF32Array(impl->handle, NvAR_Parameter_Output(OutputGazeVector), impl->gazeVector, 2); if (s != NVCV_SUCCESS) return s;
    s = NvAR_SetF32Array(impl->handle, NvAR_Parameter_Output(OutputHeadTranslation), impl->headTranslation, 3); if (s != NVCV_SUCCESS) return s;
    s = NvAR_SetObject(impl->handle, NvAR_Parameter_Output(HeadPose), &impl->headPose, sizeof(NvAR_Quaternion)); if (s != NVCV_SUCCESS) return s;
    s = NvAR_SetObject(impl->handle, NvAR_Parameter_Output(GazeDirection), &impl->gazeDirection, sizeof(NvAR_Point3f)); if (s != NVCV_SUCCESS) return s;

    impl->bboxData.assign(1, {0.f, 0.f, 0.f, 0.f});
    impl->bboxes.boxes = impl->bboxData.data();
    impl->bboxes.max_boxes = 1;
    impl->bboxes.num_boxes = 1;
    s = NvAR_SetObject(impl->handle, NvAR_Parameter_Output(BoundingBoxes), &impl->bboxes, sizeof(NvAR_BBoxes)); if (s != NVCV_SUCCESS) return s;

    impl->w = w; impl->h = h; impl->bound = true;
    return NVCV_SUCCESS;
}

// SEAM 1 (upload): CPU BGRA -> GPU BGR, via the SDK-managed tmp staging buffer.
NvCV_Status Upload(EyeContactImpl* impl, uint8_t* bgra, int w, int h) {
    NvCVImage src{};
    NvCVImage_Init(&src, w, h, w * 4, bgra, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
    return NvCVImage_Transfer(&src, &impl->inGpu, 1.0f, impl->stream, &impl->tmp);
}

// SEAM 3 (download/composite): GPU BGR redirected output -> CPU BGRA in place. The BGR->BGRA
// transfer sets alpha; force it opaque afterward so passthrough alpha semantics hold (green
// screen, if enabled, overwrites alpha with the matte downstream).
NvCV_Status Download(EyeContactImpl* impl, uint8_t* bgra, int w, int h) {
    NvCVImage dst{};
    NvCVImage_Init(&dst, w, h, w * 4, bgra, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
    NvCV_Status s = NvCVImage_Transfer(&impl->outGpu, &dst, 1.0f, impl->stream, &impl->tmp);
    if (s != NVCV_SUCCESS) return s;
    const int stride = w * 4;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = bgra + static_cast<size_t>(stride) * y;
        for (int x = 3; x < stride; x += 4) row[x] = 0xFF;
    }
    return NVCV_SUCCESS;
}
} // namespace

bool EyeContact::ProcessFrame(uint8_t* bgra, int w, int h) {
    auto* impl = static_cast<EyeContactImpl*>(impl_);
    if (!impl || !impl->handle || !bgra || w <= 0 || h <= 0) return false;

    if (BindIO(impl, w, h) != NVCV_SUCCESS) { lastError_ = "BindIO failed"; ready_ = false; return false; }
    if (Upload(impl, bgra, w, h) != NVCV_SUCCESS) { lastError_ = "Upload (Transfer) failed"; return false; }
    if (NvAR_Run(impl->handle, 0) != NVCV_SUCCESS) { lastError_ = "NvAR_Run failed"; return false; }
    if (Download(impl, bgra, w, h) != NVCV_SUCCESS) { lastError_ = "Download (Transfer) failed"; return false; }
    if (NvAR_CudaStreamSynchronize(impl->stream) != NVCV_SUCCESS) { lastError_ = "stream sync failed"; return false; }
    return true;
}

#else
// ---- Passthrough stub: built when the AR SDK is not configured. ----
EyeContact::EyeContact() = default;
EyeContact::~EyeContact() = default;
bool EyeContact::Probe(std::string& detail) { detail = "AR SDK not built in"; return false; }
bool EyeContact::Start() { lastError_ = "AR SDK not built in"; ready_ = false; return false; }
void EyeContact::Stop() { ready_ = false; }
bool EyeContact::ProcessFrame(uint8_t*, int, int) { return false; }
#endif
```

> **NvAR API cross-check (do this before building the real config):** open
> `$(COS_AR_SDK_DIR)\samples\GazeRedirect\GazeEngine.cpp` and confirm the exact spellings of
> `NvAR_Run` (arity: the sample calls `NvAR_Run(handle)` — if your header's signature is
> `NvAR_Run(handle, flags)`, keep the `, 0`; otherwise drop it), `NvAR_CudaStreamSynchronize`
> (if absent, replace with `cudaStreamSynchronize`/omit — the SDK runs synchronously per
> `NvAR_Run`), and every `NvAR_Parameter_*` macro used above. The names above are taken
> verbatim from the sample; reconcile any header drift and keep the build at 0 warnings.

- [ ] **Step 3: Add a `CosArSdkDir` property to the vcxproj**

In `native/shim/shim.vcxproj`, after the `CosVfxSdkDir` PropertyGroup (lines 21-24), add:

```xml
  <PropertyGroup>
    <!-- AR SDK dir = a clone of NVIDIA-Maxine/Maxine-AR-SDK (headers + proxy). Empty => stub. -->
    <CosArSdkDir Condition="'$(CosArSdkDir)' == ''">$(COS_AR_SDK_DIR)</CosArSdkDir>
  </PropertyGroup>
```

- [ ] **Step 4: Add the AR guard + include dir**

In `native/shim/shim.vcxproj`, after the VFX `ItemDefinitionGroup Condition="'$(CosVfxSdkDir)' != ''"` block (lines 71-76), add:

```xml
  <ItemDefinitionGroup Condition="'$(CosArSdkDir)' != ''">
    <ClCompile>
      <PreprocessorDefinitions>COS_HAS_MAXINE_AR;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <AdditionalIncludeDirectories>$(CosArSdkDir)\nvar\include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
  </ItemDefinitionGroup>
```

- [ ] **Step 5: Add `eyecontact.cpp`/`.h` to the build + AR proxy stubs**

In `native/shim/shim.vcxproj`, add `eyecontact.cpp` to the main `<ClCompile>` ItemGroup (after `aigs.cpp`, line 81):

```xml
    <ClCompile Include="eyecontact.cpp" />
```

Add `eyecontact.h` to the `<ClInclude>` ItemGroup (after `aigs.h`, line 93):

```xml
    <ClInclude Include="eyecontact.h" />
```

After the VFX proxy-stub ItemGroup (lines 85-88), add the AR proxy stub. The AR `nvARProxy.cpp` is always compiled when the AR SDK is set. The AR tree's `nvCVImageProxy.cpp` is compiled **only when the VFX SDK is NOT set** (the VFX build already compiles one copy — two copies are a duplicate-symbol link error):

```xml
  <!-- AR proxy stub provides NvAR_* by LoadLibrary'ing nvARPose.dll (no import .lib). -->
  <ItemGroup Condition="'$(CosArSdkDir)' != ''">
    <ClCompile Include="$(CosArSdkDir)\nvar\src\nvARProxy.cpp" />
  </ItemGroup>
  <!-- NvCVImage_* proxy: exactly one copy in the build. VFX provides it when present;
       otherwise pull it from the AR tree. -->
  <ItemGroup Condition="'$(CosArSdkDir)' != '' And '$(CosVfxSdkDir)' == ''">
    <ClCompile Include="$(CosArSdkDir)\nvar\src\nvCVImageProxy.cpp" />
  </ItemGroup>
```

- [ ] **Step 6: Build the stub (no AR SDK) — verify it compiles**

Run from PowerShell (NOT the Bash tool — Git Bash mangles MSBuild `/p:` switches):

```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:CosArSdkDir= /p:CosVfxSdkDir=
```
Expected: `Build succeeded. 0 Warning(s) 0 Error(s)`. The stub path compiles (`AR SDK not built in`).

- [ ] **Step 7: Build the real path (AR + VFX SDKs) — verify it compiles**

Set both SDK dirs and build (VFX last is not required for compile here; both guards on):

```powershell
$env:COS_VFX_SDK_DIR = "C:\Users\opari\OneDrive\Desktop\claude-code\VideoFX"
$env:COS_AR_SDK_DIR  = "<path to your Maxine-AR-SDK clone>"
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
```
Expected: `Build succeeded. 0 Warning(s) 0 Error(s)`. If NvAR symbol/param errors appear, reconcile against the on-disk `GazeEngine.cpp` (Step 2 cross-check note) and rebuild.

- [ ] **Step 8: Commit**

```bash
git add native/shim/eyecontact.h native/shim/eyecontact.cpp native/shim/shim.vcxproj
git commit -m "feat(shim): NvAR GazeRedirection (Eye Contact) wrapper + AR SDK build wiring"
```

---

### Task 4: Extend `CosCaps` ABI + probe the eye-contact gate

Grow `CosCaps` with the second gate (byte-parity both sides) and fill it from `EyeContact::Probe` in `cos_query_capabilities`. **Build- + parity-verified.**

**Files:**
- Modify: `native/shim/shim.h:28-31`
- Modify: `native/shim/shim.cpp:96-107`
- Modify: `src/CameraOnScreen.App/Native/PInvokeShim.cs:29-35,101-107`

**Interfaces:**
- Consumes: `EyeContact::Probe` (Task 3), `ShimCapabilities` 4-arg ctor (Task 1).
- Produces: `CosCaps { int green_screen_available; char detail[256]; int eye_contact_available; char ec_detail[256]; }` (524 bytes x64) on both sides.

- [ ] **Step 1: Extend the C struct in `shim.h`**

Replace the `CosCaps` struct (lines 28-31):

```c
typedef struct {
    int  green_screen_available; // 1 if Maxine GreenScreen can run, else 0
    char detail[256];            // green-screen status/error (UTF-8, NUL-terminated)
    int  eye_contact_available;  // 1 if Maxine GazeRedirection can run, else 0
    char ec_detail[256];         // eye-contact status/error (UTF-8, NUL-terminated)
} CosCaps;
```

- [ ] **Step 2: Fill both gates in `cos_query_capabilities`**

In `native/shim/shim.cpp`, add the include near the top (after `#include "aigs.h"`, line 4):

```cpp
#include "eyecontact.h"
```

Replace `cos_query_capabilities` (lines 96-107):

```cpp
COS_API int cos_query_capabilities(CosCaps* out) {
    if (!out) return 0;
    std::memset(out, 0, sizeof(*out));

    std::string gsDetail;
    bool gsOk = Aigs::Probe(gsDetail);
    out->green_screen_available = gsOk ? 1 : 0;
    size_t gn = gsDetail.size() < 255 ? gsDetail.size() : 255;
    std::memcpy(out->detail, gsDetail.data(), gn);
    out->detail[gn] = '\0';

    std::string ecDetail;
    bool ecOk = EyeContact::Probe(ecDetail);
    out->eye_contact_available = ecOk ? 1 : 0;
    size_t en = ecDetail.size() < 255 ? ecDetail.size() : 255;
    std::memcpy(out->ec_detail, ecDetail.data(), en);
    out->ec_detail[en] = '\0';

    // Return 1 if either effect is available (the managed side reads the per-gate ints).
    return (gsOk || ecOk) ? 1 : 0;
}
```

- [ ] **Step 3: Mirror the struct in `PInvokeShim`**

In `src/CameraOnScreen.App/Native/PInvokeShim.cs`, replace the `CosCaps` struct (lines 29-35):

```csharp
    [StructLayout(LayoutKind.Sequential)]
    private struct CosCaps
    {
        public int GreenScreenAvailable;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)]
        public byte[] Detail;
        public int EyeContactAvailable;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)]
        public byte[] EcDetail;
    }
```

Replace `QueryCapabilities` (lines 101-107):

```csharp
    public ShimCapabilities QueryCapabilities()
    {
        var caps = new CosCaps { Detail = new byte[256], EcDetail = new byte[256] };
        cos_query_capabilities(ref caps);
        return new ShimCapabilities(
            caps.GreenScreenAvailable != 0, ReadUtf8(caps.Detail, 0, 256),
            caps.EyeContactAvailable != 0, ReadUtf8(caps.EcDetail, 0, 256));
    }
```

- [ ] **Step 4: Rebuild the shim (real path) and verify exports + parity**

```powershell
$env:COS_VFX_SDK_DIR = "C:\Users\opari\OneDrive\Desktop\claude-code\VideoFX"
$env:COS_AR_SDK_DIR  = "<path to your Maxine-AR-SDK clone>"
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
& "$(((Get-ChildItem 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC' -Directory | Select-Object -Last 1).FullName))\bin\Hostx64\x64\dumpbin.exe" /exports native/shim/x64/Debug/CameraOnScreen.Shim.dll | Select-String "cos_"
```
Expected: build 0/0; `dumpbin` lists exactly the 9 `cos_*` exports (unchanged count).

- [ ] **Step 5: Build the App (deploys the shim) — verify managed parity compiles**

```powershell
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild
```
Expected: `Build succeeded. 0 Warning(s)`. (Confirms the `CosCaps` mirror marshals.)

- [ ] **Step 6: Commit**

```bash
git add native/shim/shim.h native/shim/shim.cpp src/CameraOnScreen.App/Native/PInvokeShim.cs
git commit -m "feat(shim): add eye-contact gate to CosCaps + cos_query_capabilities"
```

---

### Task 5: Wire `EyeContact` into the capture worker + status

Run eye contact in the worker before green screen; expose enable/active/error via the `Capture` API; route the param + status through `cos_set_params`/`cos_get_status`. **Build- + runtime-smoke-verified.**

**Files:**
- Modify: `native/shim/capture.h:32-39`
- Modify: `native/shim/capture.cpp`
- Modify: `native/shim/shim.cpp:59-82`

**Interfaces:**
- Consumes: `EyeContact` (Task 3).
- Produces: `Capture::SetEyeContact(bool)`, `Capture::EyeContactActive() const`, `Capture::EyeContactError() const`.

- [ ] **Step 1: Declare the eye-contact API in `capture.h`**

In `native/shim/capture.h`, after `SetMatteParams` (line 35) and the green-screen status methods (lines 37-38), add:

```cpp
    // Toggles Eye Contact for subsequent frames. Thread-safe; the worker owns the object.
    void SetEyeContact(bool enabled);
    bool EyeContactActive() const;        // true only while gaze redirection is transforming frames
    std::string EyeContactError() const;  // empty when none
```

- [ ] **Step 2: Add worker/UI shared state in `capture.cpp`**

In `native/shim/capture.cpp`, add the include (after `#include "aigs.h"`, line 2):

```cpp
#include "eyecontact.h"
```

In `struct CaptureState` (after the green-screen members, line 41), add the eye-contact mirror:

```cpp
    std::atomic<bool>     eyeContactEnabled{false}; // set by UI thread, read by worker
    std::atomic<bool>     eyeContactActive{false};  // set by worker
    std::mutex            ecErrMtx;                  // leaf lock, never nested under mtx/lifecycle
    std::string           ecError;                   // guarded by ecErrMtx
```

- [ ] **Step 3: Run eye contact in the worker before green screen**

In `WorkerLoop`, after `Aigs aigs;` (line 251), add:

```cpp
    EyeContact eyeContact;
```

Inside `if (CopyFrame(...))`, **before** the existing green-screen `const bool want = ...` block (line 293), insert the eye-contact block (it transforms `scratch` first; green screen then runs on the redirected frame):

```cpp
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
```

After the loop, tear down eye contact alongside AIGS — change `aigs.Stop();` (line 339) to:

```cpp
    eyeContact.Stop();
    aigs.Stop();
```

- [ ] **Step 4: Reset eye-contact state on Stop + implement the accessors**

In `Capture::Stop()`, after the `greenScreenActive` reset (line 369), add:

```cpp
    g_state.eyeContactActive.store(false, std::memory_order_release);
```

And in the same method, after clearing `gsError` (the existing `g_state.gsError.clear();`, line 372), add (the `ecErrMtx` is a separate leaf lock — take it after, not nested with gsErrMtx held; close the gsErr scope first or use a distinct block):

```cpp
    {
        std::lock_guard<std::mutex> e(g_state.ecErrMtx);
        g_state.ecError.clear();
    }
```

At the end of the file (near the other accessor definitions, after `GreenScreenError()`, line 401), add:

```cpp
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
```

- [ ] **Step 5: Route param + status through the shim**

In `native/shim/shim.cpp`, in `cos_set_params` (after `g_capture.SetMatteParams(...)`, line 64), add:

```cpp
    g_capture.SetEyeContact(p->eye_contact_enabled != 0);
```

In `cos_get_status`, set the active flag and prefer a green-screen error then an eye-contact error (single `error` field). Replace the body from line 75 (`out->green_screen_active = ...`) through the error copy (line 81):

```cpp
    out->green_screen_active = g_capture.GreenScreenActive() ? 1 : 0;
    out->eye_contact_active = g_capture.EyeContactActive() ? 1 : 0;
    std::string err = g_capture.GreenScreenError();
    if (err.empty()) err = g_capture.EyeContactError();
    if (!err.empty()) {
        size_t n = err.size() < 255 ? err.size() : 255;
        std::memcpy(out->error, err.data(), n);
        out->error[n] = '\0';
    }
```

- [ ] **Step 6: Rebuild the shim (real path, AR + VFX) and the App**

```powershell
$env:COS_VFX_SDK_DIR = "C:\Users\opari\OneDrive\Desktop\claude-code\VideoFX"
$env:COS_AR_SDK_DIR  = "<path to your Maxine-AR-SDK clone>"
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild
```
Expected: both 0/0. Verify the deployed DLL is the real one:

```powershell
& "C:\Program Files\Git\usr\bin\grep.exe" -a "GazeRedirection" src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.Shim.dll | Out-Null; "GazeRedirection present: $($LASTEXITCODE -eq 0)"
& "C:\Program Files\Git\usr\bin\grep.exe" -a "AR SDK not built in" src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.Shim.dll | Out-Null; "stub string present (want False): $($LASTEXITCODE -eq 0)"
```
Expected: `GazeRedirection present: True`, `stub string present (want False): False`.

- [ ] **Step 7: Commit**

```bash
git add native/shim/capture.h native/shim/capture.cpp native/shim/shim.cpp
git commit -m "feat(shim): run Eye Contact in capture worker before green screen"
```

---

### Task 6: Panel UI — eye-contact toggle gate + reason note

Bind the existing Eye Contact toggle to `EyeContactAvailable` and add its own disabled-state note bound to `EyeContactDetail`. **Build-verified** (XAML/window chrome — no unit test).

**Files:**
- Modify: `src/CameraOnScreen.App/MainWindow.xaml:28-32`
- Modify: `src/CameraOnScreen.App/MainWindow.xaml.cs`

**Interfaces:**
- Consumes: `MainViewModel.EyeContactAvailable`, `MainViewModel.EyeContactDetail` (Task 2).
- Produces: `MainWindow.EyeContactNotAvailableVisibility`.

- [ ] **Step 1: Bind the toggle + add the note in XAML**

In `src/CameraOnScreen.App/MainWindow.xaml`, replace the Eye Contact toggle (lines 28-29) so it gates on its own flag, and add a note after it:

```xml
            <ToggleSwitch Header="Eye Contact" IsEnabled="{x:Bind Vm.EyeContactAvailable, Mode=OneWay}"
                          IsOn="{x:Bind Vm.EyeContactEnabled, Mode=TwoWay}"/>
            <TextBlock Text="{x:Bind Vm.EyeContactDetail, Mode=OneWay}"
                       Visibility="{x:Bind EyeContactNotAvailableVisibility, Mode=OneWay}"
                       Foreground="{ThemeResource SystemFillColorCautionBrush}"/>
```

(The existing green-screen note at lines 30-32 stays — it remains bound to `NotAvailableVisibility`/`CapabilityDetail`.)

- [ ] **Step 2: Add the visibility property + change-notification in code-behind**

In `src/CameraOnScreen.App/MainWindow.xaml.cs`, add the property after `NotAvailableVisibility` (line 124-125):

```csharp
    public Visibility EyeContactNotAvailableVisibility =>
        Vm.EyeContactAvailable ? Visibility.Collapsed : Visibility.Visible;
```

In `OnVmPropertyChanged`, add a branch alongside the `EffectsAvailable` one (after line 137):

```csharp
        else if (e.PropertyName == nameof(MainViewModel.EyeContactAvailable))
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(EyeContactNotAvailableVisibility)));
```

- [ ] **Step 3: Build the App**

```powershell
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild
```
Expected: `Build succeeded. 0 Warning(s)`.

- [ ] **Step 4: Commit**

```bash
git add src/CameraOnScreen.App/MainWindow.xaml src/CameraOnScreen.App/MainWindow.xaml.cs
git commit -m "feat(app): gate Eye Contact toggle on its own probe + reason note"
```

---

### Task 7: Integration, deploy verification, and visual gate

Full clean build in the correct order, deploy verification, and the human visual/perf gate on the RTX 3090. **No code** — verification + documentation.

**Files:**
- Modify: `docs/superpowers/verification/2026-06-20-recorder-capture.md` (add an M4 section)

- [ ] **Step 1: Clean build in deploy order (SDK config LAST)**

```powershell
$env:COS_VFX_SDK_DIR = "C:\Users\opari\OneDrive\Desktop\claude-code\VideoFX"
$env:COS_AR_SDK_DIR  = "<path to your Maxine-AR-SDK clone>"
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64 /t:Rebuild
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj
```
Expected: shim 0/0, App 0/0, all Core tests pass.

- [ ] **Step 2: Confirm the real shim is deployed**

```powershell
& "C:\Program Files\Git\usr\bin\grep.exe" -a "GazeRedirection" src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.Shim.dll | Out-Null; "GazeRedirection: $($LASTEXITCODE -eq 0)"
```
Expected: `True`.

- [ ] **Step 3: Run the app with the AR runtime available**

For a default Program Files AR install no env var is needed (the proxy auto-resolves). `COS_VFX_SDK_DIR` is still needed for green screen.

```powershell
$env:COS_VFX_SDK_DIR = "C:\Users\opari\OneDrive\Desktop\claude-code\VideoFX"
src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.App.exe
```

- [ ] **Step 4: Human visual + perf gate (RTX 3090)**

Confirm and record:
- Eye Contact toggle is **enabled** (probe found GazeRedirection); the note is hidden.
- Toggling Eye Contact ON redirects gaze toward the camera on screen, in real time; OFF restores it (validates the live param push).
- **Both effects ON** simultaneously: gaze-redirected **and** background-removed.
- A recorder (NVIDIA ShadowPlay / DWM-hardware path) captures the overlay correctly (GDI shows black by design).
- **Perf:** the overlay holds ~30 fps with both Maxine pipelines on. If it cannot, note the measured rate (informs a possible future mutually-exclusive fallback — out of scope here).
- **Beta quality:** note the head-pose range where gaze redirect disengages / looks uncanny.

- [ ] **Step 5: Record results + commit**

Add an "M4 — Eye Contact" section to `docs/superpowers/verification/2026-06-20-recorder-capture.md` with the observations above (dated 2026-06-21+). Then:

```bash
git add docs/superpowers/verification/2026-06-20-recorder-capture.md
git commit -m "docs(verification): M4 Eye Contact visual + perf gate results"
```

---

## Self-Review

- **Spec coverage:** Architecture/pipeline → Tasks 3, 5. C-ABI (CosCaps grows, no CosParams change) → Tasks 1 (managed mirror), 4 (native + P/Invoke). Build guard + two-location SDK + proxy de-dup → Task 3. SDK discovery (`COS_AR_SDK_DIR` build, `COS_AR_RUNTIME_DIR`/Program Files runtime) → Task 3. VM/UI split gate → Tasks 2, 6. Tests → Tasks 1, 2. Build verification + deploy gotcha → Tasks 4, 5, 7. Visual + perf gate → Task 7. Deferred (multi-GPU, app-relative, license) → spec only, no task (correct).
- **Type consistency:** `ShimCapabilities(bool, string, bool=false, string="")`; `EyeContactAvailable`/`EyeContactDetail` on `Orchestrator` + `MainViewModel`; `CosCaps` fields `eye_contact_available`/`ec_detail` (C) ↔ `EyeContactAvailable`/`EcDetail` (P/Invoke); `EyeContact::{Probe,Start,Stop,ProcessFrame,IsReady,LastError}`; `Capture::{SetEyeContact,EyeContactActive,EyeContactError}`. Consistent across tasks.
- **Placeholder scan:** the only intentional `<path to your Maxine-AR-SDK clone>` placeholders are user-supplied env values, flagged as such. The NvAR cross-check note (Task 3 Step 2) directs reconciliation against the on-disk sample rather than leaving code vague.
```
