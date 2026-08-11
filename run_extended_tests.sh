#!/usr/bin/env bash

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN_DIR="$ROOT_DIR/bin/tests"
BUILD_DIR="$ROOT_DIR/build/make/rise"
JOBS="${RISE_TEST_BUILD_JOBS:-$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}"
cd "$ROOT_DIR" || exit 2

run_extended() {
	local name="$1"
	shift
	local test_path="$BIN_DIR/$name"
	if ! make -s -j "$JOBS" -C "$BUILD_DIR" --no-print-directory "build-test/$name"; then
		echo "failed to build current $name" >&2
		return 2
	fi
	if [ ! -x "$test_path" ]; then
		echo "build succeeded without producing $test_path" >&2
		return 2
	fi
	echo "=== $name $* ==="
	"$test_path" "$@"
}

# These render-heavy experiments deliberately run one at a time. They retain
# the original high-sample matrices that are unsuitable for the per-commit
# full per-commit suite.
run_extended AutoRasterizerTest --extended-fire-ablation --fire-preview-only || exit $?
run_extended PathTracingThermalEmissionTest --extended-matrix || exit $?
