# QoL Bucket 3 — Green-screen Expand + Feather Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two live green-screen matte sliders — **Expand** (dilate the matte outward so the edge stops cutting into the body) and **Feather** (blur the matte edge) — end to end from the WinUI panel through the C ABI into the Maxine composite.

**Architecture:** The reserved-but-unused `green_screen_strength` C-ABI field is **repurposed** as `green_screen_expand`; a new `green_screen_feather` `double` is **added`. Both flow UI → `MainViewModel` → `ShimParams` → P/Invoke `CosParams` → `cos_set_params` → atomic doubles on the capture worker → `Aigs::ProcessFrame`. Matte post-processing (dilate then feather) runs worker-thread-local on the downloaded CPU matte inside `aigs.cpp` **before** the premultiplied composite. No threading change, no new locks.

**Tech Stack:** C# .NET 8 + WinUI 3 (CommunityToolkit MVVM source generators), C++ Media Foundation shim, NVIDIA Maxine VideoFX GreenScreen (behind `COS_HAS_MAXINE`), xUnit.

## Global Constraints

- **Pristine build:** 0 warnings across shim, App, and Core — warnings are findings.
- **C ABI byte-parity on x64:** `CosParams` (C `shim.h`) and the `[StructLayout(LayoutKind.Sequential)]` mirror in `PInvokeShim.cs` must match byte-for-byte. New layout: `const char* camera_id` (8) · `int green_screen_enabled` (4) · `double green_screen_expand` (8) · `double green_screen_feather` (8) · `int eye_contact_enabled` (4) · `double eye_contact_sensitivity` (8) · `double eye_contact_look_away_range` (8). Verify with `dumpbin /exports` (exports unchanged: still 9) and the deploy `grep` check.
- **Worker-thread discipline:** the `Aigs` object lives only on the capture worker thread (CUDA affinity). UI thread only flips atomics. No new lock nested under `g_state.mtx` / `g_lifecycleMtx`; the `gsErrMtx` leaf-lock rule is unchanged.
- **CPU-copy seam unchanged:** matte ops are added inside `Composite`'s pre-step; the Upload/Run/Download/Composite seam structure stays.
- **Build-deploy order:** build the SDK shim config (`COS_HAS_MAXINE`, `COS_VFX_SDK_DIR` set) **LAST**, then `-t:Rebuild` the App, or the passthrough stub deploys (greyed toggles). See CLAUDE.md "Build & test".
- **Persistence:** `MainViewModel.ToAppConfig` builds a fresh `AppConfig` — any field not copied reverts to default. Both new fields must round-trip through `LoadFrom`/`ToAppConfig`.
- **Slider semantics:** Expand and Feather are both `double` in `[0,1]`. `0` = no-op (radius 0). Default Expand `0.0`, Feather `0.0` (no change to current matte behavior out of the box).

---

### Task 1: Core contracts — rename `Strength` → `Expand`, add `Feather`

**Files:**
- Modify: `src/CameraOnScreen.Core/Native/Contracts.cs:18-24` (`ShimParams`)
- Modify: `src/CameraOnScreen.Core/Config/Models.cs:24-31` (`EffectSettings`)
- Test: `tests/CameraOnScreen.Core.Tests/Config/JsonSettingsStoreTests.cs:54-72`
- Test: `tests/CameraOnScreen.Core.Tests/Orchestration/OrchestratorTests.cs:10-11`

**Interfaces:**
- Produces: `ShimParams(string? CameraId, bool GreenScreenEnabled, double GreenScreenExpand, double GreenScreenFeather, bool EyeContactEnabled, double EyeContactSensitivity, double EyeContactLookAwayRange)`
- Produces: `EffectSettings { bool GreenScreenEnabled=true; double GreenScreenExpand=0.0; double GreenScreenFeather=0.0; bool EyeContactEnabled; double EyeContactSensitivity=0.5; double EyeContactLookAwayRange=0.5; }`

- [ ] **Step 1: Update `JsonSettingsStoreTests` to the new field names**

In `tests/CameraOnScreen.Core.Tests/Config/JsonSettingsStoreTests.cs`, replace the `EffectSettings` initializer (line ~56) and assertions (line ~72):

```csharp
                Effects = new EffectSettings
                {
                    GreenScreenEnabled = false, GreenScreenExpand = 0.25, GreenScreenFeather = 0.4,
                    EyeContactEnabled = true, EyeContactSensitivity = 0.75,
                    EyeContactLookAwayRange = 0.9
                },
