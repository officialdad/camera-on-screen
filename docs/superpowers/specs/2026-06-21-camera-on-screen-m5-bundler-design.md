# Camera-on-Screen M5 (bundler) — minimal-closure Maxine runtime bundler

Date: 2026-06-21
Status: Design approved; implementation plan next.

## Purpose

M5 part 1 (app-relative discovery, merged `055e932`) taught the shim to find the Maxine
runtimes at `<app>\maxine\` and defined that layout. But today that folder is **hand-copied**
(a verbatim 2.1 GB copy of two SDK trees). This spec builds the **bundler**: the publish-time
step that *produces* `<app>\maxine\` automatically, copying only the **minimal verified load
closure** of the two effects we actually use — green screen (VFX) and eye contact / gaze
redirection (AR) — co-versioned on shared TensorRT 10.4 / CUDA 12.1.

This is the foundation for the rest of M5: the installer packages this output, the
multi-GPU work appends model files to it, and the license notices ride inside it.

## Decisions (from brainstorming)

- **Minimal closure pruning.** Ship only the DLLs the two effects actually load at *run*
  time and only the models for *those* effects. Target **~1.1–1.4 GB** on disk (down from
  the 2.1 GB verbatim copy), ~700 MB compressed download.
- **Ampere (`_86`) only for the first shippable build** — the only arch testable on the
  RTX 3090. Layout is **forward-compatible** to a single universal all-arch installer (Maxine
  loads models from one dir and selects by the `_86`/`_89`/… filename suffix, so adding an
  arch later = appending files to the same `models\vfx` / `models\ar` dirs, no restructure).
- **End-state = one universal installer**, not per-arch installers. (Runtime DLLs are
  arch-independent and shared; only the ~100 MB pruned model set is per-arch, so bundling all
  arches costs ~300 MB over one — not worth a GPU-arch download choice + N artifacts.) This
  spec only ships Ampere; the universal layout is the constraint it must not violate.
- **Bundler = a PowerShell script**, an explicit publish-time step — NOT wired into
  `dotnet build` (no 2 GB copy per incremental build). Matches the repo ethos where the shim
  is built as its own separate step outside the sln.
- **DLL allow-list = produced by a runtime trace tool, recorded in a checked-in manifest.**
  Reproducible and self-documenting; re-runnable when the SDK versions bump.
- **License notices ride along** (`NVIDIA Maxine EULA.pdf` + `ThirdPartyLicenses.txt` copied
  into the bundle). This is partial compliance only; the full EULA review remains its own
  deferred M5 item.

## Co-version invariant (hard constraint, do not break)

Two Maxine SDKs with mismatched CUDA/TensorRT cannot coexist in one process (the M4
"no kernel image" failure). The verified pair is **VFX 0.7.6 + AR 0.8.7**, both on
**TensorRT 10.4.0.26 / CUDA 12.1** — their shared runtime DLLs (`nvinfer_10.dll`,
`cudart64_12.dll`, `cublas*`, `NVCVImage.dll`, `npp*`, `nvonnxparser_10.dll`,
`nvrtc*`) are **byte-identical** between the two SDKs. The single shared `maxine\` root holds
**one** copy of each shared DLL; first-`LoadLibrary`-wins is therefore correct.

**The bundler enforces this physically:** every shared runtime DLL is copied from **one**
source SDK only (the VFX 0.7.6 runtime). Only the per-effect DLLs come from their own SDK —
`NVVideoEffects.dll` from VFX, `nvARPose.dll` from AR — and the per-effect model sets from
their own SDK. A shared DLL that is present in AR but absent in VFX 0.7.6 (e.g. `cufft*`,
`nvvpi2.dll`) is sourced from AR **only if the closure trace proves an effect loads it**;
otherwise it is dropped (these are the prime prune targets — `cufft` ~470 MB, `nvvpi2` 57 MB,
none shipped by VFX 0.7.6).

## Sources (where the bundler copies from)

- **VFX 0.7.6 runtime** — flat layout (DLLs in root, models in `models\`). Dev path today:
  `…\VideoFX-0.7.6` (the AR-matched runtime; see CLAUDE.md CO-VERSION gotcha). Provides the
  shared runtime DLLs, `NVVideoEffects.dll`, and the green-screen models.
- **AR 0.8.7 runtime** — DLLs in root, models in `models\`. Dev path:
  `%ProgramFiles%\NVIDIA Corporation\NVIDIA AR SDK` (or `COS_AR_RUNTIME_DIR`). Provides
  `nvARPose.dll`, the gaze + face models, and any AR-only DLL the trace proves is needed.

The bundler takes source dirs as parameters, defaulting to the existing `COS_*` env vars so
the dev machine and the CI runner need no new configuration.

## Target layout (from M5 part 1 — unchanged)

```
<output dir>\                  (CameraOnScreen.App.exe + CameraOnScreen.Shim.dll)
  maxine\
    nvinfer_10.dll             \  shared TRT/CUDA runtime — ONE copy, sourced from VFX 0.7.6,
    cudart64_12.dll            |  byte-identical to AR 0.8.7 (the co-version invariant)
    cublasLt64_12.dll          |
    NVCVImage.dll              |
    npp*.dll, nvrtc*, …        /  (only those the closure trace proves are loaded)
    NVVideoEffects.dll            (VFX green-screen effect DLL  ← VFX source)
    nvARPose.dll                  (AR gaze effect DLL           ← AR source)
    NVIDIA Maxine EULA.pdf        (license ride-along)
    ThirdPartyLicenses.txt        (license ride-along)
    models\
      vfx\   AIGS_288x512_86_*.engine.trtpkg        (green-screen, Ampere)
      ar\    gazeredir_*, face_detection_*, faceland_*, …  (gaze + its deps, Ampere)
