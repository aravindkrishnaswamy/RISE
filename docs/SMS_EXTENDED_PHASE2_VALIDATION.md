# Extended SMS Phase 2 validation

Phase 2 is implemented on `sms-ext-phase2`, based on master
`08552560b54b517b7a8a0696eb9d317953b26bbe`, but is not yet merged. The first independent review round found two
estimator P1s; the material/API and evidence reviewers found zero P1/P2.
Production source includes the chart/depth fixes at `8ad9a40cf` and the
threshold sibling guard at `13a31c7d7`. Committed
chart/depth red/green proofs are complete. The `8ad9a40cf` replacement
gate was stopped as incomplete after a sibling audit identified
nonpositive solver thresholds that retain eligibility despite zero
accepted Newton roots. The threshold fix and its committed proof precede
the next full replacement gate. The
completed gate below describes the earlier `89a9ab0a6` source checkpoint,
not validation of the review fixes. No ledger row closes at this checkpoint.

## Implementation and scope

The internal opt-in samples one RGB component or a fixed NM wavelength and
one native R/T seed walk per original trial. Point and spot lights use
root-level estimator A: independent conditional retries and a
survival-weighted reciprocal tail. Every failed proposal remains in the
original trial average. Deposits divide by the channel probability, emitter
selection probability and original trial count. Caster and event
probabilities are already included in rediscovery; they are not divided
again. Independent discoveries are never deduplicated. Reference paths
retain finite signed emission and bypass geometric/radiance clamps.

Prepared scene data includes eligible native casters and conservative
unsupported-material guards. Uncertified/spatial indices, inherited clear
CSG surfaces, weave pass-throughs, and unaudited providers make extended
anchors ineligible, disabling contribution, emitter-hit suppression and
delta-light shadow opacity together. A real composite or forwarded native
composite walker selects the adopted scene-wide legacy policy, naming the
first object. Area emitters retain the existing evaluator and suppression
until Phase 3. HWSS and the dedicated `SMSShaderOp` remain legacy.

Root comparison checks the selected domain, ordered objects/materials,
events, side/index and starting-membership signatures, and every vertex's
geometry, native UV and object context. Its band is capped at
sqrt(machine epsilon) times endpoint distance. Final inverse-Jacobian
correction plus coordinate roundoff must resolve that band. This is a
numerical diagnostic, **not a certified nonlinear root-isolation bound**.
Reference polishing bypasses the legacy fixed displacement dead zone.
Geometry uses true surface points after undoing authored object launch
bias; material queries retain native intersection/raster contexts.

## Historical pre-review make regression gate

All executable builds had checked zero exits and zero compiler diagnostics.
Twenty executables and 27 mode runs completed with zero test failures.
Raw logs and results are under `.claude/logs/sms-phase2/final-gate/`.

| Test | Passed / failed |
|---|---:|
| ManifoldSolverTest | 388 / 0 |
| SMSUniformDispersionTest | 300 / 0 |
| SMSUniformDispersionTest --shipped | 10 / 0 |
| ExteriorIndexInvarianceTest | 299 / 0 |
| SMSEmitterDirectionTest | 344 / 0 |
| SMSMediumAnchorTest | 27 / 0 |
| TransparentShadowPartitionTest | 42 / 0 |
| WeaveGapShadowTransmittanceTest | 244 / 0 |
| OpenSheetIndexConventionTest | 24 / 0 |
| GradedIndexInteriorFactorTest | 101 / 0 |
| ManifoldNormalDerivativeTest | 141 / 0 |
| DoubleSidedEmitterTest | 34 / 0 |
| AlphaSMSGeometryTest | 216 / 0 |
| AlphaSMSTransportTest | 20 / 0 |
| AlphaSMSReciprocalTest | 27 / 0 |
| PTGuidingMISPartitionTest | 185 / 0 |
| SourceHygieneTest | 169 / 0; 456 test files |
| CstDeriveGoldenTest | 458 matches, 0 drift; 465 corpus scenes covered |
| SMSDomainReplayTest | 8200 / 0 |
| SMSLegacyModeTest | 21 / 0 |
| SMSExtendedReferenceTest --synthetic-only | 35 / 0 |
| --geometry-only | 3241 / 0 |
| --delta-only | 177 / 0 |
| --production-only | 177 / 0 |
| --unsupported-only | 217 / 0 |
| --signed-only | 353 / 0 |
| --slab-only | 80 / 0 |

Clean make compiled 376 units with zero compiler diagnostics. Clean Xcode
Deployment and Opto each compiled 393 units with zero compiler diagnostics.
Each Xcode build had two separately discounted environment notices: the
unavailable OIDN linker search path and skipped AppIntents metadata
extraction. Source SHA-256 provenance is recorded in `final-gate/source.json`.

