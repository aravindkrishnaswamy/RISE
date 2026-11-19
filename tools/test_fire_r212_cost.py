#!/usr/bin/env python3
"""Exact quantile/invalid-input checks; these do not qualify a solver."""
import unittest
import hashlib
import json
import re
from pathlib import Path
from unittest.mock import patch
from report_fire_r212_cost import stats, authenticated_input, observer_record, report


class CostStatistics(unittest.TestCase):
    def test_qualification_document_uses_executable_not_receipt_digest(self):
        raw = Path("rendered/fire_production_calibration/r212_transport_adoption/qualification.v2.json").read_bytes()
        receipt = json.loads(raw)
        document = Path("docs/FIRE_SMOKE_TRANSPORT_ADOPTION_R212.md").read_text()
        expected = receipt["producer_executable_sha256"]
        claim = re.search(r"qualified executable SHA is\s+`([0-9a-f]{64})`", document)
        self.assertIsNotNone(claim)
        self.assertEqual(claim.group(1), expected)
        receipt_digest = hashlib.sha256(raw).hexdigest()
        self.assertNotEqual(expected, receipt_digest)
        mutant = document.replace(expected, receipt_digest)
        self.assertNotEqual(re.search(r"qualified executable SHA is\s+`([0-9a-f]{64})`",
                                      mutant).group(1), expected)

    def test_nearest_rank_and_scope(self):
        result = stats(list(range(1, 21)))
        self.assertEqual(result["p95"], 19)
        self.assertEqual(result["samples"], 20)
        self.assertEqual(result["mean"], 10.5)

    def test_one_sample(self):
        self.assertEqual(stats([3]), dict(samples=1, mean=3, stdev=0, p95=3))

    def test_invalid_samples_refused(self):
        for values in ([], [-1], [float("nan")], [float("inf")]):
            with self.assertRaises(ValueError):
                stats(values)

    def test_changed_wall_or_iteration_and_duplicate_repeat_refused(self):
        first = b"wall_ms,owner_r0_iterations\n20000,5\n"
        second = b"wall_ms,owner_r0_iterations\n21000,6\n"
        expected = {"repeat1": hashlib.sha256(first).hexdigest(),
                    "repeat2": hashlib.sha256(second).hexdigest()}
        self.assertEqual(authenticated_input("repeat1", first, expected), expected["repeat1"])
        for key, raw in (("repeat1", first.replace(b"20000", b"1000000")),
                         ("repeat1", first.replace(b",5", b",0")),
                         ("repeat2", first), ("unrecorded", first)):
            with self.assertRaises(ValueError):
                authenticated_input(key, raw, expected)

    def test_observer_requires_one_executed_success(self):
        line = "OWNER_CONVERGENCE_CROSSING path=fixture terminal_bit_identity=1 diagnostic_wall_ms=123.25 passed=1\n"
        self.assertEqual(observer_record(line), dict(calls=1, wall_ms=123.25,
                                                   included_in_production_rows=False))
        for text in ("", line + line, line.replace("passed=1", "passed=0")):
            with self.assertRaises(ValueError):
                observer_record(text)

    def test_report_refuses_log_replaced_between_reads(self):
        # Like report(), this evidence-level RED requires the archived r206,
        # r211 and r212 executed inputs restored at their recorded paths.
        original_read = Path.read_bytes
        target = Path("rendered/fire_production_calibration/r212_transport_adoption/hot_profile_1.v1.log")
        reads = 0

        def replaced_read(path):
            nonlocal reads
            raw = original_read(path)
            if path == target:
                reads += 1
                if reads == 2:
                    self.assertIn(b" kernel=", raw)
                    return raw.replace(b" kernel=", b" kernel=unauthenticated_", 1)
            return raw

        with patch.object(Path, "read_bytes", replaced_read):
            with self.assertRaisesRegex(ValueError, "producer log changed during cost analysis"):
                report()
        self.assertEqual(reads, 2)


if __name__ == "__main__":
    unittest.main()
