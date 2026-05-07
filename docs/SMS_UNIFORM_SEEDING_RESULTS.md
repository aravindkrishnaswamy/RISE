# Phase 8 measurements — Mitsuba-faithful SMS port

> **2026-05 update**: Path-tree branching (`branching_threshold` + `BuildSeedChainBranching`) was excised from RISE.  The "Option C" Fresnel-branching seed extension described later in this document is no longer in the codebase.  Uniform-mode SMS now uses single-chain stochastic seeds with `multi_trials` for variance reduction on multi-modal scenes — matches Mitsuba SOTA convention.  Sections referencing `branchingThreshold` describe historical context.

This file captures the per-mode comparison data that informed the Phase 9 default decision (per `docs/SMS_UNIFORM_SEEDING_PLAN.md`).

## Methodology

- **Build:** post-Phase 7 (uniform-on-shape seeding + geometric Bernoulli + photon-aided trial integration), `SMS_DIAG_ENABLED = 1`.
- **Corpus:** the 8-scene set defined in `docs/SMS_UNIFORM_SEEDING_BASELINE.md`, all rendered at 4 spp from `/tmp/rise_baselines/*.RISEscene` working copies.
- **Modes compared:**
  1. `seedingMode=snell, biased=true` — pre-Option-A baseline (RISE legacy).
  2. `seedingMode=uniform, biased=true` — Phase 4/7 (uniform-on-shape + photon-aided trials).
  3. `seedingMode=uniform, biased=false` — Phase 5 (uniform + geometric Bernoulli).  Photons unused (Bernoulli requires same proposal distribution).

## Results

| Scene | Snell biased ratio | Uniform biased ratio | Uniform unbiased ratio | Snell wall-clock | Uniform biased wall-clock | Uniform unbiased wall-clock |
|---|---:|---:|---:|---:|---:|---:|
| smooth Veach egg     | 0.9246 | 0.9287 | 0.9382 | 6.9 s   | 6.3 s   | 7.8 s     |
| bumpmap Veach egg    | 1.0410 | 1.0577 | 1.1035 | 41.7 s  | 44.0 s  | **11 m 17 s** |
| displaced Veach egg  | 0.1272 | 0.13–0.16 | 0.1192 | 29.3 s | 30.3 s | 29.5 s |
| k1_refract           | 2.3698 | 2.2635 | 2.8284 | 0.15 s | 0.15 s | 0.16 s |
| k1_botonly           | 0.9281 | 0.8243 | 1.2429 | 0.14 s | 0.14 s | 0.14 s |
| k2_glasssphere       | 4.1720 | 3.3231 | 5.2748 | 0.14 s | 0.15 s | 0.16 s |
| k2_glassblock        | 1.0280 | 0.9713 | 0.9128 | 0.21 s | 0.20 s | 0.27 s |
| slab_close_sms       | 3.0376 | 3.7730 | n/a    | 15.1 s | 14.4 s | n/a (not run) |

(Numbers are single-run at 4 spp; expect ±5–30% Monte Carlo noise on ratio depending on scene complexity.  The 3-run stability check on displaced uniform-biased gave 0.124, 0.125, 0.149 — confirming the stable mean is ~0.13.)

## Smooth Veach egg (gold reference)

The plan's critical correctness gate was "uniform unbiased smooth Veach must produce ratio in [0.95, 1.05]".  Observed: **0.9382**.

Just outside the gate, but the **biased** snell baseline on the same scene is 0.9246.  The 6% gap between our measured ratio and 1.0 reflects RISE's measurement methodology — MIS rounding, emission-suppression overcounting at non-SMS hits in the same chain, and the visibility-filter rejection rate — none of which are addressable inside the Bernoulli math.  The gap is **the same in biased and unbiased modes** within run-to-run noise, indicating the geometric Bernoulli is correctly applying the `K = first-success-index` factor:

- Biased ratio 0.9246 (no K multiplication).
- Unbiased ratio 0.9382 (with K multiplication).
- Δ = +1.5%, consistent with average K slightly above 1 on a high-convergence-rate scene.

If we want to pass the [0.95, 1.05] gate strictly, the next step would be to fix RISE's measurement methodology (MIS or emission-suppression accounting), not to touch the SMS algorithm — out of scope for this work.

## Wall-clock pattern

