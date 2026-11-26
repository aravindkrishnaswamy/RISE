# SMS Seed-Overtrace Fix (2026-05)

`BuildSeedChain` and `BuildSeedChainBranching` were collecting specular
hits **past the emitter point** when the emitter was inside a closed
dielectric shell.  Newton then converged to wrong-topology constraint
roots that `ValidateChainPhysics` correctly rejected.  This was the
dominant `physicsFail` source on the displaced Veach egg and gave the
**displaced-caustic energy collapse** prior investigations attributed
to Newton plateau-stalling on a multi-modal constraint landscape.

The fix is ~20 lines.

## The bug

The seed-builder ray-traces from `start` (shading point) toward `end`
(emitter sample), refracting / reflecting at each specular surface.
Stopping conditions before this fix:

1. `maxChainDepth` (default 10) reached
2. Non-specular hit
3. `range > maxDist * 3.0` (a single-segment safety cutoff that almost
   never fires)

Crucially, **none of these checks the cumulative path against the
straight-line start-to-end distance.**  For an *interior* light (e.g.
the Veach-egg cavity light at the egg center, sitting inside an air
cavity inside a glass shell), the natural caustic chain is `k=2`:

```
wall → outer-egg-back → inner-cavity-back → light_sample
       (refract air→glass)   (refract glass→air)
```

After 2 refractions the trace is inside the air cavity.  But the light
SPHERE has radius 15 inside a much larger cavity, and refractive
bending steers the ray off the original wall→light line by enough that
**it usually misses the light sphere entirely**.  Without a stop
condition, the trace continues:

```
... → inner-cavity-front → outer-egg-front → continues toward ceiling
       (refract air→glass)   (refract glass→air)
```

Resulting in a `k=4` chain whose v2 / v3 are stranded on the *opposite*
side of the light from the wall.

When this chain is handed to Newton with anchors `(wall, light_sample)`,
the only way to satisfy the half-vector constraint at v2 / v3 is to
land on a root where wi and wo are *both* on the same side of the
local face — geometric **reflection**, not refraction.  Newton finds
those roots cleanly (post-Newton ‖C‖ < 1e-4 for 99.4% of rejected
chains), `ValidateChainPhysics` rejects them.

### Anatomy of the rejection (instrumented)

On `cubic_d12` of the displaced-Veach-egg sweep, pre-fix:

| Anatomy slice | Result |
|---|---|
| Phys-fail rate | 22.2% of all Solve calls |
| Failing chains' k | 100% k=4 |
| Sign product at fail | 100% both-negative |
| Seed already physics-invalid | 99.98% (72,269 / 72,284) |
| Post-Newton ‖C‖ at rejected | 14.6% < 1e-6, 85% < 1e-4 (genuinely converged) |
| v0 ↔ v_fail angle | 96.5% on opposite hemispheres of the egg |

Newton was finding *real* constraint roots — they were just
geometrically nonsensical because the seed had put it on the wrong
side of the surface.

## The fix

Track the original `(start → end)` direction and stop adding seed
vertices once a candidate hit's projection along that direction
exceeds `totalDist × 1.05` (5% margin allows for refractive bending of
the path).

Two sites (2026-05 update: branching was excised; both still exist
but `BuildSeedChainBranching` is now a thin wrapper around
`BuildSeedChain`):

- `SnellContinueChain` (single-chain Snell trace)
- `BuildSeedChainBranching` (legacy wrapper retained for call-site
  ABI compatibility)

Both check projection of `ri.geometric.ptIntersection` against
`origStart + t · origDir` with `t > totalDist × 1.05` ⇒ break.

The cutoff catches "trace went past the emitter" cases without
rejecting valid chains where every vertex is between start and end.
Refractive bending across a thin glass shell adds at most ~5% to the
path — the 1.05× margin handles that without false positives.

## Result on the 66-cell displaced-painter sweep

| Aggregate Δ | mean | median | range |
|---|---|---|---|
| Δok% | **+21.6 pp** | +22.2 pp | [+10.1, +35.0] |
| Δphysics% | **−17.3 pp** | −17.6 pp | [−27.1, −6.5] |
| Δratio (ΣLsms/ΣLsup) | **+0.225** | +0.227 | [−0.07, +0.60] |

