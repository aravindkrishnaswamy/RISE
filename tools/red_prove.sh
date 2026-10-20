#!/usr/bin/env bash
#
# red_prove.sh - prove a bug-fix test is RED without the fix and GREEN with it.
#
# Codifies the discipline learned the hard way during the snapshot/transaction
# work: a bug-fix test is not "done" until you've SEEN it fail without the fix.
# Writing the test after the fix and running it green proves nothing (it may be
# tautological, read the wrong observable, or be -ffast-math-foldable).
#
# Usage:
#   tools/red_prove.sh [--args "<test-args>"] <TestBinaryName> <assertion-marker-substring> <src-file> [<src-file> ...]
#
# It git-stashes ONLY the listed source files (the fix), rebuilds, runs the
# test and asserts the marker FAILS; then restores the fix, rebuilds, and
# asserts the marker PASSES.  Keep the test file(s) OUT of the stash list so
# the new test is present in both phases.
#
# Example:
#   tools/red_prove.sh SceneEditTransactionTest "F7 BUG-2" \
#       src/Library/SceneEditor/SceneEditor.h src/Library/SceneEditor/SceneEditController.cpp
#
set -uo pipefail

TEST_ARGS=""
if [ "${1:-}" = "--args" ]; then
	if [ "$#" -lt 5 ]; then
		echo "usage: $0 [--args \"<test-args>\"] <TestBinaryName> <marker> <src-file> [<src-file> ...]" >&2
		exit 64
	fi
	TEST_ARGS="$2"
	shift 2
fi

if [ "$#" -lt 3 ]; then
	echo "usage: $0 [--args \"<test-args>\"] <TestBinaryName> <marker> <src-file> [<src-file> ...]" >&2
	exit 64
fi

TEST="$1"; MARKER="$2"; shift 2
SRCS=("$@")
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

build() {
	make -C build/make/rise -j8 all      >/tmp/red_prove_build.log 2>&1 \
	&& make -C build/make/rise build-test/"$TEST" >>/tmp/red_prove_build.log 2>&1
}

TEST_OUTPUT=""
TEST_RC=0
run_test() {
	TEST_OUTPUT=""
	TEST_RC=0
	if [ -n "$TEST_ARGS" ]; then
		local -a test_args_arr=()
		read -r -a test_args_arr <<< "$TEST_ARGS"
		TEST_OUTPUT="$(./bin/tests/"$TEST" "${test_args_arr[@]}" 2>&1)" || TEST_RC=$?
	else
		TEST_OUTPUT="$(./bin/tests/"$TEST" 2>&1)" || TEST_RC=$?
	fi
}

extract_pass_count() {
	printf '%s\n' "$1" \
		| sed -nE 's/.*(^|[^0-9])([0-9]+) passed.*/\2/p' \
		| tail -n 1
}

RED_MARKER_FAILS=0
RED_PASSED=""

check_red_phase() {
	run_test
	# Require a normal non-zero assertion failure (not exit 0 and not a signal crash >= 128)
	# that prints FAIL matching the requested marker.
	if [ "$TEST_RC" -eq 0 ] || [ "$TEST_RC" -ge 128 ]; then
		return 1
	fi
	RED_MARKER_FAILS="$(
		printf '%s\n' "$TEST_OUTPUT" \
			| grep -E '(^|[[:space:]])FAIL(:|[[:space:]])' \
			| grep -F -c -- "$MARKER" || true
	)"
	if [ "${RED_MARKER_FAILS:-0}" -lt 1 ]; then
		return 1
	fi
	RED_PASSED="$(extract_pass_count "$TEST_OUTPUT")"
	return 0
}

