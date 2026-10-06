# Extended SMS Phase 2 validation

Phase 2 is implemented on `sms-ext-phase2`, based on master
`08552560b54b517b7a8a0696eb9d317953b26bbe`, and remains unmerged.
The current completed replacement gate freezes checkpoint `1187f276ce66a65052b4bc0b5f01aa043c8ea2bd`: 45 make modes / 483402 checks / 0 failures, 36 actual Xcode-linked controls and 20 full-project sanitizer modes. Clean make, Deployment and Opto pass with zero owned diagnostics. The Fresnel-dependent fallback, raw-film price and wavelength-bound native-event repairs have coherent committed-source proof and a complete replacement gate. Fresh Round 12 review completed: estimator and material reviewers found two P2s in uncoated fallback replay; evidence review found no P1/P2. The repairs require a fresh Round 13 gate and review. Earlier sections are historical checkpoint evidence.


## Historical composed gate at a6b4aa37a (2026-10-04)

Evidence: `.claude/logs/sms-phase2-round5-final-v3/`, especially
`source.json`, `composition.json`, `proofs/results.json`,
`final-gate/completed-summary.json`, `final-gate/slab-summary.json`,
`final-gate/xcode-native-control-results.json`, `sanitizer-driver.log`
and `production-memory.log`. All ten native-source hashes and all three
pinned test hashes verify. This gate explicitly composes unchanged-source
results with fresh tests; it is not a claim that every command was rerun.

The preceding v2 pipeline completed clean make (376 compilation actions),
actual Deployment and Opto (393 each), all 30 make modes, and eight actual
interleaved source builds. All builds have zero owned diagnostics, with only
the allowed OIDN search-path and AppIntents notices in each Xcode build.
However, its Deployment-linked review failed **193299/10**: ten positive
acceptance checks for close roots at world x=1e6 failed. That pipeline remains
failed and is not reported as a full green gate. Its raw evidence is retained
under `.claude/logs/sms-phase2-round5-final-v2/`.

Test-only a6b4aa37a moves the disjoint patches' central edges from ±2e-9 to
±0.5e-9 relative to their translation. It gives native context probes clearance
without changing root centers, root separation, matching bands, derivatives,
or assertions that both roots must be accepted and distinguished. This
eliminates the ten Deployment fixture failures. The native implementation,
other test translation units and performance-mode code are unchanged.
Thus the v2 clean builds, nineteen unchanged executable results, and source
A/B cost measurements remain valid for identical native sources. The current
follow-up rebuilds the changed test, runs all ten of its modes and source
hygiene, recompiles its translation unit with each actual Xcode configuration's
flags and links 372 actual native objects, and reruns sanitizers and memory.

The composed regression gate is **374075 checks / 0 failures**, twenty
executables and thirty modes. CST separately reports **458 MATCH, 0 DRIFT**,
465 corpus scenes, no uncovered or stale entries. Both actual Xcode
configurations pass geometry4297/0, unsupported457/0, delta177/0,
production177/0, signed353/0 and review193319/0. Current make review is
193319/0, modifier-chart164105/0 and focused Round 4 regressions283/0.
The five partial ASan/UBSan modes pass geometry4297/0, unsupported457/0,
production177/0, signed353/0 and review193319/0. Instrumentation covers the
test and five production units: ManifoldSolver, TriangleMeshGeometry,
ObjectManager, PathTracingIntegrator and SMSShaderOp. Remaining library units
use native LTO objects; this is neither whole-library nor leak coverage.

Fresh coherent committed proofs restore all ten changed native paths together.
Pre-R4-fix936952503 gives focused151/132, review193187/132 and production177/0.
Pre-harmonic-fixd63695cab gives focused273/10, review193309/10 and production177/0.
Master08552560b gives available review121/120 and production127/50; new absent
helpers are explicitly unavailable, not numerical reds. Restoreda6b4aa37a
passes focused283/0, review193319/0, production177/0 and geometry4297/0.
All proof builds exit zero with zero diagnostics.

Eight actual alternating committed-source library/test builds compare master
08552560b with d3d5ee127, whose native sources are hash-identical to current.
Four salts per fixture yield sixteen bit-identical float32 RGBA images;
all sixteen internal-double hashes differ. Maximum whole-image mean difference
is 1.7763568394002505e-15. Strict deterministic within-build double parity
remains required. Evidence: v2 `phase2-interleaved-cost.json` and build/run logs.

| Fixture | Master seconds mean ± sample SD | Candidate seconds mean ± sample SD | Paired change % mean ± sample SD |
|---|---:|---:|---:|
| RGB k1 | 0.972532 ± 0.002642 | 0.970548 ± 0.003678 | -0.203938 ± 0.295558 |
| RGB k2 | 0.736288 ± 0.005560 | 0.734602 ± 0.004135 | -0.226246 ± 0.691925 |
| NM | 1.732332 ± 0.016117 | 1.743155 ± 0.010949 | +0.628303 ± 0.697886 |
| HWSS | 5.791303 ± 0.010209 | 5.851994 ± 0.035648 | +1.047475 ± 0.463778 |

These measurements establish neither zero overhead nor a general runtime
bound. Prepared orientation auditing currently adds mesh traversal and four temporary
vectors per object on every preparation, including extended-off preparation.
Shared instances repeat this work. The earlier one-time characterization was
inaccurate; the Round 5 preparation-cost repair is still outstanding. Modified-interface Jacobians compare four scales, including a
noncommensurate scale, with up to sixteen base-step refinements. This is opt-in
solve work; local diagnostics do not certify global continuity or isolation.
Record sizes in bytes are config112, root184, counters320, domainvertex1592
and solver216. Public SpecularInfo56, ManifoldVertex336 and SMSChainRecord2176
remain unchanged. Current production memory passes177/0 in 28.01s wall,
27.85s user and 0.11s system, with maximum RSS **102760448 bytes** and peak
footprint **82461224 bytes**. These are whole-process observations.
The last production fixture reports 131605 proposals, 125049 zero trials,
692802 Newton iterations, 66069 retries, 1957 tail trials, 44 roulette stops,
3300 owned and 125049 rejected roots. The owned counter counts accepted
nonzero physical prices, not every geometrically accepted discovery.

Current slab validation passes80/0 with eighteen channel-separated comparisons,
four salts at4096spp, both windings and each reference's own sample SD. All
twelve narrow-cone comparisons pass the original three-combined-SD bands.
The wide front red difference remains -0.17389784% (-5.13881272 SDs), accepted
by the user as variance; reverse wide red is -0.01395563% (-0.41274280 SDs).
Wide results remain measurements, with no band widening or DL-420 closure.
Scene rejection1168/0, HWSS fallback3147/0 and domain replay8200/0 pass in
the unchanged-native cost stage. No ledger row closes or opens at this gate.

## Historical complete replacement gate at44645f25e (2026-10-04)

Evidence: `.claude/logs/sms-phase2-round4-final-v3/`, including
`proofs/results.json`, `final-gate/completed-summary.json`,
`final-gate/slab-summary.json`, `phase2-interleaved-cost.json`,
`sanitizer-driver.log` and `production-memory.log`.
All ten native source hashes and the three pinned test hashes verify unchanged.
Clean make compiled 376 units; actual clean Deployment and Opto each compiled
393. All have zero owned compiler diagnostics; each Xcode build has only the
two discounted OIDN-search-path and AppIntents notices. Twenty individually
built executables run 29 modes with **373510 checks / 0 failures**. CST separately
reports **458 MATCH, 0 DRIFT**, 465 corpus scenes and no uncovered/stale entries.

Each actual Xcode configuration passes geometry4297/0, unsupported457/0,
delta177/0, production177/0, signed353/0 and review193037/0. The five partial
ASan/UBSan modes pass the same geometry, unsupported, production, signed and
review counts. Instrumentation covers the test and five production units:
ManifoldSolver, TriangleMeshGeometry, ObjectManager, PathTracingIntegrator and
SMSShaderOp. Remaining library units retain their native LTO build; this is
not whole-library instrumentation or leak coverage.

Coherent committed proofs restore all ten changed native paths together.
Pre-fix2509de0fa gives review335547/890 and production177/0; pre-context
c5f66dd80 gives review274957/40 and production177/0. Master08552560b gives
available review121/120 and production127/50; absent new helpers are explicitly
unavailable, not numerical reds. Restored44645f25e gives review193037/0,
production177/0 and geometry4297/0. Every checked build exits zero without
compiler diagnostics.

Eight actual interleaved committed-source library/test builds compare master
08552560b and candidate44645f25e, four salts per fixture. All sixteen float32
RGBA hashes match; all sixteen internal-double hashes differ. Whole-image means
differ by at most 1.7763568394002505e-15. Strict deterministic within-build
double comparisons remain in the rejection suites.

| Fixture | Master seconds mean ± sample SD | Candidate seconds mean ± sample SD | Paired change % mean ± sample SD |
|---|---:|---:|---:|
| RGB k1 | 0.963092 ± 0.009904 | 0.959250 ± 0.003569 | -0.392451 ± 0.899609 |
| RGB k2 | 0.726729 ± 0.007590 | 0.723904 ± 0.006315 | -0.378037 ± 1.584056 |
| NM | 1.708265 ± 0.007604 | 1.713349 ± 0.010513 | +0.297514 ± 0.388459 |
| HWSS | 5.737783 ± 0.028402 | 5.762772 ± 0.013305 | +0.436829 ± 0.395095 |

These four-salt default-path measurements establish neither zero overhead nor
a general runtime bound. Record sizes remain config112, root184, counters320,
domainvertex1584 and solver216 bytes. The current production-only memory run
passes177/0 in 26.36s wall, 26.08s user and 0.25s system, with maximum RSS
**101957632 bytes** and peak footprint **81642024 bytes**; these are whole-process
observations. The last production fixture reports 131605 proposals, 125049
zero trials, 692802 Newton iterations, 66069 retries, 1957 tail trials,
44 roulette stops, 3300 owned and 125049 rejected roots.

The eighteen separate RGB slab comparisons use four salts at4096spp, both
windings and each estimator's own sample SD. All twelve narrow-cone comparisons
pass their original three-combined-SD bands. Front-wound wide red remains
-0.17389784% (-5.13881272 SDs), previously accepted as variance; reverse wide
red is -0.01395563% (-0.41274280 SDs). Wide results remain measurements, with
no band widening or DL-420 closure. Scene rejection1168/0, HWSS fallback3147/0
and domain replay8200/0 also pass in the restored-source cost stage.
No ledger row closes at this checkpoint. Fresh Round 4 review remains required.

## Historical complete replacement gate at15c9c2710 (2026-10-04)

Evidence: `.claude/logs/sms-phase2-round3-final/`, including
`final-gate/completed-summary.json`, `final-gate/slab-summary.json`,
`phase2-interleaved-cost.json` and `current-production-memory.log`.
All ten native source hashes match checkpoint `761c71b51`; no production
arithmetic changed in the float32 acceptance amendment.

Clean make compiled 376 units; clean Xcode Deployment and Opto each compiled
393. All have zero compiler diagnostics. Each Xcode configuration has only
the two discounted missing-OIDN-search-path and AppIntents notices. Twenty
executables cover 28 make modes with **36373/0** reported checks. CST separately
reports **458 MATCH, 0 DRIFT**, 465 corpus scenes, no uncovered or stale entries.
Each actual Xcode configuration runs geometry4297/0, unsupported457/0,
delta177/0, production177/0, signed353/0 and review20005/0 controls.
ASan/UBSan covers the test and five changed production units (ManifoldSolver,
TriangleMeshGeometry, ObjectManager, PathTracingIntegrator and SMSShaderOp);
the remaining native library units use their normal LTO build. All five modes
pass; this is partial instrumentation, not whole-library or leak coverage.

Eight actual interleaved committed-source builds compare master `08552560b`
and candidate `15c9c2710`, four salts per fixture. All **16 float32 RGBA hashes
match**; all sixteen internal-double hashes differ. Whole-image means differ
by at most 1.7763568394002505e-15. All 32 internal-double hashes also match the
previous diagnostic outputs, linking the direct buffer measurements reported
below to this run; buffers were not recaptured in this run.

| Fixture | Master seconds mean ± sample SD | Candidate seconds mean ± sample SD | Paired change % mean ± sample SD |
|---|---:|---:|---:|
| RGB k1 | 0.944510 ± 0.005872 | 0.953831 ± 0.011585 | +0.993495 ± 1.696787 |
| RGB k2 | 0.723401 ± 0.002671 | 0.720717 ± 0.003378 | -0.371206 ± 0.174677 |
| NM | 1.683663 ± 0.020483 | 1.708787 ± 0.021311 | +1.492751 ± 0.465066 |
| HWSS | 5.658434 ± 0.051974 | 5.716075 ± 0.063540 | +1.019127 ± 0.718024 |

