# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Camera-on-Screen: a Windows webcam **desktop-overlay** app. A transparent, always-on-top, draggable overlay shows the live webcam (chromabro-inspired) so any screen recorder captures it live in one pass — no post-edit. Single-process **C# .NET 8 + WinUI 3** control panel + a native **C++ C-ABI shim** (P/Invoke) doing Media Foundation capture. **Windows + NVIDIA RTX only** (planned NVIDIA Maxine effects are RTX-locked). The C# side owns all windowing/compositing; the shim only captures.

Design intent and rationale live in `docs/superpowers/specs/2026-06-20-camera-on-screen-design.md` (overall) and `…/2026-06-21-camera-on-screen-m3-aigs-design.md` (M3 green screen); task plans (M1+M2, M3) in `docs/superpowers/plans/`. Read the relevant spec before changing cross-component contracts.

## Toolchain (host-specific, non-obvious)

- .NET 8 SDK; **VS2022 Build Tools + MSVC v143** (no full Visual Studio). There is **no WinUI workload, no MSIX tooling, no `dotnet new winui3` template** on this host.
- Because of that, the App is built **unpackaged + self-contained via NuGet only** (`WindowsPackageType=None`, `WindowsAppSDKSelfContained=true`, no `Package.appxmanifest`). It runs with no separately-installed Windows App Runtime.
- The C++ `native/shim/shim.vcxproj` is **deliberately NOT in `CameraOnScreen.sln`** — a C++ project breaks `dotnet build`/`dotnet test` (the SDK MSBuild lacks C++ targets). It is built separately and the App copies the produced DLL via a `<None>` item in its csproj.

## Build & test

The native shim must be built **before** the App (the App csproj copies `native/shim/x64/$(Configuration)/CameraOnScreen.Shim.dll` next to its exe).

```powershell
# 1. Native shim — use Build Tools MSBuild, and run it from PowerShell.
#    (Git Bash mangles MSBuild's /p: switches — do NOT build the shim from the Bash tool.)
#    Set COS_VFX_SDK_DIR (green screen, COS_HAS_MAXINE) + COS_AR_SDK_DIR (eye contact,
#    COS_HAS_MAXINE_AR) so both Maxine effects compile in. WITHOUT a var the corresponding
#    effect builds a CI-safe passthrough STUB. The two guards are orthogonal.
#      COS_VFX_SDK_DIR -> VFX SDK source (headers + nvvfx/src proxy). 1.2.0.0 tree is fine for
#                        BUILD (NvVFX green-screen ABI is stable); see CO-VERSION gotcha for runtime.
#      COS_AR_SDK_DIR  -> a clone of github.com/NVIDIA-Maxine/Maxine-AR-SDK (nvar/include +
#                        nvar/src/nvARProxy.cpp). Build-time only; AR runtime is the installer.
$env:COS_VFX_SDK_DIR = "C:\dev\VideoFX"        # VFX source (build)
$env:COS_AR_SDK_DIR  = "C:\dev\Maxine-AR-SDK"  # AR source clone (build)
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
# Output: native/shim/x64/Debug/CameraOnScreen.Shim.dll  (x64 only)
# GOTCHA: the SDK build and the CI stub (/p:CosVfxSdkDir= /p:CosArSdkDir=) write the SAME DLL path;
# whichever built LAST is what App -t:Rebuild deploys. Always build the SDK config LAST before
# running, else the app silently runs passthrough (toggles greyed). Verify the deployed DLL with
# `grep -a GreenScreen` AND `grep -a GazeRedirection` present, and `grep -a "not built in"` absent.

# 2. App (pulls in Core; copies the shim DLL). Use -t:Rebuild to avoid a transient incremental XAML warning.
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild

# 3. Core unit tests (xUnit, net8.0)
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj

# Single test / class
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~MainViewModelTests"

# Run the app (M4: BOTH effects). Two env vars at runtime:
#   COS_VFX_RUNTIME_DIR -> VFX **0.7.6** runtime (green screen). FLAT layout: DLLs in <root>,
#       models in <root>\models. MUST be the AR-MATCHED VFX SDK 0.7.6 (TensorRT 10.4.0.26 /
#       CUDA 12.1) — see the CO-VERSION gotcha below. If unset, the shim falls back to
#       COS_VFX_SDK_DIR\bin (legacy 1.2.0.0 layout) which is TRT 10.9 and CONFLICTS with AR.
#   AR (eye contact) runtime auto-resolves from %ProgramFiles%\NVIDIA Corporation\NVIDIA AR SDK
#       (proxy default; override with COS_AR_RUNTIME_DIR). Must be AR 0.8.7 (TRT 10.4).
# Without these the app runs raw passthrough with effects disabled. For Explorer/double-click,
# set them as persistent USER env vars via [Environment]::SetEnvironmentVariable(...,"User").
$env:COS_VFX_RUNTIME_DIR = "C:\dev\VideoFX-0.7.6"
src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.App.exe
```

