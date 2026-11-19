#!/usr/bin/env python3
"""Executed-evidence and deliberately synthetic parser REDs for r213 cost reporting."""
import copy
import csv
import io
from pathlib import Path
import unittest
from unittest.mock import patch

import report_fire_r213_cost as report


class CostEvidenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.log_path = report.OUT / "hot_profile_1.v1.log"
        cls.log = cls.log_path.read_text()
        cls.rows = list(csv.DictReader(io.StringIO((report.OUT /
            "hot_profile_1.v1/budgets/maximum_velocity_trajectory.csv").read_text())))
        cls.reference = list(csv.DictReader(io.StringIO((report.BASE /
            "r211_resident_migration/continuation.v2/budgets/maximum_velocity_trajectory.csv").read_text())))[:8]

    def test_executed_observation_reproduces(self):
        observed = report.report()
        self.assertEqual(observed["after"]["device_ms"]["samples"], 24)
        self.assertAlmostEqual(observed["after"]["device_ms"]["mean"], 6420.787531669096)
        self.assertAlmostEqual(observed["after"]["wall_ms"]["p95"], 8938.276375)
        self.assertEqual(len(observed["independent_processes"]), 3)
        for process in observed["equal_time_comparisons"]:
            self.assertEqual(len(process), 8)
            for row in process:
                self.assertFalse(row["full_roots_equal"])
                self.assertEqual(row["numerical_payload_byte_equality"], "pending_snapshot_comparison")

    def test_changed_executed_input_cannot_self_authorize(self):
        original = Path.read_bytes
        with patch.object(Path, "read_bytes", lambda p: b"changed" if p == self.log_path else original(p)):
            with self.assertRaisesRegex(ValueError, "executed input SHA mismatch"):
                report.report()

    def test_same_time_and_each_summary_field_required(self):
        report.validate_hot(self.log, self.rows, self.reference)
        for key in report.PHYSICS:
            changed = copy.deepcopy(self.rows)
            changed[3][key] = "wrong"
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "summary mismatch"):
                report.validate_hot(self.log, changed, self.reference)
        with self.assertRaisesRegex(ValueError, "exactly eight"):
            report.validate_hot(self.log, self.rows[:-1], self.reference)

    def test_missing_or_contradictory_completion_refuses(self):
        terminal = next(row for row in self.log.splitlines() if row.startswith("OWNER_EOS_DIAGNOSTIC_END "))
        for changed in (self.log.replace(terminal, ""), self.log+terminal+"\n",
                        self.log+terminal.replace("solver_accepted=1", "solver_accepted=0")+"\n",
                        self.log.replace("solver_accepted=1", "solver_accepted=0 solver_accepted=1")):
            with self.subTest(log=changed[-160:]), self.assertRaises(ValueError):
                report.validate_hot(changed, self.rows, self.reference)

    def test_wrong_root_identity_and_false_accounting_refuse(self):
        for key in ("qualified_kernel_set_sha256", "input_payload_root_sha256",
                    "publication_payload_root_sha256", "device_ms"):
            changed = copy.deepcopy(self.rows)
            changed[1][key] = self.reference[1][key]
            with self.subTest(key=key), self.assertRaises(ValueError):
                report.validate_hot(self.log, changed, self.reference)

    def test_second_read_and_copied_process_refuse(self):
        original = report.summarize
        def changed(path):
            result = original(path)
            result["input_sha256"] = "0"*64
            return result
        with patch.object(report, "summarize", changed):
            with self.assertRaisesRegex(ValueError, "second parse"):
                report.report()
        with patch.object(report, "LOGS", (report.LOGS[0],)*3):
            with self.assertRaisesRegex(ValueError, "must be distinct"):
                report.report()


if __name__ == "__main__":
    unittest.main()