NM shows a small measurable slowdown (3.21 paired sample SDs); these timings
do not support a zero-overhead claim. Current sizes in bytes are config112,
root184, counters320, domainvertex1584 and solver216. The solver gains 16
private bytes relative to the historical checkpoint; public metadata layouts
are retained. The checked production-only memory run passes177/0 in 25.54s
wall, 25.25s user and 0.26s system, with maximum RSS **101924864 bytes** and
peak footprint **81691176 bytes**. These are whole-process observations.

The 18 separate RGB slab comparisons retain four salts, 4096spp, both windings
and each estimator's own sample SD. All twelve narrow-cone comparisons pass
the original three-combined-SD bands. The front-wound wide red comparison is
-0.17389784% (-5.13881272 SDs), previously accepted by the user as variance;
its original band remains reported and DL-420 stays open. Reverse wide red is
-0.01395563% (-0.41274280 SDs); all remaining wide channels are reported in
the summary. The production last-fixture counters report 131605 proposals,
125049 zero trials, 692802 Newton iterations, 66069 retries, 1957 tail events,
44 roulette stops, 3300 owned and 125049 rejected. These are per-fixture
observations, not an aggregate frame guarantee.

Round 1 found two estimator P1s (chart identity and impossible depth);
a sibling audit found nonpositive solver thresholds. All three have
committed numerical red/green proofs. The interrupted `8ad9a40cf` gate
remains explicitly incomplete. Historical `89a9ab0a6` evidence is labelled
below and is not substituted for current-source validation. No ledger row
closes at this checkpoint.

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

## Historical 13a31c7d7 replacement gate

Native source `13a31c7d7` completes twenty executable builds and 28 make
mode runs: **17665 reported checks, zero failures**, plus 458 CST matches
with zero drift across 465 covered corpus scenes. Each executable build
has a checked zero exit and zero compiler diagnostics.

| Test / mode | Passed / failed |
|---|---:|
| ManifoldSolverTest | 388 / 0 |
| SMSUniformDispersionTest | 300 / 0 |
| SMSUniformDispersionTest--shipped | 10 / 0 |
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
| SourceHygieneTest | 169 / 0 |
| CstDeriveGoldenTest | 458 matches, 0 drift; 465 corpus scenes |
| SMSDomainReplayTest | 8200 / 0 |
| SMSLegacyModeTest | 21 / 0 |
| SMSExtendedReferenceTest--synthetic-only | 35 / 0 |
| SMSExtendedReferenceTest--geometry-only | 4297 / 0 |
| SMSExtendedReferenceTest--delta-only | 177 / 0 |
| SMSExtendedReferenceTest--production-only | 177 / 0 |
| SMSExtendedReferenceTest--unsupported-only | 457 / 0 |
| SMSExtendedReferenceTest--signed-only | 353 / 0 |
| SMSExtendedReferenceTest--review-only | 1297 / 0 |
| SMSExtendedReferenceTest--slab-only | 80 / 0 |

The clean make build compiles 376 units; clean Xcode Deployment and Opto
each compile 393. Compiler diagnostics are zero. Each Xcode configuration
has exactly the two documented environment notices (OIDN search path and
AppIntents metadata). Raw evidence for this historical checkpoint is under
`.claude/logs/sms-phase2-round2-final/`. All twelve actual Xcode-linked controls pass: in each configuration,
geometry **4297/0**, unsupported/configuration **457/0**, delta queries
**177/0**, PT production **177/0**, signed emission **353/0** and review
regressions **1297/0**. Each links 372 actual native Xcode objects; the test
and make-only Profiling unit use that configuration's actual response
flags and target26.2. Final compiles/links have zero diagnostics.

Partial ASan/UBSan instruments the test and four changed production units
(`ManifoldSolver`, `ObjectManager`, `PathTracingIntegrator`, `SMSShaderOp`)
at O1; unchanged objects use the native make build. Geometry **4297/0**,
unsupported/configuration **457/0**, production **177/0**, signed emission
**353/0** and review **1297/0** all pass. This is not whole-library or
leak-sanitizer coverage. `completed-summary.json` records pipeline exit0,
28 make modes, twelve actual Xcode controls, zero compiler diagnostics,
and verified source hashes. `slab-summary.json` retains all eighteen
channel comparisons; all twelve narrow-cone comparisons pass their
original three-SD bands. The accepted wide-cone measurement is unchanged.

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

## Historical 13a31c7d7 off-mode cost and output gate

The replacement gate compares committed master `08552560b` with
`5f7cdaa06` (native source `13a31c7d7`) using eight actual interleaved
library/test builds and four salted trials per fixture. All 16 paired
output hashes and channel means are identical. Worker policy and sequential
rendering match; timings are seconds and uncertainties are sample SDs.

| Fixture | Master mean ± SD | Candidate mean ± SD | Paired change % mean ± SD |
|---|---:|---:|---:|
| RGB k1 | 0.947674541 ± 0.004218858 | 0.969262479 ± 0.004444865 | 2.280967 ± 0.908073 |
| RGB k2 | 0.724560063 ± 0.005722064 | 0.735456344 ± 0.006017120 | 1.508850 ± 1.186625 |
| NM | 1.713569437 ± 0.021601537 | 1.714899407 ± 0.009998128 | 0.093451 ± 1.725068 |
| HWSS | 5.733918198 ± 0.045851410 | 5.753667854 ± 0.021939967 | 0.347747 ± 0.621277 |

All paired timing changes lie within three sample SDs. The measured
positive changes are retained; these data establish neither zero overhead
nor a speedup. Evidence:
`.claude/logs/sms-phase2-round2-final/phase2-interleaved-cost.json`
and `cost-driver.log`; every A/B library/test build has a checked zero
exit and zero compiler diagnostics. Current-source restored controls pass:
**1168/0** composite policy, **3147/0** HWSS fallback, and **8200/0** full
domain replay. Earlier `8ad9a40cf` checkpoint measurements and controls
remain archived under `.claude/logs/sms-phase2-round2/`.
Its interrupted full gate is not substituted for current-source validation.

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
replacement gate under `.claude/logs/sms-phase2-round2-final/` passes
with verified native source hashes. Fresh Round 2 reviewers and the
Phase 2 merge remain pending.
Round 1's clean verdicts do not apply to the changed source.

## Fresh round 2 disposition (2026-10-04)

All three independent reviewers examined `8f9a63f692b7d042264d989ea3d6f0c5da9441fb`
after the complete replacement gate. Estimator review found one P1: arbitrary
interior-valued mesh atlas seams with smooth world-position tint can split one
physical root's reciprocal-probability family. Material review found one P2
that conflicts with the adopted design: native wrong-side refractor events
use geometric-normal fallback, whereas the extended constraint always uses
the shading normal. Cost/tests/document review found no P1/P2 and independently
reproduced the documented source hashes, checks and timing statistics.

At that review checkpoint, neither finding had a numerical reproduction or fix.
The native-law conflict triggered the user's explicit design-stop rule; its
proposal and static evidence are recorded in `SMS_EXTENDED_DESIGN.md`. The green gate remains evidence for
the tested checkpoint, not a clean review verdict. Phase 2 is unmerged, Phase 3
and Phase 4 have not started, and all target ledger rows remain open.

### Authorized round 2 repair checkpoint (2026-10-04)

The user approved the native fallback correction after the design stop.
The repair adds barycentric mesh-edge rejection for unaudited varying contexts,
including a non-indexed sibling query without changing native intersection
payloads. It preserves actual UV records and the audited constant-law control.
Native event directions now follow geometric-horizon reflection correction and
transmission correction with matching Fresnel/TIR, including coatings. Modified
surface constraints use actual arriving-ray/raster contexts and a numerical
Jacobian on a physical surface frame; numerical stencils that cross a native
branch are rejected. Legacy evaluations retain references to their original
vertices and create no added vertex copy. The original public constructor
signature and public SpecularInfo/ManifoldVertex records are retained.

The focused working-tree test passes **19945/0** after the repair. Its native
controls include RGB/NM, both windings and incidence sides, actual complete
proposals, valid shading events, world/ray/raster normal fields, coated and
polished siblings, transformed instances, closed start-inside and nested exits,
and independently solved endpoint displacements. Endpoint derivative checks
halve the displacement twice and retain their original 1e-5 band. The 64
coarse-displacement disagreements converge under those halvings; no band was
widened. Closed positives use an endpoint 0.02 times instance scale away from
the interface, avoiding the existing 0.01 minimum-segment boundary; a separate
0.005 segment remains a rejection control. An earlier fixture incorrectly
asserted AssignModifier's return value: it returns false after retaining the
modifier. The corrected test checks retained identity; those eight initial
failures are not production red evidence.

At this checkpoint, committed-source proofs and replacement gates were pending.
They subsequently completed as recorded above; fresh Round 3 review remains
pending. No row closes and no merge is claimed.

### Committed repair proof and displaced-chart sibling

The coherent ten-file proof at native `b61844191` completes with checked
zero-diagnostic library/test builds. Pre-fix `13a31c7d7` reports review
**6489/1968**, while its existing production control remains **177/0**.
Committed master reports **121/120** for available review configuration
controls and **127/50** for production; absent reference helpers are explicitly
unavailable, not numerical red evidence. Restored `b61844191` reports review
**19945/0**, production **177/0**, and geometry **4297/0**. Evidence is in
`.claude/logs/sms-phase2-round3-pre-audit/proofs/`.

The subsequent replacement pipeline was stopped during the cost stage when
the self-audit found that displaced geometry forwards native indexed-mesh
barycentric signals but the boundary guard checked only the outer geometry
type. It exits1 and is incomplete, not passing; completed timing samples
are historical partial evidence only. The cost driver restored HEAD sources.
Its logs and explicit abort summary are retained in
`.claude/logs/sms-phase2-round3-pre-audit/`.

A new displaced atlas witness was committed at `1b924f194` before repair.
It uses actual nonzero displacement, both windings/incidence sides, all three
RGB components and NM450/650, varying smooth world tint and constant-law
controls. The native checkpoint builds without diagnostics and reports
**19985/20**: all twenty failures accept uncertain variable-law atlas roots.
The sibling repair follows the actual native triangle provider in the hit
payload, preserving its barycentrics through wrappers. The focused repair
reports **20005/0** with checked zero-diagnostic library/test builds. The subsequent
committed proof and historical complete gate are recorded above. Round 3
subsequently found further defects; the current replacement review is pending.

### Committed sibling proof and historical strict-double gate stop

At native `761c71b51`, coherent pre-fix `1b924f194` reports review
**19985/20**, production **177/0**. Master reports available review
**121/120** and production **127/50**; reference helper absence is still
explicitly unavailable. Restored native `761c71b51` passes review **20005/0**,
production **177/0**, and geometry **4297/0**, with checked zero-diagnostic
library/test builds. Evidence: `.claude/logs/sms-phase2-round3/proofs/`.

The full replacement pipeline completes all eight alternating committed-source
A/B library/test builds and four salted render sets per variant, then exits1:
**0 of 16 cross-build off-mode hashes match**. Reported whole-image means
differ by at most 1.7763568394002505e-15; means alone do not establish
per-pixel agreement. HEAD sources are restored. No clean make/Xcode,
regression or sanitizer replacement gate is claimed. Evidence and failure
disposition are under `.claude/logs/sms-phase2-round3/`, including
`failed-summary.json` and `failed-stage-cost.json`.

Timing measurements from that failed stage remain reported honestly (n4,
paired percentage change mean and sample SD): RGB k1 **-0.243344 +/- 0.640274%**,
RGB k2 **-1.054483 +/- 0.827833%**, NM **+1.275654 +/- 0.382115%**,
HWSS **+0.517435 +/- 0.336988%**. These are completed timing observations,
not a passing phase gate or a zero-overhead claim. At that checkpoint the original internal-double cross-build contract remained
unchanged. The later user-adopted amendment is recorded below; the source is unmerged.

The completed pixel diagnostic rebuilds baseline and candidate coherently,
uses the same diagnostic test source and four salts, captures 4096 RGBA
pixels per image, and restores native sources and the original test afterward.
All library/test builds have zero diagnostics and all diagnostic renders pass.
The maximum absolute RGBA difference is **2.4579116519873878e-11**; the maximum
per-image RGBA RMSE is **2.0080155837659702e-13**. Alpha is bit-identical.
The largest positive ULP distance is **10528773**, so this is not an asserted
one-ULP bound. All sixteen images have **zero changed RGBA values after
float32 conversion**, and all twelve channel means pass the original
three-combined-sample-SD check (n4, both variants report their own SD).
This establishes the measured precision scale, not the compiler mechanism,
not bit identity of internal doubles. Complete
per-image/channel results: `off-mode-diagnostic/comparison.json`.

