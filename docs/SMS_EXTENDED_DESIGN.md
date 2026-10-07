# Extended SMS: event proposals, channel geometry, and path ownership

**Status: adopted implementation contract. Phase 1 merged at `34bd520ec5e70a5cf96bcf8b8154b1a17888880f`. Phase 2 remains unmerged on `sms-ext-phase2`: cached recursion/signed-return repairs and the full Round 17 gate pass at `97d2e2ff68c25d8dc22feea2e4f83b3d74139778`. Fresh Round 17 review remains required. Pre-existing octree-accounting P2s are measured in DL-441/DL-442. Phases 3–4 remain pending; earlier audit sections are historical.**

Base: master `a8fa56224ff1e4d9284e907fcf1d1d05534530e6`, the reviewed attenuation integration. At that base, DL-437 and DL-438 remained open. The user authorized deferring them for that integration and asked for this extended design next. This proposal keeps the native material conventions established by [DL-435](DL435_SPECTRAL_SMS_ATTENUATION.md). It does not replace them with a general participating-medium or absorbing-film model.

## Recommended scope

Implement an opt-in extended solver in small increments:

1. channel-aware material queries and medium replay;
2. sampled reflection/transmission (R/T) seed walks;
3. a reference estimator, first for delta lights, where SMS has no PT partner;
4. shared PT/SMS ownership for area emitters.

Keep the current solver available as a comparison until the new estimators and the partition pass independent gates. Do not broaden PT suppression merely because reflection seeds start finding light.

The practical unit of work is **one channel and one event chain per proposal**. A dielectric with two lobes does not create a binary tree.
- RGB uses the real authored per-component indices.
- NM uses its wavelength.
- HWSS ultimately evaluates each lane's geometry and ownership independently.

Sharing numerical results between channels is an optimization, valid only when their inputs are equal.

The first production increment supports:
- isolated, regular roots that a smooth delta chain admits;
- position-independent interface IOR;
- deterministic visibility;
- bounded chain depth;
- a starting medium state that can be reconstructed.

