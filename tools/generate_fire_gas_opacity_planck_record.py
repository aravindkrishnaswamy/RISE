#!/usr/bin/env python3
"""Freeze the HITEMP-derived optically-thin Planck means into RISE-CBOR64-v1.

This is the production SS3.5 path.  It consumes the committed full Planck-mean
knots and the analytic knot derivatives generated from the committed pruned
(nu,E'') exponential-sum basis.  The resulting tensor bicubic is C1 and accepts
every interior temperature in its certified domain.  Full Voigt/LBL spectra are
deliberately outside this record's quantity semantics.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
from pathlib import Path

from generate_fire_optics_records import encode, emit_array, validate_unicode17_authority


AXIS = tuple(float(value) for value in range(300, 2501, 50))
VALIDATION_AXIS = tuple(float(value) for value in range(300, 2501, 25))
EXPECTED_SOURCE_MANIFEST_SHA256 = "977966bd9d7ba91a1232d2b5a1b76844dd2f521a7a909a1da2704ecaf1a760be"
PRUNING_ERROR = {"H2O": 2.225686e-3, "CO2": 1.812752e-3}
INTERPOLATION_ERROR = {
    "H2O": 2.2306504131733621e-3,
    "CO2": 1.816044215858666e-3,
}
VISIBLE = {
    "H2O": {
        "measured_wavelength_domain_nm": [380.0, 780.0],
        "dataset_reported_maximum_fraction_of_total_planck_mean": 1.694e-5,
        "coverage_statement": "HITEMP-2010 H2O lines cover the complete 380-780 nm renderer band",
    },
    "CO2": {
        "measured_wavelength_domain_nm": [565.0, 780.0],
        "dataset_reported_maximum_fraction_of_total_planck_mean": 1.522e-10,
        "coverage_statement": (
            "The measured HITEMP-2024 bound covers only 565-780 nm because the line list "
            "ends at 17696.93 cm^-1.  The 380-565 nm remainder is physically negligible: "
            "CO2 has no electronic absorption band in the visible; this is a physical "
            "negligibility argument, not measured HITEMP coverage."),
    },
}
EXPECTED_DATA_SHA256 = {
    "hitemp_planck_mean_h2o_v1.txt": "e2ccce619ec546bb5c13d8eaf2f01cfb7a7b78cc91c629fa98d3a9f261c2aa97",
    "hitemp_planck_mean_co2_v1.txt": "7d24505ec13bc33ccb074f350fa9f4675604f807e60c77986068ee202936860f",
    "hitemp_planck_surface_h2o_v1.txt": "b18dbd0db57294b340c0ed1f8b0da39557f5567f85015fd12d420c9c40649a7e",
    "hitemp_planck_surface_co2_v1.txt": "9f76e5a62f740fedfa5bc7e6dbbe661aafa82065d702ef7037cf07448058dea1",
    "hitemp_planck_validation_25K_h2o_v1.txt": "46be4144cf40744947e42cb9a27c4e3a12ec325d0599ad701c4d9382608c1549",
    "hitemp_planck_validation_25K_co2_v1.txt": "434134803bc82d0ab8d867743bd10cddb83bb3afcd1c47203d143e82572dad98",
    "hitemp_spectral_basis_h2o_v1.hist.gz": "efbc532d0e52559bfa4c0d1bbf857898c728613f503c1afedf223a50a734c7e3",
    "hitemp_spectral_basis_co2_v1.hist.gz": "1b50240e640539e392e60f59cf6d7b1b441b27e71db78f795db9d43a181a5cb4",
    "hitemp_visible_basis_h2o_v1.hist.gz": "17895196cadf71091ce77a07559cbe09b07a485a8c138bc3c3e0255c65f18f59",
    "hitemp_visible_basis_co2_v1.hist.gz": "152d8ee00e91a20c31e24f659a655ff6f2ffdd88967de1cfb0464a83be3c6da0",
    "hitemp_visible_tail_basis_h2o_v1.hist.gz": "530fd7cd50fbed4b3aa8acbe66a67e1fa584b742e6e23dc597d628affed79d8d",
    "hitemp_visible_tail_basis_co2_v1.hist.gz": "a91caf4e744cc8cbd90f575b29c25ff84c518bec07ac890734717ebc66c61c10",
    "hitemp_visible_certificate_h2o_v1.json": "80b46f4a045afc2cf441e85e02ebbcd70707908a4fdb4f5969643e15635cefb0",
    "hitemp_visible_certificate_co2_v1.json": "e5d523d954ff3ff36f7ecdd8d83fb3e0e7dc06a8c15d1e525419034574769149",
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require_digest(path: Path) -> str:
    expected = EXPECTED_DATA_SHA256.get(path.name)
    actual = sha256(path)
    if expected is None or actual != expected:
        raise ValueError(f"gas-opacity input SHA-256 mismatch for {path.name}")
    return actual


def _rows(path: Path, columns: int,
          axis: tuple[float, ...] = AXIS) -> list[list[float]]:
    result = []
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if raw.startswith("#") or not raw.strip():
            continue
        fields = raw.split()
        if len(fields) != columns:
            raise ValueError(f"{path}:{number}: malformed row")
        values = [float(field) for field in fields]
        if not all(math.isfinite(value) for value in values):
            raise ValueError(f"{path}:{number}: non-finite row")
        result.append(values)
    if len(result) != len(axis) ** 2:
        raise ValueError(f"{path}: incomplete square temperature surface")
    for index, values in enumerate(result):
        gas = axis[index // len(axis)]
        radiation = axis[index % len(axis)]
        if values[0] != gas or values[1] != radiation:
            raise ValueError(f"{path}: temperature lattice mismatch")
    return result


def _matmul(left: list[list[float]], right: list[list[float]]) -> list[list[float]]:
    return [[sum(left[row][k] * right[k][column] for k in range(len(right)))
             for column in range(len(right[0]))] for row in range(len(left))]


def bicubic_coefficients(corners: list[list[list[float]]], spacing: float) -> list[list[float]]:
    # corners[x][y] = [value, d/dx, d/dy, d2/dxdy]
    f00, f01 = corners[0][0], corners[0][1]
    f10, f11 = corners[1][0], corners[1][1]
    geometry = [
        [f00[0], f01[0], spacing * f00[2], spacing * f01[2]],
        [f10[0], f11[0], spacing * f10[2], spacing * f11[2]],
        [spacing * f00[1], spacing * f01[1], spacing * spacing * f00[3], spacing * spacing * f01[3]],
        [spacing * f10[1], spacing * f11[1], spacing * spacing * f10[3], spacing * spacing * f11[3]],
    ]
    hermite = [[1.0, 0.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0],
               [-3.0, 3.0, -2.0, -1.0], [2.0, -2.0, 1.0, 1.0]]
    transpose = [list(column) for column in zip(*hermite)]
    return _matmul(_matmul(hermite, geometry), transpose)


def _choose(n: int, k: int) -> int:
    if k < 0 or k > n:
        return 0
    return math.comb(n, k)


def bernstein_bounds(coefficients: list[list[float]]) -> list[float]:
    degree_x = len(coefficients) - 1
    degree_y = len(coefficients[0]) - 1
    controls = []
    for k in range(degree_x + 1):
        for ell in range(degree_y + 1):
            value = 0.0
            for i in range(k + 1):
                for j in range(ell + 1):
                    value += (coefficients[i][j] * _choose(k, i) / _choose(degree_x, i) *
                              _choose(ell, j) / _choose(degree_y, j))
            controls.append(value)
    return [math.nextafter(min(controls), -math.inf),
            math.nextafter(max(controls), math.inf)]


def derivative_bounds(coefficients: list[list[float]], spacing: float) -> list[list[float]]:
    dx = [[(i + 1) * coefficients[i + 1][j] / spacing for j in range(4)]
          for i in range(3)]
    dy = [[(j + 1) * coefficients[i][j + 1] / spacing for j in range(3)]
          for i in range(4)]
    bounds = [bernstein_bounds(dx), bernstein_bounds(dy)]
    # The native loader reconstructs the same Hermite/Bernstein transform in
    # binary64, but Python and C++ may associate its sums differently.  Expand
    # the mathematically conservative boxes by a recorded roundoff allowance
    # so a one-ulp language difference cannot turn a valid enclosure inward.
    scale = max(1.0e-300, *(abs(value) for pair in bounds for value in pair))
    allowance = 4096.0 * math.ulp(1.0) * scale
    for pair in bounds:
        pair[0] = math.nextafter(pair[0] - allowance, -math.inf)
        pair[1] = math.nextafter(pair[1] + allowance, math.inf)
    return bounds


def species_payload(data: Path, species: str) -> dict:
    stem = species.lower()
    table_path = data / f"hitemp_planck_mean_{stem}_v1.txt"
    surface_path = data / f"hitemp_planck_surface_{stem}_v1.txt"
    basis_path = data / f"hitemp_spectral_basis_{stem}_v1.hist.gz"
    validation_path = data / f"hitemp_planck_validation_25K_{stem}_v1.txt"
    visible_basis_path = data / f"hitemp_visible_basis_{stem}_v1.hist.gz"
    visible_tail_basis_path = data / f"hitemp_visible_tail_basis_{stem}_v1.hist.gz"
    visible_certificate_path = data / f"hitemp_visible_certificate_{stem}_v1.json"
    digests = {path.name: require_digest(path)
               for path in (table_path, surface_path, basis_path, validation_path,
                             visible_basis_path, visible_certificate_path)}
    digests[visible_tail_basis_path.name] = require_digest(visible_tail_basis_path)
    table = _rows(table_path, 4)
    surface = _rows(surface_path, 6)
    validation = _rows(validation_path, 4, VALIDATION_AXIS)
    nodes = []
    worst_pruned = 0.0
    for full, analytic in zip(table, surface):
        if full[2] <= 0.0 or analytic[2] <= 0.0:
            raise ValueError(f"{species} opacity is non-positive")
        worst_pruned = max(worst_pruned, abs(full[2] - analytic[2]) / full[2])
        nodes.append([full[2], analytic[3], analytic[4], analytic[5]])
    if worst_pruned > PRUNING_ERROR[species] * (1.0 + 2.0e-7):
        raise ValueError(f"{species} pruning certificate is false")
    worst_interpolation = 0.0
    for direct in validation:
        gas = min(len(AXIS) - 2, int((direct[0] - AXIS[0]) // 50.0))
        radiation = min(len(AXIS) - 2, int((direct[1] - AXIS[0]) // 50.0))
        corners = [[nodes[(gas + dx) * len(AXIS) + radiation + dy]
                    for dy in range(2)] for dx in range(2)]
        coefficients = bicubic_coefficients(corners, 50.0)
        u = (direct[0] - AXIS[gas]) / 50.0
        v = (direct[1] - AXIS[radiation]) / 50.0
        interpolated = sum(coefficients[i][j] * u ** i * v ** j
                           for i in range(4) for j in range(4))
        worst_interpolation = max(
            worst_interpolation, abs(interpolated - direct[2]) / direct[2])
    declared_interpolation = INTERPOLATION_ERROR[species]
    if (worst_interpolation > declared_interpolation or
            declared_interpolation > worst_interpolation * (1.0 + 2.0e-6)):
        raise ValueError(
            f"{species} interpolation validation certificate is false: "
            f"observed={worst_interpolation:.17g} declared={declared_interpolation:.17g}")
    cells = []
    count = len(AXIS)
    spacing = AXIS[1] - AXIS[0]
    for gas in range(count - 1):
        for radiation in range(count - 1):
            corners = [[nodes[(gas + dx) * count + radiation + dy]
                        for dy in range(2)] for dx in range(2)]
            coefficients = bicubic_coefficients(corners, spacing)
            bounds = derivative_bounds(coefficients, spacing)
            cells.append([bounds[0][0], bounds[0][1], bounds[1][0], bounds[1][1]])
    uncertainty = {
        "kind": "assumption_bound",
        "magnitude": 0.05,
        "basis": (
            "five-percent relative model bound covering the observed 0.1-3.2 percent "
            "agreement with independent Planck-mean-specific publications plus the "
            "separately measured pruning and midpoint interpolation errors"),
    }
    visible_reproduction = json.loads(visible_certificate_path.read_text(encoding="utf-8"))
    if (visible_reproduction.get("schema") != "rise-hitemp-visible-certificate-v1" or
            visible_reproduction.get("species") != species or
            visible_reproduction.get("temperature_domain_K") != [300.0, 2500.0] or
            visible_reproduction.get("bounded_renderer_wavenumber_domain_cm-1") !=
            [12820.5128205, 26315.7894737] or
            visible_reproduction.get("all_line_voigt_tail_cell_count", 0) <= 0 or
            visible_reproduction.get("continuous_upper_absorption_per_m_atm", 0.0) <
            visible_reproduction.get("observed_100K_lattice", {}).get(
                "maximum_absorption_per_m_atm", math.inf)):
        raise ValueError(f"{species} visible certificate is malformed")
    visible_certificate = dict(VISIBLE[species])
    visible_certificate.update({
        "exact_line_selection": True,
        "bounded_renderer_wavelength_domain_nm": [380.0, 780.0],
        "line_shape_scope": "visible_certificate_only_not_operational_planck_mean",
        "recomputed_observed_100K_lattice":
            visible_reproduction["observed_100K_lattice"],
        "continuous_upper_absorption_per_m_atm":
            visible_reproduction["continuous_upper_absorption_per_m_atm"],
        "continuous_bound_method": visible_reproduction["continuous_bound_method"],
        "certificate_sha256": digests[visible_certificate_path.name],
    })
    return {
        "species_id": species,
        "quantity": "planck_mean_cross_section_m2_per_molecule",
        "temperature_domain_K": [AXIS[0], AXIS[-1]],
        "gas_temperature_axis_K": list(AXIS),
        "radiation_temperature_axis_K": list(AXIS),
        "interpolation": {
            "kind": "tensor_bicubic_hermite_c1_analytic_basis_derivatives_v1",
            "node_columns": ["value_m2_per_molecule", "d_dTgas", "d_dTrad", "d2_dTgas_dTrad"],
            "nodes_row_major": [[value * 1.0e-4, dx * 1.0e-4, dy * 1.0e-4, dxy * 1.0e-4]
                                for value, dx, dy, dxy in nodes],
            "cell_partial_derivative_enclosures_row_major": [
                [bound * 1.0e-4 for bound in cell] for cell in cells],
            "interior_state_policy": "evaluate_c1_bicubic_never_reject_interior",
            "derivative_source": "analytic differentiation of the pruned HITEMP (nu,E'') exponential-sum basis with C1 TIPS interpolation",
        },
        "uncertainty": uncertainty,
        "provenance": {
            "citation": (("Rothman et al., HITEMP-2010, JQSRT 111:2139-2150, "
                          "doi:10.1016/j.jqsrt.2010.05.001; ") if species == "H2O" else
                         ("Hargreaves et al., HITEMP CO2-2024, JQSRT 333:109324, "
                          "doi:10.1016/j.jqsrt.2024.109324; original parameter sources: "
                          "AI-3000K doi:10.1016/j.jms.2023.111748, HITRAN2020 "
                          "doi:10.1016/j.jqsrt.2021.107949, Ames isotopologues "
                          "doi:10.1016/j.jqsrt.2019.03.002; ")) +
                         "TIPS-2021 data doi:10.5281/zenodo.4708099",
            "locator": "docs/data/gas_opacity/",
            "access": "derived HITEMP product; raw line bytes excluded; TIPS-2021 MIT",
            "secondary_source": False,
        },
        "input_sha256": digests,
        "applicability": "optically_thin_planck_mean_cooling_only",
        "pruning_certificate": {
            "kind": "measured_full_vs_pruned_planck_mean_relative_error_v1",
            "maximum_observed_relative": worst_pruned,
            "certified_upper_relative": PRUNING_ERROR[species],
            "domain_K": [300.0, 2500.0],
        },
        "interpolation_validation_certificate": {
            "kind": "direct_pruned_basis_vs_operational_full_knot_bicubic_25K_lattice_relative_error_v1",
            "maximum_observed_relative": worst_interpolation,
            "gas_temperature_spacing_K": 25.0,
            "radiation_temperature_spacing_K": 25.0,
            "domain_K": [300.0, 2500.0],
            "derivative_enclosure_kind": "exact_bernstein_hull_of_operational_bicubic_partials_v1",
        },
        "visible_band_certificate": visible_certificate,
    }


def payload(source_manifest_path: Path) -> dict:
    data = source_manifest_path.parent
    if sha256(source_manifest_path) != EXPECTED_SOURCE_MANIFEST_SHA256:
        raise ValueError("HITEMP source manifest does not match the independently adopted identity")
    source_manifest = json.loads(source_manifest_path.read_text(encoding="utf-8"))
    if source_manifest.get("record_name") != "hitemp-co2-h2o-planck-mean-v1":
        raise ValueError("HITEMP source manifest identity is unsupported")
    source_files = source_manifest.get("source_files", {})
    source_digests = source_files.get("sha256", {})
    tips_digests = source_manifest.get("tips_2021_source_files", {}).get("sha256", {})
    outputs = source_manifest.get("outputs", {})
    if source_files.get("file_count") != 36 or len(source_digests) != 36:
        raise ValueError("HITEMP source inventory is not the adopted 36-file set")
    if set(tips_digests) != ({f"1_{index}.QTpy" for index in range(1, 7)} |
                             {f"2_{index}.QTpy" for index in range(1, 13)}):
        raise ValueError("TIPS-2021 isotopologue inventory is incomplete")
    if (outputs.get("h2o", {}).get("records_read") != "114241164" or
            outputs.get("h2o", {}).get("isotopologues") != 6 or
            outputs.get("co2", {}).get("records_read") != "326260084" or
            outputs.get("co2", {}).get("isotopologues") != 12):
        raise ValueError("HITEMP record-count/isotopologue certificate is invalid")
    source_manifest_digest = sha256(source_manifest_path)
    return {
        "schema": "RISE-CBOR64-v1",
        "schema_version": 1,
        "version": "1.0.0",
        "record_kind": "fire_sim_gas_opacity",
        "record_name": "hitemp-co2-h2o-planck-mean-v1",
        "record_status": "predictive_qualified",
        "provenance_schema": "fire-optics-canonical-provenance-schema-v1",
        "out_of_domain_policy": "reject",
        "quantity_semantics": (
            "optically-thin two-temperature Planck-mean cross-section; line shape and "
            "self broadening cancel exactly and are not record axes"),
        "composition_basis": "sum species cross-section times actual species number density",
        "pressure_convention": "cross-section is pressure-independent; pressure enters only through species number density",
        "band_overlap_rule": "linear addition before the Planck integral; overlap is irrelevant to this linear quantity",
        "source_manifest_sha256": source_manifest_digest,
        "source_snapshot_sha256": source_manifest_digest,
        "raw_hitemp_line_bytes_committed": False,
        "species": [species_payload(data, "H2O"), species_payload(data, "CO2")],
        "independent_validation": {
            "primary": [
                "Chmielewski and Gieras HITEMP-2010 LBL: H2O agreement 1-3 percent over 300-1800 K",
                "Chmielewski and Gieras plus Zheng CDSD-4000: CO2 agreement 0.1-3.2 percent over 300-2000 K",
            ],
            "em2c_corroboration": {
                "citation": "CC-BY 4.0 EM2C-SNB DOI 10.17632/x5wjzk6sjs.1",
                "method": "epsilon/pL versus kappa_P on rows with kappa_P*pL <= 0.05",
                "role": "corroboration_only_not_a_qualification_source",
                "realized_pure_co2": {
                    "reference_sha256": "b54b8824298e41d6199dc557e970318daf020b3bc6b99dacc7f92ff55a7f7b6d",
                    "eligible_rows": 111,
                    "maximum_relative_difference": 0.5742165735325613,
                    "mean_relative_difference": 0.48165014280389,
                    "interpretation": (
                        "The smallest published finite paths retain substantial narrow-band "
                        "saturation, so this is weak corroboration and is not used to qualify "
                        "the Planck-mean record."),
                },
            },
        },
    }


def generate(source_manifest_path: Path) -> tuple[bytes, str]:
    encoded = encode(payload(source_manifest_path))
    output = io.StringIO(newline="\n")
    output.write("// Generated from docs/data/gas_opacity/hitemp_sources_v1.json.\n\n")
    emit_array(output, "kFireGasOpacityHITEMPPlanckMeanV1", encoded)
    return encoded, output.getvalue()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("source_manifest", type=Path)
    parser.add_argument("output_inc", type=Path)
    parser.add_argument("--record", type=Path)
    args = parser.parse_args()
    validate_unicode17_authority()
    encoded, include = generate(args.source_manifest)
    if args.check:
        if not args.output_inc.is_file() or args.output_inc.read_text(encoding="utf-8") != include:
            raise SystemExit(f"{args.output_inc} is stale")
        if args.record and (not args.record.is_file() or args.record.read_bytes() != encoded):
            raise SystemExit(f"{args.record} is stale")
        return
    with args.output_inc.open("w", encoding="utf-8", newline="\n") as output:
        output.write(include)
    if args.record:
        args.record.write_bytes(encoded)


if __name__ == "__main__":
    main()
