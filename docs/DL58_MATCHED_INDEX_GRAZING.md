# DL-58: matched-index extreme-grazing classification

CLOSED 2026-09-12 — repair `1b1909c0`, fibre compatibility refinement `ed8d9c94`.
Outstanding integration requirements: finish independent final review, merge,
verify the merged tree in an isolated worktree, and export the standalone
completion report with the retained evidence.

DL-58 covers a false total-internal-reflection family at extreme grazing
incidence. At equal indices, transmission remains physically valid at every
incidence cosine, including the grazing limit, and unpolarized dielectric
reflectance is exactly zero. The affected paths were `Optics::CalculateRefractedRay`,
`FibreLobeMath::FrDielectric`, the cosine Fresnel helpers in
`SubSurfaceScatteringBSDF` and `SubSurfaceScatteringSPF`, and
`ManifoldSolver::ComputeDielectricFresnel`.

The unfixed implementation formed a transmitted sine-squared value by
subtracting a cosine square from one. At equal indices, cancellation made a
positive grazing ray look like total internal reflection (the near-zero cutoff rejects a positive discriminant at `cosI = 1e-7`), while the cosine-only Fresnel helpers
could round `sinT2` to one at `cosI = 1e-9`. Those are classification failures
before the DL-56 Fresnel quotient is evaluated. The unfixed red logs record:

- direct controls: `Checks: 26  Failures: 9`;
- reachable consumers: `Checks: 36  Failures: 10`;
- throughput and sibling consumers: `Checks: 32  Failures: 11`.

The original throughput run's 11 failures contain eight valid SMS failures
and three invalid weave expectations. Weave clamps authored IOR 1 to 1.001,
so that fixture never reached matched media. The supplemental derivative run
reports `Checks: 41  Failures: 4`: one valid failure in the still-unfixed
incoming-direction derivative and the same three invalid weave expectations.
The early stale-header explanation in commit `1b1909c0` was disproved by the
clean rebuild. Commit `e74aa6f5` corrects the weave test to verify the existing
clamp. Those weave controls are not a regression red-proof. The direct fibre
helper's matched-index failures remain valid. SMS direction preservation was
baseline-green.

## Repair shape

Commit `1b1909c0` introduces shared cosine helpers in `Optics`. For finite,
positive indices, `CalculateRefractedCosine` first clamps the absolute
incidence cosine. Equal indices take an exact identity branch, returning
`cosT = cosI`; normal incidence is also explicit. For lower-to-higher-index rays (`Ni <
Nt`), it computes

`cosT = sqrt((1-r)(1+r) + (r cosI)^2)`, where `r = Ni/Nt`.

This keeps the small grazing cosine instead of reconstructing it by subtracting
near-equal squared quantities. For higher-to-lower-index rays, it computes the sine with
`sqrt((1-cosI)(1+cosI))`, divides by `Nt/Ni`, and accepts a tangent boundary
when the resulting transmitted sine is exactly one. No epsilon widening is
introduced; a result above the physical boundary remains TIR.

`CalculateDielectricReflectanceCosine` uses the same Snell classification,
returns zero for equal indices, and evaluates scaled s and p amplitude ratios
for unequal indices. The scaling factors are common to each ratio, which removes avoidable common-scale overflow and underflow. Both standalone SSS cosine helpers now call this shared implementation.
Fibre retains its existing unequal-index arithmetic and adds the missing exact
matched-index zero. A trial shared rewrite moved 82 of 204 hair golden values
by at most 4.48387e-15 relative; preserving the existing arithmetic keeps the
strict group-15a golden contract without recapturing or relaxing it.

For equal indices, the exact identities expected by the repair are:

- refraction succeeds and preserves the incoming direction, including at
  `cosI = 1e-7`;
- cosine Fresnel reflectance is `R = 0`, including at `cosI = 1e-9`;
- the SMS specular direction is the matched transmission direction;
- the derivative with respect to the surface normal is the zero matrix;
- the derivative with respect to incoming direction is negative identity.

Unequal-index controls must retain normal-incidence Fresnel, transmission on
the non-TIR side of the critical angle, TIR on the physical TIR side, signed
cosine eta inversion, and tangent classification at the exact boundary where
the API supports it.

## Sibling audit

Source inspection of the repair and its call sites covers RGB and NM SSS,
fibre and hair lobe Fresnel, SMS/manifold direction and derivative paths,
weave surface Fresnel, and shared RGB/NM consumers. Weave's `ResolveWeave`
clamps eta to at least 1.001; hair's `Resolve` and spectral `EvalFsum` accept
eta only above 1+1e-6 (otherwise retaining the default/reference index).
Consequently the recipe's real matched-index fibre consumer case is not
reachable through those material guards. Direct `FrDielectric` tests own that
identity; the weave consumer verifies its actual clamped unequal index.
Composite and fabric paths forward their base SPF results. CoatedLayer retains
its own equal-index identity guard; AR thin-film uses complex-stack optics and
is outside this helper family. The audit also retains DL-51 IOR-stack behavior,
DL-56 scaled Fresnel behavior, and existing IOR/consumer gates.

