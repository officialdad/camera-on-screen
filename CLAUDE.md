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
#    effects. WITHOUT a var, the corresponding effect builds a CI-safe passthrough STUB.
#    Build compiles against the VFX 1.2.0.0 headers/proxy (NvVFX ABI is stable); only the
#    RUNTIME must be the co-versioned 0.7.6 (see CO-VERSION below).
$env:COS_VFX_SDK_DIR = "C:\dev\VideoFX"        # VFX source (build)
$env:COS_AR_SDK_DIR  = "C:\dev\Maxine-AR-SDK"  # AR source clone (build)
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
# Output (x64 only): native/shim/x64/Debug/CameraOnScreen.Shim.dll

# 2. App (pulls in Core, copies the shim DLL). -t:Rebuild avoids a transient XAML warning.
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild

# 3. Core unit tests (xUnit, net8.0). Single test: append --filter "FullyQualifiedName~Name".
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj

# Run with effects (M4+). Runtime resolution, first hit wins:
#   COS_VFX_RUNTIME_DIR (dev override) -> VFX 0.7.6 runtime (flat: DLLs in root, models in \models)
#   COS_AR_RUNTIME_DIR / %ProgramFiles%\NVIDIA Corporation\NVIDIA AR SDK (AR)
#   <app>\maxine\ (bundled, beside the exe — no env vars needed; see Bundler)
# Without any -> raw passthrough, effects disabled (no crash).
$env:COS_VFX_RUNTIME_DIR = "C:\dev\VideoFX-0.7.6"
src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.App.exe
```

`dotnet build CameraOnScreen.sln` builds the three SDK-style projects (Core, App, tests) but **not** the vcxproj. After a shim ABI change, verify with `dumpbin /exports` (under MSVC `bin/Hostx64/x64/`).

**DEPLOY THE RIGHT SHIM (cost a full debugging cycle).** The SDK build (`COS_HAS_MAXINE*`) and the CI stub (`/p:CosVfxSdkDir= /p:CosArSdkDir=`) write the **same** DLL path; whichever built **last** is what App `-t:Rebuild` deploys. Always build the SDK config **last** before running, else the app silently runs passthrough (toggles greyed). Verify the deployed DLL: `grep -a GreenScreen` **and** `grep -a GazeRedirection` present, `grep -a "not built in"` absent.

**CO-VERSION (M4, cost a full cycle).** The Maxine **VFX** (green screen, `NvVFX_*`) and **AR** (eye contact / gaze, `NvAR_*`) SDKs each bundle an exact, pinned CUDA + TensorRT runtime. Two different TRT/CUDA runtimes **cannot coexist** in one process — same DLL names (`nvinfer_10.dll`, `cudart64_12.dll`, `NVCVImage.dll`), first `LoadLibrary` wins, loser's `NvVFX_Load`/`NvAR_Load` fails with `cudaErrorNoKernelImageForDevice`. The verified pair is **VFX 0.7.6 + AR 0.8.7** — both ship **TensorRT 10.4.0.26 / CUDA 12.1** (byte-identical `nvinfer_10.dll`/`cudart64_12.dll`). Do **not** mix VFX 1.2.0.0 (TRT 10.9) with AR 0.8.7 (TRT 10.4). Build compiles against 1.2.0.0 headers (ABI stable); only the **runtime** must be the matched 0.7.6.

## Architecture — contracts that span files

Three projects: `src/CameraOnScreen.Core` (pure .NET 8 logic, no WinUI/Win32 types, fully unit-tested), `src/CameraOnScreen.App` (WinUI 3 + raw Win32/D3D), `native/shim` (C++ Media Foundation DLL). These rules cut across files — violating one silently breaks the pipeline:

- **The shim never creates a window and never renders.** It captures (Media Foundation), runs the optional Maxine effects, and exposes a C ABI (`cos_init/enumerate_cameras/set_params/start/stop/get_status/get_frame/query_capabilities/shutdown`, `extern "C"`, 9 exports). C# does 100% of windowing/compositing.
- **Single shared D3D11 device, no shared handles.** `OverlayWindow` creates the device and passes `D3DDevicePtr` to `cos_init`. Never `OpenSharedResource`. Capture **and** the Maxine path are **CPU-copy** (frames round-trip CPU↔GPU around the effect); the shared device is reserved for the deferred zero-copy path and is currently **unused**.
- **Status is polled, never pushed.** Frame-pump timer → `Vm.PollStatusTick()` → `Orchestrator.PollStatus()` → `shim.GetStatus()`. No native→managed callbacks. (`cos_get_status` returns a hardcoded `30.0` fps stub.)
- **Effect params are pushed live, not only at Start.** The MVVM `On…Changed` partials (`GreenScreenEnabled/Strength/Expand/Feather`, `EyeContact*`, `Mirror`, `Zoom`) call `ApplyLiveParams()` → `Orchestrator.ApplyParams(BuildParams())` → `shim.SetParams` (atomic enable-flag flip, UI-thread-safe). Live push is **gated on `IsRunning`**; `ApplyParams` forces effects off when `EffectsAvailable` is false.
- **C ABI struct parity is load-bearing.** `CosStatus`/`CosParams`/`CosCaps` (C, `shim.h`) and their `[StructLayout(Sequential)]` mirrors in `PInvokeShim` must match byte-for-byte on x64 (`CosCaps` = two `int` gates + `char detail[512]` = 520 bytes). `camera_id`/`detail` are UTF-8. The 128-byte enumeration stride is duplicated in native `cos_enumerate_cameras` and `PInvokeShim.ReadUtf8` — keep in sync.
- **A native probe gates effects, not the GPU tier.** `Orchestrator` sets `EffectsAvailable`/`EyeContactAvailable`/`CapabilityDetail` from `cos_query_capabilities`, which actually tries to create+load each Maxine effect. `GpuTierDetector` is now **display-only** (GPU name string). The probe is **deferred** off the UI thread (`MainViewModel.ProbeCapabilitiesAsync()` from the `MainWindow` ctor); effects gate OFF until it lands. XAML toggle/note bindings are `Mode=OneWay`.
- **Maxine effects run on the capture worker thread** (CUDA/NvAR thread affinity). The `Aigs` (green screen, `aigs.{h,cpp}`) and `EyeContact` (gaze, `eyecontact.{h,cpp}`) objects are worker-thread-local; the UI only flips an atomic enable flag; status crosses threads via atomics + a **leaf-lock** (never nested under `g_state.mtx`/`g_lifecycleMtx`). Eye contact runs before green screen. Per frame (green screen): upload CPU BGRA→GPU, `NvVFX_Run`, download matte, composite (A = matte, RGB premultiplied, honoring matte pitch; matte ops order = dilate → feather → premultiply). Compiled behind `COS_HAS_MAXINE`/`COS_HAS_MAXINE_AR`; without them the shim is a passthrough stub.
- **Layered overlay.** `OverlayWindow` is a raw Win32 window (`WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP`, `WS_POPUP`) with a DirectComposition flip-model swap chain (`B8G8R8A8_UNorm`, `AlphaMode.Premultiplied`) — a **separate window** from the WinUI control panel. Mirror + center-zoom fold into the single DComp visual `Matrix3x2` in `UpdateScale`.
- **Persistence:** JSON at `%LOCALAPPDATA%\CameraOnScreen\config.json`, saved on `WM_EXITSIZEMOVE` and window close only (never per `WM_MOVE`/`WM_SIZE`). `MainViewModel.ToAppConfig` builds a fresh `AppConfig` — anything not copied reverts to defaults (retain loaded hotkeys).

## Gotchas that cost real debugging

- **Win32 interop structs need their own `CharSet`.** `WNDCLASSEX` must set `CharSet = CharSet.Unicode`; a `DllImport`'s `CharSet` does NOT govern by-ref struct string fields. Wrong → `lpszClassName` marshals ANSI → class-name mismatch → `CreateWindowEx` fails (1407) → startup fail-fast (`0xC0000409`). App.xaml.cs logs top-level exceptions to `%LOCALAPPDATA%\CameraOnScreen\startup-error.log` — check it on any startup crash.
- **You cannot GDI-screenshot the overlay.** `WS_EX_NOREDIRECTIONBITMAP` + DComp flip-model (promoted to a hardware MPO plane on RTX) makes GDI `BitBlt`/`CopyFromScreen` return **black** — by design, the same property that lets real recorders capture it cleanly. Verify via Windows.Graphics.Capture / OBS / Game Bar, or instrument the frame pump. **Visual/recorder confirmation is an inherent human gate** (`docs/superpowers/verification/`).
- **`CopyResource` cannot scale.** The swap chain is pinned to the **camera's native resolution** (so `PresentFrame`'s `CopyResource` is a valid 1:1 copy); scale to the window via the DComp **visual transform** (`SetTransform` on `WM_SIZE`). Do not `ResizeBuffers` to the window size.
- **Alpha is opaque in passthrough, the matte with green screen on.** MF `RGB32` is BGRX (undefined alpha); the premultiplied overlay renders alpha=0 as transparent. Passthrough forces alpha=255; green screen overwrites alpha with the matte. Honor the source row stride (`MFGetStrideForBitmapInfoHeader`, signed) — assuming `width*4` gives skewed frames.
- **Capture threading:** a worker thread fills a mutex-guarded frame buffer; `Start`/`Stop` are serialized by a *separate* lifecycle mutex (so `join()` never blocks `LatestFrame`). Never call into the worker-thread-local Maxine objects from the UI thread — toggle via the atomic flag only.
- **`MainViewModel.Dispose()` must dispose the shim** (`cos_shutdown` → joins the capture worker), else the global `std::thread` is destroyed joinable at process exit → `std::terminate`/debug abort dialog.

## Maxine SDKs (not in repo; redistribution governed by NVIDIA EULA)

- **VFX** green screen (`nvvfxgreenscreen`) and **AR** eye contact (gaze) are separate NVIDIA products. No import `.lib` — link via the SDKs' proxy stubs (`nvVideoEffectsProxy.cpp`, `nvCVImageProxy.cpp`, `nvARProxy.cpp`) compiled into the shim. Models are prebuilt TensorRT engines for compute capability **86** (RTX 3090) — arch-locked.
- **App-relative discovery** (`paths.{h,cpp}` `ShimModuleDir()` via `GetModuleHandleExW(FROM_ADDRESS)`, CWD-independent): both resolvers gain an `<app>\maxine\` tier so a shipped app finds the runtimes beside the exe with no env vars. Single shared co-versioned `maxine\` root (one TRT/CUDA runtime, both effect DLLs, `models\vfx` + `models\ar`).
- **Bundler** (`scripts/bundle-maxine.ps1` + `native/shim/bundle/maxine-manifest.psd1`): copies the **minimal verified load-closure** (the manifest's DLL allow-list was produced by `native/shim/smoke/trace_closure.cpp`, which loads both effects and enumerates loaded modules) into `<output>\maxine\` (~1.28 GB, Ampere only). Co-version enforced physically: shared DLLs from the VFX runtime, AR-only DLLs (`cufft64_11`, `nppif64_12`) from the AR runtime. `trace_closure` re-run against the produced bundle (`COS_*` unset → both effects load) is the verify gate. End-user need: an **RTX GPU + recent driver**; no NVIDIA account or SDK download (the redistributable runtime is bundled).
- **Installer** (`scripts/bundle-maxine.ps1` consumer): `scripts/build-installer.ps1`
  builds the App **.NET-self-contained**, export-verifies the deployed shim, runs the
  bundler, then compiles `installer/CameraOnScreen.iss` with Inno Setup 6 →
  `dist/CameraOnScreen-Setup-<ver>-x64.exe` (per-user, unsigned, x64). Effects are
  **Ampere-only** this build; non-RTX installs run as a plain overlay. Build the shim SDK
  config **last** before running (deploy-the-right-shim). `-DryRun` prints the plan with no
  SDK/RTX/Inno needed. **Stage via `dotnet build -p:SelfContained=true` — NOT `dotnet
  publish`** (cost a debug cycle): for this unpackaged WinUI 3 app, `publish` silently drops
  the app PRI + compiled XAML (`CameraOnScreen.App.pri`, `App.xbf`, `MainWindow.xbf`), so the
  packaged exe dies at launch with `XamlParseException 0x802B000A` at `MainWindow.InitializeComponent`.
  `build -p:SelfContained=true` bundles the .NET runtime *and* keeps the XAML resources.

## CI/CD

Public repo `github.com/officialdad/camera-on-screen` (MIT + `THIRD-PARTY-NOTICES.md`). CI = `.github/workflows/ci.yml`, **self-hosted only** (`runs-on: [self-hosted, windows, rtx]` — GitHub-hosted runners lack an RTX GPU + Maxine SDK). On PR + push-to-`main`: build shim SDK config → **export-verify** (the deploy gotcha, automated: fails unless the deployed DLL exports `GreenScreen` **and** `GazeRedirection` and lacks `not built in`) → App build → Core tests, all warnings-as-errors. **The runner runs as `NT AUTHORITY\NETWORK SERVICE`** — it does not inherit the interactive user's *User* env vars and cannot read `C:\Users\<you>\…`, so the build SDKs live under the runner tree (`C:\actions-runner\_sdk`) with `C:\actions-runner\.env` setting `COS_VFX_SDK_DIR`/`COS_AR_SDK_DIR`; changing `.env` needs a runner service restart. Runbook: `docs/ci/self-hosted-runner.md`.

## Status

M1–M5 complete: Core, App + overlay, AI green screen, QoL (overlay mirror/zoom, green-screen expand/feather, panel right-size), AI eye contact (gaze redirection), app-relative SDK discovery, the runtime **bundler**, and the **installer** (`scripts/build-installer.ps1` → Inno Setup `.exe`). All user-verified on an RTX 3090 — the installer was installed + launched from the Start Menu with both effects working (0.59 GB installer, issue #1). Remaining ship-time work tracked as GitHub issues: **multi-GPU models** (compute 86 only today; Turing models fetchable from NGC via `VideoFX\features\install_feature.ps1 -gpu 75` but co-version-trapped vs the shipped 0.7.6 runtime), **full license review** (#3 — gates any *public* redistribution), **tag-triggered release.yml** (#4 — needs #1 merged + #3 cleared).
