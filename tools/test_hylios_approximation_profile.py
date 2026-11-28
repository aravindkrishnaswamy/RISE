"""Analytic contract tests for explicitly labelled spectral approximations.

Synthetic spectra provide exact oracles; these are not physiological validation.
No network or licensed source data are required to run this suite.
"""
from __future__ import annotations

import copy
import json
import math
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

try:
    from .hylios_approximation_profile import (PROFILE_PATH, ApproximationProfile,
                                              ImportErrorDetail, build_profile, bundle_fingerprint, write_bundle)
except ImportError:
    from hylios_approximation_profile import (PROFILE_PATH, ApproximationProfile,
                                             ImportErrorDetail, build_profile, bundle_fingerprint, write_bundle)


def fixture_bundle():
    profile = json.loads(PROFILE_PATH.read_text())
    support = {"keratin_abs": (240, 421), "dna_abs": (200, 360), "uro_acid_abs": (200, 327),
               "eumelanin_extinction": (210, 820), "pheomelanin_extinction": (213, 829),
               "oxy_hemoglobin_extinction": (250, 1000), "deoxy_hemoglobin_extinction": (250, 1000),
               "carboxyhemoglobin": (450, 650), "methemoglobin": (450, 700),
               "sulfhemoglobin": (450, 750), "beta_carotene": (220, 700),
               "bilirubin": (240, 700), "water_absorption": (380, 2500),
               "lipid_absorption": (429, 2449.5), "palmer_williams_real_ior": (400, 2500)}
    curves = {}
    for key, spec in profile["curves"].items():
        lo, hi = support[key]
        curve = {"schema_version": 1, "curve_id": key,
                 "source_quantity": spec["source_quantity"], "source_unit": spec["source_unit"],
                 "reference_basis": spec["reference_basis"],
                 "wavelength_nm": [lo, (lo + hi) / 2, hi], "values": [2.0, 3.0, 4.0],
                 "provenance": {"source_files": copy.deepcopy(spec["source_files"])}}
        if key == "palmer_williams_real_ior":
            curve["values"] = [1.346, 1.3, 1.268]
        # Keep an auxiliary conversion to prove it is not used/reconverted.
        curve["reference_absorption_coefficient"] = {"values": [100.0, 200.0, 300.0]}
        curves[key] = curve
    subs = {}
    for key, spec in profile["substitution_sources"].items():
        values = [1e-8, 1e-8, 1e-8] if key.endswith("extinction") else [1.4, 1.35, 1.3]
        subs[key] = {"curve_id": key, "source_quantity": spec["source_quantity"],
                     "source_unit": spec["source_unit"], "reference_basis": "synthetic constant",
                     "wavelength_nm": [200, 400, 2500], "values": values,
                     "provenance": {"source_files": copy.deepcopy(spec["source_files"])}}
    bundle = {"schema_version": 1, "approximate": True, "profile_id": profile["profile_id"],
              "profile": profile, "curves": curves, "substitution_curves": subs}
    bundle["bundle_sha256"] = bundle_fingerprint(bundle)
    return bundle


def seal_for_structural_test(bundle):
    # Structural tests deliberately modify a synthetic bundle; recompute its
    # content fingerprint so those tests reach the intended independent check.
    bundle["bundle_sha256"] = bundle_fingerprint(bundle)
    return ApproximationProfile(bundle)


