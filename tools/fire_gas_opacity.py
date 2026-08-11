#!/usr/bin/env python3
"""Deterministic HITEMP/HITRAN line-by-line gas-opacity primitives.

Line archives remain owner-local.  This module accepts only hash-verified
inputs and emits derived spectral absorption and two-temperature Planck means.
"""

from __future__ import annotations

import bz2
import hashlib
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator


C2_CM_K = 1.4387768775039338
K_BOLTZMANN = 1.380649e-23
C_LIGHT = 299792458.0
N_AVOGADRO = 6.02214076e23
REFERENCE_TEMPERATURE_K = 296.0
REFERENCE_PRESSURE_PA = 101325.0


@dataclass(frozen=True)
class HitranLine:
    molecule: int
    isotopologue: int
    center_cm1: float
    intensity_296_cm_per_molecule: float
    einstein_a_s1: float
    gamma_air_cm1_atm: float
    gamma_self_cm1_atm: float
    lower_energy_cm1: float
    n_air: float
    pressure_shift_cm1_atm: float


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _finite_field(text: str, label: str) -> float:
    try:
        value = float(text.replace("D", "E"))
    except ValueError as exc:
        raise ValueError(f"invalid HITRAN {label} field {text!r}") from exc
    if not math.isfinite(value):
        raise ValueError(f"non-finite HITRAN {label}")
    return value


def parse_hitran160(line: str) -> HitranLine:
    """Parse the physical fields in a HITRAN/HITEMP 160-character record."""
    if len(line) != 160:
        raise ValueError(f"HITRAN record must contain exactly 160 characters, got {len(line)}")
    try:
        molecule = int(line[0:2])
        isotopologue = int(line[2:3])
    except ValueError as exc:
        raise ValueError("invalid HITRAN molecule/isotopologue field") from exc
    result = HitranLine(
        molecule=molecule,
        isotopologue=isotopologue,
        center_cm1=_finite_field(line[3:15], "line center"),
        intensity_296_cm_per_molecule=_finite_field(line[15:25], "line intensity"),
        einstein_a_s1=_finite_field(line[25:35], "Einstein A"),
        gamma_air_cm1_atm=_finite_field(line[35:40], "air width"),
        gamma_self_cm1_atm=_finite_field(line[40:45], "self width"),
        lower_energy_cm1=_finite_field(line[45:55], "lower-state energy"),
        n_air=_finite_field(line[55:59], "temperature exponent"),
        pressure_shift_cm1_atm=_finite_field(line[59:67], "pressure shift"),
    )
    if (result.molecule <= 0 or result.isotopologue <= 0 or
            result.center_cm1 <= 0.0 or result.intensity_296_cm_per_molecule < 0.0 or
            result.gamma_air_cm1_atm < 0.0 or result.gamma_self_cm1_atm < 0.0 or
            result.lower_energy_cm1 < 0.0):
        raise ValueError("HITRAN record has an inadmissible physical field")
    return result


def iter_hitran_lines(path: Path, compression: str) -> Iterator[HitranLine]:
    opener = bz2.open if compression == "bzip2" else open
    if compression not in {"none", "bzip2"}:
        raise ValueError(f"unsupported line-list compression {compression!r}")
    with opener(path, "rt", encoding="ascii", newline="") as source:
        for line_number, raw in enumerate(source, 1):
            text = raw.rstrip("\r\n")
            if not text:
                continue
            try:
                yield parse_hitran160(text)
            except ValueError as exc:
                raise ValueError(f"{path}:{line_number}: {exc}") from exc


class PartitionSums:
    def __init__(self, rows: dict[tuple[int, int], list[tuple[float, float]]]):
        self._rows = rows

    @classmethod
    def load(cls, path: Path) -> "PartitionSums":
        value = json.loads(path.read_text(encoding="utf-8"))
        if value.get("schema") != "rise-hitran-partition-sums-v1":
            raise ValueError("partition-sum schema is unsupported")
        rows: dict[tuple[int, int], list[tuple[float, float]]] = {}
        for item in value.get("isotopologues", []):
            key = (int(item["molecule"]), int(item["isotopologue"]))
            samples = [(float(row[0]), float(row[1])) for row in item["rows"]]
            if (key in rows or len(samples) < 2 or
                    any(not math.isfinite(t) or not math.isfinite(q) or t <= 0.0 or q <= 0.0
                        for t, q in samples) or
                    any(samples[index][0] >= samples[index + 1][0]
                        for index in range(len(samples) - 1))):
                raise ValueError("partition-sum table is malformed")
            rows[key] = samples
        if not rows:
            raise ValueError("partition-sum table is empty")
        return cls(rows)

    @classmethod
    def from_hitran_q_files(cls, entries: Iterable[tuple[int, int, Path]]) -> "PartitionSums":
        rows: dict[tuple[int, int], list[tuple[float, float]]] = {}
        for molecule, isotopologue, path in entries:
            key = (molecule, isotopologue)
            if key in rows:
                raise ValueError("duplicate HITRAN partition-sum isotopologue")
            samples = []
            for line_number, raw in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
                text = raw.strip()
                if not text or text.startswith(("#", "!", "%")):
                    continue
                fields = text.replace(",", " ").split()
                if len(fields) < 2:
                    raise ValueError(f"{path}:{line_number}: expected temperature and Q")
                temperature = _finite_field(fields[0], "partition temperature")
                value = _finite_field(fields[1], "partition sum")
                samples.append((temperature, value))
            if (len(samples) < 2 or any(t <= 0.0 or q <= 0.0 for t, q in samples) or
                    any(samples[index][0] >= samples[index + 1][0]
                        for index in range(len(samples) - 1))):
                raise ValueError(f"{path}: partition-sum rows are malformed")
            rows[key] = samples
        if not rows:
            raise ValueError("partition-sum file inventory is empty")
        return cls(rows)

    def evaluate(self, molecule: int, isotopologue: int, temperature_k: float) -> float:
        samples = self._rows.get((molecule, isotopologue))
        if not samples or temperature_k < samples[0][0] or temperature_k > samples[-1][0]:
            raise ValueError("partition-sum lookup is out of domain")
        for index in range(len(samples) - 1):
            t0, q0 = samples[index]
            t1, q1 = samples[index + 1]
            if temperature_k <= t1:
                fraction = (temperature_k - t0) / (t1 - t0)
                return q0 + fraction * (q1 - q0)
        return samples[-1][1]