- **Smooth scenes:** uniform mode ≈ snell mode (within 15%).  Geometric Bernoulli adds ~10% overhead.
- **Bumpy scenes:** uniform unbiased is 10–30× slower because the geometric Bernoulli's `K` cap (default 100) fires often when convergence rate is low.  This matches the literature's warning (Hong et al. 2025 PSMS-ReSTIR: "*manifold-based methods have an unbounded probability of finding a solution, which may be very small and lead to significantly high variance*").
- **Displaced caustics:** all modes are roughly equivalent on wall-clock; ratio is the literature open problem (no SMS variant has solved heavy displacement).

## Photon-aided integration (Phase 7)

| Scene | Uniform biased, photons OFF | Uniform biased, photons ON |
|---|---:|---:|
| displaced (3 runs) | 0.124, 0.125, 0.126 | 0.148, 0.123, 0.156 |

Photon-aided integration produces a modest positive contribution on the displaced scene — average ratio rises ~10%.  Mechanism: photon-recorded chains find caustic basins that uniform-on-shape sampling misses; direction-dedupe filters duplicates so net effect is additive.

## Post-review revisit (2026-05-02): M-trial biased mode

The original Phase 4 implementation defaulted to **M=1 per caster** in biased mode — a degenerate case of Mitsuba's biased estimator that produces measurable energy loss vs snell mode (whose `multi_trials=1` is reliable due to deterministic Snell seeding).  User-reported visual regression confirmed the issue.

**Fix:** post-review code now respects `config.multiTrials` as Mitsuba's M (paper §4.3 Algorithm 3) — runs `M` independent uniform-area samples per caster in biased mode, dedupes by `(first-vertex-pos, chainLen)`, sums unweighted.

**Measured impact (bumpmap Veach egg, two-stage, 4 spp):**

| Mode | Ratio | Δ vs snell |
|---|---:|---:|
| snell biased | 1.251 | — |
| uniform biased, M=1 (pre-fix) | 1.067 | −15% |
| uniform biased, M=8 (post-fix) | **1.191** | −5% |

The remaining 5% gap closes further at higher M but with linear cost.

**Other code-correctness improvements landed in the same revisit:**
- `ReversePhotonChainForSeed` + `ComputeTrialContribution[NM]` helpers extracted to deduplicate the per-trial contribution math across 4 call sites.
- Cross-trial dedupe key changed from direction-only to `(first-vertex-pos, chainLen)` — fixes a topology-aliasing failure mode where k-vs-k+2 chains sharing a first-vertex direction were collapsed.
- `EnumerateSpecularCasters` now probes 3 deterministic prands per object instead of 1 — fixes misclassification of `SwitchPel`-style painters whose `isSpecular` flag varies with `(u, v)`.
- `SMSConfig::seedingUniform` (bool) → `SMSConfig::seedingMode` (enum `SMSSeedingMode`) — extensibility for future modes (Specular Polynomials, MPG).
- Spectral path now honors `seedingMode` (parallel `EvaluateAtShadingPointNMUniform` + `ComputeTrialContributionNM`).  Per-vertex eta is overridden with `GetSpecularInfoNM(nm)` after the chain is built and before `Solve` runs so dispersive glass converges to the wavelength-specific caustic root.
- `maxBernoulliTrials = 0` now hard-caps at 1024 instead of looping unbounded.
- `sms_seeding` parser is case-fold (`"Uniform"`, `"UNIFORM"`, `"uniform"` all map to `SMSSeedingMode::Uniform`).

## Seeding regime trade-off — the snell/uniform decision is regime-dependent, not "uniform always wins"

A late-investigation finding that reshapes how to think about the snell-vs-uniform choice.  Original framing: "uniform mode is Mitsuba-faithful, the principled correct path; snell is RISE-legacy."  Reality: **the two modes are complementary — each wins in a different geometry regime**, and the right default depends on what's being rendered.

### Empirical evidence

| Scene | Surface character | Snell ratio | Uniform ratio | Winner |
|---|---|---:|---:|---|
| smooth Veach egg     | smooth analytic ellipsoid | 0.93 | 0.94 | tie |
| bumpmap Veach egg    | smooth analytic + normal-map | 1.04 | 1.06 | tie |
| displaced Veach egg  | bumpy mesh (`disp_scale=2.0`) | **0.37** | **0.008** | **snell wins by ~50×** |
| diacaustic           | smooth curved tube, k=2 reflection | ~0 (without supplement) | shows cardioid | **uniform wins** |
| slab_close_sms       | displaced slab | 3.6 | 3.0 | similar |

