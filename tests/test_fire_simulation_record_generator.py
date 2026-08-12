#!/usr/bin/env python3
"""Strong arithmetic gates for the r51 physical methane record generator."""

from __future__ import annotations

import json
import sys
import unittest
from fractions import Fraction
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import generate_fire_simulation_records as records  # noqa: E402


def decoded_rational(value: dict) -> Fraction:
    return Fraction(int(value["numerator"]), int(value["denominator"]))


def decoded_matrix(value: dict) -> list[list[Fraction]]:
    return [[decoded_rational(entry) for entry in row]
            for row in value["entries"]]


class MethaneRecordGeneratorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.snapshot_path = ROOT / "docs/data/source_pulls/fire_sim_open_sources_v1.json"
        cls.snapshot = json.loads(cls.snapshot_path.read_text(encoding="utf-8"))
        cls.record = records.methane_payload(cls.snapshot)

    def test_physical_species_and_no_owner_gated_condensable(self) -> None:
        self.assertEqual(list(records.METHANE_SPECIES), self.record["species_order"])
        self.assertEqual("physical_fuel_preset", self.record["record_class"])
        self.assertEqual("none", self.record["condensable_stream"]["kind"])
        self.assertFalse(
            self.record["condensable_stream"]["owner_gated_thermochemistry_required"])
        encoded = json.dumps(self.record, sort_keys=True).lower()
        self.assertNotIn("levoglucosan", encoded)
        self.assertNotIn("owner_gated_missing_record", encoded)

    def test_lhv_and_primary_reaction_are_exact_source_arithmetic(self) -> None:
        source = {entry["name"]: entry
                  for entry in self.snapshot["nasa_cea"]["thermochemistry"]}
        hf = {name: Fraction(str(source[name]["formation_enthalpy_J_per_kmol_298p15K"]))
              for name in ("CH4", "CO2", "H2O")}
        molecular_weight = Fraction(str(source["CH4"]["molecular_weight_kg_per_kmol"]))
        expected = -(hf["CO2"] + 2 * hf["H2O"] - hf["CH4"]) / molecular_weight
        self.assertEqual(expected, decoded_rational(
            self.record["lower_heating_value_J_per_kg"]["exact_rational"]))
        reaction = self.record["primary_reaction"]
        self.assertTrue(all(reaction["admissibility_proof"].values()))
        self.assertEqual(Fraction(0), sum(
            (decoded_rational(value)
             for value in reaction["constituent_delta_kg_per_kg_fuel"]),
            Fraction(0)))

    def test_real_A_and_C_have_exact_certified_ranks(self) -> None:
        for key, expected_rank in (
                ("conservative_reconstruction_v1", 4),
                ("nonadvective_flux_projection_v1", 5)):
            certificate = self.record[key]
            matrix = decoded_matrix(certificate["constraint_matrix"])
            basis = decoded_matrix(certificate["exact_nullspace"]["basis"])
            self.assertEqual(expected_rank, certificate["declared_rank"])
            self.assertEqual(len(matrix[0]) - expected_rank, len(basis[0]))
            self.assertEqual(
                [[Fraction(0) for _ in range(len(basis[0]))]
                 for _ in range(len(matrix))],
                records.matrix_multiply(matrix, basis))
            factor = certificate["rank_factorization"]
            self.assertEqual(matrix, records.matrix_multiply(
                decoded_matrix(factor["left"]), decoded_matrix(factor["right"])))
            projector = decoded_matrix(certificate["exact_projector"])
            self.assertEqual(projector, records.matrix_multiply(projector, projector))
            self.assertEqual(basis, records.matrix_multiply(projector, basis))

    def test_generated_include_is_current(self) -> None:
        generated = records.generate(self.snapshot_path)
        committed = (ROOT / "src/Library/Utilities/FireSimulationRecordData.inc").read_text(
            encoding="utf-8")
        self.assertEqual(committed, generated)
        self.assertIn("kFireSimMethanePhysicalV1SHA256", generated)

    def test_contrived_red_closures_are_distinct_nonpreset_records(self) -> None:
        fixtures = records.solver_fixture_payloads()
        self.assertEqual(2, len(fixtures))
        self.assertEqual(2, len({payload["record_name"] for _, payload in fixtures}))
        for _, payload in fixtures:
            self.assertEqual("synthetic_verification_fixture", payload["record_class"])
            self.assertIn("V-tier RED testing only", payload["applicability"])
            self.assertTrue(payload["expected_outcome"].startswith("reject_"))
        near_rank = fixtures[0][1]
        near_certificate = near_rank["candidate_certificate"]
        matrix = decoded_matrix(near_certificate["constraint_matrix"])
        self.assertEqual(1, near_certificate["declared_rank"])
        self.assertNotEqual(Fraction(0), records.matrix_determinant(matrix))
        factor = near_certificate["rank_factorization"]
        self.assertNotEqual(matrix, records.matrix_multiply(
            decoded_matrix(factor["left"]), decoded_matrix(factor["right"])))
        wrong = fixtures[1][1]
        wrong_certificate = wrong["candidate_certificate"]
        wrong_matrix = decoded_matrix(wrong_certificate["constraint_matrix"])
        numerical_basis = wrong_certificate["orthonormal_nullspace"]["entries"]
        product = [[sum(float(wrong_matrix[row][column]) *
                        numerical_basis[column][basis]
                        for column in range(len(wrong_matrix[0])))
                    for basis in range(len(numerical_basis[0]))]
                   for row in range(len(wrong_matrix))]
        self.assertTrue(any(value != 0.0 for row in product for value in row))
        self.assertEqual(
            records.math.nextafter(128.0 * records.sys.float_info.epsilon,
                                   records.math.inf),
            abs(product[0][0]))


if __name__ == "__main__":
    unittest.main()
