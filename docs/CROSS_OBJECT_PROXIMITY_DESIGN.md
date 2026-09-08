# Cross-Object Proximity — a contact signal for grime, dust and wear

**Status:** PROPOSED 2026-09-08, revised the same day after one adversarial
round (two reviewers, 10 P1s: the SDF exactness claim, the bbox cost, the VM
channel, the Box exemption, the query-memo key, a floating scene-D asset, an
unpinned test F, lights, unmeasurable gates); driven by the scene set in §1;
implementation phased in §8. **Predecessor:** cross-object *ambient occlusion*, re-measured and
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
| D | `scenes/Tests/Signals/proximity_mesh_contact.RISEscene` (new) | plane ← `models/risemesh/bunny.risemesh` placed so its lowest vertex touches the plane; a second mesh (`dragon_small`) resting on the bunny; the plane also under a `casts_shadows FALSE` sphere and a `rect_light` panel 2 mm above it | The mesh-neighbour path (Phase 2) on real assets, at real contact. Showroom was the first choice and is REJECTED: its torus knot floats 0.69 m above its platform (mesh Y-range 0.989–2.005 m under `scale 0.008`, platform top 0.3 m), so no authorable radius reaches it. The light panel proves lights never count (§2). |
| E | `scenes/FeatureBased/Geometry/sponza_new.RISEscene` (asset at `/Users/aravind/Working/Assets/main_sponza/...`) | 405 mesh objects, mesh ← mesh everywhere | Cost with the query forced at every hit (the §8.1 protocol), candidate-scan scaling at 405 objects, and a beauty check: dust where walls meet floor, the look production renderers get from Unreal's `DistanceToNearestSurface`. |
| F | `tests/ProximityInvalidationTest` (no scene file) | a neighbour moved through `Cst::DeriveToJobIncremental` between two renders, and a neighbour keyframed across `RasterizeAnimation` frames | END-TO-END: the next render (and the next animation frame) sees the move with no bake to invalidate — the property the AO bake could not have (O(scene)). It does NOT isolate the move from the pass-entry generation bump, and does not claim to: the unit-level pin that no stale entry survives a pass boundary is `ExpressionMemoTest`'s generation red-proof, and the pin that the query memo clears is the new (n) row there. Per-sample motion blur is NOT exercised by any scene in this set and is a stated residual (§10). |

A and B are production showcases and get the signal composed into their
materials (the plank's dirt term at `proximity(0.004)` — 4 mm, the shank's base
radius, against the header's 0.40 mm p50 pixel footprint; the bench's
"deliberately NOT painted on" flange grime at `proximity(0.02)`). C, D and F are
tests; D doubles as the Phase-2 acceptance render. E is the cost ceiling and a
beauty check. Not in the set, stated so nobody infers coverage: a CSG-composite
receiver in a showcase (Phase 3 will take `pt_alchemists_sanctum` or
`glass_pavilion`), heightfield-mode SDFs (a C fixture refuses, that is all), and
per-sample motion blur. The Sponza asset path is this machine's; the tracked
scene points at a Windows path and must be copied with `file` rewritten.

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
- **Lights never count.** `rect_light` / `shape_light` fixtures derive to
  ordinary world-visible objects, and a panel parked 2 mm off a wall must not
  paint grime. An object whose material emits (`GetMaterial()->GetEmitter()`)
  is skipped. Unreal's distance-field set and Blender's proximity target contain
  no lights either. Scene D's panel pins it.
- **CSG operands are excluded by `IsWorldVisible()`** — they ARE registered in
  the manager's map (the snapshot-clone path clones them on their own account),
  and only the composite is world-visible; the scan filters exactly as
  `IntersectOcclusionRay` does.
- **A neighbour that cannot answer contributes nothing** (it is treated as far),
  and says so once in the log. Honest absence over a wrong distance.
- **Direction of error, where a family cannot be exact: never over-read
  contact.** A reported distance is an UPPER bound on the true one (or exact),
  so `proximity` may under-paint a seam but never paints one that is not there.
  §5.2 says which families are exact and which are bounded, and by how much.