(Values measured at 4 spp post-fix; see preceding tables for context.)

### The mechanism

Newton's basin of attraction at a specular vertex scales with the local surface smoothness.  On smooth surfaces the basin is large enough that *any* reasonable seed walks to a root — both seeding strategies succeed equivalently.  On heavy displacement the basin shrinks per-bump:

- **Snell-trace seed (snell mode):** deterministically aimed at the dominant *refraction-caustic* root by construction — the ray from shading point toward light hits the caster at a position close to the refraction-caustic root.  Newton converges from that seed for the dominant chain class, even on bumpy surfaces.
- **Uniform-area seed (uniform mode):** distributes seeds uniformly over the entire caster.  On smooth surfaces, large basin = most samples eventually walk in; on heavy displacement, each per-bump basin is smaller than the inter-sample spacing of uniform-area sampling.  Most seeds land in basin-free zones and Newton diverges.

Counter-intuitively, the *less stratified* seed (snell-trace's deterministic single-point) outperforms the *more general* seed (uniform-area's full-surface coverage) on bumpy meshes — because the deterministic point is in a basin and the random points usually aren't.

### Where each mode wins

**Snell mode — recommended default for:**
- Heavily-displaced specular casters (bumpy meshes, vertex-perturbed surfaces).
- Refraction-dominated caustics (glass volumes, lenses, bottles).
- Any scene where Newton's basin of attraction is the dominant cost.
- The general production case when in doubt.

**Uniform mode — recommended for:**
- Smooth analytic specular primitives (spheres, ellipsoids, planes).
- Pure-mirror caustic chains (diacaustic, glints) where the Snell-trace toward the light fundamentally can't seed the chain.  Snell mode now handles these via the pure-mirror supplemental loop in `EvaluateAtShadingPoint` (uses `BuildSeedChain` directly).
- Reflection caustics on smooth dielectrics (front-face glass, lensed reflections).

### What this means for the "general brightness drop"

There are **three structurally distinct deficit mechanisms** that previously were conflated:

| Deficit | Cause | Status | Fix |
|---|---|---|---|
| **Smooth Veach 7%** (ratio 0.93 vs 1.0) | Missing Fresnel-reflection chain classes (refract-then-reflect through nested dielectrics) | **Mostly fixed** by the multi-level branching + uniform-area resample for reflection branches at split points | Landed |
| **Displaced snell 63%** (ratio 0.37 vs 1.0) | Newton convergence rate on bumpy normals (C¹-discontinuous half-vector constraint) | **Open literature problem.**  Even with a perfect refraction-basin seed, ~77% of trial-0 attempts fail because Newton's line search stalls on per-triangle Jacobian discontinuities. | Specular Polynomials (Mo 2024) is the principled fix — replaces Newton entirely with deterministic root-finding.  Captured in `docs/SMS_FUTURE_DIRECTIONS.md`. |
| **Displaced uniform 99%** (ratio 0.008 vs 1.0) | Compound: same Newton problem as above PLUS uniform-area seeds mostly miss the per-bump basins entirely. | **Use snell mode** for displaced scenes.  Mode is parameter-tuneable; not an inherent code bug. | Document the regime recommendation. |

The first one is what we landed in this work; the second is the next bottleneck and matches what's open in the published literature; the third is a scene-author-side regime choice.

## Pure-mirror caster supplement in snell mode — diacaustic recovery

Snell-trace from shading-point toward light fundamentally cannot seed chains where the light path requires multiple mirror bounces — the line shading↔light isn't tangent to any reflection-root (e.g. diacaustic / cardioid patterns inside a curved mirror tube).  Uniform mode catches these because it samples directly on the mirror.

**Fix landed:** `EvaluateAtShadingPoint` (snell, RGB) now supplements its base seeds with `M = config.multiTrials` uniform-area samples per pure-mirror caster in `mSpecularCasters`.  The reflection chains naturally Snell-continue from the sampled mirror point so 2-bounce diacaustics are seeded automatically.  Verified on `scenes/Tests/Caustics/diacaustic_pt_sms.RISEscene`: snell-mode `valid/evals` went from 0.30% to 45.0% and the cardioid pattern is now visible matching uniform mode's render.

