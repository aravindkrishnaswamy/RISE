# Unified Integrator Analysis Plan

**Status:** Plan of analysis — no implementation decisions yet.
**Goal:** Determine whether RISE can converge on a single integrator (or
near-single integrator) that produces low-variance images quickly across
the widest practical range of scenes, while staying physically based.
**Read first:** [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md),
[MIS_HEURISTICS.md](MIS_HEURISTICS.md),
[MLT_POSTMORTEM.md](MLT_POSTMORTEM.md),
[INTEGRATOR_REFACTOR_PLAN.md](INTEGRATOR_REFACTOR_PLAN.md),
[SMS_FUTURE_DIRECTIONS.md](SMS_FUTURE_DIRECTIONS.md),
[VCM.md](VCM.md).

This document defines **what we will measure and analyze** before
deciding what to build. It is deliberately not an implementation plan;
the implementation plan is the *output* of the analysis.

---

## 1. Goal & explicit constraints

### Primary goal

A single integrator (or a smoothly composable family of strategies under
one integrator) that delivers **low variance per unit wall-clock time**
across the full RISE scene corpus — both the regression scenes under
[scenes/Tests/](../scenes/Tests/) and the showcase scenes under
[scenes/FeatureBased/](../scenes/FeatureBased/).

"Low variance" is measured against a high-quality reference per scene
(typically PT or BDPT at very high SPP, with VCM as a reference *only*
where PT/BDPT cannot reach the transport).

### Hard constraints

| Constraint | Implication |
|---|---|
| **Physical basis preferred** | Strictly unbiased > consistent (asymptotically unbiased) > bias-bounded. VCM's finite-radius merging is consistent but biased; this is now a first-class consideration, not a footnote. |
| **Spectral parity required** | Any candidate must have a credible spectral path. The current spectral feature gaps ([RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) §4) are addressed in parallel via [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md) (spawned subtask). |
| **Refactor latitude granted** | Phases 2b and 2c of the integrator refactor ([INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md)) and broader reorganization are in scope where they reduce duplication or unblock new features. |
| **Research-territory work permitted** | We can pursue directions that are not yet baked in production renderers (ReSTIR-PT for offline, optimal MIS for VCM, etc.) provided we measure rigorously and accept the risk profile. |
| **Stay within RISE's core architectural assumptions** | CPU-tile-based rendering, scene immutability, named managers, deterministic Owen-Sobol where possible. We are not pivoting to wavefront/GPU. |

### Non-goals

- ERPT (Cline 2005) — confirmed not worth doing per prior turn.
- MLT overhaul (RJMCMC, delayed rejection, MMLT/PathMLT revival) — already deprioritized per [MLT_POSTMORTEM.md](MLT_POSTMORTEM.md) and [IMPROVEMENTS.md](IMPROVEMENTS.md) non-goals.
- Neural BSDFs / NIS — out of scope per [IMPROVEMENTS.md](IMPROVEMENTS.md).
- GPU pivot — out of scope per [IMPROVEMENTS.md](IMPROVEMENTS.md).
- Real-time / interactive optimization — separate workstream.

---

## 2. Bias-class taxonomy

The bias preference forces us to track this explicitly per candidate.
Every candidate technique below is classified as one of:

| Class | Definition | Examples in RISE today |
|---|---|---|
| **Strictly unbiased** | E[estimator] = exact integral at any finite N | PT, BDPT, MIS-weighted estimators on either, SMS in unbiased mode, ReSTIR/RIS |
| **Consistent** | E[estimator] → exact integral as N → ∞ at the prescribed schedule; biased at any finite N | VCM (radius shrinks per SPPM), MLT (transient chain bias), SMS in biased mode |
| **Bias-bounded** | Bias is an output-stage transform we can quantify | OIDN denoising (post-process, not an integrator) |

**Decision rule for this analysis**: any candidate that places the
*default* render in the consistent or bias-bounded class needs an
explicit unbiased-mode toggle and must be defensible against the
strictly-unbiased alternatives at equal wall clock. VCM-as-default does
not pass that test today; whether VCM-with-guiding-and-optimal-MIS
passes it is part of what Phase 2 of this analysis will determine.

---

## 3. Current-state recap

Captured here for quick reference; the source of truth is the linked
docs.

### 3.1 Integrators shipped

