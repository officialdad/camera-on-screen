# Camera-on-Screen — Design Spec

**Date:** 2026-06-20
**Status:** Approved (design); pending implementation plan
**Platform:** Windows 10/11, NVIDIA RTX GPU (Turing/Ampere/Ada + Tensor Cores)
**Stack:** C# / .NET 8 + WinUI 3 (single process) + native C++ Maxine shim (P/Invoke)

## 1. Goal

A Windows desktop app that shows a live webcam feed as a transparent, always-on-top,
draggable overlay anywhere on the desktop. The feed is processed in real time by NVIDIA
Maxine VFX (AI Green Screen for background removal + Eye Contact for gaze redirection).
The overlay is rendered with hardware compositing and is captured live by any desktop
screen recorder (OBS, Xbox Game Bar, Camtasia, etc.), so a recording session needs no
separate post-edit compositing step.

Inspired by [chromabro](https://github.com/ryanbeales/chromabro), rebuilt as a native
Windows app with NVIDIA Maxine for substantially better matte quality plus an eye-contact
feature chromabro does not have.

### Why a single-process native app
The performance-critical path (capture → Maxine → composite) is native and the video
never flows through the UI layer, so UI-framework choice does not affect overlay
performance. Maxine (esp. Eye Contact) is **Windows + RTX only**, so there is no
cross-platform target to design for. A single .NET 8 process with WinUI 3 for the UI and a
thin C++ shim for the native SDK work keeps a modern, polished UI, matches the
Windows/native target, and avoids a second process, an IPC boundary, and any glue
language. Maxine interop is a small, well-trodden P/Invoke layer.

### GPU tiers (defined behavior, not a fallback hack)
- **RTX present:** full feature set — AI Green Screen + Eye Contact.
- **No / non-RTX GPU:** app still runs in **plain-overlay passthrough** (raw webcam, no
  effects). Effect toggles are disabled with a clear "requires RTX GPU" note. The Maxine
  chain is simply never started.

### Non-goals (v1)
- Virtual camera device driver (apps capture the overlay via screen/window/display
  capture instead). May be revisited later.
- macOS / Linux support.

## 2. Hardware / SDK constraints

- **Maxine VFX SDK is C++-only** and requires an RTX GPU with Tensor Cores. Confirmed
  target machine: RTX 3090 (Ampere) — supported.
- Maxine VFX SDK provides **AI Green Screen** (background segmentation/matting).
  > **Correction (2026-06-21, M3 design):** Eye Contact is **not** in the VFX SDK — it lives
  > in the separate **Maxine AR SDK** (a distinct download/dependency). The original
  > assumption that "one native pipeline covers both via the VFX SDK" is wrong. AI Green
  > Screen (M3) uses the VFX SDK; Eye Contact (M4) will require adding the AR SDK. See
  > `2026-06-21-camera-on-screen-m3-aigs-design.md`.
- Maxine output is an `NvCVImage` (typically CUDA/GPU memory).
- Maxine SDK redistribution/licensing must be reviewed before any public distribution
  (see Risks).

## 3. Architecture — single process

One .NET 8 process. Heavy native work lives in a thin C++ shim DLL; **C# owns all
windowing, compositing, and UI**. The shim never creates windows and never renders.

### 3.1 Native Maxine shim (C++, C-ABI DLL)
- Owns webcam capture via **Media Foundation** (kept next to Maxine to avoid per-frame
  managed↔native copies on the capture side) and camera enumeration.
- Runs the Maxine effect chain: **AI Green Screen → Eye Contact** (→ optional Denoise).
- Writes each processed RGBA frame into a **D3D11 texture owned by C#** (see 3.3), using
  CUDA↔D3D11 interop. Does **not** render to screen.
- Exposes a flat **C ABI** (Maxine's own API is C++): `Init(d3dDevice, params)`,
  `EnumerateCameras`, `SetParams`, `Start`, `Stop`, `GetStatus`, `Shutdown`, plus
  frame-ready signalling (see 3.3).

### 3.2 C# app (.NET 8 + WinUI 3)
- **Control-panel window** — WinUI 3 / XAML (MVVM). Camera selection, effect toggles,
  overlay settings, status, hotkeys, persistence.
- **Overlay window** — a dedicated raw **Win32 HWND** (not a WinUI window; see Risks)
  with `WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP`, hardware-composited
  via **DirectComposition** + a **DXGI composition swap chain**, using
  [Vortice.Windows](https://github.com/amerkoleci/Vortice.Windows) for D3D11/DXGI/DComp
  bindings. Samples the shim-filled texture and presents it with per-pixel alpha. **C#
  creates and owns this overlay.**
- **Orchestrator** — creates the single shared D3D11 device, starts/stops the shim,
  marshals params, drives the present loop (shim-filled texture → overlay swap chain), and
  exposes status to the view-model.

### 3.3 Interop boundary
- **Single shared D3D11 device.** C# (Vortice) creates the device and passes it to the
  shim at `Init`. The shim writes Maxine output into a C#-owned `ID3D11Texture2D` on that
  device — **zero-copy, no shared handles / `OpenSharedResource`** (those are for
  cross-device/cross-process sharing, which does not apply here).
- **v1 fallback:** if CUDA↔D3D11 interop is deferred, the shim returns a CPU buffer that
  C# uploads to a dynamic texture. Fine on RTX 3090; GPU interop is the optimization.
- **Control/params:** in-process P/Invoke into the shim. The orchestrator depends on an
  **`INativeShim` interface**; the default implementation wraps the P/Invoke entry points,
  and tests substitute a fake (P/Invoke is not directly mockable).
- **Status:** the orchestrator **polls** `GetStatus` (a flat struct: fps, gaze state,
  active effects, errors) on the present/UI tick — preferred over a native→managed
  callback, which risks crashing the process on an unhandled native fault.

## 4. Overlay window behavior

- Raw Win32 layered/topmost window, DirectComposition-composited for per-pixel alpha so
  the green-screened subject shows with a transparent background.
- **Clean for capture by default:** no visible chrome (no borders, handles, or controls)
  in the normal/locked state, so recordings stay clean with no post-edit.
- **Edit affordances on demand:** drag and resize handles appear only on hover while the
  overlay is **unlocked**, and are hidden again when locked or on mouse-leave.
- **Interaction modes (mutually exclusive, resolved explicitly):**
  - *Unlocked* — overlay accepts input: **drag anywhere** to move, **corner handles** /
    scroll-to-scale to resize, snap to corners/edges.
  - *Click-through* (`WS_EX_TRANSPARENT`) — all input passes through; the overlay cannot
    be dragged or grabbed. This is the recording state.
  - Switching between modes is done from the control panel **and** via a **global hotkey**
    (see §5), so the user can re-grab the overlay even while another app is focused.
- **Shape mask:** full frame / rounded-rect / circle (rectangular HWND; masked corners are
  transparent).
- **Mirror** is applied at present time in the overlay (after Maxine), so it never feeds a
  flipped image into gaze redirection.
- Multi-monitor aware. Remembers last position, size, shape, and lock state across
  sessions.

## 5. Control panel (WinUI 3 / XAML, MVVM)

- Camera device selector (list from the shim).
- AI Green Screen: on/off + strength.
- Eye Contact: on/off + sensitivity + look-away range (how far off-axis before it
  transitions back to real eyes).
- Optional: Denoise, auto-framing (if exposed by SDK; nice-to-have, not required).
- Gaze status indicator (live: on-camera / redirected / real-eyes).
- Overlay controls: shape, size, opacity, lock/unlock, click-through, mirror.
- **Global hotkeys** (registered via `RegisterHotKey`, so they fire while any app is
  focused): toggle lock/unlock, toggle click-through, show/hide overlay, start/stop. User-
  configurable.
- Start/stop, FPS readout, GPU tier/status (effects disabled with a note when no RTX).
- Settings persisted to a JSON config in `%LOCALAPPDATA%`.

UI follows WinUI 3 design guidance for an intuitive, modern control surface.

## 6. Data flow

```
Webcam ──(Media Foundation)──> Shim
                                 │
                      Maxine: AI Green Screen ──> Eye Contact ──> (Denoise?)
                                 │
              CUDA↔D3D11 interop, writes into C#-owned texture (shared device)
                                 │
                          C# orchestrator (present loop)
                                 │
   mirror (if on) ──> DirectComposition swap chain ──> layered topmost overlay HWND (C#)
                                                          │
                                              captured by any screen recorder

Control:  WinUI VM ──> orchestrator ──(INativeShim / P/Invoke)──> Shim
Status:   orchestrator polls GetStatus ──> .NET event ──> WinUI VM
```

## 7. Error handling

- **No / non-RTX GPU:** effects disabled with a clear "requires RTX GPU" note; app still
  runs in plain-overlay passthrough. The Maxine chain is never started.
- **Camera in use / none found:** surfaced in the control panel; retry/refresh.
- **Maxine model load failure:** report the failing effect; allow disabling it and
  continuing with the rest.
- **Shim init/native fault:** the orchestrator surfaces the error and offers restart;
  P/Invoke calls are guarded so a native fault does not silently kill the UI.
- **Recorder cannot capture topmost/layered window:** documented; recommend display
  capture as the fallback in such recorders.

## 8. Milestones

- **M1** — WinUI 3 app skeleton + control-panel shell (MVVM, settings persistence) and an
  `INativeShim` no-op stub wired over P/Invoke.
- **M2** — C# builds the raw Win32 + DirectComposition layered topmost **draggable**
  overlay; shim does Media Foundation capture and fills a C#-owned texture with raw
  passthrough frames (no Maxine yet). Verify a screen recorder captures the overlay.
- **M3** — Integrate Maxine AI Green Screen in the shim; per-pixel-alpha output through the
  shared texture.
- **M4** — Integrate Maxine Eye Contact + gaze status indicator.
- **M5** — Overlay UX (clean-capture chrome rules, shape, resize, snap, mirror, global
  hotkeys, persistence) + error states + polish.

## 9. Risks

- **WinUI 3 cannot easily do a per-pixel-alpha layered click-through window** — mitigated
  by rendering the overlay in a dedicated raw Win32 + DirectComposition HWND (WinUI used
  only for the control panel).
- **Maxine C-ABI shim** — Maxine's API is C++; a small C-ABI wrapper DLL is required for
  P/Invoke. Scope it minimally.
- **CUDA↔D3D11 interop** — needed to keep Maxine output on the GPU; v1 may fall back to a
  CPU copy (fine on RTX 3090), GPU interop as an optimization.
- **Maxine SDK redistribution license** — confirm distribution + bundled-model terms
  before any public release.
- **Model/bundle size** — Maxine models add significant installer weight.
- **Recorder capture of topmost layered windows** — verify against target recorders early
  (M2); fall back to display capture where unsupported.
- **Native deployment** — ship self-contained .NET 8 + native shim + Maxine runtime;
  validate on a clean machine.

## 10. Testing

- **C# core:** unit tests (xUnit) for config (de)serialization, orchestrator state /
  start-stop logic, param marshalling, and status polling — all against a fake
  `INativeShim`.
- **Shim:** native smoke test that loads Maxine and processes a synthetic frame, and a
  capture-to-texture fill check (no rendering — rendering is C#'s).
- **UI:** view-model tests for control-panel state, command dispatch, and hotkey mapping.
- **Manual / integration:** overlay captured by OBS with no chrome in frame; drag + resize
  + snap; lock/click-through toggle via global hotkey while another app is focused;
  multi-monitor; eye-contact transition at the look-away boundary.

## 11. Component boundaries

- **Shim (C++)** — input: P/Invoke params + shared D3D11 device + webcam; output: filled
  texture + status struct. Behind a flat C ABI; never renders or creates windows;
  replaceable without touching the UI.
- **Orchestrator (C#)** — input: VM commands + shim status; output: P/Invoke via
  `INativeShim`, overlay present loop, .NET status events. Owns the shared D3D11 device. No
  SDK knowledge leaks to the VM.
- **WinUI control panel (C#)** — input: user actions + status events; output: commands to
  the orchestrator. Pure UI; no native or window-management knowledge.
- **Overlay window (C#)** — input: filled texture + geometry/lock/mirror commands; output:
  composited layered window. Pure presentation; no SDK knowledge.
