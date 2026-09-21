# DL273–DL275 — quartic tangency and representable exponent ranges

Closed 2026-09-21 alongside [DL226](DL226_QUARTIC_FACTORIZATION.md).
Independent review of the first DL226 candidate found three pre-existing
boundary defects. This revision reproduced each with committed public-API
and actual torus tests before changing the formulation. Production source
is frozen at `4b17e660cabc194b634407cb616954b4bb26c10a`; the final test
addition is `04b203ca`, with a stricter near-contact value band at
`537da86f`. All inputs discussed below have finite stored
coefficients and finite expected roots. Nonfinite input coefficients are
outside these oracles, and leading-zero dispatch remains the existing
classical cubic implementation.

## DL273: exact tangencies need accurate factor residuals

For torus major radius R, minor radius r, ray origin `(-3R,r,0)` and
direction `(1,0,0)`, the quartic is `((t−3R)²−R²)²`. Its contact roots
are exactly `2R,4R`. The committed red solver misses at R=.125,1,8;
for R=1, `[1,-12,52,-96,64]` returns no roots. Tests separately assert
the contact range, just-inside hits, and just-outside misses.

Ordinary multiply/add evaluation of Newton's coefficient residual can
round to zero while the factors' discriminants remain slightly negative.
The correction evaluates product residuals with FMA and summed residuals
with error-free summation. A scoped `float_control(precise,on,push)` / `pop` around TwoSum preserves
the individual operations for Clang/MSVC without stack barriers. Other
compilers retain the equivalent explicit volatile roundings. Product
rounding barriers remain where FMA needs the separately rounded product.
Reassociation must not algebraically erase the low error term; the previous
compiler mode is restored immediately after the helper. Newton can then
reach the exact coprime factors `(t−2R)²` and `(t−4R)²`. Quadratic
extraction still compares the discriminant to zero without a tolerance.
Positive perturbations of the constant in `(t²−6t+8)²` must have no real
roots; negative perturbations have four independently calculated roots.
These tests prevent a tangency correction from inventing nearby hits.

## DL274 and DL275: normalization must precede division

DL274's finite `[2^-1022,0,-4,0,4]` produced four NaNs after monic division
overflowed. Set `a=2^-1022` and `y=x²`: `a*y²−4*y+4=0` has two positive
roots. Its small root is `2/(1+sqrt(1−a))`; the corresponding x roots
round near ±1 and ±2^512, all finite. Non-even mixed-scale controls also
retain their golden-ratio roots, not just their large roots or count.

DL275's `(x²−r²)(x²−4r²)` at r=2^-200 or 2^-250 has representable
coefficients and roots, but the old resolvent's products underflowed.
Tests compare every returned root against ±r,±2r with relative tolerances,
and include non-even `(x−r)(x−2r)(x−3r)(x−4r)` siblings.

Write each coefficient as `mi*2^ei` with `frexp`. For `x=2^k*y`, choose
`k=max_i ceil((ei−e0)/i)` over nonzero coefficients. The normalized monic
coefficient is `(mi/m0)*2^(ei−e0−i*k)`, formed without first evaluating
an overflowing `ai/a0`. The significand quotient is bounded and the
normalized coefficients have magnitude at most about two. Scaling both
directions protects tiny as well as large root scales.

One double scale is insufficient when roots are widely separated. The
first scaling correction passed 740 checks but failed a wider independent
witness `[1,-H,6H,-11H,6H]`, H=2^600: lower coefficients underflowed and
it returned `2,0,H,2`. The stored polynomial differs from the expansion
of `(x−H)(x−1)(x−2)(x−3)` only by relative O(1/H) terms; its roots remain
near `1,2,3,H`, far inside the fixed 1e-10 relative test band. This input
rounding is distinct from the implementation discarding entire normalized
coefficients. That incomplete correction and its failing logs are retained.

The final implementation instantiates the **same OQS core** with a
binary64 significand and separate signed 64-bit exponent when the
normalized coefficient range is too wide. Dispatch occurs conservatively
below `DBL_MIN_EXP/12`, before the degree-twelve resolvent product range
becomes subnormal. This controls arithmetic representation; it does not
classify roots or change acceptance thresholds. Original nonzero
coefficients remain available for Vieta recovery even when a contribution
rounds away in an individual sum. Precision remains 53 significant bits.

Finite nonzero values have `.5<=abs(m)<1`; zero has exponent zero.
Products and quotients operate on bounded significands. Addition aligns
exponents; terms beyond alignment range cannot affect 53-bit rounding.
FMA aligns the addend to the product's binade and uses one hardware FMA,
with dominance handled before an overflowing shift. Square/cube roots
split the exponent into quotient and remainder, including negative
exponents. Trigonometric conversion occurs only for bounded resolvent
angle arguments. Final root conversion explicitly handles normal,
subnormal and out-of-range results, checking the exponent before narrowing
it to the platform `scalbn` argument. Tests retain roots at 2^1023,
2^-1023 and the minimum subnormal 2^-1074, with their ordinary siblings.
The relative band for the minimum subnormal rounds to zero, requiring
that exact output representation.

