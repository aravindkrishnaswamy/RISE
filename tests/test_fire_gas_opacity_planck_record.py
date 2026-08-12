#!/usr/bin/env python3
"""Strong oracles for the production optically-thin HITEMP record path."""

from __future__ import annotations

import json
import bz2
import gzip
import math
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path


REPO = Path(__file__).resolve().parent.parent
TOOLS = REPO / "tools"
DATA = REPO / "docs/data/gas_opacity"
sys.path.insert(0, str(TOOLS))

from generate_fire_gas_opacity_planck_record import (  # noqa: E402
    INTERPOLATION_ERROR, PRUNING_ERROR, generate, payload,
)
from fetch_verify_hitemp_planck_inputs import (  # noqa: E402
    compile_tool, stream_hitemp_to_reducer,
)


class FireGasOpacityPlanckRecordTest(unittest.TestCase):
    def test_frozen_record_is_reproducible_and_has_no_line_shape_axis(self) -> None:
        encoded, include = generate(DATA / "hitemp_sources_v1.json")
        self.assertEqual(
            encoded, (DATA / "fire_gas_opacity_hitemp_planck_mean_v1.cbor").read_bytes())
        self.assertEqual(
            include,
            (REPO / "src/Library/Utilities/FireGasOpacityRecordData.inc").read_text(
                encoding="utf-8"))
        record = payload(DATA / "hitemp_sources_v1.json")
        self.assertEqual(record["record_status"], "predictive_qualified")
        self.assertNotIn("self_broadening_mole_fractions", record)
        self.assertNotIn("pressure_Pa", record)
        for species in record["species"]:
            interpolation = species["interpolation"]
            self.assertEqual(interpolation["interior_state_policy"],
                             "evaluate_c1_bicubic_never_reject_interior")
            self.assertEqual(species["uncertainty"]["kind"], "assumption_bound")
            self.assertEqual(species["uncertainty"]["magnitude"], 0.05)
            self.assertEqual(
                species["pruning_certificate"]["certified_upper_relative"],
                PRUNING_ERROR[species["species_id"]])
            self.assertAlmostEqual(
                species["interpolation_validation_certificate"]
                ["maximum_observed_relative"],
                INTERPOLATION_ERROR[species["species_id"]],
                delta=INTERPOLATION_ERROR[species["species_id"]] * 2.0e-6)
        citations = {species["species_id"]: species["provenance"]["citation"]
                     for species in record["species"]}
        self.assertIn("10.1016/j.jqsrt.2010.05.001", citations["H2O"])
        self.assertIn("10.1016/j.jqsrt.2024.109324", citations["CO2"])
        self.assertIn("10.1016/j.jms.2023.111748", citations["CO2"])
        self.assertIn("10.1016/j.jqsrt.2019.03.002", citations["CO2"])

    def test_co2_visible_bound_does_not_claim_unmeasured_coverage(self) -> None:
        record = payload(DATA / "hitemp_sources_v1.json")
        co2 = next(item for item in record["species"] if item["species_id"] == "CO2")
        visible = co2["visible_band_certificate"]
        self.assertEqual(visible["measured_wavelength_domain_nm"], [565.0, 780.0])
        self.assertIn("not measured HITEMP coverage", visible["coverage_statement"])
        self.assertEqual(
            visible["dataset_reported_maximum_fraction_of_total_planck_mean"],
            1.522e-10)
        self.assertGreater(
            visible["continuous_upper_absorption_per_m_atm"],
            visible["recomputed_observed_100K_lattice"]
            ["maximum_absorption_per_m_atm"])
        self.assertTrue(visible["exact_line_selection"])

    def test_any_committed_derived_input_mutation_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for source in DATA.iterdir():
                if source.is_file():
                    destination = root / source.name
                    try:
                        destination.symlink_to(source)
                    except OSError:
                        shutil.copy2(source, destination)
            target = root / "hitemp_planck_mean_h2o_v1.txt"
            target.unlink()
            target.write_bytes((DATA / target.name).read_bytes() + b"\n")
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                payload(root / "hitemp_sources_v1.json")

    def test_source_authority_mutation_is_rejected_before_generation(self) -> None:
        manifest = json.loads((DATA / "hitemp_sources_v1.json").read_text(
            encoding="utf-8"))
        manifest["record_status"] = "REVOKED"
        manifest["citations_required"] = {}
        manifest["source_files"]["sha256"].pop(next(iter(
            manifest["source_files"]["sha256"])))
        manifest["source_files"]["sha256"]["foreign.par"] = "0" * 64
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hitemp_sources_v1.json"
            path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "independently adopted identity"):
                payload(path)

    def test_interpolation_certificate_is_recomputed_not_trusted(self) -> None:
        with mock.patch.dict(INTERPOLATION_ERROR, {"H2O": 0.0, "CO2": 0.0}):
            with self.assertRaisesRegex(ValueError, "interpolation validation certificate"):
                payload(DATA / "hitemp_sources_v1.json")

    def test_reducer_consumer_accepts_the_committed_five_column_basis(self) -> None:
        compiler = (shutil.which("c++") or shutil.which("clang++") or
                    shutil.which("g++"))
        if compiler is None:
            self.skipTest("a C++17 command-line compiler is not available")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "hitemp_planck_mean"
            subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Wpedantic",
                str(TOOLS / "hitemp_planck_mean.cpp"), "-o", str(executable),
            ], check=True, capture_output=True, text=True)
            histogram = root / "basis.hist"
            histogram.write_text("1 1.0e-20 100.0 10000.0 1000.0\n", encoding="ascii")
            tips = root / "tips"
            tips.mkdir()
            (tips / "q_iso1.txt").write_text(
                "296 100.0\n300 101.0\n301 101.25\n", encoding="ascii")
            result = subprocess.run([
                str(executable), "--hist", str(histogram), "--tips", str(tips),
                "--out", str(root / "result"), "--tlo", "300", "--thi", "301",
                "--tstep", "1", "--prune", "0",
            ], check=True, capture_output=True, text=True)
            self.assertIn("loaded 1 cells", result.stderr)
            surface = (root / "result_surface.txt").read_text(encoding="ascii")
            self.assertEqual(sum(1 for line in surface.splitlines()
                                 if line and not line.startswith("#")), 4)

    def test_owner_archive_bytes_flow_through_reduction_and_analytic_surface(self) -> None:
        compiler = (shutil.which("c++") or shutil.which("clang++") or
                    shutil.which("g++"))
        if compiler is None:
            self.skipTest("a C++17 command-line compiler is not available")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reducer = root / "hitemp_reduce"
            planck = root / "hitemp_planck_mean"
            compile_tool(compiler, TOOLS / "hitemp_reduce.cpp", reducer)
            compile_tool(compiler, TOOLS / "hitemp_planck_mean.cpp", planck)
            source = (REPO / "tests/fixtures/fire_gas_opacity/"
                      "CO2-HITEMP2024.synthetic-hitran160.txt").read_bytes()
            archive = root / "CO2/02_HITEMP2024.par.bz2"
            archive.parent.mkdir()
            archive.write_bytes(bz2.compress(source))
            manifest = {"source_files": {"sha256": {
                "CO2/02_HITEMP2024.par.bz2": "synthetic-test-digest"}}}
            histogram = root / "co2.hist"
            stream_hitemp_to_reducer(
                manifest, root, "co2", reducer, histogram)
            self.assertIn("# records_read 2", histogram.read_text(encoding="ascii"))
            tips = root / "tips"
            tips.mkdir()
            (tips / "q_iso1.txt").write_text(
                "296 100\n300 101\n350 115\n", encoding="ascii")
            subprocess.run([
                str(planck), "--hist", str(histogram), "--tips", str(tips),
                "--out", str(root / "derived"), "--tlo", "300", "--thi", "350",
                "--tstep", "50", "--prune", "0",
            ], check=True, capture_output=True, text=True)
            surface = (root / "derived_surface.txt").read_text(encoding="ascii")
            self.assertEqual(sum(1 for line in surface.splitlines()
                                 if line and not line.startswith("#")), 4)

    def test_visible_selection_precedes_binning_and_histogram_keys_are_sorted(self) -> None:
        compiler = (shutil.which("c++") or shutil.which("clang++") or
                    shutil.which("g++"))
        if compiler is None:
            self.skipTest("a C++17 command-line compiler is not available")

        def line(wavenumber: float, strength: float, lower_energy: float,
                 molecule: int = 1, gamma_air: float = 0.08,
                 gamma_self: float = 0.12, shift: float = 0.0) -> str:
            prefix = (f"{molecule:2d}1{wavenumber:12.6f}{strength:10.3E}"
                      f"{1.0:10.3E}{gamma_air:5.3f}{gamma_self:5.3f}"
                      f"{lower_energy:10.4f}{0.70:4.2f}{shift:8.6f}")
            return prefix.ljust(160, "S")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reducer = root / "hitemp_reduce"
            compile_tool(compiler, TOOLS / "hitemp_reduce.cpp", reducer)
            boundary = 1.0e7 / 780.0
            # Both first lines occupy the same 25 cm^-1 cell.  The stronger
            # out-of-band line makes that cell's mean nu fall below the band,
            # but exact pre-binning selection must retain the in-band line.
            source = root / "source.par"
            source.write_text("\n".join((
                line(boundary - 0.1, 9.0e-20, 100.0),
                line(boundary + 0.1, 1.0e-20, 100.0),
                line(boundary + 40.0, 1.0e-20, 50.0),
            )) + "\n", encoding="ascii")
            full = root / "full.hist"
            visible = root / "visible.hist"
            visible_tail = root / "visible_tail.hist"
            with source.open("rb") as encoded:
                subprocess.run([
                    str(reducer), "--mol", "1", "--out", str(full),
                    "--visible-out", str(visible), "--visible-lo",
                    format(boundary, ".12g"), "--visible-tail-out", str(visible_tail),
                    "--visible-hi", "26315.7894737",
                ], stdin=encoded, check=True, capture_output=True)
            text = visible.read_text(encoding="ascii")
            self.assertIn("# visible_band_line_count 2", text)
            rows = [row.split() for row in text.splitlines()
                    if row and not row.startswith("#")]
            self.assertEqual([int(row[1]) for row in rows],
                             sorted(int(row[1]) for row in rows))
            first_sum_a = float(rows[0][3])
            self.assertLess(first_sum_a, 2.0e-20)
            outside_source = root / "outside_only.par"
            outside_source.write_text(
                line(boundary - 0.1, 9.0e-20, 100.0) + "\n", encoding="ascii")
            outside_full = root / "outside_full.hist"
            outside_visible = root / "outside_visible.hist"
            outside_tail = root / "outside_tail.hist"
            with outside_source.open("rb") as encoded:
                subprocess.run([
                    str(reducer), "--mol", "1", "--out", str(outside_full),
                    "--visible-out", str(outside_visible), "--visible-tail-out",
                    str(outside_tail), "--visible-lo", format(boundary, ".12g"),
                    "--visible-hi", "26315.7894737",
                ], stdin=encoded, check=True, capture_output=True)
            self.assertIn("# visible_band_line_count 0",
                          outside_visible.read_text(encoding="ascii"))
            outside_tail_rows = [
                row.split() for row in outside_tail.read_text().splitlines()
                if row and not row.startswith("#")]
            self.assertGreater(sum(float(row[2]) for row in outside_tail_rows), 0.0)

            # Isolate the Doppler-only path for the actual lightest CO2
            # isotopologue with a positive pressure shift.  Recover the
            # reducer's probability upper from its weighted amplitude and
            # require it to enclose the exact Gaussian interval probability.
            gaussian_nu = boundary - 0.02
            gaussian_shift = 0.01
            gaussian_strength = 2.0e-20
            gaussian_source = root / "gaussian_only.par"
            gaussian_source.write_text(line(
                gaussian_nu, gaussian_strength, 0.0, molecule=2,
                gamma_air=0.0, gamma_self=0.0, shift=gaussian_shift) + "\n",
                encoding="ascii")
            gaussian_tail = root / "gaussian_tail.hist"
            with gaussian_source.open("rb") as encoded:
                subprocess.run([
                    str(reducer), "--mol", "2", "--out", str(root / "gfull.hist"),
                    "--visible-out", str(root / "gvisible.hist"),
                    "--visible-tail-out", str(gaussian_tail), "--visible-lo",
                    format(boundary, ".12g"), "--visible-hi", "26315.7894737",
                ], stdin=encoded, check=True, capture_output=True)
            weighted = sum(float(row.split()[2]) for row in
                           gaussian_tail.read_text().splitlines()
                           if row and not row.startswith("#"))
            c2 = 1.4387769
            amplitude = gaussian_strength / (1.0 - math.exp(-c2 * gaussian_nu / 296.0))
            stimulated_upper = 1.0 - math.exp(-c2 * gaussian_nu / 300.0)
            probability_upper = weighted / (amplitude * stimulated_upper)
            actual_mass = 43.9898 * 1.66053906660e-27
            sigma = ((gaussian_nu + gaussian_shift) *
                     math.sqrt(1.380649e-23 * 2500.0 /
                               (actual_mass * 299792458.0**2)))
            lower_z = (boundary - (gaussian_nu + gaussian_shift)) / sigma
            upper_z = (26315.7894737 - (gaussian_nu + gaussian_shift)) / sigma
            actual_probability = 0.5 * (math.erf(upper_z / math.sqrt(2.0)) -
                                        math.erf(lower_z / math.sqrt(2.0)))
            self.assertGreater(actual_probability, 0.0)
            self.assertGreaterEqual(probability_upper, actual_probability)

    def test_visible_artifacts_are_identity_pinned_and_exact_line_selected(self) -> None:
        manifest = json.loads((DATA / "hitemp_sources_v1.json").read_text(
            encoding="utf-8"))
        outputs = manifest["outputs"]["derived_artifact_sha256"]
        for species in ("h2o", "co2"):
            basis_name = f"hitemp_visible_basis_{species}_v1.hist.gz"
            tail_name = f"hitemp_visible_tail_basis_{species}_v1.hist.gz"
            certificate_name = f"hitemp_visible_certificate_{species}_v1.json"
            self.assertIn(basis_name, outputs)
            self.assertIn(tail_name, outputs)
            self.assertIn(certificate_name, outputs)
            basis = gzip.decompress((DATA / basis_name).read_bytes()).decode("ascii")
            self.assertIn("# histogram_kind exact_line_center_band", basis)
            tail = gzip.decompress((DATA / tail_name).read_bytes()).decode("ascii")
            self.assertIn("# histogram_kind all_line_voigt_band_probability_upper", tail)
            certificate = json.loads((DATA / certificate_name).read_text())
            self.assertEqual(certificate["schema"],
                             "rise-hitemp-visible-certificate-v1")
            self.assertGreater(certificate["continuous_upper_absorption_per_m_atm"],
                               certificate["observed_100K_lattice"]
                               ["maximum_absorption_per_m_atm"])

    def test_operational_dataset_manifest_retires_voigt_production(self) -> None:
        manifest = (REPO / "docs/FIRE_SIM_DATASET_MANIFEST.md").read_text(
            encoding="utf-8")
        item_five = manifest.split("## Item 5", 1)[1].split("## Item 6", 1)[0]
        self.assertIn("generate_fire_gas_opacity_planck_record.py", item_five)
        self.assertIn("synthetic/research-only", item_five)
        self.assertIn("cannot emit the", item_five)

    def test_source_manifest_pins_all_partition_sum_inputs(self) -> None:
        manifest = json.loads((DATA / "hitemp_sources_v1.json").read_text(
            encoding="utf-8"))
        tips = manifest["tips_2021_source_files"]
        self.assertEqual(len(tips["sha256"]), 18)
        self.assertEqual(len(tips["archive_sha256"]), 64)
        self.assertEqual(manifest["source_files"]["file_count"], 36)
        self.assertEqual(manifest["record_status"],
                         "ADOPTED_SOURCE_PIN_FOR_PREDICTIVE_RECORD")


if __name__ == "__main__":
    unittest.main()
