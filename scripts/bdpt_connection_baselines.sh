#!/usr/bin/env bash
#
# Phase 2c F3a — ConnectAndEvaluate{,NM} connection-evaluator baselines.
#
# ConnectAndEvaluate drives EVERY (s,t) connection strategy, so the divergent
# paths to cover are: s=0 emitter-hit (surface emitter + env-escape Path B),
# s=1 NEE (area-light pLuminary, point-light pLight, env-NEE), t=1 light->camera
# splat (incl. the s=1 LIGHT-direct sub-branch), the interior s>=2/t>=2
# connection (glossy), connection transmittance through media, and both NM
# modes (single-wavelength + HWSS bundle).  The pdfRev const_cast mutation
# trick + env-IBL continuous-PMF are exercised on every case.
#
# Coverage map (scene -> divergent path it pins):
#   - all (s,t) cases Pel + area light  : cornellbox_bdpt              (s0-surf, s1-NEE-pLuminary, t1, interior)
#   - all (s,t) cases NM  + area light  : cornellbox_bdpt_spectral     (s0 EvalEmitterRadianceNM, s1 NEE, interior NM)
#   - glossy interior Pel               : cornellbox_bdpt_glossy       (interior s>=2 connection)
#   - point-light Pel (pLight branch)   : cornellbox_bdpt_pointlight   (s1/t1 omni ILight)
#   - point-light NM (luminance proj)   : cornellbox_bdpt_pointlight_spectral (NM s1/t1 pLight Rec.709 proj)
#   - connection Tr through media Pel   : bdpt_homogeneous_fog         (s1/interior medium Tr)
#   - connection Tr through media NM    : bdpt_homogeneous_fog_spectral(EvalConnectionTransmittanceNM)
#   - s=0 env-escape + s=1 env-NEE Pel  : env_bounded_fog_bdpt         (pEnvLight, continuous-PMF, escape Tr)
#   - NM HWSS connection bundle         : hwss_cornellbox_bdpt         (connection in HWSS mode)
#   - MLT consumer (Gate F)             : mlt_veach_egg                (MLT -> EvaluateAllStrategies -> ConnectAndEvaluate)
#   - VCM Pel (Gate 6, no-touch net)    : cornellbox_vcm_simple        (VCM does NOT reach ConnectAndEvaluate)
#   - VCM NM  (Gate 6, no-touch net)    : cornellbox_vcm_spectral
#   - VCM env-escape (Gate 6)           : env_bounded_fog_vcm
#   - VCM MERGE (Gate 6)                : cornellbox_vcm_caustics
#   - NM connection-time vColor (Gate4) : vertex_colors_quad_bdpt_spectral
#   - NM vColor transitive (Gate 4)     : vertex_colors_quad_vcm_spectral
#
# The env connection MIS is *also* pinned by EnvLightBalanceTest (lax + strict),
# the sharpest oracle (asserts BDPT/VCM ~= PT under env-IBL, incl. the env+omni
# topology E spectral which exercises the NM pLight branch).  Run that test
# binary separately; this PNG-mean harness is the per-scene noise-floored
# render check.
#
# Usage:
#   bash scripts/bdpt_connection_baselines.sh capture <tag>  # 2 trials each
#   bash scripts/bdpt_connection_baselines.sh check   <tag>  # 1 render, vs trial-a
# Check limit: max(MAX_DELTA_PCT, captured a-vs-b floor); default 0.5%.
# Captures/checks reject images with mean encoded luma below 1.0.
# Failed or interrupted captures leave the tag blocked until capture succeeds.
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
DIR="${ROOT}/tests/baselines_refactor/${TAG}_bdptconn"
CAPTURE_MARKER="${DIR}/.capture-incomplete"
RENDERED="${ROOT}/rendered"
export RISE_MEDIA_PATH="${ROOT}/"
if ! mkdir -p "${DIR}"; then
    echo "ERROR: unable to create baseline directory: ${DIR}" >&2
    exit 1
fi

