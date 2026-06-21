# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Camera-on-Screen: a Windows webcam **desktop-overlay** app. A transparent, always-on-top, draggable overlay shows the live webcam (chromabro-inspired) so any screen recorder captures it live in one pass — no post-edit. Single-process **C# .NET 8 + WinUI 3** control panel + a native **C++ C-ABI shim** (P/Invoke) doing Media Foundation capture. **Windows + NVIDIA RTX only** (planned NVIDIA Maxine effects are RTX-locked). The C# side owns all windowing/compositing; the shim only captures.

Design intent and rationale live in `docs/superpowers/specs/2026-06-20-camera-on-screen-design.md`; the M1+M2 task plan in `docs/superpowers/plans/`. Read the spec before changing cross-component contracts.

## Toolchain (host-specific, non-obvious)

- .NET 8 SDK; **VS2022 Build Tools + MSVC v143** (no full Visual Studio). There is **no WinUI workload, no MSIX tooling, no `dotnet new winui3` template** on this host.
- Because of that, the App is built **unpackaged + self-contained via NuGet only** (`WindowsPackageType=None`, `WindowsAppSDKSelfContained=true`, no `Package.appxmanifest`). It runs with no separately-installed Windows App Runtime.
- The C++ `native/shim/shim.vcxproj` is **deliberately NOT in `CameraOnScreen.sln`** — a C++ project breaks `dotnet build`/`dotnet test` (the SDK MSBuild lacks C++ targets). It is built separately and the App copies the produced DLL via a `<None>` item in its csproj.

## Build & test

The native shim must be built **before** the App (the App csproj copies `native/shim/x64/$(Configuration)/CameraOnScreen.Shim.dll` next to its exe).

```powershell
# 1. Native shim — use Build Tools MSBuild, and run it from PowerShell.
#    (Git Bash mangles MSBuild's /p: switches — do NOT build the shim from the Bash tool.)
#    Set COS_VFX_SDK_DIR so the Maxine effects (COS_HAS_MAXINE) compile in. WITHOUT it the
#    shim builds a CI-safe passthrough STUB (effects unavailable at runtime).
$env:COS_VFX_SDK_DIR = "C:\Users\opari\OneDrive\Desktop\claude-code\VideoFX"  # your VideoFX SDK root
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
# Output: native/shim/x64/Debug/CameraOnScreen.Shim.dll  (x64 only)
# GOTCHA: the SDK build and the CI stub (/p:CosVfxSdkDir=) write the SAME DLL path; whichever
# built LAST is what App -t:Rebuild deploys. Always build the SDK config LAST before running,
# else the app silently runs passthrough (toggles greyed, "Effects require an RTX GPU"). Verify
# the deployed DLL with `grep -a GreenScreen` present and `grep -a "Maxine SDK not built in"` absent.

# 2. App (pulls in Core; copies the shim DLL). Use -t:Rebuild to avoid a transient incremental XAML warning.
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild

# 3. Core unit tests (xUnit, net8.0)
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj

# Single test / class
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~MainViewModelTests"

# Run the app. COS_VFX_SDK_DIR MUST be set in the launching process for the AI Green Screen
# effect to be available (the shim's proxy loads NVVideoEffects.dll + the CUDA/TensorRT chain
# from %COS_VFX_SDK_DIR%\bin; the GreenScreen model dir is %COS_VFX_SDK_DIR%\bin\models). Without
# it, the app runs raw passthrough with effects disabled. For Explorer/double-click launch, set it
# as a persistent USER env var: [Environment]::SetEnvironmentVariable("COS_VFX_SDK_DIR","...","User").
$env:COS_VFX_SDK_DIR = "C:\Users\opari\OneDrive\Desktop\claude-code\VideoFX"
src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.App.exe
```

`dotnet build CameraOnScreen.sln` builds the three SDK-style projects (Core, App, tests) but **not** the vcxproj — build the shim separately first. Verify the C ABI after a shim change with `dumpbin /exports` (under the MSVC `bin/Hostx64/x64/` dir).

