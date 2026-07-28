# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Camera-on-Screen: a Windows webcam **desktop-overlay** app. A transparent, always-on-top, draggable overlay shows the live webcam so any screen recorder captures it live in one pass — no post-edit. Single-process **C# .NET 8 + WinUI 3** control panel + a native **C++ C-ABI shim** (P/Invoke) doing Media Foundation capture. **Windows + NVIDIA RTX only** (Maxine effects are RTX-locked). The C# side owns all windowing/compositing; the shim only captures and runs the optional Maxine effects.

Design intent lives in `docs/superpowers/specs/`; task plans in `docs/superpowers/plans/`. **Read the relevant spec before changing cross-component contracts.** Deferred work is tracked as GitHub issues.

## Toolchain (host-specific, non-obvious)

- .NET 8 SDK; **VS2022 Build Tools + MSVC v143** (no full Visual Studio). There is **no WinUI workload, no MSIX tooling, no `dotnet new winui3` template** on this host.
- Because of that, the App is built **unpackaged + self-contained via NuGet only** (`WindowsPackageType=None`, `WindowsAppSDKSelfContained=true`, no `Package.appxmanifest`).
- The C++ `native/shim/shim.vcxproj` is **deliberately NOT in `CameraOnScreen.sln`** — a C++ project breaks `dotnet build`/`dotnet test` (the SDK MSBuild lacks C++ targets). Build it separately first; the App copies the produced DLL via a `<None>` item in its csproj.
- Builds and tests must be **pristine (0 warnings)** — warnings are treated as findings (CI enforces `/warnaserror` + `TreatWarningsAsErrors`).
- **Inno Setup 6** (`ISCC.exe`) is required to build the installer (issue #1):
  `winget install JRSoftware.InnoSetup`. Not needed for normal build/test.

## Build & test

The native shim must be built **before** the App (the App copies `native/shim/x64/$(Configuration)/CameraOnScreen.Shim.dll` next to its exe).

```powershell
# 1. Native shim — Build Tools MSBuild, from PowerShell (Git Bash mangles MSBuild /p: switches).
#    Set COS_VFX_SDK_DIR (green screen) + COS_AR_SDK_DIR (eye contact) to enable both Maxine
#    effects; COS_FRUC_SDK_DIR (NVIDIA Optical Flow SDK root) enables FRUC frame-interpolation.
#    WITHOUT a var, the corresponding effect builds a CI-safe passthrough STUB.
#    Build compiles against the VFX 1.2.0.0 + AR 1.1.1.0 headers/proxy; the co-versioned RUNTIME
#    pair is VFX 1.2.0.0 + AR 1.1.1.0 / TRT 10.9 (see CO-VERSION below).
#    CosCudaDir MSBuild prop (default: C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2)
#    supplies CUDA headers + cuda.lib for the FRUC build; override if your toolkit path differs.
$env:COS_VFX_SDK_DIR  = "C:\dev\VideoFX"            # VFX 1.2.0.0 source (build)
$env:COS_AR_SDK_DIR   = "C:\dev\Maxine-AR-SDK"      # AR 1.1.1.0 source clone (build)
$env:COS_FRUC_SDK_DIR = "C:\dev\Optical_Flow_SDK"   # Optical Flow SDK root (build; enables FRUC)
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
# Output (x64 only): native/shim/x64/Debug/CameraOnScreen.Shim.dll

# 2. App (pulls in Core, copies the shim DLL). -t:Rebuild avoids a transient XAML warning.
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild

# 3. Core unit tests (xUnit, net8.0). Single test: append --filter "FullyQualifiedName~Name".
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj

# Run with effects (M4+). Runtime resolution, first hit wins:
#   COS_VFX_RUNTIME_DIR (dev override) -> VFX 1.2.0.0 runtime (flat: DLLs in root, models in \models)
#   COS_AR_RUNTIME_DIR / %ProgramFiles%\NVIDIA Corporation\NVIDIA AR SDK (AR)
#   COS_FRUC_RUNTIME_DIR (dev override) -> Optical Flow SDK runtime (NvOFFRUC.dll + cudart64_110.dll)
#   <app>\maxine\ (bundled, beside the exe — no env vars needed; see Bundler)
# Without any -> raw passthrough, effects disabled (no crash).
$env:COS_VFX_RUNTIME_DIR = "C:\dev\maxine-stage"  # assembled co-versioned stage (assemble-maxine-stage.ps1)
src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.App.exe
```

`dotnet build CameraOnScreen.sln` builds the three SDK-style projects (Core, App, tests) but **not** the vcxproj. After a shim ABI change, verify with `dumpbin /exports` (under MSVC `bin/Hostx64/x64/`). Note the `.sln` cannot build on Linux (the WinUI App project needs Windows targeting + a Windows-only XAML compiler) — build the projects below individually there.

### Linux build (Phase 2+, issue #27; primary dev box since 2026-07)

The same shim sources build as `libCameraOnScreen.Shim.so` via CMake with a V4L2 capture backend (`capture_v4l2.cpp`; Windows keeps `capture.cpp` + the vcxproj, which stays untouched). Effects are the CI-safe passthrough stubs unless the Maxine SDK env vars are set at cmake time (Phase 4, below). The Avalonia panel (`src/CameraOnScreen.App.Avalonia`) is the Linux control panel; it uses its own app-layer `PInvokeShim` (extension-less lib name so probing finds `.dll`/`.so` per-OS) and falls back to the `FakeShim` demo VM when the `.so` is absent. Phase 3 overlay: `Overlay/OverlayWindow.cs` — frameless transparent Topmost Avalonia window (Spike B winner (a), OBS-verified on KWin/XWayland), own 33 ms pump reading `TryGetFrame`; opens/closes with Start/Stop; drag = `BeginMoveDrag`, wheel-resize = `OverlaySizer`, geometry persists to config on panel close. Premultiplied alpha ready for Phase 4 mattes.

```bash
cmake -S native/shim -B native/shim/build && cmake --build native/shim/build   # -Wall -Wextra -Werror
./native/shim/build/v4l2_probe   # ABI smoke: enumerate + caps (+3s capture when a camera exists)
dotnet build src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj  # auto-copies the .so
dotnet run --project src/CameraOnScreen.App.Avalonia   # X11/XWayland; real shim if built
```

Config lands at `$XDG_CONFIG_HOME/CameraOnScreen/config.json` (Windows keeps `%LOCALAPPDATA%`). Hosted Linux CI: `.github/workflows/ci-linux.yml` (no GPU/camera needed — stub shim; `v4l2_probe` passes with 0 cameras by design). The old `[self-hosted, windows, rtx]` runner died with the Windows dev box, so `ci.yml` jobs stay queued on PRs until Phase 5 re-homes them. V4L2 format support is XBGR32/BGR24/RGB24/YUYV (fps-first scoring, ≤1080p) — MJPEG-only high-res modes (e.g. Brio 100 1080p30) fall back to lower-res YUYV until a libjpeg decode path is added.

Phase 4 Maxine-on-Linux (green screen + eye contact): build the shim with
`COS_VFX_SDK_DIR=~/dev/VideoFX-linux/VideoFX COS_AR_SDK_DIR=~/dev/ARSDK-linux/ARSDK` — the
**Linux SDK-core trees** (headers + features + libs; refetch via `scripts/fetch-maxine-linux.sh`
+ ngc CLI, spec §6), NOT the GitHub header clones (they lack `nvVFXGreenScreen.h`). Unset = CI
stub. Runtime: `COS_VFX_RUNTIME_DIR`/`COS_AR_RUNTIME_DIR` point at the same trees (bundled tier
`<shim>/maxine/{vfx,ar}` = Phase 5). Same co-versioned pins as Windows (TRT 10.9 / CUDA 12.8);
`Aigs::Probe` runs first, so VFX 1.2's `libNVCVImage.so.1` wins first-load for both effects. The
SDK `.so` set has no RUNPATH — `maxine_linux.cpp` preloads the closure by absolute path
(`RTLD_GLOBAL`, fixpoint), promotes the shim itself to the global scope (P/Invoke dlopens it
`RTLD_LOCAL`), and interposes `libVideoFXLocal`'s `GetOSInfo` (segfaults parsing
`/etc/lsb-release` on non-Ubuntu distros). **No NVIDIA proxy compiles on Linux** (VFX/NvCVImage
are `#warning not ported`; `nvARProxy.cpp`'s `getNvARLib()` uses unguarded TCHAR) —
`maxine_proxy_linux.cpp` is ours: hidden-visibility dlsym forwarders for all three surfaces
(hidden so `dlsym(RTLD_DEFAULT)` can't self-resolve and recurse). **Preload gotchas (cost a
debugging cycle):** `external/cuda/lib/stubs/` are link-time stubs with the REAL SONAMEs —
loading one silently breaks `NvVFX_Run` ("stub version of nppi*" on stderr); the closure skips
`stubs/` dirs and dedups lib basenames across the two trees (one runtime, VFX's copies win —
the AR gaze feature binds VFX's `libVideoFXLocal` internals via the global scope, so the trees
are NOT independent). Ordering invariant: the process's FIRST `NvVFX_Load` must precede any
`NvAR_Load`/`NvCVImage_Alloc`, else `libNVCVImage` caches an internal init failure and every
later alloc returns `NVCV_ERR_LIBRARY` — guaranteed in-app because the capability probe
(`Aigs::Probe` first) always runs before effects can enable; don't drive the raw ABI with
effects on and no prior `cos_query_capabilities`. Super-res + FRUC stay
stubbed/greyed on Linux (no VSR header/feature in the Linux VFX SDK; Optical Flow SDK download
pending). Deploy-the-right-shim check: `nm -D` shows `GetOSInfo` and `strings` lacks
`"not built in"` = SDK build.

**DEPLOY THE RIGHT SHIM (cost a full debugging cycle).** The SDK build (`COS_HAS_MAXINE*`) and the CI stub (`/p:CosVfxSdkDir= /p:CosArSdkDir=`) write the **same** DLL path; whichever built **last** is what App `-t:Rebuild` deploys. Always build the SDK config **last** before running, else the app silently runs passthrough (toggles greyed). Verify the deployed DLL: `grep -a GreenScreen` **and** `grep -a GazeRedirection` present, `grep -a "not built in"` absent.

**CO-VERSION (M4/M5, cost a full cycle).** The Maxine **VFX** (green screen, `NvVFX_*`) and **AR** (eye contact / gaze, `NvAR_*`) SDKs each bundle an exact, pinned CUDA + TensorRT runtime. Two different TRT/CUDA runtimes **cannot coexist** in one process — same DLL names (`nvinfer_10.dll`, `cudart64_12.dll`, `NVCVImage.dll`), first `LoadLibrary` wins, loser's `NvVFX_Load`/`NvAR_Load` fails with `cudaErrorNoKernelImageForDevice`. The verified pair is **VFX 1.2.0.0 + AR 1.1.1.0** — both ship **TensorRT 10.9 / CUDA 12.x** (`nvinfer_10.dll`/`cudart64_12.dll` byte-identical in body, re-signed). Both SDKs use a **dispatcher + per-feature-DLL** model: `NVVideoEffects.dll`/`nvARPose.dll` are dispatchers; the real effects are `nvVFXGreenScreen.dll` and the AR feature DLLs (`nvARGazeRedirection`/`nvARFaceBoxDetection`/`nvARLandmarkDetection`) — all must sit beside the dispatcher in the flat `maxine\`. `nvARFaceExpressions` is excluded (not in the gaze closure; emotion recognition disallowed by AI Product-Specific Terms §8.17). AR 1.1.1.0 dropped the `NvAR_Feature_*` macros from `nvAR_defs.h` — `eyecontact.cpp` defines the `"GazeRedirection"` literal if absent. **FRUC is co-version-safe with Maxine VFX/AR** — it uses no TensorRT and ships its own `cudart64_110.dll` (CUDA 11; distinct name from the stack's `cudart64_12.dll`), so the first-load-wins hazard does not apply. Proven in-process on RTX 3090.

