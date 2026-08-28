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
FIRE_PRODUCTION_CALIBRATION_DIR="$REPO_ROOT/rendered/fire_production_calibration/r112_dyadic_smooth_open"
FIRE_PRODUCTION_PROTOCOL_SHA="42185c882c52e8c94db4b58f40674c53341eabe1b75b6922fdd1c7f56415a4ed"
FIRE_PRODUCTION_TARGETS_SHA="d4947cb8eedbc57732190bf1833e68c3f83a356346c1662db321d7831bce958b"
FIRE_PRODUCTION_SUBDOMINANCE_PROTOCOL="$REPO_ROOT/rendered/fire_production_calibration/r137_subdominance_protocol/subdominance_protocol.v1"
FIRE_PRODUCTION_SUBDOMINANCE_PROTOCOL_SHA="833137b54fbd507fc3b6bdcc960a23b89be60ee835f7b1ca177f93cc57d63922"
# Logs go outside the repo so they survive cloud-sync providers (iCloud,
# Dropbox, OneDrive) that can tombstone hidden build dirs inside synced
# locations like ~/Documents. Override with RISE_TEST_LOG_DIR if needed.
LOG_DIR="${RISE_TEST_LOG_DIR:-${TMPDIR:-/tmp}/rise-tests-logs-managed}"
# Parallel build jobs for the library and bulk-test build phases.  Without
# this, a change to a widely-included header (IJob.h, RISE_API.h, ...) meant
# the library recompiled one file at a time AND all ~131 test binaries
# relinked one at a time — tens of minutes that looked like "the tests are
# slow" (the test RUNS are seconds).  Override with RISE_TEST_BUILD_JOBS.
JOBS="${RISE_TEST_BUILD_JOBS:-$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}"

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
rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"
: > "$LOG_DIR/.rise-test-log-directory"
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
# Phase 1.5: Bulk-build every test binary IN PARALLEL (the Makefile's
# `tests` target is prerequisite-driven, so -j fans the links out).
# -k keeps building past an individual failure; the per-test loop in
# Phase 2 then short-circuits on everything up to date and re-runs
# make only for the failures, capturing each one's error in its own
# log.  Net effect: the heavy lifting happens wide, the diagnostics
# stay per-test.
# ============================================================
bulk_log="$LOG_DIR/.tests-bulk.log"
printf 'Building tests (-j %s) ... ' "$JOBS"
bulk_start=$(date +%s)
bulk_rc=0
make -s -j "$JOBS" -k -C "$BUILD_DIR" --no-print-directory tests > "$bulk_log" 2>&1 || bulk_rc=$?
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
	i=$((i + 1))
	name="$(basename "$test_src" .cpp)"
	test_path="$BIN_DIR/$name"
	log="$LOG_DIR/$name.build.log"
	prefix="$(printf '[ %3d/%3d ] %-46s' "$i" "$total" "$name")"

	# Fast up-to-date short-circuit: skip the make call (which would otherwise
	# stat the entire $(OBJLIB) tree — ~3s per test) when the binary is
	# already newer than its source and every library .o file.
	if [ "$bulk_rc" -eq 0 ] \
	   && [ -x "$test_path" ] \
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
	i=$((i + 1))
	name="$(basename "$test_src" .cpp)"
	test_path="$BIN_DIR/$name"
	prefix="$(printf '[ %3d/%3d ] %-46s' "$i" "$total" "$name")"

	if [ -s "$BUILD_FAIL_TSV" ] && awk -F '\t' -v test="$name" \
		'$1 == test { found=1 } END { exit(found ? 0 : 1) }' "$BUILD_FAIL_TSV"
	then
		printf '%s SKIP (current build failed; stale executable ignored)\n' "$prefix"
		skipped=$((skipped + 1))
		continue
	fi

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

# The r136 full-step proof refusal is a capability-isolated evidence gate, not a test
# source with its own executable name. Run the capability-isolated binary
# explicitly and accept only the exact structural projection-certificate result.
roundoff_name="FireProductionCalibrationOracle.r136"
roundoff_path="$BIN_DIR/FireProductionCalibrationOracle"
roundoff_log="$LOG_DIR/$roundoff_name.log"
printf '[ evidence ] %-46s ... ' "$roundoff_name"
roundoff_rc=0
if [ ! -x "$roundoff_path" ]; then
	roundoff_rc=127
elif [ -n "$timeout_bin" ]; then
	"$timeout_bin" "$RISE_TEST_TIMEOUT" "$roundoff_path" \
		--fire-production-calibration-diagnose-roundoff \
		"$FIRE_PRODUCTION_CALIBRATION_DIR" "$FIRE_PRODUCTION_PROTOCOL_SHA" \
		"$FIRE_PRODUCTION_TARGETS_SHA" >"$roundoff_log" 2>&1 || roundoff_rc=$?
else
	"$roundoff_path" --fire-production-calibration-diagnose-roundoff \
		"$FIRE_PRODUCTION_CALIBRATION_DIR" "$FIRE_PRODUCTION_PROTOCOL_SHA" \
		"$FIRE_PRODUCTION_TARGETS_SHA" >"$roundoff_log" 2>&1 || roundoff_rc=$?
fi
if [ "$roundoff_rc" -eq 237 ]; then
	echo 'PASS (exact exit=237)'
	rm -f "$roundoff_log"
else
	echo "FAIL (exit=$roundoff_rc; expected 237)"
	printf '%s\t%d\t%s\n' "$roundoff_name" "$roundoff_rc" "$roundoff_log" >> "$RUN_FAIL_TSV"
	failed=$((failed + 1))
fi

# r147/r148 exercise the accepted resident observation through an actual v13
# checkpoint/reload before the second production timestep selection.  Exact
# 255 is success for this two-step lifecycle-only probe.
if [ "$(uname -s)" = "Darwin" ]; then
	lifecycle_name="FireSequenceTest.r148_manifold_lifecycle"
	lifecycle_path="$BIN_DIR/FireSequenceTest"
	lifecycle_log="$LOG_DIR/$lifecycle_name.log"
	printf '[ evidence ] %-46s ... ' "$lifecycle_name"
	lifecycle_rc=0
	if [ ! -x "$lifecycle_path" ]; then
		lifecycle_rc=127
	elif [ -n "$timeout_bin" ]; then
		RISE_FIRE_MANIFOLD_LIFECYCLE_PROBE=1 \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$lifecycle_path" \
			--fire-production-calibration-check-dyadic-production \
			"$FIRE_PRODUCTION_CALIBRATION_DIR" "$FIRE_PRODUCTION_PROTOCOL_SHA" \
			"$FIRE_PRODUCTION_TARGETS_SHA" >"$lifecycle_log" 2>&1 || lifecycle_rc=$?
	else
		RISE_FIRE_MANIFOLD_LIFECYCLE_PROBE=1 \
			"$lifecycle_path" --fire-production-calibration-check-dyadic-production \
			"$FIRE_PRODUCTION_CALIBRATION_DIR" "$FIRE_PRODUCTION_PROTOCOL_SHA" \
			"$FIRE_PRODUCTION_TARGETS_SHA" >"$lifecycle_log" 2>&1 || lifecycle_rc=$?
	fi
	if [ "$lifecycle_rc" -eq 255 ]; then
		echo 'PASS (exact exit=255)'
		rm -f "$lifecycle_log"
	else
		echo "FAIL (exit=$lifecycle_rc; expected 255)"
		printf '%s\t%d\t%s\n' "$lifecycle_name" "$lifecycle_rc" \
			"$lifecycle_log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
