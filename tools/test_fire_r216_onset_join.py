#!/usr/bin/env python3
"""Exact synthetic r216 lineage/join REDs. These are not production evidence."""
import copy
import csv
import io
import json
from pathlib import Path
import tempfile
import subprocess
import unittest
from unittest.mock import patch

import fire_r216_onset_join as target
from fire_resident_resume_v2 import envelope
from fire_payload_merkle import merkle

HEADER = ["accepted_step", "time_s", "dt_s", "maximum_velocity_m_per_s",
          "qualified_kernel_set_sha256", "payload_digest_version", "payload_digest_format",
          "input_payload_root_sha256", "publication_payload_root_sha256"]


def serialize(data):
    stream = io.StringIO()
    writer = csv.DictWriter(stream, HEADER, lineterminator="\n")
    writer.writeheader()
    writer.writerows(data)
    return stream.getvalue().encode()


class JoinTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="rise-r216-join-synthetic-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.data = []
        time = 0.0
        for step in range(1, 2201):
            time += 0.0016
            self.data.append(dict(zip(HEADER, [str(step), repr(time), "0.0016", "5",
                target.KERNEL_SHA, "2", "rise-payload-sha256-merkle",
                target.digest(f"input{step}".encode()), target.digest(f"output{step}".encode())])))
        self.old_id = {"schema": "rise.fire.production.r193_from_zero_identity.v1",
                       "operator": "section_3_7_projected_heun_resident_owner", "resolution_tier": "10",
                       "seed": "1234", "case_record_id": "c"*64, "initial_state_sha256": "i"*64,
                       "producer_build_id": target.OLD_BUILD, "producer_executable_sha256": target.OLD_EXE_SHA,
                       "protocol_sha256": target.digest(b"original protocol synthetic"), "resume_forbidden": "true"}
        self.args = {name: self.directory / name for name in
                     ("original_csv", "original_identity", "original_protocol", "checkpoint",
                      "continuation_csv", "continuation_identity", "continuation_protocol",
                      "certificate", "bridge", "new_executable")}
        self.args["diagnostic_only"] = True
        self.args["original_csv"].write_bytes(serialize(self.data[:805]))
        self.args["original_identity"].write_text(self.id_text(self.old_id))
        self.args["original_protocol"].write_bytes(b"original protocol synthetic")
        self.args["continuation_protocol"].write_bytes(b"continuation protocol synthetic")
        self.args["checkpoint"].write_bytes(b"checkpoint synthetic")
        self.args["new_executable"].write_bytes(b"new executable synthetic")
        pins = {name: target.digest(self.args[path].read_bytes()) for name, path in
                (("PREFIX_SHA", "original_csv"), ("IDENTITY_SHA", "original_identity"),
                 ("PROTOCOL_SHA", "original_protocol"), ("CONTINUATION_PROTOCOL_SHA", "continuation_protocol"),
                 ("CHECKPOINT_SHA", "checkpoint"))}
        # Independent pins are synthetic only in this isolated fixture. The
        # CLI has no pin override and loads the approved frozen build module.
        pins.update(QUALIFIED_EXECUTABLE_SHA256=target.digest(self.args["new_executable"].read_bytes()),
                    QUALIFIED_BUILD_ID="b"*64)
        self.patches = patch.multiple(target, **pins)
        self.patches.start()
        self.addCleanup(self.patches.stop)
        new_csv = self.directory / "executed_trace.csv"
        new_csv.write_bytes(serialize(self.data[792:800]))
        log = self.directory / "executed.log"
        log.write_bytes(b"synthetic executed qualification trace")
        trace = {"digest_version": 2, "build_id": "b"*64,
                 "executable_sha256": target.digest(self.args["new_executable"].read_bytes()),
                 "csv_path": str(new_csv), "csv_sha256": target.digest(new_csv.read_bytes()),
                 "log_path": str(log), "log_sha256": target.digest(log.read_bytes()),
                 "accepted_time_s_bits": [target.bits(float(row["time_s"])) for row in self.data[792:800]],
                 "dt_s_bits": [target.bits(float(row["dt_s"])) for row in self.data[792:800]],
                 "input_payload_roots": [row["input_payload_root_sha256"] for row in self.data[792:800]],
                 "publication_payload_roots": [row["publication_payload_root_sha256"] for row in self.data[792:800]],
                 "kernel_set_sha256": [target.KERNEL_SHA]*8}
        old_trace = copy.deepcopy(trace)
        old_trace.update(build_id=target.OLD_BUILD, executable_sha256=target.OLD_EXE_SHA,
                         csv_path=str(self.args["original_csv"]), csv_sha256=target.PREFIX_SHA)
        self.payload = {"record_kind": "fire-resume-equivalence-certificate-v2", "schema_version": 2,
                        "checkpoint_sha256": target.CHECKPOINT_SHA, "resumed_from_step": 792,
                        "beginning_time_s_bits": target.bits(float(self.data[791]["time_s"])),
                        "old_build_id": target.OLD_BUILD, "new_build_id": "b"*64,
                        "old_executable_sha256": target.OLD_EXE_SHA,
                        "new_executable_sha256": trace["executable_sha256"],
                        "old_trace": old_trace, "new_trace": trace}
        self.next_id = {**self.old_id, "schema": "rise.fire.production.r216_continuation_identity.v2",
                        "producer_build_id": "b"*64, "producer_executable_sha256": trace["executable_sha256"],
                        "checkpoint_sha256": target.CHECKPOINT_SHA, "resume_forbidden": "false",
                        "protocol_sha256": target.CONTINUATION_PROTOCOL_SHA,
                        "construction_seed_state_sha256": "c"*64}
        del self.next_id["initial_state_sha256"]
        self.reseal()
        self.continued = copy.deepcopy(self.data[792:805])
        self.write_continuation()

    @staticmethod
    def id_text(values):
        return "".join(f"{key} {value}\n" for key, value in values.items())

    def reseal(self):
        raw = envelope(self.payload)
        self.args["certificate"].write_bytes(raw)
        self.args["bridge"].write_text(json.dumps({"schema": "rise.fire.r216.historical-resident-bridge.v2",
                                                  "payload": self.payload, "certificate_sha256": target.digest(raw)}))
        self.next_id["migration_certificate_sha256"] = target.digest(raw)
        self.args["continuation_identity"].write_text(self.id_text(self.next_id))

    def write_continuation(self):
        self.args["continuation_csv"].write_bytes(serialize(self.continued))

    def refused(self, pattern):
        with self.assertRaisesRegex(ValueError, pattern):
            target.join(**self.args)

    def test_exact_join_preserves_prefix_and_excludes_old_tail(self):
        # Change only a NEW diagnostic beyond the eight equivalence rows;
        # the immutable original 801–805 bytes must not sneak into the join.
        self.continued[8]["maximum_velocity_m_per_s"] = "6"
        self.write_continuation()
        before = self.args["original_csv"].read_bytes()
        raw, report = target.join(**self.args)
        self.assertEqual(raw, serialize(self.data[:792] + self.continued))
        self.assertEqual(self.args["original_csv"].read_bytes(), before)
        self.assertEqual(report["excluded_original_steps"], [793, 805])
        self.assertEqual(report["maximum_velocity_m_per_s"], 6)
        self.assertFalse(report["onset_survival_passed"])
        self.assertEqual(report["authority"], "synthetic_diagnostic_not_authoritative")

    def test_original_mutation(self):
        self.args["original_csv"].write_bytes(self.args["original_csv"].read_bytes().replace(b",5,", b",6,", 1))
        self.refused("original trajectory SHA")

    def test_duplicate_missing_wrong_step(self):
        for value in ("792", "794", "0793"):
            with self.subTest(value=value):
                self.continued[0]["accepted_step"] = value
                self.write_continuation()
                self.refused("accepted step")

    def test_wrong_join_time_one_bit(self):
        self.continued[0]["time_s"] = repr(__import__("math").nextafter(float(self.continued[0]["time_s"]), float("inf")))
        self.write_continuation()
        self.refused("accepted-time")

    def test_wrong_kernel(self):
        self.continued[-1]["qualified_kernel_set_sha256"] = "f"*64
        self.write_continuation()
        self.refused("kernel change")

    def test_falsely_from_zero(self):
        self.next_id["initial_state_sha256"] = "a"*64
        self.reseal()
        self.refused("falsely claims")

    def test_migration_fewer_than_eight(self):
        self.payload["old_trace"]["accepted_time_s_bits"].pop()
        self.reseal()
        self.refused("eight certified")

    def test_root_mismatch_in_certificate(self):
        self.payload["new_trace"]["publication_payload_roots"][4] = "f"*64
        self.reseal()
        self.refused("trace mismatch")

    def test_root_mismatch_in_continuation(self):
        self.continued[0]["publication_payload_root_sha256"] = "f"*64
        self.write_continuation()
        self.refused("trace mismatch")

    def test_forged_bridge_does_not_change_cbor(self):
        bridge = json.loads(self.args["bridge"].read_text())
        bridge["payload"]["resumed_from_step"] = 791
        self.args["bridge"].write_text(json.dumps(bridge))
        self.refused("canonical binding")

    def test_executable_mismatch(self):
        self.args["new_executable"].write_bytes(b"different executable")
        self.refused("executable changed")

    def full_horizon(self):
        self.continued = copy.deepcopy(self.data[792:2188])
        self.write_continuation()
        history = self.directory / "history.csv"
        history.write_text("accepted_step,accepted_time_s,delta_t_s\n" + "".join(
            f"{r['accepted_step']},{r['time_s']},{r['dt_s']}\n" for r in self.data[:2188]))
        self.args["accepted_history"] = history
        self.seal_final()

    def seal_final(self):
        self.args["final_checkpoint"] = self.directory / "final.checkpoint"
        self.args["final_checkpoint"].write_bytes(b"synthetic final checkpoint")
        self.args["final_summary"] = self.directory / "summary.v2"
        crossed = any(float(row["maximum_velocity_m_per_s"]) >= 15 for row in self.continued)
        summary = {"schema": "rise.fire.production.onset_campaign.summary.v2",
                   "accepted_timestep_history_sha256": target.digest(self.args["accepted_history"].read_bytes()),
                   "trajectory_sha256": target.digest(self.args["continuation_csv"].read_bytes()),
                   "continuation_identity_sha256": target.digest(self.args["continuation_identity"].read_bytes()),
                   "from_zero_identity_sha256": "not_reissued_continuation",
                   "producer_build_id": self.payload["new_build_id"],
                   "producer_executable_sha256": self.payload["new_executable_sha256"],
                   "protocol_sha256": target.CONTINUATION_PROTOCOL_SHA,
                   "accepted_steps": str(792+len(self.continued)), "simulated_time_s": self.continued[-1]["time_s"],
                   "target_time_s": "3.5", "trajectory_complete_against_accepted_schedule": "true",
                   "accepted_timestep_history_matches_final_checkpoint": "true",
                   "crossed_stop_threshold": str(int(crossed)), "onset_survival_passed": str(int(not crossed)),
                   "final_checkpoint_sha256": target.digest(self.args["final_checkpoint"].read_bytes())}
        self.args["final_summary"].write_text(self.id_text(summary))
        for key in ("final_checkpoint", "final_summary", "accepted_history", "continuation_csv", "continuation_identity"):
            path = self.args[key]
            raw = path.read_bytes()
            Path(str(path) + ".payload-v2.json").write_text(json.dumps({
                "schema": "rise.fire.published-payload.v2", "case_record_id": self.old_id["case_record_id"],
                "sha256_v1": target.digest(raw), "v2": merkle(raw)}))

    def test_horizon_requires_final_history(self):
        self.full_horizon()
        _, report = target.join(**self.args)
        self.assertTrue(report["trajectory_survival_conditions"])
        self.assertFalse(report["onset_survival_passed"])
        del self.args["accepted_history"]
        del self.args["final_summary"]
        del self.args["final_checkpoint"]
        _, report = target.join(**self.args)
        self.assertFalse(report["onset_survival_passed"])

    def test_final_step_crossing_never_survival(self):
        self.full_horizon()
        self.continued[-1]["maximum_velocity_m_per_s"] = "15"
        self.write_continuation()
        self.seal_final()
        _, report = target.join(**self.args)
        self.assertTrue(report["observed_unverified_horizon_reached"])
        self.assertTrue(report["observed_unverified_crossing"])
        self.assertIsNone(report["crossed_stop_threshold"])
        self.assertFalse(report["onset_survival_passed"])

    def test_continued_after_crossing(self):
        self.continued[-2]["maximum_velocity_m_per_s"] = "15"
        self.write_continuation()
        self.refused("beyond first crossing")

    def test_final_history_mutant(self):
        self.full_horizon()
        self.args["accepted_history"].write_text(self.args["accepted_history"].read_text().replace("0.0016", "0.0017", 1))
        self.refused("history mismatch")

    def test_nonfinite(self):
        self.continued[-1]["maximum_velocity_m_per_s"] = "nan"
        self.write_continuation()
        self.refused("nonfinite")

    def test_unsealed_terminal_history(self):
        self.full_horizon()
        del self.args["final_summary"]
        self.refused("requires native final summary")

    def test_final_checkpoint_mutant(self):
        self.full_horizon()
        self.args["final_checkpoint"].write_bytes(b"wrong checkpoint")
        self.refused("final checkpoint changed")

    def test_final_summary_history_sha_mutant(self):
        self.full_horizon()
        self.args["final_summary"].write_text(self.args["final_summary"].read_text().replace(
            "accepted_timestep_history_sha256 ", "accepted_timestep_history_sha256 f"))
        self.refused("history SHA")

    def test_terminal_merkle_mutant(self):
        self.full_horizon()
        sidecar = Path(str(self.args["accepted_history"]) + ".payload-v2.json")
        data = json.loads(sidecar.read_text())
        data["v2"]["root_sha256"] = "0"*64
        sidecar.write_text(json.dumps(data))
        self.refused("published payload seal")

    def test_native_dispatch_exact_executable_and_marker(self):
        # This is a mocked dispatch-mechanism test, not a validated join.
        executable = self.args["new_executable"]
        certificate = self.args["certificate"]
        args = ["--fire-resident-resume-validate", str(certificate)]
        marker = "RESIDENT_RESUME_VALIDATE passed=1 version=2 steps=8 error="
        with patch.object(target.subprocess, "run", return_value=subprocess.CompletedProcess(
                args, 0, (marker + "\n").encode(), b"")) as run:
            receipt = target.native_validate(executable, args,
                {certificate: target.digest(certificate.read_bytes())}, marker)
        self.assertEqual(run.call_args.args[0], [str(executable.resolve()), *args])
        self.assertEqual(receipt["exit_code"], 0)

    def test_native_failure_blocks_join_even_when_every_json_and_sidecar_matches(self):
        self.full_horizon()
        self.args["diagnostic_only"] = False
        with patch.object(target.subprocess, "run", return_value=subprocess.CompletedProcess(
                [], 0, b"caller asserted passed\n", b"")) as run:
            self.refused("success marker missing")
        self.assertEqual(run.call_count, 1)
        self.assertEqual(run.call_args.args[0][1], "--fire-resident-resume-validate")

    def test_native_final_loader_refuses_fabricated_checkpoint(self):
        self.full_horizon()
        self.args["diagnostic_only"] = False
        results = [subprocess.CompletedProcess([], 0,
                   b"RESIDENT_RESUME_VALIDATE passed=1 version=2 steps=8 error=\n", b""),
                   subprocess.CompletedProcess([], 94, b"R216_FINAL_VALIDATE passed=0\n", b"invalid checkpoint")]
        with patch.object(target.subprocess, "run", side_effect=results) as run:
            self.refused("native validation refused")
        final_call = run.call_args_list[1].args[0]
        self.assertEqual(final_call, [str(self.args["new_executable"].resolve()),
            "--fire-r216-final-validate", str(self.args["final_checkpoint"].resolve()),
            str(self.args["accepted_history"].resolve()), str(self.args["continuation_csv"].resolve()),
            str(self.args["certificate"].resolve())])

    def test_native_executable_replacement_during_invocation(self):
        executable = self.args["new_executable"]
        def replace(*args, **kwargs):
            executable.write_bytes(b"replaced executable")
            return subprocess.CompletedProcess(args, 0, b"PASS\n", b"")
        with patch.object(target.subprocess, "run", side_effect=replace):
            with self.assertRaisesRegex(ValueError, "changed during execution"):
                target.native_validate(executable, ["--fixture"], {}, "PASS")

    def test_native_input_replacement_during_invocation(self):
        executable = self.args["new_executable"]
        certificate = self.args["certificate"]
        expected = target.digest(certificate.read_bytes())
        def replace(*args, **kwargs):
            certificate.write_bytes(b"replaced certificate")
            return subprocess.CompletedProcess(args, 0, b"PASS\n", b"")
        with patch.object(target.subprocess, "run", side_effect=replace):
            with self.assertRaisesRegex(ValueError, "changed during execution"):
                target.native_validate(executable, ["--fixture"], {certificate: expected}, "PASS")

    def test_real_marker_emitter_cannot_self_approve_its_executable(self):
        # A real executable (not a subprocess mock) with fully re-sealed
        # certificate/bridge/identity is still not the independently approved
        # fixture executable. It must be refused before it can emit markers.
        marker = self.directory / "impostor-was-executed"
        impostor = self.directory / "marker-emitter"
        impostor.write_text("#!/bin/sh\n" + f"touch '{marker}'\n" +
            "echo 'RESIDENT_RESUME_VALIDATE passed=1 version=2 steps=8 error='\n")
        impostor.chmod(0o755)
        forged_sha = target.digest(impostor.read_bytes())
        self.args["new_executable"] = impostor
        self.payload["new_executable_sha256"] = forged_sha
        self.payload["new_trace"]["executable_sha256"] = forged_sha
        self.next_id["producer_executable_sha256"] = forged_sha
        self.reseal()
        self.args["diagnostic_only"] = False
        self.refused("not independently qualified")
        self.assertFalse(marker.exists())

    def test_resealed_foreign_build_cannot_self_approve(self):
        self.payload["new_build_id"] = "f"*64
        self.payload["new_trace"]["build_id"] = "f"*64
        self.next_id["producer_build_id"] = "f"*64
        self.reseal()
        self.args["diagnostic_only"] = False
        self.refused("not independently qualified")

    def test_unverified_prefix_velocity_mutation_cannot_claim_crossing(self):
        self.continued[-1]["maximum_velocity_m_per_s"] = "15"
        self.write_continuation()
        self.args["diagnostic_only"] = False
        with patch.object(target.subprocess, "run", return_value=subprocess.CompletedProcess([], 0,
                b"RESIDENT_RESUME_VALIDATE passed=1 version=2 steps=8 error=\n", b"")):
            _, report = target.join(**self.args)
        self.assertEqual(report["authority"], "migration_validated_prefix_unverified")
        self.assertEqual(report["status"], "non_verdict_prefix")
        self.assertIsNone(report["crossed_stop_threshold"])
        self.assertTrue(report["observed_unverified_crossing"])
        self.assertFalse(report["onset_survival_passed"])


if __name__ == "__main__":
    unittest.main()