**CO-VERSION GOTCHA (M4, cost a full debugging cycle).** The Maxine **VFX** SDK (green screen,
`NvVFX_*`) and **AR** SDK (eye contact / GazeRedirection, `NvAR_*`) are separate products that
each bundle an **exact, pinned** CUDA + TensorRT runtime. Two different TRT/CUDA runtimes
**cannot coexist** in one process — same DLL names (`nvinfer_10.dll`, `cudart64_12.dll`,
`NVCVImage.dll`), first `LoadLibrary` wins, and the loser's `NvVFX_Load`/`NvAR_Load` fails with
`cudaErrorNoKernelImageForDevice` ("no kernel image available for execution on the device"). The
fix is **co-versioning**: use a VFX + AR SDK pair that bundle the **same** TRT. The verified pair
is **VFX 0.7.6 + AR 0.8.7** — both ship **TensorRT 10.4.0.26 / CUDA 12.1** (byte-identical
`nvinfer_10.dll`/`cudart64_12.dll`). Both installers are direct, no-auth downloads from
`nvidia.com/.../broadcast-sdk/resources` (pick the GPU-arch variant — `_ampere` for the RTX 3090).
Do NOT mix the newer VFX 1.2.0.0 (TRT 10.9) with AR 0.8.7 (TRT 10.4). NVIDIA Broadcast does both
effects only because it uses one private co-versioned runtime; there is no public unified SDK.
**Build** still compiles against the VFX **1.2.0.0** headers/proxy at `COS_VFX_SDK_DIR` (the
green-screen NvVFX ABI is stable across 0.7.6/1.2.0.0); only the **runtime** DLLs/models come from
the matched 0.7.6 via `COS_VFX_RUNTIME_DIR`.

`dotnet build CameraOnScreen.sln` builds the three SDK-style projects (Core, App, tests) but **not** the vcxproj — build the shim separately first. Verify the C ABI after a shim change with `dumpbin /exports` (under the MSVC `bin/Hostx64/x64/` dir).

Builds and test output should be **pristine (0 warnings)** — warnings are treated as findings here.

## Architecture — the contracts that span files

Three projects: `src/CameraOnScreen.Core` (pure .NET 8 logic, no WinUI/Win32 types, fully unit-tested), `src/CameraOnScreen.App` (WinUI 3 + raw Win32/D3D), `native/shim` (C++ Media Foundation DLL). These rules cut across files — violating one silently breaks the pipeline:

