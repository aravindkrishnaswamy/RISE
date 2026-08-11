#!/usr/bin/env python3
"""Generate derived HITEMP CO2/H2O spectra and two-temperature Planck means."""

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
    PartitionSums, canonical_json_bytes, iter_hitran_lines,
    multilinear_value, planck_mean_wavenumber, sha256_file,
    species_spectra_batch, spectral_grid, tensor_linear_derivative_certificate,
)


SCHEMA = "rise-fire-gas-opacity-generator-input-v1"
EXPECTED_MOLECULES = {"H2O": 1, "CO2": 2}
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


def load_verified_manifest(path: Path, input_root: Path | None) -> tuple[dict, Path]:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("schema") != SCHEMA:
        raise ValueError("HITEMP input manifest schema is unsupported")
    status = manifest.get("status")
    if status not in {"synthetic_test_fixture", "production_pinned"}:
        raise ValueError("HITEMP inputs remain owner-download pending; no table can be generated")
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
            if species == "CO2" and mass_keys != set(range(1, 13)):
                raise ValueError("CO2 HITEMP2024 requires isotopologues 1 through 12")
            repo_root = Path(__file__).resolve().parent.parent
            local_lines = [_resolved(root, item["path"]).resolve() for item in files]
            if any(local == repo_root or repo_root in local.parents for local in local_lines):
                raise ValueError("production HITEMP line bytes must remain outside the repository")
            if species == "CO2" and not (len(files) == 1 and
                    Path(files[0]["path"]).name == "02_HITEMP2024.par.bz2" and
                    files[0]["compression"] == "bzip2" and source.get("release") == "HITEMP2024"):
                raise ValueError("CO2 production input is not the pinned HITEMP2024 archive")
            if species == "H2O" and (source.get("release") != "HITEMP2010" or
                    any(not Path(item["path"]).name.endswith(".par") for item in files)):
                raise ValueError("H2O production inventory is not HITEMP2010 .par data")
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


def evaluate_species_planck_mean(table: dict, species_name: str,
                                 self_mole_fraction: float,
                                 gas_temperature_k: float,
                                 radiation_temperature_k: float,
                                 pressure_pa: float) -> float:
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
    if certificate != species.get("planck_mean_interpolation"):
        raise ValueError("opacity interpolation certificate does not bind the table")
    return multilinear_value(
        axes, species["planck_mean_per_m_per_unit_species_mole_fraction"],
        [self_mole_fraction, gas_temperature_k, radiation_temperature_k])


def evaluate_mixture_planck_mean(table: dict, h2o_mole_fraction: float,
                                 co2_mole_fraction: float,
                                 gas_temperature_k: float,
                                 radiation_temperature_k: float,
                                 pressure_pa: float) -> float:
    fractions = (h2o_mole_fraction, co2_mole_fraction)
    if (any(not math.isfinite(value) or value < 0.0 for value in fractions) or
            sum(fractions) > 1.0):
        raise ValueError("opacity composition is outside the air-balance simplex")
    return sum(fraction * evaluate_species_planck_mean(
        table, species, fraction, gas_temperature_k,
        radiation_temperature_k, pressure_pa)
               for fraction, species in zip(fractions, ("H2O", "CO2")))


