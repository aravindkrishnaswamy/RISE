#!/usr/bin/env python3
"""Issue r78 v2 from pinned historical execution plus an actual new eight-step probe.

The old executable is authenticated, never re-executed or simulated. The old
trace is an explicitly identified excerpt of its immutable from-zero transcript.
Historical r78 issuers and the native certificate v2 format remain unchanged.
"""
import argparse
import csv
import hashlib
import io
import json
import math
import os
from pathlib import Path
import shutil
import subprocess

from fire_resident_resume_v2 import bits, envelope, require_equivalence, sha, trace_from_executed_csv
from fire_r216_qualified_build import QUALIFIED_EXECUTABLE_SHA256, QUALIFIED_BUILD_ID

OLD_EXE = "45370ad3bcc7035fb228a1f839efe5059cbcc791a64486bd2a1f0b1c3025dc2c"
OLD_BUILD = "0d7537194b877cdffd248de25eccf52262f824b4f1d4cde5fe035ad1eee52175"
CHECKPOINT = "65bb74aa8186a95acde57de593d54de664303c4d92ebaea374eea75e1f138c19"
KERNEL = "d580d078ea083ee18032abfb027271ada9d4d1dabab50b35cb6eb7a503258a0e"
CASE = "c0ace3766badb67e6a775d9fe26e9bbe063e72af539944f6f1ba0698d3173651"
BEGIN = 1.2894936816301197
STEP = 792
COUNT = 8
BASE = Path("rendered/fire_production_calibration/r214_filtered_onset")
RUN = BASE / "tier10_from_zero.v1"
PINS = {
    RUN / "budgets/maximum_velocity_trajectory.csv": "3ec4f50479a59ca90c7610290ad110279e7356f6e0907c470a6fa753a4545b50",
    RUN / "from_zero_identity.v1": "96558b780535ecadc917160084b497ffe413a77fd767b5ec7cb747b90f7f7998",
    Path(str(RUN)+".log"): "b6feee08b9634077e834fbd1de009ddbc1b0f06d5e76849217b8aca95a6768d7",
    Path("docs/fire_r214_tier10_onset.v1"): "983cfb4975a614a7bee2593c6d0f4536679925911afd0b22a5b2e897d6b04dc2",
}


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate identity field: "+key)
        result[key] = value
    return result


def text_metadata(raw):
    pairs = []
    for line in raw.decode().splitlines():
        parts = line.split(" ", 1)
        if len(parts) != 2 or not parts[0]:
            raise ValueError("invalid historical identity")
        pairs.append(parts)
    return unique_pairs(pairs)


def rows(raw):
    reader = csv.DictReader(io.StringIO(raw.decode()))
    if not reader.fieldnames or len(reader.fieldnames) != len(set(reader.fieldnames)):
        raise ValueError("missing or duplicate CSV header")
    result = list(reader)
    if any(None in row or None in row.values() for row in result):
        raise ValueError("malformed CSV row")
    return reader.fieldnames, result


def historical_excerpt(raw):
    names, original = rows(raw)
    # Authenticate the complete recorded timeline, not a caller-selected prefix.
    if len(original) != 805:
        raise ValueError("historical transcript extent differs")
    previous = 0.0
    for index, row in enumerate(original, 1):
        time, dt = float(row["time_s"]), float(row["dt_s"])
        if (row["accepted_step"] != str(index) or not math.isfinite(time)
                or not math.isfinite(dt) or dt <= 0 or time <= previous or previous+dt != time
                or row["qualified_kernel_set_sha256"] != KERNEL):
            raise ValueError("historical trajectory join or kernel differs")
        previous = time
    if bits(float(original[STEP-1]["time_s"])) != bits(BEGIN):
        raise ValueError("historical checkpoint join differs")
    selected = original[STEP:STEP+COUNT]
    stream = io.StringIO()
    writer = csv.DictWriter(stream, fieldnames=names, lineterminator="\n")
    writer.writeheader()
    writer.writerows(selected)
    return stream.getvalue().encode()


def validate_identity(identity, executable_sha):
    build = identity.get("build_id", "")
    if (len(build) != 64 or any(c not in "0123456789abcdef" for c in build)
            or build == OLD_BUILD or identity.get("executable_sha256") != executable_sha
            or executable_sha == OLD_EXE or identity.get("checkpoint_sha256") != CHECKPOINT
            or identity.get("checkpoint_build_id") != OLD_BUILD
            or type(identity.get("resumed_from_step")) is not int or identity["resumed_from_step"] != STEP
            or type(identity.get("beginning_time_s_bits")) is not int
            or identity["beginning_time_s_bits"] != bits(BEGIN)):
        raise ValueError("native executable/checkpoint identity differs")


def validate_trace_pair(old, new, identity):
    for trace, build, executable in ((old, OLD_BUILD, OLD_EXE),
                                      (new, identity["build_id"], identity["executable_sha256"])):
        if (trace["build_id"] != build or trace["executable_sha256"] != executable
                or trace["digest_version"] != 2 or trace["kernel_set_sha256"] != [KERNEL]*COUNT):
            raise ValueError("trace binary or kernel identity differs")
        for key in ("accepted_time_s_bits", "dt_s_bits", "input_payload_roots", "publication_payload_roots"):
            if len(trace[key]) != COUNT:
                raise ValueError("exactly eight accepted steps required")
    require_equivalence(old, new)


