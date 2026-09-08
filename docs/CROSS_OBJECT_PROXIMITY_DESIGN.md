# Cross-Object Proximity — a contact signal for grime, dust and wear

**Status:** PROPOSED 2026-09-08, driven by the scene set in §1; implementation
phased in §8. **Predecessor:** cross-object *ambient occlusion*, re-measured and
declined again on 2026-09-07 ([GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md)
§8.1), whose closing paragraph names this signal as the honest successor.
**Inputs:** the §8.1 measurements (two throwaway prototypes, three review-driven
sampler variants); the per-hit expression memo that shipped the same day
([OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md](OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md)
§6.5), which changes the cost side of any live per-hit scene query from "once per
painter call" to "once per hit"; a survey of the geometry zoo for distance
queries (there are none — no geometry answers "how far is your surface from this
point", and the top-level BVH has no point query); and the prior art in §3.
**Nature:** design + phased implementation plan, in the mold of the signals design.

---

## 1. The driving scenes, and what each one must prove

The signal is only worth building if it delivers on the two scenes that motivated
the cross-object work and survives the two that would break a naive design.
These six are the acceptance set; every phase gate in §8 names which of them it
must pass.

| # | Scene | Receiver → neighbour | What it proves |
|---|---|---|---|
| A | `scenes/FeatureBased/Textures/plank_closeup.RISEscene` | SDF plank ← SDF nail lying on it | **The motivating case.** Dirt must collect along the nail's contact line and fade over a few millimetres. §8.1 showed no hemisphere AO sampler can draw this seam (0.65 at the silhouette, 0.96 four pixels out); a distance signal must read ~1 under the shank and ~0 at 4 mm. Also the cost reference: 16.33 ± 0.01 s with the memo. |
| B | `scenes/FeatureBased/Textures/weathered_workbench.RISEscene` | box bench top ← SDF vise flange; infinite-plane floor ← box legs; box tray | The header declares grime under the flange "out of reach — occlusion() is self-occlusion only". The bench top is a `box_geometry` with **no signal provider at all**, so this proves the signal works on receivers outside the SDF/mesh families, and against analytic neighbours. |
| C | `scenes/Tests/Signals/proximity_closed_forms.RISEscene` (new) | plane ← sphere (closed form `sqrt(s²+ρ²) − ρ`), plane ← box, plane ← cylinder lying flat, SDF ← tessellated mesh, an instanced copy, a `casts_shadows FALSE` neighbour, a CSG-subtraction receiver with its operands, a non-uniformly scaled SDF, a neighbour beyond the radius | Drives `ProximitySignalTest`: every family's distance against a closed form, the self-exclusion rule, the visibility rule (casts_shadows does not exempt), the CSG operand rule, the scale rule, and the radius cut-off. |
| D | `scenes/FeatureBased/Combined/showroom.RISEscene` | clipped plane ← risemesh torus knot; analytic primitives | The mesh-neighbour path (Phase 2) on a real asset: dust on the plane around the knot's footprint; also the analytic zoo (cylinder, ellipsoid, sphere, box) as neighbours. |
| E | `scenes/FeatureBased/Geometry/sponza_new.RISEscene` (asset at `/Users/aravind/Working/Assets/main_sponza/...`) | 405 mesh objects, mesh ← mesh everywhere | Cost with the query forced at every hit (the §8.1 protocol), candidate-scan scaling at 405 objects, and a beauty check: dust where walls meet floor, the look production renderers get from Unreal's `DistanceToNearestSurface`. |
| F | `tests/ProximityInvalidationTest` (no scene file) | a neighbour moved through `Cst::DeriveToJobIncremental` between two renders | The signal follows the move with no bake to invalidate — the property that makes this design free where the AO bake was O(scene). |

A and B are production showcases and get the signal composed into their
materials (the dirt term on the plank; the header's "deliberately NOT painted on"
grime on the bench). C and F are tests. D and E are measurements.

---

## 2. The signal

**`proximity(r)`** — `r` a **world length** — returns `clamp(1 − d/r, 0, 1)`
where `d` is the shortest distance from the hit point to the surface of any
**other** world-visible object. **1 = touching, 0 = nothing within `r`.** The
neutral fallback is **0** (the do-nothing end, like `convexity`): no scene, no
neighbour that can answer, an unusable radius, or a non-PT integrator's rebuilt
record all read 0 and paint nothing.

Why this quantity and not a visibility one: §8.1's measurement. A thin object on
a plane subtends grazing directions only, so every hemisphere estimator reads a
2–10 px band that does not look like a seam. The seam *is* the set of points
within a millimetre or two of another surface, which is exactly what a distance
reports and exactly what a ray fan cannot (the same grazing geometry starves a
ray-estimated distance the same way). This is Unreal's `DistanceToNearestSurface`
and Houdini's `xyzdist` against the rest of the scene, not Substance's AO.

Conventions, matching the signal family (signals design §9):

- **Radius unit is a world length**, deliberately unlike the per-object fraction
  `occlusion(r)` takes. §8.1 established that a fraction of the *scene* box is a
  lighting-rig artefact (lights are objects) and a fraction of the *receiver's*
  box cannot describe a neighbour. Authors already reason in world units for `fw`
  and feature sizes (the plank header quotes millimetres throughout).
- **Unsigned distance.** A hit point inside another object's volume reads
  `d = 0` — interpenetration is contact.
- **Self is excluded** by object identity: the object the hit belongs to never
  contributes (its own surface is at distance zero, always). Instanced copies are
  separate objects and do count. A CSG composite is one object; its world-
  invisible operands are not in the render list and never count separately.
- **`casts_shadows` does not exempt** a neighbour — this is geometry presence,
  the same contract `IntersectOcclusionRay` was split out for on 2026-09-07.
- **A neighbour that cannot answer contributes nothing** (it is treated as far),
  and says so once in the log. Honest absence over a wrong distance.

---

## 3. Prior art

- **Unreal** `DistanceToNearestSurface` material node: world-space distance to the
  nearest surface from the global signed distance field; the canonical use is
  contact effects (shore foam, dirt at wall/floor seams). Excludes the pixel's own
  primitive through the "mesh distance field" of *other* objects. Our closest
  analogue in intent and unit.
- **Houdini** `xyzdist` / `ray` SOP: closest point on another geometry, world
  units; the artist composes `1 − dist/r` masks by hand.
- **Blender** Geometry Nodes `Geometry Proximity`: closest point on a target
  geometry (not the whole scene), world units.
- **Substance** has no cross-object proximity; its "Position" and "Thickness"
  bakers are self-only, and its AO baker's "by mesh name" is the only cross-mesh
  channel — an occlusion, which §8.1 showed is the wrong measure for contact.
- **Arnold / RenderMan**: no distance node; contact dirt is done with AO plus a
  hand-painted mask, which is the workflow this signal replaces.

---

## 4. The measured gates this design rests on

From §8.1 (2026-09-07), all reproduced by independent reviewers:

- **Contact grime is a proximity quantity.** Cosine-weighted AO: 0.65 / 0.96 /
  0.97 at 1 / 4 / 10 px beside the nail; horizon-biased (0.5–20°): 0.61 / 0.81 /
  0.90 at 1.93× the cost and 44 % less gain on the nail. Neither reads as a seam.
- **A live scene query from the painter is not reentrant.** The shadow/occlusion
  paths never evaluate a painter; a distance query evaluates no painter either
  (pure geometry). §3(b) of the signals design objected to reentrancy and
  control-flow shape; a distance query has neither.
- **The live query's cost was measured uncached at 2.38× on plank_closeup**
  because the painter ran 14.28× per camera sample. The memo now makes any
  per-hit query cost once per hit; on plank that alone would have cut the AO
  prototype's 2.38× toward ~1.1×.
- **Invalidation of any bake is O(scene)** (Sponza median 33 neighbours per move,
  worst 93 %, and infinite planes invisible to a bbox census). **A live query has
  nothing to invalidate.** With the memo's generation already bumped at every
  scene-change seam, a moved neighbour is seen by the very next pass.
- **Ray fans cannot estimate contact distance** for the same grazing-geometry
  reason AO fails; only true distance functions can. That is what forces the
  per-family closest-point work in §5 rather than reusing the shadow-ray path.

---

## 5. The chosen design

### 5.1 Where the scene enters — the record, not a global

The AO prototype published the object manager as a process global and called it
"not the design". The design is the codebase's own rule: **every per-hit quantity
a painter needs is stamped on the intersection record.** `RayIntersectionGeometric`
gains one typed field:

```cpp
struct ProximityInfo {
    const IProximityQuery* pScene;   // the object manager that found this hit; 0 = none
    const IObject*         pSelf;    // the object the hit belongs to (excluded)
};
```

stamped by `ObjectManager::IntersectRay` after a winning hit (one pointer pair; a
CSG composite's hit is stamped with the composite). BDPT/VCM/MLT records rebuilt
by `PathVertexEval::PopulateRIGFromVertex` carry the null default and read the
neutral 0 — the same disclosed residual the other three signals have, no wider.
The field rides `ExprEvalContext` beside `signals` and enters the L2 memo key
(two pointers; `ProgramKey::kFields` 29 → 31).

`IProximityQuery` is a two-method `const` interface on `IObjectManager`
(tail-appended, the convention that interface already uses):

```cpp
virtual bool NearestOtherSurface( const Point3& ptWorld, const IObject* self,
                                  Scalar maxDistWorld, Scalar& outDist ) const;
```

It is read-only against the sealed scene, evaluates no painter, casts no ray, and
is safe from every render thread by the same argument as `IntersectShadowRay`.

### 5.2 The query — exact distances per family, no rays

`NearestOtherSurface` scans the render list for objects whose world bbox
expanded by `maxDist` contains the point (skipping `self` and world-invisible
objects), asks each for its distance in **its own object space**, converts to
world length, and keeps the minimum. Two new virtuals, both with refusing
defaults so no implementer is forced:

- `IGeometry::DistanceToSurface( ptObject, maxDistObject, outDist ) → bool`
- `IObject::DistanceToSurface( ptWorld, maxDistWorld, outDist ) → bool`, which
  does the transform: point into object space by the inverse world matrix,
  radius by the inverse length scale, result back by the length scale.

**Scale.** Exact for rotations, reflections and uniform scale, through the
`|det|^(1/3)` length scale `Object` already caches for `curv`. For non-uniform
scale or shear the object-space distance is not a world distance; v1 converts by
the geometric-mean scale and **discloses the bound** (`σ_min/σ_gm ≤ error factor
≤ σ_max/σ_gm`) in the doc and once in the log; an exact answer for anisotropic
transforms is Phase 3. None of the driving scenes scale an SDF or mesh
non-uniformly (the plank scene's objects are rigid; the workbench's boxes are
axis-scaled boxes, which Box answers exactly in world space by construction).

**Per family:**

| Family | Distance | Phase |
|---|---|---|
| SDF / skeleton | the field itself: `EvaluateParts(parts, p)`, exact by construction (heightfield mode refuses, as it does for the other signals) | 1 |
| Infinite plane, sphere, box, cylinder, disk, clipped plane | closed forms | 1 |
| Ellipsoid, torus | closed form for the torus; the ellipsoid uses the scaled-sphere bound (exact on the axes, ≤ ratio-of-semi-axes elsewhere, disclosed) | 1 |
| Indexed triangle mesh (every loader but RAW; tessellated primitives; `displaced_geometry`'s internal mesh) | **closest point on the mesh's own BVH** — a bounded-radius traversal that visits nodes in order of AABB distance and prunes past the running best, point–triangle distance at the leaves | 2 |
| Non-indexed mesh (RAW loader) | refuses (no BVH) — honest absence | — |
| CSG composite | refuses in v1 (the composite's surface is not the min of its operands' under subtraction/intersection); a union-min with the exact CSG boundary test is Phase 3 | 3 |
| Bezier / bilinear patches, hair | refuse | — |

A refusal means "this neighbour is far", logged once per geometry kind, never a
wrong number.

### 5.3 Cost, and why it is bounded

Per hit, once (the memo): one bbox scan over N objects (405 tests on Sponza,
~1 µs) plus one distance evaluation per candidate — SDF `O(#parts)`, closed
forms `O(1)`, mesh `O(log n)` expected with the pruned traversal. There is no
sample count and no stochastic term: **the signal is deterministic and exact per
family**, so it needs no rotation trick and produces no noise. A second memo
level for the query itself, keyed on (`pScene`, `pSelf`, world point, `r`),
covers the case where several painters at one hit call it (the plank's colour,
roughness and relief stencil), exactly as L1 does for the signal builtins.

The TLAS point query (walk the top-level BVH for boxes containing the point) is
an optimisation held for observed need: the scan is linear in objects but
runs once per hit, and Sponza is the largest object count in the corpus.

### 5.4 Invalidation

**None to build.** The query is live against the scene the pass was sealed with.
The memo's generation is bumped at every seam where scene state changes
(`Scene::SetSceneTime`, `RayCaster::AttachScene`, `ObjectManager::PrepareForRendering`,
`InvalidateSignalBakes`, every render-pass entry), so a neighbour moved through
the editor or the agent (`DeriveToJobIncremental`'s closure pass drops the TLAS
only on a bbox change, but it is followed by `PrepareForRendering`, which bumps
unconditionally) is seen by the next pass with no dependency graph. Motion-blur
per-sample `EvaluateAtTime` moves neighbours mid-pass; the world point is in the
memo key and every temporal sample carries its own sub-pixel jitter, so a stale
memo entry is the same essentially-unreachable window the other signals have.
Test F pins the editor case.

### 5.5 What does not change

The existing `occlusion` / `thickness` / `convexity` semantics, radius unit and
providers are untouched; `proximity` is a new builtin with its own explicit unit,
never a widening. The VM bodies (`Eval`, `EvalVec3`, `RunAny`, `CallFunc`) stay
byte-identical under the FP contract the memo established; the builtin dispatches
through `CallFunc`'s existing signal path with a new id, and `CallFunc`'s body
gains one `case` — the one place that contract is deliberately moved, disclosed
in the commit and re-verified by `TextureExpressionVMTest` and the memo suite.

---

## 6. Engine principles, checked

- **Scene immutable after Prepare:** the query is `const`, reads sealed geometry
  and transforms, builds nothing lazily. No `mutable`, no locks.
- **Painters as pure functions of the hit and the scene:** the scene reaches the
  painter through the record, typed and `const`; the query evaluates no painter
  and casts no ray, so there is no reentrancy and no new control-flow shape.
- **Neutral fallback** 0 everywhere the record is null or a neighbour refuses.
- **Explicit radius unit:** world length, stated in the descriptor and the skill.
- **Named managers / refcounting untouched:** the record holds non-owning
  back-pointers with the same lifetime argument `SurfaceSignalInfo` makes (an
  object manager and an object outlive every hit record they stamp).
- **PT-first; BDPT/VCM/MLT residual** identical in extent to the other signals.

---

## 7. Adoption surface

`proximity(r)` in the expression descriptor and the `procedural-textures` skill
beside `occlusion`/`convexity`, with one worked example (the nail's contact line)
and the unit stated in every sentence that mentions the radius; scene A and B
headers updated to say the term is now real. `add_wear` is not extended in v1
(it composes per-object signals; a cross-object term needs the author to choose
the radius from the scene's feature sizes).

---

## 8. Phases and gates

**Phase 1 — the query, the record, the builtin, SDF + analytic families, scenes
A/B/C, test F.** Gate: warning-free build; `ProximitySignalTest` closed forms
within 1e-9 on scene C's fixtures; `ExpressionMemoTest` extended for the new key
fields and the query memo (key-separation rows, red-proved); `TextureExpressionVMTest`
846/846; test F green; plank_closeup renders a contact seam that reads (judged
from the crop, compared with §8.1's AO crops) at a cost ≤ 1.15× its 16.33 s
memo baseline; weathered_workbench's bench-top grime reads at ≤ 1.1×.

**Phase 2 — mesh closest point on the BVH (indexed meshes, displaced).** Gate:
closest-point vs brute force over every triangle on random points (differential,
≥ 10⁵ queries, exact to the ulp) and vs the closed-form tessellated sphere;
showroom's plane collects dust around the knot; Sponza with the query forced at
every hit ≤ 1.25× and a floor/wall seam that reads.

**Phase 3 — observed-need:** CSG union-min with the exact boundary test;
anisotropic transforms; the TLAS point query; a signed variant.

Each phase runs the implementation-review-loop to zero P1 before merge.

---

## 9. Test plan

- `tests/ProximitySignalTest.cpp`: scene C closed forms per family; self-exclusion;
  `casts_shadows FALSE` counts; CSG operands never count separately and the
  composite refuses (v1); non-uniform scale bound; radius cut-off exactly at `r`;
  neutral 0 with a null record; the builtin end-to-end through an
  `ExpressionPainter` at a real hit.
- `tests/ExpressionMemoTest.cpp`: two new L2 key fields separate (k rows);
  the query memo's own key fields separate; the generation clears it.
- `tests/ProximityInvalidationTest.cpp`: derive, render a probe pixel, move the
  neighbour through `DeriveToJobIncremental`, render again — the value changes,
  and does NOT change if the neighbour is moved outside `r`.
- Phase 2: `MeshClosestPointTest` differential against brute force.

---

## 10. Residuals, stated up front

- BDPT/VCM/MLT rebuilt records read 0 (the family's disclosed gap).
- Refusing families (RAW meshes, patches, hair, CSG composites in v1) read far.
- Non-uniform scale is bounded, not exact, in v1.
- The candidate scan is linear in object count; Sponza (405) is the measured
  ceiling.
- The signal is unsigned: it cannot distinguish "just above the floor" from
  "just below it"; a signed variant needs an inside test per family (Phase 3).
