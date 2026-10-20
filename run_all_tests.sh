#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$SCRIPT_DIR"
BIN_DIR="$REPO_ROOT/bin/tests"
SRC_DIR="$REPO_ROOT/tests"
BUILD_DIR="$REPO_ROOT/build/make/rise"
LIB_DIR="$REPO_ROOT/src/Library"
LIB_ARCHIVE="$REPO_ROOT/bin/librise.a"
# Logs go outside the repo so they survive cloud-sync providers (iCloud,
# Dropbox, OneDrive) that can tombstone hidden build dirs inside synced
# locations like ~/Documents. Override with RISE_TEST_LOG_DIR if needed.
LOG_DIR="${RISE_TEST_LOG_DIR:-${TMPDIR:-/tmp}/rise-tests-logs}"
# Parallel build jobs for the library and bulk-test build phases.  Without
# this, a change to a widely-included header (IJob.h, RISE_API.h, ...) meant
# the library recompiled one file at a time AND all ~131 test binaries
# relinked one at a time — tens of minutes that looked like "the tests are
# slow" (the test RUNS are seconds).  Override with RISE_TEST_BUILD_JOBS.
JOBS="${RISE_TEST_BUILD_JOBS:-$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}"
FILTER="${RISE_TEST_FILTER:-}"
TIER="${RISE_TEST_TIER:-all}"

while [ "$#" -gt 0 ]; do
	case "$1" in
		--filter|-Filter)
			FILTER="$2"
			shift 2
			;;
		--tier|-Tier)
			TIER="$2"
			shift 2
			;;
		--timeout|-TimeoutSeconds)
			RISE_TEST_TIMEOUT="$2"
			shift 2
			;;
		-h|--help)
			echo "Usage: $0 [--filter <glob1,glob2,...>] [--tier all|fast|extended] [--timeout <sec>]"
			exit 0
			;;
		*)
			echo "Unknown argument: $1" >&2
			exit 64
			;;
	esac
done

case "$TIER" in
	all|fast|extended) ;;
	*)
		echo "Invalid --tier '$TIER' (expected all, fast, or extended)" >&2
		exit 64
		;;
esac

# Heavy multi-minute statistical/integration sweep suites assigned to the
# extended tier when --tier fast or --tier extended is requested.
is_extended_test() {
	case "$1" in
		BDPTStrategyBalanceTest|VCMStrategyBalanceTest|SMSExtendedReferenceTest|\
		SMSExtendedPartitionTest|SMSUniformDispersionTest|SMSSupersededRowsTest|\
		SMSFourVertexVarianceTest|PTGuidingMISPartitionTest|BDPTGuidedContinuationTest|\
		CompositeEnergyConservationTest|LayeredWhiteFurnaceTest|\
		VolumeAbsorptionAttenuationTest|MediumInsideOutsideInvariantTest|\
		ExteriorIndexInvarianceTest|SSSExteriorIndexInvarianceTest|\
		CstDeriveGoldenTest|AgentViewModeRenderTest|AgentProposeRenderTest)
			return 0
			;;
		*)
			return 1
			;;
	esac
}

matches_selection() {
	tname="$1"
	if [ "$TIER" = "fast" ] && is_extended_test "$tname"; then
		return 1
	fi
	if [ "$TIER" = "extended" ] && ! is_extended_test "$tname"; then
		return 1
	fi
	if [ -z "$FILTER" ]; then
		return 0
	fi
	old_ifs="$IFS"
	IFS=','
	for pat in $FILTER; do
		IFS="$old_ifs"
		[ -n "$pat" ] || continue
		# shellcheck disable=SC2254
		case "$tname" in
			$pat) return 0 ;;
		esac
	done
	IFS="$old_ifs"
	return 1
}

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
TIMINGS_TSV="$LOG_DIR/.timings.tsv"
: > "$BUILD_FAIL_TSV"
: > "$RUN_FAIL_TSV"
: > "$TIMINGS_TSV"

TAB="$(printf '\t')"

total=0
built=0
build_failed=0
found=0
passed=0
failed=0
skipped=0
bulk_targets=""

