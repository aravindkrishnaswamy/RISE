#!/usr/bin/env python3
"""Mutation REDs for the native hot-snapshot report admission surface."""
import unittest
from report_fire_r213_snapshots import admit_native, pairs


def fixture():
    rows = ["schema rise.fire.r213.numeric-snapshot-comparison.v1",
            "scope canonical_persistent_payload_minus_producer_build_not_migration",
            "excluded_field producerBuildId", "required_steps 8",
            "reporter_build_id "+"a"*64, "reporter_executable_sha256 "+"b"*64]
    for step in range(1, 9):
        rows.append(f"step {step} time_s {2+step/1000} old_build {'c'*64} new_build {'d'*64} "
                    f"old_checkpoint_sha256 {'e'*64} new_checkpoint_sha256 {'f'*64} "
                    f"old_comparison_sha256 {'1'*64} new_comparison_sha256 {'1'*64} "
                    "bytes 64000000 exact_bytes_equal 1")
        for component in range(10):
            rows.append(f"scalar step {step} component {component} cells 504666 bit_mismatches 0")
        for field in ("momentum", "velocity"):
            for axis, count in enumerate((511980, 511980, 509427)):
                rows.append(f"face step {step} field {field} axis {axis} faces {count} bit_mismatches 0")
    rows += ["red persisted_source_ledger_and_temperature_history_mutations_refused 1",
             "passed 1", "migration_authority false"]
    return "\n".join(rows)


class SnapshotAdmissionTest(unittest.TestCase):
    def test_complete_native_admission(self):
        steps, headers = admit_native(fixture())
        self.assertEqual(set(steps), set(range(1, 9)))
        self.assertEqual(headers["excluded_field"], "producerBuildId")

    def test_missing_or_duplicate_step_and_field_refused(self):
        text = fixture()
        for prefix in ("step 8 ", "scalar step 1 component 2 ", "face step 8 field velocity axis 2 "):
            row = next(r for r in text.splitlines() if r.startswith(prefix))
            for mutant in (text.replace(row+"\n", ""), text+"\n"+row):
                with self.subTest(prefix=prefix), self.assertRaises(ValueError):
                    admit_native(mutant)

    def test_each_numerical_surface_refuses_mismatch(self):
        text = fixture()
        for before, after in (("exact_bytes_equal 1", "exact_bytes_equal 0"),
                              ("cells 504666", "cells 504665"),
                              ("faces 511980", "faces 511979"),
                              ("bit_mismatches 0", "bit_mismatches 1"),
                              ("new_comparison_sha256 "+"1"*64, "new_comparison_sha256 "+"2"*64)):
            with self.subTest(before=before), self.assertRaises(ValueError):
                admit_native(text.replace(before, after, 1))

    def test_no_authority_expansion_or_contradictory_result(self):
        text = fixture()
        for before, after in (("excluded_field producerBuildId", "excluded_field ledgers"),
                              ("migration_authority false", "migration_authority true"),
                              ("passed 1", "passed 0"),
                              ("reporter_build_id "+"a"*64, "reporter_build_id missing"),
                              ("mutations_refused 1", "mutations_refused 0")):
            with self.subTest(before=before), self.assertRaises(ValueError):
                admit_native(text.replace(before, after))
        with self.assertRaises(ValueError):
            admit_native(text+"\npassed 0")
        with self.assertRaises(ValueError):
            admit_native("\n".join(r for r in text.splitlines() if not r.startswith("red ")))

    def test_duplicate_native_keys_refused(self):
        with self.assertRaises(ValueError):
            pairs(["a", "1", "a", "2"])


if __name__ == "__main__":
    unittest.main()
