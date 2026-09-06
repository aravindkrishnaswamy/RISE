#!/usr/bin/env python3
"""Mutation REDs against the actual r207 v3 executed crossing fixture."""

import copy
import re
import unittest
from pathlib import Path

from check_fire_owner_instrumentation import qualify_artifact
from check_fire_r207_crossing_qualification import (EXECUTED_V3_LOG_SHA256, face_topology,
                                                  load_inputs, one, qualify_crossing, sha, tagged)


BASE = Path(__file__).resolve().parents[1] / "rendered/fire_production_calibration/r207_tier8_verdict/crossing_gate.v3"


class CrossingQualificationTests(unittest.TestCase):
    def setUp(self):
        self.inputs = list(load_inputs(BASE))

    def refused(self, values):
        with self.assertRaises(ValueError):
            qualify_crossing(*values)

    def rehash_crossing(self, values, replacement):
        values[0] = values[0].replace(sha(values[3]), sha(replacement))
        values[3] = replacement

    def rehash_trace_body(self, text):
        lines = text.splitlines(keepends=True)
        begin = next(i for i, line in enumerate(lines) if line.startswith("OWNER_CONVERGENCE_PROBE "))
        end = next(i for i, line in enumerate(lines) if line.startswith("OWNER_CONVERGENCE_BINDING "))
        lines[end] = "OWNER_CONVERGENCE_BINDING sha256=" + sha("".join(lines[begin:end]).encode()) + "\n"
        return "".join(lines).encode()

    def refused_rehashed_log(self, mutant):
        # All caller-controlled declarations agree. Only the externally recorded
        # execution root can keep this altered bundle from minting authority.
        published = one(mutant[0], "OWNER_CONVERGENCE_CROSSING")
        self.assertEqual(published["sha256"], sha(mutant[3]))
        self.assertEqual(published["csv_sha256"], sha(mutant[4]))
        self.assertNotEqual(sha(mutant[0].encode()), EXECUTED_V3_LOG_SHA256)
        qualify_artifact(*mutant[:3])  # Historical qualifier intentionally unchanged.
        with self.assertRaisesRegex(ValueError, "externally anchored executed v3"):
            qualify_crossing(*mutant)

    def test_actual_executed_fixture_passes_only_as_qualification(self):
        result = qualify_crossing(*self.inputs)
        self.assertEqual(result["crossing_red_count"], 8)
        self.assertEqual(result["tail_red_count"], 2)
        self.assertEqual(result["physics_claim"], "unavailable_synthetic_fixture")
        self.assertEqual(result["log_sha256"], EXECUTED_V3_LOG_SHA256)
        with self.assertRaises(ValueError):
            qualify_crossing(*self.inputs, scope="production_evidence")

    def test_rehashed_stale_consumed_target_identities_cannot_mint_authority(self):
        mutant = copy.deepcopy(self.inputs)
        replacement = re.sub(rb"target_identity=\d+", b"target_identity=1", mutant[3])
        self.assertNotEqual(mutant[3], replacement)
        self.rehash_crossing(mutant, replacement)
        self.refused_rehashed_log(mutant)

    def test_rehashed_equal_volume_invalid_column_cannot_mint_authority(self):
        mutant = copy.deepcopy(self.inputs)
        replacement = mutant[3].decode().replace("shape=4,4,4", "shape=16,1,4")
        self.rehash_crossing(mutant, self.rehash_trace_body(replacement))
        self.refused_rehashed_log(mutant)

    def test_rehashed_jointly_truncated_face_and_csv_cannot_mint_authority(self):
        mutant = copy.deepcopy(self.inputs)
        old_csv_sha = sha(mutant[4])
        rows = mutant[4].splitlines(keepends=True)
        del rows[1]
        mutant[4] = b"".join(rows)
        mutant[0] = mutant[0].replace(old_csv_sha, sha(mutant[4]))
        text = mutant[3].decode()
        face = tagged(text, "OWNER_CONVERGENCE_FACE")[0]
        text = text.replace(face + "\n", "", 1).replace(old_csv_sha, sha(mutant[4]))
        self.rehash_crossing(mutant, self.rehash_trace_body(text))
        self.refused_rehashed_log(mutant)

    def test_rehashed_invalid_cost_record_cannot_mint_authority(self):
        mutant = copy.deepcopy(self.inputs)
        replacement = re.sub(rb" diagnostic_device_ms=\S+", b" diagnostic_device_ms=nan", mutant[3])
        replacement = re.sub(rb" qualification_trace_staging_count=\S+",
                             b" qualification_trace_staging_count=0", replacement)
        self.rehash_crossing(mutant, replacement)
        self.refused_rehashed_log(mutant)

    def test_log_pin_preserves_raw_bytes_and_rejects_unrelated_new_execution(self):
        for log in (self.inputs[0].replace("\n", "\r\n"), self.inputs[0] + "\n"):
            mutant = copy.deepcopy(self.inputs)
            mutant[0] = log
            self.refused_rehashed_log(mutant)

    def test_geometry_and_block_checks_are_independent_of_log_pin(self):
        text = self.inputs[3].decode()
        probe = one(text, "OWNER_CONVERGENCE_PROBE")
        faces = tagged(text, "OWNER_CONVERGENCE_FACE")
        shape, blocks = face_topology(probe, faces)
        self.assertEqual(shape, (4, 4, 4))
        self.assertEqual(len(blocks), 14)
        bad_probe = dict(probe, shape="16,1,4")
        with self.assertRaisesRegex(ValueError, "shape or column"):
            face_topology(bad_probe, faces)
        for mutant in (faces[1:], [faces[1]] + faces[1:], faces[21:] + faces[:21],
                       [line.replace("phase=bootstrap", "phase=picard") for line in faces],
                       [line.replace("raw_iteration_tag=0 ", "raw_iteration_tag=2 ") for line in faces]):
            with self.subTest(first=mutant[0]):
                with self.assertRaises(ValueError):
                    face_topology(probe, mutant)

    def test_each_deleted_new_record_closes_historical_false_green(self):
        for tag in ("OWNER_CROSSING_RED", "OWNER_CONVERGENCE_PRODUCTION_RERUN",
                    "OWNER_CONVERGENCE_EXPORT_FIXTURE", "OWNER_CONVERGENCE_CROSSING", "OWNER_TAIL_TARGET_RED"):
            for line in tagged(self.inputs[0], tag):
                with self.subTest(line=line):
                    mutant = copy.deepcopy(self.inputs)
                    mutant[0] = mutant[0].replace(line + "\n", "", 1)
                    # Existing historical gate cannot see these newly required records.
                    qualify_artifact(*mutant[:3])
                    self.refused(mutant)

    def test_duplicate_failed_and_malformed_new_records(self):
        for tag in ("OWNER_CROSSING_RED", "OWNER_CONVERGENCE_PRODUCTION_RERUN",
                    "OWNER_CONVERGENCE_EXPORT_FIXTURE", "OWNER_CONVERGENCE_CROSSING", "OWNER_TAIL_TARGET_RED"):
            for line in tagged(self.inputs[0], tag):
                for replacement in (line + "\n" + line, line.replace("passed=1", "passed=0"),
                                    line + "\tpassed=0", line.replace("passed=1", "passed"),
                                    line.replace("error=", "error=passed=0\t")):
                    if replacement == line:
                        continue
                    with self.subTest(tag=tag, replacement=replacement):
                        mutant = copy.deepcopy(self.inputs)
                        mutant[0] = mutant[0].replace(line, replacement, 1)
                        self.refused(mutant)

    def test_unknown_crossing_red_name_refused(self):
        mutant = copy.deepcopy(self.inputs)
        mutant[0] = mutant[0].replace("name=stale_dt ", "name=not_the_required_red ", 1)
        self.refused(mutant)

    def test_rehashed_orphan_source_event_and_owner_roots(self):
        for key in ("input_payload_root_sha256", "output_payload_root_sha256", "kernel_set_sha256",
                    "source_ledger_sha256", "source_observation_inputs_sha256", "event_sha256"):
            mutant = copy.deepcopy(self.inputs)
            text = mutant[3].decode()
            old = next(word for word in text.splitlines()[0].split() if word.startswith(key + "="))
            self.rehash_crossing(mutant, text.replace(old, key + "=" + "0" * 64, 1).encode())
            self.refused(mutant)
        for key in self.inputs[5]:
            mutant = copy.deepcopy(self.inputs)
            old = sha(mutant[5][key])
            mutant[5][key] = b"forged source field\n"
            replacement = mutant[3].replace((key + "=" + old).encode(),
                                            (key + "=" + sha(mutant[5][key])).encode())
            self.rehash_crossing(mutant, replacement)
            self.refused(mutant)

    def test_rehashed_csv_not_matching_trace_refused(self):
        mutant = copy.deepcopy(self.inputs)
        old = sha(mutant[4])
        # Alter a real face value, leave every other identity and row intact.
        lines = mutant[4].decode().splitlines()
        values = lines[1].split(",")
        values[13] = "12345"
        lines[1] = ",".join(values)
        mutant[4] = ("\n".join(lines) + "\n").encode()
        mutant[0] = mutant[0].replace(old, sha(mutant[4]))
        self.rehash_crossing(mutant, mutant[3].replace(old.encode(), sha(mutant[4]).encode()))
        self.refused(mutant)

    def test_unsealed_trace_body_and_isolation_label_refused(self):
        for old, new in ((b"source=r207_synthetic_fixture_no_physics_claim", b"source=production_run"),
                         (b"solver_publications=0", b"solver_publications=1"),
                         (b"fixed_k_selection=not_authorized", b"fixed_k_selection=authorized"),
                         (b"dt_s=1.52587890625e-05", b"dt_s=0.001")):
            mutant = copy.deepcopy(self.inputs)
            self.rehash_crossing(mutant, mutant[3].replace(old, new, 1))
            self.refused(mutant)

    def test_missing_tampered_and_rehashed_target_payload(self):
        mutant = copy.deepcopy(self.inputs)
        mutant[6] = b""
        self.refused(mutant)
        mutant = copy.deepcopy(self.inputs)
        # First target is the uncorrected bootstrap: a forged nonzero word
        # must fail even after the attacker updates the payload/outer SHA.
        changed = bytearray(mutant[6])
        changed[:4] = b"\x00\x00\x80\x3f"
        mutant[6] = bytes(changed)
        self.refused(mutant)
        self.rehash_crossing(mutant, mutant[3].replace(sha(self.inputs[6]).encode(), sha(mutant[6]).encode()))
        self.refused(mutant)
        mutant = copy.deepcopy(self.inputs)
        mutant[6] += b"\x00\x00\x00\x00"
        replacement = mutant[3].replace(sha(self.inputs[6]).encode(), sha(mutant[6]).encode())
        replacement = replacement.replace(("bytes=" + str(len(self.inputs[6]))).encode(),
                                          ("bytes=" + str(len(mutant[6]))).encode())
        self.rehash_crossing(mutant, replacement)
        self.refused(mutant)

    def test_consumed_target_records_and_role_labels_required(self):
        for tag in ("OWNER_CONSUMED_TAIL_TARGET", "OWNER_CONSUMED_TAIL_PAYLOAD", "OWNER_PROJECTION_BUDGET"):
            line = tagged(self.inputs[3].decode(), tag)[0]
            for replacement in ("", line + "\n" + line, line + " payload_cells=0"):
                mutant = copy.deepcopy(self.inputs)
                changed = mutant[3].decode().replace(line + "\n", replacement + "\n", 1)
                self.rehash_crossing(mutant, changed.encode())
                self.refused(mutant)
        for old, new in ((b"semantics=last_consumed_target_increment_not_realized_drain", b"semantics=realized_drain"),
                         (b"separated_tail_impulse=unavailable", b"separated_tail_impulse=measured"),
                         (b"legacy_tail_columns=terminal_remaining_demand_not_realized_drain", b"legacy_tail_columns=realized_drain"),
                         (b"payload_offset_bytes=0", b"payload_offset_bytes=4"),
                         (b"nonzero_cells=0", b"nonzero_cells=1")):
            mutant = copy.deepcopy(self.inputs)
            self.rehash_crossing(mutant, mutant[3].replace(old, new, 1))
            self.refused(mutant)

    def test_v2_without_consumed_tail_is_explicitly_superseded(self):
        old = BASE.with_name("crossing_gate.v2")
        cross = Path(str(old) + ".synthetic-crossing")
        values = [Path(str(old) + ".log").read_text(), old.read_bytes(),
                  Path(str(old) + ".csv").read_bytes(), Path(str(cross) + ".convergence.v1").read_bytes(),
                  Path(str(cross) + ".convergence.v1.csv").read_bytes(), self.inputs[5], b""]
        qualify_artifact(*values[:3])
        self.refused(values)


if __name__ == "__main__":
    unittest.main()