fi

# r138 is the retained tier-6 on-device precision pilot and therefore runs only
# on Metal.  Exact 243 binds all pilot channels; r140 separately records that
# the promised golden-slice class remains fail-closed.
if [ "$(uname -s)" = "Darwin" ]; then
	subdominance_name="FireSequenceTest.r138_subdominance"
	subdominance_path="$BIN_DIR/FireSequenceTest"
	subdominance_log="$LOG_DIR/$subdominance_name.log"
	printf '[ evidence ] %-46s ... ' "$subdominance_name"
	subdominance_rc=0
	if [ ! -x "$subdominance_path" ]; then
		subdominance_rc=127
	elif [ -n "$timeout_bin" ]; then
		"$timeout_bin" "$RISE_TEST_TIMEOUT" "$subdominance_path" \
			--fire-production-calibration-measure-subdominance \
			"$FIRE_PRODUCTION_CALIBRATION_DIR" "$FIRE_PRODUCTION_PROTOCOL_SHA" \
			"$FIRE_PRODUCTION_TARGETS_SHA" "$FIRE_PRODUCTION_SUBDOMINANCE_PROTOCOL" \
			"$FIRE_PRODUCTION_SUBDOMINANCE_PROTOCOL_SHA" >"$subdominance_log" 2>&1 ||
			subdominance_rc=$?
	else
		"$subdominance_path" --fire-production-calibration-measure-subdominance \
			"$FIRE_PRODUCTION_CALIBRATION_DIR" "$FIRE_PRODUCTION_PROTOCOL_SHA" \
			"$FIRE_PRODUCTION_TARGETS_SHA" "$FIRE_PRODUCTION_SUBDOMINANCE_PROTOCOL" \
			"$FIRE_PRODUCTION_SUBDOMINANCE_PROTOCOL_SHA" >"$subdominance_log" 2>&1 ||
			subdominance_rc=$?
	fi
	if [ "$subdominance_rc" -eq 243 ]; then
		echo 'PASS (exact exit=243)'
		rm -f "$subdominance_log"
	else
		echo "FAIL (exit=$subdominance_rc; expected 243)"
		printf '%s\t%d\t%s\n' "$subdominance_name" "$subdominance_rc" \
			"$subdominance_log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
fi

# r142 is the exact cold/burning plateau-capacity campaign.  It replaces the
# ordinary r140 replay after proving that no derived burning validation band or
# cycle count can meet the frozen 75%-ceiling function gate.
if [ "$(uname -s)" = "Darwin" ]; then
	golden_refusal_name="FireSequenceTest.r142_burning_plateau_capacity"
	golden_refusal_path="$BIN_DIR/FireSequenceTest"
	golden_refusal_log="$LOG_DIR/$golden_refusal_name.log"
	printf '[ evidence ] %-46s ... ' "$golden_refusal_name"
	golden_refusal_rc=0
	if [ ! -x "$golden_refusal_path" ]; then
		golden_refusal_rc=127
	elif [ -n "$timeout_bin" ]; then
		RISE_FIRE_RESTORATION_PLATEAU_PROBE=1 \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$golden_refusal_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$golden_refusal_log" 2>&1 || golden_refusal_rc=$?
	else
		RISE_FIRE_RESTORATION_PLATEAU_PROBE=1 \
			"$golden_refusal_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$golden_refusal_log" 2>&1 || golden_refusal_rc=$?
	fi
	if [ "$golden_refusal_rc" -eq 253 ]; then
		echo 'PASS (exact exit=253)'
		rm -f "$golden_refusal_log"
	else
		echo "FAIL (exit=$golden_refusal_rc; expected 253)"
		printf '%s\t%d\t%s\n' "$golden_refusal_name" "$golden_refusal_rc" \
			"$golden_refusal_log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
fi

# r144 replays the r143 previous-step predictor at the frozen burning slice.
# Exact 254 means the represented limited step reached Metal, independently
# reproduced G, and then failed both the derived mechanism and function gates
# without publishing an accepted result.
if [ "$(uname -s)" = "Darwin" ]; then
	manifold_stop_name="FireSequenceTest.r144_manifold_predictor_stop"
	manifold_stop_path="$BIN_DIR/FireSequenceTest"
	manifold_stop_log="$LOG_DIR/$manifold_stop_name.log"
	printf '[ evidence ] %-46s ... ' "$manifold_stop_name"
	manifold_stop_rc=0
	if [ ! -x "$manifold_stop_path" ]; then
		manifold_stop_rc=127
	elif [ -n "$timeout_bin" ]; then
		RISE_FIRE_MANIFOLD_TIMESTEP_PROBE=1 \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$manifold_stop_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$manifold_stop_log" 2>&1 || manifold_stop_rc=$?
	else
		RISE_FIRE_MANIFOLD_TIMESTEP_PROBE=1 \
			"$manifold_stop_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$manifold_stop_log" 2>&1 || manifold_stop_rc=$?
	fi
	if [ "$manifold_stop_rc" -eq 254 ]; then
		echo 'PASS (exact exit=254)'
		rm -f "$manifold_stop_log"
	else
		echo "FAIL (exit=$manifold_stop_rc; expected 254)"
		printf '%s\t%d\t%s\n' "$manifold_stop_name" "$manifold_stop_rc" \
			"$manifold_stop_log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
fi

# r150 is the frozen three-timestep burning-state stage budget. Exact 245
# means the device map/reduction, independent host audit, stage ownership,
# scaling exponents, retained ten-tuple falsification, and golden identity all
# matched before the ceiling stop. Malformed activation must exit 223 before
# Metal rather than aliasing to process success.
if [ "$(uname -s)" = "Darwin" ]; then
	stage_budget_name="FireSequenceTest.r150_manifold_stage_budget"
	stage_budget_path="$BIN_DIR/FireSequenceTest"
	stage_budget_log="$LOG_DIR/$stage_budget_name.log"
	stage_budget_malformed_log="$LOG_DIR/$stage_budget_name.malformed.log"
	printf '[ evidence ] %-46s ... ' "$stage_budget_name"
	stage_budget_rc=0
	stage_budget_malformed_rc=0
	if [ ! -x "$stage_budget_path" ]; then
		stage_budget_rc=127
		stage_budget_malformed_rc=127
	elif [ -n "$timeout_bin" ]; then
		RISE_FIRE_MANIFOLD_STAGE_BUDGET_PROBE=malformed \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$stage_budget_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$stage_budget_malformed_log" 2>&1 || stage_budget_malformed_rc=$?
		RISE_FIRE_MANIFOLD_STAGE_BUDGET_PROBE=1 \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$stage_budget_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$stage_budget_log" 2>&1 || stage_budget_rc=$?
	else
		RISE_FIRE_MANIFOLD_STAGE_BUDGET_PROBE=malformed \
			"$stage_budget_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$stage_budget_malformed_log" 2>&1 || stage_budget_malformed_rc=$?
		RISE_FIRE_MANIFOLD_STAGE_BUDGET_PROBE=1 \
			"$stage_budget_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$stage_budget_log" 2>&1 || stage_budget_rc=$?
	fi
	if [ "$stage_budget_malformed_rc" -eq 223 ] && [ "$stage_budget_rc" -eq 245 ]; then
		echo 'PASS (exact exit=245)'
		rm -f "$stage_budget_log" "$stage_budget_malformed_log"
	else
		echo "FAIL (malformed_exit=$stage_budget_malformed_rc expected 223; "\
