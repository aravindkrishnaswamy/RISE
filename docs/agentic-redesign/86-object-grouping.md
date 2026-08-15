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
   `standard_shader`'s repeatable `shaderop` already uses.  The reference
   graph traces them for free.

   **⚠ CORRECTED 2026-08-15 after recon — this does NOT buy "incremental
   derive keeps working normally", as this section originally claimed.**
   `ReferenceGraph::dependents` is reverse adjacency (referenced chunk →
   its referrers) and `DocEditClosure` is a reverse-BFS over it, so:
   - editing a MEMBER correctly pulls the group into the closure ✅
   - editing the GROUP yields a closure of `{group}` alone ❌ — nothing
     references a group, and there is no forward-closure primitive.

   And re-applying the group alone is not merely stale, it is WRONG:
   `Transformable::FinalizeTransformations()` never clears
   `m_transformstack` (only the member's own re-apply does, via
   `Job::AddObject`'s `ClearAllTransforms()`), so a second push composes
   `newGroup × oldGroup × (P·O·St·S)`.  The true closure of a group edit
   is `{group} ∪ its members` — the unavailable direction.

   RISE already met this exact shape in `override_object` (a later chunk
   mutating an earlier-declared object's transform by name) and resolved
   it by refusing incremental derive rather than building forward
   closure; `instance_array` likewise.  Slice 1 follows that precedent.

   **RESOLVED (shipped 2026-08-15): member edits need the fallback too,
   and the shipped guard refuses whenever a `group` appears ANYWHERE in
   the closure** — not only when the group itself was the edited chunk.
   Reason: a group's `Finalize` re-pushes its matrix onto **every**
   member it lists, not just the one whose edit pulled it in.  A member
   edit yields the closure `{member, group}`; re-applying that re-clears
   and rebuilds the edited member (via `Job::AddObject`'s
   `ClearAllTransforms`) but does NOT re-clear its untouched SIBLINGS —
   so the group's second push would compose `G × G × M` onto each
   sibling.  Document order within the closure does not save it; the
   problem is the chunks that are *absent* from the closure.

   **And the "unrelated edits stay incremental" consolation is much
   weaker than it sounds.**  `DocEditClosure` is a TRANSITIVE reverse-BFS,
   so editing any painter / material / geometry that a grouped object
   references pulls that object into the closure, then the group as the
   object's dependent, then trips the guard.  In an agent-built scene
   where most objects are grouped and a handful of painters are shared,
   that is close to "nothing stays incremental."  The guard is still the
   right slice-1 call (it is always CORRECT, and a full derive is what
   `override_object` / `instance_array` already cost), but this is the
   motivating measurement for whether slice 1's guard needs revisiting:
   **measure the incremental-fallback rate on a real agent-built grouped
   scene before adding more group surface.**  The principled fix is a
   forward-closure primitive (group → its members) so the closure can be
   completed rather than refused.
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

**DECIDED — side index, and slice 1 already ships one** (the fix round
below forced it early).  `IJob::NoteGroupMembership` /
`GetGroupMembership` / `IsGroupDeclared` (Job.cpp, derive-scoped and
cleared in `InitializeContainers` alongside `m_objectOverrideCount`)
record, per MEMBER OBJECT NAME, the owning group name(s) and the
ACCUMULATED group matrix `G_last × … × G_first`.  Slice 4's inventory
rollup and isolate-a-group should extend this index (add the reverse
group → members direction) rather than introduce a phantom
`IObjectPriv`.

### Two P1 hazards found in the slice-1 fix round (2026-08-15)

Both are permanent hazards for **anything that round-trips or overwrites
a member's transform**, because a member's `GetFinalTransformMatrix()`
is `G × M` while its authored chunk holds only `M`, and the object
carries no back-pointer to its group.  A future slice — the GUI slice
especially — must not reintroduce them.

- **The GUI transform commit SQUARED the group matrix.**
  `SceneEditor::CommitPendingCstObjectTransforms` snapshots
  `GetFinalTransformMatrix()` (= `G × M`) and writes it verbatim as the
  object's `matrix` param via `Job::ApplyCstObjectMatrixEdit`; the
  re-derive then applies that matrix AND the group's push again, giving
  `G × G × M`.  Every gizmo drag or transform-panel edit on a grouped
  member compounded another `G`, unbounded.  **Fixed** by dividing the
  index's `G` back out at commit time (`G⁻¹ × final`), so the re-derive
  reproduces `final` exactly; a SINGULAR `G` (degenerate group `scale`)
  refuses that object's commit with a diagnostic naming the group rather
  than writing a corrupt matrix — `Matrix4Ops::Inverse` returns its
  INPUT unchanged at zero determinant, so the fix VERIFIES `G × G⁻¹ = I`
  instead of trusting a determinant epsilon.  The csg apply-gate's
  `DecomposeRigid` pre-check divides `G` out too, so it gates the same
  matrix the commit will write.
- **`override_object` after a `group` silently ERASED the group
  transform.**  Its `matrix` and `quaternion` arms route through
  `SetFinalTransformMatrix` → `ReplaceFinalStack_`, which clears
  `m_transformstack` — dropping the group's entry.  The per-field arm
  (`position` / `orientation` / `scale`) only replaces component
  matrices, so the group survives and the override composes
  GROUP-LOCALLY.  The two arms had OPPOSITE semantics on a grouped
  member with no diagnostic, and scene-variant bakes emit
  `override_object`.  **Fixed** by REFUSING the two absolute arms on a
  grouped member (log + `g_cstFinalizeDiagSink`, naming the object and
  its group and pointing at the per-field arm); the per-field arm still
  works and its group-local semantics are now in the `override_object`
  descriptor.

  **H3(e) note for a future slice:** because scene-variant bakes emit
  `override_object`, a variant that bakes an ABSOLUTE `matrix` (or
  `quaternion`) override onto a grouped member now HARD-FAILS the
  whole variant's derive — the refusal above has no variant-specific
  carve-out.  This is a NEW failure mode (no existing scene can hit it:
  `group` did not exist before this slice), and the diagnostic names
  the object and its group, so it is self-explanatory when it fires.
  Flagging it here so a future variant-baking slice knows to either
  bake the per-field arm for a grouped member, or route around
  `override_object` entirely for that case.

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

1. **`group` chunk + derive-time composition — SHIPPED, hardened in the 2026-08-15 fix round.**
   Delivered: the `group` chunk parser (member references, declare-after-members validation,
   transform composition matching `standard_object`'s own T·R·S, round-trip + rejection tests,
   plus a parse-time rejection of a non-invertible group `scale`); the Job-level group-membership
   side index (`IJob::NoteGroupMembership` / `GetGroupMembership` / `IsGroupDeclared`, §3); the GUI
   transform-commit fix that divides the group matrix back out of a member's `GetFinalTransformMatrix()`
   before writing its `matrix` param, so a gizmo drag on a grouped member doesn't compound another `G`
   per edit (`SceneEditor::CommitPendingCstObjectTransforms`, `src/Library/SceneEditor/SceneEditor.cpp`);
   the `override_object` refusal of its absolute `matrix`/`quaternion` arms on a grouped member (its
   per-field arm still works, and composes GROUP-LOCALLY); and the incremental-derive guard that falls
   back to a full derive whenever a `group` appears anywhere in an edit's closure.  Deliberately NOT
   delivered in slice 1: no agent-facing authoring surface (`place_element` collapsing to one patch and
   `build_element` emitting a real `group` are slices 2 and 3), no outliner/tree UI (slice 5), and no
   inventory rollup or isolate-a-group (slice 4) — the side index exists, but nothing outside the fix
   round's own consumers reads it yet.
2. **`place_element` collapses to one patch.**  When an element maps 1:1
   to a group, emit a single patch against the group's transform instead
   of N against members.  This deletes the `matrix`/`quaternion`
   exclusions and the `approxObjects` non-commutativity caveat as
   *categories of problem*.  Highest agent value in the workstream.
3. **Persist element grouping.**  `build_element` emits a real `group`;
   `mChunkAttribution` becomes a cache over a scene fact rather than the
   source of truth.  `finish_element`'s isolate shows all members instead
   of guessing the largest.  Retires a whole class of session-only state.
4. **Inventory rollup + isolate a group.**  The addressing decision in §3 is DECIDED (side index)
   and slice 1 already ships one keyed by member name; this slice adds the reverse group → members
   direction rather than introducing a phantom `IObjectPriv`.
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
params `ChunkParserRegistry.cpp:8911`; realize seam
`RayCaster.cpp:208-263`; agent ledger `AgentSession.cpp:11959-12029`;
`place_element` `AgentSession.cpp:14044-14296`; isolate resolution
`AgentSession.cpp:11429-11500`; outliner
`build/VS2022/RISE-GUI/OutlinerWidget.h`, `ViewportBridge.h:993-1017`;
undo composites `SceneEditor.h:129-152`.