- **A non-finite hit point refuses** (reads 0), as `RadiusUsable` refuses a
  non-finite radius.

---

## 3. Prior art

- **Unreal** `DistanceToNearestSurface` material node: world-space distance to the
  nearest surface sampled from the global signed distance field at a world
  position; the canonical use is contact effects (shore foam, dirt at wall/floor
  seams). Self-exclusion there is the material author's problem (the field
  includes the pixel's own mesh); ours is by object identity. Our closest
  analogue in intent and unit.
- **Houdini** `xyzdist` / `ray` SOP: closest point on another geometry, world
  units; the artist composes `1 − dist/r` masks by hand.
- **Blender** Geometry Nodes `Geometry Proximity`: closest point on a target
  geometry (not the whole scene), world units.
- **Substance** has no cross-object proximity; its Thickness baker is self-only
  (its Position baker encodes raw 3D position, unrelated), and its AO baker's
  "by mesh name" is the only cross-mesh channel — an occlusion, which §8.1 showed
  is the wrong measure for contact.
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
  prototype's 2.38× toward ~1.1× (an ESTIMATE from the 14.28× redundancy, not a
  measurement).
- **Invalidation of any bake is O(scene)** (Sponza median 33 neighbours per move,
  worst 93 %, and infinite planes invisible to a bbox census). **A live query has
  nothing to invalidate.** With the memo's generation already bumped at every
  scene-change seam, a moved neighbour is seen by the very next pass.
- **Ray fans cannot estimate contact distance** for the same grazing-geometry
  reason AO fails; only true distance functions can. That is what forces the
  per-family closest-point work in §5 rather than reusing the shadow-ray path.

---

## 5. The chosen design

### 5.1 Where the scene enters — the record's existing signal channel, not a global

The AO prototype published the object manager as a process global and called it
"not the design". The design is the codebase's own rule: **every per-hit quantity
a painter needs is stamped on the intersection record** — and the record already
has the typed, `const`, memoised, fallback-owning channel for signal queries:
`SurfaceSignalInfo`. The VM does not read `signals` from `ExprEvalContext`; it is
threaded to `RunAny` / `CallFunc` as a separate `const SurfaceSignalInfo*`, which
is why a *second* channel would mean a fifth parameter on `CallFunc` and `RunAny`
and a pass-through in `Eval` / `EvalVec3` — the surface the memo's FP contract
pins. So `SurfaceSignalInfo` gains three fields:

```cpp
const IObjectManager* pScene;    // the manager that found this hit; 0 = none
const IObject*        pSelf;     // == ri.pObject, the object the hit belongs to
Point3                ptWorld;   // the hit in world space (== ri.ptIntersection)
```

all defaulted to null/zero in the constructor, enumerated in the record's
hand-written copy constructor and `operator=`, **not** in `PropagateCastInputs`
(inputs only), and explicitly **declined** in `AdoptCsgSurfacePayload` (the stamp
happens above CSG resolution and names the composite). They are stamped in one
place, `ObjectManager::IntersectRay( RayIntersection&, … )`, after traversal, on
the winning record, with `pSelf` copied from `ri.pObject` so the two identities
can never disagree (`Object::IntersectRay` and `CSGObject::IntersectRay` both set
`pObject` to themselves; a CSG hit therefore names the composite, and operands —
never reached by the manager — can never be `pSelf`). The per-candidate `myRI`
copy-back happens inside traversal and is unaffected. Every path (BVH4, octree,
linear) converges there.

BDPT/VCM/MLT records rebuilt by `PathVertexEval::PopulateRIGFromVertex` carry the
defaults and read the neutral 0. **This declines `PathVertexEval.h`'s written
contract** (a new consumed field must gain a `BDPTVertex` slot, population in
both subpath generators, a copy in `PopulateRIGFromVertex` and a sentinel in
`BDPTVertexRIGRebuildTest`) for the same reason the signals design §14 item 11
declined it for the three existing signals: the gap is already disclosed, the
fix is the same widening for all four, and widening for one would leave a
mixed-truth state. The new fields are added to that contract's list as declined.

