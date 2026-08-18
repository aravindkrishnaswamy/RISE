# 87 — Recursive Scene Graph

**Status: decided 2026-08-15.  §5 steps 0, 1, 2 and 3 are IMPLEMENTED — step 3
in all four slices: 3a (`source`, single-node), 3b (subtree expansion), 3c
(`count_u`/`count_v` + per-instance exprs) and 3d (delete `instance_array`).
Step 4 is not.**  §2's "composition happens in the per-frame prepare pass" is now
literally true: step 2 re-bakes the hierarchy in
`ObjectManager::PrepareForRendering()`, so a timeline on a parent carries its
subtree.  Step 1's derive-tail and live-edit composes remain — they are what
keeps a non-rendering edit correct — and the per-frame walk is what stops the
stored parent world being a latch.
Supersedes and DELETES the single-level grouping shipped in
[86](86-object-grouping.md) (`48ab06b1`, `da0b6839`, `16b160a6`).

---

## 1. The requirement

An object may have parts, each of which may have parts, recursively,
down to a single primitive — so a dragon is authored, navigated and
animated as a dragon. Humans and agents both navigate it hierarchically
in the UI. Hierarchical transforms give hierarchical animation. One
subtree may be instanced many times.

## 2. The design

**One node type.** `standard_object` becomes the scene-graph node. It
carries exactly one of:

| mode | means |
|---|---|
| `geometry <name>` | a leaf shape — today's behaviour, unchanged |
| `source <object-name>` | an INSTANCE of that object's subtree |
| neither | a pure container (what used to need a `group` chunk) |

…plus an optional `parent <object-name>`. Exclusivity is
descriptor-validated, the same way `sweep_geometry` already validates
`profile_point` / `profile_circle` / `profile_rect`.

**Two representations, separate jobs.**

- **Authored graph** — the tree of parented objects. What the scene file
  stores, the outliner shows, animation targets, and agents build.
- **Render list** — flat, every entry carrying a fully composed world
  transform. What the TLAS is built over and what every integrator
  consumes.

**Composition happens in the per-frame prepare pass**, not at derive.
Derive only records parent links. `ObjectManager::PrepareForRendering()`
walks the tree writing `world = parent.world × local` into entries that
already exist. This is what makes animation hierarchical: any node's
timeline moves its whole subtree on the next frame, with no new
animation code.

**Instancing** expands a `source` subtree into N flat entries, each with
its own composed transform. Geometry and materials are shared by
pointer; only the small per-entry record duplicates. `Cst::ExpandInstanceArray`
is the existing precedent for synthesized entries and `name[i]` naming.

### Why flattening is the whole trick

Every existing consumer keeps working unchanged, because the flat list
IS the world:

- **Nine subsystems** walk the world-visible object list —
  `LuminaryManager` (area lights), `LightSampler` ×2 (light BVH, env
  probability), `ManifoldSolver` (SMS caustic casters), `AutoRasterizer`
  (which INTEGRATOR to use), `BDPTRasterizerBase`, `PixelBasedPelRasterizer`,
  `InteractivePelRasterizer` (extents), `SceneEditor`. Under a live
  traversal design each would go blind and need hierarchy-awareness.
  Under flattening none of them changes. **Do not "fix" them.**
- **No per-ray transform chain** — today's flat world-space TLAS,
  today's performance, today's SAH cost model.
- **Area scale and tangent-frame sign are correct by construction**,
  computed per flat entry from its composed matrix.
- **Every flat entry has exactly one world transform**, so selection,
  the object map, `scene_inventory` and the luminary list keep their
  per-entry identity. Instanced entries need PROVENANCE (source node +
  instance index) so a clicked pixel maps back into the tree.

### Precise semantics

- **World transform**: `world = parent.world × local`. Local is the
  node's own authored `position`/`orientation`/`scale` (or `matrix`).
- **Composition MUST go through `FinalizeTransformations( parentWorld )`,
  never through the transform stack.** The stack never self-clears; the
  entire 86 bug class (GUI squaring `G×G×M`, the `G⁻¹` commit division,
  the `override_object` stack-clearing conflict) came from pushing
  composed matrices onto it.
- **Cycles**: declare-before-use prevents them at parse. Reparenting at
  runtime needs its own guard.
- **Child order** for display comes from declaration order.
- **File ordering**: because `parent` is on the child, a dragon is
  declared before its parts — a 50-part model reads top-down.
- **Per-sample motion blur** reads the frame's base-time bake. The TLAS
  already has exactly this limitation (`docs/ARCHITECTURE.md`), so this
  makes an existing inconsistency uniform rather than adding one.

## 3. What this DELETES (step 0)

Remove wholesale — do not migrate. Nothing has shipped to customers, and
a half-migrated `member`/`parent` world is worse than either.

- the `group` chunk, its parser and descriptor
- `ChunkCategory::Group`; `SceneEditController::Category::Group` and the
  `kNumCategories` 13→12 revert
- the four `IJob` group virtuals (`EnumerateGroupNames`,
  `GetGroupMemberCount`, `GetGroupMemberName`, `GetGroupOwnTransform`)
  and their `tests/IJobVtableManifest.txt` lines
- `Job::m_groupsByName`, `m_groupMembership`, `NoteGroupMembership`,
  `GetGroupMembership`, `IsGroupDeclared`
- `SceneEditController`'s group snapshot, its stamp gate,
  `GroupMemberNames`, and the `mGroupMemberTruncationWarned` set
- both shells' Group category, group rows, member caches and
  `groupMembers` accessors (Qt `OutlinerWidget`/`ViewportBridge`/
  `ViewportProperties`; Swift `OutlinerView`/`PropertiesPanel`/
  `RISEViewportBridge`)
- the parse-time group-matrix invertibility gate
- `SceneEditor::GroupLocalTransformMatrix_` and the `G⁻¹` commit division
  (both call sites, incl. the csg apply-gate pre-check)
- `override_object`'s grouped-member refusal
- `Cst.cpp`'s group incremental-derive refusal
- `tests/GroupChunkTest.cpp`, `tests/GroupPanelCategoryTest.cpp`
- `scenes/Tests/Geometry/group_basic.RISEscene` and
  `group_gallery.RISEscene` — as implemented, both were DELETED and one new
  scene, `object_parenting.RISEscene`, was written in their place (the two
  group scenes demonstrated a single-level feature that no longer exists;
  rewriting each as a parent-link scene would have produced two near-duplicates
  of the same articulated-hierarchy demo)

Also folded away by the new design: `instance_array` becomes the `source`
+ count/expression mode of `standard_object` (**the parenthetical this line
used to carry — "killing its document-wide incremental-derive refusal" — did
NOT survive contact with the code; see §4 and §5 step 3's 3d block: the
document-wide refusal is restored, keyed on `source` instead**), and
`mChunkAttribution` — the agent's
session-only element ledger — is replaced by real parent links.

## 4. Engine facts worth not re-deriving

- **`ObjectManager::PrepareForRendering()` is the per-frame host.**
  `RayCaster::AttachScene` runs ONCE per render, not per frame.
- The per-frame loop is `Animator::EvaluateAtTime` →
  `InvalidateSpatialStructure` → `PrepareForRendering` → render,
  single-threaded, before the parallel tile dispatch.
- `Transformable` already implements `IKeyframable`
  (`SetIntermediateValue` → position/orientation/stretch,
  `RegenerateData` → `FinalizeTransformations`), so a container node is
  keyframable for free.
- The TLAS is already destroyed and fully rebuilt every animated frame.
- `Transformable::FinalizeTransformations` computes `P·O·St·S` then folds
  the transform stack; **it never clears the stack**.
- `Object::FinalizeTransformations` also caches `m_mxInvTranspose`,
  `m_tangentFrameSign`, and `m_worldAreaScale` (`|det|^(2/3)`) — all must
  be recomputed for every node whose world transform changes.
- `Object::GetArea()` returns `objArea * m_worldAreaScale`; under
  hierarchy that determinant must reflect the COMPOSED matrix, or
  emitter/SSS PDFs are wrong (this exact bug class was fixed 2026-08-13).
- `DeriveToJob` PASS-2 applies chunks in document order and resolves
  references by immediate manager lookup.
- `CSGObject` is the container precedent: one-directional `addref`,
  `SetWorldVisible(false)` on operands, recursive `Realize()` cascade.
  CSG stays a render-time entity (its boolean needs both operands' hit
  intervals) — it cannot be flattened away. Its operands SHOULD become
  visible as children in the UI tree.

### The two transform-commit routes are asymmetric, and must stay that way

Recorded because three review rounds in a row got this wrong in one
direction or the other, and the last one "unified" them on a symmetry
argument that cost author content for zero behavioural gain.

An object's pose can be authored across several chunks: its base chunk, then
any number of same-named `override_object` layers. A gizmo commit writes to
the LAST layer and must clear whatever earlier layers would otherwise still
contribute.

- **The matrix route strips the WRITE TARGET only.** A written `matrix` (or
  `quaternion`) goes through `SetFinalTransformMatrix` → `ReplaceFinalStack_`,
  which clears the transform stack *and* the position / orientation / scale /
  stretch components outright. Every earlier layer is therefore subsumed by
  construction, and stripping them buys nothing.
- **The components route strips EVERY layer.** A written `position` +
  `orientation` takes the per-field branch, whose `Set*` calls are independent
  and never clear an earlier layer's `scale`. A sign-flip `scale` two layers
  out survives and double-applies against the `orientation` being written —
  measured at 5.25 where the user's gesture said 4.75, silently.

Same mechanism, opposite conclusions. **Do not re-unify them.** The cost of
getting it wrong in the permissive direction is a silent wrong pose; in the
aggressive direction it is deleting params from chunks the author wrote, on
an edit that changed nothing — `override_object` declares only transform
params, so stripping every layer empties every non-owner layer, always.

A related rule this arc settled: a commit may leave an `override_object`
holding only its `name`, and may orphan a comment whose param it removed. It
must NOT delete the chunk or the comment. Silently removing author-written
prose during a gizmo drag is worse than leaving something the author can see
and re-place.

### Step 3 met three code facts the design did not anticipate

Recorded here before the work lands, so they are decisions rather than
discoveries.

