#!/usr/bin/env python3
"""Cross-check a generated opacity table against the CC-BY EM2C-SNB data.

Dataset: DOI 10.17632/x5wjzk6sjs.1.  The Mendeley bytes are independent
reference inputs and are not required to live in the repository.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

from fire_gas_opacity import (
    C2_CM_K, multilinear_value, planck_weight_wavenumber, sha256_file,
)


DOI = "10.17632/x5wjzk6sjs.1"
EM2C_SHA256 = {
    "R=00.000_EM2C-SNB_totalEmissivities_90x105.dat": "b54b8824298e41d6199dc557e970318daf020b3bc6b99dacc7f92ff55a7f7b6d",
    "R=00.050_EM2C-SNB_totalEmissivities_90x105.dat": "e60f22f96fc9ae17d8c17bd45384aa861a6521f6e5a672c097e11df625791cdf",
    "R=00.125_EM2C-SNB_totalEmissivities_90x105.dat": "6c004fe64c2e4573fd04319bd07ba6e8837308bffc1c3bbb6d483e2a56a7b9f2",
    "R=00.250_EM2C-SNB_totalEmissivities_90x105.dat": "17c724658e82ae617bdfc11ebcd988012bd2d50eee6191d19708023876a25d0c",
    "R=00.500_EM2C-SNB_totalEmissivities_90x105.dat": "926505c5b358ac09a616199d27d8c973122d9f6475731dd69b7eef8948dff073",
    "R=01.000_EM2C-SNB_totalEmissivities_90x105.dat": "aee3457c28088300a536a29f1b8f7027e4e63fe2e1b93664b3e6839f687db5a5",
    "R=02.000_EM2C-SNB_totalEmissivities_90x105.dat": "f8a5a114098db367479e3ca75b261a2ec06a6f329f1824f6ec0426d843f484ad",
    "R=05.000_EM2C-SNB_totalEmissivities_90x105.dat": "7275c95d8f502822643c9c2e798960fcfaca84ed725b52ee296e100e266e7a76",
    "R=20.000_EM2C-SNB_totalEmissivities_90x105.dat": "ef5ede24be20b17424a43984a4dd8a824a30991a9a21d8ca1c34c7f2b6a74ce3",
    "R=Infinity_EM2C-SNB_totalEmissivities_90x105.dat": "c18807d6074079b9b9e1cab9cf30887950aaa96e57ba690a7da03b909e435307",
}


def spectrum_at_temperature(table: dict, species: dict, temperature_k: float,
                            self_mole_fraction: float) -> list[float]:
    self_axis = table["self_broadening_mole_fractions"]
    temperatures = table["gas_temperatures_K"]
    spectra = species["kappa_bin_average_per_m_per_unit_species_mole_fraction"]
    result = []
    for column in range(len(table["spectral_grid_cm-1"])):
        values = [[spectra[self_index][gas_index][column]
                   for gas_index in range(len(temperatures))]
                  for self_index in range(len(self_axis))]
        result.append(multilinear_value(
            [self_axis, temperatures], values,
            [self_mole_fraction, temperature_k]))
    return result


def homogeneous_emissivity(grid: list[float], kappa: list[float],
                           temperature_k: float, path_length_m: float) -> float:
    weights = [planck_weight_wavenumber(value, temperature_k) for value in grid]
    step = grid[1] - grid[0]
    if (step <= 0.0 or any(not math.isclose(grid[index + 1] - grid[index], step,
                                           rel_tol=0.0, abs_tol=1.0e-12 * step)
                          for index in range(len(grid) - 1))):
        raise ValueError("opacity table does not use uniform spectral bins")
    numerator = step * sum(-math.expm1(-value * path_length_m) * weight
                           for value, weight in zip(kappa, weights))
    full_blackbody_weight = (math.pi ** 4 / 15.0) * (temperature_k / C2_CM_K) ** 4
    return numerator / full_blackbody_weight


def parse_em2c(path: Path, verify_digest: bool = True) -> list[tuple[float, float, float]]:
    if verify_digest:
        expected = EM2C_SHA256.get(path.name)
        if not expected or sha256_file(path) != expected:
            raise ValueError("EM2C filename or SHA-256 is not the pinned V1 dataset")
    lines = path.read_text(encoding="utf-8").splitlines()
    header = next((index for index, line in enumerate(lines)
                   if "total-emissivity[dimLESS]" in line), None)
    if header is None:
        raise ValueError("EM2C total-emissivity table header is missing")
    rows = []
    for line_number, raw in enumerate(lines[header + 1:], header + 2):
        text = raw.strip()
        if not text:
            continue
        fields = text.split()
        if len(fields) != 3:
            raise ValueError(f"{path}:{line_number}: malformed EM2C data row")
        pressure_path, temperature, emissivity = map(float, fields)
        if (not all(math.isfinite(value) for value in
                    (pressure_path, temperature, emissivity)) or
                pressure_path <= 0.0 or emissivity < 0.0 or emissivity > 1.0):
            raise ValueError(f"{path}:{line_number}: inadmissible EM2C value")
        rows.append((temperature, pressure_path, emissivity))
    if len(rows) != 90 * 105:
        raise ValueError("EM2C table does not contain 90x105 data rows")
    for path_index in range(90):
        block = rows[path_index * 105:(path_index + 1) * 105]
        pressure_path = block[0][1]
        if (path_index == 0 and not math.isclose(pressure_path, 0.01, rel_tol=0.0, abs_tol=1e-12)) or (
                path_index == 89 and not math.isclose(pressure_path, 50.0, rel_tol=0.0, abs_tol=1e-9)):
            raise ValueError("EM2C pressure-pathlength endpoints are invalid")
        for temp_index, (temperature, row_path, _) in enumerate(block):
            if temperature != 300.0 + 25.0 * temp_index or row_path != pressure_path:
                raise ValueError("EM2C 90x105 row ordering is invalid")
    return rows


def ratio_value(text: str) -> float:
    if text.lower() in {"inf", "infinity"}:
        return math.inf
    value = float(text)
    if value < 0.0:
        raise argparse.ArgumentTypeError("H2O:CO2 ratio must be nonnegative")
    return value


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", action="append", required=True,
                        help="H2O:CO2 ratio and EM2C file as RATIO=PATH")
    parser.add_argument("--relative-tolerance", type=float, default=0.05,
                        help="provisional gate; reset from the first real comparison")
    parser.add_argument("table", type=Path)
    args = parser.parse_args()
    table = json.loads(args.table.read_text(encoding="utf-8"))
    if table.get("synthetic"):
        raise ValueError("EM2C physical cross-check refuses a synthetic opacity table")
    species = {entry["species"]: entry for entry in table["species_tables"]}
    grid = table["spectral_grid_cm-1"]
    failures = 0
    worst = (0.0, "")
    for case in args.case:
        ratio_text, separator, path_text = case.partition("=")
        if not separator:
            raise ValueError("--case must have the form RATIO=PATH")
        ratio = ratio_value(ratio_text)
        x_h2o = 1.0 if math.isinf(ratio) else ratio / (1.0 + ratio)
        x_co2 = 0.0 if math.isinf(ratio) else 1.0 / (1.0 + ratio)
        cached: dict[float, list[float]] = {}
        for temperature, pressure_path_atm_m, reference in parse_em2c(Path(path_text)):
            if temperature not in cached:
                h2o = spectrum_at_temperature(
                    table, species["H2O"], temperature, x_h2o)
                co2 = spectrum_at_temperature(
                    table, species["CO2"], temperature, x_co2)
                cached[temperature] = [x_h2o * a + x_co2 * b for a, b in zip(h2o, co2)]
            predicted = homogeneous_emissivity(
                grid, cached[temperature], temperature, pressure_path_atm_m)
            relative = abs(predicted - reference) / max(abs(reference), 1.0e-12)
            label = f"R={ratio_text}, T={temperature:g} K, pL={pressure_path_atm_m:g} atm.m"
            if relative > worst[0]:
                worst = (relative, label)
            failures += relative > args.relative_tolerance
    print(f"EM2C-SNB {DOI}: worst relative error {worst[0]:.6g} at {worst[1]}")
    if failures:
        raise SystemExit(f"{failures} EM2C comparison row(s) exceed {args.relative_tolerance:.3%}")


if __name__ == "__main__":
    main()