The three fields enter the L2 memo key as part of the `signals` block
(`SignalHitKey::kFields` 11 → 16: two pointers and three scalars;
`ProgramKey::kFields` 29 → 34). **That moves memo eligibility** — bodies of 29–33
instructions stop qualifying under `ComputeMemoWorthiness`'s
`instrs >= kFields` rule — a perf-only, bit-identical change, disclosed in the
commit and in §6.5 of the convexity doc, not treated as bookkeeping.

The builtin is `proximity(r)`, id `kFnProximity` past `kFnConvexityDynR`, with
**no `DynR` twin** (there is no bake, so a computed radius is fine). `CallFunc`
gains exactly one `case`, which calls `SurfaceSignalInfo::Proximity(r)`. This is
the one deliberate move of the byte-identity contract on `CallFunc`; the commit
says so, `ExpressionMemo.h`'s header is updated to pin the new bodies, and
`TextureExpressionVMTest` (846) plus `ExpressionMemoTest`'s bit-equality rows are
the gate. `SignalKey.fn` gains the value 3 so the existing **L1 memo** covers the
query with no new level: the plank's colour, roughness and relief stencil calls at
one hit share one entry, exactly as `occlusion` does today. Its key gains `time`
(see §5.4).

The query itself is one tail-appended `const` method on `IObjectManager`, the
convention that interface already uses (`IntersectOcclusionRay`, 2026-09-07):

```cpp
virtual bool NearestOtherSurface( const Point3& ptWorld, const IObject* self,
                                  Scalar maxDistWorld, Scalar& outDist ) const;
```

It is read-only against the scene the pass was prepared with, evaluates no
painter, casts no ray, and is safe from every render thread by the same argument
as `IntersectShadowRay`. One qualification, inherited from ARCHITECTURE.md
§"Animation / Temporal Sampling": under per-sample motion blur `EvaluateAtTime`
mutates other objects' transforms from worker threads — a **pre-existing** race
that `Object::IntersectRay` already reads through; the query reads the same
matrices and joins that race, no wider.

### 5.2 The query — exact or upper-bounded distances per family, no rays

`NearestOtherSurface` walks the candidate set — objects whose **prepared world
bbox** expanded by `maxDist` contains the point, skipping `self`, world-invisible
objects and emitters — asks each for its distance in its own object space,
converts to world length, and keeps the minimum. Two new virtuals with refusing
defaults (every concrete geometry derives from `Geometry`; the only direct
`IObject` implementers outside `Object`/`CSGObject` are two test stubs, which a
defaulted method leaves compiling):

- `IGeometry::DistanceToSurface( ptObject, maxDistObject, outDist ) → bool`
- `IObject::DistanceToSurface( ptWorld, maxDistWorld, outDist ) → bool`

**The candidate scan reads cached world AABBs.** `Object::getBoundingBox()`
transforms eight corners through the world matrix on every call (tens of
nanoseconds, plus a virtual), so a naive scan of Sponza's 405 objects is
20–40 µs per query, not the ~1 µs a cached scan costs — at §8.1's ~27 M hits per
Sponza frame that is the difference between +7 % and 3×. So
`ObjectManager::PrepareForRendering` (which already computes every world AABB to
build the TLAS) keeps them in a flat array the scan reads; the TLAS point query
(walk `nodes4[]` for boxes containing the point) is the Phase-2 upgrade if the
flat scan measures above budget on E. Infinite-bbox objects (planes: `±RISE_INFINITY`
corners, some axes overflowing to ±inf after the transform) are **unconditional
candidates** — §8.1's warning that a bbox census cannot see them is met by
admitting them always, which is correct since a plane is within `r` of every
point near it and its closed form answers exactly.

