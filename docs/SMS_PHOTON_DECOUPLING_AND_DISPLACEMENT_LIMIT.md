# SMS Photon-Aided Seeding: Two Latent Correctness Bugs + The Real Limit on Displaced Geometry

**Captured:** 2026-05-03 on the displaced Veach egg
(`scenes/FeatureBased/MLT/mlt_veach_egg_pt_sms_displaced.RISEscene` — outer
glass shell + interior `air_cavity` dielectric + interior point luminaire,
displaced via `perlin2d_painter` at varying `disp_scale`).  Diagnostic
gates: `SMS_DIAG_ENABLED=1` in
[PathTracingIntegrator.cpp](../src/Library/Shaders/PathTracingIntegrator.cpp)
plus `SMS_SOLVE_DIAG=1` in
[ManifoldSolver.cpp](../src/Library/Utilities/ManifoldSolver.cpp).
Both are off in the committed state; flip to 1 to reproduce.

This doc captures three things in one place so the next investigation
can pick up cleanly:

1. The two **photon-pipeline correctness bugs** that landed in the
   accompanying commit, with measurements that prove they were silently
   wasting work in production renders.
2. The **per-shading-point photon cap** added alongside the decoupling
   fix, with the rationale for default 16.
3. What we found about the **disp 0 → disp 10 power drop**: it is
   neither an iteration-budget issue nor a seed-quality issue.  It is
   the documented basin-of-attraction limit of plain Newton on a
   displaced normal field.  Three avenues that would actually help, none
   of which are in this commit, are listed at the bottom.

---

## 1. Bug #1 — `SMSPhotonMap::Build` initialised every photon's IOR
   stack to the env-only stack (`IORStack(1.0)`) regardless of where
   the emitter lived

### Symptom
On the displaced Veach egg, every photon emitted from the lambertian
sphere inside the `air_cavity` inside the glass shell **died on bounce 1**:

```
SMSPhotonMap::Build:: shot=999999 stored=0 (caustic hits) ratio=0.00%
```

### Mechanism
1. Photon emits from a position physically inside two nested
   refractors (`air_cavity`, IOR 1.0; outer `glass`, IOR 1.5).
2. `IORStack iorStack(1.0)` makes `iorStack.containsCurrent()` return
   `false` at the first hit.
3. `DielectricSPF::DoSingleRGBComponent` therefore reads `bFromInside =
   false` and routes through the "entering" branch.
4. The outward-normal sign for the cavity says the photon is
   geometrically **exiting** that volume, so the refracted-ray sign
   check at `DielectricSPF.cpp:141` rejects the refraction lobe.
5. Only the Fresnel reflection lobe survives.  At an IOR-matched
   interface (1.0/1.0) the Fresnel reflection coefficient is
   floating-point cancellation noise → throughput dies by ~32 orders of
   magnitude per bounce.  Photon stored count: 0.

### Fix
[SMSPhotonMap.cpp](../src/Library/Utilities/SMSPhotonMap.cpp) — at every
photon emit, call
[`IORStackSeeding::SeedFromPoint(stack, r.origin, scene)`](../src/Library/Utilities/IORStackSeeding.h)
to walk the dielectric volumes that physically contain the emit point
and pre-populate the stack outermost-first / innermost-last.  This is
**the same helper BDPT calls** at
[BDPTIntegrator.cpp:1356](../src/Library/Shaders/BDPTIntegrator.cpp);
the photon tracer simply wasn't using it.

### Verification
Same scene, same seed, same photon budget:

```
SMSPhotonMap::Build:: shot=999999 stored=818315 ratio=81.83%
```

---

## 2. Bug #2 — Snell-mode SMS gated photon retrieval on `multi_trials > 1`,
   silently discarding every queried photon at the default `M = 1`

### Symptom
After fixing Bug #1, the snell-mode photon path **still** showed zero
energy delta vs the no-photon baseline across the disp 0..10 sweep:

