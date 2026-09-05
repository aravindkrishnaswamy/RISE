---
name: precision-fix-the-formulation
description: |
  When debugging precision-related symptoms — speckle / firefly
  pixels, near-zero comparisons that misclassify, FP-noise spurious
  hits, "my < NEARZERO check is wrong" — resist the urge to widen a
  threshold, add a fudge factor, or pick a magic ε.  Find the
  formulation that's losing precision and fix THAT: polynomial
  deflation when a root is mathematically zero, scale-relative
  comparisons instead of absolute constants, transform all corners
  not just two, evaluate both algorithm branches not the gated one.
  Use BEFORE bumping a `1e-N` constant to make a symptom go away,
  and especially when a reviewer asks "is that really the principled
  fix?"
---

# Precision Issues — Fix The Formulation, Not The Gate

## When To Use

- You see a speckle / firefly / black-pixel pattern on rendered
  geometry and the noise does NOT decrease with more samples (so
  it's deterministic FP, not Monte-Carlo variance).
- You're tempted to widen a `< NEARZERO`, `> EPSILON`, or
  `fabs(x) < 1e-N` check to make a symptom go away.
- You're tempted to add an "ε safety margin" or a scale-dependent
  fudge like `R * 1e-4` because the existing threshold is "too
  tight" / "too loose".
- You're considering making one geometry's `IntersectRay_*` more
  permissive than the others to silence a per-shape symptom.
- A user / reviewer pushed back asking "is that really the
  principled fix?"  (Strong signal — they likely see a deeper
  reformulation you've missed.)

## When NOT To Use

- The bug is a genuine algorithmic / API mistake unrelated to FP —
  wrong loop bound, wrong matrix order, swapped arguments.  This
  skill is for cases where math says `X = 0` but FP says `X = ε`.
- The threshold IS the right primitive but its magnitude was
  authored before the scene scale grew / shrunk.  Making it
  scale-relative is the principled fix; you don't always need a
  full reformulation.
- You've already instrumented and confirmed the formulation is
  numerically clean and a downstream consumer is the actual bug
  (genuinely rare; verify before believing it).

## The Core Question

Before tweaking a `< NEARZERO` / `< EPSILON` comparison, answer:

> Mathematically, what should the value being compared be?

- **Exactly zero (or a specific known constant)** — the FP result is
  noise around that value.  Do NOT try to detect "noise near zero"
  with a tighter or looser threshold.  Find the cancellation or
  reformulation that removes it, or factor out the known root /
  factor explicitly.
- **A specific scale-related quantity** — compare the FP result to
  the natural scale of the problem, not to a magic constant.
  `1e-12` is meaningless when the polynomial's coefficients are
  O(1e5) or when the scene's bounding box diagonal is O(1e-3).
- **An arbitrary value** — the threshold is genuinely a tuning
  knob.  This is the rare case where threshold-tweaking IS the
  principled fix.

If the answer is "exactly zero" or "a known scale", the threshold is
a symptom; the formulation is the bug.

## Decision Tree

```
You see a precision-related symptom (speckle, near-zero comparison
behaving wrong, FP-noise spurious hit / miss).
    │
    ├── Is the offending value mathematically zero (or a known
    │   constant) in the noise-free case?
    │       NO  → Continue below; might genuinely be threshold tuning.
    │       YES → STOP.  Do not tweak the threshold.  Find the
    │              cancellation / reformulation:
    │
    │              • Polynomial root that should be at t = 0
    │                (origin on surface, ray at glancing angle on a
    │                tangent surface, etc.) → DEFLATE the polynomial:
    │                factor out the known root, solve the reduced
    │                polynomial.
    │              • Discriminant near zero from a near-double root
    │                → CLAMP slightly-negative diskr to 0 in the
    │                solver (a tolerance derived from |a²| + 4|b|),
    │                so the double-root case isn't dropped.
    │              • Quantity that should match a natural scale
    │                (sum-of-cancelling-terms, polynomial natural
    │                scale, scene bbox extent) → COMPARE RELATIVE
    │                to that scale, not absolute.
    │
    ├── Is the threshold the right primitive but wrong magnitude
    │   for the scene's scale?
    │       YES → Make it scale-relative: tie the threshold to a
    │              quantity that scales with the input (e.g.,
    │              `(R + r) × 1e-N` for a torus, `|u²| + |q·t|`
    │              × 1e-N for a quartic, `bbox.diagonal() × 1e-N`
    │              for a scene-scale gate).  Document what scale it
    │              represents.
    │       NO  → Continue below.
    │
    └── Are you fixing this at one CALLER of a noisy producer?
            YES → STOP.  Move the fix upstream to the operation
                   that produces the noisy value.  Fixing one
                   caller leaves every other caller exposed.
            NO  → A localised threshold tweak might be legitimate.
                   Add a comment explaining what scale the threshold
                   represents and why this layer is the correct one
                   to enforce it.
```

## Concrete Examples (From This Repo)

### Torus shadow-ray speckle

Symptom: black speckles on rendered tori at 1 spp.  About 1 in 100
shadow rays cast from a torus surface point reported "shadow" that
wasn't there.  Speckle pattern was deterministic and scaled with
torus rotation severity.

Wrong fix (rejected after review): bump `NEARZERO` from `1e-12` to
`(R+r) * 1e-4` inside `TorusGeometry::IntersectRay_IntersectionOnly`.
That widens the self-hit rejection gate so the noisy "self" root
falls below threshold.

Why wrong:
- Magic-number scale factor (`1e-4`) — what does that represent?
- Only patches shadow rays.  `TorusGeometry::IntersectRay` (primary
  / secondary rays) keeps the old gate.
- Threshold lives in the consumer, but the FP noise is generated by
  the producer (the polynomial solver).

Right fix: in `RayTorusIntersection`, BEFORE calling the quartic
solver, detect when `C[4]` is at FP-noise level relative to the
quartic's natural scale (`|u²| + |q·t|`).  When it is, the origin
sits on the torus surface and the quartic mathematically has a root
at `t = 0`.  Factor that root out and solve the deflated cubic
instead.

```cpp
const Scalar quartScale = fabs(u*u) + fabs(q*t);
if( fabs(C[4]) <= quartScale * 1e-10 ) {
    // Origin on surface — drop the known t = 0 root and solve
    // the deflated cubic for the remaining real roots.
    Scalar cubicC[4] = { C[0], C[1], C[2], C[3] };
    n = Polynomial::SolveCubic( cubicC, s );
} else {
    n = Polynomial::SolveQuartic( C, s );
}
```

Principled because:
- Mathematically motivated — deflating a known root is a standard
  polynomial technique, exact at infinite precision.
- Threshold is scale-relative (`1e-10` of the polynomial's natural
  scale), not a fudge constant; works for any torus size.
- Fixes every caller of `RayTorusIntersection` — primary, shadow,
  Manifold-Solver continuations, photon paths — without each having
  to compensate.

### Rotated-object world bbox

Symptom: in `shapes.RISEscene`, middle-column objects had clean
vertical strips of background showing through their visible surface
— only when BSP / Octree was enabled.

Wrong fix: grow the bbox by a fudge factor in BSP construction so
rotated objects' actual extent fits.

Why wrong:
- Only patches BSP placement; the underlying bbox returned by
  `Object::getBoundingBox()` is still wrong for rotated objects.
- Scale-dependent fudge that doesn't generalise.

Right fix: `getBoundingBox()` was transforming `bbox.ll` and
`bbox.ur` — 2 of 8 corners — and `SanityCheck()`-ing the result.
The world AABB of a rotated cube is the AABB of all 8 corners
under the transform.  Transform all 8 and take per-axis min/max.
No fudge factor; the formulation is now exactly correct.

### OQS quartic fallback gate

Symptom: heavily-rotated tori had black speckles where primary rays
hit the surface but the quartic solver returned 0 real roots.

Wrong fix: loosen the `oqs_fact_d0` "is d2 ≈ 0" threshold (the gate
deciding whether to try the identical-α fallback factorisation).

Why wrong:
- `oqs_fact_d0 = sqrt(macheps)` is the published OQS value;
  arbitrary inflation admits non-degenerate cases the algorithm
  shouldn't fall back on.
- Doesn't address the actual cause: the algorithm chose one of
  three LDL^T parameterisations (the lowest-LDL-error one) and
  committed to its real-vs-complex verdict, even when another would
  have found real roots.

Right fix: drop the gate.  Always evaluate the identical-α fallback
when `d3 ≤ 0` (the regime where it produces real roots), compute
its forward error, pick whichever factorisation has lower error.
When the primary path was already valid with lower error we keep
it; when the fallback wins we switch.  Both branches evaluated, no
threshold guesswork, no missed roots.

### Bilinear-patch shadow-ray self-shadowing (masqueraded as an MIS bug)

Symptom: a `weave_material` curtain (`transmission thin`,
`ScattersFullSphere() == true`) lit from behind by a mesh area light
showed path tracing's transmitted radiance scaling super-linearly in
the material's `transmit` parameter (`transmit = 1.0` gave ~47x
`transmit = 0.1`'s response, not the ideal `10x`) while BDPT — sharing
the identical material code — stayed exactly linear.  Every pointwise
MIS check came back clean: the `(p_light, p_bsdf)` pair
`LightSampler::EvaluateDirectLighting` computes for a mesh-luminary NEE
sample and the `(bsdfPdf, p_nee)` pair `PathTracingIntegrator` computes
for the matching BSDF-sampled emission hit were numerically identical
for the same direction, so `PowerHeuristic`'s partition-of-unity held
exactly, as it algebraically must.

Wrong direction (the one the symptom points at): keep digging in the
MIS weight code for a subtle pairing mismatch — a plausible-looking
hypothesis (a full-sphere material's NEE and BSDF-hit sides using
inconsistent `cosLight` sign conventions) that a `git grep` and careful
derivation both ruled out.

Right diagnosis: isolate each strategy alone and unweighted (NEE-only:
force `w = 1`, suppress the competing BSDF-sampled emission; BSDF-only:
the reverse) and compare their means directly.  Two unbiased estimators
of the same integral must agree; they didn't — NEE-only read only ~10%
of BSDF-only's mean, both linear in `transmit` individually.  That
narrowed the search to "why is NEE's REALIZED value wrong", not "why is
its WEIGHT wrong" — and instrumenting NEE's shadow ray found the answer
directly: 93.6% of shadow rays from the curtain toward the light behind
it were spuriously self-shadowed by the curtain's OWN geometry.

The self-intersection distance is mathematically zero — the shadow
ray's origin sits exactly on the same `clippedplane_geometry` bilinear
patch it just scattered from.  `RayBilinearPatchIntersection`'s self-hit
rejection compared the computed `dRange` against the fixed absolute
`NEARZERO` (`1e-12`), but `dRange`'s numerator (a difference of two
~O(1-3)-magnitude world coordinates, one of them reconstructed through a
quadratic-solved `(u, v)`) carries FP round-off that scales with the
MAGNITUDE of those coordinates.  Measured directly: self-intersection
`dRange` values of `1e-12` to `3e-12` at this scene's coordinate scale —
straddling `NEARZERO`, so roughly half of all self-intersections
registered as genuine tiny-positive-distance hits.

