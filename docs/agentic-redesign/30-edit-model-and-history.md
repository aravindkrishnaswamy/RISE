# Facet 3 — Edit Model, History & State

> **Updated per [`01-DECISIONS.md`](01-DECISIONS.md) (review round 1).** Sections
> below conform to the ratified decisions; where this doc and a decision differ,
> [`01-DECISIONS.md`](01-DECISIONS.md) wins. Decisions this facet implements:
> **D1** (immutable COW derived-scene snapshot + the single gesture model),
> **D2** (red-green CST: relative widths + red cursor, no absolute spans),
> **D6** (external-file conflict: load/flush fingerprint + compare-and-swap save),
> **D7** (v7 is a single-file *tree*, not a forest), and **D9** (dual identity:
> immutable `NodeId` for lineage, name-path for addressing).

> **Status:** design-in-progress. One of the parallel facet docs under
> [`00-CHARTER.md`](00-CHARTER.md). Design only — no source/build/scene changes.
> This doc supersedes the entire current edit subsystem
> (`SceneEdit` / `SceneEditor` / `EditHistory` / the `SceneEditController`
> transaction machinery). See §3 for the precise deletion inventory.

**Owns (charter §6, row 3):** the edit primitive, undo/redo as CST versioning,
unification of structured + text edits (L2), gesture debouncing (O2), the
document-vs-session split (L4), branch semantics, and the explicit deletion
inventory.

**Neighbors I depend on:**
- **Facet 1** (`10-scene-language-and-cst.md`) — owns the lossless red-green CST
  data structure (D2), node identity (the immutable **NodeId** for lineage +
  **name-path** for addressing, D9), text↔CST round-trip, and the descriptor
  schema. I consume its `Cst` value type, its `NodeId` (identity) and `NodePath`
  (addressing), and its `parse(text) → Cst` / `serialize(Cst) → text` functions.
  **I do not define the CST; I define how it is mutated and versioned.**
- **Facet 2** (`20-derivation-engine.md`) — owns `derive(Cst) → Scene`,
  incrementally and deterministically. I hand it CST versions and a changed-node
  set; it returns a new derived scene (or an incrementally patched one).
- **Facet 4** (`40-dynamic-ui.md`) — consumes the version I publish and issues
  structured edits back through my one edit API.
- **Facet 5** (`50-agentic-surface.md`) — the MCP server is *just another client*
  of the same edit API.

---

## 1. Current-state grounding (what exists today)

Everything below is **Model A**: a live mutable `Scene` is the working state,
edits are commands that mutate it, and undo is reconstructed from captured
inverse state. The subsystem lives in `src/Library/SceneEditor/`.

### 1.1 The edit record — `SceneEdit` (`SceneEdit.h`)

A trivially-copyable tagged union. One `enum Op` with **24 op kinds**
(`TranslateObject`, `RotateObjectArb`, `SetObjectPosition`, …,
`SetMaterialProperty`, `SetMediumProperty`, plus `CompositeBegin` /
`CompositeEnd` markers). Payload is a fixed bag of fields shared across all ops:
`v3a/v3b/s`, `objectName`, `propertyName/propertyValue`, and — critically — a
**large block of `prev*` capture fields** so undo can restore prior state:
`prevTransform` (Matrix4), `prevTransformState` (`TransformState`),
`hasTransformState`, `prevBindingWasNull`, `prevPropertyValue`,
`prevShadowFlags`, `prevTime`, `prevCameraPos/LookAt/Up/TargetOrient/Orient`,
`cameraSnapshot` (a 30-field `CameraSnapshot` so `AddCamera` can be re-played),
plus identity bookkeeping `historySeq` and `capturedTargetSerial`. Static
predicates `IsObjectOp` / `IsCameraOp` / `OpNeedsSpatialRebuild` /
`IsCompositeMarker` classify ops.

> The shape of this struct is itself the P-STATE disease in miniature: every new
> op widened a shared field bag, and undo correctness depends on every relevant
> `prev*` being captured at exactly the right moment.

### 1.2 The mutator — `SceneEditor` (`SceneEditor.{h,cpp}`, ~1800 lines)

The "only sanctioned mutator." Public surface: `Apply(edit)`, `Undo()`,
`Redo()`, `BeginComposite/EndComposite`, plus the H1/H2/H3 hardening additions
(`CaptureDirtyState/RestoreDirtyState`, `DirtySnapshot`, `HasUnsavedChanges`,
`FireDirtyChangedIfTransitioned`, `ForceCompositeDepthZero`). Internally:

- **Apply** = `CaptureForApply(edit)` (resolve target + run **~30 rejection
  gates** + record every `prev*`) → `ResolveTargetSerial` → `ApplyForwardMutation`
  → `mHistory.Push`.
- **The H2 dispatchers** `ApplyForwardMutation` / `ApplyRevertMutation` are the
  single per-op forward/revert switches (each ~140 lines), shared by single +
  composite paths. They were collapsed from five parallel walks (the P-WALK fix).
- **Composite atomic rollback** (`Undo`/`Redo`): walk the history LIFO/FIFO
  reverting/replaying inner edits with nesting depth tracking; on *any* failure,
  re-apply what was already reverted and move the whole group back across the
  stacks (`RestoreLastUndoFromRedo` × N) so a failed composite is a true no-op.
- **Invariant chain** `RunObjectInvariantChain` = `FinalizeTransformations` →
  `ResetRuntimeData` → `InvalidateSpatialStructure` → conditional light-gen bump.
- **Light-topology invalidation** (the P-INVALIDATE surface):
  `BumpSceneLightGeneration{,IfEmitterSetChanged,IfMaterialEmits}` —
  `dynamic_cast` to concrete `Implementation::Scene` and bump a generation
  counter so a reused `RayCaster` rebuilds its `LightSampler`.
- **Identity serial** (`capturedTargetSerial` + `ResolveTargetSerial`):
  remove+re-add of a different instance under the same name is detected by
  comparing a per-entity monotonic serial at capture vs apply; mismatch ⇒ refuse.
- **Manager plumbing**: borrowed nullable `mMaterialManager`, `mShaderManager`,
  `mPainterManager`, `mScalarPainterManager`, `mJob`. O(N) reverse-lookups
  (`FindManagerName`, `FindMediumName`, `FindGeometryName`) recover names because
  `IMaterial`/`IShader` don't expose `GetName`.

### 1.3 The history — `EditHistory.{h,cpp}`

Two `std::deque<SceneEdit>` (undo / redo), cap 1024. `Push` stamps a monotonic
`historySeq` and clears redo. `TrimToMax` is nesting-aware (never splits a
composite group, never trims an open/most-recent group). Adds for transactions:
`NextSeq`, `PeekUndoSeq`, `MaxTrimmedSeq`/`DidTrim`,
`Snapshot{Redo,Undo}ForRollback` / `Restore*FromSnapshot` /
`ClearRollbackSnapshots`, and `RestoreLast{Undo,Redo}From*`. Also tracks
`mDirtyObjects` for round-trip save.

### 1.4 Transactions, gestures & session state — `SceneEditController.cpp` (~4100 lines)

Conflates **three concerns**:
1. **The render orchestrator**: a render thread + cancel-and-park loop
   (`Start/Stop`, `RenderLoop`, `DoOneRenderPass`, `KickRender`, `mMutex/mCV`,
   `mRendering/mEditPending/mCancelProgress`, preview-scale adaptation, polish
   passes, watchdog).
2. **The transaction machine**: `BeginTransaction` (capture `EditorStateSnapshot`
   baseline = `historyMarker` + dirty + selection; snapshot undo/redo stacks),
   `RollbackTransaction` (cancel-park → `mEditor.Undo()` while
   `topSeq >= historyMarker` → `ClearRedo` → `ForceCompositeDepthZero` → reset
   gesture flags → `RestoreEditorState` → conditionally restore stacks),
   `EndTransaction` (record-only close).
