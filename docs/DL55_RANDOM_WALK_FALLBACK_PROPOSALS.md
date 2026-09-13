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

`c187f2cc`, `RandomWalkDensityCutoffTest`: `All DL-57 density cutoff tests passed`.
The pre-fix run had one failure: the large RGB collision was missing because the
old density cutoff rejected a valid exit; scaled RGB, NM and pure-absorption
controls passed. The repair retains finite positive densities using the existing
`FiniteMath` predicate and changes no denominator, weight, proposal-rate or
throughput-pruning formula.

The former RGB guard rejected pdfMixture < 1e-20 even with matching
physical/proposal rates. Root recomputed neutral sigma_s=sigma_t=1e-20 at
t=5e7: the density is 9.999999999995e-21 but the conditional collision weight
is exactly one. The real closed-sphere regression measured initial and
post-collision ranges of `199999999.99999899` and `149995554.97086945` for the
large sphere, and `1.9999989999989998` and `1.4999545497077045` for the scaled
pair. Large RGB spatial weights were `1` in every channel and full weights
`1.0171875000000001`; the scaled pair was spatial
`1.0000000000000002` and full `1.0171875000000004` in every channel. NM was
spatial `1`, full `1.0171875000000001`; pure-absorption collision controls
remained inactive.

The legal sampler bound is static evidence, not a renderer measurement:
`xi < 1` gives maximum optical distance `36.7368005696771`, selected survival
`1.1102230246251573e-16`, mixture survival lower bound
`3.700743415417191e-17`, and collision-density lower bound
`3.7007434154171905e-37` at minimum effective rate `1e-20`. The sibling
`pdfExit < 1e-20` guard remains unchanged because sampled exit distance cannot
exceed the sampled distance; throughput pruning remains a separate policy.
DL-49/DL-52 and the separate DL-58 matched-index grazing-classification group
remain open work; DL-56 is closed by `df7e3dad`.

At this closure the main ledger has 57 rows: 45 open and 12 closed by cleanup.

## File status and gates

| File | Status |
|---|---|
| `src/Library/Utilities/RandomWalkSSS.cpp` | Modified: finite-positive RGB collision-density guard (`c187f2cc`); no production `src/Library` files added or removed. |
| `tests/RandomWalkDensityCutoffTest.cpp` | Added before first execution (`b86925e5`, `a6b6622f`, `0226242d`): real closed-sphere RGB/NM regression and controls. |
| `tests/README.md` | Updated with DL-57 coverage and red/fixed evidence. |
| `docs/DEBT_LEDGER.md` | Updated DL-57 status and counts. |
| `docs/DL55_RANDOM_WALK_FALLBACK_PROPOSALS.md` | Struck residual heading and recorded closure evidence. |
| `docs/DL57_RANDOM_WALK_DENSITY_CUTOFF.md` | Added bounded closure record. |
| `docs/README.md` | Added closure index link. |

No library files were added or removed; build-project source lists need no
changes. Selected gates: RandomWalkFallbackProposalTest,
RandomWalkSurvivalTest, RandomWalkSSSTest, BSSRDFEntryPointTest,
BSSRDFNormalizationTest, SobolDimensionBudgetTest, SSSRadianceScalingTest and
SourceHygieneTest. The external completion report retains exact counters,
clean build evidence, fresh independent reviews and merged-master validation.