```

## Components

### 1. Closure tracer — `native/shim/smoke/trace_closure.cpp`

Extends the existing `bundle_probe.cpp` pattern. Drives the **full shim C ABI** (not just
`Probe`), so every DLL **and** model the runtime touches at *run* time is loaded —
`Probe`/`NvAR_Load` alone can miss the gaze feature's face-detection / landmark sub-models,
which load when a frame is actually processed:

1. `cos_init` → `cos_set_params` with **both** effects enabled → `cos_start`.
2. Pump several synthetic frames through `cos_get_frame` (forces engine build/load + the gaze
   face pipeline + the green-screen matte path).
3. `cos_stop` / `cos_shutdown`.
4. `EnumProcessModulesEx` + `GetModuleFileNameW` over the process → print every loaded module
   whose path is under the `maxine\` dir, as relative paths.

Run **once** (dev or CI, on the RTX box) against the current full 2.1 GB staged `maxine\`.
Its output is the authoritative **DLL allow-list** that seeds the manifest. It is a dev/CI
tool, not shipped; it builds like the other smoke binaries (a `.bat`/script next to it).

> **Models are not LoadLibrary'd** (the SDK `fopen`s them), so module enumeration does not
> list them. The model allow-list is curated by feature (below) and **verified by the gate
> in §4** — a missing required model makes `NvVFX_Load`/`NvAR_Load` fail, which the gate
> catches. No filesystem trace tool is needed.

### 2. Manifest — `native/shim/bundle/maxine-manifest.psd1`

A checked-in PowerShell data file (hashtable) the bundler reads. Sections:

- **`SharedDlls`** — the traced shared-runtime DLL list, all sourced from VFX 0.7.6.
- **`VfxDll` / `ArDll`** — `NVVideoEffects.dll` (VFX) / `nvARPose.dll` (AR).
- **`VfxModels`** — glob(s) for the green-screen Ampere models (`AIGS_288x512_86_*`).
- **`ArModels`** — explicit list/globs for gaze + its deps
  (`gazeredir_*_86*`, `face_detection_86*`, `faceland_*_86*`, `face_expressions_*_86*`).
  **Excludes** bodypose / peoplenet / fullupperbody (unused by gaze; the bulk of AR's 323 MB).
- **`License`** — `NVIDIA Maxine EULA.pdf`, `ThirdPartyLicenses.txt` (from each source).

The model entries are **arch-tagged in their globs** (`_86`); the universal-installer step
later appends `_75` / `_89` / `_120` globs to the same sections — no layout change.

The manifest is the single source of truth for "what's in the bundle," version-pinned to
VFX 0.7.6 + AR 0.8.7. A header comment records how `SharedDlls` was produced (the trace tool)
and the verified SDK versions.

### 3. Bundler — `scripts/bundle-maxine.ps1`

```
bundle-maxine.ps1
  -OutDir <publish dir>                 # required; <OutDir>\maxine\ is produced
  -VfxRuntime <dir>   (default $env:COS_VFX_RUNTIME_DIR)
  -ArRuntime  <dir>   (default $env:COS_AR_RUNTIME_DIR or %ProgramFiles%\NVIDIA…\NVIDIA AR SDK)
  -ManifestPath <file> (default native/shim/bundle/maxine-manifest.psd1)
