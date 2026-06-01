#!/usr/bin/env bash
#
# divergent_baselines.sh - capture / check rendering baselines for the
# DIVERGENT PT code paths (guiding, BSSRDF, random-walk SSS, SMS, volume)
# that the standard cornellbox baseline set does NOT exercise.  Used to
# verify the Phase 2b-part-2 IntegrateFromHit templatization is
# zero-behaviour-change on those paths (PRE_PHASE1_STATUS.md stop-rule #2).
#
# Usage:
#   bash scripts/divergent_baselines.sh capture <tag>   # 2 trials each; saves trial-a + noise floor
#   bash scripts/divergent_baselines.sh check   <tag>   # 1 render each; compares to saved trial-a
#
# Mean-luminance % delta is the reliable metric (Phase 2a finding: log_rms
# is mis-calibrated for noisy spectral/SMS scenes).  The capture pass also
# records the per-scene run-to-run noise floor (trial-a vs trial-b) so
# "within noise" is quantified per scene rather than against a fixed 0.27%.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/bin/rise"
MODE="${1:?capture|check}"
TAG="${2:?tag}"
DIR="${ROOT}/tests/baselines_refactor/${TAG}_divergent"
RENDERED="${ROOT}/rendered"
export RISE_MEDIA_PATH="${ROOT}/"
mkdir -p "${DIR}"

# scene_rel : output_png_basename : path-tag
MANIFEST=(
    "scenes/Tests/PathTracing/pt_guiding_stress_guided.RISEscene:pt_guiding_stress_guided:GUIDING(Pel)"
    "scenes/Tests/SubsurfaceScattering/pt_sss_wax_sphere.RISEscene:pt_sss_wax_sphere:BSSRDF(Pel)"
    "scenes/Tests/SubsurfaceScattering/rwsss_thin_slab.RISEscene:rwsss_thin_slab:RW-SSS(Pel)"
    "scenes/Tests/SMS/sms_k1_refract.RISEscene:sms_k1_sms:SMS(Pel)"
    "scenes/Tests/SMS/sms_k2_glasssphere.RISEscene:sms_k2_glasssphere:SMS(Pel)"
    "scenes/Tests/Volumes/env_bounded_fog_pt.RISEscene:env_bounded_fog_pt:VOLUME-escapeTr(Pel)"
    "scenes/Tests/Materials/spectral_skin_fast.RISEscene:spectral_skin_fast:SSS(NM)"
    "scenes/Tests/Spectral/spectral_dispersive_caustic_pt_sms.RISEscene:spectral_dispersive_caustic_pt_sms:SMS(NM)"
)

render() {  # scene_abs output_png -> copies produced png to $2
    local scene="$1" outpng="$2"
    rm -f "${outpng}"
    printf "render\nquit\n" | "${BIN}" "${scene}" > /tmp/diverg_$$.log 2>&1 || true
    [ -f "${outpng}" ]
}

meanlum() {  # png -> prints mean luminance
    python3 - "$1" <<'PY'
import sys, numpy as np
from PIL import Image
a=np.array(Image.open(sys.argv[1]).convert("RGB"),dtype=np.float64)
print("%.6f"%(a[:,:,0]*0.2126+a[:,:,1]*0.7152+a[:,:,2]*0.0722).mean())
PY
}

cmp_pct() {  # base fresh -> prints pct delta
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
rm -f /tmp/diverg_$$.log
echo "DONE ${MODE}"
