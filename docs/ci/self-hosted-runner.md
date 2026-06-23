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

### `release.yml` only (tag `v*`) — not needed for build+test CI

- **Inno Setup 6**, installed **machine-wide** (`winget install JRSoftware.InnoSetup
  --scope machine` → `C:\Program Files (x86)\Inno Setup 6\ISCC.exe`). A per-user winget
  install lands in the interactive user's `%LOCALAPPDATA%`, which the `NETWORK SERVICE`
  runner cannot read — so it must be machine-scope.
- **Pre-built Maxine stage** (multi-GPU migration, VFX 1.2.0.0 + AR 1.1.1.0). The stage is
  version-pinned and does NOT change between app releases, so assemble it **once** and point
  **`COS_MAXINE_STAGE`** at it (reachable by `NETWORK SERVICE`, NOT under `C:\Users\<you>\…`).
  `release.yml` only checks it exists and prunes it — no per-release NGC download.

  One-time assembly (re-run only on an SDK bump), where the NGC key is needed:
  ```powershell
  $env:NGC_CLI_API_KEY = 'nvapi-...'   # only here, only once; not a CI secret
  scripts\assemble-maxine-stage.ps1 -OutStage C:\actions-runner\_sdk\maxine-stage `
    -VfxSdk $env:COS_VFX_SDK_DIR -ArSdk $env:COS_AR_SDK_DIR `
    -ArFeatureLibs C:\actions-runner\_sdk\ar-feature-libs   # the four nvAR* per-feature lib pkgs
  # then set COS_MAXINE_STAGE = C:\actions-runner\_sdk\maxine-stage (machine env) + restart the runner
  ```
  The old `COS_VFX_RUNTIME_DIR` / `COS_AR_RUNTIME_DIR` flat-runtime dirs are **no longer used by the
  bundler** (it prunes the stage); keep them only if you run the app directly on the runner.

## Required persistent environment variables

Set these as **machine/User** env vars (so the runner service sees them):

| Variable | Purpose |
|----------|---------|
| `COS_VFX_SDK_DIR` | VFX SDK source (headers + proxy) for the build — green screen |
| `COS_AR_SDK_DIR`  | AR SDK source clone (nvar/include + nvARProxy.cpp) for the build — eye contact |
| `COS_MAXINE_STAGE` | Pre-built co-versioned Maxine stage (assembled once, above) — `release.yml`'s bundler prunes it |

See the repo `CLAUDE.md` "Build & test" and the CO-VERSION gotcha for the exact SDK versions
(now VFX 1.2.0.0 + AR 1.1.1.0 / TRT 10.9). The build+test CI job needs only `COS_VFX_SDK_DIR`
and `COS_AR_SDK_DIR`; `release.yml` additionally needs `COS_MAXINE_STAGE` (pre-built) + Inno
Setup. The NGC key is used only at one-time stage assembly, **not** in CI.

## Why the export-verify step exists

The SDK build and the CI stub write the **same** DLL path; whichever built last
wins. The workflow's "Verify deployed shim" step fails the run if the deployed
DLL is the passthrough stub (missing `GreenScreen`/`GazeRedirection`, or
containing `not built in`), so a stale stub can never pass CI green.
