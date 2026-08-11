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
from typing import Iterable, Iterator, Sequence


C2_CM_K = 1.4387768775039338
K_BOLTZMANN = 1.380649e-23
C_LIGHT = 299792458.0
N_AVOGADRO = 6.02214076e23
REFERENCE_TEMPERATURE_K = 296.0
REFERENCE_PRESSURE_PA = 101325.0

# Positive nodes and weights for 32-point Gauss-Legendre quadrature.
# Symmetry is applied explicitly below.
LEGENDRE32_POSITIVE = (
    (0.048307665687738316, 0.0965400885147278),
    (0.14447196158279649, 0.09563872007927486),
    (0.23928736225213707, 0.09384439908080457),
    (0.33186860228212765, 0.09117387869576388),
    (0.42135127613063535, 0.08765209300440381),
    (0.5068999089322294, 0.08331192422694676),
    (0.5877157572407623, 0.07819389578707031),
    (0.6630442669302152, 0.07234579410884851),
    (0.7321821187402897, 0.06582222277636185),
    (0.7944837959679424, 0.05868409347853555),
    (0.8482065834104272, 0.050998059262376176),
    (0.8963211557660521, 0.04283589802222668),
    (0.9349060759377397, 0.03427386291302143),
    (0.9647622555875064, 0.02539206530926206),
    (0.9856115115452684, 0.01627439473090567),
    (0.9972638618494816, 0.007018610009470097),
)


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
        isotope_code = line[2:3]
        isotopologue = ({"0": 10, "A": 11, "B": 12}.get(
            isotope_code, int(isotope_code) if isotope_code.isdigit() else -1))
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

    def minimum(self, molecule: int, isotopologue: int,
                minimum_temperature_k: float, maximum_temperature_k: float) -> float:
        """Exact minimum of the piecewise-linear Q table on a closed interval."""
        if (not math.isfinite(minimum_temperature_k) or
                not math.isfinite(maximum_temperature_k) or
                minimum_temperature_k > maximum_temperature_k):
            raise ValueError("partition-sum interval is invalid")
        samples = self._rows.get((molecule, isotopologue))
        if (not samples or minimum_temperature_k < samples[0][0] or
                maximum_temperature_k > samples[-1][0]):
            raise ValueError("partition-sum interval is out of domain")
        candidates = [self.evaluate(molecule, isotopologue, minimum_temperature_k),
                      self.evaluate(molecule, isotopologue, maximum_temperature_k)]
        candidates.extend(q for temperature, q in samples
                          if minimum_temperature_k < temperature < maximum_temperature_k)
        return min(candidates)


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


def line_area_temperature_interval_upper(
        line: HitranLine, minimum_temperature_k: float,
        maximum_temperature_k: float, pressure_pa: float,
        partition_sums: PartitionSums) -> float:
    """Conservative line-area upper bound for every T in a closed interval."""
    if (minimum_temperature_k <= 0.0 or
            minimum_temperature_k > maximum_temperature_k or pressure_pa <= 0.0):
        raise ValueError("line-area interval state is inadmissible")
    q_reference = partition_sums.evaluate(
        line.molecule, line.isotopologue, REFERENCE_TEMPERATURE_K)
    q_minimum = partition_sums.minimum(
        line.molecule, line.isotopologue,
        minimum_temperature_k, maximum_temperature_k)
    boltzmann_upper = math.exp(
        -C2_CM_K * line.lower_energy_cm1 *
        (1.0 / maximum_temperature_k - 1.0 / REFERENCE_TEMPERATURE_K))
    stimulated_upper = -math.expm1(
        -C2_CM_K * line.center_cm1 / minimum_temperature_k)
    stimulated_reference = -math.expm1(
        -C2_CM_K * line.center_cm1 / REFERENCE_TEMPERATURE_K)
    strength_upper = (line.intensity_296_cm_per_molecule *
                      q_reference / q_minimum * boltzmann_upper *
                      stimulated_upper / stimulated_reference)
    number_density_upper = pressure_pa / (
        K_BOLTZMANN * minimum_temperature_k) / 1.0e6
    result = strength_upper * number_density_upper * 100.0
    if not math.isfinite(result) or result < 0.0:
        raise ValueError("line-area interval upper bound is invalid")
    return result


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


