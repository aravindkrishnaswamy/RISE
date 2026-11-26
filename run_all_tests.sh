#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$SCRIPT_DIR"
BIN_DIR="$REPO_ROOT/bin/tests"
SRC_DIR="$REPO_ROOT/tests"
BUILD_DIR="$REPO_ROOT/build/make/rise"
LIB_DIR="$REPO_ROOT/src/Library"
FIRE_OPTICS_GENERATOR="$REPO_ROOT/tools/generate_fire_optics_records.py"
FIRE_OPTICS_DATA="$REPO_ROOT/docs/data"
FIRE_OPTICS_EMBEDDED="$LIB_DIR/Utilities/FireOpticsRecordData.inc"
FIRE_SIM_GENERATOR="$REPO_ROOT/tools/generate_fire_simulation_records.py"
FIRE_SIM_DATA="$REPO_ROOT/docs/data/source_pulls/fire_sim_open_sources_v1.json"
FIRE_SIM_METHANE_CONSTANTS="$REPO_ROOT/docs/data/fire_fuel_methane_v1.draft.json"
FIRE_SIM_EMBEDDED="$LIB_DIR/Utilities/FireSimulationRecordData.inc"
FIRE_SIM_GENERATOR_TEST="$REPO_ROOT/tests/test_fire_simulation_record_generator.py"
FIRE_GAS_OPACITY_GENERATOR="$REPO_ROOT/tools/generate_fire_gas_opacity_record.py"
FIRE_GAS_OPACITY_MANIFEST="$REPO_ROOT/tests/fixtures/fire_gas_opacity/synthetic_manifest.json"
FIRE_GAS_OPACITY_TABLE="$REPO_ROOT/docs/data/fire_gas_opacity_synthetic_v1.json"
FIRE_GAS_OPACITY_TEST="$REPO_ROOT/tests/test_fire_gas_opacity_tools.py"
FIRE_GAS_PLANCK_GENERATOR="$REPO_ROOT/tools/generate_fire_gas_opacity_planck_record.py"
FIRE_GAS_PLANCK_MANIFEST="$REPO_ROOT/docs/data/gas_opacity/hitemp_sources_v1.json"
FIRE_GAS_PLANCK_EMBEDDED="$LIB_DIR/Utilities/FireGasOpacityRecordData.inc"
FIRE_GAS_PLANCK_RECORD="$REPO_ROOT/docs/data/gas_opacity/fire_gas_opacity_hitemp_planck_mean_v1.cbor"
FIRE_GAS_PLANCK_TEST="$REPO_ROOT/tests/test_fire_gas_opacity_planck_record.py"
# Logs go outside the repo so they survive cloud-sync providers (iCloud,
# Dropbox, OneDrive) that can tombstone hidden build dirs inside synced
# locations like ~/Documents. Override with RISE_TEST_LOG_DIR if needed.
# The default is per checkout (a checksum of its path), so runs in two
# worktrees at once do not delete each other's logs and run state.
REPO_SUM="$(printf '%s' "$REPO_ROOT" | cksum | cut -d' ' -f1)"
LOG_DIR="${RISE_TEST_LOG_DIR:-${TMPDIR:-/tmp}/rise-tests-logs-managed-$REPO_SUM}"
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
# without changing any result.  Each test takes one of these run slots, or
# N of them when tests/test_schedule.tsv marks it `wide:N` (multi-core
# renders); tests marked `exclusive` (wall-clock gates) always run alone.
# 1 runs one test at a time in alphabetical order (single tests still get one
# render thread).  Override with --jobs or RISE_TEST_RUN_JOBS.
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
	''|*[!0-9]*|0*)
		# A leading zero is refused too: $(( )) would read it as octal while
		# [ -lt ] and awk read it as decimal.
		echo "Invalid --jobs '$RUN_JOBS' (expected a positive integer without leading zeros)" >&2
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
		MediumEnclosureConnectibilityTest|MediumInsideOutsideInvariantTest)
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
		AgentSkillsTest|ClothValidationTest|FireSolverTest|LightExitanceTest|LiquidPhysicsValidationTest|LiquidRigidValidationTest|LiquidValidationTest|SPFPdfConsistencyTest|TimeIndexedMotionBlurTest)
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