## Architecture — contracts that span files

Three projects: `src/CameraOnScreen.Core` (pure .NET 8 logic, no WinUI/Win32 types, fully unit-tested), `src/CameraOnScreen.App` (WinUI 3 + raw Win32/D3D), `native/shim` (C++ Media Foundation DLL). These rules cut across files — violating one silently breaks the pipeline:

- **The shim never creates a window and never renders.** It captures (Media Foundation), runs the optional Maxine effects, and exposes a C ABI (`cos_init/enumerate_cameras/set_params/start/stop/get_status/get_frame/query_capabilities/shutdown`, `extern "C"`, 9 exports). C# does 100% of windowing/compositing.
- **Single shared D3D11 device, no shared handles.** `OverlayWindow` creates the device and passes `D3DDevicePtr` to `cos_init`. Never `OpenSharedResource`. Capture **and** the Maxine path are **CPU-copy** (frames round-trip CPU↔GPU around the effect); the shared device is reserved for the deferred zero-copy path and is currently **unused**.
- **Status is polled, never pushed.** Frame-pump timer → `Vm.PollStatusTick()` → `Orchestrator.PollStatus()` → `shim.GetStatus()`. No native→managed callbacks. (`cos_get_status` returns a hardcoded `30.0` fps stub.)
- **Effect params are pushed live, not only at Start.** The MVVM `On…Changed` partials (`GreenScreenEnabled/Strength/Expand/Feather`, `EyeContact*`, `Mirror`, `Zoom`) call `ApplyLiveParams()` → `Orchestrator.ApplyParams(BuildParams())` → `shim.SetParams` (atomic enable-flag flip, UI-thread-safe). Live push is **gated on `IsRunning`**; `ApplyParams` forces effects off when `EffectsAvailable` is false.
- **C ABI struct parity is load-bearing.** `CosStatus`/`CosParams`/`CosCaps` (C, `shim.h`) and their `[StructLayout(Sequential)]` mirrors in `PInvokeShim` must match byte-for-byte on x64 (`CosCaps` = two `int` gates + `char detail[512]` = 520 bytes). `camera_id`/`detail` are UTF-8. The 128-byte enumeration stride is duplicated in native `cos_enumerate_cameras` and `PInvokeShim.ReadUtf8` — keep in sync.
- **A native probe gates effects, not the GPU tier.** `Orchestrator` sets `EffectsAvailable`/`EyeContactAvailable`/`CapabilityDetail` from `cos_query_capabilities`, which actually tries to create+load each Maxine effect. `GpuTierDetector` is now **display-only** (GPU name string). The probe is **deferred** off the UI thread (`MainViewModel.ProbeCapabilitiesAsync()` from the `MainWindow` ctor); effects gate OFF until it lands. XAML toggle/note bindings are `Mode=OneWay`.
- **Maxine effects run on the capture worker thread** (CUDA/NvAR thread affinity). The `Aigs` (green screen, `aigs.{h,cpp}`) and `EyeContact` (gaze, `eyecontact.{h,cpp}`) objects are worker-thread-local; the UI only flips an atomic enable flag; status crosses threads via atomics + a **leaf-lock** (never nested under `g_state.mtx`/`g_lifecycleMtx`). Eye contact runs before green screen. Per frame (green screen): upload CPU BGRA→GPU, `NvVFX_Run`, download matte, composite (A = matte, RGB premultiplied, honoring matte pitch; matte ops order = dilate → feather → premultiply). Compiled behind `COS_HAS_MAXINE`/`COS_HAS_MAXINE_AR`; without them the shim is a passthrough stub. **FRUC** (`fruc.{h,cpp}`, `COS_HAS_FRUC`) runs last in the worker chain — a streaming effect (feed-once, holds prev frame); per camera frame it double-publishes (mid-frame interpolated first, then the real frame). The C# pump runs at **16 ms** (≈60 Hz) while FRUC is active.
- **Layered overlay.** `OverlayWindow` is a raw Win32 window (`WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP`, `WS_POPUP`) with a DirectComposition flip-model swap chain (`B8G8R8A8_UNorm`, `AlphaMode.Premultiplied`) — a **separate window** from the WinUI control panel. Mirror + center-zoom fold into the single DComp visual `Matrix3x2` in `UpdateScale`.
- **Persistence:** JSON at `%LOCALAPPDATA%\CameraOnScreen\config.json`, saved on `WM_EXITSIZEMOVE` and window close only (never per `WM_MOVE`/`WM_SIZE`). `MainViewModel.ToAppConfig` builds a fresh `AppConfig` — anything not copied reverts to defaults (retain loaded hotkeys).

