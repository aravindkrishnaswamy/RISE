# RISE Agentic Redesign — Decision Record (review rounds 1–6)

> **Status:** authoritative. r1 → **D1–D10**; r2 → **D11–D20**; r3 → **D21–D28**; r4 → **D29–D37**;
> r5 → **D38–D44**; r6 → **D45–D51**. Later decisions *amend* earlier ones (r2: D11/D12 amend D1, D14
> amends D9, D15/D16 amend D2, D17 amends D5/D6, D18 amends D10, D20 amends D4; r3: D21/D22 amend D12,
> D22 amends D5, D23 amends D11/D20, D24 amends D11, D25 amends D14, D26 completes D15, D27 amends D19,
> D28 amends D5; r4: D29 amends D13/D22, D30 amends D26/D20, D31 amends D21, D32 amends D22/D12, D33
> amends D22, D34 amends D12/D22, D35 amends D25, D36 completes D26, **D37 corrects D27**; r5: D38
> amends D13/D29, D39 amends D34/D35/D5, D40 amends D33, D41 amends D5/D17, D42 amends D29/D22, D43
> amends D34, **D44 fixes the locked charter L5/INV-5**; r6: D45 amends D42, D46 amends D41/D5, D47/D48
> amend D43, D49/D50 amend D38, D51 amends D39 — D47/D51 also **supersede the legacy `docs/gui/`
> RENDER_COORDINATOR/AI_SECURITY specs** where they conflict). Where a decision conflicts with a facet
> doc (10–60), the overview (00), or the charter, **this document wins**, and a later decision wins
> over the earlier one it amends. Read after [`00-CHARTER.md`](00-CHARTER.md) and [`00-OVERVIEW.md`](00-OVERVIEW.md).
>
> **Enabling permission (from the product owner):** *"I am open to deprecating anything. All the
> scenes that have ever been produced live on this machine and we can migrate/change them if
> needed."* This is decisive for **D7** (single-file) and **D8** (time-bounded v6) — it removes the
> backward-compat constraints that forced the two most complex designs.

---

## D1 — Derived scene is an immutable COW snapshot; the renderer swaps snapshots (resolves P1-1)

**Contradiction:** F2 mutated one long-lived `Job`/`Scene` in place and parked the renderer for
safety; F3 created a new immutable `DerivedScene` per version and avoided parking; gesture preview
was described three different ways.

**Decision — one model:**
- The derived scene is an **immutable snapshot** (`DerivedScene`) with **structural sharing**. A new
  CST version derives a **new snapshot via copy-on-write**: only the changed subgraph's engine
  objects are re-derived (reusing the apply-layer on *copies*); unchanged objects (meshes, BVH leaves,
  materials, the TLAS where untouched) are **shared by reference** with the prior snapshot.
- The renderer holds a **refcounted pointer to one snapshot**. A commit/preview publishes a new
  snapshot and the render loop **atomically swaps the pointer at a tile/pass boundary**. The old
  snapshot stays alive (refcount) until the in-flight pass drains. **There is no parking-for-safety**
  (no thread reads an object another thread mutates — snapshots are immutable). Cancel-and-park
  survives only as an **optional latency optimization** (coalescing rapid edits), never a correctness
  requirement.
- **Gesture model (one description):** each pointer-move applies a CST patch advancing the
  **uncommitted head** (a persistent-tree root, D2) and derives a **cheap preview snapshot**
  (debounced per O2). Preview snapshots are **ephemeral** (not history versions). At **gesture end**
  the intermediate roots **coalesce into ONE committed version** (one undo unit, F3). The "working
  head" is CST state, not a side-channel of non-CST mutable state.
- **Last-good-scene:** trivially the most recent snapshot that derived without a hard error; it is
  immutable + refcounted, so the renderer simply keeps rendering it while a broken head is edited
  (pairs with derive-with-holes, F2 §5).
- **Abort:** dropping an in-flight pass needs no rollback — nothing was half-mutated.

**Rationale:** reconciles F2 (incremental, apply-layer reuse) with F3 (immutable, no parking) — the
apply-layer is reused, but on COW copies, so immutability + sharing both hold. This is the
red-green discipline (D2) extended from the CST to the derived scene.

**Overrides:** F2 §2.8 (in-place mutate + park) → COW snapshot; F3 §2.8/§2.x (no-park assertion is
*confirmed*, but clarified as COW-sharing, not full rebuild); F3 §gesture (the single model above).

---

## D2 — Red-green CST: immutable green tree (relative widths + NodeId), version-specific red overlay (resolves P1-2)

> **⚠ Amended by D15 + D16 (round 2):** the **NodeId is NOT stored in the (shared) green node** — it lives in each Version's persistent **`identityRoot`** side-map (a green node is content-addressed + shared across occurrences, so it carries no identity); and child sequences are a **persistent rope** giving **O(log N)**, not O(depth), for wide nodes. D15/D16 win where they differ.

**Contradiction:** absolute byte offsets stored in immutable shared nodes — a length change shifts
every later offset, forcing O(document) copy and contradicting O(depth) structural sharing.

**Decision:** adopt the **red-green tree** model (rust-analyzer / Roslyn):
- **Green nodes** are immutable, content-addressed, structurally shared. They store **relative width**
  (byte length of the node incl. its trivia), typed content, descriptor link, and a stable
  **NodeId** (D9) — **never an absolute offset**.
- **Absolute positions are computed on demand** by a **red cursor** that walks from a given version's
  root accumulating widths (O(depth)). The red layer is version-specific and cheap; it is *not*
  stored in the shared green nodes.
- An edit produces a new root by **path-copy** (O(depth) new green nodes; siblings/subtrees shared).
  No later node's stored data changes ⇒ sharing is real.
- **`CstSpan` (absolute byteBegin/byteEnd) and `ApplyOffsetDeltas` are DELETED** as stored node
  state. Diagnostics/edits that need an absolute position derive it via the red cursor.

**Rationale:** this is the canonical persistent-syntax-tree design and the only one consistent with
both INV-4 (lossless) and the O(depth) cost model F3 depends on. It also *simplifies* (no
offset-shifting machinery).

**Overrides:** F1 §2.4 (absolute `CstSpan` in nodes; incremental offset shifting) → relative width +
red overlay; the `SourceSpanIndex`/`ApplyOffsetDeltas` "evolve" rows become "delete" (their job is
the red cursor now).

---

## D3 — Ordered children are canonical; RepeatGroup is a derived view (resolves P1-3)

**Contradiction:** a `RepeatGroup` container that pulls all occurrences of a repeatable param out of
document order loses interleaving with comments/trivia/other params and breaks order-dependent
semantics (`timeline` value/time pairing; sticky `interpolator`).

**Decision:**
- **The canonical structure is the ordered child list.** Every parameter occurrence (every `part`,
  `cp`, `point`, `value`, `time`, `interpolator`, `shaderop`) is an ordinary `Parameter` node in the
  `Chunk`'s **document-ordered** children, interleaved with `Comment`/`Blank`/`Trivia` exactly as
  written. Order is never lost.
- **`RepeatGroup` becomes a derived, read-through index/view** ("all occurrences of param `X`",
  computed over the ordered children) for binding/validation/UI convenience. It does **not** replace
  or reorder the underlying nodes.
- **Addressing:** `geometry/dial_sdf.part[3]` = "the 4th occurrence of `part` in document order"
  (the view resolves the index to an ordered-children NodeId). Order-paired semantics (timeline) and
  sticky semantics (interpolator) are preserved because derivation reads the ordered children.

**Overrides:** F1 §2.2 (`RepeatGroup` as a structural container kind) → derived view over ordered
`Parameter` children.

---

## D4 — Dependencies are *traced during derivation*; memo key = node structure + traced-input versions (resolves P1-4)

**Contradiction:** (a) the dep graph was claimed derivable from `referenceCategories` + 3 special
edges, but `timeline.element`/`.animation` are plain strings whose target category is chosen
dynamically by `element_type` — invisible to that scheme; (b) hashing only *resolved values* makes
`expr(A)` ≡ literal `5`, hiding the differing future-invalidation dependency.

**Decision:**
- **Dependency edges are recorded as a by-product of derivation, not pre-computed statically.** When
  `derive()` resolves a name / reads another node / loads an asset, it **records the edge it
  traversed** (the reactive-build / "tracing" model). Dynamic references (`timeline.element` resolved
  via `element_type`, `timeline.animation`) are captured **automatically** because derivation
  actually performs the resolution.
- **`referenceCategories` is demoted to a UI/rename *hint*** (drives pickers + rename's referrer
  search), **not** the source of truth for the dependency graph.
- **The memo/semantic key of a derived node = (its green-node structural hash) + (the identities &
  versions of every traced input)** — CST inputs *and* asset inputs (D5). Therefore `expr(A)` keys on
  "structure=expr + input A@version"; literal `5` keys on "structure=literal 5, no inputs". When `A`
  changes, `expr(A)`'s key changes (input version bumped) and re-derives; literal `5` stays a cache
  hit. Equal *current values* never collapse unequal *dependency identities*.

