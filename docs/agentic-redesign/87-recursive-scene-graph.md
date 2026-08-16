# 87 — Recursive Scene Graph

**Status: decided 2026-08-15.  §5 steps 0 and 1 are IMPLEMENTED; steps 2–4 are
not.**  The prose below is the DESIGN as decided and is deliberately left
as-written — in particular §2's "composition happens in the per-frame prepare
pass" describes the END STATE, which §5 step 2 delivers; step 1 composes at the
derive tail and on every live edit, and animating a parent does not yet move its
children (see `docs/SCENE_CONVENTIONS.md` §5).
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
- rewrite `scenes/Tests/Geometry/group_basic.RISEscene` and
  `group_gallery.RISEscene` as parent-link scenes

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

### Known, accepted, NOT fixed by step 1

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
3. **Instancing via `source`**, folding in `instance_array`. Needs the
   provenance/naming decision (`name[i]`).
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
