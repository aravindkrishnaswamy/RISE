#!/usr/bin/env python3
"""Corroborate the production Planck means against EM2C's thin-path end.

The EM2C-SNB dataset is CC-BY 4.0, DOI 10.17632/x5wjzk6sjs.1.  This harness
uses only rows satisfying kappa_P*pL <= 0.05 and compares epsilon/pL with the
continuous production Planck mean.  It is deliberately corroborative: the
Planck-mean-specific published comparisons remain the qualification sources.
No Voigt spectrum or line-shape code is used here.
"""

from __future__ import annotations

import argparse
import hashlib
import math
from pathlib import Path

from generate_fire_gas_opacity_planck_record import AXIS, bicubic_coefficients, payload


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
K_BOLTZMANN = 1.380649e-23
REFERENCE_PRESSURE_PA = 101325.0


def ratio(path: Path) -> float:
    text = path.name.partition("_EM2C-SNB_")[0].removeprefix("R=")
    return math.inf if text == "Infinity" else float(text)


def parse(path: Path) -> list[tuple[float, float, float]]:
    expected = EM2C_SHA256.get(path.name)
    if expected is None or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise ValueError("EM2C filename or SHA-256 is not pinned")
    lines = path.read_text(encoding="utf-8").splitlines()
    path_header = next(i for i, line in enumerate(lines) if "iPL" in line)
    table_header = next(i for i, line in enumerate(lines) if "total-emissivity" in line)
    paths = []
    for line in lines[path_header + 1:table_header]:
        fields = line.split()
        if fields:
            if len(fields) != 2 or int(fields[0]) != len(paths) + 1:
                raise ValueError("EM2C path header is malformed")
            paths.append(float(fields[1]))
    rows = []
    for line in lines[table_header + 1:]:
        fields = line.split()
        if fields:
            if len(fields) != 3:
                raise ValueError("EM2C data row is malformed")
            rows.append(tuple(map(float, fields)))
    if len(paths) != 90 or len(rows) != 90 * 105:
        raise ValueError("EM2C table shape is not 90x105")
    result = []
    for path_index, pressure_path in enumerate(paths):
        for temperature_index in range(105):
            row_path, temperature, emissivity = rows[path_index * 105 + temperature_index]
            if (temperature != 300.0 + 25.0 * temperature_index or
                    not math.isclose(row_path, round(pressure_path, 5), abs_tol=1.0e-12)):
                raise ValueError("EM2C row ordering or rounded path is invalid")
            result.append((temperature, pressure_path, emissivity))
    return result


def sigma(species: dict, gas_temperature: float, radiation_temperature: float) -> float:
    count = len(AXIS)
    gas = min(count - 2, int((gas_temperature - AXIS[0]) // 50.0))
    radiation = min(count - 2, int((radiation_temperature - AXIS[0]) // 50.0))
    nodes = species["interpolation"]["nodes_row_major"]
    corners = [[nodes[(gas + dx) * count + radiation + dy]
                for dy in range(2)] for dx in range(2)]
    coefficients = bicubic_coefficients(corners, 50.0)
    u = (gas_temperature - AXIS[gas]) / 50.0
    v = (radiation_temperature - AXIS[radiation]) / 50.0
    return sum(coefficients[i][j] * u ** i * v ** j
               for i in range(4) for j in range(4))


def check(manifest: Path, reference: Path) -> dict:
    record = payload(manifest)
    species = {entry["species_id"]: entry for entry in record["species"]}
    h2o_to_co2 = ratio(reference)
    h2o_fraction = 1.0 if math.isinf(h2o_to_co2) else h2o_to_co2 / (1.0 + h2o_to_co2)
    comparisons = []
    for temperature, pressure_path, emissivity in parse(reference):
        if temperature > AXIS[-1]:
            continue
        mixture_sigma = (h2o_fraction * sigma(species["H2O"], temperature, temperature) +
                         (1.0 - h2o_fraction) * sigma(
                             species["CO2"], temperature, temperature))
        kappa = mixture_sigma * REFERENCE_PRESSURE_PA / (K_BOLTZMANN * temperature)
        if kappa * pressure_path <= 0.05:
            reference_kappa = emissivity / pressure_path
            comparisons.append(abs(reference_kappa - kappa) / kappa)
    if not comparisons:
        raise ValueError("EM2C file has no rows inside the declared thin-limit criterion")
    return {"eligible_rows": len(comparisons),
            "maximum_relative_difference": max(comparisons),
            "mean_relative_difference": sum(comparisons) / len(comparisons)}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("references", nargs="+", type=Path)
    args = parser.parse_args()
    for reference in args.references:
        result = check(args.manifest, reference)
        print(reference.name, " ".join(f"{key}={value}" for key, value in result.items()))


if __name__ == "__main__":
    main()
