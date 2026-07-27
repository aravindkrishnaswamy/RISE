#!/usr/bin/env python3
"""Run a paired beauty-only versus perception-atlas local vision evaluation.

The tool intentionally has no third-party Python dependencies. It drives the
real RISE JSON-RPC render/read_image surface, sends the resulting PNG through an
OpenAI-compatible Ollama endpoint, and writes every observation and response for
audit. See evals/perception_ab/README.md for methodology and interpretation.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import random
import re
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zlib
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
DEFAULT_MANIFEST = Path("evals/perception_ab/cases.json")
DEFAULT_ENDPOINT = "http://127.0.0.1:11434"
DEFAULT_EDGE = 384


def _finite_number(value, label):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{label} must be a number")
    value = float(value)
    if not math.isfinite(value):
        raise ValueError(f"{label} must be finite")
    return value


def _choice(value, allowed, label):
    if value not in allowed:
        raise ValueError(f"{label} must be one of {sorted(allowed)}")
    return value


def load_cases(path):
    with path.open("r", encoding="utf-8") as stream:
        root = json.load(stream)
    return validate_cases(root)


def validate_cases(root):
    if (not isinstance(root, dict) or root.get("schemaVersion") != 1 or
            not isinstance(root.get("cases"), list)):
        raise ValueError("manifest requires schemaVersion 1 and a cases array")
    seen = set()
    out = []
    for index, case in enumerate(root["cases"]):
        label = f"cases[{index}]"
        if not isinstance(case, dict):
            raise ValueError(f"{label} must be an object")
        case_id = case.get("id")
        if not isinstance(case_id, str) or not re.fullmatch(r"[a-z0-9_]+", case_id):
            raise ValueError(f"{label}.id must match [a-z0-9_]+")
        if case_id in seen:
            raise ValueError(f"duplicate case id {case_id}")
        seen.add(case_id)
        family = _choice(case.get("family"),
                         {"depth_order", "surface_normal", "material_lighting"},
                         f"{label}.family")
        question = case.get("question")
        choices = case.get("choices")
        answer = case.get("answer")
        params = case.get("params")
        if not isinstance(question, str) or not question.strip():
            raise ValueError(f"{label}.question must be a non-empty string")
        if (not isinstance(choices, list) or len(choices) != 2 or
                any(not isinstance(x, str) or not re.fullmatch(r"[A-Z0-9_]+", x)
                    for x in choices) or choices[0] == choices[1]):
            raise ValueError(f"{label}.choices must contain two distinct uppercase labels")
        if not isinstance(params, dict):
            raise ValueError(f"{label}.params must be an object")
        expected_choices, expected_answer = validate_case_params(family, params, label)
        if set(choices) != expected_choices:
            raise ValueError(f"{label}.choices must be exactly {sorted(expected_choices)}")
        if answer != expected_answer:
            raise ValueError(f"{label}.answer must be {expected_answer}, derived from params")
        out.append({"id": case_id, "family": family, "question": question.strip(),
                    "choices": choices, "answer": answer, "params": params})
    if not out:
        raise ValueError("manifest cases array cannot be empty")
    return out


def validate_case_params(family, params, label):
    if family == "depth_order":
        near_color = _choice(params.get("nearColor"), {"red", "blue"},
                             f"{label}.params.nearColor")
        _choice(params.get("nearSide"), {"left", "right"}, f"{label}.params.nearSide")
        near_z = _finite_number(params.get("nearZ"), f"{label}.params.nearZ")
        far_z = _finite_number(params.get("farZ"), f"{label}.params.farZ")
        if near_z <= far_z:
            raise ValueError(f"{label}: nearZ must exceed farZ for the +Z camera")
        camera_z = 10.0
        near_radius = 0.72
        near_distance = camera_z - near_z
        far_distance = camera_z - far_z
        if near_distance <= near_radius:
            raise ValueError(f"{label}: near sphere must be fully in front of the camera")
        far_radius = near_radius * far_distance / near_distance
        near_front = near_distance - near_radius
        far_front = far_distance - far_radius
        if far_radius <= 0.0 or far_distance <= far_radius:
            raise ValueError(f"{label}: far sphere must be fully in front of the camera")
        if not near_front < far_front:
            raise ValueError(f"{label}: near sphere must have the closer front surface")
        return {"RED", "BLUE"}, near_color.upper()
    elif family == "surface_normal":
        angle = _finite_number(params.get("angleDegrees"), f"{label}.params.angleDegrees")
        if abs(angle) < 10.0 or abs(angle) > 70.0:
            raise ValueError(f"{label}: abs(angleDegrees) must be in [10,70]")
        return {"POSITIVE_X", "NEGATIVE_X"}, "POSITIVE_X" if angle > 0 else "NEGATIVE_X"
    else:
        high_side = _choice(params.get("highSide"), {"left", "right"},
                            f"{label}.params.highSide")
        if params.get("tiltSign") not in (-1, 1):
            raise ValueError(f"{label}.params.tiltSign must be -1 or 1")
        low = _finite_number(params.get("lowAlbedo"), f"{label}.params.lowAlbedo")
        high = _finite_number(params.get("highAlbedo"), f"{label}.params.highAlbedo")
        if not (0.05 <= low < high <= 0.95):
            raise ValueError(f"{label}: require 0.05 <= lowAlbedo < highAlbedo <= 0.95")
        return {"LEFT", "RIGHT"}, high_side.upper()


def common_scene(edge, camera_z=10.0, fov=30.0, ambient=False):
    light = ("ambient_light\n{\n\tname ambient\n\tpower 1.0\n\tcolor 1 1 1\n}\n"
             if ambient else
             "directional_light\n{\n\tname key\n\tpower 3.141592653589793\n"
             "\tcolor 1 1 1\n\tdirection 0 0 1\n}\n")
    return f"""RISE ASCII SCENE 7
