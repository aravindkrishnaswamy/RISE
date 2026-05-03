# Two-Stage SMS Solver: Design and Implementation

## Background

`ManifoldSolver` (Specular Manifold Sampling, Zeltner et al. 2020) Newton-iterates a half-vector chain through specular surfaces between a diffuse shading point and a light. On smooth analytical surfaces (sphere, ellipsoid) it converges 96 % of the time. On Phong-shaded triangle meshes (displaced or normal-mapped) the strict convergence rate collapses to ≤ 30 %, and the rendered caustic is dim because PT's emission-suppression machinery removes ~the same amount of energy regardless of whether SMS recovers it. Diagnostics (see [SMS investigation thread](MLT_POSTMORTEM.md) — actually the live investigation) localised the failure mode: Newton's *line search* fails to make progress when its step crosses a triangle edge, because the per-triangle Jacobian (built from per-triangle UV-Jacobian-inverted derivatives) is C1-discontinuous at edges. Newton stalls near the chord-vs-arc tessellation precision floor (~1e-3 for `detail=128`) without reaching the strict convergence threshold (1e-4).

This is exactly the problem the [paper](https://rgl.epfl.ch/publications/Zeltner2020Specular)'s reference implementation [`tizian/specular-manifold-sampling`](https://github.com/tizian/specular-manifold-sampling) solves with its **two-stage solver**:

1. **Stage 1**: Newton on a *smoothed* version of the surface (C1-continuous). Converges easily.
2. **Stage 2**: Newton on the *actual* surface, seeded from Stage 1's result. Refines to the true caustic with the seed already in the basin of attraction.

The smoothing parameter `s ∈ [0, 1]` is passed through the BSDF (in Mitsuba) or — in our case — through the geometry. `s = 0` is full detail (high-frequency bumps); `s = 1` is the underlying smooth surface (no bumps).

## Goal

Add the two-stage solver to RISE's `ManifoldSolver` and a smoothing-aware analytical-derivative API to `IGeometry`. Match Mitsuba's approach where it makes architectural sense; deviate where RISE's structure is different (smoothing on geometry, not BSDF).

## Non-goals

- Normal-map LEAN smoothing (Olano-Baker 2010). RISE doesn't yet have normal-mapped specular materials in test scenes; defer to future work when that becomes a hot path.
- Geometric-vs-shading-normal split in `ManifoldVertex`. Worth doing eventually for refraction-validity checks but lower priority than the two-stage Newton itself.
- Adaptive β line search (double-on-success). Our halving-only scheme is a different strategy from Mitsuba's; restructuring is out of scope.

## Design

### API surface

A new virtual on `IGeometry`:

```cpp
virtual bool ComputeAnalyticalDerivatives(
    const Point2& uv,
    Scalar smoothing,             // 0 = full detail, 1 = underlying smooth
    Point3&  outPosition,          // object space
    Vector3& outNormal,            // unit, object space
    Vector3& outDpdu, outDpdv,
    Vector3& outDndu, outDndv ) const;
```

Default impl returns `false` — the SMS code keeps its existing FD-probe / per-triangle-Jacobian fallbacks for any geometry that can't answer.

A matching virtual on `IObject` forwards to `IGeometry` and applies the object transform (positions by `m_mxFinalTrans`, tangent vectors by its rotational/scale part, normals and their derivatives by `m_mxInvTranspose`).

### Smoothing semantics

For each geometry, `smoothing` interpolates between the actual surface (`s = 0`) and a "smoothed" reference surface (`s = 1`). The interpolation is **linear in displacement amplitude**:

- `EllipsoidGeometry`: no high-frequency detail to smooth. `smoothing` is ignored — same analytical formulas at any `s`.
- `DisplacedGeometry` (`base + s_disp · f(uv) · N_base`): replace `disp_scale` with `(1 - smoothing) · disp_scale` everywhere. At `s = 1` the displacement vanishes and the chain rule collapses to `m_pBase->ComputeAnalyticalDerivatives(uv, 0, ...)` — the smooth base surface. At `s = 0` it matches the actual displaced mesh.
- Nested `displaced_geometry`: each level applies its own factor. At `s = 1` the entire nesting tower of displacements collapses to the analytical leaf (the innermost `EllipsoidGeometry` etc.).

This is simpler than LEAN/mipmap (no slope-distribution maths) but matches the spirit: at `s = 1` we get a Lipschitz-smooth surface; at `s = 0` we get the truth.

### `ManifoldSolverConfig` additions

```cpp
bool   twoStage           = false;  // Enable two-stage Newton solve
Scalar smoothingStage1    = 1.0;    // Smoothing factor for first pass (1 = fully smooth)
```

Default `twoStage = false` preserves baseline behaviour — opt-in. Scene authors enable it via the rasterizer chunk. Wired through the parser's `ManifoldSolverConfig` parameter list, alongside `sms_max_iterations` etc.

### `ComputeVertexDerivatives` smoothing parameter

`ManifoldSolver::ComputeVertexDerivatives(vertex, smoothing)` gains a smoothing parameter. When `smoothing > 0`, prefer the analytical query (`vertex.pObject->ComputeAnalyticalDerivatives(vertex.uv, smoothing, ...)`) — the only path that knows what to do with a smoothing value. If the query returns false, fall back to the existing FD-probe path (no smoothing applied — best-effort).

### Two-stage flow in `Solve`

After the existing seed-chain construction and `ComputeVertexDerivatives` loop:

```
if config.twoStage and ALL chain vertices' pObject support analytical:
    // STAGE 1: smooth surface
    For each vertex: ComputeVertexDerivatives(v, smoothing=1)
    NewtonSolve(...)                       // converges easily on C1 surface
    if converged:
        // Vertex positions are now on the smooth surface.
        // Re-evaluate at smoothing=0 to land on the actual surface at the same uv.
        For each vertex: ComputeVertexDerivatives(v, smoothing=0)
    else:
        // Stage 1 failed — proceed to Stage 2 with the original on-mesh seed.
        For each vertex: ComputeVertexDerivatives(v, smoothing=0)

// STAGE 2 (always runs, with or without two-stage seeding)
NewtonSolve(...)                           // refines on the actual surface
```

Subtlety at the Stage 1 → Stage 2 transition: Stage 1's converged chain is on the *smooth* surface. To seed Stage 2, we need on-*actual*-surface positions/normals. Two options:

1. **Re-evaluate analytical at smoothing=0** at the same `uv`. Cheap, but the actual mesh might not have a hit at that `uv` if the displacement pulled the surface elsewhere — usually fine for small-amplitude displacement.
2. **Ray-cast back to the mesh** from the smooth-surface position along the smooth normal. More robust but extra cost.

Pick option 1 first; if it shows visible artifacts on highly-displaced surfaces, fall back to option 2.

### Step-size handling within `NewtonSolve`

`UpdateVertexOnSurface` currently ray-casts to the actual mesh after a linear step. With `smoothing > 0` the chain is supposed to live on the smoothed surface, where the ray-cast won't agree (the mesh approximates the *actual* displaced surface, not the smooth one). For Stage 1 we need an alternative step that uses the smoothing-aware analytical query.

`ManifoldSolver::UpdateVertexOnSurface(vertex, du, dv, smoothing)` gains a smoothing parameter. When `smoothing > 0` and analytical is available:
1. Update `vertex.uv += (du, dv)`. Wrap u modulo 1; clamp v to `[0, 1]` (poles).
2. Re-query analytical at the new `(u, v, smoothing)`. Get fresh position/normal/derivatives.
3. Done — no mesh ray-cast.

When `smoothing == 0`, fall through to existing mesh ray-cast snap.

Pole crossings (Newton step takes v out of `[0, 1]`) are clamped. If Newton genuinely wants to cross a pole, line search will halve β until the step stays in-domain. Acceptable for the Veach-egg scene where chains stay far from poles.

### Validity check

Plumbed through the existing `ValidateChainPhysics`: no change. The check uses `vertex.normal` (shading), and that stays self-consistent within each stage.

## Implementation steps

We implement in order, validating each step against both the smooth and displaced Veach-egg scenes:

| # | Step                                                        | Verification                                                                |
|---|-------------------------------------------------------------|-----------------------------------------------------------------------------|
| 1 | Add `ComputeAnalyticalDerivatives(uv, smoothing, ...)` to `IGeometry`, `IObject`, `Object`. Default returns false; concrete `Object` forwards + applies transform. | Build clean. No behaviour change yet. |
| 2 | Override on `EllipsoidGeometry` (smoothing-invariant — analytical formulas).                                                   | Build clean. Smooth scene: still 0.4150 valid/evals (analytical not yet wired into Solve). |
| 3 | Override on `DisplacedGeometry` (chain rule with `effective_disp_scale = (1 - smoothing) · m_dispScale`). At `s = 1` collapses to base-only. | Build clean. Displaced scene: still 0.137 valid/evals. |
| 4 | Add `twoStage`, `smoothingStage1` to `ManifoldSolverConfig`. Wire through the scene-parser to the rasterizer chunks (`sms_two_stage` boolean parameter on `pathtracing_pel_rasterizer` etc.). | Default off. Both scenes unchanged. |
| 5 | Add `smoothing` parameter to `ComputeVertexDerivatives` and `UpdateVertexOnSurface`. When `smoothing > 0` and analytical is available, use it. When `smoothing == 0`, existing fallback. | Default-call sites pass `smoothing = 0`. No behaviour change yet. |
| 6 | Add the two-stage block in `Solve`. Gated behind `config.twoStage`. | With `sms_two_stage TRUE` in the displaced scene, expect Newton convergence rate to climb from ≤ 30 % → ≥ 70 %. |
| 7 | Run all existing SMS test scenes (`scenes/Tests/SMS/`) with `sms_two_stage TRUE` — all should still render without regressions. | No new failures vs. baseline. |

At each step, the existing `SMS_TRACE_DIAGNOSTIC = 1` is flipped on for measurement, then off for production.

## Measured outcomes (post-implementation)

The expected-outcome section above predicted gains on the displaced Veach-egg.  Reality is more nuanced — and matches Mitsuba's actual published demonstration scope after careful reading of the reference implementation.  This section records what the implementation delivers in each regime, with citations.

### Where two-stage works (verified)

**Regime: smooth analytic geometry + perturbed normals.**  Test scene: `scenes/FeatureBased/MLT/mlt_veach_egg_pt_sms_bumpmap.RISEscene` (smooth `ellipsoid_geometry` + `bumpmap_modifier` driving normal perturbation via the same Perlin painter the displaced scene uses for vertex displacement).  At sane bump amplitudes, the two-stage solver delivers the energy-ratio shift the design targets.

| Bump scale (max angle) | Single-stage `valid` / `ΣL_sms/ΣL_supp` | Two-stage `valid` / `ΣL_sms/ΣL_supp` | Two-stage advantage |
|---|---|---|---|
| 10 (≈9°, gentle)   | 0.36 / 0.83          | 0.37 / **1.15**         | +39 % energy |
| 40 (≈33°, medium)  | 0.19 / 0.32          | 0.22 / **0.46**         | +42 % energy |
| 200 (≈90°, heavy)  | 0.016 / 0.019        | 0.018 / 0.032           | +68 % energy |

At gentle bumps the energy ratio matches the smooth-geometry baseline (0.94) within stochastic / MIS-weighting variance — i.e. SMS is recovering essentially all the suppressed caustic energy.

### Where two-stage does *not* help (verified, and matches reference)

**Regime: heavy vertex displacement (e.g. `displaced_geometry` with `disp_scale` > ~10 % of curvature radius).**  At `disp_scale=20` on the Veach-egg's outer ellipsoid (radius 70, so ~28 % relative), single-stage SMS achieves `ΣL_sms/ΣL_supp = 0.16`; two-stage achieves `0.12`.  Two-stage **regresses** the displaced case: the smooth-surface caustic root is *physically* far from the bumpy-mesh caustic root, and Stage 1's converged seed is worse than the original Snell-traced on-mesh seed.

This matches Mitsuba's documented scope.  The reference implementation [`tizian/specular-manifold-sampling`](https://github.com/tizian/specular-manifold-sampling) demonstrates two-stage **only on normal-mapped smooth analytic primitives** — see [`results/Figure_9_Twostage/`](https://github.com/tizian/specular-manifold-sampling/tree/master/results/Figure_9_Twostage), whose `plane.xml` and `sphere.xml` use `<shape type="rectangle">` and `<shape type="sphere">` wrapped in `<bsdf type="normalmap">`.  Their `Figure_16_Displacement` scene **compares displaced meshes vs normal-mapped equivalents but does not engage two-stage at all** — the [`render.py` driver](https://github.com/tizian/specular-manifold-sampling/blob/master/results/Figure_16_Displacement/render.py) never sets `-Dcaustics_twostage=true` for it.

The structural reason in Mitsuba: their two-stage is plumbed through the BSDF's `lean()` method (LEAN mapping, Olano-Baker 2010, slope-distribution moments).  See [`manifold_ss.cpp::sample_path`](https://github.com/tizian/specular-manifold-sampling/blob/master/src/librender/manifold_ss.cpp) lines 254–272:

```cpp
auto [mu, sigma] = si_init.bsdf()->lean(si_init, true);
Point2f slope = SpecularManifold::sample_gaussian(0.5f*(mu+mu_offset), sigma, ...);
Normal3f lean_normal_local = normalize(Normal3f(-slope[0], -slope[1], 1.f));
auto [success_smooth, si_smooth] = newton_solver(si, vtx_init, ei, lean_normal_local, 1.f);
```

Per `bsdf.h`'s explicit comment: "*The default implementation [of `lean`] returns zero, indicating no LEAN information is available for that material.*"  For a plain `dielectric` BSDF on a displaced OBJ mesh, `lean()` is zero → Stage 1's `lean_normal_local` collapses to the actual offset normal → two-stage degenerates to a redundant Newton solve.  **Mitsuba does not claim two-stage helps displaced meshes.**

### Why our geometry-side smoothing fails on displaced meshes

Our RISE implementation is structurally different from Mitsuba's: smoothing is on the *geometry* (`IGeometry::ComputeAnalyticalDerivatives(uv, smoothing, …)`) rather than the BSDF.  This **does** allow smoothing of vertex-displaced surfaces — `DisplacedGeometry` collapses to its base ellipsoid at `smoothing=1`.  But the resulting two-stage flow has a different geometric structure than Mitsuba's normal-map case:

- **Normal-map case (Mitsuba's regime, our bumpmap test).**  Position is invariant under smoothing; only the normal field changes.  Stage 1's converged uv is at the same world position as the bumpy caustic root — Stage 2 starts in the basin of attraction and refines.  Energy ratio approaches 1.0.

- **Displacement case (our extension, paper does not cover).**  Stage 1 walks on the smooth ellipsoid, converging at a uv whose corresponding *displaced-mesh position* is up to `disp_scale` units away from where the smooth-surface chain converged.  Stage 2's seed lands at a point that's *farther* from the bumpy caustic than the original Snell-traced on-mesh seed.  Two-stage actively makes things worse.

### Recommendation

- **Use `sms_two_stage TRUE` on smooth analytic primitives with bump-mapped / normal-mapped specular surfaces.**  Verified to deliver the design's claimed gains.
- **Leave `sms_two_stage FALSE` (default) on heavily displaced specular geometry.**  Single-stage Newton finds whatever caustics the on-mesh Snell-traced seed can reach; two-stage hurts.  For high-quality displaced caustics, the right tool is VCM (photon merging covers the diffuse caustic that single-chain SMS structurally cannot).
- **Lightly displaced geometry** (`disp_scale` < a few percent of the curvature radius) sits in a borderline regime.  Empirically two-stage neither hurts nor helps much there because single-stage already converges.

A future "auto-disable two-stage on heavy-displacement objects" heuristic is plausible but not implemented; it would require per-object tracking of displacement amplitude relative to bounding-sphere radius and a path-tracer-side flag suppressing emission only through SMS-eligible specular surfaces.  See the discussion thread that landed this implementation for design notes.

## Energy-loss compensation for failed SMS attempts (investigated, not adopted)

After landing two-stage we investigated whether Mitsuba's Bernoulli inverse-probability estimator (paper §4.3) could compensate for the residual energy loss on displaced caustics — pulling `ΣL_sms / ΣL_supp` from ~0.13 closer to the unbiased target of 1.0.  Two implementations were tried; **neither produced the expected energy-ratio shift in RISE's setup**, for a structural reason worth recording.

### The Mitsuba framework

Mitsuba implements `T = 1 / p_k` where `p_k` is the probability of finding solution `k` from a fresh sample.  In [`manifold_ss.cpp::specular_manifold_sampling`](https://github.com/tizian/specular-manifold-sampling/blob/master/src/librender/manifold_ss.cpp) the Bernoulli trial loop *re-samples a uniform-random point on the specular caster's surface* and re-runs Newton; the trial inherits the *same* sampling distribution as the main solve (which also draws a uniform-random surface position via `shape->sample_position`).  Because main and Bernoulli share the same proposal distribution, `1/p_uniform` is a meaningful inverse-pdf estimate.  Successful trials count toward `1/p`; unsuccessful trials don't bias the estimator.

### Why it doesn't translate to RISE

RISE's main SMS solve uses `BuildSeedChain(shadingPoint, lightPoint, ...)` — a Snell-traced seed walk from the shading point through the chain of specular casters toward the sampled emitter point.  This is a *different* and analytically opaque distribution from uniform-area surface sampling.

| Implementation | Sampling distribution | ΣL_sms/ΣL_supp on displaced Veach-egg | Why |
|---|---|---|---|
| baseline (biased=TRUE) | Snell-traced seed only | 0.13 | reference |
| Mitsuba-style: fresh uniform-random surface samples per Bernoulli trial | uniform-area on caster | **1.98** | distribution mismatch — trial samples uniformly but `1/p_uniform` overestimates the inverse-pdf of the (different) Snell-traced main draw, so successful paths get a 15× over-amplification |
| RISE-style: seed-perturbation Bernoulli (perturb the Snell seed in tangent plane) | local perturbation around Snell seed | 0.12 | seed-perturbation rarely converges to the *same* root within `maxBernoulliTrials = 100` for displaced meshes — count=0 fallback path returns `pdf=1.0`, no compensation fires |

The first attempt is over-bright by construction: when main and Bernoulli sample from different distributions, the inverse-pdf estimator is meaningless and biases the estimate by the ratio of the two pdfs (here ~15× on average).  The second attempt is correct in principle but Newton's success rate is too low on bumpy meshes for the Bernoulli loop to ever reach `count = targetCount` within the cap — the compensation factor never fires where it would matter most.

### What would actually work

Two paths, both larger than this work's scope:

1. **Two-pass kernel-density estimation** ([Hanika 2015](https://jo.dreggn.org/home/2015_pkk.pdf) PT extension): treat each successful SMS solution as a sample on the photon-image plane, kernel-blur, normalize against trial count.  Doesn't depend on knowing the Snell-trace pdf analytically.

2. **Switch RISE's main SMS solve to Mitsuba-style uniform surface sampling** — replacing `BuildSeedChain` with `shape->sample_position` for the inner specular vertex.  Then Bernoulli inverse-probability becomes principled (same distribution both sides).  Trade-off: loses RISE's photon-guided seed prior (which has its own value for VCM-coupled caustics) and loses the Snell-trace's bias toward the actual caustic root (which makes Newton's basin-of-attraction larger when there *is* a single dominant caustic).  The right thing for an SMS-pure scene; the wrong thing for an SMS-as-NEE-helper scene.

For now we accept the energy loss on displaced caustics and recommend VCM for that regime.  See the parent investigation thread for full diagnostics.

## References

- Tizian Zeltner, Iliyan Georgiev, Wenzel Jakob, "Specular Manifold Sampling for Rendering High-Frequency Caustics and Glints", SIGGRAPH 2020. [project page](https://rgl.epfl.ch/publications/Zeltner2020Specular).
- Reference implementation: [tizian/specular-manifold-sampling](https://github.com/tizian/specular-manifold-sampling), specifically `src/librender/manifold_ss.cpp::sample_path` (two-stage flow) and `include/mitsuba/render/bsdf.h::frame(si, smoothing, ...)` (smoothing API).
- Olano, Baker, "LEAN Mapping", I3D 2010. (Smoothing of normal maps via slope-distribution moments — relevant when we add normal-mapped specular materials.)
