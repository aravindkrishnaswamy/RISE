---
name: write-highly-effective-tests
description: |
  Structured procedure for writing or upgrading high-leverage tests.
  Use when: adding new test coverage, reviewing weak existing tests,
  converting smoke tests into real regression guards, or deciding
  whether coverage belongs in `tests/`, `scenes/Tests`, or `tools/`.
  Prioritizes strong oracles (analytic identities, naive/reference
  comparisons, and real bug topologies), separates exact contract
  checks from weaker sampled heuristics, and prevents benchmarks from
  masquerading as correctness tests.
---

# Write Highly Effective Tests

## When To Use

- Adding a new test for new behavior or a bug fix.
- Reviewing existing tests that feel like smoke tests.
- Strengthening range-only / determinism-only tests into real guards.
- Deciding whether a check belongs in `tests/`, `scenes/Tests`, or
  `tools/`.
- Designing shared test helpers because multiple files are repeating
  the same weak patterns.

## When NOT To Use

- Pure performance work where the goal is throughput, not correctness.
  Use [performance-work-with-baselines](performance-work-with-baselines.md)
  and keep the benchmark out of `tests/`.
- Broad image-quality validation that fundamentally needs rendered
  scene comparison rather than a deterministic oracle.  Put that in
  `scenes/Tests` instead.
- Trivial refactors where existing strong coverage already exercises
  the changed contract.

## Core Rule

The best test is not "something ran."  The best test has a strong
oracle tied to a failure mode that could actually happen.

Oracle quality, from strongest to weakest:

1. **Analytic / exact identity**
2. **Naive or reference implementation**
3. **Real regression topology from a past bug**
4. **Structural invariant**
5. **Sampled-difference heuristic**
6. **Smoke test**

Prefer climbing this list before adding more assertions.

## Procedure

### 1. Start from the contract, not from the implementation shape

Write down what must be true if the code is correct:

- exact endpoint or collapse behavior
- symmetry / asymmetry
- sign / monotonicity / ordering
- normalization / conservation
- wrapping / periodicity
- degenerate parameter behavior
- compatibility with a known reference

If you cannot state the contract in one or two sentences, you are not
ready to write the test yet.

### 2. Choose the strongest available oracle

Before writing the test, ask in this order:

- Is there an exact formula or identity?
- Can I compare against a simpler reference implementation?
- Is there a real historical bug shape to lock down?
- Is there a structural invariant that must always hold?
- Only if all of those fail: can I justify a sampled heuristic?

For RISE, common strong-oracle patterns include:

- parameter collapse: `blend=0`, `blend=1`, `warpAmplitude=0`,
  `warpLevels=0`, `persistence=0`
- exact periodicity or wrap equivalence
- symmetry / sign invariance in geometry or SDFs
- exact known values at special points
- equality to normalized Perlin or another simpler primitive
- comparison to naive geometry / math code

### 3. Encode edge cases first, not last

Most real regressions live at edges:

- zero / one / negative / maxed parameters
- exact boundaries and corners
- tied cases
- degenerate orientation vectors
- single-octave / no-op / empty-input behavior
- negative coordinates

If the test only exercises "normal-looking" inputs, it is usually
missing the reason it exists.

### 4. Separate exact contract checks from weaker heuristics

If a file needs both strong and weak checks, label and order them
accordingly:

- **Exact contract checks** first
- **Sampled-difference heuristics** second

Do not let "different settings produce different outputs" read like it
carries the same weight as an exact identity.

### 5. Treat sampled-difference checks as secondary support

Heuristic checks are acceptable only when clearly labeled and backed by
stronger checks elsewhere in the file.

Good heuristic use:

- confirming a parameter materially affects output after exact collapse
  cases are already covered
- confirming octave count or metric choice changes a distribution
  after exact endpoint identities are covered

Bad heuristic use:

- the only evidence that the algorithm is correct
- "range + determinism + values changed" as the whole test

### 6. Share helper logic once patterns repeat

When multiple test files repeat:

- closeness helpers
- unit-interval checks
- normalized-reference computations
- sampled-grid comparisons
- sampled difference counters

move that logic into shared helpers under `tests/`.

Shared helpers are not just cleanup; they raise test quality by making
 stronger patterns cheap to reuse.

### 7. Put the coverage in the right place

Use the repo’s three buckets deliberately:

- `tests/`: deterministic, assertion-style correctness checks
- `scenes/Tests`: rendered-image or statistical validation
- `tools/`: diagnostics, utilities, and benchmarks

If a binary mainly prints timings and asks the human to compare runs,
it is not a correctness test and does not belong in `tests/`.

