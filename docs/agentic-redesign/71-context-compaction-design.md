# 71 — Long-Session Text Compaction (design)

Status: **SHIPPED.** `AgentChatLoop::CompactTranscript` and
`WouldCompactNow` implement the compaction policy; prompt-cache breakpoints live
in `AgentChatCodecs.cpp`. This file is the executed design record.

## 1. Problem

The chat loop's `mTranscript` grows without bound. Every turn re-sends the entire
history (the Messages API is stateless), so a long GUI co-editing session pays,
on every request, for:

- the full assistant reasoning + tool-call echoes of every prior round,
- every tool-result envelope (scene-doc snippets, validate diagnostics,
  object-map JSON, render metadata) ever returned,
- the system prompt + tool defs (fixed prefix — now cached by #1).

Two independent ceilings result:

1. **Cost/latency** — input tokens climb linearly with turn count. Prompt caching
   (#1) flattens the *fixed prefix* cost but does nothing for the *growing
   conversation body*; cache also only covers the byte-identical prefix, and any
   in-place history rewrite (image elision today) invalidates the cache from the
   first rewritten byte onward.
2. **Hard context-window / request-size limit** — a sufficiently long session
   eventually exceeds the model's context window or the provider's request-size
   cap and the turn hard-fails. Image elision (keep-newest render, cap user
   images at 4) bounds the *image* contribution but leaves *text* unbounded.

We manage images. We do not manage text. This doc designs text compaction.

## 2. What already exists (and the contract it must respect)

- **`mTranscript`**: ordered entries. Three provenance classes:
  - *Assistant* entries — raw provider-native JSON, spliced VERBATIM. **Byte-
    preservation contract: never rewritten.** This is load-bearing for the wire
    invariant (Anthropic hard-400s an unanswered `tool_use` id) and for replay.
  - *ToolResults* entries — loop-generated. Legally rewritable in place; this is
    exactly what `RewriteElidedImages` does.
  - *User* entries — loop-generated via `MakeUserEntry`. Legally rewritable;
    `RewriteElidedUserImages` does this for reference-image attachments.
- **Wire invariant**: every recorded tool call is answered in the immediately-
  following user message. Any compaction that removes or replaces entries MUST
  preserve tool_use↔tool_result pairing, or the next `BuildRequest` produces a
  400 (Anthropic) / mismatch (Gemini).
- **Replay / eval contract**: the transcript replays byte-for-byte; the eval
  harness records and replays trajectories. Compaction changes the bytes sent,
  so the design must define replay behavior (see §6).
- **Image elision is the precedent**: in-place rewrite of *loop-generated*
  entries only, replacing heavy content with a short text placeholder, leaving
  assistant entries untouched. Text compaction should follow the same boundary.

## 3. Constraints (hard)

C1. **Never rewrite an assistant entry's bytes.** Compaction may *drop* an
    assistant entry (with its paired tool_result) or *replace a contiguous run*
    of entries with a single synthesized summary entry, but it may not edit an
    assistant entry in place. Dropping is the only assistant-side move.

C2. **Preserve tool_use↔tool_result pairing.** The unit of compaction is a whole
    *round* (assistant turn + its following tool-results user entry), or a whole
    *user-turn span* (user msg → …rounds… → final assistant text), never a
    partial round. Cutting between a tool_use and its answer is illegal.

C3. **Never drop the head-version anchor.** The current scene document / head
    version the agent edits against must remain reconstructable. The most recent
    `read_document` result and the live head version are load-bearing; compaction
    must retain (or re-materialize) them.

C4. **Deterministic given the same transcript** (for replay/eval reproducibility)
    — or explicitly disabled under replay. See §6.

C5. **Budget-driven, not turn-count-driven.** Trigger on an estimated-token
    threshold, not "every N turns" — turns vary wildly in token weight (a
    validate dump vs. a one-line ack).

## 4. Options

### Option A — Summarize-old-turns (LLM-generated running summary)

When estimated transcript tokens exceed a high-water mark, take the oldest
*closed* user-turn spans (everything before the last K live turns), send them to
a cheap model with a "summarize what was decided/edited and the current scene
state" prompt, and replace that whole prefix span with a single synthesized
`User`/system summary entry ("[Earlier in this session: …]").

- **Pros**: highest compression; preserves *semantic* continuity (decisions,
  naming, why-we-did-X) that a mechanical window loses.
- **Cons**: (a) an extra LLM call per compaction (cost + latency + a new failure
  mode); (b) **non-deterministic** → breaks replay unless the summary is recorded
  into the trajectory and replayed verbatim (doable, but couples compaction to
  the recorder); (c) summary fidelity risk — a dropped constraint the agent still
  needed becomes an invisible regression the eval must catch; (d) must still obey
  C2/C3 (summarize whole spans; keep the head anchor live).

### Option B — Head-version-anchored structural compaction (no LLM)

Exploit that RISE has a *canonical externalizable state*: the scene document.
Instead of summarizing prose, drop the historical *derivation* and keep the
*result*. Concretely, when over budget:

- Retain: the system prompt (cached), the newest `read_document` snapshot (or
  inject a fresh head-version doc as a synthesized tool-result), the last K live
  turns verbatim, and a compact structured ledger of edits applied so far
  ("chunks inserted/removed, params changed, current head version N").
- Drop: older rounds whose *effect* is already captured by the current head
  version — old propose_patch/insert/remove echoes and their result envelopes,
  old validate dumps, old render metadata.

- **Pros**: **deterministic** (pure function of the transcript + current head
  version) → replay-safe with no recorder coupling; no extra LLM call; leans on
  RISE's real invariant (the scene doc *is* the accumulated state, so old edit
  derivations are genuinely redundant once applied); naturally satisfies C3.
- **Cons**: loses *conversational* intent that isn't reflected in the doc (e.g.
  "the user said keep it subtle" — a preference, not a chunk). Mitigation: keep
  the *user* messages of dropped spans (they're small and carry intent) while
  dropping the bulky assistant/tool-result derivation — a hybrid.

### Option C — Sliding window (drop oldest whole spans, no summary)

Keep the last K user-turn spans verbatim; drop older spans entirely (respecting
C2/C3 — always keep the head anchor even if it falls outside the window, by
re-materializing a fresh head-version doc entry).

- **Pros**: trivial, deterministic, zero extra calls.
- **Cons**: hard amnesia at the window edge; the agent forgets earlier decisions
  and can undo/re-litigate them. Worst task-success profile of the three.

## 5. Recommendation

**Option B (head-version-anchored structural compaction), with the Option-B
hybrid of retaining dropped spans' *user* messages.** Rationale:

- It is the only option that is both **deterministic** (replay-safe, C4 satisfied
  with zero recorder coupling) and **semantically sound** for *this* app —
  because RISE has a canonical state object, "old edit derivations are redundant
  once applied" is *true here* in a way it isn't for a generic chat agent. We get
  most of Option A's continuity without the extra LLM call or the
  non-determinism.
- Keeping dropped spans' short user messages preserves stated intent/preferences
  (the main thing pure structural compaction loses) at negligible token cost.
- It composes cleanly with #1's cache: the retained prefix (system + tools + the
  synthesized head-version anchor) is stable across turns *until a compaction
  event fires*, which is rare (only at the high-water mark). Between compactions
  the growing tail is what changes — same profile as today.

