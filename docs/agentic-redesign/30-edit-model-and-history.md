# Facet 3 — Edit Model, History & State

> **Updated per [`01-DECISIONS.md`](01-DECISIONS.md) (rounds 1–6, D1–D51).** Sections
> below conform to the ratified decisions; where this doc and a decision differ,
> [`01-DECISIONS.md`](01-DECISIONS.md) wins (a later round amends an earlier one).
> Decisions this facet implements:
> **D1** (immutable COW derived-scene snapshot + the single gesture model) as
> **amended by D11** (COW = reverse-dependency-closure copy), **D12** (build →
> phase-B → seal → publish; the sealed snapshot owns its render structures;
> adopt at a **pass** boundary), and **D21/D22** (the snapshot is layered:
> config-**independent** `DerivedScene` vs `PreparedRenderState =
> prepare(DerivedScene, RenderConfig)` — the config input is sharpened to the resolved
> **`EffectiveRenderConfig`** by D42, below; animation is per-frame derivation with time
> `t` as an input; render-populated caches — irradiance cache, accumulation — are
> render-local mutable, not snapshot state),
> **D2** (red-green CST: relative widths + red cursor, no absolute spans) as
> **amended by D15** (NodeId lives in the **red layer / side-map**, not the shared
> green node; content hash / derivation key / lineage identity are three distinct
> things), **D16** (wide child sequences are persistent ropes → O(log N)), and
> **D23** (the version DAG, the identity side-map, and the derivation cache rely on
> **persistent immutable containers** for O(log N) sharing — a named prerequisite;
> the O(log N)/O(closure) claims are **gated** on that work, with a copy-on-snapshot
> O(N) v1 fallback),
> **D6/D17** (external-file conflict: prefilter + content-hash fingerprint, and an
> atomic temp-write → fsync → revalidate → rename save, not stat-then-write) as
> **amended by D28** (history preserves the CST only; re-deriving an old version
> uses **current** asset bytes — an external asset change may alter the render; a
> content-addressed asset store is a future option),
> **D7** (v7 is a single-file *tree*, not a forest),
> **D9** (dual identity: immutable `NodeId` for lineage, name-path for addressing)
> as **amended by D14** (rename rewrites referrers from traced `ReferenceUse`
> records, not `referenceCategories`), D15, and **D25** (rename requires a
> **head-stamped** `ReferenceUse` trace) as **amended by D35** (rename reuses the
> **one** derivation resolver — derive head, sharing that code, not a separate
> tracing-pass reimplementation; refuse if head can't be derived) and **D39** (that
> "derive head" is the **bounded synchronous semantic phase** — parse→CST→
> reference-resolution→typecheck, edit-thread-OK — *not* the async expensive phase),
> **D13** (coherent version status exposing **headVersion** *and* the derived stamp)
> as **amended by D29** (the derived side is a full **`DerivedStamp`**, and staleness
> is **cstVersion DAG ancestry**, not numeric `<`) and **D38** (the status carries both
> the **requested** and **published** stamps; **`ok` ⟺ full-stamp equality**),
> and **D26** (every `Version` owns a persistent identity side-map, where the
> `identityRoot` is the per-Version occurrence→NodeId map that reparse-matching
> writes and that undo/UI bindings resolve against) as **amended by D30** (a
> `Version` is `{ greenRoot, identityRoot, metadata }` — CST + occurrence identity
> ONLY; the derivation cache is **NOT** on the immutable Version, it lives on a
> stamp-keyed `DerivedArtifact`).
> From **round 4** it further implements:
> **D29** (`DerivedStamp = { cstVersion, assetManifestGen, animationName,
> shutterInterval }` identifies a `DerivedScene`, `PreparedStamp = DerivedStamp +
> { renderConfig, cameraOverride, samplingSeed }` a `PreparedRenderState` — round 5
> sharpens these axes: `assetManifestGen` → **`assetDigest`** (D41) and `renderConfig`/
> `cameraOverride` → **`effectiveRenderConfigHash`** + **`viewCameraStateHash`** (D42);
> the head-vs-derived staleness check is **cstVersion DAG ancestry, not numeric `<`**),
> **D30** (the derivation cache lives on a `DerivedArtifact` keyed by `DerivedStamp`
> — and a `PreparedArtifact` keyed by `PreparedStamp` — in a stamp-keyed LRU, **not**
> on the immutable Version, which commits before async derivation and spawns many
> caches),
> **D31** (animation is per-frame, time-**INTERVAL** derivation — a motion-blurred
> frame's `DerivedScene` bakes animated quantities as immutable functions over the
> shutter `[t0,t1]` and the renderer reads `at(τ)` per sample; v1 supports
> single-time, motion blur is gated),
> **D34** (the edit thread only commits a CST `Version`; the **expensive** derivation
> phase — realize/TLAS/prepare/render — runs **asynchronously + cancellably on the
> render arbiter**, off the edit thread — this is the head-vs-derived lag source; **D39
> carves out a bounded synchronous semantic phase that DOES run on the edit thread**),
> **D35** (rename obtains head's reference set via **the same evaluator/resolver as
> derivation** — derive head, sharing that code, **not** a separate tracing-pass
> reimplementation; refuse if head can't be derived; per **D39** "derive head" = run the
> bounded sync semantic phase to head), and
> **D36** (the staged-edit state — `GestureBuffer`, working head — carries
> **`{ greenRoot, identityRoot }`** (BOTH roots) so insertions/reparses during a
> gesture update occurrence identity as they go; the `CstPatch`/`EditIntent`,
> `Widget`/`ViewNode`, and selection carry the target **`NodeId`**, not just a
> name-path).
> From **round 5** it further implements:
> **D38** (`VersionStatus` carries **requested AND published** stamps —
> `requestedDerivedStamp`/`requestedPreparedStamp` + `published*`; **`status:ok`
> requires full-stamp equality** on *every* axis; the requested stamp is set — on the
> edit thread, at commit, and whenever any input axis changes — and the arbiter
> publishes),
> **D39** (derivation splits into a **bounded synchronous semantic phase** —
> parse→CST→bind→**reference resolution (traced `ReferenceUse`)**→typecheck,
> deterministic and edit-thread-OK — used for `propose_patch` precommit validation and
> for **rename**, and an **asynchronous expensive phase** — realize/tessellate, TLAS,
> `prepare`, render — which is D34's arbiter job; the sync phase **is the front of the
> async job**, the same resolver, so there is no second resolution path: "rename
> synchronously derives head" (D35) means **run the sync semantic phase to head**),
> **D40** (the seed makes **`prepare` deterministic** — same `PreparedStamp` → same
> photon maps — while the **final render is reproducible *within Monte-Carlo
> tolerance*, not bit-identical**; bit-identical rendering is a named future option),
> **D41** (an asset binds to the stamp by the **content digest of the exact bytes the
> loader consumed** — load-and-hash one buffer, or revalidate-after-load; the
> `DerivedStamp` asset axis is a **content digest**, not a session generation),
> **D42** (the `PreparedStamp` carries the resolved **`effectiveRenderConfigHash`**
> (from `ResolveEffectiveRenderConfig(DerivedScene, request)` — scene-authored ← request
> overrides ← defaults ← auto-resolution) and a **`viewCameraStateHash`** (the
> ephemeral viewport pose/lens), **not** a raw `RenderConfig` / a `CameraId`), and
> **D43** (a commit triggers a **latest-wins preview** job — cancelled by a newer head,
> D34's policy — while an explicit "render *this* stamp" is a **stamp-pinned** job, NOT
> cancelled by a newer head; the two are separate arbiter job classes).
>
> **Round 6 refinements:** **D49** — `VersionStatus` gains a **`phase`** (idle/deriving/routing/
> preparing/rendering/complete/error); "done"/ok ⟺ full-stamp equality **AND `phase==complete`** (a
> matching stamp mid-render is not "done"). **D50** — the preview is the *one* latest-wins surface;
> **pinned renders are a `RenderJobId`-keyed set** with per-job phase + targeted stop/pause. **D47/D48**
> — pinned renders are **not** dropped on a head change and run on the **one render slot** previews
> suspend for (RISE's single-render invariant). **D51** — a commit passes only the **bounded sync
> semantic phase** (D39), so a committed `Version` can be a **broken-but-valid CST** whose async
> derive/render later fails (surfaced as `error` phase + diagnostics, never silent corruption); an
> opt-in `awaitFullValidation` mode awaits async derive+prepare. **D45** — the effective config is
> resolved **after** `DerivedScene` (auto-route may probe), so the PreparedStamp's config axis is the
> *resolved* `effectiveRenderConfigHash`.

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
  data structure (D2), node identity (the lineage **NodeId**, living in the **red
  layer / a side-map** per D15, *not* the shared green node — plus the separate
  content hash and derivation key — and **name-path** for addressing, D9), text↔CST
  round-trip, and the descriptor schema. I consume its `Cst` value type, its
  `NodeId` (identity) and `NodePath` (addressing), and its `parse(text) → Cst` /
  `serialize(Cst) → text` functions. **I do not define the CST; I define how it is
  mutated and versioned.**
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
**red-green tree** (D2): immutable, content-addressed green nodes store *relative
width* + typed content, never an absolute span; the stable **lineage `NodeId`**
(D9) lives in the **red layer / a side-map, NOT in the shared green node** (D15 —
a shared green node is reused at many occurrences, so it cannot carry one
occurrence's id). That side-map is **owned by each `Version` as its `identityRoot`**
(D26: a persistent occurrence→NodeId map, parallel to the green tree and
structurally shared across versions), so every version has an owned home for
per-occurrence identity — there is no free-floating global id table. Absolute
positions are computed on demand by a version-specific red cursor.

### 2.1 The version model: red-green CST + structural sharing (D2)

The CST (owned by Facet 1) is an **immutable persistent red-green tree** (D2,
rust-analyzer / Roslyn lineage). Every **green** node is immutable,
content-addressed, reference-counted, and shared; it stores **relative width**
(byte length incl. its trivia), typed content, and the descriptor link —
**never an absolute byte offset, and never a `NodeId`** (D15: the lineage id lives
in the red layer / side-map, below). An "edit" produces a **new green root** that
shares all untouched subtrees with the old root (path-copying; the changed spine
is O(depth), or **O(depth · log(width)) ≈ O(log N)** through any rope-backed wide
child sequence — D16, below — **gated on the persistent-container prerequisite,
D23, see the gating note**). **Absolute positions are computed on demand** by a
version-specific **red cursor** that walks from a root accumulating widths; the red
layer is cheap and is *not* stored in the shared green nodes.

> **Persistent containers are a named prerequisite for the O(log N) claims (D23).**
> The green tree's path-copy is O(log N) on its own, but the model also leans on
> several side structures that must be persistent for snapshot creation to be
> O(log N)/O(closure) rather than O(N): the **version DAG** (§2.5), each `Version`'s
> **identity side-map** `identityRoot` (occurrence→NodeId, D26), the
> **derivation cache** (the version-scoped memo + dependency graph, D20 — which per
> **D30** lives on a stamp-keyed `DerivedArtifact`, *not* on the immutable `Version`),
> and — in the derived scene — the **manager roots** (name→entity
> maps). Per **D23** **all of these must be persistent immutable containers** (HAMT /
> persistent balanced tree, immer-style) — a named infrastructure prerequisite on par
> with the red-green tree (D2) and the rope (D16). **Honest v1 fallback (D23):** a
> first implementation **may** copy-on-snapshot mutable maps — **O(N_entities) per
> snapshot** — acceptable while entity counts are modest and commits are debounced.
> **In that case the cost claims in this facet are O(N), not O(log N)/O(closure);
> the O(log N) headline is gated on the persistent-container work landing and must
> not be claimed before it.** Every "O(log N)" / "O(closure)" below carries this
> gate implicitly.

> **Three separated concepts — content hash, derivation key, lineage `NodeId` (D15).**
> Round 2 split apart three things round 1 conflated on the green node:
> 1. **Content hash** (green, lossless, **trivia-sensitive**) — a hash of the green
>    node's exact bytes *including* trivia; its purpose is **structural
>    sharing/dedup** of byte-identical subtrees. It carries **no identity**, so two
>    identical syntax fragments share one green node. (A whitespace change makes a
>    *different* green node.)
> 2. **Derivation key** (semantic, **trivia-INsensitive**) — a hash of the node's
>    *meaning* (typed values + child structure, excluding comments/whitespace) **+
>    traced-input versions** (D4). It is **Facet 2's memo cache key**, so a
>    whitespace-only edit is a derivation cache *hit*. (This facet only emits the
>    `ChangeSet`; the key itself is Facet 2's, called out here for D15 conformance.)
> 3. **Lineage `NodeId`** — a per-**occurrence** stable id living in the **red layer
>    / a side-map, NOT in the shared green node** (D15). Because a shared green node
>    appears at many occurrences it cannot carry one id; the side-map keyed per
>    occurrence is what undo lineage, UI bindings, durable agent refs, and rename
>    (§2.2) key on. Everywhere this facet says "the node's NodeId," read "the NodeId
>    the red-layer side-map associates with that occurrence."

> **Wide child sequences are ropes, not vectors (D16).** A naive "red cursor is
> O(depth)" claim is false when a node's children are a plain vector — locating
> child *k* or its offset scans preceding siblings, so a `Document` with thousands
> of top-level chunks is O(N) per lookup and per edit. So a node's child list is a
> **persistent balanced sequence / rope**, each subtree caching aggregate
> byte-width + newline counts → **O(log N)** position lookup *and* O(log N)
> structural insert/remove, with structural sharing preserved. Narrow lists (a
> chunk's handful of params) may stay vectors; the **Document's chunk list and any
> large `RepeatGroup`/heightfield use the rope.** This makes the per-edit and
> red-cursor costs in §2.8 **O(depth · log(width)) ≈ O(log N)**, not O(depth).

> **No absolute-span maintenance (D2).** There is no `CstSpan`
> (byteBegin/byteEnd) stored in nodes and no `ApplyOffsetDeltas` offset-shifting
> machinery — a length change does **not** ripple stored offsets through later
> nodes, so sharing is genuinely **O(log N)** (O(depth) through narrow vector levels,
> O(log width) through each rope-backed wide level, D16) **once the persistent-
> container prerequisite (D23) holds — O(N) per snapshot in the copy-on-snapshot v1
> fallback**. Any edit or diagnostic that needs an absolute position derives it via
> the red cursor. (See §2.8 for why this is what makes the cost model real.)

```
        v0 (green root)            v1 = edit(v0, objects/sphere.position, "1 2 3")
        /   |   \                       /   |   \
    film  objs  lights            film   objs'  lights      ← film, lights SHARED
            |                              |                   (same green nodes;
          sphere                        sphere'                 NodeIds carried by the
          /  |  \                       /  |  \                 red-layer side-map, D15)
                                                            ← only the spine to the
       geom pos mat                  geom pos' mat             edited leaf is path-copied;
                                                               geom, mat SHARED (side-map NodeIds
                                                               carried forward for those occurrences)
```

A **version** is **not** "just a green root + metadata." Per **D26** (as **amended
by D30**) a `Version` owns *two* persistent, structurally-shared roots plus its
metadata — `{ greenRoot, identityRoot, metadata }`. **The derivation cache is NOT
on the `Version`** (D30): a `Version` commits *before* async derivation completes
and can spawn *many* caches (per time/asset/config), so the memo + dependency graph
lives on a **`DerivedArtifact` keyed by a `DerivedStamp`** (and a `PreparedArtifact`
keyed by a `PreparedStamp`), held in a stamp-keyed LRU — one `Version` → many
artifacts (see the artifact/stamp model below):

```cpp
struct Version {
    // ---- the two persistent, structurally-shared roots (D26 as amended by D30) ----
    NodeRef                 greenRoot;       // immutable GREEN CST root (Facet 1's type) — the lossless syntax
    IdentityRoot            identityRoot;    // persistent occurrence→NodeId side-map (D15/D26): where reparse-
                                             //   matching WRITES NodeId assignments and where rename/undo/UI
                                             //   bindings RESOLVE a NodeId. Parallel to greenRoot, shared via D23.
    // ---- NO derivationCacheRoot here (D30): the cache lives on a DerivedArtifact ----
    //   keyed by DerivedStamp, NOT on this immutable Version (which commits before
    //   async derivation and spawns many caches). See the DerivedArtifact model below.
    // ---- metadata ----
    VersionId               id;             // monotonic, process-unique
    VersionId               parent;         // the version this was derived from
    EditLabel               label;          // "Move sphere", "Edit material.ior", "agent: add light"
    Origin                  origin;         // Gui | TextBuffer | Agent | Load
    ChangeSet               changed;        // NodeIds whose subtree differs from parent (D9; drives Facet 2)
    std::chrono::time_point committedAt;
};
```

> **Why a Version is two roots — and where the cache went (D26 amended by D30).**
> D15 placed `NodeId` "in the red layer / a side-map," but a side-map needs an
> **owner**: identical green nodes represent multiple occurrences, so there is no
> place *on the green tree* for per-occurrence identity. D26 gives every `Version` an
> owned **`identityRoot`** — a persistent occurrence/identity structure (a side-tree
> or persistent map, parallel to the green tree, structurally shared across versions
> via D23's persistent containers) mapping each **occurrence/position → its stable
> `NodeId`**. This is *the* place reparse-matching (§2.4) writes NodeId assignments
> and where rename (§2.2), undo lineage (§2.5), and UI/agent bindings (§2.6) resolve
> a NodeId. **D30 removed `derivationCacheRoot` from the `Version`:** the immutable
> CST commits the instant the edit thread runs (cheap), but derivation runs *later*,
> *asynchronously*, and a single `Version` can spawn *many* derivation caches (one
> per `DerivedStamp` axis — time/asset-generation/animation, then per `PreparedStamp`
> — render-config/camera/seed). So the memo + dependency-edge graph (D20) lives on a
> **`DerivedArtifact` keyed by its `DerivedStamp`** (and a `PreparedArtifact` keyed by
> its `PreparedStamp`), held in a **stamp-keyed LRU**, never owned by the immutable
> `Version`. Both `Version` roots remain persistent and structurally shared — sharing
> them cheaply across versions is exactly what **D23's persistent-container
> prerequisite** buys (O(N) per snapshot in the v1 copy-on-snapshot fallback).

> **The stamp/artifact model (D29/D30).** A `Version` identifies only the CST. The
> downstream products are keyed by **stamps**, produced *asynchronously by the render
> arbiter* (D34, cancellable), and held in a stamp-keyed LRU — one `Version` → many
> of each:
>
> ```cpp
> // What a DerivedScene / PreparedRenderState is keyed by (D29 as amended by D41/D42).
> // NOT a single "derivedVersion" int — a DerivedScene also depends on assets +
> // animation + shutter, and the staleness check is cstVersion DAG ancestry, never `<`.
> struct DerivedStamp {
>     VersionId   cstVersion;       // the CST Version this was derived from (DAG node, D2/D26)
>     AssetDigest assetDigest;      // per-asset CONTENT DIGEST of the bytes the loader consumed (D41) —
>                                   //   NOT a session generation counter (that stays only as a fast
>                                   //   in-process change signal); a content digest is a reproducible identity
>     AnimName    animationName;    // the active animation (D31) — an explicit input
>     Interval    shutterInterval;  // the time INTERVAL [t0,t1] (D31, motion blur) — NOT a single time
> };
> struct PreparedStamp {           // identifies a PreparedRenderState (D29 amended by D42)
>     DerivedStamp derived;
>     Hash         effectiveRenderConfigHash; // content hash of ResolveEffectiveRenderConfig(DerivedScene, request)
>                                             //   (D42): scene-authored ← request overrides ← defaults ←
>                                             //   auto-resolution (resolved integrator/resolution) — NOT a raw RenderConfig
>     Hash         viewCameraStateHash;       // content hash / generation of the COMPLETE view-camera state —
>                                             //   ephemeral viewport pose + lens (D42) — NOT a CameraId
>                                             //   (a CameraId can't identify a continuously-changing pose)
>     Seed         samplingSeed;              // RNG-stream identity → deterministic PREPARE (D33/D40);
>                                             //   the render itself is reproducible within MC tolerance, not bit-identical (D40)
> };
> struct DerivedArtifact {         // the cache lives HERE, not on Version (D30)
>     DerivedStamp        derivedStamp;
>     /*const*/ DerivedScene       derivedScene;      // sealed, immutable (D12/D22)
>     DerivationCacheRoot derivationCache;            // version-scoped memo + dep-edge graph (D20),
>                                                     //   persistent + structurally shared (D23)
> };
> struct PreparedArtifact { PreparedStamp preparedStamp; /*const*/ PreparedRenderState preparedRenderState; };
> ```
>
> Cache lookups match the **full stamp by equality**; the "is the render stale vs
> head?" check compares the **`cstVersion` axis only**, via **version-DAG
> ancestry** (the rendered `cstVersion` is an ancestor-or-equal of head's), **never
> numeric `<`** — the DAG has branches and the other axes are equality-matched, not
> ordered (D29).
>
> > **The stamp's config + camera + asset axes are *resolved/hashed*, not raw
> > (D41/D42).** Three axes above are deliberately content-derived so the stamp is a
> > reproducible identity, not a request snapshot: (a) the **asset axis is a content
> > digest of the actually-loaded buffer** (D41) — the loader reads bytes once and the
> > digest of *those* bytes is the identity (or the loaded bytes are re-hashed and
> > revalidated against the manifest after load), closing the path-hash→reopen TOCTOU
> > where a file changes between hash and load; a session generation counter survives
> > only as a *fast in-process change signal*, never as the stamp identity. (b) The
> > **render-config axis is `effectiveRenderConfigHash`** (D42) — the hash of
> > `ResolveEffectiveRenderConfig(DerivedScene, request)`, a deterministic merge of scene-authored
> > rasterizer/integrator settings ← request overrides ← defaults ← auto-resolution
> > (e.g. the auto-rasterizer's resolved integrator/resolution) — not a raw
> > `RenderConfig`, because the raw request alone does not capture what the engine will
> > actually run. (c) The **camera axis is `viewCameraStateHash`** (D42) — a hash /
> > generation of the *complete* ephemeral view-camera state (pose + lens) — not a
> > `CameraId`, which cannot identify the continuously-changing interactive viewport
> > pose (§2.6's `SessionState.previewCamera`). A render-time integrator or camera
> > override therefore changes `effectiveRenderConfigHash` / `viewCameraStateHash` and
> > re-runs **only `prepare`** (a new `PreparedStamp` from the same sealed `DerivedScene`),
> > not the scene derivation.

`NodeRef` is `boost/std::shared_ptr<const GreenNode>` (or an arena handle — Facet
1's call); `IdentityRoot` is a persistent-container handle, and `DerivationCacheRoot`
(now carried by a `DerivedArtifact`, D30) is another (D23). The key property: **both
`Version` roots are immutable**, so holding a `Version` is holding a complete,
self-consistent *CST* snapshot at O(1) **handle-copy** cost. There is no "live scene
that must be reverted." Undo *is* re-publishing an older version's two roots (green +
identity); the derived/prepared artifacts for that version's stamps are re-found in
(or re-derived into) the stamp-keyed LRU (D30), not carried on the `Version`.

> **The layered immutable model (D21/D22) — what a `Version` is, and what hangs off
> it.** A `Version` is the **CST** version (the two roots above). Everything the
> renderer consumes is a **downstream immutable product** of it — keyed by stamp,
> held in a stamp-keyed LRU (D30) — in two layers, with render-local mutable scratch
> beneath:
>
> ```
> Version (CST: greenRoot + identityRoot, D26/D30)        ← cache is NOT here (D30)
>   + AssetManifest (path → (size,mtime) prefilter → content DIGEST of loaded buffer, D5/D17/D41)
>   + animationName + shutter [t0,t1]          ← animation inputs (D21/D31): a time INTERVAL
>         │ SYNC semantic phase: parse→CST→bind→ref-resolution→typecheck (D39)
>         │           bounded, deterministic, edit-thread-OK — backs validate + rename (D35/D39)
>         │ ──────────────────────────────── (async expensive phase below, on the arbiter) ────────────
>         │ derive  — config-INDEPENDENT (D22); manager roots persistent (D23);
>         │           realize/tessellate loads asset BYTES; async + cancellable (D34/D39)
>         ▼
>   DerivedArtifact{ DerivedStamp, DerivedScene, derivationCache }   ← cache lives HERE (D30)
>     DerivedScene(CST, assets, [t0,t1])       immutable: realized/tessellated geometry,
>         │                                    materials, lights-as-emitters, TLAS;
>         │                                    animated quantities baked as at(τ) over the shutter (D31)
>         │ prepare(scene, EffectiveRenderConfig) — config-DEPENDENT (D22); DETERMINISTIC, seeded (D33/D40)
>         ▼
>   PreparedArtifact{ PreparedStamp, PreparedRenderState }
>     PreparedRenderState                      immutable: light samplers, photon maps
>         │                                    (integrator-specific)
>         │ render(EffectiveRenderConfig)       — reproducible WITHIN MC tolerance, not bit-identical (D40);
>         ▼                                      render-LOCAL MUTABLE scratch (D21): irradiance cache, accum
>   Image
> ```
>
> - **`DerivedScene = f(CST, AssetManifest, animationName, shutter)` is
>   config-independent (D22).** It owns realized/tessellated geometry, materials,
>   lights-as-emitters, and the **TLAS**. **Keyed by its `DerivedStamp = { cstVersion,
>   assetDigest, animationName, shutterInterval }`** (D29 amended by D41: the asset axis
>   is a **content digest of the loaded buffer**, not a session generation) and held on
>   a `DerivedArtifact` together with its derivation cache (D30) — *not* by a single
>   `derivedVersion` int (the asset digest and animation/shutter are real axes).
> - **`PreparedRenderState = prepare(DerivedScene, EffectiveRenderConfig)` is
>   config-dependent (D22).** Light samplers depend on the integrator's
>   light-sampling strategy; photon maps exist only for photon-consuming
>   integrators — so they are **not** pure `f(CST, AssetManifest)` and live *here*,
>   not in `DerivedScene`. The config input is the **resolved `EffectiveRenderConfig`**
>   (D42: `ResolveEffectiveRenderConfig(DerivedScene, request)` = scene-authored ← request
>   overrides ← defaults ← auto-resolution), not a raw request. **Keyed by its
>   `PreparedStamp = DerivedStamp + { effectiveRenderConfigHash, viewCameraStateHash,
>   samplingSeed }`** (D29 amended by D42: the config + camera axes are *resolved/hashed*,
>   not a raw `RenderConfig` / a `CameraId`); the seed makes **`prepare` deterministic
>   and cacheable** (D33/D40 — the render itself is only reproducible-within-tolerance,
>   D40). A render-time integrator/camera override re-runs **only `prepare`** (a new
>   `PreparedStamp` — a different `effectiveRenderConfigHash` / `viewCameraStateHash`),
>   not the scene derivation.
> - **Both layers are immutable + sealed (D12).** The render loop adopts a
>   `PreparedRenderState` at a **pass boundary** (§2.9). What the renderer *swaps* is
>   the `PreparedRenderState` pointer; the `DerivedScene` it was prepared from stays
>   alive by refcount.
> - **Two derivation phases — a bounded SYNC semantic phase + the async expensive
>   phase (D39, refining D34).** Derivation is **not** monolithically async. It splits
>   into:
>   - a **bounded synchronous semantic phase** — lex → parse → CST → bind-to-descriptor
>     → **reference resolution (the traced `ReferenceUse` set)** → type/pipe/typecheck.
>     It is deterministic, touches no asset bytes beyond *identity*, realizes nothing,
>     and is therefore **safe to run on the edit thread**. Its output is a validated CST
>     + reference graph + diagnostics. This is what `propose_patch` precommit validation
>     (Facet 5) and **rename (D35, §2.2/§4.1)** use — and what D5's `validate` means.
>   - the **asynchronous expensive phase** — realize/tessellate (which loads asset
>     *bytes*), build the **TLAS**, `prepare` (light samplers / photon maps), and
>     render. This is D34's cancellable arbiter job.
>   The crucial no-drift property: **the sync semantic phase IS the front of the async
>   job** (literally the same code), so it is **not a second resolver** — D35's
>   one-resolution-path guarantee holds. **Scope note (D39):** the sync phase resolves
>   references to **CST-declared** name-paths only; references *into* asset-expanded
>   sub-entities (e.g. a glTF import's children) need the async realize phase and are
>   **out of v1 cross-reference scope**.
> - **The expensive phase runs async + cancellable on the render arbiter (D34).** The
>   edit thread runs only the cheap CST commit (and, when needed, the bounded sync
>   semantic phase above, D39); the arbiter runs realize → seal → prepare → seal →
>   render as cancellable phases, restarting at a new stamp when a newer head arrives.
>   This async lag is exactly why `headVersion` and the rendered `cstVersion` differ
>   (D13/D29). Nothing *expensive* (asset-byte loading, tessellation, TLAS, photon
>   tracing, render) runs on the edit/agent thread — only the bounded, deterministic
>   semantic phase does (D39).
> - **`prepare` is deterministic; the render is reproducible-within-tolerance, not
>   bit-identical (D40, refining D33).** The `samplingSeed` axis of the `PreparedStamp`
>   makes **`prepare` (photon-map construction) deterministic** — same `PreparedStamp`
>   → same photon maps — which is all the `PreparedArtifact` cache soundness needs (D33).
>   The **final render is NOT bit-identical**: per-worker independently-seeded RNGs,
>   tile assignment, splat reduction, and denoise all vary, so the same
>   scene/config/seed yields the same converged image only **up to Monte-Carlo noise**.
>   The git-native "diffable renders" thesis weakens accordingly to **review-by-image,
>   not byte-diff**. Fully **bit-identical rendering** (deterministic per-pixel/per-sample
>   streams + deterministic reduction + deterministic denoise across *every* renderer)
>   is a **named future option**, not v1.
> - **Animation = per-frame, time-INTERVAL derivation (D21 amended by D31).** The
>   active **animation name** and the **shutter interval `[t0,t1]`** are derivation
>   inputs (both axes of the `DerivedStamp`, D29). A motion-blurred frame's
>   `DerivedScene` is **interval-parameterized**: animated quantities are baked as
>   immutable functions/samples over the shutter (PBRT-style `AnimatedTransform`), and
>   the renderer reads `at(τ)` per sample **read-only** — preserving the per-sample
>   random-time motion blur the rasterizers/photon-tracers do today, with no
>   mutation-during-render. The TLAS for such a frame is a **motion BVH**. This is
>   **gated work (D31)**: **v1 supports single-time (no motion blur)** — `[t0,t1]`
>   collapses to a point; the AnimatedTransform-in-`DerivedScene` + motion-BVH path is
>   a named follow-on (like the TLAS-refit gate, D24). Either way this is **not** a
>   `Version`/snapshot-history concern — the interval is an input to derivation, not a
>   new CST version per frame.
> - **Irradiance caching / accumulation are render-local mutable (D21).** Caches
>   *populated during a pass* are owned by the render pass, **not** part of any
>   immutable snapshot. They may persist across passes/frames for temporal coherence
>   (the renderer's concern), keyed to the snapshot they accelerate, and are
>   invalidated when the scene changes. Caches *built before a pass* (photon maps,
>   light samplers, TLAS) are immutable, computed-once inputs — TLAS in `DerivedScene`,
>   light samplers + photon maps in `PreparedRenderState` (D22).
>
> Everywhere this facet earlier said "the snapshot owns light samplers + photon
> maps" (the D12 wording), read the **D22 refinement**: `DerivedScene` owns the
> config-independent scene + TLAS; light samplers + photon maps live in
> `PreparedRenderState`. §2.3 and §2.9 are written to this layered model.

> **Head version vs derived stamp are two distinct surfaces (D13 as amended by
> D29/D38).** The *document* advances ahead of the *rendered scene*: a commit moves the
> CST head immediately, but derivation (Facet 2) runs **asynchronously on the render
> arbiter** (D34), may still be in-flight, or may have failed and be serving a
> last-good artifact. So the session never collapses the two into one id. It
> publishes **one coherent status value** that exposes both **what the arbiter is
> trying to produce (the *requested* stamps) and what it has actually published (the
> *published* stamps)** (D38) — and crucially each side is a **full stamp**, not a
> single `derivedVersion` int (D29: a `DerivedScene` also depends on the asset digest +
> animation/shutter, and a `PreparedRenderState` on the resolved config + view-camera +
> seed, so a bare CST-version int can't identify either):
>
> ```cpp
> struct VersionStatus {
>     VersionId                   headVersion;            // the CST truth the document is at (advances on every Commit/Undo/Redo)
>     // ---- REQUESTED: what the arbiter is TRYING to produce (D38) ----
>     DerivedStamp                requestedDerivedStamp;  // the DerivedStamp the arbiter is targeting NOW;
>                                                         //   set by the edit thread at commit AND whenever ANY input
>                                                         //   axis changes (assets / animation / shutter), not just the CST head
>     PreparedStamp               requestedPreparedStamp; // the PreparedStamp targeted now (adds effective-config /
>                                                         //   view-camera / seed axes, D42); set when any of THOSE axes change too
>     // ---- PUBLISHED: what the arbiter has actually sealed + published (D38) ----
>     DerivedStamp                publishedDerivedStamp;  // FULL stamp the published DerivedScene reflects (D29) —
>                                                         //   { cstVersion, assetDigest, animationName, shutterInterval };
>                                                         //   its cstVersion may be an ANCESTOR of head, or last-good on error
>     PreparedStamp               publishedPreparedStamp; // FULL stamp the published PreparedRenderState reflects (D29/D42)
>     std::shared_ptr<const DerivedScene>        snapshot;  // the sealed config-INDEPENDENT scene publishedDerivedStamp points at (D12/D22/D30)
>     std::shared_ptr<const PreparedRenderState> prepared;  // prepare(snapshot, EffectiveRenderConfig) — what the render loop ADOPTS at a pass boundary (D22/D42)
>     // (D49) "done"/ok REQUIRES full-stamp equality published==requested on EVERY axis (D38)
>     //   AND phase==Complete. A matching stamp mid-render is "being produced", not "produced":
>     //   the PreparedRenderState is PUBLISHED before rendering starts, so stamp-equality alone
>     //   is true throughout the render — the phase is what says the output actually exists.
>     enum class Phase { Idle, Deriving, Routing, Preparing, Rendering, Complete, Error } phase;
>     std::vector<Diagnostic>     diagnostics;             // why published lags requested / failed / is broken (D51)
> };
> // This is the single LATEST-WINS preview surface (one). Explicit/pinned renders are a
> // separate RenderJobId-keyed SET, each with its own pinned stamp + Phase + targeted
> // stop/pause/resume (D50); a pinned render is NOT dropped on a head change (D47), and
> // runs on the ONE render slot that previews suspend for (D48).
> ```
>
> - `headVersion` is what `read_document` (Facet 5) is stamped with — the CST truth.
> - The **`requested*` stamps are what the arbiter is trying to produce** (D38). They
>   are set by the edit thread on commit and **whenever *any* input axis changes —
>   not just `headVersion`**: a time/shutter scrub, an asset edit, a render-config
>   change, or a viewport camera move all bump the relevant requested stamp even though
>   the CST head is unchanged. This is the surface a client uses to see "the engine
>   knows it owes me a new render," which a published-only status could not show.
> - The **`published*` stamps are what `read_graph` / `render` / `derive_preview` are
>   stamped with** — what the scene actually reflects. `publishedDerivedStamp.cstVersion`
>   may be a strict **DAG ancestor** of `headVersion` (derivation lagging) or pinned to
>   the **last-good** artifact while a broken head is edited (`status == Error`; see
>   §2.9's last-good rule).
> - **`status:ok`/done requires *full-stamp equality* — `published == requested` on EVERY
>   axis** (cstVersion, assetDigest, animation, shutter, effective-config,
>   view-camera, seed) (D38) **AND `phase == complete`** (D49 — the PreparedRenderState is
>   published *before* rendering starts, so stamp-equality alone holds throughout the render; the
>   phase attests the output exists). Until then the status is one of
>   `Deriving | Preparing | Rendering` (which phase the arbiter is in), or `Error`. The
>   two stamps are **never asserted equal when they are not**; `status` + `diagnostics`
>   explain the lag or failure. The steady state is `published == requested && status ==
>   Ok`; transient and error states are first-class, not hidden. **The *staleness vs
>   head* test is DAG ancestry on the `cstVersion` axis, never numeric `<`** (D29: the
>   version DAG has branches); the other axes are matched by **equality** (D29/D38), and
>   `ok` is precisely all-axes-equal.
> - **A patch's optimistic-concurrency precondition (Facet 5) is checked against
>   `headVersion`**, not either derived stamp — an edit re-bases against the CST truth,
>   independent of how far derivation has caught up. The §2.6 concurrent-edit
>   conflict check and the §2.9 agent-vs-gesture race both pivot on `headVersion`.

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
| **Persistent (structurally-shared) immutable tree — the red-green model (D2)** | **chosen**: green nodes store relative width (no absolute spans, no NodeId — NodeId is red-layer/side-map per D15, owned per-Version as `identityRoot` per D26), positions via the red cursor over rope-backed children (D16); O(log N) memory/time per edit **(gated on D23's persistent containers; O(N) per snapshot in the v1 fallback)**, O(1) snapshot **handle-copy**, atomic by construction, natural branch support. The *only* candidate consistent with both INV-4 (lossless) and the (D23-gated) O(log N) cost model. |

### 2.2 The edit primitive: a CST patch

One type. Both clients (GUI, agent, text) ultimately produce it.

```cpp
// A patch is a pure description of a CST transformation. It does NOT carry
// inverse/prev state — the previous Version is the inverse.
struct CstPatch {
    enum class Kind { SetField, InsertNode, RemoveNode, ReplaceSubtree, Rename, ReparseSpan };
    Kind            kind;
    NodeRefId       target;        // the lineage NodeId (red-layer/side-map, D9/D15/D36) being edited.
                                   // D36: the patch carries the target NodeId, NOT just a name-path —
                                   // same as EditIntent/Widget/ViewNode/selection (all NodeId-keyed).
    // Clients address by name-path; the API resolves name-path → NodeId against the
    // base version BEFORE building the patch, so the patch is rename-stable.
    // SetField:        newText (the verbatim token(s) to splice for this field)
    // InsertNode:      newSubtree + position
    // RemoveNode:      (target only)
    // Rename:          newName (NodeId-preserving; rewrites the name token + all referrers
    //                  found via traced ReferenceUse records, D14 — not referenceCategories)
    // ReparseSpan:     rawText (a re-typed region; best-effort identity, see §2.4)
    CstFragment     payload;       // parsed fragment or raw text, per Kind
    EditLabel       label;         // human/UI-facing; defaults from Kind+target
};
```

> **Addressing vs identity at the patch boundary (D9).** Clients (panel, agent,
> text view) speak **name-path** (`objects/sphere.position`); the edit API resolves
> that to the immutable **NodeId** *against the base version* and stamps it into
> `target`. The patch therefore survives a concurrent rename of an ancestor — it
> still points at the right node. **Rename is its own first-class, NodeId-preserving
> patch kind** (`Kind::Rename`): it rewrites the name token in place (same side-map
> NodeId, D15) and rewrites every referrer found via the **traced `ReferenceUse`
> records (D14)** — derivation's resolved `{sourceValueNodeId, targetNodeId}` set,
> which captures *dynamic* refs (e.g. `timeline.element`/`.animation`, whose target
> category is chosen by `element_type`) that the static `referenceCategories` hint
> cannot see — with a descriptor-resolver fallback for untraced (e.g. error-subtree)
> referrers and a **flag** for any that stay unresolved. So UI/agent bindings keyed
> on NodeId survive automatically. This is strictly better than treating rename as
> remove+add (which would mint a fresh NodeId and drop bindings — see §4.1).
>
> **Rename uses the ONE derivation resolver, against head (D25 as amended by
> D35/D39).** Rename rewrites referrers at **`headVersion`** (the CST truth), yet the
> `ReferenceUse` set is produced by **derivation**, whose `cstVersion` may be a DAG
> ancestor of head (D13/D29). A reference *added in head but not yet derived* would be
> **missed** — silently leaving a dangling old name. D25 closed that by requiring a
> head-stamped trace; **D35 sharpens *how*: there is exactly ONE
> reference-resolution implementation — derivation's own evaluator/resolver — and
> rename reuses it, never a parallel "tracing pass" reimplementation** (a second path
> would drift from real derivation, precisely the static-walk failure D4 rejected for
> dynamic refs). Concretely, rename **runs the bounded synchronous semantic phase to
> head** (D39: parse→CST→bind→reference-resolution→typecheck — the *front of the async
> derivation job*, not a separate pass, so no drift) and reads the resulting traced
> `ReferenceUse` records. This phase is deterministic and edit-thread-OK (it loads no
> asset bytes and realizes nothing — D39), and rename is a deliberate, infrequent op,
> so running it synchronously is acceptable; it does **not** trigger the async expensive
> phase (realize/TLAS/prepare/render). A rename **never** runs against a stale or
> separately-computed trace; **if the sync semantic phase to head fails (semantic
> error), the rename is refused** (surfaced as an error), never run best-effort/partial.
> (Concrete handling in §4.1.)

Applying a patch is a **pure function** owned jointly with Facet 1. It takes — and
returns — **both roots** (green + identity, D36), because an `InsertNode` / `Rename`
/ `ReparseSpan` mints or re-associates `NodeId`s, so the identity side-map must
advance in lock-step with the green tree (this is what lets the `GestureBuffer`
carry a live `workingIdentityRoot`, §2.3):

```cpp
// Returns NEW roots sharing all untouched subtrees, plus the changed-path set.
// Pure: no I/O, no global state, no mutation of the inputs.
struct ApplyResult {
    NodeRef       newGreenRoot;     // path-copied green root (D2)
    IdentityRoot  newIdentityRoot;  // identity side-map with NodeId mints/re-associations applied (D36)
    ChangeSet     changed;
    std::optional<ParseError> error;
};
ApplyResult ApplyPatch(NodeRef baseGreen, IdentityRoot baseIdentity, const CstPatch& p);
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
*uncommitted working head* — BOTH roots, a green root *and* its identity side-map
(D36) — and derives a *cheap preview snapshot* (debounced per O2, ephemeral, NOT a
history version); at gesture end the intermediate roots coalesce into exactly ONE
committed version (one undo unit).** The "working head" is **CST state** (green +
identity), not a side-channel of non-CST mutable state.

```cpp
class GestureBuffer {
    // BOTH roots travel together (D36): a working head is { greenRoot, identityRoot }.
    // A gesture is not always pure SetField — an insertion/reparse mid-gesture mints or
    // re-associates NodeIds, so the working identityRoot MUST advance alongside greenRoot,
    // or those new occurrences would have no identity until commit.
    NodeRef                 baseGreenRoot;     // committed Version's green root at gesture start (the undo target)
    IdentityRoot            baseIdentityRoot;  // committed Version's identity side-map at gesture start (D26/D36)
    NodeRef                 workingGreenRoot;  // base + all staged patches applied, in memory (a green root, D2)
    IdentityRoot            workingIdentityRoot; // base + identity updates from each staged patch (D36) — advances WITH greenRoot
    std::vector<NodeRefId>  touched;           // union of changed NodeIds (D9; drives incremental re-derive)
    EditLabel               label;
public:
    void Stage(const CstPatch& p);             // (workingGreenRoot, workingIdentityRoot) = ApplyPatch(working…, p);
                                               //   ApplyPatch updates BOTH roots (insert/reparse update identity, D36);
                                               //   union touched, then derive a cheap, EPHEMERAL preview snapshot (debounced, O2)
    Version Commit();                          // coalesce: publish ONE Version{ greenRoot=working, identityRoot=working,
                                               //   parent=base, changed=touched } (two roots, D26/D30)
    void    Abort();                           // drop both working roots; base roots stay the committed head
};
```

> **Why the GestureBuffer carries BOTH roots (D36).** Identity stopped at `Version`
> in the pre-round-4 design: the gesture buffer carried only a green root, so an
> **insertion or a mid-gesture reparse** (e.g. dragging a control point that adds a
> `cp`, or a scrub that re-types a region) could not update the working
> `identityRoot` — the new occurrences would have **no NodeId** until the final
> commit re-matched them, breaking any binding that wanted to reference a
> just-inserted node *during* the gesture. So the working head is `{ greenRoot,
> identityRoot }` and `ApplyPatch` (§2.2) advances **both** as it goes; occurrence
> identity is live throughout the gesture, not retrofitted at commit.

- **During the drag** (working-assumption O2 = debounced-commit): each
  `OnPointerMove` calls `Stage`, advancing only the **in-memory working head**
  (both roots — a real green root produced by path-copy + its updated identity
  side-map; it *is* CST state, not side state) and asking Facet 2 for a **cheap,
  debounced, ephemeral preview snapshot** — an incremental **closure-copy re-derive**
  of `touched` (one object's transform's reverse-dependency closure, not a world
  rebuild — INV-3, D11). The preview's **expensive phase** (realize/TLAS/prepare/render)
  runs **async + cancellable on the render arbiter** (D34), off the gesture thread,
  while the bounded **sync semantic phase** that validates the staged patch is
  edit-thread-OK (D39). **A gesture preview is a *latest-wins* arbiter job (D43):** each
  newer `Stage` (or any newer requested stamp) cancels the in-flight preview — that is
  exactly the behaviour wanted mid-drag, and it is *distinct* from a stamp-pinned
  explicit render (§2.9), which a newer head must **not** cancel. Like any snapshot the
  **`DerivedScene` is built → sealed** (D12), then **`prepare`d into a
  `PreparedRenderState`** for the active `EffectiveRenderConfig` (D22/D42) before the
  viewport reads it, and the render loop **adopts that `PreparedRenderState` at a pass
  boundary** (D12). The whole preview product has a transient lifetime — it is not a
  history version, and if the preview is mid-animation its **shutter interval** is
  just a derivation input on the `DerivedStamp` (D29/D31), not a new version. The
  **committed head does not move yet**, so undo history is untouched and the viewport
  reflects the working head's sealed, prepared preview.
- **At gesture end** (`OnPointerUp` / `OnTimeScrubEnd` / slider release):
  `Commit()` **coalesces the intermediate working roots into exactly one
  `Version`** (both roots — `greenRoot` + `identityRoot`, D26/D30) parented on the
  base. That single committed version is the only history entry for the whole drag
  (one undo unit). **The composite-begin/end markers, `mCompositeDepth`,
  `ForceCompositeDepthZero`, the nesting-aware trim, and the composite
  atomic-rollback walks all evaporate** — a gesture is one version because we
  coalesce once, full stop.
- **`Abort()`** (gesture cancelled, view torn down, agent supersedes mid-drag) is
  trivially correct: drop both working roots and the ephemeral preview; the committed
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
    newCst   = Facet1::parse(newText)                          // full lossless re-parse (cheap; see latency)
    diff     = Facet1::DiffCst(currentVersion.greenRoot, newCst) // structural diff → ChangeSet + per-node match map
    newIds   = MatchIdentities(currentVersion.identityRoot, diff) // best-effort occurrence→NodeId carry-over;
                                                              //   unmatched occurrences get fresh NodeIds, durable
                                                              //   refs to unmatched nodes INVALIDATED/flagged (D15/D26)
    version  = PublishReparse(newCst, newIds, diff.changed)    // one Version{greenRoot,identityRoot,…}; drives derive
```

Two sub-strategies, chosen by Facet 1's diff capability:
> **The reparse identity contract (D15) — two tiers, one best-effort.** Identity is
> the red-layer side-map `NodeId`, **owned by the version's `identityRoot` (D26)** —
> reparse-matching is exactly the pass that *writes* the new version's `identityRoot`,
> by matching new green nodes to the prior version's occurrence→NodeId entries. The
> guarantee is **not** uniform across the two ways a CST changes:
> - **Structured edit (a `CstPatch` other than `ReparseSpan`) preserves `NodeId`
>   exactly.** It targets a *known* node (the patch carries its `NodeId`), so the
>   side-map entry is carried through deterministically — no matching, no ambiguity.
>   This is the path panels, the gizmo, and agents use, and it is exact.
> - **Whole-region reparse is best-effort.** Matching new green nodes to prior
>   `NodeId`s by structural position + content is **genuinely ambiguous for
>   identical repeated rows** (e.g. two byte-identical `part` lines, or two
>   same-shape params — nothing distinguishes which prior occurrence a new one
>   continues). So reparse **cannot guarantee identity after an arbitrary text
>   replacement.** Unmatched (or ambiguously-matched) occurrences get **fresh
>   `NodeId`s, and any durable reference still bound to the old `NodeId` is
>   INVALIDATED and flagged — never silently remapped** to a guessed node.

- **Whole-buffer reparse + structural diff** (default, simplest): re-parse the
  whole file, structurally diff against the current root to recover the minimal
  `ChangeSet`, and publish the new root. **Identity is recovered *best-effort* by
  matching new green nodes to prior side-map `NodeId`s by structural position +
  content** (D9/D15: rust-analyzer-style node reuse). A *localized, unambiguous*
  text edit (e.g. retyping one scalar) re-matches cleanly, so selection/agent
  references and UI bindings survive (INV-5) — but per the contract above this is
  best-effort, not a guarantee: identical repeated rows are ambiguous, and any
  durable reference whose node is unmatched is **invalidated/flagged, not remapped**.
  The `ReparseSpan` patch kind is the *incremental* optimization: when the edit is
  confined to one chunk's byte span, reparse only that span into a subtree and
  `ReplaceSubtree` (NodeIds re-matched by the same best-effort structural match,
  with the same invalidate-unmatched rule). Default to whole-buffer; add
  `ReparseSpan` if profiling demands it.

> **Conflict at the seam:** if the user edits the *same node* via a panel and via
> raw text "simultaneously," they are two patches against (possibly) two different
> base versions. This is handled exactly like any other concurrent edit: each
> client edits against a base version; on commit, if the base ≠ current head, the
> editor either fast-forwards (the patch's `target` NodeId's subtree is unchanged at
> head → re-base trivially) or surfaces a conflict (the target's subtree changed at
> head → reject with a structured error, Facet 5). Because edits are scoped to a
> `target` NodeId (D9), true conflicts are *node-local*, not whole-file. See §2.6.

### 2.5 Undo / redo / branch semantics

History is a **DAG of versions** (each a structurally-shared **two-root** value —
`greenRoot` + `identityRoot`, D26 as amended by D30; the derivation cache is **not**
on the version, it lives on stamp-keyed artifacts), not two stacks. Undo/redo is a
**pointer move that republishes a prior version's two roots** (D2/D26): no absolute
spans are reconstructed, no inverse is applied — the red cursor recomputes any
positions lazily for whatever `greenRoot` is published, and bindings resolve against
the republished version's `identityRoot`.

```cpp
class VersionGraph {
    // The version DAG is a PERSISTENT immutable container (D23) — sharing its
    // entries across snapshots/branches is O(log N) only once D23 lands (O(N) copy
    // per snapshot in the v1 fallback). It is NOT a plain mutable unordered_map.
    PersistentMap<VersionId, Version> versions;       // all live versions (each = 2 shared roots, D26/D30)
    VersionId head;                                   // the CST HEAD version (headVersion, D13) — NOT necessarily what's rendered
    // Per-head "redo" is the child you came from when you undid:
    PersistentMap<VersionId, VersionId> lastChildOf;  // for linear redo UX
public:
    // Commit takes the two roots (D26/D30): the new green CST root and the identity
    // side-map written by the edit/reparse. NO derivation-cache arg — the cache is
    // produced LATER, async, on a DerivedArtifact keyed by DerivedStamp (D30/D34).
    Version  Commit(NodeRef greenRoot, IdentityRoot, ChangeSet, EditLabel, Origin); // head→new; new.parent=head
    bool     Undo();   // head = versions[head].parent (if any)
    bool     Redo();   // head = lastChildOf[head] (the branch you last left)
    void     Trim(MemoryBudget);                                      // GC unreachable/old versions
};
```

> **`head` is `headVersion`, not a derived stamp (D13/D29).** `VersionGraph::head` is
> the CST truth — it advances the instant `Commit`/`Undo`/`Redo` runs (on the edit
> thread; cheap). The *derived* product (what the renderer reflects) is tracked
> separately in the session's `VersionStatus` as a **`DerivedStamp`** (§2.1, D29),
> produced **asynchronously by the render arbiter** (D34); its `cstVersion` lags
> `head` while Facet 2 catches up or stays pinned to last-good on error. Undo/redo
> move **`head`** (and therefore `headVersion`); the published derived stamp follows
> asynchronously. The two are never reported equal when they are not, and the
> staleness test is **DAG ancestry on the `cstVersion` axis, not numeric `<`** (D29).

- **Undo** = `head = parent`. **Redo** = `head = lastChildOf[head]`. Both
  republish the target version's **two roots** (D26/D30) atomically; Facet 2
  re-derives the `ChangeSet` (NodeIds, D9) between old-head and new-head (the diff
  of two green roots is cheap — shared subtrees compare by pointer). **No inverse
  application, no `prev*` replay, no composite walk, no partial-revert atomicity
  logic, no absolute-offset recomputation.** An undo cannot "fail half-way" because
  it is a single pointer assignment.
  > **Re-deriving an old version uses *current* asset bytes (D28).** Undo/redo/branch
  > history preserves the **CST (the source) only** — not historical rendered output.
  > Re-deriving an older version (after undo, or by hopping to a branch) re-reads the
  > **live filesystem**: the `AssetManifest` is re-stamped on access (D5/D17), so if a
  > referenced texture/mesh/glTF changed on disk since that version was current, the
  > old version's *CST* is byte-identical but its *render* may differ. This is
  > **explicitly documented and intended** — exactly like git versioning source while
  > the build's large binary inputs are the user's responsibility. The
  > "Scene = f(CST, AssetManifest)" purity holds *within a manifest*; across time the
  > manifest is the live filesystem. A **content-addressed asset store** (snapshotting
  > asset bytes by hash for fully reproducible historical renders — the git-LFS
  > analogue, layered at the VCS boundary, not the editor) is a **named future
  > option**, not core. So a derivation cache (now carried by a `DerivedArtifact`,
  > **not** by the old `Version` — D30) is a *memo*, not a guarantee of identical
  > pixels under changed assets: a re-stamped asset content-hash is a different
  > **`assetDigest`** axis on the `DerivedStamp` (D29 amended by D41 — the asset axis is
  > the content digest of the loaded buffer, not a session generation) and an input
  > version bump (D4), so it keys a *different* artifact and correctly misses the stale
  > memo.
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
  nesting-aware composite-preservation logic. **Once the persistent-container
  prerequisite (D23) holds**, versions share structure across **both** roots
  (green + identity), so the cost of retaining history is "the sum of
  *distinct* subtrees across all retained versions," not N full copies. **In the v1
  copy-on-snapshot fallback (D23) the identity side-map is copied whole
  per snapshot — O(N_entities) per retained version — so this sharing claim is gated
  on D23.** Derivation caches are **not** part of a `Version` (D30) — they live on
  stamp-keyed `DerivedArtifact`s in their own LRU, evicted independently of history
  trimming. Trim policy: keep the last K versions reachable from head's ancestry +
  any branch tips younger than T; GC the rest; ref-counting reclaims subtrees no
  version references. A 60Hz drag is **one** version (it committed once), so the
  pathological "17s drag = 1000 history entries" case that drove the
  nesting-aware-trim complexity **cannot occur**.

### 2.6 Document vs session/view state (L4) — the split drawn explicitly

| State | Today (conflated) | Model B home | Persisted? |
|---|---|---|---|
| **Scene structure & all parameters** | live `Scene` + source bytes | **CST (the document) → `DerivedScene` (config-independent, D22) → `PreparedRenderState` (config-dependent, D22)** | yes (the CST/file; the derived layers are products, not persisted) |
| Object transform / material / light / medium / camera params | live objects + `prev*` | CST leaves | yes |
| **Animation name + shutter interval `[t0,t1]`** (which frame/interval to derive) | mutated into the live scene per frame (`Scene.cpp:561`) | **derivation inputs** (D21/D31), not CST/version state — a frame is a time-INTERVAL `DerivedScene([t0,t1])`; both are axes of the `DerivedStamp` (D29) | no (inputs, not stored state) |
| **Irradiance cache / accumulation buffers** | populated *during* render, intertwined with the live scene | **render-local mutable** scratch owned by the render pass (D21) — NOT snapshot state | no |
| **Selection** (category, NodeId + addressing name, per-category memory) | `mSelectionCategory/Name`, `mSelectionByCategory[9]` | **`SessionState`** (binds to a NodeId, D9; name-path is how it's displayed/re-resolved) | no |
| **Section expanded** (panel accordion) | `mSectionExpanded[9]` | `SessionState` (or per-user UI prefs) | no |
| **Active tool + per-category memory** | `mTool`, `mLastSubToolPerCategory` | `SessionState` | no |
| **Gizmo drag-in-progress** | `mGizmoDrag` (~20 fields) | `GestureBuffer` (transient) | no |
| **Gesture bookkeeping** | `mGestureOpenedComposite`, `mScrubOpenedComposite`, `mPointerDown`, `mLastPx` | `GestureBuffer` lifecycle | no |
| **Orbit-preview camera** (interactive pose during a drag, distinct from the saved camera) | folded into the live camera | `SessionState` *view camera* (see note) | no |
| **Render progress / in-flight pass / preview-scale / polish state** | `mRendering`, `mCancelProgress`, `mPreviewScale`, `mPolishState`, … | **render orchestrator** (its own component, §2.9) | no |
| **`mSceneEpoch`** (UI re-pull trigger) | controller | derived from version id changes | no |
| **"Dirty / unsaved?"** (in-process) | `DirtyTracker` (2 channels) + `mScaleFromAnchorSet` + `HasUnsavedChanges` | **`headVersion != lastFlushedVersionId`** (one comparison, see below) | n/a |
| **External-file conflict** (on-disk changed under us) | *(not detected today)* | **load/flush fingerprint (prefilter + content hash) + atomic save (D6/D17)** | the fingerprint travels with the head |

```cpp
// Pure ephemeral state — never serialized, never versioned, never captured for undo.
struct SessionState {
    Selection            selection;       // a NodeId (D36) + category + per-category memory;
                                          //   name-path is derived for display/re-resolution, NOT the key
    Tool                 activeTool;
    ToolMemory           toolMemory;      // per-category last sub-tool
    PanelExpansion       expanded;        // accordion state (candidate for per-user prefs file)
    ViewCamera           previewCamera;   // interactive orbit pose (NOT the document camera)
    VersionId            viewingVersion;  // which CST version this view is bound to
};
```

> **Selection (and every `Widget`/`ViewNode`/`EditIntent`) is `NodeId`-keyed (D36).**
> Per D36 these store the **`NodeId`**, not a name-path; the name-path is resolved
> *from* the NodeId for the header/display and re-resolved on demand. So a structured
> rename/edit leaves the binding intact (the NodeId is preserved), and a whole-region
> reparse re-associates it best-effort, invalidating/flagging on a miss (D15) — never
> silently remapping a name-path to a guessed node.

**Two independent dirtiness concepts (D6), both required.** Model A had a single
in-process "dirty" notion because the live scene could diverge from the file, and
the `SaveEngine` had to reconcile. Model B replaces it with **two orthogonal
checks** — an in-process signal *and* an on-disk conflict guard — because the
in-process version id alone **cannot** detect that git or another editor rewrote
the file underneath us.

**(1) In-process "unsaved" signal — `headVersion != lastFlushedVersionId`.** In
Model B the CST *is* the file's content, so (this is the **head** version, D13 —
the CST truth, independent of how far the derived stamp's `cstVersion` has caught
up, D29):

- If we adopt **autosave / file-as-canonical** (O1 text-canonical leaning): every
  committed version is serialized to disk (debounced), so "unsaved" ≈ "the last
  commit hasn't flushed yet" — a transient I/O state, not a user-facing modal.
  The git-native framing (Facet 5) makes "save" ≈ nothing and "checkpoint" ≈ a
  commit the user names.
- If we keep an **explicit save** affordance (lossless-CST-canonical, in-memory
  head not yet flushed): "dirty" = `headVersion != lastFlushedVersionId` — a
  single integer comparison, **not** a 2-channel tracker + a 5th set + transition
  listener. The entire `DirtyTracker`/`mScaleFromAnchorSet`/
  `FireDirtyChangedIfTransitioned` apparatus collapses to that comparison.

This signal is **in-process only** (§438 of the legacy model): it answers "have I
made changes since my last flush," nothing about the bytes on disk.

**(2) On-disk external-conflict guard — load/flush fingerprint + atomic save
(D6/D17).** A *second, independent* concept is required to stop autosave (or an
explicit save) from silently clobbering an external change:

- Record the file's **fingerprint** **at load and at each flush**. Per **D17**
  the fingerprint is **(size, mtime) as a fast prefilter → a content hash** when
  the prefilter changed or determinism is required (size+mtime alone is *not*
  authoritative — bytes can change with mtime preserved). The content hash is the
  same AssetManifest identity D5/D17 uses; it travels with the head.
- **The save is atomic (D17), not stat-then-write.** The round-1 "compare-and-swap"
  phrasing was a **TOCTOU race** (check fingerprint, *then* write — an external
  writer can land between the two). The conforming sequence is **temp-file write
  (in the target's directory) → `fsync` → revalidate the target's current content
  hash == the fingerprint loaded with the head → atomic `rename()` over the
  target.** If the revalidated hash ≠ the loaded fingerprint, the file changed
  externally → **do not overwrite**; surface the D6 conflict UX with explicit
  choices — **reload** (discard the in-process head), **diff/merge**, or
  **force-overwrite** — never a silent overwrite. On a successful rename, update the
  stored fingerprint.
- **Documented residual (D17):** the atomic rename closes the *check→write* TOCTOU,
  but a **non-cooperating concurrent writer can still race the final `rename()`**
  (last-writer-wins at the filesystem layer — our rename and theirs are not
  ordered). Eliminating that requires **advisory file locking**, offered as an
  **opt-in** for shared-storage setups; it is deliberately not the default
  (advisory locks are unreliable across NFS/SMB and add an unlock-on-crash failure
  mode). The default accepts the bounded residual.
- A background watcher (D5's file-watch mechanism) can surface "file changed on
  disk" proactively, so the user learns of the conflict before they try to save.

```cpp
// The two signals are computed independently; neither subsumes the other.
bool   hasUnsavedChanges = headVersion != lastFlushedVersionId;       // (1) in-process (D13 head)

// (2) on-disk: atomic temp-write + fsync + revalidate + rename (D17) — NOT stat-then-write.
enum class SaveOutcome { Wrote, Conflict_External };
SaveOutcome Flush(NodeRef head, Fingerprint loadedFingerprint) {     // loadedFingerprint = (size,mtime) prefilter → content hash (D17)
    Path tmp = WriteTextToTemp(targetDir,                            // 1) write into the TARGET's dir (same fs, so rename is atomic)
                               serialize(head, /*via red cursor*/));  //    INV-4 round-trip (§2.4)
    Fsync(tmp);                                                       // 2) fsync the temp's bytes to disk
    if (ContentHash(target) != loadedFingerprint.hash) {             // 3) revalidate the LIVE target vs what we loaded (D17)
        Remove(tmp);                                                 //    external change since load → never overwrite
        return SaveOutcome::Conflict_External;                       //    → D6 conflict UX: reload / diff / force
    }
    AtomicRename(tmp, target);                                       // 4) atomic rename over the target (closes check→write TOCTOU)
    loadedFingerprint = Fingerprint(target);                         //    update stored fingerprint
    return SaveOutcome::Wrote;
    // Residual (D17): a non-cooperating writer can still race step 4's rename
    // (last-writer-wins); advisory locking is an opt-in, not the default.
}
```

Either way the **P-STATE class is gone**: there is no hand-assembled "what's
dirty" set to keep complete — the in-process signal is two version ids and the
external-conflict guard is two fingerprints (prefilter + content hash, D17). (The
legacy `FileIdentity` external-mod guard's *intent* is retained here even though its
byte-splice *mechanism* is deleted — D6/D17 / §3.1.)

> **Why selection is *not* in the CST:** selection binds to a node's lineage
> **NodeId** (red layer / side-map, D9/D15, INV-5), displayed/re-resolved via its
> name-path. Because it keys on NodeId, the binding survives a **structured**
> rename/edit *exactly* and a whole-region text-edit reparse **best-effort** (NodeIds
> re-associated by structural matching, D15); if the selected node is unmatched after
> a reparse, the binding is **invalidated/flagged** (handled like any stale selection
> below), never silently remapped. When a version changes, `viewingVersion` advances
> and selection is re-validated against the new root (the `DropStaleSelection_` check
> survives, but as a pure function `resolves(selection, version)` with no
> mutation entanglement; it resolves the selection's NodeId through the new version's
> `identityRoot`, D26). Putting selection in the document would violate INV-1 (it'd
> be a second mutable thing to keep consistent and would pollute diffs/round-trip).

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
  reports *what NodeIds changed*. The traced dependency (the `ReferenceUse` graph,
  D14) replaces a dozen open-coded `Bump*` calls; its per-node edge set is owned by
  the persistent derivation cache with an explicit lifecycle (D20) — which per **D30**
  lives on a `DerivedArtifact` keyed by `DerivedStamp` (a stamp-keyed LRU), *not* on
  the `Version`. (Cross-facet ask on Facet 2: trace the light-sampler/env-IBL
  dependency on the emitter-set; see §5.)
- **P-FFMATH (NaN sentinels) — UNCHANGED but shrunk.** The model removes most of
  the code that used NaN-as-not-found (the reverse-lookups, the `prev*` "absent"
  encodings). The `SourceHygieneTest` + `red_prove.sh` guardrails stay. New CST
  code should use `std::optional` for absence, never NaN.

### 2.8 Memory & performance of versioning a large CST

> **All four cost rows below are gated on D23.** They hold once the version DAG and
> each `Version`'s identity side-map (`identityRoot`, D26) are **persistent immutable
> containers** (the derivation cache is **not** a `Version` side structure — per D30
> it lives on stamp-keyed `DerivedArtifact`s in their own LRU, so it does not factor
> into per-*commit* CST cost). In the **honest v1 copy-on-snapshot fallback (D23)**
> each commit copies the identity side-map whole — **O(N_entities) per commit** — so
> the green tree's O(log N) path-copy is real but the *per-version* cost is O(N) until
> the persistent containers land. The O(log N)/O(closure) figures are the **target**,
> not the v1 guarantee.
- **Per-edit cost:** O(depth of edited path) green-node copies (D2 path-copy) —
  **O(depth · log(width)) ≈ O(log N)** once a wide child sequence on the spine is a
  rope (D16: re-linking a rope child is O(log width), not O(width)); the version's
  `identityRoot` carries forward the prior NodeIds for unchanged occurrences (D15/
  D26), siblings are shared by reference. A scene CST is shallow (file → chunks →
  params); editing one parameter copies ~3–5 green nodes (root → chunk → param).
  Sub-microsecond *for the green spine*; allocations dominated by one small node.
  **No offset rewrite of later nodes** (D2: relative widths, positions via the red
  cursor) — that, plus the rope (D16), is what keeps the green path O(log N) rather
  than O(document). **(The whole-edit cost is O(log N) only when the `identityRoot`
  is persistent too — D23; else O(N) for the identity side-map. The derivation cache
  is not copied per commit — it is artifact-scoped, D30.)**
- **Per-edit memory:** the copied green spine only. Untouched chunks (the other 154
  objects in a Sponza scene) are pointer-shared. A drag commits **one** version,
  so 5s @ 60Hz = 1 retained version's worth of distinct nodes, not 300. **(Side
  structures: O(log N) shared under D23; O(N) copied per snapshot in v1.)**
- **History memory:** Σ distinct subtrees across retained versions **once D23
  holds**. Editing 50 different parameters across 50 versions retains ~50 small green
  spines + one shared bulk — kilobytes, not 50× the file. **(In v1 each version also
  holds a whole copy of the identity side-map — O(N) — until the persistent
  containers land. Derivation caches are artifact-scoped, D30 — bounded by the
  stamp-keyed LRU, not by history depth.)**
- **Snapshot/branch cost:** O(1) **handle-copy** of a version's two roots (green +
  identity, D26/D30). (The *content* sharing behind those handles is O(log N) under
  D23, O(N) in the v1 fallback.)
- **Persistence (on disk / branch tips):** serialize head to text by walking the
  green tree with the red cursor (D2) into **one self-contained `.RISEscene` file**
  (D7: a v7 document is single-file; `> load`/`> run` are deprecated, so there is
  no forest to serialize). For multi-version branch persistence (if surfaced),
  store the **text-diff between adjacent versions** — this is where the
  rejected-as-primary text-diff model earns its keep: compact at rest, and
  reconstituted into the persistent tree on load by parse+apply. So the canonical
  pivot is "persistent red-green tree in memory, single-file text (± diffs) at
  rest." Each flush runs the D17 atomic save — temp-write → fsync → revalidate →
  rename (§2.6) — before committing the new bytes.
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

> **A render pass renders a specific immutable, sealed `PreparedRenderState` (D22) —
> `prepare(DerivedScene, EffectiveRenderConfig)`, where the `DerivedScene` is the
> config-independent sealed snapshot (D22) and the config is the *resolved*
> `EffectiveRenderConfig` (D42), not a raw request.** Publishing a new version derives +
> seals a new `DerivedScene` and `prepare`s a `PreparedRenderState` for the active
> `EffectiveRenderConfig`; the renderer atomically swaps its **`PreparedRenderState`**
> pointer **only at a PASS boundary** (never mid-frame / per-tile, D12); the old prepared
> state (and the `DerivedScene` behind it) stays alive (refcount) until the in-flight
> pass drains. Publishing never mutates what the render thread is reading — and
> nothing mutates a `DerivedScene` *or* a `PreparedRenderState` after it is sealed
> (D12: all of phase B happens before each seal). **Per-pass-populated caches
> (irradiance, accumulation) are render-local mutable scratch (D21), owned by the
> pass — not part of either immutable layer.**

This is the D1 model as sharpened by D11/D12 and layered by D21/D22, and the big
simplification immutability buys. In Model A, an edit *mutates the live scene the worker is
mid-`IntersectRay` on* → the whole cancel-park-mutate-under-lock dance (and the
time-scrub UAF the comments describe) exists to prevent a use-after-free. In Model
B:

- **Two immutable layers, each built → sealed → published; neither mutated after
  seal (D12 as refined by D22).** Derivation builds into a **mutable
  `DerivedSceneBuilder`** (a COW view, below); the **config-independent phase B —
  realize/tessellate geometry, build the TLAS — runs on the builder**, which is then
  **sealed into an immutable `DerivedScene` value** owning realized geometry, the
  spatial index, and materials/lights-as-emitters (D22). **The config-dependent
  structures — light samplers (they depend on the integrator's light-sampling
  strategy) and photon maps (only for photon-consuming integrators) — are NOT in
  `DerivedScene`; they are built by `prepare(DerivedScene, EffectiveRenderConfig)`
  (D42 — the *resolved* config, not a raw request) into a sealed, immutable
  `PreparedRenderState` (D22)** (these moved *out* of the RayCaster, but per D22 land in
  `PreparedRenderState`, not `DerivedScene` as the earlier D12 wording said).
  **`prepare` is deterministic via the `samplingSeed` (D33/D40)** — same `PreparedStamp`
  → same photon maps — even though the subsequent *render* is only
  reproducible-within-MC-tolerance, not bit-identical (D40). *Only sealed values are ever
  published* at either layer, so what the renderer adopts is fully render-ready and
  self-contained. There is **no publish-then-build-phase-B** sequence at either layer.
  **A render-time integrator/camera override re-runs only `prepare` (a new
  `PreparedRenderState` from the same sealed `DerivedScene` — a different
  `effectiveRenderConfigHash` / `viewCameraStateHash`, D42), not the scene derivation
  (D22).**
- **COW is a reverse-dependency-closure copy, not "copy the leaf, share its
  referrers" (D11).** The engine scene is a raw-pointer graph (objects hold direct
  material/geometry pointers; materials hold direct painter pointers), so you
  **cannot share a referrer of a changed node** — a shared material would keep
  pointing at the *old* painter. A new version therefore copies the **reverse-
  dependency closure** of each changed node — the node **plus every node that
  transitively references it up to the roots** (managers / spatial index) — repoints
  the copies, and **shares everything outside the closure** by refcount. Cost is
  **O(closure / fan-in of the edited node), not O(scene)** — **gated on D23's
  persistent manager roots** (the name→entity maps must be persistent immutable
  containers, else copying a manager root is itself O(N) per snapshot): a
  material/light property copies `{material}` + the objects binding it + their TLAS
  leaves; a widely-shared painter copies its full referrer closure (larger, bounded
  by fan-in, still dwarfed by the ensuing render). **Render stays direct-pointer** —
  the closure copies hold correct pointers into the new snapshot's immutable objects.
  (Named, deferred optimization, D11: per-snapshot indirection tables for
  very-high-fan-in classes collapse the edit to O(log N) at a render-time lookup cost
  — adopt only if profiling shows closure-copy is the bottleneck. A first
  implementation **may** full-rebuild — closure = everything — for correctness, then
  add closure-tracking; closure-copy is the design *target*.) This is the red-green
  discipline (D2) extended from the derived scene.
  > **TLAS is full-rebuild in v1; incremental TLAS is a named future prerequisite
  > (D24).** The "transform one object → copy `{object}` + the TLAS spine to its leaf
  > (O(log N))" path-copy claim is **withdrawn for v1**: a `BVH<>` top-level structure
  > is not persistent, so **v1 fully rebuilds the TLAS on any
  > geometry/transform/structural change** — O(N log N), acceptable because it is
  > dwarfed by the render and edits are debounced. A **persistent BVH** (path-copy) or
  > **refit-with-periodic-rebuild** is the explicit, named future prerequisite for
  > cheap transform edits on very large scenes — not claimed for v1. Until it lands, a
  > transform edit's cost includes a TLAS rebuild (still « the render).
- The render thread holds a **refcounted pointer to one sealed
  `PreparedRenderState`** (`shared_ptr<const PreparedRenderState>`, D22), which in
  turn refcounts the sealed `DerivedScene` it was prepared from. A commit (or a
  gesture preview, §2.3) derives + seals a new `DerivedScene`, `prepare`s a new
  `PreparedRenderState`, and the render loop **atomically swaps that pointer at a
  PASS boundary** (D12 — never per-tile, which would mix versions in one frame). The
  old prepared state (and its `DerivedScene`) stays alive until the in-flight pass
  finishes. **No UAF is possible** — you cannot free what a worker holds a ref to,
  and **there is no parking-for-safety** (no thread reads an object another thread
  mutates — both sealed layers are immutable). **Render-local caches (irradiance,
  accumulation) the pass populates are the pass's own mutable scratch (D21), keyed to
  the prepared state they accelerate and discarded/invalidated when it is replaced.**
- **Cancel-and-park survives only as an optional latency optimization, never a
  correctness requirement (D1).** When a new version is published, we *may* still
  want to stop the now-stale pass promptly so the user sees their edit sooner (and
  to coalesce rapid edits). So `KickRender` may trip the cancel flag and the loop
  picks up the new snapshot. But we no longer need to *block the editing thread*
  until the pass drains — the edit already succeeded on an independent immutable
  structure. The `mMutex`-held-across-`Apply` pattern (today required for
  time-scrub) is **dropped**; the only shared state is "which snapshot should the
  loop render next," an atomic `shared_ptr` swap at a **pass boundary** (D12).
- **Two arbiter job classes — latest-wins preview vs stamp-pinned render (D43).** The
  cancel-and-park "newer head cancels the in-flight pass" policy above is the **preview**
  policy only. The orchestrator runs **two** job classes:
  - **Preview jobs — latest-wins.** A commit (and every mid-gesture `Stage`, §2.3)
    triggers an interactive viewport **preview** that tracks head; a newer head (or
    any newer requested stamp — a scrub, a camera move) **cancels** the in-flight
    preview (D34's policy). Previews are ephemeral, cheap, and **never pinned**.
  - **Pinned render jobs — stamp-pinned.** An explicit "render *this*" (final / export)
    is **pinned to its `requestedPreparedStamp`** (D38/D42) and a newer head does
    **NOT** cancel it — it runs to completion (or is cancelled only by its requester).
    This is the edit model's contract with the arbiter: an unrelated edit must never
    silently destroy a deliberately requested final render. The coordinator may **queue**
    pinned renders (one heavy render at a time) on the **one render slot** — latest-wins previews
    **suspend** while a pinned render owns it (D48, single-render invariant; no concurrency);
    a pinned render carries its full stamp so its output is labeled with the exact
    `PreparedStamp` it was for (and is "stale" only by DAG-ancestry against a *later*
    head, never silently discarded).
- **Abort needs no rollback (D1):** dropping an in-flight pass is free — nothing
  was half-mutated.
- **Last-good-scene (D1) is exactly the derived-stamp surface (D13/D29/D38):** the most
  recent **sealed `DerivedScene`** (and the `PreparedRenderState` prepared from it,
  D22) that derived without a hard error; immutable + refcounted, so the renderer
  simply keeps rendering it while a broken head is edited (pairs with
  derive-with-holes, Facet 2). In that state the **`publishedDerivedStamp.cstVersion`**
  is a strict **DAG ancestor** of `headVersion` (not `<`, D29), `publishedDerivedStamp
  != requestedDerivedStamp`, and `status == Error` with `diagnostics` — the session
  reports the lag truthfully (§2.1) rather than stamping the render with the broken head.
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
  run off-thread and publish a **sealed `DerivedScene`** (and its `prepare`d
  `PreparedRenderState`, D22) when ready (D12). The render thread only ever reads
  the immutable layers and swaps the `PreparedRenderState` at a **pass boundary**
  (D12). This is a clean three-party contract with one shared mutable cell (the head
  prepared-state pointer) instead of today's web of atomics + condvar + per-mutator
  park.

---

## 3. Delete / Evolve / Reuse — the deletion inventory

Precise fate of every component this facet supersedes (charter §7.3, L7).

### 3.1 DELETE (removed entirely; the concept ceases to exist)

| File / class / member | What it did | Why it dies |
|---|---|---|
| `SceneEdit.h` — entire `struct SceneEdit` (24 ops + all `prev*` fields + `CameraSnapshot`) | tagged-union inverse-edit record | replaced by `CstPatch` (forward-only, no prev capture). The whole *concept* of a per-op inverse record is gone. |
| `SceneEdit::CompositeBegin/End`, `IsCompositeMarker`, `mCompositeDepth` | bracket a gesture into one undo unit | a gesture is one version because `GestureBuffer` commits once (§2.3) |
| `SceneEdit::historySeq`, `capturedTargetSerial`, `ResolveTargetSerial`, the identity-serial guard | detect remove+re-add under a reused name | identity is the lineage **NodeId** (red layer / side-map, D9/D15), not the name; undo restores a whole consistent version, so "the instance under the name changed" cannot corrupt anything — there's no captured state being mis-applied. (The serial's *intent* — survive a renamed entity — is subsumed by NodeId-keyed identity.) |
| `EditHistory.{h,cpp}` — **entire class** (2 deques, `Push`, `PopForUndo/Redo`, `TrimToMax` nesting logic, `RestoreLast*`, `Snapshot/Restore*ForRollback`, `ClearRollbackSnapshots`, `NextSeq`/`PeekUndoSeq`/`MaxTrimmedSeq`/`DidTrim`, `mDirtyObjects`) | bounded undo/redo stacks + transaction stack snapshots | replaced by `VersionGraph` (DAG of immutable roots). Every stack-snapshot/restore/trim-nesting subtlety is moot. |
| `SceneEditor.cpp` — `Apply`/`Undo`/`Redo`, `CaptureForApply` (~30 gates), `ApplyForwardMutation`, `ApplyRevertMutation`, composite atomic-rollback walks, `ApplyObjectOpForward`, `RestoreObjectTransform`, `ApplyMaterialSlotByName`, `AggregateCompositeScope` | the mutate-live-scene engine + inverse replay | replaced by `ApplyPatch` (pure `Cst→Cst`, Facet 1+3) + derivation (Facet 2). Forward-only; no revert dispatcher. |
| `SceneEditor` — `Bump*` family (`BumpSceneLightGeneration{,IfEmitterSetChanged,IfMaterialEmits}`) + the `dynamic_cast<Implementation::Scene*>` light-gen plumbing | scattered light-topology invalidation (P-INVALIDATE) | invalidation becomes Facet 2's dependency-graph job, keyed off the `ChangeSet` (§2.7) |
| `SceneEditor` — `prev*` capture, `FindManagerName`/`FindMediumName`/`FindGeometryName` reverse-lookups, the borrowed manager pointers (`mMaterialManager`/`mShaderManager`/`mPainterManager`/`mScalarPainterManager`/`mJob`) used *for capture* | recover prior names for undo | no prior-state capture exists; patches reference a node by its **NodeId** (D9), resolved from a name-path at the API edge |
| `SceneEditor::DirtyScope` enum + `LastDirtyScope` + `MarkEditEntityDirty` per-op routing | tell the orchestrator how much to re-render | replaced by the `ChangeSet` (which nodes changed) — strictly more precise; the orchestrator/derivation decide cost from *what changed*, not a coarse 5-value enum |
| `SceneEditController` — `BeginTransaction`/`RollbackTransaction`/`EndTransaction`, `mTxnOpen`, `mTxnBaseline`, `CaptureEditorState`/`RestoreEditorState`, `EditorStateSnapshot` | transaction begin/rollback/commit + baseline snapshot | replaced by `GestureBuffer` lifecycle (`Stage`/`Commit`/`Abort`). The baseline-snapshot (P-STATE epicenter) is gone. |
| `SceneEditController` — `mGestureOpenedComposite`, `mScrubOpenedComposite`, `OnPointerDown`'s composite-open + the orphan-composite guards, `ForceCompositeDepthZero`, `IsCompositeOpen` | reconcile gesture lifecycle with composite markers | no composites; gesture lifecycle is `GestureBuffer` construct/commit/abort |
| `DirtyTracker.{h,cpp}` (the *interactive-editor* role), `mScaleFromAnchorSet`, `HasUnsavedChanges`, `FireDirtyChangedIfTransitioned`, `DirtyChangedFn` | "what's dirty since load/save" for the save engine + Save-button | the **in-process** signal becomes `headVersion != lastFlushedVersionId` (§2.6, D6/D13 concept 1). **Retained-in-spirit:** the *external*-change detection is **not** dropped — it re-emerges as the D6 load/flush fingerprint (prefilter + content hash, D17) + the D17 atomic save (§2.6, concept 2). **Caveat:** `DirtyTracker`/`SourceSpanIndex`/`OverrideSpanIndex`/`TransformSnapshot`/`SaveEngine` are co-owned with Facet 6 (migration) — they only fully die once Model B's CST round-trip + D17 atomic save replaces Model-A save. This facet *marks them dead*; Facet 6 sequences the removal. |
| `SaveEngine.{h,cpp}` Mode-A byte-splice + Mode-B `override_object` block (the "SaveEngine gap" L7) | preserve typed text while writing model deltas | INV-4 round-trip is now automatic (§2.4): serialize the green CST via the red cursor (D2), untouched nodes are byte-identical. The whole splice/override-block *mechanism* is obviated. **The `FileIdentity` external-mod guard's *intent* is retained** as the D6/D17 atomic save — temp-write → fsync → revalidate → rename (§2.6) — only its byte-splice mechanism dies. (Facet 6 owns the deletion timeline + scene-corpus migration.) |
| `tests/SceneEditTransactionTest.cpp`, `EditHistory`-specific tests, `SceneSnapshotTest`, `SamplerRebuildOnRestoreTest` (the snapshot/restore-rebuild path) | guard the deleted machinery | their invariants move to version-graph tests (§6); the *behaviors* (undo restores state, light samplers rebuild on relevant change) are re-tested against the new model |

### 3.2 EVOLVE (kept in spirit, reshaped)

| Component | Today | Becomes |
|---|---|---|
| `SceneEditController` pointer-event handlers (`OnPointerDown/Move/Up`, `OnTimeScrub*`, `BeginPropertyScrub`/`EndPropertyScrub`) | translate gestures → live mutations + composite brackets | translate gestures → `CstPatch`es staged into a `GestureBuffer` (each move advances the uncommitted head + an ephemeral preview snapshot; coalesce into one committed version at gesture end, D1/§2.3). The **gizmo pixel→world math** (`ProjectWorldToScreen_`, axis-velocity solves, `BuildGizmoHandles_`, `GizmoHandleAt`) is **reused verbatim** — it's a view-projection, model-agnostic. |
| `DropStaleSelection_` / `SelectionStillResolves` | re-validate selection after undo/redo (mutating controller members) | pure `resolves(selection, version)` (against the version's `greenRoot` + `identityRoot`, D26); selection lives in `SessionState`; re-run on every head change |
| `ResolveTargetSerial`'s *intent* (don't corrupt a renamed entity) | serial compare | subsumed by **NodeId** identity on immutable trees (D9; rename is a NodeId-preserving op, INV-5) |
| `EditLabel`/`OpName` (the "Undo Move sphere" tooltip strings) | `EditHistory::LabelForUndo`, `OpName` switch | `Version::label` set at commit; tooltip reads `versions[head].label` / `versions[lastChildOf].label` |
| The cancel-and-park orchestrator (`Start/Stop/RenderLoop/DoOneRenderPass/KickRender`, preview-scale, polish) | conflated into `SceneEditController`, parks to avoid UAF | **extracted into its own render-orchestrator component**; the **render arbiter** runs the **async expensive phase** — realize → seal → prepare → seal → render — as **cancellable phases** off the edit thread (D34), while the **bounded sync semantic phase** (parse→CST→ref-resolution→typecheck) stays on the edit thread (D39, backing validate + rename); parks for *latency* only; renders an immutable `PreparedRenderState` = `prepare(DerivedScene, EffectiveRenderConfig)` (D22/D42), **deterministic in `prepare`, render reproducible-within-tolerance** (D40); runs **two job classes — latest-wins previews (a newer head cancels) vs stamp-pinned explicit renders (a newer head does NOT cancel)** (D43); animation drives it as a sequence of per-frame, time-INTERVAL `DerivedScene([t0,t1])` snapshots (D21/D31, single-time in v1) and owns the render-local irradiance/accumulation caches (D21). Out of this facet's deletion scope but its coupling to edits is redefined here. |

### 3.3 REUSE (unchanged, depended upon)

| Component | Role in Model B |
|---|---|
| `ChunkDescriptorRegistry` + `IAsciiChunkParser::Describe()` (L6) | the schema for patch validation, CST node shape, and the dynamic UI. Reused as-is. |
| `*Introspection` (`Material`/`Media`/`Camera`/`Light`/`Object`/`Film`/`Rasterizer`) | today they read live objects for the panels; in Model B the panels read CST leaves directly via descriptors, so most introspection **shrinks**. Retained where it computes *derived* read-only display values (e.g. a camera's post-orbit eye) — those read the *derived scene*, not the CST. (Boundary owned with Facet 4.) |
| `CancellableProgressCallback` | the render orchestrator's cancel primitive — unchanged |
| Facet 1's `parse`/`serialize` (red-cursor walk over rope-backed children, D16), `DiffCst` (best-effort NodeId re-association via structural matching, invalidate-unmatched, D15), `ApplyPatch`-core, the green `NodeRef`, the lineage `NodeId` (red layer / side-map — owned per-`Version` as its `identityRoot`, D15/D26) + the separate content hash / derivation key (D15), `NodePath` (D9 addressing), and the **persistent immutable container** primitive (HAMT / persistent tree, D23) backing the identity side-map, version DAG, and derivation cache | the red-green substrate (D2, amended by D15/D16/D23/D26) this whole facet is built on |

---

## 4. Hard problems & open questions

1. **Re-parse identity stability — RESOLVED by D9, *scoped* by D15.** Identity is
   the red-layer side-map `NodeId` (D15), and the guarantee depends on *how* the
   change arrives (the §2.4 contract):
   - **Structured edit / `Kind::Rename` is exact.** A panel/agent **rename**
     (`objects/sphere` → `objects/ball`) is the first-class `Kind::Rename` patch
     (§2.2): it targets a *known* node, so it is **NodeId-preserving by
     construction** (rewrites the name token in place, same side-map `NodeId`) and
     rewrites every referrer found via the **traced `ReferenceUse` records (D14)** —
     the dependency tracer's resolved `{sourceValueNodeId, targetNodeId}` set, which
     captures dynamic refs (e.g. `timeline.element`/`.animation` strings whose
     target category is chosen by `element_type`) that `referenceCategories` cannot
     see. (`referenceCategories` is demoted to a UI-picker hint only, D4/D14.) For a
     referrer in a node that did **not** derive (e.g. inside an error subtree, so
     untraced), fall back to descriptor-provided reference resolvers; any referrer
     that still cannot be resolved is **flagged, never silently renamed.** Selection
     and durable agent references keyed on NodeId survive this path exactly.
     **The reference set comes from the ONE derivation resolver, at head (D25 amended
     by D35/D39):** rename rewrites referrers at `headVersion`, but the trace comes from
     derivation, whose `cstVersion` may be a DAG ancestor of head (D13/D29). A
     reference added in head-but-not-yet-derived would be missed. D25 required a
     head-stamped trace; **D35 sharpens it: there is exactly ONE resolution
     implementation — derivation's own evaluator — and rename reuses it, not a second
     "tracing pass" that could drift from real derivation.** So rename **runs the
     bounded synchronous semantic phase to head** (D39: parse→CST→bind→
     reference-resolution→typecheck — the *front of the async derivation job*, sharing
     that code, not a separate pass) and reads the resulting `ReferenceUse`. That phase
     loads no asset bytes and realizes nothing, so it is edit-thread-OK (D39); the async
     expensive phase is not triggered. Rename is infrequent, so a synchronous
     semantic-phase-to-head is acceptable. **If that phase fails to reach head (semantic
     error), rename is refused**, never run best-effort. So rename never silently leaves
     a dangling old name.
   - **Whole-region reparse is best-effort (D15) — NOT guaranteed.** When the same
     rename arrives as a *raw text edit*, the matcher tries to re-associate the new
     green node with the prior side-map `NodeId` by structural position + content,
     **but this is best-effort and cannot guarantee identity after an arbitrary text
     replacement.** Identical repeated rows are genuinely ambiguous; an unmatched (or
     ambiguously-matched) occurrence gets a **fresh `NodeId`, and any durable
     reference still bound to the old one is invalidated/flagged, not remapped.** The
     old "(a) drop the selection / (b) heuristic follow / (c) best-effort"
     trichotomy collapses to: **exact for structured edits, explicitly best-effort
     (with invalidate-on-miss) for whole-region reparse.** The residual cross-facet
     asks are narrow: Facet 1 owns the best-effort matcher + the
     invalidate-unmatched flag, and Facet 5 defines agent rename semantics on top of
     `Kind::Rename` (preferring the structured path precisely because reparse is not
     guaranteed).
2. **Whole-buffer reparse latency on huge scenes.** A 50k-line generated scene
   re-parsed on every text commit could blow the 20 ms budget. Mitigation:
   `ReparseSpan` (chunk-local reparse) — but that needs Facet 1 to map a byte
   range → the enclosing chunk node reliably. Open until Facet 1's parser
   incrementality is specced. **This is the second of the charter's two named
   tar-pits (derivation latency / lossless preservation) viewed from the edit
   side.**
3. **Derivation granularity vs `ChangeSet` precision.** My model's efficiency
   assumes Facet 2 can re-derive *just* the changed nodes' **reverse-dependency
   closure** (D11), not the whole world. If derivation has coarse dependencies (e.g.
   any light change rebuilds all light samplers — which may be *correct*, see the
   env-IBL/`LightSampler` coupling in the engine, which lives in
   `PreparedRenderState` per D22, rebuilt by `prepare`, not in the config-independent
   `DerivedScene`), the per-edit latency is bounded by that, not by my (D23-gated)
   O(log N) patch cost (D16). The editor's job ends at "here's the precise
   `ChangeSet`"; how finely Facet 2 acts on it is **the** cross-facet performance
   contract.
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
   per-single-root, undo single-rooted, and save one file (atomic save, D17). The earlier
   "one tree vs forest? version = set of roots?" question no longer exists. (If
   library-sharing demand returns, D7 routes it through a future declarative
   `import` chunk with its own scoping/fingerprint/versioning story — explicitly
   out of core v7, never the imperative `> load`.)

---

## 5. Cross-facet dependencies & assumptions

- **On Facet 1 (CST):** I assume the **red-green model (D2)** as amended by D15/D16:
  (a) an immutable, ref-counted/persistent **green** `NodeRef` tree whose nodes
  store **relative width** + typed content, **never absolute spans and never a
  `NodeId`** (D15 — the lineage `NodeId` lives in the **red layer / a side-map**),
  with **wide child sequences backed by a persistent rope** (D16) so position lookup
  and structural edit are O(log N); (b) **dual identity (D9)** — `NodeId` is the
  lineage identity (stable across structured edits, renames, and *best-effort* across
  reparses); `NodePath` is version-resolved addressing; (c) `parse`/`serialize`
  (the latter a **red-cursor** walk) with **byte-exact round-trip of untouched
  nodes** (the load-bearing INV-4 guarantee); (d) a `DiffCst(a,b) → ChangeSet` (of
  NodeIds) that compares shared subtrees by pointer and **re-associates prior NodeIds
  by best-effort structural matching, invalidating/flagging unmatched durable
  references** (D15 — a structured edit preserves NodeId exactly; whole-region
  reparse is best-effort); (e) a low-level persistent-update primitive for
  `ApplyPatch` to build on; (f) green leaves store the **verbatim author token**
  (resolves §4.5); (g) the **content hash** (trivia-sensitive, for sharing) is
  distinct from the **derivation key** (trivia-insensitive, Facet 2's memo key) which
  is distinct from the **lineage `NodeId`** (D15). Facet 1 owns the D7 single-file
  decision (no forest) and supplies the D17 load/flush fingerprint surface (prefilter
  + content hash). If Facet 1 chose a mutable CST with a separate snapshot mechanism,
  my version model would degrade to "snapshot per commit" — but D2 makes the
  red-green tree the ratified substrate, so this is settled, not open.
- **On Facet 2 (derivation):** I hand it `(newRoot, ChangeSet)` (NodeIds) — and, for
  an animation frame, the **active animation name + a shutter interval `[t0,t1]`**
  (D21/D31) — and expect an incremental `derive` that recomputes only dependent
  subgraphs (INV-3), and a **dependency model that owns light-topology/env-IBL
  invalidation** (absorbing P-INVALIDATE) keyed off the traced `ReferenceUse` graph
  (D14). Per **D1 as amended by D11/D12/D21/D22/D29–D34/D39–D42**, the render-ready
  scene is **two immutable, sealed layers**, each keyed by a **stamp** (D29): **`DerivedScene =
  f(CST, AssetManifest, animationName, shutter)`** (config-independent:
  realized/tessellated geometry, materials, lights-as-emitters, **TLAS**), keyed by
  **`DerivedStamp = { cstVersion, assetDigest, animationName, shutterInterval }`** (D29
  amended by **D41** — the asset axis is a **content digest of the exact loaded buffer**
  (load-and-hash one buffer, or revalidate-after-load), not a session generation
  counter), and **`PreparedRenderState = prepare(DerivedScene, EffectiveRenderConfig)`**
  (config-dependent: **light samplers + photon maps**, D22), keyed by **`PreparedStamp =
  DerivedStamp + { effectiveRenderConfigHash, viewCameraStateHash, samplingSeed }`** (D29
  amended by **D42/D45** — the config axis is the hash of `ResolveEffectiveRenderConfig(DerivedScene,
  request)` (run *after* DerivedScene; auto-route may probe) = scene-authored ← request overrides ← defaults ← auto-resolution, and the
  camera axis is a hash of the *complete* ephemeral view-camera state, **not** a raw
  `RenderConfig` / a `CameraId`). Motion blur is preserved by baking
  animated quantities as immutable `at(τ)` functions over the shutter (D31; v1 is
  single-time, motion BVH is gated). **`prepare` must be deterministic (D33/D40):** it
  takes the `samplingSeed`/RNG-stream identity from the `EffectiveRenderConfig` (no
  `rand()`-seeded photon tracing), so a `PreparedStamp` is a pure key and the
  `PreparedArtifact` cache is sound — **but the final render is only
  reproducible-within-MC-tolerance, NOT bit-identical** (D40: per-worker RNGs, tile
  assignment, splat reduction, denoise all vary; bit-identical rendering is a named
  future option). **`prepare` reads the sealed
  `DerivedScene` through non-mutating (const) input APIs and writes a separate
  `PreparedRenderStateBuilder` (D32)** — `BuildPendingPhotonMaps` and light-sampler
  construction are refactored from "mutate the `Scene`" to `build(const DerivedScene&,
  PreparedRenderStateBuilder&)` (named prerequisite work). The COW is a
  **reverse-dependency-closure copy** (D11 — you cannot share a referrer of a changed
  node in the engine's raw-pointer graph): only the changed node's closure (it + its
  transitive referrers up to the roots) is copied + repointed, everything else shared by
  refcount, render stays direct-pointer — **O(closure)/O(log N) only once the persistent
  manager roots (D23) land; O(N) per snapshot in the v1 copy-on-snapshot fallback; v1
  also full-rebuilds the TLAS (D24)**. Derivation builds the config-independent layer
  into a `DerivedSceneBuilder` (realize/tessellate + TLAS) and **seals**; `prepare`
  then builds light samplers + photon maps into a `PreparedRenderState` and **seals** —
  only sealed values are published. **Derivation splits into a bounded SYNC semantic
  phase and the async expensive phase (D39, refining D34).** The **synchronous semantic
  phase** (lex→parse→CST→bind→**reference resolution / traced `ReferenceUse`**→typecheck)
  is deterministic, loads no asset bytes, realizes nothing, and runs **on the edit
  thread** — it backs `propose_patch` precommit validation and rename (D35: rename runs
  *this* phase to head, sharing the code, not a second resolver). The **async expensive
  phase** (realize/tessellate → seal → prepare → seal → render) is D34's cancellable
  arbiter job, and the sync phase **is its front** (same code, no drift). So the edit
  thread runs only the cheap CST commit + (when needed) the bounded semantic phase; the
  arbiter runs the expensive phases off-thread; when a newer head arrives, in-flight
  expensive phases cancel and restart at the new stamp (the head-vs-derived lag source,
  D13/D29).
  The render thread holds a refcounted `PreparedRenderState` pointer and swaps at a
  **pass boundary** (D12 — never per-tile). **Caches populated *during* a pass
  (irradiance, accumulation) are render-local mutable scratch (D21), not part of either
  sealed layer.** Per **D20 as amended by D30** the derivation memo + dependency graph
  is **version-scoped/persistent** but lives on a **`DerivedArtifact` keyed by its
  `DerivedStamp`** (a stamp-keyed LRU, *not* on the immutable `Version`),
  structurally shared across versions/branches **via D23's persistent containers**,
  with an explicit per-node edge lifecycle (atomic edge-set replace on re-derive; purge
  edges + cache entry + flag dangling `ReferenceUse` on delete). **History is CST-only
  (D28):** re-deriving an old version re-reads the live asset bytes (manifest re-stamped
  on access → a different **`assetDigest`** axis, D29 amended by D41 — a content digest
  of the loaded buffer), so a changed asset may alter an old version's render — a
  content-addressed asset store is a future option. Conflict
  risk: if derivation is whole-world per edit, the latency budget (§2.8, ≤ D10 G2's
  50 ms ceiling) is Facet 2's to meet, not mine.
- **On Facet 4 (UI):** structured edits arrive as `CstPatch` through one API;
  `SessionState` (selection/tool/expansion) is owned jointly (it's UI state but I
  define the split). UI widget bindings and selection key on the **lineage `NodeId`**
  (red layer / side-map, D9/D15), addressed by name-path; a binding whose node is
  unmatched after a whole-region reparse is **invalidated/flagged**, not silently
  remapped (D15). The gizmo math is shared code.
- **On Facet 5 (agent):** the MCP server is a `GestureBuffer`/`Commit` client and
  a `VersionGraph` reader; structured errors come from `ApplyPatch`'s
  validation + the §2.6 conflict result. A patch's **optimistic-concurrency
  precondition is checked against `headVersion`** (D13). `read_document` is stamped
  with `headVersion`; `read_graph`/`render`/`derive_preview` with the **published
  `DerivedStamp`** (D13/D29/D38 — the status surface also exposes the *requested* stamps,
  and `propose_patch` precommit validation runs the bounded sync semantic phase, D39) —
  its `cstVersion` related to head by **DAG ancestry, not `<`** — never told
  they are equal when they are not. Durable agent references key on the **lineage
  `NodeId`** (D9/D15/D36) — agent `EditIntent`s carry the target `NodeId`; the agent
  prefers the structured/`Kind::Rename` path precisely because whole-region reparse
  identity is only best-effort (D15), and `Kind::Rename` reuses the one derivation
  resolver (D35). "GUI is just another agent" holds because both go through
  `ApplyPatch`.
- **Decision conformance (D1–D44).** This facet implements, from round 1: **D1**
  (immutable COW derived-scene snapshot + the single gesture model, §2.1/§2.3/§2.9),
  **D2** (red-green CST, §2.1/§2.8), **D6** (external-file conflict, §2.6), **D7**
  (single-file tree, not a forest, §2.8 + §4 item 7), and **D9** (dual identity,
  throughout). It **consumes** the round-1 remainder without conflict: **D3** (ordered
  children) — patches address ordered-child NodeIds; **D4** (traced deps) — referenced
  by `Kind::Rename` (via `ReferenceUse`, D14) and P-INVALIDATE; **D5** (`AssetManifest`
  fingerprint) — reused as the D6/D17 save fingerprint; **D8** (time-bounded v6) —
  this facet ships only the v7 path; **D10** (phased fixture + shared gates) — §6 maps
  to G1–G5. From **round 2** it implements/conforms: **D11** (COW = reverse-dependency-
  closure copy, §2.9), **D12** (build → phase-B → seal → publish; snapshot owns its
  render structures; adopt at a **pass** boundary, §2.3/§2.9), **D13** (coherent
  status exposing `headVersion` *and* the derived stamp — amended by D29, §2.1/§2.5/
  §2.6/§2.9), **D14**
  (rename via traced `ReferenceUse`, not `referenceCategories`, §2.2/§4.1), **D15**
  (content hash / derivation key / lineage `NodeId` separated; NodeId in the red layer;
  reparse best-effort + invalidate-unmatched, §2.1/§2.4/§4.1), **D16** (wide child
  sequences are ropes → O(log N), §2.1/§2.8), **D17** (fingerprint = prefilter +
  content hash; save = temp-write + fsync + revalidate + atomic rename, with the
  documented rename-race residual + opt-in advisory locking, §2.6); and it **consumes**
  **D18** (corrected first-slice fixture — §6 references D10's phases, which D18 amends),
  **D19** (no imperative `>` layer in v7 — this facet edits only declarative chunks;
  nothing here depends on `> set`/`> load`/`> run`), and **D20** (persistent
  derivation cache + edge lifecycle — amended by D30 so the cache is **artifact-scoped**
  (keyed by `DerivedStamp`), not on the `Version`; a Facet 2 contract this facet relies
  on, §5 Facet-2 bullet). From **round 3** it implements/conforms: **D21** (animation =
  per-frame derivation with time as a derivation input — amended by D31 to a time
  *interval*; render-populated caches — irradiance, accumulation — are render-local
  mutable, not snapshot state — §2.1 layered-model note / §2.3 / §2.6 table / §2.9),
  **D22** (split config-independent `DerivedScene` from `PreparedRenderState =
  prepare(DerivedScene, RenderConfig)`; light samplers + photon maps live in
  `PreparedRenderState`, not `DerivedScene`; the renderer adopts a `PreparedRenderState`
  at a pass boundary; a render-time integrator override re-runs only `prepare` — §2.1 /
  §2.3 / §2.9 / §5 Facet-2 bullet), **D23** (the version DAG and identity side-map (and,
  artifact-side, the derivation cache) require persistent immutable containers for
  O(log N) sharing — a named prerequisite; **all O(log N)/O(closure) claims here are
  gated on it, with an honest O(N) copy-on-snapshot v1 fallback** — §2.1 gating note /
  §2.5 / §2.8), **D24** (TLAS is full-rebuild in v1; incremental/persistent BVH is a
  named future prerequisite; the D11 "O(log N) TLAS path-copy" claim is withdrawn for v1
  — §2.9 COW bullet), **D25** (rename requires a **head-stamped** reference trace —
  amended by D35 to reuse the one derivation resolver, derive head — §2.2 / §4.1), and
  **D26** (every `Version` owns a persistent identity side-map, the `identityRoot` —
  amended by D30 so a `Version` is `{ greenRoot, identityRoot, metadata }` (the cache is
  NOT a Version root); the `identityRoot` is the per-occurrence→NodeId map reparse
  writes and bindings resolve against — §2.0 / §2.1 / §2.4 / §2.5 / §3.3 / §5 Facet-1
  bullet); and it **consumes** **D27** (no `>` commands in v7 — extends D19 to
  `> set <setting>` and the `> modify` forms; this facet edits only declarative chunks,
  so nothing here depends on any `>` command — note D37 corrects D27's census to ZERO
  active `> modify`, immaterial to this facet) and **D28** (history preserves the CST
  only; re-deriving an old version uses **current** asset bytes — an external asset
  change may alter the render; a content-addressed asset store is a future option —
  §2.5 re-derivation note / §2.6 / §5 Facet-2 bullet). **From round 4** it
  implements/conforms: **D29** (full `DerivedStamp = { cstVersion, assetManifestGen,
  animationName, shutterInterval }` and `PreparedStamp = DerivedStamp + { renderConfig,
  cameraOverride, samplingSeed }` — round 5 sharpens the axes: `assetManifestGen` →
  **`assetDigest`** (D41), `renderConfig`/`cameraOverride` → **`effectiveRenderConfigHash`**
  + **`viewCameraStateHash`** (D42); head-vs-derived staleness is **cstVersion DAG
  ancestry, not `<`** — §2.1 stamp/artifact model + `VersionStatus` / §2.5 / §5 Facet-2
  + Facet-5 bullets), **D30** (the derivation cache lives on a **`DerivedArtifact` keyed
  by `DerivedStamp`** — and a `PreparedArtifact` by `PreparedStamp` — in a stamp-keyed
  LRU, **not** on the immutable `Version`, which is `{ greenRoot, identityRoot,
  metadata }` — §2.1 `Version` struct + stamp/artifact model / §2.5 / §2.8 / §6),
  **D31** (animation is per-frame, time-**INTERVAL** derivation — a motion-blurred
  frame bakes animated quantities as immutable `at(τ)` over the shutter; animation name
  + shutter are `DerivedStamp` axes; v1 single-time, motion BVH gated — §2.1 diagram +
  animation bullet / §2.6 table / §3.2 orchestrator row / §5 Facet-2 bullet), **D34**
  (the edit thread only commits a CST `Version`; the **expensive** derivation phase —
  realize/TLAS/prepare/render — runs **async + cancellable on the render arbiter**, off
  the edit thread — the head-vs-derived lag source; **D39 carves out the bounded sync
  semantic phase that runs on the edit thread** — §2.1 two-phase bullets + `VersionStatus`
  / §2.3 / §3.2 orchestrator row / §5 Facet-2 bullet), **D35** (rename reuses the **one**
  derivation resolver — derive head (= the bounded sync semantic phase, D39), sharing that
  code, not a separate tracing-pass reimplementation; refuse if head can't be derived —
  §2.2 / §4.1), and **D36** (the `GestureBuffer`/working head
  carries **both** roots `{ greenRoot, identityRoot }` so insertions/reparses update
  occurrence identity mid-gesture; `CstPatch`/`EditIntent`, `Widget`/`ViewNode`, and
  selection carry the target **`NodeId`** — §2.2 `CstPatch.target` + `ApplyPatch` / §2.3
  `GestureBuffer` / §2.6 `SessionState`). It **consumes** the prepare-layer machinery
  decisions **D32** (`prepare` reads the sealed `DerivedScene` through non-mutating const
  APIs and writes a `PreparedRenderStateBuilder` — a Facet 2 refactor of
  `BuildPendingPhotonMaps`/light-sampler construction, §5 Facet-2 bullet) and **D33**
  (`prepare` is deterministic — the `EffectiveRenderConfig` carries the sampling seed /
  RNG-stream identity, part of the `PreparedStamp`; sharpened by D40 — *prepare*
  deterministic, *render* reproducible-within-tolerance — §2.1 / §5 Facet-2 bullet), and
  notes **D37** (the migrator must be comment/token-aware; active `> modify` = 0) as a
  migration-side correction immaterial to this facet's declarative-only edits. **From
  round 5** it implements/conforms: **D38** (the status surface carries **requested AND
  published** stamps — `requestedDerivedStamp`/`requestedPreparedStamp` +
  `published*` — and **`status:ok` requires full-stamp equality** on every axis; the edit
  thread sets the requested stamp on commit and whenever any input axis changes, the
  arbiter publishes — §2.1 `VersionStatus` / §2.9 last-good bullet), **D39** (derivation
  splits into a **bounded synchronous semantic phase** — parse→CST→reference-resolution
  →typecheck, deterministic + edit-thread-OK, backing `propose_patch` precommit and
  **rename** — and the **async expensive phase** — realize/TLAS/`prepare`/render — on the
  arbiter; the sync phase **is the front of the async job**, the same resolver, so D35's
  no-drift holds; "rename synchronously derives head" now reads "run the sync semantic
  phase to head", and the absolute "all derivation async" (D34) is corrected — §2.1
  two-phase bullets / §2.2 + §4.1 rename / §3.2 orchestrator row / §5 Facet-2 bullet),
  **D40** (the seed makes **`prepare` deterministic** while the **final render is
  reproducible *within MC tolerance*, not bit-identical**; bit-identical rendering is a
  named future option — §2.1 determinism bullet + `PreparedStamp` comment / §2.9 COW
  bullet / §5 Facet-2 bullet), **D41** (an asset binds to the stamp by the **content
  digest of the exact loaded buffer** — load-once-and-hash or revalidate-after-load; the
  `DerivedStamp` asset axis is **`assetDigest`**, not a session generation — §2.1
  `DerivedStamp` + the resolved-axes note / §2.5 re-derivation note / §5 Facet-2 bullet),
  **D42** (the `PreparedStamp` carries the resolved **`effectiveRenderConfigHash`** (from
  `ResolveEffectiveRenderConfig(DerivedScene, request)`) + a **`viewCameraStateHash`**, NOT a raw
  `RenderConfig` / a `CameraId` — §2.1 `PreparedStamp` + the resolved-axes note + layered
  model / §2.9 render-pass rule + COW bullet / §3.2 orchestrator row / §5 Facet-2 bullet),
  and **D43** (a commit triggers a **latest-wins preview** job — cancelled by a newer head
  — while an explicit render is a **stamp-pinned** job — NOT cancelled by a newer head;
  two arbiter job classes — §2.3 preview bullet / §2.9 two-job-classes bullet / §3.2
  orchestrator row). It **notes D44** (the locked charter L5/INV-5 is corrected so NodeId
  is lineage identity and name-path is addressing — this facet already conformed to
  D9/D15/D26/D36 throughout, so D44 only ratifies the charter to match; see the
  Locked-decision note below). **No contradiction with any of D1–D44** (a later round
  amends an earlier one; the amended forms are the ones implemented above).
- **Locked-decision conformance:** L2 (one pathway = `ApplyPatch`) ✔; L4
  (doc/session split, §2.6) ✔; **L5 + INV-5** (identity first-class — the immutable
  **`NodeId`** is the lineage identity; **name-path** is addressing — per D9/D15/D26/D36,
  and now ratified into the *locked charter itself* by **D44**, which corrects the
  pre-round-5 charter's stale "name-path is the stable identity currency": this facet
  already keyed every durable reference — selection, agent refs, UI bindings, undo
  lineage — on the `NodeId`, so D44 brings the charter into line with the design, not the
  reverse) ✔; L7 (supersession inventory, §3) ✔. **No conflict with any Locked
  decision** (D44 is the single round that edits the locked charter, and it does so to
  *match* this facet's long-standing NodeId-identity model).
- **Open-decision stances:** O1 — designed for lossless-CST-pivot; noted the
  text-canonical delta (the in-process dirty signal dissolves to one comparison;
  the D6 external-conflict guard remains regardless) in §2.6 / §2.8. O2 — designed
  for debounced-commit; showed the 60Hz delta is a pure scheduling knob (§2.3), the
  strongest possible robustness result for that decision.

---

## 6. First-slice implications (minimal end-to-end vertical)

This facet plugs into **D10's single phased fixture and shared gate set** (with the
phases as **corrected by D18** — gates G1–G5 unchanged) — it does **not** nominate
its own first slice. Phase 1 is `sphere_geometry` (2 scalar params); a **scalar
`SetField`** on it is the smallest end-to-end edit. The **first reference** arrives at
D18 phase 2 (`lambertian_material { reflectance <uniformcolor_painter> }`), and the
real **geometry→material→object** chain — with `standard_object` and **rename**
integrity (now via the traced `ReferenceUse` set, D14, exercised across refs) —
arrives at D18 phase 3, where this facet's NodeId-keyed identity and `Kind::Rename`
are exercised. An **asset-backed node** (D18 phase 6, `image_painter` or a mesh) is
what makes **G5** (AssetManifest fingerprint invalidation, D17) and the external-file
path actually testable. Below, each contribution is tagged with the D10 gate it
satisfies.

1. **Version graph + one commit.** `VersionGraph` holding immutable **two-root**
   versions (`greenRoot` + `identityRoot`, D2/D26/D30 — the derivation cache is NOT a
   `Version` root; it lives on a stamp-keyed `DerivedArtifact`, D30; `head` =
   `headVersion`, D13); `v0` = parse of the phase-1 `sphere_geometry` scene.
   `Undo`/`Redo` republish a prior version's two roots. *(This facet's irreducible
   core — buildable against a stub `NodeRef`; the v1 slice may use the **D23
   copy-on-snapshot fallback** for the identity side-map — O(N) per snapshot, with the
   O(log N) target gated on the persistent-container work.)* **→ D10 G4 (versioning).**
2. **One patch kind end-to-end.** `CstPatch{SetField, radiusNodeId, text}` (NodeId
   resolved from the name-path `geometry/dial_sphere.radius`, D9/D15/D26 — the
   `identityRoot` id) → `ApplyPatch` → `v1` (shares everything but the edited spine) →
   Facet 2 incrementally derives the one object's closure (D11) into a sealed
   `DerivedScene`, then `prepare`s a `PreparedRenderState` (D22) before the render
   kick (D12). **→ D10 G3 (minimal invalidation) + G2 (latency).**
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
6. **External inputs / save (D18 phase 6's asset-backed node).** When the fixture
   gains an asset reference (`image_painter` / mesh, D18 phase 6), an asset
   fingerprint change (prefilter + content hash, D17) re-derives its consumers (D5),
   and an external file change is caught at **save revalidate** (temp-write → fsync →
   revalidate → atomic rename, D17) before any overwrite. **→ D10 G5 (external
   inputs).**
7. **Two measured latencies** (§2.8, within D10 G2's 50 ms ceiling): structured
   `SetField` end-to-end (target ≤ 2 ms) and whole-buffer reparse+diff of the tiny
   scene (sanity for the 20 ms budget at scale). These two numbers are the facet's
   first-class perf output.

The first slice deliberately exercises **structured edit, text round-trip, undo,
and gesture-coalescing** on one chunk — every core claim of this facet, against
D10's shared gates, with the SaveEngine/EditHistory/transaction code provably
absent.
