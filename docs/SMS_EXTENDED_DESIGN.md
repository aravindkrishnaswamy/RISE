# Extended SMS: event proposals, channel geometry, and path ownership

**Status: proposed design, 2026-10-03. No extended solver is implemented by this document.**

Base: master `a8fa56224ff1e4d9284e907fcf1d1d05534530e6`, the reviewed attenuation integration. DL-437 and DL-438 remain open. The user authorized their deferral for integration and requested this extended design next. This proposal preserves the native material conventions established by [DL-435](DL435_SPECTRAL_SMS_ATTENUATION.md); it does not replace them with a general participating-medium or absorbing-film model.

## Recommended scope

Implement an opt-in extended solver in small increments: channel-aware material queries and medium replay; sampled reflection/transmission seed walks; a clean reference estimator; then shared PT/SMS ownership. Keep the current solver available as a comparison until the new estimator and partition pass independent gates. Do not turn on broader PT suppression merely because reflection seeds start finding light.

The practical unit of work is **one channel and one event chain per proposal**. A dielectric with two lobes does not create a binary tree. RGB uses real authored RGB indices; NM uses its wavelength; HWSS ultimately evaluates each lane's geometry and ownership independently. Sharing numerical results is an optimization only when their inputs agree.

The first production increment supports isolated, regular, smooth delta roots, position-independent interface IOR, deterministic visibility, bounded chain depth, and reconstructible starting medium state. Arbitrary spatial IOR, stochastic alpha, participating media, photon-guided mixtures, and rough/finite-scattering caustics require later contracts. Unsupported configurations retain the existing solver or run ordinary PT with extended SMS disabled for the entire affected anchor/domain. Choosing PT must also disable that domain's SMS contributions and suppression together.

## What is wrong today

| Finding | Current behavior | Required change |
|---|---|---|
| DL-437 | `SnellContinueChain` initially sets `isReflection = !canRefract`; below-TIR reflection from a refracting material is not independently proposed. Front-wound reflection pilot is black in RGB/NM/HWSS. | Explicit native-supported R/T proposals, including below-TIR reflection. |
| DL-438 | RGB specular metadata takes the red component of authored IOR; one scalar pair drives all three geometries. | Solve and price the selected RGB component's actual index pair. Film wavelength alone cannot fix geometry. |
| DL-391 | NM material-side refresh retains stale seeded exterior indices in nested media. | Resolve both sides from medium membership at the requested channel/wavelength. |
| DL-398 | Newton moves vertices while general spatial IOR remains seeded; gradients are absent. | Keep spatial IOR out of the initial supported domain; later add a complete material/Jacobian contract. Final-root attenuation still needs refresh. |
| DL-339(a), DL-378/379 | Some stochastic modes and HWSS bodies suppress paths more broadly than SMS coverage supports. | One ownership predicate, used by every PT/SMS entry and delegation. |
| DL-376 | Existing uniform estimates have measured over-read; deduplication and PDF accounting are not cleared. | New reference estimator with explicit averaging and controlled synthetic root tests before importing optimizations. |

These are design dependencies, not closures. DL-331's photon stack reconstruction and DL-419's medium attenuation also remain separate prerequisites for their respective later modes.

## Material and medium contract

Introduce an internal query domain, conceptually `RGBComponent(c)` or `Wavelength(nm)`. Do not silently translate an authored RGB triple into a spectrum: native RGB scattering is the oracle for DL-438. A scalar/constant triple can share geometry only after exact query equality is established. Start with deterministic equal channel allocation in preview and uniform random channel selection in the reference estimator; postpone luminance-adaptive probabilities.

A channel query returns supported events, the selected material index, reflection/interface law, and attenuation semantics at a real intersection context (UV, object/child position, normals, object identity). Retain neutral defaults for extensions that do not implement optional queries. A legacy metadata provider that cannot supply the selected domain is ineligible for extended suppression; guessing its other indices is forbidden. Keep new internal records separate from public `SpecularInfo`/`ManifoldVertex` until the API/ABI impact is reviewed; any public virtual extension must be optional, tail-appended, and neutral by default.

