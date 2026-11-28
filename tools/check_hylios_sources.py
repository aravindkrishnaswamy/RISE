#!/usr/bin/env python3
"""Validate literal HyLIoS source-table transcription, not biological correctness."""
from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
PRESETS = ROOT / "data/hylios/presets_source.json"
MANIFEST = ROOT / "data/hylios/source_manifest.json"

EXPECTED_IDS = {"S1", "S2", "S3", "S4"}
EXPECTED_THICKNESS_CM = {
    "S1": [0.001, 0.0017, 0.0017, 0.0017, 0.02, 0.1],
    "S2": [0.001, 0.0033, 0.0033, 0.0033, 0.01, 0.1],
    "S3": [0.0004, 0.0033, 0.0033, 0.0033, 0.02, 0.125],
    "S4": [0.0002, 0.0007, 0.0007, 0.0007, 0.023, 0.2],
}
EXPECTED_SCALAR_FIELDS = {
    "surface_fold_aspect_ratio": {"S1": 0.1, "S2": 0.25, "S3": 0.1, "S4": 0.45},
    "melanosome_eumelanin_concentration_mg_per_ml": {"S1": 90.0, "S2": 50.0, "S3": 32.0, "S4": 50.0},
    "melanosome_pheomelanin_concentration_mg_per_ml": {"S1": 4.0, "S2": 2.0, "S3": 2.0, "S4": 4.0},
    "oxygenated_blood_fraction_percent": {"S1": 75.0, "S2": 95.0, "S3": 75.0, "S4": 75.0},
}
EXPECTED_LAYERS = [
    "stratum_corneum", "stratum_granulosum", "stratum_spinosum",
    "stratum_basale", "papillary_dermis", "reticular_dermis",
]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def close_list(actual: list[float], expected: list[float], label: str) -> None:
    require(len(actual) == len(expected), f"{label}: expected {len(expected)} source values, got {len(actual)}")
    for index, (got, want) in enumerate(zip(actual, expected)):
        require(math.isclose(got, want, rel_tol=0.0, abs_tol=1e-12),
                f"{label}[{index}]: source transcription {got!r}, expected {want!r}")


def convert_source_value(value: float, unit: str) -> tuple[float, str]:
    """Convert a single explicitly supported source unit to SI without changing source records."""
    conversions = {"cm": (0.01, "m"), "percent": (0.01, "fraction"),
                   "mol/L": (1000.0, "mol/m^3"), "um": (1e-6, "m")}
    require(unit in conversions, f"unsupported source unit {unit!r}")
    factor, si_unit = conversions[unit]
    return value * factor, si_unit