| Integrator | Bias class | Algorithmic strengths | Algorithmic weaknesses |
|---|---|---|---|
| PT | Unbiased | Diffuse interreflection, well-importance-sampled scenes, full optional-feature stack | SDS, hard caustics, small/occluded lights |
| PT+SMS | Unbiased (snell+unbiased mode); biased (biased mode) | PT regime + smooth dielectric refraction caustics | Heavily-displaced caustics, k≥3 chains, reflection-only specular chains |
| BDPT | Unbiased | Area-source indirect, glossy interreflection, partial portals | Caustics; pure diffuse (overhead); spectral path-guiding partial |
| VCM | **Consistent** | Strictly subsumes BDPT (every (s,t) connection); SDS via merging; caustics | No path guiding; no optimal MIS; no SMS; spectral merge uses luminance proxy; biased at finite N |
| MLT (PSSMLT) | Unbiased asymptotically (transient chain bias) | Sparse paths in narrow regimes; vanishingly few real cases | Per-pixel convergence; opt-out of feature matrix; no proven win on RISE corpus |
| `pixelpel` (legacy) | Unbiased (PT-shader-op) | Custom shader-op chains | Throughput; new authoring should use modern variants |

### 3.2 Optional-feature matrix — gaps that matter for this analysis

From [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) §4. Reproduced
here with focus on the gaps:

| Feature | Where wired today | Where missing | Notes |
|---|---|---|---|
| Path guiding (OpenPGL) | PT, BDPT (partial spectral) | VCM (Pel + spectral), MLT, spectral PT (limited) | Biggest single feature gap; closes the most variance on hard indirect. |
| Optimal MIS | PT, BDPT | VCM, MLT | Power-vs-balance debate moot once locally-learned weights are available. |
| SMS | PT, MLT (Pel) | VCM, BDPT (excised 2026-05) | VCM doesn't need SMS (handles caustics via merging); BDPT-SMS reintegration is design-open. |
| Adaptive sampling | PT, BDPT (Pel), VCM (Pel) | BDPT spectral, MLT, possibly others | Independent variance lever; not algorithm-blocking. |
| OIDN | PT, BDPT, VCM (Pel full bypass; spectral limited) | MLT, spectral integrators (limited) | Post-process; treated as bias-bounded output stage. |

### 3.3 Recently-completed transport infrastructure

From [IMPROVEMENTS.md](IMPROVEMENTS.md) "Current RISE Baseline":

- Light BVH for many-light sampling (Apr 2026) — 4.78× variance reduction on 100-light corridor.
- Null-scattering volumes (Apr 2026) — unbiased delta tracking with arbitrary majorant.
- Random-walk SSS (Apr 2026) — fully unbiased BSSRDF transport.
- Optimal MIS in PT (Kondapaneni 2019) — locally learned alpha.
- Light-subpath guiding in BDPT (Apr 2026).
- HWSS, Owen-scrambled Sobol, ZSobol blue-noise sampling.
- VCM with progressive radius shrinkage (consistent estimator).

This infrastructure is **the substrate** for the analysis. We do not
replace it; we ask which integrator best leverages it.

---

## 4. Phase 1 — Empirical scene→integrator mapping

**Purpose:** stop guessing about which integrator is "best" on which
scene category. Measure it. The rest of the analysis is far less
useful without this baseline.

### 4.1 Test corpus selection

Curate a representative subset (~20-30 scenes) covering the full
transport-difficulty space:

- **Diffuse-dominated** (Cornell, architectural): expect PT to dominate.
- **Glossy interreflection**: expect BDPT to dominate.
- **Refractive caustics, smooth dielectric** (Veach egg, glass sphere): SMS regime.
- **Refractive caustics, heavily displaced**: VCM-only regime today.
- **Reflective caustics / mirror chains**: VCM regime.
- **SDS chains**: VCM and SMS regime.
- **Many-light scenes** (corridor, lights array): light-BVH stress.
- **Volumetric** (homogeneous + heterogeneous): null-scattering stress.
- **Spectral / dispersive** (prism, dispersion caustic): HWSS + spectral parity.
- **Mixed transport** (hard one scene to characterize): the hardest cases.

The selection criteria, scene paths, and reference-render protocol
go in `docs/UNIFIED_INTEGRATOR_BASELINES.md` (a *measurement* document
produced as Phase 1 output, separate from this *plan* document).

### 4.2 Metrics

For each (scene, integrator) pair at multiple wall-clock budgets:

