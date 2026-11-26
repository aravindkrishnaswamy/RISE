#!/usr/bin/env python3
"""HyLIoS measurement CLI contracts and independent analytic controls.

Build bin/tools/hylios_measure first, or set RISE_HYLIOS_MEASURE to another binary.
Standard unittest discovery and command-line selection are preserved.
"""
import hashlib
import json
import math
import os
import signal
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "bin" / "tools" / ("hylios_measure.exe" if os.name == "nt" else "hylios_measure")
BINARY = Path(os.environ.get("RISE_HYLIOS_MEASURE", str(DEFAULT_BINARY))).expanduser().resolve()
MISSING_BINARY = (f"hylios_measure binary is missing: {BINARY}; "
                  "build with make -C build/make/rise build-tool/hylios_measure "
                  "or set RISE_HYLIOS_MEASURE; Windows instructions: docs/HYLIOS_MEASUREMENT.md")
NAMES = ["stratum_corneum", "stratum_granulosum", "stratum_spinosum", "stratum_basale", "papillary_dermis", "reticular_dermis"]


def specimen(mu=0.0):
    return dict(schema_version=1, approximate=True, composition_revision="synthetic-test-v1",
                coverage_profile_id="explicit-synthetic-approximation", spectral_bundle_sha256="a" * 64,
                resolved_specimen_sha256="b" * 64, preset_id="synthetic", interpretation="synthetic mechanics only",
                units=dict(length="m", coefficient="m^-1", wavelength="nm", mass_concentration="kg/m^3"),
                wavelength_nm=[550.0, 1400.0], surface_fold_aspect_ratio=.75,
                provenance={"test": "explicit synthetic source", "nested": {"sha256": "c" * 64}},
                layers=[dict(layer_id=name, thickness_m=.001, host_absorption_m_inv=[mu, mu],
                             real_refractive_index=[1., 1.], constituent_absorption_m_inv={"synthetic": [mu, mu]},
                             particles=None) for name in NAMES])


def active_particle():
    return dict(enabled=True, organelle_fraction=.01, organization="individually_dispersed_particles",
                interior_absorption_evaluated=True, interior_absorption_m_inv=[100., 100.],
                semi_minor_m=1e-7, semi_major_m=1e-7, real_refractive_index=1.)


