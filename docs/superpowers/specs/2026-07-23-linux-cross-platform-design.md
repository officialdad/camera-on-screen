# Camera-on-Screen — Design Spec: Linux Cross-Platform Migration

**Date:** 2026-07-23
**Status:** Proposed (grounded 2026-07-23; awaiting Phase 0 spike results before committing later phases)
**Parent spec:** `docs/superpowers/specs/2026-06-20-camera-on-screen-design.md`.
**Scope:** Windows **and** Linux, NVIDIA RTX. **macOS explicitly out of scope** (no NVIDIA hardware there → no Maxine).

## 1. Goal

Run Camera-on-Screen on a Linux desktop with **full effect parity** to the Windows build
(Maxine AI green screen, eye-contact/gaze, FRUC 30→60), reusing the maximum of the existing
investment (`Core`, MVVM view models, the C-ABI shim, the Maxine integration), while keeping
the Windows build working. Motivated by a full OS migration Windows → Linux.

## 2. Decision — Option 2: Avalonia control panel + native C-ABI shim (`.so`)

**Chosen:** keep the native pipeline; port it. New cross-platform **Avalonia** control panel
reuses `Core` + view models; the existing C++ shim is compiled to a Linux `.so` with a
V4L2 capture backend; Maxine + FRUC run via their Linux `.so` runtimes; a per-OS transparent
overlay presents frames.

**Rejected: Electron / Tauri / Rust rebuild.** Grounded reasoning (research 2026-07-23):

- A web framework's one real advantage for this app — `getUserMedia` "capture for free" — **only
  pays off if Maxine is dropped.** The moment Maxine is kept (the stated requirement), every
  camera frame must round-trip JS→native→CUDA→native→JS, reintroducing native code *and* a
  per-frame marshaling tax. The framework then only replaces the **control panel**.
- If a framework only replaces the control panel, the lazy cross-platform swap for our **existing
  C# control panel is Avalonia** (keeps `Core` + VMs), not Electron/Tauri (throws away every line
  of C# and rewrites the UI in JS).