3. **Ephemeral session/view state**: selection (`mSelectionCategory`,
   `mSelectionName`, `mSelectionByCategory[9]`, `mSectionExpanded[9]`), active
   tool + per-category memory (`mTool`, `mLastSubToolPerCategory`), gizmo drag
   (`mGizmoDrag` ~20 fields: anchors, pivot projection, axis screen-velocities),
   gesture bookkeeping (`mPointerDown`, `mGestureOpenedComposite`,
   `mScrubOpenedComposite`, `mScrubInProgress`, `mLastPx`), `mSceneEpoch`,
   preview camera dims.

Gestures: `OnPointerDown` opens a composite + captures gizmo drag-start;
`OnPointerMove` converts pixel deltas → constrained world deltas → a `SceneEdit`
applied **per frame**; `OnPointerUp` closes the composite + queues a polish pass.
Timeline scrub: `OnTimeScrubBegin/Scrub/End`. Selection validity:
`DropStaleSelection_` re-checks the selection resolves after every undo/redo.

### 1.5 Round-trip save (the L7 "SaveEngine gap" — already partly built)

`SaveEngine.{h,cpp}` + `SourceSpanIndex` + `OverrideSpanIndex` +
`TransformSnapshot` + `DirtyTracker`. Mode A = in-place byte-splice of a source
line; Mode B = managed `override_object` block for entities the source can't
represent in-place. This is a **whole subsystem dedicated to preserving the text
the user typed while writing back model deltas** — i.e. a byte-level
reimplementation of "lossless round-trip" *because Model A's truth is the live
scene, not the text*. **Model B's canonical CST dissolves this entire problem
class** (see §3, §4.2).

### 1.6 The de-brittling post-mortem (the *why*)

[`docs/gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md`] catalogues ~24 P1
defects across 8 review rounds, reducing to **four root patterns**:

| Pattern | One-line essence | Where it lives today |
|---|---|---|
| **P-STATE** | the undo/transaction baseline is a *hand-assembled list of captured state*, perpetually incomplete | every `prev*` field in `SceneEdit`; `EditorStateSnapshot`; the "5th dirty set" saga |
| **P-WALK** | edit handling *duplicated across parallel walks*; a fix to one must be hand-replicated | the (now-collapsed) 5 Apply/Undo/Redo/composite walks |
| **P-INVALIDATE** | light/env invalidation *scattered across every mutator* | the `Bump*` family open-coded at ~a dozen sites |
| **P-FFMATH** | `-ffast-math` folds NaN/Inf sentinels ⇒ false-green tests | 3 test files (now guarded by `SourceHygieneTest`) |

The unifying lesson: **brittleness = state/logic/invalidation replicated instead
of owned by one chokepoint.** §2.7 shows how Model B *structurally* dissolves
each — not by adding more chokepoints, but by removing the representations whose
mutual consistency created the bugs.

---

## 2. The Model-B design

### 2.0 One sentence

> **An edit is a function `Cst → Cst` that path-copies a new immutable *green*
> root (D2); undo/redo is a pointer move over an immutable, structurally-shared
> version DAG of those green roots; structured edits and text edits are the *same*
> function applied two ways; a gesture advances an uncommitted head per move and
> coalesces into exactly one committed version at gesture end (D1); and *nothing
> the renderer or the panels needs is captured for undo, because the prior version
> still exists.***

This is the entire model. Sections 2.1–2.9 make it concrete. The substrate is the
**red-green tree** (D2): immutable green nodes store *relative width* + a stable
**NodeId** (D9), never an absolute span; absolute positions are computed on demand
by a version-specific red cursor.

### 2.1 The version model: red-green CST + structural sharing (D2)

The CST (owned by Facet 1) is an **immutable persistent red-green tree** (D2,
rust-analyzer / Roslyn lineage). Every **green** node is immutable,
content-addressed, reference-counted, and shared; it stores **relative width**
(byte length incl. its trivia), typed content, the descriptor link, and a stable
**NodeId** (D9) — **never an absolute byte offset**. An "edit" produces a **new
green root** that shares all untouched subtrees with the old root (path-copying;
the changed spine is O(depth)). **Absolute positions are computed on demand** by a
version-specific **red cursor** that walks from a root accumulating widths
(O(depth)); the red layer is cheap and is *not* stored in the shared green nodes.

> **No absolute-span maintenance (D2).** There is no `CstSpan`
> (byteBegin/byteEnd) stored in nodes and no `ApplyOffsetDeltas` offset-shifting
> machinery — a length change does **not** ripple stored offsets through later
> nodes, so sharing is genuinely O(depth). Any edit or diagnostic that needs an
> absolute position derives it via the red cursor. (See §2.8 for why this is what
> makes the cost model real.)

```
        v0 (green root)            v1 = edit(v0, objects/sphere.position, "1 2 3")
        /   |   \                       /   |   \
    film  objs  lights            film   objs'  lights      ← film, lights SHARED
            |                              |                   (same green nodes,
          sphere                        sphere'                 same NodeIds)
          /  |  \                       /  |  \              ← only the spine to the
       geom pos mat                  geom pos' mat             edited leaf is path-copied;
                                                               geom, mat SHARED (NodeIds preserved)
```

A **version** is just `shared_ptr<const GreenNode> root` plus metadata:

```cpp
struct Version {
    NodeRef                 root;          // immutable GREEN CST root (Facet 1's type)
    VersionId               id;           // monotonic, process-unique
    VersionId               parent;       // the version this was derived from
    EditLabel               label;        // "Move sphere", "Edit material.ior", "agent: add light"
    Origin                  origin;        // Gui | TextBuffer | Agent | Load
    ChangeSet               changed;       // NodeIds whose subtree differs from parent (D9; drives Facet 2)
    std::chrono::time_point committedAt;
};
```

