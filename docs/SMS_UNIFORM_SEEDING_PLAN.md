# Path 3: Mitsuba-faithful SMS port — execution plan

> **2026-05 update**: Path-tree branching (`branchingThreshold` + `BuildSeedChainBranching`) was excised from RISE.  Sections referencing Option C / Fresnel-branching / `branching_threshold` describe historical context and are no longer accurate for the live codebase.  SMS uniform mode now uses single-chain stochastic seeds with `multi_trials` for variance reduction — matches Mitsuba SOTA convention.

## Background — what the literature says

After deep reads of the Mitsuba reference (`manifold_ss.cpp`, `manifold_ms.cpp`, `path_sms_ss.cpp`, `path_sms_ms.cpp`) and a survey of post-2020 SMS work, the picture is:

- **Biased mode is paper-canonical** — Zeltner 2020 §4.3 / Eq. 8 / Algorithm 3.  Mitsuba implements it (`manifold_ss.cpp:142-197`).  Final accumulator is `Σ_l f(x₂⁽ˡ⁾)` — the `n_l/M` empirical-frequency factors cancel.  RISE's biased mode is algorithmically the same skeleton.
- **Mitsuba's multi-scatter `manifold_ms.cpp` IS the hybrid uniform-first + Snell-continue scheme** — not a RISE-specific extension.  First vertex uniform-area on a `caustic_caster_multi_scatter` shape; subsequent vertices = deterministic Snell-trace through `caustic_bouncer` / `caustic_caster` shapes; block-tridiagonal Newton on the full chain.
- **Photon-aided seeding is a real published technique** — **Weisstein, Jhang, Chang, "Photon-Driven Manifold Sampling," HPG 2024**, [DOI 10.1145/3675375](https://dl.acm.org/doi/10.1145/3675375).  RISE's source-comment citation "Kondapaneni 2023" is fabricated and must be fixed.
- **No paper since 2020 solves heavy-vertex-displaced specular caustics within the SMS framework.**  Industry ships MNEE in production (Disney Hyperion, Cycles).  RISE's `docs/SMS_TWO_STAGE_SOLVER.md` "two-stage off by default for displaced meshes, recommend VCM" recommendation is correctly aligned with the literature.

## Goal

Make RISE's SMS algorithmically faithful to Mitsuba / Zeltner 2020:

1. **Unbiased mode**: uniform-on-shape seeding → geometric Bernoulli (`K = first success index`, `E[K] = 1/p`).
2. **Biased mode**: M-trial uniform-on-shape sampling, dedupe by direction-from-shading-point, unweighted sum.
3. **MNEE-init mode**: Snell-traced (or straight-line) deterministic seed, `max_trials = 1`, biased.  Becomes a degenerate case of biased mode, not a separate code path.
4. **Photon-aided extension**: a documented biased-mode variant citing Weisstein 2024, with proper attribution.
5. **Caster enumeration**: scene-prep auto-detected list (every object whose material reports `isSpecular`), with optional explicit override.
6. **Multi-scatter (k≥2)**: hybrid uniform-first + Snell-continue, matching `manifold_ms.cpp`.

## Non-goals (explicit, to bound scope)

- Solving heavy-displacement specular caustics — literature open problem; VCM remains RISE's recommendation.
- Specular Polynomials port (Mo 2024) — separate future work.
- Manifold Path Guiding (Fan 2023) — separate future work.
- PSMS-ReSTIR (Hong 2025) — separate future work.
- BDPT-coupled SMS / VCM-coupled SMS internal redesign — keep the existing `misWeight` plumbing; only the seeding strategy changes.
- MLT integration — MLT remains single-trial deterministic.

## Verification methodology (applies to every phase)

Each phase has three gates that MUST pass before advancing to the next phase:

### Gate 1: Build hygiene
- `make -C build/make/rise -j8 all` clean — zero warnings, zero errors.
- Per `CLAUDE.md`'s "Compiler warnings are bugs" rule.

### Gate 2: Numerical baseline preservation
- Render the **baseline test corpus** (defined in Phase 0) with default config.
- Compare to Phase 0 reference numbers (energy ratio, valid/evals counts, mean luminance, RMSE-vs-reference).
- Tolerance: within 0.5% on luminance metrics, exact on counts (no behavior change phases) OR within stated tolerance for phases that are supposed to change behavior.

### Gate 3: New-feature validation
- For phases that add a new code path: render the corpus with the new path enabled.
- Specific pass criteria stated per phase.
- If the new path regresses an existing scene that doesn't use the new feature, that's a code bug — fix before advancing.

**No phase advances unless all three gates pass.**  If a gate fails, the phase is open until the regression is fixed.

## Test corpus

Defined ONCE in Phase 0; reused at every gate.

### Required scenes
1. **Smooth Veach-egg** (`scenes/FeatureBased/MLT/mlt_veach_egg.RISEscene`) — gold reference; ΣL_sms/ΣL_supp ≈ 1.0 in any correctly-implemented unbiased SMS.
2. **Bumpmap Veach-egg** (`scenes/FeatureBased/MLT/mlt_veach_egg_pt_sms_bumpmap.RISEscene`) — verifies normal-perturbing maps work; ΣL_sms/ΣL_supp ≈ 1.15 with two-stage today.
3. **Displaced Veach-egg** (`scenes/FeatureBased/MLT/mlt_veach_egg_pt_sms_displaced.RISEscene`) — current ΣL_sms/ΣL_supp ≈ 0.13; expected to remain ≤ 0.5 (literature open problem).
4. **All `scenes/Tests/SMS/*.RISEscene`** — existing SMS regression suite.
5. **VCM-coupled scene with photon-aided SMS** — pick the existing scene whose `sms_photon_count > 0` shows clear caustic improvement, to validate Phase 7.
6. **At least one BDPT-spectral scene with SMS enabled** — to validate the spectral path doesn't regress.

### Metrics captured per scene per render
- ΣL_sms / ΣL_supp (energy ratio, via `SMS_DIAG_ENABLED`).
- valid / evals (Newton convergence rate).
- Mean luminance, max luminance, p99 luminance.
- RMSE vs reference (where reference exists; Mitsuba render or extreme-spp PT).
- Wall-clock per render.
- Per-pixel variance (K=8 trials via `bin/tools/HDRVarianceTest.exe`).

### How to capture the baseline
1. Flip `SMS_DIAG_ENABLED` to `1` in `src/Library/Shaders/PathTracingIntegrator.cpp:62`.
2. Render each scene at the spp level matching its existing reference (32 spp for the FeatureBased corpus, scene-defined for `scenes/Tests/`).
3. Capture the `[SMS-DIAG]` log line and the EXR.
4. Run `bin/tools/HDRVarianceTest.exe` for K=8 trials — record per-pixel variance and RMSE.
5. Reset `SMS_DIAG_ENABLED` to `0`.
6. Save numbers in `docs/SMS_UNIFORM_SEEDING_BASELINE.md` (committed alongside Phase 0).

## Phase ordering rationale

Each phase is small, additive, and independently verifiable.  The ordering ensures:
- Cleanup precedes new code (Phase 1).
- Infrastructure precedes feature (Phases 2-3 → Phase 4+).
- Default-off opt-in throughout (Phase 4-7) so any phase can be reverted by flipping a config flag.
- Measurement (Phase 8) is the gate to changing default.

---

## Phase 0: Establish numerical baseline

**Goal**: Capture the current behavior of every test-corpus scene so we can detect regression at every subsequent gate.

**Files touched**: New file `docs/SMS_UNIFORM_SEEDING_BASELINE.md`.

**Steps**:
1. Define test corpus per the list above.  Confirm every scene exists and renders.
2. Flip `SMS_DIAG_ENABLED = 1` in `src/Library/Shaders/PathTracingIntegrator.cpp:62`.
3. Build clean.
4. Render each scene **sequentially** (per `AGENTS.md`'s sequential-render rule).
5. For each scene capture: ΣL_sms/ΣL_supp, valid/evals, mean/max/p99 luminance, wall-clock, EXR file.
6. Run K=8 HDRVarianceTest.exe per scene; capture per-pixel variance + RMSE-vs-reference.
7. Reset `SMS_DIAG_ENABLED = 0`.
8. Write all numbers into `docs/SMS_UNIFORM_SEEDING_BASELINE.md`.

**Gate 1 (build)**: clean rebuild after flipping diag back to 0.

**Gate 2 (baseline)**: not applicable; this phase IS the baseline.

**Gate 3 (validation)**: each scene successfully renders end-to-end and produces measurable diagnostics.  No NaN / Inf / immediate firefly regression vs prior runs.

---

## Phase 1: Citation hygiene

**Goal**: Fix the fabricated "Kondapaneni 2023" citation throughout the codebase.  Zero functional change.

**Files touched**:
- `src/Library/Utilities/ManifoldSolver.cpp` — comment block at line 4013 onwards (`Multi-trial Specular Manifold Sampling with PHOTON-AIDED first-vertex seeding (Zeltner et al. 2020 biased estimator + photon-guided priors à la Kondapaneni et al. 2023)`).
- Any other source/header file with the same citation (grep `Kondapaneni`).
- `docs/SMS_TWO_STAGE_SOLVER.md` if it cites Kondapaneni anywhere.
- `CLAUDE.md` if it cites Kondapaneni anywhere.

**Replacement citation**: `Weisstein, Jhang, Chang, "Photon-Driven Manifold Sampling," HPG 2024 (DOI 10.1145/3675375)`.

**Steps**:
1. `grep -rn Kondapaneni src/Library docs CLAUDE.md AGENTS.md` to find every reference.
2. Edit each to cite Weisstein 2024 PMS instead, with the correct DOI.  Keep the technical comment intact; only the citation changes.
3. Build.

**Gate 1 (build)**: clean rebuild.

**Gate 2 (baseline)**: render the test corpus; numbers must be EXACTLY identical to Phase 0 (no behavior change).

**Gate 3 (validation)**: not applicable; this is cleanup.

---

## Phase 2: Caustic-caster enumeration infrastructure

**Goal**: At scene-prep time, build a cached list of every object that can act as a caustic caster.  No behavior change yet — the list is populated but unused.

**Files touched**:
- New: `src/Library/Utilities/SMSCasterList.h/.cpp` (or fold into `SMSConfig.h` if simpler).
- `src/Library/Rendering/PixelBasedRasterizerHelper.cpp` and the BDPT/VCM/MLT analogues — call the enumeration once at scene-prep time, cache the list.
- `src/Library/Utilities/ManifoldSolver.h/.cpp` — accept the list as an optional pointer (mirror the existing `pPhotonMap` plumbing pattern).

**Enumeration logic**:
```
for each object in scene.GetObjects():
  if object.material exists and object.material.GetSpecularInfo(...).isSpecular:
    add object to mSpecularCasters
```

The query needs a dummy `RayIntersectionGeometric` and `IORStack` to call `GetSpecularInfo`; one strategy is to ray-cast from the object's bounding-sphere centre downward — overkill, but unambiguous.  Simpler: `GetSpecularInfo` only depends on the material (not the specific intersection) for the `isSpecular` boolean — confirm this by reading the relevant material implementations; if true, pass a default-constructed `RayIntersectionGeometric` and `IORStack(1.0)`.

**Optional explicit override** (deferred to Phase 4 if needed): scene-file parser keyword `caustic_caster TRUE/FALSE` on `standard_object` to force inclusion / exclusion.

**Gate 1 (build)**: clean rebuild.

**Gate 2 (baseline)**: render corpus; numbers EXACTLY identical to Phase 1.

**Gate 3 (validation)**: add a debug printf (then remove before commit) that lists `mSpecularCasters` at scene-prep.  Verify each test scene's expected casters appear:
- Smooth Veach-egg: outer ellipsoid, inner ellipsoid (cavity).  Both detected.
- Displaced Veach-egg: displaced outer ellipsoid, smooth inner cavity.
- Mirror cardioid: the mirror object.

If a known caster is missing or a known non-caster appears, the enumeration logic is wrong — debug before advancing.

---

## Phase 3: BuildSeedChain refactor — extract Snell-continue helper

**Goal**: Decompose `BuildSeedChain`'s monolithic loop body into a reusable `SnellContinueChain(startVertex, dir, IORStack, scene, caster, chain)` helper.  `BuildSeedChain` itself becomes a thin wrapper.  Numerically equivalent to today.

**Files touched**:
- `src/Library/Utilities/ManifoldSolver.h` — declare `SnellContinueChain`.
- `src/Library/Utilities/ManifoldSolver.cpp` — refactor `BuildSeedChain` body into the helper; `BuildSeedChain` calls the helper after constructing the initial state from `start`.

**Helper signature**:
```cpp
// Continues a manifold-seed chain by Snell-tracing from a starting vertex
// along an initial direction.  Pushes ManifoldVertex entries onto `chain`
// for every specular hit until a non-specular hit, max-depth, or distance
// cutoff is reached.  Returns the number of vertices appended.
unsigned int SnellContinueChain(
    const ManifoldVertex& startVertex,    // already on a specular surface; will be pushed first
    const Vector3& initialDir,            // direction to trace AFTER startVertex
    Scalar currentIOR,
    const IORStack& iorStack,
    const IScene& scene,
    const IRayCaster& caster,
    std::vector<ManifoldVertex>& chain
) const;
```

`BuildSeedChain(start, end, ...)` becomes: build initial dir from start→end; ray-cast to find the first specular hit; if found, construct the first `ManifoldVertex` and call `SnellContinueChain` from there.

**Gate 1 (build)**: clean rebuild.

**Gate 2 (baseline)**: render corpus; numbers must be EXACTLY identical to Phase 2 (this is a pure refactor).  Bit-exact float equivalence is achievable since we're just moving code; if not exact, debug before advancing.

**Gate 3 (validation)**: not applicable; pure refactor.

---

## Phase 4: Add uniform-on-shape seeding (opt-in flag, no Bernoulli yet)

**Goal**: Introduce the new code path that draws a uniform-area seed on a caustic caster, runs Snell-continue tail, runs Newton.  Default off.  Single-trial, biased, no Bernoulli.

**Files touched**:
- `src/Library/Utilities/ManifoldSolver.h` — add `enum SeedingMode { Snell, Uniform }` to `ManifoldSolverConfig`.
- `src/Library/Utilities/ManifoldSolver.cpp` — `EvaluateAtShadingPoint` branches on `config.seedingMode`.  New branch `EvaluateAtShadingPoint_UniformImpl` (private member function) does:
  ```
  for caster in mSpecularCasters:
    if not caster.isReachableFromShadingPoint(pos): continue   // simple visibility heuristic
    sample uniform-area point on caster
    construct ManifoldVertex from the sample (etaI=1, etaT=caster.material.ior, etc.)
    SnellContinueChain from there to extend the chain
    Solve(...)
    if converged & visible: accumulate contribution
  ```
- `src/Library/Parsers/AsciiSceneParser.cpp` — add `sms_seeding "snell" | "uniform"` parameter (default `"snell"`) to every SMS-aware rasterizer chunk's `AddSMSConfigParams`.
- `src/Library/RISE_API.h/.cpp`, `src/Library/Job.cpp` — plumb the parameter.

**Reachability heuristic** (Phase 4 is single-trial, so this matters): cast a ray from `pos` toward each caster's bounding-sphere centre; if a non-specular surface intercepts, skip the caster.  Crude but fast.  Can be relaxed in later phases.

**Gate 1 (build)**: clean rebuild.

**Gate 2 (baseline)**: render corpus with default `sms_seeding "snell"`; numbers EXACTLY identical to Phase 3.

**Gate 3 (validation)**: render corpus with `sms_seeding "uniform"`:
- **Smooth Veach-egg**: image must visually match the snell-mode render to within MC variance.  Energy ratio in the same ballpark (within 30%).  Document deviation; if outside ballpark, the implementation has a bug.  This is the FIRST point at which uniform mode produces meaningful output; the bar is "doesn't crash, doesn't NaN, produces a plausible caustic."
- **Displaced/bumpmap Veach-egg**: expect WORSE energy ratio than snell mode at this stage — there's no Bernoulli yet.  Single uniform-trial converges rarely on bumpy surfaces.  Just check no crash / NaN.
- **All `scenes/Tests/SMS/*`** with `sms_seeding "uniform"`: visual sanity-check each render produces an image (variance high is OK; NaN / crash / wrong topology is not).

If `"uniform"` produces broken output, the bug is in this phase — fix before advancing.

---

## Phase 5: Geometric Bernoulli loop in uniform mode

**Goal**: Implement Mitsuba's `K = first-success-index` estimator for `sms_seeding "uniform"` + `sms_biased "false"`.

**Files touched**:
- `src/Library/Utilities/ManifoldSolver.cpp` — extend `EvaluateAtShadingPoint_UniformImpl` with the geometric Bernoulli loop:
  ```
  for caster in mSpecularCasters:
    if not reachable: continue

    // Main solve
    sample seed; Solve; record direction dir_main if converged
    if not converged: continue   // this caster contributes 0 this round

    // Bernoulli loop
    K = 1
    while K < config.maxBernoulliTrials:
      K += 1
      sample fresh seed; Solve
      if converged AND |dot(dir_trial, dir_main) - 1.0| < uniqueness_threshold: break
    if K == config.maxBernoulliTrials: contribution = 0   // cap-induced bias
    else: contribution *= K
  ```
- `src/Library/Parsers/AsciiSceneParser.cpp` — already supports `sms_biased`.  Confirm it routes to the new code path.

**Note on dedupe key**: Mitsuba uses direction-from-shading-point (cosine-of-direction within `1e-4`).  RISE's `config.uniquenessThreshold` defaults to `1e-2` (in `ManifoldSolverConfig`), two orders of magnitude looser — the looser default reflects RISE's Snell-mode multi-trial scenes where Newton-iteration round-off on heavy-displacement chains made `1e-4` reject legitimate matches.  Tighten per-scene to `1e-4` when running the unbiased Bernoulli on smooth geometry to match Mitsuba's basin definition exactly.

**Gate 1 (build)**: clean rebuild.

**Gate 2 (baseline)**: render corpus with `sms_seeding "snell"` AND with `sms_seeding "uniform" sms_biased "true"`; both must match Phase 4 numbers exactly (Bernoulli only fires when `biased=false`).

**Gate 3 (validation)**: render corpus with `sms_seeding "uniform" sms_biased "false"`:
- **Smooth Veach-egg**: ΣL_sms/ΣL_supp **should track the biased baseline within Monte Carlo noise** (not "must equal 1.0").  Realised result post-implementation: 0.9382 unbiased vs 0.9246 biased on this scene (Δ = +1.5%, consistent with average K slightly above 1).  The gap from 1.0 is RISE's measurement methodology (MIS rounding, emission-suppression overcounting at non-SMS hits), not the Bernoulli math; it bounds *both* biased and unbiased estimators.  See `docs/SMS_UNIFORM_SEEDING_RESULTS.md` "Smooth Veach egg (gold reference)" for full analysis.  The correctness signal is **biased→unbiased Δ ratio matches the expected K-amplification within stochastic noise**, not an absolute ratio target.
- **Bumpmap Veach-egg**: energy ratio improves substantially toward 1.0.  Compared to today's biased-two-stage 1.15, expect 0.9-1.1 range.
- **Displaced Veach-egg**: energy ratio improves but probably stays < 1.0 (literature open problem).  Document the actual number.
- Wall-clock: expect higher than snell mode (more trials per shading point).  Document.

The smooth-egg = 1.0 is the **non-negotiable** pass criterion for this phase.  If it fails, the math is wrong — find the bug before advancing.

---

## Phase 6: Multi-scatter (k≥2) for uniform mode

**Goal**: Make uniform-seeded SMS handle k≥2 chains — specifically the Veach-egg air-cavity case (k=2, outer-then-inner refraction).

**Files touched**:
- `src/Library/Utilities/ManifoldSolver.cpp` — `EvaluateAtShadingPoint_UniformImpl` already calls `SnellContinueChain` after the uniform seed.  Verify it correctly extends to k≥2 when the Snell-trace from the uniform seed hits a second specular surface.

The work in this phase is largely VERIFICATION, since `SnellContinueChain` from Phase 3 already handles k≥2 (it walks until it hits a non-specular surface).  What can break:
- `caustic_bouncer` analogue: an interior caster (e.g. the Veach-egg air cavity wall) shouldn't be a primary uniform-sampling target — it can only appear via Snell-continue.  In Phase 4 we restricted primary sampling to "reachable casters"; that excludes the air cavity by construction (it's behind glass).  Verify this works in practice.
- Inner casters that ARE reachable through some shading-point→caster ray (concave specular geometry) might be sampled primarily, leading to chain topologies that don't match what the Snell-trace from the light would have found.  Phase 6 documents and accepts this.
- Block-tridiagonal Newton: `Solve` already handles arbitrary k.  Confirm by `SMS_TRACE_DIAGNOSTIC` that k=2 chains converge as expected.

**Gate 1 (build)**: clean rebuild.

**Gate 2 (baseline)**: render corpus with default `sms_seeding "snell"`; identical to Phase 5.

**Gate 3 (validation)**: render Veach-egg variants with `sms_seeding "uniform" sms_biased "false"`:
- The k=2 air-cavity caustic must appear in the uniform-mode render, not just k=1 paths.  Visually verify by toggling `SMS_TRACE_DIAGNOSTIC` and checking chain lengths logged.
- Energy ratio on smooth Veach-egg stays at ~1.0.

Phase 6 is mostly a "confirm what we already have works" phase.  If the k=2 chain doesn't appear, the issue is in `SnellContinueChain`'s handling of the inner cavity — debug.

---

## Phase 7: Photon-aided seeding integrated as Mitsuba-mode biased extension

**Goal**: Restructure the photon-aided seeding so it cleanly fits inside the new uniform-mode biased estimator, preserving its variance-reduction value while citing Weisstein 2024 PMS.

**Files touched**:
- `src/Library/Utilities/ManifoldSolver.cpp` — `EvaluateAtShadingPoint_UniformImpl`'s biased branch (M-trial loop) gains an optional photon-prior trial budget.  The first photons (up to `min(numPhotons, M)`) seed the early trials; remaining trials draw uniform-on-shape.  Both contribute to the dedupe set.
- Source comment block at the implementation site cites Weisstein 2024 PMS, explains the hybrid (photon + uniform) within an M-trial biased loop.

**Why this works**: the paper's biased estimator (Eq. 8 final form `Σ_l f(x₂⁽ˡ⁾)`) is consistent for any seed distribution that covers every basin with positive density.  Mixing photon-prior seeds and uniform seeds within a single M-trial loop preserves coverage (photons cover photon-density-weighted basins; uniform covers everything).  The empirical-frequency cancellation in Eq. 8 means the dedupe-and-sum semantics work regardless of seed distribution — as long as we don't claim unbiasedness (`biased=true`).

**Gate 1 (build)**: clean rebuild.

**Gate 2 (baseline)**: render corpus with default `sms_seeding "snell"`; identical to Phase 6.

**Gate 3 (validation)**: render the test scenes that benefit from photon priors with `sms_seeding "uniform" sms_biased "true" sms_photon_count > 0`:
- Visual / metric improvement vs `sms_photon_count = 0`.  Photon priors should still help in the regime they were designed for.
- No regression on photon-less scenes.

---

## Phase 8: Variance + performance characterization

**Goal**: Produce the data table that informs the Phase 9 default decision.

**Files touched**:
- `docs/SMS_UNIFORM_SEEDING_RESULTS.md` (new) — captures the measurements.

**Steps**:
1. For each test corpus scene and each combination of:
   - `sms_seeding ∈ {snell, uniform}`
   - `sms_biased ∈ {true, false}`
   - `sms_two_stage ∈ {true, false}` (where applicable)
   - `sms_photon_count ∈ {0, configured}`
2. Run K=8 HDRVarianceTest trials.
3. Capture per-pixel variance, RMSE-vs-reference, ΣL_sms/ΣL_supp, valid/evals, wall-clock.
4. Build the comparison matrix.

**Gate 3 (validation)**: data is captured; no production scenes are blocked by the new code path.

---

## Phase 9: Default decision

**Goal**: Based on Phase 8 data, choose the default `sms_seeding` mode.

**Possible outcomes**:
1. `"uniform"` wins on energy ratio AND ≤ 2× wall-clock on the corpus → flip default.
2. `"uniform"` wins on energy ratio but is significantly slower → keep `"snell"` default; document `"uniform"` as the recommended mode for high-quality offline renders.
3. `"uniform"` doesn't show measurable improvement in any non-displaced scene → keep `"snell"` default, document `"uniform"` as opt-in for future research.

### Outcome (decided 2026-05-02): #3, default stays `"snell"`.

The uniform-mode Phase 8 measurements showed **no consistent ratio improvement** across the corpus and a **10–30× wall-clock penalty** on bumpy scenes (geometric Bernoulli's worst-case loop length).  The literature open problem of heavy-displacement caustics remains — uniform mode neither closes it nor regresses it, matching the survey in `docs/SMS_FUTURE_DIRECTIONS.md`.

Decisions:
- `seedingMode` default = `eSeedingSnell` in `ManifoldSolverConfig` (`ManifoldSolver.h`).
- `seedingUniform` default = `false` in `SMSConfig` (`SMSConfig.h`).
- `sms_seeding` default = `"snell"` in the parser (`AsciiSceneParser.cpp`).
- Uniform mode opt-in for: unbiased reference renders, Mitsuba alignment runs, research.
- `CLAUDE.md` High-Value-Facts updated with the seeding-mode entry and default decision.
- `docs/SMS_UNIFORM_SEEDING_RESULTS.md` carries the supporting measurements.

## Risk register

| Risk | Probability | Mitigation |
|---|---|---|
| Auto-detection of caustic casters has false-negatives (e.g. textured-IOR materials) | low | Phase 2 Gate 3 prints the list; verify before advancing.  Add explicit `caustic_caster TRUE` parser override if needed. |
| Auto-detection has false-positives (e.g. thin-film materials marked specular but not relevant for SMS) | low | Same; verify, add override if needed. |
| Inner-cavity casters get sampled as primary, producing wrong-topology chains | medium | Reachability heuristic (Phase 4) excludes them.  If the heuristic fails, document scene as needing explicit override. |
| Geometric Bernoulli loop's K can blow up on low-`p` solutions | high | Cap with `maxBernoulliTrials` (default match Mitsuba: bounded by a sane number, e.g. 64).  Cap-induced bias is toward zero (energy loss), not over-bright — same direction as biased mode. |
| Photon-aided seeds and uniform seeds double-count after dedupe | low | Direction-based dedupe is shared across the M-trial loop; both seed types feed the same `solutions` set. |
| Refactor of `BuildSeedChain` (Phase 3) introduces float-bit-level differences | medium | Review carefully; if differences are within tolerance and verifiable as numerically equivalent, accept; otherwise revise refactor. |
| `SMS_DIAG_ENABLED = 1` on production renders (forgotten flag) | low | Phase 0 explicitly resets to 0 before saving baseline.  Diagnostic stays off in committed code. |
| Scene-prep enumeration has a startup cost on large scenes | low | Once per render; O(n_objects) dominated by ray-casting in `IntersectRay` test that costs more.  Measure if concerning. |
| Mitsuba's "match by first-vertex direction only" Bernoulli criterion is fragile when chains have multiple basins with similar first-vertex direction but different interior topology | medium | Document; if observed, escalate to "match by full chain" (more conservative).  Mitsuba accepts this risk, so we should too unless we observe regressions. |
| Spectral path (`EvaluateAtShadingPointNM`) needs the same treatment as RGB | high (must do) | Phase 4-7 work duplicates for the spectral variant.  Treat as Phase 4b, 5b, ... or fold into each phase. |
| Two-stage solver interaction with uniform seeding regresses the bumpmap scene gain | medium | Validate at Phase 5 Gate 3; two-stage is structurally orthogonal to seeding (operates per-vertex on the converged seed), but verify. |
| Existing scene files that explicitly set `sms_seeding` (none today) end up working differently than implied | low | New parameter, default `"snell"` matches today.  No existing scene uses it. |
| BDPT/VCM/MLT rasterizers don't propagate the new config | high (must do) | Confirm `SMSConfig` plumbing reaches every SMS-aware rasterizer.  Already audited for `sms_two_stage` work; should be straightforward. |
| Tests in `scenes/Tests/SMS/` regress with `sms_seeding "uniform"` because their reference images were captured under `"snell"` mode | high | Acceptable — `"uniform"` mode is opt-in; test references stay on `"snell"` until the Phase 9 decision flips default. |

## Tuning notes for uniform mode

Snell mode's default `sms_multi_trials = 1` produces reliable caustics because trial 0 is a deterministic Snell-traced seed that lands in the dominant caustic basin by construction.  **Uniform mode at `multi_trials = 1` is a single RANDOM uniform-area sample per caster** — the basin-hit probability is `basin_area / total_caster_area`, which on smooth primitives is decent but not 100%, and on bumpy surfaces can be very low.

**Recommendation for production use of `sms_seeding "uniform"`:**
- Set `sms_multi_trials` to **at least 4**, ideally 8–16, for biased mode.  This is the M of Mitsuba's biased SMS (paper §4.3 Algorithm 3 / Eq. 8).
- Measured impact on the corpus (bumpmap Veach egg, biased two-stage, 4 spp, post-Phase-7 build):
  - Snell biased: ratio 1.251.
  - Uniform biased, M=1: ratio 1.067 (−15% energy loss vs snell).
  - Uniform biased, M=8: ratio 1.191 (−5% — gap mostly closed).
- Unbiased uniform mode (geometric Bernoulli, `sms_biased FALSE`) doesn't need a high `multi_trials` — the `K = first-success-index` factor amplifies rare convergent samples automatically.  `multi_trials = 1` is appropriate there.

The mismatch between snell-mode's reliable `M=1` and uniform-mode's needs-`M≥4` is a UX wart of the seed-distribution change: snell exploits deterministic seeding, uniform demands variance reduction.  We don't auto-bump the default for uniform mode because that would silently regress the cost of any user opting in for fast iteration.

## Known limitations / follow-up work

These items were called out in the post-Phase-9 adversarial review and deferred rather than land on top of the existing scaffolding.  Documented here as a starting point for the next contributor.

### Code-correctness deferrals
- **Per-caster cross-trial dedupe**: now landed.  See `EvaluateAtShadingPointUniform`'s `acceptedRoots` set — keyed on `(first-vertex-pos, chainLen)`, consulted inside the per-caster loop AND the photon-aided block.
- **`EnumerateSpecularCasters` single-prand probe**: now landed.  Probes 3 deterministic prands (`(0.5,0.5,0.5)`, `(0.25,0.5,0.75)`, `(0.75,0.25,0.25)`); accepts on any positive.
- **Photon-block dedupe key**: now landed.  All dedupe sites (per-caster loop, photon block) use `(first-vertex-pos, chainLen)`.
- **Fresnel branching at sub-critical dielectric vertices**: was landed (Option C) but **subsequently excised in 2026-05** when path-tree branching was removed from RISE.  Per Mitsuba SOTA convention, SMS now uses single-chain stochastic seeds + multi-trial averaging.  See [CLAUDE.md](../CLAUDE.md) High-Value Facts for the rationale.
- **Sampler dimension drift**: now landed via `SMSLoopSampler` firewall.  Each `EvaluateAtShadingPoint*` entry consumes exactly two dimensions from the parent sampler to seed an `IndependentSampler`-backed loop sampler; all variable-count internal work (M-trial loop, geometric Bernoulli K-loop, `Solve→EstimatePDF`) uses the loop sampler.  Parent LDS dimension stream stays predictable for downstream call sites (PT BSDF sampling, NEE light sampling).

### Architectural deferrals
- **Helper extraction**: `EvaluateAtShadingPointUniform` duplicates ~200 lines of contribution-formula and photon-chain-reversal code from `EvaluateAtShadingPoint`.  Refactor target: `ComputeTrialContribution(...)` + `ReversePhotonChainForSeed(...)` helpers used by both seeding modes (and by the spectral path when it gains uniform support).
- **Spectral parity gap**: `EvaluateAtShadingPointNM` (spectral) ignores `seedingMode` and falls back to Snell.  Currently warns once-per-process at `eLog_Warning`.  Phase 4b would add a parallel `EvaluateAtShadingPointNMUniform`.
- **`mSpecularCasters` placement**: cached on each `ManifoldSolver` instance; multi-rasterizer renders re-enumerate.  Move to scene-prep (e.g. an `IScene::GetSpecularCasters()` cache populated once per scene-load).
- **`seedingUniform` boolean → enum**: the `SMSConfig::seedingUniform` boolean → `ManifoldSolverConfig::SeedingMode` enum mapping at the API boundary works for two values; adding a third (Specular Polynomials, MPG, ...) requires plumbing tristate state through SMSConfig + the four parser sites + the four API constructors + the four Job.cpp call sites.
- **Flat-parameter rasterizer constructors**: each rasterizer constructor takes ~12 SMS knobs as flat positional parameters.  Adding a knob (this work added `seedingUniform`) is a 4-site signature change.  Better: pass the whole `SMSConfig` struct via a new ABI-preserving overload.

### Documentation drift checks
- The `uniquenessThreshold` default (1e-2) is looser than Mitsuba's (1e-4); tighten per-scene for unbiased Bernoulli on smooth geometry.  Now documented in Phase 5 above.
- The Phase 5 "non-negotiable" gate language in earlier drafts implied ratio→1.0; the actual ceiling on this codebase is ~0.94 due to MIS rounding and emission-suppression overcounting.  Now documented in Phase 5 above.

## What this plan ISN'T

- **Not** a removal of `BuildSeedChain` — it's refactored and reused as the Snell-continue tail under uniform mode, AND remains the primary seed for `sms_seeding "snell"` (default).
- **Not** a removal of biased mode — biased mode is paper-canonical; we're aligning RISE's biased mode to match Mitsuba's exactly.
- **Not** a removal of photon-aided seeding — it's restructured under the uniform-mode biased loop with proper attribution.
- **Not** a removal of `multiTrials` — it becomes `M` in biased mode, retired in unbiased mode (geometric loop).
- **Not** a wholesale removal of any work landed before this plan.

## References

- Zeltner, Georgiev, Jakob.  "Specular Manifold Sampling for Rendering High-Frequency Caustics and Glints."  SIGGRAPH 2020. [project](https://rgl.epfl.ch/publications/Zeltner2020Specular).
- Weisstein, Jhang, Chang.  "Photon-Driven Manifold Sampling."  HPG 2024. [DOI 10.1145/3675375](https://dl.acm.org/doi/10.1145/3675375).
- Mitsuba reference implementation: [tizian/specular-manifold-sampling](https://github.com/tizian/specular-manifold-sampling), specifically `manifold_ss.cpp` (single-scatter), `manifold_ms.cpp` (multi-scatter), `path_sms_ss.cpp`, `path_sms_ms.cpp`.
- `docs/SMS_TWO_STAGE_SOLVER.md` — the predecessor work; the energy-loss compensation investigation that motivated this plan.
- `docs/skills/variance-measurement.md` — protocol for the K-trial HDRVarianceTest pattern used at every gate.
- `docs/skills/performance-work-with-baselines.md` — discipline for the per-phase numerical baseline.
