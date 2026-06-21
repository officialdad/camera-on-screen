# CI/CD + Public Repo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish Camera-on-Screen as a public `officialdad/camera-on-screen` repo with MIT + NVIDIA legal notices, remove project-scoped skill tooling from the tree, and gate every build on a self-hosted RTX GitHub Actions runner.

**Architecture:** Local repo prep (untrack skills, add license/notice/README/CI yaml + runner doc) committed first; then create the public org repo and push existing `main` history; CI is a single GitHub Actions job on a `self-hosted, windows, rtx` runner that does the full co-versioned Maxine build + Core tests + a warning gate + a shim-export verify (automating the CLAUDE.md stale-stub gotcha).

**Tech Stack:** GitHub Actions (self-hosted Windows runner), `gh` CLI, MSVC v143 MSBuild, .NET 8 SDK, xUnit, MIT license.

## Global Constraints

- Repo: `officialdad/camera-on-screen`, **public**, default branch `main`, push existing history.
- CI runner: `runs-on: [self-hosted, windows, rtx]` — no GitHub-hosted runner (no GPU/SDK there).
- Build must stay **pristine (0 warnings)** — CI fails on any warning (`-warnaserror` / `/p:TreatWarningsAsErrors=true`).
- Shim build is **SDK config** (`COS_VFX_SDK_DIR`/`COS_AR_SDK_DIR` from runner env); CI must verify the deployed DLL exports `GreenScreen` **and** `GazeRedirection` and does **not** contain `not built in` (stale-stub guard).
- Native shim built with **PowerShell** MSBuild invocation, never Bash (Bash mangles `/p:` switches).
- Project license = **MIT**, © 2026 officialdad. NVIDIA notice is source-only (no binary redistribution this milestone).
- Skill tooling (`.agents/`, `skills-lock.json`) must be **untracked + gitignored**, copies relocated to `~/.claude/skills`.
- No CI artifact upload, no release workflow (deferred to M5 bundler/installer).

---

### Task 1: Untrack project skills + gitignore, relocate to user scope

**Files:**
- Modify: `.gitignore` (append skill paths)
- Untrack: `.agents/` (37 files), `skills-lock.json`
- Relocate (filesystem copy, outside repo): `.agents/skills/*` → `~/.claude/skills/`

**Interfaces:**
- Consumes: nothing.
- Produces: a clean tree where `git ls-files` shows no `.agents/` or `skills-lock.json`; the 5 skills exist under `~/.claude/skills/`.

- [ ] **Step 1: Copy skill trees to user scope (PowerShell tool — paths with `~`)**

```powershell
$dst = "$env:USERPROFILE\.claude\skills"
Get-ChildItem ".agents\skills" -Directory | ForEach-Object {
  Copy-Item $_.FullName -Destination (Join-Path $dst $_.Name) -Recurse -Force
}
Get-ChildItem $dst | Select-Object Name
```
Expected output includes: `csharp-async`, `dotnet-best-practices`, `winui-app`, `winui-design`, `winui-dev-workflow` (alongside the existing user skills).

- [ ] **Step 2: Untrack from the repo (keep working-tree copies)**

```bash
git rm -r --cached .agents skills-lock.json
```
Expected: `rm '.agents/...'` lines (~38 paths), files remain on disk.

- [ ] **Step 3: Add to `.gitignore`**

Append to `.gitignore` (after the existing MSVC-artifact block):

```gitignore

# Project-scoped agent skills — user-scoped instead (relocated to ~/.claude/skills)
/.agents/
/skills-lock.json
```

- [ ] **Step 4: Verify untracked**

```bash
git ls-files | grep -iE '\.agents|skills-lock' || echo "CLEAN: no skill files tracked"
git status --porcelain .agents skills-lock.json || true
```
Expected: prints `CLEAN: no skill files tracked`; `git status` shows no untracked `.agents`/`skills-lock.json` (they are now ignored).

- [ ] **Step 5: Commit**

```bash
git add .gitignore
git commit -m "chore: untrack project-scoped skills; relocate to user scope

.agents/ and skills-lock.json are third-party agent-skill tooling, not part
of the app. Untrack + gitignore so the public repo stays clean; copies live
in ~/.claude/skills for global use.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: MIT license + NVIDIA third-party notice

**Files:**
- Create: `LICENSE`
- Create: `THIRD-PARTY-NOTICES.md`

**Interfaces:**
- Consumes: nothing.
- Produces: `LICENSE` (GitHub detects as MIT), `THIRD-PARTY-NOTICES.md` (referenced by README in Task 5).

- [ ] **Step 1: Write `LICENSE` (exact MIT text)**

```
MIT License

Copyright (c) 2026 officialdad

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

- [ ] **Step 2: Write `THIRD-PARTY-NOTICES.md` (exact content)**

```markdown
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
```

- [ ] **Step 3: Verify**

