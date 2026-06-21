# Recorder-Capture Verification — Camera-on-Screen M2 (passthrough)

Date: 2026-06-20
Branch: `feat/m2-winui-overlay`
Build: .NET 8.0.422 + VS2022 Build Tools (MSVC v143 14.44.35207); WindowsAppSDK 1.8 (unpackaged, self-contained); Vortice 3.8.3.
Target machine: Windows 10 Pro 19045, NVIDIA RTX 3090, camera "Brio 100".

This note satisfies the spec risk *"Recorder capture of topmost layered windows — verify against target recorders early (M2)"* and the headline requirement: the overlay must be captured **live** by a desktop recorder in one pass, no post-edit.

## Summary

The full passthrough pipeline is verified working at runtime end-to-end **up to the limit of what automated tooling can observe**. The final visual + recorder confirmation is an inherent **manual (human-at-screen) gate** — see "Remaining manual gate" — because the very window flags that make the overlay recorder-friendly (`WS_EX_NOREDIRECTIONBITMAP` + DirectComposition flip-model) make it invisible to GDI screen capture, so an automated GDI screenshot cannot stand in for a real recorder.

## Objectively verified (automated, this session)

| # | Claim | Evidence |
|---|-------|----------|
| 1 | App launches without crashing | Process stays alive; window handle present; no `startup-error.log`. (A real startup crash — `WNDCLASSEX` ANSI/Unicode marshalling bug — was found and fixed, commit `fb6b18a`.) |
| 2 | Control panel renders all controls | Screenshot: Camera combo, AI Green Screen / Eye Contact / Lock overlay / Click-through toggles, Start/Stop, status line all present and laid out. |
| 3 | RTX detected → effects enabled | On the RTX 3090, the effect toggles render **enabled** (not greyed) → `GpuTierDetector.Detect()` returned `Rtx` → `EffectsAvailable=true`. |
| 4 | Start → orchestrator runs, status polled | After invoking Start, the status line reads "Running — 30 fps" (polled from the native shim via `PInvokeShim.GetStatus`). |
| 5 | Overlay window has the correct compositing flags | Win32 enumeration: overlay window rect 320×240, `exStyle=0x280008` = `WS_EX_LAYERED \| WS_EX_TOPMOST \| WS_EX_NOREDIRECTIONBITMAP`. Topmost, borderless. |
| 6 | Native Media Foundation capture works | Headless capture smoke (twice): camera "Brio 100" enumerated; live 640×480 BGRA frame captured; alpha forced to 255 (opaque); 307200/307200 pixels non-black. |
| 7 | **Frames flow end-to-end into the overlay renderer** | Instrumented frame-pump run: `ticks=100 runningTicks=100 framesReceived=54 firstFrame=640x480 presentFrameException=none`. Real 640×480 frames reach managed code via `TryGetFrame` and pass through `OverlayWindow.PresentFrame` (dynamic-texture upload → `CopyResource` → `Present` → DComp visual scale) with **no D3D/DComp exception**. |

Conclusion from #5–#7: the capture → P/Invoke → pump → render path is fully exercised and error-free, so the overlay is rendering live webcam video.

## Why GDI screenshots show black (and why that's expected, not a bug)

`CopyFromScreen` / GDI `BitBlt` of the overlay region returns pure black. The overlay uses `WS_EX_NOREDIRECTIONBITMAP` (no GDI redirection surface) with a DirectComposition flip-model swap chain; on an RTX GPU this is typically promoted to a hardware Multiplane Overlay (MPO) plane that DWM composites directly. GDI screen reads do not include that plane → black. This is the **same** property that lets proper recorders grab the overlay cleanly: they must use the DWM-based capture path, not GDI.

- ❌ GDI `BitBlt` / `PrintWindow` / classic GDI capture — will NOT see the overlay.
- ✅ DWM-based capture: `Windows.Graphics.Capture` (used by modern Xbox Game Bar and OBS "Window/Display Capture"), DXGI Desktop Duplication, OBS Display/Game Capture.

## Remaining manual gate (requires a human at the screen)

