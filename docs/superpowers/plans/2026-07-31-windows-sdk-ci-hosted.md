# Windows SDK-Config CI on Hosted Runners (#38 option 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore the parked Windows PR-CI coverage from issue #38 — shim **Maxine SDK-config** MSVC build + the export-verify gate — on GitHub-**hosted** `windows-latest` runners, with no Windows VM and no GPU.

**Architecture:** Hosted `windows-latest` already ships MSBuild/MSVC v143 + .NET 8, so issue #38 option 1's real cost (toolchain reconstruction) vanishes. The SDK-config build needs only *headers + proxy sources* (the proxies LoadLibrary the runtime at run time; CI never runs the DLL). Those files are NVIDIA-proprietary, so they live in a tiny **private build-kit repo** (`officialdad/maxine-winci-buildkit`, ~few hundred KB), assembled once by a script from the SDK trees already on the dev box, and checked out in CI via a read-only deploy key. `ci.yml`'s existing Windows job gains: kit checkout → (existing) stub build + stub assert → SDK-config rebuild → export-verify (`GreenScreen` + `GazeRedirection` present, `not built in` absent) → App build (deploys the SDK shim — "deploy the right shim" order preserved) → Core tests.

**Tech Stack:** GitHub Actions (`windows-latest`), MSBuild `shim.vcxproj`, bash assembly script, `gh` CLI (repo/deploy-key/secret setup).

## Global Constraints

- Builds pristine: shim `\warnaserror`, App/tests `TreatWarningsAsErrors=true` (CLAUDE.md: warnings are findings).
- The build kit contains **headers + proxy `.cpp` sources only** — never runtime DLLs, models, or engines (NVIDIA 2025 Software License: no public redistribution; private repo = internal use).
- **No NVIDIA-proprietary file enters the public repo.** The authored VSR compat header is our own text (2 API macros) and lives inside the assembly script heredoc — that is the only SDK-related content committed publicly.
- Verified co-version pins: VFX **1.2.0.0** + AR **1.1.1.0** (headers/proxies must stay this pair).
- `shim.vcxproj` stays out of `CameraOnScreen.sln`; x64 only; vcxproj expects kit layout `<dir>\nvvfx\include`, `<dir>\nvvfx\src`, `<dir>\features\nvvfxgreenscreen\include`, `<dir>\features\nvvfxvideosuperres\include`, `<dir>\nvar\include`, `<dir>\nvar\src` (`shim.vcxproj:82-136`).
- Fork PRs have no secrets: SDK-config steps must **skip** (not fail) when the deploy-key secret is absent; the existing stub coverage still runs.
- FRUC stays a stub on hosted CI (Optical Flow SDK is developer-program-gated + needs CUDA toolkit). Safe: `fruc.cpp:257` — its stub message deliberately avoids the `not built in` marker, so export-verify still passes on an SDK-without-FRUC build.
- `release.yml` untouched — installer releases remain parked on #38 (option 3, dual-boot on release day).
- MSVC compile can only be verified ON the hosted runner — the test loop for CI changes is: push branch → open PR → `gh pr checks --watch`.

## File Structure

- Create: `scripts/assemble-winci-buildkit.sh` — one-shot kit assembly from dev-box SDK trees (committed to the public repo; documents kit provenance and is the reproducible source of the authored VSR header).
- Create (out of repo): `~/dev/maxine-winci-buildkit/` → pushed to private repo `officialdad/maxine-winci-buildkit`.
- Modify: `.github/workflows/ci.yml` — extend the `windows-compile` job.
- Modify: `CLAUDE.md` — CI/CD section, one-paragraph update.
- GitHub state: private repo, read-only deploy key on it, `MAXINE_BUILDKIT_SSH_KEY` actions secret on `officialdad/camera-on-screen`, comment on issue #38.

---

### Task 1: Build-kit assembly script + private repo + deploy key

**Files:**
- Create: `scripts/assemble-winci-buildkit.sh`

