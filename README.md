<p align="center">
  <img src="cos.png" alt="Camera-on-Screen" width="200">
</p>

<h1 align="center">Camera-on-Screen</h1>

<p align="center">
  Put your webcam on top of your screen — live, draggable, captured in one pass.
</p>

<p align="center">
  <a href="https://github.com/officialdad/camera-on-screen/actions/workflows/ci.yml"><img src="https://github.com/officialdad/camera-on-screen/actions/workflows/ci.yml/badge.svg" alt="ci"></a>
</p>

---

A transparent, always-on-top webcam overlay for Windows. It floats your live
camera over everything else, so any screen recorder captures you and your screen
together — no editing afterward. Optional NVIDIA **AI Green Screen** (background
removal) and **AI Eye Contact** effects run in real time.

> **You need:** Windows + an **NVIDIA RTX GPU** with a recent driver for the AI
> effects. On other hardware the overlay still works — the AI effects are just
> turned off.

## Install

1. Download the latest `CameraOnScreen-Setup-<ver>-x64.exe` from
   [**Releases**](https://github.com/officialdad/camera-on-screen/releases).
2. Run it. It installs **per-user** (no admin), adds a Start Menu shortcut, and
   bundles everything it needs — no extra downloads.
3. Windows SmartScreen may warn (the installer is unsigned): click
   **More info → Run anyway**.

Uninstall from **Settings → Apps** as usual. Your preferences are kept at
`%LOCALAPPDATA%\CameraOnScreen\config.json`.

> The AI effects in this build are tuned for **RTX 30-series (Ampere)** GPUs. On
> other GPUs the app still installs and runs as a plain webcam overlay.

## Using it

- **Move it** — drag the centre **+** handle.
- **Resize it** — scroll the mouse wheel over the overlay.
- **Mirror / zoom** — toggle in the control panel.
- **AI Green Screen** — removes your background with adjustable edge expand /
  feather (RTX only).
- **AI Eye Contact** — gently redirects your gaze toward the camera (RTX only).

Then record with OBS, Game Bar, or any screen recorder — the overlay shows up
live in the capture.

## Contributing

Issues and pull requests are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for
how to build, the RTX/Maxine requirements, and the bar PRs need to clear.

## License

[MIT](LICENSE). The bundled NVIDIA Maxine runtime is governed separately under
the NVIDIA Maxine SDK License — see
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

## NVIDIA Maxine SDKs (not in this repo)

The AI effects use the **NVIDIA Maxine Video Effects SDK** (green screen) and
**NVIDIA Maxine AR SDK** (eye contact). These are **not bundled** in the
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
runtime env vars needed to actually run the effects, see
[`CLAUDE.md`](CLAUDE.md).

## CI / Release

Every PR and push to `main` is gated by GitHub Actions on a **self-hosted RTX
runner** — full co-versioned Maxine build, a stale-stub export verify, the App
build, and Core unit tests, all with warnings treated as errors. A second
workflow builds the installer and publishes a GitHub release on a `v*` tag. See
[`docs/ci/self-hosted-runner.md`](docs/ci/self-hosted-runner.md).

## Status

M1–M5 complete: Core, overlay passthrough, AI Green Screen, AI Eye Contact,
app-relative SDK discovery, the runtime/model **bundler**, the **installer** (a
per-user Inno Setup `.exe`), license compliance, and the tag-triggered release
pipeline — all verified on an RTX 3090. Remaining: multi-GPU models (Ampere
only today).
