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

# ONNX green-screen runtime (issue #24): ORT + selfie-segmentation model -> $OUT/onnx
# (the shim's bundled tier; COS_SEG_RUNTIME_DIR overrides in dev). Pinned + sha-verified;
# cached under ~/.cache so rebuilds cost nothing.
ORT_VER="1.28.0"
ORT_SHA="a3e1b79d7bb1bf09696ce675f49e4064e6c81f6202b8225624fff0e93f8d6407"
MODEL_SHA="de212dabbc6266f0047711d1dfae80900f7b596b9ed5f7665f3d1cf68c5443ee"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/camera-on-screen"
mkdir -p "$CACHE" "$OUT/onnx"
[ -f "$CACHE/ort-$ORT_VER.tgz" ] || curl -fsSL -o "$CACHE/ort-$ORT_VER.tgz" \
  "https://github.com/microsoft/onnxruntime/releases/download/v$ORT_VER/onnxruntime-linux-x64-$ORT_VER.tgz"
echo "$ORT_SHA  $CACHE/ort-$ORT_VER.tgz" | sha256sum -c - >/dev/null
[ -f "$CACHE/pinto109.tar.gz" ] || curl -fsSL -o "$CACHE/pinto109.tar.gz" \
  "https://s3.ap-northeast-2.wasabisys.com/pinto-model-zoo/109_Selfie_Segmentation/resources.tar.gz"
tar -xzf "$CACHE/ort-$ORT_VER.tgz" -C "$CACHE" \
  "onnxruntime-linux-x64-$ORT_VER/lib/libonnxruntime.so.$ORT_VER" \
  "onnxruntime-linux-x64-$ORT_VER/LICENSE" "onnxruntime-linux-x64-$ORT_VER/ThirdPartyNotices.txt"
tar -xzf "$CACHE/pinto109.tar.gz" -C "$CACHE" saved_model_tflite_tfjs_tftrt_onnx_coreml/model_float32.onnx
echo "$MODEL_SHA  $CACHE/saved_model_tflite_tfjs_tftrt_onnx_coreml/model_float32.onnx" | sha256sum -c - >/dev/null
cp "$CACHE/onnxruntime-linux-x64-$ORT_VER/lib/libonnxruntime.so.$ORT_VER" "$OUT/onnx/libonnxruntime.so.1"
cp "$CACHE/onnxruntime-linux-x64-$ORT_VER/LICENSE" "$OUT/onnx/ONNXRUNTIME-LICENSE"
cp "$CACHE/onnxruntime-linux-x64-$ORT_VER/ThirdPartyNotices.txt" "$OUT/onnx/ONNXRUNTIME-ThirdPartyNotices.txt"
cp "$CACHE/saved_model_tflite_tfjs_tftrt_onnx_coreml/model_float32.onnx" "$OUT/onnx/selfie_segmentation.onnx"
# The PINTO tarball ships no LICENSE file (verified: `tar tzf` has no LICENSE/README entry) —
# vendor the Apache-2.0 text the model is licensed under (native/shim/bundle/APACHE-2.0.txt,
# already carrying the PINTO/Katsuya Hyodo copyright notice — same copyright holder as the
# selfie-segmentation ONNX conversion per THIRD-PARTY-NOTICES.md).
cp native/shim/bundle/APACHE-2.0.txt "$OUT/onnx/MODEL-LICENSE"
cp THIRD-PARTY-NOTICES.md "$OUT/"
# Desktop-entry icon: cos.png is an AvaloniaResource (embedded in the assembly), so the tree
# would otherwise carry no image file for scripts/install.sh's .desktop Icon= to point at.
cp cos.png "$OUT/"

if strings "$OUT/libCameraOnScreen.Shim.so" | grep -q "not built in"; then
  echo "ERROR: stub shim deployed (deploy-the-right-shim)" >&2; exit 1
fi
echo "OK: $OUT/CameraOnScreen.App.Avalonia (no COS_* env vars needed)"

if [ "$TAR" = 1 ]; then
  VER=$(git describe --tags --always 2>/dev/null || echo dev)
  PKG="$(dirname "$OUT")/CameraOnScreen-$VER-linux-x64.tar.zst"
  tar -C "$OUT" --use-compress-program='zstd -T0 -12' -cf "$PKG" .
  # scripts/install.sh depends on three properties of this asset that nothing else checks:
  # packed FLAT (no top-level directory), cos.png at the root, and the linux-x64.tar.zst suffix
  # its GitHub-API match greps for. Breaking any one of them breaks end-user installs silently
  # (#65 — cos.png was absent for a whole release and the install still reported success), so
  # assert them here, against the produced artifact, rather than trusting the lines above.
  case "$PKG" in
    *-linux-x64.tar.zst) ;;
    *) echo "ERROR: asset name must end in -linux-x64.tar.zst (got $(basename "$PKG"))" >&2; exit 1 ;;
  esac
  LIST="$(tar --zstd -tf "$PKG")"
  for want in "./CameraOnScreen.App.Avalonia" "./cos.png" "./THIRD-PARTY-NOTICES.md"; do
    printf '%s\n' "$LIST" | grep -qxF "$want" \
      || { echo "ERROR: $want missing from the root of $PKG (not packed flat?)" >&2; exit 1; }
  done
  echo "OK: $PKG ($(stat -c %s "$PKG" | awk '{printf "%.2f GiB", $1/1073741824}'))"
fi