# Generated by tools/perception_ab_eval.py; controlled diagnostic scene.
standard_shader
{{
\tname global
\tshaderop DefaultDirectLighting
}}

pixelpel_rasterizer
{{
\tmax_recursion 4
\tsamples 4
\tlum_samples 1
\tpixel_filter box
\toidn_denoise false
}}

film
{{
\twidth {edge}
\theight {edge}
}}

pinhole_camera
{{
\tlocation 0 0 {camera_z:.9g}
\tlookat 0 0 0
\tup 0 1 0
\tfov {fov:.9g}
}}

{light}
"""


def painter_material(name, rgb):
    r, g, b = rgb
    return f"""uniformcolor_painter
{{
\tname pnt_{name}
\tcolor {r:.9g} {g:.9g} {b:.9g}
\tcolorspace Rec709RGB_Linear
}}

lambertian_material
{{
\tname mat_{name}
\treflectance pnt_{name}
}}

"""


def depth_scene(case, edge):
    p = case["params"]
    camera_z = 10.0
    near_z = float(p["nearZ"])
    far_z = float(p["farZ"])
    near_radius = 0.72
    # Match projected angular radius closely enough that beauty does not leak
    # which sphere is physically larger/farther.
    far_radius = near_radius * (camera_z - far_z) / (camera_z - near_z)
    near_x = -1.35 if p["nearSide"] == "left" else 1.35
    far_x = -near_x
    near_color = p["nearColor"]
    far_color = "blue" if near_color == "red" else "red"
    by_color = {
        near_color: (near_x, near_z, near_radius),
        far_color: (far_x, far_z, far_radius),
    }
    text = common_scene(edge, camera_z=camera_z, fov=28.0, ambient=True)
    text += painter_material("red", (0.85, 0.055, 0.04))
    text += painter_material("blue", (0.035, 0.11, 0.90))
    for color in ("red", "blue"):
        x, z, radius = by_color[color]
        text += f"""sphere_geometry
{{
\tname geom_{color}
\tradius {radius:.9g}
}}

standard_object
{{
\tname obj_{color}
\tgeometry geom_{color}
\tmaterial mat_{color}
\tposition {x:.9g} 0 {z:.9g}
\treceives_shadows false
}}

"""
    return text


def card_points(center_x, angle_degrees, projected_width=2.0, height=2.7):
    angle = math.radians(angle_degrees)
    cosine = math.cos(angle)
    sine = math.sin(angle)
    world_width = projected_width / cosine
    ux, uz = cosine, -sine
    half_w, half_h = world_width * 0.5, height * 0.5
    return [
        (center_x - ux * half_w, -half_h, -uz * half_w),
        (center_x + ux * half_w, -half_h, +uz * half_w),
        (center_x + ux * half_w, +half_h, +uz * half_w),
        (center_x - ux * half_w, +half_h, -uz * half_w),
    ]


def clipped_plane(name, points, material):
    lines = []
    for key, point in zip(("pta", "ptb", "ptc", "ptd"), points):
        lines.append(f"\t{key} {point[0]:.9g} {point[1]:.9g} {point[2]:.9g}")
    return f"""clippedplane_geometry
{{
\tname geom_{name}
{chr(10).join(lines)}
}}

standard_object
{{
\tname obj_{name}
\tgeometry geom_{name}
\tmaterial {material}
\treceives_shadows false
}}

