# ~~DL-50: random-walk spectral survival weighting~~

CLOSED 2026-09-12 — `29ce61c7`, RandomWalkSurvivalTest:
`All DL-50 survival tests passed` (13 weight failures before repair).

## Mechanism

`RandomWalkSSS::SampleExit` samples a free-flight distance before deciding
whether the path reaches a boundary. If the physical scalar extinction is
s and the exponential proposal rate is q, the boundary survival probability
is exp(-q d), while physical transmittance is exp(-s d). The conditional
importance weight must therefore be exp((q-s)d).

Ordinarily q=s and that factor is one. The original NM branch multiplied
by exp(-s d) without dividing by the survival probability, so neutral pure
absorption paid Beer attenuation twice in expectation. The same branch
runs at each internally reflected boundary, and the resulting throughput
feeds both `weightNM` and `weightSpatialNM`.

The implementation also has a tiny-extinction fallback, where q can differ
from s. A general transmittance-to-survival ratio preserves that proposal;
unconditionally replacing the factor with one would not do so. The ratio
can be evaluated as a single exponential, avoiding division of two tiny
values. Angular weighting, diffusion normalization, boundary Fresnel,
and the existing low-scatter diffuse-exit approximation are separate.

## Audit scope

See the final evidence for the source audit and regression results.
PT Pel/NM, the PT HWSS per-wavelength SSS fallback, and BDPT eye/light
Pel/NM walks consume this shared sampler. VCM and MLT reuse the BDPT
subpath generators. Their consumers do not undo or reapply interior
free-flight survival, so the correction belongs in the sampler.

## ~~Independent residual DL-55: fallback proposal density~~

CLOSED 2026-09-12 — `cba4e88c`, RandomWalkFallbackProposalTest:
`All DL-55 fallback proposal tests passed` (unfixed: 17 weight failures).

RGB sampling replaced tiny channel rates with the maximum extinction,
but collision and survival denominators still used the original rates.
DL-55 now shares the effective proposal rates between sampling and both
denominators, retaining physical extinction in the numerators. NM collision
now includes exp((q-s)t) and applies its weight at the exact positive
fallback threshold; DL-50's NM boundary survival ratio is preserved.

The new regression uses actual closed spheres, a nonuniform RGB rate
mixture, conditional collision/boundary oracles and unconditional Beer
quadrature (2948/12288 active exits). The tiny NM cases verify geometry
range, activity and full/spatial ratios. The discovery here was static;
DL-55 supplies the committed executed red-proof. The independent RGB
collision-density cutoff remains DL-57. See [DL-55 closure](DL55_RANDOM_WALK_FALLBACK_PROPOSALS.md).

## Regression evidence

Tests were committed before execution against the unchanged library at
`f36b2f03f3fdd435431319294967009ce5d3bf96`. Terra authored the initial
regression in `d909b8f0`; root corrected fixture ownership before execution
and added collision/reflection/fallback cases. The final unfixed test tree
was `84155984`; the test and all controls were unchanged for the repair.

| Observation | Unfixed | Fixed |
|---|---:|---:|
| Neutral conditional NM spatial weight (each of 450/550/650 nm) | 0.13533541857209896 | 1 |
| Unconditional NM pure-absorption mean | 0.060806422833329661 | 0.24658203125 |
| Independent Beer expectation | 0.2465971365597143 | 0.2465971365597143 |
| Active pure-absorption strata | 1010/4096 | 1010/4096 |
| NM after one collision, each wavelength | 0.13901929377971942 | 0.80000000000000004 |
| NM after one internal reflection, each wavelength | 0.018315675520103544 | 1 |
| NM tiny-extinction conditional weight, each wavelength | 0.99999999999855604 | 1.000000000018556 |

The midpoint-stratification tolerance is 2/4096; this is deterministic
quadrature, not an estimate of independent-render uncertainty. The fallback
oracle divides independently evaluated physical transmittance by proposal
survival and uses tolerance 1e-12. Its verified chord is
199999999.99999899; the three final samples each consume four draws and exit
at the far boundary. Ordinary measured chord: 1.9999989999989998.

A first radius-1e19 fallback fixture failed activity, so its weight failures
were not counted as red-proof. The final radius-1e8 fixture keeps the inward
offset representable. Explicit vector storage for its sampler matches the
ordinary fixtures and makes the final draw-count guard pass. All intermediate
logs are retained; no geometry or sampler-production changes were made.

## Files and scope

| File | Status |
|---|---|
| `src/Library/Utilities/RandomWalkSSS.cpp` | Modified: NM boundary survival ratio only (`29ce61c7`). |
| `tests/RandomWalkSurvivalTest.cpp` | Added: committed unfixed regression, activity controls, independent probability checks. |
| `tests/README.md` | Modified: regression scope and red/fixed outcome. |
| `docs/DEBT_LEDGER.md` | Modified: DL-50 closure, DL-55 residual, counts and recipe. |
| `docs/DL04_SSS_RADIANCE_DECISION.md` | Modified: close its spectral-survival confound; retain historical observations. |
| `docs/DL50_RANDOM_WALK_SURVIVAL.md` | Added: mechanism, evidence and residual. |

No source files were added or removed under `src/Library`, so the five build
project source lists require no changes. The selected gates are
RandomWalkSurvivalTest, RandomWalkSSSTest, BSSRDFEntryPointTest,
BSSRDFNormalizationTest, SobolDimensionBudgetTest, SSSRadianceScalingTest,
and SourceHygieneTest. The standalone completion report records clean-build
results, exact gate counters, independent review rounds and merged-master
validation.
