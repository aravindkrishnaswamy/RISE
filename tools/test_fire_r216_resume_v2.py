#!/usr/bin/env python3
"""Synthetic parser REDs; optionally validate actual issued certificate mutants.

Set RISE_R216_NATIVE_VALIDATOR and RISE_R216_CERTIFICATE for read-only native
certificate verification. No solver is run and no original evidence is edited.
"""
import copy
import csv
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import fire_r216_resume_v2 as issuer


class IssuerContract(unittest.TestCase):
    def identity(self):
        return dict(build_id="a"*64, executable_sha256="b"*64,
                    checkpoint_build_id=issuer.OLD_BUILD, checkpoint_sha256=issuer.CHECKPOINT,
                    resumed_from_step=issuer.STEP, beginning_time_s_bits=issuer.bits(issuer.BEGIN))

    def synthetic_original(self):
        rows = []
        previous = 0.0
        for index in range(1, 806):
            dt = issuer.BEGIN-791/1024 if index == 1 else 1/1024
            previous += dt
            rows.append(dict(accepted_step=str(index), time_s=repr(previous), dt_s=repr(dt),
                             qualified_kernel_set_sha256=issuer.KERNEL))
        return rows

    def encoded(self, rows):
        stream = io.StringIO()
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
        return stream.getvalue().encode()

    def traces(self):
        old = dict(build_id=issuer.OLD_BUILD, executable_sha256=issuer.OLD_EXE, digest_version=2,
                   accepted_time_s_bits=[issuer.bits(issuer.BEGIN+(i+1)/1024) for i in range(8)],
                   dt_s_bits=[issuer.bits(1/1024)]*8, input_payload_roots=["c"*64]*8,
                   publication_payload_roots=["d"*64]*8, kernel_set_sha256=[issuer.KERNEL]*8)
        new = copy.deepcopy(old)
        new.update(build_id=self.identity()["build_id"], executable_sha256=self.identity()["executable_sha256"])
        return old, new

    def test_exact_historical_selection(self):
        _, selected = issuer.rows(issuer.historical_excerpt(self.encoded(self.synthetic_original())))
        self.assertEqual([int(row["accepted_step"]) for row in selected], list(range(793, 801)))
        self.assertEqual(float(selected[0]["time_s"])-float(selected[0]["dt_s"]), issuer.BEGIN)

    def test_historical_gap_duplicate_short_time_and_kernel_reds(self):
        for key in ("accepted_step", "time_s", "dt_s", "qualified_kernel_set_sha256"):
            for index in (0, 791, 792, 799, 804):
                rows = self.synthetic_original()
                rows[index][key] = "0" if key != "qualified_kernel_set_sha256" else "0"*64
                with self.subTest(key=key, index=index), self.assertRaises(ValueError):
                    issuer.historical_excerpt(self.encoded(rows))
        rows = self.synthetic_original()
        with self.assertRaises(ValueError):
            issuer.historical_excerpt(self.encoded(rows[:-1]))

    def test_inspected_checkpoint_and_binary_reds(self):
        issuer.validate_identity(self.identity(), "b"*64)
        for key in self.identity():
            mutant = self.identity()
            mutant[key] = 0 if isinstance(mutant[key], int) else "0"*64
            if key == "build_id":
                mutant[key] = issuer.OLD_BUILD
            with self.subTest(key=key), self.assertRaises(ValueError):
                issuer.validate_identity(mutant, "b"*64)
        with self.assertRaises(ValueError):
            issuer.validate_identity(self.identity(), "c"*64)

    def test_each_step_exact_time_dt_root_and_kernel_reds(self):
        issuer.validate_trace_pair(*self.traces(), self.identity())
        for key in ("accepted_time_s_bits", "dt_s_bits", "input_payload_roots",
                    "publication_payload_roots", "kernel_set_sha256"):
            for index in range(8):
                old, new = self.traces()
                new[key][index] = 0 if isinstance(new[key][index], int) else "0"*64
                with self.subTest(key=key, index=index), self.assertRaises(ValueError):
                    issuer.validate_trace_pair(old, new, self.identity())
            old, new = self.traces()
            old[key] = old[key][:7]
            new[key] = new[key][:7]
            with self.subTest(short=key), self.assertRaises(ValueError):
                issuer.validate_trace_pair(old, new, self.identity())
        for side in (0, 1):
            for key in ("build_id", "executable_sha256"):
                traces = self.traces()
                traces[side][key] = "0"*64
                with self.subTest(side=side, key=key), self.assertRaises(ValueError):
                    issuer.validate_trace_pair(*traces, self.identity())

    def test_duplicate_metadata_red(self):
        with self.assertRaises(ValueError):
            issuer.text_metadata(b"build a\nbuild b\n")
        with self.assertRaises(ValueError):
            json.loads('{"build":1,"build":2}', object_pairs_hook=issuer.unique_pairs)


@unittest.skipUnless(os.environ.get("RISE_R216_NATIVE_VALIDATOR") and os.environ.get("RISE_R216_CERTIFICATE"),
                     "actual native certificate must first be issued; no fabricated execution")
class ActualNativeCertificate(unittest.TestCase):
    def test_actual_certificate_and_each_bound_surface_mutant(self):
        certificate = Path(os.environ["RISE_R216_CERTIFICATE"])
        bridge = json.loads((certificate.parent / "bridge.v2.json").read_text())
        payload = bridge["payload"]
        self.assertEqual(certificate.read_bytes(), issuer.envelope(payload))
        executable = os.environ["RISE_R216_NATIVE_VALIDATOR"]

        def check(value, accepted):
            with tempfile.TemporaryDirectory(prefix="rise-r216-certificate-red-") as temporary:
                path = Path(temporary) / "mutant.cbor"
                path.write_bytes(issuer.envelope(value))
                result = subprocess.run([executable, "--fire-resident-resume-validate", str(path)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, accepted, result.stdout+result.stderr)

        check(payload, True)
        for key in ("checkpoint_sha256", "old_build_id", "new_build_id", "old_executable_sha256",
                    "new_executable_sha256", "resumed_from_step", "beginning_time_s_bits"):
            mutant = copy.deepcopy(payload)
            mutant[key] = 0 if isinstance(mutant[key], int) else "0"*64
            check(mutant, False)
        for side in ("old_trace", "new_trace"):
            for key in ("accepted_time_s_bits", "dt_s_bits", "input_payload_roots",
                        "publication_payload_roots", "kernel_set_sha256"):
                for index in range(8):
                    mutant = copy.deepcopy(payload)
                    mutant[side][key][index] = 0 if isinstance(mutant[side][key][index], int) else "0"*64
                    check(mutant, False)
                mutant = copy.deepcopy(payload)
                mutant[side][key].pop()
                check(mutant, False)
            for key in ("csv_sha256", "log_sha256", "executable_sha256"):
                mutant = copy.deepcopy(payload)
                mutant[side][key] = "0"*64
                check(mutant, False)


if __name__ == "__main__":
    unittest.main()
