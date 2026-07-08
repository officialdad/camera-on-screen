<p align="center">
  <img src="cos.png" alt="Camera-on-Screen" width="200">
</p>

<h1 align="center">Camera-on-Screen</h1>

<p align="center">
  I made this because I'm too lazy to do any post editing.
</p>

<p align="center">
  <a href="https://github.com/officialdad/camera-on-screen/actions/workflows/ci.yml"><img src="https://github.com/officialdad/camera-on-screen/actions/workflows/ci.yml/badge.svg" alt="ci"></a>
</p>

---

An always-on-top webcam overlay for Windows. It floats your live
camera feed over everything else, so any screen recorder or screen sharing session
captures you and your screen together in real-time. Useful for teaching too.

Included NVIDIA powered features such as
- **AI Green Screen** (background removal)
- **AI Eye Contact** (gaze redirection).
- **AI Interpolation** (doubles webcam FPS)

> **You need:** Windows + an **NVIDIA RTX GPU** for the AI effects.
> On other hardware the overlay still works without the AI effects.

## Install

1. Download the latest `CameraOnScreen-Setup-<ver>-x64.exe` from
   [**Releases**](https://github.com/officialdad/camera-on-screen/releases).
2. Run it. It installs **per-user** (no admin), adds a Start Menu shortcut, and
   bundles everything it needs - no extra downloads.
3. Windows SmartScreen will warn (the installer is unsigned): click
   **More info → Run anyway**.

Uninstall from **Settings → Apps** as usual. Your preferences are kept at
`%LOCALAPPDATA%\CameraOnScreen\config.json`.

> The AI effects ship with models for **RTX 20/30/40-series and Blackwell**
> GPUs, but are only verified on **RTX 30-series & 20-series** so far - other RTX
> architectures load best-effort. On non-RTX hardware the app still installs and
> runs as a plain webcam overlay.

## Using it

- **Move it** - drag the centre **+** handle.
- **Resize it** - scroll the mouse wheel over the overlay.
- **Mirror / zoom** - toggle in the control panel.
- **AI Green Screen** - removes your background with adjustable edge expand /
  feather.
- **AI Eye Contact** - gently redirects your gaze toward the camera.

Then record with NVIDIA ShadowPlay, Game Bar or stream live with the overlay
always visible on real time in the capture.

## Contributing

Issues and pull requests are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for
how to build, the RTX/Maxine requirements, and the bar PRs need to clear.

## License

[MIT](LICENSE). The bundled NVIDIA Maxine runtime is governed separately under
the NVIDIA Maxine SDK License - see
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md). NVIDIA, Maxine, and RTX are
trademarks of NVIDIA Corporation; this project is not affiliated with NVIDIA.

---

# Technical details

## What it is

- Single-process **C# .NET 8 + WinUI 3** control panel.
- A native **C++ C-ABI shim** (P/Invoke) doing Media Foundation capture and the
  optional Maxine effects.
- The C# side owns all windowing/compositing (a layered DirectComposition
  overlay); the shim only captures and applies effects.

## NVIDIA Maxine SDKs

The AI effects use the **NVIDIA Maxine Video Effects SDK** (green screen) and
**NVIDIA Maxine AR SDK** (eye contact). These are **not bundled** in the
repository - download them from <https://developer.nvidia.com/maxine> and point
the build at them. See [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

The two SDKs each pin an exact CUDA + TensorRT runtime and **cannot mix** in one
process. Use a co-versioned pair - verified: **VFX 1.2.0.0 + AR 1.1.1.0** (shared
TensorRT 10.9 / CUDA 12.x).

## NVIDIA Optical Flow SDK

AI Interpolation uses the **NVIDIA Optical Flow SDK** (`NvOFFRUC.dll`) to synthesize
a temporal mid-frame between each real camera frame, doubling the overlay frame rate
from 30 to 60 fps. This is a **separate product** from Maxine, governed by the
**NVIDIA DesignWorks SDK License** - not bundled in the repository, point the build
at it via `COS_FRUC_SDK_DIR`. See [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

FRUC uses CUDA 11 (`cudart64_110.dll`) - a distinct runtime from Maxine's CUDA 12 -
so both stacks coexist in one process without conflict. Verified on RTX 3090.
Requires driver ≥ 528.24; the CUDA 11 runtime is bundled (no separate install).

## Build

Prerequisites: .NET 8 SDK, VS2022 Build Tools + MSVC v143. The native shim must
be built **before** the App.

```powershell
# 1. Native shim (PowerShell - Bash mangles MSBuild /p: switches).
$env:COS_VFX_SDK_DIR  = "<path-to-VideoFX-SDK>"
$env:COS_AR_SDK_DIR   = "<path-to-Maxine-AR-SDK-clone>"
$env:COS_FRUC_SDK_DIR = "<path-to-Optical-Flow-SDK>"   # optional; omit for passthrough stub
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  native/shim/shim.vcxproj /p:Configuration=Release /p:Platform=x64

# 2. App (copies the shim next to the exe).
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild

# 3. Core unit tests.
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj
```

Without the SDK env vars the shim builds a CI-safe **passthrough stub** (effects
disabled) so the project still builds on machines without the SDK. For the
runtime env vars needed to actually run the effects, see
[`CLAUDE.md`](CLAUDE.md).

## CI / Release

Every PR is gated by GitHub Actions on a **self-hosted RTX runner** - full
co-versioned Maxine build, a stale-stub export verify, the App build, and Core
unit tests, all with warnings treated as errors. A second workflow builds the
installer and publishes a GitHub release on a `v*` tag. See
[`docs/ci/self-hosted-runner.md`](docs/ci/self-hosted-runner.md).