```

Behavior: validate sources exist; create `<OutDir>\maxine\{,models\vfx,models\ar}`; copy each
manifest entry from its designated source to its destination; **fail loudly** if any
manifest-listed file is missing from its source (a stale/wrong SDK); report the produced total
size and a per-section breakdown. Idempotent (safe to re-run; overwrites). Does **not** touch
the App build or the dev env-var run flow.

### 4. Verification gate

After bundling, **build + run the tracer (or `bundle_probe`) against the PRODUCED pruned
`maxine\`** with **all `COS_*` env vars unset**, CWD elsewhere. Pass criteria: both effects
report AVAILABLE and a frame round-trips with both effects on (exit 0). This catches an
over-aggressive prune (a dropped DLL or model). It is the real correctness gate for the
manifest and is suitable as a CI step on the self-hosted RTX runner. The bundle is invalid if
this fails.

### 5. License ride-along

The bundler copies `NVIDIA Maxine EULA.pdf` + `ThirdPartyLicenses.txt` (from each SDK source)
into `maxine\`. This ensures the legally-required notices travel with the redistributed
runtime. **It is not a substitute for the full license review** (a separate M5 item) — it is
the minimum so the bundle is never shipped without its notices.

## Testing & verification

- **No Core unit tests** — this is native tooling + a PowerShell copy step, no managed surface
  (mirrors M5 part 1 and M4, which added none for the native runtime path). Existing Core
  tests and C-ABI parity are **untouched and must stay green**; there is **no C-ABI change**.
- **Native build stays pristine (0 warnings)** in both configs (SDK + CI stub). `trace_closure`
  is a smoke binary, not part of the shipped shim.
- **The §4 gate is the functional verification** — automatable on the RTX runner.
- **Human gate:** publish the App, run `bundle-maxine.ps1`, launch the app from the publish
  dir with `COS_*` unset, confirm green screen + eye contact both work alone and together on
  the RTX 3090, and record the produced bundle size. Log in `docs/superpowers/verification/`.

## Size accounting (estimate to validate against)

| Item | Verbatim | After prune (expected) |
|------|----------|------------------------|
| Shared runtime DLLs | ~1.5 GB | ~0.9–1.0 GB (drop `cufft` ~470 MB, `nvvpi2` 57 MB, any unused `npp` — pending trace) |
| `NVVideoEffects` + `nvARPose` | included | included |
| `models\vfx` (Ampere) | 124 MB | 124 MB |
| `models\ar` (Ampere) | 323 MB | ~100 MB (gaze + face deps only) |
| **Total** | **~2.1 GB** | **~1.1–1.4 GB** |

The trace + gate replace these estimates with measured truth; the table is the target to
sanity-check against, not a contract.

## Out of scope (own specs, deferred)

1. **Installer** — package the published app (incl. `maxine\`) for distribution; tech TBD.
2. **Multi-GPU models** — append `_75` / `_89` / `_120` model variants to the manifest for the
   universal installer (or option B: generic + first-run TensorRT build — note VFX 0.7.6 +
   AR 0.8.7 both ship `nvonnxparser_10.dll`, so a generic path may be feasible; to investigate
   in that spec).
3. **Full license review** — read + comply with the Maxine EULA + bundled-model licenses
   before public release (this spec only carries the notice files).
4. **A tag-triggered `release.yml`** consuming the bundler output (deferred to the installer).
