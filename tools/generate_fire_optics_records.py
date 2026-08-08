#!/usr/bin/env python3
"""Freeze the draft fire-optics data into canonical RISE-CBOR64-v1 bytes."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import re
import struct
import sys
from functools import lru_cache
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


@lru_cache(maxsize=1)
def unicode17_tables():
    source = (
        Path(__file__).resolve().parent.parent
        / "src/Library/Utilities/UnicodeNFCData.inc"
    ).read_text(encoding="utf-8")

    def array_body(name: str) -> str:
        match = re.search(
            rf"static const [^\n]+ {name}\[\] = \{{(.*?)\n\}};",
            source,
            re.DOTALL,
        )
        if not match:
            raise ValueError(f"runtime Unicode-17 table {name} is missing")
        return match.group(1)

    decomposition_rows = [
        tuple(int(value, 16 if index == 0 else 10) for index, value in enumerate(row))
        for row in re.findall(
            r"\{0x([0-9a-f]+)u,([0-9]+)u,([0-9]+)u\}",
            array_body("kCanonicalDecompositions"),
        )
    ]
    decomposition_data = [
        int(value, 16)
        for value in re.findall(
            r"0x([0-9a-f]+)u", array_body("kCanonicalDecompositionData")
        )
    ]
    decompositions = {
        codepoint: tuple(decomposition_data[offset : offset + length])
        for codepoint, offset, length in decomposition_rows
    }
    combining = {
        int(codepoint, 16): int(value)
        for codepoint, value in re.findall(
            r"\{0x([0-9a-f]+)u,([0-9]+)u\}",
            array_body("kCanonicalCombiningClasses"),
        )
    }
    compositions = {
        (int(first, 16), int(second, 16)): int(composite, 16)
        for first, second, composite in re.findall(
            r"\{0x([0-9a-f]+)u,0x([0-9a-f]+)u,0x([0-9a-f]+)u\}",
            array_body("kCanonicalCompositions"),
        )
    }
    return decompositions, combining, compositions


def is_unicode17_nfc(value: str) -> bool:
    decompositions, combining, compositions = unicode17_tables()

    def decompose(codepoint: int, output: list[int]) -> None:
        s_base, l_base, v_base, t_base = 0xAC00, 0x1100, 0x1161, 0x11A7
        l_count, v_count, t_count = 19, 21, 28
        n_count = v_count * t_count
        s_count = l_count * n_count
        if s_base <= codepoint < s_base + s_count:
            s_index = codepoint - s_base
            output.append(l_base + s_index // n_count)
            output.append(v_base + (s_index % n_count) // t_count)
            t_index = s_index % t_count
            if t_index:
                output.append(t_base + t_index)
            return
        parts = decompositions.get(codepoint)
        if not parts:
            output.append(codepoint)
            return
        for part in parts:
            decompose(part, output)

    def compose_pair(first: int, second: int) -> int | None:
        s_base, l_base, v_base, t_base = 0xAC00, 0x1100, 0x1161, 0x11A7
        l_count, v_count, t_count = 19, 21, 28
        n_count = v_count * t_count
        s_count = l_count * n_count
        l_index = first - l_base
        if 0 <= l_index < l_count:
            v_index = second - v_base
            if 0 <= v_index < v_count:
                return s_base + (l_index * v_count + v_index) * t_count
        s_index = first - s_base
        if 0 <= s_index < s_count and s_index % t_count == 0:
            t_index = second - t_base
            if 0 < t_index < t_count:
                return first + t_index
        return compositions.get((first, second))

    codepoints = [ord(character) for character in value]
    decomposed: list[int] = []
    for codepoint in codepoints:
        decompose(codepoint, decomposed)
    for index in range(1, len(decomposed)):
        ccc = combining.get(decomposed[index], 0)
        if ccc == 0:
            continue
        insertion = index
        while insertion > 0:
            previous = combining.get(decomposed[insertion - 1], 0)
            if previous == 0 or previous <= ccc:
                break
            decomposed[insertion], decomposed[insertion - 1] = (
                decomposed[insertion - 1],
                decomposed[insertion],
            )
            insertion -= 1

    composed: list[int] = []
    starter_index = 0
    last_class = 0
    for codepoint in decomposed:
        ccc = combining.get(codepoint, 0)
        composite = compose_pair(composed[starter_index], codepoint) if composed else None
        if composite is not None and (last_class < ccc or last_class == 0):
            composed[starter_index] = composite
        else:
            if ccc == 0:
                starter_index = len(composed)
            composed.append(codepoint)
            last_class = ccc
    return composed == codepoints


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
        if not is_unicode17_nfc(value):
            raise ValueError("text is not Unicode-17 NFC")
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


def canonical_value_envelope(envelope: dict) -> dict:
    result = dict(envelope)
    result["value"] = float(envelope["value"])
    return result


def component_statuses(*records: tuple[str, dict]) -> dict:
    return {name: record["record_status"] for name, record in records}


def validate_unicode17_authority() -> None:
    composed = "\U00016121"
    decomposed = "\U0001611e\U0001611e"
    if not is_unicode17_nfc(composed) or is_unicode17_nfc(decomposed):
        raise ValueError("Unicode-17 NFC authority self-test failed")


def predictive_operational_projection(record: dict) -> dict:
    effective = record["effective_absorption"]
    hot = record["hot_soot"]
    cool = record["cool_carbon"]
    condensed = record["condensed_organics"]

    def phi_operations(section: dict) -> dict:
        phi = section["phi_T_partition"]
        return {
            "form": phi["form"],
            "hot_fraction_temperature_band_K": phi[
                "hot_fraction_temperature_band_K"
            ],
        }

    return {
        "condensed_organics": {
            key: condensed[key]
            for key in (
                "applicability",
                "columns",
                "domain_nm",
                "extinction_angstrom_exponent_450_633",
                "g_interpolation",
                "ir_closure_status",
                "k_ext_interpolation",
                "omega_interpolation",
                "out_of_domain_policy",
                "predictive_reason_code",
                "rows",
            )
        },
        "cool_carbon": {
            "certified_domain_nm": cool["certified_domain_nm"],
            "g_633nm": cool["g_633nm"],
            "k_m_extinction_633nm_m2_per_g": cool[
                "k_m_extinction_633nm_m2_per_g"
            ],
            "n_spectral_exponent": cool["n_spectral_exponent"],
            "n_supported_range": cool["n_supported_range"],
            "omega_633nm": cool["omega_633nm"],
            "out_of_domain_policy": cool["out_of_domain_policy"],
            "phi_T_partition": phi_operations(cool),
        },
        "effective_absorption": {
            key: effective[key]
            for key in (
                "columns",
                "domain_nm",
                "mac_interpolation",
                "normative_quantity",
                "out_of_domain_policy",
                "pinned_density_g_cm3",
                "rows",
            )
        },
        "hot_soot": {
            "columns": hot["columns"],
            "domain_nm": hot["domain_nm"],
            "g_interpolation": hot["g_interpolation"],
            "omega_interpolation": hot["omega_interpolation"],
            "out_of_domain_policy": hot["out_of_domain_policy"],
            "phi_T_partition": phi_operations(hot),
            "rows": hot["rows"],
        },
        "interpolation": record["interpolation"],
        "record_class": record["record_class"],
    }


def predictive_payload(data_dir: Path) -> dict:
    schema = json.loads(
        (data_dir / "fire_optics_record_schema.draft.json").read_text()
    )
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
    adopted_hot = hot["computed_outputs"]["young_in_flame_550nm"]
    for row in hot_rows:
        if row[0] == 550.0:
            row[1] = float(adopted_hot["omega_central"])
            row[2] = float(adopted_hot["g_central"])
    hot_x = [row[0] for row in hot_rows]
    hot_omega = [row[1] for row in hot_rows]
    hot_g = [row[2] for row in hot_rows]

    hot_phi_source = hot["phi_T_partition"]
    cool_phi_source = cool["phi_T_partition"]
    if encode(hot_phi_source) != encode(cool_phi_source):
        raise ValueError(
            "hot-soot and cool-carbon phi_T_partition fields must be bit-identical"
        )
    hot_phi = dict(hot_phi_source)
    hot_phi["hot_fraction_temperature_band_K"] = [
        float(value) for value in hot_phi_source["hot_fraction_temperature_band_K"]
    ]
    hot_phi["uncertainty"] = {
        "kind": "design_pinned_exact",
        "magnitude": 0,
    }
    cool_phi = dict(hot_phi)

    condensed_rows = [
        [float(value) for value in row]
        for row in condensed["computed_outputs_full_mie"]["table"]["rows"]
    ]
    condensed_x = [row[0] for row in condensed_rows]
    condensed_k = [row[1] for row in condensed_rows]
    condensed_omega = [row[2] for row in condensed_rows]
    condensed_g = [row[3] for row in condensed_rows]

    cool_values = cool["values"]
    hot_table_metadata = dict(
        hot["computed_outputs"]["spectral_young_dp30_N50_metadata"]
    )
    hot_table_metadata["adopted_550nm"] = adopted_hot
    hot_table_metadata["adoption_ruling"] = hot["g_hot_adopted"]
    return {
        "schema_version": 1,
        "record_kind": "fire_optics_preset",
        "record_name": "fire-optics-predictive-v1",
        "version": effective["version"],
        "record_status": effective["record_status"],
        "component_record_statuses": component_statuses(
            ("effective_absorption", effective),
            ("hot_soot", hot),
            ("cool_carbon", cool),
            ("condensed_organics", condensed),
        ),
        "record_class": "predictive_optical_preset",
        "interpolation": "pchip_monotone_c1_v1",
        "provenance_schema": schema,
        "source_records": {
            "condensed_organics": condensed,
            "cool_carbon": cool,
            "effective_absorption": effective,
            "hot_soot": hot,
        },
        "effective_absorption": {
            "record_name": effective["record_name"],
            "quantity_name": effective["quantity_name"],
            "normative_quantity": effective["normative_quantity"],
            "out_of_domain_policy": effective["out_of_domain_policy"],
            "pinned_density_g_cm3": float(
                effective["definition"]["pinned_density_g_cm3"]["value"]
            ),
            "domain_nm": [380.0, 780.0],
            "columns": ["lambda_nm", "E_eff", "MAC_m2_per_g"],
            "rows": effective_rows,
            "table_metadata": effective["table"]["table_metadata"],
            "mac_interpolation": tabulated_spectrum(effective_x, effective_mac),
        },
        "hot_soot": {
            "record_name": hot["record_name"],
            "phi_T_partition": hot_phi,
            "domain_nm": [380.0, 780.0],
            "columns": ["lambda_nm", "omega", "g"],
            "rows": hot_rows,
            "table_metadata": hot_table_metadata,
            "omega_interpolation": tabulated_spectrum(hot_x, hot_omega),
            "out_of_domain_policy": hot["out_of_domain_policy"],
            "g_interpolation": tabulated_spectrum(hot_x, hot_g),
        },
        "cool_carbon": {
            "record_name": cool["record_name"],
            "phi_T_partition": cool_phi,
            "k_m_extinction_633nm_m2_per_g": float(
                cool_values["k_m_extinction_633nm_m2_per_g"]["value"]
            ),
            "omega_633nm": float(cool_values["omega_633nm"]["value"]),
            "g_633nm": float(cool_values["g_asymmetry"]["value"]),
            "n_spectral_exponent": float(cool_values["n_spectral_exponent"]["value"]),
            "n_supported_range": [
                float(value)
                for value in cool_values["n_spectral_exponent"]["uncertainty"][
                    "magnitude"
                ]
            ],
            "certified_domain_nm": [
                float(value) for value in cool_values["n_spectral_exponent"]["validity_nm"]
            ],
            "out_of_domain_policy": cool_values["n_spectral_exponent"][
                "out_of_domain_policy"
            ],
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
            "table_metadata": condensed["computed_outputs_full_mie"][
                "table_metadata"
            ],
            "k_ext_interpolation": tabulated_spectrum(condensed_x, condensed_k),
            "omega_interpolation": tabulated_spectrum(condensed_x, condensed_omega),
            "out_of_domain_policy": condensed["out_of_domain_policy"],
            "g_interpolation": tabulated_spectrum(condensed_x, condensed_g),
            "ir_closure_status": condensed["ir_closure"]["status"].lower(),
            "predictive_reason_code": condensed["ir_closure"]["predictive_reason_code"],
        },
    }


def synthetic_payload(data_dir: Path) -> dict:
    schema = json.loads(
        (data_dir / "fire_optics_record_schema.draft.json").read_text()
    )
    fixtures = json.loads(
        (data_dir / "fire_optics_synthetic_fixtures.draft.json").read_text()
    )
    effective = json.loads(
        (data_dir / "fire_optics_mac_equivalent_e.draft.json").read_text()
    )
    cool = json.loads((data_dir / "fire_optics_cool_carbon.draft.json").read_text())
    hot = json.loads((data_dir / "fire_optics_hot_soot.draft.json").read_text())
    condensed = json.loads(
        (data_dir / "fire_optics_condensed_organics.draft.json").read_text()
    )
    hot_phi_source = hot["phi_T_partition"]
    cool_phi_source = cool["phi_T_partition"]
    if encode(hot_phi_source) != encode(cool_phi_source):
        raise ValueError(
            "hot-soot and cool-carbon phi_T_partition fields must be bit-identical"
        )
    hot_phi = dict(hot_phi_source)
    hot_phi["hot_fraction_temperature_band_K"] = [
        float(value) for value in hot_phi_source["hot_fraction_temperature_band_K"]
    ]
    hot_phi["uncertainty"] = {
        "kind": "design_pinned_exact",
        "magnitude": 0,
    }
    cool_phi = dict(hot_phi)
    fixture_values = fixtures["values"]
    fixture_hot = fixture_values["hot_soot"]
    fixture_cool = fixture_values["fresh_smoke_cool_carbon"]
    fixture_condensed = fixture_values["organic_droplets_condensed"]
    return {
        "schema_version": 1,
        "record_kind": "fire_optics_preset",
        "record_name": "fire-optics-synthetic-regression-v1",
        "version": fixtures["version"],
        "record_status": fixtures["record_status"],
        "component_record_statuses": component_statuses(
            ("effective_absorption", effective),
            ("hot_soot", hot),
            ("cool_carbon", cool),
            ("condensed_organics", condensed),
            ("synthetic_fixtures", fixtures),
        ),
        "record_class": "synthetic_regression_fixture",
        "interpolation": "analytic_fixture_v1",
        "provenance_schema": schema,
        "source_records": {
            "condensed_organics": condensed,
            "cool_carbon": cool,
            "effective_absorption": effective,
            "hot_soot": hot,
            "synthetic_fixtures": fixtures,
        },
        "effective_absorption": {
            "model": "constant_E_eff",
            "E_eff": canonical_value_envelope(fixture_values["E_eff_fixture"]),
            "pinned_density_g_cm3": float(
                effective["definition"]["pinned_density_g_cm3"]["value"]
            ),
            "pinned_density_metadata": effective["definition"][
                "pinned_density_g_cm3"
            ],
            "out_of_domain_policy": fixtures["out_of_domain_policy"],
            "domain_nm": [380.0, 780.0],
        },
        "hot_soot": {
            "omega": canonical_value_envelope(fixture_hot["omega"]),
            "out_of_domain_policy": fixtures["out_of_domain_policy"],
            "g": canonical_value_envelope(fixture_hot["g"]),
            "phi_T_partition": hot_phi,
        },
        "cool_carbon": {
            "k_m_extinction_633nm_m2_per_g": float(
                cool["values"]["k_m_extinction_633nm_m2_per_g"]["value"]
            ),
            "k_m_extinction_metadata": cool["values"][
                "k_m_extinction_633nm_m2_per_g"
            ],
            "n_spectral_exponent": canonical_value_envelope(
                fixture_cool["n_exponent"]
            ),
            "omega": canonical_value_envelope(fixture_cool["omega"]),
            "out_of_domain_policy": fixtures["out_of_domain_policy"],
            "g": canonical_value_envelope(fixture_cool["g"]),
            "phi_T_partition": cool_phi,
        },
        "condensed_organics": {
            "k_m_extinction_633nm_m2_per_g": float(
                condensed["computed_outputs_full_mie"]["table"]["rows"][3][1]
            ),
            "k_m_extinction_metadata": condensed["computed_outputs_full_mie"][
                "table_metadata"
            ],
            "n_spectral_exponent": canonical_value_envelope(
                fixture_condensed["n_exponent"]
            ),
            "omega": canonical_value_envelope(fixture_condensed["omega"]),
            "out_of_domain_policy": fixtures["out_of_domain_policy"],
            "g": canonical_value_envelope(fixture_condensed["g"]),
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
        f'static const char {name}SHA256[] = "{hashlib.sha256(payload).hexdigest()}";\n'
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("data_dir", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    validate_unicode17_authority()
    predictive_record = predictive_payload(args.data_dir)
    operational_projection = predictive_operational_projection(predictive_record)
    legacy_projection = predictive_operational_projection(predictive_record)
    del legacy_projection["effective_absorption"]["out_of_domain_policy"]
    del legacy_projection["hot_soot"]["out_of_domain_policy"]
    del legacy_projection["condensed_organics"]["out_of_domain_policy"]
    legacy_operational_sha256 = hashlib.sha256(
        encode(legacy_projection)
    ).hexdigest()
    if legacy_operational_sha256 != (
        "8e68d6da455f0af89e2334d162aa05944a581753c56cf8a90f45c295dc7ad44c"
    ):
        raise ValueError(
            "schema-v3 policy rebaseline changed a pre-existing operational field: "
            + legacy_operational_sha256
        )
    operational_sha256 = hashlib.sha256(
        encode(operational_projection)
    ).hexdigest()
    if operational_sha256 != (
        "e006a52c644f2ea52ea7538788ea73e4948e1caf4162b42e62d236b55c9245c9"
    ):
        raise ValueError(
            "metadata regeneration changed the canonical operational payload: "
            + operational_sha256
        )
    predictive = encode(predictive_record)
    synthetic = encode(synthetic_payload(args.data_dir))
    target = io.StringIO(newline="\n")
    target.write(
        "// Generated exclusively from docs/data/fire_optics_*.draft.json.\n\n"
    )
    emit_array(target, "kPredictiveFireOpticsV1", predictive)
    emit_array(target, "kSyntheticFireOpticsV1", synthetic)
    generated = target.getvalue()
    if args.check:
        if not args.output.is_file() or args.output.read_text(encoding="utf-8") != generated:
            raise SystemExit(
                f"{args.output} is stale; regenerate it with {Path(__file__).name}"
            )
        return
    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        output.write(generated)


if __name__ == "__main__":
    main()
