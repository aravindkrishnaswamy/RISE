#!/usr/bin/env python3
"""eval_report.py -- aggregate + compare RISE agent-eval (E4) live-runner output.

Consumes the on-disk layout written by `RunEvalMatrix` in
src/Library/Agent/AgentEvalRunner.cpp.  For a sweep rooted at <runDir>, one
subdirectory is written per (scenario, provider, model, repeat):

    <runDir>/<scenarioId>__<provider>[__<model>]__r<repeat>/
        <scenarioId>.trajectory.jsonl   E1 trajectory (one JSON object/line,
                                         field "run_type" in
                                         {session,user,llm,tool,history_edit,summary};
                                         see src/Library/Agent/ChatTrajectory.cpp)
        <scenarioId>.result.jsonl       one-line run result: scenarioId,
                                         terminalStatus, llmCalls, toolCalls,
                                         budgetHit, headVersionStart,
                                         headVersionFinal, finalText,
                                         optional errorMessage
                                         (AgentEvalRunner.cpp ~line 584-601)
        results.jsonl                   E3 checker output, appended: scenarioId,
                                         checkpointFraction, allPassed,
                                         checkpoints[] = {kind,passed,weight,detail}
                                         (AgentEvalRunner.cpp ~line 1520-1544)

Directory-name separator is literally "__"; the model segment is present only
when the provider config supplied a non-empty model string; "r<repeat>" is
1-based and NOT zero-padded (e.g. "myScenario__anthropic__claude-3-5-sonnet__r1",
or "myScenario__openai__r2" when no model was set).

This tool is pure-Python-3-stdlib (json, argparse, math, os, glob, collections)
-- no numpy/scipy/pandas -- since Python tooling is not part of the RISE make
build and must run standalone on any dev machine.

Subcommands
-----------
  report <runDir> [--json] [--markdown]
      Aggregate every run subdirectory under <runDir>, grouped by
      (scenarioId, provider, model).  Prints pass@1, pass^k (all-repeats-pass
      reliability), a 95% Wilson score interval on pass@1, mean partial-credit
      checkpointFraction, an estimated $-per-successful-task cost, and a
      terminalStatus failure-label breakdown per group.
        Example: python3 tools/eval_report.py report evals/runs/2026-07-10

  diff <trajectoryA.jsonl> <trajectoryB.jsonl>
      Align two E1 trajectory files record-by-record (ordered by
      trace_id + dotted_order) and print where they diverge: tool-call
      sequence, llm-round count, terminalStatus, and finalText (read from each
      trajectory's sibling *.result.jsonl, if present in the same directory).
        Example: python3 tools/eval_report.py diff \
            runs/s1__anthropic__r1/s1.trajectory.jsonl \
            runs/s1__openai__r1/s1.trajectory.jsonl

  --selftest
      Runs in-process unit checks of the pure math (Wilson interval, pass^k,
      pass@1, cost-per-success) against hand-computed synthetic values.  No
      runDir needed.  This is the only correctness check available for this
      file -- the C++ test suite does not execute Python.
        Example: python3 tools/eval_report.py --selftest
"""

import argparse
import difflib
import glob
import json
import math
import os
import sys
from collections import Counter, defaultdict