```bash
test -f LICENSE && head -1 LICENSE && test -f THIRD-PARTY-NOTICES.md && echo "OK both present"
```
Expected: `MIT License` then `OK both present`.

- [ ] **Step 4: Commit**

```bash
git add LICENSE THIRD-PARTY-NOTICES.md
git commit -m "docs(legal): add MIT LICENSE + NVIDIA third-party/trademark notice

Source-only repo redistributes no NVIDIA material; notice states the Maxine
VFX+AR build/runtime dependency, NVIDIA EULA governance, trademarks, and the
deferred binary-bundle terms (M5).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: CI workflow — self-hosted RTX build + test gate

**Files:**
- Create: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: runner env vars `COS_VFX_SDK_DIR`, `COS_AR_SDK_DIR` (documented in Task 4).
- Produces: a `ci` workflow gating PRs and `main` pushes.

- [ ] **Step 1: Write `.github/workflows/ci.yml` (exact content)**

```yaml
name: ci

on:
  push:
    branches: [main]
  pull_request:

# One run per ref; cancel superseded runs.
concurrency:
  group: ci-${{ github.ref }}
  cancel-in-progress: true

jobs:
  build-and-test:
    name: Build (Maxine) + Test
    runs-on: [self-hosted, windows, rtx]
    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Build native shim (SDK config, x64 Release)
        shell: pwsh
        run: |
          $msbuild = "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe"
          & $msbuild native/shim/shim.vcxproj `
            /p:Configuration=Release /p:Platform=x64 `
            /warnaserror /nologo
          if ($LASTEXITCODE -ne 0) { throw "shim build failed ($LASTEXITCODE)" }

      - name: Verify deployed shim has both Maxine effects (stale-stub guard)
        shell: pwsh
        run: |
          $dll = "native/shim/x64/Release/CameraOnScreen.Shim.dll"
          if (-not (Test-Path $dll)) { throw "shim DLL missing: $dll" }
          $dumpbin = (Get-ChildItem "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC" -Directory |
            Sort-Object Name -Descending | Select-Object -First 1).FullName + "/bin/Hostx64/x64/dumpbin.exe"
          $exports = & $dumpbin /exports $dll | Out-String
          $strings = [System.IO.File]::ReadAllText($dll)
          $hasGS  = $exports -match 'GreenScreen'   -or $strings -match 'GreenScreen'
          $hasGaze = $exports -match 'GazeRedirection' -or $strings -match 'GazeRedirection'
          $isStub = $strings -match 'not built in'
          Write-Host "GreenScreen=$hasGS GazeRedirection=$hasGaze stub=$isStub"
          if (-not $hasGS)   { throw "GreenScreen missing — green-screen effect not built in" }
          if (-not $hasGaze) { throw "GazeRedirection missing — eye-contact effect not built in" }
          if ($isStub)       { throw "shim is the passthrough STUB ('not built in' present)" }

      - name: Build App (copies shim, warnings = errors)
        shell: pwsh
        run: |
          dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj `
            -c Release -t:Rebuild /p:TreatWarningsAsErrors=true --nologo
          if ($LASTEXITCODE -ne 0) { throw "App build failed ($LASTEXITCODE)" }

      - name: Test Core (xUnit, warnings = errors)
        shell: pwsh
        run: |
          dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj `
            -c Release /p:TreatWarningsAsErrors=true --nologo
          if ($LASTEXITCODE -ne 0) { throw "Core tests failed ($LASTEXITCODE)" }
```

- [ ] **Step 2: Validate YAML syntax locally**

```bash
python -c "import yaml,sys; yaml.safe_load(open('.github/workflows/ci.yml')); print('YAML OK')"
```
Expected: `YAML OK`. (If `python` absent, skip — GitHub validates on push; Task 6 confirms the run.)

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: self-hosted RTX build+test gate with stale-stub export verify

Single job on [self-hosted, windows, rtx]: SDK-config shim build, verify the
deployed DLL exports GreenScreen+GazeRedirection (not the passthrough stub),
App -t:Rebuild, Core xUnit. Warnings are errors throughout (pristine rule).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Self-hosted runner setup doc

**Files:**
- Create: `docs/ci/self-hosted-runner.md`

**Interfaces:**
- Consumes: nothing.
- Produces: human runbook for registering the `rtx` runner; referenced by README Task 5.

- [ ] **Step 1: Write `docs/ci/self-hosted-runner.md` (exact content)**

```markdown
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
```

- [ ] **Step 2: Verify + commit**

```bash
test -f docs/ci/self-hosted-runner.md && echo OK
git add docs/ci/self-hosted-runner.md
git commit -m "docs(ci): self-hosted RTX runner setup runbook

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Public README

**Files:**
- Create: `README.md`

