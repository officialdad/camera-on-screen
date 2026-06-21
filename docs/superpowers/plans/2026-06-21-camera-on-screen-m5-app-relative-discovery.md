# App-relative SDK Discovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bundled-beside-exe (`<app>\maxine\`) resolution tier to both Maxine runtime resolvers so a shipped app finds the co-versioned VFX + AR runtimes with no environment variables.

**Architecture:** A new pure-Win32 helper pair (`ShimModuleDir`, `DirExists`) resolves the shim DLL's own directory CWD-independently. Each resolver (`ResolveSdkPaths` in aigs.cpp, `ResolveArPaths` in eyecontact.cpp) gains a third tier that activates when `<shimDir>\maxine` exists. Env vars stay highest-precedence as the dev override; the bundled tier is the ship default; AR's `%ProgramFiles%` install drops to last resort.

**Tech Stack:** C++17, Win32 (`GetModuleHandleExW`, `GetModuleFileNameW`, `GetFileAttributesW`, `WideCharToMultiByte`), MSVC v143, MSBuild (Build Tools). No new managed code, no C-ABI change.

## Global Constraints

- **Windows + x64 only.** Shim builds x64 exclusively.
- **Build via Build Tools MSBuild from PowerShell** — never the Bash tool (mangles `/p:` switches).
- **Both build configs must stay pristine (0 warnings):** SDK config (`COS_VFX_SDK_DIR` + `COS_AR_SDK_DIR` set → `COS_HAS_MAXINE` + `COS_HAS_MAXINE_AR`) and CI-stub config (`/p:CosVfxSdkDir= /p:CosArSdkDir=`).
- **`paths.{h,cpp}` compiles unconditionally** — no SDK guard, pure Win32, so it builds in the CI-stub config too. The resolvers that *call* it stay inside their existing `#ifdef COS_HAS_MAXINE` / `#ifdef COS_HAS_MAXINE_AR` guards.
- **No C-ABI change.** `CosStatus`/`CosParams`/`CosCaps` parity untouched; Core tests must stay green.
- **Bundled layout (target):** `<app>\maxine\` holds shared TRT/CUDA DLLs + `NVVideoEffects.dll` + `nvARPose.dll` + `models\vfx\` + `models\ar\`. Both proxies `SetDllDirectory(<app>\maxine)`.
- **Resolver precedence:**
  - VFX: `COS_VFX_RUNTIME_DIR` → `COS_VFX_SDK_DIR` → `<shimDir>\maxine` → error.
  - AR: `COS_AR_RUNTIME_DIR` → `<shimDir>\maxine` → `%ProgramFiles%\NVIDIA Corporation\NVIDIA AR SDK` → error.
- Env-var dev paths are **byte-for-byte unchanged** — only new tiers are appended/inserted.
- **Build SDK config LAST before any run** (CI stub + SDK write the same DLL path); verify the deployed DLL with `grep -a GreenScreen` AND `grep -a GazeRedirection` present, `grep -a "not built in"` absent.
- Convert paths to UTF-8 `std::string` (CP_UTF8) to match existing path plumbing; strip trailing slash; no trailing slash on returned dirs.

---

### Task 1: `paths.{h,cpp}` helper + headless smoke test

**Files:**
- Create: `native/shim/paths.h`
- Create: `native/shim/paths.cpp`
- Create: `native/shim/smoke/paths_smoke.cpp`
- Modify: `native/shim/shim.vcxproj` (add `paths.cpp` to ClCompile ItemGroup, `paths.h` to ClInclude ItemGroup)

**Interfaces:**
- Consumes: nothing (pure Win32).
- Produces:
  - `std::string ShimModuleDir();` — directory containing the module whose code calls it (the shim DLL at runtime), UTF-8, no trailing slash. Returns `""` on failure.
  - `bool DirExists(const std::string& path);` — true iff `path` is an existing directory.

- [ ] **Step 1: Write the headless smoke test (the failing test)**

Create `native/shim/smoke/paths_smoke.cpp`:

```cpp
// Headless smoke test for paths.cpp (ShimModuleDir + DirExists). Pure Win32, no SDK.
// Build ad hoc from a Developer PowerShell for VS 2022:
//
//   cl /EHsc /std:c++17 native\shim\smoke\paths_smoke.cpp native\shim\paths.cpp
//   .\paths_smoke.exe
//
// Expected output (exit 0):
//   ShimModuleDir: <some existing absolute dir>
//   DirExists(self): PASS
//   DirExists(self\__nope__): PASS (correctly false)
#include <cstdio>
#include <string>
#include "../paths.h"