# Discover selected test count first so the [ i/N ] display is correctly sized.
for test_src in "$SRC_DIR"/*.cpp; do
	[ -f "$test_src" ] || continue
	tname="$(basename "$test_src" .cpp)"
	if matches_selection "$tname"; then
		total=$((total + 1))
		bulk_targets="$bulk_targets build-test/$tname"
	fi
done

if [ "$total" -eq 0 ]; then
	echo "No matching test sources found in: $SRC_DIR (filter='${FILTER}', tier='${TIER}')"
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

	if [ "$found" -gt 1 ] && [ -s "$TIMINGS_TSV" ]; then
		echo
		echo "Slowest tests (s):"
		sort -rn -k1,1 "$TIMINGS_TSV" | head -n 10 | while IFS="$TAB" read -r tdur tname; do
			[ -n "$tname" ] || continue
			printf '  %4ds  %s\n' "$tdur" "$tname"
		done
	fi

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
# Phase 1: Build the library .o files and librise.a used to link tests.
# ============================================================
lib_log="$LOG_DIR/.library.log"
printf 'Building library ... '
lib_start=$(date +%s)
lib_rc=0
make -s -j "$JOBS" -C "$BUILD_DIR" --no-print-directory test-objs > "$lib_log" 2>&1 || lib_rc=$?
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
# Phase 1.5: Bulk-build selected test binaries IN PARALLEL (the Makefile's
# `tests` and `build-test/%` targets are prerequisite-driven, so -j fans
# the links out). -k keeps building past an individual failure; the per-test
# loop in Phase 2 then short-circuits on everything up to date and re-runs
# make only for the failures, capturing each one's error in its own log.
# ============================================================
bulk_log="$LOG_DIR/.tests-bulk.log"
printf 'Building tests (-j %s) ... ' "$JOBS"
bulk_start=$(date +%s)
bulk_rc=0
if [ -z "$FILTER" ] && [ "$TIER" = "all" ]; then
	make -s -j "$JOBS" -k -C "$BUILD_DIR" --no-print-directory tests > "$bulk_log" 2>&1 || bulk_rc=$?
else
	# shellcheck disable=SC2086
	make -s -j "$JOBS" -k -C "$BUILD_DIR" --no-print-directory $bulk_targets > "$bulk_log" 2>&1 || bulk_rc=$?
fi
bulk_dur=$(( $(date +%s) - bulk_start ))
if [ "$bulk_rc" -eq 0 ]; then
	printf 'done (%ds)\n' "$bulk_dur"
	rm -f "$bulk_log"
else
	printf 'INCOMPLETE (exit=%d, %ds) — failing tests are isolated below\n' "$bulk_rc" "$bulk_dur"
fi
echo

# ============================================================
# Phase 2: Build each test individually with captured output.
# ============================================================
i=0
for test_src in "$SRC_DIR"/*.cpp; do
	[ -f "$test_src" ] || continue
	name="$(basename "$test_src" .cpp)"
	matches_selection "$name" || continue
	i=$((i + 1))
	test_path="$BIN_DIR/$name"
	log="$LOG_DIR/$name.build.log"
	prefix="$(printf '[ %3d/%3d ] %-46s' "$i" "$total" "$name")"

	# Fast up-to-date short-circuit: Phase 1.5 just ran make over the selected
	# targets with full -MMD header dependency tracking, and Phase 1 updated
	# $LIB_ARCHIVE. Checking against $test_src and $LIB_ARCHIVE avoids spawning
	# a `find $LIB_DIR -name '*.o'` subprocess on every iteration.
	if [ -x "$test_path" ] \
	   && { [ "$bulk_rc" -eq 0 ] || { [ "$test_path" -nt "$test_src" ] && [ "$test_path" -nt "$LIB_ARCHIVE" ]; }; }
	then
		printf '%s UP TO DATE\n' "$prefix"
		built=$((built + 1))
		continue
	fi

	printf '%s ... ' "$prefix"
	bs=$(date +%s)
	rc=0
	make -s -j "$JOBS" -C "$BUILD_DIR" --no-print-directory "build-test/$name" > "$log" 2>&1 || rc=$?
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
	name="$(basename "$test_src" .cpp)"
	matches_selection "$name" || continue
	i=$((i + 1))
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
	printf '%d\t%s\n' "$dur" "$name" >> "$TIMINGS_TSV"

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