Why this was invisible as a simple linearity check: the self-shadow
rate doesn't depend on `transmit` (it's pure geometry), so NEE-only was
still exactly linear, just deflated by a constant factor.  The
super-linear SHAPE came from MIS correctly shifting weight share from
the (deflated) NEE strategy toward the (healthy) BSDF-sampling strategy
as `transmit` grew and `bsdfPdf` grew with it — a real, correctly-computed
weight shift, wearing a geometry bug as a costume.

Wrong fix (rejected by inspection, matching the torus example above):
widen `NEARZERO` inside `ClippedPlaneGeometry::IntersectRay_IntersectionOnly`
alone.  Same three objections as the torus case — magic multiplier,
leaves `IntersectRay` (primary/continuation rays) exposed to the
identical noise, and the threshold lives in a caller of the noisy
producer rather than in the producer itself.

Right fix: in `RayBilinearPatchIntersection` (the shared producer, whose
other LIVE caller is `BilinearPatchGeometry` — the `bilinearpatch_geometry`
chunk area lights use; `RayTriangleIntersectionWithDisplacement` also
calls it but is dead, never-invoked code), compute a scale-relative floor once —
`tMin = NEARZERO * (1 + coordScale)`, `coordScale` the largest L1
magnitude among the ray origin and the patch's four corners — and use
`dRange > tMin` (not `dRange > 0` / `dRange > NEARZERO`) at all three
of the function's hit-acceptance sites.  Every caller benefits: shadow
rays, primary rays, and continuation rays after a scatter event.