Per-displacement phys-fail rate fell from 9.6% → 0.0000% at disp=0,
22.9% → 3.1% at disp=40.  Energy ratio went from 0.43-0.94 to
0.79-0.97 across the grid.

### What the fix does NOT change

The residual ~30% Newton-fail rate at high displacement is unchanged
— that's the documented basin-of-attraction limit of plain Newton on
a displaced normal field.  Pre-fix Newton-fail ‖C‖ histogram (76.8%
plateau-stalled at ‖C‖ ∈ [0.1, 1)) is identical to post-fix.  The
seed-fix shifted seed quality, not the constraint landscape.

The residual 1-6% phys-fail rate at high displacement (post-fix) is
now genuinely from Newton finding wrong-topology roots starting from
**valid** seeds — the small natural background rate of plain Newton
on multi-modal constraints.  These are the cases where photon-aided
seeding or all-roots specular polynomials would actually move the
needle (and were prior-investigated against a contaminated baseline).

## Smoothness paradox (clarified, not resolved)

With the seeding bug out of the picture, the prior "smoother painter
is worse" finding stands more cleanly.  At disp=40, ranking by ratio:

| Mode | Smoothness | Ratio |
|---|---|---|
| quadratic | C¹ | 0.950 |
| tent      | C⁰ | 0.930 |
| heaviside | C⁻¹ | 0.857 |
| cubic     | C² | 0.755 |
| quintic   | C⁴ | 0.711 |
| gaussian  | C^∞ | **0.635** |

The smoothest painter (Gaussian) produces the worst SMS quality at
high amplitude, and a mid-smoothness painter (Quadratic) is best.
This is now purely a Newton-convergence story — phys-fails are
≤6%, leaving Newton-fail rate as the dominant factor in the residual
gap.  Investigation of why Newton plateaus more often on Gaussian
domes than on Quadratic ones is the natural next step.

## What this rewrites in the prior story

Prior conclusions that now need to be re-read:

| Claim | Status |
|---|---|
| "Spurious basin is topologically separated; no local fix possible" | **Wrong.**  The seed was driving Newton into the bad basin from the start. |
| "94.5% deep cascade is a constraint multi-modality on closed shells" | **Mis-attributed.**  It's the over-traced seed putting Newton on the wrong side. |
| "Photons don't help (G aggregate Δok ≈ 0)" | **Contaminated.**  Photon-retrieval used the same broken seed-builder; re-test on fixed baseline. |
| "Newton plateau-stalling is the displaced-caustic limit" | **Half-true.**  Plateau-stalling is real but explains only the residual ~10pp gap (post-fix), not the full ~50pp gap reported pre-fix. |

The Pearson r=−0.914 finding (phys-fail rate vs PT/VCM energy ratio)
is correct *as a measurement* — it just measured a property of the
broken-seed state rather than a deep property of the constraint
landscape.  Post-fix the same correlation is still strong (r=−0.829)
but on much smaller phys-fail magnitudes.

## Diagnostic instrumentation

The investigation added an extensive set of phys-fail anatomy
counters (sign products, seed-validity at Newton entry, post-Newton
‖C‖, chain-length distribution, cascade depth, v0↔v_fail surface
angle, perturbed-restart effectiveness) gated under `SMS_SOLVE_DIAG`
in `ManifoldSolver.cpp`.  Default is 0 (off, no perf impact); flip
to 1 to re-engage them when investigating a future SMS regression.

## Files touched

- `src/Library/Utilities/ManifoldSolver.cpp` — fix + diag
  instrumentation (compile-time gated)
- `src/Library/Shaders/PathTracingIntegrator.cpp` — `SMS_DIAG_ENABLED`
  flag (compile-time gated)
- `CLAUDE.md` — high-value-fact correction
- this doc

## Uniform-mode follow-up (2026-05, second pass)