**Adopted acceptance amendment (user, 2026-10-04, “Proceed”):** use
bit-identical float32 image pixels for the cross-build shipped mode-off gate,
retaining strict internal-double on/off comparisons within the same build
for deterministic rejection cases. Retain salted n>=4 channel-separated
original three-SD comparisons where stochastic output differs. The failed
internal-double gate remains failed. The new complete gate above passes under
the adopted rule. Round 3 subsequently found further defects; current replacement
gates pass at44645f25e; fresh Round 4 review remains pending.
The phase remains on `sms-ext-phase2`; no ledger row closes and no new row
is opened. Master stays `08552560b54b517b7a8a0696eb9d317953b26bbe` and clean.


## Round 3 findings and current repair (2026-10-04)

Fresh estimator and material reviewers found implementation P1s in generated
UV seam handling and non-top exits of overlapping native solids. The fresh
read-only evidence reviewer reproduced the old full gate and cost claims,
and found a P2 coverage gap: modifier-aware numerical Jacobians had only
single-vertex tests. No reviewer identified a design defect. Agent-tool
capacity allowed two fresh reviewers; the third ran concurrently in a fresh
read-only ephemeral CLI session. No tracked state changed during review.

Generated UV overrides could bypass native seam guards while raw UV still
split smooth world-tint roots into separate reciprocal families. Actual
spherical/cylindrical generators and an interior-valued discontinuous custom
generator now have rejection witnesses; a continuous fixed-coordinate
world-tint control stays positive. At the d2508b4dc checkpoint, the repair probed four nearby actual
intersection contexts at one eighth of the physical root matching band,
rejecting generated UV variation beyond one eighth of the UV matching band.
It retains the original generated coordinates and performs no UV averaging.
This is a local numerical ambiguity diagnostic, not a certificate of global
continuity. Audited constant event laws without modifiers need no probe.
That checkpoint added four object intersections per applicable refreshed vertex,
only in active extended solves with authored generators and unaudited context
laws or modifiers. Mode-off execution does not enter this check.

At an overlapping closed-object exit, native SPFs use the exiting object's
queried index for direction, Fresnel and TIR, even when its identity is not
the stack top. Their consumer's radiance factor uses the incoming and outgoing
stack tops. The repair reproduces both native conventions separately; it
does not change native SPF or shared IOR-stack arithmetic. Actual overlapping
closed double-sided meshes cover both windings, transformed instances,
RGB/NM, perfect refractor, dielectric and AR-coated dielectric, transmission
and TIR, direct solved roots and complete visible proposals. Native SPF weights
and consumer radiance scaling are separate price oracles.

The initial committed witness at77fe155d5 reports25199/630. Ten cylinder
failures used the continuous side of its chart and are fixture failures,
excluded from production red evidence. Corrected committed tests at5d9776ce8
place that fixture at its actual positive-X seam. Repair d2508b4dc with these
expanded tests passes **28933/0**. The expanded test checks every diagonal and
off-diagonal block in modified R-T and R-T-R chains with arriving-ray-dependent
normals, plus independently solved physical endpoint shifts at h,h/2,h/4,
retaining the original 1e-5 band. Coherent pre-fix/master proofs and the complete replacement gate now pass at
44645f25e; fresh Round 4 review and integration remain pending.
Evidence: `.claude/logs/sms-phase2-round3-final/` witness/repair/expanded logs.

## Post-modifier UV sibling audit (2026-10-04)

The Round 4 preparation pipeline at34dda05c4 completed coherent source proofs
and eight interleaved source builds, but was deliberately stopped before its
clean make/Xcode stage. It is a partial checkpoint, not a passing full gate.
Evidence: `.claude/logs/sms-phase2-round4-final/`; the cost rows remain valid
for that earlier source only. No fresh external Round 4 review ran there.

The native interior-quad witness at54b2e58e2 against d2508b4dc reports
**131405/40**. All forty discontinuous-modifier cases accepted both .25/.75
UV families at one physical root, across both mesh windings, both incidence
sides, transformed instances and RGB/NM domains. Its modifier changes actual
UVs from true object-local position while leaving normals unchanged; the smooth
world-tint price does not justify separate physical roots. Continuous UV
modifier controls remain a required positive test.

The first repair atcc85a9bad rejected the jumps but also rejected steep
continuous charts (**71221/1840**); that failed run is retained, not a green
claim. The revised c5f66dd80 check applies the native modifier exactly once
per intersection context and compares actual full/half-displacement UVs.
The midpoint second difference cancels a smooth chart's first-order slope,
while a finite jump remains. The probe displacement and numerical reserve use
the existing physical/UV matching bands, with no band widening. This remains
a local diagnostic rather than a global regularity certificate. It adds eight
actual object intersections, and their native modifier calls where present,
per applicable refreshed vertex in active extended solves. Audited constant
laws without modifiers still skip generated-chart probes. Mode-off does not
enter the check. `.claude/logs/sms-phase2-round4-final-v2/` is the pending
replacement proof/full-gate location.

The midpoint repair passes **90485/0** focused checks, including all existing
modified R-T/R-T-R Jacobian and native horizon controls. Build and run evidence:
`.claude/logs/sms-phase2-round4-final/post-modifier-midpoint-{library,test,green}.log`.

## Matched normal-context sibling (2026-10-04)

The v2 preparation completed coherent ten-path proofs: pre2509de0fa review
**130595/850**, isolated pre34dda05c4 modifier review **131405/40**, master
available review **121/120** and production **127/50**, then restored review
**90485/0**, production **177/0**, geometry **4297/0**. Every build exited0
with zero diagnostics. Its pipeline was deliberately stopped before cost or
clean builds for the remaining matched-context audit; it is not a full gate.
Evidence: `.claude/logs/sms-phase2-round4-final-v2/proofs/results.json`.

A native mirror with discontinuous shading normals supplies a second concrete
counterexample. Both normal records fall back to the same native geometric
reflection direction and smooth event price, but raw-normal root matching
splits their reciprocal families. Committed test32241b5eb againstc5f66dd80 is
**246025/40** in `--modifier-chart-only`. All forty discontinuous-normal cases
accept both families, with explicit native SPF endpoint and event-price oracles;
continuous tilted-normal modifiers stay positive. Both windings, incidence
sides, transformed instances and five RGB/NM domains are tested.

Repair5d7b6ff38 uses the same eight post-modifier context probes to check UV,
shading/geometric normals and object position against their matching bands,
cancelling smooth first-order variation at full/half displacement. Object
position retains the relative scale used by root matching. It rejects
nonfinite contexts before final acceptance and preserves all actual native
records. No extra intersections are added beyond the UV midpoint repair.
The full focused review suite passes **193037/0**. This remains a local
numerical ambiguity diagnostic, not certified isolation or continuity.
Evidence: `.claude/logs/sms-phase2-round4-final-v2/context-sibling-*.log`.
The passing complete replacement is under
`.claude/logs/sms-phase2-round4-final-v3/`, followed by fresh external Round 4.


## Fresh Round 4 findings and replacement preparation (2026-10-04)

Fresh independent estimator, material and evidence reviewers examined
936952503 and reported seven implementation P1s, with no design defect.
The completed v3 gate above is historical evidence for that tree; it does not
validate the subsequent repairs. Round 5 is the final allowed review round
and has not been launched. Replacement evidence is being collected under
`.claude/logs/sms-phase2-round5-final/`.

Native committed witnesses reproduce steep continuous UV family splitting,
fixed-step normal-Jacobian aliasing, Polished back-face coat-index reversal
and empty native support, discontinuous authored corner-normal family splitting,
and closed-mesh start-inside misclassification with opposing corner normals.
The first combined witness is 81 passed/64 failed at140bb3995. The original
thin translated close-patch review example fails before root acceptance in
native acceleration, so it is not a valid accepted-root red proof. Corrected
wide, disjoint, double-sided native indexed patches at world x=1e6 preserve
both native roots: testc7a04dbf4 yields143/10 againstb28b2efe1, with both
accepted roots separated by7.68341124058e-9 but incorrectly matched.
These failed and partial runs remain under the v3 evidence directory.

Repairs preserve native records and the mode-off geometry/intersection law.
Polished eligibility enforces native positive incoming shading cosine and
uses ambient-to-coat indices for reflection on both sheet sides. Prepared
extended scenes audit mesh geometric orientation against corner normals once;
uncertain orientation makes anchors ineligible rather than seeding an empty
stack. Constant-law mesh edges now receive the same context probes as
modified charts. Measured context slopes constrain final root uncertainty,
and deterministic polishing resolves smooth steep charts before acceptance.
Modified Jacobians compare three progressively refined scales and can halve
the base step up to16 times; unresolved derivatives are rejected. Positional
root equality uses the joint final-correction/coordinate-roundoff uncertainty,
with the existing endpoint-scale band as a ceiling. These remain numerical
diagnostics, not certified root isolation or continuity bounds.

The focused gate atb28b2efe1 passes145/0 and the prior review suite passes
193037/0; subsequent tests add four-salt finite two-seed reciprocal accounting
for one and two physical roots, a second translation scale, and all five
RGB/NM continuous-normal derivative domains. Their current gate is pending.
The two-seed accounting uses actual accepted native roots with a finite toy
proposal law; it is not a production render or a measured production density.
The scene-preparation audit adds one-time mesh traversal and temporary vectors,
and adaptive modified Jacobians add opt-in solve work. Current cost, memory,
complete regression and actual Xcode evidence must be regenerated before merge.
No ledger row is opened or closed by this preparation.


The first Round 5 preparation completed coherent proofs at5e4e5beb0:
pre936952503 focused117/122 and review193153/122, production177/0;
master available review121/120 and production127/50; restored focused239/0,
review193275/0, production177/0 and geometry4297/0. All builds exited0 with
zero diagnostics. It deliberately stopped before cost/clean gates for a
native harmonic audit, so it is not a completed gate or an external round.

Testd63695cab confirms a continuous-normal period equal to the finest
halved Jacobian probe step still aliases all three scales:273/10 in
`--r4-only`, in both windings and all five RGB/NM domains. The derivative
remains−0.324324 versus independently refined−0.524323. Repair71c080bd3
adds a noncommensurate fourth scale (the finest halved step divided by sqrt2)
and compares its native branch and derivative as well, retaining bounded
base-step refinement and the existing convergence reserve. The focused
mode passes283/0; the expanded review passes193319/0. No continuity
certificate is claimed. Complete replacement evidence is now under
`.claude/logs/sms-phase2-round5-final-v2/`; the earlier preparation is preserved.


## Fresh Round 5: stop at the review limit (2026-10-04)

Three fresh independent read-only reviewers examined committed
`f3ebf3b926505a361636bb8dc93a8619f35fa9b7`. All reviewer processes exited0;
review convergence failed: estimator reports one P1, material reports two
P1s, and evidence reports two P1s plus two P2s. No reviewer identifies a
separate design defect. Raw verdicts, CLI logs and reviewed-head metadata are
under `.claude/logs/sms-phase2-round5-final-v3/fresh-reviews/`.

The source confirms the evidence P1s: `ObjectManager::PrepareForRendering`
tessellates/scans eligible meshes on every preparation, including mode-off,
repeated frames and shared instances. Earlier “one-time” wording means
preparation work rather than per-anchor work, but understates its repetitions
and allocations. `ProposeExtendedRoot`/`SolveDomainCore` and adaptive Jacobians
allocate fresh vectors; the adopted worker-local scratch storage is absent.
Required ray-intersection, material-query and peak-scratch counters are also
absent. Whole-process RSS and proposal counters do not satisfy those gates.

The material reviewer traces two additional defects: the fixed0.05 native
frame probe can replace a lower parallel patch root with the nearer upper
patch of the same object, eliminating proposal support; native event replay
reads modified `vNormal` while native SPFs use modified `onb.w()`, so an
ONB-only modifier can produce mismatched directions/prices and frame identity.
These source traces are supported by the corresponding implementation and
native SPF consumers, but new native C++ witnesses have not been executed.

The estimator reviewer numerically evaluates a near-commensurate continuous
normal field with period cbrt(epsilon)/(4*114243): the four production stencils
agree near−0.29166666667, while a P/1024 reference gives−0.49166541168,
with an estimated68.6% inverse-Jacobian overprice. This is a numerical
native-formula counterexample, not a newly compiled C++ fixture or render.
It needs committed native reproduction before repair.

