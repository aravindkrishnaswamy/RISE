"""Unit checks for source evidence transcription (not a scientific model test)."""
import copy
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from check_hylios_sources import MANIFEST, PRESETS, convert_source_value, validate


class HyLIoSSourceEvidenceTest(unittest.TestCase):
    def test_rejects_wrong_source_basis(self):
        changed = copy.deepcopy(self.presets)
        changed["table_iii_fields"]["melanosome_dimensions_um"]["unit"] = "m"
        with self.assertRaisesRegex(ValueError, "source unit must remain"):
            validate(changed, self.manifest)

    def test_rejects_reversed_layer_mapping(self):
        changed = copy.deepcopy(self.presets)
        changed["table_iii_fields"]["melanosome_content_percent_by_epidermal_layer"]["layer_order"].reverse()
        with self.assertRaisesRegex(ValueError, "layer order mismatch"):
            validate(changed, self.manifest)

    def test_rejects_wrong_s2_organization(self):
        changed = copy.deepcopy(self.presets)
        changed["organization_by_preset"]["S2"] = "individual"
        with self.assertRaisesRegex(ValueError, "organization transcription"):
            validate(changed, self.manifest)

    @classmethod
    def setUpClass(cls):
        cls.presets = json.loads(PRESETS.read_text())
        cls.manifest = json.loads(MANIFEST.read_text())

    def test_source_tables_and_conversion_identities(self):
        messages = validate(self.presets, self.manifest)
        self.assertEqual(len(messages), 3)
        # Oracle against published Table III: S3 0.0004 cm stratum corneum = 4e-6 m.
        source_cm = self.presets["table_iii_fields"]["thickness_cm_by_layer"]["values"]["S3"][0]
        converted, unit = convert_source_value(source_cm, "cm")
        self.assertAlmostEqual(converted, 4e-6, places=18)
        self.assertEqual(unit, "m")
        self.assertEqual(convert_source_value(15.0, "percent")[1], "fraction")
        self.assertAlmostEqual(convert_source_value(15.0, "percent")[0], 0.15, places=15)
        self.assertEqual(convert_source_value(0.01, "mol/L"), (10.0, "mol/m^3"))
        self.assertEqual(convert_source_value(0.41, "um")[1], "m")
        self.assertAlmostEqual(convert_source_value(0.41, "um")[0], 4.1e-7, places=20)

    def test_rejects_mutated_s3_thickness(self):
        changed = copy.deepcopy(self.presets)
        changed["table_iii_fields"]["thickness_cm_by_layer"]["values"]["S3"][0] = 0.0005
        with self.assertRaisesRegex(ValueError, "Table III thickness S3\\[0\\]"):
            validate(changed, self.manifest)

    def test_rejects_missing_preset_id(self):
        changed = copy.deepcopy(self.presets)
        del changed["table_iii_fields"]["surface_fold_aspect_ratio"]["values"]["S4"]
        with self.assertRaisesRegex(ValueError, "expected exactly S1-S4"):
            validate(changed, self.manifest)

    def test_rejects_false_full_coverage_claim(self):
        changed = copy.deepcopy(self.manifest)
        changed["curve_inventory"]["coverage_is_certified"] = True
        with self.assertRaisesRegex(ValueError, "must not claim full coverage"):
            validate(self.presets, changed)

    def test_rejects_wrong_table_iv_value(self):
        changed = copy.deepcopy(self.presets)
        changed["table_iv_fixed_source_values"]["stratum_corneum_keratin_content_percent"]["value"] = 65.1
        with self.assertRaisesRegex(ValueError, "Table IV source transcription mismatch"):
            validate(changed, self.manifest)

    def test_preserves_source_unit_and_checks_s4_distinct_values(self):
        fields = self.presets["table_iii_fields"]
        self.assertEqual(fields["melanosome_content_percent_by_epidermal_layer"]["values"]["S4"], [10.0, 10.0, 10.0])
        self.assertEqual(fields["colloidal_melanin_content_percent_by_epidermal_layer"]["values"]["S4"], [15.0, 15.0, 15.0])
        self.assertEqual(fields["colloidal_melanin_content_percent_by_epidermal_layer"]["unit"], "percent")

    def test_rejects_executable_preset_claim(self):
        changed = copy.deepcopy(self.presets)
        changed["not_executable"] = False
        with self.assertRaisesRegex(ValueError, "remain marked not_executable"):
            validate(changed, self.manifest)


if __name__ == "__main__":
    unittest.main()
