#!/usr/bin/env python3
"""Verify a separate cost probe reproduces the sealed prefix before attributing cost."""

import argparse
import collections
import csv
import hashlib
import json
import math
import copy
from pathlib import Path

from check_fire_owner_instrumentation import trees
from analyze_fire_producer_kernels import fields


def records(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def metadata(path):
    result = {}
    for line in path.read_text().splitlines():
        if not line:
            continue
        key, separator, value = line.partition(" ")
        if not separator or not key or key in result:
            raise ValueError("malformed or duplicate metadata: " + str(path))
        result[key] = value
    return result


def completion_record(text, rows, outcome_path):
    terminal = [fields(line) for line in text.splitlines() if line.split()[:1] == ["OWNER_COST_PREFIX"]]
    if len(terminal) != 1:
        raise ValueError("missing or duplicate prefix completion record")
    row = terminal[0]
    if (row["complete"] != "1" or row["error"] != "" or row["full_verdict"] != "unavailable"
            or int(row["steps"]) != len(rows) or not rows
            or not math.isfinite(float(row["wall_s"])) or float(row["wall_s"]) <= 0
            or float(row["time"]) != float(rows[-1]["time_s"])
            or row["outcome_sha256"] != hashlib.sha256(outcome_path.read_bytes()).hexdigest()):
        raise ValueError("missing log/outcome binding")


def compare(reference, probe):
    if len(probe) != 3 or len(reference) < 3:
        raise ValueError("expected exactly three measured diagnostic steps")
    timing_fields = {"device_ms", "wall_ms", "owner_projection_device_ms",
                     "owner_nonprojection_device_ms"}
    fields = set(reference[0]) - timing_fields
    if set(probe[0]) != set(reference[0]):
        raise ValueError("trajectory field schema changed")
    for before, after in zip(reference, probe):
        for field in sorted(fields):
            if before[field] != after[field]:
                raise ValueError("prefix mismatch step=" + after["accepted_step"] + " field=" + field)
    return sorted(fields)


def bind_profile(profile, probe):
    if len(profile) != 3 or len(probe) != 3:
        raise ValueError("expected three complete owner timing trees and trajectory rows")
    for tree, row in zip(profile, probe):
        root = tree[0]
        measured = float(row["device_ms"])
        if not math.isfinite(measured) or measured < 0:
            raise ValueError("invalid trajectory device time")
        # The profile prints nine decimal places; the CSV prints 17 significant
        # digits. This bound covers formatting only, never solver arithmetic.
        if abs(root["device_sum_ms"] - measured) > 1e-9 + 4 * math.ulp(measured):
            raise ValueError("profile device interval belongs to another step")
        if (root["owner_commits"] != int(row["owner_commits"]) or
                root["projection_invocations"] != int(row["owner_projections"])):
            raise ValueError("profile counters belong to another step")
        if [r["stage"] for r in tree if r["phase"] == "SolveStage"] != [0, 1, 2]:
            raise ValueError("missing or reordered owner stage")
        for stage in range(3):
            count = int(row["owner_r%d_iterations" % stage])
            expected = list(range(count))
            for phase, iterations in (("PicardIteration", expected),
                                      ("TerminalVerification", [0x80000000 | (count - 1)]),
                                      ("BuildStageProducerGroup", [0xffffffff] + expected +
                                       [0x80000000 | (count - 1)])):
                actual = [r["raw_iteration"] for r in tree
                          if r["phase"] == phase and r["stage"] == stage]
                if actual != iterations:
                    raise ValueError("profile iteration schedule mismatch: " + phase)


def self_test():
    rows = [dict(accepted_step=str(step), owner_identity=str(step + 7), velocity="0.1",
                 device_ms="12", wall_ms="13", owner_projection_device_ms="2",
                 owner_nonprojection_device_ms="10") for step in range(1, 4)]
    assert "owner_identity" in compare(rows, rows)
    for field in ("owner_identity", "velocity", "accepted_step"):
        mutant = [dict(row) for row in rows]
        mutant[1][field] = "0"
        try:
            compare(rows, mutant)
        except ValueError:
            continue
        raise AssertionError("prefix RED escaped: " + field)
    print("cost prefix identity/velocity/schedule REDs: 3 passed")
    fixture = Path(__file__).resolve().parents[1] / "rendered/fire_production_calibration/r202_owner_cost/exact_edb4afb6"
    profile = trees((fixture / "tier8_profile.log").read_text())
    probe = records(fixture / "tier8_prefix/budgets/maximum_velocity_trajectory.csv")
    bind_profile(profile, probe)
    mutants = [list(reversed(profile)), [profile[0], profile[0], profile[2]]]
    for field, value in (("device_sum_ms", 0), ("owner_commits", 0),
                         ("projection_invocations", 0)):
        mutant = copy.deepcopy(profile)
        mutant[0][0][field] = value
        mutants.append(mutant)
    mutant = copy.deepcopy(profile)
    for row in mutant[0]:
        row["stage"] = 77
    mutants.append(mutant)
    for mutant in mutants:
        try:
            bind_profile(mutant, probe)
        except ValueError:
            continue
        raise AssertionError("profile/trajectory association RED escaped")
    print("profile/trajectory association REDs: 6 passed")
    for invalid in ("nan", "inf", "-inf"):
        mutant = copy.deepcopy(probe)
        mutant[0]["device_ms"] = invalid
        try:
            bind_profile(profile, mutant)
        except ValueError:
            continue
        raise AssertionError("nonfinite trajectory timing RED escaped")
    print("nonfinite trajectory timing REDs: 3 passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--reference-identity", type=Path)
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not all((args.reference, args.reference_identity, args.probe, args.profile, args.output)):
        parser.error("reference, identity, probe directory, profile log, and output required")
    trajectory = args.probe / "budgets/maximum_velocity_trajectory.csv"
    probe = records(trajectory)
    fields = compare(records(args.reference), probe)
    original = metadata(args.reference_identity)
    actual = metadata(args.probe / "diagnostic_from_zero_identity.v1")
    for field in ("seed", "case_record_id", "initial_state_sha256", "resolution_tier"):
        if not actual.get(field) or actual.get(field) != original.get(field):
            raise ValueError("from-zero identity mismatch: " + field)
    outcome = metadata(args.probe / "diagnostic_prefix_outcome.v1")
    if (outcome.get("prefix_complete") != "true" or outcome.get("full_verdict") != "unavailable"
            or outcome.get("scope") != "diagnostic_prefix"):
        raise ValueError("prefix completion/scope check failed")
    profile_text = args.profile.read_text()
    for field, path in (("protocol_sha256", args.probe / "diagnostic_prefix_protocol.v1"),
                        ("from_zero_identity_sha256", args.probe / "diagnostic_from_zero_identity.v1"),
                        ("trajectory_sha256", trajectory),
                        ("retry_trajectory_sha256", args.probe / "budgets/retry_attempt_trajectory.csv")):
        if outcome.get(field) != hashlib.sha256(path.read_bytes()).hexdigest():
            raise ValueError("stale diagnostic outcome: " + field)
    completion_record(profile_text, probe, args.probe / "diagnostic_prefix_outcome.v1")
    profile = trees(profile_text)
    bind_profile(profile, probe)
    measured = []
    for step, tree in enumerate(profile, 1):
        grouped = collections.defaultdict(list)
        for row in tree:
            grouped[row["phase"]].append(row)
        measured.append({"step": step, "owner": tree[0],
                         "stage_totals": grouped["SolveStage"],
                         "phase_exclusive_totals_ms": {
                             phase: {"calls": len(rows),
                                     "wall": sum(r["exclusive_wall_ms"] for r in rows),
                                     "device": sum(r["exclusive_device_sum_ms"] for r in rows)}
                             for phase, rows in grouped.items()},
                         "per_iteration": [r for r in tree if r["phase"] in
                                           ("PicardIteration", "TerminalVerification")]})
    inputs = [args.reference, args.reference_identity, trajectory, args.profile,
              args.probe / "diagnostic_from_zero_identity.v1",
              args.probe / "diagnostic_prefix_protocol.v1", args.probe / "diagnostic_prefix_outcome.v1"]
    report = {"schema": "rise.fire.owner.cost_prefix_comparison.v1",
              "scope": "diagnostic_three_step_prefix", "full_verdict": "unavailable",
              "fixed_k_selected": False, "physics_identity_and_counters_exact": True,
              "compared_fields": fields, "measured": measured,
              "inputs": {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}}
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print("three-step cost prefix: exact initial identity, owner identities, physics, and counters; timing trees valid")


if __name__ == "__main__":
    main()