```

```csharp
            Assert.False(loaded.Effects.GreenScreenEnabled);
            Assert.Equal(0.25, loaded.Effects.GreenScreenExpand);
            Assert.Equal(0.4, loaded.Effects.GreenScreenFeather);
            Assert.True(loaded.Effects.EyeContactEnabled);
```

- [ ] **Step 2: Update `OrchestratorTests.Requested()` to the new params**

In `tests/CameraOnScreen.Core.Tests/Orchestration/OrchestratorTests.cs` (line ~10):

```csharp
    private static ShimParams Requested() =>
        new(CameraId: "cam", GreenScreenEnabled: true, GreenScreenExpand: 0.5, GreenScreenFeather: 0.3,
            EyeContactEnabled: true, EyeContactSensitivity: 0.5, EyeContactLookAwayRange: 0.5);
```

- [ ] **Step 3: Run the tests to verify they FAIL to compile**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
Expected: BUILD FAIL — `ShimParams` has no `GreenScreenExpand`/`GreenScreenFeather`, `EffectSettings` has no such members.

- [ ] **Step 4: Rename + add the field in `ShimParams`**

In `src/CameraOnScreen.Core/Native/Contracts.cs` replace the record (line 18):

```csharp
public sealed record ShimParams(
    string? CameraId,
    bool GreenScreenEnabled,
    double GreenScreenExpand,
    double GreenScreenFeather,
    bool EyeContactEnabled,
    double EyeContactSensitivity,
    double EyeContactLookAwayRange);
```

- [ ] **Step 5: Rename + add the field in `EffectSettings`**

In `src/CameraOnScreen.Core/Config/Models.cs` replace the record body (line 24):

```csharp
public sealed record EffectSettings
{
    public bool GreenScreenEnabled { get; init; } = true;
    public double GreenScreenExpand { get; init; }
    public double GreenScreenFeather { get; init; }
    public bool EyeContactEnabled { get; init; }
    public double EyeContactSensitivity { get; init; } = 0.5;
    public double EyeContactLookAwayRange { get; init; } = 0.5;
}
```

- [ ] **Step 6: Build Core to surface every remaining reference**

Run: `dotnet build src/CameraOnScreen.Core/CameraOnScreen.Core.csproj`
Expected: FAIL in `MainViewModel.cs` (still references `GreenScreenStrength`). That is fixed in Task 2 — do not fix here. Core's own `Contracts`/`Models` compile.

> Note: do not run the full test suite green yet — `MainViewModel` (same Core project) won't compile until Task 2. Tasks 1+2 land Core green together; commit at the end of Task 2.

---

### Task 2: ViewModel + XAML — props, live push, two sliders

**Files:**
- Modify: `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs` (props line ~54; `LoadFrom` ~72; `ToAppConfig` ~99; partials ~112; `BuildParams` ~125)
- Modify: `src/CameraOnScreen.App/MainWindow.xaml:19-35`
- Test: `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs:99-106`

**Interfaces:**
- Consumes: `ShimParams` and `EffectSettings` from Task 1.
- Produces: `MainViewModel.GreenScreenExpand` (double), `MainViewModel.GreenScreenFeather` (double) observable props; `BuildParams()` returns the 7-arg `ShimParams` above.

- [ ] **Step 1: Update the `MainViewModel` round-trip test**

In `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs` (line ~99) replace the body that set `GreenScreenStrength`:

```csharp
        var vm = Build(GpuTier.Rtx, out _);
        vm.GreenScreenEnabled = true;
        vm.GreenScreenExpand = 0.7;
        vm.GreenScreenFeather = 0.2;
        vm.SelectedCamera = new CameraInfo("cam", "Cam");
        var p = vm.BuildParams();
        Assert.Equal("cam", p.CameraId);
        Assert.Equal(0.7, p.GreenScreenExpand);
        Assert.Equal(0.2, p.GreenScreenFeather);