Full write-up, the MIS-side proof that the weights were never wrong,
and the regression test: [CLOTH_FABRIC_DESIGN.md §15 debt
21](../CLOTH_FABRIC_DESIGN.md) and
[`tests/FabricRenderTest.cpp::TestAreaLitSheerWeave`](../../tests/FabricRenderTest.cpp).

**Sibling audit, closed 2026-09-05.** The same scale-relative floor —
computed once at the producer, `tMin = NEARZERO * (1 + coordScale)` —
now lives in `RaySphereIntersection` (both overloads), `RayQuadricIntersection`,
`RayPlaneIntersection`, `RayTriangleIntersection`, and both cylinder
intersection paths, closing the identical bug class each of those
producers had: a self-hit root at the origin's own published point,
sized by the SAME coordinates the fixed absolute `NEARZERO` never scaled
with. The worst measured cases were `RayPlaneIntersection`'s: a
`circulardisk_geometry` / `infiniteplane_geometry` curtain lit from
behind by a full-sphere-transmissive `weave_material` read PT at 1/393
and 1/354 of BDPT before the floor and 1.000 after, at unit scale — no large
coordinates needed, because a shadow ray that CROSSES a flat surface hits
this the same way at any scale. `box_geometry` does not take this fix; it
keeps the per-axis plane-distance band below, because a box's self-hit
`t` blows up at grazing exit angles in a way no single scale-relative
range threshold can bound (see the next example). Full sweep across all
these producers, plus the torus (reverted — did not close) and a
1000×-coordinate Lambertian regime the crossing-shadow-ray trigger alone
doesn't reach: [CLOTH_FABRIC_DESIGN.md §15 debt
25](../CLOTH_FABRIC_DESIGN.md)'s "Sibling audit" sub-block.

