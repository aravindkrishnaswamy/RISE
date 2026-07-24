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
# Check limit: max(MAX_DELTA_PCT, captured a-vs-b floor); default 0.5%.
# Captures/checks reject images with mean encoded luma below 1.0.
# Failed or interrupted captures leave the tag blocked until capture succeeds.
#
# Mean-luminance % delta is the reliable metric (Phase 2a finding: log_rms
# is mis-calibrated for noisy spectral/SMS scenes).  The capture pass also
# records the per-scene run-to-run noise floor (trial-a vs trial-b) so
# "within noise" is quantified per scene rather than against a fixed 0.27%.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/bin/rise"
MODE="${1:?capture|check}"
TAG="${2:?tag}"
MAX_DELTA_PCT="${MAX_DELTA_PCT:-0.5}"
if ! python3 - "${MAX_DELTA_PCT}" <<'PY'
import math, sys
try:
    value = float(sys.argv[1])
except ValueError:
    sys.exit(1)
sys.exit(0 if math.isfinite(value) and value >= 0.0 else 1)
PY
then
    echo "ERROR: MAX_DELTA_PCT must be finite and nonnegative" >&2
    exit 2
fi
DIR="${ROOT}/tests/baselines_refactor/${TAG}_divergent"
CAPTURE_MARKER="${DIR}/.capture-incomplete"
RENDERED="${ROOT}/rendered"
export RISE_MEDIA_PATH="${ROOT}/"
case "${MODE}" in
    capture|check) ;;
    *) echo "ERROR: MODE must be capture or check" >&2; exit 2 ;;
esac
if ! mkdir -p "${DIR}"; then
    echo "ERROR: unable to create baseline directory: ${DIR}" >&2
    exit 1
fi

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
    if ! rm -f "${outpng}"; then
        echo "ERROR: unable to remove stale render output: ${outpng}" >&2
        return 1
    fi
    printf "render\nquit\n" | "${BIN}" "${scene}" > /tmp/diverg_$$.log 2>&1 || true
    [ -f "${outpng}" ]
}

cmp_pct() {  # base fresh -> prints pct delta
    python3 - "$1" "$2" <<'PY'
import sys, numpy as np
from PIL import Image
b=np.array(Image.open(sys.argv[1]).convert("RGB"),dtype=np.float64)
f=np.array(Image.open(sys.argv[2]).convert("RGB"),dtype=np.float64)
if b.shape != f.shape:
    print(f"image shape mismatch: baseline={b.shape} fresh={f.shape}", file=sys.stderr)
    sys.exit(1)
def L(x): return (x[:,:,0]*0.2126+x[:,:,1]*0.7152+x[:,:,2]*0.0722).mean()
bm,fm=L(b),L(f)
if bm < 1.0 or fm < 1.0:
    print("near-black image rejected", file=sys.stderr)
    sys.exit(1)
print("%.4f"%(abs(bm-fm)/bm*100.0))
PY
}

capture_pair() {  # scene output target-a target-b -> prints validated noise floor
    local scene="$1" outpng="$2" target_a="$3" target_b="$4"
    local tmp_a="${target_a}.tmp.$$" tmp_b="${target_b}.tmp.$$" floor

    if ! rm -f "${target_a}" "${target_b}" "${tmp_a}" "${tmp_b}"; then
        echo "ERROR: unable to clear prior baseline pair" >&2
        return 1
    fi
    if ! render "${scene}" "${outpng}"; then
        echo "ERROR: trial-a render produced no output" >&2
        return 1
    fi
    if ! cp "${outpng}" "${tmp_a}"; then
        echo "ERROR: unable to stage trial-a baseline" >&2
        return 1
    fi
    if ! render "${scene}" "${outpng}"; then
        echo "ERROR: trial-b render produced no output" >&2
        rm -f "${tmp_a}" "${tmp_b}" || true
        return 1
    fi
    if ! cp "${outpng}" "${tmp_b}"; then
        echo "ERROR: unable to stage trial-b baseline" >&2
        rm -f "${tmp_a}" "${tmp_b}" || true
        return 1
    fi
    if ! floor="$(cmp_pct "${tmp_a}" "${tmp_b}")"; then
        echo "ERROR: captured baseline pair is invalid" >&2
        rm -f "${tmp_a}" "${tmp_b}" || true
        return 1
    fi
    if ! mv "${tmp_b}" "${target_b}"; then
        echo "ERROR: unable to publish trial-b baseline" >&2
        rm -f "${tmp_a}" "${tmp_b}" || true
        return 1
    fi
    if ! mv "${tmp_a}" "${target_a}"; then
        echo "ERROR: unable to publish trial-a baseline" >&2
        rm -f "${target_b}" "${tmp_a}" || true
        return 1
    fi
    printf '%s\n' "${floor}"
}

