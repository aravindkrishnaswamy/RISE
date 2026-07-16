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
                                         budgetHit, wallMs, headVersionStart,
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
or "myScenario__openai__r2" when no model was set).  A scenario id or model
string that itself contains "__" (e.g. model "gpt__xai") can make that
dir-leaf ambiguous to parse -- scan_run_dir therefore treats the dir-leaf
parse as a FALLBACK only; the PRIMARY scenarioId/provider/model identity for
each cell comes from the STAMPED *.result.jsonl found inside the
subdirectory (byte-true, never sanitized), so identity discovery is
reversible even when the leaf itself is not.

This tool is pure-Python-3-stdlib (json, argparse, math, os, glob, collections)
-- no numpy/scipy/pandas -- since Python tooling is not part of the RISE make
build and must run standalone on any dev machine.

Subcommands
-----------
  report <runDir> [--json] [--markdown]
      Aggregate every run subdirectory under <runDir>, grouped by
      (scenarioId, provider, model).  Prints pass@1, pass^k (all-repeats-pass
      reliability), a 95% Wilson score interval on pass@1, mean partial-credit
      checkpointFraction, an estimated $-per-successful-task cost, a mean wall-clock completion
      time per run (the provider speed metric), and a terminalStatus
      failure-label breakdown per group.
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
import shutil
import sys
import tempfile
from collections import Counter, defaultdict

# ---------------------------------------------------------------------------
# Pricing table
#
# PRICES AS OF 2026-07, PUBLIC LIST PRICES, EDIT ME; cost is an ESTIMATE.
#
# Keyed by provider -> ("_default" plus optional per-model overrides, plus a
# "cached_in_input" bool documented below).  Model lookup is a
# case-insensitive substring match against the model key (e.g. the key
# "claude-3-5-sonnet" matches an actual model string of
# "claude-3-5-sonnet-20241022"); the most specific (longest) matching key
# wins ("_default" and "cached_in_input" are not model keys and are skipped
# by that scan).  All prices are USD per 1,000,000 tokens.  "cached_input" is
# the rate charged for cache-read input tokens
# (gen_ai.usage.cache_read_input_tokens); providers that don't distinguish a
# cached rate should set it equal to "input".  A provider/model combination
# absent from this table (or an unrecognized provider) makes cost report as
# "n/a (no pricing)" -- it never crashes the tool.
#
# "cached_in_input" (bool): whether this provider's reported input-token
# count is the TOTAL (cached tokens are a SUBSET already included in it,
# "cached_in_input": True) or whether cached tokens are reported separately/
# ADDITIVELY on top of a cache-excluding input count ("cached_in_input":
# False).  This is NOT a stylistic choice -- it mirrors each provider's real
# usage-accounting, verified against `ParseUsage` in
# src/Library/Agent/AgentChatCodecs.cpp, 2026-07:
#   anthropic (False, additive): `input_tokens` EXCLUDES cache reads --
#     `cache_read_input_tokens` is a separate, additive field.
#   openai (True, subset): `input_tokens` = `prompt_tokens` is the TOTAL, and
#     `cached_tokens` (`prompt_tokens_details.cached_tokens`) is a SUBSET.
#   gemini (True, subset): `input_tokens` = `promptTokenCount` is the TOTAL,
#     and `cached_tokens` (`cachedContentTokenCount`) is a SUBSET.
# See estimate_cost() for how the flag changes the arithmetic; getting this
# wrong double-counts the cached portion for openai/gemini (once in the full
# input term, once again in the cached term) -- up to 2x overcount on
# cache-heavy conversations.
# ---------------------------------------------------------------------------
PROVIDER_PRICING = {
    "anthropic": {
        "cached_in_input": False,
        "_default": {"input": 3.00, "output": 15.00, "cached_input": 0.30},
        # NOTE: the generic "claude-opus" below is Opus-3-era pricing; current
        # Opus (4.6/4.7/4.8) is $5/$25. The specific "claude-opus-4-8" key is
        # longer and so wins the most-specific-match lookup for the 4.8 shootout
        # runs, leaving the generic as a legacy fallback only.
        "claude-opus-4-8": {"input": 5.00, "output": 25.00, "cached_input": 0.50},
        "claude-opus": {"input": 15.00, "output": 75.00, "cached_input": 1.50},
        "claude-sonnet": {"input": 3.00, "output": 15.00, "cached_input": 0.30},
        "claude-haiku": {"input": 0.80, "output": 4.00, "cached_input": 0.08},
    },
    "openai": {
        "cached_in_input": True,
        "_default": {"input": 2.50, "output": 10.00, "cached_input": 1.25},
        "gpt-4o-mini": {"input": 0.15, "output": 0.60, "cached_input": 0.075},
        "gpt-4o": {"input": 2.50, "output": 10.00, "cached_input": 1.25},
        "gpt-4.1-mini": {"input": 0.40, "output": 1.60, "cached_input": 0.10},
        "gpt-4.1": {"input": 2.00, "output": 8.00, "cached_input": 0.50},
        # Shootout model. Official rates (developers.openai.com/api/docs/models/
        # gpt-5.6-terra, verified 2026-07-15). Standard tier; the 2x-input/1.5x-
        # output tier for >272K-token prompts never applies to the eval scenarios
        # (tiny prompts). cached_input is the cache-READ rate (cache writes bill
        # at 1.25x input, not modelled here -- same as every other openai entry).
        "gpt-5.6-terra": {"input": 2.50, "output": 15.00, "cached_input": 0.25},
    },
    "gemini": {
        "cached_in_input": True,
        "_default": {"input": 1.25, "output": 5.00, "cached_input": 0.3125},
        "gemini-1.5-flash": {"input": 0.075, "output": 0.30, "cached_input": 0.01875},
        "gemini-1.5-pro": {"input": 1.25, "output": 5.00, "cached_input": 0.3125},
        "gemini-2.0-flash": {"input": 0.10, "output": 0.40, "cached_input": 0.025},
        # Shootout model. Official rates (ai.google.dev/gemini-api/docs/pricing,
        # verified 2026-07-15): flat, no context-length tier. Cache storage
        # ($1/1M-tok-hour) is not modelled (per-token read rate only).
        "gemini-3.5-flash": {"input": 1.50, "output": 9.00, "cached_input": 0.15},
    },
    "xai": {
        # xAI (Grok) bills like OpenAI: cached-read tokens are a SUBSET of the
        # reported input tokens, not additive.
        "cached_in_input": True,
        # _default: a conservative fallback for any UNLISTED xai model (cached ==
        # input, an overestimate when caching hits). grok-4.5 now has its own
        # exact entry below, so this default no longer applies to it.
        "_default": {"input": 2.00, "output": 6.00, "cached_input": 2.00},
        "grok-4.3": {"input": 1.25, "output": 2.50, "cached_input": 0.20},
        # Shootout model. Official rates (docs.x.ai/developers/models/grok-4.5,
        # verified 2026-07-15). Standard tier (<200k prompt tokens); the
        # 2x higher-context tier (>=200k) never applies to the eval scenarios.
        "grok-4.5": {"input": 2.00, "output": 6.00, "cached_input": 0.50},
    },
    "local": {
        # Local inference (Ollama et al.): no marginal per-token cost, so every
        # rate is 0.0 and cost reports $0.00.  cached_in_input matches the
        # OpenAI-compatible convention it inherits (moot at zero rates).
        # default_is_authoritative: the $0 _default is DEFINITIONAL here (not an
        # estimate), so a local model with no specific entry is still PRICED at
        # $0.00 -- unlike a hosted provider's generic _default, which is
        # reported unpriced (see estimate_cost).
        "cached_in_input": True,
        "default_is_authoritative": True,
        "_default": {"input": 0.0, "output": 0.0, "cached_input": 0.0},
    },
}

# The three shootout models are now PRICED with published rates verified against
# the official provider docs on 2026-07-15 (gemini-3.5-flash, gpt-5.6-terra,
# grok-4.5 -- see the per-entry comments above for the source URLs), so they
# report a real $/success. To price a NEWLY-added shootout model, add a specific
# entry under the matching provider with the published USD/1M-token rates:
#   provider "model-id": {"input": ?, "output": ?, "cached_input": ?}
# A model with no specific entry falls back to "n/a (no pricing)" (honest) rather
# than the generic per-provider _default. Do NOT invent rates -- an unpriced row
# is better than a wrong one.

