#!/usr/bin/env python3
"""Generate quarantined line-shaped spectra for non-SS3.5 research only.

This Voigt/LBL path is not the production fire-solver path.  It remains for
transmission/band-resolved experiments and its synthetic regression suite.
Section 3.5 records come from generate_fire_gas_opacity_planck_record.py.
"""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import math
import re
from pathlib import Path
from typing import Sequence

from fire_gas_opacity import (
    REFERENCE_PRESSURE_PA, PartitionSums, canonical_json_bytes, iter_hitran_lines,
    finite_path_emissivity_refinement_certificate, multilinear_value,
    homogeneous_emissivity, planck_mean_wavenumber, sha256_file,
    species_spectra_batch, spectral_grid, tensor_linear_derivative_certificate,
)


SCHEMA = "rise-fire-gas-opacity-generator-input-v1"
EXPECTED_MOLECULES = {"H2O": 1, "CO2": 2}
EXPECTED_ARCHIVE_LINE_COUNTS = {"H2O": 114241164, "CO2": 326260084}
MAXIMUM_PRODUCTION_SPECTRAL_STORAGE_BYTES = 2_000_000_000
MAXIMUM_PRODUCTION_LINE_STATE_EVALUATIONS = 5_000_000_000
EXPECTED_H2O_SEGMENT_NAMES = {
    f"01_{lower}-{upper}_HITEMP2010.par" for lower, upper in (
        ("00000", "00050"), ("00050", "00150"), ("00150", "00250"),
        ("00250", "00350"), ("00350", "00500"), ("00500", "00600"),
        ("00600", "00700"), ("00700", "00800"), ("00800", "00900"),
        ("00900", "01000"), ("01000", "01150"), ("01150", "01300"),
        ("01300", "01500"), ("01500", "01750"), ("01750", "02000"),
        ("02000", "02250"), ("02250", "02500"), ("02500", "02750"),
        ("02750", "03000"), ("03000", "03250"), ("03250", "03500"),
        ("03500", "04150"), ("04150", "04500"), ("04500", "05000"),
        ("05000", "05500"), ("05500", "06000"), ("06000", "06500"),
        ("06500", "07000"), ("07000", "07500"), ("07500", "08000"),
        ("08000", "08500"), ("08500", "09000"), ("09000", "11000"),
        ("11000", "30000"),
    )
}
EM2C_PATH_LENGTHS_M = tuple(
    0.01 * (50.0 / 0.01) ** (index / 89.0) for index in range(90))
PLACEHOLDER_CITATION = re.compile(
    r"\b(?:pending|placeholder|tbd|todo|owner[- ]download|distributed with)\b",
    re.IGNORECASE)


def _is_digest(value: object) -> bool:
    return (isinstance(value, str) and len(value) == 64 and
            all(character in "0123456789abcdef" for character in value))


def _resolved(root: Path, text: str) -> Path:
    path = Path(text)
    return path if path.is_absolute() else root / path


def generator_identity(paths: Sequence[Path] | None = None) -> str:
    inventory = list(paths) if paths is not None else [
        Path(__file__).resolve(),
        Path(__file__).with_name("fire_gas_opacity.py").resolve(),
        Path(__file__).with_name("fire_gas_opacity_native.py").resolve(),
        Path(__file__).with_name("fire_gas_opacity_native.cpp").resolve(),
        Path(__file__).with_name("build_fire_gas_opacity_native.py").resolve(),
    ]
    digest = hashlib.sha256(b"rise-fire-gas-opacity-generator-inventory-v1\0")
    for path in sorted(inventory, key=lambda item: item.name):
        name = path.name.encode("utf-8")
        payload = path.read_bytes()
        digest.update(len(name).to_bytes(8, "big"))
        digest.update(name)
        digest.update(len(payload).to_bytes(8, "big"))
        digest.update(payload)
    return digest.hexdigest()


def _concrete_citation(value: str) -> bool:
    if len(value.strip()) < 24 or PLACEHOLDER_CITATION.search(value):
        return False
    return ("http://" in value or "https://" in value or "doi" in value.lower() or
            re.search(r"\b(?:19|20)\d{2}\b", value) is not None)


def validate_em2c_path_domain(paths: Sequence[float]) -> None:
    if (len(paths) != len(EM2C_PATH_LENGTHS_M) or
            any(not math.isclose(actual, expected, rel_tol=0.0, abs_tol=5.1e-10)
                for actual, expected in zip(paths, EM2C_PATH_LENGTHS_M))):
        raise ValueError("production emissivity qualification must cover the pinned "
                         "90-point EM2C path domain")


def validate_production_species_inventory(
        source: dict, mass_keys: set[int]) -> None:
    species = source.get("species")
    files = source.get("files", [])
    if species == "CO2":
        if (mass_keys != set(range(1, 13)) or len(files) != 1 or
                Path(files[0].get("path", "")).name !=
                "02_HITEMP2024.par.bz2" or
                files[0].get("compression") != "bzip2" or
                source.get("release") != "HITEMP2024"):
            raise ValueError("CO2 production input is not the pinned HITEMP2024 archive")
        return
    if species == "H2O":
        names = {Path(item.get("path", "")).name for item in files}
        if (source.get("release") != "HITEMP2010" or
                names != EXPECTED_H2O_SEGMENT_NAMES or
                len(files) != len(EXPECTED_H2O_SEGMENT_NAMES) or
                any(item.get("compression") != "none" for item in files) or
                mass_keys != set(range(1, 7))):
            raise ValueError("H2O production inventory is not the pinned "
                             "34-segment HITEMP2010 archive")
        return
    raise ValueError("production opacity species inventory is unsupported")


def refined_axis(knots: Sequence[float], subdivisions: int) -> list[float]:
    if (len(knots) < 2 or subdivisions < 2 or subdivisions > 8 or
            any(not math.isfinite(value) for value in knots) or
            any(knots[index] >= knots[index + 1]
                for index in range(len(knots) - 1))):
        raise ValueError("opacity state-refinement axis is invalid")
    result = []
    for index in range(len(knots) - 1):
        lower, upper = knots[index], knots[index + 1]
        result.extend(lower + (upper - lower) * offset / subdivisions
                      for offset in range(subdivisions))
    result.append(float(knots[-1]))
    return result


