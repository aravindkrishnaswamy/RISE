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

# r161 executes the pre-registered anomaly closure, then applies the amended
# 25%-headroom manifold predictor.  Exact 219 is the fail-closed target-schedule
# stop: the binary64 oracle cannot derive the limiter step's dt-dependent Heun
# target within its frozen Picard topology, so production is never invoked.
if [ "$(uname -s)" = "Darwin" ]; then
	closure_name="FireSequenceTest.r161_advective_anomaly_closure"
	closure_path="$BIN_DIR/FireSequenceTest"
	closure_cfl_log="$LOG_DIR/$closure_name.cfl.log"
	closure_limited_log="$LOG_DIR/$closure_name.limited.log"
	closure_options="$REPO_ROOT/rendered/fire_production_calibration/r159_timestep_velocity_ceiling_stop/benchmark.options"
	printf '[ evidence ] %-46s ... ' "$closure_name"
	closure_cfl_rc=0
	closure_limited_rc=0
	if [ ! -x "$closure_path" ]; then
		closure_cfl_rc=127
		closure_limited_rc=127
	elif [ -n "$timeout_bin" ]; then
		RISE_FIRE_GOLDEN_LONG_SHADOW=1 RISE_OPTIONS_FILE="$closure_options" \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$closure_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$closure_cfl_log" 2>&1 || closure_cfl_rc=$?
		RISE_FIRE_GOLDEN_LONG_SHADOW=1 \
			RISE_FIRE_ADVECTIVE_ANOMALY_CLOSURE_TEST=limited \
			RISE_OPTIONS_FILE="$closure_options" \
			"$timeout_bin" "$RISE_TEST_TIMEOUT" "$closure_path" \
			--fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$closure_limited_log" 2>&1 || closure_limited_rc=$?
	else
		RISE_FIRE_GOLDEN_LONG_SHADOW=1 RISE_OPTIONS_FILE="$closure_options" \
			"$closure_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$closure_cfl_log" 2>&1 || closure_cfl_rc=$?
		RISE_FIRE_GOLDEN_LONG_SHADOW=1 \
			RISE_FIRE_ADVECTIVE_ANOMALY_CLOSURE_TEST=limited \
			RISE_OPTIONS_FILE="$closure_options" \
			"$closure_path" --fire-production-golden-composition \
			"$REPO_ROOT/rendered/fire_methane_capstone/tier10.run.checkpoint" \
			"$REPO_ROOT" >"$closure_limited_log" 2>&1 || closure_limited_rc=$?
	fi
	if [ "$closure_cfl_rc" -eq 252 ] && [ "$closure_limited_rc" -eq 219 ] &&
		grep -Fq 'dt=0.0016462659696117043 G=0.066569089889526367' "$closure_cfl_log" &&
		grep -Fq 'field_max=0.066569089889526367' "$closure_cfl_log" &&
		grep -Fq 'passes=2 cell_submaps=10 dual_submaps=15 source_commits=2 scalar_reads=2' "$closure_cfl_log" &&
		grep -Fq 'certified_bytes=1919317208 actual_bytes=1630052936 accepted_token=0' "$closure_cfl_log" &&
		grep -Fq 'ADVECTIVE_ANOMALY_LIMITER_TARGET_STOP dt=0.00057953997747972608 binary64_target_schedule=unavailable' "$closure_limited_log" &&
		grep -Fq 'first=7.41824 last=1.44776 minimum=1.44776 target=0.561256 mass=1.44776 coefficient=0.017278 active_set=1 tolerance=0.000479545' "$closure_limited_log" &&
		grep -Fq 'golden=1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947' "$closure_limited_log"; then
		echo 'PASS (exact exit=219, target-schedule stop)'
		rm -f "$closure_cfl_log" "$closure_limited_log"
	else
		echo "FAIL (CFL_exit=$closure_cfl_rc expected 252; limited_exit=$closure_limited_rc expected 219)"
		printf '%s\t%d\t%s\n' "$closure_name" "$closure_limited_rc" \
			"$closure_limited_log" >> "$RUN_FAIL_TSV"
		failed=$((failed + 1))
	fi
fi

print_summary

if [ "$failed" -ne 0 ] || [ "$build_failed" -ne 0 ] \
   || [ "$skipped" -ne 0 ] || [ "$found" -ne "$total" ]; then
	exit 1
fi
echo "All $found tests passed"