Option A is a *future* upgrade if structural compaction proves to lose too much
intent in practice (the eval harness will tell us — §6). Option C is the
fallback if B's ledger proves fiddly; it's strictly worse but trivially correct.

### Compaction interaction with prompt caching (#1)

- A compaction event rewrites the prefix → invalidates the Anthropic cache and
  costs one cold write on the next turn. This is fine: compaction fires rarely
  (at the high-water mark), and the whole point is that the *post*-compaction
  prefix is far smaller, so the cold write is cheap and every subsequent turn
  reads the smaller cached prefix. Net win.
- Place the compaction high-water mark well below the context window but well
  above the cache-minimum, so we don't thrash (compact → grow → compact) turn
  over turn. Suggested: trigger at ~50% of the model's context window, compact
  down to ~25%, giving hysteresis.

## 6. Replay / eval-harness validation

This is the reason to build compaction *in this codebase*: the harness can prove
it doesn't regress task success.

- **Determinism**: Option B is a pure function of (transcript, current head
  version), so a replayed trajectory reproduces the identical compaction. No
  recorder changes needed. (Option A would require recording the summary text
  into the trajectory and replaying it verbatim — a reason B wins.)
- **Validation protocol**:
  1. Take the existing A1–A5 + E6 scenarios (multi-round editing/recovery).
  2. Run each **with compaction disabled** (baseline) and **with compaction
     enabled at an artificially low high-water mark** (forces compaction to fire
     mid-scenario, exercising the drop/anchor logic on short transcripts).
  3. Assert the checker verdict (document/render/objectmap/trajectory
     checkpoints) is unchanged between the two runs — i.e. compaction changed the
     token cost, not the outcome.
  4. Report `$/successful-task` and input-token deltas via `eval_report.py` to
     quantify the savings.