def voigt_profile_interval_upper(distance_cm1: float, sigma_cm1: float,
                                 gamma_cm1: float) -> float:
    """Conservative Voigt maximum at points at least ``distance`` from center."""
    if (not all(math.isfinite(value) for value in
                (distance_cm1, sigma_cm1, gamma_cm1)) or
            distance_cm1 < 0.0 or sigma_cm1 < 0.0 or gamma_cm1 < 0.0 or
            (sigma_cm1 == 0.0 and gamma_cm1 == 0.0)):
        raise ValueError("Voigt upper-bound arguments are inadmissible")
    if sigma_cm1 == 0.0:
        return gamma_cm1 / (math.pi *
                            (distance_cm1 * distance_cm1 + gamma_cm1 * gamma_cm1))
    gaussian_peak = 1.0 / (sigma_cm1 * math.sqrt(2.0 * math.pi))
    if gamma_cm1 == 0.0:
        return gaussian_peak * math.exp(
            -0.5 * (distance_cm1 / sigma_cm1) ** 2)
    lorentz_peak = 1.0 / (math.pi * gamma_cm1)
    if distance_cm1 == 0.0:
        return min(gaussian_peak, lorentz_peak)
    gaussian_radius = min(0.5 * distance_cm1, 8.0 * sigma_cm1)
    gaussian_tail = math.erfc(gaussian_radius /
                              (math.sqrt(2.0) * sigma_cm1))
    near_distance = max(0.0, distance_cm1 - gaussian_radius)
    near_lorentz = gamma_cm1 / (math.pi *
                                (near_distance * near_distance + gamma_cm1 * gamma_cm1))
    return min(gaussian_peak, lorentz_peak,
               gaussian_tail * lorentz_peak + near_lorentz)


def voigt_profile_state_cell_upper(
        distance_cm1: float, shifted_center_cm1: float,
        molecular_mass_kg: float, line: HitranLine,
        minimum_temperature_k: float, maximum_temperature_k: float,
        minimum_self_fraction: float, maximum_self_fraction: float,
        pressure_pa: float) -> float:
    """Upper-bound a visible-band Voigt profile throughout one T/x cell.

    The bound uses the Gaussian-convolution representation.  A Cauchy shift
    is split at half the distance to the band; its far probability is bounded
    with the largest Lorentz width in the cell, and its near contribution by
    the largest Gaussian density allowed by the Doppler-width interval.
    """
    if (not all(math.isfinite(value) for value in (
            distance_cm1, shifted_center_cm1, molecular_mass_kg,
            minimum_temperature_k, maximum_temperature_k,
            minimum_self_fraction, maximum_self_fraction, pressure_pa)) or
            distance_cm1 < 0.0 or shifted_center_cm1 <= 0.0 or
            molecular_mass_kg <= 0.0 or minimum_temperature_k <= 0.0 or
            minimum_temperature_k > maximum_temperature_k or
            minimum_self_fraction < 0.0 or
            minimum_self_fraction > maximum_self_fraction or
            maximum_self_fraction > 1.0 or pressure_pa <= 0.0):
        raise ValueError("Voigt state-cell upper-bound arguments are inadmissible")
    sigma_minimum = shifted_center_cm1 * math.sqrt(
        K_BOLTZMANN * minimum_temperature_k /
        (molecular_mass_kg * C_LIGHT * C_LIGHT))
    sigma_maximum = shifted_center_cm1 * math.sqrt(
        K_BOLTZMANN * maximum_temperature_k /
        (molecular_mass_kg * C_LIGHT * C_LIGHT))
    reference_widths = [
        (1.0 - fraction) * line.gamma_air_cm1_atm +
        fraction * line.gamma_self_cm1_atm
        for fraction in (minimum_self_fraction, maximum_self_fraction)]
    temperature_factors = [
        (REFERENCE_TEMPERATURE_K / temperature) ** line.n_air
        for temperature in (minimum_temperature_k, maximum_temperature_k)]
    reference_width_maximum = max(reference_widths)
    temperature_factor_maximum = max(temperature_factors)
    gamma_maximum = (reference_width_maximum *
                     pressure_pa / REFERENCE_PRESSURE_PA *
                     temperature_factor_maximum)
    gamma_minimum = (min(reference_widths) *
                     pressure_pa / REFERENCE_PRESSURE_PA *
                     min(temperature_factors))
    gaussian_peak = 1.0 / (sigma_minimum * math.sqrt(2.0 * math.pi))
    if distance_cm1 == 0.0:
        return gaussian_peak
    radius = 0.5 * distance_cm1
    cauchy_tail = (0.0 if gamma_maximum == 0.0 else
                   1.0 - 2.0 / math.pi * math.atan(radius / gamma_maximum))
    near_distance = distance_cm1 - radius
    candidate_sigma = min(sigma_maximum, max(sigma_minimum, near_distance))
    near_gaussian = (math.exp(-0.5 * (near_distance / candidate_sigma) ** 2) /
                     (candidate_sigma * math.sqrt(2.0 * math.pi)))
    candidates = [gaussian_peak,
                  cauchy_tail * gaussian_peak + near_gaussian]
    if gamma_minimum > 0.0:
        candidates.append(1.0 / (math.pi * gamma_minimum))
        gaussian_radius = min(radius, 8.0 * sigma_maximum)
        gaussian_tail = math.erfc(
            gaussian_radius / (math.sqrt(2.0) * sigma_maximum))
        near_distance = distance_cm1 - gaussian_radius
        candidate_gamma = min(gamma_maximum,
                              max(gamma_minimum, near_distance))
        near_lorentz = candidate_gamma / (
            math.pi * (near_distance * near_distance +
                       candidate_gamma * candidate_gamma))
        candidates.append(
            gaussian_tail / (math.pi * gamma_minimum) + near_lorentz)
    result = min(candidates)
    if not math.isfinite(result) or result < 0.0:
        raise ValueError("Voigt state-cell upper bound is invalid")
    return result


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
    step = xs_cm1[1] - xs_cm1[0]
    if step <= 0.0 or any(not math.isclose(xs_cm1[index + 1] - xs_cm1[index], step,
                                           rel_tol=0.0, abs_tol=1.0e-12 * step)
                          for index in range(len(xs_cm1) - 1)):
        raise ValueError("spectral bin centers must be uniformly spaced")
    numerator = step * sum(value * weight for value, weight in zip(kappa_m1, weights))
    full_blackbody_weight = (math.pi ** 4 / 15.0) * (temperature_k / C2_CM_K) ** 4
    result = numerator / full_blackbody_weight
    if not math.isfinite(result) or result < 0.0:
        raise ValueError("Planck mean is invalid")
    return result


