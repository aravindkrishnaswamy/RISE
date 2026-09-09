# Cross-Object Proximity — a contact signal for grime, dust and wear

**Status:** PHASE 2 SHIPPED 2026-09-09 — the MESH family answers, on a bounded
closest-point traversal of its own BVH (exact: bit-identical to brute force
on all 63,096 answered points of 110,000), `displaced_geometry` forwards to
its baked mesh, scene D is placed by its own test, and the candidate scan
became a **TLAS point query** because the flat scan FAILED Sponza's cost
gate at 1.273× (it now passes at 1.067×).  §8.3 carries every measured
number, each gate PASS/FAIL, and the four places building it corrected this
document — including one the table got wrong: **a mesh is a SHEET for this
query, with no interpenetration clamp**, unlike every solid family.
PHASE 1 SHIPPED — wave 1 (the engine) and wave 2 (the two
showcases, every Phase-1 gate measured) both landed 2026-09-08; §8.1 and §8.2
carry what each wave built and where the code and the measurements corrected
this document.  **One gate FAILED and is reported, not tuned to pass**: the
plank's tip station reads 0.882 against a ≥ 0.9 gate, because §8's local
`ρ ≈ 1 mm` model for the tip was optimistic (the tip is a furrow, not a
touchdown point — see §8.2).  **The plank cost gate passes in relative
terms** (1.11–1.13× against an interleaved control) **but fails the literal
"≤ 1.15 × 16.33 s"**, because that 16.33 s reference was measured on a
different day's machine; the pristine pre-wave-2 file and its cost-equivalent
no-prox baseline (§8.2) both measure 16.76–18.10 s today, above 16.33 s on
their own.  Accepted after three adversarial rounds
(round 3: the nail rests on its buried tip, not its shank, with its head rim
nearly touching (0.146 mm clear) — the flagship
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
| B | `scenes/FeatureBased/Textures/weathered_workbench.RISEscene` | box bench top ← SDF vise flange; infinite-plane floor ← box legs; box tray | The header declared grime under the flange "out of reach — occlusion() is self-occlusion only" until wave 2 retracted it. The bench top is a `box_geometry` with **no signal provider at all**, so this proves the signal works on receivers outside the SDF/mesh families, and against analytic neighbours. |
| C | `scenes/Tests/Signals/proximity_closed_forms.RISEscene` (new) | plane ← sphere (closed form `sqrt(s²+ρ²) − ρ`), plane ← box, plane ← capped and open cylinder lying flat, plane ← disk, clipped plane, torus, ellipsoid; SDF ← tessellated mesh; a single-sphere SDF (exact field) and a composed `smin`/`subtract` SDF (bounded field); an instanced copy; a `casts_shadows FALSE` neighbour; a hand-authored emissive box; a CSG-subtraction receiver with its operands; a heightfield-mode SDF; a non-uniformly scaled SDF; a receiver point inside a neighbour; a neighbour below the receiver; a neighbour beyond the radius | Drives `ProximitySignalTest`: every Phase-1 family's distance against a closed form or its stated bound, the self-exclusion rule, the visibility rule (casts_shadows does not exempt; emitters never count), the CSG operand rule, the scale rule, interpenetration, the unsigned rule, the radius cut-off, and every neutral. |
| D | `scenes/Tests/Signals/proximity_mesh_contact.RISEscene` (new) | plane ← `models/risemesh/bunny.risemesh` placed so its lowest vertex touches the plane; a second mesh (`dragon_small`) resting on the bunny; the plane also under a `casts_shadows FALSE` sphere and a `rect_light` panel 2 mm above it | The mesh-neighbour path (Phase 2) on real assets, at real contact. Showroom was the first choice and is REJECTED: its torus knot floats 0.69 m above its platform (mesh Y-range 0.989–2.005 m under `scale 0.008`, platform top 0.3 m), so no authorable radius reaches it. The light panel proves lights never count (§2). |
| E | `scenes/FeatureBased/Geometry/sponza_new.RISEscene` (asset at `/Users/aravind/Working/Assets/main_sponza/...`) | 405 mesh objects, mesh ← mesh everywhere | Cost with the query forced at every hit (the §8.1 protocol), candidate-scan scaling at 405 objects, and a beauty check: dust where walls meet floor, the look production renderers get from Unreal's `DistanceToNearestSurface`. |
| F | `tests/ProximityInvalidationTest` (no scene file) | a neighbour moved through `Cst::DeriveToJobIncremental` between two renders, and a neighbour keyframed across `RasterizeAnimation` frames | END-TO-END: the next render (and the next animation frame) sees the move with no bake to invalidate — the property the AO bake could not have (O(scene)). It does NOT isolate the move from the pass-entry generation bump, and does not claim to: the unit-level pin that no stale entry survives a pass boundary is `ExpressionMemoTest`'s generation red-proof, and the pin that the query memo clears is the new (n) row there. Per-sample motion blur is NOT exercised by any scene in this set and is a stated residual (§10). |

A and B are production showcases and get the signal composed into their
materials (the plank's dirt term at `proximity(0.002)` — 2 mm against the
header's 0.40 mm p50 pixel footprint; the bench's "deliberately NOT painted on"
flange grime at `proximity(0.02)`). **Where the nail actually touches the plank
is the point of scene A**, and it is not along the shank: the head disc
(radius 7.5 mm) is thicker than the shank's 4 mm base, so the nail rests on
its buried tip; its head rim nearly rests (0.146 mm clear — see §8.2) and,
with its 6° nose-down, the shank's underside lifts from the tip up to
+3.5 mm above the plank at the head end, ≈ +1.5 mm mid-way, buried ≈ 0.55 mm
at the tip itself. No pitch fixes that without burying the head. So the seam
a distance signal must draw is a broad near-touch patch under the head rim,
a ~9.5 mm furrow at the tip, and a band that FADES along the lifting shank —
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
plane disappears. The TLAS point query was the Phase-2 upgrade if the flat scan measured above
budget on E. **It did (1.273× against a ≤ 1.25× gate) and the upgrade SHIPPED**
— but not as "walk `nodes4[]` for boxes containing the point". It reuses
`BVH::ClosestPointDistance`, the same bounded closest-point traversal the mesh
family uses on its own triangles, one level up, over the BVH2 `nodes` array;
that prunes by the running best and orders nearest-first, which a containment
walk does not. §8.3 has the measurement that forced it, the one that kept it,
and the contract shift it carries. The flat scan REMAINS for scenes with no
TLAS (≤ 4 objects, or `bUseBSPtree` off).

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
| Indexed triangle mesh (every loader but RAW; tessellated primitives; `displaced_geometry`'s internal mesh) | **SHIPPED (§8.3).** Closest point on the mesh's own BVH2: a bounded-radius traversal visiting the two children of each node nearest-AABB-first, pruning any node whose point-to-AABB distance is ≥ the running best, `PointTriangleDistance` (Ericson's region method) at the leaves. **SHEETS, with NO interpenetration clamp** — unlike every solid family, which clamps a signed field at zero so a point inside reads 0; a triangle soup carries no inside test (it may be open, non-manifold or self-intersecting), so a point inside a closed mesh reports its honest distance to the nearest triangle. That is the safe direction: an over-report under-paints, never over-paints. | exact — identical to brute force under the same point–triangle formula, **measured bit-for-bit on 63,096 answered points of 110,000** (§8.3); node boxes are conservative `float`, rounded OUTWARD at build, so reading them can only ADMIT a node the pruning could have skipped | 2 |
| Non-indexed mesh (RAW) | refuses (no BVH). Still true after Phase 2, and pinned by `MeshClosestPointTest` (e), which asks the RAW family and its INDEXED twin the same question about the same triangle: the twin answers 1.0, the RAW one refuses | — | — |
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

### 5.6 Phase 3 design — signed lower bounds, CSG composites, exact σ, `interior(r)`, `add_wear`

Phase 3 is not four independent conveniences; three of its items rest on one
new capability — a per-family **signed distance LOWER bound** with an exact
sign — and the fourth (exact σ) tightens a bound Phase 1 left loose. Written
2026-09-09 before Phase 3 began; revised the same day after three adversarial
rounds (round 5, 3 P1s: the exact-operand list named sheets, a nested
composite's exactness was unspecified, the barren `add_wear` body's dead-param
list was wrong with its fbm argument unpinned; round 4, 1 P1: strict membership alone discards the exact landing on
an operand's boundary, so composites of exact primitives would read `d + ε`
with ε undefined; round 3, 1 P1: the bracket's `f ≤ 0` landing admits the phantom
touching set where operand boundaries are tangent — `glass_pavilion`'s flute
slot is exactly that case; round 2, 3 P1s: a union had no signed rule; the glass_pavilion probe
stations sat inside the column's cap; `add_wear`'s flat-receiver gate lives in
the conditions scan, not the verb; round 1, 7 P1s: the union fast path composed under-reading magnitudes; CSG
operands live in the COMPOSITE's frame, not world space; the inside depth was
scaled by σ_max instead of σ_min; the σ gate's reference was a tautology; the
`interior` gate contradicted the fixture; `add_wear`'s predicate and mask
algebra; the ellipsoid/anisotropic operands over-read and break the descent).

**Two bounds per operand, and which one each consumer needs.** Phase 1 ships
one query, `DistanceToSurface`, whose answer is exact or an UPPER bound
(never over-reads contact). A composed field for CSG, and an interior depth,
both need the OTHER direction: a magnitude that is a LOWER bound, so that the
early-out `f > maxDist ⇒ skip` cannot skip a genuine neighbour and the descent
step `|f|` cannot overshoot the zero set. Phase 3 therefore adds, beside the
unsigned upper bound, a **signed lower bound** with an exact sign:

- `IGeometry::SignedDistanceLower( ptObject, maxDistObject, outSigned ) → bool`
  (refusing default) and `IObject::SignedDistanceLower( ptWorld, … )`, which
  does the transform: the point through the inverse, the radius by `/σ_min`,
  and the magnitude **back by `σ_min`** — the lower bound's safe direction
  (`d_w ≥ σ_min·d_o`), the opposite of the unsigned query's `σ_max`.
- Per family: sphere, box, capped cylinder, torus — exact closed forms, so
  the lower bound IS the distance, inside and out; ellipsoid — sign exact
  from `Σ (p_i/a_i)² ≤ 1`, magnitude `dUnit × min(a,b,c)` (the unsigned query
  keeps `× max`); SDF — `Map(p)` itself (exact sign, 1-Lipschitz under-read),
  heightfield mode refuses as the unsigned query does; planes, disks, clipped
  planes, open cylinders, meshes, patches, hair — sheets, no inside: they
  answer the unsigned query as before and REFUSE this one.

**Why `min(operands)` is the wrong CSG answer, and what the right one is.**
For a point `p` outside both operands of a **union**, the true distance to the
union's surface is exactly `min(d_A, d_B)` (every union-boundary point lies on
∂A or ∂B, so `d ≥ min`; and if the nearest point of ∂A lies inside B, the
segment to it enters B first at a point on ∂B that is outside A — a boundary
point no farther than `d_A` — so `d ≤ min`; tangential contact falls in the
first case). But the composite can only use what the operands report, and
`min(exact, under-read)` under-reads. So the union's UNSIGNED answer is
`min(u_A, u_B)` over the operands' unsigned upper bounds, itself an upper bound
(`d = min(d_A, d_B) ≤ min(u_A, u_B)`), exact when both operands are exact —
taken over the operands that ANSWER (`d ≤ d_A ≤ u_A` holds whatever B does),
so a union refuses only when both operands refuse; and
the union's SIGNED LOWER BOUND — which a parent composite ("a union inside a
subtraction") and `interior` both need — is `min(f_A, f_B)` over the operands'
signed lower bounds (sign exact: inside the union iff inside either; outside,
`min(f_A, f_B) ≤ min(d_A, d_B) = d`; inside, the depth to leave `A ∪ B` is at
least `max(depth_A, depth_B) = |min(f_A, f_B)|`), answered only when BOTH
operands answer it. A union accepts a SHEET operand for the unsigned query
(its unsigned answer is all the proof needs) and refuses the signed one with
it; it cannot say whether `p` is inside a sheet operand, so a point inside a
mesh operand reads a positive distance rather than contact — the under-paint
direction, disclosed. For **intersection** and **subtraction** the min is only
a lower bound (the nearest operand-surface point may not be on the composite's
surface at all), the forbidden direction; those two compose the operands'
signed lower bounds into a field `f` — `intersection → max(a, b)`,
`subtraction → max(a, −b)` (inside = negative) — whose sign is exact and whose
magnitude is a lower bound (any composite-boundary point lies in
`closure(A) ∩ complement(int B)` for a subtraction, so its distance is at
least both `f_A` and `|f_B|`; likewise for an intersection), and run Phase 1's
bracket on it: descend along the numerical gradient of `f` (six evaluations
per gradient, each recursing into both operands), probe until the landing
point `q` is PROVEN to lie in the closure of the composite's REAL solid by
the operands' own signs, and report the chord `|p − q|`. Two arms prove it.
The STRICT arm — intersection `f_A < 0 ∧ f_B < 0`, subtraction `f_A < 0 ∧
f_B > 0` (a negative lower bound proves strictly inside its operand, a
positive one strictly outside) — puts `q` in the solid's interior, so the
segment from `p` crosses the boundary. The BOUNDARY arm admits a landing
exactly ON one operand's surface when THAT operand's signed distance is
EXACT — `SignedDistanceLower` returns an exactness flag, set ONLY by the four
SOLID closed forms (sphere, box, capped cylinder, torus); ellipsoid and SDF
never set it, and sheets (plane, disk, clipped plane, open cylinder, mesh)
refuse the signed query altogether, so they can never carry it — a
subtracted plane admitted to this arm would report the chord to a face the
subtraction removes nothing at, the round-3 phantom in a new form. The arm:
intersection `(f_A ≤ τ ∧ f_B < 0) ∨ (f_A < 0 ∧ f_B ≤ τ)`, subtraction `(f_A
≤ τ ∧ f_B > 0) ∨ (f_A < 0 ∧ f_B ≥ −τ)`, where the `τ` side is the exact
operand and the other side is STRICT — a point on `∂A` strictly outside `B`
(or on `∂B` strictly inside `A`) is on the real solid's boundary, because a
neighbourhood of it holds interior points of the solid. The exact side is
tested to a tolerance `τ = 1e-12 × the composite's local diagonal` (not
`≤ 0`), because the descent `p ← p − f·ĝ` lands on the exact operand's
surface only to rounding, and a `+1 ulp` miss must not throw the exact
landing away; a landing up to `τ` OUTSIDE the exact operand is within `τ`
of its real surface, so the chord under-reads distance by at most `τ` —
stated, and negligible. Neither arm admits `f_A = f_B = 0` (the strict side
rejects it), which is what keeps the flute tangency out. Which arm applies
is decided PER LANDING by the exactness of the operand whose surface was
reached, not by what the composite contains: `SDF − box` landing on the box
face takes the boundary arm and is exact to `τ`. When the reached operand
is a BOUND one (SDF, ellipsoid) only the strict arm can prove the landing,
so the probe steps by the composite's own `ε = max(1e-6, 5e-5 × local
bounding-box diagonal)` — the SDF family's rule; `CSGObject` has no
`IGeometry` and its only box accessor (`getBoundingBox()`) is WORLD-space,
so the local diagonal is computed from the operands' composed local box
(a subtraction takes A's) and cached at finalize as `m_localDiagonal` —
and the answer lies in `[d, d + ε]` locally. Hence an exact-operand landing
answers within `[d − τ, d]`, a bound-operand landing within `[d, d + ε]`,
and the world-space slack is `σ_max` times either (ε = 2.52e-4 on
`column2`'s 5.0498 local diagonal, σ = 1 — stated in §8 and §10; the
composite's 5e-5 is fixed while an SDF operand's `m_epsFrac` is
author-settable, so a composite may probe finer than its operand's own
surface band, harmlessly). A query point lying exactly ON the composite's
surface has `q = p` as its zero-length descent endpoint: admitted by the
boundary arm when the reached operand is exact (reads 0), otherwise the
probe steps and it reads at most `ε` — the unsigned clamp is on a NEGATIVE
composed sign, since `f = 0` is also the phantom set. Refuse when no
admitted landing arrives within budget. `f ≤ 0` is NOT the landing test (the round-3 P1): `{max(f_A, −f_B)
≤ 0}` is `closure(A) ∩ complement(int B)`, which contains every point where
`∂A` merely TOUCHES `∂B` — a phantom set no ray can hit. `glass_pavilion`'s
flute slot is that case exactly: `flutegeom` is 0.5 deep, the column's
diameter, so its ±z faces are tangent to the cylinder at local `z = ±0.25`;
from a station 1 cm outside that face the descent lands on the tangency
with `f_A = f_B = 0`, and an `f ≤ 0` test would report 1.00 cm where the
nearest real surface is the slot-wall/cylinder corner at 4.21 cm
(`sqrt(0.04² + (0.26 − sqrt(0.25² − 0.04²))²)`) — an OVER-read of contact,
the forbidden direction. Under the admitted-landing test that station keeps probing into the slot
(inside B, so `f_B < 0`, and `f_B = 0` on the tangent face is refused by
both arms), never lands, and refuses: an under-paint, disclosed, and the
correct `proximity(0.02)` there is 0 anyway. Phase 1's bracket caches the
descent gradient and recomputes it only when it has none, so the degenerate
central difference AT the tangency (a V-valley of value 0) is never taken;
an implementation that recomputes it there gets the same refusal through
the 1e-12 seam rule instead. The composed field an intersection/subtraction
descends is ALSO what it exports as its own `SignedDistanceLower` to a
parent composite and to `interior`, with the composite's `×σ_min` applied —
and it NEVER sets the exactness flag: `max(a, b)` under-reads near a seam
even over exact operands, and its zero set is the phantom touching set, so a
parent's boundary arm must not land on it (the round-3 bug one level up).
A union exports `min(f_A, f_B)` and sets the flag only when both operands
set it AND its own σ is exact (a uniform transform), since `×σ_min` under a
non-uniform one is a bound. At a max/min seam two nearly opposed operand
gradients can cancel the composite's, and a gradient below 1e-12 makes the
candidate REFUSE (an under-paint, never a wrong answer — the SDF's fabricated
`(0,1,0)` fallback is not reused). `SignedDistanceLower` NEVER refuses for
range: its `maxDist` argument bounds effort, and the descent evaluates
operands at points outside any radius — an operand copying
`DistanceToSurface`'s lower-bound early-out into the signed query would make
every intersection with a small subtrahend refuse; only a family or
degeneracy refusal propagates. An operand that refuses the signed lower bound
(any sheet, a heightfield SDF) makes an intersection or subtraction REFUSE.

**The composite's own frame — the round-1 blocker.** Operands are NOT in world
space: `CSGObject::IntersectRay` maps the ray by the composite's own inverse
transform and then calls each operand, whose `Object::IntersectRay` applies its
own inverse on top (`SelfHitRootFloor` states the rule: "the arguments are in
THIS composite's local frame; a child geometry's gate is expressed in the
CHILD's local frame"). `glass_pavilion`'s fluted columns are exactly this —
`csg_object … position 2.5 2.5 2.5 orientation 0 45 0` over untransformed
operands. So `CSGObject` implements BOTH queries with its own transform layer,
exactly as `Object` does: map `ptWorld` through the composite's inverse, convert
the radius by the composite's `/σ_min`, recurse into the operands with the
LOCAL point (each operand applies its own transform; a nested composite repeats
the same two conversions), and convert the answer back — `×σ_max` for the
unsigned upper bound, `×σ_min` for the signed lower bound. The composite's
unsigned query clamps at zero when its composed sign is negative (a point
inside the composite's solid is contact, §2); a point inside operand A of an
intersection it does not share with B is OUTSIDE the composite and reads a
positive distance — correct, not a violation, and stated here so nobody "fixes"
it. `bComplementedField` never enters: the composite composes signs itself.
The per-object refusal log covers composites as it does any object, naming
the kind as "csg <op>" rather than the current "(no geometry)" fallback and
widening the sentence's parenthetical ("an SDF bracket that did not close")
to "an SDF or composite bracket that did not close"; §2's
unbounded-radius confirm gains a second non-`O(1)` case beside the SDF (an
intersection/subtraction re-running its bracket, once per object), and §2's
TRUTHFULNESS caveat extends to it verbatim — the confirm cannot tell "this
composite never answers" from a per-point failure (a bracket that finds no
strict landing, a seam gradient), the sentence is best-effort, and the
one-shot latch is spent by the first such point (the flute station above is
a healthy point that spends `column2`'s latch).

**Exact σ.** Phase 1's loose bounds (`‖M‖_F`, `|det|/σ_max²`) inflate the
search radius by `r/σ_min` and the reported distance by `σ_max` — on a
`scale (3, 1, 0.4)` object the bounds are `σ_max = ‖M‖_F = 3.19` and `σ_min
= |det|/‖M‖_F² = 0.118`, so the search radius inflates 8.47× (true: 2.5×)
and the worst over-read is bounded by `‖M‖_F/0.4 = 7.97×` (§5.2's 7.96× is
the measured value under that bound; true `σ_max/σ_min` = 7.5;
the 26.99 the log prints is the ratio of the two bounds, a bound on a bound,
not a reported-over-true ratio). Phase 3's honest deltas are therefore
search radius 8.47× → 2.5× and worst over-read 7.97× → 7.5×. Phase 3 computes the extreme singular
values of the upper 3×3 by a **one-sided Jacobi SVD on `M`** (rotations
applied to columns of `M` until they are mutually orthogonal; singular values
are the column norms; high relative accuracy for every singular value, which
the eigenvalues of `MᵀM` do not give — forming `MᵀM` squares the condition
number, and a 1e-3..1e3 scale range would leave σ_min with ~1e-4 relative
error), ≤ 30 sweeps with the existing Frobenius/determinant pair as the
fallback if it does not converge, keeping the exact-uniform fast path (which
stays exact and un-nudged, so `m_sigmaExact` keeps its meaning — and the
loose-σ diagnostic that `Object.cpp` prints from `m_sigmaExact == false`
("through the Frobenius/determinant bounds … over-read by at most Nx") is
replaced by a three-state `m_sigmaSource` — `Exact` (fast path), `Jacobi`
(converged, ratio = true `σ_max/σ_min`, 7.5 there) and `Loose` (the sweep
cap hit, Frobenius/det pair, ratio 26.99) — and the print site names the
state, since a converged and a fallen-back object are otherwise
indistinguishable at `!m_sigmaExact`) and the degenerate refusal (which runs BEFORE Jacobi on `|det|`, so `σ_min > 0` holds
after the nudge). Because the chain of inequalities the design rests on must
survive rounding, on the Jacobi path the stored `σ_max` is nudged UP and
`σ_min` DOWN by four ulps.
Reflections need nothing (singular values are those of `|M|`). `Object` exposes
`SigmaMin()/SigmaMax()` for tests, which compare against reference values
written into the test for a rotation, a reflection, a uniform scale,
`(3, 1, 0.4)` and a shear; the first three exercise the fast path, the last
two Jacobi.

**`interior(r)`** — the signed variant, as a second builtin rather than a sign
on `proximity`: `clamp(depth / r, 0, 1)` where `depth` is the largest, over
every OTHER world-visible non-emitter object that CONTAINS the hit point, of
that object's inside-depth LOWER bound (`|SignedDistanceLower|`, world-scaled
by `σ_min`; if the point is inside two overlapping solids, leaving their union
needs at least the larger of the two depths, so the max is still a lower
bound — the under-paint direction, as for `proximity`); **0 = inside no
neighbour**, **1 = at least `r` deep**; neutral 0; world-length radius; `r`
mandatory, no `DynR` twin. Together `proximity` and `interior` cover the
signed distance without a sign convention an author has to remember. Plumbing,
each named because the round-1 review found every one of them missing:
`kFnInterior = 58` (the next free id; the `kFnProximity` assert's "58–59 free"
text is updated), one more named `case` in `CallFunc` (the pin moves once
more, disclosed), `SignalKey.fn = 4` / `eInterior = 4`, `SurfaceSignalInfo::Interior(r)`
beside `Proximity(r)` sharing the L1 helper; `ParseCall`'s `isSignalFn` gains
the id AND its unit diagnostic becomes a three-way (fraction / world length for
`proximity` / world length for `interior`) instead of a binary ternary;
`ExpressionProgram::UsesProximity()` becomes `UsesCrossObject()` at its two
call sites (`ExpressionPainter.h`'s `m_proximityDemand` initialisers), and
`ProximityDemand` keeps its name but is documented as "registered when the
program calls `proximity()` or `interior()`" so the eager snapshot covers an
`interior`-only scene; `kFnInterior = 58` leaves only 59 free before
`CallFuncVec3`'s 60+ band (stated on the assert); the
`ISurfaceSignalProvider.h` warning text that enumerates
`curv/occlusion/thickness/convexity/proximity` gains `interior`; and, as for
`proximity`, an `interior` call lands in `m_sigCalls`, so
`UsesSurfaceSignals()` and `SurfaceSignalDemand` go true for an
`interior`-only program (diagnostic-only, harmless, disclosed);
the query is `IObjectManager::DeepestOtherContainment( ptWorld, self,
maxDepthWorld, outDepth ) → bool` over the same candidate snapshot — a
candidate whose box does not contain the point cannot contain it; the loop
keeps a running MAXIMUM and has no distance-based prune (unlike
`NearestOtherSurface`'s shrinking `best`); the per-object call is
`IObject::SignedDistanceLower`, used only when it returns a negative value. **It
does not log refusals**: every sheet family refuses containment at every point
by design, so a shared latch would print the proximity message for every mesh
and plane in the scene; refusal here is silent and disclosed. **The L1 memo's
cliff**: `kL1Ways = 4` equals the number of signal kinds today, and `interior`
is the fifth, so a body that queries all five would evict round-robin at 0 %
hit rate; `kL1Ways` goes to 8, which puts `Tables` at ~2400 bytes, over the
2048-byte ceiling `ExpressionMemoTest` (g) asserts — the ceiling was a
regression guard, not a budget, and is raised to 4096 with the reason recorded
in the test and in §6.5 of the convexity doc (a 43 kB total across 18
workers). Residual, stated: meshes and every sheet family contribute nothing
to `interior`; a hit point inside a MESH neighbour reads `proximity = 1 −
d_shell/r` (1 only within the shell's `r`, 0 deep inside a large mesh) and
`interior = 0`.

**`add_wear` composition.** The verb gains two parameters, `contact_radius`
(world length, default 0 = off) and `contact_grime` (mask weight, default
0.5). Its candidate gate — clause (c) of the CONDITIONS scan, not the verb —
rejects flat receivers ("a plane, disk, box or patch, where `curv` is 0
everywhere") before any call, so the two flagship contacts (a plank on a plane,
a flange on a box) are outside the verb's candidate set today, and on a
curving receiver the cavity mask's `crevice_raw` (`clamp(−curv·grime +
breakup·fbm, 0, 1)`) is dominated by breakup where `curv ≥ 0`. When
`contact_radius > 0` the term therefore enters as an **additive third mask**:
the prelude gains `def contact_r <param>` (emitted as a retunable `param`,
which is safe because `proximity` has no `DynR` twin) and
`def contact_mask clamp(proximity(contact_r) + breakup_amp*fbm(…), 0, 1)`, and
the two consuming `expr` lines fold `contact_grime*contact_mask` into the SAME
patina/roughness endpoint the crevice mask drives
(`clamp(crevice_mask + contact_grime*contact_mask, 0, 1)` in place of
`crevice_mask`), never relief (automatic — §5.3). The flat-receiver gate is
relaxed WITHOUT touching the scan's existing outputs: the scan keeps
barren-only materials in a SEPARATE list (`wearBarrenCandidates`, with its
own decline reason left as is), and when `contact_radius > 0` BOTH of
`AddWear`'s lookups consult that list — the named-material path checks it
BEFORE the `wearDeclineReasons` refusal (today that refusal fires first and
would turn `add_wear {material: "bench_top", contact_radius: 0.002}` away),
and the bare-call path selects over the union of the two lists, with the
"no material qualifies" message gaining a clause that a flat receiver
qualifies once `contact_radius` is set. A barren pick's `curvGeometryKind`
names the receiver's geometry kind and the success message says it touches a
neighbour rather than that it curves; the bare call ranks the barren list
together with the curved one under `SelectMaterialToWear_`, while
`qualifyingMaterials` keeps reporting the curved count (unchanged field
meaning), and the new clause on the "no material qualifies" message is
APPENDED, because `AgentAddWearTest` matches that message by its opening
substring. The contact mask's breakup is `fbm(P*contact_scale + jitter, …)` with its
OWN `param contact_scale` (the prelude's other two fbm calls are scaled by
`breakup_scale` and `grime_scale` respectively, each consumed by exactly one
mask). On a barren receiver `curv ≡ 0`, so the prelude's `wear_mask` and
`crevice_raw` would collapse to `clamp(breakup_amp·fbm)` — a pure-noise
edge-wear and patina wash over the whole flat face; the barren path
therefore does NOT emit the wear body with its masks zeroed (that leaves
`edge_wear`, `breakup_scale`, `crevice_grime`, `grime_scale`, `cavity_gain`
in the prelude and `edge_desat`, `edge_lift`, `edge_tint`, `rough_polished`
in the consuming lines as live-but-inert sliders, and pays the dead
`occlusion(0.08)` per shade) but a CONTACT body: prelude = `contact_r`,
`contact_scale`, `breakup_amp`, the seed/jitter lines, the patina colour
and `rough_grimy` params, and `contact_mask`; consuming lines = `mix(base,
patina, contact_grime*contact_mask)` and `mix(rough, rough_grimy,
contact_grime*contact_mask)`. Every `param` in a contact body is read by
an expression that reaches a consuming line; the contact term is the ONLY
mask that paints there, and the success message says so. The design-note
path keeps advertising from the curved list only (`c.addWearName` comes
from `SelectMaterialToWear_(wearCandidateMaterials)`), so with
`contact_radius > 0` the verb's bare pick MAY differ from the note's — the
note advertises a bare call — and the "must be one function" comment at
the verb is updated to say so. The descriptor text states the unit and
that the author chooses the radius from the scene's feature sizes.
`contact_radius 0` is byte-identical to today's output: the scan's existing
lists, counts and messages are unchanged, and the prelude is a built string
that omits the contact lines at 0. `WearBodyReadsGeometrySignals_` gains `"interior"`
(it lists `proximity` today; `interior` is NOT yet there). Surfaces: the
`props.set` block and `desc` in `AgentMcpAdapter.cpp`, `kToolDefs`' `add_wear`
entry in `AgentChatCodecs.cpp` (the two texts must stay semantically
identical), the arg decode and header comment in `AgentRpc.cpp`, the `AddWear`
signature and `BuildWearMaskPreludeText` plumbing in `AgentSession.{h,cpp}`,
`tests/AgentAddWearTest.cpp`. The tool COUNTS do not move (43 MCP / 38 chat,
asserted in `AgentMcpStdioSmokeTest` and `AgentChatLoopTest`).

**Not in Phase 3, still gated on a scene that needs it:** the
emissive-decorative-object exclusion (§10), a per-object opt-out, and planes
as half-space CSG operands (the interval algebra's acceptance of a plane
operand is a separate question; here a plane is a sheet and an
intersection/subtraction with one refuses).

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
term is now real. `add_wear` is not extended in Phase 1; Phase 3 gives it
`contact_radius` (§5.6), with the author still choosing the radius from the
scene's feature sizes.

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
  its buried tip; its head rim nearly rests (§1) — so the stations are: (1) the plank beside the
  **head rim's** near-touch — the head is a 7.5 mm-radius disc, its rim a
  circle; at 1 px (0.4 mm) outside the rim ≥ 0.9, 0 at ≥ 8 mm; (2) the plank
  under the **shank mid-way**, where the underside floats ≈ 1.5 mm: the model
  gives `1 − 1.5/2 = 0.25`, gate 0.1–0.4 — this is the discriminating check,
  because an AO would read the shank's shadow as contact and the distance
  reads the gap; (3) the plank at the **head end of the shank**, floating
  ≈ 3.5 mm: 0; (4) the plank beside the **tip**, buried in a furrow: at 1 px
  ≥ 0.9 and 0 at ≥ 8 mm (model with `ρ ≈ 1 mm`: 0.96 at 0.4 mm, 0 at 4 mm —
  §8.2 measures the tip as a furrow of `ρ ≈ 1.2 mm` sunk 0.55 mm, where the
  distance instead grows ≈ 0.59 mm per mm of lateral offset, which is why
  station 4 reads 0.882 and FAILS this gate); 0 on the
  open plank. Phase 1 first measures the three underside gaps with the signal
  itself and records them in the scene header (wave 2 corrects the header's
  prose to the tip-buried / rim-nearly-touching picture); the scene is NOT
  re-pitched. Cost ≤ 1.15 × 16.33 s;
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

**Phase 3 — signed lower bounds, CSG composites, exact σ, `interior(r)`,
`add_wear` (design in §5.6).** Gate:

- signed lower bounds: for sphere/box/capped cylinder/torus,
  `SignedDistanceLower` equals `+distance` outside and `−depth` inside within
  1e-9 on scene C fixtures extended with interior probe points; the
  ellipsoid's sign exact and its magnitude `dUnit × min(a,b,c)` (≤ true
  distance, recorded); SDF sign exact with `Map` inside; every sheet family
  refuses the signed query and still answers the unsigned one; under a
  `(3, 1, 0.4)` transform the lower bound is ≤ and the unsigned answer ≥ a
  brute-force distance to the scaled solid, both recorded;
- CSG: a union of two spheres answers `min` within 1e-9 outside (both operands
  exact); a union with a mesh operand answers the sphere's or the mesh's
  unsigned distance (no refusal); an intersection and a subtraction answer
  `lower ≤ reported ≤ reference + gap_max` against a grid search of the
  composite's SOLID by strict operand membership on the operands' EXACT
  signed distances (`d_A < 0 ∧ d_B < 0` / `d_A < 0 ∧ d_B > 0` — never
  `f ≤ 0`, which contains the phantom touching set) with `gap_max` recorded
  and the grid spacing stated, and for a composite of EXACT operands
  `|reported − reference| ≤ τ` (the boundary arm fired; `τ = 1e-12 ×` the
  local diagonal) at the radial station of a cylinder-minus-box where
  `sqrt(0.0625) = 0.25` makes `f_A` a true 0, and `reported − reference ≤ ε`
  with ε printed where a bound operand is reached; a subtracted PLANE
  operand makes the composite refuse (no exactness flag on a sheet); a
  nested intersection reports `exact = false` to its parent; a TANGENT pair
  (a box whose face is tangent to a cylinder, the flute case) from a station
  outside the tangent face either refuses or reports ≥ the true corner
  distance, never the tangency; a nested intersection inside a subtraction
  (beside the union-in-subtraction case) composes; a point inside operand A of an
  intersection but outside B reads a positive distance; a point inside the
  composite reads 0; a nested composite (a union inside a subtraction)
  composes; a TRANSFORMED composite (`position`/`orientation` on the
  `csg_object`, untransformed operands — the `glass_pavilion` column shape)
  answers within 1e-9 of the same composite built with the transform folded
  into the operands; an intersection or subtraction with a sheet operand
  refuses; the operands never count separately (unchanged);
- showcase: `glass_pavilion`'s fluted `subtraction` column `column2` stands
  UPRIGHT on its cap `cap2` (a 0.7 × 0.15 × 0.7 box whose top is y = 0.175;
  the column, radius 0.25, spans y ∈ [0, 5]) — the floor beneath is hidden by
  the cap out to ~10 cm, so the receiver is the CAP'S TOP FACE. An upright wall
  gives `d(s) = s`, so `proximity(0.02)` on the cap top at 1 / 2 / 4 cm from
  the column's nominal wall predicts 0.5 / 0 / 0, each measured within 0.05;
  the three stations sit along the composite's local +x axis, 90° from the
  slot wedge (half-angle `asin(0.04/0.25)` = 9.21°, inside which the radial
  descent lands in the slot and the query refuses, reading 0); the un-cut
  wall is the exact cylinder, so the boundary arm fires and the 1 cm
  prediction is 0.5 within τ, with `0.5 − ε/r` = 0.4874 (ε = 2.52e-4) the
  floor if the probe steps instead — either way inside 0.05; and one station on the
  cap top 1 cm outside a FLUTE's tangent face (local `(0, ·, 0.26)`), where
  the nearest real surface is the slot-wall/cylinder corner at 4.21 cm:
  `proximity(0.02)` there must read 0 (a phantom landing would read 0.5),
  and a direct `NearestOtherSurface` call at radius 0.1 REFUSES — always, by
  the cached-gradient walk above — which is asserted, with the recomputed
  4.21 cm recorded beside it as the true distance the refusal under-paints. Harness-only: the cap and floor bind
  checker/uniform painters, not expression painters, so the beauty crop is a
  probe-albedo render of a scene COPY (raw `proximity(0.02)` on the cap top —
  `marble_col` is bound to the ceiling, the pedestal, all eight capitals and
  every column operand — 18 objects — so the copy paints all of them),
  judged honestly, and the tracked scene is not
  edited;
- exact σ: `SigmaMin()/SigmaMax()` within 1e-12 of the written reference on
  the rotation, reflection and uniform scale (fast path) and within 1e-9 on
  `(3, 1, 0.4)` and the shear (Jacobi); the `(3, 1, 0.4)` object's search-
  radius inflation falls from 8.47× to `1/σ_min = 2.5×`; its unsigned answer
  is ≥ and its lower bound ≤ a brute-force distance to the scaled sphere, with
  the over-report factor recorded (exactness is NOT claimed: `×σ_max` remains
  an upper bound attained only along the top singular vector); the `Loose`
  state is reached by a unit test that calls the σ routine with a zero-sweep
  budget and checks the printed state name;
- `interior(r)`: 0 on every scene-C probe that lies OUTSIDE its neighbours;
  on the six existing interpenetration probes (box, sphere, capped cylinder,
  torus, ellipsoid, SDF centres — depths 1, 1, 1, 0.5, 1, 1) `min(depth/r, 1)`
  within 1e-9 of the closed form, and exactly 1 where `r ≤` the depth — the
  ellipsoid row is a lower bound that is tight only at the centre, so its
  probe stays at the centre and the test says so; inside two overlapping spheres the
  larger depth; a mesh neighbour contributes 0; a computed radius is accepted;
  the parse diagnostic names a world length for `interior`; an `interior`-only
  scene builds the eager snapshot (demand covers it); the memo keys/L1 rows
  extended (`fn = 4`), red-proved; `kL1Ways = 8` with the raised TLS ceiling
  asserted and the bytes printed; `TextureExpressionVMTest` 846/846;
- `add_wear` with `contact_radius`: refuses on a body that already reads
  `proximity`/`interior`; with a radius, the written body calls
  `proximity(contact_r)` exactly once as an additive mask on the crevice
  endpoint, a flat box receiver is accepted from the barren list, and the
  scene derives; a barren pick's emitted body is the CONTACT body: it
  contains no `curv` and no `occlusion` term, and every `param` it declares
  is referenced at least once outside its own declaration (no inert
  slider), asserted by name over the emitted text;
  `contact_radius 0` is byte-identical to today's output INCLUDING the
  conditions scan's counts, kinds and decline messages;
  `AgentAddWearTest` extended; the MCP and chat tool counts unchanged
  (43 / 38).

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
§6.5. Batch 3, the only batch that ran the untouched pre-wave-2 file itself,
measures it at **16.763–17.758 s** on a quiet machine today (best run 2.6 %
above 16.33 s, mean 5.7 % above). Batch 1 did not run the pristine file at
all — its baseline was the no-prox copy, at 18.096 s — but batch 3 shows the
two baselines agree within 0.3 % (line below), so 18.096 s stands as an
equivalent reading of the same unmodified-scene cost. Against 16.33 s
literally the shipped scene is 1.18–1.25× depending on the batch, and the
gate FAILS in its absolute form; against a
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
`AgentSession`. What the code says is worth recording anyway, because at the
time this was checked it contradicted the MCP tool's own description. That
description USED TO claim draft "IGNORES the scene's authored materials and
lighting entirely" — but
`InteractiveMaterialPreviewShader::MaterialAlbedo` calls
`bsdf->albedo( ri.geometric )`, and `GGXBRDF::albedo` calls
`pDiffuse->GetColor( ri )`, which runs the expression and therefore the
`proximity` query. So draft DOES pay it: once per primary hit at 1 spp
(~3 × 10⁵ queries for a 640 × 480 frame) against the production frame's
8.2 × 10⁷ — about 0.4 % of the production query count. Not measured as wall
clock; stated as the bound the code supports. **Fixed in this fix round**:
every one of those descriptor strings (`AgentMcpAdapter.cpp`,
`AgentChatCodecs.cpp`, `AgentSession.h`/`.cpp`, `AgentRpc.h`) said draft
ignores LIGHTING only and evaluates each material's diffuse albedo — see
§10. **Refined the same day, same-session follow-up**: "diffuse albedo" was
itself an understatement — `InteractiveMaterialPreviewShader::PreviewPel`
also evaluates the material's full `bsdf->value()` (not just `albedo()`)
against three fixed key/fill/top studio lights, so a material's specular/
roughness response, not only its base colour, is visible in draft. Every
descriptor string above now reads "shades with the material's own BSDF
under a fixed studio rig" rather than "evaluates diffuse albedo", and
additionally states what draft does NOT show (emission, transmission/
refraction, subsurface scattering — an emissive/transmissive/volumetric
material only shrinks the ambient-occlusion weight, it is never actually
traced) and confirms relief/bump/normal modifiers DO apply to the draft
shading normal (`RayCaster::CastRay` runs `ri.pModifier->Modify` before
`Shade`, unconditionally on the pipeline).

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

### 8.3 Phase 2 — the mesh family, the TLAS point query, scene D and Sponza

Phase 2 shipped the mesh row of §5.2's table, the scene-D fixture and its
test, and — because the Sponza cost gate FAILED on the flat candidate scan —
the TLAS point query that §5.2 had held in reserve. `ExpressionEval.h` is
**byte-identical to the branch point** (`d722d4d6`): Phase 2 touches no VM.

**What was built.** `BVH<Element>::ClosestPointDistance`
([BVH.h](../src/Library/Acceleration/BVH.h)) is a bounded-radius closest-point
traversal: it visits the two children of each node nearest-AABB-first, prunes
any node whose point-to-AABB distance is ≥ the running best, and evaluates a
caller-supplied exact primitive distance at the leaves. It uses the BVH2
`nodes` array, **not** the BVH4 SoA — the wide layout exists to batch four
ray-vs-AABB *slab* tests in one SIMD op, and a point query has no ray, no slab
test and no direction to sort by; its win is a sorted nearest-first descent,
which a binary node gives by sorting two and a 4-wide node would need a partial
sort of four to reproduce. `nodes` is always populated when `nodes4` is, since
the wide layout is derived from it.

*Why it is identical to brute force and not merely close:* a node is skipped
only when the point is at least `best` from that node's AABB, and every
primitive the node owns lies inside that AABB, so none of them could have
lowered `best`. The boxes are `float` and rounded OUTWARD at build, which can
only make the box distance SMALLER than the true one — i.e. can only ADMIT a
node the pruning could have skipped. Sound in the only direction that matters.
A tie (two primitives at bit-equal distance) is the sole freedom, and it does
not move the reported value.

`TriangleMeshGeometryIndexed::PointTriangleDistance` is public and static so
the test drives the same formula for its reference. Degenerate (zero-area)
triangles take an explicit three-edge fallback instead of the barycentric
interior branch, whose denominator is the vanishing doubled area: a NaN there
would lose every comparison in the traversal and vanish as an **invisible hole
in the mesh** rather than fail loudly.

`DisplacedGeometry::DistanceToSurface` forwards to the baked mesh after
`Realize()` — which the ray forwarders deliberately do not call, because they
are the hot path and pay for a bake `RayCaster::AttachScene` already did, while
this query is rare, heavy, and reachable from a test or tool that never went
through AttachScene. On an already-realized geometry `Realize()` is one acquire
load and returns before its own render-freeze assert.

**Where this document was wrong, or under-specified.**

1. **The mesh row is SHEETS, and §5.2's table did not say so.** Every solid
   family Phase 1 shipped clamps a signed field at zero, so a point inside
   reads 0 and interpenetration is contact (§2). A triangle mesh has no inside
   test *at all* — it may be open, non-manifold or self-intersecting, and
   nothing in the class distinguishes "inside the bunny" from "in the air
   beside it". So there is **no interpenetration clamp** on this row, and a
   point inside a closed mesh reports its honest distance to the nearest
   triangle. That is the safe direction (over-report → under-paint), and the
   table now says it.
2. **The TLAS upgrade is not the walk §5.2 described.** §5.2 said "walk
   `nodes4[]` for boxes containing the point expanded by r". A containment walk
   collects candidates but neither prunes by the running best nor visits them
   in a useful order; the implementation reuses `ClosestPointDistance` over the
   BVH2 nodes instead, which does both.
3. **The TLAS carries a contract shift, not only a speed-up.** The AABB
   snapshot's count check (§8.1) rebuilds it when an object was ADDED without
   an invalidate; the TLAS has no such check. On a TLAS-backed scene a
   proximity query is now exactly as stale as the RENDER — `IntersectRay` walks
   that same tree, so an object invisible to this query is equally invisible to
   the picture. That is *closer* to "the signal measures the scene you are
   looking at" than the previous state, where proximity could see a neighbour
   the frame did not. The case §8.1's check was actually written for — an add
   on a scene of four or fewer objects, where the linear `IntersectRay` loop
   WOULD draw the new object — keeps the flat scan and keeps the check, and
   `ProximitySignalTest` (g) still pins it.
4. **A scene-language trap found while authoring scene D, and NOT fixed here.**
   `standard_object`'s `scale` is a `DoubleVec3`. Writing it with ONE number
   (`scale 0.35`) derives to a **degenerate transform with no diagnostic** —
   the object silently disappears from the render and refuses every proximity
   query (`Object::DistanceToSurface` rejects `σ_min ≤ 0`). Verified by
   re-introducing the edit: `MeshClosestPointTest`'s mesh-on-mesh station goes
   1.0 → 0. This is a parser gap outside Phase 2's scope; recorded in §10.

**(a) The differential — `MeshClosestPointTest` (a), 56 checks total in the
suite.** Traversal vs brute force over EVERY triangle, under the SAME
point-triangle formula, at three radii per point (unbounded, the mesh's box
diagonal, and 2 % of it, so refusals are compared as well as answers):

| mesh | tris | points | answered | refused-both | bit-identical | value mismatch | yes/no mismatch | max abs diff |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| tessellated sphere (engine `TessellateToMesh`, detail 48) | 4,608 | 40,000 | 27,244 | 12,756 | 27,244 | 0 | 0 | **0** |
| `bunny.risemesh` | 69,451 | 40,000 | 15,643 | 24,357 | 15,643 | 0 | 0 | **0** |
| two-triangle sliver | 2 | 15,000 | 10,194 | 4,806 | 10,194 | 0 | 0 | **0** |
| degenerate (collinear / coincident) | 4 | 15,000 | 10,015 | 4,985 | 10,015 | 0 | 0 | **0** |
| **total** | | **110,000** | **63,096** | **46,904** | **63,096** | **0** | **0** | **0** |

**(b) The closed form.** A tessellated sphere is an INSCRIBED polyhedron, so
for a point at radius `t > R` the exact mesh distance is bracketed with no
appeal to a chord-error formula: `t − R ≤ d ≤ t − ρ`, where `ρ` is the
polyhedron's inradius (the ray from the origin through `p` meets the mesh at
some `q` with `|q| ≥ ρ`). `ρ` is **measured from the fixture** with the same
point-triangle formula — it is exactly the distance from the origin to the mesh
— so the bound is a property of the mesh that was built, not one assumed.

| quantity | value |
|---|---:|
| R | 1 |
| measured inradius ρ (detail 48) | 0.997327 |
| **tessellation bound `R − ρ`** | **0.00267306** |
| worst over-report vs `\|p\| − R` across 18 probes | **0.000505703** |

Every probe lands in `[t − R, t − R + (R − ρ)]`; the worst is inside the bound
and is NOT zero, so the bound is doing work rather than being vacuous.

**(c) The radius cut-off** is driven at the exact boundary from both sides.
`maxDist` exactly equal to the distance REFUSES; one ulp above answers with the
same value the unbounded call gave; one ulp below refuses — the half-open
`[0, maxDist)` range §10 states. Zero and negative radii refuse.

**(d) The displaced forward.** A `DisplacedGeometry` over a sphere of R = 1
with a constant height and `disp_scale = 0.25`, probed at radius 5, answers
**3.75** (the displaced surface at R + h = 1.25) and not 4 (the smooth base).
The 0.25 separation is the whole error an author would eat if this forwarded to
the base.

**(e) RAW still refuses**, and the check has teeth: the identical triangle in
the INDEXED family answers exactly 1.0 from the same probe point.

**(f) Concurrency.** 8 threads × 4,000 queries against one bunny mesh: **0
mismatches** against the serial reference, bit for bit.

**(h) The two candidate sources agree.** Two managers over ONE SHARED object
set (10 spheres + a tessellated-sphere mesh, and since review round 1 an
infinite plane, an emitter, a world-invisible sphere and an anisotropically
scaled sphere), differing only in `bUseBSPtree`,
so one walks the TLAS and the other cannot build one; 6,000 probes at three
radii with `self` rotating through the set: **3,018 agreed-answered, 2,982
agreed-far, 0 mismatches** (2,484 / 3,516 before the four families joined). Sharing the objects rather than rebuilding them is
what makes the comparison exact — there is no second construction for a float
to differ in.

**Scene D — `scenes/Tests/Signals/proximity_mesh_contact.RISEscene`.**
Placement is OWNED by the test, which loads both meshes through the same
deserialize the scene's `risemesh_geometry` chunks use and re-derives every
number at 1e-6:

| quantity | value | method |
|---|---:|---|
| bunny bbox floor / lowest vertex y | 0.0329874 | equal to 1e-15 — the box is computed in double from the vertex array, and the test checks that too |
| bunny `position.y` | **−0.0329874** | `−(bbox floor)`, putting the lowest vertex exactly on the plane (residual 0.0) |
| bunny contact footprint (world x, z) | (−0.053835, 0.0179508) | the lowest VERTEX, a foot — not the bbox midpoint |
| dragon scale | 0.35 | |
| dragon `position` | **(−0.0318315, 0.135862595, −0.014760295)** | its lowest vertex onto the bunny's highest vertex — vertex-onto-vertex, **not** bbox-onto-bbox, because the latter only makes the two BOXES touch and leaves the SURFACES an unknown distance apart |

Gate, at `proximity(0.02)`:

| station | measured | gate | verdict |
|---|---:|---|---|
| plane 1 mm outside the footprint, +X / −X / +Z / −Z | 0.9519 / 0.9740 / 0.9865 / 0.9913 | ≥ 0.9 | **PASS** |
| plane 2 mm outside the footprint (worst of four) | **0.9037** | ≥ 0.9 | **PASS** (narrowly, and reported as such) |
| plane under the `rect_light` panel at (0.6, 0, 0) | **0** | exactly 0 | **PASS** — would read 0.9 if emitters counted |
| plane under the `casts_shadows FALSE` sphere at (−0.6, 0, 0) | **0.85** | closed form 1 − 0.003/0.02 = 0.85 at 1e-6 | **PASS** |
| open plane at (1.8, 0, 1.8) | 0 | 0 | **PASS** |
| mesh-on-mesh at the shared contact vertex, `self` = bunny | **1.0** | ≥ 0.999 | **PASS** |
| …5 mm below it, `self` = bunny | **0.75** | closed form 1 − 0.005/0.02 at 1e-6 | **PASS** |

*The mesh-on-mesh station asks with the BUNNY as `self`, and that is not
cosmetic.* The shared contact point lies on BOTH surfaces, so asking with the
dragon as `self` reads 1 even when the dragon is absent — and it did: the first
draft wrote `scale 0.35` into the per-axis slot (trap 4 above), which made the
dragon degenerate and invisible, and that station still read 1.0. With the
bunny excluded, a missing dragon reads the plane 0.135 m below and the station
goes to 0.

*The render* (`$OUT/sceneD_proximity_mesh_contact.png`) is the acceptance
picture. The floor's grime is **tinted toward brown rather than merely
darkened**, and that is a legibility decision made after looking at the frame:
a multiply-down term produces a band under the bunny that is visually
indistinguishable from the key light's own contact shadow — precisely the
confusion this signal exists to resolve (§2), and a picture where the two look
alike proves nothing to a reader. Honest read of the frame: under the
`casts_shadows FALSE` sphere, where no shadow exists at all, the grime is an
unambiguous soft warm ellipse; at the bunny's feet it is a thin warm band
hugging the footprint, legible but adjacent to (and partly overlapped by) the
neutral cast shadow.

**Scene E — Sponza, the cost gate.** `640×360×32`, the §8.1 protocol, the query
forced at EVERY hit through a temporary env-gated hook in the PT integrator's
surface-hit path (`RISE_PROX_FORCE`, a `volatile` sink so nothing is
dead-code-eliminated; removed before the final commit, and
`grep -rn "RISE_PROX_FORCE" src` is empty). The asset lives outside the repo,
so the tracked scene was copied with its Windows `file` path rewritten.

*Queries per frame and per sample:* **27,000,000** forced queries per frame —
matching §8.1's "~27 M hits per Sponza frame" estimate — which over
`640×360×32 = 7,372,800` camera samples is **3.66 queries per camera sample**.

*The flat scan, and why it failed.* Per-query counters at the 25 M mark:
**405.0 entries walked, 4.6426 candidates past the box test, 8,235,325 of 25 M
queries answered (32.9 %)** — 87 box tests for every distance call. Interleaved
runs, two warm-ups first (18.389 s base, 24.948 s / 24.571 s forced), machine
confirmed idle with `pgrep -fl "make|clang|bin/rise"`:

| pair | base | forced | ratio | scan-only |
|---|---:|---:|---:|---:|
| p1 | 19.092 s | 24.817 s | 1.300× | 24.534 s |
| p2 | 19.859 s | 24.501 s | 1.234× | 23.971 s |
| p3 | 19.045 s | 24.511 s | 1.287× | 23.545 s |
| **mean** | **19.332 s** | **24.610 s** | **1.273×** | **24.017 s** |

**1.273× against a ≤ 1.25× gate: FAIL.** The scan-only column is a third
variant that walks the snapshot and runs the box test but never asks a
geometry, so the split is measured rather than argued:
`(24.017 − 19.332) / (24.610 − 19.332)` = **88.8 % of the added cost is the
flat scan**, 11.2 % the per-family distance work.

*The TLAS point query, and why it was kept.* Same protocol, two fresh
warm-ups (18.610 s base, 21.577 s forced):

| pair | base | forced | ratio |
|---|---:|---:|---:|
| q1 | 18.946 s | 20.764 s | 1.096× |
| q2 | 18.823 s | 19.955 s | 1.060× |
| q3 | 19.957 s | 20.446 s | 1.025× |
| q4 | 19.261 s | 20.950 s | 1.088× |
| **mean** | **19.247 s** | **20.529 s** | **1.067×** |

**1.067× against ≤ 1.25×: PASS.** Kept on that measurement; had it not measured
faster it would have been reverted, and this section would have said so. The
counters after the change: **0 entries flat-scanned, 11.0785 candidates
evaluated per query** — *more* geometry calls than the flat scan's 4.64,
because a BVH leaf holds up to four objects and the traversal evaluates all of
a leaf's occupants once the leaf's own box is within the running best, with no
per-object box test in between. That trade is strongly positive here (405 box
tests traded for ~6 extra distance calls, most of which refuse immediately),
and it is the obvious place to look first if a future scene regresses.

**Post per-element pre-test re-measurement (review round 1, item 2).** The
`useElementBoxTest` per-element AABB pre-test (BVH.h, gated on TLAS leaves
only) was added to cut the 11.0785 candidates/query figure above by skipping a
leaf's occupants whose own box is already past the running best. Re-measured
with the SAME `RISE_PROX_FORCE` hook and protocol (removed again before this
commit; `grep -rn "RISE_PROX_FORCE" src` empty at commit time), two warm-ups
(20.069 s / 20.434 s base) then **five** interleaved base/forced pairs — more
than the ≥ 3 the protocol asks for, because pair 2 read anomalously (forced
*faster* than base) and a third pair alone would not have shown whether that
was noise:

| pair | base | forced | ratio |
|---|---:|---:|---:|
| p1 | 19.564 s | 21.502 s | 1.099× |
| p2 | 20.503 s | 20.373 s | 0.994× |
| p3 | 19.683 s | 21.638 s | 1.099× |
| p4 | 19.677 s | 22.476 s | 1.142× |
| p5 | 19.531 s | 22.229 s | 1.138× |
| **mean** | **19.792 s** | **21.644 s** | **1.094×** (mean of per-pair ratios; 1.094× on the pooled means, 21.6436/19.7916) |

**1.094× against ≤ 1.25×: PASS.** The counter after the pre-test: **5.617
candidates evaluated per query** (5.6172 / 5.6166 / 5.6167 / 5.6172 / 5.6174
across the five forced runs), essentially halving the 11.0785 figure above,
exactly as the pre-test is supposed to do. **The wall-clock ratio did not
improve alongside it** — 1.094× here against 1.067× before the pre-test
landed, both comfortably inside the gate but the pre-test's candidate-count
win is not visibly a wall-clock win on this scene. The likely reason is
`useElementBoxTest`'s own stated cost: `Object::getBoundingBox()` "RE-TRANSFORMS
all 8 local-box corners through the object's full world matrix on every call
… not cached" (BVH.h), so on Sponza — where most TLAS leaf occupants a query
reaches are already close enough to answer rather than to refuse — the box
test's own per-candidate cost is competing with, not clearly beating, the
mesh-recursion cost it exists to skip. Both measurements pass the ≤ 1.25× gate
with headroom, so this is recorded as an open, honest finding rather than a
regression to fix: the pre-test is a legitimate reduction in *distance calls*,
which is the metric its own commit message named, and this document no longer
overclaims a wall-clock win it did not clearly produce.

**The mesh-path question is UNTESTED, not measured.** BVH.h's
`useElementBoxTest` doc comment reasons (rather than measures) that the
pre-test is pure overhead on the mesh family's own traversal — a single
triangle's `primDist` is already cheap, so paying a fresh
`GetElementBoundingBox` (three vertex reads + an `Include`) ahead of it has
nothing to win back. That reasoning was not exercised with a timing run in
this round; the comment is worded as reasoning rather than as a Sponza-style
measurement, and any future contributor testing `useElementBoxTest = true` for
`TriangleMeshGeometryIndexed::DistanceToSurface` should record a real number
rather than trust the inference.

*The seam gate.* Measured with a throwaway probe tool (deleted with the hooks)
that finds the floor and the nearest wall by ray casting, so the coordinates
are the asset's rather than guessed. The floor point is (−6, ≈0, 0); the
nearest wall is the −X one at 5.130 m. **The wall must be probed at floor
level, not at a convenient height** — a first pass cast the wall ray at 5 cm and
put its "2 cm" station at a true 2.16 cm, reading 0.4600 and appearing to fail;
the signal was exactly right (0.4600 = 1 − 0.0216/0.04) and the probe was not.

| step back from the wall | true distance | `proximity(0.04)` | gate |
|---:|---:|---:|---|
| 1 cm | 0.0100 | **0.75** | — |
| 2 cm | 0.0200 | **0.50** | ≥ 0.5 → **PASS**, exactly at it |
| 4 cm | ≥ 0.04 (refused) | **0** | — the half-open cut-off (§10) |
| 8 cm | refused | 0 | — |

*The beauty check.* A second temporary hook (`RISE_PROX_DUST`, removed with the
rest) multiplied throughput by `1 − 0.6·proximity(0.04)` at every hit, against
an unmodified control. Honest read: at 4 cm on a scene measured in metres the
signal fires along the paving-slab joints, the column plinth/floor junctions
and the arch springings, and in a side-by-side 4× crop of a column base the
dust frame is visibly darker at exactly those lines. But the frame is also
darker **overall**, and that is the hook rather than the signal — multiplying
*throughput* at every vertex darkens indirect light from any path that touched
a near-contact point anywhere, so the effect spreads through GI instead of
staying on the seam an albedo term would draw. A difference image is
**not** usable evidence here and is reported as such: the two renders draw
independent random sequences (path guiding is on in this scene, and renders
seed from the wall clock), so the difference is dominated by sampling noise,
not by dust. Frames and crops are in `$OUT/sponzaE_*.png`.

**`weathered_workbench`'s Phase-1 cost gate re-measured on the Phase-2 TLAS
path (review round 1, item 1).** §8.2's number (1.075×/1.092×) was measured
before the TLAS point query existed, on the flat-scan-always code of that
era. `weathered_workbench.RISEscene` has 8 objects, above `nMaxObjectsPerNode`
(4), so on this branch its proximity query goes through
`BVH::ClosestPointDistance` exactly as Sponza's does — a different code path
than the one §8.2 measured, and that number is stale for it.

Re-measured with §8.2's own protocol: two baselines (a "no-prox" copy with
both `def contact proximity(0.02)` lines replaced by `def contact 0.0`,
isolating just the query's cost; and "pristine", the file at `9ae731a9`, the
commit immediately before the flange-grime feature landed), interleaved as
triples, 640 × 480 × 12, two warm-ups (2.449 s / 2.454 s on the shipped
scene) then **five** interleaved triples on a machine confirmed idle before
each (`ps aux | grep -E "bin/rise|make|clang|cc1plus"` empty):

| triple | pristine | no-prox | shipped | shipped/no-prox | shipped/pristine |
|---|---:|---:|---:|---:|---:|
| t1 | 2.247 s | 2.452 s | 2.482 s | 1.012× | 1.105× |
| t2 | 2.287 s | 2.473 s | 2.515 s | 1.017× | 1.100× |
| t3 | 2.314 s | 2.502 s | 2.525 s | 1.009× | 1.091× |
| t4 | 2.345 s | 2.524 s | 2.547 s | 1.009× | 1.086× |
| t5 | 2.347 s | 2.553 s | 2.555 s | 1.001× | 1.089× |
| **mean** | **2.308 s** | **2.501 s** | **2.525 s** | **1.010×** (mean of per-triple ratios) | **1.094×** (mean of per-triple ratios) |

**1.010× against the no-prox control, 1.094× against pristine — PASS against
the ≤ 1.10 gate on both baselines**, though the two disagree far more than
§8.2's original ~1.6% baseline gap did (theirs: no-prox and pristine agreed
within 0.3%; this run's pristine-to-no-prox gap is ~8%). That gap is NOT the
proximity query — the no-prox copy isolates exactly that cost and reads
1.010×, comfortably inside the gate with room to spare. The gap is scene
EVOLUTION between `9ae731a9` (pristine) and the shipped file that has nothing
to do with this signal: the vise geometry work (`e3bef58c`, cast-iron field
sized to a measured footprint) and other tuning commits in between added real
cost of their own. Reported for completeness because the protocol asks for
both baselines, but **the no-prox ratio (1.010×) is the number that actually
answers "what does `proximity()` cost on this scene through the TLAS path"**,
and it says: negligible. A monotonic upward drift across all three arms
triple-to-triple (2.247 s → 2.347 s pristine, similarly for the other two) is
visible in the table and is machine warm-up / thermal, not a measurement
artifact specific to one arm — it moves all three columns together and
the RATIOS stay flat.

**The once-per-mesh refusal confirm's cost (review round 1, item 6b) is a
REASONED BOUND, not a fresh measurement** -- the `RISE_PROX_FORCE` hook that
would make a Sponza number for this cheap was already removed by the time this
question was asked, and re-adding it solely for this bound was not judged
worth the overhead. `ObjectManager::ProximityCandidateDistance`'s comment used
to claim the confirm (an unbounded `BVH::ClosestPointDistance` call, run once
per mesh object via the one-shot latch) "visits the whole tree", which
OVERSTATES it: the traversal seeds `best` at `maxDist` and prunes any subtree
whose box-to-point distance is `>= best`; with `best` seeded at infinity, the
first leaf primitive the nearest-first descent reaches collapses `best` to a
real finite number, after which every later node is pruned exactly as a
normally-bounded query would prune it. So the unbounded confirm costs about
the same as one ORDINARY closest-point query against that mesh, not a
full-tree scan -- bounded by the BVH's own build quality, not by this call
being unbounded. Comment corrected at the site (ObjectManager.cpp, the
`ProximityCandidateDistance` refusal-confirm block).

**Gate summary.**

| Phase-2 gate | verdict |
|---|---|
| warning-free clean `make -C build/make/rise -j8 all` | PASS |
| closest point identical to brute force, ≥ 10⁵ random points | PASS (110,000 points, 63,096 answered, 0 mismatches, max abs diff exactly 0) |
| within the closed form on a tessellated sphere | PASS — bracketed by the mesh's own measured inradius; worst over-report 5.06e-4 against a 2.67e-3 bound. **NOT** the literal "within 1e-9": an inscribed polyhedron is not the sphere, and quoting 1e-9 would have meant testing the tessellation, not the query |
| scene D placed by the test, asserted at 1e-6 | PASS (4 placement checks; residual 0.0 on the bunny's contact vertex) |
| scene D ≥ 0.9 within 2 mm of the bunny's footprint | PASS (0.9037 worst of four, narrowly) |
| scene D reads 0 under the light panel | PASS (exactly 0) |
| Sponza ≤ 1.25× baseline with the query forced at every hit | **PASS at 1.067× — after the TLAS point query. FAILED at 1.273× on the flat scan, which is why the upgrade shipped** |
| Sponza floor ≥ 0.5 within 2 cm of a wall at `proximity(0.04)` | PASS (0.50, exactly at the gate) |
| `MeshClosestPointTest` | 65 passed, 0 failed (56 before review round 1's four (h) families and three station checks; 63 before review round 2's TLAS-ran assertions) |
| `ProximitySignalTest` | 123 passed, 0 failed (107 before review round 1's (g2) TLAS staleness contract; 124 before review round 2 dropped its tautological third check) |
| `ProximityInvalidationTest` | 25 passed, 0 failed |
| `ExpressionMemoTest` | 196 passed, 0 failed |
| `TextureExpressionVMTest` | 846 passed, 0 failed (unmoved) |
| `MeshSignalBakeTest` | 110 passed, 0 failed |
| `DisplacedGeometryTest` / `GeometryUVRoundtripTest` | PASS |
| `CstDeriveGoldenTest` | PASS; regenerated golden differs by exactly ONE added row (scene D) |
| `SourceHygieneTest` | 165 passed, 0 failed |
| every suite naming `TriangleMeshGeometryIndexed` / `DisplacedGeometry` / `BVH` | `grep -rlE "TriangleMeshGeometryIndexed\|DisplacedGeometry\|BVH" tests/*.cpp` -- **47** suites (re-run 2026-09-09, review round 1 item 6a; the original "46" was off by one, not re-derived at the time), all green, run one at a time |
| `ExpressionEval.h` byte-identical to `d722d4d6` | PASS (`git diff` empty) |
| temporary hooks removed | PASS (`grep -rn "RISE_PROX_FORCE" src` empty; probe tool deleted) |


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
- Phase 2 (SHIPPED, §8.3): `tests/MeshClosestPointTest.cpp`, 56 checks --
  the differential against brute force over every triangle (110,000 points,
  0 mismatches); the tessellated sphere against `|p| - R` inside a bound
  measured from the mesh; the `maxDist` cut-off at the exact boundary from
  both sides; the `displaced_geometry` forward; the RAW family still
  refusing where its indexed twin answers; 8 threads agreeing with serial;
  the TLAS point query and the flat scan agreeing over one SHARED object
  set; and scene D's placement RE-DERIVED FROM THE ASSETS and asserted at
  1e-6 against the scene header, plus its probe values.

---

## 10. Residuals, stated up front

- BDPT/VCM/MLT rebuilt records read 0 for `proximity` and, after Phase 3,
  `interior` (the family's disclosed gap; the `PathVertexEval` contract is
  explicitly declined for the same reason as for the other three signals).
- Refusing families (RAW meshes, patches, hair, heightfield SDFs; CSG
  composites until Phase 3 — after it, an intersection/subtraction with a
  sheet operand, a union of two refusing operands, a bracket with no admitted
  landing in budget, and a seam gradient below 1e-12) read far. An
  intersection/subtraction answers within `[d − τ, d]` when the landing
  reaches an exact operand (τ = 1e-12 of its local diagonal) and within
  `[d, d + ε]` when it reaches a bound one (ε = 5e-5 of the local diagonal,
  floored at 1e-6), both scaled by the composite's σ_max in world space; a
  point exactly on its surface reads 0 in the first case and up to ε in the
  second; an intersection/subtraction is never an exact operand to a parent.
- SDF neighbours are an upper bound on distance (never over-read contact),
  bounded by the last probe step; the gap is measured on C, not bounded
  analytically. A candidate whose crossing is not found within budget reads far.
- Non-uniform scale and ellipsoids are upper-bounded, not exact (Phase 3 makes
  σ exact but `×σ_max` remains a bound attained only along the top singular
  vector).
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
  which makes three distinct queries per hit — one under capacity. Phase 3
  (§5.6) raises `kL1Ways` to 8 with `interior` as a fifth kind and lifts the
  asserted ceiling to 4096 B (~2432 B used); the cliff moves to a ninth
  distinct query, it does not disappear.
- **Emissive objects never count**, including decorative ones (a lava pool, a
  glowing rune) that should collect contact; the predicate is per object and
  RISE binds one material per object. Phase 3 if a scene needs it.
- **`proximity` cannot drive relief**: `ReliefModifier` holds `signals` fixed
  across its stencil by documented design.
- Per-sample motion blur: a neighbour moving under a non-keyframed painter can
  serve a stale query-memo entry only at a bit-identical receiver point, which
  sub-pixel jitter makes essentially unreachable; no scene in §1 exercises it.
- **The candidate scan is no longer linear in object count where a TLAS
  exists** (§8.3): `NearestOtherSurface` walks the top-level BVH as a point
  query there, and the linear scan over cached AABBs remains only for
  scenes with no TLAS (four objects or fewer, or `bUseBSPtree` off).  Two
  residuals come with that.  (i) A **contract shift**: on a TLAS-backed
  scene the query is exactly as stale as the render is, because
  `IntersectRay` walks the same tree -- the snapshot's add-detecting count
  check (§8.1) governs only the small-scene fallback now.  (ii) A BVH leaf
  holds up to four objects and the traversal evaluates all of a leaf's
  occupants once the leaf's box is within the running best, with no
  per-object box test in between: 11.08 distance calls per query on Sponza
  against the flat scan's 4.64.  Strongly positive there (405 box tests
  traded for ~6 mostly-refusing distance calls) and the first place to look
  if a scene with expensive-to-answer neighbours regresses.
- **A mesh neighbour is a SHEET, and no other shipped family is.**  Every
  solid family clamps a signed field at zero, so a point inside reads 1;
  a point inside a closed MESH reads its honest distance to the nearest
  triangle instead, because a triangle soup carries no inside test.  The
  direction is safe (an over-report under-paints) but the inconsistency is
  real: a receiver buried inside a mesh neighbour will not read contact.
  A signed/inside variant for meshes needs a robustly closed-mesh test and
  belongs with the Phase-3 signed variant.
- **`standard_object`'s `scale` written with ONE number derives to a
  DEGENERATE transform, silently.**  It is a `DoubleVec3`; `scale 0.35`
  produces no diagnostic, makes the object vanish from the render, and
  makes it refuse every proximity query (`Object::DistanceToSurface`
  rejects `sigma_min <= 0`).  Found while authoring scene D and verified by
  re-introducing it (§8.3, trap 4).  A parser gap, outside this design's
  scope, recorded here because it is a live trap for anyone placing an
  object for a contact scene.
- `proximity` is unsigned; Phase 3's `interior(r)` supplies the inside half for
  the solid families only (meshes and every sheet family contribute 0 to it).
- The query reads other objects' transforms and so joins the pre-existing
  per-sample `EvaluateAtTime` race (ARCHITECTURE.md), no wider than
  `Object::IntersectRay` already does.
- The scalar pipe stamps `time = 0` (no `m_time` there), so its memo entries
  rely on the jitter argument alone under motion blur.
- Until Phase 3, non-uniform transforms use the Frobenius/determinant σ
  bounds, which are loose (never unsafe); Phase 3's one-sided Jacobi makes them
  exact to rounding, with the same pair as the non-convergence fallback.
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
  CSG result. Phase 3's composite queries (§5.6: union-min, and the bracket
  over `max(f_A, f_B)` / `max(f_A, −f_B)` for intersection and subtraction)
  are what fix it; until then a scene whose contact surface is a CSG result
  needs a non-CSG proxy. (Wave 1, 2026-09-08.)
- **The signal is PT-only more sharply than the first bullet suggests.** The
  channel is stamped by `ObjectManager::IntersectRay`, so EVERY consumer that
  builds its own hit record reads the neutral 0 — `PathVertexEval`, the GUI's
  painter preview, realize-time displacement, `HairGenerator`. That is the same
  set the other three signals are neutral on, but here the neutral means "no
  contact anywhere in the scene", which is a more visible absence than
  "unoccluded".
- **Fixed in this fix round (wave-2 integration, 2026-09-08): the agent-facing
  draft-preview descriptor strings were wrong.** They claimed quality:"draft"
  "IGNORES the scene's authored materials and lighting entirely"; in fact
  `InteractiveMaterialPreviewShader::MaterialAlbedo` calls `bsdf->albedo()`
  (→ `GGXBRDF::albedo` → `pDiffuse->GetColor(ri)` for a diffuse lobe), which
  runs an expression painter and therefore any `proximity`/`occlusion` query
  it makes — draft ignores LIGHTING only (a fixed synthetic studio rig stands
  in). Corrected in `AgentMcpAdapter.cpp`, `AgentChatCodecs.cpp`,
  `AgentSession.h`/`.cpp`, and `AgentRpc.h`; see §8.2's Draft-preview
  paragraph. `GEOMETRY_SHADING_SIGNALS_DESIGN.md` §7.3 carried the same
  error in its mesh-bake laziness rationale — corrected there too, with the
  consequence noted: an albedo evaluation that reaches `occlusion()` on a
  mesh in draft DOES trigger `MeshSignalBakeCache`'s bake on the scene's
  first draft render, not only on its first production render.
- **Refined same-session, same day: "diffuse albedo" was itself an
  understatement.** `InteractiveMaterialPreviewShader::PreviewPel` also
  evaluates the material's full `bsdf->value()` (not just `albedo()`)
  against three fixed key/fill/top studio lights, so specular/roughness
  response is visible in draft too, not only base colour; relief/bump/
  normal modifiers apply (`RayCaster::CastRay` runs them before `Shade`,
  unconditionally); and draft does NOT show emission (a directly-visible
  emitter is hidden to background), transmission/refraction, or
  subsurface scattering — those material kinds only shrink the
  ambient-occlusion weight, never traced. Every descriptor string listed
  above now says "shades with the material's own BSDF under a fixed
  studio rig" and states the emission/transmission/subsurface omission
  explicitly, rather than "evaluates diffuse albedo".