# ---------------------------------------------------------------------------
# Pricing table
#
# PRICES AS OF 2026-07, PUBLIC LIST PRICES, EDIT ME; cost is an ESTIMATE.
#
# Keyed by provider -> ("_default" plus optional per-model overrides).  Model
# lookup is a case-insensitive substring match against the model key (e.g. the
# key "claude-3-5-sonnet" matches an actual model string of
# "claude-3-5-sonnet-20241022"); the most specific (longest) matching key
# wins.  All prices are USD per 1,000,000 tokens.  "cached_input" is the rate
# charged for cache-read input tokens (gen_ai.usage.cache_read_input_tokens);
# providers that don't distinguish a cached rate should set it equal to
# "input".  A provider/model combination absent from this table (or an
# unrecognized provider) makes cost report as "n/a (no pricing)" -- it never
# crashes the tool.
# ---------------------------------------------------------------------------
PROVIDER_PRICING = {
    "anthropic": {
        "_default": {"input": 3.00, "output": 15.00, "cached_input": 0.30},
        "claude-opus": {"input": 15.00, "output": 75.00, "cached_input": 1.50},
        "claude-sonnet": {"input": 3.00, "output": 15.00, "cached_input": 0.30},
        "claude-haiku": {"input": 0.80, "output": 4.00, "cached_input": 0.08},
    },
    "openai": {
        "_default": {"input": 2.50, "output": 10.00, "cached_input": 1.25},
        "gpt-4o-mini": {"input": 0.15, "output": 0.60, "cached_input": 0.075},
        "gpt-4o": {"input": 2.50, "output": 10.00, "cached_input": 1.25},
        "gpt-4.1-mini": {"input": 0.40, "output": 1.60, "cached_input": 0.10},
        "gpt-4.1": {"input": 2.00, "output": 8.00, "cached_input": 0.50},
    },
    "gemini": {
        "_default": {"input": 1.25, "output": 5.00, "cached_input": 0.3125},
        "gemini-1.5-flash": {"input": 0.075, "output": 0.30, "cached_input": 0.01875},
        "gemini-1.5-pro": {"input": 1.25, "output": 5.00, "cached_input": 0.3125},
        "gemini-2.0-flash": {"input": 0.10, "output": 0.40, "cached_input": 0.025},
    },
}

KNOWN_PROVIDERS = ("anthropic", "openai", "gemini")

WILSON_Z = 1.96


# ---------------------------------------------------------------------------
# Pure math helpers (covered by --selftest)
# ---------------------------------------------------------------------------

def wilson_score_interval(successes, n, z=WILSON_Z):
    """95% (default z=1.96) Wilson score interval on a binomial proportion.

    Returns (lower, upper), both None if n == 0.
    """
    if n <= 0:
        return (None, None)
    phat = successes / n
    denom = 1.0 + z * z / n
    centre = phat + z * z / (2 * n)
    adj = math.sqrt((phat * (1 - phat) + z * z / (4 * n)) / n)
    lower = (centre - z * adj) / denom
    upper = (centre + z * adj) / denom
    return (max(0.0, lower), min(1.0, upper))


def pass_at_1(successes, n):
    """Mean success rate over N repeats. None if n == 0."""
    if n <= 0:
        return None
    return successes / n


def pass_at_k(successes, n):
    """All-repeats-pass reliability indicator: 1 if successes == n (n>0), else 0."""
    if n <= 0:
        return 0
    return 1 if successes == n else 0


def lookup_pricing(provider, model):
    """Return {"input","output","cached_input"} USD/1M-token dict, or None
    if the provider/model isn't in PROVIDER_PRICING."""
    prov_table = PROVIDER_PRICING.get(provider)
    if prov_table is None:
        return None
    model_l = (model or "").lower()
    best_key = None
    for key in prov_table:
        if key == "_default":
            continue
        if key.lower() in model_l:
            if best_key is None or len(key) > len(best_key):
                best_key = key
    if best_key is not None:
        return prov_table[best_key]
    return prov_table.get("_default")


def estimate_cost(provider, model, input_tokens, output_tokens, cached_tokens):
    """Return (cost_usd, priced_bool). priced_bool False -> no pricing found."""
    price = lookup_pricing(provider, model)
    if price is None:
        return (0.0, False)
    cost = (
        input_tokens / 1_000_000.0 * price["input"]
        + output_tokens / 1_000_000.0 * price["output"]
        + cached_tokens / 1_000_000.0 * price["cached_input"]
    )
    return (cost, True)


