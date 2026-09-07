#!/usr/bin/env python3
"""Synthetic evidence only: exercise the actual C++ v2 loader and refusal surface."""
import copy
import csv
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

from fire_resident_resume_v2 import bits, envelope, require_equivalence, sha, trace_from_executed_csv

EXE = Path(__file__).resolve().parents[1] / "bin/tests/FireSequenceTest"


class ResidentResumeV2Test(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="rise-r211-red-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.payload = {"record_kind": "fire-resume-equivalence-certificate-v2", "schema_version": 2,
                        "checkpoint_sha256": "a" * 64, "resumed_from_step": 1300,
                        "beginning_time_s_bits": bits(2.0), "old_build_id": "b" * 64,
                        "new_build_id": "c" * 64}
        header = ["accepted_step", "time_s", "dt_s", "payload_digest_format", "payload_digest_version",
                  "input_payload_root_sha256", "publication_payload_root_sha256", "qualified_kernel_set_sha256"]
        self.rows = [[str(1301 + i), repr(2.0 + (i + 1) / 1024), repr(1 / 1024),
                      "rise-payload-sha256-merkle", "2", "d" * 64,
                      hashlib.sha256(str(i).encode()).hexdigest(), "f" * 64] for i in range(8)]
        for label, build in (("old", "b" * 64), ("new", "c" * 64)):
            binary, log, csv_path = (self.root / (label + suffix) for suffix in (".binary", ".log", ".csv"))
            binary.write_bytes(("synthetic qualification only " + label).encode())
            log.write_text("synthetic execution fixture, no production authority\n")
            with csv_path.open("w", newline="") as stream:
                writer = csv.writer(stream, lineterminator="\n")
                writer.writerow(header)
                writer.writerows(self.rows)
            trace = trace_from_executed_csv(csv_path, log, binary, build, self.payload)
            self.payload[label + "_trace"] = trace
            self.payload[label + "_executable_sha256"] = sha(binary)

    def validate(self, payload, expected):
        path = self.root / "candidate.cbor"
        path.write_bytes(envelope(payload))
        result = subprocess.run([str(EXE), "--fire-resident-resume-validate", str(path)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, expected, result.stdout + result.stderr)
        self.assertIn("RESIDENT_RESUME_VALIDATE passed=" + str(int(expected)), result.stdout)

    def test_green_and_each_step_root_mismatch(self):
        self.validate(self.payload, True)
        for index in range(8):
            with self.subTest(step=index):
                mutant = copy.deepcopy(self.payload)
                mutant["new_trace"]["publication_payload_roots"][index] = "0" * 64
                self.validate(mutant, False)
        # Defeat raw-CSV checking too: re-seal the altered NEW CSV. Cross-trace
        # equality must still reject the one changed final step.
        mutant = copy.deepcopy(self.payload)
        path = Path(mutant["new_trace"]["csv_path"])
        text = path.read_text().replace(self.rows[-1][6], "0" * 64)
        path.write_text(text)
        mutant["new_trace"]["csv_sha256"] = sha(path)
        mutant["new_trace"]["publication_payload_roots"][-1] = "0" * 64
        self.validate(mutant, False)
        with self.assertRaises(ValueError):
            require_equivalence(mutant["old_trace"], mutant["new_trace"])

    def test_short_trace_and_exact_time(self):
        for field in ("accepted_time_s_bits", "dt_s_bits", "input_payload_roots",
                      "publication_payload_roots", "kernel_set_sha256"):
            mutant = copy.deepcopy(self.payload)
            mutant["new_trace"][field].pop()
            self.validate(mutant, False)
        mutant = copy.deepcopy(self.payload)
        for label in ("old_trace", "new_trace"):
            for key in ("accepted_time_s_bits", "dt_s_bits", "input_payload_roots",
                        "publication_payload_roots", "kernel_set_sha256"):
                mutant[label][key] = mutant[label][key][:7]
        self.validate(mutant, False)
        mutant = copy.deepcopy(self.payload)
        mutant["new_trace"]["accepted_time_s_bits"][3] ^= 1
        self.validate(mutant, False)

    def test_executable_binding_and_evidence_tamper(self):
        for field in ("old_executable_sha256", "new_executable_sha256", "old_build_id", "new_build_id"):
            mutant = copy.deepcopy(self.payload)
            mutant[field] = "0" * 64
            self.validate(mutant, False)
        mutant = copy.deepcopy(self.payload)
        mutant["new_trace"]["executable_sha256"] = "0" * 64
        mutant["new_executable_sha256"] = "0" * 64
        self.validate(mutant, False)
        Path(self.payload["new_trace"]["executable_path"]).write_bytes(b"binary replaced after execution")
        self.validate(self.payload, False)

    def test_version_and_canonical_envelope(self):
        for version in (0, 1, 3):
            mutant = copy.deepcopy(self.payload)
            mutant["schema_version"] = version
            self.validate(mutant, False)
        mutant = copy.deepcopy(self.payload)
        mutant["new_trace"]["digest_version"] = 1
        self.validate(mutant, False)


if __name__ == "__main__":
    unittest.main()