`NodeRef` is `boost/std::shared_ptr<const GreenNode>` (or an arena handle — Facet
1's call). The key property: **`root` is immutable**, so holding a `Version` is
holding a complete, self-consistent snapshot at O(1) cost. There is no "live
scene that must be reverted." Undo *is* re-publishing an older green `root`.

> **Identity vs addressing in the version (D9).** `ChangeSet` carries **NodeIds**
> (the immutable lineage identity), not name-paths — so it is rename-stable and is
> exactly what Facet 2's incremental derivation keys on. A name-path
> (`objects/sphere.position`) is **addressing**: it resolves to a NodeId *within a
> given version* and is what humans/agents type. The two are bridged at the API
> edge (§2.2), never conflated in stored state.

**Why persistent tree, not the alternatives** (evaluated, charter §4 O1):

| Candidate | Verdict |
|---|---|
| **Full snapshot per edit** (deep-copy the CST) | rejected: a Sponza-class scene CST is MBs; a 60Hz drag for 5s = 300 deep copies = unbounded RAM + GC pauses |
| **Text-diff / patch list** (store only byte diffs) | rejected as the *primary* model: replaying N diffs to reach version k is O(N) and re-parsing each is the derivation tar-pit; **kept as the on-disk/branch persistence format**, see §2.8 |
| **Inverse-edits** (today's `prev*`) | rejected: this *is* P-STATE — see §2.7 |
| **Persistent (structurally-shared) immutable tree — the red-green model (D2)** | **chosen**: green nodes store relative width + NodeId (no absolute spans), positions via the red cursor; O(depth) memory/time per edit, O(1) snapshot, atomic by construction, natural branch support. The *only* candidate consistent with both INV-4 (lossless) and the O(depth) cost model. |

### 2.2 The edit primitive: a CST patch

One type. Both clients (GUI, agent, text) ultimately produce it.

```cpp
// A patch is a pure description of a CST transformation. It does NOT carry
// inverse/prev state — the previous Version is the inverse.
struct CstPatch {
    enum class Kind { SetField, InsertNode, RemoveNode, ReplaceSubtree, Rename, ReparseSpan };
    Kind            kind;
    NodeRefId       target;        // the immutable NodeId being edited (D9, lineage identity)
    // Clients address by name-path; the API resolves name-path → NodeId against the
    // base version BEFORE building the patch, so the patch is rename-stable.
    // SetField:        newText (the verbatim token(s) to splice for this field)
    // InsertNode:      newSubtree + position
    // RemoveNode:      (target only)
    // ReplaceSubtree:  newSubtree
    // Rename:          newName (NodeId-preserving; rewrites the name token + all referrers, D9)
    // ReparseSpan:     rawText (a re-typed region; see §2.4)
    CstFragment     payload;       // parsed fragment or raw text, per Kind
    EditLabel       label;         // human/UI-facing; defaults from Kind+target
};
```

> **Addressing vs identity at the patch boundary (D9).** Clients (panel, agent,
> text view) speak **name-path** (`objects/sphere.position`); the edit API resolves
> that to the immutable **NodeId** *against the base version* and stamps it into
> `target`. The patch therefore survives a concurrent rename of an ancestor — it
> still points at the right node. **Rename is its own first-class, NodeId-preserving
> patch kind** (`Kind::Rename`): it rewrites the name token in place (same NodeId)
> and rewrites every referrer found via the `referenceCategories` rename hint (D4),
> so UI/agent bindings keyed on NodeId survive automatically. This is strictly
> better than treating rename as remove+add (which would mint a fresh NodeId and
> drop bindings — see §4.1).

Applying a patch is a **pure function** owned jointly with Facet 1:

```cpp
// Returns a NEW root sharing all untouched subtrees, plus the changed-path set.
// Pure: no I/O, no global state, no mutation of `base`.
struct ApplyResult { NodeRef newRoot; ChangeSet changed; std::optional<ParseError> error; };
ApplyResult ApplyPatch(NodeRef base, const CstPatch& p);
```

`ApplyPatch` is the **single edit pathway (L2, INV-6)**. There is exactly one
function that changes a CST. Validation against the descriptor schema (L6)
happens *inside* it (or in a thin `Validate(patch, schema)` it calls first), so
GUI and agent edits are validated identically.

#### 2.2.1 How a gizmo drag becomes a patch on `objects/sphere.position`

Today (`OnPointerMove`): pixel delta → constrained world delta → `SceneEdit{op:
TranslateObject, v3a: worldDelta, objectName}` → mutate live object. The pixel→
world math (the `ProjectWorldToScreen_` / axis-velocity solve) is **kept verbatim
— it is correct and orthogonal to the storage model** (it's a *view→document*
projection, exactly the kind of thing Model B blesses). What changes is the tail:

```
OnPointerMove(px):
    worldDelta   = SolveConstrainedDrag(px, mGizmoDrag)        // UNCHANGED math
    posNodeId    = resolve("objects/sphere.position", head)     // name-path → NodeId (D9)
    newPosText   = FormatVec3(currentPos(version) + worldDelta) // read from current Version's CST
    patch        = CstPatch{ SetField, posNodeId, newPosText, label:"Move sphere" }
    session.StageGesturePatch(patch)                            // coalesces; see §2.3
```

Note the gizmo now produces an **absolute** `SetField` (the new position value),
not a relative delta. This is strictly better for the version model: each staged
patch is idempotent-on-replay and the coalescer (§2.3) just keeps the latest.
The "drag-start anchor" (`mGizmoDrag.dragStartMatrix`, the
`ScaleObjectFromAnchor` machinery) survives only as a *session-state read base*
for the math — it is **not** persisted and **not** needed for undo (undo = the
pre-gesture Version).

#### 2.2.2 How a property-field edit becomes a patch

`material.ior` typed as `1.5` in a panel field → the API resolves
`materials/glass.ior` → its NodeId (D9) → `CstPatch{ SetField, iorNodeId, "1.5" }`.
The descriptor schema (L6) tells the panel the field is a scalar; `ApplyPatch`
validates the token parses as one. **No `prevPropertyValue` capture, no
reverse-lookup, no manager plumbing** — the patch references the field by its
immutable NodeId (addressed by name-path) and writes verbatim text into the CST
leaf.

### 2.3 The gesture model (D1): uncommitted head + ephemeral preview, one committed version

A gesture (drag, scrub, slider) must be **one undo unit** without the old
composite-marker machinery. The model is the single one ratified in **D1**
(resolving F3's earlier three descriptions): **each pointer-move advances an
*uncommitted head* — a persistent-tree green root (D2) — and derives a *cheap
preview snapshot* (debounced per O2, ephemeral, NOT a history version); at gesture
end the intermediate roots coalesce into exactly ONE committed version (one undo
unit).** The "working head" is **CST state** (a green root), not a side-channel of
non-CST mutable state.

```cpp
class GestureBuffer {
    NodeRef                 baseRoot;        // the committed Version root at gesture start (the undo target)
    NodeRef                 uncommittedHead; // baseRoot + all staged patches applied, in memory (a green root, D2)
    std::vector<NodeRefId>  touched;         // union of changed NodeIds (D9; drives incremental re-derive)
    EditLabel               label;
public:
    void Stage(const CstPatch& p);           // uncommittedHead = ApplyPatch(uncommittedHead, p); union touched
                                             //   then derive a cheap, EPHEMERAL preview snapshot (debounced, O2)
    Version Commit();                        // coalesce: publish ONE Version{parent=base, changed=touched}
    void    Abort();                         // drop uncommittedHead; baseRoot stays the committed head
};
```

- **During the drag** (working-assumption O2 = debounced-commit): each
  `OnPointerMove` calls `Stage`, advancing only the **in-memory `uncommittedHead`**
  (a real green root produced by path-copy — it *is* CST state, not side state) and
  asking Facet 2 for a **cheap, debounced, ephemeral preview snapshot** — an
  incremental re-derive of `touched` (one object's transform, not a world rebuild —
  INV-3). The preview snapshot is **not** a history version and the **committed
  head does not move yet**, so undo history is untouched and the viewport reflects
  the uncommitted head's preview.
- **At gesture end** (`OnPointerUp` / `OnTimeScrubEnd` / slider release):
  `Commit()` **coalesces the intermediate uncommitted roots into exactly one
  `Version`** parented on `baseRoot`. That single committed version is the only
  history entry for the whole drag (one undo unit). **The composite-begin/end
  markers, `mCompositeDepth`, `ForceCompositeDepthZero`, the nesting-aware trim, and
  the composite atomic-rollback walks all evaporate** — a gesture is one version
  because we coalesce once, full stop.
- **`Abort()`** (gesture cancelled, view torn down, agent supersedes mid-drag) is
  trivially correct: drop `uncommittedHead` and its ephemeral preview; the committed
  head was never moved. This replaces `RollbackTransaction`'s entire
  cancel-park-revert-restore dance.

**The transaction == the gesture buffer.** "Begin transaction" = construct a
`GestureBuffer` from the current committed version. "Rollback" = `Abort()`.
"Commit" = `Commit()`. There is no baseline-snapshot to assemble (P-STATE gone),
no stack snapshot/restore, no `historyMarker` walk, no `ClearRedo` residue. The
agent surface (Facet 5) uses the *same* buffer for a multi-patch proposal: stage
all patches into the uncommitted head, derive an ephemeral preview snapshot, then
`Commit` (coalesce to one version) on accept or `Abort` on reject.

> **O2 delta if 60Hz incremental derivation were mandated instead:** nothing in
> the version model changes — `Stage` already advances the uncommitted head and
> incrementally derives `touched` into an ephemeral preview each move. The only
> difference is cadence/throttle policy (how often we ask Facet 2 to derive the
> preview snapshot and the viewport to repaint mid-gesture). The coalesce-to-one-
> version-at-end semantics are identical. So O2 is purely a derivation/scheduling
> knob, not an edit-model fork. This is a strong reason the model is O2-robust.

### 2.4 Unifying structured + text edits (L2) — the round-trip

The canonical truth is the CST. The text buffer is one *view*; the panels are
another. Both edit through `ApplyPatch`.

**Structured → text.** A `SetField` patch writes the new token(s) into the CST
leaf, preserving the node's surrounding trivia (comments, whitespace, the rest of
the line). Serializing the new CST (Facet 1's `serialize`) changes **only the
edited token's bytes** — **INV-4 (no gratuitous reformat)** holds *by
construction* because untouched nodes are the *same shared nodes* and serialize
to the same bytes. This is the property the SaveEngine's Mode-A byte-splicing was
hand-implementing; here it falls out of structural sharing for free.

**Text → CST (re-parse into a diff).** When the human or agent edits raw text
(in the source view, or an agent submitting a new file), we must turn text into a
patch without clobbering identity or reformatting siblings. The pathway:

```
OnTextBufferCommit(newText, editedByteRange):
    newCst   = Facet1::parse(newText)                 // full lossless re-parse (cheap; see latency)
    diff     = Facet1::DiffCst(currentVersion.root, newCst)   // structural diff → ChangeSet + per-node patches
    version  = PublishReparse(newCst, diff.changed)   // one Version; changed-set drives incremental derive
```

Two sub-strategies, chosen by Facet 1's diff capability:
- **Whole-buffer reparse + structural diff** (default, simplest): re-parse the
  whole file, structurally diff against the current root to recover the minimal
  `ChangeSet`, and publish the new root. **Identity is recovered by reusing prior
  NodeIds via structural matching** (D9: rust-analyzer-style node reuse — match new
  green nodes to prior NodeIds by structural position + content; unmatched nodes get
  fresh NodeIds). Because identity rides on NodeId (not on the name token), a raw
  text edit does **not** reset identities, so selection/agent references and UI
  bindings survive (INV-5, D9). The `ReparseSpan` patch kind is the *incremental*
  optimization: when the edit is confined to one chunk's byte span, reparse only
  that span into a subtree and `ReplaceSubtree` (NodeIds reused by the same
  structural match). Default to whole-buffer; add `ReparseSpan` if profiling
  demands it.

> **Conflict at the seam:** if the user edits the *same node* via a panel and via
> raw text "simultaneously," they are two patches against (possibly) two different
> base versions. This is handled exactly like any other concurrent edit: each
> client edits against a base version; on commit, if the base ≠ current head, the
> editor either fast-forwards (the patch's `target` NodeId's subtree is unchanged at
> head → re-base trivially) or surfaces a conflict (the target's subtree changed at
> head → reject with a structured error, Facet 5). Because edits are scoped to a
> `target` NodeId (D9), true conflicts are *node-local*, not whole-file. See §2.6.

### 2.5 Undo / redo / branch semantics

History is a **DAG of versions** (each a structurally-shared green root, D2), not
two stacks. Undo/redo is a **pointer move over green roots** (D2): no absolute
spans are reconstructed, no inverse is applied — the red cursor recomputes any
positions lazily for whatever root is published.

```cpp
class VersionGraph {
    std::unordered_map<VersionId, Version> versions;  // all live versions (ref-counted roots)
    VersionId head;                                   // the currently-published version
    // Per-head "redo" is the child you came from when you undid:
    std::unordered_map<VersionId, VersionId> lastChildOf;  // for linear redo UX
public:
    Version  Commit(NodeRef newRoot, ChangeSet, EditLabel, Origin);  // head→new; new.parent=head
    bool     Undo();   // head = versions[head].parent (if any)
    bool     Redo();   // head = lastChildOf[head] (the branch you last left)
    void     Trim(MemoryBudget);                                      // GC unreachable/old versions
};
```

- **Undo** = `head = parent`. **Redo** = `head = lastChildOf[head]`. Both are
  pointer moves over green roots (D2); the published root flips atomically; Facet 2
  re-derives the `ChangeSet` (NodeIds, D9) between old-head and new-head (the diff
  of two roots is cheap — shared green subtrees compare by pointer). **No inverse
  application, no `prev*` replay, no composite walk, no partial-revert atomicity
  logic, no absolute-offset recomputation.** An undo cannot "fail half-way" because
  it is a single pointer assignment.
- **Branching is free** (and is the agentic-native payoff). A new edit after an
  undo does **not** destroy the redo subtree by default — it adds a sibling child
  of the current head. We keep a linear-redo *UX* (`lastChildOf`) to match user
  expectation, but the graph retains branches, enabling: agent proposes edit B
  while the human's edit A sits on another branch; "try this variation" without
  losing the current one; A/B comparison of two derived scenes. This is
  impossible in Model A's two-deque history and is a differentiator, not an
  accident. Whether to *surface* branches in the UI is Facet 4's call; the model
  supports it either way.
- **Trim / memory bound.** Replaces `EditHistory::TrimToMax`'s 1024-cap + the
  nesting-aware composite-preservation logic. Because versions share structure,
  the cost of retaining history is "the sum of *distinct* subtrees across all
  retained versions," not N full copies. Trim policy: keep the last K versions
  reachable from head's ancestry + any branch tips younger than T; GC the rest;
  ref-counting reclaims subtrees no version references. A 60Hz drag is **one**
  version (it committed once), so the pathological "17s drag = 1000 history
  entries" case that drove the nesting-aware-trim complexity **cannot occur**.

### 2.6 Document vs session/view state (L4) — the split drawn explicitly

| State | Today (conflated) | Model B home | Persisted? |
|---|---|---|---|
| **Scene structure & all parameters** | live `Scene` + source bytes | **CST (the document) → derived Scene** | yes (the file) |
| Object transform / material / light / medium / camera params | live objects + `prev*` | CST leaves | yes |
| **Selection** (category, NodeId + addressing name, per-category memory) | `mSelectionCategory/Name`, `mSelectionByCategory[9]` | **`SessionState`** (binds to a NodeId, D9; name-path is how it's displayed/re-resolved) | no |
| **Section expanded** (panel accordion) | `mSectionExpanded[9]` | `SessionState` (or per-user UI prefs) | no |
| **Active tool + per-category memory** | `mTool`, `mLastSubToolPerCategory` | `SessionState` | no |
| **Gizmo drag-in-progress** | `mGizmoDrag` (~20 fields) | `GestureBuffer` (transient) | no |
| **Gesture bookkeeping** | `mGestureOpenedComposite`, `mScrubOpenedComposite`, `mPointerDown`, `mLastPx` | `GestureBuffer` lifecycle | no |
| **Orbit-preview camera** (interactive pose during a drag, distinct from the saved camera) | folded into the live camera | `SessionState` *view camera* (see note) | no |
| **Render progress / in-flight pass / preview-scale / polish state** | `mRendering`, `mCancelProgress`, `mPreviewScale`, `mPolishState`, … | **render orchestrator** (its own component, §2.9) | no |
| **`mSceneEpoch`** (UI re-pull trigger) | controller | derived from version id changes | no |
| **"Dirty / unsaved?"** (in-process) | `DirtyTracker` (2 channels) + `mScaleFromAnchorSet` + `HasUnsavedChanges` | **`head.id != lastFlushed.id`** (one comparison, see below) | n/a |
| **External-file conflict** (on-disk changed under us) | *(not detected today)* | **load/flush content fingerprint + CAS save (D6)** | the fingerprint travels with the head |

```cpp
// Pure ephemeral state — never serialized, never versioned, never captured for undo.
struct SessionState {
    Selection            selection;       // category + name + per-category memory
    Tool                 activeTool;
    ToolMemory           toolMemory;      // per-category last sub-tool
    PanelExpansion       expanded;        // accordion state (candidate for per-user prefs file)
    ViewCamera           previewCamera;   // interactive orbit pose (NOT the document camera)
    VersionId            viewingVersion;  // which version this view is bound to
};
```

**Two independent dirtiness concepts (D6), both required.** Model A had a single
in-process "dirty" notion because the live scene could diverge from the file, and
the `SaveEngine` had to reconcile. Model B replaces it with **two orthogonal
checks** — an in-process signal *and* an on-disk conflict guard — because the
in-process version id alone **cannot** detect that git or another editor rewrote
the file underneath us.

**(1) In-process "unsaved" signal — `head.id != lastFlushed.id`.** In Model B the
CST *is* the file's content, so:

- If we adopt **autosave / file-as-canonical** (O1 text-canonical leaning): every
  committed version is serialized to disk (debounced), so "unsaved" ≈ "the last
  commit hasn't flushed yet" — a transient I/O state, not a user-facing modal.
  The git-native framing (Facet 5) makes "save" ≈ nothing and "checkpoint" ≈ a
  commit the user names.
- If we keep an **explicit save** affordance (lossless-CST-canonical, in-memory
  head not yet flushed): "dirty" = `headVersion.id != lastFlushedVersionId` — a
  single integer comparison, **not** a 2-channel tracker + a 5th set + transition
  listener. The entire `DirtyTracker`/`mScaleFromAnchorSet`/
  `FireDirtyChangedIfTransitioned` apparatus collapses to that comparison.

This signal is **in-process only** (§438 of the legacy model): it answers "have I
made changes since my last flush," nothing about the bytes on disk.

**(2) On-disk external-conflict guard — load/flush fingerprint + compare-and-swap
save (D6).** A *second, independent* concept is required to stop autosave (or an
explicit save) from silently clobbering an external change:

- Record the file's **content fingerprint** (size+mtime, upgradable to a content
  hash — the AssetManifest fingerprint of D5) **at load and at each flush**. The
  fingerprint travels with the head.
- Before writing, **compare-and-swap**: if the on-disk fingerprint ≠ the
  last-known fingerprint, the file changed externally → **do not silently
  overwrite.** Surface a conflict with explicit choices — **reload** (discard the
  in-process head), **diff/merge**, or **force-overwrite** — never a silent
  overwrite. On a successful flush, update the stored fingerprint.
- A background watcher (D5's file-watch mechanism) can surface "file changed on
  disk" proactively, so the user learns of the conflict before they try to save.

```cpp
// The two signals are computed independently; neither subsumes the other.
bool   hasUnsavedChanges = head.id != lastFlushedVersionId;          // (1) in-process
enum class SaveOutcome { Wrote, Conflict_External };                  // (2) on-disk CAS
SaveOutcome Flush(NodeRef head, Fingerprint lastKnownOnDisk) {
    if (StatFingerprint(path) != lastKnownOnDisk)                     // CAS check (D6)
        return SaveOutcome::Conflict_External;                        // → reload / diff / overwrite
    WriteText(serialize(head, /*via red cursor*/));                   // INV-4 round-trip (§2.4)
    lastKnownOnDisk = StatFingerprint(path);                          // update fingerprint
    return SaveOutcome::Wrote;
}
```

Either way the **P-STATE class is gone**: there is no hand-assembled "what's
dirty" set to keep complete — the in-process signal is two version ids and the
external-conflict guard is two fingerprints. (The legacy `FileIdentity`
external-mod guard's *intent* is retained here even though its byte-splice
*mechanism* is deleted — D6 / §3.1.)

> **Why selection is *not* in the CST:** selection binds to a node's immutable
> **NodeId** (D9, INV-5), displayed/re-resolved via its name-path. Because it keys
> on NodeId, the binding survives a rename and a text-edit reparse (NodeIds are
> reused by structural matching, D9). When a version changes, `viewingVersion`
> advances and selection is re-validated against the new root (the
> `DropStaleSelection_` check survives, but as a pure function
> `resolves(selection, version.root)` with no mutation entanglement). Putting
> selection in the document would violate INV-1 (it'd be a second mutable thing to
> keep consistent and would pollute diffs/round-trip).

### 2.7 How this provably dissolves the 4 root patterns

This is the load-bearing argument (memory `project_editor_state_hardening`).

- **P-STATE (incomplete captured baseline) — DISSOLVED, not mitigated.** There is
  **no captured baseline**. Undo restores a prior *complete* version; you cannot
  "forget to capture field X" because nothing is captured — the old root *is* X
  and everything else. `SceneEdit::prev*` (12+ fields), `EditorStateSnapshot`,
  `DirtySnapshot`, the "5th set" — all deleted. The thing the bug census kept
  re-finding ("another uncaptured field") is **unrepresentable**.
- **P-WALK (duplicated edit walks) — DISSOLVED.** There is **one** function that
  changes a CST (`ApplyPatch`) and **one** that changes the head (a pointer
  assignment). Undo/redo do not re-dispatch per-op logic at all — they don't
  touch ops, they move a pointer. The 5-walks→2-dispatchers H2 work is moot
  because the dispatchers themselves disappear: forward application is the only
  direction; "revert" is "publish the parent." Composite walks are gone (§2.3).
- **P-INVALIDATE (scattered light-gen bumps) — DISSOLVED at this layer.**
  Invalidation is **derivation's** problem now (Facet 2), keyed off the
  `ChangeSet` (NodeIds, D9): when a node under `lights/` or an emissive
  `materials/` slot or an emissive object's transform changes, the *derivation*
  recomputes the dependent light samplers because the **dependency edge it traced
  during the previous derive** (D4) says so — deps are recorded as a by-product of
  derivation, not pre-declared. The editor does **not** bump anything — it just
  reports *what NodeIds changed*. The traced dependency replaces a dozen open-coded
  `Bump*` calls. (Cross-facet ask on Facet 2: trace the light-sampler/env-IBL
  dependency on the emitter-set; see §5.)
- **P-FFMATH (NaN sentinels) — UNCHANGED but shrunk.** The model removes most of
  the code that used NaN-as-not-found (the reverse-lookups, the `prev*` "absent"
  encodings). The `SourceHygieneTest` + `red_prove.sh` guardrails stay. New CST
  code should use `std::optional` for absence, never NaN.

### 2.8 Memory & performance of versioning a large CST

- **Per-edit cost:** O(depth of edited path) green-node copies (D2 path-copy); the
  copied nodes carry forward the prior NodeIds (D9), siblings are shared by
  reference. A scene CST is shallow (file → chunks → params); editing one parameter
  copies ~3–5 green nodes (root → chunk → param). Sub-microsecond; allocations
  dominated by one small node. **No offset rewrite of later nodes** (D2: relative
  widths, positions via the red cursor) — that is what keeps this O(depth) rather
  than O(document).
- **Per-edit memory:** the copied spine only. Untouched chunks (the other 154
  objects in a Sponza scene) are pointer-shared. A drag commits **one** version,
  so 5s @ 60Hz = 1 retained version's worth of distinct nodes, not 300.
- **History memory:** Σ distinct subtrees across retained versions. Editing 50
  different parameters across 50 versions retains ~50 small spines + one shared
  bulk — kilobytes, not 50× the file.
- **Snapshot/branch cost:** O(1) (copy a `shared_ptr`).
- **Persistence (on disk / branch tips):** serialize head to text by walking the
  green tree with the red cursor (D2) into **one self-contained `.RISEscene` file**
  (D7: a v7 document is single-file; `> load`/`> run` are deprecated, so there is
  no forest to serialize). For multi-version branch persistence (if surfaced),
  store the **text-diff between adjacent versions** — this is where the
  rejected-as-primary text-diff model earns its keep: compact at rest, and
  reconstituted into the persistent tree on load by parse+apply. So the canonical
  pivot is "persistent red-green tree in memory, single-file text (± diffs) at
  rest." Each flush runs the D6 compare-and-swap (§2.6) before writing.
- **Latency budget (first-class output, INV-3; ties to D10 G2).** D10's shared
  gate **G2** sets the *ceiling*: a single-parameter edit re-derives in **< 50 ms**
  on a Sponza-class scene. This facet sets tighter internal targets *within* that
  ceiling: ≤ **2 ms** for a single-parameter structured edit end-to-end (ApplyPatch
  on green nodes + incremental derive of one object + repaint kick), and ≤ **20 ms**
  for a whole-buffer reparse on a ~5k-line scene (parse is linear; Facet 1's number,
  flagged as a dependency). G2 bounds *derivation*; the 2 ms / 20 ms numbers are the
  two to measure first (§6).

### 2.9 Concurrency: UI thread vs render thread vs agent edits

**No parking-for-safety (D1); cancel-and-park survives only as an optional
latency optimization.** Immutability removes the correctness need to park.

The render orchestrator (the `Start/Stop/RenderLoop/DoOneRenderPass/KickRender`
machinery in today's controller) is **kept and extracted into its own
component** — it is *not* an edit-model concern and was only conflated into
`SceneEditController` for proximity. The edit model interacts with it through one
rule:

> **A render pass renders a specific immutable `DerivedScene` snapshot. Publishing
> a new version derives a new snapshot; the renderer atomically swaps its snapshot
> pointer at a tile/pass boundary; the old snapshot stays alive (refcount) until
> the in-flight pass drains. Publishing never mutates the snapshot the render thread
> is reading.**

This is the D1 model, and the big simplification immutability buys. In Model A, an
edit *mutates the live scene the worker is mid-`IntersectRay` on* → the whole
cancel-park-mutate-under-lock dance (and the time-scrub UAF the comments describe)
exists to prevent a use-after-free. In Model B:

- **The `DerivedScene` is an immutable COW snapshot with structural sharing (D1).**
  A new version does **not** rebuild the whole scene: only the **changed subgraph's
  engine objects are re-derived** (Facet 2 reuses the apply-layer on *copies*),
  while unchanged objects (meshes, BVH leaves, materials, the TLAS where untouched)
  are **shared by reference** with the prior snapshot. This is the red-green
  discipline (D2) extended from the CST to the derived scene.
- The render thread holds a **refcounted pointer to one snapshot**
  (`shared_ptr<const DerivedScene>`). A commit (or a gesture preview, §2.3) derives
  a new snapshot; the render loop **atomically swaps the pointer at a tile/pass
  boundary**. The old snapshot stays alive until the in-flight pass finishes. **No
  UAF is possible** — you cannot free what a worker holds a ref to, and **there is
  no parking-for-safety** (no thread reads an object another thread mutates —
  snapshots are immutable).
- **Cancel-and-park survives only as an optional latency optimization, never a
  correctness requirement (D1).** When a new version is published, we *may* still
  want to stop the now-stale pass promptly so the user sees their edit sooner (and
  to coalesce rapid edits). So `KickRender` may trip the cancel flag and the loop
  picks up the new snapshot. But we no longer need to *block the editing thread*
  until the pass drains — the edit already succeeded on an independent immutable
  structure. The `mMutex`-held-across-`Apply` pattern (today required for
  time-scrub) is **dropped**; the only shared state is "which snapshot should the
  loop render next," an atomic `shared_ptr` swap at a tile/pass boundary.
- **Abort needs no rollback (D1):** dropping an in-flight pass is free — nothing
  was half-mutated.
- **Last-good-scene (D1):** trivially the most recent snapshot that derived without
  a hard error; immutable + refcounted, so the renderer simply keeps rendering it
  while a broken head is edited (pairs with derive-with-holes, Facet 2).
- **Agent edits vs UI edits** are the same: both call `Commit` on the version
  graph (UI-thread-affine for the graph itself, or a small mutex around the graph
  — the graph op is microseconds). An agent committing while the user is
  mid-gesture is the §2.6 concurrent-edit case: the agent's commit advances head;
  the user's `GestureBuffer` is based on the *old* root; on the user's
  `Commit`, re-base node-locally or surface a conflict. The gesture's preview
  keeps showing its own uncommitted head's snapshot until commit, so the agent's
  change doesn't yank the viewport mid-drag.
- **Thread affinity:** the version graph and `SessionState` are UI-thread-owned
  (or guarded by a short mutex for agent/MCP threads). Derivation (Facet 2) may
  run off-thread and publish a `DerivedScene` snapshot when ready. The render
  thread only ever reads immutable snapshots and swaps at a tile/pass boundary.
  This is a clean three-party contract with one shared mutable cell (the head
  snapshot pointer) instead of today's web of atomics + condvar + per-mutator
  park.

---

## 3. Delete / Evolve / Reuse — the deletion inventory

Precise fate of every component this facet supersedes (charter §7.3, L7).

### 3.1 DELETE (removed entirely; the concept ceases to exist)

| File / class / member | What it did | Why it dies |
|---|---|---|
| `SceneEdit.h` — entire `struct SceneEdit` (24 ops + all `prev*` fields + `CameraSnapshot`) | tagged-union inverse-edit record | replaced by `CstPatch` (forward-only, no prev capture). The whole *concept* of a per-op inverse record is gone. |
| `SceneEdit::CompositeBegin/End`, `IsCompositeMarker`, `mCompositeDepth` | bracket a gesture into one undo unit | a gesture is one version because `GestureBuffer` commits once (§2.3) |
| `SceneEdit::historySeq`, `capturedTargetSerial`, `ResolveTargetSerial`, the identity-serial guard | detect remove+re-add under a reused name | identity is the immutable **NodeId** (D9), not the name; undo restores a whole consistent version, so "the instance under the name changed" cannot corrupt anything — there's no captured state being mis-applied. (The serial's *intent* — survive a renamed entity — is subsumed by NodeId-keyed identity.) |
| `EditHistory.{h,cpp}` — **entire class** (2 deques, `Push`, `PopForUndo/Redo`, `TrimToMax` nesting logic, `RestoreLast*`, `Snapshot/Restore*ForRollback`, `ClearRollbackSnapshots`, `NextSeq`/`PeekUndoSeq`/`MaxTrimmedSeq`/`DidTrim`, `mDirtyObjects`) | bounded undo/redo stacks + transaction stack snapshots | replaced by `VersionGraph` (DAG of immutable roots). Every stack-snapshot/restore/trim-nesting subtlety is moot. |
| `SceneEditor.cpp` — `Apply`/`Undo`/`Redo`, `CaptureForApply` (~30 gates), `ApplyForwardMutation`, `ApplyRevertMutation`, composite atomic-rollback walks, `ApplyObjectOpForward`, `RestoreObjectTransform`, `ApplyMaterialSlotByName`, `AggregateCompositeScope` | the mutate-live-scene engine + inverse replay | replaced by `ApplyPatch` (pure `Cst→Cst`, Facet 1+3) + derivation (Facet 2). Forward-only; no revert dispatcher. |
| `SceneEditor` — `Bump*` family (`BumpSceneLightGeneration{,IfEmitterSetChanged,IfMaterialEmits}`) + the `dynamic_cast<Implementation::Scene*>` light-gen plumbing | scattered light-topology invalidation (P-INVALIDATE) | invalidation becomes Facet 2's dependency-graph job, keyed off the `ChangeSet` (§2.7) |
| `SceneEditor` — `prev*` capture, `FindManagerName`/`FindMediumName`/`FindGeometryName` reverse-lookups, the borrowed manager pointers (`mMaterialManager`/`mShaderManager`/`mPainterManager`/`mScalarPainterManager`/`mJob`) used *for capture* | recover prior names for undo | no prior-state capture exists; patches reference a node by its **NodeId** (D9), resolved from a name-path at the API edge |
| `SceneEditor::DirtyScope` enum + `LastDirtyScope` + `MarkEditEntityDirty` per-op routing | tell the orchestrator how much to re-render | replaced by the `ChangeSet` (which nodes changed) — strictly more precise; the orchestrator/derivation decide cost from *what changed*, not a coarse 5-value enum |
| `SceneEditController` — `BeginTransaction`/`RollbackTransaction`/`EndTransaction`, `mTxnOpen`, `mTxnBaseline`, `CaptureEditorState`/`RestoreEditorState`, `EditorStateSnapshot` | transaction begin/rollback/commit + baseline snapshot | replaced by `GestureBuffer` lifecycle (`Stage`/`Commit`/`Abort`). The baseline-snapshot (P-STATE epicenter) is gone. |
| `SceneEditController` — `mGestureOpenedComposite`, `mScrubOpenedComposite`, `OnPointerDown`'s composite-open + the orphan-composite guards, `ForceCompositeDepthZero`, `IsCompositeOpen` | reconcile gesture lifecycle with composite markers | no composites; gesture lifecycle is `GestureBuffer` construct/commit/abort |
| `DirtyTracker.{h,cpp}` (the *interactive-editor* role), `mScaleFromAnchorSet`, `HasUnsavedChanges`, `FireDirtyChangedIfTransitioned`, `DirtyChangedFn` | "what's dirty since load/save" for the save engine + Save-button | the **in-process** signal becomes `head.id != lastFlushed.id` (§2.6, D6 concept 1). **Retained-in-spirit:** the *external*-change detection is **not** dropped — it re-emerges as the D6 load/flush fingerprint + CAS save (§2.6, concept 2). **Caveat:** `DirtyTracker`/`SourceSpanIndex`/`OverrideSpanIndex`/`TransformSnapshot`/`SaveEngine` are co-owned with Facet 6 (migration) — they only fully die once Model B's CST round-trip + CAS save replaces Model-A save. This facet *marks them dead*; Facet 6 sequences the removal. |
| `SaveEngine.{h,cpp}` Mode-A byte-splice + Mode-B `override_object` block (the "SaveEngine gap" L7) | preserve typed text while writing model deltas | INV-4 round-trip is now automatic (§2.4): serialize the green CST via the red cursor (D2), untouched nodes are byte-identical. The whole splice/override-block *mechanism* is obviated. **The `FileIdentity` external-mod guard's *intent* is retained** as the D6 compare-and-swap save (§2.6) — only its byte-splice mechanism dies. (Facet 6 owns the deletion timeline + scene-corpus migration.) |
| `tests/SceneEditTransactionTest.cpp`, `EditHistory`-specific tests, `SceneSnapshotTest`, `SamplerRebuildOnRestoreTest` (the snapshot/restore-rebuild path) | guard the deleted machinery | their invariants move to version-graph tests (§6); the *behaviors* (undo restores state, light samplers rebuild on relevant change) are re-tested against the new model |

### 3.2 EVOLVE (kept in spirit, reshaped)

| Component | Today | Becomes |
|---|---|---|
| `SceneEditController` pointer-event handlers (`OnPointerDown/Move/Up`, `OnTimeScrub*`, `BeginPropertyScrub`/`EndPropertyScrub`) | translate gestures → live mutations + composite brackets | translate gestures → `CstPatch`es staged into a `GestureBuffer` (each move advances the uncommitted head + an ephemeral preview snapshot; coalesce into one committed version at gesture end, D1/§2.3). The **gizmo pixel→world math** (`ProjectWorldToScreen_`, axis-velocity solves, `BuildGizmoHandles_`, `GizmoHandleAt`) is **reused verbatim** — it's a view-projection, model-agnostic. |
| `DropStaleSelection_` / `SelectionStillResolves` | re-validate selection after undo/redo (mutating controller members) | pure `resolves(selection, version.root)`; selection lives in `SessionState`; re-run on every head change |
| `ResolveTargetSerial`'s *intent* (don't corrupt a renamed entity) | serial compare | subsumed by **NodeId** identity on immutable trees (D9; rename is a NodeId-preserving op, INV-5) |
| `EditLabel`/`OpName` (the "Undo Move sphere" tooltip strings) | `EditHistory::LabelForUndo`, `OpName` switch | `Version::label` set at commit; tooltip reads `versions[head].label` / `versions[lastChildOf].label` |
| The cancel-and-park orchestrator (`Start/Stop/RenderLoop/DoOneRenderPass/KickRender`, preview-scale, polish) | conflated into `SceneEditController`, parks to avoid UAF | **extracted into its own render-orchestrator component**; parks for *latency* only; renders an immutable `Version`'s `DerivedScene` (§2.9). Out of this facet's deletion scope but its coupling to edits is redefined here. |

### 3.3 REUSE (unchanged, depended upon)

| Component | Role in Model B |
|---|---|
| `ChunkDescriptorRegistry` + `IAsciiChunkParser::Describe()` (L6) | the schema for patch validation, CST node shape, and the dynamic UI. Reused as-is. |
| `*Introspection` (`Material`/`Media`/`Camera`/`Light`/`Object`/`Film`/`Rasterizer`) | today they read live objects for the panels; in Model B the panels read CST leaves directly via descriptors, so most introspection **shrinks**. Retained where it computes *derived* read-only display values (e.g. a camera's post-orbit eye) — those read the *derived scene*, not the CST. (Boundary owned with Facet 4.) |
| `CancellableProgressCallback` | the render orchestrator's cancel primitive — unchanged |
| Facet 1's `parse`/`serialize` (red-cursor walk), `DiffCst` (NodeId-reuse via structural matching), `ApplyPatch`-core, the green `NodeRef`, the immutable `NodeId` (D9 lineage), and `NodePath` (D9 addressing) | the red-green substrate (D2) this whole facet is built on |

---

## 4. Hard problems & open questions

1. **Re-parse identity stability — RESOLVED by D9.** Text→CST relies on `DiffCst`
   reusing prior **NodeIds** by structural matching (position + content), so
   identity rides on the immutable NodeId, *not* on the name token. Consequently a
   raw-text **rename** (`objects/sphere` → `objects/ball`) is matched as the *same*
   node (same structural position + content shape, only the name token differs) and
   keeps its NodeId — selection and durable agent references keyed on NodeId
   survive. **Rename from a panel/agent is the first-class `Kind::Rename` patch**
   (§2.2): NodeId-preserving, rewrites the name token + all referrers via the
   `referenceCategories` hint (D4). The residual cross-facet asks are narrow: Facet
   1 must guarantee the structural matcher treats a name-token-only change as a
   match (not remove+add), and Facet 5 defines agent rename semantics on top of
   `Kind::Rename`. The old "(a) drop the selection / (b) heuristic follow / (c)
   best-effort" trichotomy is obsolete — D9 makes NodeId-preserving the *defined*
   behavior, not a heuristic.
2. **Whole-buffer reparse latency on huge scenes.** A 50k-line generated scene
   re-parsed on every text commit could blow the 20 ms budget. Mitigation:
   `ReparseSpan` (chunk-local reparse) — but that needs Facet 1 to map a byte
   range → the enclosing chunk node reliably. Open until Facet 1's parser
   incrementality is specced. **This is the second of the charter's two named
   tar-pits (derivation latency / lossless preservation) viewed from the edit
   side.**
3. **Derivation granularity vs `ChangeSet` precision.** My model's efficiency
   assumes Facet 2 can re-derive *just* the changed nodes. If derivation has
   coarse dependencies (e.g. any light change rebuilds all light samplers — which
   may be *correct*, see the env-IBL/`LightSampler` coupling in the engine), the
   per-edit latency is bounded by that, not by my O(depth) patch cost. The
   editor's job ends at "here's the precise `ChangeSet`"; how finely Facet 2 acts
   on it is **the** cross-facet performance contract.
4. **Branch UX vs simplicity.** The graph *supports* branches for free; exposing
   them risks user confusion. Working assumption: linear undo/redo UX over a
   graph that quietly retains branches for agent A/B and "don't lose my work after
   undo+edit." Whether/how to surface is Facet 4. Decision needed: default GC
   policy for abandoned branches (time-based? depth-based?).
5. **Floating-point round-trip of structured edits.** A gizmo drag formats a
   position as text (`%g`) into the CST. Re-deriving parses it back. If a user
   round-trips many drags, does `%g` precision drift accumulate? In Model A this
   was the `prevTransform` matrix (binary-exact). Recommend: format with enough
   precision (`%.17g` for doubles) so parse∘format is identity; or store the
   high-precision token and let the *display* round. **Largely resolved by D2:** a
   green node stores the **verbatim author token** as its typed content (the
   red-green tree is lossless, INV-4), so a hand-typed `1.5` stays `1.5` and a gizmo
   write is whatever token it formatted — there is no re-quantization on undo/redo
   (a pointer move republishes the exact prior green node). The residual is only the
   *formatting* choice at write time (`%.17g`), not a storage question.
6. **Where exactly is `ApplyPatch` owned — Facet 1 or Facet 3?** It straddles:
   the *tree mutation mechanics* are Facet 1's (it knows node internals); the
   *patch taxonomy + validation-against-schema + versioning* are mine. Proposed
   split: Facet 1 exposes a low-level `withChild`/`withField` persistent-update
   primitive; Facet 3 builds `ApplyPatch` (taxonomy + validate + `ChangeSet`) on
   top. Must be reconciled in synthesis.
7. **Multi-document / include files — RESOLVED by D7 (tree, single root; the
   open question is deleted).** A v7 document is a **single self-contained file**:
   `> load` and `> run` are deprecated and removed, and the one-shot migrator
   **flattens** every include/run by inlining the referenced content. So the CST is
   **one tree with a single root**, *never* a forest. This makes version identity
   per-single-root, undo single-rooted, and save one file (the D6 CAS). The earlier
   "one tree vs forest? version = set of roots?" question no longer exists. (If
   library-sharing demand returns, D7 routes it through a future declarative
   `import` chunk with its own scoping/fingerprint/versioning story — explicitly
   out of core v7, never the imperative `> load`.)

---

## 5. Cross-facet dependencies & assumptions

- **On Facet 1 (CST):** I assume the **red-green model (D2)**: (a) an immutable,
  ref-counted/persistent **green** `NodeRef` tree whose nodes store **relative
  width** + a stable **NodeId** (D9), **never absolute spans**; (b) **dual identity
  (D9)** — `NodeId` is the lineage identity (stable across edits, renames, and
  reparses); `NodePath` is version-resolved addressing; (c) `parse`/`serialize`
  (the latter a **red-cursor** walk) with **byte-exact round-trip of untouched
  nodes** (the load-bearing INV-4 guarantee); (d) a `DiffCst(a,b) → ChangeSet` (of
  NodeIds) that compares shared subtrees by pointer and **reuses prior NodeIds via
  structural matching** (so a text edit / rename does not reset identity, D9); (e) a
  low-level persistent-update primitive for `ApplyPatch` to build on; (f) green
  leaves store the **verbatim author token** (resolves §4.5). Facet 1 already owns
  the D7 single-file decision (no forest) and supplies the load/flush fingerprint
  surface D6 needs. If Facet 1 chose a mutable CST with a separate snapshot
  mechanism, my version model would degrade to "snapshot per commit" — but D2 makes
  the red-green tree the ratified substrate, so this is settled, not open.
- **On Facet 2 (derivation):** I hand it `(newRoot, ChangeSet)` (NodeIds) and
  expect an incremental `derive` that recomputes only dependent subgraphs (INV-3),
  and a **dependency model that owns light-topology/env-IBL invalidation**
  (absorbing P-INVALIDATE). Per **D1**, `DerivedScene` is an **immutable COW
  snapshot with structural sharing** — only the changed subgraph's engine objects
  are re-derived; unchanged objects are shared by reference with the prior snapshot;
  the render thread holds a refcounted pointer and swaps at a tile/pass boundary
  (§2.9). Conflict risk: if derivation is whole-world per edit, the latency budget
  (§2.8, ≤ D10 G2's 50 ms ceiling) is Facet 2's to meet, not mine.
- **On Facet 4 (UI):** structured edits arrive as `CstPatch` through one API;
  `SessionState` (selection/tool/expansion) is owned jointly (it's UI state but I
  define the split). UI widget bindings and selection key on **NodeId** (D9),
  addressed by name-path. The gizmo math is shared code.
- **On Facet 5 (agent):** the MCP server is a `GestureBuffer`/`Commit` client and
  a `VersionGraph` reader; structured errors come from `ApplyPatch`'s
  validation + the §2.6 conflict result. Durable agent references key on **NodeId**
  (D9). "GUI is just another agent" holds because both go through `ApplyPatch`.
- **Decision conformance (D1–D10).** This facet implements **D1** (immutable COW
  derived-scene snapshot + the single gesture model, §2.1/§2.3/§2.9), **D2**
  (red-green CST, §2.1/§2.8), **D6** (external-file conflict: fingerprint + CAS
  save, §2.6), **D7** (single-file tree, not a forest, §2.8 + §4 item 7), and **D9** (dual
  identity, throughout). It **consumes** the others without conflict: **D3**
  (ordered children) — patches address ordered-child NodeIds; **D4** (traced deps +
  the `referenceCategories` rename hint) — referenced by `Kind::Rename` and
  P-INVALIDATE; **D5** (`AssetManifest` fingerprint) — reused as the D6 save
  fingerprint; **D8** (time-bounded v6) — this facet ships only the v7 path; **D10**
  (phased fixture + shared gates) — §6 maps to G1–G5. **No contradiction with any
  of D1–D10.**
- **Locked-decision conformance:** L2 (one pathway = `ApplyPatch`) ✔; L4
  (doc/session split, §2.6) ✔; L5 (identity first-class — now **NodeId** lineage +
  name-path addressing per D9, replacing the serial) ✔; L7 (supersession
  inventory, §3) ✔. **No conflict with any Locked decision.**
- **Open-decision stances:** O1 — designed for lossless-CST-pivot; noted the
  text-canonical delta (the in-process dirty signal dissolves to one comparison;
  the D6 external-conflict guard remains regardless) in §2.6 / §2.8. O2 — designed
  for debounced-commit; showed the 60Hz delta is a pure scheduling knob (§2.3), the
  strongest possible robustness result for that decision.

---

## 6. First-slice implications (minimal end-to-end vertical)

This facet plugs into **D10's single phased fixture and shared gate set** — it does
**not** nominate its own first slice. D10 phase 1 is `sphere_geometry` (2 scalar
params); a **scalar `SetField`** on it is the smallest end-to-end edit. The
`standard_object` three-node chain (cross-node refs, **rename** integrity via D9,
the dependency graph) arrives at D10 phase 3, where this facet's NodeId-keyed
identity and `Kind::Rename` are exercised. Below, each contribution is tagged with
the D10 gate it satisfies.

1. **Version graph + one commit.** `VersionGraph` holding immutable green roots
   (D2); `v0` = parse of the phase-1 `sphere_geometry` scene. `Undo`/`Redo` = head
   pointer moves over green roots. *(This facet's irreducible core — buildable
   against a stub `NodeRef`.)* **→ D10 G4 (versioning).**
2. **One patch kind end-to-end.** `CstPatch{SetField, radiusNodeId, text}` (NodeId
   resolved from the name-path `geometry/dial_sphere.radius`, D9) → `ApplyPatch` →
   `v1` (shares everything but the edited spine) → Facet 2 incrementally derives
   the one object → render kick. **→ D10 G3 (minimal invalidation) + G2 (latency).**
3. **One structured widget.** Facet 4's scalar field reads `v0`'s green leaf,
   writes a `SetField` patch on edit. Verify the panel and the (eventual) source
   view show the same value because both project the same node.
4. **Round-trip proof (INV-4 / D10 G1).** `serialize(v1)` (red-cursor walk, D2)
   differs from `serialize(v0)` **only** in the edited token's bytes — assert
   byte-identity of every other line on a hand-formatted fixture (tabs, trailing
   comment, blank line). This is the single most important first-slice test; it
   proves the SaveEngine is unnecessary. **→ D10 G1 (round-trip identity).**
5. **Gesture-as-one-version.** A scripted 10-step "drag" (10 `Stage` calls
   advancing the uncommitted head + one `Commit` that coalesces, §2.3) yields
   **one** version; `Undo` returns to `v0` in one step; round-trip after the undo is
   byte-identical to pre-edit. Proves the composite machinery is unneeded.
   **→ D10 G4 (a gesture is one undo unit; undo round-trips to G1).**
6. **External inputs / save (phase ≥2).** When the fixture gains
   `uniformcolor_painter` and (later) an asset reference, an asset-fingerprint
   change re-derives its consumers (D5) and an external file change is caught by the
   D6 CAS save before any overwrite. **→ D10 G5 (external inputs).**
7. **Two measured latencies** (§2.8, within D10 G2's 50 ms ceiling): structured
   `SetField` end-to-end (target ≤ 2 ms) and whole-buffer reparse+diff of the tiny
   scene (sanity for the 20 ms budget at scale). These two numbers are the facet's
   first-class perf output.

The first slice deliberately exercises **structured edit, text round-trip, undo,
and gesture-coalescing** on one chunk — every core claim of this facet, against
D10's shared gates, with the SaveEngine/EditHistory/transaction code provably
absent.
