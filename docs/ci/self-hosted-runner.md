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
- **Maxine stage sources** (multi-GPU migration, VFX 1.2.0.0 + AR 1.1.1.0). `release.yml`
  runs `scripts/assemble-maxine-stage.ps1` which reads the build SDK trees
  (`COS_VFX_SDK_DIR` / `COS_AR_SDK_DIR`) plus a new **`COS_AR_FEATURE_LIBS`** dir holding the
  four AR per-feature lib packages (`<root>\<name>\<name>\{bin,license}` for
  `nvargazeredirection` / `nvarfaceboxdetection` / `nvarlandmarkdetection` /
  `nvarfaceexpressions`), all reachable by `NETWORK SERVICE` (NOT under `C:\Users\<you>\…`).
- **NGC API key** as repo secret **`NGC_CLI_API_KEY`** — the assemble step fetches the
  multi-arch engines (`scripts/fetch-maxine-engines.ps1`) from NGC. (Pre-seed the engines into
  the stage + `-SkipEngineFetch` for an air-gapped runner.) The old `COS_VFX_RUNTIME_DIR` /
  `COS_AR_RUNTIME_DIR` flat-runtime dirs are **no longer used by the bundler** (it now prunes the
  assembled stage); keep them only if you still run the app directly on the runner.

## Required persistent environment variables

Set these as **machine/User** env vars (so the runner service sees them):

| Variable | Purpose |
|----------|---------|
| `COS_VFX_SDK_DIR` | VFX SDK source (headers + proxy) for the build — green screen |
| `COS_AR_SDK_DIR`  | AR SDK source clone (nvar/include + nvARProxy.cpp) for the build — eye contact |
| `COS_AR_FEATURE_LIBS` | Root of the four AR per-feature lib packages (gaze + deps) — `release.yml`'s stage-assembly step; not needed for build+test |
| `COS_VFX_RUNTIME_DIR` | Optional: assembled co-versioned stage if you run the app directly on the runner — no longer used by the bundler |

Plus the repo secret **`NGC_CLI_API_KEY`** for the release engine fetch. See the repo
`CLAUDE.md` "Build & test" and the CO-VERSION gotcha for the exact SDK versions (now VFX
1.2.0.0 + AR 1.1.1.0 / TRT 10.9). The build+test CI job needs only `COS_VFX_SDK_DIR` and
`COS_AR_SDK_DIR`; `release.yml` additionally needs `COS_AR_FEATURE_LIBS`, the NGC secret, and
Inno Setup.

## Why the export-verify step exists

The SDK build and the CI stub write the **same** DLL path; whichever built last
wins. The workflow's "Verify deployed shim" step fails the run if the deployed
DLL is the passthrough stub (missing `GreenScreen`/`GazeRedirection`, or
containing `not built in`), so a stale stub can never pass CI green.
