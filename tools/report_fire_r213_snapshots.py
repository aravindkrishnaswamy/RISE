#!/usr/bin/env python3
"""Admit native r213 hot snapshot comparison; never a migration certificate."""
import argparse
import csv
import hashlib
import io
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "rendered/fire_production_calibration"
OUT = BASE / "r213_composition"
NATIVE_SHA = "7a9709517395144d8c6fb5ff94f6cc3cbf27a9cf6882f5e7611509584dc3698a"
REPORTER_RECEIPT_SHA = "0f4aae46c703a96c4ee88ccf14a055bcbbb728f3cac672ef985014dc96e48d9c"
REPORTER_EXECUTABLE = "13f799d3c2fc8b8c6d406c6171f724bd8c321bd51862995e7544d0a1b4e288fb"
REPORTER_BUILD = "c9a2608719e9d962dd8234fe416391bd2c24331fb78bae60489f08ad7d7c82e4"
ROWS = ("accepted_step time_s dt_s maximum_velocity_m_per_s axis face x y z "
        "manifold_max manifold_p95 manifold_p50 tail_cells tail_drained_m3 "
        "owner_r0_iterations owner_r1_iterations owner_r2_iterations "
        "qualified_kernel_set_sha256 input_payload_root_sha256 publication_payload_root_sha256").split()
EXECUTABLES = {
    "baseline": ("9a96d516ddd9e034a64be17818f1669d56f1d434",
                 "a41ff711c91075d94498a02205ca60872e0506b95cf0e26e3ebcb7b63ff8ba62",
                 "baseline_snapshot_qualification.v1.json"),
    "candidate": ("d1406f4f2bfa25bc32f0009867033bbb93b920f1",
                  "298fbbec37e7adcac94ffe3792544fd5cc4f0e8c82c3edb3a7afaf1dcd449ae6",
                  "qualification.v3.json"),
}
RECEIPTS = {"baseline": "4cc28462f9bc6428bc64b11624422e42501c6a4433a1e8c8d948aa27c323e5cd",
            "candidate": "4fed2be311e0cc42cfa839702dfbad89fba61a74e52064ee02e402e306362234"}
LOGS = {"baseline": "29563a32594be06fb0533711215acda6e569f59628c368497f404306c7107413",
        "candidate": "ad1d758f071400daf9967176e730b8a60c8df37e6962f583e8006c332687bad7"}


def pairs(tokens):
    if len(tokens) % 2 or len(set(tokens[::2])) != len(tokens)//2:
        raise ValueError("malformed or duplicate native fields")
    return dict(zip(tokens[::2], tokens[1::2]))


