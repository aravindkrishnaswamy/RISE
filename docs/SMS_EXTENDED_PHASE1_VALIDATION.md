# Extended SMS Phase 1 validation (2026-10-03)

Status: Phase 1 primitives and interim scene policy merged from `sms-ext` at master `34bd520ec5e70a5cf96bcf8b8154b1a17888880f` after the final serial gate and fresh round 3 returned zero P1/P2. No ledger closure is claimed. This merge-record documentation adds no source change.

## Scope and rejection contract

`SMSDomainReplay` and `ManifoldSolver::SolveDomain` are isolated native-domain primitives. RGB queries use the authored component; NM queries evaluate both medium sides at the requested wavelength. Starting medium captures preserve identities and intersection context. Reflection preserves membership; transmission uses native closed-object and certified DL-345 sheet crossing policies. Solved roots refresh native tint, absorption, Fresnel and AR coatings from real intersections, retaining caller raster coordinates.

Composite queries fail. The adopted interim policy disables extended mode scene-wide when preparation finds a composite, including material wrappers, nested composites and CSG operands. Existing SMS contribution, PT suppression and delta-light shadow behavior then run unchanged. Starting captures decline composite scenes, including empty-stack anchors outside composite bounds. Positive composite replay needs a separate walker estimator and DL-407 repair.

Positive extended production estimators are not enabled in this phase. Default RGB/NM retain the existing estimator; HWSS retains legacy eligibility through a scoped mode flag on its NM delegations. Composite preparation emits its own scene policy warning; an active, composite-free extended configuration emits one HWSS warning per prepared solver. Production channel/medium correctness and HWSS debt closures require their later activation gates. No target row closes merely because these primitives pass.

## Numerical evidence

`SMSDomainReplayTest` exercises double-sided indexed meshes with both windings and incidence sides, open and closed shapes, transformed instances, sphere/plane controls, nested exits, start-inside, and an enclosing SF11 medium absent from the solved chain. Independent Sellmeier, Snell and single-film Airy oracles check indices and pricing. Composite PT comparisons use four salted trials, paired per-channel differences and a three-standard-error band (3 × sample SD / sqrt(4)); deterministic camera-to-emitter paths require exact raw hashes.

At `8172ad28c`, the complete domain suite passed **2917/0**, reporting **120 attempts, 184 Newton iterations, 118 accepted and 2 rejected roots**. These isolated biased solves have **0 retries, 0 tail events and 0 owned roots**; they do not test rediscovery or canonical ownership.

Committed master `ffc70c1c2` sources plus all dependent changed headers, built coherently against the committed regression test, produce **14 passed / 10 failed**. Failures are two component Snell roots, three root tint/Fresnel weights, four pixel-context RGB/NM weights and one nested spectral exterior. A compile-time feature marker selects actual master `BuildSeedChain`, `Solve`, `BuildSnellBaseSeed` and `EvaluateChainThroughputNM` calls because master has no `SolveDomain`; compilation failure is not counted as a red proof. Logs: `.claude/logs/sms-phase1/latest-master-{build,test-build,red}.log`.

Additional committed-helper regressions:

- Finite-Phong query eligibility before correction: **168/96**; corrected query-only suite **504/0**. Exponents 1e4 and 1e5 are admitted at their native delta-limit approximation; the query records that approximation. HG partial coherent mixtures remain unsupported. This is a new helper defect, not a claimed master defect.
- Pixel-context isolated corrected fixture: **16/0**. The initial diagnostic had one bad setup assertion (the existing `AssignMaterial` return value), which is excluded from numerical evidence; the committed-master proof above compares actual native material queries at caller raster coordinates.
- Inherited composite CSG boundaries at committed `ac7802caa`: **6/2**, both failures are missing rejection for start-inside membership. Corrected complete domain suite at `7e66dc267`: **2925/0**, with the same 120/184/118/2 diagnostics. The two missing rejection assertions now pass. Complete phase acceptance remains pending below.