**Interfaces:**
- Produces: private repo `officialdad/maxine-winci-buildkit` with layout `vfx/nvvfx/{include,src}`, `vfx/features/{nvvfxgreenscreen,nvvfxvideosuperres}/include`, `ar/nvar/{include,src}` — exactly what `shim.vcxproj` expects for `CosVfxSdkDir`/`CosArSdkDir`.
- Produces: repo secret `MAXINE_BUILDKIT_SSH_KEY` on `officialdad/camera-on-screen` (consumed by Task 2).

- [ ] **Step 1: Write the script**

Create `scripts/assemble-winci-buildkit.sh` (mode 755):

```bash
#!/bin/bash
# Assemble the PRIVATE Windows-CI Maxine build kit (issue #38 option 1): the header +
# proxy-source subset that lets hosted windows-latest CI compile the shim's SDK config
# (COS_HAS_MAXINE + COS_HAS_MAXINE_AR). No runtime DLLs, no models — compile-only, so
# no GPU and no NGC at CI time. Sources are the SDK trees already on the dev box:
#   VFX 1.2.0.0 Linux SDK-core  (core headers, greenscreen header, proxy sources — OS-shared)
#   Maxine-VFX-SDK GitHub clone (MIT; nvTransferD3D{,11}.h — absent from the Linux tree,
#                                signatures verified identical to the 1.2 proxy's wrappers)
#   Maxine-AR-SDK GitHub clone  (AR 1.1.1.0 headers + nvARProxy.cpp — same tree the old
#                                Windows box built against)
# nvVFXVideoSuperRes.h is a COMPILE-COMPAT STAND-IN authored below (the real header ships
# only in the Windows VSR feature install, which died with the old dev box; NGC SDK zips
# are NVAIE-gated). Replace it with the real header when a Windows SDK install exists.
#
# The kit is NVIDIA-proprietary material -> push ONLY to the private repo
# officialdad/maxine-winci-buildkit. Never commit it to camera-on-screen.
#
# Usage: scripts/assemble-winci-buildkit.sh [outdir]
set -euo pipefail

OUT="${1:-$HOME/dev/maxine-winci-buildkit}"
VFX_LINUX="$HOME/dev/VideoFX-linux/VideoFX"
VFX_CLONE="$HOME/dev/Maxine-VFX-SDK"
AR_CLONE="$HOME/dev/Maxine-AR-SDK"

for d in "$VFX_LINUX" "$VFX_CLONE" "$AR_CLONE"; do
    [ -d "$d" ] || { echo "missing SDK tree: $d" >&2; exit 1; }
done

mkdir -p "$OUT/vfx/nvvfx/include" "$OUT/vfx/nvvfx/src" \
         "$OUT/vfx/features/nvvfxgreenscreen/include" \
         "$OUT/vfx/features/nvvfxvideosuperres/include" \
         "$OUT/ar/nvar/include" "$OUT/ar/nvar/src"

cp "$VFX_LINUX"/include/nvVideoEffects.h "$VFX_LINUX"/include/nvCVImage.h \
   "$VFX_LINUX"/include/nvCVStatus.h "$OUT/vfx/nvvfx/include/"
cp "$VFX_CLONE"/nvvfx/include/nvTransferD3D.h "$VFX_CLONE"/nvvfx/include/nvTransferD3D11.h \
   "$OUT/vfx/nvvfx/include/"
cp "$VFX_LINUX"/share/nvvfx/src/nvVideoEffectsProxy.cpp \
   "$VFX_LINUX"/share/nvvfx/src/nvCVImageProxy.cpp "$OUT/vfx/nvvfx/src/"
cp "$VFX_LINUX"/features/nvvfxgreenscreen/include/nvVFXGreenScreen.h \
   "$OUT/vfx/features/nvvfxgreenscreen/include/"

cat > "$OUT/vfx/features/nvvfxvideosuperres/include/nvVFXVideoSuperRes.h" <<'EOF'
// COMPILE-COMPAT STAND-IN for the NVIDIA VSR feature header (nvvfxvideosuperres).
// The real header ships only via the Windows VFX SDK feature install (install_feature.ps1),
// which died with the old dev box — issue #38. This defines exactly what superres.cpp
// consumes: the effect selector + the quality-level parameter selector (CLAUDE.md, VFX
// feature catalog). Used ONLY by the hosted Windows CI compile job; never shipped.
// The NVVFX_QUALITY_LEVEL string value is best-effort — REPLACE THIS FILE with the real
// header before trusting any runtime behavior.
#ifndef NVVFXVIDEOSUPERRES_H_COMPAT
#define NVVFXVIDEOSUPERRES_H_COMPAT
#define NVVFX_FX_VIDEO_SUPER_RES "VideoSuperRes"
#define NVVFX_QUALITY_LEVEL "QualityLevel"
#endif
EOF

cp "$AR_CLONE"/nvar/include/nvAR.h "$AR_CLONE"/nvar/include/nvAR_defs.h \
   "$AR_CLONE"/nvar/include/nvCVImage.h "$AR_CLONE"/nvar/include/nvCVStatus.h \
   "$OUT/ar/nvar/include/"
cp "$AR_CLONE"/nvar/src/nvARProxy.cpp "$OUT/ar/nvar/src/"

cat > "$OUT/README.md" <<EOF
# maxine-winci-buildkit (PRIVATE — do not make public)

Header + proxy-source subset for camera-on-screen's hosted Windows CI SDK-config compile
(issue #38 option 1). NVIDIA-proprietary material under the 2025 NVIDIA Software License —
internal use only, no redistribution. No runtime DLLs or models.

- vfx/: VFX 1.2.0.0 (core headers + proxies from the Linux SDK-core tree — OS-shared;
  nvTransferD3D{,11}.h from the MIT GitHub clone; nvVFXVideoSuperRes.h is an authored
  compile-compat stand-in, see its header comment)
- ar/:  AR 1.1.1.0 from the Maxine-AR-SDK GitHub clone @ $(git -C "$AR_CLONE" rev-parse HEAD)

Regenerate: scripts/assemble-winci-buildkit.sh in officialdad/camera-on-screen.
Consumed by: .github/workflows/ci.yml (deploy-key checkout, path .buildkit).
EOF

# Self-check: every file shim.vcxproj's SDK config compiles or includes must exist.
for f in vfx/nvvfx/include/nvVideoEffects.h vfx/nvvfx/include/nvCVImage.h \
         vfx/nvvfx/include/nvCVStatus.h vfx/nvvfx/include/nvTransferD3D.h \
         vfx/nvvfx/include/nvTransferD3D11.h vfx/nvvfx/src/nvVideoEffectsProxy.cpp \
         vfx/nvvfx/src/nvCVImageProxy.cpp \
         vfx/features/nvvfxgreenscreen/include/nvVFXGreenScreen.h \
         vfx/features/nvvfxvideosuperres/include/nvVFXVideoSuperRes.h \
         ar/nvar/include/nvAR.h ar/nvar/include/nvAR_defs.h \
         ar/nvar/include/nvCVImage.h ar/nvar/include/nvCVStatus.h \
         ar/nvar/src/nvARProxy.cpp; do
    [ -f "$OUT/$f" ] || { echo "SELF-CHECK FAILED: missing $f" >&2; exit 1; }
done
echo "kit assembled at $OUT"
```

