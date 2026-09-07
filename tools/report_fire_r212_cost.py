#!/usr/bin/env python3
"""Read executed r206/r211/r212 timings; never issue solver acceptance."""
import collections
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics

from analyze_fire_producer_kernels import summarize
from check_fire_owner_instrumentation import trees


def stats(values):
    if not values or any(not math.isfinite(x) or x < 0 for x in values):
        raise ValueError("missing or invalid nonnegative timing samples")
    return dict(samples=len(values), mean=statistics.mean(values),
                stdev=statistics.stdev(values) if len(values) > 1 else 0.0,
                p95=sorted(values)[math.ceil(.95 * len(values)) - 1])


def report(base=Path("rendered/fire_production_calibration")):
    inputs = {}

    def read(path):
        raw = path.read_bytes()
        inputs[str(path)] = hashlib.sha256(raw).hexdigest()
        return raw.decode()

    def rows(path):
        return list(csv.DictReader(read(path).splitlines()))

    trajectory = base / "r211_resident_migration/continuation.v2/budgets/maximum_velocity_trajectory.csv"
    live = rows(trajectory)
    if inputs[str(trajectory)] != "cb10278dc49f33c9a9718496804bf81fe5493faf4af0eb27cd1a3b7db3d1a057":
        raise ValueError("not the accepted r211 trajectory")
    cold = []
    for repeat in range(1, 4):
        cold.extend(rows(base / f"r206_eos/endpoints_qualified_profile_{repeat}.v1/budgets/maximum_velocity_trajectory.csv"))
    keys = ("device_ms", "wall_ms", "owner_projection_device_ms", "owner_nonprojection_device_ms",
            "owner_projections", "owner_r0_iterations", "owner_r1_iterations", "owner_r2_iterations")

    def table(data):
        result = {key: stats([float(r[key]) for r in data]) for key in keys}
        result["total_picard_iterations"] = stats([sum(float(r[f"owner_r{s}_iterations"]) for s in range(3)) for r in data])
        result["wall_minus_device_ms"] = stats([float(r["wall_ms"]) - float(r["device_ms"]) for r in data])
        return result

    hot, phases, kernels = [], collections.defaultdict(lambda: [0.0, 0.0]), collections.defaultdict(lambda: [0, 0.0])
    repeats = []
    for repeat in range(1, 4):
        directory = base / f"r212_transport_adoption/hot_profile_{repeat}.v1"
        data = rows(directory / "budgets/maximum_velocity_trajectory.csv")
        log_path = Path(str(directory) + ".log")
        log = read(log_path)
        if len(data) != 8 or "OWNER_EOS_DIAGNOSTIC_END solver_accepted=1 checkpoint_unchanged=1 migration_authority=false error=\n" not in log:
            raise ValueError("incomplete hot-state execution")
        identity_keys = ("accepted_step", "time_s", "dt_s", "input_payload_root_sha256", "publication_payload_root_sha256", "qualified_kernel_set_sha256")
        if any(any(a[k] != b[k] for k in identity_keys) for a, b in zip(data, live)):
            raise ValueError("profile changed the accepted trajectory")
        forest = trees(log)
        if len(forest) != len(data):
            raise ValueError("missing owner profile")
        for tree, row in zip(forest, data):
            total = sum(x["exclusive_device_sum_ms"] for x in tree)
            formatting = (2 * len(tree) + 4) * 1e-9 + 4 * math.ulp(float(row["device_ms"]))
            if abs(total - float(row["device_ms"])) > formatting:
                raise ValueError("exclusive device accounting does not reconcile")
            for item in tree:
                phases[item["phase"]][0] += item["exclusive_device_sum_ms"]
                phases[item["phase"]][1] += item["exclusive_wall_ms"]
        for name, item in summarize(log_path)["kernels"].items():
            kernels[name][0] += item["calls"]
            kernels[name][1] += item["inclusive_total_ms"]
        hot.extend(data)
        repeats.append(table(data))
    return dict(schema="rise.fire.r212.cost-reconciliation.v1", inputs_sha256=inputs,
                cold_r206=table(cold), production_r211=table(live), hot_profiles=table(hot),
                hot_repeats=repeats,
                exclusive_phases_mean_ms={k: dict(device=v[0]/len(hot), wall=v[1]/len(hot)) for k,v in phases.items()},
                inclusive_kernels={k: dict(calls=v[0], ms_per_call=v[1]/v[0], ms_per_step=v[1]/len(hot)) for k,v in kernels.items()},
                observer=dict(calls=1, wall_ms=19593.469709000001, included_in_production_rows=False),
                scope="846 production continuation steps; three independent eight-step hot profiles; nine historical cold steps. Inclusive kernel intervals are not an additive decomposition. Wall-device residual is not measured hashing cost. No fixed-k or full-window cost claim.")


if __name__ == "__main__":
    print(json.dumps(report(), indent=2, sort_keys=True, allow_nan=False))