This extends range rather than providing certified root isolation,
extra significand precision, or a guarantee for every ill-conditioned
multiple root. Signed zero is numerically zero in the OQS comparisons;
there is no distinct signed-zero root contract. The exponent headroom is
far larger than the initial finite binary64 coefficient range and the
bounded OQS iteration intermediates exercised here.

## Retained red/green provenance

All logs are under `/private/tmp/rise-debt-codex-20260919/dl226/`.
Every table row was committed and rebuilt through the library and exact
`PolynomialTest` target before execution; successful builds had zero warnings.

| State | Explicit checks / failures | Meaning |
|---|---:|---|
| `571883a7` | 728 / 95 | Independent tangency, normalization and tiny-scale red, including non-even siblings |
| `e241579a` | 740 / 0 | Compensated residuals plus single-scale correction; subsequently disproved by wider root separation |
| `a71c77b5` | 906 / 25 | Wider committed red: 17 genuine failures plus 8 invalid-fixture failures |
| `080c2adc` | 906 / 0 | Exponent-preserving core and corrected finite fixture |
| `52ff8673` | 1024 / 0 | Adds explicit finite-input fixture assertions |
| `04b203ca`, `537da86f` | 1057 / 0 | Adds output boundaries, then tightens the near-contact value band to separate every root |
| `4b17e660` | 1057 / 0 | Same formulation with scoped precise TwoSum; renewed clean gates |

The invalid fixture used r=2^255, making its `24*r^4` constant infinite
during construction. It was corrected to 2^254; the original 906/25 log
remains separate. Subsequent fixture entry asserts all stored coefficients
are finite. This setup correction did not change a solver acceptance band.
Check totals also vary with returned-root diagnostics; restoring the three
lost tangent cases adds twelve such checks. The earlier DL226 528/90,
466/3 and 521/0 results remain historical evidence for that selection fix.

`revision1-{red,first,spread-red,wide}-status.json` index these runs.
`revision2-final-gates-status.json` records the final fresh clean library and all
11 exact named build/run pairs: 24 stages, all rc0 and zero warnings.
The suites are PolynomialTest, BezierClippingUnitsTest,
GeometryUVRoundtripTest, GeometrySurfaceDerivativesTest,
GeometryShadingTangentTest, TextureFootprintTest, ProximitySignalTest,
CsgProbeFloorTest, BoxGeometryTest, TessellatedShapeDerivativesTest and
SourceHygieneTest. The later boundary-only test edit has its exact target
relink/run in `revision1-boundary-final-status.json`, both rc0, zero warnings. The final near-contact assertion tightening is
separately relinked in `revision1-contact-final-status.json`, also rc0 and
zero warnings; its 1e-10 absolute band is well below the smallest root
separation (about 1.69e-7).

Supervisor-run clean Deployment and Opto on frozen production also passed,
each rc0, two documented OIDN-path/AppIntents notices and zero actionable
warnings. Their supervisor-produced logs and manifest are in
`/private/tmp/rise-debt-codex-20260919/dl226_xcode_revision2/`.
The earlier `revision1-final-gates-status.json` and platform revision1 logs
remain historical evidence for the slower volatile implementation. No new
source files, build entries, APIs or renderer changes were introduced.
The prior caller classification still holds: live torus consumers,
Bezier cubic dispatch, and source-only audit of the abstract QuarticFunction
wrapper. No rendering was needed for these direct geometric proofs.

## Independent arithmetic and cost evidence

An external translation unit reads the actual production implementation,
compiled with O3/LTO/fast-math and `-fno-finite-math-only`. Seed 273274275
uses full 53-bit significands and exponents from −5000 to +5000, including
same-binade sums and cancelling products. An independent Python Fraction
oracle checked 2,000 additions, 1,000 products, 1,000 divisions and 2,013
FMAs to at most .5 output ULP. A 90-digit Decimal oracle checked thirteen
square roots and thirteen cube roots across positive/negative exponents;
maximum observed relative error was below 6.85e-17. All **6,039 checks
passed**. Another **500 checks passed** for known-root double/extended
core parity. These finite tests do not constitute exhaustive arithmetic
certification. Raw operands, scripts and results are
`revision2-arithmetic-*` and `check-revision2-arithmetic.py`. The earlier
`revision1-arithmetic-v2-*` records verify the volatile fallback formula;
MSVC/GCC runtime builds were not performed in this slice.

The first DL226 candidate's timing table does not describe this implementation.
The initial correct range/refinement candidate had a material solver cost:
69 ns → 146 ns for the torus input (n=5). A separate quiet n=3 ablation
measured full/direct-monic/raw-residual/precise-residual torus CPU costs of
147.061±1.934 / 137.779±1.511 / 80.155±5.098 / 103.793±3.313 ns. This
identified the volatile TwoSum barriers as the dominant avoidable cost;
normalization accounted for only about 6% in that experiment. The selected
optimization retains both normalization and compensated residuals.
Diagnostic variants are not correctness candidates: direct-monic and
raw-residual builds had 14 and 3 unused-function warnings respectively,
retained in `ablation-*-build.log`; full/precise builds had zero warnings.
All root counts and twelve process results remain in `ablation-raw.json`.