def line_intensity(line: HitranLine, temperature_k: float,
                   partition_sums: PartitionSums) -> float:
    q_ref = partition_sums.evaluate(
        line.molecule, line.isotopologue, REFERENCE_TEMPERATURE_K)
    q_t = partition_sums.evaluate(line.molecule, line.isotopologue, temperature_k)
    boltzmann = math.exp(-C2_CM_K * line.lower_energy_cm1 *
                         (1.0 / temperature_k - 1.0 / REFERENCE_TEMPERATURE_K))
    stimulated = -math.expm1(-C2_CM_K * line.center_cm1 / temperature_k)
    stimulated_ref = -math.expm1(
        -C2_CM_K * line.center_cm1 / REFERENCE_TEMPERATURE_K)
    value = line.intensity_296_cm_per_molecule * (q_ref / q_t) * boltzmann * (
        stimulated / stimulated_ref)
    if not math.isfinite(value) or value < 0.0:
        raise ValueError("temperature-scaled line intensity is invalid")
    return value


def _humlicek_w4_real(x: float, y: float) -> float:
    """Real part of w(x+iy), Humlicek W4 region approximation."""
    t = complex(y, -x)
    s = abs(x) + y
    if s >= 15.0:
        value = t * 0.5641895835477563 / (0.5 + t * t)
    elif s >= 5.5:
        value = t * (1.4104739589 + 0.5641895835477563 * t * t) / (
            0.75 + t * t * (3.0 + t * t))
    elif y >= 0.195 * abs(x) - 0.176:
        value = (16.4955 + t * (20.20933 + t * (11.96482 + t * (
            3.778987 + t * 0.5642236)))) / (16.4955 + t * (38.82363 + t * (
            39.27121 + t * (21.69274 + t * (6.699398 + t)))))
    else:
        u = t * t
        numerator = 36183.31 - u * (3321.9905 - u * (1540.787 - u * (
            219.0313 - u * (35.76683 - u * (1.320522 - u * 0.56419)))))
        denominator = 32066.6 - u * (24322.8 - u * (9022.23 - u * (
            2186.18 - u * (364.2191 - u * (61.57037 - u * (1.841439 - u))))))
        value = math.e ** u - t * numerator / denominator
    result = value.real
    if not math.isfinite(result) or result < 0.0:
        raise ValueError("Voigt evaluation is non-finite or negative")
    return result


def voigt_profile_cm(x_cm1: float, sigma_cm1: float, gamma_cm1: float) -> float:
    if not all(math.isfinite(value) for value in (x_cm1, sigma_cm1, gamma_cm1)):
        raise ValueError("Voigt arguments must be finite")
    if sigma_cm1 < 0.0 or gamma_cm1 < 0.0 or (sigma_cm1 == 0.0 and gamma_cm1 == 0.0):
        raise ValueError("Voigt widths are inadmissible")
    if sigma_cm1 == 0.0:
        return gamma_cm1 / (math.pi * (x_cm1 * x_cm1 + gamma_cm1 * gamma_cm1))
    scaled_x = x_cm1 / (sigma_cm1 * math.sqrt(2.0))
    scaled_y = gamma_cm1 / (sigma_cm1 * math.sqrt(2.0))
    return _humlicek_w4_real(scaled_x, scaled_y) / (
        sigma_cm1 * math.sqrt(2.0 * math.pi))


def planck_weight_wavenumber(wavenumber_cm1: float, temperature_k: float) -> float:
    if wavenumber_cm1 <= 0.0 or temperature_k <= 0.0:
        return 0.0
    exponent = C2_CM_K * wavenumber_cm1 / temperature_k
    if exponent > 700.0:
        return 0.0
    return wavenumber_cm1 ** 3 / math.expm1(exponent)


