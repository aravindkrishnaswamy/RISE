# DL-58: matched-index extreme-grazing classification

STATUS: PENDING final execution, review, merge, and ledger closure.

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
for unequal indices. The scaling factors are common to each ratio, which removes avoidable common-scale overflow and underflow. Fibre and
both standalone SSS cosine helpers now call this shared implementation.

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

## Validation status

The three original red runs above are from the unfixed library; the supplemental
derivative run used the documented partial fix. The fixed-run check totals,
clean rebuild status, consumer activity measurements, derivative confirmation,
and any merged-master validation remain PENDING. No compiled gate is claimed
here, and this document does not claim final review or merge approval.

| File | Status |
|---|---|
| `src/Library/Utilities/Optics.cpp` | Modified by `1b1909c0`. |
| `src/Library/Utilities/Optics.h` | Modified by `1b1909c0`. |
| `src/Library/Materials/FibreLobeMath.h` | Modified by `1b1909c0`. |
| `src/Library/Materials/SubSurfaceScatteringBSDF.cpp` | Modified by `1b1909c0`. |
| `src/Library/Materials/SubSurfaceScatteringSPF.cpp` | Modified by `1b1909c0`. |
| `src/Library/Utilities/ManifoldSolver.cpp` | Modified by `1b1909c0`. |
| `tests/OpticsTest.cpp` | Adjusted by `1b1909c0`. |
| `tests/GrazingSnellFresnelTest.cpp` | Added before execution. |
| `tests/MatchedIndexGrazingConsumerTest.cpp` | Added before execution. |
| `tests/GrazingFresnelThroughputTest.cpp` | Added before execution. |
| `tests/README.md` | Updated test-scope entry. |
| `docs/DL58_MATCHED_INDEX_GRAZING.md` | This pending closure document. |
| `docs/DEBT_LEDGER.md` | Future DL-58 closure/count update. |
| `docs/DL56_GRAZING_FRESNEL.md` | Future DL-58 source-residual update. |