validate_log_dir() {
	log_base="$(basename -- "$LOG_DIR")"
	case "$log_base" in
		''|'/'|'.'|'..')
			echo "Refusing unsafe test log directory: $LOG_DIR" >&2
			exit 1
			;;
	esac
	case "$log_base" in
		rise-tests-logs|rise-tests-logs-*) ;;
		*)
			echo "Refusing non-dedicated test log directory: $LOG_DIR" >&2
			exit 1
			;;
	esac
	log_parent="$(dirname -- "$LOG_DIR")"
	mkdir -p "$log_parent"
	canonical_parent="$(CDPATH= cd -- "$log_parent" && pwd -P)"
	canonical_log="$canonical_parent/$log_base"
	user_profile="$(CDPATH= cd -- && pwd -P)"
	case "$canonical_log" in
		'/'|"$user_profile"|"$REPO_ROOT")
			echo "Refusing unsafe test log directory: $canonical_log" >&2
			exit 1
			;;
	esac
	case "$REPO_ROOT/" in
		"$canonical_log/"*)
			echo "Refusing test log directory that contains the repository: $canonical_log" >&2
			exit 1
			;;
	esac
	case "$canonical_log/" in
		"$REPO_ROOT/"*)
			echo "Refusing test log directory inside the repository: $canonical_log" >&2
			exit 1
			;;
	esac
	marker="$canonical_log/.rise-test-log-directory"
	if [ -d "$canonical_log" ] && [ ! -f "$marker" ] \
	   && [ -n "$(find "$canonical_log" -mindepth 1 -print -quit 2>/dev/null)" ]
	then
		echo "Refusing unowned nonempty test log directory: $canonical_log" >&2
		exit 1
	fi
	LOG_DIR="$canonical_log"
}

validate_log_dir
if [ "${RISE_TEST_VALIDATE_LOG_DIR_ONLY:-0}" = "1" ]; then
	echo "Safe test log directory: $LOG_DIR"
	exit 0
fi

if [ ! -d "$BUILD_DIR" ]; then
	echo "Missing build directory: $BUILD_DIR"
	exit 1
fi

python_bin="$(command -v python3 || command -v python || true)"
if [ -z "$python_bin" ]; then
	echo "Missing Python interpreter required for the fire-optics record parity gate"
	exit 1
fi
printf 'Checking embedded fire-optics records ... '
"$python_bin" "$FIRE_OPTICS_GENERATOR" --check \
	"$FIRE_OPTICS_DATA" "$FIRE_OPTICS_EMBEDDED"
echo "pass"
printf 'Checking embedded fire-simulation records ... '
"$python_bin" "$FIRE_SIM_GENERATOR" --check \
	--methane-constants "$FIRE_SIM_METHANE_CONSTANTS" \
	"$FIRE_SIM_DATA" "$FIRE_SIM_EMBEDDED"
echo "pass"
printf 'Testing physical methane record arithmetic ... '
"$python_bin" "$FIRE_SIM_GENERATOR_TEST"
echo "pass"
printf 'Checking production HITEMP Planck-mean record ... '
"$python_bin" "$FIRE_GAS_PLANCK_GENERATOR" --check \
	"$FIRE_GAS_PLANCK_MANIFEST" "$FIRE_GAS_PLANCK_EMBEDDED" \
	--record "$FIRE_GAS_PLANCK_RECORD"
echo "pass"
printf 'Testing production HITEMP Planck-mean tools ... '
"$python_bin" "$FIRE_GAS_PLANCK_TEST"
echo "pass"
printf 'Checking quarantined synthetic HITEMP LBL record ... '
"$python_bin" "$FIRE_GAS_OPACITY_GENERATOR" --check \
	"$FIRE_GAS_OPACITY_MANIFEST" "$FIRE_GAS_OPACITY_TABLE"
