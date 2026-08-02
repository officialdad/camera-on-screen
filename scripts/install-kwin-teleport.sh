#!/usr/bin/env bash
# Install + enable the KWin overlay-teleport script (Wayland/KDE only, #53).
# Re-runnable: upgrades in place. Shortcut default Ctrl+Alt+B, rebindable in
# System Settings > Shortcuts > KWin ("Camera overlay: teleport to cursor").
set -euo pipefail
pkg="$(dirname "$0")/kwin/cameraoverlay-teleport"

kpackagetool6 --type KWin/Script -u "$pkg" 2>/dev/null || kpackagetool6 --type KWin/Script -i "$pkg"
kwriteconfig6 --file kwinrc --group Plugins --key cameraoverlay-teleportEnabled true

# Load AND run it in the live session (enabled scripts otherwise only start at next login).
# Gotcha (cost a debug cycle): Scripting.start is a no-op for scripts loaded after KWin
# startup — the per-script org.kde.kwin.Script.run is what actually executes it.
dbus-send --session --print-reply --dest=org.kde.KWin /Scripting \
  org.kde.kwin.Scripting.unloadScript string:cameraoverlay-teleport >/dev/null 2>&1 || true
id=$(dbus-send --session --print-reply --dest=org.kde.KWin /Scripting \
  org.kde.kwin.Scripting.loadScript \
  string:"$HOME/.local/share/kwin/scripts/cameraoverlay-teleport/contents/code/main.js" \
  string:cameraoverlay-teleport 2>/dev/null | awk '/int32/{print $2}')
dbus-send --session --print-reply --dest=org.kde.KWin "/Scripting/Script$id" \
  org.kde.kwin.Script.run >/dev/null
echo "OK: KWin teleport script installed + running (Ctrl+Alt+B)"
