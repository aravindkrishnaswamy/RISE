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
#   tools/red_prove.sh <TestBinaryName> <assertion-marker-substring> <src-file> [<src-file> ...]
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

if [ "$#" -lt 3 ]; then
	echo "usage: $0 <TestBinaryName> <marker> <src-file> [<src-file> ...]" >&2
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
marker_fails() { ./bin/tests/"$TEST" 2>&1 | grep -q "FAIL:.*${MARKER}"; }

rc=0

echo "== RED phase: stashing ${#SRCS[@]} source file(s) (the fix) =="
git stash push -q -- "${SRCS[@]}" || { echo "stash failed"; exit 2; }
trap 'git stash pop -q >/dev/null 2>&1 || true' EXIT
if build; then
	if marker_fails; then echo "RED  ✓  '${MARKER}' FAILS without the fix"
	else echo "RED  ✗  '${MARKER}' did NOT fail without the fix -- test may be FALSE-GREEN"; rc=1; fi
else echo "RED build failed (see /tmp/red_prove_build.log)"; rc=2; fi
git stash pop -q; trap - EXIT

echo "== GREEN phase: fix restored =="
if build; then
	if marker_fails; then echo "GREEN ✗  '${MARKER}' STILL fails with the fix"; rc=1
	else echo "GREEN ✓  '${MARKER}' passes with the fix"; fi
else echo "GREEN build failed (see /tmp/red_prove_build.log)"; rc=2; fi

[ "$rc" = "0" ] && echo "RED-PROVEN ✓" || echo "NOT RED-PROVEN ✗ (rc=$rc)"
exit "$rc"