int main() {
    std::string dir = ShimModuleDir();
    std::printf("ShimModuleDir: %s\n", dir.c_str());
    if (dir.empty()) { std::printf("FAIL: empty dir\n"); return 1; }
    if (!DirExists(dir)) { std::printf("FAIL: own dir not found\n"); return 2; }
    std::printf("DirExists(self): PASS\n");
    if (DirExists(dir + "\\__nope__")) { std::printf("FAIL: ghost dir reported present\n"); return 3; }
    std::printf("DirExists(self\\__nope__): PASS (correctly false)\n");
    return 0;
}
```

- [ ] **Step 2: Run the smoke test to verify it fails (paths.cpp does not exist yet)**

Run from PowerShell (Developer prompt or with `cl` on PATH):
```
cl /EHsc /std:c++17 native\shim\smoke\paths_smoke.cpp native\shim\paths.cpp
```
Expected: FAIL — `cl` cannot open `native\shim\paths.cpp` (file does not exist) / unresolved `ShimModuleDir`.

- [ ] **Step 3: Write `paths.h`**

Create `native/shim/paths.h`:

```cpp
#pragma once
#include <string>

// Directory containing the module whose compiled code calls this function — i.e. the shim
// DLL at runtime (CWD-independent; correct under Explorer double-click). UTF-8, no trailing
// slash. Returns "" on failure.
std::string ShimModuleDir();

// True iff `path` exists and is a directory.
bool DirExists(const std::string& path);
```

- [ ] **Step 4: Write `paths.cpp`**

Create `native/shim/paths.cpp`:

```cpp
#include "paths.h"

#define NOMINMAX
#include <windows.h>
#include <vector>

namespace {
std::string Utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
} // namespace

std::string ShimModuleDir() {
    HMODULE hmod = nullptr;
    // Resolve the module that contains THIS function's code (the shim DLL at runtime, or the
    // smoke exe when linked into a test). UNCHANGED_REFCOUNT: do not bump the module's refcount.
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ShimModuleDir), &hmod)) {
        return std::string();
    }
    std::vector<wchar_t> buf(1024);
    DWORD n = GetModuleFileNameW(hmod, buf.data(), (DWORD)buf.size());
    if (n == 0) return std::string();
    // Grow once if the path was truncated (ERROR_INSUFFICIENT_BUFFER).
    while (n == buf.size() && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        buf.resize(buf.size() * 2);
        n = GetModuleFileNameW(hmod, buf.data(), (DWORD)buf.size());
        if (n == 0) return std::string();
    }
    std::wstring path(buf.data(), n);
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::string();
    return Utf8(path.substr(0, slash));
}