The two P2s are an undisclosed non-indexed triangle scan in
`TriangleMeshGeometry::NativeTriangleEdgeDistance`, and weak harmonic test
guards: rejected solves skip derivative assertions and nonfinite Jacobians
explicitly pass. Current logs show positive finite controls, but the guards
can false-green under a rejection/NaN fault and need strengthening.

The green measured gate above remains valid evidence, not proof that these
review findings are absent. No production repair, sixth review, ledger closure,
new row, Phase 2 merge, Phase 3/4 work or push follows. Master remains
`08552560b54b517b7a8a0696eb9d317953b26bbe`, clean. Phase 1 remains merged at
`34bd520ec5e70a5cf96bcf8b8154b1a17888880f`. Continuation requires a user ruling
on the explicit stop when the review loop has not converged after five rounds.


## Resumed Round 5 repair witnesses (2026-10-04)

Evidence under `.claude/logs/sms-phase2-round6/` remains isolated repair
work, not a sixth external review or a full gate. Tests committed at
`66772ac50` against the then-current native implementation report
**18115 passing / 2024 failing** checks in `native-witness-red.log`.
That fixture's first normal-alias witness was on a diagonal seam and
therefore rejected; it did not demonstrate an accepted aliased Jacobian.
`36de2c28c` moves the normal witness into the triangle interior without
changing its field or derivative oracle. `interior-normal-red.log` reports
**35/10**, reproducing the accepted derivative mismatch in all five domains
and both windings.

The material/frame repair at `e81612ee4`, with an indentation-only warning
fix, reports **35959/50** in `frame-r5-result.log`. Forty remaining failures
concern stacked-sheet projection/support; ten are the unchanged Jacobian
alias witness. This result verifies substantial improvement in native ONB
replay but does not complete either P1. The initial library build had one
misleading-indentation warning; `frame-library-build-fixed.log` and the
successful fresh `frame-reference-build.log` have no diagnostics.

The next stacked-sheet repair compares both projection rays before choosing
the closest eligible surface. Its direct-solve fixture seeds the actual
traced native point, undoing Object's documented launch backoff, rather
than assuming the parsed decimal `.01` is represented exactly. Positive
proposal-support and same-visible-sheet assertions are unchanged.
The modifier differential proposal in SMS_EXTENDED_DESIGN.md is pending a
user ruling; no new supported-domain policy is implemented.

The nearest-frame implementation's successful fresh library and test builds
(`nearest-frame-library-build.log`, `nearest-frame-reference-build-final.log`)
have no compiler diagnostics. `nearest-frame-r5-result.log` reports
**36067/10**: all stacked-sheet support/provenance and ONB-only / vNormal-only
native material checks pass; the ten alias derivative checks still fail.
Two intermediate test builds failed due to the imported Object class
colliding with the scene-text helper name and then its member-name repair;
both failed builds are preserved. No stale binary was run after them.
The fresh passing build precedes this run. This isolated result is not a
claim of a full green phase or external review convergence.

The broader geometry mode then reports **3957/20** in
`nearest-frame-geometry-result.log`: the new full-frame continuity check
incorrectly rejects twenty nested start-inside roots at mesh UV diagonals.
The subsequent correction restricts tangent-orientation continuity to
contexts that use it; audited constant isotropic events still check W and
both normal records. `nearest-frame-r4-result.log` reports **341/10**,
with only the known alias witness failing. These failed checks are retained
as regressions, not omitted from the repair record.

On committed repair `6ca64acac`, fresh incremental library/test builds
(`audited-frame-match-library-build.log`,
`audited-frame-match-reference-build.log`) exit zero with no compiler
diagnostics. The restored geometry mode passes **4297/0** in
`audited-frame-match-geometry-result.log`; Round 5 isolated witnesses report
**36067/10** in `audited-frame-match-r5-result.log`, with only the ten
near-commensurate Jacobian checks failing. This verifies the nested
start-inside regression repair while preserving native modified-frame and
stacked-sheet positives. Full committed-source rollback proofs, clean make,
actual Xcode, sanitizer, regression, cost and fresh review gates remain
required after all Round 5 repairs are complete. Preparation-audit caching,
non-indexed edge-query cost, worker-local scratch and work counters remain
outstanding. The user subsequently adopted the derivative contract; its implementation evidence follows below.
Main remains clean at `08552560b54b517b7a8a0696eb9d317953b26bbe`;
no Phase 2 merge, push, new ledger row or ledger closure occurred.


## Adopted differential contract and preparation repair (2026-10-04)

The user adopted the audited differential contract at `969dd5b77`.
`268ef013d` adds a separate optional modifier capability in the existing
header, preserving the original IRayIntersectionModifier vtable. Providers
supply analytic directional derivatives for both possible native frame
consumers. Native constraints and emitter-endpoint Jacobians apply the chain
rule. Four-scale checks now concern raw native geometry/input transport,
not black-box modified-field derivatives. Uncertified caster modifiers make
the anchor ineligible; proposal and direct-solve entry points also reject
unaudited records. The initial shipped modifiers expose no certificate and
remain ineligible. Audited continuous fixtures remain positive.

Fresh library and reference-test builds exit zero with no diagnostics
(`differential-library-build.log`, `differential-reference-build.log`).
Committed native controls pass **36077/0** (`differential-r5-result.log`),
**4297/0** geometry, **761/0** unsupported/configuration, and **193401/0**
review controls. The alias witness retains positive solves in all domains
and windings, with its actual analytic derivative used in the Jacobian.
The intended uncertified identity-modifier control did not execute: fresh
Round6 review found that the kind4 assignment was unreachable in a loop over
kinds0–3, and the assignment also preceded final preparation. The claimed
after-preparation modifier coverage is withdrawn; the recorded unsupported
counts cover the actually executed kinds0–3 only. A reachable post-preparation
control across RGB/two NM domains, both windings and mixed casters is required.

The shared-mesh preparation witness is committed at `e46744cb7` and reports
**15/6** on pre-cache native sources (`preparation-witness-red.log`).
`167f37d98` folds intrinsic orientation auditing into the existing area pass
at finalization and actual mutation. Preparation reads the result without
four-array tessellation or repeated triangle traversal; shared instances
reuse it and displaced geometry forwards its realized mesh. Fresh builds
exit zero with no diagnostics (`preparation-cache-final-library-build.log`,
`preparation-cache-reference-build.log`). The expanded diagnostics/mutation
control passes **31/0** (`preparation-cache-green.log`). It verifies no new
audits or triangle visits across four preparations of shared indexed and
non-indexed meshes, and exactly one replacement audit on indexed normal
mutation. The area pass performs additional orientation dot products during
construction/mutation; no whole-render cost claim is made before the full
interleaved cost gate. An initial test compile failed on the scene Geometry
helper/native Geometry class name collision; the failed log is retained,
then the successful build preceded the numerical red run.

The non-indexed primitive provenance witness is committed at `7f33ceecf`;
its build/run and repair are recorded below. These isolated controls do not replace full rollback,
clean make, actual Xcode, sanitizer, regression, cost or fresh review gates.
No Phase 2 merge, push, ledger closure or new ledger row is claimed.


## Native edge provenance and worker scratch repair (2026-10-04)

The committed provenance witness reports **33/8** before the fix
(`provenance-witness-red.log`). `e3ca99663` stamps actual primitive and
barycentric hit provenance in the native non-indexed closest-hit kernel.
No shading signal provider is advertised. Edge classification now reads
that hit in constant time instead of scanning every triangle. Fresh builds
have no diagnostics, and indexed/non-indexed, both-winding, 1/1024-triangle
controls pass **41/0** (`provenance-green.log`); geometry and review controls
pass **4297/0** and **193401/0** (`cache-provenance-*.log`).

The scratch repair retains one live and one retry root in the production
trial scope and uses nested worker-local leases for native Newton, Jacobian,
residual, factorization and replay buffers. Buffers reserve the configured
maximum depth, retain capacity between trials, and clear borrowed hit and
medium records at scope exit. Public proposal results retain their own
storage. Legacy evaluation uses its existing buffers and does not lease
extended scratch. This is buffer reuse, not a claim that all native
metadata/IOR-stack operations allocate nothing.

Actual Object/CSG closest-hit entry points and ObjectManager queries count
object and scene intersection calls during extended proposal/solve scopes,
including hidden medium probes within those scopes. These are separate
query categories. `materialQueries` counts logical SMSDomainReplay material
queries, not every underlying painter read or every native SPF/BSDF call.
Eligibility before the estimator scope is excluded. `scratchPeakBytes`
reports retained leased vector capacities plus frame/root records and frame
pointer capacity; it excludes IOR-stack auxiliary storage, caller-owned
results, general metadata and whole-process memory. The full process peak
is a separate memory gate. Counters remain optional, owned by the caller.

Incremental library/test builds exit zero without warnings
(`scratch-counters-library-build.log`, `scratch-reference-build-fixed.log`).
The initial test command used an invalid make target and exited before
compilation; its log is retained and the corrected target succeeded before
any tests ran. Focused controls pass **31/0** preparation, **41/0** provenance,
**36077/0** Round 5, **4297/0** geometry, **193401/0** review and **369/0**
production (`scratch-*-result.log`). Production uses four salted renders,
16384 samples per salt, point/spot, reflection/dielectric, both windings,
RGB and two NM domains. Buffer growth and frame creation stop after the
first salt. Its first fixture reports 28 buffer growths, six frames and
201376 retained scratch bytes; later fixtures reuse that capacity. The subsequent legacy control passes **393/0** production checks
(`scratch-final-production-result.log`), confirming that legacy evaluations
lease no extended scratch; final preparation/provenance remain **31/0** and
**41/0**. Compatibility scaffolding lets committed older native
headers compile these numerical witnesses; it does not certify old solvers.

All paths above refer to `.claude/logs/sms-phase2-round6/`. These focused
checks precede the fresh complete rollback, default-path interleaved cost,
clean make, actual Xcode, expanded sanitizer, full regressions and review
round. No current full gate, Phase 2 merge, push or ledger change is claimed.


The first fresh rollback attempt uses all **19** changed native source/header
paths coherently. Pre-differential `6ca64acac` builds successfully and reports
**36067/10**, reproducing the intended alias failures. The following
pre-scratch `e3ca99663` test build fails because its interface already exists
but its later feature macro does not: the test shim redeclares the types.
This compile failure is **not** a red proof. The driver restores committed
native sources automatically; restored library and test builds exit zero
without diagnostics. The test shim now detects complete interface types
and selects a separate fallback only when the native capability is absent.
Failed logs are preserved under
`.claude/logs/sms-phase2-round6-final/attempt1-proofs/`. Full gates restart on
the committed shim repair; the compile failure is not bypassed or counted.


## Fresh Round 6 gate: precision stop (2026-10-04)

Candidate native/test commit: `cf76d5d0a675091529a6a288efc0de5c477b11d5`
on `sms-ext-phase2`; baseline master remains clean at
`08552560b54b517b7a8a0696eb9d317953b26bbe`. All evidence below lives in
`.claude/logs/sms-phase2-round6-final/`. No merge or push occurred.

Coherent rollback restores all **19** related source/header paths together.
Current tests compile without warnings on every completed checkpoint.
Pre-differential `6ca64acac` reports **36067/10** Round 5, **23/8** preparation,
and **33/8** provenance. Pre-scratch `e3ca99663` reports **177/24** production:
its radiance checks pass and the 24 failures are missing scratch/query
measurement capability, not physical estimator failures. Master production
reports **127/50**, including real analytic radiance failures. Restored
candidate controls pass **36077/0**, **31/0**, **41/0**, **193401/0**, **393/0**,
and **4297/0** for Round 5, preparation, provenance, review, production and
geometry. The 40 salted production means are bit-identical before and after
the scratch refactor (`scratch-production-means.json`). Record sizes are
ManifoldVertex336, SMSDomainVertex1592, SMSDomainRoot184 and
SMSReferenceCounters368 bytes; the prior counter record is320 bytes.

Eight interleaved actual library/test builds and all sixteen mode-off
renders complete with no compiler diagnostics. Worker policy is one forced
worker and reserve_count0, identical for A/B. The original float32 identity
gate reports **15 passed / 1 failed** (`failed-mode-off-summary.json`).
Only fixture2, salt/trial1, NM
`scenes/Tests/Spectral/spectral_dispersive_caustic_pt_sms_uniform.RISEscene`
has a mismatching float32 hash. Its reported mean is identical:
1.5160162795669927. The pipeline stops here and restores committed sources;
clean make, actual Xcode, full regression, sanitizer and process-memory
stages are **not run** on this candidate. Fresh Round6 reviews are not
launched because their full gate has not passed.