Final quiet microbenchmarks link separate saved original-red and final
production objects. Five alternating AB/BA samples per family retain every
CPU/wall timing, root count and checksum. Eight ordinary families use
500,000 calls/sample; the two exceptional exponent-range families use
20,000. Inner-loop timing includes dispatch and root checksum, excluding
process startup (separately recorded). No sample was excluded or retried.


| Input | Old CPU ns/call ± sample SD | Final CPU ns/call ± sample SD | Final/old |
|---|---:|---:|---:|
| four_real | 68.325 ± 16.475 | 91.817 ± 18.132 | 1.3438 |
| positive_even | 30.555 ± 2.479 | 27.481 ± 1.196 | 0.8994 |
| repeated_real | 44.873 ± 1.545 | 42.828 ± 0.514 | 0.9544 |
| near_multiple_positive | 50.841 ± 1.293 | 75.372 ± 2.751 | 1.4825 |
| torus_four_crossings | 69.849 ± 1.265 | 102.449 ± 0.433 | 1.4667 |
| scaled_real | 59.891 ± 0.359 | 83.373 ± 0.342 | 1.3921 |
| exact_tangent | 77.919 ± 0.582 | 96.530 ± 1.653 | 1.2389 |
| tiny_non_even | 51.454 ± 0.097 | 101.951 ± 0.567 | 1.9814 |
| normalization_overflow | 192.680 ± 4.390 | 624.350 ± 16.512 | 3.2403 |
| wide_root_spread | 195.910 ± 2.809 | 948.430 ± 18.089 | 4.8412 |

Every row has n=5. The largest tested final mean is 948.430 ns for the
wide-root input, 4.84× its incorrect original result. Ordinary torus cost
is 1.47×; the near-multiple positive input is 1.48×. These are solver costs,
not a global worst-case bound. Positive, tangent, tiny and wide-range
cases change incorrect roots or refinement work, so their speed ratios
compare different necessary work. The noisy first-family sample is
retained. `revision2-cost-{raw,summary}.json` and the build manifest contain
all 100 raw rows and exact release-flag linkage.

A second coordinated cost experiment rendered two shipped scenes at
192×192/64 spp, box filter, OIDN off, linear Rec709 EXR32. The 30-link
`mlt_torus_chain_atrium` scene used a PT rasterizer substitution; the
non-torus control was `cornellbox_pointlight_pt`. Other physical scene
content was retained. The CLI binaries use the exact make full-object
link command, substituting only the saved original `Polynomial.o` for
the old version. Separate external archives also had their unique
Polynomial member byte-verified and independent public witnesses linked:
old 7 cases/4 failures, final 7/0. ABI and all other production objects
were identical; no production files were swapped for this comparison.

The first control load failed because two relative rawmesh assets were
missing from its external working directory. The harness rejected that
PARTIAL SCENE even though it wrote an EXR. That scene/log/image and the
rejection remain preserved. Only those two paths were made absolute,
with asset hashes and a path-only diff; the valid first torus pair was
retained and the remaining ten valid renders completed. This was one
setup correction, not an outlier exclusion or a noise retry.

| Scene / state | Process seconds ± sample SD | Frame seconds ± sample SD | Image mean Y ± sample SD |
|---|---:|---:|---:|
| torus / old | 2.176708 ± 0.199295 | 2.034333 ± 0.012503 | 2.918912647 ± 0.008379993 |
| torus / new | 2.248624 ± 0.215295 | 2.104333 ± 0.053985 | 2.915092570 ± 0.008455661 |
| control / old | 1.073200 ± 0.008820 | 1.055000 ± 0.009539 | 0.674181406 ± 0.000192636 |
| control / new | 1.084962 ± 0.011009 | 1.066333 ± 0.010970 | 0.674484963 ± 0.000222849 |

Each row has n=3. Measured frame-time changes were +3.44% for the torus
scene and +1.07% for the non-torus control. Small sample counts and observed
variation limit precise slowdown claims. Process times retain startup,
including the slower first launches; frame times use the renderer's
millisecond-resolution Rasterize timer. All twelve valid outputs were fresh,
finite 192×192 EXRs. CLI rc1 is the explicit `ParseQuit::exit(1)` convention;
it was accepted only alongside completion, no partial-load diagnostic,
and a newly written validated EXR. The failed setup also returned rc1 and
was rejected, demonstrating that status alone was not normalized to success.

This is a bounded cost experiment, not an estimator-equality test: the
old solver has incorrect geometry, so path workloads may differ. Image
means and their repeat SD are retained without claiming pixel equality,
convergence, or that repeat SD bounds integration error. Full manifests,
commands, asset hashes, scenes, logs and images are in `whole-cost/`,
particularly `binary-manifest.json`, `scene-manifest.json`,
`asset-path-only.patch`, `resolved-assets.json`, `whole-raw.json`,
`whole-summary.json` and `whole-initial-with-failed-setup.json`.
