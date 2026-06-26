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
That redistribution is permitted under the 2025 NVIDIA Software License
Agreement + AI Product-Specific Terms (runtime DLLs) and the NVIDIA Open Model
License / Community Model License (the model engines); this notices file and the
license texts are installed alongside those binaries.

## NVIDIA Maxine SDKs

This product builds against, and at runtime loads, two NVIDIA Maxine products:

- **NVIDIA Maxine Video Effects SDK** (`NvVFX_*`) — AI Green Screen.
- **NVIDIA Maxine AR SDK** (`NvAR_*`) — AI Eye Contact / Gaze Redirection.

Each SDK bundles its own pinned CUDA and TensorRT runtimes and OSS dependency
libraries. Use of the NVIDIA Maxine SDKs and their redistributable runtime is
governed by NVIDIA's 2025 license framework and the per-component third-party
licenses distributed with each SDK. The full texts are installed with this
product:

| Installed file | Covers |
| --- | --- |
| `maxine\NVIDIA-Software-License-Agreement-2025.05.05.pdf` | The runtime DLLs / SDK (with the Product-Specific Terms below) |
| `maxine\product-specific-terms-for-nvidia-ai-products-2025.05.05.pdf` | AI Product-Specific Terms (exhibit to the Software License Agreement) |
| `maxine\NVIDIA-Open-Model-License-Agreements-24-10-2025.pdf` | Green-screen (AIGS) model engines |
| `maxine\NVIDIA-Models-Community-License-2025-04-15.pdf` | Gaze / face-box / landmark model engines |
| `maxine\ThirdPartyLicenses-VFX.txt` | Video Effects SDK open-source dependencies |
| `maxine\ThirdPartyLicenses-AR.txt` | AR SDK open-source dependencies |

The bundled model engines are redistributed under the NVIDIA Community Model
License §1.2 exception (i) — they are designated for use with NVIDIA RTX /
GeForce RTX GPUs, run locally on a single user's PC, and are shipped with a copy
of the agreement (unmodified, notices intact). The NVIDIA Software License
Agreement is also published at
<https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-software-license-agreement/>.
The bundled NVIDIA components are licensed for use only on systems with NVIDIA GPUs.

### NVIDIA source-code notice

This product's native shim incorporates NVIDIA-provided proxy/sample source
code (compiled from the SDK), distributed under the permission grant carried in
the SDK headers. As required by the AI Product-Specific Terms §1.7.1:

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

## NVIDIA Optical Flow SDK

This product builds against, and at runtime loads, the NVIDIA Optical Flow SDK for its FRUC
(AI frame-rate upscaling, 30→60 fps) feature:

- **NVIDIA Optical Flow SDK v5.0.7** — `NvOFFRUC.dll` (frame-rate upscaler) and its CUDA 11
  runtime dependency `cudart64_110.dll`.

> **REDISTRIBUTION NOT YET HUMAN-VERIFIED — LEGAL GATE REQUIRED.**
> Unlike the Maxine SDKs above (redistribution cleared after human review), the right to
> redistribute `NvOFFRUC.dll` and `cudart64_110.dll` under the NVIDIA Optical Flow SDK license
> has **not yet been confirmed by a human reviewer**. These binaries **must not be included in a
> distributed installer** until this gate is cleared.
>
> The SDK license agreement is at:
> `C:\actions-runner\_sdk\Optical_Flow_SDK_5.0.7\Optical_Flow_SDK_5.0.7\LicenseAgreement.pdf`
> (also available with the SDK download from the NVIDIA developer site).
>
> A reviewer must confirm: (1) whether the EULA permits no-charge binary redistribution of
> `NvOFFRUC.dll` and `cudart64_110.dll` in a per-user installer; (2) whether any copy-of-agreement,
> attribution, or branding obligations apply; and (3) whether the CUDA 11 runtime bundled with the
> OF SDK carries its own redistribution terms. Update this entry and `maxine-manifest.psd1` once
> cleared (mirroring the Maxine entry structure above).

## Trademarks

NVIDIA, Maxine, RTX, GeForce, TensorRT, and CUDA are trademarks and/or
registered trademarks of NVIDIA Corporation in the United States and other
countries. This project is an independent work and is **not affiliated with,
sponsored by, or endorsed by NVIDIA Corporation**.
