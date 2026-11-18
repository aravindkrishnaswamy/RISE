#!/usr/bin/env python3
"""Admit executed target-basis costs only after the full owner/byte gates."""
import argparse
import collections
import csv
import hashlib
import io
import json
import math
from pathlib import Path

from analyze_fire_producer_kernels import fields, summarize
from check_fire_owner_instrumentation import trees
from report_fire_r212_cost import stats
from report_fire_r213_cost import PHYSICS, table
from report_fire_r213_snapshots import admit_native
from run_fire_r215_target_profiles import ROOT, OUT, EXE_SHA, SOURCE_COMMIT, CHECKPOINT_SHA
from seal_fire_payload_placement import gate, distinct_repeats

BASELINE_SHA = "2b2a597b46759b6e2ea2ef8722996c6463ba8ca4e1053c97d72ff4863c14d600"
KERNEL_SET = "f68ef76b6cb83d2752d5bb38a2228f494f14bb0a25d59559c464b1c204faec02"
BASELINE_BYTES_SHA = "f60563f0a9e9313ecb9f8a78ecc1ea9d915f33c43fc5881a79f0a99e28257931"
CANDIDATE_BUILD = "4aa5da6758f20c63362ee0f201507127a021be2faac22397d294fbc9ce0d799e"
COMPARISON_SHA = "14a2dbe33ba3d9e80778f20ad16b4bda7150b86e44637251d42f1e7bc9febc2b"
EXECUTION_RECEIPT_SHA = "a87d286f28a4ea8c4cc59985e91694517715e53dfa8cd50898f0bd3701b0033d"


def validate_comparison_lineage(comparison, baseline):
    if set(comparison) != set(range(1, 9)) or set(baseline) != set(map(str, range(1, 9))):
        raise ValueError("eight baseline-bound comparisons required")
    for index, row in comparison.items():
        prior = baseline[str(index)]
        for key in ("build", "checkpoint_sha256", "comparison_sha256"):
            if row["old_"+key] != prior["new_"+key]:
                raise ValueError("wrong baseline "+key)
        if (row["new_build"] != CANDIDATE_BUILD or
                row["new_comparison_sha256"] != prior["new_comparison_sha256"] or
                row["time_s"] != prior["time_s"] or row["bytes"] != prior["bytes"]):
            raise ValueError("candidate byte proof not bound to matched execution")