Builds and test output should be **pristine (0 warnings)** — warnings are treated as findings here.

## Architecture — the contracts that span files

Three projects: `src/CameraOnScreen.Core` (pure .NET 8 logic, no WinUI/Win32 types, fully unit-tested), `src/CameraOnScreen.App` (WinUI 3 + raw Win32/D3D), `native/shim` (C++ Media Foundation DLL). These rules cut across files — violating one silently breaks the pipeline:

- **The shim never creates a window and never renders.** It captures frames (Media Foundation) into a CPU buffer and exposes a C ABI (`cos_init/enumerate/set_params/start/stop/get_status/get_frame/shutdown`, `extern "C"`). C# does 100% of windowing/compositing.
- **Single shared D3D11 device, no shared handles.** `OverlayWindow` creates the D3D11 device and passes `D3DDevicePtr` to `shim.Init`. Never use `OpenSharedResource`/shared handles. (M2 capture is still CPU-side; the shared device is the contract M3 will move frame production onto.)
- **Status is polled, never pushed.** The frame-pump timer calls `Vm.PollStatusTick()` → `Orchestrator.PollStatus()` → `shim.GetStatus()`. There are no native→managed callbacks. (Note: the native `cos_get_status` returns a hardcoded `30.0` fps stub, not a real count.)
- **C ABI struct parity is load-bearing.** `CosStatus`/`CosParams` (C) and their `[StructLayout(LayoutKind.Sequential)]` mirrors in `PInvokeShim` must match byte-for-byte on x64. `camera_id` is UTF-8 (`StringToCoTaskMemUTF8`). The 128-byte enumeration stride is duplicated in native `cos_enumerate_cameras` and `PInvokeShim.ReadUtf8` — keep them in sync.
- **GPU tier gates effects.** `GpuTierDetector.Detect()` (Vortice DXGI, "RTX" substring heuristic) feeds the `Orchestrator`; on non-RTX the effect toggles are disabled and only passthrough runs. M2 is passthrough-only — Maxine effects (M3–M5) are not implemented yet.
- **Layered overlay.** `OverlayWindow` is a raw Win32 window (`WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP`, `WS_POPUP`) with a DirectComposition flip-model swap chain (`B8G8R8A8_UNorm`, `AlphaMode.Premultiplied`). It is a **separate window from the WinUI control panel**, not a WinUI surface. Single-overlay design: the WndProc routes to one static instance.
- **Persistence:** JSON at `%LOCALAPPDATA%\CameraOnScreen\config.json`, saved on `WM_EXITSIZEMOVE` (drag/resize end) and window close only — never per `WM_MOVE`/`WM_SIZE` (disk thrash). `MainViewModel.ToAppConfig` must retain loaded hotkeys (it builds a fresh `AppConfig`, so anything not copied reverts to defaults).

## Gotchas that cost real debugging

- **Win32 interop structs need their own `CharSet`.** `WNDCLASSEX` must set `CharSet = CharSet.Unicode` in `[StructLayout]`; a `DllImport`'s `CharSet` does NOT govern by-ref struct string fields. Getting this wrong made `lpszClassName` marshal as ANSI vs `RegisterClassExW`/`CreateWindowExW` → class-name mismatch → `CreateWindowEx` fails (Win32 1407) → the app fail-fasts at startup (`0xC0000409`, faulting `Microsoft.UI.Xaml.dll`). App.xaml.cs has a top-level `UnhandledException` handler that logs to `%LOCALAPPDATA%\CameraOnScreen\startup-error.log` — check it on any startup crash.
- **You cannot GDI-screenshot the overlay.** `WS_EX_NOREDIRECTIONBITMAP` + DComp flip-model (promoted to a hardware MPO plane on RTX) means GDI `BitBlt`/`CopyFromScreen` returns **black**. This is by design — it is the same property that lets real recorders capture it cleanly. To verify the overlay renders, instrument the frame pump (log frames received / PresentFrame exceptions) or use a DWM-based capture (Windows.Graphics.Capture / OBS / Game Bar) — not GDI. Visual/recorder confirmation is an inherent human gate; see `docs/superpowers/verification/`.
- **`CopyResource` cannot scale.** It requires identical src/dst dimensions. The overlay keeps its swap chain pinned to the **camera's native resolution** (so `PresentFrame`'s `CopyResource` stays a valid 1:1 copy) and scales to the window via the DirectComposition **visual transform** (`SetTransform` on `WM_SIZE`). Do not `ResizeBuffers` to the window size on user resize.
- **Captured frames must be opaque (alpha = 0xFF).** Media Foundation `RGB32` is BGRX (undefined alpha); the premultiplied-alpha overlay would render alpha=0 as fully transparent (invisible video). The capture code forces alpha to 255. Honor the source row stride (`MFGetStrideForBitmapInfoHeader`, signed) — assuming `width*4` gives upside-down/skewed frames.
- **Capture threading:** a worker thread fills a mutex-guarded frame buffer; `Start`/`Stop` are serialized by a *separate* lifecycle mutex (so `join()` never blocks `LatestFrame`). The managed frame pump (UI thread) and the capture worker are two caller threads — preserve this separation.