def production_resource_budget(grid_count: int, self_count: int,
                               gas_temperature_count: int) -> dict:
    if grid_count < 2 or self_count < 2 or gas_temperature_count < 2:
        raise ValueError("production opacity resource dimensions are invalid")
    state_count = self_count * gas_temperature_count
    spectral_storage_bytes = 2 * state_count * grid_count * 8
    line_state_evaluations = sum(EXPECTED_ARCHIVE_LINE_COUNTS.values()) * state_count
    result = {
        "kind": "hard_preflight_upper_bound_v1",
        "operational_state_count_per_species": state_count,
        "two_cutoff_spectral_storage_bytes_per_species": spectral_storage_bytes,
        "line_state_evaluations_both_species": line_state_evaluations,
        "maximum_spectral_storage_bytes_per_species":
            MAXIMUM_PRODUCTION_SPECTRAL_STORAGE_BYTES,
        "maximum_line_state_evaluations_both_species":
            MAXIMUM_PRODUCTION_LINE_STATE_EVALUATIONS,
        "within_budget": (
            spectral_storage_bytes <= MAXIMUM_PRODUCTION_SPECTRAL_STORAGE_BYTES and
            line_state_evaluations <= MAXIMUM_PRODUCTION_LINE_STATE_EVALUATIONS),
    }
    return result


def _validate_tensor(value: object, dimensions: Sequence[int], label: str) -> None:
    if not dimensions:
        if not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0.0:
            raise ValueError(f"{label} contains an invalid scalar")
        return
    if not isinstance(value, list) or len(value) != dimensions[0]:
        raise ValueError(f"{label} has the wrong tensor shape")
    for child in value:
        _validate_tensor(child, dimensions[1:], label)


