#!/usr/bin/env python3
"""Join immutable r214 steps 1–792 to certified r216 continuation steps 793+.

This is evidence processing, never a solver or a from-zero identity issuer.
Prefix rows 793–805 remain in the original artifact, outside the joined claim.
No survival verdict is issued without a matching final accepted-step history.
"""
import argparse
import csv
import hashlib
import io
import json
import math
import os
from pathlib import Path
import struct
import subprocess

from fire_resident_resume_v2 import envelope
from fire_payload_merkle import verify
from fire_r216_qualified_build import QUALIFIED_EXECUTABLE_SHA256, QUALIFIED_BUILD_ID

PREFIX_SHA = "3ec4f50479a59ca90c7610290ad110279e7356f6e0907c470a6fa753a4545b50"
IDENTITY_SHA = "96558b780535ecadc917160084b497ffe413a77fd767b5ec7cb747b90f7f7998"
PROTOCOL_SHA = "983cfb4975a614a7bee2593c6d0f4536679925911afd0b22a5b2e897d6b04dc2"
CONTINUATION_PROTOCOL_SHA = "30196fdcfa1a17911e1200cf9a3eff1b805dff236c6529301054479f2ce2a28e"
CHECKPOINT_SHA = "65bb74aa8186a95acde57de593d54de664303c4d92ebaea374eea75e1f138c19"
KERNEL_SHA = "d580d078ea083ee18032abfb027271ada9d4d1dabab50b35cb6eb7a503258a0e"
OLD_EXE_SHA = "45370ad3bcc7035fb228a1f839efe5059cbcc791a64486bd2a1f0b1c3025dc2c"
OLD_BUILD = "0d7537194b877cdffd248de25eccf52262f824b4f1d4cde5fe035ad1eee52175"
PREFIX_STEPS = 792


def require(condition, clause):
    if not condition:
        raise ValueError(clause)


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def read_pinned(path, expected, clause):
    raw = Path(path).read_bytes()
    require(digest(raw) == expected, clause)
    return raw


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate identity/JSON key: " + key)
        result[key] = value
    return result


def identity(raw):
    pairs = []
    for line in raw.decode("utf-8").splitlines():
        require(bool(line) and " " in line, "invalid identity line")
        pairs.append(line.split(" ", 1))
    return unique_pairs(pairs)


def number(token):
    value = float(token)
    require(math.isfinite(value), "nonfinite trajectory quantity")
    return value


def bits(value):
    return struct.unpack("<Q", struct.pack("<d", value))[0]


def sha_text(value):
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)


def rows(raw):
    require(raw.endswith(b"\n") and b"\r" not in raw, "incomplete or noncanonical CSV framing")
    reader = csv.DictReader(io.StringIO(raw.decode("ascii")))
    require(reader.fieldnames and len(reader.fieldnames) == len(set(reader.fieldnames)), "duplicate CSV header")
    result = list(reader)
    require(result and len(raw.splitlines()) == len(result) + 1, "empty or multiline CSV")
    require(all(None not in row and None not in row.values() for row in result), "CSV row extent")
    return reader.fieldnames, result


def validate_rows(data, first, previous):
    for index, row in enumerate(data, first):
        time, dt = number(row["time_s"]), number(row["dt_s"])
        require(row["accepted_step"] == str(index), "duplicate, missing or out-of-order accepted step")
        require(0 < dt <= 0.0016462659696117043 and time > previous and time == previous + dt,
                "non-exact accepted-time join or source-step ceiling")
        require(number(row["maximum_velocity_m_per_s"]) >= 0, "negative velocity magnitude")
        require(row["qualified_kernel_set_sha256"] == KERNEL_SHA, "qualified kernel change")
        require(row["payload_digest_version"] == "2" and
                row["payload_digest_format"] == "rise-payload-sha256-merkle", "payload digest class")
        require(all(sha_text(row[key]) for key in
                    ("input_payload_root_sha256", "publication_payload_root_sha256")), "invalid payload root")
        previous = time


