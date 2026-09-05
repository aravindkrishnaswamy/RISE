#!/usr/bin/env python3
"""Summarize a sealed owner timing trajectory; never promote a prefix to a verdict."""

import argparse
import collections
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import statistics


TIMINGS = ("device_ms", "wall_ms", "owner_projection_device_ms",
           "owner_nonprojection_device_ms")
COUNTS = ("owner_r0_iterations", "owner_r1_iterations", "owner_r2_iterations")


def p95(values):
    if not values:
        raise ValueError("p95 needs at least one sample")
    return sorted(values)[math.ceil(0.95 * len(values)) - 1]


def summarize(raw):
    rows = list(csv.DictReader(io.StringIO(raw.decode("utf-8"))))
    if not rows:
        raise ValueError("empty owner trajectory")
    groups = collections.defaultdict(list)
    elapsed = 0.0
    for step, row in enumerate(rows, 1):
        if int(row["accepted_step"]) != step or int(row["owner_identity"]) == 0:
            raise ValueError("nonconsecutive or unauthenticated accepted step")
        for name in TIMINGS + ("time_s", "dt_s", "maximum_velocity_m_per_s"):
            row[name] = float(row[name])
            if not math.isfinite(row[name]) or row[name] < 0:
                raise ValueError("invalid measurement: " + name)
        if row["dt_s"] <= 0:
            raise ValueError("nonpositive accepted dt")
        elapsed += row["dt_s"]
        if row["time_s"] != elapsed:
            raise ValueError("recorded endpoint does not equal accepted schedule")
        device_sum = row["owner_projection_device_ms"] + row["owner_nonprojection_device_ms"]
        # Only a timing-accounting check, measured in ms; no physical gate uses it.
        if abs(device_sum - row["device_ms"]) > 4 * math.ulp(max(device_sum, row["device_ms"])):
            raise ValueError("device timing decomposition does not reconcile")
        row["wall_minus_device_ms"] = row["wall_ms"] - row["device_ms"]
        if row["wall_minus_device_ms"] < 0:
            raise ValueError("device time exceeds wall time")
        key = tuple(int(row[name]) for name in COUNTS)
        if any(count < 1 for count in key):
            raise ValueError("accepted stage lacks an iteration count")
        groups[key].append(row)

    def metrics(samples):
        return {name: {"mean": statistics.fmean(r[name] for r in samples),
                       "p95": p95([r[name] for r in samples]),
                       "min": min(r[name] for r in samples),
                       "max": max(r[name] for r in samples)}
                for name in TIMINGS + ("wall_minus_device_ms",)}

    return {
        "schema": "rise.fire.owner.cost.v1",
        "input_sha256": hashlib.sha256(raw).hexdigest(),
        "scope": "accepted_prefix_only; excludes failed attempt and outer source/checkpoint work",
        "p95_definition": "sorted[ceil(0.95*N)-1]; no interpolation",
        "units": "milliseconds",
        "samples": len(rows),
        "end_time_s": elapsed,
        "maximum_velocity_m_per_s": max(r["maximum_velocity_m_per_s"] for r in rows),
        "metrics": metrics(rows),
        "iteration_groups": [
            {"R0_R1_R2": list(key), "samples": len(samples), "metrics": metrics(samples),
             "projection_counts": sorted({int(r["owner_projections"]) for r in samples}),
             "command_counts": sorted({int(r["owner_commits"]) for r in samples})}
            for key, samples in sorted(groups.items())],
        "attribution": {
            "projection_device": "measured resident projection command intervals",
            "other_device": "transport, flux, FCT, EOS, targets, authority checks, momentum, terminal",
            "wall_minus_device": "aggregate encoding/allocation/staging/waits gap; not pure CPU compute",
            "kernel_and_host_subcategories": "not separately measured in this sealed trajectory",
            "iteration_group_differences": "observational, not a matched-state causal speedup"},
        "full_window_verdict": "unavailable",
        "fixed_k_selected": False,
    }


def self_test():
    assert p95(list(range(1, 45))) == 42
    assert p95([5, 1, 3, 2, 4]) == 5
    names = ("accepted_step", "owner_identity", "time_s", "dt_s",
             "maximum_velocity_m_per_s") + TIMINGS + COUNTS + ("owner_projections", "owner_commits")
    values = [1, 7, 0.125, 0.125, 0.2, 10, 12, 3, 7, 2, 2, 3, 13, 74]

    def fixture(vals):
        return (",".join(names) + "\n" + ",".join(map(str, vals)) + "\n").encode()

    result = summarize(fixture(values))
    assert result["metrics"]["wall_minus_device_ms"]["mean"] == 2
    assert result["iteration_groups"][0]["R0_R1_R2"] == [2, 2, 3]
    assert result["fixed_k_selected"] is False
    assert result["full_window_verdict"] == "unavailable"
    mutants = {"missing_owner_seal": (1, 0), "stale_endpoint": (2, 0.25),
               "missing_first_step": (0, 2), "bad_device_partition": (8, 8),
               "device_exceeds_wall": (6, 9), "nan_timing": (5, "nan"),
               "missing_stage_iterations": (9, 0)}
    for label, (index, value) in mutants.items():
        mutated = list(values)
        mutated[index] = value
        try:
            summarize(fixture(mutated))
        except ValueError:
            continue
        raise AssertionError("RED failed: " + label)
    print("owner cost REDs: 7 passed; percentile/scope/iteration checks passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trajectory", type=Path, nargs="?")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if args.trajectory is None:
        parser.error("trajectory required")
    output = json.dumps(summarize(args.trajectory.read_bytes()), indent=2, sort_keys=True) + "\n"
    if args.output:
        with args.output.open("x", encoding="utf-8") as stream:
            stream.write(output)
    else:
        print(output, end="")


if __name__ == "__main__":
    main()