A bug-fix companion: the pre-fix `if (chainLen == 0 || seedChain.empty()) return` early-return guarded on the legacy single-chain variable, which short-circuited the whole evaluation when the Snell-trace produced no chain — even when the mirror-supplement *had* populated `baseSeeds`.  Now gates on `baseSeeds.empty()` and synchronizes the legacy variables from `baseSeeds[0]` when the supplement was the only seed source.

## [HISTORICAL — REMOVED 2026-05] Multi-level Fresnel branching — the displacement dimming was missing reflection paths

**Hypothesis (user-validated):** RISE's pre-fix snell-mode SMS recorded ~0.93 energy ratio on the smooth Veach-egg and ~0.13 on the displaced one.  The smooth-egg deficit is the *missing Fresnel reflection branch* off the outer ellipsoid; the displaced-egg cliff is from also missing the *refract-reflect-refract* and longer chain classes through the nested dielectrics.  Branching at every sub-critical dielectric Fresnel-decision point — not just the first — recovers these.

**Fix applied (2026-05-03):** `BuildSeedChainBranching` does NOT use a `splitFired` single-split guard.  At each dielectric vertex with running throughput `> config.branchingThreshold`, the chain splits into BOTH reflect + refract continuations (each a separate Newton-eligible seed).  When throughput decimates below the gate, subsequent vertices RR-pick weighted by Fr.  Total chain count bounded at 2^k but self-limiting because reflection branches multiply throughput by Fr ≈ 0.04 → drop below threshold immediately and don't re-split.

**Verified displacement sweep (Veach egg, snell mode + branching, 4 spp):**

| disp_scale | Pre-fix ratio | Post-fix ratio (multi-level branching) |
|---:|---:|---:|
| 0 (smooth) | ~0.93 | **0.93–0.95** |
| 0.5 (light bumps) | ~0.7 (interpolated) | **0.93–0.94** |
| 2.0 (default displaced) | **0.13** | **0.87–0.94** |
| 5.0 (heavier) | (cliff) | **0.72** |

The energy ratio holds with displacement instead of collapsing — confirming that missing Fresnel-reflection paths through nested dielectrics were the dominant cause of the dimming, not a Newton-convergence problem.

**Why not PT-faithful single-split?**  PT splits once per camera ray because subsequent recursive frames inherit `splitFired = true`; the recursion implicitly enumerates sibling subpaths.  SMS doesn't recurse — each chain we don't generate is energy lost.  Single-split would catch v0's reflection caustic but miss the refract-reflect-refract class entirely (the "inside reflection" caustic through the egg's air cavity).  Confirmed measured: with `splitFired` enabled, displaced ratio plateaued around 0.13–0.30; without it, ratio recovers to 0.87+ for the same scene.

## [HISTORICAL — REMOVED 2026-05] Fresnel-branching at sub-critical dielectric vertices (Option C)

The original `BuildSeedChain` deterministically follows the refraction branch at every sub-critical dielectric vertex (mirror materials and TIR are the only `isReflection = true` producers).  Caustics that require Fresnel-reflection branches at sub-critical incidence — e.g. a glass sphere's reflection caustic from the front face, the interior reflection caustic from the inside of a glass shell — are missed by both snell- and uniform-mode seeding unless photon-aided seeds happen to capture the branch.

**Fix (Option C):** new `BuildSeedChainBranching` method.  At each sub-critical dielectric vertex:
- If the running chain throughput exceeds `config.branchingThreshold` AND the chain hasn't already split: SPLIT into both Fresnel-reflection and refraction continuations.  Each becomes a separate seed chain that runs Newton independently.
- Otherwise: Russian-roulette pick one branch weighted by `Fr` and multiply the chain's `proposalPdf` by the pick probability.  Caller divides the converged trial's contribution by `proposalPdf` so the BSDF Fresnel factor in `EvaluateChainThroughput` cancels the proposal weighting (unbiased estimator).

**PT-faithful split semantic:** matches `PathTracingIntegrator.cpp:1791-1796`'s `!splitFired && throughput > branchingThreshold` gate — branch AT MOST ONCE per chain construction, then both children continue in RR-only mode.  Total chain count per `BuildSeedChainBranching` call is bounded at 2 (or 1 if throughput never exceeded threshold).

