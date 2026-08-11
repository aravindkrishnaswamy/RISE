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
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parent.parent
TOOLS = REPO / "tools"
FIXTURE = REPO / "tests/fixtures/fire_gas_opacity"
sys.path.insert(0, str(TOOLS))

from fire_gas_opacity import (  # noqa: E402
    C2_CM_K, PartitionSums, iter_hitran_lines, line_intensity,
    parse_hitran160, planck_weight_wavenumber, species_spectra_batch,
    spectral_grid, voigt_interval_probability, voigt_profile_cm,
)
from generate_fire_gas_opacity_record import (  # noqa: E402
    evaluate_mixture_planck_mean, evaluate_species_planck_mean, generate,
    generator_identity, load_verified_manifest,
)
from crosscheck_fire_gas_opacity_em2c import homogeneous_emissivity, parse_em2c  # noqa: E402


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
            _, counts, _, _ = species_spectra_batch(
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
        spectra, counts, _, _ = species_spectra_batch(
            one_shot, 1, {1: 0.018010565}, partition, grid,
            [300.0, 1000.0], 101325.0, [0.0, 1.0], [25.0, 50.0], [300.0])
        self.assertEqual(one_shot.passes, 1)
        self.assertTrue(all(value > 0 for rows in counts for row in rows for value in row))
        step = grid[1] - grid[0]
        for cutoff_rows in spectra:
            for self_rows in cutoff_rows:
                areas = [sum(row) * step for row in self_rows]
                self.assertTrue(all(value > 0.0 for value in areas))

        aligned, _, _, _ = species_spectra_batch(
            [lines[0]], 1, {1: 0.018010565}, partition, grid,
            [300.0], 101325.0, [0.0], [25.0])
        shifted_grid = spectral_grid(75.0, 3025.0, 50.0)
        half_cell, _, _, _ = species_spectra_batch(
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
            self.assertTrue(all(value > 0.0 for row in
                                species["planck_mean_per_m_per_unit_species_mole_fraction"]
                                for gas_rows in row for value in gas_rows))
            self.assertEqual(species["planck_mean_interpolation"]["kind"],
                             "tensor_multilinear_exact_cell_bounds_v1")
        h2o = next(entry for entry in generated["species_tables"]
                   if entry["species"] == "H2O")
        self.assertAlmostEqual(
            h2o["planck_mean_per_m_per_unit_species_mole_fraction"][0][0][0],
            0.017490268700836465, places=15)
        self.assertEqual(generated["archive_passes_per_species"], 1)
        self.assertNotEqual(
            h2o["kappa_bin_average_per_m_per_unit_species_mole_fraction"][0],
            h2o["kappa_bin_average_per_m_per_unit_species_mole_fraction"][1])

    def test_operational_evaluator_is_bounded_and_certificate_bound(self) -> None:
        table = generate(FIXTURE / "synthetic_manifest.json", None)
        h2o = next(entry for entry in table["species_tables"]
                   if entry["species"] == "H2O")
        expected = h2o["planck_mean_per_m_per_unit_species_mole_fraction"][0][0][0]
        self.assertEqual(evaluate_species_planck_mean(
            table, "H2O", 0.0, 300.0, 300.0, 101325.0), expected)
        self.assertGreater(evaluate_mixture_planck_mean(
            table, 0.2, 0.1, 900.0, 1100.0, 101325.0), 0.0)
        epsilon = 1.0e-3
        upper = evaluate_species_planck_mean(
            table, "H2O", 0.2, 900.0 + epsilon, 1100.0, 101325.0)
        lower = evaluate_species_planck_mean(
            table, "H2O", 0.2, 900.0 - epsilon, 1100.0, 101325.0)
        derivative = (upper - lower) / (2.0 * epsilon)
        first_cell = next(cell for cell in h2o["planck_mean_interpolation"]["cells"]
                          if cell["lower_indices"] == [0, 0, 1])
        self.assertGreaterEqual(derivative, first_cell["partial_derivative_bounds"][1][0])
        self.assertLessEqual(derivative, first_cell["partial_derivative_bounds"][1][1])
        for arguments in (
                ("H2O", -1e-9, 300.0, 300.0, 101325.0),
                ("H2O", 0.0, 299.0, 300.0, 101325.0),
                ("H2O", 0.0, 300.0, 2001.0, 101325.0),
                ("H2O", 0.0, 300.0, 300.0, 101324.0)):
            with self.assertRaisesRegex(ValueError, "out of domain"):
                evaluate_species_planck_mean(table, *arguments)
        with self.assertRaisesRegex(ValueError, "simplex"):
            evaluate_mixture_planck_mean(table, 0.8, 0.3, 300.0, 300.0, 101325.0)
        mutated = copy.deepcopy(table)
        mutated["species_tables"][0]["planck_mean_interpolation"]["cells"][0][
            "partial_derivative_bounds"][0][0] -= 1.0
        with self.assertRaisesRegex(ValueError, "does not bind"):
            evaluate_species_planck_mean(
                mutated, mutated["species_tables"][0]["species"],
                0.0, 300.0, 300.0, 101325.0)

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
            rows = ["PL[atm-m]   Tg[K]    total-emissivity[dimLESS]"]
            for path_index in range(90):
                path_length = 0.01 * (50.0 / 0.01) ** (path_index / 89.0)
                for temp_index in range(105):
                    rows.append(f"{path_length:.12f} {300.0 + 25.0 * temp_index:.2f} 0.25")
            source.write_text("\n".join(rows) + "\n", encoding="utf-8")
            parsed = parse_em2c(source, verify_digest=False)
            self.assertEqual(len(parsed), 9450)
            self.assertEqual(parsed[0], (300.0, 0.01, 0.25))
            self.assertEqual(parsed[-1][0], 2900.0)

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
            with self.assertRaisesRegex(ValueError, "pending or non-concrete"):
                load_verified_manifest(path, FIXTURE)
            first = Path(directory) / "a.py"
            second = Path(directory) / "b.py"
            first.write_bytes(b"ab")
            second.write_bytes(b"c")
            original = generator_identity([first, second])
            first.write_bytes(b"a")
            second.write_bytes(b"bc")
            self.assertNotEqual(original, generator_identity([first, second]))

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

    def test_no_hitemp_line_archive_is_tracked(self) -> None:
        tracked = subprocess.check_output(
            ["git", "ls-files", "-z"], cwd=REPO).decode("utf-8").split("\0")
        forbidden = [path for path in tracked
                     if path.lower().endswith((".par", ".par.bz2"))]
        self.assertEqual(forbidden, [])


if __name__ == "__main__":
    unittest.main()