# Must stay in lockstep with the provider set recognized by
# `ParseReplayProviderName` / `ChatProvider` in
# src/Library/Agent/AgentChatCodecs.{h,cpp} + AgentChatLoop.{h,cpp} -- adding a
# provider there requires adding it here (and to PROVIDER_PRICING) too.
KNOWN_PROVIDERS = ("anthropic", "openai", "gemini", "xai", "local")

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
    """pass^k (caret, NOT pass@k): all-repeats-pass reliability indicator -- 1 if
    successes == n (n>0), else 0. This is the reliability metric (did EVERY one
    of the k repeats pass), NOT the conventional pass@k (>=1 of k passed). The
    function name reads like 'pass_at_k' for identifier legality; the metric and
    every user-facing label are pass^k. See the report legend."""
    if n <= 0:
        return 0
    return 1 if successes == n else 0


def lookup_pricing(provider, model):
    """Return (price_dict, exact). `price_dict` is the {"input","output",
    "cached_input"} USD/1M-token rates, or None when the provider is unknown.
    `exact` is True ONLY when a SPECIFIC model key matched -- False when we fell
    back to the provider `_default`. Callers pair `exact` with the provider's
    `default_is_authoritative` flag to decide whether the resulting cost is a
    real per-model rate or must be reported unpriced (a generic provider
    `_default` applied to an unlisted model -- e.g. gemini-3.5-flash at the
    generic Gemini rate -- is a wrong number, not a price)."""
    prov_table = PROVIDER_PRICING.get(provider)
    if prov_table is None:
        return (None, False)
    model_l = (model or "").lower()
    best_key = None
    for key in prov_table:
        if key in ("_default", "cached_in_input", "default_is_authoritative"):
            continue
        if key.lower() in model_l:
            if best_key is None or len(key) > len(best_key):
                best_key = key
    if best_key is not None:
        return (prov_table[best_key], True)
    return (prov_table.get("_default"), False)


def estimate_cost(provider, model, input_tokens, output_tokens, cached_tokens):
    """Return (cost_usd, priced_bool). priced_bool False -> no pricing found.

    Cached-token accounting is provider-dependent (see the "cached_in_input"
    comment on PROVIDER_PRICING above): some providers report cached tokens
    as a SUBSET already counted inside input_tokens (openai, gemini), others
    report them ADDITIVELY/disjoint from input_tokens (anthropic). Charging
    the naive `input*rate + output*rate + cached*rate` formula for a
    subset-convention provider double-bills the cached portion, so we branch
    on the flag instead of applying one formula universally.
    """
    price, exact = lookup_pricing(provider, model)
    if price is None:
        return (0.0, False)
    prov_table = PROVIDER_PRICING[provider]
    # A `_default` fallback (exact=False) is only trustworthy when the provider
    # marks its default authoritative -- `local` alone does (no marginal
    # per-token cost, so $0 is definitional, not an estimate). For every hosted
    # provider the `_default` is a GENERIC rate that is simply wrong for an
    # unlisted model, so report those UNPRICED rather than present a fabricated
    # $/success. Add a specific PROVIDER_PRICING entry for a model to price it
    # (the three shootout models now have exact entries; a future unlisted model
    # reports n/a until one is added).
    if not exact and not prov_table.get("default_is_authoritative", False):
        return (0.0, False)
    # Provider is guaranteed present in PROVIDER_PRICING here (lookup_pricing
    # only returns non-None price when it found a table for `provider`). Missing
    # the flag defaults to False (additive/anthropic-style) -- the
    # under-count-rather-than-double-count safer default.
    cached_in_input = prov_table.get("cached_in_input", False)
    if cached_in_input:
        noncached_input = max(0, input_tokens - cached_tokens)
        cost = (
            noncached_input / 1_000_000.0 * price["input"]
            + cached_tokens / 1_000_000.0 * price["cached_input"]
            + output_tokens / 1_000_000.0 * price["output"]
        )
    else:
        cost = (
            input_tokens / 1_000_000.0 * price["input"]
            + cached_tokens / 1_000_000.0 * price["cached_input"]
            + output_tokens / 1_000_000.0 * price["output"]
        )
    return (cost, True)


def cost_per_success(total_cost_usd, priced, successes):
    if not priced:
        return "n/a (no pricing)"
    if successes <= 0:
        return "n/a"
    return total_cost_usd / successes


# ---------------------------------------------------------------------------
# FNV-1a 64-bit -- Finding 2: raw FILE-BYTE hashes a standalone Python
# reporter can trivially recompute byte-for-byte, closing the
# equal-checkpoint-count blind spot check_staleness's C++-hash cross-check
# can't (this tool cannot replicate AgentEvalRunner.cpp's semantic
# ScenarioContentHash canonicalization). MUST stay bit-identical to the C++
# FnvHex helper in src/Library/Agent/AgentEvalRunner.cpp -- the cross-language
# pin is FNV-1a 64 of b"hello" == "a430d84680aabd0b" (asserted by both
# --selftest here and a C++ test; see AgentEvalReplayTest.cpp).
# ---------------------------------------------------------------------------

def fnv1a64_hex(data):
    """FNV-1a 64-bit hex digest (16 lowercase hex chars) of `data` (bytes)."""
    h = 14695981039346656037  # offset basis
    prime = 1099511628211
    mask = 0xFFFFFFFFFFFFFFFF
    for b in data:
        h ^= b
        h = (h * prime) & mask
    return "%016x" % h


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
#
# By construction the provider token always comes AFTER the scenario id and
# BEFORE the (optional) model + trailing "r<repeat>" tokens, so scan from the
# RIGHT (last candidate token down to the first) and take the FIRST match
# found that way -- i.e. the LAST known-provider token in the leaf. A
# left-to-right scan would instead mis-attribute a leaf whose sanitized
# scenario id happens to contain a standalone provider-name token as one of
# its own "__"-separated pieces (e.g. scenario id "test, local, edit" ->
# "test__local__edit", provider "anthropic" -> leaf
# "test__local__edit__anthropic__<model>__r1" would find "local" first
# left-to-right instead of the real provider "anthropic").
# ---------------------------------------------------------------------------

def parse_rep_from_leaf(leaf):
    """Extract ONLY the trailing "__r<repeat>" token from a run-dir leaf --
    the ONE piece of identity that is unambiguous no matter what characters a
    scenario id or model string contain (Finding 1b: everything else about a
    cell's identity now comes from the STAMPED result.jsonl, not this parse;
    see scan_run_dir). Returns an int repeat number, or None if the leaf
    does not end with an "__r<digits>" token (not an eval run-dir at all)."""
    tokens = leaf.split("__")
    if len(tokens) < 2:
        return None
    last = tokens[-1]
    if not (last.startswith("r") and last[1:].isdigit()):
        return None
    return int(last[1:])


def parse_run_dir_name(leaf):
    tokens = leaf.split("__")
    if len(tokens) < 2:
        return None
    last = tokens[-1]
    if not (last.startswith("r") and last[1:].isdigit()):
        return None
    rep = int(last[1:])
    provider_idx = None
    for i in range(len(tokens) - 2, 0, -1):
        if tokens[i] in KNOWN_PROVIDERS:
            provider_idx = i
            break
    if provider_idx is None:
        # No known-provider token found -- do NOT guess a provider/model
        # split. A leaf with both an unrecognized provider token AND a model
        # segment (e.g. "s5__mockprov__mockmodel__r1") would silently
        # mis-split under the old "assume no model segment, provider is
        # second-to-last token" fallback, misreading scenario/provider/model
        # and pulling foreign data into a wrong-but-plausible row. Returning
        # None here routes through the caller's existing "could not parse
        # run-dir name, skipping: <leaf>" warning instead.
        return None
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
        "notes", "stale_marker",
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
        # "" (default) / "unversioned" (no stamped scenarioContentHash --
        # pre-content-hash result) / "stale" (stamped checkpoint count does
        # not match the current on-disk scenario file). Set by
        # check_staleness(); comma-joined if a future check adds a second
        # marker to the same record (today's checks are mutually exclusive
        # per-record, but the table-tagging code below treats this as a
        # set, not a single value, to stay correct if that changes).
        self.stale_marker = ""


