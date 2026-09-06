#!/usr/bin/env python3
"""CPU evidence REDs; mutations never alter the published fixture files."""
from pathlib import Path
import unittest
from unittest.mock import patch

from analyze_fire_eos_warm_start import self_test
from seal_fire_payload_placement import gate


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "rendered/fire_production_calibration/r206_eos"


class EOSGateREDs(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
