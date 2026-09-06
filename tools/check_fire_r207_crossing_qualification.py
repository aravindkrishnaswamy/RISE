#!/usr/bin/env python3
"""Verify the externally anchored, executed r207 v3 synthetic qualification.

This is not an issuer for arbitrary future executions or physics evidence.
The recorded raw log SHA is the campaign's authority root, not a caller's
self-declared digest. Rehashing a changed log cannot authorize new evidence.
"""

import argparse
import csv
import hashlib
import io
import json
import math
import re
import struct
from pathlib import Path

from analyze_fire_producer_kernels import fields
from check_fire_owner_instrumentation import qualify_artifact


RED_NAMES = {"stale_dt", "missing_event_binding", "export_stale_dt",
             "export_terminal_bit_mutation", "observed_terminal_bit_mutation",
             "stale_input_payload_root", "stale_target_parent", "diagnostic_cannot_publish"}
TAIL_RED_NAMES = {"tail_drained_below_threshold", "terminal_only_excess_not_drained"}
SOURCE = "r207_synthetic_fixture_no_physics_claim"
SYNTHETIC = b"synthetic r207 exporter fixture; not production evidence\n"
EXECUTED_V3_LOG_SHA256 = "d63b483a24d459ac154cb18c566ec9e47f466d6c0619e03d31fd7bebe6c7edd0"
BINDINGS = {"source_ledger_sha256": ".source_ledger.v1",
            "source_observation_inputs_sha256": ".source_observation_inputs.bin",
            "event_sha256": ".event.v1"}


def sha(payload):
    return hashlib.sha256(payload).hexdigest()


def tagged(text, tag):
    return [line for line in text.splitlines() if line.split()[:1] == [tag]]


def one(text, tag):
    lines = tagged(text, tag)
    if len(lines) != 1:
        raise ValueError("missing or duplicate " + tag)
    return fields(lines[0])


def prose_record(line, tag, schema):
    parts = line.split(maxsplit=1)
    if len(parts) != 2 or parts[0] != tag:
        raise ValueError("missing record body: " + tag)
    body = parts[1]
    keys = list(re.finditer(r"(?:^|\s+)([A-Za-z_][A-Za-z_0-9]*)=", body))
    names = [key.group(1) for key in keys]
    if (not keys or keys[0].start() != 0 or len(names) != len(schema)
            or set(names) != set(schema)):
        raise ValueError("incomplete, malformed or duplicate counters: " + tag)
    row = {key.group(1): body[key.end():keys[i+1].start() if i+1 < len(keys)
                            else len(body)].strip() for i, key in enumerate(keys)}
    if any("=" in value for value in row.values()):
        raise ValueError("nested counter inside unquoted value: " + tag)
    return row


def one_prose(text, tag, schema):
    lines = tagged(text, tag)
    if len(lines) != 1:
        raise ValueError("missing or duplicate " + tag)
    return prose_record(lines[0], tag, schema)


def face_topology(probe, faces):
    """Check geometry and complete ordered blocks independently of CSV agreement."""
    shape = tuple(int(value) for value in probe["shape"].split(","))
    column = tuple(int(value) for value in probe["column"].split(","))
    if (len(shape) != 3 or min(shape) <= 0 or len(column) != 2
            or not 0 <= column[0] < shape[0] or not 0 <= column[1] < shape[1]):
        raise ValueError("invalid crossing grid shape or column")
    nx, ny, nz = shape
    x, y = column
    per_block = 5 * nz + 1
    if not faces or len(faces) % per_block or len(faces) < 9 * per_block:
        raise ValueError("incomplete crossing face blocks")
    expected = []
    for z in range(nz):
        for side in range(2):
            expected.extend((("0", str(x + side + (nx + 1) * (y + ny * z)), str(z), str(side)),
                             ("1", str(x + nx * (y + side + (ny + 1) * z)), str(z), str(side))))
    expected.extend(("2", str(x + nx * (y + ny * z)), str(z), "0") for z in range(nz + 1))
    topology = []
    stage = 0
    index = 0
    next_iteration = 0
    for start in range(0, len(faces), per_block):
        block = [fields(line) for line in faces[start:start + per_block]]
        first = block[0]
        raw = int(first["raw_iteration_tag"])
        bootstrap = raw == 0xffffffff
        terminal = not bootstrap and bool(raw & 0x80000000)
        iteration = raw & 0x7fffffff
        phase = "bootstrap" if bootstrap else "terminal_reprojection" if terminal else "picard"
        if (stage > 2 or not 0 <= raw <= 0xffffffff or (index == 0) != bootstrap
                or (not bootstrap and not terminal and iteration != next_iteration)
                or (terminal and (not next_iteration or iteration != next_iteration - 1))):
            raise ValueError("invalid crossing stage or iteration sequence")
        counters = {"stage": "R" + str(stage), "trace_index": str(index),
                    "raw_iteration_tag": str(raw), "phase": phase,
                    "iteration": str(0 if bootstrap else iteration + 1)}
        for row, location in zip(block, expected):
            if (any(row.get(key) != value for key, value in counters.items())
                    or tuple(row.get(key) for key in ("axis", "face", "column_z", "side")) != location):
                raise ValueError("stale, missing or duplicate crossing face")
        topology.append((counters["stage"], counters["trace_index"], counters["raw_iteration_tag"]))
        if terminal:
            stage += 1
            index = 0
            next_iteration = 0
        else:
            index += 1
            if not bootstrap:
                next_iteration += 1
    if stage != 3 or index:
        raise ValueError("missing crossing terminal or stage")
    return shape, topology


