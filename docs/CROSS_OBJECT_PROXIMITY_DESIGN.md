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
  Phase 3's CSG composites (§5.6) keep this invariant whole: a composite's
  landing test admits a point only when the operands' own signs PROVE it
  lies in the closure of the real solid, with no tolerance on either side
  (a tolerance re-admits the phantom touching set — §5.6 walks the station
  that showed it). That proof is over the reals; the operands' closed forms
  are ROUNDED, so within ~1 ulp of a tangency of two exact operands the
  computed pair could satisfy an arm the real set does not — a band of
  order `sqrt(2R·ulp)`, ~5 nm at R = 0.25 (down from 1.6 µm under a
  tolerance) for a hypothetical tangent pair; on `column2`'s only tangent
  pair, the flute, it is foreclosed outright because the computed `f_A` is
  never below the computed `f_B` near the tangency (§5.6). Stated, not
  tolerated.
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
rounds (round 14, no P1 — the design is converged; round 13, no P1 — sweep statistics re-attached to their conditions,
the rounding band scoped, the exact-station reference named as the closed
form, the exception count made consistent; round 12, 2 P1s: the flute walk's numbers were wrong and blamed the
wrong disjunct; the header that hosts the `interior` body was unnamed;
round 11, 3 P1s: the boundary arm's τ tolerance re-admitted the
flute phantom from a station 1 µm off its axis, so the tolerance is gone
and the never-over-read invariant is restored whole; the procedural-textures
skill and its snippet count were missing from the plumbing; the bare-call
message is a second byte-identity exception; round 10, 2 P1s: §2's exception lacked its `R` hypothesis; the
`interior` plumbing list missed the builtin name table and the chunk
descriptors; round 9, 2 P1s: the byte-identity claim collided with the clause
(d) string fix; §2's never-over-read invariant lacked the composite
exception; round 8, 4 P1s: the grazing ceiling was mis-solved by the dropped
factor 2; the exact-landing interval omitted the grazing term; the flute
gate's direct query must be per-object since the floor answers at 7.5 cm;
not every sheet answers the unsigned query; round 7, 2 P1s: one analytic interval survived round 6; caching
the composite's diagonal at operand assignment is stale under re-point and
animation; round 6, 5 P1s: the answer intervals claimed an analytic gap §5.2
only measures; leaf exactness ignored the operand's own transform; the union
flag over-claimed its interior; the contact body's params were wrong; its
`mix` weight was unclamped; round 5, 3 P1s: the exact-operand list named sheets, a nested
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

- `IGeometry::SignedDistanceLower( ptObject, maxDistObject, outSigned,
  outExact ) → bool` (the exactness flag §5.6's boundary arm consumes is an
  out-parameter, cleared by the refusing default)
  (refusing default; the sheets — plane, disk, clipped plane, open cylinder,
  mesh, patch, hair — and heightfield SDFs keep it) and
  `IObject::SignedDistanceLower( ptWorld, … )`, which
  does the transform: the point through the inverse, the radius by `/σ_min`,
  and the magnitude **back by `σ_min`** — the lower bound's safe direction
  (`d_w ≥ σ_min·d_o`), the opposite of the unsigned query's `σ_max`.
- Per family (the unsigned answers are unchanged: plane, disk,
  convex-coplanar clipped plane, open cylinder and — after Phase 2 — the
  indexed mesh answer it; patches, RAW meshes, hair, non-convex or
  non-coplanar clipped planes and heightfield SDFs refuse both queries, as
  scene C asserts): sphere, box, capped cylinder, torus — exact closed forms,
  so the lower bound IS the distance, inside and out; ellipsoid — sign exact
  from `Σ (p_i/a_i)² ≤ 1`, magnitude `dUnit × min(a,b,c)` (the unsigned query
  keeps `× max`); SDF — `Map(p)` itself (exact sign, 1-Lipschitz under-read),
  heightfield mode refuses as the unsigned query does; planes, disks, clipped
  planes, open cylinders, meshes, patches, hair — sheets, no inside: they
  keep whatever unsigned answer they give today (the partition above) and
  REFUSE this one.

**Why `min(operands)` is the wrong CSG answer, and what the right one is.**
For a point `p` outside both operands of a **union**, the true distance to the
union's surface is exactly `min(d_A, d_B)` (every union-boundary point lies on
∂A or ∂B, so `d ≥ min`; and if the nearest point of ∂A lies inside B, the
segment to it enters B first at a point on ∂B that is outside A — a boundary
point no farther than `d_A` — so `d ≤ min`; tangential contact falls in the
first case). But the composite can only use what the operands report, and
`min(exact, under-read)` under-reads. So the union's UNSIGNED answer is
`min(u_A, u_B)` over the operands' unsigned upper bounds, itself an upper bound
(`d = min(d_A, d_B) ≤ min(u_A, u_B)`), exact when both operands' unsigned
answers are exact under a similarity (the unsigned path's `×σ_max` is a
bound under anisotropy exactly as the signed path's `×σ_min` is; no flag is
carried for the unsigned answer, nothing consumes one) —
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
per gradient, each recursing into both operands; the composed field is
still 1-Lipschitz — an operand's transformed lower bound `σ_min·d_o(M⁻¹x)`
has gradient ≤ `σ_min/σ_min`, the ellipsoid's `dUnit × min(a,b,c)` likewise,
and `max`/`min` preserve it — so §5.2's non-overshoot argument for a step
of `|f|` along `−∇̂f` carries over), probe until the landing
point `q` is PROVEN to lie in the closure of the composite's REAL solid by
the operands' own signs, and report the chord `|p − q|`. Two arms prove it.
The STRICT arm — intersection `f_A < 0 ∧ f_B < 0`, subtraction `f_A < 0 ∧
f_B > 0` (a negative lower bound proves strictly inside its operand, a
positive one strictly outside) — puts `q` in the solid's interior, so the
segment from `p` crosses the boundary. The BOUNDARY arm admits a landing
exactly ON one operand's surface when THAT operand's signed distance is
EXACT — `SignedDistanceLower` returns an exactness flag, set ONLY by the four
SOLID closed forms (sphere, box, capped cylinder, torus) AND only under a
SIMILARITY transform (`σ_min = σ_max`, the exact fast path): the object
layer converts the magnitude back by `×σ_min`, which under an anisotropic
transform is a bound, not the distance — the same condition the union rule
below carries; ellipsoid and SDF never set it, and sheets (plane, disk, clipped plane, open cylinder, mesh, patch, hair)
refuse the signed query altogether, so they can never carry it — a
subtracted plane admitted to this arm would report the chord to a face the
subtraction removes nothing at, the round-3 phantom in a new form. The arm:
intersection `(f_A ≤ 0 ∧ f_B < 0) ∨ (f_A < 0 ∧ f_B ≤ 0)`, subtraction `(f_A
≤ 0 ∧ f_B > 0) ∨ (f_A < 0 ∧ f_B ≥ 0)`, where the `≤ 0` / `≥ 0` side is the
exact operand (so `f_A ≤ 0` proves `q ∈ closure(A)` exactly) and the other
side is STRICT — a point in `closure(A)` strictly outside `B` is in the
closure of `A \ B` (interior if inside `A`; on `∂A` a neighbourhood outside
`B` holds interior points of `A`, all in the solid), and a point strictly
inside `A` on or outside `∂B` likewise. There is NO TOLERANCE on the exact
side, deliberately (rounds 5–11): the descent `p ← p − f·ĝ` lands on the
exact operand's surface only to rounding, and a `+1 ulp` miss fails this
arm — the probe then steps (one ε step on the fixtures; a solid thinner
than ε at the landing takes the doubling steps or refuses, both safe) and
the answer is `d` plus that step instead of `d`, which is the SAFE
direction. A tolerance `τ` on EITHER side looked harmless and is not,
because the nearby points of an operand's surface may all lie inside the
other operand, so a landing within `τ` of a surface can be within `τ` of
the phantom, not of the real solid. On the flute, stations within a few
micrometres of the slot's axis show it (recomputed in doubles, round 12):
from local `(1e-6, ·, 0.26)` the radial descent lands at `q ≈ (9.615e-7,
·, 0.25 − 1.85e-12)` — the descent is a radial rescale `q = (0.25/R)·p`,
so `f_A(q)` computes to exactly 0.0 at this station (not in general: it is
strictly negative at 45–47 % of nearby stations and +1 ulp at under 1 %,
the exact figures depending on whether the descent is modelled as the
rescale or as the literal `p − f·ĝ`) and
`f_B(q) = −1.8491·x² = −1.85e-12`, 1.85 pm INSIDE the slab. Across 200 000
such stations `f_A < 0` strictly at 45–47 % and `f_B ≥ −τ` (τ = 5e-12) at
53 % (the τ crossover `|x| = sqrt(5e-12/1.8491)` = 1.64 µm over the
sweep's `[1e-7, 3e-6]`), jointly ~24 %, so the tolerant `f_A < 0 ∧ f_B ≥ −τ` disjunct admits a
landing that is inside the subtrahend — not in the real solid at all — and
reports a 1.00 cm chord where the real solid is 4.21 cm away:
`proximity(0.02)` = 0.5 against a truth of 0, the forbidden direction by
3 cm, on a quarter of nearby stations. Under the strict rule those landings
are rejected by the STRICT side (`f_B < 0` by 1.85e-12, four orders above
ulp noise), and structurally near the tangency (where the slab's active
face is its z face, `|z| ≥ |x| + 0.21`): `fl(sqrt(fl(x² + z²))) ≥
fl(sqrt(fl(z²)))` by monotone rounding and `fl(sqrt(fl(z²))) = |z|` for
every double away from over/underflow — the box's own form takes `|z|` by
`fabs`, which is exact — so the computed `f_A` is never below the computed
`f_B` there and `{f_A ≤ 0 ∧ f_B > 0}` is empty NUMERICALLY, not only over
the reals. Neither arm admits
`f_A = f_B = 0` (the strict side rejects it), which is what keeps the exact
tangency out as well. The exactness flag is set only for a NON-DEGENERATE
operand (a zero radius or extent makes `closure(A)` lower-dimensional, the
composite renders nothing, and the closure argument below needs interior
points near the landing). Which arm applies
is decided PER LANDING by the exactness of the operand whose surface was
reached, not by what the composite contains: `SDF − box` landing on the box
face takes the boundary arm and is exact. When the reached operand
is a BOUND one (SDF, ellipsoid) only the strict arm can prove the landing,
so the probe steps by the composite's own `ε = max(1e-6, 5e-5 × local
bounding-box diagonal)` — the SDF family's rule; `CSGObject` has no
`IGeometry` and its `getBoundingBox()` answers in the PARENT's frame, so
the local diagonal is the operands' composed local box (a subtraction takes
A's) — exactly what `CSGObject::SelfHitRootFloor` already computes,
including its `isfinite`/`< 1e30` extent screen (an unbuilt mesh or an
infinite plane reports ±RISE_INFINITY), hoisted into a helper and computed LAZILY per query, never cached — the
operands' boxes move under `FinalizeTransformations` (every animation frame
and hierarchy re-bake) and under an operand's own incremental re-point, and
the CST re-point path calls `AssignObjects` BEFORE `SetOperation`, so any
cache filled at assignment is stale in the over-read direction (a
stale-large diagonal inflates the probe); `SelfHitRootFloor` pays the same
`getBoundingBox` per call for the same reason, and a nested operand's box
recurses through its own `getBoundingBox`, which answers in the parent's
frame (the composite's local frame, since CSG operands cannot be parented). The landing is A boundary point, not the NEAREST one (the piecewise gradient
follows whichever operand is active, and a descent that exits at its cap
overshoots by more than ε), so `|p − q| − d` is a MEASURED gap, `gap_max`,
exactly as §5.2 treats the SDF bracket — never an analytic bound. Hence
EVERY landing answers within `[d, d + gap_max]` — an exact-operand landing
with `gap = 0` when the arm fires and one probe step (ε on the fixtures)
when a `+1 ulp` miss sends it to the probe, a bound-operand landing with
the probe's ε the first term of the gap — and the world-space slack is
`σ_max` times it (ε = 2.52e-4 on
`column2`'s 5.0498 local diagonal, σ = 1 — stated in §8 and §10; the
composite's 5e-5 is fixed while an SDF operand's `m_epsFrac` is
author-settable, so a composite may probe finer than its operand's own
surface band, harmlessly). A query point lying exactly ON the composite's
surface has `q = p` as its zero-length descent endpoint: admitted by the
boundary arm when the reached operand is exact (reads 0), otherwise the
probe steps and it reads within the measured gap (one ε step where the
first probe lands, as it does on the fixtures) — the unsigned clamp is on a NEGATIVE
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
correct `proximity(0.02)` there is 0 anyway. Phase 1's bracket recomputes
the descent gradient on every descent iteration but exits the descent on
the backoff break (`f ≤ kBackoff·ε`) before the next iteration, and the
probe reuses the last gradient — so the degenerate central difference AT
the tangency (a V-valley of value 0) is never taken from a descended
landing. The one exception is the probe's `!haveGradient` branch: a query
point that STARTS inside the backoff band exits the descent on its first
test and the probe takes a fresh central difference there, straddling the
valley; the landing test still demands operand-sign proof, so that case
refuses rather than reporting a chord. An implementation that evaluates the
gradient at a descended tangency gets the same refusal through the 1e-12
seam rule instead. The composed field an intersection/subtraction
descends is ALSO what it exports as its own `SignedDistanceLower` to a
parent composite and to `interior`, with the composite's `×σ_min` applied —
and it NEVER sets the exactness flag: `max(a, b)` under-reads near a seam
even over exact operands, and its zero set is the phantom touching set, so a
parent's boundary arm must not land on it (the round-3 bug one level up).
A union exports `min(f_A, f_B)`; INSIDE, that is
only a lower bound on the union's depth (two overlapping unit-deep
slabs read depth 0.5 at a point 1.5 deep in their union), so `interior(r)`
under-reads inside a union's overlap — a §10 residual.
**~~and sets the flag only when both operands set it AND its own σ is
exact~~ — CORRECTED AT IMPLEMENTATION (§8.4 S3): NO COMPOSITE exports the
exactness flag, a union included.** The rule above is unsound, and the
review that found it traced the whole failure: `min(f_A, f_B)` is 0 not only
on the union's boundary but on every INTERIOR point where the two operands'
boundaries meet from opposite sides — a shared face, a set of positive AREA.
Two boxes stacked into a cube read `min = 0` all over the seam plane, where
the true signed distance is the cube's depth. Exported with the flag set,
that lets a PARENT subtraction's boundary arm (`exB ∧ f_A < 0 ∧ f_B ≥ 0`)
admit a landing strictly INSIDE the subtrahend — not in the real solid at
all — which is the round-3 phantom one level up and an OVER-READ of contact.
Measured: `box(2,2,1)@−0.5 ∪ box(2,2,1)@+0.5` subtracted from a 4-cube
reported **0.25 against a true 0.75**. Dropping the flag closes it
completely, because every other consumer of the exported field reads only a
STRICT sign (the strict arms need `< 0` or `> 0`, and an interior seam reads
0; `interior` counts only `f < 0`, so a seam contributes 0 depth — the safe
direction). The §8 gate's union-in-subtraction row used two OVERLAPPING
spheres, whose `∂A ∩ ∂B` circle lies ON the union's boundary — precisely the
configuration that dodges this — which is why an ABUTTING fixture is now
pinned beside it. At a max/min seam two nearly opposed operand
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
unsigned query — for an intersection or subtraction — clamps at zero when
its composed sign is negative (a point inside the composite's solid is
contact, §2; a union needs no composed sign, its operands' own clamps
already return 0); a point inside operand A of an
intersection it does not share with B is OUTSIDE the composite and reads a
positive distance — correct, not a violation, and stated here so nobody "fixes"
it. `bComplementedField` never enters: the composite composes signs itself.
The per-object refusal log covers composites as it does any object, naming
the kind as "csg <op>" rather than the current "(no geometry)" fallback —
through a new `IObject::DescribeKind()` accessor the log calls in place of
`typeid(*GetGeometry())`, since a composite reaches the log only as an
`IObjectPriv*` with no geometry (`Object` answers the geometry's type name,
`CSGObject` its operation) — and
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
and the worst over-read is bounded by `‖M‖_F/0.4 = 7.97×` (§5.2's measured
7.96× coincides with that bound to the rounding; true `σ_max/σ_min` = 7.5;
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
replaced by a three-state `m_sigmaSource` (copied in `Object::CloneStateTo`
beside `m_sigmaExact`) — `Exact` (fast path), `Jacobi`
(converged, ratio = true `σ_max/σ_min`, 7.5 there) and `Loose` (the sweep
cap hit, Frobenius/det pair, ratio 26.99) — and the print site names the
state, since a converged and a fallen-back object are otherwise
indistinguishable at `!m_sigmaExact`) and the degenerate refusal (which runs BEFORE Jacobi on `|det|`, so `σ_min > 0` holds
after the nudge). Because the chain of inequalities the design rests on must
survive rounding, on the Jacobi path the stored `σ_max` is widened UP and
`σ_min` DOWN. **CORRECTED AT IMPLEMENTATION (§8.4 S2): the widening is
RELATIVE and condition-scaled (`16 · eps · σ_max/σ_min`), not "four ulps".**
A fixed ulp count cannot bound a relative error — a review checked the
four-ulp claim in exact arithmetic and broke it on an ordinary
well-conditioned matrix (cond 29.6, every entry O(1)), where the computed
`σ_min` sat TWELVE ulps above the true one so four downward nudges left the
stored number still above it; at cond ~3e12 the gap reaches 6.4e-5 relative,
which no ulp count reaches. Widening is free in the safe direction (it can
only make the unsigned answer larger and the signed one smaller), so the
factor is a heuristic tied to the measured error's shape rather than a proof.
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
the builtin's row in `ParseCall`'s name→signature `Lookup` table
(`{"interior", kFnInterior, 1, {S,S,S,S}, S}` beside `proximity`'s — the
one item whose absence is a parse failure rather than a stale comment),
`kFnInterior = 58` (the next free id; the `kFnProximity` assert's "58–59 free"
text is updated), one more named `case` in `CallFunc` (the pin moves once
more, disclosed), `SignalKey.fn = 4` / `eInterior = 4`, `SurfaceSignalInfo::Interior(r)`
beside `Proximity(r)` sharing the L1 helper — IN
`src/Library/Interfaces/SurfaceSignalProximity.h`, the header that exists
to host the cross-object body outside the provider's include cycle (a new
header would trip the five-build-project rule), whose every enumerating
sentence is rewritten: its title ("the CROSS-OBJECT half" becomes the
cross-object PAIR), "its three siblings", "THE SECOND POLICY BODY … not a
fourth branch", and "WHO MUST INCLUDE IT: any translation unit that CALLS
`Proximity`" (or `Interior`); and `src/Library/Utilities/PathVertexEval.h`'s
declined-contract comment (all five count phrases: "the three signals that
share this channel", "one widening for all four", "widening for ONE of the
four", "exactly as `occlusion`, `thickness` and `convexity` read theirs",
"doing all four at once") gains `interior`, since §10 commits it to the
same gap; `ParseCall`'s `isSignalFn` gains
the id AND its unit diagnostic becomes a three-way (fraction / world length for
`proximity` / world length for `interior`) instead of a binary ternary —
and its other two consumers are re-read as the code demands: the
`expression_function2d` refusal extends to `interior` unchanged, and the
`DynR` remap's final arm is the guarded `sig->id` fall-through, which is
what lets a computed radius through (the `proximity`-as-`convexity`
miscompile came from silence here);
`ExpressionProgram::UsesProximity()` becomes `UsesCrossObject()` at its two
call sites (`ExpressionPainter.h`'s `m_proximityDemand` initialisers), and
`ProximityDemand` keeps its name but is documented as "registered when the
program calls `proximity()` or `interior()`" so the eager snapshot covers an
`interior`-only scene; the two `ISurfaceSignalProvider.h` comments that
pin `eProximity` as "NOT a fourth branch of `SignalQuery`" and "3 and
nothing else" are rewritten for `eInterior = 4` (also a separate body, not
a `SignalQuery` branch); `kFnInterior = 58` leaves only 59 free before
`CallFuncVec3`'s 60+ band (stated on the assert); the
`ISurfaceSignalProvider.h` warning text that enumerates
`curv/occlusion/thickness/convexity/proximity` gains `interior`, as do `skills/agent/procedural-textures.md` (its
"the fourth signal, and the only cross-object one" heading, its family
enumeration and radius-unit contrast, plus one `interior` fence, which
moves `AgentSkillsTest`'s pinned seed-snippet count 32 → 33 — §7 names
that skill as the adoption surface, so it is not "only" the descriptors)
and the three `ChunkParserRegistry.cpp` descriptor
strings that enumerate the builtins (the scalar pipe's `expression` param,
the `expression_painter` chunk description with its CROSS-OBJECT PROXIMITY
paragraph, and its `def` param) — plus `ISurfaceSignalProvider.h`'s
preamble ("the four honest-fallback wrappers", "THREE OF THE FOUR ARE
SELF-SIGNALS … the fourth, `proximity`, is CROSS-OBJECT", "ZERO IS THE
HONEST ABSENCE for all four") and a `NeutralInterior()` beside
`NeutralProximity()` for `CallFunc`'s no-provider arm,
`ExpressionEval.h`'s `SignalRadiusCall::fn` comment, its "58 and 59
remain free in this band" prose beside the assert, and the
`UsesSurfaceSignals`/`SurfaceSignalCalls` comments that enumerate four
calls, `ExpressionPainter.cpp`'s "the one signal whose answer can move
because a NEIGHBOUR moved" and its "a roughness pipe's proximity entries"
note — plus `ExpressionPainter.h`'s four count-bearing sentences ("carries
all four", "all five" twice, "the same four builtins"), `ExpressionMemo.h`'s `fn` field comment and its `kL1Ways` block
("exactly four signals wide", "1824 bytes", "eight ways would blow the
2048-byte ceiling" — all false after Phase 3); `CSGObject`'s new
`DistanceToSurface`/`SignedDistanceLower`/`DescribeKind` bodies carry NO
`override` keyword, matching that class's documented rule (one `override`
wakes `-Winconsistent-missing-override` on its seven sibling virtuals);
and, as for `proximity`, an `interior` call lands in `m_sigCalls`, so
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
hit rate; `kL1Ways` goes to 8, which puts `Tables` at 2432 bytes, over the
2048-byte ceiling `ExpressionMemoTest` (g) asserts — the ceiling was a
regression guard, not a budget, and is raised to 4096 with the reason recorded
in the test and in §6.5 of the convexity doc (43.8 kB across this machine's 18
workers). Residual, stated: meshes and every sheet family contribute nothing
to `interior`; a hit point inside a MESH neighbour reads `proximity = 1 −
d_shell/r` (1 only within the shell's `r`, 0 deep inside a large mesh) and
`interior = 0`.

**`add_wear` composition.** The verb gains two parameters, `contact_radius`
(world length, default 0 = off) and `contact_grime` (mask weight, default
0.5, range [0, 1] in the descriptor and emitted as `param contact_grime …
min 0 max 1` — SLIDER METADATA ONLY: the VM ignores `min`/`max` at
evaluation and `mix` does not clamp `t`, so the bound that actually holds
is the outer `clamp(…, 0, 1)` on the `crevice_mask` line, present on both
recipes). Its candidate gate — clause (c) of the
CONDITIONS scan, not the verb — rejects flat receivers (its own decline
reason: "every object bound to it sits on planar or patch geometry, where
`curv` is 0 everywhere a ray can land"; the bare-call refusal says "a
plane, disk, box or patch") before any call, so the two flagship
contacts (a plank on a plane, a flange on a box) are outside the verb's
candidate set today. The body today is ONE shared mask recipe
(`BuildWearMaskPreludeText`: six mask `param`s, `seed`, `jitter`,
`wear_mask`, `crevice_raw`, `cavity_boost`, `crevice_mask`) emitted
byte-identically into the colour chunk and the roughness chunk, each of
which adds its OWN endpoint params (`base_r/g/b`, `edge_desat`, `edge_lift`,
`patina_desat`, `patina_darken` with `def`s `base`/`lum`/`grey`/`edge_tint`/
`patina_tint`; `rough_base`/`rough_polished`/`rough_crusted`) and one
consuming line, `mix(mix(base, edge_tint, wear_mask), patina_tint,
crevice_mask)` / `mix(mix(rough_base, rough_polished, wear_mask),
rough_crusted, crevice_mask)`. When `contact_radius > 0` the contact term
enters the SHARED prelude, so both chunks agree by construction as today:
three `param` lines (`contact_grime`; `contact_r <radius>`, retunable —
safe because `proximity` has no `DynR` twin, unlike the `occlusion(0.08)`
literal whose comment explains the opposite choice; `contact_scale`, the
breakup frequency, defaulted like `grime_scale`), one `def contact_mask
clamp(proximity(contact_r) + breakup_amp*fbm(P*contact_scale + jitter, 4,
0.5, 2.0), 0, 1)`, and the existing `crevice_mask` line becomes `clamp(
crevice_raw*cavity_boost + contact_grime*contact_mask, 0, 1)` — the contact
term folds into the crevice endpoint (patina colour, crusted roughness) at
the ONE line both chunks read, the consuming lines are untouched, and the
result is clamped. Never relief (automatic — §5.3). The flat-receiver gate
is relaxed WITHOUT touching the scan's existing outputs: the scan keeps
barren-only materials in a SEPARATE list (`wearBarrenCandidates`: the
`WearMaterial_` fill — base colour, object count, roughness slots and band —
is hoisted ABOVE clause (c)'s test, since today the record is built only
after it, and the loop-local geometry kind is CAPTURED into the record's
`curvGeometryKind` before the `continue` drops it; the record then goes to
one list or the other, the decline reason left as is), and when `contact_radius > 0` BOTH of `AddWear`'s lookups
consult that list — the named-material path checks it BEFORE the
`wearDeclineReasons` refusal (today that refusal fires first and would turn
`add_wear {material: "bench_top", contact_radius: 0.002}` away), and the
bare-call path selects over the union of the two lists under
`SelectMaterialToWear_`, with the "no material qualifies" message gaining
an APPENDED clause that a flat receiver qualifies once `contact_radius` is
set — that message prints at `contact_radius 0` (it is advice for exactly
that call), so it is the SECOND pinned exception to the byte-identity claim
below, beside clause (d)'s string, and like it needs a NEW assertion (the
existing checks match the opening substring only). `qualifyingMaterials`
keeps reporting the curved count. On a barren receiver `curv ≡ 0`, so the
wear recipe would collapse to `clamp(breakup_amp·fbm)` — a pure-noise
edge-wear and patina wash over the whole flat face, plus a dead
`occlusion(0.08)` per shade; the barren path therefore emits a CONTACT
recipe instead: prelude = `contact_grime`, `contact_r`, `contact_scale`,
`breakup_amp`, `seed`, `jitter`, `contact_mask`, and `def crevice_mask
clamp(contact_grime*contact_mask, 0, 1)`; the colour chunk keeps
`base_r/g/b`, `patina_desat`, `patina_darken`, `base`, `lum`, `grey`,
`patina_tint` and consumes `mix(base, patina_tint, crevice_mask)`; the
roughness chunk keeps `rough_base`, `rough_crusted` (with `sliderMax` and
the `asColourPipe` `expr`/`expression` split as today) and consumes
`mix(rough_base, rough_crusted, crevice_mask)`; a `hasRoughness == false`
pick gets the colour chunk alone, as today. Clause (d)'s decline string,
which names only `curv`/`occlusion`/`thickness` while the predicate matches
five signals, gains `proximity`/`convexity`/`interior` — a second `add_wear`
on a contact body is the common case that hits it. That string lives in
the conditions scan and changes for EVERY input, so it is the first of the
TWO deliberate exceptions to the byte-identity claim below (the bare-call
message's appended clause is the second); the test pins its new text with
a NEW assertion that names `interior` (today's `already-worn` check matches
the opening substring, which an appended clause leaves intact, so it cannot
detect the correction). The edge half (`edge_wear`,
`crevice_grime`, `breakup_scale`, `grime_scale`, `cavity_gain`,
`edge_desat`, `edge_lift`, `edge_tint`, `rough_polished`, `wear_mask`,
`crevice_raw`, `cavity_boost`) is not emitted, so no chunk declares a
`param` nothing reads — the VM has no compile-time backstop for an
unreferenced `param` (`AddParam` only registers a slot), which is why §8
asserts it over the emitted text. A barren pick's `curvGeometryKind` names
the receiver's geometry kind and the success message says it touches a
neighbour rather than that it curves. The design-note path keeps
advertising from the curved list only (`c.addWearName` comes from
`SelectMaterialToWear_(wearCandidateMaterials)`), so with `contact_radius >
0` the verb's bare pick MAY differ from the note's — the note advertises a
bare call — and the "must be one function" comment at the verb is updated
to say so. The descriptor text states the unit and that the author chooses
the radius from the scene's feature sizes. `contact_radius 0` is
byte-identical to today's output: the scan's existing lists, counts, kinds
and messages are unchanged except clause (d)'s corrected string and the
bare-call message's appended clause, and both
recipes are built strings: above 0 the
prelude adds the three params and `contact_mask` and REPLACES the
`crevice_mask` line; at 0 it emits today's `crevice_mask` line verbatim. `WearBodyReadsGeometrySignals_` gains `"interior"`
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
  refuses the signed query, and the sheets that answer the unsigned one
  (plane, disk, convex-coplanar clipped plane, open cylinder, indexed mesh)
  still do — patches, RAW meshes and non-convex or non-coplanar clipped
  planes refuse both, as scene C already asserts; under a
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
  `reported == closed form` to 1e-12 (the CLOSED FORM, not the grid — a
  grid reference is `d + O(spacing)`; scene C's `n_sdf_sphere` — a field
  exact outside — is the precedent for asserting a closed form at 1e-9) at the radial station of a cylinder-minus-box (the boundary arm fired: `f_A = 0.26 ⊖ 0.25` is exact
  by Sterbenz, so `0.26 ⊖ f_A = 0.25` exactly and `sqrt(x·x) = x` makes
  `f_A` a true 0 at the landing — the station does NOT exercise rounding)
  and `d ≤ reported ≤ d + gap_max` at an OBLIQUE station (a torus operand,
  or a cylinder station off its axis) whose landing residual is genuinely
  nonzero, `d` the closed form there, the fixture chosen so the reached
  operand carries the composite's nearest point (then `gap_max` is one
  probe step, ε, when the arm misses), with the test recording WHICH arm
  fired there and asserting `reported ≥ d` (the invariant a tolerance would
  have broken); `reported −
  reference ≤ gap_max` with `gap_max` recorded where a bound operand is
  reached; the phantom stations themselves — cylinder-minus-tangent-slab
  queried from local `(x, ·, 0.26)` for a sweep of `x ∈ [1e-7, 3e-6]`, the
  band where a tolerant arm admitted a quarter of landings — all REFUSE
  rather than reporting 1.00 cm; a subtracted PLANE
  operand makes the composite refuse (a sheet refuses the signed query
  outright — not merely lacks the flag, which a bound operand also lacks
  without forcing a refusal); a
  nested intersection reports `exact = false` to its parent; a
  `scale (3, 1, 0.4)` SOLID operand reports `exact = false` and the composite
  reaching it takes the strict arm (the similarity-only rule); a TANGENT pair
  (a box whose face is tangent to a cylinder, the flute case) from a station
  outside the tangent face either refuses or reports ≥ the true corner
  distance, never the tangency; a nested intersection inside a subtraction
  (beside the union-in-subtraction case) composes; a point inside operand A of an
  intersection but outside B reads a positive distance; a point inside the
  composite reads 0; a nested composite (a union inside a subtraction)
  composes; a TRANSFORMED composite (`position`/`orientation` on the
  `csg_object`, untransformed operands — the `glass_pavilion` column shape)
  answers within 1e-9 of the same composite built with the transform folded
  into the operands — with operand A AXISYMMETRIC about the rotation axis
  (the column's Y cylinder), because folding a rotation into a box operand
  grows its parent-frame AABB, moves ε and shifts a probe-stepped answer by
  ~ε, five orders above 1e-9; a box-minus-box fixture is compared at ~ε
  instead; an intersection or subtraction with a sheet operand
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
  wall is the exact cylinder (`colcylgeom` is capped by the parser default,
  pinned here because an open tube would be a sheet and void this gate), the
  radial landing IS the nearest point by symmetry, so the boundary arm fires
  and the 1 cm prediction is 0.5 to 1e-9, with `0.5 − ε/r` = 0.4874 (ε = 2.52e-4) the
  floor if the probe steps instead — exactly one step here, because the
  single radial step lands on the nearest point — either way inside 0.05; and one station on the
  cap top 1 cm outside a FLUTE's tangent face (local `(0, ·, 0.26)`), where
  the nearest real surface is the slot-wall/cylinder corner at 4.21 cm:
  `proximity(0.02)` there must read 0 (a phantom landing would read 0.5),
  and a direct PER-OBJECT `column2->DistanceToSurface` call at radius 0.1
  REFUSES — always, by the walk above — which is asserted (the scene-wide
  `NearestOtherSurface` at that radius would ANSWER 0.075 from the floor
  top at y = 0.1 under the cap top at 0.175), with the recomputed 4.21 cm
  recorded beside it as the true distance the refusal under-paints. Harness-only: the cap and floor bind
  checker/uniform painters, not expression painters, so the beauty crop is a
  probe-albedo render of a scene COPY (raw `proximity(0.02)` on the cap top —
  `marble_col` is bound to the ceiling, the pedestal, all eight capitals and
  every column operand — 18 objects — so the copy paints all of them),
  judged honestly, and the tracked scene is not edited beyond one pin: a
  `capped TRUE` line written into `colcylgeom`, so a later edit cannot turn
  the column into a sheet and void this gate silently (the scene is in
  `CstDeriveGoldenTest`'s manifest, which hashes `DumpJob`, and the pin is
  the parser default, so the digest does not move); `SourceHygieneTest`'s
  closed `signals`-writer census already lists `CSGObject.cpp`, so the
  composite bodies do not trip it;
- exact σ: `SigmaMin()/SigmaMax()` within 1e-12 of the written reference on
  the rotation, reflection and uniform scale (fast path) and within 1e-9 on
  `(3, 1, 0.4)` and the shear (Jacobi); the `(3, 1, 0.4)` object's search-
  radius inflation falls from 8.47× to `1/σ_min = 2.5×`; its unsigned answer
  is ≥ and its lower bound ≤ a brute-force distance to the scaled sphere, with
  the over-report factor recorded (exactness is NOT claimed: `×σ_max` remains
  an upper bound attained only along the top singular vector); the `Loose`
  state is reached by a unit test that calls the σ routine — hoisted out of
  `FinalizeTransformations`' inline cache fill into a free
  `ComputeSigmaExtremes( M, maxSweeps, … )` the object calls — with a
  zero-sweep budget and checks the state the routine RETURNS (the log line
  that prints it is reachable only through `Object::DistanceToSurface`);
- `interior(r)`: 0 on every scene-C probe that lies OUTSIDE its neighbours;
  on the six existing interpenetration probes (box, sphere, capped cylinder,
  torus on its tube's centre circle, ellipsoid and SDF centres — depths
  1, 1, 1, 0.5, 1, 1) `min(depth/r, 1)`
  within 1e-9 of the closed form, and exactly 1 where `r ≤` the depth — the
  ellipsoid row is a lower bound that is tight only at the centre, so its
  probe stays at the centre (the bound is tight along the whole minor axis,
  the centre is simply the point the fixture has) and the test says so;
  inside two overlapping spheres the
  larger depth; inside the overlap of a UNION composite of those two spheres
  the exported `min` under-reads (asserted ≤ the true depth, and < it at a
  point deeper than either operand's depth); a mesh neighbour contributes 0; a computed radius is accepted;
  the parse diagnostic names a world length for `interior`; an `interior`-only
  scene builds the eager snapshot (demand covers it); the memo keys/L1 rows
  extended (`fn = 4`), red-proved; `kL1Ways = 8` with the raised TLS ceiling
  asserted and the bytes printed; `TextureExpressionVMTest` 846/846;
- `add_wear` with `contact_radius`: refuses on a body that already reads
  `proximity`/`interior`; with a radius, the written body calls
  `proximity(contact_r)` exactly once as an additive mask on the crevice
  endpoint, a flat box receiver is accepted from the barren list, and the
  scene derives; on a curving receiver the `crevice_mask` line carries the
  contact term and the two consuming lines are byte-identical to today's; a
  barren pick's emitted chunks are the CONTACT recipe: no `curv`, no
  `occlusion`, no `wear_mask`, and every `param` either chunk declares is
  referenced at least once outside its own declaration (no inert slider),
  asserted by name over the emitted text since the VM has no such check;
  the `crevice_mask` line is wrapped in `clamp(…, 0, 1)` on both recipes
  (the only bound that holds — `max 1` is slider metadata);
  `contact_radius 0` is byte-identical to today's output INCLUDING the
  conditions scan's counts, kinds and decline messages, with clause (d)'s
  corrected string and the bare-call message's appended clause the two
  pinned exceptions, each asserted by a new check naming its new text;
  `AgentAddWearTest` extended; the MCP and chat tool counts unchanged
  (43 / 38); `AgentSkillsTest` at 33 snippets, the new `interior` fence
  deriving with zero diagnostics and rendering.

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


### 8.4 What Phase 3 actually built

Written slice by slice as each landed, with the measured number beside every
gate §8's Phase-3 block asserts, and every deviation from §5.6 named.

#### S1 — the signed lower bound per family

`IGeometry::SignedDistanceLower( ptObject, maxDistObject, outSigned,
outExact )` (refusing default, declared last, clears `outExact`) and
`IObject::SignedDistanceLower` beside it; `IObject::DescribeKind()` added in
the same header pass for the S3 refusal log.

| Phase-3 S1 gate | verdict |
|---|---|
| warning-free `make -C build/make/rise -j8 all` | PASS |
| sphere / box / capped cylinder / torus: `−depth` inside, within 1e-9 | PASS — −1, −1, −1, −0.5 at the six fixture probes (capped cylinder's centre is the RADIAL wall at 1, not the 2.0 cap) |
| the same four: `+distance` outside, within 1e-9 | PASS — +2, +2, +2, +2.5 from the floor stations |
| the four carry the exactness flag | PASS |
| ellipsoid: sign exact, magnitude `dUnit × min(a,b,c)`, flag never set | PASS — −1 at the centre (tight there); 1.0 against a true 2 at the station 4 along +x, i.e. a genuine lower bound |
| SDF: sign exact, `Map` inside, flag never set | PASS — −1 at the exact-field sphere's centre, +2 outside; the composed `smin` SDF's sign is exact inside |
| the signed query never refuses for RANGE | PASS — a 1 mm budget still answers +2 where the UNSIGNED query at the same budget refuses (both asserted) |
| every sheet refuses the signed query, and the sheets that answer the unsigned one still do | PASS — 9 fixtures: plane, disk, convex-coplanar clipped plane, open cylinder answer unsigned and refuse signed; skew quad, dart quad, patch, RAW mesh, heightfield SDF refuse both |
| a refusal CLEARS `outExact` | PASS on all 9 |
| `scale (3, 1, 0.4)` solid: lower ≤ and unsigned ≥ a brute-force distance | PASS — brute force 4.000000 (1201 × 2400 surface samples, matching the closed form `h − 1` to 1e-5), signed lower bound 0.472441, unsigned upper bound 12.7499 (both with the Phase-1 loose σ; S2 tightens them to 1.6 and 12.0) |
| `scale (3, 1, 0.4)` solid reports `exact = false` | PASS, with teeth: the same sphere under `scale 1.5` DOES carry the flag and reads 3.5 exactly |
| a degenerate transform, and a degenerate (zero-radius) OPERAND, refuse | PASS |
| `ProximitySignalTest` | 211 passed, 0 failed (123 before S1's section (h)) |
| `ProximityInvalidationTest` | 25 passed, 0 failed |
| `MeshClosestPointTest` | 65 passed, 0 failed |
| `SDFGeometryTest` | 685 passed, 0 failed |
| `SurfaceSignalsTest` | 318 passed, 0 failed |
| `SourceHygieneTest` | 165 passed, 0 failed |
| `TextureExpressionVMTest` | 846 passed, 0 failed (unmoved) |
| `ExpressionMemoTest` | 196 passed, 0 failed |

Deviations from §5.6, all in the refusing direction:

- **A DEGENERATE instance refuses the signed query outright** rather than
  answering without the flag. §5.6 says the flag "is set only for a
  NON-DEGENERATE operand"; the code goes one step further and refuses,
  because a zero-radius sphere's or zero-extent box's field is not a
  distance to any surface a ray can hit. The UNSIGNED query is deliberately
  unchanged (it still answers, and its answer is still an upper bound), so
  the two queries disagree about a degenerate instance exactly as they
  disagree about a sheet.
- **`DisplacedGeometry` inherits the refusing default** rather than
  delegating to its baked mesh, since the mesh family is a sheet and refuses
  anyway; that is one fewer forwarding body to keep honest.

#### S2 — exact σ

The σ computation is hoisted out of `Object::FinalizeTransformations`' inline
cache fill into a free `ComputeSigmaExtremes( m, maxSweeps, outSigmaMin,
outSigmaMax, outSource )` (declared in `Object.h`, defined in `Object.cpp`;
no new file, so the five-build-project rule does not fire). Three branches —
the unchanged similarity fast path, a one-sided Jacobi SVD on `M` (≤ 30
sweeps, columns rotated until mutually orthogonal, singular values read off
as the column norms, results widened four ulps apart), and the Phase-1
Frobenius/determinant pair as the non-convergence fallback. The degenerate
refusal runs on `|det|` **before** Jacobi, which is what makes `σ_min > 0`
true after the downward nudge. `m_sigmaSource` (three-state) carries the
provenance; `m_sigmaExact` survives as a derived bool assigned at the one
site that assigns the source, and both are copied in `CloneStateTo`.

| Phase-3 S2 gate | verdict |
|---|---|
| warning-free `make -C build/make/rise -j8 all` | PASS |
| fast path within 1e-12 of the written reference on a rotation, a reflection, a uniform scale | PASS — 1, 1, 1.5 on both ends of each |
| Jacobi within 1e-9 on `(3, 1, 0.4)` | PASS — σ_max 3, σ_min 0.4 (the loose pair was 3.187 / 0.1181) |
| Jacobi within 1e-9 on a SHEAR | PASS — 1.6180339887 / 0.6180339887, the golden ratio and its reciprocal, derived by hand from `M^T M`'s eigenvalues `(3 ± √5)/2`; the same shear at zero sweeps falls back to 2.0 / 0.25 |
| the four-ulp widening goes OUTWARD | PASS — `σ_max ≥` and `σ_min ≤` the written reference asserted on both Jacobi fixtures |
| `(3, 1, 0.4)` search-radius inflation 8.47× → 2.5× | PASS — **8.46667× → 2.5×**, both computed in the test from the same matrix by the same routine (the "before" number comes from a zero-sweep call, not from a quotation). σ ratio 26.9873 → 7.5, matching §5.6's two figures |
| exactness is NOT claimed: unsigned ≥ and lower bound ≤ the truth | PASS — at `(0,5,0)` on the `(3,1,0.4)` solid: true 4, unsigned 12 (over-report **3.0×**, down from 12.7499 / **3.19×**), signed lower bound 1.6 (up from 0.472441); `exact = false` still, since Jacobi is not a similarity |
| the `Loose` state is reached by a zero-sweep unit call checking the RETURNED state | PASS |
| the degenerate refusal runs BEFORE Jacobi, pair zeroed | PASS |
| `ProximitySignalTest` | 238 passed, 0 failed (211 after S1) |
| `ProximityInvalidationTest` | 25 passed, 0 failed |
| `MeshClosestPointTest` | 65 passed, 0 failed |
| `SurfaceSignalsTest` | 318 passed, 0 failed |
| `SourceHygieneTest` | 165 passed, 0 failed |
| `GeometryUVRoundtripTest` / `CSGObjectIdentityTest` / `CsgOperandTransformTest` | PASS (18 and 36 checks respectively) |

One measured side effect worth recording: scene C's `n_scaled_sdf`
(`scale 2 1 1`) now reports **4.0** where it reported **4.899** — its
σ_max falls from `‖M‖_F` = 2.449 to the true 2. Still an upper bound on the
true 2, and the existing bound assertions were written against the truth
rather than against the reported number, so they did not move.

#### S3 — CSG composites

`CSGObject` gains `DistanceToSurface`, `SignedDistanceLower` and
`DescribeKind` (none carrying `override`, matching that class's documented
rule), plus four protected helpers: `LocalBoxDiagonal` (the box-diagonal
computation hoisted out of `SelfHitRootFloor`, computed lazily per query and
never cached), `ComposedSignedLocal`, `LandingAdmits` and
`BracketDistanceLocal`. `DistanceToSurfaceWithArm` is the same query with the
landing arm reported — `DistanceToSurface` is a one-line forward to it, so
there is one code path; it exists because two §8 gates ask which arm fired
and that is not observable from the number.
`ObjectManager::LogDistanceRefusal` now asks `IObject::DescribeKind()` instead
of `typeid(*GetGeometry())` and its parenthetical is widened to "an SDF or
composite bracket that did not close"; the unbounded-confirm comment gains
composites as its third non-`O(1)` case, with §2's truthfulness caveat
extended verbatim.

| Phase-3 S3 gate | verdict |
|---|---|
| warning-free `make -C build/make/rise -j8 all` | PASS |
| a union of two spheres answers `min` within 1e-9 outside | PASS — 4 from above A (against B's 4.831) and 4 from above B |
| a union with a MESH operand answers, no refusal | PASS — the sphere's 2 where the sphere is nearer, the mesh's 0.5 where the mesh is; and its SIGNED query refuses, so the same union cannot be an intersection's operand |
| intersection vs a grid search of the SOLID by strict operand membership | PASS — 4 stations, grid spacing **0.02** (121³ samples over a 1.6 × 2.4 × 2.4 box); `lower ≤ reported` at every one; **gap_max = −0.00799** (reported sits *below* the grid's upper reference at every station, which is what `true ≤ reported ≤ reference` predicts) |
| subtraction vs the same grid search | PASS — 4 stations, grid spacing **0.02** (111³ samples); **gap_max = −0.02**, and every station's `reported` is its closed form exactly (0.5 / 0.3 / 0.7 / 0.2) on the BOUNDARY arm |
| exact-operand radial station of cylinder-minus-box `== closed form` to 1e-12 | PASS — **0.01 exactly**, BOUNDARY arm, gap 0 (`0.26 ⊖ 0.25` is exact by Sterbenz, so the landing's `f_A` is a true zero) |
| oblique station: `d ≤ reported ≤ d + gap_max`, arm recorded, `reported ≥ d` | PASS on two fixtures — cylinder off-axis (0.20, 1, 0.20): closed form 0.0328427, reported 0.0328427, gap **−1.4e-17**, STRICT; torus-minus-box at (−1.0, 0.8, −1.0): closed form 0.304586, reported 0.304933, gap **3.46e-4 = 1.21 ε**, STRICT |
| the phantom sweep — cylinder r 0.25 minus a 0.08 × 5.2 × 0.5 tangent slab, stations local `(x, 1, 0.26)` for `x ∈ [1e-7, 3e-6]` — ALL refuse | PASS — **60 of 60 refuse**; none reports the 1.00 cm tangency chord, and the true corner distance the refusal under-paints is **0.0421282** (4.21 cm), matching §5.6's recomputation |
| a subtracted PLANE operand makes the composite refuse | PASS, with teeth (the same box minus a SOLID answers 4) — and an INTERSECTION with the mesh operand refuses where the UNION answered |
| a nested intersection reports `exact = false` to its parent | PASS |
| a `scale (3, 1, 0.4)` SOLID operand takes the STRICT arm | PASS — arm STRICT, reported **4.03732** against a true 4 |
| a transformed composite matches the folded one | PASS **at one probe step, not at 1e-9** — see the deviation below. Transformed 0.01 (BOUNDARY, the closed form exactly), folded 0.0102547 (STRICT), difference **1.009 ε**. The box-minus-box twin agrees **exactly** (0.3 / 0.3) |
| inside operand A of an intersection but outside B reads a POSITIVE distance | PASS — 1.0, the closed form to the lens's near tip |
| a point inside the composite reads 0 | PASS — exactly 0, on both the lens and the fluted column |
| union-in-subtraction and intersection-in-subtraction compose | PASS — 2 and 3, their minuends' closed forms |
| `DescribeKind` names the operation, and follows a re-point | PASS — "csg subtraction" → "csg union"; an ordinary `Object` still names its geometry |
| scene C's `n_csg` is now a NEIGHBOUR through the manager | PASS — 2 at 1e-9 from the floor beneath it (this check previously asserted a refusal) |
| `ProximitySignalTest` | 325 passed, 0 failed (238 after S2) |
| `CSGObjectIdentityTest` / `CsgSurfacePayloadTest` / `CsgFloorOwnershipTest` / `CsgOperandTransformTest` / `CsgProbeFloorTest` / `CSGNullGeometryLuminaireCrashTest` | 18 / 348 / 72 / 36 / 49 / PASS, 0 failed |
| `ProximityInvalidationTest` / `MeshClosestPointTest` / `SurfaceSignalsTest` / `SourceHygieneTest` / `SceneSnapshotTest` / `ObjectMirrorTest` / `BoxGeometryTest` / `DeferredRealizeTest` | 25 / 65 / 318 / 165 / 107 / PASS / PASS / PASS, 0 failed |

Two deviations from §8's Phase-3 text, both found by the gates themselves:

- **"`gap_max` is one probe step, ε" is the step SIZE at the floor, not the
  bound.** The probe's first step is `max(ε, f)` and the descent exits its
  loop as soon as `f ≤ kBackoff·ε` with `kBackoff = 2` — so the residual
  handed to the probe can be up to `2ε` and the first step with it. Measured
  on the torus oblique station at **1.21 ε**. The tests assert `2ε`.
- **The transformed-vs-folded gate does not hold at 1e-9, and the reason is
  not the one §8 gives.** §8 asks for 1e-9 with an AXISYMMETRIC operand A,
  reasoning that folding a rotation into a BOX operand would grow its
  parent-frame AABB and move ε. That reasoning is right about ε — measured
  identical for the cylinder — and incomplete about the LANDING. In the
  unrotated frame the descent's arithmetic is exact by Sterbenz, `f_A` at the
  landing is a true zero, and the BOUNDARY arm fires with gap 0. Push the same
  point through a 45° rotation and that exactness is gone: `f_A` misses zero
  by ulps, the boundary arm declines, and the probe takes exactly one ε step.
  That is the `+1 ulp` miss §5.6 already describes ("the answer is `d` plus
  that step instead of `d`, which is the SAFE direction") — so the two agree
  to one probe step and their ARMS differ. The test asserts both are at or
  above the closed form, that the transformed one hits it to 1e-12, and that
  the pair agree within one probe step. The box-minus-box twin, compared at
  ~ε as §8 prescribes, happens to agree exactly.

#### S4 — `interior(r)`

`kFnInterior = 58` (only 59 now free before `CallFuncVec3`'s 60+ band), one
`Lookup` row, one named `case kFnInterior:` in `CallFunc` — the pin moves once
more, disclosed. `SignalKind::eInterior = 4`, `NeutralInterior()`,
`SurfaceSignalInfo::Interior(r)` beside `Proximity(r)` in
`SurfaceSignalProximity.h` sharing `MakeL1Key`, and
`IObjectManager::DeepestOtherContainment` over the same candidate set (the
three exclusions factored into `ObjectManager::ProximityCandidateCounts` so the
two queries cannot drift on them). `ExpressionProgram::UsesProximity()` becomes
`UsesCrossObject()`; `kL1Ways` goes 4 → 8.

| Phase-3 S4 gate | verdict |
|---|---|
| warning-free `make -C build/make/rise -j8 all` | PASS |
| `interior` is 0 outside every neighbour | PASS — exactly 0 (tolerance 0, not 1e-9) at five scene-C probes |
| the six interpenetration probes read `min(depth/r, 1)` within 1e-9 | PASS — box / sphere / capped cylinder / torus tube / ellipsoid / SDF at depths 1, 1, 1, 0.5, 1, 1; each checked at `r = 4` (reading depth/4) **and** at `r = depth` (reading exactly 1). The ellipsoid probe stays at the CENTRE, where its lower bound is tight, and the test says so |
| inside two overlapping spheres, the LARGER depth | PASS — 1.0 where the operands' own depths are 1.0 and 0.5; a point inside only the smaller reads its 0.1; a point inside neither REFUSES (which `interior` reads as 0) |
| inside a UNION composite's overlap the exported depth under-reads | PASS — at `(0.4, 0, 0)` between two R = 2 spheres 1.5 apart: operand depths **1.6 / 0.9**, exported **1.6**, true union depth **1.95959**. Asserted ≤ the truth *and* STRICTLY < it. The probe is deliberately OFF the mid-plane: review round 1 moved it there because at `(0.75, 0, 0)` both operand depths are 1.25 and `min`/`max` are indistinguishable — which is how the check's own claim was found to be inverted (`|min(f_A, f_B)|` is `max(depth_A, depth_B)`, the DEEPER, not the shallower) |
| a mesh neighbour contributes 0 | PASS, with teeth (the same mesh answers the UNSIGNED query at 0.5) |
| a computed radius is accepted | PASS — `interior(2.0*2.0)` equals `interior(4.0)` to 1e-12 at a real manager hit, with a non-zero reading (0.25) proving the agreement is not two neutrals matching |
| the parse diagnostic names a WORLD LENGTH, in `interior`'s own words | PASS — the ternary is three-way; `interior`'s message says "depth of burial", proximity's does not, and neither says FRACTION |
| an `interior`-only scene builds the eager snapshot | PASS — `ProximityInvalidationTest` (c) now probes three bodies: none (no snapshot), `proximity()` (snapshot), `interior()` only (**snapshot**) |
| the memo keys extend, red-proved | PASS — a new `ExpressionMemoTest` (p): at the centre of a box neighbour `proximity` reads 1 and `interior` reads 0.125 at the SAME hit and radius, **in both orders**, so `fn` is doing the separating; and the generation bump clears an `interior` entry with the stale answer red-proved first |
| `kL1Ways = 8`, the raised ceiling asserted and the bytes printed | PASS — **2432 bytes** exactly (asserted as an equality, not only under the 4096 ceiling), 43.8 kB across 18 workers |
| **L1 hit rate on `plank_closeup` unchanged** | PASS — **0.8967 → 0.8968**. Measured, not argued: a temporary hit/miss counter in `L1Find`, `plank_closeup` at 160 × 120 / 2 spp, `kL1Ways = 4` gives 544 479 hits / 62 698 misses and `kL1Ways = 8` gives 543 931 / 62 607. The counter and the probe scene were removed in the same slice (`grep L1PROBE src` empty). Worth measuring rather than reasoning: round-robin replacement is FIFO, and FIFO is not a stack algorithm, so more ways *can* in principle lower a hit rate (Belady) |
| `TextureExpressionVMTest` 846/846 | PASS (unmoved) |
| `AgentSkillsTest` at 33 snippets, the new `interior` fence deriving with zero diagnostics and rendering | PASS — 628 passed, 0 failed; the fence renders 128 × 128 at mean RGB (0.2565, 0.2633, 0.2761) |
| `ProximitySignalTest` | 386 passed, 0 failed (325 after S3) |
| `ExpressionMemoTest` | 205 passed, 0 failed (196 before) |
| `ProximityInvalidationTest` | 26 passed, 0 failed (25 before) |

Beyond §5.6's list, two stale claims the plumbing pass found and corrected —
both are the doc-fidelity lens rather than new behaviour:

- The two `expression_painter` / scalar-pipe descriptor strings still said
  **"CSG composites … contribute nothing"** and **"triangle meshes … contribute
  nothing"** to `proximity`. The mesh half went stale in Phase 2 and the
  composite half in S3 above; both are rewritten, and the composite sentence now
  states the refusal that DOES remain (an intersection or subtraction with a
  sheet operand). The same two strings' "ALL FOUR SIGNALS (curv, occlusion,
  convexity, thickness)" is now "ALL SIX".
- `skills/agent/procedural-textures.md` carried the same stale sentence
  ("triangle meshes, patches, hair and CSG composites contribute nothing"),
  corrected there too.

One design detail decided at implementation time and recorded here:
`DeepestOtherContainment` uses the **flat AABB scan even where a TLAS
exists**, unlike `NearestOtherSurface`. The top-level tree prunes on "this
subtree is further than the running best", which a running MAXIMUM has no
use for, so walking it would visit every leaf anyway with the traversal's
overhead added. `interior` is therefore linear in object count on every
scene — added to §10.

#### S5 — `add_wear`'s contact term

Two new parameters on the verb, `contact_radius` (world length, default 0 =
off) and `contact_grime` (default 0.5, clamped to [0,1] at the entry point).
`BuildWearMaskPreludeText` gains three `param` lines and a `def contact_mask`,
and the existing `crevice_mask` line grows one addend — the SHARED prelude, so
the colour and roughness chunks agree by construction and their two consuming
lines are untouched. The `WearMaterial_` fill is hoisted above clause (c)'s
test and barren records go to a new `wearBarrenCandidates` list that only a
`contact_radius > 0` call reads. A barren pick gets the CONTACT recipe: no
`curv`, no `occlusion`, no `wear_mask`.

| Phase-3 S5 gate | verdict |
|---|---|
| warning-free `make -C build/make/rise -j8 all` | PASS |
| refuses on a body already reading `proximity` / `interior` | PASS — both, each with the clause-(d) refusal and a byte-identical document afterwards |
| a curving receiver calls `proximity(contact_r)` EXACTLY ONCE per chunk, via `crevice_mask` | PASS — 1 in the colour chunk, 1 in the roughness chunk, counted per chunk (the shared prelude is in both, so a whole-document count would have been 2 and meaningless) |
| the two CONSUMING lines are byte-identical to today's | PASS — `mix(mix(base, edge_tint, wear_mask), patina_tint, crevice_mask)` and its roughness twin, asserted verbatim |
| the `crevice_mask` line is wrapped in `clamp(…, 0, 1)` on BOTH recipes | PASS — `clamp(crevice_raw*cavity_boost + contact_grime*contact_mask, 0, 1)` and `clamp(contact_grime*contact_mask, 0, 1)` |
| the radius is the retunable `contact_r` param; `occlusion(0.08)` stays a literal | PASS — both asserted in the same chunk, which is where the contrast lives |
| a flat BOX receiver is accepted from the barren list, and the scene derives | PASS — `add_wear { material: "mat_deck", contact_radius: 0.01 }` applies on a `box_geometry` receiver; the document derives, validates with ZERO error diagnostics, and renders non-black |
| the same receiver WITHOUT the argument is still refused, in the same words | PASS — "planar or patch geometry", unchanged |
| the barren body has no `curv`, no `occlusion`, no `wear_mask` | PASS — plus no `edge_tint` / `edge_desat` / `edge_lift` / `rough_polished`, each asserted absent |
| every `param` either barren chunk declares is referenced outside its own declaration | PASS — asserted BY NAME over the emitted text (whole-word count ≥ 2 per declared name), since the VM has no such check |
| `qualifyingMaterials` keeps reporting the CURVED count | PASS — 0 on a scene whose only qualifying-shaped material is the flat one, while the call still applies |
| the bare call selects over the UNION when contact is on | PASS — takes `mat_deck`, the flat receiver |
| `contact_radius 0` is byte-identical: today's `crevice_mask` line, today's consuming lines, and NOT ONE `contact_` token or `proximity(` call anywhere | PASS |
| exception 1 — clause (d)'s corrected string, pinned by a NEW check naming its new text | PASS — the message now names `curv` / `occlusion` / `thickness` / `convexity` / `proximity` / `interior`; the existing check matches only the opening substring, which is why this needed its own |
| exception 2 — the bare-call message's appended clause, pinned by a NEW check | PASS — the opening words are unchanged (asserted) and the appended clause names `contact_radius` |
| the MCP and chat tool COUNTS do not move (43 / 38) | PASS — `AgentMcpStdioSmokeTest` 21/21 pins 43, `AgentChatLoopTest` 1745/1745 pins 38 |
| `AgentAddWearTest` | 351 passed, 0 failed (287 before) |
| `AgentAddWetnessTest` / `AgentSkillsTest` / `AgentReadValidateTest` / `AgentAutonomyPolicyTest` | 210 / 628 / 341 / 404, 0 failed |

One bug the gates caught in this slice's own code, recorded because the
symptom was unhelpful: the bare-call union pool was declared inside the
`else` branch while `pick` pointed INTO it, so every later `pick->` read was
a dangling one — the verb reported ``rebinding `` on `` did not take``. The
vector now lives at function scope. A second, in the test rather than the
code: `add_wear` mints `X_wear` and `X_wearrough` and splices the roughness
chunk last (so it lands first), so a plain `find("name X_wear")` returned the
ROUGHNESS chunk when asked for the colour one — the helper now requires a word
boundary and a `name` line.

#### S6 — the showcase, and the pins

`glass_pavilion`'s `colcylgeom` gains one line, `capped TRUE` — the parser
default, spelled out as a PIN with the reason at the chunk: an OPEN cylinder is
a SHEET, refuses the signed query, and would make every fluted column stop
answering `proximity()` for every neighbour in the scene. The tracked scene is
not otherwise edited. `ProximitySignalTest` (l) drives the gate against that
scene, expressing every station in the COMPOSITE'S LOCAL FRAME and pushing it
through `column2`'s own final matrix, so the test cannot disagree with the
engine about what `orientation 0 45 0` means.

| Phase-3 S6 gate | verdict |
|---|---|
| the `capped TRUE` pin has teeth | PASS — `colcylgeom` answers the signed query AND carries the exactness flag, which is what admits a boundary landing on the column wall |
| cap-top stations at 1 / 2 / 4 cm from the nominal wall, each within 0.05 | PASS — **0.5 / 0 / 0**, and the 1 cm station is **exactly 0.5** (the boundary arm fires: the radial landing IS the nearest point by symmetry and the cylinder operand is exact). Also asserted never above 0.5, and never below the one-probe-step floor 0.4874 |
| the flute station at local `(0, ·, 0.26)` reads `proximity(0.02)` = 0 | PASS — **exactly 0** to 1e-12. A phantom landing on the tangency would have read 0.5 |
| a direct PER-OBJECT `column2->DistanceToSurface` at radius 0.1 REFUSES | PASS — and the SCENE-WIDE query at the same radius ANSWERS **0.075** (the floor top at y = 0.1 under the cap top at 0.175), which is exactly why the refusal has to be asked per object |
| the true corner distance the refusal under-paints | **0.0421282 m** (4.21 cm), recomputed in the test rather than quoted — four times the 1 cm a phantom would report |
| `CstDeriveGoldenTest` — the digest does not move | PASS — **447 MATCH, 0 DRIFT** (of 447 golden scenes; 454 corpus, 0 UNCOVERED, 0 STALE) |
| `ProximitySignalTest` | 401 passed, 0 failed (386 after S4) |

**The probe-albedo crop, judged honestly.** Harness-only, on a COPY: the
tracked `marble_col` (a lambertian, bound to a uniform white) has its
reflectance swapped to `expression_painter { expr vec3(proximity(0.02),
proximity(0.02), proximity(0.02)) }`, a bright `ambient_light` is added so the
irradiance over the crop is near-uniform and the image reads as the albedo up
to one global constant, and the camera is dropped to 3.45 / 0.62 / 3.45 looking
at the `cap2` / `column2` junction. 420 × 320, 24 spp, `pixelpel`. What the
crop actually shows:

- **The contact ring is drawn, and it is the money.** The cap's top face is
  black everywhere except a clean white band ~2 cm wide following the column's
  cross-section exactly — including around the flute slot's mouth, where the
  slot WALLS are real surfaces of the composite and legitimately within 2 cm.
  Before Phase 3 the whole face was black, because `column2` refused.
- **Everything else on `marble_col` is black**, which is correct rather than
  disappointing: nothing else in frame is within 2 cm of another surface.
- **The phantom band is absent.** A tolerant landing test would have painted a
  band on the cap top 1 cm outside the slot's tangent FACE — which is where
  the test's flute station sits, and it reads 0.
- **Two honest caveats.** The bright floor in the crop is NOT the signal: the
  floor binds `polished_floor`, a different material the copy does not touch.
  And the column reads as two separate bars because the scene's single flute
  slab is 0.5 deep — the column's whole diameter — so it cuts right through;
  that is the tracked scene as authored, and it is the same fact that makes
  the slot's ±z faces tangent to the cylinder and the phantom possible at all.

#### Review round 1 — two P1s, both in code the gates passed

Four orthogonal reviewers on the six committed slices. Two P1s, and neither
was a slip: each is a rule **§5.6 itself states** that turns out to be
unsound, so the fix is a documented deviation from a converged design rather
than a correction to the implementation of it. Both are now pinned by
fixtures the original gates could not have caught.

**P1-A — a UNION must NOT export the exactness flag (§5.6 says it may).**
`min(f_A, f_B)` is 0 not only on the union's boundary but on every INTERIOR
point where the operands' boundaries meet from opposite sides — a shared
face, a set of positive area. Two boxes stacked into a cube read 0 all over
the seam plane. Flagged exact, that lets a parent subtraction's boundary arm
(`exB ∧ f_A < 0 ∧ f_B ≥ 0`) admit a landing strictly inside the subtrahend.
Measured on `box(2,2,1)@−0.5 ∪ box(2,2,1)@+0.5` subtracted from a 4-cube,
probed from the cavity at `(0,0,−0.25)`: **reported 0.25 against a true
0.75** — contact painted 3× too close, the one direction §2 forbids. The
descent's single step lands on the seam *exactly* (Sterbenz), so it is not a
knife-edge coincidence; the whole slab `z ∈ [−0.5, −0.25]` does it, and a
query point ON the seam reads 0. **Why the gate missed it:** §8's
union-in-subtraction row uses two OVERLAPPING spheres, whose intersection
circle lies ON the union's boundary — the one configuration that dodges it.
**Fix:** no composite exports exactness. Every other consumer of the field
reads a strict sign, so nothing else moves. An ABUTTING fixture is now pinned
beside the overlapping one, asserting the seam reads exactly 0, that the flag
is false there, and that the carved cube never reports below 0.75.

**P1-B — the four-ulp σ widening does not bound a relative error.** A
reviewer replicated `ComputeSigmaExtremes` in exact arithmetic (reference
singular values from the rational `MᵀM` cubic, Newton-refined to 80 digits)
and broke the claim on an **ordinary well-conditioned matrix** — cond 29.6,
every entry O(1) — where the computed `σ_min` sat **twelve ulps** above the
true one, so four downward nudges left the stored number still above it and
`d_w ≥ σ_min·d_o` false of what is actually stored. 18 of 7 500
`R₂·diag(3,1,0.4)·R₁` transforms (a `scale (3,1,0.4)` object under a rotated
parent — authorable today) violate it; at cond ~3e12 the gap is **6.4e-5
relative**, which no ulp count reaches. Practical magnitude is ~1e-15
relative and nothing renders differently; what was wrong is the *claim*, in
three code sites and the design. **Fix:** the widening is now RELATIVE and
condition-scaled, `16·eps·(σ_max/σ_min)`, capped at 0.5. Widening is free in
the safe direction — it can only make the unsigned answer larger and the
signed one smaller — so this is a heuristic tied to the error's measured
shape, and it is now described as one rather than as a guarantee.

Also fixed in the same round:

| finding | fix |
|---|---|
| the `interior` computed-radius check was a **tautology** — on a provider-less plane a mis-compiled `convexity` reads its neutral 0 and `interior` reads 0 too, so `0 == 0` passed with the DynR guard removed | moved to the **SDF** receiver, which publishes a provider: `convexity(0.5)` reads a real non-zero there (asserted, as the third probe) while `interior` reads 0, so the two are no longer confusable |
| the grid gate asserted only `gap_max` — the **harmless** side. A composite that under-reported at every station (contact painted where there is none) passed | added the LOWER-side guard `reported ≥ ref − cellDiagonal` per station and a `gap_min` summary, plus `ref < RISE_INFINITY` so an all-refusing operand cannot make the check vacuous. This would have caught P1-A |
| the union-overlap probe sat on the mid-plane, where both operand depths are 1.25 — `min` and `max` are indistinguishable | moved to `(0.4, 0, 0)`: depths 1.6 and 0.9. **The move immediately failed the check and showed the claim was inverted**: `min` is taken on the SIGNED values, so `|min(f_A, f_B)|` is `max(depth_A, depth_B)` — the DEEPER of the two (1.6), which is also the CORRECT lower bound, since leaving the union means leaving both. The assertion and the surrounding prose now say the deeper |
| the phantom sweep had **no positive control** — "the guard refuses the phantom" was not separated from "this composite refuses everything near that face" | added a station one slot-width further out in x, where the same composite at the same radius must ANSWER at its closed form |
| `SignedAt` pre-cleared `outExact`, defeating the `true` sentinel the sheet loop sets and reducing nine "the refusal CLEARS the flag" checks to "does not SET it" | the helper no longer pre-clears |
| `ParamValueInChunk` (pre-existing) carried the same prefix bug `ChunkTextOf` was hardened against — asked for `X_wear` it matched inside `X_wearrough`, so the colour chunk's contact params were never actually asserted | word-boundary match |
| `LocalBoxDiagonal` was **duplicated**, not hoisted, while three places said "hoisted" | `SelfHitRootFloor` now calls it; the A-only rationale moved into the helper |
| `LandingAdmits` selected its arms with `intersection ? … : …`, so a union would silently take the subtraction disjuncts (unreachable, but a trap) | explicit refusal for any other op |
| `SDFGeometry::SignedDistanceLower` could return `EvaluateParts`' `+1e30` "nothing here" sentinel as a lower bound (a part list whose first op is `subtract`/`intersect`) | screened, as the unsigned query already is by its bracket |
| the fast path's 1e-9 similarity tolerance set the exactness flag on a `1 + 7e-10` anisotropy — harmless in Phase 1, but Phase 3 gave that flag a consumer that treats it as "the magnitude IS the distance" | tightened to **1e-12** (four orders of headroom over a composed Euler rotation's ~1e-16 drift), with the residual window disclosed on the flag itself rather than claimed away |
| `sqrt(alpha*beta)` in the Jacobi threshold overflows to `inf` above column norms ~1.2e77, after which no pair rotates and the loop declares CONVERGENCE on raw column norms | two separate square roots |
| the `add_wear` bare-call clause about `contact_radius` printed even when the caller **had** passed it | gated on `!contactOn`, matching §5.6's "advice for exactly that call" |
| `contact_r` was emitted with a fixed `max 1`, so a metre-scale radius of 2 sat under its own slider max and the first slider touch would halve it | bounds widen to contain the value, as the roughness chunk's `sliderMax` already does |
| six enumerating comments falsified by this arc (`UsesCrossObject`'s own doc still said "proximity specifically"; `CallFunc`'s "ONE case"; "the CROSS-OBJECT signal"; the `SurfaceSignalProximity.h` include note; `ProximityDemand::Any`'s one-liner; a subject/verb break this arc introduced into two descriptor strings) | corrected |
| the skill's `interior` fence taught the **wrong model** — "1 cm below the surface" was 5 cm, and the mask is not monotone in a shallow pool (it saturates at mid-water, because `interior` measures distance to the NEAREST face of the container, including its floor) | the pool is now 60 cm deep so the surface really is the nearest face over the stone, the numbers are recomputed (0 at the waterline, 0.5 one cm under, saturated from 2 cm down), and the nearest-face rule is stated as the first thing that decides whether the signal is the right tool |

Two findings recorded and NOT acted on, with reasons: `DeepestOtherContainment`
could prune on saturation (`best ≥ maxDepth`) — exact and free for the one
caller, but the design deliberately specifies no prune and a direct caller
need not pass the radius as the budget; and a numerically-singular matrix
still passes the `|det| > 0` gate — unchanged from Phase 1, now stated at the
gate as a residual rather than a guarantee.


---

## 9. Test plan

- `tests/ProximitySignalTest.cpp` on scene C: closed forms per exact family
  (plane, sphere, box, capped and open cylinder, disk, clipped plane, torus);
  the ellipsoid bound; the composed-SDF bound with `gap_max`; the exact single-
  sphere SDF at 1e-9; self-exclusion; the instanced copy counts (named); a
  `casts_shadows FALSE` neighbour counts; an emissive neighbour does not (the
  fixture is a hand-authored emissive box, so the disclosed decorative-object
  exclusion is what the test pins); CSG operands never count, and the composite
  ANSWERS since Phase 3 (this check asserted a refusal until 2026-09-09); a heightfield SDF refuses **asked directly**, so the check
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
- **Phase 3 adds five sections to the same file** (§8.4): **(h)** the SIGNED
  lower bound per family -- exact sign, lower-bound magnitude, the exactness
  flag, every sheet refusing it while still answering the unsigned one, and the
  `(3, 1, 0.4)` solid bracketing a brute-force distance from opposite sides;
  **(i)** the three σ branches against references written into the test, and
  the `Loose` state reached by a zero-sweep call; **(j)** CSG composites --
  union `min`, the bracket's two landing arms recorded per station, grid
  searches of the SOLID by STRICT operand membership, the phantom sweep that a
  tolerance would have admitted, the composite's own transform layer, and the
  nesting rules; **(k)** `interior(r)`; **(l)** the `glass_pavilion` showcase,
  driven against the TRACKED scene with every station expressed in the
  composite's LOCAL frame and pushed through its own final matrix.
- `tests/ExpressionMemoTest.cpp`: the four new `SignalHitKey` fields separate in
  both L1 and L2 rows; the generation clears the query memo (red-proved); the
  TLS ceiling. **Phase 3 adds (p)**: `interior` (fn = 4) keys apart from
  `proximity` (fn = 3) at the SAME hit and radius, in BOTH orders, and its
  entry clears on a generation bump with the stale answer red-proved first.
- `tests/AgentAddWearTest.cpp`: **Phase 3 adds (P)** -- `contact_radius` 0
  emitting today's body to the line, the contact term entering the SHARED
  prelude with both consuming lines byte-identical, a flat BOX receiver
  accepted from the barren list with a body carrying no `curv` and no
  `occlusion`, every declared `param` asserted BY NAME to be read, and the two
  pinned exceptions to the byte-identity claim.
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
  composites until Phase 3 — **after it (SHIPPED 2026-09-09)**, an
  intersection/subtraction with a
  sheet or heightfield-SDF operand, a union of two refusing operands, a bracket with no admitted
  landing in budget, and a seam gradient below 1e-12) read far. An
  intersection/subtraction answers within `[d, d + gap_max]` — never below
  `d` — with `gap = 0` when an exact-operand landing's arm fires, one probe
  step (ε on the fixtures; a solid thinner than ε at the landing takes the
  doubling steps or refuses) when rounding sends an exact landing to the
  probe, and the measured `gap_max`
  (probe step ε = 5e-5 of the local diagonal, floored at 1e-6, its first
  term) when a bound operand is reached, scaled by the composite's σ_max in
  world space; a point exactly on its surface reads 0 in the first case and
  within the measured gap otherwise; an intersection/subtraction
  is never an exact operand to a parent; `interior` under-reads inside a
  union's overlap (the exported `min` is a depth lower bound there).
- SDF neighbours are an upper bound on distance (never over-read contact),
  bounded by the last probe step; the gap is measured on C, not bounded
  analytically. A candidate whose crossing is not found within budget reads far.
- Non-uniform scale and ellipsoids are upper-bounded, not exact (Phase 3 makes
  σ exact but `×σ_max` remains a bound attained only along the top singular
  vector), and an anisotropically scaled solid never carries the exactness
  flag, so a composite reaching it always probes.
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
  compiled program calls `kFnProximity` (after Phase 3, `kFnInterior` too —
  §5.6's `UsesCrossObject()`). Ungated, the per-ray call was a load, a
  compare and a heap indirection on the hottest path in the renderer, charged to
  every scene in the tree. Correctness does not rest on the counter:
  `NearestOtherSurface` still builds lazily under `treeCreationMutex` when asked
  with no snapshot. **The trade:** an unregistered consumer — a hypothetical
  direct caller of `NearestOtherSurface` — pays the whole build under the lock on
  its first call instead of finding it ready.
- ~~**`kL1Ways == 4` now exactly equals the number of signal KINDS.**~~
  **SUPERSEDED by Phase 3 S4 (2026-09-09): `kL1Ways` is 8, `Tables` is 2432
  bytes, the asserted ceiling is 4096, and the cliff moved from a FIFTH
  distinct (kind, radius) query per hit to a NINTH — it did not disappear.
  Measured, not argued: the L1 hit rate on `plank_closeup` is 0.8967 at four
  ways and 0.8968 at eight (§8.4 S4), which is worth measuring because
  round-robin replacement is FIFO and FIFO is not a stack algorithm.** The
  original reasoning, kept because the cliff itself is unchanged in shape: When the
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
  holds up to four objects; the traversal first evaluated all of a leaf's
  occupants once the leaf's box was within the running best (11.08 distance
  calls per query on Sponza against the flat scan's 4.64), and review
  round 1 added a per-element box pre-test on the TLAS path that halves
  that to 5.617 calls per query (§8.3) -- without moving the wall clock
  (1.094× on five pairs), so the remaining cost is the traversal itself,
  not the distance calls, and a scene with expensive-to-answer neighbours
  (composed SDFs in a leaf) is still the first place to look if one
  regresses.
- **A mesh neighbour is a SHEET, and no other shipped family is.**  Every
  solid family clamps a signed field at zero, so a point inside reads 1;
  a point inside a closed MESH reads its honest distance to the nearest
  triangle instead, because a triangle soup carries no inside test.  The
  direction is safe (an over-report under-paints) but the inconsistency is
  real: a receiver buried inside a mesh neighbour will not read contact.
  A signed/inside variant for meshes needs a robustly closed-mesh test.
  **Phase 3's `interior(r)` did NOT close this**: it is the signed variant for
  every SOLID family, and a mesh -- being a sheet -- contributes 0 to it, so a
  receiver buried inside a closed mesh reads `interior` 0 as well as
  `proximity` its honest distance. The closed-mesh test is still the missing
  piece.
- **`standard_object`'s `scale` written with ONE number derives to a
  DEGENERATE transform, silently.**  It is a `DoubleVec3`; `scale 0.35`
  produces no diagnostic, makes the object vanish from the render, and
  makes it refuse every proximity query (`Object::DistanceToSurface`
  rejects `sigma_min <= 0`).  Found while authoring scene D and verified by
  re-introducing it (§8.3, trap 4).  A parser gap, outside this design's
  scope, recorded here because it is a live trap for anyone placing an
  object for a contact scene.
- `proximity` is unsigned; Phase 3's `interior(r)` **ships** and supplies the
  inside half for the solid families only (meshes and every sheet family
  contribute 0 to it, silently — a shared refusal latch would print the
  proximity message for every mesh and plane in the scene).
- **`interior` sees a TLAS-backed scene's object set through the AABB
  SNAPSHOT, not through the tree — so it is stale in a DIFFERENT way from
  `proximity`.** `DeepestOtherContainment` calls `EnsureBoxSnapshot()`,
  which carries the add-detecting entry-count check; `NearestOtherSurface`
  on a TLAS-backed scene walks `pBVH`, which has none. After a
  `Job::AddObject` with no `InvalidateSpatialStructure` on a scene of more
  than four objects, the new object is invisible to `IntersectRay` and to
  `proximity` (§8.3's "the signal is exactly as stale as the render") and
  VISIBLE to `interior`. That is the OVER-paint direction — burial read
  from geometry that is not in the frame — and it is the one place this
  design has it. Not fixed: the fix is either a count check on the TLAS
  (which §8.3 deliberately declined) or a tree walk for a MAXIMUM query
  (which has no pruning to offer). Stated, not tolerated.
- **`interior` is LINEAR in object count on every scene, including a
  TLAS-backed one** (added Phase 3 S4). `DeepestOtherContainment` walks the
  flat AABB snapshot with an ordinary containment test rather than the
  top-level tree, because the tree prunes on "this subtree is further than the
  running best" and a running MAXIMUM has no use for that — walking it would
  visit every leaf anyway with the traversal's overhead added. `proximity` on
  a TLAS-backed scene is not linear; `interior` is.
- **`interior` UNDER-READS inside a UNION composite's overlap.** A union
  exports `min(f_A, f_B)` as its signed lower bound, which is exact ON and
  OUTSIDE the zero set and only a lower bound INSIDE: at a point deeper in the
  union than either operand alone, the min is the SHALLOWER of the two depths.
  Measured (§8.4 S4): 1.25 exported against a true union depth of 1.85405.
  The under-paint direction, and asserted as a strict inequality rather than
  claimed.
- The query reads other objects' transforms and so joins the pre-existing
  per-sample `EvaluateAtTime` race (ARCHITECTURE.md), no wider than
  `Object::IntersectRay` already does.
- The scalar pipe stamps `time = 0` (no `m_time` there), so its memo entries
  rely on the jitter argument alone under motion blur.
- ~~Until Phase 3, non-uniform transforms use the Frobenius/determinant σ
  bounds, which are loose.~~ **DONE in Phase 3 S2 (2026-09-09):** the one-sided
  Jacobi ships, with the same Frobenius/determinant pair as the
  non-convergence fallback. **Measured on `scale (3, 1, 0.4)`:** the search
  radius inflation fell **8.46667× → 2.5×** and the σ ratio **26.9873 → 7.5**,
  both derived in the test from the same matrix by the same routine (§8.4 S2).
  What did NOT go away: `×σ_max` is attained only along the top singular
  vector, so the unsigned answer at `(0, 5, 0)` on that solid is still **12**
  against a true **4** — an over-report of **3.0×**, down from 3.19×. Phase 3
  removed the PAIR's slack, not the anisotropy.
- The memo-eligibility threshold moves with `kFields` (29 → 35) for
  pure-arithmetic bodies; bit-identical, perf-only, disclosed. `ptWorld` is a
  redundant compare in the L2 key.
- ~~**A CSG composite refuses, so its OWN surface is invisible to every
  neighbour's query.**~~ **RETIRED by Phase 3 S3 (2026-09-09.)** A union now
  answers `min` over the operands that answer; an intersection or a
  subtraction brackets the composed signed field. What REMAINS of it is
  narrower and is stated in the refusing-families bullet above: an
  intersection or subtraction with a SHEET or heightfield-SDF operand, a union
  of two refusing operands, a bracket that finds no admitted landing in
  budget, and a seam gradient below 1e-12.
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
