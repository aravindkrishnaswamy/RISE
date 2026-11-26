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
import selectors
import time
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


def active_complex(count=1, radius=1e-4, fraction=.05, full_child=True):
    return dict(enabled=True, organelle_fraction=fraction, organization="complexes",
                interior_absorption_evaluated=True, interior_absorption_m_inv=[0., 0.],
                semi_minor_m=radius if full_child else radius/5,
                semi_major_m=radius if full_child else radius/2,
                real_refractive_index=1., complex_diameter_m=2*radius,
                maximum_internal_count=count, encounter_rate_m_inv=3*fraction/(4*radius))


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
                  "--post-scatter": "diagnostic", "--boundary": "smooth", "--histories": "100", "--max-transitions": "10000",
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
        for cell in a["exit_distribution"]["joint_cells"]:
            self.assertEqual(cell["all_returns"]["mean"], 0)
            self.assertEqual(cell["all_returns"]["standard_error"], 0)
            self.assertEqual(cell["all_returns"]["history_count"], 50)
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

    def test_complex_explicit_policy_and_atomic_resolution(self):
        p = specimen();p["layers"][1]["particles"] = active_complex()
        self.reject_case(p, "active complexes require explicit")
        self.reject_case(p, "required option", **{"--complex-layout": "explicit-pack-v1", "--post-scatter": None})
        self.reject_case(p, "complex-layout must be", **{"--complex-layout": "implicit"})
        cases = []
        for field in ("maximum_internal_count", "complex_diameter_m"):
            value = specimen();value["layers"][1]["particles"] = active_complex()
            del value["layers"][1]["particles"][field]
            cases.append((value, "must be a number"))
        for count in (-1, .5, 65, True, "5"):
            value = specimen();value["layers"][1]["particles"] = active_complex()
            value["layers"][1]["particles"]["maximum_internal_count"] = count
            cases.append((value, "complex child count"))
        for diameter, message in ((0, "complex diameter"), (-1, "complex diameter"),
                                  (5e-324, "underflows"),
                                  (1e-4, "unsupported complex dimensions/count"),
                                  (.001, "layer cannot contain positive-measure envelopes")):
            value = specimen();value["layers"][1]["particles"] = active_complex()
            value["layers"][1]["particles"]["complex_diameter_m"] = diameter
            value["layers"][1]["particles"].pop("encounter_rate_m_inv")
            cases.append((value, message))
        value = specimen();value["layers"][1]["particles"] = active_complex(count=2)
        cases.append((value, "unsupported complex dimensions/count"))
        value = specimen();value["layers"][1]["particles"] = active_complex(count=2)
        value["layers"][1]["particles"].update(semi_minor_m=8e-5, semi_major_m=8e-5)
        cases.append((value, "child volume exceeds envelope volume"))
        for declared in (0, 3*.05/(4*2e-5), 3*.05/1e-4):
            value = specimen();value["layers"][1]["particles"] = active_complex()
            value["layers"][1]["particles"]["encounter_rate_m_inv"] = declared
            cases.append((value, "declared particle rate disagrees"))
        for value, error in cases:
            with self.subTest(error=error, particle=value["layers"][1]["particles"]):
                self.reject_case(value, error, **{"--complex-layout": "explicit-pack-v1"})
        pd = specimen();pd["layers"][4]["particles"] = active_complex()
        self.reject_case(pd, "PD complex host gaps with fiber scattering", **{
            "--complex-layout": "explicit-pack-v1", "--fiber-volume-fraction": ".22"})

    def test_complex_full_child_counters_and_provenance(self):
        p = specimen();p["layers"][1]["particles"] = active_complex()
        a = self.run_case(p, **{"--complex-layout": "explicit-pack-v1", "--post-scatter": "discard", "--histories": "500"})
        self.assertEqual(a["measurement_version"], "hylios-forward-measure-v5")
        self.assertEqual(a["complex_layout_policy"], "hylios_complex_explicit_pack_v1")
        self.assertEqual(a["explicit_cli_options"]["--complex-layout"], "explicit-pack-v1")
        convention = a["complex_layout_convention"]
        self.assertEqual(convention["maximum_children"], 64)
        self.assertEqual(convention["proposals_per_child_before_restart"], 256)
        self.assertIsNone(convention["interaction_count_cap"])
        self.assertIn("isotropic", convention["child_axis_prior"])
        chosen = a["selected_resolved_layers"][1]
        self.assertIsNone(chosen["active_individual_spheroids"])
        selected = chosen["active_complex_pack"]
        self.assertEqual(selected["envelope_radius_m"], 1e-4)
        self.assertEqual(selected["actual_child_count"], 1)
        self.assertEqual(selected["count_interpretation"], "resolved_maximum_internal_count_as_actual_children_v1")
        self.assertAlmostEqual(selected["nominal_geometric_rate_m_inv"], 375)
        counters = a["aggregate_counters"]
        accepted = counters["acceptedComplexes"]
        self.assertGreater(accepted, 0)
        # R==child radius gives one centred child filling the envelope. At the
        # matched-index zero-angle endpoint, every accepted envelope has exactly
        # one complete child entry/exit and one five-draw accepted proposal.
        for key in ("acceptedParticles", "complexExits", "complexChildEntries", "complexChildExits", "particleExits", "complexLayoutProposals"):
            self.assertEqual(counters[key], accepted, key)
        for key in ("complexChildReflections", "complexLayoutRestarts", "complexLayoutPairChecks", "perturbationProposals"):
            self.assertEqual(counters[key], 0, key)
        self.assertGreater(a["particle_path_length_m_by_layer"][1], 0)
        self.assertTrue(a["complete_physical_estimate"])
        self.assertEqual(a["estimates"]["escaped_reflectance"]["mean"], 1)
        self.assertEqual(a["estimates"]["physical_absorption"]["mean"], 0)
        self.assertEqual(a["input_bytes_sha256"], hashlib.sha256(self.source.read_bytes()).hexdigest())
        self.assertEqual(a["provenance"], p["provenance"])
        # The declared rate is a checked producer claim, not an alternative
        # proposal clock: omitting it preserves the identical resolved process.
        p["layers"][1]["particles"].pop("encounter_rate_m_inv")
        b = self.run_case(p, **{"--complex-layout": "explicit-pack-v1", "--post-scatter": "discard", "--histories": "500"})
        for key in ("outcomes", "aggregate_counters", "estimates", "random_draw_count"):
            self.assertEqual(a[key], b[key], key)

    def test_complex_five_child_physical_paths_and_work(self):
        p = specimen(100)
        particle = active_complex(count=5, fraction=.015, full_child=False)
        particle.update(interior_absorption_m_inv=[5000., 5000.], real_refractive_index=1.7)
        p["layers"][1]["particles"] = particle
        a = self.run_case(p, **{"--complex-layout": "explicit-pack-v1", "--post-scatter": "discard",
                                "--estimator": "weighted", "--histories": "200", "--max-transitions": "100000"})
        selected = a["selected_resolved_layers"][1]["active_complex_pack"]
        self.assertEqual(selected["actual_child_count"], 5)
        counters = a["aggregate_counters"]
        self.assertGreater(counters["acceptedComplexes"], 0)
        self.assertGreaterEqual(counters["complexLayoutProposals"], 5*counters["acceptedComplexes"])
        self.assertGreater(counters["complexLayoutPairChecks"], 0)
        self.assertGreater(counters["complexChildEntries"], 0)
        self.assertGreater(counters["complexChildReflections"], 0)
        self.assertGreater(sum(a["particle_path_length_m_by_layer"]), 0)
        self.assertGreater(sum(a["particle_absorbed_weight_by_layer"]), 0)
        self.assertGreater(sum(a["host_absorbed_weight_by_layer"]), 0)
        self.assertEqual(a["unresolved_history_count"], 0)
        self.assertTrue(a["complete_physical_estimate"])
        self.assertAlmostEqual(a["estimates"]["accounted_weight"]["mean"], 1, places=12)

    def test_complex_construction_budget_retains_power(self):
        p = specimen();p["layers"][0]["particles"] = active_complex(count=5, fraction=.2, full_child=False)
        p["layers"][0]["particles"]["interior_absorption_m_inv"] = [1e6, 1e6]
        a = self.run_case(p, **{"--complex-layout": "explicit-pack-v1", "--post-scatter": "discard",
                                "--histories": "128", "--max-transitions": "3", "--wavelength-nm": "550"})
        c = a["aggregate_counters"]
        self.assertGreater(c["acceptedComplexes"], 0)
        self.assertEqual(c["complexLayoutProposals"], c["acceptedComplexes"])
        self.assertEqual(c["complexChildEntries"], 0)
        self.assertEqual(c["complexExits"], 0)
        self.assertEqual(c["workUnits"], 3*128)
        self.assertEqual(a["outcomes"]["budget_exceeded"]["count"], 128)
        self.assertEqual(a["estimates"]["unresolved_live_weight"]["mean"], 1)
        self.assertEqual(a["estimates"]["physical_absorption"]["mean"], 0)
        self.assertEqual(sum(a["particle_path_length_m_by_layer"]), 0)
        self.assertFalse(a["complete_physical_estimate"])

    def test_visible_empty_complex_matches_independent_host_integral(self):
        p = specimen(100)
        particle = active_complex(count=0, fraction=.02, full_child=False)
        particle.update(interior_absorption_m_inv=[1e6, 1e6], real_refractive_index=7.)
        p["layers"][1]["particles"] = particle
        bins, tau = 100000, .6
        expected = math.exp(-tau)*sum(2*((i+.5)/bins)*math.exp(-tau/((i+.5)/bins)) for i in range(bins))/bins
        for estimator in ("analog", "weighted"):
            a = self.run_case(p, **{"--complex-layout": "explicit-pack-v1", "--post-scatter": "discard",
                                    "--wavelength-nm": "550", "--estimator": estimator, "--histories": "3000"})
            self.assertGreater(a["aggregate_counters"]["acceptedComplexes"], 0)
            for key in ("complexChildEntries", "complexChildExits", "complexLayoutProposals", "perturbationProposals"):
                self.assertEqual(a["aggregate_counters"][key], 0, key)
            self.assertEqual(sum(a["particle_absorbed_weight_by_layer"]), 0)
            self.assertEqual(sum(a["particle_path_length_m_by_layer"]), 0)
            self.assertEqual(a["host_path_length_m_by_layer"], a["path_length_m_by_layer"])
            escaped = a["estimates"]["escaped_reflectance"]
            self.assertLessEqual(abs(escaped["mean"]-expected), 5*escaped["standard_error"]+1e-4)
            self.assertEqual(a["unresolved_history_count"], 0)
            self.assertTrue(a["complete_physical_estimate"])

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

    def test_explicit_virtual_particle_policy_and_delta_limit(self):
        source = specimen()
        particle = active_particle()
        particle.update(semi_minor_m=5e-5, semi_major_m=5e-5,
                        organelle_fraction=.15, interior_absorption_m_inv=[0., 0.])
        source["layers"][1]["particles"] = particle
        common = {"--histories": "1000", "--max-transitions": "10000"}
        diagnostic = self.run_case(source, **dict(common, **{"--wavelength-nm": "550"}))
        discard = self.run_case(source, **dict(common, **{"--wavelength-nm": "550", "--post-scatter": "discard"}))
        self.assertGreater(diagnostic["outcomes"]["post_exit_reentry"]["count"], 0)
        self.assertGreater(discard["aggregate_counters"]["inwardPostExitEvents"], 0)
        self.assertEqual(discard["outcomes"]["post_exit_reentry"]["count"], 0)
        self.assertEqual(discard["unresolved_history_count"], 0)
        self.assertTrue(discard["complete_physical_estimate"])
        self.assertEqual(discard["post_exit_reentry_policy"], "hylios_discard_after_scatter_v1")
        self.assertEqual(diagnostic["post_exit_reentry_policy"], "hylios_diagnostic_retain_particle_v1")
        self.assertEqual(discard["estimates"]["escaped_reflectance"]["mean"], 1)
        self.assertEqual(sum(discard["host_absorbed_weight_by_layer"] + discard["particle_absorbed_weight_by_layer"]), 0)
        # At the exact zero-angle endpoint the selected policy changes only
        # provenance; physical histories, counters and RNG consumption agree.
        delta_diagnostic = self.run_case(source, **common)
        delta_discard = self.run_case(source, **dict(common, **{"--post-scatter": "discard"}))
        for key in ("outcomes", "aggregate_counters", "estimates", "random_draw_count"):
            self.assertEqual(delta_diagnostic[key], delta_discard[key], key)

    def test_incidence_controls_conventions_and_atomic_validation(self):
        normal = self.run_case()
        explicit = self.run_case(**{"--incident-polar-deg": "0", "--incident-azimuth-deg": "473"})
        for key in ("outcomes", "aggregate_counters", "surface_boundary_counters", "estimates", "random_draw_count", "exit_distribution"):
            self.assertEqual(normal[key], explicit[key], key)
        self.assertEqual(normal["incoming_travel_direction"], [0, 0, 1])
        self.assertEqual(normal["incident_source_direction"], [0, 0, -1])
        self.assertFalse(explicit["incident_azimuth_defined"])
        self.assertEqual(explicit["incident_source_azimuth_degrees"], 0)
        oblique = self.run_case(**{"--incident-polar-deg": "30", "--incident-azimuth-deg": "-90"})
        self.assertEqual(oblique["incident_source_azimuth_degrees"], 270)
        for source, travel in zip(oblique["incident_source_direction"], oblique["incoming_travel_direction"]):
            self.assertEqual(source, -travel)
        self.assertAlmostEqual(oblique["incident_source_direction"][1], -.5, places=14)
        self.assertAlmostEqual(oblique["incident_source_direction"][2], -math.sqrt(.75), places=14)
        for option, value in (("--incident-polar-deg", "-1"), ("--incident-polar-deg", "90"),
                              ("--incident-polar-deg", "nan"), ("--incident-polar-deg", "inf"),
                              ("--incident-azimuth-deg", "nan"), ("--incident-azimuth-deg", "inf")):
            self.reject_case(specimen(), "incident", **{option: value})

    def test_oblique_specular_bin_mapping_and_unknown_measure(self):
        value = specimen()
        for layer in value["layers"]:
            layer["real_refractive_index"] = [1.5, 1.5]
        a = self.run_case(value, **{"--incident-polar-deg": "30", "--incident-azimuth-deg": "22.5",
                                    "--histories": "1", "--max-transitions": "1", "--seed": "48"})
        self.assertEqual(a["outcomes"]["external_reflection"]["count"], 1)
        h = a["exit_distribution"]
        nonzero = [c for c in h["joint_cells"] if c["all_returns"]["mean"]>0]
        self.assertEqual(len(nonzero), 1)
        cell = nonzero[0]
        # Source azimuth22.5 maps to the planar mirror's outgoing202.5,
        # not22.5. cos30 belongs to [.75,.875); reflection exits at radius0.
        self.assertEqual((cell["radial_bin"], cell["azimuth_bin"], cell["cosine_bin"]), (0, 4, 6))
        self.assertEqual(cell["all_returns"]["mean"], 1)
        self.assertEqual(cell["external_reflection"]["mean"], 1)
        self.assertEqual(cell["subsurface_return"]["mean"], 0)
        self.assertTrue(all(c[channel]["standard_error"] is None for c in h["joint_cells"]
                            for channel in ("all_returns", "external_reflection", "subsurface_return")))
        self.assertFalse(h["conditional_on_escape"])
        self.assertFalse(h["is_density"])
        self.assertFalse(h["continuous_pdf_available"])
        self.assertFalse(h["production_bake_available"])
        self.assertIn("unclassified", h["angular_measure_classification"])

    def test_joint_diffuse_bottom_analytic_control_and_measures(self):
        n = 10000
        a = self.run_case(**{"--histories": str(n)})
        h = a["exit_distribution"];radial = h["radial_edges_m"]
        phis, mus = h["outgoing_azimuth_edges_rad"], h["outgoing_cosine_edges"]
        depth = .006
        self.assertEqual(len(h["joint_cells"]), 512)
        self.assertEqual(h["nonzero_cell_updates"], 2*n)
        for cell in h["joint_cells"]:
            r, p, m = cell["radial_bin"], cell["azimuth_bin"], cell["cosine_bin"]
            low = 0 if r==len(radial)-1 else depth/math.hypot(depth, radial[r+1])
            high = depth/math.hypot(depth, radial[r])
            lo, hi = max(low, mus[m]), min(high, mus[m+1])
            expected = max(0, (hi-lo)*(hi+lo))*(phis[p+1]-phis[p])/(2*math.pi)
            observed = cell["all_returns"]["mean"]
            tolerance = 5*math.sqrt(expected*(1-expected)/n)+64*math.ulp(1.)
            self.assertLessEqual(abs(observed-expected), tolerance, (r, p, m, observed, expected))
            self.assertEqual(cell["all_returns"]["history_count"], n)
            self.assertEqual(cell["all_returns"], cell["subsurface_return"])
            self.assertEqual(cell["external_reflection"]["mean"], 0)
            self.assertAlmostEqual(cell["all_returns"]["standard_error"], math.sqrt(observed*(1-observed)/(n-1)), places=14)
            if r==len(radial)-1:
                self.assertIsNone(cell["annular_area_m2"])
                self.assertIsNone(cell["area_solid_angle_m2_sr"])
        self.assertAlmostEqual(sum(c["all_returns"]["mean"] for c in h["joint_cells"]), 1, places=14)
        expected_tail = depth*depth/(depth*depth+radial[-1]*radial[-1])
        tail = h["radial_marginal"][-1]["all_returns"]
        self.assertLessEqual(abs(tail["mean"]-expected_tail), 5*math.sqrt(expected_tail*(1-expected_tail)/n))
        for marginal in h["angular_marginal"]:
            p, m = marginal["azimuth_bin"], marginal["cosine_bin"]
            selected = [c for c in h["joint_cells"] if c["azimuth_bin"]==p and c["cosine_bin"]==m]
            self.assertAlmostEqual(sum(c["all_returns"]["mean"] for c in selected), marginal["all_returns"]["mean"], places=14)
        angular_cells = [c for c in h["joint_cells"] if c["radial_bin"]==0]
        self.assertAlmostEqual(sum(c["solid_angle_sr"] for c in angular_cells), 2*math.pi, places=13)
        self.assertAlmostEqual(sum(c["projected_angle_measure_sr"] for c in angular_cells), math.pi, places=13)

    def test_oblique_bottom_law_rotation_and_uncropped_overflow(self):
        baseline = self.run_case(**{"--histories": "1000"})
        grazing = self.run_case(**{"--histories": "1000", "--incident-polar-deg": "89", "--incident-azimuth-deg": "37"})
        # In an equal-index transparent slab the bottom samples the same global
        # cosine law with the same RNG stream, regardless of incoming direction.
        self.assertEqual(baseline["exit_distribution"]["angular_marginal"], grazing["exit_distribution"]["angular_marginal"])
        self.assertGreater(grazing["exit_distribution"]["radial_marginal"][-1]["all_returns"]["mean"], .9)
        self.assertEqual(grazing["estimates"]["escaped_reflectance"]["mean"], 1)
        self.assertAlmostEqual(sum(c["all_returns"]["mean"] for c in grazing["exit_distribution"]["joint_cells"]), 1, places=14)
        left = self.run_case(**{"--histories": "8000", "--incident-polar-deg": "45", "--incident-azimuth-deg": "0", "--seed": "1234"})
        right = self.run_case(**{"--histories": "8000", "--incident-polar-deg": "45", "--incident-azimuth-deg": "90", "--seed": "10001234"})
        by_index = {(c["radial_bin"], c["azimuth_bin"], c["cosine_bin"]): c for c in right["exit_distribution"]["joint_cells"]}
        for c in left["exit_distribution"]["joint_cells"]:
            other = by_index[c["radial_bin"], (c["azimuth_bin"]+2)%8, c["cosine_bin"]]
            x, y = c["all_returns"], other["all_returns"]
            self.assertLessEqual(abs(x["mean"]-y["mean"]), 5*math.hypot(x["standard_error"], y["standard_error"])+64*math.ulp(1.))

    def test_sparse_histogram_matches_independent_dense_seeded_replay(self):
        def dense(values):
            mean = m2 = 0.
            for count, x in enumerate(values, 1):
                delta = x-mean;mean += delta/count;m2 += delta*(x-mean)
            return mean, math.sqrt(m2/len(values)/(len(values)-1))
        channels = ("all_returns", "external_reflection", "subsurface_return")
        for roulette in ("0", "2"):
            options = {"--histories": "32", "--estimator": "weighted", "--roulette-target": roulette, "--seed": "1234"}
            whole = self.run_case(specimen(20), **options)
            observations = []
            for history in range(32):
                one = self.run_case(specimen(20), **dict(options, **{"--histories": "1", "--seed": str(1234+history)}))
                observations.append(one["exit_distribution"]["joint_cells"])
            if roulette!="0":
                self.assertTrue(any(sum(c["all_returns"]["sum_weight"] for c in one)>1 for one in observations), "weighted/RR fixture actually exercises return weight above one")
            for index, cell in enumerate(whole["exit_distribution"]["joint_cells"]):
                for channel in channels:
                    values = [one[index][channel]["sum_weight"] for one in observations]
                    mean, error = dense(values);actual = cell[channel]
                    self.assertAlmostEqual(actual["mean"], mean, delta=512*math.ulp(1.)*max(1, abs(mean)))
                    self.assertAlmostEqual(actual["standard_error"], error, delta=512*math.ulp(1.)*max(1, abs(error)))
                    self.assertEqual(actual["history_count"], 32)
                    self.assertEqual(actual["nonzero_history_count"], sum(x>0 for x in values))
            for channel, estimate in (("all_returns", "escaped_reflectance"), ("external_reflection", "external_reflection"), ("subsurface_return", "subsurface_return")):
                self.assertAlmostEqual(sum(c[channel]["mean"] for c in whole["exit_distribution"]["joint_cells"]), whole["estimates"][estimate]["mean"], places=13)

    def test_cli_budget_and_explicit_boundary_preflight(self):
        for changes, message in [({"--boundary": None}, "required option"), ({"--boundary": "folded"}, "only explicit"),
                                 ({"--placement": None}, "required option"),
                                 ({"--post-scatter": None}, "required option"),
                                 ({"--post-scatter": "silent"}, "post-scatter must be"), ({"--histories": "0"}, "positive"),
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

    def test_hylios_boundary_macro_mass_and_pinned_reflection(self):
        value = specimen(100000)
        value["surface_fold_aspect_ratio"] = 1.
        for layer in value["layers"]:
            layer["real_refractive_index"] = [1.5, 1.5]
        a = self.run_case(value, **{"--boundary": "hylios2015", "--histories": "10000", "--max-transitions": "100"})
        c = a["surface_boundary_counters"]
        # Independent normal-incidence Fresnel identity. The opaque host
        # absorbs transmitted paths before another boundary, so reflected mass
        # is the initial macro F even though normal rejection is frequent.
        expected = ((1.5-1)/(1.5+1))**2
        observed = a["outcomes"]["external_reflection"]["count"]/10000
        self.assertLessEqual(abs(observed-expected), 5*math.sqrt(expected*(1-expected)/10000))
        self.assertEqual(c["branchAttempts"], 10000)
        self.assertEqual(c["exteriorReflectionBranches"], a["outcomes"]["external_reflection"]["count"])
        self.assertEqual(c["exteriorTransmissionBranches"], a["outcomes"]["physical_absorption"]["count"])
        self.assertGreater(c["normalProposalAttempts"], c["exteriorReflectionBranches"])
        self.assertGreater(c["normalRejections"], 0)
        self.assertEqual(c["normalProposalAttempts"]-c["normalRejections"], c["exteriorReflectionBranches"])
        self.assertEqual(a["caller_work_units"], a["aggregate_counters"]["workUnits"]+c["workUnits"])
        self.assertEqual(c["workUnits"], c["branchAttempts"]+c["normalProposalAttempts"])
        self.assertEqual(a["unresolved_history_count"], 0)
        self.assertTrue(a["authored_surface_folds_simulated"])
        self.assertEqual(a["boundary_law"], "hylios2015_macro_fresnel_normal_perturb_v1")
        self.assertEqual(a["surface_polar_normalization"], 1)
        self.assertAlmostEqual(a["estimates"]["accounted_weight"]["mean"], 1, places=14)

    def test_hylios_zero_fold_preserves_exact_smooth_histories(self):
        value = specimen(40)
        value["surface_fold_aspect_ratio"] = 0.
        for layer in value["layers"]:
            layer["real_refractive_index"] = [1.3, 1.3]
        common = {"--histories": "1000", "--estimator": "weighted", "--max-transitions": "10000"}
        smooth = self.run_case(value, **common)
        folded = self.run_case(value, **dict(common, **{"--boundary": "hylios2015"}))
        for key in ("outcomes", "aggregate_counters", "estimates", "random_draw_count",
                    "host_absorbed_weight_by_layer", "particle_absorbed_weight_by_layer",
                    "host_path_length_m_by_layer", "particle_path_length_m_by_layer"):
            self.assertEqual(smooth[key], folded[key], key)
        self.assertEqual(folded["surface_boundary_counters"]["normalProposalAttempts"], 0)
        self.assertEqual(folded["surface_polar_normalization"], 0)
        self.assertEqual(smooth["caller_work_units"], smooth["aggregate_counters"]["workUnits"])
        self.assertGreater(folded["caller_work_units"], smooth["caller_work_units"])
        self.assertFalse(smooth["authored_surface_folds_simulated"])
        self.assertTrue(folded["authored_surface_folds_simulated"])
        self.assertIn("legacy smooth diagnostic", smooth["boundary_failure_convention"])
        self.assertIn("pending transport cancelled", folded["boundary_failure_convention"])
        # Smooth remains an explicit override and ignores the authored aspect.
        value["surface_fold_aspect_ratio"] = .75
        ignored = self.run_case(value, **common)
        for key in ("outcomes", "aggregate_counters", "estimates", "random_draw_count"):
            self.assertEqual(smooth[key], ignored[key], key)

    def test_hylios_reflected_proposal_budget_is_live_power(self):
        value = specimen();value["surface_fold_aspect_ratio"] = 1.
        for layer in value["layers"]:
            layer["real_refractive_index"] = [1.5, 1.5]
        # Documented SplitMix64 seed48 gives branch U=.0157795 < F=.04,
        # then polar cosine .6633609 < sqrt(.5): a reflected direction in the
        # wrong macro hemisphere. No numerical threshold or timed race is used.
        for cap, proposals in ((1, 0), (2, 1), (3, 2)):
            a = self.run_case(value, **{"--boundary": "hylios2015", "--seed": "48",
                                        "--histories": "1", "--max-transitions": str(cap)})
            c = a["surface_boundary_counters"]
            self.assertEqual(c["branchAttempts"], 1)
            self.assertEqual(c["exteriorReflectionBranches"], 1)
            self.assertEqual(c["exteriorTransmissionBranches"], 0)
            self.assertEqual(c["normalProposalAttempts"], proposals)
            self.assertEqual(a["caller_work_units"], cap)
            self.assertEqual(a["aggregate_counters"]["workUnits"], 0)
            self.assertEqual(a["random_draw_count"], 1+3*proposals)
            self.assertEqual(a["estimates"]["physical_absorption"]["mean"], 0)
            if cap<=2:
                self.assertEqual(a["outcomes"]["budget_exceeded"]["terminal_live_weight"], 1)
                self.assertEqual(c["budgetStops"], 1)
                self.assertFalse(a["complete_physical_estimate"])
                self.assertEqual(a["estimates"]["unresolved_live_weight"]["mean"], 1)
            if cap==2:
                self.assertEqual(c["normalRejections"], 1)
        # Seed0 transmits at the first macro branch. A second caller unit is
        # exactly one weighted host flight, so budget stop must retain its
        # separate already-absorbed and remaining live power.
        weighted = specimen(200)
        for layer in weighted["layers"]:
            layer["real_refractive_index"] = [1.5, 1.5]
        a = self.run_case(weighted, **{"--boundary": "hylios2015", "--seed": "0",
            "--histories": "1", "--estimator": "weighted", "--max-transitions": "2"})
        self.assertEqual(a["caller_work_units"], 2)
        self.assertEqual(a["aggregate_counters"]["workUnits"], 1)
        self.assertEqual(a["surface_boundary_counters"]["exteriorTransmissionBranches"], 1)
        self.assertAlmostEqual(a["estimates"]["unresolved_live_weight"]["mean"], math.exp(-.2), places=14)
        self.assertAlmostEqual(a["estimates"]["physical_absorption"]["mean"], -math.expm1(-.2), places=14)
        self.assertAlmostEqual(a["estimates"]["accounted_weight"]["mean"], 1, places=14)

    def test_hylios_authored_fold_validation_is_atomic(self):
        for fold in (-1, 1e-12, 1e9, True, "0.1", None):
            value = specimen();value["surface_fold_aspect_ratio"] = fold
            self.reject_case(value, "fold", **{"--boundary": "hylios2015"})
        for fold in (.1, .25, .3, .45, 1., math.sqrt(2**-52), 1/math.sqrt(2**-52)):
            # Matched indices guarantee transmission; supported extreme folds
            # therefore validate without requiring an unbounded reflection walk.
            value = specimen();value["surface_fold_aspect_ratio"] = fold
            a = self.run_case(value, **{"--boundary": "hylios2015", "--histories": "10"})
            self.assertTrue(a["complete_physical_estimate"])
            self.assertEqual(a["surface_boundary_counters"]["normalProposalAttempts"], 0)
            self.assertEqual(a["authored_surface_fold_aspect_ratio"], fold)
        # The smooth diagnostic does not silently apply the new working range.
        value = specimen();value["surface_fold_aspect_ratio"] = 1e-12
        a = self.run_case(value)
        self.assertFalse(a["authored_surface_folds_simulated"])

    @unittest.skipUnless(os.name == "posix", "POSIX signal delivery contract")
    def test_hylios_cancel_pinned_surface_branch(self):
        value = specimen();value["surface_fold_aspect_ratio"] = 1/math.sqrt(2**-52)
        for layer in value["layers"]:
            layer["real_refractive_index"] = [1e6, 1e6]
        self.source.write_text(json.dumps(value))
        process = subprocess.Popen(self.command(**{"--boundary": "hylios2015", "--seed": "48",
            "--histories": "100", "--max-transitions": "10000000"}),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            # Read binary pipe chunks so buffered readline cannot hide the
            # second readiness line from select. The deadline bounds failures;
            # it does not decide when to deliver cancellation.
            expected = [b"hylios_measure: signal handlers ready",
                        b"hylios_measure: reflected surface branch pending"]
            buffered = b"";deadline = time.monotonic()+10
            with selectors.DefaultSelector() as ready:
                ready.register(process.stderr, selectors.EVENT_READ)
                while expected:
                    self.assertTrue(ready.select(max(0, deadline-time.monotonic())), "surface readiness deadline exceeded")
                    chunk = os.read(process.stderr.fileno(), 4096)
                    self.assertTrue(chunk, "process ended before reflected-branch readiness")
                    buffered += chunk
                    while b"\n" in buffered and expected:
                        line, buffered = buffered.split(b"\n", 1)
                        self.assertEqual(line, expected.pop(0))
            self.assertIsNone(process.poll(), "pinned reflected branch is still active")
            process.send_signal(signal.SIGINT)
            out, err = process.communicate(timeout=30)
            self.assertEqual(process.returncode, 130, (out, err))
        finally:
            if process.poll() is None:
                process.kill();process.communicate(timeout=30)
        a = json.loads(self.output.read_text())
        self.assertEqual(a["outcomes"]["cancelled"]["count"], 100)
        self.assertEqual(a["outcomes"]["cancelled"]["terminal_live_weight"], 100)
        self.assertEqual(a["surface_boundary_counters"]["branchAttempts"], 1)
        self.assertEqual(a["surface_boundary_counters"]["cancellations"], 1)
        self.assertEqual(a["estimates"]["physical_absorption"]["mean"], 0)
        self.assertEqual(a["estimates"]["accounted_weight"]["mean"], 1)
        self.assertFalse(a["complete_physical_estimate"])

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
        self.assertEqual(value["exit_distribution"]["nonzero_cell_updates"], 2*(value["outcomes"]["external_reflection"]["count"]+value["outcomes"]["subsurface_return"]["count"]))
        for cell in value["exit_distribution"]["joint_cells"]:
            self.assertEqual(cell["all_returns"]["history_count"], 1000000)
        self.assertAlmostEqual(sum(c["all_returns"]["mean"] for c in value["exit_distribution"]["joint_cells"]), value["estimates"]["escaped_reflectance"]["mean"], places=13)
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