def homogeneous_emissivity(grid_cm1: Sequence[float], kappa_m1: Sequence[float],
                           temperature_k: float, path_length_m: float) -> float:
    if (len(grid_cm1) != len(kappa_m1) or len(grid_cm1) < 2 or
            temperature_k <= 0.0 or path_length_m <= 0.0):
        raise ValueError("homogeneous-emissivity inputs are invalid")
    step = grid_cm1[1] - grid_cm1[0]
    spacing_tolerance = max(
        1.0e-12 * step,
        16.0 * math.ulp(max(abs(grid_cm1[0]), abs(grid_cm1[-1]))))
    if (step <= 0.0 or any(not math.isclose(
            grid_cm1[index + 1] - grid_cm1[index], step,
            rel_tol=0.0, abs_tol=spacing_tolerance)
            for index in range(len(grid_cm1) - 1))):
        raise ValueError("opacity table does not use uniform spectral bins")
    numerator = step * sum(
        -math.expm1(-value * path_length_m) *
        planck_weight_wavenumber(wavenumber, temperature_k)
        for wavenumber, value in zip(grid_cm1, kappa_m1))
    full_blackbody_weight = ((math.pi ** 4 / 15.0) *
                             (temperature_k / C2_CM_K) ** 4)
    result = numerator / full_blackbody_weight
    if not math.isfinite(result) or result < 0.0:
        raise ValueError("homogeneous emissivity is invalid")
    return result


def _coarsen_finite_volume_spectrum(
        grid_cm1: Sequence[float], kappa_m1: Sequence[float],
        factor: int) -> tuple[list[float], list[float]]:
    if factor <= 0 or len(grid_cm1) % factor != 0:
        raise ValueError("spectral grid cannot be coarsened by the requested factor")
    coarse_grid = []
    coarse_kappa = []
    for first in range(0, len(grid_cm1), factor):
        coarse_grid.append(sum(grid_cm1[first:first + factor]) / factor)
        coarse_kappa.append(sum(kappa_m1[first:first + factor]) / factor)
    return coarse_grid, coarse_kappa