Arbitrary spatial IOR, stochastic alpha, participating media, photon-guided mixtures and rough casters need later contracts. Finite-scattering casters are handled as described under [Eligibility](#eligibility-and-the-three-coupled-switches).

## SMS in RISE today (verified against `a8fa56224`)

The design builds on these facts. Code references are to that commit.

- **Integrators.** Only the path tracer evaluates SMS: PT Pel/Spectral/HWSS through `PathTracingIntegrator` and `PathTracingShaderOp`, at PART 2 surface anchors (`PathTracingIntegrator.cpp:1488`, HWSS `:6734`).
  - The legacy `SMSShaderOp` also calls the solver, but it has no PT suppression partner.
  - BDPT, VCM and MLT do not use SMS; no MLT source references `ManifoldSolver`, and the MLT factories take no SMS configuration. CLAUDE.md's older "PT, MLT still use SMS" statement is stale and is corrected alongside this design.
- **Environment lights.** SMS ignores them: all four `ManifoldSolver` entry points return early on `lightSample.pEnvLight` (`ManifoldSolver.cpp:6352, 7326, 7772, 8180`). PT's environment escape path is not gated by SMS suppression, so environment caustics remain PT's. This design keeps that.
- **R events at refracting casters.**
  - `SnellContinueChain` starts a refracting vertex as `isReflection = !canRefract` and promotes it to R only at TIR (`ManifoldSolver.cpp:4202, 4479`). Snell and uniform seeds therefore never propose a below-TIR reflection from a refracting material (DL-437).
  - Photon-seeded chains already carry R events at refracting materials (`SMSPhotonMap.cpp:307, 401`, consumed at `ManifoldSolver.cpp:6030, 6861`).
- **RGB index.** `DielectricSPF.h:142` and `PerfectRefractorSPF.h:71` publish `GetValuesAt(ri).v[0]`: one red-component index drives every RGB constraint and Fresnel term (DL-438).
- **NM exterior.** NM refresh replaces only the material side and keeps the seeded exterior index (`ManifoldSolver.cpp:7813-7838`, DL-391). Newton moves vertices while a spatial IOR stays at its seed value (DL-398).
- **Uniform unbiased retries** (`ManifoldSolver.cpp:7558-7612`).
  - With alpha, retries use `SMSReciprocalTail` (tested by `AlphaSMSReciprocalTest`).
  - Without alpha, a hard retry cap drops the root when it fires (`if( capHit ) continue;`), which biases the estimate toward zero.
  - `SMSLoopSampler` (`ManifoldSolver.cpp:3719`) already isolates retry draws from the path sampler's dimensions.
- **Existing split suppression (debt-smssplit).**
  - `ManifoldSolver::ClassifyEmitterHitCoverage` (`ManifoldSolver.cpp:8633`) re-runs SMS's own endpoint-aware Snell seed and solve from the recorded anchor toward the hit emitter point. PT suppresses the hit only if that deterministic solve reaches the same chain; an "unknown" result suppresses.
  - It applies only where `SplitSuppressionExact` holds: snell seeding, biased, no photons, no alpha; RGB additionally needs no pure-mirror caster.
  - It applies to area emitters only (`PathTracingIntegrator.cpp:2745`), and the chain record holds at most 16 vertices (`ManifoldSolver.h:381`).
  - It is consistent because the SMS estimator in that mode is itself deterministic in (anchor, emitter point): what SMS finds and what PT suppresses are the same set.
- **Delta-light shadow rule.** DL-344's `bSMSCoversDeltaLights` makes a delta light's NEE shadow ray opaque at dielectrics, but only at a PART 2 point where a solver is present (`PathTracingIntegrator.cpp:1294`). DL-413 builds a point light's Jacobian plane perpendicular to the last chain segment.
- **`IORStack`.** It stores object keys (`IORStack.h:78`); composite layers use per-instance keys and the environment entry has no object. It stores no crossing position or UV context.

## What is wrong today

| Finding | Current behavior | Required change |
|---|---|---|
| DL-437 | Snell/uniform seeds never propose below-TIR reflection at a refracting material. The front-wound reflection pilot is black in RGB/NM/HWSS. | Explicit native-supported R/T proposals, including below-TIR reflection. |
| DL-438 | RGB metadata takes the red component of an authored IOR triple; one scalar pair drives all three geometries. | Solve and price the selected component's actual index pair. A film wavelength alone cannot fix geometry. |
| DL-391 | NM material-side refresh keeps stale seeded exterior indices in nested media. | Resolve both sides from medium membership at the requested channel/wavelength. |
| DL-398 | Newton moves vertices while a general spatial IOR stays seeded; gradients are absent. | Keep spatial IOR out of the initial supported domain; a later contract adds material-index derivatives. Final-root attenuation still needs refresh. |
| DL-312 | NM snell mode lacks RGB's pure-mirror caster supplemental seeds. | Subsumed by the uniform R/T generator, which treats mirrors as ordinary R-only casters in every channel. |
| DL-339(a), DL-378, DL-379 | Some stochastic modes and the HWSS body suppress more broadly than SMS coverage supports; finite-scattering casters are partitioned at the delta limit. | One ownership predicate, used by every PT/SMS entry and delegation (see [Eligibility](#eligibility-and-the-three-coupled-switches) for DL-379). |
| DL-353 (remainder) | Uniform spectral dispersion is partial; the residual is DL-391/398. | Closed by the channel/medium phase, not separately. |
| DL-376, DL-421 | Uniform estimates over-read on DL-372's scene and read about 4.5 % low through a glass slab. Deduplication, PDF accounting and the hard cap are not cleared. | New reference estimator with explicit averaging; DL-376 and DL-421 become its validation fixtures. |
| DL-420 | Snell SMS reads +1.6..+1.8 % high for a spot light through a refracting caster. | Becomes a fixture of the delta-light reference phase. |
| DL-352 | Newton stalls on displaced casters. | Not an estimator defect: displaced and Phong-shaded meshes are eligible, but their low root coverage is reported, not hidden. |

These are design dependencies, not closures. DL-331's photon stack reconstruction and DL-419's medium attenuation remain separate prerequisites for their later modes; DL-396 (an RGB-black spectral emitter) is out of scope.

## Material and medium contract

Introduce an internal query domain, conceptually `RGBComponent(c)` or `Wavelength(nm)`.
- Do not silently translate an authored RGB triple into a spectrum. Native RGB scattering is the reference behaviour for DL-438.
- When all three RGB components return identical query results (index, event law and attenuation equality established at the anchor and at every vertex), the trial may solve once and deposit all three components with `q = 1`. Otherwise, choose one component per reference trial (below).

A channel query returns, at a real intersection context (UV, object/child position, normals, object identity):
- the supported events;
- the selected material index;
- the reflection/interface law;
- the attenuation semantics.

Rules for providers and records:
- Retain neutral defaults for extensions that do not implement optional queries.
- A legacy metadata provider that cannot supply the selected domain is ineligible for extended suppression; guessing its other indices is forbidden.
- Keep new internal records separate from public `SpecularInfo`/`ManifoldVertex` until the API/ABI impact is reviewed. Any public virtual extension must be optional, tail-appended and neutral by default.

Medium membership and evaluated index are distinct.
- Store stable medium/object identities and the native crossing policy in the chain's starting state, then evaluate those media in the requested domain.
- Reflection preserves membership. Transmission applies the shared native push/pop rules, including the DL-345 open-sheet face rule (`IORStackSeeding::ResolveOpenSheetCrossing`). Composite membership is rejected under the adopted exclusion below; its per-instance keys are not replayed in this domain.
- Exiting a nested solid restores the enclosing medium, not the previous vertex's scalar `etaT`.
- A medium absent from the chain must still be resolved from starting membership.
- `IORStack` carries object keys but no evaluation context. Phase 1 must add an internal capture/replay record holding identity plus the context needed to evaluate each enclosing medium in the requested domain, rather than inventing an index.
- A medium whose `ior` is a world-position field (DL-09 `GradedIndexMedium`) is spatial IOR and is ineligible in the first phases.

Sides, TIR and refresh:
- Use true unflipped geometric normals for side classification. The original shading-normal-only constraint convention is disputed by the Phase 2 round 2 native-SPF counterexample below; modified-interface support follows the adopted native fallback correction below.
- A chosen R event must not change `isExiting` to force a usable normal.
- Rebuild ordered `etaI/etaT` in each requested domain, including HWSS companion replay, and validate native TIR decisions.
- At TIR, choose only R, with probability one. Never sample an impossible T and silently relabel it R while keeping T's proposal probability.

After Newton convergence:
- Reconstruct full intersection contexts and refresh attenuation and native Fresnel (including DL-436 AR coatings) at the solved incidence.
- Re-resolve constant-domain indices and reject inconsistent topology or membership.
- Position-dependent attenuation is compatible with fixed-IOR geometry once queried at the root; arbitrary spatial IOR is not. Supporting spatial IOR later requires material-index derivatives in the residual/Jacobian evaluation, not just a final query.
- The native perfect-refractor tint remains transmission-only, and dielectric interior transmittance keeps its current exiting-segment convention. Segment medium extinction is a separate later feature (DL-419).

## Seed proposal

Given a fixed anchor, a sampled emitter point and a channel:
1. Select a caster using a normalized distribution with positive mass for every eligible caster; initially uniform.
2. Sample its surface with the existing uniform-surface API.
3. Trace one chain, sampling a supported delta event at each hit. There is no enumeration of event masks.

Event probabilities:
- Below TIR, use a native-Fresnel-informed R/T probability with a configured positive exploration floor for each supported nonzero lobe. The floor changes proposal variance, not physical throughput; the final contribution always uses the native event law.
- Distinguish a material that supports only T from one that supports both R and T.
- A zero-contribution lobe may be omitted only when zero is established throughout the supported domain. A zero at the seed does not prove a zero at the root.

Chains and failures:
- Mixed chains such as R-T-R are first-class records.
- Unsupported hits, misses, a wrong first caster, too-long chains, singular solves and failed validation are ordinary zero trials.

Topology:
- Newton keeps the proposed object order and event topology fixed.
- An iterate that changes side or topology, or becomes TIR-inconsistent, is rejected. It cannot be quietly repaired into a differently priced root.
- Initial event selection may use the seed incidence, but retries must replay the same complete algorithm: caster choice, event probabilities, alpha policy and acceptance tests.

Existing knobs, in the extended mode:

| Knob | Role |
|---|---|
| `sms_target_bounces`, `sms_max_chain_depth`, emitter-stop | Acceptance filters; they are part of the proposal law and must be applied identically by the ownership predicate. |
| Solver threshold, `sms_two_stage`, Levenberg-Marquardt | Deterministic solver configuration; allowed, but part of both the proposal and the predicate. |
| `sms_multi_trials` | The trial count N. |
| `sms_biased` | Selects the preview/biased estimator (below); the reference estimators ignore it. |
| `sms_seeding` | Selects the existing snell/uniform modes, which remain available unchanged. |
| `sms_photon_count` | Excluded until DL-331. |
| `maxGeometricTerm` clamp | Disabled in reference mode (see below). |

Endpoint-aware Snell seeds remain a future mixture component, not an unaccounted extra root sum. The first reference version uses only the uniform generator. Photons are deferred until their ordered medium records are correct and their mixture component can be reproduced during probability estimation.

## Estimators

Three estimators are defined. They must not share a vaguely named weight field.

### A. Root-level reference estimator (standalone ownership: delta lights)

For a fixed endpoint context and selected channel c:
- Let the complete proposal/solve/acceptance algorithm return root r with probability p(r|c). This includes caster selection, the surface seed, every discrete R/T choice and failed solves.
- Let C(r,c) be that root's physical SMS contribution: geometry, BSDF, native event attenuation and emitter radiance, excluding root-selection and channel probabilities.
- Keep the actual emitter sampling density as a separate factor `pL`, including emitter selection. For a delta light it is the selection probability only. Preserve its conditional domain and conventions; do not substitute an area density for a solid-angle density or vice versa.

Procedure:
1. Choose c with `q(c) = 1/3` for RGB (or deposit all components with q = 1 under the equal-query rule).
2. Run one full proposal. A failed proposal contributes zero.
3. For an accepted root, repeat the **same conditional-channel proposal** independently until the same root is rediscovered. If K is the first-success count, its expectation is `1/p(r|c)`.
4. Deposit only component c with weight `C(r,c) * K / (q(c) * pL)`.
5. Average over the original N trials, including zero trials.

Why this is correct:
- For RGB the conditional expectation is `sum_r p(r|c) C(r,c) / p(r|c)`. Averaging over channel choices cancels q, leaving each component's root sum divided by `pL`.
- The argument assumes independent retries, positive root probability and an unchanged proposal/acceptance law.

Accounting rules:
- Do not divide separately by event or caster probabilities; they are already inside p.
- Do not include channel sampling inside the retries and also divide by q.
- NM has q = 1 for its fixed wavelength. HWSS lane/wavelength weighting stays with its existing spectral estimator and must not be added twice.

Retry independence:
- Retries use independently seeded pseudorandom substreams. `SMSLoopSampler`'s isolation is the starting point, with the retry seed derived independently of the discovery draw. A deterministic discovery salt must not be reused as the retry seed.
- Applying a deterministic Sobol' sequence directly to the K loop does not establish the geometric independent-trial law. Render-level `ValueSalt` still separates repeated measurements.

No root deduplication: repeated independent discoveries are separate Monte Carlo trials.

This estimator owns its roots outright. It is the correct first production target for **delta lights** (point and spot, including DL-437's own fixture and DL-420), where PT has no partner strategy for the caustic. There:
- no ownership filter is needed;
- DL-344's `bSMSCoversDeltaLights` rule already makes the delta light's shadow ray opaque at SMS-evaluated points;
- the only coupling is that the extended mode must set that flag exactly where it evaluates.

Area emitters use estimator B or C, never A with PT suppression on.

### B. Topology-level estimator (partition mode: area emitters)

Under a canonical ownership predicate (next section), the owned roots for a topology T are exactly the canonical solve results O(T). Rediscovering each root is unnecessary. Instead:
1. Run one proposal walk; it yields a topology T (object order, events, membership signature) or a zero trial.
2. Solve the canonical seeds of T deterministically and sum `C(r,c)` once over the distinct owned roots r in O(T).
3. Estimate `1/P(T|c)` with K = the number of independent seed walks (no Newton) until the walk yields T again.
4. Deposit `[sum over r in O(T) of C(r,c)] * K / (q(c) * pL)`, averaged over N including zeros.

Its expectation is `sum_T C(O(T))`, the owned set, with variance governed by 1/P(T) ≤ 1/p(r). Retries cost ray traversal only.

Requirements:
- Ownership is decided before the K loop; a walk whose canonical set is empty contributes zero and enters no loop.
- Every topology with a nonempty owned set needs positive walk probability. This is much weaker than estimator A's need for positive basin probability for each root.
- The partition-eligible proposal must be one whose walk probability `P(T|c)` is reproducible: the same caster distribution, event law and filters.

### C. Preview (bounded, biased)

A preview may spend a fixed per-channel budget, deduplicate, and sum each found root's physical C once. That is a coverage-biased root sum; it receives no `1/q` or `1/p` and must be labelled biased.

**A stochastic preview must never be combined with ownership suppression.** It contributes about `P(found r) * C(r)` per owned root while PT suppresses all of `C(r)`, so the missing `(1 - P) * C` is lost at every spp. Hence:
- a preview runs with suppression **off**, and PT keeps all emitter hits;
- or it uses today's deterministic split mode, where the preview's proposal **is** the canonical seed set, so found and owned coincide.

### Tails, clamps and diagnostics

- An uncapped K has unbounded worst-case cost, and a hard cap that drops its tail is biased toward zero, which is today's ordinary-uniform behaviour.
- For reference mode, first use the uncapped estimator in offline tests with external cancellation. Then validate a Russian-roulette, survival-weighted tail before production use; `SMSReciprocalTail` is the candidate.
- Do not copy today's mixed alpha/non-alpha retry implementation.
- Reference estimators disable the geometric/radiance clamps and any unweighted work-based discard; otherwise the result is labelled biased.
- Newton convergence and validation still define the supported numerical root set; full physical coverage is a separate gate.
- Finite variance is not guaranteed for rare roots even with an unbiased mean.
- Report retry quantiles, cap/roulette incidence and outliers alongside image means. Cancellation is an aborted measurement, not a zero sample or a passing render.
- The existing snell-unbiased `EstimatePDF` is a basin-width heuristic, not a reference estimator, and is not a comparison oracle.

The geometric rediscovery identity follows the original SMS construction; the extension's joint proposal, channel conditioning and topology-level variant are this repository's proposed contract. The [authors' paper and corrected reference implementation](https://rgl.epfl.ch/publications/Zeltner2020Specular) distinguish biased/unbiased variants and document a 2021 angle-periodicity correction; use the corrected reference if constraint formulations are ported.

## Shared path ownership (area emitters)

Do not use "SMS found a root this sample" to suppress PT. Discovery is random, while ownership must agree between both estimators at an identical endpoint/path context.

### The canonical-root predicate

Ownership is a deterministic, bounded canonical-root predicate, conditioned on the **actual** ordered object/event topology and query domain.

For a given anchor/emitter pair, medium state, topology and channel:
1. Construct a small fixed set of canonical seeds.
   - The first is a **topology-constrained, endpoint-aware Snell trace**, as today's `ClassifyEmitterHitCoverage` uses but with the topology fixed. A seed at an arbitrary hashed surface point usually fails Newton, which would leave the owned fraction small.
   - Further seeds, if any, use deterministic surface coordinates keyed by the full context.
2. Solve each seed. The owned roots are their validated results.

Evaluate only the topology of the candidate SMS root or of the actual PT emitter hit.

The predicate must apply **every acceptance filter the proposal applies**; otherwise an owned root can have zero proposal probability, which is a hole. Its inputs are:
- `sms_target_bounces`;
- `sms_max_chain_depth` and the chain-record bound (today 16 vertices; a longer chain is unowned and stays with PT);
- emitter-stop;
- the wrong-first-caster rule;
- solver threshold, two-stage and Levenberg-Marquardt settings;
- eligibility.

### Partition rules

- SMS contributes only roots the predicate owns.
- PT suppresses a delta-chain emitter hit only if the identical predicate owns that full chain in that channel.
- All other roots remain with PT. A random seed's failure does not create a hole, and a new reflected root does not automatically remove the corresponding PT path.
- Regular isolated roots and reproducible acceptance are prerequisites; degenerate or continuous solutions are outside this mode.

### Root identity

- Root identity includes:
  - query domain;
  - ordered objects;
  - events;
  - crossing/membership signature;
  - chain length;
  - all vertex geometry.
- Comparing only the first direction or first position is insufficient.
- Specify one shared root-equivalence policy, residual acceptance and ambiguity handling. An uncertain classification assigns the path to PT and rejects its SMS contribution; this deliberately differs from today's "unknown suppresses" rule and is a reviewed decision.
- Hashes accelerate lookup but do not establish equality.
- Tolerance and separation behaviour remain a numerical implementation proof obligation, with close-root adversarial tests and scale changes; this proposal claims no exact floating-point partition theorem.

### Determinism of ownership inputs

- Ownership inputs may not depend on mutable seed history, discovered-root sets, per-render stochastic salts, caches or retry budget.
- A cache may store the deterministic answer but cannot decide it.
- Start without cross-anchor caches; key any later cache by the complete immutable context and keep per-worker storage bounded.

### Channels and HWSS

- For RGB PT, ownership can differ per component; emission weighting must mask components separately.
- HWSS must carry lane-specific ownership through its own loop and every NM delegation; one hero decision cannot suppress all companions.
- Until phase 4 passes, an HWSS render **ignores** the extended mode, logs one warning, and keeps today's solver and today's suppression unchanged.

### Scope of the partition

- The first partition is limited to non-delta area emitters that ordinary PT can actually reach.
- Environment lights stay entirely with PT, as today.
- Delta lights use estimator A with no partition (above).

### Cost and alternatives

- Canonical solves add cost per candidate emitter hit and per SMS root. Their count is bounded by the fixed seed count per classification, but ray traversal and Newton costs must be measured.
- Increasing the canonical seed count expands owned support. It must remain a static policy, never adapted to which paths PT happened to sample.
- A future alternative is full PT/SMS MIS with actual strategy densities. Do not invent those densities from a successful discovery count.

## Eligibility and the three coupled switches

Eligibility is decided per anchor and channel before any SMS work, from inputs both estimators see identically:
- the caster set;
- emitter type;
- medium state;
- alpha presence;
- the HWSS flag;
- whether the chain depth fits.

Three switches move together for an ineligible (or legacy-mode) anchor:
1. SMS contributions;
2. PT emitter-hit suppression;
3. DL-344's delta-light shadow opacity (`bSMSCoversDeltaLights`).

Switching any one alone double-counts or loses light.

Anchors that keep the existing solver must also carry a mode bit in their chain record, so PT applies the matching suppression rule (the existing split rule, or suppress-all) rather than the extended predicate.

**Finite-scattering dielectrics.** The parser default is 1e4, and shipped SMS scenes use 1e5. `DielectricSPF` advertises such a warp through `DeltaTransmissionWarpExponent() > -1`. Excluding them would exclude nearly every real dielectric. **Adopted user ruling:** they are eligible and treated at their delta limit, as DL-379's split suppression does today. The resulting bias is measured as a gate against VCM, never hidden, and DL-379 stays open for the exact treatment.

**Legacy `SMSShaderOp`** keeps its existing behaviour and is out of scope; it has no suppression partner, so "every PT/SMS entry" in this document means the PT integrator paths and `PathTracingShaderOp`.

## Work and memory policy

- Use explicit worker-local scratch vectors sized to the configured maximum depth: one live seed and one retry chain per trial.
- Avoid three resident RGB chains by choosing a component per reference trial.
- Preview allocates a fixed budget across components. Constant-index inputs may share solved geometry after equality checks, while attenuation stays component-specific.

Expose internal counters before tuning:
- attempts and selected event counts;
- ray intersections and Newton iterations;
- accepted, owned and rejected roots;
- canonical solves and rediscovery trials (separately for root-level and topology-level K);
- tail events;
- scratch peak bytes and material queries.

The design avoids explicit exponential event-tree enumeration; that is an algorithm choice, not a measured runtime bound. Newton can still struggle, rare roots can require arbitrarily many retries, and classification can dominate. No speedup or whole-render cost guarantee is claimed.

Measurement rules:
- Baseline and candidate measurements use identical worker policy, salts, spp, ROI, build configuration and sequential rendering.
- Evaluate make, Deployment and Opto.
- Include ABI/record size and CPU memory reports if layouts change.

Keep public knobs minimal: one opt-in extended mode, plus the existing depth and trial budgets.
- Do not overload `branching_threshold`, whose PT/BDPT semantics differ.
- Separate preview work limits from reference estimator settings internally.
- Parser/API exposure waits for reviewed semantics and CST/API parity tests.
- Until then, tests enable the mode through an internal `ManifoldSolverConfig` field.

## Implementation sequence and acceptance gates

1. **Domain and medium replay — implemented**, master merge `34bd520ec5e70a5cf96bcf8b8154b1a17888880f` (`sms-ext`). Native-domain primitives and conservative eligibility are measured; production activation and ledger closures remain later gates.
   - Build native RGB per-component and NM index oracles.
   - Cover nested outer/inner media, start-inside, open sheets (DL-345 face rule) and composite rejection; check final-root tint and AR coatings.
   - Preserve legacy-neutral providers.
   - Close DL-438, DL-391 and the DL-353 remainder only after measured gates, not after adding a field.
2. **R/T proposal and estimator A, in isolated tests, then delta-light production.**
   - Synthetic two-root/three-event distributions with known probabilities and zero trials: verify averaging, channel factors, no doubled event PDFs, and rare-root tails.
   - Mixed R-T and R-T-R, TIR transitions, and mirror-only (DL-312) and transmission-only controls.
   - Root identity, plus finite-difference constraint/Jacobian checks.
   - Then opt-in production for point/spot lights: DL-437's reflection-only upward-spot fixture against an analytic/light-tracing reference, DL-420's spot-through-slab against BDPT/VCM, and the DL-344 shadow-flag coupling.
3. **Estimator B and the area-emitter partition, RGB/NM.**
   - Compare SMS-owned and PT-kept contributions separately and together.
   - Prove no dependence on trial budget or discovery order.
   - Validate all PT entry points, SPF/BSDF and SSS delegations, emitter sidedness (`EmitterSides`), and the three-switch coupling.
   - Fixtures: DL-376, DL-421, DL-372 and DL-336. DL-339(a)/379 residuals need their own red proofs.
4. **HWSS lane geometry and ownership.**
   - Distinct companion indices, nested exterior replay, per-lane TIR and emitter masks.
   - No hero-only reuse unless equivalence is proven.
   - Close DL-378 only on its actual BSDF-delta body plus delegated paths.
5. **Optional extensions.** None is implied by phases 1-4.
   - Photon proposal mixtures, after DL-331.
   - Stochastic alpha, after a visibility/partition design.
   - Spatial and graded IOR, after DL-398 derivatives.
   - Participating media, after DL-419 segment laws.
   - Guiding, after proposal replay and density accounting are stable.

**Geometry coverage in every phase.** Every crossing/event/index phase must include:
- real double-sided `indexedmesh_geometry`, both windings and both incidence sides;
- closed and open geometry, nested exits and start-inside;
- sphere/plane controls and transformed instances.

Synthetic vertices alone are insufficient.

**Regression gates.** Each production increment builds and runs, individually with checked exit codes:
- `ManifoldSolverTest`;
- `SMSUniformDispersionTest` (all modes);
- `ExteriorIndexInvarianceTest`;
- `SMSEmitterDirectionTest`;
- `SMSMediumAnchorTest`;
- `TransparentShadowPartitionTest`;
- `WeaveGapShadowTransmittanceTest`;
- `OpenSheetIndexConventionTest`;
- `GradedIndexInteriorFactorTest`;
- `ManifoldNormalDerivativeTest`;
- `DoubleSidedEmitterTest`;
- the `AlphaSMS*` suites (including `AlphaSMSReciprocalTest`);
- `PTGuidingMISPartitionTest`;
- `SourceHygieneTest` and `CstDeriveGoldenTest`;
- plus every test the increment adds.

Shipped mode-off RGBA pixels must differ by at most one float32 ULP across builds
(user ruling 2026-10-04).
Deterministic rejection on/off comparisons within one build retain strict
internal-double identity (adopted precision contract below).

**Proof per fix:**
1. Commit.
2. Restore committed master sources and dependent headers coherently.
3. Build one targeted executable with a checked exit code and no warnings, and observe the intended failure.
4. Restore HEAD and rebuild green.

**Render bands:**
- Use at least four independent `SobolSamplerTestHooks::ValueSalt` salts and at least three standard deviations under the documented metric.
- For dispersion, use channel-separated radiance and centroids, not the biased max-channel RegionMean metric (DL-433).
- Reference PT/VCM must match the tested material domain and report their uncertainty; a single noisy PT render is not ground truth.

After each coherent production increment, apply [the independent review loop](skills/implementation-review-loop.md), with separate lenses for estimator/partition, material/medium/API, and cost/doc fidelity. No merge until a fresh final tree has zero P1, followed by the targeted master gate. Stop and record any new design decision outside this proposal before changing that contract.

## Adopted initial decisions

1. Finite-scattering casters are eligible at their delta limit, with bias measured in Phase 3; DL-379 remains open for exact treatment.
2. Uncertain root classification goes to PT, reversing today's "unknown suppresses" rule.
3. Delta lights precede the area-emitter partition, because estimator A needs no ownership machinery and covers DL-437's own fixture.

## Integration evidence

The pre-design master gate for the attenuation integration (`a8fa56224`) is recorded in [DL_CHEAPBATCH_VALIDATION.md](DL_CHEAPBATCH_VALIDATION.md) under "Post-merge master gate". The design import introduced no source changes. Current Phase 1 primitive and rejection evidence is recorded in [SMS_EXTENDED_PHASE1_VALIDATION.md](SMS_EXTENDED_PHASE1_VALIDATION.md); production activation and acceptance remain pending. Phase 2 implementation and current isolated/production evidence are recorded in [SMS_EXTENDED_PHASE2_VALIDATION.md](SMS_EXTENDED_PHASE2_VALIDATION.md); The user accepted the four-salt wide-spot slab/BDPT discrepancy as variance and authorized continuation. Cone 80/85 remains a reported measurement with the original three-SD band; it supplies no DL-420 closure. The historical `13a31c7d7` replacement gate passes 28 make modes (17665 reported checks/0 failures), twelve actual Xcode-linked controls and the disclosed partial ASan/UBSan suite, with zero compiler diagnostics and verified source hashes. Fresh final review and Phase 2 integration remain pending. No ledger row closes on the contract statement.

## Adopted interim composite policy (2026-10-03)

The user ruled that composites are outside the initial extended domain.
For Phase 1, if the prepared scene contains any object whose effective
material is or wraps `composite_material`, extended mode is inert for the
entire scene. Static preparation inspects luminaire wrappers, CSG operands
and nested composites once, and logs one warning naming the first composite
object found. All three extended switches are off together; the existing
solver, PT emitter-hit suppression and delta-light shadow handling run
unchanged, exactly as with extended mode off.

Composite caster queries and isolated seed walks still decline unsupported
hits as ordinary zero trials. No composite PT chain is owned by extended
SMS. Composite scenes must match extended-on/off renders: bit-identical
where deterministic, otherwise at least four salted renders within three
measured standard deviations. Fixtures include real double-sided indexed
meshes with both windings and closed-object start-inside geometry, wrapped
and nested composites, CSG operands, and a composite-free control that
still activates the extended eligibility path.

Positive composite replay requires a separate later design, a DL-407 fix,
and an estimator for the composite walker; this design provides none.
`CompositeMaterial` currently supplies no index metadata, and containment
seeding skips it. `CompositeSPFImpl::BottomKey` is internal; `ToExternal`
normalizes bottom membership to the external scene-object identity. Do not
infer that the synthetic bottom key necessarily escapes into PT.

### Future: path-level membership provenance

A future design may introduce a per-path **seed certain** bit. Clear it at
the walk root when the seeding probe meets a composite (DL-407). Also clear
it on any IOR-stack update not made by a traced crossing, unless the
operation belongs to an audited list covering subsurface jumps, HWSS
per-lane re-seeding, photon reconstruction and DL-370 handoffs. That
provenance mechanism needs its own design and independent review before it
can relax scene-wide rejection. It is not implemented by Phase 1, and no
ledger row is opened for this note.

Setup branch `sms-ext` imported the contract at `e37b8e288` from master
`ffc70c1c2`. The adopted policy and Phase 1 primitives merged at `34bd520ec5e70a5cf96bcf8b8154b1a17888880f`; the decision itself provides no estimator or production debt closure.

## Adopted native normal fallback correction (2026-10-04)

Fresh round 2 reviewed committed `8f9a63f692b7d042264d989ea3d6f0c5da9441fb`.
The material reviewer identified a conflict in the adopted constraint convention:
`PerfectRefractorSPF::DoSingleRGBComponent` and `ScatterNM` rederive a
wrong-side reflection about the ray-opposing geometric normal. Wrong-side
transmission likewise uses geometric-normal refraction and recomputes Fresnel,
including geometric-interface TIR. At the reviewed checkpoint, extended proposals and constraints used only
the shading normal, although modified casters were eligible. The earlier claim
that this convention covers those native events is therefore withdrawn.

A static counterexample uses a horizontal narrow patch at `(5,0,0)`, receiver
`(0,0,1)`, light `(10,0,1)`, geometric normal `(0,0,1)` and shading normal
`(sqrt(3)/2,0,1/2)`. The unit incident direction is
`(0.9805806757,0,-0.1961161351)`. Shading-normal reflection is
`(-0.3204487827,0,-0.9472658432)`, on the wrong geometric side; native
geometric-normal reflection is `(0.9805806757,0,0.1961161351)` and reaches
the light. The patch can exclude other shading-constraint solutions. This
was initially established by source inspection and direction arithmetic.
The implementation regression now includes actual native RGB/NM reflection
and transmission, both windings and incidence sides, spatial/ray/raster normal
fields, coated dielectric and polished siblings, transformed instances, closed
start-inside and nested exits, and independently shifted endpoint Jacobians.
Committed-source red/green proofs and the replacement gate now pass; fresh
review remains required before integration.

**Adopted correction, user ruling 2026-10-04:** reproduce each native
material's geometric-horizon fallback in the extended event direction,
constraint/Jacobian and final price, preserving shading-normal behavior where
the native event accepts it. Branch changes must be validated consistently by
proposal, retries and later ownership. Alternatively, conservative exclusion
of affected modified casters would require a separate scope ruling. Conservative
exclusion is not adopted. The user authorized reproducing the native fallback
consistently. The native fallback and indexed/non-indexed atlas repair has committed numerical
red/green proof at `b61844191`; the displaced-wrapper sibling repair at
`761c71b51` and complete replacement gate at `15c9c2710` now pass;
Phase 2 remains unmerged until its fixes, gate and fresh review converge.

Round 2 also found a separate implementation P1: mesh atlas boundaries authored
at interior UV coordinates such as `.25` and `.75` evade the current 0/1 seam
check. Smooth world-position tint can then make raw-UV root matching split one
regular, uniquely priced physical root into two reciprocal-accounting families.
The native mesh-boundary fix has committed numerical red/green proof. A
subsequent self-audit reproduced the same gap through displaced indexed-mesh
wrappers (**19985/20** at committed `1b924f194`); the guard must follow the
actual native triangle provider rather than the outer geometry recipe. This
is not an approved deferral or a reason to drop UV context globally. The
third reviewer found no P1/P2 in cost, tests or document evidence. No Phase 2
ledger closure follows from the passing pre-review gate.

## Adopted mode-off precision contract (2026-10-04)

The latest native repair `761c71b51` has coherent committed proofs (review
20005/0, production177/0, geometry4297/0), but its replacement pipeline
stops at the strict shipped mode-off comparison. Eight alternating source
builds and four salts complete; all sixteen master/candidate hashes differ.
Reported radiance means differ by no more than 1.7763568394002505e-15.
Pixel diagnostics measure maximum absolute RGBA difference
2.4579116519873878e-11 and maximum per-image RMSE2.0080155837659702e-13.
All sixteen images match bit-identically after float32 conversion, alpha
matches in doubles, and all twelve channel means pass their original
three-SD comparison (n4). This suggests arithmetic rounding but does not
prove its cause or satisfy internal-double bit identity. The original internal-double cross-build gate failed; that failed run remains
recorded as failed. The subsequent complete replacement gate passes **36373/0**
across 28 modes, twelve actual Xcode-linked controls and five partial-sanitizer
modes, with zero compiler diagnostics. No Phase 2 merge or fresh review
verdict is claimed. See `SMS_EXTENDED_PHASE2_VALIDATION.md`.

**Original ruling, superseded by the one-ULP ruling below:** cross-build shipped
mode-off image pixels must match bit-identically after float32 conversion.
Deterministic rejection on/off comparisons within one build remain strict
in internal doubles. Stochastic comparisons retain n>=4 and their original
channel-separated three-SD bands. This changes only the cross-build
acceptance precision; it does not change rendering arithmetic or relax
within-build composite rejection. The new complete gate passes all sixteen
float32 comparisons after eight actual source builds, with four salts per
fixture. NM mode-off cost rises 1.492751 ± 0.465066% in paired measurements;
no zero-overhead claim is made. Round 3 subsequently found further defects,
recorded below; current replacement gates and Round 4 review are required
before integration.


### Phase 2 Round 3 implementation audit (2026-10-04)

Two further implementation inconsistencies were found: generated charts
bypassed seam ambiguity checks, and non-top exits in overlapping closed solids
used the stack-top index for the native direction and Fresnel law. The existing
native-law contract covers both repairs. Generated chart contexts now undergo
local physical-band probes, retaining continuous override controls and actual
UV records. This numerical diagnostic does not certify arbitrary generator
continuity. Overlapping exits now use the queried exiting-object index for
native direction/Fresnel/TIR while independently retaining the native consumer
radiance scaling from stack tops; native walkers are unchanged. Expanded
modified multi-vertex Jacobian tests address the evidence reviewer's coverage
gap. The earlier focused tests pass28933/0 at native d2508b4dc/tests5d9776ce8;
replacement committed proofs, complete gates and fresh Round 4 review remain
required. The earlier complete gate is historical and no merge is claimed.

The post-modifier UV sibling self-audit reproduces forty uncertain native
interior-mesh cases, with both actual UV families at one physical root. The
repair atc5f66dd80 probes actual post-modifier contexts at full and half
displacement, cancelling continuous UV scale in its midpoint check. Continuous
modifier controls remain required. This is within the adopted uncertain-root
contract, not a new regularity certificate or ledger closure. The earlier
Round 4 preparation was stopped before clean builds; its partial evidence is
retained. The earlier focused validation passes90485/0; complete replacement gates now pass at44645f25e, with fresh Round 4 pending.

The matched-normal sibling is also reproduced natively: shading normals on
opposite sides of a modifier discontinuity both use the same native
geometric-horizon mirror fallback, but raw-normal matching splits one root.
Test32241b5eb againstc5f66dd80 reports246025/40. Repair5d7b6ff38 extends the
same post-modifier midpoint probes to every matched geometric context field,
retaining continuous modifier controls and native price/direction oracles.
Focused validation passes193037/0; v2 preparation completed coherent proofs
but stopped before full gates. Complete v3 gates pass373510/0 across29 make modes, twelve actual Xcode controls and five partial-sanitizer modes, with zero owned compiler diagnostics; fresh Round 4 is pending.
No design change, closure or new ledger row follows from this repair.


### Phase 2 fresh Round 4 implementation audit (2026-10-04)

Three independent reviewers of936952503 found seven implementation P1s and
no design defect. The v3 passing gate is historical. Native witnesses and
repairs address root-family merging/splitting, continuous-normal derivative
aliasing, Polished sheet indices and native support, authored mesh seams,
and uncertain start-inside orientation. The original thin close-patch example
was not reachable through native acceleration; a corrected wide disjoint
native mesh demonstrates the accepted-root alias numerically. Current fixes
and their limits are recorded in
[SMS_EXTENDED_PHASE2_VALIDATION.md](SMS_EXTENDED_PHASE2_VALIDATION.md).
Complete replacement gates and fresh Round 5 convergence are required before
Phase 2 integration. Round 5 has not begun; no ledger closure is claimed.


Before launching Round 5, an internal native harmonic audit found ten remaining
normal-Jacobian failures at the finest halved probe period. A fourth
noncommensurate probe repairs that numerical alias without widening acceptance
bands. Focused283/0 and review193319/0 are current partial evidence; complete
replacement gates and fresh review remain required. Round 5 has not begun.


### Phase 2 current composed gate (2026-10-04)

Native71c080bd3 and corrected testsa6b4aa37a pass374075/0 across thirty
make modes, twelve actual Xcode controls and five partial-sanitizer modes.
The prior v2 pipeline failed ten Deployment positive close-root checks;
that failure is preserved. Test-only patch-edge clearance fixes the fixture
without changing separation, thresholds or native production code. Current
hash-checked evidence composes unchanged native clean builds and eight source
A/B builds with fresh committed proofs, all changed-test modes, actual Xcode
controls, partial sanitizers and memory. Details and limits are in
[SMS_EXTENDED_PHASE2_VALIDATION.md](SMS_EXTENDED_PHASE2_VALIDATION.md).
Fresh Round 5 is still required before merge; it is the final allowed round.
No design decision, ledger closure or new row follows from this gate.


### Phase 2 Round 5 stop (2026-10-04)

Three fresh reviewers of f3ebf3b92 report five implementation P1s and two P2s,
with no separate design defect. Required worker-local scratch/work counters
are missing, and preparation-audit cost was understated. Further reports
concern near-commensurate Jacobian aliasing, fixed-offset surface-frame
reconstruction and modified ONB replay. Numerical/source-traced findings and
verification limits are recorded in
[SMS_EXTENDED_PHASE2_VALIDATION.md](SMS_EXTENDED_PHASE2_VALIDATION.md).
The initial five-round stop was reported. The user subsequently authorized
continuation beyond Round 5 and asked for explicit checks against local
maxima. Phase 2 remains unmerged; no ledger closure or new row is claimed.


### Modifier differential contract: adopted decision (2026-10-04)

The Round 5 interior normal witness on committed native sources reports
35 passing checks and 10 failing checks: all five RGB/NM domains and both
windings accept a root whose Jacobian entry is approximately -0.291667,
while an independent period/1024 refinement gives -0.491665. The fixture
uses a smooth sinusoidal normal field; its root is inside a triangle,
away from the diagonal seam. The raw evidence is
`.claude/logs/sms-phase2-round6/interior-normal-red.log`, at `36de2c28c`.
This isolates derivative aliasing from surface projection and frame replay.

The current modifier interface exposes only `Modify(hit)`, with no derivative
or feature-size bound. Any finite collection of black-box probes can miss a
smooth field that agrees at the sampled positions but has a different
root derivative. Adding another step size does not establish the Jacobian
required by the estimator's change of variables. For example, for any finite
set of nonzero one-dimensional probe displacements `s_i`, the smooth field
`f(x) = a*x*product(x*x-s_i*s_i)/product(-s_i*s_i)` vanishes at the root
and every `+/-s_i`, yet `f'(0)=a`. Thus matching values at every stencil
point cannot bound the unobserved derivative, even for a continuous field.

**Adopted by user ruling:** accept modified interfaces
only through an audited differential contract. A provider must expose either
the actual local differential of the material-consumed frame, or a feature
bound and error bound sufficient for a resolved numerical differential.
The contract must cover incoming-ray dependencies and all modified context
used by the native event, including the distinction between `onb.w()` and
Polished's `vNormal`. Uncertified contexts are ineligible, with all three
switches off together and PT retaining their paths. Audited continuous
modifiers remain positive controls; this is not a blanket modifier ban.
Existing legacy/off behavior is preserved. Provider API details, lifetime,
boundary handling and cost require design and review before implementation.

The user adopted this supported-domain contract and authorized implementation.
The initial implementation uses an optional capability in the existing
modifier header, preserving the original modifier interface and legacy
behavior. Audited providers supply directional derivatives of vNormal and
onb.w from the raw native hit differential (world/object points, UV,
normal/frame and incoming ray); raster state is held fixed. Providers may
modify these frame/UV fields but must preserve the physical surface and
geometric normal. Unsupported dependencies or boundaries return uncertified.
Native constraint and emitter-endpoint derivatives use the chain rule;
finite differences validate native geometry reconstruction, not a black-box
modified field. A caster with no audited capability makes the anchor
ineligible for all three switches; every isolated proposal also checks it.
No ledger row is opened or closed by this decision.


### Phase 2 fresh Round 6 precision stop (2026-10-04)

The adopted analytic modifier contract and scratch/provenance/preparation
repairs pass their coherent committed-source controls on `cf76d5d0a`.
The new full gate stops at mode-off precision: all eight alternating source
builds and sixteen salted renders complete, but one NM image fails the
existing float32 bit-identity contract. Exact-binary native pixel captures
reproduce the hashes and locate one R component at `(38,59)` differing by
one float32 ULP (5.684341886080802e-14), from underlying double values only
4.642574878406611e-16 apart. All other fifteen float32 image hashes match.
The measured candidate cost changes range from +0.069% to +1.185% (n4;
SD0.595%–1.003%). See SMS_EXTENDED_PHASE2_VALIDATION.md for complete raw
provenance, counts, timing SDs and limitations.

This run is **failed**, not a passing gate or a review convergence. No
Phase2 merge, new ledger row, closure or fresh Round6 review occurred.
The precision policy above remains adopted and unchanged. A proposed
cross-build bound of one float32 ULP needs an explicit user ruling before
code can enforce it; deterministic within-build identity and original
stochastic bands remain unchanged. Alternatively, continue investigation
and repair under exact float32 identity. No cause is guessed from the
small measured difference, and no exception is silently granted.


### Adopted cross-build one-ULP bound (2026-10-04)

The user approved the proposed bound (“approved, go!”) after the exact-binary
pixel evidence above. This replaces strict float32 bit identity for
cross-build shipped mode-off comparisons with a maximum of **one float32
ULP per RGBA component**. Nonfinite pixels fail; opposite signed zeros have
zero numeric distance. Exact hashes remain diagnostics, not the acceptance
predicate. Full per-pixel comparisons, not image means or hashes alone,
enforce the bound. Deterministic on/off rejection within one build remains
bit-identical in internal doubles. All stochastic comparisons retain their
original salted n>=4, channel-separated three-SD bands. The earlier strict
gate remains recorded as failed; this approval does not retroactively pass
it or establish the source of the rounding difference. Fresh gates and
reviews remain required, with no ledger changes implied by this ruling.


### One-ULP gate and defined hash arithmetic (2026-10-04)

The approved one-ULP A/B comparison passes all sixteen mode-off renders;
mean paired overhead is +1.098% to +2.045% (n4, SD0.534%–1.144%). The
subsequent source-hygiene census repair has committed-master-test168/1 to
restored170/0 proof, including a primitive-only field guard. The composed
regression stage passes33 make modes /411404 reported checks and twelve
actual Xcode-linked controls, with zero failures and owned diagnostics.
This is not full-gate completion: mixed sanitizer instrumentation first
reports a vector container overflow; a fully instrumented project build
resolves that report but reveals real signed overflow in the vertex-weld
cell hash. The repair uses defined unsigned wrapping, preserving the weld
coordinates/tolerance and distance acceptance. Rebuilt pre-fix UBSan fails;
restored full-instrumentation review passes193401/0. A complete fresh gate
and fresh reviews remain required. Evidence and limits are recorded in
SMS_EXTENDED_PHASE2_VALIDATION.md. No contract change, ledger row, closure,
merge or push follows from this implementation repair.


### Fresh Phase 2 replacement gate (2026-10-04)

The repaired native/test tree at `746fa42c2` passes the fresh complete gate:
33 make modes /411404 reported checks, twelve actual Xcode-linked controls,
eight fully instrumented sanitizer modes, process-memory measurement and
all sixteen shipped one-ULP comparisons. Clean make and both actual Xcode
configurations have zero owned diagnostics. The coherent19-path committed
proofs reproduce their intended reds and restore all focused controls green.
The40 salted production means are unchanged by scratch storage (0 double
ULP). Default paired costs are RGBk1 +0.061% ±0.577%, RGBk2 +0.051% ±0.977%,
NM +1.759% ±0.919% and HWSS +1.616% ±1.028% (n4). All12 narrow-cone slab
channels pass; the previously accepted wide-cone red −0.174% /−5.139 combined
SD measurement remains disclosed under the original band, with no DL-420
closure. Full provenance, counts, memory and scope limits are in
SMS_EXTENDED_PHASE2_VALIDATION.md. Fresh Round6 review remains required;
Phase2 is unmerged and no ledger row is opened or closed.

### Fresh Round 6: composed input differential stop (2026-10-04)

Three fresh independent reviewers completed on `eca4f2eccb3309bba655cbf49b6b602955894be4`.
They verified the recorded gate provenance but did not return a clean round.
The estimator reviewer identified a DESIGN gap: the analytic modifier receives
numerically differentiated generated UVs, although `IUVGenerator` has no
audited derivative or feature-bound contract. An analytic modifier therefore
cannot establish the derivative of its composed input transport.

The committed native witness `3e497f4b5` reproduces this gap in
`SMSExtendedReferenceTest --r6-generated-uv-only`: **35 passing /10 failing
checks**, with a successful checked test build and no compiler diagnostics.
All three RGB domains and NM450/NM650, in both real double-sided indexed-mesh
windings, accept the root. Its Jacobian entry is approximately **-0.291667**;
an independent period/1024 residual oracle gives **-0.491665**. A custom UV
generator supplies `u=A*sin(2*pi*x/P)`, with
`P=cbrt(epsilon)/(4*114243)` and `A=.2*P/(2*pi)`. The modifier differentiates
`normalize((u,0,1))` analytically with respect to its supplied UV input.
Moving the harmonic field upstream of the modifier bypasses the previous
analytic-normal repair. Raw evidence is
`.claude/logs/sms-phase2-round6-findings/generated-uv-build.log` and
`generated-uv-red.log`. This is an executed native Jacobian counterexample;
no new production radiance/bias measurement is claimed.

**Adopted correction, user ruling 2026-10-05:** certification must cover the
composed input transport as well as the modifier. A modifier that consumes
generated UVs requires an audited UV differential (analytic, or a sufficient
feature/error bound); otherwise that caster makes the extended anchor
ineligible, with all three switches off and PT keeping its paths. Providers
must declare relevant input dependencies conservatively. Audited modifiers
independent of generated UVs remain eligible. Existing legacy/off behavior
and the scene-wide composite rule remain unchanged. More black-box stencil
probes cannot certify an arbitrary generator. The user approved this correction ("approved, proceed"). Implementation uses
a separate optional UV differential capability and a conservative modifier
UV-dependency declaration; the legacy interfaces remain unchanged. No ledger
row is opened for this contract decision. Repairs and fresh gates/review are
required before integration.

Other Round 6 implementation findings at that checkpoint required repair: `Object.cpp` and
`CSGObject.cpp` include the SMS header before the Windows `/Yu` precompiled
header; the weld-coordinate conversion can exceed `int64_t` before the
unsigned hash; and the unsupported-caster loop never executes kind4, so its
claimed after-final-preparation uncertified-modifier coverage is withdrawn.
The passing full gate remains valid for its recorded workload and checkpoint;
it neither covers these missing cases nor establishes review convergence.
Phase2 remains unmerged. No rows are opened/closed and no changes are pushed.

### Round 6 repairs and completed replacement gate (2026-10-05)

The composed-input correction is implemented through optional `ISMSUVDifferential` and conservative `SMSFrameDependsOnUV`. Generated-UV differentials receive the native object-space point/normal and their transported derivatives. Transformed real-mesh controls cover both windings and incidence sides, all RGB components and NM450/NM650, with independently shifted constraint and emitter endpoint Jacobians. PCH order, safe weld-cell conversion and the actually executed post-preparation rejection cases are repaired. The committed pre-transform proof is281/60; pre-composed UV321/30, unsupported1005/76 and hygiene170/2; restored UV321/0, unsupported1081/0 and hygiene172/0. The fully instrumented committed weld proof reproduces native float-cast overflow, then restores weld37/0 and UV321/0.

The current checkpoint `f5f04b1f5` passes35 make modes /412084 reported checks /0 failures, sixteen actual Xcode-linked controls, eight rebuilt native controls, ten full-project sanitizer modes /236320 checks /0 failures, and clean make/Deployment/Opto with zero owned diagnostics. The gate retains29 completed make modes and resumes six on the same checked binary with unchanged20-native/4-test hashes; the interrupted slab is rerun in full. Four earlier committed rollback scenarios and eight interleaved A/B builds are retained on identical native hashes; current native-master/restored proofs and updated toolchain builds/controls are rerun. The initial rejected Xcode setup retry and sanitizer harness termination mismatch are preserved separately. Full provenance, cost and scope are in [SMS_EXTENDED_PHASE2_VALIDATION.md](SMS_EXTENDED_PHASE2_VALIDATION.md). All12 narrow-cone comparisons retain and pass their original3SD bands; the accepted wide-cone discrepancy supplies no DL-420 closure. Fresh independent Round7 review and integration remain pending. No ledger row is opened or closed.

### Fresh Round 7 implementation audit (2026-10-05)

Three independent reviewers completed on `01fcb931e`. The estimator lens found irrelevant oscillatory UVs could split one regular physical mirror root into separate reciprocal families. The material lens found direct-object final refresh and modifier/differential hits omitted scene signals needed by cross-object painters. The evidence lens found the flat generated-UV control did not exercise a nonzero normal derivative. No DESIGN finding was reported. Committed native witnesses reproduce the UV defect at3/20 and the initial signal-context defect at353/240; the initial signal witness transmission oracle lacked `SetCurrentObject` and its corrected committed-source proof is required before final recording. UV dependency matching and shared scene-signal completion are implemented; curved off-axis controls pass425/0 and detect an intentionally omitted normal derivative. Repairs, coherent proofs, full replacement gates and fresh review remain required. Phase2 is unmerged; no new ledger row or closure is claimed.

### Phase 2 fresh Round 8 replacement gate (2026-10-05)

Checkpoint `7c7248a4baca25673ec682a6095e65fa1368a853` has fresh six-scenario committed rollback proofs,39 make modes /414590/0,24 actual Xcode-linked controls /406140/0,14 all374-unit sanitizer modes /238826/0, clean make/Deployment/Opto with zero owned diagnostics, full interleaved n4 A/B costs and process-memory evidence. Round7 irrelevant-UV, native signal provenance and curved-normal UV coverage findings are repaired. All20 native/four test hashes verify; no retained Round7 gate substitution. Fresh independent review is pending; Phase2 remains unmerged and all target ledger rows remain open. See SMS_EXTENDED_PHASE2_VALIDATION.md for actual counts, historical failed attempts and cost scope.

### Phase 2 fresh Round 8 disposition (2026-10-05)

Fresh review on 1a1eaeedd found two implementation P1s (Polished native normal normalization/derivatives, and SSS return-boundary clamping of downstream reference A) plus a P2 factorization-capacity reservation/coverage gap. No DESIGN finding. These require native witnesses, repairs and another fresh gate/review before integration. The contract remains unchanged and Phase2 is unmerged. See SMS_EXTENDED_PHASE2_VALIDATION.md.


Round 8 implementation repair: reference-A radiance provenance survives opaque SSS shader returns. A return containing an actual nonzero reference contribution remains unclamped as a whole at its SSS caller; ordinary-only and HWSS legacy returns retain existing clamps. This implements the adopted unclamped reference contract without introducing the deferred path-level membership provenance. Replacement gate and fresh review are required before Phase 2 integration.

Fresh Round 9 gate at 4495ae81be81107fd604bfad0faedfd46a6fe846: all 21 native/four test hashes verify;42 make modes/ 416773/ 0,30 actual Xcode-linked controls/ 410506/ 0,17 all 374-unit sanitizer modes/ 241009/ 0, clean make/Deployment/Opto 0 owned diagnostics, nine committed rollback scenarios and full interleaved n4 cost/memory evidence. All three Round 8 findings are repaired. Fresh independent review is pending; Phase 2 remains unmerged. See SMS_EXTENDED_PHASE2_VALIDATION.md.

### Round 9 scaled ONB sibling repair

Fresh material review found native Optics normalizes a nonunit ONB W, while the extended constraint retained its magnitude. A corrected committed witness reproduces 4145/1040, including actual native root-position and price failures; focused repair passes 5185/0. Estimator/evidence reviews reported no P1/P2 and no lens found a DESIGN defect. Reproduce native event normalization and its derivative branch while retaining raw coating/material query fields. Replacement gate and fresh review remain required; no ledger closure is claimed. See SMS_EXTENDED_PHASE2_VALIDATION.md.

Fresh Round 10 gate: 43 make modes / 424086/0, 32 actual Xcode-linked controls / 425132/0, 18 all-374-unit sanitizer modes / 248322/0, clean make/Deployment/Opto zero owned diagnostics, fresh coherent scaled-ONB proof and full n = 4 interleaved cost/memory evidence. All 21 native/four test hashes verify. Fresh review and Phase 2 integration remain pending. See SMS_EXTENDED_PHASE2_VALIDATION.md.


### Fresh Round 10 disposition and native-event repairs (2026-10-06)

Review of `30a7ef45f` found one material P1: a raw-incidence coating query could overwrite mandatory native TIR. Both estimator/material reviewers also found a P2: Optics tolerates normals within its normalization boundary, producing a nonunit outgoing vector, while the previous half vector assumed a unit result. Evidence: `.claude/logs/sms-phase2-round10-final/fresh-reviews/`. Evidence review found no P1/P2; no DESIGN finding.

Replay now retains mandatory TIR before coating evaluation, including the proposal sibling. Audited-frame constraints retain native outgoing-vector length in the generalized half vector and differentiate that length; the projection basis is normalized separately. Native normalization-branch changes participate in differential regularity checks. The native SPF and Optics implementations, legacy constraint path and tolerances remain unchanged.

The focused combined witness passes 44681/0: RGB/NM native events, both indexed windings/incidences, transformed/variable modifiers, scales on both sides of both normalization boundaries, a bounded indexed patch excluding the spurious root, coated TIR on open-plane/closed-indexed/nested/actual non-top overlap exits, and actual PT production with four salts at N=2048 against a single-open-sheet analytic reflection using native BSDF/emitter values. The original box production attempt admitted other walls and is discarded as an unmatched single-root oracle. Early TIR fixture attempts had incomplete vertex fields and an unaudited open-mesh assumption; they are not claimed as final coherent numerical proofs. The current 21-path committed rollback and full replacement gate remain pending; this focused result does not authorize integration. No ledger row is closed or opened.

Fresh Round 11 gate: 44 make modes / 468767/0, 34 actual Xcode-linked controls / 514494/0, 19 all-374-unit sanitizer modes / 293003/0, clean make/Deployment/Opto zero owned diagnostics, fresh coherent native-event proof and full n = 4 interleaved cost/memory evidence. All 21 native/four test hashes verify. Fresh review and Phase 2 integration remain pending. See SMS_EXTENDED_PHASE2_VALIDATION.md.


Phase 2 Round 11 review and repair checkpoint (2026-10-06): estimator and evidence reviewers reported no P1/P2 or design finding. The material reviewer found a P2: native DielectricSPF only attempts the geometric transmission fallback when the initial reflectance is below one. Extended replay had attempted it after every successful shading Snell calculation. The repair binds native constraints to the RGB component or NM wavelength, distinguishes true TIR from film saturation, preserves the native raw-film query after successful refraction, and sweeps crossing, pricing and proposal consumers. No native scattering algorithm or virtual SPF interface changes.

`SMSExtendedReferenceTest --r11-saturated-film-only` passes 14635/0 on the repaired source. It exercises four film variants (unsaturated, critical/saturating, opaque after fallback, absorptive), actual RGB/NM SPF events, both indexed windings/incidences, transformed open and closed geometry, start-inside, plane and sphere controls, and invalid RGB domain rejection. Active-event branch and full/half coating-Fresnel price probes enforce ordinary-zero rejection when the final root cannot resolve native support or price continuity. Independent Jacobian checks cover regular unsaturated controls; critical-film discontinuities are tested for native event pricing and uncertainty rejection, not claimed as differentiable. Earlier fixture experiments and partial working-tree red counts are historical; a new coherent committed-source rollback and full replacement gate are required before integration. No ledger row opens or closes.

The first Round 12 proof run stopped at the geometry regression (2817/40): the new native-event check had reused a seed flag that also means derivatives are not computed yet. Unmodified three-event seeds therefore failed the initial constraint check before derivative construction. The local native constraint copy now initializes event/context validity independently, retaining false for failed native frame reconstruction or impossible transmission. Existing mixed-event geometry controls pass 4297/0 and the four-film witness remains 14635/0. The failed gate is retained as an interrupted historical attempt; the final replacement gate must restart on the corrected committed source and include this committed regression checkpoint as a second coherent rollback proof.

The second Round 12 proof run stopped on four transformed NM signal-painter roots (1645/4). The newly added optical-discontinuity probe had tested full attenuation weight, including native float-precision spectral reconstruction, against a double-precision optical band. The probe now checks only the coating Fresnel price that gates native fallback; painter input contexts retain the existing geometric/UV/normal checks. This narrows the implementation to its intended native optical support check without changing bands or attenuation evaluation. Both interrupted gate attempts remain historical; the final complete gate must restart on the corrected source.

The corrected optical-price scope passes the focused sequence: signal prices 1665/0, mixed geometry 4297/0, four-film fallback 14635/0, signal production 393/0, curved UV 425/0, polished normals 1089/0, scratch depth 109/0, SSS clamps 1065/0, scaled frames 7233/0 and native events 44681/0. Checked library/test builds have zero compiler diagnostics. These are focused results, not substitutes for the final complete gate or fresh review.

Fresh Round 12 gate: 45 make modes / 483402/0, 36 actual Xcode-linked controls / 543774/0, 20 all-374-unit sanitizer modes / 307648/0, clean make/Deployment/Opto zero owned diagnostics, fresh coherent native-event proof and full n = 4 interleaved cost/memory evidence. All 23 native/four test hashes verify. Fresh review and Phase 2 integration remain pending. See SMS_EXTENDED_PHASE2_VALIDATION.md.

Fresh Round 13 gate: 46 make modes / 500509/0, 38 actual Xcode-linked controls / 577988/0, 21 all-374-unit sanitizer modes / 324755/0, clean make/Deployment/Opto zero owned diagnostics, fresh coherent native-event proof and full n = 4 interleaved cost/memory evidence. All 23 native/four test hashes verify. Fresh review and Phase 2 integration remain pending. See SMS_EXTENDED_PHASE2_VALIDATION.md.

Fresh Round 14 gate: 47 make modes / 508576/0, 40 actual Xcode-linked controls / 594122/0, 22 all-374-unit sanitizer modes / 332822/0, clean make/Deployment/Opto zero owned diagnostics, fresh coherent native-event proof and full n = 4 interleaved cost/memory evidence. All 23 native/four test hashes verify. Fresh review and Phase 2 integration remain pending. See SMS_EXTENDED_PHASE2_VALIDATION.md.