def compare_trace(trace, data):
    columns = {"accepted_time_s_bits": lambda row: bits(number(row["time_s"])),
               "dt_s_bits": lambda row: bits(number(row["dt_s"])),
               "input_payload_roots": lambda row: row["input_payload_root_sha256"],
               "publication_payload_roots": lambda row: row["publication_payload_root_sha256"],
               "kernel_set_sha256": lambda row: row["qualified_kernel_set_sha256"]}
    for key, extract in columns.items():
        require(trace[key] == [extract(row) for row in data], "certificate executed trace mismatch: " + key)


def validate_history(raw, joined):
    header, data = rows(raw)
    require(header == ["accepted_step", "accepted_time_s", "delta_t_s"], "accepted history header")
    require(len(data) == len(joined), "accepted history extent")
    for expected, observed in zip(joined, data):
        require(observed["accepted_step"] == expected["accepted_step"] and
                bits(number(observed["accepted_time_s"])) == bits(number(expected["time_s"])) and
                bits(number(observed["delta_t_s"])) == bits(number(expected["dt_s"])), "accepted history mismatch")


def verify_published(path, raw, case_id):
    sidecar_raw = Path(str(path) + ".payload-v2.json").read_bytes()
    sidecar = json.loads(sidecar_raw, object_pairs_hook=unique_pairs)
    require(set(sidecar) == {"schema", "case_record_id", "sha256_v1", "v2"} and
            sidecar["schema"] == "rise.fire.published-payload.v2" and sidecar["case_record_id"] == case_id and
            sidecar["sha256_v1"] == digest(raw) and verify(raw, sidecar["v2"]), "terminal published payload seal")
    return digest(sidecar_raw)


def native_validate(executable, arguments, bindings, marker):
    """Run only the certificate's actual executable, never a caller verifier.

    Every consumed file and the executable are SHA checked before and after
    execution. A successful process without the exact native success line is
    not validation. These receipts record execution, not a caller assertion.
    """
    require(not any(key.startswith("RISE_FIRE_") for key in os.environ),
            "native validation refuses fire mutation/diagnostic environment")
    require(sha_text(QUALIFIED_EXECUTABLE_SHA256) and sha_text(QUALIFIED_BUILD_ID),
            "independent qualified build pins are unavailable")
    executable = Path(executable).resolve(strict=True)
    expected = {Path(path).resolve(strict=True): sha for path, sha in bindings.items()}
    require(executable not in expected, "native executable aliases validation input")
    expected[executable] = QUALIFIED_EXECUTABLE_SHA256
    for path, sha in expected.items():
        read_pinned(path, sha, "native validation input/executable changed before execution")
    command = [str(executable), *arguments]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    for path, sha in expected.items():
        read_pinned(path, sha, "native validation input/executable changed during execution")
    require(result.returncode == 0 and result.stdout.strip() == marker.encode("ascii"),
            "native validation refused or success marker missing: " + arguments[0])
    return {"command": command, "exit_code": result.returncode,
            "stdout_sha256": digest(result.stdout), "stderr_sha256": digest(result.stderr),
            "executable_sha256": QUALIFIED_EXECUTABLE_SHA256,
            "input_sha256": {str(path): sha for path, sha in expected.items()}}