echo "pass"
printf 'Testing quarantined HITEMP LBL tools ... '
"$python_bin" "$FIRE_GAS_OPACITY_TEST"
echo "pass"

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
# One run per checkout at a time: a second run would rebuild and relink the
# test binaries the first is running, write the same scene output, and (with
# the default log dir) delete the first one's logs and per-test TMPDIRs.  The
# lock is a directory holding the owner's pid; one whose owner is gone
# (kill -9, reboot) is taken over.  An empty pid file is an owner still
# writing it, unless the lock is older than a minute.
LOCK_DIR="$REPO_ROOT/bin/.run_all_tests.lock"
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
	lock_owner=$(cat "$LOCK_DIR/pid" 2>/dev/null || true)
	lock_live=0
	if [ -n "$lock_owner" ]; then
		kill -0 "$lock_owner" 2>/dev/null && lock_live=1
	elif [ -z "$(find "$LOCK_DIR" -prune -mmin +1 2>/dev/null)" ]; then
		lock_live=1
	fi
	if [ "$lock_live" -eq 1 ]; then
		echo "Another run_all_tests.sh (pid ${lock_owner:-starting}) is running in this checkout." >&2
		echo "Wait for it to finish; if no run is active, remove $LOCK_DIR." >&2
		exit 1
	fi
	rm -rf "$LOCK_DIR"
	mkdir "$LOCK_DIR" 2>/dev/null || { echo "Cannot create $LOCK_DIR (another run took it?)" >&2; exit 1; }
