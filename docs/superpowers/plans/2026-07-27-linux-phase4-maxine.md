# Linux Phase 4 — Maxine Effects (Green Screen + Eye Contact) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Linux shim runs the real Maxine effects — AI green screen (VFX 1.2.0.0) and eye contact / gaze redirection (AR 1.1.1.0) — inside the V4L2 capture worker, gated by the existing capability probe, so the Avalonia panel's toggles ungrey and work end to end on the RTX 3090.

**Architecture:** All spike verdicts are in (see spec §6): both effects load in one process on this box; Linux cores bundle **TRT 10.9.0 + CUDA 12.8.90** (the Windows-verified pins). The existing effect classes (`Aigs`, `EyeContact`, `SuperRes`) and their `COS_HAS_MAXINE*` guards stay; what's new on Linux: (1) portable guards replacing `<windows.h>`/`GetEnvironmentVariableA`, (2) Linux branches of the two runtime resolvers, (3) a **preload-closure** loader (the SDK `.so` set has **no RUNPATH** — dlopen every runtime lib by absolute path with `RTLD_GLOBAL`; later plain-name dlopens and `DT_NEEDED` then resolve by SONAME against the already-loaded set), (4) a hand-rolled **Linux VFX/NvCVImage proxy** (NVIDIA's `NVVideoEffectsProxy.cpp`/`nvCVImageProxy.cpp` are `#warning not ported` on Linux; the AR proxy `nvARProxy.cpp` IS ported and is reused as-is), (5) the **`GetOSInfo` interposer** (verified fix: `libVideoFXLocal.so` segfaults parsing CachyOS's `/etc/lsb-release`), (6) the worker-chain insertion in `capture_v4l2.cpp` mirroring `capture.cpp`.

**Tech Stack:** C++17, CMake (existing `native/shim/CMakeLists.txt`), dlfcn. SDK trees on this box: `~/dev/VideoFX-linux/VideoFX` and `~/dev/ARSDK-linux/ARSDK` (cores + features + engines installed); header/proxy clones at `~/dev/Maxine-VFX-SDK`, `~/dev/Maxine-AR-SDK`. Refetch anytime: `scripts/fetch-maxine-linux.sh` + `ngc registry resource download-version nvidia/maxine/{vfx_sdk_core:1.2.0.0_linux,ar_sdk_core:1.1.1.0_linux}` (key: `NVIDIA_API_KEY` in repo `.env`).

## Global Constraints

- Builds pristine: shim `-Wall -Wextra -Werror`; dotnet `/p:TreatWarningsAsErrors=true`, 0 warnings.
- User-local toolchain: `export DOTNET_ROOT="$HOME/.dotnet" PATH="$HOME/.dotnet:$PATH"`; cmake at `~/.local/bin`. GUI runs need `DISPLAY=:0` + `XAUTHORITY=$(command ls /run/user/1000/xauth_* | head -1)`.
- **Never touch** `src/CameraOnScreen.App` (WinUI), `native/shim/shim.vcxproj`, `capture.cpp` — Windows paths stay as-is; no Windows CI to verify them (runner offline).
- **Stub-by-default is load-bearing for CI:** without `COS_VFX_SDK_DIR`/`COS_AR_SDK_DIR` at cmake time, the shim MUST still build the passthrough stubs exactly as today (`ci-linux.yml` has no SDK). Green screen/gaze stub `Probe` strings keep saying `"not built in"`; never introduce that substring into the SDK-enabled build.
- The C ABI (9 exports) and struct layouts do not change. `cos_query_capabilities` already routes to `Aigs::Probe`/`EyeContact::Probe` — no shim.cpp API work.
- Maxine objects are **worker-thread-local** (CUDA thread affinity): construct/Start/Stop/ProcessFrame only inside `WorkerLoop`; UI toggles cross via existing atomics. Eye contact runs BEFORE green screen (repo contract).
- Frames are BGRA in-place buffers; effects mutate `cur.data()` (`Aigs::ProcessFrame(uint8_t* bgra, w, h, expand, feather)`, `EyeContact::ProcessFrame(bgra, w, h)`).
- Dev-box env for SDK runs: `export COS_VFX_RUNTIME_DIR=$HOME/dev/VideoFX-linux/VideoFX COS_AR_RUNTIME_DIR=$HOME/dev/ARSDK-linux/ARSDK`.
- FRUC and NGX super-res stay OUT of scope: FRUC needs the Optical Flow SDK Linux download (manual dev-site login; separate issue), and `libnvngxruntime.so` VSR is unverified on Linux — `SuperRes::Probe` failing → toggle greys, which is correct behavior. Do not block on either.
- **Deploy-the-right-shim applies on Linux too:** the SDK and stub configs write the same `libCameraOnScreen.Shim.so`. Always build the SDK config last before app runs; verify with `nm -D` (GetOSInfo present = SDK build) or `strings ... | grep "not built in"` (absent = SDK build).

## File Structure

- `native/shim/maxine_linux.h` / `maxine_linux.cpp` — NEW: Linux-only runtime discovery (`ResolveVfxRootLinux`, `ResolveArRootLinux`), `PreloadMaxineClosure`, exported `GetOSInfo` interposer.
- `native/shim/nvvfx_proxy_linux.cpp` — NEW: dlsym stubs for the NvVFX_* + NvCVImage_* surface (the unported proxies' Linux replacement).
- `native/shim/vfx_paths.cpp`, `eyecontact.cpp`, `aigs.cpp`, `superres.cpp` — MODIFY: portable guards + Linux resolver branches.
- `native/shim/capture_v4l2.cpp` — MODIFY: effect chain in `WorkerLoop`.
- `native/shim/CMakeLists.txt` — MODIFY: optional Maxine config from `COS_VFX_SDK_DIR`/`COS_AR_SDK_DIR` env.
- `CLAUDE.md` — MODIFY: Linux build section gains the Maxine paragraph.

---

### Task 1: Portable effect sources (Windows-only code behind `_WIN32`)

**Files:**
- Modify: `native/shim/aigs.cpp`, `native/shim/eyecontact.cpp`, `native/shim/superres.cpp`, `native/shim/vfx_paths.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: these four files compile on Linux under `-Werror` with AND without `COS_HAS_MAXINE`/`COS_HAS_MAXINE_AR`. Resolver signatures unchanged: `vfx::ResolveSdkPaths(std::string& binDir, std::string& modelDir, std::string& err)`, `ResolveArPaths(std::string& runtimeDir, std::string& modelDir, std::string& err)` (static, eyecontact.cpp), `vfx::PointProxiesAt`, `PointProxyAt`.

- [ ] **Step 1: Guard the Windows includes.** In each of `aigs.cpp`, `eyecontact.cpp`, `superres.cpp` (all inside their `#ifdef COS_HAS_MAXINE*` block) change `#include <windows.h>` to:

```cpp
#ifdef _WIN32
#include <windows.h>
#endif
```

- [ ] **Step 2: Portable env reads.** Replace each `GetEnvironmentVariableA("NAME", buf, sizeof(buf))` pattern (eyecontact.cpp `ResolveArPaths`, and its siblings in `vfx_paths.cpp`/`superres.cpp` if present — grep `GetEnvironmentVariableA`) with this helper (add once per file, or in `paths.h` if both use it — prefer `paths.h`):

```cpp
// paths.h addition:
// Portable env read: empty string when unset.
std::string EnvVar(const char* name);

// paths.cpp addition:
#include <cstdlib>
std::string EnvVar(const char* name) {
#ifdef _WIN32
    char buf[1024];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}
```

(`paths.cpp` will need its own `#ifdef _WIN32`-guarded `<windows.h>` include; check whether `ShimModuleDir()` there is Windows-only — if so, add the Linux implementation via `dladdr`:)

```cpp
#ifndef _WIN32
#include <dlfcn.h>
std::string ShimModuleDir() {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&ShimModuleDir), &info) || !info.dli_fname) return "";
    std::string p(info.dli_fname);
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? "" : p.substr(0, slash);
}
bool DirExists(const std::string& path); // implement with stat() S_ISDIR if the current one is Win32-only
#endif
```

- [ ] **Step 3: Linux resolver branches.** In `vfx_paths.cpp` `ResolveSdkPaths` and `eyecontact.cpp` `ResolveArPaths`, add a `#ifndef _WIN32` branch BEFORE the Windows logic (keep Windows logic untouched under `#else`). Linux search order (first hit wins), root = an SDK-core-shaped tree (`lib/`, `lib/models/`, `features/`):

```cpp
#ifndef _WIN32
    // Linux: COS_VFX_RUNTIME_DIR (SDK-core tree) -> <shimdir>/maxine/vfx (bundled, Phase 5).
    for (std::string root : { EnvVar("COS_VFX_RUNTIME_DIR"), ShimModuleDir() + "/maxine/vfx" }) {
        if (root.empty() || !DirExists(root + "/lib")) continue;
        binDir = root;                    // NOTE: root, not root/lib — Preload walks the whole tree
        modelDir = root + "/lib/models";
        return true;
    }
    err = "VFX runtime not found (set COS_VFX_RUNTIME_DIR to the VideoFX SDK root)";
    return false;
#else
    ... existing Windows logic ...
#endif
```

AR mirror: env `COS_AR_RUNTIME_DIR`, bundled tier `<shimdir>/maxine/ar`, err naming the AR var. `PointProxiesAt`/`PointProxyAt` on Linux call `PreloadMaxineClosure(binDir)` from Task 2 (add `#ifndef _WIN32` branches; the proxy-path globals stay Windows-only).

- [ ] **Step 4: Stub-config build stays green**

Run: `cmake -S native/shim -B native/shim/build && cmake --build native/shim/build`
Expected: builds clean (no SDK env set — passthrough stubs, `-Werror`).

- [ ] **Step 5: Commit**

```bash
git add native/shim && git commit -m "refactor(shim): portable guards + Linux runtime resolver branches (#27 Phase 4)"
```

---

### Task 2: `maxine_linux.{h,cpp}` — preload closure + GetOSInfo interposer

**Files:**
- Create: `native/shim/maxine_linux.h`, `native/shim/maxine_linux.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `void PreloadMaxineClosure(const std::string& sdkRoot);` (idempotent, called by both resolvers' `PointProxiesAt`/`PointProxyAt` Linux branches) and the exported C++ symbol `int GetOSInfo(const char*, std::string&)`.

- [ ] **Step 1: Write the files.** The SDK `.so` set has no RUNPATH, so nothing resolves by search path; fixpoint-dlopen every lib by absolute path with `RTLD_GLOBAL` — each pass loads whatever now has satisfied deps; SONAME reuse then serves every later plain-name dlopen (the AR proxy) and `DT_NEEDED`. Skip `*Triton*` (server variant, pulls extra deps deliberately not shipped).

```cpp
// maxine_linux.h
#pragma once
#ifndef _WIN32
#include <string>
// Loads every non-Triton .so under sdkRoot (lib/, external/*/lib/, features/*/lib/) by
// absolute path with RTLD_GLOBAL, iterating to a fixpoint (deps have no RUNPATH; SONAME
// reuse resolves later loads). Safe to call repeatedly and for both SDK roots.
void PreloadMaxineClosure(const std::string& sdkRoot);
#endif
```

```cpp
// maxine_linux.cpp
#ifndef _WIN32
#include "maxine_linux.h"
#include <dirent.h>
#include <dlfcn.h>
#include <cstring>
#include <string>
#include <vector>

namespace {
void CollectSos(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string full = dir + "/" + name;
        if (e->d_type == DT_DIR) { CollectSos(full, out); continue; }
        if (name.find(".so") != std::string::npos && name.find("Triton") == std::string::npos)
            out.push_back(full);
    }
    closedir(d);
}
} // namespace

void PreloadMaxineClosure(const std::string& sdkRoot) {
    std::vector<std::string> libs;
    CollectSos(sdkRoot + "/lib", libs);
    CollectSos(sdkRoot + "/external", libs);
    CollectSos(sdkRoot + "/features", libs);
    // Fixpoint: a pass that loads at least one new lib may unblock others.
    bool progress = true;
    std::vector<bool> loaded(libs.size(), false);
    while (progress) {
        progress = false;
        for (size_t i = 0; i < libs.size(); ++i) {
            if (loaded[i]) continue;
            if (dlopen(libs[i].c_str(), RTLD_NOW | RTLD_GLOBAL)) { loaded[i] = true; progress = true; }
        }
    }
    // Leftovers (unresolvable deps) are fine: the effect Probe reports precise failure.
}

// libVideoFXLocal.so's exported GetOSInfo(key, out) parses /etc/lsb-release and SEGFAULTS
// on non-Ubuntu distros (CachyOS: DISTRIB_RELEASE="rolling"). The shim loads before the
// SDK libs in its dlopen scope, so exporting the same mangled symbol interposes it.
// Claim Ubuntu 22.04 (a supported OS); rc=0 = success. Verified fix (spec §6 Spike A).
__attribute__((visibility("default")))
int GetOSInfo(const char* key, std::string& out) {
    const std::string k = key ? key : "";
    if (k == "ID") out = "ubuntu";
    else if (k.find("RELEASE") != std::string::npos || k.find("VERSION") != std::string::npos) out = "22.04";
    else out = "Ubuntu";
    return 0;
}
#endif // !_WIN32
```

- [ ] **Step 2: Stub build still green** (file compiles even without SDK env — it has no `COS_HAS_MAXINE` guard deliberately; the interposer is harmless in a stub shim).

Run: `cmake --build native/shim/build` after adding `maxine_linux.cpp` to `add_library` in CMakeLists (Linux always).
Expected: clean.

- [ ] **Step 3: Commit** — `git commit -m "feat(shim): Linux Maxine preload closure + GetOSInfo interposer (#27 Phase 4)"`

---

### Task 3: `nvvfx_proxy_linux.cpp` — the unported-proxy replacement

**Files:**
- Create: `native/shim/nvvfx_proxy_linux.cpp`

**Interfaces:**
- Consumes: `PreloadMaxineClosure` must have run first (symbols reachable via `RTLD_DEFAULT`) — guaranteed because `vfx::PointProxiesAt` runs before any NvVFX_* call (same contract the Windows proxy relies on).
- Produces: definitions of every NvVFX_*/NvCVImage_* symbol `aigs.cpp`/`superres.cpp` reference. AR needs none of this — compile NVIDIA's ported `nvARProxy.cpp` from the clone.

- [ ] **Step 1: Write the stubs.** Compiled only in the Maxine config (`COS_HAS_MAXINE`) on Linux. `decltype` on the header declarations keeps every signature exact — a mismatch becomes a compile error, not a crash:

```cpp
// Linux replacement for NVIDIA's NVVideoEffectsProxy.cpp + nvCVImageProxy.cpp, which are
// "#warning not ported" on Linux. PreloadMaxineClosure has already dlopen'd the runtime
// with RTLD_GLOBAL, so every symbol is reachable via RTLD_DEFAULT.
#if !defined(_WIN32) && defined(COS_HAS_MAXINE)
#include <dlfcn.h>
#include "nvCVImage.h"
#include "nvVideoEffects.h"

namespace {
template <typename Fn>
Fn* Sym(const char* name) { return reinterpret_cast<Fn*>(dlsym(RTLD_DEFAULT, name)); }
}

#define COS_FWD(fn, ...)                                   \
    static auto* f = Sym<decltype(fn)>(#fn);               \
    if (!f) return NVCV_ERR_LIBRARY;                       \
    return f(__VA_ARGS__)

NvCV_Status NvVFX_CreateEffect(NvVFX_EffectSelector code, NvVFX_Handle* obj) { COS_FWD(NvVFX_CreateEffect, code, obj); }
NvCV_Status NvVFX_DestroyEffect(NvVFX_Handle obj) { COS_FWD(NvVFX_DestroyEffect, obj); }
NvCV_Status NvVFX_SetString(NvVFX_Handle obj, NvVFX_ParameterSelector name, const char* str) { COS_FWD(NvVFX_SetString, obj, name, str); }
NvCV_Status NvVFX_SetU32(NvVFX_Handle obj, NvVFX_ParameterSelector name, unsigned int val) { COS_FWD(NvVFX_SetU32, obj, name, val); }
NvCV_Status NvVFX_SetImage(NvVFX_Handle obj, NvVFX_ParameterSelector name, NvCVImage* im) { COS_FWD(NvVFX_SetImage, obj, name, im); }
NvCV_Status NvVFX_SetCudaStream(NvVFX_Handle obj, NvVFX_ParameterSelector name, CUstream stream) { COS_FWD(NvVFX_SetCudaStream, obj, name, stream); }
NvCV_Status NvVFX_CudaStreamCreate(CUstream* stream) { COS_FWD(NvVFX_CudaStreamCreate, stream); }
NvCV_Status NvVFX_CudaStreamDestroy(CUstream stream) { COS_FWD(NvVFX_CudaStreamDestroy, stream); }
NvCV_Status NvVFX_CudaStreamSynchronize(CUstream stream) { COS_FWD(NvVFX_CudaStreamSynchronize, stream); }
NvCV_Status NvVFX_Load(NvVFX_Handle obj) { COS_FWD(NvVFX_Load, obj); }
NvCV_Status NvVFX_Run(NvVFX_Handle obj, int async) { COS_FWD(NvVFX_Run, obj, async); }

NvCV_Status NvCVImage_Alloc(NvCVImage* im, unsigned width, unsigned height, NvCVImage_PixelFormat format, NvCVImage_ComponentType type, unsigned layout, unsigned memSpace, unsigned alignment) { COS_FWD(NvCVImage_Alloc, im, width, height, format, type, layout, memSpace, alignment); }
NvCV_Status NvCVImage_Dealloc(NvCVImage* im) { COS_FWD(NvCVImage_Dealloc, im); }
NvCV_Status NvCVImage_Init(NvCVImage* im, unsigned width, unsigned height, int pitch, void* pixels, NvCVImage_PixelFormat format, NvCVImage_ComponentType type, unsigned layout, unsigned memSpace) { COS_FWD(NvCVImage_Init, im, width, height, pitch, pixels, format, type, layout, memSpace); }
NvCV_Status NvCVImage_Transfer(const NvCVImage* src, NvCVImage* dst, float scale, CUstream stream, NvCVImage* tmp) { COS_FWD(NvCVImage_Transfer, src, dst, scale, stream, tmp); }
#endif
```

**Verify against the headers before committing:** every signature above must match `~/dev/Maxine-VFX-SDK/nvvfx/include/{nvVideoEffects.h,nvCVImage.h}` exactly (grep the declarations); `decltype` will error on drift, but argument names/count in the forward calls are hand-written. Also grep `aigs.cpp`/`superres.cpp` for any NvVFX_*/NvCVImage_* symbol not covered (e.g. `NvVFX_SetF32`, `NvCVImage_Create`) and add it in the same pattern.

- [ ] **Step 2: Commit** — `git commit -m "feat(shim): Linux VFX/NvCVImage dlsym proxy (#27 Phase 4)"`

---

### Task 4: CMake Maxine configuration

**Files:**
- Modify: `native/shim/CMakeLists.txt`

**Interfaces:**
- Consumes: env `COS_VFX_SDK_DIR` (VFX header/proxy clone, e.g. `~/dev/Maxine-VFX-SDK`), `COS_AR_SDK_DIR` (AR clone). Same env names as the Windows build (CLAUDE.md contract).
- Produces: cmake-time `COS_HAS_MAXINE`/`COS_HAS_MAXINE_AR` defines + sources, exactly like the vcxproj's conditional model. Unset env → today's stub build byte-for-byte.

- [ ] **Step 1: Add the conditional block** (after the existing `add_library`; adjust the source list additively):

```cmake
# Maxine (Phase 4): headers + the ported AR proxy come from the NVIDIA-Maxine GitHub
# clones (COS_VFX_SDK_DIR / COS_AR_SDK_DIR env, same names as the Windows build).
# Unset -> CI-safe passthrough stubs, identical to before.
target_sources(shim PRIVATE maxine_linux.cpp)
if(DEFINED ENV{COS_VFX_SDK_DIR} AND EXISTS "$ENV{COS_VFX_SDK_DIR}/nvvfx/include/nvVideoEffects.h")
    target_compile_definitions(shim PRIVATE COS_HAS_MAXINE)
    target_include_directories(shim PRIVATE "$ENV{COS_VFX_SDK_DIR}/nvvfx/include")
    target_sources(shim PRIVATE nvvfx_proxy_linux.cpp)
    message(STATUS "Maxine VFX: ENABLED ($ENV{COS_VFX_SDK_DIR})")
else()
    message(STATUS "Maxine VFX: stub (COS_VFX_SDK_DIR unset)")
endif()
if(DEFINED ENV{COS_AR_SDK_DIR} AND EXISTS "$ENV{COS_AR_SDK_DIR}/nvar/include/nvAR.h")
    target_compile_definitions(shim PRIVATE COS_HAS_MAXINE_AR)
    target_include_directories(shim PRIVATE "$ENV{COS_AR_SDK_DIR}/nvar/include")
    target_sources(shim PRIVATE "$ENV{COS_AR_SDK_DIR}/nvar/src/nvARProxy.cpp")
    message(STATUS "Maxine AR: ENABLED ($ENV{COS_AR_SDK_DIR})")
else()
    message(STATUS "Maxine AR: stub (COS_AR_SDK_DIR unset)")
endif()
target_link_libraries(shim PRIVATE ${CMAKE_DL_LIBS})
```

Note: NVIDIA's `nvARProxy.cpp` will not survive `-Werror -Wall -Wextra` untouched — wrap its compilation with relaxed flags: `set_source_files_properties("$ENV{COS_AR_SDK_DIR}/nvar/src/nvARProxy.cpp" PROPERTIES COMPILE_OPTIONS "-w")` (third-party source, not ours). Same for any warning the VFX headers trigger — prefer `SYSTEM` include dirs (`target_include_directories(shim SYSTEM PRIVATE ...)`).

The AR include dir must also come BEFORE the core-SDK includes if both define NvAR macros; the clone's `nvAR_defs.h` (AR 1.1.1.0) dropped `NvAR_Feature_*` macros — `eyecontact.cpp` already defines the `"GazeRedirection"` literal fallback (repo gotcha, verified present at eyecontact.cpp:20).

- [ ] **Step 2: Both configs build**

```bash
# stub
rm -rf native/shim/build && cmake -S native/shim -B native/shim/build && cmake --build native/shim/build
strings native/shim/build/libCameraOnScreen.Shim.so | grep -c "not built in"   # expect >= 2
# SDK
export COS_VFX_SDK_DIR=$HOME/dev/Maxine-VFX-SDK COS_AR_SDK_DIR=$HOME/dev/Maxine-AR-SDK
rm -rf native/shim/build && cmake -S native/shim -B native/shim/build && cmake --build native/shim/build
strings native/shim/build/libCameraOnScreen.Shim.so | grep -c "not built in"   # expect 0 for GS/gaze (FRUC stub line may remain — check it's the FRUC one only)
nm -D native/shim/build/libCameraOnScreen.Shim.so | grep GetOSInfo             # interposer exported
```

- [ ] **Step 3: Commit** — `git commit -m "build(shim): optional Maxine config on Linux (COS_VFX_SDK_DIR/COS_AR_SDK_DIR) (#27 Phase 4)"`

---

### Task 5: Effect chain in `capture_v4l2.cpp` WorkerLoop

**Files:**
- Modify: `native/shim/capture_v4l2.cpp` (the `// Phase 4 (Maxine on Linux) inserts the effect chain here` marker)

**Interfaces:**
- Consumes: `Aigs`/`EyeContact` classes (`Start/Stop/ProcessFrame/IsReady/LastError`); the atomics already in `capture_v4l2.cpp`'s `CaptureState` (they mirror `capture.cpp` — verify exact member names in BOTH files before writing; the reference implementation is `capture.cpp:445-505`).
- Produces: live effects on the Linux capture path; status atomics (`green_screen_active`, `eye_contact_active`, effect error strings via the leaf lock) update exactly as Windows does.

- [ ] **Step 1: Mirror the chain.** Inside `WorkerLoop`, after the BGRA conversion and BEFORE publishing the frame (order: eye contact → green screen), add — adapting atomic/member names to `capture_v4l2.cpp`'s actual `CaptureState` (read both files first; do not guess):

```cpp
    // Worker-thread-local Maxine objects (CUDA thread affinity — repo contract).
    Aigs aigs;
    EyeContact eyeContact;
    ...
    // per frame, frame now BGRA in buf/w/h:
    {
        const bool wantEc = g_state.eyeContactEnabled.load(std::memory_order_relaxed);
        if (wantEc && !eyeContact.IsReady()) {
            if (!eyeContact.Start()) SetEffectError(eyeContact.LastError());   // leaf-lock setter, mirror capture.cpp
        } else if (!wantEc && eyeContact.IsReady()) {
            eyeContact.Stop();
        }
        bool ecApplied = false;
        if (wantEc && eyeContact.IsReady())
            ecApplied = eyeContact.ProcessFrame(buf.data(), w, h);
        g_state.eyeContactActive.store(ecApplied, std::memory_order_relaxed);

        const bool wantGs = g_state.greenScreenEnabled.load(std::memory_order_relaxed);
        if (wantGs && !aigs.IsReady()) {
            if (!aigs.Start()) SetEffectError(aigs.LastError());
        } else if (!wantGs && aigs.IsReady()) {
            aigs.Stop();
        }
        bool gsApplied = false;
        if (wantGs && aigs.IsReady())
            gsApplied = aigs.ProcessFrame(buf.data(), w, h,
                g_state.greenScreenExpand.load(std::memory_order_relaxed),
                g_state.greenScreenFeather.load(std::memory_order_relaxed));
        g_state.greenScreenActive.store(gsApplied, std::memory_order_relaxed);
    }
    ...
    // teardown before closing the device (thread affinity): eyeContact.Stop(); aigs.Stop();
```

Alpha contract: passthrough already forces alpha=255; `Aigs::ProcessFrame` overwrites alpha with the matte — no extra work. If `capture_v4l2.cpp` lacks expand/feather/error atomics that `capture.cpp` has, add them mirroring `capture.cpp`'s exact naming and the leaf-lock error pattern (never nested under `g_state.mtx`).

- [ ] **Step 2: Stub + SDK builds clean** (both cmake configs, as Task 4 Step 2).

- [ ] **Step 3: Live probe on the box (real camera + 3090).** Build SDK config LAST, then:

```bash
export COS_VFX_RUNTIME_DIR=$HOME/dev/VideoFX-linux/VideoFX COS_AR_RUNTIME_DIR=$HOME/dev/ARSDK-linux/ARSDK
./native/shim/build/v4l2_probe
```
Expected: capabilities report green screen + gaze AVAILABLE (probe strings from the real `Probe()`), camera capture still passes. If probe segfaults in `GetOSInfo`, the interposer isn't binding — check `nm -D` export and that the shim loads before SDK libs (it does: PreloadMaxineClosure runs from inside the shim).

- [ ] **Step 4: Commit** — `git commit -m "feat(shim): Maxine effect chain in the V4L2 worker (#27 Phase 4)"`

---

### Task 6: End-to-end app gate + docs + bookkeeping

**Files:**
- Modify: `CLAUDE.md` (Linux section), issue #27, PR from `feat/linux-phase4-maxine`

- [ ] **Step 1: App run (HUMAN GATE).** SDK shim built last; `dotnet build src/CameraOnScreen.App.Avalonia/...` (deploys the .so), then run with the two `COS_*_RUNTIME_DIR` vars + display env. Verify: effect toggles ungrey after the deferred probe; green screen ON → background transparent in the overlay (desktop shows through); eye contact ON → gaze locks to camera; toggles flip live mid-capture; Stop/Start round-trip stable; panel close clean (no terminate).

- [ ] **Step 2: CLAUDE.md.** Append to the Linux build section:

```
Phase 4 Maxine-on-Linux: build the shim with COS_VFX_SDK_DIR=~/dev/Maxine-VFX-SDK
COS_AR_SDK_DIR=~/dev/Maxine-AR-SDK (GitHub header/proxy clones; unset = CI stub). Runtime:
COS_VFX_RUNTIME_DIR=~/dev/VideoFX-linux/VideoFX COS_AR_RUNTIME_DIR=~/dev/ARSDK-linux/ARSDK
(SDK-core trees; refetch via scripts/fetch-maxine-linux.sh + ngc CLI, see spec §6). Same
co-versioned pins as Windows (TRT 10.9.0 / CUDA 12.8). The SDK .so set has no RUNPATH —
the shim preloads the closure by absolute path (maxine_linux.cpp) and interposes
libVideoFXLocal's GetOSInfo (segfaults on non-Ubuntu distros). VFX/NvCVImage proxies are
NOT ported by NVIDIA on Linux — nvvfx_proxy_linux.cpp is ours; the AR proxy is NVIDIA's.
FRUC + super-res stay stubbed/greyed on Linux (Optical Flow SDK download + NGX VSR
verification pending).
```

- [ ] **Step 3: Push, PR to `main`, tick the #27 Phase 4 box** (note FRUC exclusion in the comment), file a follow-up issue for FRUC-on-Linux + NGX VSR verification.

---

## Self-Review

- **Spec coverage:** spec §7 Phase 4 = "paths.cpp Linux tier, co-versioned maxine/ stage, green screen + gaze (Spike A passed) + FRUC". Tasks 1–2 cover paths/discovery; the "stage" is the SDK trees for dev (bundled `<shim>/maxine/{vfx,ar}` tier resolves already — assembling that bundle belongs to Phase 5 packaging); Tasks 3–5 the effects; FRUC explicitly out (SDK unobtainable non-interactively — follow-up issue, spec kill-criterion honored by grey-out).
- **Placeholders:** none; the two "verify names against the file" directives are deliberate read-the-code steps with the reference implementation cited, not TBDs.
- **Type consistency:** resolver signatures match `vfx_paths.h`/`eyecontact.cpp` as read this session; `ProcessFrame` signatures match `aigs.h`/`eyecontact.h` verbatim; proxy signatures are `decltype`-checked at compile time against the real headers.
