#!/usr/bin/env python3
"""Check observer off/on identity and the printed owner timing tree."""

import argparse
import hashlib
import json
import math
from pathlib import Path
from analyze_fire_producer_kernels import fields


PREFIX = "RISE_FIRE_OWNER_PROFILE_V1 "
FP64_PASS = ("PROJECTED_HEUN_METAL_OWNER_FP64 source=1 begin=1 r0=1 r1=1 accepted=1 "
             "criterion=conjunction_of_per_cell_per_field_same_unit_enclosures error= passed=1")
RED_NAMES = {"stale_column_trace", "out_of_order_iteration", "truncated_face_shape",
             "mismatched_stage", "production_scope_forbidden", "mismatched_owner"}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate profile counter: " + key)
        result[key] = value
    return result


def qualify_artifact(log, trace, csv):
    def tagged(tag):
        return [line for line in log.splitlines() if line.split()[:1] == [tag]]
    verdicts = [line for line in log.splitlines() if line.startswith("RESIDENT_TARGET passed=")]
    if len(verdicts) != 1 or tagged("RESIDENT_TARGET") != verdicts or fields(verdicts[0]).get("passed") != "1":
        raise ValueError("complete qualification fixture did not pass")
    if (tagged("PROJECTED_HEUN_METAL_OWNER_FP64") != [FP64_PASS]
            or tagged("OWNER_CONVERGENCE_PROBE") != ["OWNER_CONVERGENCE_PROBE passed=1 error="]):
        raise ValueError("fp64 owner or convergence qualification did not pass")
    artifacts = [line for line in log.splitlines() if line.startswith("OWNER_CONVERGENCE_ARTIFACT ")]
    expected = " sha256=" + hashlib.sha256(trace).hexdigest() + " scope=qualified_fixture_only passed=1"
    if len(artifacts) != 1 or not artifacts[0].endswith(expected):
        raise ValueError("trace does not belong to qualified run")
    csv_seals = [line for line in trace.decode().splitlines() if line.startswith("OWNER_CONVERGENCE_CSV ")]
    if csv_seals != ["OWNER_CONVERGENCE_CSV sha256=" + hashlib.sha256(csv).hexdigest()]:
        raise ValueError("CSV does not belong to qualified trace")
    reds = [line for line in log.splitlines() if line.startswith("OWNER_CONVERGENCE_RED ")]
    if (len(reds) != len(RED_NAMES) or
            {line.split()[1] for line in reds} != {"name=" + name for name in RED_NAMES} or
            any(not line.endswith(" passed=1") or " atomic_refusal=1 " not in line for line in reds)):
        raise ValueError("named convergence RED battery did not pass")


def trees(text):
    pending = {}
    result = []
    for line in text.splitlines():
        if not line.startswith(PREFIX):
            continue
        row = json.loads(line[len(PREFIX):], object_pairs_hook=unique_object)
        expected = {"scope", "parent", "phase", "stage", "raw_iteration", "iteration_kind",
                    "wall_ms", "device_sum_ms", "wall_minus_device_ms", "exclusive_wall_ms",
                    "exclusive_device_sum_ms", "child_observer_wall_ms", "count_scope",
                    "owner_commits", "projection_invocations"}
        if set(row) != expected or row["count_scope"] != "inclusive":
            raise ValueError("incomplete or unknown profile schema")
        stage, iteration = row["stage"], row["raw_iteration"]
        if stage not in (0, 1, 2, 0xffffffff) or not isinstance(iteration, int) or not 0 <= iteration <= 0xffffffff:
            raise ValueError("invalid stage or iteration tag")
        kind = ("owner" if stage == 0xffffffff else "bootstrap" if iteration == 0xffffffff
                else "terminal" if iteration & 0x80000000 else "picard")
        if row["iteration_kind"] != kind:
            raise ValueError("inconsistent iteration class")
        index = row["scope"]
        if index in pending or index < 1:
            raise ValueError("duplicate profile scope")
        for name in ("wall_ms", "device_sum_ms", "exclusive_wall_ms",
                     "exclusive_device_sum_ms", "child_observer_wall_ms"):
            if not math.isfinite(row[name]) or row[name] < -1e-8:
                raise ValueError("invalid timing: " + name)
        if not math.isfinite(row["wall_minus_device_ms"]):
            raise ValueError("nonfinite wall-device gap")
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
    root.update(stage=0xffffffff, raw_iteration=0xffffffff, iteration_kind="owner")
    child.update(stage=0, raw_iteration=0xffffffff, iteration_kind="bootstrap")
    for row in (root, child):
        row.update(count_scope="inclusive", owner_commits=0, projection_invocations=0)
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
    for field in ("wall_ms", "device_sum_ms", "exclusive_wall_ms",
                  "exclusive_device_sum_ms", "child_observer_wall_ms", "wall_minus_device_ms"):
        for value in (float("nan"), float("inf"), -float("inf")):
            mutant = dict(root)
            mutant[field] = value
            try:
                trees(raw([child, mutant]))
            except ValueError:
                continue
            raise AssertionError("nonfinite timing RED escaped: " + field)
    print("owner profile accounting REDs: 4 passed; nonfinite REDs: 18 passed")
    for field, value in (("stage", 77), ("raw_iteration", -1), ("iteration_kind", "impossible")):
        mutant = dict(root)
        mutant[field] = value
        try:
            trees(raw([child, mutant]))
        except ValueError:
            continue
        raise AssertionError("iteration tag RED escaped")
    print("profile stage/iteration tag REDs: 3 passed")
    fixture = Path(__file__).resolve().parents[1] / "rendered/fire_production_calibration/r202_owner_cost/exact_edb4afb6"
    log = (fixture / "fixture_off.log").read_text()
    trace, csv = ((fixture / name).read_bytes() for name in ("fixture_off.v1", "fixture_off.v1.csv"))
    qualify_artifact(log, trace, csv)
    for mutant in ((log, b"", b""), (log, trace, b""),
                   (log.replace(FP64_PASS, ""), trace, csv),
                   (log.replace("name=mismatched_owner", "name=stale_column_trace"), trace, csv),
                   (log.split("OWNER_CONVERGENCE_PROBE passed=1 error=")[0] +
                    "OWNER_CONVERGENCE_PROBE passed=1 error=\n", trace, csv),
                   (log.replace("RESIDENT_TARGET passed=1", "RESIDENT_TARGET passed=0"), trace, csv)):
        try:
            qualify_artifact(*mutant)
        except ValueError:
            continue
        raise AssertionError("qualified artifact/RED-name binding mutant escaped")
    print("qualified artifact/fp64/RED-name/terminal-verdict binding REDs: 6 passed")


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
    for text, trace in ((off, args.off_trace), (on, args.on_trace)):
        qualify_artifact(text, trace.read_bytes(), Path(str(trace) + ".csv").read_bytes())
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
