#!/usr/bin/env python3
"""Safely import a small pinned subset of public HyLIoS MATLAB numeric data.

No MATLAB code is evaluated. Only allowlisted numeric array assignments are
parsed, source checksums are required, and original grids are preserved. Other
MATLAB statements are ignored as text; this is not a whole-language validator.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = ROOT / "data/hylios/source_manifest.json"
LN10 = math.log(10.0)

# Explicit supported data subset. Other manifest entries fail closed.
CURVE_SPECS = {
    "keratin_abs": {"arrays": ("wave0", "data0"), "quantity": "decadic_absorbance", "unit": "dimensionless", "path_m": 4e-6, "reference_basis": "solid keratin sample; no Table IV 65% mixture applied", "transform": "mu_a_m^-1 = ln(10) * absorbance / 4e-6 m"},
    "dna_abs": {"arrays": ("wave0", "data0"), "quantity": "normalized_decadic_absorbance", "unit": "dimensionless", "path_m": 0.01, "reference_basis": "50 ug/mL sample; MATLAB point-12 normalization applied once; no skin concentration scaling", "transform": "mu_a_m^-1 = ln(10) * normalized_absorbance / 0.01 m"},
    "uro_acid_abs": {"arrays": ("wave0", "data0"), "quantity": "decadic_absorbance", "unit": "dimensionless", "path_m": 0.01, "reference_basis": "15 umol/L sample; no Table IV concentration scaling", "transform": "mu_a_m^-1 = ln(10) * absorbance / 0.01 m"},
    "eumelanin_extinction": {"arrays": ("wave0", "data0"), "quantity": "mass_extinction_coefficient", "unit": "cm^-1/(mg/mL)", "reference_basis": "author tabulation; no concentration scaling", "transform": None},
    "pheomelanin_extinction": {"arrays": ("wave0", "data0"), "quantity": "mass_extinction_coefficient", "unit": "cm^-1/(mg/mL)", "reference_basis": "author tabulation; no concentration scaling", "transform": None},
    "oxy_hemoglobin_extinction": {"arrays": ("wave0", "data0"), "quantity": "molar_extinction_coefficient", "unit": "cm^-1/(mol/L)", "manifest_id": "oxy_deoxy_hemoglobin", "script_suffix": "oxy_hemoglobin_extinction.m", "reference_basis": "author tabulation; no concentration or molar-mass scaling", "transform": None},
    "deoxy_hemoglobin_extinction": {"arrays": ("wave0", "data0"), "quantity": "molar_extinction_coefficient", "unit": "cm^-1/(mol/L)", "manifest_id": "oxy_deoxy_hemoglobin", "script_suffix": "deoxy_hemoglobin_extinction.m", "reference_basis": "author tabulation; no concentration or molar-mass scaling", "transform": None},
    "carboxyhemoglobin": {"matrix": "M", "quantity": "molar_extinction_coefficient", "unit": "cm^-1/(mol/L)", "reference_basis": "author tabulation; no concentration or molar-mass scaling", "transform": None},
    "methemoglobin": {"matrix": "Mmeth", "quantity": "molar_extinction_coefficient", "unit": "cm^-1/(mol/L)", "reference_basis": "author tabulation; no concentration or molar-mass scaling", "transform": None},
    "sulfhemoglobin": {"matrix": "Msulf", "quantity": "molar_extinction_coefficient", "unit": "cm^-1/(mol/L)", "reference_basis": "author tabulation; no concentration or molar-mass scaling", "transform": None},
    "beta_carotene": {"arrays": ("wave0", "data0"), "quantity": "molar_extinction_coefficient", "unit": "cm^-1/(mol/L)", "reference_basis": "author tabulation; no concentration or molar-mass scaling", "transform": None},
    "bilirubin": {"arrays": ("wave0", "data0"), "quantity": "molar_extinction_coefficient", "unit": "cm^-1/(mol/L)", "reference_basis": "author tabulation; no concentration or molar-mass scaling", "transform": None},
    "water_absorption": {"arrays": ("wave", "data"), "quantity": "Napierian_absorption_coefficient", "unit": "cm^-1", "reference_basis": "author-combined water table; original source branches are 380–725 nm and 730–2500 nm", "transform": "mu_a_m^-1 = 100 * mu_a_cm^-1"},
    "lipid_absorption": {"arrays": ("wave0", "data0"), "quantity": "Napierian_absorption_coefficient", "unit": "cm^-1", "reference_basis": "author combined mammalian-fat/lipid table; no tissue-mixture scaling", "transform": "mu_a_m^-1 = 100 * mu_a_cm^-1"},
    "palmer_williams_real_ior": {"arrays": ("data",), "quantity": "real_refractive_index", "unit": "dimensionless", "grid_nm": (400, 2500, 5), "reference_basis": "water; author table states linearly interpolated source values", "transform": None},
}


NUMBER = re.compile(r"[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?")


class ImportErrorDetail(ValueError):
    pass


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ImportErrorDetail(message)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _parse_numeric_array(text: str, variable: str) -> list[float]:
    # Only a literal numeric vector RHS is accepted; no MATLAB expression parsing/evaluation.
    pattern = rf"(?m)^\s*{re.escape(variable)}\s*=\s*\[(.*?)\]\s*;?\s*$"
    matches = list(re.finditer(pattern, text, re.DOTALL))
    _require(len(matches) == 1, f"expected exactly one literal {variable}=[numeric values] assignment")
    body = matches[0].group(1).strip()
    tokens = re.split(r"[,\s;]+", body) if body else []
    _require(bool(tokens) and all(NUMBER.fullmatch(token) for token in tokens),
             f"unsupported expression or token in {variable} array")
    values = [float(token) for token in tokens]
    _require(len(values) >= 2, f"{variable} array must contain at least two values")
    _require(all(math.isfinite(value) for value in values), f"{variable} contains non-finite values")
    return values


def _parse_numeric_matrix(text: str, variable: str) -> list[list[float]]:
    """Read a two-column MATLAB literal matrix with semicolon rows and `...` continuations."""
    pattern = rf"(?m)^\s*{re.escape(variable)}\s*=\s*\[(.*?)\]\s*;?\s*$"
    matches = list(re.finditer(pattern, text, re.DOTALL))
    _require(len(matches) == 1, f"expected exactly one literal {variable}=[two-column rows] assignment")
    body = re.sub(r"(?m)\.\.\.[ \t]*(?:\r?\n|$)", " ", matches[0].group(1))
    rows = [row.strip() for row in body.split(";") if row.strip()]
    _require(len(rows) >= 2, f"{variable} matrix must contain at least two rows")
    parsed = []
    for row_index, row in enumerate(rows):
        columns = [column.strip() for column in row.split(",")]
        _require(len(columns) == 2 and all(NUMBER.fullmatch(column) for column in columns),
                 f"unsupported {variable} matrix row {row_index}: expected two numeric literals")
        values = [float(column) for column in columns]
        _require(all(math.isfinite(value) for value in values), f"{variable} contains non-finite values")
        parsed.append(values)
    return parsed


def _load_pinned_files(source_dir: Path, entry: dict[str, Any]) -> tuple[dict[str, bytes], dict[str, str]]:
    files, hashes = entry.get("files"), entry.get("sha256")
    _require(isinstance(files, list) and isinstance(hashes, list) and len(files) == len(hashes),
             f"manifest entry {entry.get('id')} must contain aligned files and sha256 arrays")
    payloads, actual = {}, {}
    for relative, expected in zip(files, hashes):
        path = (source_dir / relative).resolve()
        _require(path.is_relative_to(source_dir.resolve()), f"source path escapes source-dir: {relative}")
        _require(path.is_file(), f"missing pinned source file: {relative}")
        data = path.read_bytes()
        digest = _sha256(data)
        _require(digest == expected, f"SHA256 mismatch for {relative}: expected {expected}, got {digest}")
        payloads[relative] = data
        actual[relative] = digest
    return payloads, actual


def _entry_map(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    entries = list(manifest.get("author_data_index", {}).get("items", []))
    entries.extend(manifest.get("author_water_data_index", {}).get("items", []))
    return {entry["id"]: entry for entry in entries}


def import_curve(curve_id: str, entry: dict[str, Any], source_dir: Path) -> dict[str, Any]:
    _require(curve_id in CURVE_SPECS,
             f"unsupported dataset {curve_id!r}; supported: {', '.join(CURVE_SPECS)}")
    spec = CURVE_SPECS[curve_id]
    expected_manifest_id = spec.get("manifest_id", curve_id)
    _require(entry.get("id") == expected_manifest_id,
             f"manifest entry id {entry.get('id')!r} does not match requested {curve_id!r} (expected {expected_manifest_id!r})")
    payloads, hashes = _load_pinned_files(source_dir, entry)
    script_suffix = spec.get("script_suffix")
    if script_suffix:
        expected_name = Path(script_suffix).name
        matches = [p for p in payloads if Path(p).name == expected_name]
    else:
        matches = [p for p in payloads if p.endswith(".m")]
    _require(len(matches) == 1,
             f"{curve_id}: expected exactly one pinned MATLAB source file matching {script_suffix or '*.m'}, found {len(matches)}")
    script_rel = matches[0]
    # Public scripts vary between UTF-8 and Windows-1252 comments; numeric syntax is ASCII.
    source = payloads[script_rel].decode("latin-1")
    if "matrix" in spec:
        variable = spec["matrix"]
        assignment_count = len(re.findall(rf"(?im)^\s*{re.escape(variable)}\s*=", source))
        _require(assignment_count == 1, f"{curve_id}: unsupported or duplicate assignment to {variable} ({assignment_count} assignments)")
        matrix = _parse_numeric_matrix(source, variable)
        wavelengths = [row[0] for row in matrix]
        values = [row[1] for row in matrix]
        arrays = {}
    else:
        arrays = {name: _parse_numeric_array(source, name) for name in spec["arrays"]}
        for variable in spec["arrays"]:
            assignment_count = len(re.findall(rf"(?im)^\s*{re.escape(variable)}\s*=", source))
            expected_count = 2 if curve_id == "dna_abs" and variable == "data0" else 1
            _require(assignment_count == expected_count,
                     f"{curve_id}: unsupported or duplicate assignment to {variable} ({assignment_count} assignments)")

    if curve_id == "palmer_williams_real_ior":
        start, stop, step = spec["grid_nm"]
        wavelengths = [float(w) for w in range(start, stop + 1, step)]
        values = arrays["data"]
        _require(len(wavelengths) == len(values),
                 f"water1.m has {len(values)} values but declared 400–2500 nm/5 nm grid has {len(wavelengths)}")
        _require(all(value > 0.0 for value in values), "water refractive-index samples must be positive")
    elif "matrix" not in spec:
        wavelength_name, value_name = spec["arrays"]
        wavelengths, values = arrays[wavelength_name], arrays[value_name]
        _require(len(wavelengths) == len(values), f"{curve_id}: wavelength/value array lengths differ")
    _require(all(math.isfinite(w) and w > 0.0 for w in wavelengths),
             f"{curve_id}: wavelengths must be positive and finite")
    _require(all(b > a for a, b in zip(wavelengths, wavelengths[1:])),
             f"{curve_id}: wavelengths must be strictly increasing")
    _require(all(value >= 0.0 for value in values), f"{curve_id}: source samples must be nonnegative")
    if curve_id == "dna_abs":
        # This exact operation is the only supported non-literal assignment to data0.
        normalizer = re.compile(r"(?im)^\s*data0\s*=\s*data0\s*/\s*data0\s*\(\s*12\s*\)\s*;\s*%[^\n]*$")
        _require(len(normalizer.findall(source)) == 1,
                 "DNA source must contain exactly the documented data0=data0/data0(12) normalization")
        _require(len(values) >= 12, "DNA normalization requires at least 12 data samples")
        divisor = values[11]
        _require(divisor > 0.0, "DNA point-12 normalization divisor must be positive")
        values = [value / divisor for value in values]
        _require(all(math.isfinite(value) for value in values), "DNA normalization produced non-finite absorbance")

    result: dict[str, Any] = {
        "schema_version": 1,
        "curve_id": curve_id,
        "source_quantity": spec["quantity"],
        "source_unit": spec["unit"],
        "reference_basis": spec["reference_basis"],
        "wavelength_nm": wavelengths,
        "values": values,
        "provenance": {
            "index_url": "https://www.npsg.uwaterloo.ca/data/skin.php" if curve_id != "palmer_williams_real_ior" else "https://www.npsg.uwaterloo.ca/data/water.php",
            "source_files": hashes,
            "download_urls": dict(zip(entry["files"], entry.get("download_urls", []))),
            "source_citation": entry.get("source", "See source data index and pinned MATLAB header."),
            "importer": "tools/hylios_import_data.py; safe literal parser, MATLAB not executed",
            "coverage_nm": [wavelengths[0], wavelengths[-1]],
            "wavelength_grid_irregular": any(not math.isclose(b - a, wavelengths[1] - wavelengths[0], rel_tol=1e-9, abs_tol=1e-9)
                                              for a, b in zip(wavelengths[1:], wavelengths[2:])) if len(wavelengths) > 2 else False,
            "maximum_adjacent_interval_nm": max(b - a for a, b in zip(wavelengths, wavelengths[1:])),
            "tail_policy": "none; no extrapolation or zero-tail inference",
            "source_text_decoding": "ISO-8859-1 byte mapping for inspection; no MATLAB execution",
        },
    }
    if "path_m" in spec:
        coefficient = [LN10 * value / spec["path_m"] for value in values]
        _require(all(math.isfinite(x) and x >= 0.0 for x in coefficient), f"{curve_id}: invalid converted coefficient")
        result["reference_absorption_coefficient"] = {
            "quantity": "Napierian absorption coefficient", "unit": "m^-1", "values": coefficient,
            "conversion": spec["transform"], "path_m": spec["path_m"],
            "concentration_scaling_applied": False,
            "note": "Reference-sample coefficient only; not a resolved tissue-mixture coefficient.",
        }
    elif curve_id in {"water_absorption", "lipid_absorption"}:
        coefficient = [100.0 * value for value in values]
        _require(all(math.isfinite(x) and x >= 0.0 for x in coefficient), f"{curve_id}: invalid SI coefficient")
        result["si_absorption_coefficient"] = {
            "quantity": "Napierian absorption coefficient", "unit": "m^-1", "values": coefficient,
            "conversion": spec["transform"],
        }
    elif "extinction_coefficient" in spec["quantity"]:
        result["note"] = "Extinction data are retained in the source basis; no ln(10), molar-mass, concentration, or tissue-mixture conversion is applied."
    else:
        result["note"] = "Real refractive index values are retained on the source-provided grid; no absorption conversion or extrapolation is applied."
    return result


def run(source_dir: Path, output_dir: Path, selected: list[str]) -> list[Path]:
    manifest = json.loads(MANIFEST_PATH.read_text())
    entries = _entry_map(manifest)
    prepared: list[tuple[str, dict[str, Any]]] = []
    for curve_id in selected:
        _require(curve_id in CURVE_SPECS, f"unsupported dataset {curve_id!r}; supported: {', '.join(CURVE_SPECS)}")
        manifest_id = CURVE_SPECS[curve_id].get("manifest_id", curve_id)
        _require(manifest_id in entries, f"supported dataset {curve_id!r} (manifest id {manifest_id!r}) is absent from the source manifest")
        result = import_curve(curve_id, entries[manifest_id], source_dir)
        # Validate serialization before creating or modifying any outputs.
        json.dumps(result, indent=2, sort_keys=True, ensure_ascii=False, allow_nan=False)
        prepared.append((curve_id, result))
    output_dir.mkdir(parents=True, exist_ok=True)
    written = []
    for curve_id, result in prepared:
        destination = output_dir / f"{curve_id}.json"
        temporary = destination.with_suffix(destination.suffix + ".tmp")
        temporary.write_text(json.dumps(result, indent=2, sort_keys=True, ensure_ascii=False, allow_nan=False) + "\n")
        temporary.replace(destination)
        written.append(destination)
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, required=True, help="cached author data root matching manifest paths")
    parser.add_argument("--output", type=Path, required=True, help="directory for generated checked JSON curves")
    parser.add_argument("--curve", action="append", choices=sorted(CURVE_SPECS), help="curve to import (repeatable; default: all supported)")
    args = parser.parse_args()
    try:
        for path in run(args.source_dir, args.output, args.curve or list(CURVE_SPECS)):
            print(f"Wrote {path}")
    except (OSError, json.JSONDecodeError, KeyError, ImportErrorDetail) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
