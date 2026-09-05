#!/usr/bin/env python3
"""Check observer off/on identity and the printed owner timing tree."""

import argparse
import hashlib
import json
import math
from pathlib import Path


PREFIX = "RISE_FIRE_OWNER_PROFILE_V1 "


def trees(text):
    pending = {}
    result = []
    for line in text.splitlines():
        if not line.startswith(PREFIX):
            continue
        row = json.loads(line[len(PREFIX):])
        index = row["scope"]
        if index in pending or index < 1:
            raise ValueError("duplicate profile scope")
        for name in ("wall_ms", "device_sum_ms", "exclusive_wall_ms",
                     "exclusive_device_sum_ms", "child_observer_wall_ms"):
            if not math.isfinite(row[name]) or row[name] < -1e-8:
                raise ValueError("invalid timing: " + name)
        pending[index] = row
        if row["parent"] == 0:
            if row["phase"] != "OwnerRun" or index != 1:
                raise ValueError("missing owner root")
            for scope in pending.values():
                if scope["parent"] and scope["parent"] not in pending:
                    raise ValueError("missing profile parent")
                if scope["parent"] >= scope["scope"]:
                    raise ValueError("noncausal parent or cycle")
                children = [r for r in pending.values() if r["parent"] == scope["scope"]]
                # Each printed ms value is rounded to 9 decimals. Bound only
                # that decimal formatting loss, not physical solver error.
                rounding_bound_ms = (len(children) + 4) * 1e-9
                expected_device = scope["exclusive_device_sum_ms"] + sum(r["device_sum_ms"] for r in children)
                expected_wall = (scope["exclusive_wall_ms"] + scope["child_observer_wall_ms"]
                                 + sum(r["wall_ms"] for r in children))
                if abs(expected_device - scope["device_sum_ms"]) > rounding_bound_ms:
                    raise ValueError("double-counted or missing device interval")
                if abs(expected_wall - scope["wall_ms"]) > rounding_bound_ms:
                    raise ValueError("double-counted or missing wall interval")
                if abs(scope["wall_ms"] - scope["device_sum_ms"] - scope["wall_minus_device_ms"]) > rounding_bound_ms:
                    raise ValueError("incorrect wall-device gap")
            result.append([pending[key] for key in sorted(pending)])
            pending = {}
    if pending or not result:
        raise ValueError("incomplete or absent owner profile")
    return result


def self_test():
    def raw(rows):
        return "\n".join(PREFIX + json.dumps(row) for row in rows)
    root = dict(scope=1, parent=0, phase="OwnerRun", wall_ms=3.0, device_sum_ms=2.0,
                wall_minus_device_ms=1.0, exclusive_wall_ms=1.0,
                exclusive_device_sum_ms=1.0, child_observer_wall_ms=0.0)
    child = dict(scope=2, parent=1, phase="BuildStageProducerGroup", wall_ms=2.0, device_sum_ms=1.0,
                 wall_minus_device_ms=1.0, exclusive_wall_ms=2.0,
                 exclusive_device_sum_ms=1.0, child_observer_wall_ms=0.0)
    assert len(trees(raw([child, root]))) == 1
    for field, value in (("exclusive_device_sum_ms", 2.0), ("exclusive_wall_ms", 2.0),
                         ("wall_minus_device_ms", 2.0), ("parent", 7)):
        mutant = dict(root)
        mutant[field] = value
        try:
            trees(raw([child, mutant]))
        except ValueError:
            continue
        raise AssertionError("profile accounting RED escaped: " + field)
    print("owner profile accounting REDs: 4 passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--off", type=Path)
    parser.add_argument("--on", type=Path)
    parser.add_argument("--off-trace", type=Path)
    parser.add_argument("--on-trace", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not all((args.off, args.on, args.off_trace, args.on_trace, args.output)):
        parser.error("all off/on log/trace and output paths are required")
    off = args.off.read_text()
    on = args.on.read_text()
    if PREFIX in off:
        raise ValueError("profile observer emits when disabled")
    profile = trees(on)
    for suffix in ("", ".csv"):
        first = Path(str(args.off_trace) + suffix).read_bytes()
        second = Path(str(args.on_trace) + suffix).read_bytes()
        if first != second:
            raise ValueError("observer altered diagnostic operands or identity: " + suffix)
    for text in (off, on):
        if "OWNER_CONVERGENCE_PROBE passed=1 error=\n" not in text:
            raise ValueError("convergence exporter gate did not pass")
        reds = [line for line in text.splitlines() if line.startswith("OWNER_CONVERGENCE_RED ")]
        if len(reds) != 6 or any("passed=1" not in line for line in reds):
            raise ValueError("convergence RED battery did not pass")
    report = {"schema": "rise.fire.owner.observer_checks.v1", "scope": "qualification_fixture_only",
              "profile_off_silent": True, "column_trace_and_csv_bit_identical": True,
              "timing_tree_accounting_passed": True,
              "full_result_bit_comparison_scope": "existing owner fp64 per-field gates; column off/on identity",
              "trees": profile,
              "inputs": {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in (args.off, args.on, args.off_trace, args.on_trace)}}
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print("owner observer: profile off silent; column traces byte-identical; timing trees reconcile")


if __name__ == "__main__":
    main()