Medium membership and evaluated index are distinct. Store stable medium/object identities and the native crossing policy in the chain's starting state, then evaluate those media in the requested domain. Reflection preserves membership. Transmission applies the shared native push/pop/open-sheet/CSG rule. Exiting a nested solid restores the enclosing medium, not the previous vertex's scalar `etaT`. A medium absent from the chain must still be resolved from starting membership. If current public `IORStack` contains only scalar values without enough identity/query information, this phase must extend the internal capture/replay contract rather than inventing an index.

Use true unflipped geometric normals for side classification; shading normals remain constraint inputs under the existing validated convention. A chosen R event must not change `isExiting` to force a usable normal. Rebuild ordered `etaI/etaT` at each requested domain, including HWSS companion replay, and validate native TIR decisions. At TIR, choose only R with probability one; never sample impossible T and silently relabel it R while preserving T's proposal probability.

After Newton convergence, reconstruct full intersection contexts and refresh attenuation and native Fresnel at solved incidence. Re-resolve constant-domain indices and reject inconsistent topology/membership. Position-dependent attenuation is compatible with fixed-IOR geometry once queried at the root; arbitrary spatial IOR is not. Supporting spatial IOR later requires material-index derivatives in residual/Jacobian evaluation, not just a final query. The native perfect-refractor tint remains transmission-only; dielectric interior transmittance retains its current exiting-segment convention. Segment medium extinction is a separate later feature.

## Seed proposal

Given fixed anchor, sampled emitter point, and channel, select a caster using a normalized distribution with positive mass for every eligible caster. Initially use uniform selection. Sample its surface using the existing uniform-surface API, then trace one chain, sampling supported delta events at each hit. No enumeration of all event masks.

Below TIR, use a native-Fresnel-informed R/T probability with a configured positive exploration floor for each supported nonzero lobe. The floor changes proposal variance, not physical throughput; final contribution always uses the native event law. Distinguish a material that supports only T from one that supports both R/T. Zero-contribution lobes can be omitted only when zero is established throughout the supported domain; a zero at the seed does not prove a zero at the root. Mixed chains, such as R-T-R, are first-class records. Unsupported hits, misses, wrong first caster, too-long chains, singular solves, and failed validation are ordinary zero trials.

Newton keeps the proposed object order and event topology fixed. An iterate that changes side/topology or becomes TIR-inconsistent is rejected or handled by a later explicitly defined proposal; it cannot be quietly repaired into a differently priced root. Initial event selection can use the seed incidence, but retries must replay the same complete algorithm, including caster choice, event probabilities, alpha policy, and acceptance tests.

Retain endpoint-aware Snell seeds as a future mixture component, not as an unaccounted extra root sum. For the first reference version use only the uniform generator. Photons are deferred until their ordered medium records are correct and their mixture component can be reproduced during probability estimation.

## Reference probability accounting

For fixed endpoint context and selected channel c, let the complete proposal/solve/acceptance algorithm return root r with probability p(r|c). This probability includes caster selection, surface seed, all discrete R/T choices, and failed solves. Let C(r,c) be that root's physical SMS contribution including geometry, BSDF, native event attenuation and emitter radiance, but excluding root-selection and channel probabilities. Keep the actual emitter sampling density as a separate factor `pL`, including emitter selection and the point measure already consumed by chain geometry. Preserve its conditional domain and conventions; do not substitute an area density for a solid-angle density or vice versa. Native SPD light-selection policy remains DL-396 and is not silently redesigned here.

Choose c with `q(c)=1/3` for RGB. Run one full proposal. A failed proposal contributes zero. For an accepted root, independently repeat the **same conditional-channel proposal** until the same root is rediscovered. If K is the first-success count, its expectation is `1/p(r|c)`. Deposit only component c with weight:

`C(r,c) * K / (q(c) * pL)`.

