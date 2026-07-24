#!/usr/bin/env bash
#
# bdpt_light_subpath_baselines.sh - capture / check rendering baselines for the
# BDPT GenerateLightSubpath{,NM} templatization (Phase 2c part 3 / family F2b,
# the LIGHT half of the subpath-generation family).  Sibling of
# bdpt_eye_subpath_baselines.sh (F2a) and bdpt_transmittance_baselines.sh (F1) —
# same proven mechanism, light-subpath-specific manifest.
#
# The family under test is the light-side path generator, so the load-bearing
# divergent paths are: the light-EMISSION vertex 0 (mesh-area, env-IBL, point),
# the surface/medium walk, the spectral (NM) Le-conversion preamble
# (emittedRadianceNM / luminance-weight / GetRadianceNM), the HWSS companion-Le
# bundle, glossy interreflection, and — F2b's highest risk — the cross-integrator
# consumers whose light-vertex store derives ENTIRELY from this method: VCM
# (connection AND merge/photon) and MLT.
#
# Coverage map:
#   - std Pel light walk + area-light v0 : cornellbox_bdpt              (bdpt_pel)
#   - glossy interreflection Pel         : cornellbox_bdpt_glossy       (bdpt_pel)
#   - in-medium scatter Pel (light-walk) : bdpt_homogeneous_fog         (MEDIUM vertex + Tr)
#   - env-IBL light EMISSION v0 + escTr   : env_bounded_fog_bdpt         (pEnvLight v0, pdfSelect)
#   - std NM (non-HWSS) + Le-conv mesh   : cornellbox_bdpt_spectral     (emittedRadianceNM v0)
#   - in-medium scatter NM (light-walk)  : bdpt_homogeneous_fog_spectral(NM MEDIUM vertex + TrNM)
#   - NM HWSS companion-Le bundle        : hwss_cornellbox_bdpt         (hwssBetaNM init from Le)
#   - MLT consumer (Gate F)              : mlt_veach_egg                (MLT -> GenerateLightSubpath)
#   - VCM Pel light-store (Gate 6)       : cornellbox_vcm_simple        (VCM connect light verts)
#   - VCM NM light-store (Gate 6)        : cornellbox_vcm_spectral      (VCM connect light verts NM)
#   - VCM env-escape (Gate 6)            : env_bounded_fog_vcm          (VCM s=0 via shared gen)
#   - VCM MERGE light-store (Gate 6)***  : cornellbox_vcm_caustics      (VC+VM: merge over light verts)
#   - vColor fold (Gate 4)               : vertex_colors_quad_bdpt_spectral (NM surface vColor)
#   - vColor fold transitive (Gate 4)    : vertex_colors_quad_vcm_spectral  (VCM-spectral reuse)
#
#   *** cornellbox_vcm_caustics is the F2b-specific addition vs the F2a (eye)
#   manifest: VCM's whole light-vertex store (the merge/photon kd-tree) is built
#   from GenerateLightSubpath{,NM}, so a caustic scene that MERGES over those
#   vertices is the strongest cross-integrator regression signal for this family.
#
# NOTE: env-IBL light-emission correctness (Pel AND NM, HWSS on/off) is *also*
# guarded by the binary EnvLightBalanceTest (80/80 lax oracle) — a physical-
# balance oracle strictly stronger than the mean-luminance PNG delta below.
#
# Usage:
#   bash scripts/bdpt_light_subpath_baselines.sh capture <tag>  # 2 trials each
#   bash scripts/bdpt_light_subpath_baselines.sh check   <tag>  # 1 render, vs trial-a
# Check limit: max(MAX_DELTA_PCT, captured a-vs-b floor); default 0.5%.
#
# Mean-luminance % delta is the reliable metric (Phase 2a finding).  Capture
# records the per-scene run-to-run noise floor (trial-a vs trial-b) so "within
# noise" is quantified per scene.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/bin/rise"
MODE="${1:?capture|check}"
TAG="${2:?tag}"
MAX_DELTA_PCT="${MAX_DELTA_PCT:-0.5}"
DIR="${ROOT}/tests/baselines_refactor/${TAG}_bdptlight"
RENDERED="${ROOT}/rendered"
export RISE_MEDIA_PATH="${ROOT}/"
mkdir -p "${DIR}"

