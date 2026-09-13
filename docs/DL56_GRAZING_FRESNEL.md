# DL-56: grazing dielectric Fresnel quotient

CLOSED 2026-09-12 — `df7e3dad`, DielectricGrazingFresnelTest:
`Checks: 338  Failures: 0` (unfixed: `Checks: 128  Failures: 20`).

## Defect and repair

`Optics::CalculateDielectricReflectance` treated a small numerator and a
small denominator in its combined Fresnel quotient as total reflection. The
static recomputation retained in
`.claude/debt-DL56/logs/static-recomputation.json` records the matched-index
grazing case Ni=Nt=1.5 and cosI=cosT=.01: numerator `0`, denominator
`8.100000000000002e-7`, old reflectance `1`, and physical reflectance `0`.

`df7e3dad` replaces that absolute-cutoff heuristic with scaled physical s/p
amplitude ratios. Common index and cosine factors are scaled before division,
so the formulation preserves the matched-index identity while avoiding the
underflow/overflow of the combined quotient. There is no epsilon fallback.
The repair does not change Snell classification, IOR-stack transitions, or
radiance/importance eta conventions.

## Regression evidence

The test commits `a0614a6d` and `b4e176b6` were present before the first
execution against the unchanged library. `a0614a6d` carries the required
Terra co-author trailer. The unfixed run in `.claude/debt-DL56/logs/red.log`
reported `Checks: 128  Failures: 20`; eight failures were direct helper
controls and twelve were missing transmission in consumers. The repaired run
in `.claude/debt-DL56/logs/fixed-test.log` reported `Checks: 338  Failures:
0`. The larger successful-run count is expected: successful paths enable
deeper checks. No test source changed after the red run.

Direct controls cover matched-index grazing cosines `.01`, `.005`, and
`.001`; nearby `1.5001` unequal-index checks; exact tangent; normal-incidence
common-IOR scales `1e-4`, `1`, and `1e100`; and a TIR control. They use
independent scalar Fresnel and Snell calculations. Consumer coverage uses
real RGB/NM `DielectricSPF` paths and real RGB/NM standalone non-absorbing
`SubSurfaceScatteringSPF` exits from a synthetic intersection. This proves
the shared helper retains transmission without adding a rendered fixture.

## Scope and remaining classification work

The shared helper serves DielectricSPF, PerfectRefractorSPF,
SubSurfaceScatteringSPF, PolishedSPF, BioSpecSkinSPFHelpers, RandomWalkSSS,
and transparent-shadow handling in RayCaster. Composite and fabric paths
forward their base SPF results; SMS photon paths consume SPF lobes. CoatedLayer has its own matched-index identity, and AR thin-film uses complex-stack
optics. No interfaces, stack behavior, eta scaling, or integrator MIS formula
changed.

DL-58 remains a distinct false-TIR/cancellation group, before Fresnel is
evaluated: `Optics::CalculateRefractedRay`, `FibreLobeMath::FrDielectric`,
the `SubSurfaceScatteringBSDF` and `SubSurfaceScatteringSPF` cosine helpers,
and `ManifoldSolver::ComputeDielectricFresnel`. The static recomputation
records matched-index cosI=`1e-7` with
`k=9.992007221626409e-15`, rejected by the `<1e-12` cutoff, and cosI=`1e-9`
where sinT2 rounds to `1` and reports TIR although physical reflectance is
`0`. Those are Snell/TIR classification failures, not the DL-56 Fresnel
quotient bug, and this closure does not claim them fixed.

## Validation and file status

The retained audit selects 26 gates across 54 clean-build/link/run stages.
At source repair `df7e3dad`, the clean Xcode audit succeeded with zero
compiler warnings. Its four notices were three missing local OIDN search-path
lines and one AppIntents metadata notice. The standalone completion report records the full final branch and merged-master gate results.

| File | Status |
|---|---|
| `src/Library/Utilities/Optics.cpp` | Modified by `df7e3dad`: scaled Fresnel amplitude-ratio evaluation. |
| `tests/DielectricGrazingFresnelTest.cpp` | Added by the committed-before-execution test work: direct controls and real RGB/NM consumer paths. |
| `tests/README.md` | Updated test-scope entry. |
| `docs/DEBT_LEDGER.md` | DL-56 closure, regression recipe, counts, and DL-58 separation. |
| `docs/DL51_SUBSURFACE_EXIT_IOR.md` | Replaces the stale DL-56-open residual with this closure. |
| `docs/DL54_BSSRDF_PROJECTION_NORMAL.md` | Updates the later DL-56 closure and distinct DL-58 residual. |
| `docs/DL55_RANDOM_WALK_FALLBACK_PROPOSALS.md` | Updates the later DL-56 closure and labels its own counts historical. |
| `docs/README.md` | Adds a link to this closure record. |
| `docs/DL56_GRAZING_FRESNEL.md` | This closure record. |

No source files were added or removed, so no build-project source-list update
is required.