def validate(presets: dict[str, Any], manifest: dict[str, Any]) -> list[str]:
    require(presets.get("schema_version") == 1, "presets_source schema_version must be 1")
    require(presets.get("not_executable") is True, "source table transcription must remain marked not_executable")
    sources = manifest.get("sources")
    require(isinstance(sources, list) and bool(sources), "manifest source provenance is required")
    by_id = {source["id"]: source for source in sources}
    require(len(by_id) == len(sources), "duplicate source provenance IDs")
    for source in sources:
        require(isinstance(source.get("sha256"), str) and
                re.fullmatch(r"[0-9a-f]{64}", source["sha256"]) is not None,
                "source SHA256 must contain 64 lowercase hexadecimal digits")
        for key in ("id", "citation", "url"):
            require(isinstance(source.get(key), str) and bool(source[key].strip()),
                    f"source provenance requires {key}")
    references = presets.get("sources")
    require(isinstance(references, dict) and set(references) == {"table_iii", "table_iv", "organization"},
            "preset source provenance requires both tables and organization")
    for reference in references.values():
        require(reference.get("source_id") in by_id, "preset references unknown source ID")
        require(reference.get("sha256") == by_id[reference["source_id"]]["sha256"],
                "preset source SHA256 does not match manifest source")
        require(isinstance(reference.get("citation"), str) and bool(reference["citation"].strip()),
                "preset source citation is required")
    blockers = manifest.get("unresolved_blockers")
    require(isinstance(blockers, list) and all(isinstance(x, str) and x.strip() for x in blockers),
            "unresolved blocker list is required (may be empty after resolution)")
    require(presets.get("layer_order_outer_to_inner") == EXPECTED_LAYERS, "outer-to-inner source layer order mismatch")
    require(set(presets["organization_by_preset"]) == EXPECTED_IDS, "organization must have exactly S1-S4")
    require(presets["organization_by_preset"]["S1"] == "complexes" and
            presets["organization_by_preset"]["S2"] == "complexes" and
            presets["organization_by_preset"]["S3"] == "complexes" and
            presets["organization_by_preset"]["S4"] == "individually_dispersed_particles",
            "paper Table I organization transcription mismatch")
    fields = presets["table_iii_fields"]
    require(fields["melanosome_dimensions_um"].get("interpretation_status") in {
        "unresolved_full_dimensions_vs_semiaxes", "resolved_full_dimensions", "resolved_semiaxes"},
        "melanosome dimension interpretation status is required")
    units = {
        "surface_fold_aspect_ratio": "dimensionless",
        "thickness_cm_by_layer": "cm",
        "melanosome_content_percent_by_epidermal_layer": "percent",
        "colloidal_melanin_content_percent_by_epidermal_layer": "percent",
        "melanosome_eumelanin_concentration_mg_per_ml": "mg/mL",
        "melanosome_pheomelanin_concentration_mg_per_ml": "mg/mL",
        "melanosome_dimensions_um": "µm × µm",
        "blood_content_percent_by_dermis_layer": "percent",
        "oxygenated_blood_fraction_percent": "percent",
    }
    for name, unit in units.items():
        require(fields[name].get("unit") == unit, f"{name}: source unit must remain {unit}")
    for name in ("melanosome_content_percent_by_epidermal_layer",
                 "colloidal_melanin_content_percent_by_epidermal_layer"):
        require(fields[name].get("layer_order") == EXPECTED_LAYERS[1:4],
                f"{name}: epidermal layer order mismatch")
    require(fields["blood_content_percent_by_dermis_layer"].get("layer_order") == EXPECTED_LAYERS[4:],
            "blood content dermal layer order mismatch")
    for name, expected in EXPECTED_SCALAR_FIELDS.items():
        values = fields[name]["values"]
        require(set(values) == EXPECTED_IDS, f"{name}: expected exactly S1-S4")
        for sid, want in expected.items():
            require(math.isclose(values[sid], want, rel_tol=0.0, abs_tol=1e-12),
                    f"{name}.{sid}: expected published source value {want}, got {values[sid]}")
    expected_full_fields = {
        "melanosome_content_percent_by_epidermal_layer": {
            "S1": [1.0, 1.0, 1.0], "S2": [0.0, 0.0, 3.75],
            "S3": [0.0, 0.0, 3.0], "S4": [10.0, 10.0, 10.0]},
        "colloidal_melanin_content_percent_by_epidermal_layer": {
            "S1": [0.8, 0.8, 0.8], "S2": [1.25, 1.25, 1.25],
            "S3": [1.35, 1.35, 1.35], "S4": [15.0, 15.0, 15.0]},
        "melanosome_dimensions_um": {
            "S1": [0.41, 0.17], "S2": [0.41, 0.17],
            "S3": [0.41, 0.17], "S4": [0.69, 0.28]},
        "blood_content_percent_by_dermis_layer": {
            "S1": [0.2, 0.2], "S2": [0.7, 0.7],
            "S3": [0.3, 0.3], "S4": [2.5, 2.5]},
    }
    for name, expected in expected_full_fields.items():
        values = fields[name]["values"]
        require(set(values) == EXPECTED_IDS, f"{name}: expected exactly S1-S4")
        for sid, want in expected.items():
            close_list(values[sid], want, f"Table III {name}.{sid}")
    require(set(fields) == {
        "surface_fold_aspect_ratio", "thickness_cm_by_layer",
        "melanosome_content_percent_by_epidermal_layer",
        "colloidal_melanin_content_percent_by_epidermal_layer",
        "melanosome_eumelanin_concentration_mg_per_ml",
        "melanosome_pheomelanin_concentration_mg_per_ml", "melanosome_dimensions_um",
        "blood_content_percent_by_dermis_layer", "oxygenated_blood_fraction_percent",
    }, "Table III field set changed; review source table before updating checker")
    table_iv = presets["table_iv_fixed_source_values"]
    expected_table_iv = {
        "skin_layer_refractive_index_at_1300_nm": {"unit":"dimensionless", "values":{"stratum_corneum":1.55,"epidermis":1.4,"papillary_dermis":1.39,"reticular_dermis":1.41}},
        "melanin_refractive_index": {"unit":"dimensionless", "value":1.7},
        "hemoglobin_concentration_whole_blood_mg_per_ml": {"unit":"mg/mL", "value":147.0},
        "methemoglobin_concentration_mg_per_ml": {"unit":"mg/mL", "value":1.5},
        "carboxyhemoglobin_concentration_mg_per_ml": {"unit":"mg/mL", "value":1.5},
        "sulfhemoglobin_concentration_mg_per_ml": {"unit":"mg/mL", "value":0.0},
        "whole_blood_bilirubin_concentration_mg_per_ml": {"unit":"mg/mL", "value":0.003},
        "beta_carotene_concentration_mg_per_ml": {"unit":"mg/mL", "values":{"stratum_corneum":0.00021,"epidermis":0.00021,"blood":0.00007}},
        "water_content_percent": {"unit":"percent", "values":{"stratum_corneum":35.0,"epidermis":60.0,"papillary_dermis":75.0,"reticular_dermis":75.0}},
        "lipid_content_percent": {"unit":"percent", "values":{"stratum_corneum":20.0,"epidermis":15.1,"papillary_dermis":17.33,"reticular_dermis":17.33}},
        "stratum_corneum_keratin_content_percent": {"unit":"percent", "value":65.0},
        "stratum_corneum_urocanic_acid_density_mol_per_l": {"unit":"mol/L", "value":0.01},
        "skin_dna_density_mg_per_ml": {"unit":"mg/mL", "value":0.185},
    }
    require(set(table_iv) == set(expected_table_iv), "Table IV field set changed; review source before updating checker")
    require(table_iv == expected_table_iv, "Table IV source transcription mismatch")

    thickness = fields["thickness_cm_by_layer"]
    require(thickness["unit"] == "cm", "thickness source unit must remain cm")
    require(thickness["layer_order"] == "layer_order_outer_to_inner", "thickness layer-order reference mismatch")
    require(set(thickness["values"]) == EXPECTED_IDS, "thickness must have exactly S1-S4")
    for sid, expected in EXPECTED_THICKNESS_CM.items():
        close_list(thickness["values"][sid], expected, f"Table III thickness {sid}")


    require(manifest.get("schema_version") == 1, "source_manifest schema_version must be 1")
    require(manifest.get("author_data_index", {}).get("url") == "https://www.npsg.uwaterloo.ca/data/skin.php",
            "author data index URL missing")
    require(manifest["curve_inventory"].get("coverage_is_certified") is False,
            "curve inventory must not claim full coverage certification")
    decisions = {item["id"]: item for item in manifest.get("scientific_decisions", [])}
    require({"melanosome_dimension_convention", "keratin_absorptance_to_coefficient",
             "dna_concentration_scaling", "urocanic_acid_concentration_scaling",
             "full_range_spectral_coverage"} <= set(decisions), "required scientific decision records missing")
    require(all(item.get("status") in {"unresolved", "resolved_with_interpretation", "resolved_from_source"}
                for item in decisions.values()), "scientific decision status invalid")
    author_ids = {item["id"] for item in manifest["author_data_index"]["items"]}
    require({"keratin_abs", "dna_abs", "uro_acid_abs", "water_absorption", "lipid_absorption"} <= author_ids,
            "author numeric data inventory incomplete")
    water = {item["id"]: item for item in manifest["author_water_data_index"]["items"]}
    hale = water["hale_querry_water_real_ior"]
    require(hale["files"] == ["water/water2_n.m"],
            "Hale–Querry must use its script; riW2.txt is the IAPWS filename collision")
    require(water["iapws_water_real_ior"]["reported_range_nm"] == [200, 1100],
            "IAPWS candidate coverage must remain 200–1100 nm")
    require(hale["reported_range_nm"] == [200, 2500],
            "Hale–Querry candidate coverage must remain 200–2500 nm")
    require(any(item.get("file") == "water/riW2.txt" for item in hale.get("excluded_artifacts", [])),
            "Hale–Querry filename collision must remain documented")
    return ["Source-table transcription schema and exact Table III values validated.",
            "source values remain in original units; conversion helper is independently unit-tested.",
            "Coverage certification and unresolved axis/data blockers remain explicit."]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="validate source transcription (default action)")
    args = parser.parse_args()
    del args
    try:
        results = validate(json.loads(PRESETS.read_text()), json.loads(MANIFEST.read_text()))
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError) as exc:
        parser.error(str(exc))
    for result in results:
        print(f"OK: {result}")
    print("Scope: source transcription only; no physiological or spectral-data certification.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
