# RISE Agentic Redesign — Decision Record (resolves review round 1)

> **Status:** authoritative. This document resolves the 8 P1 contradictions + 2 P2 issues from the
> first external design review. Where a decision conflicts with a facet doc (10–60) or the overview
> (00), **this document wins** and the facet doc is being updated to conform. Read after
> [`00-CHARTER.md`](00-CHARTER.md) and [`00-OVERVIEW.md`](00-OVERVIEW.md).
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