**Interfaces:**
- Consumes: `LICENSE` (Task 2), `THIRD-PARTY-NOTICES.md` (Task 2), `docs/ci/self-hosted-runner.md` (Task 4), `ci.yml` (Task 3, for the badge).
- Produces: the public face of the repo.

- [ ] **Step 1: Write `README.md` (exact content)**

```markdown
# Camera-on-Screen

[![ci](https://github.com/officialdad/camera-on-screen/actions/workflows/ci.yml/badge.svg)](https://github.com/officialdad/camera-on-screen/actions/workflows/ci.yml)

A Windows webcam **desktop-overlay** app. A transparent, always-on-top,
draggable overlay shows your live webcam so any screen recorder captures it
live in one pass — no post-edit. Optional NVIDIA Maxine **AI Green Screen** and
**AI Eye Contact** effects composite in real time.

> **Requirements:** Windows + an **NVIDIA RTX GPU** with a recent driver. On
> non-RTX hardware the app still runs, but the AI effects are disabled by
> design.

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
```

- [ ] **Step 2: Verify + commit**

```bash
test -f README.md && grep -q "officialdad/camera-on-screen" README.md && echo OK
git add README.md
git commit -m "docs: public README (overview, RTX requirement, build, CI badge, license)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: Create public repo, push, verify CI

**Files:** none (git/gh operations).

**Interfaces:**
- Consumes: all prior commits on `main`.
- Produces: live `officialdad/camera-on-screen` public repo with `origin` remote and a CI run.

- [ ] **Step 1: Confirm no remote yet + clean tree**

```bash
git remote -v; git status --porcelain
```
Expected: no `origin`; clean working tree (all prior tasks committed).

- [ ] **Step 2: Create the public repo and push `main`**

```bash
gh repo create officialdad/camera-on-screen \
  --public \
  --source . \
  --remote origin \
  --description "Windows RTX webcam desktop-overlay with NVIDIA Maxine AI green screen + eye contact" \
  --push
```
Expected: repo created, `origin` added, `main` pushed.

- [ ] **Step 3: Verify repo + tracked-file hygiene on the remote**

```bash
gh repo view officialdad/camera-on-screen --json visibility,licenseInfo --jq '{visibility,license:.licenseInfo.key}'
git ls-files | grep -iE '\.agents|skills-lock' && echo "LEAK" || echo "CLEAN: no skill files"
```
Expected: `{"visibility":"PUBLIC","license":"mit"}` and `CLEAN: no skill files`.

- [ ] **Step 4: Confirm CI registered (run appears once a runner is online)**

```bash
gh run list --repo officialdad/camera-on-screen --workflow ci.yml --limit 3
```
Expected: a `ci` run listed (queued/in-progress/completed). If it stays
**queued**, the self-hosted `rtx` runner is not yet registered/online — follow
`docs/ci/self-hosted-runner.md`. This runner registration is a **manual human
step**; CI cannot self-provision it.

- [ ] **Step 5 (human gate): runner online → run green**

Once the runner is registered and online, the run must finish **green**: shim
SDK build, export-verify passes (both effects present, not stub), App build,
Core tests — all with warnings-as-errors. Re-check:

```bash
gh run list --repo officialdad/camera-on-screen --workflow ci.yml --limit 1
```
Expected: latest conclusion `success`.

---

## Self-Review

**Spec coverage:**
- Repo creation/push → Task 6. ✓
- Skills untrack + relocate → Task 1. ✓
- MIT LICENSE → Task 2. ✓
- NVIDIA third-party/trademark notice → Task 2. ✓
- CI self-hosted build + test + warning gate + export-verify → Task 3. ✓
- Runner setup doc → Task 4. ✓
- README → Task 5. ✓
- No artifact / no release workflow → enforced by omission; stated in Global Constraints. ✓
- Repo-level runner fallback for missing org admin → Task 4 doc. ✓

**Placeholder scan:** Concrete file contents in every create step. `<path-to-...>` placeholders in the README are intentional user-supplied paths (a published README must not hardcode one machine's absolute paths), not plan gaps. No TBD/TODO.

**Type/name consistency:** Workflow file `ci.yml`, label `rtx`, repo `officialdad/camera-on-screen`, DLL path `native/shim/x64/Release/CameraOnScreen.Shim.dll`, env vars `COS_VFX_SDK_DIR`/`COS_AR_SDK_DIR` — used consistently across Tasks 3, 4, 5, 6 and the spec.

## Notes / risks

- **Order matters:** Tasks 1–5 commit locally first, Task 6 pushes — so the public repo's tip is clean. (History still contains the old skill tree; accepted per spec Open Risks. A history rewrite is out of scope.)
- **CI green depends on the runner**, which is a manual human registration step. The plan's automated portion ends at "run is queued/registered"; the green conclusion is a human gate (Task 6 Step 5).
- `dumpbin` path globs the newest MSVC toolset under Build Tools; if the runner uses full VS or a different layout, adjust the path in `ci.yml`.