- **New scenario**: a deliberately long multi-turn "torture" scenario (≥ the
  natural high-water mark) whose late turns depend on an early decision — the
  discriminating test for "did compaction drop something load-bearing." Under
  Option C this should *fail* (amnesia); under Option B (hybrid, user-messages
  retained) it should *pass*. That contrast is the design's acceptance test.

## 6b. Within-span supersession elision (SHIPPED 2026-07-27, ahead of S2's gate)

Span compaction (§7 S1-S4) is a **cross-turn** mechanism: its unit is a span,
and a span begins at a `User` entry. That makes it structurally unable to help
a whole class of blow-up that happens *inside one turn* — including the most
common one in the measured GUI corpus.

(Scoping that honestly: the single **largest** recorded context in the corpus is
not this shape. `20260710T191019Z-95c9935e` peaked at 141 K provider-reported
input, driven by one bare `read_schema` returning the 305 KB whole-grammar dump
— a shape that predates the cheap `{category:…}` listing mode the tool
description now steers to, and one this rule does **not** address, because
`read_schema` fails admission properties (1) and (2). What follows is the
most-frequent within-turn blow-up, not the largest single one.)

**The measurement** (trajectory `20260727T063526Z-a7ee472c`): ONE user message
("make the middle object red"), 23 model rounds, **six** `read_document` calls
each returning a 19.8 KB document (a ~21.5 KB JSON-RPC response),
provider-reported input growing **10 K -> 68 K** tokens — to check a
**three**-parameter patch (three `propose_patch` calls of ~183 bytes each). Every one of those six lived in a **single span**, so
`CompactTranscript` would have dropped nothing (`kMinRetainedSpans` = 2 floors
it at the previous + current turn). A second session (`20260727T064005Z-d815b0c7`) made five `read_skill`
calls -- the index plus four named skills, 52 KB of markdown (58 KB of
JSON-RPC responses) -- before
touching the scene.  That one is NOT addressed here: see the allowlist
discussion below for why read_skill results do not supersede each other.

**The rule shipped**: SUPERSEDED-READ RETENTION — the text-side sibling of the
image-elision precedent §2 names. For a read verb whose result is the *whole*
view of one piece of *mutable* state and does not depend on its arguments, only
the MOST RECENT result stays live; older ones are rewritten in place to
`[<verb> result elided -- superseded by a later <verb> call in this
conversation; call <verb> again for the current state]`. Same boundary as image
elision (loop-generated `ToolResults` entries only, C1 untouched), same wire
guarantee (the per-result call binding — `tool_use_id` / `functionResponse.id`
/ `call_id` — is copied through, so C2 holds), and a deterministic pure
function of the transcript, so §6's replay contract holds unchanged.

The allowlist lives in `ChatToolResultSupersessionKey`, whose four admission
properties — and, per excluded verb, either the property it fails or the other
reason it is off the list — are documented on its declaration in
`AgentChatCodecs.h`. Today it holds
`read_document` alone. Two exclusions worth restating here because they look
eligible and are not: `read_schema` / `read_skill` are argument-keyed and
STATELESS (two calls return different, never-stale content), and `render`
results are explicitly meant to be COMPARED across calls, so eliding an older
one would destroy information the tool description tells the model to use.

Note this is a **correctness** improvement as much as a cost one — stated
precisely: **five** of the six documents were byte-identical (19,824 B), i.e.
pure redundancy, and the **sixth** differed (19,828 B), i.e. the head *did* move
under the agent mid-task. That last fact is what makes an older copy potentially
STALE rather than merely wasteful.