"evidence_exit=$stage_budget_rc expected 245)"
		printf '%s\t%d\t%s\n' "$stage_budget_name" "$stage_budget_rc" \
			"$stage_budget_log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
fi

# r159 audits the velocity actually carried by the immutable burning state.
# Exact 247 binds the transport, physical-projection, and restoration-correction
# maxima, their owning faces, the physical CFL step, and the resident topology.
# Malformed activation must return 222 before Metal work.
if [ "$(uname -s)" = "Darwin" ]; then
	velocity_audit_name="FireSequenceTest.r159_timestep_velocity_audit"
	velocity_audit_path="$BIN_DIR/FireSequenceTest"
	velocity_audit_log="$LOG_DIR/$velocity_audit_name.log"
	velocity_audit_malformed_log="$LOG_DIR/$velocity_audit_name.malformed.log"
	velocity_audit_options="$REPO_ROOT/rendered/fire_production_calibration/r159_timestep_velocity_ceiling_stop/benchmark.options"
	printf '[ evidence ] %-46s ... ' "$velocity_audit_name"
	velocity_audit_rc=0
	velocity_audit_malformed_rc=0
	if [ ! -x "$velocity_audit_path" ]; then
		velocity_audit_rc=127
		velocity_audit_malformed_rc=127
	elif [ -n "$timeout_bin" ]; then
		RISE_FIRE_TIMESTEP_VELOCITY_AUDIT=malformed \
			RISE_OPTIONS_FILE="$velocity_audit_options" \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$velocity_audit_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$velocity_audit_malformed_log" 2>&1 || velocity_audit_malformed_rc=$?
		RISE_FIRE_TIMESTEP_VELOCITY_AUDIT=1 \
			RISE_OPTIONS_FILE="$velocity_audit_options" \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$velocity_audit_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$velocity_audit_log" 2>&1 || velocity_audit_rc=$?
	else
		RISE_FIRE_TIMESTEP_VELOCITY_AUDIT=malformed \
			RISE_OPTIONS_FILE="$velocity_audit_options" \
			"$velocity_audit_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$velocity_audit_malformed_log" 2>&1 || velocity_audit_malformed_rc=$?
		RISE_FIRE_TIMESTEP_VELOCITY_AUDIT=1 \
			RISE_OPTIONS_FILE="$velocity_audit_options" \
			"$velocity_audit_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$velocity_audit_log" 2>&1 || velocity_audit_rc=$?
	fi
	if [ "$velocity_audit_malformed_rc" -eq 222 ] && [ "$velocity_audit_rc" -eq 247 ]; then
		echo 'PASS (exact exit=247)'
		rm -f "$velocity_audit_log" "$velocity_audit_malformed_log"
	else
		echo "FAIL (malformed_exit=$velocity_audit_malformed_rc expected 222; "\
"evidence_exit=$velocity_audit_rc expected 247)"
		printf '%s\t%d\t%s\n' "$velocity_audit_name" "$velocity_audit_rc" \
			"$velocity_audit_log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
fi

# r160 corrects the inherited pressure-detector derivation.  The audited CFL
# step is allowed to execute only as a diagnostic and must fail closed before
# a long-shadow token is minted when its realized field exceeds 2^-5.
if [ "$(uname -s)" = "Darwin" ]; then
	long_shadow_name="FireSequenceTest.r160_golden_long_shadow_admission"
	long_shadow_path="$BIN_DIR/FireSequenceTest"
	long_shadow_log="$LOG_DIR/$long_shadow_name.log"
	long_shadow_malformed_log="$LOG_DIR/$long_shadow_name.malformed.log"
	long_shadow_options="$REPO_ROOT/rendered/fire_production_calibration/r159_timestep_velocity_ceiling_stop/benchmark.options"
	printf '[ evidence ] %-46s ... ' "$long_shadow_name"
	long_shadow_rc=0
	long_shadow_malformed_rc=0
	if [ ! -x "$long_shadow_path" ]; then
		long_shadow_rc=127
		long_shadow_malformed_rc=127
	elif [ -n "$timeout_bin" ]; then
		RISE_FIRE_GOLDEN_LONG_SHADOW=malformed RISE_OPTIONS_FILE="$long_shadow_options" \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$long_shadow_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$long_shadow_malformed_log" 2>&1 || long_shadow_malformed_rc=$?
		RISE_FIRE_GOLDEN_LONG_SHADOW=1 \
			RISE_FIRE_ADVECTIVE_ANOMALY_CLOSURE_TEST=disabled \
			RISE_OPTIONS_FILE="$long_shadow_options" \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$long_shadow_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$long_shadow_log" 2>&1 || long_shadow_rc=$?
	else
		RISE_FIRE_GOLDEN_LONG_SHADOW=malformed RISE_OPTIONS_FILE="$long_shadow_options" \
			"$long_shadow_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$long_shadow_malformed_log" 2>&1 || long_shadow_malformed_rc=$?
		RISE_FIRE_GOLDEN_LONG_SHADOW=1 \
			RISE_FIRE_ADVECTIVE_ANOMALY_CLOSURE_TEST=disabled \
			RISE_OPTIONS_FILE="$long_shadow_options" \
			"$long_shadow_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$long_shadow_log" 2>&1 || long_shadow_rc=$?
	fi
	if [ "$long_shadow_malformed_rc" -eq 249 ] && [ "$long_shadow_rc" -eq 252 ] &&
		grep -Fq 'dt=0.0016462659696117043 G=0.085895776748657227' "$long_shadow_log" &&
		grep -Fq 'field_max=0.085895776748657227 low_mach_ceiling=0.03125' "$long_shadow_log" &&
		grep -Fq 'accepted_token=0' "$long_shadow_log" &&
		grep -Fq 'golden=1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947' "$long_shadow_log"; then
		echo 'PASS (exact exit=252, low-Mach refusal)'
		rm -f "$long_shadow_log" "$long_shadow_malformed_log"
	else
		echo "FAIL (malformed_exit=$long_shadow_malformed_rc expected 249; "\
"evidence_exit=$long_shadow_rc expected 252)"
		printf '%s\t%d\t%s\n' "$long_shadow_name" "$long_shadow_rc" \
			"$long_shadow_log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
fi