def validate_opacity_table(table: dict, *, allow_synthetic: bool = False,
                           expected_payload_identity: str | None = None) -> None:
    if table.get("schema") != "rise-fire-gas-opacity-table-v1":
        raise ValueError("opacity table schema is unsupported")
    synthetic = table.get("synthetic")
    status = table.get("record_status")
    if (synthetic, status) not in {
            (True, "synthetic_test_only"), (False, "production_derived")}:
        raise ValueError("opacity table status/synthetic pair is invalid")
    if not synthetic:
        raise ValueError("Voigt/LBL production tables are retired and non-operational")
    if synthetic and not allow_synthetic:
        raise ValueError("synthetic opacity table is forbidden for operational use")
    identity = table.get("canonical_payload_without_identity_sha256")
    if not _is_digest(identity):
        raise ValueError("opacity table canonical identity is missing")
    payload = dict(table)
    payload.pop("canonical_payload_without_identity_sha256", None)
    if hashlib.sha256(canonical_json_bytes(payload)).hexdigest() != identity:
        raise ValueError("opacity table canonical identity mismatch")
    if not synthetic:
        if not _is_digest(expected_payload_identity) or identity != expected_payload_identity:
            raise ValueError("production opacity table lacks the externally pinned identity")
    elif expected_payload_identity is not None and identity != expected_payload_identity:
        raise ValueError("synthetic opacity table identity does not match its test pin")
    if (table.get("out_of_domain_policy") != "reject" or
            table.get("archive_passes_per_species") != 1 or
            table.get("spectral_coordinate") != "vacuum_wavenumber_cm-1" or
            table.get("state_lookup_policy") !=
            "axis_knots_only_reject_interior_v1"):
        raise ValueError("opacity table operational policy is unsupported")
    backend = table.get("accumulator_backend")
    if ((not synthetic and backend != "native_streaming_v1") or
            (synthetic and backend not in {
                "python_reference_test_only", "native_streaming_v1"})):
        raise ValueError("opacity accumulator backend is not qualified")
    provenance_value = table.get("provenance")
    if (not isinstance(provenance_value, dict) or
            provenance_value.get("generator_sha256") != generator_identity() or
            provenance_value.get("line_bytes_committed") is not False or
            not _is_digest(provenance_value.get("input_manifest_sha256"))):
        raise ValueError("opacity table provenance is invalid")
    native_digest = provenance_value.get("native_accumulator_library_sha256")
    if ((backend == "native_streaming_v1" and not _is_digest(native_digest)) or
            (backend == "python_reference_test_only" and native_digest is not None)):
        raise ValueError("opacity accumulator binary identity is invalid")
    for field in ("hitemp_citations", "original_source_citations"):
        citations = provenance_value.get(field)
        if (not isinstance(citations, list) or not citations or
                any(not isinstance(item, str) or not item.strip() for item in citations)):
            raise ValueError("opacity table citations are invalid")
        if not synthetic and any(not _concrete_citation(item) for item in citations):
            raise ValueError("production opacity table citation is not concrete")
    self_axis = table.get("self_broadening_mole_fractions")
    gas_axis = table.get("gas_temperatures_K")
    radiation_axis = table.get("radiation_temperatures_K")
    grid = table.get("spectral_grid_cm-1")
    for axis, label in ((self_axis, "self"), (gas_axis, "gas temperature"),
                        (radiation_axis, "radiation temperature"),
                        (grid, "spectral")):
        if (not isinstance(axis, list) or len(axis) < 2 or
                any(not isinstance(value, (int, float)) or not math.isfinite(value)
                    for value in axis) or
                any(axis[index] >= axis[index + 1]
                    for index in range(len(axis) - 1))):
            raise ValueError(f"opacity {label} axis is malformed")
    if self_axis[0] != 0.0 or self_axis[-1] != 1.0:
        raise ValueError("opacity self-broadening axis does not span its domain")
    budget = table.get("production_resource_budget")
    if synthetic:
        if budget != {"kind": "synthetic_test_fixture_not_production_budgeted"}:
            raise ValueError("synthetic opacity resource-budget marker is invalid")
    else:
        expected_budget = production_resource_budget(
            len(grid), len(self_axis), len(gas_axis))
        if budget != expected_budget or budget.get("within_budget") is not True:
            raise ValueError("production opacity resource budget is invalid")
    pressure = table.get("pressure_Pa")
    edges = table.get("spectral_bin_edge_domain_cm-1")
    if (not isinstance(pressure, (int, float)) or not math.isfinite(pressure) or
            pressure != REFERENCE_PRESSURE_PA or
            not isinstance(edges, list) or len(edges) != 2 or
            not all(isinstance(value, (int, float)) and math.isfinite(value)
                    for value in edges) or edges[0] <= 0.0 or edges[0] >= edges[1]):
        raise ValueError("opacity pressure or spectral-bin domain is malformed")
    step = grid[1] - grid[0]
    if (any(not math.isclose(grid[index + 1] - grid[index], step,
                             rel_tol=0.0, abs_tol=1.0e-12 * step)
            for index in range(len(grid) - 1)) or
            edges != [grid[0] - 0.5 * step, grid[-1] + 0.5 * step]):
        raise ValueError("opacity spectral bins are inconsistent")
    species_tables = table.get("species_tables")
    if (not isinstance(species_tables, list) or
            {item.get("species") for item in species_tables
             if isinstance(item, dict)} != set(EXPECTED_MOLECULES) or
            len(species_tables) != len(EXPECTED_MOLECULES)):
        raise ValueError("opacity species inventory is incomplete or duplicated")
    for species in species_tables:
        name = species["species"]
        if (species.get("molecule_number") != EXPECTED_MOLECULES[name] or
                not isinstance(species.get("release"), str) or not species["release"]):
            raise ValueError("opacity species identity is invalid")
        archive_line_count = species.get("archive_line_count")
        if (not isinstance(archive_line_count, int) or
                isinstance(archive_line_count, bool) or archive_line_count <= 0 or
                (not synthetic and archive_line_count !=
                 EXPECTED_ARCHIVE_LINE_COUNTS[name])):
            raise ValueError("opacity species archive line count is invalid")
        _validate_tensor(species.get("line_counts_used"),
                         [len(self_axis), len(gas_axis)], "line counts")
        if any(count <= 0 for row in species["line_counts_used"] for count in row):
            raise ValueError("opacity species has an empty state")
        _validate_tensor(
            species.get("kappa_bin_average_per_m_per_unit_species_mole_fraction"),
            [len(self_axis), len(gas_axis), len(grid)], "spectral opacity")
        means = species.get("planck_mean_per_m_per_unit_species_mole_fraction")
        _validate_tensor(means, [len(self_axis), len(gas_axis), len(radiation_axis)],
                         "Planck mean")
        if tensor_linear_derivative_certificate(
                [self_axis, gas_axis, radiation_axis], means) != species.get(
                    "planck_mean_interpolation_diagnostic"):
            raise ValueError("opacity interpolation certificate does not bind the table")
        state_certificate = species.get("state_interpolation_diagnostic")
        finite_path_state = species.get(
            "finite_path_state_interpolation_diagnostic")
        finite_path_state_paths = []
        if synthetic:
            state_subdivisions = (state_certificate.get("subdivisions_per_cell")
                                  if isinstance(state_certificate, dict) else None)
            expected_state_samples = (0 if not isinstance(state_subdivisions, int) else
                ((len(self_axis) - 1) * state_subdivisions + 1) *
                ((len(gas_axis) - 1) * state_subdivisions + 1) *
                ((len(radiation_axis) - 1) * state_subdivisions + 1))
            if (not isinstance(state_certificate, dict) or
                    state_certificate.get("kind") !=
                    "direct_lbl_uniform_subcell_state_validation_diagnostic_v1" or
                    not isinstance(state_subdivisions, int) or
                    isinstance(state_subdivisions, bool) or
                    state_subdivisions < 2 or state_subdivisions > 8 or
                    state_certificate.get("sample_count") != expected_state_samples or
                    not _is_digest(state_certificate.get("validation_samples_sha256")) or
                    not isinstance(state_certificate.get("maximum_relative"),
                                   (int, float)) or
                    not math.isfinite(state_certificate["maximum_relative"]) or
                    not isinstance(state_certificate.get("maximum_allowed_relative"),
                                   (int, float)) or
                    not math.isfinite(state_certificate[
                        "maximum_allowed_relative"]) or
                    state_certificate["maximum_allowed_relative"] <= 0.0 or
                    state_certificate.get("sample_gate_passed") != (
                        state_certificate["maximum_relative"] <=
                        state_certificate["maximum_allowed_relative"])):
                raise ValueError("opacity state-interpolation diagnostic is invalid")
            refined_self_count = (len(self_axis) - 1) * state_subdivisions + 1
            refined_gas_count = (len(gas_axis) - 1) * state_subdivisions + 1
            finite_path_state_paths = (finite_path_state.get("path_lengths_m", [])
                                       if isinstance(finite_path_state, dict) else [])
            expected_finite_path_samples = ((2 * refined_self_count - 1) *
                                            refined_gas_count *
                                            len(finite_path_state_paths))
            if (not isinstance(finite_path_state, dict) or
                    finite_path_state.get("kind") !=
                    "direct_lbl_refined_state_finite_path_emissivity_diagnostic_v1" or
                    finite_path_state.get("composition_scales") !=
                    "unit_and_self_fraction" or
                    finite_path_state.get("sample_count") !=
                    expected_finite_path_samples or
                    not _is_digest(finite_path_state.get("validation_samples_sha256")) or
                    not isinstance(finite_path_state.get("maximum_relative"),
                                   (int, float)) or
                    not math.isfinite(finite_path_state["maximum_relative"]) or
                    not isinstance(finite_path_state.get("maximum_allowed_relative"),
                                   (int, float)) or
                    not math.isfinite(finite_path_state[
                        "maximum_allowed_relative"]) or
                    finite_path_state["maximum_allowed_relative"] <= 0.0 or
                    finite_path_state.get("sample_gate_passed") != (
                        finite_path_state["maximum_relative"] <=
                        finite_path_state["maximum_allowed_relative"])):
                raise ValueError("opacity spectral-state finite-path diagnostic is invalid")
        else:
            expected_state_stub = {
                "kind": "skipped_axis_knots_only_production_v1",
                "operational_use": False,
                "reason": "continuous interpolation is rejected; refined diagnostics "
                          "are synthetic-test-only to keep production work bounded",
            }
            expected_finite_stub = {
                "kind": "skipped_axis_knots_only_production_v1",
                "operational_use": False,
                "reason": "finite-path state interpolation is not operational",
            }
            if state_certificate != expected_state_stub or finite_path_state != expected_finite_stub:
                raise ValueError("production opacity diagnostics are not the bounded stubs")
        emissivity_certificate = species.get("finite_path_emissivity_grid_qualification")
        if not isinstance(emissivity_certificate, dict):
            raise ValueError("opacity finite-path emissivity qualification is missing")
        if not synthetic:
            validate_em2c_path_domain(
                emissivity_certificate.get("path_lengths_m", []))
        if (synthetic and finite_path_state_paths !=
                emissivity_certificate.get("path_lengths_m", [])):
            raise ValueError("opacity finite-path qualifications use different domains")
        expected_emissivity_certificate = finite_path_emissivity_refinement_certificate(
            grid,
            species["kappa_bin_average_per_m_per_unit_species_mole_fraction"],
            gas_axis, emissivity_certificate.get("path_lengths_m", []),
            emissivity_certificate.get(
                "maximum_allowed_estimated_remaining_relative_error", math.nan),
            emissivity_certificate.get(
                "maximum_allowed_contraction_ratio", math.nan))
        expected_emissivity_certificate[
            "state_scope"] = "operational_axis_knots_only"
        if (emissivity_certificate != expected_emissivity_certificate or
                (not synthetic and emissivity_certificate.get("qualified") is not True)):
            raise ValueError("opacity finite-path emissivity qualification is invalid")
        visible_upper = species.get(
            "visible_380_780nm_conservative_upper_m-1_per_unit_species_mole_fraction")
        visible_allowed = species.get(
            "visible_380_780nm_maximum_allowed_m-1_per_unit_species_mole_fraction")
        if (not isinstance(visible_upper, (int, float)) or
                not isinstance(visible_allowed, (int, float)) or
                not math.isfinite(visible_upper) or not math.isfinite(visible_allowed) or
                visible_upper < 0.0 or visible_allowed <= 0.0 or
                visible_upper > visible_allowed or
                species.get("visible_upper_bound_domain") !=
                "continuous_self_fraction_and_gas_temperature_cells_v1"):
            raise ValueError("opacity visible-band certificate is invalid")
        files = species.get("input_files")
        q_files = species.get("partition_sums", {}).get("files")
        q_citation = species.get("partition_sums", {}).get("citation")
        if (not isinstance(files, list) or not files or
                any(not _is_digest(item.get("sha256")) for item in files) or
                not isinstance(q_files, list) or not q_files or
                any(not _is_digest(item.get("sha256")) for item in q_files) or
                not isinstance(q_citation, str) or not q_citation.strip() or
                (not synthetic and not _concrete_citation(q_citation))):
            raise ValueError("opacity species source inventory is invalid")


