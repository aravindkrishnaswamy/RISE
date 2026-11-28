#!/usr/bin/env python3
"""Resolve source-table HyLIoS specimens using an explicitly named clean-room contract.

No renderer integration, downloads, implicit spectral tails, or BioSpec changes.
See docs/HYLIOS_COMPOSITION.md for source evidence and interpretation limits.
"""
from __future__ import annotations

import argparse
import bisect
import copy
import hashlib
import json
import math
import os
import sys
import tempfile
from pathlib import Path
from typing import Any

try:
    from .hylios_import_data import CURVE_SPECS, MANIFEST_PATH, _entry_map
except ImportError:
    from hylios_import_data import CURVE_SPECS, MANIFEST_PATH, _entry_map

ROOT = Path(__file__).resolve().parents[1]
PRESETS_PATH = ROOT / "data/hylios/presets_source.json"
COMPOSITION_REVISION = "hylios_cleanroom_composition_v1"
LN10 = math.log(10.0)
CONVENTIONS = [
    "keratin_inverse_reference_path_v1", "dna_reference_concentration_ratio_v1",
    "uca_reference_concentration_ratio_v1", "prahl_molecular_64500_v1",
    "npsg_dyshemoglobin_literal_molar_66500_v1", "whole_blood_total_pool_v1",
    "viable_epidermal_dna_v1", "colloid_shared_melanin_concentration_v1",
    "independent_effective_loadings_v1", "full_diameter_melanosome_v1",
    "low_pigmentation_complex_v1", "water_anchor_1300_v1",
]
LAYER_ORDER = ["stratum_corneum", "stratum_granulosum", "stratum_spinosum",
               "stratum_basale", "papillary_dermis", "reticular_dermis"]


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _number(value: Any, name: str, *, positive: bool = False,
            fraction: bool = False) -> float:
    _require(isinstance(value, (int, float)) and not isinstance(value, bool)
             and math.isfinite(value), f"{name}: expected finite number")
    _require(value > 0 if positive else value >= 0, f"{name}: invalid negative/zero value")
    _require(not fraction or value <= 1, f"{name}: fraction must be within [0,1]")
    return float(value)


def _digest(value: Any) -> str:
    payload = json.dumps(value, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=False, allow_nan=False).encode()
    return hashlib.sha256(payload).hexdigest()


def _product(name: str, *factors: float) -> float:
    """Keep true zero inputs distinct from positive out-of-range products."""
    for factor in factors:
        _number(factor, name)
    if any(factor == 0 for factor in factors):
        return 0.0
    result = math.prod(factors)
    _require(math.isfinite(result) and result > 0,
             f"{name}: positive conversion underflow/overflow; outside supported numeric range")
    return result


def _spheroid_surface_volume(a: float, b: float) -> float:
    _number(a, "semi-minor metres", positive=True)
    _number(b, "semi-major metres", positive=True)
    _require(a <= b, "major axis below minor axis")
    r = a/b
    _require(r > 0, "axis ratio: positive conversion underflow; outside supported numeric range")
    e = math.sqrt((1-r)*(1+r))
    angular = math.atan2(e,r)/e if e else 1.0
    return _product("surface/volume ratio", 1.5/a, r+angular)