The supplementary numerical harness completes five controls in each Xcode
configuration: geometry **3241/0**, unsupported casters **217/0**, delta
queries **177/0**, PT production **177/0** and signed emission **353/0**.
Each links 372 actual Xcode native objects. The test translation unit and
make-only Profiling utility are compiled using that configuration's actual
common response flags and deployment target. Final compiles/links have
zero diagnostics. Its initial mixed make/Xcode link had target/LTO warnings
and ran no controls; failed attempts are retained under
`xcode-link-target-mismatch/` and `xcode-mixed-lto/`. Renderer source/build
settings are unchanged. The original pipeline exits1 at this supplementary
harness; the corrected harness exits0. `completed-summary.json` records
that recovery alongside all 27 green mode runs, ten green Xcode controls
and verified source hashes; no failed attempt is relabelled as a pass.

ASan/UBSan instruments the test translation unit and four changed production
translation units (`ManifoldSolver`, `ObjectManager`, `PathTracingIntegrator`
and `SMSShaderOp`) at O1. Geometry, unsupported casters, PT production and
signed emission pass; expanded Snell/uniform signed coverage is **353/0**.
Unchanged library objects are native LTO objects. This is not whole-library
instrumentation or a leak-sanitizer claim. Evidence: `sanitizer-driver.log`
and `sanitizer-signed-entry-driver.log` under the Phase 2 log directory.

## Native geometry, production and tail measurements

Geometry covers actual R/T, mixed R-T and R-T-R walks, TIR, closed nested
exits and start-inside, both double-sided indexed-mesh windings and
incidence sides, transforms, sphere/plane controls, and finite-difference
constraint Jacobians. Distinct roots on separate patches of one mesh stay
distinct at scales 0.01, 1 and 100. Very thin 1e-9 patches consistently
return zero proposals; they do not establish positive coverage.

Point/spot query and PT production fixtures use real camera paths to a
Lambertian receiver below an open double-sided mirror or index-1.5 refractor,
both windings, RGB and NM at 450/650 nm. Each comparison uses four salts
and 16,384 original samples per salt. The upward spot contributes only by
reflection; the native-domain analytic virtual-image reference has zero
SD. Point production also includes its unoccluded direct light. Receiver
depth one matches that transport order. Every channel passes the original
three-sample-SD plus ordinary summation-roundoff check. Observable
compensated summation preserves the bound under make/Opto reassociation.
Signed RGB point/spot emission (`1 -0.5 0.2`) is tested through both Snell
and uniform evaluator routes and their PT production entry points.

The slab matrix completes both windings and cones 30/45, 44/45 and 80/85,
with four salts at 4096 spp on a shared 16x16 pinhole camera. Native BDPT
uses matching materials/transport and reports its own channel SD. All
12 narrow-cone channel comparisons pass three combined sample SDs.
The user accepted the front-winding wide-cone red discrepancy as variance:
A 0.6289669133101607, SD 0.0001961442784790; BDPT
0.6300625785345996, SD 0.0000835912663127; -0.1738978%,
-5.138813 combined SDs. Its original band remains printed. Wide-cone
comparisons are measurements, not accuracy assertions or DL-420 closure
evidence. Final per-channel data are in `final-gate/slab-summary.json`.
Earlier missing-shader/orthographic references were invalid; the partial
pre-ruling slab run was cancelled with exit143 and is not a passing matrix.

Synthetic known laws include zero trials, channel factors, signed deposits,
uncapped reciprocal controls and rare-root roulette tails. Ordinary/uncapped
controls use 2,000,000 original trials per salt; rare roulette uses
100,000,000, each with four independent discovery/retry salts. The original
three-SD and 6% checks remain unchanged. Rare roulette channel means/SDs
are 12.1689496902/0.499668805014, 20.3670429425/0.880404706406,
and 27.0517586064/1.38477847942, against analytic 12.5/20/27.5
(reference SD0). Logarithmic retry histograms put the mixture median in
[2,3], p90 [4,7], p99 [8,15], and p99.9 [128,255]. These are mixture,
not conditional rare-root quantiles. Stops affect 0.463–0.467% of loops;
maximum weighted reciprocal estimates are 1.11–1.77 million and maximum
normalized single deposits 0.499–0.859. Cost is unbounded in the worst case;
no speedup or finite-variance guarantee is claimed. Historical `maxK` labels
mean weighted reciprocal estimate; current logs use `maxReciprocal`.

