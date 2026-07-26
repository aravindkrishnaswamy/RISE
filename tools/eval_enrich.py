#!/usr/bin/env python3
"""eval_enrich.py -- post-process already-written RISE agent-eval (E4) run
directories into small per-cell summary artifacts.

This is a PURE post-processing tool. It never runs a model and never
changes the C++ eval runner (src/Library/Agent/AgentEvalRunner.cpp). The
only external process it may launch is `./bin/rise`, and only when asked
(`--render`), to turn a persisted final scene into a beauty PNG for human
review. No network access.

Consumes the on-disk layout written by `RunEvalMatrix` (see the header of
tools/eval_report.py for the full field-by-field description of that
layout). For a sweep rooted at <runDir>, one subdirectory ("cell") is
written per (scenario, provider, model, repeat):

    <runDir>/<scenarioId>__<provider>__<model>__r<repeat>/
        <scenarioId>.trajectory.jsonl   E1 trajectory, one JSON object per
                                         line, "run_type" in {session, user,
                                         llm, tool, history_edit, summary}.
        <scenarioId>.result.jsonl       one-line run result: scenarioId,
                                         terminalStatus, llmCalls, toolCalls,
                                         wallMs, provider, model, finalText, ...
        results.jsonl                   E3 checker output: scenarioId,
                                         checkpointFraction, allPassed,
                                         checkpoints[] = {kind,passed,weight,detail}
        <scenarioId>.final.RISEscene    the final assembled scene, re-loadable.
                                         PRESENT ONLY for runs made after this
                                         file was added to the runner; ABSENT
                                         for older runs.

This tool is pure-Python-3-stdlib (argparse, json, os, glob, re, subprocess,
tempfile, shutil, collections) -- no numpy/scipy/pandas, matching
tools/eval_report.py's standalone-machine constraint.

Subcommands
-----------
  enrich <path> [--render] [--samples N] [--force]
      <path> is either a single cell directory (one that directly contains
      a *.trajectory.jsonl) or a runDir tree (its immediate subdirectories
      are scanned for cells) -- auto-detected. For each cell, writes:

        <cell>/digest.json      a single JSON object: scenarioId, provider,
                                 model, terminalStatus, checkpointFraction,
                                 allPassed, checkpoints, a `tools` call-name
                                 histogram, an `edits` breakdown (patches by
                                 param, inserts by chunk keyword, rejections
                                 with reasons), a `renders` convergence
                                 series (meanR/G/B, luma, channelBalance per
                                 render call, in call order), a `scene`
                                 summary parsed from the final scene (object
                                 counts by chunk keyword, parsed lights) when
                                 the final scene file is present, `tokens`
                                 and `timing` roll-ups, and a `reasoning`
                                 availability summary.

        <cell>/reasoning.jsonl  ONLY IF the provider's raw LLM responses
                                 carried a reasoning/thinking field -- one
                                 line per turn that had one: {i, reasoning}.

        <cell>/final.beauty.png ONLY IF --render was passed AND the cell has
                                 a <scenarioId>.final.RISEscene. Renders via
                                 `./bin/rise` (skipped, never fatal, if
                                 bin/rise is missing or the render fails --
                                 the digest is the primary artifact). Skips
                                 an existing final.beauty.png unless --force.
                                 NOTE: objectmap/normals/depth/facets/
                                 wireframe rendering is not available via the
                                 CLI -- only a beauty (production) render.

        Example: python3 tools/eval_enrich.py enrich evals/runs/2026-07-20
        Example: python3 tools/eval_enrich.py enrich \\
            evals/runs/2026-07-20/build_watch_scene__gemini__gemini-3.6-flash__r1 \\
            --render --samples 64

  --selftest
      Builds a tiny synthetic cell directory under a temp dir and asserts
      every digest/reasoning field extracts correctly. No run directory
      needed; this is the only correctness check available for this file --
      the C++ test suite does not execute Python.
        Example: python3 tools/eval_enrich.py --selftest
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ---------------------------------------------------------------------------
# JSONL loading -- tolerant of malformed lines and missing files (mirrors
# tools/eval_report.py's load_jsonl).
# ---------------------------------------------------------------------------

def load_jsonl(path, warnings):
    """Return list of dicts parsed from a JSONL file. Missing file -> [].
    Malformed lines are skipped with a warning appended to `warnings`."""
    records = []
    if not os.path.isfile(path):
        return records
    try:
        with open(path, "r", encoding="utf-8") as f:
            for lineno, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    records.append(json.loads(line))
                except json.JSONDecodeError as e:
                    warnings.append(f"warning: {path}:{lineno}: malformed JSON skipped ({e})")
    except OSError as e:
        warnings.append(f"warning: could not read {path}: {e}")
    return records


# ---------------------------------------------------------------------------
# Cell discovery
# ---------------------------------------------------------------------------

def find_trajectory_file(cell_dir):
    """Return the single *.trajectory.jsonl path in cell_dir, or None."""
    candidates = sorted(glob.glob(os.path.join(cell_dir, "*.trajectory.jsonl")))
    return candidates[0] if candidates else None


def find_cells(path, warnings):
    """Auto-detect: a single cell dir (directly contains a *.trajectory.jsonl)
    or a runDir tree (glob its immediate subdirs for cells). Returns a list
    of cell directory paths, sorted."""
    path = os.path.abspath(path)
    if not os.path.isdir(path):
        warnings.append(f"warning: not a directory: {path}")
        return []
    if find_trajectory_file(path):
        return [path]
    cells = []
    try:
        entries = sorted(os.listdir(path))
    except OSError as e:
        warnings.append(f"warning: could not list {path}: {e}")
        return []
    for entry in entries:
        sub = os.path.join(path, entry)
        if os.path.isdir(sub) and find_trajectory_file(sub):
            cells.append(sub)
    if not cells:
        warnings.append(f"warning: no cells found under {path} (no *.trajectory.jsonl "
                         f"directly inside it or inside any immediate subdirectory)")
    return cells


# ---------------------------------------------------------------------------
# Reasoning extraction (ollama/qwen "reasoning", xAI/grok "reasoning_content";
# gpt/gemini carry neither -- see AgentChatCodecs.{h,cpp} for the per-provider
# response shapes this mirrors).
# ---------------------------------------------------------------------------

def extract_reasoning(response_body):
    """Return (field_name, text) if `response_body` (string or already-parsed
    object) carries a non-empty choices[0].message.reasoning or
    .reasoning_content, else (None, None). Never raises -- any shape mismatch
    is treated as "no reasoning this turn"."""
    try:
        if isinstance(response_body, str):
            obj = json.loads(response_body)
        elif isinstance(response_body, dict):
            obj = response_body
        else:
            return (None, None)
        choices = obj.get("choices")
        if not isinstance(choices, list) or not choices:
            return (None, None)
        message = choices[0].get("message")
        if not isinstance(message, dict):
            return (None, None)
        for field in ("reasoning", "reasoning_content"):
            val = message.get(field)
            if isinstance(val, str) and val.strip():
                return (field, val)
    except Exception:
        return (None, None)
    return (None, None)


# ---------------------------------------------------------------------------
# Scene parsing -- a lightweight, brace-depth chunk tokenizer over the
# RISEscene ASCII text. Not the real parser (src/Library/Cst/Cst.cpp); a
# best-effort reader good enough for object/light summaries.
# ---------------------------------------------------------------------------

_CHUNK_HEADER_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\{")


def parse_scene_chunks(text):
    """Return [(keyword, body), ...] for every top-level `keyword { ... }`
    block in `text`, in file order. Nested braces inside a body are consumed
    as part of that body (brace-depth counted), not re-emitted as their own
    top-level chunk."""
    chunks = []
    idx = 0
    n = len(text)
    while True:
        m = _CHUNK_HEADER_RE.search(text, idx)
        if not m:
            break
        keyword = m.group(1)
        brace_pos = m.end() - 1  # index of the matched '{'
        depth = 0
        j = brace_pos
        while j < n:
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        body = text[brace_pos + 1:j] if j < n else text[brace_pos + 1:]
        chunks.append((keyword, body))
        idx = j + 1 if j < n else n
    return chunks


def parse_light_body(body):
    """Best-effort (color, scale) extraction from a light/luminaire chunk
    body. `color` is the first `exitance`/`color`/`radiance` line's first 3
    numeric tokens; `scale` is the first `scale`/`power`/`intensity` line's
    first numeric token. Either is None if not found/unparseable."""
    color = None
    scale = None
    for raw_line in body.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        parts = line.split()
        if not parts:
            continue
        key = parts[0].lower()
        rest = parts[1:]
        if color is None and key in ("exitance", "color", "radiance"):
            try:
                vals = [float(x) for x in rest[:3]]
                if len(vals) == 3:
                    color = vals
            except ValueError:
                pass
        if scale is None and key in ("scale", "power", "intensity"):
            try:
                scale = float(rest[0])
            except (ValueError, IndexError):
                pass
    return (color, scale)


def summarize_scene(scene_text):
    chunks = parse_scene_chunks(scene_text)
    objects_by_kind = Counter(keyword for keyword, _ in chunks)
    lights = []
    for keyword, body in chunks:
        kl = keyword.lower()
        if "light" in kl or "luminaire" in kl:
            color, scale = parse_light_body(body)
            lights.append({"kind": keyword, "color": color, "scale": scale})
    return {"objectsByKind": dict(objects_by_kind), "lights": lights}


# ---------------------------------------------------------------------------
# Digest construction
# ---------------------------------------------------------------------------

def extract_rejection_item(tool_name, args, result):
    """Given a rejected tool call's args and jsonrpc `result` dict, return
    {"verb","param","value","reason"}. Pulls param/value/reason from
    result["issues"][0] when present (non-empty field wins over args), else
    falls back to args["param"]/args["value"] with reason left None."""
    args = args if isinstance(args, dict) else {}
    param = args.get("param")
    value = args.get("value")
    reason = None
    issues = result.get("issues") if isinstance(result, dict) else None
    if isinstance(issues, list) and issues and isinstance(issues[0], dict):
        issue0 = issues[0]
        reason = issue0.get("reason", reason)
        ip = issue0.get("param")
        if ip:
            param = ip
        iv = issue0.get("value")
        if iv is not None and iv != "":
            value = iv
    return {"verb": tool_name, "param": param, "value": value, "reason": reason}


def first_token(text):
    if not isinstance(text, str):
        return None
    stripped = text.strip()
    if not stripped:
        return None
    return stripped.split()[0]


def build_digest(cell_dir, warnings):
    traj_path = find_trajectory_file(cell_dir)
    if traj_path is None:
        warnings.append(f"warning: {cell_dir}: no *.trajectory.jsonl found, skipping")
        return None

    scenario_id = os.path.basename(traj_path)[: -len(".trajectory.jsonl")]
    traj_records = load_jsonl(traj_path, warnings)

    result_path = os.path.join(cell_dir, scenario_id + ".result.jsonl")
    result_lines = load_jsonl(result_path, warnings)
    result = result_lines[-1] if result_lines else {}

    results_path = os.path.join(cell_dir, "results.jsonl")
    check_lines = load_jsonl(results_path, warnings)
    matching_checks = [c for c in check_lines if c.get("scenarioId") == scenario_id]
    check = matching_checks[-1] if matching_checks else (check_lines[-1] if check_lines else {})

    provider = result.get("provider") or "unknown"
    model = result.get("model", "")
    terminal_status = result.get("terminalStatus")
    checkpoint_fraction = check.get("checkpointFraction")
    all_passed = check.get("allPassed")
    checkpoints = [
        {"kind": c.get("kind"), "passed": c.get("passed"), "detail": c.get("detail")}
        for c in (check.get("checkpoints") or [])
        if isinstance(c, dict)
    ]

    tools_hist = Counter()
    patches_by_param = Counter()
    inserts_by_kind = Counter()
    rejection_items = []
    rejection_by_reason = Counter()
    render_series = []

    llm_calls = 0
    input_total = 0
    output_total = 0
    cache_read_total = 0
    peak_input_tokens = 0
    sum_latency_ms = 0.0
    reasoning_lines = []
    reasoning_field_seen = None

    tool_ordinal = -1
    llm_ordinal = -1

    for rec in traj_records:
        rt = rec.get("run_type")
        latency = rec.get("latency_ms")
        if isinstance(latency, (int, float)):
            sum_latency_ms += latency

        if rt == "tool":
            tool_ordinal += 1
            name = rec.get("name")
            tools_hist[name] += 1
            args = rec.get("args") if isinstance(rec.get("args"), dict) else {}
            response = rec.get("jsonrpc.response")
            result_obj = response.get("result") if isinstance(response, dict) else None
            result_obj = result_obj if isinstance(result_obj, dict) else None

            if name == "propose_patch":
                param = args.get("param")
                if param:
                    patches_by_param[param] += 1
            elif name == "propose_patches":
                patches_list = args.get("patches")
                if isinstance(patches_list, list):
                    for p_item in patches_list:
                        if isinstance(p_item, dict):
                            param = p_item.get("param")
                            if param:
                                patches_by_param[param] += 1
            elif name == "insert_chunk":
                kw = first_token(args.get("chunkText"))
                inserts_by_kind[kw] += 1
            elif name == "render" and result_obj is not None and "meanR" in result_obj \
                    and "meanG" in result_obj and "meanB" in result_obj:
                try:
                    r = float(result_obj["meanR"])
                    g = float(result_obj["meanG"])
                    b = float(result_obj["meanB"])
                    luma = (r + g + b) / 3.0
                    channel_balance = max(r, g, b) / max(min(r, g, b), 1e-9)
                    render_series.append({
                        "i": tool_ordinal, "meanR": r, "meanG": g, "meanB": b,
                        "luma": luma, "channelBalance": channel_balance,
                    })
                except (TypeError, ValueError):
                    pass

            if result_obj is not None and result_obj.get("status") == "rejected":
                item = extract_rejection_item(name, args, result_obj)
                rejection_items.append(item)
                rejection_by_reason[item["reason"]] += 1

        elif rt == "llm":
            llm_ordinal += 1
            llm_calls += 1
            it = rec.get("gen_ai.usage.input_tokens")
            ot = rec.get("gen_ai.usage.output_tokens")
            ct = rec.get("gen_ai.usage.cache_read_input_tokens")
            if isinstance(it, (int, float)) and it > 0:
                input_total += it
                if it > peak_input_tokens:
                    peak_input_tokens = it
            if isinstance(ot, (int, float)) and ot > 0:
                output_total += ot
            if isinstance(ct, (int, float)) and ct > 0:
                cache_read_total += ct

            field, text = extract_reasoning(rec.get("response_body"))
            if field is not None:
                if reasoning_field_seen is None:
                    reasoning_field_seen = field
                reasoning_lines.append({"i": llm_ordinal, "reasoning": text})

    digest = {
        "scenarioId": scenario_id,
        "provider": provider,
        "model": model,
        "terminalStatus": terminal_status,
        "checkpointFraction": checkpoint_fraction,
        "allPassed": all_passed,
        "checkpoints": checkpoints,
        "tools": dict(tools_hist),
        "edits": {
            "patchesByParam": dict(patches_by_param),
            "insertsByKind": dict(inserts_by_kind),
            "rejections": {
                "count": len(rejection_items),
                "byReason": dict(rejection_by_reason),
                "items": rejection_items,
            },
        },
        "renders": {
            "count": len(render_series),
            "series": render_series,
        },
        "scene": None,
        "tokens": {
            "llmCalls": llm_calls,
            "inputTotal": input_total,
            "outputTotal": output_total,
            "cacheReadTotal": cache_read_total,
            "peakInputTokens": peak_input_tokens,
        },
        "timing": {
            "wallMs": result.get("wallMs"),
            "sumLatencyMs": sum_latency_ms,
        },
        "reasoning": {
            "available": bool(reasoning_lines),
            "provider_field": reasoning_field_seen,
            "turnsWithReasoning": len(reasoning_lines),
        },
    }

    final_scene_path = os.path.join(cell_dir, scenario_id + ".final.RISEscene")
    if os.path.isfile(final_scene_path):
        try:
            with open(final_scene_path, "r", encoding="utf-8") as f:
                scene_text = f.read()
            digest["scene"] = summarize_scene(scene_text)
        except OSError as e:
            warnings.append(f"warning: {final_scene_path}: could not read ({e})")

    return digest, reasoning_lines, scenario_id, final_scene_path


# ---------------------------------------------------------------------------
# --render
# ---------------------------------------------------------------------------

_SAMPLES_LINE_RE = re.compile(r"(?m)^(\s*samples\s+)[0-9.]+\s*$")


def rewrite_samples(scene_text, n):
    """Rewrite the `samples <n>` line inside the first *_rasterizer chunk.
    If that chunk has no samples line, one is appended just before its
    closing brace. If no *_rasterizer chunk exists at all, the text is
    returned unchanged (best-effort -- render will just use the scene's
    existing sample count)."""
    for m in _CHUNK_HEADER_RE.finditer(scene_text):
        keyword = m.group(1)
        if "rasterizer" not in keyword.lower():
            continue
        brace_pos = m.end() - 1
        depth = 0
        j = brace_pos
        n_chars = len(scene_text)
        while j < n_chars:
            if scene_text[j] == "{":
                depth += 1
            elif scene_text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        if j >= n_chars:
            continue
        body = scene_text[brace_pos + 1:j]
        new_body, count = _SAMPLES_LINE_RE.subn(lambda mm: mm.group(1) + str(n), body, count=1)
        if count == 0:
            new_body = body + f"\tsamples {n}\n"
        return scene_text[: brace_pos + 1] + new_body + scene_text[j:]
    return scene_text


def render_final_scene(cell_dir, scenario_id, final_scene_path, samples, force):
    """Render <cell_dir>/<scenarioId>.final.RISEscene to
    <cell_dir>/final.beauty.png via `./bin/rise`. Returns a short status
    string for the one-line summary; never raises -- a failure here must not
    abort the whole enrich pass."""
    out_png = os.path.join(cell_dir, "final.beauty.png")
    if os.path.isfile(out_png) and not force:
        return "render skipped (exists, use --force)"

    bin_rise = os.path.join(REPO_ROOT, "bin", "rise")
    if not os.path.isfile(bin_rise):
        return "render skipped (bin/rise not found -- build it first)"

    try:
        with open(final_scene_path, "r", encoding="utf-8") as f:
            scene_text = f.read()
    except OSError as e:
        return f"render failed (could not read final scene: {e})"

    if samples is not None:
        scene_text = rewrite_samples(scene_text, samples)

    # The temp dir MUST live under REPO_ROOT: the renderer resolves a
    # file_rasterizeroutput `pattern` against RISE_MEDIA_PATH (= REPO_ROOT),
    # prepending it -- so the pattern has to be RELATIVE to the repo root, and
    # an absolute /tmp path would be mangled into REPO_ROOT + "/private/tmp/..."
    # and fail to a `fro_temp_*.png` emergency file. Keeping media path at the
    # repo root also lets any texture the scene references still resolve.
    tmpdir = tempfile.mkdtemp(prefix=".eval_enrich_render_", dir=REPO_ROOT)
    try:
        tmp_scene_path = os.path.join(tmpdir, scenario_id + ".RISEscene")
        output_abs = os.path.join(tmpdir, "output")  # ./bin/rise appends ".png"
        output_base = os.path.relpath(output_abs, REPO_ROOT)  # media-path-relative pattern
        if not scene_text.endswith("\n"):
            scene_text += "\n"
        scene_text += (
            "\nfile_rasterizeroutput\n"
            "{\n"
            f"\tpattern {output_base}\n"
            "\ttype PNG\n"
            "\tbpp 8\n"
            "\tcolor_space sRGB\n"
            "}\n"
        )
        with open(tmp_scene_path, "w", encoding="utf-8") as f:
            f.write(scene_text)

        env = dict(os.environ)
        env["RISE_MEDIA_PATH"] = REPO_ROOT + "/"
        try:
            proc = subprocess.run(
                [bin_rise, tmp_scene_path],
                input="render\nquit\n",
                cwd=REPO_ROOT,
                env=env,
                timeout=180,
                capture_output=True,
                text=True,
            )
        except subprocess.TimeoutExpired:
            return "render failed (timed out after 180s)"
        except OSError as e:
            return f"render failed (could not launch bin/rise: {e})"

        produced = output_abs + ".png"
        if not os.path.isfile(produced):
            tail = (proc.stderr or proc.stdout or "").strip().splitlines()
            tail_str = tail[-1] if tail else f"exit code {proc.returncode}"
            return f"render failed (no output png: {tail_str})"

        shutil.move(produced, out_png)
        return "render ok"
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


# ---------------------------------------------------------------------------
# enrich subcommand
# ---------------------------------------------------------------------------

def enrich_cell(cell_dir, do_render, samples, force, warnings):
    built = build_digest(cell_dir, warnings)
    if built is None:
        return None
    digest, reasoning_lines, scenario_id, final_scene_path = built

    digest_path = os.path.join(cell_dir, "digest.json")
    with open(digest_path, "w", encoding="utf-8") as f:
        json.dump(digest, f, indent=2, sort_keys=False)
        f.write("\n")

    if reasoning_lines:
        reasoning_path = os.path.join(cell_dir, "reasoning.jsonl")
        with open(reasoning_path, "w", encoding="utf-8") as f:
            for line in reasoning_lines:
                f.write(json.dumps(line) + "\n")

    render_note = "render not requested"
    if do_render:
        if os.path.isfile(final_scene_path):
            render_note = render_final_scene(cell_dir, scenario_id, final_scene_path, samples, force)
        else:
            render_note = "render skipped (no .final.RISEscene in this cell)"

    cell_name = os.path.basename(cell_dir.rstrip(os.sep))
    print(f"{cell_name}: digest.json written, "
          f"{len(reasoning_lines)} reasoning turn(s), {render_note}")
    return digest


def cmd_enrich(args):
    warnings = []
    cells = find_cells(args.path, warnings)
    for w in warnings:
        print(w, file=sys.stderr)
    if not cells:
        return 1

    for cell in cells:
        cell_warnings = []
        enrich_cell(cell, args.render, args.samples, args.force, cell_warnings)
        for w in cell_warnings:
            print(w, file=sys.stderr)

    return 0


# ---------------------------------------------------------------------------
# --selftest
# ---------------------------------------------------------------------------

def _check(cond, msg, failures):
    if cond:
        print(f"PASS: {msg}")
    else:
        print(f"FAIL: {msg}")
        failures.append(msg)


def build_synthetic_cell(tmp_root):
    scenario_id = "selftest_scenario"
    cell_dir = os.path.join(tmp_root, f"{scenario_id}__local__qwen-test__r1")
    os.makedirs(cell_dir, exist_ok=True)

    traj_records = [
        {"trace_id": "t1", "dotted_order": "1", "run_type": "session",
         "provider": "local", "gen_ai.request.model": "qwen-test"},
        {"trace_id": "t1", "dotted_order": "2", "run_type": "user", "text": "build a scene", "attachments": 0},
        {
            "trace_id": "t1", "dotted_order": "3", "run_type": "llm",
            "gen_ai.request.model": "qwen-test",
            "gen_ai.usage.input_tokens": 100,
            "gen_ai.usage.output_tokens": 50,
            "gen_ai.usage.cache_read_input_tokens": 10,
            "latency_ms": 500,
            "response_body": json.dumps({
                "choices": [{"message": {"reasoning": "thinking about it", "content": "ok"}}]
            }),
        },
        {
            "trace_id": "t1", "dotted_order": "4", "run_type": "tool", "name": "insert_chunk",
            "args": {"chunkText": "omni_light\n{\n\tname sun\n\tcolor 1 0.9 0.8\n\tpower 2.5\n}"},
            "jsonrpc.response": {"result": {"applied": True, "status": "applied"}},
            "latency_ms": 5,
        },
        {
            "trace_id": "t1", "dotted_order": "5", "run_type": "tool", "name": "insert_chunk",
            "args": {"chunkText": "lambertian_material\n{\n\tname mat1\n}"},
            "jsonrpc.response": {"result": {"applied": True, "status": "applied"}},
            "latency_ms": 4,
        },
        {
            "trace_id": "t1", "dotted_order": "6", "run_type": "tool", "name": "propose_patch",
            "args": {"target": "pinhole_camera", "param": "fov", "value": "28.0"},
            "jsonrpc.response": {"result": {
                "applied": False, "status": "rejected",
                "issues": [{"param": "", "value": "pinhole_camera", "reason": "unknown_target", "suggestions": []}],
            }},
            "latency_ms": 3,
        },
        {
            "trace_id": "t1", "dotted_order": "7", "run_type": "tool", "name": "render",
            "args": {"width": 64, "height": 48},
            "jsonrpc.response": {"result": {
                "ok": True, "meanR": 0.5, "meanG": 0.3, "meanB": 0.2,
            }},
            "latency_ms": 20,
        },
        {"trace_id": "t1", "dotted_order": "8", "run_type": "summary", "wall_ms": 1234,
         "n_tool_calls": 4, "status": "final_text"},
    ]
    with open(os.path.join(cell_dir, f"{scenario_id}.trajectory.jsonl"), "w", encoding="utf-8") as f:
        for rec in traj_records:
            f.write(json.dumps(rec) + "\n")

    result = {
        "scenarioId": scenario_id, "terminalStatus": "final_text",
        "llmCalls": 1, "toolCalls": 4, "wallMs": 1234,
        "provider": "local", "model": "qwen-test", "finalText": "done",
    }
    with open(os.path.join(cell_dir, f"{scenario_id}.result.jsonl"), "w", encoding="utf-8") as f:
        f.write(json.dumps(result) + "\n")

    check = {
        "scenarioId": scenario_id, "checkpointFraction": 0.5, "allPassed": False,
        "checkpoints": [
            {"kind": "document", "passed": True, "weight": 1, "detail": "ok"},
            {"kind": "render", "passed": False, "weight": 1, "detail": "too bright"},
        ],
    }
    with open(os.path.join(cell_dir, "results.jsonl"), "w", encoding="utf-8") as f:
        f.write(json.dumps(check) + "\n")

    final_scene = (
        "RISE ASCII SCENE 7\n"
        "omni_light\n"
        "{\n"
        "\tname\tsun\n"
        "\tcolor\t1.0 0.9 0.8\n"
        "\tpower\t2.5\n"
        "}\n"
    )
    with open(os.path.join(cell_dir, f"{scenario_id}.final.RISEscene"), "w", encoding="utf-8") as f:
        f.write(final_scene)

    return cell_dir


def selftest():
    failures = []
    tmp_root = tempfile.mkdtemp(prefix="eval_enrich_selftest_")
    try:
        cell_dir = build_synthetic_cell(tmp_root)
        warnings = []
        digest = enrich_cell(cell_dir, do_render=False, samples=None, force=False, warnings=warnings)
        for w in warnings:
            print(w, file=sys.stderr)

        digest_path = os.path.join(cell_dir, "digest.json")
        _check(os.path.isfile(digest_path), "digest.json exists", failures)

        with open(digest_path, "r", encoding="utf-8") as f:
            on_disk = json.load(f)
        _check(on_disk == digest, "digest.json on disk matches returned digest", failures)

        tools = digest.get("tools", {})
        _check(tools.get("insert_chunk") == 2, f"tools.insert_chunk == 2 (got {tools.get('insert_chunk')})", failures)
        _check(tools.get("propose_patch") == 1, f"tools.propose_patch == 1 (got {tools.get('propose_patch')})", failures)
        _check(tools.get("render") == 1, f"tools.render == 1 (got {tools.get('render')})", failures)

        rejections = digest.get("edits", {}).get("rejections", {})
        _check(rejections.get("count") == 1, f"rejections.count == 1 (got {rejections.get('count')})", failures)
        _check(rejections.get("byReason", {}).get("unknown_target") == 1,
               f"rejections.byReason.unknown_target == 1 (got {rejections.get('byReason')})", failures)
        items = rejections.get("items", [])
        _check(len(items) == 1 and items[0].get("reason") == "unknown_target",
               f"rejections.items[0].reason == 'unknown_target' (got {items})", failures)

        series = digest.get("renders", {}).get("series", [])
        _check(len(series) == 1, f"renders.series has 1 entry (got {len(series)})", failures)
        if series:
            entry = series[0]
            expected_balance = max(0.5, 0.3, 0.2) / max(min(0.5, 0.3, 0.2), 1e-9)
            _check(abs(entry.get("channelBalance", -1) - expected_balance) < 1e-9,
                   f"renders.series[0].channelBalance == {expected_balance} (got {entry.get('channelBalance')})",
                   failures)

        scene = digest.get("scene") or {}
        lights = scene.get("lights", [])
        _check(len(lights) == 1, f"scene.lights has 1 entry (got {len(lights)})", failures)
        if lights:
            _check(lights[0].get("color") == [1.0, 0.9, 0.8],
                   f"scene.lights[0].color == [1.0, 0.9, 0.8] (got {lights[0].get('color')})", failures)

        reasoning = digest.get("reasoning", {})
        _check(reasoning.get("available") is True, f"reasoning.available == True (got {reasoning.get('available')})", failures)

        reasoning_path = os.path.join(cell_dir, "reasoning.jsonl")
        _check(os.path.isfile(reasoning_path), "reasoning.jsonl exists", failures)
        if os.path.isfile(reasoning_path):
            with open(reasoning_path, "r", encoding="utf-8") as f:
                lines = [json.loads(l) for l in f if l.strip()]
            _check(len(lines) == 1 and lines[0].get("reasoning") == "thinking about it",
                   f"reasoning.jsonl has 1 line with the reasoning text (got {lines})", failures)
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)

    if failures:
        print(f"\neval_enrich selftest: FAIL ({len(failures)} assertion(s) failed)")
        return 1
    print("\neval_enrich selftest: ALL PASS")
    return 0


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def build_arg_parser():
    parser = argparse.ArgumentParser(
        prog="eval_enrich.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--selftest", action="store_true",
        help="Run in-process unit checks against a synthetic cell and exit (no run dir needed).",
    )
    subparsers = parser.add_subparsers(dest="command")

    p_enrich = subparsers.add_parser(
        "enrich", help="Write digest.json (+ optional reasoning.jsonl / final.beauty.png) per cell.")
    p_enrich.add_argument("path", help="A single cell directory, or a runDir tree of cells.")
    p_enrich.add_argument("--render", action="store_true",
                           help="Also render <scenarioId>.final.RISEscene to final.beauty.png via ./bin/rise "
                                "(skipped per-cell, never fatal, if bin/rise or the final scene is missing). "
                                "Beauty (production) render only -- objectmap/normals/depth/facets/wireframe "
                                "are not available via the CLI.")
    p_enrich.add_argument("--samples", type=int, default=None,
                           help="Rewrite the final scene's pathtracing `samples` line to N before rendering "
                                "(higher = cleaner analysis image). Only meaningful with --render.")
    p_enrich.add_argument("--force", action="store_true",
                           help="Re-render even if final.beauty.png already exists.")

    return parser


def main(argv=None):
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    if args.command == "enrich":
        return cmd_enrich(args)

    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