The polished native-SPF audit is a positive law oracle, not a bug fix: the corrected fixture passes **328/0** on committed pre-audit helper sources. The initial fixture assumed the native black substrate emitted no diffuse ray; native SPF emits a zero-weight diffuse ray too. Its count-based failures are not numerical red evidence. `SpecularInfo` already defaults reflection tint to true, so the suspected pricing defect was disproved and the redundant source change removed.

- Participating media at committed `ed5a5b5ab`: **168/312**; corrected query-only fixture at `f94e5dca3`: **480/0**. Refuse global media, local-medium capture/replay and local-medium R/T crossings before mutation. This concerns participating media; graded IOR painters already refuse position-dependent IOR. Logs: `medium-red-run.log` and `medium-green-run.log`.

The updated complete domain suite at `f94e5dca3` passes **3789/0**, including four-salt, both-winding HWSS legacy-ignore checks for every lane on direct and start-inside composite paths. Exactly **16** warning lines occur: one per extended-mode render instance. Diagnostics remain 120/184/118/2 for attempts/Newton/accepted/rejected, with no retries, tails or owned roots.

## Default-path cost and output

Interleaved coherent master/candidate source builds (A=`ffc70c1c2`, B=`4108e8f96`) used four salts for each fixture, sequential rendering, identical worker policy and bounded 64×64/64 spp settings with denoising disabled. All **16 matched raw hashes and image means were identical**. Subsequent finite-domain, pixel-refresh and CSG changes are confined to opt-in helper paths; they were not included in this timing checkpoint.

| Fixture | A seconds mean (sample SD) | B seconds mean (sample SD) | Paired change % mean (sample SD) |
|---|---:|---:|---:|
| shipped RGB k=1 | 0.958059 (0.009534) | 0.961576 (0.011595) | +0.372 (1.373) |
| shipped RGB k=2 | 0.732073 (0.006861) | 0.732741 (0.006744) | +0.093 (0.685) |
| uniform scalar NM | 1.706870 (0.029903) | 1.710620 (0.026398) | +0.223 (0.554) |
| uniform HWSS | 5.727214 (0.071455) | 5.761057 (0.067400) | +0.592 (0.182) |

The HWSS checkpoint has a small positive cost change; no zero-overhead claim is made. The design promises no whole-render cost bound. Evidence: `.claude/logs/sms-phase1/interleaved-cost.json` and the A/B build/run logs. `SMSLegacyModeTest` alone is a finite/lit probe; hash equivalence is asserted by the interleaved driver.

## Record and build diagnostics

Domain checkpoint at `1c19bbf42`: **3253/0**, including the 328 positive polished native-law checks. Counters remain 120 attempts / 184 Newton iterations / 118 accepted / 2 rejected; no estimator retries, tails or ownership are exercised.

On arm64, the diagnostic reports bytes: `SpecularInfo` 56, `ManifoldVertex` 336, `SMSChainRecord` 2176, internal `ManifoldSolverConfig` 96, `SMSDomainVertex` 1584, `SMSMediumCapture` 1264, `SMSStartingMedia` 40, and `SMSDomainCounters` 32. Standalone arm64 header probes against committed master `ffc70c1c2` and candidate `f94e5dca3` confirm `SpecularInfo` 56, `ManifoldVertex` 336, `SMSChainRecord` 2176 and `IORStack` 40 in both. Internal `ManifoldSolverConfig` grows 80→96 bytes and `ManifoldSolver` 160→184 bytes. Evidence: `record-size-driver.log`, with zero-warning builds of copied committed headers whose relative includes resolve to the worktree. The config adds internal opt-in/diagnostic fields; the solver adds its warning latch. Capture scratch holds one identity/context per enclosing object, plus temporary scene-object enumeration. This is not the later proposal scratch-budget measurement.

