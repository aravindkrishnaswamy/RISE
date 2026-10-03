# Extended SMS Phase 1 validation (2026-10-03)

Status: implementation and acceptance gate in progress on `sms-ext`; no phase merge or ledger closure is claimed.

## Scope and rejection contract

`SMSDomainReplay` and `ManifoldSolver::SolveDomain` are isolated native-domain primitives. RGB queries use the authored component; NM queries evaluate both medium sides at the requested wavelength. Starting medium captures preserve identities and intersection context. Reflection preserves membership; transmission uses native closed-object and certified DL-345 sheet crossing policies. Solved roots refresh native tint, absorption, Fresnel and AR coatings from real intersections, retaining caller raster coordinates.

Composite queries fail. Starting composite membership and uncertain DL-407 containment fail eligibility; contribution, PT emitter suppression and delta-light shadow opacity use the same anchor eligibility. A world-visible CSG owner with inherited composite boundaries is inspected through its operands. The conservative bounds rejection may reject anchors outside the actual solid but inside its bounds; it never claims positive composite replay. Positive composite replay needs a separate walker estimator and DL-407 repair.

Positive extended production estimators are not enabled in this phase. Default RGB/NM retain the existing estimator; HWSS ignores the extended flag with one warning and its NM delegations retain legacy eligibility. Production channel/medium correctness and HWSS debt closures require their later activation gates. No target row closes merely because these primitives pass.

## Numerical evidence

`SMSDomainReplayTest` exercises double-sided indexed meshes with both windings and incidence sides, open and closed shapes, transformed instances, sphere/plane controls, nested exits, start-inside, and an enclosing SF11 medium absent from the solved chain. Independent Sellmeier, Snell and single-film Airy oracles check indices and pricing. Composite PT comparisons use four salted trials, paired per-channel differences and a three-standard-error band (3 × sample SD / sqrt(4)); deterministic camera-to-emitter paths require exact raw hashes.

At `8172ad28c`, the complete domain suite passed **2917/0**, reporting **120 attempts, 184 Newton iterations, 118 accepted and 2 rejected roots**. These isolated biased solves have **0 retries, 0 tail events and 0 owned roots**; they do not test rediscovery or canonical ownership.

Committed master `ffc70c1c2` sources plus all dependent changed headers, built coherently against the committed regression test, produce **14 passed / 10 failed**. Failures are two component Snell roots, three root tint/Fresnel weights, four pixel-context RGB/NM weights and one nested spectral exterior. A compile-time feature marker selects actual master `BuildSeedChain`, `Solve`, `BuildSnellBaseSeed` and `EvaluateChainThroughputNM` calls because master has no `SolveDomain`; compilation failure is not counted as a red proof. Logs: `.claude/logs/sms-phase1/latest-master-{build,test-build,red}.log`.

Additional committed-helper regressions:

- Finite-Phong query eligibility before correction: **168/96**; corrected query-only suite **504/0**. Exponents 1e4 and 1e5 are admitted at their native delta-limit approximation; the query records that approximation. HG partial coherent mixtures remain unsupported. This is a new helper defect, not a claimed master defect.
- Pixel-context isolated corrected fixture: **16/0**. The initial diagnostic had one bad setup assertion (the existing `AssignMaterial` return value), which is excluded from numerical evidence; the committed-master proof above compares actual native material queries at caller raster coordinates.
- Inherited composite CSG boundaries at committed `ac7802caa`: **6/2**, both failures are missing rejection for start-inside membership. Corrected complete domain suite at `7e66dc267`: **2925/0**, with the same 120/184/118/2 diagnostics. The two missing rejection assertions now pass. Complete phase acceptance remains pending below.

The polished native-SPF audit is a positive law oracle, not a bug fix: the corrected fixture passes **328/0** on committed pre-audit helper sources. The initial fixture assumed the native black substrate emitted no diffuse ray; native SPF emits a zero-weight diffuse ray too. Its count-based failures are not numerical red evidence. `SpecularInfo` already defaults reflection tint to true, so the suspected pricing defect was disproved and the redundant source change removed.

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

On arm64, the diagnostic reports bytes: `SpecularInfo` 56, `ManifoldVertex` 336, `SMSChainRecord` 2176, internal `ManifoldSolverConfig` 96, `SMSDomainVertex` 1584, `SMSMediumCapture` 1264, `SMSStartingMedia` 40, and `SMSDomainCounters` 32. Public specular/vertex/chain layouts are unchanged; the config adds internal opt-in/diagnostic fields. Capture scratch holds one identity/context per enclosing object, plus temporary scene-object enumeration. This is not the later proposal scratch-budget measurement.

Clean make: **376 compilation actions, exit 0, zero warnings/errors** (`final-clean-library-build.log`). Clean Xcode RISE-GUI Deployment and RISE-GUI-Opto builds: **393 CompileC actions each, exit 0, zero compiler warnings/errors**. Each emits exactly the two documented checkout/tool notices: missing `extlib/oidn/install/lib` linker search path and skipped AppIntents extraction. These are excluded under AGENTS.md's explicit instructions; no compiler warning was suppressed. Xcode source state matches the final helper: the intervening redundant polished flag change was removed.

## Acceptance gate

The full individually built regression gate and fresh three-lens review remain in progress. The updated domain test additionally checks bit-identical HWSS lane outputs on the real composite fixtures with the extended flag ignored; its final count is pending the serial gate. Clean make and Xcode Deployment/Opto build gates have passed. Final results and merge provenance must replace this status before a phase is marked implemented.
