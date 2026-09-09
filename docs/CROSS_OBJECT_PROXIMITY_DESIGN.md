# Cross-Object Proximity — a contact signal for grime, dust and wear

**Status:** PHASE 1 SHIPPED — wave 1 (the engine) and wave 2 (the two
showcases, every Phase-1 gate measured) both landed 2026-09-08; §8.1 and §8.2
carry what each wave built and where the code and the measurements corrected
this document.  Accepted after three adversarial rounds
(round 3: the nail rests on its head rim and tip, not its shank — the flagship
gate is re-derived from where contact actually is; the DynR remap tail; the AABB
cache as an immutable snapshot; degenerate transforms refuse; clipped planes are
bilinear patches; round 2: the projected-point SDF bound was NOT a bound — replaced by a
bracketed sign change; `time` as a fourth stamped field; the AABB cache and the
σ-extremes are new work; gates re-derived from the contact geometry; round 1: the SDF exactness claim, the bbox cost, the VM
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
| C | `scenes/Tests/Signals/proximity_closed_forms.RISEscene` (new) | plane ← sphere (closed form `sqrt(s²+ρ²) − ρ`), plane ← box, plane ← capped and open cylinder lying flat, plane ← disk, clipped plane, torus, ellipsoid; SDF ← tessellated mesh; a single-sphere SDF (exact field) and a composed `smin`/`subtract` SDF (bounded field); an instanced copy; a `casts_shadows FALSE` neighbour; a hand-authored emissive box; a CSG-subtraction receiver with its operands; a heightfield-mode SDF; a non-uniformly scaled SDF; a receiver point inside a neighbour; a neighbour below the receiver; a neighbour beyond the radius | Drives `ProximitySignalTest`: every Phase-1 family's distance against a closed form or its stated bound, the self-exclusion rule, the visibility rule (casts_shadows does not exempt; emitters never count), the CSG operand rule, the scale rule, interpenetration, the unsigned rule, the radius cut-off, and every neutral. |
| D | `scenes/Tests/Signals/proximity_mesh_contact.RISEscene` (new) | plane ← `models/risemesh/bunny.risemesh` placed so its lowest vertex touches the plane; a second mesh (`dragon_small`) resting on the bunny; the plane also under a `casts_shadows FALSE` sphere and a `rect_light` panel 2 mm above it | The mesh-neighbour path (Phase 2) on real assets, at real contact. Showroom was the first choice and is REJECTED: its torus knot floats 0.69 m above its platform (mesh Y-range 0.989–2.005 m under `scale 0.008`, platform top 0.3 m), so no authorable radius reaches it. The light panel proves lights never count (§2). |
| E | `scenes/FeatureBased/Geometry/sponza_new.RISEscene` (asset at `/Users/aravind/Working/Assets/main_sponza/...`) | 405 mesh objects, mesh ← mesh everywhere | Cost with the query forced at every hit (the §8.1 protocol), candidate-scan scaling at 405 objects, and a beauty check: dust where walls meet floor, the look production renderers get from Unreal's `DistanceToNearestSurface`. |
| F | `tests/ProximityInvalidationTest` (no scene file) | a neighbour moved through `Cst::DeriveToJobIncremental` between two renders, and a neighbour keyframed across `RasterizeAnimation` frames | END-TO-END: the next render (and the next animation frame) sees the move with no bake to invalidate — the property the AO bake could not have (O(scene)). It does NOT isolate the move from the pass-entry generation bump, and does not claim to: the unit-level pin that no stale entry survives a pass boundary is `ExpressionMemoTest`'s generation red-proof, and the pin that the query memo clears is the new (n) row there. Per-sample motion blur is NOT exercised by any scene in this set and is a stated residual (§10). |

A and B are production showcases and get the signal composed into their
materials (the plank's dirt term at `proximity(0.002)` — 2 mm against the
header's 0.40 mm p50 pixel footprint; the bench's "deliberately NOT painted on"
flange grime at `proximity(0.02)`). **Where the nail actually touches the plank
is the point of scene A**, and it is not along the shank: the head disc
(radius 7.5 mm) is thicker than the shank's 4 mm base, so the nail rests on its
head rim and its tip — the scene header says exactly this — and with its 6°
nose-down the shank's underside sits +3.5 mm above the plank at the head end,
≈ +1.5 mm mid-way, and −0.6 mm (buried) at the tip. No pitch fixes that without
burying the head. So the seam a distance signal must draw is a ring under the
head rim, a spot at the tip, and a band that FADES along the lifting shank —
precisely the picture an AO cannot draw and the gate in §8 is derived from. C, D and F are
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
  and says so **once per refusing object** in the log, naming the chunk and its
  geometry kind. Honest absence over a wrong distance. (The latch is
  `IObject::NoteDistanceRefusal`, an `std::atomic<bool>` on `Object` so the hot
  path is one relaxed load; the line is printed by `ObjectManager`, the only
  party that knows the chunk's name.) **The refusal is confirmed at an
  unbounded radius before it is printed**, because a `false` return is not
  only "I cannot answer": an SDF's step-1 lower-bound early-out returns false
  for a neighbour that is merely out of range, which happens at the far corners
  of every SDF's expanded box and is a healthy outcome. For a closed-form or
  never-answering family (§5.2) the confirm costs one extra `O(1)` call per
  object, ever, and the printed sentence is fully true: that family never
  answers. For a non-heightfield **SDF** the confirm is not `O(1)` — it re-runs
  the whole bracket search (descent + doubling probe, ~80 `Map()` calls ×
  `O(parts)`, §5.2) — and it only rules out the radius-dependent false
  positive; it cannot distinguish "this SDF never answers" from "this SDF hit
  a one-off per-point solver failure" (an unclosed bracket, a stalled descent,
  a fabricated `(0,1,0)` gradient at a flat blend seam). The printed sentence
  is therefore **best-effort for SDFs**: it says the query could not be
  answered at that one point even unbounded, not that the object belongs to a
  family that never answers — and because the latch is one-shot, an SDF's
  first refusal (very often exactly this kind of per-point failure) spends it,
  so a later genuine family-level refusal on the same object stays silent.
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
- **Houdini** `xyzdist()`: closest point on another geometry, world units (the
  Ray SOP is a directed projection, a different tool); the artist composes
  `1 − dist/r` masks by hand.
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
  prototype's 2.38× toward ~1.1× (an ESTIMATE: `1 + (2.38 − 1)/14.28 = 1.10`,
  not a measurement).
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
threaded to `RunAny` / `CallFunc` as a separate `const SurfaceSignalInfo*`
(`pSignals == &ctx.signals`), which is why a *second* channel would mean a fifth
parameter on `CallFunc` and `RunAny` and a pass-through in `Eval` / `EvalVec3` —
the surface the memo's FP contract pins. So `SurfaceSignalInfo` gains **four**
fields:

```cpp
const IObjectManager* pScene;    // the manager that found this hit; 0 = none
const IObject*        pSelf;     // == ri.pObject, the object the hit belongs to
Point3                ptWorld;   // the hit in world space (== ri.ptIntersection)
Scalar                time;      // the painter's m_time, see §5.4
```

The first three are stamped in one place, `ObjectManager::IntersectRay(
RayIntersection&, … )`, after traversal, on the winning record, with `pSelf`
copied from `ri.pObject` so the two identities can never disagree
(`Object::IntersectRay` and `CSGObject::IntersectRay` both set `pObject` to
themselves; a CSG hit therefore names the composite, and operands — never reached
by the manager — can never be `pSelf`). All three branches of that function
(BVH4, octree, linear) fall through to one tail, and the per-candidate `myRI`
copy-back happens inside traversal, so the stamp is unaffected. The fourth,
`time`, is stamped by `ExpressionPainter::BuildContext` from its `m_time` (the
manager does not know a painter's time; the VM's `CallFunc` has no time
parameter); `ExpressionScalarPainter::BuildContext` has no `m_time` and stamps
0, so on the roughness pipe the motion-blur mitigation is the jitter argument
alone (§5.4). All four are
zero-initialised in `SurfaceSignalInfo`'s constructor. The record's hand-written
copy constructor and `operator=` copy `signals` as a whole struct and
`AdoptCsgSurfacePayload` assigns it wholesale, so the new fields ride along
without edits; **the invariant that makes the stamp survive to the painter is
that nothing assigns `signals` after `ObjectManager::IntersectRay` returns** —
true today — the write sites are the two geometry intersectors
(`SDFGeometry.cpp`, `TriangleMeshGeometryIndexedSpecializations.h`),
`CSGObject.cpp` (adoption plus three `nObject`/`bComplementedField` flips inside
`CSGObject::IntersectRay`), the record's own `operator=`, and `BuildContext`'s
by-value copy — all at or below the stamp — and pinned by `SourceHygieneTest`
at FILE granularity (its guards are substring finds over flattened bodies
against an exact filename set), which is the granularity it can express. `PropagateCastInputs` (inputs only) does not carry it.