def cost_per_success(total_cost_usd, priced, successes):
    if not priced:
        return "n/a (no pricing)"
    if successes <= 0:
        return "n/a"
    return total_cost_usd / successes


# ---------------------------------------------------------------------------
# JSONL loading -- tolerant of malformed lines and missing files
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
                    warnings.append(
                        f"warning: {path}:{lineno}: malformed JSON skipped ({e})"
                    )
    except OSError as e:
        warnings.append(f"warning: could not read {path}: {e}")
    return records


# ---------------------------------------------------------------------------
# Run-directory name parsing:
#   <scenarioId>__<provider>[__<model>]__r<repeat>
# provider is always one of KNOWN_PROVIDERS (post-sanitization it is
# unchanged, since those strings are already alnum-only), so we locate it by
# scanning tokens for a known provider name. This tolerates scenarioIds that
# themselves contain "__" (from sanitized whitespace/punctuation).
# ---------------------------------------------------------------------------

def parse_run_dir_name(leaf):
    tokens = leaf.split("__")
    if len(tokens) < 2:
        return None
    last = tokens[-1]
    if not (last.startswith("r") and last[1:].isdigit()):
        return None
    rep = int(last[1:])
    provider_idx = None
    for i in range(1, len(tokens) - 1):
        if tokens[i] in KNOWN_PROVIDERS:
            provider_idx = i
            break
    if provider_idx is None:
        # Fall back: assume no model segment, provider is second-to-last token.
        if len(tokens) < 3:
            return None
        provider = tokens[-2]
        scenario_id = "__".join(tokens[:-2])
        model = ""
    else:
        scenario_id = "__".join(tokens[:provider_idx])
        provider = tokens[provider_idx]
        remaining = tokens[provider_idx + 1 : -1]
        model = "__".join(remaining) if remaining else ""
    if not scenario_id:
        return None
    return (scenario_id, provider, model, rep)


# ---------------------------------------------------------------------------
# Per-run-subdir data extraction
# ---------------------------------------------------------------------------

class RunRecord:
    __slots__ = (
        "dir_path", "scenario_id", "provider", "model", "rep",
        "result", "check", "input_tokens", "output_tokens", "cached_tokens",
        "notes",
    )

    def __init__(self):
        self.dir_path = None
        self.scenario_id = None
        self.provider = None
        self.model = None
        self.rep = None
        self.result = None      # dict or None
        self.check = None       # dict or None (last matching results.jsonl line)
        self.input_tokens = 0
        self.output_tokens = 0
        self.cached_tokens = 0
        self.notes = []


def scan_run_dir(run_dir, warnings):
    """Return list of RunRecord for every parseable run subdirectory under run_dir."""
    out = []
    try:
        entries = sorted(os.listdir(run_dir))
    except OSError as e:
        warnings.append(f"warning: could not list {run_dir}: {e}")
        return out

    for leaf in entries:
        sub = os.path.join(run_dir, leaf)
        if not os.path.isdir(sub):
            continue
        parsed = parse_run_dir_name(leaf)
        if parsed is None:
            warnings.append(f"warning: could not parse run-dir name, skipping: {leaf}")
            continue
        scenario_id, provider, model, rep = parsed

        rec = RunRecord()
        rec.dir_path = sub
        rec.scenario_id = scenario_id
        rec.provider = provider
        rec.model = model
        rec.rep = rep

        result_path = os.path.join(sub, scenario_id + ".result.jsonl")
        result_lines = load_jsonl(result_path, warnings)
        if result_lines:
            rec.result = result_lines[-1]
        else:
            rec.notes.append("missing .result.jsonl")

        check_path = os.path.join(sub, "results.jsonl")
        check_lines = load_jsonl(check_path, warnings)
        matching = [c for c in check_lines if c.get("scenarioId") == scenario_id]
        if matching:
            rec.check = matching[-1]
        elif check_lines:
            # results.jsonl exists but no line matches this scenarioId.
            rec.check = check_lines[-1]
            rec.notes.append("results.jsonl present but scenarioId mismatch")
        else:
            rec.notes.append("missing results.jsonl (check result)")

        traj_path = os.path.join(sub, scenario_id + ".trajectory.jsonl")
        traj_lines = load_jsonl(traj_path, warnings)
        if not traj_lines:
            rec.notes.append("missing/empty .trajectory.jsonl")
        for line in traj_lines:
            if line.get("run_type") != "llm":
                continue
            it = line.get("gen_ai.usage.input_tokens", -1)
            ot = line.get("gen_ai.usage.output_tokens", -1)
            ct = line.get("gen_ai.usage.cache_read_input_tokens", -1)
            if isinstance(it, (int, float)) and it > 0:
                rec.input_tokens += it
            if isinstance(ot, (int, float)) and ot > 0:
                rec.output_tokens += ot
            if isinstance(ct, (int, float)) and ct > 0:
                rec.cached_tokens += ct

        for note in rec.notes:
            warnings.append(f"warning: {sub}: {note}")

        out.append(rec)
    return out