Clean make: **376 compilation actions, exit 0, zero warnings/errors** (`medium-final-library-build.log`). Clean Xcode RISE-GUI Deployment and RISE-GUI-Opto builds: **393 CompileC actions each, exit 0, zero compiler warnings/errors**. Each emits exactly the two documented checkout/tool notices: missing `extlib/oidn/install/lib` linker search path and skipped AppIntents extraction. AGENTS.md explicitly discounts the missing OIDN search path; skipped AppIntents extraction is a tool notice rather than a compiler diagnostic. No compiler warning was suppressed. The initial Xcode source checkpoint matched the helper after removing the redundant polished flag change. Following the participating-medium refusal fix, clean make and clean Xcode Deployment have passed again (376 and 393 actions respectively); the final Opto rebuild has also passed (393 CompileC actions).

## Acceptance gate

At the pre-review `77714bd89` checkpoint, all 20 required test/mode entries had latest exit **0**, each built individually with checked exit code and no build warnings. Source fixes after the early gate entries affect only opt-in primitives; the unchanged legacy paths were tested throughout, and the final domain executable was rebuilt after every helper fix. Logs are in `.claude/logs/sms-phase1/gate`; `results.json` preserves the initial artifact-only hygiene failure as well as its passing rerun. `final-verification.json` checks the latest result for each entry.

| Test/mode | Result |
|---|---:|
| ManifoldSolverTest | 388/0 counted DL-435 assertions; other unit groups pass (no aggregate count emitted) |
| SMSUniformDispersionTest | 300/0 |
| SMSUniformDispersionTest --shipped | 10/0 |
| ExteriorIndexInvarianceTest | 299/0 |
| SMSEmitterDirectionTest | 344/0 |
| SMSMediumAnchorTest | 27/0 |
| TransparentShadowPartitionTest | 42/0 |
| WeaveGapShadowTransmittanceTest | 244/0 |
| OpenSheetIndexConventionTest | 24/0 |
| GradedIndexInteriorFactorTest | 101/0 |
| ManifoldNormalDerivativeTest | 141/0 |
| DoubleSidedEmitterTest | 34/0 |
| AlphaSMSGeometryTest | 216/0 |
| AlphaSMSTransportTest | 20/0 |
| AlphaSMSReciprocalTest | 27/0 |
| PTGuidingMISPartitionTest | 185/0 |
| SourceHygieneTest | 169/0 |
| CstDeriveGoldenTest | 458 match, 0 drift; 465 corpus, 0 uncovered/stale |
| SMSDomainReplayTest | 3789/0 |
| SMSLegacyModeTest | 21/0 |

The pre-review checkpoint's off-mode output was compared against all 16 salted committed-master A checkpoints: raw hashes **and means match exactly**. That checkpoint's HWSS ignore guard emitted exactly 16 warnings for 16 extended render instances. The final clean make and Xcode Deployment/Opto builds pass with zero compiler warnings. Both Xcode configurations have the two documented environment/tool notices only.

The first hygiene run reported 168/1 because generated GUI resource copies lived in `DerivedData-Deployment` and `DerivedData-Opto`, names outside the scanner's existing `DerivedData` exclusion. Products were relocated under `.claude/logs/sms-phase1/DerivedData/{Deployment,Opto}` without changing maintained sources or the test. The preserved failed scan is `SourceHygieneTest-artifact-failure-run.log`; the rebuilt/rerun test passes 169/0. Build stdout remains at its original log path, and `xcode-products-relocation.json` records the artifact relocation.

Round 1 completed with two P1s, recorded below. A fresh round on the corrected tree remains pending. No phase merge or debt closure is claimed. Review must distinguish this primitive/rejection increment from the later proposal, full ordered scene visibility/acceptance, reference estimator and canonical ownership gates. In particular, final-root refresh currently re-intersects the recorded objects; the later production proposal/validation must establish full scene order and supported intervening hits before pricing a physical estimator contribution.

## Independent review round 1 — not converged