BDPT/VCM/MLT records rebuilt by `PathVertexEval::PopulateRIGFromVertex` carry the
defaults and read the neutral 0. **This declines `PathVertexEval.h`'s written
contract** (a new consumed field must gain a `BDPTVertex` slot, population in
both subpath generators, a copy in `PopulateRIGFromVertex` and a sentinel in
`BDPTVertexRIGRebuildTest`) for the same reason the signals design §14 item 11
declined it for the three existing signals: the gap is already disclosed, the
fix is the same widening for all four, and widening for one would leave a
mixed-truth state. The new fields are added to that contract's list as declined.

**Memo keys.** The four fields enter `SignalHitKey` (11 → 17: two pointers, three
scalars, `time`) and therefore `ProgramKey` (29 → 35) and `SignalKey` (3 + 17 =
20; `time` is unconditional, not gated on the query kind — a conditional field
in a POD compared by `Equals` is not expressible and would hide a correctness
split). `ptWorld` duplicates `P` inside `ProgramKey`; three redundant compares
on every L2 probe, accepted for one key layout rather than an L1-only tail, and
disclosed. `Tables` grows 1440 → 1824 bytes per worker (six 8-byte slots per
`SignalHitKey`, which appears eight times across the two 4-way tables), inside
`ExpressionMemoTest` (g)'s 2048 ceiling, which the test keeps asserting.
**Memo eligibility moves** with `kFields` (bodies of 29–34 instructions with no
signal and no noise call stop qualifying — `ComputeMemoWorthiness` returns true
early for any signal or noise call, so only pure-arithmetic bodies are affected):
perf-only, bit-identical, disclosed in the commit and in §6.5 of the convexity
doc.