def join(*, original_csv, original_identity, original_protocol, checkpoint,
         continuation_csv, continuation_identity, continuation_protocol,
         certificate, bridge, new_executable, accepted_history=None,
         final_summary=None, final_checkpoint=None, diagnostic_only=False):
    # Internal synthetic fixture mode is deliberately absent from the CLI.
    # It can exercise structural joins but can never emit a survival verdict.
    require(type(diagnostic_only) is bool, "diagnostic mode must be explicit boolean")
    original = read_pinned(original_csv, PREFIX_SHA, "original trajectory SHA changed")
    old_identity_raw = read_pinned(original_identity, IDENTITY_SHA, "original from-zero identity changed")
    read_pinned(original_protocol, PROTOCOL_SHA, "original protocol changed")
    read_pinned(continuation_protocol, CONTINUATION_PROTOCOL_SHA, "continuation protocol changed")
    read_pinned(checkpoint, CHECKPOINT_SHA, "checkpoint changed")
    old_id = identity(old_identity_raw)
    header, old = rows(original)
    require(len(old) == 805, "original trajectory extent changed")
    validate_rows(old, 1, 0.0)
    require(old_id["producer_executable_sha256"] == OLD_EXE_SHA and
            old_id["producer_build_id"] == OLD_BUILD and old_id["resume_forbidden"] == "true" and
            old_id["protocol_sha256"] == PROTOCOL_SHA, "original identity authority")

    certificate_raw = Path(certificate).read_bytes()
    bridge_raw = Path(bridge).read_bytes()
    bridge_record = json.loads(bridge_raw, object_pairs_hook=unique_pairs)
    payload = bridge_record["payload"]
    require(bridge_record["schema"] == "rise.fire.r216.historical-resident-bridge.v2" and
            bridge_record["certificate_sha256"] == digest(certificate_raw) and
            certificate_raw == envelope(payload), "certificate/bridge canonical binding")
    require(payload["record_kind"] == "fire-resume-equivalence-certificate-v2" and
            payload["schema_version"] == 2 and payload["checkpoint_sha256"] == CHECKPOINT_SHA and
            payload["resumed_from_step"] == PREFIX_STEPS and
            payload["beginning_time_s_bits"] == bits(number(old[PREFIX_STEPS-1]["time_s"])), "certificate checkpoint/time authority")
    require(payload["old_executable_sha256"] == OLD_EXE_SHA and payload["old_build_id"] == OLD_BUILD and
            payload["new_executable_sha256"] != OLD_EXE_SHA and payload["new_build_id"] != OLD_BUILD,
            "certificate build authority")
    if not diagnostic_only:
        require(sha_text(QUALIFIED_EXECUTABLE_SHA256) and sha_text(QUALIFIED_BUILD_ID) and
                payload["new_executable_sha256"] == QUALIFIED_EXECUTABLE_SHA256 and
                payload["new_build_id"] == QUALIFIED_BUILD_ID,
                "certificate new executable/build is not independently qualified")
    read_pinned(new_executable, payload["new_executable_sha256"], "new executable changed")
    for role in ("old", "new"):
        trace = payload[role + "_trace"]
        require(trace["build_id"] == payload[role + "_build_id"] and
                trace["executable_sha256"] == payload[role + "_executable_sha256"] and
                trace["digest_version"] == 2, "trace executable authority")
        require(len(trace["accepted_time_s_bits"]) == 8, "migration requires exactly eight certified steps")
        compare_trace(trace, old[PREFIX_STEPS:PREFIX_STEPS+8])
    require(payload["old_trace"]["csv_sha256"] == PREFIX_SHA, "certificate original transcript binding")
    new_trace = payload["new_trace"]
    new_trace_raw = read_pinned(new_trace["csv_path"], new_trace["csv_sha256"], "executed certificate CSV changed")
    read_pinned(new_trace["log_path"], new_trace["log_sha256"], "executed certificate log changed")
    trace_header, trace_rows = rows(new_trace_raw)
    require(trace_header == header, "executed certificate CSV header")
    compare_trace(new_trace, trace_rows)

    continuation_raw = Path(continuation_csv).read_bytes()
    next_header, continued = rows(continuation_raw)
    require(next_header == header, "continuation header differs")
    validate_rows(continued, PREFIX_STEPS + 1, number(old[PREFIX_STEPS-1]["time_s"]))
    for index, row in enumerate(continued[:8]):
        # A fresh continuation from the certified checkpoint must reproduce
        # the certificate prefix, not merely point to a genuine certificate.
        compare_trace({key: values[index:index+1] for key, values in new_trace.items()
                       if key in ("accepted_time_s_bits", "dt_s_bits", "input_payload_roots",
                                  "publication_payload_roots", "kernel_set_sha256")}, [row])
    next_id_raw = Path(continuation_identity).read_bytes()
    next_id = identity(next_id_raw)
    require(next_id["schema"] == "rise.fire.production.r216_continuation_identity.v2" and
            next_id["resume_forbidden"] == "false" and "initial_state_sha256" not in next_id and
            next_id.get("from_zero", "false") == "false", "continuation falsely claims from-zero authority")
    for key in ("operator", "resolution_tier", "seed", "case_record_id"):
        require(next_id[key] == old_id[key], "continuation identity mismatch: " + key)
    require(next_id["checkpoint_sha256"] == CHECKPOINT_SHA and
            next_id["migration_certificate_sha256"] == digest(certificate_raw) and
            next_id["protocol_sha256"] == CONTINUATION_PROTOCOL_SHA and
            next_id["producer_build_id"] == payload["new_build_id"] and
            next_id["producer_executable_sha256"] == payload["new_executable_sha256"], "continuation seal lineage")
    require(sha_text(next_id["construction_seed_state_sha256"]), "missing construction seed diagnostic")
    native_receipts = {}
    if not diagnostic_only:
        native_receipts["migration_certificate"] = native_validate(new_executable,
            ["--fire-resident-resume-validate", str(Path(certificate).resolve())],
            {certificate: digest(certificate_raw)},
            "RESIDENT_RESUME_VALIDATE passed=1 version=2 steps=8 error=")

    prefix = old[:PREFIX_STEPS]
    joined = prefix + continued
    crossed = any(number(row["maximum_velocity_m_per_s"]) >= 15.0 for row in joined)
    crossing_rows = [row for row in continued if number(row["maximum_velocity_m_per_s"]) >= 15.0]
    require(not crossing_rows or crossing_rows[0] is continued[-1], "continued beyond first crossing")
    require(all(number(row["time_s"]) < 3.5 for row in continued[:-1]), "continued beyond horizon")
    history_sha = None
    summary_sha = None
    final_checkpoint_sha = None
    terminal_sidecar_shas = {}
    reached = number(joined[-1]["time_s"]) >= 3.5
    if accepted_history is not None:
        require(final_summary is not None and final_checkpoint is not None,
                "accepted history requires native final summary and checkpoint")
        history_raw = Path(accepted_history).read_bytes()
        validate_history(history_raw, joined)
        history_sha = digest(history_raw)
        final_raw = Path(final_summary).read_bytes()
        final = identity(final_raw)
        require(final["schema"] == "rise.fire.production.onset_campaign.summary.v2" and
                final["accepted_timestep_history_sha256"] == history_sha and
                final["trajectory_sha256"] == digest(continuation_raw) and
                final["continuation_identity_sha256"] == digest(next_id_raw) and
                final["from_zero_identity_sha256"] == "not_reissued_continuation" and
                final["producer_build_id"] == payload["new_build_id"] and
                final["producer_executable_sha256"] == payload["new_executable_sha256"] and
                final["protocol_sha256"] == CONTINUATION_PROTOCOL_SHA,
                "native final summary lineage or history SHA")
        require(final["accepted_steps"] == str(len(joined)) and
                bits(number(final["simulated_time_s"])) == bits(number(joined[-1]["time_s"])) and
                number(final["target_time_s"]) == 3.5 and
                final["trajectory_complete_against_accepted_schedule"] == "true" and
                final["accepted_timestep_history_matches_final_checkpoint"] == "true" and
                final["crossed_stop_threshold"] == str(int(crossed)) and
                final["onset_survival_passed"] == str(int(reached and not crossed)),
                "native final summary checkpoint-history or verdict mismatch")
        final_checkpoint_sha = final["final_checkpoint_sha256"]
        checkpoint_raw = read_pinned(final_checkpoint, final_checkpoint_sha, "final checkpoint changed")
        summary_sha = digest(final_raw)
        for label, path, raw in (("accepted_history", accepted_history, history_raw),
                                 ("final_summary", final_summary, final_raw),
                                 ("final_checkpoint", final_checkpoint, checkpoint_raw),
                                 ("continuation_csv", continuation_csv, continuation_raw),
                                 ("continuation_identity", continuation_identity, next_id_raw)):
            terminal_sidecar_shas[label] = verify_published(path, raw, old_id["case_record_id"])
        if not diagnostic_only:
            native_receipts["final_checkpoint_history"] = native_validate(new_executable,
                ["--fire-r216-final-validate", str(Path(final_checkpoint).resolve()),
                 str(Path(accepted_history).resolve()), str(Path(continuation_csv).resolve()),
                 str(Path(certificate).resolve())],
                {final_checkpoint: final_checkpoint_sha, accepted_history: history_sha,
                 continuation_csv: digest(continuation_raw), certificate: digest(certificate_raw)},
                "R216_FINAL_VALIDATE passed=1")
    else:
        require(final_summary is None and final_checkpoint is None, "terminal artifacts require accepted history")
    joined_raw = b"".join(original.splitlines(keepends=True)[:PREFIX_STEPS+1]) + b"".join(continuation_raw.splitlines(keepends=True)[1:])
    terminal_validated = not diagnostic_only and "final_checkpoint_history" in native_receipts
    report = {"schema": "rise.fire.r216.joined-onset-evidence.v1",
              "authority": "synthetic_diagnostic_not_authoritative" if diagnostic_only else
                  ("native_validated" if terminal_validated else "migration_validated_prefix_unverified"),
              "native_validation_receipts": native_receipts,
              "origin": "original_from_zero_prefix_plus_certified_continuation_not_new_from_zero",
              "original_trajectory_sha256": digest(original), "original_identity_sha256": digest(old_identity_raw),
              "original_protocol_sha256": PROTOCOL_SHA, "continuation_protocol_sha256": CONTINUATION_PROTOCOL_SHA,
              "checkpoint_sha256": CHECKPOINT_SHA, "certificate_sha256": digest(certificate_raw),
              "bridge_sha256": digest(bridge_raw), "continuation_identity_sha256": digest(next_id_raw),
              "continuation_trajectory_sha256": digest(continuation_raw), "joined_trajectory_sha256": digest(joined_raw),
              "preserved_prefix_steps": [1, PREFIX_STEPS], "excluded_original_steps": [793, 805],
              "continuation_first_step": 793, "accepted_steps": len(joined),
              "qualified_kernel_set_sha256": KERNEL_SHA, "accepted_history_sha256": history_sha,
              "final_summary_sha256": summary_sha, "final_checkpoint_sha256": final_checkpoint_sha,
              "terminal_payload_sidecar_sha256": terminal_sidecar_shas,
              "final_time_s": number(joined[-1]["time_s"]),
              "maximum_velocity_m_per_s": max(number(row["maximum_velocity_m_per_s"]) for row in joined),
              "crossed_stop_threshold": crossed if terminal_validated else None,
              "horizon_reached": reached if terminal_validated else None,
              "observed_unverified_crossing": crossed if not terminal_validated else None,
              "observed_unverified_horizon_reached": reached if not terminal_validated else None,
              "trajectory_survival_conditions": reached and not crossed and history_sha is not None,
              "onset_survival_passed": reached and not crossed and terminal_validated,
              "status": "synthetic_diagnostic" if diagnostic_only else
                  ("non_verdict_prefix" if not terminal_validated else
                   ("crossing" if crossed else ("horizon_verified" if reached else "non_verdict_prefix")))}
    return joined_raw, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("original_csv", "original_identity", "original_protocol", "checkpoint", "continuation_csv",
                 "continuation_identity", "continuation_protocol", "certificate", "bridge", "new_executable"):
        parser.add_argument("--" + name.replace("_", "-"), required=True, type=Path)
    parser.add_argument("--accepted-history", type=Path)
    parser.add_argument("--final-summary", type=Path)
    parser.add_argument("--final-checkpoint", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = vars(parser.parse_args())
    output = args.pop("output")
    try:
        require(not output.exists(), "output already exists")
        raw, report = join(**args)
        output.mkdir(parents=False)
        with (output / "maximum_velocity_trajectory.csv").open("xb") as stream:
            stream.write(raw)
        with (output / "joined_evidence.v1.json").open("x") as stream:
            json.dump(report, stream, sort_keys=True, indent=2, allow_nan=False)
            stream.write("\n")
        print(json.dumps(report, sort_keys=True, allow_nan=False))
    except (ValueError, KeyError, TypeError, OSError, csv.Error) as error:
        parser.exit(2, "r216 join refused: " + str(error) + "\n")


if __name__ == "__main__":
    main()
