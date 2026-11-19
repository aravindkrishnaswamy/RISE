#!/usr/bin/env python3
"""Execute the r211 r78 N=8 bridge; never certify caller-authored trace values.

The legacy adapter is pinned to the preserved r208 executable/build pair.
Both runs are launched here, sequentially, from independent checkpoint copies.
CBOR v1 encoding is retained; the certificate's schema is explicitly v2.
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
import struct
import subprocess
import unicodedata

OLD_EXE = "70eca63d46873ab13cee9d97f189d9fbc013e0b1d1831b0b100a3985294b7db9"
OLD_BUILD = "a3ef35639751dbc0c3f21aaca7d2b99f0e18ed477ed7c98ab396e2ab8244e1f5"
CHECKPOINT = "2422002e0d45746989b1fa3676f1f4027c785e91bec6b99c3d88b9b01ef12bb2"


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def bits(number):
    return struct.unpack("<Q", struct.pack("<d", number))[0]


def cbor(value):
    """RISE-CBOR64 restricted subset: unsigned integers, NFC text, arrays, maps."""
    def head(major, n):
        if n < 24:
            return bytes([major * 32 + n])
        for tag, size in ((24, 1), (25, 2), (26, 4), (27, 8)):
            if n < 1 << (8 * size):
                return bytes([major * 32 + tag]) + n.to_bytes(size, "big")
        raise ValueError("CBOR integer overflow")
    if type(value) is int and value >= 0:
        return head(0, value)
    if isinstance(value, str):
        if unicodedata.normalize("NFC", value) != value:
            raise ValueError("non-NFC text")
        encoded = value.encode("utf-8")
        return head(3, len(encoded)) + encoded
    if isinstance(value, list):
        return head(4, len(value)) + b"".join(map(cbor, value))
    if isinstance(value, dict) and all(isinstance(k, str) for k in value):
        # RISE orders map keys by UTF-8 bytes, not CBOR encoded-key length.
        return head(5, len(value)) + b"".join(cbor(k) + cbor(value[k])
                                             for k in sorted(value, key=lambda k: k.encode()))
    raise ValueError("unsupported CBOR value")


def envelope(payload):
    return cbor({"certificate_id": hashlib.sha256(cbor(payload)).hexdigest(), "payload": payload})


def trace_from_executed_csv(csv_path, log, executable, build, identity):
    raw = csv_path.read_bytes()
    reader = csv.DictReader(io.StringIO(raw.decode("utf-8")))
    if len(reader.fieldnames) != len(set(reader.fieldnames)):
        raise ValueError("duplicate CSV header")
    rows = list(reader)
    if len(rows) < 8:
        raise ValueError("fewer than eight accepted steps")
    trace = {"build_id": build, "executable_sha256": sha(executable),
             "executable_path": str(executable), "csv_path": str(csv_path),
             "csv_sha256": hashlib.sha256(raw).hexdigest(),
             "log_path": str(log), "log_sha256": sha(log), "digest_version": 2,
             "accepted_time_s_bits": [], "dt_s_bits": [], "input_payload_roots": [],
             "publication_payload_roots": [], "kernel_set_sha256": []}
    previous = struct.unpack("<d", struct.pack("<Q", identity["beginning_time_s_bits"]))[0]
    for i, row in enumerate(rows):
        time, dt = float(row["time_s"]), float(row["dt_s"])
        if (None in row or not all(math.isfinite(v) for v in (time, dt)) or dt <= 0
                or time <= previous or time != previous + dt
                or row["accepted_step"] != str(identity["resumed_from_step"] + i + 1)
                or row["payload_digest_version"] != "2"
                or row["payload_digest_format"] != "rise-payload-sha256-merkle"):
            raise ValueError("non-contiguous or unqualified resident trace")
        trace["accepted_time_s_bits"].append(bits(time))
        trace["dt_s_bits"].append(bits(dt))
        for key, column in (("input_payload_roots", "input_payload_root_sha256"),
                            ("publication_payload_roots", "publication_payload_root_sha256"),
                            ("kernel_set_sha256", "qualified_kernel_set_sha256")):
            root = row[column]
            if len(root) != 64 or any(c not in "0123456789abcdef" for c in root):
                raise ValueError("invalid full payload root")
            trace[key].append(root)
        previous = time
    return trace


def require_equivalence(old, new):
    for key in ("accepted_time_s_bits", "dt_s_bits", "input_payload_roots",
                "publication_payload_roots", "kernel_set_sha256"):
        if old[key] != new[key]:
            raise ValueError("r78 equal-time equivalence failed: " + key)


def certify(old_exe, new_exe, checkpoint, output):
    old_exe, new_exe, checkpoint, output = (p.resolve() for p in
                                            (old_exe, new_exe, checkpoint, output))
    if output.exists() or sha(old_exe) != OLD_EXE or sha(checkpoint) != CHECKPOINT:
        raise ValueError("existing output or wrong pinned legacy executable/checkpoint")
    if any(name.startswith("RISE_FIRE_") for name in os.environ):
        raise ValueError("fire mutation/diagnostic environment must be empty")
    new_sha = sha(new_exe)
    identity = json.loads(subprocess.check_output(
        [str(new_exe), "--fire-resident-resume-inspect", str(checkpoint)]))
    if (identity["executable_sha256"] != new_sha or identity["checkpoint_sha256"] != CHECKPOINT
            or identity["checkpoint_build_id"] != OLD_BUILD or identity["resumed_from_step"] != 1300
            or identity["build_id"] == OLD_BUILD):
        raise ValueError("executable-reported identity mismatch")
    output.mkdir(parents=True)
    traces, executions = [], []
    for label, executable, build, command, expected_exit in (
            ("old", old_exe, OLD_BUILD, "--fire-production-owner-eos-refusal", 93),
            ("new", new_exe, identity["build_id"], "--fire-production-owner-certificate-diagnostic", 0)):
        copied = output / (label + ".checkpoint")
        shutil.copyfile(checkpoint, copied)
        run_dir, log = output / label, output / (label + ".log")
        before = sha(executable)
        args = [str(executable), command, str(copied), str(run_dir)]
        print("executing " + label + " eight-step owner", flush=True)
        with log.open("xb") as stream:
            status = subprocess.run(args, stdout=stream, stderr=subprocess.STDOUT).returncode
        text = log.read_bytes().decode("utf-8")
        end_tag = "OWNER_EOS_DIAGNOSTIC_END" if label == "old" else "OWNER_CERTIFICATE_DIAGNOSTIC_END"
        ends = [line for line in text.splitlines() if line.startswith(end_tag + " ")]
        if (status != expected_exit or len(ends) != 1
                or "solver_accepted=1 checkpoint_unchanged=1 migration_authority=false error=" not in ends[0]
                or not ends[0].endswith("error=") or sha(executable) != before
                or sha(copied) != CHECKPOINT or sha(checkpoint) != CHECKPOINT):
            raise ValueError("executed continuation failed or changed inputs: " + label)
        trace = trace_from_executed_csv(run_dir / "budgets/maximum_velocity_trajectory.csv",
                                        log, executable, build, identity)
        if trace["executable_sha256"] != before or len(trace["accepted_time_s_bits"]) != 8:
            raise ValueError("execution identity/count mismatch")
        traces.append(trace)
        executions.append({"command": args, "exit_code": status, "executable_before_sha256": before,
                           "executable_after_sha256": sha(executable), "trace": trace})
    require_equivalence(*traces)
    payload = {"record_kind": "fire-resume-equivalence-certificate-v2", "schema_version": 2,
               "checkpoint_sha256": CHECKPOINT, "resumed_from_step": 1300,
               "beginning_time_s_bits": identity["beginning_time_s_bits"],
               "old_build_id": OLD_BUILD, "new_build_id": identity["build_id"],
               "old_executable_sha256": OLD_EXE, "new_executable_sha256": new_sha,
               "old_trace": traces[0], "new_trace": traces[1]}
    certificate = output / "resume_equivalence.v2.cbor"
    certificate.write_bytes(envelope(payload))
    subprocess.run([str(new_exe), "--fire-resident-resume-validate", str(certificate)], check=True)
    bridge = {"schema": "rise.fire.r78.resident-bridge.v2", "historical_v1": "unchanged_verifier_retained",
              "legacy_adapter": "SHA-pinned r208 binary; historical successful EOS diagnostic exits 93",
              "checkpoint_sha256": CHECKPOINT, "certificate_sha256": sha(certificate),
              "equivalence": "N=8 exact accepted-time, input-root and publication-root equality",
              "executions": executions, "payload": payload}
    (output / "bridge.v2.json").write_text(json.dumps(bridge, indent=2, sort_keys=True) + "\n")
    print("CERTIFIED " + str(certificate) + " sha256=" + sha(certificate), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("old_executable", "new_executable", "checkpoint", "output"):
        parser.add_argument(arg, type=Path)
    args = parser.parse_args()
    certify(args.old_executable, args.new_executable, args.checkpoint, args.output)
