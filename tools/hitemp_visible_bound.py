#!/usr/bin/env python3
"""Certify visible absorption from an exact-line-selected HITEMP histogram.

The reducer selects line centres before binning.  This tool then computes:

* the historical 100 K-lattice observed visible fraction and absorption; and
* a continuous-domain upper bound on absolute visible absorption.

The continuous bound does not evaluate cell moments.  It uses only ΣA and the
known (nu,E'') extent of every occupied bin, so moment approximation and a
line straddling a wavelength boundary cannot make it optimistic.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path


C2 = 1.4387769
TREF = 296.0
N_ATM = 7.3389965e21  # molecules/cm^3 times 1/T[K]
NU_BIN_WIDTH = 25.0
E_BIN_WIDTH = 50.0
PLANCK_NORM = math.pi**4 / 15.0


def load_tips(tips_dir: Path) -> dict[int, tuple[list[float], list[float]]]:
    result = {}
    for path in sorted(tips_dir.glob("q_iso*.txt")):
        isotopologue = int(path.stem.removeprefix("q_iso"))
        rows = [tuple(map(float, line.split())) for line in path.read_text().splitlines()]
        temperatures = [row[0] for row in rows]
        partitions = [row[1] for row in rows]
        if (len(rows) < 2500 or any(right <= left for left, right in
                                    zip(temperatures, temperatures[1:])) or
                any(right < left for left, right in zip(partitions, partitions[1:])) or
                temperatures[0] > TREF):
            raise ValueError(f"TIPS table is not a monotone covering table: {path}")
        result[isotopologue] = (temperatures, partitions)
    if not result:
        raise ValueError("no TIPS tables found")
    return result


def qval(tips: dict[int, tuple[list[float], list[float]]], iso: int,
         temperature: float) -> float:
    temperatures, partitions = tips[iso]
    if not temperatures[0] <= temperature <= temperatures[-1]:
        raise ValueError("temperature is outside TIPS coverage")
    lo, hi = 0, len(temperatures) - 1
    while hi - lo > 1:
        middle = (lo + hi) // 2
        if temperatures[middle] <= temperature:
            lo = middle
        else:
            hi = middle
    fraction = ((temperature - temperatures[lo]) /
                (temperatures[hi] - temperatures[lo]))
    return partitions[lo] + fraction * (partitions[hi] - partitions[lo])


def load_cells(path: Path) -> tuple[list[tuple[int, int, int, float, float, float, float]], dict[str, str]]:
    cells = []
    metadata = {}
    for line in path.read_text(encoding="ascii").splitlines():
        if line.startswith("#"):
            parts = line[1:].split()
            if len(parts) >= 2:
                metadata[parts[0]] = " ".join(parts[1:])
            continue
        parts = line.split()
        if len(parts) != 8:
            raise ValueError("visible histogram row is malformed")
        cells.append((int(parts[0]), int(parts[1]), int(parts[2]),
                      float(parts[3]), float(parts[4]), float(parts[5]),
                      float(parts[6])))
    if metadata.get("histogram_kind") != "exact_line_center_band":
        raise ValueError("visible certificate requires an exact-line-selected histogram")
    if int(metadata.get("cells", "-1")) != len(cells) or not cells:
        raise ValueError("visible histogram cell count is invalid")
    return cells, metadata


def load_total_table(path: Path) -> dict[tuple[float, float], float]:
    result = {}
    for line in path.read_text(encoding="ascii").splitlines():
        if line.startswith("#") or not line:
            continue
        gas, radiation, cross_section, _ = map(float, line.split())
        result[(gas, radiation)] = cross_section
    return result


def load_tail_cells(path: Path) -> tuple[list[tuple[int, int, float]], dict[str, str]]:
    cells = []
    metadata = {}
    for line in path.read_text(encoding="ascii").splitlines():
        if line.startswith("#"):
            parts = line[1:].split()
            if len(parts) >= 2:
                metadata[parts[0]] = " ".join(parts[1:])
            continue
        parts = line.split()
        if len(parts) != 4:
            raise ValueError("visible Voigt-tail row is malformed")
        cells.append((int(parts[0]), int(parts[1]), float(parts[2])))
    if (metadata.get("histogram_kind") != "all_line_voigt_band_probability_upper" or
            metadata.get("pressure_Pa") != "101325" or
            int(metadata.get("cells", "-1")) != len(cells) or not cells):
        raise ValueError("visible certificate requires the all-line one-atmosphere tail basis")
    return cells, metadata


def planck_weight(wavenumber: float, temperature: float) -> float:
    x = C2 * wavenumber / temperature
    return (wavenumber**3 / math.expm1(x) /
            ((temperature / C2)**4 * PLANCK_NORM))


def observed_strength(cell: tuple[int, int, int, float, float, float, float],
                      gas_temperature: float,
                      tips: dict[int, tuple[list[float], list[float]]]) -> float:
    iso, _, _, sum_a, mean_e, mean_e2, mean_nu = cell
    variance = max(0.0, mean_e2 - mean_e * mean_e)
    inverse_temperature = C2 / gas_temperature
    correction = 1.0 + 0.5 * inverse_temperature**2 * variance
    return (sum_a * qval(tips, iso, TREF) / qval(tips, iso, gas_temperature) *
            math.exp(-inverse_temperature * mean_e) *
            (1.0 - math.exp(-inverse_temperature * mean_nu)) * correction)


def continuous_absorption_upper(
        cells: list[tuple[int, int, float]],
        tips: dict[int, tuple[list[float], list[float]]], tlo: float, thi: float,
        band_lo: float, band_hi: float, gas_interval: float) -> float:
    # For the normalized Planck weight in this band, x/(1-exp(-x)) > 4,
    # hence it increases with T_r.  At fixed T_r, x/(1-exp(-x)) > 3,
    # hence it decreases with nu.  This proves the per-bin maximum used below.
    smallest_x = C2 * band_lo / thi
    ratio = smallest_x / (1.0 - math.exp(-smallest_x))
    if ratio <= 4.0:
        raise ValueError("visible-domain Planck monotonicity precondition failed")

    gas_intervals = []
    lower = tlo
    while lower < thi:
        upper = min(thi, lower + gas_interval)
        gas_intervals.append((lower, upper))
        lower = upper

    total_upper = 0.0
    planck_upper = planck_weight(band_lo, thi)
    for iso, e_bin, sum_a_stim_tail in cells:
        if (iso not in tips or not math.isfinite(sum_a_stim_tail) or
                sum_a_stim_tail < 0.0):
            raise ValueError("visible histogram has invalid cell data")
        e_lower = e_bin * E_BIN_WIDTH
        q_reference = qval(tips, iso, TREF)
        gas_upper = 0.0
        for interval_lower, interval_upper in gas_intervals:
            # Each factor is bounded in its known monotone direction.  Their
            # extrema need not occur at one temperature; multiplying them is
            # deliberately conservative and covers every interior state.
            candidate = (
                N_ATM / interval_lower * 100.0 *
                q_reference / qval(tips, iso, interval_lower) *
                math.exp(-C2 * e_lower / interval_upper))
            gas_upper = max(gas_upper, candidate)
        total_upper += sum_a_stim_tail * gas_upper * planck_upper
    return math.nextafter(total_upper, math.inf)


def certificate(args: argparse.Namespace) -> dict:
    cells, metadata = load_cells(args.visible_histogram)
    tail_cells, tail_metadata = load_tail_cells(args.visible_tail_histogram)
    tips = load_tips(args.tips)
    total = load_total_table(args.total_table)
    band_lo = float(metadata["visible_band_lo_cm-1"])
    band_hi = float(metadata["visible_band_hi_cm-1"])
    if not math.isclose(band_lo, args.band_lo, rel_tol=0.0, abs_tol=5.0e-8) or not math.isclose(
            band_hi, args.band_hi, rel_tol=0.0, abs_tol=5.0e-8):
        raise ValueError("visible histogram band does not match the declared band")
    if (tail_metadata.get("visible_band_lo_cm-1") != metadata["visible_band_lo_cm-1"] or
            tail_metadata.get("visible_band_hi_cm-1") != metadata["visible_band_hi_cm-1"]):
        raise ValueError("visible centre and Voigt-tail bases have different bands")

    maximum_fraction = 0.0
    maximum_absorption = 0.0
    maximum_fraction_at = None
    maximum_absorption_at = None
    temperatures = [float(value) for value in range(int(args.tlo), int(args.thi) + 1, 100)]
    if temperatures[-1] != args.thi:
        temperatures.append(args.thi)
    for gas_temperature in temperatures:
        strengths = [observed_strength(cell, gas_temperature, tips) for cell in cells]
        number_density = N_ATM / gas_temperature
        for radiation_temperature in temperatures:
            visible_cross_section = sum(
                strength * planck_weight(cell[6], radiation_temperature)
                for cell, strength in zip(cells, strengths))
            absorption = visible_cross_section * number_density * 100.0
            total_cross_section = total.get((gas_temperature, radiation_temperature))
            if total_cross_section is None or total_cross_section <= 0.0:
                raise ValueError("total table lacks a positive 100 K validation knot")
            fraction = visible_cross_section / total_cross_section
            if fraction > maximum_fraction:
                maximum_fraction = fraction
                maximum_fraction_at = [gas_temperature, radiation_temperature]
            if absorption > maximum_absorption:
                maximum_absorption = absorption
                maximum_absorption_at = [gas_temperature, radiation_temperature]

    upper = continuous_absorption_upper(
        tail_cells, tips, args.tlo, args.thi, band_lo, band_hi, args.gas_bound_interval)
    if upper < maximum_absorption:
        raise ValueError("continuous visible bound is below the observed maximum")
    return {
        "schema": "rise-hitemp-visible-certificate-v1",
        "species": args.species,
        "temperature_domain_K": [args.tlo, args.thi],
        "bounded_renderer_wavenumber_domain_cm-1": [band_lo, band_hi],
        "exact_selected_line_count": int(metadata["visible_band_line_count"]),
        "histogram_cell_count": len(cells),
        "all_line_voigt_tail_cell_count": len(tail_cells),
        "observed_100K_lattice": {
            "maximum_fraction_of_total_planck_mean": maximum_fraction,
            "maximum_fraction_at_K": maximum_fraction_at,
            "maximum_absorption_per_m_atm": maximum_absorption,
            "maximum_absorption_at_K": maximum_absorption_at,
        },
        "continuous_upper_absorption_per_m_atm": upper,
        "continuous_bound_method": (
            "all HITEMP lines; one-atmosphere Voigt band probability bounded by a "
            "Cauchy interval plus Gaussian tail union bound using worst air/self width, "
            "temperature exponent, pressure shift and lightest-isotopologue Doppler width; "
            "then monotone E/T/Q bounds over 50 K gas intervals and the exact visible-band "
            "normalized-Planck-weight maximum over the radiation-temperature interval"),
        "gas_bound_interval_K": args.gas_bound_interval,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("visible_histogram", type=Path)
    parser.add_argument("visible_tail_histogram", type=Path)
    parser.add_argument("tips", type=Path)
    parser.add_argument("--total-table", required=True, type=Path)
    parser.add_argument("--species", required=True, choices=("H2O", "CO2"))
    parser.add_argument("--band-lo", required=True, type=float)
    parser.add_argument("--band-hi", required=True, type=float)
    parser.add_argument("--tlo", type=float, default=300.0)
    parser.add_argument("--thi", type=float, default=2500.0)
    parser.add_argument("--gas-bound-interval", type=float, default=50.0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if (args.tlo != int(args.tlo) or args.thi != int(args.thi) or
            args.tlo < TREF or args.thi <= args.tlo or args.gas_bound_interval <= 0.0):
        raise ValueError("invalid visible-certificate temperature domain")
    result = json.dumps(certificate(args), indent=1, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(result, encoding="utf-8")
    else:
        print(result, end="")


if __name__ == "__main__":
    main()