def finite_path_emissivity_refinement_certificate(
        grid_cm1: Sequence[float], spectra: Sequence[Sequence[Sequence[float]]],
        gas_temperatures_k: Sequence[float], path_lengths_m: Sequence[float],
        maximum_estimated_remaining_relative: float,
        maximum_contraction_ratio: float) -> dict:
    """Qualify nonlinear emissivity using h/2h/4h finite-volume grids."""
    if (len(grid_cm1) < 8 or len(grid_cm1) % 4 != 0 or
            not path_lengths_m or
            any(not math.isfinite(value) or value <= 0.0
                for value in path_lengths_m) or
            not math.isfinite(maximum_estimated_remaining_relative) or
            maximum_estimated_remaining_relative <= 0.0 or
            not math.isfinite(maximum_contraction_ratio) or
            maximum_contraction_ratio <= 0.0 or maximum_contraction_ratio >= 1.0):
        raise ValueError("finite-path emissivity qualification configuration is invalid")
    worst_adjacent_relative = 0.0
    worst_contraction = 0.0
    worst_remaining_relative = 0.0
    converged = True
    sample_count = 0
    for self_rows in spectra:
        if len(self_rows) != len(gas_temperatures_k):
            raise ValueError("emissivity qualification spectrum shape is invalid")
        for temperature_k, spectrum in zip(gas_temperatures_k, self_rows):
            if len(spectrum) != len(grid_cm1):
                raise ValueError("emissivity qualification spectrum shape is invalid")
            medium_grid, medium_spectrum = _coarsen_finite_volume_spectrum(
                grid_cm1, spectrum, 2)
            coarse_grid, coarse_spectrum = _coarsen_finite_volume_spectrum(
                grid_cm1, spectrum, 4)
            for path_length_m in path_lengths_m:
                fine = homogeneous_emissivity(
                    grid_cm1, spectrum, temperature_k, path_length_m)
                medium = homogeneous_emissivity(
                    medium_grid, medium_spectrum, temperature_k, path_length_m)
                coarse = homogeneous_emissivity(
                    coarse_grid, coarse_spectrum, temperature_k, path_length_m)
                fine_difference = abs(fine - medium)
                coarse_difference = abs(medium - coarse)
                scale = max(abs(fine), 1.0e-12)
                adjacent_relative = fine_difference / scale
                if coarse_difference == 0.0:
                    contraction = 0.0 if fine_difference == 0.0 else math.inf
                else:
                    contraction = fine_difference / coarse_difference
                remaining_relative = (0.0 if fine_difference == 0.0 else
                    (fine_difference * contraction / (1.0 - contraction) / scale
                     if contraction < 1.0 else math.inf))
                worst_adjacent_relative = max(
                    worst_adjacent_relative, adjacent_relative)
                worst_contraction = max(worst_contraction, contraction)
                worst_remaining_relative = max(
                    worst_remaining_relative, remaining_relative)
                converged = converged and (
                    contraction <= maximum_contraction_ratio and
                    remaining_relative <= maximum_estimated_remaining_relative)
                sample_count += 1
    return {
        "kind": "finite_volume_h_2h_4h_nonlinear_emissivity_v1",
        "path_lengths_m": list(path_lengths_m),
        "sample_count": sample_count,
        "maximum_adjacent_h_2h_relative_difference": worst_adjacent_relative,
        "maximum_contraction_ratio": (
            worst_contraction if math.isfinite(worst_contraction) else None),
        "maximum_allowed_contraction_ratio": maximum_contraction_ratio,
        "maximum_estimated_remaining_relative_error": (
            worst_remaining_relative
            if math.isfinite(worst_remaining_relative) else None),
        "maximum_allowed_estimated_remaining_relative_error":
            maximum_estimated_remaining_relative,
        "qualified": bool(converged),
    }


def spectral_grid(minimum: float, maximum: float, step: float) -> list[float]:
    if minimum <= 0.0 or maximum <= minimum or step <= 0.0:
        raise ValueError("spectral grid is invalid")
    count = int(round((maximum - minimum) / step))
    if count < 1 or not math.isclose(minimum + count * step, maximum,
                                     rel_tol=0.0, abs_tol=1.0e-11 * maximum):
        raise ValueError("spectral grid endpoint is not an integer number of steps")
    return [minimum + index * step for index in range(count + 1)]


def pressure_broadened_half_width(line: HitranLine, temperature_k: float,
                                  pressure_pa: float,
                                  self_mole_fraction: float) -> float:
    if (temperature_k <= 0.0 or pressure_pa <= 0.0 or
            self_mole_fraction < 0.0 or self_mole_fraction > 1.0):
        raise ValueError("pressure-broadening state is inadmissible")
    pressure_atm = pressure_pa / REFERENCE_PRESSURE_PA
    reference_width = ((1.0 - self_mole_fraction) * line.gamma_air_cm1_atm +
                       self_mole_fraction * line.gamma_self_cm1_atm)
    result = reference_width * pressure_atm * (
        REFERENCE_TEMPERATURE_K / temperature_k) ** line.n_air
    if not math.isfinite(result) or result < 0.0:
        raise ValueError("pressure-broadened line width is invalid")
    return result