```

- [ ] **Step 2: Run it to verify it FAILS to compile**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~MainViewModelTests"`
Expected: BUILD FAIL — `MainViewModel` has no `GreenScreenExpand`/`GreenScreenFeather`.

- [ ] **Step 3: Rename + add the observable properties**

In `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs` replace the strength field (line 54):

```csharp
    [ObservableProperty] private double greenScreenExpand;
    [ObservableProperty] private double greenScreenFeather;
```

- [ ] **Step 4: Update `LoadFrom`**

Replace line ~72:

```csharp
        GreenScreenExpand = config.Effects.GreenScreenExpand;
        GreenScreenFeather = config.Effects.GreenScreenFeather;
```

- [ ] **Step 5: Update `ToAppConfig`**

Replace the `Effects = new EffectSettings { ... }` green-screen line (line ~99):

```csharp
            GreenScreenEnabled = GreenScreenEnabled, GreenScreenExpand = GreenScreenExpand,
            GreenScreenFeather = GreenScreenFeather,
```

- [ ] **Step 6: Update the live-push partials**

Replace the strength partial (line ~112) with two:

```csharp
    partial void OnGreenScreenExpandChanged(double value) => ApplyLiveParams();
    partial void OnGreenScreenFeatherChanged(double value) => ApplyLiveParams();
```

- [ ] **Step 7: Update `BuildParams`**

Replace the `GreenScreenStrength:` arg (line ~125):

```csharp
        GreenScreenExpand: GreenScreenExpand,
        GreenScreenFeather: GreenScreenFeather,
```

- [ ] **Step 8: Run the Core tests — all green**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
Expected: PASS (all tests, 0 warnings).

- [ ] **Step 9: Add the two sliders to the panel**

In `src/CameraOnScreen.App/MainWindow.xaml`, insert after the AI Green Screen `ToggleSwitch` (line 21, before the Eye Contact toggle). The sliders are enabled only when green screen is on:

```xml
            <Slider Header="Green-screen Expand" Minimum="0" Maximum="1" StepFrequency="0.05"
                    IsEnabled="{x:Bind Vm.GreenScreenEnabled, Mode=OneWay}"
                    Value="{x:Bind Vm.GreenScreenExpand, Mode=TwoWay}"/>
            <Slider Header="Green-screen Feather" Minimum="0" Maximum="1" StepFrequency="0.05"
                    IsEnabled="{x:Bind Vm.GreenScreenEnabled, Mode=OneWay}"
                    Value="{x:Bind Vm.GreenScreenFeather, Mode=TwoWay}"/>
```

- [ ] **Step 10: Build the App (managed only; existing shim DLL is fine here)**

Run: `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild`
Expected: BUILD SUCCESS, 0 warnings.

- [ ] **Step 11: Commit (Tasks 1+2 — managed end-to-end)**

```bash
git add src/CameraOnScreen.Core src/CameraOnScreen.App tests
git commit -m "feat(effects): green-screen Expand/Feather VM + sliders (managed)"
```

---

### Task 3: C ABI + native plumbing — `CosParams`, P/Invoke mirror, worker atomics

**Files:**
- Modify: `native/shim/shim.h:18-25` (`CosParams`)
- Modify: `src/CameraOnScreen.App/Native/PInvokeShim.cs:20-26` (mirror) and `:69-77` (marshal)
- Modify: `native/shim/shim.cpp:59-64` (`cos_set_params`)
- Modify: `native/shim/capture.h:33` (add `SetMatteParams`)
- Modify: `native/shim/capture.cpp` (atomics ~36, `SetMatteParams` ~381, worker read ~306)

