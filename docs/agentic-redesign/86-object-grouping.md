# 86 — Hierarchical Object Grouping

**Status: DESIGN (2026-08-14) — no code.**  Successor concern to the
geometry-expressiveness work in [85](85-geometry-expressiveness-candidates.md):
richer geometry made individual objects better; grouping makes SETS of
objects addressable, placeable, and reusable — by the agent, by the GUI,
and across sessions.

---

## 1. What is actually broken

RISE's scene graph is flat.  `GenericManager` is a `map<String, T*>` with
no parent pointer, no child list, nothing beyond `name → pointer`
(`src/Library/Managers/GenericManager.h:91`).  Every "grouping" in the
system today is a workaround:

- **The agent's `element` concept is session memory.**
  `AgentSession::mChunkAttribution` is a plain member vector populated by
  `AttributeChunkToActiveElement_`; it is never written to the
  `.RISEscene`, so it dies with the session and is invisible to the GUI
  and to any reload.  `ElementChunkNamePrefix` is NOT a lookup key — it
  only composes prompt text.
- **`place_element` does per-object text surgery.**  It re-parses the
  document, reads each member's `position`/`scale`/`orientation` param
  strings, does the composition in agent C++, and emits up to three
  patches per object.  It SKIPS objects authored with `matrix`, warns
  and skips rotation for `quaternion`, and reports `approxObjects`
  because summing Euler angles per-axis is only exact when both
  rotations share an axis.  This is the verb the creativity journal
  measured being called 24 times without converging.
- **`finish_element`'s isolate render guesses.**  With several members it
  picks the ONE with the largest bounding-box diagonal.
- **`scene_inventory` has no rollup.**  Every object is listed flat;
  grouping exists only by failure reason.
- **`instance_array` produces no group.**  It expands to N independent
  `standard_object`s named `name[i,j]` with no residual handle — which
  is why `ResolveIsolateObject` needs a special case just to say "that's
  a generator name, pick one instance."  It instances *repetition*, not
  *composition*: one geometry template, no per-member material variation.
- **Both GUI outliners are flat two-level trees** (category → entity
  list), single-selection, refreshed off a coarse `sceneEpoch()` counter.

## 2. The architecture, and where the recon's version differs

The recon proposed a derive-time post-pass modeled on
`Cst::ExpandInstanceArray`.  **Rejected, for one reason:** because
`instance_array` is not a registry chunk, the static reference graph
skips it and `DeriveToJobIncremental` globally refuses whenever one is
present, falling back to a full re-derive.  That is affordable for
instance arrays (rare).  Groups would appear in essentially every
agent-built scene, so every `propose_patch` would pay a full re-derive.

**Post-pass expansion and flatten semantics are separable.**  Take the
second without the first:

1. **`group` is an ordinary registry chunk.**  Its `member` parameter is
   a repeatable `ValueKind::Reference` with
   `referenceCategories = {ChunkCategory::Object}` — exactly the shape
   `standard_shader`'s repeatable `shaderop` already uses.  Because the
   references are declared, the reference graph traces them and
   incremental derive keeps working normally.  No global refusal.
2. **Members must be declared before the group.**  This is the rule
   `standard_shader` and `file_rasterizeroutput` already live under
   (`DeriveToJob`'s PASS-2 applies chunks in document order and resolves
   references by immediate manager lookup).  Enforcing it at parse time
   with a clear diagnostic costs one check and removes the need for any
   post-pass or topological sort.  The save path serializes the
   `Document` in item order, so an editor that inserts a `group` after
   its members round-trips correctly; an editor that moves it earlier
   must be prevented from doing so.
3. **The group's transform is composed into its members at derive time**,
   before `ObjectManager::PrepareForRendering()` builds the TLAS.  This
   satisfies the render-immutability rule for free — resolution is over
   before the Prepare phase starts, using the same single-threaded Setup
   discipline `DisplacedGeometry::Realize` relies on.
4. **Zero traversal change.**  `ObjectManager::IntersectRay`,
   `RayElementIntersection`, `CreateBVH`, and `CSGObject` are untouched.
   The TLAS still sees a flat list of objects carrying pre-composed
   world transforms.

