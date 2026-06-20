# Camera-on-Screen — Design Spec

**Date:** 2026-06-20
**Status:** Approved (design); pending implementation plan
**Platform:** Windows 10/11, NVIDIA RTX GPU (Turing/Ampere/Ada + Tensor Cores)

## 1. Goal

A Windows desktop app that shows a live webcam feed as a transparent, always-on-top,
draggable overlay anywhere on the desktop. The feed is processed in real time by NVIDIA
Maxine VFX (AI Green Screen for background removal + Eye Contact for gaze redirection).
The overlay is rendered natively and is captured live by any desktop screen recorder
(OBS, Xbox Game Bar, Camtasia, etc.), so a recording session needs no separate
post-edit compositing step.

Inspired by [chromabro](https://github.com/ryanbeales/chromabro) (Electron + TensorFlow
BodyPix), rebuilt on Tauri v2 with NVIDIA Maxine for substantially better matte quality
plus an eye-contact feature chromabro does not have.

### Non-goals (v1)
- Virtual camera device driver (apps capture the overlay via screen/window/display
  capture instead). May be revisited later.
- macOS / Linux support (Windows + WebView2 only for v1).
- Non-RTX GPU fallback path (eye contact requires RTX; see Error Handling).

## 2. Hardware / SDK constraints

- **Maxine VFX SDK is C++-only** and requires an RTX GPU with Tensor Cores. Confirmed
  target machine: RTX 3090 (Ampere) — supported.
- Maxine VFX SDK provides both **AI Green Screen** (background segmentation/matting) and
  **Eye Contact** (gaze redirection), so one native pipeline covers both. No JS/WASM ML
  (MediaPipe/TensorFlow.js) is needed.
- Maxine SDK redistribution/licensing must be reviewed before any public distribution
  (see Risks).

## 3. Architecture — two processes

### 3.1 Maxine sidecar (C++)
- Owns webcam capture via **Media Foundation**.
- Runs the Maxine effect chain: **AI Green Screen → Eye Contact** (→ optional Denoise).
- Renders the overlay **itself** with **Direct3D 11** into a layered, topmost,
  per-pixel-alpha window. Frames never cross the IPC boundary (avoids 60 fps RGBA
  bandwidth problems).
- Controlled by line-delimited JSON commands over stdin; emits line-delimited JSON
  status events over stdout.

Rationale for C++ (not Rust FFI): Maxine has no official Rust bindings; the SDK samples
are C++. A `bindgen`-based Rust FFI is possible but is more upfront work and is deferred.
The Tauri Rust core only supervises the sidecar.

### 3.2 Tauri app (Rust core + Svelte 5 frontend)
- A single **control-panel** window (not the overlay).
- Spawns and supervises the sidecar binary via `tauri-plugin-shell` (sidecar).
- Sends commands to the sidecar: camera selection, effect toggles/strength, overlay
  shape/size/position/opacity, click-through, hotkeys.
- Receives status from the sidecar: FPS, gaze state, errors, available cameras.
- Persists user settings (Tauri store / JSON config) and restores them on launch.

### 3.3 IPC
- **Command/status channel:** JSON lines over the sidecar's stdio, marshalled by the
  Rust core. Small, low-frequency messages only.
- **Video:** none across IPC — the sidecar renders the overlay natively.

## 4. Overlay window behavior

- Borderless; `WS_EX_LAYERED | WS_EX_TOPMOST`; optional `WS_EX_TRANSPARENT` for
  click-through (toggle).
- Per-pixel alpha so the green-screened subject shows with a transparent background.
- **Drag** anywhere on the overlay to move it (no title bar).
- **Resize** via corner handles and/or scroll-to-scale.
- **Snap** to screen corners/edges.
- **Shape mask:** full frame / rounded-rect / circle.
- Multi-monitor aware.
- Remembers last position and size across sessions.

## 5. Control panel (Svelte 5 + shadcn-svelte + Tailwind)

- Camera device selector (enumerated by the sidecar).
- AI Green Screen: on/off + strength.
- Eye Contact: on/off + sensitivity + look-away range (how far off-axis before it
  transitions back to real eyes).
- Optional: Denoise, auto-framing (if exposed by SDK; nice-to-have, not required).
- Gaze status indicator (live: on-camera / redirected / real-eyes).
- Overlay controls: shape, size, opacity, click-through, lock position, mirror.
- Hotkey configuration.
- Start/stop, FPS readout, GPU status.
- Settings persisted via Tauri store.

UI built with Svelte 5 runes; components from shadcn-svelte; styling via Tailwind.

## 6. Data flow

```
Webcam ──(Media Foundation)──> Sidecar
                                  │
                       Maxine: AI Green Screen ──> Eye Contact ──> (Denoise?)
                                  │
                          D3D11 render ──> layered topmost overlay window
                                                      │
                                          captured by any screen recorder

Control:  Svelte ──> Tauri Rust core ──(JSON/stdin)──> Sidecar
Status:   Sidecar ──(JSON/stdout)──> Tauri Rust core ──> Svelte
```

## 7. Error handling

- **No / unsupported GPU:** clear message that Eye Contact requires an RTX GPU; refuse
  to start the Maxine chain rather than crash.
- **Camera in use / none found:** surfaced in the control panel; retry/refresh.
- **Maxine model load failure:** report the failing effect; allow disabling it.
- **Sidecar crash:** the Rust supervisor restarts it (bounded retries) and reports status.
- **Recorder cannot capture topmost/layered window:** documented; recommend display
  capture as the fallback in such recorders.

## 8. Milestones

- **M1** — Tauri shell + Svelte control-panel skeleton + sidecar spawn/supervise + JSON
  command/status channel (no video yet).
- **M2** — C++ sidecar: Media Foundation capture + D3D11 transparent topmost draggable
  overlay with raw passthrough (no Maxine yet).
- **M3** — Integrate Maxine AI Green Screen.
- **M4** — Integrate Maxine Eye Contact + gaze status indicator.
- **M5** — Overlay UX (shape, resize, snap, hotkeys, persistence) + error states + polish.

## 9. Risks

- **Maxine SDK redistribution license** — confirm distribution terms and bundled-model
  redistribution before any public release.
- **Model/bundle size** — Maxine models add significant installer weight.
- **Recorder capture of topmost layered windows** — verify against target recorders early
  (M2); fall back to display capture where unsupported.
- **Windows build toolchain** — MSVC + Maxine SDK dependencies; CI build of the C++
  sidecar must be set up.
- **Frame sync / perf** — expected fine on RTX 3090, but validate end-to-end latency.

## 10. Testing

- **Rust core:** unit tests for config (de)serialization, sidecar supervisor/restart
  logic, command/status serde.
- **Sidecar:** smoke test that loads Maxine and processes a synthetic frame; capture +
  render passthrough check.
- **Svelte:** component tests (vitest) for control-panel state and command emission.
- **Manual / integration:** overlay captured by OBS; drag + resize + snap; multi-monitor;
  eye-contact transition at look-away boundary.

## 11. Component boundaries

- **Sidecar** — input: JSON commands + webcam; output: rendered overlay + JSON status.
  Replaceable without touching the frontend.
- **Rust core** — input: Svelte invokes + sidecar status; output: sidecar commands +
  events to Svelte. Pure supervision + marshalling; no video.
- **Svelte frontend** — input: user actions + status events; output: invoke calls. Pure
  UI; no direct SDK or window-management knowledge.
