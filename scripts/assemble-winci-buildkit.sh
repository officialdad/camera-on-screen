#!/bin/bash
# Assemble the PRIVATE Windows-CI Maxine build kit (issue #38 option 1): the header +
# proxy-source subset that lets hosted windows-latest CI compile the shim's SDK config
# (COS_HAS_MAXINE + COS_HAS_MAXINE_AR). No runtime DLLs, no models — compile-only, so
# no GPU and no NGC at CI time. Sources are the SDK trees already on the dev box:
#   VFX 1.2.0.0 Linux SDK-core  (core headers, greenscreen header, proxy sources — OS-shared)
#   Maxine-VFX-SDK GitHub clone (MIT; nvTransferD3D{,11}.h — absent from the Linux tree,
#                                signatures verified identical to the 1.2 proxy's wrappers)
#   Maxine-AR-SDK GitHub clone  (AR 1.1.1.0 headers + nvARProxy.cpp — same tree the old
#                                Windows box built against)
#   Optical Flow SDK 5.0.7      (NvOFFRUC.h only — FRUC compile; DesignWorks license,
#                                developer-program-gated so no anonymous CI fetch)
# nvVFXVideoSuperRes.h is a COMPILE-COMPAT STAND-IN authored below (the real header ships
# only in the Windows VSR feature install, which died with the old dev box; NGC SDK zips
# are NVAIE-gated). Replace it with the real header when a Windows SDK install exists.
#
# The kit is NVIDIA-proprietary material -> push ONLY to the private repo
# officialdad/maxine-winci-buildkit. Never commit it to camera-on-screen.
#
# Usage: scripts/assemble-winci-buildkit.sh [outdir]
set -euo pipefail

OUT="${1:-$HOME/dev/maxine-winci-buildkit}"
VFX_LINUX="$HOME/dev/VideoFX-linux/VideoFX"
VFX_CLONE="$HOME/dev/Maxine-VFX-SDK"
AR_CLONE="$HOME/dev/Maxine-AR-SDK"
FRUC_SDK="$HOME/dev/Optical_Flow_SDK_5.0.7"

for d in "$VFX_LINUX" "$VFX_CLONE" "$AR_CLONE" "$FRUC_SDK"; do
    [ -d "$d" ] || { echo "missing SDK tree: $d" >&2; exit 1; }
done

# Clean kit payload (keep .git) so removed files don't linger across re-assembles.
rm -rf "$OUT/vfx" "$OUT/ar" "$OUT/fruc" "$OUT/README.md"
mkdir -p "$OUT/vfx/nvvfx/include" "$OUT/vfx/nvvfx/src" \
         "$OUT/vfx/features/nvvfxgreenscreen/include" \
         "$OUT/vfx/features/nvvfxvideosuperres/include" \
         "$OUT/ar/nvar/include" "$OUT/ar/nvar/src" \
         "$OUT/fruc/NvOFFRUC/Interface"

cp "$VFX_LINUX"/include/nvVideoEffects.h "$VFX_LINUX"/include/nvCVImage.h \
   "$VFX_LINUX"/include/nvCVStatus.h "$OUT/vfx/nvvfx/include/"
cp "$VFX_CLONE"/nvvfx/include/nvTransferD3D.h "$VFX_CLONE"/nvvfx/include/nvTransferD3D11.h \
   "$OUT/vfx/nvvfx/include/"
cp "$VFX_LINUX"/share/nvvfx/src/nvVideoEffectsProxy.cpp \
   "$VFX_LINUX"/share/nvvfx/src/nvCVImageProxy.cpp "$OUT/vfx/nvvfx/src/"
cp "$VFX_LINUX"/features/nvvfxgreenscreen/include/nvVFXGreenScreen.h \
   "$OUT/vfx/features/nvvfxgreenscreen/include/"

