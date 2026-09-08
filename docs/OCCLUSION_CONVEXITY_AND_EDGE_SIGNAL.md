# Occlusion, Convexity, and the Planar Reference

**Status:** design + implementation record, 2026-09-06; **cost revision
2026-09-07** (§6) — lockstep marching plus a per-hit rotation of both sample
sets took `plank_closeup` from 120.0 s to 54.7 s, and the **per-hit memo of
§6.5, also 2026-09-07**, took it from there to **16.33 s**, with the closed
forms held and the image inside the renderer's own noise throughout. The memo's
**3.32×** is measured against §6.5's own interleaved base of **54.16 s**
(54.16 → 16.33), not against the 54.7 s the previous commit reported on a
separate run; the two bases are the same configuration measured twice.
§3.1, §3.2, §6, §8 and §10 carry the 2026-09-07 numbers; everything else is the
2026-09-06 record.
**Supersedes:** [GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md)
§6.2's description of the SDF occlusion estimator, and its Phase-2 claim that
"a plane or convex body has `map(p + h·n̂) = h` at every tap and reads **1**".
That claim is true of a single exact primitive and **false of every convex edge
a CSG `intersect` / `subtract` produces** — which is the bug this document
fixes. Everything else in that document (the dispatch channel, the radius
convention, the lazy-bake architecture, `thickness`, `curv`) stands unchanged.

---

## 0. Two symptoms, one missing quantity

1. `occlusion(radius)` returns a **residual on a merely convex edge** — a value
   below the neutral 1 where there is no cavity at all. `plank_closeup` hides
   this behind a hand-tuned `smoothstep(0.62, 0.18, occ)` so that its dirt term
   only fires on the real end check.
2. `curv` is a *radius-free* differential quantity normalised by the object's
   bounding-box diagonal, so "edge" is a different number on every object — a
   3 mm shank on an 82 mm nail reads ≈ 14, a flat face 0 — and an author has to
   probe-render before they can pick a threshold. There is no radius-sampled
   edge signal at all.

Both want the same missing thing: **a radius-sampled measure of how open the
surface is, taken against the planar reference.** Look at a ball of radius `r`
around the hit. A locally planar surface is exactly half open. A concave fold is
less than half (cavity → occlusion). A convex edge is more than half (edge).

What follows is that idea, plus the one place it does *not* survive contact with
a real scene — which is why the two signals ship as two different estimators
rather than as two clamps of one number, and §3.3 is the honest record of that.

---

## 1. The residual, precisely

### 1.1 What the shipped estimator measured

`SDFGeometry::ComputeOcclusion` was the Evans / IQ normal-line estimator: five
taps at `h_i = R·2^(i−5)`, weights `w_i = 2^(1−i)`, and

```
occ = Σ wᵢ·(hᵢ − dᵢ) / Σ wᵢ·hᵢ ,   dᵢ = Map(p + hᵢ·n̂) − Map(p)
ao  = 1 − occ
```

It measures a **shortfall in the field's magnitude** along the outward normal:
"the field says the surface is nearer than `h`, so something must be in the
way." Its correctness rests entirely on the identity `Map(p + h·n̂) = h`, i.e.
on `Map` being the **exact Euclidean distance**.

### 1.2 Why the identity fails at a convex edge

RISE's composed field is **not** an exact distance.
`SDFGeometry::EvaluateParts` composes parts with `min` (union), `sminP`, and
`smaxP` (intersect / subtract). `smaxP(a, b, 0)` is the hard `max`, and **the
max of two half-space distances is not the distance to their intersection
anywhere outside the solid.**

Take a convex edge where two planar faces meet, with inward face normals `n̂₁`,
`n̂₂`, and let `2γ` be the angle between them. The solid is
`{d₁ < 0} ∩ {d₂ < 0}` and the field is `max(d₁, d₂)`. The surface normal at the
edge (the bisector — which is also the fillet normal at the middle of a rounded
arris) is `n̂ = (n̂₁ + n̂₂)/|n̂₁ + n̂₂|`, so at the tap `p + h·n̂`:

```
dᵢ = h·(n̂·n̂ᵢ) = h·cos γ    for both i
Map = max(d₁, d₂) = h·cos γ    but the true distance is h
```

The shortfall is the *same fraction* `1 − cos γ` at every tap, so it survives
the weighted normalisation exactly:

> **`ao = cos γ` at a convex CSG edge, for every query radius, and independently
> of the fillet radius.**

For a **90° arris** (`n̂₁ ⊥ n̂₂`, `γ = 45°`): **`ao = cos 45° = 0.7071`**.
Measured, on `max(y, z)` at the origin with `R = 0.5`: **0.7071**.

Rounding the arris does not help. The residual is a property of the
*combinator*, not of the sharpness: on a rounded box modelled as a single
`roundbox` part the field is `length(max(q,0)) − r`, which *is* exact outside,
so that arris reads 1. Build the same visual edge as an `intersect` of two parts
and it reads 0.707. Two ways of modelling the identical surface disagree by 30 %
of the signal's full range.

### 1.3 The estimator could not tell a convex edge from a concave one

The same measurement on the **concave** 90° valley `min(y, z)` — where `Map`
*is* exact and the shortfall is a genuine cavity — also gives **0.7071**
(measured). The two were indistinguishable:

| feature at 90°                    | old `occlusion` | truth |
|-----------------------------------|----------------:|------:|
| flat plane                        | 1.000           | 1.000 |
| convex arris (`roundbox`, exact)  | 1.000           | 1.000 |
| convex arris (`intersect`, `max`) | **0.707**       | 1.000 |
| concave valley                    | **0.707**       | 0.500 |

The root cause in one sentence: **the estimator read `|Map|`, which is only a
conservative lower bound on distance, and a lower bound is one-sided — it can
only ever darken. A convex edge sat at the very top of the estimator's range,
with no headroom above 1 to absorb that, so every bit of field inexactness at a
convex feature became visible darkening with nothing to cancel it.**

### 1.4 Where it actually bit in the shipped corpus

Measured on the shipped `plank_closeup` geometry, through the real
`SDFGeometry::IntersectRay`:

* **The plank's own arrises were clean.** The board is a single `roundbox` part,
  whose field is exact outside, so over the scene camera's full 640×480 frame
  (146 112 board hits) every arris and corner read **1**; the worst value
  anywhere on the board away from the end check was **0.999997**. The scene
  header's attribution of its dark-arris line to the estimator's convex residual
  is therefore **not reproducible on the geometry that shipped** — the mechanism
  is real but this particular board does not exhibit it.
* **The nail did exhibit it.** `geo_nail` squares its shank with a
  `box intersect`, which is exactly the `max` of §1.2. Over a 200×60 top-down
  sweep (2 895 hits), `occlusion(0.10)` read **below 0.85 on 267 of them**,
  minimum **0.639**, on surface that is convex everywhere it is low. The nail's
  material consumes `crev = clamp(1 − occ, 0, 1)` *raw*, so up to 0.36 of
  `rust_crev` was being deposited on convex forged ridges by the artefact.