def load_verified_manifest(path: Path, input_root: Path | None) -> tuple[dict, Path]:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("schema") != SCHEMA:
        raise ValueError("HITEMP input manifest schema is unsupported")
    status = manifest.get("status")
    if status not in {"synthetic_test_fixture", "production_pinned"}:
        raise ValueError("HITEMP inputs remain owner-download pending; no table can be generated")
    if status == "production_pinned":
        raise ValueError("Voigt/LBL production generation is retired; use the adopted Planck-mean path")
    root = input_root.resolve() if input_root else path.parent.resolve()
    if status == "production_pinned" and manifest.get("synthetic") is not False:
        raise ValueError("production HITEMP manifest must explicitly declare synthetic=false")
    if status == "synthetic_test_fixture" and manifest.get("synthetic") is not True:
        raise ValueError("synthetic fixture manifest must explicitly declare synthetic=true")
    for field in ("hitemp_citations", "original_source_citations"):
        citations = manifest.get(field)
        if (not isinstance(citations, list) or not citations or
                any(not isinstance(value, str) or not value.strip() for value in citations)):
            raise ValueError(f"{field} must contain nonempty citations")
        if status == "production_pinned" and any(
                not _concrete_citation(value) for value in citations):
            raise ValueError(f"{field} contains a pending or non-concrete citation")
    seen_species: set[str] = set()
    for source in manifest.get("sources", []):
        species = source.get("species")
        if species not in EXPECTED_MOLECULES or species in seen_species:
            raise ValueError("manifest must contain unique H2O and CO2 sources")
        if int(source.get("molecule_number", 0)) != EXPECTED_MOLECULES[species]:
            raise ValueError(f"{species} molecule number is not the pinned HITRAN identity")
        seen_species.add(species)
        files = source.get("files")
        if not isinstance(files, list) or not files:
            raise ValueError(f"{species} line-file inventory is empty")
        for item in files:
            expected = item.get("sha256")
            if not _is_digest(expected):
                raise ValueError(f"{species} line-file digest is not pinned")
            local = _resolved(root, item["path"])
            if not local.is_file() or sha256_file(local) != expected:
                raise ValueError(f"{species} line-file SHA-256 mismatch: {local}")
            if status == "production_pinned" and local.suffix.lower() == ".par" and path.parent in local.parents:
                raise ValueError("production .par bytes must remain outside the repository manifest tree")
        partition = source.get("partition_sums", {})
        if partition.get("format") != "hitran_q_temperature_value_columns":
            raise ValueError(f"{species} partition-sum format is unsupported")
        partition_citation = partition.get("citation")
        if (not isinstance(partition_citation, str) or
                not partition_citation.strip() or
                (status == "production_pinned" and
                 not _concrete_citation(partition_citation))):
            raise ValueError(f"{species} partition-sum citation is missing or non-concrete")
        q_files = partition.get("files")
        if not isinstance(q_files, list) or not q_files:
            raise ValueError(f"{species} partition-sum inventory is empty")
        q_keys = set()
        for item in q_files:
            key = (int(item.get("molecule", 0)), int(item.get("isotopologue", 0)))
            expected_q = item.get("sha256")
            local_q = _resolved(root, item.get("path", ""))
            if (key in q_keys or key[0] != int(source["molecule_number"]) or key[1] <= 0 or
                    not _is_digest(expected_q) or not local_q.is_file() or
                    sha256_file(local_q) != expected_q):
                raise ValueError(f"{species} partition-sum SHA-256/inventory mismatch")
            q_keys.add(key)
        mass_keys = {int(key) for key in source["isotopologue_molar_masses_kg_per_mol"]}
        if {key[1] for key in q_keys} != mass_keys:
            raise ValueError(f"{species} partition sums and isotopologue masses do not align")
        if status == "production_pinned":
            validate_production_species_inventory(source, mass_keys)
            repo_root = Path(__file__).resolve().parent.parent
            local_lines = [_resolved(root, item["path"]).resolve() for item in files]
            if any(local == repo_root or repo_root in local.parents for local in local_lines):
                raise ValueError("production HITEMP line bytes must remain outside the repository")
    if seen_species != {"H2O", "CO2"}:
        raise ValueError("manifest requires both H2O and CO2")
    return manifest, root


def _line_iterator(source: dict, root: Path):
    return itertools.chain.from_iterable(
        iter_hitran_lines(_resolved(root, item["path"]), item["compression"])
        for item in source["files"]
    )


def _partition_sums(source: dict, root: Path) -> PartitionSums:
    return PartitionSums.from_hitran_q_files(
        (int(item["molecule"]), int(item["isotopologue"]),
         _resolved(root, item["path"]))
        for item in source["partition_sums"]["files"])


def _exact_axis_index(axis: Sequence[float], value: float) -> int:
    if not math.isfinite(value):
        raise ValueError("opacity state is out of domain")
    try:
        return list(axis).index(value)
    except ValueError as exc:
        raise ValueError("opacity state is out of domain; production tables "
                         "accept declared axis knots only") from exc