class ApproximationProfileTest(unittest.TestCase):
    def setUp(self):
        self.bundle = fixture_bundle()
        self.evaluator = seal_for_structural_test(self.bundle)

    def test_all_source_knots_in_domain_are_exact_and_raw_basis_is_unchanged(self):
        for key, curve in self.bundle["curves"].items():
            for wave, expected in zip(curve["wavelength_nm"], curve["values"]):
                if not 250 <= wave <= 2500:
                    continue
                with self.subTest(curve=key, wavelength=wave):
                    result = self.evaluator.evaluate_with_provenance(key, wave)
                    self.assertEqual(result["value"], expected)
                    self.assertEqual(result["source_unit"], curve["source_unit"])
                    self.assertEqual(result["source_quantity"], curve["source_quantity"])
                    self.assertEqual(result["reference_basis"], curve["reference_basis"])
                    self.assertEqual(result["evaluation_trace"][0]["origin"], "source_sample")
                    self.assertIs(result["approximate"], True)

    def test_irregular_grid_interpolation_and_no_unit_conversion(self):
        key = "beta_carotene"
        self.bundle["curves"][key]["wavelength_nm"] = [220, 530, 700]
        self.bundle["curves"][key]["values"] = [10, 72, 140]
        evaluator = seal_for_structural_test(self.bundle)
        self.assertEqual(evaluator.evaluate(key, 375), 41)
        self.assertEqual(evaluator.evaluate(key, 615), 106)

    def test_melanin_tail_power_law_anchor_and_ratios(self):
        for key, power in (("eumelanin_extinction", -3.33), ("pheomelanin_extinction", -4.75)):
            endpoint = self.bundle["curves"][key]["wavelength_nm"][-1]
            with self.subTest(curve=key):
                self.assertEqual(self.evaluator.evaluate(key, endpoint), 4.0)
                self.assertAlmostEqual(self.evaluator.evaluate(key, 2 * endpoint), 4.0 * 2.0 ** power)
                self.assertAlmostEqual(self.evaluator.evaluate(key, 2500), 4.0 * (2500 / endpoint) ** power)

    def test_exponential_efold_is_explicit_and_continuous_at_anchor(self):
        for key, anchor in (("beta_carotene", 700), ("bilirubin", 700),
                            ("oxy_hemoglobin_extinction", 1000), ("deoxy_hemoglobin_extinction", 1000)):
            with self.subTest(curve=key):
                self.assertEqual(self.evaluator.evaluate(key, anchor), 4)
                self.assertAlmostEqual(self.evaluator.evaluate(key, anchor + 100), 4 / math.e)
                result = self.evaluator.evaluate_with_provenance(key, 2500)
                self.assertEqual(result["evaluation_trace"][0]["policy"]["e_fold_nm"], 100)
                self.assertIn("arbitrary", result["evaluation_trace"][0]["policy"]["rationale"])

    def test_explicit_zero_uv_tails_differ_from_nonzero_measured_endpoints(self):
        for key in ("keratin_abs", "dna_abs", "uro_acid_abs"):
            endpoint = self.bundle["curves"][key]["wavelength_nm"][-1]
            with self.subTest(curve=key):
                self.assertEqual(self.evaluator.evaluate(key, endpoint), 4)
                result = self.evaluator.evaluate_with_provenance(key, endpoint + 1)
                self.assertEqual(result["value"], 0)
                self.assertEqual(result["evaluation_trace"][0]["origin"], "approximation")
                self.assertIn("not evidence", result["evaluation_trace"][0]["policy"]["rationale"])

    def test_lipid_endpoint_hold_in_both_directions(self):
        self.assertEqual(self.evaluator.evaluate("lipid_absorption", 250), 2)
        self.assertEqual(self.evaluator.evaluate("lipid_absorption", 429), 2)
        self.assertEqual(self.evaluator.evaluate("lipid_absorption", 2449.5), 4)
        self.assertEqual(self.evaluator.evaluate("lipid_absorption", 2500), 4)

    def test_dyshemoglobin_proxy_uses_endpoint_ratio_and_recursive_upper_policy(self):
        # Oxy fixture is the affine function 2 + (wavelength-250)/375.
        for key, endpoint in (("carboxyhemoglobin", 650), ("methemoglobin", 700), ("sulfhemoglobin", 750)):
            proxy_at_endpoint = 2 + (endpoint - 250) / 375
            with self.subTest(curve=key):
                self.assertAlmostEqual(self.evaluator.evaluate(key, 250), 2 * 2 / (2 + 200 / 375))
                self.assertEqual(self.evaluator.evaluate(key, endpoint), 4)
                self.assertAlmostEqual(self.evaluator.evaluate(key, 1100), 4 * (4 / math.e) / proxy_at_endpoint)
                result = self.evaluator.evaluate_with_provenance(key, 1100)
                self.assertTrue(any(event.get("policy", {}).get("kind") == "exponential"
                                    for event in result["evaluation_trace"]))
                expected_sources = dict(self.bundle["curves"][key]["provenance"]["source_files"])
                expected_sources.update(self.bundle["curves"]["oxy_hemoglobin_extinction"]["provenance"]["source_files"])
                self.assertEqual(result["source_files"], expected_sources)

    def test_water_k_uv_conversion_and_continuous_multiplicative_anchor(self):
        # Constant k => absorption proportional to 1/lambda. Anchoring at 380
        # cancels amplitude and 4*pi but must keep the wavelength dependence.
        self.assertEqual(self.evaluator.evaluate("water_absorption", 380), 2)
        self.assertAlmostEqual(self.evaluator.evaluate("water_absorption", 250), 2 * 380 / 250)
        self.assertAlmostEqual(self.evaluator.evaluate("water_absorption", 379), 2 * 380 / 379)
        self.assertAlmostEqual(self.evaluator.evaluate("water_absorption", 380 - 1e-7), 2, places=8)

    def test_real_index_uv_additive_splice(self):
        # Hale-Querry fixture: n(250)=1.3875, n(400)=1.35; offset=-0.004.
        self.assertAlmostEqual(self.evaluator.evaluate("palmer_williams_real_ior", 250), 1.3875 - 0.004)
        self.assertEqual(self.evaluator.evaluate("palmer_williams_real_ior", 400), 1.346)
        self.assertAlmostEqual(self.evaluator.evaluate("palmer_williams_real_ior", 400 - 1e-7), 1.346, places=8)

    def test_domain_nonfinite_and_unknown_queries_fail_closed(self):
        for wave in (249.999, 2500.001, -1, float("nan"), float("inf"), True, "500"):
            with self.subTest(wavelength=wave), self.assertRaises(ImportErrorDetail):
                self.evaluator.evaluate("water_absorption", wave)
        with self.assertRaisesRegex(ImportErrorDetail, "unknown curve"):
            self.evaluator.evaluate("invented_species", 500)

    def test_both_domain_endpoints_have_labelled_finite_values_for_every_species(self):
        for key in self.bundle["curves"]:
            for wave in (250, 2500):
                with self.subTest(curve=key, wavelength=wave):
                    result = self.evaluator.evaluate_with_provenance(key, wave)
                    self.assertTrue(math.isfinite(result["value"]))
                    self.assertGreaterEqual(result["value"], 0)
                    self.assertIs(result["approximate"], True)
                    self.assertEqual(result["profile_id"], "hylios_cleanroom_approximation_v1")
                    self.assertTrue(result["source_files"])
                    self.assertIn("Unvalidated", result["validation_status"])

    def test_rejects_mislabelled_or_incomplete_bundle(self):
        mutations = [lambda b: b.update(approximate=False),
                     lambda b: b["profile"].update(approximate=False),
                     lambda b: b.update(profile_id="different-profile"),
                     lambda b: b["curves"].pop("lipid_absorption"),
                     lambda b: b["curves"]["beta_carotene"].update(source_unit="m^-1"),
                     lambda b: b["curves"]["beta_carotene"]["provenance"].update(source_files={}),
                     lambda b: b["profile"]["curves"]["beta_carotene"]["above"].update(kind="invented")]
        for mutation in mutations:
            bundle = copy.deepcopy(self.bundle)
            mutation(bundle)
            with self.subTest(mutation=mutation), self.assertRaises(ImportErrorDetail):
                seal_for_structural_test(bundle)

    def test_proxy_cycles_wrong_bases_and_zero_anchors_fail_closed(self):
        bundle = copy.deepcopy(self.bundle)
        bundle["profile"]["curves"]["carboxyhemoglobin"]["below"]["proxy_curve_id"] = "carboxyhemoglobin"
        with self.assertRaisesRegex(ImportErrorDetail, "cyclic"):
            seal_for_structural_test(bundle)
        bundle = copy.deepcopy(self.bundle)
        bundle["profile"]["curves"]["carboxyhemoglobin"]["below"]["proxy_curve_id"] = "lipid_absorption"
        with self.assertRaisesRegex(ImportErrorDetail, "same source basis"):
            seal_for_structural_test(bundle)
        bundle = copy.deepcopy(self.bundle)
        bundle["curves"]["oxy_hemoglobin_extinction"]["values"] = [0, 0, 0]
        with self.assertRaisesRegex(ImportErrorDetail, "zero-valued proxy"):
            seal_for_structural_test(bundle)

    def test_bad_grid_and_nonphysical_samples_fail_closed(self):
        for values in ([1, -1, 2], [1, float("nan"), 2], [1, float("inf"), 2]):
            bundle = copy.deepcopy(self.bundle)
            bundle["curves"]["water_absorption"]["values"] = values
            with self.subTest(values=values), self.assertRaises(ImportErrorDetail):
                seal_for_structural_test(bundle)
        bundle = copy.deepcopy(self.bundle)
        bundle["curves"]["water_absorption"]["wavelength_nm"] = [380, 380, 2500]
        with self.assertRaisesRegex(ImportErrorDetail, "strictly increasing"):
            seal_for_structural_test(bundle)

    def test_private_evaluation_state_is_not_mutated_by_caller_or_result(self):
        expected = self.evaluator.evaluate("eumelanin_extinction", 1600)
        self.bundle["curves"]["eumelanin_extinction"]["values"][-1] = 999
        result = self.evaluator.evaluate_with_provenance("eumelanin_extinction", 1600)
        result["evaluation_trace"][0]["policy"]["exponent"] = 0
        self.assertEqual(self.evaluator.evaluate("eumelanin_extinction", 1600), expected)

    def test_stale_fingerprint_rejects_array_and_policy_changes(self):
        for field in ("value", "policy", "fingerprint"):
            bundle = copy.deepcopy(self.bundle)
            if field == "value":
                bundle["curves"]["eumelanin_extinction"]["values"][-1] += 1
            elif field == "policy":
                bundle["profile"]["curves"]["eumelanin_extinction"]["above"]["exponent"] = -2
            else:
                bundle.pop("bundle_sha256")
            with self.subTest(field=field), self.assertRaisesRegex(ImportErrorDetail, "fingerprint mismatch"):
                ApproximationProfile(bundle)

    def test_identity_is_readonly_and_provenance_is_detached(self):
        with self.assertRaises(AttributeError):
            self.evaluator.profile_id = "relabeled"
        with self.assertRaises(AttributeError):
            self.evaluator.bundle_sha256 = "relabeled"
        identity = self.evaluator.provenance()
        self.assertEqual(identity["bundle_sha256"], bundle_fingerprint(self.bundle))
        self.assertEqual(identity["profile_id"], self.bundle["profile_id"])
        self.assertIn("not original bytes", identity["source_verification"])
        identity["sources"]["water_absorption"]["source_files"].clear()
        identity["tail_policies"]["water_absorption"]["below"]["kind"] = "zero"
        self.assertTrue(self.evaluator.provenance()["sources"]["water_absorption"]["source_files"])
        self.assertEqual(self.evaluator.provenance()["tail_policies"]["water_absorption"]["below"]["kind"],
                         "water_k_substitution")
        result = self.evaluator.evaluate_with_provenance("water_absorption", 250)
        self.assertEqual(result["bundle_sha256"], self.bundle["bundle_sha256"])

    def test_write_roundtrip_preserves_raw_and_auxiliary_arrays_without_resampling(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bundle.json"
            original = copy.deepcopy(self.bundle)
            write_bundle(self.bundle, path)
            loaded = json.loads(path.read_text())
            self.assertEqual(loaded, original)
            self.assertEqual(loaded["bundle_sha256"], bundle_fingerprint(original))
            self.assertEqual(ApproximationProfile(loaded).evaluate("water_absorption", 250),
                             self.evaluator.evaluate("water_absorption", 250))

    def test_unique_write_preserves_unrelated_tmp_and_cleans_up_on_publication_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "bundle.json"
            unrelated = output.with_suffix(".json.tmp")
            unrelated.write_text("unrelated existing file")
            write_bundle(self.bundle, output)
            self.assertEqual(unrelated.read_text(), "unrelated existing file")
            original = output.read_bytes()
            module = write_bundle.__module__
            with patch(f"{module}.os.replace", side_effect=OSError("simulated publication failure")):
                with self.assertRaisesRegex(OSError, "simulated publication failure"):
                    write_bundle(self.bundle, output)
            self.assertEqual(output.read_bytes(), original)
            self.assertEqual(unrelated.read_text(), "unrelated existing file")
            self.assertEqual(list(Path(temporary).glob("hylios-profile-*.tmp")), [])

    def test_invalid_write_and_missing_cache_create_no_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "missing" / "bundle.json"
            self.bundle["approximate"] = False
            with self.assertRaises(ImportErrorDetail):
                write_bundle(self.bundle, output)
            self.assertFalse(output.parent.exists())
            with self.assertRaisesRegex(ImportErrorDetail, "missing pinned source"):
                build_profile(Path(temporary))
            self.assertFalse(output.parent.exists())


if __name__ == "__main__":
    unittest.main()
