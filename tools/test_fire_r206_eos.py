#!/usr/bin/env python3
"""CPU evidence REDs; mutations never alter the published fixture files."""
from pathlib import Path
import unittest
from unittest.mock import patch

from analyze_fire_eos_warm_start import histogram, self_test
from analyze_fire_producer_kernels import fields, summarize
from seal_fire_payload_placement import bind_counters, gate, records
from check_fire_owner_instrumentation import trees


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "rendered/fire_production_calibration/r206_eos"


class EOSGateREDs(unittest.TestCase):
    def test_counter_family_rejects_duplicates_missing_and_unknown_fields(self):
        path = EVIDENCE / "endpoints_qualified_profile_1.v1.log"
        prefix = path.with_suffix("")
        text = path.read_text()
        rows = records(prefix / "budgets/maximum_velocity_trajectory.csv")
        outcome = prefix / "diagnostic_prefix_outcome.v1"
        original_text, original_bytes = Path.read_text, Path.read_bytes
        consumers = {
            "OWNER_COST_PREFIX": lambda: bind_counters(path, rows, outcome),
            "PRODUCER_COMMAND_V1": lambda: (bind_counters(path, rows, outcome), summarize(path)),
            "PRODUCER_KERNEL_V1": lambda: summarize(path),
        }
        for tag, consume in consumers.items():
            consume()
            line = next(line for line in text.splitlines() if line.startswith(tag+" "))
            mutants = [line+" unknown=0", line+" malformed"]
            for key, value in fields(line).items():
                token = key+"="+value
                mutants += [line.replace(token, "", 1), line+" "+key+"=conflict",
                            line.replace(token, key+"=conflict "+token, 1)]
            for mutant in mutants:
                with self.subTest(tag=tag, mutant=mutant):
                    changed = text.replace(line, mutant, 1)
                    with patch.object(Path, "read_text", lambda p: changed if p == path else original_text(p)), \
                         patch.object(Path, "read_bytes", lambda p: changed.encode() if p == path else original_bytes(p)):
                        with self.assertRaises(ValueError):
                            consume()
        import json
        profile_tag = "RISE_FIRE_OWNER_PROFILE_V1 "
        line = next(line for line in text.splitlines() if line.startswith(profile_tag))
        row = json.loads(line[len(profile_tag):])
        for key in row:
            mutants = [line[:-1]+',"'+key+'":0}',
                       profile_tag+json.dumps({k:v for k,v in row.items() if k != key})]
            for mutant in mutants:
                with self.subTest(profile_key=key, mutant=mutant):
                    with self.assertRaises(ValueError):
                        trees(text.replace(line, mutant, 1))
        with self.assertRaises(ValueError):
            trees(text.replace(line, line[:-1]+',"unknown":0}', 1))
        path = EVIDENCE / "warm_iteration_prefix.v1.log"
        text = path.read_text()
        line = next(line for line in text.splitlines() if line.startswith("OWNER_COST_PREFIX "))
        for key, value in fields(line).items():
            token = key+"="+value
            for mutant in (line.replace(token, "", 1), line+" "+key+"=conflict",
                           line.replace(token, key+"=conflict "+token, 1)):
                changed = text.replace(line, mutant, 1)
                with self.subTest(histogram_terminal=key, mutant=mutant):
                    with patch.object(Path, "read_bytes", lambda p: changed.encode() if p == path else original_bytes(p)):
                        with self.assertRaises(ValueError):
                            histogram(path)

    def test_iteration_and_independent_process_evidence(self):
        result = self_test(EVIDENCE)
        self.assertIn("missing_bit_verdict", result["refused"])
        self.assertIn("copied_process_logs", result["refused"])

    def test_owner_requires_each_named_field_and_its_complete_extent(self):
        path = EVIDENCE / "qualification.v1.json.owner.log"
        text = path.read_text()
        gate(path)
        rows = [line for line in text.splitlines()
                if line.startswith("OWNER_SEALING_EQUIVALENCE field=")]
        self.assertEqual(len(rows), 41)
        for row in rows:
            values = dict(word.split("=", 1) for word in row.split()[1:])
            field, words = values["field"], int(values["words"])
            replacements = {
                "renamed": row.replace("field="+field, "field=NOT_"+field),
                "dropped": "",
                "zero": row.replace("words="+str(words), "words=0"),
                "short": row.replace("words="+str(words), "words="+str(words-1)),
                "long": row.replace("words="+str(words), "words="+str(words+1)),
                "mismatch": row.replace("bit_mismatches=0", "bit_mismatches=1"),
                "duplicate_counter": row.replace("words=", "words=0 words="),
                "duplicate_row": row+"\n"+row,
            }
            for name, replacement in replacements.items():
                with self.subTest(field=field, mutation=name):
                    mutant = text.replace(row, replacement, 1)
                    self.assertNotEqual(mutant, text)
                    with patch.object(Path, "read_text", return_value=mutant):
                        with self.assertRaises(ValueError):
                            gate(path)
        for replacement in ("iterations=0/2/4", "iterations=2/2", "iterations=3/2/4"):
            mutant = text.replace("iterations=2/2/4", replacement)
            self.assertNotEqual(mutant, text)
            with patch.object(Path, "read_text", return_value=mutant):
                with self.assertRaises(ValueError):
                    gate(path)
        for prefix in ("OWNER_SEALING_EQUIVALENCE passed=", "PROJECTED_HEUN_METAL_OWNER_FP64 "):
            row = next(line for line in text.splitlines() if line.startswith(prefix))
            for replacement in (row.replace("passed=1", "passed=10"), row+"\n"+row):
                mutant = text.replace(row, replacement, 1)
                with patch.object(Path, "read_text", return_value=mutant):
                    with self.assertRaises(ValueError):
                        gate(path)
        for tag, counter in (("PROJECTED_HEUN_METAL_OWNER_FP64", "passed"),
                             ("OWNER_PUBLICATION_DIGEST_RED", "full_packet_cpu_match"),
                             ("OWNER_SEALING_EQUIVALENCE passed=", "passed")):
            row = next(line for line in text.splitlines() if line.startswith(tag))
            with self.subTest(contradictory_verdict=tag):
                mutant = text + "\n" + row.replace(counter+"=1", counter+"=0") + "\n"
                with patch.object(Path, "read_text", return_value=mutant):
                    with self.assertRaises(ValueError):
                        gate(path)
        for replacement in ("accepted=0 accepted=1", "iterations=0/0/0 iterations=2/2/4"):
            old = "accepted=1" if replacement.startswith("accepted") else "iterations=2/2/4"
            row = next(line for line in text.splitlines() if line.startswith("PROJECTED_HEUN_METAL_OWNER_SMOKE "))
            mutant = text.replace(row, row.replace(old, replacement), 1)
            with patch.object(Path, "read_text", return_value=mutant):
                with self.assertRaises(ValueError):
                    gate(path)


if __name__ == "__main__":
    unittest.main()
