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

Inspired by [chromabro](https://github.com/ryanbeales/chromabro) (Electron + TensorFlow
BodyPix), rebuilt as a native Windows app with NVIDIA Maxine for substantially better
matte quality plus an eye-contact feature chromabro does not have.

### Stack decision (why not Tauri/Electron)
The performance-critical path (capture → Maxine → composite) is native regardless of UI
framework — the video never flows through the UI layer, so framework choice does not
affect overlay performance. Maxine (esp. Eye Contact) is **Windows + RTX only**, so
cross-platform portability (Tauri/Electron's main draw) cannot benefit the headline
feature; Linux would require a different ML backend entirely, not a recompile. Given that,
a single-process native app removes a UI process, an IPC boundary, and a glue language
(Rust) for no perf or portability loss. C# + WinUI 3 keeps a modern, polished UI while
matching the Windows/native target; Maxine interop is a small, well-trodden P/Invoke layer.

### Non-goals (v1)
- Virtual camera device driver (apps capture the overlay via screen/window/display
  capture instead). May be revisited later.
- macOS / Linux support (Windows + RTX only).
- Non-RTX GPU fallback path (eye contact requires RTX; see Error Handling).

## 2. Hardware / SDK constraints

- **Maxine VFX SDK is C++-only** and requires an RTX GPU with Tensor Cores. Confirmed
  target machine: RTX 3090 (Ampere) — supported.
- Maxine VFX SDK provides both **AI Green Screen** (background segmentation/matting) and
  **Eye Contact** (gaze redirection), so one native pipeline covers both. No JS/WASM ML
  is needed.
- Maxine SDK redistribution/licensing must be reviewed before any public distribution
  (see Risks).

## 3. Architecture — single process

One .NET 8 process. Heavy native work lives in a thin C++ shim DLL; C# owns windowing,
compositing orchestration, and UI.

### 3.1 Native Maxine shim (C++, C-ABI DLL)
- Owns webcam capture via **Media Foundation** (kept next to Maxine to avoid per-frame
  managed↔native frame copies on the capture side).
- Runs the Maxine effect chain: **AI Green Screen → Eye Contact** (→ optional Denoise).
- Exposes a flat **C ABI** (so it is P/Invoke-able; Maxine's own API is C++): `Init`,
  `SetParams`, `Start`, `Stop`, `Shutdown`, plus frame delivery (see 3.3).
- Output: a processed RGBA frame as a **shared D3D11 texture handle** (preferred, keeps
  the frame on the GPU) or a CPU buffer (simpler fallback for v1).

### 3.2 C# app (.NET 8 + WinUI 3)
- **Control-panel window** — WinUI 3 / XAML (MVVM). Camera selection, effect toggles,
  overlay settings, status, hotkeys, persistence.
- **Overlay window** — a dedicated raw **Win32 HWND** (not a WinUI window; see Risks)
  with `WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP`, hardware-composited
  via **DirectComposition** + a **DXGI composition swap chain**, using
  [Vortice.Windows](https://github.com/amerkoleci/Vortice.Windows) (or Silk.NET) for
  D3D11/DXGI/DComp bindings. Presents the shim's output texture with per-pixel alpha.
- **Orchestrator** — starts/stops the shim, marshals params, pumps frames from shim to
  the overlay swap chain, raises status events to the view-model.

### 3.3 Interop boundary
- **Control/params:** in-process P/Invoke calls into the shim (`SetParams`, etc.).
- **Frames:** shim hands the orchestrator a shared D3D11 texture handle per frame (open
  via `OpenSharedResource`), or a CPU buffer uploaded to a dynamic texture (v1 fallback).
  No cross-process IPC anywhere.
- **Status:** shim → orchestrator via a registered callback or polled status struct;
  orchestrator → view-model via .NET events.

## 4. Overlay window behavior

- Raw Win32 layered/topmost window, DirectComposition-composited for per-pixel alpha so
  the green-screened subject shows with a transparent background.
- Optional click-through via `WS_EX_TRANSPARENT` (toggle).
- **Drag** anywhere on the overlay to move it (no title bar).
- **Resize** via corner handles and/or scroll-to-scale.
- **Snap** to screen corners/edges.
- **Shape mask:** full frame / rounded-rect / circle.
- Multi-monitor aware.
- Remembers last position and size across sessions.

## 5. Control panel (WinUI 3 / XAML, MVVM)

- Camera device selector (enumerated by the shim).
- AI Green Screen: on/off + strength.
- Eye Contact: on/off + sensitivity + look-away range (how far off-axis before it
  transitions back to real eyes).
- Optional: Denoise, auto-framing (if exposed by SDK; nice-to-have, not required).
- Gaze status indicator (live: on-camera / redirected / real-eyes).
- Overlay controls: shape, size, opacity, click-through, lock position, mirror.
- Hotkey configuration.
- Start/stop, FPS readout, GPU status.
- Settings persisted to a JSON config in `%LOCALAPPDATA%`.

UI follows WinUI 3 design guidance for an intuitive, modern control surface.

## 6. Data flow

```
Webcam ──(Media Foundation)──> Shim
                                 │
                      Maxine: AI Green Screen ──> Eye Contact ──> (Denoise?)
                                 │
                    shared D3D11 texture ──> C# orchestrator
                                 │
                  DirectComposition swap chain ──> layered topmost overlay HWND
                                                          │
                                              captured by any screen recorder

Control:  WinUI VM ──> orchestrator ──(P/Invoke)──> Shim
Status:   Shim ──(callback)──> orchestrator ──(.NET event)──> WinUI VM
```

## 7. Error handling

- **No / unsupported GPU:** clear message that Eye Contact requires an RTX GPU; refuse
  to start the Maxine chain rather than crash.
- **Camera in use / none found:** surfaced in the control panel; retry/refresh.
- **Maxine model load failure:** report the failing effect; allow disabling it.
- **Shim init/native crash:** orchestrator surfaces the error and offers restart; guard
  the P/Invoke boundary so native faults don't silently kill the UI.
- **Recorder cannot capture topmost/layered window:** documented; recommend display
  capture as the fallback in such recorders.

## 8. Milestones

- **M1** — WinUI 3 app skeleton + control-panel shell (MVVM, settings persistence) and a
  no-op shim stub wired over P/Invoke.
- **M2** — Native shim: Media Foundation capture + raw Win32 DirectComposition layered
  topmost **draggable** overlay rendering raw passthrough frames (no Maxine yet).
- **M3** — Integrate Maxine AI Green Screen in the shim; per-pixel-alpha output.
- **M4** — Integrate Maxine Eye Contact + gaze status indicator.
- **M5** — Overlay UX (shape, resize, snap, hotkeys, persistence) + error states + polish.

## 9. Risks

- **WinUI 3 cannot easily do a per-pixel-alpha layered click-through window** — mitigated
  by rendering the overlay in a dedicated raw Win32 + DirectComposition HWND (WinUI used
  only for the control panel).
- **Maxine C-ABI shim** — Maxine's API is C++; a small C-ABI wrapper DLL is required for
  P/Invoke. Scope it minimally.
- **CUDA↔D3D11 interop** — keeping Maxine output on the GPU needs CUDA/D3D interop; v1 may
  fall back to a CPU copy (fine on RTX 3090), GPU interop as an optimization.
- **Maxine SDK redistribution license** — confirm distribution + bundled-model terms
  before any public release.
- **Model/bundle size** — Maxine models add significant installer weight.
- **Recorder capture of topmost layered windows** — verify against target recorders early
  (M2); fall back to display capture where unsupported.
- **Native deployment** — ship self-contained .NET 8 + native shim + Maxine runtime;
  validate on a clean machine.

## 10. Testing

- **C# core:** unit tests (xUnit) for config (de)serialization, orchestrator state /
  start-stop logic, param marshalling, status-event plumbing (shim mocked behind an
  interface).
- **Shim:** native smoke test that loads Maxine and processes a synthetic frame; a
  capture + passthrough render check.
- **UI:** view-model tests for control-panel state and command dispatch.
- **Manual / integration:** overlay captured by OBS; drag + resize + snap; multi-monitor;
  eye-contact transition at the look-away boundary.

## 11. Component boundaries

- **Shim (C++)** — input: P/Invoke params + webcam; output: processed frames + status.
  Behind a flat C ABI; replaceable without touching the UI.
- **Orchestrator (C#)** — input: VM commands + shim status/frames; output: P/Invoke calls
  + overlay presentation + .NET events. No SDK knowledge leaks to the VM.
- **WinUI control panel (C#)** — input: user actions + status events; output: commands to
  the orchestrator. Pure UI; no native or window-management knowledge.
- **Overlay window (C#)** — input: frames + geometry commands; output: composited layered
  window. Pure presentation; no SDK knowledge.
