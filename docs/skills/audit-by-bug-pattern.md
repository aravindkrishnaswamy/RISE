---
name: audit-by-bug-pattern
description: |
  When you find a bug, IDENTIFY the pattern and then SWEEP the codebase
  for sibling sites with the same pattern before declaring the fix
  done.  Use immediately after fixing a non-trivial bug, especially
  when the codebase has parallel code paths (RGB vs NM, snell vs
  uniform, biased vs unbiased, RGB vs spectral, sync vs async) — these
  paths drift over time and historically harbour the same bug at
  multiple sites.  The fix isn't done when the user-reported site is
  fixed; it's done when no sibling carries the same pattern.
---

# Audit by Bug Pattern

## When To Use

- You just fixed a bug.  Before declaring done, before committing,
  before responding to the user.
- The bug had a recognisable **pattern**: an API was called with the
  wrong kind of value, a flag was set incorrectly, a check was
  misplaced, etc.
- The codebase has **duplicated code paths** that historically drift:
  - RGB / NM (spectral) twins of the same algorithm.
  - snell / uniform (or any other algorithmic-mode) variants.
  - biased / unbiased branches in the same function.
  - main path / fallback path / photon-aided path / surface-sample
    fallback — these tend to copy each other and share bug families.
  - RGB / spectral / HWSS rasterizer trios.
- A user-reported bug touched ONE site but the bug PATTERN could
  apply to many.  Don't accept "I fixed the line they pointed to" as
  a stopping point.

## When NOT To Use

- The bug was a one-off (typo in a comment, wrong magic number that
  came from a measurement, single-site test fixture).
- The bug pattern can ONLY exist at the fixed site by construction
  (e.g. a singleton-initialisation race in a header-only static).
- You're under time pressure for a hotfix and the audit can be
  scheduled as a follow-up.  In that case, OPEN a TODO with the
  specific pattern to sweep, don't silently skip.

## Procedure

### 1. Articulate the bug pattern in one sentence

Before grepping, write down the pattern.  A good articulation has the
shape:

> "Call site passes X to helper, but X should be in context Y, and
> the helper's default behaviour assumes Y."

Examples from this codebase:

- "Call site passes `sp` (uniform sample on caster) to `BuildSeedChain`,
  but `BuildSeedChain`'s emitter-projection cap was designed for
  `sp = light position`."
- "Dedupe key uses `(firstPos)` only, but distinct caustic basins
  can share a first vertex if they differ in Fresnel-branch pattern
  on later vertices."
- "Per-path clamp on `smsGeometric` allows `N × maxGeometricTerm`
  total energy; sum-level clamp is what bounds per-pixel total."

If you can't write the pattern in one sentence, you don't understand
the bug yet.  Go read the code more carefully before sweeping.

### 2. Enumerate the candidate sibling sites

Common families to check:

- **Same-helper callers**.  `grep` for the helper's name.  Each
  caller passes its own arguments — is the same kind of value being
  passed at each?
- **RGB / NM twins**.  In rendering code, every RGB algorithm has a
  spectral twin (suffix `NM` typically).  Search for the function
  name with `NM` appended, or for the file's near-mirror.  Verify
  the spectral path has the same fix.
- **Algorithmic-mode branches**.  Search for `if (config.X)` /
  `else` pairs in the same function — both branches usually need
  matching treatment.
- **Fallback / photon / surface-sample paths**.  Functions that
  delegate to a main path, a fallback path, and an extension path
  (photon-aided, surface-sample, retry) tend to drift.  Audit each.
- **Static helpers that the bug-site already calls**.  Walk one hop
  deeper: are there helpers downstream that ALSO consume the bad
  value, and were they written assuming the bug's value semantics?

For each candidate, ask: **"Does the bug pattern apply here?"** —
not "does this look similar."  Specifically check the parameter
values, the data flow, the branching that gates the bug.

### 3. Confirm or refute each candidate

For each candidate site:

1. Read the code.  Not just the call line — the full function.
2. Trace the value back to its source.  Is the value the same kind
   that triggered the original bug?