Paired default cost differences, mean percent ± sample SD (n4):

| Fixture | Candidate versus master |
|---|---:|
| RGB, k1 | +1.060071% ±0.711975% |
| RGB, k2 | +0.069361% ±0.595337% |
| NM | +1.040116% ±1.003496% |
| HWSS | +1.185431% ±0.690732% |

Exact master and candidate binaries are preserved. A debugger captures their
native pixel buffers at HashPixels without recompilation. Both binaries
were disassembled to verify LTO's begin/end register arguments; empty-vector
calls are excluded. Earlier diagnostic callback failures are retained in
attempt1/attempt2 logs and are not passing captures. Successful captures
reproduce **both recorded double and float32 hashes** for all four fixtures
at salt1 (`exact-native-pixel-differences.json`). The one changed float32
component is R at `(38,59)`:

- master double: 8.735136418730979e-7;
- candidate double: 8.735136414088404e-7;
- double difference: 4.642574878406611e-16;
- master float32: 8.735136702853197e-7;
- candidate float32: 8.735136134419008e-7;
- float32 difference: 5.684341886080802e-14, exactly one ULP.

The NM image's maximum double RGBA difference is1.4053203045705231e-12 and
RMSE1.2280086430428958e-14. Its float32 mismatch is one component of one
pixel; the other three captured images have no float32 mismatches. This
quantifies a small rounding-boundary difference but does not satisfy the
adopted bit-identity contract or establish its exact compiler/code cause.
The strict gate stays **failed**. No row is opened or closed, no precision
exception is adopted, and no Phase2 completion or fresh-review convergence
is claimed. The original user's STOP rule and the adopted precision contract
require a ruling before relaxing that contract; the next proposed choice is
a cross-build one-ULP float32 bound versus continued investigation under
exact float32 identity. Within-build deterministic and stochastic contracts
would remain unchanged under either choice.


## Approved one-ULP cross-build policy (2026-10-04)

The user approved the proposed maximum one-float32-ULP cross-build RGBA
bound. Same-build deterministic rejection remains exact in doubles, and
stochastic bands are unchanged. SMSLegacyModeTest now writes optional raw
float32 reference buffers and compares every component against a matching
master build. Exact hashes remain diagnostics. A representation-based
ordered ULP metric handles negative values and signed zero, and rejects
NaN/Inf under fast-math. Unit controls include zero/one/two steps, negative
values and nonfinite inputs. The earlier strict gate remains failed.
Fresh comparison/build/regression/sanitizer/review gates follow this adopted
policy; no merge, closure or fresh-review convergence is claimed here.


## One-ULP gate attempt and sanitizer repair (2026-10-04)

Evidence: `.claude/logs/sms-phase2-round6-final-v2/`. The source/test
checkpoint starts at `78152901e`. Eight alternating source/test builds and
sixteen shipped mode-off renders pass the approved per-component bound:
fifteen float32 images are identical, and NM salt1 changes one component by
one ULP. No nonfinite pixels pass. Same-binary dump/reference controls give
18/0 then22/0; injecting a two-ULP reference difference gives21/1 and exit1,
then the original reference bytes are restored. Timing excludes pixel-dump
and comparison I/O. Paired render-time differences, mean percent ± sample SD
(n4, identical one-worker/reserve_count0 policy): RGBk1 +1.098145 ±1.041775,
RGBk2 +1.394356 ±0.533959, NM +2.045421 ±1.143960 and
HWSS +2.040712 ±1.144034. No zero-overhead claim is made.

Clean make compiles376 units, and actual Deployment/Opto each compile393,
with zero owned diagnostics. The regression pipeline initially stops at
SourceHygieneTest168/1: native nonindexed primitive provenance is absent
from its writer census. Repair `5c0573137` permits that writer and adds a
field allowlist restricted to pProvider/primId/baryA/baryB. Rebuilt committed
master test reproduces168/1; restored test passes170/0. A temporary injected
pScene write fails the field guard and is restored in finally. All native
and previously completed test hashes are unchanged, so the resumed test
pipeline composes the completed prefix with fresh remaining modes, recording
33 make modes /411404 reported checks /0 failures and twelve actual
Xcode-linked controls /0 failures. CST separately has458 MATCH /0 DRIFT,
465 corpus scenes /0 UNCOVERED /0 STALE. The initial hygiene failure stays
in `results-before-hygiene.json` and `SourceHygieneTest-failed-run.log`.

The partial sanitizer build (test plus nine changed native units) stops on
an indexed-mesh vector container-overflow report. Rebuilding all374 linked
project C++/Objective-C++ units with ASan/UBSan, without disabling any
checks or changing native source, passes geometry4297/0, unsupported761/0,
production393/0 and signed641/0. This resolves the mixed-instrumentation
report in this workload; the failed partial run is preserved. Third-party
libraries remain uninstrumented. The full-instrumentation review mode then
finds a real signed overflow in WeldVertexPositions's cell-key products at
large translated coordinates. Therefore this attempt is **not a passing
full gate**; memory and fresh reviewers have not run.

Repair `7cb140469` makes the hash products and map keys uint64_t, with
explicit modulo2^64 arithmetic. Cell coordinates, bucket candidate distance
tests and weld tolerance are unchanged. The hash-constant sibling sweep
finds no other copy. A rebuilt fully instrumented committed pre-fix
`5c0573137` checkpoint reproduces UBSan overflow; restored repair passes
193401/0 with no sanitizer findings (`hash-focused-red-driver.log`,
`hash-focused-green-driver.log`, `sanitizer-full/pre-fix-*`). This proof
recompiles the changed unit and relinks with all other project units
instrumented; it does not reuse an old binary as a rebuild. A complete fresh
native/test gate is prepared in `.claude/logs/sms-phase2-round6-final-v3/`.
Phase2 remains unmerged, with no new ledger row or closure claimed.


## Fresh replacement gate on the repaired native tree (2026-10-04)

Native/test/document checkpoint `746fa42c2` on `sms-ext-phase2`; baseline
master `08552560b` is unchanged and clean. Evidence lives under
`.claude/logs/sms-phase2-round6-final-v3/`. Source hashes verify all19 changed
native paths and four changed test sources. The complete pipeline and summary
both exit0, with all sources restored and a clean implementation worktree.
This is a fresh gate, rather than reuse of the failed v2 sanitizer run.

| Gate | Result |
|---|---|
| Coherent committed-source proofs | pre-differential36067/10, preparation23/8, provenance33/8; pre-scratch177/24 (missing measurement capability, radiance passes); master production127/50; restored36077/0,31/0,41/0,193401/0,393/0,4297/0 |
| Clean make |376 compilation units, zero diagnostics |
| Actual Xcode Deployment / Opto |393 compilation units each, zero owned diagnostics; only the documented missing OIDN search-path and AppIntents metadata notices excluded |
| Make regressions |20 executables /33 modes /411404 reported checks /0 failures; CST458 MATCH /0 DRIFT,465 corpus scenes /0 UNCOVERED /0 STALE separately |
| Actual Xcode-linked controls |12 runs /399772 reported checks /0 failures;372 Xcode project objects plus Profiling utility compiled with the matching response flags |
| Full-project ASan/UBSan |374 linked project C++/Objective-C++ units instrumented, eight modes /235642 reported checks /0 failures; third-party libraries uninstrumented |
| Process memory |production393/0; maximum RSS102612992 bytes; peak footprint82346536 bytes;27.77s real; no swaps |
| Scratch output invariant |40 salted production means bit-identical before/after scratch storage refactor (maximum0 double ULP); process memory is separate from scratch capacity |
| Shipped mode-off pixels |eight alternating source/test builds,16 renders, all within the adopted one-ULP bound;15 float32 images identical, NM salt1 has one changed component /one ULP |

Current paired render-time changes, mean percent ± sample SD (n4), with
identical one-worker/reserve_count0 policy and dump/compare I/O excluded:
RGBk1 +0.061317 ±0.577191; RGBk2 +0.050677 ±0.976868;
NM +1.759001 ±0.918555; HWSS +1.616429 ±1.027699.
The exact hashes and separate A/B timing means/SDs remain in
`phase2-interleaved-cost.json`; no zero-overhead claim is made.

All12 narrow-cone, both-winding channel comparisons pass their original
three-combined-SD band, with each reference's own SD. Five of six wide-cone
channel measurements also lie inside that original band; front winding/red
is extended0.6289669133101606 ±0.00019614427847928844 versus
BDPT0.6300625785345996 ±0.00008359126631271482, −0.17389784% and
−5.13881272 combined SDs. This is the already user-accepted variance
measurement, disclosed without changing its band; it supplies no DL-420
closure. `final-gate/slab-summary.json` records all18 comparisons.

Production counters include131605 proposals,125049 zero trials,692802
Newton iterations,66069 rediscovery trials,1957 tail events,44 roulette
terminations,3300 accepted/standalone-owned roots and125049 rejections.
Logical query counts are1329162 scene,3986397 object and677693 domain
material queries, under the documented scope. Warmed scratch capacity is
201376 bytes with no additional growth or frame creation in this fixture;
its allocator/frame/context exclusions remain documented above. Record sizes
are336-byte ManifoldVertex,1592-byte SMSDomainVertex,184-byte SMSDomainRoot
and368-byte SMSReferenceCounters.

The earlier failures and rebuilt repair proofs remain in the v2 directory.
Fresh independent review is next; no convergence, Phase2 merge, push, new
ledger row or closure is claimed by this passing gate alone.

## Adopted composed-input correction and Round 6 repairs (2026-10-05)

User ruling adopted in `3925ad599`: UV-dependent audited modifiers also require an audited generated-UV differential; unaudited composition makes the anchor ineligible, coupling all three switches and retaining PT. Audited UV-independent modifiers remain eligible. `ISMSUVDifferential` is a separate optional capability; `IUVGenerator` and `IRayIntersectionModifier` legacy vtables are unchanged. `ISMSModifierDifferential::SMSFrameDependsOnUV` is optional and appended with conservative default true. Object exposes its borrowed generator through nonvirtual `SMSUVGenerator`.

Self-audit `87c4e5421` corrects generated-UV normal input transport: native Object calls GenerateUV with object-space point and normal. The differential transforms the world geometric normal by the transpose of the final object transform, normalizes it, and transports its derivative. Both windings and incidences under nonuniform scale and rotation now exercise all RGB components and NM450/NM650, including independent constraint and endpoint Jacobians. Uncertified harmonic UV composition is rejected; audited harmonic composition and audited UV-independent normals remain positive.

Round 6 implementation repairs also restore pch.h as the first include in Object.cpp/CSGObject.cpp and add mechanical hygiene controls. Weld cell coordinates are checked for finite/int64-safe conversion; large translated meshes use the same distance and tolerance tests with a mesh-local origin when the absolute grid is unrepresentable. This adds bounded linear preparation passes, with no per-sample loop. Open and closed native meshes at zero and ±1e14 translation cover both windings. Post-preparation unsupported cases now actually execute unaudited modifier kind4 and generated-UV kind5, proving predicate/proposal rejection and exact-double on/off RGB/NM output under four salts.

Coherent proofs disclose their composition in `composition.json`: four completed rollback scenarios at 87c4e5421 are retained after test-only preprocessor-guard repair f5f04b1f5, with all20 native hashes unchanged. A-enabled witness logic is unchanged; the missing-feature master compile guard is fixed. The failed original master compilation and interrupted earlier own build remain preserved and are not numerical red proofs. Native-master and every restored green are rebuilt on current tests. Pre-transform gives281/60, pre-composed UV321/30, unsupported1005/76 and hygiene170/2; pre-differential36067/10, preparation23/8 and provenance33/8; pre-scratch177/24 fails missing diagnostic capability while its radiance checks pass. Current master production127/50 is a numerical red. Restored controls total235851/0 across ten modes. Fresh fully instrumented pre-range UBSan red/current green is recorded separately after the complete sanitizer gate.

No new ledger row, closure or Phase2 convergence is implied by these repair proofs. The completed gate and fresh review results follow below.

## Round 7 composed replacement gate (2026-10-05)

Native/test checkpoint `f5f04b1f5601dc9ff856e065ad9ae4cba559fb57`; master `08552560b54b517b7a8a0696eb9d317953b26bbe` unchanged. Evidence: `.claude/logs/sms-phase2-round7-final/`. The completed tree has the composed gate described below; rollback proof composition is explicitly disclosed above and in composition.json. Source hashes verify all20 native paths and four test sources.