def trapezoid_mean(xs: list[float], values: list[float], weights: list[float]) -> float:
    if len(xs) != len(values) or len(xs) != len(weights) or len(xs) < 2:
        raise ValueError("Planck-mean arrays are not aligned")
    numerator = 0.0
    denominator = 0.0
    for index in range(len(xs) - 1):
        step = xs[index + 1] - xs[index]
        if step <= 0.0:
            raise ValueError("spectral grid must increase")
        numerator += 0.5 * step * (
            values[index] * weights[index] + values[index + 1] * weights[index + 1])
        denominator += 0.5 * step * (weights[index] + weights[index + 1])
    result = numerator / denominator
    if not math.isfinite(result) or result < 0.0:
        raise ValueError("Planck mean is invalid")
    return result


def planck_mean_wavenumber(xs_cm1: list[float], kappa_m1: list[float],
                           temperature_k: float) -> float:
    if len(xs_cm1) != len(kappa_m1) or len(xs_cm1) < 2 or temperature_k <= 0.0:
        raise ValueError("Planck-mean inputs are invalid")
    weights = [planck_weight_wavenumber(value, temperature_k) for value in xs_cm1]
    numerator = 0.0
    for index in range(len(xs_cm1) - 1):
        step = xs_cm1[index + 1] - xs_cm1[index]
        if step <= 0.0:
            raise ValueError("spectral grid must increase")
        numerator += 0.5 * step * (
            kappa_m1[index] * weights[index] + kappa_m1[index + 1] * weights[index + 1])
    full_blackbody_weight = (math.pi ** 4 / 15.0) * (temperature_k / C2_CM_K) ** 4
    result = numerator / full_blackbody_weight
    if not math.isfinite(result) or result < 0.0:
        raise ValueError("Planck mean is invalid")
    return result


def spectral_grid(minimum: float, maximum: float, step: float) -> list[float]:
    if minimum <= 0.0 or maximum <= minimum or step <= 0.0:
        raise ValueError("spectral grid is invalid")
    count = int(round((maximum - minimum) / step))
    if count < 1 or not math.isclose(minimum + count * step, maximum,
                                     rel_tol=0.0, abs_tol=1.0e-11 * maximum):
        raise ValueError("spectral grid endpoint is not an integer number of steps")
    return [minimum + index * step for index in range(count + 1)]


def species_spectrum(lines: Iterable[HitranLine], molecule: int,
                     molar_masses_kg_per_mol: dict[int, float],
                     partition_sums: PartitionSums, grid_cm1: list[float],
                     temperature_k: float, pressure_pa: float,
                     wing_cutoff_cm1: float) -> tuple[list[float], int]:
    if pressure_pa <= 0.0 or wing_cutoff_cm1 <= 0.0:
        raise ValueError("pressure and line-wing cutoff must be positive")
    number_density_cm3 = pressure_pa / (K_BOLTZMANN * temperature_k) / 1.0e6
    pressure_atm = pressure_pa / REFERENCE_PRESSURE_PA
    result = [0.0] * len(grid_cm1)
    used = 0
    for line in lines:
        if line.molecule != molecule or line.intensity_296_cm_per_molecule == 0.0:
            continue
        molar_mass = molar_masses_kg_per_mol.get(line.isotopologue)
        if not molar_mass or molar_mass <= 0.0:
            raise ValueError("line isotopologue has no positive molar mass")
        center = line.center_cm1 + line.pressure_shift_cm1_atm * pressure_atm
        if center + wing_cutoff_cm1 < grid_cm1[0] or center - wing_cutoff_cm1 > grid_cm1[-1]:
            continue
        strength = line_intensity(line, temperature_k, partition_sums)
        gamma = line.gamma_air_cm1_atm * pressure_atm * (
            REFERENCE_TEMPERATURE_K / temperature_k) ** line.n_air
        molecular_mass_kg = molar_mass / N_AVOGADRO
        sigma = center * math.sqrt(K_BOLTZMANN * temperature_k /
                                   (molecular_mass_kg * C_LIGHT * C_LIGHT))
        if gamma == 0.0 and sigma == 0.0:
            raise ValueError("line has zero Lorentz and Doppler width")
        first = max(0, int(math.ceil(
            (center - wing_cutoff_cm1 - grid_cm1[0]) /
            (grid_cm1[1] - grid_cm1[0]))))
        last = min(len(grid_cm1) - 1, int(math.floor(
            (center + wing_cutoff_cm1 - grid_cm1[0]) /
            (grid_cm1[1] - grid_cm1[0]))))
        for index in range(first, last + 1):
            cross_section_cm2 = strength * voigt_profile_cm(
                grid_cm1[index] - center, sigma, gamma)
            result[index] += cross_section_cm2 * number_density_cm3 * 100.0
        used += 1
    if any(not math.isfinite(value) or value < 0.0 for value in result):
        raise ValueError("generated absorption spectrum is invalid")
    return result, used


def canonical_json_bytes(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")
