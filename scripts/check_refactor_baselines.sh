#!/usr/bin/env bash
#
# check_refactor_baselines.sh - compare a fresh render against the
# pre-refactor baselines captured by capture_refactor_baselines.sh.
#
# For each scene:
#   - render fresh
#   - compute per-pixel RMS vs baseline PNG
#   - compute mean luminance delta
#   - fail if either exceeds the configured threshold
#
# Usage:
#   bash scripts/check_refactor_baselines.sh [phase_tag]
#
# Thresholds:
#   Mean luminance drift:  < 0.5%
#   Per-pixel log-luminance RMS: < 3.0
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/bin/rise"
TAG="${1:-phase0}"
BASELINE_DIR="${ROOT}/tests/baselines_refactor/${TAG}"
RENDERED_DIR="${ROOT}/rendered"

export RISE_MEDIA_PATH="${ROOT}/"

if [ ! -d "${BASELINE_DIR}" ]; then
    echo "ERROR: baseline directory not found: ${BASELINE_DIR}"
    echo "Run capture_refactor_baselines.sh first."
    exit 1
fi

SCENES=(
    "scenes/Tests/PathTracing/cornellbox_pathtracer.RISEscene"
    "scenes/Tests/Spectral/cornellbox_spectral.RISEscene"
    "scenes/Tests/BDPT/cornellbox_bdpt.RISEscene"
    "scenes/Tests/BDPT/cornellbox_bdpt_spectral.RISEscene"
    "scenes/Tests/BDPT/cornellbox_bdpt_caustics.RISEscene"
    "scenes/Tests/VCM/cornellbox_vcm_simple.RISEscene"
    "scenes/Tests/VCM/cornellbox_vcm_spectral.RISEscene"
    "scenes/Tests/VCM/cornellbox_vcm_caustics.RISEscene"
    "scenes/Tests/Spectral/hwss_cornellbox_pt.RISEscene"
    "scenes/Tests/Spectral/hwss_cornellbox_bdpt.RISEscene"
    "scenes/Tests/RussianRoulette/cornellbox_highalbedo_pt.RISEscene"
    "scenes/Tests/RussianRoulette/cornellbox_highalbedo_bdpt.RISEscene"
)

LUM_THRESHOLD_PCT=${LUM_THRESHOLD_PCT:-0.5}
RMS_THRESHOLD=${RMS_THRESHOLD:-3.0}

total_fail=0
total_pass=0
total_missing=0

echo "Checking baselines against fresh renders (tag=${TAG})"
echo "Thresholds: mean luminance ${LUM_THRESHOLD_PCT}%, RMS ${RMS_THRESHOLD}"
echo

for scene_rel in "${SCENES[@]}"; do
    scene_abs="${ROOT}/${scene_rel}"
    base_name="$(basename "${scene_rel}" .RISEscene)"
    baseline_png="${BASELINE_DIR}/${base_name}.png"
    fresh_png="${RENDERED_DIR}/${base_name}.png"

    if [ ! -f "${baseline_png}" ]; then
        echo "MISS ${base_name}: no baseline"
        total_missing=$((total_missing + 1))
        continue
    fi

    # Fresh render
    printf "render\nquit\n" | "${BIN}" "${scene_abs}" > /tmp/rise_check_$$.log 2>&1 || {
        echo "FAIL ${base_name}: render failed"
        total_fail=$((total_fail + 1))
        continue
    }

    if [ ! -f "${fresh_png}" ]; then
        echo "FAIL ${base_name}: no output PNG"
        total_fail=$((total_fail + 1))
        continue
    fi

    # Compare via python
    result=$(python3 - "${baseline_png}" "${fresh_png}" "${LUM_THRESHOLD_PCT}" "${RMS_THRESHOLD}" <<'PYEOF'
import sys
import numpy as np
from PIL import Image

baseline_path, fresh_path, lum_thresh_str, rms_thresh_str = sys.argv[1:5]
lum_thresh = float(lum_thresh_str)
rms_thresh = float(rms_thresh_str)

try:
    base = np.array(Image.open(baseline_path).convert("RGB"), dtype=np.float64)
    fresh = np.array(Image.open(fresh_path).convert("RGB"), dtype=np.float64)
except Exception as e:
    print(f"ERROR_IO: {e}")
    sys.exit(1)

if base.shape != fresh.shape:
    print(f"ERROR_SHAPE: baseline={base.shape} fresh={fresh.shape}")
    sys.exit(1)

def luminance(img):
    return img[:,:,0]*0.2126 + img[:,:,1]*0.7152 + img[:,:,2]*0.0722

base_lum = luminance(base)
fresh_lum = luminance(fresh)

base_mean = base_lum.mean()
fresh_mean = fresh_lum.mean()

if base_mean < 1e-9:
    lum_pct = 0.0 if fresh_mean < 1e-9 else 100.0
else:
    lum_pct = abs(base_mean - fresh_mean) / base_mean * 100.0

# Log-luminance RMS (adds 1 to avoid log(0))
base_log = np.log10(base_lum + 1.0)
fresh_log = np.log10(fresh_lum + 1.0)
log_rms = np.sqrt(((base_log - fresh_log) ** 2).mean()) * 100.0

verdict = "PASS" if (lum_pct <= lum_thresh and log_rms <= rms_thresh) else "FAIL"

# Also identical-pixel count
identical = int(np.all(base == fresh, axis=-1).sum())
total_pixels = base.shape[0] * base.shape[1]
identical_pct = 100.0 * identical / total_pixels

print(f"{verdict} lum_delta={lum_pct:.3f}% log_rms={log_rms:.3f} identical={identical_pct:.1f}%")
sys.exit(0 if verdict == "PASS" else 1)
PYEOF
    )
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "PASS ${base_name}: ${result#PASS }"
        total_pass=$((total_pass + 1))
    else
        echo "FAIL ${base_name}: ${result#FAIL }"
        total_fail=$((total_fail + 1))
    fi
    rm -f /tmp/rise_check_$$.log
done

echo
echo "Summary: ${total_pass} passed, ${total_fail} failed, ${total_missing} missing"
if [ $total_fail -gt 0 ]; then
    exit 1
fi
exit 0