# ---------------------------------------------------------------------------
# Grouping + stats
# ---------------------------------------------------------------------------

def group_key(rec):
    return (rec.scenario_id, rec.provider, rec.model)


def build_groups(records):
    groups = defaultdict(list)
    for r in records:
        groups[group_key(r)].append(r)
    return groups


def compute_group_stats(key, recs):
    scenario_id, provider, model = key
    n = len(recs)
    successes = 0
    checkpoint_fracs = []
    status_counter = Counter()
    total_input = total_output = total_cached = 0

    for r in recs:
        all_passed = False
        if r.check is not None and isinstance(r.check.get("allPassed"), bool):
            all_passed = r.check["allPassed"]
        if all_passed:
            successes += 1

        if r.check is not None and isinstance(r.check.get("checkpointFraction"), (int, float)):
            checkpoint_fracs.append(r.check["checkpointFraction"])
        else:
            checkpoint_fracs.append(0.0)

        if r.result is not None and isinstance(r.result.get("terminalStatus"), str):
            status_counter[r.result["terminalStatus"]] += 1
        else:
            status_counter["<missing result>"] += 1

        total_input += r.input_tokens
        total_output += r.output_tokens
        total_cached += r.cached_tokens

    p1 = pass_at_1(successes, n)
    pk = pass_at_k(successes, n)
    lower, upper = wilson_score_interval(successes, n)
    mean_ckpt = (sum(checkpoint_fracs) / len(checkpoint_fracs)) if checkpoint_fracs else None

    cost, priced = estimate_cost(provider, model, total_input, total_output, total_cached)
    cps = cost_per_success(cost, priced, successes)

    return {
        "scenarioId": scenario_id,
        "provider": provider,
        "model": model,
        "n": n,
        "successes": successes,
        "pass_at_1": p1,
        "pass_at_k": pk,
        "wilson_lower": lower,
        "wilson_upper": upper,
        "mean_checkpoint_fraction": mean_ckpt,
        "total_input_tokens": total_input,
        "total_output_tokens": total_output,
        "total_cached_tokens": total_cached,
        "estimated_cost_usd": cost if priced else None,
        "cost_per_success": cps,
        "terminal_status_counts": dict(status_counter),
    }


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def fmt_pct(x):
    return "n/a" if x is None else f"{x * 100:.1f}%"


def fmt_ci(lower, upper):
    if lower is None or upper is None:
        return "n/a"
    return f"[{lower * 100:.1f}%, {upper * 100:.1f}%]"


def fmt_cost(x):
    if isinstance(x, str):
        return x
    return f"${x:.4f}"


def fmt_status_counts(counts):
    return " ".join(f"{k}:{v}" for k, v in sorted(counts.items())) if counts else "-"