Three fresh, read-only reviewers inspected `77714bd89` after the serial gate.
The estimator/partition lens found one P1: HWSS legacy-mode provenance is
call-local. `RayCaster::CastRayHWSS` can fall back to `CastRayNM` for a
participating medium before either guarded HWSS integrator entry. Shader
dispatch then invokes `IntegrateFromHitNM` with its default false bypass.
The same bypass can be lost when a delegated NM path recasts through the
diffusion-profile or random-walk SSS continuation. These routes need a
persistent, scoped provenance signal and shader-dispatch regressions; the
existing direct-integrator composite parity tests do not prove them.

The material/medium lens found one P1: a finite composite sheet and an
anchor behind it, outside its bounds, with an empty live IOR stack can
produce `Capture::reconstructible=true`. Bounds cannot certify absence of
DL-407 open-sheet membership. The capture API has no trustworthy history
provenance. The user's STOP rule requires a ruling before adopting either
scene-wide conservative exclusion or a new provenance mechanism. The
adopted composite rejection contract itself remains unchanged.

The cost/tests/docs lens found zero P1s and two P2s, corrected in this
review record: cite the final 376-action make log rather than the earlier
375-action log, and distinguish the AppIntents tool notice from AGENTS'
explicit OIDN exclusion. It independently recomputed the A/B statistics,
verified all 16 baseline/candidate and baseline/final hashes and means,
and checked the numerical red proofs, latest 20 passing gate entries,
3789/0 domain result, and final Xcode builds.

At the round-1 review checkpoint, both P1s were source-traced findings
without new numerical red proofs or source corrections. Earlier build/test
numbers remain checkpoint evidence; they do not establish completion of
Phase 1. Subsequent corrections and proofs are recorded below. No phase
merge, ledger closure or new ledger row occurred.

## Round 1 corrections — targeted proofs complete

The user adopted scene-wide composite rejection from prepared static scene
data. `ObjectManager::PrepareForRendering` caches the first composite
object name and emits one policy warning per preparation. RGB/NM/HWSS
transport reads that immutable prepared decision rather than probing
composite membership at each anchor. Composite-free scenes retain the
extended eligibility path. The future seed-certainty bit is documented
only; no ledger row is opened.

`SMSLegacyModeScope` carries HWSS legacy-mode provenance in the worker's
`RuntimeContext` across medium fallback, NM shader dispatch and nested
diffusion/random-walk SSS recasts, restoring the caller's mode on scope
exit. Tests use actual shader-dispatch paths and count nested NM calls.
The targeted committed red/green proofs are complete; the clean full
gate and fresh independent review are in progress. Earlier round-1
findings remain historical evidence.


Committed pre-fix source `031292067` (including coherent dependent headers)
was compiled successfully against the targeted regression harness. The
prepared-policy probe returned **31 passed / 33 failed**: six missing or
incorrect warning count/name assertions, three incorrect capture
acceptances outside composite bounds, and twenty-four wrapped-composite
RGB/NM image parity failures. The HWSS shader-dispatch probe returned
**51 passed / 12 failed**, with an image/lane comparison failing for each
of four salts in global-medium, diffusion SSS and random-walk SSS routes.
Missing pre-fix mode telemetry is reported as unavailable, not zero loss.
These are committed Phase 1 helper regressions: master has no extended
mode or these new APIs, so this pre-fix source checkpoint is the relevant
baseline. The earlier master-native **14/10** oracle remains separately
recorded above.

The initial policy red probe contained three feature-availability
assertions; they were removed before the final **31/33** proof. The final
proof asserts actual warning, capture and renderer behavior. Logs:
`round2-final-prior-library-build.log`, `round2-final-prior-domain-build.log`,
`round2-final-scene-policy-red.log`, and
`round2-final-hwss-dispatch-red.log`.

Current targeted results: prepared-policy **1168/0**, HWSS dispatch
**3147/0**, complete domain **8176/0**. The prepared-policy total includes
four salted NM controls in a composite-free scene: unsupported-photon
eligibility executes instead of the legacy bypass. Both outputs are finite;
the legacy baseline is lit. Ordinary PT can be dark after rejecting that
point-light SMS caustic. A preliminary assertion incorrectly required the
rejected result to remain lit (three failures); it was corrected without
changing transport or adding a source. The failure log is retained as
`round2-scene-policy-dark-control-failure.log`. Composite parity controls
still require a lit baseline and exact matched RGB/NM outputs. Library sources at that checkpoint matched cost source `6988bd1c9`.
The later empty-name correction below changes preparation and opt-in policy
reads only; it adds no default per-sample work. Current domain counters remain **120 attempts / 184 Newton
iterations / 118 accepted / 2 rejected**, with no proposal retries, tails
or owned roots claimed.