3. If the answer is "yes, same pattern": flag it as a fix site.
4. If the answer is "no, the pattern doesn't apply here because
   <reason>": briefly note WHY in a comment if it's a non-obvious
   distinction.  Future readers will reach for the same audit.

### 4. Fix all confirmed sites in coordinated commits

Two options:

- **Single coordinated commit** if the fixes are all small and the
  pattern explanation is the same.  Commit message lists each site
  and explains why each was a sibling.
- **One commit per site (or grouping)** if the sites have meaningfully
  different contexts and the change at each is non-trivial.  Each
  commit cross-references the original via "Audit follow-up to
  commit XYZ".

Either way, make sure the commit history captures:
- The bug pattern in plain English.
- The list of sites that had the pattern.
- Any sites that LOOKED like siblings but weren't, with a
  one-sentence reason.

### 5. Audit one hop deeper: downstream consumers

After fixing the immediate sites, ask: who CONSUMES the data that
the buggy sites were producing?  If the bug's value flowed into
downstream code that interpreted it under the bug's assumption,
that downstream code might also need updating.

Example: if the original bug was "we were storing wrong-length
chains," then downstream code that decided based on chain length
(dedupe, contribution-evaluation, statistics) might have been built
around the wrong assumption.  Audit it.

This is the step most often skipped.  Skipping it leaves a "fixed
producer, broken consumer" mismatch that often surfaces later as
a different-looking symptom.

### 6. Verify with regression scenes / tests

If the codebase has a regression suite (RISE has 86+ unit tests
plus canonical scenes like `sms_k1_*`, `sms_k2_*`), run them after
each commit.  Add a new regression that specifically exercises the
sibling sites if the pattern is novel and not yet covered.

### 7. Stop rule

You're done when:

- A `grep` for the bug's pattern fingerprint returns no more
  candidate sites you haven't audited.
- Every candidate site is either fixed or has a one-sentence
  rejection reason.
- Tests pass at every commit.
- The commit history clearly captures the audit's scope so a future
  maintainer reaching for the same pattern can verify nothing was
  missed.

Don't stop just because the user-reported symptom is gone.  Stop
when the PATTERN is gone.

## Anti-Patterns

### "I fixed the line they pointed at"

The most common failure mode.  User reports a bug at line X; you
fix line X; you ship.  But the same bug pattern exists at lines
Y, Z, W in the same file or sibling files because they all came
from the same template / copy-paste / parallel implementation.
Each unfixed sibling is a future bug report.

### "RGB and NM are different algorithms so I only need to fix RGB"

False.  RGB and NM are usually ALMOST-identical implementations
of the same algorithm with per-wavelength evaluation differences.
A bug in the RGB algorithmic structure almost always exists in the
NM twin.  Always check.

### "Biased mode is the production path so I'll skip the unbiased fix"

Biased might be the production path now.  But the unbiased path
exists because someone needed it (convergence study, ground-truth
reference, future user).  Leaving it broken means the next person
who turns it on hits the bug and either fights the codebase or
silently produces wrong results.  Fix both.

### "The fallback path is rarely hit so the bug doesn't matter"

Two failure modes here.  First, "rarely" is empirical and might be
wrong — the path could be hit on scenes you haven't tested.  Second,
when the fallback IS hit, it's typically because the main path
ALREADY failed; users hitting the fallback are already stressed,
and a broken fallback gives them no recovery.  Fix it.

### "The pattern is too generic to grep for"

If your bug-pattern fingerprint is so generic that grep returns
hundreds of false positives, your articulation isn't specific
enough.  Refine: include the helper name, the parameter type,
the surrounding context.  A specific pattern means a focused
audit; a generic one means you're not looking carefully.

### "I'll add a TODO and come back to it"

The audit is most efficient WHEN THE BUG PATTERN IS FRESH IN YOUR
HEAD.  In a week you'll have to re-derive the pattern, re-read the
fixed site, and rebuild the model.  Just do it now.  An hour of
audit pays back the next time the pattern reappears in a similar
shape.

## Concrete Examples (from this repo's history)

### Example A — seed-overtrace fix sweep (2026-05)