The interruption stopped the original process during the slab mode;29 completed modes are retained on the unchanged20-native/4-test hashes. Six remaining modes use the same previously checked test binary; the slab is rerun in full, preserving the interrupted log. Clean make and both Xcode configurations are rebuilt after user completion of the updated toolchain license and first-launch package setup, with eight additional rebuilt native controls. Pre-interruption A/B evidence is retained on the identical source tree. This is an explicitly composed gate.

| Gate | Result |
|---|---|
| Clean make |376 compilation units /0 diagnostics |
| Actual Xcode Deployment / Opto |393 units each /0 owned diagnostics |
| Make regressions |20 executables /35 modes /412084 reported checks /0 failures; CST corpus reported separately |
| Actual Xcode-linked controls |16 runs /401128 checks /0 failures |
| Rebuilt native controls after toolchain update |8 runs /231382 checks /0 failures |
| Full ASan/UBSan including float-cast-overflow |374 linked project C++/Objective-C++ units,10 modes /236320 checks /0 failures; third-party libraries uninstrumented |
| Fully instrumented weld conversion proof |committed eca4f2ecc float-cast UB reproduced; restored weld37/0 and composed UV321/0 |
| Process memory |production393/0; maximum RSS103071744 bytes; peak footprint82870824 bytes |
| Scratch output invariant |40 salted production means, maximum0.0 double ULP vs pre-storage-refactor |
| Mode-off pixels |16 salted comparisons within one F32 ULP,15 images identical /1 NM image one component one ULP |

The first retry compiled Deployment successfully but the strict gate rejected two Xcode startup errors (CoreDevice/CoreSimulator package mismatch). Those logs remain preserved. After the user completed first-launch setup, the successful builds above were rerun from clean products. The intentional UBSan proof initially ended with SIGABRT under the updated runtime while its ignored harness expected exit1; the exact native float-cast diagnostic was preserved, sources were restored, and the repaired harness was rerun. It accepts exit1 or SIGABRT only with that exact diagnostic; restored weld37/0 and composedUV321/0 pass. No numerical band changed.

Paired render-time changes, percent mean ± sample SD, n4, one worker and reserve_count0: RGBk1 +1.514415 ±0.380165; RGBk2 +0.939036 ±0.438353; NM +2.169488 ±0.573695; HWSS +2.018847 ±0.537038. Separate A/B seconds/SDs and image hashes are in phase2-interleaved-cost.json; no zero-overhead claim.

All12 narrow-cone channel comparisons pass their original three-combined-SD bands. Wide-cone measurements outside that band: 1 of6, reported in slab-summary.json. The already accepted wide-cone discrepancy does not widen any band or supply DL-420 closure.

Fresh independent Round7 review is required next. Phase2 remains unmerged; no push, ledger closure or new row is claimed.

## Fresh Round 7 disposition and native witnesses (2026-10-05)

Review logs: `.claude/logs/sms-phase2-round7-final/fresh-reviews/`. Estimator P1: irrelevant oscillatory UVs split a physical mirror root into reciprocal families. Material P1: direct-object final refresh omits cross-object scene signals, mispricing supported spatial tint. Evidence P2: flat transformed generated-UV controls do not exercise normal derivatives. No DESIGN finding; no clean round.

Evidence under `.claude/logs/sms-phase2-round8/`: committed `66180cc62` UV witness build is checked and warning-free, with3 passing/20 failing checks and mean2.011962890625 versus the single-root analytic1. The initial committed `d49ad082c` signal witness has353/240, exposing missing final scene/self/world provenance and prices. Its native transmission oracle also lacked `SetCurrentObject`; that oracle is repaired and the corrected committed-source red/green is still required. The initial repaired run553/40 belongs to this oracle defect, not a new production finding. An initial curved-test build failed because the test oracle had not exposed a protected geometry projection helper; no stale test binary was used as a pass.

The UV repair passes23/0; curved generated-UV control passes425/0 in both incidences, untransformed and nonuniform-scale/rotation instances, all RGB domains and NM450/NM650. Its object-space `p.x*dn.z` term is measurably nonzero; the deliberate omission control produces Jacobian errors around0.0128–0.0519 while complete transport errors are below1e-8. Independently solved endpoint Jacobians are also checked. Native signal completion repairs and final signal controls are in progress. All new proofs/full gates/reviews remain pending for this changed tree. No merge, ledger row or closure is claimed.

### Round 7 focused repair greens (2026-10-05)

Checked warning-free focused builds pass UV-root23/0, scene-signal1665/0, curved-UV425/0 and signal point/spot production393/0. The final signal test starts from a different traced seed context and solves back to the independently priced native root; it checks current final world coordinates and scene/self provenance. Modifier/differential inputs use the same completion routine. Point/spot fixtures use an off-diagonal real indexed-mesh root: the first varying-price fixture landed on a conservatively rejected chart seam and its333/60 log is preserved, not counted as a pass. The analytic reference and original n4/3SD bands are unchanged. Corrected committed-source reds, full replacement gates and a fresh review are pending.

## Fresh Round 8 replacement gate (2026-10-05)

Native/test checkpoint `7c7248a4baca25673ec682a6095e65fa1368a853`; master `08552560b54b517b7a8a0696eb9d317953b26bbe`. Evidence: .claude/logs/sms-phase2-round8-final/, including source.json, proofs/results.json, final-gate/completed-summary.json, phase2-interleaved-cost.json and production-memory.log. All20 native and four test hashes verify. This gate reruns all six coherent committed rollback scenarios with current tests, all eight alternating A/B library/test builds and16 salted render comparisons, clean make and both actual Xcode configurations, the complete regression gate and full-project sanitizers. No Round7 result substitutes for this Round8 gate.

| Gate | Result |
|---|---|
| Clean make |376 compilation units /0 diagnostics |
| Actual Xcode Deployment / Opto |393 /393 units /0 owned diagnostics |
| Make regressions |20 executables /39 modes /414590 reported checks /0 failures; CST reported separately |
| Actual Xcode-linked controls |24 runs /406140 checks /0 failures |
| Full ASan/UBSan including float-cast-overflow |all374 linked project units,14 modes /238826 checks /0 failures; third-party libraries uninstrumented |
| Committed weld conversion proof |native float-cast UB reproduced, diagnostic-qualified exit1 or SIGABRT; restored weld37/0 and UV321/0 |
| Process memory |production393/0; maximum RSS102334464 bytes; peak footprint82100752 bytes |
| Scratch output invariant |40 salted production means; maximum0.0 double ULP vs pre-storage-refactor |
| Mode-off pixels |all16 salted comparisons within adopted one-float32-ULP bound |

Fresh committed-source reds: pre-r7-numerical-red: 3 passed, 20 failed; pre-r7-r7-signals-only-red: 945 passed, 400 failed; pre-r7-r7-signal-production-only-red: 353 passed, 40 failed; pre-transform-numerical-red: 281 passed, 60 failed; pre-composed-numerical-red: 321 passed, 30 failed; pre-composed-unsupported-red: 1005 passed, 76 failed; pre-composed-hygiene-red: (scanned 456 test files) 170 passed, 2 failed.; pre-differential-numerical-red: 36067 passed, 10 failed; pre-differential-r5-preparation-only-red: 23 passed, 8 failed; pre-differential-r5-provenance-only-red: 33 passed, 8 failed; pre-scratch-numerical-red: 177 passed, 24 failed; native-master-numerical-red: 127 passed, 50 failed. The first Round8 attempt stopped on a baseline test compilation error after completing five rollback scenarios; its logs are preserved under proofs-attempt1/. The unchanged quad-mesh helper was moved outside the optional-interface guard in7c7248a4b, then the entire pipeline was rerun. The failed build is not a numerical proof.

The native repair shares ObjectManager::CompleteShadingSignals with direct SMS final refresh, enclosing capture, modifier and differential inputs. It stamps current scene/self/world provenance while retaining native primitive payload. Irrelevant UV/frame comparisons follow the audited material and modifier input dependency; geometry/normal equivalence and finite contexts remain required. Curved generated-UV coverage exercises nonzero normal input derivatives and a deliberate omitted-term control.

Paired render-time changes, percent mean ± sample SD, n4, one worker, reserve_count0: RGBk1 +0.945183 ±1.511301; RGBk2 +0.739550 ±1.531106; NM +1.593759 ±1.061928; HWSS +1.901903 ±1.238740. Separate A/B times, SDs and pixel precision records remain in phase2-interleaved-cost.json. No zero-overhead claim. Original narrow-cone channelwise three-combined-SD bands and the previously accepted wide-cone measure-only treatment remain unchanged; no DL420 closure.

All12 narrow-cone channel comparisons pass the original three-combined-SD band;5 of6 wide-cone measurements fall within it. Outside-band measurement: winding0, component0, extended0.62896691331016064 ±0.00019614427847928844, BDPT0.63006257853459957 ±8.3591266312714821e-05, -0.17389784% /-5.13881272 combined SD. This is the user-accepted measure-only discrepancy; no band is widened. All16 mode-off comparisons pass;15 images are float32-identical, and one NM image has one component differing by one ULP.

Fresh independent Round8 review is next. Phase2 remains unmerged; no ledger closure, new row or push is claimed.

## Fresh Round 8 review disposition (2026-10-05)

All three fresh reviewers exited successfully on 1a1eaeedd. Material P1: Polished replay omits native vNormal normalization and its derivative chain rule. Estimator P1: both diffusion and random-walk SSS caller clamps can clip downstream reference A across shader dispatch. Evidence P2: factorization helper scratch buffers reserve current chain length rather than the documented configured maximum; the one-vertex warm-up control misses later longer systems. No DESIGN finding and no clean round. Evidence: .claude/logs/sms-phase2-round8-final/fresh-reviews/. The completed 7c7248a4b gate remains valid for that checkpoint; it is not a gate for subsequent repairs. Native witnesses, repairs, replacement gates and fresh review are in progress; Phase2 remains unmerged with no ledger closure or new row.

### Round 8 Polished native proof and focused repair (2026-10-05)

Committed native witness a7e05e412 reproduces849/160 on unchanged Round8 native hashes. Strengthened 39e10e782 adds spatial rotation as well as normal magnitude and again gives849/160. The first test build had API-call signature errors and is preserved; only the subsequent checked warning-free build was run. The normalization repair passes1009/0 with actual native SPF/replay/root-price comparisons and independently shifted constraint/endpoint Jacobians under both windings/incidences, RGB/NM and nonuniform-scale/rotation. It normalizes only the consumed Polished field and transports the derivative through normalization, retaining raw context fields and the other native consumers. Evidence: r8-polished-* logs under .claude/logs/sms-phase2-round8-final/ and polished-repair-* under .claude/logs/sms-phase2-round9/. This focused green does not replace the full gate or fresh review. SSS clamp and scratch-capacity findings remain pending; Phase2 is unmerged.

### Round 8 factorization capacity native proof and focused repair (2026-10-05)

Committed580370722 cold-worker witness gives81/28: warming a one-vertex block system leaves both helper buffers growing at each later k up to8, under unchanged configured maximum8 and the same native trial nesting depth. Both native LU helpers retain the independently known identity-block determinant and solution. The repair reserves the configured maximum (or current k if larger) and passes109/0, with warmed growth count4 unchanged through k8, on fresh workers for both indexed windings/incidences. Caller-owned synthetic matrix vectors are not part of the worker scratch capacity claim. Checked library/test builds have zero diagnostics. Evidence: scratch-witness-* and scratch-repair-* under .claude/logs/sms-phase2-round9/. Full replacement gates and fresh review remain required; the SSS clamp finding is pending.


### Round 8 SSS return-boundary repair (2026-10-05; replacement gate pending)

The committed native witness (899a36d51) dispatches diffusion and random-walk SSS continuations through an actual StandardShader/PathTracingShaderOp and RayCaster, from an existing-depth anchor, into indexed receivers/mirrors with both windings. The first witness exposed 46 clamp failures (443 passing checks); its initial attempt had a test-only borrowed shader-op lifetime error, corrected before the numerical red run. The strengthened witness uses four original salted paths, 512 samples each, checks positive recursive native returns, RGB and 450/650-nm reference prices, ordinary-only controls and actual HWSS handoffs. Its focused repair run passed 1065/0 with bit-identical reference means with/without indirect clamps, and bit-identical HWSS extended on/off means with both clamp settings. HWSS and ordinary-only returns retain their historical clamps.

RuntimeContext now carries scoped radiance provenance, set only when the PT integrator actually adds a nonzero reference-A contribution. Each SSS recursive cast starts a fresh provenance scope and propagates it outward on return. The opaque shader return combines ordinary and reference radiance: when it contains reference A, the whole combined SSS return is exempt from the caller's indirect clamp. This deliberately preserves the unclamped reference contract; it does not claim to selectively clamp an ordinary subterm inside that opaque return. Ordinary-only returns still clamp. This is contribution provenance, not the deferred medium-membership seed-certain design. The shared Pel/NM implementation covers both SSS models. HWSS's legacy mode scope prevents reference A in its nested NM returns. Standalone SMSShaderOp already forces legacy, so it needs no provenance change.