---

## 2. The two signals

```
occlusion(r)  in [0,1]   1 = open,           0 = fully enclosed    neutral 1
convexity(r)  in [0,1]   0 = flat or concave, 1 = a knife edge     neutral 0
```

They partition the same idea about the same query ball. Occlusion reports how
much *less* open than a plane the surface is; convexity how much *more*. Neither
is ever a residual of the other: a flat face and every convex edge read
`occlusion` **exactly 1**, and a flat face and every cavity read `convexity`
**exactly 0**.

Canonical values (`α` = the empty dihedral opening; `α = π` is a plane):

| feature                     | `occlusion` | `convexity` |
|-----------------------------|------------:|------------:|
| plane (`α = π`)             | **1** exact | **0** exact |
| convex 90° arris            | **1** exact | **0.500**   |
| convex three-face corner    | **1** exact | **0.750**   |
| knife edge                  | 1           | → 1         |
| concave 90° valley (`α=π/2`)| **0.500**   | 0           |
| slot / pocket               | → 0         | 0           |
| convex sphere, radius ρ     | 1           | `3R/(8ρ)`   |

Both exactness claims are *structural*, not tolerances — see §3.1 and §3.2 — and
both are asserted at 1e-12 in the tests rather than inside a band.

### 2.1 Relationship to the estimator this replaces

On the concave-wedge family — the family every shipped scene tunes against —
both have closed forms. Writing the *empty* opening angle as `α`:

```
occ_new = sin²(α/2)                 (cosine-weighted visibility, §3.1)
occ_old = cos( (π − α)/2 ) = sin(α/2)   (bisector-normal shortfall)
```