### Box self-root: an on-face origin is a plane-distance fact, not a range threshold

Symptom: a closed `box_geometry` carrying a thin-transmissive
`weave_material` read BDPT/VCM ≈ 0.17× PT with a light behind the box
and ≈ 2.25× PT with a light inside it, while the same six faces built
as free-standing `clippedplane_geometry` quads read 1.00 either way
([CLOTH_FABRIC_DESIGN.md §15 debt 25](../CLOTH_FABRIC_DESIGN.md)).

The self-intersection is the same shape as the torus and bilinear-patch
examples above: a ray origin published by `Object::IntersectRay` sits
~1e-12 (`SURFACE_INTERSEC_ERROR`) off the face it just hit, and the
box's slab test reports that re-crossing as the PRIMARY root, whose `t`
straddles `NEARZERO`.

Wrong direction: widen the `dRange < NEARZERO` range-reject threshold
inside `IntersectRay_IntersectionOnly`, or pick a bigger epsilon.  Both
fail for the same reasons the torus and bilinear-patch fudges do, plus
a shape-specific one: the self-hit `t` for a box is
`t_self = δ · |cos θ_in| / |cos θ_out|`, `δ` the ~1e-12 back-off
applied along the INCOMING ray (so the plane distance is `δ·|cos θ_in|`)
and `θ_out` the angle between the outgoing ray and the face normal.  A
grazing
continuation ray (`θ_out` near 90°) makes `t_self` arbitrarily LARGE —
there is no single range threshold, however generous, that rejects
every self-hit and accepts every genuine near hit, because the two
overlap in `t` for a small enough grazing angle.  A range threshold is
the wrong KIND of test for a fact that has nothing to do with distance.

Right fix: "the origin is on this face" is a PLANE-DISTANCE fact, not a
range fact — independent of `θ_out` entirely.
`BoxGeometry::DropSelfHitRoot` tests, per candidate root's face,
`|origin.axis − bound| ≤ 4·NEARZERO + 64·DBL_EPSILON · |origin.axis|`
(PER AXIS: the plane distance is an exact subtraction of two nearby
numbers, so only THAT coordinate's rounding can move it — a first cut
that summed all three coordinates and the box's half-extents, on the
bilinear-patch pattern above, grew with the box's transverse size and
swallowed the deliberate 1e-12 standoff CSGObject's exit-face probe
relies on; review round 2 of debt 25) and drops
that root in favour of the OTHER root when it matches — cancellation-free,
because it never computes a `t` for the self-hit at all; it asks the one
question that is actually true regardless of ray direction.  The
survivor is then treated as an EXIT hit, matching what would have
happened had the origin been published a hair further along the ray.

