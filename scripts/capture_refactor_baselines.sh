#!/usr/bin/env bash
#
# capture_refactor_baselines.sh - capture pre-refactor rendering baselines
# for the integrator templatization gates.
#
# Renders a small curated set of deterministic-ish test scenes at fixed SPP
# and stores the PNG output under tests/baselines_refactor/.  The companion
# script check_refactor_baselines.sh compares a fresh render against these.
#
# Usage:
#   cd <RISE root>
#   bash scripts/capture_refactor_baselines.sh [phase_tag]
#
# The phase_tag (default "phase0") is used as the subdirectory under
# tests/baselines_refactor/ so we can diff between phases later.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/bin/rise"
TAG="${1:-phase0}"
BASELINE_DIR="${ROOT}/tests/baselines_refactor/${TAG}"
RENDERED_DIR="${ROOT}/rendered"

export RISE_MEDIA_PATH="${ROOT}/"

# Pin all threads to low priority / disable reservations so rendering
# is as deterministic as a multi-threaded tracer allows.  (The render
# itself is multi-threaded but the accumulation order is still
# pixel-wise deterministic with fixed SPP + no adaptive.)

mkdir -p "${BASELINE_DIR}"

# Curated scene list.  Each entry renders quickly (< 60s each on modern
# hardware at the configured SPP) and exercises one of the code paths
# affected by the refactor.
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

echo "Capturing baselines to: ${BASELINE_DIR}"
echo "Total scenes: ${#SCENES[@]}"
echo

for scene_rel in "${SCENES[@]}"; do
    scene_abs="${ROOT}/${scene_rel}"
    if [ ! -f "${scene_abs}" ]; then
        echo "SKIP (missing): ${scene_rel}"
        continue
    fi

    base_name="$(basename "${scene_rel}" .RISEscene)"
    echo "=== Rendering ${base_name} ==="

    start_time=$(date +%s)
    printf "render\nquit\n" | "${BIN}" "${scene_abs}" > /tmp/rise_capture_$$.log 2>&1 || {
        echo "  FAIL rendering ${base_name}"
        tail -10 /tmp/rise_capture_$$.log
        continue
    }
    end_time=$(date +%s)
    elapsed=$((end_time - start_time))

    # Copy the rendered PNG to the baseline dir.  Each scene's pattern
    # puts it at rendered/<basename>.png or a similar path.
    candidate_png="${RENDERED_DIR}/${base_name}.png"
    if [ -f "${candidate_png}" ]; then
        cp "${candidate_png}" "${BASELINE_DIR}/${base_name}.png"
        size=$(wc -c < "${BASELINE_DIR}/${base_name}.png")
        echo "  captured ${base_name}.png (${size} bytes, ${elapsed}s)"
    else
        echo "  WARN: expected output ${candidate_png} not found"
        ls "${RENDERED_DIR}/${base_name}"* 2>/dev/null || true
    fi
    rm -f /tmp/rise_capture_$$.log
done

echo
echo "Baseline capture complete.  ${BASELINE_DIR}"
ls -la "${BASELINE_DIR}"