def evaluate_species_planck_mean(table: dict, species_name: str,
                                 self_mole_fraction: float,
                                 gas_temperature_k: float,
                                 radiation_temperature_k: float,
                                 pressure_pa: float, *,
                                 allow_synthetic: bool = False,
                                 expected_payload_identity: str | None = None) -> float:
    validate_opacity_table(
        table, allow_synthetic=allow_synthetic,
        expected_payload_identity=expected_payload_identity)
    if (not math.isfinite(pressure_pa) or
            pressure_pa != float(table.get("pressure_Pa", math.nan))):
        raise ValueError("opacity pressure is out of domain")
    species = next((entry for entry in table.get("species_tables", [])
                    if entry.get("species") == species_name), None)
    if species is None:
        raise ValueError("opacity species record is missing")
    axes = [table["self_broadening_mole_fractions"],
            table["gas_temperatures_K"], table["radiation_temperatures_K"]]
    certificate = tensor_linear_derivative_certificate(
        axes, species["planck_mean_per_m_per_unit_species_mole_fraction"])
    if certificate != species.get("planck_mean_interpolation_diagnostic"):
        raise ValueError("opacity interpolation certificate does not bind the table")
    if not table["synthetic"]:
        indices = [_exact_axis_index(axis, value) for axis, value in zip(
            axes, [self_mole_fraction, gas_temperature_k,
                   radiation_temperature_k])]
        return species[
            "planck_mean_per_m_per_unit_species_mole_fraction"][indices[0]][
                indices[1]][indices[2]]
    return multilinear_value(
        axes, species["planck_mean_per_m_per_unit_species_mole_fraction"],
        [self_mole_fraction, gas_temperature_k, radiation_temperature_k])


def evaluate_mixture_planck_mean(table: dict, h2o_mole_fraction: float,
                                 co2_mole_fraction: float,
                                 gas_temperature_k: float,
                                 radiation_temperature_k: float,
                                 pressure_pa: float, *,
                                 allow_synthetic: bool = False,
                                 expected_payload_identity: str | None = None) -> float:
    validate_opacity_table(
        table, allow_synthetic=allow_synthetic,
        expected_payload_identity=expected_payload_identity)
    fractions = (h2o_mole_fraction, co2_mole_fraction)
    if (any(not math.isfinite(value) or value < 0.0 for value in fractions) or
            sum(fractions) > 1.0):
        raise ValueError("opacity composition is outside the air-balance simplex")
    return sum(fraction * evaluate_species_planck_mean(
        table, species, fraction, gas_temperature_k,
        radiation_temperature_k, pressure_pa,
        allow_synthetic=allow_synthetic,
        expected_payload_identity=expected_payload_identity)
               for fraction, species in zip(fractions, ("H2O", "CO2")))


def evaluate_species_kappa_bin(table: dict, species_name: str,
                               self_mole_fraction: float,
                               gas_temperature_k: float,
                               wavenumber_cm1: float, pressure_pa: float, *,
                               allow_synthetic: bool = False,
                               expected_payload_identity: str | None = None) -> float:
    validate_opacity_table(
        table, allow_synthetic=allow_synthetic,
        expected_payload_identity=expected_payload_identity)
    if (not math.isfinite(pressure_pa) or
            pressure_pa != float(table["pressure_Pa"])):
        raise ValueError("opacity pressure is out of domain")
    edges = table["spectral_bin_edge_domain_cm-1"]
    if (not math.isfinite(wavenumber_cm1) or
            wavenumber_cm1 < edges[0] or wavenumber_cm1 > edges[1]):
        raise ValueError("opacity spectral coordinate is out of domain")
    species = next((entry for entry in table["species_tables"]
                    if entry["species"] == species_name), None)
    if species is None:
        raise ValueError("opacity species record is missing")
    grid = table["spectral_grid_cm-1"]
    step = grid[1] - grid[0]
    bin_index = min(len(grid) - 1, max(0, int(
        math.floor((wavenumber_cm1 - edges[0]) / step))))
    spectra = species[
        "kappa_bin_average_per_m_per_unit_species_mole_fraction"]
    if not table["synthetic"]:
        self_index = _exact_axis_index(
            table["self_broadening_mole_fractions"], self_mole_fraction)
        gas_index = _exact_axis_index(
            table["gas_temperatures_K"], gas_temperature_k)
        return spectra[self_index][gas_index][bin_index]
    tensor = [[spectra[self_index][gas_index][bin_index]
               for gas_index in range(len(table["gas_temperatures_K"]))]
              for self_index in range(len(table["self_broadening_mole_fractions"]))]
    return multilinear_value(
        [table["self_broadening_mole_fractions"], table["gas_temperatures_K"]],
        tensor, [self_mole_fraction, gas_temperature_k])