# scene_rel : output_png_basename : path-tag
MANIFEST=(
    "scenes/Tests/BDPT/cornellbox_bdpt.RISEscene:cornellbox_bdpt:allCases-Pel+areaLight"
    "scenes/Tests/BDPT/cornellbox_bdpt_spectral.RISEscene:cornellbox_bdpt_spectral:allCases-NM+areaLight"
    "scenes/Tests/BDPT/cornellbox_bdpt_glossy.RISEscene:cornellbox_bdpt_glossy:glossy-interior-Pel"
    "scenes/Tests/BDPT/cornellbox_bdpt_pointlight.RISEscene:cornellbox_bdpt_pointlight:pLight-Pel(s1/t1)"
    "scenes/Tests/Spectral/cornellbox_bdpt_pointlight_spectral.RISEscene:cornellbox_bdpt_pointlight_spectral:pLight-NM-lumProj(s1/t1)"
    "scenes/Tests/Volumes/bdpt_homogeneous_fog.RISEscene:bdpt_homogeneous_fog:connTr-Pel"
    "scenes/Tests/Volumes/bdpt_homogeneous_fog_spectral.RISEscene:bdpt_homogeneous_fog_spectral:connTr-NM"
    "scenes/Tests/Volumes/env_bounded_fog_bdpt.RISEscene:env_bounded_fog_bdpt:envEscape+envNEE+escTr-Pel"
    "scenes/Tests/Spectral/hwss_cornellbox_bdpt.RISEscene:hwss_cornellbox_bdpt:NM-HWSS-connection"
    "scenes/FeatureBased/MLT/mlt_veach_egg.RISEscene:mlt_veach_egg:MLT-consumer(GateF)"
    "scenes/Tests/VCM/cornellbox_vcm_simple.RISEscene:cornellbox_vcm_simple:VCM-Pel(Gate6-noTouch)"
    "scenes/Tests/VCM/cornellbox_vcm_spectral.RISEscene:cornellbox_vcm_spectral:VCM-NM(Gate6-noTouch)"
    "scenes/Tests/Volumes/env_bounded_fog_vcm.RISEscene:env_bounded_fog_vcm:VCM-envEscape(Gate6)"
    "scenes/Tests/VCM/cornellbox_vcm_caustics.RISEscene:cornellbox_vcm_caustics:VCM-MERGE(Gate6)"
    "scenes/Tests/Geometry/vertex_colors_quad_bdpt_spectral.RISEscene:vertex_colors_quad_bdpt_spectral:NM-connVColor(Gate4)"
    "scenes/Tests/Geometry/vertex_colors_quad_vcm_spectral.RISEscene:vertex_colors_quad_vcm_spectral:NM-vColor-transitive(Gate4)"
)

render() {  # scene_abs output_png -> 0 if produced
    local scene="$1" outpng="$2"
    if ! rm -f "${outpng}"; then
        echo "ERROR: unable to remove stale render output: ${outpng}" >&2
        return 1
    fi
    printf "render\nquit\n" | "${BIN}" "${scene}" > /tmp/bdptconn_$$.log 2>&1 || true
    [ -f "${outpng}" ]
}

cmp_pct() {  # base fresh -> prints pct delta of mean luminance
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
case "${MODE}" in
    capture|check) ;;
    *) echo "ERROR: MODE must be capture or check" >&2; exit 2 ;;
esac
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
    if [ ! -f "${scene_abs}" ]; then echo "FAIL missing ${name}"; failures=$((failures + 1)); continue; fi
    outpng="${RENDERED}/${out_base}.png"

    if [ "${MODE}" = "capture" ]; then
        echo "=== capture ${name} [${ptag}] ==="
        if ! floor="$(capture_pair "${scene_abs}" "${outpng}" "${DIR}/${name}.a.png" "${DIR}/${name}.b.png")"; then
            echo "  FAIL capture pair ${name}"
            failures=$((failures + 1))
            continue
        fi
        echo "  noise-floor(a-vs-b) = ${floor}%"
    else
        base="${DIR}/${name}.a.png"
        noise="${DIR}/${name}.b.png"
        if [ ! -f "${base}" ] || [ ! -f "${noise}" ]; then echo "FAIL incomplete-baseline ${name}"; failures=$((failures + 1)); continue; fi
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
            echo "  FAIL render ${name}"
            failures=$((failures + 1))
        fi
    fi
done

[ "${failures}" -eq 0 ] || exit 1
if [ "${MODE}" = "capture" ] && ! rm -f "${CAPTURE_MARKER}"; then
    echo "ERROR: unable to mark baseline capture complete: ${CAPTURE_MARKER}" >&2
    exit 1
fi