echo "MODE=${MODE} TAG=${TAG} DIR=${DIR}"
if [ "${MODE}" = "capture" ]; then
    if ! touch "${CAPTURE_MARKER}"; then
        echo "ERROR: unable to mark baseline capture incomplete: ${CAPTURE_MARKER}" >&2
        exit 1
    fi
elif [ -e "${CAPTURE_MARKER}" ]; then
    echo "ERROR: baseline capture is incomplete: ${DIR}" >&2
    exit 1
fi
failures=0
for entry in "${MANIFEST[@]}"; do
    scene_rel="${entry%%:*}"
    if [ ! -f "${ROOT}/${scene_rel}" ]; then
        echo "ERROR: configured scene is missing: ${scene_rel}" >&2
        failures=$((failures + 1))
    fi
done
[ "${failures}" -eq 0 ] || exit 1

for entry in "${MANIFEST[@]}"; do
    scene_rel="${entry%%:*}"; rest="${entry#*:}"; out_base="${rest%%:*}"; ptag="${rest#*:}"
    scene_abs="${ROOT}/${scene_rel}"; name="$(basename "${scene_rel}" .RISEscene)"
    outpng="${RENDERED}/${out_base}.png"

    if [ "${MODE}" = "capture" ]; then
        echo "=== capture ${name} [${ptag}] ==="
        t0=$(date +%s)
        if ! floor="$(capture_pair "${scene_abs}" "${outpng}" "${DIR}/${name}.a.png" "${DIR}/${name}.b.png")"; then
            echo "  FAIL capture pair ${name}"
            failures=$((failures + 1))
            continue
        fi
        t1=$(date +%s)
        echo "  baseline=${name}.a.png  noise_floor(a-vs-b)=${floor}%  ($((t1-t0))s)"
    else  # check
        base="${DIR}/${name}.a.png"
        noise="${DIR}/${name}.b.png"
        if [ ! -f "${base}" ] || [ ! -f "${noise}" ]; then
            echo "FAIL incomplete-baseline ${name}"
            failures=$((failures + 1))
            continue
        fi
        if render "${scene_abs}" "${outpng}"; then
            if ! d="$(cmp_pct "${base}" "${outpng}")"; then
                echo "  FAIL compare ${name}"
                failures=$((failures + 1))
                continue
            fi
            if ! floor="$(cmp_pct "${base}" "${noise}")"; then
                echo "  FAIL compare noise floor ${name}"
                failures=$((failures + 1))
                continue
            fi
            limit="$(awk -v configured="${MAX_DELTA_PCT}" -v measured="${floor}" 'BEGIN { measured += 0; if (measured > configured) configured = measured; printf "%.4f", configured }')"
            if awk -v delta="${d}" -v limit="${limit}" 'BEGIN { exit(delta <= limit ? 0 : 1) }'; then
                echo "  PASS ${name} [${ptag}]: post-Δ=${d}%  (floor=${floor}%, limit=${limit}%)"
            else
                echo "  FAIL ${name} [${ptag}]: post-Δ=${d}%  (floor=${floor}%, limit=${limit}%)"
                failures=$((failures + 1))
            fi
        else
            echo "FAIL ${name}: no render"
            failures=$((failures + 1))
        fi
    fi
done
rm -f /tmp/diverg_$$.log
[ "${failures}" -eq 0 ] || exit 1
if [ "${MODE}" = "capture" ] && ! rm -f "${CAPTURE_MARKER}"; then
    echo "ERROR: unable to mark baseline capture complete: ${CAPTURE_MARKER}" >&2
    exit 1
fi
echo "DONE ${MODE}"
