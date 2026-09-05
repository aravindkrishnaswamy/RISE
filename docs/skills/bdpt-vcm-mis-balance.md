---
name: bdpt-vcm-mis-balance
description: |
  Disciplined procedure for diagnosing BDPT and VCM correctness bugs that
  manifest as bias, fireflies, or "splotches" against a path-traced
  reference.  Use when: a BDPT or VCM render disagrees with PT (mean
  off, max blown out, pixels with structured noise that doesn't
  decrease with sample count); user reports "fireflies even at high
  spp"; or you've just changed MIS code (`MISWeight`, `EvaluateNEE`,
  `EvaluateMerges`, `Convert*Subpath`, the auto-radius pre-pass).
  Walks through the PT-vs-X property, isolation tests for individual
  strategies, the recurring "delta-light vs delta-surface" trap, and
  the running-quantity instrumentation pattern that pinpoints which
  strategy / vertex / pdf is producing the wrong value.  Starts with
  pre-flight checks for the known NON-MIS causes of the same
  symptom: output-layer splat loss in the plain file when
  `oidn_denoise` is on (compare against `_denoised`),
  light-surface sampler density bugs (deficit tracks the emitter
  shape), and a reference path tracer that is itself broken --
  including the case where the harness's chosen PT is the legacy
  `pixelpel_rasterizer`.
---

# BDPT / VCM MIS Balance Diagnosis

## When To Use

- BDPT or VCM render disagrees with PT on a scene where they should
  converge (no caustics, no specular SDS chains, no transmissive
  shadow specifics).
- Image has visible "splotches", fireflies, or texture-like patterns
  that don't smooth out at high spp.
- Mean / median / p99 / max comparison against PT trips a tolerance
  in `BDPTStrategyBalanceTest` or `VCMStrategyBalanceTest`.
- You changed code in:
  - [BDPTIntegrator::MISWeight](../../src/Library/Shaders/BDPTIntegrator.cpp) — power-heuristic walk
  - [BDPTIntegrator::ConnectAndEvaluate](../../src/Library/Shaders/BDPTIntegrator.cpp) — per-strategy contribution + pdfRev updates
  - [VCMIntegrator::EvaluateNEE / EvaluateMerges / SplatLightSubpathToCamera](../../src/Library/Shaders/VCMIntegrator.cpp) — VC + VM strategies
  - [VCMRecurrence.cpp / .h](../../src/Library/Shaders/VCMRecurrence.cpp) — `InitLight`, `InitCamera`, `ApplyGeometricUpdate`, `ApplyBsdfSamplingUpdate`, `Convert*Subpath`
  - [VCMRasterizerBase::PreRenderSetup](../../src/Library/Rendering/VCMRasterizerBase.cpp) — `foundSpecular`, auto-radius
- A new light type (delta-direction, environment, mesh emitter) was
  added or modified and the MIS code wasn't simultaneously taught
  about it.

## When NOT To Use

- Caustic scenes (glass / mirror dominating the path budget).  PT and
  BDPT/VCM converge to the same answer in the limit but at very
  different rates: PT under-samples specular paths, BDPT/VCM finds
  them with high variance.  PT-vs-BDPT mean disagreement on caustic
  scenes is a sampling efficiency disparity, not an MIS bug.  The
  right tool there is SMS or path guiding, not MIS surgery.
- Variance that scales with `1/sqrt(N)`.  Render at 2× and 16× spp:
  if the disagreement shrinks, it's noise.  If it stays put, it's
  bias (the bug this skill fixes).

## Procedure

### 0. Rule out the seven known non-MIS causes first

Seven failure modes produce exactly the "bidirectional render
disagrees with PT" symptom (or, in cause 3's case, "PT itself
disagrees with its own material's proven-linear response"; or, in
cause 4's case, "BDPT/VCM looks like it's over-counting when PT is
actually the one under-counting") while the MIS arithmetic is
perfectly healthy.  All are minutes to check; do them before any
integrator instrumentation:

0. **PT may be the broken one — check IOR-stack seeding when the
   camera (or an emitter) sits inside a dielectric.**  (Found
   2026-08-13 on `vcm_sdf_luminaire_jellyfish`: VCM read 2.2× PT's
   mean and was blamed, but PT was losing ~8× of the env energy.)
   A transport walk that STARTS inside a dielectric must seed its
   IORStack from the start point (`IORStackSeeding::SeedFromPoint`)
   or DielectricSPF misclassifies the first boundary crossing
   (`bFromInside==false`) and silently DROPS the delta-transmission
   lobe.  BDPT/VCM eye+light subpaths have always seeded;
   PathTracingIntegrator, the legacy PixelBased rasterizers, and the
   photon tracers only gained seeding 2026-08-13.  Fast diagnosis:
   build a camera-inside-a-delta-shell + uniform-env micro-scene
   (closed form: every pixel == env colour exactly — see
   EnvLightBalanceTest's "submerged camera" topology); or flip
   `RISE_DISABLE_IOR_STACK_SEEDING=1` and see whether the healthy
   integrator collapses to match the broken one.  Related trap the
   same investigation hit: `dielectric_material` `scattering 0.0` is
   MAXIMALLY DIFFUSE transmission (Phong exponent 0), not "no
   scattering" — a delta pass-through needs `scattering 1000000`.

1. **Output-layer splat loss — compare the plain file against
   `_denoised`, or re-render with `oidn_denoise FALSE`.**  With
   `oidn_denoise` on (the DEFAULT), the plain (non-`_denoised`) file
   is written by the bound-mode `FileEncoderObserver` reading the
   canonical FrameStore at `MarkPreDenoiseComplete`.  Energy that
   reaches the image only via the splat film (the t=1 light-tracing
   strategy) must be resolved into the canonical around that Mark —
   `VCMRasterizerBase::FlushPreDenoisedToOutputs` and
   `BDPTRasterizerBase`'s pre-denoise block do this (Resolve → flush
   → Unresolve) since 2026-06-10.  Pre-fix, the plain file silently
   dropped ALL t=1 energy: under VCM's balance heuristic t=1 carries
   most direct lighting on simple scenes, so a torus-arealight floor
   measured 0.36× of PT while the image was otherwise CLEAN — no
   splotches, emitter rendered correctly (s=0 emission is weight-1
   and never splatted).  BDPT masked the same hole because power-2
   crushes its t=1 share on PT-convergent scenes.  If plain and
   `_denoised` disagree beyond noise, the bug is in the output
   layer, not transport.