def scan_run_dir(run_dir, warnings):
    """Return list of RunRecord for every parseable run subdirectory under run_dir.

    Identity discovery (Finding 1b -- report-artifact-discovery reversibility):
    a scenario id may legally contain characters that make the "__"-delimited
    dir-leaf ambiguous or plain wrong to parse (pre-Finding-1a scenario ids;
    a raw id containing a provider-name-shaped token; a model id itself
    containing "__", e.g. "gpt__xai" inside "param_edit__openai__gpt__xai__r1",
    which parse_run_dir_name's right-to-left provider scan can misattribute).
    The dir-leaf parse is therefore demoted to a FALLBACK; the PRIMARY source
    of scenario_id/provider/model is the STAMPED *.result.jsonl found inside
    the subdirectory -- exactly the fields RunScenarioDriven writes and that
    are, by construction, byte-true (never sanitized/lossy)."""
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

        # `rep` is the one dir-leaf field that's unambiguous regardless of
        # what a scenario id/model contain (it's always the LAST "__"-token,
        # "r<digits>"). A leaf that doesn't carry one isn't an eval run-dir
        # at all -- skip it exactly as before.
        rep = parse_rep_from_leaf(leaf)
        if rep is None:
            warnings.append(f"warning: could not parse run-dir name, skipping: {leaf}")
            continue

        # FALLBACK identity, for pre-stamp results / a subdir whose
        # result.jsonl is missing entirely. None when the leaf's tokens don't
        # contain a recognizable provider (see parse_run_dir_name's own
        # caveats) -- in that case scenario_id/provider start out None and
        # MUST be filled in by the stamp below, or the record is dropped.
        fallback = parse_run_dir_name(leaf)
        scenario_id = fallback[0] if fallback else None
        provider = fallback[1] if fallback else None
        model = fallback[2] if fallback else ""

        # PRIMARY identity: glob for *.result.jsonl rather than guessing the
        # filename from the (possibly-wrong) dir-parsed scenario_id -- there
        # is exactly one per healthy cell, named "<RAW scenarioId>.result.jsonl"
        # by RunScenarioDriven, where "RAW" is the un-sanitized id (Finding 1a
        # closed the class of ids that could make this diverge from the
        # sanitized dir-leaf going forward, but old runDirs may still hold
        # pre-fix cells). Several matches should never happen; warn and take
        # the lexicographically first, deterministically.
        result_candidates = sorted(glob.glob(os.path.join(sub, "*.result.jsonl")))
        if len(result_candidates) > 1:
            warnings.append(
                f"warning: {sub}: multiple *.result.jsonl files found "
                f"({', '.join(os.path.basename(p) for p in result_candidates)}) -- "
                f"using {os.path.basename(result_candidates[0])}"
            )
        result_path = result_candidates[0] if result_candidates else None

        result = None
        if result_path is not None:
            result_lines = load_jsonl(result_path, warnings)
            if result_lines:
                result = result_lines[-1]

        # Prefer the TRUE (un-sanitized) scenarioId/provider/model stamped
        # inside the result over the lossy dir-name parse (P1b + P2):
        # SanitizeForPath maps any char outside [A-Za-z0-9._-] to '_', so two
        # distinct ids/model ids can collapse to one leaf fragment -- the
        # stamped fields are the authoritative source of what actually ran.
        # scenarioId/provider are never legitimately empty (a run always has
        # both), so a present+truthy check is right for them; model CAN be
        # legitimately empty ("" => the codec default), which is meaningfully
        # different from "unknown", so a present-only check (which lets an
        # explicit "" override the dir-parsed value) is right for it.
        if isinstance(result, dict):
            if result.get("scenarioId"):
                scenario_id = result["scenarioId"]
            if result.get("provider"):
                provider = result["provider"]
            if "model" in result:
                model = result["model"]

        if scenario_id is None or provider is None:
            # Neither a stamped result NOR a parseable dir-leaf could name
            # this cell -- the original "could not parse" skip path.
            warnings.append(f"warning: could not parse run-dir name, skipping: {leaf}")
            continue

        rec = RunRecord()
        rec.dir_path = sub
        rec.scenario_id = scenario_id
        rec.provider = provider
        rec.model = model
        rec.rep = rep
        rec.result = result
        if result is None:
            rec.notes.append("missing .result.jsonl")

        check_path = os.path.join(sub, "results.jsonl")
        check_lines = load_jsonl(check_path, warnings)
        matching = [c for c in check_lines if c.get("scenarioId") == scenario_id]
        if matching:
            rec.check = matching[-1]
        elif check_lines:
            # results.jsonl exists but no line matches this scenarioId --
            # treat as MISSING check data (leave rec.check None, same as the
            # "missing results.jsonl" branch below) rather than silently
            # substituting a foreign scenario's line, which would misreport
            # pass/fail for this run under a wrong-but-plausible read.
            rec.check = None
            rec.notes.append(
                "results.jsonl present but scenarioId mismatch (treated as missing)"
            )
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


def _mark_stale(rec, marker):
    """Add `marker` ("unversioned" / "stale") to rec.stale_marker, which is
    treated as a comma-joined set so a record that somehow trips more than
    one check keeps every marker rather than the last writer winning."""
    existing = rec.stale_marker.split(",") if rec.stale_marker else []
    if marker not in existing:
        existing.append(marker)
    rec.stale_marker = ",".join(existing)


