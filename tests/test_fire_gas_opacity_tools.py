#!/usr/bin/env python3
"""Strong-oracle tests for the synthetic HITEMP LBL toolchain."""

from __future__ import annotations

import bz2
import copy
import hashlib
import json
import math
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
from pathlib import Path


REPO = Path(__file__).resolve().parent.parent
TOOLS = REPO / "tools"
FIXTURE = REPO / "tests/fixtures/fire_gas_opacity"
sys.path.insert(0, str(TOOLS))

from fire_gas_opacity import (  # noqa: E402
    C2_CM_K, C_LIGHT, K_BOLTZMANN, N_AVOGADRO, HitranLine, PartitionSums,
    canonical_json_bytes, conservative_voigt_bin_weights,
    finite_path_emissivity_refinement_certificate,
    iter_hitran_lines, line_area_temperature_interval_upper, line_intensity,
    parse_hitran160, planck_weight_wavenumber, species_spectra_batch,
    spectral_grid, tensor_linear_derivative_certificate,
    voigt_interval_probability, voigt_profile_cm,
    voigt_profile_interval_upper,
    voigt_profile_state_cell_upper,
)
from generate_fire_gas_opacity_record import (  # noqa: E402
    EM2C_PATH_LENGTHS_M, EXPECTED_H2O_SEGMENT_NAMES,
    evaluate_mixture_planck_mean, evaluate_species_kappa_bin,
    evaluate_species_planck_mean, generate, generator_identity,
    load_verified_manifest, validate_em2c_path_domain, _exact_axis_index,
    production_resource_budget, validate_opacity_table,
    validate_production_species_inventory,
)
from build_fire_gas_opacity_native import build as build_native  # noqa: E402
from fire_gas_opacity_native import species_spectra_batch_native  # noqa: E402
from fetch_verify_hitemp_inputs import verify_and_generate  # noqa: E402
from crosscheck_fire_gas_opacity_em2c import (  # noqa: E402
    homogeneous_emissivity, parse_em2c, ratio_from_em2c_filename,
    spectrum_at_temperature,
    validate_case_ratio, validate_contraction_limit,
    validate_emissivity_grid_state, validate_relative_tolerance,
)