### Updated interleaved cost checkpoint

Each of four salts used an actual committed-source A/B library and test
build followed by its render, interleaved A then B. A is master
`ffc70c1c2`; B is library source `6988bd1c9`. This supersedes the preliminary
measurement that alternated two saved static executables. Both methods
matched all sixteen hashes and means exactly. Worker/resolution settings
remain the explicitly bounded single-worker 64×64/64 spp configuration,
not an all-core GUI throughput benchmark.

| Fixture | A seconds mean (sample SD) | B seconds mean (sample SD) | Paired change % mean (sample SD) |
|---|---:|---:|---:|
| shipped RGB k=1 | 0.956903 (0.012980) | 0.968139 (0.010093) | +1.185 (1.499) |
| shipped RGB k=2 | 0.732758 (0.004240) | 0.728365 (0.003373) | −0.597 (0.752) |
| uniform scalar NM | 1.713578 (0.003774) | 1.721044 (0.014080) | +0.436 (0.900) |
| uniform HWSS | 5.745235 (0.021722) | 5.763783 (0.031801) | +0.324 (0.651) |

No zero-overhead claim is made. Each paired mean is within three measured
standard errors of zero (n=4); no design cost bound was invented. Evidence:
`round2-interleaved-cost.json`, the individual
`round2-interleaved-library-build-*`, `round2-interleaved-test-build-*` and
`round2-interleaved-*` render logs. All eight library/test build pairs
passed without compiler diagnostics. The interleaved driver later exited
nonzero on the preliminary dark-control assertion after finishing cost
measurement; sources were restored, the test assertion corrected, and
the rebuilt targeted/full tests passed as reported above. The numerical
cost data and source restoration are independently retained.


### Empty object names — preparation presence is separate from name

The actual object-manager API accepts an empty name. A self-audit found
that `ExtendedSMSAllowed` inferred absence from an empty first-composite
name, so an empty-name alias could re-enable mode and skip the warning.
Committed regression state `10a2c2cdc` built successfully and failed
**8 passed / 16 failed** over real double-sided open/closed indexed meshes,
both windings: four cases each of missing warning, missing first-name
representation, incorrectly active mode, and incorrectly accepted capture.
`ad02ff7d2` caches an explicit presence bit separately from the display name;
its rebuilt query-only probe passes **24/0**. An empty first name is logged
as `composite object ''`. Complete domain now passes **8200/0**; domain
attempt/Newton/accepted/rejected counters remain **120/184/118/2**.

Evidence: `empty-name-red-build.log`, `empty-name-red.log`,
`empty-name-green-build.log`, `empty-name-green.log`, and
`round2-final-domain-green.log`. The cost checkpoint predates this cold
preparation correction; default per-sample control flow still returns from
`ExtendedModeActive` on the false opt-in flag before reading scene policy.
No timing claim covers a composite-enabled scene or the new empty-name
fixture. The first round-2 full gate was stopped at a completed test
boundary after this source correction: clean make and both Xcode builds,
ManifoldSolverTest and SMSUniformDispersionTest (300/0) had passed. Their
logs remain under `round2-gate`. A full clean gate on the corrected tree
will be recorded separately under `round2-final-gate` before fresh review.


## Final round-2 gate before fresh review

The full gate on library source `ad02ff7d2` (documentation checkpoint
`116cf5fa3`) completed serially with all **20 test/mode entries passing**.
Each test was built individually, its successful exit and absence of
compiler diagnostics checked before execution. Clean make compiled **376**
actions; clean Xcode Deployment and Opto compiled **393** actions each.
All three builds exited zero with **zero compiler warnings/errors**. Each
Xcode build has the same OIDN linker-search and AppIntents tool notices
reported above; neither is a suppressed compiler warning.