# r166 retains r162's contraction and stale-CFL controls, then replays r164's
# drain-aware retry.  The plateau-passing retry mints accepted-state authority,
# while its motion-contaminated Eulerian G remains unavailable to the selector.
if [ "$(uname -s)" = "Darwin" ]; then
	closure_name="FireSequenceTest.r164_drain_aware_retry"
	closure_path="$BIN_DIR/FireSequenceTest"
	closure_cfl_log="$LOG_DIR/$closure_name.cfl.log"
	closure_contraction_log="$LOG_DIR/$closure_name.contraction.log"
	closure_limited_log="$LOG_DIR/$closure_name.limited.log"
	closure_controller_log="$LOG_DIR/$closure_name.controller.log"
	closure_options="$REPO_ROOT/rendered/fire_production_calibration/r159_timestep_velocity_ceiling_stop/benchmark.options"
	printf '[ evidence ] %-46s ... ' "$closure_name"
	closure_cfl_rc=0
	closure_contraction_rc=0
	closure_limited_rc=0
	closure_controller_rc=0
	if [ ! -x "$closure_path" ]; then
		closure_cfl_rc=127
		closure_contraction_rc=127
		closure_limited_rc=127
		closure_controller_rc=127
	elif [ -n "$timeout_bin" ]; then
		RISE_FIRE_GOLDEN_LONG_SHADOW=1 RISE_OPTIONS_FILE="$closure_options" \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$closure_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$closure_cfl_log" 2>&1 || closure_cfl_rc=$?
		RISE_FIRE_EQUAL_TIME_CONTRACTION_PROBE=1 \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$closure_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$closure_contraction_log" 2>&1 || closure_contraction_rc=$?
		RISE_FIRE_GOLDEN_LONG_SHADOW=1 \
			RISE_FIRE_ADVECTIVE_ANOMALY_CLOSURE_TEST=limited \
			RISE_OPTIONS_FILE="$closure_options" \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$closure_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$closure_limited_log" 2>&1 || closure_limited_rc=$?
		RISE_FIRE_DRAIN_AWARE_RETRY_CONTROLLER_RED=1 \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$closure_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$closure_controller_log" 2>&1 || closure_controller_rc=$?
	else
		RISE_FIRE_GOLDEN_LONG_SHADOW=1 RISE_OPTIONS_FILE="$closure_options" \
			"$closure_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$closure_cfl_log" 2>&1 || closure_cfl_rc=$?
		RISE_FIRE_EQUAL_TIME_CONTRACTION_PROBE=1 \
			"$closure_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$closure_contraction_log" 2>&1 || closure_contraction_rc=$?
		RISE_FIRE_GOLDEN_LONG_SHADOW=1 \
			RISE_FIRE_ADVECTIVE_ANOMALY_CLOSURE_TEST=limited \
			RISE_OPTIONS_FILE="$closure_options" \
			"$closure_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$closure_limited_log" 2>&1 || closure_limited_rc=$?
		RISE_FIRE_DRAIN_AWARE_RETRY_CONTROLLER_RED=1 \
			"$closure_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$closure_controller_log" 2>&1 || closure_controller_rc=$?
	fi
	if [ "$closure_cfl_rc" -eq 252 ] && [ "$closure_contraction_rc" -eq 217 ] &&
		[ "$closure_limited_rc" -eq 206 ] && [ "$closure_controller_rc" -eq 204 ] &&
		grep -Fq 'DRAIN_AWARE_RETRY_CONTROLLER_RED refused_candidate=1 next_candidate=2 cap=20 golden=1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947' "$closure_controller_log" &&
		grep -Fq 'dt=0.0016462659696117043 G=0.066569089889526367' "$closure_cfl_log" &&
		grep -Fq 'field_max=0.066569089889526367' "$closure_cfl_log" &&
		grep -Fq 'passes=2 cell_submaps=10 dual_submaps=15 source_commits=2 scalar_reads=2' "$closure_cfl_log" &&
		grep -Fq 'certified_bytes=1920120024 actual_bytes=1630855752' "$closure_cfl_log" &&
		grep -Fq 'tier10_device_hours=' "$closure_cfl_log" &&
		grep -Fq 'accepted_token=0 golden=1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947' "$closure_cfl_log" &&
		grep -Fq 'largest_converged_dt=7.244249718496576e-05 largest_converged_level=3 monotone=1 trace=900a7acc56a0c9c51d07788753132d59269bdb591aee699960a3c62b448a051c' "$closure_contraction_log" &&
		grep -Fq 'EQUAL_TIME_LIMITED_PRODUCTION candidate=0 dt=0.00055762444389984012 reference_substeps=8 reference_substep_dt=6.9703055487480015e-05' "$closure_limited_log" &&
		grep -Fq 'schedule=1c7944ddde6e673330ccf115c388b0a424027cfcb86e0b8bf8d503f22d7c531d terminal_target=522347125277cf97fe0c58fb4db86e906892ef81182f280b95a92ac35d68f833 predictor_G=0.019734203815460205 G=0.023458957672119141 field_max=0.023458957672119141' "$closure_limited_log" &&
		grep -Fq 'initial_audit_dt=0.00057953997747972608 initial_audit_G=0.024358630180358887 initial_selected_dt=0.0005576244690940563 initial_calibration=1' "$closure_limited_log" &&
		grep -Fq 'headroom_allowance=0.0234375 low_mach_ceiling=0.03125 headroom_met=0' "$closure_limited_log" &&
		grep -Fq 'next_dt_manifold=0.00055691943198942959 limiter_binding=1' "$closure_limited_log" &&
		grep -Fq 'accepted_token=0' "$closure_limited_log" &&
		grep -Fq 'DRAIN_AWARE_RETRY_GATE exact=1 retry_allowed=1 attempt_succeeded=0 ordinary_refused=1 diagnostics=1' "$closure_limited_log" &&
		grep -Fq 'DRAIN_AWARE_PLATEAU_RETRY refused_candidate=0 refused_dt=0.00055762444389984012 field_max=0.023458957672119141 allowance=0.0234375 suggested_dt=0.00055691943198942959 next_candidate=1 cap=20 ordinary_advance_refused=1 attempt_diagnostics=1' "$closure_limited_log" &&
		grep -Fq 'DRAIN_AWARE_PLATEAU_RETRY_CONTINUE refused_candidate=0 refused_dt=0.00055762444389984012 suggested_dt=0.00055691943198942959 next_candidate=1 cap=20' "$closure_limited_log" &&
		grep -Fq 'EQUAL_TIME_LIMITED_PRODUCTION candidate=1 dt=0.0005569194327108562' "$closure_limited_log" &&
		grep -Fq 'schedule=1640f2922f8030fe5fe6f983e3c3cae90eb1324bff0b927dde4439df52ed3e3c terminal_target=673d3fc35e9b56e3d04499f37c82aaee9330a8d62ae84f25f49deb7854a9c95a predictor_G=0.019709646701812744 G=0.023430228233337402 field_max=0.023430228233337402' "$closure_limited_log" &&
		grep -Fq 'G_material_authority=0' "$closure_limited_log" &&
		grep -Fq 'accepted_token=1 golden=1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947' "$closure_limited_log" &&
		grep -Fq 'golden=1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947' "$closure_limited_log"; then
		echo 'PASS (exact exit=206, accepted state with Eulerian G withheld)'
		rm -f "$closure_cfl_log" "$closure_contraction_log" "$closure_limited_log" "$closure_controller_log"
	else
		echo "FAIL (CFL_exit=$closure_cfl_rc expected 252; contraction_exit=$closure_contraction_rc expected 217; limited_exit=$closure_limited_rc expected 206)"
		printf '%s\t%d\t%s\n' "$closure_name" "$closure_limited_rc" \
			"$closure_limited_log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
fi

