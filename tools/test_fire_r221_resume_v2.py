#!/usr/bin/env python3
"""Synthetic r221 issuer refusal tests; native REDs use real executed overlap."""
import copy
import csv
import io
from pathlib import Path
import subprocess
import tempfile
import unittest

import fire_r221_resume_v2 as issuer


class OverlapContract(unittest.TestCase):
    def test_historical_live_aliases_refuse_before_checkpoint_io(self):
        executable = Path(__file__).resolve().parents[1] / "bin/tests/FireSequenceTest"
        with tempfile.TemporaryDirectory(prefix="rise-r221-alias-red-") as temporary:
            root = Path(temporary)
            for name in ("--fire-production-r211-continue", "--fire-r216-continue"):
                result = subprocess.run([str(executable), name, str(root/"unopened.checkpoint"),
                                         str(root/"unopened.cbor"), str(root/"never_created"),
                                         str(root/"unopened.protocol"), "0"*64],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                self.assertEqual(result.returncode, 91, result.stdout)
                self.assertIn("historical_live_alias_retired read_only_verifiers_retained=1", result.stdout)
                self.assertFalse((root/"never_created").exists())

    def identity(self):
        return dict(build_id="a"*64, executable_sha256="b"*64,
                    checkpoint_build_id=issuer.OLD_BUILD, checkpoint_sha256=issuer.CHECKPOINT,
                    resumed_from_step=826, beginning_time_s_bits=issuer.bits(issuer.BEGIN))

    def original(self):
        rows, previous = [], 0.0
        for index in range(1, 836):
            dt = issuer.BEGIN-825/1024 if index == 1 else 1/1024
            previous += dt
            rows.append(dict(accepted_step=str(index), time_s=repr(previous), dt_s=repr(dt),
                             qualified_kernel_set_sha256=issuer.KERNEL))
        return rows

    def encode(self, rows):
        stream = io.StringIO()
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
        return stream.getvalue().encode()

    def traces(self):
        old = dict(build_id=issuer.OLD_BUILD, executable_sha256=issuer.OLD_EXE, digest_version=2,
                   accepted_time_s_bits=[issuer.bits(issuer.BEGIN+(i+1)/1024) for i in range(9)],
                   dt_s_bits=[issuer.bits(1/1024)]*9, input_payload_roots=["c"*64]*9,
                   publication_payload_roots=["d"*64]*9, kernel_set_sha256=[issuer.KERNEL]*9)
        new = copy.deepcopy(old)
        new.update(build_id="a"*64, executable_sha256="b"*64)
        return old, new

    def test_nine_exact_historical_steps(self):
        _, rows = issuer.rows(issuer.historical_excerpt(self.encode(self.original())))
        self.assertEqual([int(row["accepted_step"]) for row in rows], list(range(827, 836)))
        self.assertEqual(float(rows[0]["time_s"])-float(rows[0]["dt_s"]), issuer.BEGIN)

    def test_full_history_not_just_excerpt(self):
        for key in ("accepted_step", "time_s", "dt_s", "qualified_kernel_set_sha256"):
            for index in (0, 824, 825, 826, 834):
                rows = self.original()
                rows[index][key] = "0"
                with self.subTest(key=key, index=index), self.assertRaises(ValueError):
                    issuer.historical_excerpt(self.encode(rows))
        with self.assertRaises(ValueError):
            issuer.historical_excerpt(self.encode(self.original()[:-1]))

    def test_identity_pinned_to_826_and_qualified_binary(self):
        issuer.validate_identity(self.identity(), "b"*64, "a"*64)
        for key in self.identity():
            value = self.identity()
            value[key] = 0 if isinstance(value[key], int) else "0"*64
            with self.subTest(key=key), self.assertRaises(ValueError):
                issuer.validate_identity(value, "b"*64, "a"*64)
        with self.assertRaises(ValueError):
            issuer.validate_identity(self.identity(), "c"*64, "a"*64)
        with self.assertRaises(ValueError):
            issuer.validate_identity(self.identity(), "b"*64, "c"*64)

    def test_all_steps_all_surfaces_not_pooled(self):
        issuer.validate_trace_pair(*self.traces(), self.identity())
        for side in (0, 1):
            for key in ("accepted_time_s_bits", "dt_s_bits", "input_payload_roots",
                        "publication_payload_roots", "kernel_set_sha256"):
                for index in range(9):
                    traces = self.traces()
                    traces[side][key][index] = 0 if isinstance(traces[side][key][index], int) else "0"*64
                    with self.subTest(side=side, key=key, index=index), self.assertRaises(ValueError):
                        issuer.validate_trace_pair(*traces, self.identity())
                traces = self.traces()
                for trace in traces:
                    trace[key] = trace[key][:7]
                with self.assertRaises(ValueError):
                    issuer.validate_trace_pair(*traces, self.identity())
            for key in ("build_id", "executable_sha256", "digest_version"):
                traces = self.traces()
                traces[side][key] = 0 if key == "digest_version" else "0"*64
                with self.assertRaises(ValueError):
                    issuer.validate_trace_pair(*traces, self.identity())


if __name__ == "__main__":
    unittest.main()
