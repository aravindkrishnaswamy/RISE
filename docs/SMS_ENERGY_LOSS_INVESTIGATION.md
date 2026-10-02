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
on direction.  One branch keeps a mismatch (review P3-1, DL-345's family):
at an exit through an open sheet that was never pushed onto the stack,
the walk refracts with the stack's index while the vertex still records
`etaI = specInfo.ior`; unmeasurable on `sms_k1_refract`.

After: PT+SMS / PT **0.9840** on the same Sobol' points; found 97.3 %,
Newton failure 0, other root 2.7 % (a share of the SUPPRESSED energy by
per-hit classification; the image deficit is 1.6 %, which implies ~1.55 %
of the suppressed energy -- the two figures are different quantities and
are not reconciled further here).  The residual chains are k = 4 / 6 /
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

## 6. The shared mechanism: proposed fix (as proposed; landed as section 7)

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
| DL-372 perfect-refractor twin | 4.1738 | 3.8325 | 4.1712 (-0.06 %, single render) |
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

## 7. Split suppression (landed, `debt-smssplit`, 2026-10-01)

User decision, 2026-10-01: implement section 6 for the DEFAULT seeding
mode.  It closes DL-372 and DL-336 in the modes where SMS's estimator is
deterministic, and leaves every other mode on the old rule.

**The rule.**  At a BSDF-sampled emitter hit that the old rule suppressed
(an SMS anchor, then a chain of SMS casters, `bSMSChainUncovered` false),
PT now asks `ManifoldSolver::ClassifyEmitterHitCoverage` whether SMS's
own estimator contains this path, and suppresses only if it does:

1. Re-run SMS's base seed for the pair (anchor, PT's hit point):
   `BuildSnellBaseSeed`, the one function `EvaluateAtShadingPoint{,NM}`
   now also call (BuildSeedChain toward the light, the normal-target and
   midpoint fallbacks, the per-wavelength eta override for NM), from the
   anchor's recorded position, geometric normal and IOR stack.  No seed:
   not covered.
2. `Solve` it (biased Solve draws nothing; the phys-fail restart is
   compiled off, `kPhysFailRetries = 0`).  Invalid: not covered.
