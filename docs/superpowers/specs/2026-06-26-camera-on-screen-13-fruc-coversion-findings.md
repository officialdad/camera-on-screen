# Issue #13 — FRUC fps interpolation: co-version HARD-GATE findings

**Date:** 2026-06-26 · **Branch:** `spike/13-fruc-coversion-smoke` · **Verdict: FEASIBLE — gate PASSED.**

## Question

Can NVIDIA Optical Flow **FRUC** (`NvOFFRUC.dll`, frame-rate up-conversion 30→60) run in the
**same process** as the app's pinned Maxine **VFX 1.2.0.0 + AR 1.1.1.0** runtime
(CUDA 12.x / TensorRT 10.9) without the CO-VERSION conflict CLAUDE.md warns about
(two same-named CUDA/TRT runtimes → first `LoadLibrary` wins → loser fails with
`cudaErrorNoKernelImageForDevice`)?

## Answer: yes. The documented hazard does not apply to FRUC.

The hazard requires a **shared DLL name** to fight over. FRUC has none with VFX/AR:

| | VFX 1.2.0.0 + AR 1.1.1.0 | FRUC (OF SDK 5.0.7) |
|---|---|---|
| CUDA runtime | `cudart64_12.dll` (CUDA 12.x) | `cudart64_110.dll` (CUDA 11.0) — **distinct name** |
| TensorRT | `nvinfer_10.dll` (TRT 10.9) | **none — FRUC uses no TensorRT** |
| Imaging | `NVCVImage.dll` | none |
| Engine | TRT deserialized `.trtpkg` | NVOFA hardware optical-flow engine + embedded CUDA fatbin |
| Driver | `nvcuda.dll` (shared, backward-compatible — no conflict) | same |

FRUC's full dependency closure (`dumpbin /dependents NvOFFRUC.dll`): `cudart64_110.dll`,
`nvcuda.dll`, `KERNEL32`, `MSVCP140`, `VCRUNTIME140`, CRT. No TRT, no NVCV, no OpenCV dep.
So FRUC and the Maxine stack share only `nvcuda.dll` (the driver), which is single-instance and
version-agnostic by design.

## Empirical proof — `native/shim/smoke/of_fruc_smoke.cpp`

Standalone exe: bring up Aigs (green screen) + EyeContact (gaze) + SuperRes — loading the full
CUDA-12 / TRT-10.9 stack — **then** `LoadLibrary` FRUC and run `NvOFFRUCCreate` →
`RegisterResource` → `NvOFFRUCProcess`. Run on the RTX 3090 (sm86) against the co-versioned
`maxine-stage` (app-relative tier, so `models\ar` resolves):

```
# Phase1 VFX+AR resident: greenScreen=1 eyeContact=1 superRes=1
#### GATE: NvOFFRUCCreate -> 0 (SUCCESS -- co-version OK)
#   Process frame 0 -> 0 (repeat=0)
#   Process frame 1 -> 0 (repeat=1)        # repeat is expected: constant synthetic frames, no motion
# co-resident runtime modules:
    ...\maxine\nvVFXGreenScreen.dll
    ...\maxine\nvinfer_10.dll
    ...\maxine\nvARGazeRedirection.dll
    ...\maxine\cudart64_12.dll             # <-- CUDA 12 (VFX/AR)
    ...\NvOFFRUC.dll
    ...\bin\win64\cudart64_110.dll         # <-- CUDA 11 (FRUC), co-resident, no clash
#### VERDICT: FRUC co-version GATE PASSED. Process produced output too.
```

`cudart64_12.dll` **and** `cudart64_110.dll` co-resident in one process, full Maxine TRT stack +
FRUC NVOFA both functional, no `cudaErrorNoKernelImageForDevice`. Exit 0.

### Reproduce
```powershell
# build (VS2022 x64). Env defaults baked in build_of_fruc_smoke.bat (runner _sdk paths):
cmd /c native\shim\smoke\build_of_fruc_smoke.bat native\shim\smoke\of_fruc_smoke.exe
# run: app-relative maxine tier so models\ar resolves (COS_*_RUNTIME_DIR unset)
cmd /c mklink /J native\shim\smoke\maxine C:\actions-runner\_sdk\maxine-stage
$env:PATH="<OF_SDK>\NvOFFRUC\NvOFFRUCSample\bin\win64;$env:PATH"
native\shim\smoke\of_fruc_smoke.exe "<OF_SDK>\NvOFFRUC\NvOFFRUCSample\bin\win64\NvOFFRUC.dll"
```
Note: OF SDK is a dev-portal (login-gated) download, **not** on NGC. The NGC `nvidia/multimedia/nvof`
model is an unrelated TensorRT optical-flow CNN — would *re-add* TRT; do not use it.

## Costs / risks (unchanged from the issue, now design-worthy)

- **Latency +1 frame (~33 ms).** Interpolating N+0.5 needs N+1 → hold one frame back.
- **Motion artifacts.** OF interpolation can smear fast hand/mouth motion on a talking head.
- **CUDA-11 runtime to bundle.** Adds `NvOFFRUC.dll` + `cudart64_110.dll` to stage/manifest/closure.
  (Two CUDA runtimes resident is fine — proven above — but bundle size grows ~1.2 MB.)
- **Doubled downstream rate.** Frame pump ~33 ms → ~16 ms; doubles present + active-effect cost.
- **API shape:** CUDA mode, `pDevice=NULL` (FRUC uses the current ctx), ARGB or NV12, min 3
  registered `CUdeviceptr` buffers (1 interpolate + 2 render), `pFrame = &CUdeviceptr`, pitch = `w*4`.

## Post-gate integration sketch (NOT yet built)

- Run on the capture worker thread **after** the effect chain (interpolate the final composite).
- Hold frame N; when N+1 arrives, emit N then synthesized N+0.5.
- New `CosParams` enable flag + `CosCaps` availability gate (same pattern as the other effects).
- Bundle: add FRUC's closure to `maxine-manifest.psd1`; re-run `trace_closure`/`bundle_probe`.
- Real-fps readout (issue #14 counter) reflects the doubled rate.

**Recommendation:** unblock #13. The gate that could have killed it is cleared. Next step is a
full implementation plan for the worker-thread integration + bundling (separate, larger effort).
