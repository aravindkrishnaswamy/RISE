---
name: adversarial-code-review
description: |
  Launch 2-3 independent reviewers with orthogonal concerns in parallel
  to validate a non-trivial code change.  Use when: the change affects
  correctness-sensitive code, crosses a public API boundary, touches
  threading or concurrency, or the user asks for "adversarial review",
  "multiple reviewers", or a second opinion.  Also use proactively
  after shipping a change the user flagged as risky.
---

# Adversarial Code Review

## When To Use

- A non-trivial change has just landed and you want it validated.
- The change is correctness-sensitive (sampling math, threading,
  numerical stability, public-API contracts).
- The user explicitly asks for multiple reviewers / an adversarial
  pass / a second opinion.
- Proactively, after finishing any change the user flagged as "make
  sure this is right" or similar.

## When NOT To Use

- Trivial refactors (renames, moves, single-line fixes) where a
  self-check is adequate.
- Exploration or prototype code where correctness is not yet a goal.
- Pure documentation or comment changes.
- When you have not yet actually completed the change — reviewers
  should review finished code, not speculate about a work in progress.

## Procedure

### 1. Pick orthogonal reviewer axes

Two reviewers with overlapping remits produce correlated findings and
a shared blind spot.  Before launching, write down the axes on which
the change could be wrong and pick 2-3 genuinely independent lenses.
Typical axes for a rendering change:

- **Mathematical / algorithmic correctness** — Is the math right?
  Does it match the reference paper / established derivation?
- **Thread safety** — Races, lifetime, reentrancy, flush points,
  memory ordering.
- **API / plumbing integrity** — Do parser → Job → API → constructor
  signatures all agree?  Reference counting?  Name hiding?
- **ABI preservation** — Virtual-method order in headers, exported
  function signatures, derived-class overload resolution.
- **Numerical stability** — NaN / Inf paths, divide-by-zero, grazing
  angles, clamps.
- **Memory / allocation** — Sizes, alignment, heap vs stack, cap
  behavior.

Pick axes that do not overlap.  "Math correctness" and "numerical
stability" overlap; do not pair them.  "Math correctness" and "thread
safety" do not overlap; pair them.

### 2. Write self-contained prompts

Each reviewer starts from zero context — it has not seen the
conversation.  Every prompt must include:

- **One paragraph describing what changed** — plain English, the
  reviewer's mental model of the problem.
- **The specific concern for THIS reviewer** — what lens they bring,
  what they should look for.
- **Absolute file paths to the critical files** — do not make the
  reviewer hunt.  List the central file plus any files whose contract
  the change depends on.
- **Pointed questions** — explicit, lettered questions ("A) Does the
  discrete footprint sum to unity?  B) Are the loop bounds inclusive
  on both ends?") not "find bugs."
- **A word cap** — "Report in under 400 words" or similar.  Without a
  cap, reviewers produce prose the user will not read.
- **An instruction to report concretely** — "file:line + a concrete
  failure scenario for each finding."  Without it, you get "this
  could be cleaner."

### 3. Launch in parallel

Send a single tool-call batch with all reviewers in one message.
They run concurrently, cut wall time, and — crucially — cannot
influence each other because they never see each other's output.

Serial dispatch (one after another) wastes wall time AND tempts the
reviewer to defer to earlier findings.

### 4. Verify each finding before fixing

Reviewers are capable but not infallible.  A false-positive finding
acted on is worse than no review: you spent time "fixing" something
that was correct and may have broken it.

For every non-trivial finding, spend 30 seconds confirming:

- Read the cited file:line yourself.
- Trace the call graph if the claim is about a missing call.
- Check inheritance if the claim is about a missing method.
- Confirm the concrete failure scenario is reachable.

Cheap to do, invaluable when it catches a false positive.

### 5. Fix the real findings, then iterate

After fixing, run a second round with narrower scope — "the issues
from round 1 have been addressed per the summary below; please
re-review the specific files and confirm."  Or launch a fresh
reviewer on a different axis that round 1 did not cover.

Rounds 2-5 reliably find bugs that round 1 missed.  Do not stop after
round 1 unless the change is small.

### 6. Stop

Stop when one of:

- A round returns no new P1 or P2 findings.
- The user says "good" or equivalent.
- You have reached the reasonable limit of review depth for the
  change size (typically 2-5 rounds).

## Anti-patterns

### Overlapping reviewers

Two reviewers both focused on "correctness" will surface the same
issues and miss the ones neither is looking for.  If you catch
yourself writing two prompts that rhyme, merge them and replace one
with a genuinely different lens.

### Vague prompts

"Here is the codebase, look for bugs in the change" produces
essays, not findings.  Force the reviewer into specifics with a
numbered question list.

### Acting without verification

Reviewers have confidently flagged things that turned out to already
be handled by inheritance or by a different code path.  Always check
before "fixing."

### Stopping after one round

The first round catches the loudest bugs.  The quieter ones — ABI
layout, name hiding, stream aliasing, per-pixel vs per-sample
normalization — emerge in later rounds once the noisy bugs are out
of the way.

### Unbounded reports

Without a word cap, reviewers default to prose.  With a cap, they
default to file:line + concrete failure scenario.  The cap is cheap
and high-leverage; set it every time.

### Reviewers informed by your current answer

Never write "based on the reviewer's findings, fix the bug."  That
delegates synthesis.  Read the findings yourself, decide which are
real, write the fix yourself with file:line specifics.

## Concrete Example (From The Repo)

The MLT pixel-filter work in mid-April 2026 ran five rounds of
adversarial review covering:

- Round 1: math correctness / thread safety / API plumbing
- Round 2: discrete-sum normalization, OIDN invariant violation, API
  ABI, thin-lens RNG independence
- Round 3: stream aliasing in spectral MLT, lens small-step
  continuity, IJob source compatibility
- Round 4: vtable layout ABI, derived-class name hiding
- Round 5: new-caller / old-camera vtable crash

Each round found real issues the previous rounds missed because
earlier rounds focused on the loudest problems.  The cumulative set
of fixes ranged from a half-pixel coordinate offset (round 1) to a
rethink of how ICamera's vtable could be extended without breaking
out-of-tree cameras (round 5).  Skipping any round would have shipped
a real bug.

The pattern was: five rounds × three reviewers = 15 prompts, each
self-contained, each under 400 words, each producing a concrete
finding list.  Total wall-clock for the review work was well under
the total work the fixes represented.

## Stop Rule

A round that returns no P1 or P2 findings — OR the user signaling
satisfaction — means this skill's work is done.  Record the number of
rounds in the final summary so the user can judge the thoroughness.