2. **Light-surface sampler density — if the deficit tracks the
   emitter SHAPE, chi-square the geometry's `UniformRandomPoint`.**
   MIS weights assume the claimed `pdfPosition = 1/GetArea()` is the
   ACTUAL sampling density.  If the sampler deviates (TorusGeometry's
   rejection-retry collapsed ~27% of draws onto a single tube circle
   pre-2026-06-10), every light-sampled strategy carries the same
   spatial bias — and integrators MIX those biased estimators with
   different MIS shares (PT ≈65% light-sampled vs VCM ≈90%+ under
   balance), so they disagree pointwise while each remains
   internally consistent.  Signature: discrepancy appears with one
   emitter shape (curved / self-occluding) but vanishes with a flat
   `clippedplane` quad in the same placement; image-wide means
   roughly match because the redistribution cancels globally — which
   makes `*StrategyBalanceTest` mean/median/p99 comparisons
   structurally blind to it.  A density-histogram regression
   (`tests/GeometryUVRoundtripTest.cpp`, TestTorus) shows the
   pattern to copy for other primitives.

3. **A strategy's REALIZED samples are biased, not its WEIGHT
   formula — check for shadow-ray self-intersection before trusting
   a pointwise pdf trace.**  (Found 2026-09-03 on a `weave_material`
   full-sphere-transmissive curtain lit by a mesh area light: PT's
   response to `transmit` looked super-linear, exponent ≈ 1.7, while
   BDPT — sharing the identical material code — stayed exactly
   linear.)  The `(p_light, p_bsdf)` / `(bsdfPdf, p_nee)` pairs
   `PowerHeuristic` combines can be verified numerically IDENTICAL for
   the same direction (same alias-table pdf, same `Pdf()` call) —
   partition-of-unity holds exactly, as it algebraically must — and
   the bug can still be real, because a strategy's REALIZED value can
   be biased for a reason that has nothing to do with its weight.
   Here, `ClippedPlaneGeometry`'s NEE shadow rays were spuriously
   self-shadowed by their OWN originating surface ~94% of the time
   (`RayBilinearPatchIntersection`'s self-hit epsilon, a fixed
   absolute `NEARZERO`, was too tight for the FP noise actually
   produced at the scene's coordinate scale) — a purely geometric
   effect, independent of `transmit`, so the affected strategy
   (NEE) stayed internally linear but deflated by a constant factor.
   The super-linear SHAPE came from MIS correctly shifting weight
   share toward the OTHER (healthy) strategy as the competing pdf
   grew — a real, correctly-computed weight shift wearing a geometry
   bug as a costume.  Fast diagnosis: force each strategy to fire
   ALONE and UNWEIGHTED (the other's contribution suppressed) and
   compare their means directly — two unbiased estimators of the
   same integral must agree; if they don't, the pdf pairing is
   probably fine and one strategy's own sampling is broken.  Full
   mechanism and the geometry fix:
   [precision-fix-the-formulation.md](precision-fix-the-formulation.md)'s
   "Bilinear-patch shadow-ray self-shadowing" example and
   [CLOTH_FABRIC_DESIGN.md §15 debt 21](../CLOTH_FABRIC_DESIGN.md).

4. **The SAME geometry-layer bug that biases PT can also make a
   PERFECTLY CORRECT BDPT/VCM look like the thing that's over-counting
   — check whether PT's own reference value moved before trusting a
   "BDPT/VCM reads Nx over PT" measurement.**  (Found 2026-09-04 on the
   SAME `weave_material` full-sphere-transmissive curtain as cause 3,
   this time lit by a DELTA `omni_light`: a prior session had measured
   "BDPT reads 100-350x over PT" and filed it as a BDPT/VCM
   vertex-connection geometric-term singularity —
   `BDPTUtilities::GeometricTerm`'s `cosA·cosB/dist²`, plausible in
   principle for a flat, zero-thickness, two-sided surface.  The
   measurement does not reproduce once cause 3's fix lands.)  Cause 3's
   own fast-diagnosis technique — "force each strategy to fire ALONE
   and UNWEIGHTED, compare their means" — does NOT catch this variant:
   a delta light has no competing BSDF-sampling strategy for NEE to be
   compared against (`w = 1` unconditionally), so there is no SHAPE
   distortion to notice, only a flat multiplicative deflation of PT's
   mean that looks exactly like "PT is small and correct, BDPT/VCM are
   huge and wrong" if you don't separately ask whether PT's number
   itself is trustworthy.  Here, PT's own NEE shadow ray toward the
   point light suffered the identical `RayBilinearPatchIntersection`
   self-hit bug cause 3 describes — spuriously self-occluded ~94% of
   the time, deflating PT's reference value by ~370x — while BDPT's and
   VCM's OWN connection-visibility shadow rays were shielded from the
   same bug by their own, much larger epsilon bump
   (`BDPT_RAY_EPSILON` / `VCM_RAY_EPSILON = 1e-6`, applied via
   `Ray::Advance()` before casting — six orders of magnitude above the
   ~1e-12 FP-noise floor the underlying bug produces), so BDPT's
   absolute output barely moved across the fix (~0.0253 before and
   after).  The "100-350x" figure was a STABLE BDPT/VCM number compared
   against a PT reference that was itself broken.  **Diagnostic
   takeaway**: before accepting a "BDPT/VCM over-counts by Nx"
   conclusion, check whether BDPT/VCM's OWN connection/shadow-ray code
   has an epsilon bump the PT code path you're comparing against
   lacks — if it does, re-derive PT's reference value with that bug
   fixed before trusting the ratio, exactly like step 0's "PT may be
   the broken one" cause 0 says for the IOR-stack case, generalized to
   any code path where BDPT/VCM happen to carry their own protective
   margin and PT does not.  Full mechanism and numbers:
   [CLOTH_FABRIC_DESIGN.md §15 debt 20](../CLOTH_FABRIC_DESIGN.md).

5. **Subpath vertex connectibility misclassified on mixed delta+continuum
   materials — check whether a stochastic delta lobe marked a surface
   non-connectible.** (Found 2026-09-04 on `weave_material` with
   `transmission thin` and `gap > 0` backlit by a delta light: BDPT/PT
   was exactly 0.900 for linen with `gap = 0.1` and 0.800 with
   `gap = 0.2`.) When a material has both a delta lobe (e.g. weave gap
   pass-through) and a continuum lobe (e.g. diffuse yarn transmission)
   under a single-ray sampling budget, drawing the delta lobe leaves the
   `scattered` ray container containing only delta rays. If the integrator
   determines vertex connectibility via `if (!scattered[i].isDelta) hasNonDelta = true;`,
   drawing the delta lobe spuriously marks the surface vertex
   `isConnectible = false`, dropping Next Event Estimation (NEE, $s=1$) on a
   `gap` fraction of camera rays. On the remaining $1 - \text{gap}$ fraction
   of camera rays where NEE does fire, `brdf.value()` additionally evaluates
   the continuum lobe scaled by $1 - \text{gap}$, yielding an unweighted
   realized share of $(1 - \text{gap})^2$ instead of $(1 - \text{gap})$.
   **Diagnostic takeaway**: `isDelta` is a property of a sampled
   continuation *ray/lobe*, but `isConnectible` is a property of the
   *surface* (whether continuous scattering capability exists, i.e.
   `ri.pMaterial->GetBSDF() != nullptr`). Check whether unweighted $s=1$
   NEE matches PT total; if it is short by exactly a factor of $1 - \text{gap}$,
   the vertex was misclassified as non-connectible when the delta lobe was drawn.
   Full writeup: [CLOTH_FABRIC_DESIGN.md §15 debt 23](../CLOTH_FABRIC_DESIGN.md).

   **Audit the same predicate ONE HOP DOWNSTREAM.** The identical
   per-draw-standing-in-for-per-surface mistake was sitting in the MEDIUM
   vertex branch of the same two functions: a medium vertex inside an object
   was marked non-connectible when `prev.type == SURFACE && prev.isDelta`.
   "Enclosed by a specular boundary" is a property of the boundary
   *material*, so a mixed boundary (weave gap, polished coat, Fresnel
   composite) made the same medium vertex connectible or not depending on the
   draw. It now tests `!prev.isConnectible`. Whenever you fix an
   `isDelta`-should-have-been-`isConnectible` site, grep the file for the
   OTHER vertex types that inherit connectibility from a neighbour.

   **And do not stop at the first integrator that goes green.** Fixing this
   put BDPT on 1.0000 and left VCM on 1.030 — which was a *separate* bug in
   VCM's delta-light MIS partition that the deficit had been masking (step 2's
   VCM bias mode 7). Two bugs of opposite sign on the same scene is not
   unusual; when one integrator lands and its sibling does not, the sibling
   has its own bug, not a residual of yours.

6. **The reference is the LEGACY rasterizer.**  (2026-09-04.)
   `tests/BDPTStrategyBalanceTest.cpp` compares against
   `pixelpel_rasterizer`, not `pathtracing_pel_rasterizer`.  On a
   `weave_material` with `transmission thin, gap 0.1` in front of an area
   emitter the legacy path reads **0.0431** where the progressive PT
   reads **0.1040** at the same 1024 spp (raising `max_recursion` 2 → 8
   changes nothing), because it does not follow the delta gap lobe onto
   the emitter — so BDPT and VCM, which both read 0.1040, look like they
   over-count by 2.41x.  Whenever a `*StrategyBalanceTest` topology
   involves a delta lobe that can REACH a light, render it through both
   path tracers before believing either.  This is cause 0/4's family
   again, one layer out: the broken reference is the harness's choice of
   integrator rather than a bug inside one.

A useful invariant for separating these from real MIS bugs: when you
instrument per-strategy totals (step 3), compare the per-strategy
SUM across integrators, not the mix — balance vs power-2
legitimately split the same energy very differently (VCM may carry
~63% of floor-direct in t=1 splats where BDPT carries ~0%).
Matching sums + a dim image ⇒ the loss happens AFTER the integrator
(output layer).  Consistent weight formulas + emitter-shape-dependent
disagreement ⇒ suspect sampled densities, not weights.

### 1. Establish the property: BDPT/VCM mean = PT mean

The single load-bearing property of an MIS-weighted bidirectional
integrator is that **its expected value equals the path tracer's
expected value** for any scene where both can be unbiased.  PT is the
trusted reference.  BDPT and VCM MIS sums to 1 over all strategies,
so the per-pixel expectation must equal what PT gets via NEE +
emission paths alone.

**Action:** add the scene to or extend
[`BDPTStrategyBalanceTest.cpp`](../../tests/BDPTStrategyBalanceTest.cpp)
or [`VCMStrategyBalanceTest.cpp`](../../tests/VCMStrategyBalanceTest.cpp)
with a topology block.  These tests render the same scene with PT
and the integrator-under-test, capture the radiance buffer in memory
(via `CapturingRasterizerOutput`), and compare four statistics:

| Metric | What it catches |
|---|---|
| **mean** | Systematic bias (the canonical MIS-doesn't-sum-to-1 bug). |
| **median** | Robust mean — if it agrees but mean doesn't, the bias is concentrated in a few outliers. |
| **p99** | "1% of pixels are wildly off" failures the mean hides. |
| **max** | Single-pixel firefly explosions. |

The standard tolerances (`8% mean / 25% p99 / 100% max`) trip on
every concrete bug we've found while ignoring multi-thread non-
determinism.  Don't loosen them; tightening them is fine.

### 2. Confirm bias vs variance — render at 2× and 16× spp

If the disagreement shrinks proportionally to `1/sqrt(N)` it's
variance and a denoiser / more samples / better sampling is the
answer.  If it stays put (or grows) you have **bias** and one of the
strategies is being computed or weighted incorrectly.

**Run this step per SCENE, not per scene FAMILY.**  A 2026-09-04 round
filed a −0.9 % reading as bias because "the noise floor on this scene is
5e-5".  It was — on the DELTA-lit curtain, whose image is
near-deterministic NEE against a point light.  The reading came from the
AREA-lit variant of the same geometry, where the emitter is seen through
a 10 % gap and the floor is ~1 %, two orders of magnitude larger.
Re-measured properly it went 1.00133 ± 0.0104 at 256 spp to
0.99957 ± 0.0023 at 2048 spp — shrinking, i.e. variance.  Changing the
LIGHT changes the noise floor even when nothing else moves.

For BDPT, the canonical bias modes are:

1. A strategy excluded from the MIS denominator that should be
   included (e.g. NEE through a delta light skipped at line 4807-
   class skip rules).  Symptom: `misWeight=1` for a strategy that
   should be `< 1`, contributions add without partition.
2. A strategy contribution computed against the wrong reference
   measure (area vs solid angle), off by a `cos / dist²` factor.
3. `pdfRev` at the connection vertex not being updated correctly
   for the strategy at hand (read but never written, or vice versa).

For VCM, additional modes:

4. Auto-radius `foundSpecular` triggering on delta-position lights
   instead of delta-surface scatters → VM enabled when it should
   not be → photon-density splotches.
5. `directPdfW = 1` placeholder for delta lights treated as a real
   solid-angle pdf in `wLight` / `wCamera` — adds a non-zero MIS-
   denominator term that biases NEE down (~27% on omni + Lambertian
   under the balance heuristic).
6. `mMisVmWeightFactor` term in `wCamera` for delta-light NEE not
   gated by `ls.isDelta`, similar bias even when VM has been
   correctly disabled by the auto-radius pre-pass.
7. `wCamera` for delta-light NEE gated OFF entirely by `ls.isDelta` —
   the OVER-correction of 5 and 6, and the harder one to see, because
   it makes NEE take weight exactly 1 (which looks like the textbook
   answer for a delta light) while the t=1 light-tracing splat quietly
   contributes the rest of the partition on top. See "VCM — delta-light
   NEE took weight 1 while light tracing splatted the same path" below.

### 3. Isolate the culprit strategy

You can't fix what you can't see.  For BDPT, instrument the
contribution accumulation in
[`BDPTPelRasterizer.cpp`](../../src/Library/Rendering/BDPTPelRasterizer.cpp)
right after `weighted = cr.contribution * cr.misWeight`:

```cpp
{
    static FILE* dbg_f = std::fopen( "/tmp/bdpt_dbg.log", "w" );
    const Scalar dbg_max = ColorMath::MaxValue( weighted );
    if( dbg_f && dbg_max > Scalar(0.05) ) {
        std::fprintf( dbg_f,
            "s=%u t=%u splat=%d contrib=(%.4g,%.4g,%.4g) misW=%.5g weighted=(%.4g,%.4g,%.4g)\n",
            cr.s, cr.t, cr.needsSplat ? 1 : 0,
            cr.contribution.r, cr.contribution.g, cr.contribution.b,
            cr.misWeight,
            weighted.r, weighted.g, weighted.b );
        std::fflush( dbg_f );
    }
}
```

A threshold of `0.05` on a 32×32 32-spp scene ignores normal direct
lighting and surfaces strategy outliers.  Tune as needed.  Buckets
to look for in the log:

- **`misW = 1.0` for a non-trivial path** — the strategy thinks it's
  the only one that can sample the path.  Verify against pencil-
  and-paper: enumerate strategies, check connection vertices.  If
  another valid strategy exists, your skip rule is wrong.
- **`misW < 0.01` on the only valid strategy** — the MIS denominator
  is double-counting, or `directPdfW = 1` is being treated as real.
  Check the wLight / wCamera formulas for the strategy.
- **`contrib` huge with `misW` tiny but `weighted` still huge** —
  contribution-formula bug (wrong measure / off by cos/dist² /
  throughput accumulation drift).

For VCM, the path log lives in `EvaluateNEE` /
`SplatLightSubpathToCamera` / `EvaluateMerges` — same instrumentation
pattern, log per-strategy `weight`, `contribution`, `wLight`,
`wCamera`, and the eye/light running quantities at the relevant
vertex.

`fopen` once with `static` storage works because the build is
multi-threaded but file ops are serialised by the kernel; the only
hazard is interleaved writes within a single line, harmless for our
purposes.  **Strip the instrumentation before committing.**

### 4. Decode the path topology from the log

For each high-contribution sample, walk the `eyeVerts` array entries
the integrator stored:

```cpp
for( unsigned int k = 0; k < t; k++ ) {
    fprintf( dbg, " [%u %s d=%d c=%d pdfF=%.4g pdfR=%.4g]",
        k,
        VertexTypeName( eyeVerts[k].type ),
        eyeVerts[k].isDelta ? 1 : 0,
        eyeVerts[k].isConnectible ? 1 : 0,
        eyeVerts[k].pdfFwd, eyeVerts[k].pdfRev );
}
```

Read off the connection edge of the actual strategy (`s, t`) and
every alternative.  An edge whose endpoints are both delta vertices
has zero connection density — that strategy should be skipped from
MIS.  An edge with one delta endpoint that is an NEE-friendly LIGHT
has well-defined density via direct light sampling — that strategy
**must** be in MIS, even if generic delta-skip rules say otherwise.

### 5. Apply the fix at the right layer

Once you know which strategy is wrong, the fix lives in one of three
places (in order of preference):

- **Per-strategy contribution code** if the math is wrong — fix the
  formula, don't paper over it in MIS.
- **MIS skip rule / weight formula** if the strategy is excluded
  from the denominator when it shouldn't be (or vice versa).  This
  is where the BDPT delta-light fix and the VCM delta-light NEE fix
  both live.
- **Running-quantity recurrence** (`InitLight`, `ApplyGeometricUpdate`,
  `ApplyBsdfSamplingUpdate`) if the bug shows up across multiple
  strategies.  Fix it at the source of `dVCM` / `dVC` / `dVM`.

For VCM, the auto-radius pre-pass `foundSpecular` gating in
`VCMRasterizerBase::PreRenderSetup` is a fourth fixpoint: it decides
whether VM is even needed for the scene.  Wrong gating produces
"VM-induced splotches in scenes that don't need VM" — a class of bug
that doesn't show up as MIS-arithmetic-wrong but as VM-shouldn't-fire.

### 6. Add a regression topology to the strategy-balance test

Every fix in this skill is a one-line rule that's easy to miss
again.  Add the scene that revealed the bug as a new topology in the
relevant `*StrategyBalanceTest` so the next time someone touches
`MISWeight` or the recurrence, the fix's invariant is checked
automatically.  Don't rely on the user re-running the failing scene
manually.

## Anti-patterns

- **"Increase samples until the splotches go away"** — masks bias
  with brute force.  Distinguish bias from variance per step 2; if
  it's bias, more samples never converge, they just take longer.
- **"Loosen the test tolerance"** when a `*StrategyBalanceTest`
  fires.  The tolerances were calibrated against multi-thread non-
  determinism on real bug magnitudes; if the test trips, there's a
  real bias.  Loosening it just hides the next regression too.
- **"Special-case this one scene in the integrator"** — don't add
  per-light-type / per-material conditionals to MIS without first
  reasoning about whether the underlying invariant holds.  The
  delta-light fixes look like special-cases but are actually
  applying the MIS framework correctly: a delta vertex with a
  dedicated sampling strategy (NEE for delta lights) belongs in the
  denominator with weight reflecting its dedicated density.
- **"Use the power heuristic since the bug is balance-heuristic-
  specific"** — neither heuristic is wrong; balance is what
  SmallVCM uses and what RISE's VCM is calibrated against.  Mixing
  heuristics across BDPT (power) and VCM (balance) is fine because
  the two integrators don't share an MIS denominator.  Switching
  VCM to power without re-deriving the running-quantity recurrence
  changes the math.

## Concrete examples (from this repo)

### BDPT — delta-light NEE skipped from MIS denominator

`BDPTIntegrator::MISWeight` (line 4804 area) walked the light-side
ratios and applied a PBRT-convention skip rule:

```cpp
if( i > 0 && lightVerts[i-1].isDelta ) {
    continue;
}
```

For omni / spot / directional lights at `lightVerts[0]`, this skipped
the `i == 1` strategy — which is NEE.  NEE handles delta lights via
direct position sampling, so it's a valid strategy and **must** be
in the MIS denominator.  Excluding it left s≥2 light-tracing splats
at `misWeight = 1.0` instead of being downweighted to ~0, producing
22% mean bias and visible per-pixel firefly splats on omni-lit scenes.

The fix: exempt `i == 1 && lightVerts[0].type == BDPTVertex::LIGHT`
from the skip.  See the long comment block at the call site for the
full reasoning.  Catches if regressed by `BDPTStrategyBalanceTest`
"delta-position omni light" topology.

### VCM — delta-light NEE wLight/wCamera not zeroed

`VCMIntegrator::EvaluateNEE` set `directPdfW = 1` as a placeholder
for delta lights (SmallVCM convention) and then plugged it into:

```cpp
const Scalar wLight = bsdfDirPdfW / ( lightPickProb * directPdfW );
const Scalar camFactor = ( emissionPdfW * cosAtEye ) / ( directPdfW * cosAtLight );
wCamera = camFactor * ( mMisVmWeightFactor + dVCM + dVC * bsdfRevPdfW );
```

For delta lights, BSDF sampling at the receiver can't land on the
delta direction by chance — the alternative strategy has zero
density.  Treating `directPdfW = 1` as a real pdf left `wLight =
bsdfDirPdfW = 1/π ≈ 0.32`, biasing NEE down to `1 / (1 + 0.32) ≈
0.76` of its true weight under the balance heuristic.  ~24% darker
direct lighting on omni + Lambertian.  (That arithmetic models only
the `wLight` half; the pre-fix `wCamera` was live too, computed as
`emissionPdfW·cosAtEye / (1·1)` -- the correct camFactor inflated by
dist² -- so on a 3-unit scene the original bug was nearer 40 %; the
24 % was a near-unit-distance measurement.  Reconstructing 0.76 on
another scene will not work without that term.)

The fix: skip both alternatives when `ls.isDelta`:

```cpp
const Scalar wLight = ls.isDelta ? Scalar(0) : bsdfDirPdfW / ( lightPickProb * directPdfW );
Scalar wCamera = 0;
if( !ls.isDelta && directPdfW > 0 && cosAtLight > 0 ) {
    // ... compute wCamera
}
```

NEE is the only valid strategy at a delta light; it gets weight 1.
Catches if regressed by `VCMStrategyBalanceTest` "delta-position
omni light" topology.

**PARTIALLY SUPERSEDED 2026-09-04 — the `wCamera` half of this fix was an
over-correction.**  Zeroing `wLight` stands.  Zeroing `wCamera` does not:
"NEE is the only valid strategy at a delta light" is false, because
light TRACING starts at the light rather than landing on it and has an
ordinary emission-direction density there.  The `wCamera = 0` line left
NEE at weight 1 while the t=1 splat contributed the rest of the partition
on top, a +0.8…+3.0 % over-count depending on geometry.  See "VCM —
delta-light NEE took weight 1 while light tracing splatted the same path"
below for the corrected form.

### VCM — auto-radius `foundSpecular` gating delta lights

`VCMRasterizerBase::PreRenderSetup` walked the light-subpath sample
to decide whether VM was needed for the scene:

```cpp
if( curr.isDelta || prevV.isDelta ) {
    foundSpecular = true;
}
```

This conflated **delta-position lights** (omni / spot / directional —
which are NEE-friendly and don't need VM) with **delta-surface
scatters** (specular reflection / refraction — which DO need VM to
sample caustics).  On a Lambertian-quad-lit-by-omni scene,
`lightVerts[0].isDelta = true` triggered `foundSpecular = true`,
auto-radius computed an etaVCM of ~500, VM activated, and the
photon-density-estimation strategy got significant MIS weight — at
4 spp the rendered image showed visible photon-density splotches
even though VC alone would have produced a clean direct lighting.

The fix: only set `foundSpecular = true` when the delta vertex is a
SURFACE:

```cpp
const bool currIsSpecularSurface  = curr.isDelta  && curr.type  == BDPTVertex::SURFACE;
const bool prevIsSpecularSurface  = prevV.isDelta && prevV.type == BDPTVertex::SURFACE;
if( currIsSpecularSurface || prevIsSpecularSurface ) {
    foundSpecular = true;
}
```

Catches if regressed by `VCMStrategyBalanceTest` "delta-position
omni light" topology — the pre-fix splotches blow out the p99 and
max comparisons.

### Output layer — OIDN-on plain file dropped the t=1 splat film

(2026-06-10, the step-0 example.)  A torus-arealight floor rendered
at 0.366× of PT under VCM while BDPT tracked PT at 0.998 — uniform
dimming, clean image, correct emitter.  Per-strategy totals showed
VCM's NEE + s=0 + t=1 SUM matched BDPT's within 1%, the t=1 splat
film held the missing ~63% with the correct resolve divisor, and the
spatial grid of the splat film matched PT's bright pool — the energy
was complete and correctly placed but never reached the written
file.  Root cause: post-L8, CLI file outputs are canonical-FrameStore
observers; at `MarkPreDenoiseComplete` the canonical was deliberately
splat-free for OIDN, and the old scratch-composite only fed the
legacy IRasterizerOutput chain.  Fix: Resolve → flush → Unresolve in
`VCMRasterizerBase::FlushPreDenoisedToOutputs` and BDPT's pre-denoise
block (observer dispatch is synchronous, so the temporary resolve
cannot race the file write).  No integrator code changed.

### Sampler density — TorusGeometry point-mass biased all light-sampled strategies

(2026-06-10, the step-0 example's second layer.)  With the splat
film correctly composited, VCM still sat at 0.90× of PT with a
shape: +9% directly under the torus, −10% in the surrounding ring —
while a flat quad emitter in the same open scene gave 1.0002.  The
analytic torus's `UniformRandomPoint` rejection-retry multiplied its
[0,1) float candidates by 2⁻³² (an integer-hash constant), so every
rejected first draw collapsed to the single tube angle v = 2π·0.618
— a point-mass holding r/(R+r) ≈ 27% of all samples on one circle.
All light-sampled estimators divide by the claimed uniform 1/area
pdf, so all carried the same circle-shaped bias; PT and VCM disagree
only because they MIX those estimators with different MIS shares.
Fix: exact CDF inversion (bisection-safeguarded Newton on
R·v + r·sin v) in `TorusGeometry::UniformRandomPoint`; regression:
the tube-angle density histogram in
`tests/GeometryUVRoundtripTest.cpp`.  Note this is invisible to
`VCMStrategyBalanceTest`-style image-mean checks (the redistribution
cancels globally) — the sharp regression lives at the sampler layer.

### BDPT/VCM — stochastic delta lobe on mixed delta+continuum material marking vertex non-connectible

`BDPTIntegrator.cpp` (in `GenerateEyeSubpathImpl` and `GenerateLightSubpathImpl`)
determined subpath vertex connectibility by scanning the scattered continuation
rays:
```cpp
bool hasNonDelta = false;
for( unsigned int i = 0; i < scattered.Count(); i++ ) {
    if( !scattered[i].isDelta ) { hasNonDelta = true; break; }
}
vertices.back().isConnectible = hasNonDelta;
```

For pure delta materials (perfect mirror, glass) this correctly marks the vertex
non-connectible. For pure continuum materials (Lambertian, Ward, GGX) this
correctly marks the vertex connectible.

However, for **mixed delta + continuum** materials (such as `WeaveMaterial` with
`transmission thin` and `gap > 0`), `WeaveSPF::ScatterImpl` has a single-ray
budget and stochastically samples either the delta gap lobe (with probability
`gap`) or the continuum yarn lobe (with probability `1 - gap`).

Whenever the delta gap ray was selected, `scattered` contained only `isDelta = true`,
causing `hasNonDelta` to evaluate to `false` and marking the surface vertex
`isConnectible = false`. This completely suppressed Next Event Estimation (NEE,
$s=1$) at that vertex on a `gap` fraction of camera rays. On the remaining
$1 - \text{gap}$ rays where NEE was allowed, `brdf.value()` scaled by
`p.available = 1 - gap`. This produced a realized NEE contribution of
$(1 - \text{gap})^2$ instead of the correct $(1 - \text{gap})$ — an exact deficit
factor of $1 - \text{gap}$ (0.900 at `gap = 0.1`, 0.800 at `gap = 0.2`).

The fix: surface vertices with a valid continuous BSDF (`ri.pMaterial->GetBSDF() != nullptr`)
are always connectible, regardless of which stochastic continuation ray was drawn:
```cpp
if( ri.pMaterial->GetBSDF() ) {
    hasNonDelta = true;
}
vertices.back().isConnectible = hasNonDelta;
```

Catches if regressed by `FabricRenderTest::TestBacklitSheerCurtain` and
`BDPTStrategyBalanceTest` Topology E (`TestBacklitThinCurtain`).

### VCM — delta-light NEE took weight 1 while light tracing splatted the same path

(2026-09-04, found while cleaning up after the entry above.)  With the
connectibility fix in, BDPT landed on PT exactly and VCM sat **+3.0 %**
over it — and the same +3.0 % turned up on a bare Lambertian quad under
an omni light, with no fabric, no gap and no delta surface anywhere.  It
is a delta-**light** bug, not a delta-**surface** one.

`VCMIntegrator::EvaluateNEEImpl` zeroed BOTH MIS alternatives when the
sampled light was delta:

```cpp
const Scalar wLight = ls.isDelta ? Scalar(0)
                                 : bsdfDirPdfW / ( lightPickProb * directPdfW );
Scalar wCamera = 0;
if( !ls.isDelta && directPdfW > 0 && cosAtLight > 0 ) { /* ...camFactor... */ }
```

`wLight` is right to zero: BSDF sampling at the receiver cannot land on
a Dirac *position* by chance.  `wCamera` is not, and the reason is worth
internalising because the two look symmetric and are not:

| alternative | how it reaches the light vertex | density at a delta-POSITION light |
|---|---|---|
| `wLight` — BSDF sampling from the eye vertex | must **land on** the light by chance | **zero** — correctly skipped |
| `wCamera` — light tracing / interior connections / merges | **starts at** the light and samples an emission **direction** | ordinary solid angle (`PointLight` 1/4π, `SpotLight` the cone density) — must be counted |

So NEE took weight 1 while `SplatLightSubpathToCameraImpl` independently
contributed its own `1/(1+wLight)` share of the identical path.  The
over-count is exactly the splat's (plus the merge's) share of the
partition, which is **geometry-dependent** — +2.99 % on a 2.8-unit quad
3 units from the light, +0.82 % on `VCMStrategyBalanceTest`'s topology A,
0 % on any area light — so it can never be bounded by a constant or hidden
under a widened tolerance.

Instrumented mean MIS weight per strategy (the step-3 pattern, one
accumulator per strategy, printed at exit):

| strategy | before | after |
|---|---|---|
| s=1 NEE | 1.00000 | 0.96312 |
| VM (merge) | 0.00815 | 0.00816 |
| t=1 light-tracing splat | 0.02963 | 0.02963 |
| **Σ** | **1.03778** | **1.00092** |

The fix is a **measure** fix, not a gate change (step 5's middle layer):
compute `camFactor` as the emission-direction AREA density,

```cpp
if( emissionDirPdfSA > 0 && distSq > 0 ) {
    const Scalar camFactor = ( emissionDirPdfSA * cosAtEye ) / distSq;
    wCamera = camFactor * ( norm.mMisVmWeightFactor
                          + eyeMis[i].dVCM
                          + eyeMis[i].dVC * bsdfRevPdfW );
}
```

which is the SAME NUMBER as the SmallVCM spelling
`(emissionPdfW_geom · cosAtEye) / (directPdfW · cosAtLight)` whenever
`directPdfW` is the honest conversion `pdfPosition · dist² / cosAtLight`
(both cancel), so non-delta and env lights are algebraically untouched —
but it never divides by `directPdfW` or `cosAtLight`, which are
**placeholders** (1 and 1) at a delta light.  That is why the gate could
not simply be deleted: un-gating the old spelling would have produced a
wrong finite number instead of a wrong zero.

Why it closes exactly: with `dVCM_eye = N / p_cam→A(x)`, the product
`camFactor · dVCM_eye` is the reciprocal of the splat's own
`wLight = (p_cam→A / N)·(vmFactor + dVCM_light)`, and the merge's
`wLight`/`wCamera` complete the same ratio set — NEE + splat + merge sum
to 1 by construction, not by tuning.  Lights with no emission-direction
sampling at all (`DirectionalLight`, `AmbientLight`: `pdfDirection()`
returns 0, and VCM emits no photons from them) fall out at
`camFactor = 0`, i.e. NEE keeps weight 1, which is correct -- and vacuous today: VCM does not sample directional or ambient lights at all (a directional-lit quad renders 0 under VCM where PT and BDPT agree), so the branch states the weight the path will take once that sampling exists.

Catches if regressed by `FabricRenderTest::TestBacklitSheerCurtain`
(`kThinCurtainVcmPtTol` is 1 %, and the bug is 3 %).  Full writeup:
[CLOTH_FABRIC_DESIGN.md §15 debt 24](../CLOTH_FABRIC_DESIGN.md).

### The auto-radius pre-pass was accused of this and was innocent

Worth recording because the accusation is a natural one and cost a round.
`VCMRasterizerBase::PreRenderSetup`'s `foundSpecular` predicate is
`curr.isDelta && curr.type == SURFACE`, a PER-DRAW flag — so a weave's
delta gap lobe does flip a scene to "caustic-bearing" and turn VM on, and
the +3.0 % above did grow to +3.7 % when it did.  But:

- VM accounted for only 0.7 pp of the 3.7 pp;
- VM-off still read +2.96 %;
- gap 0 (VM disabled by the pre-pass) read +2.96 % as well.

The bias was the delta-light NEE partition in all three cases.  With that
fixed, VCM is unbiased with VM **on** (0.99990 at gap 0.1), so the
predicate is a **cost** decision, not a correctness one.  Narrowing it to
"the material is PURELY delta" (`GetBSDF() == 0`) was considered and
rejected: `polished_material`, `subsurfacescattering_material` and
dielectric-over-diffuse `composite_material` all have a non-null BSDF and
all make real caustics, so that predicate would silently disable VM for
them.  The right property is "the material has ANY delta lobe", which is
what the existing loop already estimates — it is a Monte-Carlo test over
width × height × `lightSubpathsPerPixel` subpaths, exact for any lobe
probability a scene realistically uses.  **Generalisable lesson:** a
per-draw flag read inside a loop that runs N times per scene is not the
same defect as a per-draw flag read once per path; check which one you
have before rewriting an interface to fix it.

### The reference can be the legacy rasterizer

Third instance of step 0's "PT may be the broken one" in this file, and
the cheapest to fall into because it is the *test harness* that is wrong.
`BDPTStrategyBalanceTest` uses `pixelpel_rasterizer` as its PT reference.
On a gapped weave in front of an area emitter it reads **0.0431** where
`pathtracing_pel_rasterizer` reads **0.1040** at the same 1024 spp —
`max_recursion 8` does not help — while BDPT (0.104006) and VCM (0.103925)
track the modern PT to 1.4e-3.  A topology added there would have banked a
2.41x reference error as expected behaviour.  Before adding a topology to
a `*StrategyBalanceTest`, render it through BOTH path tracers and check
they agree.

## Mental model for delta lights and MIS

The recurring trap is that `isDelta = true` on a vertex doesn't tell
you whether that vertex is **sampleable** by a non-default strategy.
There are two distinct delta classes:

| Delta class | Example | Sampleable by NEE? | Sampleable by light tracing? | Sampleable by VM? |
|---|---|---|---|---|
| **Delta position** | omni, spot, directional light source | **Yes** (deterministic direct-position sampling) | Yes (light path emits from this vertex) | Yes (photons emitted from here) |
| **Delta surface** | mirror reflection, glass refraction | **No** (NEE shadow ray can't end on a delta direction by chance) | Yes (BSDF sample reproduces the delta) | No (can't density-estimate a delta interaction) |

The MIS denominator includes a strategy iff that strategy can sample
the path with non-zero density.  Generic skip rules ("if any vertex
on the path is delta, skip") conflate the two classes and exclude
strategies that should be counted (delta-position lights have NEE
as a valid strategy).  Specific skip rules ("if the connection
vertex is delta, skip") get this right but need awareness of which
side of the delta is the connection.

## Stop rule

You're done when:

1. The `*StrategyBalanceTest` topology that revealed the bug now
   passes within the standard tolerances.
2. The full test suite (`./run_all_tests.sh`) is green.
3. A regression topology has been added to the relevant
   `*StrategyBalanceTest` so the fix's invariant is checked on
   every future run.
4. Existing torture scenes that exercise the same code path (e.g.
   `scenes/Tests/VCM/diacaustic_vcm.RISEscene` for VCM caustic
   regressions) still render correctly.

The bug is not "fixed" if any of those four don't hold — pretending
to be done invites the next agent to re-derive the same lesson.