The emitter-projection cap above is **correct for snell mode but wrong
for uniform mode** (`sms_seeding "uniform"`).  Snell mode passes the
LIGHT position as `end` to `BuildSeedChain`, so the cap fires at the
emitter's distance — exactly what we want.  Uniform mode passes the
**uniform-area-sampled point on the caster** as `end`, which sits ON
the specular surface — typically far closer to the shading point than
the actual light.  The cap then clips the seed chain to a single
specular vertex when the natural caustic is `k=2`.

### Diagnosis (cubic_d40, gaussian_d40 displaced Veach egg)

Phys-fail chain-length distribution in uniform mode (pre-fix):

| k | gaussian_d40 |
|---|---|
| 1 | 35 % |
| 2 | 5.5 % ← natural caustic length |
| 3 | 35 % |
| 4 | 25 % |

5.5 % matches expected vs. snell mode's 100 % k=2 chains.  The
emitter-projection cap is firing on `sp` (caster sample) instead of
the actual emitter, clipping most chains short.

### Mitsuba reference comparison

Re-fetched `manifold_ms.cpp` `sample_seed_path()`:

> `m_config.bounces` is a **fixed target**, not a maximum. The code
> explicitly breaks once `bounce >= m_config.bounces` is satisfied.
> If can't reach K hits, return false (reject seed).

Mitsuba's chain length is exactly K (user-specified per scene).  RISE
has `sms_max_chain_depth` as a **maximum** (default 10), not a target.
Uniform mode therefore produced variable-k chains; k=2 happened only
by accident.

### Fix

Two-part:

**Part 1 — code (this commit):** Added an `applyEmitterStop` parameter
to `BuildSeedChain`, `BuildSeedChainBranching`, and
`SnellContinueChain`.  Default `true` preserves snell-mode behaviour.
Uniform-mode call sites and the synthesized-direction-probe fallbacks
pass `false`.  (2026-05 update: `BuildSeedChainBranching` is now a
thin wrapper around `BuildSeedChain` since path-tree branching was
excised; `applyEmitterStop` is forwarded directly.)

**Part 2 — scene authoring:** For `sms_seeding "uniform"`, set
`sms_max_chain_depth` to the natural caustic K for the scene (typically
2 for a glass shell or interior-light scene).  Default 10 is
appropriate for snell mode — the emitter-projection cap discovers the
right depth automatically — but in uniform mode it allows over-traces
that produce wrong-length seed chains.

### Result

| metric | uniform-broken | uniform-fixed | snell mode (default) |
|---|---|---|---|
| **gaussian_d40** ok % | 24.0 | **42.9** | 68.5 |
| **gaussian_d40** phys % | 48.8 | **11.1** | 3.9 |
| **gaussian_d40** ratio | 0.013 | **0.249** | 0.594 |
| **glasssphere** ok % | 58.5 | **92.8** | 57.8 |
| **glasssphere** phys % | 36.5 | **3.4** | 17.6 |
| **glasssphere** ratio | (high noise) | **5.81** | 6.60 |

On smooth canonical scenes (`sms_k2_glasssphere`), uniform mode is
now competitive with — actually slightly better-recovering than —
snell mode (93 % ok vs 58 %).  On heavily-displaced refraction
caustics, snell remains the better choice (43 % vs 68 %), consistent
with the existing `sms_seeding` regime guidance in CLAUDE.md.

### Implication for prior Mitsuba-parity finding

The earlier "RISE outperforms Mitsuba's reference algorithm 12-80×"
([SMS_WRONG_TOPOLOGY_LITERATURE.md](SMS_WRONG_TOPOLOGY_LITERATURE.md)
recommendation 2 follow-up) was based on a **broken** uniform mode.
With this fix the comparison is apples-to-apples: RISE-uniform ≈
Mitsuba-reference on smooth scenes, and RISE-snell beats both on
displaced.  The 5 % residual phys-fail rate that the prior literature
survey called "the consensus floor" is in fact the floor for uniform
mode; snell mode achieves a lower rate on the same scene class
because the deterministic Snell-trace places seeds in
correct-topology basins by construction.