| Metric | What it tells us | Tool |
|---|---|---|
| RMSE vs reference | Convergence quality | `bin/tools/HDRVarianceTest.exe` per [docs/skills/variance-measurement.md](skills/variance-measurement.md) |
| Pure variance (K-trial stddev) | Noise floor at fixed budget | Same |
| Mean luminance bias | Bias detection (essential for VCM) | Same |
| 99th-percentile pixel error | Firefly / outlier sensitivity | Custom; uses K trials |
| Wall-clock per equivalent SPP | Throughput parity baseline | RISE log |
| Memory peak | Photon stores, kdtree, training data | RSS sampling |

**Bias measurement protocol for VCM specifically:** render the same scene at multiple radii (e.g., 0.5×, 1×, 2× of auto) and at multiple sample counts (1×, 4×, 16× the budget) to track convergence as both shrinkage and N grow. Compare the limit value against PT/BDPT reference where reachable. Document the bias magnitude per scene type. *This is the empirical answer to "how much bias does VCM actually carry on RISE's scenes."*

### 4.3 Output of Phase 1

A scene-by-integrator quality matrix with three dimensions:

- **Scene class → best unbiased integrator at fixed wall-clock**
- **Scene class → best consistent integrator at fixed wall-clock**
- **Per-class delta** between best-unbiased and best-consistent (the "bias premium" for each scene class)

This map IS the prior on which directions to pursue. If the
bias-premium is small everywhere, pursue the unbiased path. If it's
large on caustic-class scenes specifically and small everywhere else,
the answer is a hybrid (unbiased default + consistent caustic strategy).

### 4.4 Phase 1 budget

Estimate: 2–3 weeks elapsed. Most time is render time, not engineering.
A handful of small instrumentation additions (per-strategy contribution
splatting, bias accumulator) may be needed.

---

## 5. Phase 2 — Candidate inventory & literature deep-dive

**Purpose:** rigorous taxonomy of techniques that could move us toward
the goal, with bias class, scope, risk, and dependence on the Phase 1
findings made explicit.

The full literature catalog lives in subsections below. Each candidate
gets a structured entry: bias class, what it solves, what it costs, and
what RISE infrastructure it depends on or replaces.

### 5.1 Path-guiding extensions

#### 5.1.1 Path guiding in VCM (eye + light, with merge-skip during training)

- **Bias class:** preserves VCM's consistent class (does not introduce additional bias).
- **What it solves:** VCM's biggest practical weakness. Closes ~80% of the variance gap with PT-with-guiding on diffuse-dominated scenes.
- **Dependencies:** Phase 2a templatization (✓ shipped); Phase 2c BDPT refactor would let us share the eye-guiding code outright.
- **Open question:** whether merging-pass samples corrupt OpenPGL training (correlated; same problem MMLT documented). Likely needs a "training samples come from connection strategies only" gate.
- **Effort estimate:** 3–6 weeks if training-corruption hypothesis holds; could balloon if it doesn't.
- **Reference precedent:** none directly (Mitsuba's VCM doesn't have OpenPGL guiding either). Engineering-research territory.

#### 5.1.2 Manifold Path Guiding (Fan et al. 2023)