Production logs report proposals, zero trials, Newton iterations, retries,
tail trials, roulette stops, accepted/owned discoveries and rejected roots.
Counters are per fixture, not whole-render complexity bounds. The historical `89a9ab0a6` committed
production proof's peak process footprint is **82,379,280 bytes** under
`/usr/bin/time -l`, including library/test/render state; it is not a
per-worker scratch bound. Arm64 sizes: config112, root184, reference
counters320, domain vertex1584, medium capture1264, starting media40,
domain counters32 bytes. Legacy SpecularInfo56, ManifoldVertex336,
SMSChainRecord2176 and IORStack40 remain unchanged.

## Committed-source red/green proofs

Each proof rebuilt coherent committed native sources and dependent headers,
checked library/test build exits and compiler diagnostics, then restored
HEAD and rebuilt green. Missing helper APIs are unavailable tests, not
numerical failures. Checkpoint suites expanded over time; their different
counts are not interchangeable.

| Defect / source checkpoint | Red passed / failed | Restored passed / failed | Evidence under Phase 2 logs |
|---|---:|---:|---|
| Unsupported-index coupling, f925c29c4 | 14 / 10 | 24 / 0 | caster-coupling--unsupported-only-red.log; proof-results.json |
| Earlier numerical/root/spot source, 60a461692 | geometry 3226 / 14; production 132 / 44 | 3240 / 0; 176 / 0 | proof-results.json |
| Initial native master production | 126 / 50 | 176 / 0 | native-master--production-only-red.log |
| Inherited-CSG plus weave siblings, 6d2c9891d | 105 / 40 | 145 / 0 | caster-proofs/proof-results.json |
| Weave pass-through, f44caa909 | 125 / 20 | 145 / 0 | caster-proofs/proof-results.json |
| Native master expanded caster/production | 101 / 44; 127 / 50 | 145 / 0; 177 / 0 | caster-proofs/proof-results.json |
| Neutral provider checkpoint, 298bf64ea | 173 / 20 | later expanded 217 / 0 | provider-guard-green-run.log; legacy-provider-red-run.log |
| Forwarded composite walker, f62eae39a | 185 / 32 | 217 / 0 | provider-proofs/proof-results.json |
| Native master provider control | 165 / 52 | 217 / 0 | provider-proofs/proof-results.json |
| Signed clamp checkpoint, e4ecbb2f1 | 149 / 28 | 177 / 0 | signed-proofs/proof-results.json |
| Native master signed control | 111 / 66 | 177 / 0 | signed-proofs/proof-results.json |
| Signed Snell/uniform siblings, e4ecbb2f1 | 297 / 56 | 353 / 0 | signed-entry-proofs/proof-results.json |
| Native master signed siblings | 221 / 132 | 353 / 0 | signed-entry-proofs/proof-results.json |

Master's signed failures include legacy coverage/accuracy differences;
they are not counts of signed-clamp defects. Its front-wound upward-spot
glass reflection means are exactly zero in RGB and 450/650 nm; candidate
production is positive within three measured SDs. The simple mirror
control is already positive on master NM and does not prove DL-312's
original missing supplemental seeds. That row requires its actual
ExteriorIndexInvarianceTest mirror fixture before closure. DL-437 still
requires HWSS; DL-420 retains the accepted wide-cone measurement. All three
rows stay open. No new ledger row is added. The aborted pre-signed gate,
cost and sanitizer evidence are preserved under `checkpoint-4e211d99f/`;
none is substituted for corrected-source evidence.

## Historical pre-review off-mode cost and output gate

Actual interleaved library/test builds compare master `08552560b` with
`96b73057ddeea4e7068b3a198bacc4139cb7d17d`, four salted trials per fixture, identical
worker policy and sequential renders. All 16 paired output hashes and
channel means are identical. Time includes the same native test-render
work on each side; means and sample SDs are seconds.

| Fixture | Master mean ± SD | Candidate mean ± SD | Paired change % mean ± SD |
|---|---:|---:|---:|
| RGB k1 | 0.961168094 ± 0.006179403 | 0.942920094 ± 0.002791386 | -1.895499 ± 0.690842 |
| RGB k2 | 0.734318333 ± 0.003011683 | 0.726885292 ± 0.008980231 | -1.013908 ± 0.946574 |
| NM | 1.710615302 ± 0.005328788 | 1.715625479 ± 0.002314363 | 0.293572 ± 0.321692 |
| HWSS | 5.743522740 ± 0.012806160 | 5.740002062 ± 0.018625509 | -0.061426 ± 0.147112 |

Evidence: `.claude/logs/sms-phase2/phase2-interleaved-cost.json` and
each `phase2-interleaved-{A,B}{0..3}.log`, with checked build exits in
`cost-driver.log`. No whole-render speedup is inferred from these timings.

