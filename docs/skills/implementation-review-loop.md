---
name: implementation-review-loop
description: |
  The DEFINITION OF DONE for implementation work in this repo.  Any code
  change beyond a trivial one-liner is not "done" until a fresh round of
  independent adversarial reviewers returns ZERO P1s.  Use this for ALL
  implementation work — features, refactors, bug fixes, structural changes
  — not only when asked.  Drive the loop yourself: implement → gate (build/
  test/warnings/sanitizers) → spawn 2-4 orthogonal reviewers → fix every P1
  → commit → re-spawn fresh reviewers → repeat until a round on the CURRENT
  state finds no P1.  Builds on adversarial-code-review (single-round
  mechanics) and audit-by-bug-pattern (siblings); adds the convergence loop,
  the per-round gate, the standing cost-honesty + doc-fidelity lenses, and
  the stale-comment-family lesson.
---

# Implementation Review Loop — done means "reviewers find no P1s"

## The mandate

For **all** implementation work in this repo — a feature, a refactor, a bug
fix, a data-structure change — the work is **not done** until a fresh round
of independent adversarial reviewers comes back with **zero P1 findings**.
Not "I fixed the reported issues." Not "tests pass." Not "looks right."
Zero P1s from a review round that looked at the *current* state.

This is not optional polish or a thing you do only when the user asks. It is
the definition of done. Solo verification reliably misses false-green tests,
incomplete fixes, cost-claim lies, and comments that lie about behavior — and
the review loop is what catches them, every time. It is load-bearing.

The user should not have to be the review loop. Drive it yourself.

## When this applies

- Any non-trivial code change (new feature, refactor, bug fix, structural
  edit, performance work, a new data structure or algorithm).
- Especially: correctness-sensitive code, public-API/ABI surfaces, threading,
  numerical stability, persistent/immutable data structures, anything with a
  claimed complexity bound.

## When a lighter touch is enough

- A genuinely trivial one-liner, a rename, a doc-only typo, a comment fix —
  a self-check (build + the obvious test) is adequate; you do not need a
  three-reviewer loop for a one-word change.
- Exploration / throwaway prototype code where correctness is not yet a goal
  (but say so explicitly; "prototype" is not cover for shipping it later
  unreviewed).

If unsure whether a change is "trivial," it isn't — run the loop.

## The loop

```
implement a coherent increment
  └─> SELF-AUDIT: write the 3-5 ways it's most likely wrong + the surfaces a
      bug or a stale claim would hide in
  └─> GATE (must pass before spawning reviewers):
        - clean build, ZERO new warnings (warnings are bugs — CLAUDE.md)
        - full relevant test suite green
        - ASan/UBSan clean for memory-/UB-sensitive code
        - commit on the branch (never push)
  └─> SPAWN 2-4 orthogonal reviewers IN PARALLEL, FRESH each round
        (one message, multiple Agent calls; new agents, not the prior round's)
  └─> read findings yourself; VERIFY each before acting (reviewers false-positive)
  └─> FIX EVERY P1 (provenance is irrelevant — see below); fix P2s opportunistically
  └─> GATE again; commit the round's fixes
  └─> if this round produced any P1  ──► loop (re-spawn fresh reviewers)
      else (zero P1s on the CURRENT state) ──► CONVERGED, done
```

Two consecutive zero-P1 rounds, or one zero-P1 round on the final state after
the last fix, is the stop signal. The converging round MUST have reviewed the
current HEAD — if you changed anything after the clean round (even a comment),
the clean verdict is stale; run one more round on the new state.

## The standing reviewer lenses

Pick orthogonal axes (see adversarial-code-review for why and how). For most
implementation work, three standing lenses plus any domain-specific ones:

1. **Correctness** — invariants, edge cases, lifetime/aliasing/UB, leaks.
   For a data structure: a randomized **differential fuzzer vs a shadow model**
   under ASan/UBSan, asserting every invariant after every op, is the gold
   standard — it found (and re-confirmed clean) the item-4 identity layer
   across billions of assertions.
2. **Cost-honesty** — every claimed complexity bound must be **measured, not
   asserted**. The reviewer reproduces the claim at several N and tries to
   defeat it with an adversarial input. A bound that's wrong *or* undisclosed
   is a P1. Back claims with committed **gate counters** (a `Debug*` visit
   counter the test asserts is sub-linear), not prose. See
   [performance-work-with-baselines](performance-work-with-baselines.md) and
   [variance-measurement](variance-measurement.md).
3. **Design-fidelity / comment-honesty** — does the code deliver the design
   contract (the relevant decisions / spec), and do the comments and docs
   HONESTLY describe what the code does? A comment or doc that materially
   misdescribes current behavior is a **P1**, not a nit (see "doc drift").

Plus domain axes as warranted: math/algorithm, thread-safety, API/ABI,
numerical stability, principled-fix audit (any new threshold/ε — pair with
[precision-fix-the-formulation](precision-fix-the-formulation.md)).

Use **fresh** agents each round — a reviewer asked to re-check its own prior
verdict rubber-stamps. Fresh independent reviewers each round reduce
correlated blind spots.

## Severity: what's a P1

- **P1** — a real defect: wrong result / crash / invariant violation / leak /
  UB; a false or undisclosed cost claim; an unmet design-contract requirement;
  **a comment or doc that materially misdescribes current behavior**.
- **P2** — wording imprecision, a loose-but-honest bound, a missing
  cross-reference, a test that proves less than it claims.

The loop stops on **zero P1s**. Fix P2s too before final sign-off (they are
cheap and they prevent a P2 from being re-raised as a P1 next round), but P2s
do not block a round from being "clean."

## Fix EVERY bug, regardless of provenance