def certify(old_exe, new_exe, checkpoint, historical_root, output):
    old_exe, new_exe, checkpoint, historical_root, output = (
        p.resolve() for p in (old_exe, new_exe, checkpoint, historical_root, output))
    if output.exists() or sha(old_exe) != OLD_EXE or sha(checkpoint) != CHECKPOINT:
        raise ValueError("existing output or wrong historical executable/checkpoint")
    if any(name.startswith("RISE_FIRE_") for name in os.environ):
        raise ValueError("fire mutation/diagnostic environment must be empty")
    historical, authority = {}, {}
    for relative, expected in PINS.items():
        path = historical_root / relative
        raw = path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected:
            raise ValueError("historical executed authority SHA differs: "+str(relative))
        historical[relative] = raw
        authority[str(relative)] = expected
    identity_old = text_metadata(historical[RUN / "from_zero_identity.v1"])
    expected_old = dict(producer_build_id=OLD_BUILD, producer_executable_sha256=OLD_EXE,
                        case_record_id=CASE, resolution_tier="10", seed="1234", resume_forbidden="true",
                        protocol_sha256=PINS[Path("docs/fire_r214_tier10_onset.v1")])
    if any(identity_old.get(k) != v for k, v in expected_old.items()):
        raise ValueError("historical from-zero identity differs")
    excerpt = historical_excerpt(historical[RUN / "budgets/maximum_velocity_trajectory.csv"])
    new_sha = sha(new_exe)
    if new_sha != QUALIFIED_EXECUTABLE_SHA256:
        raise ValueError("executable differs from independent r216 qualification pin")
    inspect_raw = subprocess.check_output([str(new_exe), "--fire-resident-resume-inspect", str(checkpoint)])
    identity = json.loads(inspect_raw, object_pairs_hook=unique_pairs)
    validate_identity(identity, new_sha)
    if identity["build_id"] != QUALIFIED_BUILD_ID:
        raise ValueError("build differs from independent r216 qualification pin")
    if sha(new_exe) != new_sha or sha(checkpoint) != CHECKPOINT:
        raise ValueError("native inspection changed inputs")
    output.mkdir(parents=True)
    excerpt_path = output / "historical_rows_793_800.csv"
    excerpt_path.write_bytes(excerpt)
    (output / "new_inspection.json").write_bytes(inspect_raw)
    copied = output / "probe_input.checkpoint"
    shutil.copyfile(checkpoint, copied)
    if sha(copied) != CHECKPOINT:
        raise ValueError("probe checkpoint copy differs")
    run_dir, log = output / "new", output / "new.log"
    command = [str(new_exe), "--fire-r216-resume-probe", str(copied), str(run_dir)]
    with log.open("xb") as stream:
        status = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT).returncode
    lines = log.read_text().splitlines()
    expected_end = "OWNER_CERTIFICATE_DIAGNOSTIC_END solver_accepted=1 checkpoint_unchanged=1 migration_authority=false error="
    expected_execution = "RESIDENT_RESUME_EXECUTION build_id="+identity["build_id"]+" executable_sha256="+new_sha
    if (status != 0 or [v for v in lines if v.startswith("OWNER_CERTIFICATE_DIAGNOSTIC_END ")] != [expected_end]
            or [v for v in lines if v.startswith("RESIDENT_RESUME_EXECUTION ")] != [expected_execution]
            or sha(new_exe) != new_sha or sha(old_exe) != OLD_EXE
            or sha(copied) != CHECKPOINT or sha(checkpoint) != CHECKPOINT):
        raise ValueError("new execution incomplete or its bound inputs changed")
    old = trace_from_executed_csv(excerpt_path, historical_root / Path(str(RUN)+".log"),
                                 old_exe, OLD_BUILD, identity)
    # Native r216 admission re-reads the full historical transcript and selects
    # the pinned rows itself. The explanatory excerpt is never its authority.
    old["csv_path"] = str(historical_root / RUN / "budgets/maximum_velocity_trajectory.csv")
    old["csv_sha256"] = PINS[RUN / "budgets/maximum_velocity_trajectory.csv"]
    new = trace_from_executed_csv(run_dir / "budgets/maximum_velocity_trajectory.csv", log,
                                 new_exe, identity["build_id"], identity)
    validate_trace_pair(old, new, identity)
    for relative, expected in PINS.items():
        if sha(historical_root / relative) != expected:
            raise ValueError("historical evidence changed during probe")
    payload = dict(record_kind="fire-resume-equivalence-certificate-v2", schema_version=2,
                   checkpoint_sha256=CHECKPOINT, resumed_from_step=STEP, beginning_time_s_bits=bits(BEGIN),
                   old_build_id=OLD_BUILD, new_build_id=identity["build_id"], old_executable_sha256=OLD_EXE,
                   new_executable_sha256=new_sha, old_trace=old, new_trace=new)
    pending = output / "resume_equivalence.v2.pending.cbor"
    pending.write_bytes(envelope(payload))
    subprocess.run([str(new_exe), "--fire-resident-resume-validate", str(pending)], check=True)
    if sha(new_exe) != new_sha:
        raise ValueError("validator executable changed")
    certificate = output / "resume_equivalence.v2.cbor"
    pending.rename(certificate)
    bridge = dict(schema="rise.fire.r216.historical-resident-bridge.v2", historical_v1="unchanged_verifier_retained",
                  historical_authority=authority, historical_root=str(historical_root),
                  old_execution="historical_transcript_not_reexecuted", selected_old_accepted_steps=list(range(793, 801)),
                  checkpoint_sha256=CHECKPOINT, certificate_sha256=sha(certificate), payload=payload,
                  execution=dict(command=command, exit_code=status, identity=identity, trace=new),
                  equivalence="N=8 exact time/dt/input-root/publication-root/kernel equality; no shortened onset verdict")
    with (output / "bridge.v2.json").open("x") as stream:
        json.dump(bridge, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    print("CERTIFIED "+str(certificate)+" sha256="+sha(certificate), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("old_executable", "new_executable", "checkpoint", "historical_root", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    certify(args.old_executable, args.new_executable, args.checkpoint, args.historical_root, args.output)
