# Camera-on-Screen M5 (part 1) — App-relative SDK discovery

Date: 2026-06-21
Status: Design approved; implementation plan next.

## Purpose

Today the shim locates the Maxine VFX (green screen) and AR (eye contact) runtimes via
**environment variables** (`COS_VFX_RUNTIME_DIR` / `COS_VFX_SDK_DIR` for VFX,
`COS_AR_RUNTIME_DIR` with a `%ProgramFiles%` fallback for AR). That is a developer-only
setup — a shipped end user has no env vars and no system SDK install.

This spec adds a third resolution tier to each resolver: **a runtime bundled beside the
app** (`<app>\maxine\`). With the bundle present and no env vars set, the shim finds the
co-versioned VFX + AR runtimes self-contained next to the exe. Env vars stay as a
**developer override** so the current dev run-from-SDK-tree flow is unchanged.

This is **part 1 of M5**, intentionally the smallest piece: **resolver logic only**. It
unblocks the rest of M5 (it defines the bundled layout the bundler/installer must produce)
while touching no other subsystem.

## Scope (YAGNI)

- **Resolver logic only.** Two functions change: `ResolveSdkPaths` (aigs.cpp) and
  `ResolveArPaths` (eyecontact.cpp), plus one small shared path helper.
- **Env vars remain as dev override**, highest precedence, behavior byte-identical to today.
  Zero regression for the RTX 3090 dev machine.
- **Single shared `maxine\` bundle layout** (see below) — the co-version principle made
  physical: one folder, one TRT/CUDA runtime, both effect DLLs, per-effect model subdirs.
- **NOT in this spec (deferred to later M5 parts):** copying the real DLLs/models into the
  publish output (the bundler), the installer, multi-GPU model variants, license
  compliance. This spec is verified by **hand-copying** a known-good co-versioned runtime
  into `<app>\maxine\` and confirming both effects load with no env vars set.

## Bundled layout — single shared `maxine\` root

```
<app dir>\                       (holds CameraOnScreen.App.exe + CameraOnScreen.Shim.dll)
  maxine\
    nvinfer_10.dll               \
    cudart64_12.dll              |  shared TRT/CUDA runtime — byte-identical between
    NVCVImage.dll                |  VFX 0.7.6 and AR 0.8.7 (the co-version invariant)
    ...                          /
    NVVideoEffects.dll           (VFX green-screen effect DLL)
    nvARPose.dll                 (AR gaze effect DLL)
    models\
      vfx\                       (green-screen .trtpkg / .engine models)
      ar\                        (gaze-redirection models)
```

Both proxies `SetDllDirectory(<app>\maxine)` — the **same** root. Because the shared
TRT/CUDA DLLs are byte-identical across the two co-versioned SDKs, first-`LoadLibrary`-wins
is correct: whichever effect loads first pulls `nvinfer_10.dll` / `cudart64_12.dll` /
`NVCVImage.dll` from `maxine\`, and the second effect's `LoadLibrary` reuses the
already-loaded same-named modules. This is exactly the co-version fix from M4, now expressed
as one on-disk runtime instead of two env-pointed trees. Model dirs differ per effect
(`models\vfx` vs `models\ar`) and are passed explicitly via `NVVFX_MODEL_DIRECTORY` /
`NvAR_Parameter_Config(ModelDir)`, so no model-file collision.

## Finding the app dir — from the shim DLL, not the CWD

The shim is a DLL that the App copies next to its exe, so the shim DLL's own directory **is**
the app dir — and it is correct regardless of the process working directory (Explorer
double-click, `cd` elsewhere, etc.). Resolve it from the shim's own module handle:

```cpp
// paths.{h,cpp} (new, native/shim) — shared by aigs.cpp and eyecontact.cpp.
// Returns the directory containing the shim DLL (UTF-8, no trailing slash), or "" on failure.
std::string ShimModuleDir();
```

Implementation: `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&ShimModuleDir, &hmod)` →
`GetModuleFileNameW(hmod, …)` → strip the file name → UTF-8 (`WideCharToMultiByte`,
CP_UTF8) to match the existing `std::string` path plumbing. Using the address of
`ShimModuleDir` itself as the lookup address guarantees the shim module (not the host exe).

A second small helper avoids activating the bundled tier when the folder is absent:

```cpp
bool DirExists(const std::string& path);  // GetFileAttributesW + FILE_ATTRIBUTE_DIRECTORY
```

These live in `native/shim/paths.{h,cpp}` (new TU added to `shim.vcxproj`) rather than being
duplicated in both effect files. Pure Win32 + std::string, no SDK dependency, so they
compile in both the SDK and CI-stub configurations.

## Resolver changes

### VFX — `ResolveSdkPaths` (aigs.cpp)

New precedence (first hit wins):

1. `COS_VFX_RUNTIME_DIR` — flat runtime (DLLs in root, models in `root\models`). *Dev
   override, unchanged.*
2. `COS_VFX_SDK_DIR` — legacy full-SDK tree (`root\bin`, `root\bin\models`). *Dev fallback,
   unchanged.*
3. **NEW:** `<ShimModuleDir()>\maxine` and `DirExists` → `binDir = <…>\maxine`,
   `modelDir = <…>\maxine\models\vfx`.
4. None → error `"VFX runtime not found: set COS_VFX_RUNTIME_DIR or bundle maxine\\ beside the app"`.

### AR — `ResolveArPaths` (eyecontact.cpp)

New precedence (first hit wins):

1. `COS_AR_RUNTIME_DIR` — *dev override, unchanged.*
2. **NEW:** `<ShimModuleDir()>\maxine` and `DirExists` → `runtimeDir = <…>\maxine`,
   `modelDir = <…>\maxine\models\ar`.
3. `%ProgramFiles%\NVIDIA Corporation\NVIDIA AR SDK` — *dev last-resort fallback, kept.*
   (`modelDir = root\models`, as today.)

**Ordering decision:** the bundled `maxine\` tier is placed **above** the `%ProgramFiles%`
install. A shipped app must be self-contained even on a machine that happens to have the AR
SDK installed — preferring a system install would risk loading a **non-co-versioned** AR
(different TRT) and reintroducing the M4 "no kernel image" failure. ProgramFiles stays only
as the dev last resort when neither env var nor bundle is present.

## Proxy pointing — unchanged mechanism, shared root

`PointProxiesAt` (VFX) and `PointProxyAt` (AR) are unchanged in mechanism — each stows the
resolved dir in a function-static `std::string` and assigns it to its proxy global
(`g_nvVFXSDKPath` / `g_nvARSDKPath`), which the proxy `SetDllDirectory`'s before
`LoadLibrary`. In the bundled case both resolve to the **same** `<app>\maxine` string; that
is intentional and correct (see layout rationale). No change to the proxy stubs themselves.

## Error / fallback behavior

- If no tier resolves, the resolver returns `false` with a human-readable `detail`, which
  flows up through the existing `Probe()` → `cos_query_capabilities` path. The effect's
  capability gate goes **off**, the panel toggle greys out, and the note shows the reason —
  exactly today's behavior when an env var is unset. No crash, passthrough preserved.
- The app-relative tier only activates when `maxine\` **exists** (`DirExists`); a missing
  bundle falls through to the error rather than handing the SDK a bogus path.

## Testing & verification

- **No new Core unit tests** — this is native path logic with no managed surface (mirrors
  M4, which added none for the resolvers). The C-ABI is unchanged, so `CosStatus` /
  `CosParams` / `CosCaps` parity and existing Core tests are untouched and must stay green.
- **Native build** must stay pristine (0 warnings) in both configs: SDK
  (`COS_HAS_MAXINE` + `COS_HAS_MAXINE_AR`) and the CI stub. `paths.cpp` compiles in both.
- **Human visual gate (the real verification):** hand-copy a known-good co-versioned runtime
  (VFX 0.7.6 + AR 0.8.7, shared TRT 10.4) into `<app>\maxine\` with the layout above,
  **unset all `COS_*` env vars**, launch the app, and confirm: both capability probes pass,
  both toggles enable, green screen + eye contact work alone and together on the RTX 3090.
  This proves the bundled path end-to-end without any env var. Record in
  `docs/superpowers/verification/`.

## Deferred (rest of M5)

Each gets its own spec → plan → implementation cycle:

1. **Bundler** — make the App publish step actually copy the co-versioned DLLs + models into
   `<output>\maxine\` with this layout (today they live in the SDK trees the env vars point
   at). This spec defines the target layout; the bundler produces it.
2. **Installer** — package the published app (incl. `maxine\`) for distribution (tech TBD:
   WiX / MSIX / Inno / self-extract). The ~GB Maxine runtime is the installer-weight risk.
3. **Multi-GPU models** — the bundled models are compute **86** (RTX 3090) only; other arches
   (75 / 89 / 120) need their variants bundled, or ship a generic model and let TensorRT
   build the engine on first run.
4. **License compliance** — Maxine redistribution is governed by NVIDIA's terms + the
   bundled-model licenses (EULA PDFs in each SDK). Review and comply before publishing.