@unittest.skipUnless(BINARY.is_file(), MISSING_BINARY)
class MeasurementCLI(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hylios-measure-test-")
        self.root = Path(self.temp.name)
        self.source, self.output = self.root / "specimen.json", self.root / "measurement.json"

    def tearDown(self):
        self.temp.cleanup()

    def command(self, **changes):
        values = {"--specimen": str(self.source), "--wavelength-nm": "1400", "--placement": "null",
                  "--boundary": "smooth", "--histories": "100", "--max-transitions": "10000",
                  "--seed": "1234", "--output": str(self.output), "--fiber-volume-fraction": "0"}
        values.update(changes)
        return [str(BINARY)] + [str(v) for pair in values.items() if pair[1] is not None for v in pair]

    def run_case(self, value=None, **changes):
        if value is None:
            value = specimen()
        self.source.write_text(json.dumps(value, sort_keys=True) + "\n")
        result = subprocess.run(self.command(**changes), text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(self.output.read_text())

    def reject_case(self, value, expected, failure_marker="", **changes):
        self.source.write_text(json.dumps(value))
        self.output.write_text("KEEP-EXISTING-OUTPUT")
        result = subprocess.run(self.command(**changes), text=True, capture_output=True, timeout=30)
        self.assertNotEqual(result.returncode, 0, failure_marker)
        self.assertIn(expected, result.stderr)
        self.assertEqual(self.output.read_text(), "KEEP-EXISTING-OUTPUT")
        self.assertEqual(list(self.root.glob("measurement.json.tmp.*")), [])

    def test_vacuum_identity_provenance_hash_and_reproducibility(self):
        source = specimen()
        a = self.run_case(source)
        b = self.run_case(source)
        self.assertEqual(a, b)
        self.assertEqual(a["input_bytes_sha256"], hashlib.sha256(self.source.read_bytes()).hexdigest())
        self.assertFalse(a["claimed_resolved_specimen_sha256_verified"])
        self.assertEqual(a["resolved_specimen_sha256"], source["resolved_specimen_sha256"])
        self.assertEqual(a["provenance"], source["provenance"])
        self.assertTrue(a["approximate"])
        self.assertFalse(a["authored_surface_folds_simulated"])
        self.assertEqual(a["authored_surface_fold_aspect_ratio"], .75)
        self.assertIn("override", a["boundary_law"])
        self.assertTrue(a["complete_physical_estimate"])
        self.assertEqual(a["estimates"]["escaped_reflectance"]["mean"], 1)
        self.assertEqual(a["estimates"]["escaped_reflectance"]["standard_error"], 0)
        self.assertEqual(a["outcomes"]["subsurface_return"]["count"], 100)
        self.assertEqual(sum(a["host_absorbed_weight_by_layer"] + a["particle_absorbed_weight_by_layer"]), 0)
        self.assertEqual(a["aggregate_counters"]["acceptedParticles"], 0)
        self.assertEqual(a["aggregate_counters"]["fiberEvents"], 0)
        self.assertEqual(list(self.root.glob("measurement.json.tmp.*")), [])

    def test_lossy_metadata_integer_tokens_are_rejected_atomically(self):
        for token in ("9007199254740993", "-9007199254740993",
                      "9.007199254740993e15", "9007199254740993.0"):
            with self.subTest(token=token):
                value = specimen()
                value["provenance"]["sequence_id"] = "RAW_NUMERIC_TOKEN"
                self.source.write_text(json.dumps(value).replace('"RAW_NUMERIC_TOKEN"', token))
                self.output.write_text("KEEP-EXISTING-OUTPUT")
                result = subprocess.run(self.command(), text=True, capture_output=True, timeout=30)
                self.assertNotEqual(result.returncode, 0, "LOSSLESS_METADATA_INTEGER")
                self.assertIn("supported JSON numeric magnitude", result.stderr)
                self.assertEqual(self.output.read_text(), "KEEP-EXISTING-OUTPUT")
                self.assertEqual(list(self.root.glob("measurement.json.tmp.*")), [])
                print("PASS: LOSSLESS_METADATA_INTEGER rejects", token)

    def test_invalid_metadata_unicode_is_rejected_atomically(self):
        for invalid in ("\ud800", "\udc00", "\ud800x", "\ud800\u0041"):
            for as_key in (False, True):
                with self.subTest(unicode=repr(invalid), as_key=as_key):
                    value = specimen()
                    if as_key:
                        value["provenance"][invalid] = "invalid key"
                    else:
                        value["provenance"]["path"] = invalid
                    self.reject_case(value, "invalid Unicode", failure_marker="LOSSLESS_METADATA_UNICODE")
                    print("PASS: LOSSLESS_METADATA_UNICODE rejects", repr(invalid), "key", as_key)
        for invalid in (b"\xed\xa0\x80", b"\xc0\xaf", b"\xf4\x90\x80\x80", b"\xff"):
            with self.subTest(raw_utf8=invalid):
                value = specimen()
                value["provenance"]["path"] = "RAW_UTF8_TOKEN"
                self.source.write_bytes(json.dumps(value).encode().replace(b"RAW_UTF8_TOKEN", invalid))
                self.output.write_text("KEEP-EXISTING-OUTPUT")
                result = subprocess.run(self.command(), text=True, capture_output=True, timeout=30)
                self.assertNotEqual(result.returncode, 0, "LOSSLESS_METADATA_UNICODE")
                self.assertIn("invalid Unicode", result.stderr)
                self.assertEqual(self.output.read_text(), "KEEP-EXISTING-OUTPUT")
                self.assertEqual(list(self.root.glob("measurement.json.tmp.*")), [])
                print("PASS: LOSSLESS_METADATA_UNICODE rejects raw", invalid)

    def test_supported_unicode_and_integer_metadata_round_trip(self):
        value = specimen()
        value["provenance"].update(path="café/雪/😀", sequence_id=2 ** 53,
                                   large_identifier_as_string="9007199254740993")
        escaped = self.run_case(value)
        self.assertEqual(escaped["provenance"], value["provenance"])
        self.source.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
        result = subprocess.run(self.command(), text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        raw = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertEqual(raw["provenance"], value["provenance"])
        self.assertEqual(raw["input_bytes_sha256"], hashlib.sha256(self.source.read_bytes()).hexdigest())

    def test_active_particle_resolution_and_strict_label_preservation(self):
        p = specimen()
        p["approximate"] = False
        p["coverage_profile_id"] = "explicit-synthetic-strict-coverage-fixture"
        p["layers"][1]["particles"] = active_particle()
        a = self.run_case(p, **{"--histories": "1000"})
        self.assertFalse(a["approximate"])
        self.assertEqual(a["coverage_profile_id"], p["coverage_profile_id"])
        chosen = a["selected_resolved_layers"][1]["active_individual_spheroids"]
        self.assertEqual(chosen["absorption_m_inv"], 100)
        self.assertAlmostEqual(chosen["nominal_geometric_rate_m_inv"], 75000)
        self.assertGreater(a["aggregate_counters"]["acceptedParticles"], 0)
        self.assertGreater(sum(a["particle_path_length_m_by_layer"]), 0)
        self.assertEqual(sum(v["count"] for v in a["outcomes"].values()), 1000)
        self.assertTrue(a["complete_physical_estimate"])

    def test_all_history_budget_accounting(self):
        a = self.run_case(**{"--histories": "50", "--max-transitions": "1"})
        self.assertFalse(a["complete_physical_estimate"])
        self.assertEqual(a["initial_history_count"], 50)
        self.assertEqual(a["outcomes"]["budget_exceeded"]["count"], 50)
        self.assertEqual(a["outcomes"]["budget_exceeded"]["terminal_live_weight"], 50)
        self.assertEqual(a["estimates"]["escaped_reflectance"]["history_count"], 50)
        self.assertEqual(a["estimates"]["escaped_reflectance"]["mean"], 0)
        self.assertEqual(a["estimates"]["unresolved_live_weight"]["mean"], 1)
        self.assertEqual(a["estimates"]["accounted_weight"]["mean"], 1)
        self.assertEqual(sum(v["count"] for v in a["outcomes"].values()), 50)
        # Partial weighted histories preserve both already-absorbed and live power.
        w = self.run_case(specimen(200), **{"--estimator": "weighted", "--max-transitions": "1"})
        self.assertAlmostEqual(w["estimates"]["unresolved_live_weight"]["mean"], math.exp(-.2), places=14)
        self.assertAlmostEqual(w["estimates"]["physical_absorption"]["mean"], -math.expm1(-.2), places=14)
        self.assertAlmostEqual(w["estimates"]["accounted_weight"]["mean"], 1, places=14)

    def test_independent_beer_bottom_integral_and_roulette(self):
        # Independent midpoint quadrature for normal down / cosine diffuse up.
        tau = .6
        bins = 100000
        expected = math.exp(-tau) * sum(2 * ((i + .5) / bins) * math.exp(-tau / ((i + .5) / bins)) for i in range(bins)) / bins
        for estimator, target in [("analog", "0"), ("weighted", "0"), ("weighted", ".9")]:
            a = self.run_case(specimen(100), **{"--histories": "6000", "--estimator": estimator, "--roulette-target": target})
            e = a["estimates"]["escaped_reflectance"]
            self.assertLessEqual(abs(e["mean"] - expected), 5 * e["standard_error"] + 1e-4)
            accounted = a["estimates"]["accounted_weight"]
            self.assertLessEqual(abs(accounted["mean"] - 1), 5 * accounted["standard_error"] + 1e-4)
            self.assertEqual(a["unresolved_history_count"], 0)
            if target != "0":
                self.assertGreater(a["aggregate_counters"]["rouletteKills"], 0)
                self.assertFalse(a["outcomes"]["roulette_killed"]["unresolved"])
                self.assertEqual(a["estimates"]["unresolved_live_weight"]["mean"], 0)

    def test_missing_coverage_complex_and_inactive_skip(self):
        p = specimen()
        self.reject_case(p, "exact resolved coverage", **{"--wavelength-nm": "551"})
        p["layers"][1]["particles"] = active_particle()
        p["layers"][1]["particles"]["interior_absorption_evaluated"] = False
        self.reject_case(p, "not been evaluated")
        p["layers"][1]["particles"] = active_particle()
        p["layers"][1]["particles"]["organization"] = "complexes"
        self.reject_case(p, "active complexes")
        p["layers"][1]["particles"] = dict(enabled=False, organelle_fraction=0, organization="complexes")
        a = self.run_case(p)
        self.assertIsNone(a["selected_resolved_layers"][1]["active_individual_spheroids"])
        self.assertEqual(a["estimates"]["escaped_reflectance"]["mean"], 1)

    def test_structural_rejections_are_atomic(self):
        cases = []
        p = specimen();p["units"]["length"] = "cm";cases.append((p, "unsupported units"))
        p = specimen();p["schema_version"] = 2;cases.append((p, "unsupported resolved specimen schema"))
        p = specimen();p["layers"].pop();cases.append((p, "exactly six"))
        p = specimen();p["layers"][0]["host_absorption_m_inv"] = [0];cases.append((p, "shape mismatch"))
        p = specimen();p["layers"][0]["constituent_absorption_m_inv"] = {"x": [0]};cases.append((p, "shape mismatch"))
        p = specimen();p["layers"][0]["real_refractive_index"][0] = -1;cases.append((p, "finite supported range"))
        p = specimen();p["wavelength_nm"] = [1400, 1400];cases.append((p, "duplicate wavelength"))
        p = specimen();p["approximate"] = "false";cases.append((p, "must be boolean"))
        p = specimen();p["coverage_profile_id"] = "strict_source_coverage_v1";cases.append((p, "contradicts approximate"))
        p = specimen();p["provenance"]["spectral_bundle"] = {"approximate": False, "profile_id": p["coverage_profile_id"], "bundle_sha256": p["spectral_bundle_sha256"]};cases.append((p, "nested approximation flag"))
        for value, error in cases:
            with self.subTest(error=error):
                self.reject_case(value, error)
        self.source.write_text('{"x": 1, "x": 2}')
        result = subprocess.run(self.command(), capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate JSON key", result.stderr)
        self.source.write_text(json.dumps(specimen()).replace('"schema_version": 1', '"schema_version": 1e9999'))
        result = subprocess.run(self.command(), capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)

    def test_cli_budget_and_explicit_boundary_preflight(self):
        for changes, message in [({"--boundary": None}, "required option"), ({"--boundary": "folded"}, "only explicit"),
                                 ({"--placement": None}, "required option"), ({"--histories": "0"}, "positive"),
                                 ({"--max-transitions": "10000001"}, "supported bound"),
                                 ({"--histories": "10000000", "--max-transitions": "10000000"}, "work budget"),
                                 ({"--seed": "18446744073709551616"}, "supported bound"),
                                 ({"--roulette-target": ".1"}, "requires weighted"),
                                 ({"--fiber-volume-fraction": "2"}, "<=1"),
                                 ({"--estimator": "bogus"}, "estimator must")]:
            with self.subTest(changes=changes):
                self.reject_case(specimen(), message, **changes)
        a = self.run_case(**{"--seed": "18446744073709551615", "--histories": "1"})
        self.assertEqual(a["seed_u64"], "18446744073709551615")
        self.assertIsNone(a["estimates"]["escaped_reflectance"]["standard_error"])
        self.reject_case(specimen(), "paths must differ", **{"--output": str(self.source)})

    @unittest.skipUnless(os.name == "posix", "POSIX signal delivery contract")
    def test_cancellation_accounts_unstarted_histories(self):
        self.source.write_text(json.dumps(specimen()))
        process = subprocess.Popen(self.command(**{"--histories": "1000000"}), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        ready = process.stderr.readline()
        self.assertEqual(ready, "hylios_measure: signal handlers ready\n")
        self.assertIsNone(process.poll(), "ready handshake precedes active measurement")
        process.send_signal(signal.SIGINT)
        out, err = process.communicate(timeout=30)
        self.assertEqual(process.returncode, 130, (out, err))
        value = json.loads(self.output.read_text())
        self.assertEqual(value["initial_history_count"], 1000000)
        cancelled = value["outcomes"]["cancelled"]
        self.assertGreater(cancelled["count"], 0)
        self.assertEqual(cancelled["terminal_live_weight"], cancelled["count"])
        self.assertEqual(sum(v["count"] for v in value["outcomes"].values()), 1000000)
        self.assertEqual(value["estimates"]["escaped_reflectance"]["history_count"], 1000000)
        self.assertAlmostEqual(value["estimates"]["accounted_weight"]["mean"], 1, places=14)
        self.assertFalse(value["complete_physical_estimate"])
        self.assertEqual(sum(value["host_absorbed_weight_by_layer"]+value["particle_absorbed_weight_by_layer"]), 0)

    def test_unique_concurrent_publication(self):
        self.source.write_text(json.dumps(specimen()))
        processes = [subprocess.Popen(self.command(**{"--seed": str(seed)}), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for seed in (71, 81)]
        for p in processes:
            out, err = p.communicate(timeout=30)
            self.assertEqual(p.returncode, 0, (out, err))
        value = json.loads(self.output.read_text())
        self.assertIn(value["seed_u64"], ["71", "81"])
        self.assertEqual(value["outcomes"]["subsurface_return"]["count"], 100)
        self.assertEqual(list(self.root.glob("measurement.json.tmp.*")), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