3. Compare with PT's recorded chain: same length, same object and
   reflect/refract pattern at every vertex (the pattern is read from the
   path's own geometry, `dot(in, n) * dot(out, n) < 0`), every vertex
   within `1e-3` of the path length.  A match is covered.
4. No match: an EXACT delta chain (a perfect refractor, a dielectric at
   `scattering >= 1e6`) is itself a root (its constraint is below the
   solver threshold), so it is a different root: not covered.  A chain
   through a WARPED lobe (a dielectric with finite `scattering` -- the
   parser default is 1e4) is near, not on, the manifold: Newton is run
   from PT's own vertices and the path is covered iff it lands on SMS's
   root.  That is the delta-limit partition, not an exact one (below).
5. SMS's external-segment visibility test fails: not covered.

An emitter SMS's light sampler cannot draw (not `CanBeAreaLight`) is not
covered.  A covered hit is suppressed; a not-covered one is kept at full
weight (the last vertex was delta, so its MIS partner density is 0).

**What PT carries.**  `SMSChainRecord` (ManifoldSolver.h): the anchor's
SMS inputs (recorded in PART 2, committed when PART 3 scatters non-delta)
and every delta vertex since (position, shading and true geometric normal,
uv, object, reflect bit; 16 vertices, more reads "unknown").  A medium
scatter marks the record broken (DL-340's case: old rule).  PART 3's own
guard after a delta lobe at a BSDF vertex (`considerEmission = false`) is
remembered as `smsGuardedEmission`, so those hits reach the same test.
The HWSS body keeps its own record and hands it to both per-wavelength NM
delegations (the no-BSDF caster and the SSS vertex), whose PART 1 applies
the split per lane at the lane's wavelength.

**Exactness, per mode** (`SplitSuppressionExact`, then per query):

| Mode | Split applied | Why |
|---|---|---|
| snell, biased, no photons, no alpha (the default) | yes, RGB and spectral | the seed set is one deterministic chain per (anchor, emitter point) |
| + a pure-mirror caster, RGB | no (old rule) | the RGB evaluator adds `multi_trials` uniform-area mirror seeds; spectral has no such loop, so spectral still splits |
| a k = 1 mirror base seed, RGB (per query) | no (old rule) | replaced by a random surface sample |
| `sms_photon_count > 0` | no (old rule) | photon trials add seeds the classifier cannot replay |
| `sms_seeding uniform` | no (old rule) | sampled seeds; DL-376 |
| `sms_biased FALSE` | no (old rule) | the Bernoulli `1/p` weight estimates every root, so suppress-all is its partition |
| scene alpha coverage | no (old rule) | routes snell to the uniform evaluator |
| `sms_multi_trials > 1`, snell, nothing else | yes | the extra trials are no-ops without photons or a mirror fallback |
| anchor inherited without a record (HWSS medium walks), medium vertex in the chain | no (old rule) | DL-340 |
| HWSS body's own emitter hits after a delta lobe at a BSDF vertex | no (old rule) | per-lane SMS on one hero path; DL-378 |

**Red -> green** (single worker, salted; master `2ef0655d5` library vs
this slice, the same test code):

| Row | pre-split | split |
|---|---|---|
| ball lens caustic crop, perfect refractor (DL-372 twin), RGB, n 8 | 0.38101 +/- 0.00703 | 0.99661 +/- 0.00772 |
| same, HWSS (no-BSDF hand-off carries the record) | 0.38609 +/- 0.01050 (n 8) | 0.99218 +/- 0.00636 (n 4) |
| same lens as the shipped dielectric, `scattering 100000`, n 8 | 0.38278 +/- 0.00850 | 1.03378 +/- 0.01055 |
| row S constant ior-1.4 box (DL-336), n 8 | 0.70040 +/- 0.00634 | 1.01232 +/- 0.01044 |
| row S air control (SMS finds every root), n 8 | 0.98466 +/- 0.01261 | 0.98449 +/- 0.01236 |
| receiver -> smooth-SSS ceiling, anchored (DL-339 (a)), RGB, n 8 | 0.00026 | 1.00268 +/- 0.00668 |
| receiver -> polished ceiling, anchored, RGB, n 8 | 0.00000 | 0.99680 +/- 0.00789 |
| receiver -> smooth-SSS ceiling, anchored, HWSS | 0.00028 (n 8) | 0.99780 +/- 0.00717 (n 4) |
| DL-373 `TwoChainScene( false, true )`, 4096 spp, single render | 0.9840 | 1.0115 |
| shipped `sms_visibility_unoccluded` (CLI, n 3 unsalted / PT 1 render) | 3.8267 (0.917) | 4.1868 (1.003 of PT 4.1741) |

All ratios are PT+SMS / PT without SMS on the same points.  The air
control's mean and spread are unchanged, so where SMS already finds every
root the split costs no noise.  The anchored caster-reflection rows are
DL-339 (a): SMS treats those casters as refractors and never estimates
their reflection, the seed refracts, the classifier says "not covered",
and PT keeps the path -- ~1, not ~2, so nothing is double-counted.  The
double-count audit (`dl295audit`, n 2, PT+SMS / PT / VCM): no sheet
whole 0.9957 / ROI 0.9940 (pre-split, DL-295's record: 0.958 / 0.913-0.918),
sheet over the whole emitter 0.9760 / 0.9700, over the x < 0 half 0.9965 /
0.9840 -- nothing reads above PT.

**The warped-lobe residual (DL-379).**  A dielectric with finite
`scattering` is a delta caster to SMS and a narrow lobe to PT.  At the
ball lens's paraxial focus a 3e-3 rad warp scatters PT's chains across the
roots, and assigning each to the root Newton reaches from it reads +3.4 %
on the caustic crop (1.034 +/- 0.011); the whole shipped image reads
1.003.  The perfect-refractor twin (no warp) reads 0.997.

**Cost.**  Only suppressed emitter hits pay: one base seed (a few rays)
and one Solve, plus a second Solve for a warped chain that missed.  CLI,
10 threads, interleaved n 3, user CPU: `sms_visibility_unoccluded` 38.79 s
-> 40.61 s (+4.7 %; 3.79 -> 3.97 us per sample, 200 x 200 x 256 spp);
`sms_k2_glasssphere` 3.457 s -> 3.590 s (+3.8 %, scene load included).
If it ever matters: skip step 4's second Solve when PT's chain misses
SMS's root by more than the lens's root spacing, or cache the base-seed
solve per (anchor, emitter triangle).  Not implemented.

**Shipped scenes.**  See the table below (`WeaveGapShadowTransmittanceTest`
`scenehash`, single worker, `srand(4242)`, salt 0, 8 spp).

| Scene | scenehash mean, master -> split | PT+SMS / PT (CLI, 64 spp, OIDN off): master -> split | |
|---|---|---|---|
| `sms_visibility_unoccluded` | 3.80106 -> 4.16125 (+9.5 %) | 0.917 -> 1.003 (n 3, shipped spp) | toward PT and VCM |
| `sms_visibility_occluded` | 0.28938 -> 0.30169 (+4.3 %) | 0.9593 -> 1.0030 | toward PT |
| `sms_veach_egg_displaced` | 0.48441 -> 0.74916 (+54.7 %) | 0.4651 -> 0.8749 | toward PT (Newton-plateau misses now counted; the rest is not attributed) |
| `sms_k1_botonly` | 0.17096 -> 0.18239 (+6.7 %) | 0.9299 -> 0.9934 | toward PT |
| `sms_k2_glassblock` | 0.17240 -> 0.18660 (+8.2 %) | 0.9515 -> 1.0257 (VCM: 0.906 -> 0.976) | toward VCM; past PT (open sheets, below) |
| `sms_k2_flatslab` | 0.17886 -> 0.19335 (+8.1 %) | 0.9691 -> 1.0483 (VCM: 0.960 -> 1.038) | overshoots by about what it undershot (open sheets) |
| `sms_k1_refract` | 0.18125 -> 0.19308 (+6.5 %) | 1.0209 -> 1.0855 (VCM: 0.987 -> 1.050) | AWAY from PT and VCM (open sheet, below) |
| `sms_k2_glasssphere_tess` | 0.18786 -> 0.19359 (+3.0 %) | 0.9954 -> 1.0156 | within the 64-spp noise of PT |
| `triplecaustic_pt_sms` | 0.63685 -> 0.64979 (+2.0 %) | 0.9731 -> 0.9941 | toward PT |
| `sms_veach_egg` | 1.02066 -> 1.04123 (+2.0 %) | not measured | |
| `sms_veach_egg_bumpmap` | 1.01750 -> 1.06971 (+5.1 %) | not measured | |
| `sms_luminous_orb` | 0.27392 -> 0.27476 (+0.3 %) | not measured | |
| `spectral_dispersive_caustic_pt_sms` | 1.62696 -> 1.68848 (+3.8 %) | not measured | |

Bit-identical: `diacaustic_pt_sms` (a pure-mirror caster: RGB keeps the old
rule), `sms_k2_glasssphere`, `..._tess_disp`, `sms_k2_torus_cross`,
`sms_slab_close_pt_sms_hispp`, `sms_slab_close_sms`, `sms_teapot_close_sms`,
`sms_through_glass_emitter_pt_sms` (at 8 spp no suppressed hit there
classified differently).  Every move is upward: the split only ever turns
a dropped hit into a kept one.

**Open sheets (DL-339 (b), DL-345).**  `sms_k1_refract`, `sms_k2_flatslab`
and `sms_k2_glassblock` are OPEN displaced `perfectrefractor_material`
sheets.  A suppression-off decomposition (256 spp; `nosupp - PT` is SMS's
estimate E, `E - (split - PT)` the PT energy the split leaves to SMS):
`sms_k1_refract` E 0.02921 against 0.01117 of PT energy for the same
paths (2.6x), `sms_k2_flatslab` 0.02526 / 0.01425 (1.8x).  SMS prices an
open sheet's roots differently from PT -- the flat open ior-1.5 sheet of
`WeaveGapShadowTransmittanceTest` reads 2.25x with SMS on in every build,
split included (2.2515).  Review (2026-10-01, instrumented single renders
of `sms_k1_refract` at 200x150): the overshoot is NOT specific to
displacement (a flat open sheet in that layout moves 1.028 -> 1.095, the
same as the displaced one) and NOT a double count -- every root SMS's own
evaluator accepted re-classifies as covered, PT keeps only pairs SMS has
no estimate for (no seed / failed solve), and a CLOSED flat slab reads
1.0006 with SMS energy 0.997 of what it replaced.  On open sheets the
guarantee is therefore that each (anchor, emitter point) pair goes to
exactly one estimator -- NOT exact path identity: PT's perfect-refractor
paths are not exact roots of SMS's open-sheet model (DL-345's index
mismatch), so ~92-93 % of suppressed hits there go through the
Newton-assignment branch, not only finite-scattering dielectrics.  The
totals carry SMS's open-sheet pricing (~2.5x, DL-339 (b) / DL-345), which
the old rule half-hid behind its losses.  Closed dielectrics (every
other fixture above) read within ~1-2 %.