**Overrides:** F2 §2.x graph-from-`referenceCategories`+3-edges → traced dependencies; F2 §2.x memo
key "hash resolved values" → "structural hash + traced-input versions".

---

## D5 — Derivation input = (CST, AssetManifest); output paths excluded (resolves P1-5)

**Contradiction:** `Scene = f(CST)` is false when a referenced texture/mesh/spectral/glTF changes
on disk without the CST or filename changing — a clean derive then disagrees with a cache hit.

**Decision:**
- The formal derivation input is **`(CST, AssetManifest)`**. The **`AssetManifest`** maps each
  referenced asset path → **{resolved absolute identity, content fingerprint (size+mtime, upgradable
  to a content hash)}**.
- Asset reads are **traced** (D4), so an asset's fingerprint participates in the memo keys of the
  nodes that consumed it. A fingerprint change invalidates exactly those nodes.
- Invalidation is driven by a **file watcher** (or an explicit re-stat on focus/render). The manifest
  is part of the derivation environment, versioned alongside the CST head.
- **Output paths** (`file_rasterizeroutput.pattern` and any sink) are **explicitly excluded** from
  the input dependency set — they are sinks, not sources.

**Overrides:** F1 §2.10 "opaque assets handled by reference" → reference **plus** an AssetManifest
entry that is a first-class derivation input; F2's input model gains the manifest.

---

## D6 — External-file conflict protection: load/flush fingerprint + compare-and-swap save (resolves P1-6)

**Contradiction:** in-process version-ID dirtiness can't detect that git/another editor changed the
file on disk; autosave could silently overwrite an external change.

**Decision:** two *independent* dirtiness concepts, both required:
- **In-process:** head-version-id ≠ last-flushed-version-id (the "unsaved changes" signal, F3).
- **On-disk:** record the file's **content fingerprint at load and at each flush.** Before writing,
  **compare-and-swap**: if the on-disk fingerprint ≠ the last-known fingerprint, the file changed
  externally → **do not silently overwrite.** Surface a conflict with explicit choices: **reload**
  (discard in-process head), **diff/merge**, or **force-overwrite**. After a successful flush, update
  the stored fingerprint.
