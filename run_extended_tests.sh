#!/usr/bin/env bash

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN_DIR="$ROOT_DIR/bin/tests"

run_extended() {
	local name="$1"
	shift
	local test_path="$BIN_DIR/$name"
	if [ ! -x "$test_path" ]; then
		echo "missing $test_path; run: make -C build/make/rise tests" >&2
		return 2
	fi
	echo "=== $name $* ==="
	"$test_path" "$@"
}

# These render-heavy experiments deliberately run one at a time. They retain
# the original high-sample matrices that are unsuitable for the per-commit
# 236-test gate.
run_extended AutoRasterizerTest --extended-fire-ablation || exit $?
run_extended PathTracingThermalEmissionTest --extended-matrix || exit $?