def check_staleness(records, warnings):
    """Cross-check each record's stamped scenarioContentHash /
    scenarioCheckpointCount (written by AgentEvalRunner.cpp's
    RunScenarioDriven -- see the P1 content-aware-resume-guard finding)
    against the CURRENT evals/scenarios/<id>.json on disk, and against each
    other within a scenario group, appending a warning wherever a cell looks
    to have been graded under a since-changed scenario version. Non-fatal --
    appends to `warnings`, never raises: the report still prints exactly as
    before, just with more warnings when a cell is stale. ALSO tags each
    affected record's `stale_marker` ("unversioned" / "stale") so callers
    that group/render records (cmd_report's table renderers) can mark the
    affected scenario cells directly instead of relying on the reader to
    correlate free-text warnings back to a row. Call this BEFORE grouping --
    grouping copies records into per-group lists by reference, so the tags
    are visible from the group's records either way, but callers should not
    rely on that; check_staleness is meant to run first.

    ADVISORY ONLY for the scenarioContentHash/checkpoint-COUNT cross-check
    above: this function cannot recompute the C++ FNV ScenarioContentHash
    from the on-disk scenario (the semantic canonicalization is not
    replicated in Python), so that half of the check is only the raw
    checkpoint COUNT -- an edit that changes a checkpoint's VALUE (or a
    prompt/budget/scene) WITHOUT changing the count is invisible to it if you
    run the report against old result files without re-running the matrix.

    Finding 2 closes that blind spot for FILE-BACKED scenarios (i.e. the
    whole committed suite): "scenarioFileFnv" / "sceneFileFnv" are raw
    FNV-1a 64 hashes of the scenario/scene files' BYTES, stamped by
    RunScenarioDriven (AgentEvalRunner.cpp) and recomputed here byte-for-byte
    via fnv1a64_hex -- a mismatch means the file's CONTENT changed even when
    the checkpoint COUNT (and therefore the count cross-check above) did not.
    The remaining advisory gap is only PROGRAMMATICALLY-BUILT scenarios that
    carry no `sourcePath` (AgentEvalScenario.sourcePath is empty, so no
    scenarioFileFnv/sceneFileFnv was ever stamped) -- those still rely solely
    on the semantic-hash/count check above. The AUTHORITATIVE staleness
    protection remains the C++ matrix resume guard, which hashes the full
    semantic content and RE-RUNS a changed cell; re-run the matrix and this
    report then reflects the current oracle. This pass is a best-effort
    heads-up, not that guarantee."""
    by_scenario = defaultdict(list)
    for r in records:
        by_scenario[r.scenario_id].append(r)

    for scenario_id, recs in by_scenario.items():
        hashes = set()
        for r in recs:
            if r.result is None:
                continue

            # Key file lookups on the AUTHORITATIVE scenarioId stamped in the
            # result (the raw scenario id), not the dir-parsed (sanitized)
            # group key, so a scenario id that SanitizeForPath would rewrite
            # still resolves to its file. Falls back to the group key for old
            # results.
            file_id = r.result.get("scenarioId") or scenario_id
            scenario_path = os.path.join("evals", "scenarios", f"{file_id}.json")

            h = r.result.get("scenarioContentHash")
            if not h:
                warnings.append(
                    f"warning: unversioned result (pre-content-hash) -- may have "
                    f"been graded under a different scenario version: {r.dir_path}"
                )
                _mark_stale(r, "unversioned")
            else:
                hashes.add(h)

                # A cheaper, filesystem-anchored cross-check: the stamped raw
                # checkpoint count vs the scenario file's CURRENT checkpoint
                # count. A mismatch is a definite oracle change even if the
                # hash alone can't say which direction.
                stored_count = r.result.get("scenarioCheckpointCount")
                try:
                    with open(scenario_path, "r", encoding="utf-8") as f:
                        current = json.load(f)
                    current_count = len(current.get("checkpoints", []))
                except (OSError, ValueError):
                    current_count = None  # scenario file not found / unreadable -- skip the count check
                if current_count is not None and isinstance(stored_count, (int, float)) \
                        and int(stored_count) != current_count:
                    warnings.append(
                        f"warning: STALE result: {r.dir_path} was graded with "
                        f"{int(stored_count)} checkpoints but evals/scenarios/{scenario_id}.json "
                        f"now has {current_count} -- re-run to refresh"
                    )
                    _mark_stale(r, "stale")

            # --- Finding 2: raw file-byte FNV-1a 64 cross-check. Independent
            # of scenarioContentHash's presence above -- runs whenever the
            # corresponding stamp is present, closing the equal-count blind
            # spot for FILE-backed scenarios/scenes. Absent stamps or
            # unreadable files are skipped SILENTLY (absent means "not
            # applicable / unknown", never "stale") -- only an actual
            # mismatch between a present stamp and the current file's bytes
            # warns.
            scenario_fnv = r.result.get("scenarioFileFnv")
            if scenario_fnv:
                try:
                    with open(scenario_path, "rb") as f:
                        current_bytes = f.read()
                except OSError:
                    pass  # unreadable -- skip silently
                else:
                    if fnv1a64_hex(current_bytes) != scenario_fnv:
                        warnings.append(
                            f"warning: STALE result: {r.dir_path} -- evals/scenarios/{file_id}.json "
                            f"changed since this cell was graded (file hash mismatch) -- "
                            f"re-run to refresh"
                        )
                        _mark_stale(r, "stale")

            scene_fnv = r.result.get("sceneFileFnv")
            if scene_fnv:
                scene_path = None
                try:
                    with open(scenario_path, "r", encoding="utf-8") as f:
                        current_scenario = json.load(f)
                    scene_obj = current_scenario.get("scene")
                    if isinstance(scene_obj, dict):
                        scene_path = scene_obj.get("path") or None
                except (OSError, ValueError):
                    scene_path = None
                if scene_path:
                    try:
                        with open(scene_path, "rb") as f:
                            current_scene_bytes = f.read()
                    except OSError:
                        pass  # unreadable -- skip silently
                    else:
                        if fnv1a64_hex(current_scene_bytes) != scene_fnv:
                            warnings.append(
                                f"warning: STALE result: {r.dir_path} -- scene file {scene_path} "
                                f"changed since this cell was graded (file hash mismatch) -- "
                                f"re-run to refresh"
                            )
                            _mark_stale(r, "stale")

        # Two cells in the same group with different stamped hashes means the
        # sweep straddled a scenario change -- some rows are stale regardless of
        # whether the current on-disk file matches either.
        if len(hashes) > 1:
            warnings.append(
                f"warning: mixed scenario versions in group {scenario_id} -- "
                f"some cells are stale; re-run"
            )


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
    wall_ms_list = []

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

        # Wall-clock completion time per run (ms) -- the provider "time to
        # completion" comparison. Skip -1 (load_error, never timed) and a
        # missing field (result.jsonl written before wallMs existed).
        if r.result is not None and isinstance(r.result.get("wallMs"), (int, float)) and r.result["wallMs"] >= 0:
            wall_ms_list.append(r.result["wallMs"])

        total_input += r.input_tokens
        total_output += r.output_tokens
        total_cached += r.cached_tokens

    p1 = pass_at_1(successes, n)
    pk = pass_at_k(successes, n)
    lower, upper = wilson_score_interval(successes, n)
    mean_ckpt = (sum(checkpoint_fracs) / len(checkpoint_fracs)) if checkpoint_fracs else None
    mean_wall_ms = (sum(wall_ms_list) / len(wall_ms_list)) if wall_ms_list else None

    cost, priced = estimate_cost(provider, model, total_input, total_output, total_cached)
    cps = cost_per_success(cost, priced, successes)

    # Whether ANY record in this group was tagged by check_staleness() --
    # drives the "†"/"!" markers on the rendered scenario cell (see
    # render_text_report / render_markdown_report).
    has_unversioned = any("unversioned" in (r.stale_marker or "").split(",") for r in recs)
    has_stale = any("stale" in (r.stale_marker or "").split(",") for r in recs)

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
        "mean_wall_ms": mean_wall_ms,
        "terminal_status_counts": dict(status_counter),
        "has_unversioned": has_unversioned,
        "has_stale": has_stale,
    }


