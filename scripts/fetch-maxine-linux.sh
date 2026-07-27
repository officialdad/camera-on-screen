#!/bin/bash
# Fetch the Maxine Linux runtime libs + TensorRT engines from the NGC nvidia/maxine model
# registry (Linux twin of fetch-maxine-engines.ps1), sha256-verified. The SDK *zips* on NGC
# are NVAIE-gated, but the models registry (engines + lib_linux runtime tarballs) is
# accessible with a plain NGC personal key (NVIDIA_API_KEY, nvapi-..., Bearer auth) —
# same co-versioned pair as Windows: VFX 1.2.0.0 + AR 1.1.1.0.
#
# Usage: NVIDIA_API_KEY=nvapi-... scripts/fetch-maxine-linux.sh [outdir] [archs]
#   outdir default: ~/dev/maxine-linux-stage
#   archs  default: "86" (this box's RTX 3090); pass "75 86 89 100" for the bundle set
set -euo pipefail

OUT="${1:-$HOME/dev/maxine-linux-stage}"
ARCHS="${2:-86}"
[ -n "${NVIDIA_API_KEY:-}" ] || { echo "NVIDIA_API_KEY not set" >&2; exit 1; }
BASE="https://api.ngc.nvidia.com/v2/org/nvidia/team/maxine/models"
AUTH="Authorization: Bearer $NVIDIA_API_KEY"

# name|version|kind(dst subdir for engines)
MODELS="
nvvfxgreenscreen|1.2.0.0|vfx
nvargazeredirection|1.1.1.0|ar
nvarfaceboxdetection|1.1.1.0|ar
nvarlandmarkdetection|1.1.1.0|ar
"

download_version() { # $1=model $2=versionId $3=dstdir
    local model="$1" vid="$2" dst="$3"
    mkdir -p "$dst"
    curl -sf -H "$AUTH" "$BASE/$model/$vid/files" -o "$dst/.listing.json" \
        || { echo "  SKIP $model/$vid (no access/absent)"; return 0; }
    python3 - "$dst" <<'PYEOF'
import base64, hashlib, json, os, subprocess, sys
dst = sys.argv[1]
d = json.load(open(os.path.join(dst, ".listing.json")))
fps, urls, shas = d["filepath"], d["urls"], d.get("sha256_base64", [])
if isinstance(fps, str): fps, urls, shas = [fps], [urls], [shas]
for i, fp in enumerate(fps):
    out = os.path.join(dst, os.path.basename(fp))
    want = base64.b64decode(shas[i]).hex() if i < len(shas) and shas[i] else None
    if os.path.exists(out) and want and hashlib.sha256(open(out, "rb").read()).hexdigest() == want:
        print(f"  have {os.path.basename(fp)}"); continue
    print(f"  get  {os.path.basename(fp)}")
    subprocess.run(["curl", "-sfL", "-o", out, urls[i]], check=True)
    if want:
        got = hashlib.sha256(open(out, "rb").read()).hexdigest()
        if got != want: sys.exit(f"sha256 MISMATCH for {fp}")
os.remove(os.path.join(dst, ".listing.json"))
PYEOF
}

echo "$MODELS" | while IFS='|' read -r name ver kind; do
    [ -n "$name" ] || continue
    echo "== $name $ver (runtime libs)"
    download_version "$name" "${ver}_lib_linux" "$OUT/lib/$name"
    for a in $ARCHS; do
        echo "== $name $ver (engines sm$a)"
        download_version "$name" "${ver}_models_linux_sm$a" "$OUT/models/$kind/sm$a"
    done
done
echo "done -> $OUT"
