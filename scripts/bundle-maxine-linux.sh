#!/usr/bin/env bash
# Prune the two Maxine Linux SDK-core trees into a redistributable <out>/maxine/{vfx,ar}
# bundle (issue #27 Phase 5) — the Linux twin of bundle-maxine.ps1. The lib set is the
# DT_NEEDED closure of the shim's entry libs, computed here at bundle time (readelf walk)
# instead of a static manifest, plus the cuDNN/TRT libs those load via dlopen (never in
# DT_NEEDED). Triton variants and CUDA link-time stubs/ are excluded (same rules as
# PreloadMaxineClosure). Shared CUDA/TRT libs land only under vfx/ — at runtime the
# preload dedups basenames across trees and the VFX copies must win (AR binds VFX's
# libVideoFXLocal internals), so shipping AR's byte-identical copies would waste 3.5 GB.
# Files are named by SONAME (no symlink chains); dlopen resolves loaded libs by ELF
# SONAME, not filename. Models (sm86 engines) + license files + ThirdPartyLicenses ship;
# headers, installer helpers, and .so dev symlinks do not.
#
# Usage: scripts/bundle-maxine-linux.sh <outdir>   (creates <outdir>/{vfx,ar})
# Env: COS_VFX_SDK_DIR / COS_AR_SDK_DIR override the SDK-core tree locations.
set -euo pipefail

VFX="${COS_VFX_SDK_DIR:-$HOME/dev/VideoFX-linux/VideoFX}"
AR="${COS_AR_SDK_DIR:-$HOME/dev/ARSDK-linux/ARSDK}"
OUT="${1:?usage: bundle-maxine-linux.sh <outdir>}"

[ -f "$VFX/lib/libVideoFXLocal.so.1" ] || { echo "ERROR: VFX SDK-core tree not found at $VFX" >&2; exit 1; }
[ -f "$AR/lib/libnvARPoseLocal.so.1" ] || { echo "ERROR: AR SDK-core tree not found at $AR" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"

VFX_TREE="$VFX" AR_TREE="$AR" OUT_DIR="$OUT" python3 - <<'PY'
import os, shutil, subprocess, sys

VFX, AR, OUT = os.environ["VFX_TREE"], os.environ["AR_TREE"], os.environ["OUT_DIR"]

# What the shim actually enters through: dispatchers + local impls + NVCVImage + the
# green-screen feature and the 3 AR gaze-closure features (gaze needs landmark+facebox,
# per the SDK *_dependencies.txt; nvARFaceExpressions stays excluded — AI terms §8.17).
ENTRY = [
    "libVideoFX.so.1", "libVideoFXLocal.so.1", "libNVCVImage.so.1",
    "libnvVFXGreenScreen.so",
    "libnvARPose.so.1", "libnvARPoseLocal.so.1",
    "libnvARGazeRedirection.so", "libnvARFaceBoxDetection.so", "libnvARLandmarkDetection.so",
]
# Loaded via dlopen at runtime, invisible to readelf: cuDNN 9's sub-libraries and the
# TRT plugin/cuDNN hookup. Seeded only if the parent lands in the closure.
DLOPEN_EXTRAS = {
    "libcudnn.so.9": [
        "libcudnn_adv.so.9", "libcudnn_cnn.so.9", "libcudnn_graph.so.9",
        "libcudnn_ops.so.9", "libcudnn_heuristic.so.9",
        "libcudnn_engines_precompiled.so.9", "libcudnn_engines_runtime_compiled.so.9",
    ],
    "libnvinfer.so.10": ["libnvinfer_plugin.so.10", "libcudnn.so.9"],
}

# Index every candidate .so by name, VFX first (= the runtime first-load-wins order).
index = {}
for tree in (VFX, AR):
    for root, dirs, files in os.walk(tree):
        dirs[:] = [d for d in dirs if d != "stubs"]
        for f in files:
            if ".so" in f and "Triton" not in f and f not in index:
                index[f] = os.path.join(root, f)

def dyn(path, tag):
    out = subprocess.run(["readelf", "-d", path], capture_output=True, text=True, check=True).stdout
    return [l.split("[")[1].rstrip("]") for l in out.splitlines() if tag in l]

seen, system, queue = {}, [], list(ENTRY)  # seen: realpath -> requested name
while queue:
    name = queue.pop()
    path = index.get(name)
    if not path:
        if name not in system:
            system.append(name)  # libc/libstdc++/zlib/driver — the end user's problem, by design
        continue
    real = os.path.realpath(path)
    if real in seen:
        continue
    seen[real] = name
    queue += dyn(real, "(NEEDED)") + DLOPEN_EXTRAS.get(name, [])

def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    subprocess.run(["cp", "--reflink=auto", "-p", src, dst], check=True)

total = 0
for real in sorted(seen):
    tree, sub = (VFX, "vfx") if real.startswith(VFX) else (AR, "ar")
    rel = os.path.relpath(os.path.dirname(real), tree)
    # One file per lib, named by its ELF SONAME (what NEEDED/dlopen matching actually
    # uses — the dev trees' symlink chains are linker convenience, not runtime need).
    soname = (dyn(real, "(SONAME)") or [os.path.basename(real)])[0]
    copy(real, os.path.join(OUT, sub, rel, soname))
    total += os.path.getsize(real)
    print(f"  {os.path.getsize(real)/1e6:8.1f}M  {sub}/{rel}/{soname}")

# AR's own NVCVImage must ALSO ship (under its versioned name, so VFX's stays first for
# SONAME matching): NVIDIA statically embedded libstdc++ pieces into it, and
# libnvARPoseLocal binds _ZNSt12__cow_stringC1EPKcm from there — no other lib (system
# libstdc++ included) exports it. Dropping it = NvAR_CudaStreamCreate fails NVCV_ERR_LIBRARY.
ar_nvcv = os.path.realpath(os.path.join(AR, "lib/libNVCVImage.so.1"))
copy(ar_nvcv, os.path.join(OUT, "ar/lib", os.path.basename(ar_nvcv)))
print(f"  {os.path.getsize(ar_nvcv)/1e6:8.1f}M  ar/lib/{os.path.basename(ar_nvcv)}  (AR-private libstdc++ symbols)")

# Models, license files, third-party notices — whole dirs, they are small next to the libs.
for tree, sub in ((VFX, "vfx"), (AR, "ar")):
    for rel in ("lib/models", "share/external"):
        if os.path.isdir(os.path.join(tree, rel)):
            shutil.copytree(os.path.join(tree, rel), os.path.join(OUT, sub, rel),
                            copy_function=lambda s, d: copy(s, d))
    for root, dirs, files in os.walk(os.path.join(tree, "features")):
        if os.path.basename(root) == "license":
            for f in files:
                rel = os.path.relpath(root, tree)
                copy(os.path.join(root, f), os.path.join(OUT, sub, rel, f))
    for f in os.listdir(os.path.join(tree, "share")):
        if f.endswith(".pdf"):
            copy(os.path.join(tree, "share", f), os.path.join(OUT, sub, "share", f))

print(f"closure: {len(seen)} libs, {total/1e9:.2f} GB; system deps left to the host: {sorted(set(system))}")
PY

echo "OK: $OUT"
