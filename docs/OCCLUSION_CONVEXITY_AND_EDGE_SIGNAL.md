# Occlusion, Convexity, and the Planar Reference

**Status:** design + implementation record, 2026-09-06.
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
occlusion = (of kOcclusionDirs = 24 cosine-weighted outward directions,
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
* **24 directions** puts occlusion's quantum at 1/24 ≈ 4 %. It is the one knob
  that decides what this feature costs (§6).

### 3.2 `convexity` — ball-volume excess, not marched

```
A         = (1/2M)·Σ over M mirrored point PAIRS of Hs(Map(x ± R·yᵢ) − d₀)
convexity = clamp(2A − 1, 0, 1)
Hs(u)     = ½ + ½(1.5t − 0.5t³),  t = clamp(u/w, −1, 1),  w = (3/16)·R
```

with `M = kBallPairs = 40` (so 80 field evaluations, no rays, no marching).

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

  (truth for the wedge is 0.750). 40 is the knee: first row with orientation
  spread ≤ 0.021 *and* sphere error under 0.01; 64 buys 0.008 of spread for 60 %
  more evaluations.
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

| query | field evaluations per call | note |
|-------|---------------------------:|------|
| old `occlusion` (retired) | 6 | 5 taps + `d₀` |
| `occlusion` (new) | ~24 sphere traces, ≤ 24 steps each | typically ~200–300 `Map()`; the expensive one |
| `convexity` | **81** | 80 ball samples + `d₀`; no rays, no marching |
| `curv` (for scale) | ~18 | 3 `GradientNormal` calls, gated |
| `thickness` | O(march) | unchanged |

Each `Map()` is `O(#parts)`. All of it is paid **only when an expression
actually calls the builtin** — the lazy-by-construction property from Phase 2 is
unchanged, and a scene whose materials never mention the builtins pays nothing.

Measured on `plank_closeup` (640×480, 48 spp, both signals live on both objects,
one of them inside a relief-differenced field so the whole program re-runs
several times per hit):

* baseline, before any change: **51.0 s**
* shipped: **155 s / 206 s / 219 s** across three runs → **≈ 3–4×**

Two decompositions, both measured back to back on one binary so machine state
cancels:

* **Adding the second signal is nearly free**: the scene's *original* expression
  text (one `occlusion` call, `curv` for the edge) took 411 s against the same
  binary where the shipped text (two `occlusion` calls plus two `convexity`
  calls) took 420 s — **+2 %**. The cost is the estimator, not the extra term.
* **Direction count is the whole knob**: at `kOcclusionDirs = 40` the shipped
  scene took 420 s; at 24 it takes ~210 s, and the two renders are visually
  indistinguishable (the crack, the arris band and the nail all read the same).
  24 is what ships.

Absolute wall-clock on this workstation is noisy -- identical configurations
came in 20 % apart depending on what else had just finished -- so treat "3-4x"
as the honest figure and the evaluation counts as the structural one. An author who wants the old cost
profile has `curv`, unchanged and ~4× cheaper than `convexity`.

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

`tests/SurfaceSignalsTest.cpp` (SDF + VM surface, 287 assertions) and
`tests/MeshSignalBakeTest.cpp` (mesh bake, 110 assertions). Closed-form wherever
one exists — the point of defining the signals this way is that one usually
does.

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

**Mesh:** `occlusion` values unchanged on the existing fixtures (the bake was
already the right quantity); `convexity` on a baked box mesh ≈ 0 on a face,
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
  here is still strictly **object-local self-occlusion**.

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
* **`kOcclusionDirs = 24` quantises occlusion to ~4 %.** Invisible in the
  shipped scene at 48 spp (sub-pixel jitter averages it), but a scene that
  ramped a mask very steeply over a large flat gradient could see it. The knob
  is one constant.
