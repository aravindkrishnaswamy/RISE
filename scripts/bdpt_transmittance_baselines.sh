#!/usr/bin/env bash
#
# bdpt_transmittance_baselines.sh - capture / check rendering baselines for
# the BDPT EvalConnectionTransmittance{,NM} templatization (Phase 2c part 1,
# the first / lowest-divergence BDPT family).  Sibling of
# divergent_baselines.sh (Phase 2b) — same proven mechanism, BDPT/VCM/media
# manifest.
#
# The family under test is the connection-edge transmittance walk, so the
# load-bearing divergent path is PARTICIPATING MEDIA along connections.
# Coverage:
#   - Pel transmittance  : bdpt_homogeneous_fog (per-object/global media walk)
#   - NM  transmittance  : bdpt_homogeneous_fog_spectral (per-wavelength NM walk;
#                          new fixture — no prior scene exercised ...NM in a render)
#   - escape-Tr (Gate 4) : env_bounded_fog_bdpt  (commit 2b58236b fixture)
#   - VCM cross-integ.   : vcm_env_through_fog, env_bounded_fog_vcm
#                          (VCM calls BDPTIntegrator::EvalConnectionTransmittance)
#   - leak-check / ident : cornellbox_bdpt, cornellbox_bdpt_spectral
#                          (no media -> Tr walk returns identity; confirms the
#                           forwarder change does not perturb non-media renders)
#
# Usage:
#   bash scripts/bdpt_transmittance_baselines.sh capture <tag>  # 2 trials each
#   bash scripts/bdpt_transmittance_baselines.sh check   <tag>  # 1 render, vs trial-a
#
# Mean-luminance % delta is the reliable metric (Phase 2a finding).  Capture
# records the per-scene run-to-run noise floor (trial-a vs trial-b) so "within
# noise" is quantified per scene.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/bin/rise"
MODE="${1:?capture|check}"
TAG="${2:?tag}"
DIR="${ROOT}/tests/baselines_refactor/${TAG}_bdpttr"
RENDERED="${ROOT}/rendered"
export RISE_MEDIA_PATH="${ROOT}/"
mkdir -p "${DIR}"

# scene_rel : output_png_basename : path-tag
MANIFEST=(
    "scenes/Tests/Volumes/bdpt_homogeneous_fog.RISEscene:bdpt_homogeneous_fog:Tr-Pel(media)"
    "scenes/Tests/Volumes/bdpt_homogeneous_fog_spectral.RISEscene:bdpt_homogeneous_fog_spectral:TrNM-spectral(media)"
    "scenes/Tests/Volumes/env_bounded_fog_bdpt.RISEscene:env_bounded_fog_bdpt:escapeTr-BDPT(Gate4)"
    "scenes/Tests/Volumes/vcm_env_through_fog.RISEscene:vcm_env_through_fog:VCM-Tr-consumer(Gate6)"
    "scenes/Tests/Volumes/env_bounded_fog_vcm.RISEscene:env_bounded_fog_vcm:VCM-escapeTr(Gate6)"
    "scenes/Tests/BDPT/cornellbox_bdpt.RISEscene:cornellbox_bdpt:leak-Pel(no-media)"
    "scenes/Tests/BDPT/cornellbox_bdpt_spectral.RISEscene:cornellbox_bdpt_spectral:leak-NM(no-media)"
)

render() {  # scene_abs output_png -> 0 if produced
    local scene="$1" outpng="$2"
    rm -f "${outpng}"
    printf "render\nquit\n" | "${BIN}" "${scene}" > /tmp/bdpttr_$$.log 2>&1 || true
    [ -f "${outpng}" ]
}

cmp_pct() {  # base fresh -> prints pct delta of mean luminance
    python3 - "$1" "$2" <<'PY'
import sys, numpy as np
from PIL import Image
b=np.array(Image.open(sys.argv[1]).convert("RGB"),dtype=np.float64)
f=np.array(Image.open(sys.argv[2]).convert("RGB"),dtype=np.float64)
def L(x): return (x[:,:,0]*0.2126+x[:,:,1]*0.7152+x[:,:,2]*0.0722).mean()
bm,fm=L(b),L(f)
print("%.4f"%(0.0 if bm<1e-9 else abs(bm-fm)/bm*100.0))
PY
}

echo "MODE=${MODE} TAG=${TAG} DIR=${DIR}"
for entry in "${MANIFEST[@]}"; do
    scene_rel="${entry%%:*}"; rest="${entry#*:}"; out_base="${rest%%:*}"; ptag="${rest#*:}"
    scene_abs="${ROOT}/${scene_rel}"; name="$(basename "${scene_rel}" .RISEscene)"
    if [ ! -f "${scene_abs}" ]; then echo "SKIP-missing ${name}"; continue; fi
    outpng="${RENDERED}/${out_base}.png"

    if [ "${MODE}" = "capture" ]; then
        echo "=== capture ${name} [${ptag}] ==="
        t0=$(date +%s)
        if render "${scene_abs}" "${outpng}"; then cp "${outpng}" "${DIR}/${name}.a.png"; else echo "  FAIL render-a"; continue; fi
        if render "${scene_abs}" "${outpng}"; then cp "${outpng}" "${DIR}/${name}.b.png"; else echo "  FAIL render-b"; continue; fi
        t1=$(date +%s)
        floor=$(cmp_pct "${DIR}/${name}.a.png" "${DIR}/${name}.b.png")
        echo "  baseline=${name}.a.png  noise_floor(a-vs-b)=${floor}%  ($((t1-t0))s)"
    else  # check
        if [ ! -f "${DIR}/${name}.a.png" ]; then echo "MISS ${name}: no baseline"; continue; fi
        if render "${scene_abs}" "${outpng}"; then
            d=$(cmp_pct "${DIR}/${name}.a.png" "${outpng}")
            echo "${name} [${ptag}]: post-vs-pre = ${d}%"
        else echo "FAIL ${name}: no render"; fi
    fi
done
rm -f /tmp/bdpttr_$$.log
echo "DONE ${MODE}"