def admit_native(text):
    headers, steps, scalar, faces, reds = {}, {}, {}, {}, set()
    for line in text.splitlines():
        t = line.split()
        if not t:
            raise ValueError("empty native row")
        if t[0] == "step":
            row = pairs(t)
            key = int(row["step"])
            if key in steps:
                raise ValueError("duplicate native step")
            steps[key] = row
        elif t[0] in ("scalar", "face"):
            row = pairs(t[1:])
            table = scalar if t[0] == "scalar" else faces
            key = (int(row["step"]), row.get("component", row.get("field")), row.get("axis", ""))
            if key in table:
                raise ValueError("duplicate native field")
            table[key] = row
        elif t[0] == "red":
            if len(t) != 3 or t[-1] != "1" or t[1] in reds:
                raise ValueError("failed native RED")
            reds.add(t[1])
        else:
            if len(t) != 2 or t[0] in headers:
                raise ValueError("malformed or duplicate native header")
            headers[t[0]] = t[1]
    required = dict(schema="rise.fire.r213.numeric-snapshot-comparison.v1",
                    scope="canonical_persistent_payload_minus_producer_build_not_migration",
                    excluded_field="producerBuildId", required_steps="8", passed="1",
                    migration_authority="false")
    if any(headers.get(k) != v for k, v in required.items()):
        raise ValueError("native authority or completion mismatch")
    if "persisted_source_ledger_and_temperature_history_mutations_refused" not in reds:
        raise ValueError("native persisted-data RED missing")
    for name in ("reporter_build_id", "reporter_executable_sha256"):
        value = headers.get(name, "")
        if len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
            raise ValueError("native reporter identity missing")
    if set(steps) != set(range(1, 9)) or len(scalar) != 80 or len(faces) != 48:
        raise ValueError("eight complete per-field snapshots required")
    for step, row in steps.items():
        if (row["exact_bytes_equal"] != "1" or int(row["bytes"]) <= 0
                or row["old_comparison_sha256"] != row["new_comparison_sha256"]):
            raise ValueError("canonical persistent payload differs")
        for name in ("old_checkpoint_sha256", "new_checkpoint_sha256", "old_comparison_sha256",
                     "new_comparison_sha256", "old_build", "new_build"):
            if len(row[name]) != 64 or any(c not in "0123456789abcdef" for c in row[name]):
                raise ValueError("malformed native SHA")
        for component in range(10):
            field = scalar[(step, str(component), "")]
            if field["cells"] != "504666" or field["bit_mismatches"] != "0":
                raise ValueError("scalar bit comparison failed or incomplete")
        for field in ("momentum", "velocity"):
            for axis, count in enumerate((511980, 511980, 509427)):
                row_field = faces[(step, field, str(axis))]
                if row_field["faces"] != str(count) or row_field["bit_mismatches"] != "0":
                    raise ValueError("face bit comparison failed or incomplete")
    return steps, headers