This is the same idea `RaySphereIntersection` already applies, just
spelled as a range threshold instead of a plane-distance test: it skips
any root `<= NEARZERO` at the origin and returns the NEXT one.  That
works for a sphere because a sphere has no flat faces for `t_self` to
blow up against at grazing angles — the same simplification would be
unsound on a box.

Full write-up: [CLOTH_FABRIC_DESIGN.md §15 debt
25](../CLOTH_FABRIC_DESIGN.md) and
[`src/Library/Geometry/BoxGeometry.cpp`](../../src/Library/Geometry/BoxGeometry.cpp)'s
`DropSelfHitRoot` comment block.

## Anti-Patterns

### "Add an ε to the comparison"

`if (x < EPSILON)` widened to `if (x < 100*EPSILON)` is the classic
paper-over.  Ask: what should `x` actually be in exact arithmetic?
If the answer is "zero by mathematical identity," you have a
cancellation; reformulate.

### "Add a scale factor with a magic number"

`if (x < scene_scale * 1e-N)` is better than absolute `1e-N` but
still has a magic constant.  If the scale factor is necessary,
derive it from the actual numerical operation (sum-of-cancelling-
terms × macheps × safety, polynomial natural scale, etc.) and
document what the constant represents.  Numbers like `1e-10` are
fine when they're "10 × machine-epsilon safety on the natural
scale"; they are not fine when they're "this is what happened to
make speckles disappear."

### "Fix it at one of the N callers"

If a noisy value is produced by `f()` and consumed by `g1()`,
`g2()`, ..., fixing the threshold in `g1()` leaves `g2()` exposed.
Move the fix into `f()` so every caller benefits.  Concretely in
this repo: fixing only `IntersectRay_IntersectionOnly` (shadow
rays) would have left `IntersectRay` (primary / secondary rays)
running the same noisy path through the same FP cancellation.

### "Add `if (fabs(x) < ε) x = 0;` to clamp the noise"

Occasionally legitimate (clamping a discriminant that should be
≥ 0 but FP rounded slightly negative — e.g., `oqs_solve_quadratic_real`
on a near-double root).  But ask first: WHY is `x` noisy?  If it's
"`u² − q·t` where `u² ≈ q·t`," clamping at the consumer doesn't
help anything downstream that depends on `x`.  Often the right move
is to compute the difference accurately (compensated arithmetic,
algebraic factoring, or evaluating the equivalent form that doesn't
cancel).

### "Tighten the gate to be more aggressive"

The mirror failure mode: making a gate stricter to reject more
"noise."  Rejects legitimate close-hits along with the noise.
Symptom: missing shadows from objects that ARE genuinely close, or
missing reflections at glancing angles.  Same diagnosis as widening
— the gate shouldn't be doing the work; the formulation should.

## How To Recognise You're In This Skill's Territory

Watch for these in your own thinking / code:

- "I'll just bump NEARZERO to a bigger value here."
- "Add a fudge factor scaled by R / scene_diameter / r²."
- "This works for most cases — I'll add a special threshold for
  the edge case."
- "The solver returns a tiny root that shouldn't be there.  I'll
  filter it out."
- "Increase the epsilon and see if it fixes it."
- "This geometry is special — it needs a wider tolerance than the
  others."

Each of these is a signal to STOP and ask: what should this value
be in exact arithmetic?  Where is the precision being lost?  Can I
reformulate to remove the cancellation / preserve the known
identity?

## Stop Rule

The skill's work is done when one of:

1. You found and fixed the formulation bug — the threshold tweak is
   no longer needed at all.
2. You can articulate, in a one-line code comment, what the
   threshold actually represents in scale-relative terms (e.g.,
   "FP-noise bound on `u² − q·t` cancellation, scaled by the
   natural quartic scale `|u²| + |q·t|`").  The constant is
   derived, not magic.
3. You traced the noisy value to its source and confirmed the fix
   genuinely belongs at a downstream gate (rare).  Then the
   gate's comment cites which producer is structurally unable to
   produce a clean value and why.

If you've added a constant `1e-N` somewhere and can't explain in
one sentence what numerical operation produces noise of that
magnitude, the principled fix has not been found yet.