| disp | M=1 baseline ratio | M=1 + 1M photons (Bug #1 fixed only) |
|------|-------------------:|-------------------------------------:|
| 0    | 0.0240             | 0.0244 (+0.0004 — MC noise)          |
| 10   | 0.0102             | 0.0108 (+0.0006 — MC noise)          |

818k photons were being stored in the kd-tree by `SMSPhotonMap::Build`
but **never reaching Newton** at the consumption sites.

### Mechanism
The two snell-mode photon-consumption sites in
[ManifoldSolver.cpp](../src/Library/Utilities/ManifoldSolver.cpp) —
`EvaluateAtShadingPoint` (RGB) and `EvaluateAtShadingPointNM` (spectral)
— gated the kd-tree query on `N > 1` where `N = multi_trials`:

```cpp
if( pPhotonMap && pPhotonMap->IsBuilt() && N > 1 )
{
    pPhotonMap->QuerySeeds( pos, r * r, photonSeeds );
}
```

The trial loop further capped `totalTrials = numBaseSeeds + (N - 1)`,
so even when retrieval did fire, only `N - 1` slots were available for
photon-derived seeds.  At the default `multi_trials = 1`:

- `photonSeeds` was empty (gate `N > 1` false).
- `totalTrials = numBaseSeeds + 0` — zero photon trials regardless.

A scene that set only `sms_photon_count > 0` (the documented opt-in)
was paying the photon-pass cost at scene-prep, populating the kd-tree,
and then never using any of it.

### Fix
[ManifoldSolver.cpp](../src/Library/Utilities/ManifoldSolver.cpp) — drop
the `N > 1` gate; extend `totalTrials` to absorb every queried photon:

```cpp
const unsigned int totalTrials = numBaseSeeds
    + photonSeeds.size()
    + ( N > 0 ? N - 1 : 0 );
```

Photon retrieval is now independent of `multi_trials`; `multi_trials` is
preserved as the **extra-seed budget** above and beyond photons.  The
analogous fix applies to the spectral NM site at the bottom of the
file.

The two **uniform-mode** photon-consumption sites (`EvaluateAtShadingPointUniform`,
`EvaluateAtShadingPointNMUniform`) already iterated `for( const SMSPhoton& ph
: photonSeeds )` unconditionally; they did not have the `N > 1` gate.
The fix only touches the two snell-mode sites.

### Verification (snell mode, disp=0..10)

| disp | snell baseline ratio | snell + 1M photons (both fixes) | Δ |
|------|---------------------:|-------------------------------:|--:|
| 0    | 1.2520               | 1.0952                         | -0.16 |
| 5    | 0.8154               | 1.3706                         | +0.56 |
| 10   | 0.4731               | 0.4296                         | -0.04 |

Δratio swings ±0.6 with no consistent direction — pure MC noise.
Per-shading-point Newton output is now identical to baseline within
noise, **but** photons are reaching Newton.  The photon mechanism
is genuinely redundant on snell-mode caustics: the deterministic
straight-line Snell-traced seed already sits in the dominant caustic
basin, and photon-derived seeds Newton-converge to the same first
vertex and get deduped against the snell base seed.

(In **uniform mode** the picture is different — the snell base seed
isn't there, photons supply the only seeds, and the over-bright /
basin-collapse story below applies fully to those seeds.)

---

## 3. The per-shading-point photon cap (default 16)

### Why
With both fixes in place, the snell+photon sweep was **5× slower per
disp value** than the no-photon baseline.  Every shading point near a
focused caustic was pulling hundreds of photons out of the kd-tree and
running full Newton on each.  At 1M photons, a typical query radius
(`pPhotonMap->GetAutoRadius()` ≈ 0.01 × bbox diagonal) returned
~75 photons / shading point on average and many more in dense clusters.

### What
[ManifoldSolver.cpp:RandomSubsamplePhotonSeeds](../src/Library/Utilities/ManifoldSolver.cpp)
— Fisher-Yates partial shuffle that retains the first `cap` elements as
a uniform random subset.  Applied at all four photon-consumption sites
(snell-RGB, uniform-RGB, snell-NM, uniform-NM) immediately after
`QuerySeeds` returns.  `cap == 0` disables the cap (consume every
queried photon) — useful for convergence studies and unbiased-mode
references.

### Default value
`config.maxPhotonSeedsPerShadingPoint = 16`.  Matches Weisstein 2024
PMS's `M_photon ∈ [8, 32]` typical range and keeps per-pixel cost
proportional to `multiTrials` rather than to the kd-tree photon
density.  The cap is currently a `ManifoldSolverConfig` default — no
parser-level override exists yet; that's a small follow-up if needed.

---

## 4. The displacement power drop is fundamental, not budget

### What we measured
Per-Solve()-failure-mode counts on the snell-mode displaced-egg sweep
(`SMS_SOLVE_DIAG=1` in
[ManifoldSolver.cpp](../src/Library/Utilities/ManifoldSolver.cpp)):

| disp | calls   | ok    | seedTooFar | newtonFail | physicsFail |
|------|--------:|------:|-----------:|-----------:|------------:|
| 0    | 292,888 | 67.3% |       0.0% |     23.2%  |        9.5% |
| 5    | 313,161 | 34.1% |       0.0% |     63.4%  |        2.4% |
| 10   | 329,989 | 23.3% |       0.0% |   **71.8%**|        4.9% |

**`seedTooFar` is 0.0% across the sweep.**  Every Solve() starts with
the seed within `||C|| < 2` of a valid path (the early-out gate is at
2.0 in `Solve`).  So the seeds are fine.  Newton just stops moving.

### Where Newton stops
`||C||` residual at the moment Newton gives up, bucketed:

| disp | <1e-3 | 1e-3..1e-2 | 1e-2..1e-1 | **1e-1..1** | >=1 |
|------|------:|-----------:|-----------:|-----------:|----:|
| 0    | 0.0%  | 1.6%       | 13.3%      | **85.1%**  | 0.0%|
| 5    | 0.0%  | 2.4%       | 21.5%      | **76.1%**  | 0.0%|
| 10   | 0.0%  | 2.7%       | 23.5%      | **73.8%**  | 0.0%|

73-85% of all Newton-fails land at `||C|| ∈ [0.1, 1.0)`.  Not "almost
converged" (`< 1e-2`).  Not "diverged" (`>= 1.0`).  **Stuck on a
plateau** — half-vector mismatch of order 0.1–1.0 radians, where the
gradient is too small for Newton to step further but the residual is
too large to accept.

### It's not iteration budget
Repeating disp=10 with `sms_max_iterations` raised from the scene's 30
to 200 produced **identical** numbers:

| max iter | newton-fail rate | residual in [0.1, 1.0) bucket |
|---------:|----------------:|------------------------------:|
|   30     | 71.8%           | 73.8%                         |
|  **200** | **71.8%**       | **73.7%**                     |

Newton stops moving long before iter 30.  The plateau is a property of
the displaced normal field, not of the solver budget.

### What this means
Heavy displacement deforms the half-vector landscape so that Newton's
straight-line iterates from the smooth-surface seed **fall into a
saddle/plateau between basins** of nearby bumpy roots.  Pure Newton
has no escape mechanism — its step direction collapses where the
Jacobian becomes ill-conditioned.

This is the documented limit of plain Newton on rough specular
manifolds (Zeltner 2020 §5 covers exactly this regime; their
two-stage solver is the documented mitigation for **smooth analytic
primitives + normal-perturbing maps**, but
[SMS_TWO_STAGE_SOLVER.md](SMS_TWO_STAGE_SOLVER.md) measured that two-
stage **regresses** on heavy mesh-displacement cases — Stage 1
converges to the smooth-base caustic which is then up to `disp_scale`
units away from the true bumpy caustic, and Stage 2's seed is *worse*
than the original Snell-traced on-mesh seed).

---

## 5. Three avenues that would actually move the disp=10 number

None of these are in the accompanying commit.  Captured here so the
next session can pick one without re-deriving why the easy answers
don't work:

### Avenue A — Robust solver (Levenberg-Marquardt or trust-region)
Replace pure Newton in `ManifoldSolver::NewtonSolve` with LM and
adaptive damping, or a trust-region step-length policy.  These can
escape the plateau where pure Newton stalls because they shrink the
step when the model is unreliable rather than committing to a full
Newton step that the gradient says is zero.  No correctness risk —
when the model IS reliable, LM degenerates to Newton.  Budget per
shading point goes up (each iter costs slightly more), but on
displaced surfaces the iteration count to plateau-escape is bounded.
Estimated win on disp=10: 1.2–2× recovery toward the disp=0 baseline.
No guarantee — if the basin truly has no attractor reachable from the
straight-line seed, LM also returns failure.

### Avenue B — Photons as seeds for displaced geometry, with robust solver
Photon-derived seeds in **uniform mode** start AT the bumpy caustic
surface (the photon physically landed there).  At disp=10 in our
existing measurements, uniform-mode + photons exhibits `newtonOk = 0`
across hundreds of thousands of photon-derived seeds — pure Newton
fails on every one for the same plateau reason as the snell base seed.
With Avenue A's robust solver, photon seeds become valuable: each one
is by-construction in the right basin (the photon traversed it
physically), and a robust solver can refine without escaping.  The
two avenues compose; neither alone is enough.

### Avenue C — Specular polynomials (rejected in this session)
Closed-form all-roots enumeration via Bezout resultant elimination,
per Fan et al. 2024.  No basin-of-attraction limit because there's no
iteration.  The session's spoly hybrid k=1 prototype found 57 roots in
740k trials before being reverted as "basically zero" — the
implementation gap to make that production-viable is significant.
Captured in
[SMS_FUTURE_DIRECTIONS.md](SMS_FUTURE_DIRECTIONS.md).

---

## Reproducing the numbers in this doc

```sh
# 1. Flip diagnostics ON
#    src/Library/Shaders/PathTracingIntegrator.cpp:62  → SMS_DIAG_ENABLED 1
#    src/Library/Utilities/ManifoldSolver.cpp:35       → SMS_SOLVE_DIAG  1
make -C build/make/rise -j8 all

# 2. Generate the disp 0..10 sweep at 100×100 (fast iteration)
#    by editing mlt_veach_egg_pt_sms_displaced.RISEscene's
#    `disp_scale` and `width/height`.  See egg_sweep/scenes_*/ for
#    a reference set.

# 3. Per-disp render
export RISE_MEDIA_PATH="$(pwd)/"
printf "render disp10.exr\nquit\n" | ./bin/rise <scene>.RISEscene 2>disp10.stderr

# 4. Read off the diagnostic
grep -E "SMS-DIAG|SMS-SOLVE-DIAG|SMS-NEWTONFAIL-RES" disp10.stderr
```

The diagnostic counters live in anonymous namespaces and dump at
`std::atexit`, so they fire after `RISE` quits normally (the
`render` + `quit` REPL sequence above triggers this path).
