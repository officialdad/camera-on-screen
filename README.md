# Camera-on-Screen

[![ci](https://github.com/officialdad/camera-on-screen/actions/workflows/ci.yml/badge.svg)](https://github.com/officialdad/camera-on-screen/actions/workflows/ci.yml)

A Windows webcam **desktop-overlay** app. A transparent, always-on-top,
draggable overlay shows your live webcam so any screen recorder captures it
live in one pass — no post-edit. Optional NVIDIA Maxine **AI Green Screen** and
**AI Eye Contact** effects composite in real time.

> **Requirements:** Windows + an **NVIDIA RTX GPU** with a recent driver. On
> non-RTX hardware the app still runs, but the AI effects are disabled by
> design.

## Install

Download `CameraOnScreen-Setup-<ver>-x64.exe` (built by `scripts\build-installer.ps1`;
GitHub Releases once the release workflow lands) and run it. It installs **per-user**
(no admin) to `%LOCALAPPDATA%\Programs\CameraOnScreen`, adds a Start Menu shortcut, and
registers an uninstaller. The .NET runtime is bundled — no prerequisite install.

The installer is **unsigned**, so Windows SmartScreen may warn on first run: click
**More info → Run anyway**. Uninstalling removes the app but keeps your settings
(`%LOCALAPPDATA%\CameraOnScreen\config.json`).

> **AI effects in this build are for RTX 30-series (Ampere) GPUs.** On other GPUs the
> app installs and runs as a plain webcam overlay with the effects disabled.

## What it is

- Single-process **C# .NET 8 + WinUI 3** control panel.
- A native **C++ C-ABI shim** (P/Invoke) doing Media Foundation capture and the
  optional Maxine effects.
- The C# side owns all windowing/compositing (a layered DirectComposition
  overlay); the shim only captures and applies effects.

## NVIDIA Maxine SDKs (not included)

The AI effects use the **NVIDIA Maxine Video Effects SDK** (green screen) and
**NVIDIA Maxine AR SDK** (eye contact). These are **not bundled** in this
repository — download them from <https://developer.nvidia.com/maxine> and point
the build at them. See [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

The two SDKs each pin an exact CUDA + TensorRT runtime and **cannot mix** in one
process. Use a co-versioned pair — verified: **VFX 0.7.6 + AR 0.8.7** (shared
TensorRT 10.4 / CUDA 12.1).

## Build

Prerequisites: .NET 8 SDK, VS2022 Build Tools + MSVC v143. The native shim must
be built **before** the App.

```powershell
# 1. Native shim (PowerShell — Bash mangles MSBuild /p: switches).
$env:COS_VFX_SDK_DIR = "<path-to-VideoFX-SDK>"
$env:COS_AR_SDK_DIR  = "<path-to-Maxine-AR-SDK-clone>"
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  native/shim/shim.vcxproj /p:Configuration=Release /p:Platform=x64

# 2. App (copies the shim next to the exe).
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild

# 3. Core unit tests.
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj
```

Without the SDK env vars the shim builds a CI-safe **passthrough stub** (effects
disabled) so the project still builds on machines without the SDK. For the
runtime env vars needed to actually run the effects, see `CLAUDE.md`.

## CI

Every PR and push to `main` is gated by GitHub Actions on a **self-hosted RTX
runner** — full co-versioned Maxine build, a stale-stub export verify, the App
build, and Core unit tests, all with warnings treated as errors. See
[`docs/ci/self-hosted-runner.md`](docs/ci/self-hosted-runner.md).

## Status

M1–M4 (Core, overlay passthrough, AI Green Screen, AI Eye Contact) and M5 part 1
(app-relative SDK discovery) are complete. Next: the M5 ship-time work —
runtime/model bundler, installer, multi-GPU models, license packaging.

## License

[MIT](LICENSE). NVIDIA Maxine SDKs are governed separately — see
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md). NVIDIA, Maxine, and RTX are
trademarks of NVIDIA Corporation; this project is not affiliated with NVIDIA.