## Status

M1 (Core), M2 (App + shim + overlay passthrough), and M3 (AI Green Screen) are complete and merged to `main`. M3 is runtime-verified on an RTX 3090 (green screen works on screen).

**M3: AI Green Screen** — design `docs/superpowers/specs/2026-06-21-camera-on-screen-m3-aigs-design.md`, plan `docs/superpowers/plans/2026-06-21-camera-on-screen-m3-aigs.md`. Decisions: green-screen only; **CPU-copy** interop (GPU work on Maxine, frames round-trip CPU↔GPU — zero-copy D3D11 interop deferred); SDK located via `COS_VFX_SDK_DIR` env var. The RTX-substring tier heuristic is replaced as the effect gate by a new `cos_query_capabilities` shim probe; `GpuTierDetector` keeps only the GPU-name display. Known M3 follow-ups (logged, not done): the probe runs synchronously in the `Orchestrator` ctor (≈1s UI freeze at startup — make it async/lazy); the disabled-effects note shows a static "requires RTX GPU" string instead of the real `CapabilityDetail` from the probe.

- **Maxine VFX SDK is installed** (not in repo) at the path given by `COS_VFX_SDK_DIR` (target machine: `…/claude-code/VideoFX`). GreenScreen feature `nvvfxgreenscreen` 1.2.0.0; models built for compute capability **86** (RTX 3090). No import `.lib` — link via the SDK's proxy stubs (`nvvfx/src/nvVideoEffectsProxy.cpp`, `nvCVImageProxy.cpp`) compiled into the shim. Heavy runtime DLL chain (CUDA + TensorRT) under `VideoFX/bin`.
- **Eye Contact (M4) is NOT in the VFX SDK** — it lives in the separate **Maxine AR SDK** (not installed). M4 adds that as a distinct dependency.
- **Distribution / ship-time gates (deferred).** End users do **not** need an NVIDIA Developer account or the SDK download — the NVIDIA sign-in is a *developer*-side gate. To ship: (1) **bundle** the Maxine runtime (`NVVideoEffects.dll`, `NVCVImage.dll`, CUDA cudart/cublas/npp, TensorRT nvinfer/nvonnxparser, nvngx, nvrtc, the GreenScreen DLL) **+ models** with the installer (~GBs — the "installer weight" risk); (2) switch the shim's SDK discovery from `COS_VFX_SDK_DIR` to **app-relative** (bundled beside the exe), so no env var is needed; (3) **multi-GPU models** — the installed models are prebuilt for compute **86** only; other GPUs (75/89/120) need their variants bundled, or ship the generic model and let TensorRT build the engine on first run; (4) **license review** — Maxine redistribution is permitted but governed by NVIDIA's terms + the bundled-model license (PDFs in `VideoFX/features/nvvfxgreenscreen/license/`) — read/comply before publishing. End-user requirement after bundling: an **RTX GPU + recent driver** (non-RTX → app runs, effects disabled by design).