- **Bias class:** unbiased.
- **What it solves:** k≥3 specular chain robustness; the regime where vanilla SMS fails.
- **Dependencies:** sits on SMS infrastructure; would replace `caustic_caster` enumeration.
- **Effort:** 4–8 weeks (per [SMS_FUTURE_DIRECTIONS.md](SMS_FUTURE_DIRECTIONS.md)).
- **Reference precedent:** [paper code](https://github.com/mollnn/manifold-path-guiding); reproducible.

#### 5.1.3 Online Photon Guiding with 3D Gaussians (Xu et al. 2024)

- Mentioned for completeness; orthogonal to integrator choice. Defer to a later candidate-evaluation pass.

### 5.2 MIS extensions

#### 5.2.1 Variance-aware MIS for BDPT (Grittmann et al. 2019)

- **Bias class:** unbiased (MIS reweighting is invariance-preserving).
- **What it solves:** BDPT's power-heuristic variance can be improved per-pixel with second-moment-aware weights.
- **Dependencies:** RISE's `OptimalMISAccumulator` + `MISWeights::OptimalMIS2Weight` machinery is the foundation; Grittmann extends it to N>2 strategies.
- **Effort:** 8–12 weeks. Published with explanation; reference implementation available in Mitsuba 0.6 fork.
- **Risk:** medium. The N-strategy generalization complicates the BDPT walk; we already have a clean walk to extend.

#### 5.2.2 Correlation-aware MIS for BDPT (Grittmann et al. 2021)

- **Bias class:** unbiased.
- **What it solves:** strategy correlations within BDPT walks; the failure mode where two strategies sample nearly the same path.
- **Dependencies:** builds on 5.2.1.
- **Effort:** +4–6 weeks on top of 5.2.1.

#### 5.2.3 Optimal MIS for VCM

- **Bias class:** preserves VCM's consistent class.
- **What it solves:** the algorithmic asymmetry between BDPT (power) and VCM (balance); locally-learned alphas eliminate the dichotomy.
- **Dependencies:** open research. The Georgiev 2012 dVCM/dVC/dVM running-quantities recurrence may not factor cleanly under variance-aware weights. Needs a focused literature survey before scoping.
- **Effort:** 12–20 weeks if tractable, possibly unbounded if it isn't.
- **Risk:** high. This is the "open research" direction acknowledged in [MIS_HEURISTICS.md](MIS_HEURISTICS.md).

### 5.3 Specular / manifold methods

#### 5.3.1 Specular Polynomials (Mo et al. 2024) — **next SMS target**

- **Bias class:** unbiased.
- **What it solves:** Newton-fragility on heavily-displaced meshes. Returns *all* admissible paths via deterministic root-finding. Limited to k≤3.
- **Dependencies:** sits beside SMS Newton solver as a parallel solver (`sms_solver "newton"|"polynomial"`).
- **Effort:** 6–10 weeks per [SMS_FUTURE_DIRECTIONS.md](SMS_FUTURE_DIRECTIONS.md).
- **Reference precedent:** [paper code](https://github.com/mollnn/spoly).

#### 5.3.2 Bernstein Bounds for Caustics (Schaufler et al. 2025)

- **Bias class:** unbiased.
- **What it solves:** heavy-vertex-displaced caustics; the regime SMS gives up on. Different machinery (interval analysis) than Newton.
- **Effort:** 6–8 weeks. Different mathematical foundation.
- **Why it matters:** if SMS-with-Specular-Polynomials still leaves a displaced-caustic gap, this is the next candidate.

#### 5.3.3 Specular Manifold Bisection Sampling (Jhang & Chang 2022)

- **Bias class:** unbiased.
- **What it solves:** Newton-divergence rescue (~2× success rate).
- **Effort:** 1–2 weeks.
- **Why it's listed:** cheap incremental win; possibly worth doing regardless of which larger direction we pick.

### 5.4 Path-space resampling — the "uniformly great" lever

#### 5.4.1 ReSTIR-PT for offline rendering (Lin et al. 2022)

- **Bias class:** unbiased (RIS is provably unbiased).
- **What it solves:** the *uniformity* problem. Reservoirs reuse good paths spatially, so per-pixel quality becomes much less scene-dependent. The most uniform per-scene quality of anything published in the last decade.
- **Dependencies:** RISE has zero ReSTIR infrastructure today. Spatial reservoir buffers, RIS estimator combination, MIS-with-resampling theory. CPU-tile-based architecture is *unusual* for ReSTIR (which is overwhelmingly GPU literature) — unknown whether the spatial-reuse cost amortizes well on tiles.
- **Effort:** 16–24 weeks for a credible offline ReSTIR-PT.
- **Risk:** high. Literature is overwhelmingly real-time / GPU. Offline-quality with no temporal reuse exists in publications but is far less battle-tested. The "ReSTIR-PT for tile-based CPU" execution model is a research question, not just engineering.
- **Open question:** does the spatial-reuse pass increase per-tile cost enough that the variance-vs-wall-clock trade-off is favorable on RISE's hardware mix?

#### 5.4.2 Generalized RIS for BDPT (Bitterli et al. 2022, Lin/Karis 2022)

- **Bias class:** unbiased.
- **What it solves:** path-space resampling as a generic sampling primitive on top of BDPT. Subsumes 5.4.1 for the BDPT variant.
- **Dependencies:** same ReSTIR infrastructure as 5.4.1.
- **Effort:** if 5.4.1 is built first, +6–8 weeks.

#### 5.4.3 PSMS-ReSTIR (Hong et al. 2025)

- **Bias class:** unbiased.
- **What it solves:** ReSTIR specifically for SMS — caustic resampling.
- **Dependencies:** ReSTIR infrastructure + SMS already shipped.
- **Effort:** if 5.4.1 is built first, +4–6 weeks.

### 5.5 BDPT–SMS reintegration (revisit the 2026-05 excision)

Per [CLAUDE.md](../CLAUDE.md) High-Value Facts, BDPT-SMS was excised
because the cross-strategy MIS overlap (`ShouldSuppressSMSOverlap`) was
structural complexity for no measurable variance gain. **However**: that
analysis was made on the assumption that PT+SMS or VCM is the right
caustic answer. If the goal is "single integrator", revisiting BDPT-SMS
becomes interesting again — but **only** with one of:

- **Specular Polynomials as the SMS solver** (deterministic, all-roots, no Newton fragility — much friendlier to BDPT MIS than the previous Newton-plus-Bernoulli formulation).
- **A path-space MIS framework that handles SMS as a connection strategy** (rather than the `ShouldSuppressSMSOverlap` predicate-based collision avoidance).

This is captured here as a candidate to *re-evaluate after Phase 1*,
not as a recommendation. The previous excision rationale stands until
data says otherwise.

### 5.6 Out-of-scope (already decided)

- ERPT, MMLT, PathMLT, RJMCMC, delayed-rejection MLT.
- Neural BSDFs, NIS, NeRF/3DGS scene acquisition.
- Polarization.
- GPU pivot.

---

## 6. Phase 3 — Synthesis decision frameworks

**Purpose:** turn Phase 1 measurements + Phase 2 candidate inventory
into a small set of viable end-state architectures, each fully specified
and comparable.

We expect 2–4 viable end states, evaluated against the same criteria.

### 6.1 Candidate end-state architectures (sketches, not commitments)

#### Candidate A — "Strictly unbiased, BDPT-centric"

```
BDPT
  + path guiding (eye + light, already shipped)
  + variance-aware MIS (Grittmann 2019)
  + correlation-aware MIS (Grittmann 2021)
  + SMS reintegrated as a connection strategy via Specular Polynomials
  + path-space resampling (G-RIS for BDPT, optional Phase 4 add)
  + spectral parity from spectral-parity-audit subtask
```

**Bias class:** strictly unbiased everywhere.
**Scope:** Phase 1+2 (path guiding/MIS extensions) is ~6 months; Specular Polynomials adds ~2 months; G-RIS adds ~6 months. Full sequence: ~14 months.
**Strength:** physically defensible at every step; spectral path is clear; subsumes what PT+SMS does today plus what BDPT does today.
**Weakness:** does not directly address the SDS / merging caustic cases that VCM handles via photons; relies on Specular Polynomials + path-space resampling to close that gap, both of which are research-territory at the implementation extremes.

#### Candidate B — "VCM-as-default, consistent"

```
VCM
  + path guiding (eye + light, novel implementation)
  + optimal MIS for VCM (open research)
  + spectral merging via per-wavelength photons (replaces luminance proxy)
  + spectral parity from spectral-parity-audit subtask
```

**Bias class:** consistent (VCM's finite-radius bias remains).
**Scope:** path guiding ~3-6 months; optimal MIS ~3-5 months if tractable; per-wavelength photons ~2 months. Full sequence: ~8-13 months if research lands.
**Strength:** strictly subsumes BDPT in transport coverage; SDS / merging caustics handled natively; smaller new integrator footprint.
**Weakness:** *fails the hard constraint on physical basis as default*. Would need an unbiased-mode fallback. The Phase 1 bias measurement on RISE's scenes determines whether the bias is small enough to make this acceptable.

#### Candidate C — "Hybrid: BDPT default + VCM caustic fallback"

```
BDPT default
  + path guiding (eye + light, already shipped)
  + variance-aware MIS (Grittmann 2019)
  + SMS via Specular Polynomials for k≤3 caustics
  + VCM kept as opt-in for SDS / heavy-displaced caustic regimes only
  + bias clearly disclosed when VCM mode is chosen
```

**Bias class:** strictly unbiased default; consistent only when user opts in.
**Scope:** smaller than A; VCM stays as-is.
**Strength:** matches the hard constraint (unbiased default); reduces total integrator count if other rasterizers can be retired (PT? MLT certainly).
**Weakness:** still two integrators; not a "single integrator" answer.

#### Candidate D — "Path-space resampling-first" (research bet)

```
BDPT or PT base
  + path guiding (already shipped on PT/BDPT)
  + ReSTIR-PT spatial resampling pass
  + Specular Polynomials for caustics
  + variance-aware MIS
```

**Bias class:** strictly unbiased.
**Scope:** dominated by ReSTIR-PT-for-CPU-offline (~6 months exploratory). Total: ~12-18 months.
**Strength:** the *only* candidate that directly attacks the per-scene-uniformity goal at the algorithm level rather than the per-feature-completion level. ReSTIR-PT explicitly amortizes hard-path-finding cost across pixels — the most uniform per-scene quality in the recent literature.
**Weakness:** highest research risk; CPU-tile execution model unproven for ReSTIR. May produce a great paper and a meh integrator if the cost model doesn't work on RISE's hardware mix.

### 6.2 Criteria matrix for choosing among end states

| Criterion | A (BDPT-centric) | B (VCM-default) | C (Hybrid) | D (ReSTIR-PT) |
|---|---|---|---|---|
| Strictly unbiased default | ✓ | ✗ | ✓ | ✓ |
| Single-integrator goal | partially | ✓ | ✗ | ✓ |
| Spectral parity tractable | ✓ | requires per-λ photons | ✓ | ✓ |
| Closes SDS gap | via Specular Polynomials + path resampling | ✓ natively | via SMS only (gaps remain) | via Specular Polynomials |
| Per-scene uniformity | medium | medium-high | medium | **high** |
| Engineering scope | medium | medium | small | high |
| Research risk | medium | high (optimal-MIS-VCM) | low-medium | very high |
| Reuses existing infrastructure | high | medium | very high | low |

### 6.3 Decision gate

After Phase 1 + Phase 2 are complete, we hold a focused decision
session that:

1. Reviews the empirical scene→integrator map from Phase 1.
2. Evaluates each candidate end-state against the matrix.
3. Picks **either** a single direction **or** a sequenced approach
   (e.g., "C now, evaluate D in 12 months when A's MIS work has
   landed on BDPT").
4. Produces an *implementation* plan document per the chosen direction.

We do not ship a default-choice from this analysis document. The point
of the analysis is to hold the choice until measurement allows us to
make it well.

---

## 7. Parallel tracks

These run independently of the main analysis and feed it.

### 7.1 Spectral parity audit (spawned subtask)

See `docs/SPECTRAL_PARITY_AUDIT.md` (produced by the
spawned task). Output:

- Per-gap inventory of spectral-vs-Pel feature parity issues.
- Special section on VCM spectral merging (luminance proxy correctness).
- OIDN "(limited)" decoded per spectral rasterizer.
- Remediation plan ranked by ROI.

This audit feeds Phase 3: the cost of "candidate end-state X" includes
the cost of bringing X's spectral path to parity, which the audit
quantifies.

### 7.2 Integrator refactor extensions (Phase 2b/2c)

Per [INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md),
Phases 2b (PathTracingIntegrator templatization) and 2c (BDPTIntegrator
templatization) were deferred. The deferred phases would close ~6,000
more lines of Pel↔NM duplication.

**Question for the analysis:** should Phase 2b/2c land *before* the
chosen new feature work, *during*, or *after*?

- **Before** lets us write each new feature once across Pel/NM/HWSS — no per-feature Pel↔NM duplication tax. This is the original refactor rationale.
- **During** merges them into the feature work; risky because each refactor pass has been carefully bounded. Probably wrong.
- **After** means each new feature lands as Pel/NM pairs initially and gets folded into the templated form during a future refactor pass.

Phase 1 of *this* analysis runs in parallel with Phase 2b/2c if we
choose to land them. The analysis output (Phase 3 decision) should
specify the integrator-refactor sequencing for the chosen direction.

### 7.3 Variance-measurement infrastructure

Phase 1 needs solid measurement scaffolding. The
[`HDRVarianceTest`](skills/variance-measurement.md) tool exists; we may
need a wrapper that scripts the K-trial protocol across a scene matrix
and produces structured CSV output, plus a per-strategy splatting
instrumentation hook in BDPT/VCM for the bias-decomposition step.

This is small (1–2 weeks) and can be done before Phase 1 starts.

---

## 8. Open questions for the user before Phase 1

These are the genuinely-unanswered things that should be resolved
before the analysis kicks off, to avoid spending Phase 1 measurement
budget on the wrong scenes.

### 8.1 What "wide variety of scenes" means concretely

Is the test set:

- **(a) The existing scene corpus** (`scenes/Tests/` + `scenes/FeatureBased/`) interpreted as-is, with whatever distribution it currently has?
- **(b) A curated subset** of the existing corpus weighted toward what you care most about (production-class showcase scenes vs. regression scenes)?
- **(c) An expanded set** including scenes you'd *want* to render but haven't authored because the current integrators handle them poorly (this is a real possibility — scenes you've been avoiding)?

The answer changes Phase 1 scope by 2-3×.

### 8.2 Bias tolerance threshold

You said "physically based as much as possible" and noted VCM is
biased. **How much** finite-N bias is acceptable in a non-default
opt-in mode?

- 1% mean luminance bias on the 90th-percentile scene class is invisible to most viewers.
- 0.1% bias is below detection but constrains radius shrinkage aggressively (slower convergence).
- 0% bias means no consistent estimators in the toolbox at all (no VCM, no MLT).

This calibrates the bias-class taxonomy in §2.

### 8.3 Wall-clock vs SPP equivalence

"Low variance fast" can mean:

- **(a)** Lowest variance at fixed SPP (algorithmic quality per sample).
- **(b)** Lowest variance at fixed wall-clock (algorithmic quality + per-sample cost).
- **(c)** Lowest variance to reach a target quality (time-to-converge).

Path guiding wins on (b)/(c), often loses on (a). VCM wins on (b) for
caustic scenes, loses on (b) for diffuse scenes (photon overhead).
ReSTIR wins on (b)/(c) for hard scenes, possibly loses on (b) for easy
scenes.

We default to (b) but should confirm.

### 8.4 Acceptable feature-completion delay

Some directions have 12+ month sequences. Are you OK with a long
single-direction commitment, or do you prefer shorter "win something
in 3 months" cycles even if the final destination is further?

This determines whether we sequence *deep* (one direction at a time,
fully) or *broad* (parallel small wins across directions).

---

## 9. Anticipated phase outputs

| Phase | Document | Decisions captured |
|---|---|---|
| Phase 1 | `docs/UNIFIED_INTEGRATOR_BASELINES.md` (new) | Empirical scene-by-integrator quality matrix; bias-premium-per-scene-class; reference scene corpus and variance tools used |
| Phase 2 | This document, §5 (revised) | Candidate inventory with bias-class, scope, dependencies, references — refined per Phase 1 |
| Phase 3 | `docs/UNIFIED_INTEGRATOR_DECISION.md` (new) | Selected end-state architecture (one of A/B/C/D or a hybrid), sequencing plan, refactor sequencing (Phase 2b/2c position) |
| Implementation | Per-direction implementation plans | One per workstream once Phase 3 has decided |

Total elapsed time before the implementation plan is finalized:
~2 months (Phase 1 measurement + Phase 2 literature + Phase 3
decision). This is intentional — the worst outcome is a 12-month
implementation in the wrong direction.

---

## 10. What we want from the user before Phase 1 starts

1. Answers to the four open questions in §8.
2. Pointer to scenes currently considered "must render well" — the prioritized subset of the existing corpus.
3. Confirmation on the Phase 2b/2c integrator-refactor sequencing question (§7.2): land before, during, or after the chosen new-feature direction?
4. Confirmation that the spectral-parity audit subtask should run in parallel and feed into Phase 3.
5. Any candidate techniques in §5 worth ruling out up-front (saves literature-review effort).

---

## 11. Cross-references

- [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) — selection guide and feature matrix
- [MIS_HEURISTICS.md](MIS_HEURISTICS.md) — power vs balance, and the principled forward direction
- [MLT_POSTMORTEM.md](MLT_POSTMORTEM.md) — confirmed-not-worth-doing branch
- [INTEGRATOR_REFACTOR_PLAN.md](INTEGRATOR_REFACTOR_PLAN.md) — Pel/NM duplication and the templatization path
- [INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md) — what shipped vs deferred
- [SMS_FUTURE_DIRECTIONS.md](SMS_FUTURE_DIRECTIONS.md) — manifold method candidate inventory
- [VCM.md](VCM.md) — VCM design + spectral proxy details
- [IMPROVEMENTS.md](IMPROVEMENTS.md) — current baseline, ranked roadmap, non-goals
- [skills/variance-measurement.md](skills/variance-measurement.md) — measurement protocol Phase 1 will use
- [skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md) — diagnostic procedure for cross-integrator disagreement
- [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md) — produced by the spawned spectral-parity subtask