A latent bug in the surrounding code is still a bug. "Pre-existing",
"not introduced by this change", "not reachable today" are NOT grounds to
skip a fix — the only non-fix dispositions are "genuinely not a bug" (with a
stated reason) or an explicit user-approved deferral. See
[adversarial-code-review](adversarial-code-review.md) and
[the feedback memory on provenance]. The item-4 loop's first action was
fixing a *pre-existing* stale-format test failure the rebuild surfaced.

## The doc-drift family (the lesson that made item 4 take seven rounds)

When you change a behavior, the bug is almost never the one comment a reviewer
cites. It is a **family**: the same stale framing lives in the struct comment,
the section comment, the field comment, the function header, the test-file
header, the design facet doc, the decisions footnote, and the slices doc. Fix
the cited one and its twin tees up next round.

**So:** when a reviewer flags a stale/over-claiming comment, do not fix only
that line. **Grep every surface for the OLD framing in one pass and fix the
whole family.** Then proactively grep for the old framing of *every* behavior
you changed this increment, before the next round, so the loop converges
instead of playing whack-a-mole one-comment-per-round.

Corollary, and the deeper lesson: **the code can be correctness-clean while
the docs churn for rounds.** In item 4 the algorithms were proven correct from
round 1; *every* subsequent P1 was a stale comment or a false cost claim. In a
repo whose thesis is "the text is the source of truth," a comment that lies
about behavior is the same class of defect as a wrong line of code — review and
fix it with the same rigor. Cost claims are the same: a complexity bound stated
but not measured is a claim, not a fact; narrow it to the truth or implement
what makes it true.

## Converging honestly when the ideal isn't built yet

If a bound or property can't be met by the shipped v1 (e.g. an order-
maintenance reflow that's O(N) worst-case, not the target O(log N)), you have
two honest moves: **implement the real thing**, or **narrow the claim to the
truth and disclose it** (cite the decision that sanctions a v1 fallback; name
the refinement that would close it; gate-test the honest bound). What you may
NOT do is keep asserting the unmet bound — a false claim is a P1 that will be
re-found every round until the words match reality.

## Per-round hygiene (RISE specifics)

- Build: `make -C build/make/rise build-test/<TestName>` then run the binary;
  the full CST/feature suite as relevant.
- Warnings are bugs — a clean rebuild must be warning-free (touch the .cpp to
  force a recompile when checking; incremental builds hide warnings).
- ASan/UBSan: `g++ -std=gnu++17 -fsanitize=address,undefined -I. <test>.cpp <impl>.cpp`.
- Test-exe relink race: test executables link the library object list, so
  touching a shared .cpp relinks them — run binaries in a phase SEPARATE from
  any concurrent `make`, or a binary read mid-relink reports a phantom failure.
- Commit each round's fixes on the branch with a message that names the round,
  the reviewer verdicts, and each P1 fixed. NEVER push (the user pushes).
- Render sequentially, never in parallel (a render takes all cores).

## Reporting

When the loop converges, report: the number of rounds, the reviewer axes, the
P1s found and fixed per round, the final verdicts (e.g. CLEAN / COSTS HONEST /
FAITHFUL), and any honest residual (a disclosed v1 limitation + the named
refinement). State it plainly; do not claim "done / sound / no P1" on the
strength of narrow tests — claim it on the strength of a fresh zero-P1 round.

## Concrete example (from the repo)

The agentic-redesign **item 4** (NodeId identity + name-path addressing) ran
**five external review rounds (15 P1s) then seven self-driven internal rounds**.
Pattern worth internalizing:

- The **code** was correctness-clean from the first internal round and stayed
  clean — the final round threw ~3.5 billion invariant assertions (differential
  fuzz + shadow model, ASan/UBSan) and found nothing.
- **Every internal-round P1 was doc/claim drift**: reparse-rename "invalidates"
  (it carries lineage), param key "(chunkId, role)" (it's a 3-tuple), reflow
  "O(log² N) amortized" (the gap-exhaustion reflow is Θ(N), making `DocInsertItem`
  **Θ(N·log N) worst-case**, disclosed — matching IMPLEMENTATION_SLICES.md's item-4
  bound). Each lived in a family of surfaces; the loop only converged once the whole
  family was fixed in one pass and all surfaces were grepped proactively.
- A real cost P1 was resolved by **narrowing the claim to the truth** (the
  Θ(N) reflow is a disclosed v1 fallback per D23; Bender level-scaled is the
  named refinement) plus a **gate counter** that witnesses the worst case.
- Converged at rounds 6 and 7: zero P1s; round 7 = CLEAN / COSTS HONEST /
  FAITHFUL across all three lenses.

## Anti-patterns

- **Ping-ponging with the user as the review loop.** Drive it yourself; come
  back when it has converged, not after each single fix pass.
- **Whack-a-mole on comments.** Fixing the one stale comment a reviewer cited
  and waiting for the next round to find its twin. Grep the family; fix it all.
- **Declaring done after one fix pass / on narrow tests.** "Tests pass" ≠
  "reviewers find no P1." Run the loop.
- **Asserting a complexity without measuring it.** A bound is a claim until a
  gate counter proves it at scale against an adversarial input.
- **Re-using the same reviewer agents to "re-check."** They rubber-stamp; spawn
  fresh ones.
- **Stopping on a clean round that reviewed a stale state.** If you edited
  anything after the clean verdict, run one more round on the new state.
- **Treating a comment/doc that lies about behavior as a P2.** It is a P1.

## Stop rule

Done when a fresh adversarial round on the **current** committed state returns
**zero P1s** across the standing lenses, every earlier P1 is fixed (not
deferred without user approval), P2s are cleaned up, the gate (build/warnings/
tests/sanitizers) is green, and the final report states rounds, axes, verdicts,
and any disclosed residual.
