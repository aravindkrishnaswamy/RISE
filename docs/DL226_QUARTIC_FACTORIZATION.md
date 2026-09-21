# DL226 — compare real and complex quartic candidates consistently

Closed 2026-09-21. The final slice also fixes review-discovered
[DL273–DL275 tangency and exponent-range defects](DL273_DL274_DL275_QUARTIC_RANGE.md),
with final **1057/0** checks and renewed gates/cost evidence. The 521/0
candidate and timing table below are retained historical DL226 evidence.

The public `Polynomial::SolveQuartic` no longer replaces
an accurate conjugate-complex factorization with an incompatible real
alternative. The independent witness `[1,0,2,0,1]`, in the API's descending
coefficient order, is `(x²+1)² > 0`; the old solver returned four roots
`±1`, each with residual 4 and normalized residual 1. Actual torus central-hole
rays also falsely hit. The correction changes factor selection, with no
post-root residual filter, root acceptance epsilon, or relaxed test band.

## Factorization and exact coalescence

For monic coefficients `(1,A,B,C,D)`, write `Q=x²+l1*x+l3` and `L=x+l2`.
The selected LDLT representation is `Q²+d2*L²`. Its reconstructed coefficients
are `2*l1`, `l1²+2*l3+d2`, `2*l1*l3+2*d2*l2`, and `l3²+d2*l2²`.
For `d2<0` this is a product of real quadratics; for `d2>0`, conjugate
complex quadratics. Both have a meaningful coefficient error. The existing
`oqs_calc_err_ldlt` supplies the B/C/D reconstruction error for the latter
without complex arithmetic; `2*l1=A` already holds by construction. Like
OQS's other candidate metrics, nonzero coefficients use relative error and
zero coefficients use absolute error.

The identical-alpha alternative uses `Q±sqrt(-d3)`, where `d3=D-l3²`.
It remains evaluated whenever `d3<=0`, including away from `d2=0`, preserving
the alternate-factor recovery used by difficult torus rays. It must beat
the actual primary error. Previously, a complex primary was assigned a
maximum-error sentinel and the alternative won unconditionally. For the
positive witness the primary error is 0, the alternative error is 2:
the alternative changes `+2*x²` into `-2*x²`. Singular repeated-factor
Newton refinement cannot repair that substitution. Only the `d2==0`
state, where this implementation has not constructed a primary candidate,
requires unconditional selection of the alternative.

An independently measured first correction exposed a second selection
ambiguity for `(x−1)^4`. With `d2=-2.2204460492503131e-16`, the primary
quadratics have alpha `-2±1.4901161193847656e-8` and beta 1. The alternative
is exactly `(x²-2*x+1)²`, with `d3=0`; both reconstruction errors round to
0. Refinement happened to coalesce the old compiled factors but left the
first corrected candidate with roots `1.000122077763308` and
`0.99987793713785322`. Choosing the exactly coalesced alternative when
`d3==0 && err1==err0` avoids depending on that singular refinement. This
is an exact equality rule, with no error band; other ties preserve the
previous ordering. Strictly positive squares and `(x−1)^4+delta` controls
separate genuine repeated-real cases from positive near-multiple cases.

A sum of squares can share a real zero, so complex factors alone are not
a general proof of no real root. For the ideal dominant-resolvent choice,
a repeated real root can be translated to zero: `x²(x²+a*x+b)` has
resolvent roots `2*b/3, -b/3, -b/3`; the dominant choice gives
`d2=-a²/4<=0`. The resolvent invariants and d2 are translation invariant.
The repeated-real/complex-pair controls cover this distinction, while the
alternate and coalescence comparisons address the measured numerical case.
This remains a floating-point solver, not a certified root-isolation API.

