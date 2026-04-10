#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$SCRIPT_DIR"
BIN_DIR="$REPO_ROOT/bin/tests"
SRC_DIR="$REPO_ROOT/tests"

if [ ! -d "$BIN_DIR" ]; then
	echo "Missing test folder: $BIN_DIR"
	echo "Build tests first with: make -C $REPO_ROOT/build/make/rise tests"
	exit 1
fi

stale_object_count="$(find "$SRC_DIR" -type f -name '*.o' -print | wc -l | awk '{print $1}')"
if [ "$stale_object_count" -ne 0 ]; then
	echo "Removing $stale_object_count stray test object file(s) from $SRC_DIR"
	find "$SRC_DIR" -type f -name '*.o' -exec rm -f -- {} +
fi

# Only run binaries that have a matching source file in tests/.
# This skips stale build artifacts (e.g., binaries with spaces in
# the name or leftover from renamed/deleted tests).
found=0
failed=0

for test_src in "$SRC_DIR"/*.cpp; do
	[ -f "$test_src" ] || continue
	test_name="$(basename "$test_src" .cpp)"
	test_path="$BIN_DIR/$test_name"

	if [ ! -x "$test_path" ]; then
		echo "SKIP: $test_name (not built)"
		continue
	fi

	found=$((found + 1))
	echo "Running $test_name"
	if ! "$test_path"; then
		echo "FAIL: $test_name"
		failed=$((failed + 1))
	fi
done

if [ "$found" -eq 0 ]; then
	echo "No test binaries found in: $BIN_DIR"
	echo "Build tests first with: make -C $REPO_ROOT/build/make/rise tests"
	exit 1
fi

if [ "$failed" -ne 0 ]; then
	echo "$failed of $found tests FAILED"
	exit 1
fi

echo "All $found tests passed"
