# ~~DL-55: random-walk fallback proposal densities~~

CLOSED 2026-09-12 — `cba4e88c`, RandomWalkFallbackProposalTest:
`All DL-55 fallback proposal tests passed` (unfixed: 17 weight failures).

## Mechanism and repair

The RGB sampler replaces a physical extinction below 1e-20 with the maximum
channel rate when sampling distance. Its collision and boundary survival
PDFs previously used the original physical rates, so the evaluated density
did not describe the sampled proposal. The repair computes effective rates
once and uses their uniform mixture in both denominators. Physical
transmittance and scattering remain in the numerators. Ordinary rates reuse
the already computed physical transmittances when no substitution occurs.

For physical NM extinction s and sampled rate q, collision weighting is
sigma_s/q * exp((q-s)*t). The old formula omitted the exponential ratio and
skipped the entire collision weight at q exactly 1e-20. The initial
maximum-extinction gate guarantees a positive q, so the corrected formula
also applies at that equality. DL-50's NM boundary ratio is unchanged.

## Committed red-proof and observations

Terra committed the test and README as `4ba81ba4` before execution. Root
committed `fd9da1e0` to order Get2D draws, assert full/spatial fields separately
and print numerical evidence. The first run used the unchanged library at
`86622dc2b35f67da05ae00e659ebf5e8c85bbf28`. It produced 17 failures: six RGB
boundary fields, four RGB collision fields, three unconditional Beer means
and four NM collision fields. Geometry, draw-count, activity and zero-scatter
controls passed. No test code changed in repair `cba4e88c`.

The fixture uses synthetic south-pole entry records and real closed sphere
intersections. RGB physical extinction (0,0.4,1) yields effective proposal
rates (1,0.4,1), which distinguish the mixture from a selected-channel PDF.
One test selects the zero channel before a boundary event; another selects
the nonzero green channel for a collision followed by an exit. Full weights
use an independent angular Schlick calculation; spatial weights expose the
free-flight ratios directly. Zero-scatter collision controls remain inactive.

The pure-absorption check uses 4096 midpoint distance strata for each of
three channel choices: 12288 attempts, 2948 active exits before and after
repair. It is deterministic quadrature, not a confidence interval or
independent-render variance estimate. Observations after repair are:

| Channel | Observed spatial mean | Physical Beer target |
|---|---:|---:|
| R | 0.99962025208276473 | 1 |
| G | 0.44915851204245183 | 0.44932914384902295 |
| B | 0.13528402522876865 | 0.13533541857209896 |

All are within the test's absolute tolerance 0.00048828125. Before repair,
the corresponding means were 0.45418227905622033, 0.20407733459972346 and
0.061466948844096758.

NM uses radius 1e8 so the entry epsilon remains representable. The real
initial chord is 199999999.99999899. For q=1.3e-19 and physical extinction
9.386e-21, the sampled collision distance is 50000174.9407233 and remaining
chord 149999825.05927569. The omitted collision factor is
1.0000000000060307; the old spatial value 0.072200000001306247 differs from
the independent target 0.07220000000174169 by more than the 1e-13 tolerance.
After repair the observed spatial value is 0.072200000001741677.
At q=1e-20, the old skipped weight gives 1.0000000000013916 instead of the
independent target 0.072200000000133976; repair gives 0.072200000000133963.
Both NM tests independently evaluate physical/proposal collision and
survival factors and check full as well as spatial weights.

These are direct helper regressions. They do not measure rendered image
bias or establish arbitrary spectral material behavior.

## Sibling and consumer audit

The shared sampler serves RGB and NM. Boundary reflection/TIR repeats reuse
the event-weight updates. PT Pel/NM consumes full/spatial fields; HWSS
falls back to NM for SSS. BDPT eye/light generators consume the same fields,
and VCM/MLT reuse those generators. No consumer recomputes the fallback
free-flight density. Angular Sw, Fresnel and eta conventions are untouched.

General homogeneous-medium transport already samples and evaluates its
maximum-rate RGB proposal consistently; its NM path uses wavelength
extinction. Disk BSSRDF sampling has radial area proposals rather than
exponential fallback distances. Coated/Fabric exclude SSS substrates and
CompositeMaterial does not forward random-walk parameters. These are
source-audit conclusions, not additional integration renders of this fixture.

## ~~Independent residual DL-57: collision density cutoff~~ CLOSED 2026-09-12

`c187f2cc`, `RandomWalkDensityCutoffTest`: `All DL-57 density cutoff tests passed`
(unfixed: one missing-large-RGB-exit activity failure). The finite-positive
collision-density guard replaces the dimensional cutoff; proposal densities,
physical weights and throughput pruning are unchanged. A real closed-sphere
scale pair preserves unit normalized spatial weight, with NM and absorption
controls. See [DL-57 closure](DL57_RANDOM_WALK_DENSITY_CUTOFF.md) for measured
ranges, weights, committed red-proof and the sibling audit.

DL-49/DL-52 and the separate DL-58 grazing-classification group remain open;
DL-56 was closed by `df7e3dad`.

At this closure the main ledger had 56 rows: 46 open and 10 closed by cleanup.

## File status and gates

| File | Status |
|---|---|
| `src/Library/Utilities/RandomWalkSSS.cpp` | Modified: effective RGB proposal densities and NM collision ratio (`cba4e88c`). |
| `src/Library/Utilities/RandomWalkSSS.h` | Modified: describe effective proposal sampling/weighting. |
| `tests/RandomWalkFallbackProposalTest.cpp` | Added: committed-before-execution regression and numerical observations. |
| `tests/README.md` | Modified: coverage and red/fixed evidence. |
| `docs/DEBT_LEDGER.md` | Modified: DL-55 closure, DL-57 residual and counts. |
| `docs/DL50_RANDOM_WALK_SURVIVAL.md` | Modified: close the discovered fallback residual. |
| `docs/DL54_BSSRDF_PROJECTION_NORMAL.md` | Modified: update the related DL-55 open-status reference. |
| `docs/DL55_RANDOM_WALK_FALLBACK_PROPOSALS.md` | Added: mechanism, evidence, scope and residual. |

No library files were added or removed; build-project source lists need no
changes. Selected gates: RandomWalkFallbackProposalTest,
RandomWalkSurvivalTest, RandomWalkSSSTest, BSSRDFEntryPointTest,
BSSRDFNormalizationTest, SobolDimensionBudgetTest, SSSRadianceScalingTest and
SourceHygieneTest. The external completion report retains exact counters,
clean build evidence, fresh independent reviews and merged-master validation.
