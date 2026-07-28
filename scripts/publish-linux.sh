#!/usr/bin/env bash
# Self-contained Linux build of the Avalonia panel with the SDK shim and the bundled-tier
# maxine/ layout, launchable with NO env vars: <shimdir>/maxine/{vfx,ar} is the resolvers'
# last tier. maxine/ is the pruned redistributable bundle (bundle-maxine-linux.sh:
# DT_NEEDED closure + models + licenses; reflink copies, so rebuilds cost seconds).
#
# Usage: scripts/publish-linux.sh [outdir] [--tar]   (default dist/linux; --tar also writes
# CameraOnScreen-<git describe>-linux-x64.tar.zst beside outdir — zstd -12 keeps the 3.6 GB
# tree at ~1.85 GiB, under GitHub's 2 GiB release-asset cap; default zstd was a 7 MB squeak)
set -euo pipefail
cd "$(dirname "$0")/.."

VFX="${COS_VFX_SDK_DIR:-$HOME/dev/VideoFX-linux/VideoFX}"
AR="${COS_AR_SDK_DIR:-$HOME/dev/ARSDK-linux/ARSDK}"
FRUC="${COS_FRUC_SDK_DIR:-$HOME/dev/Optical_Flow_SDK_5.0.7}"   # optional; absent = FRUC greys out
OUT="dist/linux"; TAR=0
for a in "$@"; do case "$a" in --tar) TAR=1 ;; *) OUT="$a" ;; esac; done

[ -f "$VFX/include/nvVideoEffects.h" ] || { echo "ERROR: VFX SDK-core tree not found at $VFX" >&2; exit 1; }
[ -f "$AR/include/nvAR.h" ] || { echo "ERROR: AR SDK-core tree not found at $AR" >&2; exit 1; }

# Deploy-the-right-shim: build the SDK config LAST so it is what ships.
COS_VFX_SDK_DIR="$VFX" COS_AR_SDK_DIR="$AR" COS_FRUC_SDK_DIR="$FRUC" \
  cmake -S native/shim -B native/shim/build
cmake --build native/shim/build

dotnet publish src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj \
  -c Release -r linux-x64 --self-contained -o "$OUT" --nologo

# The publish carries whatever .so the csproj copied; overwrite with the fresh SDK build.
cp native/shim/build/libCameraOnScreen.Shim.so "$OUT/"

COS_VFX_SDK_DIR="$VFX" COS_AR_SDK_DIR="$AR" COS_FRUC_SDK_DIR="$FRUC" \
  scripts/bundle-maxine-linux.sh "$OUT/maxine" | tail -2

if strings "$OUT/libCameraOnScreen.Shim.so" | grep -q "not built in"; then
  echo "ERROR: stub shim deployed (deploy-the-right-shim)" >&2; exit 1
fi
echo "OK: $OUT/CameraOnScreen.App.Avalonia (no COS_* env vars needed)"

if [ "$TAR" = 1 ]; then
  VER=$(git describe --tags --always 2>/dev/null || echo dev)
  PKG="$(dirname "$OUT")/CameraOnScreen-$VER-linux-x64.tar.zst"
  tar -C "$OUT" --use-compress-program='zstd -T0 -12' -cf "$PKG" .
  echo "OK: $PKG ($(stat -c %s "$PKG" | awk '{printf "%.2f GiB", $1/1073741824}'))"
fi