## Gotchas that cost real debugging

- **Win32 interop structs need their own `CharSet`.** `WNDCLASSEX` must set `CharSet = CharSet.Unicode`; a `DllImport`'s `CharSet` does NOT govern by-ref struct string fields. Wrong → `lpszClassName` marshals ANSI → class-name mismatch → `CreateWindowEx` fails (1407) → startup fail-fast (`0xC0000409`). App.xaml.cs logs top-level exceptions to `%LOCALAPPDATA%\CameraOnScreen\startup-error.log` — check it on any startup crash.
- **You cannot GDI-screenshot the overlay.** `WS_EX_NOREDIRECTIONBITMAP` + DComp flip-model (promoted to a hardware MPO plane on RTX) makes GDI `BitBlt`/`CopyFromScreen` return **black** — by design, the same property that lets real recorders capture it cleanly. Verify via Windows.Graphics.Capture / OBS / Game Bar, or instrument the frame pump. **Visual/recorder confirmation is an inherent human gate** (`docs/superpowers/verification/`).
- **Overlay move/resize is hook+timer, NOT `WM_NCHITTEST` (cost a full debug cycle).** At larger sizes the DComp swap chain is promoted to a hardware **MPO plane**, so DWM loses the per-pixel alpha it uses to hit-test the `WS_EX_LAYERED` window over the video → clicks on the video **pass through** (`WM_NCHITTEST`/drag-anywhere is unreliable, size-dependent). So a global `WH_MOUSE_LL` hook (`OverlayMouseHook`, installed on + fired on the UI thread) drives both: **wheel** → resize (`OverlaySizer`+`SetBounds`), and **left-down on the centre "+" handle** → drag. **The drag MOVE must be polled on the frame-pump timer via `GetCursorPos`, never `SetBounds` inside the hook's `WM_MOUSEMOVE`** — moving the window under the cursor emits synthesized moves that re-enter the hook = an ~800 Hz feedback loop that pins the overlay (±1 px jitter). The hook only starts/ends the drag and swallows the click; the 30 Hz timer does `SetBounds(GetCursorPos − grabOffset)`.
- **`CopyResource` cannot scale.** The swap chain is pinned to the **camera's native resolution** (so `PresentFrame`'s `CopyResource` is a valid 1:1 copy); scale to the window via the DComp **visual transform** (`SetTransform` on `WM_SIZE`). Do not `ResizeBuffers` to the window size.
- **Alpha is opaque in passthrough, the matte with green screen on.** MF `RGB32` is BGRX (undefined alpha); the premultiplied overlay renders alpha=0 as transparent. Passthrough forces alpha=255; green screen overwrites alpha with the matte. Honor the source row stride (`MFGetStrideForBitmapInfoHeader`, signed) — assuming `width*4` gives skewed frames.
- **Capture threading:** a worker thread fills a mutex-guarded frame buffer; `Start`/`Stop` are serialized by a *separate* lifecycle mutex (so `join()` never blocks `LatestFrame`). Never call into the worker-thread-local Maxine objects from the UI thread — toggle via the atomic flag only.
- **`MainViewModel.Dispose()` must dispose the shim** (`cos_shutdown` → joins the capture worker), else the global `std::thread` is destroyed joinable at process exit → `std::terminate`/debug abort dialog.

