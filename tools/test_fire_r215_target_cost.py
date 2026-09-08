#!/usr/bin/env python3
"""Synthetic evidence mutants; never manufacturing production measurements."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import report_fire_r215_target_cost as report
from seal_fire_payload_placement import distinct_repeats


class TargetCostEvidence(unittest.TestCase):
    def fixture(self):
        # Synthetic parser fixture derived from an immutable historical schema.
        path = report.ROOT / "rendered/fire_production_calibration/r213_composition/hot_numerical_qualification.v2.json"
        baseline = json.loads(path.read_text())["per_step"]
        comparison = {}
        for index in range(1, 9):
            prior = baseline[str(index)]
            row = copy.deepcopy(prior)
            for name in ("build", "checkpoint_sha256", "comparison_sha256"):
                row["old_"+name] = prior["new_"+name]
            row["new_build"] = report.CANDIDATE_BUILD
            comparison[index] = row
        return comparison, baseline

    def test_exact_bound_lineage(self):
        report.validate_comparison_lineage(*self.fixture())

    def test_wrong_baseline_or_candidate_identity_red(self):
        for field in ("old_build", "old_checkpoint_sha256", "old_comparison_sha256",
                      "new_build", "new_comparison_sha256", "time_s", "bytes"):
            with self.subTest(field=field):
                comparison, baseline = self.fixture()
                comparison[4][field] = "forged-synthetic-value"
                with self.assertRaises(ValueError):
                    report.validate_comparison_lineage(comparison, baseline)

    def test_short_or_extra_comparison_red(self):
        for index in (0, 8):
            comparison, baseline = self.fixture()
            if index == 0:
                comparison[0] = copy.deepcopy(comparison[1])
            else:
                del comparison[8]
            with self.assertRaises(ValueError):
                report.validate_comparison_lineage(comparison, baseline)

    def repeats(self, directory, kind):
        names = ["synthetic_1", "synthetic_2", "synthetic_3"]
        for index, name in enumerate(names):
            tick = 100+index if kind == "distinct" else 100
            tag = index if kind != "identical_logs" else 0
            text = (f"SYNTHETIC_ONLY tag={tag}\nPRODUCER_COMMAND_V1 stage=0 raw_iteration=0 cpu_begin={tick} "
                    f"cpu_end={tick+1} gpu_begin={tick} gpu_end={tick+1} "
                    "device_ms=0.001 encoders=1 interval_scope=possibly_overlapping\n")
            (directory / (name+".log")).write_text(text)
        return names

    def test_independent_clock_evidence(self):
        with tempfile.TemporaryDirectory(prefix="r215-synthetic-clocks-") as temporary:
            directory = Path(temporary)
            distinct_repeats(directory, self.repeats(directory, "distinct"))

    def test_duplicated_log_and_clock_red(self):
        for kind in ("identical_logs", "same_clocks_different_text"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory(prefix="r215-synthetic-clocks-") as temporary:
                directory = Path(temporary)
                names = self.repeats(directory, kind)
                expected = "duplicate process logs" if kind == "identical_logs" else "duplicate measured execution clock identities"
                with self.assertRaisesRegex(ValueError, expected):
                    distinct_repeats(directory, names)

    def test_corrupted_payload_and_rewritten_proof_red(self):
        checkpoint = report.OUT / "hot_snapshots.v1/checkpoints/step_01.checkpoint"
        comparison = report.OUT / "hot_byte_comparison.v2"
        original_read = Path.read_bytes
        payload = bytearray(original_read(checkpoint))
        old_hash = hashlib.sha256(payload).hexdigest()
        payload[0] ^= 1
        rewritten = original_read(comparison).replace(old_hash.encode(), hashlib.sha256(payload).hexdigest().encode())
        self.assertNotEqual(rewritten, original_read(comparison))

        def mutant(path):
            if path == comparison:
                return rewritten
            if path == checkpoint:
                return bytes(payload)
            return original_read(path)

        with mock.patch.object(Path, "read_bytes", mutant):
            with self.assertRaisesRegex(ValueError, "evidence SHA mismatch:.*hot_byte_comparison"):
                report.report()

    def test_execution_receipt_cannot_reseal_its_own_edits_red(self):
        path = report.OUT / "hot_profiles.execution.v1.json"
        original_read = Path.read_bytes
        receipt = json.loads(original_read(path))
        receipt["runs"][0]["start_unix_ns"] += 1
        rewritten = json.dumps(receipt).encode()
        with mock.patch.object(Path, "read_bytes", lambda p: rewritten if p == path else original_read(p)):
            with self.assertRaisesRegex(ValueError, "evidence SHA mismatch:.*hot_profiles.execution"):
                report.report()


if __name__ == "__main__":
    unittest.main()
