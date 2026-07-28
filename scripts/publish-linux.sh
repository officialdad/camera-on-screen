#!/usr/bin/env bash
# Self-contained Linux build of the Avalonia panel with the SDK shim and the bundled-tier
# maxine/ layout, launchable with NO env vars: <shimdir>/maxine/{vfx,ar} is the resolvers'
# last tier. Dev-box convenience — maxine/ are SYMLINKS to the SDK-core trees, not a
# redistributable bundle (pruned closure + licenses = Phase 5).
#
# Usage: scripts/publish-linux.sh [outdir]   (default dist/linux)
set -euo pipefail
cd "$(dirname "$0")/.."

VFX="${COS_VFX_SDK_DIR:-$HOME/dev/VideoFX-linux/VideoFX}"
AR="${COS_AR_SDK_DIR:-$HOME/dev/ARSDK-linux/ARSDK}"
OUT="${1:-dist/linux}"

[ -f "$VFX/include/nvVideoEffects.h" ] || { echo "ERROR: VFX SDK-core tree not found at $VFX" >&2; exit 1; }
[ -f "$AR/include/nvAR.h" ] || { echo "ERROR: AR SDK-core tree not found at $AR" >&2; exit 1; }

# Deploy-the-right-shim: build the SDK config LAST so it is what ships.
COS_VFX_SDK_DIR="$VFX" COS_AR_SDK_DIR="$AR" cmake -S native/shim -B native/shim/build
cmake --build native/shim/build

dotnet publish src/CameraOnScreen.App.Avalonia/CameraOnScreen.App.Avalonia.csproj \
  -c Release -r linux-x64 --self-contained -o "$OUT" --nologo

# The publish carries whatever .so the csproj copied; overwrite with the fresh SDK build.
cp native/shim/build/libCameraOnScreen.Shim.so "$OUT/"

mkdir -p "$OUT/maxine"
ln -sfn "$VFX" "$OUT/maxine/vfx"
ln -sfn "$AR" "$OUT/maxine/ar"

if strings "$OUT/libCameraOnScreen.Shim.so" | grep -q "not built in"; then
  echo "ERROR: stub shim deployed (deploy-the-right-shim)" >&2; exit 1
fi
echo "OK: $OUT/CameraOnScreen.App.Avalonia (no COS_* env vars needed)"
