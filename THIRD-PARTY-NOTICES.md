# Third-Party Notices

Camera-on-Screen uses NVIDIA Maxine SDKs for its AI green-screen and AI
eye-contact features. This file describes those components and their licenses.

**Scope of licenses in this product:**

- The **Camera-on-Screen application** (overlay, control panel, and the native
  capture shim authored for this project) is licensed under the **MIT License**
  (see `LICENSE`). The MIT License covers **only this project's own code** — it
  does **not** cover the bundled NVIDIA components described below.
- The **bundled NVIDIA Maxine runtime libraries, their dependencies, and model
  files** are licensed by NVIDIA, not under MIT. See "NVIDIA Maxine SDKs" below.

## Source repository vs. binary distribution

The **source repository** contains **no** NVIDIA source, headers, proxy stubs,
binaries, or model files. The NVIDIA proxy sources and headers are compiled
from a developer-supplied SDK located via the `COS_VFX_SDK_DIR` and
`COS_AR_SDK_DIR` environment variables at build time.

The **distributed installer** (built by `scripts/build-installer.ps1` +
`scripts/bundle-maxine.ps1`) additionally bundles the co-versioned NVIDIA
Maxine runtime DLLs, their CUDA / TensorRT / third-party dependency libraries,
and pre-built model files, under the `maxine\` folder beside the application.
That redistribution is permitted under the NVIDIA Maxine SDK License Agreement
(see below); this notices file and the per-SDK license texts are installed
alongside those binaries.

## NVIDIA Maxine SDKs

This product builds against, and at runtime loads, two NVIDIA Maxine products:

- **NVIDIA Maxine Video Effects SDK** (`NvVFX_*`) — AI Green Screen.
- **NVIDIA Maxine AR SDK** (`NvAR_*`) — AI Eye Contact / Gaze Redirection.

Each SDK bundles its own pinned CUDA and TensorRT runtimes and OSS dependency
libraries. Use of the NVIDIA Maxine SDKs and their redistributable runtime is
governed by the **NVIDIA Maxine SDK License Agreement** and the per-component
third-party licenses distributed with each SDK. The full texts are installed
with this product:

| Installed file | Covers |
| --- | --- |
| `maxine\NVIDIA Maxine EULA.pdf` | NVIDIA Maxine SDK / Software License Agreement |
| `maxine\ThirdPartyLicenses-VFX.txt` | Video Effects SDK open-source dependencies |
| `maxine\ThirdPartyLicenses-AR.txt` | AR SDK open-source dependencies (FFT, NPP image primitives, etc.) |

The NVIDIA Maxine SDK License Agreement is also published at
<https://developer.nvidia.com/downloads/maxine-sdk-license>. The bundled NVIDIA
components are licensed for use only on systems with NVIDIA GPUs.

### NVIDIA source-code notice

This product's native shim incorporates NVIDIA-provided proxy/sample source
code (compiled from the SDK), distributed under the permission grant carried in
the SDK headers. As required by the NVIDIA Maxine SDK License Agreement:

> This software contains source code provided by NVIDIA Corporation.

### Attribution

This application makes use of the NVIDIA Maxine SDK and displays the attribution
**"AI effects powered by NVIDIA® Maxine™"** in its control panel, in accordance
with the NVIDIA Maxine branding guidelines
(<https://www.nvidia.com/maxine-sdk-guidelines>) and NVIDIA's trademark-attribution
format (® on first NVIDIA reference, ™ on the product mark, no debranding).

> The Maxine branding portal (`brand.nvidia.com`) is interactive and login-gated, so
> the exact mandated wording/placement could not be machine-verified. The wording above
> is a good-faith fit to NVIDIA's documented trademark format; confirm the precise Maxine
> string via the portal or `maxinesdk-support@nvidia.com`, and update
> `AppInfo.MaxineAttribution` (single source of truth) if it differs.

## Trademarks

NVIDIA, Maxine, RTX, GeForce, TensorRT, and CUDA are trademarks and/or
registered trademarks of NVIDIA Corporation in the United States and other
countries. This project is an independent work and is **not affiliated with,
sponsored by, or endorsed by NVIDIA Corporation**.