**Interfaces:**
- Consumes: `ShimParams.GreenScreenExpand`/`GreenScreenFeather` from Task 1.
- Produces (C ABI): `CosParams { const char* camera_id; int green_screen_enabled; double green_screen_expand; double green_screen_feather; int eye_contact_enabled; double eye_contact_sensitivity; double eye_contact_look_away_range; }`.
- Produces (native): `Capture::SetMatteParams(double expand, double feather)`; `Capture::CurrentMatteExpand()`/`CurrentMatteFeather()` are NOT exposed — the worker reads the atomics directly (same file). Worker passes `(expand, feather)` into `aigs.ProcessFrame(...)` (Task 4 signature).

- [ ] **Step 1: Rename + add the field in C `CosParams`**

In `native/shim/shim.h` replace the struct (line 18):

```c
typedef struct {
    const char* camera_id;   // UTF-8, may be null
    int    green_screen_enabled;
    double green_screen_expand;       // 0..1 matte dilate (was green_screen_strength, unused)
    double green_screen_feather;      // 0..1 matte blur
    int    eye_contact_enabled;
    double eye_contact_sensitivity;
    double eye_contact_look_away_range;
} CosParams;
```

- [ ] **Step 2: Mirror the rename + add in `PInvokeShim` `CosParams`**

In `src/CameraOnScreen.App/Native/PInvokeShim.cs` replace the struct (line 20):

```csharp
    [StructLayout(LayoutKind.Sequential)]
    private struct CosParams
    {
        public IntPtr camera_id;
        public int green_screen_enabled; public double green_screen_expand;
        public double green_screen_feather;
        public int eye_contact_enabled; public double eye_contact_sensitivity;
        public double eye_contact_look_away_range;
    }
```

- [ ] **Step 3: Update the `SetParams` marshal**

In `PInvokeShim.cs` (line ~69) replace the `green_screen_strength` assignment:

```csharp
                green_screen_enabled = p.GreenScreenEnabled ? 1 : 0,
                green_screen_expand = p.GreenScreenExpand,
                green_screen_feather = p.GreenScreenFeather,
```

- [ ] **Step 4: Plumb params from `cos_set_params` to the capture**

In `native/shim/shim.cpp` replace `cos_set_params` (line 59):

```cpp
COS_API void cos_set_params(const CosParams* p) {
    if (!p) return;
    g_params = *p;
    g_cameraId = p->camera_id ? p->camera_id : "";
    g_capture.SetGreenScreen(p->green_screen_enabled != 0);
    g_capture.SetMatteParams(p->green_screen_expand, p->green_screen_feather);
}
```

- [ ] **Step 5: Declare `SetMatteParams` on `Capture`**

In `native/shim/capture.h` after the `SetGreenScreen` declaration (line 33):

```cpp
    // Sets matte post-process amounts (0..1). Thread-safe; worker reads per frame.
    void SetMatteParams(double expand, double feather);
```

- [ ] **Step 6: Add the worker-read atomics**

In `native/shim/capture.cpp`, in the capture state struct after `greenScreenEnabled` (line ~36):

```cpp
    std::atomic<double>   matteExpand{0.0};  // set by UI thread, read by worker
    std::atomic<double>   matteFeather{0.0}; // set by UI thread, read by worker
```

- [ ] **Step 7: Implement `SetMatteParams`**

In `native/shim/capture.cpp` after `Capture::SetGreenScreen` (line ~383):

```cpp
void Capture::SetMatteParams(double expand, double feather) {
    g_state.matteExpand.store(expand, std::memory_order_release);
    g_state.matteFeather.store(feather, std::memory_order_release);
}
```

- [ ] **Step 8: Pass the amounts into `ProcessFrame` in the worker**

