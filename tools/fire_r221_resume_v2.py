#!/usr/bin/env python3
"""Certify the approved 826 -> 835 historical overlap, never an invented old run."""
import argparse
import copy
import csv
import io
import json
import math
import os
from pathlib import Path
import shutil
import subprocess

from fire_resident_resume_v2 import bits, envelope, require_equivalence, sha, trace_from_executed_csv
from fire_r216_resume_v2 import rows, text_metadata, unique_pairs

OLD_EXE = "91978041bb53124e45324838a082d6fb64bb0f2a6da28c68ddc7e8c186faaefa"
OLD_BUILD = "f2ee8ec4cdf43ed7c527dce95718439888561cb70861f99c22f3f5fd4a311609"
CHECKPOINT = "f18cac44b71b1fdac106d30d86bcc1efb955ed39ed4edc0a66be912a4130c337"
KERNEL = "e938373ec6cd9e0217c04242ad96cd4d46055236e422bd7ad0ea34e164d0a8b2"
CASE = "c0ace3766badb67e6a775d9fe26e9bbe063e72af539944f6f1ba0698d3173651"
BEGIN, STEP, COUNT = 1.3440701774088666, 826, 9
RUN = Path("rendered/fire_production_calibration/r217_clause_diagnostic/tier10_from_zero.v1")
PROTOCOL = Path("docs/fire_r214_tier10_onset.v1")
PINS = {
    RUN / "budgets/maximum_velocity_trajectory.csv": "7c9414de24da4a82dbcc9381364b4b2d2fae2a54a5cc6c7b5005b026ab03fb30",
    RUN / "from_zero_identity.v1": "2334b0bb913b9bc5fa42a34f55c6fb1bf6ba1d43015baec7afcd2403a5292828",
    Path(str(RUN)+".log"): "3682feb561f5c201da8120bb76c3dc533e5fffa0bc67a04c069886997a2f5b51",
    PROTOCOL: "983cfb4975a614a7bee2593c6d0f4536679925911afd0b22a5b2e897d6b04dc2",
}


def historical_excerpt(raw):
    names, original = rows(raw)
    if len(original) != 835:
        raise ValueError("historical transcript extent differs")
    previous = 0.0
    for index, row in enumerate(original, 1):
        time, dt = float(row["time_s"]), float(row["dt_s"])
        if (row["accepted_step"] != str(index) or not math.isfinite(time)
                or not math.isfinite(dt) or dt <= 0 or time <= previous or previous+dt != time
                or row["qualified_kernel_set_sha256"] != KERNEL):
            raise ValueError("historical timeline/kernel differs")
        previous = time
    if bits(float(original[STEP-1]["time_s"])) != bits(BEGIN):
        raise ValueError("historical checkpoint join differs")
    stream = io.StringIO()
    writer = csv.DictWriter(stream, fieldnames=names, lineterminator="\n")
    writer.writeheader()
    writer.writerows(original[STEP:STEP+COUNT])
    return stream.getvalue().encode()