cat > "$OUT/vfx/features/nvvfxvideosuperres/include/nvVFXVideoSuperRes.h" <<'EOF'
// COMPILE-COMPAT STAND-IN for the NVIDIA VSR feature header (nvvfxvideosuperres).
// The real header ships only via the Windows VFX SDK feature install (install_feature.ps1),
// which died with the old dev box — issue #38. This defines exactly what superres.cpp
// consumes: the effect selector + the quality-level parameter selector (CLAUDE.md, VFX
// feature catalog). Used ONLY by the hosted Windows CI compile job; never shipped.
// The NVVFX_QUALITY_LEVEL string value is best-effort — REPLACE THIS FILE with the real
// header before trusting any runtime behavior.
#ifndef NVVFXVIDEOSUPERRES_H_COMPAT
#define NVVFXVIDEOSUPERRES_H_COMPAT
#define NVVFX_FX_VIDEO_SUPER_RES "VideoSuperRes"
#define NVVFX_QUALITY_LEVEL "QualityLevel"
#endif
EOF

cp "$AR_CLONE"/nvar/include/nvAR.h "$AR_CLONE"/nvar/include/nvAR_defs.h \
   "$AR_CLONE"/nvar/include/nvCVImage.h "$AR_CLONE"/nvar/include/nvCVStatus.h \
   "$OUT/ar/nvar/include/"
# nvCVImageProxy.cpp also under ar/: shim.vcxproj's AR-without-VFX variant compiles
# $(CosArSdkDir)\nvar\src\nvCVImageProxy.cpp (unused while CI sets both dirs, but the
# kit shouldn't silently miss a provisioned build variant).
cp "$AR_CLONE"/nvar/src/nvARProxy.cpp "$AR_CLONE"/nvar/src/nvCVImageProxy.cpp \
   "$OUT/ar/nvar/src/"

# FRUC: the one header shim.vcxproj's COS_HAS_FRUC config includes
# ($(CosFrucSdkDir)\NvOFFRUC\Interface). cuda.h/cuda.lib come from NVIDIA's public
# cuda_cudart redist zip fetched at CI time — not kit material.
cp "$FRUC_SDK"/NvOFFRUC/Interface/NvOFFRUC.h "$OUT/fruc/NvOFFRUC/Interface/"

cat > "$OUT/README.md" <<EOF
# maxine-winci-buildkit (PRIVATE — do not make public)

Header + proxy-source subset for camera-on-screen's hosted Windows CI SDK-config compile
(issue #38 option 1). NVIDIA-proprietary material under the 2025 NVIDIA Software License —
internal use only, no redistribution. No runtime DLLs or models.

- vfx/: VFX 1.2.0.0 (core headers + proxies from the Linux SDK-core tree — OS-shared;
  nvTransferD3D{,11}.h from the MIT GitHub clone; nvVFXVideoSuperRes.h is an authored
  compile-compat stand-in, see its header comment)
- ar/:  AR 1.1.1.0 from the Maxine-AR-SDK GitHub clone @ $(git -C "$AR_CLONE" rev-parse HEAD)
- fruc/: NvOFFRUC.h from Optical Flow SDK 5.0.7 (NVIDIA DesignWorks SDK License —
  internal use only, no redistribution; separate product from Maxine VFX/AR)

Regenerate: scripts/assemble-winci-buildkit.sh in officialdad/camera-on-screen.
Consumed by: .github/workflows/ci.yml (deploy-key checkout, path .buildkit).
EOF

# Self-check: every file shim.vcxproj's SDK config compiles or includes must exist.
for f in vfx/nvvfx/include/nvVideoEffects.h vfx/nvvfx/include/nvCVImage.h \
         vfx/nvvfx/include/nvCVStatus.h vfx/nvvfx/include/nvTransferD3D.h \
         vfx/nvvfx/include/nvTransferD3D11.h vfx/nvvfx/src/nvVideoEffectsProxy.cpp \
         vfx/nvvfx/src/nvCVImageProxy.cpp \
         vfx/features/nvvfxgreenscreen/include/nvVFXGreenScreen.h \
         vfx/features/nvvfxvideosuperres/include/nvVFXVideoSuperRes.h \
         ar/nvar/include/nvAR.h ar/nvar/include/nvAR_defs.h \
         ar/nvar/include/nvCVImage.h ar/nvar/include/nvCVStatus.h \
         ar/nvar/src/nvARProxy.cpp ar/nvar/src/nvCVImageProxy.cpp \
         fruc/NvOFFRUC/Interface/NvOFFRUC.h; do
    [ -f "$OUT/$f" ] || { echo "SELF-CHECK FAILED: missing $f" >&2; exit 1; }
done
echo "kit assembled at $OUT"
