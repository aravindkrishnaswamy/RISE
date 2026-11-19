#!/usr/bin/env python3
"""Decompose the attested r204 owner prefixes without confusing inclusive costs."""
import argparse
import collections
import csv
import hashlib
import json
import math
from pathlib import Path

from analyze_fire_producer_kernels import summarize
from check_fire_owner_instrumentation import trees
from seal_fire_payload_placement import bind_counters
from fire_payload_merkle import verify

# This report analyzes this attested campaign, not arbitrary self-signed inputs.
INVENTORY_SHA256 = "294b13cd3c5ba6608526b4f916db07d4ec38c6d046777da9e77eec809ada5a05"


def analyze(directory):
    inventory_path = directory / "inventory.v1.json"
    inventory_bytes = inventory_path.read_bytes()
    seal = json.loads(Path(str(inventory_path) + ".seal-v2.json").read_text())
    if (hashlib.sha256(inventory_bytes).hexdigest() != INVENTORY_SHA256
            or seal["sha256"] != INVENTORY_SHA256
            or not verify(inventory_bytes, seal["payload_v2"])):
        raise ValueError("inventory binding failed")
    inventory = {r["path"]: r for r in json.loads(inventory_bytes)["files"]}
    used = {}

    def bound(relative):
        path = directory / relative
        record = inventory[relative]
        data = path.read_bytes()
        if (hashlib.sha256(data).hexdigest() != record["sha256"]
                or not verify(data, record["payload_v2"])):
            raise ValueError("artifact binding failed: " + relative)
        used[relative] = record
        return path

    samples, kernel_profiles = [], []
    for repeat in range(1, 4):
        name = "attested_repeat%d" % repeat
        log = bound(name + ".log")
        trajectory = bound(name + "/budgets/maximum_velocity_trajectory.csv")
        outcome = bound(name + "/diagnostic_prefix_outcome.v1")
        outcome_fields = dict(line.split(" ", 1) for line in outcome.read_text().splitlines() if " " in line)
        if outcome_fields["trajectory_sha256"] != hashlib.sha256(trajectory.read_bytes()).hexdigest():
            raise ValueError("trajectory does not belong to outcome")
        with trajectory.open() as stream:
            rows = list(csv.DictReader(stream))
        bind_counters(log, rows, outcome)
        for tree, row in zip(trees(log.read_text()), rows):
            samples.append(dict(repeat=repeat, step=int(row["accepted_step"]),
                                device_ms=float(row["device_ms"]), wall_ms=float(row["wall_ms"]),
                                tree=tree))
        kernel_profiles.append(summarize(log))

    def phase_table(sample):
        totals = collections.defaultdict(lambda: dict(device_ms=0.0, wall_ms=0.0))
        for row in sample["tree"]:
            totals[row["phase"]]["device_ms"] += row["exclusive_device_sum_ms"]
            totals[row["phase"]]["wall_ms"] += row["exclusive_wall_ms"]
        return dict(totals)

    selected = sorted(samples, key=lambda r: r["device_ms"])[math.ceil(0.95 * len(samples)) - 1]
    phases = phase_table(selected)
    # Each profile value prints nine decimal places. CSV values use 17
    # significant digits; this bounds text formatting, never solver error.
    formatting_bound = (2 * len(selected["tree"]) + 4) * 1e-9 + 4 * math.ulp(selected["device_ms"])
    if abs(sum(r["device_ms"] for r in phases.values()) - selected["device_ms"]) > formatting_bound:
        raise ValueError("exclusive decomposition failed decimal-format accounting")
    observer = sum(r["child_observer_wall_ms"] for r in selected["tree"])
    root_wall = selected["tree"][0]["wall_ms"]
    outer = selected["wall_ms"] - root_wall
    if outer < 0 or abs(sum(r["wall_ms"] for r in phases.values()) + observer - root_wall) > formatting_bound:
        raise ValueError("wall attribution does not reconcile")
    kernel_totals = collections.defaultdict(lambda: dict(calls=0, inclusive_ms=0.0))
    for profile in kernel_profiles:
        for name, data in profile["kernels"].items():
            kernel_totals[name]["calls"] += data["calls"]
            kernel_totals[name]["inclusive_ms"] += data["inclusive_total_ms"]
    for data in kernel_totals.values():
        data["mean_per_call_ms"] = data["inclusive_ms"] / data["calls"]
        data["mean_per_step_ms"] = data["inclusive_ms"] / len(samples)
    return dict(schema="rise.fire.whole-owner-decomposition.v1", units="milliseconds",
                scope="nine cold-prefix steps; not an onset or whole-window projection",
                inventory_sha256=hashlib.sha256(inventory_bytes).hexdigest(), inputs=used,
                device_p95_step=dict(repeat=selected["repeat"], step=selected["step"],
                                     device_ms=selected["device_ms"], wall_ms=selected["wall_ms"],
                                     inner_owner_wall_ms=root_wall, observer_wall_ms=observer,
                                     outside_owner_scope_wall_ms=outer,
                                     exclusive_phases=phases,
                                     inclusive_stages=[r for r in selected["tree"] if r["phase"] == "SolveStage"]),
                all_steps=[dict(repeat=s["repeat"], step=s["step"], device_ms=s["device_ms"],
                                wall_ms=s["wall_ms"], exclusive_phases=phase_table(s)) for s in samples],
                producer_kernels=kernel_totals,
                producer_command_ms=sum(p["command_elapsed_total_ms"] for p in kernel_profiles),
                producer_encoder_union_ms=sum(p["encoder_union_total_ms"] for p in kernel_profiles),
                producer_uncaptured_ms=sum(p["uncaptured_total_ms"] for p in kernel_profiles),
                coverage="Per-kernel counters cover producer-translation-unit encoders. Force RHS, "
                         "projection internals, blits and gaps are named but not kernel-resolved here. "
                         "Kernel intervals are inclusive and can overlap; only their union is additive. "
                         "Wall-device residual is elapsed overhead, not measured CPU utilization.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.directory), indent=2, sort_keys=True, allow_nan=False))
