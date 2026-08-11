#!/usr/bin/env python3
"""Strong-oracle tests for the synthetic HITEMP LBL toolchain."""

from __future__ import annotations

import bz2
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
    parse_hitran160, planck_weight_wavenumber, voigt_profile_cm,
)
from generate_fire_gas_opacity_record import generate, load_verified_manifest  # noqa: E402
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

    def test_bzip2_uses_the_same_hitran_parser(self) -> None:
        source = FIXTURE / "CO2-HITEMP2024.synthetic-hitran160.txt"
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "synthetic.bz2"
            archive.write_bytes(bz2.compress(source.read_bytes()))
            lines = list(iter_hitran_lines(archive, "bzip2"))
        self.assertEqual([line.center_cm1 for line in lines], [667.0, 2350.0])

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
                                species["planck_mean_per_m_per_mole_fraction"]
                                for value in row))
            self.assertEqual(len(species["planck_mean_interpolation"][
                "gas_temperature_columns"]), 3)
        h2o = next(entry for entry in generated["species_tables"]
                   if entry["species"] == "H2O")
        self.assertAlmostEqual(h2o["planck_mean_per_m_per_mole_fraction"][0][0],
                               3.544166775034623, places=13)

    def test_em2c_harness_has_constant_opacity_identity_and_row_parser(self) -> None:
        grid = [100.0, 500.0, 1000.0]
        kappa = [0.7, 0.7, 0.7]
        temperature = 900.0
        band_weight = sum(
            0.5 * (grid[index + 1] - grid[index]) *
            (planck_weight_wavenumber(grid[index], temperature) +
             planck_weight_wavenumber(grid[index + 1], temperature))
            for index in range(len(grid) - 1))
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

    def test_no_hitemp_line_archive_is_tracked(self) -> None:
        tracked = subprocess.check_output(
            ["git", "ls-files", "-z"], cwd=REPO).decode("utf-8").split("\0")
        forbidden = [path for path in tracked
                     if path.lower().endswith((".par", ".par.bz2"))]
        self.assertEqual(forbidden, [])


if __name__ == "__main__":
    unittest.main()
