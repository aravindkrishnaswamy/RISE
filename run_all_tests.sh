#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$SCRIPT_DIR"
BIN_DIR="$REPO_ROOT/bin/tests"
SRC_DIR="$REPO_ROOT/tests"
BUILD_DIR="$REPO_ROOT/build/make/rise"
LIB_DIR="$REPO_ROOT/src/Library"
# Logs go outside the repo so they survive cloud-sync providers (iCloud,
# Dropbox, OneDrive) that can tombstone hidden build dirs inside synced
# locations like ~/Documents. Override with RISE_TEST_LOG_DIR if needed.
LOG_DIR="${RISE_TEST_LOG_DIR:-${TMPDIR:-/tmp}/rise-tests-logs}"

if [ ! -d "$BUILD_DIR" ]; then
	echo "Missing build directory: $BUILD_DIR"
	exit 1
fi

# Remove orphan .o files only (no matching .cpp). Active .o files are kept
# so the per-test build target can skip up-to-date binaries.
orphan_count=0
for obj in "$SRC_DIR"/*.o; do
	[ -f "$obj" ] || continue
	name="$(basename "$obj" .o)"
	if [ ! -f "$SRC_DIR/$name.cpp" ]; then
		rm -f "$obj"
		orphan_count=$((orphan_count + 1))
	fi
done
if [ "$orphan_count" -ne 0 ]; then
	echo "Removed $orphan_count orphan test object file(s) from $SRC_DIR"
fi

mkdir -p "$BIN_DIR"
rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"
BUILD_FAIL_TSV="$LOG_DIR/.build_failures.tsv"
RUN_FAIL_TSV="$LOG_DIR/.run_failures.tsv"
: > "$BUILD_FAIL_TSV"
: > "$RUN_FAIL_TSV"

TAB="$(printf '\t')"

total=0
built=0
build_failed=0
found=0
passed=0
failed=0
skipped=0

# Discover total test count first so the [ i/N ] display is correctly sized.
for test_src in "$SRC_DIR"/*.cpp; do
	[ -f "$test_src" ] || continue
	total=$((total + 1))
done

if [ "$total" -eq 0 ]; then
	echo "No test sources found in: $SRC_DIR"
	exit 1
fi

# Optional per-test runtime timeout: set RISE_TEST_TIMEOUT=<seconds>. Requires
# `timeout` (Linux) or `gtimeout` (macOS w/ coreutils) on PATH.
timeout_bin=""
if [ -n "${RISE_TEST_TIMEOUT:-}" ]; then
	if command -v timeout >/dev/null 2>&1; then
		timeout_bin="timeout"
	elif command -v gtimeout >/dev/null 2>&1; then
		timeout_bin="gtimeout"
	else
		echo "warning: RISE_TEST_TIMEOUT=$RISE_TEST_TIMEOUT set but neither 'timeout' nor 'gtimeout' is on PATH; running without a timeout"
	fi
fi

print_summary() {
	echo "============================================================"
	echo "Build: $built built, $build_failed failed (of $total)"
	echo "Run:   $passed passed, $failed failed, $skipped skipped (of $found run)"

	if [ "$build_failed" -gt 0 ] && [ -s "$BUILD_FAIL_TSV" ]; then
		echo
		echo "Build failures:"
		while IFS="$TAB" read -r fname frc flog; do
			[ -n "$fname" ] || continue
			echo "  - $fname"
		done < "$BUILD_FAIL_TSV"
	fi

	if [ "$failed" -gt 0 ] && [ -s "$RUN_FAIL_TSV" ]; then
		echo
		while IFS="$TAB" read -r fname frc flog; do
			[ -n "$fname" ] || continue
			echo "--- RUN FAIL: $fname (exit=$frc) — see $flog ---"
			if [ -f "$flog" ]; then
				cat "$flog"
			else
				echo "(log file missing)"
			fi
			echo
		done < "$RUN_FAIL_TSV"
		echo "Run failures:"
		while IFS="$TAB" read -r fname frc flog; do
			[ -n "$fname" ] || continue
			echo "  - $fname"
		done < "$RUN_FAIL_TSV"
	fi
}

cleanup_interrupted() {
	echo
	echo "Interrupted — partial results below:"
	print_summary
	exit 130
}
trap cleanup_interrupted INT TERM

# ============================================================
# Phase 1: Build the library .o files used to link tests.
# ============================================================
lib_log="$LOG_DIR/.library.log"
printf 'Building library ... '
lib_start=$(date +%s)
lib_rc=0
make -s -C "$BUILD_DIR" --no-print-directory test-objs > "$lib_log" 2>&1 || lib_rc=$?
lib_dur=$(( $(date +%s) - lib_start ))
if [ "$lib_rc" -eq 0 ]; then
	printf 'done (%ds)\n' "$lib_dur"
	rm -f "$lib_log"
else
	printf 'FAILED (exit=%d, %ds) — see %s\n' "$lib_rc" "$lib_dur" "$lib_log"
	echo
	if [ -f "$lib_log" ]; then cat "$lib_log"; fi
	exit 1
fi
echo

# ============================================================
# Phase 2: Build each test individually with captured output.
# ============================================================
i=0
for test_src in "$SRC_DIR"/*.cpp; do
	[ -f "$test_src" ] || continue
	i=$((i + 1))
	name="$(basename "$test_src" .cpp)"
	test_path="$BIN_DIR/$name"
	log="$LOG_DIR/$name.build.log"
	prefix="$(printf '[ %3d/%3d ] %-46s' "$i" "$total" "$name")"

	# Fast up-to-date short-circuit: skip the make call (which would otherwise
	# stat the entire $(OBJLIB) tree — ~3s per test) when the binary is
	# already newer than its source and every library .o file.
	if [ -x "$test_path" ] \
	   && [ "$test_path" -nt "$test_src" ] \
	   && [ -z "$(find "$LIB_DIR" -name '*.o' -newer "$test_path" -print -quit 2>/dev/null)" ]
	then
		printf '%s UP TO DATE\n' "$prefix"
		built=$((built + 1))
		continue
	fi

	printf '%s ... ' "$prefix"
	bs=$(date +%s)
	rc=0
	make -s -C "$BUILD_DIR" --no-print-directory "build-test/$name" > "$log" 2>&1 || rc=$?
	bd=$(( $(date +%s) - bs ))

	if [ "$rc" -eq 0 ]; then
		printf 'BUILD PASS (%ds)\n' "$bd"
		rm -f "$log"
		built=$((built + 1))
	else
		printf 'BUILD FAIL (exit=%d, %ds)\n' "$rc" "$bd"
		printf '%s\t%d\t%s\n' "$name" "$rc" "$log" >> "$BUILD_FAIL_TSV"
		build_failed=$((build_failed + 1))
	fi
done
printf 'Build: %d built, %d failed (of %d)\n' "$built" "$build_failed" "$total"

if [ "$build_failed" -gt 0 ] && [ -s "$BUILD_FAIL_TSV" ]; then
	while IFS="$TAB" read -r fname frc flog; do
		[ -n "$fname" ] || continue
		echo
		echo "--- BUILD FAIL: $fname (exit=$frc) — see $flog ---"
		if [ -f "$flog" ]; then cat "$flog"; fi
	done < "$BUILD_FAIL_TSV"
fi
echo

# ============================================================
# Phase 3: Run each built test with captured output.
# ============================================================
i=0
for test_src in "$SRC_DIR"/*.cpp; do
	[ -f "$test_src" ] || continue
	i=$((i + 1))
	name="$(basename "$test_src" .cpp)"
	test_path="$BIN_DIR/$name"
	prefix="$(printf '[ %3d/%3d ] %-46s' "$i" "$total" "$name")"

	if [ ! -x "$test_path" ]; then
		printf '%s SKIP (build failed)\n' "$prefix"
		skipped=$((skipped + 1))
		continue
	fi

	found=$((found + 1))
	log="$LOG_DIR/$name.log"
	printf '%s ... ' "$prefix"

	start_ts=$(date +%s)
	rc=0
	if [ -n "$timeout_bin" ]; then
		"$timeout_bin" "$RISE_TEST_TIMEOUT" "$test_path" >"$log" 2>&1 || rc=$?
	else
		"$test_path" >"$log" 2>&1 || rc=$?
	fi
	end_ts=$(date +%s)
	dur=$((end_ts - start_ts))

	if [ "$rc" -eq 0 ]; then
		printf 'PASS (%ds)\n' "$dur"
		rm -f "$log"
		passed=$((passed + 1))
	else
		printf 'FAIL (exit=%d, %ds)\n' "$rc" "$dur"
		printf '%s\t%d\t%s\n' "$name" "$rc" "$log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
done

print_summary

if [ "$failed" -ne 0 ] || [ "$build_failed" -ne 0 ]; then
	exit 1
fi
echo "All $found tests passed"
