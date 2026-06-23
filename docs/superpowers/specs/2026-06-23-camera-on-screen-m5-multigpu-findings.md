# Camera-on-Screen M5 — multi-GPU model coverage: findings & decision (issue #2)

Date: 2026-06-23
Status: **Investigation complete. Not implementable on the current runtime — migration-gated.**

## Question

Issue #2 asks for one **universal installer** covering all RTX (Turing 75 / Ampere 86 /
Ada 89 / Blackwell 120), where today only Ampere (`_86`) engines ship. Two candidate paths:

- **Option A** — append `_75`/`_89`/`_120` model globs to the bundler manifest and ship all
  arches in one installer.
- **Option B** — ship a generic ONNX model and let TensorRT build the per-GPU engine on first
  launch (the issue's "investigate" item; would future-proof RTX 50/60 with no re-bundle).

## Method (all run on the RTX 3090 dev box, this session)

1. **SDK inventory.** Listed the model dirs of every local SDK tree (VFX 1.2.0.0 build src,
   VFX 0.7.6 runtime, AR 0.8.7 build src + installed runtime).
2. **NGC registry probe** — new `scripts/probe-ngc-maxine.ps1` (read-only). Personal NGC keys
   lack team-LIST scope (HTTP 403), but collection discovery (`maxine_vfx_sdk`,
   `maxine_ar_sdk`) + direct `/models/{name}/versions` queries work — exactly what
   `install_feature.ps1` uses. Enumerated every model + version + arch.
3. **Co-version load test** — built a known-good baseline bundle (shipped 0.7.6 GS + 0.8.7
   gaze, TRT 10.4) and `bundle_probe`, confirmed both effects AVAILABLE. Then, per NGC version,
   swapped **only** the version-matched engines (`AIGS_*` for GS; `gazeredir_*` for gaze,
   keeping the known-good TRT-10.4 face deps) into the bundle and re-ran the probe. A swapped
   engine that **loads** is TRT-10.4-compatible; one that fails `NvVFX_Load`/`NvAR_Load`
   ("no kernel image / GPU incompatible") is a newer TRT. Baseline reconfirmed AVAILABLE after
   each restore (rig validity).

## Findings

### Option B — generic ONNX — DEAD

No `.onnx` exists in **any** SDK tree (VFX 1.2.0.0 src, VFX 0.7.6 runtime, AR 0.8.7 src, AR
0.8.7 runtime). NVIDIA Maxine distributes **prebuilt per-arch TensorRT engines only**
(`*.engine.trtpkg`, an encrypted `NVnn` container — TRT version not readable off disk).
`nvonnxparser_10.dll` ships, but there is no ONNX to feed it. A first-run build path is not
possible with what NVIDIA provides.

### All-arch engines DO exist on NGC (corrects the prior assumption)

`nvidia/maxine` hosts Windows engine packages for **sm75, 86, 89, 100 (=120)** for both
effects **and** all gaze deps:

| Model | NGC versions (Windows) | Arches each |
|-------|------------------------|-------------|
| `nvvfxgreenscreen`     | 1.0.0.0, 1.1.0.0, 1.2.0.0 | 75, 86, 89, 100 |
| `nvargazeredirection`  | 1.0.0.0, 1.1.0.0, 1.1.1.0 | 75, 86, 89, 100 |
| `nvarfaceboxdetection` | 1.0.0.0, 1.1.0.0, 1.1.1.0 | 75, 86, 89, 100 |
| `nvarlandmarkdetection`| 1.0.0.0, 1.1.0.0, 1.1.1.0 | 75, 86, 89, 100 |
| `nvarfaceexpressions`  | 1.0.0.0, 1.1.0.0, 1.1.1.0 | 75, 86, 89, 100 |

So gaze/eye-contact **does** have an NGC fetch path (model `nvargazeredirection` + the named
deps). The `install_feature.ps1` version gate (refuses MAJOR.MINOR ≠ the SDK's) is
**client-side only** — the NGC API serves any version directly. (`-gpu 75/89/120` also needs no
physical card; arches map `120 → sm100` on Windows.)

### But NGC serves NO TRT-10.4 engines → the existing runtime cannot take them

The co-version load test is conclusive — **every** NGC version of **both** effects failed to
load on the shipped 0.7.6/0.8.7 (TRT 10.4) runtime:

```
GS   v1.0.0.0 -> UNAVAILABLE      gaze v1.0.0.0 -> UNAVAILABLE
GS   v1.1.0.0 -> UNAVAILABLE      gaze v1.1.0.0 -> UNAVAILABLE
GS   v1.2.0.0 -> UNAVAILABLE      gaze v1.1.1.0 -> UNAVAILABLE
(baseline shipped engines: both AVAILABLE, before and after each swap)
```

GS 1.2.0.0 (= VFX SDK 1.2.0.0 = TRT 10.9, the M4-regression pair) failing was the expected
negative control. The shipped `_86` engines are NOT byte-identical to any NGC package — they
are an **older build NGC has retired**. NGC's current line is a newer TRT than 10.4.

This is exactly why the runtime is pinned (M4 spec
`2026-06-21-camera-on-screen-m4-eyecontact-design.md`): two TRT/CUDA runtimes cannot coexist
in one process, so VFX was dragged **down** to 0.7.6 to match AR 0.8.7's TRT 10.4. NGC has
moved past TRT 10.4; the frozen runtime cannot consume the only available multi-arch engines.

## Decision

**Option A is feasible only via a full runtime migration; Option B is impossible. Neither
ships now.**

The only path to a universal installer:

1. **Re-base both effects onto a current, mutually co-versioned VFX + AR pair** (one shared
   TRT, almost certainly 10.9). VFX 1.2.0.0 (TRT 10.9) is on hand; **a current AR SDK at the
   same TRT is NOT** — only AR 0.8.7 (TRT 10.4) is available locally. Acquiring a TRT-10.9 AR
   SDK is the first hard blocker (without it the co-version war cannot even be re-fought).
   Note: `VideoFX\bin\NVVideoEffects.dll` is a 0.2 MB dispatch stub, not the real effect DLL —
   the VFX 1.2.0.0 green-screen runtime only materializes after `install_feature` pulls the
   feature lib, so even the VFX half needs assembly before it can be verified.
2. **Re-verify co-version on the 3090** (sm86) with the current pair — this is the M4
   regression re-fought; may resolve cleanly if both effects are genuinely TRT 10.9, or may
   re-open it.
3. **Fetch all-arch engines from NGC** (the matrix above) for the chosen version line; append
   `_75`/`_89`/`_100` globs to `maxine-manifest.psd1`; wire an NGC fetch into the bundler.
4. **Per-arch run verification** — sm75/89/120 engines only deserialize on real
   Turing/Ada/Blackwell silicon; the 3090 cannot test them. Ships best-effort until each card
   is available.

## ADDENDUM 3 2026-06-23 — IN-PROCESS PROOF: both effects load together on TRT 10.9 (sm86)

Built `bundle_probe` against **VFX 1.2.0.0 + AR 1.1.1.0** headers/proxies, assembled a flat
`maxine\` runtime (VFX wins on shared DLLs; AR-only DLLs added; both SDKs' per-feature DLLs +
all `_86` engines), ran it on the 3090 with `COS_*` unset:

```
VFX  Probe: AVAILABLE (GreenScreen available)
AR   Probe: AVAILABLE (GazeRedirection available)
EXIT=0
```

**The M4 co-version war is won in-process** — one process, single `NVCVImage.dll`, both
TensorRT-10.9 effects create + `*_Load` (build/deserialize their engines) with no
`cudaErrorNoKernelImageForDevice`. The central feasibility risk for issue #2 is eliminated on
sm86. Multi-arch is now a packaging exercise (fetch `_75/_89/_100` engines) + per-arch HW verify.

### Migration scope discovered (what shipping actually requires)

Both SDKs moved to a **dispatcher + per-feature-DLL** model (new vs the shipped 0.7.6/0.8.7):

- **VFX 1.2.0.0:** `bin\NVVideoEffects.dll` is a 0.24 MB dispatcher; the real green-screen
  effect is `features\nvvfxgreenscreen\bin\nvVFXGreenScreen.dll` (17.3 MB). The dispatcher
  finds the feature DLL via the normal search path → it must sit **beside** `NVVideoEffects.dll`
  in the flat `maxine\`. (Confirmed: GS loaded with it co-located.)
- **AR 1.1.1.0:** `nvARPose.dll` is the dispatcher; gaze needs FOUR per-feature DLLs co-located
  — `nvARGazeRedirection.dll`, `nvARFaceBoxDetection.dll`, `nvARLandmarkDetection.dll`,
  `nvARFaceExpressions.dll` (each ~0.5 MB, from each model's `1.1.1.0_lib_windows` NGC package).
  Without them `NvAR_Create("GazeRedirection")` fails even though `nvARPose.dll` loads.
- **One source delta:** AR 1.1.1.0 removed the `NvAR_Feature_*` convenience macros from
  `nvAR_defs.h` (now per-feature headers; `NvAR_FeatureID` is `const char*`). `eyecontact.cpp`
  uses `NvAR_Feature_GazeRedirection` (2 sites). Fix: include the gaze feature header, or define
  the literal `"GazeRedirection"`. (Probe used `-DNvAR_Feature_GazeRedirection="GazeRedirection"`.)
- **Engine filenames** (1.1.1.0 `_86`): gaze `gazeredir_{encoder,decoder}_fp16_86`; face
  `face_detection_86`, `faceland_*_86`, `face_expressions_fp16_mlp_86`; GS `AIGS_288x512_86_m*`.
- **`NVCVImage.dll` differs** between the two SDKs but a single copy (VFX's) serves both — proven
  by the EXIT=0 run.

### Remaining to ship a universal installer (now fully de-risked on sm86)

1. **Shim source:** `eyecontact.cpp` feature-ID macro (include `nvARGazeRedirection.h` or define
   the string). Verify no other AR 1.1.1.0 API deltas in the full shim build (only `bundle_probe`
   path compiled so far; aigs.cpp compiled clean).
2. **Bundler/manifest (`bundle-maxine.ps1` + `maxine-manifest.psd1`):** add VFX
   `nvVFXGreenScreen.dll` + the 4 AR feature DLLs; update engine globs to the 1.1.1.0 names;
   re-run `trace_closure.cpp` against the new runtime for the minimal DLL allow-list (the probe
   used a superset). Co-version physically: shared DLLs from VFX 1.2.0.0, AR-only from AR 1.1.1.0.
3. **Multi-arch:** fetch `_75/_89/_100` engines (same NGC models, `_models_windows_sm{75,89,100}`)
   for GS + the 4 AR features; append arch globs.
4. **Runtime env / CLAUDE.md:** update the co-version note to **VFX 1.2.0.0 + AR 1.1.1.0 / TRT
   10.9** (was 0.7.6/0.8.7 / TRT 10.4); update `COS_VFX_RUNTIME_DIR` doc.
5. **Per-arch HW verify:** sm75/89/120 engines only deserialize on real Turing/Ada/Blackwell;
   3090 verifies sm86 only — ships best-effort per arch.

Tooling: `scripts/probe-ngc-maxine.ps1` (registry probe). NGC download mechanics now known
(resources 302→ `xfiles` presigned; models return `urls[]` directly).

## ADDENDUM 2 2026-06-23 — co-version CONFIRMED at the runtime level (AR 1.1.1.0 ⇄ VFX 1.2.0.0)

Downloaded **AR SDK 1.1.1.0 Windows** from NGC (`resources/ar_sdk_core/1.1.1.0_windows`,
`ARSDK_windows_1.1.1.0.zip`, sha256 verified) and byte-compared its bundled TRT/CUDA runtime
DLLs against the on-hand VFX 1.2.0.0 `bin\`:

| shared DLL | body comparison | verdict |
|---|---|---|
| `nvinfer_10.dll` (440,983,664 B) | 106×4 MB blocks; **only block 0 (PE header: checksum/timestamp) + block 105 (Authenticode cert) differ** — blocks 1–104 (~416 MB code/data) byte-identical | **same TensorRT 10.9 build, re-signed** |
| `cudart64_12.dll` (584,304 B) | only first + last 64 KB block differ | **same CUDA runtime, re-signed** |
| `NVCVImage.dll` (2,487,408 B) | most blocks differ | genuinely different (Maxine CV helper, *not* TRT/CUDA) — bundler already ships a single copy; the in-process `bundle_probe` is the gate |

The two DLLs whose mismatch produced the M4 `cudaErrorNoKernelImageForDevice` break
(`nvinfer_10.dll` + `cudart64_12.dll`) are **byte-identical in body**. So **AR 1.1.1.0 + VFX
1.2.0.0 are the co-versioned TRT-10.9 pair** that was missing — the runtime-level half of the
M4 co-version war is won by construction. Remaining proof is the *in-process* load (both effects
+ a single `NVCVImage.dll` in one process), which only `bundle_probe` on real silicon settles.

Next: assemble VFX 1.2.0.0 GS effect lib (`install_feature -features nvvfxgreenscreen`), build a
TRT-10.9 `_86` baseline bundle (VFX GS + AR 1.1.1.0 gaze), run `bundle_probe` on the 3090. Both
load → migration fully unblocked; then append NGC `_75/_89/_100` globs + bundler NGC fetch.

## ADDENDUM 2026-06-23 — the #1 blocker likely just lifted (AR SDK 1.1.1.0 shipped)

The hard blocker above ("no current AR SDK at TRT 10.9 exists; only AR 0.8.7 / TRT 10.4 on
hand") is **probably stale**. As of **2026-06-02** NVIDIA shipped **AR SDK v1.1.1.0** under a
new repo (`github.com/NVIDIA-Maxine/AR-SDK-Samples`) — a jump from the 0.8.7 line. This is the
exact version line NGC already serves all-arch gaze engines for (`nvargazeredirection` v1.1.1.0,
sm75/86/89/100), and the co-version load test in this doc **already proved those v1.1.1.0
engines fail on the shipped TRT-10.4 runtime** → they are a newer TRT. VFX 1.2.0.0 (on hand) is
TRT 10.9. Both SDKs current in 2026 ⇒ **strong likelihood AR 1.1.1.0 + VFX 1.2.0.0 are the
co-versioned TRT-10.9 pair that was missing.**

**NOT yet verified** (NVIDIA does not publish the bundled TRT number; only the installed SDK's
`nvinfer_10.dll` file-version shows it). Verification needs the SDK in hand. The migration is
no longer "acquire a thing that may not exist" — it's "download AR 1.1.1.0 and run the test."

Updated next action (replaces the old blocker as the gate):
1. Download **AR SDK 1.1.1.0** (new `AR-SDK-Samples` installer / NGC).
2. Assemble VFX 1.2.0.0 GS feature lib: `install_feature.ps1 -features nvvfxgreenscreen`.
3. Compare `nvinfer_10.dll` file-version in both runtimes. **Match → co-version holds; proceed.
   Mismatch → still TRT-split, migration dead again.**
4. Build a TRT-10.9 `_86` baseline bundle (VFX 1.2.0.0 GS + AR 1.1.1.0 gaze), run `bundle_probe`
   on the 3090 = the M4 co-version war re-fought. Both load → unblocked.
5. Append NGC `_75/_89/_100` globs, wire bundler NGC fetch, ship best-effort (non-Ampere arches
   final-verify only on real Turing/Ada/Blackwell silicon).

## Unblock checklist (for whoever picks this up)

- [x] ~~Obtain a current NVIDIA Maxine AR SDK/runtime at TRT 10.9~~ — **AR SDK 1.1.1.0 shipped
      2026-06-02** (`NVIDIA-Maxine/AR-SDK-Samples`); likely TRT 10.9. Download + confirm
      `nvinfer_10.dll` matches VFX 1.2.0.0 (step 3 above).
- [ ] Assemble the full VFX 1.2.0.0 green-screen runtime (`install_feature.ps1 -features
      nvvfxgreenscreen`) — the bin stub alone is not loadable.
- [ ] Re-fight + verify the VFX-1.2.0.0 + current-AR co-version on the 3090 (both effects load
      together, sm86).
- [ ] Append `_75`/`_89`/`_100` model globs; teach the bundler to fetch from NGC
      (`nvvfxgreenscreen` + `nvargazeredirection` + `nvarfaceboxdetection` +
      `nvarlandmarkdetection` + `nvarfaceexpressions`).
- [ ] Verify each non-Ampere arch on real hardware (Turing/Ada/Blackwell).

## Tooling produced

- `scripts/probe-ngc-maxine.ps1` — read-only NGC `nvidia/maxine` registry probe (auth mirrors
  `install_feature.ps1`; lists version×arch per model). Reusable for the migration.

## Out of scope / unchanged

No shipped runtime, bundler, manifest, or C-ABI change this session — investigation only. The
Ampere-only installer remains the shippable build; non-RTX / non-Ampere already runs as a plain
overlay with effects gated off (by design, no crash).