def render_text_report(stats_list, warnings):
    headers = [
        "scenario", "provider", "model", "N", "pass@1", "wilson95%",
        "pass^k", "meanCkpt", "$/success", "failureBreakdown",
    ]
    rows = []
    for s in stats_list:
        rows.append([
            s["scenarioId"], s["provider"], s["model"] or "-",
            str(s["n"]), fmt_pct(s["pass_at_1"]),
            fmt_ci(s["wilson_lower"], s["wilson_upper"]),
            str(s["pass_at_k"]),
            "n/a" if s["mean_checkpoint_fraction"] is None else f"{s['mean_checkpoint_fraction']:.3f}",
            fmt_cost(s["cost_per_success"]),
            fmt_status_counts(s["terminal_status_counts"]),
        ])

    widths = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(cell))

    def fmt_row(cells):
        return "  ".join(c.ljust(widths[i]) for i, c in enumerate(cells))

    lines = []
    lines.append(fmt_row(headers))
    lines.append("  ".join("-" * w for w in widths))
    for row in rows:
        lines.append(fmt_row(row))

    # Overall roll-up.
    total_n = sum(s["n"] for s in stats_list)
    total_succ = sum(s["successes"] for s in stats_list)
    groups_fully_reliable = sum(1 for s in stats_list if s["pass_at_k"] == 1)
    overall_p1 = pass_at_1(total_succ, total_n)
    lower, upper = wilson_score_interval(total_succ, total_n)
    lines.append("")
    lines.append(
        f"OVERALL: {len(stats_list)} groups, {total_n} repeats total, "
        f"{total_succ} successes, pass@1={fmt_pct(overall_p1)} {fmt_ci(lower, upper)}, "
        f"{groups_fully_reliable}/{len(stats_list)} groups fully reliable (pass^k=1)"
    )

    if warnings:
        lines.append("")
        lines.append(f"{len(warnings)} warning(s):")
        lines.extend(f"  {w}" for w in warnings)

    return "\n".join(lines)


def render_markdown_report(stats_list, warnings):
    headers = [
        "scenario", "provider", "model", "N", "pass@1", "wilson95%",
        "pass^k", "meanCkpt", "$/success", "failureBreakdown",
    ]
    lines = []
    lines.append("| " + " | ".join(headers) + " |")
    lines.append("|" + "|".join(["---"] * len(headers)) + "|")
    for s in stats_list:
        row = [
            s["scenarioId"], s["provider"], s["model"] or "-",
            str(s["n"]), fmt_pct(s["pass_at_1"]),
            fmt_ci(s["wilson_lower"], s["wilson_upper"]),
            str(s["pass_at_k"]),
            "n/a" if s["mean_checkpoint_fraction"] is None else f"{s['mean_checkpoint_fraction']:.3f}",
            fmt_cost(s["cost_per_success"]),
            fmt_status_counts(s["terminal_status_counts"]).replace("|", "\\|"),
        ]
        lines.append("| " + " | ".join(row) + " |")

    total_n = sum(s["n"] for s in stats_list)
    total_succ = sum(s["successes"] for s in stats_list)
    groups_fully_reliable = sum(1 for s in stats_list if s["pass_at_k"] == 1)
    overall_p1 = pass_at_1(total_succ, total_n)
    lower, upper = wilson_score_interval(total_succ, total_n)
    lines.append("")
    lines.append(
        f"**OVERALL**: {len(stats_list)} groups, {total_n} repeats total, "
        f"{total_succ} successes, pass@1={fmt_pct(overall_p1)} {fmt_ci(lower, upper)}, "
        f"{groups_fully_reliable}/{len(stats_list)} groups fully reliable (pass^k=1)"
    )
    if warnings:
        lines.append("")
        lines.append(f"{len(warnings)} warning(s):")
        for w in warnings:
            lines.append(f"- {w}")
    return "\n".join(lines)


def render_json_report(stats_list, warnings):
    return json.dumps({"groups": stats_list, "warnings": warnings}, indent=2, sort_keys=False)


