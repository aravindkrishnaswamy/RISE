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
#   - MLT consumer (Gate F)             : mlt_veach_egg_bdpt           (MLT -> EvaluateAllStrategies -> ConnectAndEvaluate)
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
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/bin/rise"
MODE="${1:?capture|check}"
TAG="${2:?tag}"
DIR="${ROOT}/tests/baselines_refactor/${TAG}_bdptconn"
RENDERED="${ROOT}/rendered"
export RISE_MEDIA_PATH="${ROOT}/"
mkdir -p "${DIR}"

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
    "scenes/FeatureBased/MLT/mlt_veach_egg_bdpt.RISEscene:mlt_veach_egg_bdpt:MLT-consumer(GateF)"
    "scenes/Tests/VCM/cornellbox_vcm_simple.RISEscene:cornellbox_vcm_simple:VCM-Pel(Gate6-noTouch)"
    "scenes/Tests/VCM/cornellbox_vcm_spectral.RISEscene:cornellbox_vcm_spectral:VCM-NM(Gate6-noTouch)"
    "scenes/Tests/Volumes/env_bounded_fog_vcm.RISEscene:env_bounded_fog_vcm:VCM-envEscape(Gate6)"
    "scenes/Tests/VCM/cornellbox_vcm_caustics.RISEscene:cornellbox_vcm_caustics:VCM-MERGE(Gate6)"
    "scenes/Tests/Geometry/vertex_colors_quad_bdpt_spectral.RISEscene:vertex_colors_quad_bdpt_spectral:NM-connVColor(Gate4)"
    "scenes/Tests/Geometry/vertex_colors_quad_vcm_spectral.RISEscene:vertex_colors_quad_vcm_spectral:NM-vColor-transitive(Gate4)"
)

render() {  # scene_abs output_png -> 0 if produced
    local scene="$1" outpng="$2"
    rm -f "${outpng}"
    printf "render\nquit\n" | "${BIN}" "${scene}" > /tmp/bdptconn_$$.log 2>&1 || true
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
        if render "${scene_abs}" "${outpng}"; then cp "${outpng}" "${DIR}/${name}.a.png"; else echo "  FAIL render-a"; continue; fi
        if render "${scene_abs}" "${outpng}"; then cp "${outpng}" "${DIR}/${name}.b.png"; else echo "  FAIL render-b"; continue; fi
        floor="$(cmp_pct "${DIR}/${name}.a.png" "${DIR}/${name}.b.png")"
        echo "  noise-floor(a-vs-b) = ${floor}%"
    else
        base="${DIR}/${name}.a.png"
        if [ ! -f "${base}" ]; then echo "SKIP-nobaseline ${name}"; continue; fi
        if render "${scene_abs}" "${outpng}"; then
            d="$(cmp_pct "${base}" "${outpng}")"
            floor="n/a"; [ -f "${DIR}/${name}.b.png" ] && floor="$(cmp_pct "${DIR}/${name}.a.png" "${DIR}/${name}.b.png")"
            echo "  ${name} [${ptag}]: post-Δ=${d}%  (floor=${floor}%)"
        else
            echo "  FAIL render ${name}"
        fi
    fi
done