# r165 retains the derived physical-projection retry before any trajectory
# claim.  The separate three-step diagnostic exposed an unresolved Eulerian
# anomaly observable and is kept only as calibrating evidence, not replayed as
# an accepted-state or cost contract.
if [ "$(uname -s)" = "Darwin" ]; then
	projection_retry_name="FireSequenceTest.r165_physical_projection_retry"
	projection_retry_path="$BIN_DIR/FireSequenceTest"
	projection_retry_log="$LOG_DIR/$projection_retry_name.log"
	projection_retry_options="$REPO_ROOT/rendered/fire_production_calibration/r159_timestep_velocity_ceiling_stop/benchmark.options"
	printf '[ evidence ] %-46s ... ' "$projection_retry_name"
	projection_retry_rc=0
	if [ ! -x "$projection_retry_path" ]; then
		projection_retry_rc=127
	elif [ -n "$timeout_bin" ]; then
		RISE_FIRE_GOLDEN_LONG_SHADOW=1 RISE_FIRE_PHYSICAL_PROJECTION_RETRY_RED=1 \
			RISE_FIRE_ADVECTIVE_ANOMALY_CLOSURE_TEST=limited \
			RISE_OPTIONS_FILE="$projection_retry_options" \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$projection_retry_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$projection_retry_log" 2>&1 || projection_retry_rc=$?
	else
		RISE_FIRE_GOLDEN_LONG_SHADOW=1 RISE_FIRE_PHYSICAL_PROJECTION_RETRY_RED=1 \
			RISE_FIRE_ADVECTIVE_ANOMALY_CLOSURE_TEST=limited \
			RISE_OPTIONS_FILE="$projection_retry_options" \
			"$projection_retry_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$projection_retry_log" 2>&1 || projection_retry_rc=$?
	fi
	if [ "$projection_retry_rc" -eq 209 ] &&
		grep -Fq 'PHYSICAL_PROJECTION_RETRY step=0 refused_cycles=12' "$projection_retry_log" &&
		grep -Fq 'next_cycles=13 cap=64 accepted_token=0' "$projection_retry_log" &&
		grep -Fq 'PHYSICAL_PROJECTION_RETRY step=0 refused_cycles=13' "$projection_retry_log" &&
		grep -Fq 'next_cycles=14 cap=64 accepted_token=0' "$projection_retry_log" &&
		grep -Fq 'PHYSICAL_PROJECTION_RETRY_RED initial_cycles=12 retry_count=2 final_cycles=14 physical_valid=1 manifold_refused=1 accepted_token=0 golden=1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947' "$projection_retry_log"; then
		echo 'PASS (exact exit=209, physical-projection retry)'
		rm -f "$projection_retry_log"
	else
		echo "FAIL (exit=$projection_retry_rc expected 209)"
		printf '%s\t%d\t%s\n' "$projection_retry_name" "$projection_retry_rc" \
			"$projection_retry_log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
fi

# r166 exercises the motion-invariant distribution observables, then retains
# the exact slice-7 equal-time reference-schedule refusal.  Seven production
# steps are accepted; no 104-step plateau verdict is claimed.
if [ "$(uname -s)" = "Darwin" ]; then
	distribution_shadow_name="FireSequenceTest.r166_distribution_long_shadow"
	distribution_shadow_path="$BIN_DIR/FireSequenceTest"
	distribution_shadow_log="$LOG_DIR/$distribution_shadow_name.log"
	distribution_shadow_options="$REPO_ROOT/rendered/fire_production_calibration/r159_timestep_velocity_ceiling_stop/benchmark.options"
	printf '[ evidence ] %-46s ... ' "$distribution_shadow_name"
	distribution_shadow_rc=0
	if [ ! -x "$distribution_shadow_path" ]; then
		distribution_shadow_rc=127
	elif [ -n "$timeout_bin" ]; then
		RISE_FIRE_GOLDEN_LONG_SHADOW=1 RISE_FIRE_ACCEPTED_LONG_SHADOW=1 \
			RISE_FIRE_ADVECTIVE_ANOMALY_CLOSURE_TEST=limited \
			RISE_OPTIONS_FILE="$distribution_shadow_options" \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$distribution_shadow_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$distribution_shadow_log" 2>&1 || distribution_shadow_rc=$?
	else
		RISE_FIRE_GOLDEN_LONG_SHADOW=1 RISE_FIRE_ACCEPTED_LONG_SHADOW=1 \
			RISE_FIRE_ADVECTIVE_ANOMALY_CLOSURE_TEST=limited \
			RISE_OPTIONS_FILE="$distribution_shadow_options" \
			"$distribution_shadow_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$distribution_shadow_log" 2>&1 || distribution_shadow_rc=$?
	fi
	if [ "$distribution_shadow_rc" -eq 115 ] &&
		[ "$(grep -Fc 'GOLDEN_LONG_SHADOW_ACCEPT_CANDIDATE step=' "$distribution_shadow_log")" -eq 7 ] &&
		grep -Fq 'GOLDEN_LONG_SHADOW_ACCEPT_CANDIDATE step=0 dt=0.0005569194327108562 predictor_G=0.019709646701812744 G=0.023430228233337402 field_max=0.023430228233337402 field_p95=5.245208740234375e-06 field_p50=7.152557373046875e-07' "$distribution_shadow_log" &&
		grep -Fq 'GOLDEN_LONG_SHADOW_ACCEPT_CANDIDATE step=1 dt=0.0005569194327108562 predictor_G=0.02268320694565773 G=0.045641358941793442 field_max=0.022211194038391113 field_p95=0.00015151500701904297 field_p50=1.9073486328125e-06' "$distribution_shadow_log" &&
		grep -Fq 'GOLDEN_LONG_SHADOW_ACCEPT_CANDIDATE step=6 dt=3.7466904814209556e-06 predictor_G=0.013319537974894047 G=0.034910492599010468 field_max=0.020704150199890137 field_p95=2.6166439056396484e-05 field_p50=1.7881393432617188e-06' "$distribution_shadow_log" &&
		grep -Fq 'EQUAL_TIME_REFERENCE_RETRY slice=7 failed_substeps=8 next_substeps=16' "$distribution_shadow_log" &&
		grep -Fq 'EQUAL_TIME_REFERENCE_RETRY slice=7 failed_substeps=16 next_substeps=32' "$distribution_shadow_log" &&
		grep -Fq 'EQUAL_TIME_REFERENCE_RETRY slice=7 failed_substeps=32 next_substeps=64' "$distribution_shadow_log" &&
		grep -Fq 'production golden parallel reference 7 failed: equal-time reference substep 8: R1: fire solver open conservative Picard stage did not converge: first=6.94718 last=0.0191989 minimum=0.000270212 target=0.0191989 mass=0.0131144 coefficient=7.04504e-05 active_set=0 tolerance=0.000479545' "$distribution_shadow_log" &&
		! grep -Fq 'GOLDEN_LONG_SHADOW steps=104' "$distribution_shadow_log"; then
		echo 'PASS (exact exit=115, slice-7 equal-time reference schedule refusal)'
		rm -f "$distribution_shadow_log"
	else
		echo "FAIL (exit=$distribution_shadow_rc expected 115)"
		printf '%s\t%d\t%s\n' "$distribution_shadow_name" "$distribution_shadow_rc" \
			"$distribution_shadow_log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
fi

