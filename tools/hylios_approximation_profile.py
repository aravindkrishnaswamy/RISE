#!/usr/bin/env python3
"""Build/query an explicitly approximate 250–2500 nm HyLIoS spectral bundle.

No downloads, MATLAB execution, concentration rules or strict-reference changes.
The bundle preserves complete imported curves and source bases; extrapolation is
evaluated on demand, never disguised as a resampled measured spectrum.
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
    from .hylios_import_data import (CURVE_SPECS, MANIFEST_PATH, ImportErrorDetail,
                                    _entry_map, _load_pinned_files,
                                    _parse_numeric_array, _require, import_curve)
except ImportError:
    from hylios_import_data import (CURVE_SPECS, MANIFEST_PATH, ImportErrorDetail,
                                   _entry_map, _load_pinned_files,
                                   _parse_numeric_array, _require, import_curve)

PROFILE_PATH = Path(__file__).resolve().parents[1] / "data/hylios/approximation_profile_v1.json"


def bundle_fingerprint(bundle: dict[str, Any]) -> str:
    """SHA-256 of canonical finite JSON, excluding only bundle_sha256 itself.

    This detects accidental edits of generated arrays/policies. It is distinct
    from source_files digests of original downloaded bytes and cannot establish
    source authenticity if someone deliberately recomputes it after an edit.
    """
    _require(isinstance(bundle, dict), "bundle must be a JSON object")
    try:
        scientific = {key: value for key, value in bundle.items() if key != "bundle_sha256"}
        payload = json.dumps(scientific, sort_keys=True, ensure_ascii=False,
                             separators=(",", ":"), allow_nan=False).encode("utf-8")
    except (ValueError, TypeError, OverflowError) as exc:
        raise ImportErrorDetail("bundle must contain canonical finite JSON content") from exc
    return hashlib.sha256(payload).hexdigest()


def _finite(value: Any) -> bool:
    return isinstance(value, (float, int)) and not isinstance(value, bool) and math.isfinite(value)


def _validate_curve(curve: dict[str, Any]) -> None:
    _require(isinstance(curve, dict), "curve must be a JSON object")
    wave, values = curve.get("wavelength_nm"), curve.get("values")
    _require(isinstance(wave, list) and isinstance(values, list) and len(wave) == len(values)
             and len(wave) >= 2, "curve requires aligned wavelength/value arrays with >=2 samples")
    _require(all(_finite(w) and w > 0 for w in wave), "invalid curve wavelength")
    _require(all(b > a for a, b in zip(wave, wave[1:])), "curve wavelengths must be strictly increasing")
    _require(all(_finite(v) and v >= 0 for v in values), "curve values must be finite and nonnegative")
    if curve.get("source_quantity") == "real_refractive_index":
        _require(all(v > 0 for v in values), "real refractive index must be positive")


def _interpolate(curve: dict[str, Any], wavelength_nm: float) -> float:
    wave, values = curve["wavelength_nm"], curve["values"]
    _require(wave[0] <= wavelength_nm <= wave[-1], "interpolation outside source support")
    index = bisect.bisect_left(wave, wavelength_nm)
    if wave[index] == wavelength_nm:
        return values[index]
    fraction = (wavelength_nm - wave[index - 1]) / (wave[index] - wave[index - 1])
    return values[index - 1] * (1.0 - fraction) + values[index] * fraction


def _import_substitution(source_dir: Path, source_id: str, spec: dict[str, Any],
                         entry: dict[str, Any]) -> dict[str, Any]:
    payloads, hashes = _load_pinned_files(source_dir, entry)
    _require(hashes == spec["source_files"], f"profile/manifest source identity mismatch for {source_id}")
    scripts = [name for name in payloads if name.endswith(".m")]
    _require(len(scripts) == 1, f"expected one literal source script for {source_id}")
    text = payloads[scripts[0]].decode("latin-1")
    # The pinned files are not executed. Only their single literal data RHS is read.
    values = _parse_numeric_array(text, "data")
    start, stop, step = spec["grid_nm"]
    wave = list(range(start, stop + 1, step))
    _require(len(wave) == len(values), f"{source_id}: wrong sample count for recorded grid")
    curve = {"schema_version": 1, "curve_id": source_id,
             "source_quantity": spec["source_quantity"], "source_unit": spec["source_unit"],
             "reference_basis": "Hale-Querry water optical constant, author-interpolated table",
             "wavelength_nm": wave, "values": values,
             "provenance": {"source_files": hashes, "coverage_nm": [wave[0], wave[-1]],
                            "index_url": spec["source_url"], "source_citation": spec["source_citation"],
                            "note": spec["note"]}}
    _validate_curve(curve)
    return curve


def build_profile(source_dir: Path, profile_path: Path = PROFILE_PATH) -> dict[str, Any]:
    """Import checksummed inputs before returning an approximation bundle.

    This performs no writes. Both regular and substitution identities must match
    the checked-in profile and manifest, so this cannot silently follow a changed
    source manifest. Imported raw/reference-sample quantities remain unchanged.
    """
    profile = json.loads(profile_path.read_text())
    entries = _entry_map(json.loads(MANIFEST_PATH.read_text()))
    _require(set(profile["curves"]) == set(CURVE_SPECS), "profile must specify every supported curve")
    curves = {}
    for curve_id, policy in profile["curves"].items():
        curve = import_curve(curve_id, entries[policy["source_manifest_id"]], source_dir)
        _require(curve["provenance"]["source_files"] == policy["source_files"],
                 f"profile/manifest source identity mismatch for {curve_id}")
        _require(curve["source_unit"] == policy["source_unit"]
                 and curve["source_quantity"] == policy["source_quantity"]
                 and curve["reference_basis"] == policy["reference_basis"],
                 f"profile source basis mismatch for {curve_id}")
        curves[curve_id] = curve
    substitutions = {key: _import_substitution(source_dir, key, spec, entries[key])
                     for key, spec in profile["substitution_sources"].items()}
    bundle = {"schema_version": 1, "approximate": True, "profile_id": profile["profile_id"],
              "profile": profile, "curves": curves, "substitution_curves": substitutions}
    bundle["bundle_sha256"] = bundle_fingerprint(bundle)
    ApproximationProfile(bundle)  # Validate all policies before any output is created.
    return bundle


class ApproximationProfile:
    """Validated, privately copied evaluation state in the original source basis.

    Loaded bundles are structurally checked; verifying their physical source
    files requires build_profile(source_dir), which recomputes pinned hashes.
    """

    def __init__(self, bundle: dict[str, Any]):
        _require(isinstance(bundle, dict), "bundle must be a JSON object")
        self._bundle = copy.deepcopy(bundle)
        _require(bundle.get("bundle_sha256") == bundle_fingerprint(self._bundle),
                 "bundle content fingerprint mismatch or missing bundle_sha256")
        profile = self._bundle["profile"]
        _require(isinstance(profile, dict), "profile must be a JSON object")
        _require(bundle.get("schema_version") == 1 and profile.get("schema_version") == 1,
                 "unsupported approximation schema")
        _require(bundle.get("approximate") is True and profile.get("approximate") is True,
                 "approximation bundle must be explicitly marked approximate=true")
        _require(isinstance(profile.get("profile_id"), str) and bool(profile["profile_id"])
                 and bundle.get("profile_id") == profile["profile_id"], "profile identity mismatch")
        _require(profile.get("domain_nm") == [250.0, 2500.0], "unsupported approximation domain")
        self._curves = self._bundle["curves"]
        self._substitutions = self._bundle["substitution_curves"]
        self._policies = profile["curves"]
        _require(all(isinstance(value, dict) for value in
                     (self._curves, self._substitutions, self._policies, profile["substitution_sources"])),
                 "curve and policy inventories must be JSON objects")
        _require(set(self._curves) == set(self._policies) == set(CURVE_SPECS),
                 "bundle must contain all supported curves and policies")
        _require(set(self._substitutions) == set(profile["substitution_sources"]),
                 "substitution curve set mismatch")
        for curve_id, curve in {**self._curves, **self._substitutions}.items():
            _validate_curve(curve)
            spec = self._policies.get(curve_id, profile["substitution_sources"].get(curve_id))
            _require(curve.get("curve_id") == curve_id and curve.get("source_quantity") == spec["source_quantity"]
                     and curve.get("source_unit") == spec["source_unit"], f"source basis mismatch for {curve_id}")
            _require(curve.get("provenance", {}).get("source_files") == spec["source_files"],
                     f"source identity mismatch for {curve_id}")
        for curve_id, policy in self._policies.items():
            for side in ("below", "above"):
                self._validate_policy(curve_id, side, policy[side])
        # Prove configured support/cycle validity throughout all dependency paths,
        # including branches that the target domain itself does not exercise.
        for curve_id in self._curves:
            self._check_dependencies(curve_id, ())
            for wavelength in (250.0, 2500.0):
                self.evaluate(curve_id, wavelength)

    @property
    def profile_id(self) -> str:
        return self._bundle["profile_id"]

    @property
    def bundle_sha256(self) -> str:
        return self._bundle["bundle_sha256"]

    def provenance(self) -> dict[str, Any]:
        """Return detached scientific identity and source coverage metadata.

        Original-byte verification occurs during build_profile. A loaded bundle
        checks only generated-content integrity plus structural/numerical rules.
        """
        sources = {}
        for curve_id, curve in {**self._curves, **self._substitutions}.items():
            sources[curve_id] = {"source_files": copy.deepcopy(curve["provenance"]["source_files"]),
                                 "coverage_nm": [curve["wavelength_nm"][0], curve["wavelength_nm"][-1]],
                                 "source_quantity": curve["source_quantity"],
                                 "source_unit": curve["source_unit"],
                                 "reference_basis": curve["reference_basis"]}
        return {"profile_id": self.profile_id, "bundle_sha256": self.bundle_sha256,
                "approximate": True, "domain_nm": [250.0, 2500.0], "sources": sources,
                "tail_policies": copy.deepcopy(self._policies),
                "validation_status": self._bundle["profile"]["validation_status"],
                "source_verification": "Original pinned source-byte hashes are recomputed only by build_profile(source_dir); loading a bundle verifies generated-content fingerprint and structure, not original bytes."}

    def _validate_policy(self, curve_id: str, side: str, rule: dict[str, Any]) -> None:
        _require(isinstance(rule, dict), "tail policy must be a JSON object")
        kind = rule.get("kind")
        _require(kind in {"zero", "endpoint_hold", "exponential", "power_law", "scaled_proxy",
                          "water_k_substitution", "real_index_substitution", "reject"}, "unknown tail policy")
        _require(isinstance(rule.get("rationale"), str) and bool(rule["rationale"]), "tail policy requires rationale")
        if kind == "exponential":
            _require(_finite(rule.get("e_fold_nm")) and rule["e_fold_nm"] > 0, "invalid exponential scale")
        if kind == "power_law":
            _require(_finite(rule.get("exponent")) and rule["exponent"] < 0, "invalid power-law exponent")
        if kind == "scaled_proxy":
            proxy = self._curves.get(rule.get("proxy_curve_id"))
            _require(proxy is not None and proxy["source_unit"] == self._curves[curve_id]["source_unit"]
                     and proxy["source_quantity"] == self._curves[curve_id]["source_quantity"],
                     "proxy must exist and have the same source basis")
        if kind in {"water_k_substitution", "real_index_substitution"}:
            sub = self._substitutions.get(rule.get("substitution_curve_id"))
            _require(side == "below" and sub is not None, "invalid substitution curve/side")
            expected = "imaginary_refractive_index" if kind == "water_k_substitution" else "real_refractive_index"
            _require(sub["source_quantity"] == expected and sub["source_unit"] == "dimensionless",
                     "wrong water substitution quantity/basis")
            target = self._curves[curve_id]
            _require(sub["wavelength_nm"][0] <= 250.0 <= target["wavelength_nm"][0]
                     <= sub["wavelength_nm"][-1], "substitution does not cover required interval")
            if kind == "water_k_substitution":
                _require(target["source_quantity"] == "Napierian_absorption_coefficient"
                         and target["source_unit"] == "cm^-1" and rule.get("anchor") == "multiplicative",
                         "wrong absorption splice basis/anchor")
            else:
                _require(target["source_quantity"] == "real_refractive_index"
                         and target["source_unit"] == "dimensionless" and rule.get("anchor") == "additive",
                         "wrong index splice basis/anchor")

    def _check_dependencies(self, curve_id: str, visited: tuple[str, ...]) -> None:
        _require(curve_id not in visited, "cyclic proxy policies")
        for rule in (self._policies[curve_id]["below"], self._policies[curve_id]["above"]):
            if rule["kind"] == "scaled_proxy":
                self._check_dependencies(rule["proxy_curve_id"], visited + (curve_id,))

    def _evaluate(self, curve_id: str, wavelength: float) -> tuple[float, list[dict[str, Any]]]:
        curve = self._curves[curve_id]
        wave, values = curve["wavelength_nm"], curve["values"]
        if wave[0] <= wavelength <= wave[-1]:
            index = bisect.bisect_left(wave, wavelength)
            origin = "source_sample" if wave[index] == wavelength else "source_interpolation"
            return _interpolate(curve, wavelength), [{"curve_id": curve_id, "origin": origin}]
        side = "below" if wavelength < wave[0] else "above"
        anchor_nm, anchor_value = (wave[0], values[0]) if side == "below" else (wave[-1], values[-1])
        rule = self._policies[curve_id][side]
        kind, dependencies = rule["kind"], []
        if kind == "zero":
            value = 0.0
        elif kind == "endpoint_hold":
            value = anchor_value
        elif kind == "exponential":
            value = anchor_value * math.exp(-abs(wavelength - anchor_nm) / rule["e_fold_nm"])
        elif kind == "power_law":
            value = anchor_value * (wavelength / anchor_nm) ** rule["exponent"]
        elif kind == "scaled_proxy":
            proxy_id = rule["proxy_curve_id"]
            reference, anchor_dependencies = self._evaluate(proxy_id, anchor_nm)
            proxy_value, dependencies = self._evaluate(proxy_id, wavelength)
            dependencies = anchor_dependencies + dependencies
            _require(reference > 0, "cannot scale a zero-valued proxy anchor")
            value = anchor_value * (proxy_value / reference)
        elif kind in {"water_k_substitution", "real_index_substitution"}:
            sub_id = rule["substitution_curve_id"]
            sub = self._substitutions[sub_id]
            sub_value = _interpolate(sub, wavelength)
            sub_anchor = _interpolate(sub, anchor_nm)
            if kind == "water_k_substitution":
                # k is imaginary refractive index; lambda in cm -> mu in cm^-1.
                sub_value *= 4.0 * math.pi / (wavelength * 1e-7)
                sub_anchor *= 4.0 * math.pi / (anchor_nm * 1e-7)
                _require(sub_anchor > 0, "cannot scale a zero-valued water k anchor")
                value = anchor_value * (sub_value / sub_anchor)
            else:
                value = sub_value + (anchor_value - sub_anchor)
            dependencies = [{"curve_id": sub_id, "origin": "substitution_interpolation"}]
        else:
            raise ImportErrorDetail(f"{curve_id}: rejected wavelength outside source support")
        return value, [{"curve_id": curve_id, "origin": "approximation", "side": side,
                        "policy": copy.deepcopy(rule)}] + dependencies

    def evaluate(self, curve_id: str, wavelength_nm: float) -> float:
        """Return the unchanged source quantity/unit, not a tissue coefficient."""
        return self.evaluate_with_provenance(curve_id, wavelength_nm)["value"]

    def evaluate_with_provenance(self, curve_id: str, wavelength_nm: float) -> dict[str, Any]:
        _require(curve_id in self._curves, f"unknown curve {curve_id!r}")
        _require(_finite(wavelength_nm) and 250.0 <= wavelength_nm <= 2500.0,
                 "wavelength must be finite and within [250,2500] nm")
        value, trace = self._evaluate(curve_id, wavelength_nm)
        _require(_finite(value) and value >= 0, "approximation result must be finite and nonnegative")
        curve = self._curves[curve_id]
        if curve["source_quantity"] == "real_refractive_index":
            _require(value > 0, "approximated real refractive index must be positive")
        identities = {}
        for event in trace:
            source = self._curves.get(event["curve_id"], self._substitutions.get(event["curve_id"]))
            identities.update(source["provenance"]["source_files"])
        return {"profile_id": self.profile_id, "bundle_sha256": self.bundle_sha256,
                "approximate": True, "curve_id": curve_id,
                "wavelength_nm": wavelength_nm, "value": value,
                "source_quantity": curve["source_quantity"], "source_unit": curve["source_unit"],
                "reference_basis": curve["reference_basis"], "source_files": identities,
                "evaluation_trace": trace,
                "validation_status": self._bundle["profile"]["validation_status"]}


def write_bundle(bundle: dict[str, Any], output: Path) -> None:
    """Validate/serialize before writing; publish from a unique temporary file."""
    ApproximationProfile(bundle)
    payload = json.dumps(bundle, indent=2, sort_keys=True, ensure_ascii=False, allow_nan=False) + "\n"
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix="hylios-profile-", suffix=".tmp", dir=output.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(payload)
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--source-dir", type=Path, help="pinned cache, including Hale-Querry .m/.txt sources")
    inputs.add_argument("--bundle", type=Path,
                        help="generated bundle (content integrity and structure checked; original source bytes not reverified)")
    parser.add_argument("--output", type=Path, help="JSON bundle file to create")
    parser.add_argument("--curve", choices=sorted(CURVE_SPECS), help="curve to query in its original source basis")
    parser.add_argument("--wavelength-nm", type=float, action="append", help="query wavelength (repeatable)")
    args = parser.parse_args()
    if bool(args.curve) != bool(args.wavelength_nm):
        parser.error("--curve and --wavelength-nm must be provided together")
    if not args.output and not args.curve:
        parser.error("provide --output and/or a curve/wavelength query")
    try:
        bundle = (build_profile(args.source_dir) if args.source_dir
                  else json.loads(args.bundle.read_text()))
        evaluator = ApproximationProfile(bundle)
        # Evaluate all queries before writing the bundle or printing results.
        results = [evaluator.evaluate_with_provenance(args.curve, wave)
                   for wave in args.wavelength_nm or []]
        if args.output:
            write_bundle(bundle, args.output)
            print(f"Wrote explicitly approximate bundle {args.output}", file=sys.stderr)
        for result in results:
            print(json.dumps(result, sort_keys=True, ensure_ascii=False, allow_nan=False))
    except (OSError, ValueError, KeyError, TypeError, OverflowError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
