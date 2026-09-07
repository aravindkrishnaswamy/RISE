#!/usr/bin/env python3
"""Diagnostic same-column, equal-time rates; NOT an additive contract gate."""
import argparse
import csv
import hashlib
import io
import json
import math
from pathlib import Path

from fire_payload_merkle import verify

BEGIN = 2.1080244191689417
END = 2.1096706851385534
PRODUCTION_COLUMN_SHA = "f26f66da5842ca7e38d21e5ef92d867e646905bd8a12b3777cf61d09f18938fd"


def rows(raw):
    reader = csv.DictReader(io.StringIO(raw.decode()))
    if not reader.fieldnames or len(set(reader.fieldnames)) != len(reader.fieldnames):
        raise ValueError("missing or duplicated CSV fields")
    result = list(reader)
    if any(None in row or None in row.values() for row in result):
        raise ValueError("malformed CSV row")
    return result


def scalar(row, name):
    value = float(row[name])
    if not math.isfinite(value):
        raise ValueError("nonfinite " + name)
    return value


def report(directory, production_column):
    inputs = {}

    def read(path, published=True):
        raw = path.read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        inputs[str(path)] = digest
        if published:
            sidecar = json.loads(path.with_name(path.name + ".payload-v2.json").read_bytes())
            if sidecar["sha256_v1"] != digest or not verify(raw, sidecar["v2"]):
                raise ValueError("payload seal mismatch: " + str(path))
        return raw

    production_raw = read(production_column, False)
    if hashlib.sha256(production_raw).hexdigest() != PRODUCTION_COLUMN_SHA:
        raise ValueError("not the sealed r211 production column")
    production = rows(production_raw)
    schedule = rows(read(directory / "accepted_schedule.csv"))
    summary = read(directory / "composition_summary.v1").decode().splitlines()
    if summary.count("solver_accepted 1") != 1 or summary.count("exact_endpoint_and_schedule_valid 1") != 1:
        raise ValueError("composition has no accepted exact-endpoint result")
    if len(schedule) < 8:
        raise ValueError("composition schedule is incomplete")
    read(directory / "composition_request.v1")
    integrated = [0.0] * 107
    time = BEGIN
    for index, step in enumerate(schedule, 1):
        dt = scalar(step, "dt_s")
        if (step["substep"] != str(index) or scalar(step, "beginning_time_s") != time
                or dt <= 0 or scalar(step, "end_time_s") != time + dt or time + dt > END):
            raise ValueError("noncontiguous or wrong exact schedule")
        checkpoint = directory / "checkpoints" / f"step_{index:02d}.checkpoint"
        if hashlib.sha256(read(checkpoint)).hexdigest() != step["checkpoint_sha256"]:
            raise ValueError("schedule checkpoint mismatch")
        matches = []
        for budget in directory.glob(f"attempt_{805 + index}_*.budget.csv.column.csv"):
            candidate = rows(read(budget))
            if candidate and all(scalar(r, "beginning_time_s") == time and scalar(r, "dt_s") == dt for r in candidate):
                matches.append((budget, candidate))
        if len(matches) != 1:
            raise ValueError("accepted substep budget absent or ambiguous")
        budget, column = matches[0]
        read(Path(str(budget).replace(".budget.csv.column.csv", ".source_packets.bin")))
        if len(column) != 107 or {r["z_face"] for r in column} != {str(z) for z in range(107)}:
            raise ValueError("missing/duplicated column face")
        for row in column:
            if row["x"] != "30" or row["y"] != "33":
                raise ValueError("wrong-tier column mapping")
            integrated[int(row["z_face"])] += dt * scalar(row, "advection_rate")
        time += dt
    if time != END:
        raise ValueError("stale endpoint")
    oracle = [value / (END - BEGIN) for value in integrated]
    if len(production) != 107 or {r["z_face"] for r in production} != {str(z) for z in range(107)}:
        raise ValueError("production column extent mismatch")
    observed = [0.0] * 107
    for row in production:
        if (row["x"] != "30" or row["y"] != "33" or scalar(row, "beginning_time_s") != BEGIN
                or BEGIN + scalar(row, "dt_s") != END):
            raise ValueError("production scope mismatch")
        observed[int(row["z_face"])] = scalar(row, "advection_rate")
    max_reference = max(map(abs, oracle))
    return {"schema": "rise.fire.r213.column-context.v1",
            "scope": "separate_trajectory_context_not_shared_input_or_additive_gate",
            "formal_three_way_verdict": "pending_additive_terms_and_shared_input_gate",
            "beginning_time_s": BEGIN, "end_time_s": END, "column": [30, 33],
            "resolution_tier": 8, "units": "kg_per_m2_s2", "oracle_substeps": len(schedule),
            "observable": "per_face_sum(substep_dt*Heun_advection_rate)/production_interval",
            "oracle_column_max_abs": max_reference,
            "production_column_max_abs": max(map(abs, observed)),
            "production_over_oracle_max_abs": max(map(abs, observed)) / max_reference if max_reference else None,
            "faces": [{"z": z, "oracle_rate": oracle[z], "production_rate": observed[z],
                       "difference": observed[z] - oracle[z]} for z in range(107)],
            "input_sha256": inputs}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("production_column", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    value = report(args.directory, args.production_column)
    with args.output.open("x") as stream:
        json.dump(value, stream, sort_keys=True, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({k: v for k, v in value.items() if k not in ("faces", "input_sha256")}, sort_keys=True))