"""


def normal_scene(case, edge):
    angle = float(case["params"]["angleDegrees"])
    text = common_scene(edge, camera_z=30.0, fov=9.0, ambient=True)
    text += painter_material("card", (0.72, 0.64, 0.12))
    text += clipped_plane("card", card_points(0.0, angle, 2.4, 3.1), "mat_card")
    return text


def albedo_scene(case, edge):
    p = case["params"]
    low = float(p["lowAlbedo"])
    high = float(p["highAlbedo"])
    # Under a head-on directional light, Lambertian radiance is albedo*cos.
    # Tilt the high-albedo card so its beauty intensity matches the low card.
    tilt = math.degrees(math.acos(low / high)) * int(p["tiltSign"])
    high_side = p["highSide"]
    text = common_scene(edge, camera_z=18.0, fov=18.0, ambient=False)
    text += painter_material("low", (low, low, low))
    text += painter_material("high", (high, high, high))
    for side, center_x in (("left", -1.45), ("right", 1.45)):
        is_high = side == high_side
        name = f"{side}_{'high' if is_high else 'low'}"
        points = card_points(center_x, tilt if is_high else 0.0, 1.75, 2.5)
        text += clipped_plane(name, points, "mat_high" if is_high else "mat_low")
    return text


def make_scene(case, edge):
    if case["family"] == "depth_order":
        return depth_scene(case, edge)
    if case["family"] == "surface_normal":
        return normal_scene(case, edge)
    return albedo_scene(case, edge)


def png_dimensions(data):
    if len(data) < 8 or data[:8] != PNG_SIGNATURE:
        raise ValueError("read_image payload has no PNG signature")
    offset = 8
    dimensions = None
    saw_idat = False
    saw_iend = False
    while offset < len(data):
        if len(data) - offset < 12:
            raise ValueError("read_image PNG has a truncated chunk header")
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        chunk_end = offset + 12 + length
        if chunk_end > len(data):
            raise ValueError("read_image PNG has a truncated chunk payload")
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + length]
        expected_crc = struct.unpack(">I", data[offset + 8 + length:chunk_end])[0]
        if zlib.crc32(kind + payload) & 0xffffffff != expected_crc:
            raise ValueError(f"read_image PNG has a bad {kind!r} CRC")
        if dimensions is None:
            if kind != b"IHDR" or length != 13:
                raise ValueError("read_image PNG must begin with a 13-byte IHDR")
            dimensions = struct.unpack(">II", payload[:8])
            if dimensions[0] <= 0 or dimensions[1] <= 0:
                raise ValueError("PNG dimensions must be positive")
        elif kind == b"IHDR":
            raise ValueError("read_image PNG has multiple IHDR chunks")
        if kind == b"IDAT":
            saw_idat = True
        if kind == b"IEND":
            if length != 0 or chunk_end != len(data):
                raise ValueError("read_image PNG has an invalid IEND")
            saw_iend = True
            break
        offset = chunk_end
    if not saw_idat or not saw_iend or dimensions is None:
        raise ValueError("read_image PNG is missing IDAT or IEND")
    return dimensions


def render_case(rise_bin, repo_root, case, edge, image_dir, scene_dir, timeout):
    scene = make_scene(case, edge)
    image_dir.mkdir(parents=True, exist_ok=True)
    scene_dir.mkdir(parents=True, exist_ok=True)
    scene_path = scene_dir / f"{case['id']}.RISEscene"
    scene_path.write_text(scene, encoding="utf-8")
    requests = [
        {"jsonrpc": "2.0", "id": 1, "method": "render",
         "params": {"width": edge, "height": edge, "samples": 4, "perception": True}},
        {"jsonrpc": "2.0", "id": 2, "method": "read_image",
         "params": {"representation": "beauty", "maxEdge": edge}},
        {"jsonrpc": "2.0", "id": 3, "method": "read_image",
         "params": {"representation": "perception", "maxEdge": edge}},
    ]
    stdin = "".join(json.dumps(item, separators=(",", ":")) + "\n" for item in requests)
    started = time.monotonic()
    proc = subprocess.run([str(rise_bin), "--agent-stdio", str(scene_path)],
                          input=stdin, text=True, cwd=repo_root,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=timeout, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"RISE exited {proc.returncode} for {case['id']}: {proc.stderr[-2000:]}")
    envelopes = {}
    for line in proc.stdout.splitlines():
        if not line.strip():
            continue
        try:
            env = json.loads(line)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"non-JSON stdout from RISE for {case['id']}: {line[:200]}") from exc
        rpc_id = env.get("id")
        if rpc_id in envelopes:
            raise RuntimeError(f"duplicate RISE RPC id {rpc_id!r} for {case['id']}")
        envelopes[rpc_id] = env
    render_env = envelopes.get(1, {})
    if "error" in render_env:
        raise RuntimeError(f"render failed for {case['id']}: {render_env['error']}")
    render_result = render_env.get("result")
    if (not isinstance(render_result, dict) or render_result.get("ok") is not True or
            render_result.get("perceptionAvailable") is not True or
            render_result.get("width") != edge or render_result.get("height") != edge):
        raise RuntimeError(f"render contract failed for {case['id']}: {render_result}")
    images = {}
    metadata = {}
    for rpc_id, arm in ((2, "beauty"), (3, "atlas")):
        env = envelopes.get(rpc_id, {})
        if "error" in env:
            raise RuntimeError(f"read_image {arm} failed for {case['id']}: {env['error']}")
        result = env.get("result")
        if not isinstance(result, dict) or not result.get("png_base64"):
            raise RuntimeError(f"read_image {arm} returned no PNG for {case['id']}: {result}")
        representation = "perception" if arm == "atlas" else "beauty"
        if (result.get("representation") != representation or result.get("width") != edge or
                result.get("height") != edge):
            raise RuntimeError(f"read_image {arm} contract failed for {case['id']}: {result}")
        if arm == "atlas" and (result.get("available") is not True or
                result.get("sourceWidth") != edge or result.get("sourceHeight") != edge or
                result.get("panels") != ["beauty", "albedo", "world_normal", "log_depth"]):
            raise RuntimeError(f"perception atlas metadata failed for {case['id']}: {result}")
        data = base64.b64decode(result["png_base64"], validate=True)
        dims = png_dimensions(data)
        if dims != (edge, edge):
            raise RuntimeError(f"{case['id']} {arm} is {dims}, expected {(edge, edge)}")
        path = image_dir / f"{case['id']}.{arm}.png"
        path.write_bytes(data)
        images[arm] = data
        metadata[arm] = {
            "path": str(path), "sha256": hashlib.sha256(data).hexdigest(),
            "bytes": len(data), "width": dims[0], "height": dims[1],
            "rpc": {key: value for key, value in result.items() if key != "png_base64"},
        }
    scene_meta = {"path": str(scene_path), "sha256": hashlib.sha256(scene.encode("utf-8")).hexdigest()}
    return images, metadata, scene_meta, round((time.monotonic() - started) * 1000)


def make_prompt(case):
    choices = " or ".join(case["choices"])
    return (
        "You are taking a controlled vision test. The attached RISE observation is either "
        "a normal beauty render or a 2x2 perception atlas. When it is an atlas, the aligned "
        "panels are: top-left beauty; top-right intrinsic diffuse albedo; bottom-left "
        "world-space shading normal encoded as RGB=(normal+1)/2; bottom-right logarithmic "
        "camera depth, with nearer valid surfaces brighter and misses black. Use every cue "
        "that is present, but do not assume a panel exists when the image is an ordinary "
        f"beauty render. Question: {case['question']} Reply with exactly one label: {choices}. "
        "Do not add punctuation or an explanation."
    )


def normalize_answer(content, choices):
    if not isinstance(content, str):
        return None
    candidate = content.strip().strip(".,:;!?`'\"").strip().upper()
    return candidate if candidate in choices else None


def http_json(url, payload=None, timeout=900):
    data = None if payload is None else json.dumps(payload, separators=(",", ":")).encode("utf-8")
    req = urllib.request.Request(url, data=data,
                                 headers={"Content-Type": "application/json"} if data else {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return json.load(response)
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code} from {url}: {body[:2000]}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"could not reach {url}: {exc}") from exc


def choose_model(endpoint, requested):
    tags = http_json(endpoint.rstrip("/") + "/api/tags", timeout=10)
    models = tags.get("models", []) if isinstance(tags, dict) else []
    by_name = {item.get("name"): item for item in models if isinstance(item, dict)}
    if requested:
        candidates = [item for item in models
                      if item.get("name") == requested or item.get("model") == requested]
        if not candidates:
            raise ValueError(f"requested model {requested!r} is not installed")
        chosen = candidates[0]
        if "vision" not in chosen.get("capabilities", []):
            raise ValueError(f"requested model {requested!r} does not advertise vision capability")
        return chosen
    candidates = [item for item in models if "vision" in item.get("capabilities", [])]
    if not candidates:
        raise ValueError("no installed Ollama model advertises vision capability")
    return min(candidates, key=lambda item: (int(item.get("size", 1 << 63)), item.get("name", "")))


def ask_model(endpoint, model, prompt, png, temperature, timeout):
    encoded = base64.b64encode(png).decode("ascii")
    payload = {
        "model": model,
        "messages": [{"role": "user", "content": [
            {"type": "text", "text": prompt},
            {"type": "image_url", "image_url": {"url": "data:image/png;base64," + encoded}},
        ]}],
        "temperature": temperature,
        "max_completion_tokens": 32,
        "reasoning_effort": "none",
        "stream": False,
    }
    started = time.monotonic()
    root = http_json(endpoint.rstrip("/") + "/v1/chat/completions", payload, timeout)
    elapsed_ms = round((time.monotonic() - started) * 1000)
    if not isinstance(root, dict) or root.get("error"):
        raise RuntimeError(f"invalid model response envelope: {root}")
    choices = root.get("choices")
    if (not isinstance(choices, list) or not choices or not isinstance(choices[0], dict) or
            not isinstance(choices[0].get("message"), dict) or
            not isinstance(choices[0]["message"].get("content"), str) or
            not choices[0]["message"]["content"].strip()):
        raise RuntimeError(f"model response contains no answer content: {root}")
    message = choices[0]["message"]
    return {
        "content": message.get("content", ""),
        "reasoning": message.get("reasoning", message.get("reasoning_content", "")),
        "finishReason": choices[0].get("finish_reason") if choices else None,
        "usage": root.get("usage", {}),
        "latencyMs": elapsed_ms,
        "responseId": root.get("id"),
    }


def exact_mcnemar(atlas_only, beauty_only):
    discordant = atlas_only + beauty_only
    if discordant == 0:
        return 1.0
    tail = sum(math.comb(discordant, k) for k in range(min(atlas_only, beauty_only) + 1))
    return min(1.0, 2.0 * tail / (2 ** discordant))


def arm_stats(rows, arm):
    selected = [row for row in rows if row["arm"] == arm]
    successes = sum(bool(row["correct"]) for row in selected)
    return {"correct": successes, "total": len(selected),
            "accuracy": successes / len(selected) if selected else None}


def paired_stats(rows, include_exact=False):
    by_pair = defaultdict(dict)
    for row in rows:
        by_pair[(row["caseId"], row["repeat"])][row["arm"]] = row
    counts = {"both_correct": 0, "both_wrong": 0,
              "atlas_only_wins": 0, "beauty_only_wins": 0}
    complete_pairs = 0
    for key, pair in sorted(by_pair.items()):
        if set(pair) != {"beauty", "atlas"}:
            continue
        beauty = bool(pair["beauty"]["correct"])
        atlas = bool(pair["atlas"]["correct"])
        if beauty and atlas:
            counts["both_correct"] += 1
        elif atlas:
            counts["atlas_only_wins"] += 1
        elif beauty:
            counts["beauty_only_wins"] += 1
        else:
            counts["both_wrong"] += 1
        complete_pairs += 1
    result = {**counts, "completePairs": complete_pairs}
    if include_exact:
        result["exactTwoSidedP"] = exact_mcnemar(counts["atlas_only_wins"],
                                                 counts["beauty_only_wins"])
    return result


def summarize(rows):
    by_pair = defaultdict(dict)
    for row in rows:
        by_pair[(row["caseId"], row["repeat"])][row["arm"]] = row
    parity_deltas = []
    complete_pairs = 0
    for pair in by_pair.values():
        if set(pair) != {"beauty", "atlas"}:
            continue
        complete_pairs += 1
        b_tokens = pair["beauty"].get("usage", {}).get("prompt_tokens")
        a_tokens = pair["atlas"].get("usage", {}).get("prompt_tokens")
        valid_b = (not isinstance(b_tokens, bool) and isinstance(b_tokens, (int, float)) and
                   math.isfinite(b_tokens) and b_tokens >= 0)
        valid_a = (not isinstance(a_tokens, bool) and isinstance(a_tokens, (int, float)) and
                   math.isfinite(a_tokens) and a_tokens >= 0)
        if valid_b and valid_a:
            parity_deltas.append(abs(a_tokens - b_tokens) / max(a_tokens, b_tokens, 1))

    case_votes = defaultdict(lambda: {"beauty": [], "atlas": [], "family": None})
    for row in rows:
        case_votes[row["caseId"]][row["arm"]].append(bool(row["correct"]))
        case_votes[row["caseId"]]["family"] = row["family"]
    majority_rows = []
    case_majority = {"beauty": 0, "atlas": 0, "total": len(case_votes)}
    for case_id, votes in case_votes.items():
        for arm in ("beauty", "atlas"):
            correct = bool(votes[arm] and sum(votes[arm]) * 2 > len(votes[arm]))
            if correct:
                case_majority[arm] += 1
            majority_rows.append({"caseId": case_id, "family": votes["family"],
                                  "repeat": 1, "arm": arm, "correct": correct})
    case_majority["paired"] = paired_stats(majority_rows, include_exact=True)
    case_majority["families"] = {}
    for family in sorted({row["family"] for row in majority_rows}):
        subset = [row for row in majority_rows if row["family"] == family]
        case_majority["families"][family] = {
            "beauty": arm_stats(subset, "beauty"),
            "atlas": arm_stats(subset, "atlas"),
            "paired": paired_stats(subset, include_exact=True),
        }

    families = {}
    for family in sorted({row["family"] for row in rows}):
        subset = [row for row in rows if row["family"] == family]
        families[family] = {"beauty": arm_stats(subset, "beauty"),
                            "atlas": arm_stats(subset, "atlas"),
                            "paired": paired_stats(subset)}

    beauty_stats = arm_stats(rows, "beauty")
    atlas_stats = arm_stats(rows, "atlas")
    delta = ((atlas_stats["accuracy"] - beauty_stats["accuracy"])
             if atlas_stats["accuracy"] is not None and beauty_stats["accuracy"] is not None else None)
    return {
        "beauty": beauty_stats,
        "atlas": atlas_stats,
        "accuracyDelta": delta,
        "paired": paired_stats(rows),
        "caseMajority": case_majority,
        "families": families,
        "tokenParity": {
            "completePairs": complete_pairs,
            "pairsWithUsage": len(parity_deltas),
            "missingPairs": complete_pairs - len(parity_deltas),
            "maxRelativePromptTokenDifference": max(parity_deltas) if parity_deltas else None,
            "withinFivePercent": (complete_pairs > 0 and len(parity_deltas) == complete_pairs and
                                  max(parity_deltas) <= 0.05),
        },
    }


def percent(value):
    return "n/a" if value is None else f"{100.0 * value:.1f}%"


def report_markdown(metadata, summary):
    b = summary["beauty"]
    a = summary["atlas"]
    paired = summary["paired"]
    majority = summary["caseMajority"]
    majority_paired = majority["paired"]
    lines = [
        "# RISE perception-atlas local A/B result", "",
        f"- Model: `{metadata['model']}` (`{metadata.get('digest', '')[:12]}`)",
        f"- Generated: {metadata['generatedAt']}",
        f"- Cases/repeats: {metadata['caseCount']} / {metadata['repeats']}",
        f"- Input: one {metadata['edge']}x{metadata['edge']} PNG per call; identical prompt and output budget",
        f"- Primary unique-case majority: beauty {majority['beauty']}/{majority['total']}; "
        f"atlas {majority['atlas']}/{majority['total']}",
        f"- Unique-case discordance: atlas-only {majority_paired['atlas_only_wins']}, "
        f"beauty-only {majority_paired['beauty_only_wins']}; exact p="
        f"{majority_paired['exactTwoSidedP']:.6g}",
        f"- Repeat-pooled descriptive accuracy: beauty {b['correct']}/{b['total']} "
        f"({percent(b['accuracy'])}); atlas {a['correct']}/{a['total']} ({percent(a['accuracy'])})",
        f"- Repeat-pooled descriptive discordance: atlas-only {paired['atlas_only_wins']}, "
        f"beauty-only {paired['beauty_only_wins']}",
        f"- Prompt-token parity within 5%: {summary['tokenParity']['withinFivePercent']} "
        f"({summary['tokenParity']['pairsWithUsage']}/{summary['tokenParity']['completePairs']} pairs)",
        "", "## By family", "",
        "| Family | Repeat-pooled beauty | Repeat-pooled atlas | Case-majority beauty | Case-majority atlas |",
        "|---|---:|---:|---:|---:|",
    ]
    for family, item in summary["families"].items():
        unique = majority["families"][family]
        lines.append(f"| {family} | {item['beauty']['correct']}/{item['beauty']['total']} "
                     f"({percent(item['beauty']['accuracy'])}) | {item['atlas']['correct']}/"
                     f"{item['atlas']['total']} ({percent(item['atlas']['accuracy'])}) | "
                     f"{unique['beauty']['correct']}/{unique['beauty']['total']} | "
                     f"{unique['atlas']['correct']}/{unique['atlas']['total']} |")
    lines += [
        "", "Repeated calls on byte-identical case images are correlated. Their pooled counts "
        "are a stability diagnostic only; the unique-case majority is the inferential unit.", "",
        "This diagnostic establishes only whether this model can use the atlas on cue-targeted "
        "tasks. It does not establish general agent-task lift; see evals/perception_ab/README.md.", "",
    ]
    return "\n".join(lines)


def git_head(repo_root):
    proc = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo_root,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, check=False)
    return proc.stdout.strip() if proc.returncode == 0 else ""


def git_status(repo_root):
    proc = subprocess.run(["git", "status", "--porcelain", "--untracked-files=all"], cwd=repo_root,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, check=False)
    return None if proc.returncode != 0 else proc.stdout.strip()


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def input_provenance(repo_root, harness, manifest, rise_bin):
    return {
        "gitHead": git_head(repo_root),
        "gitStatus": git_status(repo_root),
        "harnessSha256": file_sha256(harness),
        "manifestSha256": file_sha256(manifest),
        "riseBinarySha256": file_sha256(rise_bin),
    }


def build_jobs(cases, repeats, order_seed):
    """Block-randomize first arm within every family and across each case."""
    rng = random.Random(order_seed)
    first_by_pair = {}
    by_family = defaultdict(list)
    for case in cases:
        by_family[case["family"]].append(case)
    for family in sorted(by_family):
        family_cases = list(by_family[family])
        extra_arms = (["beauty", "atlas"] * ((len(family_cases) + 1) // 2))[:len(family_cases)]
        rng.shuffle(extra_arms)
        for case, extra_arm in zip(family_cases, extra_arms):
            first_arms = ["beauty", "atlas"] * (repeats // 2) + [extra_arm]
            rng.shuffle(first_arms)
            for repeat, first_arm in enumerate(first_arms, 1):
                first_by_pair[(case["id"], repeat)] = first_arm
    jobs = []
    for repeat in range(1, repeats + 1):
        for case in cases:
            first_arm = first_by_pair[(case["id"], repeat)]
            second_arm = "atlas" if first_arm == "beauty" else "beauty"
            jobs.extend(((case, repeat, first_arm), (case, repeat, second_arm)))
    return jobs


def run(args):
    repo_root = Path(args.repo_root).resolve()
    manifest = (repo_root / args.manifest).resolve() if not Path(args.manifest).is_absolute() else Path(args.manifest)
    rise_bin = (repo_root / args.rise_bin).resolve() if not Path(args.rise_bin).is_absolute() else Path(args.rise_bin)
    if not rise_bin.is_file():
        raise ValueError(f"RISE binary not found: {rise_bin}")
    harness = Path(__file__).resolve()
    start_provenance = input_provenance(repo_root, harness, manifest, rise_bin)
    manifest_bytes = manifest.read_bytes()
    if hashlib.sha256(manifest_bytes).hexdigest() != start_provenance["manifestSha256"]:
        raise RuntimeError("manifest changed while initial provenance was captured")
    cases = validate_cases(json.loads(manifest_bytes))
    endpoint = args.endpoint.rstrip("/")
    model_info = choose_model(endpoint, args.model)
    model = model_info.get("name") or model_info.get("model")
    ollama_version = http_json(endpoint + "/api/version", timeout=10).get("version")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run_dir = Path(args.output_dir) if args.output_dir else repo_root / "evals/runs" / f"perception_ab_{stamp}"
    if not run_dir.is_absolute():
        run_dir = repo_root / run_dir
    run_dir.mkdir(parents=True, exist_ok=False)
    image_dir = run_dir / "images"
    scene_dir = run_dir / "scenes"
    provenance_dir = run_dir / "provenance"
    provenance_dir.mkdir(parents=True)
    (provenance_dir / "perception_ab_eval.py").write_bytes(harness.read_bytes())
    (provenance_dir / "cases.json").write_bytes(manifest_bytes)
    prompts = {case["id"]: make_prompt(case) for case in cases}
    (provenance_dir / "prompts.json").write_text(
        json.dumps(prompts, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (provenance_dir / "start.json").write_text(
        json.dumps(start_provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"model={model} cases={len(cases)} repeats={args.repeats} output={run_dir}", flush=True)

    rendered = {}
    for index, case in enumerate(cases, 1):
        images, image_meta, scene_meta, render_ms = render_case(
            rise_bin, repo_root, case, args.edge, image_dir, scene_dir, args.render_timeout)
        rendered[case["id"]] = (images, image_meta, scene_meta, render_ms)
        print(f"render {index:02d}/{len(cases)} {case['id']} {render_ms}ms", flush=True)

    jobs = build_jobs(cases, args.repeats, args.order_seed)

    rows = []
    results_path = run_dir / "results.jsonl"
    with results_path.open("w", encoding="utf-8") as stream:
        for index, (case, repeat, arm) in enumerate(jobs, 1):
            images, image_meta, scene_meta, render_ms = rendered[case["id"]]
            prompt = prompts[case["id"]]
            response = ask_model(endpoint, model, prompt, images[arm],
                                 args.temperature, args.model_timeout)
            parsed = normalize_answer(response["content"], case["choices"])
            row = {
                "caseId": case["id"], "family": case["family"], "repeat": repeat,
                "arm": arm, "answer": case["answer"], "choices": case["choices"],
                "parsedAnswer": parsed, "correct": parsed == case["answer"],
                "rawContent": response["content"], "reasoning": response["reasoning"],
                "finishReason": response["finishReason"], "usage": response["usage"],
                "latencyMs": response["latencyMs"], "responseId": response["responseId"],
                "promptSha256": hashlib.sha256(prompt.encode("utf-8")).hexdigest(),
                "image": image_meta[arm], "scene": scene_meta, "renderLatencyMs": render_ms,
            }
            rows.append(row)
            stream.write(json.dumps(row, sort_keys=True) + "\n")
            stream.flush()
            mark = "PASS" if row["correct"] else "FAIL"
            print(f"model {index:03d}/{len(jobs)} {case['id']} r{repeat} {arm}: "
                  f"{parsed or '<invalid>'} {mark} {response['latencyMs']}ms", flush=True)

    end_provenance = input_provenance(repo_root, harness, manifest, rise_bin)
    (provenance_dir / "end.json").write_text(
        json.dumps(end_provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if end_provenance != start_provenance:
        raise RuntimeError("harness, manifest, RISE binary, or Git state changed during the run")
    summary = summarize(rows)
    metadata = {
        "schemaVersion": 1,
        "generatedAt": datetime.now(timezone.utc).isoformat(),
        "gitHead": start_provenance["gitHead"],
        "gitDirty": bool(start_provenance["gitStatus"]),
        "gitStatus": start_provenance["gitStatus"],
        "provenanceVerified": True,
        "harness": str(harness),
        "harnessSha256": start_provenance["harnessSha256"],
        "manifest": str(manifest),
        "manifestSha256": start_provenance["manifestSha256"],
        "riseBinary": str(rise_bin),
        "riseBinarySha256": start_provenance["riseBinarySha256"],
        "model": model,
        "digest": model_info.get("digest", ""),
        "modelSizeBytes": model_info.get("size"),
        "modelCapabilities": model_info.get("capabilities", []),
        "ollamaVersion": ollama_version,
        "endpoint": endpoint,
        "edge": args.edge,
        "temperature": args.temperature,
        "reasoningEffort": "none",
        "maxCompletionTokens": 32,
        "repeats": args.repeats,
        "caseCount": len(cases),
        "orderSeed": args.order_seed,
    }
    (run_dir / "summary.json").write_text(
        json.dumps({"metadata": metadata, "summary": summary}, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    report = report_markdown(metadata, summary)
    (run_dir / "REPORT.md").write_text(report, encoding="utf-8")
    print("\n" + report, flush=True)
    return 0


def self_test(repo_root, manifest_arg):
    def png_chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))

    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
    valid_png = (PNG_SIGNATURE + png_chunk(b"IHDR", ihdr) +
                 png_chunk(b"IDAT", zlib.compress(b"\x00\x00\x00\x00")) + png_chunk(b"IEND", b""))
    assert png_dimensions(valid_png) == (1, 1)
    try:
        png_dimensions(valid_png[:-1])
        raise AssertionError("truncated PNG accepted")
    except ValueError:
        pass
    assert normalize_answer(" red.\n", ["RED", "BLUE"]) == "RED"
    assert normalize_answer("RED because it is closer", ["RED", "BLUE"]) is None
    assert exact_mcnemar(0, 0) == 1.0
    assert exact_mcnemar(6, 0) == 0.03125
    manifest = Path(manifest_arg)
    if not manifest.is_absolute():
        manifest = Path(repo_root).resolve() / manifest
    cases = load_cases(manifest)
    assert len(cases) == 12
    for case in cases:
        scene = make_scene(case, 64)
        assert scene.startswith("RISE ASCII SCENE 7\n")
        assert "\nfilm\n" in scene and "\twidth 64\n" in scene
        prompt = make_prompt(case)
        assert case["question"] in prompt and all(choice in prompt for choice in case["choices"])
    try:
        validate_case_params("depth_order", {"nearColor": "red", "nearSide": "left",
                                             "nearZ": 9.5, "farZ": 0.0}, "bad_depth")
        raise AssertionError("camera-enclosing depth case accepted")
    except ValueError:
        pass
    jobs = build_jobs(cases, 3, 7)
    first_counts = defaultdict(lambda: defaultdict(int))
    case_counts = defaultdict(lambda: defaultdict(int))
    for index in range(0, len(jobs), 2):
        case, repeat, arm = jobs[index]
        assert jobs[index + 1][0] is case and jobs[index + 1][1] == repeat
        assert {arm, jobs[index + 1][2]} == {"beauty", "atlas"}
        first_counts[case["family"]][arm] += 1
        case_counts[case["id"]][arm] += 1
    assert all(counts["beauty"] == counts["atlas"] for counts in first_counts.values())
    assert all(abs(counts["beauty"] - counts["atlas"]) == 1 for counts in case_counts.values())
    synthetic = []
    for repeat in (1, 2):
        synthetic.append({"caseId": "a", "family": "depth_order", "repeat": repeat,
                          "arm": "beauty", "correct": False, "usage": {"prompt_tokens": 100}})
        synthetic.append({"caseId": "a", "family": "depth_order", "repeat": repeat,
                          "arm": "atlas", "correct": True, "usage": {"prompt_tokens": 100}})
    summary = summarize(synthetic)
    assert summary["paired"]["atlas_only_wins"] == 2
    assert summary["accuracyDelta"] == 1.0
    assert summary["tokenParity"]["withinFivePercent"] is True
    synthetic[-1]["usage"] = {}
    summary = summarize(synthetic)
    assert summary["tokenParity"]["pairsWithUsage"] == 1
    assert summary["tokenParity"]["withinFivePercent"] is False
    print("perception_ab_eval self-test: PASS")
    return 0


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=".")
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    parser.add_argument("--rise-bin", default="bin/rise")
    parser.add_argument("--endpoint", default=DEFAULT_ENDPOINT)
    parser.add_argument("--model", default="", help="installed vision model; default picks smallest")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--edge", type=int, default=DEFAULT_EDGE)
    parser.add_argument("--temperature", type=float, default=0.1)
    parser.add_argument("--order-seed", type=int, default=20260726)
    parser.add_argument("--render-timeout", type=int, default=300)
    parser.add_argument("--model-timeout", type=int, default=900)
    parser.add_argument("--output-dir", default="")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.repeats < 1 or args.repeats % 2 == 0:
        parser.error("--repeats must be a positive odd number for unambiguous case majorities")
    if args.edge < 64 or args.edge > 512:
        parser.error("--edge must be in [64,512]")
    if not math.isfinite(args.temperature) or args.temperature < 0.0 or args.temperature > 2.0:
        parser.error("--temperature must be finite and in [0,2]")
    if args.render_timeout <= 0 or args.model_timeout <= 0:
        parser.error("--render-timeout and --model-timeout must be positive")
    return args


if __name__ == "__main__":
    parsed_args = parse_args(sys.argv[1:])
    try:
        raise SystemExit(self_test(parsed_args.repo_root, parsed_args.manifest)
                         if parsed_args.self_test else run(parsed_args))
    except (ValueError, RuntimeError, OSError, json.JSONDecodeError) as exc:
        print(f"perception_ab_eval: ERROR: {exc}", file=sys.stderr)
        raise SystemExit(2)