**Span compaction was enabled at the same time**, as a secondary measure — but
its reach must be stated honestly, because an earlier draft of this section
overstated it. `CompactTranscript` stops while `spanCount <= kMinRetainedSpans`
(2), so it can only fire in a conversation of **three or more user turns**.
Across the 27-trajectory corpus, 20 sessions have one user turn, 6 have two, and
exactly **one** has three. It therefore does **not** catch the 141 K
`read_schema` session above (one user turn ⇒ one span ⇒ the loop breaks before
erasing anything) — the same single-span reasoning this section already applies
to `a7ee472c`. It is a genuine backstop for long multi-turn sessions and nothing
more. Enabled via
`SetContextBudget` from both GUI drivers (Mac `RISEAgentChatBridge`, Windows
`ChatPanel`) using the shared `AgentChatLoop::kDefaultContextBudgetHighTokens`
/ `...LowTokens` (150 K / 75 K — rationale, and the honest ~1.5× margin over the
only compactable session on record, in that header). It would not have
helped the session above, and is not claimed to; it bounds genuinely long
multi-turn sessions. Until this wiring landed, S2's span-dropper had shipped
but had **never once run in production** — `SetContextBudget` was called from
neither GUI. `SourceHygieneTest` now guards both call sites.

One consequence had to be handled with it, and it bites **both** drivers in
opposite ways. Windows' `ChatPanel` renders the chat directly out of the loop's
transcript, so a compaction event makes the user's visible history *vanish*.
macOS' `ChatViewModel` keeps its own append-only display array, so *nothing*
changes on screen — the panel keeps showing turns the model can no longer see,
and the resulting amnesia is indistinguishable from a model defect, which is
arguably the worse of the two. `AgentChatLoop::CompactedEntryCount()` now
reports how many entries were dropped and **both** drivers surface it (a panel
row on Windows, a `.notice` transcript row on macOS); `SourceHygieneTest` guards
both. Dropping spans from the wire is intended; dropping them from the user's
view — or from the model's memory while the view still shows them — without
saying so is not.

Finally, note that SUPERSEDED-READ RETENTION is **unconditional** (no budget
gates it), so unlike span compaction it applies to the eval runner too. That is
deliberate — it is a correctness rule about stale reads, not a cost knob — but
the 312-run baseline in [70-agent-eval-harness.md](70-agent-eval-harness.md) §6.4
predates it and is no longer a like-for-like comparison.

## 7. Phasing (when built)

- S1: token estimator + high-water/low-water config (no rewriting yet; just
  log when we'd compact). Land the estimator + a `context_budget` eval knob.
- S2: structural span-dropper (Option C core) — deterministic, respects C1/C2.
  A *span* is a maximal run starting at a `User` entry (the transcript always
  starts with one, and a `ToolResults` entry always immediately follows its
  `Assistant` tool_use, per the wire invariant). Compaction drops the OLDEST
  WHOLE spans and keeps the last N. Because whole spans are the unit, the result
  automatically (a) starts at a `User` boundary (Anthropic requires first
  message = user) and (b) never orphans a `tool_result` from its `tool_use`.
  **C3 note**: the sans-IO loop cannot safely *pin* a bare doc-snapshot
  `ToolResults` entry across a drop — it is a user-role wire message that must
  answer the immediately-preceding assistant's tool_use, and pinning it alone
  (or its round, mid-history) risks orphaning / a non-user first message. So S2
  does NOT re-materialize a doc anchor; it relies on `read_document` remaining
  callable and on the retained window still echoing the current `headVersion`
  (which `propose_patch`'s `baseHeadVersion` needs). The proper head-anchor
  ledger is S3 (Option B), which can inject a synthesized snapshot legally.
  Compaction runs in `BuildRequest` AFTER `FlushPendingToolResults` +
  `ElideAllLiveImages` (so there are no unanswered tool_use blocks to split) and
  BEFORE assembling `rawEntries`. Gate: a unit wire-validity test (first msg
  user; tool_use count == tool_result count; last-N spans intact; estimate ≤
  low-water or floored at the min-retained-spans count) + a no-op/idempotence
  test below budget + the A1–A5 runner equality check.
- S3: the hybrid retain-user-messages + edit-ledger (Option B). Gate on the long
  torture scenario passing where C fails.
- S4: (optional, future) Option A summary layer behind a flag, recorded into the
  trajectory for replay — only if S3's structural compaction measurably loses
  intent on real sessions.

Each slice is independently shippable and independently eval-gated; the harness
is the definition of done (no task-success regression vs. the compaction-off
baseline).
