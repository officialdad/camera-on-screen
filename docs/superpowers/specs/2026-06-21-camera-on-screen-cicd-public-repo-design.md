# Camera-on-Screen — CI/CD + Public Repo Design

**Date:** 2026-06-21
**Status:** Approved (brainstorming)
**Topic:** Publish the project as a public GitHub repo under the `officialdad` org, add MIT + NVIDIA legal notices, untrack project-scoped skills, and automate the build/test gate via GitHub Actions on a self-hosted RTX runner.

## Goal

The project so far has been built and verified by hand on one RTX 3090 machine (see CLAUDE.md "Build & test"). This milestone makes the build **reproducible and gated in CI** and makes the repo **publishable** — correct open-source license, NVIDIA third-party/trademark notices, no third-party skill tooling leaking into the public tree.

This is the "automate the building" follow-on. It is **not** the M5 bundler/installer — release packaging stays deferred (see Non-goals).

## Context / constraints (load-bearing)

- **No GPU/SDK on GitHub-hosted runners.** Hosted Windows runners have no NVIDIA RTX GPU and no Maxine SDK, so they cannot build the Maxine-enabled shim against the real SDK nor run any GPU path. **Decision: CI runs on a self-hosted runner on the RTX 3090 machine** — full co-versioned build (VFX 0.7.6 + AR 0.8.7, shared TensorRT 10.4).
- **Repo ships zero NVIDIA code.** The NVIDIA proxy stubs (`nvVideoEffectsProxy.cpp`, `nvCVImageProxy.cpp`, `nvARProxy.cpp`) and headers are pulled at build time from the env-var SDK dirs (`COS_VFX_SDK_DIR`, `COS_AR_SDK_DIR`); none are tracked. So the public source repo redistributes no NVIDIA material → NVIDIA legal exposure is limited to a **notice + trademark** statement now. Full redistribution terms only apply once binaries are bundled (M5, deferred).
- **Pristine build rule.** CLAUDE.md: builds/tests must be 0 warnings. CI enforces this as a hard gate.
- **Deploy-the-right-shim gotcha.** The SDK build and the CI stub write the *same* DLL path; whichever built last wins. CI must build the SDK config and then **verify the deployed DLL** (`GreenScreen` + `GazeRedirection` exports present, `not built in` absent) so a stale stub can never pass green.

## Decisions