- [ ] **Step 2: Run it and verify the self-check passes**

Run: `bash scripts/assemble-winci-buildkit.sh`
Expected: `kit assembled at /home/ariff/dev/maxine-winci-buildkit`, exit 0. Re-run once more to confirm idempotence (same output, no error).

- [ ] **Step 3: Create the private repo and push the kit**

```bash
cd ~/dev/maxine-winci-buildkit
git init -b main
git add -A
git commit -m "Windows-CI Maxine build kit: VFX 1.2.0.0 + AR 1.1.1.0 headers/proxies (camera-on-screen #38)"
gh repo create officialdad/maxine-winci-buildkit --private --source . --push
```

Expected: repo created, push succeeds. Verify privacy: `gh repo view officialdad/maxine-winci-buildkit --json visibility -q .visibility` → `PRIVATE`.

- [ ] **Step 4: Deploy key (read-only) + actions secret**

```bash
KEYDIR=$(mktemp -d)
ssh-keygen -t ed25519 -N "" -f "$KEYDIR/buildkit_key" -C "camera-on-screen ci.yml buildkit checkout"
gh repo deploy-key add "$KEYDIR/buildkit_key.pub" -R officialdad/maxine-winci-buildkit \
  --title "camera-on-screen ci.yml (read-only)"
gh secret set MAXINE_BUILDKIT_SSH_KEY -R officialdad/camera-on-screen < "$KEYDIR/buildkit_key"
rm -rf "$KEYDIR"
```

