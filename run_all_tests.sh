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
# The default is per checkout (a checksum of its path), so runs in two
# worktrees at once do not delete each other's logs and run state.
REPO_SUM="$(printf '%s' "$REPO_ROOT" | cksum | cut -d' ' -f1)"
LOG_DIR="${RISE_TEST_LOG_DIR:-${TMPDIR:-/tmp}/rise-tests-logs-$REPO_SUM}"
# Absolute (against the caller's directory): tests get TMPDIRs under it and
# some chdir before using them, and the runner changes to the repo root below.
case "$LOG_DIR" in
	/*) ;;
	*) LOG_DIR="$(pwd)/$LOG_DIR" ;;
esac
# A caller's relative RISE_OPTIONS_FILE likewise means their directory.
if [ -n "${RISE_OPTIONS_FILE:-}" ]; then
	case "$RISE_OPTIONS_FILE" in
		/*) ;;
		*) RISE_OPTIONS_FILE="$(pwd)/$RISE_OPTIONS_FILE"; export RISE_OPTIONS_FILE ;;
	esac
fi
# Tests open scenes/..., write rendered/ and read global.options relative to
# the working directory, so run them from the repo root wherever the script
# was started (e.g. `make test` starts it in build/make/rise).
cd "$REPO_ROOT"
# Parallel build jobs for the library and bulk-test build phases.  Without
# this, a change to a widely-included header (IJob.h, RISE_API.h, ...) meant
# the library recompiled one file at a time AND all ~131 test binaries
# relinked one at a time — tens of minutes that looked like "the tests are
# slow" (the test RUNS are seconds).  Override with RISE_TEST_BUILD_JOBS.
JOBS="${RISE_TEST_BUILD_JOBS:-$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}"
FILTER="${RISE_TEST_FILTER:-}"
TIER="${RISE_TEST_TIER:-all}"
# How many test binaries may RUN at once (Phase 3).  Most suites render tiny
# films on one core, so running them side by side cuts wall time several-fold
# without changing any result.  Tests marked `exclusive` in
# tests/test_schedule.tsv (multi-core renders, wall-clock gates) always run
# alone.  1 runs one test at a time in alphabetical order (single tests still
# get one render thread).  Override with --jobs or
# RISE_TEST_RUN_JOBS.
RUN_JOBS="${RISE_TEST_RUN_JOBS:-}"

while [ "$#" -gt 0 ]; do
	case "$1" in
		--filter|-Filter)
			[ "$#" -ge 2 ] || { echo "Missing value for $1" >&2; exit 64; }
			FILTER="$2"
			shift 2
			;;
		--tier|-Tier)
			[ "$#" -ge 2 ] || { echo "Missing value for $1" >&2; exit 64; }
			TIER="$2"
			shift 2
			;;
		--jobs|-Jobs)
			[ "$#" -ge 2 ] || { echo "Missing value for $1" >&2; exit 64; }
			RUN_JOBS="$2"
			shift 2
			;;
		--timeout|-TimeoutSeconds)
			[ "$#" -ge 2 ] || { echo "Missing value for $1" >&2; exit 64; }
			RISE_TEST_TIMEOUT="$2"
			shift 2
			;;
		-h|--help)
			echo "Usage: $0 [--filter <glob1,glob2,...>] [--tier all|fast|extended] [--jobs <n>] [--timeout <sec>]"
			exit 0
			;;
		*)
			echo "Unknown argument: $1" >&2
			exit 64
			;;
	esac
done

# Clear any caller-inherited RISE_MEDIA_PATH (mirroring run_all_tests.ps1) so
# tests running from repo root resolve relative `scenes/...` and `models/...`
# paths cleanly even if the user shell set RISE_MEDIA_PATH without a trailing
# separator or pointing at another checkout.
unset RISE_MEDIA_PATH || true

case "$TIER" in
	all)
		export RISE_TEST_EXTENDED="${RISE_TEST_EXTENDED:-1}"
		;;
	fast)
		export RISE_TEST_EXTENDED=0
		;;
	extended)
		export RISE_TEST_EXTENDED=1
		;;
	*)
		echo "Invalid --tier '$TIER' (expected all, fast, or extended)" >&2
		exit 64
		;;
esac

if [ -z "$RUN_JOBS" ]; then
	RUN_JOBS="$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 1 )"
fi
case "$RUN_JOBS" in
	''|*[!0-9]*|0)
		echo "Invalid --jobs '$RUN_JOBS' (expected a positive integer)" >&2
		exit 64
		;;
esac

# Heavy multi-minute statistical/integration sweep suites assigned exclusively
# to the extended tier when --tier fast or --tier extended is requested.
# Rule: a test that takes more than about 200 s on its own does not belong in
# the fast tier.  Put it here, or give it a cheap fast mode (has_extended_mode
# below).  Keep this list and run_all_tests.ps1's $ExtendedTests identical.
is_extended_test() {
	case "$1" in
		AgentProposeRenderTest|AgentViewModeRenderTest|BDPTDepthCapMISTest|\
		BDPTGuidedContinuationTest|BDPTStrategyBalanceTest|CompositeEnergyConservationTest|\
		ConnectionTypeSplitTest|CstDeriveGoldenTest|ExteriorIndexInvarianceTest|\
		GradedIndexInteriorFactorTest|LayeredWhiteFurnaceTest|\
		MediumEnclosureConnectibilityTest|MediumInsideOutsideInvariantTest|\
		MotionBlurTimeAverageTest|OIDNAutoDeterminismTest|OpenSheetIndexConventionTest|\
		PTGuidingMISPartitionTest|RandomWalkFogEnergyTest|RefractiveRadianceScalingTest|\
		SignalIntegratorConsistencyTest|SMSDisplacedEnergyTest|SMSEmitterDirectionTest|\
		SMSExtendedHWSSTest|SMSExtendedPartitionTest|SMSExtendedReferenceTest|\
		SMSFourVertexVarianceTest|SMSHWSSNestedEnergyTest|SMSLegacyModeTest|\
		SMSMediumAnchorTest|SMSPhotonSeedEnergyTest|SMSSupersededRowsTest|\
		SMSUniformDispersionTest|SpectralOnlyEmitterTest|SSSContactContinuationTest|\
		SSSExteriorIndexInvarianceTest|UnlistedEmitterWeightTest|VCMStrategyBalanceTest|\
		VCMVolumeMergeTest|VolumeAbsorptionAttenuationTest|WeaveGapShadowTransmittanceTest)
			return 0
			;;
		*)
			return 1
			;;
	esac
}

# Dual-mode suites that run fast smoke/structural checks under --tier fast
# (RISE_TEST_EXTENDED=0) and full extended workloads under --tier extended / all
# (RISE_TEST_EXTENDED=1).
has_extended_mode() {
	case "$1" in
		AgentSkillsTest|InteriorCandidateWalkCostTest|LightExitanceTest|\
		LuminarySamplerAreaTest|PavilionColonnadeShowcaseTest|PolishedBRDFConsistencyTest|\
		SchlickSPFPdfConsistencyTest|ShelfBunnyShowcaseTest|SPFPdfConsistencyTest|\
		TidalStonesShowcaseTest|TimeIndexedMotionBlurTest)
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
	if [ "$TIER" = "extended" ] && ! is_extended_test "$tname" && ! has_extended_mode "$tname"; then
		return 1
	fi
	if [ -z "$FILTER" ]; then
		return 0
	fi
	rest="$FILTER,"
	while [ -n "$rest" ]; do
		pat="${rest%%,*}"
		rest="${rest#*,}"
		[ -n "$pat" ] || continue
		# shellcheck disable=SC2254
		case "$tname" in
			$pat) return 0 ;;
		esac
	done
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
# name, wall s, CPU s (user+sys), mean cores, schedule class -- written when
# /usr/bin/time is available.  A `pool` test whose mean cores is well above 1
# belongs in tests/test_schedule.tsv as `exclusive`; the summary flags those.
CPU_TSV="$LOG_DIR/.cpu.tsv"
: > "$BUILD_FAIL_TSV"
: > "$RUN_FAIL_TSV"
: > "$TIMINGS_TSV"
: > "$CPU_TSV"

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
# `timeout` (Linux, recent macOS) or `gtimeout` (coreutils) on PATH.
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
	if [ -n "${run_wall:-}" ] && [ "$found" -gt 0 ]; then
		run_sum=$(LC_ALL=C awk -F"$TAB" '{ s += $1 } END { printf "%d", s }' "$TIMINGS_TSV")
		echo "Time:  ${run_wall}s wall for ${run_sum}s of test time (--jobs $RUN_JOBS)"
	fi

	if [ "$found" -gt 1 ] && [ -s "$TIMINGS_TSV" ]; then
		echo
		echo "Slowest tests (s):"
		sort -rn -k1,1 "$TIMINGS_TSV" | head -n 10 | while IFS="$TAB" read -r tdur tname; do
			[ -n "$tname" ] || continue
			printf '  %4ds  %s\n' "$tdur" "$tname"
		done
	fi

	# A pool test that keeps several cores busy competes with its neighbours
	# and breaks the one-multi-core-render-at-a-time rule, so flag it for an
	# `exclusive` entry.  Only meaningful when tests ran side by side.
	if [ "$RUN_JOBS" -gt 1 ] && [ -s "$CPU_TSV" ]; then
		busy=$(LC_ALL=C awk -F"$TAB" '$5 != "exclusive" && $2 >= 20 && $4 + 0 >= 1.6 { printf "  %-46s %-6s %5.1f cores over %ds\n", $1, $5, $4, $2 }' "$CPU_TSV")
		if [ -n "$busy" ]; then
			echo
			echo "Pool / single tests that kept several cores busy (mark them exclusive in tests/test_schedule.tsv):"
			echo "$busy"
		fi
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

# Running tests (Phase 3), one "name<TAB>class<TAB>pid<TAB>start" line each.
RUNNING=""

# Signal a process and all of its descendants: the per-test subshell wraps
# /usr/bin/time, which wraps timeout, which wraps the test binary.
kill_tree() {
	for kt_child in $(pgrep -P "$1" 2>/dev/null); do
		kill_tree "$kt_child"
	done
	kill -TERM "$1" 2>/dev/null || true
}

kill_running() {
	if [ -n "$RUNNING" ]; then
		while IFS="$TAB" read -r rn_name rn_class rn_pid rn_start; do
			[ -n "$rn_pid" ] || continue
			kill_tree "$rn_pid"
		done <<EOF
$RUNNING
EOF
	fi
	RUNNING=""
}

cleanup_interrupted() {
	trap '' INT TERM
	kill_running
	echo
	echo "Interrupted — partial results below:"
	print_summary
	exit 130
}
trap cleanup_interrupted INT TERM

# Any other way out (set -e on an unexpected failure) must not leave tests
# running behind a dead runner.
trap kill_running EXIT

# ============================================================
# Phase 1: Build the library archive used to link tests (`make test-objs`:
# bin/librise_test.a, a non-LTO copy, where the config uses LTO; see the
# Makefile).
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

	# Fast up-to-date short-circuit: when Phase 1.5 succeeded ($bulk_rc == 0),
	# make just verified every selected target with full -MMD header dependency
	# tracking. When Phase 1.5 failed ($bulk_rc != 0), query `make -q` on the
	# binary itself (which honors .d header dependencies as well as .cpp and
	# the test library; the phony-like build-test/% alias always reads stale) before
	# skipping, so a header-induced compile error never masks behind a stale
	# pre-existing binary.
	if [ -x "$test_path" ] \
	   && { [ "$bulk_rc" -eq 0 ] || make -q -C "$BUILD_DIR" --no-print-directory "../../../bin/tests/$name" >/dev/null 2>&1; }
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
#
# Tests run up to $RUN_JOBS at a time.  tests/test_schedule.tsv names the
# tests that must run ALONE (`exclusive`: multi-core renders, wall-clock
# gates), the tests that share the cores with their render threads forced to
# one (`single`: their renders would spread over a few cores without gaining
# throughput), and the last measured run time of the long ones, so long tests
# start first.  Exclusive tests run first, one at a time on an otherwise idle
# machine; the rest then share the cores.  Unlisted tests are `pool`.
#
# Every test gets its own TMPDIR ($LOG_DIR/.tmp/<name>), so two different
# tests that pick the same scratch-file name cannot collide.  Like the log, it
# is removed when the test passes and kept when it fails.
# ============================================================
SCHEDULE_FILE="$SRC_DIR/test_schedule.tsv"
PLAN="$LOG_DIR/.plan"
DONE_DIR="$LOG_DIR/.done"
RUSAGE_DIR="$LOG_DIR/.rusage"
TMP_ROOT="$LOG_DIR/.tmp"
mkdir -p "$DONE_DIR" "$RUSAGE_DIR" "$TMP_ROOT"

selected="$LOG_DIR/.selected"
: > "$selected"
for test_src in "$SRC_DIR"/*.cpp; do
	[ -f "$test_src" ] || continue
	name="$(basename "$test_src" .cpp)"
	matches_selection "$name" || continue
	printf '%s\n' "$name" >> "$selected"
done

# Plan lines: class<TAB>seconds<TAB>name.  An unknown class is treated as
# exclusive (the safe choice) with a warning.  With --jobs 1 the plan keeps
# the old alphabetical order; otherwise exclusive tests come first, then the
# rest longest first.
schedule_src=""
[ -f "$SCHEDULE_FILE" ] && schedule_src="$SCHEDULE_FILE"
awk -F"$TAB" -v OFS="$TAB" -v sched="$schedule_src" '
	BEGIN {
		# Read the schedule with getline: an awk NR == FNR two-file idiom
		# would treat the selected list as the schedule when the schedule
		# file is empty.
		while (sched != "" && (getline line < sched) > 0) {
			if (line ~ /^[ \t]*(#|$)/) continue
			n = split(line, f, "\t")
			if (f[2] != "pool" && f[2] != "exclusive" && f[2] != "single") {
				printf "warning: tests/test_schedule.tsv: unknown class \"%s\" for %s; running it exclusive\n", f[2], f[1] > "/dev/stderr"
				f[2] = "exclusive"
			}
			cls[f[1]] = f[2]; sec[f[1]] = (n >= 3) ? f[3] + 0 : 0
		}
	}
	{ c = ($1 in cls) ? cls[$1] : "pool"; t = ($1 in sec) ? sec[$1] : 0; print (c == "exclusive") ? 0 : 1, c, t, $1 }
' "$selected" > "$PLAN.unsorted"
if [ "$RUN_JOBS" -eq 1 ]; then
	cut -f2- "$PLAN.unsorted" > "$PLAN"
else
	sort -t "$TAB" -k1,1n -k3,3rn -k4,4 "$PLAN.unsorted" | cut -f2- > "$PLAN"
fi

# `single` tests read this options file: the repo's options (or the caller's
# RISE_OPTIONS_FILE) plus force_number_of_threads 1.
# The force line goes FIRST: Options keeps the first occurrence of a key.
SINGLE_OPTIONS="$LOG_DIR/.single-thread.options"
printf 'force_number_of_threads 1\n' > "$SINGLE_OPTIONS"
if [ -n "${RISE_OPTIONS_FILE:-}" ] && [ -f "$RISE_OPTIONS_FILE" ]; then
	cat "$RISE_OPTIONS_FILE" >> "$SINGLE_OPTIONS"
elif [ -f "$REPO_ROOT/global.options" ]; then
	cat "$REPO_ROOT/global.options" >> "$SINGLE_OPTIONS"
fi
plan_count=$(wc -l < "$PLAN" | tr -d ' ')

time_bin=""
if [ -x /usr/bin/time ] && /usr/bin/time -p -o /dev/null true >/dev/null 2>&1; then
	time_bin=/usr/bin/time
fi

# Start one test in the background.  The subshell writes the exit status to
# $DONE_DIR/<name> when the test finishes; the loop below polls for it.
launch_test() {
	lt_name="$1"
	lt_path="$BIN_DIR/$lt_name"
	lt_log="$LOG_DIR/$lt_name.log"
	lt_tmp="${TMP_ROOT:?}/${lt_name:?}"
	rm -rf "$lt_tmp"
	mkdir -p "$lt_tmp"
	(
		TMPDIR="$lt_tmp"
		export TMPDIR
		if [ "$2" = "single" ]; then
			RISE_OPTIONS_FILE="$SINGLE_OPTIONS"
			export RISE_OPTIONS_FILE
		fi
		lt_rc=0
		if [ -n "$time_bin" ] && [ -n "$timeout_bin" ]; then
			"$time_bin" -p -o "$RUSAGE_DIR/$lt_name" "$timeout_bin" "$RISE_TEST_TIMEOUT" "$lt_path" >"$lt_log" 2>&1 || lt_rc=$?
		elif [ -n "$time_bin" ]; then
			"$time_bin" -p -o "$RUSAGE_DIR/$lt_name" "$lt_path" >"$lt_log" 2>&1 || lt_rc=$?
		elif [ -n "$timeout_bin" ]; then
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$lt_path" >"$lt_log" 2>&1 || lt_rc=$?
		else
			"$lt_path" >"$lt_log" 2>&1 || lt_rc=$?
		fi
		printf '%d\n' "$lt_rc" > "$DONE_DIR/$lt_name.part"
		mv "$DONE_DIR/$lt_name.part" "$DONE_DIR/$lt_name"
	) &
	RUNNING="$RUNNING$lt_name$TAB$2$TAB$!$TAB$(date +%s)
"
}

# Report one finished test.  Wall time comes from /usr/bin/time when present
# (exact), else from the launch timestamp (to within the poll interval).
finish_test() {
	ft_name="$1"; ft_class="$2"; ft_pid="$3"; ft_start="$4"
	wait "$ft_pid" 2>/dev/null || true
	ft_rc=$(cat "$DONE_DIR/$ft_name" 2>/dev/null || echo 1)
	ft_dur=$(( $(date +%s) - ft_start ))
	ft_cpu=""
	if [ -s "$RUSAGE_DIR/$ft_name" ]; then
		# /usr/bin/time writes its numbers in the caller's locale (a comma
		# decimal under de_DE); read them as C-locale dot decimals, and do all
		# formatting here, none with the shell's locale-aware printf.
		ft_line=$(LC_ALL=C awk '{ gsub( ",", ".", $2 ) } $1 == "real" { r = $2 + 0 } $1 == "user" { u = $2 + 0 } $1 == "sys" { s = $2 + 0 }
			END { if (r != "") { c = (r > 0) ? (u + s) / r : 0; printf "%d %.1f %.2f %.1f", r + 0.5, u + s, c, c } }' "$RUSAGE_DIR/$ft_name")
		if [ -n "$ft_line" ]; then
			# shellcheck disable=SC2086
			set -- $ft_line
			ft_dur="$1"
			printf '%s\t%s\t%s\t%s\t%s\n' "$ft_name" "$1" "$2" "$3" "$ft_class" >> "$CPU_TSV"
			ft_cpu=", $4 cores"
		fi
	fi
	printf '%d\t%s\n' "$ft_dur" "$ft_name" >> "$TIMINGS_TSV"
	done_count=$((done_count + 1))
	ft_prefix="$(printf '[ %3d/%3d ] %-46s' "$done_count" "$total" "$ft_name")"
	if [ "$ft_rc" -eq 0 ]; then
		printf '%s PASS (%ds%s)\n' "$ft_prefix" "$ft_dur" "$ft_cpu"
		rm -f "${LOG_DIR:?}/${ft_name:?}.log"
		rm -rf "${TMP_ROOT:?}/${ft_name:?}"
		passed=$((passed + 1))
	else
		printf '%s FAIL (exit=%d, %ds%s)\n' "$ft_prefix" "$ft_rc" "$ft_dur" "$ft_cpu"
		printf '%s\t%d\t%s\n' "$ft_name" "$ft_rc" "$LOG_DIR/$ft_name.log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
}

nap() {
	sleep 0.2 2>/dev/null || sleep 1
}

echo "Running $plan_count test(s), up to $RUN_JOBS at a time (exclusive tests alone)"
run_start=$(date +%s)
done_count=0
next=1
running_count=0
exclusive_running=0
while :; do
	# Reap finished tests.
	if [ "$running_count" -gt 0 ]; then
		still=""
		while IFS="$TAB" read -r rn_name rn_class rn_pid rn_start; do
			[ -n "$rn_name" ] || continue
			if [ ! -f "$DONE_DIR/$rn_name" ] && ! kill -0 "$rn_pid" 2>/dev/null && [ ! -f "$DONE_DIR/$rn_name" ]; then
				# The runner subshell is gone but left no status (its marker
				# could not be written, e.g. the log dir vanished): a failure.
				mkdir -p "$DONE_DIR" 2>/dev/null || true
				echo 1 > "$DONE_DIR/$rn_name" 2>/dev/null || true
				echo "warning: $rn_name exited without recording its status; counting it as failed" >&2
			fi
			if [ -f "$DONE_DIR/$rn_name" ] || ! kill -0 "$rn_pid" 2>/dev/null; then
				finish_test "$rn_name" "$rn_class" "$rn_pid" "$rn_start"
				running_count=$((running_count - 1))
				if [ "$rn_class" = "exclusive" ]; then
					exclusive_running=0
				fi
			else
				still="$still$rn_name$TAB$rn_class$TAB$rn_pid$TAB$rn_start
"
			fi
		done <<EOF
$RUNNING
EOF
		RUNNING="$still"
	fi

	# Start as many as the schedule allows: an exclusive test only on an idle
	# machine, and nothing beside a running exclusive test.
	while [ "$next" -le "$plan_count" ]; do
		# Nothing can start beside an exclusive test or in a full pool.
		if [ "$running_count" -gt 0 ] && { [ "$exclusive_running" -eq 1 ] || [ "$running_count" -ge "$RUN_JOBS" ]; }; then
			break
		fi
		plan_line=$(sed -n "${next}p" "$PLAN")
		pl_class=$(printf '%s\n' "$plan_line" | cut -f1)
		pl_name=$(printf '%s\n' "$plan_line" | cut -f3)
		if [ ! -x "$BIN_DIR/$pl_name" ]; then
			done_count=$((done_count + 1))
			printf '[ %3d/%3d ] %-46s SKIP (build failed)\n' "$done_count" "$total" "$pl_name"
			skipped=$((skipped + 1))
			next=$((next + 1))
			continue
		fi
		if [ "$running_count" -gt 0 ]; then
			[ "$exclusive_running" -eq 0 ] || break
			[ "$pl_class" != "exclusive" ] || break
			[ "$running_count" -lt "$RUN_JOBS" ] || break
		fi
		found=$((found + 1))
		launch_test "$pl_name" "$pl_class"
		running_count=$((running_count + 1))
		if [ "$pl_class" = "exclusive" ]; then
			exclusive_running=1
		fi
		next=$((next + 1))
	done

	if [ "$running_count" -eq 0 ] && [ "$next" -gt "$plan_count" ]; then
		break
	fi
	nap
done
run_wall=$(( $(date +%s) - run_start ))

print_summary

if [ "$failed" -ne 0 ] || [ "$build_failed" -ne 0 ] || [ "$bulk_rc" -ne 0 ]; then
	exit 1
fi
echo "All $found tests passed"