Average over the original number of trials N, including zero trials. Do not separately divide by event/caster probabilities: they are already inside p. Do not include channel sampling inside retries and also divide by q. NM has q=1 for its fixed wavelength; HWSS lane/wavelength weighting remains owned by its existing spectral estimator and must not be added twice.

For RGB, the conditional expectation is `sum_r p(r|c) C(r,c) / p(r|c)`; averaging channel choices cancels q, leaving each component's root sum divided by pL. This argument assumes independent retries, positive root probability, and an unchanged proposal/acceptance law. A deterministic discovery salt must not be reused as the retry seed.

Use independently seeded pseudorandom substreams for reference proposal and rediscovery trials; applying a deterministic Sobol sequence directly to the K loop does not establish the geometric independent-trial law. Render-level ValueSalt still separates repeated measurements. A later quasi-Monte-Carlo retry scheme needs its own expectation argument.

No root deduplication in this reference estimator. Repeated independent discoveries are separate Monte Carlo trials. In contrast, bounded preview may deduplicate a fixed per-channel budget and sum each root's physical C once; that is a coverage-biased root sum. It does not receive `1/q` or `1/p`, and must be labeled biased. The two estimators must not share a vaguely named weight field.

The geometric rediscovery identity follows the original SMS construction, but the extension's joint proposal and channel conditioning are this repository's proposed contract. The [authors' paper and corrected reference implementation](https://rgl.epfl.ch/publications/Zeltner2020Specular) distinguish biased/unbiased variants and document a 2021 angle-periodicity correction; use the corrected reference if constraint formulations are ported.

An uncapped K estimator has unbounded worst-case cost. A hard retry cap that discards its tail is biased. For responsive preview, impose explicit solve/work limits and report lost/unfinished proposals. For reference mode, first use the uncapped estimator in offline tests with external cancellation; then separately validate a Russian-roulette survival-weighted tail before production use. Existing `SMSReciprocalTail` is used on alpha-enabled uniform retries, while ordinary uniform retries have a hard-cap zero-bias path; its presence is not proof that all current modes are unbiased. Do not copy that mixed implementation into the new reference path.

Reference C must disable geometric/radiance clamps and any unweighted work-based discard; otherwise label the result biased. Newton convergence/validation still defines the supported numerical root set, and full physical coverage is a separate gate. Finite variance is not guaranteed for rare roots even with an unbiased mean. Report retry quantiles, cap/roulette incidence and outliers alongside image means. Cancellation is an aborted measurement, not a zero sample or a passing render.

## Shared path ownership

Do not use “SMS found a root this sample” to suppress PT. Discovery is random, while ownership must agree between both estimators at identical endpoint/path context.

Recommended initial partition: a deterministic, bounded canonical-root predicate conditioned on the **actual** ordered object/event topology and query domain. For a given anchor/emitter pair, medium state and topology, construct a small fixed set of canonical seeds (begin with one) using deterministic surface coordinates keyed by that full context, solve them, and define owned roots as their validated results. This is an extension of the existing deterministic coverage idea, not enumeration of every possible event mask. Evaluate only the topology of the candidate SMS root or actual PT emitter hit.

SMS contributes a sampled root only if the canonical predicate owns it. PT suppresses a delta-chain emitter hit only if the identical predicate owns that full chain in that channel. Other roots remain with PT. Thus a random seed's failure does not create a hole, and a new reflected root does not automatically remove the corresponding PT path. The uniform generator must have positive basin probability for every canonical-owned root; regular isolated roots and reproducible acceptance are prerequisites. Degenerate/continuous solutions are outside this mode.

Root identity must include query domain, ordered objects, events, crossing/membership signature, chain length and all vertex geometry. Comparing only first direction or first position is insufficient. Specify one shared root-equivalence policy, residual acceptance and ambiguity handling; uncertain classification assigns the path to PT and rejects its SMS contribution. Hashes accelerate lookup but do not establish equality. Tolerance/separation behavior remains a numerical implementation proof obligation, with close-root adversarial tests and scale changes; this proposal claims no exact floating-point partition theorem.

Ownership inputs may not depend on mutable seed history, discovered-root sets, per-render stochastic salts, caches or retry budget. A cache can store the deterministic answer but cannot decide it. Start without cross-anchor caches; key any later cache by complete immutable context and keep per-worker storage bounded.

For RGB PT, ownership can differ per component; emission weighting must mask components separately. HWSS must carry lane-specific ownership through its own loop and every NM delegation; one hero decision cannot suppress all companions. This may require a deeper HWSS refactor. Until that passes, extended HWSS stays off rather than retaining today's blanket suppression. Limit the first partition to truly delta events and eligible non-delta anchors; finite-scattering polished/SSS cases need a separate treatment. Stochastic alpha is excluded because deterministic endpoint ownership must also agree with its visibility measure.

The initial combined PT/SMS partition is limited to non-delta area emitters that ordinary PT can actually reach. Analytic point/spot lights need a separate singular-light contract: ordinary PT emitter hits cannot be assumed to estimate their delta caustics. Use the reflection-only spot fixture as a standalone SMS/analytic or light-tracing reference test, not as proof of the area-emitter fallback. A later point/spot mode can assign the entire supported smooth-delta family to SMS only after establishing positive proposal support and correct light measure; canonical-root filtering would otherwise discard roots that PT cannot recover. Unsupported analytic-light configurations keep their pre-extension behavior, explicitly documented; this proposal does not promise that switching to PT repairs those caustics.

Canonical solves add cost per candidate emitter hit and SMS root. Their count is bounded by the fixed seed count per classification, but ray traversal and Newton iteration costs must be measured. Increasing canonical seed count expands owned support; it must remain a static policy, never adapt to which paths PT happened to sample. A future alternative is full PT/SMS MIS with actual strategy densities; do not invent those densities from a successful discovery count.

## Work and memory policy

Use explicit worker-local scratch vectors sized to configured maximum depth; one live seed and one retry chain per trial. Avoid three resident RGB chains by choosing a component per reference trial. Preview allocates a fixed budget across components; constant-index inputs may share solved geometry after equality checks, while attenuation stays component-specific.

Expose internal counters before tuning: attempts, selected event counts, ray intersections, Newton iterations, accepted/owned/rejected roots, canonical solves, rediscovery trials, tail events, scratch peak bytes and material queries. The intended implementation avoids explicit exponential event-tree enumeration; that is an algorithm choice, not a measured runtime bound. Newton can still struggle, rare roots can require arbitrarily many retries, and classification can dominate. No speedup or whole-render cost guarantee is claimed here. Baseline and candidate measurements must use identical worker policy, salts, spp, ROI and build configuration, and sequential rendering. Evaluate make, Deployment and Opto; include ABI/record size and CPU memory reports if layouts change.

Keep public knobs minimal: initially one opt-in extended mode and existing depth/trial budgets. Do not overload `branching_threshold`, whose PT/BDPT semantics differ. Separate preview work limits from reference estimator settings internally; parser/API exposure waits for reviewed semantics and CST/API parity tests.

## Implementation sequence and acceptance gates

1. **Domain and medium replay.** Build native RGB per-component and NM index oracles; nested outer/inner media, start-inside and open sheets; final-root tint/AR checks. Preserve legacy-neutral providers. Resolve DL-438 and DL-391 only after measured gates, not after adding a field.
2. **R/T proposal and reference estimator in isolated tests.** Synthetic two-root/three-event distributions with known probabilities and zero trials; verify averaging, channel factors, no doubled event PDFs, and rare-root tails. Reflection-only upward-spot DL-437 against an analytic/light-tracing reference, an area-light counterpart for PT partition tests, mixed R-T/R-T-R, TIR transitions, mirror-only and transmission-only controls. Establish root identity and finite-difference constraint/Jacobian checks.
3. **Partition, then RGB/NM production opt-in.** Start with area emitters. Compare SMS-owned and PT-kept contributions separately and together. Prove no dependence on trial budget or discovery order. Validate all integrator entry points, SPF/BSDF and SSS delegations, and emitter sidedness. Keep unsupported anchors entirely in PT for this mode. DL-376/339 residuals need their own red proofs.
4. **HWSS lane geometry and ownership.** Distinct companion indices, nested exterior replay, per-lane TIR and emitter masks; no hero-only reuse unless equivalence is proven. Close DL-378 only on its actual BSDF-delta body plus delegated paths.
5. **Optional extensions.** Photon proposal mixtures after DL-331; stochastic alpha after a visibility/partition design; spatial IOR after DL-398 derivatives; participating media after DL-419 segment laws; guiding after proposal replay and density accounting are stable. None is implied by phase 1–4.

Every crossing/event/index phase must include real double-sided `indexedmesh_geometry`, both windings, both incidence sides, closed and open geometry, nested exits and start-inside. Include sphere/plane controls and transformed instances; synthetic vertices alone are insufficient.

For each fix: commit; restore committed master sources and dependent headers coherently; build one targeted executable with checked exit and warnings; observe the intended failure; restore HEAD and rebuild green. Render bands use at least four independent `SobolSamplerTestHooks::ValueSalt` salts and at least three standard deviations under the repository's documented metric. Use channel-separated radiance and centroids for dispersion, not the biased max-channel RegionMean metric (DL-433). Reference PT/VCM must themselves match the tested material domain and have uncertainty reported; do not substitute a single noisy PT render for ground truth.

After each coherent production increment, apply [the independent review loop](skills/implementation-review-loop.md), with separate estimator/partition, material/medium/API and cost/doc-fidelity lenses. No merge until a fresh final tree has zero P1, followed by the targeted master gate. Stop and record any new design decision outside this proposal before changing that contract.

## Integration evidence

The attenuation repair was integrated with `git merge --no-ff`; master `a8fa56224` has exactly the reviewed `5481c0ead` tree. Two fresh integration reviewers returned zero P1/P2. No push was performed. The post-merge targeted master gate passed all 12 individually built executables and three additional SMS modes, with zero compiler warnings. Builds were checked before running each binary, renders ran sequentially with RISE_MEDIA_PATH set, and the exact master checkout was clean before and after. This design introduces no source changes and claims no extended-SMS render results.

| Master gate | Result |
|---|---|
| SourceHygieneTest | 169 passed / 0 failed |
| CstDeriveGoldenTest | 458 MATCH; 0 DRIFT / UNCOVERED / STALE |
| ManifoldSolverTest | DL-435/439 388 passed; legacy and photon-context checks pass |
| ExteriorIndexInvarianceTest --unit-only | 125 / 0 |
| SMSUniformDispersionTest --attenuation-only | 96 / 0 |
| SMSUniformDispersionTest --interface-only | 18 / 0 |
| SMSUniformDispersionTest --coating-only | 72 / 0 |
| SMSUniformDispersionTest --shipped | 10 / 0 |
| OIDNAutoDeterminismTest --policy-only | 176 / 0 |
| SSSRadianceScalingTest | 576256 / 0 |
| DoubleSidedEmitterTest | 34 / 0 |
| FrameStoreTest | 123 / 0 |
| RasterizerDefaultsConsistencyTest | 164 / 0 |
| AgentEvalCheckTest | 2075 / 0 |
| PTGuidingMISPartitionTest | 185 / 0 |

Local gate records: `/tmp/rise-cheapbatch-master-gate.json`, driver `/tmp/rise-cheapbatch-master-gate-driver.log`, per-command logs `/tmp/rise-cheapbatch-master-*.log`. Those temporary paths are diagnostic evidence on the originating machine; the results above are the durable record. Earlier full, sanitizer, clean make and Deployment/Opto evidence remains in [DL_CHEAPBATCH_VALIDATION.md](DL_CHEAPBATCH_VALIDATION.md). The design-only branch passes whitespace/link checks and retains 289 unique ledger rows, 46 open / 243 closed, highest ID 440; no row was closed by writing this proposal.