**Transform.** The point goes to object space through `GetFinalInverseTransformMatrix()`,
the radius through the inverse of the cached `|det|^(1/3)` length scale, the
answer back through the length scale. Exact for rotations, reflections and
uniform scale. For a general linear map `M` a point-to-surface distance
satisfies `σ_min·d_o ≤ d_w ≤ σ_max·d_o` (the map stretches every displacement
by a factor in `[σ_min, σ_max]`, and the nearest surface point in one space
bounds the other's), so v1 reports **`σ_max·d_o` — the upper bound** — for
non-uniform scale or shear (error factor ≤ `σ_max/σ_min`, disclosed once in the
log), never the geometric mean, which could over-read contact. Boxes get no
exemption: `BoxGeometry` stores object-space extents like every other family;
the workbench's boxes are exact because their transforms are translation-only.

**Per family — and the honest exactness of each:**

| Family | Distance | Exactness | Phase |
|---|---|---|---|
| Infinite plane, sphere, box, capped and open cylinder, disk, clipped plane, torus | closed forms in the geometry's own parameterisation | exact | 1 |
| Ellipsoid | scaled-sphere bound | upper bound, ≤ ratio of semi-axes | 1 |
| SDF / skeleton | see below | **upper bound**, gap measured in C | 1 |
| Indexed triangle mesh (every loader but RAW; tessellated primitives; `displaced_geometry`'s internal mesh) | closest point on the mesh's own BVH: a bounded-radius traversal ordered by AABB distance, pruning past the running best, point–triangle distance at the leaves | exact (identical to brute force under the same point–triangle formula; node boxes are conservative `float`, sound for pruning) | 2 |
| Non-indexed mesh (RAW) | refuses (no BVH) | — | — |
| CSG composite | refuses in v1 (its surface is not the min of its operands' under subtraction/intersection); union-min with the exact boundary test is Phase 3 | — | 3 |
| Patches, hair | refuse | — | — |

**The SDF family is NOT exact, and the flagship scene is the counter-example.**
`SDFGeometry::Map` is only 1-Lipschitz: `partEval` scales each primitive's unit
field by the part's conservative `minScale`, and `smin`/`smax` composition
(`subtract`, `intersect`, and any `smin` with `k > 0`) yields a field that may
report **less** than the true distance — the plank is `roundbox union` +
`box subtract`, the nail `roundcone union` + `box intersect` + `cylinder smin`,
and the plank header's own measurement is a 6.7× under-read for a part scaled
`(0.15, 1, 1)` *inside* object space (a per-part scale, nothing to do with the
object transform). Reporting the field directly would over-read contact by an
unbounded factor. So an SDF neighbour answers with a **projected-point upper
bound**: from the query point `p`, iterate `p ← p − Map(p)·∇Map(p)` (central
differences, the same gradient the intersector uses) until `|Map(p)| < ε` or a
small step budget is spent; a point on the zero set is a point on the surface,
so `|p − p_surface|` is ≥ the true distance and is reported. The field's own
value is the matching **lower bound**, used only to skip a candidate early
(`Map(p) > maxDist` ⇒ farther than `r`, no projection needed) and to certify the
answer in tests. On fields that are exact outside (a single `union` part at
uniform part scale, e.g. one sphere) the two bounds coincide and scene C asserts
that to 1e-9; on composed fields C reports the gap against a grid-search
reference and asserts `lower ≤ reported ≤ reference + tolerance`. Heightfield
mode refuses, as for the other signals.

### 5.3 Cost, and why it is bounded

Per hit, once: the L1 memo makes every painter's `proximity(r)` call at one hit
share one entry (the plank's colour, roughness and four relief taps), and the L2
memo makes each painter's whole evaluation once per hit — the same two levels,
with the measured 96.2 % / 82.7 % hit rates on plank; the L1 rate for the new
query is a Phase-1 gate, not an assumption, because it is what keeps the
candidate scan at ~one per hit rather than the ~2.4 that L2's misses alone would
give. Per query: one cached-AABB scan over N objects (405 on Sponza), one
closed-form or field evaluation per candidate (SDF: ≤ 8 field evaluations for
the projection), mesh `O(log n)` expected. Deterministic, no sampling, no noise.
TLS budget: the L1 `SignalKey` grows by five fields (~40 B × 4 ways) and the L2
`ProgramKey` by the same; `ExpressionMemoTest` (g)'s 2048-byte ceiling holds
(1440 → ~1760 B per worker) and the test asserts it.

### 5.4 Invalidation

**None to build.** The query is live against the scene the pass was prepared
with. What makes a moved neighbour visible to the next render is the
**unconditional generation bump at every render-pass entry**
(`PixelBasedRasterizerHelper`'s three entries: "whatever moved since the last
one … is covered here even if it travelled through a seam the enumerated
mutation sites do not name") — not `DeriveToJobIncremental`, which commits the
document and returns without preparing anything. The agent's `quality:"draft"`
preview runs `InteractivePelRasterizer` through the same helper and shades
materials, so it pays the full query; Phase 1 measures it.

**Per-sample motion blur is where this signal's failure mode differs from the
other three.** For `occlusion`/`convexity`/`thickness` anything that moves the
answer moves the receiver and hence the key. For `proximity` a *neighbour* can
move while the receiver's world point is unchanged. The query's L1 key therefore
carries `time` (the painter's `m_time`, the same field L2 keys on), which covers
keyframed painters; a non-keyframed painter's `time` is constant, and for it the
argument is the family's: every temporal sample carries its own sub-pixel jitter,
so a bit-identical receiver point across two samples essentially does not occur.
Stated as a residual in §10; no scene in §1 exercises it.

### 5.5 What does not change

The existing `occlusion` / `thickness` / `convexity` semantics, radius unit and
providers are untouched; `proximity` is a new builtin with its own explicit unit,
never a widening. `Eval`, `EvalVec3`, `RunAny` and `CallFuncVec3` stay
byte-identical; `CallFunc` gains one `case` and its new body becomes the pinned
one.

---

## 6. Engine principles, checked

- **Scene immutable after Prepare:** the query is `const`, reads prepared
  geometry, transforms and cached AABBs, builds nothing lazily. No `mutable`, no
  locks. The one pre-existing race (per-sample `EvaluateAtTime`) is inherited,
  not added.
- **Painters as pure functions of the hit and the scene:** the scene reaches the
  painter through the record's existing signal channel, typed and `const`; the
  query evaluates no painter and casts no ray, so there is no reentrancy and no
  new control-flow shape.
- **Neutral fallback** 0 everywhere the channel is null, the point or radius is
  unusable, or every neighbour refuses.
- **Explicit radius unit:** world length, in the descriptor, the skill and every
  sentence that mentions it.
- **Named managers / refcounting untouched:** non-owning back-pointers with the
  lifetime argument `SurfaceSignalInfo` already makes (a manager and an object
  outlive every record they stamp).
- **Thread safety of the memo:** the query rides the existing `thread_local`
  tables (trivially constructible and destructible, `static_assert`ed); nothing
  new is shared.
- **PT-first; BDPT/VCM/MLT residual** identical in extent to the other signals.

---

## 7. Adoption surface

`proximity(r)` in the expression descriptor and the `procedural-textures` skill
beside `occlusion`/`convexity`, with one worked example (the nail's contact line
at `proximity(0.004)`) and the unit stated in every sentence that mentions the
radius; scene A and B headers updated to say the term is now real. `add_wear` is
not extended in v1 (it composes per-object signals; a cross-object term needs
the author to choose the radius from the scene's feature sizes).

---

## 8. Phases and gates

**Phase 1 — the query, the record channel, the builtin, the cached AABBs, SDF +
analytic families, scenes A/B/C, tests C and F.** Gate, all numeric:

- warning-free build; `ProximitySignalTest` closed forms within 1e-9 on C's exact
  fixtures, and `lower ≤ reported ≤ reference + 1e-6·r` on its composed-SDF
  fixture; `ExpressionMemoTest` extended (five new key fields separate,
  red-proved; the query memo clears on a bump; TLS ≤ 2048 B);
  `TextureExpressionVMTest` 846/846; test F green;
- **plank_closeup, probe protocol from §8.1** (albedo = raw signal ÷ white
  control, 16 spp, relief modifiers stripped): `proximity(0.004)` on the plank's
  top face ≥ 0.75 within 1 px (0.4 mm) of the shank silhouette, ≤ 0.10 at ≥ 10 px
  (4 mm), 0.00 on the open plank; the beauty crop is judged after the numbers,
  not instead of them; cost ≤ 1.15 × 16.33 s;
- **weathered_workbench**: `proximity(0.02)` on the bench top ≥ 0.5 within 5 mm of
  the flange edge and 0 beyond 20 mm; cost ≤ 1.10 × its memo baseline;
- L1 hit rate for `proximity` on plank ≥ 80 % (temporary counter, removed).

**Phase 2 — mesh closest point on the BVH (indexed meshes, displaced); the TLAS
point query if E's scan measures above budget.** Gate: closest point identical to
brute force over every triangle on ≥ 10⁵ random points (same point–triangle
formula) and within 1e-9 of the closed form on a tessellated sphere; scene D's
plane reads ≥ 0.5 within 5 mm of the bunny's footprint at `proximity(0.02)` and
0 under the light panel; Sponza with the query forced at every hit ≤ 1.25 × its
baseline and the floor within 2 cm of a wall ≥ 0.5 at `proximity(0.04)`.

**Phase 3 — observed-need:** CSG union-min with the exact boundary test (and a
CSG showcase beauty check on `pt_alchemists_sanctum` or `glass_pavilion`);
anisotropic transforms exactly; a signed variant; the `add_wear` composition.

Each phase runs the implementation-review-loop to zero P1 before merge.

---

## 9. Test plan

- `tests/ProximitySignalTest.cpp` on scene C: closed forms per exact family;
  the composed-SDF bound; self-exclusion; the instanced copy counts (named);
  `casts_shadows FALSE` counts; an emitter does not; CSG operands never count
  and the composite refuses (v1); heightfield SDF refuses; the non-uniform-scale
  upper bound (`σ_max`); radius cut-off exactly at `r`; a neighbour BELOW the
  receiver reads the same as one above (unsigned); neutral 0 with a null channel
  and with a non-finite point; the builtin end-to-end through an
  `ExpressionPainter` at a real hit.
- `tests/ExpressionMemoTest.cpp`: five new `SignalHitKey` fields separate in
  both L1 and L2 rows; `time` in the query key; the generation clears the query
  memo (red-proved); the TLS ceiling.
- `tests/ProximityInvalidationTest.cpp` (F): derive, render a probe pixel, move
  the neighbour through `DeriveToJobIncremental`, render again — the value
  changes; move it beyond `r` — it reads 0; a keyframed neighbour across
  `RasterizeAnimation` frames — each frame's value matches a fresh evaluation.
- Phase 2: `MeshClosestPointTest` differential against brute force; scene D's
  probe values.

---

## 10. Residuals, stated up front

- BDPT/VCM/MLT rebuilt records read 0 (the family's disclosed gap; the
  `PathVertexEval` contract is explicitly declined for the same reason as for
  the other three signals).
- Refusing families (RAW meshes, patches, hair, CSG composites in v1) read far.
- SDF neighbours are an upper bound on distance (never over-read contact); the
  gap is measured on C, not bounded analytically.
- Non-uniform scale and ellipsoids are upper-bounded, not exact, in v1.
- Per-sample motion blur: a neighbour moving under a non-keyframed painter can
  serve a stale query-memo entry only at a bit-identical receiver point, which
  sub-pixel jitter makes essentially unreachable; no scene in §1 exercises it.
- The candidate scan is linear in object count over cached AABBs; Sponza (405)
  is the measured ceiling, the TLAS point query the upgrade.
- The signal is unsigned: it cannot distinguish "just above the floor" from
  "just below it"; a signed variant needs an inside test per family (Phase 3).
- The memo-eligibility threshold moves with `kFields` (29 → 34); bit-identical,
  perf-only, disclosed.