def generate(manifest_path: Path, input_root: Path | None,
             native_library: Path | None = None) -> dict:
    manifest, root = load_verified_manifest(manifest_path, input_root)
    if not manifest["synthetic"]:
        raise ValueError("Voigt/LBL production generation is retired; use the adopted Planck-mean path")
    if not manifest["synthetic"] and native_library is None:
        raise ValueError("production HITEMP generation requires the native accumulator")
    if native_library is not None and not native_library.is_file():
        raise ValueError("native opacity accumulator library is missing")
    grid_config = manifest["spectral_grid_cm-1"]
    grid = spectral_grid(float(grid_config["minimum"]), float(grid_config["maximum"]),
                         float(grid_config["step"]))
    gas_temperatures = [float(value) for value in manifest["gas_temperatures_K"]]
    radiation_temperatures = [float(value) for value in
                              manifest["radiation_temperatures_K"]]
    self_fractions = [float(value) for value in
                      manifest["self_broadening_mole_fractions"]]
    subdivisions = manifest.get("state_interpolation_subdivisions_per_cell")
    maximum_state_interpolation_relative = float(
        manifest.get("maximum_planck_mean_state_interpolation_relative_error",
                     math.nan))
    maximum_finite_path_state_interpolation_relative = float(manifest.get(
        "maximum_finite_path_state_interpolation_relative_error", math.nan))
    if (len(gas_temperatures) < 2 or len(radiation_temperatures) < 2 or
            any(gas_temperatures[index] >= gas_temperatures[index + 1]
                for index in range(len(gas_temperatures) - 1)) or
            any(radiation_temperatures[index] >= radiation_temperatures[index + 1]
                for index in range(len(radiation_temperatures) - 1)) or
            len(self_fractions) < 2 or self_fractions[0] != 0.0 or
            self_fractions[-1] != 1.0 or
            any(self_fractions[index] >= self_fractions[index + 1]
                for index in range(len(self_fractions) - 1))):
        raise ValueError("temperature grids must contain increasing closed intervals")
    if (not isinstance(subdivisions, int) or isinstance(subdivisions, bool) or
            subdivisions < 2 or subdivisions > 8 or
            not math.isfinite(maximum_state_interpolation_relative) or
            maximum_state_interpolation_relative <= 0.0 or
            not math.isfinite(maximum_finite_path_state_interpolation_relative) or
            maximum_finite_path_state_interpolation_relative <= 0.0):
        raise ValueError("opacity state-interpolation qualification is invalid")
    if manifest["synthetic"]:
        evaluation_gas_temperatures = refined_axis(gas_temperatures, subdivisions)
        evaluation_radiation_temperatures = refined_axis(
            radiation_temperatures, subdivisions)
        evaluation_self_fractions = refined_axis(self_fractions, subdivisions)
        operational_indices = [
            index * subdivisions for index in range(len(gas_temperatures))]
        operational_self_indices = [
            index * subdivisions for index in range(len(self_fractions))]
        operational_radiation_indices = [
            index * subdivisions for index in range(len(radiation_temperatures))]
    else:
        evaluation_gas_temperatures = gas_temperatures
        evaluation_radiation_temperatures = radiation_temperatures
        evaluation_self_fractions = self_fractions
        operational_indices = list(range(len(gas_temperatures)))
        operational_self_indices = list(range(len(self_fractions)))
        operational_radiation_indices = list(range(len(radiation_temperatures)))
    pressure_pa = float(manifest["pressure_Pa"])
    if not math.isfinite(pressure_pa) or pressure_pa != REFERENCE_PRESSURE_PA:
        raise ValueError("opacity table pressure must be exactly 101325 Pa (1 atm)")
    grid_step = grid[1] - grid[0]
    spectral_bin_edges = [grid[0] - 0.5 * grid_step,
                          grid[-1] + 0.5 * grid_step]
    if spectral_bin_edges[0] <= 0.0:
        raise ValueError("spectral bins must have positive wavenumber edges")
    cutoff = float(manifest["line_wing_cutoff_cm-1"])
    convergence_factor = float(manifest["line_wing_convergence_factor"])
    maximum_grid_relative = float(manifest["maximum_planck_mean_grid_relative_error"])
    maximum_wing_relative = float(manifest["maximum_planck_mean_wing_relative_error"])
    maximum_visible_upper = float(
        manifest["maximum_visible_gas_absorption_m-1_per_unit_species_mole_fraction"])
    emissivity_config = manifest.get("finite_path_emissivity_grid_qualification", {})
    emissivity_paths = [float(value) for value in
                        emissivity_config.get("path_lengths_m", [])]
    emissivity_remaining_limit = float(
        emissivity_config.get("maximum_estimated_remaining_relative_error", math.nan))
    emissivity_contraction_limit = float(
        emissivity_config.get("maximum_contraction_ratio", math.nan))
    if not manifest["synthetic"]:
        validate_em2c_path_domain(emissivity_paths)
    resource_budget = production_resource_budget(
        len(grid), len(self_fractions), len(gas_temperatures))
    if not manifest["synthetic"] and not resource_budget["within_budget"]:
        raise ValueError("production opacity manifest exceeds the hard resource budget")
    if (convergence_factor <= 1.0 or maximum_grid_relative <= 0.0 or
            maximum_wing_relative <= 0.0 or maximum_visible_upper <= 0.0):
        raise ValueError("line-wing convergence factor must exceed one")

    species_tables = []
    for source in manifest["sources"]:
        partition = _partition_sums(source, root)
        masses = {int(key): float(value) for key, value in
                  source["isotopologue_molar_masses_kg_per_mol"].items()}
        visible_interval = [1.0e7 / 780.0, 1.0e7 / 380.0]
        if native_library is None:
            accumulator_result = species_spectra_batch(
                _line_iterator(source, root), int(source["molecule_number"]), masses,
                partition, grid, evaluation_gas_temperatures, pressure_pa,
                evaluation_self_fractions,
                [cutoff, cutoff * convergence_factor],
                evaluation_radiation_temperatures,
                visible_interval)
        else:
            from fire_gas_opacity_native import species_spectra_batch_native
            accumulator_result = species_spectra_batch_native(
                native_library, source, root, masses, partition, grid,
                evaluation_gas_temperatures, pressure_pa,
                evaluation_self_fractions,
                [cutoff, cutoff * convergence_factor],
                evaluation_radiation_temperatures,
                visible_interval)
        (all_spectra, all_counts, tail_bounds, center_means,
         _visible_knot_bounds, visible_cell_bounds,
         archive_line_count) = accumulator_result
        if (not manifest["synthetic"] and archive_line_count !=
                EXPECTED_ARCHIVE_LINE_COUNTS[source["species"]]):
            raise ValueError(
                f"{source['species']} archive line count does not match HITEMP")
        evaluation_spectra = all_spectra[0]
        evaluation_expanded = all_spectra[1]
        evaluation_line_counts = all_counts[0]
        evaluation_expanded_line_counts = all_counts[1]
        if (any(count <= 0 for row in evaluation_line_counts for count in row) or
                any(count <= 0 for row in evaluation_expanded_line_counts
                    for count in row)):
            raise ValueError(f"{source['species']} archive contributes no lines to a table state")
        evaluation_means = [[[
            planck_mean_wavenumber(
                grid, evaluation_spectra[self_index][gas_index], temperature)
            for temperature in evaluation_radiation_temperatures
        ] for gas_index in range(len(evaluation_gas_temperatures))]
                            for self_index in range(len(evaluation_self_fractions))]
        evaluation_expanded_means = [[[
            planck_mean_wavenumber(
                grid, evaluation_expanded[self_index][gas_index], temperature)
            for temperature in evaluation_radiation_temperatures
        ] for gas_index in range(len(evaluation_gas_temperatures))]
                                     for self_index in range(
                                         len(evaluation_self_fractions))]
        spectra = [[evaluation_spectra[self_index][gas_index]
                    for gas_index in operational_indices]
                   for self_index in operational_self_indices]
        expanded = [[evaluation_expanded[self_index][gas_index]
                     for gas_index in operational_indices]
                    for self_index in operational_self_indices]
        line_counts = [[evaluation_line_counts[self_index][gas_index]
                        for gas_index in operational_indices]
                       for self_index in operational_self_indices]
        expanded_line_counts = [[
            evaluation_expanded_line_counts[self_index][gas_index]
            for gas_index in operational_indices]
            for self_index in operational_self_indices]
        means = [[[evaluation_means[self_index][gas_index][radiation_index]
                   for radiation_index in operational_radiation_indices]
                  for gas_index in operational_indices]
                 for self_index in operational_self_indices]
        maximum_abs_sensitivity = 0.0
        maximum_relative_sensitivity = 0.0
        for self_index in range(len(evaluation_self_fractions)):
            for gas_index in range(len(evaluation_gas_temperatures)):
                for value, reference in zip(
                        evaluation_spectra[self_index][gas_index],
                        evaluation_expanded[self_index][gas_index]):
                    difference = abs(value - reference)
                    maximum_abs_sensitivity = max(maximum_abs_sensitivity, difference)
                    maximum_relative_sensitivity = max(
                        maximum_relative_sensitivity,
                        difference / max(abs(reference), 1.0e-300))
        maximum_grid_abs = 0.0
        maximum_grid_rel = 0.0
        maximum_wing_mean_abs = 0.0
        maximum_wing_mean_rel = 0.0
        for self_index, self_rows in enumerate(evaluation_means):
            for gas_index, row in enumerate(self_rows):
                for radiation_index, value in enumerate(row):
                    reference = center_means[gas_index][radiation_index]
                    difference = abs(value - reference)
                    maximum_grid_abs = max(maximum_grid_abs, difference)
                    maximum_grid_rel = max(maximum_grid_rel,
                                           difference / max(abs(reference), 1.0e-300))
                    wing_reference = evaluation_expanded_means[
                        self_index][gas_index][radiation_index]
                    wing_difference = abs(value - wing_reference)
                    maximum_wing_mean_abs = max(maximum_wing_mean_abs, wing_difference)
                    maximum_wing_mean_rel = max(
                        maximum_wing_mean_rel,
                        wing_difference / max(abs(wing_reference), 1.0e-300))
        if maximum_grid_rel > maximum_grid_relative:
            raise ValueError(f"{source['species']} spectral grid fails its Planck-mean gate")
        if maximum_wing_mean_rel > maximum_wing_relative:
            raise ValueError(f"{source['species']} line-wing comparison fails its Planck-mean gate")
        maximum_state_interpolation_abs = 0.0
        maximum_state_interpolation_rel = 0.0
        worst_state = None
        validation_samples = []
        diagnostic_self_fractions = (evaluation_self_fractions
                                     if manifest["synthetic"] else [])
        for self_index, self_fraction in enumerate(diagnostic_self_fractions):
            for gas_index, gas_temperature in enumerate(evaluation_gas_temperatures):
                for radiation_index, radiation_temperature in enumerate(
                        evaluation_radiation_temperatures):
                    direct = evaluation_means[self_index][gas_index][radiation_index]
                    interpolated = multilinear_value(
                        [self_fractions, gas_temperatures, radiation_temperatures],
                        means, [self_fraction, gas_temperature, radiation_temperature])
                    difference = abs(direct - interpolated)
                    relative = difference / max(abs(direct), 1.0e-300)
                    validation_samples.append([
                        self_fraction, gas_temperature, radiation_temperature,
                        direct, interpolated])
                    maximum_state_interpolation_abs = max(
                        maximum_state_interpolation_abs, difference)
                    if relative > maximum_state_interpolation_rel:
                        maximum_state_interpolation_rel = relative
                        worst_state = [self_fraction, gas_temperature,
                                       radiation_temperature]
        state_interpolation_certificate = {
            "kind": "direct_lbl_uniform_subcell_state_validation_diagnostic_v1",
            "subdivisions_per_cell": subdivisions,
            "sample_count": len(validation_samples),
            "validation_samples_sha256": hashlib.sha256(
                canonical_json_bytes(validation_samples)).hexdigest(),
            "maximum_absolute_m-1_per_unit_species_mole_fraction":
                maximum_state_interpolation_abs,
            "maximum_relative": maximum_state_interpolation_rel,
            "maximum_allowed_relative": maximum_state_interpolation_relative,
            "worst_state_self_fraction_Tgas_K_Trad_K": worst_state,
            "sample_gate_passed": (maximum_state_interpolation_rel <=
                                   maximum_state_interpolation_relative),
        }
        if not manifest["synthetic"]:
            state_interpolation_certificate = {
                "kind": "skipped_axis_knots_only_production_v1",
                "operational_use": False,
                "reason": "continuous interpolation is rejected; refined diagnostics "
                          "are synthetic-test-only to keep production work bounded",
            }
        maximum_finite_path_state_abs = 0.0
        maximum_finite_path_state_rel = 0.0
        worst_finite_path_state = None
        finite_path_state_samples = []
        for self_index, self_fraction in enumerate(diagnostic_self_fractions):
            for gas_index, gas_temperature in enumerate(evaluation_gas_temperatures):
                direct_spectrum = evaluation_spectra[self_index][gas_index]
                interpolated_spectrum = []
                for bin_index in range(len(grid)):
                    bin_tensor = [[spectra[operational_self][operational_gas][bin_index]
                                   for operational_gas in range(len(gas_temperatures))]
                                  for operational_self in range(len(self_fractions))]
                    interpolated_spectrum.append(multilinear_value(
                        [self_fractions, gas_temperatures], bin_tensor,
                        [self_fraction, gas_temperature]))
                for composition_scale in sorted({1.0, self_fraction}):
                    scaled_direct = [composition_scale * value
                                     for value in direct_spectrum]
                    scaled_interpolated = [composition_scale * value
                                           for value in interpolated_spectrum]
                    for path_length in emissivity_paths:
                        direct_emissivity = homogeneous_emissivity(
                            grid, scaled_direct, gas_temperature, path_length)
                        interpolated_emissivity = homogeneous_emissivity(
                            grid, scaled_interpolated, gas_temperature, path_length)
                        difference = abs(direct_emissivity - interpolated_emissivity)
                        relative = difference / max(abs(direct_emissivity), 1.0e-12)
                        finite_path_state_samples.append([
                            self_fraction, gas_temperature, composition_scale,
                            path_length, direct_emissivity, interpolated_emissivity])
                        maximum_finite_path_state_abs = max(
                            maximum_finite_path_state_abs, difference)
                        if relative > maximum_finite_path_state_rel:
                            maximum_finite_path_state_rel = relative
                            worst_finite_path_state = [
                                self_fraction, gas_temperature,
                                composition_scale, path_length]
        finite_path_state_certificate = {
            "kind": "direct_lbl_refined_state_finite_path_emissivity_diagnostic_v1",
            "path_lengths_m": emissivity_paths,
            "composition_scales": "unit_and_self_fraction",
            "sample_count": len(finite_path_state_samples),
            "validation_samples_sha256": hashlib.sha256(
                canonical_json_bytes(finite_path_state_samples)).hexdigest(),
            "maximum_absolute_emissivity": maximum_finite_path_state_abs,
            "maximum_relative": maximum_finite_path_state_rel,
            "maximum_allowed_relative":
                maximum_finite_path_state_interpolation_relative,
            "worst_state_self_fraction_Tgas_K_scale_path_m":
                worst_finite_path_state,
            "sample_gate_passed": (maximum_finite_path_state_rel <=
                                   maximum_finite_path_state_interpolation_relative),
        }
        if not manifest["synthetic"]:
            finite_path_state_certificate = {
                "kind": "skipped_axis_knots_only_production_v1",
                "operational_use": False,
                "reason": "finite-path state interpolation is not operational",
            }
        visible_maximum = max(value for row in visible_cell_bounds for value in row)
        if visible_maximum > maximum_visible_upper:
            raise ValueError(f"{source['species']} fails the visible-gas upper-bound gate")
        emissivity_certificate = finite_path_emissivity_refinement_certificate(
            grid, spectra, gas_temperatures, emissivity_paths,
            emissivity_remaining_limit, emissivity_contraction_limit)
        emissivity_certificate["state_scope"] = "operational_axis_knots_only"
        if not manifest["synthetic"] and not emissivity_certificate["qualified"]:
            raise ValueError(
                f"{source['species']} spectral grid fails the finite-path emissivity gate")
        species_tables.append({
            "species": source["species"],
            "release": source["release"],
            "molecule_number": int(source["molecule_number"]),
            "archive_line_count": archive_line_count,
            "line_counts_used": line_counts,
            "kappa_bin_average_per_m_per_unit_species_mole_fraction": spectra,
            "planck_mean_per_m_per_unit_species_mole_fraction": means,
            "planck_mean_interpolation_diagnostic": tensor_linear_derivative_certificate(
                [self_fractions, gas_temperatures, radiation_temperatures], means),
            "state_interpolation_diagnostic": state_interpolation_certificate,
            "finite_path_state_interpolation_diagnostic":
                finite_path_state_certificate,
            "finite_path_emissivity_grid_qualification": emissivity_certificate,
            "visible_380_780nm_conservative_upper_m-1_per_unit_species_mole_fraction": visible_maximum,
            "visible_380_780nm_maximum_allowed_m-1_per_unit_species_mole_fraction": maximum_visible_upper,
            "visible_upper_bound_domain":
                "continuous_self_fraction_and_gas_temperature_cells_v1",
            "spectral_grid_discretization": {
                "reference": "line-area-weighted Planck function evaluated at shifted line centers",
                "maximum_absolute_planck_mean_m-1_per_unit_species_mole_fraction": maximum_grid_abs,
                "maximum_relative_planck_mean": maximum_grid_rel,
                "maximum_allowed_relative_planck_mean": maximum_grid_relative,
            },
            "line_wing_sensitivity": {
                "comparison_cutoff_cm-1": cutoff * convergence_factor,
                "comparison_line_counts_used": expanded_line_counts,
                "maximum_omitted_tail_probability_bound": max(
                    value for rows in tail_bounds[0] for value in rows),
                "maximum_absolute_m-1_per_mole_fraction": maximum_abs_sensitivity,
                "maximum_relative": maximum_relative_sensitivity,
                "maximum_absolute_planck_mean_m-1_per_unit_species_mole_fraction": maximum_wing_mean_abs,
                "maximum_relative_planck_mean": maximum_wing_mean_rel,
                "maximum_allowed_relative_planck_mean": maximum_wing_relative,
            },
            "input_files": [{"path": Path(item["path"]).name,
                             "sha256": item["sha256"]}
                            for item in source["files"]],
            "partition_sums": {
                "files": [{"molecule": int(item["molecule"]),
                           "isotopologue": int(item["isotopologue"]),
                           "sha256": item["sha256"]}
                          for item in source["partition_sums"]["files"]],
                "citation": source["partition_sums"]["citation"],
            },
        })

    payload = {
        "schema": "rise-fire-gas-opacity-table-v1",
        "record_status": ("synthetic_test_only" if manifest["synthetic"]
                          else "production_derived"),
        "synthetic": bool(manifest["synthetic"]),
        "pressure_Pa": pressure_pa,
        "composition_basis": "unit-species opacity times species mole fraction at total pressure",
        "composition_domain": {
            "H2O_mole_fraction": [0.0, 1.0],
            "CO2_mole_fraction": [0.0, 1.0],
            "simplex_constraint": "x_H2O+x_CO2<=1; balance is air broadener",
        },
        "self_broadening_mole_fractions": self_fractions,
        "broadening_convention": ("linear gamma_air/gamma_self blend at the species mole "
                                  "fraction; HITRAN n_air exponent applies to both terms; "
                                  "pressure shift uses total pressure"),
        "band_overlap_rule": "spectral absorption coefficients add before Planck integration",
        "spectral_coordinate": "vacuum_wavenumber_cm-1",
        "state_lookup_policy": "axis_knots_only_reject_interior_v1",
        "production_resource_budget": (
            {"kind": "synthetic_test_fixture_not_production_budgeted"}
            if manifest["synthetic"] else resource_budget),
        "spectral_value_semantics": "uniform finite-volume bin average, area-preserving deposition",
        "spectral_grid_cm-1": grid,
        "spectral_bin_edge_domain_cm-1": spectral_bin_edges,
        "wavelength_domain_um": [1.0e4 / spectral_bin_edges[-1],
                                 1.0e4 / spectral_bin_edges[0]],
        "gas_temperatures_K": gas_temperatures,
        "radiation_temperatures_K": radiation_temperatures,
        "line_wing_cutoff_cm-1": cutoff,
        "voigt_algorithm": ("32-point Gauss-Legendre Gaussian/Cauchy convolution of "
                            "analytic interval CDFs; exact Gaussian/Lorentz degenerate "
                            "limits; binary64"),
        "archive_passes_per_species": 1,
        "accumulator_backend": ("python_reference_test_only" if native_library is None
                                else "native_streaming_v1"),
        "out_of_domain_policy": "reject",
        "species_tables": species_tables,
        "provenance": {
            "generator_sha256": generator_identity(),
            "native_accumulator_library_sha256": (
                sha256_file(native_library) if native_library is not None else None),
            "hitemp_citations": manifest["hitemp_citations"],
            "hitran_definitions": "https://hitran.org/docs/definitions-and-units/",
            "original_source_citations": manifest["original_source_citations"],
            "line_bytes_committed": False,
            "input_manifest_sha256": sha256_file(manifest_path),
        },
    }
    payload["canonical_payload_without_identity_sha256"] = hashlib.sha256(
        canonical_json_bytes(payload)).hexdigest()
    validate_opacity_table(
        payload, allow_synthetic=bool(manifest["synthetic"]),
        expected_payload_identity=(
            payload["canonical_payload_without_identity_sha256"]
            if not manifest["synthetic"] else None))
    return payload


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--input-root", type=Path)
    parser.add_argument("--native-library", type=Path,
                        help="required compiled accumulator for production manifests")
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    generated = canonical_json_bytes(generate(
        args.manifest, args.input_root, args.native_library))
    if args.check:
        if not args.output.is_file() or args.output.read_bytes() != generated:
            raise SystemExit(f"{args.output} is stale; regenerate with {Path(__file__).name}")
        return
    args.output.write_bytes(generated)


if __name__ == "__main__":
    main()