Expected: `gh repo deploy-key list -R officialdad/maxine-winci-buildkit` shows the key with read-only: true; `gh secret list -R officialdad/camera-on-screen` shows `MAXINE_BUILDKIT_SSH_KEY`.

- [ ] **Step 5: Commit the script on a feature branch**

```bash
cd /home/ariff/officialdad/camera-on-screen
git checkout -b ci/windows-sdk-hosted
git add scripts/assemble-winci-buildkit.sh
git commit -m "ci: add Windows-CI Maxine build-kit assembly script (#38)"
```

---

### Task 2: ci.yml — SDK-config build + export-verify on the hosted Windows job

**Files:**
- Modify: `.github/workflows/ci.yml` (whole file below)

**Interfaces:**
- Consumes: secret `MAXINE_BUILDKIT_SSH_KEY` + private repo from Task 1 (checked out at `.buildkit`; `CosVfxSdkDir=<ws>\.buildkit\vfx`, `CosArSdkDir=<ws>\.buildkit\ar`).
- Produces: PR gate — SDK-config shim compiles, export-verify passes, App deploys the SDK shim, Core tests run.

- [ ] **Step 1: Replace `.github/workflows/ci.yml` with:**

```yaml
# Windows PR-CI on GitHub-hosted runners (#38 option 1): hosted windows-latest has
# MSVC v143 + .NET 8, and the shim's Maxine SDK config needs only headers + proxy
# sources (the proxies LoadLibrary the runtime later; CI never runs the DLL). Those
# files are NVIDIA-proprietary, so they live in the PRIVATE repo
# officialdad/maxine-winci-buildkit (assembled by scripts/assemble-winci-buildkit.sh),
# checked out via a read-only deploy key. Fork PRs have no secrets: SDK steps skip,
# stub coverage still runs. Still parked on #38: FRUC compile (Optical Flow SDK is
# developer-program-gated), runtime/GPU verification, installer releases (release.yml).
name: ci

on:
  pull_request:

# One run per ref; cancel superseded runs.
concurrency:
  group: ci-${{ github.ref }}
  cancel-in-progress: true

jobs:
  windows-compile:
    name: Windows compile (shim stub + SDK config + WinUI App)
    runs-on: windows-latest
    env:
      HAS_BUILDKIT: ${{ secrets.MAXINE_BUILDKIT_SSH_KEY != '' }}
    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Checkout Maxine build kit (private, headers + proxies only)
        if: env.HAS_BUILDKIT == 'true'
        uses: actions/checkout@v4
        with:
          repository: officialdad/maxine-winci-buildkit
          ssh-key: ${{ secrets.MAXINE_BUILDKIT_SSH_KEY }}
          path: .buildkit

      - name: Locate MSBuild
        uses: microsoft/setup-msbuild@v2

      - name: Build native shim (stub config, x64 Release)
        shell: pwsh
        run: |
          msbuild native/shim/shim.vcxproj /p:Configuration=Release /p:Platform=x64 /warnaserror /nologo
          if ($LASTEXITCODE -ne 0) { throw "shim stub build failed ($LASTEXITCODE)" }
          $dll = "native/shim/x64/Release/CameraOnScreen.Shim.dll"
          if (-not (Test-Path $dll)) { throw "shim DLL missing: $dll" }
          # No SDK props passed => this MUST be the passthrough stub.
          $strings = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($dll))
          if ($strings -notmatch 'not built in') { throw "expected the stub build without SDK dirs" }

      - name: Build native shim (Maxine SDK config, x64 Release)
        if: env.HAS_BUILDKIT == 'true'
        shell: pwsh
        run: |
          # /t:Rebuild: same output path as the stub build; SDK config must build LAST so
          # the App step deploys it (CLAUDE.md "deploy the right shim").
          msbuild native/shim/shim.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 `
            /p:CosVfxSdkDir=$env:GITHUB_WORKSPACE\.buildkit\vfx `
            /p:CosArSdkDir=$env:GITHUB_WORKSPACE\.buildkit\ar `
            /warnaserror /nologo
          if ($LASTEXITCODE -ne 0) { throw "shim SDK build failed ($LASTEXITCODE)" }

      - name: Export-verify deployed shim (SDK markers present, stub marker absent)
        if: env.HAS_BUILDKIT == 'true'
        shell: pwsh
        run: |
          $dll = "native/shim/x64/Release/CameraOnScreen.Shim.dll"
          $strings = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($dll))
          if ($strings -notmatch 'GreenScreen')     { throw "export-verify: GreenScreen missing (VFX not compiled in)" }
          if ($strings -notmatch 'GazeRedirection') { throw "export-verify: GazeRedirection missing (AR not compiled in)" }
          if ($strings -match 'not built in')       { throw "export-verify: stub marker present (stale stub deployed)" }

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