- **Tauri is disqualified on Linux specifically:** WebKitGTK ships `enable-media-stream`/`enable-webrtc`
  **off by default** → `getUserMedia` unavailable without a custom WebKitGTK build
  (tauri#12547, wry#85); Wayland `always_on_top` silently no-ops (tauri#3117, tao#1134).
- **Electron** works (bundled Chromium → reliable `getUserMedia`; ~80–150 MB) but buys nothing
  over Avalonia here except a heavier footprint and the JS↔native frame tax (electron#11479,
  measured >100 ms Buffer return in older Electron — must be benchmarked, not assumed).

**Fallback tier already exists.** Non-RTX users (any OS) keep the ONNX/MediaPipe CPU green-screen
path (`docs/superpowers/specs/2026-07-16-onnx-greenscreen-fallback-design.md`) and the passthrough
overlay. This spec does not change that; it adds a Linux *native/RTX* tier beside the Windows one.

## 3. Grounded research findings (2026-07-23)

Five parallel research agents; sources inline. Verdicts that drive the design:

### 3.1 One-pass recorder capture — CONFIRMED on Windows + Linux-X11 + Linux-Wayland

- Reliable path is **display/screen capture** (compositor's final composited framebuffer), into
  which a transparent always-on-top window is already alpha-blended. Windows: DXGI Desktop
  Duplication / WGC (learn.microsoft.com desktop-dup-api; obs#4140). Linux X11: XSHM/XComposite
  screen capture. Linux Wayland: `xdg-desktop-portal` → `ScreenCast` → PipeWire, on
  GNOME(mutter)/KDE(kwin)/wlroots.
- **Not** GDI/BitBlt window capture (shows GPU-composited windows black; use WGC if a window
  path is ever needed) — obs#127687, electron#24346. This is a GPU-compositing artifact, **not**
  transparency-specific, and the current Windows MPO/`WS_EX_NOREDIRECTIONBITMAP` trick is **not
  required** for a normal composited window.
- **HARD RULE — never enable content protection.** `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`
  / Electron `setContentProtection` / Tauri `set_content_protection` delete the overlay from every
  capture surface, enforced at the compositor (learn.microsoft.com SetWindowDisplayAffinity;
  tauri#5132; electron#45990). Leave OFF.
- Wayland extra: **one-time portal permission grant** per capture session; legacy X11 capture
  sources read black under Wayland (use the PipeWire source).

### 3.2 Maxine on Linux — ALL THREE effects CONFIRMED (RTX/Tensor-Core only)

- **VFX green screen:** official Linux build; feature catalog identical Win/Linux
  (docs.nvidia.com vfx-sdk-system-guide; NVIDIA-Maxine/VFX-SDK-Samples has `build_samples.sh`).
  Windows docs frame VFX as client-side, Linux as "server-side" tuning — **same features**.
- **AR eye-contact / gaze:** dedicated "Maxine Linux Augmented Reality SDK" on NGC (v0.8.4.0),
  `nvargazeredirection` enumerated; Ubuntu 20.04/22.04/24.04, driver ≥ 570.26
  (docs.nvidia.com maxine/ar/1.1.0/LinuxARSDK). **Caveat:** Linux AR is documented as optimized
  for server/Triton; some samples are Triton-only. The in-process C-ABI gaze path (what we use)
  exists but is **less battle-tested on Linux → Spike A.**
- **FRUC (Optical Flow SDK):** Linux supported, ships `libNvFRUC.so`, driver ≥ 510.47.03,
  Turing+, no TensorRT (docs.nvidia.com nvfruc-programming-guide).
- **Same C ABI** (`NvVFX_*`, `NvAR_*`, `NvOFFRUC*`) as the Windows shim → the shim's Maxine
  integration reuses; only the capture backend and build system are new.
- **Redistribution:** same platform-agnostic Maxine EULA + Optical Flow/DesignWorks license as
  Windows; existing legal clearance carries over (re-confirm the Linux `.so` set is the same
  "distributable portions" — same EULA).
- **Co-version hazard persists on Linux** and the pinned CUDA/TRT versions **differ per-OS**
  (Linux VFX/AR bundle their own pins ≠ Windows VFX 1.2.0.0 + AR 1.1.1.0) → **Spike C.**

### 3.3 Framework/toolkit for the control panel

- **Avalonia** targets **X11 directly** on Linux, renders its own widgets via **Skia** — **no GTK/Qt
  dependency, desktop-environment-agnostic** (GNOME/KDE/XFCE identical). Any distro with the .NET
  SDK + X11 runs it (docs: supported-platforms; app-development/window-management). Matches this
  host's "NuGet-only, no special workload" constraint — **no WinUI-style workload needed**.
- Linux supports `Topmost` (always-on-top) + `WindowDecorations.None` (frameless).
  `TransparencyLevelHint = Transparent` on Linux **depends on a compositor** (present on all
  mainstream desktops).
- **Wayland only via XWayland** (native = private preview); under it "applications cannot set
  absolute window positions — `Window.Left`/`Window.Top` may be ignored" — the same limit that
  breaks drag-overlay-under-cursor. **Conclusion: run under X11** (`GDK_BACKEND=x11`), identical
  to the Electron/Tauri conclusion. Wayland is a fragile path for *every* toolkit; X11 is the
  supported target for v1.

### 3.4 In-browser effects (context only — not the chosen path)

`@mediapipe/tasks-vision` does green screen + fist/open-palm gesture in any webview, any GPU;
**no in-browser eye-contact exists** (gaze redirection is Maxine-only). This is the existing
non-RTX fallback tier's technology, not part of the RTX-parity Linux port.

## 4. Target architecture — reuse map

| Component | Today (Windows) | Linux port | Reuse |
|---|---|---|---|
| `CameraOnScreen.Core` | pure .NET 8 | **unchanged** | 100% |
| View models (MVVM) | **already in `Core`** (`Core/ViewModels/MainViewModel.cs`, `CommunityToolkit.Mvvm`, no WinUI types) | **referenced unchanged** — verified by Phase 0 Track A | 100% |
| Control panel UI | WinUI 3 XAML | **new** `App.Avalonia` (Avalonia XAML) | XAML re-authored; bindings reuse VMs |
| C-ABI shim (9 exports) | `native/shim` MSBuild `.dll` | + **CMake build → `.so`** | ABI + Maxine/FRUC code reused |
| Capture backend | Media Foundation | **new** V4L2 backend behind a capture interface | interface new; consumers reuse |
| Maxine VFX/AR/FRUC | Windows runtimes | Linux `.so` runtimes; `paths.cpp` gains Linux tier | integration reused |
| Overlay window + present | C# `OverlayWindow` (Win32 + DComp + D3D11) | **new** transparent X11 present (see §5.2) | none — per-OS |
| Persistence | `%LOCALAPPDATA%\...\config.json` | `$XDG_CONFIG_HOME`/`~/.config/...` | logic reuse, path new |

**Guiding boundary (unchanged from parent spec):** the shim never creates a window and never
renders; it captures + runs effects + exposes the C ABI. All windowing/compositing stays in the
managed/host side. On Linux the "host" is the Avalonia app + a native present surface.

## 5. Platform-specific design points

### 5.1 Shim build system

The current shim is an MSBuild `vcxproj` (Windows-only, deliberately outside the `.sln`). Linux
needs a **CMake** build (clang/gcc) producing `libCameraOnScreen.Shim.so`, compiling the same
sources + the V4L2 backend + the Maxine/FRUC proxy stubs. Windows keeps its `vcxproj` (or CMake
adopts both — deferred; CMake-for-both is the tidier end state but not required for v1).

### 5.2 Linux transparent overlay + present (highest-risk, gated on Spike B)

Two candidate designs; the spike picks one:

- **(a) Avalonia transparent Topmost window** hosting an `OpenGlControlBase` (or composition
  surface) that blits camera frames pushed from the shim. Pro: one window system, less native
  code. Con: per-frame frame handoff into Avalonia's render loop at 60 Hz is unproven for this
  use; drag/click-through under X11 needs verifying.
- **(b) Separate native X11 window** (`override-redirect` ARGB visual + EGL/OpenGL), drawn by a
  small Linux present layer, mirroring today's separate `OverlayWindow`. Pro: full control, matches
  current architecture (overlay is already a separate window from the control panel). Con: most
  new native code (X11 + EGL + input hook for drag).

Both must honor: **premultiplied alpha** (matte = A, RGB premultiplied — identical to the Aigs
contract), a running **compositor**, **no content protection**, and X11 (not native Wayland).

### 5.3 Input / drag-to-move overlay

Windows uses a global `WH_MOUSE_LL` hook + timer `GetCursorPos`→`SetBounds` (MPO-plane alpha loss
makes `WM_NCHITTEST` unreliable). Linux equivalent: X11 `XGrabPointer` / `XQueryPointer` polled on
a timer, or the toolkit's pointer events under X11. **Wayland cannot self-position** → another
reason X11-only for v1. Design finalized after Spike B picks 5.2(a) vs (b).

## 6. Risks & de-risk spikes (GATE — run before committing Phases 2+)

These require a **Linux + RTX box** (this dev host is Windows and cannot build/run any of them).

- **Spike A — in-process Maxine gaze on Linux.** Build `AR-SDK-Samples` eye-contact on Ubuntu+RTX
  via the **native (non-Triton)** `nvargazeredirection` path; confirm it loads and runs in-process.
  *Kill criterion:* if only Triton works, eye-contact becomes Linux-unavailable (greys out) and the
  Linux build ships green-screen + FRUC only.
- **Spike B — transparent X11 overlay captured live.** Minimal `override-redirect` ARGB + GL window
  (or Avalonia transparent Topmost) over the desktop; confirm OBS **screen capture** grabs it live,
  transparent regions show the desktop (not black), no content-protection set. Picks §5.2 (a) vs (b).
- **Spike C — per-OS CUDA/TRT co-version.** Assemble a Linux `maxine/` stage with the Linux VFX +
  AR `.so` set; confirm VFX + AR **coexist in one process** (the Windows first-load-wins hazard,
  re-verified with Linux pins) + FRUC's CUDA-11 `.so` stays compatible. Mirrors the Windows
  co-version verification (`docs/superpowers/specs/2026-06-26-camera-on-screen-13-fruc-coversion-findings.md`).

Other risks: Electron/JS frame tax (N/A — Avalonia path avoids it); WebKitGTK (N/A — Avalonia
uses Skia, not a webview); CI (self-hosted runner is Windows+RTX — a Linux+RTX runner is a new
CI dependency, deferred to the phase that needs Linux CI).

## 7. Phased roadmap

Each phase produces working, testable software and becomes its own plan when reached. Only
**Phase 0** is detailed here (§8); later phases are outlined and expanded post-spike.

- **Phase 0 — Windows-buildable foundation + spikes.** (a) Scaffold `App.Avalonia`, reuse `Core`,
  port a minimal control panel, build+run on **Windows** (proves Core/VM reuse + Avalonia in this
  toolchain, zero Linux dependency). (b) Run Spikes A/B/C on a Linux+RTX box. **Gate:** spikes pass.
- **Phase 1 — Avalonia panel parity (Windows).** Port remaining controls/bindings; Avalonia app
  reaches feature parity with the WinUI panel on Windows, still driving the Windows shim. Keeps the
  UI rewrite decoupled from the native Linux work.
- **Phase 2 — Shim `.so` + V4L2 capture (Linux).** CMake build; V4L2 backend behind a capture
  interface; passthrough frames on Linux (no effects yet). Core tests unchanged.
- **Phase 3 — Linux overlay + present.** Implement §5.2 winner; drag/resize; premultiplied
  passthrough captured live by OBS (human verification gate, per `docs/superpowers/verification/`).
- **Phase 4 — Maxine on Linux.** `paths.cpp` Linux runtime tier; Linux `maxine/` stage
  (co-versioned per Spike C); green screen + gaze (if Spike A passed) + FRUC.
- **Phase 5 — Packaging + CI.** Linux bundle (AppImage/`.deb`/tarball), Linux+RTX CI runner,
  redistribution notice update.

## 8. Phase 0 tasks (executable now)

**Track A (Windows host — buildable/testable here):**

1. Add `src/CameraOnScreen.App.Avalonia` (Avalonia .NET 8 desktop project) referencing
   `CameraOnScreen.Core`. Install templates: `dotnet new install Avalonia.Templates`.
2. Add the project to `CameraOnScreen.sln` (SDK-style; safe for `dotnet build`, unlike the shim
   vcxproj).
3. Port the smallest real slice: the main window + **one** bound control (e.g. the Green Screen
   toggle) wired to the existing view model, proving VM reuse across toolkits.
4. `dotnet build src/CameraOnScreen.App.Avalonia` clean (0 warnings — repo standard).
5. Run on Windows; confirm the toggle drives the VM. Commit.

*Note:* Phase 0 Track A does **not** touch the shim, the WinUI app, or CI. It is additive and
reversible — the WinUI app remains the shipping Windows build until Phase 1 reaches parity.

**Track A result (2026-07-23 — DONE):** `src/CameraOnScreen.App.Avalonia` created, references `Core`,
binds `MainViewModel.GreenScreenEnabled`/`CapabilityDetail` unchanged (via `FakeShim` +
`Orchestrator`), builds **0 warnings / 0 errors**, launches crash-free, added to the `.sln`.
**Toolchain pin (gotcha):** the Avalonia template defaults to **net10 + Avalonia 12**, whose
Roslyn-4.14 source generator silently no-ops on this host's SDK 8 (Roslyn 4.11) → missing
`InitializeComponent`. Pinned to **net8 + Avalonia 11.2.1** (dropped `AvaloniaUI.DiagnosticsSupport`
+ `.WithDeveloperTools()`, both Avalonia-12-only). Revisit Avalonia 12 only after the host SDK moves
past Roslyn 4.14.

**Track B (Linux + RTX box — cannot run on this host):** execute Spikes A, B, C (§6), record
results as a findings spec (`docs/superpowers/specs/2026-MM-DD-linux-spike-findings.md`) matching
the FRUC co-version findings format. Later phases are planned only after this lands.

## 9. Non-goals / open questions

- **Non-goals (v1):** macOS; native Wayland (XWayland/X11 only); a single CMake build for both OSes
  (nice-to-have, not required); dropping the WinUI app before Avalonia reaches parity.
- **~~Open — view-model location~~ RESOLVED (Phase 0 Track A):** the MVVM VMs already live in
  `Core` (`CommunityToolkit.Mvvm`, zero WinUI types), so both WinUI and Avalonia reference them
  unchanged. No VM extraction needed. Confirmed by binding `MainViewModel.GreenScreenEnabled` from
  the Avalonia scaffold with a clean build + crash-free launch.
- **Open — target distro/desktop** for first-class support (drives packaging + the Linux CI
  runner image). Ubuntu 22.04/24.04 + X11 is the Maxine-documented baseline.
