# Self-Hosted CI Runner (RTX)

CI builds the full co-versioned Maxine shim and therefore **cannot** run on
GitHub-hosted runners (no NVIDIA RTX GPU, no Maxine SDK there). It runs on a
self-hosted Windows runner on the RTX machine.

## Runner labels

The workflow targets `runs-on: [self-hosted, windows, rtx]`. When registering,
add the custom label `rtx` (the `self-hosted` and `windows` labels are applied
automatically).

## Register the runner

Repo-level registration (sufficient — repo admin only; use this if you lack org
admin):

1. GitHub → repo **Settings → Actions → Runners → New self-hosted runner**
   (Windows x64).
2. Run the shown `config.cmd` on the RTX machine; when prompted for additional
   labels, enter `rtx`.
3. Install it as a service (`svc install` / `svc start`) so CI works without an
   interactive session.

Org-level registration is equivalent but needs org-admin rights (the org runner
API returned 403 for the current account — repo-level is the fallback).

## Required prerequisites on the runner machine

- .NET 8 SDK
- Visual Studio 2022 Build Tools + MSVC v143 (provides MSBuild + `dumpbin`)
- A clone/install of the NVIDIA Maxine VFX SDK and AR SDK (build sources)

## Required persistent environment variables

Set these as **machine/User** env vars (so the runner service sees them):

| Variable | Purpose |
|----------|---------|
| `COS_VFX_SDK_DIR` | VFX SDK source (headers + proxy) for the build — green screen |
| `COS_AR_SDK_DIR`  | AR SDK source clone (nvar/include + nvARProxy.cpp) for the build — eye contact |
| `COS_VFX_RUNTIME_DIR` | VFX **0.7.6** runtime (co-versioned with AR 0.8.7 on TensorRT 10.4) — needed only if the runner ever runs the app; not required for build+test |

See the repo `CLAUDE.md` "Build & test" and the CO-VERSION gotcha for the exact
SDK versions. The build+test CI job needs only `COS_VFX_SDK_DIR` and
`COS_AR_SDK_DIR`.

## Why the export-verify step exists

The SDK build and the CI stub write the **same** DLL path; whichever built last
wins. The workflow's "Verify deployed shim" step fails the run if the deployed
DLL is the passthrough stub (missing `GreenScreen`/`GazeRedirection`, or
containing `not built in`), so a stale stub can never pass CI green.
