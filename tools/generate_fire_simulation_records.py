#!/usr/bin/env python3
"""Freeze Phase-C open physical-property records into RISE-CBOR64-v1.

The operational records are deliberately generated from redistribution-safe
NASA CEA, NIST ThermoML, and GRI-Mech/Cantera inputs.  Burcat and HITEMP line
content is excluded so owner-gated coefficients cannot enter these records.
The legacy aggregate thermochemistry output remains a property subset for
wax/wood bring-up.  The same pinned NASA CEA snapshot also produces the
complete physical methane record required by the r51 solver bring-up rule.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import itertools
import json
import math
import re
import struct
import sys
from fractions import Fraction
from pathlib import Path

from generate_fire_optics_records import (
    encode, emit_array, tabulated_spectrum, validate_unicode17_authority,
)


R_KMOL = 8314.46261815324
CEA_REVISION = "0c99ecefce3e9a885ec912040477caf74e69c8f3"
EXPECTED_SOURCE_SHA256 = {
    "nasa_cea_thermo": "fa7746572952d74e249e818a82a35c113829742fb421a308e167185528884363",
    "nasa_cea_transport": "379c514a7f7638371d8d9254a1d653cd078084a9371061aa0b45c5a5c3334d41",
    "nist_thermoml": "64186a74e71e3b973ab7ea385ba42ef9d2583ce73323c4f9f42435c7503f632b",
    "gri_transport": "e2ef4437568311ad0ba6c2311a564966c9accb2b43ea3157b764c1e8febc5825",
    "cantera_gri30_yaml": "06650b1e0ee0012f6903d5328b1bb218cb6007d07f8ebe375d18f24811039345",
    "idaes_wms_source": "c2bb4cd7d387a17585250eea69465d04caa0aee4d003e91b18fd62856fe028e5",
    "idaes_wms_documentation": "f400247ec4517ea1b2576a4b1aedaddcc4c7203a94af2457197e98e8ee00bf8f",
    "idaes_license": "4fdde00bd663a045a688899c7e6924bd3b747204518e75540273c97f4bc98fa0",
    "vreman_2004_pdf": "06cc0118d6441d34e4a6e97ee3437cd1de422c4a789efb18aac3bbb556955b82",
    "fds_cons": "85f1ccb4553714c063f0b8235011fe728e57e7aec86d474ef845e4190c56fa83",
    "fds_data": "2808d4cfc1a9ca2f17446c8563c13ead9824921342de549115f92a9306b070a4",
    "fds_license": "38c542304b97afc4171a9b67866499eaf222509cab45c095ea69bf88d57755b7",
}
EXPECTED_SOURCE_SNAPSHOT_SHA256 = "063c75b9e23079ad08d601666a587733c4546692eeabb4ba8bffdafa8cbf35bd"
THERMO_NAMES = (
    "Ar", "CH4", "CH3OH", "CO", "CO2", "C7H16,n-heptane",
    "H2O", "N2", "O2", "C(gr)",
)
ALKANE_INCREMENT_NAMES = (
    "C4H10,n-butane", "C5H12,n-pentane", "C6H14,n-hexane",
    "C7H16,n-heptane", "C8H18,n-octane",
)
THERMO_SOURCE_NAMES = tuple(dict.fromkeys(THERMO_NAMES + ALKANE_INCREMENT_NAMES))
PENTACOSANE_NAME = "C25H52,n-pentacosane"
TRANSPORT_NAMES = ("Ar", "CH4", "CH3OH", "CO", "CO2", "H2O", "N2", "O2")
FORMULAS = {
    "Ar": {"Ar": 1.0},
    "CH4": {"C": 1.0, "H": 4.0},
    "CH3OH": {"C": 1.0, "H": 4.0, "O": 1.0},
    "CO": {"C": 1.0, "O": 1.0},
    "CO2": {"C": 1.0, "O": 2.0},
    "C7H16,n-heptane": {"C": 7.0, "H": 16.0},
    "H2O": {"H": 2.0, "O": 1.0},
    "N2": {"N": 2.0},
    "O2": {"O": 2.0},
    "C(gr)": {"C": 1.0},
    "C4H10,n-butane": {"C": 4.0, "H": 10.0},
    "C5H12,n-pentane": {"C": 5.0, "H": 12.0},
    "C6H14,n-hexane": {"C": 6.0, "H": 14.0},
    "C8H18,n-octane": {"C": 8.0, "H": 18.0},
    PENTACOSANE_NAME: {"C": 25.0, "H": 52.0},
}

METHANE_SPECIES = ("CH4", "O2", "N2", "CO2", "H2O", "CO", "C(gr)")
METHANE_ELEMENTS = ("C", "H", "O", "N")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require_source_digest(path: Path, source_id: str) -> str:
    actual = sha256(path)
    expected = EXPECTED_SOURCE_SHA256[source_id]
    if actual != expected:
        raise ValueError(f"{source_id} SHA-256 mismatch: expected {expected}, got {actual}")
    return actual


def fortran_numbers(line: str) -> list[float]:
    line = re.sub(r"([DEde]) ([0-9]{2})", r"\1+\2", line)
    pattern = r"[+-]?(?:\d+\.\d*|\.\d+)(?:[DEde][+-]?\d+)?"
    return [float(value.replace("D", "E").replace("d", "e"))
            for value in re.findall(pattern, line)]


def find_exact_header(lines: list[str], name: str) -> int:
    for index, line in enumerate(lines):
        if line[:18].strip() == name:
            return index
    raise ValueError(f"NASA CEA record {name!r} is missing")


def parse_thermo(lines: list[str], name: str) -> dict:
    start = find_exact_header(lines, name)
    count = int(lines[start + 1].split()[0])
    metadata_numbers = fortran_numbers(lines[start + 1])
    if len(metadata_numbers) < 3:
        raise ValueError(f"NASA CEA metadata is malformed for {name}")
    molecular_weight = metadata_numbers[-2]
    formation_enthalpy = metadata_numbers[-1] * 1000.0
    segments = []
    cursor = start + 2
    for _ in range(count):
        interval = [float(lines[cursor][0:11]), float(lines[cursor][11:22])]
        coefficients = fortran_numbers(lines[cursor + 1]) + fortran_numbers(lines[cursor + 2])
        if len(interval) < 2 or len(coefficients) != 9:
            raise ValueError(f"NASA-9 interval is malformed for {name}")
        segments.append({
            "temperature_min_K": interval[0],
            "temperature_max_K": interval[1],
            "coefficients": coefficients,
        })
        cursor += 3
    if count == 0:
        raise ValueError(f"reactant-only NASA entry {name} has no heat-capacity fit")
    return {
        "name": name,
        "formula": FORMULAS[name],
        "molecular_weight_kg_per_kmol": molecular_weight,
        "formation_enthalpy_J_per_kmol_298p15K": formation_enthalpy,
        "source_header": lines[start].rstrip("\n"),
        "segments": segments,
    }


def parse_transport(lines: list[str], name: str) -> dict:
    start = find_exact_header(lines, name)
    header = lines[start].rstrip("\n")
    segments = {"viscosity": [], "conductivity": []}
    cursor = start + 1
    while cursor < len(lines) and lines[cursor].startswith(" "):
        line = lines[cursor]
        kind = line.strip()[:1]
        values = fortran_numbers(line)
        if kind in ("V", "C") and len(values) == 6:
            target = "viscosity" if kind == "V" else "conductivity"
            segments[target].append({
                "temperature_min_K": values[0],
                "temperature_max_K": values[1],
                "coefficients": values[2:],
            })
        cursor += 1
    if not segments["viscosity"] or not segments["conductivity"]:
        raise ValueError(f"NASA transport entry {name} lacks V/C fits")
    return {"name": name, "source_header": header, **segments}


def extract_levoglucosan_cp(source: dict) -> dict:
    matches = []
    for dataset in source.get("PureOrMixtureData", []):
        properties = dataset.get("Property", [])
        if len(properties) != 1:
            continue
        group = properties[0].get("Property-MethodID", {}).get("PropertyGroup", {})
        entry = group.get("HeatCapacityAndDerivedProp", {})
        if entry.get("ePropName") != "Molar heat capacity at constant pressure, J/K/mol":
            continue
        phase = properties[0].get("PropPhaseID", {}).get("ePropPhase")
        if phase != "Crystal 2":
            continue
        for row in dataset.get("NumValues", []):
            variables = row.get("VariableValue", [])
            values = row.get("PropertyValue", [])
            if len(variables) == 1 and len(values) == 1:
                uncertainty = values[0].get("CombinedUncertainty", {}).get(
                    "nCombExpandUncertValue")
                matches.append([
                    float(variables[0]["nVarValue"]),
                    float(values[0]["nPropValue"]),
                    float(uncertainty),
                ])
    if len(matches) != 45:
        raise ValueError(f"expected 45 Kabo levoglucosan Cp rows, found {len(matches)}")
    return {
        "formula": "C6H10O5",
        "phase": "crystal_2",
        "columns": ["temperature_K", "cp_J_per_mol_K", "expanded_95_J_per_mol_K"],
        "rows": matches,
    }


def cantera_transport_tables(gri_yaml: Path) -> dict:
    try:
        import cantera as ct
    except ImportError as exc:
        raise ValueError("source extraction requires Cantera on PYTHONPATH") from exc
    if ct.__version__ != "3.1.0":
        raise ValueError(f"source extraction requires Cantera 3.1.0, got {ct.__version__}")
    gas = ct.Solution(str(gri_yaml))
    aliases = {"Ar": "AR"}
    if gas.min_temp != 300.0 or gas.max_temp != 3000.0:
        raise ValueError(f"pinned GRI phase domain changed to [{gas.min_temp},{gas.max_temp}] K")
    temperatures = [300.0 + 25.0 * index for index in range(109)]
    result = {}
    for name in TRANSPORT_NAMES:
        cantera_name = aliases.get(name, name)
        viscosity = []
        conductivity = []
        for temperature in temperatures:
            gas.TPX = temperature, 101325.0, {cantera_name: 1.0}
            viscosity.append(float(gas.viscosity))
            conductivity.append(float(gas.thermal_conductivity))
        result[name] = {
            "columns": ["temperature_K", "viscosity_Pa_s", "conductivity_W_m_K"],
            "rows": [[temperatures[i], viscosity[i], conductivity[i]]
                     for i in range(len(temperatures))],
            "viscosity_interpolation": tabulated_spectrum(temperatures, viscosity),
            "conductivity_interpolation": tabulated_spectrum(temperatures, conductivity),
        }
    return {"cantera_version": ct.__version__, "tables": result}


def extract_sources(thermo: Path, transport: Path, thermoml: Path,
                    gri_transport: Path, gri_yaml: Path, idaes_wms: Path,
                    idaes_wms_doc: Path, idaes_license: Path, vreman_pdf: Path,
                    fds_cons: Path, fds_data: Path, fds_license: Path,
                    output: Path) -> None:
    forbidden = "burcat" in str(thermo).lower() or "burcat" in str(transport).lower()
    if forbidden:
        raise ValueError("Burcat-derived bytes are license-gated and forbidden")
    thermo_hash = require_source_digest(thermo, "nasa_cea_thermo")
    transport_hash = require_source_digest(transport, "nasa_cea_transport")
    thermoml_hash = require_source_digest(thermoml, "nist_thermoml")
    gri_transport_hash = require_source_digest(gri_transport, "gri_transport")
    gri_yaml_hash = require_source_digest(gri_yaml, "cantera_gri30_yaml")
    idaes_wms_hash = require_source_digest(idaes_wms, "idaes_wms_source")
    idaes_doc_hash = require_source_digest(idaes_wms_doc, "idaes_wms_documentation")
    idaes_license_hash = require_source_digest(idaes_license, "idaes_license")
    vreman_hash = require_source_digest(vreman_pdf, "vreman_2004_pdf")
    fds_cons_hash = require_source_digest(fds_cons, "fds_cons")
    fds_data_hash = require_source_digest(fds_data, "fds_data")
    fds_license_hash = require_source_digest(fds_license, "fds_license")
    thermo_lines = thermo.read_text(encoding="ascii").splitlines(keepends=True)
    transport_lines = transport.read_text(encoding="ascii").splitlines(keepends=True)
    thermoml_value = json.loads(thermoml.read_text(encoding="utf-8"))
    snapshot = {
        "schema_version": 1,
        "source_kind": "fire_sim_open_source_snapshot",
        "nasa_cea": {
            "repository": "https://github.com/nasa/cea",
            "revision": CEA_REVISION,
            "license": "Apache-2.0",
            "thermo_inp_sha256": thermo_hash,
            "trans_inp_sha256": transport_hash,
            "thermochemistry": [parse_thermo(thermo_lines, name)
                                for name in THERMO_SOURCE_NAMES],
            "transport": [parse_transport(transport_lines, name) for name in TRANSPORT_NAMES],
        },
        "nist_thermoml": {
            "locator": "https://trc.nist.gov/ThermoML/10.1016/j.jct.2015.01.005.json",
            "sha256": thermoml_hash,
            "doi": "10.1016/j.jct.2015.01.005",
            "license": "NIST ThermoML public data; no SPDX identifier stated",
            "levoglucosan_cp": extract_levoglucosan_cp(thermoml_value),
        },
        "gri_mech_3": {
            "transport_locator": "http://combustion.berkeley.edu/gri-mech/version30/files30/transport.dat",
            "transport_sha256": gri_transport_hash,
            "cantera_gri30_yaml_sha256": gri_yaml_hash,
            "cantera_version": "3.1.0",
            "cantera_source_revision": "v3.1.0",
            "license": ("Cantera gri30.yaml is BSD-3-Clause; original GRI-Mech "
                        "transport.dat redistribution terms are not separately stated"),
            **cantera_transport_tables(gri_yaml),
        },
        "transport_closure_references": {
            "idaes_wms": {
                "repository": "https://github.com/IDAES/idaes-pse",
                "revision": "a60fbf8192a697c60564b68379847ce6e9dbb3b9",
                "source_sha256": idaes_wms_hash,
                "documentation_sha256": idaes_doc_hash,
                "license": "BSD-3-Clause",
                "license_sha256": idaes_license_hash,
            },
            "vreman_2004": {
                "locator": "https://www.vremanresearch.nl/Vreman-PF2004-subgridmodel.pdf",
                "sha256": vreman_hash,
                "derived_Cv": 0.07,
            },
            "fds_source": {
                "locator": "https://github.com/firemodels/fds",
                "revision": "a30dd017ac0a74cd082929978aeb97148a754e72",
                "cons_f90_sha256": fds_cons_hash,
                "data_f90_sha256": fds_data_hash,
                "license": "NIST public-domain notice",
                "license_sha256": fds_license_hash,
                "C_vreman": 0.07,
                "tau_chem_s": 1.0e-5,
                "default_critical_flame_temperature_C": 1427.0,
            },
        },
    }
    output.write_text(json.dumps(snapshot, indent=1, sort_keys=True) + "\n", encoding="utf-8")


def provenance(citation: str, locator: str = "docs/FIRE_SMOKE_DESIGN.md") -> dict:
    return {
        "citation": citation,
        "locator": locator,
        "access": "open",
        "secondary_source": False,
    }


def exact(value: float, citation: str, applicability: str) -> dict:
    return exact_value(float(value), citation, applicability)


def exact_value(value, citation: str, applicability: str) -> dict:
    return {
        "value": value,
        "uncertainty": {"kind": "design_pinned_exact", "magnitude": 0},
        "provenance": provenance(citation),
        "applicability": applicability,
    }


def source_envelope(value: float, citation: str, applicability: str) -> dict:
    return {
        "value": float(value),
        "uncertainty": {
            "kind": "computed_range_from_input_sensitivity",
            "magnitude": [0.0, 0.0],
            "basis": "exact evaluation of the pinned source coefficients",
            "scope": ("binary64 evaluation of pinned coefficients only; physical "
                      "source-fit uncertainty is not quantified"),
        },
        "provenance": provenance(citation, "https://github.com/nasa/cea"),
        "applicability": applicability,
    }


def assumption_envelope(value: float, magnitude: float, basis: str,
                        citation: str, applicability: str) -> dict:
    return {
        "value": float(value),
        "uncertainty": {
            "kind": "assumption_bound",
            "magnitude": float(magnitude),
            "basis": basis,
        },
        "provenance": provenance(citation, "https://github.com/nasa/cea"),
        "applicability": applicability,
    }


def linear_increment_fit(values: list[float]) -> dict:
    if len(values) != 5 or not all(math.isfinite(value) for value in values):
        raise ValueError("CH2 increment fit requires five finite C4-C8 values")
    carbon_numbers = (4.0, 5.0, 6.0, 7.0, 8.0)
    mean_x = 6.0
    mean_y = sum(values) / len(values)
    denominator = sum((value - mean_x) ** 2 for value in carbon_numbers)
    slope = sum((x - mean_x) * (y - mean_y)
                for x, y in zip(carbon_numbers, values)) / denominator
    increments = [values[index + 1] - values[index]
                  for index in range(len(values) - 1)]
    residuals = [increment - slope for increment in increments]
    fitted = [mean_y + slope * (x - mean_x) for x in carbon_numbers]
    sum_squared_residual = sum((value - prediction) ** 2
                               for value, prediction in zip(values, fitted))
    sum_squared_total = sum((value - mean_y) ** 2 for value in values)
    r_squared = (1.0 - sum_squared_residual / sum_squared_total
                 if sum_squared_total > 0.0 else 1.0)
    if not all(math.isfinite(value) for value in
               [slope, *residuals, r_squared]):
        raise ValueError("CH2 increment fit produced a non-finite diagnostic")
    return {
        "slope_per_CH2": slope,
        "adjacent_increment_residuals": residuals,
        "max_abs_adjacent_increment_residual": max(abs(value)
                                                   for value in residuals),
        "r_squared": r_squared,
    }


def cp_increment_residual_bound(coefficients: list[float], lo: float,
                                hi: float) -> float:
    maximum = 0.0
    cursor = lo
    while cursor < hi:
        upper = min(cursor + 1.0, hi)
        midpoint = 0.5 * (cursor + upper)
        enclosure = abs(cp_over_r(coefficients, midpoint)) + \
            cp_derivative_bound(coefficients, cursor, upper) * (upper - cursor) * 0.5
        maximum = max(maximum, enclosure)
        cursor = upper
    return math.nextafter(maximum, math.inf)


def derived_pentacosane_source(sources: dict[str, dict]) -> dict:
    series = [sources[name] for name in ALKANE_INCREMENT_NAMES]
    expected_domains = ((300.0, 1000.0), (1000.0, 6000.0))
    for source in series:
        domains = tuple((float(segment["temperature_min_K"]),
                         float(segment["temperature_max_K"]))
                        for segment in source["segments"])
        if domains != expected_domains:
            raise ValueError(f"alkane increment source {source['name']} has changed domains")

    octane = series[-1]
    derived_segments = []
    fit_diagnostics = []
    maximum_cp_bound = 0.0
    maximum_hs_bound = 0.0
    molecular_weight_fit = linear_increment_fit([
        float(source["molecular_weight_kg_per_kmol"]) for source in series
    ])
    molecular_weight = float(octane["molecular_weight_kg_per_kmol"]) + \
        17.0 * molecular_weight_fit["slope_per_CH2"]
    formation_fit = linear_increment_fit([
        float(source["formation_enthalpy_J_per_kmol_298p15K"]) for source in series
    ])
    formation_enthalpy = float(octane["formation_enthalpy_J_per_kmol_298p15K"]) + \
        17.0 * formation_fit["slope_per_CH2"]

    for segment_index, source_domain in enumerate(expected_domains):
        coefficient_fits = []
        coefficients = []
        for coefficient_index in range(9):
            fit = linear_increment_fit([
                float(source["segments"][segment_index]["coefficients"][coefficient_index])
                for source in series
            ])
            coefficient_fits.append(fit)
            coefficients.append(float(
                octane["segments"][segment_index]["coefficients"][coefficient_index]
            ) + 17.0 * fit["slope_per_CH2"])

        increment_slopes = [fit["slope_per_CH2"] for fit in coefficient_fits]
        residual_polynomials = []
        for adjacent_index in range(len(series) - 1):
            residual_polynomials.append([
                float(series[adjacent_index + 1]["segments"][segment_index]["coefficients"][i]) -
                float(series[adjacent_index]["segments"][segment_index]["coefficients"][i]) -
                increment_slopes[i]
                for i in range(9)
            ])
        certified_lo = 200.0 if segment_index == 0 else source_domain[0]
        cp_residual_over_r = max(cp_increment_residual_bound(
            residual[:7], certified_lo, source_domain[1])
            for residual in residual_polynomials)
        cp_bound = 17.0 * cp_residual_over_r * R_KMOL / molecular_weight
        hs_bound = cp_bound * max(abs(certified_lo - 300.0),
                                  abs(source_domain[1] - 300.0))
        maximum_cp_bound = max(maximum_cp_bound, cp_bound)
        maximum_hs_bound = max(maximum_hs_bound, hs_bound)
        derived_segments.append({
            "temperature_min_K": certified_lo,
            "temperature_max_K": source_domain[1],
            "coefficients": coefficients,
        })
        fit_diagnostics.append({
            "temperature_domain_K": [certified_lo, source_domain[1]],
            "source_temperature_domain_K": list(source_domain),
            "coefficient_increment_fits": coefficient_fits,
            "certified_max_abs_cp_increment_residual_over_R": cp_residual_over_r,
            "propagated_17_CH2_cp_bound_J_per_kg_K": cp_bound,
            "propagated_17_CH2_hs_bound_J_per_kg": hs_bound,
        })

    basis = ("least-squares per-CH2 increment across pinned NASA CEA C4H10 through "
             "C8H18 NASA-9 entries; nominal C25H52 is n-octane plus 17 increments; "
             "bound is 17 times the largest certified adjacent-increment residual")
    citation = (f"NASA CEA {CEA_REVISION} data/thermo.inp entries: " +
                "; ".join(source["source_header"] for source in series))
    return {
        "name": PENTACOSANE_NAME,
        "formula": FORMULAS[PENTACOSANE_NAME],
        "molecular_weight_kg_per_kmol": molecular_weight,
        "formation_enthalpy_J_per_kmol_298p15K": formation_enthalpy,
        "source_header": "derived C25H52 by pinned C4-C8 CH2 increment fit",
        "segments": derived_segments,
        "assumption_bound": {
            "derivation_kind": "nasa_cea_c4_c8_least_squares_ch2_increment_v1",
            "added_CH2": 17.0,
            "source_species": list(ALKANE_INCREMENT_NAMES),
            "basis": basis,
            "citation": citation,
            "molecular_weight_magnitude_kg_per_kmol": 17.0 *
                molecular_weight_fit["max_abs_adjacent_increment_residual"],
            "formation_enthalpy_magnitude_J_per_kmol": 17.0 *
                formation_fit["max_abs_adjacent_increment_residual"],
            "maximum_cp_magnitude_J_per_kg_K": maximum_cp_bound,
            "maximum_hs_magnitude_J_per_kg": maximum_hs_bound,
            "molecular_weight_increment_fit": molecular_weight_fit,
            "formation_enthalpy_increment_fit": formation_fit,
            "segment_increment_fits": fit_diagnostics,
            "low_temperature_extension": (
                "the fitted 300-1000 K low NASA-9 segment is evaluated over 200-300 K "
                "under the same 17-CH2 residual bound"),
            "corroboration_only": {
                "repository": "https://github.com/ReactionMechanismGenerator/RMG-database",
                "revision": "fc7bb138f9380f1274cc9645ef6586c83dda450e",
                "license_status": ("no repository license found; no bytes committed and no "
                                   "RMG value used as an operational source"),
                "method": "Benson Cs-CsHHH and Cs-CsCsHH group-additivity comparison",
                "maximum_relative_cp_difference_300_to_1000K": 0.018360502400747634,
                "relative_formation_enthalpy_difference_at_298p15K": 0.001389220390589775,
            },
        },
    }


def cp_over_r(coefficients: list[float], temperature: float) -> float:
    a = coefficients
    return (a[0] / temperature**2 + a[1] / temperature + a[2] +
            a[3] * temperature + a[4] * temperature**2 +
            a[5] * temperature**3 + a[6] * temperature**4)


def cp_derivative_bound(coefficients: list[float], lo: float, hi: float) -> float:
    a = coefficients
    return (2.0 * abs(a[0]) / lo**3 + abs(a[1]) / lo**2 + abs(a[3]) +
            2.0 * abs(a[4]) * hi + 3.0 * abs(a[5]) * hi**2 +
            4.0 * abs(a[6]) * hi**3)


def certified_cp_lower(coefficients: list[float], lo: float, hi: float,
                       molecular_weight: float) -> float:
    minimum = math.inf
    cursor = lo
    while cursor < hi:
        upper = min(cursor + 1.0, hi)
        midpoint = 0.5 * (cursor + upper)
        bound = cp_over_r(coefficients, midpoint) - cp_derivative_bound(
            coefficients, cursor, upper) * (upper - cursor) * 0.5
        minimum = min(minimum, bound * R_KMOL / molecular_weight)
        cursor = upper
    return math.nextafter(minimum, -math.inf)


def cp_antiderivative(coefficients: list[float], temperature: float) -> float:
    a = coefficients
    return (-a[0] / temperature + a[1] * math.log(temperature) +
            a[2] * temperature + a[3] * temperature**2 / 2.0 +
            a[4] * temperature**3 / 3.0 + a[5] * temperature**4 / 4.0 +
            a[6] * temperature**5 / 5.0)


def clipped_thermo_species(source: dict, domain_min: float, domain_max: float,
                           reference_temperature: float) -> dict:
    mw = float(source["molecular_weight_kg_per_kmol"])
    segments = []
    offset = 0.0
    previous_end = reference_temperature
    ordered = source["segments"]
    for raw in ordered:
        lo = max(domain_min, float(raw["temperature_min_K"]))
        hi = min(domain_max, float(raw["temperature_max_K"]))
        if lo >= hi:
            continue
        coefficients = [float(value) for value in raw["coefficients"]]
        if not segments:
            offset = -(R_KMOL / mw) * cp_antiderivative(coefficients, reference_temperature)
        elif abs(lo - previous_end) <= 1.0e-9:
            prior = segments[-1]
            prior_h = ((R_KMOL / mw) * cp_antiderivative(
                prior["coefficients"], lo) + prior["hs_offset_J_per_kg"])
            offset = prior_h - (R_KMOL / mw) * cp_antiderivative(coefficients, lo)
        else:
            raise ValueError(f"thermochemistry intervals do not cover the common domain for {source['name']}")
        segments.append({
            "temperature_min_K": lo,
            "temperature_max_K": hi,
            "coefficients": coefficients,
            "hs_offset_J_per_kg": offset,
            "certified_cp_lower_J_per_kg_K": certified_cp_lower(coefficients, lo, hi, mw),
        })
        previous_end = hi
    if not segments or segments[0]["temperature_min_K"] != domain_min or previous_end != domain_max:
        raise ValueError(f"thermochemistry does not span [{domain_min},{domain_max}] for {source['name']}")
    source_citation = f"NASA CEA {CEA_REVISION} data/thermo.inp: {source['source_header']}"
    assumption = source.get("assumption_bound")
    if assumption:
        source_citation = assumption["citation"]
        molecular_weight = assumption_envelope(
            mw, assumption["molecular_weight_magnitude_kg_per_kmol"],
            assumption["basis"], source_citation,
            "C25H52 nominal formula and pinned C4-C8 increment fit")
        formation_enthalpy = assumption_envelope(
            source["formation_enthalpy_J_per_kmol_298p15K"],
            assumption["formation_enthalpy_magnitude_J_per_kmol"],
            assumption["basis"], source_citation,
            "standard-state extrapolation at 298.15 K")
        model_uncertainty = {
            "kind": "assumption_bound",
            "magnitude": assumption["maximum_cp_magnitude_J_per_kg_K"],
            "basis": assumption["basis"],
            "scope": ("absolute cp bound in J/kg/K; the derivation certificate also "
                      "records the propagated sensible-enthalpy bound"),
        }
        model_applicability = (
            "C25H52 gas; 200-300 K is an explicit same-method low-temperature "
            "extension; no extrapolation outside the closed domain")
    else:
        molecular_weight = source_envelope(mw, source_citation,
                                            "pinned NASA interval set")
        formation_enthalpy = source_envelope(
            source["formation_enthalpy_J_per_kmol_298p15K"], source_citation,
            "standard state at 298.15 K")
        model_uncertainty = {
            "kind": "computed_range_from_input_sensitivity",
            "magnitude": [0.0, 0.0],
            "basis": "pinned NASA coefficients; source fit uncertainty is not published",
        }
        model_applicability = (
            "closed interval only; continuous h_s is integrated from source cp")
    result = {
        "species_id": source["name"],
        "formula": source["formula"],
        "phase": "aerosol_solid" if source["name"] == "C(gr)" else "gas",
        "molecular_weight_kg_per_kmol": molecular_weight,
        "formation_enthalpy_J_per_kmol_298p15K": formation_enthalpy,
        "cp_hs_model": {
            "kind": "nasa9_cp_with_continuous_integrated_hs_v1",
            "temperature_domain_K": [domain_min, domain_max],
            "reference_temperature_K": reference_temperature,
            "segments": segments,
            "table_metadata": {
                "uncertainty": model_uncertainty,
                "provenance": provenance(source_citation, "https://github.com/nasa/cea"),
                "applicability": model_applicability,
                "out_of_domain_policy": "reject",
            },
        },
    }
    if assumption:
        result["assumption_bound_certificate"] = assumption
    return result


def fraction(value) -> Fraction:
    """Convert source decimals to exact rationals without a binary64 detour."""
    if isinstance(value, Fraction):
        return value
    if isinstance(value, int):
        return Fraction(value)
    return Fraction(str(value))


def rational(value: Fraction) -> dict:
    value = fraction(value)
    return {
        "numerator": str(value.numerator),
        "denominator": str(value.denominator),
    }


def rational_matrix(matrix: list[list[Fraction]]) -> dict:
    if not matrix or not matrix[0] or any(len(row) != len(matrix[0]) for row in matrix):
        raise ValueError("rational matrix is empty or ragged")
    return {
        "rows": len(matrix),
        "columns": len(matrix[0]),
        "entries": [[rational(value) for value in row] for row in matrix],
    }


def matrix_transpose(matrix):
    return [list(row) for row in zip(*matrix)]


def matrix_multiply(left, right):
    if not left or not right or len(left[0]) != len(right):
        raise ValueError("invalid exact matrix product")
    return [[sum((left[i][k] * right[k][j] for k in range(len(right))),
                 Fraction(0))
             for j in range(len(right[0]))]
            for i in range(len(left))]


def matrix_inverse(matrix):
    size = len(matrix)
    if size == 0 or any(len(row) != size for row in matrix):
        raise ValueError("exact inverse requires a square matrix")
    work = [[fraction(value) for value in row] +
            [Fraction(int(i == j)) for j in range(size)]
            for i, row in enumerate(matrix)]
    for column in range(size):
        pivot = next((row for row in range(column, size)
                      if work[row][column] != 0), None)
        if pivot is None:
            raise ValueError("exact matrix is singular")
        work[column], work[pivot] = work[pivot], work[column]
        scale = work[column][column]
        work[column] = [value / scale for value in work[column]]
        for row in range(size):
            if row == column or work[row][column] == 0:
                continue
            scale = work[row][column]
            work[row] = [work[row][index] - scale * work[column][index]
                         for index in range(2 * size)]
    return [row[size:] for row in work]


def matrix_determinant(matrix):
    size = len(matrix)
    if size == 0 or any(len(row) != size for row in matrix):
        raise ValueError("exact determinant requires a square matrix")
    total = Fraction(0)
    for permutation in itertools.permutations(range(size)):
        inversions = sum(permutation[i] > permutation[j]
                         for i in range(size) for j in range(i + 1, size))
        product = Fraction(-1 if inversions % 2 else 1)
        for row, column in enumerate(permutation):
            product *= matrix[row][column]
        total += product
    return total


def exact_rref(matrix):
    work = [[fraction(value) for value in row] for row in matrix]
    pivot_columns = []
    pivot_row = 0
    for column in range(len(work[0])):
        selected = next((row for row in range(pivot_row, len(work))
                         if work[row][column] != 0), None)
        if selected is None:
            continue
        work[pivot_row], work[selected] = work[selected], work[pivot_row]
        scale = work[pivot_row][column]
        work[pivot_row] = [value / scale for value in work[pivot_row]]
        for row in range(len(work)):
            if row == pivot_row or work[row][column] == 0:
                continue
            scale = work[row][column]
            work[row] = [work[row][index] - scale * work[pivot_row][index]
                         for index in range(len(work[0]))]
        pivot_columns.append(column)
        pivot_row += 1
        if pivot_row == len(work):
            break
    return work, pivot_columns


def exact_nullspace(matrix):
    reduced, pivot_columns = exact_rref(matrix)
    free_columns = [column for column in range(len(matrix[0]))
                    if column not in pivot_columns]
    columns = []
    for free in free_columns:
        vector = [Fraction(0) for _ in range(len(matrix[0]))]
        vector[free] = Fraction(1)
        for row, pivot in enumerate(pivot_columns):
            vector[pivot] = -reduced[row][free]
        columns.append(vector)
    return matrix_transpose(columns), pivot_columns


def first_nonzero_minor(matrix, rank):
    for rows in itertools.combinations(range(len(matrix)), rank):
        for columns in itertools.combinations(range(len(matrix[0])), rank):
            minor = [[matrix[row][column] for column in columns] for row in rows]
            determinant = matrix_determinant(minor)
            if determinant != 0:
                return list(rows), list(columns), determinant
    raise ValueError("declared exact rank has no nonzero pivot minor")


def rank_factorization(matrix, rank):
    rows, columns, determinant = first_nonzero_minor(matrix, rank)
    pivot = [[matrix[row][column] for column in columns] for row in rows]
    left = [[row[column] for column in columns] for row in matrix]
    selected_rows = [[matrix[row][column] for column in range(len(matrix[0]))]
                     for row in rows]
    right = matrix_multiply(matrix_inverse(pivot), selected_rows)
    if matrix_multiply(left, right) != matrix:
        raise ValueError("exact rank factorization did not reproduce the constraint matrix")
    return left, right, rows, columns, determinant


def orthonormalize_exact_columns(basis):
    columns = matrix_transpose(basis)
    orthonormal = []
    for exact_column in columns:
        vector = [float(value) for value in exact_column]
        # Two modified-Gram-Schmidt passes make the emitted binary64 columns
        # deterministic and comfortably tighter than the §3.7 envelopes.
        for _ in range(2):
            for prior in orthonormal:
                projection = sum(vector[i] * prior[i]
                                 for i in range(len(vector)))
                vector = [vector[i] - projection * prior[i]
                          for i in range(len(vector))]
        norm = math.sqrt(sum(value * value for value in vector))
        if not math.isfinite(norm) or norm == 0.0:
            raise ValueError("exact nullspace produced a singular numerical basis")
        vector = [value / norm for value in vector]
        first = next(value for value in vector if value != 0.0)
        if first < 0.0:
            vector = [-value for value in vector]
        orthonormal.append(vector)
    return matrix_transpose(orthonormal)


def float_matrix(matrix):
    matrix = [[0.0 if value == 0.0 else value for value in row]
              for row in matrix]
    packed = b"".join(struct.pack(">d", value)
                      for row in matrix for value in row)
    return {
        "rows": len(matrix),
        "columns": len(matrix[0]),
        "entries": matrix,
        "binary64_layout": "ieee754_big_endian_row_major",
        "binary64_sha256": hashlib.sha256(packed).hexdigest(),
    }


def nullspace_certificate(name: str, row_order: list[str], state_order: list[str],
                          constraint):
    reduced, pivot_columns = exact_rref(constraint)
    rank = len(pivot_columns)
    exact_basis, _ = exact_nullspace(constraint)
    if len(exact_basis[0]) != len(state_order) - rank:
        raise ValueError("exact nullity disagrees with the rank certificate")
    factor_left, factor_right, pivot_rows, pivot_columns, pivot_det = \
        rank_factorization(constraint, rank)
    null_rows, null_columns, null_det = first_nonzero_minor(
        exact_basis, len(exact_basis[0]))
    if any(value != 0 for row in matrix_multiply(constraint, exact_basis)
           for value in row):
        raise ValueError("exact nullspace residual is nonzero")
    gram = matrix_multiply(matrix_transpose(exact_basis), exact_basis)
    projector = matrix_multiply(
        matrix_multiply(exact_basis, matrix_inverse(gram)),
        matrix_transpose(exact_basis))
    numerical_basis = orthonormalize_exact_columns(exact_basis)
    numeric_projector = [[sum(numerical_basis[i][k] * numerical_basis[j][k]
                              for k in range(len(numerical_basis[0])))
                          for j in range(len(numerical_basis))]
                         for i in range(len(numerical_basis))]
    projector_error = max(sum(abs(numeric_projector[i][j] - float(projector[i][j]))
                              for j in range(len(projector)))
                          for i in range(len(projector)))
    residual = max(sum(abs(sum(float(constraint[i][j]) * numerical_basis[j][k]
                               for j in range(len(state_order))))
                       for k in range(len(numerical_basis[0])))
                   for i in range(len(constraint)))
    orthonormality = max(sum(abs(sum(numerical_basis[k][i] * numerical_basis[k][j]
                                       for k in range(len(state_order))) -
                                   (1.0 if i == j else 0.0))
                               for j in range(len(numerical_basis[0])))
                           for i in range(len(numerical_basis[0])))
    eps = sys.float_info.epsilon
    if projector_error > 1024.0 * eps or residual > 128.0 * eps * max(
            1.0, max(sum(abs(float(value)) for value in row) for row in constraint)) or \
            orthonormality > 128.0 * eps:
        raise ValueError("numerical nullspace does not meet the §3.7 fp64 certificate")
    return {
        "record_kind": name,
        "state_order": state_order,
        "constraint_row_order": row_order,
        "constraint_matrix": rational_matrix(constraint),
        "declared_rank": rank,
        "rank_factorization": {
            "left": rational_matrix(factor_left),
            "right": rational_matrix(factor_right),
            "pivot_rows": pivot_rows,
            "pivot_columns": pivot_columns,
            "pivot_minor_determinant": rational(pivot_det),
        },
        "exact_nullspace": {
            "basis": rational_matrix(exact_basis),
            "pivot_rows": null_rows,
            "pivot_columns": null_columns,
            "pivot_minor_determinant": rational(null_det),
        },
        "exact_projector": rational_matrix(projector),
        "orthonormal_nullspace": float_matrix(numerical_basis),
        "residual_envelopes": {
            "A_N_infinity_factor_epsilon64": 128.0,
            "Nt_N_minus_I_infinity_factor_epsilon64": 128.0,
            "NNt_minus_exact_projector_infinity_factor_epsilon64": 1024.0,
            "generator_measured_A_N_infinity": residual,
            "generator_measured_Nt_N_minus_I_infinity": orthonormality,
            "generator_measured_projector_infinity": projector_error,
        },
    }


def methane_payload(snapshot: dict) -> dict:
    common_min, common_max, reference = 300.0, 5000.0, 300.0
    sources = {entry["name"]: entry
               for entry in snapshot["nasa_cea"]["thermochemistry"]}
    species = [clipped_thermo_species(sources[name], common_min, common_max,
                                      reference)
               for name in METHANE_SPECIES]
    molecular_weights = {name: fraction(sources[name]["molecular_weight_kg_per_kmol"])
                         for name in METHANE_SPECIES}
    atomic_weights = {
        "C": molecular_weights["C(gr)"],
        "O": molecular_weights["O2"] / 2,
        "N": molecular_weights["N2"] / 2,
        "H": (molecular_weights["H2O"] - molecular_weights["O2"] / 2) / 2,
    }
    for name in METHANE_SPECIES:
        formula_weight = sum(atomic_weights[element] * fraction(count)
                             for element, count in FORMULAS[name].items())
        if formula_weight != molecular_weights[name]:
            raise ValueError(f"NASA formula arithmetic does not reproduce W for {name}")
    element_matrix = [[atomic_weights[element] * fraction(FORMULAS[name].get(element, 0)) /
                       molecular_weights[name] for name in METHANE_SPECIES]
                      for element in METHANE_ELEMENTS]

    # The r51 rule makes air and the injection state case declarations rather
    # than measurements.  The bring-up case pins dry 21/79 molar air and pure
    # methane at the record's common reference temperature.
    ambient_moles = {"O2": Fraction(21, 100), "N2": Fraction(79, 100)}
    ambient_mass_total = sum(ambient_moles[name] * molecular_weights[name]
                             for name in ambient_moles)
    ambient_mass = [ambient_moles.get(name, Fraction(0)) * molecular_weights[name] /
                    ambient_mass_total for name in METHANE_SPECIES]
    injected_mass = [Fraction(int(name == "CH4")) for name in METHANE_SPECIES]
    ambient_elements = [sum(element_matrix[row][column] * ambient_mass[column]
                            for column in range(len(METHANE_SPECIES)))
                        for row in range(len(METHANE_ELEMENTS))]
    injected_elements = [sum(element_matrix[row][column] * injected_mass[column]
                             for column in range(len(METHANE_SPECIES)))
                         for row in range(len(METHANE_ELEMENTS))]

    state_order = ["rho_tot_Z"] + [f"q:{name}" for name in METHANE_SPECIES]
    reconstruction = [[-(injected_elements[row] - ambient_elements[row])] +
                      [element_matrix[row][column] - ambient_elements[row]
                       for column in range(len(METHANE_SPECIES))]
                      for row in range(len(METHANE_ELEMENTS))]
    flux_projection = reconstruction + [[Fraction(0)] +
                                        [Fraction(1)] * len(METHANE_SPECIES)]
    reconstruction_certificate = nullspace_certificate(
        "conservative_reconstruction_v1", list(METHANE_ELEMENTS), state_order,
        reconstruction)
    flux_certificate = nullspace_certificate(
        "nonadvective_flux_projection_v1",
        list(METHANE_ELEMENTS) + ["sum_constituent_flux"], state_order,
        flux_projection)

    hf = {name: fraction(sources[name]["formation_enthalpy_J_per_kmol_298p15K"])
          for name in METHANE_SPECIES}
    reaction_enthalpy = hf["CO2"] + 2 * hf["H2O"] - hf["CH4"]
    if reaction_enthalpy >= 0:
        raise ValueError("methane source formation enthalpies do not yield exothermic combustion")
    lhv = -reaction_enthalpy / molecular_weights["CH4"]
    stoichiometric_oxygen = 2 * molecular_weights["O2"] / molecular_weights["CH4"]
    product_co2 = molecular_weights["CO2"] / molecular_weights["CH4"]
    product_h2o = 2 * molecular_weights["H2O"] / molecular_weights["CH4"]
    soot_oxygen = molecular_weights["O2"] / molecular_weights["C(gr)"]
    soot_product_co2 = molecular_weights["CO2"] / molecular_weights["C(gr)"]
    soot_heat = -(hf["CO2"] - hf["C(gr)"] - hf["O2"]) / molecular_weights["C(gr)"]
    if Fraction(1) + stoichiometric_oxygen != product_co2 + product_h2o:
        raise ValueError("methane primary reaction mass balance failed")
    reaction_delta = [Fraction(-1), -stoichiometric_oxygen, Fraction(0),
                      product_co2, product_h2o, Fraction(0), Fraction(0)]
    for row in range(len(METHANE_ELEMENTS)):
        if sum(element_matrix[row][column] * reaction_delta[column]
               for column in range(len(METHANE_SPECIES))) != 0:
            raise ValueError("methane primary reaction element balance failed")

    return {
        "schema_version": 1,
        "version": "1.0.0-preview.1",
        "record_kind": "fire_sim_methane_fuel_closure",
        "record_name": "fire-sim-methane-physical-v1",
        "record_status": "preview_only",
        "record_class": "physical_fuel_preset",
        "provenance_schema": "fire-optics-canonical-provenance-schema-v1",
        "source_snapshot_sha256": hashlib.sha256(encode(snapshot)).hexdigest(),
        "common_temperature_domain_K": [common_min, common_max],
        "reference_temperature_K": exact(reference, "FIRE_SMOKE_DESIGN.md SS3.3", "all species"),
        "thermodynamic_pressure_Pa": exact(101325.0, "FIRE_SMOKE_DESIGN.md SS3.2", "open-domain methane bring-up"),
        "species_order": list(METHANE_SPECIES),
        "element_order": list(METHANE_ELEMENTS),
        "atomic_weights_kg_per_kmol": {key: rational(value)
                                        for key, value in atomic_weights.items()},
        "element_mass_fraction_matrix": rational_matrix(element_matrix),
        "species": species,
        "ambient_state": {
            "temperature_K": exact(reference, "FIRE_SMOKE_DESIGN.md SS3.9 case declaration", "methane bring-up"),
            "mole_fractions": {name: rational(ambient_moles.get(name, Fraction(0)))
                               for name in METHANE_SPECIES},
            "mass_fractions": [rational(value) for value in ambient_mass],
            "element_mass_fractions": [rational(value) for value in ambient_elements],
        },
        "injected_fuel_state": {
            "temperature_K": exact(reference, "FIRE_SMOKE_DESIGN.md SS3.9 case declaration", "methane bring-up"),
            "mass_fractions": [rational(value) for value in injected_mass],
            "element_mass_fractions": [rational(value) for value in injected_elements],
        },
        "fuel_formula": FORMULAS["CH4"],
        "lower_heating_value_J_per_kg": {
            "value": float(lhv),
            "exact_rational": rational(lhv),
            "derivation": "-(hf_CO2+2*hf_H2O-hf_CH4)/W_CH4; all gases in NASA reference states",
            "provenance": provenance(
                f"NASA CEA {CEA_REVISION} data/thermo.inp formation enthalpies",
                "https://github.com/nasa/cea"),
        },
        "primary_reaction": {
            "equation": "CH4+2O2->CO2+2H2O",
            "stoichiometric_oxygen_kg_per_kg_fuel": rational(stoichiometric_oxygen),
            "product_coefficients_kg_per_kg_fuel": {
                "CO2": rational(product_co2), "H2O": rational(product_h2o),
                "CO": rational(Fraction(0)), "C(gr)": rational(Fraction(0)),
            },
            "constituent_delta_kg_per_kg_fuel": [rational(value) for value in reaction_delta],
            "reaction_enthalpy_J_per_kmol": rational(reaction_enthalpy),
            "gross_soot_yield_kg_per_kg_fuel": rational(Fraction(0)),
            "condensable_yield_kg_per_kg_fuel": rational(Fraction(0)),
            "effective_stoichiometric_oxygen_kg_per_kg_fuel": rational(stoichiometric_oxygen),
            "effective_heat_release_J_per_kg_fuel": rational(lhv),
            "admissibility_proof": {
                "all_product_coefficients_nonnegative": True,
                "gross_soot_yield_nonnegative": True,
                "condensable_yield_nonnegative": True,
                "effective_stoichiometric_oxygen_positive": True,
                "effective_heat_release_positive": True,
                "exact_mass_residual": rational(Fraction(0)),
                "exact_element_residuals": [rational(Fraction(0))
                                            for _ in METHANE_ELEMENTS],
            },
        },
        "soot_oxidation": {
            "equation": "C(gr)+O2->CO2",
            "oxygen_kg_per_kg_carbon": rational(soot_oxygen),
            "co2_kg_per_kg_carbon": rational(soot_product_co2),
            "heat_release_J_per_kg_carbon": rational(soot_heat),
            "exact_mass_residual": rational(Fraction(1) + soot_oxygen - soot_product_co2),
        },
        "conservative_reconstruction_v1": reconstruction_certificate,
        "nonadvective_flux_projection_v1": flux_certificate,
        "condensable_stream": {
            "kind": "none",
            "yield_kg_per_kg_fuel": rational(Fraction(0)),
            "owner_gated_thermochemistry_required": False,
        },
        "predictive_blockers": [
            "thermochemistry_source_fit_uncertainties_unpublished",
            "methane_soot_yield_calibration_not_present",
            "methane_chem_radiant_source_record_not_present",
        ],
    }


def thermo_payload(snapshot: dict) -> dict:
    common_min, common_max, reference = 300.0, 5000.0, 300.0
    sources = {entry["name"]: entry for entry in snapshot["nasa_cea"]["thermochemistry"]}
    sources[PENTACOSANE_NAME] = derived_pentacosane_source(sources)
    operational_names = THERMO_NAMES + (PENTACOSANE_NAME,)
    species = []
    for name in operational_names:
        source = sources[name]
        species_minimum = max(200.0, float(source["segments"][0]["temperature_min_K"]))
        species.append(clipped_thermo_species(
            source, species_minimum, common_max, reference))
    measured_levoglucosan = dict(snapshot["nist_thermoml"]["levoglucosan_cp"])
    measured_levoglucosan["rows"] = [
        row for row in measured_levoglucosan["rows"] if row[0] <= 370.0
    ]
    if (not measured_levoglucosan["rows"] or
            measured_levoglucosan["rows"][-1][0] != 370.0):
        raise ValueError("Kabo levoglucosan record does not close at 370 K")
    return {
        "schema_version": 1,
        "version": "1.0.0-preview.1",
        "record_kind": "fire_sim_thermochemistry_property_subset",
        "record_name": "fire-sim-thermochemistry-open-subset-v1",
        "record_status": "preview_only",
        "provenance_schema": "fire-optics-canonical-provenance-schema-v1",
        "source_snapshot_sha256": hashlib.sha256(encode(snapshot)).hexdigest(),
        "common_temperature_domain_K": [common_min, common_max],
        "reference_temperature_K": exact(reference, "FIRE_SMOKE_DESIGN.md SS3.3", "all species"),
        "species": species,
        "predictive_blockers": [
            "complete_fuel_element_matrix_and_atom_balance_not_present",
            "fuel_lhv_and_primary_product_coefficients_not_present",
            "injected_fuel_composition_and_temperature_not_present",
            "operational_condensed_organic_thermochemistry_not_present",
            "thermochemistry_source_fit_uncertainties_unpublished",
            "levoglucosan_vapor_owner_gated_missing_record",
            "levoglucosan_condensed_cp_above_370K_owner_gated_missing_record",
        ],
        "missing_required_records": [
            {
                "record_kind": "gas_species_thermochemistry",
                "species_id": "C6H10O5,levoglucosan",
                "required_role": "condensable_vapor",
                "status": "owner_gated_missing_record",
                "failure_policy": "reject_consumers_requiring_species",
            },
            {
                "record_kind": "condensed_species_thermochemistry",
                "species_id": "C6H10O5,condensed-organics",
                "required_role": "condensed_organic_aerosol_above_370K",
                "status": "owner_gated_missing_record",
                "failure_policy": "reject_consumers_requiring_species",
            },
        ],
        "measured_condensed_organics": measured_levoglucosan,
        "measured_condensed_organics_metadata": {
            "uncertainty": {"kind": "expanded_95",
                            "magnitude": "column:expanded_95_J_per_mol_K"},
            "provenance": provenance("Kabo et al., JCT 2015 ThermoML",
                                     snapshot["nist_thermoml"]["locator"]),
            "applicability": "levoglucosan crystal 2; measured 5-370 K only",
            "out_of_domain_policy": "reject",
        },
    }


def transport_payload(snapshot: dict) -> dict:
    common_min, common_max = 300.0, 3000.0
    sources = snapshot["gri_mech_3"]["tables"]
    species = []
    for name in TRANSPORT_NAMES:
        source = sources[name]
        source_citation = ("Cantera 3.1.0 gri30.yaml GRI-Mech transport parameters "
                           "evaluated by its mixture-averaged pure-species implementation; "
                           "original GRI-Mech 3.0 transport.dat hash archived separately")
        species.append({
            "species_id": name,
            "viscosity_model": {
                "kind": "pchip_monotone_c1_v1",
                "columns": source["columns"],
                "rows": source["rows"],
                "interpolation": source["viscosity_interpolation"],
                "value_column": 1,
                "table_metadata": {"uncertainty": {"kind": "computed_range_from_input_sensitivity", "magnitude": [0.0, 0.0],
                                                      "basis": "deterministic evaluation of the pinned GRI/Cantera parameters; physical source-fit uncertainty is not published",
                                                      "scope": "binary64 evaluation and tabulation only"},
                                   "provenance": provenance(source_citation,
                                       "http://combustion.berkeley.edu/gri-mech/version30/files30/transport.dat"),
                                   "applicability": "dilute gas",
                                   "out_of_domain_policy": "reject"},
            },
            "conductivity_model": {
                "kind": "pchip_monotone_c1_v1",
                "columns": source["columns"],
                "rows": source["rows"],
                "interpolation": source["conductivity_interpolation"],
                "value_column": 2,
                "table_metadata": {"uncertainty": {"kind": "computed_range_from_input_sensitivity", "magnitude": [0.0, 0.0],
                                                      "basis": "deterministic evaluation of the pinned GRI/Cantera parameters; physical source-fit uncertainty is not published",
                                                      "scope": "binary64 evaluation and tabulation only"},
                                   "provenance": provenance(source_citation,
                                       "http://combustion.berkeley.edu/gri-mech/version30/files30/transport.dat"),
                                   "applicability": "dilute gas",
                                   "out_of_domain_policy": "reject"},
            },
        })
    return {
        "schema_version": 1,
        "version": "1.0.0-preview.1",
        "record_kind": "fire_sim_transport_closure",
        "record_name": "fire-sim-transport-open-v1",
        "record_status": "preview_only",
        "provenance_schema": "fire-optics-canonical-provenance-schema-v1",
        "source_snapshot_sha256": hashlib.sha256(encode(snapshot)).hexdigest(),
        "common_temperature_domain_K": [common_min, common_max],
        "species": species,
        "viscosity_mixing_law": exact_value("wilke_v1",
            "CHEMKIN TRANSPORT manual Eqs. 48-49", "dilute-gas mixture"),
        "conductivity_mixing_law": exact_value("wassiljewa_mason_saxena_v1",
            "IDAES WMS implementation at the pinned source revision", "dilute-gas mixture"),
        "molecular_diffusivity_relationship": exact_value("D_mol=k_mol/(rho_g*cp_g)",
            "FIRE_SMOKE_DESIGN.md SS3.2", "unit-Lewis Phase-C closure"),
        "sgs_diffusivity_relationship": exact_value("D_sgs=nu_sgs/Sc_t",
            "FIRE_SMOKE_DESIGN.md SS3.2", "Phase-C LES"),
        "total_diffusivity_relationship": exact_value("D=D_mol+D_sgs",
            "FIRE_SMOKE_DESIGN.md SS3.2", "Phase-C gas and mixture-fraction transport"),
        "effective_conductivity_relationship": exact_value(
            "k_eff=k_mol+rho_g*cp_g*nu_sgs/Pr_t",
            "FIRE_SMOKE_DESIGN.md SS3.2", "Phase-C gas sensible-energy transport"),
        "effective_viscosity_relationship": exact_value(
            "mu_eff=mu_mol+rho_g*nu_sgs",
            "FIRE_SMOKE_DESIGN.md SS3.2", "Phase-C gas momentum transport"),
        "shared_diffusivity_rule": exact_value("same_D_for_every_J_j_and_J_Z",
            "FIRE_SMOKE_DESIGN.md SS3.2", "Phase-C constituent transport"),
        "dns_sgs_rule": exact_value("nu_sgs=0;D_sgs=0;retain_molecular_laws",
            "FIRE_SMOKE_DESIGN.md SS3.2", "Phase-C DNS mode"),
        "turbulent_prandtl": exact(0.7, "FIRE_SMOKE_DESIGN.md SS3.2; engineering-LES choice", "Phase-C LES"),
        "turbulent_schmidt": exact(0.7, "FIRE_SMOKE_DESIGN.md SS3.2; engineering-LES choice", "Phase-C LES"),
        "vreman_Cv": exact(0.07, "Vreman 2004 and FIRE_SMOKE_DESIGN.md SS3.2", "directional MAC widths"),
        "vreman_alpha_relationship": exact_value("alpha_ij=du_j/dx_i",
            "Vreman 2004 and FIRE_SMOKE_DESIGN.md SS3.2", "Phase-C velocity gradient"),
        "vreman_beta_relationship": exact_value(
            "beta_ij=sum_m(Delta_m^2*alpha_mi*alpha_mj)",
            "Vreman 2004 and FIRE_SMOKE_DESIGN.md SS3.2", "directional MAC widths"),
        "vreman_B_beta_relationship": exact_value(
            "B_beta=beta_11*beta_22-beta_12^2+beta_11*beta_33-beta_13^2+beta_22*beta_33-beta_23^2",
            "Vreman 2004 and FIRE_SMOKE_DESIGN.md SS3.2", "Phase-C LES"),
        "vreman_nu_sgs_relationship": exact_value(
            "nu_sgs=C_v*sqrt(B_beta/sum_ij(alpha_ij^2))",
            "Vreman 2004 and FIRE_SMOKE_DESIGN.md SS3.2", "Phase-C LES"),
        "vreman_nonnegative_B_rule": exact_value("B_beta=max(0,raw_B_beta)",
            "Vreman 2004 and FIRE_SMOKE_DESIGN.md SS3.2", "binary64 evaluation"),
        "vreman_zero_denominator_rule": exact_value(
            "nu_sgs=0_when_sum_ij(alpha_ij^2)=0",
            "Vreman 2004 and FIRE_SMOKE_DESIGN.md SS3.2", "Phase-C LES"),
        "vreman_Cnu": exact(0.1, "FIRE_SMOKE_DESIGN.md SS3.3", "k_sgs closure"),
        "tau_chem_s": exact(1.0e-4, "FIRE_SMOKE_DESIGN.md SS3.3; declared bound distinct from FDS 1e-5", "Phase-C fuels"),
        "critical_flame_temperature_K": exact(1700.0, "FDS data.f90 and FIRE_SMOKE_DESIGN.md SS3.3", "hydrocarbon baseline"),
        "wall_stress": exact_value("resolved_molecular_no_slip",
            "FIRE_SMOKE_DESIGN.md SS3.2", "Phase-C walls"),
        "wall_heat_flux": exact_value("adiabatic",
            "FIRE_SMOKE_DESIGN.md SS3.2", "Phase-C walls"),
        "filter_widths": exact_value("directional_mac_cell_widths",
            "Vreman 2004 and FIRE_SMOKE_DESIGN.md SS3.2", "uniform Phase-C MAC grid"),
        "predictive_blockers": [
            "transport_source_fit_uncertainties_unpublished",
        ],
    }


def solver_fixture_payloads() -> list[tuple[str, dict]]:
    """Contrived RED inputs; never physical fuel presets or solver defaults."""
    delta = Fraction(1, 1 << 60)
    common = {
        "schema_version": 1,
        "version": "1.0.0-preview.1",
        "record_kind": "fire_sim_solver_verification_fixture",
        "record_class": "synthetic_verification_fixture",
        "applicability": "V-tier RED testing only; forbidden as a physical preset",
    }
    near_rank = dict(common)
    near_rank.update({
        "record_name": "fire-sim-solver-fixture-near-rank-deficient-v1",
        "fixture_kind": "declared_rank_too_low",
        "candidate_constraint_matrix": rational_matrix([
            [Fraction(1), Fraction(0)], [Fraction(0), delta],
        ]),
        "candidate_declared_rank": 1,
        "exact_rank": 2,
        "expected_outcome": "reject_exact_rank_certificate",
    })
    wrong_subspace = dict(common)
    wrong_subspace.update({
        "record_name": "fire-sim-solver-fixture-correct-rank-wrong-subspace-v1",
        "fixture_kind": "correct_rank_wrong_numerical_subspace",
        "candidate_constraint_matrix": rational_matrix([
            [Fraction(1), Fraction(0), -delta, Fraction(-1)],
            [Fraction(0), Fraction(0), delta, Fraction(0)],
            [Fraction(-1), Fraction(0), Fraction(0), Fraction(1)],
        ]),
        "candidate_declared_rank": 2,
        "candidate_numerical_basis": [
            [0.0, 0.0], [1.0, 0.0], [0.0, 1.0], [0.0, 0.0],
        ],
        "expected_outcome": "reject_projector_subspace_certificate",
    })
    return [
        ("kFireSimSolverNearRankDeficientFixtureV1", near_rank),
        ("kFireSimSolverWrongSubspaceFixtureV1", wrong_subspace),
    ]


def generate(snapshot_path: Path) -> str:
    snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
    snapshot_id = hashlib.sha256(encode(snapshot)).hexdigest()
    if snapshot_id != EXPECTED_SOURCE_SNAPSHOT_SHA256:
        raise ValueError(
            f"open-source snapshot identity mismatch: expected "
            f"{EXPECTED_SOURCE_SNAPSHOT_SHA256}, got {snapshot_id}"
        )
    if "burcat" in json.dumps(snapshot).lower() or "hitemp" in json.dumps(snapshot).lower():
        raise ValueError("license-gated Burcat/HITEMP bytes are forbidden in this generator")
    thermo = encode(thermo_payload(snapshot))
    methane = encode(methane_payload(snapshot))
    transport = encode(transport_payload(snapshot))
    target = io.StringIO(newline="\n")
    target.write("// Generated from docs/data/source_pulls/fire_sim_open_sources_v1.json.\n\n")
    emit_array(target, "kFireSimThermochemistryOpenV1", thermo)
    emit_array(target, "kFireSimMethanePhysicalV1", methane)
    emit_array(target, "kFireSimTransportOpenV1", transport)
    for symbol, payload in solver_fixture_payloads():
        emit_array(target, symbol, encode(payload))
    return target.getvalue()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--extract-sources", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--thermo", type=Path)
    parser.add_argument("--transport", type=Path)
    parser.add_argument("--thermoml", type=Path)
    parser.add_argument("--gri-transport", type=Path)
    parser.add_argument("--gri-yaml", type=Path)
    parser.add_argument("--idaes-wms", type=Path)
    parser.add_argument("--idaes-wms-doc", type=Path)
    parser.add_argument("--idaes-license", type=Path)
    parser.add_argument("--vreman-pdf", type=Path)
    parser.add_argument("--fds-cons", type=Path)
    parser.add_argument("--fds-data", type=Path)
    parser.add_argument("--fds-license", type=Path)
    parser.add_argument("source_snapshot", type=Path)
    parser.add_argument("output", type=Path, nargs="?")
    args = parser.parse_args()
    validate_unicode17_authority()
    if args.extract_sources:
        if (not args.thermo or not args.transport or not args.thermoml or
                not args.gri_transport or not args.gri_yaml or not args.idaes_wms or
                not args.idaes_wms_doc or not args.idaes_license or not args.vreman_pdf or
                not args.fds_cons or not args.fds_data or not args.fds_license):
            parser.error("--extract-sources requires every pinned open physical source")
        extract_sources(args.thermo, args.transport, args.thermoml,
                        args.gri_transport, args.gri_yaml, args.idaes_wms,
                        args.idaes_wms_doc, args.idaes_license, args.vreman_pdf,
                        args.fds_cons, args.fds_data, args.fds_license,
                        args.source_snapshot)
        return
    if not args.output:
        parser.error("output is required unless --extract-sources is used")
    generated = generate(args.source_snapshot)
    if args.check:
        if not args.output.is_file() or args.output.read_text(encoding="utf-8") != generated:
            raise SystemExit(f"{args.output} is stale")
    else:
        with args.output.open("w", encoding="utf-8", newline="\n") as output:
            output.write(generated)


if __name__ == "__main__":
    main()