def write_specimen(result: dict[str, Any], output: Path) -> None:
    """Serialize first, then atomically replace via a unique sibling temporary."""
    payload = json.dumps(result,indent=2,sort_keys=True,allow_nan=False)+"\n"
    output.parent.mkdir(parents=True,exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=output.parent,
                                         prefix=f".{output.name}.",suffix=".tmp",delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(payload)
        os.replace(temporary,output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


class StrictImportedCurves:
    """Copied raw imported arrays, checked bases and strict interpolation coverage.

    JSON content is addressed by its own digest. Source hash metadata is not
    evidence of re-hashing the original downloaded source bytes; use the pinned
    importer on those bytes to produce these records when that guarantee is needed.
    """

    def __init__(self, curves: dict[str, Any], *, manifest: dict[str, Any] | None = None):
        self._curves = copy.deepcopy(curves)
        entries = _entry_map(manifest) if manifest is not None else None
        for curve_id, curve in self._curves.items():
            _require(curve_id in CURVE_SPECS, f"unknown curve {curve_id}")
            spec = CURVE_SPECS[curve_id]
            _require(curve.get("schema_version") == 1 and curve.get("curve_id") == curve_id,
                     f"{curve_id}: invalid imported identity/schema")
            _require(curve.get("source_quantity") == spec["quantity"]
                     and curve.get("source_unit") == spec["unit"]
                     and curve.get("reference_basis") == spec["reference_basis"],
                     f"{curve_id}: source basis mismatch")
            wave, values = curve.get("wavelength_nm"), curve.get("values")
            _require(isinstance(wave, list) and isinstance(values, list)
                     and len(wave) == len(values) and len(wave) >= 2,
                     f"{curve_id}: aligned arrays with >=2 samples required")
            for value in wave:
                _number(value, f"{curve_id} wavelength", positive=True)
            _require(all(b > a for a, b in zip(wave, wave[1:])),
                     f"{curve_id}: wavelengths must increase")
            for value in values:
                _number(value, f"{curve_id} raw value", positive=curve_id == "palmer_williams_real_ior")
            files = curve.get("provenance", {}).get("source_files")
            _require(isinstance(files, dict) and bool(files), f"{curve_id}: source_files required")
            _require(all(isinstance(path, str) and isinstance(sha, str) and len(sha) == 64
                         and all(c in "0123456789abcdef" for c in sha) for path, sha in files.items()),
                     f"{curve_id}: invalid source SHA256 metadata")
            if entries is not None:
                entry = entries[spec.get("manifest_id", curve_id)]
                _require(files == dict(zip(entry["files"], entry["sha256"])),
                         f"{curve_id}: manifest source identity mismatch")
        self._manifest_sha = _digest(manifest) if manifest is not None else None

    @classmethod
    def from_directory(cls, directory: Path) -> StrictImportedCurves:
        curves = {}
        for curve_id in CURVE_SPECS:
            path = directory / f"{curve_id}.json"
            if path.is_file():
                curves[curve_id] = json.loads(path.read_text())
        _require(bool(curves), "no supported imported curves found")
        return cls(curves, manifest=json.loads(MANIFEST_PATH.read_text()))

    def provenance(self) -> dict[str, Any]:
        return {"profile_id": "strict_source_coverage_v1", "approximate": False,
                "bundle_sha256": _digest(self._curves),
                "source_verification": "Imported JSON content addressed; source hash metadata checked when manifest supplied; original source bytes not reverified here.",
                "source_manifest_sha256": self._manifest_sha,
                "curves": {key: {"source_files": value["provenance"]["source_files"],
                                  "coverage_nm": [value["wavelength_nm"][0], value["wavelength_nm"][-1]],
                                  "source_quantity": value["source_quantity"], "source_unit": value["source_unit"],
                                  "reference_basis": value["reference_basis"]}
                           for key, value in self._curves.items()}}

    def evaluate_with_provenance(self, curve_id: str, wavelength_nm: float) -> dict[str, Any]:
        wavelength_nm = _number(wavelength_nm, "wavelength_nm", positive=True)
        _require(curve_id in self._curves, f"missing active curve {curve_id}")
        curve = self._curves[curve_id]
        wave, values = curve["wavelength_nm"], curve["values"]
        _require(wave[0] <= wavelength_nm <= wave[-1],
                 f"uncovered active curve {curve_id} at {wavelength_nm} nm; no implicit tail")
        i = bisect.bisect_left(wave, wavelength_nm)
        if wave[i] == wavelength_nm:
            value = values[i]
        else:
            t = (wavelength_nm - wave[i-1]) / (wave[i] - wave[i-1])
            low, high = min(values[i-1],values[i]), max(values[i-1],values[i])
            f = t if values[i] >= values[i-1] else 1-t
            increment = _product(f"{curve_id} interpolation",high-low,f) if low == 0 else (high-low)*f
            value = low+increment
        return {"profile_id": "strict_source_coverage_v1", "approximate": False,
                "curve_id": curve_id, "wavelength_nm": wavelength_nm, "value": value,
                "source_quantity": curve["source_quantity"], "source_unit": curve["source_unit"],
                "reference_basis": curve["reference_basis"],
                "source_files": curve["provenance"]["source_files"],
                "evaluation_trace": [{"curve_id": curve_id, "origin": "source_interpolation"}]}


def compose_specimen(evaluator: Any, preset_id: str, wavelengths_nm: list[float], *,
                     presets: dict[str, Any] | None = None) -> dict[str, Any]:
    """Resolve the named specimen; evaluator must return raw source-basis quantities.

    The optional source-table document supports explicitly content-addressed
    experimental inputs; its complete hash and selected values are recorded.
    It never changes the clean-room formula conventions or the source tables on disk.
    """
    canonical_tables = json.loads(PRESETS_PATH.read_text())
    source = copy.deepcopy(presets if presets is not None else canonical_tables)
    _require(source.get("schema_version") == 1, "unsupported source table schema")
    _require(preset_id in {"S1", "S2", "S3", "S4"}, "preset must be S1, S2, S3 or S4")
    _require(source.get("layer_order_outer_to_inner") == LAYER_ORDER, "layer order mismatch")
    _require(isinstance(wavelengths_nm, list) and bool(wavelengths_nm), "wavelength list must be nonempty")
    waves = [_number(w, "wavelength_nm", positive=True) for w in wavelengths_nm]
    _require(all(b > a for a, b in zip(waves, waves[1:])), "wavelengths must be strictly increasing")
    _require(callable(getattr(evaluator, "provenance", None)), "evaluator requires public provenance()")
    bundle_identity = evaluator.provenance()
    _require(isinstance(bundle_identity.get("bundle_sha256"), str)
             and len(bundle_identity["bundle_sha256"]) == 64, "evaluator requires content-addressed bundle")
    _require(type(bundle_identity.get("approximate")) is bool,
             "evaluator requires explicit approximate boolean")
    _require(isinstance(bundle_identity.get("profile_id"),str) and bool(bundle_identity["profile_id"]),
             "evaluator requires coverage profile identity")
    raw_evaluations = {}

    def raw(curve_id: str, wavelength: float) -> float:
        key = (curve_id, wavelength)
        if key not in raw_evaluations:
            result = evaluator.evaluate_with_provenance(curve_id, wavelength)
            spec = CURVE_SPECS[curve_id]
            _require(result.get("curve_id") == curve_id and result.get("wavelength_nm") == wavelength,
                     "evaluator query identity mismatch")
            _require(result.get("source_quantity") == spec["quantity"]
                     and result.get("source_unit") == spec["unit"]
                     and result.get("reference_basis") == spec["reference_basis"],
                     f"{curve_id}: evaluator source basis mismatch")
            _number(result.get("value"), f"{curve_id} evaluation", positive=curve_id == "palmer_williams_real_ior")
            raw_evaluations[key] = copy.deepcopy(result)
        return raw_evaluations[key]["value"]

    fields, fixed = source["table_iii_fields"], source["table_iv_fixed_source_values"]
    for group in ("table_iii_fields", "table_iv_fixed_source_values"):
        _require(set(source[group]) == set(canonical_tables[group]), f"{group}: field set mismatch")
        for name, specification in canonical_tables[group].items():
            _require(source[group][name].get("unit") == specification["unit"],
                     f"{name}: source unit mismatch")
            if "layer_order" in specification:
                _require(source[group][name].get("layer_order") == specification["layer_order"],
                         f"{name}: source layer order mismatch")
    selected = {key: value["values"][preset_id] for key, value in fields.items()}

    def scalar(key: str) -> float:
        return _number(fixed[key]["value"], key)

    def fraction(value: Any, name: str) -> float:
        return _number(_product(name,_number(value,name),.01), name, fraction=True)

    thickness = selected["thickness_cm_by_layer"]
    organs = selected["melanosome_content_percent_by_epidermal_layer"]
    colloids = selected["colloidal_melanin_content_percent_by_epidermal_layer"]
    blood = selected["blood_content_percent_by_dermis_layer"]
    dimensions = selected["melanosome_dimensions_um"]
    for vector, count, name in [(thickness,6,"thickness"),(organs,3,"organelle"),
                                (colloids,3,"colloid"),(blood,2,"blood"),(dimensions,2,"dimensions")]:
        _require(isinstance(vector, list) and len(vector) == count, f"{name}: wrong number of layers/components")
    fold = _number(selected["surface_fold_aspect_ratio"], "surface fold aspect ratio", positive=True)
    oxygenation = fraction(selected["oxygenated_blood_fraction_percent"], "oxygenation")
    concentrations = {"total": scalar("hemoglobin_concentration_whole_blood_mg_per_ml"),
                      "met": scalar("methemoglobin_concentration_mg_per_ml"),
                      "co": scalar("carboxyhemoglobin_concentration_mg_per_ml"),
                      "sulf": scalar("sulfhemoglobin_concentration_mg_per_ml")}
    functional = concentrations["total"] - concentrations["met"] - concentrations["co"] - concentrations["sulf"]
    _require(functional >= 0, "total Hb below dysfunctional Hb sum")
    concentrations.update({"functional":functional,
                           "oxy":_product("oxy concentration",functional,oxygenation),
                           "deoxy":_product("deoxy concentration",functional,1-oxygenation)})
    c_e = _number(selected["melanosome_eumelanin_concentration_mg_per_ml"], "eumelanin concentration")
    c_p = _number(selected["melanosome_pheomelanin_concentration_mg_per_ml"], "pheomelanin concentration")
    major, minor = [_product("full melanosome diameter metres",
                            _number(v,"full melanosome diameter",positive=True),1e-6) for v in dimensions]
    _require(major >= minor, "major diameter below minor diameter")
    mel_ior = _number(fixed["melanin_refractive_index"]["value"], "melanin index", positive=True)
    organization = source["organization_by_preset"][preset_id]
    _require(organization in {"complexes", "individually_dispersed_particles"}, "unsupported organelle organization")

    def converted(curve_id: str, wave: float, loading: float, factor: float) -> float:
        if loading == 0:
            return 0.0
        return _product(f"{curve_id} coefficient",raw(curve_id,wave),loading,factor)

    layers = []
    for i, layer_id in enumerate(LAYER_ORDER):
        group = "stratum_corneum" if i == 0 else "epidermis" if i < 4 else layer_id
        water = fraction(fixed["water_content_percent"]["values"][group], "water")
        lipid = fraction(fixed["lipid_content_percent"]["values"][group], "lipid")
        anchor = _number(fixed["skin_layer_refractive_index_at_1300_nm"]["values"][group], "index anchor", positive=True)
        nw_anchor = raw("palmer_williams_real_ior", 1300.0) if water else 0.0
        if water == 1:
            _require(anchor == nw_anchor, "pure-water loading requires matching water index anchor")
        else:
            _number((anchor-water*nw_anchor)/(1-water), "dry matrix index", positive=True)
        particle = None
        f_colloid = fraction(colloids[i-1], "colloid") if 0 < i < 4 else 0.0
        if 0 < i < 4:
            f_organ = fraction(organs[i-1], "organelle")
            scale = [.25,.5,1.0][i-1]
            a = _product("semi-minor metres",minor,scale,.5)
            b = _product("semi-major metres",major,scale,.5)
            ratio = _spheroid_surface_volume(a,b)
            complex_diameter = _product("complex diameter metres",4,b) if organization == "complexes" else None
            rate = _product("organelle encounter coefficient",
                            6/complex_diameter if complex_diameter is not None else ratio,f_organ,.25)
            particle = {"enabled":f_organ > 0, "organization":organization,
                        "organelle_fraction":f_organ, "semi_minor_m":a, "semi_major_m":b,
                        "layer_size_scale":scale, "encounter_rate_m_inv":rate,
                        "complex_diameter_m":complex_diameter,
                        "maximum_internal_count":max(1,math.floor(complex_diameter/(2*a)+.5)) if complex_diameter else 1,
                        "real_refractive_index":mel_ior, "interior_absorption_evaluated":f_organ > 0,
                        "interior_absorption_m_inv":[]}
        contributions: dict[str,list[float]] = {}
        indices = []
        for wave in waves:
            terms = {"water":converted("water_absorption",wave,water,100),
                     "lipid":converted("lipid_absorption",wave,lipid,100)}
            mel = 0.0
            if f_colloid or (particle and particle["enabled"]):
                mel = (converted("eumelanin_extinction",wave,c_e,100*LN10)
                       + converted("pheomelanin_extinction",wave,c_p,100*LN10))
            if particle:
                particle["interior_absorption_m_inv"].append(mel if particle["enabled"] else 0.0)
            if i == 0:
                terms.update({"keratin":converted("keratin_abs",wave,fraction(scalar("stratum_corneum_keratin_content_percent"),"keratin"),LN10/4e-6),
                              "urocanic_acid":converted("uro_acid_abs",wave,scalar("stratum_corneum_urocanic_acid_density_mol_per_l"),LN10/.01/15e-6),
                              "beta_carotene":converted("beta_carotene",wave,_number(fixed["beta_carotene_concentration_mg_per_ml"]["values"][group],"beta-carotene concentration"),100*LN10/537)})
            elif i < 4:
                terms.update({"dna":converted("dna_abs",wave,scalar("skin_dna_density_mg_per_ml"),LN10/.01/.05),
                              "beta_carotene":converted("beta_carotene",wave,_number(fixed["beta_carotene_concentration_mg_per_ml"]["values"][group],"beta-carotene concentration"),100*LN10/537),
                              "colloidal_melanin":_product("colloidal coefficient",f_colloid,mel)})
            else:
                vb = fraction(blood[i-4],"blood")
                for name, curve, mass in [("oxy","oxy_hemoglobin_extinction",64500),
                                          ("deoxy","deoxy_hemoglobin_extinction",64500),
                                          ("met","methemoglobin",66500),
                                          ("co","carboxyhemoglobin",66500),
                                          ("sulf","sulfhemoglobin",66500)]:
                    terms["blood_"+name] = converted(curve,wave,_product("blood "+name+" loading",vb,concentrations[name]),100*LN10/mass)
                terms["blood_beta_carotene"] = converted("beta_carotene",wave,_product("blood beta-carotene loading",vb,_number(fixed["beta_carotene_concentration_mg_per_ml"]["values"]["blood"],"blood beta carotene")),100*LN10/537)
                terms["blood_bilirubin"] = converted("bilirubin",wave,_product("blood bilirubin loading",vb,scalar("whole_blood_bilirubin_concentration_mg_per_ml")),100*LN10/585)
            for key,value in terms.items():
                contributions.setdefault(key,[]).append(value)
            index = anchor + water*(raw("palmer_williams_real_ior",wave)-nw_anchor) if water else anchor
            indices.append(_number(index,"layer real index",positive=True))
        total = [_number(math.fsum(values),"host absorption") for values in zip(*contributions.values())]
        layers.append({"layer_id":layer_id,"thickness_m":_product("thickness metres",_number(thickness[i],"thickness",positive=True),.01),
                       "water_loading":water,"lipid_loading":lipid,"colloidal_loading":f_colloid,
                       "index_anchor_1300_nm":anchor,"real_refractive_index":indices,
                       "constituent_absorption_m_inv":contributions,"host_absorption_m_inv":total,
                       "particles":particle})
    result = {"schema_version":1,"composition_revision":COMPOSITION_REVISION,
              "approximate":bundle_identity["approximate"],
              "coverage_profile_id":bundle_identity["profile_id"],
              "spectral_bundle_sha256":bundle_identity["bundle_sha256"],
              "interpretation":"Source-informed clean-room model; not author-equivalent or physiologically validated.",
              "preset_id":preset_id,"wavelength_nm":waves,"units":{"length":"m","coefficient":"m^-1","wavelength":"nm","mass_concentration":"kg/m^3"},
              "surface_fold_aspect_ratio":fold,"oxygenation":oxygenation,
              "blood_concentrations_kg_per_m3":concentrations,"layers":layers,
              "provenance":{"conventions":CONVENTIONS.copy(),"composition_document":"docs/HYLIOS_COMPOSITION.md",
                            "source_table_sha256":_digest(source),"source_tables":source["sources"],
                            "source_table_is_checked_in_transcription":source == canonical_tables,
                            "selected_table_iii_source_inputs":selected,"table_iv_source_inputs":fixed,
                            "spectral_bundle":bundle_identity,
                            "unevaluated_curve_ids":sorted(set(CURVE_SPECS)-{key[0] for key in raw_evaluations}),
                            "raw_evaluations":[value for _,value in sorted(raw_evaluations.items())]}}
    # Reject overflow even in geometric/intermediate output and guarantee JSON safety.
    json.dumps(result, allow_nan=False)
    result["resolved_specimen_sha256"] = _digest(result)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--strict-curves-dir", type=Path)
    inputs.add_argument("--approximation-bundle", type=Path, help="Explicit opt-in to the named approximation bundle")
    parser.add_argument("--preset", choices=["S1","S2","S3","S4"], required=True)
    parser.add_argument("--wavelength-nm", type=float, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.strict_curves_dir:
            evaluator = StrictImportedCurves.from_directory(args.strict_curves_dir)
        else:
            try:
                from .hylios_approximation_profile import ApproximationProfile
            except ImportError:
                from hylios_approximation_profile import ApproximationProfile
            evaluator = ApproximationProfile(json.loads(args.approximation_bundle.read_text()))
        result = compose_specimen(evaluator,args.preset,args.wavelength_nm)
        write_specimen(result,args.output)
        print(f"Wrote {args.output} ({COMPOSITION_REVISION}; {result['provenance']['spectral_bundle']['profile_id']})")
    except (OSError,ValueError,KeyError,TypeError,OverflowError) as exc:
        print(f"ERROR: {exc}",file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
