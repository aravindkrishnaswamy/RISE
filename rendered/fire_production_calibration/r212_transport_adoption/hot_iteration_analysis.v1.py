"""Observational hot EOS counts; not fixed-k or performance qualification."""
import collections
import csv
import hashlib
import json
from pathlib import Path

BASE = Path("rendered/fire_production_calibration")
log_path = BASE / "r212_transport_adoption/hot_iteration_distribution.v1.log"
data_path = BASE / "r212_transport_adoption/hot_iteration_distribution.v1/budgets/maximum_velocity_trajectory.csv"
reference_path = BASE / "r211_resident_migration/continuation.v2/budgets/maximum_velocity_trajectory.csv"
inputs = {}


def read(path):
    raw = path.read_bytes()
    inputs[str(path)] = hashlib.sha256(raw).hexdigest()
    return raw.decode()


log = read(log_path)
assert "OWNER_EOS_DIAGNOSTIC_END solver_accepted=1 checkpoint_unchanged=1 migration_authority=false error=\n" in log
data = list(csv.DictReader(read(data_path).splitlines()))
reference = list(csv.DictReader(read(reference_path).splitlines()))
assert inputs[str(reference_path)] == "cb10278dc49f33c9a9718496804bf81fe5493faf4af0eb27cd1a3b7db3d1a057"
assert len(data) == 8
keys = ("accepted_step", "time_s", "dt_s", "input_payload_root_sha256",
        "publication_payload_root_sha256", "qualified_kernel_set_sha256")
assert all(all(a[k] == b[k] for k in keys) for a, b in zip(data, reference))
expected = [(stage, iteration) for row in data for stage in range(3)
            for n in [int(row[f"owner_r{stage}_iterations"]) ]
            for iteration in [0xffffffff, *range(n), 0x80000000 | (n - 1)]]
observed = []
stages = {stage: dict(calls=0, cell_evaluations=0, bisections=0,
                      previous_temperature_bit_equal=0, lower_endpoint=0,
                      histogram=collections.Counter()) for stage in range(3)}
for line in log.splitlines():
    if not line.startswith("EOS_ITERATIONS_V1 "):
        continue
    words = line.split()[1:]
    row = dict(word.split("=", 1) for word in words)
    assert len(row) == len(words)
    stage, iteration = int(row["stage"]), int(row["raw_iteration"])
    bins = list(map(int, row["histogram"].split(",")))
    cells = int(row["cells"])
    assert row["scope"] == "diagnostic_extra_dispatch"
    assert cells == 504666 and int(row["total"]) == cells
    assert len(bins) == 33 and min(bins) >= 0 and sum(bins) == cells
    assert sum(i*n for i, n in enumerate(bins)) == int(row["bisections"])
    assert int(row["refused"]) == int(row["bit_mismatch"]) == 0
    assert int(row["lower_endpoint"]) == bins[0]
    assert float(row["Tmin"]) == 300 and float(row["Tmax"]) == 2300
    assert 0 <= int(row["previous_temperature_bit_equal"]) <= cells
    observed.append((stage, iteration))
    out = stages[stage]
    out["calls"] += 1
    out["cell_evaluations"] += cells
    for key in ("bisections", "previous_temperature_bit_equal", "lower_endpoint"):
        out[key] += int(row[key])
    out["histogram"].update({i: n for i, n in enumerate(bins) if n})
assert observed == expected
for out in stages.values():
    out["interior_fraction"] = 1 - out["lower_endpoint"] / out["cell_evaluations"]
    out["previous_temperature_bit_equal_fraction"] = out["previous_temperature_bit_equal"] / out["cell_evaluations"]
print(json.dumps(dict(schema="rise.fire.r212.hot-eos-observation.v1", inputs_sha256=inputs,
                     accepted_steps=8, calls=len(observed), stages=stages,
                     payload_roots_equal_at_all_eight_equal_times=True,
                     temperature_bit_mismatches=0,
                     scope="Diagnostic extra dispatch; concurrent oracle means no timing claim. Not migration authority, fixed-k acceptance, or warm-start qualification."),
                 sort_keys=True, indent=2, allow_nan=False))
