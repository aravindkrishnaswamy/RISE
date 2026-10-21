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
export RISE_RED_PROVE_MARKER="$MARKER"
SRCS=("$@")
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

build() {
	make -C build/make/rise -j8 all      >/tmp/red_prove_build.log 2>&1 \
	&& make -C build/make/rise build-test/"$TEST" >>/tmp/red_prove_build.log 2>&1
}

TEST_LOG="$(mktemp "${TMPDIR:-/tmp}/red_prove_test.XXXXXX")"
TEST_RC=0
run_test() {
	: > "$TEST_LOG"
	TEST_RC=0
	if [ -n "$TEST_ARGS" ]; then
		local -a test_args_arr=()
		read -r -a test_args_arr <<< "$TEST_ARGS"
		./bin/tests/"$TEST" "${test_args_arr[@]}" >"$TEST_LOG" 2>&1 || TEST_RC=$?
	else
		./bin/tests/"$TEST" >"$TEST_LOG" 2>&1 || TEST_RC=$?
	fi
}

is_uint() {
	[[ "${1:-}" =~ ^[0-9]+$ ]]
}

extract_pass_count() {
	awk '
		match($0, /(^|[^0-9])[0-9]+ passed/) {
			s = substr($0, RSTART, RLENGTH)
			sub(/^[^0-9]+/, "", s)
			sub(/[^0-9].*$/, "", s)
			val = s
		}
		END { if (val != "") print val }
	' "$1"
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
	if ! RED_MARKER_FAILS="$(
		awk '
			BEGIN { marker = ENVIRON["RISE_RED_PROVE_MARKER"] }
			/(^|[[:space:]\[])FAIL([[:space:]:\]]|$)/ && index($0, marker) > 0 { c++ }
			END { print c + 0 }
		' "$TEST_LOG"
	)"; then
		return 1
	fi
	if ! is_uint "$RED_MARKER_FAILS" || [ "$RED_MARKER_FAILS" -lt 1 ]; then
		return 1
	fi
	if ! RED_PASSED="$(extract_pass_count "$TEST_LOG")"; then
		return 1
	fi
	if [ -n "$RED_PASSED" ] && ! is_uint "$RED_PASSED"; then
		return 1
	fi
	return 0
}

check_green_phase() {
	run_test
	if [ "$TEST_RC" -ne 0 ]; then
		echo "GREEN ✗  '${TEST}' exited with non-zero status ${TEST_RC}"
		return 1
	fi

	local stats="" marker_fails="" any_fails="" marker_ok_lines=""
	if ! stats="$(
		awk '
			BEGIN { marker = ENVIRON["RISE_RED_PROVE_MARKER"] }
			{
				is_fail = ($0 ~ /(^|[[:space:]\[])FAIL([[:space:]:\]]|$)/)
				has_marker = (index($0, marker) > 0)
				is_ok = (!is_fail && $0 ~ /(^|[[:space:]\[])(ok:|OK:|PASS|PASSED|pass:|passed:|✓)([[:space:]:\]]|$)/)
				if (is_fail) {
					any_fails++
					if (has_marker) marker_fails++
				} else if (is_ok && has_marker) {
					marker_ok_lines++
				}
			}
			END {
				printf "%d %d %d\n",
					marker_fails + 0, any_fails + 0, marker_ok_lines + 0
			}
		' "$TEST_LOG"
	)"; then
		echo "GREEN ✗  failed to parse GREEN test output"
		return 1
	fi
	read -r marker_fails any_fails marker_ok_lines <<< "$stats"
	if ! is_uint "$marker_fails" || ! is_uint "$any_fails" || ! is_uint "$marker_ok_lines"; then
		echo "GREEN ✗  invalid parser counters (${stats})"
		return 1
	fi

	if [ "$marker_fails" -gt 0 ]; then
		echo "GREEN ✗  '${MARKER}' STILL fails with the fix"
		return 1
	fi
	if [ "$any_fails" -gt 0 ]; then
		echo "GREEN ✗  '${TEST}' reported another failure in GREEN output"
		return 1
	fi

	local green_passed=""
	if ! green_passed="$(extract_pass_count "$TEST_LOG")"; then
		echo "GREEN ✗  failed to parse GREEN pass count"
		return 1
	fi
	if [ -n "$green_passed" ] && ! is_uint "$green_passed"; then
		echo "GREEN ✗  invalid GREEN pass count (${green_passed})"
		return 1
	fi
	if [ -n "$green_passed" ] && [ -n "$RED_PASSED" ]; then
		local min_expected=$(( RED_PASSED + RED_MARKER_FAILS ))
		if [ "$green_passed" -lt "$min_expected" ]; then
			echo "GREEN ✗  pass count (${green_passed}) did not increase by >= ${RED_MARKER_FAILS} over RED (${RED_PASSED}) -- assertion may have been skipped"
			return 1
		fi
	fi

	# Require positive evidence tied to MARKER in GREEN output: at least
	# RED_MARKER_FAILS explicit pass lines (ok:/PASS/PASSED/✓) containing MARKER.
	if [ "$marker_ok_lines" -ge "$RED_MARKER_FAILS" ]; then
		return 0
	fi
	echo "GREEN ✗  expected >= ${RED_MARKER_FAILS} passing line(s) for '${MARKER}', saw ${marker_ok_lines}"
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

cleanup() {
	restore_stash >/dev/null 2>&1 || true
	rm -f "$TEST_LOG"
}
trap cleanup EXIT

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

if build; then
	if check_red_phase; then echo "RED  ✓  '${MARKER}' FAILS without the fix"
	else echo "RED  ✗  '${MARKER}' did NOT fail cleanly without the fix (rc=${TEST_RC}) -- test may be FALSE-GREEN"; rc=1; fi
else echo "RED build failed (see /tmp/red_prove_build.log)"; rc=2; fi
restore_stash || rc=2

echo "== GREEN phase: fix restored =="
if build; then
	if check_green_phase; then echo "GREEN ✓  '${MARKER}' passes with the fix"
	else rc=1; fi
else echo "GREEN build failed (see /tmp/red_prove_build.log)"; rc=2; fi

[ "$rc" = "0" ] && echo "RED-PROVEN ✓" || echo "NOT RED-PROVEN ✗ (rc=$rc)"
exit "$rc"
