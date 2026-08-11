#!/usr/bin/env python3
"""Freeze Phase-C open property subsets into RISE-CBOR64-v1.

The operational records are deliberately generated from redistribution-safe
NASA CEA, NIST ThermoML, and GRI-Mech/Cantera inputs.  Burcat and HITEMP content
is excluded so license-gated coefficients cannot enter the generated records.
The thermochemistry output is intentionally not a solver-ready §3.3 gas
record: its explicit blockers name every missing fuel/aerosol closure.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import re
import sys
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
}
EXPECTED_SOURCE_SNAPSHOT_SHA256 = "cdfaefb6f6153a9afc5a1dd1bb0dbd7c6e2581d5c47e26ff110f97aee5a4ee6a"
THERMO_NAMES = (
    "Ar", "CH4", "CH3OH", "CO", "CO2", "C7H16,n-heptane",
    "H2O", "N2", "O2", "C(gr)",
)
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
}


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
                    gri_transport: Path, gri_yaml: Path, output: Path) -> None:
    forbidden = "burcat" in str(thermo).lower() or "burcat" in str(transport).lower()
    if forbidden:
        raise ValueError("Burcat-derived bytes are license-gated and forbidden")
    thermo_hash = require_source_digest(thermo, "nasa_cea_thermo")
    transport_hash = require_source_digest(transport, "nasa_cea_transport")
    thermoml_hash = require_source_digest(thermoml, "nist_thermoml")
    gri_transport_hash = require_source_digest(gri_transport, "gri_transport")
    gri_yaml_hash = require_source_digest(gri_yaml, "cantera_gri30_yaml")
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
            "thermochemistry": [parse_thermo(thermo_lines, name) for name in THERMO_NAMES],
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
                "locator": "https://idaes-pse.readthedocs.io/en/stable/explanations/components/property_package/general/transport_properties/thermal_conductivity_wms.html",
                "status": "open locator recorded; exact reference bytes/revision not pinned",
            },
            "vreman_2004": {
                "locator": "https://www.vremanresearch.nl/Vreman2004.pdf",
                "status": "author-hosted open locator recorded; exact PDF bytes not pinned",
            },
            "fds_source": {
                "locator": "https://github.com/firemodels/fds",
                "status": "open repository locator recorded; exact revision/files not pinned",
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


def clipped_thermo_species(source: dict, common_min: float, common_max: float,
                           reference_temperature: float) -> dict:
    mw = float(source["molecular_weight_kg_per_kmol"])
    segments = []
    offset = 0.0
    previous_end = reference_temperature
    ordered = source["segments"]
    for raw in ordered:
        lo = max(common_min, float(raw["temperature_min_K"]))
        hi = min(common_max, float(raw["temperature_max_K"]))
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
    if not segments or segments[0]["temperature_min_K"] != common_min or previous_end != common_max:
        raise ValueError(f"thermochemistry does not span [{common_min},{common_max}] for {source['name']}")
    source_citation = f"NASA CEA {CEA_REVISION} data/thermo.inp: {source['source_header']}"
    return {
        "species_id": source["name"],
        "formula": source["formula"],
        "phase": "aerosol_solid" if source["name"] == "C(gr)" else "gas",
        "molecular_weight_kg_per_kmol": source_envelope(mw, source_citation, "pinned NASA interval set"),
        "formation_enthalpy_J_per_kmol_298p15K": source_envelope(
            source["formation_enthalpy_J_per_kmol_298p15K"], source_citation, "standard state at 298.15 K"),
        "cp_hs_model": {
            "kind": "nasa9_cp_with_continuous_integrated_hs_v1",
            "temperature_domain_K": [common_min, common_max],
            "reference_temperature_K": reference_temperature,
            "segments": segments,
            "table_metadata": {
                "uncertainty": {"kind": "computed_range_from_input_sensitivity", "magnitude": [0.0, 0.0],
                                "basis": "pinned NASA coefficients; source fit uncertainty is not published"},
                "provenance": provenance(source_citation, "https://github.com/nasa/cea"),
                "applicability": "closed interval only; continuous h_s is integrated from source cp",
                "out_of_domain_policy": "reject",
            },
        },
    }


def thermo_payload(snapshot: dict) -> dict:
    common_min, common_max, reference = 300.0, 5000.0, 300.0
    sources = {entry["name"]: entry for entry in snapshot["nasa_cea"]["thermochemistry"]}
    species = [clipped_thermo_species(sources[name], common_min, common_max, reference)
               for name in THERMO_NAMES]
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
            "levoglucosan_vapor_burcat_redistribution_unresolved",
            "pentacosane_vapor_burcat_redistribution_unresolved",
            "levoglucosan_condensed_cp_above_370K_assumption_bound_unpinned",
            "pentacosane_domain_extension_below_298p15K_assumption_bound_unpinned",
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
            "IDAES WMS implementation; exact reference bytes not yet pinned", "dilute-gas mixture"),
        "unit_lewis_diffusivity": exact_value("D_mol=k_mol/(rho_g*cp_g)",
            "FIRE_SMOKE_DESIGN.md SS3.2", "unit-Lewis Phase-C closure"),
        "turbulent_prandtl": exact(0.7, "FIRE_SMOKE_DESIGN.md SS3.2; engineering-LES choice", "Phase-C LES"),
        "turbulent_schmidt": exact(0.7, "FIRE_SMOKE_DESIGN.md SS3.2; engineering-LES choice", "Phase-C LES"),
        "vreman_Cv": exact(0.07, "Vreman 2004 and FIRE_SMOKE_DESIGN.md SS3.2", "directional MAC widths"),
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
            "idaes_wms_reference_bytes_unpinned",
            "vreman_reference_bytes_unpinned",
            "fds_constant_reference_bytes_unpinned",
        ],
    }


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
    transport = encode(transport_payload(snapshot))
    target = io.StringIO(newline="\n")
    target.write("// Generated from docs/data/source_pulls/fire_sim_open_sources_v1.json.\n\n")
    emit_array(target, "kFireSimThermochemistryOpenV1", thermo)
    emit_array(target, "kFireSimTransportOpenV1", transport)
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
    parser.add_argument("source_snapshot", type=Path)
    parser.add_argument("output", type=Path, nargs="?")
    args = parser.parse_args()
    validate_unicode17_authority()
    if args.extract_sources:
        if (not args.thermo or not args.transport or not args.thermoml or
                not args.gri_transport or not args.gri_yaml):
            parser.error("--extract-sources requires NASA, ThermoML, and GRI/Cantera inputs")
        extract_sources(args.thermo, args.transport, args.thermoml,
                        args.gri_transport, args.gri_yaml, args.source_snapshot)
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