**Initial bug**: `BuildSeedChain` over-traced past the emitter when
the emitter was inside a closed dielectric shell.  Fix: emitter-
projection cap.  2 sites, ~20 lines.  Commit `467ad76`.

**Audit pattern articulation**: "`BuildSeedChain`'s emitter-
projection cap fires at `|end - start| × 1.05`, but `end` was
sometimes a synthesized/derived point (caster sample, normal-target,
midpoint), not the actual emitter."

**Audit grep**: every callsite of `BuildSeedChain` and
`BuildSeedChainBranching` (post-2026-05: the latter is a thin
wrapper around the former, so the audit collapses to a single
function — historical counts below predate that consolidation).
10 callsites total at the time.  Categorised:

- Snell-mode toward-light traces (3 sites): correct as-is.
- Synthesized-100-unit fallback paths (4 sites: normalTarget × 2,
  midTarget × 2): bug present, needed `applyEmitterStop = false`.
  Commit `5a942df`.
- Pure-mirror caster supplemental seeds (1 site): bug present,
  same fix.  Commit `9bd7c5b`.
- Photon-aided seeds (4 sites: 2 inline + 2 via
  `ReversePhotonChainForSeed`): downstream consumer pattern — chains
  passed through Newton without the new `targetBounces` length filter
  the snell/uniform main paths were enforcing.  Same commit.
- Uniform-mode call sites (6 sites): the original bug location.
  Commit `7bc6e3f`.

Without the audit pass, the pure-mirror supplement and photon-aided
sites would have shipped broken on top of the seed-overtrace "fix."

### Example B — dedupe + sum-clamp asymmetry sweep (2026-05)

**Initial bug** (user-reported): uniform mode's dedupe key was
`(firstPos, chainLen)` — Fresnel-branched siblings collapsed.
Uniform mode's geometric-term clamp was per-path — `N × cap` total.

**Audit pattern**: "RGB snell mode has correct dedupe (three-field)
and correct clamp (sum-level); the other three paths (RGB uniform,
NM uniform, NM snell) had drifted away from this template."

**Audit grep**: `struct RootKey`, `acceptedRootPositions`,
`fmin(.*smsGeometric.*maxGeometricTerm)`, `clampedGeometric` —
every site that built a dedupe key or applied the clamp.

Found and fixed:
- RGB uniform (2 fixes, dedupe + clamp): commit `e853ab8` + `bb166f5`.
- NM uniform (same 2): same commits.
- NM snell (same 2): commit `3e1fe32`.

The user only reported 2 bugs.  The audit surfaced 2 more (NM snell
twins) that would otherwise have shipped broken indefinitely (the
spectral path is exercised less often, so the bug would lurk).

### Example C — geometric-vs-shading normal audit (2026-04)

**Initial finding**: shading-vs-geometric normal mismatch in
`ValidateChainPhysics` was over-rejecting valid chains.  Fix: use
geometric normal at that site.

**Audit pattern**: "any site doing wi/wo side tests against a
normal should use the GEOMETRIC normal, not the shading normal."

**Audit method**: spawned a sub-agent (general-purpose) with a
specific brief: walk every `vNormal` consumer in RISE, decide per
call site whether geometric or shading was correct, vet against
PBRT/Mitsuba conventions.  Output: `docs/NORMAL_USAGE_AUDIT.md`.

This is an important variant: when the audit scope is too broad
for an inline pass, **delegate to a subagent with a specific
question + reference list**.  The subagent's deliverable is the
audit report; you triage findings into fix commits.

## Stop Rule

The audit is done when:

- The bug pattern's fingerprint, articulated specifically, returns
  zero un-audited sites in the codebase.
- Every audited site is either fixed or has a written rejection
  reason (one-sentence comment in the commit message or in code).
- The fix has been verified with the regression suite at every
  affected path.
- The commit history captures the audit's scope clearly enough that
  a future maintainer encountering the pattern again can verify
  whether your audit covered their suspect site.

Do NOT stop at "the user-reported site is fixed."  The user
reported the symptom they could see; the audit is for the symptoms
they CAN'T see yet.
