#!/usr/bin/env bash
# One-command installer for the Camera-on-Screen Linux build.
#
#   curl -fsSL https://raw.githubusercontent.com/officialdad/camera-on-screen/main/scripts/install.sh | bash
#
# Re-running replaces an existing install — that is also the upgrade path.
# No sudo, no PATH changes; everything lands under $HOME.
#
# Reads NOTHING from stdin: under `curl | bash` this script IS stdin, so a stray `read` would
# eat the rest of itself.
#
# Env overrides (used by the smoke check, not by end users):
#   COS_INSTALL_DIR      destination instead of ~/.local/share/camera-on-screen
#   COS_INSTALL_TARBALL  install from a local tarball instead of downloading a release
set -euo pipefail

REPO="officialdad/camera-on-screen"
EXE="CameraOnScreen.App.Avalonia"
DEST="${COS_INSTALL_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/camera-on-screen}"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"

# DEST gets rm -rf'd below — refuse anything that isn't a safe, absolute, non-home path.
case "$DEST" in
  /*) ;;
  *) echo "ERROR: COS_INSTALL_DIR must be an absolute path (got '$DEST')." >&2; exit 1 ;;
esac
if [ "$DEST" = "/" ] || [ "$DEST" = "$HOME" ]; then
  echo "ERROR: refusing to install into '$DEST'." >&2
  exit 1
fi

for tool in curl tar zstd; do
  command -v "$tool" >/dev/null 2>&1 \
    || { echo "ERROR: '$tool' is required but not installed." >&2; exit 1; }
done
# --zstd landed in GNU tar 1.31; older tars fail mid-extract instead of up front.
tar --help 2>/dev/null | grep -q -- '--zstd' \
  || { echo "ERROR: your tar does not support --zstd (needs GNU tar 1.31 or newer)." >&2; exit 1; }

# Overwriting a running install leaves a half-swapped tree behind the running process.
if pgrep -f "$EXE" >/dev/null 2>&1; then
  echo "ERROR: Camera-on-Screen is running. Quit it (tray icon -> Quit), then re-run this." >&2
  exit 1
fi

# mktemp -d defaults to /tmp, which is a RAM-backed tmpfs on systemd-default distros — a
# ~1.9 GB download would eat 1.9 GB of RAM there and can fail after a long download on a
# tmpfs capped at 50% of RAM. Put TMP beside DEST instead, on disk, and on the same
# filesystem so the later extract isn't a cross-device copy. DEST's parent may not exist
# yet on a first install.
mkdir -p "$(dirname "$DEST")"
TMP="$(mktemp -d -p "$(dirname "$DEST")")"
trap 'rm -rf "$TMP"' EXIT

if [ -n "${COS_INSTALL_TARBALL:-}" ]; then
  TARBALL="$COS_INSTALL_TARBALL"
  echo "Installing from $TARBALL"
else
  echo "Finding the latest release..."
  # The asset name carries the version, so there is no fixed /releases/latest/download/ URL.
  # `|| true`: under pipefail, a no-match grep (or a failed curl) makes this whole
  # assignment fail and set -e would abort here, before the check below ever runs.
  URL="$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" \
    | grep -o '"browser_download_url": *"[^"]*linux-x64\.tar\.zst"' \
    | head -1 | cut -d'"' -f4)" || true
  [ -n "$URL" ] || { echo "ERROR: no Linux release asset found for $REPO." >&2; exit 1; }
  echo "Downloading $(basename "$URL") — about 1.9 GB, this takes a while..."
  TARBALL="$TMP/cos.tar.zst"
  curl -fL --progress-bar "$URL" -o "$TARBALL"
fi

echo "Installing to $DEST ..."
rm -rf "$DEST"
mkdir -p "$DEST"
# The release tarball is packed flat (tar -C "$OUT" -cf "$PKG" .), so there is no top dir.
tar -C "$DEST" --zstd -xf "$TARBALL"
[ -f "$DEST/$EXE" ] || { echo "ERROR: '$EXE' missing from the archive." >&2; exit 1; }
chmod +x "$DEST/$EXE"

mkdir -p "$APPS"
cat > "$APPS/camera-on-screen.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Camera-on-Screen
Comment=Always-on-top webcam overlay for recording and screen sharing
Exec=$DEST/$EXE
Icon=$DEST/cos.png
Terminal=false
Categories=AudioVideo;Video;
EOF

cat <<EOF

Done. Launch "Camera-on-Screen" from your application menu, or run:
  $DEST/$EXE

To uninstall:
  rm -rf "$DEST" "$APPS/camera-on-screen.desktop"
EOF
