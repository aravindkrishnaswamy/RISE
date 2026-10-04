# Extended SMS Phase 2 validation

Phase 2 is implemented on `sms-ext-phase2`, based on master
`08552560b54b517b7a8a0696eb9d317953b26bbe`, but is not yet merged.
The latest native source is `761c71b51`; its coherent committed repair proof
passes review **20005/0**, production **177/0** and geometry **4297/0**.
The new full replacement pipeline stops at the strict mode-off bit-identity
gate: all sixteen baseline/candidate hashes differ, with reported radiance
mean differences no larger than 1.7763568394002505e-15. Pixel diagnostics are complete (see the gate-stop section below). Clean make/Xcode regressions and sanitizers in this replacement
have not started; fresh Round 3 review and integration remain pending.
The complete `13a31c7d7` gate below is historical evidence for that checkpoint,
not a passing validation of the latest source.

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

## Current-source off-mode cost and output gate

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

These focused numbers do not replace the prior complete 13a31c7d7 gate.
Committed-source proofs, a full replacement gate, new timing measurements and
fresh round 3 reviewers remain pending. No row closes and no merge is claimed.

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
reports **20005/0** with checked zero-diagnostic library/test builds. New committed proofs,
the full replacement gate and fresh round 3 review remain required.

### Latest committed sibling proof and mode-off gate stop

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
not a passing phase gate or a zero-overhead claim. The mode-off bit-identical
contract remains unchanged; the latest source is unmerged.

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
not a relaxed contract and not bit identity of internal doubles. Complete
per-image/channel results: `off-mode-diagnostic/comparison.json`.

**Proposed acceptance amendment, not adopted:** use bit-identical float32
image pixels for the cross-build shipped mode-off gate, retaining strict
internal-double on/off comparisons within the same build for deterministic
rejection cases. Retain salted n>=4 channel-separated original three-SD
comparisons where stochastic output differs. This requires the user's ruling
before changing any acceptance test or continuing the interrupted gate.
The phase remains on `sms-ext-phase2`; no ledger row closes and no new row
is opened. Master stays `08552560b54b517b7a8a0696eb9d317953b26bbe` and clean.