- **The shim never creates a window and never renders.** It captures frames (Media Foundation), runs the optional Maxine effect, and exposes a C ABI (`cos_init/enumerate_cameras/set_params/start/stop/get_status/get_frame/query_capabilities/shutdown`, `extern "C"`, 9 exports). C# does 100% of windowing/compositing.
- **Single shared D3D11 device, no shared handles.** `OverlayWindow` creates the D3D11 device and passes `D3DDevicePtr` to `shim.Init`. Never use `OpenSharedResource`/shared handles. (Capture AND the M3 Maxine path are **CPU-copy**: frames round-trip CPU↔GPU around the effect. The shared device is reserved for the deferred zero-copy path and is currently **unused** — `d3d11_device` is passed to `cos_init` but not consumed.)
- **Status is polled, never pushed.** The frame-pump timer calls `Vm.PollStatusTick()` → `Orchestrator.PollStatus()` → `shim.GetStatus()`. There are no native→managed callbacks. (Note: the native `cos_get_status` returns a hardcoded `30.0` fps stub, not a real count.)
- **Effect params are pushed live, not only at Start.** Flipping a toggle / moving a slider while running must reach the shim immediately. The MVVM `On…Changed` partials for the effect props (`GreenScreenEnabled`, `GreenScreenStrength`, `EyeContact*`) call `MainViewModel.ApplyLiveParams()` → `Orchestrator.ApplyParams(BuildParams())` → `shim.SetParams` (which the native side treats as an atomic enable-flag flip — safe from the UI thread). `Start` calls the same `ApplyParams` for the initial push. The live push is **gated on `IsRunning`** so `LoadFrom`/pre-start changes don't drive a not-yet-started shim, and `ApplyParams` applies the same effect gate as `Start` (effects forced off when `EffectsAvailable` is false). Regression once: `SetParams` only fired in `Start`, so toggling green screen did nothing until the next Stop→Start.
- **C ABI struct parity is load-bearing.** `CosStatus`/`CosParams`/`CosCaps` (C, in `shim.h`) and their `[StructLayout(LayoutKind.Sequential)]` mirrors in `PInvokeShim` must match byte-for-byte on x64 (`CosCaps` = `int green_screen_available` + `char detail[256]` = 260 bytes). `camera_id`/`detail` are UTF-8. The 128-byte enumeration stride is duplicated in native `cos_enumerate_cameras` and `PInvokeShim.ReadUtf8` — keep them in sync.
- **A native probe gates effects (not the GPU tier).** The `Orchestrator` sets `EffectsAvailable`/`CapabilityDetail` from `INativeShim.QueryCapabilities()` → `cos_query_capabilities`, which actually tries to create+load the Maxine GreenScreen effect. `GpuTierDetector.Detect()` (Vortice DXGI, "RTX" substring) is now **display-only** (GPU name string), NOT the gate. No SDK / effect-load failure → toggles disabled, passthrough (no crash). The probe is **deferred, not run in the ctor** — `Orchestrator.ProbeCapabilities()` (the ~1s TensorRT load) is kicked off off the UI thread via `MainViewModel.ProbeCapabilitiesAsync()` from the `MainWindow` ctor; until it lands, effects are gated OFF and the note shows "Checking effect availability…". The disabled-state note's text is bound to `Vm.CapabilityDetail` (the real probe reason), not a static string; its visibility (`MainWindow.NotAvailableVisibility`) re-evaluates when `EffectsAvailable` raises `PropertyChanged`. The XAML toggle/note bindings are `Mode=OneWay` so the async result propagates.
- **M3 AI Green Screen pipeline (CPU-copy).** `native/shim/aigs.{h,cpp}` wraps the Maxine GreenScreen effect and runs in the **capture worker thread** — the CUDA stream + effect have thread affinity, so the `Aigs` object is worker-thread-local, `cos_set_params` (UI thread) only flips an atomic enable flag, and status crosses threads via atomics + a **leaf-lock** `gsErrMtx` (never nested under `g_state.mtx`/`g_lifecycleMtx`). Per frame: upload CPU BGRA→GPU, `NvVFX_Run`, download matte→CPU, composite (**A = matte, RGB *= matte/255 premultiplied**, honoring the matte's `pitch`). Upload/Download/Composite are a deliberate **swappable seam** for the future zero-copy path. Compiled behind `COS_HAS_MAXINE` (defined when `COS_VFX_SDK_DIR` is set at build); without it the shim is a passthrough **stub**, so CI / no-SDK machines still build and Core tests still pass.
- **Layered overlay.** `OverlayWindow` is a raw Win32 window (`WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP`, `WS_POPUP`) with a DirectComposition flip-model swap chain (`B8G8R8A8_UNorm`, `AlphaMode.Premultiplied`). It is a **separate window from the WinUI control panel**, not a WinUI surface. Single-overlay design: the WndProc routes to one static instance.
- **Persistence:** JSON at `%LOCALAPPDATA%\CameraOnScreen\config.json`, saved on `WM_EXITSIZEMOVE` (drag/resize end) and window close only — never per `WM_MOVE`/`WM_SIZE` (disk thrash). `MainViewModel.ToAppConfig` must retain loaded hotkeys (it builds a fresh `AppConfig`, so anything not copied reverts to defaults).

## Gotchas that cost real debugging

- **Win32 interop structs need their own `CharSet`.** `WNDCLASSEX` must set `CharSet = CharSet.Unicode` in `[StructLayout]`; a `DllImport`'s `CharSet` does NOT govern by-ref struct string fields. Getting this wrong made `lpszClassName` marshal as ANSI vs `RegisterClassExW`/`CreateWindowExW` → class-name mismatch → `CreateWindowEx` fails (Win32 1407) → the app fail-fasts at startup (`0xC0000409`, faulting `Microsoft.UI.Xaml.dll`). App.xaml.cs has a top-level `UnhandledException` handler that logs to `%LOCALAPPDATA%\CameraOnScreen\startup-error.log` — check it on any startup crash.
- **You cannot GDI-screenshot the overlay.** `WS_EX_NOREDIRECTIONBITMAP` + DComp flip-model (promoted to a hardware MPO plane on RTX) means GDI `BitBlt`/`CopyFromScreen` returns **black**. This is by design — it is the same property that lets real recorders capture it cleanly. To verify the overlay renders, instrument the frame pump (log frames received / PresentFrame exceptions) or use a DWM-based capture (Windows.Graphics.Capture / OBS / Game Bar) — not GDI. Visual/recorder confirmation is an inherent human gate; see `docs/superpowers/verification/`.
- **`CopyResource` cannot scale.** It requires identical src/dst dimensions. The overlay keeps its swap chain pinned to the **camera's native resolution** (so `PresentFrame`'s `CopyResource` stays a valid 1:1 copy) and scales to the window via the DirectComposition **visual transform** (`SetTransform` on `WM_SIZE`). Do not `ResizeBuffers` to the window size on user resize.
- **Alpha is opaque in passthrough, the matte with green screen on.** Media Foundation `RGB32` is BGRX (undefined alpha); the premultiplied-alpha overlay renders alpha=0 as fully transparent (invisible video). So passthrough forces alpha=255; with green screen ON the AIGS composite **overwrites** alpha with the matte and premultiplies RGB. Honor the source row stride (`MFGetStrideForBitmapInfoHeader`, signed) — assuming `width*4` gives upside-down/skewed frames.
- **Capture threading:** a worker thread fills a mutex-guarded frame buffer; `Start`/`Stop` are serialized by a *separate* lifecycle mutex (so `join()` never blocks `LatestFrame`). The managed frame pump (UI thread) and the capture worker are two caller threads — preserve this separation. **The Maxine `Aigs` object lives entirely on the worker thread** (CUDA affinity) — never call into it from the UI thread; toggle it via the atomic flag only.
- **Deploy the right shim build.** The SDK build (`COS_HAS_MAXINE`) and the CI stub write the *same* DLL path; build the SDK config **last**, then `-t:Rebuild` the App, or it silently deploys the passthrough stub (greyed toggles). See the GOTCHA in Build & test for the `grep` verification.