### 8. Make the failure message explain the broken contract

A good failure message tells you what invariant was violated:

- actual and expected values
- the parameterization being exercised
- the meaningful special point or condition

Avoid opaque failures like:

- "values differ"
- "range too small"
- "non-deterministic"

without the coordinates or expected relationship.

### 9. If you must lock in a known bug, say so explicitly

Sometimes a regression test intentionally preserves currently wrong
behavior because changing semantics is deferred.  If you do this:

- comment that the behavior is intentionally frozen
- say what bug or compatibility constraint it represents
- make it easy for the next person to find and revisit

Silent bug ossification is worse than no test.

### 10. Stop when the test would catch a realistic break

Ask the final question:

"If I broke the code in one of the plausible ways this subsystem tends
to break, would this test fail for the right reason?"

If the honest answer is no, the test is not done.

## Anti-Patterns

### Range + determinism + "values changed"

This is the most common weak test shape in procedural code.  It proves
almost nothing about algorithmic correctness.

### Testing that the implementation repeats itself

Calling the same function twice and asserting equality only proves
determinism, not correctness.  Keep it if determinism matters, but do
not mistake it for a strong oracle.

### No exact degenerate cases

If a parameter has a natural no-op or collapse mode and the test does
not check it, the best oracle is being left on the table.

### Benchmarks in the correctness suite

A benchmark that prints timings is not a unit test.  Put it in
`tools/` or a dedicated benchmark harness.

### One-off helper duplication

Five local `IsClose` functions across five files is a smell.  The
duplication hides patterns and makes stronger tests expensive to add.

### Image-only checking for deterministically testable behavior

If a property can be checked analytically in `tests/`, do not force it
through a rendered-scene diff.

### NaN/Inf as a not-found sentinel (FALSE-GREEN under -ffast-math)

RISE builds with `-ffast-math` (`-ffinite-math-only`).  A helper that
returns `std::nan("")` for "not found" and is then compared
(`abs(x - K) < eps`) is **folded to constant-true** by the compiler — the
assertion silently passes even when the lookup failed.  This shipped as a
false-green THREE times in the snapshot/transaction work.  Use a **finite
poison** (e.g. `return -1.0e30;`, which fails an `abs(x-K)<eps` "equals"
check loudly) or an explicit `Check( ptr != nullptr )` before reading.
`tests/SourceHygieneTest.cpp` now FAILS the suite on any
`return <NaN/Inf>` sentinel — do not add a new one (or justify it inline
with `// HYGIENE-OK: <reason>`).  Note a finite poison still does NOT fail
a `> eps` "differs" check — guard those with an existence check.

### Asserting GREEN without proving RED (wrong-observable / tautology)

Writing a bug-fix test AFTER the fix and running it green proves nothing:
it may be tautological, or read the WRONG observable (a real example this
session: a camera test checked `GetTargetOrientation().x` when the orbit
writes `.y`, so its "reverted" assertions passed trivially).  **RED-prove
every bug-fix test:** see it FAIL without the fix.  `tools/red_prove.sh
<TestName> <marker> <src-file>...` automates it (stash the fix → build →
assert FAIL → restore → assert PASS).  If it can't be made to fail without
the fix, the test isn't pinning the bug.

## Concrete Examples From This Repo

- **Domain warp**: `warpLevels=0` and `warpAmplitude=0` should collapse
  exactly to normalized Perlin.  That is a far stronger test than
  merely checking that changing warp amplitude changes samples.
- **Worley noise**: `jitter=0` gives analytically known Euclidean F1,
  F2, and `F2-F1` values at cell centers and corners.
- **Wavelet / reaction-diffusion**: periodic wrapping is a contract, so
  compare wrapped coordinates on a sampled grid instead of only checking
  negative coordinates stay in range.
- **SDF primitives**: sign symmetry, axis symmetry, and primitive
  coverage are stronger than generic "returned a number" checks.
- **Mailboxing performance**: timing output with a human-read threshold
  belongs in `tools/`, not `tests/`.

## Stop Rule

This skill's work is done when:

- the test contains at least one strong oracle tied to the contract,
- edge cases are covered,
- any remaining heuristic checks are clearly secondary,
- the coverage lives in the right place (`tests/`, `scenes/Tests`, or
  `tools/`),
- the failure message would make the regression obvious,
- and (for a bug fix) it is **RED-proven** — seen to fail without the fix
  via `tools/red_prove.sh` — with no `-ffast-math`-foldable sentinel
  (`tests/SourceHygieneTest.cpp` enforces the latter).