def voigt_interval_probability(lower_cm1: float, upper_cm1: float,
                               sigma_cm1: float, gamma_cm1: float) -> float:
    """Integrate a normalized Voigt profile over one interval."""
    if (not all(math.isfinite(value) for value in
                (lower_cm1, upper_cm1, sigma_cm1, gamma_cm1)) or
            lower_cm1 >= upper_cm1 or sigma_cm1 < 0.0 or gamma_cm1 < 0.0 or
            (sigma_cm1 == 0.0 and gamma_cm1 == 0.0)):
        raise ValueError("Voigt interval is inadmissible")
    if gamma_cm1 == 0.0:
        scale = sigma_cm1 * math.sqrt(2.0)
        return 0.5 * (math.erf(upper_cm1 / scale) -
                      math.erf(lower_cm1 / scale))

    def lorentz_interval(shift: float) -> float:
        return (math.atan((upper_cm1 - shift) / gamma_cm1) -
                math.atan((lower_cm1 - shift) / gamma_cm1)) / math.pi

    if sigma_cm1 == 0.0:
        return lorentz_interval(0.0)
    total = 0.0
    boundary_near_gaussian_core = min(abs(lower_cm1), abs(upper_cm1)) <= 6.0 * sigma_cm1
    if gamma_cm1 <= sigma_cm1 and boundary_near_gaussian_core:
        # Average the analytic Gaussian interval over a Cauchy random shift,
        # using y=gamma*tan(theta); the Cauchy measure becomes dtheta/pi.
        def gaussian_interval(shift: float) -> float:
            scale = sigma_cm1 * math.sqrt(2.0)
            return 0.5 * (math.erf((upper_cm1 - shift) / scale) -
                          math.erf((lower_cm1 - shift) / scale))

        def integrand(theta: float) -> float:
            return gaussian_interval(gamma_cm1 * math.tan(theta)) / math.pi

        def integrate_segment(lower: float, upper: float) -> float:
            midpoint = 0.5 * (lower + upper)
            half_width = 0.5 * (upper - lower)
            return half_width * sum(
                weight * (integrand(midpoint + half_width * node) +
                          integrand(midpoint - half_width * node))
                for node, weight in LEGENDRE32_POSITIVE)

        breakpoints = [-0.5 * math.pi,
                       math.atan(lower_cm1 / gamma_cm1),
                       math.atan(upper_cm1 / gamma_cm1),
                       0.5 * math.pi]
        result = sum(integrate_segment(breakpoints[index], breakpoints[index + 1])
                     for index in range(3))
    else:
        # Average the analytic Lorentz interval over a Gaussian random shift.
        # Eight sigma truncation contributes less than 1.3e-15 probability.
        scale = 8.0 * sigma_cm1
        normalization = scale / (sigma_cm1 * math.sqrt(2.0 * math.pi))
        for node, weight in LEGENDRE32_POSITIVE:
            shift = scale * node
            total += weight * normalization * (
                math.exp(-0.5 * (shift / sigma_cm1) ** 2) *
                (lorentz_interval(shift) + lorentz_interval(-shift)))
        result = total
    if not math.isfinite(result) or result < 0.0 or result > 1.0 + 1.0e-12:
        raise ValueError("Voigt bin integral is invalid")
    return min(1.0, result)


