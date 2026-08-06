<p align="center">
  <img src="cos.png" alt="Camera-on-Screen" width="200">
</p>

<h1 align="center">Camera-on-Screen</h1>

<p align="center">
  I made this because I'm too lazy to do any post editing.
</p>

<p align="center">
  <a href="https://github.com/officialdad/camera-on-screen/actions/workflows/ci-linux.yml"><img src="https://github.com/officialdad/camera-on-screen/actions/workflows/ci-linux.yml/badge.svg" alt="ci-linux"></a>
  <a href="https://github.com/officialdad/camera-on-screen/actions/workflows/ci.yml"><img src="https://github.com/officialdad/camera-on-screen/actions/workflows/ci.yml/badge.svg" alt="ci"></a>
</p>

---

An always-on-top webcam overlay for **Linux and Windows**. It floats your live
camera feed over everything else, so any screen recorder or screen sharing
session captures you and your screen together in real time — no post editing.
Useful for teaching too.

AI features:

- **AI Green Screen** — removes your background
- **AI Eye Contact** — gently redirects your gaze toward the camera
- **Smooth 60 fps** — doubles the overlay's frame rate

> **What you need:** AI Eye Contact and Smooth 60 fps need an **NVIDIA RTX
> graphics card**. **AI Green Screen works on any computer.** Without an RTX
> card everything else still runs — the unavailable options simply grey out.

## Install

### Linux

One command. It downloads the latest release, installs it under your home
folder, and adds Camera-on-Screen to your application menu. No admin rights.

```bash
curl -fsSL https://raw.githubusercontent.com/officialdad/camera-on-screen/main/scripts/install.sh | bash
```

Re-run the same command later to upgrade — quit Camera-on-Screen first, since the
installer refuses to run while it's still open.

Piping a script straight into `bash` runs it before you can read it. To read it
first:

```bash
curl -fsSL -O https://raw.githubusercontent.com/officialdad/camera-on-screen/main/scripts/install.sh
less install.sh     # read it
bash install.sh
```

Needs `curl`, `tar` and `zstd`, which nearly every desktop Linux already has.
The download is about 1.9 GB — everything is bundled, so there is nothing else
to install.

To uninstall:

```bash
rm -rf ~/.local/share/camera-on-screen ~/.local/share/applications/camera-on-screen.desktop
```

### Windows

Windows installers are paused for now — the latest one is
[**v0.6.0**](https://github.com/officialdad/camera-on-screen/releases/tag/v0.6.0)
(`CameraOnScreen-Setup-0.6.0-x64.exe`). It installs **per-user** (no admin) and
bundles everything it needs. Windows SmartScreen will warn that it is unsigned:
click **More info → Run anyway**. Uninstall from **Settings → Apps**.

## Using it

- **Move it** — drag the centre **+** handle (Windows) or drag anywhere (Linux).
- **Resize it** — scroll the mouse wheel over the overlay.
- **Mirror** — toggle in the control panel.
- **Pick a camera** — switch cameras from the control panel while it is running.
- **Minimize to tray** (Linux) — closing or minimizing the control panel sends
  it to the system tray while the overlay keeps running. Click the tray icon, or
  use its **Show Panel** menu item, to bring the panel back; the tray menu can
  also start/stop the camera and quit. Turn it off with the **Minimize to tray**
  toggle in the control panel.
- **AI Green Screen** — removes your background, with adjustable edge expand and
  feather.
- **AI Eye Contact** — gently redirects your gaze toward the camera.
- **Smooth 60 fps** — doubles the overlay's frame rate.

Then record with OBS, NVIDIA ShadowPlay or Game Bar, or just share your screen —
the overlay is live in the capture, in real time.

## Settings and troubleshooting

Your preferences live in `~/.config/CameraOnScreen/config.json` on Linux and
`%LOCALAPPDATA%\CameraOnScreen\config.json` on Windows.

**The panel disappeared and there is no tray icon.** A few Linux desktops have
no system tray at all, so "minimize to tray" hides the panel with no way back.
Recover with:

```bash
pkill -f CameraOnScreen.App.Avalonia
```

then set `"MinimizeToTray": false` in `~/.config/CameraOnScreen/config.json` and
launch again. Edit the file only while the app is closed — a running app never
re-reads it.

## Under the hood

Curious how it works, or want to build it yourself? See
[`CLAUDE.md`](CLAUDE.md) for the architecture, build and runtime details, and
[`CONTRIBUTING.md`](CONTRIBUTING.md) for how to get set up and the bar pull
requests need to clear.

## Contributing

Issues and pull requests are welcome — start with
[`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

[MIT](LICENSE). The bundled NVIDIA Maxine runtime is governed separately under
the NVIDIA Maxine SDK License — see
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md). NVIDIA, Maxine, and RTX are
trademarks of NVIDIA Corporation; this project is not affiliated with NVIDIA.