- A background watcher (D5's mechanism) can surface "file changed on disk" proactively.

**Overrides:** F1 §3 SaveEngine-deletion row and F3's dirty model gain the on-disk fingerprint + CAS
save contract (the `FileIdentity` external-mod guard's *intent* is retained even though its
byte-splice *mechanism* is deleted).

---

## D7 — v7 is single-file; deprecate `> load` / `> run` (resolves P1-7) — uses the deprecation permission

**Contradiction:** multi-file (`> load`/`> run`, used by 204 files) had no agreed version-identity,
cross-root undo, atomic multi-file save, include-cycle, or shared-include-update model; F1/F3/F6
disagreed on tree-vs-forest.

**Decision (enabled by the owner's migrate-everything permission):**
- **A v7 document is a single self-contained file.** `> load` and `> run` are **deprecated and
  removed** from v7. The migrator **flattens** every include/run by inlining the referenced content
  into the consuming document (e.g. the shared `standard_colors` painters are inlined where used).
- This **dissolves** the entire multi-file problem set: version identity is per single root; undo is
  single-rooted; save is one file (CAS per D6); there are no include cycles, no shared-include
  fan-out, no cross-file edit semantics.
- **Cost accepted:** some duplication of previously-shared content across scenes. The owner controls
  the whole corpus and prefers the simplification; duplication in a generated/migrated corpus is
  cheap and diff-visible.
- **Future option (explicitly OUT of core v7):** if library-sharing demand returns, add a
  **declarative `import` chunk** with explicit lexical scoping, an AssetManifest-style fingerprint,
  and a defined versioning/undo story — designed as its own feature, never the imperative
  thread-local-resetting `> load`.

**Overrides:** F1 §2.8.4 (multi-file CST / `FileId` sub-documents) → removed from v7 (kept only in the
one-shot migrator that reads v6); F3's tree-vs-forest open question → **tree** (single root); F6's
"cross-file edits become normal" → removed.

---

## D8 — Time-bounded v6 migration, then delete the v6 path (resolves P1-8) — uses the deprecation permission

**Contradiction:** F1/overview promised "v6 loads forever / coexists indefinitely" while F6 deleted
the v6 parser after a deprecation window.

**Decision (enabled by the migrate-everything permission): time-bounded, not permanent.**
- The v6 parser + macro preprocessor are kept **only long enough to migrate the corpus** with the
  one-shot migrator, then **DELETED**. v7 is the **sole runtime format** after migration.
- **There are NO "legacy nodes."** F1 §2.8.3's `LegacyForLoop`/`LegacyMacroDef`/`LegacyExprValue`/
  `LegacyMacroRef` and the relocated legacy-expansion derivation pre-pass are **removed from the
  design** — the runtime never derives a v6 construct. `FOR`/`DEFINE`/`$(...)`/`hal` exist only as
  *inputs to the migrator*, which emits `instance_array`/`let`/`expr(...)`/`halton(dim,idx)`.
- The migrator is a build/CI gate over `scenes/` with render-equivalence acceptance (D5/D10 gates);
  once green, the v6 reader is dropped.

**Rationale:** carrying a second parser + legacy-derivation forever is pure complexity with no payoff
when we own and can migrate every scene. This is a large simplification of F1 (no legacy-node
machinery, no dual derivation path).

**Overrides:** F1 §2.8.3 + §6.1 "coexists indefinitely" → migrate-then-delete; overview §4.4 → same;
F6 §"deletion phase" → confirmed and made the single contract.

---

## D9 — Dual identity: immutable NodeId (lineage) + name-path (addressing) (resolves P2-1)

**Contradiction:** name-path changes on rename and positional indices shift on insert, yet the
agent/UI contracts called identities "stable."

**Decision:**
- **Internal `NodeId`** (a green-node-borne, immutable, process-stable token) is the **lineage
  identity.** It survives renames, value edits, and reparses, and is what undo lineage, UI widget
  bindings, and durable agent references key on.
- **name-path** (`objects/sphere.material`) is the **human/agent addressing scheme**, resolved to a
  `NodeId` **within a given version.** It changes on rename (by design — it's a name).
- **Rename** is a `NodeId`-preserving edit op: it rewrites the name token in place (same NodeId) and
  rewrites all referrers (found via the `referenceCategories` hint, D4). UI/agent bindings keyed on
  NodeId survive automatically.
- **Reparse matching:** when a text edit reparses a region, new green nodes are **matched to prior
  NodeIds by structural position + content** (rust-analyzer-style node reuse) so a text edit does not
  reset identities and break bindings/lineage. Unmatched nodes get fresh NodeIds.

**Overrides:** F1 §2.5 (name-path as *the* identity) → name-path is addressing; NodeId is identity;
F6 §identity open-decision → resolved here; F4 binds widgets to NodeId (addressed by name-path).

---

## D10 — One phased first-slice fixture + one shared gate set (resolves P2-2)

**Contradiction:** four different first slices were nominated across docs.

**Decision — one canonical fixture, phased; one gate set referenced by all docs:**

**Phased fixture (each phase is additive; the prior phase's gates keep passing):**
1. **`sphere_geometry`** — simplest chunk (2 scalar params, no refs/repeats/exprs).
2. **`+ uniformcolor_painter`** — introduces a color value + a **reference** + the ref-picker widget.
3. **`+ standard_object`** (the geom+material+object **three-node chain**) — cross-node references,
   **rename** integrity (D9), the dependency graph (D4) end-to-end.
4. **`+ expr(...)`** on one `Double` param — the expression sublanguage + traced-input invalidation.
5. **`+ instance_array`** replacing a nested `FOR` (migrated `loops.RISEscene`) — generators.

**Shared gates (every phase must pass all that apply):**
- **G1 Round-trip identity:** `parse → serialize` is **byte-for-byte identical** on a hand-formatted
  fixture (tabs, trailing comment, blank line). *(The non-negotiable correctness gate.)*
- **G2 Incremental-derive latency:** a single-parameter edit re-derives in **< 50 ms** on a
  Sponza-class scene (≈155 meshes) — i.e. derive is negligible vs the render-kick. (Preview render
  latency is the rasterizer's; G2 bounds *derivation*.)
- **G3 Minimal invalidation:** the edit re-derives **only** the changed node's forward cone (assert
  the touched-object set; assert TLAS/light-sampler/photon flags flip only when warranted).
- **G4 Versioning:** undo/redo is a version-DAG pointer move; a gesture is one undo unit; round-trip
  after undo is byte-identical to pre-edit (G1 again).
- **G5 External inputs (phases ≥2):** an asset fingerprint change (D5) re-derives its consumers; an
  external file change is caught by CAS save (D6).

**Overrides:** overview §6 + F2 §6 + F6 §first-slice → all reference this single definition.

---

## Net effect on complexity

The owner's deprecation permission turns the two worst findings into **deletions**: D7 removes the
multi-file subsystem; D8 removes the legacy-node/dual-parser subsystem. D2 (red-green) and D1
(COW snapshot) make the persistent model *actually* O(depth) instead of contradictory. D4/D5 make
derivation correct under dynamic refs and external assets. D6/D9/D10 close the remaining gaps. The
design is **smaller** after this round, not larger.

---

# Review Round 2 (D11–D20)

Round 2 stress-tested the *mechanics*. No P0s; 8 P1 + a P2 batch. Several amend round-1 decisions.

## D11 — Derived-scene COW is reverse-dependency-closure copy (amends D1; resolves R2-P1-1)

**Contradiction:** the engine's scene is a raw-pointer graph (materials hold direct painter pointers;
objects hold direct material/geometry pointers — `LambertianBRDF.h:37`, `Object.h:34`). D1's example
"copy a painter, share the materials" is **wrong**: a shared material keeps pointing at the *old*
painter. You cannot share a referrer of a changed node.

**Decision:** the derived scene is a DAG of immutable, refcounted nodes; a new version **copies the
reverse-dependency closure** of each changed node — the changed node **plus every node that
transitively references it, up to the roots** (managers / spatial index) — repointing the copies,
and **shares everything outside the closure** by refcount. Cost is **O(closure / fan-in of the
edited node), not O(scene)**:
- transform one object → closure ≈ {object} + path-copy of the TLAS spine to its leaf (O(log N));
- one material/light property → {material} + the objects binding it + their TLAS leaves;
- a widely-shared painter → its full referrer closure (larger, but bounded by fan-in and still
  dwarfed by the ensuing render). The corrected example must say exactly this.
- **Render stays direct-pointer** (closure copies hold correct pointers into the new snapshot's
  immutable objects). 
- **Optimization (named, deferred):** for very-high-fan-in classes (painters, materials),
  *per-snapshot indirection tables* (objects reference by id, resolved through the snapshot's table)
  collapse the edit to O(log N) at the cost of a render-time lookup — adopt only if profiling shows
  closure-copy is a bottleneck. **First implementation may use full-rebuild (closure = everything)**
  for correctness, then add closure-tracking — the design *target* is closure-copy.

**Overrides:** F2's O(1)-painter example (§ ~810) and any "share a referrer of a changed node" claim.

## D12 — Build → phase-B → seal → publish; the snapshot owns its render structures; adopt at a PASS boundary (amends D1; resolves R2-P1-2)

**Contradiction:** F2 published the snapshot and *then* ran phase B (realize geometry, build TLAS,
light samplers, photon maps) — mutation after publication. Light samplers are currently
**RayCaster-owned**, not part of `Scene` (`Scene.h:405`). Tile-level adoption would mix versions in
one frame.

**Decision:**
- Derivation builds into a **mutable `DerivedSceneBuilder`** (a COW view per D11). Phase B
  (realize/tessellate, TLAS, **light samplers, photon maps**) runs **on the builder**. Then **seal**
  → an immutable `DerivedScene` **value**. Only the sealed value is ever published. **No mutation
  after publication.**
- The sealed `DerivedScene` **owns** the realized geometry, spatial index, **light samplers, AND
  photon maps** (moved *into* the snapshot from the RayCaster, so a snapshot is fully render-ready
  and self-contained).
- The render loop **adopts a new snapshot only at a PASS boundary** (never mid-frame / per-tile). The
  prior snapshot drains by refcount.

**Overrides:** F2 §2.8 publish-before-phase-B sequence; the "tile/pass boundary" wording everywhere →
**pass boundary only**; sampler/photon ownership moves into `DerivedScene`.

## D13 — Coherent version status; expose head AND derived versions (resolves R2-P1-3)

> **⚠ Amended by D29 (round 4):** the single `derivedVersion` becomes a full `DerivedStamp`/`PreparedStamp`, and head-vs-derived is compared by **version-DAG ancestry, not `<`**. D29 wins where they differ.

**Contradiction:** `read_document` and `read_graph`/render all stamp the same `documentId`, but
derivation may lag (async) or serve a last-good snapshot. When head N is broken/deriving while the
graph/render is at N−1, a single stamp is false.

**Decision:** the session publishes one coherent status value:
`{ headVersion, derivedVersion, snapshot, status ∈ {deriving, ok, error}, diagnostics }`.
- `read_document` is stamped with **`headVersion`** (the CST truth).
- `read_graph` / `render` / `derive_preview` are stamped with **`derivedVersion`** (what the scene
  reflects — may be < headVersion, or last-good on error).
- Clients are never told the two are equal when they are not; `status` + `diagnostics` explain a lag
  or failure. A patch's precondition (optimistic concurrency, F5) is checked against `headVersion`.

**Overrides:** F5 §2 snapshot/`documentId` contract; F2's last-good description; F3's version surface.

## D14 — Rename uses traced `ReferenceUse` records, not `referenceCategories` (amends D9; resolves R2-P1-4)

**Contradiction:** D4 demoted `referenceCategories` (can't represent dynamic refs); D9 then used it
to find rename referrers. `timeline.element`/`.animation` are plain strings whose target category is
chosen by `element_type` — invisible to `referenceCategories`, so rename would silently leave them
pointing at the old name.

**Decision:** derivation's dependency tracing (D4) records, for every resolved reference, a
**`ReferenceUse { sourceValueNodeId, targetNodeId }`**. **Rename rewrites referrers from the traced
`ReferenceUse` set** (which captures dynamic refs automatically), not from `referenceCategories`
(which remains a UI-picker hint only). For references in nodes that did **not** derive (e.g. inside
an error subtree, so untraced), fall back to descriptor-provided **reference resolvers**; any
referrer that cannot be resolved is **surfaced/flagged**, never silently renamed. This unifies D4 and
D9 on the one traced reference graph.

**Overrides:** D9's "found via the `referenceCategories` hint" → "found via traced `ReferenceUse`
records (+ descriptor resolver fallback, flag the unresolvable)".

## D15 — Separate three concepts: content hash / derivation key / lineage identity (amends D2, D9; resolves R2-P1-5)

**Contradiction:** a lossless content-addressed green node includes trivia (so whitespace changes its
content hash); a unique per-node `NodeId` prevents identical syntax from being content-addressed
(shared); yet F2 needs a *trivia-insensitive* derivation key. These were conflated.

**Decision — three distinct things:**
1. **Content hash (green, lossless, trivia-sensitive):** hash of the green node's exact bytes incl.
   trivia. Purpose: **structural sharing/dedup** of byte-identical green subtrees. Carries **no
   identity** (so identical subtrees share one green node).
2. **Derivation key (semantic, trivia-INsensitive):** a hash of the node's *meaning* (typed values +
   child structure, **excluding** comments/whitespace/trivia) **+ traced-input versions** (D4).
   Purpose: the **memo cache** — a whitespace-only edit is a derivation cache **hit**.
3. **Lineage identity (`NodeId`):** a per-**occurrence** stable id living in the **red layer / a
   side-map, NOT in the shared green node** (a shared green node is reused at many occurrences, so it
   cannot carry one id). Purpose: UI binding, undo lineage, durable agent refs, rename.

**Reparse identity is best-effort, explicitly:** a **structured edit preserves `NodeId` exactly**
(it targets a known node). A **whole-region reparse** matches new green nodes to prior `NodeId`s by
position + content, **but this is best-effort** — identical repeated rows are genuinely ambiguous;
**unmatched durable references are INVALIDATED (flagged), not silently remapped.**

**Overrides:** D2's "green node stores … a stable NodeId" → NodeId lives in the red layer; F1 §2.5 /
§green-node definition; F2's "whitespace-insensitive key" is the *derivation key* (#2), distinct from
the content hash (#1); D9's "reparse matches" gains the best-effort + invalidate-unmatched contract.

## D16 — Wide child sequences use a persistent balanced sequence (rope), not vectors (amends D2; resolves R2-P1-6)

**Contradiction:** "red cursor is O(depth)" is false with vector children — locating child *k* or
computing its offset scans preceding siblings; a `Document` with 10 000 top-level chunks is **O(N)**,
and prefix-offset arrays make *edits* O(N).

**Decision:** a node's child list is a **persistent balanced sequence / rope**, with each subtree
caching **aggregate byte-width and newline counts**. This gives **O(log N)** position lookup
(byteBegin of child *k*, byte→node) **and O(log N)** structural edit (insert/remove a child),
preserving structural sharing. The cost claim becomes **O(depth · log(width)) ≈ O(log N)**, not
O(depth). Narrow lists (a chunk's handful of params) may stay vectors (small N); the **Document's
chunk list and any large `RepeatGroup`/heightfield (e.g. 10 000 `part`s) use the rope.**

**Overrides:** F1 §2.2/§2.4 "ordered vector" + "O(depth)" → persistent balanced sequence + O(log N).

> **Implementation footnote (item-4 identity layer):** this O(log N) is the **rope child-list
> primitive** — locating/inserting/removing a child. The item-4 identity layer adds a per-item
> order-maintenance LABEL (so a durable NodeId → current position is O(log N)); a label-gap-exhausting
> insert triggers a *windowed* reflow, making the combined `DocInsertItem` **O(log² N) amortized**, not
> O(log N) worst-case. Bender's two-level order-maintenance (window → O(1) amortized) restores O(log N)
> and is the documented refinement. See `docs/agentic-redesign/IMPLEMENTATION_SLICES.md` item 4.

## D17 — Asset fingerprint = prefilter + content hash; save = temp-write + fsync + revalidate + atomic rename (amends D5, D6; resolves R2-P1-7)

**Contradiction:** (a) size+mtime can be unchanged when bytes change — not deterministic for asset
invalidation; (b) the "CAS" save is actually stat-then-write (TOCTOU race between check and write).

**Decision:**
- **Asset identity** = **(size, mtime) as a fast prefilter** → on a prefilter change (or whenever
  determinism is required) compute a **content hash**, which is the authoritative identity in the
  AssetManifest (D5). Memo keys use the content hash.
- **Save is atomic:** write to a **temp file in the target dir → fsync → revalidate the target's
  content hash == the loaded fingerprint → atomic `rename()` over the target.** This replaces D6's
  stat-then-write. **Document the residual:** a non-cooperating concurrent writer can still race the
  final rename (last-writer-wins at the FS layer); offer **advisory file locking** as an opt-in for
  shared-storage setups. A fingerprint mismatch at revalidate → the D6 conflict UX (reload / diff /
  force), never a silent overwrite.

**Overrides:** D5 fingerprint definition; D6 "compare-and-swap" mechanism → temp+fsync+revalidate+
rename with documented residual.

## D18 — Corrected first-slice fixture (real reference chain + an asset phase) (amends D10; resolves R2-P1-8)

**Contradiction:** `uniformcolor_painter` has **no reference** (can't test a ref-picker); phase 3's
"geometry→material→object chain" **omitted the material node**; G5 (asset invalidation) had no
asset-backed node.

**Decision — corrected phased fixture:**
1. `sphere_geometry` — simplest chunk (no refs).
2. `+ uniformcolor_painter` **and** `+ lambertian_material { reflectance <the uniform painter> }` —
   the material is the **first reference** (→ exercises the ref-picker + the dependency edge).
3. `+ standard_object { geometry <sphere>  material <lambertian> }` — the real
   **geometry→material→object** chain; exercises rename integrity (D14) across refs.
4. `+ expr(...)` on one `Double` param — expression sublanguage + traced-input invalidation.
5. `+ instance_array` replacing a nested `FOR`.
6. **`+ image_painter` (or a mesh-backed geometry)** — an **asset-backed node** so **G5**
   (AssetManifest fingerprint invalidation, D17) and the external-file-conflict path (D6/D17) are
   actually testable.

Gates G1–G5 from D10 are unchanged; phase 6 is what makes G5 exercisable.

**Overrides:** D10 fixture phases (gates unchanged).

## D19 — Remove ALL embedded `>` commands; `> set` forms become declarative chunks (resolves P2-1; uses the deprecation permission)

**Contradiction:** `> set` (accelerator / global-medium / …) is imperative and non-descriptor, but F2
needs accelerator & global-medium configuration to be **declarative derivation inputs**.

**Decision:** migrate the three surviving `> set` forms to **normal descriptor-driven chunks** (e.g.
an `acceleration { … }` chunk, a `global_medium { … }` chunk) and **remove embedded runtime `>`
commands from v7 entirely** (`> load`/`> run` already gone per D7). v7 has **no imperative command
layer at all** — everything is a declarative chunk the migrator emits. Cleaner derivation (all config
is graph input) and one less language layer.

**Overrides:** F1's `Command` node kind (§2.8.4) — removed from v7 (migrator-only); F6 inventory.

## D20 — Derivation cache is version-scoped/persistent; explicit dependency-edge lifecycle (amends D4; resolves P2-3, P2-4)

**Contradiction:** (P2-4) a single `DerivationCache<NodeId,…>` cannot represent divergent branches or
simultaneous committed/preview versions; (P2-3) traced reruns need a defined edge lifecycle.

**Decision:**
- The derivation cache (memo + dependency graph) is **version-scoped and persistent**, structurally
  shared across versions like the green tree — **carried alongside each derived snapshot**, not a
  single global mutable map. Branches and a committed-vs-preview pair each see their own consistent
  cache view (sharing unchanged entries).
- **Edge lifecycle:** on re-deriving a node, **atomically replace that node's outgoing edge set**
  (drop stale edges, add freshly-traced ones) so a removed input dependency disappears. On **deleting
  a node**, purge its edges *and* its cache entry (and surface any now-dangling `ReferenceUse`, D14).

**Overrides:** F2's single-cache assumption + the unspecified edge lifecycle.

---

# Residual conformance sweep (round-2 P2-2)

The round-1 conformance left three superseded contracts that must be fixed in the facet docs (not new
decisions — cleanup): **(i)** the overview still implies *resolved-value* memo keys → fix to D4/D15
(structural + traced-input key); **(ii)** F2 still has *commit-only* derivation language in places →
reconcile with D1's uncommitted preview snapshots (gesture previews derive uncommitted heads);
**(iii)** F4/F5 still call **name-path a "stable identity"** in spots → fix to D9/D15 (NodeId is the
stable lineage identity; name-path is addressing). These are propagated in the round-2 doc pass.

# Net effect (round 2)

Round 2 made the mechanics *correct* rather than smaller: COW is now closure-copy (D11) built→sealed
before publish (D12); the tree is a persistent rope with three separated hashes/ids (D15/D16); rename,
assets, saves, status, and the cache all get precise contracts (D13/D14/D17/D20); and D19 deletes the
last imperative language layer. The first slice (D18) can now actually exercise its gates.

---

# Review Round 3 (D21–D28)

Round 3 tested the model against **existing engine capabilities** (animation, irradiance caching,
render config) and against whether the **data structures** can deliver the claimed complexity. No
P0s; 8 P1. Several are "downgrade the over-claim + name the prerequisite"; two (D21, D22) refine the
derived-scene model. Net layered model after this round:

```
CST (persistent red-green + per-Version identity side-map, D26)
  + AssetManifest (path → (size,mtime) prefilter → content hash)
  + time t                       ← animation input (D21)
        │ derive   — config-INDEPENDENT; manager roots are persistent maps (D23)
        ▼
DerivedScene(CST, assets, t)     immutable: realized geometry, materials, lights-as-emitters, TLAS
        │ prepare(scene, RenderConfig)   — config-DEPENDENT (D22)
        ▼
PreparedRenderState              immutable: light samplers, photon maps (integrator-specific)
        │ render(RenderConfig)           — render-LOCAL MUTABLE scratch (D21): irradiance cache, accum
        ▼
Image
```

## D21 — Animation = per-frame derivation; render-populated caches are render-local mutable (amends D12; resolves R3-P1-1)

> **⚠ Amended by D31 (round 4):** a motion-blurred frame's `DerivedScene` is **time-INTERVAL-parameterized**, not a single-time `DerivedScene(t)`. D31 wins where they differ.

**Contradiction:** existing animation **mutates** scene objects / TLAS / photon maps / irradiance
caches *during* rendering (`Scene.cpp:561`, `PixelBasedRasterizerHelper.cpp:1887`) — incompatible
with sealed immutable snapshots (D12).

**Decision (keep both capabilities; re-express them):**
- **Animation is per-frame derivation.** **Time `t` is a derivation input** (alongside CST +
  AssetManifest); each frame derives its own immutable `DerivedScene(t)` (keyframed params evaluate
  at `t`). No mutation-during-render; an animation render is a *sequence of sealed snapshots*. This
  matches the CST model (a `timeline` keyframes a param; derivation evaluates it at `t`).
- **Caches populated DURING a pass (irradiance cache, accumulation buffers) are render-local mutable
  state**, owned by the render pass — **NOT** part of the immutable snapshot. They may persist across
  passes/frames for temporal coherence (the renderer's concern), keyed to the snapshot they
  accelerate, and are invalidated when the scene changes.
- **Caches built BEFORE a pass (photon maps, light samplers, TLAS) stay immutable in the snapshot**
  (built at seal, D12) — they are computed-once inputs, not during-render-populated. (Photon-map
  ownership: see D22 — they move to `PreparedRenderState`.)

**Overrides:** D12's "snapshot owns … photon maps" is refined (built-before = immutable;
populated-during = render-local). Animation is **not** deprecated — it is per-frame derivation.

## D22 — Split DerivedScene (config-independent) from PreparedRenderState = prepare(scene, RenderConfig) (amends D12, D5; resolves R3-P1-2)

**Contradiction:** light samplers + photon-map prep depend on rasterizer/integrator settings, and
`render` permits an integrator override — so the render-ready scene is **not** purely `f(CST,
AssetManifest)`.

**Decision — two derivation layers:**
- **`DerivedScene = f(CST, AssetManifest, t)`** — config-**independent**: realized/tessellated
  geometry, materials, lights-as-emitters, **TLAS**. Cached by (CST-version, asset-content-hashes, t).
- **`PreparedRenderState = prepare(DerivedScene, RenderConfig)`** — config-**dependent**: light
  samplers (depend on the integrator's light-sampling strategy), **photon maps** (only for
  photon-consuming integrators), integrator-specific structures. Cached by (DerivedScene-version,
  RenderConfig). **`RenderConfig`** (rasterizer/integrator selection + the render-time override) is an
  explicit third input.
- Both layers are immutable + sealed (D12); the render loop adopts a `PreparedRenderState` at a pass
  boundary. The render-time integrator override re-runs only `prepare`, not the scene derivation.

**Overrides:** D12/D5 "snapshot owns light samplers + photon maps" → those live in
`PreparedRenderState`; `DerivedScene` owns the config-independent scene + TLAS.

## D23 — Persistent immutable containers are an explicit prerequisite for the O(closure) claims (amends D11, D20; resolves R3-P1-3)

**Contradiction:** `GenericManager` uses mutable `std::map` and the "persistent" cache an
`unordered_map`; copying either is O(N), so neither delivers O(closure)/O(log N) snapshot creation.

**Decision:**
- The **manager roots** (name→entity maps), the **derivation cache**, and the **identity side-map**
  (D26) must be **persistent immutable containers** (HAMT / persistent balanced tree, immer-style) to
  deliver O(log N) update + structural sharing across snapshots. This is a **named infrastructure
  prerequisite**, on par with the red-green tree (D2) and the rope (D16).
- **Honest v1 fallback:** a first implementation **may** use copy-on-snapshot mutable maps —
  **O(N_entities) per snapshot** — which is acceptable while entity counts are modest and commits are
  debounced. **In that case the complexity claims are explicitly O(N), not O(closure)/O(log N).** The
  O(closure)/O(log N) headline is **gated on the persistent-container work** and must not be claimed
  before it lands.

**Overrides:** D11/D20 O(closure)/version-scoped-cache claims are gated on D23; un-gated they are O(N).

## D24 — TLAS: full rebuild in v1; persistent/refit BVH is a named future prerequisite (amends D11; resolves R3-P1-4)

**Contradiction:** D11 promised O(log N) TLAS path-copy, a worked example rebuilt the whole TLAS, and
§6 left incremental TLAS open.

**Decision:** **v1 fully rebuilds the TLAS** on any geometry/transform/structural change (O(N log N),
acceptable: dwarfed by the render, edits are debounced). **Incremental TLAS** (a persistent BVH with
path-copy, or refit-with-periodic-rebuild) is an **explicit, named future prerequisite** for cheap
transform edits on very large scenes — **not** claimed for v1. The D11 "O(log N) path-copy of the
TLAS" is withdrawn until the persistent-BVH work exists; until then a transform edit's cost includes
a TLAS rebuild (still « the render).

**Overrides:** D11's TLAS path-copy O(log N) → v1 full rebuild; refit/persistent-BVH = future.

## D25 — Rename requires a head-stamped reference trace (amends D14; resolves R3-P1-5)

> **⚠ Amended by D35 (round 4):** the head-stamped trace is produced by **derivation's own resolver (derive head)** — NOT a separate "reference-tracing pass." D35 wins where they differ.

**Contradiction:** derivation may lag head (D13), but rename runs against **head** using
`ReferenceUse` data produced by **derivation** (at `derivedVersion ≤ head`). A reference added in
head-but-not-yet-derived would be **missed**, silently leaving a dangling old name.

**Decision:** **rename requires a `ReferenceUse` set stamped for the exact head** it renames against.
`ReferenceUse` records carry the version they were traced at; if `derivedVersion < head`, the rename
**synchronously brings the reference trace up to head first** (a full re-derive is not required — only
the reference-tracing pass — and rename is a deliberate, infrequent op, so a synchronous trace-to-head
is acceptable). A rename **never** runs against a stale trace; if it cannot obtain a head-stamped
trace, it is refused (not silently partial).

**Overrides:** D14 gains the head-stamping requirement.

## D26 — Every Version owns a persistent occurrence/identity side-map (completes D15; resolves R3-P1-6)

> **⚠ Amended by D30 (round 4):** the derivation cache is **NOT** in `Version` — it lives on a `DerivedArtifact` keyed by `DerivedStamp`. `Version = { greenRoot, identityRoot, metadata }`. D30 wins where they differ.

**Contradiction:** D15 put `NodeId` "in the red layer / a side-map," but a Version was described as
just a green root + metadata — and identical green nodes can represent multiple occurrences, so there
is no owned place for per-occurrence identity.

**Decision:** **every `Version` owns a persistent occurrence/identity structure** (a side-tree or
persistent map, parallel to the green tree, structurally shared across versions via D23's persistent
containers) mapping each **occurrence/position → its stable `NodeId`**. This is where reparse-matching
(D15) writes NodeId assignments and where rename/undo/UI bindings resolve a NodeId. A `Version` is
therefore `{ greenRoot, identityRoot, derivationCacheRoot, metadata }` — all persistent, all
structurally shared.

**Overrides:** D15's "side-map" is concretized as a persistent per-Version identity root (depends on
D23).

## D27 — Migrate or retire ALL embedded `>` commands, incl. `> set light_rr_threshold` and the seven `> modify` forms (amends D19; resolves R3-P1-7)

> **⚠ Corrected by D37 (round 4):** the "seven `> modify`" are **commented-out** (inside `/* */`); the **active corpus has ZERO `> modify`**. The migrator must be comment/token-aware. D37 wins where they differ.

**Contradiction:** D19 only concretely handled `> set accelerator`/`global_medium`; the corpus also
has `> set light_rr_threshold` and **seven `> modify`** commands.

**Decision (uses the deprecation permission):** enumerate **every** surviving `>` command; v7 has
none.
- **`> set <render-setting>`** (e.g. `light_rr_threshold`) → a **declarative parameter** on the
  relevant rasterizer/integrator chunk.
- **`> modify <entity> …`** (imperative post-definition mutation) is **incompatible with a canonical
  declarative document** and is **retired**: the **migrator folds each `> modify` into the target
  entity's final authored values** (it computes the post-modify state and emits the entity chunk with
  those values). No runtime `modify`.
- The "every scene render-matches after migration" requirement **holds** because the migrator computes
  the post-command state; the migration plan (F6) is amended to enumerate and handle (or explicitly
  retire) **all** `>` forms, with a hard-fail on any unhandled command for hand review.

**Overrides:** D19 is extended to the full `>` command set; F6's migrator gains `> set <setting>` →
param and `> modify` → fold-into-chunk.

## D28 — History preserves the CST only; re-derivation uses current asset bytes (amends D5; resolves R3-P1-8)

**Contradiction:** the AssetManifest records a path + hash but **not the bytes**; after an asset is
overwritten and cached snapshots are evicted, an older branch/version **cannot be re-derived** to its
original pixels.

**Decision (git-native framing):** **undo/redo/branch history preserves the CST (the source), not
historical rendered output.** Re-deriving an old version uses the **current** asset bytes (the
manifest is re-stamped on access); if an external asset changed, the old version's *render* may
differ — this is **explicitly documented**, exactly like git versioning source while build inputs
(large binaries) are the user's responsibility. A **content-addressed asset store** (snapshotting
asset bytes by hash for fully reproducible historical renders) is a **named future option**, not core
— the analogue of git-LFS, layered at the VCS boundary, not the editor. The "Scene = f(CST,
AssetManifest)" purity holds *within a manifest*; across time, the manifest is the live filesystem.

**Overrides:** D5 — the manifest is identity+fingerprint, not a byte store; history = CST-only; asset
CAS is future.

# Net effect (round 3)

Round 3 made the model **honest about its prerequisites and its scope**: animation is per-frame
derivation and irradiance caching is render-local (D21); scene derivation splits from render-config
preparation (D22); the O(closure)/O(log N) headline is explicitly gated on persistent containers
(D23) and the TLAS is full-rebuild-v1 (D24); rename is head-synchronized (D25); identity gets an owned
persistent home (D26); the full `>` command set is migrated/retired (D27); and history is CST-only
with an optional future asset store (D28). No capability is lost — animation and irradiance caching
survive, re-expressed to fit the immutable-snapshot model.

---

# Review Round 4 (D29–D37)

Round 4 tightened identity/determinism and surfaced real machinery work. No P0s; 9 P1. **One was a
factual error in D27** (D37). The refined object model after this round:

```
Version            = { greenRoot, identityRoot, metadata }          (CST + occurrence identity ONLY)
DerivedStamp       = { cstVersion, assetManifestGen, animationName, shutterInterval }
DerivedArtifact    = { derivedStamp, derivedScene, derivationCache } (cache lives HERE, not on Version)
PreparedStamp      = DerivedStamp + { renderConfig, cameraOverride, samplingSeed }
PreparedArtifact   = { preparedStamp, preparedRenderState }
```
> *(Round 5 refines this diagram: `assetManifestGen`→**`assetDigest`** (content digest of the loaded
> buffer, D41); `PreparedStamp`'s `{renderConfig, cameraOverride}`→**`{effectiveRenderConfigHash,
> viewCameraStateHash}`** (D42); both stamps gain **requested-vs-published** status with `ok` ⟺
> full-stamp equality (D38).)*

Artifacts are produced **asynchronously by the render arbiter** (cancellable), keyed by stamp; a
`Version` can have many of each.

## D29 — Complete DerivedStamp/PreparedStamp; compare by equality/ancestry, not `<` (amends D13, D22; resolves R4-P1-1)

**Contradiction:** `derivedVersion` named only the CST version, but a `DerivedScene` also depends on
the asset-manifest generation and time — so t=0 and t=1 (or pre/post asset change) at one CST version
get the same stamp; and `<` is meaningless across independent axes.

**Decision:**
- **`DerivedStamp = { cstVersion, assetManifestGen, animationName, shutterInterval }`** identifies a
  `DerivedScene`. **`PreparedStamp = DerivedStamp + { renderConfig, cameraOverride, samplingSeed }`**
  identifies a `PreparedRenderState`.
- Cache lookups match the **full stamp by equality**. The "is the render stale vs head?" check is on
  the **`cstVersion` axis only**, using **version-DAG ancestry** (rendered cstVersion is an
  ancestor-or-equal of head's), **never numeric `<`** (the DAG has branches; the other axes are
  equality-matched, not ordered).

**Overrides:** D13's single `derivedVersion` → DerivedStamp/PreparedStamp; D22's informal cache keys
are formalized as these stamps; all `<` version comparisons → DAG ancestry/equality.

## D30 — The derivation cache lives on DerivedArtifact, not on the immutable Version (amends D26, D20; resolves R4-P1-2)

**Contradiction:** D26 put `derivationCacheRoot` in `Version`, but a Version commits *before* async
derivation completes and can spawn *many* caches (per time/asset/config).

**Decision:** **`Version = { greenRoot, identityRoot, metadata }`** — CST + occurrence identity only,
no cache. The memo/dependency cache lives on a **`DerivedArtifact`** keyed by the `DerivedStamp` (and
a `PreparedArtifact` keyed by `PreparedStamp`); artifacts are held in a stamp-keyed LRU, not owned by
the immutable Version. One Version → many artifacts.

**Overrides:** D26's `Version` shape drops `derivationCacheRoot`; D20's version-scoped cache is
artifact-scoped (keyed by full stamp).

## D31 — Motion blur preserved via a time-INTERVAL immutable scene; animation name is an input (amends D21; resolves R4-P1-3)

**Contradiction:** rasterizers/photon-tracers evaluate animation at a **random time per sample**
(`PixelBasedPelRasterizer.cpp:636`) for motion blur; a single frozen `DerivedScene(t)` destroys it.
The active animation name was also missing from the signature.

**Decision (keep motion blur):**
- A motion-blurred frame's `DerivedScene` is **time-interval-parameterized**: animated quantities are
  baked as **immutable functions/samples over the shutter `[t0,t1]`** (PBRT-style `AnimatedTransform`),
  and the renderer evaluates `at(τ)` per sample **read-only** (no mutation). The "time" axis of the
  DerivedStamp is the **shutter interval**, and the **active animation name** is an explicit input
  (both in `DerivedStamp`, D29).
- The TLAS for a motion-blurred frame is a **motion BVH** (time-interval). This is **gated work**:
  **v1 supports single-time (no motion blur)**; motion blur (AnimatedTransform-in-DerivedScene +
  motion BVH) is a named follow-on, like the TLAS-refit gate (D24). Motion blur is **not retired**.

**Overrides:** D21's `DerivedScene(t)` → time-interval immutable scene; +animationName/shutter inputs.

## D32 — prepare() needs a real PreparedRenderStateBuilder + non-mutating scene input APIs (amends D22, D12; resolves R4-P1-4)

**Contradiction:** photon maps are `Scene`-owned and `BuildPendingPhotonMaps` **mutates** pending
flags/maps/gather params (`Scene.cpp:750`); light samplers are `RayCaster`-owned. You cannot build
these by mutating a sealed immutable `DerivedScene`.

**Decision:** `prepare()` reads the sealed `DerivedScene` through **non-mutating (const) input APIs**
and writes into a separate mutable **`PreparedRenderStateBuilder`**, then seals → `PreparedRenderState`.
This requires refactoring `BuildPendingPhotonMaps` and light-sampler construction from
"mutate the Scene" to `build(const DerivedScene&, PreparedRenderStateBuilder&)`. **Named prerequisite
work** (the prepare layer cannot reuse the mutating machinery unchanged).

**Overrides:** D22/D12 "prepare builds samplers/photons" gains the builder + non-mutating-input refactor.

## D33 — prepare() is deterministic: seed/stream identity in RenderConfig + the prepare key (amends D22; resolves R4-P1-5)

**Contradiction:** photon tracers seed RNGs with `rand()` by default (`RandomNumbers.h:32`), so the
same `(DerivedScene, RenderConfig)` yields *different* photon maps — not cacheable, not reproducible.

**Decision:** **`RenderConfig` carries a sampling seed / RNG-stream identity**; all stochastic
preparation (photon tracing, any sampled prep) uses it instead of `rand()`. The seed is part of the
`PreparedStamp` (D29), so `prepare` is a pure function of its key → cacheable **and** reproducible
(a win for the git-native/agentic thesis: deterministic renders).

**Overrides:** D22 — `prepare` inputs include the seed; `rand()`-seeded stochastic prep is refactored.

## D34 — prepare/derive run as cancellable phases of the render arbiter, off the edit thread (amends D12, D22; resolves R4-P1-6)

**Contradiction:** photon-map construction takes seconds + all cores; running it synchronously on the
UI/agent edit thread would freeze it, and contradicts the single-render-arbiter promise.

**Decision:** the edit thread only **commits a CST `Version`** (cheap). The **render arbiter**
asynchronously runs **derive → seal → prepare → seal → render** as **cancellable phases of its render
job**; when a newer head arrives, the in-flight phases cancel and restart at the new stamp. This is
exactly the source of the head-vs-derived lag (D13/D29): the arbiter is mid-derive/prepare/render on
an older stamp. Nothing expensive runs on the edit/agent thread.

**Overrides:** D12's build/seal/publish is an **async, cancellable arbiter job**, not synchronous.

## D35 — Rename reuses the derivation resolver (no second resolution path) (amends D25; resolves R4-P1-7)

**Contradiction:** D25's "synchronous reference-tracing pass" is a **second** resolution path that can
drift from real derivation — but D4 demoted static schema walks *precisely because* dynamic refs need
real derivation.

**Decision:** rename obtains head's reference set with **the exact same evaluator/resolver as
derivation** — there is **one** resolution implementation. Concretely: rename **synchronously derives
head** (or runs derivation's own reference-resolution step to head, sharing that code), and reads the
resulting traced `ReferenceUse`. **No parallel "tracing pass" reimplementation.** If head cannot be
derived (semantic error), rename is refused, not best-effort.

**Overrides:** D25's separate tracing pass → the shared derivation resolver (derive-to-head).

## D36 — Propagate {greenRoot, identityRoot} through edits; store NodeId in widgets/view-nodes/intents/selection (completes D26; resolves R4-P1-8)

**Contradiction:** identity stops at `Version`. The `GestureBuffer` carries only a green root (so
additions/reparses can't update the working `identityRoot`); `Widget` "holds a NodeId" but has no such
field; F4 selection is still name-path-based.

**Decision:**
- Staged-edit state (**`GestureBuffer`**, working head) carries **`{ greenRoot, identityRoot }`** —
  both roots — so insertions/reparses during a gesture update occurrence identity as they go.
- **`Widget`, `ViewNode`, `EditIntent`, and selection store the `NodeId`** (name-path is kept for
  display/addressing only). Selection is a `NodeId` (resolved to name-path for the header); an
  `EditIntent` carries the target `NodeId`; a `ViewNode`/`Widget` binds by `NodeId`.

**Overrides:** D26 propagation completed into F3's `GestureBuffer` and F4's `Widget`/`ViewNode`/
`EditIntent`/selection (which gain a real `NodeId` field).

## D37 — Migration is comment/token-aware; the active corpus has ZERO `> modify` (corrects D27; resolves R4-P1-9)

**Factual error in D27 (owned):** the "seven `> modify` in `watch_dial`" are **inside a `/* … */`
block** (`watch_dial.RISEscene:2392`) — a commented-out *night-mode* variant. They are **not active.**
The earlier "verified corpus counts" used a naive line grep that is **not comment-aware** and so
counted commented lines. **Folding them would have flipped the day scene to night and broken render
equivalence.**

**Decision:**
- **The active corpus has ZERO `> modify` commands.** D27's `> modify`-folding is **moot for the
  current corpus** (nothing active to fold).
- **The migrator MUST be comment/token-aware** — it operates on the **parsed token stream / CST**,
  never a raw line/grep scan, so it never activates a commented-out command (and preserves `/* */`
  blocks verbatim as CST comment nodes). D27's `> set`→param and (if ever active) `> modify`→fold
  rules stand, but apply **only to active commands**.
- **Verification lesson (codified):** corpus audits feeding migration/decisions must be comment/
  token-aware (parse, don't grep). A naive grep is not evidence about *active* scene content.

**Overrides:** D27's census (active `> modify` = 0, not 7) and migrator (must be comment/token-aware).

# Net effect (round 4)

Round 4 made **identity, determinism, and threading precise**: full Derived/Prepared **stamps**
(D29) keying **artifacts** that hold the cache (D30), off the immutable Version; **deterministic**
seeded prepare (D33) run **async + cancellable on the arbiter** (D34) via a real non-mutating
**builder** (D32); rename reuses the **one** resolver (D35); identity propagates through edits + UI
(D36). **Motion blur is preserved** as a time-interval immutable scene (D31, gated). And D37 corrects
a real factual error — the `> modify` lines are commented out, so the migrator must parse, not grep.

---

# Review Round 5 (D38–D44)

Round 5 probed render-determinism, config resolution, cancellation, and a charter-level staleness.
No P0s; 7 P1. Two are "split the conflated thing" (D39, D43); two are honest weakenings (D40, D42);
**D44 fixes the locked charter** (L5 still said name-path is identity).

## D38 — Status exposes requested AND published stamps; ok ⟺ full-stamp equality (amends D13/D29; resolves R5-P1-1)

**Contradiction:** the status surface published only what the arbiter *produced*; when time/assets/
camera/config change without changing `headVersion`, a client can't see what the arbiter is *trying*
to produce.

**Decision:** status = `{ headVersion, requestedDerivedStamp, requestedPreparedStamp,
publishedDerivedStamp, publishedPreparedStamp, status, diagnostics }`. **`status:ok` requires
full-stamp equality** (`published == requested` on *every* axis — cstVersion, assetDigest, animation,
shutter, effective-config, view-camera, seed); otherwise `status:deriving|preparing|rendering`. The
requested stamps are set when *any* input axis changes (not just the CST head).

**Overrides:** D13/D29 status surface gains the requested stamps; `ok` is full-stamp equality.

## D39 — Two derivation phases: a bounded SYNC semantic phase + the async expensive phase (amends D34/D35/D5; resolves R5-P1-2)

**Contradiction:** `propose_patch` precommit needs a synchronous validate, and rename synchronously
derives head — but D34 says *all* derivation is async/off-edit-thread.

**Decision — split derivation:**
- **Synchronous semantic phase (bounded, deterministic, edit-thread-OK):** lex → parse → CST →
  bind-to-descriptor → **reference resolution (traced `ReferenceUse`)** → type/pipe/typecheck. Output:
  a validated CST + reference graph + diagnostics. No asset I/O beyond identity, no realization. This
  is what `propose_patch` precommit and **rename (D35)** use.
- **Asynchronous expensive phase (arbiter, cancellable):** realize/tessellate (loads asset bytes),
  TLAS, `prepare` (samplers/photons), render — D34's async job.
- The semantic phase **is the front of the async job** (same code), so it is *not* a second resolver
  (D35's no-drift holds). **Scope note:** the sync phase resolves references to **CST-declared**
  name-paths; references *into* asset-expanded sub-entities (e.g. a glTF import's children) require
  the async phase and are out of v1 cross-reference scope.

**Overrides:** D34 "all derivation async" → bounded semantic phase may be sync; D35's "derive head" =
run the sync semantic phase to head; D5's `validate` = the sync semantic phase.

## D40 — The seed makes PREPARE deterministic; the RENDER is reproducible-within-tolerance, not bit-identical (amends D33; resolves R5-P1-3)

**Contradiction:** renderers use per-worker independently-seeded RNGs + `GlobalRNG()`
(`RasterizeDispatchers.h:146`, `PixelBasedRasterizerHelper.cpp:897`); tile assignment, splat
reduction, and denoise also vary — a single seed does **not** yield bit-identical images.

**Decision (honest weakening):**
- **`prepare` (photon maps) IS made deterministic** by the seed (so the `PreparedArtifact` cache is
  sound — same `PreparedStamp` → same photon maps; this is all D33's cache-soundness actually needed).
- **The final RENDER is NOT bit-identical** — it is **reproducible within MC tolerance** (same scene/
  config/seed → the same converged image up to Monte-Carlo noise). The git-native "diffable renders"
  claim weakens accordingly (review-by-image, not byte-diff).
- **Bit-identical rendering** (deterministic per-pixel/per-sample RNG streams keyed by pixel+sample,
  deterministic splat reduction, deterministic denoise — across *every* renderer) is a **named future
  option**, not v1.

**Overrides:** D33's "reproducible/bit-identical render" → prepare-deterministic + render-reproducible-
within-tolerance; bit-identical is future.

## D41 — Asset bytes bind to the stamp by content digest of the actually-loaded buffer (amends D5/D17/D29; resolves R5-P1-4)

**Contradiction:** the manifest hashes a *path*, but loaders reopen that path later
(`TriangleMeshLoaderPLY.cpp:795`) — a TOCTOU window where the file can change between hash and load,
stamping an artifact with the wrong identity. And a session-local generation counter is not a
reproducible identity.

**Decision:** the asset's identity is the **content digest of the exact bytes the loader consumed** —
either **load-and-hash one buffer** (read once, hash that buffer, hand the buffer to the loader) or
**revalidate after load** (re-hash the loaded bytes vs the manifest; on mismatch, retry/refuse). The
**`DerivedStamp` asset axis is a content digest** (per-asset content hashes), not a session
generation; the generation counter remains only as a fast in-process change signal.

**Overrides:** D5/D17 path-hash → loaded-buffer content digest (load-once or revalidate-and-retry);
D29's `assetManifestGen` → `assetDigest` (content) for the stamp's reproducible identity.

## D42 — Stamp the normalized EffectiveRenderConfig + a view-camera-state hash, not a raw request + CameraId (amends D29/D22; resolves R5-P1-5)

> **⚠ Amended by D45 (round 6):** `ResolveEffectiveRenderConfig` takes the **`DerivedScene`** (not the CST) and runs **after** derive — auto-routing inspects the assembled scene and may probe-render. D45 wins where they differ.

**Contradiction:** scene-authored rasterizer settings, request overrides, defaults, and
auto-resolution (e.g. auto-rasterizer) have no defined merge; and a `CameraId` cannot identify the
continuously-changing ephemeral viewport camera pose.

**Decision:**
- Define **`ResolveEffectiveRenderConfig(CST, request) → EffectiveRenderConfig`** — a deterministic
  merge: scene-authored rasterizer/integrator settings ← request overrides ← defaults ←
  auto-resolution (the resolved integrator/resolution). The **normalized result + its content hash**
  goes in the `PreparedStamp` (not the raw request).
- The viewport camera is an **ephemeral pose**, not a `CameraId`: stamp a **content hash / generation
  of the complete view-camera state** (pose, lens). So `PreparedStamp = DerivedStamp +
  { effectiveRenderConfigHash, viewCameraStateHash, samplingSeed }`.

**Overrides:** D29/D22 `RenderConfig`/`cameraOverride: CameraId` → `EffectiveRenderConfig` (resolved +
hashed) + `viewCameraStateHash`.

## D43 — Separate latest-wins preview jobs from stamp-pinned explicit renders (amends D34; resolves R5-P1-6)

> **⚠ Amended by D47/D48/D50 (round 6):** pinned renders are **not** dropped on a head change (D47); there is **one render slot** so previews **suspend** while a pinned render owns it (NOT "run alongside" — D48, RISE's single-render invariant); pinned renders are **`RenderJobId`-keyed** with targeted control (D50). These win where they differ.

**Contradiction:** D34 says every newer head cancels the in-flight render; the agent surface allows
preempt/queue/reject + stale stamped results. As one policy, an unrelated edit silently destroys a
requested final render.

**Decision — two arbiter job classes:**
- **Preview jobs — latest-wins:** interactive viewport previews track head; a newer head cancels the
  in-flight preview (D34's policy). Ephemeral, cheap, never pinned.
- **Pinned render jobs — stamp-pinned:** an explicit "render *this* stamp" (final/export) is **pinned
  to its `requestedPreparedStamp`**; a newer head does **not** cancel it — it runs to completion (or
  is cancelled only by its requester). The coordinator may queue pinned renders (one heavy render at a
  time) and run previews alongside.

**Overrides:** D34's "newer head cancels in-flight render" applies to **preview** jobs only; pinned
renders are stamp-pinned.

## D44 — Charter L5/INV-5 fixed: NodeId is lineage identity, name-path is addressing (amends charter L5/INV-5; resolves R5-P1-7)

**Contradiction:** the *locked* charter still defines **name-path as the stable identity currency**
(L5, INV-5), contradicting D9/D15/D26/D36 (NodeId is lineage identity; name-path is addressing) and
the corrected UI structures.

**Decision:** update the charter so the locked foundation matches the ratified decisions:
- **L5:** identity is the immutable **`NodeId`** (lineage; survives rename + reparse; lives in each
  Version's `identityRoot`); **name-path** (`objects/sphere.material`) is the **addressing** scheme
  (human/agent-readable; resolves to a NodeId within a version; changes on rename).
- **INV-5 (Stable identity):** durable references (selection, agent refs, UI bindings, undo lineage)
  key on the **NodeId**, not the name-path.
- Propagate to the overview diagrams + first-slice language (name-path = addressing, NodeId = identity).

**Overrides:** charter L5 + INV-5 (the only round that edits the "locked" charter — a correction to
keep the foundation consistent with D9/D15/D26/D36).

# Net effect (round 5)

Round 5 made the **runtime semantics** precise: status shows requested vs published with full-stamp
`ok` (D38); derivation splits into a bounded sync semantic phase (validate/rename) + the async
expensive phase (D39); the seed makes *prepare* deterministic while the *render* is honestly
reproducible-within-tolerance (D40); assets bind by content digest of the loaded buffer (D41); the
stamp carries a resolved EffectiveRenderConfig + view-pose hash (D42); previews are latest-wins while
explicit renders are stamp-pinned (D43); and the locked charter's identity model is corrected (D44).

---

# Review Round 6 (D45–D51)

Round 6 tested the round-5 runtime decisions against the *real* auto-router, the *real* glTF importer,
and the *retained* Model-A GUI coordinator/security specs. No P0s; 7 P1. Several reconcile a legacy
`docs/gui/` spec that contradicts a new decision (those legacy specs are superseded where they
conflict — charter L7). The corrected pipeline:

```
CST (+AssetManifest +t)
  → sync semantic phase   (parse/resolve/typecheck — D39, bounded, edit-thread)
  → ASYNC ARBITER (D34), single render slot (D48):
        DerivedScene = derive(CST, assets, t)                      (config-independent, D22)
        EffectiveRenderConfig = ResolveEffectiveRenderConfig(DerivedScene, request)  (D45: auto-route, may probe)
        PreparedRenderState  = prepare(DerivedScene, EffectiveRenderConfig)           (D22/D32)
        seal → publish stamps
        render → image       (preview latest-wins | pinned-by-RenderJobId; phase→complete — D43/D48/D49/D50)
```

## D45 — Effective config is resolved AFTER DerivedScene (auto-route may probe-render) (amends D42; resolves R6-P1-1)

**Contradiction:** `ResolveEffectiveRenderConfig(CST, request)` (D42) can't work — auto-routing
(`auto_rasterizer`, the shipped Candidate-C dispatcher) inspects the *assembled scene* and may run
**Tier-2 probe renders** (`AutoRasterizer.cpp:333`) to pick the integrator.

**Decision:** routing runs **after `DerivedScene` exists**: `ResolveEffectiveRenderConfig(DerivedScene,
request)` — it may inspect geometry/lights and execute a probe render — and produces the
`EffectiveRenderConfig`, which is then **hashed into the `PreparedStamp` and fed to `prepare`**. The
`DerivedScene` stays config-independent (D22 holds); routing is a step *between* derive and prepare.
The probe render is a bounded sub-step that takes the single render slot (D48) briefly; with the seed
(D33) it is deterministic, so the resolved config is cacheable by (`DerivedScene`-version, request).

**Overrides:** D42's `ResolveEffectiveRenderConfig(CST, …)` → `(DerivedScene, …)`, run post-derive.

## D46 — Asset identity = transitive byte-closure digest; pinned jobs pin the closure (amends D41/D5; resolves R6-P1-2)

**Contradiction:** glTF consumes the main file **plus external buffers and textures**
(`GLTFSceneImporter.cpp:1849`). A digest of the *direct* chunk path (D41) does not identify that
transitive byte closure, and a queued/pinned job can't reproduce it unless those bytes are pinned.

**Decision:** a composite asset's identity is the **content digest of its transitive byte closure**
(main file + every transitively-referenced external buffer/texture) — the importer reports its full
dependency set, each hashed (load-and-hash or revalidate, D41). For a **pinned render job** (D43), the
entire dependency **closure is pinned** (bytes snapshotted/held for the job's lifetime) so a queued
render reproduces deterministically regardless of later on-disk changes. (This is the bounded,
per-job version of D28's "content-addressed asset store, future".)

**Overrides:** D41/D5 direct-path digest → transitive-closure digest; pinned jobs pin the closure.

## D47 — Pinned renders are NOT dropped on a head change (supersedes the legacy coordinator's stale-drop; resolves R6-P1-3)

**Contradiction:** the retained `RENDER_COORDINATOR.md` (`:796`) drops queued/completed jobs when the
document revision changes — the exact opposite of D43's promise for pinned renders.

**Decision:** the legacy "drop all stale jobs on revision change" rule is **Model-A and superseded for
pinned renders**. The Model-B coordinator applies stale-drop to **preview jobs only** (latest-wins,
D43); **pinned render jobs survive a head/revision change** — they are pinned to their
`requestedPreparedStamp` and run to completion (or are cancelled only by their requester, D50). The
`RENDER_COORDINATOR.md` stale-drop is reframed accordingly (preview-only).

**Overrides:** `RENDER_COORDINATOR.md` revision-stale-drop applies to previews, not pinned renders.

## D48 — One render slot (process-wide single-render invariant); previews suspend while a pinned render owns it (amends D43; resolves R6-P1-4)

**Contradiction:** D43 said the coordinator "may run previews alongside" a pinned render — violating
RISE's **hard single-render invariant** (a render consumes all cores; two concurrent renders make the
machine unusable — the repo's "render sequentially, never in parallel" rule).

**Decision:** **exactly one render owns the slot at a time.** While a **pinned render** owns the slot,
**previews suspend/queue** (they do not run alongside); the newest pending preview runs when the slot
frees (latest-wins among queued previews). The requester may **pause/cancel** the pinned render (D50)
to yield the slot. A routing probe (D45) also takes the slot briefly. No concurrency; the invariant
holds.

**Overrides:** D43's "run previews alongside" → serialize on one slot; previews suspend during a
pinned render.

## D49 — status:ok requires phase==complete, not just stamp equality (amends D38; resolves R6-P1-5)

**Contradiction:** the `PreparedRenderState` is published *before* rendering starts, so
`requested == published` stamps can match for the entire duration of the render — `status:ok` (D38,
stamp equality) would be true mid-render.

**Decision:** status carries an explicit **phase** ∈ `{ idle, deriving, routing, preparing, rendering,
complete, error }` plus progress. **`ok`/done ⟺ full-stamp equality (D38) AND `phase == complete`**
(the output image for that stamp exists, with its own completion marker — samples-done / converged).
Stamp equality alone means "the right thing is being produced," not "it is produced."

**Overrides:** D38 `ok ⟺ full-stamp equality` → `… AND phase == complete`.

## D50 — Pinned renders have per-job identity: `RenderJobId`, per-job status/result, targeted control (amends D38/D43; resolves R6-P1-6)

**Contradiction:** D43 allows *queuing* pinned renders, but the surface exposes one requested stamp and
untargeted `stop_render`/`pause_render` — no way to address one of several queued jobs.

**Decision:** an explicit (pinned) render request **returns a `RenderJobId`**; **status, progress,
result/output, and `stop`/`pause`/`resume` are per-job** (keyed by `RenderJobId`). The single
requested/published-stamp surface (D38) describes the **preview** (latest-wins, one); **pinned renders
are a set keyed by `RenderJobId`**, each with its own pinned stamp + phase (D49).

**Overrides:** D38/D43 single-render-surface → preview (one) + pinned renders (a `RenderJobId`-keyed
set with targeted control).

## D51 — Commit = semantic validation only; broken heads are possible; optional awaited full-validation (amends D39; resolves R6-P1-7)

**Contradiction:** the agent-safety risk table (`AI_SECURITY_MODEL.md`) promises **full precommit
gating**, but D39 commits after only the **bounded sync semantic phase** (parse/resolve/typecheck) —
*before* asset loading, realization, prepare, and render. So a committed head can be semantically valid
but fail to derive/render (missing asset, bad geometry).

**Decision:**
- The safety contract is corrected: **commit guarantees *semantic* validity, not full derivation/
  render success.** A committed head may be a **broken-but-valid CST** whose async expensive phase
  fails — surfaced as `status:error` + node-local diagnostics (D38/D49), never a silent corruption.
- Provide an **optional awaited full-validation mode** (`propose_patch{ awaitFullValidation: true }`)
  that synchronously awaits the async derive+prepare (not the render) before reporting the commit as
  fully validated — for callers (e.g. a CI/headless agent) that need the stronger guarantee. Default
  is fast semantic-only commit.

**Overrides:** the `AI_SECURITY_MODEL.md` "full precommit gating" promise → semantic-only commit +
broken-head diagnostics + opt-in awaited full validation.

# Net effect (round 6)

Round 6 reconciled the runtime model with the real engine + the legacy GUI specs: routing runs
**post-DerivedScene** and may probe (D45); asset identity is the **transitive closure**, pinned for
queued jobs (D46); pinned renders **survive head changes** (D47) on a **single render slot** that
previews yield to (D48); `status:ok` needs **completion**, not just stamp match (D49); pinned renders
get **`RenderJobId`** identity + targeted control (D50); and the safety contract honestly admits
**semantic-only commit** with an opt-in awaited full validation (D51). The legacy `RENDER_COORDINATOR`/
`AI_SECURITY_MODEL` GUI specs are superseded where they conflict.