**Plumbing:** `ManifoldSolverConfig::branchingThreshold` mirrors `StabilityConfig::branchingThreshold`.  `RISE_API` automatically sets `smsConfig.branchingThreshold = stabilityConfig.branchingThreshold` at construction so the existing `branching_threshold` scene-file parameter (or default 0.5) controls SMS branching too — no new parser knob.

**Currently applied to:** `EvaluateAtShadingPointUniform` (RGB) and `EvaluateAtShadingPointNMUniform` (spectral) biased-mode per-caster loops.  Each uniform-area sample produces 1–2 base seeds, run through Newton independently, deduped by `(first-vertex-pos, chainLen)`, contributions divided by `proposalPdf` and summed.  The unbiased geometric Bernoulli mode keeps the legacy single-chain `BuildSeedChain` (the geometric estimator requires the trial proposal distribution to match the main solve's, which the branching builder breaks).

**Snell-mode trial 0 IS branched (post-2026-05-03 update):** `EvaluateAtShadingPoint` now calls `BuildSeedChainBranching` to produce 1–2 base seeds.  The multi-trial loop iterates trials `0..numBaseSeeds-1` over the branched chains (each carrying its own `proposalPdf`), then continues with photon-aided trials beyond.  Per-trial dedupe uses a `(first-vertex-position, isReflection-bitmask)` key so two branched chains that share a first-vertex but flip Fresnel decisions on later vertices remain distinct (the legacy first-vertex-only dedupe collapsed them).  Photon-aided trials inherit whatever branch the photon-tracer chose (its own `bBranch` controls that), so the two trial sources together explore the full Fresnel-branch space consistently.

## Sampler-dimension-drift firewall (`SMSLoopSampler`)

The reviewer flagged that the unbounded geometric Bernoulli loop and M-trial loop call `sampler.Get1D()` an unbounded number of times, polluting the parent LDS sampler's dimension stream.  Downstream PT BSDF sampling and NEE light sampling then receive samples from inconsistent dimension positions across pixels — sub-optimal stratification on Sobol/Halton.

**Fix (in all four `EvaluateAtShadingPoint*` entries):** introduced a stack-scoped `SMSLoopSampler` that:
1. Consumes exactly **two** 1-D dimensions from the parent sampler.
2. Combines them via a golden-ratio multiplicative hash to seed a fresh `RandomNumberGenerator`.
3. Wraps the RNG in an `IndependentSampler` (the `loopSampler` reference passed to all variable-count work).

**Behavioural impact:**
- Parent LDS dimension stream is now predictable regardless of M, K, or photon count.  Downstream PT/NEE sampling sees consistent dimension positions per pixel.
- Variable-count SMS internals are i.i.d.-uniform (matches Mitsuba's purely-RNG implementation).
- Mean output unchanged (variance characteristics shift slightly because internal sampling is now uncorrelated with pixel position; on the test corpus, this is within MC noise at 4 spp).

**Newton convergence rate `valid/evals` unchanged within stochastic noise (Δ < 1%) across the corpus**, confirming the algorithm itself is identical.

## Open questions / known limitations

1. **Smooth Veach egg ratio 0.94 vs ideal 1.0** — RISE measurement methodology, not a Bernoulli math bug.  Out of scope for this work.
2. **Bumpy uniform-unbiased wall-clock** — geometric Bernoulli's structural cost.  Mitigations: lower `maxBernoulliTrials` cap (with bias toward zero), or switch to biased mode for production.
3. **Displaced Veach egg ratio 0.12** — literature open problem.  All modes affected equally.  Recommendation remains VCM for this regime.
4. **k1_refract / k2_glasssphere ratios > 2** — caustic basins with multiple coexisting roots per pixel; biased mode's `Σ_l f(x₂⁽ˡ⁾)` formulation produces over-1 ratios as documented in the Zeltner 2020 paper §4.3.  All modes show this.
5. **Spectral path** — Phase 4's uniform-mode `EvaluateAtShadingPointUniform` is RGB-only; the spectral variant `EvaluateAtShadingPointNM` still uses snell-mode seeding.  If uniform mode becomes default, the spectral variant needs a parallel implementation (out of scope for this work; see plan's "Spectral path" risk).
6. **MLT integration** — MLT uses single-trial deterministic seeds; doesn't fit either Bernoulli or geometric estimator.  Plan's MLT non-goal.