def compute_overall_rollup(stats_list):
    """Pooled OVERALL rollup across a list of compute_group_stats() dicts.

    pass@1 here is the POOLED aggregate sum(successes)/sum(n) across all
    groups -- NOT the mean of each group's individual pass_at_1. Those two
    differ whenever groups have unequal n (a group with 2 repeats and a group
    with 100 repeats should not weigh equally in the overall figure).
    """
    total_n = sum(s["n"] for s in stats_list)
    total_succ = sum(s["successes"] for s in stats_list)
    groups_fully_reliable = sum(1 for s in stats_list if s["pass_at_k"] == 1)
    overall_p1 = pass_at_1(total_succ, total_n)
    lower, upper = wilson_score_interval(total_succ, total_n)
    return {
        "num_groups": len(stats_list),
        "total_n": total_n,
        "total_successes": total_succ,
        "pass_at_1": overall_p1,
        "wilson_lower": lower,
        "wilson_upper": upper,
        "groups_fully_reliable": groups_fully_reliable,
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


def fmt_wall(mean_ms):
    """Mean wall-clock completion time per run, formatted in seconds."""
    if mean_ms is None:
        return "n/a"
    return f"{mean_ms / 1000.0:.2f}s"


def fmt_status_counts(counts):
    return " ".join(f"{k}:{v}" for k, v in sorted(counts.items())) if counts else "-"


def stale_cell_suffix(stats):
    """"†" ('†') when the group contains any unversioned
    (pre-content-hash) record, "!" when it contains any stale
    (checkpoint-count-mismatch) record, both concatenated when it has both,
    "" otherwise. Appended to the rendered scenario-id cell so a stale/
    unversioned group is visible IN THE TABLE, not just in the warnings
    list below it."""
    suffix = ""
    if stats.get("has_unversioned"):
        suffix += "†"
    if stats.get("has_stale"):
        suffix += "!"
    return suffix


def render_text_report(stats_list, warnings):
    headers = [
        "scenario", "provider", "model", "N", "pass@1", "wilson95%",
        "pass^k", "meanCkpt", "$/success", "wall(s)", "failureBreakdown",
    ]
    rows = []
    for s in stats_list:
        rows.append([
            s["scenarioId"] + stale_cell_suffix(s), s["provider"], s["model"] or "-",
            str(s["n"]), fmt_pct(s["pass_at_1"]),
            fmt_ci(s["wilson_lower"], s["wilson_upper"]),
            str(s["pass_at_k"]),
            "n/a" if s["mean_checkpoint_fraction"] is None else f"{s['mean_checkpoint_fraction']:.3f}",
            fmt_cost(s["cost_per_success"]),
            fmt_wall(s["mean_wall_ms"]),
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

    # Overall roll-up (pooled, not a mean-of-per-group-pass@1 -- see
    # compute_overall_rollup()).
    ov = compute_overall_rollup(stats_list)
    any_tagged = any(s.get("has_unversioned") or s.get("has_stale") for s in stats_list)
    lines.append("")
    lines.append(
        f"OVERALL: {ov['num_groups']} groups, {ov['total_n']} repeats total, "
        f"{ov['total_successes']} successes, pass@1={fmt_pct(ov['pass_at_1'])} "
        f"{fmt_ci(ov['wilson_lower'], ov['wilson_upper'])}, "
        f"{ov['groups_fully_reliable']}/{ov['num_groups']} groups fully reliable (pass^k=1)"
        + (" [contains unversioned/stale rows -- see †/! markers]" if any_tagged else "")
    )

    lines.append("")
    lines.append(
        "Legend: pass@1 = mean success rate over N repeats. "
        "pass^k (NOT pass@k) = 1 only when ALL N repeats passed -- a per-group "
        "reliability indicator, distinct from the conventional pass@k "
        "(>=1 of k). $/success = total cost / successes ('n/a (no pricing)' when "
        "the model has no specific pricing entry). wall(s) = mean wall-clock "
        "completion time per run in seconds (LLM round-trips + tool dispatch; "
        "the provider speed metric -- runs are serialized so it is comparable). "
        "failureBreakdown = terminal-status counts across the N repeats. "
        "† = contains unversioned (pre-content-hash) results -- not tied to "
        "the current scenario definitions; re-run the matrix to refresh. "
        "! = contains results graded under a different checkpoint count (stale)."
    )

    if warnings:
        lines.append("")
        lines.append(f"{len(warnings)} warning(s):")
        lines.extend(f"  {w}" for w in warnings)

    return "\n".join(lines)


def render_markdown_report(stats_list, warnings):
    headers = [
        "scenario", "provider", "model", "N", "pass@1", "wilson95%",
        "pass^k", "meanCkpt", "$/success", "wall(s)", "failureBreakdown",
    ]
    lines = []
    lines.append("| " + " | ".join(headers) + " |")
    lines.append("|" + "|".join(["---"] * len(headers)) + "|")
    for s in stats_list:
        row = [
            s["scenarioId"] + stale_cell_suffix(s), s["provider"], s["model"] or "-",
            str(s["n"]), fmt_pct(s["pass_at_1"]),
            fmt_ci(s["wilson_lower"], s["wilson_upper"]),
            str(s["pass_at_k"]),
            "n/a" if s["mean_checkpoint_fraction"] is None else f"{s['mean_checkpoint_fraction']:.3f}",
            fmt_cost(s["cost_per_success"]),
            fmt_wall(s["mean_wall_ms"]),
            fmt_status_counts(s["terminal_status_counts"]).replace("|", "\\|"),
        ]
        lines.append("| " + " | ".join(row) + " |")

    ov = compute_overall_rollup(stats_list)
    any_tagged = any(s.get("has_unversioned") or s.get("has_stale") for s in stats_list)
    lines.append("")
    lines.append(
        f"**OVERALL**: {ov['num_groups']} groups, {ov['total_n']} repeats total, "
        f"{ov['total_successes']} successes, pass@1={fmt_pct(ov['pass_at_1'])} "
        f"{fmt_ci(ov['wilson_lower'], ov['wilson_upper'])}, "
        f"{ov['groups_fully_reliable']}/{ov['num_groups']} groups fully reliable (pass^k=1)"
        + (" [contains unversioned/stale rows -- see †/! markers]" if any_tagged else "")
    )
    lines.append("")
    lines.append(
        "**Legend**: `pass@1` = mean success rate over N repeats. "
        "`pass^k` (NOT pass@k) = 1 only when ALL N repeats passed -- a per-group "
        "reliability indicator, distinct from the conventional pass@k (>=1 of k). "
        "`$/success` = total cost / successes (`n/a (no pricing)` when the model "
        "has no specific pricing entry). `wall(s)` = mean wall-clock completion "
        "time per run in seconds (the provider speed metric; runs are "
        "serialized so it is comparable). `failureBreakdown` = terminal-status "
        "counts across the N repeats. "
        "† = contains unversioned (pre-content-hash) results -- not tied to "
        "the current scenario definitions; re-run the matrix to refresh. "
        "! = contains results graded under a different checkpoint count (stale)."
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
    check_staleness(records, warnings)
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
    # A CURRENT model id substring-matches its specific key (claude-sonnet-5
    # contains "claude-sonnet") -> priced exactly. (A legacy version-suffixed id
    # like claude-3-5-sonnet-20241022 does NOT substring-match "claude-sonnet"
    # and, post-P1(d), reports unpriced rather than borrowing the generic
    # _default -- the honest behavior; add a specific entry to price it.)
    est_cost, priced = estimate_cost("anthropic", "claude-sonnet-5", 1_000_000, 0, 0)
    if not priced:
        raise AssertionError("expected anthropic/claude-sonnet-5 (matches 'claude-sonnet') to be priced")
    _assert_close(est_cost, PROVIDER_PRICING["anthropic"]["claude-sonnet"]["input"], 1e-9,
                  "estimate_cost anthropic sonnet substring match")

    # --- Shootout-model pricing: the three shootout models now have SPECIFIC
    # entries (published rates verified against the official provider docs
    # 2026-07-15), so they must be PRICED at exactly those rates. For 1M input
    # tokens with no cache/output, the cost equals the input rate for every
    # provider (subset-convention: noncached_input == input_tokens). ---
    flash_cost, flash_priced = estimate_cost("gemini", "gemini-3.5-flash", 1_000_000, 0, 0)
    if not flash_priced:
        raise AssertionError("gemini-3.5-flash has a specific pricing entry -> must be priced")
    _assert_close(flash_cost, 1.50, 1e-9, "gemini-3.5-flash input rate")
    terra_cost, terra_priced = estimate_cost("openai", "gpt-5.6-terra", 1_000_000, 0, 0)
    if not terra_priced:
        raise AssertionError("gpt-5.6-terra has a specific pricing entry -> must be priced")
    _assert_close(terra_cost, 2.50, 1e-9, "gpt-5.6-terra input rate")
    grok45_cost, grok45_priced = estimate_cost("xai", "grok-4.5", 1_000_000, 0, 0)
    if not grok45_priced:
        raise AssertionError("grok-4.5 has a specific pricing entry -> must be priced")
    _assert_close(grok45_cost, 2.00, 1e-9, "grok-4.5 input rate")

    # --- P1(d) regression (invariant preserved): a genuinely UNLISTED hosted
    # model must still report UNPRICED, not silently bill at the generic provider
    # _default. Use a synthetic id that matches no specific key. ---
    _, unlisted_priced = estimate_cost("gemini", "gemini-9.9-nonexistent-xyz", 1_000_000, 0, 0)
    if unlisted_priced:
        raise AssertionError("an unlisted hosted model must be UNPRICED, not billed at the generic _default")
    if cost_per_success(0.0, unlisted_priced, 5) != "n/a (no pricing)":
        raise AssertionError("an unpriced hosted model's $/success must render 'n/a (no pricing)'")
    # local's _default is DEFINITIONAL $0 (default_is_authoritative), so an
    # unlisted local model is still PRICED -- at $0.00, not unpriced.
    local_cost, local_priced = estimate_cost("local", "qwen3:32b", 1_000_000, 500_000, 0)
    if not local_priced:
        raise AssertionError("local models must stay PRICED at $0 (default_is_authoritative), not unpriced")
    _assert_close(local_cost, 0.0, 1e-12, "local model costs $0.00")
    # a specific xai entry still prices exactly (grok-4.3 is listed).
    _, grok43_priced = estimate_cost("xai", "grok-4.3", 1_000_000, 0, 0)
    if not grok43_priced:
        raise AssertionError("grok-4.3 has a specific entry -> must be priced")
    est_cost2, priced2 = estimate_cost("unknown-provider", "some-model", 1_000_000, 0, 0)
    if priced2:
        raise AssertionError("unknown provider should never be priced")

    # --- FIX 1 regression: estimate_cost() cached-token accounting must
    # branch per-provider (openai/gemini cached tokens are a SUBSET of
    # input_tokens; anthropic's are additive/disjoint) -- see the
    # "cached_in_input" comment on PROVIDER_PRICING. Each case below calls
    # the REAL estimate_cost() (not a re-derivation of its formula) and pins
    # the result against a hand-computed literal.

    # OpenAI "gpt-4o" seeded rates (USD / 1M tokens): input=2.50,
    # output=10.00, cached_input=1.25. Case: input_tokens=10000 (TOTAL,
    # includes the 8000 cached), cached_tokens=8000 (subset), output=500.
    # Corrected (non-double-counting) arithmetic:
    #   noncached_input = 10000 - 8000 = 2000
    #   cost = 2000/1e6*2.50 + 8000/1e6*1.25 + 500/1e6*10.00
    #        = 0.005        + 0.01         + 0.005        = 0.02
    # (The buggy pre-fix formula would have charged the full 10000 input
    # tokens PLUS the 8000 cached tokens again: 10000/1e6*2.50 +
    # 8000/1e6*1.25 + 500/1e6*10.00 = 0.025 + 0.01 + 0.005 = 0.04 -- 2x the
    # correct cached-token charge.)
    openai_cost, openai_priced = estimate_cost("openai", "gpt-4o", 10000, 500, 8000)
    if not openai_priced:
        raise AssertionError("expected openai/gpt-4o to be priced")
    _assert_close(openai_cost, 0.02, 1e-9,
                  "estimate_cost openai cached-as-subset (FIX 1 regression)")

    # Anthropic "claude-sonnet" seeded rates: input=3.00, output=15.00,
    # cached_input=0.30. Case: input_tokens=100000 (EXCLUDES cache reads,
    # per anthropic's additive convention), cached_tokens=20000 (additive,
    # separate from input_tokens), output=5000.
    #   cost = 100000/1e6*3.00 + 20000/1e6*0.30 + 5000/1e6*15.00
    #        = 0.3            + 0.006          + 0.075        = 0.381
    # Use claude-sonnet-5 (substring-matches the "claude-sonnet" key -> priced);
    # a legacy id like claude-3-5-sonnet-20241022 does NOT match and is unpriced
    # post-P1(d). Rates are the same "claude-sonnet" ones, so 0.381 is unchanged.
    anthropic_cost, anthropic_priced = estimate_cost(
        "anthropic", "claude-sonnet-5", 100000, 5000, 20000
    )
    if not anthropic_priced:
        raise AssertionError("expected anthropic/claude-sonnet-5 to be priced")
    _assert_close(anthropic_cost, 0.381, 1e-9,
                  "estimate_cost anthropic additive cached tokens (FIX 1 regression)")

    # xAI "grok-4.3" seeded rates: input=1.25, output=2.50, cached_input=0.20.
    # xai is a SUBSET-convention provider (cached_in_input), so cached tokens
    # are subtracted from input before charging. Case: input_tokens=10000
    # (TOTAL, includes 4000 cached), cached=4000, output=1000.
    #   noncached_input = 10000 - 4000 = 6000
    #   cost = 6000/1e6*1.25 + 4000/1e6*0.20 + 1000/1e6*2.50
    #        = 0.0075        + 0.0008        + 0.0025        = 0.0108
    # (grok-4.3's specific entry exercises the xai cached-as-subset accounting;
    # grok-4.5 is now priced too but with a different cached rate.)
    xai_cost, xai_priced = estimate_cost("xai", "grok-4.3", 10000, 1000, 4000)
    if not xai_priced:
        raise AssertionError("expected xai/grok-4.3 to be priced")
    _assert_close(xai_cost, 0.0108, 1e-9,
                  "estimate_cost xai cached-as-subset (grok-4.3)")

    # local inference: every rate is 0.0, so any token counts report $0.00 --
    # but the provider IS priced (the entry exists), distinguishing "$0.00 by
    # policy" from "n/a (no pricing)".
    local_cost, local_priced = estimate_cost("local", "qwen3:32b", 50000, 8000, 3000)
    if not local_priced:
        raise AssertionError("expected local/qwen3:32b to be priced (at $0.00)")
    _assert_close(local_cost, 0.0, 1e-12,
                  "estimate_cost local inference is $0.00")

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

    # --- FIX 4 regression: a leaf with BOTH an unrecognized provider token
    # AND a model segment must never be guess-split into a wrong-but-
    # plausible (scenario, provider) -- it must come back None so the caller
    # skips it with a warning instead of silently pulling foreign data. ---
    parsed = parse_run_dir_name("s5__mockprov__mockmodel__r1")
    if parsed is not None:
        raise AssertionError(
            f"parse_run_dir_name must not guess-split an unrecognized "
            f"provider+model leaf, got {parsed}"
        )

    # --- provider-token scan direction regression: a scenario id that
    # itself contains a standalone provider-name token (here "local", from a
    # sanitized "test, local, edit") must not be mistaken for the real
    # provider token, which always comes AFTER the scenario id. The real
    # provider here is "anthropic". ---
    parsed = parse_run_dir_name("test__local__edit__anthropic__claude-3-5-sonnet__r1")
    if parsed != ("test__local__edit", "anthropic", "claude-3-5-sonnet", 1):
        raise AssertionError(
            f"parse_run_dir_name must pick the provider token AFTER the "
            f"scenario id, not a provider-name-shaped token embedded in the "
            f"scenario id itself, got {parsed}"
        )

    # --- FIX 3 regression: per-group pass@1 and the pooled OVERALL rollup.
    # compute_group_stats() takes in-memory RunRecord lists directly (no file
    # IO needed), and compute_overall_rollup() takes the resulting stats
    # dicts. Build two groups with DIFFERENT n so the pooled aggregate and
    # the naive mean-of-per-group-pass@1 provably differ. ---
    def _mk_rec(all_passed):
        r = RunRecord()
        r.check = {"allPassed": all_passed, "checkpointFraction": 1.0 if all_passed else 0.0}
        r.result = {"terminalStatus": "success" if all_passed else "failed",
                    "wallMs": 1200 if all_passed else 800}
        r.input_tokens = r.output_tokens = r.cached_tokens = 0
        return r

    # Group A: 10 repeats, 8 successes -> pass@1 = 0.8
    group_a = [_mk_rec(True)] * 8 + [_mk_rec(False)] * 2
    stats_a = compute_group_stats(("scenA", "anthropic", ""), group_a)
    if stats_a["n"] != 10 or stats_a["successes"] != 8:
        raise AssertionError(f"group A n/successes wrong: {stats_a}")
    _assert_close(stats_a["pass_at_1"], 0.8, 1e-12, "group A pass@1 = successes/n")
    # wall-time mean over all runs: (8*1200 + 2*800)/10 = 1120 ms
    _assert_close(stats_a["mean_wall_ms"], 1120.0, 1e-9, "mean_wall_ms = mean run wall-clock")

    # wallMs == -1 (load_error, never timed) and a MISSING wallMs are both
    # excluded from the mean -- only genuinely-timed runs count.
    r_untimed = RunRecord()
    r_untimed.check = {"allPassed": True, "checkpointFraction": 1.0}
    r_untimed.result = {"terminalStatus": "load_error", "wallMs": -1}
    r_untimed.input_tokens = r_untimed.output_tokens = r_untimed.cached_tokens = 0
    r_nowall = _mk_rec(True)
    del r_nowall.result["wallMs"]
    stats_mixed = compute_group_stats(("scenC", "anthropic", ""), [_mk_rec(True), r_untimed, r_nowall])
    _assert_close(stats_mixed["mean_wall_ms"], 1200.0, 1e-9,
                  "mean_wall_ms excludes -1 and missing (only the one timed 1200ms run counts)")

    # Group B: 2 repeats, 0 successes -> pass@1 = 0.0
    group_b = [_mk_rec(False)] * 2
    stats_b = compute_group_stats(("scenB", "anthropic", ""), group_b)
    if stats_b["n"] != 2 or stats_b["successes"] != 0:
        raise AssertionError(f"group B n/successes wrong: {stats_b}")
    _assert_close(stats_b["pass_at_1"], 0.0, 1e-12, "group B pass@1 = successes/n")

    # Pooled OVERALL rollup: sum(successes)/sum(n) = (8+0)/(10+2) = 8/12 =
    # 0.6666666666666666 -- NOT the naive mean of per-group pass@1, which
    # would be (0.8 + 0.0) / 2 = 0.4. The two must differ given unequal n,
    # proving the rollup is pooled and not a mean-of-means.
    overall = compute_overall_rollup([stats_a, stats_b])
    if overall["total_n"] != 12 or overall["total_successes"] != 8:
        raise AssertionError(f"overall total_n/total_successes wrong: {overall}")
    _assert_close(overall["pass_at_1"], 8.0 / 12.0, 1e-12,
                  "pooled OVERALL pass@1 = sum(successes)/sum(n)")
    naive_mean = (stats_a["pass_at_1"] + stats_b["pass_at_1"]) / 2.0
    if abs(overall["pass_at_1"] - naive_mean) < 1e-9:
        raise AssertionError(
            "pooled OVERALL rollup must differ from the naive mean-of-"
            "per-group-pass@1 when group sizes differ (test is degenerate)"
        )

    # --- P1 report-side staleness surfacing (check_staleness) + P2 provider/
    # model preference (scan_run_dir). See the content-aware-resume-guard and
    # lossy-model-id findings. ---

    # Anchor the STALE cross-check to a REAL committed scenario's actual
    # checkpoint count (read from disk, so this test never rots if that
    # scenario's checkpoints[] later change).
    _param_edit_path = os.path.join("evals", "scenarios", "param_edit.json")
    with open(_param_edit_path, "r", encoding="utf-8") as _f:
        _param_edit_count = len(json.load(_f).get("checkpoints", []))

    # (1) a stamped count that does NOT match the scenario file's real count ->
    # a STALE warning naming the scenario.
    _rec_stale = RunRecord()
    _rec_stale.scenario_id = "param_edit"
    _rec_stale.dir_path = "fake/param_edit__anthropic__r1"
    _rec_stale.result = {"scenarioContentHash": "deadbeef",
                         "scenarioCheckpointCount": _param_edit_count + 1}
    _w = []
    check_staleness([_rec_stale], _w)
    if not any("STALE" in m and "param_edit" in m for m in _w):
        raise AssertionError(f"check_staleness should flag a wrong checkpoint count as STALE; got {_w}")
    if _rec_stale.stale_marker != "stale":
        raise AssertionError(
            f"check_staleness must tag a checkpoint-count-mismatch record's "
            f"stale_marker == 'stale'; got {_rec_stale.stale_marker!r}")

    # (2) a MATCHING count + a present hash -> no staleness / mixed-version noise.
    _rec_ok = RunRecord()
    _rec_ok.scenario_id = "param_edit"
    _rec_ok.dir_path = "fake/param_edit__anthropic__r1"
    _rec_ok.result = {"scenarioContentHash": "cafef00d",
                      "scenarioCheckpointCount": _param_edit_count}
    _w = []
    check_staleness([_rec_ok], _w)
    if any("STALE" in m or "mixed scenario versions" in m for m in _w):
        raise AssertionError(f"a current, matching-count result must not warn STALE/mixed; got {_w}")
    if _rec_ok.stale_marker != "":
        raise AssertionError(
            f"a current, matching-count result must not be stale_marker-tagged; "
            f"got {_rec_ok.stale_marker!r}")

    # (3) a result missing scenarioContentHash entirely -> an "unversioned" warning.
    _rec_unver = RunRecord()
    _rec_unver.scenario_id = "param_edit"
    _rec_unver.dir_path = "fake/param_edit__anthropic__r1"
    _rec_unver.result = {"terminalStatus": "final_text"}  # no hash stamped (pre-P1 result)
    _w = []
    check_staleness([_rec_unver], _w)
    if not any("unversioned" in m for m in _w):
        raise AssertionError(f"a result with no scenarioContentHash must warn 'unversioned'; got {_w}")
    if _rec_unver.stale_marker != "unversioned":
        raise AssertionError(
            f"a hash-less result must be tagged stale_marker == 'unversioned'; "
            f"got {_rec_unver.stale_marker!r}")

    # (4) two records in one scenario group with DIFFERENT hashes -> a
    # "mixed scenario versions" warning. Point them at a nonexistent scenario id
    # so the per-file count cross-check is skipped and ONLY the intra-group hash
    # divergence fires.
    _rec_h1 = RunRecord()
    _rec_h1.scenario_id = "no_such_scenario_xyz"
    _rec_h1.dir_path = "fake/a__anthropic__r1"
    _rec_h1.result = {"scenarioContentHash": "1111111111111111"}
    _rec_h2 = RunRecord()
    _rec_h2.scenario_id = "no_such_scenario_xyz"
    _rec_h2.dir_path = "fake/a__anthropic__r2"
    _rec_h2.result = {"scenarioContentHash": "2222222222222222"}
    _w = []
    check_staleness([_rec_h1, _rec_h2], _w)
    if not any("mixed scenario versions" in m for m in _w):
        raise AssertionError(f"two differing hashes in one group must warn 'mixed scenario versions'; got {_w}")

    # (6) reporter-side table tagging: a group containing a stale_marker-tagged
    # record must carry a "†"/"!" suffix on its rendered scenario-id cell (both
    # text and markdown renderers), and the OVERALL line must carry the
    # "contains unversioned/stale" note. Build one CLEAN group and one
    # unversioned-tagged group + one stale-tagged group so the clean group's
    # cell must NOT be marked while the other two must.
    _rec_clean = _mk_rec(True)
    _rec_clean.scenario_id = "scenClean"
    _rec_clean.result["scenarioContentHash"] = "abc123"
    check_staleness([_rec_clean], [])  # present hash, no on-disk scenario file -> no tag

    _rec_group_unver = _mk_rec(True)
    _rec_group_unver.scenario_id = "scenUnver"
    # no scenarioContentHash stamped -> tagged "unversioned" by check_staleness
    check_staleness([_rec_group_unver], [])

    _rec_group_stale = _mk_rec(True)
    _rec_group_stale.scenario_id = "param_edit"
    _rec_group_stale.result["scenarioContentHash"] = "deadbeef"
    _rec_group_stale.result["scenarioCheckpointCount"] = _param_edit_count + 1
    check_staleness([_rec_group_stale], [])

    _stats_clean = compute_group_stats(("scenClean", "anthropic", ""), [_rec_clean])
    _stats_unver = compute_group_stats(("scenUnver", "anthropic", ""), [_rec_group_unver])
    _stats_stale = compute_group_stats(("param_edit", "anthropic", ""), [_rec_group_stale])

    if _stats_clean["has_unversioned"] or _stats_clean["has_stale"]:
        raise AssertionError(f"a clean group must not be tagged; got {_stats_clean}")
    if not _stats_unver["has_unversioned"] or _stats_unver["has_stale"]:
        raise AssertionError(f"the unversioned group must be tagged has_unversioned only; got {_stats_unver}")
    if _stats_stale["has_unversioned"] or not _stats_stale["has_stale"]:
        raise AssertionError(f"the stale group must be tagged has_stale only; got {_stats_stale}")

    _text = render_text_report([_stats_clean, _stats_unver, _stats_stale], [])
    _first_tokens = {line.split()[0] for line in _text.splitlines() if line.strip()}
    if "scenClean" not in _first_tokens:
        raise AssertionError(f"clean group's scenario cell must render unmarked as 'scenClean'; text table:\n{_text}")
    if "scenUnver†" not in _first_tokens:
        raise AssertionError("unversioned group's scenario cell must carry '†'; text table:\n" + _text)
    if "param_edit!" not in _first_tokens:
        raise AssertionError("stale group's scenario cell must carry '!'; text table:\n" + _text)
    if "contains unversioned/stale rows" not in _text:
        raise AssertionError("OVERALL line must note the presence of tagged rows; text table:\n" + _text)
    if "† = contains unversioned" not in _text or "! = contains results graded" not in _text:
        raise AssertionError("legend must define the †/! markers; text table:\n" + _text)

    _md = render_markdown_report([_stats_clean, _stats_unver, _stats_stale], [])
    if "| scenClean |" not in _md:
        raise AssertionError(f"clean group's markdown cell must not carry a †/! marker:\n{_md}")
    if "| scenUnver† |" not in _md:
        raise AssertionError(f"unversioned group's markdown cell must carry '†':\n{_md}")
    if "| param_edit! |" not in _md:
        raise AssertionError(f"stale group's markdown cell must carry '!':\n{_md}")
    if "contains unversioned/stale rows" not in _md:
        raise AssertionError(f"markdown OVERALL line must note tagged rows:\n{_md}")

    # (5) scan_run_dir prefers the TRUE stamped provider/model over the lossy
    # dir-name parse (P2). Build a leaf with NO model segment (so the dir parse
    # alone yields model=="") but stamp a real model inside result.jsonl.
    _tmp = tempfile.mkdtemp(prefix="eval_report_selftest_")
    try:
        _leaf_dir = os.path.join(_tmp, "myScenario__anthropic__r1")
        os.makedirs(_leaf_dir)
        with open(os.path.join(_leaf_dir, "myScenario.result.jsonl"), "w", encoding="utf-8") as _f:
            _f.write(json.dumps({"scenarioId": "myScenario", "provider": "anthropic",
                                 "model": "claude-sonnet-5-true-name"}) + "\n")
        _recs = scan_run_dir(_tmp, [])
        if len(_recs) != 1:
            raise AssertionError(f"scan_run_dir should find exactly 1 record; got {len(_recs)}")
        if _recs[0].model != "claude-sonnet-5-true-name":
            raise AssertionError(
                "scan_run_dir must prefer the stamped model over the (model-less) "
                f"dir-name parse; got {_recs[0].model!r}")
        if _recs[0].provider != "anthropic":
            raise AssertionError(f"scan_run_dir provider wrong; got {_recs[0].provider!r}")
    finally:
        shutil.rmtree(_tmp, ignore_errors=True)

    # (6) Finding 2 cross-language pin: fnv1a64_hex must stay bit-identical
    # to the C++ FnvHex helper (src/Library/Agent/AgentEvalRunner.cpp). See
    # AgentEvalReplayTest.cpp for the C++-side half of this pin.
    _hello_fnv = fnv1a64_hex(b"hello")
    if _hello_fnv != "a430d84680aabd0b":
        raise AssertionError(
            f"fnv1a64_hex cross-language pin mismatch: FNV-1a 64 of b'hello' "
            f"must be 'a430d84680aabd0b'; got {_hello_fnv!r}"
        )

    # (7) Finding 2: raw file-byte FNV staleness cross-check (scenarioFileFnv).
    # Exercise both branches -- a stamp matching the current file's bytes must
    # NOT warn/tag; a stamp that does not match must warn STALE + tag "stale".
    # Uses a TEMP "evals/scenarios/" directory (via a chdir) so this test never
    # depends on, or perturbs, the real committed scenario suite.
    _tmp_fnv = tempfile.mkdtemp(prefix="eval_report_selftest_fnv_")
    _orig_cwd = os.getcwd()
    try:
        os.makedirs(os.path.join(_tmp_fnv, "evals", "scenarios"))
        _scn_bytes = b'{"id": "fnv_test_scenario", "checkpoints": []}'
        _scn_path = os.path.join(_tmp_fnv, "evals", "scenarios", "fnv_test_scenario.json")
        with open(_scn_path, "wb") as _f:
            _f.write(_scn_bytes)
        _correct_fnv = fnv1a64_hex(_scn_bytes)

        os.chdir(_tmp_fnv)

        _rec_fnv_match = RunRecord()
        _rec_fnv_match.scenario_id = "fnv_test_scenario"
        _rec_fnv_match.dir_path = "fake/fnv_test_scenario__anthropic__r1"
        _rec_fnv_match.result = {"scenarioId": "fnv_test_scenario", "scenarioContentHash": "somehash",
                                 "scenarioFileFnv": _correct_fnv}
        _w = []
        check_staleness([_rec_fnv_match], _w)
        if any("STALE" in m for m in _w):
            raise AssertionError(f"a matching scenarioFileFnv must not warn STALE; got {_w}")
        if _rec_fnv_match.stale_marker != "":
            raise AssertionError(
                f"a matching scenarioFileFnv must not be stale-tagged; got {_rec_fnv_match.stale_marker!r}")

        _rec_fnv_mismatch = RunRecord()
        _rec_fnv_mismatch.scenario_id = "fnv_test_scenario"
        _rec_fnv_mismatch.dir_path = "fake/fnv_test_scenario__anthropic__r1"
        _rec_fnv_mismatch.result = {"scenarioId": "fnv_test_scenario", "scenarioContentHash": "somehash",
                                    "scenarioFileFnv": "0" * 16}
        _w = []
        check_staleness([_rec_fnv_mismatch], _w)
        if not any("STALE" in m and "file hash mismatch" in m for m in _w):
            raise AssertionError(
                f"a mismatching scenarioFileFnv must warn STALE (file hash mismatch); got {_w}")
        if _rec_fnv_mismatch.stale_marker != "stale":
            raise AssertionError(
                f"a mismatching scenarioFileFnv must be tagged stale; got {_rec_fnv_mismatch.stale_marker!r}")
    finally:
        os.chdir(_orig_cwd)
        shutil.rmtree(_tmp_fnv, ignore_errors=True)

    # (8) Finding 1b: discovery reversibility. A dir-leaf whose right-to-left
    # provider scan MISPARSES (a legal model id itself containing "__", e.g.
    # "gpt__xai" inside "param_edit__openai__gpt__xai__r1") must still resolve
    # to the STAMPED identity via scan_run_dir's *.result.jsonl glob, not the
    # dir-parse's wrong guess.
    _tmp_disc = tempfile.mkdtemp(prefix="eval_report_selftest_disc_")
    try:
        _leaf = "param_edit__openai__gpt__xai__r1"
        # Sanity: prove the dir-leaf parse alone WOULD misparse this leaf (the
        # premise of this test) before showing scan_run_dir corrects it.
        _misparsed = parse_run_dir_name(_leaf)
        if _misparsed is None or _misparsed[1] != "xai" or _misparsed[2] != "":
            raise AssertionError(
                f"test premise broken: expected parse_run_dir_name({_leaf!r}) to "
                f"misparse provider='xai'/model=''; got {_misparsed}"
            )
        _leaf_dir = os.path.join(_tmp_disc, _leaf)
        os.makedirs(_leaf_dir)
        with open(os.path.join(_leaf_dir, "param_edit.result.jsonl"), "w", encoding="utf-8") as _f:
            _f.write(json.dumps({"scenarioId": "param_edit", "provider": "openai",
                                 "model": "gpt__xai"}) + "\n")
        _recs = scan_run_dir(_tmp_disc, [])
        if len(_recs) != 1:
            raise AssertionError(f"scan_run_dir should find exactly 1 record; got {len(_recs)}")
        _r = _recs[0]
        if (_r.scenario_id, _r.provider, _r.model) != ("param_edit", "openai", "gpt__xai"):
            raise AssertionError(
                "scan_run_dir must recover the STAMPED identity from a misparsed dir-leaf; "
                f"got scenario_id={_r.scenario_id!r} provider={_r.provider!r} model={_r.model!r}"
            )
        if _r.rep != 1:
            raise AssertionError(f"scan_run_dir rep wrong; got {_r.rep!r}")
    finally:
        shutil.rmtree(_tmp_disc, ignore_errors=True)

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
