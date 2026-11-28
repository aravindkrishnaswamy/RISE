"""Tests for the non-executing, checksum-pinned HyLIoS data importer."""
from __future__ import annotations

import hashlib
from concurrent.futures import ThreadPoolExecutor
import json
import sys
import tempfile
import unittest
from threading import Barrier
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from hylios_import_data import CURVE_SPECS, ImportErrorDetail, LN10, _entry_map, import_curve, run


class HyLIoSImportDataTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.output = self.root / "output"

    def tearDown(self):
        self.temp.cleanup()

    def entry(self, curve_id: str, script: str, companion: str = "pinned numeric text table\n"):
        files = [f"skin/{curve_id}.m", f"skin/{curve_id}.txt"] if curve_id != "palmer_williams_real_ior" else ["water/sample.m", "water/sample.txt"]
        (self.source / files[0]).parent.mkdir(parents=True, exist_ok=True)
        (self.source / files[0]).write_text(script)
        (self.source / files[1]).write_text(companion)
        digests = [hashlib.sha256((self.source / f).read_bytes()).hexdigest() for f in files]
        return {"id": curve_id, "files": files, "sha256": digests,
                "download_urls": ["https://data.example/" + f for f in files],
                "source": "fixture source"}

    def test_keratin_uses_original_grid_and_dimensional_coefficient(self):
        script = """clear all
data0=[0.1, 0.2, 0.4];
wave0=[245, 250.5, 260];
"""
        entry = self.entry("keratin_abs", script)
        result = import_curve("keratin_abs", entry, self.source)
        self.assertEqual(result["wavelength_nm"], [245.0, 250.5, 260.0])
        self.assertEqual(result["values"], [0.1, 0.2, 0.4])
        self.assertAlmostEqual(result["reference_absorption_coefficient"]["values"][0], LN10 * 0.1 / 4e-6)
        self.assertFalse(result["reference_absorption_coefficient"]["concentration_scaling_applied"])

    def test_dna_applies_exact_point12_normalization_once(self):
        script = """clear all
data0=[1,2,4,8,16,32,64,128,256,512,1024,4];
data0 = data0/data0(12); % normalize to A260 = 1
wave0=[250,251,252,253,254,255,256,257.9556,259,260,261,270];
"""
        entry = self.entry("dna_abs", script)
        result = import_curve("dna_abs", entry, self.source)
        self.assertEqual(result["values"][0:3], [0.25, 0.5, 1.0])
        coeff = result["reference_absorption_coefficient"]["values"]
        self.assertAlmostEqual(coeff[1], LN10 * 0.5 / 0.01)
        self.assertEqual(result["wavelength_nm"][7], 257.9556)

    def test_urocanic_acid_absorbance_to_coefficient(self):
        entry = self.entry("uro_acid_abs", "data0=[0,0.25];\nwave0=[205,210];\n")
        result = import_curve("uro_acid_abs", entry, self.source)
        self.assertAlmostEqual(result["reference_absorption_coefficient"]["values"][1], LN10 * 0.25 / 0.01)

    def test_rejects_unsupported_dataset(self):
        with self.assertRaisesRegex(ImportErrorDetail, "unsupported dataset"):
            import_curve("iapws_water_real_ior", {}, self.source)

    def test_rejects_additional_unsupported_target_assignment(self):
        entry = self.entry("keratin_abs", "data0=[0.1,0.2];\nwave0=[245,250];\ndata0=evil(1);\n")
        with self.assertRaisesRegex(ImportErrorDetail, "unsupported or duplicate assignment to data0"):
            import_curve("keratin_abs", entry, self.source)

    def test_rejects_malformed_number_operators_and_exponents(self):
        for literal in ("1+2", "1-2", "1e+", "--2"):
            with self.subTest(literal=literal):
                entry = self.entry("keratin_abs", f"data0=[{literal},3];\nwave0=[245,250];\n")
                with self.assertRaisesRegex(ImportErrorDetail, "unsupported expression"):
                    import_curve("keratin_abs", entry, self.source)

    def test_rejects_negative_wavelength_empty_and_singleton_arrays(self):
        entry = self.entry("keratin_abs", "data0=[0,0.1];\nwave0=[-1,250];\n")
        with self.assertRaisesRegex(ImportErrorDetail, "wavelengths must be positive"):
            import_curve("keratin_abs", entry, self.source)
        for literal in ("", "245"):
            with self.subTest(literal=literal):
                entry = self.entry("keratin_abs", f"data0=[{literal}];\nwave0=[245,250];\n")
                with self.assertRaises(ImportErrorDetail):
                    import_curve("keratin_abs", entry, self.source)

    def test_rejects_mismatched_manifest_id(self):
        entry = self.entry("keratin_abs", "data0=[0.1,0.2];\nwave0=[245,250];\n")
        entry["id"] = "dna_abs"
        with self.assertRaisesRegex(ImportErrorDetail, "does not match requested"):
            import_curve("keratin_abs", entry, self.source)

    def test_rejects_nonpositive_water_ior(self):
        for value in ("0", "-1"):
            with self.subTest(value=value):
                script = "data=[\n" + "\n".join([value] * 421) + "\n];\n"
                entry = self.entry("palmer_williams_real_ior", script)
                with self.assertRaisesRegex(ImportErrorDetail, "must be positive"):
                    import_curve("palmer_williams_real_ior", entry, self.source)

    def test_rejects_dna_normalization_or_coefficient_overflow(self):
        wave = ",".join(str(200 + i) for i in range(12))
        for data in ([1e308] * 11 + [1e-308], [1e308] * 11 + [1.0]):
            with self.subTest(divisor=data[11]):
                literals = ",".join(repr(x) for x in data)
                script = f"data0=[{literals}];\ndata0=data0/data0(12); % normalize\nwave0=[{wave}];\n"
                entry = self.entry("dna_abs", script)
                with self.assertRaisesRegex(ImportErrorDetail, "non-finite|invalid converted coefficient"):
                    import_curve("dna_abs", entry, self.source)

    def test_batch_failure_writes_no_partial_outputs(self):
        keratin = self.entry("keratin_abs", "data0=[0.1,0.2];\nwave0=[245,250];\n")
        dna_wave = ",".join(str(200 + i) for i in range(12))
        dna_data = ",".join(["1"] * 12)
        dna = self.entry("dna_abs", f"data0=[{dna_data}];\ndata0=data0/data0(12); % normalize\nwave0=[{dna_wave}];\n")
        # Corrupt the second curve's source after pinning it; first curve validates but no file may be written.
        (self.source / dna["files"][0]).write_text("data0=[1,1];\ndata0=data0/data0(12); % normalize\nwave0=[200,201];\n")
        from unittest.mock import patch
        manifest_file = self.root / "manifest.json"
        manifest_file.write_text(json.dumps({"author_data_index":{"items":[keratin,dna]},"author_water_data_index":{"items":[]}}))
        with patch("hylios_import_data.MANIFEST_PATH", manifest_file):
            with self.assertRaisesRegex(ImportErrorDetail, "SHA256 mismatch"):
                run(self.source, self.output, ["keratin_abs", "dna_abs"])
        self.assertFalse(self.output.exists())

    def test_rejects_nonliteral_expression_in_target_array(self):
        entry = self.entry("keratin_abs", "data0=system('bad');\nwave0=[245,250];\n")
        with self.assertRaisesRegex(ImportErrorDetail, "literal data0"):
            import_curve("keratin_abs", entry, self.source)

    def test_rejects_checksum_mismatch(self):
        entry = self.entry("keratin_abs", "data0=[0.1,0.2];\nwave0=[245,250];\n")
        entry["sha256"][0] = "0" * 64
        with self.assertRaisesRegex(ImportErrorDetail, "SHA256 mismatch"):
            import_curve("keratin_abs", entry, self.source)

    def test_rejects_invalid_arrays(self):
        entry = self.entry("keratin_abs", "data0=[0.1,-0.1];\nwave0=[245,250];\n")
        with self.assertRaisesRegex(ImportErrorDetail, "source samples must be nonnegative"):
            import_curve("keratin_abs", entry, self.source)
        entry = self.entry("keratin_abs", "data0=[0.1,0.2,0.3];\nwave0=[245,250];\n")
        with self.assertRaisesRegex(ImportErrorDetail, "lengths differ"):
            import_curve("keratin_abs", entry, self.source)
        entry = self.entry("keratin_abs", "data0=[0.1,0.2];\nwave0=[245,245];\n")
        with self.assertRaisesRegex(ImportErrorDetail, "strictly increasing"):
            import_curve("keratin_abs", entry, self.source)

    def test_molar_extinction_matrix_preserves_source_units_without_concentration(self):
        script = """Mmeth = [ ...
  450.0, 10.5; ...
  455.0, 0; ...
  460.0, 7.25];
"""
        entry = self.entry("methemoglobin", script)
        result = import_curve("methemoglobin", entry, self.source)
        self.assertEqual(result["wavelength_nm"], [450.0, 455.0, 460.0])
        self.assertEqual(result["values"], [10.5, 0.0, 7.25])
        self.assertEqual(result["source_unit"], "cm^-1/(mol/L)")
        self.assertNotIn("reference_absorption_coefficient", result)
        self.assertIn("no ln(10)", result["note"])

    def test_water_absorption_converts_cm_inverse_to_m_inverse_and_keeps_raw(self):
        script = "data=[0,0.0025,1.25];\nwave=[380,385,390];\n"
        entry = self.entry("water_absorption", script)
        result = import_curve("water_absorption", entry, self.source)
        self.assertEqual(result["values"], [0.0, 0.0025, 1.25])
        self.assertEqual(result["source_unit"], "cm^-1")
        self.assertEqual(result["si_absorption_coefficient"]["values"], [0.0, 0.25, 125.0])
        self.assertEqual(result["wavelength_nm"], [380.0, 385.0, 390.0])

    def test_lipid_preserves_irregular_grid_and_does_not_fill_gap_or_tail(self):
        script = "data0=[0.2,0,0.4];\nwave0=[429,1098,1110.2];\n"
        entry = self.entry("lipid_absorption", script)
        result = import_curve("lipid_absorption", entry, self.source)
        self.assertEqual(result["wavelength_nm"], [429.0, 1098.0, 1110.2])
        self.assertEqual(result["values"], [0.2, 0.0, 0.4])
        self.assertEqual(result["provenance"]["maximum_adjacent_interval_nm"], 669.0)
        self.assertEqual(result["provenance"]["tail_policy"], "none; no extrapolation or zero-tail inference")
        self.assertEqual(result["si_absorption_coefficient"]["values"], [20.0, 0.0, 40.0])

    def test_combined_oxy_deoxy_manifest_order_does_not_swap_spectra(self):
        files = [
            "skin/deoxy_hemoglobin_extinction.m", "skin/deoxy_hemoglobin_extinction.txt",
            "skin/oxy_hemoglobin_extinction.m", "skin/oxy_hemoglobin_extinction.txt",
        ]
        scripts = {
            files[0]: "data0=[20,21];\nwave0=[250,255];\n",
            files[2]: "data0=[10,11];\nwave0=[250,255];\n",
        }
        hashes = []
        (self.source / "skin").mkdir(parents=True, exist_ok=True)
        for relative in files:
            content = scripts.get(relative, "pinned companion\n").encode()
            (self.source / relative).write_bytes(content)
            hashes.append(hashlib.sha256(content).hexdigest())
        entry = {"id":"oxy_deoxy_hemoglobin", "files":files, "sha256":hashes,
                 "download_urls":["https://data.example/"+f for f in files]}
        oxy = import_curve("oxy_hemoglobin_extinction", entry, self.source)
        deoxy = import_curve("deoxy_hemoglobin_extinction", entry, self.source)
        self.assertEqual(oxy["values"], [10.0,11.0])
        self.assertEqual(deoxy["values"], [20.0,21.0])

    def test_rejects_malformed_ellipsis_inside_matrix_number(self):
        entry = self.entry("methemoglobin", "Mmeth=[ ...\n 450, 1...2; ...\n 455, 3];\n")
        with self.assertRaisesRegex(ImportErrorDetail, "expected two numeric literals"):
            import_curve("methemoglobin", entry, self.source)

    def test_rejects_unsupported_matrix_expression(self):
        entry = self.entry("methemoglobin", "Mmeth=[ ...\n 450, system('x'); ...\n 455, 1];\n")
        with self.assertRaisesRegex(ImportErrorDetail, "expected two numeric literals"):
            import_curve("methemoglobin", entry, self.source)

    def test_supported_skin_absorbers_have_manifest_pinned_source_entries(self):
        manifest = json.loads((Path(__file__).resolve().parents[1] / "data/hylios/source_manifest.json").read_text())
        entries = _entry_map(manifest)
        absorber_ids = set(CURVE_SPECS) - {"palmer_williams_real_ior"}
        self.assertEqual(len(absorber_ids), 14)
        for curve_id in absorber_ids:
            spec = CURVE_SPECS[curve_id]
            entry = entries[spec.get("manifest_id", curve_id)]
            self.assertEqual(len(entry["files"]), len(entry["sha256"]), curve_id)
            self.assertTrue(any(name.endswith(".m") for name in entry["files"]), curve_id)

    def test_water_real_ior_preserves_declared_400_to_2500_grid(self):
        values = "\n".join("1.33" for _ in range(421))
        script = "data=[\n" + values + "\n];\n"
        entry = self.entry("palmer_williams_real_ior", script)
        result = import_curve("palmer_williams_real_ior", entry, self.source)
        self.assertEqual(len(result["wavelength_nm"]), 421)
        self.assertEqual(result["wavelength_nm"][0], 400.0)
        self.assertEqual(result["wavelength_nm"][-1], 2500.0)
        self.assertNotIn("reference_absorption_coefficient", result)

    def test_run_writes_deterministic_json_files(self):
        entry = self.entry("keratin_abs", "data0=[0.1,0.2];\nwave0=[245,250];\n")
        # Exercise writer using a temporary manifest path without modifying repository data.
        from unittest.mock import patch
        fake_manifest = {"author_data_index":{"items":[entry]},"author_water_data_index":{"items":[]}}
        manifest_file = self.root / "manifest.json"
        manifest_file.write_text(json.dumps(fake_manifest))
        with patch("hylios_import_data.MANIFEST_PATH", manifest_file):
            paths = run(self.source, self.output, ["keratin_abs"])
        content_a = paths[0].read_text()
        with patch("hylios_import_data.MANIFEST_PATH", manifest_file):
            paths = run(self.source, self.output, ["keratin_abs"])
        self.assertEqual(content_a, paths[0].read_text())
        self.assertEqual(json.loads(content_a)["curve_id"], "keratin_abs")

    def writer_fixture_manifest(self):
        entry = self.entry("water_absorption", "data=[0.1,0.2];\nwave=[380,385];\n")
        manifest_file = self.root / "writer-manifest.json"
        manifest_file.write_text(json.dumps({"author_data_index": {"items": [entry]},
                                             "author_water_data_index": {"items": []}}))
        return entry, manifest_file

    def test_concurrent_writers_publish_complete_json_from_unique_temporary_files(self):
        entry, manifest_file = self.writer_fixture_manifest()
        expected = import_curve("water_absorption", entry, self.source)
        barrier = Barrier(2)
        real_replace = Path.replace
        temporary_paths = []
        def synchronized_replace(temporary, destination):
            temporary_paths.append(temporary)
            barrier.wait(timeout=5)
            return real_replace(temporary, destination)
        with patch("hylios_import_data.MANIFEST_PATH", manifest_file), \
             patch.object(Path, "replace", synchronized_replace), \
             ThreadPoolExecutor(max_workers=2) as executor:
            futures = [executor.submit(run, self.source, self.output, ["water_absorption"])
                       for _ in range(2)]
            results = [future.result(timeout=10) for future in futures]
        destination = self.output / "water_absorption.json"
        self.assertEqual(results, [[destination], [destination]])
        self.assertEqual(json.loads(destination.read_text()), expected)
        self.assertEqual(len(set(temporary_paths)), 2)
        self.assertEqual(list(self.output.glob("*.tmp")), [])

    def test_failed_write_or_replace_preserves_existing_output_and_cleans_temporary_file(self):
        _, manifest_file = self.writer_fixture_manifest()
        with patch("hylios_import_data.MANIFEST_PATH", manifest_file):
            run(self.source, self.output, ["water_absorption"])
        destination = self.output / "water_absorption.json"
        original = destination.read_bytes()
        unrelated = destination.with_suffix(".json.tmp")
        unrelated.write_text("unrelated file must survive")
        real_write_text = Path.write_text
        def fail_after_write(path, *args, **kwargs):
            real_write_text(path, *args, **kwargs)
            raise OSError("injected write failure")
        for operation in ("write", "replace"):
            mocked = (patch.object(Path, "write_text", fail_after_write) if operation == "write"
                      else patch.object(Path, "replace", side_effect=OSError("injected replace failure")))
            with self.subTest(operation=operation), \
                 patch("hylios_import_data.MANIFEST_PATH", manifest_file), mocked:
                with self.assertRaisesRegex(OSError, "injected .* failure"):
                    run(self.source, self.output, ["water_absorption"])
            self.assertEqual(destination.read_bytes(), original)
            self.assertEqual(unrelated.read_text(), "unrelated file must survive")
            self.assertEqual(list(self.output.glob("hylios-import-*.tmp")), [])


if __name__ == "__main__":
    unittest.main()