**The builtin.** `proximity(r)`, id `kFnProximity = 57` (ids 57–59 are free;
60–62 belong to `CallFuncVec3`), with **no `DynR` twin** (there is no bake, so a
computed radius is fine). `ParseCall`'s `isSignalFn` three-way test gates four
things: the `expression_function2d` refusal, the `m_sigCalls` registration
(which drives `ComputeMemoWorthiness`'s early-out and `SurfaceSignalDemand`),
the literal-radius diagnostic, and the `DynR` remap — whose final ternary arm is
an UNGUARDED fall-through to `kFnConvexityDynR`. Extending `isSignalFn` and
leaving the remap alone would therefore compile every computed-radius
`proximity(...)` as a convexity call; the remap ternary gets an explicit id
guard (one line in `ParseCall`, which is not a VM body). The diagnostic's text hard-codes
"radius … is a FRACTION of the object's own size … not a world length"; for
`proximity` it must say the opposite. `CallFunc` gains exactly one named `case`,
`case kFnProximity:` → `pSignals->Proximity( a[0] )` (the `ParseCall` edits
above are compiler-side). This is the one deliberate move of the byte-identity
contract on `CallFunc`; the commit says so,
`ExpressionMemo.h`'s header is updated to pin the new bodies, and
`TextureExpressionVMTest` (846) plus `ExpressionMemoTest`'s bit-equality rows are
the gate. `Eval`, `EvalVec3`, `RunAny`, `CallFuncVec3` stay byte-identical.

`SurfaceSignalInfo::Proximity(r)` is a **second policy body** beside
`SignalQuery`, not a fourth branch of it: `SignalQuery` is gated on
`pProvider && RadiusUsable(r)`, and scene B's receiver — a `box_geometry` — has
no provider (`pProvider == 0`, which short-circuits to the neutral). `Proximity`
is gated on `pScene && pSelf && RadiusUsable(r) && finite(ptWorld)` instead, and
shares the L1 find/insert helper (one table, `fn = 3`) so the two bodies cannot
drift on memo policy while owning different preconditions.

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

`NearestOtherSurface` walks the candidate set — objects whose **cached world
AABB** expanded by `maxDist` contains the point, skipping `self`, world-invisible
objects (`IsWorldVisible()`, the filter `IntersectOcclusionRay` uses; this is
what excludes CSG operands, which ARE registered in the manager's map) and
emitters (`GetMaterial() && GetMaterial()->GetEmitter()`, the same predicate
`LuminaryManager` uses to decide what NEE samples) — asks each for its distance
in its own object space, converts to world length, and keeps the minimum. Two
new virtuals with refusing defaults (every in-tree Library geometry derives from
`Geometry`; the 3DS Max plugin's two `IGeometry` implementers and the two test
`IObject` stubs are covered by the defaults):

- `IGeometry::DistanceToSurface( ptObject, maxDistObject, outDist ) → bool`
- `IObject::DistanceToSurface( ptWorld, maxDistWorld, outDist ) → bool`

**The world-AABB cache is new work, not a free byproduct.** `PrepareForRendering`
computes no bounding boxes itself: world AABBs are computed inside `CreateBVH()`
/ `CreateOctree()`, which run only when `items.size() > nMaxObjectsPerNode` (4)
and `!pBVH`, so a ≤4-object scene never builds one, an existing TLAS short-
circuits, and `ObjectManager::IntersectRay` has a lazy `CreateBVH()` path for
callers that skipped `PrepareForRendering`. `Object::getBoundingBox()` itself
transforms eight corners through the world matrix on every call (tens of
nanoseconds plus a virtual), so a naive scan of Sponza's 405 objects is 20–40 µs
per query, not ~1 µs — at §8.1's ~27 M hits per Sponza frame the difference
between +7 % and 3×. So Phase 1 adds an **immutable snapshot object** — a flat
`{world AABB, const IObjectPriv*}` array built once and never mutated —
published behind ONE pointer exactly as `pBVH` is: built unconditionally at the
END of `PrepareForRendering` (after the `RebakeHierarchy()` block, whose caller
invalidates at its close, and after the TLAS build — i.e. after the
`CreateBVH()/CreateOctree()` lines, never inside the rebake `if`), released in
`InvalidateSpatialStructure()` beside `pBVH`/`pOctree` under the same
"never during a pass" contract those already carry, and built lazily under
`treeCreationMutex` in `ObjectManager::IntersectRay` when the pointer is null —
on BOTH the tree branch and the ≤4-object linear branch (the existing lazy
`CreateBVH()` sits only on the former; scene C is the latter). A query copies
the pointer once and reads the immutable snapshot lock-free; because the
snapshot is never grown or edited in place there is no reallocation hazard, and
the only race is the pre-existing publish/clear one `pBVH` has, no wider. Infinite planes need no rule: their bbox is
`±RISE_INFINITY` (= `DBL_MAX`, which `IsFiniteDouble` reports finite; under a
translation-only transform it stays `±DBL_MAX`, under rotation some axes
overflow to ±inf) and either box contains every point under ordinary
containment with no NaN, so they are admitted by the same test as everything
else. An implementer must **not** skip non-finite boxes to dodge NaN, or every
plane disappears. The TLAS point query (walk `nodes4[]` for boxes containing the
point) is the Phase-2 upgrade if the flat scan measures above budget on E.

**Transform.** The point goes to object space through `GetFinalInverseTransformMatrix()`.
For an invertible linear map `M` (translations cancel) a point-to-set distance
satisfies `σ_min·d_o ≤ d_w ≤ σ_max·d_o`: `d_w = min_q |M(p−q)|` and
`σ_min|v| ≤ |Mv| ≤ σ_max|v|`, so evaluating at the object-space minimiser gives
the upper bound and bounding the world minimiser from below gives the lower. The
engine caches only `|det|^(1/3)` today (`m_worldLinearScale`) and has no
eigenvalue or SVD code anywhere; Phase 1 adds **σ_max and σ_min bounds** to
`FinalizeTransformations` WITHOUT new eigen-numerics: if the upper 3×3 `M`
satisfies `MᵀM = s²·I` within 1e-9 relative (a rotation, reflection or uniform
scale — every object in scenes A–E) then `σ_max = σ_min = s` **exactly**;
otherwise the sound one-liners `σ_max ≤ ‖M‖_F` and `σ_min ≥ |det| / σ_max²`
(loose, never unsafe; the error factor `σ_max/σ_min` is disclosed once per
object in the log).

**How loose, measured, because the printed factor is a bound on a bound and
reads alarmingly.** On `scale (3, 1, 0.4)` — the worst anisotropy in the
acceptance set — the three numbers are:

| quantity | value |
|---|---:|
| printed factor `σ_max/σ_min` (Frobenius/det pair) | **26.99** |
| the transform's TRUE singular ratio (3 / 0.4) | **7.5** |
| worst over-report actually measured on that object | **7.96×** |
| object-space search-radius inflation `r/σ_min` | **8.47×** |

So the log line bounds an over-report of 7.96× at 26.99×: sound, and roughly
3.4× pessimistic, because `‖M‖_F` counts all three singular values into `σ_max`
and the `|det|/σ_max²` lower bound then inherits that slack twice. The log
therefore names the printed number as a bound on a bound and prints `1/σ_min`
separately, since that one is the **cost** (a wider object-space search), not
the contact error. A degenerate transform (`det == 0`, for which `Matrix4Ops::Inverse`
silently returns its input) makes the object **refuse** as a candidate. The
query converts the radius **into** object space by `r / σ_min` (a candidate
within world `r` has `d_o ≤ d_w/σ_min ≤ r/σ_min`, so this cannot miss one), and
converts the answer **back** by `σ_max` — the upper bound, the safe direction:
over-reading contact means reporting `d` too small, and `σ_max·d_o ≥ d_w`. Boxes get no exemption: `BoxGeometry` stores object-space
extents like every other family; the workbench's boxes are exact because their
transforms are translation-only.

**Per family — and the honest exactness of each:**

| Family | Distance | Exactness | Phase |
|---|---|---|---|
| Infinite plane, sphere, box, capped and open cylinder (`m_bCapped`, axis + `m_dAxisMin/Max`), disk, torus (`sdTorusY`, the engine's own form) | closed forms in the geometry's own parameterisation | exact | 1 |
| Clipped plane | `ClippedPlaneGeometry` stores four ARBITRARY corners — it is a bilinear patch; answers the point-to-quad closed form only when the corners are **coplanar AND convex** (both checked once at construction), else refuses | exact on **CONVEX** planar quads (every `rect_light`), **refuses otherwise** | 1 |
| Ellipsoid | scaled-sphere bound | upper bound, ≤ ratio of semi-axes (a 4:1 ellipsoid at true 2.75 reports 11.0) | 1 |
| SDF / skeleton | bracketed sign change, below | **upper bound**, gap measured in C | 1 |
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
and `SDFGeometry.h`'s own measurement is a 6.7× under-read for a part scaled
`(0.15, 1, 1)` *inside* object space. Every part in scenes A and B is scale
`1 1 1`, so there the per-part mechanism contributes nothing and the under-read
comes from `smin` (≤ k/4: 0.33 mm on the nail, 0.15 mm on the plank) and from
`subtract`/`intersect` — small, but reporting the field directly would still
over-read contact by a factor the design cannot bound in general. **A projected point does not fix it
either**: stopping a gradient descent at `|Map| < ε` on an under-reading field
does not place the point within ε of the surface (the shortfall is `ε/shrink`,
6.7ε on that part, and blend seams flatten the gradient further), so `|p − p_k|`
can be *smaller* than the true distance — the forbidden direction.

What IS rigorous is the field's **sign**: the surface is by definition the zero
set the intersector renders, so any point with `Map ≤ 0` is on or inside the
solid and the segment from `p` (outside, `Map(p) > 0`) to it crosses the
surface. Hence an SDF neighbour answers with a **bracketed inside point**:

1. early-out on the lower bound — `Map_o(p) > maxDist_o ⇒ true d > maxDist`
   (under-read means `Map ≤ d`), skip, with `maxDist_o = r/σ_min` as above so
   no neighbour within `r` is skipped;
2. descend `p ← p − Map(p)·∇̂Map(p)` (normalised central-difference gradient,
   the intersector's own `GradientNormal`; a step of `|Map|` along `−∇̂` cannot
   overshoot a 1-Lipschitz field's zero set, so the iteration is monotone from
   outside) for at most `N_d` iterations;
3. probe past the last descent point along `−∇̂` with a doubling step starting
   at `max(ε, Map(p_k))` until `Map ≤ 0` or the accumulated path exceeds
   `maxDist`; the first inside point `q` found gives the answer `|p − q|`,
   which is ≥ the true distance because the surface lies on the segment;
4. **no crossing within budget or within `maxDist` ⇒ refuse** (this candidate
   contributes nothing) — never the unconverged point's distance, which would
   over-read contact.

The over-report is bounded by the last probe step; C's composed-SDF fixture
measures the gap `reported − reference` against a fine grid search of the same
`{Map ≤ 0}` set (the reference surface is the zero set, not the sphere tracer's
`|Map| ≤ 2·m_eps` acceptance band — say so in the test) and asserts
`lower ≤ reported` and `reported ≤ reference + gap_max` with `gap_max` recorded
as a number, not a claim. On fields exact outside (one `union` sphere at uniform
part scale) the bounds coincide and C asserts 1e-9. Heightfield mode refuses, as
for the other signals. Two more things the implementation must know: the
field's SENSE flips only through `SurfaceSignalInfo::bComplementedField` on
CSG-subtracted operands, which never reach the query in v1 (operands are
world-invisible, composites refuse) — Phase 3's CSG work reopens that; and
`GradientNormal` returns a fabricated `(0,1,0)` when the gradient length is
below 1e-12 (flat blend seams), so descent there walks an arbitrary axis and
may refuse — a feature failure (under-paint), never a wrong answer.

**Cost of the SDF path, stated honestly:** `GradientNormal` is six `Map()` calls
and the step needs a seventh, so each descent iteration is **7 × O(parts)**
evaluations; the probe doubles from `max(ε, Map)` with `ε = m_epsFrac × diagonal`
(sub-millimetre on the nail), so covering `maxDist` takes `log2(maxDist/ε + 1)`
≈ 8 probes; with `N_d = 6` that is ≤ 50 `Map()` per SDF candidate per hit
(2 parts on the plank, 3 on the nail). On plank_closeup that is of the
same order as one `occlusion(0.03)` call today (181.7 field evaluations, per the
code's own count) — paid once per hit through L1, not per painter call.

### 5.3 Cost, and why it is bounded

Per hit, once: the L1 memo makes every painter's `proximity(r)` call at one hit
share one entry (the plank's colour, roughness and four relief taps — the taps
hit by construction, because `ReliefModifier` holds `signals` fixed across its
stencil), and the L2 memo makes each painter's whole evaluation once per hit —
the same two levels, with the measured 96.2 % / 82.7 % hit rates on plank; the
L1 rate for the new query is a Phase-1 gate, not an assumption, because it is
what keeps the candidate scan at ~one per hit rather than the ~2.4 that L2's
misses alone would give. Per query: one cached-AABB scan over N objects (405 on
Sponza), then per candidate a closed form `O(1)`, an SDF ≤ 50 × O(parts) field
evaluations, a mesh `O(log n)` expected. Deterministic, no sampling, no noise.

**A consequence of riding `signals`:** `ReliefModifier` holds `signals` fixed
across its four taps (so a `curv`- or `occlusion`-driven height has zero
gradient by documented design), and `proximity` inherits that — **a
`proximity`-driven height expression produces no relief.** Stated in §10 and
in the skill; the seam is a colour/roughness signal, not a displacement one.

### 5.4 Invalidation

**None to build.** The query is live against the scene the pass was prepared
with. What makes a moved neighbour visible to the next render is the
**unconditional generation bump at every render-pass entry**
(`PixelBasedRasterizerHelper`'s three entries, each ahead of its camera
null-check: "whatever moved since the last one … is covered here even if it
travelled through a seam the enumerated mutation sites do not name") — not
`DeriveToJobIncremental`, which commits the document and returns without
preparing anything — and, per animation frame, `ObjectManager::PrepareForRendering`'s
own unconditional bumps. The agent's `quality:"draft"` preview runs
`InteractivePelRasterizer` through the same helper and shades materials, so it
pays the full query; Phase 1 measures it.

**Per-sample motion blur is where this signal's failure mode differs from the
other three.** For `occlusion`/`convexity`/`thickness` anything that moves the
answer moves the receiver and hence the key. For `proximity` a *neighbour* can
move while the receiver's world point is unchanged. The `time` field (§5.1)
covers keyframed painters; a non-keyframed painter's `time` is constant, and for
it the argument is the family's: every temporal sample carries its own sub-pixel
jitter, so a bit-identical receiver point across two samples essentially does
not occur. Stated as a residual in §10; no scene in §1 exercises it.

### 5.5 What does not change

The existing `occlusion` / `thickness` / `convexity` semantics, radius unit and
providers are untouched; `proximity` is a new builtin with its own explicit unit,
never a widening. `Eval`, `EvalVec3`, `RunAny` and `CallFuncVec3` stay
byte-identical; `CallFunc` gains one named `case` and its new body becomes the
pinned one.

---

## 6. Engine principles, checked

- **Scene immutable after Prepare:** the query is `const` and lock-free over
  prepared geometry, transforms and the AABB snapshot; the snapshot is an
  immutable object published behind one pointer under `treeCreationMutex` at
  prepare time (or lazily on first use), released at the same call sites as
  `pBVH` (there under `treeCreationMutex`, since the builder retires and
  republishes inside that lock), so it
  joins the manager's existing sanctioned set with no new race class. The one
  pre-existing race (per-sample `EvaluateAtTime`) is inherited, not added.
- **Painters as pure functions of the hit and the scene:** the scene reaches the
  painter through the record's existing signal channel, typed and `const`; the
  query evaluates no painter and casts no ray, so there is no reentrancy and no
  new control-flow shape.
- **Neutral fallback** 0 everywhere the channel is null, the point or radius is
  unusable, or every neighbour refuses.
- **Never over-read contact:** every family is exact or an upper bound on
  distance (§5.2), including the transform.
- **Explicit radius unit:** world length, in the descriptor, the skill, the
  parse-time diagnostic and every sentence that mentions it.
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
at `proximity(0.002)`), the unit stated in every sentence that mentions the
radius, and the two facts an author needs: it drives colour and roughness, not
relief; emissive objects do not count. Scene A and B headers updated to say the
term is now real. `add_wear` is not extended in v1 (it composes per-object
signals; a cross-object term needs the author to choose the radius from the
scene's feature sizes).

---

## 8. Phases and gates

Gates are derived from the contact geometry, not from a picture. The local
model for a body of cross-section radius `ρ` resting on a plane is
`d(s) = sqrt(s² + ρ²) − ρ` at lateral offset `s` from the touchdown line, and
`proximity(r) = clamp(1 − d/r, 0, 1)`.

**Phase 1 — the query, the record channel, the builtin, the AABB cache, the
σ-extremes, SDF + analytic families, scenes A/B/C, tests C and F.** Gate:

- warning-free build; `ProximitySignalTest` closed forms within 1e-9 on C's exact
  fixtures (plane, sphere, box, both cylinders, disk, clipped plane, torus), the
  ellipsoid within its stated ratio, and `lower ≤ reported ≤ reference + gap_max`
  on its composed-SDF fixture with `gap_max` reported; `ExpressionMemoTest`
  extended (four new key fields separate in L1 and L2 rows, red-proved; the
  query entry clears on a bump; TLS ≤ 2048 B); `TextureExpressionVMTest`
  846/846; test F green;
- **plank_closeup, `proximity(0.002)`, probe protocol from §8.1** (albedo = raw
  signal ÷ white control, 16 spp, relief modifiers stripped). The nail rests on
  its head rim and its tip (§1), so the stations are: (1) the plank beside the
  **head rim's** touchdown — the head is a 7.5 mm-radius disc, its rim a
  circle; at 1 px (0.4 mm) outside the rim ≥ 0.9, 0 at ≥ 8 mm; (2) the plank
  under the **shank mid-way**, where the underside floats ≈ 1.5 mm: the model
  gives `1 − 1.5/2 = 0.25`, gate 0.1–0.4 — this is the discriminating check,
  because an AO would read the shank's shadow as contact and the distance
  reads the gap; (3) the plank at the **head end of the shank**, floating
  ≈ 3.5 mm: 0; (4) the plank beside the **tip**, buried 0.6 mm: at 1 px ≥ 0.9
  and 0 at ≥ 8 mm (model with `ρ ≈ 1 mm`: 0.96 at 0.4 mm, 0 at 4 mm); 0 on the
  open plank. Phase 1 first measures the three underside gaps with the signal
  itself and records them in the scene header (the header's current prose
  already says rim-and-tip); the scene is NOT re-pitched. Cost ≤ 1.15 × 16.33 s;
- **weathered_workbench, `proximity(0.02)`**: the flange is a `roundbox` with
  half-extents (0.195, 0.021, 0.172) m and 18 mm corner rounding on a 21 mm
  half-height, so its bottom edge is a quarter-circle of `ρ = 18 mm` whose
  touchdown line lies **18 mm inside the nominal outer edge** (at `u = 0.177`
  along the long side, measured from the flange centre; the model breaks down
  within 18 mm of the flange's corners where two roundings compound, so the
  stations sit mid-side). The model gives 0.966 / 0.555 / 0 at 5 / 20 / 33.5 mm
  outward from that line: gate ≥ 0.9 within 5 mm, ≤ 0.6 at 20 mm, 0 beyond
  35 mm; the scene's prose ("390 × 28 × 344 mm, 12 mm arris") disagrees with its
  own part data and is corrected in Phase 1; cost ≤ 1.10 × its memo baseline;
- the draft preview (`InteractivePelRasterizer`) on A: cost measured and stated;
- L1 hit rate for `proximity` on plank ≥ 80 % (temporary counter, removed).

**Every bullet above was measured in wave 2; the results, and where this gate
was wrong, are in §8.2.**

**Phase 2 — mesh closest point on the BVH (indexed meshes, displaced); the TLAS
point query if E's scan measures above budget.** Gate: closest point identical to
brute force over every triangle on ≥ 10⁵ random points (same point–triangle
formula) and within 1e-9 of the closed form on a tessellated sphere; scene D
placed by the test (it loads the mesh, reads `GenerateBoundingBox()`, positions
the object so the lowest vertex touches the plane, and ASSERTS that value within
1e-6 against the number the scene file's header records, so a replaced asset
fails the test rather than silently faking contact) reads ≥ 0.9 within 2 mm of
the bunny's footprint at `proximity(0.02)` and 0 under the light panel; Sponza
with the query forced at every hit ≤ 1.25 × its baseline and the floor within
2 cm of a wall ≥ 0.5 at `proximity(0.04)`.

**Phase 3 — observed-need:** CSG union-min with the exact boundary test (and a
CSG showcase beauty check on `pt_alchemists_sanctum` or `glass_pavilion`);
anisotropic transforms exactly; a signed variant; the `add_wear` composition;
the emissive-decorative-object exclusion (§10) revisited if a scene needs it.

Each phase runs the implementation-review-loop to zero P1 before merge.

### 8.1 What Phase 1 wave 1 actually built, and where the code contradicted this document

Wave 1 shipped the channel, the memo keys, the builtin, the query, the AABB
snapshot, the σ bounds, the analytic + SDF families, scene C,
`ProximitySignalTest` (107 checks), `ProximityInvalidationTest` (25) and four
new `ExpressionMemoTest` rows (161 → 196).  Both proximity counts moved in the
2026-09-08 fix round: `ProximitySignalTest` 104 → 107 (the coplanar-DART
refusal and its convex twin's teeth, plus the heightfield refusal asked of the
object directly with an ordinary-mode SDF as its teeth), and
`ProximityInvalidationTest` 20 → 25 (the demand-gate section (c), and two
checks with teeth on what the moved pose's rendered value actually IS).
Scenes A and B, their probe protocols and EVERY cost measurement §8's Phase-1
gate names are wave 2, and are recorded in §8.2 below — where one station
gate FAILS (the plank's tip, 0.882 against ≥ 0.9) because §8's local model for
it was optimistic, and two of §8's own claims about the flagship's contact
geometry turn out to be wrong. Six places where building it
corrected this document:

- **`r / σ_min` is sound but not separately observable.** §5.2 reasons that
  converting the radius in by the *smallest* singular value is what keeps a
  neighbour within `r` from being missed. It is conservative in the safe
  direction, but it cannot change an outcome: the answer comes back multiplied
  by `σ_max`, so any candidate whose reported distance lands within `r` had
  `d_o ≤ r/σ_max ≤ r/σ_min` and the tighter conversion would have found it
  too; any candidate the tighter conversion would skip reports a distance
  above `r` and the caller drops it regardless. Keep the conversion (it is
  free and it is the right shape if the σ_max out-scaling ever changes), but
  do not claim it prevents a miss anyone can observe. `ProximitySignalTest`
  (d) asserts the observable behaviour instead — an anisotropic neighbour
  between its true distance and its bound is legitimately not painted.
- **Interpenetration needed a code change, not just a test.** §2 and §9 both
  say a point inside a neighbour reads `d = 0` / proximity 1. The obvious
  reading of "unsigned distance" gives the distance to the nearest *face* —
  1.0 at the centre of a 2 × 2 × 2 box. The solid families now clamp their
  signed field at zero rather than taking its absolute value; the signed
  field's own sign is the inside test, so it is free.
- **§5.1's write-site list is one file short.** It names six files that may
  assign `signals`; `ExpressionEval.h`'s `k.signals = ctx.signals.MemoHitKey()`
  is a seventh. It assigns an `ExpressionMemo::SignalHitKey`, not a
  `SurfaceSignalInfo`, and a text scan cannot tell the two apart, so
  `SourceHygieneTest`'s census lists seven and says why.
- **`PathVertexEval.h` had no "declined list" to add to.** §5.1 says to add the
  four fields to one. There was none; the commit creates it, covering
  `derivatives` and the whole of `signals`.
- **The design's file-level assumptions about `override` do not hold for two
  geometries.** `InfinitePlaneGeometry` and `CircularDiskGeometry` mark no
  member `override`, so the new method does not either — adding the first
  would make clang's `-Winconsistent-missing-override` fire on every other
  member of those classes.
- **The AABB snapshot has a lifecycle §5.2 does not describe, and it is not an
  implementation detail.** §5.2 says the snapshot is built at the end of
  `PrepareForRendering`, released in `InvalidateSpatialStructure`, and
  otherwise immutable behind one pointer. Two things were added in `ccec5ff1`
  because that is not sufficient:
  1. **`EnsureBoxSnapshot` rebuilds when the OBJECT COUNT has moved under it**,
     not only when the pointer is null. The reason is specific and small:
     `Job::AddObject` calls `RegisterOrDiag` and returns — it does **not**
     invalidate — and on a scene of ≤ 4 objects there is no TLAS either, so
     nothing else notices. The linear `IntersectRay` loop would RENDER the
     added object while the stale snapshot left it invisible to every
     proximity query: a wrong answer, not a stale one. The count check is what
     makes the added object visible. It exists for **adds**; a removal is
     caught only incidentally, because every removal path in this tree calls
     `InvalidateSpatialStructure`, which drops the snapshot outright. It does
     not catch an add and a removal in the same gap, and does not claim to.
  2. **A superseded snapshot is RETIRED, not freed** (`retiredBoxes`), and the
     whole retired set is freed at the next `InvalidateSpatialStructure` (and
     in the destructor). `EnsureBoxSnapshot` is reachable from `IntersectRay`,
     so a rebuild can happen while other render threads are mid-scan of the
     snapshot being replaced; `delete`ing it there would be a use-after-free,
     strictly worse than the staleness the count check is fixing. Both freeing
     sites already carry the "never during a pass" contract that makes freeing
     safe. **What retiring protects is the ARRAY, not the objects it points
     at** — the entries hold raw, un-addrefed `const IObjectPriv*` — and that
     gap is closed by the removal paths' own invalidate, not by retiring.

  Two consequences worth stating with them. The snapshot **extends the "never
  invalidate during a pass" contract to scenes of four objects or fewer**,
  where it had been vacuous (no TLAS is built there, so nothing depended on
  it); that is a real widening, not "no race class `pBVH` does not already
  have". And the pointer is a `std::atomic` with a release store and acquire
  loads, because "published whole" has to be true at the memory model and not
  only in prose.

Measured, for the record: `gap_max` on scene C's composed 1-Lipschitz SDF is
**0.0155** (reported 2.0787, grid reference 2.0632, `Map` lower bound 1.9594),
asserted at 0.05; the exact-field SDF over-reports by **0**; the ellipsoid's
bound is 4 against a true 2, exactly its semi-axis ratio; TLS per worker is
**1824 bytes** against the 2048 ceiling, exactly as §5.1 predicted.

### 8.2 Wave 2 — the two showcases, the measured gates, and what the gate got wrong

Wave 2 composed the signal into scenes A and B, measured every Phase-1 gate
§8 names, and updated the adoption surfaces. **Two instruments** are used
throughout and both are reported, because on these two cameras they do not
agree and the reason is not a bug:

- **The query itself** — a throwaway harness linked against `librise.a` that
  loads the real scene, calls `PrepareForRendering()`, and asks
  `IObjectManager::NearestOtherSurface( p, receiver, r, d )` from points on the
  receiver's top plane. Exact, noise-free, no pixel footprint. Deleted after
  the measurement; the method is recorded in each scene's header so it is
  reproducible.
- **The §8.1 probe render** — albedo = the raw signal, divided by a
  `clamp(N.y,0,1)` white control rendered identically, relief modifiers
  stripped, EXR in `Rec709RGB_Linear`, every other object a black Lambertian.

**The probe render cannot resolve either gate at the scene's own camera, and
the numbers say by how much.** On `plank_closeup` the seam that must read
≥ 0.9 is a patch 1.7 × 1.6 mm against a 0.40 mm pixel, and most of it lies
inside the nail's own silhouette. On `weathered_workbench` the camera sits
**11° above the bench top**, so **one shipped pixel spans 31.4 mm along the
view direction** — wider than the entire 20 mm ramp the gate measures. Both
were therefore re-rendered at 4× linear resolution (2560 × 1920) to show the
rendered value converging on the query's as the footprint shrinks; at 4× the
bench pixel is still 7.9 mm along the view. **The gates below are adjudicated
on the query**, with the render given as corroboration. Also worth recording:
the first probe pass ran at the protocol's 16 spp and produced ratios **above
1** at silhouette pixels — impossible for a `[0,1]` signal. The cause is that
each render seeds its RNG from the wall clock, so signal and control carry
independent noise, and the ratio of two noisy small numbers is unstable where
the control is dark. At 512 spp every ratio lands in range. **A 16-spp
divide-by-control probe is noise-limited exactly where a contact seam lives.**

#### A. `plank_closeup`, `proximity(0.002)`

First, **where the nail actually touches**, measured with the query (0.5 mm
grid, refined to 0.1 mm) and now recorded in the scene header. §1's model is
confirmed and sharpened in two places:

| | measured |
|---|---|
| head rim, closest approach | **0.146 mm at (x, z) = (147.4, −16.7) mm — it does NOT touch.** The rim is a 7.5 mm disc tilted ~2° off vertical, so the tangency is shallow and the near-contact patch is broad: `d ≤ 0.20 mm` over 1.7 × 1.6 mm, `d ≤ 0.40 mm` over 4.5 × 4.2 mm |
| shank underside, head end (x = 152.5 mm) | 3.506 mm |
| shank underside (x = 167.5 mm) | 2.186 mm |
| shank underside, mid-way (x = 181.0 mm) | 1.307 mm |
| shank underside (x = 194.0 mm) | 0.445 mm |
| shank reaches the board (x = 201.0 mm) | 0.000 mm |
| tip | **buried, and a FURROW not a spot**: the last ~9.5 mm of shank lies below y = 0 (footprint x 200.8…210.3 mm, z −45.2…−40.5 mm, ~2.2 mm wide), deepest between 0.50 and 0.60 mm |

§1 predicted +3.5 / ≈+1.5 / −0.6 mm and "a ring under the head rim, a spot at
the tip". The gaps are right. **The two shape claims are not**: the head rim
floats 0.146 mm rather than touching, and the tip's contact is a 9.5 mm furrow,
not a spot. The scene was NOT re-pitched.

Stations, `proximity(0.002)`, pixel coordinates in the shipped 640 × 480 frame:

| # | station | world (mm) | px | query | render 640 (16 spp) | render 2560 (256 spp) | gate | verdict |
|---|---|---|---|---|---|---|---|---|
| 1 | plank 0.4 mm outside the head-rim touchdown | (147.6, 0, −16.3) | (321, 212) | **0.918** | 0.82 | 0.94 | ≥ 0.9 | **PASS** |
| 2 | plank under the shank at the head end | (152.5, 0, −20.5) | (334, 212) | **0.000** | 0.000 | 0.000 | 0 | **PASS** |
| 3 | plank under the shank mid-way | (181.0, 0, −32.0) | (392, 220) | **0.347** | 0.32 | 0.34 | 0.1–0.4 | **PASS** |
| 4 | plank 0.4 mm beside the tip furrow | (205.0, 0, −41.2) | (442, 228) | **0.882** | 0.75 | 0.84 | ≥ 0.9 | **FAIL** |
| 5 | 8 mm out from the head touchdown | (147.5, 0, −8.7) | (318, 219) | **0.000** | 0.000 | 0.000 | 0 | **PASS** |
| 6 | 8 mm beside the tip | (205.0, 0, −33.6) | (434, 234) | **0.000** | 0.000 | 0.000 | 0 | **PASS** |
| 7 | open plank, mid-board | (100, 0, −30) | (272, 179) | **0.000** | 0.000 | 0.000 | 0 | **PASS** |
| 8 | open plank, near corner | (90, 0, 20) | (195, 205) | **0.000** | 0.000 | 0.000 | 0 | **PASS** |

**Station 4 FAILS at 0.882 against ≥ 0.9, and the radius was NOT tuned to
rescue it.** §8's model for the tip assumed `ρ ≈ 1 mm` and predicted 0.96 at
0.4 mm. The real geometry is a rod of ρ ≈ 1.2 mm sunk 0.55 mm, so where it
crosses the plank the surface is steep and the distance grows at ≈ 0.59 mm per
mm of lateral offset: 0.235 mm at 0.4 mm out. At 0.2 mm out the reading is
0.949. The gate misses by 0.018 because the model's `ρ` was optimistic, not
because the signal is wrong — and the honest fix, if one is wanted, is a 2.5 mm
radius or a nail that sits 0.2 mm higher, neither of which is a Phase-1 change.

**Cost**, interleaved, warmed sessions, 640 × 480 × 48, **every run listed
including the contaminated ones**. Two baselines: a copy of the shipped scene
with the `proximity` term and its roughness bridge removed ("no-prox"), and the
untouched pre-wave-2 file ("pristine").

*Batch 1 — quiet machine (`pgrep` clean at batch start), no-prox control:*

| run | 1 | 2 | 3 | 4 | mean |
|---|---:|---:|---:|---:|---:|
| no-prox | 18.176 | 17.654 | 17.622 | 18.930 | **18.096 s** |
| shipped | 20.454 | 19.812 | 20.086 | 21.133 | **20.371 s** |

**Ratio 1.126×.** Per-pair 1.125 / 1.122 / 1.140 / 1.116.

*Batch 2 — under a concurrent agent's link; both halves inflated ~45 %:*
pristine 25.196 / 25.526 / 27.772 = 26.16 s; shipped 29.373 / 28.953 / 28.884
= 29.07 s. **Ratio 1.111** — the interleaving protocol doing its job.

*Batch 3 — quiet, all three arms. The first triple is the machine still
settling and is listed but not averaged:*

| triple | pristine | no-prox | shipped |
|---|---:|---:|---:|
| 1 (settling) | 24.581 | 24.924 | 25.528 |
| 2 | 17.758 | 17.348 | 19.622 |
| 3 | 16.763 | 17.065 | 19.296 |
| mean (2, 3) | **17.261 s** | **17.207 s** | **19.459 s** |

**Ratio 1.127× against pristine, 1.131× against no-prox — PASS against the
≤ 1.15 gate**, and the two baselines agree within 0.3 %, so the no-prox copy's
extra zero-weighted `add` node costs nothing measurable here. Across all three
batches the ratio sits in **1.111–1.131**.

**One caveat on the absolute form of the gate.** §8 writes it as "≤ 1.15 ×
16.33 s", the memo baseline from
[OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md](OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md)
§6.5. The same scene's own pre-wave-2 file measures **16.763–17.758 s** on a
quiet machine today (best run 2.6 % above 16.33 s, mean 5.7 % above), and
18.096 s in batch 1. Against 16.33 s literally the shipped scene is 1.18–1.25×
depending on the batch, and the gate FAILS in its absolute form; against a
control rendered in the same interleaved session it is 1.111–1.131× and passes.
The same-session ratio is the trustworthy number — it is what §8.1's own cost
table used, and batch 2 shows why: a 45 % machine-state swing moved the ratio
by 0.02.

**L1 hit rate**: a temporary `std::atomic` counter inside
`SurfaceSignalInfo::Proximity`, printed at render end, removed before commit.
On the shipped plank frame: **82,401,587 probes, 74,446,695 hits, 7,954,892
misses — 90.35 %.** Gate ≥ 80 % — **PASS**. (The scene makes exactly one
distinct `(kind, radius)` proximity query per hit, shared across the colour
program's ramp/relief taps and the roughness program, which is why the rate is
high; it is one under the four-way L1 capacity §10's cliff bullet describes.)

**Draft preview: measured by reading the code, not by rendering, because it is
NOT reachable from the CLI.** There is no `interactive_pel_rasterizer` chunk
and no `RISE_API_Create*` factory; `quality:"draft"` reaches
`RISE::Implementation::CreateInteractiveMaterialPreviewPipeline` only through
`AgentSession`. What the code says is worth recording anyway, because it
contradicts the MCP tool's own description. That description claims draft
"IGNORES the scene's authored materials and lighting entirely" — but
`InteractiveMaterialPreviewShader::MaterialAlbedo` calls
`bsdf->albedo( ri.geometric )`, and `GGXBRDF::albedo` calls
`pDiffuse->GetColor( ri )`, which runs the expression and therefore the
`proximity` query. So draft DOES pay it: once per primary hit at 1 spp
(~3 × 10⁵ queries for a 640 × 480 frame) against the production frame's
8.2 × 10⁷ — about 0.4 % of the production query count. Not measured as wall
clock; stated as the bound the code supports.

**Beauty.** Full frame plus a 3× crop at `x0=280 y0=150 w=200 h=120` (the AO
evidence framing), judged against a no-proximity control rendered from the same
tree.

*What reads:* at the head, a distinct dark patchy smudge where the rim meets
the board — it grounds the head, which previously floated on its own contact
shadow. Measured over the 93 plank pixels whose probe proximity exceeds 0.6,
mean luma falls **107.7 → 63.8, −40.8 %**; over the 134 pixels at 0.3–0.6,
−17.3 %; at 0.05–0.3, −2.9 %; and **off-seam, over 117,081 pixels, +0.00 %** —
the term is exactly as localised as the geometry.

*What does not read:* the tip's furrow. It is the strongest contact in the
scene (proximity 1.0 over 9.5 mm) and the shank itself hides almost all of it
from this camera; the visible sliver is a faint darker line under the point.
And the whole feature is **small**: 896 pixels of 307,200 (0.3 %) carry any
signal at all, of which 227 exceed 0.3.

*Honestly, against the AO crop's failure mode* (§8.1: 0.65 / 0.96 / 0.97 at
1 / 4 / 10 px, "a fifth of the field on a few pixels", no seam): the distance
signal is better in the way that matters and worse in the way that does not.
It is **correct where AO was wrong** — it reads 0 under the shank's head end
where the gap is 3.5 mm and AO read the nail's shadow as contact for the
shank's whole length, and it reaches ~0.93 at the rim where AO saturated at
0.65. It is **not more visible than AO was**, because the pixels it lights are
the same pixels the contact shadow already darkened. The gain is that the mark
is now in the right place and fades over the right distance, not that there is
more of it.

One unplanned and welcome consequence: the plank's own underside sits 0.1 mm
above the bench plane, so the bottom 2 mm of its side faces pick up the same
term — a physically-correct plank-meets-bench seam. It reads at ≤ 0.12 on a
2–3 px diagonal line and is visually negligible; recorded so nobody later
mistakes it for a leak.

#### B. `weathered_workbench`, `proximity(0.02)`

The flange's touchdown geometry is exactly as §8 modelled it. Profiles
outward from the touchdown line at the middle of each of the four sides,
measured with the query on the bench top (`y = 0.91`, 2 mm grid):

| outward | +X | −X | +Z | −Z | gate |
|---|---:|---:|---:|---:|---|
| 0 mm | 0.998 | 0.998 | 0.998 | 0.998 | — |
| 5 mm | **0.952** | **0.956** | **0.965** | **0.972** | ≥ 0.9 → **PASS** |
| 20 mm | **0.554** | **0.564** | **0.534** | **0.554** | ≤ 0.6 → **PASS** |
| 35 mm | **0.000** | **0.000** | **0.000** | **0.000** | 0 → **PASS** |
| 40 mm | 0.000 | 0.000 | 0.000 | 0.000 | — |

(the underlying distances at 5 / 20 / 35 mm are 0.97 / 8.92 / 21.9 mm on +X and
0.56 / 8.92 / 20.6 mm on −Z; §8's model predicted 0.966 / 0.555 / 0). Mid-side
world coordinates and their pixel projections: +X 5 mm is (1.2495, 0.91,
−0.1753) → px (535, 206); −Z 5 mm is (1.0095, 0.91, −0.1465) → px (495, 207).
**Those pixels read `nan` in the probe** — the flange is 42 mm tall and the
camera is 11° above the bench, so the flange hides ~216 mm of bench behind it
and its own touchdown line is not visible from this camera at all. The
rendered corroboration is therefore taken over every visible bench-top pixel,
binned by the query's distance:

| band | render 640 × 480 | render 2560 × 1920 | query at band centre |
|---|---:|---:|---:|
| 0–2 mm | (too few px) | 0.820 | 0.950 |
| 2–4 mm | (too few px) | 0.874 | 0.850 |
| 4–6 mm | 0.732 | 0.804 | 0.750 |
| 8–10 mm | 0.740 | 0.618 | 0.550 |
| 14–16 mm | 0.454 | 0.326 | 0.250 |
| 20–22 mm | 0.257 | 0.047 | 0.000 |
| 26–28 mm | 0.097 | 0.000 | 0.000 |
| ≥ 60 mm | 0.006 ± 0.059 | 0.000 | 0.000 |

At 640 × 480 the profile is smeared across ~40 mm — the 31.4 mm-per-pixel
footprint quoted above. At 4× it tracks the query within ~0.08 and reaches 0
by 22–24 mm. Neither render is the gate instrument here and neither can be.

**Cost**, interleaved, 640 × 480 × 12, every run listed, two batches:

*Batch 1 — run while another agent was compiling:*

| run | 1 | 2 | 3 | 4 | mean |
|---|---:|---:|---:|---:|---:|
| no-prox copy | 2.989 | 3.018 | 2.976 | 3.008 | **2.998 s** |
| shipped | 3.259 | 3.291 | 3.247 | 3.300 | **3.274 s** |
| pristine | 2.935 | 2.949 | 2.967 | 2.961 | **2.953 s** |

1.092× against no-prox, 1.109× against pristine.

*Batch 2 — quiet machine (no compilers running at batch start or end):*

| run | 1 | 2 | 3 | 4 | mean |
|---|---:|---:|---:|---:|---:|
| no-prox copy | 2.970 | 2.910 | 2.926 | 2.918 | **2.931 ± 0.023 s** |
| shipped | 3.153 | 3.143 | 3.174 | 3.128 | **3.149 ± 0.017 s** |
| pristine | 2.908 | 2.899 | 2.855 | 2.879 | **2.885 ± 0.020 s** |

**1.075× against the no-proximity copy and 1.092× against the untouched
pre-wave-2 file — PASS against the ≤ 1.10 gate on both baselines.** The 1.6 %
gap between the two baselines is the no-prox copy's extra (zero-weighted) `add`
node, which the copy keeps so that only the query differs; both are reported
because batch 1's pristine ratio (1.109×) sat within 0.001 of the gate and only
the quiet batch settles it.

**Beauty.** 4× crop at `x0=460 y0=150 w=160 h=90`, against a no-proximity
control. The grime reads: a soft, patchy dark stain on the bench hugging the
flange, strongest at the front-left where the bench is most visible, with no
hard outline. Measured on the bench-top plane: over the 76 pixels above
proximity 0.6, mean luma **90.0 → 72.4, −19.5 %**; 214 pixels at 0.3–0.6,
−9.6 %; 77 at 0.05–0.3, −6.0 %; **off-seam, 16,913 pixels, −0.0 %**. This is
the one §8.1 said an AO could not have ("at the vise foot the ring is invisible
at every magnitude"), and it is visible here for a reason worth naming: it is
not confined to the 2–3 px the flange's own contact shadow occupies, because a
20 mm distance ramp is wider than that shadow.

The **floor at the legs' feet is deliberately not painted**: the floor's
material is a `uniformcolor_painter`, a flat supporting surface, and making it
an expression painter to carry one dust band is a larger change than the band
is worth. The LEG side of the same seam does get the term, since the legs share
`mat_wood_top`. The **tray** also rests on the bench top and collects the term
on the bench beneath its edge — physically right, and noted so it is not read
as a leak.

#### Composition notes that cost time and are worth carrying forward

- **`fbm` in this VM is SIGNED and zero-mean** (`NoiseCore::Fbm3D` sums signed
  Perlin octaves with no normalisation), so a breakup multiplier written
  `0.45 + 1.35*fbm(...)` averages **0.45**, not the ~0.9 it reads like. The
  first draft of the plank's term lost 55 % of its weight that way — probed,
  `cg` came back at 0.339 where the signal itself was 0.745. Centre the
  multiplier on the value you want: `0.85 + 1.5*fbm(...)`.
- **The weight has to reach the ramp's reserved stops, and where the seam
  pixels START is a measurement.** The plank's seam pixels sit at field p50
  **0.195** (early-wood plateau in the nail's shadow, `crev` = 0), while the
  ramp reserves 0.86–1.00 for rare events. At `grime 0.45` the whole term was
  worth **−6.9 %** luma on 93 pixels and read as nothing; at 0.85 it moves the
  seam's field p50 to 0.78 (p90 0.97, 9 of 99 pixels clamped) and −40.8 %.
- **A contact term needs its OWN roughness bridge on both scenes, and for
  opposite reasons.** The plank's roughness bridge has a negative scale (its
  field's high end is dense, smooth late wood) and the bench's a positive one
  (its high end is pale, weathered sapwood); grime must read ROUGHER on both,
  so on the plank it cannot ride an additive term into a negative bridge and on
  the bench it cannot ride a subtractive term into a positive one. One bridge
  cannot give one field two signs. Both scenes now sum a small dedicated
  `scalar_painter` (the pattern `sp_pore` already established), sharing the
  colour term's breakup field verbatim so the dull patches and the dark patches
  coincide.
- **The nail's own contact rust was tried and REMOVED on measurement.** §8.1's
  one genuine AO gain was the nail's underside, so the symmetric term
  (`rust_cont 0.45 * clamp(proximity(0.002)*(0.85+1.5*fbm), 0, 1)`) was
  composed and rendered. Over the 2539 visible nail pixels whose proximity
  exceeds 0.3 it moved mean luma from 48.75 to 48.96 — **+0.4 %**, on pixels
  already at luma 49 because they sit in the contact shadow. The nail's contact
  surfaces are the head rim's underside, hidden by the head disc, and the buried
  tip, hidden by the shank. Removed; the measurement is recorded in the scene.

#### Adoption surfaces

`skills/agent/procedural-textures.md` gains "`proximity(r)` — the fourth
signal, and the only cross-object one", with the world-length unit in every
sentence that names the radius, the 1-touching/0-nothing/neutral-0 convention,
the colour-and-roughness-not-relief rule, the emitters-never-count rule, the
ellipsoid-ratio rule, and one worked example: the nail's contact seam as a
complete standalone 128 × 128 scene (`AgentSkillsTest` renders every fence, so
it has to be). A/B'd at authoring time (`grime` 0.85 → 0, same seeds): **760 of
16,384 pixels move, max delta 0.415**, so the snippet demonstrates the term
rather than merely parsing with it. The snippet count assertion moves 31 → 32.
`scenes/FeatureBased/README.md`'s entries for both showcases say the signal is
now in them, and the workbench's says which claim it retracts.

#### Gate summary

| gate | result |
|---|---|
| warning-free `make all` | PASS |
| plank station 1 (head rim, 0.4 mm) ≥ 0.9 | PASS (0.918) |
| plank station 2 (shank, head end) = 0 | PASS (0.000) |
| plank station 3 (mid-shank) 0.1–0.4 | PASS (0.347) |
| plank station 4 (tip, 0.4 mm) ≥ 0.9 | **FAIL (0.882)** — §8's `ρ ≈ 1 mm` model was optimistic; radius NOT tuned |
| plank 0 at ≥ 8 mm, 0 on open plank | PASS |
| plank cost ≤ 1.15× | PASS (1.111–1.131× across three interleaved batches; 1.18–1.25× against the 16.33 s literal, which is a different day's machine state) |
| plank L1 hit rate ≥ 80 % | PASS (90.35 %) |
| draft preview cost measured | not CLI-reachable; bounded from the code at ~0.4 % of the production query count |
| bench ≥ 0.9 within 5 mm | PASS (0.952–0.972, four sides) |
| bench ≤ 0.6 at 20 mm | PASS (0.534–0.564) |
| bench 0 beyond 35 mm | PASS (0.000) |
| bench cost ≤ 1.10× | PASS on a quiet machine (1.075× vs the control, 1.092× vs the pristine file); a contended batch read 1.092× / 1.109× |
| bench prose corrected | PASS (390 × 42 × 344 mm / 18 mm rounding; the "out of reach" claim retracted) |
| `CstDeriveGoldenTest` | 446 MATCH, 0 DRIFT (regenerated: exactly the two showcase rows) |
| `AgentSkillsTest` | 618 passed, 0 failed (32 snippets) |
| `ProximitySignalTest` | 107 passed, 0 failed |
| `ExpressionMemoTest` | 196 passed, 0 failed |
| temporary counter removed | PASS |

---

## 9. Test plan

- `tests/ProximitySignalTest.cpp` on scene C: closed forms per exact family
  (plane, sphere, box, capped and open cylinder, disk, clipped plane, torus);
  the ellipsoid bound; the composed-SDF bound with `gap_max`; the exact single-
  sphere SDF at 1e-9; self-exclusion; the instanced copy counts (named); a
  `casts_shadows FALSE` neighbour counts; an emissive neighbour does not (the
  fixture is a hand-authored emissive box, so the disclosed decorative-object
  exclusion is what the test pins); CSG operands never count and the composite
  refuses (v1); a heightfield SDF refuses **asked directly**, so the check
  isolates that family rather than resting on a neighbour's probe budget; a RAW
  (non-indexed) mesh and a Bezier
  patch refuse; a coplanar CONVEX clipped plane is exact while a non-coplanar
  one and a coplanar DART both refuse; a degenerate (`det == 0`) transform
  refuses; the non-uniform-scale
  upper bound (`σ_max`, and the `r/σ_min` candidate conversion cannot miss a
  neighbour within `r`); the uniform-scale detection is exact (a rotated,
  uniformly scaled sphere within 1e-9); the parse-time diagnostic for a
  non-positive literal `proximity` radius names a WORLD LENGTH; radius cut-off exactly at `r`; a neighbour BELOW the receiver
  reads the same as one above (unsigned); **interpenetration reads 1** (a point
  inside a neighbour); neutral 0 with a null channel, with a non-finite point,
  and with a computed radius ≤ 0 or non-finite; the builtin end-to-end through
  an `ExpressionPainter` at a real hit.
- `tests/ExpressionMemoTest.cpp`: the four new `SignalHitKey` fields separate in
  both L1 and L2 rows; the generation clears the query memo (red-proved); the
  TLS ceiling.
- `tests/ProximityInvalidationTest.cpp` (F): derive, render a probe pixel, move
  the neighbour through `DeriveToJobIncremental`, render again — the value
  changes; move it beyond `r` — it reads 0; a keyframed neighbour across
  `RasterizeAnimation` frames — each frame's value matches a fresh evaluation.
  **The moved pose keeps the neighbour OFF TO THE SIDE.** Moving it to `x = 0`
  parks it between the camera and the probe, so the frame then shows the box's
  own flat albedo and every check reads a constant while appearing to measure
  the signal — the exact failure the file's header warns about, which the first
  version of the test committed anyway. The moved-pose check therefore has
  teeth: the rendered mean must match the closed-form `proximity` for that pose
  divided by π, within a tolerance derived from the 8×8 patch's footprint, and
  must NOT equal the box albedo `0.5/π`.
- The **demand gate**: after a render of a scene with no live `proximity()`
  consumer, `ObjectManager::ForTest_HasBoxSnapshot()` is false; with one live it
  is true. The gate is a NEGATIVE (work not done), so a timing assertion would
  be a flake and an accessor is the only honest observation.
- `SourceHygieneTest`: no assignment to `.signals` outside the two intersectors,
  CSG adoption, and `BuildContext`.
- Phase 2: `MeshClosestPointTest` differential against brute force; scene D's
  probe values.

---

## 10. Residuals, stated up front

- BDPT/VCM/MLT rebuilt records read 0 (the family's disclosed gap; the
  `PathVertexEval` contract is explicitly declined for the same reason as for
  the other three signals).
- Refusing families (RAW meshes, patches, hair, CSG composites in v1, heightfield
  SDFs) read far.
- SDF neighbours are an upper bound on distance (never over-read contact),
  bounded by the last probe step; the gap is measured on C, not bounded
  analytically. A candidate whose crossing is not found within budget reads far.
- Non-uniform scale and ellipsoids are upper-bounded, not exact, in v1.
- **An eccentric ellipsoid neighbour needs an author to inflate the radius.**
  The bound is the semi-axis ratio, and at 4:1 that is not academic: a point at
  a true distance of **2.75** from such an ellipsoid is reported at **11.0**, so
  `proximity(3)` paints nothing there. The rule an author needs is
  **≈ ratio × the radius you actually mean** against an eccentric ellipsoid (and
  against an anisotropically scaled object of any family, which the log line
  quantifies per object). Said in the descriptor text as well as here, because
  the failure is silent — an unpainted seam, never a wrong one.
- **A NON-CONVEX coplanar clipped plane refuses**, along with the non-coplanar
  one. Coplanarity alone is not enough: what the class traces is the bilinear
  patch, whose image is the polygon only when the quad is convex. Over a dart's
  reflex lobe the polygon form would report `|h|` where the surface is further
  away — an under-report, the forbidden direction. Every `rect_light` and every
  hand-authored panel is convex, so nothing in the acceptance set moves.
- **The reported range is `[0, maxDist)`, not `(0, maxDist]`**: zero is attained
  (interpenetration is contact — the solid families clamp their signed field at
  zero rather than taking its absolute value) and the radius itself is excluded
  (a candidate is accepted only on `d < best`, with `best` starting at the
  radius). The signal is continuous across the cut-off either way, since
  `1 − r/r = 0` is the neutral.
- **The eager snapshot build and the per-ray `EnsureBoxSnapshot()` are gated on
  `ProximityDemand`** — a counter in the mould of `SurfaceSignalDemand`,
  registered by `ExpressionPainter` / `ExpressionScalarPainter` when the
  compiled program calls `kFnProximity`. Ungated, the per-ray call was a load, a
  compare and a heap indirection on the hottest path in the renderer, charged to
  every scene in the tree. Correctness does not rest on the counter:
  `NearestOtherSurface` still builds lazily under `treeCreationMutex` when asked
  with no snapshot. **The trade:** an unregistered consumer — a hypothetical
  direct caller of `NearestOtherSurface` — pays the whole build under the lock on
  its first call instead of finding it ready.
- **`kL1Ways == 4` now exactly equals the number of signal KINDS.** When the
  memo shipped, four ways was headroom above a two-signal working set; with
  `proximity` there are four kinds, and the L1 key separates on
  (kind, radius, hit), so a body making four distinct (kind, radius) queries per
  hit sits exactly at capacity and a fifth collapses the round-robin set to ~0 %
  — a cliff, not a slope. Eight ways would blow the 2048-byte TLS ceiling
  (already 1824 B). Wave 2's L1 hit-rate gate measures this on `plank_closeup`,
  which makes three distinct queries per hit — one under capacity.
- **Emissive objects never count**, including decorative ones (a lava pool, a
  glowing rune) that should collect contact; the predicate is per object and
  RISE binds one material per object. Phase 3 if a scene needs it.
- **`proximity` cannot drive relief**: `ReliefModifier` holds `signals` fixed
  across its stencil by documented design.
- Per-sample motion blur: a neighbour moving under a non-keyframed painter can
  serve a stale query-memo entry only at a bit-identical receiver point, which
  sub-pixel jitter makes essentially unreachable; no scene in §1 exercises it.
- The candidate scan is linear in object count over cached AABBs; Sponza (405)
  is the measured ceiling, the TLAS point query the upgrade.
- The signal is unsigned: it cannot distinguish "just above the floor" from
  "just below it"; a signed variant needs an inside test per family (Phase 3).
- The query reads other objects' transforms and so joins the pre-existing
  per-sample `EvaluateAtTime` race (ARCHITECTURE.md), no wider than
  `Object::IntersectRay` already does.
- The scalar pipe stamps `time = 0` (no `m_time` there), so its memo entries
  rely on the jitter argument alone under motion blur.
- Non-uniform transforms use the Frobenius/determinant σ bounds, which are
  loose (never unsafe); exact σ needs eigen-numerics the engine does not have.
  **Measured on `scale (3, 1, 0.4)`** (the §5.2 table): the log's printed factor
  is **26.99**, the transform's true singular ratio is **7.5**, and the worst
  over-report actually observed is **7.96×** — so the bound is roughly 3.4×
  pessimistic. The object-space search radius is inflated **8.47×** (`1/σ_min`),
  which is the cost side rather than the contact error, and is printed as its
  own number for that reason.
- The memo-eligibility threshold moves with `kFields` (29 → 35) for
  pure-arithmetic bodies; bit-identical, perf-only, disclosed. `ptWorld` is a
  redundant compare in the L2 key.
- **A CSG composite refuses, so its OWN surface is invisible to every
  neighbour's query** — a sharper statement than "its operands do not count
  separately". `Object::DistanceToSurface` forwards to the geometry and
  `CSGObject` has none, so nothing in the scene can measure its distance to a
  CSG result. Phase 3's union-min work is what fixes it; until then a scene
  whose contact surface is a CSG result needs a non-CSG proxy. (Wave 1,
  2026-09-08.)
- **The signal is PT-only more sharply than the first bullet suggests.** The
  channel is stamped by `ObjectManager::IntersectRay`, so EVERY consumer that
  builds its own hit record reads the neutral 0 — `PathVertexEval`, the GUI's
  painter preview, realize-time displacement, `HairGenerator`. That is the same
  set the other three signals are neutral on, but here the neutral means "no
  contact anywhere in the scene", which is a more visible absence than
  "unoccluded".