# r167 retains the deciding fixed-pass closure curve.  Exact 201 means both
# arithmetic classes remain above 1e-3 after pass 8; no production acceptance
# policy is installed from this diagnostic.
if [ "$(uname -s)" = "Darwin" ]; then
	closure_convergence_name="FireSequenceTest.r167_anomaly_closure_convergence"
	closure_convergence_path="$BIN_DIR/FireSequenceTest"
	closure_convergence_log="$LOG_DIR/$closure_convergence_name.log"
	closure_convergence_options="$REPO_ROOT/rendered/fire_production_calibration/r159_timestep_velocity_ceiling_stop/benchmark.options"
	printf '[ evidence ] %-46s ... ' "$closure_convergence_name"
	closure_convergence_rc=0
	if [ ! -x "$closure_convergence_path" ]; then
		closure_convergence_rc=127
	elif [ -n "$timeout_bin" ]; then
		RISE_FIRE_ADVECTIVE_ANOMALY_CONVERGENCE_PROBE=1 \
			RISE_OPTIONS_FILE="$closure_convergence_options" \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$closure_convergence_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$closure_convergence_log" 2>&1 || closure_convergence_rc=$?
	else
		RISE_FIRE_ADVECTIVE_ANOMALY_CONVERGENCE_PROBE=1 \
			RISE_OPTIONS_FILE="$closure_convergence_options" \
			"$closure_convergence_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$closure_convergence_log" 2>&1 || closure_convergence_rc=$?
	fi
	if [ "$closure_convergence_rc" -eq 201 ] &&
		[ "$(grep -Fc 'ANOMALY_CLOSURE_CONVERGENCE pass=' "$closure_convergence_log")" -eq 8 ] &&
		grep -Fq 'pass=1 tolerance=8.1249999999999996e-05 G32=0.085895776748657227 field32=0.085895776748657227 G64=0.085895672361020692 field64=0.085895672361034014' "$closure_convergence_log" &&
		grep -Fq 'pass=2 tolerance=8.1249999999999996e-05 G32=0.066569089889526367 field32=0.066569089889526367 G64=0.066569466014946732 field64=0.066569466023368884' "$closure_convergence_log" &&
		grep -Fq 'pass=8 tolerance=8.1249999999999996e-05 G32=0.16477346420288086 field32=0.16477346420288086 G64=0.16477412949642256 field64=0.16477412939898373' "$closure_convergence_log" &&
		grep -Fq 'cell_submaps=40 source_commits=8 scalar_reads=8 projections=8 token=0' "$closure_convergence_log" &&
		grep -Fq 'ANOMALY_CLOSURE_FEEDBACK_GAIN pairs=6 slope=0.79258751342062539 intercept=0.022579619687232797 pearson=0.7129565317645592 raw=4979ab6b85cbd72fb4a807f8f8d10d6a67991bbd8543cfa15f46767cb1c113ed' "$closure_convergence_log" &&
		grep -Fq 'ANOMALY_CLOSURE_CONVERGENCE_VERDICT tolerance=8.1249999999999996e-05 G32_pass8=0.16477346420288086 G64_pass8=0.16477412949642256 converged=0 stalled_above_1e-3=1 CFL_dt=0.0016462659696117043' "$closure_convergence_log"; then
		echo 'PASS (exact exit=201, fp32/fp64 closure architecture stop)'
		rm -f "$closure_convergence_log"
	else
		echo "FAIL (exit=$closure_convergence_rc expected 201)"
		printf '%s\t%d\t%s\n' "$closure_convergence_name" "$closure_convergence_rc" \
			"$closure_convergence_log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
fi

# r168/r169: the production default monitors the bulk distribution and engages
# the retained restoration projection only for a beginning tail beyond 2^-3.
# This golden one-step replay is the exact zero-tail/no-op boundary; the sealed
# r169 transcript binds the later conditional engagement and 2^-2 refusal.
if [ "$(uname -s)" = "Darwin" ]; then
	monitored_shadow_name="FireSequenceTest.r168_monitored_manifold_smoke"
	monitored_shadow_path="$BIN_DIR/FireSequenceTest"
	monitored_shadow_log="$LOG_DIR/$monitored_shadow_name.log"
	monitored_shadow_options="$REPO_ROOT/rendered/fire_production_calibration/r159_timestep_velocity_ceiling_stop/benchmark.options"
	printf '[ evidence ] %-46s ... ' "$monitored_shadow_name"
	monitored_shadow_rc=0
	if [ ! -x "$monitored_shadow_path" ]; then
		monitored_shadow_rc=127
	elif [ -n "$timeout_bin" ]; then
		RISE_FIRE_GOLDEN_LONG_SHADOW=1 \
			RISE_FIRE_MONITORED_MANIFOLD_SHADOW=1 \
			RISE_FIRE_MONITORED_MANIFOLD_SHADOW_SMOKE=1 \
			RISE_OPTIONS_FILE="$monitored_shadow_options" \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$monitored_shadow_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$monitored_shadow_log" 2>&1 || monitored_shadow_rc=$?
	else
		RISE_FIRE_GOLDEN_LONG_SHADOW=1 \
			RISE_FIRE_MONITORED_MANIFOLD_SHADOW=1 \
			RISE_FIRE_MONITORED_MANIFOLD_SHADOW_SMOKE=1 \
			RISE_OPTIONS_FILE="$monitored_shadow_options" \
			"$monitored_shadow_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$monitored_shadow_log" 2>&1 || monitored_shadow_rc=$?
	fi
	if [ "$monitored_shadow_rc" -eq 195 ] &&
		grep -Fq 'MONITORED_TARGET_POLICY absolute_reference_pressure_gate=0 producer_precision=2 strict_pressure_detector_refused=1 affine_RED_refused=1' "$monitored_shadow_log" &&
		grep -Fq 'dt=0.0016462659696117043 G_eulerian=0.085895776748657227 field_max=0.085895776748657227 field_p95=0.00071418285369873047 field_p50=1.1920928955078125e-07 allowance_crossed=1 ceiling_crossed=1 projection_valid=1 restoration_passes=0 tail_cells=0 tail_excess=0 tail_drain_m3=0 dynamics_bound=1 scalar_reads=1' "$monitored_shadow_log" &&
		grep -Fq 'MONITORED_MANIFOLD_SHADOW_SMOKE dt=0.0016462659696117043 field_max=0.085895776748657227 field_p95=0.00071418285369873047 field_p50=1.1920928955078125e-07 allowance_crossed=1 ceiling_crossed=1 projection_valid=1 restoration_passes=0 accepted=1' "$monitored_shadow_log" &&
		grep -Fq 'golden=1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947' "$monitored_shadow_log"; then
		echo 'PASS (exact exit=195, zero-tail monitored step accepted)'
		rm -f "$monitored_shadow_log"
	else
		echo "FAIL (exit=$monitored_shadow_rc expected 195)"
		printf '%s\t%d\t%s\n' "$monitored_shadow_name" "$monitored_shadow_rc" \
			"$monitored_shadow_log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
fi

