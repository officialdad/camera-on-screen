#!/usr/bin/env bash
# Install + enable the KWin overlay-teleport script (Wayland/KDE only, #53).
# Re-runnable: upgrades in place. Shortcut default Ctrl+Alt+O, rebindable in
# System Settings > Shortcuts > KWin ("Camera overlay: teleport to cursor").
set -euo pipefail
pkg="$(dirname "$0")/kwin/cameraoverlay-teleport"

kpackagetool6 --type KWin/Script -u "$pkg" 2>/dev/null || kpackagetool6 --type KWin/Script -i "$pkg"
kwriteconfig6 --file kwinrc --group Plugins --key cameraoverlay-teleportEnabled true
dbus-send --session --dest=org.kde.KWin /KWin org.kde.KWin.reconfigure
echo "OK: KWin teleport script installed + enabled (Ctrl+Alt+O)"
