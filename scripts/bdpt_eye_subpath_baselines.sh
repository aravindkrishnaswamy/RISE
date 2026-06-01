#!/usr/bin/env bash
#
# bdpt_eye_subpath_baselines.sh - capture / check rendering baselines for the
# BDPT GenerateEyeSubpath{,NM} templatization (Phase 2c part 2 / family F2a,
# the eye half of the subpath-generation family).  Sibling of
# bdpt_transmittance_baselines.sh (F1) and divergent_baselines.sh (2b) — same
# proven mechanism, eye-subpath-specific manifest.
#
# The family under test is the camera-side path generator, so the load-bearing
# divergent paths are: the surface scatter walk, the env-IBL eye-escape
# (Path B, synthetic env vertex), the in-medium scatter + escape-Tr, glossy
# interreflection, and the spectral (NM) twins of all of the above — including
# the two NM modes (single-wavelength pSwlHWSS=NULL and HWSS-bundle
# pSwlHWSS=&swl).  Cross-integrator consumers (MLT, VCM call GenerateEyeSubpath
# directly) are exercised too (Gate F, Gate 6).
#
# Coverage map:
#   - std Pel surface walk      : cornellbox_bdpt                 (bdpt_pel)
#   - glossy interreflection Pel: cornellbox_bdpt_glossy          (bdpt_pel)
#   - in-medium scatter Pel     : bdpt_homogeneous_fog            (eye-walk medium vertex + Tr)
#   - env-escape + escape-Tr Pel: env_bounded_fog_bdpt            (Path B synthetic env vertex)
#   - std NM (non-HWSS)         : cornellbox_bdpt_spectral        (GenerateEyeSubpathNM, pSwlHWSS=NULL)
#   - in-medium scatter NM      : bdpt_homogeneous_fog_spectral   (NM eye-walk medium vertex + TrNM)
#   - NM HWSS bundle            : hwss_cornellbox_bdpt            (GenerateEyeSubpathNM, pSwlHWSS=&swl)
#   - MLT consumer (Gate F)     : mlt_veach_egg_bdpt              (MLTRasterizer -> GenerateEyeSubpath)
#   - VCM Pel consumer (Gate 6) : cornellbox_vcm_simple           (VCMPelRasterizer -> GenerateEyeSubpath)
#   - VCM NM consumer (Gate 6)  : cornellbox_vcm_spectral         (VCMSpectralRasterizer -> GenerateEyeSubpathNM)
#   - VCM env-escape (Gate 6)   : env_bounded_fog_vcm             (VCM s=0 env-escape via shared generator)
#
# NOTE: env-IBL eye-escape correctness (Pel AND NM, HWSS on/off) is *also*
# guarded by the binary EnvLightBalanceTest (80/80 lax oracle) which renders
# uniform-env topologies through the spectral BDPT rasterizer with HWSS both
# on and off — that is a physical-balance oracle, strictly stronger than the
# mean-luminance PNG delta below for the env path.
#
# Usage:
#   bash scripts/bdpt_eye_subpath_baselines.sh capture <tag>  # 2 trials each
#   bash scripts/bdpt_eye_subpath_baselines.sh check   <tag>  # 1 render, vs trial-a
#
# Mean-luminance % delta is the reliable metric (Phase 2a finding).  Capture
# records the per-scene run-to-run noise floor (trial-a vs trial-b) so "within
# noise" is quantified per scene.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/bin/rise"
MODE="${1:?capture|check}"
TAG="${2:?tag}"
DIR="${ROOT}/tests/baselines_refactor/${TAG}_bdpteye"
RENDERED="${ROOT}/rendered"
export RISE_MEDIA_PATH="${ROOT}/"
mkdir -p "${DIR}"

# scene_rel : output_png_basename : path-tag
MANIFEST=(
    "scenes/Tests/BDPT/cornellbox_bdpt.RISEscene:cornellbox_bdpt:std-Pel"
    "scenes/Tests/BDPT/cornellbox_bdpt_glossy.RISEscene:cornellbox_bdpt_glossy:glossy-Pel"
    "scenes/Tests/Volumes/bdpt_homogeneous_fog.RISEscene:bdpt_homogeneous_fog:media-Pel(eye-walk)"
    "scenes/Tests/Volumes/env_bounded_fog_bdpt.RISEscene:env_bounded_fog_bdpt:envEscape+escapeTr-Pel"
    "scenes/Tests/BDPT/cornellbox_bdpt_spectral.RISEscene:cornellbox_bdpt_spectral:std-NM(non-HWSS)"
    "scenes/Tests/Volumes/bdpt_homogeneous_fog_spectral.RISEscene:bdpt_homogeneous_fog_spectral:media-NM(eye-walk)"
    "scenes/Tests/Spectral/hwss_cornellbox_bdpt.RISEscene:hwss_cornellbox_bdpt:NM-HWSS-bundle"
    "scenes/FeatureBased/MLT/mlt_veach_egg_bdpt.RISEscene:mlt_veach_egg_bdpt:MLT-consumer(GateF)"
    "scenes/Tests/VCM/cornellbox_vcm_simple.RISEscene:cornellbox_vcm_simple:VCM-Pel-consumer(Gate6)"
    "scenes/Tests/VCM/cornellbox_vcm_spectral.RISEscene:cornellbox_vcm_spectral:VCM-NM-consumer(Gate6)"
    "scenes/Tests/Volumes/env_bounded_fog_vcm.RISEscene:env_bounded_fog_vcm:VCM-envEscape(Gate6)"
)

render() {  # scene_abs output_png -> 0 if produced
    local scene="$1" outpng="$2"
    rm -f "${outpng}"
    printf "render\nquit\n" | "${BIN}" "${scene}" > /tmp/bdpteye_$$.log 2>&1 || true
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