def conservative_voigt_bin_weights(center_cm1: float, sigma_cm1: float,
                                    gamma_cm1: float, grid_cm1: list[float],
                                    cutoff_cm1: float) -> tuple[list[tuple[int, float]], float]:
    """Voigt finite-volume bin integrals and a conservative tail bound."""
    step = grid_cm1[1] - grid_cm1[0]
    first = max(0, int(math.ceil(
        (center_cm1 - cutoff_cm1 - 0.5 * step - grid_cm1[0]) / step)))
    last = min(len(grid_cm1) - 1, int(math.floor(
        (center_cm1 + cutoff_cm1 + 0.5 * step - grid_cm1[0]) / step)))
    if first > last:
        return [], 1.0
    weights = []
    for index in range(first, last + 1):
        lower = max(grid_cm1[index] - 0.5 * step,
                    center_cm1 - cutoff_cm1) - center_cm1
        upper = min(grid_cm1[index] + 0.5 * step,
                    center_cm1 + cutoff_cm1) - center_cm1
        if lower < upper:
            weight = voigt_interval_probability(lower, upper, sigma_cm1, gamma_cm1)
            if weight > 0.0:
                weights.append((index, weight))
    available_half_span = min(
        cutoff_cm1,
        center_cm1 - (grid_cm1[0] - 0.5 * step),
        (grid_cm1[-1] + 0.5 * step) - center_cm1,
    )
    if available_half_span <= 0.0:
        return [], 1.0
    # If |G+L| exceeds the available span, then either |G| or |L| exceeds
    # half that span.  The sum below is therefore a true union bound for the
    # convolution tail (using the full span for both terms is not).
    union_half_span = 0.5 * available_half_span
    gaussian_tail = (math.erfc(union_half_span /
                               (math.sqrt(2.0) * sigma_cm1))
                     if sigma_cm1 > 0.0 else 0.0)
    lorentz_tail = (1.0 - 2.0 / math.pi *
                    math.atan(union_half_span / gamma_cm1)
                    if gamma_cm1 > 0.0 else 0.0)
    return weights, min(1.0, gaussian_tail + lorentz_tail)


def _validate_axis(values: Sequence[float], label: str) -> None:
    if (len(values) < 2 or any(not math.isfinite(value) for value in values) or
            any(values[index] >= values[index + 1]
                for index in range(len(values) - 1))):
        raise ValueError(f"{label} axis must be finite and strictly increasing")


def _validate_samples(values: Sequence[float], label: str) -> None:
    if (not values or any(not math.isfinite(value) for value in values) or
            any(values[index] >= values[index + 1]
                for index in range(len(values) - 1))):
        raise ValueError(f"{label} samples must be finite and strictly increasing")