# r170: the deeper tail margin completes the 104-step shadow.  Two separately
# serialized replays bind the retired 2^-3 stop and the ordinary hard-bound
# refusal -> reduced-dt -> accepted transition on that same sealed state.
if [ "$(uname -s)" = "Darwin" ]; then
	run_r170_evidence() {
		if [ -n "$timeout_bin" ]; then
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$@"
		else
			"$@"
		fi
	}
	for r170_case in shadow retired retry; do
		r170_name="FireSequenceTest.r170_${r170_case}"
		r170_path="$BIN_DIR/FireSequenceTest"
		r170_log="$LOG_DIR/$r170_name.log"
		r170_options="$REPO_ROOT/rendered/fire_production_calibration/r159_timestep_velocity_ceiling_stop/benchmark.options"
		printf '[ evidence ] %-46s ... ' "$r170_name"
		r170_rc=0
		if [ ! -x "$r170_path" ]; then
			r170_rc=127
		else
			case "$r170_case" in
				shadow)
					r170_expected=196
					RISE_FIRE_GOLDEN_LONG_SHADOW=1 \
						RISE_FIRE_MONITORED_MANIFOLD_SHADOW=1 \
						RISE_OPTIONS_FILE="$r170_options" \
						run_r170_evidence "$r170_path" --fire-production-golden-composition \
						"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
						"$REPO_ROOT" >"$r170_log" 2>&1 || r170_rc=$?
					;;
				retired)
					r170_expected=193
					RISE_FIRE_GOLDEN_LONG_SHADOW=1 \
						RISE_FIRE_MONITORED_MANIFOLD_SHADOW=1 \
						RISE_FIRE_MANIFOLD_TAIL_THRESHOLD_RED=1 \
						RISE_OPTIONS_FILE="$r170_options" \
						run_r170_evidence "$r170_path" --fire-production-golden-composition \
						"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
						"$REPO_ROOT" >"$r170_log" 2>&1 || r170_rc=$?
					;;
				retry)
					r170_expected=192
					RISE_FIRE_GOLDEN_LONG_SHADOW=1 \
						RISE_FIRE_MONITORED_MANIFOLD_SHADOW=1 \
						RISE_FIRE_MANIFOLD_TAIL_THRESHOLD_RED=1 \
						RISE_FIRE_MANIFOLD_HARD_BOUND_RETRY_RED=1 \
						RISE_OPTIONS_FILE="$r170_options" \
						run_r170_evidence "$r170_path" --fire-production-golden-composition \
						"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
						"$REPO_ROOT" >"$r170_log" 2>&1 || r170_rc=$?
					;;
			esac
		fi
		r170_bound=0
		if [ "$r170_case" = shadow ]; then
			grep -Fq 'MONITORED_MANIFOLD_SHADOW_STEP step=33 dt=0.0011745213996618986' "$r170_log" &&
			grep -Fq 'field_max=0.14402782917022705' "$r170_log" &&
			grep -Fq 'MONITORED_MANIFOLD_SHADOW_COMPLETE steps=104' "$r170_log" &&
			grep -Fq 'max_peak=0.15430498123168945' "$r170_log" &&
			grep -Fq 'restoration_passes=100 tail_population_peak=9698' "$r170_log" &&
			grep -Fq 'hard_bound_retries=0 step33_refusals=0' "$r170_log" &&
			grep -Fq 'trace=e3273037f56068efb2c067b8b70ec9524cfbd4edcf742c4ac51084b8bde507fa' "$r170_log" && r170_bound=1
		elif [ "$r170_case" = retired ]; then
			grep -Fq 'OUTLIER_BOUNDED_MANIFOLD_REFUSAL step=33 candidate=0 dt=0.0011971283238381147 field_max=0.25728172063827515' "$r170_log" &&
			grep -Fq 'suggested_dt=0.0011418721405789256 next_candidate=1 cap=20 ordinary_atomic=1' "$r170_log" && r170_bound=1
		else
			grep -Fq 'OUTLIER_BOUNDED_MANIFOLD_REFUSAL step=33 candidate=0 dt=0.0011971283238381147 field_max=0.25728172063827515' "$r170_log" &&
			grep -Fq 'OUTLIER_BOUNDED_HARD_RETRY_ACCEPTED step=33 candidate=1 dt=0.0011418721405789256 field_max=0.24757766723632812 physical_valid=1 restoration_valid=1 accepted_token=1' "$r170_log" && r170_bound=1
		fi
		if [ "$r170_rc" -eq "$r170_expected" ] && [ "$r170_bound" -eq 1 ] &&
			grep -Fq 'golden=1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947' "$r170_log"; then
			echo "PASS (exact exit=$r170_expected, r170 $r170_case)"
			rm -f "$r170_log"
		else
			echo "FAIL (exit=$r170_rc expected $r170_expected)"
			printf '%s\t%d\t%s\n' "$r170_name" "$r170_rc" "$r170_log" >> "$RUN_FAIL_TSV"
			failed=$((failed + 1))
		fi
	done
fi

# r171: deterministically regenerate and SHA-gate the current-build golden
# continuation, then execute all 152 same-scheme fp32/fp64 subdominance gates.
# The historical r138a hashes remain historical and are never overwritten.
if [ "$(uname -s)" = "Darwin" ]; then
	r171_path="$BIN_DIR/FireSequenceTest"
	r171_options="$REPO_ROOT/rendered/fire_production_calibration/r159_timestep_velocity_ceiling_stop/benchmark.options"
	r171_protocol="$REPO_ROOT/rendered/fire_production_calibration/r171_golden_subdominance_protocol/golden_subdominance_protocol.v1"
	r171_temp="$(mktemp -d "${TMPDIR:-/tmp}/rise-r171-golden.XXXXXX")"
	case "$(basename -- "$r171_temp")" in rise-r171-golden.*) ;; *)
		echo "Refusing unsafe r171 temporary directory: $r171_temp" >&2; exit 1;; esac
	r171_trace="$r171_temp/golden.trace"
	r171_frame="$r171_temp/golden.vdb"
	r171_generate_log="$LOG_DIR/FireSequenceTest.r171_generate.log"
	r171_measure_log="$LOG_DIR/FireSequenceTest.r171_subdominance.log"
	printf '[ evidence ] %-46s ... ' 'FireSequenceTest.r171_golden_subdominance'
	r171_generate_rc=0
	r171_measure_rc=0
	if [ ! -x "$r171_path" ]; then
		r171_generate_rc=127
	elif [ -n "$timeout_bin" ]; then
		"$timeout_bin" "$RISE_TEST_TIMEOUT" "$r171_path" \
			--fire-r171-golden-beginnings \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$r171_trace" "$r171_frame" >"$r171_generate_log" 2>&1 ||
			r171_generate_rc=$?
	else
		"$r171_path" --fire-r171-golden-beginnings \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$r171_trace" "$r171_frame" >"$r171_generate_log" 2>&1 ||
			r171_generate_rc=$?
	fi
	r171_inputs_bound=1
	if [ "$r171_generate_rc" -eq 189 ]; then
		if ! grep -Fq 'r171 golden beginning filter-scale mutation refused original=935af89dc8f7d000427dd239a877cdc5ed64191e75214f8273d86f979c4da449 mutated=2f2a43c95c39560756cd27f4232979f109280a969f1574c195862472875b34fe' \
			"$r171_generate_log"; then
			r171_inputs_bound=0
		fi
		for r171_slice in 0 1 2 3 4 5 6 7; do
			r171_expected="$(awk -v key="beginning_${r171_slice}_state_sha256" \
				'$1==key {print $2}' "$r171_protocol")"
			if [ -z "$r171_expected" ] || ! grep -Fq \
				"r171 golden beginning step=${r171_slice} state_sha256=${r171_expected}" \
				"$r171_generate_log"; then
				r171_inputs_bound=0
			fi
		done
	fi
	if [ "$r171_generate_rc" -eq 189 ] && [ "$r171_inputs_bound" -eq 1 ]; then
		if [ -n "$timeout_bin" ]; then
			RISE_FIRE_GOLDEN_SUBDOMINANCE=1 \
				RISE_FIRE_GOLDEN_SUBDOMINANCE_PROTOCOL="$r171_protocol" \
				RISE_OPTIONS_FILE="$r171_options" \
				"$timeout_bin" "$RISE_TEST_TIMEOUT" "$r171_path" \
				--fire-production-golden-composition \
				"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
				"$r171_trace.snapshots" >"$r171_measure_log" 2>&1 || r171_measure_rc=$?
		else
			RISE_FIRE_GOLDEN_SUBDOMINANCE=1 \
				RISE_FIRE_GOLDEN_SUBDOMINANCE_PROTOCOL="$r171_protocol" \
				RISE_OPTIONS_FILE="$r171_options" \
				"$r171_path" --fire-production-golden-composition \
				"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
				"$r171_trace.snapshots" >"$r171_measure_log" 2>&1 || r171_measure_rc=$?
		fi
	else
		r171_measure_rc=127
	fi
	if [ "$r171_generate_rc" -eq 189 ] && [ "$r171_inputs_bound" -eq 1 ] &&
		[ "$r171_measure_rc" -eq 190 ] &&
		grep -Fq 'r171 golden beginning generation complete slices=8 root=1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947' "$r171_generate_log" &&
		[ "$(grep -c '^golden subdominance slice=' "$r171_measure_log")" -eq 8 ] &&
		grep -Fq 'golden subdominance complete trace=1e48343aa5589cded65f2345e74d2ba103508bdf07fb9ab56e5ea7351cd00b61 slices=8 gates=152 physical_retries=0 velocity_max=3.2429213131399156e-09 velocity_bound=0.00073630739506866123 velocity_margin=227050.65093168768 scalar_min_margin=7972.9650118056461 inventory_min_margin=8894.785377013457' "$r171_measure_log"; then
		echo 'PASS (exact exits=189/190, 152 gates)'
		rm -f "$r171_generate_log" "$r171_measure_log"
	else
		echo "FAIL (generator=$r171_generate_rc expected 189; measurement=$r171_measure_rc expected 190)"
		printf '%s\t%d\t%s\n' 'FireSequenceTest.r171_generate' "$r171_generate_rc" \
			"$r171_generate_log" >> "$RUN_FAIL_TSV"
		printf '%s\t%d\t%s\n' 'FireSequenceTest.r171_subdominance' "$r171_measure_rc" \
			"$r171_measure_log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
	rm -rf "$r171_temp"