class FireGasOpacityToolsTest(unittest.TestCase):
    def test_fixed_width_parser_and_reference_intensity(self) -> None:
        text = (FIXTURE / "H2O-HITEMP2010.synthetic-hitran160.txt").read_text(
            encoding="ascii").splitlines()[0]
        self.assertEqual(len(text), 160)
        line = parse_hitran160(text)
        self.assertEqual((line.molecule, line.isotopologue), (1, 1))
        self.assertEqual(line.center_cm1, 1000.0)
        partition = PartitionSums.from_hitran_q_files([
            (1, 1, FIXTURE / "H2O.synthetic-hitran-q.txt")])
        self.assertEqual(line_intensity(line, 296.0, partition),
                         line.intensity_296_cm_per_molecule)
        with self.assertRaisesRegex(ValueError, "out of domain"):
            partition.evaluate(1, 1, 199.0)

    def test_voigt_has_exact_degenerate_and_center_oracles(self) -> None:
        x = 0.37
        gamma = 0.12
        self.assertAlmostEqual(
            voigt_profile_cm(x, 0.0, gamma),
            gamma / (math.pi * (x * x + gamma * gamma)), places=15)
        sigma = 0.3
        y = gamma / (sigma * math.sqrt(2.0))
        exact_center = math.exp(y * y) * math.erfc(y) / (
            sigma * math.sqrt(2.0 * math.pi))
        self.assertLess(abs(voigt_profile_cm(0.0, sigma, gamma) - exact_center) /
                        exact_center, 3.0e-5)
        self.assertAlmostEqual(
            voigt_interval_probability(-0.4, 0.7, 0.0, gamma),
            (math.atan(0.7 / gamma) - math.atan(-0.4 / gamma)) / math.pi,
            places=15)
        self.assertAlmostEqual(
            voigt_interval_probability(-0.4, 0.7, sigma, 0.0),
            0.5 * (math.erf(0.7 / (sigma * math.sqrt(2.0))) -
                   math.erf(-0.4 / (sigma * math.sqrt(2.0)))), places=15)
        intervals = 20000
        step = 1.1 / intervals
        simpson = 0.0
        for index in range(intervals + 1):
            value = voigt_profile_cm(-0.4 + index * step, sigma, gamma)
            simpson += value * (1.0 if index in (0, intervals) else
                                4.0 if index % 2 else 2.0)
        simpson *= step / 3.0
        integrated = voigt_interval_probability(-0.4, 0.7, sigma, gamma)
        self.assertLess(abs(integrated - simpson) / simpson, 3.0e-4)
        narrow_sigma = 0.001
        integrated = voigt_interval_probability(
            -0.4, 0.7, narrow_sigma, gamma)
        intervals = 40000
        step = 1.1 / intervals
        simpson = sum(
            voigt_profile_cm(-0.4 + index * step, narrow_sigma, gamma) *
            (1.0 if index in (0, intervals) else 4.0 if index % 2 else 2.0)
            for index in range(intervals + 1)) * step / 3.0
        self.assertLess(abs(integrated - simpson) / simpson, 3.0e-5)
        self.assertAlmostEqual(
            voigt_interval_probability(25.0, 50.0, 0.1, 0.05),
            0.00031831805653399656, delta=1.0e-12)
        self.assertAlmostEqual(
            voigt_interval_probability(-25.0, 25.0, 0.1, 0.05),
            0.9987267417795154, delta=1.0e-12)
        bound = voigt_profile_interval_upper(0.0, 0.001, 0.08)
        actual_peak = voigt_profile_cm(0.0, 0.001, 0.08)
        self.assertGreaterEqual(bound, actual_peak)

    def test_voigt_tail_certificate_is_a_true_convolution_union_bound(self) -> None:
        grid = spectral_grid(0.05, 20.05, 0.01)
        _, tail_bound = conservative_voigt_bin_weights(
            10.05, 1.0, 1.0, grid, 2.0)
        actual_tail = 1.0 - voigt_interval_probability(-2.0, 2.0, 1.0, 1.0)
        self.assertGreaterEqual(tail_bound, actual_tail)

    def test_visible_bound_covers_interior_temperature_peak(self) -> None:
        line = HitranLine(1, 1, 20000.0, 1.0e-25, 1.0, 0.0, 0.0,
                          2000.0, 0.0, 0.0)
        partition = PartitionSums({
            (1, 1): [(296.0, 296.0), (300.0, 300.0),
                     (1151.0, 1151.0), (3000.0, 3000.0)]})
        molar_mass = 0.018010565
        molecular_mass = molar_mass / N_AVOGADRO
        bound = line_area_temperature_interval_upper(
            line, 300.0, 3000.0, 101325.0, partition) * (
                voigt_profile_state_cell_upper(
                    0.0, line.center_cm1, molecular_mass, line,
                    300.0, 3000.0, 0.0, 1.0, 101325.0))
        interior_temperature = 1151.0
        sigma = line.center_cm1 * math.sqrt(
            K_BOLTZMANN * interior_temperature /
            (molecular_mass * C_LIGHT * C_LIGHT))
        actual = (line_intensity(line, interior_temperature, partition) *
                  101325.0 / (K_BOLTZMANN * interior_temperature) / 1.0e6 *
                  100.0 * voigt_profile_cm(0.0, sigma, 0.0))
        knot_values = []
        for temperature in (300.0, 3000.0):
            knot_sigma = line.center_cm1 * math.sqrt(
                K_BOLTZMANN * temperature /
                (molecular_mass * C_LIGHT * C_LIGHT))
            knot_values.append(
                line_intensity(line, temperature, partition) *
                101325.0 / (K_BOLTZMANN * temperature) / 1.0e6 * 100.0 *
                voigt_profile_cm(0.0, knot_sigma, 0.0))
        self.assertGreater(actual, max(knot_values))
        self.assertGreaterEqual(bound, actual)

    def test_finite_path_emissivity_requires_nonlinear_grid_convergence(self) -> None:
        coarse_grid = [100.0 + 50.0 * index for index in range(8)]
        saturated_line = [[[0.0, 0.0, 0.0, 100.0, 0.0, 0.0, 0.0, 0.0]]]
        rejected = finite_path_emissivity_refinement_certificate(
            coarse_grid, saturated_line, [900.0], [50.0], 0.05, 0.8)
        self.assertFalse(rejected["qualified"])
        with self.assertRaisesRegex(ValueError, "nonlinear h/2h/4h"):
            validate_emissivity_grid_state(
                coarse_grid, saturated_line[0][0], 900.0, 50.0, 0.05, 0.8)

        fine_grid = [100.0 + 0.2 * index for index in range(400)]
        smooth = [[[0.7 for _ in fine_grid]]]
        qualified = finite_path_emissivity_refinement_certificate(
            fine_grid, smooth, [900.0], [0.01, 50.0], 0.05, 0.8)
        self.assertTrue(qualified["qualified"])
        validate_emissivity_grid_state(
            fine_grid, smooth[0][0], 900.0, 50.0, 0.05, 0.8)

    def test_bzip2_uses_the_same_hitran_parser(self) -> None:
        source = FIXTURE / "CO2-HITEMP2024.synthetic-hitran160.txt"
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "synthetic.bz2"
            archive.write_bytes(bz2.compress(source.read_bytes()))
            lines = list(iter_hitran_lines(archive, "bzip2"))
        self.assertEqual([line.center_cm1 for line in lines], [667.0, 2350.0])

    def test_co2_double_digit_isotopologue_codes_decode(self) -> None:
        source = list((FIXTURE / "CO2-HITEMP2024.synthetic-hitran160.txt").read_text(
            encoding="ascii").splitlines()[0])
        decoded = []
        for code, expected in (("0", 10), ("A", 11), ("B", 12)):
            encoded = list(source)
            encoded[2] = code
            decoded.append(parse_hitran160("".join(encoded)))
            self.assertEqual(decoded[-1].isotopologue, expected)
        encoded = list(source)
        encoded[2] = "C"
        with self.assertRaisesRegex(ValueError, "inadmissible"):
            parse_hitran160("".join(encoded))
        with tempfile.TemporaryDirectory() as directory:
            q_entries = []
            for isotopologue in (10, 11, 12):
                path = Path(directory) / f"q-{isotopologue}.txt"
                path.write_text("200 100\n296 120\n3000 700\n", encoding="ascii")
                q_entries.append((2, isotopologue, path))
            partition = PartitionSums.from_hitran_q_files(q_entries)
            _, counts, _, _, _, _, _ = species_spectra_batch(
                decoded, 2, {10: 0.044, 11: 0.045, 12: 0.046}, partition,
                spectral_grid(50.0, 3000.0, 50.0), [300.0], 101325.0,
                [0.0], [50.0])
            self.assertEqual(counts[0][0][0], 3)

    def test_bin_deposition_preserves_line_area_and_single_archive_pass(self) -> None:
        source = FIXTURE / "H2O-HITEMP2010.synthetic-hitran160.txt"
        lines = list(iter_hitran_lines(source, "none"))
        partition = PartitionSums.from_hitran_q_files([
            (1, 1, FIXTURE / "H2O.synthetic-hitran-q.txt")])

        class OneShot:
            def __init__(self, values):
                self.values = values
                self.passes = 0

            def __iter__(self):
                self.passes += 1
                if self.passes != 1:
                    raise AssertionError("archive was scanned more than once")
                return iter(self.values)

        one_shot = OneShot(lines)
        grid = spectral_grid(50.0, 3000.0, 50.0)
        spectra, counts, _, _, _, _, archive_count = species_spectra_batch(
            one_shot, 1, {1: 0.018010565}, partition, grid,
            [300.0, 1000.0], 101325.0, [0.0, 1.0], [25.0, 50.0], [300.0])
        self.assertEqual(one_shot.passes, 1)
        self.assertEqual(archive_count, len(lines))
        self.assertTrue(all(value > 0 for rows in counts for row in rows for value in row))
        step = grid[1] - grid[0]
        for cutoff_rows in spectra:
            for self_rows in cutoff_rows:
                areas = [sum(row) * step for row in self_rows]
                self.assertTrue(all(value > 0.0 for value in areas))

        aligned, _, _, _, _, _, _ = species_spectra_batch(
            [lines[0]], 1, {1: 0.018010565}, partition, grid,
            [300.0], 101325.0, [0.0], [25.0])
        shifted_grid = spectral_grid(75.0, 3025.0, 50.0)
        half_cell, _, _, _, _, _, _ = species_spectra_batch(
            [lines[0]], 1, {1: 0.018010565}, partition, shifted_grid,
            [300.0], 101325.0, [0.0], [25.0])
        self.assertAlmostEqual(sum(aligned[0][0][0]) * step,
                               sum(half_cell[0][0][0]) * step, places=13)

    def test_generator_matches_frozen_synthetic_record(self) -> None:
        manifest = FIXTURE / "synthetic_manifest.json"
        generated = generate(manifest, None)
        frozen = json.loads((REPO / "docs/data/fire_gas_opacity_synthetic_v1.json").read_text(
            encoding="utf-8"))
        self.assertEqual(generated, frozen)
        self.assertEqual(generated["record_status"], "synthetic_test_only")
        self.assertTrue(generated["synthetic"])
        self.assertEqual({entry["species"] for entry in generated["species_tables"]},
                         {"H2O", "CO2"})
        for species in generated["species_tables"]:
            self.assertGreater(species["archive_line_count"], 0)
            self.assertTrue(all(value > 0.0 for row in
                                species["planck_mean_per_m_per_unit_species_mole_fraction"]
                                for gas_rows in row for value in gas_rows))
            self.assertEqual(species["planck_mean_interpolation_diagnostic"]["kind"],
                             "tensor_multilinear_exact_cell_bounds_v1")
            self.assertFalse(species["state_interpolation_diagnostic"][
                "sample_gate_passed"])
            self.assertGreater(
                species["state_interpolation_diagnostic"]["maximum_relative"], 1.0)
            self.assertFalse(species[
                "finite_path_state_interpolation_diagnostic"]["sample_gate_passed"])
        h2o = next(entry for entry in generated["species_tables"]
                   if entry["species"] == "H2O")
        self.assertAlmostEqual(
            h2o["planck_mean_per_m_per_unit_species_mole_fraction"][0][0][0],
            0.017490268700836465, places=15)
        self.assertEqual(generated["archive_passes_per_species"], 1)
        self.assertNotEqual(
            h2o["kappa_bin_average_per_m_per_unit_species_mole_fraction"][0],
            h2o["kappa_bin_average_per_m_per_unit_species_mole_fraction"][1])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "verified-generated.json"
            verify_and_generate(manifest, FIXTURE, output, None)
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")), generated)

    def test_native_accumulator_matches_reference_and_has_production_throughput(self) -> None:
        manifest = FIXTURE / "synthetic_manifest.json"
        reference = generate(manifest, None)
        with tempfile.TemporaryDirectory() as directory:
            library = Path(directory) / ("fire-opacity-native.dll" if sys.platform == "win32"
                                         else "libfire-opacity-native.so")
            build_native(library)
            native = generate(manifest, None, library)
            self.assertEqual(native["accumulator_backend"], "native_streaming_v1")
            for expected_species, actual_species in zip(
                    reference["species_tables"], native["species_tables"]):
                self.assertEqual(expected_species["species"], actual_species["species"])
                self.assertEqual(expected_species["line_counts_used"],
                                 actual_species["line_counts_used"])
                self.assertAlmostEqual(
                    expected_species[
                        "visible_380_780nm_conservative_upper_m-1_per_unit_species_mole_fraction"],
                    actual_species[
                        "visible_380_780nm_conservative_upper_m-1_per_unit_species_mole_fraction"],
                    delta=1.0e-12 * max(1.0, expected_species[
                        "visible_380_780nm_conservative_upper_m-1_per_unit_species_mole_fraction"]))
                expected = expected_species[
                    "planck_mean_per_m_per_unit_species_mole_fraction"]
                actual = actual_species[
                    "planck_mean_per_m_per_unit_species_mole_fraction"]
                for self_index in range(len(expected)):
                    for gas_index in range(len(expected[self_index])):
                        for radiation_index in range(len(expected[self_index][gas_index])):
                            self.assertAlmostEqual(
                                expected[self_index][gas_index][radiation_index],
                                actual[self_index][gas_index][radiation_index],
                                delta=1.0e-7 * max(
                                    1.0, expected[self_index][gas_index][radiation_index]))

            raw = (FIXTURE / "H2O-HITEMP2010.synthetic-hitran160.txt").read_bytes(
                ).splitlines(keepends=True)[0]
            archive = Path(directory) / "production-scale-sample.txt"
            archive.write_bytes(raw * 100000)
            partition = PartitionSums.from_hitran_q_files([
                (1, 1, FIXTURE / "H2O.synthetic-hitran-q.txt")])
            source = {"molecule_number": 1,
                      "files": [{"path": str(archive), "compression": "none"}]}
            started = time.perf_counter()
            species_spectra_batch_native(
                library, source, Path(directory), {1: 0.018010565}, partition,
                spectral_grid(50.0, 3000.0, 50.0), [300.0, 1000.0, 2000.0],
                101325.0, [0.0, 1.0], [50.0, 100.0],
                [300.0, 1000.0, 2000.0], [1.0e7 / 780.0, 1.0e7 / 380.0])
            throughput = 100000.0 / (time.perf_counter() - started)
            self.assertGreater(throughput, 25000.0)

    def test_operational_evaluator_is_bounded_and_certificate_bound(self) -> None:
        table = generate(FIXTURE / "synthetic_manifest.json", None)
        h2o = next(entry for entry in table["species_tables"]
                   if entry["species"] == "H2O")
        expected = h2o["planck_mean_per_m_per_unit_species_mole_fraction"][0][0][0]
        with self.assertRaisesRegex(ValueError, "synthetic opacity table"):
            evaluate_species_planck_mean(
                table, "H2O", 0.0, 300.0, 300.0, 101325.0)
        self.assertEqual(evaluate_species_planck_mean(
            table, "H2O", 0.0, 300.0, 300.0, 101325.0,
            allow_synthetic=True), expected)
        self.assertGreater(evaluate_mixture_planck_mean(
            table, 0.2, 0.1, 900.0, 1100.0, 101325.0,
            allow_synthetic=True), 0.0)
        epsilon = 1.0e-3
        upper = evaluate_species_planck_mean(
            table, "H2O", 0.2, 900.0 + epsilon, 1100.0, 101325.0,
            allow_synthetic=True)
        lower = evaluate_species_planck_mean(
            table, "H2O", 0.2, 900.0 - epsilon, 1100.0, 101325.0,
            allow_synthetic=True)
        derivative = (upper - lower) / (2.0 * epsilon)
        first_cell = next(cell for cell in h2o[
            "planck_mean_interpolation_diagnostic"]["cells"]
                          if cell["lower_indices"] == [0, 0, 1])
        self.assertGreaterEqual(derivative, first_cell["partial_derivative_bounds"][1][0])
        self.assertLessEqual(derivative, first_cell["partial_derivative_bounds"][1][1])
        for arguments in (
                ("H2O", -1e-9, 300.0, 300.0, 101325.0),
                ("H2O", 0.0, 299.0, 300.0, 101325.0),
                ("H2O", 0.0, 300.0, 2001.0, 101325.0),
                ("H2O", 0.0, 300.0, 300.0, 101324.0)):
            with self.assertRaisesRegex(ValueError, "out of domain"):
                evaluate_species_planck_mean(table, *arguments, allow_synthetic=True)
        with self.assertRaisesRegex(ValueError, "simplex"):
            evaluate_mixture_planck_mean(
                table, 0.8, 0.3, 300.0, 300.0, 101325.0,
                allow_synthetic=True)
        self.assertGreaterEqual(evaluate_species_kappa_bin(
            table, "H2O", 0.2, 900.0, table["spectral_grid_cm-1"][20],
            101325.0, allow_synthetic=True), 0.0)
        with self.assertRaisesRegex(ValueError, "spectral coordinate"):
            evaluate_species_kappa_bin(
                table, "H2O", 0.2, 900.0,
                math.nextafter(table["spectral_bin_edge_domain_cm-1"][0], -math.inf),
                101325.0, allow_synthetic=True)
        mutated = copy.deepcopy(table)
        mutated["species_tables"][0]["planck_mean_interpolation_diagnostic"]["cells"][0][
            "partial_derivative_bounds"][0][0] -= 1.0
        with self.assertRaisesRegex(ValueError, "canonical identity mismatch"):
            evaluate_species_planck_mean(
                mutated, mutated["species_tables"][0]["species"],
                0.0, 300.0, 300.0, 101325.0, allow_synthetic=True)
        recertified = copy.deepcopy(table)
        target = recertified["species_tables"][0]
        target["planck_mean_per_m_per_unit_species_mole_fraction"][0][0][0] *= 2.0
        target["planck_mean_interpolation_diagnostic"] = tensor_linear_derivative_certificate(
            [recertified["self_broadening_mole_fractions"],
             recertified["gas_temperatures_K"],
             recertified["radiation_temperatures_K"]],
            target["planck_mean_per_m_per_unit_species_mole_fraction"])
        with self.assertRaisesRegex(ValueError, "canonical identity mismatch"):
            evaluate_species_planck_mean(
                recertified, target["species"], 0.0, 300.0, 300.0,
                101325.0, allow_synthetic=True)
        relabeled = copy.deepcopy(table)
        relabeled["synthetic"] = False
        relabeled["record_status"] = "production_derived"
        payload = dict(relabeled)
        payload.pop("canonical_payload_without_identity_sha256")
        relabeled["canonical_payload_without_identity_sha256"] = hashlib.sha256(
            (json.dumps(payload, sort_keys=True, separators=(",", ":"),
                        ensure_ascii=False, allow_nan=False) + "\n").encode()).hexdigest()
        with self.assertRaisesRegex(ValueError, "non-operational"):
            evaluate_species_planck_mean(
                relabeled, "H2O", 0.0, 300.0, 300.0, 101325.0)

    def test_em2c_harness_has_constant_opacity_identity_and_row_parser(self) -> None:
        grid = [100.0, 550.0, 1000.0]
        kappa = [0.7, 0.7, 0.7]
        temperature = 900.0
        step = grid[1] - grid[0]
        band_weight = step * sum(planck_weight_wavenumber(value, temperature)
                                 for value in grid)
        total_weight = (math.pi ** 4 / 15.0) * (temperature / C2_CM_K) ** 4
        self.assertAlmostEqual(
            homogeneous_emissivity(grid, kappa, temperature, 0.4),
            (1.0 - math.exp(-0.7 * 0.4)) * band_weight / total_weight, places=15)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "em2c.dat"
            rows = ["iPL    PL[atm-m]"]
            rows.extend(f"{index + 1} {path_length:.12f}"
                        for index, path_length in enumerate(EM2C_PATH_LENGTHS_M))
            rows.append("PL[atm-m]   Tg[K]    total-emissivity[dimLESS]")
            for path_index in range(90):
                path_length = 0.01 * (50.0 / 0.01) ** (path_index / 89.0)
                for temp_index in range(105):
                    rows.append(f"{path_length:.5f} {300.0 + 25.0 * temp_index:.2f} 0.25")
            source.write_text("\n".join(rows) + "\n", encoding="utf-8")
            parsed = parse_em2c(source, verify_digest=False)
            self.assertEqual(len(parsed), 9450)
            self.assertEqual(parsed[0], (300.0, 0.01, 0.25))
            self.assertEqual(parsed[-1][0], 2900.0)
            rows[1 + 45] = f"46 {EM2C_PATH_LENGTHS_M[45] * 1.1:.12f}"
            source.write_text("\n".join(rows) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "grid is invalid"):
                parse_em2c(source, verify_digest=False)
        self.assertEqual(ratio_from_em2c_filename(Path(
            "R=01.000_EM2C-SNB_totalEmissivities_90x105.dat")), 1.0)
        self.assertTrue(math.isinf(ratio_from_em2c_filename(Path(
            "R=Infinity_EM2C-SNB_totalEmissivities_90x105.dat"))))
        with self.assertRaisesRegex(ValueError, "does not match"):
            validate_case_ratio(1.0, Path(
                "R=00.000_EM2C-SNB_totalEmissivities_90x105.dat"))
        for invalid in (math.nan, math.inf, -math.inf, 0.0, -0.1, 1.1):
            with self.assertRaisesRegex(ValueError, "finite"):
                validate_relative_tolerance(invalid)
        for invalid in (math.nan, math.inf, -math.inf, 0.0, -0.1, 1.0):
            with self.assertRaisesRegex(ValueError, "finite"):
                validate_contraction_limit(invalid)

    def test_wrong_hash_and_owner_pending_fail_closed(self) -> None:
        manifest = json.loads((FIXTURE / "synthetic_manifest.json").read_text())
        manifest["sources"][0]["files"][0]["sha256"] = "0" * 64
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                load_verified_manifest(path, FIXTURE)
        with self.assertRaisesRegex(ValueError, "owner-download pending"):
            load_verified_manifest(
                REPO / "docs/data/source_pulls/hitemp_owner_inputs_v1.pending.json", None)

    def test_production_rejects_placeholder_citations_and_identity_is_framed(self) -> None:
        manifest = json.loads((FIXTURE / "synthetic_manifest.json").read_text())
        manifest["status"] = "production_pinned"
        manifest["synthetic"] = False
        manifest["original_source_citations"] = [
            "original-source citations distributed with owner download"]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad-production.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "production generation is retired"):
                load_verified_manifest(path, FIXTURE)
            first = Path(directory) / "a.py"
            second = Path(directory) / "b.py"
            first.write_bytes(b"ab")
            second.write_bytes(b"c")
            original = generator_identity([first, second])
            first.write_bytes(b"a")
            second.write_bytes(b"bc")
            self.assertNotEqual(original, generator_identity([first, second]))

    def test_pressure_and_partition_sum_provenance_fail_closed(self) -> None:
        manifest = json.loads((FIXTURE / "synthetic_manifest.json").read_text())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "invalid.json"
            manifest["pressure_Pa"] = 202650.0
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "exactly 101325"):
                generate(path, FIXTURE)

            manifest["pressure_Pa"] = 101325.0
            manifest["sources"][0]["partition_sums"]["citation"] = ""
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "partition-sum citation"):
                load_verified_manifest(path, FIXTURE)

            manifest["status"] = "production_pinned"
            manifest["synthetic"] = False
            manifest["hitemp_citations"] = [
                "HITEMP 2024 release, https://hitran.org/hitemp/"]
            manifest["original_source_citations"] = [
                "Rothman et al. 2010, DOI 10.1016/j.jqsrt.2010.05.001"]
            manifest["sources"][0]["partition_sums"]["citation"] = "PENDING"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "production generation is retired"):
                load_verified_manifest(path, FIXTURE)

    def test_production_inventory_and_em2c_path_grid_are_independently_pinned(self) -> None:
        h2o = {
            "species": "H2O",
            "release": "HITEMP2010",
            "files": [{"path": name, "compression": "none"}
                      for name in sorted(EXPECTED_H2O_SEGMENT_NAMES)],
        }
        validate_production_species_inventory(h2o, set(range(1, 7)))
        missing = copy.deepcopy(h2o)
        missing["files"].pop()
        with self.assertRaisesRegex(ValueError, "34-segment"):
            validate_production_species_inventory(missing, set(range(1, 7)))
        duplicated = copy.deepcopy(h2o)
        duplicated["files"][-1] = copy.deepcopy(duplicated["files"][0])
        with self.assertRaisesRegex(ValueError, "34-segment"):
            validate_production_species_inventory(duplicated, set(range(1, 7)))
        with self.assertRaisesRegex(ValueError, "34-segment"):
            validate_production_species_inventory(h2o, {1})

        validate_em2c_path_domain(EM2C_PATH_LENGTHS_M)
        with self.assertRaisesRegex(ValueError, "90-point EM2C"):
            validate_em2c_path_domain([0.01, 1.0, 50.0])
        with self.assertRaisesRegex(ValueError, "axis knots only"):
            _exact_axis_index([300.0, 1000.0], 700.0)
        self.assertTrue(production_resource_budget(600, 2, 3)["within_budget"])
        self.assertFalse(production_resource_budget(150000, 10, 105)["within_budget"])

    def test_production_shaped_native_flow_enforces_counts_and_axis_knots(self) -> None:
        manifest_path = FIXTURE / "synthetic_manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["status"] = "production_pinned"
        manifest["synthetic"] = False
        manifest["hitemp_citations"] = [
            "Rothman et al. 2010, DOI 10.1016/j.jqsrt.2010.05.001"]
        manifest["original_source_citations"] = [
            "HITEMP original sources, https://hitran.org/hitemp/"]
        for source in manifest["sources"]:
            source["partition_sums"]["citation"] = (
                "Gamache et al. 2021, DOI 10.1016/j.jqsrt.2021.107713")
        with tempfile.TemporaryDirectory() as directory:
            retired_manifest = Path(directory) / "retired-production.json"
            retired_manifest.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "production generation is retired"):
                load_verified_manifest(retired_manifest, FIXTURE)
            with mock.patch(
                    "generate_fire_gas_opacity_record.load_verified_manifest",
                    return_value=(manifest, FIXTURE)):
                with self.assertRaisesRegex(ValueError, "production generation is retired"):
                    generate(retired_manifest, FIXTURE)
            retired_table = json.loads(
                (REPO / "docs/data/fire_gas_opacity_synthetic_v1.json").read_text())
            retired_table["synthetic"] = False
            retired_table["record_status"] = "production_derived"
            with self.assertRaisesRegex(ValueError, "non-operational"):
                validate_opacity_table(retired_table)

    def test_empty_or_foreign_line_archives_fail_closed(self) -> None:
        manifest = json.loads((FIXTURE / "synthetic_manifest.json").read_text())
        for source in manifest["sources"]:
            for item in source["files"]:
                item["path"] = str(FIXTURE / item["path"])
            for item in source["partition_sums"]["files"]:
                item["path"] = str(FIXTURE / item["path"])
        with tempfile.TemporaryDirectory() as directory:
            empty = Path(directory) / "empty.txt"
            empty.write_bytes(b"")
            h2o_file = manifest["sources"][0]["files"][0]
            h2o_file["path"] = str(empty)
            h2o_file["sha256"] = hashlib.sha256(b"").hexdigest()
            manifest_path = Path(directory) / "empty-manifest.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "contributes no lines"):
                generate(manifest_path, None)

            foreign = FIXTURE / "CO2-HITEMP2024.synthetic-hitran160.txt"
            h2o_file["path"] = str(foreign)
            h2o_file["sha256"] = hashlib.sha256(foreign.read_bytes()).hexdigest()
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "foreign molecule"):
                generate(manifest_path, None)

    def test_declared_grid_and_wing_convergence_gates_are_operational(self) -> None:
        manifest = json.loads((FIXTURE / "synthetic_manifest.json").read_text())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tight.json"
            manifest["maximum_planck_mean_grid_relative_error"] = 1.0e-12
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "spectral grid fails"):
                generate(path, FIXTURE)
            manifest["maximum_planck_mean_grid_relative_error"] = 0.05
            manifest["maximum_planck_mean_wing_relative_error"] = 1.0e-12
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "line-wing comparison fails"):
                generate(path, FIXTURE)
            manifest["maximum_planck_mean_wing_relative_error"] = 0.05
            manifest[
                "maximum_visible_gas_absorption_m-1_per_unit_species_mole_fraction"] = 1.0e-12
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "visible-gas upper-bound"):
                generate(path, FIXTURE)

    def test_no_hitemp_line_archive_is_tracked(self) -> None:
        tracked = subprocess.check_output(
            ["git", "ls-files", "-z"], cwd=REPO).decode("utf-8").split("\0")
        forbidden = [path for path in tracked
                     if path.lower().endswith((".par", ".par.bz2"))]
        self.assertEqual(forbidden, [])


if __name__ == "__main__":
    unittest.main()