so **`occ_new = occ_old²`**. Each form is that estimator's reading at the
configuration where *it* has a closed form (the new one on a face with that
face's normal as the axis, the old one along the bisector), so the identity
compares the two estimators' characteristic values on a wedge of opening `α`;
it is not a pointwise conversion to apply hit by hit. At the 90° anchor both
sides are measured: 0.707 before, 0.500 now.

Squaring is a strictly increasing bijection of [0,1] onto itself, so:

* the ordering of cavities is **exactly preserved** — anything that was darker
  than something else still is;
* both endpoints are **fixed** — flat still reads 1, a sealed pocket still 0;
* every intermediate cavity reads **darker**, never lighter. A cavity that read
  0.707 (a right-angle valley) now reads 0.5.

So the correction does not rescale cavities in the sense that matters: no cavity
can become lighter, none can change places with another. It re-bases the scale
so the planar reference is a ceiling that convex features can *reach* rather
than one they get pushed below.

---

## 3. The SDF estimators

Two of them, deliberately. `SDFGeometry::PrepareSignalQuery` is the shared
preamble (validate, resolve `R = radiusFraction · m_diagonal`, take the
sphere-trace band residual `d₀ = Map(hit)`); the estimators then diverge.

### 3.1 `occlusion` — directional visibility, marched

```
occlusion = (of kOcclusionDirs = 12 cosine-weighted outward directions,
             the set spun about the normal by one of kSignalRotations = 32
             pre-built azimuths chosen by a hash of the hit position,
             the fraction whose sphere trace reaches R without entering the solid)
```

* **The planar reference is free and exact.** Over a plane *every* outward
  direction escapes, so the answer is 1 with no normalisation — at any
  orientation, any direction count, any field. The same holds for every convex
  feature, which is the whole bug fixed. Nothing here reads the field's
  magnitude as a distance; the march steps by it (which is what sphere tracing
  is for, and where a conservative under-estimate is safe by construction) and
  decides *blocked* on its **sign**, which is exact for the surface RISE
  actually renders — the zero set of the composed field *is* the blended
  surface.
* **It is the mesh family's question, verbatim.** `MeshSignalBake`'s occlusion
  bake casts 64 cosine-weighted hemisphere rays and stores the escape fraction.
  This is the same integral, evaluated by marching a field instead of by
  querying a BVH. That is what makes "the same builtin on meshes and SDFs" a
  portability claim rather than a naming coincidence.
* **Closed form on a wedge:** for a point on one face of a wedge of empty
  opening `α`, with that face's own normal as the axis,
  `(1/π)∬(cos γ sin ψ) cos γ dψ dγ = (1−cos α)/2 = sin²(α/2)`. Measured
  (α = 180/165/150/135/120/105/90): 1.000 / 0.975 / 0.950 / 0.850 / 0.775 /
  0.650 / 0.525 against 1.000 / 0.983 / 0.933 / 0.854 / 0.750 / 0.629 / 0.500 —
  max deviation 0.025.
* **Closed form in a spherical pocket:** a ray at angle φ from the inward normal
  crosses a chord of `2ρ cos φ`, so it is blocked exactly when
  `cos φ ≤ R/(2ρ)`, whose cosine-weighted measure is `(R/2ρ)²`. Hence
  `occlusion = 1 − (R/2ρ)²`.

Implementation details that are load-bearing rather than incidental:

* **The march origin is lifted `R/64` along the hit normal.** A ray begun
  exactly on the zero set in a direction near the tangent plane sits at height
  ≈ 0 and reads as *inside*; before the lift existed a flat surface reported 1
  or 2 of its directions blocked and occlusion came out at 0.95 instead of 1
  (measured). The mesh bake applies the same lift for the same reason
  (`kOriginEpsilonFraction`), and both err the same way: toward *escaped*, i.e.
  toward the neutral.
* **The step budget is 24, and running out is reported as ESCAPED.** The rays
  that exhaust it are the near-tangent ones over open surface, which genuinely
  do escape. At 16 the under-darkening becomes measurable (the 120° wedge read
  0.825 against a closed-form 0.750); at 24 every closed form above lands within
  0.025.
* **Directions are cosine-weighted**, which is both the right measure (it is the
  AO integral, and the mesh's) and the cheap one — near-tangent directions, the
  ones that burn the whole step budget, are exactly the ones a cosine weight
  makes rare.
* **12 directions, spun per hit** (2026-09-07; was 24 fixed). A *fixed* set
  answers `escaped/N`, so its quantum is `1/N` and every closed form has to
  land on a multiple of it — and *which* multiple is decided by where the
  branchless ONB happened to put `u`. Swept over the frame azimuth on the 90°
  wedge fixture, the 12-direction fixed set lands anywhere in **[0.417,
  0.583]**; that the 24-direction set hit 0.500 exactly was accuracy bought
  with directions to out-vote an accident. Spinning the set about the normal
  makes the azimuth a *sampled* variable instead: the azimuth-averaged reading
  is **0.5000 at N = 8, 12, 16 and 24 alike**, and 0.7486 / 0.7492 / 0.7495 /
  0.7497 against the 120° closed form 0.7500. The elevations are **not**
  jittered — they stay stratified at the midpoints `cos θᵢ = √((i+½)/N)`,
  whose own midpoint-rule error is the 0.001 in that row, because jittering
  within a stratum would let a direction land arbitrarily close to the tangent
  plane and a near-tangent ray is precisely the one that burns the whole
  24-step budget. Rotating what is free and stratifying what is expensive is
  the trade. It also retires the branchless-ONB discontinuity as a source of
  spread, since the set is uniformly spun about the normal either way.
  Direction count remains the one knob that decides what this feature costs
  (§6).
* **The rotation is keyed on the hit position, in the provider's own object
  space.** Object space so two instances of one geometry at different world
  scales still agree (the radius convention's own guarantee, and
  `SurfaceSignalsTest` case (i)); position rather than a sample index because
  a position is all the estimator is handed, and it is the right key anyway —
  every camera sample in a pixel lands at a different sub-pixel position, so a
  pixel averages many rotations, while a shading point queried repeatedly
  *within* one hit gets one rotation and stays self-consistent. The
  `relief_modifier`'s four-tap stencil holds `signals` fixed across the
  stencil by construction, so a spun signal contributes exactly zero relief
  gradient, as it did before.
* **The traces run in LOCKSTEP across the directions** — one step of every
  still-live ray before the next step of any of them — rather than one ray to
  completion before the next is begun. Same points, same step rule, same exit
  tests, same evaluation count, bit-identical answer; what changes is that the
  hardware sees `nActive` *independent* `Map()` calls at a time instead of one
  serial chain in which step k+1's sample point cannot be formed until step k's
  `Map` has returned. Worth **1.8×** on the whole frame on its own (§6).

### 3.2 `convexity` — ball-volume excess, not marched

```
A         = (1/2M)·Σ over M mirrored point PAIRS of Hs(Map(x ± R·yᵢ) − d₀)
convexity = clamp(2A − 1, 0, 1)
Hs(u)     = ½ + ½(1.5t − 0.5t³),  t = clamp(u/w, −1, 1),  w = (3/16)·R
```

with `M = kBallPairs = 16` (so 32 field evaluations, no rays, no marching),
the set rotated per hit by one of `kSignalRotations = 32` pre-built **full 3-D**
rotations chosen by the same position hash occlusion uses.

* **The point set is CENTRALLY SYMMETRIC and lives in OBJECT SPACE — there is no
  tangent frame.** Central symmetry through the query point maps `{d<0}` onto
  `{d>0}` for *any* plane through it, so a planar surface gives `A = ½`
  *exactly*, at any orientation, at any `M`. Measured over 40 orientations:
  spread **0.00000**. An earlier draft aligned the set to the hit normal and
  mirrored about the tangent plane — also exact on a plane, but it needs an
  orthonormal basis, and every branchless ONB has a discontinuity somewhere on
  the sphere: measured frame-rotation spread of `A` at a 90° wedge was **0.028**,
  a 5.5 % step in the mask wherever the branch flipped.
* **`Hs` is smoothed, and odd-symmetric.** A hard `Map > 0` test makes `A` jump
  by `1/2M` every time a sample crosses the surface — deterministic in position,
  so it appears as spatial *banding* rather than as noise, which is worse. Odd
  symmetry (`Hs(u) + Hs(−u) = 1`) is what lets the smoothing coexist with the
  exactness above: for a plane the paired samples sum to exactly 1 however wide
  the band.
* **`M = 40`, band `3/16`** chosen by sweep against closed forms:

  | `M` | band | 90° wedge spread over 40 orientations | mid | sphere `3R/(8ρ)` max err | max step-to-step \|ΔA\| across an arris (64 steps) |
  |----:|-----:|--------------------------------------:|----:|-----------------------:|--------------------------------------------------:|
  | 32  | 3/16 | 0.0312 | 0.7460 | 0.0284 | 0.0140 |
  | **40** | **3/16** | **0.0211** | **0.7471** | **0.0080** | **0.0141** |
  | 48  | 3/16 | 0.0211 | 0.7454 | 0.0155 | 0.0143 |
  | 64  | 3/16 | 0.0135 | 0.7474 | 0.0252 | 0.0139 |

  (truth for the wedge is 0.750). 40 was the knee **for a fixed set**: first row
  with orientation spread ≤ 0.021 *and* sphere error under 0.01; 64 bought 0.008
  of spread for 60 % more evaluations.
* **`M = 16` since 2026-09-07, because the set is now ROTATED per hit and the
  orientation stopped being a variable to out-vote.** Read the table above
  again: every column in it is an *orientation* statistic. A fixed set has to
  carry each closed form pointwise at whatever orientation the feature happens
  to present, and that is what the extra pairs were buying — at 32 fixed pairs
  the sphere identity read **0.159** against 0.1875 on the shipped fixture, and
  at 20 the 90° arris read **0.416** against 0.5, both purely on orientation
  luck. The rotation-averaged reading is flat in the count instead: modelled
  over 64 rotations, **0.185 / 0.178 / 0.183 / 0.181** at 8 / 16 / 24 / 40 pairs
  against 0.1875 (the residual −0.004 is the smoothing band, not the count). 16
  is taken with that margin at 40 % of the evaluations.
* **The rotations are full 3-D and uniform, not a spin about one axis.** This
  set has no frame to spin about — that is the point of it — and a spin about a
  fixed *world* axis would leave any feature whose edge runs parallel to that
  axis sampled identically by all 32 entries, which is exactly the orientation
  accident the table exists to remove. Shoemake's uniform-quaternion map of a
  Halton triple gives the uniform measure on SO(3). **Central symmetry survives
  every rotation** (`−(Qy) = Q(−y)`), which is what keeps the planar `A = ½`
  identity exact — at every orientation, every rotation *and* every count.
* The lattice is a Hammersley-style triple — `cos θ` stratified over the upper
  hemisphere, azimuth from the base-2 radical inverse, radius from the base-3
  radical inverse through `ρ = u^{1/3}` so the points are **uniform by volume**,
  which is what makes `A` a volume fraction and not a weighted one. Compile-time
  constant table: no RNG, no per-hit state, identical on every thread and run.

### 3.3 Why they are not one estimator — the part that cost a render to learn

The obvious design, and the one the first implementation shipped internally, is
`occlusion = clamp(2A, 0, 1)` off the same ball: one measurement, two clamps,
every closed form in §2 satisfied, all 287 unit assertions green.

**It is wrong on the feature occlusion exists for, and only a render said so.**

*Volume is not visibility.* On the wall of `plank_closeup`'s 2.6 mm end check,
queried at 18.6 mm, the ball reaches up out of the slot into open air and the
slot itself removes only ~5 % of a ball that much bigger than it, so `A` came
out at **0.55** — the crack read *unoccluded*, and its dirt vanished from the
image. A half-ball variant (empty-in-front over solid-behind, which is exact on
a plane by the same pairing argument and reproduces every wedge form) moves the
number to **0.81** and does not fix it either, for the same reason: the open sky
above a shallow crack genuinely *is* most of the volume in front of its wall.
Only a directional test knows that none of that volume is reachable.

*And the converse holds, which is why convexity keeps the ball.* Solid angle
cannot see the convexity of a smooth body at all: from a point on a sphere of
**any** radius the solid subtends exactly a hemisphere, so a directional
convexity reads 0 on every sphere. The ball's volume reads `3R/(8ρ)` — "this
bead is proud of its surroundings at scale R", which is what an edge-wear mask
wants.

On **wedges** — edges, creases, corners, the features both are mostly used on —
the two measures agree exactly. They diverge only where each is the right tool:
narrow apertures (visibility wins) and smooth curvature (volume wins).

The lesson worth keeping: *the unit tests could not have caught this.* Every
closed form the accessibility definition offers is a wedge or a plane, and the
ball measure satisfies all of them. The failure lives in the regime the closed
forms do not cover — an aperture much narrower than the query radius — and it
took looking at the picture. `SurfaceSignalsTest` (q) now carries a narrow-slot
guard specifically so the next reader does not have to re-learn it that way.

### 3.4 Heightfield mode refuses, for both

Heightfield-mode `Map` is divided by a single **global** Lipschitz bound
`m_hfLip`, so its magnitude is wrong everywhere the local slope is below the
global maximum. The *sign* is correct, which is all convexity reads — but the
band `w` is expressed in **field units**, so its effective width in true
distance is `w·m_hfLip`; and occlusion's march *steps* by the field value, so it
would crawl by that same factor and exhaust its budget (reporting everything
escaped) wherever the slope is gentle. Neither is worth publishing under a
systematically wrong length, so both refuse, as `thickness` already did.

---

## 4. The mesh counterpart

### 4.1 `occlusion` on a mesh does NOT change — it was already the same quantity

`MeshSignalBake`'s occlusion bake casts 64 **cosine-weighted** rays over the
outward hemisphere and stores the escape fraction — bit for bit the integral
§3.1 now marches. The two families already agreed; only the SDF's estimator was
on a different footing. **No baked mesh occlusion value moves as a result of
this work**, which is worth stating plainly: the change is confined to the SDF
family.

### 4.2 `convexity` on a mesh is a new bake, over the FULL sphere

A cosine hemisphere cannot report convexity: it saturates at 1 for a plane and
for every convex feature alike. So:

```
A_ray     = fraction of 64 uniform FULL-sphere directions that escape within R
convexity = clamp(2·A_ray − 1, 0, 1)
```

Directions are emitted as antipodal pairs from the same hemisphere lattice the
other bakes use, so the half of the sphere pointing *into* the solid is resolved
as well as the open half — and that half being blocked is precisely what makes
`A = ½` on a plane. On a wedge of solid dihedral `θ` this is `1 − θ/2π`, so it
matches the SDF's ball-volume answer on every edge, crease and corner; on a
smooth sphere it reads 0 where the SDF reads `3R/(8ρ)` (§3.3's asymmetry,
documented rather than reconciled).

**The one bias, stated at its bound.** Bake rays start at
`vertex + n̂·originEpsilon` — they must, or the triangles incident on the vertex
answer "hit" for free. On a flat surface a ray aimed just below the horizon then
escapes instead of hitting whenever `|cos θ| < originEpsilon/R`, so a flat mesh
reads `A ≈ ½ + originEpsilon/R`. With `kOriginEpsilonFraction = 1e-4` and a
typical `R = 0.05` of the diagonal that is `convexity ≈ 0.004`. Bounded,
one-sided (never negative, so a mask lights nothing), and an order of magnitude
under any threshold an author would set — but it is a bias, not noise, and
`MeshSignalBakeTest` (p) pins it at that bound rather than leaving it to be
rediscovered.

### 4.3 One structural bug this surfaced

`MeshSignalBakeCache::FindOrBuild` opened with a **whitelist** of the two kinds
that existed when it was written, rather than a range check against
`eKindCount`. `eConvexity` therefore silently returned its neutral fallback and
baked nothing — a whole review round of "the mesh convexity test reads 0" before
the missing log line gave it away. It is a range check now, with the reason
recorded in the code.

---

## 5. Neutral fallbacks, and the parameter surface

| signal      | range | neutral (absence) | why that end |
|-------------|-------|-------------------|--------------|
| `occlusion` | [0,1] | **1** (unoccluded) | unchanged |
| `thickness` | [0,1] | **1** (thick)      | unchanged |
| `convexity` | [0,1] | **0** (flat)       | the do-nothing end: an edge-wear mask on an unsupported geometry must light *nothing* |

Note `convexity`'s neutral sits at the opposite end of its range from the other
two. That is the point, not an inconsistency: all three neutrals are "the
do-nothing end", and for an edge-wear mask that means *no edge here*, never
*knife edge everywhere*.

Who answers, unchanged from Phase 2/3: the **SDF family** answers all three
live, at any radius, constant or computed; **indexed triangle meshes** answer
from a lazily baked per-vertex table and therefore require a *literal* radius
(at most 8 distinct radii per signal per mesh); **everything else** — analytic
primitives, non-indexed meshes, heightfield-mode `sdf_geometry` — returns the
neutral value. `convexity` joins the existing kind enumeration, so it inherits
the whole contract (`kFnConvexity` / `kFnConvexityDynR`, the literal-radius
proof, the 8-table cap, the null-sentinel-on-failure discipline) without a
second mechanism.

**Radius semantics are identical to `occlusion`'s**: `radius` is a **fraction of
the hit geometry's own bounding-box diagonal**, not a world length, so
`convexity(0.02)` means "the 2 %-of-the-object edges" and reads the same on every
instance and at every scene scale. A literal `radius ≤ 0` is a compile error; a
computed one that lands `≤ 0` returns the neutral value.

### 5.1 CSG subtraction needs one more bit than it used to

Four sites in `CSGObject` already negate `signals.nObject` when a subtraction
credits the reported surface to the subtrahend. That flip remains load-bearing —
`thickness` marches along `−n`, and occlusion lifts its march origin along `+n`
and samples the outward hemisphere about it. But **both new estimators also read
the field's SIGN to decide what is solid, and no normal flip can reverse a
sign.** Under the retired estimator the flip did double duty, because a
sign-blind shortfall measure could not tell "reverse the direction" from
"reverse the sense".

So the sense now travels explicitly, as `SurfaceSignalInfo::bComplementedField`,
toggled (not set, so nested subtractions compose) at those same four sites.
Without it a carved pocket's floor reads *solid where it is empty*: every
outward ray blocks at its first sample and occlusion collapses to a sealed 0 —
wrong in the direction that looks plausible, since a black cavity floor is what
one expects to see. `SurfaceSignalsTest` (k) pins both the corrected value
(`1 − (R/2ρ)² = 0.52` on the shipped fixture) and that collapse.

The mesh family ignores the flag: a baked table is computed over the mesh's own
solid and cannot be complemented after the fact. That is the same behaviour a
subtracted mesh operand had before the flag existed, not a new gap.

### 5.2 `convexity` vs `curv`

Complements, not substitutes, and the distinction is exactly the one §0 opened
with:

* `curv` is the **differential** mean curvature, radius-free, in units of
  1/length normalised by the object diagonal. It answers "how sharply does this
  surface bend *here*", which is unbounded above and therefore has no
  object-independent threshold — the nail's 3 mm shank reads ≈ 14.
* `convexity(r)` is a **radius-sampled** [0,1] with a fixed geometric meaning at
  every value: 0.5 *is* a 90° arris, 0.75 *is* a three-face corner, on every
  object in every scene. Thresholds are portable by construction.

Use `curv` when you want the form's signed bending at no particular scale; use
`convexity(r)` when you want "wear the edges that are about `r` across".

### 5.3 Naming

Shipped as **`convexity(radius)`**, not `edge(radius)`.

* Every other builtin in this VM is named for the **quantity** it returns —
  `occlusion`, `thickness`, `curv` — not for the effect an author reaches for it
  with. `edge` would be the odd one out, and would read as a promise about
  *sharpness* the measurement does not make: a large smooth bead genuinely reads
  convex at a small radius, and under the name `edge` that looks like a bug.
* `convexity` states the relation to its sibling in the name: `occlusion` is the
  deficit below the planar reference, `convexity` the excess above it.
* `edge` is also a very generic identifier to spend in a small builtin namespace.

The one thing the name gives up: it can be misread as *signed* (concave →
negative). It is not — it is clamped at 0 below, because `occlusion` already
owns that half of the range. The descriptor text says so in its first clause.

---

## 6. Cost

### 6.1 As shipped 2026-09-06, and why it was not acceptable

| query | field evaluations per call | note |
|-------|---------------------------:|------|
| old `occlusion` (retired) | 6 | 5 taps + `d₀` |
| `occlusion` (2026-09-06) | 24 sphere traces × 7.57 steps = **182** | the expensive one |
| `convexity` (2026-09-06) | **81** | 80 ball samples + `d₀` |
| `curv` (for scale) | ~18 | 3 `GradientNormal` calls, gated |
| `thickness` | O(march) | unchanged |

`plank_closeup` (640×480, 48 spp) went from **36 s to 120 s**. The user's
standing rule is that a performance hit has to be worth it, and a 3.3× frame
for two texture terms is not.

### 6.2 The measurement, 2026-09-07

**Protocol.** Renders are *interleaved* — base, candidate, base, candidate — in
one warmed session, because this workstation's clock state moves far more than
the effect being measured: the first two or three renders after a build come in
up to 35 % fast and then settle, which is how §6.1's own "identical
configurations came in 20 % apart" was produced. Interleaved, run-to-run spread
is **± 0.2 s on a 120 s frame**. Call and evaluation counts come from a
temporary thread-local counter (removed before commit).

**Where the time was.** 640×480×48 = 14.7 M camera samples, but each signal is
evaluated **210,762,743** times — **14.3× per camera sample**, because
`expr_plank` feeds `ramp_plank` (rd), `sp_plank_rough` (alphax *and* alphay) and
`sp_plank_relief` (the relief modifier's four-tap stencil), and every one of
them re-runs the whole program at the same hit. Per frame that is **5.52 × 10¹⁰
field evaluations**: 3.83 × 10¹⁰ in occlusion, 1.69 × 10¹⁰ in convexity.
`Map()` *is* the inner loop — the marching body is one `EvaluateParts` plus
three multiply-adds and two compares — so `EvaluateParts` is 100 % of the term,
at `O(#parts)` (2 for the plank, 3 for the nail).

**Where the marching time goes.** Of 5.06 × 10⁹ direction traces, **99.3 %**
exit through the `d ≥ R − t` escape certificate, 0.68 % are blocked, 0.04 %
exhaust the 24-step budget and 0.0002 % walk to `t ≥ R`. The mean is **7.57
steps of a 24 cap** — i.e. the budget is almost never the binding constraint,
and the cost is the ramp-up from the `R/64` lift to a clearance that covers the
remaining reach. That ramp is geometric at rate `1 + cos θ`, so it is the
near-tangent directions that are expensive, which is exactly what the cosine
weight already makes rare.

### 6.3 What each change bought, measured alone

| configuration | frame (3 interleaved runs) | vs base | occlusion | convexity | rest of frame |
|---|---:|---:|---:|---:|---:|
| base (24 dirs, 40 pairs, ray-at-a-time) | **120.00 ± 0.15 s** | 1.00× | 69.8 s | 15.7 s | 34.5 s |
| + lockstep marching (bit-identical) | **77.16 ± 0.15 s** | 1.56× | 27.7 s | 15.7 s | 34.5 s |
| + 12 dirs / 16 pairs, rotated per hit | **54.68 ± 0.06 s** | **2.20×** | 13.9 s | 6.3 s | 34.5 s |

The three frame figures are one interleaved session (120.04/119.84/120.13,
77.33/77.03/77.12, 54.61/54.72/54.71). The per-signal columns come from a
separate same-binary sweep in which each estimator's sample count could be set
to zero at runtime, interleaved the same way: 24 dirs + 40 pairs 77.9 s, 24
dirs alone 62.2 s, 40 pairs alone 50.2 s, hence convexity 15.7 s, occlusion
27.7 s, everything else 34.5 s. Both estimators are linear in their count, so
the model predicts 34.5 + 12·1.154 + 16·0.393 = **54.7 s** for the shipped
configuration against **54.68 s** measured.

Per field evaluation, which is where the lockstep result is legible:

| | evaluations / call | ns / evaluation |
|---|---:|---:|
| occlusion, ray-at-a-time | 181.7 | **1.82** |
| occlusion, lockstep | 181.7 | **0.71** |
| convexity (no chain between samples, either way) | 80 | **0.93** |

Ray-at-a-time marching is one long serial dependency — step k+1's sample point
cannot be formed until step k's `Map` has returned — so the core stalls on
`Map`'s latency once per step. Lockstep hands it `nActive` independent `Map`
calls at a time and the per-evaluation cost falls *below* convexity's, which
never had a chain to begin with. Same points, same step rule, same evaluation
count, bit-identical answer.

The counts then come down 2× and 2.5× **because the per-hit rotation removed
the orientation as a variable** (§3.1, §3.2) — not because the accuracy
requirement was relaxed. Options measured and rejected on the way:

* **Cutting the counts without rotating** (the obvious first move): at 12 dirs /
  20 pairs the *render* is indistinguishable from base (mean |Δ| 1.048 against a
  same-binary noise floor of 1.032, on 0–255), but `SurfaceSignalsTest` goes red
  in three places — the 90° wedge reads 0.583 against 0.500, the arris 0.416
  against 0.500, the corner 0.638 against 0.750. The contract is the closed
  forms, not the one scene.
* **Over-relaxed sphere tracing** (Keinert 2014, with the sphere-overlap
  validity check): modelled at **7.62 → 5.0 steps** at ω = 1.8, i.e. ~34 % off
  occlusion, or ~5 s of the final frame. Not taken: it needs three more
  per-ray state values, a retreat branch, and a special case where the relaxed
  step would carry `t` past `R` with the interval `[t+d, R]` unverified — real
  complexity in a numerically delicate estimator for a fifth of what lockstep
  gave for free. It remains the next lever if one is wanted.
* **A per-hit or per-position memo.** Not attempted *in this pass*, and named
  here as the largest remaining lever by a wide margin (~10× on the redundant
  calls). It was then attempted and **shipped the same day** — §6.5. It did not
  need the blast radius §10 feared: it is thread-local, not on the hit record.

### 6.4 The result

`plank_closeup`: **120.00 ± 0.15 s → 54.68 ± 0.06 s**, a **2.20×** speedup,
against a target of ≤ 55 s. Field evaluations per frame fall 5.52 × 10¹⁰ →
2.58 × 10¹⁰ (occlusion 3.83 → 1.91 × 10¹⁰, convexity 1.69 → 0.67 × 10¹⁰).

The image is inside the renderer's own noise. Two renders of the *same* binary
differ by mean |Δ| **1.034–1.036** / RMS 2.78 / p99 13 (0–255, the render seeds
from the wall clock); new against base differ by mean |Δ| **1.037–1.038** /
RMS 2.77–2.79 / p99 13, with a signed mean of −0.002 to −0.017. Read side by
side, the end check still collects dirt along its length and fades as it
closes, and the nail's head-to-shank fold still rusts.

Each `Map()` is `O(#parts)`. All of it is paid **only when an expression
actually calls the builtin** — the lazy-by-construction property from Phase 2 is
unchanged, and a scene whose materials never mention the builtins pays nothing.
An author who wants a cheaper profile still has `curv`, unchanged.

### 6.5 The per-hit memo, 2026-09-07

§6.3's last rejected option and §10's last bullet both name the redundant calls
as the largest remaining lever. It was taken the same day, and it is the larger
of the two results on this page.

**What shipped.** A **thread-local two-level memo**,
[`src/Library/Utilities/ExpressionMemo.h`](../src/Library/Utilities/ExpressionMemo.h),
four ways at each level, keys compared exactly (never hashed), dropped by a
process-wide generation counter bumped at every seam where scene state can move
between passes. **L1** keys the signal builtin on the query (`fn`, radius,
constant-radius proof) plus every field of the hit's `SurfaceSignalInfo`, and
sits in `SurfaceSignalInfo::SignalQuery` — the one place the three wrappers'
fallback/clamp policy already lived, so a hit and a miss are indistinguishable
by construction. It is what catches the relief stencil, which holds `signals`
fixed while moving `P`/`Po`/(u,v). **L2** keys the whole program on (a
process-unique compile-time program id, **which painter pipe is asking**, every
field of `ExprEvalContext`) and sits in the two painters, catching repeated
consumers at one hit and the spectral pipe's per-wavelength `GetColorNM`. The
pipe tag was added in review round 1 and is not decoration: on a scalar-typed
program the colour pipe calls `EvalVec3` and the scalar pipe calls `Eval`,
those two are free to differ in the last ulp under `-ffast-math`, and one
compiled program can reach both pipes through the API because a copied program
keeps its id.

Note what it is **not**: it is not on `RayIntersectionGeometric`, which is what
§10 assumed a memo would have to be. Nothing is added to the hit record, no
`mutable` appears anywhere, and the scene stays immutable within a pass.

**Measured**, interleaved base/memo in one warmed session under the same
protocol as §6.2, one binary toggled by the new `expression_memo` option:

| scene | base | memo | speedup |
|---|---:|---:|---:|
| `plank_closeup` 640×480×48 | **54.16 ± 0.17 s** | **16.33 ± 0.01 s** | **3.32×** |
| `weathered_workbench` | 7.17 ± 0.15 s | 2.95 ± 0.06 s | 2.43× |
| `oxidized_copper` | 2.62 ± 0.02 s | 1.72 ± 0.02 s | 1.53× |
| a cheap sub-threshold body | 1.91 ± 0.02 s | 1.94 ± 0.01 s | 0.98× |
| `shapes.RISEscene` (no expressions) | 0.73 ± 0.01 s | 0.73 ± 0.01 s | 1.00× |

**RE-MEASURED 2026-09-07, review round 1**, on different machine load. Both
sets are in the record; the spread between them is what a wall-clock number on
this machine is worth, and the review's numbers are the more conservative end
where they differ:

| scene | claim above | independent re-measure | note |
|---|---:|---:|---|
| `plank_closeup` | 54.16 → 16.33 = **3.32×** | 61.17/61.49/61.02 → 17.24/17.23/17.13 = **3.56×** | the re-run was **under a concurrent link**, which inflates both halves; 3.32× stands as the quiet-machine number |
| `weathered_workbench` | 7.17 → 2.95 = 2.43× | 5.518 ± 0.047 → 2.305 ± 0.013 = **2.39×** | agrees within the spread |
| `oxidized_copper` | 2.62 → 1.72 = 1.53× | 1.914 ± 0.027 → 1.328 ± 0.021 = **1.44×** (n = 6, under **both** contended and clean conditions) | **take the range 1.44–1.53×**; the re-measure is the tighter-error one |
| `shapes.RISEscene` | 0.73 → 0.73 = 1.00× | 0.554 → 0.554 = **1.000×** | confirms the no-expression row is exactly flat |

Hit rates on `plank_closeup` at the shipped four ways, from a temporary counter
that recomputed on every would-be hit and compared (zero mismatches over 421.5 M
L1 and 320.3 M L2 probes): **L1 96.2 %, L2 82.7 %**. L1 saturates at two ways
(the body makes two distinct queries) and L2 at eight (four captures 82.7 of the
84.9 points available). Storage is **1408 bytes of thread-local per worker**,
24.7 kB across 18 — **1440 bytes / 25.9 kB since the L2 key gained its `pipe`
field** (2026-09-07 review round 1); `ExpressionMemoTest` (g) prints the live
figure and asserts a 2048-byte ceiling rather than either number.

**The image check is a NOISE-FLOOR COMPARISON, not a bit comparison, and it
cannot be anything else: there is no render seed to pin.** The CLI seeds the
process RNG from the wall clock (`src/RISE/commandconsole.cpp`,
`srand( GetMilliseconds() )`) and `RandomNumberGenerator`'s default seed is
`rand()`, so the pixel-filter warp and the temporal samples differ run to run —
two renders of the same scene by the same binary do not agree bit for bit even
with no change at all. So the memo's effect is measured against the renderer's
own run-to-run spread, on the same footing as §6.4: mean |Δ| 0.776–0.778 on 0–255
against a same-configuration base-vs-base floor of 0.778–0.781, RMS 2.40–2.42
both ways, p99 11–12 both ways — i.e. the memo moves the image by *less* than
re-running the base does. The bit-exactness claim is made where it can actually
be made: in `ExpressionMemoTest`, where the same painter is driven twice at a
pinned context with the memo off and on.

**The compile-time gate is kept for legibility, not for performance.** A body
shorter than the key comparison's 29 fields (28 before the `pipe` tag), with no
signal and no noise call,
is excluded — that is the 0.98× row above, which returns to ≈1.00× with the gate
forced on. The gate is what makes "the memo never makes a scene slower" a
property of the code rather than of a benchmark.

`expression_memo` (`RISE_OPTIONS_FILE`, default on) is the A/B lever the memo
was accepted on and stays as the debugging aid: if a render ever disagrees with
itself, one run with `expression_memo false` says whether the memo is why.
[`tests/ExpressionMemoTest.cpp`](../tests/ExpressionMemoTest.cpp) is the
correctness gate — differential bit-equality over 10 500 randomized contexts
with live SDF signals, a live mesh bake, red-proofs for the generation counter,
the program id and the kill switch, and (since review round 1)
**one-field-at-a-time separation for every key field at both levels** plus a
cross-pipe check that one scalar-typed program shared between
`expression_painter` and `scalar_painter { expression … }` keys apart. That
last one closed a structural hazard: the two pipes call different VM entry
points on a scalar-typed program, a copied program keeps its id, and both API
factories take the program by const reference — so the L2 key now carries a
pipe tag. **Disclosed precisely:** the *sharing* is certain and readable off the
key, but the *divergence* is not observed on this toolchain — removing the tag
leaves the cross-pipe check green, because `Eval` and `EvalVec3` reach one
`RunAny` and produce the same bits today. The tag guards a `-ffast-math`
inlining freedom that has already moved an ulp in this exact code once (§6.5's
"why the memo lives in the painters"), at a cost of one int compare and 32 bytes
of thread-local. It should not be removed on the strength of that check staying
green without it.

Note what that says about the randomized sweep: a review counted **zero**
aliasing draws across its 10 500 for every key field, and the two red-proofs
confirm the consequence directly — dropping `fwo` from the L2 key, or `baryB`
from the L1 key, each turns exactly ONE assertion red (the new per-field row for
that field) and leaves the rest of the suite green, `(a)`'s 10 500-draw
differential included. Dropping `baryB` does not even redden `(i)`, which
splices all three mesh fields at once and separates on the surviving two. **A
differential sweep proves the memo is invisible; it does not prove the key is
complete.**

Check (j), the shipped-default check, is also the one that touches the machine:
it writes two options files into a private temp directory, sets and restores
`RISE_OPTIONS_FILE`, and spawns a child process — and reports SKIP rather than
FAIL where the environment cannot support that.

---

## 7. BDPT / VCM / MLT residual

Unchanged from, and identical in extent to, the Phase-2 residual already
recorded in [GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md)
§14: **all of `curv`, `occlusion`, `thickness` — and now `convexity` — are fully
correct under the path-tracing rasterizer family (the default), and are
evaluated as their NEUTRAL fallback in parts of the BDPT / VCM / MLT
transport** — the forward walk's own per-bounce re-evaluation, connection / NEE,
and the MIS reverse-pdf evaluation. `convexity` adds no new gap and closes none:
it travels the same `RayIntersectionGeometric::signals` channel, so wherever
that channel is populated it answers, and wherever a synthesised or re-derived
vertex carries no provider it reads its neutral **0** (flat) exactly as
`occlusion` reads its neutral 1.

The renderer's existing one-time warning covers it: `SurfaceSignalDemand::Any()`
is asked "does any live compiled expression call these builtins", and
`convexity` registers a call site in the same `m_signalCalls` list, so a scene
that uses only `convexity` under BDPT still gets the warning. The neutral
direction is the conservative one for the new signal too — an absent `convexity`
wears nothing, rather than wearing every edge in the frame.

---

## 8. Test plan, and what shipped

`tests/SurfaceSignalsTest.cpp` (SDF + VM surface, **318** assertions) and
`tests/MeshSignalBakeTest.cpp` (mesh bake, 110 assertions). Closed-form wherever
one exists — the point of defining the signals this way is that one usually
does.

**POINTWISE versus EXPECTATION, since 2026-09-07.** A hit now draws one of 32
rotations of its sample set (§3.1), so the assertions split in two and the
split is the load-bearing part:

* **Pointwise, unchanged tolerances, and now checked across rotations.** Every
  identity that holds for *each* rotation individually — a plane and every
  convex feature read `occlusion` exactly 1, a plane reads `convexity` exactly 0
  — keeps its 1e-9/1e-12 band, and (m), (o) and (p) additionally require the
  max-minus-min across 128 slid probes to be **exactly zero**. That is a
  *stronger* statement than the old single-probe form: it says no rotation moved
  the answer at all, rather than that one particular rotation gave the right
  one.
* **Expectation, same tolerances, over 128 hits slid along the feature.** The
  quadrature-dependent readings — (o)'s arris and corner, (p)'s sphere series,
  (q)'s wedge sweep — are checked as means via `EvalAtHitMean`, which slides the
  probe 1e-5 along the arris / the crease / a tangent of the sphere, where the
  closed form is constant and only the rotation varies. Tolerances did **not**
  need loosening (0.08 / 0.10 / 0.02 / 0.06 as before); measured means are
  0.449 arris, 0.086 / 0.176 / 0.363 sphere, 1.000 / 0.951 / 0.763 / 0.527
  wedge.
* **Red-proofs that the means still discriminate.** Each expectation is
  additionally required to be *outside* the neighbouring feature's band — the
  arris must not also satisfy the corner's closed form, each sphere radius must
  not satisfy the previous radius', each wedge opening must not satisfy the
  previous opening's (from the 120° step on, where the forms are 0.183 and 0.250
  apart against a 0.06 band; 180 → 150 is only 0.067 apart and the test does not
  pretend to a claim there). Verified red by hand: perturbing the three closed
  forms by 0.10 / 0.03 / ×0.85 turns 8 assertions red.

**Red-proof of the old estimator (n).** A convex 90° edge built as an SDF
`intersect` of two boxes — the hard-`max` case, since a single `roundbox` would
*not* reproduce the bug. The old estimator returned `cos 45° = 0.7071` there;
the test asserts `occlusion ≥ 0.99` and re-derives 0.7071 in a comment. A second
assertion pins the discriminating property the old estimator lacked outright: it
compares that convex edge against a concave valley of the same 90° angle and
requires them to differ by at least 0.4 — under the old estimator both read
0.7071 and the difference was **zero**.

**SDF, closed form:**

1. **(m)** flat plane → `occlusion == 1` and `convexity == 0` **exactly**
   (tolerance 1e-12, on three differently-oriented faces at three radii).
2. **(o)** rounded box: arris `convexity` 0.5 ± 0.08, three-face corner
   0.75 ± 0.10, corner strictly above arris, and `occlusion == 1` at all three
   — the last being the case the old estimator already got right, so the rewrite
   is pinned not to have broken it.
3. **(p)** `convexity(r)` on an SDF sphere against `3R/(8ρ)` at
   `R/ρ ∈ {0.25, 0.5, 1.0}` to ±0.02, plus strict monotonicity, plus
   `occlusion == 1` across the series.
4. **(q)** `occlusion == sin²(α/2)` across a wedge sweep (180/150/120/90°) to
   ±0.06, exactly 1 at 180°, strictly monotone — **and the narrow-slot guard**
   of §3.3: a 1-unit slot queried at 9 units must read below 0.35 on its wall
   and be separated from the open face beside it by more than 0.6. The ball
   drafts read 0.55–0.81 there.
5. **(k)** CSG subtraction: pocket floor against `1 − (R/2ρ)²`, plus the
   collapse-to-0 the complement flag prevents, plus the concave floor being
   reported as a convex edge without it.
6. Everything Phase 2 already pinned, extended to `convexity`: neutral
   fallbacks, parse-time arity / non-positive-literal / vec3-arg rejection, the
   frozen UV-only surface, the constant-radius record, end-to-end through
   `scalar_painter`, VM concurrency, radius-fraction scale invariance,
   heightfield refusal.

**Mesh: the bake is UNCHANGED, and that is the consistent choice, not an
oversight.** The mesh family already applies exactly the policy the SDF family
just adopted — `MeshSignalBake`'s `GoldenRotation` spins its cosine-weighted set
per VERTEX for the same reason, "so 64 samples do not land on the same 64
directions at every vertex". What differs is the count, and it should: the bake
runs **once per vertex at build time**, not once per hit, so there is nothing to
buy by trading its 64 rays down, and its result is barycentrically interpolated
rather than averaged over samples — interpolation smears per-vertex noise
across a triangle instead of cancelling it, which is the opposite of what 48 spp
does to the SDF family's per-hit draw. Same integral, same direction policy,
counts sized to their own cost model. `occlusion` values unchanged on the
existing fixtures (the bake was already the right quantity); `convexity` on a baked box mesh ≈ 0 on a face,
0.5 ± 0.15 on an arris, 0.75 ± 0.20 at a corner, with the ordering pinned; the
origin-epsilon bias held under its `originEpsilon/R` bound at three radii; the
bake deterministic, counted once under thread racing, and skipped in draft.

---

## 9. What this does not change

* `thickness`, in any respect.
* `curv` / `curvR`, in any respect.
* Mesh `occlusion` values.
* The dispatch channel, the lazy-bake architecture, invalidation, the
  literal-radius contract, or the neutral-fallback philosophy.
* Scene-wide (inter-object) AO, which remains declined for v1 — every signal
  here is still strictly **object-local self-occlusion** (re-measured and
  declined again 2026-09-07: [GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md)
  §8.1, which also prices the 14.3× multiplicity from §6.2 as the reason a
  live scene query costs 2.4× on this scene).

## 10. Known residuals

* **Every cavity reading moved, by exactly the §2.1 square.** Flat and convex
  surface is pinned at 1, but anything that was between 0 and 1 is now its own
  square. Measured on the `materials-and-media-basics` head fixture, the knot
  seam went 0.68 → **0.50** (0.68² = 0.46, within the 24-direction quantum);
  its scar, a shallow spherical dimple, stayed at **1.0** — both estimators are
  blind to a pit until the query radius approaches its diameter, the old one
  because the field reports no shortfall inside a sphere and the new one because
  a ray crosses a chord of `2ρ cos φ` before hitting anything. Two documents
  quoted the seam figure; both now carry the new one and the reason.
* **The SDF and mesh families disagree on smooth curvature for `convexity`**
  (SDF `3R/(8ρ)` on a sphere, mesh 0), for the reason in §3.3. Reconciling would
  mean giving the SDF a per-direction march for convexity too (hundreds of
  evaluations) or giving the mesh an inside/outside test (ray parity, fragile on
  the open meshes RISE routinely renders). They agree on every wedge.
* **Heightfield-mode `sdf_geometry` still publishes no signals at all** (§3.4).
  The sign of its field is usable; only the band width and the march step length
  are not. A locally-normalised field would fix both and is not attempted here.
* **`kOcclusionDirs = 12` makes a single occlusion reading a DRAW, not a
  value** (2026-09-07). A hit gets one of 32 rotations, so its answer is one
  sample of a random variable whose expectation is the closed form. In a render
  this is strictly better than the fixed set it replaces — the sub-pixel jitter
  puts every camera sample on a different position and therefore a different
  rotation, so 48 spp averages ~32 rotations and the ~4 % *quantisation* the
  24-direction set had is gone too — but it does mean a single-sample consumer
  of the raw signal sees noise where it used to see a staircase. The two places
  that matters are both already handled: the `relief_modifier` holds `signals`
  fixed across its stencil, so a spun signal contributes zero gradient exactly
  as before; and the closed-form unit tests are expectation checks over 128
  slid hits (`EvalAtHitMean`). A future consumer that reads the signal ONCE per
  pixel with no averaging — a hypothetical non-stochastic preview path — would
  want `kSignalRotations = 1` and the old counts back. Every POINTWISE identity
  (plane and convex → occlusion exactly 1, plane → convexity exactly 0) holds
  under every rotation and is pinned with zero-spread assertions.
* **The estimators are called far more often than they are needed.** On
  `plank_closeup` the two signals are evaluated **210.8 million** times for
  14.7 million camera samples — 14.3× — because the field feeds three material
  slots plus a relief modifier and each re-runs the whole expression at the
  same hit. **27 % of those calls (57 M) are the relief stencil's, and they
  cannot affect the image at all**: `ReliefModifier` deliberately holds
  `signals` constant across its four taps, so the signal term cancels out of
  `dT`/`dB` exactly. **CLOSED 2026-09-07 by the memo of §6.5** — a thread-local
  two-level memo that keys the signal builtin on the hit and the whole program
  on (program id, context), 3.32× on `plank_closeup`, and 96.2 % / 82.7 % hit
  rates. The residual multiplicity is unchanged (the estimators are still
  *called* 14.3× per camera sample); what changed is that all but ~4 % of those
  calls now return a stored double. The blast radius this bullet predicted did
  not materialise: the memo is `thread_local` and does **not** live on
  `RayIntersectionGeometric`, so nothing is copied into a painter's context and
  the CSG sense-flip sites are simply a key field (`bComplementedField`).