True nested instancing (a `GroupObject : Object` that transforms the ray
at traversal, mirroring `CSGObject::IntersectRay` but N-ary) stays
available behind the SAME authoring surface.  A scene that says
`group { … }` does not change when the implementation does — the same
property that made C2's sweep affordances swappable.  Defer it until
there is demand for shared/instanced *composition*, which
`instance_array` does not cover.

## 3. Open decisions

**How is a group addressed by isolate / GUI selection?**  The recon
suggested registering a "fictional handle" — a never-hit `IObjectPriv`
in `ObjectManager` — so `ResolveIsolateObject` and
`ViewportBridge::selectionName()` work through existing single-object
machinery.  That is cheap and tempting, but it puts a phantom row in
`scene_inventory` that never covers pixels, and the census's entire
value is telling a model where things actually are.  A never-covered
phantom is exactly the kind of noise that misled the model in arc 79.

Alternative: keep a real side index (group name → member names) and
teach the three consumers that need it.  This costs more code in three
places and requires `isolate` to accept a SET (it resolves exactly one
object today).  **Decide before slicing.**  Lean: side index, because
the inventory surface is load-bearing and should not carry fictions.

**Does the group own its members?**  Follow `CSGObject`'s discipline
exactly: one-directional `addref` from parent to child, never a child →
parent back-pointer.  `Reference` has no weak-ref primitive and no cycle
detection, so a back-pointer is the one change that would introduce a
genuine leak.  Any "which group owns me" query is a side index, not a
field on the object.

**Do members stay independently visible?**  Under flatten semantics,
yes — they remain ordinary renderable objects.  `SetWorldVisible(false)`
(CSG's mechanism) is only needed under true nested instancing.

**Nesting depth.**  Groups containing groups is natural under both
designs but multiplies the ordering and cycle-validation surface.  V1:
single level, validated.  Note it explicitly so the grammar can grow.

## 4. Slices

1. **`group` chunk + derive-time composition.**  Parser, member
   references, declare-after-members validation, transform composition,
   round-trip test, rejection tests.  No agent or GUI change.
2. **`place_element` collapses to one patch.**  When an element maps 1:1
   to a group, emit a single patch against the group's transform instead
   of N against members.  This deletes the `matrix`/`quaternion`
   exclusions and the `approxObjects` non-commutativity caveat as
   *categories of problem*.  Highest agent value in the workstream.
3. **Persist element grouping.**  `build_element` emits a real `group`;
   `mChunkAttribution` becomes a cache over a scene fact rather than the
   source of truth.  `finish_element`'s isolate shows all members instead
   of guessing the largest.  Retires a whole class of session-only state.
4. **Inventory rollup + isolate a group.**  Depends on the addressing
   decision in §3.
5. **GUI.**  Model-layer member query first (reuses the existing
   single-selection surface and epoch refresh); real nested-tree
   rendering in `OutlinerWidget` / `OutlinerView.swift` after.
6. **Later, if demanded:** true nested instancing, group-aware repeats
   (the open thread from the creative-richness journal: population can
   reuse a geometry but not an assembly, which is why the jellyfish run
   produced orphaned tentacles).

No `CURRENT_SCENE_VERSION` bump: an additive optional chunk is
backwards-compatible, and the CST loader is version-agnostic on read.

## 5. Source pointers

Flat storage `GenericManager.h:91`; transform composition
`Transformable.cpp:436-450`; TLAS build `ObjectManager.cpp:187-234`;
traversal `ObjectManager.cpp:68-101`; CSG ownership + hiding
`CSGObject.cpp:164-193`; instance-array expansion `Cst.cpp:1327-1405`
and its incremental refusal `Cst.cpp:1324-1325`; repeatable reference
params `ChunkParserRegistry.cpp:8658`; realize seam
`RayCaster.cpp:208-263`; agent ledger `AgentSession.cpp:11959-12029`;
`place_element` `AgentSession.cpp:14044-14296`; isolate resolution
`AgentSession.cpp:11429-11500`; outliner
`build/VS2022/RISE-GUI/OutlinerWidget.h`, `ViewportBridge.h:993-1017`;
undo composites `SceneEditor.h:129-152`.