Logs: .claude/logs/sms-phase2-round9/sss-witness-red2.log, sss-repair-green2.log. The focused run precedes the final nonzero-provenance guard rebuild; fresh full-gate evidence must replace it before integration. Round 8's completed gate remains historical evidence for its own frozen source, not evidence for these new repairs. No ledger row is closed.


### Fresh Round 9 replacement gate (2026-10-05)

Native/test checkpoint 4495ae81be81107fd604bfad0faedfd46a6fe846; master 08552560b54b517b7a8a0696eb9d317953b26bbe. Evidence: .claude/logs/sms-phase2-round9-final/source.json and final-gate/completed-summary.json. All 21 native and four test hashes verify. This complete replacement gate reruns nine coherent committed-source rollback scenarios, eight alternating A/B library/test builds and 16 salted mode-off comparisons, clean make and both actual Xcode builds, all required regressions, full-project sanitizer checks and process memory. No earlier gate substitutes for this frozen source.

| Gate | Result |
|---|---|
| Clean make | 376 compilation actions; 0 owned diagnostics |
| Actual Xcode Deployment/Opto | 393/393 compilation actions; 0 owned diagnostics |
| Make regressions | 20 executables / 42 modes / 416773 reported checks / 0 failures; CST separately |
| Actual Xcode-linked controls | 30 modes / 410506 checks / 0 failures |
| Full ASan/UBSan/float-cast-overflow |all 374 linked project units / 17 modes / 241009 checks / 0 failures; third-party libraries uninstrumented |
| Weld-range sanitizer red/green |diagnostic-qualified committed-source fatal float-cast overflow; restored native weld and composed-UV controls green |
| Storage-output invariant |40 original salted production means; maximum 0.0 double ULP vs pre-storage source |
| Mode-off image parity |16 salted comparisons; every RGBA component within adopted 1 float32 ULP |

Xcode logs retain the pre-existing optional OIDN search-path linker warning and the AppIntents metadata notice. These are disclosed environment/tool notices, not owned source compiler diagnostics; they are not suppressed by compiler flags.

Fresh reds: pre-normalization-numerical-red: 849 passed, 160 failed; pre-factorization-numerical-red: 81 passed, 28 failed; pre-sss-clamp-numerical-red: 1017 passed, 48 failed; pre-r7-numerical-red: 3 passed, 20 failed; pre-r7-r7-signals-only-red: 945 passed, 400 failed; pre-r7-r7-signal-production-only-red: 353 passed, 40 failed; pre-transform-numerical-red: 281 passed, 60 failed; pre-composed-numerical-red: 321 passed, 30 failed; pre-composed-unsupported-red: 1005 passed, 76 failed; pre-composed-hygiene-red: (scanned 456 test files) 170 passed, 2 failed.; pre-differential-numerical-red: 36067 passed, 10 failed; pre-differential-r5-preparation-only-red: 23 passed, 8 failed; pre-differential-r5-provenance-only-red: 33 passed, 8 failed; pre-scratch-numerical-red: 177 passed, 24 failed; native-master-numerical-red: 127 passed, 50 failed. The pre-scratch failure is missing diagnostic capabilities, with its radiance checks positive; it is not a numerical estimator failure. Native-master production failures are the separate analytic-price proof.

The current SSS witness checks actual positive recursive shader returns for four salted 512-sample paths, diffusion/random-walk, indexed both windings, RGB/NM and actual HWSS. Reference clamp-on/off means are exact, ordinary-only and HWSS legacy clamp controls remain positive, and HWSS extended on/off means are exact. The polished-normal witness compares native SPF, replay/root prices and independent constraint/endpoint Jacobians, including variable direction/magnitude and transforms/both incidence sides. Fresh-thread factorization witnesses verify both helpers reserve configured maximum at fixed nesting depth and stop growing after first warm-up. Focused repairs plus full-gate replacements cover all three Round 8 findings; fresh Round 9 independent review remains required.

| Mode-off fixture | Paired cost change mean (%) | Sample SD (%) |
|---|---:|---:|
| 0 | 0.607050 | 0.717423 |
| 1 | 0.410710 | 0.549869 |
| 2 | 1.742170 | 0.607656 |
| 3 | 1.734842 | 0.415270 |

Cost uses actual alternating committed-source builds, n = 4 salts, one worker, reserve 0. Current production witness maximum retained worker-scratch capacity is 204192 bytes; this replaces earlier checkpoint capacities. Full timing/memory/counter records are retained in phase2-interleaved-cost.json, production-memory.log and fixture logs. Scratch accounting measures the documented worker-local buffers; caller-owned topology/results and preparation allocations are excluded. The accepted wide-cone slab discrepancy remains measured, with original channelwise 3-combined-SD bands and independent reference SD; no DL-420 closure. All target rows remain OPEN and Phase 2 remains unmerged.


### Fresh Round 9 disposition and scaled ONB repair

Fresh independent reviewers on `c82889fc4` returned clean estimator and evidence verdicts and one material P2 concerning scaled ONB W. The native witness verifies that this affects root position and price; it is repaired rather than deferred. No DESIGN finding. Evidence: `.claude/logs/sms-phase2-round9-final/fresh-reviews/`. The initial fixture used an invalid dielectric descriptor and failed to parse; that crash is retained as a failed test attempt, not a numerical red. The corrected, strengthened committed witness at `e528ca43f` gives **4145 passed / 1040 failed** on pre-repair native code: 400 native root-position failures and 640 replay/solved-price failures. Repair `80afa29b9` gives **5185/0** on that witness. Added coated-dielectric controls pass **7233/0**; the complete replacement gate remains pending. Evidence: `.claude/logs/sms-phase2-round10/frame-coating-green.log`.

The repair reproduces the existing native Optics normalization condition, including its derivative chain rule, for consumed geometric event normals and ordinary vector-Fresnel incidence. It retains raw ONB/material records and raw coating cosine convention. This is a sibling correction under the adopted native-event contract, not a new normalization policy for the native walker. The Round 9 gate remains historical evidence for its frozen checkpoint; it is not claimed as the replacement gate for this repair. Phase 2 remains unmerged and target rows remain OPEN.


### Fresh Round 10 replacement gate (2026-10-05)

Native/test checkpoint `7838405b68d69eb83d09055f5c2e826a18440ce6`; master `08552560b54b517b7a8a0696eb9d317953b26bbe`. Evidence: `.claude/logs/sms-phase2-round10-final/source.json` and `final-gate/completed-summary.json`. All 21 native and four test hashes verify. The scaled-ONB fix has one fresh coherent committed-source rollback scenario across all native paths: 5913 passed, 1320 failed; restored frame controls pass 7233/0. Earlier nine rollback scenarios remain explicitly historical evidence. The current gate reruns clean make and both Xcode builds, every required regression, full-project sanitizers, eight interleaved A/B source builds and sixteen salted mode-off comparisons, weld-range sanitizer proof and process memory.

| Gate | Result |
|---|---|
| Clean make | 376 compilation actions; zero owned diagnostics |
| Actual Xcode Deployment/Opto | 393/393 compilation actions; zero owned diagnostics |
| Make regressions | 20 executables / 43 modes / 424086 checks / 0 failures; CST separately |
| Actual Xcode-linked controls | 32 modes / 425132 checks / 0 failures |
| ASan/UBSan/float-cast-overflow | all 374 linked project units / 18 modes / 248322 checks / 0 failures; third-party libraries uninstrumented |
| Weld-range sanitizer proof | committed-source fatal float-cast overflow; restored weld and composed-UV controls pass |
| Storage-output invariant | 40 original salted production means; maximum 0.0 double ULP vs pre-storage checkpoint |
| Mode-off parity | 16 salted image comparisons; every RGBA component within adopted one float32 ULP |

The Xcode logs retain only the disclosed pre-existing optional OIDN search-path linker warning and AppIntents metadata notice; no source compiler warnings are suppressed. Native nonunit-normal warnings in the scaled-frame witness are expected Optics diagnostics demonstrating its normalization path.

The current frame witness covers PerfectReflector, PerfectRefractor, Dielectric and coated Dielectric actual RGB/NM events, actual root positions and prices, independent constraint/endpoint Jacobians, both indexed windings/incidences, constant/varying direction and magnitude, and nonuniform transformed instances. Ordinary vector-Fresnel uses the consumed normal while coating queries retain raw incidence. Polished vNormal normalization, configured-max LU reservation and opaque SSS reference-radiance provenance remain covered by restored make/Xcode/sanitizer controls. No new contract or native-walker normalization change is claimed.

| Mode-off fixture | Paired cost change mean (%) | Sample SD (%) |
|---|---:|---:|
| RGB k=1 | 1.145760 | 0.635085 |
| RGB k=2 | 0.950089 | 0.341971 |
| NM | 2.152881 | 0.348166 |
| HWSS | 2.046038 | 0.727164 |

Actual alternating builds use n = 4 independent salts, one worker and reserve 0. Current production witness retained worker-scratch capacity peaks at 204192 bytes; auxiliary topology/results and preparation allocations are outside that documented counter scope. Process memory and all timing/counter records are retained under the evidence directory. No multicore memory bound or general speed guarantee is claimed. Narrow-cone comparisons retain their original channelwise three-combined-SD bands and matching reference SD. The user-accepted wide-cone discrepancy remains measurement-only and supplies no DL-420 closure.

Fresh independent Round 10 review remains required. Phase 2 remains unmerged; target rows stay OPEN and no new row is opened.


### Fresh Round 10 disposition and focused repairs (2026-10-06)

Reviewed head `30a7ef45f`: estimator P2 near-unit event law; material P1 coated TIR and the same P2; evidence no P1/P2. No DESIGN finding. Logs: `.claude/logs/sms-phase2-round10-final/fresh-reviews/`.

Mandatory native TIR is now preserved before any raw-cosine film query. The audited-frame half vector uses the native outgoing-vector length, its analytic derivative and a normalized projection basis; normalization-branch transitions invalidate unresolved derivatives. This reproduces the existing event law without changing native walkers, public interfaces or solver tolerances.

The combined focused witness passes **44681/0** in `.claude/logs/sms-phase2-round10/native-events-final-green.log`. It combines the native event/geometry matrix, a bounded indexed patch, **1185/0** open-plane/closed-indexed/nested/overlap coated-TIR controls and actual PT RGB/NM production. The final production oracle is one open sheet, with four salts at N=2048, channel-separated metrics and analytic geometry priced using actual native BSDF/emitter values (reference SD=0). Evidence: `tir-production-matched-green.log`. The earlier box production attempt included other reflective walls and is not a matched single-root reference. Earlier incomplete TIR fixture attempts are retained as failed test development and are not final numerical proofs. Added production counter reporting changes diagnostics only and will be compiled by the replacement gate.

The full 21-path committed-source rollback, replacement clean builds/regressions/Xcode controls/sanitizers/parity/cost/memory and fresh Round 11 review remain required. Historical Round 10 gate greens do not establish correctness of this new native source. Phase 2 remains unmerged and all target ledger rows remain OPEN.


### Fresh Round 11 replacement gate (2026-10-06)

Native/test checkpoint `ba8cac0dc3d470a90df8a62bb1aa41a4374a9482`; master `08552560b54b517b7a8a0696eb9d317953b26bbe`. Evidence: `.claude/logs/sms-phase2-round11-final/source.json` and `final-gate/completed-summary.json`. All 21 native and four test hashes verify. The coated-TIR/near-unit-event fix has one fresh coherent committed-source rollback scenario across all native paths: 40365 passed, 4316 failed; restored native-event controls pass 44681/0. Ten earlier rollback scenarios remain explicitly historical evidence. The current gate reruns clean make and both Xcode builds, every required regression, full-project sanitizers, eight interleaved A/B source builds and sixteen salted mode-off comparisons, weld-range sanitizer proof and process memory.

| Gate | Result |
|---|---|
| Clean make | 376 compilation actions; zero owned diagnostics |
| Actual Xcode Deployment/Opto | 393/393 compilation actions; zero owned diagnostics |
| Make regressions | 20 executables / 44 modes / 468767 checks / 0 failures; CST separately |
| Actual Xcode-linked controls | 34 modes / 514494 checks / 0 failures |
| ASan/UBSan/float-cast-overflow | all 374 linked project units / 19 modes / 293003 checks / 0 failures; third-party libraries uninstrumented |
| Weld-range sanitizer proof | committed-source fatal float-cast overflow; restored weld and composed-UV controls pass |
| Storage-output invariant | 40 original salted production means; maximum 0.0 double ULP vs pre-storage checkpoint |
| Mode-off parity | 16 salted image comparisons; every RGBA component within adopted one float32 ULP |