def report(comparison):
    inputs = {}

    def read(path, expected=None):
        path = path.resolve()
        raw = path.read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        if expected is not None and digest != expected:
            raise ValueError("evidence SHA mismatch: "+str(path))
        inputs[str(path.relative_to(ROOT))] = digest
        return raw

    steps, headers = admit_native(read(comparison, NATIVE_SHA).decode())
    metal_path = "src/Library/Utilities/FireProductionAdvectionMac.mm"
    source_identity = {}
    for kind, expected in (("baseline", "c19d92c93fbfb488fcd2eb6c4be36f57368cf09a0efdd6705a4e52075ad4adb5"),
                           ("candidate", "5b09beacf034257b5bc846023da7f0d42ccf690dd49d61229f6de9859007e9d7")):
        commit = EXECUTABLES[kind][0]
        source = subprocess.check_output(["git", "show", commit+":"+metal_path], cwd=ROOT)
        if hashlib.sha256(source).hexdigest() != expected:
            raise ValueError("snapshot executable source lineage changed")
        source_identity[kind] = dict(source_commit=commit, metal_path=metal_path, metal_source_sha256=expected)
    old_commit = "9f1b80fb64a196b81b1f067833f77f4d7325e4b7"
    if subprocess.check_output(["git", "show", old_commit+":"+metal_path], cwd=ROOT) != subprocess.check_output(
            ["git", "show", EXECUTABLES["baseline"][0]+":"+metal_path], cwd=ROOT):
        raise ValueError("baseline changed original EOS source")
    delta = subprocess.check_output(["git", "diff", "--name-only", old_commit,
                                    EXECUTABLES["baseline"][0]], cwd=ROOT).decode().splitlines()
    if delta != ["tests/FireSequenceTest.cpp"]:
        raise ValueError("baseline addon is not diagnostic-only")
    reporter = json.loads(read(OUT / "qualification.v4.json", REPORTER_RECEIPT_SHA))
    if (reporter["producer_executable_sha256"] != REPORTER_EXECUTABLE
            or headers["reporter_executable_sha256"] != REPORTER_EXECUTABLE
            or headers["reporter_build_id"] != REPORTER_BUILD
            or reporter["source_commit"] != "02456eeb198133e2d8567db4ca443adeb653c575"
            or set(reporter["runs"]) != {"build", "publication", "owner"}
            or any(run["exit_code"] != 0 for run in reporter["runs"].values())):
        raise ValueError("native comparison is not bound to qualified reporter")
    for kind, (commit, executable, receipt) in EXECUTABLES.items():
        qualified = json.loads(read(OUT / receipt, RECEIPTS[kind]))
        if (qualified["source_commit"] != commit or qualified["producer_executable_sha256"] != executable
                or set(qualified["runs"]) != {"build", "publication", "owner"}
                or any(run["exit_code"] != 0 for run in qualified["runs"].values())):
            raise ValueError("qualified snapshot producer identity mismatch")
        directory = OUT / (kind+"_hot_snapshots.v1")
        log = read(Path(str(directory)+".log"), LOGS[kind]).decode()
        expected_end = "OWNER_EOS_DIAGNOSTIC_END solver_accepted=1 checkpoint_unchanged=1 migration_authority=false error="
        if [r for r in log.splitlines() if r.startswith("OWNER_EOS_DIAGNOSTIC_END")] != [expected_end]:
            raise ValueError("snapshot producer did not complete")
        execution = [r for r in log.splitlines() if r.startswith("OWNER_EOS_SNAPSHOT_EXECUTION ")]
        if len(execution) != 1 or ("executable_sha256="+executable) not in execution[0]:
            raise ValueError("snapshot execution identity missing")
        actual = list(csv.DictReader(io.StringIO(read(directory / "budgets/maximum_velocity_trajectory.csv").decode())))
        reference_path = (BASE / "r211_resident_migration/continuation.v2/budgets/maximum_velocity_trajectory.csv"
                          if kind == "baseline" else OUT / "hot_profile_1.v1/budgets/maximum_velocity_trajectory.csv")
        reference_sha = ("cb10278dc49f33c9a9718496804bf81fe5493faf4af0eb27cd1a3b7db3d1a057" if kind == "baseline"
                         else "ccb7b42ded700273c49218c11c7c917de22f805f019afe674fdc2d0508154e3e")
        reference = list(csv.DictReader(io.StringIO(read(reference_path, reference_sha).decode())))[:8]
        if len(actual) != 8 or len(reference) != 8:
            raise ValueError("exact eight-step root reproduction required")
        side = "old" if kind == "baseline" else "new"
        for index, (row, historical) in enumerate(zip(actual, reference), 1):
            native = steps[index]
            if (any(row[k] != historical[k] for k in ROWS) or row["time_s"] != native["time_s"]
                    or row["accepted_step"] != str(1300+index)
                    or ("build_id="+native[side+"_build"]) not in execution[0]):
                raise ValueError("exact-time historical payload reproduction failed")
            read(directory / "checkpoints" / f"step_{index:02d}.checkpoint", native[side+"_checkpoint_sha256"])
    return dict(schema="rise.fire.r213.hot-numerical-qualification.v2", inputs_sha256=inputs,
                snapshot_producer_source_identity=source_identity,
                baseline_parent_commit=old_commit, baseline_changed_paths=delta,
                reporter_source_commit=reporter["source_commit"], reporter_build_id=headers["reporter_build_id"],
                reporter_executable_sha256=headers["reporter_executable_sha256"],
                accepted_steps=8, beginning_s=2.1080244191689417, end_s=float(steps[8]["time_s"]),
                canonical_persistent_payload_identical=True, excluded_field="producerBuildId",
                scalar_components_per_step=10, cells_per_component=504666,
                face_fields_per_step=6, face_counts_per_axis=[511980, 511980, 509427],
                historical_full_root_reproduction=dict(baseline=True, candidate=True),
                per_step=steps, migration_authority=False,
                scope="Diagnostic-qualified old/new numerical-byte comparison using native checkpoint loader and serializer. The full persistent canonical payload, including source/statistical ledgers and history, is equal after excluding only producerBuildId. No r78 migration, fixed-k, or long-window authority; no timing claim from these snapshot runs.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("comparison", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    text = json.dumps(report(args.comparison), sort_keys=True, indent=2, allow_nan=False)+"\n"
    if args.output:
        with args.output.open("x") as stream:
            stream.write(text)
    else:
        print(text, end="")