def generate(manifest_path: Path, input_root: Path | None) -> dict:
    manifest, root = load_verified_manifest(manifest_path, input_root)
    grid_config = manifest["spectral_grid_cm-1"]
    grid = spectral_grid(float(grid_config["minimum"]), float(grid_config["maximum"]),
                         float(grid_config["step"]))
    gas_temperatures = [float(value) for value in manifest["gas_temperatures_K"]]
    radiation_temperatures = [float(value) for value in
                              manifest["radiation_temperatures_K"]]
    self_fractions = [float(value) for value in
                      manifest["self_broadening_mole_fractions"]]
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
    pressure_pa = float(manifest["pressure_Pa"])
    grid_step = grid[1] - grid[0]
    spectral_bin_edges = [grid[0] - 0.5 * grid_step,
                          grid[-1] + 0.5 * grid_step]
    if spectral_bin_edges[0] <= 0.0:
        raise ValueError("spectral bins must have positive wavenumber edges")
    cutoff = float(manifest["line_wing_cutoff_cm-1"])
    convergence_factor = float(manifest["line_wing_convergence_factor"])
    maximum_grid_relative = float(manifest["maximum_planck_mean_grid_relative_error"])
    maximum_wing_relative = float(manifest["maximum_planck_mean_wing_relative_error"])
    if (convergence_factor <= 1.0 or maximum_grid_relative <= 0.0 or
            maximum_wing_relative <= 0.0):
        raise ValueError("line-wing convergence factor must exceed one")

    species_tables = []
    for source in manifest["sources"]:
        partition = _partition_sums(source, root)
        masses = {int(key): float(value) for key, value in
                  source["isotopologue_molar_masses_kg_per_mol"].items()}
        all_spectra, all_counts, tail_bounds, center_means = species_spectra_batch(
            _line_iterator(source, root), int(source["molecule_number"]), masses,
            partition, grid, gas_temperatures, pressure_pa, self_fractions,
            [cutoff, cutoff * convergence_factor], radiation_temperatures)
        spectra = all_spectra[0]
        expanded = all_spectra[1]
        line_counts = all_counts[0]
        expanded_line_counts = all_counts[1]
        if (any(count <= 0 for row in line_counts for count in row) or
                any(count <= 0 for row in expanded_line_counts for count in row)):
            raise ValueError(f"{source['species']} archive contributes no lines to a table state")
        means = [[[
            planck_mean_wavenumber(grid, spectra[self_index][gas_index], temperature)
            for temperature in radiation_temperatures
        ] for gas_index in range(len(gas_temperatures))]
                 for self_index in range(len(self_fractions))]
        expanded_means = [[[
            planck_mean_wavenumber(grid, expanded[self_index][gas_index], temperature)
            for temperature in radiation_temperatures
        ] for gas_index in range(len(gas_temperatures))]
                          for self_index in range(len(self_fractions))]
        maximum_abs_sensitivity = 0.0
        maximum_relative_sensitivity = 0.0
        for self_index in range(len(self_fractions)):
            for gas_index in range(len(gas_temperatures)):
                for value, reference in zip(
                        spectra[self_index][gas_index],
                        expanded[self_index][gas_index]):
                    difference = abs(value - reference)
                    maximum_abs_sensitivity = max(maximum_abs_sensitivity, difference)
                    maximum_relative_sensitivity = max(
                        maximum_relative_sensitivity,
                        difference / max(abs(reference), 1.0e-300))
        maximum_grid_abs = 0.0
        maximum_grid_rel = 0.0
        maximum_wing_mean_abs = 0.0
        maximum_wing_mean_rel = 0.0
        for self_index, self_rows in enumerate(means):
            for gas_index, row in enumerate(self_rows):
                for radiation_index, value in enumerate(row):
                    reference = center_means[gas_index][radiation_index]
                    difference = abs(value - reference)
                    maximum_grid_abs = max(maximum_grid_abs, difference)
                    maximum_grid_rel = max(maximum_grid_rel,
                                           difference / max(abs(reference), 1.0e-300))
                    wing_reference = expanded_means[self_index][gas_index][radiation_index]
                    wing_difference = abs(value - wing_reference)
                    maximum_wing_mean_abs = max(maximum_wing_mean_abs, wing_difference)
                    maximum_wing_mean_rel = max(
                        maximum_wing_mean_rel,
                        wing_difference / max(abs(wing_reference), 1.0e-300))
        if maximum_grid_rel > maximum_grid_relative:
            raise ValueError(f"{source['species']} spectral grid fails its Planck-mean gate")
        if maximum_wing_mean_rel > maximum_wing_relative:
            raise ValueError(f"{source['species']} line-wing comparison fails its Planck-mean gate")
        visible_indices = [
            index for index, wn in enumerate(grid)
            if (wn + 0.5 * grid_step >= 1.0e7 / 780.0 and
                wn - 0.5 * grid_step <= 1.0e7 / 380.0)
        ]
        visible_maximum = max(
            (gas_row[index] for self_rows in spectra for gas_row in self_rows
             for index in visible_indices), default=0.0)
        species_tables.append({
            "species": source["species"],
            "release": source["release"],
            "molecule_number": int(source["molecule_number"]),
            "line_counts_used": line_counts,
            "kappa_bin_average_per_m_per_unit_species_mole_fraction": spectra,
            "planck_mean_per_m_per_unit_species_mole_fraction": means,
            "planck_mean_interpolation": tensor_linear_derivative_certificate(
                [self_fractions, gas_temperatures, radiation_temperatures], means),
            "visible_380_780nm_maximum_bin_average_per_m_per_unit_species_mole_fraction": visible_maximum,
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
        "out_of_domain_policy": "reject",
        "species_tables": species_tables,
        "provenance": {
            "generator_sha256": generator_identity(),
            "hitemp_citations": manifest["hitemp_citations"],
            "hitran_definitions": "https://hitran.org/docs/definitions-and-units/",
            "original_source_citations": manifest["original_source_citations"],
            "line_bytes_committed": False,
            "input_manifest_sha256": sha256_file(manifest_path),
        },
    }
    payload["canonical_payload_without_identity_sha256"] = hashlib.sha256(
        canonical_json_bytes(payload)).hexdigest()
    return payload


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--input-root", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    generated = canonical_json_bytes(generate(args.manifest, args.input_root))
    if args.check:
        if not args.output.is_file() or args.output.read_bytes() != generated:
            raise SystemExit(f"{args.output} is stale; regenerate with {Path(__file__).name}")
        return
    args.output.write_bytes(generated)


if __name__ == "__main__":
    main()