The Xcode logs retain only the disclosed pre-existing optional OIDN search-path linker warning and AppIntents metadata notice; no source compiler warnings are suppressed. Native nonunit-normal warnings in the scaled-frame witness are expected Optics diagnostics demonstrating its normalization path.

The current native-event witness covers PerfectReflector, PerfectRefractor, Dielectric and coated Dielectric actual RGB/NM events, actual root positions and prices, independent constraint/endpoint Jacobians, both indexed windings/incidences, constant/varying direction and magnitude, nonuniform transforms, magnitudes inside/outside both Optics normalization boundaries, and a narrow indexed patch containing the native root while excluding the spurious solution. Coated TIR is tested on an open plane, closed indexed solids, nested exits and actual non-top overlapping solids. The actual PT production control uses four salts at N=2048, channel-separated RGB/NM means, a single open-sheet reflection and analytic geometry with native BSDF/emitter values and zero reference SD. Earlier box production attempt was not a matched single-root oracle and is retained only as a discarded test attempt. Mandatory native TIR stays unity before any coating query. The half vector includes the native outgoing vector length with its analytic derivative and a unit projection basis; normalization branch changes reject unresolved derivatives. Ordinary vector-Fresnel uses the consumed normal while non-TIR coating queries retain raw incidence. Polished vNormal normalization, configured-max LU reservation and opaque SSS reference-radiance provenance remain covered by restored make/Xcode/sanitizer controls. No new contract or native-walker normalization change is claimed.

| Mode-off fixture | Paired cost change mean (%) | Sample SD (%) |
|---|---:|---:|
| RGB k=1 | 0.445559 | 0.523223 |
| RGB k=2 | 1.111597 | 0.734787 |
| NM | 1.409409 | 1.168709 |
| HWSS | 1.818822 | 0.939373 |

Actual alternating builds use n = 4 independent salts, one worker and reserve 0. The original flat delta-light production witness retained worker-scratch capacity peaks at 204192 bytes; the modified coated-TIR production witness peaks at 372432 bytes across its two retained scratch frames, including warm-up growth. Current modified-witness counters: `coated TIR counters proposals=32770 zeros=202 newton=243966 retries=16386 tails=0 roulette=0 owned=16284 rejected=202 sceneQueries=392836 objectQueries=49795139 domainMaterialQueries=261560 scratchGrowths=32 scratchFrames=2 scratchPeakBytes=372432`. These are separate measured fixture scopes; auxiliary topology/results and preparation allocations are outside that documented counter scope. Process memory and all timing/counter records are retained under the evidence directory. No multicore memory bound or general speed guarantee is claimed. Narrow-cone comparisons retain their original channelwise three-combined-SD bands and matching reference SD. The user-accepted wide-cone discrepancy remains measurement-only and supplies no DL-420 closure.

Fresh independent Round 11 review remains required. Phase 2 remains unmerged; target rows stay OPEN and no new row is opened.


Phase 2 Round 11 review and repair checkpoint (2026-10-06): estimator and evidence reviewers reported no P1/P2 or design finding. The material reviewer found a P2: native DielectricSPF only attempts the geometric transmission fallback when the initial reflectance is below one. Extended replay had attempted it after every successful shading Snell calculation. The repair binds native constraints to the RGB component or NM wavelength, distinguishes true TIR from film saturation, preserves the native raw-film query after successful refraction, and sweeps crossing, pricing and proposal consumers. No native scattering algorithm or virtual SPF interface changes.

`SMSExtendedReferenceTest --r11-saturated-film-only` passes 14635/0 on the repaired source. It exercises four film variants (unsaturated, critical/saturating, opaque after fallback, absorptive), actual RGB/NM SPF events, both indexed windings/incidences, transformed open and closed geometry, start-inside, plane and sphere controls, and invalid RGB domain rejection. Active-event branch and full/half coating-Fresnel price probes enforce ordinary-zero rejection when the final root cannot resolve native support or price continuity. Independent Jacobian checks cover regular unsaturated controls; critical-film discontinuities are tested for native event pricing and uncertainty rejection, not claimed as differentiable. Earlier fixture experiments and partial working-tree red counts are historical; a new coherent committed-source rollback and full replacement gate are required before integration. No ledger row opens or closes.

The first Round 12 proof run stopped at the geometry regression (2817/40): the new native-event check had reused a seed flag that also means derivatives are not computed yet. Unmodified three-event seeds therefore failed the initial constraint check before derivative construction. The local native constraint copy now initializes event/context validity independently, retaining false for failed native frame reconstruction or impossible transmission. Existing mixed-event geometry controls pass 4297/0 and the four-film witness remains 14635/0. The failed gate is retained as an interrupted historical attempt; the final replacement gate must restart on the corrected committed source and include this committed regression checkpoint as a second coherent rollback proof.

The second Round 12 proof run stopped on four transformed NM signal-painter roots (1645/4). The newly added optical-discontinuity probe had tested full attenuation weight, including native float-precision spectral reconstruction, against a double-precision optical band. The probe now checks only the coating Fresnel price that gates native fallback; painter input contexts retain the existing geometric/UV/normal checks. This narrows the implementation to its intended native optical support check without changing bands or attenuation evaluation. Both interrupted gate attempts remain historical; the final complete gate must restart on the corrected source.

The corrected optical-price scope passes the focused sequence: signal prices 1665/0, mixed geometry 4297/0, four-film fallback 14635/0, signal production 393/0, curved UV 425/0, polished normals 1089/0, scratch depth 109/0, SSS clamps 1065/0, scaled frames 7233/0 and native events 44681/0. Checked library/test builds have zero compiler diagnostics. These are focused results, not substitutes for the final complete gate or fresh review.


### Fresh Round 12 replacement gate (2026-10-06)

Native/test checkpoint `1187f276ce66a65052b4bc0b5f01aa043c8ea2bd`; master `08552560b54b517b7a8a0696eb9d317953b26bbe`. Evidence: `.claude/logs/sms-phase2-round12-final/source.json` and `final-gate/completed-summary.json`. All 23 native and four test hashes verify. The Fresnel-dependent fallback fix has three fresh coherent committed-source rollback scenarios across all native paths: Fresnel fallback 14203 passed, 432 failed; seed-event validity 2817 passed, 40 failed; optical-price scope 1645 passed, 4 failed; restored native-event controls pass 14635/0. Eleven earlier rollback scenarios remain explicitly historical evidence. The current gate reruns clean make and both Xcode builds, every required regression, full-project sanitizers, eight interleaved A/B source builds and sixteen salted mode-off comparisons, weld-range sanitizer proof and process memory.

| Gate | Result |
|---|---|
| Clean make | 376 compilation actions; zero owned diagnostics |
| Actual Xcode Deployment/Opto | 393/393 compilation actions; zero owned diagnostics |
| Make regressions | 20 executables / 45 modes / 483402 checks / 0 failures; CST separately |
| Actual Xcode-linked controls | 36 modes / 543774 checks / 0 failures |
| ASan/UBSan/float-cast-overflow | all 374 linked project units / 20 modes / 307648 checks / 0 failures; third-party libraries uninstrumented |
| Weld-range sanitizer proof | committed-source fatal float-cast overflow; restored weld and composed-UV controls pass |
| Storage-output invariant | 40 original salted production means; maximum 0.0 double ULP vs pre-storage checkpoint |
| Mode-off parity | 16 salted image comparisons; every RGBA component within adopted one float32 ULP |

The Xcode logs retain only the disclosed pre-existing optional OIDN search-path linker warning and AppIntents metadata notice; no source compiler warnings are suppressed. Native nonunit-normal warnings in the scaled-frame witness are expected Optics diagnostics demonstrating its normalization path.

The current native-event witness covers PerfectReflector, PerfectRefractor, Dielectric and coated Dielectric actual RGB/NM events, actual root positions and prices, independent constraint/endpoint Jacobians, both indexed windings/incidences, constant/varying direction and magnitude, nonuniform transforms, magnitudes inside/outside both Optics normalization boundaries, and a narrow indexed patch containing the native root while excluding the spurious solution. Coated TIR is tested on an open plane, closed indexed solids, nested exits and actual non-top overlapping solids. The actual PT production control uses four salts at N=2048, channel-separated RGB/NM means, a single open-sheet reflection and analytic geometry with native BSDF/emitter values and zero reference SD. Earlier box production attempt was not a matched single-root oracle and is retained only as a discarded test attempt. Mandatory native TIR stays unity before any coating query. The half vector includes the native outgoing vector length with its analytic derivative and a unit projection basis; normalization branch changes reject unresolved derivatives. Ordinary vector-Fresnel uses the consumed normal while non-TIR coating queries retain raw incidence. Polished vNormal normalization, configured-max LU reservation and opaque SSS reference-radiance provenance remain covered by restored make/Xcode/sanitizer controls. No new contract or native-walker normalization change is claimed.

| Mode-off fixture | Paired cost change mean (%) | Sample SD (%) |
|---|---:|---:|
| RGB k=1 | 2.874100 | 4.434951 |
| RGB k=2 | 2.542661 | 2.424490 |
| NM | 2.843019 | 1.180798 |
| HWSS | 2.197942 | 0.240475 |

Actual alternating builds use n = 4 independent salts, one worker and reserve 0. The original flat delta-light production witness retained worker-scratch capacity peaks at 204192 bytes; the modified coated-TIR production witness peaks at 372432 bytes across its two retained scratch frames, including warm-up growth. Current modified-witness counters: `coated TIR counters proposals=32770 zeros=202 newton=243966 retries=16386 tails=0 roulette=0 owned=16284 rejected=202 sceneQueries=392836 objectQueries=49789955 domainMaterialQueries=261560 scratchGrowths=32 scratchFrames=2 scratchPeakBytes=372432`. These are separate measured fixture scopes; auxiliary topology/results and preparation allocations are outside that documented counter scope. Process memory and all timing/counter records are retained under the evidence directory. No multicore memory bound or general speed guarantee is claimed. Narrow-cone comparisons retain their original channelwise three-combined-SD bands and matching reference SD. The user-accepted wide-cone discrepancy remains measurement-only and supplies no DL-420 closure.

Fresh Round 12 review completed: estimator and material reviewers found two P2s in uncoated fallback replay; evidence review found no P1/P2. The repairs require a fresh Round 13 gate and review. Phase 2 remains unmerged; target rows stay OPEN and no new row is opened.

Round 12 process-memory measurement: maximum RSS 103153664 bytes; peak footprint 82903592 bytes (`production-memory.log`). The eight-build n=4 cost figures above are observed overheads, not speedups. The new four-film optical-price witness retains the stated regular-Jacobian/critical-film uncertainty scopes; no attenuation quantization, solver band or native scattering law is changed.


### Round 12 review repair: uncoated fallback family

Three fresh independent reviewers examined committed checkpoint `755e48f2d`. Estimator review found a P2: the native PerfectRefractor `ref < 1` fallback gate was absent from SMS replay. Material review found a P2: uncoated Dielectric/PerfectRefractor reflection-price jumps caused by transmission fallback were outside the coating-only final root probe. Evidence review found no P1/P2 or new design issue. Both P2s are fixed in the shared RGB/NM replay; native SPFs are unchanged. HWSS remains legacy in Phase 2.

The `--r12-uncoated-fallback-only` witness uses actual RGB/NM SPFs on both indexed windings and incidences, open and closed geometry, plane and sphere geometry, nonuniform transforms, start-inside media, neighboring fallback contexts and actual seed proposals. The initially committed 0.01 start segment straddled the unchanged legacy minimum reliable segment and is discarded as test setup. Its corrected 0.1-segment committed red had 12887 passes / 1340 failures before the family repair. That result predates the final point-price oracle correction and is historical, not the current final-test red proof.

The independent native full/half root-band probes determine whether a context has a resolved optical price. Exact transmission-horizon discontinuities require an invalid root (ordinary zero); independently regular contexts require matching native event and root prices, including the constant unity-Fresnel grazing controls. Twenty transformed-sphere point-price assertions at an independently unresolved discontinuity were removed from the exact-price oracle, while their zero-root assertions remain. No uncertainty band, native walker or attenuation-painter quantization rule changed. The current focused uncoated witness passes 17107/0. A coherent committed native rollback with this final test, full replacement gates and fresh review remain required before integration.
