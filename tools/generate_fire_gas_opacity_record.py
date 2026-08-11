#!/usr/bin/env python3
"""Generate derived HITEMP CO2/H2O spectra and two-temperature Planck means."""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
from pathlib import Path

from fire_gas_opacity import (
    PartitionSums, canonical_json_bytes, iter_hitran_lines,
    planck_mean_wavenumber, sha256_file, species_spectrum, spectral_grid,
)
from generate_fire_optics_records import tabulated_spectrum


SCHEMA = "rise-fire-gas-opacity-generator-input-v1"


def _is_digest(value: object) -> bool:
    return (isinstance(value, str) and len(value) == 64 and
            all(character in "0123456789abcdef" for character in value))


def _resolved(root: Path, text: str) -> Path:
    path = Path(text)
    return path if path.is_absolute() else root / path


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
    seen_species: set[str] = set()
    for source in manifest.get("sources", []):
        species = source.get("species")
        if species not in {"H2O", "CO2"} or species in seen_species:
            raise ValueError("manifest must contain unique H2O and CO2 sources")
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


def _interpolation_certificate(gas_temperatures: list[float],
                               radiation_temperatures: list[float],
                               matrix: list[list[float]]) -> dict:
    radiation = [tabulated_spectrum(radiation_temperatures, row) for row in matrix]
    gas = [tabulated_spectrum(gas_temperatures,
                              [matrix[row][column] for row in range(len(matrix))])
           for column in range(len(radiation_temperatures))]
    return {
        "kind": "tensor_pchip_c1_v1",
        "evaluation_order": "gas_temperature_then_radiation_temperature",
        "radiation_temperature_rows": radiation,
        "gas_temperature_columns": gas,
    }


def generate(manifest_path: Path, input_root: Path | None) -> dict:
    manifest, root = load_verified_manifest(manifest_path, input_root)
    grid_config = manifest["spectral_grid_cm-1"]
    grid = spectral_grid(float(grid_config["minimum"]), float(grid_config["maximum"]),
                         float(grid_config["step"]))
    gas_temperatures = [float(value) for value in manifest["gas_temperatures_K"]]
    radiation_temperatures = [float(value) for value in
                              manifest["radiation_temperatures_K"]]
    if (len(gas_temperatures) < 2 or len(radiation_temperatures) < 2 or
            any(gas_temperatures[index] >= gas_temperatures[index + 1]
                for index in range(len(gas_temperatures) - 1)) or
            any(radiation_temperatures[index] >= radiation_temperatures[index + 1]
                for index in range(len(radiation_temperatures) - 1))):
        raise ValueError("temperature grids must contain increasing closed intervals")
    pressure_pa = float(manifest["pressure_Pa"])
    cutoff = float(manifest["line_wing_cutoff_cm-1"])
    convergence_factor = float(manifest["line_wing_convergence_factor"])
    if convergence_factor <= 1.0:
        raise ValueError("line-wing convergence factor must exceed one")

    species_tables = []
    for source in manifest["sources"]:
        partition = _partition_sums(source, root)
        masses = {int(key): float(value) for key, value in
                  source["isotopologue_molar_masses_kg_per_mol"].items()}
        spectra: list[list[float]] = []
        means: list[list[float]] = []
        line_counts: list[int] = []
        expanded_line_counts: list[int] = []
        maximum_abs_sensitivity = 0.0
        maximum_relative_sensitivity = 0.0
        for gas_temperature in gas_temperatures:
            spectrum, count = species_spectrum(
                _line_iterator(source, root), int(source["molecule_number"]), masses,
                partition, grid, gas_temperature, pressure_pa, cutoff)
            expanded, expanded_count = species_spectrum(
                _line_iterator(source, root), int(source["molecule_number"]), masses,
                partition, grid, gas_temperature, pressure_pa,
                cutoff * convergence_factor)
            for value, reference in zip(spectrum, expanded):
                difference = abs(value - reference)
                maximum_abs_sensitivity = max(maximum_abs_sensitivity, difference)
                maximum_relative_sensitivity = max(
                    maximum_relative_sensitivity,
                    difference / max(abs(reference), 1.0e-300))
            spectra.append(spectrum)
            line_counts.append(count)
            expanded_line_counts.append(expanded_count)
            means.append([
                planck_mean_wavenumber(grid, spectrum, temperature)
                for temperature in radiation_temperatures
            ])
        visible_indices = [index for index, wn in enumerate(grid)
                           if 1.0e7 / 780.0 <= wn <= 1.0e7 / 380.0]
        visible_maximum = max(
            (spectra[row][index] for row in range(len(spectra))
             for index in visible_indices), default=0.0)
        species_tables.append({
            "species": source["species"],
            "release": source["release"],
            "molecule_number": int(source["molecule_number"]),
            "line_counts_used": line_counts,
            "kappa_per_m_per_mole_fraction": spectra,
            "planck_mean_per_m_per_mole_fraction": means,
            "planck_mean_interpolation": _interpolation_certificate(
                gas_temperatures, radiation_temperatures, means),
            "visible_380_780nm_maximum_per_m_per_mole_fraction": visible_maximum,
            "line_wing_sensitivity": {
                "comparison_cutoff_cm-1": cutoff * convergence_factor,
                "comparison_line_counts_used": expanded_line_counts,
                "maximum_absolute_m-1_per_mole_fraction": maximum_abs_sensitivity,
                "maximum_relative": maximum_relative_sensitivity,
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

    generator_paths = [Path(__file__).resolve(),
                       Path(__file__).with_name("fire_gas_opacity.py").resolve()]
    generator_digest = hashlib.sha256(
        b"".join(path.read_bytes() for path in generator_paths)).hexdigest()
    payload = {
        "schema": "rise-fire-gas-opacity-table-v1",
        "record_status": ("synthetic_test_only" if manifest["synthetic"]
                          else "production_derived"),
        "synthetic": bool(manifest["synthetic"]),
        "pressure_Pa": pressure_pa,
        "composition_basis": "m^-1 per species mole fraction at total pressure",
        "composition_domain": {
            "H2O_mole_fraction": [0.0, 1.0],
            "CO2_mole_fraction": [0.0, 1.0],
            "simplex_constraint": "x_H2O+x_CO2<=1; balance is air broadener",
        },
        "broadening_convention": ("air-broadened Voigt; gamma_air and pressure shift "
                                  "at total pressure; linear additive species opacity"),
        "band_overlap_rule": "spectral absorption coefficients add before Planck integration",
        "spectral_coordinate": "vacuum_wavenumber_cm-1",
        "spectral_grid_cm-1": grid,
        "wavelength_domain_um": [1.0e4 / grid[-1], 1.0e4 / grid[0]],
        "gas_temperatures_K": gas_temperatures,
        "radiation_temperatures_K": radiation_temperatures,
        "line_wing_cutoff_cm-1": cutoff,
        "voigt_algorithm": "Humlicek-W4 deterministic binary64",
        "out_of_domain_policy": "reject",
        "species_tables": species_tables,
        "provenance": {
            "generator_sha256": generator_digest,
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
