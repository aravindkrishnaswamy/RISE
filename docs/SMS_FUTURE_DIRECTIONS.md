# SMS Future Directions: Literature & Candidate Extensions

This doc captures the post-2020 SMS literature surveyed during the planning of `docs/SMS_UNIFORM_SEEDING_PLAN.md` (Option A), plus the candidate future extensions (Option B and beyond) considered and deferred.  The current work is Option A — bringing RISE's SMS into algorithmic alignment with the Zeltner 2020 paper / Mitsuba reference.  Future work may revisit any of the candidates below.

## Why this doc exists

After the literature survey reframed our understanding (see `docs/SMS_TWO_STAGE_SOLVER.md` and the planning thread for context), we identified three options:

- **Option A — Mitsuba-faithful core, properly-cited extensions.** Refactor RISE's SMS to match the published reference for unbiased / biased / MNEE-init modes, fix the fabricated Kondapaneni 2023 citation to Weisstein 2024 PMS, restructure photon-aided seeding as a documented biased-mode extension.  See `docs/SMS_UNIFORM_SEEDING_PLAN.md`.  **Currently in progress.**
- **Option B — Port a newer paper.** Implement one of the published post-2020 SMS extensions — most likely Specular Polynomials (Mo 2024) for deterministic seed-finding, or Manifold Path Guiding (Fan 2023) for k≥3 chain robustness.  Bigger lift than A; addresses different failure modes.  **Deferred.**
- **Option C — Retire the "SMS for displaced meshes" goal.** Accept the literature consensus that no published paper solves heavy-vertex-displaced specular caustics inside SMS; document VCM as the production answer for that regime.  **Captured as a non-goal in `docs/SMS_TWO_STAGE_SOLVER.md` and `docs/SMS_UNIFORM_SEEDING_PLAN.md`.  Effectively the current policy.**

## Literature snapshot (post-2020)

Brief summaries of every relevant paper located during the survey, ordered roughly by relevance to RISE.

### Specular-chain manifold methods (descendants of Zeltner 2020)

#### Photon-Driven Manifold Sampling (PMS) — Weisstein, Jhang, Chang, HPG 2024

