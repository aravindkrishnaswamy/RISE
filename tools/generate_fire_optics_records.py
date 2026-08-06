#!/usr/bin/env python3
"""Freeze the draft fire-optics data into canonical RISE-CBOR64-v1 bytes."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
import unicodedata
from pathlib import Path


def pchip_slopes(xs: list[float], ys: list[float]) -> list[float]:
    if len(xs) != len(ys) or len(xs) < 2:
        raise ValueError("a spectrum needs at least two aligned knots")
    h = [xs[i + 1] - xs[i] for i in range(len(xs) - 1)]
    if any(step <= 0.0 for step in h):
        raise ValueError("spectrum knots must increase")
    delta = [(ys[i + 1] - ys[i]) / h[i] for i in range(len(h))]
    slopes = [0.0] * len(xs)
    if len(xs) == 2:
        return [delta[0], delta[0]]
    for i in range(1, len(xs) - 1):
        if delta[i - 1] * delta[i] <= 0.0:
            slopes[i] = 0.0
        else:
            w1 = 2.0 * h[i] + h[i - 1]
            w2 = h[i] + 2.0 * h[i - 1]
            slopes[i] = (w1 + w2) / (w1 / delta[i - 1] + w2 / delta[i])

    def endpoint(h0: float, h1: float, d0: float, d1: float) -> float:
        value = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1)
        if value * d0 <= 0.0:
            return 0.0
        if d0 * d1 < 0.0 and abs(value) > abs(3.0 * d0):
            return 3.0 * d0
        return value

    slopes[0] = endpoint(h[0], h[1], delta[0], delta[1])
    slopes[-1] = endpoint(h[-1], h[-2], delta[-1], delta[-2])
    return slopes


def derivative_enclosures(
    xs: list[float], ys: list[float], slopes: list[float]
) -> list[list[float]]:
    result: list[list[float]] = []
    for i in range(len(xs) - 1):
        h = xs[i + 1] - xs[i]
        a = 2.0 * ys[i] - 2.0 * ys[i + 1] + h * (slopes[i] + slopes[i + 1])
        b = -3.0 * ys[i] + 3.0 * ys[i + 1] - h * (
            2.0 * slopes[i] + slopes[i + 1]
        )
        c = h * slopes[i]

        def derivative(u: float) -> float:
            return (3.0 * a * u * u + 2.0 * b * u + c) / h

        candidates = [derivative(0.0), derivative(1.0)]
        if a != 0.0:
            vertex = -b / (3.0 * a)
            if 0.0 < vertex < 1.0:
                candidates.append(derivative(vertex))
        lower = min(candidates)
        upper = max(candidates)
        roundoff_margin = 128.0 * sys.float_info.epsilon * max(
            1.0, abs(lower), abs(upper)
        )
        lower -= roundoff_margin
        upper += roundoff_margin
        for _ in range(8192):
            lower = math.nextafter(lower, -math.inf)
            upper = math.nextafter(upper, math.inf)
        result.append([xs[i], xs[i + 1], lower, upper])
    return result


def tabulated_spectrum(xs: list[float], ys: list[float]) -> dict:
    slopes = pchip_slopes(xs, ys)
    return {
        "slopes": slopes,
        "derivative_enclosures": derivative_enclosures(xs, ys, slopes),
    }


def encode_argument(major: int, value: int) -> bytes:
    if value < 24:
        return bytes([(major << 5) | value])
    if value <= 0xFF:
        return bytes([(major << 5) | 24, value])
    if value <= 0xFFFF:
        return bytes([(major << 5) | 25]) + struct.pack(">H", value)
    if value <= 0xFFFFFFFF:
        return bytes([(major << 5) | 26]) + struct.pack(">I", value)
    return bytes([(major << 5) | 27]) + struct.pack(">Q", value)


def encode(value) -> bytes:
    if value is None:
        return b"\xf6"
    if value is False:
        return b"\xf4"
    if value is True:
        return b"\xf5"
    if isinstance(value, int):
        return encode_argument(0, value) if value >= 0 else encode_argument(1, -1 - value)
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError("RISE-CBOR64-v1 forbids non-finite floats")
        if value == 0.0:
            value = 0.0
        return b"\xfb" + struct.pack(">d", value)
    if isinstance(value, str):
        if unicodedata.normalize("NFC", value) != value:
            raise ValueError("text is not NFC")
        payload = value.encode("utf-8")
        return encode_argument(3, len(payload)) + payload
    if isinstance(value, bytes):
        return encode_argument(2, len(value)) + value
    if isinstance(value, list):
        return encode_argument(4, len(value)) + b"".join(encode(item) for item in value)
    if isinstance(value, dict):
        keys = sorted(value, key=lambda key: key.encode("utf-8"))
        if len(keys) != len(set(keys)) or not all(isinstance(key, str) for key in keys):
            raise ValueError("maps require unique text keys")
        return encode_argument(5, len(keys)) + b"".join(
            encode(key) + encode(value[key]) for key in keys
        )
    raise TypeError(type(value))


def predictive_payload(data_dir: Path) -> dict:
    effective = json.loads(
        (data_dir / "fire_optics_mac_equivalent_e.draft.json").read_text()
    )
    hot = json.loads((data_dir / "fire_optics_hot_soot.draft.json").read_text())
    cool = json.loads((data_dir / "fire_optics_cool_carbon.draft.json").read_text())
    condensed = json.loads(
        (data_dir / "fire_optics_condensed_organics.draft.json").read_text()
    )

    effective_rows = [[float(value) for value in row] for row in effective["table"]["rows"]]
    effective_x = [row[0] for row in effective_rows]
    effective_mac = [row[2] for row in effective_rows]

    hot_rows = [
        [float(value) for value in row]
        for row in hot["computed_outputs"]["spectral_young_dp30_N50"]["rows"]
    ]
    hot_x = [row[0] for row in hot_rows]
    hot_omega = [row[1] for row in hot_rows]
    hot_g = [row[2] for row in hot_rows]

    condensed_rows = [
        [float(value) for value in row]
        for row in condensed["computed_outputs_full_mie"]["table"]["rows"]
    ]
    condensed_x = [row[0] for row in condensed_rows]
    condensed_k = [row[1] for row in condensed_rows]
    condensed_omega = [row[2] for row in condensed_rows]
    condensed_g = [row[3] for row in condensed_rows]

    cool_values = cool["values"]
    return {
        "schema_version": 1,
        "record_kind": "fire_optics_preset",
        "record_name": "fire-optics-predictive-v1",
        "record_class": "predictive_optical_preset",
        "interpolation": "pchip_monotone_c1_v1",
        "effective_absorption": {
            "record_name": effective["record_name"],
            "quantity_name": effective["quantity_name"],
            "normative_quantity": effective["normative_quantity"],
            "pinned_density_g_cm3": float(effective["definition"]["pinned_density_g_cm3"]),
            "domain_nm": [380.0, 780.0],
            "columns": ["lambda_nm", "E_eff", "MAC_m2_per_g"],
            "rows": effective_rows,
            "mac_interpolation": tabulated_spectrum(effective_x, effective_mac),
        },
        "hot_soot": {
            "record_name": hot["record_name"],
            "domain_nm": [380.0, 780.0],
            "columns": ["lambda_nm", "omega", "g"],
            "rows": hot_rows,
            "omega_interpolation": tabulated_spectrum(hot_x, hot_omega),
            "g_interpolation": tabulated_spectrum(hot_x, hot_g),
        },
        "cool_carbon": {
            "record_name": cool["record_name"],
            "k_m_extinction_633nm_m2_per_g": float(
                cool_values["k_m_extinction_633nm_m2_per_g"]["value"]
            ),
            "omega_633nm": float(cool_values["omega_633nm"]["value"]),
            "g_633nm": float(cool_values["g_asymmetry"]["value"]),
            "n_spectral_exponent": float(cool_values["n_spectral_exponent"]["value"]),
            "n_supported_range": [
                float(value) for value in cool_values["n_spectral_exponent"]["range"]
            ],
            "certified_domain_nm": [
                float(value) for value in cool_values["n_spectral_exponent"]["validity_nm"]
            ],
            "preview_extrapolation_rule": "same_power_law_v1",
        },
        "condensed_organics": {
            "record_name": condensed["record_name"],
            "applicability": condensed["applies_to"],
            "extinction_angstrom_exponent_450_633": float(
                condensed["computed_outputs_full_mie"]["extinction_angstrom_exponent_450_633"]
            ),
            "domain_nm": [380.0, 780.0],
            "columns": ["lambda_nm", "k_ext_m2_per_g", "omega", "g"],
            "rows": condensed_rows,
            "k_ext_interpolation": tabulated_spectrum(condensed_x, condensed_k),
            "omega_interpolation": tabulated_spectrum(condensed_x, condensed_omega),
            "g_interpolation": tabulated_spectrum(condensed_x, condensed_g),
            "ir_closure_status": condensed["ir_closure"]["status"].lower(),
            "predictive_reason_code": condensed["ir_closure"]["predictive_reason_code"],
        },
    }


def synthetic_payload(data_dir: Path) -> dict:
    effective = json.loads(
        (data_dir / "fire_optics_mac_equivalent_e.draft.json").read_text()
    )
    cool = json.loads((data_dir / "fire_optics_cool_carbon.draft.json").read_text())
    condensed = json.loads(
        (data_dir / "fire_optics_condensed_organics.draft.json").read_text()
    )
    return {
        "schema_version": 1,
        "record_kind": "fire_optics_preset",
        "record_name": "fire-optics-synthetic-regression-v1",
        "record_class": "synthetic_regression_fixture",
        "interpolation": "analytic_fixture_v1",
        "effective_absorption": {
            "model": "constant_E_eff",
            "E_eff": 0.26,
            "pinned_density_g_cm3": float(effective["definition"]["pinned_density_g_cm3"]),
            "domain_nm": [380.0, 780.0],
        },
        "hot_soot": {"omega": 0.10, "g": 0.50},
        "cool_carbon": {
            "k_m_extinction_633nm_m2_per_g": float(
                cool["values"]["k_m_extinction_633nm_m2_per_g"]["value"]
            ),
            "n_spectral_exponent": 1.20,
            "omega": 0.60,
            "g": 0.60,
        },
        "condensed_organics": {
            "k_m_extinction_633nm_m2_per_g": float(
                condensed["computed_outputs_full_mie"]["table"]["rows"][3][1]
            ),
            "n_spectral_exponent": 0.50,
            "omega": 0.90,
            "g": 0.70,
            "predictive_reason_code": condensed["ir_closure"]["predictive_reason_code"],
        },
    }


def emit_array(target, name: str, payload: bytes) -> None:
    target.write(f"static const unsigned char {name}[] = {{\n")
    for start in range(0, len(payload), 16):
        values = ",".join(f"0x{value:02x}u" for value in payload[start : start + 16])
        target.write(f"\t{values},\n")
    target.write("};\n")
    target.write(f"static const std::size_t {name}Size = sizeof({name});\n")
    target.write(
        f'static const char {name}SHA256[] = "{hashlib.sha256(payload).hexdigest()}";\n\n'
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("data_dir", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    predictive = encode(predictive_payload(args.data_dir))
    synthetic = encode(synthetic_payload(args.data_dir))
    with args.output.open("w", encoding="utf-8", newline="\n") as target:
        target.write(
            "// Generated exclusively from docs/data/fire_optics_*.draft.json and\n"
            "// the synthetic fixture values frozen in FIRE_SMOKE_DESIGN.md SS12.\n\n"
        )
        emit_array(target, "kPredictiveFireOpticsV1", predictive)
        emit_array(target, "kSyntheticFireOpticsV1", synthetic)


if __name__ == "__main__":
    main()