- **§3's "killing its document-wide incremental-derive refusal" is NOT
  achievable by folding `instance_array` into `source`.** The refusal exists
  because the generator's input edges are untraced. Making `source` a
  `ValueKind::Reference` does get it traced — but only to the ROOT of the source
  subtree. `DocEditClosure` is a reverse-BFS over `dependents`, and the edge
  direction inside a subtree is child → parent (`parent` is a Reference on the
  child), so editing a DESCENDANT's geometry reaches that descendant's chunk and
  stops: nothing references the descendant, so the instancing chunk is never
  reached, and the incremental apply would re-point the source's descendant
  while N stale clones keep the old binding — exactly the divergence the refusal
  prevents. Closing it needs a transitive structural edge (source → every
  `parent`-descendant), which breaks `MaintainedReferenceGraph`'s incremental
  premise: `SetParamValue` re-runs `ComputeChunkRefs` for the edited chunk only,
  while a `parent` edit anywhere changes subtree membership for every ancestor
  `source`. **The refusal survives step 3**, and retiring it is its own
  arc — it also needs a typed "drop this chunk's N synthesized objects"
  primitive, which the provenance map is the inverse index for.
  **THIS PARAGRAPH IS CORRECT AS WRITTEN, AND 3d WRONGLY STAMPED IT RESOLVED.**
  3d replaced the text above with a "CORRECTED BY 3d" note claiming the
  document-wide form was deleted because "3b closed the subtree-MEMBER hop
  through the manager resolution inside the armed sink."  That claim is FALSE
  for two independent reasons, either one sufficient: (1) the armed resolution
  sink is **never armed in production** — `DeriveToJob`'s recording is opt-in via
  `outRecorded` and every production call site passes `nullptr` (Job.cpp,
  AgentSession.cpp; the only non-null caller in tree is
  `tests/CstRecordDeriveTest.cpp`), so nothing is recorded when
  `ClonePlanBuilder` resolves a subtree member; and (2) even armed, the recorded
  graph is **not the closure consumer** — `Cst.h` says so verbatim, closure
  consumers still read `BuildReferenceGraph` until the consumer-switch lands, and
  no descriptor `Reference` points at a subtree member.  Measured on the probe
  `A` / `B parent A` / `I source A`: `DocEditClosure(d, B)` has size 1 and does
  NOT contain `I`; `Job::ApplyCstParamEdit("B", …, "position", "0 9 0")` returned
  rc=1 with zero diagnostics and left `I.B` at the pre-edit pose while a full
  re-derive moved it.  The DEFECT itself predates 3d (the deleted refusal keyed on
  `instanceArrayCount`, which is 0 in a `source`-only document, so it never
  covered this case); what 3d did was record the hop as closed in four places,
  which is what stops the next reviewer looking.  **The refusal is RESTORED**, now
  keyed on `Document::sourceInstanceCount` — the count of `standard_object`
  chunks carrying a live `source` — so a `source` document always takes the full
  re-derive.  Retiring it needs BOTH the closure consumer-switch AND the
  transitive `source` → `parent`-descendant edge described above.