[DOI 10.1145/3675375](https://dl.acm.org/doi/10.1145/3675375) · [HPG slides](https://www.highperformancegraphics.org/slides24/Photon-Driven%20Manifold%20Sampling%20HPG.pdf)

Deposits multi-bounce caustic photons during a scene-prep light pass; queries them at render time to seed SMS Newton iterations.  Remains unbiased via the same Bernoulli inverse-probability framework as Zeltner 2020.  **This is what RISE actually implements** (via `SMSPhotonMap` / `kSMSMaxPhotonChain`), although our source comments incorrectly cite "Kondapaneni 2023."  Phase 1 of `SMS_UNIFORM_SEEDING_PLAN.md` fixes the citation.

**Status in RISE**: shipped, mis-cited.  Phase 1 corrects.

#### Specular Manifold Bisection Sampling (SMBS) — Jhang & Chang, Pacific Graphics 2022

[DOI 10.1111/cgf.14673](https://onlinelibrary.wiley.com/doi/abs/10.1111/cgf.14673)

Adds an MLT-style "large mutation" bisection step when Newton diverges; reports ~2× success rate on complex refractive geometry.  Architecturally cheap to add — slots into the existing Newton loop as a fallback when line-search halving stalls.

**Implementation effort**: 1-2 weeks.  Modifies `NewtonSolve`'s line-search; touches `ManifoldSolver.cpp` only.  Compatible with both Snell-traced and uniform seeding.

**Why it matters for RISE**: The displaced-mesh Newton-divergence problem (`docs/SMS_TWO_STAGE_SOLVER.md`) is *partly* a basin-of-attraction problem.  SMBS doesn't widen the basin but recovers when Newton over-shoots.  Likely incremental but measurable improvement on bumpy meshes.

#### Manifold Path Guiding (MPG) — Fan, Hong, Guo, Zou, Guo, Yan, SIGGRAPH Asia 2023

[DOI 10.1145/3618360](https://dl.acm.org/doi/abs/10.1145/3618360) · [arXiv 2311.12818](https://arxiv.org/abs/2311.12818) · [code](https://github.com/mollnn/manifold-path-guiding)

First general framework for k≥3 specular chains.  Builds a progressive guiding distribution over historical sub-paths, used to seed manifold walks adaptively.  Reports up to 40× variance reduction on long chains vs vanilla SMS.

**Implementation effort**: significant — 4-8 weeks.  Requires building a learned proposal distribution (sub-path histogram or neural-net-flavored variant) at scene prep and updating it during render.  Touches the seed-construction code path in a fundamental way.

**Why it matters for RISE**: scenes with k≥3 chains (e.g. multi-bounce mirror caustics, complex glass tableware) are currently impractical with SMS in any renderer.  MPG is the published state-of-the-art.

**Compatibility with Option A**: MPG's guiding distribution can replace the `caustic_caster` enumeration from Option A's Phase 2 — it learns which casters / chain topologies to seed.  An Option-A-then-MPG sequencing is natural.

#### Specular Polynomials — Mo, Bai, Sun, Yang, Wang, SIGGRAPH 2024 — **NEXT TARGET**

[DOI 10.1145/3658132](https://dl.acm.org/doi/10.1145/3658132) · [arXiv 2405.13409](https://arxiv.org/abs/2405.13409) · [code](https://github.com/mollnn/spoly)

Replaces Newton iteration with **deterministic polynomial root-finding**.  Encodes specular constraints as a polynomial system, eliminates variables via resultants, returns *all* admissible paths in one call.  No basin-of-attraction; no firefly fragility.  **Limited to k≤3 in practice** because the resultant elimination scales poorly.  Open-source reference implementation.

**Implementation effort**: significant — 6-10 weeks.  Replaces the Newton machinery (`Solve`, `NewtonSolve`, `BuildJacobian`, `EvaluateConstraint`, `UpdateVertexOnSurface`).  The constraint system itself is reused.

**Why it matters for RISE — promoted to next-target priority:** post-Option-A measurements (`docs/SMS_UNIFORM_SEEDING_RESULTS.md`) confirmed that the bottleneck on heavily-displaced caustics is **Newton's basin of attraction on bumpy normals**, not the seeding strategy.  Specifically:

- Smooth Veach egg with our fixes: ratio 0.93 → 0.94 (close to 1.0; Fresnel paths recovered).
- Displaced Veach egg with our fixes (snell mode + branching): ratio still 0.37 — even with a perfect refraction-basin seed, Newton fails on ~77% of trial-0 attempts because the half-vector constraint is C¹-discontinuous at every triangle edge of the displaced mesh.

This is the open literature problem.  Two-stage solver (Zeltner 2020 §5) doesn't help on heavily-displaced meshes (Mitsuba's reference scope confirms; see `docs/SMS_TWO_STAGE_SOLVER.md`).  SMBS (Jhang & Chang 2022) is a marginal Newton-rescue.  **Specular Polynomials structurally bypasses the problem** by not iterating from a seed at all — it solves the polynomial system once and returns every root.

Add this as a NEW seeding/solving path (`sms_solver "newton" | "polynomial"`), keep Newton as default for compatibility, switch to polynomial for k ≤ 3 displaced scenes.

**Compatibility with Option A**: largely orthogonal.  Option A produces correctly-seeded Newton solves; Specular Polynomials would replace the solver entirely.  Sequenced: Option A landed (this work) → **Specular Polynomials next** → optionally drop Newton when polynomial path proves out.

**Architectural sketch:**
- New parser parameter `sms_solver "newton"|"polynomial"` (default `"newton"` for compatibility).
- New `ManifoldSolver::SolvePolynomial(...)` returning `std::vector<ManifoldResult>` (multiple roots per call).
- New chain construction: instead of seeding + Newton walk, the polynomial system is built directly from `(shading-point, light-point, caster surface representation)`.  Caster geometry needs to expose a polynomial form (analytic ellipsoid: directly; mesh: per-triangle local polynomial fit).
- Integration: `EvaluateAtShadingPoint` branches on `config.solverMode`; polynomial path bypasses `BuildSeedChainBranching` / `BuildSeedChain` entirely — its outputs feed directly into the contribution formula via `ComputeTrialContribution`.
- Reuses everything downstream: `ComputeTrialContribution`, `EvaluateChainThroughput`, `CheckChainVisibility`, the dedupe set, the photon extension.

#### Batch Specular Manifold Sampling — Lou, Wang, Wei, Liu, The Visual Computer 2025

[DOI 10.1007/s00371-025-03955-0](https://link.springer.com/article/10.1007/s00371-025-03955-0)

Variance-reducing batch allocation of Bernoulli trials.  Pools work across nearby pixels.  Modest gains; minor architectural impact.

**Implementation effort**: 2-3 weeks.

**Why it matters for RISE**: variance reduction at fixed compute budget on existing Bernoulli loops.  Lower-priority than MPG / Specular Polynomials; nice-to-have.

#### PSMS-ReSTIR — Hong, Duan, Wang, Yuksel, Zeltner, Lin, SIGGRAPH Asia 2025

[DOI 10.1145/3757377.3763927](https://dl.acm.org/doi/10.1145/3757377.3763927) · [NVIDIA project page](https://research.nvidia.com/labs/rtr/publication/hong2025partition/) · [Utah project page](https://graphics.cs.utah.edu/research/projects/psms-restir/)

Tile-based seed-space partitioning + ReSTIR temporal reuse for *interactive* SMS.  Tizian Zeltner (original SMS author) is co-author.  Goal is real-time / animation-rate caustics; quality follows from temporal reuse rather than per-pixel sample count.

**Implementation effort**: very significant — requires ReSTIR infrastructure (RISE doesn't have it today), plus temporal-reservoir buffers across frames.  More appropriate for a real-time / GPU-pivoted RISE roadmap.

**Why it matters for RISE**: out of scope today.  Captured for completeness.

### Adjacent / orthogonal techniques

#### MNEE — Hanika, Droske, Fascione, EGSR 2015

[DOI 10.1111/cgf.12681](https://onlinelibrary.wiley.com/doi/abs/10.1111/cgf.12681) · [PDF](https://jo.dreggn.org/home/2015_mnee.pdf)

Direct ancestor of SMS; deterministic single-walk manifold NEE.  **Strictly weaker than RISE's current SMS** — single-trial, no Bernoulli, no photon prior.  Production renderers (Disney Hyperion, Cycles) ship MNEE *because* it's simpler, not because it's better quality.

**Status in RISE**: subsumed by SMS.  Mitsuba's `mnee_init=true, biased=true, max_trials=1` mode reproduces MNEE within the SMS framework — Option A automatically gets this for free as a degenerate case.

#### Bernstein Bounds for Caustics — Schaufler et al., SIGGRAPH 2025

[DOI 10.1145/3731145](https://dl.acm.org/doi/10.1145/3731145) · [arXiv 2504.19163](https://arxiv.org/abs/2504.19163)

Bounds per-triangle-tuple irradiance contributions and samples accordingly; structurally suited to displaced meshes.  **Capped at k≤2 specular vertices.**

**Implementation effort**: significant — 6-8 weeks.  Different mathematical machinery from SMS (interval analysis on triangle tuples, not Newton on chain vertices).  Doesn't replace SMS but addresses a regime SMS doesn't handle.

**Why it matters for RISE**: closest published candidate for the **heavy-vertex-displaced caustic problem** — the regime where `docs/SMS_TWO_STAGE_SOLVER.md` documents SMS giving up.  If RISE wants to render displaced caustics correctly without VCM, Bernstein Bounds is the most promising direction.

**Compatibility with Option A**: orthogonal — Bernstein Bounds is a separate sampling technique that runs alongside (or instead of) SMS for specific scene types.

#### Online Photon Guiding with 3D Gaussians — Xu et al., 2024

[arXiv 2403.03641](https://arxiv.org/abs/2403.03641)

Guides *photon emission*, not SMS seeding.  Could compose with PMS-style photon-aided manifold seeding to make the photons themselves more useful.

**Implementation effort**: medium-high.  Requires a 3D-Gaussian fitting pipeline at scene prep.

**Why it matters for RISE**: could improve PMS's photon density specifically in caustic-relevant regions, lifting the variance-reduction floor on photon-aided SMS.

#### Caustic Connection Strategies for BDPT — Pixar

[paper](https://graphics.pixar.com/library/CausticConnections/paper.pdf)

Production approach to caustics via BDPT extensions, not manifold methods.  Captured for completeness; not a candidate for porting (RISE's BDPT path is separate from SMS).

### Production-renderer notes

- **Disney Hyperion**: MNEE for eye caustics (Burley et al. ToG 2018).  Procedural eye geometry + manifold NEE.  Production validation that *some* manifold method earns its keep.
- **Cycles**: MNEE-derived "shadow caustics" mode (commit [`1fb0247`](https://projects.blender.org/blender/blender/commit/1fb0247)).  Limitations: refractive only, in shadows only, max 4 bounces, requires smooth normals, no Metal backend.  An [Uppsala Master's thesis](https://www.diva-portal.org/smash/get/diva2:1985010/FULLTEXT02.pdf) on integrating full Zeltner-2020 SMS into Cycles is publicly available with honest performance numbers.
- **Hanika 2019, "Path Tracing in Production"**: [PDF](https://jo.dreggn.org/path-tracing-in-production/2019/johannes_hanika.pdf).  Pragmatic survey of MNEE-class techniques in production.

## Decision matrix for revisiting this list

For each candidate technique, the question is: "what specific RISE problem would this solve?"

| Candidate | Problem it solves | When to revisit |
|---|---|---|
| **MPG (Fan 2023)** | k≥3 chain robustness; complex glass tableware, multi-bounce mirror chains | when a user reports a scene with k≥3 SMS chains where vanilla SMS produces excessive noise |
| **Specular Polynomials (Mo 2024)** | Newton fragility; firefly outliers in tight basins | when SMS Newton-divergence is the dominant variance source on a target scene class |
| **SMBS (Jhang & Chang 2022)** | mild Newton-divergence rescue; ~2× success rate cheap | as a quick win after Option A lands; ~1-2 weeks of work |
| **Bernstein Bounds (Schaufler 2025)** | heavy-vertex-displaced caustics; the gap `docs/SMS_TWO_STAGE_SOLVER.md` documents | when displaced-caustic quality is a user-blocking issue and VCM isn't acceptable |
| **PSMS-ReSTIR (Hong 2025)** | interactive / real-time SMS | when RISE pivots toward real-time; out of scope today |
| **Batch SMS (Lou 2025)** | nice-to-have variance reduction | low priority; revisit only if other paths are blocked |

## Why Option A first

Option A is sequenced before any of the above because:
1. **It's a prerequisite for honest comparison.**  Comparing RISE-with-fabricated-citation against MPG / Specular Polynomials is comparing-apples-to-oranges; we don't actually know what the baseline is.  Option A produces a known-faithful SMS implementation against which to measure the next paper.
2. **It removes one fabricated citation from the codebase.**  Code-comment integrity matters; this isn't optional.
3. **It's the cheapest of the candidates** by a wide margin (~1-2 weeks vs 6-10 for Specular Polynomials / MPG).
4. **It strictly dominates the current state on energy ratio** for the smooth + normal-mapped regime (mathematical guarantee from the geometric Bernoulli estimator).
5. **Every later candidate slots cleanly on top of Option A** — none of them require ripping out the uniform-seeding work.

## What "explore Option B" would look like

If/when we revisit, the natural sequence is:

1. **Pick a target scene class** that motivates the work — e.g. "multi-bounce mirror caustics" → MPG; "displaced-mesh caustics where VCM is unacceptable" → Bernstein Bounds; "Newton firefly elimination" → Specular Polynomials.
2. **Read the candidate paper end-to-end** with the same depth we read Zeltner 2020 (multi-agent literature review, source-code inspection of the reference implementation if available).
3. **Write a planning doc** matching `docs/SMS_UNIFORM_SEEDING_PLAN.md`'s structure — phased, with verification gates.
4. **Capture a numerical baseline** (Phase 0 equivalent) on the target scene class with Option A's SMS.
5. **Implement against the baseline** with each phase verified.
6. **Document the result** (success or failure, measured numbers) in this directory.

The discipline of `docs/skills/performance-work-with-baselines.md` and `docs/skills/variance-measurement.md` applies regardless of which candidate is chosen.

## References (consolidated)

- Specular Manifold Sampling — Zeltner et al. 2020 ([RGL/EPFL](https://rgl.epfl.ch/publications/Zeltner2020Specular))
- Mitsuba reference — [tizian/specular-manifold-sampling](https://github.com/tizian/specular-manifold-sampling)
- Manifold Next Event Estimation — Hanika et al. 2015 ([Wiley](https://onlinelibrary.wiley.com/doi/abs/10.1111/cgf.12681))
- Photon-Driven Manifold Sampling — Weisstein, Jhang, Chang, HPG 2024 ([ACM](https://dl.acm.org/doi/10.1145/3675375))
- Specular Manifold Bisection Sampling — Jhang & Chang 2022 ([Wiley](https://onlinelibrary.wiley.com/doi/abs/10.1111/cgf.14673))
- Manifold Path Guiding — Fan et al. 2023 ([ACM](https://dl.acm.org/doi/abs/10.1145/3618360), [arXiv](https://arxiv.org/abs/2311.12818), [code](https://github.com/mollnn/manifold-path-guiding))
- Specular Polynomials — Mo et al. 2024 ([ACM](https://dl.acm.org/doi/10.1145/3658132), [arXiv](https://arxiv.org/abs/2405.13409), [code](https://github.com/mollnn/spoly))
- Batch Specular Manifold Sampling — Lou et al. 2025 ([Springer](https://link.springer.com/article/10.1007/s00371-025-03955-0))
- PSMS-ReSTIR — Hong et al. 2025 ([ACM](https://dl.acm.org/doi/10.1145/3757377.3763927), [NVIDIA](https://research.nvidia.com/labs/rtr/publication/hong2025partition/))
- Bernstein Bounds for Caustics — Schaufler et al. 2025 ([ACM](https://dl.acm.org/doi/10.1145/3731145), [arXiv](https://arxiv.org/abs/2504.19163))
- Online Photon Guiding with 3D Gaussians — Xu et al. 2024 ([arXiv](https://arxiv.org/abs/2403.03641))
- Caustic Connection Strategies for BDPT — Pixar ([PDF](https://graphics.pixar.com/library/CausticConnections/paper.pdf))
- Cycles MNEE shadow caustics — [Blender commit](https://projects.blender.org/blender/blender/commit/1fb0247)
- Cycles SMS Master's thesis (Uppsala 2025) — [DiVA](https://www.diva-portal.org/smash/get/diva2:1985010/FULLTEXT02.pdf)
- Disney Hyperion (Burley et al. ToG 2018) — [Karl Li blog](https://blog.yiningkarlli.com/2018/08/hyperion-tog-paper.html)