The source repair is in six production files: `Optics.cpp`, `Optics.h`,
`FibreLobeMath.h`, `SubSurfaceScatteringBSDF.cpp`,
`SubSurfaceScatteringSPF.cpp`, and `ManifoldSolver.cpp`. The test adjustment is
in `tests/OpticsTest.cpp`; the three new regression tests are
`tests/GrazingSnellFresnelTest.cpp`,
`tests/MatchedIndexGrazingConsumerTest.cpp`, and
`tests/GrazingFresnelThroughputTest.cpp`. `tests/README.md` records their
scope.

## Validation and file status

The branch completed 30 selected individual gates across 62 stages (clean,
Make build, 30 test links, 30 serial executions) at `ed8d9c940771bdb06ceb41dd44404bafa5b1a293`.
The three new suites report:

- `GrazingSnellFresnelTest`: `Checks: 38  Failures: 0`.
- `MatchedIndexGrazingConsumerTest`: `Checks: 60  Failures: 0`.
- `GrazingFresnelThroughputTest`: `Checks: 44  Failures: 0`.

Clean Xcode validation also passed at that source state with zero compiler
warnings; four environment notices (three local OIDN paths and one AppIntents
metadata notice) are retained in the evidence. No full-suite runner was used.
The remaining integration/report requirements above include retaining all
gate output verbatim, review rounds and isolated merged-master verification.
Documentation-only closure edits retain compiled source and executable test
inputs.

| File | Status |
|---|---|
| `docs/DEBT_LEDGER.md` | Modified in this slice. |
| `docs/DL51_SUBSURFACE_EXIT_IOR.md` | Modified in this slice. |
| `docs/DL54_BSSRDF_PROJECTION_NORMAL.md` | Modified in this slice. |
| `docs/DL55_RANDOM_WALK_FALLBACK_PROPOSALS.md` | Modified in this slice. |
| `docs/DL56_GRAZING_FRESNEL.md` | Modified in this slice. |
| `docs/DL58_MATCHED_INDEX_GRAZING.md` | Added in this slice. |
| `docs/README.md` | Modified in this slice. |
| `src/Library/Materials/FibreLobeMath.h` | Modified in this slice. |
| `src/Library/Materials/SubSurfaceScatteringBSDF.cpp` | Modified in this slice. |
| `src/Library/Materials/SubSurfaceScatteringSPF.cpp` | Modified in this slice. |
| `src/Library/Utilities/ManifoldSolver.cpp` | Modified in this slice. |
| `src/Library/Utilities/Optics.cpp` | Modified in this slice. |
| `src/Library/Utilities/Optics.h` | Modified in this slice. |
| `tests/GrazingFresnelThroughputTest.cpp` | Added in this slice. |
| `tests/GrazingSnellFresnelTest.cpp` | Added in this slice. |
| `tests/MatchedIndexGrazingConsumerTest.cpp` | Added in this slice. |
| `tests/OpticsTest.cpp` | Modified in this slice. |
| `tests/README.md` | Modified in this slice. |

No Library source files were added or removed; the five explicit build-project
source lists require no change. One independent residual, DL-59, was opened during review. Ledger at closure:
58 main rows, 45 open and 13 closed by cleanup.

## Remaining unequal-index derivative convention (DL-59)

OPEN-confirmed by source and scalar recomputation; executed red-proof pending.
`ManifoldSolver::ComputeSpecularDirectionDerivativeWrtNormal` still uses `eta`
directly for unequal indices, whereas `ComputeSpecularDirection` uses `1/eta`
on entry and flips the normal on exit. At `wi=n=(0,0,1)`, `eta=1.5`, the
raw tangential derivative `dwo.x/dn.x` is `0.5`; the direction formula gives
`1/1.5-1 = -0.33333333333333337`. Output normalization does not remove this
tangential discrepancy.

This convention defect is independent of the matched-index cancellation
repaired here. `BuildJacobianAngleDiff` consumes the derivative, but that
angle-difference family is test-only; production `Solve` uses the half-vector
Jacobian. Existing normal-derivative finite-difference tests are disabled.
DL-59 is S / physics-bias / latent (test-only analytical Jacobian). Its recipe
requires entering/exiting normal and oblique cases, unit-normal tangent
perturbations, the normalized-output derivative correction, and curved
angle-difference analytical/numerical Jacobian checks. It must also correct
the stale helper comment that says it is entirely unused. No production
render effect has been demonstrated.