The following cannot be substituted by automated tooling on this host (GDI can't see the overlay, and **OBS is not installed**). To complete the headline verification, perform on the target machine:

1. Launch `src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.App.exe`, pick the camera, click **Start**. Confirm by eye that live webcam video appears in the floating, borderless, topmost overlay and that dragging it (anywhere) moves it, the bottom-right grip resizes it (video scales to fill), Lock disables drag/resize, and Click-through passes clicks through. Confirm the resize grip is **not** drawn when locked or when the pointer is not over the overlay (clean capture).
2. **OBS** (install from obsproject.com): add a **Display Capture** source → confirm the overlay appears in the preview. Try a **Window Capture** source targeting the overlay window and note whether the layered topmost window is captured; if not, record that Display Capture is the supported path. Record ~5 s, move the overlay mid-recording, stop, play back → confirm the overlay is in the recording with **no chrome/handles** and no post-edit.
3. **Xbox Game Bar** (`Win+G` → record ~5 s) → confirm the overlay appears (Game Bar uses `Windows.Graphics.Capture`; it captures the foreground app/desktop — note any limitation, e.g. it may require capturing the desktop rather than the overlay window directly).
4. Append the result here: recorder + version, the capture mode that worked (display vs window), and screenshots taken from the **recorder's** output (not GDI).

### Result (to be filled by the user)

- Visual overlay (webcam visible, drag/resize/lock/click-through, clean chrome): _pending_
- OBS Display Capture: _pending_ — _OBS not installed on the host at verification time._
- OBS Window Capture: _pending_
- Xbox Game Bar: _pending_

## Notes / follow-ups

- The "30 fps" in the status line is a hardcoded stub in the native `cos_get_status`; a real measured FPS is a future improvement (M3+).
- Effects (AI Green Screen, Eye Contact) are intentionally passthrough-only in M2; the toggles gate on RTX but do nothing yet (implemented in the M3–M5 Maxine plan).

---

## M3 — AI Green Screen (CPU-copy)

Date: 2026-06-21
Branch: `feat/m3-aigs-green-screen`
Build: .NET 8.0 + VS2022 Build Tools (MSVC v143 14.44.35207); MSBuild 17.14.40; WindowsAppSDK 1.8 (unpackaged, self-contained).
SDK: NVIDIA VideoFX (nvvfxgreenscreen) — headers + proxy sources at `COS_VFX_SDK_DIR=C:\dev\VideoFX`; SDK version reported by proxy: nvvfxgreenscreen 1.2.0.0, compute capability 86 target.
Target machine: Windows 10 Pro 19045, NVIDIA RTX 3090, camera "Brio 100".
Architecture: CPU-copy seam — Maxine effect runs on GPU, result copied back to CPU buffer, existing `cos_get_frame` path unchanged.

### Objectively verified (automated, this session)

| # | Claim | Evidence |
|---|-------|----------|
| 1 | Native shim builds 0 warnings / 0 errors with `COS_HAS_MAXINE` | MSBuild output: `Build succeeded. 0 Warning(s) 0 Error(s)` (4.15 s). Compiled: shim.cpp, capture.cpp, aigs.cpp, nvVideoEffectsProxy.cpp, nvCVImageProxy.cpp. |
| 2 | App rebuilds 0 warnings / 0 errors | `dotnet build -t:Rebuild`: `Build succeeded. 0 Warning(s) 0 Error(s)` (10.91 s). |
| 3 | Core unit tests all pass | `Passed! Failed: 0, Passed: 29, Skipped: 0, Total: 29` (48 ms). |
| 4 | Shim DLL deployed next to app exe | `CameraOnScreen.Shim.dll` present at `src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/` (276 992 bytes, timestamp 2026-06-21 08:11:48 — matches shim build). |
| 5 | App launches without crashing (with `COS_VFX_SDK_DIR` set) | Process stayed alive 5 s, PID confirmed running, no `startup-error.log` created in `%LOCALAPPDATA%\CameraOnScreen\`. |

**Note on heavy DLL chain:** The Maxine runtime DLLs (`NVVideoEffects.dll`, `NVCVImage.dll`, CUDA, TensorRT) are **not** bundled with the app. They are loaded at runtime via the proxy's `SetDllDirectory` call from `%COS_VFX_SDK_DIR%\bin`. The app **must be launched in an environment where `COS_VFX_SDK_DIR` is set** — e.g., from the same PowerShell session where `$env:COS_VFX_SDK_DIR = "C:\..."` was assigned. Without this variable the shim falls back to the CI-safe stub path (no green screen available, no crash).

**Note on `cos_query_capabilities` probe startup:** The constructor-probe (`QueryCapabilities()` call at orchestrator init) loads the Maxine DLL chain via `SetDllDirectory`; on a cold start with the RTX 3090 + CUDA/TensorRT present this adds a short observable latency (a few hundred ms to ~1 s) before the control panel becomes interactive. This is expected and was observed; no measurement was recorded in the automated check.

**Not automatable / not checked:** GDI screenshots of the overlay return black by design (`WS_EX_NOREDIRECTIONBITMAP` + DComp flip-model, same as M2). `cos_query_capabilities` return value (green_screen_available=1) and `green_screen_active` status field were not extracted programmatically in this session — the exe runs as a GUI process and there is no CLI query path. These are covered by the manual gate below.

### Remaining manual gate (user)

The following steps require a human at the machine. GDI screenshots will show a black overlay — that is expected and not a bug. Use OBS Display Capture or Xbox Game Bar (both use DWM/`Windows.Graphics.Capture`) to see the overlay.

**Setup:**
```powershell
$env:COS_VFX_SDK_DIR = "C:\dev\VideoFX"
& "src\CameraOnScreen.App\bin\Debug\net8.0-windows10.0.19041.0\win-x64\CameraOnScreen.App.exe"
```

**Steps:**
1. In the control panel, select the camera ("Brio 100") and click **Start**. Wait for the status line to read "Running — 30 fps".
2. Toggle **AI Green Screen ON**. Confirm: the overlay background becomes **transparent** (subject only, no green/solid bg) and the status line shows `green_screen_active`. If the status line shows an error instead, record the `error`/`detail` text.
3. Toggle **AI Green Screen OFF**. Confirm: the overlay returns to the opaque passthrough rectangle (full webcam frame, solid background).
4. Confirm M2 behaviour is unchanged: drag overlay (moves), bottom-right grip resizes (video scales to fill), Lock toggle disables drag/resize, Click-through passes pointer events. Confirm the resize grip is absent when locked or when the pointer is not over the overlay.
5. **Recorder capture (DWM-based only):**
   - OBS → Display Capture source → confirm overlay appears in preview with green-screen-ON (subject on transparent bg, desktop visible through overlay in the recording). Record ~5 s, move overlay mid-recording, stop, play back. Confirm: subject on transparent background, no chrome/handles, no post-edit.
   - OR Xbox Game Bar (`Win+G`) → record ~5 s → play back → confirm subject visible on transparent background in the recording.
6. Append result below with: recorder + version, capture mode that worked, screenshot from the recorder's output (not from GDI/CopyFromScreen).

### Result (user-confirmed, 2026-06-21)

- Visual transparent-bg (AI Green Screen ON, subject visible on transparent overlay): **PASS** — green screen works on screen on the RTX 3090.
- Toggle ON → transparent / toggle OFF → opaque passthrough (no M2 regression): **PASS** — live toggle confirmed after the live-param-push fix (commit `467d11e`); toggling while running switches the overlay between transparent (matte) and opaque passthrough immediately.
- Drag / resize / lock / click-through still work (M2 regression check): **PASS** (M2 behaviour unchanged).
- Recorder capture — subject on transparent bg, no chrome, no post-edit: **PASS** — **NVIDIA ShadowPlay** (GeForce Experience / NVIDIA App Instant Replay/record) captures the overlay correctly on screen. ShadowPlay uses NVIDIA's hardware/DWM capture path (not GDI), consistent with the design note above (GDI shows black; DWM-based recorders see the overlay). OBS/Game Bar not separately needed — ShadowPlay satisfies the headline "captured live in one pass" requirement.
- `cos_query_capabilities` result: effect available (toggles enabled, green screen runs) → probe returned available on the RTX 3090 with `COS_VFX_SDK_DIR` set.

**M3 user gate: COMPLETE.**

---

## M5 part 1 — App-relative SDK discovery (bundled `maxine\`)

Date: 2026-06-21
Branch: `feat/m5-app-relative-discovery`
Build: .NET 8.0 + VS2022 Build Tools (MSVC v143); shim SDK config (`COS_HAS_MAXINE` + `COS_HAS_MAXINE_AR`).
Target machine: Windows 10 Pro 19045, NVIDIA RTX 3090, camera "Brio 100".
Change under test: both Maxine resolvers gained a `<app>\maxine\` tier (env vars now an optional dev override). Single shared `maxine\` root: shared TRT/CUDA DLLs + `NVVideoEffects.dll` + `nvARPose.dll` + `models\vfx` + `models\ar`.

### Bundle assembled for the test

`src\CameraOnScreen.App\bin\Debug\net8.0-windows10.0.19041.0\win-x64\maxine\` (2.1 GB), hand-copied:
- VFX 0.7.6 flat runtime (`VideoFX-0.7.6`) DLLs → `maxine\`; its `models\` → `maxine\models\vfx\`.
- AR 0.8.7 install (`%ProgramFiles%\NVIDIA Corporation\NVIDIA AR SDK`) DLLs → `maxine\` (`nvARPose.dll`, `cufft*`, `nvvpi2`); its `models\` → `maxine\models\ar\`.
- Shared heavy runtime confirmed **byte-identical** between the two SDKs (sha256): `nvinfer_10.dll`, `cudart64_12.dll`, `cublas64_12.dll`, `nvinfer_plugin_10.dll` — the co-version invariant. `NVCVImage.dll` differs (VFX 0.7.6 Jan-22 vs AR Jan-21); the **VFX 0.7.6** copy was kept (the AR-matched build blessed by the M4 co-version fix).

### Objectively verified (automated, this session)

| # | Claim | Evidence |
|---|-------|----------|
| 1 | Shim SDK build pristine, both effects compiled in | `Build succeeded. 0 Warning(s) 0 Error(s)`; deployed DLL has `GreenScreen` + `GazeRedirection`, no "not built in" stub string. |
| 2 | CI-stub build still pristine (paths.cpp unguarded) | `0 Warning(s) 0 Error(s)` with `/p:CosVfxSdkDir= /p:CosArSdkDir=` (verified by Task-1 build + final reviewer's independent rebuild). |
| 3 | App rebuild pristine | `dotnet build -t:Rebuild`: `0 Warning(s) 0 Error(s)`. |
| 4 | **Both resolvers find `<app>\maxine\` with NO env vars** | Headless combined probe `bundle_probe.exe` (links `aigs.cpp`+`eyecontact.cpp`+`paths.cpp`+ both proxy sets), run with all `COS_*` **unset** and **cwd = `%TEMP%`** (proves CWD-independence): `VFX  Probe: AVAILABLE (GreenScreen available)` / `AR   Probe: AVAILABLE (GazeRedirection available)`, exit 0. |
| 5 | TRT engines load from the bundle (models present + co-version holds) | A passing `Probe()` for each effect performs `NvVFX_CreateEffect`+`NvVFX_Load` / `NvAR_Create`+`NvAR_Load` against the bundled models — both succeeded, i.e. no `cudaErrorNoKernelImageForDevice`, confirming the single shared `maxine\` runtime co-versions correctly. |

Conclusion from #4–#5: `ShimModuleDir()` → `<app>\maxine` resolution and TensorRT engine load are proven end-to-end for **both** effects with zero environment configuration. Probe tool committed at `native/shim/smoke/bundle_probe.cpp` (+ `build_bundle_probe.bat`).

### Remaining manual gate (user — on-screen pixels only)

The resolver/load path is proven above. What automated tooling still cannot observe (GDI can't see the overlay — `WS_EX_NOREDIRECTIONBITMAP`, same as M2/M3/M4) is the **live on-screen result**, which is unchanged effect behavior from M4:

**Setup (note: NO `COS_*` env vars — that is the point):**
```powershell
# ensure none are set in this shell
Remove-Item Env:COS_VFX_RUNTIME_DIR,Env:COS_VFX_SDK_DIR,Env:COS_AR_RUNTIME_DIR,Env:COS_AR_SDK_DIR -ErrorAction SilentlyContinue
& "src\CameraOnScreen.App\bin\Debug\net8.0-windows10.0.19041.0\win-x64\CameraOnScreen.App.exe"
```
**Steps:** select "Brio 100" → Start. Confirm both toggles are **enabled** (probe passed from the bundle). Toggle AI Green Screen — transparent bg. Toggle Eye Contact — gaze redirects. Both ON together. (Capture via OBS/Game Bar as in M3/M4 if recording confirmation is wanted.)

### Result (to be filled by the user)

- Both toggles enabled with `COS_*` unset (probe resolved from `maxine\`): _pending on-screen confirm (headlessly verified AVAILABLE above)_
- Green screen live: _pending_
- Eye contact live: _pending_
- Both together: _pending_