## Maxine SDKs (not in repo; redistribution governed by the 2025 NVIDIA Software License + Open/Community Model Licenses)

- **VFX** green screen (`nvvfxgreenscreen`) and **AR** eye contact (gaze) are separate NVIDIA products. No import `.lib` — link via the SDKs' proxy stubs (`nvVideoEffectsProxy.cpp`, `nvCVImageProxy.cpp`, `nvARProxy.cpp`) compiled into the shim. Models are prebuilt per-arch TensorRT engines; the bundle ships **sm75/86/89/100** (Turing/Ampere/Ada/Blackwell) — fetched from NGC by arch. **sm86 is the only arch verified on real silicon (RTX 3090); the others ship best-effort and grey out gracefully if an engine fails to deserialize.**
- **VFX feature catalog — verify availability + the REAL API BEFORE coding an effect (cost ~6 wasted tasks once).** Authoritative installable set: `<VFX_SDK>\features\install_feature.ps1 -list_features` (NGC key via `NGC_CLI_API_KEY`). VFX 1.2.0.0 = `aigsrelighting, backgroundblur, denoising, greenscreen, relighting, transfer, upscale, videosuperres` (8). **There is NO `nvvfxartifactreduction`** — it's in the script's help text but NOT the catalog (removed since older VFX SDKs); coding against it = a dead, always-unavailable effect. **Super Resolution = NGX VSR** (`nvvfxvideosuperres`; ships `nvngx_vsr.dll`, NOT a per-arch TRT engine — "no models" is normal): real header `features\nvvfxvideosuperres\include\nvVFXVideoSuperRes.h` → selector `NVVFX_FX_VIDEO_SUPER_RES "VideoSuperRes"` + param `NVVFX_QUALITY_LEVEL` (NOT `"SuperRes"`/`NVVFX_MODE`/scale-from-dims). A feature's real selector + params + image format live ONLY in its per-feature header (`features\<name>\include\*.h`), installed on demand — never infer them; if the NGC key is the gate, get it up front.
- **App-relative discovery** (`paths.{h,cpp}` `ShimModuleDir()` via `GetModuleHandleExW(FROM_ADDRESS)`, CWD-independent): both resolvers gain an `<app>\maxine\` tier so a shipped app finds the runtimes beside the exe with no env vars. Single shared co-versioned `maxine\` root (one TRT/CUDA runtime, dispatcher + feature DLLs, `models\vfx` + `models\ar`).
- **Stage + Bundler.** Two steps now (dispatcher + per-feature-DLL SDK layout): (1) `scripts/assemble-maxine-stage.ps1` curates a co-versioned flat **stage** from the VFX 1.2.0.0 + AR 1.1.1.0 SDK trees (shared DLLs from VFX; AR dispatcher + 3 gaze feature DLLs from AR; the 6 license files; multi-arch engines via `scripts/fetch-maxine-engines.ps1`, NGC key). (2) `scripts/bundle-maxine.ps1 -OutDir X -MaxineStage <stage>` PRUNES the stage to the manifest's verified load-closure (`native/shim/bundle/maxine-manifest.psd1`; the 19-DLL `Dlls` list was produced by `native/shim/smoke/trace_closure.cpp`) + model globs + required `LicenseFiles`, into `<output>\maxine\`. Co-version is enforced at stage assembly, not by the bundler. `trace_closure`/`bundle_probe` re-run against the produced bundle (`COS_*` unset → both effects load) is the verify gate. End-user need: an **RTX GPU + recent driver**; no NVIDIA account or SDK download.
- **Installer** (`scripts/bundle-maxine.ps1` consumer): `scripts/build-installer.ps1 -MaxineStage <stage>`
  builds the App **.NET-self-contained**, export-verifies the deployed shim, prunes the stage
  into `<staging>\maxine\`, then compiles `installer/CameraOnScreen.iss` with Inno Setup 6 →
  `dist/CameraOnScreen-Setup-<ver>-x64.exe` (per-user, unsigned, x64). Multi-GPU bundle
  (sm86-verified, others best-effort); non-RTX installs run as a plain overlay. Build the shim SDK
  config **last** before running (deploy-the-right-shim). `-DryRun` prints the plan with no
  SDK/RTX/Inno needed. **Stage via `dotnet build -p:SelfContained=true` — NOT `dotnet
  publish`** (cost a debug cycle): for this unpackaged WinUI 3 app, `publish` silently drops
  the app PRI + compiled XAML (`CameraOnScreen.App.pri`, `App.xbf`, `MainWindow.xbf`), so the
  packaged exe dies at launch with `XamlParseException 0x802B000A` at `MainWindow.InitializeComponent`.
  `build -p:SelfContained=true` bundles the .NET runtime *and* keeps the XAML resources.

### FRUC / Optical Flow SDK (separate product; redistribution = bundled-with-app only)

- **FRUC** (frame-rate upscaling, 30→60 fps) uses the **NVIDIA Optical Flow SDK** (`NvOFFRUC.dll`) — a **separate product** from Maxine VFX/AR, governed by the **NVIDIA DesignWorks SDK License** (not the Maxine license above); build via `COS_FRUC_SDK_DIR`; compiled behind `COS_HAS_FRUC`. Runtime: `COS_FRUC_RUNTIME_DIR` else `<app>\maxine\`.
- **Redistribution:** `NvOFFRUC.dll` + `cudart64_110.dll` ship **only bundled inside the app** (never standalone) — permitted under the DesignWorks license's distributable-portions terms (material additional functionality; SDK accessed only by our app). See `THIRD-PARTY-NOTICES.md`. End-user needs an NVIDIA driver **≥ 528.24**; the CUDA 11 runtime is bundled (no CUDA Toolkit install needed). No suitable GPU/driver → effect greys out, app runs normally.

## CI/CD

Public repo `github.com/officialdad/camera-on-screen` (MIT + `THIRD-PARTY-NOTICES.md`). CI = `.github/workflows/ci.yml`, **self-hosted only** (`runs-on: [self-hosted, windows, rtx]` — GitHub-hosted runners lack an RTX GPU + Maxine SDK). On `pull_request` only (the push-to-`main` trigger was dropped — the PR head already ran; merges go through PRs): build shim SDK config → **export-verify** (the deploy gotcha, automated: fails unless the deployed DLL exports `GreenScreen` **and** `GazeRedirection` and lacks `not built in`) → App build → Core tests, all warnings-as-errors. **The runner runs as `NT AUTHORITY\NETWORK SERVICE`** — it does not inherit the interactive user's *User* env vars and cannot read `C:\Users\<you>\…`, so the build SDKs live under the runner tree (`C:\actions-runner\_sdk`) with `C:\actions-runner\.env` setting `COS_VFX_SDK_DIR`/`COS_AR_SDK_DIR`/`COS_FRUC_SDK_DIR` (the last enables FRUC in shim builds + stages `NvOFFRUC.dll`+`cudart64_110.dll` for the bundle). Changing `.env` needs a runner **service restart** to take effect (the running service caches its environment). Runbook: `docs/ci/self-hosted-runner.md`. A second workflow, `.github/workflows/release.yml`, triggers on tag `v*` (same runner): `build-installer.ps1` → `verify-bundle.ps1` (runtime probe of the produced `maxine\`, `COS_*` unset) → upload the installer as a GitHub release.