def validate_identity(identity, executable_sha, qualified_build):
    if (identity.get("build_id") != qualified_build or qualified_build == OLD_BUILD
            or identity.get("executable_sha256") != executable_sha or executable_sha == OLD_EXE
            or identity.get("checkpoint_sha256") != CHECKPOINT
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
            raise ValueError("trace binary/kernel identity differs")
        for key in ("accepted_time_s_bits", "dt_s_bits", "input_payload_roots", "publication_payload_roots"):
            if len(trace[key]) != COUNT:
                raise ValueError("exactly nine historical overlap steps required")
    require_equivalence(old, new)


def native_reds(executable, payload, output):
    """Mutate real certificate assertions, re-hash the envelope, require native refusal."""
    results = []
    mutations = []
    for step in range(COUNT):
        for field in ("input_payload_roots", "publication_payload_roots", "accepted_time_s_bits", "dt_s_bits"):
            bad = copy.deepcopy(payload)
            value = bad["new_trace"][field][step]
            bad["new_trace"][field][step] = value ^ 1 if isinstance(value, int) else ("0" if value[0] != "0" else "1")+value[1:]
            mutations.append((f"{field}_step_{STEP+step+1}", bad))
    bad = copy.deepcopy(payload)
    for name in ("old_trace", "new_trace"):
        for field in ("accepted_time_s_bits", "dt_s_bits", "input_payload_roots", "publication_payload_roots", "kernel_set_sha256"):
            bad[name][field] = bad[name][field][:7]
    mutations.append(("fewer_than_eight", bad))
    for name in ("old_trace", "new_trace"):
        bad = copy.deepcopy(payload)
        bad[name]["executable_sha256"] = "0"*64
        mutations.append((name+"_wrong_binary", bad))
    bad = copy.deepcopy(payload)
    bad["resumed_from_step"] = 825
    mutations.append(("wrong_anchor", bad))
    # Defeat raw consistency too: altered scratch bytes and their trace/hash
    # agree internally. Only the cross-execution comparator can refuse these.
    names, original = rows(Path(payload["new_trace"]["csv_path"]).read_bytes())
    for column, field in (("input_payload_root_sha256", "input_payload_roots"),
                          ("publication_payload_root_sha256", "publication_payload_roots"),
                          ("qualified_kernel_set_sha256", "kernel_set_sha256"),
                          ("time_s", "accepted_time_s_bits")):
        changed, bad = copy.deepcopy(original), copy.deepcopy(payload)
        if column == "time_s":
            previous = BEGIN
            for index, row in enumerate(changed):
                dt = float(row["dt_s"])
                if index == 4:
                    dt += math.ulp(float(row["time_s"]))
                previous += dt
                row["dt_s"], row["time_s"] = repr(dt), repr(previous)
                bad["new_trace"]["dt_s_bits"][index] = bits(dt)
                bad["new_trace"]["accepted_time_s_bits"][index] = bits(previous)
        else:
            value = changed[4][column]
            changed[4][column] = ("0" if value[0] != "0" else "1")+value[1:]
            bad["new_trace"][field][4] = changed[4][column]
        name = "coherent_raw_"+column
        csv_path = output / (name+".csv")
        with csv_path.open("x", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=names, lineterminator="\n")
            writer.writeheader()
            writer.writerows(changed)
        bad["new_trace"]["csv_path"], bad["new_trace"]["csv_sha256"] = str(csv_path), sha(csv_path)
        actual = trace_from_executed_csv(csv_path, Path(bad["new_trace"]["log_path"]),
                                         Path(bad["new_trace"]["executable_path"]),
                                         bad["new_trace"]["build_id"],
                                         dict(beginning_time_s_bits=bits(BEGIN), resumed_from_step=STEP))
        if actual != bad["new_trace"]:
            raise ValueError("coherent native RED has inconsistent raw evidence: "+name)
        mutations.append((name, bad))
    for name, bad in mutations:
        path = output / (name+".cbor")
        path.write_bytes(envelope(bad))
        result = subprocess.run([str(executable), "--fire-resident-resume-validate", str(path)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode != 98 or "RESIDENT_RESUME_VALIDATE passed=0" not in result.stdout:
            raise ValueError("native certificate RED did not refuse: "+name+" "+result.stdout)
        results.append(dict(name=name, exit_code=result.returncode, output=result.stdout, sha256=sha(path)))
    return results


def certify(old_exe, new_exe, checkpoint, historical_root, output):
    from fire_r221_qualified_build import QUALIFIED_EXECUTABLE_SHA256, QUALIFIED_BUILD_ID
    old_exe, new_exe, checkpoint, historical_root, output = (
        p.resolve() for p in (old_exe, new_exe, checkpoint, historical_root, output))
    if output.exists() or sha(old_exe) != OLD_EXE or sha(checkpoint) != CHECKPOINT:
        raise ValueError("existing output or wrong historical binary/checkpoint")
    if any(name.startswith("RISE_FIRE_") for name in os.environ):
        raise ValueError("fire diagnostic/mutation environment must be empty")
    for relative, expected in PINS.items():
        if sha(historical_root / relative) != expected:
            raise ValueError("historical authority SHA differs: "+str(relative))
    old_identity = text_metadata((historical_root / RUN / "from_zero_identity.v1").read_bytes())
    for key, expected in dict(producer_build_id=OLD_BUILD, producer_executable_sha256=OLD_EXE,
                             case_record_id=CASE, resolution_tier="10", seed="1234", resume_forbidden="true",
                             protocol_sha256=PINS[PROTOCOL]).items():
        if old_identity.get(key) != expected:
            raise ValueError("historical identity differs: "+key)
    excerpt = historical_excerpt((historical_root / RUN / "budgets/maximum_velocity_trajectory.csv").read_bytes())
    if sha(new_exe) != QUALIFIED_EXECUTABLE_SHA256:
        raise ValueError("executable differs from independent qualification pin")
    inspect_raw = subprocess.check_output([str(new_exe), "--fire-resident-resume-inspect", str(checkpoint)])
    identity = json.loads(inspect_raw, object_pairs_hook=unique_pairs)
    validate_identity(identity, QUALIFIED_EXECUTABLE_SHA256, QUALIFIED_BUILD_ID)
    output.mkdir(parents=True)
    excerpt_path = output / "historical_rows_827_835.csv"
    excerpt_path.write_bytes(excerpt)
    (output / "new_inspection.json").write_bytes(inspect_raw)
    copied = output / "probe_input.checkpoint"
    shutil.copyfile(checkpoint, copied)
    sidecar = Path(str(checkpoint)+".payload-v2.json")
    copied_sidecar = Path(str(copied)+".payload-v2.json")
    shutil.copyfile(sidecar, copied_sidecar)
    sidecar_sha = sha(sidecar)
    run_dir, log = output / "new", output / "new.log"
    command = [str(new_exe), "--fire-r221-resume-probe", str(copied), str(run_dir)]
    print("Executing nine-step 826 -> 835 probe; migration pending", flush=True)
    with log.open("xb") as stream:
        status = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT).returncode
    lines = log.read_text().splitlines()
    expected_end = "OWNER_CERTIFICATE_DIAGNOSTIC_END solver_accepted=1 checkpoint_unchanged=1 migration_authority=false error="
    expected_execution = "RESIDENT_RESUME_EXECUTION build_id="+QUALIFIED_BUILD_ID+" executable_sha256="+QUALIFIED_EXECUTABLE_SHA256
    if (status != 0 or [v for v in lines if v.startswith("OWNER_CERTIFICATE_DIAGNOSTIC_END ")] != [expected_end]
            or [v for v in lines if v.startswith("RESIDENT_RESUME_EXECUTION ")] != [expected_execution]
            or sha(new_exe) != QUALIFIED_EXECUTABLE_SHA256 or sha(old_exe) != OLD_EXE
            or sha(checkpoint) != CHECKPOINT or sha(copied) != CHECKPOINT
            or sha(sidecar) != sidecar_sha or sha(copied_sidecar) != sidecar_sha):
        raise ValueError("probe incomplete or immutable input changed")
    old = trace_from_executed_csv(excerpt_path, historical_root / Path(str(RUN)+".log"), old_exe, OLD_BUILD, identity)
    old["csv_path"] = str(historical_root / RUN / "budgets/maximum_velocity_trajectory.csv")
    old["csv_sha256"] = PINS[RUN / "budgets/maximum_velocity_trajectory.csv"]
    new = trace_from_executed_csv(run_dir / "budgets/maximum_velocity_trajectory.csv", log, new_exe, QUALIFIED_BUILD_ID, identity)
    validate_trace_pair(old, new, identity)
    payload = dict(record_kind="fire-resume-equivalence-certificate-v2", schema_version=2,
                   checkpoint_sha256=CHECKPOINT, resumed_from_step=STEP, beginning_time_s_bits=bits(BEGIN),
                   old_build_id=OLD_BUILD, new_build_id=QUALIFIED_BUILD_ID, old_executable_sha256=OLD_EXE,
                   new_executable_sha256=QUALIFIED_EXECUTABLE_SHA256, old_trace=old, new_trace=new)
    pending = output / "resume_equivalence.v2.pending.cbor"
    pending.write_bytes(envelope(payload))
    subprocess.run([str(new_exe), "--fire-resident-resume-validate", str(pending)], check=True)
    reds_dir = output / "native_reds"
    reds_dir.mkdir()
    reds = native_reds(new_exe, payload, reds_dir)
    subprocess.run([str(new_exe), "--fire-r221-native-overlap-red", str(pending),
                    str(run_dir / "budgets/maximum_velocity_trajectory.csv")], check=True)
    for relative, expected in PINS.items():
        if sha(historical_root / relative) != expected:
            raise ValueError("historical authority changed during probe")
    if sha(new_exe) != QUALIFIED_EXECUTABLE_SHA256 or sha(checkpoint) != CHECKPOINT:
        raise ValueError("qualified inputs changed")
    certificate = output / "resume_equivalence.v2.cbor"
    pending.rename(certificate)
    bridge = dict(schema="rise.fire.r221.historical-resident-bridge.v2", historical_verifiers="v1/v2 retained unchanged",
                  historical_authority={str(k): v for k, v in PINS.items()}, historical_root=str(historical_root),
                  old_execution="historical_transcript_not_reexecuted", selected_old_accepted_steps=list(range(827, 836)),
                  checkpoint_sha256=CHECKPOINT, checkpoint_sidecar_sha256=sidecar_sha,
                  certificate_sha256=sha(certificate), payload=payload, native_reds=reds,
                  execution=dict(command=command, exit_code=status, identity=identity, trace=new),
                  equivalence="N=9 exact time/dt/input-root/publication-root/kernel equality; no onset verdict")
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
