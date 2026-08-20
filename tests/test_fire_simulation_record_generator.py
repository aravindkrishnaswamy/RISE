#!/usr/bin/env python3
"""Strong arithmetic gates for the r51 physical methane record generator."""

from __future__ import annotations

import json
import copy
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
        cls.constants_path = ROOT / "docs/data/fire_fuel_methane_v1.draft.json"
        cls.snapshot = json.loads(cls.snapshot_path.read_text(encoding="utf-8"))
        cls.constants = records.load_methane_constants(cls.constants_path)
        cls.record = records.methane_payload(cls.snapshot,cls.constants)

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
                ("conservative_reconstruction_v1", 3),
                ("nonadvective_flux_projection_v1", 4)):
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
            conditioning = certificate["conditioning_certificate"]
            lower = decoded_rational(conditioning["lower_bound_exact_dyadic"])
            determinant = decoded_rational(conditioning["gram_determinant"])
            frobenius = decoded_rational(conditioning["frobenius_norm_squared"])
            self.assertGreater(lower, 0)
            self.assertLessEqual(lower * lower * frobenius ** (expected_rank - 1),
                                 determinant)

    def test_r56_physical_kernel_exact_source_and_dependent_row_red(self) -> None:
        physical = self.record["physical_kernel_consistency"]
        self.assertEqual(["C", "H", "O"],
                         physical["stored_independent_constraint_rows"])
        full = []
        element = decoded_matrix(self.record["element_mass_fraction_matrix"])
        ambient = [decoded_rational(value) for value in
                   self.record["ambient_state"]["element_mass_fractions"]]
        injected = [decoded_rational(value) for value in
                    self.record["injected_fuel_state"]["element_mass_fractions"]]
        for row in range(4):
            full.append([-(injected[row] - ambient[row])] +
                        [value - ambient[row] for value in element[row]])
        directions = [{"name": entry["name"],
                       "state_delta": [decoded_rational(value)
                                       for value in entry["state_delta"]]}
                      for entry in physical["physical_directions"]]
        records.verify_exact_physical_directions(full, directions)

        # RED for the shipped defect: independently rounding the exactly
        # dependent N row makes the physical reaction miss the dyadic kernel.
        bad = [row[:] for row in full[:3]]
        bad.append([Fraction.from_float(float(value)) for value in full[3]])
        with self.assertRaisesRegex(ValueError, "fails exact A_Q identity"):
            records.verify_exact_physical_directions(bad, directions)

        # Sustained-combustion RED: the exact rational source has zero
        # per-step bias, hence its cumulative envelope is no worse than
        # sqrt(steps) round-off (zero is the strongest instance).  Recreating
        # the released four-row, entrywise-rounded A produces a fixed-sign
        # primary residual whose cumulative error is exactly linear.
        primary = next(direction for direction in directions
                       if direction["name"] == "primary_combustion")
        good_residual = records.matrix_multiply(
            full, records.matrix_transpose([primary["state_delta"]]))
        self.assertTrue(all(value == 0 for row in good_residual for value in row))
        separately_rounded = [[Fraction.from_float(float(value)) for value in row]
                              for row in full]
        released_residual = records.matrix_multiply(
            separately_rounded,
            records.matrix_transpose([primary["state_delta"]]))
        per_step = max(abs(row[0]) for row in released_residual)
        self.assertGreater(per_step, 0)
        steps = 4096
        self.assertGreater(steps * per_step,
                           32 * (steps ** 0.5) * per_step)

    def test_r60_single_feasibility_envelope_and_mutation_reds(self) -> None:
        certificate = self.record["accepted_state_feasibility_envelope"]
        records.validate_accepted_state_feasibility_certificate(certificate)
        self.assertEqual(2384.0, certificate["derived_union_factor_epsilon64"])
        self.assertEqual(4096.0, certificate["kappa_epsilon64"])
        second_kappa = copy.deepcopy(certificate)
        second_kappa["producer_bounds"]["limiter_outward_factor_epsilon64"] = 1023.0
        with self.assertRaisesRegex(ValueError, "canonical derivation"):
            records.validate_accepted_state_feasibility_certificate(second_kappa)
        result_scaled = copy.deepcopy(certificate)
        result_scaled["mass_block_scale"] = "max(1,abs(row_result))"
        with self.assertRaisesRegex(ValueError, "canonical derivation"):
            records.validate_accepted_state_feasibility_certificate(result_scaled)
        core = (ROOT / "tools/fire_simulator_core.h").read_text(encoding="utf-8")
        advance = (ROOT / "tools/fire_simulator_3d_advance.h").read_text(encoding="utf-8")
        combined = core + advance
        self.assertNotIn("ConservativeStateFeasible", combined)
        self.assertNotIn("ThermochemicalDensitiesWithinForwardEnvelope", combined)
        self.assertNotIn("roundoffMultiplier", combined)
        self.assertNotIn("candidate[cell][component]=0.0", combined)
        self.assertEqual(4, combined.count("CertifiedLimiterInequalityBudget("))
        budget_body = combined[combined.index(
            "inline double CertifiedLimiterInequalityBudget("):]
        budget_body = budget_body[:budget_body.index("\n\t\tinline ", 1)]
        self.assertIn("envelope.kappaEpsilon64*epsilon*rowScaleLowerBound", budget_body)
        self.assertIn("envelope.limiterOutwardFactorEpsilon64*", budget_body)
        self.assertIn("rowScaleLowerBound<1.0", budget_body)
        scale_body = combined[combined.index("inline double InequalityRoundoffScale("):]
        scale_body = scale_body[:scale_body.index("\n\t\tinline ", 1)]
        self.assertIn("AcceptedStateMassScale(state)", scale_body)
        self.assertIn("AcceptedStateEnergyScale(state,ambientEnthalpy,adiabaticEnthalpy)",
                      scale_body)
        self.assertNotIn("InequalityValue", scale_body)

        def inline_body(source: str, name: str) -> str:
            start = source.index("inline bool " + name + "(")
            end = source.find("\n\t\tinline ", start + 1)
            return source[start:] if end < 0 else source[start:end]

        canonical_gate_sites = (
            (core, "InvertMethaneTemperatureWithinAcceptedEnvelope"),
            # Molecular transport is the state-consuming half of the split
            # transport evaluation.  The cached and uncached paths both enter
            # here; the gradient-only half consumes no conservative state.
            (core, "EvaluateCellMolecularTransport"),
            (core, "EquationOfStateResidual"),
            (core, "ApplyPeriodicSharedFCT"),
            (core, "DivergenceFromDiscreteRate"),
            (core, "BuildMethaneReactionPacket"),
            # ApplySourcePacket is a thin public wrapper; the noinline
            # canonical routine is the byte-identical predicate/commit path.
            (core, "CanonicalApplySourcePacket"),
            (core, "BuildOpenBoundaryStage3D"),
            (core, "BuildIgnitionEligibility"),
            (advance, "ApplyPeriodicSharedFCT3D"),
            (advance, "ApplyOpenSharedFCT3D"),
        )
        for source, name in canonical_gate_sites:
            body = inline_body(source, name)
            self.assertTrue("AcceptedStateAdmissible(" in body or
                            "AcceptedMethaneCellStateAdmissible(" in body,
                            name + " bypasses the canonical r60 predicate")
            self.assertNotRegex(
                body,
                r"(?:256|512|1024|2048|4096)\.0\s*\*[^;\n]*epsilon",
                name + " adds a second literal feasibility envelope")
            self.assertNotRegex(
                body,
                r"(?:row_result|rowResult|result_scale|resultScale)",
                name + " adds a result-scaled feasibility envelope")
        for name in ("ApplyPeriodicSharedFCT",):
            self.assertIn("config.producerPrecision", inline_body(core, name),
                          name + " drops the raw state's producer precision")
        for name in ("ApplyPeriodicSharedFCT3D", "ApplyOpenSharedFCT3D"):
            self.assertIn("config.producerPrecision", inline_body(advance, name),
                          name + " drops the raw state's producer precision")
            self.assertIn("CertifiedBinary32ZeroSource", inline_body(advance, name),
                          name + " admits an uncertified binary32 source")
        self.assertIn("CertifiedBinary32ZeroSource", inline_body(core,
                      "ApplyPeriodicSharedFCT"))
        self.assertIn("CertifiedBinary32ZeroSourcePacket", inline_body(core,
                      "CanonicalApplySourcePacket"))
        self.assertIn("CertifiedBinary32ZeroSourcePacket", inline_body(advance,
                      "AdvanceConservative3D"))
        for name in ("DivergenceFromDiscreteRate", "BuildOpenBoundaryStage3D"):
            self.assertIn("producerPrecision", inline_body(core, name),
                          name + " hardcodes a raw-state precision class")
        for name in ("AcceptedConservativeVolumeRatio", "DivergenceFromDiscreteIncrement",
                     "ManifoldExactDivergenceTarget",
                     "FrozenSourcePacketExpansionAdmissible",
                     "PeriodicDivergenceTargetFromPhysicalFlux"):
            self.assertIn("producerPrecision", inline_body(core, name),
                          name + " drops precision in the finite-increment chain")
        for name in ("BuildPeriodicStageTransport3D",
                     "PeriodicDivergenceTargetFromPhysicalFlux3D",
                     "BuildCellMolecularTransportEvaluations3D",
                     "BuildOpenStageTransportEvaluations3D",
                     "OpenDivergenceTargetFromPhysicalFlux3D"):
            self.assertIn("producerPrecision", inline_body(advance, name),
                          name + " drops precision in the owning 3-D chain")
        for name in ("SolveConservativeStage3D", "SolveOpenConservativeStage3D",
                     "AdvancePeriodicConservative3DImplementation",
                     "AdvanceOpenConservative3DImplementation"):
            self.assertIn("config.transport.producerPrecision", inline_body(advance, name),
                          name + " selects a compatibility precision overload")
        for name in ("ReferenceAdvancePeriodicTransportHeun1D",
                     "SolvePeriodicCoupledStage", "ReferenceAdvancePeriodicProjectedHeun1D"):
            self.assertIn("config.producerPrecision", inline_body(core, name),
                          name + " selects a compatibility precision overload")
        self.assertIn("config.producerPrecision",
                      inline_body(advance, "ReferenceAdvancePeriodicTransportHeun3DWithSource"),
                      "the 3-D reference owner selects a compatibility precision overload")
        for name in ("BuildFrozenMethaneSourcePacket", "BuildFrozenMethaneSourcePackets"):
            self.assertIn("beginning", inline_body(core, name))
            self.assertIn("producerPrecision", inline_body(core, name),
                          name + " drops accepted-state producer metadata")
        for name in ("EquationOfStateResidual", "DivergenceFromDiscreteRate",
                     "AcceptedConservativeVolumeRatio", "EvaluateGasExchange",
                     "CertifiedGasExchangeDerivativeLower"):
            self.assertIn("PositivePartThermochemicalDensitiesOrdered",
                          inline_body(core, name),
                          name + " consumes signed property availability")
        for name in ("InvertMethaneTemperatureWithinAcceptedEnvelope",
                     "EquationOfStateResidual", "EvaluateCellMolecularTransport"):
            self.assertIn("producerPrecision!=state.producerPrecision",
                          inline_body(core, name),
                          name + " permits a mixed producer/state precision gate")
        self.assertEqual(4, core.count("producerPrecision!=state.producerPrecision"),
                         "a MethaneCellState consumer lost the metadata mismatch gate")
        self.assertIn("EvaluateCellMolecularTransport(",
                      inline_body(core, "EvaluateCellTransport"))
        self.assertIn("CanonicalApplySourcePacket(",
                      inline_body(core, "ApplySourcePacket"))
        self.assertNotRegex(combined,
                            r"ConservativeStateFeasible\s*\([^)]*,\s*(256|512|1024|2048|4096)")

    def test_precision_class_extension_is_derived_and_keeps_one_gate(self) -> None:
        extension = records.accepted_state_binary32_feasibility_extension()
        records.validate_accepted_state_binary32_feasibility_extension(extension)
        self.assertEqual(832.0, extension["derived_union_factor_epsilon32"])
        self.assertEqual(1024.0, extension["kappa_epsilon32"])
        mutated = copy.deepcopy(extension)
        mutated["producer_bounds"]["projection_factor_epsilon32"] = 255.0
        with self.assertRaisesRegex(ValueError, "canonical derivation"):
            records.validate_accepted_state_binary32_feasibility_extension(mutated)
        restoration_mutated = copy.deepcopy(extension)
        restoration_mutated["producer_bounds"][
            "restoration_projection_factor_epsilon32"] = 255.0
        with self.assertRaisesRegex(ValueError, "canonical derivation"):
            records.validate_accepted_state_binary32_feasibility_extension(
                restoration_mutated)
        core = (ROOT / "tools/fire_simulator_core.h").read_text(encoding="utf-8")
        self.assertEqual(2, core.count("inline double AcceptedStateRoundoffFactor("))
        self.assertIn("FireStateProducerPrecision::Binary64", core)
        self.assertIn("FireStateProducerPrecision::Binary32", core)
        self.assertNotIn("candidate[cell][component]=0.0", core)

    def test_r80_open_active_set_two_class_sites(self) -> None:
        core = (ROOT / "tools/fire_simulator_core.h").read_text(encoding="utf-8")
        advance = (ROOT / "tools/fire_simulator_3d_advance.h").read_text(
            encoding="utf-8")

        def inline_body(source: str, name: str) -> str:
            start = source.index("inline bool " + name + "(")
            end = source.find("\n\t\tinline ", start + 1)
            return source[start:] if end < 0 else source[start:end]

        projector = inline_body(core, "ProjectPressureOpenMACVelocity3D")
        self.assertIn("activeSetDiscontinuousClass=true", projector)
        self.assertIn("OpenActiveSetComplementarityDiscrepancy3D", projector)
        self.assertIn("OpenActiveSetCanonicalHistoryIndex3D", projector)
        self.assertIn("error,1u,true", projector)
        self.assertNotIn("augmented 3-D active set cycled", projector)

        owner = inline_body(advance, "SolveOpenConservativeStage3D")
        self.assertIn("OpenActiveSetCanonicalBefore3D", owner)
        self.assertIn("outerActiveSetHistory", owner)
        self.assertGreaterEqual(owner.count("ObserveOpenActiveSetHistory3D"), 2)
        self.assertIn("activeSetMismatch&&!cycleProved", owner)
        self.assertIn("activeSetDiscontinuousEventCount=activeSetDiscontinuousEvents", owner)
        self.assertGreaterEqual(owner.count("1u,true"), 1)
        self.assertNotIn("coefficientResidual<=config.projectionTolerancePerS&&!activeSetChanged",
                         owner)
        self.assertNotIn("verification>config.projectionTolerancePerS||!verifiedActiveSet",
                         owner)
        self.assertNotRegex(owner, r"activeSetCycleLength\s*=\s*std::max<[^>]+>\([^;]*2u")

        scalar_flux = inline_body(core, "BuildOpenBoundaryFlux3D")
        self.assertIn("OpenScalarInflow3D", scalar_flux)
        self.assertIn("const ConservativeVector& donor = scalarInflow ?", scalar_flux)
        self.assertIn("if( !scalarInflow ) return true", scalar_flux)
        self.assertNotIn("if( !inflow ) return true", scalar_flux)

        scalar_classification = inline_body(core, "OpenScalarInflow3D")
        self.assertIn("outwardVelocityMPerS < -velocityToleranceMPerS",
                      scalar_classification)
        self.assertIn("outwardVelocityMPerS > velocityToleranceMPerS",
                      scalar_classification)

        flux_pair = inline_body(advance, "BuildOpenFluxPair3D")
        self.assertIn("OpenScalarInflow3D", flux_pair)
        self.assertNotIn("kind==PressureOpenBoundary3D&&projection.inflow", flux_pair)

        sequence = (ROOT / "tests/FireSequenceTest.cpp").read_text(encoding="utf-8")
        self.assertIn('return "open_active_set_two_class_r81_v2"', sequence)
        self.assertIn("CurrentActiveSetAlgorithmVersion()", sequence)
        self.assertIn('"active_set_algorithm_version"', sequence)
        self.assertIn('"active_set_prior_algorithm_version"', sequence)
        self.assertIn("active_set_thread_identity_mismatch", sequence)
        self.assertIn("priorActiveSetHistoryValid", sequence)
        self.assertIn("advancedOK=DiscontinuousThreadIdentityAccepted", sequence)
        self.assertIn("discontinuousActiveSetEvents+=", sequence)
    def test_generated_include_is_current(self) -> None:
        generated = records.generate(self.snapshot_path,self.constants_path)
        committed = (ROOT / "src/Library/Utilities/FireSimulationRecordData.inc").read_text(
            encoding="utf-8")
        self.assertEqual(committed, generated)
        self.assertIn("kFireSimMethanePhysicalV1SHA256", generated)

    def test_r69_manifold_restoration_has_no_zero_increment_bypass(self) -> None:
        core = (ROOT / "tools/fire_simulator_core.h").read_text(encoding="utf-8")
        start = core.index("inline bool DivergenceFromDiscreteIncrement(")
        end = core.index("\n\t\tinline ", start + 1)
        body = core[start:end]
        self.assertNotRegex(body, r"if\s*\(\s*zero\s*\).*result\s*=\s*0\.0")
        self.assertIn("candidateVector=stateVector+nonadvectiveAndSourceIncrement", body)
        self.assertIn("result=(candidateVolume-1.0)/deltaTimeS", body)
        one_d_start = core.index("inline bool PeriodicDivergenceTargetFromPhysicalFlux(")
        one_d_end = core.index("\n\t\tinline ", one_d_start + 1)
        one_d_body = core[one_d_start:one_d_end]
        self.assertIn("DivergenceFromDiscreteIncrement", one_d_body)
        self.assertNotIn("DivergenceFromDiscreteRate", one_d_body)

    def test_r52_operational_constant_taxonomy(self) -> None:
        ignition = self.record["ignition_gate"]
        pilot = ignition["pilot_temperature_K"]
        autoignition = ignition["autoignition_temperature_K"]
        self.assertEqual("design_gate_not_measured_property",
                         pilot["model_basis"]["kind"])
        self.assertNotIn("provenance", pilot)
        self.assertLess(pilot["value"], autoignition["value"])
        self.assertEqual("range", autoignition["uncertainty"]["kind"])
        self.assertEqual("measured_1sigma",
                         self.record["gross_soot_yield_kg_per_kg_fuel"]
                         ["uncertainty"]["kind"])
        self.assertEqual(0.0,
                         self.record["gross_soot_yield_kg_per_kg_fuel"]["value"])
        radiative = self.record["radiative_fraction_default"]
        self.assertEqual([0.07,0.28],radiative["uncertainty"]["magnitude"])
        self.assertIn("case_record",radiative["override_policy"])
        density = self.record["soot_density_reference"]
        self.assertNotIn("value",density)
        self.assertEqual(records.PREDICTIVE_FIRE_OPTICS_RECORD_SHA256,
                         density["optics_preset_record_id"])

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