# ---------------------------------------------------------------------------
# report subcommand
# ---------------------------------------------------------------------------

def cmd_report(args):
    warnings = []
    records = scan_run_dir(args.runDir, warnings)
    groups = build_groups(records)

    stats_list = [
        compute_group_stats(key, recs) for key, recs in sorted(groups.items())
    ]

    if args.json:
        print(render_json_report(stats_list, warnings))
    elif args.markdown:
        print(render_markdown_report(stats_list, warnings))
    else:
        print(render_text_report(stats_list, warnings))
    return 0


# ---------------------------------------------------------------------------
# diff subcommand
# ---------------------------------------------------------------------------

def trajectory_record_label(rec):
    rt = rec.get("run_type", "<unknown>")
    if rt == "session":
        return f"session provider={rec.get('provider','?')} model={rec.get('gen_ai.request.model','?')}"
    if rt == "user":
        return f"user text_len={len(rec.get('text',''))} attachments={rec.get('attachments',0)}"
    if rt == "llm":
        return (
            f"llm model={rec.get('gen_ai.request.model','?')} "
            f"in={rec.get('gen_ai.usage.input_tokens','?')} "
            f"out={rec.get('gen_ai.usage.output_tokens','?')} "
            f"finish={rec.get('gen_ai.response.finish_reasons','?')}"
            + (f" error={rec['error.type']}" if "error.type" in rec else "")
        )
    if rt == "tool":
        return f"tool name={rec.get('name','?')} error={rec.get('error', False)}"
    if rt == "history_edit":
        return f"history_edit reason={rec.get('reason','?')}"
    if rt == "summary":
        return f"summary status={rec.get('status','?')} n_turns={rec.get('n_turns','?')} n_tool_calls={rec.get('n_tool_calls','?')}"
    return f"{rt} {rec}"


def load_trajectory(path, warnings):
    records = load_jsonl(path, warnings)
    # Records already arrive in file order (== dotted_order chronological
    # order, by construction of the writer), but sort defensively in case a
    # file was hand-edited or concatenated out of order.
    def sort_key(rec):
        return rec.get("dotted_order", "")
    return sorted(records, key=sort_key)


def sibling_result(traj_path):
    """Given <dir>/<scenarioId>.trajectory.jsonl, return the parsed
    <scenarioId>.result.jsonl in the same directory, or None."""
    if not traj_path.endswith(".trajectory.jsonl"):
        return None
    result_path = traj_path[: -len(".trajectory.jsonl")] + ".result.jsonl"
    warnings = []
    lines = load_jsonl(result_path, warnings)
    return lines[-1] if lines else None


def cmd_diff(args):
    warnings = []
    a_records = load_trajectory(args.trajA, warnings)
    b_records = load_trajectory(args.trajB, warnings)

    a_labels = [trajectory_record_label(r) for r in a_records]
    b_labels = [trajectory_record_label(r) for r in b_records]

    print(f"--- A: {args.trajA} ({len(a_records)} records)")
    print(f"+++ B: {args.trajB} ({len(b_records)} records)")
    print()

    sm = difflib.SequenceMatcher(a=a_labels, b=b_labels, autojunk=False)
    any_diff = False
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        any_diff = True
        print(f"@@ A[{i1}:{i2}] vs B[{j1}:{j2}] ({tag}) @@")
        for line in a_labels[i1:i2]:
            print(f"- {line}")
        for line in b_labels[j1:j2]:
            print(f"+ {line}")
        print()

    if not any_diff:
        print("(record sequences are identical)")
        print()

    # Explicit high-signal comparisons.
    a_llm_rounds = sum(1 for r in a_records if r.get("run_type") == "llm")
    b_llm_rounds = sum(1 for r in b_records if r.get("run_type") == "llm")
    print(f"llm rounds: A={a_llm_rounds} B={b_llm_rounds}"
          + ("  <-- DIFFERS" if a_llm_rounds != b_llm_rounds else ""))

    a_summary = next((r for r in reversed(a_records) if r.get("run_type") == "summary"), None)
    b_summary = next((r for r in reversed(b_records) if r.get("run_type") == "summary"), None)
    a_status = a_summary.get("status") if a_summary else None
    b_status = b_summary.get("status") if b_summary else None
    print(f"terminalStatus (trajectory summary.status): A={a_status} B={b_status}"
          + ("  <-- DIFFERS" if a_status != b_status else ""))

    a_res = sibling_result(args.trajA)
    b_res = sibling_result(args.trajB)
    a_final = a_res.get("finalText") if a_res else None
    b_final = b_res.get("finalText") if b_res else None
    if a_res is None or b_res is None:
        print("finalText: sibling *.result.jsonl not found for one or both trajectories -- skipped")
    elif a_final == b_final:
        print("finalText: identical")
    else:
        print("finalText: DIFFERS")
        print(f"  A: {a_final!r}")
        print(f"  B: {b_final!r}")

    if warnings:
        print()
        print(f"{len(warnings)} warning(s):")
        for w in warnings:
            print(f"  {w}")
    return 0