def report():
    inputs = {}

    def read(path, expected=None):
        raw = path.read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        if expected is not None and digest != expected:
            raise ValueError("evidence SHA mismatch: "+str(path))
        inputs[str(path.relative_to(ROOT))] = digest
        return raw

    receipt = json.loads(read(OUT / "hot_profiles.execution.v1.json", EXECUTION_RECEIPT_SHA))
    if (receipt["schema"] != "rise.fire.r215.executed-hot-profiles.v1" or
            receipt["source_commit"] != SOURCE_COMMIT or
            receipt["executable_sha256"] != EXE_SHA or
            receipt["checkpoint_sha256"] != CHECKPOINT_SHA or
            receipt["migration_authority"] is not False or len(receipt["runs"]) != 3):
        raise ValueError("execution scope mismatch")
    qualified = json.loads(read(OUT / "qualification.contended.v1.json",
                                receipt["qualification_sha256"]))
    if qualified["source_commit"] != SOURCE_COMMIT or qualified["producer_executable_sha256"] != EXE_SHA:
        raise ValueError("executable not qualified")
    for name in ("build", "publication", "owner"):
        run = qualified["runs"][name]
        if run["exit_code"] != 0:
            raise ValueError("qualification failed")
        log = ROOT / run["log"]
        read(log, run["log_sha256"])
        if name == "owner":
            gate(log)
    comparison, headers = admit_native(read(OUT / "hot_byte_comparison.v2", COMPARISON_SHA).decode())
    if headers["reporter_executable_sha256"] != EXE_SHA or headers["reporter_build_id"] != CANDIDATE_BUILD:
        raise ValueError("byte comparison reporter differs")
    baseline_bytes = json.loads(read(ROOT / "rendered/fire_production_calibration/"
                                    "r213_composition/hot_numerical_qualification.v2.json", BASELINE_BYTES_SHA))
    validate_comparison_lineage(comparison, baseline_bytes["per_step"])
    reference = list(csv.DictReader(io.StringIO(read(
        OUT / "hot_snapshots.v1/budgets/maximum_velocity_trajectory.csv").decode())))
    if len(reference) != 8:
        raise ValueError("incomplete snapshot trajectory")
    for index, row in enumerate(reference, 1):
        native = comparison[index]
        if row["time_s"] != native["time_s"] or row["accepted_step"] != str(1300+index):
            raise ValueError("snapshot time disagrees with byte proof")
        read(OUT / f"hot_snapshots.v1/checkpoints/step_{index:02d}.checkpoint",
             native["new_checkpoint_sha256"])
    baseline = json.loads(read(ROOT / "rendered/fire_production_calibration/"
                              "r213_composition/cost_observation.v1.json", BASELINE_SHA))
    collected, repeats = [], []
    kernels = collections.defaultdict(lambda: [0, 0.0])
    phases = collections.defaultdict(lambda: [0.0, 0.0])
    previous_end = 0
    distinct_repeats(OUT, [f"hot_profile_{index}.v1" for index in range(2, 5)])
    for index, run in enumerate(receipt["runs"], 2):
        directory = OUT / f"hot_profile_{index}.v1"
        expected_log = str(Path(str(directory)+".log").relative_to(ROOT))
        expected_csv = str((directory / "budgets/maximum_velocity_trajectory.csv").relative_to(ROOT))
        if (run["log"] != expected_log or run["trajectory"] != expected_csv or
                run["process_exit_code"] != 93 or run["solver_accepted"] is not True or
                run["executable_before_after_sha256"] != EXE_SHA or
                run["checkpoint_before_after_sha256"] != CHECKPOINT_SHA or
                not previous_end < run["start_unix_ns"] < run["end_unix_ns"]):
            raise ValueError("independent sequential process evidence differs")
        previous_end = run["end_unix_ns"]
        log_path = ROOT / run["log"]
        log = read(log_path, run["log_sha256"]).decode()
        starts = [fields(line) for line in log.splitlines() if line.startswith("OWNER_EOS_DIAGNOSTIC ")]
        ends = [line for line in log.splitlines() if line.startswith("OWNER_EOS_DIAGNOSTIC_END ")]
        if (len(starts) != 1 or starts[0]["checkpoint_sha256"] != CHECKPOINT_SHA or
                starts[0]["beginning_s"] != "2.1080244191689417" or
                ends != ["OWNER_EOS_DIAGNOSTIC_END solver_accepted=1 checkpoint_unchanged=1 migration_authority=false error="]):
            raise ValueError("hot run did not finish exactly")
        rows = list(csv.DictReader(io.StringIO(read(ROOT / run["trajectory"], run["trajectory_sha256"]).decode())))
        if len(rows) != 8:
            raise ValueError("eight accepted steps required")
        for row, ref in zip(rows, reference):
            for key in PHYSICS + ["input_payload_root_sha256", "publication_payload_root_sha256"]:
                if row[key] != ref[key]:
                    raise ValueError("per-step qualified trajectory differs: "+key)
            if row["qualified_kernel_set_sha256"] != KERNEL_SET:
                raise ValueError("unexpected qualified kernel set")
        forest = trees(log)
        if len(forest) != 8:
            raise ValueError("whole-owner profile missing")
        for tree, row in zip(forest, rows):
            total = sum(phase["exclusive_device_sum_ms"] for phase in tree)
            rounding = (2*len(tree)+4)*1e-9+4*math.ulp(float(row["device_ms"]))
            if abs(total-float(row["device_ms"])) > rounding:
                raise ValueError("exclusive device accounting disagrees")
            for phase in tree:
                phases[phase["phase"]][0] += phase["exclusive_device_sum_ms"]
                phases[phase["phase"]][1] += phase["exclusive_wall_ms"]
        counter = summarize(log_path)
        if counter["input_sha256"] != run["log_sha256"]:
            raise ValueError("profile changed during parse")
        for name, value in counter["kernels"].items():
            kernels[name][0] += value["calls"]
            kernels[name][1] += value["inclusive_total_ms"]
        collected.extend(rows)
        repeats.append(table(rows))
    current = table(collected)
    current["wall_minus_device_ms"] = stats([float(r["wall_ms"])-float(r["device_ms"]) for r in collected])
    return dict(schema="rise.fire.r215.target-basis-cost.v1", inputs_sha256=inputs,
                source_commit=SOURCE_COMMIT, executable_sha256=EXE_SHA,
                baseline=baseline["after"], after=current, independent_processes=repeats,
                process_mean_wall_ms=stats([r["wall_ms"]["mean"] for r in repeats]),
                process_mean_device_ms=stats([r["device_ms"]["mean"] for r in repeats]),
                wall_speedup=baseline["after"]["wall_ms"]["mean"]/current["wall_ms"]["mean"],
                device_speedup=baseline["after"]["device_ms"]["mean"]/current["device_ms"]["mean"],
                inclusive_kernels={k:dict(calls=v[0], ms_per_call=v[1]/v[0], ms_per_step=v[1]/24)
                                   for k,v in kernels.items()},
                exclusive_phases_mean_ms={k:dict(device=v[0]/24, wall=v[1]/24) for k,v in phases.items()},
                full_persistent_numerical_bytes_equal=True, excluded_field="producerBuildId",
                hot_accepted_steps=8, timing_samples=24, migration_authority=False,
                scope="Tier8 matched hot states only. Contended qualification and snapshot timings excluded. "
                      "Inclusive kernel intervals overlap; never sum them. Wall minus device remains unattributed. "
                      "No fixed-k, tier10 timing, long-window, or fidelity claim.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = report()
    with args.output.open("x") as stream:
        json.dump(result, stream, sort_keys=True, indent=2, allow_nan=False)
        stream.write("\n")