fi
echo "$$" > "$LOCK_DIR/pid"
release_lock() {
	if [ "$(cat "$LOCK_DIR/pid" 2>/dev/null)" = "$$" ]; then
		rm -rf "$LOCK_DIR"
	fi
}
trap release_lock EXIT
rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"
: > "$LOG_DIR/.rise-test-log-directory"
BUILD_FAIL_TSV="$LOG_DIR/.build_failures.tsv"
RUN_FAIL_TSV="$LOG_DIR/.run_failures.tsv"
TIMINGS_TSV="$LOG_DIR/.timings.tsv"
# Each test's time in the last run that recorded it (name<TAB>seconds; see
# the end of this script for which runs do), kept outside the log dir, which
# every run deletes; it orders the next run.
TIMINGS_HISTORY="$REPO_ROOT/bin/.test_timings.tsv"
# name, wall s, CPU s (user+sys), mean cores, schedule class -- written when
# /usr/bin/time is available.  A test whose mean cores is well above its run
# slots belongs in tests/test_schedule.tsv as `wide:N`; the summary flags those.
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
	tname=${test_src##*/}
	tname=${tname%.cpp}
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

	# A test that keeps more cores busy than the slots it holds competes with
	# its neighbours, so flag it for a wider `wide:N` (or `exclusive`) entry.
	# Only meaningful when tests ran side by side.
	if [ "$RUN_JOBS" -gt 1 ] && [ -s "$CPU_TSV" ]; then
		busy=$(LC_ALL=C awk -F"$TAB" -v jobs="$RUN_JOBS" '
			$5 != "exclusive" && $2 >= 20 {
				s = ($5 ~ /^wide:/) ? substr($5, 6) + 0 : 1
				if (s > jobs) s = jobs
				if ($4 + 0 >= s + 0.6) printf "  %-46s %-7s %5.1f cores over %ds\n", $1, $5, $4, $2
			}' "$CPU_TSV")
		if [ -n "$busy" ]; then
			echo
			echo "Tests that kept more cores busy than their run slots (give them wide:N in tests/test_schedule.tsv):"
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
# /usr/bin/time, which wraps timeout, which wraps the test binary.  ps is
# the fallback where pgrep (procps) is missing, e.g. a slim container.
if command -v pgrep >/dev/null 2>&1; then
	children_of() { pgrep -P "$1" 2>/dev/null; }
else
	children_of() { ps -A -o pid= -o ppid= 2>/dev/null | awk -v p="$1" '$2 == p { print $1 }'; }
fi
kill_tree() {
	for kt_child in $(children_of "$1"); do
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
	trap '' INT TERM HUP QUIT
	kill_running
	echo
	echo "Interrupted — partial results below:"
	print_summary
	exit "$1"
}
trap 'cleanup_interrupted 130' INT
trap 'cleanup_interrupted 143' TERM
trap 'cleanup_interrupted 129' HUP
trap 'cleanup_interrupted 131' QUIT

# Any other way out (set -e on an unexpected failure) must not leave tests
# running behind a dead runner.
trusted_fixture_root=""
cleanup_run() {
	kill_running
	if [ -n "$trusted_fixture_root" ]; then rm -rf "$trusted_fixture_root"; fi
	release_lock
}
trap cleanup_run EXIT

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
	name=${test_src##*/}
	name=${name%.cpp}
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
# Tests share $RUN_JOBS run slots.  tests/test_schedule.tsv names the tests
# that must run ALONE (`exclusive`: wall-clock gates), the tests that take N
# slots (`wide:N`: multi-core renders), the tests that take one slot with
# their render threads forced to one (`single`: their renders would spread
# over a few cores without gaining throughput), and a run time for the long
# ones.  Exclusive tests run first, one at a time on an otherwise idle
# machine; the rest then share the slots, longest first by the recorded
# times ($TIMINGS_HISTORY, else the schedule's times).  Unlisted tests are
# `pool` (one slot).
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
	name=${test_src##*/}
	name=${name%.cpp}
	matches_selection "$name" || continue
	printf '%s\n' "$name" >> "$selected"
done

# Plan lines: class<TAB>slots<TAB>seconds<TAB>name.  slots is how many of
# the $RUN_JOBS run slots the test takes: exclusive all of them, wide:N N
# (at most $RUN_JOBS), pool and single 1.  seconds is the test's recorded
# time ($TIMINGS_HISTORY), else its schedule entry, else 0.
# An unknown class is treated as exclusive (the safe choice) with a warning.
# With --jobs 1 the plan keeps the old alphabetical order; otherwise it is
# the exclusive tests longest first, then the wide tests widest first, then
# the pool and single tests longest first.  The scheduler below starts the
# wide tests ahead of the rest: each fits into the slots the one before it
# frees, so none waits behind a long pool test.
schedule_src=""
[ -f "$SCHEDULE_FILE" ] && schedule_src="$SCHEDULE_FILE"
history_src=""
[ -f "$TIMINGS_HISTORY" ] && history_src="$TIMINGS_HISTORY"
awk -F"$TAB" -v OFS="$TAB" -v sched="$schedule_src" -v hist="$history_src" -v jobs="$RUN_JOBS" '
	BEGIN {
		# Read the schedule with getline: an awk NR == FNR two-file idiom
		# would treat the selected list as the schedule when the schedule
		# file is empty.
		while (sched != "" && (getline line < sched) > 0) {
			if (line ~ /^[ \t]*(#|$)/) continue
			n = split(line, f, "\t")
			if (f[2] != "pool" && f[2] != "exclusive" && f[2] != "single" && f[2] !~ /^wide:[1-9][0-9]*$/) {
				printf "warning: tests/test_schedule.tsv: unknown class \"%s\" for %s; running it exclusive\n", f[2], f[1] > "/dev/stderr"
				f[2] = "exclusive"
			}
			cls[f[1]] = f[2]; sec[f[1]] = (n >= 3) ? f[3] + 0 : 0
		}
		while (hist != "" && (getline line < hist) > 0) {
			sub(/\r$/, "", line)
			split(line, f, "\t")
			if (f[1] != "" && f[2] ~ /^[0-9]+$/) last[f[1]] = f[2] + 0
		}
	}
	{
		c = ($1 in cls) ? cls[$1] : "pool"
		t = ($1 in last) ? last[$1] : (($1 in sec) ? sec[$1] : 0)
		if (c == "exclusive") { k = 0; s = jobs }
		else if (c ~ /^wide:/) { k = 1; s = substr(c, 6) + 0; if (s > jobs) s = jobs }
		else { k = 2; s = 1 }
		print k, c, s, t, $1
	}
' "$selected" > "$PLAN.unsorted"
if [ "$RUN_JOBS" -eq 1 ]; then
	# One queue: every test takes the one slot, in alphabetical order.
	cut -f2- "$PLAN.unsorted" > "$PLAN"
	plan_ex=0
	plan_wide=0
else
	sort -t "$TAB" -k1,1n -k3,3rn -k4,4rn -k5,5 "$PLAN.unsorted" | cut -f2- > "$PLAN"
	plan_ex=$(awk -F"$TAB" '$1 == 0' "$PLAN.unsorted" | wc -l | tr -d ' ')
	plan_wide=$(awk -F"$TAB" '$1 == 1' "$PLAN.unsorted" | wc -l | tr -d ' ')
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
	# These small image-output fixtures exercise trusted staging parents. A
	# group-writable mount point is valid for the suite's bulk scratch but
	# must be refused by the production writer, even below a private folder.
	case "$lt_name" in
		EntityTemplatesTest|FileRasterizerOutputShimTest|FrameEncoderTest|VolumeShortReadTest)
			if [ -z "$trusted_fixture_root" ]; then
				system_tmp=$(CDPATH= cd /tmp && pwd -P)
				trusted_fixture_root=$(mktemp -d "$system_tmp/rise-trusted-fixtures.XXXXXXXX")
			fi
			lt_tmp="$trusted_fixture_root/$lt_name"
			;;
	esac
	rm -rf "$lt_tmp"
	mkdir -p "$lt_tmp"
	(
		TMPDIR="$lt_tmp"
		# Isolate demand-imported frames just like the rest of the test fixtures.
		RISE_SIMULATION_CACHE_DIR="${RISE_SIMULATION_CACHE_DIR:-$lt_tmp/simulation-cache}"
		export TMPDIR RISE_SIMULATION_CACHE_DIR
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
	) 2>>"$lt_log" &
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

echo "Running $plan_count test(s) in $RUN_JOBS run slots (exclusive tests alone)"

# The run slots a running test holds, from its class.
slots_of() {
	case "$1" in
		exclusive) echo "$RUN_JOBS" ;;
		wide:*)
			so_n=${1#wide:}
			[ "$so_n" -le "$RUN_JOBS" ] || so_n=$RUN_JOBS
			echo "$so_n" ;;
		*) echo 1 ;;
	esac
}

# Skip test $1 when it cannot run; returns 0 when it was skipped.  A test
# whose build failed this run is skipped even when an older executable is
# still on disk: that binary is stale.
skip_unrunnable() {
	su_name="$1"
	if [ -s "$BUILD_FAIL_TSV" ] && awk -F '\t' -v test="$su_name" \
		'$1 == test { found=1 } END { exit(found ? 0 : 1) }' "$BUILD_FAIL_TSV"
	then
		done_count=$((done_count + 1))
		printf '[ %3d/%3d ] %-46s SKIP (current build failed; stale executable ignored)\n' "$done_count" "$total" "$su_name"
		skipped=$((skipped + 1))
		return 0
	fi
	if [ ! -x "$BIN_DIR/$su_name" ]; then
		done_count=$((done_count + 1))
		printf '[ %3d/%3d ] %-46s SKIP (build failed)\n' "$done_count" "$total" "$su_name"
		skipped=$((skipped + 1))
		return 0
	fi
	return 1
}

# Start plan line $1 if its test fits in the free slots (or skip it).
# Returns 0 when the line is used up (started or skipped), 1 when it must
# wait for slots.  On an idle machine every test fits: slots never exceed
# $RUN_JOBS.
# The line last read is kept (ts_cached), so a test waiting for slots costs
# no processes on later passes: an exclusive test may be timing something.
ts_cached=""
try_start() {
	if [ "$1" != "$ts_cached" ]; then
		ts_line=$(sed -n "${1}p" "$PLAN")
		ts_class=$(printf '%s\n' "$ts_line" | cut -f1)
		ts_slots=$(printf '%s\n' "$ts_line" | cut -f2)
		ts_name=$(printf '%s\n' "$ts_line" | cut -f4)
		ts_cached="$1"
		if skip_unrunnable "$ts_name"; then
			return 0
		fi
	fi
	if [ "$running_count" -gt 0 ] && [ $((slots_used + ts_slots)) -gt "$RUN_JOBS" ]; then
		return 1
	fi
	found=$((found + 1))
	launch_test "$ts_name" "$ts_class"
	running_count=$((running_count + 1))
	slots_used=$((slots_used + ts_slots))
	return 0
}

run_start=$(date +%s)
done_count=0
running_count=0
slots_used=0
# The plan's three runs of lines: exclusive [1, plan_ex], wide
# (plan_ex, plan_ex + plan_wide], then pool and single to the end.
wide_end=$((plan_ex + plan_wide))
next_ex=1
next_wide=$((plan_ex + 1))
next_narrow=$((wide_end + 1))
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
				slots_used=$((slots_used - $(slots_of "$rn_class")))
			else
				still="$still$rn_name$TAB$rn_class$TAB$rn_pid$TAB$rn_start
"
			fi
		done <<EOF
$RUNNING
EOF
		RUNNING="$still"
	fi

	# Start what the slots allow.  Exclusive tests come first, one at a time
	# on an idle machine; nothing else starts until they are done.  Then the
	# wide tests in plan order (widest first), while the pool and single
	# tests fill whatever slots the next wide test cannot use.
	while [ "$next_ex" -le "$plan_ex" ] && [ "$running_count" -eq 0 ]; do
		try_start "$next_ex" || break
		next_ex=$((next_ex + 1))
	done
	if [ "$next_ex" -gt "$plan_ex" ]; then
		while [ "$next_wide" -le "$wide_end" ] && [ "$slots_used" -lt "$RUN_JOBS" ]; do
			try_start "$next_wide" || break
			next_wide=$((next_wide + 1))
		done
		while [ "$next_narrow" -le "$plan_count" ] && [ "$slots_used" -lt "$RUN_JOBS" ]; do
			try_start "$next_narrow" || break
			next_narrow=$((next_narrow + 1))
		done
	fi

	if [ "$running_count" -eq 0 ] && [ "$next_ex" -gt "$plan_ex" ] \
	   && [ "$next_wide" -gt "$wide_end" ] && [ "$next_narrow" -gt "$plan_count" ]; then
		break
	fi
	nap
done
run_wall=$(( $(date +%s) - run_start ))

# Remember each test's time for the next run's order: this run's times
# replace the old ones, tests this run did not reach keep theirs, and tests
# whose source is gone are dropped.  Only passing tests of an unfiltered
# full-tier run (RISE_TEST_EXTENDED=1) on several slots count: a failure can
# stop early, the fast tier runs cheaper modes, and with --jobs 1 or a filter
# a test has few or no neighbours.  An interrupted run writes nothing.
if [ -s "$TIMINGS_TSV" ] && [ "$TIER" = "all" ] && [ "${RISE_TEST_EXTENDED:-}" = "1" ] \
   && [ -z "$FILTER" ] && [ "$RUN_JOBS" -gt 1 ]; then
	all_tests="$LOG_DIR/.all_tests"
	: > "$all_tests"
	for test_src in "$SRC_DIR"/*.cpp; do
		[ -f "$test_src" ] || continue
		test_base=${test_src##*/}
		printf '%s\n' "${test_base%.cpp}" >> "$all_tests"
	done
	mkdir -p "$(dirname "$TIMINGS_HISTORY")" || true
	if awk -F"$TAB" -v OFS="$TAB" -v hist="$TIMINGS_HISTORY" -v fails="$RUN_FAIL_TSV" -v known="$all_tests" '
		BEGIN {
			while ((getline line < known) > 0) exists[line] = 1
			while ((getline line < fails) > 0) { split(line, f, "\t"); failed[f[1]] = 1 }
			while ((getline line < hist) > 0) {
				sub(/\r$/, "", line)
				split(line, f, "\t")
				if (f[1] != "" && f[2] ~ /^[0-9]+$/) { t[f[1]] = f[2]; if (!(f[1] in seen)) { seen[f[1]] = 1; order[++n] = f[1] } }
			}
		}
		$2 != "" && !($2 in failed) { if (!($2 in seen)) { seen[$2] = 1; order[++n] = $2 } t[$2] = $1 }
		END { for (i = 1; i <= n; i++) if (order[i] in exists) print order[i], t[order[i]] }
	' "$TIMINGS_TSV" > "$TIMINGS_HISTORY.tmp.$$" \
	   && mv "$TIMINGS_HISTORY.tmp.$$" "$TIMINGS_HISTORY"; then
		:
	else
		rm -f "$TIMINGS_HISTORY.tmp.$$"
		echo "warning: could not update $TIMINGS_HISTORY" >&2
	fi
fi

# The r136-r175 historical evidence replays that ran here were retired on
# 2026-11-14: each bound milestone evidence to source and grid sizes that later
# commits intentionally changed (see docs/FIRE_SMOKE_STATUS.md, "Retired test gates").

print_summary

if [ "$failed" -ne 0 ] || [ "$build_failed" -ne 0 ] || [ "$bulk_rc" -ne 0 ] \
   || [ "$skipped" -ne 0 ] || [ "$found" -ne "$total" ]; then
	exit 1
fi
echo "All $found tests passed"