In `native/shim/capture.cpp` worker loop replace the `ProcessFrame` call (line ~307):

```cpp
                    const double expand  = g_state.matteExpand.load(std::memory_order_acquire);
                    const double feather = g_state.matteFeather.load(std::memory_order_acquire);
                    applied = aigs.ProcessFrame(scratch.data(), width, height, expand, feather);
```

> This won't compile until Task 4 widens `ProcessFrame`'s signature. Do Task 4 next, then build.

---

### Task 4: Matte ops — dilate + feather in `aigs.cpp`

**Files:**
- Modify: `native/shim/aigs.h:30` (`ProcessFrame` signature)
- Modify: `native/shim/aigs.cpp` (`AigsImpl` scratch ~47; new matte passes; `Composite` ~175; `ProcessFrame` ~193; stub ~214)

**Interfaces:**
- Consumes: worker call `ProcessFrame(uint8_t* bgra, int w, int h, double expand, double feather)`.
- Produces: in-place premultiplied BGRA, matte post-processed **dilate → feather → composite**.

- [ ] **Step 1: Widen the `ProcessFrame` declaration**

In `native/shim/aigs.h` replace line 30 (and its doc comment above as needed):

```cpp
    // Run GreenScreen on a tightly-packed BGRA buffer (width*height*4) in place:
    // A = matte, RGB premultiplied by matte/255. expand/feather (0..1) dilate then
    // blur the matte before compositing. Returns true if applied; false leaves 'bgra' untouched.
    bool ProcessFrame(uint8_t* bgra, int width, int height, double expand, double feather);
```

- [ ] **Step 2: Add packed scratch buffers to `AigsImpl`**

In `native/shim/aigs.cpp` add to `struct AigsImpl` (line ~47, alongside the NvCVImage members):

```cpp
    std::vector<uint8_t> matteWork; // packed w*h, post-processed matte (pitch == w)
    std::vector<uint8_t> matteTmp;  // packed w*h, separable-pass scratch
```