check_green_phase() {
	run_test
	if [ "$TEST_RC" -ne 0 ]; then
		echo "GREEN ✗  '${TEST}' exited with non-zero status ${TEST_RC}"
		return 1
	fi
	if printf '%s\n' "$TEST_OUTPUT" \
		| grep -E '(^|[[:space:]])FAIL(:|[[:space:]])' \
		| grep -Fq -- "$MARKER"; then
		echo "GREEN ✗  '${MARKER}' STILL fails with the fix"
		return 1
	fi
	if printf '%s\n' "$TEST_OUTPUT" | grep -qE '(^|[[:space:]])FAIL(:|[[:space:]])'; then
		echo "GREEN ✗  '${TEST}' reported another failure in GREEN output"
		return 1
	fi

	# Require positive evidence that the intended assertion actually ran and passed:
	# - Whenever both RED and GREEN report `<N> passed`, GREEN's pass count must
	#   increase by at least RED_MARKER_FAILS over RED's pass count.
	# - Otherwise, require an explicit pass line mentioning MARKER or a positive
	#   completion summary after MARKER output.
	local green_passed
	green_passed="$(extract_pass_count "$TEST_OUTPUT")"
	if [ -n "$green_passed" ] && [ -n "$RED_PASSED" ]; then
		local min_expected=$(( RED_PASSED + RED_MARKER_FAILS ))
		if [ "$green_passed" -lt "$min_expected" ]; then
			echo "GREEN ✗  pass count (${green_passed}) did not increase by >= ${RED_MARKER_FAILS} over RED (${RED_PASSED}) -- assertion may have been skipped"
			return 1
		fi
		return 0
	fi
	if printf '%s\n' "$TEST_OUTPUT" \
		| grep -E '(^|[[:space:]])(ok:|PASS)([[:space:]]|:|$)' \
		| grep -Fq -- "$MARKER"; then
		return 0
	fi
	if [ -n "$green_passed" ] && [ "$green_passed" -gt 0 ]; then
		return 0
	fi
	if printf '%s\n' "$TEST_OUTPUT" | grep -Fq -- "$MARKER" \
		&& printf '%s\n' "$TEST_OUTPUT" | grep -qE '(ALL PASSED|PASSED)'; then
		return 0
	fi

	echo "GREEN ✗  no evidence that '${MARKER}' assertion executed in GREEN output"
	return 1
}

STASH_OID=""
restore_stash() {
	if [ -n "$STASH_OID" ]; then
		local current_top
		current_top="$(git rev-parse -q --verify refs/stash 2>/dev/null || true)"
		if [ "$current_top" = "$STASH_OID" ]; then
			git stash pop -q || { echo "error: failed to pop stash $STASH_OID" >&2; return 1; }
		else
			echo "error: top stash ($current_top) does not match created stash ($STASH_OID); not popping" >&2
			return 1
		fi
		STASH_OID=""
	fi
}

rc=0

echo "== RED phase: stashing ${#SRCS[@]} source file(s) (the fix) =="
if [ -z "$(git status --porcelain --untracked-files=no -- "${SRCS[@]}")" ]; then
	echo "stash failed: no local tracked changes in specified source file(s): ${SRCS[*]}" >&2
	exit 2
fi
before_stash="$(git rev-parse -q --verify refs/stash 2>/dev/null || true)"
git stash push -q -- "${SRCS[@]}" || { echo "stash failed" >&2; exit 2; }
after_stash="$(git rev-parse -q --verify refs/stash 2>/dev/null || true)"
if [ -z "$after_stash" ] || [ "$after_stash" = "$before_stash" ]; then
	echo "stash failed: git stash did not create a new entry for: ${SRCS[*]}" >&2
	exit 2
fi
STASH_OID="$after_stash"
trap 'restore_stash >/dev/null 2>&1 || true' EXIT

if build; then
	if check_red_phase; then echo "RED  ✓  '${MARKER}' FAILS without the fix"
	else echo "RED  ✗  '${MARKER}' did NOT fail cleanly without the fix (rc=${TEST_RC}) -- test may be FALSE-GREEN"; rc=1; fi
else echo "RED build failed (see /tmp/red_prove_build.log)"; rc=2; fi
restore_stash || rc=2
trap - EXIT

echo "== GREEN phase: fix restored =="
if build; then
	if check_green_phase; then echo "GREEN ✓  '${MARKER}' passes with the fix"
	else rc=1; fi
else echo "GREEN build failed (see /tmp/red_prove_build.log)"; rc=2; fi

[ "$rc" = "0" ] && echo "RED-PROVEN ✓" || echo "NOT RED-PROVEN ✗ (rc=$rc)"
exit "$rc"