def species_spectra_batch(
        lines: Iterable[HitranLine], molecule: int,
        molar_masses_kg_per_mol: dict[int, float],
        partition_sums: PartitionSums, grid_cm1: list[float],
        temperatures_k: Sequence[float], pressure_pa: float,
        self_mole_fractions: Sequence[float],
        wing_cutoffs_cm1: Sequence[float],
        radiation_temperatures_k: Sequence[float] = (),
        visible_wavenumber_interval_cm1: Sequence[float] = (),
) -> tuple[list[list[list[list[float]]]], list[list[list[int]]],
           list[list[list[float]]], list[list[float]], list[list[float]],
           list[list[float]]]:
    """Accumulate every requested state in one archive pass.

    Returned axes are cutoff, self mole fraction, gas temperature, and
    spectral bin.  Counts and maximum omitted-tail probability use the first
    three axes.  Values are finite-volume bin averages, so integrating a line
    over the output bins preserves its temperature-scaled area independently
    of its phase within a bin.
    """
    _validate_samples(temperatures_k, "gas temperature")
    _validate_samples(self_mole_fractions, "self mole fraction")
    _validate_samples(wing_cutoffs_cm1, "wing cutoff")
    if radiation_temperatures_k:
        _validate_samples(radiation_temperatures_k, "radiation temperature")
    if (visible_wavenumber_interval_cm1 and
            (len(visible_wavenumber_interval_cm1) != 2 or
             not all(math.isfinite(value) for value in visible_wavenumber_interval_cm1) or
             visible_wavenumber_interval_cm1[0] <= 0.0 or
             visible_wavenumber_interval_cm1[0] >= visible_wavenumber_interval_cm1[1])):
        raise ValueError("visible wavenumber interval is invalid")
    if (pressure_pa <= 0.0 or self_mole_fractions[0] < 0.0 or
            self_mole_fractions[-1] > 1.0):
        raise ValueError("batch opacity state is inadmissible")
    shape = (len(wing_cutoffs_cm1), len(self_mole_fractions),
             len(temperatures_k))
    spectra = [[[[0.0] * len(grid_cm1) for _ in range(shape[2])]
                for _ in range(shape[1])] for _ in range(shape[0])]
    counts = [[[0 for _ in range(shape[2])] for _ in range(shape[1])]
              for _ in range(shape[0])]
    tail_bounds = [[[0.0 for _ in range(shape[2])] for _ in range(shape[1])]
                   for _ in range(shape[0])]
    line_center_means = [[0.0 for _ in radiation_temperatures_k]
                         for _ in temperatures_k]
    visible_upper_bounds = [[0.0 for _ in temperatures_k]
                            for _ in self_mole_fractions]
    visible_cell_upper_bounds = [[0.0 for _ in range(max(1, len(temperatures_k) - 1))]
                                 for _ in range(max(1, len(self_mole_fractions) - 1))]
    step = grid_cm1[1] - grid_cm1[0]
    pressure_atm = pressure_pa / REFERENCE_PRESSURE_PA
    for line in lines:
        if line.molecule != molecule:
            raise ValueError("line archive contains a foreign molecule")
        if line.intensity_296_cm_per_molecule == 0.0:
            continue
        molar_mass = molar_masses_kg_per_mol.get(line.isotopologue)
        if not molar_mass or molar_mass <= 0.0:
            raise ValueError("line isotopologue has no positive molar mass")
        center = line.center_cm1 + line.pressure_shift_cm1_atm * pressure_atm
        if center <= 0.0:
            raise ValueError("pressure-shifted line center is non-positive")
        contributes_to_spectral_table = not (
            center + wing_cutoffs_cm1[-1] < grid_cm1[0] or
            center - wing_cutoffs_cm1[-1] > grid_cm1[-1])
        molecular_mass_kg = molar_mass / N_AVOGADRO
        if visible_wavenumber_interval_cm1:
            visible_lo, visible_hi = visible_wavenumber_interval_cm1
            distance = (visible_lo - center if center < visible_lo else
                        center - visible_hi if center > visible_hi else 0.0)
            for self_cell in range(max(1, len(self_mole_fractions) - 1)):
                self_minimum = self_mole_fractions[self_cell]
                self_maximum = (self_mole_fractions[self_cell + 1]
                                if len(self_mole_fractions) > 1 else self_minimum)
                for temperature_cell in range(max(1, len(temperatures_k) - 1)):
                    temperature_minimum = temperatures_k[temperature_cell]
                    temperature_maximum = (temperatures_k[temperature_cell + 1]
                                           if len(temperatures_k) > 1
                                           else temperature_minimum)
                    visible_cell_upper_bounds[self_cell][temperature_cell] += (
                        line_area_temperature_interval_upper(
                            line, temperature_minimum, temperature_maximum,
                            pressure_pa, partition_sums) *
                        voigt_profile_state_cell_upper(
                            distance, center, molecular_mass_kg, line,
                            temperature_minimum, temperature_maximum,
                            self_minimum, self_maximum, pressure_pa))
        for temperature_index, temperature_k in enumerate(temperatures_k):
            strength = line_intensity(line, temperature_k, partition_sums)
            number_density_cm3 = pressure_pa / (K_BOLTZMANN * temperature_k) / 1.0e6
            line_area_m1_cm1 = strength * number_density_cm3 * 100.0
            if contributes_to_spectral_table:
                for radiation_index, radiation_temperature in enumerate(
                        radiation_temperatures_k):
                    line_center_means[temperature_index][radiation_index] += (
                        line_area_m1_cm1 *
                        planck_weight_wavenumber(center, radiation_temperature))
            sigma = center * math.sqrt(K_BOLTZMANN * temperature_k /
                                       (molecular_mass_kg * C_LIGHT * C_LIGHT))
            for self_index, self_fraction in enumerate(self_mole_fractions):
                gamma = pressure_broadened_half_width(
                    line, temperature_k, pressure_pa, self_fraction)
                if gamma == 0.0 and sigma == 0.0:
                    raise ValueError("line has zero Lorentz and Doppler width")
                if visible_wavenumber_interval_cm1:
                    visible_lo, visible_hi = visible_wavenumber_interval_cm1
                    distance = (visible_lo - center if center < visible_lo else
                                center - visible_hi if center > visible_hi else 0.0)
                    visible_upper_bounds[self_index][temperature_index] += (
                        line_area_m1_cm1 *
                        voigt_profile_interval_upper(distance, sigma, gamma))
                if not contributes_to_spectral_table:
                    continue
                for cutoff_index, cutoff in enumerate(wing_cutoffs_cm1):
                    weights, tail_bound = conservative_voigt_bin_weights(
                        center, sigma, gamma, grid_cm1, cutoff)
                    if not weights:
                        continue
                    for bin_index, weight in weights:
                        spectra[cutoff_index][self_index][temperature_index][bin_index] += (
                            line_area_m1_cm1 * weight / step)
                    counts[cutoff_index][self_index][temperature_index] += 1
                    tail_bounds[cutoff_index][self_index][temperature_index] = max(
                        tail_bounds[cutoff_index][self_index][temperature_index], tail_bound)
    if any(not math.isfinite(value) or value < 0.0
           for cutoff_rows in spectra for self_rows in cutoff_rows
           for temperature_row in self_rows for value in temperature_row):
        raise ValueError("generated absorption spectrum is invalid")
    for temperature_index in range(len(temperatures_k)):
        for radiation_index, radiation_temperature in enumerate(
                radiation_temperatures_k):
            denominator = ((math.pi ** 4 / 15.0) *
                           (radiation_temperature / C2_CM_K) ** 4)
            line_center_means[temperature_index][radiation_index] /= denominator
    return (spectra, counts, tail_bounds, line_center_means,
            visible_upper_bounds, visible_cell_upper_bounds)