- **Animation does not compose with instancing.** §1 pairs them ("hierarchical
  transforms give hierarchical animation. One subtree may be instanced many
  times"), but an instance is a COPY, not a live view, and
  `Job::AddKeyframeToAnimation` caches a raw `IKeyframable*` resolved from one
  name. A timeline on the source moves the source only; its instances stand
  still. Targeting each instance means hand-writing N timelines. This is a real
  hole in the §1 pairing, not an implementation gap.
- **Subtree instancing is a step-function regression against step 2's cost
  model.**  **CONFIRMED BY MEASUREMENT in 3b — ~26x, and the numbers, the
  attribution and a proposed fix are in §5 step 3's 3b block.** Today's `instance_array` produces FLAT objects — zero links — so a
  10,000-instance grid costs one predicate per frame. Under `source`, 10,000
  instances of a 5-node subtree is 50,000 links re-baked every frame through a
  string-keyed walk. This is what decides the open semantics question: the
  instancing node IS the clone of the source root, so `source <leaf>` collapses
  to exactly one object under the chunk's own name and the common array case
  costs ZERO links. It also keeps `name[i,j]` byte-compatible with the
  object-map tests that already pin it, and gives every instance a real
  CST-backed name the properties panel and the gizmo can edit — which today's
  `instance_array` entries do not have.

### A creation rule is not a history rule

The container rule — "a node with no `geometry` takes no surface binding" — is
enforced on every FORWARD path: the derive, the `IJob` setters, the editor's
forward mutation, the agent param commit, the agent chunk insert. It is
**deliberately absent from Undo and Redo**, and that asymmetry is the design,
not an oversight.  Redo shares `ApplyForwardMutation` with the creation path,
so that function takes an `isReplay` flag which suppresses exactly these gates
and nothing else; Redo, the composite roll-forward and the composite-undo
rollback all pass it.

Two review rounds added a gate to `SceneEditor::ApplyRevertMutation`. The second
was wrong in a way that cost the user their scene:

1. `set_param(O, geometry, none)` — permitted; `geometry` is not a binding. `O`
   becomes a container, the derive warns and drops its material, the Document
   still carries it.
2. `set_param(O, material, none)` — permitted, deliberately: a container that
   acquired a stale binding has to stay tidyable.
3. Cmd-Z. The gated revert refused. `PopForUndo` had already moved the record,
   so it was pushed back — and every later Cmd-Z re-popped and re-failed. Step 1
   and everything older became permanently unreachable.

A refused revert wedges the undo stack with no escape (unlike Redo, which any
new edit clears). And what the gate refuses is not the creation of a novel
state: it is the RESTORATION of a document state that existed moments earlier,
one the derive already tolerates — warn, drop the binding, carry on — because
that is the contract for every scene file ever authored.

The alternative considered and rejected was an "honest partial": skip the write
but let the undo consume its record. That avoids the wedge but makes undo
LOSSY — undoing further to restore the `geometry` would then leave the object a
leaf with no material, when the authored scene had one. Undo must be lossless.

Regression guards: `SceneGraphParentTest` cases U, V step 5, and W (W drives
the exact three-step sequence above and asserts two Cmd-Zs return the scene to
what was authored, live material included).

### Known, accepted, NOT fixed by step 1

- ~~**An interactive edit rebases an ANIMATED parent's subtree.**~~ **CLOSED by
  step 2.**  The global compose an interactive edit runs read the parent's world
  as it stood at the parked time and stored it into every child, so scrubbing
  back left the subtree displaced for the rest of the session.  Re-baking every
  frame recomputes that stored value instead of inheriting it, so it is no
  longer a latch.  Regression guard: `SceneGraphParentTest` case Y drives the
  exact sequence — park at an animated `t=1`, apply a live gizmo-shaped
  transform to an unrelated object, scrub back — and it is red without the
  per-frame walk.  (A panel/param edit does NOT reproduce it: that routes
  through the CST and re-derives at the animator's default time, which resets
  the parent and hides the bug.  The test uses a live transform op for that
  reason.)

- **The light sampler's cached selection weights are one attach stale for any
  ANIMATED emitter, hierarchy or not.** `LightSampler::Prepare()` caches each
  luminary's `exitance` and representative position once, at
  `RayCaster::AttachScene()` — which runs BEFORE the per-frame animation loop
  starts. Emitter surface sampling and `GetArea()` are queried live, so the
  estimator stays unbiased; what freezes is the alias-table weight and the RIS
  representative position, i.e. it is a variance issue, not a bias one. This
  predates 87 and applies equally to a flat animated emitter — but step 2 makes
  it reachable through a container (animate the fixture, the lamp rides along),
  and §2 names `LightSampler` as one of the nine consumers of the composed
  matrix, so it belongs on this list. Note a fix would still be one frame late
  as long as `AttachScene` precedes `PrepareForRendering`.

  **Measured, so the bound is known rather than assumed** (2026-08-16, three
  96×72 PT renders): an emissive sphere carried by an animated container, with
  the sampler frozen at `t=0`, versus a fresh sampler at the same pose —
  means 35.665 vs 35.708 (0.12 %), luminance centroids 0.03 px apart, and an
  RMS to the static reference *lower* than the RMS between two independently
  noisy renders of identical geometry. `LuminaryManager` holds only
  `const IObject*` and reads the live composed transform, so sampling is
  **unbiased**; what goes stale is `LightSampler::Prepare`'s cached selection
  weight, its RIS representative position and the cached scene centre/radius —
  variance, not correctness.

  The same shape appears once more, on the ordering: `AttachScene` runs BEFORE
  `PrepareForRendering` in every rasterizer, so the first render after a
  re-parent builds the sampler from the pre-re-bake pose and
  `builtLightGeneration = liveGen` then blocks a later rebuild. Also
  variance-only, by the same measurement — but it is exactly the shape that was
  a real bug on the TLAS side (a signal consumed before the thing that needed
  it ran), and it becomes a correctness bug the day anything position-dependent
  in `LightSampler::Prepare` stops being a pure selection heuristic.
- **The interactive depth view's extent cache is one pass stale on a parented,
  animated scene.** `InteractivePelRasterizer` reads
  `GetSpatialStructureGeneration()` inside `AttachScene`, which the helper runs
  BEFORE `PrepareForRendering` — so the re-bake bumps the generation after the
  cache key was read, and frame N normalizes depth against frame N−1's
  transforms. Self-corrects on the next refinement pass; visible only while
  scrubbing.
- **A rank-deficient container silently widens an existing hazard to its
  whole subtree.** `Transformable::FinalizeTransformations` ends with
  `m_mxInvFinalTrans = Matrix4Ops::Inverse( m_mxFinalTrans )`, and
  `Matrix4Ops::Inverse` returns its INPUT UNCHANGED when `det == 0.0`
  (`MatricesOps.h`). Author `scale 0 0 0` on a leaf and that leaf's
  `IntersectRay` transforms world rays with a singular matrix instead of
  an inverse — projecting the ray onto a plane or line — while its
  bounding box transforms forward into a degenerate-but-finite box the
  TLAS still admits, so it yields arbitrary hits rather than none. That
  is PRE-EXISTING and unchanged. What hierarchy adds is reach: the same
  authoring mistake on a CONTAINER (itself invisible, so it looks
  harmless) makes `det == 0` for every descendant at once.
  `m_worldAreaScale` correctly goes to zero, so light sampling stays
  safe; the exposure is intersection only.
  There IS an invertibility guard two dozen lines above, but it protects
  `m_mxParentWorldInv` — the change-of-frame used to conjugate a
  world-space gizmo op — and has no counterpart on the final inverse.
  Giving it one would change render behaviour for every existing scene
  that authors a degenerate `scale`, which is out of step 1's scope and
  wants its own measured change.

## 5. Sequencing

0. **Delete §3.** Its own commit, first.
1. **`parent` + optional `geometry`.** Derive records links; the
   structural flatten composes world transforms. Cycle guards at parse
   and at reparent. Delivers dragon → parts → subparts → primitive.
2. **Per-frame re-bake** in `PrepareForRendering`. Delivers hierarchical
   animation. Do NOT re-flatten structurally per frame (that churns the
   name map) — re-walk into already-allocated entries.
3. **Instancing via `source`**, folding in `instance_array`.
   **Provenance/naming — DECIDED 2026-08-16:**
   - The instancing node IS the clone of the source root, so `source <leaf>`
     collapses to exactly one object under the chunk's own name. See the
     cost argument above for why this, and not "compose the source's own
     local transform in".
   - A multi-node subtree instance names its descendants `I.X` — one level
     of qualification, because descendant names are already globally unique
     in the manager, so nesting composes without a path. `.` not `/`:
     `ChunkNamePath` keys the document index as `role + "/" + name`.
   - With counts: `I[i,j]` and `I[i,j].X`, keeping today's `instance_array`
     legend entries byte-compatible.
   - Provenance is a MAP (`entry -> (instancing chunk, source node)`) on
     ObjectManager, never a string-split on `.` — an author may legitimately
     write `name my.object`.
   Slices: 3a `source` single-node + refusals + provenance; 3b subtree
   expansion; 3c `count_u`/`count_v` + per-instance exprs; 3d delete
   `instance_array`.  ALL FOUR ARE IMPLEMENTED.

   **3d (2026-08-17) — `instance_array` DELETED, IMPLEMENTED.**  `Cst::Expand
   InstanceArray` and its trailing expansion loop, the `instance_array` role skip
   in the PASS-1 collect walk, `Document::instanceArrayCount` and all five of its
   maintenance sites, the `generatorChildrenOf` index and BOTH subtree-walk
   refusals that read it, the `instanceArrayCount`-keyed incremental-derive
   refusal, and `tests/CstInstanceArrayTest.cpp`.  The corpus had ZERO uses
   (verified: 0 of 3914 `.RISEscene` files under the repo), and
   `CstDeriveGoldenTest` reports 383 MATCH / 0 DRIFT, so no golden output moved.
   Four things met the code:
   - **THERE WAS NO CHUNK PARSER TO DELETE.**  The 3d brief called for removing
     the `instance_array` `IAsciiChunkParser` subclass and its
     `CreateAllChunkParsers()` registration.  Neither exists: the generator was
     always CST-only (recorded in
     [61](61-v6v7-parser-cutover-execution-plan.md) §"`instance_array` is
     CST-only"), which is exactly why PASS-1 skipped its chunks and why its
     inputs were untraced in the first place.  Deleting the role skip is what
     retires the keyword: an `instance_array` chunk now reaches the registry
     lookup and is refused as an unknown chunk type.
   - **THE LEDGER IS DOCUMENT-ORDER AGAIN.**  `grep -n 'entryBudget'
     src/Library/Cst/Cst.cpp` now shows one declaration, `ExpandSourceInstance`'s
     parameter, its cap read (two lines) and its two `--entryBudget` sites — one
     writer.  That is what turns 3c review round 1's `source`-path ledger
     assertions from partial into total, and it also deletes 3c review round 2's
     expansion-order-vs-document-order discrepancy: the only remaining expansion
     runs at its own PASS-2 position, so an earlier chunk's spend is on the ledger
     when a later refusal quotes it.  Both decrements re-red-proved on the
     post-deletion tree (deleting either moves the quoted remainder and fails two
     `count-cap-ledger` assertions).
   - **THE ERANGE OVERFLOW TERM IS NOW DEAD, AND ONLY THE UNDERFLOW HALF IS
     LIVE.**  3c recorded that the shared count validator's `errno == ERANGE`
     term stayed reachable both ways "from `instance_array`, which PASS-1 skips".
     With the generator gone, the counts are only ever read from a
     descriptor-declared numeric slot, whose PASS-1 string-layer check refuses
     `1e999` first.  The term is kept — strtod reports both directions through one
     errno, and UNDERFLOW (`1e-999`) is caught by nothing else — but its overflow
     half is unreachable and the fixture comment says so.
   - **~~TWO REFUSALS SURVIVE, AND ONE OF THEM IS NOT THE ONE §4 PREDICTED.~~
     WITHDRAWN — THIS WAS THE 3d P1.**  3d wrote here that the document-wide form
     "is not needed", because "`source` is a descriptor-declared Reference, so
     editing the source reaches the instancing chunk, and 3b closed the
     subtree-MEMBER hop by resolving each member through the manager inside the
     derive's armed sink."  The first clause is true and the second is FALSE: the
     armed sink is opt-in via `DeriveToJob`'s `outRecorded`, which every
     production caller passes `nullptr` for, and the recorded graph is not what
     closure consumers read anyway.  A subtree-MEMBER edit therefore took the
     incremental path and left every clone stale, silently and live-only.  The
     defect is OLDER than 3d — the deleted refusal keyed on `instanceArrayCount`,
     which is 0 in a `source`-only document — but 3d recorded it as closed in four
     places.  **THREE refusals now survive**: 3a/3b's PER-CHUNK pair (a closure
     chunk carrying a real `source`; one still holding a live PROVENANCE row),
     both still correct and still needed, plus a RESTORED DOCUMENT-WIDE form keyed
     on `Document::sourceInstanceCount`.  §4's original conclusion — "the refusal
     survives step 3" — was right, and the cost it named (a `source` scene always
     full-derives) is the cost now paid.
   Migrated, not deleted: the `grid[i,j]` legend fixtures in
   `AgentObjectMapTest` and `AgentViewModeRenderTest` are now counted `source`
   chunks.  The names are byte-identical, which is what 3c's naming decision was
   for — but the SOURCE object is a fifth world-visible entry (a `source` copies;
   it does not hide the original), where the generator's `template` was a
   geometry and produced only four.  The count assertion says 5 and names `src`
   explicitly rather than leaving the difference as slack.
   NOT MIGRATED, stated plainly: the one fixture that reached
   `standard_object`'s PARSER-side `source` gates (it fed a `source` through the
   generator's param pass-through, arriving at `Finalize` alongside a `geometry`
   from `template`).  Both gates are now unreachable from any document — PASS-2
   routes every `source`-carrying chunk to the expansion — so they are backstops
   with no test, which is what their own comments already claim they are.  The
   EXPANSION's copy of the exclusivity rule, the reachable one, keeps its
   fixture.

   **3d CHECKLIST (accumulated by earlier slices' reviews; done in 3d):**
   - After removing `ExpandInstanceArray`, verify there is **exactly one writer**
     to the shared entry ledger: `grep -n 'entryBudget' src/Library/Cst/Cst.cpp`
     must show only the declaration in `DeriveToJob`, `ExpandSourceInstance`'s
     parameter, its cap read and its two `--entryBudget` sites.  Until then the
     `source`-path ledger assertions in `CstSourceInstanceTest` are partial, and
     the generator's own read/decrement pair is uncovered in both directions
     (3c review round 2).
   - The generator's trailing expansion loop is also what makes the ledger
     EXPANSION-order rather than document-order; deleting it removes that
     discrepancy, so the clause on the ledger fixture's comment can go with it.
   - The `v`-discriminating fixtures in `AgentObjectMapTest` /
     `AgentViewModeRenderTest` are on the `instance_array` copy and die with it;
     the `source`-path replacements landed in 3c review round 1, so check that
     the migration does not remove coverage rather than move it.

   **3b (2026-08-17) — subtree expansion, IMPLEMENTED.**  The 3a
   has-children refusal is replaced by a real expansion.  Mechanics: build the
   clone by re-`Finalize`ing each subtree node's OWN chunk through the registry
   parser for that node's role, with `name` and `parent` remapped.  That choice
   is what makes a `rect_light` in the subtree come out right for free —
   remapping the one `name` renames all four of the entities its Finalize
   synthesizes — where an object-level clone walk would collide on all three
   helpers.  Six things met the code and are recorded because they are
   decisions, not details:
   - **The naming recursion advances only across a `source` boundary.**  A
     document child of a cloned node keeps the current qualification level
     (`I.D`, not `I.C.D`); crossing into a nested instance's source advances it
     by one, which is exactly where the copied ENTRY name is itself qualified.
     So `I2.I1` and `I2.I1.A2` fall out of the same rule that gives `I.C`, with
     no path anywhere.
   - **The subtree walk is over the DOCUMENT, not over live `parentByName`.**
     The document form gives every member a chunk to re-Finalize; a live-entry
     walk would reach synthesized entries that have no chunk of their own and
     would have to recover one from provenance.
     **The two do NOT describe the same tree, and an earlier draft of this line
     claiming they do was wrong — the bug it hid is recorded here because the
     shape recurs.**  A document node may write `parent I1.B`, naming an entry
     that some earlier expansion SYNTHESIZED.  The live tree then holds an
     `I1.B -> X` link, while the document index is keyed by the parent NAMES as
     written and so has a key `I1.B` that no chunk declares — and no key at all
     under the member's own chunk name `B`.  A walk that looked a member's
     children up by its bare chunk name therefore dropped that entire branch
     with no diagnostic, and the same hole let an `instance_array` parented onto
     a synthesized entry EVADE the generator refusal, deriving clean with the
     generator's objects silently missing from the copy.  The walk now asks for
     every key an entry can answer to (`ClonePlanBuilder::ChildKeysOf`): its
     fully-qualified entry name, its bare chunk name, and every intermediate,
     each paired with how much of the qualification a child found there
     inherits.  It is a UNION and not a first-match fallback — both keys can be
     non-empty at once (`parent B` and `parent I1.B` are both real children of
     `I1.B`), so trying one and falling back to the other drops whichever loses.
   - **3a's has-children scan covered only `standard_object` / `csg_object`,
     which was a hole.**  A source whose only child was a `rect_light` or
     `shape_light` passed the refusal and instanced ROOT-ONLY, silently
     dropping the lamp.  3b indexes all four object-producing roles, and the
     document collision scan now runs over that same ENTRY-name keyspace — a
     `rect_light` named `I.C` claims that entry name just as a
     `standard_object` would.
   - **DECLARE-BEFORE-USE applies to the whole subtree.**  A member declared
     BELOW the instancing chunk does not exist when the copy is made; copying
     "the part that happens to precede me" is the silent partial copy 3a
     refused, so it is refused with its own message.
   - **An `override_object` layer on a subtree MEMBER is refused; one on the
     source ROOT is not.**  An override is applied to the live object by name
     after its base chunk, so it never reaches a clone built from that chunk.
     Overlaying its params onto the merge is NOT equivalent — a base `matrix`
     plus an override `position` COMPOSES on the live object but would be
     swallowed by `standard_object`'s matrix-wins precedence in one merged
     param list.  The root is exempt by construction: `override_object`
     declares only transform params, and the collapse semantics drop the
     source's transform entirely.
   - **The `source S parent S` refusal survives, and a second guard joins it.**
     Round 1's "every child of S instances S" test no longer separates anything
     (a source with a genuine other child is now expanded, not refused), so the
     pre-walk check is simply "is the instancing chunk itself among the
     source's children".  A recursion through a TRANSITIVE descendant
     (`C source M parent M2` where `M2 parent M`) is invisible to that check and
     is caught by the walk's own path-revisit guard, which is why both exist and
     why their messages differ.

   **⚠ THE PARAGRAPH BELOW WAS WRONG.  Struck 2026-08-17 — 3b did NOT close
   the subtree-MEMBER hop.**  It is kept, struck, because it is the ORIGIN of the
   false claim that 3d later cited to delete the document-wide incremental-derive
   refusal.  3d's four repetitions were reverted at the time; this fifth, inherited
   copy survived that sweep untouched and was found by the review round after.
   The refutation is in §4: the resolution sink is **never armed in production**
   (every production `DeriveToJob` call site passes `outRecorded = nullptr`; the
   only non-null caller in the tree is `tests/CstRecordDeriveTest.cpp`), and
   production closure is `DocEditClosure` → the static `BuildReferenceGraph`,
   which would not consult that sink even if it were armed.  Editing a subtree
   member therefore kept taking the incremental path, leaving every clone stale,
   until `fab30721` restored a document-wide refusal keyed on
   `Document::sourceInstanceCount`.

   > ~~**The incremental-derive trace was closed for subtree MEMBERS, one hop
   > further than 3a.**  The reference graph's `parent` edge runs child → parent,
   > so editing a member reaches that member's chunk and stops — the instancing
   > chunk never enters the closure, and the incremental apply would re-point the
   > member while N stale clones kept the old binding.  The plan walk therefore
   > resolves each member through `objMgr->GetItem` inside the derive's armed
   > resolution sink, which records the member's chunk as a producer the expansion
   > consumes.  Editing a member now reaches `I`, and `I` carries `source`, which
   > the existing per-chunk gate turns into a full derive.~~

   The surviving, TRUE half of the original paragraph: chunk INSERT and
   REMOVE never took the incremental path at all (`Job::ApplyCstInsertChunk` /
   `ApplyCstRemoveChunk` both route through `RederiveCstDocumentFull_`), so the
   structural half needs nothing.

   **THE PER-FRAME COST, MEASURED (2026-08-17, `renderanimation` at 8x8,
   200 frames, load subtracted, 3 reps; same harness as step 2's table):**

   | scene | objects | links | ms/pass |
   |---|---|---|---|
   | flat, 16 000 objects | 16 000 | 0 | 0.19 – 0.23 |
   | one single-node `source` instance in that scene | 16 000 | 0 | 0.18 – 0.20 |
   | 100 instances of a 5-node subtree | 505 | 404 | 0.45 – 0.46 |
   | 1000 instances of a 5-node subtree | 5 005 | 4 004 | 5.53 – 5.55 |
   | 2000 instances of a 5-node subtree | 10 005 | 8 004 | 12.0 – 12.2 |
   | **the same 5 005 objects / 4 004 links authored BY HAND, no `source`** | 5 005 | 4 004 | **5.50 – 5.52** |

   Read in order, those say three things.  (1) **The collapse case really is
   free** — a `source` naming a leaf costs zero links and measures at or below
   the flat baseline, which is the property the semantics were chosen for.
   (2) **It IS a step-function against `instance_array`'s cost model, and by
   ~26x**: 5000 objects flat cost ~0.2 ms/pass, the same 5000 reached through
   subtree instancing cost ~5.5 ms/pass, and the curve is linear in LINKS
   (~1.4 µs per link per pass; 2x links → 2.2x time).  (3) **3b adds nothing of
   its own** — the hand-authored hierarchy with the identical link count
   measures the same to within 1 %.  The cost belongs to step 2's
   `RebakeHierarchy`, which per pass rebuilds three string-keyed containers,
   sorts each child list, and walks with a string-keyed visited set and a
   String-copying stack.  3b did not create it; it makes thousands of links easy
   to author, which is how it becomes visible.

   **Proposed fix, NOT implemented (it is step-2 work, and the brief for 3b was
   to measure rather than optimise speculatively):** memoize the walk.
   `parentByName` changes only through `SetObjectParent` / `RemoveItem` /
   `Shutdown`, every one of which already calls the STRUCTURAL
   `ComposeWorldTransforms` — so `RebakeHierarchy` can cache a flat,
   pointer-keyed topological plan (`vector<pair<IObjectPriv*, IObjectPriv*>>`)
   behind a link-generation counter and, on the common pass, do one pointer walk
   with one `FinalizeTransformations` per link and no map or String traffic at
   all.  That should take the 1.4 µs/link to well under 0.2 µs.  Measure before
   and after with the table above.

   **Also NOT done in 3b, stated plainly:** the document-wide synthesized-entry
   cap (`kMaxSynthesizedEntries`, 10 000 000, shared with `instance_array` and
   counting ENTRIES rather than per-generator instances) has no cheap regression
   test — `instance_array` clamps each count to 1e6, so crossing the budget
   means materialising ten million objects.  Its arithmetic was verified by
   temporarily lowering the constant.

   **3c (2026-08-17) — `count_u` / `count_v` + per-instance exprs, IMPLEMENTED.**
   A chunk carrying `source` may also carry `count_u U [count_v V]`, repeating the
   WHOLE instance -- root plus subtree -- `U x V` times as `I[i,j]` and
   `I[i,j].X`, with the instancing chunk's own parameters evaluated per repetition
   over `i`/`j` and `u`/`v`.  `EvalInstanceValue`, the count validator and the
   caps are `instance_array`'s own, CALLED rather than re-derived: the validator
   was lifted out of `ExpandInstanceArray`'s `evalCount` lambda verbatim into
   `EvalInstanceCount`, with only the diagnostic prefix parameterised, and one
   mutation to it now reddens BOTH suites (verified: deleting the `(long long)`
   round-trip fails 3 assertions in `CstSourceInstanceTest` and 3 in
   `CstInstanceArrayTest`).  Seven things met the code and are decisions:

   - **PRESENCE of a count selects the repeated naming, never its VALUE.
     `count_u 1` derives `I[0,0]`, not `I`.**  A count may be an `expr(...)` over
     a `let`, so a value-keyed rule would silently re-name every entry -- dangling
     every `parent I[0,0]` in the file -- when a constant went from 2 to 1.  It
     also keeps the entry names byte-compatible with the `instance_array`
     generator 3d retires, which names its one-instance case `g[0,0]` for exactly
     the same reason.  The count-less form is untouched: still plain `I` / `I.X`.
   - **PASS-1 VALIDATES AN INSTANCING CHUNK AT INSTANCE ZERO, and without that
     the feature does not parse at all.**  `position expr(i*2) 0 0` is a
     three-component value with one expr in it, which `TryEvalExprValue` (a
     WHOLE-value rule) passes through verbatim -- and that is not a finite numeric
     triple, so PASS-1's descriptor check refused the scene before any expansion
     ran.  (`instance_array` never met this because PASS-1 SKIPS its chunks
     entirely.)  `ResolveChunkParams` now takes an optional per-instance context
     and PASS-2's trigger (`source` read off the CST token) is hoisted above it.
     The bag built there is never applied for such a chunk, but the arity and kind
     checks are real: `position expr(i) bogus 0` is still refused.
   - **THE SAME CONTEXT IS PASSED WHEN THERE ARE NO COUNTS.**  A single instance
     IS instance zero.  Evaluating the uncounted form by the whole-value rule
     instead would have PASS-1 admit a chunk that PASS-2 then refuses at the
     descriptor -- the live/CST divergence class this arc keeps closing.  For a
     value with no `expr(` in it the two paths are byte-identical, so every 3a/3b
     scene derives exactly as before (`CstDeriveGoldenTest` 383 MATCH / 0 DRIFT).
   - **PER-INSTANCE VARIATION IS THE INSTANCING CHUNK'S OWN PARAMS ONLY, and that
     is `instance_array`'s scope too -- not a reduction of it.**  A subtree MEMBER
     is an ordinary `standard_object`, so PASS-1 refuses a per-component expr on
     one; `instance_array` had no descendants at all, so there is no parity to
     lose.  Say that in the docs rather than implying per-descendant variation.
   - **THE CAP COUNTS `count_u * count_v * subtreeSize`, and 3c is the first
     slice in which its ARITHMETIC can be TESTED.**  3b could only cross the
     document budget by materialising ten million objects; a count is refused from
     arithmetic alone, before one entry is built.  The regression guard asserts the
     NUMBER in the refusal: 1e6 x 10 repetitions of a 3-node subtree is 1e7
     instances -- exactly the budget, so an instance-counting cap would admit it --
     and 3e7 entries, which is what reaches the TLAS.  `instance_array`'s per-count
     1e6 clamp is kept (it arrives with the shared validator); no separate product
     cap is needed, because `perInstance >= 1` makes the entry total subsume it.
   - **A COUNTED CHUNK CANNOT BE A `source`, NOR A MEMBER OF A COPIED SUBTREE.**
     It is N entries, not a node; copying "the first one" is the silent partial
     copy every other refusal in the walk exists to prevent.  The source-side check
     sits BEFORE the manager probe deliberately: a counted chunk produces `A[0,0]`
     and no `A`, so the probe would have refused it with "declared earlier but did
     not produce an object (its own chunk failed)" -- a cause that did not happen.
     The member-side check is reachable and pinned; the one inside `SourceSubtree`
     (a counted chunk deeper in a `source` chain) is defensive by the same
     PASS-2-stops-at-the-first-refusal argument the walk's other dead tails carry.
   - **The clone plan is now RELATIVE to the instance root** (`srcEntryName` +
     `parentRel`), so it is built ONCE and composed into `I[i,j].X` per
     repetition.  A 100x100 grid walks the document once, not 10 000 times.

   **THE PER-FRAME COST, MEASURED (2026-08-17, `renderanimation` at 8x8, 1000
   frames, load excluded via the CLI's own "Total Rasterization Time", 3 reps;
   same harness as step 2's and 3b's tables):**

   | scene | objects | links | ms/pass |
   |---|---|---|---|
   | flat 10 000, short names | 10 000 | 0 | 0.174 – 0.178 |
   | flat 10 000, authored as `I[i,j]` at the same poses | 10 000 | 0 | 0.182 – 0.184 |
   | **`source <leaf> count_u 100 count_v 100`** | 10 000 | **0** | **0.177 – 0.183** |
   | flat 500 | 500 | 0 | 0.054 – 0.055 |
   | hand-authored 5-node chains, short names | 500 | 400 | 0.460 – 0.474 |
   | hand-authored 5-node chains, named `I[i,j].Cn` | 500 | 400 | 0.510 – 0.513 |
   | **`source <5-node subtree> count_u 100`** | 505 | **404** | **0.507 – 0.510** |

   (1) **THE COLLAPSE CASE IS FREE, AND THE ARRAY CASE INHERITS THAT.**  10 000
   repetitions of a LEAF source cost ZERO links and measure at the flat baseline
   -- indistinguishable from the byte-equivalent hand-authored flat scene at the
   same poses and names (0.177–0.183 vs 0.182–0.184).  This is the property the
   collapse semantics were chosen for in the first place, now measured at the
   scale the counts make easy to reach.
   (2) **3c ADDS NOTHING OF ITS OWN.**  The counted subtree (505 objects, 404
   links) measures the same as the hand-authored hierarchy with matching names and
   poses (0.507–0.510 vs 0.510–0.513) -- if anything a hair faster, with four more
   links.
   (3) **LINEAR IN LINKS, AGAINST 3b's NUMBER.**  `(0.467 - 0.055) / 400 =
   1.03 µs/link/pass` on the short-name control, which is 3b's own 404-link row
   (0.45–0.46) to within measurement noise; 3b's ~1.4 µs/link is the slope at its
   4004-link point.  Nothing about counts changes the per-link cost.
   (4) **ONE MEASURED SURPRISE, and it is step 2's, not 3c's: LONGER ENTRY NAMES
   COST ~10% AT 400 LINKS.**  The identical hierarchy authored as `H37_3` measures
   0.460–0.474 and as `I[37,0].C3` measures 0.510–0.513.  `RebakeHierarchy` is
   string-keyed (three `String`-keyed containers rebuilt per pass, a String-copying
   stack), so key LENGTH is on the per-pass path -- and 3c's names are the longest
   the language can produce.  The memoized pointer-keyed plan already proposed for
   step 2 removes this along with the rest.  At ZERO links there is no walk and no
   effect (the 10 000-object rows differ only by spatial layout).

   **NOT DONE in 3c, stated plainly:** a `scenes/Tests` visual scene (render
   equivalence is carried by the DumpJob oracle against hand-written twins);
   `AgentSession::ResolveIsolateObject` is verified only through the provenance
   rows it reads, not driven end-to-end from this binary; and the ERANGE branch of
   the shared validator is reachable from a `standard_object` only for UNDERFLOW
   (`count_u 1e-999`) -- OVERFLOW is refused earlier by PASS-1's string-layer
   check, since the counts are descriptor-declared numeric.  It stays reachable
   both ways from `instance_array`, which PASS-1 skips.

   **3c review round 1 (2026-08-17) — no P1.  Four things the reviewer's
   out-of-tree mutants proved, and every one of them is about what a GREEN test
   was NOT saying:**
   - **"THE CAP CAN NOW BE TESTED" WAS TRUE OF THE ARITHMETIC AND FALSE OF THE
     ACCOUNTING.**  The cap fixture was the document's ONLY expansion, so
     `entryBudget` was still the full 1e7 when it ran and the refusal read "room
     for only 10000000 more" under BOTH `--entryBudget` deletions.  The refusal
     PRINTS the remaining budget, which makes the accounting testable for the price
     of one earlier expansion in the same document: `I source S count_u 2` over a
     3-node subtree spends 6, so the next generator's refusal must say 9999994
     (9999996 with the repetition-root decrement deleted, 9999998 with the clone
     decrement deleted).  Now asserted, both decrements red-proved.
     **CORRECTED BY ROUND 2 (below): that pins the two `source`-path decrements
     and no more.  There is a THIRD writer to the same ledger --
     `ExpandInstanceArray`'s own read + `--entryBudget` -- and it is untested in
     both directions.  "The threading" was an overclaim; "the `source` path's
     threading" is what was bought.**
     STILL UNTESTED, and cheaply so only in principle: that the budget is
     DOCUMENT-WIDE rather than per-chunk.  Separating those needs a scene whose
     total is under 1e7 (so a per-chunk cap would admit it) but over what earlier
     expansions have left -- i.e. ~1e7 entries actually materialised.  Same
     "ten million objects" cost 3b had; not paid.
   - **THE `v` NORMALIZATION HAD NO DISCRIMINATING FIXTURE AT ALL.**  The only
     `expr(v)` assertion used `count_u 1` with no `count_v`, where `v` is 0 under
     every wrong implementation too -- `v = 0.0`, an off-by-one `j/countV`, and the
     copy-paste `v = i/(countU-1)` all stayed green.  The last is the shape two
     adjacent near-identical lines actually invite, and it was the greenest.
     Aggravating: the only `v`-discriminating fixtures in the tree were on the
     `instance_array` copy, which **3d deletes** -- so this had to exist on the
     `source` path BEFORE 3d lands.  Now `count_u 2 count_v 3` +
     `position expr(u) expr(v) 0`, asserting `I[0,1]` at (0, 0.5, 0) and `I[1,2]`
     at (1, 1, 0); all three mutants red.
   - **"NOTHING HALF-APPLIED" IS THE COLLISION SCAN'S GUARANTEE, NOT THE
     EXPANSION'S.**  The scan does run in full before anything is applied — but
     per-instance parameter evaluation runs INSIDE the apply loop, and PASS-1
     validated only (i,j) = (0,0), so `position expr(sqrt(1-i)) 0 0` with
     `count_u 4` applies `I[0,0]` and `I[1,0]` and then refuses at `[2,0]`.
     **Decided: leave it, and say so in the code** (both sites now do).
     Pre-validating every (i,j) would cost a second `count_u * count_v` evaluation
     pass to buy an atomicity the surrounding machinery does not have anyway --
     PASS-2 applies chunk by chunk and `break`s on the first refusal, so a later
     chunk's failure already leaves the Job partial.  The containment is the
     CALLER's: `LoadAsciiSceneViaCst` returns false on any diagnostic, and the GUI
     re-derive dry-runs into a staging Job.
   - **`InstanceBaseName` TRUNCATED, WHICH IS EXACTLY THE INVARIANT THE COLLISION
     SCAN RESTS ON.**  `char[256]` + `snprintf`: a 253-character chunk name with
     `count_u 1 count_v 2` truncates both repetitions to `<name>[0`, the scan's
     "distinct (i,j) give distinct bases by construction" stops holding, and the
     second repetition fails at AddItem blaming an apply failure with the first
     already applied.  Now `std::string` + `std::to_string`.  `instance_array` held
     the identical shape (pre-existing); rather than fix a second copy it now CALLS
     `InstanceBaseName`, so the two spellings cannot drift in the window before 3d
     deletes it.  Both sides pinned by a long-name fixture.

   Three smaller notes, all recorded in
   [SCENE_CONVENTIONS.md](../SCENE_CONVENTIONS.md) § "Three ways a per-instance
   `expr(...)` fails QUIETLY" rather than fixed in code, and the reason is the same
   for all three -- **the evaluator is shared with the procedural painters, where
   totality is the right property**:
   - **Division / modulo by zero evaluates to `0`, so `EvalExprBody`'s non-finite
     guard never fires for `/`.**  `expr(1/(i-1))` over `count_u 2` derives x = -1
     then x = 0, silently; `count_u expr(n/0)` is a silently EMPTY array.  Newly
     REACHABLE in 3c (an instance index is a natural divisor) but not newly broken.
     The `EvalExprBody` header, which read as if exprs were protected against this,
     is corrected.
   - **`i` / `j` / `u` / `v` inside a COUNT bind to 0**, so `count_u expr(i)` means
     `count_u 0`.  Refusing them was considered and NOT done: the check would have
     to be a lexical scan of the raw token for a standalone `i`/`j`/`u`/`v`, in a
     validator SHARED with `instance_array`, to catch an authoring shape nothing in
     the corpus uses -- a parser-level heuristic bought with real refusal risk.
     Documented instead.
   - **An `expr(...)` on a BOOLEAN slot is always false**, because
     `String::toBoolean` is "first character is `t`" and an expr evaluates to a
     number.  Pre-existing and whole-value (any `expr` on any bool slot, any chunk),
     newly per-instance.  Not fixed: the fix is either a `ValueKind::Bool` check in
     `DispatchChunkParameters` (which would start refusing every non-`TRUE`/`FALSE`
     spelling in the entire corpus) or widening `toBoolean` (which changes the
     meaning of every boolean in every scene file) -- both far outside a 3c review
     round.

   **3c review round 2 (2026-08-17) — no P1.  One production fix, one hang
   converted into a red, and two claims corrected rather than tested:**
   - **`parent <counted chunk>` GOT EXACTLY THE MISLEADING DIAGNOSTIC 3c FIXED FOR
     `source`, and so did `override_object`.**  3c added a dedicated refusal for
     `source A` where `A` carries counts, precisely because the fallback message
     ("declared earlier but did not produce an object -- its own chunk failed")
     named a cause that did not happen.  The identical shape on the other two
     references was not covered: `parent I` landed on "A `parent` must be a
     DECLARED-EARLIER object; must not be this object; must not already be one of
     its descendants; and must not be a CSG operand" -- four enumerated causes and
     `I` satisfies every one of them -- and `override_object { name I }` landed on
     "target `I` not found in scene.  Possible causes: (a) ... appears BEFORE the
     chunk that creates the target (b) ... deleted (c) ... typo", none of them
     either.  Neither parser CAN do better: both run from a `Finalize` that sees
     the live manager and not the DOCUMENT, so neither can know the name belongs to
     a counted chunk.  Fixed with a document scan
     (`RefuseBareReferencesToCountedChunks`, run once between
     `BuildObjectChunkIndex` and PASS-2, refuse-all on the PASS-1 model) that names
     the real cause AND the working spelling -- `parent I[0,0]` is not an error at
     all, it is the intended idiom.  Tested both ways, with the recommended form
     driven as a POSITIVE control on each: the negative half of each pair (the old
     text is GONE) is what carries the claim, since both scenes were already being
     refused, which is exactly why the gap was invisible.
   - **A `perInstance` REGRESSION HUNG THE SUITE INSTEAD OF REDDENING IT.**
     `perInstance` is `plan.size() + 1`; drop the `+ 1` and a LEAF source (empty
     plan) computes `total = 0`, sails past the entry cap, and drops the
     per-repetition collision scan and apply loop into a `count_u * count_v` walk
     with nothing bounding it -- the leaf cap fixture (`count_u 1000000
     count_v 100`) becomes 1e8 iterations and `CstSourceInstanceTest` never
     terminates.  A hang in CI reads as infrastructure flake, not as a red test.
     **Chosen fix: an assert-and-REFUSE invariant** (`perInstance < 1` pushes a
     diagnostic and returns false), not a bare `assert` -- MSVC Release carries
     `/DNDEBUG` and `run_all_tests.ps1` defaults to Release, so an assert compiles
     to nothing on the exact configuration a hang is hardest to diagnose on (commit
     `64d73157`).  A bounded fixture was the alternative and was NOT taken: it
     would fix this one mutation's symptom while leaving every other route to
     `total == 0` unbounded.  Red-proof is the mutation itself -- the suite now
     goes red in seconds instead of hanging.
   - **THE `instance_array` LEG OF THE SHARED LEDGER HAS ZERO COVERAGE, AND IS
     DELIBERATELY LEFT THAT WAY.**  `ExpandInstanceArray` reads the budget and
     decrements it, and deleting either its `--entryBudget` or its over-budget
     refusal leaves both `CstInstanceArrayTest` and `CstSourceInstanceTest` fully
     green.  **Decided: correct the claim, do not write the test.**  3d deletes
     `ExpandInstanceArray` outright, so a fixture written now is deleted next
     commit along with the suite that holds it -- it would buy one commit of
     coverage on code with a scheduled end date, at the price of noise in the
     commit whose entire purpose is the deletion.  The round-1 overclaim is
     corrected above instead, because the CLAIM outlives the code.  **Added to
     3d's checklist:** after the deletion, verify there is EXACTLY ONE writer to
     `entryBudget` (`grep -n 'entryBudget' src/Library/Cst/Cst.cpp` should show
     only the declaration, `ExpandSourceInstance`'s parameter, its cap read and its
     two decrements) -- which is the property that makes the `source`-path ledger
     assertions total rather than partial.
   - **THE "DOCUMENT-WIDE" LEDGER IS EXPANSION-ORDER, NOT DOCUMENT-ORDER.**
     `instance_array` chunks are skipped by the PASS-2 walk and expanded in a
     trailing loop, so a generator declared FIRST has spent nothing by the time a
     later `source` refusal is composed: that refusal says "room for only 10000000
     more" while 5 entries are already committed.  The reverse order reads
     correctly.  The global cap still holds -- the generators check last, against
     whatever is left -- so this is message accuracy, not over-allocation, and it
     dies with `ExpandInstanceArray` in 3d.  Recorded as a clause on the ledger
     fixture's comment, which is true only because the earlier expansion there is a
     `source`.

   **3b review round 2 (2026-08-17) — no P1.  One durable rule, and two claims
   that were TRUE but UNPINNED:**
   - **SPLITTING ONE BIT INTO TWO PIECES OF STATE BREAKS EVERY
     CAPTURE-THEN-WRITE-BACK OF THE COMPOSED GETTER, and round 1 walked into
     that on the very interface it was being careful about.**  Round 1 gave CSG
     operands a separate consumption COUNT and said, in its own commit message,
     that it was deliberately NOT overloading `SetWorldVisible` into a counter
     *because the agent's isolate save/restore restores a captured boolean*.  It
     dodged the counting-SETTER trap and landed in the capturing-GETTER one:
     `IsWorldVisible()` became `bIsWorldVisible && nConsumedBy == 0` while
     `SetWorldVisible` still owned only the base flag, so
     `AgentSession`'s `ApplyObjectSolo` / `ObjectSoloRestoreGuard` — which
     captured the composed value for EVERY object and wrote it back through the
     setter — began zeroing the base flag of every operand in the scene on every
     `isolate` render.  Invisible while a composite still consumed them; once
     the count dropped (console `remove object`) the operands were permanently
     hidden AND still misread as operands by `SetObjectParent`, healed only by a
     full re-derive.  **The fix is structural, not a corrected capture:** the
     hide pass records ONLY the objects it actually hid and the restore writes
     `true`, which has no round-trip obligation at all.  Capturing the BASE flag
     was rejected — it needs a new accessor and still writes into every object,
     so the next state this pair splits re-opens the same hole.  **The rule to
     carry forward: when a getter starts composing state a setter does not own,
     grep for capture-then-write-back of the GETTER; the setter is not where the
     hazard is.**  Two smaller consequences of the same split, both fixed here:
     `Object::CopySnapshotStateInto` (and its `CloneFull` / `CloneGeometric`
     siblings) copied the BASE flag into a clone that starts at ZERO consumers,
     so `Scene::CreateSnapshot` — which clones every manager item by name — gave
     a consumed operand a second, WORLD-VISIBLE standalone clone (zero such
     clones before 3b, one per operand after); they copy the composed value now,
     which is exactly the pre-3b outcome.  And `Object::RemoveConsumer` clamped
     an unbalanced release to zero in silence, turning the very failure the
     count exists to prevent into an unexplained extra shape in a render; the
     clamp stays (wrapping an `unsigned` would pin the operand invisible
     forever) but the zero branch is now logged.
   - **"Correct at arbitrary depth" and "siblings keep document order across
     keys" were both TRUE and both UNPINNED, because every 3b fixture was at
     most ONE qualification level deep** — where an entry's key set is exactly
     {fully-qualified, bare} and nothing distinguishes the union from a two-key
     special case.  Deleting the cross-key `std::sort` left the whole suite
     green while a copy's child list came out reversed against the original's;
     trimming `ChildKeysOf` to the outer two keys left the suite green while a
     depth-3 scene lost a WHOLE BRANCH with zero diagnostics — round 1's P1
     re-created exactly one level deeper.  There is now a three-level fixture
     (`I1 source A` / `X parent I1.B` / `I2 source I1` / `Z parent I2.I1.B` /
     `I3 source I2`) asserting all three branches — `I3.I2.I1.Y` via the bare
     key, `I3.I2.X` via the INTERMEDIATE key, `I3.Z` via the fully-qualified one
     — their parents, the three different amounts of qualification in their
     names, and their serial order.
   - **NOT CLAIMED: the `instance_array` refusal on an INTERMEDIATE key has no
     distinguishing test, and the reason is structural.**  A key that is
     intermediate at depth 3 is `keys[0]` of the depth-2 expansion; that
     expansion is necessarily earlier in the document (declare-before-use) and
     PASS-2 breaks on the first refusal — so the scene is refused with the
     identical message whether or not the deeper walk would have caught it.
     Same shape as the override-lookup unreachability argued at that site.  What
     IS pinned is the generator refusal on a TWO-level synthesized entry
     (`parent I2.I1.B` with `I3 source I2`), which no shallower expansion can
     see.  Also unpinned by construction: the override refusal now prints the
     key that actually matched (`ov->first`) rather than always `keys[0]`, so a
     match on a shorter name names the right entry — one expression, and the
     branch remains unreachable for the reason written at the site.

   **3b review round 3 (2026-08-17) — no P1.  Every finding is a MISSING
   REGRESSION GUARD on something a previous round had just fixed or just
   widened, which is the pattern to notice: three rounds in, the code is right
   and the evidence for it keeps lagging one commit behind.**
   - **A GUARD'S *POSITIVE* CLAIM NEEDS ITS OWN FIXTURE.  The subtree walk's
     revisit guard had two fixtures for "a real cycle is refused" and none for
     "a legal repeat visit is ALLOWED"** — because every nested fixture in the
     file had exactly ONE instance per source on a LINEAR chain, so no chunk was
     ever reached twice in one expansion.  `path` is a PATH (push on entry, pop
     on exit), and the two `path.erase` lines at the tails of
     `ClonePlanBuilder::ClonedEntry` / `SourceSubtree` are the whole of that;
     deleting EITHER turns the guard into a visited-set and refuses ORDINARY
     AUTHORING with a false "recursive definition" while silently dropping the
     entire instance subtree, at 316/0.  Both shapes are mainstream — an
     assembly holding two copies of one part, then instanced itself, is the
     canonical kit-bash and is what `source` is for.  Now pinned by two
     fixtures, each red under ITS OWN deletion and green under the other's:
     `revisit-diamond` (`B parent A`, `D parent B`, `C parent A source B`, then
     `I source A` — `I`'s walk re-enters chunks B and D by a disjoint path to
     produce `I.C.D`) pins `ClonedEntry`'s erase; `revisit-sibling` (`P source
     L`, `Q source L`, both `parent A`, then `I source A`, with `L` deliberately
     a LEAF so only the chunk `SourceSubtree` pushes is visited twice) pins
     `SourceSubtree`'s.  The oracle is the WHOLE ENTRY SET plus the deepest
     composed world position, because a false refusal drops the subtree
     wholesale and a per-name spot check is the weaker instrument.
   - **ROUND 2's NEW RETURN CONTRACT SHIPPED UNPINNED, and its natural one-line
     violation resurrects every 87 CONTAINER.**  Round 2 replaced
     capture-then-write-back with "record only what you actually hid, restore
     `true`"; hoisting `hidden.push_back( obj )` above the
     `if( obj != keep && wasVisible )` gate is *restore blanket-true*, the exact
     shape the contract forbids, and left AgentViewModeRenderTest at 674/0.  The
     G1-i tests structurally cannot catch it: since 3b a CSG operand's BASE flag
     is already `true`, so writing `true` into it is a NO-OP.  **Only an object
     hidden by its BASE flag distinguishes the two, and the scene language has
     exactly one — a container** (geometry-less, created world-invisible by
     `RISE_API_CreateObjectOrContainer_`, nothing composing over that flag).
     Blanket-true resurrects it into the TLAS as a leaf with an EMPTY bounding
     box, into the nine world-visible enumerations that flag exists to keep it
     out of, and into the objectmap legend.  The isolate fixture now carries a
     container plus a `parent` child, and asserts it is still hidden after the
     restore, still hidden after TWO consecutive isolate renders, and absent
     from a later plain objectmap render's legend — that last one a different
     code path (`BuildObjectMapPalette`'s world-visible filter) and the
     user-visible face of the corruption.  **Round 2 wrote "the isolate
     red-proof cannot be a single line"; that was true of the OLD bug and false
     of the NEW contract, which has one.**
   - **AN ASSERT-ONLY TEST FILE VERIFIES NOTHING ON THE PROJECT'S DOCUMENTED
     WINDOWS PATH — INCLUDING ITS OWN SETUP.**
     `build/cmake/rise-tests/CMakeLists.txt` does not override
     `CMAKE_CXX_FLAGS_RELEASE`, MSVC's default for that config carries
     `/DNDEBUG`, and `run_all_tests.ps1` defaults to `-Config Release`.
     `CSGObjectIdentityTest` — the consumption count's ONLY regression guard
     anywhere in the tree, and the property both earlier rounds lean on — was
     ~100 lines of pure `assert()`.  Demonstrated end-to-end: with the count
     defect LIVE and `-DNDEBUG` set, the old file printed all three "Passed!"
     and exited 0, and the log line `CSGObject::IntersectRay:: No subobjects for
     this CSG object` proves the *setup* went with it (`assert(
     pCSG->AssignObjects(...) )` compiles out, so no composite was ever
     assigned and the case tested an unrelated scene).  Converted to the counted
     `Check()` / tally / non-zero-exit style, with every side-effecting call on
     its own line and only its RESULT checked; the same mutation is now red and
     exits 1 under `-DNDEBUG`.  Assert-only is a pre-existing convention in
     dozens of files under `tests/` (the review said 24; a crude grep here says
     ~34, so treat the exact count as unestablished and the convention as
     widespread) — the rule going forward is that a file carrying the sole guard
     for a load-bearing invariant does not get to be one of them.  **The rest of
     that convention is a standing latent gap, not something this round fixed.**
   - **P3s.** The generator key-scan's comment asserted the OPPOSITE of the
     correct reasoning already written 80 lines above for the identical key set:
     it claimed the union "has to" be read because a generator at `parent I1.B`
     would otherwise be invisible, but `I1.B` **is** `keys[0]` of the depth-2
     expansion that catches it, so the example argued for the union while
     demonstrating `keys[0]`.  Rewritten to the override site's actual argument
     (a key at `ki > 0` is `keys[0]` of a shallower expansion, necessarily
     earlier in the document, and PASS-2 breaks on the first refusal) and
     labelled defensive.  **Naming the narrowing round 2's commit message left
     unnamed: the obvious one — `keys[0]` only — is GREEN, verified at 327/0.**
     The CHILD walk between them is the one that genuinely needs every index,
     and it has the depth-3 guard.  Separately, `ApplyObjectSolo`'s
     `outHiddenCount` out-param was write-only tree-wide (declared, passed,
     never read; there is no such field on `AgentRenderResult` and the result
     message carries no count) and round 2's comment justified keeping it with a
     caller need that does not exist — deleted rather than re-justified.

   **3a review round 1 (2026-08-16) settled four refusal rules that the
   first implementation got subtly wrong. They are semantics, not
   phrasing, so they are recorded here. Round 2 then narrowed the last
   two of the four — as written they were each defeated by a spelling
   round 1 had not enumerated — and both now read in their corrected
   form:**
   - **`geometry none` on an instancing chunk is a SECOND FORM**, not zero
     forms. Only `source` is dropped from the instancing chunk's own
     params before the merge, so a `geometry none` written there overrides
     the geometry inherited from the source and the "copy" derives as an
     empty container. `none` is the container spelling only when there is
     no `source` to contradict.
   - **The CSG-operand refusal is a DOCUMENT scan** (`obja` / `objb` over
     every `csg_object` chunk), never the live `IsWorldVisible()` state:
     visibility only goes false once the composite's `Finalize` has run,
     which would make the rule depend on declaration order. Its reason is
     that the composite CONSUMES the operand (the `SetObjectParent` rule) —
     *not* "the operand's matrix is CSG-local", which is false here because
     the collapse semantics drop the source's matrix entirely.
   - **`source S` + `parent S` is its own refusal.** It makes `S` appear to
     have children (the instance is its own source's only child), so the
     3b has-children rule fires on a scene with no subtree. Detect
     "EVERY child of `S` is a chunk that instances `S`" — *not* "`S` has
     exactly one child", which a second self-parenting instance defeats,
     falling straight back through to the misdirection the rule removes —
     and report the recursive definition the author's `parent` line created.
     A source with a genuine other child still gets the 3b message.
   - **The full-re-derive gate is the LIVE PROVENANCE ROW, not the `source`
     text.** Provenance is retired only by `RemoveItem` / `Shutdown`, so an
     in-place re-point leaves a stale `(I, S)` row on an object that is no
     longer an instance — but keying on the text closes `source none` and
     misses `source` DELETED, which is the spelling production reaches
     (SceneEditor's agent-Undo of an INSERTED `source` routes through
     `ApplyCstParamRemoveChecked`). It also over-applies the other way: a
     plain `standard_object { source none … }` container that never had
     provenance would pay a ClearAll + full derive + manager rebind on every
     later edit forever. So: a real `source` → full derive (it needs PASS-2
     expansion); a live provenance row → full derive (one-shot, to retire
     it); neither → incremental.

   **3a review round 2 (2026-08-16) added a fifth, on the transform gate:**
   - **`CstObjectTransformKind` answers for the SOURCE's role, not the
     chunk's.** An instancing chunk is always a `standard_object`, but the
     entry is built through the SOURCE's parser — so `source <a csg_object>`
     yields a node with `position`/`orientation` and no
     `matrix`/`quaternion`/`scale`. Answering on the chunk role said "kind 1,
     commit the full `matrix`", `SceneEditor` declared the gizmo op
     committable and mutated the live object, and the deferred
     `ApplyCstObjectMatrixEdit` then hit the expansion's target-descriptor
     check and refused — the exact live/CST divergence that gate exists to
     make unreachable. `ApplyCstObjectComponentsEdit` correspondingly owns
     the csg-sourced instance as well as the authored `csg_object`;
     `position`/`orientation` are the two transform params both roles
     declare, so the commit lands on the instancing chunk and the expansion
     merges them onto the synthesized composite.

   **3a review round 3 (2026-08-16) closed the hole that reroute opened, and
   corrects what round 2 said about it:**
   - **Every transform-commit route must perform the `override_object` owner
     walk.** A same-named `override_object` is applied AFTER the base chunk
     and REPLACES the transform fields it names, so a commit written to the
     base chunk is overwritten by the very re-derive that follows it: the
     gizmo moves the object, the commit returns SUCCESS, nothing is logged,
     and the object snaps back. `ApplyCstObjectMatrixEdit` has walked to the
     override for exactly this reason since long before 87;
     `ApplyCstObjectComponentsEdit` now does too.
     Round 2's commit message disclosed this asymmetry as pre-existing. That
     is true for an AUTHORED `csg_object` and **false for a csg-sourced
     instance**: at 469dc2c2/77969fd6 the instance answered kind 1 and its
     drag went through the MATRIX route — which has the walk, and which
     `override_object` can record (it declares `matrix`) — so the drag
     WORKED. Re-answering the kind as 2 rerouted it to a function without
     the walk and converted a working drag into a silent revert. A reroute
     is only safe once the destination carries every guarantee the origin
     did.
   - ~~**`scale` is the one param the two routes treat differently.**~~
     **WITHDRAWN by round 3a (2026-08-16) — see the correction below.** Round 3
     stripped `scale` only from a BASE chunk and KEPT it on an
     `override_object`, reasoning that the override's per-field branch applies
     position, orientation and scale independently so a `scale` there is live
     on the object being dragged. **That reasoning is excluded by the
     function's own contract and the guard preserved a wrong answer.** Both
     halves were measured:
     - The only production caller,
       `SceneEditor::CommitPendingCstObjectTransforms`, derives the
       `position`/`orientation` it passes from `DecomposeRigid`, which refuses
       any non-unit column magnitude — and the editor's post-mutate gate has
       already restored-and-refused the gesture before the commit runs. **A
       real `scale 2 2 2` on an override cannot reach the kept-scale branch at
       all**; the gesture is refused and the object never moves.
     - The scales that DO reach it are unit SIGN FLIPS. `DecomposeRigid`
       admits `scale -1 -1 1` (all magnitudes 1, det > 0) and FOLDS its
       180-degree rotation into the `orientation` the commit writes; keeping
       the `scale` applies that rotation a SECOND time. Measured on the full
       production path with `override_object { name C  scale -1 -1 1 }` and a
       panel `position 5 0 0`: live x = 4.75, committed x = 5.25 — silently
       wrong by +0.5; with `scale` stripped, 4.75. (Not a regression from
       round 3 — the pre-round-3 document derives 5.25 too — but the guard is
       what PRESERVED the wrong answer.)

     **The rule is therefore symmetric: `matrix`, `quaternion` and `scale` are
     all stripped from whichever chunk the commit lands on**, base or override.
     It is safe for the same reason the matrix route's strip is: the caller's
     `DecomposeRigid` contract guarantees unit magnitudes, and a sign flip's
     rotation content is already carried by the `orientation` being written,
     exactly as the matrix route's `matrix` carries it.
   - **A refusal names what the AUTHOR wrote.** The kind-2 scale refusal on a
     csg-sourced instance said "csg_object has no scale param" for a chunk
     spelled `standard_object { name I  source C }` — a chunk type absent
     from their scene, and no mention of the `source` line that is the whole
     reason for the restriction. The provenance-based rewrite beside it
     cannot fire in the collapse case (it is gated on
     `instancingChunk != objectName`, and the collapse row is `I -> (I, S)`),
     so the message keys on the provenance row's SOURCE field instead — which
     IS populated there, and is empty for an `instance_array` entry.

     **The gate one hop further in had the same defect (fixed 2026-08-16).**
     The POST-MUTATE decomposability check answered all five `DecomposeRigid`
     rejections with one line about ROTATION and GIMBAL-LOCK. A pure
     `position 5 0 0` on an object whose only blocker is a non-unit `scale`
     (from a same-named `override_object`) was refused with "rotation is not
     committable … gimbal-lock" — for an author who neither rotated anything
     nor went near a singularity. `DecomposeRigid` now reports WHICH rejection
     fired (not-affine / non-unit scale / shear / reflection / gimbal-lock /
     rebuild-mismatch) and the refusal is built from it.
   - **The transform assertions are end-to-end now.** Every transform
     assertion in `tests/CstSourceInstanceTest.cpp` used to call
     `Job::ApplyCstObject*Edit` DIRECTLY — the middle of the chain, and where
     three rounds of defects hid, because the direct call accepts inputs the
     real caller can never produce (that is how the `scale 2 2 2` fixture
     pinned the defect above). The file now also drives
     `SceneEditController → SceneEditor::Apply → the DecomposeRigid gate →
     CommitPendingCstObjectTransforms → ApplyCstObjectComponentsEdit` and
     asserts the FINAL COMMITTED pose, for an authored `csg_object` and a
     csg-sourced instance, with and without an override.
   - **FIXED (2026-08-16), and now pinned:** the components commit
     DOUBLE-APPLIED a translation on the no-override path (authored
     `csg_object`, no `source`: panel `position 5 0 0` committed to 10.25). The
     live gesture pushes onto the transform STACK via
     `TranslateObject`/`PushBottomTransStack`, then the INCREMENTAL re-apply
     called `SetPosition` without clearing the stack, so
     `FinalizeTransformations` folded T(5)·T(5). Pre-existing (it predates step
     3a — `CstObjectTransformKind` has always answered 2 for an authored
     `csg_object`); csg-sourced instances escaped it only because a `source`
     chunk forces the full re-derive, an accident of the incremental-refusal
     rule rather than a guard, so anything that later restores an incremental
     path for `source` chunks would have inherited it.
     **Fix:** `Job::AddCSGObject` now calls `ClearAllTransforms()` before its
     `SetPosition`/`SetOrientation` — the reset `Job::AddObject` has done since
     review P1.1 and `AddObjectMatrix` gets for free from
     `SetFinalTransformMatrix` → `ReplaceFinalStack_`. Those are the only three
     in-place re-point sites (`m_bIncrementalRepoint`), so the family is closed.
     Clearing — rather than writing the pose the way the MATRIX route does — is
     what keeps this path off the stack, per §2's rule: a full derive builds a
     fresh object with an EMPTY stack, so a surviving entry is live state the
     document does not describe, and stacking a composed pose here would make
     `override_object`'s per-field arm compose on top of a stacked translation
     instead of replacing it. **Pinned** by `e2e-authored-csg-no-override` in
     the `CstSourceInstanceTest` end-to-end table plus an `e2e-repeat` block
     that drives a SECOND panel edit (under the bug the error changed sign:
     5 → 10.25, then 3 → 1.25), both asserting the committed pose.
4. **UI: Objects as a recursive tree** over the AUTHORED graph — a
   generic node-children API replacing per-category flat lists; Qt to a
   real tree model, Swift to ~~`OutlineGroup`~~ (see below); expand state
   keyed by tree PATH, not name. Both outliners are currently hand-rolled
   two-level lists, so this is a structural rewrite in each.

   **⚠ `OutlineGroup` WAS THE WRONG TOOL, and this clause contradicted its
   own next clause.  Corrected 2026-08-18 during 4b.**  `OutlineGroup` owns
   its expansion state and exposes no binding to it, so it CANNOT implement
   "expand state keyed by tree PATH" — the requirement in the very same
   sentence.  Second, independent reason: the eleven category headers are
   not tree nodes; they expand through the bridge (`collapseSection` /
   `setSelection(cat, "")`), which `PropertiesPanel` also reads, and
   `OutlineGroup` offers no hook to route a toggle into a bridge call — so
   the top level would stay hand-rolled and the result would be a hybrid,
   not the uniform recursion this step is asking for.

   Recursive `DisclosureGroup` was ALSO rejected, deliberately: it brings
   platform chevron chrome, while the category header already draws a
   hand-rolled `▸`/`▾`, and mixing the two disclosure idioms in one list is
   a visual regression.  **What 4b shipped instead:** the tree is flattened
   to visible rows (`outlinerFlatten` + `outlinerVisibleRows`, both pure and
   iterative) and drawn by a single `ForEach`, which keeps the existing
   `ScrollView`/`VStack` structure, reuses one glyph at both levels, and
   sidesteps SwiftUI's opaque-type recursion limit (`some View` cannot
   contain itself) without `AnyView`.  The recursion lives in the flatten,
   iteratively — for the same reason `BuildAuthoredTree` is iterative.

   **The Qt half (4c) went the other way, and 87's "real tree model" was
   right there.**  `QAbstractItemModel` + `QTreeView` has none of
   `OutlineGroup`'s obstacles: it exposes expansion via
   `setExpanded`/`isExpanded` and lets the model own index identity.  What
   the naive form WOULD have broken is the flat categories — a default
   `QTreeView` draws its own branch indicators and depth insets, moving
   every depth-0 row.  `indentation(0)` + `rootIsDecorated(false)` + a
   delegate that owns the whole row is what makes "a real tree model" and
   "a flat category looks exactly as before" compatible.  `QModelIndex::
   internalId()` holds a slot in the model's OWN flat row table, never a
   controller `TreeNodeHandle`, and a rebuild resets the model so every
   index is invalidated — which is how 4a's no-handle-across-a-turn rule
   is honoured on a surface built to tempt the opposite.

   4c also **added `roots` to the Qt payload** rather than re-deriving it
   by scanning for `parent == -1`: sibling ORDER is contract (§2, display
   order comes from declaration order) and only `AuthoredTree::roots`
   states it, while the scan works solely because `BuildAuthoredTree`
   happens to emit in presentation order — an assembler detail.  Safe to
   change the signature because 4a shipped `categoryTree()` with zero
   consumers.

   Two further findings from 4c, both in code it merely passed through:
   the pre-4c `kCategories` table held **copies** of `Theme::cat*`, which
   are `inline QColor` globals that `applyThemeTokens()` reassigns IN
   PLACE — so after one Dark/Light switch every tag chip painted the
   previous palette forever.  Fixed by storing pointers and dereferencing
   at paint time.  And the LIVE THEME-SWITCH CONTRACT's queued-rebuild
   clause **no longer applies** to this panel: it exists for panels whose
   row widgets bake tokens at construction, and there are no row widgets
   any more, so the fix collapses to `viewport()->update()`.  A deliberate
   deviation from a written contract, recorded here rather than buried.

   **⚠ THE QT CODE IS UNCOMPILED.**  No Qt and no Windows toolchain on the
   development machine; it needs a Windows build before it can be trusted.
   Specifically unobserved: row height and vertical centring, whether
   `indexAt()` returns an index for the zero-width branch region, whether
   `customContextMenuRequested`'s position is viewport-relative on that Qt
   build, whether the `QTreeView` background stylesheet perturbs
   `PE_PanelItemViewRow`, and how `setViewportMargins` interacts with the
   305px cap.

   **Selection is by NAME, so two same-named siblings highlight together.**
   4a settled that handles are not durable and selection addresses an
   entity name; the tree can now legitimately contain two rows with the
   same name — the Painter union's `DUP` (`uniformcolor_painter` and
   `scalar_painter` may each declare it; `CollectPainterUnionEntries`
   deliberately does not dedup, and `ChunkNamePath` keys on
   `role + "/" + name`, so both survive the derive).  `isSelected` is
   `selectionName == node.name`, so clicking either highlights BOTH.
   Inherent to name-selection, not introduced by step 4; changing it means
   changing selection identity, which is a larger decision than the
   outliner.  Recorded here rather than left to be rediscovered.

   Note the two shells diverge on the related PATH-KEY question, and that
   is correct rather than drift: on macOS the key IS row identity
   (`ForEach(rows, id: \.path)` silently drops a duplicate id), so 4b's
   key had to become node-injective via an occurrence suffix; on Qt
   identity is the model's own table slot in `QModelIndex::internalId()`,
   so a key collision could only tie shared disclosure state — and cannot
   even do that today, because Painter is the only category that can
   collide and `BuildCategoryTreeLocked_` seeds every non-Object category
   with an empty `parent`, making its rows childless BY CONSTRUCTION.
   Both shells carry the same occurrence encoding anyway, so the latent
   case is closed on both.

   **The GUI check-scene is `scenes/Tests/Geometry/object_instancing.RISEscene`**
   (added 2026-08-18).  Nothing in the corpus exercised `source` or the counted
   arrays -- `object_parenting.RISEscene` covers step 1 only -- so there was no
   scene a person could open to see any of step 3 or step 4 work.  This one
   authors ONE four-node lantern and then instances it three ways (subtree
   clone, leaf collapse, 3x2 counted array), giving 42 live objects from 7
   authored chunks and 7 outliner rows.  Its header comment enumerates what to
   look for, including the two things only a human can confirm: that the array
   is one editable row rather than thirty, and that picking a copy in the
   viewport highlights that row (step 4d).

   **Two consequences of §2 that will look like regressions and are not.**
   The Objects count now counts AUTHORED nodes, so an 8x8 `count_u`/`count_v`
   array counts 1, not 64 — that is the point of the fold: every row has a
   chunk to edit.  And selecting a synthesized clone (`I[1,0]`) in the
   viewport highlights NO outliner row, because it folded into `I`; mapping
   a clone back to its fold target needs the provenance map, which the
   bridges do not expose.  Open, not papered over.

Then the agent surface (`build_element` emits a container plus parented
children; `place_element` becomes ONE transform edit on the container).

## Appendix — alternatives considered and rejected

**Live traversal (`GroupObject : Object`, ray transformed per level,
N-ary `CSGObject`).** Rejected: it blinds the nine enumeration sites,
costs a matrix transform per level per intersection, breaks the SAH's
uniform-leaf-cost assumption, and amplifies the documented per-sample
motion-blur race from "one object jitters" to "a whole subtree moves".
Flattening removes all four problems instead of solving them.

**Flatten at derive time (what 86 shipped).** Rejected: composition
happens once, so animating a parent never propagates — hierarchical
animation is impossible by construction.

**Geometry groups (`GroupGeometry : IGeometry`).** Dropped, not
deferred. `sdf_geometry` already IS geometry-level grouping for implicit
shapes (7 primitives, boolean + smooth ops, per-part transforms, full
`IGeometry` contract with area sampling); object parenting covers
assemblies of arbitrary kinds. The remaining sliver — a union of
non-implicit geometry as one `IGeometry` — would cost the full 10-method
contract, of which uniform-area sampling over a transformed union is the
hard part and precisely the bug class that has bitten twice. The one
capability with no workaround: a compound of MESHES as a
`path_instances_geometry` template or `displaced_geometry` base.

**A separate `instance` node.** Folded into `standard_object`'s `source`
mode — three chunk types collapse to one node with three modes.

**Multi-parenting as instancing.** Rejected: parent links give one
parent by construction, and an explicit `source` reference is both
easier to reason about and gives instances a NAME for provenance.

**A forward-closure primitive for incremental derive.** No longer
needed: derive stops composing anything, so a container edit is an
ordinary one-chunk param edit.