- [ ] **Step 2: Commit and open the PR (this IS the failing-test run)**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: build shim Maxine SDK config + export-verify on hosted Windows (#38 option 1)"
git push -u origin ci/windows-sdk-hosted
gh pr create --title "ci: Windows SDK-config compile + export-verify on hosted runners (#38 option 1)" \
  --body "$(cat <<'EOF'
Restores the parked Windows PR-CI coverage from #38 via option 1, on GitHub-hosted runners (no VM: windows-latest already has MSVC v143 + .NET 8).

- Private build kit `officialdad/maxine-winci-buildkit` (headers + proxy sources only, VFX 1.2.0.0 + AR 1.1.1.0; assembled by `scripts/assemble-winci-buildkit.sh`), checked out with a read-only deploy key.
- ci.yml Windows job now: stub build + stub assert -> SDK-config rebuild -> export-verify (GreenScreen + GazeRedirection present, "not built in" absent) -> App build (deploys the SDK shim) -> Core tests.
- Fork PRs (no secrets): SDK steps skip, stub coverage unchanged.
- Still parked on #38: FRUC compile, runtime/GPU verify, installer releases.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 3: Watch the run**

Run: `gh pr checks --watch`
Expected: `Windows compile (shim stub + SDK config + WinUI App)` green.

Known plausible failures + fixes (iterate on the branch, re-push):
- **NVIDIA proxy source trips `/warnaserror`** → in `shim.vcxproj`, on the proxy `<ClCompile>` items only (lines 123-136), add metadata `<TreatWarningAsError>false</TreatWarningAsError>` (third-party code, not ours). Do NOT relax `/warnaserror` globally.
- **`nvTransferD3D{,11}.h` decl mismatch vs the 1.2 proxy** (signatures were verified identical, but if MSVC disagrees) → copy the exact declarations the proxy defines into the kit's headers and re-push the kit repo; note it in the kit README.
- **Kit checkout auth failure** → deploy key vs secret mismatch; re-run Task 1 Step 4.

- [ ] **Step 4: Confirm the export-verify actually gates (one-time negative test)**

Temporarily empty the SDK props in the SDK-build step (`/p:CosVfxSdkDir= /p:CosArSdkDir=`), push, expect the export-verify step to FAIL with "GreenScreen missing". Revert the temporary commit (`git revert HEAD --no-edit`), push. This proves the gate can fail — the whole point of #38's stale-stub lesson.

---

### Task 3: Docs + issue bookkeeping + merge

**Files:**
- Modify: `CLAUDE.md` (CI/CD section)
- GitHub: comment on issue #38; merge the PR

**Interfaces:**
- Consumes: green PR from Task 2.

- [ ] **Step 1: Update CLAUDE.md CI/CD section**

In the `## CI/CD` section, replace the sentence `(#38: `ci.yml` is a hosted stub-compile job and `release.yml`'s installer job is `workflow_dispatch`-only until a Windows RTX runner exists)` with:

```
(#38: `ci.yml`'s hosted `windows-latest` job now builds BOTH shim configs — stub, then Maxine SDK config from the private `officialdad/maxine-winci-buildkit` header/proxy kit (read-only deploy key, `MAXINE_BUILDKIT_SSH_KEY` secret; assembled by `scripts/assemble-winci-buildkit.sh`) — and runs the export-verify gate, then App build + Core tests. Compile-only: FRUC stays stubbed there and `release.yml`'s installer job stays `workflow_dispatch`-only until a Windows RTX environment exists)
```

- [ ] **Step 2: Commit, push, merge**

```bash
git add CLAUDE.md
git commit -m "docs: CLAUDE.md CI/CD — hosted Windows SDK-config job (#38)"
git push
gh pr checks --watch   # confirm still green with the docs commit
gh pr merge --merge
```

- [ ] **Step 3: Comment on issue #38 (do not close)**

```bash
gh issue comment 38 --repo officialdad/camera-on-screen --body "$(cat <<'EOF'
Option 1 landed cheaper than estimated — no VM: hosted `windows-latest` already has MSVC v143 + .NET 8, and the SDK-config compile only needs headers + proxy sources. PR #<PR-NUMBER>:

- Private `officialdad/maxine-winci-buildkit` (VFX 1.2.0.0 + AR 1.1.1.0 headers/proxies; `scripts/assemble-winci-buildkit.sh`; read-only deploy key).
- ci.yml Windows job: stub build + assert → SDK-config rebuild → export-verify (GreenScreen + GazeRedirection present, `not built in` absent) → App build → Core tests.
- `nvVFXVideoSuperRes.h` is an authored compile-compat stand-in (real header died with the old box; NGC SDK zips are NVAIE-gated) — replace when a Windows SDK install exists.

Still parked here: FRUC compile on Windows CI (Optical Flow SDK gated + CUDA toolkit), runtime/GPU verification, installer releases (option 3, dual-boot on release day), #24/#25.
EOF
)"
```

(Replace `<PR-NUMBER>` with the merged PR's number.)

---

## Self-Review

- **Spec coverage:** option 1's scope = PR-CI compile jobs + export-verify → Tasks 1-2; issue bookkeeping → Task 3. Installer/runtime/GPU explicitly out of scope (issue recommends option 3 for releases). FRUC gap called out in constraints, workflow comment, and issue comment.
- **Placeholders:** none — full script, full workflow YAML, exact commands. `<PR-NUMBER>` is a runtime substitution instruction, not a TBD.
- **Consistency:** kit layout in script Step 1 ↔ `CosVfxSdkDir`/`CosArSdkDir` paths in ci.yml ↔ `shim.vcxproj:82-136` expectations; secret name `MAXINE_BUILDKIT_SSH_KEY` identical in Task 1 Step 4, ci.yml, CLAUDE.md.