def species_spectrum(lines: Iterable[HitranLine], molecule: int,
                     molar_masses_kg_per_mol: dict[int, float],
                     partition_sums: PartitionSums, grid_cm1: list[float],
                     temperature_k: float, pressure_pa: float,
                     wing_cutoff_cm1: float,
                     self_mole_fraction: float = 0.0) -> tuple[list[float], int, float]:
    if pressure_pa <= 0.0 or wing_cutoff_cm1 <= 0.0:
        raise ValueError("pressure and line-wing cutoff must be positive")
    spectra, counts, tails, _, _, _ = species_spectra_batch(
        lines, molecule, molar_masses_kg_per_mol, partition_sums, grid_cm1,
        [temperature_k], pressure_pa, [self_mole_fraction], [wing_cutoff_cm1])
    return spectra[0][0][0], counts[0][0][0], tails[0][0][0]


def linear_axis_weights(axis: Sequence[float], value: float) -> tuple[int, float]:
    _validate_axis(axis, "interpolation")
    if not math.isfinite(value) or value < axis[0] or value > axis[-1]:
        raise ValueError("opacity lookup is out of domain")
    if value == axis[-1]:
        return len(axis) - 2, 1.0
    index = next(i for i in range(len(axis) - 1) if value <= axis[i + 1])
    return index, (value - axis[index]) / (axis[index + 1] - axis[index])


def multilinear_value(axes: Sequence[Sequence[float]], tensor: object,
                      coordinates: Sequence[float]) -> float:
    if len(axes) != len(coordinates) or not axes:
        raise ValueError("multilinear interpolation rank mismatch")
    cells = [linear_axis_weights(axis, coordinate)
             for axis, coordinate in zip(axes, coordinates)]
    result = 0.0
    for corner in range(1 << len(axes)):
        value = tensor
        weight = 1.0
        for dimension, (index, fraction) in enumerate(cells):
            upper = (corner >> dimension) & 1
            value = value[index + upper]
            weight *= fraction if upper else 1.0 - fraction
        result += weight * float(value)
    if not math.isfinite(result) or result < 0.0:
        raise ValueError("interpolated opacity is invalid")
    return result


def tensor_linear_derivative_certificate(axes: Sequence[Sequence[float]],
                                         tensor: object) -> dict:
    """Exact per-cell partial-derivative enclosures for multilinear data."""
    if len(axes) != 3:
        raise ValueError("Planck-mean certificate requires a rank-three tensor")
    for axis in axes:
        _validate_axis(axis, "certificate")
    cells = []
    for i in range(len(axes[0]) - 1):
        for j in range(len(axes[1]) - 1):
            for k in range(len(axes[2]) - 1):
                lower = [i, j, k]
                bounds = []
                for dimension in range(3):
                    derivatives = []
                    other_dimensions = [value for value in range(3)
                                        if value != dimension]
                    for bits in range(4):
                        lo = list(lower)
                        hi = list(lower)
                        hi[dimension] += 1
                        for bit_index, other in enumerate(other_dimensions):
                            offset = (bits >> bit_index) & 1
                            lo[other] += offset
                            hi[other] += offset
                        lo_value = tensor[lo[0]][lo[1]][lo[2]]
                        hi_value = tensor[hi[0]][hi[1]][hi[2]]
                        derivatives.append((hi_value - lo_value) /
                                           (axes[dimension][lower[dimension] + 1] -
                                            axes[dimension][lower[dimension]]))
                    bounds.append([min(derivatives), max(derivatives)])
                cells.append({
                    "lower_indices": lower,
                    "partial_derivative_bounds": bounds,
                    "local_equal_temperature_derivative_bound": [
                        bounds[1][0] + bounds[2][0],
                        bounds[1][1] + bounds[2][1],
                    ],
                })
    return {
        "kind": "tensor_multilinear_exact_cell_bounds_v1",
        "axes": ["self_mole_fraction", "gas_temperature_K",
                 "radiation_temperature_K"],
        "cells": cells,
    }


def canonical_json_bytes(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")