Add `#include <vector>`, `#include <algorithm>`, `#include <cmath>` at the top of the file if not already present (inside the `COS_HAS_MAXINE` section is fine since the stub doesn't use them).

- [ ] **Step 3: Add the separable matte passes (anonymous namespace, above `Composite`)**

Insert into the anonymous namespace in `native/shim/aigs.cpp` (before `Composite`, line ~174):

```cpp
// Max amount of dilate/blur at slider value 1.0, in pixels.
constexpr int kMaxDilateRadius = 16;
constexpr int kMaxBlurRadius   = 16;

static int RadiusFromAmount(double amount, int maxRadius) {
    if (amount <= 0.0) return 0;
    if (amount >= 1.0) return maxRadius;
    return static_cast<int>(std::lround(amount * maxRadius));
}

// Copy the (possibly padded) CPU matte into a packed w*h buffer.
static void PackMatte(AigsImpl* impl, int w, int h) {
    impl->matteWork.resize(static_cast<size_t>(w) * h);
    impl->matteTmp.resize(static_cast<size_t>(w) * h);
    const uint8_t* m = static_cast<const uint8_t*>(impl->matteCpu.pixels);
    const int mpitch = impl->matteCpu.pitch;
    for (int y = 0; y < h; ++y)
        std::memcpy(impl->matteWork.data() + static_cast<size_t>(w) * y,
                    m + static_cast<size_t>(mpitch) * y, static_cast<size_t>(w));
}

// Separable morphological dilate (max filter) over matteWork; result back in matteWork.
static void DilateMatte(AigsImpl* impl, int w, int h, int r) {
    if (r <= 0) return;
    uint8_t* src = impl->matteWork.data();
    uint8_t* tmp = impl->matteTmp.data();
    for (int y = 0; y < h; ++y) {       // horizontal
        const uint8_t* srow = src + static_cast<size_t>(w) * y;
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
            src[static_cast<size_t>(w) * y + x] = mx;
        }
    }
}

// Separable box blur over matteWork; result back in matteWork.
static void FeatherMatte(AigsImpl* impl, int w, int h, int r) {
    if (r <= 0) return;
    const int win = 2 * r + 1;
    uint8_t* src = impl->matteWork.data();
    uint8_t* tmp = impl->matteTmp.data();
    for (int y = 0; y < h; ++y) {       // horizontal
        const uint8_t* srow = src + static_cast<size_t>(w) * y;
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
            src[static_cast<size_t>(w) * y + x] = static_cast<uint8_t>(sum / win);
        }
    }
}
```

> Box-blur edge note: the divisor is the full window `win` even near borders (clamped read range is shorter), so edges darken slightly toward 0 — acceptable for a soft feather and matches the dilate's clamp behavior. Keep it simple.

- [ ] **Step 4: Make `Composite` read the packed `matteWork` (pitch == w)**

Replace `Composite` (line 175) so it reads `impl->matteWork` instead of `matteCpu`:

```cpp
// SEAM 3 (composite): apply the post-processed packed matte to BGRA in place, premultiplied.
void Composite(AigsImpl* impl, uint8_t* bgra, int w, int h) {
    const uint8_t* m = impl->matteWork.data(); // packed, pitch == w
    for (int y = 0; y < h; ++y) {
        const uint8_t* mrow = m + static_cast<size_t>(w) * y;
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
```

- [ ] **Step 5: Wire the passes into `ProcessFrame` (order: pack → dilate → feather → composite)**

Replace `ProcessFrame` (line 193):

```cpp
bool Aigs::ProcessFrame(uint8_t* bgra, int w, int h, double expand, double feather) {
    auto* impl = static_cast<AigsImpl*>(impl_);
    if (!impl || !impl->effect || !bgra || w <= 0 || h <= 0) return false;

    if (EnsureImages(impl, w, h) != NVCV_SUCCESS) { lastError_ = "EnsureImages/Load failed"; ready_ = false; return false; }
    if (Upload(impl, bgra, w, h) != NVCV_SUCCESS) { lastError_ = "Upload (Transfer) failed"; return false; }
    if (NvVFX_Run(impl->effect, 0) != NVCV_SUCCESS) { lastError_ = "NvVFX_Run failed"; return false; }
    if (Download(impl) != NVCV_SUCCESS) { lastError_ = "Download (Transfer) failed"; return false; }
    if (NvVFX_CudaStreamSynchronize(impl->stream) != NVCV_SUCCESS) { lastError_ = "stream sync failed"; return false; }

    PackMatte(impl, w, h);
    DilateMatte(impl, w, h, RadiusFromAmount(expand, kMaxDilateRadius));
    FeatherMatte(impl, w, h, RadiusFromAmount(feather, kMaxBlurRadius));
    Composite(impl, bgra, w, h);
    return true;
}
```

- [ ] **Step 6: Update the passthrough stub signature**

Replace the stub `ProcessFrame` (line ~214):

```cpp
bool Aigs::ProcessFrame(uint8_t*, int, int, double, double) { return false; }
```

---

### Task 5: Build native (SDK last), verify ABI, deploy, run + visual gate

**Files:** none new — build, verify, run, commit.

- [ ] **Step 1: Build the shim — SDK config (must be the LAST shim build)**

Run in **PowerShell** (not Bash — MSBuild `/p:` mangling):

```powershell
$env:COS_VFX_SDK_DIR = "C:\Users\opari\OneDrive\Desktop\claude-code\VideoFX"
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
```
Expected: BUILD SUCCEEDED, 0 warnings. Output: `native/shim/x64/Debug/CameraOnScreen.Shim.dll`.

- [ ] **Step 2: Verify the C ABI exports (still 9) and SDK build deployed**

```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" /version > $null
dumpbin /exports native/shim/x64/Debug/CameraOnScreen.Shim.dll | Select-String "cos_"
```
Expected: 9 `cos_*` exports, unchanged.

Then the deploy-build check (after Step 3): `grep -a GreenScreen` present, `grep -a "Maxine SDK not built in"` absent in the deployed DLL.

- [ ] **Step 3: Rebuild the App (copies the SDK shim next to the exe)**

Run: `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild`
Expected: BUILD SUCCESS, 0 warnings.

- [ ] **Step 4: Confirm the SDK (not stub) DLL deployed**

```bash
grep -a GreenScreen src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.Shim.dll | head -c 1
grep -a "Maxine SDK not built in" src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.Shim.dll
```
Expected: first prints a match (non-empty), second prints nothing.

- [ ] **Step 5: Re-run Core tests (regression)**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
Expected: PASS, 0 warnings.

- [ ] **Step 6: Run the app and VISUAL GATE on the RTX 3090 (human)**

```powershell
$env:COS_VFX_SDK_DIR = "C:\Users\opari\OneDrive\Desktop\claude-code\VideoFX"
src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.App.exe
```
Verify (overlay is NOT GDI-capturable — observe live or via DWM/ShadowPlay):
- Green screen ON, Expand at 0 → current behavior.
- Raise **Expand** → matte coverage grows outward; edge stops cutting into the body. Live, no Stop/Start.
- Raise **Feather** → matte edge softens. Live.
- Both sliders disabled when green screen is OFF.
- Settings persist across restart (drag/close saves; relaunch restores slider values).

- [ ] **Step 7: Commit the native + ABI work**

```bash
git add native/shim src/CameraOnScreen.App/Native/PInvokeShim.cs
git commit -m "feat(aigs): matte expand (dilate) + feather (blur), live via C ABI"
```

- [ ] **Step 8: Update CLAUDE.md status + memory**

- Mark Open follow-up #4 (Bucket 3) DONE; note Bucket 1 is NEXT.
- Update the QoL paragraph: Bucket 3 done/merged + user-verified.
- Update `memory/camera-on-screen-build-state.md` accordingly.

```bash
git add CLAUDE.md
git commit -m "docs: CLAUDE.md — QoL Bucket 3 done, Bucket 1 next"
```

---

## Self-Review

**Spec coverage** (Bucket 3 section of `2026-06-21-camera-on-screen-qol-polish-design.md`):
- Expand = dilate, Feather = blur → Task 4 `DilateMatte`/`FeatherMatte`. ✓
- Lives in `aigs.cpp` `Composite`, order dilate → feather → composite, honors pitch (PackMatte reads `matteCpu.pitch`), worker-thread-local, live via atomic path. ✓ (Task 3 atomics + Task 4 ops)
- C ABI: repurpose `green_screen_strength` → `green_screen_expand`, add `green_screen_feather`, both double, all 6 parity sites: shim.h (T3.1), PInvokeShim (T3.2), Contracts (T1.4), Models (T1.5), MainViewModel (T2.3–7), XAML (T2.9). ✓
- Panel: two sliders, gated on green-screen on. ✓ (T2.9 `IsEnabled` bind)
- Tests: renamed/added `EffectSettings` + `BuildParams`/round-trip. ✓ (T1.1–2, T2.1)
- dumpbin + grep deploy check + RTX visual gate. ✓ (T5.2, T5.4, T5.6)
- SDK build last. ✓ (T5.1)

**Placeholder scan:** no TBD/"handle errors"/"similar to" — all steps carry concrete code. ✓

**Type consistency:** `GreenScreenExpand`/`GreenScreenFeather` used identically across `ShimParams`, `EffectSettings`, `MainViewModel`, `CosParams` (C + C#). `ProcessFrame(uint8_t*, int, int, double, double)` matches between aigs.h decl (T4.1), worker call (T3.8), impl (T4.5), stub (T4.6). `SetMatteParams(double, double)` matches capture.h (T3.5) / capture.cpp (T3.7) / call site (T3.4). ✓