# ---------------------------------------------------------------------------
# --selftest
# ---------------------------------------------------------------------------

def _assert_close(actual, expected, tol, msg):
    if actual is None or expected is None:
        if actual != expected:
            raise AssertionError(f"{msg}: expected {expected!r}, got {actual!r}")
        return
    if abs(actual - expected) > tol:
        raise AssertionError(f"{msg}: expected {expected!r}, got {actual!r} (tol {tol})")


def selftest():
    # --- Wilson score interval ---
    # Hand-computed (see formula in wilson_score_interval; cross-checked via
    # `python3 -c` at implementation time, matches the standard textbook
    # worked example of p=0.8, n=10):
    #   n=10, successes=8  -> (0.49015684672072346, 0.9433190520193067)
    lower, upper = wilson_score_interval(8, 10)
    _assert_close(lower, 0.49015684672072346, 1e-9, "wilson(8,10) lower")
    _assert_close(upper, 0.9433190520193067, 1e-9, "wilson(8,10) upper")

    # n=3, successes=3 (all pass) -> (0.4384939195509822, 1.0)
    lower, upper = wilson_score_interval(3, 3)
    _assert_close(lower, 0.4384939195509822, 1e-9, "wilson(3,3) lower")
    _assert_close(upper, 1.0, 1e-9, "wilson(3,3) upper")

    # n=5, successes=0 (all fail) -> (0.0, 0.43449149475208104)
    lower, upper = wilson_score_interval(0, 5)
    _assert_close(lower, 0.0, 1e-9, "wilson(0,5) lower")
    _assert_close(upper, 0.43449149475208104, 1e-9, "wilson(0,5) upper")

    # n=0 -> empty interval
    lower, upper = wilson_score_interval(0, 0)
    if lower is not None or upper is not None:
        raise AssertionError(f"wilson(0,0) should be (None, None), got ({lower}, {upper})")

    # --- pass@1 ---
    _assert_close(pass_at_1(7, 10), 0.7, 1e-12, "pass@1(7,10)")
    if pass_at_1(0, 0) is not None:
        raise AssertionError("pass@1(0,0) should be None")

    # --- pass^k ---
    if pass_at_k(5, 5) != 1:
        raise AssertionError("pass^k(5,5) should be 1 (all pass)")
    if pass_at_k(4, 5) != 0:
        raise AssertionError("pass^k(4,5) should be 0 (one fail)")
    if pass_at_k(0, 0) != 0:
        raise AssertionError("pass^k(0,0) should be 0 (no repeats)")

    # --- cost / successful task ---
    # 2,000,000 input tokens + 500,000 output tokens + 1,000,000 cached tokens
    # at made-up prices $2/$10/$1 per 1M -> cost = 2*2 + 0.5*10 + 1*1 = 4+5+1 = 10.0
    price = {"input": 2.0, "output": 10.0, "cached_input": 1.0}
    cost = (
        2_000_000 / 1_000_000.0 * price["input"]
        + 500_000 / 1_000_000.0 * price["output"]
        + 1_000_000 / 1_000_000.0 * price["cached_input"]
    )
    _assert_close(cost, 10.0, 1e-9, "hand cost calc")
    cps = cost_per_success(cost, True, 5)
    _assert_close(cps, 2.0, 1e-9, "cost_per_success(10.0, 5 successes)")
    if cost_per_success(cost, True, 0) != "n/a":
        raise AssertionError("cost_per_success with 0 successes should be 'n/a'")
    if cost_per_success(0.0, False, 5) != "n/a (no pricing)":
        raise AssertionError("cost_per_success with priced=False should be 'n/a (no pricing)'")

    # --- estimate_cost via PROVIDER_PRICING lookup (sanity, not a pin on
    # the live price table -- just checks the lookup/substring-match plumbing
    # doesn't crash and returns a priced result for a known provider) ---
    est_cost, priced = estimate_cost("anthropic", "claude-3-5-sonnet-20241022", 1_000_000, 0, 0)
    if not priced:
        raise AssertionError("expected anthropic/claude-sonnet family to be priced")
    _assert_close(est_cost, PROVIDER_PRICING["anthropic"]["claude-sonnet"]["input"], 1e-9,
                  "estimate_cost anthropic sonnet substring match")
    est_cost2, priced2 = estimate_cost("unknown-provider", "some-model", 1_000_000, 0, 0)
    if priced2:
        raise AssertionError("unknown provider should never be priced")

    # --- run-dir name parsing ---
    parsed = parse_run_dir_name("myScenario__anthropic__claude-3-5-sonnet__r1")
    if parsed != ("myScenario", "anthropic", "claude-3-5-sonnet", 1):
        raise AssertionError(f"parse_run_dir_name with model failed: {parsed}")
    parsed = parse_run_dir_name("myScenario__openai__r2")
    if parsed != ("myScenario", "openai", "", 2):
        raise AssertionError(f"parse_run_dir_name without model failed: {parsed}")
    parsed = parse_run_dir_name("not_a_valid_dir")
    if parsed is not None:
        raise AssertionError(f"parse_run_dir_name should reject malformed leaf, got {parsed}")

    print("eval_report selftest: ALL PASS")
    return 0


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def build_arg_parser():
    parser = argparse.ArgumentParser(
        prog="eval_report.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--selftest", action="store_true",
        help="Run in-process unit checks of the pure math and exit (no runDir needed).",
    )
    subparsers = parser.add_subparsers(dest="command")

    p_report = subparsers.add_parser("report", help="Aggregate metrics across a run sweep.")
    p_report.add_argument("runDir", help="Directory containing per-run subdirectories.")
    p_report.add_argument("--json", action="store_true", help="Emit JSON instead of a text table.")
    p_report.add_argument("--markdown", action="store_true", help="Emit a GitHub-flavored markdown table.")

    p_diff = subparsers.add_parser("diff", help="Diff two trajectory files.")
    p_diff.add_argument("trajA", help="Path to the first *.trajectory.jsonl file.")
    p_diff.add_argument("trajB", help="Path to the second *.trajectory.jsonl file.")

    return parser


def main(argv=None):
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    if args.selftest:
        try:
            return selftest()
        except AssertionError as e:
            print(f"eval_report selftest: FAIL: {e}", file=sys.stderr)
            return 1

    if args.command == "report":
        if args.json and args.markdown:
            print("error: --json and --markdown are mutually exclusive", file=sys.stderr)
            return 2
        return cmd_report(args)
    if args.command == "diff":
        return cmd_diff(args)

    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