| Test/mode | Result |
|---|---:|
| ManifoldSolverTest | DL-435 388/0; other unit groups pass, no aggregate emitted |
| SMSUniformDispersionTest | 300/0 |
| SMSUniformDispersionTest --shipped | 10/0 |
| ExteriorIndexInvarianceTest | 299/0 |
| SMSEmitterDirectionTest | 344/0 |
| SMSMediumAnchorTest | 27/0 |
| TransparentShadowPartitionTest | 42/0 |
| WeaveGapShadowTransmittanceTest | 244/0 |
| OpenSheetIndexConventionTest | 24/0 |
| GradedIndexInteriorFactorTest | 101/0 |
| ManifoldNormalDerivativeTest | 141/0 |
| DoubleSidedEmitterTest | 34/0 |
| AlphaSMSGeometryTest | 216/0 |
| AlphaSMSTransportTest | 20/0 |
| AlphaSMSReciprocalTest | 27/0 |
| PTGuidingMISPartitionTest | 185/0 |
| SourceHygieneTest | 169/0; 455 test files scanned |
| CstDeriveGoldenTest | 458 match, 0 drift; 465 corpus, 0 uncovered/stale |
| SMSDomainReplayTest | 8200/0 |
| SMSLegacyModeTest | 21/0 |

Evidence: `.claude/logs/sms-phase1/round2-final-gate/results.json`,
individual checked build/run logs in that directory, and
`round2-final-gate-driver.log`. The final legacy run matches all **16**
committed-master interleaved A raw hashes **and means exactly**;
`round2-final-gate/legacy-parity.json` records that independent comparison.
Final domain counters are **120 attempts / 184 Newton iterations / 118
accepted / 2 rejected / 0 retries / 0 tail events / 0 owned roots**.
These remain isolated biased solves, not production estimator evidence.
Fresh independent round-2 review is the next gate; Phase 1 is not merged.


## Fresh review round 2

Three fresh, read-only reviewers examined committed `0a2913105` against
master `ffc70c1c2`: estimator/partition, material/medium/API, and
cost/tests/documentation. All three returned **zero P1 and zero P2**.
Each independently ran the six permitted query-only modes, obtaining
finite **504/0**, raster **16/0**, composite/CSG **8/0**, polished **328/0**,
medium **480/0**, and empty-name **24/0**. No reviewer edited, rebuilt or
rendered. The evidence reviewer independently recalculated the raw cost
statistics, numerical red labels, compilation counts and exact legacy
hash/mean comparisons; historical build/render proofs rely on retained
logs, not a new reviewer render or sanitizer run.

The scoped HWSS provenance and prepared composite policy resolve round
1's two P1 findings under the user's adopted interim contract. Production
proposal/acceptance, estimators, ownership and HWSS channel geometry remain
later phases. DL-438, DL-391 and the DL-353 remainder now carry their
primitive red/green evidence in place, without closure or totals changes.
This ledger/review documentation update will receive fresh round 3 before
merge; the passing source gate is unchanged.


## Round 3 and Phase 1 merge

Fresh estimator/partition, material/medium/API and cost/tests/documentation
reviewers independently examined `7866181b6`. Each reported **zero P1 and
zero P2**, reran the six query-only modes with the same passing counts,
and verified the final ledger annotations distinguish primitive evidence
from later production gates. They made no edits, builds or renders.

Phase 1 merged from `sms-ext` to master at
`34bd520ec5e70a5cf96bcf8b8154b1a17888880f`. Main checkout status was empty
before and after the authorized merge. No rows were opened or closed;
DL-438, DL-391 and the DL-353 remainder retain their measured primitive
notes and their production obligations. Source/test trees match the final
clean gate. The merge-record documentation receives fresh round 4; no
new source behavior or timing claim is introduced by that record.