The primary reference is [Orellana and De Michele, Algorithm 1010](https://doi.org/10.1145/3386241).
The [authors' current source](https://github.com/cridemichel/quartic_C/blob/main/Src/quartic_solver.c)
also compares the complex primary's coefficient error. It was consulted
for comparison; the repair reuses RISE's existing reconstruction helper.
The old unsupported “MIT-like” attribution was corrected to refer readers
to the upstream notice.

## Independent committed proof and callers

The external discovery came from a Ward experiment; this slice derived
its own positive-polynomial/factored-root oracles before adding assertions.
Committed red `fec5b0a328750cd844995675ac4df7ed20d832ad` produced **528
explicit checks, 90 failures**, rc1: 75 failures from 15 positive quartics,
9 center-origin axial torus false hits, and 6 transformed primary/shadow
false hits. All real-root and grazing-hit controls passed. Earlier setup
attempts failed because `QuarticFunction` is abstract and subsequently
because its implementation is not in the make library; both failed build
logs are retained, separate from measured solver results.

First correction `080ecc55ec170de0a9c497125971b743a52e49aa` produced
**466 checks, 3 failures**, rc1, entirely on the exact fourfold root above.
The count changed because corrected positive cases emitted 60 fewer
per-returned-root checks and the fourfold case emitted 2 fewer:
`528-60-2=466`. Fixed semantic count/coverage and torus checks stayed in
place. First review candidate `3afbb16d845c2bf97c5251c8f758750118d13993`
produced **521 checks, 0 failures**, rc0: restored fourfold roots give 468
original checks, plus 53 added repeated/positive-near-multiple checks.
Existing assert-based polynomial tests also completed. No failed result
was discarded or obtained by retrying until green.

The explicit tests remain active under `NDEBUG`. They include positive
even and translated positive squares; common coefficient factors
`±2^k`, k=-600,-200,0,200,600; real root variable scales `2^k`,
k=-100,-20,0,20,100; repeated/zero roots; repeated-real plus complex pairs;
near-real complex pairs; and leading-zero cubic/quadratic/linear/constant
fallbacks. Positive `(x−1)^4+delta` uses delta=2^-4,-20,-40,-48. Roots
are compared to independent factors; residuals are diagnostic, not a
production or test-side root filter.

- `RayTorusIntersection` is the live nondegenerate quartic consumer. It
  feeds `TorusGeometry`, `Object` primary/shadow paths, CSG and proximity.
  Direct tests use major radii .125/1/8, minor ratios .125/.25/.5,
  central-hole rays and four-crossing rays with independent circle roots;
  translated/rotated objects test primary and shadow misses plus grazing
  hits. Existing surface-origin cubic deflation and ray floors are unchanged.
- `RayBezierPatchIntersection` sets the leading quartic coefficient to
  zero: its resultant actually dispatches to the classical cubic solver.
  Its bounds, F1/F2 polishing/residual checks and grid fallback are unchanged.
  The stale “OQS-stabilized cubic” comment was corrected.
- `QuarticFunction` forwards to this public API. It is abstract, listed in
  Xcode/VS2022, absent from make/Android, and has no in-tree instantiation
  found. This is a source forwarding audit, not a linked runtime test or
  a claim that the wrapper is universally dead.

## Gates and evidence

All evidence is external under `/private/tmp/rise-debt-codex-20260919/dl226/`.
The library and exact `PolynomialTest` target were rebuilt at each measured
committed state. `red3-build-status.json`, `red-test-status.json`,
`green-build-status.json`, `green-test-status.json`, `tie-build-status.json`
and `tie-test-status.json` record commands, return codes and raw logs.
All successful library/test builds had zero warnings. Initial setup
failures remain in `red-build-status.json` and `red2-build-status.json`.

At that first review candidate, `final-gates-status.json` records **24 successful
stages**, zero warnings: clean library plus exact build/run pairs for
PolynomialTest, BezierClippingUnitsTest, GeometryUVRoundtripTest,
GeometrySurfaceDerivativesTest, GeometryShadingTangentTest,
TextureFootprintTest, ProximitySignalTest, CsgProbeFloorTest,
BoxGeometryTest, TessellatedShapeDerivativesTest and SourceHygieneTest.
UV coverage reported 85,156 constructed hits, 0 lost, 14,844 near-tangent/
degenerate skips, 331 oracle cross-checks with 0 disagreements, and 98,779
parallel-plane rays with 0 phantoms. Existing intentional skip/control
semantics were not changed. No rendering was needed for the direct false-hit
proof; this is not a rendered scene coverage claim.

Supervisor-run clean Xcode Deployment and Opto also passed on the same
production source, each rc0, two documented notices (OIDN path and
AppIntents), zero actionable warnings. The worker inspected the supervisor's
results manifest; these are supervisor-produced measurements:
`/private/tmp/rise-debt-codex-20260919/dl226_xcode_candidate/results.json`,
with `Deployment.log` and `Opto.log` adjacent. Those platform measurements apply only to the first candidate; the linked
DL273–DL275 note records renewed gates for the changed production.

Additional reconstruction evidence: `initial-oracle.md`,
`factor-comparison-derivation.md`, `factor-{red,candidate}-run.log`,
`factor-diagnostic-status.json`, committed patches, saved A/B object files
and binaries. Instrumented diagnostic copies are external and explicitly
separate from production executables; they show each candidate's error,
coefficients, pre/post-refinement values and returned roots.

## Historical first-candidate quiet solver cost

A coordinated idle window measured five alternating AB/BA process pairs,
500,000 calls per input per sample. Separate binaries link the saved red
and final committed polynomial objects, using O3/LTO/fast-math with
`-fno-finite-math-only`. A volatile function-pointer dispatch prevents
hoisting the calls. CPU and steady-clock wall timings cover the solver
loop and checksum accumulation; process startup is excluded from these
loop values and recorded separately. All samples, including the variable
first process, remain in `cost-raw.json`; no outlier removal or reruns.
The external driver and exact build commands are `solver-cost.cpp` and
`cost-build-status.json`; `cost-summary.json` gives both clocks.

| Fixed input | Red CPU ns/call, mean ± sample SD | Final CPU ns/call, mean ± sample SD | Final/red |
|---|---:|---:|---:|
| `(x²−1)(x²−4)` | 68.506 ± 18.514 | 65.787 ± 15.128 | .9603 |
| `(x²+1)²` | 29.770 ± 3.155 | 25.311 ± 2.284 | .8502 |
| `(x−1)^4` | 43.836 ± 2.412 | 27.743 ± 1.433 | .6329 |
| `(x−1)^4+2^-40` | 50.034 ± 1.211 | 51.874 ± .616 | 1.0368 |
| Torus R=1,r=.25, O=(-3,0,0), D=(1,0,0) | 68.492 ± .291 | 69.764 ± .235 | 1.0186 |
| First input scaled by 2^-600 | 58.854 ± .258 | 58.952 ± .128 | 1.0017 |

Every row has n=5. The largest measured relative increase is 3.68% for
the positive near-multiple input; the largest final absolute mean is
69.764 ns/call for the torus input. These are the worst of these six
inputs, not a global worst-case bound or full-render cost estimate.
The positive-even input changes four false roots into zero roots;
repeated-real selection preserves four roots but changes refinement work.
Their speed changes therefore compare different necessary work. Other
rows retain the same root counts and checksums; the highly variable first
row does not support a precise speedup claim.
