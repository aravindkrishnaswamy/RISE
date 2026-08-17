# 87 — Recursive Scene Graph

**Status: decided 2026-08-15.  §5 steps 0, 1 and 2 are IMPLEMENTED, and so are
step 3's slices 3a (`source`, single-node) and 3b (subtree expansion); 3c
(`count_u`/`count_v`) , 3d (delete `instance_array`) and step 4 are not.**  §2's "composition happens in the per-frame prepare pass" is now
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
+ count/expression mode of `standard_object` (killing its document-wide
incremental-derive refusal), and `mChunkAttribution` — the agent's
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
  `source`. **The refusal survives step 3, renamed**, and retiring it is its own
  arc — it also needs a typed "drop this chunk's N synthesized objects"
  primitive, which the provenance map is the inverse index for.
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
   `instance_array`.

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

   **The incremental-derive trace was closed for subtree MEMBERS, one hop
   further than 3a.**  The reference graph's `parent` edge runs child → parent,
   so editing a member reaches that member's chunk and stops — the instancing
   chunk never enters the closure, and the incremental apply would re-point the
   member while N stale clones kept the old binding.  The plan walk therefore
   resolves each member through `objMgr->GetItem` inside the derive's armed
   resolution sink, which records the member's chunk as a producer the expansion
   consumes.  Editing a member now reaches `I`, and `I` carries `source`, which
   the existing per-chunk gate turns into a full derive.  Chunk INSERT and
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
   real tree model, Swift to `OutlineGroup`; expand state keyed by tree
   PATH, not name. Both outliners are currently hand-rolled two-level
   lists, so this is a structural rewrite in each.

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