# scene_rel : output_png_basename : path-tag
MANIFEST=(
    "scenes/Tests/BDPT/cornellbox_bdpt.RISEscene:cornellbox_bdpt:std-Pel+areaLightV0"
    "scenes/Tests/BDPT/cornellbox_bdpt_glossy.RISEscene:cornellbox_bdpt_glossy:glossy-Pel"
    "scenes/Tests/Volumes/bdpt_homogeneous_fog.RISEscene:bdpt_homogeneous_fog:media-Pel(light-walk)"
    "scenes/Tests/Volumes/env_bounded_fog_bdpt.RISEscene:env_bounded_fog_bdpt:envEmitV0+escapeTr-Pel"
    "scenes/Tests/BDPT/cornellbox_bdpt_spectral.RISEscene:cornellbox_bdpt_spectral:std-NM+LeConvMesh"
    "scenes/Tests/Volumes/bdpt_homogeneous_fog_spectral.RISEscene:bdpt_homogeneous_fog_spectral:media-NM(light-walk)"
    "scenes/Tests/Spectral/hwss_cornellbox_bdpt.RISEscene:hwss_cornellbox_bdpt:NM-HWSS-companionLe"
    "scenes/FeatureBased/MLT/mlt_veach_egg.RISEscene:mlt_veach_egg:MLT-consumer(GateF)"
    "scenes/Tests/VCM/cornellbox_vcm_simple.RISEscene:cornellbox_vcm_simple:VCM-Pel-lightStore(Gate6)"
    "scenes/Tests/VCM/cornellbox_vcm_spectral.RISEscene:cornellbox_vcm_spectral:VCM-NM-lightStore(Gate6)"
    "scenes/Tests/Volumes/env_bounded_fog_vcm.RISEscene:env_bounded_fog_vcm:VCM-envEscape(Gate6)"
    "scenes/Tests/VCM/cornellbox_vcm_caustics.RISEscene:cornellbox_vcm_caustics:VCM-MERGE-lightStore(Gate6)"
    "scenes/Tests/Geometry/vertex_colors_quad_bdpt_spectral.RISEscene:vertex_colors_quad_bdpt_spectral:vColor-fold(Gate4)"
    "scenes/Tests/Geometry/vertex_colors_quad_vcm_spectral.RISEscene:vertex_colors_quad_vcm_spectral:vColor-fold-transitive(Gate4)"
)

render() {  # scene_abs output_png -> 0 if produced
    local scene="$1" outpng="$2"
    rm -f "${outpng}"
    printf "render\nquit\n" | "${BIN}" "${scene}" > /tmp/bdptlight_$$.log 2>&1 || true
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
case "${MODE}" in
    capture|check) ;;
    *) echo "ERROR: MODE must be capture or check" >&2; exit 2 ;;
esac

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
        if render "${scene_abs}" "${outpng}"; then cp "${outpng}" "${DIR}/${name}.a.png"; else echo "  FAIL render-a"; failures=$((failures + 1)); continue; fi
        if render "${scene_abs}" "${outpng}"; then cp "${outpng}" "${DIR}/${name}.b.png"; else echo "  FAIL render-b"; failures=$((failures + 1)); continue; fi
        if ! floor="$(cmp_pct "${DIR}/${name}.a.png" "${DIR}/${name}.b.png")"; then
            echo "  FAIL compare capture pair ${name}"
            failures=$((failures + 1))
            continue
        fi
        echo "  noise-floor(a-vs-b) = ${floor}%"
    else
        base="${DIR}/${name}.a.png"
        if [ ! -f "${base}" ]; then echo "FAIL no-baseline ${name}"; failures=$((failures + 1)); continue; fi
        if render "${scene_abs}" "${outpng}"; then
            if ! d="$(cmp_pct "${base}" "${outpng}")"; then
                echo "  FAIL compare ${name}"
                failures=$((failures + 1))
                continue
            fi
            floor="n/a"
            if [ -f "${DIR}/${name}.b.png" ] && ! floor="$(cmp_pct "${DIR}/${name}.a.png" "${DIR}/${name}.b.png")"; then
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