| Question | Decision |
|---|---|
| Repo | `officialdad/camera-on-screen`, **public**, push existing `main` history |
| CI host | **Self-hosted only**, label `rtx`, on the RTX 3090 machine (full Maxine) |
| Project license | **MIT**, © officialdad |
| NVIDIA legal | `THIRD-PARTY-NOTICES.md` — Maxine VFX+AR are NVIDIA, not bundled, user obtains, NVIDIA EULA governs, NVIDIA/Maxine/RTX trademarks, no endorsement |
| Project skills | **Untrack + relocate** `.agents/skills` + `skills-lock.json` to user scope (`~/.claude/skills`) |
| CI artifact | **None now** — deferred to M5 bundler/installer (artifact is worthless until the `maxine\` runtime sits beside the exe) |

## Components

### 1. Repo creation & remote
- `gh repo create officialdad/camera-on-screen --public --source . --remote origin --push` (after the skills-untrack commit, so the public history never contains the third-party skill tree's *future* changes; the existing history already contains them — acceptable, they are third-party skill docs, not secrets).
- Default branch `main`.

### 2. Skills untrack + relocate
- `git rm -r --cached .agents skills-lock.json` — removes from the repo, keeps working-tree copies.
- `.gitignore` gains `/.agents/` and `/skills-lock.json`.
- Relocate the 5 skill trees (`csharp-async`, `dotnet-best-practices`, `winui-app`, `winui-design`, `winui-dev-workflow`) into `~/.claude/skills/` so they load globally for the user, not per-project.
- **Tradeoff (accepted):** `~/.claude/skills` has no lock manager → loses `skills-lock.json` hash-pinned auto-update. Plain copies.

### 3. Licenses
- `LICENSE` — standard MIT text, `Copyright (c) 2026 officialdad`.
- `THIRD-PARTY-NOTICES.md`:
  - Declares the build-time dependency on **NVIDIA Maxine Video Effects SDK** and **NVIDIA Maxine AR SDK** (not included in this repo).
  - Users must download them from NVIDIA; use is governed by the **NVIDIA Software Developer License Agreement** and per-feature model licenses.
  - **Trademarks:** NVIDIA, Maxine, RTX, TensorRT, CUDA are trademarks of NVIDIA Corporation. This project is not affiliated with or endorsed by NVIDIA.
  - Forward note: a future binary distribution (M5 bundler/installer) will bundle the Maxine runtime + models and must then carry the full Maxine redistribution terms + bundled-model licenses (PDFs in each SDK's `license/` dir).

### 4. CI workflow — `.github/workflows/ci.yml`
- **Triggers:** `pull_request` + `push` to `main`.
- **Runner:** `runs-on: [self-hosted, windows, rtx]`.
- **Steps (one job, ordered):**
  1. `actions/checkout`.
  2. **Build native shim, SDK config** — invoke MSVC MSBuild on `native/shim/shim.vcxproj` `/p:Configuration=Release /p:Platform=x64`, with `COS_VFX_SDK_DIR` / `COS_AR_SDK_DIR` from the runner's persistent env. (PowerShell step — Bash mangles `/p:` per CLAUDE.md.)
  3. **Verify deployed shim** — `dumpbin /exports` (or `grep -a`) asserts `GreenScreen` **and** `GazeRedirection` present and `not built in` absent. Fail the job otherwise. Automates the CLAUDE.md deploy gotcha.
  4. **Build App** — `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild -c Release` (copies the shim).
  5. **Test Core** — `dotnet test tests/CameraOnScreen.Core.Tests/...`.
  6. **Warning gate** — pass `-warnaserror` (or `/p:TreatWarningsAsErrors=true`) on the dotnet steps; fail on any MSBuild/MSVC warning. Pristine rule.
- **No artifact upload** (see Non-goals).

### 5. Runner setup doc — `docs/ci/self-hosted-runner.md`
- How to register a self-hosted runner on the RTX 3090 machine (org or repo level), apply labels `self-hosted, windows, rtx`.
- Required persistent runner env: `COS_VFX_SDK_DIR`, `COS_AR_SDK_DIR`, `COS_VFX_RUNTIME_DIR` (and `COS_AR_RUNTIME_DIR` if overriding the ProgramFiles default).
- Note the runner needs: .NET 8 SDK, VS2022 Build Tools + MSVC v143, the co-versioned VFX 0.7.6 + AR 0.8.7 runtimes (CO-VERSION gotcha).
- **This is a manual human step** (needs the machine + a registration token); CI cannot self-provision it.

### 6. README rewrite
- Promote from CLAUDE.md essentials: what it is, RTX-only, the co-version constraint (one-liner + link), build steps, CI badge, license + NVIDIA notice pointers, current status (M1–M4 + M5 part 1 done; M5 bundler/installer next).
- Keep CLAUDE.md as the deep internal doc; README is the public face.

## Data / control flow

```
PR / push main
  -> self-hosted rtx runner
     -> MSBuild shim (SDK config, env SDK dirs)   [fail on MSVC warning]
     -> verify exports (GreenScreen + GazeRedirection present, "not built in" absent)
     -> dotnet build App -t:Rebuild -warnaserror   [copies shim]
     -> dotnet test Core
  -> all green => merge allowed
```

## Error handling / failure modes

- **Stale stub deployed** → export-verify step fails (the whole point of step 3).
- **Runner offline** → job queues; PRs can't go green until the machine is up. Accepted (self-hosted-only was the chosen tradeoff).
- **SDK env var missing on runner** → shim builds the stub → export-verify fails loudly (not a silent passthrough merge).
- **Warning introduced** → `-warnaserror` fails the build.

## Testing / verification

- CI itself is the test harness for the build. No new unit tests (this milestone is infra, not Core logic).
- Verification gate: open a trivial PR, confirm the workflow runs on the `rtx` runner, all steps green, and that a deliberately-introduced warning (scratch test, reverted) would fail. The export-verify step is exercised by the normal SDK build.
- Human gate: confirm the public repo renders (README, LICENSE detected as MIT by GitHub, notices present) and the `.agents`/skills tree is absent from the published file list.

## Non-goals (deferred to M5)

- **No release/packaging workflow.** No `release.yml`, no artifact upload, no installer. The shippable artifact requires the **M5 bundler** to place the co-versioned `maxine\` runtime + models beside the exe; until then an uploaded exe+shim is non-functional for end users.
- **No multi-GPU model handling** (compute 86 only today).
- **No full Maxine redistribution license work** — only the source-repo notice/trademark statement. Full EULA + model-license compliance lands when binaries are bundled.

## Open risks

- Org-level self-hosted runner registration may need admin rights the current account lacks (org runner list returned 403 for `admin:org`). Fallback: register the runner at **repo** level (repo admin is sufficient, which the creator has). The runner-setup doc covers both.
- Existing git history already contains the third-party skill tree. Untracking removes it going forward but not from history. Accepted — the skill docs are public third-party content, not secrets. (If undesired, a history rewrite is a separate, explicit task.)
