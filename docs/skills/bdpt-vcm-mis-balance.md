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
  strategy / vertex / pdf is producing the wrong value.
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
direct lighting on omni + Lambertian.

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
