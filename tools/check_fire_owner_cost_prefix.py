#!/usr/bin/env python3
"""Verify a separate cost probe reproduces the sealed prefix before attributing cost."""

import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path

from check_fire_owner_instrumentation import trees


def records(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def metadata(path):
    return dict(line.split(" ", 1) for line in path.read_text().splitlines() if " " in line)


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
    fields = compare(records(args.reference), records(trajectory))
    original = metadata(args.reference_identity)
    actual = metadata(args.probe / "diagnostic_from_zero_identity.v1")
    for field in ("seed", "case_record_id", "initial_state_sha256", "resolution_tier"):
        if actual.get(field) != original.get(field):
            raise ValueError("from-zero identity mismatch: " + field)
    outcome = metadata(args.probe / "diagnostic_prefix_outcome.v1")
    if (outcome.get("prefix_complete") != "true" or outcome.get("full_verdict") != "unavailable"
            or outcome.get("scope") != "diagnostic_prefix"):
        raise ValueError("prefix completion/scope check failed")
    profile = trees(args.profile.read_text())
    if len(profile) != 3:
        raise ValueError("expected three complete owner timing trees")
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
