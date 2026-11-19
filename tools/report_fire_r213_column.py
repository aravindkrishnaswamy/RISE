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
REQUEST_FIXED = {
    "schema": "rise.fire.r213.oracle-tier8-composition.v1",
    "scope": "separate_oracle_trajectory_reference_not_shared_input_gate",
    "formal_contract_verdict": "pending_additive_terms_and_shared_input_gate",
    "resolution_tier": "8", "column_x": "30", "column_y": "33", "seed": "1234", "case_duration_s": "3",
    "case_record_id": "6eb8b1f95bfdb27d06245db94d6e9513c0cdef3662fc055547d35b353094f7e1",
    "substep_policy": "CFL_and_ordinary_rejection_below_nominated_ceiling",
    "source_policy": "canonical_fp64_recomputed_at_each_substep_full_bytes_retained",
    "checkpoint_sha256": "ab91898e0279a347e85983853e8fe4167113a2364cdcffb6bad78e327d4e03bf",
    "checkpoint_producer_build_id": "35c4f59e4d603eebcad2c37d67860a018b0e13266b6ff7f4807f0c3667d30feb",
    "producer_build_id": "5b8590a4dd9e6d862ae19861793f1f9b6890bb18e25cb22b34fc2563474135c6",
    "producer_executable_sha256": "8b7432df0c5637fc019df52dd77cae0648d9a5c82bc9c3db4f2779ef97c465bb",
    "migration_authority": "false",
}


def metadata(raw):
    result = {}
    for line in raw.decode().splitlines():
        key, separator, value = line.partition(" ")
        if not key or not separator or key in result:
            raise ValueError("missing or duplicated metadata field")
        result[key] = value
    return result


def unique_json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON seal key")
        result[key] = value
    return result


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
            sidecar_raw = path.with_name(path.name + ".payload-v2.json").read_bytes()
            sidecar = json.loads(sidecar_raw, object_pairs_hook=unique_json_object)
            inputs[str(path) + ".payload-v2.json"] = hashlib.sha256(sidecar_raw).hexdigest()
            if (set(sidecar) != {"schema", "case_record_id", "sha256_v1", "v2"}
                    or sidecar["schema"] != "rise.fire.published-payload.v2"
                    or sidecar["case_record_id"] != REQUEST_FIXED["case_record_id"]
                    or sidecar["sha256_v1"] != digest or not verify(raw, sidecar["v2"])):
                raise ValueError("payload seal mismatch: " + str(path))
        return raw

    production_raw = read(production_column, False)
    if hashlib.sha256(production_raw).hexdigest() != PRODUCTION_COLUMN_SHA:
        raise ValueError("not the sealed r211 production column")
    production = rows(production_raw)
    schedule_raw = read(directory / "accepted_schedule.csv")
    schedule = rows(schedule_raw)
    summary = metadata(read(directory / "composition_summary.v1"))
    expected_summary = {"schema", "solver_accepted", "exact_endpoint_and_schedule_valid", "end_time_s",
                        "wall_s", "schedule_sha256", "formal_contract_verdict", "error"}
    if (set(summary) != expected_summary or summary["schema"] != "rise.fire.r213.oracle-composition-summary.v1"
            or summary["solver_accepted"] != "1" or summary["exact_endpoint_and_schedule_valid"] != "1"
            or scalar(summary, "end_time_s") != END or scalar(summary, "wall_s") <= 0
            or summary["error"] or summary["formal_contract_verdict"] != REQUEST_FIXED["formal_contract_verdict"]
            or summary["schedule_sha256"] != hashlib.sha256(schedule_raw).hexdigest()):
        raise ValueError("composition has no accepted exact-endpoint result")
    if len(schedule) < 8:
        raise ValueError("composition schedule is incomplete")
    request = metadata(read(directory / "composition_request.v1"))
    varying = {"beginning_time_s", "end_time_s", "subdivision_nomination", "maximum_substep_s"}
    if (set(request) != set(REQUEST_FIXED) | varying or any(request[key] != value for key, value in REQUEST_FIXED.items())
            or scalar(request, "beginning_time_s") != BEGIN or scalar(request, "end_time_s") != END
            or request["subdivision_nomination"] not in ("8", "16", "32")
            or scalar(request, "maximum_substep_s") != (END - BEGIN) / int(request["subdivision_nomination"])):
        raise ValueError("composition request identity mismatch")
    ceiling = scalar(request, "maximum_substep_s")
    integrated = [0.0] * 107
    time = BEGIN
    for index, step in enumerate(schedule, 1):
        dt = scalar(step, "dt_s")
        if (step["substep"] != str(index) or scalar(step, "beginning_time_s") != time
                or dt <= 0 or dt > ceiling or scalar(step, "end_time_s") != time + dt or time + dt > END):
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
    oracle_face = max(range(107), key=lambda z: abs(oracle[z]))
    production_face = max(range(107), key=lambda z: abs(observed[z]))
    return {"schema": "rise.fire.r213.column-context.v1",
            "scope": "separate_trajectory_context_not_shared_input_or_additive_gate",
            "formal_three_way_verdict": "pending_additive_terms_and_shared_input_gate",
            "beginning_time_s": BEGIN, "end_time_s": END, "column": [30, 33],
            "resolution_tier": 8, "units": "kg_per_m2_s2", "oracle_substeps": len(schedule),
            "observable": "per_face_sum(substep_dt*Heun_advection_rate)/production_interval",
            "oracle_column_max_abs": max_reference,
            "production_column_max_abs": max(map(abs, observed)),
            "production_over_oracle_max_abs": max(map(abs, observed)) / max_reference if max_reference else None,
            "maximum_ratio_authority": "descriptive_only_maxima_can_be_at_different_faces",
            "oracle_maximum_face_z": oracle_face,
            "production_maximum_face_z": production_face,
            "oracle_rate_at_production_maximum_face": oracle[production_face],
            "production_rate_at_production_maximum_face": observed[production_face],
            "same_production_face_magnitude_ratio": (abs(observed[production_face] / oracle[production_face])
                                                      if oracle[production_face] else None),
            "production_rate_at_oracle_maximum_face": observed[oracle_face],
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
