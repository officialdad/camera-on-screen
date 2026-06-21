# Third-Party Notices

Camera-on-Screen depends on NVIDIA Maxine SDKs **at build and run time**. These
SDKs are **not included** in this repository and are **not redistributed** by
this project. You must obtain them directly from NVIDIA.

## NVIDIA Maxine SDKs

This project builds against, and at runtime loads, two NVIDIA Maxine products:

- **NVIDIA Maxine Video Effects SDK** (`NvVFX_*`) — AI Green Screen.
- **NVIDIA Maxine AR SDK** (`NvAR_*`) — AI Eye Contact / Gaze Redirection.

Each SDK bundles its own pinned CUDA and TensorRT runtimes. This repository
contains **no NVIDIA source, headers, proxy stubs, binaries, or model files** —
the NVIDIA proxy sources and headers are compiled from a developer-supplied SDK
located via the `COS_VFX_SDK_DIR` and `COS_AR_SDK_DIR` environment variables at
build time, and the runtimes are resolved at run time (see the project README).

Use of the NVIDIA Maxine SDKs is governed by the **NVIDIA Software Developer
License Agreement** and the per-feature model licenses distributed with each
SDK (the license PDFs in each SDK's `license/` directory). Download the SDKs and
review those terms here:

- https://developer.nvidia.com/maxine

## Trademarks

NVIDIA, Maxine, RTX, GeForce, TensorRT, and CUDA are trademarks and/or
registered trademarks of NVIDIA Corporation in the United States and other
countries. This project is an independent work and is **not affiliated with,
sponsored by, or endorsed by NVIDIA Corporation**.

## Future binary distribution

This repository is **source only**. A future packaged/installer build (project
milestone M5) will bundle the co-versioned NVIDIA Maxine runtime DLLs and model
files alongside the application. That distribution will additionally be subject
to NVIDIA's redistribution terms and the bundled-model licenses, which will be
included with the installer. No such binaries are distributed today.
