#!/usr/bin/env bash
# Install + enable the KWin overlay-teleport script (Wayland/KDE only, #53).
# Re-runnable: upgrades in place. Shortcut default Ctrl+Alt+B, rebindable in
# System Settings > Shortcuts > KWin ("Camera overlay: teleport to cursor").
set -euo pipefail
pkg="$(dirname "$0")/kwin/cameraoverlay-teleport"

kpackagetool6 --type KWin/Script -u "$pkg" 2>/dev/null || kpackagetool6 --type KWin/Script -i "$pkg"
kwriteconfig6 --file kwinrc --group Plugins --key cameraoverlay-teleportEnabled true

# Live activation order matters (cost a debug cycle): reconfigure makes KWin reparse
# kwinrc (sees the Plugins flag), then Scripting.start loads newly-enabled scripts through
# the native path — the only one where registerShortcut actually wires the global shortcut.
# (Manual Scripting.loadScript runs the code but its shortcut never fires.)
dbus-send --session --print-reply --dest=org.kde.KWin /Scripting \
  org.kde.kwin.Scripting.unloadScript string:cameraoverlay-teleport >/dev/null 2>&1 || true
dbus-send --session --dest=org.kde.KWin /KWin org.kde.KWin.reconfigure
sleep 1
dbus-send --session --print-reply --dest=org.kde.KWin /Scripting \
  org.kde.kwin.Scripting.start >/dev/null
loaded=$(dbus-send --session --print-reply --dest=org.kde.KWin /Scripting \
  org.kde.kwin.Scripting.isScriptLoaded string:cameraoverlay-teleport 2>/dev/null | awk '/boolean/{print $2}')
echo "OK: KWin teleport script installed, loaded=$loaded (Ctrl+Alt+B)"