bool DirExists(const std::string& path) {
    if (path.empty()) return false;
    int wn = MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), nullptr, 0);
    if (wn <= 0) return false;
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), &w[0], wn);
    DWORD attr = GetFileAttributesW(w.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
```

- [ ] **Step 5: Run the smoke test to verify it passes**

Run:
```
cl /EHsc /std:c++17 native\shim\smoke\paths_smoke.cpp native\shim\paths.cpp
.\paths_smoke.exe
```
Expected: PASS — exit 0, output:
```
ShimModuleDir: <absolute dir of paths_smoke.exe>
DirExists(self): PASS
DirExists(self\__nope__): PASS (correctly false)
```
(Clean up the stray `paths_smoke.exe` / `.obj` afterward; they are build artifacts, not committed.)

- [ ] **Step 6: Add `paths.{h,cpp}` to the vcxproj**

In `native/shim/shim.vcxproj`, add `paths.cpp` to the unconditional ClCompile ItemGroup (currently lines 90–95):

```xml
  <ItemGroup>
    <ClCompile Include="shim.cpp" />
    <ClCompile Include="capture.cpp" />
    <ClCompile Include="aigs.cpp" />
    <ClCompile Include="eyecontact.cpp" />
    <ClCompile Include="paths.cpp" />
  </ItemGroup>
```

And add `paths.h` to the ClInclude ItemGroup (currently lines 113–118):

```xml
  <ItemGroup>
    <ClInclude Include="shim.h" />
    <ClInclude Include="capture.h" />
    <ClInclude Include="aigs.h" />
    <ClInclude Include="eyecontact.h" />
    <ClInclude Include="paths.h" />
  </ItemGroup>
```

- [ ] **Step 7: Build the shim in CI-stub config and verify pristine (paths.cpp must compile with no SDK)**

Run from PowerShell:
```
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:CosVfxSdkDir= /p:CosArSdkDir=
```
Expected: `Build succeeded. 0 Warning(s) 0 Error(s)`. `paths.cpp` compiles; `ShimModuleDir`/`DirExists` are unreferenced in this config (no warning — external, non-static).

- [ ] **Step 8: Commit**

```bash
git add native/shim/paths.h native/shim/paths.cpp native/shim/smoke/paths_smoke.cpp native/shim/shim.vcxproj
git commit -m "feat(shim): add ShimModuleDir/DirExists path helpers (M5)"
```

---

### Task 2: VFX resolver — bundled `maxine\` tier

**Files:**
- Modify: `native/shim/aigs.cpp` (add `#include "paths.h"` inside the `COS_HAS_MAXINE` guard; extend `ResolveSdkPaths`)

**Interfaces:**
- Consumes: `ShimModuleDir()`, `DirExists()` from Task 1.
- Produces: no signature change — `ResolveSdkPaths(std::string& binDir, std::string& modelDir, std::string& err)` keeps the same shape; only its body gains a tier.

- [ ] **Step 1: Add the paths.h include**

In `native/shim/aigs.cpp`, after the existing SDK includes inside the guard (after `#include "nvVFXGreenScreen.h"`, currently line 15), add:

```cpp
#include "paths.h"
```

- [ ] **Step 2: Extend `ResolveSdkPaths` with the bundled tier**

In `ResolveSdkPaths`, replace the final fallback block (currently lines 44–50, starting `n = GetEnvironmentVariableA("COS_VFX_SDK_DIR", ...)`) with:

```cpp
    n = GetEnvironmentVariableA("COS_VFX_SDK_DIR", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        std::string root(buf, n);
        if (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
        binDir   = root + "\\bin";
        modelDir = root + "\\bin\\models";
        return true;
    }
    // Ship default: runtime bundled beside the app at <shimDir>\maxine (shared co-versioned
    // root; green-screen models in models\vfx).
    std::string appDir = ShimModuleDir();
    if (!appDir.empty()) {
        std::string maxine = appDir + "\\maxine";
        if (DirExists(maxine)) {
            binDir   = maxine;
            modelDir = maxine + "\\models\\vfx";
            return true;
        }
    }
    err = "VFX runtime not found: set COS_VFX_RUNTIME_DIR or bundle maxine\\ beside the app";
    return false;
```

- [ ] **Step 3: Build the shim in SDK config and verify pristine + symbol present**

Run from PowerShell (env vars set per CLAUDE.md, SDK config built LAST):
```
$env:COS_VFX_SDK_DIR = "C:\dev\VideoFX"
$env:COS_AR_SDK_DIR  = "C:\dev\Maxine-AR-SDK"
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
```
Expected: `Build succeeded. 0 Warning(s) 0 Error(s)`.

- [ ] **Step 4: Verify the GreenScreen path still compiles into the DLL**

Run (Bash tool ok for grep):
```
grep -a GreenScreen native/shim/x64/Debug/CameraOnScreen.Shim.dll >/dev/null && echo OK
```
Expected: `OK`.

- [ ] **Step 5: Commit**

```bash
git add native/shim/aigs.cpp
git commit -m "feat(shim): VFX resolver app-relative maxine\\ tier (M5)"
```

---

### Task 3: AR resolver — bundled `maxine\` tier above `%ProgramFiles%`

**Files:**
- Modify: `native/shim/eyecontact.cpp` (add `#include "paths.h"` inside the `COS_HAS_MAXINE_AR` guard; extend `ResolveArPaths`)

**Interfaces:**
- Consumes: `ShimModuleDir()`, `DirExists()` from Task 1.
- Produces: no signature change — `ResolveArPaths(std::string& runtimeDir, std::string& modelDir, std::string& err)` keeps the same shape; only its body gains a tier inserted between the env var and the ProgramFiles fallback.

- [ ] **Step 1: Add the paths.h include**

In `native/shim/eyecontact.cpp`, after the existing SDK includes inside the guard (after `#include "nvAR_defs.h"`, currently line 13), add:

```cpp
#include "paths.h"
```

- [ ] **Step 2: Extend `ResolveArPaths` with the bundled tier above ProgramFiles**

Replace the body of `ResolveArPaths` (currently lines 23–40) with:

```cpp
bool ResolveArPaths(std::string& runtimeDir, std::string& modelDir, std::string& err) {
    char buf[1024] = {0};
    // Tier 1: dev override.
    DWORD n = GetEnvironmentVariableA("COS_AR_RUNTIME_DIR", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        std::string root(buf, n);
        if (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
        runtimeDir = root;
        modelDir   = root + "\\models";
        return true;
    }
    // Tier 2 (ship default): runtime bundled beside the app at <shimDir>\maxine (shared
    // co-versioned root; gaze models in models\ar). Placed ABOVE the ProgramFiles install so a
    // shipped app stays self-contained and never loads a non-co-versioned system AR SDK.
    std::string appDir = ShimModuleDir();
    if (!appDir.empty()) {
        std::string maxine = appDir + "\\maxine";
        if (DirExists(maxine)) {
            runtimeDir = maxine;
            modelDir   = maxine + "\\models\\ar";
            return true;
        }
    }
    // Tier 3 (dev last resort): the installed AR SDK under Program Files.
    char pf[1024] = {0};
    DWORD m = GetEnvironmentVariableA("ProgramFiles", pf, sizeof(pf));
    if (m == 0 || m >= sizeof(pf)) { err = "AR runtime not found: set COS_AR_RUNTIME_DIR or bundle maxine\\ beside the app"; return false; }
    std::string root(pf, m);
    root += "\\NVIDIA Corporation\\NVIDIA AR SDK";
    if (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
    runtimeDir = root;
    modelDir   = root + "\\models";
    return true;
}
```

- [ ] **Step 3: Build the shim in full SDK config and verify pristine + both symbols present**

Run from PowerShell (env vars set, SDK config built LAST so the SDK DLL is what gets deployed):
```
$env:COS_VFX_SDK_DIR = "C:\dev\VideoFX"
$env:COS_AR_SDK_DIR  = "C:\dev\Maxine-AR-SDK"
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
```
Expected: `Build succeeded. 0 Warning(s) 0 Error(s)`.

- [ ] **Step 4: Verify both effect paths compiled into the DLL**

Run:
```
grep -a GreenScreen native/shim/x64/Debug/CameraOnScreen.Shim.dll >/dev/null && \
grep -a GazeRedirection native/shim/x64/Debug/CameraOnScreen.Shim.dll >/dev/null && \
! grep -a "not built in" native/shim/x64/Debug/CameraOnScreen.Shim.dll && echo OK
```
Expected: `OK` (both symbols present, stub string absent).

- [ ] **Step 5: Commit**

```bash
git add native/shim/eyecontact.cpp
git commit -m "feat(shim): AR resolver app-relative maxine\\ tier above ProgramFiles (M5)"
```

---

### Task 4: End-to-end build + human visual gate (env-unset, hand-bundled `maxine\`)

**Files:**
- Modify: `docs/superpowers/verification/2026-06-20-recorder-capture.md` (add an M5 app-relative-discovery section with results), or create a new verification note if the structure suggests it.

**Interfaces:**
- Consumes: the shim DLL from Task 3 + the App build.
- Produces: a recorded pass/fail of the bundled-path verification.

- [ ] **Step 1: Rebuild the App so the SDK-config shim DLL is deployed**

Run from PowerShell:
```
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild
```
Expected: `Build succeeded. 0 Warning(s)`. The SDK shim DLL is copied next to the exe.

- [ ] **Step 2: Hand-assemble the bundled `maxine\` folder beside the exe**

Into `src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/maxine\`, copy a **known-good co-versioned runtime** (VFX 0.7.6 + AR 0.8.7, shared TRT 10.4) per the spec layout:
```
maxine\
  <shared TRT/CUDA DLLs: nvinfer_10.dll, cudart64_12.dll, NVCVImage.dll, ...>
  NVVideoEffects.dll        (from VFX 0.7.6 runtime)
  nvARPose.dll              (from AR 0.8.7 runtime)
  models\vfx\               (green-screen models from VFX 0.7.6)
  models\ar\                (gaze models from AR 0.8.7)
```
Source DLLs/models: copy the VFX 0.7.6 flat runtime contents (the `COS_VFX_RUNTIME_DIR` tree) into `maxine\` and `maxine\models\vfx\`; copy the AR 0.8.7 install (`nvARPose.dll` + its `models\`) into `maxine\` and `maxine\models\ar\`. Shared same-named DLLs are byte-identical — keep one copy.

- [ ] **Step 3: Unset all `COS_*` env vars in the run shell and launch the app**

Run from PowerShell:
```
Remove-Item Env:COS_VFX_RUNTIME_DIR -ErrorAction SilentlyContinue
Remove-Item Env:COS_VFX_SDK_DIR     -ErrorAction SilentlyContinue
Remove-Item Env:COS_AR_RUNTIME_DIR  -ErrorAction SilentlyContinue
Remove-Item Env:COS_AR_SDK_DIR      -ErrorAction SilentlyContinue
src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.App.exe
```
Expected: app launches, no startup-error.log written.

- [ ] **Step 4: Confirm both effects resolve from the bundle (human visual gate)**

Verify on the RTX 3090, with NO env vars set:
- Both capability probes pass (green screen + eye contact toggles are **enabled**, not greyed).
- Green screen works (matte composites live).
- Eye contact works (gaze redirects live).
- Both work together (toggle both on).

This is the real verification — it proves the `<app>\maxine\` tier resolves both runtimes end-to-end with zero environment configuration.

- [ ] **Step 5: Record the result in the verification doc**

Add an "M5 — app-relative discovery" section to `docs/superpowers/verification/2026-06-20-recorder-capture.md` (or a new note) capturing: env vars unset, `maxine\` layout used, both probes passed, both effects confirmed live, GPU = RTX 3090.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/verification/
git commit -m "docs(M5): verify app-relative maxine\\ discovery on RTX 3090"
```

---

## Notes on testing approach (deliberate deviation from pure unit TDD)

The resolver logic is native Win32 + Maxine SDK glue with no managed surface and no C++ unit-test framework wired into this repo. Per the established repo convention (CLAUDE.md: native effects are verified by build-pristine + a human visual gate; M3 and M4 added **no** unit tests for their resolvers), this plan tests via: (1) a real headless smoke test for the pure-Win32 helpers in Task 1 (red→green), (2) build-pristine + symbol-grep verification for the SDK-guarded resolver edits, and (3) the human visual gate in Task 4. The C-ABI is unchanged, so the existing xUnit Core tests remain the regression net and must stay green (run `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj` if any managed file is touched — none is in this plan).