## Chart/depth checkpoint off-mode cost and output gate

The replacement gate compares committed master `08552560b` with
`74d4365dd` (native source `8ad9a40cf`) using eight actual interleaved
library/test builds and four salted trials per fixture. All 16 paired
output hashes and channel means are identical. Worker policy and sequential
rendering match; timings are seconds and uncertainties are sample SDs.

| Fixture | Master mean ± SD | Candidate mean ± SD | Paired change % mean ± SD |
|---|---:|---:|---:|
| RGB k1 | 0.945636687 ± 0.010939475 | 0.942783167 ± 0.001736797 | -0.290174 ± 1.336248 |
| RGB k2 | 0.719287781 ± 0.007837574 | 0.716280834 ± 0.005068141 | -0.414833 ± 0.415376 |
| NM | 1.685690115 ± 0.018286842 | 1.695444927 ± 0.019160919 | 0.580515 ± 0.764642 |
| HWSS | 5.648385990 ± 0.050602435 | 5.668684760 ± 0.037738600 | 0.361267 ± 0.377854 |

All paired timing changes lie within three sample SDs. These measurements
do not establish a speedup. Restored composite-policy, HWSS-fallback and
full-domain controls pass at **1168/0**, **3147/0** and **8200/0**.
Evidence: `.claude/logs/sms-phase2-round2/phase2-interleaved-cost.json`
and `cost-driver.log`; every source restore, library build and test build
has a checked zero exit and zero compiler diagnostics.

## Review and integration

Round 1 reviewed `83615cc347c4a70062f907f8942f325396ca7c70` read-only.
The material/medium/API and cost/tests/doc-fidelity reviewers returned zero
P1/P2. The estimator reviewer found two P1s, both reproduced:

- Raw UV differences split a physical root across native chart seams.
  The sibling audit reproduced this on an ellipsoid and added sphere,
  ellipsoid, torus, cylinder and real double-sided indexed-mesh seam
  controls. Actual native UV records remain intact. Native periodic
  charts use periodic coordinate distance; an audited constant event law
  without a normal modifier establishes that UV cannot affect a root's
  price/index/event inputs. Geometry and context UV records must agree.
  Varying/unaudited native seam and mesh-boundary contexts remain uncertain
  zero proposals. Authored UV generators retain their native coordinates
  and ordinary context comparison for varying laws. This is conservative
  numerical coverage, not a claim of complete textured-seam coverage.
- Zero maximum depth or a target greater than the maximum left anchors
  eligible despite every proposal being zero. The shared eligibility guard
  now disables contribution, suppression and shadow opacity together.
  Actual double-sided index-1 panes in both windings compare RGB and NM
  450/650 PT against the impossible extended configurations, four salts
  each, with bit-identical output required.

Initial tests were committed at `b26da1d84` and fixes at `d83c71d1e`;
the broader chart audit at `8ad9a40cf` supersedes the initial sphere-only
rewrite. Its `8ad9a40cf` targeted test reports **1177/0**. The initial
committed proof reports pre-fix `83615cc34` **175/117**, master **61/60**
(helper-dependent seam tests unavailable), restored `d83c71d1e` **292/0**.
The expanded chart test is red on `d83c71d1e` at **1085/92**; committed
master reports **61/60** for available depth tests and restored `8ad9a40cf`
reports **1177/0**, with checked zero-diagnostic builds. Evidence is under
`.claude/logs/sms-phase2/periodic-review-proofs/`. The `8ad9a40cf` full
replacement gate was aborted with exit143 during the WeaveGap test build
after the threshold sibling was identified; its completed rows and stop
reason remain in `.claude/logs/sms-phase2-round2/aborted-summary.json`.
It is not a passing gate. The threshold sibling is reproduced and fixed at `13a31c7d7`: zero and
negative thresholds cannot satisfy any Newton acceptance condition, yet
previously retained anchor eligibility. Active anchors now require a finite,
positive solver threshold. The expanded committed checkpoint test reports
**1237/60**, with plain PT positive and extended PT zero in the new cases.
Coherent master reports **121/120** for available configuration controls
(seam helpers absent), and restored `13a31c7d7` reports **1297/0**. All
library/test builds have checked zero exits and zero compiler diagnostics;
evidence is in `.claude/logs/sms-phase2/threshold-proofs/`. The full
replacement gate under `.claude/logs/sms-phase2-round2-final/`, fresh
Round 2 reviewers and Phase 2 merge remain pending.
Round 1's clean verdicts do not apply to the changed source.