def qualify_crossing(log, trace, csv_bytes, crossing, crossing_csv, bindings, tail_payload=b"",
                     scope="qualification_only"):
    if scope != "qualification_only":
        raise ValueError("synthetic exporter inputs cannot qualify production evidence")
    if sha(log.encode("utf-8")) != EXECUTED_V3_LOG_SHA256:
        raise ValueError("log is not the externally anchored executed v3 qualification")
    qualify_artifact(log, trace, csv_bytes)
    reds = [prose_record(line, "OWNER_CROSSING_RED", {"name", "atomic_refusal", "error", "passed"})
            for line in tagged(log, "OWNER_CROSSING_RED")]
    if (len(reds) != 8 or {row["name"] for row in reds} != RED_NAMES
            or any(row["atomic_refusal"] != "1" or row["passed"] != "1"
                   or not row["error"] for row in reds)):
        raise ValueError("crossing RED conjunction did not pass")
    tail_reds = [fields(line) for line in tagged(log, "OWNER_TAIL_TARGET_RED")]
    if (len(tail_reds) != 2 or {row.get("name") for row in tail_reds} != TAIL_RED_NAMES
            or any(set(row) != {"name", "consumed_engagement_retained", "old_terminal_estimator_refuted", "passed"}
                   or row["consumed_engagement_retained"] != "1" or row["old_terminal_estimator_refuted"] != "1"
                   or row["passed"] != "1" for row in tail_reds)):
        raise ValueError("consumed-target RED conjunction did not pass")
    rerun = one_prose(log, "OWNER_CONVERGENCE_PRODUCTION_RERUN",
                      {"terminal_bit_identity", "stage_identity", "reds", "error", "passed"})
    if rerun != {"terminal_bit_identity": "1", "stage_identity": "1", "reds": "1",
                 "error": "", "passed": "1"}:
        raise ValueError("production-stage rerun did not pass")
    export = one_prose(log, "OWNER_CONVERGENCE_EXPORT_FIXTURE", {"synthetic_inputs", "error", "passed"})
    if export != {"synthetic_inputs": "1", "error": "", "passed": "1"}:
        raise ValueError("synthetic writer did not pass")
    published = one(log, "OWNER_CONVERGENCE_CROSSING")
    if (set(published) != {"path", "sha256", "csv_sha256", "terminal_bit_identity",
                          "diagnostic_wall_ms", "passed"}
            or published["passed"] != "1" or published["terminal_bit_identity"] != "1"
            or published["sha256"] != sha(crossing) or published["csv_sha256"] != sha(crossing_csv)
            or published["path"] != one(log, "OWNER_CONVERGENCE_ARTIFACT")["path"]
            + ".synthetic-crossing.convergence.v1"):
        raise ValueError("orphan or failed crossing publication")
    text = crossing.decode()
    event = one(text, "OWNER_CONVERGENCE_EVENT")
    if set(event) != {"version", "accepted_step_beginning", "beginning_time_s", "dt_s", "cell_width_m",
                      "end_time_s", "input_payload_root_sha256", "output_payload_root_sha256",
                      "kernel_set_sha256", "source_packet_identity", *BINDINGS,
                      "terminal_fields_bit_identical", "stage_identities_identical", "solver_publications",
                      "diagnostic_device_ms", "diagnostic_wall_ms", "qualification_trace_staging_count",
                      "fixed_k_selection"}:
        raise ValueError("incomplete or unknown crossing event schema")
    for key, expected in {"version": "1", "accepted_step_beginning": "0",
                          "terminal_fields_bit_identical": "1", "stage_identities_identical": "1",
                          "solver_publications": "0", "fixed_k_selection": "not_authorized"}.items():
        if event.get(key) != expected:
            raise ValueError("invalid synthetic event authority: " + key)
    begin, dt, end = (float(event[key]) for key in ("beginning_time_s", "dt_s", "end_time_s"))
    if not all(math.isfinite(x) for x in (begin, dt, end)) or begin != 0 or dt <= 0 or end != begin + dt:
        raise ValueError("invalid synthetic event times")
    wall = float(event["diagnostic_wall_ms"])
    if not math.isfinite(wall) or wall < 0 or wall != float(published["diagnostic_wall_ms"]):
        raise ValueError("diagnostic timing not bound")
    device = float(event["diagnostic_device_ms"])
    if (not math.isfinite(device) or device < 0
            or not re.fullmatch(r"[1-9][0-9]*", event["source_packet_identity"])
            or int(event["source_packet_identity"]) > 0xffffffffffffffff):
        raise ValueError("invalid crossing device timing or source identity")
    if set(bindings) != set(BINDINGS):
        raise ValueError("incomplete source/event inputs")
    for key, payload in bindings.items():
        if payload != SYNTHETIC or event.get(key) != sha(payload):
            raise ValueError("synthetic binding replaced or orphaned: " + key)
    seals = [fields(line) for line in tagged(log, "OWNER_SEALING_EQUIVALENCE")]
    seals = [row for row in seals if "passed" in row]
    if len(seals) != 1 or seals[0].get("passed") != "1":
        raise ValueError("missing parent owner sealing verdict")
    for child, parent in (("input_payload_root_sha256", "input_root"),
                          ("output_payload_root_sha256", "output_root"),
                          ("kernel_set_sha256", "kernel_set")):
        if event.get(child) != seals[0].get(parent) or not re.fullmatch("[0-9a-f]{64}", event.get(child, "")):
            raise ValueError("orphan crossing owner root: " + child)
    probe = one(text, "OWNER_CONVERGENCE_PROBE")
    if probe.get("source") != SOURCE or probe.get("scope") != "diagnostic_only":
        raise ValueError("synthetic crossing lost isolation label")
    production = one_prose(log, "PROJECTED_HEUN_METAL_OWNER_PRODUCTION_ENTRY",
                           {"accepted", "token", "token_matches", "owner_identity", "diagnostic_identity",
                            "monitored", "enforced", "transfers", "staging", "error", "passed"})
    if (production["passed"] != "1" or production["owner_identity"] != probe.get("owner_identity")
            or production["diagnostic_identity"] != probe.get("owner_identity")):
        raise ValueError("crossing owner identity not production parent")
    lines = text.splitlines(keepends=True)
    probe_start = next(i for i, line in enumerate(lines) if line.split()[:1] == ["OWNER_CONVERGENCE_PROBE"])
    binding_lines = [i for i, line in enumerate(lines) if line.split()[:1] == ["OWNER_CONVERGENCE_BINDING"]]
    if len(binding_lines) != 1 or binding_lines[0] <= probe_start:
        raise ValueError("missing or duplicate crossing trace seal")
    if one(text, "OWNER_CONVERGENCE_BINDING") != {"sha256": sha("".join(lines[probe_start:binding_lines[0]]).encode())}:
        raise ValueError("trace body is not SHA-bound")
    if one(text, "OWNER_CONVERGENCE_CSV") != {"sha256": sha(crossing_csv)}:
        raise ValueError("orphan crossing CSV")
    rows = list(csv.DictReader(io.StringIO(crossing_csv.decode())))
    faces = tagged(text, "OWNER_CONVERGENCE_FACE")
    if len(rows) != len(faces) or not rows:
        raise ValueError("CSV does not cover trace faces")
    metadata = {"scope", "source", "case_record_sha256", "owner_identity"}
    numeric_columns = {"stage", "phase", "raw_iteration_tag", "iteration", "trace_index", "axis", "face",
                       "column_z", "side", "provisional_momentum_kg_m-2_s-1", "projected_velocity_m_s-1",
                       "projected_momentum_kg_m-2_s-1", "shared_alpha", "delta_reference_trace_index",
                       "delta_previous_provisional_momentum_kg_m-2_s-1",
                       "delta_previous_projected_velocity_m_s-1",
                       "delta_previous_projected_momentum_kg_m-2_s-1", "local_convergence_enclosure"}
    header = next(csv.reader(io.StringIO(crossing_csv.decode())))
    if len(header) != len(set(header)) or set(header) != metadata | numeric_columns:
        raise ValueError("missing or duplicate CSV columns")
    for row, face_line in zip(rows, faces):
        if (None in row or row.get("scope") != "diagnostic_only" or row.get("source") != SOURCE
                or row.get("owner_identity") != probe["owner_identity"]
                or row.get("case_record_sha256") != probe["case_record_sha256"]):
            raise ValueError("CSV synthetic isolation or parent identity mismatch")
        face = fields(face_line)
        if any(row[key] != face.get(key, "unavailable") for key in numeric_columns):
            raise ValueError("CSV differs from corresponding trace face")
    payload_record = one(text, "OWNER_CONSUMED_TAIL_PAYLOAD")
    if (set(payload_record) != {"encoding", "sha256", "bytes"}
            or payload_record["encoding"] != "binary32_little_endian"
            or payload_record["sha256"] != sha(tail_payload)
            or payload_record["bytes"] != str(len(tail_payload)) or not tail_payload):
        raise ValueError("missing, tampered or unbound consumed tail payload")
    budget = one(text, "OWNER_PROJECTION_BUDGET")
    if budget != {"pressure_impulse": "combined_tangent_source_tail_projection",
                  "standalone_restoration_projection": "absent", "separated_tail_impulse": "unavailable",
                  "legacy_tail_columns": "terminal_remaining_demand_not_realized_drain"}:
        raise ValueError("projection budget mislabels consumed demand as restoration impulse")
    shape, topology = face_topology(probe, faces)
    cells = math.prod(shape)
    dx = float(event["cell_width_m"])
    if not math.isfinite(dx) or dx <= 0:
        raise ValueError("invalid tail cell width")
    if event["qualification_trace_staging_count"] != str(len(topology)):
        raise ValueError("diagnostic staging count does not cover trace blocks")
    tail_rows = [fields(line) for line in tagged(text, "OWNER_CONSUMED_TAIL_TARGET")]
    schema = {"stage", "trace_index", "raw_iteration_tag", "target_identity", "correction_iteration",
              "nonzero_cells", "requested_increment_volume_m3", "payload_offset_bytes", "payload_cells", "semantics"}
    if len(tail_rows) != len(topology):
        raise ValueError("consumed-tail blocks do not cover owner trace")
    offset = 0
    for row, triple in zip(tail_rows, topology):
        if (set(row) != schema or (row["stage"], row["trace_index"], row["raw_iteration_tag"]) != triple
                or row["payload_offset_bytes"] != str(offset) or row["payload_cells"] != str(cells)
                or row["semantics"] != "last_consumed_target_increment_not_realized_drain"
                or not re.fullmatch(r"[1-9][0-9]*", row["target_identity"])
                or not re.fullmatch(r"0|[1-9][0-9]*", row["correction_iteration"])):
            raise ValueError("stale, noncontiguous or mislabelled consumed target block")
        end_offset = offset + 4 * cells
        if end_offset > len(tail_payload):
            raise ValueError("truncated consumed-target payload")
        values = struct.unpack("<" + "f" * cells, tail_payload[offset:end_offset])
        if any(not math.isfinite(value) for value in values):
            raise ValueError("nonfinite consumed target word")
        count = sum(value != 0 for value in values)
        if row["nonzero_cells"] != str(count) or (row["correction_iteration"] == "0" and count):
            raise ValueError("incorrect consumed target population or uncorrected nonzero target")
        volume = 0.0
        for value in values:
            volume += abs(value) * dt * dx * dx * dx
        reported_volume = float(row["requested_increment_volume_m3"])
        if not math.isfinite(reported_volume) or reported_volume != volume:
            raise ValueError("consumed target volume differs from payload reduction")
        offset = end_offset
    if offset != len(tail_payload):
        raise ValueError("orphan consumed-target payload suffix")
    return {"schema": "rise.fire.r207.crossing_exporter.qualification.v1", "scope": scope,
            "execution_authority": "externally_anchored_v3_raw_log_sha256",
            "crossing_red_count": len(reds), "tail_red_count": len(tail_reds), "log_sha256": sha(log.encode()),
            "consumed_target_sha256": sha(tail_payload), "consumed_target_blocks": len(tail_rows),
            "crossing_sha256": sha(crossing), "crossing_csv_sha256": sha(crossing_csv),
            "physics_claim": "unavailable_synthetic_fixture", "passed": True}


def load_inputs(base):
    cross = Path(str(base) + ".synthetic-crossing")
    # Decode exact bytes: read_text() would silently normalize CRLF before hashing.
    return (Path(str(base) + ".log").read_bytes().decode("utf-8"), base.read_bytes(),
            Path(str(base) + ".csv").read_bytes(), Path(str(cross) + ".convergence.v1").read_bytes(),
            Path(str(cross) + ".convergence.v1.csv").read_bytes(),
            {key: Path(str(cross) + suffix).read_bytes() for key, suffix in BINDINGS.items()},
            Path(str(cross) + ".convergence.v1.tail_targets.bin").read_bytes())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path, help="crossing_gate.vN prefix, not a physics run")
    args = parser.parse_args()
    print(json.dumps(qualify_crossing(*load_inputs(args.fixture)), indent=2, sort_keys=True))
