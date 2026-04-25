---
name: adversarial-code-review
description: |
  Launch 2-3 independent reviewers with orthogonal concerns in parallel
  to validate a non-trivial code change.  Use when: the change affects
  correctness-sensitive code, crosses a public API boundary, touches
  threading or concurrency, or the user asks for "adversarial review",
  "multiple reviewers", or a second opinion.  Also use proactively
  after shipping a change the user flagged as risky.  Forces concrete
  findings with severity and failure scenarios, a fix-and-rereview
  loop, and at least one post-fix review round before moving on from
  material issues.
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

### 1. Do a brief self-audit before spawning reviewers

Before launching reviewers, write down:

- the 3-5 ways the change is most likely to be wrong
- the files where a bug would most likely hide
- the contracts that must not regress
- the tests or scenes that would best expose a break

This is not a substitute for review.  It is how you choose better
reviewer axes and better reviewer questions.  If you cannot name the
likely bug classes, your review prompts will be vague.

### 2. Pick orthogonal reviewer axes

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

Also reserve one axis for **blast radius / missing tests** when the
change is broad: ask one reviewer to look specifically for affected
callers, variant implementations, parser/API plumbing, and missing
regression coverage.

### 3. Write self-contained prompts

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
- **A reporting format** — require `severity`, `confidence`,
  `file:line`, and a concrete user-visible or correctness-visible
  failure scenario for each finding.
- **A requirement to call out missing tests** — reviewers should name
  the test or scene they wish existed, not just the bug.
- **A word cap** — "Report in under 400 words" or similar.  Without a
  cap, reviewers produce prose the user will not read.
- **An instruction to report concretely** — "file:line + a concrete
  failure scenario for each finding."  Without it, you get "this
  could be cleaner."
- **An explicit bias toward findings** — "prioritize correctness,
  regressions, and missing coverage; ignore style unless it hides a
  correctness risk."

Recommended finding format:

```text
[P1|confidence 0.84] /abs/path/file.cpp:123
Problem: <one sentence>
Why it can fail: <concrete scenario>
What should cover it: <test or scene>
```

If the reviewer finds nothing material, require an explicit "no P1/P2
findings" statement rather than silence.

### 4. Launch in parallel

Send a single tool-call batch with all reviewers in one message.
They run concurrently, cut wall time, and — crucially — cannot
influence each other because they never see each other's output.

Serial dispatch (one after another) wastes wall time AND tempts the
reviewer to defer to earlier findings.

### 5. Normalize the findings into a ledger

Before fixing anything, make a small ledger:

- `new`
- `confirmed`
- `rejected`
- `fixed`
- `rereviewed`

Each finding gets one row with severity, file, one-line issue summary,
and current status.  This prevents the common failure mode where round
1 findings are half-addressed, forgotten, or not revisited after a
large follow-up patch.

If multiple reviewers reported the same issue, deduplicate into one
ledger row and keep the strongest phrasing / clearest failure
scenario.

### 6. Verify each finding before fixing

Reviewers are capable but not infallible.  A false-positive finding
acted on is worse than no review: you spent time "fixing" something
that was correct and may have broken it.

For every non-trivial finding, spend 30 seconds confirming:

- Read the cited file:line yourself.
- Trace the call graph if the claim is about a missing call.
- Check inheritance if the claim is about a missing method.
- Confirm the concrete failure scenario is reachable.

Cheap to do, invaluable when it catches a false positive.

Reject a finding only if you can state why the cited failure mode is
not reachable.  Record that reason in the ledger.

### 7. Fix the confirmed findings, then ask for more review

After fixing, run a second round with narrower scope — "the issues
from round 1 have been addressed per the summary below; please
re-review the specific files and confirm."  Or launch a fresh
reviewer on a different axis that round 1 did not cover.

Rounds 2-5 reliably find bugs that round 1 missed.  Do not stop after
round 1 unless the change is small.

For any material issue (`P1` / `P2`) or any fix that changed logic in a
meaningful way, a post-fix rereview round is mandatory.  Do not move
on just because the code now "looks right."

When asking for rereview:

- summarize only the fixes, not the expected answer
- point reviewers at the changed files again
- ask whether the fix fully closes the failure mode
- ask what the next most likely hidden bug is now that the noisy ones
  are gone

### 8. Require at least one "what's left?" review round

After addressing the currently known findings, run one more review
round whose job is not to confirm old fixes but to search for the next
most plausible miss.

Prompt shape:

- "Assume at least one meaningful issue may still remain."
- "Ignore already-fixed findings unless the fix is incomplete."
- "Look for the next most likely correctness regression, ABI hazard,
  threading bug, or missing test."

This catches the common failure mode where round 2 merely rubber-stamps
round 1 fixes and nobody asks what was still missed.

### 9. Stop

Stop only when both are true:

- every material finding in the ledger is either `rejected` with a
  concrete reason or `fixed`
- at least one post-fix review round returns no new `P1` / `P2`
  findings

The user saying "good" can still stop the process, but absent that,
do not exit after the first fix pass.

Typical depth remains 2-5 rounds, but broad or correctness-sensitive
changes should bias toward the high end.

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

### No severity / confidence / failure scenario

Without structure, reviewers drift into opinion.  Severity and a
concrete failure scenario force prioritization and make validation
faster.

### Fixing findings without a ledger

If findings live only in chat scrollback, some will be forgotten, some
will be partially fixed, and some will never get rereviewed.

### Acting without verification

Reviewers have confidently flagged things that turned out to already
be handled by inheritance or by a different code path.  Always check
before "fixing."

### Stopping after one round

The first round catches the loudest bugs.  The quieter ones — ABI
layout, name hiding, stream aliasing, per-pixel vs per-sample
normalization — emerge in later rounds once the noisy bugs are out
of the way.

### Stopping after the first fix pass

"I fixed the reported issues, so I’m done" ships follow-on bugs.
Material fixes change the code enough to deserve another adversarial
pass.

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

This skill's work is done when:

- all material findings have explicit ledger status,
- confirmed findings are fixed,
- at least one post-fix adversarial review round returns no new P1/P2
  findings,
- and the final summary states the number of rounds, reviewer axes
  used, and any findings intentionally rejected with rationale.
