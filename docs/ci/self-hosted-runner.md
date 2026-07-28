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

## Linux RTX runner (Phase 5 — `ci-linux.yml` `maxine-rtx` job)

The CachyOS dev box (RTX 3090) is registered as a **repo-level** runner named
`cachyos-rtx3090` with labels `[self-hosted, linux, rtx]`. It runs the SDK-config
shim build + deploy-check + Maxine capability probe (GPU-only; the full camera
gate `effects_drive` stays manual because the box's camera is in interactive use).

Setup (already done on the box; repeat only on a rebuild):

```bash
mkdir ~/actions-runner && cd ~/actions-runner
curl -sLo r.tar.gz https://github.com/actions/runner/releases/download/v<VER>/actions-runner-linux-x64-<VER>.tar.gz
tar xzf r.tar.gz && rm r.tar.gz
TOKEN=$(gh api -X POST repos/officialdad/camera-on-screen/actions/runners/registration-token --jq .token)
./config.sh --unattended --url https://github.com/officialdad/camera-on-screen \
  --token "$TOKEN" --name cachyos-rtx3090 --labels rtx --replace
```

No sudo on the box, so **not** `svc.sh` — a **user** systemd unit
(`~/.config/systemd/user/actions-runner.service`, `ExecStart=~/actions-runner/run.sh`,
`Restart=always`), then:

```bash
systemctl --user enable --now actions-runner.service
loginctl enable-linger ariff   # runner survives logout/reboot without a session
```

Environment: the runner reads `~/actions-runner/.env` at service start —
`COS_VFX_SDK_DIR` / `COS_AR_SDK_DIR` point at the **Linux SDK-core trees**
(`~/dev/VideoFX-linux/VideoFX`, `~/dev/ARSDK-linux/ARSDK`), and `COS_FRUC_SDK_DIR` at
the **Optical Flow SDK root** (`~/dev/Optical_Flow_SDK_5.0.7` — manual developer-site
download, issue #35; enables FRUC in the shim build + `maxine/fruc` in bundles). Same
caveat as Windows: changing `.env` needs `systemctl --user restart actions-runner`. Two lessons already
paid for: the runner's `.path` file did **not** reach job PATH under this user unit, so
the `maxine-rtx` job adds user-local tool dirs (`~/.local/bin` — cmake) via
`$GITHUB_PATH` instead; and do **not** set `KillMode=process` in the unit — it kills
only `run.sh` on restart, leaving an orphan `Runner.Listener` with the stale
environment to keep taking jobs.
