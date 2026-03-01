#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$SCRIPT_DIR"
BIN_DIR="$REPO_ROOT/bin/tests"

if [ ! -d "$BIN_DIR" ]; then
	echo "Missing test folder: $BIN_DIR"
	echo "Build tests first with: make -C $REPO_ROOT/build/make/rise tests"
	exit 1
fi

set -- "$BIN_DIR"/*
if [ "$1" = "$BIN_DIR/*" ]; then
	echo "No test binaries found in: $BIN_DIR"
	echo "Build tests first with: make -C $REPO_ROOT/build/make/rise tests"
	exit 1
fi

for test_path in "$BIN_DIR"/*; do
	if [ -f "$test_path" ] && [ -x "$test_path" ]; then
		echo "Running $(basename "$test_path")"
		"$test_path"
	fi
done

echo "All tests passed"
