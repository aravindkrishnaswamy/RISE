# SMS energy-loss investigation (DL-372, DL-373, DL-336)

2026-10-01, branch `debt-smspush` from `master` `43e9f3bb8`.  Three open
rows in which PT with `sms_enabled TRUE` reads below PT without it.  This
document attributes all three, separates one shared mechanism from one
independent defect, records the fix that landed (DL-373) and the measured
design for the one that did not (DL-372 / DL-336).

## 1. Result

| Row | Fixture | PT+SMS / PT before | Mechanism | Share of the suppressed energy SMS never estimates | After this slice |
|---|---|---|---|---|---|
| DL-372 | shipped `sms_visibility_unoccluded` | 0.917 | **multi-root coverage**: the deterministic snell seed reaches one root per light sample; the ball lens (receiver at its paraxial focus) has up to three | 64.0 % = other root 50.3, Newton failure 7.9, no seed 5.7 (perfect-refractor twin) | unchanged (not fixed) |
| DL-336 | `GradedIndexInteriorFactorTest` row Q, constant ior-1.4 box | 0.700 | **the same multi-root coverage**: at relative index 1.5/1.4 the sphere is a weak lens whose two images (either side of the axis) both reach the floor | 31.4 % = other root 31.4, Newton failure 0 | unchanged (not fixed) |
| DL-373 | `TwoChainScene( false, true )` | 0.9425 | **seed-walk bug**: `SnellContinueChain` refracted every EXIT as index-matched | 6.1 % = Newton failure 4.0 (k=2 chain seeded k=4), other root 2.1 | **0.9840** (same Sobol' points); residual 2.7 % = DL-372's mechanism (TIR light-guide chains through the slabs' side faces) |

**Pricing is exact.**  On every fixture the SMS estimate equals the
found share of the suppressed energy, and on the DL-372 sphere every root
SMS finds carries the true measure-conversion factor: SMS's
`G_x_v1 * |det dv1/dy|` divided by an independent finite-difference
`|d omega / dA_y|` (own analytic sphere tracer, no RISE transport code)
reads p1 0.9998 / p50 1.0000 / p99 1.0003 over 924 roots, unchanged across
true factors from < 0.1 to 10.  The `maxGeometricTerm` clamp (10) costs
0.1-0.2 % of the suppressed energy.  Nothing in the three rows is the
eta^2 factor, the receiver stack, the emitter's Le or cosine, or the pdf.

**Shared vs separate.**  DL-372 and DL-336 are one mechanism: PT leaves
every anchored caster chain to SMS, SMS's snell mode evaluates ONE
deterministic seed per light sample (`BuildSeedChain` from the anchor
straight toward the sampled emitter point, then Newton), and a caustic
with more than one root per emitter point loses every root that seed does
not converge to.  DL-336's medium is not a medium effect: it changes the
relative index, which moves the configuration from one root (air: found
99.9 %) into the two-image regime.  DL-373 was a separate, contained
defect in the seed walk; its residual is the shared mechanism.

## 2. Method

Two toggles in a measurement build (not committed; the patch is described
here so it can be re-applied):

1. **Suppression off**: PART 1's `smsSuppressEmission` forced false.
   PT+SMS with suppression off double-counts, so with the same Sobol'
   points `suppressed = noSupp - PT+SMS` and `SMS estimate = noSupp - PT`.
2. **Per-hit coverage classification**: at every emitter hit PT
   suppresses, re-run SMS's own snell base seed (`BuildSeedChain` with the
   production normal-target and midpoint fallbacks, then `Solve`) from the
   anchor -- position, both normals and IOR stack recorded where PART 2
   evaluated SMS -- toward the PT hit point, and compare the converged
   chain with PT's (length, first vertex within 1e-3).  Classes: found /
   no seed / Newton failure / other root (converged to a different chain)
   / not visible.  Weighted by `MaxValue( throughput * Le )` of the
   suppressed emission.  Biased snell mode is deterministic in (anchor,
   emitter point) (no photons, no pure-mirror caster, no alpha), so this
   is exactly the question "does SMS's estimator contain this root".

A third, independent check sat at a fixed receiver point on the DL-372
geometry (scratch probe, not committed): cosine-sampled directions traced
through the sphere analytically give the ground-truth irradiance and, for
each direction that reaches the emitter, the root SMS should find; dense
seeding (24 x 24 directions over the sphere's solid angle) enumerates
every root SMS's `Solve` can reach for a given emitter point, each priced
by `ComputeTrialContribution`.

Renders: CLI or the suites' own `Render`, `oidn_denoise FALSE`, box
filter, single renders unless stated (each comparison below is paired on
identical Sobol' points, so the toggles' differences are not seed noise).

## 3. DL-372

`scenes/Tests/SMS/sms_visibility_unoccluded.RISEscene` (shipped 200 x 200,
256 spp): PT 4.1743, PT+SMS 3.8292 (0.917), suppression off 4.3643.
Suppressed 0.535, SMS estimate 0.190: **SMS reaches 36 % of what PT hands
it.**  The whole loss is the 20 x 20-pixel cells over the caustic (21.35
suppressed / 7.78 estimated in the brightest cell, 0.53 / 0.32 beside it).

Index sweep (dielectric, same scene): 1.05 -> 0.9918, 1.2 -> 0.9511,
1.5 -> 0.917; SMS reaches 94 / 64 / 36 % of the suppressed energy.

Classification on the perfect-refractor twin (the ledger's control,
`perfectrefractor_material`, same loss): found 36.0 %, other root 50.3 %,
Newton failure 7.9 %, no seed 5.7 %, not visible 0.  The fixed-point probe
agrees at five receiver offsets (found 36 / 37 / 36 / 35 / 61 %, clamp
loss <= 0.2 %).  Why: the floor point under the sphere sits 1.5 from the
centre, the ball lens's paraxial focal distance `nR / (2(n - 1)) = 1.5`, so
paraxial rays leave collimated and marginal rays, over-corrected, cross
the axis; near the axis an emitter point is reached by up to three roots
(3.1 on average in the central 0.36-radius bin of the emitter), and the
straight-line seed converges to one.  The dielectric version cannot be
classified this way: its `scattering 100000` warps PT's transmitted
direction, so PT's chain is not exactly a root and the 1e-3 match fails
(found reads 0.3 %); the ledger's perfect-refractor control loses the
same 8.3 %.

## 4. DL-336

Row Q, constant ior-1.4 box (camera inside), 1024 spp: PT 0.1206, PT+SMS
0.0845 (0.700), suppression off 0.2037.  Suppressed 0.1192, SMS estimate
0.0831 (69.7 %).  Classification: found 68.6 %, other root 31.4 %, Newton
failure 0, no seed 0; found x suppressed = 0.0818 against the measured
0.0831, so the found roots are priced correctly in the medium.  Air
control (box removed): found 99.9 %.  The misses are converged, valid
chains of the same length whose first vertex is 1-12 cm from PT's: the
second image.  At relative index 1.071 the r = 0.15 sphere's focal
distance is ~1.1, the floor (~0.5 away) is inside it, and a weak lens
images an off-axis point twice.

## 5. DL-373 (fixed)

`TwoChainScene( false, true )`, 16 x 16, 4096 spp: PT+SMS / PT 0.9425,
suppression off 1.974; SMS estimates 94.4 % of the suppressed energy.
Classification: found 93.9 %, Newton failure 4.0 %, other root 2.1 %.
Every Newton failure was a k = 2 chain (diffuser -> upper slab ->
emitter) whose seed had length 4: the walk entered the upper slab
correctly, left it along its inside direction (2.3x steeper), passed the
small emitter and entered the lower slab inside the emitter-projection
cap.

The defect: `SnellContinueChain` refracted an exit with
`specInfo.ior / currentIOR`, and while the walk is inside an object both
are that object's index -- ratio 1.  Present since the first commit
(`9da9d6a43`); DL-290 fixed which medium is on the far side but not this
ratio.  The fix resolves the far-side stack (the same pop / containment
logic, on a copy) BEFORE refracting, refracts with
`currentIOR / destIOR`, commits the stack only when the refraction
succeeds, and gives a TIR exit the far index as `etaT` (previously
unreachable: a ratio-1 exit never reflected).  The containment probe now
runs along the incident direction: it asks whether the crossing point is
inside the stack-top object, which for a closed object does not depend
on direction.

After: PT+SMS / PT **0.9840** on the same Sobol' points; found 97.3 %,
Newton failure 0, other root 2.7 %.  The residual chains are k = 4 / 6 /
8: paths that enter a slab through its side face and are guided by total
internal reflection -- real roots no straight seed reaches, i.e. DL-372's
mechanism.  The fix does not move DL-372 (0.917) or DL-336 (0.701).

Red-proof: `ManifoldSolverTest` Group 16 (the measured miss's geometry:
two closed ior-1.5 slabs, a seed from y 2.2 toward an emitter point at
y 1.5) -- k = 2 and an exit continuation parallel to the incident
direction; the pre-fix walk returns k = 4.

Shipped SMS scenes (`WeaveGapShadowTransmittanceTest` `scenehash`: single
worker, `srand(4242)`, salt 0, 8 spp, pre-fix `43e9f3bb8` vs fix): 13 of
21 hashes move, every one a scene whose seed walk leaves a refractive
caster -- triplecaustic 0.636926 -> 0.636847, k1_botonly 0.170962 ->
0.170955, k1_refract 0.180705 -> 0.181250 (+0.30 %), k2_flatslab 0.178864
-> 0.178856, k2_glassblock 0.172475 -> 0.172404, k2_glasssphere 0.192965 ->
0.192965, k2_glasssphere_tess 0.187861 -> 0.187861, ..._tess_disp
0.186367 -> 0.186376, k2_torus_cross 0.192448 -> 0.192334,
slab_close_pt_sms_hispp 0.322774 -> 0.322747, slab_close_sms 0.292578 ->
0.292570, teapot_close_sms 0.332172 -> 0.332229, veach_egg_displaced
0.4844081 -> 0.4844089.  Unchanged bit for bit: diacaustic (mirror
chains), luminous_orb, veach_egg, veach_egg_bumpmap, visibility_occluded,
visibility_unoccluded and both spectral scenes.  At 8 spp these moves are
seed-path changes inside the noise; none is a resolved energy change.
`ExteriorIndexInvarianceTest`'s not-gated A7-KF seed-walk lines (T6, T5c)
print the same before and after, so moving the containment probe to the
incident direction did not change those configurations.

## 6. The shared mechanism: proposed fix, not landed

**Coverage-aware suppression.**  With deterministic seeding, SMS's
estimator at an anchor x is `sum over roots k in F(x, y) of w_k(y)`, where
`F(x, y)` is the set of roots the seed reaches for emitter point y.  PT
can keep exactly the complement: at a suppressed emitter hit, run the same
seed + Newton from the recorded anchor toward the hit point and suppress
only if the result is PT's own chain.  The two estimators then partition
every root, with no estimate of a seeding probability.  Prototype (the
classification above with "keep the emission unless found"):

| Fixture | PT | PT+SMS now | PT+SMS, coverage-aware |
|---|---|---|---|
| DL-372 perfect-refractor twin | 4.1738 | 3.8325 | 4.1712 (-0.06 %) |
| DL-336 row Q constant | 0.1204 | 0.0846 | 0.1224 (+1.6 %, single render) |
| DL-373 (with the seed fix) | 0.013123 | 0.012914 | 0.013273 (+1.1 %, single render) |

Why it did not land in this slice: (1) it is exact only where the seed set
is deterministic -- snell mode, biased, no photon map, no pure-mirror
caster (random supplemental seeds), no alpha coverage, no k = 1
reflection surface-sample fallback; uniform mode and the random modes need
an independent replica of the whole trial set, and `sms_biased FALSE`
weights by an estimated `1/p` and needs its own rule; (2) PT must carry
the anchor (position, normals, IOR stack) and the chain's first vertex
and length through the Pel / NM loop, and the HWSS loop suppresses through
`considerEmission = false`, so it needs its own restructuring; anchors
inherited across a recursive `CastRay` carry no record; (3) it moves every
shipped SMS scene with a multi-root caustic, Newton failures or seed
misses (most of them), toward PT and adds PT's caustic noise back for the
roots SMS misses -- a change in what `sms_enabled` means for a render, not
a contained bug fix.  The clamp complement (keep `1 - min(1, maxG / G)` of
a found root) would remove the 0.1-0.2 % clamp loss in the same pass.
The alternative is better seeding (multiple stochastic seeds with
`1/p` estimation, specular polynomials), which is the solver redesign the
literature takes; it would close the multi-root share without touching PT
but not the Newton-failure share.