fi

# r172: execute r139's three-level temporal protocol after r171 and before
# equal-time readmission.  All rows are emitted even after the first refusal.
if [ "$(uname -s)" = "Darwin" ]; then
	r172_name="FireSequenceTest.r172_temporal_refinement_stop"
	r172_path="$BIN_DIR/FireSequenceTest"
	r172_oracle_path="$BIN_DIR/FireProductionCalibrationOracle"
	r172_log="$LOG_DIR/$r172_name.log"
	r172_seal_log="$LOG_DIR/$r172_name.seal.log"
	r172_options="$REPO_ROOT/rendered/fire_production_calibration/r159_timestep_velocity_ceiling_stop/benchmark.options"
	r172_directory="$REPO_ROOT/rendered/fire_production_calibration/r172_temporal_refinement_stop"
	r172_protocol="$REPO_ROOT/rendered/fire_production_calibration/r139_temporal_protocol/temporal_protocol.v1"
	r172_protocol_sha="e58ee48de0c79dc35aa6e6bf344729c12cc74bf7d89cfa3e78cfeaf28cc9630c"
	r172_targets_sha="6e0af5dcb7602b6fd067bc6d4b113378c14444643300ede6375d442c7cdef42c"
	r172_temp="$(mktemp -d "${TMPDIR:-/tmp}/rise-r172-targets.XXXXXX")"
	printf '[ evidence ] %-46s ... ' "$r172_name"
	r172_rc=0 r172_seal_rc=0 r172_sealed=0
	if [ ! -x "$r172_path" ] || [ ! -x "$r172_oracle_path" ]; then
		r172_rc=127
	elif "$r172_oracle_path" --fire-production-calibration-seal-temporal-targets \
		"$r172_temp" "$r172_protocol" "$r172_protocol_sha" >"$r172_seal_log" 2>&1; then
		r172_seal_rc=0
	else
		r172_seal_rc=$?
	fi
	if [ "$r172_seal_rc" -eq 194 ] &&
		grep -Fq "temporal targets sealed manifest_sha256=$r172_targets_sha" "$r172_seal_log" &&
		cmp -s "$r172_temp/temporal_targets.v1" "$r172_directory/temporal_targets.v1" &&
		cmp -s "$r172_temp/temporal_level0_sdiv.f64" "$r172_directory/temporal_level0_sdiv.f64" &&
		cmp -s "$r172_temp/temporal_level1_sdiv.f64" "$r172_directory/temporal_level1_sdiv.f64" &&
		cmp -s "$r172_temp/temporal_level2_sdiv.f64" "$r172_directory/temporal_level2_sdiv.f64"; then
		r172_sealed=1
	fi
	if [ "$r172_rc" -ne 127 ] && [ "$r172_sealed" -ne 1 ]; then
		r172_rc="$r172_seal_rc"
	elif [ -n "$timeout_bin" ]; then
		RISE_OPTIONS_FILE="$r172_options" "$timeout_bin" "$RISE_TEST_TIMEOUT" \
			"$r172_path" --fire-production-calibration-measure-temporal "$r172_directory" \
			"$r172_protocol" "$r172_protocol_sha" "$r172_targets_sha" \
			>"$r172_log" 2>&1 || r172_rc=$?
	else
		RISE_OPTIONS_FILE="$r172_options" "$r172_path" \
			--fire-production-calibration-measure-temporal "$r172_directory" "$r172_protocol" \
			"$r172_protocol_sha" "$r172_targets_sha" >"$r172_log" 2>&1 || r172_rc=$?
	fi
	if [ "$r172_rc" -eq 193 ] &&
		grep -Fq 'temporal scalar component=8 production_D=1.3664113219736267/0.68316914382060645 order=1 E=2.7328226439472538 accepted=1 oracle_D=0.0012312438866646748/0.001273209006325096 order=0 E=0 accepted=0' "$r172_log" &&
		grep -Fq 'temporal velocity production_D=6.7800078709060773e-06/3.735511745788258e-06 order=0.85998104987383395 E=1.5098888236479335e-05 accepted=1 oracle_D=6.2907169766867145e-06/5.3409610560218166e-06 order=0.23612509109838353 E=4.1666621096787029e-05 accepted=1' "$r172_log" &&
		grep -Fq 'temporal refinement complete baseline=0.0018513043178245425 horizon=0.01481043454259634 target_sha256=1cf6244040426b2704f8ac2c4b953c32efd1d0c71cdebea7eacef85f2217d05c/1f6a95c059bf63224b63697689e3498a05add9418f9470b1e4c3f8d6e9e30cf1/95e0f5032efb2171bc412d4e26411e7dedd91962d171b187a752555862112551 refusals=0/1 expected=1' "$r172_log" &&
		[ "$(grep -c '^temporal scalar component=' "$r172_log")" -eq 9 ] &&
		[ "$(grep -c '^temporal ledger component=' "$r172_log")" -eq 9 ] &&
		[ "$(grep -o 'accepted=0' "$r172_log" | wc -l | tr -d ' ')" -eq 1 ]; then
		echo 'PASS (exact exit=193, oracle energy temporal refusal)'
		rm -f "$r172_log" "$r172_seal_log"
	else
		echo "FAIL (exit=$r172_rc expected 193)"
		printf '%s\t%d\t%s\n' "$r172_name" "$r172_rc" "$r172_log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
	rm -rf "$r172_temp"
fi

print_summary

if [ "$failed" -ne 0 ] || [ "$build_failed" -ne 0 ] \
   || [ "$skipped" -ne 0 ] || [ "$found" -ne "$total" ]; then
	exit 1
fi
echo "All $found tests passed"