## Status

M1 (Core), M2 (App + shim + overlay passthrough), M3 (AI Green Screen), the 3 QoL buckets, **M4 (AI Eye Contact / gaze redirection)**, and **M5 part 1 (app-relative SDK discovery)** are all complete and merged to `main`. **M4 is user-verified on an RTX 3090 (`5be7619`): green screen + eye contact work together and alone, live toggles work.** M4 required a **co-version fix** — two Maxine SDKs with mismatched CUDA/TensorRT can't share one process; pinned VFX **0.7.6** + AR **0.8.7** onto shared **TensorRT 10.4** via `COS_VFX_RUNTIME_DIR` (see the CO-VERSION GOTCHA in Build & test, and follow-up #6).

**M5 part 1 — app-relative SDK discovery (merged `055e932`, user-verified RTX 3090).** Both Maxine resolvers gained a third tier, `<app>\maxine\` (resolved from the shim DLL's own dir via new `native/shim/paths.{h,cpp}` `ShimModuleDir()` — `GetModuleHandleExW(FROM_ADDRESS,&ShimModuleDir)`, CWD-independent). Env vars (`COS_VFX_RUNTIME_DIR`/`COS_VFX_SDK_DIR`, `COS_AR_RUNTIME_DIR`) are now an **optional dev override**; a shipped app finds the runtimes beside the exe with no env. **Single shared `maxine\` root** = the co-version invariant on disk (one TRT/CUDA runtime, both effect DLLs, `models\vfx` + `models\ar`; AR's bundled tier sits **above** the `%ProgramFiles%` install so a shipped app never loads a non-co-versioned system AR). No C-ABI change; `paths.{h,cpp}` compile unconditionally so the CI stub still builds. Verified headlessly by `native/shim/smoke/bundle_probe.cpp` (COS_* unset, cwd elsewhere → both `Probe` AVAILABLE, engines load from the bundle). Also fixed a **pre-existing exit-abort** (`578ef09`): `MainViewModel.Dispose()` now disposes the shim (`cos_shutdown` → joins the capture worker) so the global `std::thread` isn't destroyed joinable at process exit (else `std::terminate`/debug abort dialog). **Next: remaining M5 — bundler, installer, multi-GPU models, license (follow-up #7).**

**QoL polish (between M3 and M4/M5).** A small user-driven QoL effort in three independent buckets — design `docs/superpowers/specs/2026-06-21-camera-on-screen-qol-polish-design.md`, build order **2 → 3 → 1**, each its own plan + review checkpoint + visual gate. **Bucket 3 (green-screen Expand + Feather) is DONE and merged to `main`, user-verified on the RTX 3090** — see Open follow-up #4 for detail (matte dilate/feather in `aigs.cpp`, one new `double` across the C ABI, two live-pushed sliders). **Bucket 2 (overlay mirror + center zoom) is DONE and merged to `main` (`d086095`), user-verified on the RTX 3090.** It is presentation-side only — a horizontal-mirror toggle and a center-only zoom slider (1.0×–3.0×) folded into the single DirectComposition visual `Matrix3x2` in `OverlayWindow.UpdateScale` (`Fit*Mirror*Zoom` about the window centre; the window clips zoom overflow = tighter framing). `OverlaySettings.Zoom` added (`Mirror` already existed), VM props round-trip via `LoadFrom`/`ToAppConfig`, host routes them to `SetMirror`/`SetZoom` like `Locked`/`ClickThrough`. **No shim/C-ABI/`ShimParams` change; the swap chain stays pinned to frame res (1:1 `CopyResource` unchanged).** **Bucket 1 (control panel right-size) is DONE and merged, user-verified** — `MainWindow.RightSizePanel()` `AppWindow.Resize` to 400×720 DIP (DPI-scaled). **All three QoL buckets complete; M4 (Eye Contact) is next.**

**M3: AI Green Screen** — design `docs/superpowers/specs/2026-06-21-camera-on-screen-m3-aigs-design.md`, plan `docs/superpowers/plans/2026-06-21-camera-on-screen-m3-aigs.md`. Decisions: green-screen only; **CPU-copy** interop (GPU work on Maxine, frames round-trip CPU↔GPU — zero-copy D3D11 interop deferred); SDK located via `COS_VFX_SDK_DIR` env var. The RTX-substring tier heuristic is replaced as the effect gate by a new `cos_query_capabilities` shim probe; `GpuTierDetector` keeps only the GPU-name display. The two M3 polish follow-ups are **done**: the probe is now deferred and run off the UI thread (no startup freeze), and the disabled-effects note is bound to the real `CapabilityDetail` from the probe (not a static "requires RTX GPU" string).

- **Maxine VFX SDK is installed** (not in repo) at the path given by `COS_VFX_SDK_DIR` (target machine: `…/claude-code/VideoFX`). GreenScreen feature `nvvfxgreenscreen` 1.2.0.0; models built for compute capability **86** (RTX 3090). No import `.lib` — link via the SDK's proxy stubs (`nvvfx/src/nvVideoEffectsProxy.cpp`, `nvCVImageProxy.cpp`) compiled into the shim. Heavy runtime DLL chain (CUDA + TensorRT) under `VideoFX/bin`.
- **Eye Contact (M4, DONE) is in the separate Maxine AR SDK** (`NvAR_*`), installed at `%ProgramFiles%\NVIDIA Corporation\NVIDIA AR SDK` (runtime) + build source clone at `COS_AR_SDK_DIR`. It bundles its **own** CUDA/TensorRT — see the CO-VERSION GOTCHA: VFX **0.7.6** + AR **0.8.7** must share TensorRT **10.4** or `NvVFX_Load` fails ("no kernel image").
- **Distribution / ship-time gates (deferred).** End users do **not** need an NVIDIA Developer account or the SDK download — the NVIDIA sign-in is a *developer*-side gate. To ship: (1) **bundle** the Maxine runtime (`NVVideoEffects.dll`, `NVCVImage.dll`, CUDA cudart/cublas/npp, TensorRT nvinfer/nvonnxparser, nvngx, nvrtc, the GreenScreen DLL) **+ models** with the installer (~GBs — the "installer weight" risk); (2) switch the shim's SDK discovery from `COS_VFX_SDK_DIR` to **app-relative** (bundled beside the exe), so no env var is needed; (3) **multi-GPU models** — the installed models are prebuilt for compute **86** only; other GPUs (75/89/120) need their variants bundled, or ship the generic model and let TensorRT build the engine on first run; (4) **license review** — Maxine redistribution is permitted but governed by NVIDIA's terms + the bundled-model license (PDFs in `VideoFX/features/nvvfxgreenscreen/license/`) — read/comply before publishing. End-user requirement after bundling: an **RTX GPU + recent driver** (non-RTX → app runs, effects disabled by design).

## Open follow-ups (next session)

Ordered roughly by size.

1. ~~**Surface the real probe reason.**~~ **DONE.** The note's text is bound to `Vm.CapabilityDetail` (real `cos_query_capabilities` reason), not the static "requires RTX GPU" string.
2. ~~**Make the capability probe async/lazy.**~~ **DONE.** The probe is deferred out of the `Orchestrator` ctor into `ProbeCapabilities()` and run off the UI thread via `MainViewModel.ProbeCapabilitiesAsync()` (kicked off from the `MainWindow` ctor); effects gate OFF until it lands, OneWay bindings propagate the result. No startup freeze.
3. ~~**M3 user gate.**~~ **COMPLETE (2026-06-21).** Green screen confirmed on screen on the RTX 3090, live toggle works, and **NVIDIA ShadowPlay** captures the overlay correctly (DWM/hardware path, as designed — GDI shows black). Results filled in `docs/superpowers/verification/2026-06-20-recorder-capture.md` (M3 section).
4. ~~**QoL Bucket 3 — green-screen Expand + Feather.**~~ **DONE (2026-06-21), merged to `main`, user-verified on the RTX 3090.** Repurposed the unused `green_screen_strength` → `green_screen_expand` (matte **dilate**) and added `green_screen_feather` (matte **blur**), both `double` across all 6 ABI parity sites byte-parity on x64. Matte ops in `aigs.cpp` (`PackMatte`→`DilateMatte`→`FeatherMatte`→`Composite`, order **dilate → feather → premultiply**, separable passes, max radius 16px each, honor matte pitch), worker-thread-local, pushed live via the atomic param path (`Capture::SetMatteParams`). Two panel sliders gated on the green-screen toggle; defaults 0.0/0.0 = unchanged matte out of the box. Plan: `docs/superpowers/plans/2026-06-21-camera-on-screen-qol-bucket3-expand-feather.md`.
5. ~~**QoL Bucket 1 — control panel right-size.**~~ **DONE (2026-06-21), user-verified.** `MainWindow.RightSizePanel()` (called after `InitializeComponent`) does `AppWindow.Resize` to a compact **400×720 DIP**, scaled by `GetDpiForWindow` (Resize takes physical px). No layout redesign. Visual gate only — no meaningful tests (window chrome). Tweak via `PanelWidthDip`/`PanelHeightDip` consts atop `MainWindow.xaml.cs`. **All three QoL buckets now done.**
6. ~~**M4 — Eye Contact.**~~ **DONE (2026-06-21), merged to `main` (`5be7619`), user-verified RTX 3090.** NvAR GazeRedirection in `native/shim/eyecontact.{h,cpp}` (worker-thread-local, runs before green screen), own `EyeContactAvailable` gate + toggle, `CosCaps` grew a 2nd gate (520B parity), build guard `COS_HAS_MAXINE_AR`. **The hard part = the CO-VERSION fix** (see Build & test gotcha + Status): VFX 0.7.6 + AR 0.8.7 on shared TRT 10.4; runtime via `COS_VFX_RUNTIME_DIR`→`…/VideoFX-0.7.6`. Spec/plan: `docs/superpowers/{specs,plans}/2026-06-21-camera-on-screen-m4-eyecontact*.md`. **Toggle-only UI** — the pre-plumbed `eye_contact_sensitivity`/`look_away_range` params exist across the ABI but are unused (SDK defaults); a future slider pass needs no ABI change.
7. **M5 — ship-time.** Spec/plan: `docs/superpowers/{specs,plans}/2026-06-21-camera-on-screen-m5-app-relative-discovery-*`.
   - **Part 1 — app-relative SDK discovery — DONE (2026-06-21), merged `055e932`, user-verified RTX 3090.** Both resolvers gained a `<app>\maxine\` tier (via `paths.{h,cpp}` `ShimModuleDir`); env vars now an optional dev override; single shared co-versioned `maxine\` root (`models\vfx` + `models\ar`, AR tier above ProgramFiles); no C-ABI change; CI-stub still builds. Headless `bundle_probe.cpp` confirmed both effects resolve+load with `COS_*` unset. Also fixed pre-existing exit-abort (`578ef09`, `MainViewModel.Dispose` disposes shim → worker joins). See Status.
   - **Remaining (NEXT, each own spec):** (a) **bundler** — make the App publish step copy the **co-versioned** runtime+models for BOTH SDKs (VFX 0.7.6 + AR 0.8.7, shared TRT 10.4) into `<output>\maxine\` with the part-1 layout (today they're hand-copied; ~2.1 GB); (b) **installer** (WiX/MSIX/Inno/self-extract — TBD; the ~GB Maxine runtime is the installer-weight risk); (c) **multi-GPU models** (compute 86 only today — other arches need their variants, or ship generic + first-run TensorRT build); (d) **license compliance** (Maxine EULA PDFs in each SDK). The co-version constraint is a hard ship requirement.
