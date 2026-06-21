# Facet 6 — Supersession Inventory, Migration Path & Risk Register

> **Updated per [`01-DECISIONS.md`](01-DECISIONS.md) (rounds 1 & 2).** Where this
> doc previously hedged behind open decisions (O1/O2/O3) or offered forks, it now
> conforms to the ratified decisions **D1–D20** (round 2's D11–D20 *amend* several
> round-1 decisions and win where they conflict). The biggest changes: single-file
> v7 with `> load`/`> run` flattened away (D7) **and ALL embedded `>` commands
> removed — `> set` forms become declarative `acceleration{}`/`global_medium{}`
> chunks (D19)**; a *singular* migrate-then-DELETE contract for the v6 path with
> **no** permanent coexistence and **no** runtime legacy nodes (D8); `SaveEngine`
> byte-splice DELETE, the external-conflict guard retained but as **temp-write →
> fsync → revalidate content-hash → atomic rename** (D17 supersedes D6's
> compare-and-swap); `SourceSpanIndex`/`ApplyOffsetDeltas` DELETE (the red cursor
> over a **persistent rope** replaces absolute offsets, **O(log N)**, D2 + D16);
> the derived scene is a **reverse-dependency-closure COW snapshot, built→sealed→
> published at a pass boundary** (D11/D12 amend D1); identity/key/lineage split
> three ways (D15) with rename via traced `ReferenceUse` records (D14); and the
> first slice deferred to the corrected phased fixture in **§D18** (adds a real
> reference chain + an asset phase; D18 amends D10).

> **Status:** design-in-progress. One of six parallel facet docs under the
> [Agentic Redesign Charter](00-CHARTER.md). This facet is **cross-cutting**: it
> inventories the *entire* current architecture, assigns each component a fate
> under Model B (delete / evolve / reuse / unchanged), lays out a phased,
> non-big-bang migration, specs the scene-corpus migration tooling, and carries
> the risk register (including the two tar-pits). It reconciles Facets 1–5.
>
> **Design only. No source, build, or scene changes are made by this doc.**

**Reconciliation note.** [`01-DECISIONS.md`](01-DECISIONS.md) is now authoritative
(rounds 1 & 2) and resolves the cross-facet contradictions this doc previously
flagged; round 2's **D11–D20** amend several round-1 decisions and win where they
conflict. The remaining `10`–`50` facet docs are being conformed to the same
decisions; where this doc says "Facet N deletes X," that fate is now pinned by a
numbered decision (cited inline) rather than an open assumption.

---

## 1. Current-state grounding

This section is the factual baseline the rest of the doc builds on. Every claim is
anchored to a real file. The picture was assembled by reading the parser, the
assembly layer, the edit subsystem, the build projects, the test suite, the scene
corpus, and the `docs/gui/` roadmap directly.

### 1.1 The one framing that governs everything

**The existing GUI roadmap already commits to "text is the source of truth," but
the code and the deep-dive specs implement Model A.** [GUI_ROADMAP.md](../GUI_ROADMAP.md)
principle #1 ("Text is the source of truth") and #6 ("Everything routes through one
mutation path") *are Model B in spirit*. But the actual spine —
`src/Library/SceneEditor/` + [docs/gui/TRANSACTION_MODEL.md](../gui/TRANSACTION_MODEL.md)
— is a mutable in-memory `Scene`/`Job` graph, mutated through
`SceneEditController` → `SceneEditor::Apply`, with inverse-edit undo and a
byte-splicing `SaveEngine` that **re-reads the file from disk at save time**
(`SaveEngine.cpp:869`) because the `Job` retains no source text (only byte offsets,
`Job.h:116`).

> **The supersession is therefore not a reversal of the roadmap's thesis — it is
> its fulfillment.** Model B closes the gap between the stated principle and the
> Model-A implementation. This framing matters for the risk register: we are not
> fighting the existing direction, we are concentrating the change in exactly one
> subsystem family while the marquee AI layer and the spectral/feature
> differentiators carry over.

### 1.2 Parser & scene language (Facet 1's territory — grounding only)

- **Descriptor-driven, single forward streaming pass.** `AsciiSceneParser::ParseAndLoadScene`
  (`src/Library/Parsers/AsciiSceneParser.cpp:~10473`) reads line-by-line, tokenizes
  via `AsciiCommandParser::TokenizeString`, branches on `tokens[0]`, and for a chunk
  accumulates body lines into a `ParseStateBag`, then calls
  `pChunkParser->ParseChunk()` → `Finalize(bag, pJob)` → `pJob.AddX(...)`
  **immediately, in file order**. **No tree is retained for the engine scene.**
- **The schema is data.** Each `IAsciiChunkParser` overrides only `Describe()`
  (returns a `ChunkDescriptor`) and `Finalize()`. `ChunkDescriptor` /
  `ParameterDescriptor` / `ValueKind` (`Bool, UInt, Double, DoubleVec3, DoubleVec4,
  DoubleMat4, String, Filename, Enum, Reference`) / `ChunkCategory` live in
  `src/Library/Parsers/ChunkDescriptor.h`. **153 chunk parsers** are registered in
  `CreateAllChunkParsers()` (`AsciiSceneParser.cpp:~9640`). The descriptor IS the
  accepted-parameter-name set (drift on *names* is structurally impossible);
  **enum *values* are advisory, not enforced** (`reference_riseascene_parser_quirks.md`,
  the 2026-06-08 filter_type bug) — a real hazard for a schema-driven UI.
- **The imperative macro layer to be removed (Charter L3).** `DEFINE`/`!NAME`
  macros (`map<String,Scalar>`, all scalars; `substitute_macro` at `:10999`),
  `@`/`%` reference sigils, `$(...)` arithmetic (`evaluate_expression` at `:298`;
  `MathExpressionEvaluator.{h,cpp}`), inline `sin/cos/tan/sqrt/hal` functions, and
  `FOR`/`ENDFOR` implemented by **file rewind** (`struct LOOP` saves `tellg()`,
  `ENDFOR` does `seekg`; `:10728`–`:10802`). All run inline per-line *before* chunk
  dispatch. **Corpus usage: 29 of 376 scenes** (13 FOR, 22 DEFINE, 17 `$(...)`, 2
  `hal(`).
- **The proto-CST already exists (critical).** A parallel byte-offset retention
  layer runs alongside the parse: `RawTokenCapture` (`src/Library/Parsers/`,
  records every line's tokens with byte ranges + an `isSymbolic` flag for
  `$`-derived tokens + comment spans) and `SourceSpanIndex` /`OverrideSpanIndex`
  (`src/Library/SceneEditor/`, per-entity `SourceSpan` with chunk/brace/parameter
  byte ranges, `AuthorMode`, `chunkRevisited` FOR-flag, `loadedPropertyValues`).
  This is **partial** (only savable categories: Object/Camera/Light/Material/Medium)
  and discards inter-token bytes (the save engine re-reads disk), but it already
  solves the hard cases (FOR-revisit, `isSymbolic`, matrix-vs-quaternion author
  mode) at span granularity. **It is the seed a real lossless CST grows from.**
- **Format version:** `#define CURRENT_SCENE_VERSION 6` (`AsciiSceneParser.cpp:156`);
  enforced at `:10561`; v5 special-cased with a migration message. Header line:
  `RISE ASCII SCENE 6`.

### 1.3 Assembly: `RISE_API` + `IJob` + `Job` + managers (Facet 2's territory)

- **Two-layer apply.** `RISE_API.h` (3810 lines; **~326 `RISE_API_Create*`**
  factories) builds standalone refcounted objects from already-built dependencies
  *by reference* (no names). `IJob.h` (3222 lines; **138 `Add*` + 41 `Set*`**) is
  the **name-keyed** layer: resolves dependency *names* through managers, calls the
  matching factory, registers the result.
- **Assembly is strictly order-dependent.** Dependencies are resolved eagerly by
  name at `Add*` time via `GetItem`; a missing dependency is a **hard failure**, no
  forward-reference handling, no fixup pass (`Job.cpp` `AddObject` at `:5220` returns
  false on missing geometry/material). File order = assembly order. *(This is the
  single biggest behavioral constraint a deterministic tree-derivation must respect
  — see §2 and §4.)*
- **Managers are name-keyed with identity serials.** `GenericManager<T>`
  (`src/Library/Managers/GenericManager.h`) is a `std::map<String, ...>` with
  duplicate-name rejection, intrusive refcounting (`Utilities/Reference.h`), and a
  **monotonic per-name serial** (`m_nNextSerial`, `GetItemSerial`) so a remove+re-add
  under the same name is *detectable*. Identity = name string; the serial is the
  "did the instance behind this name change" hook. (Charter L5 makes name-path
  identity first-class — this is the substrate.)
- **The render-ready seam is clean and single-point.** `RayCaster::AttachScene`
  (+ `ObjectManager::PrepareForRendering`) realizes deferred geometry from world
  roots (cascade into CSG operands; idempotent, single-threaded, asserts the
  `RenderParallelScope` freeze gauge is 0), builds light samplers, builds the
  TLAS (BVH4 default), and optionally shoots photons (gated on
  `IRasterizer::ConsumesScenePhotonMaps`). See `project_deferred_realization.md`.
  **This entire seam is assembly-agnostic** — it consumes the `IObject`/`IGeometry`
  graph regardless of how it was built.

### 1.4 The edit subsystem (Facet 3's territory — the supersession centroid)

`src/Library/SceneEditor/` — **34 files, ~15,325 lines**. Five layers:

| Layer | Files | Lines | What it is |
|---|---|---|---|
| Command model | `SceneEdit.h` | 421 | Trivially-copyable tagged union; **27 `Op` kinds**; captured `prev*` state for inverse-replay undo; `historySeq` + `capturedTargetSerial` identity. |
| Undo/redo | `EditHistory.{h,cpp}` | 416 | Two `std::deque<SceneEdit>`, linear (no branches), 1024 cap, trim-immune `mNextSeq`, owns the dirty-object name set. |
| Mutation engine | `SceneEditor.{h,cpp}` | 2217 | "The only sanctioned mutator." Apply/Undo/Redo + the post-mutation invariant chain (`FinalizeTransformations → ResetRuntimeData → InvalidateSpatialStructure`) + light-gen bumps. The "5 parallel walks" (now partially consolidated to `CaptureForApply` / `ApplyForwardMutation` / `ApplyRevertMutation`, but composite loops still inline). |
| Orchestration | `SceneEditController.{h,cpp}` | 5239 | The cross-platform GUI brain. **~104 public methods.** Owns the render thread (cancel-restart-on-edit), the tool/gizmo state machine, selection (`mSelectionByCategory[9]`), adaptive preview-resolution, the properties-panel snapshot caches, transaction state (`mTxnOpen` + `EditorStateSnapshot`), and the save trigger. |
| Source-retention + save | `SourceSpanIndex`, `OverrideSpanIndex`, `DirtyTracker`, `TransformSnapshot`, `SaveEngine`, 7 `*Introspection`, `ChunkDescriptorRegistry` | ~6800 | The round-trip-save machinery: the **proto-CST** (span indices) + `SaveEngine` (byte-splice) + the engine→editable readback (`*Introspection`). |

- **`SaveEngine` is splice, not regenerate** (`SaveEngine.cpp`, 1898 lines). It reads
  the original file bytes from disk, models edits as `EditOp{begin,end,replacement}`
  text-span replacements, applies them descending-begin (refuses on overlap). Mode A
  = in-place value-token rewrite (comments/whitespace preserved by construction);
  Mode B = a sentinel-bracketed managed `# === RISE editor overrides ===` block.
  **Round-trip save IS implemented** (transform + property + created-camera passes),
  contradicting `ROUND_TRIP_SAVE_PLAN.md`'s stale "design-pending" header. **The real
  gap is *creation*:** only cameras can be round-trip-*created* (`AddCamera` is the
  only creation `Op`; `CloneActiveCamera` is the only creation path); created
  objects/lights/materials/media don't persist; cross-file (`>load`) dirty entities
  → `Refused`; non-camera property edits have no Mode-B fallback (un-spliceable ⇒
  Refuse). Film has introspection but no save pass wired.
- **The 7 `*Introspection` files are the engine→editable readback** (the reverse of
  the apply layer; "UI is a view of \<engine\>" today). **5 are descriptor-driven**
  (Camera/Film/Light/Object/Rasterizer — iterate `DescriptorForKeyword().parameters`,
  can't drift; but Rasterizer surfaces shape only, not live values).
  **2 are hardcoded** (`MaterialIntrospection`, 945 cpp, a ~25-branch `dynamic_cast`
  cascade; `MediaIntrospection`, string-literal matching) — the rewrite cost.
- **The de-brittling post-mortem is the *why*** ([EDITOR_STATE_AND_TRANSACTION_HARDENING.md](../gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md);
  `project_editor_state_hardening.md`). ~17 defects over 8 review rounds = **4 root
  patterns**: P-STATE (hand-assembled transaction baseline perpetually incomplete),
  P-WALK (edit logic duplicated across 5 walks), P-INVALIDATE (scattered light-gen
  bumps), P-FFMATH (`-ffast-math` folds NaN sentinels → false-green tests). H1/H2/H3
  hardening shipped (on this branch). **Every one of P-STATE/P-WALK/P-INVALIDATE is a
  symptom of multiple mutable representations that must be kept consistent** — the
  exact disease Model B's single-source-of-truth dissolves (Charter §2 rationale).

### 1.5 The agent/MCP surface (Facet 5's territory)

**100% unbuilt.** `src/Library/Agent/` **does not exist**. The MCP server, LLM
runtime, provider adapters, credential store, the safe `validate` pass, scene-text
retention on `Job`, framebuffer encoding — all net-new, all design-only in
[docs/gui/MCP_TOOL_SURFACE.md](../gui/MCP_TOOL_SURFACE.md) /
[LLM_AGENT_RUNTIME.md](../gui/LLM_AGENT_RUNTIME.md). **This is the cleanest possible
win for Model B**: there is no agent code to supersede; the agent is built *on* the
CST from day one (Facet 5).

### 1.6 Build, tests, corpus (this facet's direct territory)

- **The "touch 5 build projects" tax.** No project auto-discovers `src/Library/`
  sources. `build/make/rise/Filelist` (465 lines, canonical),
  `build/cmake/rise-android/rise_sources.cmake` (391), `Library.vcxproj` (1238) +
  `.vcxproj.filters` (3152), and the Xcode `project.pbxproj` (6761 lines, ~8
  references per file × 2 targets). **Every new source file costs a 5× edit; the
  Xcode pbxproj is the worst.** *Asymmetry:* `tests/*.cpp` IS globbed (no project
  edit to add a test).
- **Test suite: 146 standalone `tests/*.cpp` executables** (not a framework; each
  has `main()` + `assert`). The migration's regression guard is **~30 edit/save/
  parser tests**: `SaveEngineTest`, `SceneEditControllerSaveTest`,
  `SceneSnapshotTest`, `SceneRestoreTest`, `SamplerRebuildOnRestoreTest`,
  `RawTokenCaptureTest`, `SourceSpanIndexBuilderTest`, `SourceHygieneTest`, the
  `SceneEditor*` cluster (16 files), the `*IntrospectionTest` set, `AnimationProgressTest`.
  `run_all_tests.sh` builds in parallel phases (45 min → 43 s fix,
  `reference_run_all_tests_build_jobs.md`); LTO links the full object list deliberately
  (static-initializer self-registration).
- **Scene corpus: 376 `.RISEscene` files.** Comment char `#` (375/376 use it).
  **29 use the imperative macros** Charter L3 removes (13 FOR — the real rewrite
  target; the rest are constant-folding/inlining). The other ~92% are already flat
  declarative chunk lists. **204 are multi-file** (they pull shared content via
  `> load`/`> run` includes — e.g. the shared `standard_colors` painters). Per
  **D7** these includes are *flattened by inlining* at migration time, so every v7
  scene is self-contained (see §3.5).
- **Migrator precedent.** Three `tools/migrate_scenes_*.py` (v5→v6 format bump,
  color-space, iscalarpainter). Pattern: regex line-rewriter, header-guarded for
  idempotency, in-place with line-ending preservation, recursive over `scenes/`,
  documented preserve-list. `migrate_scenes_iscalarpainter.py` (v3) is **cross-file
  aware** (resolves `> run …RISEscript` includes). **This is the v6→v7 corpus
  migrator template.**

---

## 2. The supersession inventory (the centerpiece)

Fate vocabulary:
- **DELETE** — removed; its responsibility evaporates under Model B (the CST or the
  derivation engine subsumes it).
- **EVOLVE** — substantially rewritten/re-pointed but its logic or data partly
  survives.
- **REUSE** — kept essentially as-is; Model B makes it *more* central, not less.
- **UNCHANGED** — orthogonal to the redesign; not touched.

Cross-facet ownership is noted so synthesis can detect double-claims or gaps.

### 2.1 Parser & scene language

| Component | Files / anchor | Fate | Why (one line) | Facet |
|---|---|---|---|---|
| Descriptor model (`ChunkDescriptor`/`ParameterDescriptor`/`ValueKind`/`ChunkCategory`) | `ChunkDescriptor.h` | **REUSE** | The schema. Drives CST node shape, UI widgets, agent validation (Charter L6). Becomes *more* central. | 1 |
| The 153 `Describe()` methods | `AsciiSceneParser.cpp` | **REUSE** | Pure data; transfer verbatim as the per-node schema. | 1 |
| The 153 `Finalize()` methods (bag → `IJob::AddX`) | `AsciiSceneParser.cpp` | **EVOLVE** | Become the CST→engine *lowering* step; fed from CST nodes instead of text lines. Logic survives nearly verbatim. | 1/2 |
| The streaming `ParseAndLoadScene` control flow (getline/seekg) | `AsciiSceneParser.cpp:~10473` | **EVOLVE → DELETE** | One-way text→Job pass replaced by **lex → retained CST → derive**. The lexer survives; the side-effecting drive loop does not. | 1 |
| `AsciiCommandParser::TokenizeString` (lexer) | `AsciiCommandParser.cpp` | **REUSE** | The CST tokenizer foundation (`RawTokenCapture` already extends it). | 1 |
| `RawTokenCapture` | `Parsers/RawTokenCapture.{h,cpp}` | **EVOLVE → CST core** | Lossless pre-substitution token+byte+`isSymbolic`+comment capture = the CST tokenization seed; generalize to retain *all* trivia. | 1 |
| FOR/ENDFOR rewind (`struct LOOP`, seekg/tellg) | `AsciiSceneParser.cpp:10728` | **DELETE** | Imperative iteration removed (L3); replaced by declarative instancer/function nodes. | 1 |
| DEFINE/macros (`substitute_macro`, `MacroTable`, `@`/`%`) | `AsciiSceneParser.cpp:10999` | **DELETE** | Imperative metaprogramming removed (L3); typing-shortcut loops desugar to explicit entities at author time. | 1 |
| `$(...)` + inline `sin/cos/tan/sqrt/hal` + `MathExpressionEvaluator` + `MultiHalton` static | `:191`–`:355`, `MathExpressionEvaluator.{h,cpp}` | **DELETE** (mostly) | Inline arithmetic removed; *if* declarative function-expressions need eval, Facet 1 reintroduces a pure expression node (no `hal()` side-effect). | 1 |
| `AsciiScriptParser` (`.RISEscript` batch runner) | `AsciiScriptParser.{h,cpp}` | **EVOLVE / keep narrow** | Batch-command runner; survives for headless CLI scripting but is *not* a scene-authoring path under Model B. | 1/5 |
| `> load` / `> run` include machinery (multi-file scenes) | `AsciiCommandParser.cpp`, `AsciiScriptParser.{h,cpp}` | **DELETE** (kept only inside the one-shot migrator) | **D7**: v7 is single-file; the migrator *flattens* every include by inlining referenced content into the consuming document. No `> load`/`> run` in the v7 runtime. | 1 |
| `AsciiCommandParser` imperative verbs (`set/remove/rasterize/load/run/…`) | `AsciiCommandParser.cpp` | **EVOLVE (CLI) / DELETE from scene-authoring (D19, migrator-only)** | The standalone CLI/REPL command surface stays. But **per D19 v7 has no embedded `>` command layer at all**: `> set` (accelerator/global-medium) → declarative `acceleration{}`/`global_medium{}` chunks; `> set`/`> remove` overrides/tombstones → ordinary CST edits; `> load`/`> run` → flattened (D7). All embedded-`>` parsing in the *scene* path is migrator-only (see §3.4). | 1/5 |
| Scene version gate (`CURRENT_SCENE_VERSION 6`) | `:156`, `:10561` | **EVOLVE → DELETE** | Bump to declare v7. **D8**: there is no permanent v6 compatibility branch — the v6 reader lives only in the one-shot migrator and the version gate's v6 arm is **deleted** once the corpus is green (§3.1). | 1 |

### 2.2 Assembly: `RISE_API` / `IJob` / `Job` / managers

| Component | Files / anchor | Fate | Why | Facet |
|---|---|---|---|---|
| `RISE_API_Create*` factories (326) | `RISE_API.h` | **REUSE** | Pure object construction, no naming/ordering coupling. CST→engine still builds the same objects. | 2 |
| `IJob::Add*`/`Set*` apply layer (138+41) | `IJob.h`, `Job.cpp` | **REUSE, lightly EVOLVE** | Name-resolution + manager-registration is exactly what a derivation walk emits; may grow in-place replace/remove paths for incremental re-derive. | 2 |
| `Job::InitializeContainers` / default `"none"` / default shader-ops | `Job.cpp:265` | **REUSE** | Container + defaults bootstrap unchanged; the derivation engine calls it before lowering. | 2 |
| **Eager, order-dependent, fail-on-missing name resolution** | `Job::Add*` (`GetItem`-or-false) | **EVOLVE** | The hardest constraint: derivation must emit in dependency-topological order (or add a declare-then-bind two-phase). Main behavioral change Model B forces. | 2 |
| `GenericManager<T>` + name keying + identity serials | `GenericManager.h`, trivial `*Manager.h` | **REUSE** | Name-keyed store + monotonic serial = the substrate for name-path identity (L5) and incremental serial-diffing. | 2 |
| Reference counting | `Utilities/Reference.h` | **UNCHANGED** | Foundational, orthogonal. | — |
| `RequestItemUse` / `IDeletedCallback` weak-binding | `GenericManager.h`, `IManager.h` | **EVOLVE or DELETE** | Currently *unused* by the apply layer; either adopt for incremental dependent-invalidation or delete as dead weight. | 2 |
| `ObjectManager` TLAS + realize | `ObjectManager.{h,cpp}` | **UNCHANGED** | Render mechanics, assembly-agnostic. Per **D12** the realize/TLAS step (phase B) runs on the **`DerivedSceneBuilder`** before seal; the sealed snapshot **owns** the spatial index. | 2 |
| Realize-from-roots + `RenderParallelScope` freeze guard | `RayCaster.cpp`, `*::Realize`, `RenderParallelScope.{h,cpp}` | **UNCHANGED (re-sequenced by D12)** | The assembled→render-ready seam; CST-derived scene produces the same graph. **D12** orders it **build → phase-B → seal → publish** (no mutation after publication; adopt at a **pass boundary** only). | 2 |
| Light samplers (currently RayCaster-owned, `Scene.h:405`) | `RayCaster.cpp` | **EVOLVE (ownership moves into the snapshot, D12)** | **D12** moves light-sampler construction into phase B on the builder and the built samplers **into the sealed `DerivedScene`**, so a snapshot is fully render-ready and self-contained. | 2 |
| Photon-pass gate (`ConsumesScenePhotonMaps`) | `IRasterizer.h:142` | **UNCHANGED** | Rasterizer-capability gate, assembly-agnostic. Per **D12** photon maps are built in phase B and **owned by the sealed snapshot** (moved off the RayCaster). | 2 |
| `DAGObjectManager` (alternate, no-TLAS) | `DAGObjectManager.{h,cpp}` | **EVALUATE → likely DELETE** | Legacy alternate; verify it's wired anywhere before relying on it. | 2 |

### 2.3 The edit subsystem (the supersession centroid)

| Component | Files | Lines | Fate | Why | Facet |
|---|---|---|---|---|---|
| `SceneEdit.h` (27-op tagged union + prev-state) | `SceneEdit.h` | 421 | **DELETE** | Command-with-captured-prev-state subsumed by CST version diffs; the `Op` taxonomy survives *conceptually* as the structured-edit vocabulary (Facet 3), but the value-record form is dead. | 3 |
| `EditHistory.{h,cpp}` | | 416 | **DELETE** | Linear bounded undo stack → CST version history (atomic, correct by construction); the session tracks both **`headVersion` and `derivedVersion`** (**D13**, since derivation may lag the head). The cap/trim/rollback-snapshot complexity is all Model-A compensation. | 3 |
| `SceneEditor.{h,cpp}` — Apply/Undo/Redo + the 5 walks | | 2217 | **EVOLVE (gut)** | Delete the walks + history ownership + transaction rollback; **keep the invariant chain** (`RunObjectInvariantChain`, light-gen bumps, spatial invalidation) + manager-resolution as part of the CST→engine apply layer (any "apply current CST state to engine" path still needs these). | 2/3 |
| `SceneEditController.{h,cpp}` | | 5239 | **EVOLVE (split in two)** | **Keep** the render-thread / preview-scaling / gizmos / selection / pointer-dispatch / panel-cache *view* plumbing (genuine GUI, orthogonal to the doc model). **Rewrite** the mutation/undo/transaction/save surface (`SetProperty`→ CST edit, `Undo`/`Redo`, `Begin/Rollback/EndTransaction`, `RequestSave`) to mutate the CST and read panel state *from* the CST; rename routes through traced `ReferenceUse` (**D14**); the version surface exposes head vs derived (**D13**). | 3/4 |
| `DirtyTracker.{h,cpp}` | | 239 | **DELETE** | "Dirty" → "in-process: `headVersion` ≠ last-flushed version" (a pure comparison, **D13**). **D6/D17** layer an *independent* on-disk content-fingerprint signal on top, checked by the atomic save (see §2.4). | 3 |
| `SourceSpanIndex.{h,cpp}` + `ApplyOffsetDeltas` | | 443 | **DELETE** | **D2/D16**: absolute byte offsets in shared nodes force O(document) shifting and contradict structural sharing. The green tree stores **relative width** (the `NodeId` lives in the **red layer / a side-map**, not the shared green node, **D15**); absolute positions are computed on demand by the **red cursor over the rope, O(log N)** (per **D16** child sequences are a persistent balanced sequence / rope caching subtree byte-width + newline counts, so position lookup AND structural edit are O(log N) — *not* O(depth)). The span-index/`ApplyOffsetDeltas` job is the red cursor now. (`RawTokenCapture`'s lossless *trivia* capture still seeds the green tree's typed content — §2.1.) | 1 |
| `OverrideSpanIndex.{h,cpp}` | | 314 | **DELETE** | The `override_object` side-car + Mode-A/Mode-B routing exists *only* because the source isn't a mutable CST. **D8**: there are no runtime legacy (FOR/DEFINE) nodes to edit *inside*; v6 constructs exist only as migrator inputs, so every edit is a normal v7 CST edit. | 1/3 |
| `TransformSnapshot.{h,cpp}` | | 104 | **DELETE** | Base/loaded diff baselines subsumed by the CST. | 3 |
| `SaveEngine.{h,cpp}` (byte-splice, EditOp ordering, Mode-A/B, refuse cases) | | 2009 | **DELETE — byte-splice; retain the D6/D17 fingerprint guard** | 85 KB of splice logic → CST lossless serialization (save = serialize the CST). **D7** removes the cross-file Refuse case outright (v7 is single-file). **D6+D17** retain the `FileIdentity` external-mod guard's *intent*: the save path keeps a content fingerprint at load/flush and performs an **atomic save — temp-write → fsync → revalidate content-hash == loaded fingerprint → atomic rename** (D17 supersedes D6's stat-then-write CAS, which had a TOCTOU race); reload/diff/force on a fingerprint mismatch, documented rename-race residual + opt-in advisory locking. *Carry forward:* its FOR/`$(...)`/`AuthorMode` handling is the catalog of hard cases the migrator's v6 reader must get right. | 1/3 |
| `CameraIntrospection` (descriptor-driven read/write/clone) | | 1161 | **EVOLVE** | Keystone reuse: rebind `engine-getter → CameraProperty` to `CST-node ↔ CameraProperty`; the 5 descriptor-driven files port directly. | 3/4 |
| `Film/Light/Object/RasterizerIntrospection` | | ~1100 | **EVOLVE** | Same descriptor-driven port (Rasterizer lacks live-value getters — fine, the CST node *is* the value source under Model B). | 3/4 |
| `MaterialIntrospection` (hardcoded 25-type cascade) | | 1060 | **EVOLVE (rewrite)** | The big rewrite: make material chunks descriptor-driven so readback is uniform. Worth doing regardless of Model B. | 1/4 |
| `MediaIntrospection` (hardcoded slot matching) | | 225 | **EVOLVE (rewrite)** | Same — descriptor-driven medium chunks. | 1/4 |
| `ChunkDescriptorRegistry.{h,cpp}` (lazy keyword→descriptor map) | | 77 | **REUSE** | The cleanest reusable component; exactly what a descriptor-driven CST UI + validator need. Keep verbatim, ABI-clean. | 1/4 |

### 2.4 Save / round-trip (subsumed)

See `SaveEngine` and the span indices above. **Net:** the entire round-trip-save
*mechanism* (`SaveEngine` byte-splice + `OverrideSpanIndex` + `TransformSnapshot` +
`SourceSpanIndex`/`ApplyOffsetDeltas` + the Mode-A/B/refuse model) is **DELETE —
subsumed by CST canonicality**, because "save" becomes "serialize the CST"
(lossless because the green tree retained the trivia; positions come from the red
cursor over the rope in O(log N), **D2/D16**). `RawTokenCapture`'s lossless trivia
capture seeds the green tree (§2.1); it does not survive as a separate index.

**Two contracts are explicitly retained from the deleted machinery:**
- **D6 + D17 — on-disk conflict protection via an atomic save.** Two *independent*
  dirtiness concepts, both required: in-process (head-version-id ≠ last-flushed-
  version-id) *and* on-disk (a content fingerprint recorded at load and each flush).
  **D17 supersedes D6's stat-then-write compare-and-swap (a TOCTOU race) with an
  atomic save:** write to a **temp file in the target dir → `fsync` → revalidate the
  target's content hash == the loaded fingerprint → atomic `rename()` over the
  target.** If the revalidated fingerprint ≠ the last-known fingerprint the file
  changed externally, so it does **not** silently overwrite — it surfaces reload /
  diff-merge / force-overwrite (the D6 conflict UX). **Documented residual (D17):** a
  non-cooperating concurrent writer can still race the final rename (last-writer-wins
  at the FS layer); **opt-in advisory file locking** is offered for shared-storage
  setups. This is the `FileIdentity` external-mod guard's intent kept even though its
  byte-splice mechanism dies.
- **D5 + D17 — asset fingerprints.** The derivation input is `(CST, AssetManifest)`;
  the manifest maps each referenced asset path → {resolved absolute identity, content
  fingerprint}. **D17 makes the fingerprint a `(size, mtime)` fast *prefilter* → on a
  prefilter change (or whenever determinism is required) a content hash, which is the
  authoritative identity** (mtime alone is not deterministic — bytes can change with
  size+mtime unchanged); memo keys use the content hash. Asset reads are traced
  (**D4**) so a fingerprint change invalidates exactly its consumers. Output paths
  (`file_rasterizeroutput.pattern`) are excluded — they are sinks, not sources.
  (Risks R11/R12 in §6.2.)

### 2.5 Panels / bridges / cross-platform UI

| Component | Files / anchor | Fate | Why | Facet |
|---|---|---|---|---|
| Descriptor-driven properties panel (auto-row per `ChunkDescriptor` entry) | per-platform GUI + introspection | **EVOLVE** | The "any descriptor param → editable row" mechanism is exactly the dynamic UI (Facet 4); re-point its source from engine-introspection to CST nodes. | 4 |
| Hand-built accordion / category panels (`mSelectionByCategory[9]`, section-expanded) | `SceneEditController` + platform shells | **EVOLVE → mostly DELETE** | Static category panels → adaptive widgets generated per CST node (Charter L7 explicitly supersedes the hand-built accordions). | 4 |
| The bridge enum-translation getters (`panelMode`, `selectionCategory`, switch-on-int → None on miss) | Mac `.mm` / Windows bridges | **EVOLVE** | A known drift hazard (`feedback_bridge_enum_translation_audit.md`); Facet 4 should generate these or eliminate the enum boundary. | 4 |
| The C-ABI boundary (`RISE_API_SceneEditController_*`, ~70 entry points) | `RISE_API.h`, the 3 bridges | **EVOLVE (boundary moves one level out)** | Survives structurally; the library becomes CST-canonical instead of in-memory-canonical, so the boundary gains `GetSceneText`/`ValidateSceneText`/`ApplySceneText`/CST-diff entry points. `CROSS_PLATFORM_ARCHITECTURE.md` already reserves these. | 4/5 |
| Per-platform shells (Mac SwiftUI+ObjC++, Windows Qt6, Android JNI) | `build/XCode/…/RISE-GUI`, `build/VS2022/RISE-GUI`, Android | **EVOLVE (thin)** | Stay thin sinks; the dynamic-UI generation is shared C++. Cross-platform cost bounded (each shell renders schema-generated widgets). | 4 |

### 2.6 FOR / DEFINE / hal / `$(...)` macros (migrator-only; D8)

Per **D8** these exist **only as inputs to the one-shot migrator**; the v7 runtime
has no legacy nodes and never derives a v6 construct. The migrator emits the
first-class v7 replacements in the right-hand column.

| Construct | Fate | Migrator emits (D8) |
|---|---|---|
| `FOR`/`ENDFOR` | **DELETE** | `instance_array` for homogeneous instancing; typing-shortcut loops → desugar to explicit, separately-editable entities at migration time (§3.5). |
| `DEFINE`/`!`/`@`/`%` | **DELETE** | `let` (D8); referenced-once constants may instead be inlined. |
| `$(...)` arithmetic | **DELETE** | `expr(...)` (D8) where the value must stay dynamic; otherwise folded to a literal at migration time. |
| `hal()` | **DELETE** | `halton(dim,idx)` (D8) — pure, explicitly-indexed, no hidden sampler state. |

### 2.7 The scene corpus

The macro-usage buckets below are *orthogonal* to the multi-file dimension — a scene
can be both (e.g. a FOR scene that also `> load`s shared colors). The migrator
applies the **multi-file flatten (D7)** first, then the **embedded-`>`-command
removal (D19** — `> set` → `acceleration{}`/`global_medium{}` chunks, overrides/
tombstones folded), then the macro tier (§3.5).

| Bucket | Count | Fate |
|---|---|---|
| **Multi-file scenes (`> load`/`> run`)** | **204 / 376** | **EVOLVE (automated flatten, D7)** — inline includes into a self-contained v7 file (§3.5 tier 0); some duplication accepted. |
| Fully-declarative scenes (no macros) | ~347 / 376 | **EVOLVE (automated)** — version-bump only (or no-op) via `migrate_scenes_v6_to_v7.py`. |
| FOR-loop scenes | 13 | **EVOLVE (automated desugar + spot-check)** — `instance_array` / loop-unroll into explicit chunks (§3.5). |
| DEFINE / `$(...)` only | ~16 | **EVOLVE (automated)** — `let`/`expr(...)` or constant-fold (§3.5). |
| `hal()` scenes | 2 | **HAND-MIGRATE** — to `halton(dim,idx)` (D8). |

### 2.8 The build projects

| Project | Fate | Why |
|---|---|---|
| All five build file-lists | **UNCHANGED (mechanism); pay the 5× tax per new file** | New CST/derivation sources must be added to all 5; no structural change to the build system itself. *Mitigation in §6.* |
| `run_all_tests.sh` phased build | **UNCHANGED** | Already parallel; the new tests are globbed (`tests/*.cpp`). |

### 2.9 The test suite

| Bucket | Fate | Why |
|---|---|---|
| ~30 edit/save/parser tests | **EVOLVE / repurpose** | The Model-A-specific ones (`SaveEngineTest` splice paths, `SceneEditTransactionTest` rollback, `SceneSnapshotTest`/`SceneRestoreTest` clone) become the **golden render-equivalence + round-trip guard** during transition, then are retired/rewritten against CST semantics as their subjects are deleted. *(Do not delete a Model-A test until its Model-B replacement is RED-proven — §6 coverage-gap risk.)* |
| `RawTokenCaptureTest`, `SourceSpanIndexBuilderTest` | **EVOLVE → CST round-trip tests (G1)** | `RawTokenCapture`'s trivia seeds the green tree and `SourceSpanIndex`/`ApplyOffsetDeltas` are DELETE (red cursor, **D2**); the *tests'* intent survives as the byte-identical CST round-trip / loss test (gate **G1**). |
| `SourceHygieneTest`, `tools/red_prove.sh` | **UNCHANGED (keep as guardrail)** | The P-FFMATH guardrail and RED-prove discipline apply to all new code (`feedback_fix_all_bugs_regardless_of_provenance.md`). |
| The ~116 rendering/integrator tests | **UNCHANGED** | Engine behavior is untouched (INV-2: scene = f(CST) must produce the *same* scene). These ARE the render-equivalence oracle. |

### 2.10 The MCP / agent surface

| Component | Fate | Why |
|---|---|---|
| `src/Library/Agent/` (MCP server, LLM runtime, providers, credential store) | **NEW (no supersession)** | Does not exist; built on the CST from the start (Facet 5). Model B is the cleanest substrate (read CST/text, propose patch, validate→derive→render). Per **D13** the session publishes a coherent status `{headVersion, derivedVersion, status, diagnostics}`: `read_document` is stamped with `headVersion`, `read_graph`/`render` with `derivedVersion` (which may lag or be last-good), and a patch's optimistic-concurrency precondition is checked against `headVersion`. |
| `docs/gui/MCP_TOOL_SURFACE.md` / `LLM_AGENT_RUNTIME.md` / `MATERIAL_EDITOR.md` / `SPECTRAL_DIFFERENTIATORS.md` / `CROSS_PLATFORM_ARCHITECTURE.md` | **SURVIVE (text-level by design)** | Already document/text-centric; Model B *enforces* their stated intent. The `apply_scene_text` wholesale-rewrite fallback becomes the canonical path. | 5 |
| `docs/gui/TRANSACTION_MODEL.md` + `EDITOR_STATE_AND_TRANSACTION_HARDENING.md` | **SUPERSEDED** | The authoritative owners of the Model-A authority/transaction/undo machinery the charter replaces. | 3 |
| `docs/gui/VALIDATION_ARCHITECTURE.md` (Tier-1 parse-only IR) / `RENDER_COORDINATOR.md` / `ENTITY_CREATION.md` | **MOSTLY SURVIVE (re-layer onto CST)** | Tier-1 IR ≈ the CST seam; render scheduling neutral; entity-creation concepts (outliner = CST view, reference graph, safe deletion) survive — the `SceneEdit`-op *mechanism* dies. | 2/3/4 |

### 2.11 Subsystem-level summary

```
                       DELETE              EVOLVE                 REUSE / UNCHANGED
Parser/lang     FOR/DEFINE/$()/hal,   ParseAndLoadScene loop,   ChunkDescriptor model,
                MathExprEval,         RawTokenCapture→green tree,153 Describe(), TokenizeString
                ALL embedded > cmds   153 Finalize()
                (load/run/set/remove,
                 D7/D19),
                v6 path (after green)
Assembly        (DAGObjectManager?)   eager-order resolution,    RISE_API factories, IJob Add*,
                                      RequestItemUse?            GenericManager, Reference,
                                                                 realize/TLAS/photon seam
Edit subsystem  SceneEdit, EditHistory, SceneEditor (gut to     ChunkDescriptorRegistry
                DirtyTracker,         invariant-chain),
                OverrideSpanIndex,    SceneEditController (split),
                TransformSnapshot,    7 Introspection (rebind),
                SourceSpanIndex/      Material/Media (rewrite)
                ApplyOffsetDeltas,
                SaveEngine (splice;
                D6/D17 fingerprint +
                atomic-save retained)
Panels/UI       hand-built accordions Properties panel→dynamic,  —
                                      bridges, C-ABI boundary,
                                      per-platform shells
Agent/MCP       —                     (specs survive)            — (Agent is NEW)
Build/tests     v6 path tests         ~30 edit tests (repurpose) 5 build lists, run_all_tests,
                (after corpus green)                             116 render tests, SourceHygiene
```

**The supersession is concentrated.** ~40% of `src/Library/SceneEditor/` is
"scaffolding that exists *because* there is no CST" (DELETE), ~40% is genuinely
reusable view/binding/descriptor infra (REUSE/EVOLVE), and the two big files split.
The engine, the apply layer, the managers, and the render-ready seam are
**untouched**. The agent layer is **net-new on top of the CST**.

---

## 3. Phased, non-big-bang migration

The governing constraint: **never a flag-day rewrite.** RISE must keep rendering the
376-scene corpus and passing its 146 tests at every commit. The strategy is a
**strangler-fig** around the parser: stand up the CST *beside* the live one-way
parse, prove equivalence, then move clients (derivation, then UI, then save, then
agent) onto it one at a time, deleting Model-A scaffolding only as each client lands.

### 3.0 The load-bearing enabling invariant

Everything downstream depends on one property the code does **not** have today and
**both** Model A and Model B need: **a side-effect-free parse that produces a
retained representation, separable from `IJob` construction.** Today the parser
builds the Job *as it parses* (`VALIDATION_ARCHITECTURE.md`'s central impossibility
proof). Phase 1 below *is* establishing that seam. Frame it as Model-B-native, not a
bolt-on.

### 3.1 Transitional dual-path, then the singular migrate-then-DELETE contract (D8)

The migration is **non-big-bang in its *sequencing*** but **singular in its
*contract*** (**D8**): migrate the whole corpus to v7, then **DELETE** the v6
parser + macro preprocessor + the version gate's v6 arm. There is **no permanent v6
coexistence** and **no runtime legacy nodes**. The v6 reader lives **only inside the
one-shot migrator** and is dropped once the corpus is green.

During the transition, a v6-read path and the v7 CST path share one lex boundary and
converge at `IJob::Add*` — that shared convergence is what makes the equivalence
harness (§3.6) a mechanical oracle. This is a *temporary* scaffold to migrate safely,
**not** a long-lived dual runtime.

```
   .RISEscene  ──► lexer (RawTokenCapture, shared) ──┐
                                                      │
                                          ┌───────────┴───────────┐
                              v6 read     │                       │ v7 path
                           (MIGRATOR-ONLY,▼                       ▼  (Model-B,
                            transitional) ParseAndLoadScene  Build green tree  canonical)
                                          (one-way, today)   (retained CST)
                                          │                       │
                                          ▼                       ▼
                                  IJob::Add* (apply)  ◄── derive ──┘
                                          │            (same Finalize→Add*)
                                          ▼
                                   Scene → Render  (identical downstream)
                            ── after corpus green: the v6 read path is DELETED ──
```

- **The convergence point is `IJob::Add*`.** Both paths emit the *same* apply calls,
  so the engine, managers, realize seam, and all 116 render tests are a **shared
  oracle** — a v6 scene and its v7 migration must produce byte-identical renders.
  This is the equivalence harness (§3.6).
- **Format gate (O3 resolved by D8).** The version header selects the path during
  transition: `RISE ASCII SCENE 6` → the transitional v6 read; `RISE ASCII SCENE 7`
  → CST path. Migration to v7 is an explicit `tools/` step (§3.5) run over the whole
  corpus. **Once the corpus is migrated and green, the v6 arm is DELETED and v7 is
  the sole runtime format.** There is no indefinite "old scenes keep working"
  guarantee — the owner's migrate-everything permission (01-DECISIONS.md preamble)
  is what makes deleting the v6 path correct rather than a breakage.
- **There are NO legacy runtime nodes (D8).** `FOR`/`DEFINE`/`$(...)`/`hal` are
  **inputs to the migrator only**; the runtime CST never contains a `LegacyForLoop`/
  `LegacyMacroDef`/`LegacyExprValue`/`LegacyMacroRef` and never derives a v6
  construct. The migrator emits `instance_array`/`let`/`expr(...)`/`halton(dim,idx)`
  (and inlined includes per D7), so the v7 runtime sees only first-class v7 nodes.
  *(The earlier "open a v6 scene read-only with opaque FOR/`$()` nodes in the UI"
  idea is **removed** — it implied permanent legacy-node machinery D8 forbids. The
  path to editing an old scene in the new UI is: run the migrator, then open the v7
  output.)* *(Assumption confirmed against Facet 1: the v7 CST grammar subsumes v6's
  declarative chunks 1:1 because the descriptors are shared.)*

### 3.2 The first slice — the corrected phased fixture + shared gates (D18, amends D10)

The first slice is **not** a single nominated chunk. **D18** (amending D10's fixture;
gates unchanged) replaces the divergent first-slice proposals across docs with **one
corrected canonical phased fixture** (each phase additive; the prior phase's gates
keep passing) and **one shared gate set G1–G5** referenced by every facet. **This doc
defers to [01-DECISIONS.md §D18](01-DECISIONS.md) for the authoritative phase
definitions** (and §D10 for the gates) and does not re-define them. The corrections
D18 makes over D10: `uniformcolor_painter` has **no** reference, so the first real
reference is a `lambertian_material` binding it; D10's "geometry→material→object chain"
had **omitted** the material node; and G5 (asset invalidation) needs an **asset-backed
node**, added as phase 6. In brief:

**Corrected phased fixture (see §D18 for full rationale):**
1. **`sphere_geometry`** — simplest chunk (2 scalar params, no refs/repeats/exprs).
2. **`+ uniformcolor_painter` AND `+ lambertian_material { reflectance <the uniform
   painter> }`** — the material is the **first reference** (exercises the ref-picker +
   the dependency edge, **D4**).
3. **`+ standard_object { geometry <sphere> material <lambertian> }`** — the real
   **geometry→material→object** three-node chain; **rename** integrity across refs
   (**D14**, via traced `ReferenceUse`), the dependency graph (D4) end-to-end.
4. **`+ expr(...)`** on one `Double` param — the expression sublanguage + traced-
   input invalidation (D4).
5. **`+ instance_array`** replacing a nested `FOR` (migrated `loops.RISEscene`) —
   generators (D7/D8 migration).
6. **`+ image_painter` (or a mesh-backed geometry)** — an **asset-backed node** so
   **G5** (AssetManifest fingerprint invalidation, **D17**) and the external-file-
   conflict path (**D6/D17**) are actually testable.

**Shared gates (G1–G5, defined in §D10; unchanged by D18; summarized here for this
facet's harness):** G1 round-trip byte-identity, G2 incremental-derive latency (< 50 ms
on a Sponza-class scene), G3 minimal invalidation (only the changed node's forward
cone), G4 versioning (gesture = one undo unit; round-trip-after-undo byte-identical),
G5 external inputs (asset-fingerprint change re-derives consumers, **D5/D17**; external
file change caught by the atomic save, **D6/D17**).

This facet's job at the first slice is the **safety net** (§6.1), not the fixture
itself: the render-equivalence harness (§3.6) is the substrate G1–G5 run on. The
fixture phases map cleanly onto the harness — phase 1 isolates CST/derivation
mechanics on a leaf chunk, phase 2 adds the first reference (painter→material), phase 3
brings in the order-dependence problem (§4.3) and cross-ref rename, phase 4 the
expression evaluator, phase 5 the FOR→`instance_array` migration (§3.5), phase 6 the
asset-backed node. G5 is where **D5/D6/D17** (asset manifest, atomic save) first get
exercised end-to-end — on phase 6's asset-backed node.

### 3.3 The phase sequence

Each phase is independently shippable and leaves the tree green.

| Phase | Name | T-shirt | What ships | What it deletes (only when proven) |
|---|---|---|---|---|
| **P0** | Side-effect-free parse seam + CST scaffold | **L** | Lexer (`RawTokenCapture`-derived) produces a retained **green tree** (relative widths, **D2**; child sequences as a persistent rope, **D16**; `NodeId` in the red layer/side-map + the content-hash/derivation-key/`NodeId` split, **D15**); a CST→`IJob` derivation walk for the **declarative subset** with **traced** dependency edges + `ReferenceUse` records (**D4/D14**) over `(CST, AssetManifest)` with `(size,mtime)`-prefilter→content-hash fingerprints (**D5/D17**); v7 header recognized; both paths converge at `Add*`. Equivalence harness (§3.6) green on the macro-free corpus (~347 scenes). | nothing yet |
| **P1** | First slice: the corrected phased fixture (D18) | **M** | The §D18 phased fixture (`sphere_geometry` → `+uniformcolor_painter`+`lambertian_material` → `+standard_object` chain → `+expr` → `+instance_array` → `+image_painter`/mesh) driven text⟷CST⟷derive⟷schema-widget⟷incremental re-derive⟷CST-version undo⟷agent-patch, behind a GUI feature flag, gated by **G1–G5**. The interactive-bar latency (G2) and lossless round-trip (G1) tar-pits are pressure-tested here; phase 6's asset-backed node exercises G5 (D17). | nothing yet |
| **P2** | Derivation engine: traced deps + incremental + order-independence | **XL** | CST→Scene as a memoized, deterministic, incremental function (Facet 2); the memo key = the node's **derivation key** (trivia-INsensitive: typed values + child structure) + traced-input versions (**D4/D15**); topological emission solves the eager-order constraint; localized edit re-derives only the changed node's forward cone (G3); the derivation cache is **version-scoped/persistent** with an explicit edge lifecycle (**D20**). Hooks into the existing realize/TLAS/photon seam. **Reverse-dependency-closure COW** (**D11**): a new version copies the changed node + every node that transitively references it up to the roots, sharing everything else by refcount (first impl may full-rebuild, then add closure-tracking). **Build → phase-B → seal → publish at a PASS boundary** (**D12**); the sealed snapshot owns geometry/TLAS/light-samplers/photon-maps. | nothing yet |
| **P3** | CST versioning replaces the edit model | **L** | Undo/redo = CST version-DAG pointer move (**D1/D2**); a gesture coalesces to one undo unit (**D1**); `NodeId` lineage (red layer/side-map, **D15**) survives renames (rename via traced `ReferenceUse`, **D14**) and best-effort reparse (unmatched durable refs flagged, **D15**); the session exposes **`headVersion` AND `derivedVersion`** (**D13**); `SceneEditor`/`SceneEditController` mutation surface re-pointed onto the CST (Facet 3). | **`SceneEdit`, `EditHistory`, `DirtyTracker`, `TransformSnapshot`, `SourceSpanIndex`/`ApplyOffsetDeltas`** (their subjects gone; positions via the red cursor over the rope, D16). |
| **P4** | Dynamic UI from descriptors | **L–XL** | Properties/outliner generated per CST node from descriptors (Facet 4); widgets bind to `NodeId` (the stable lineage id, **D15**; addressed by name-path, **D9**); panels surface `headVersion` vs `derivedVersion` (**D13**); the 5 descriptor-driven introspection files rebind to CST nodes; hand-built accordions retired. | hand-built accordion panels; engine-introspection-as-UI-source. |
| **P5** | Save = serialize CST (+ D17 atomic save) | **M** | "Save" writes the CST losslessly (Facet 1); the save path keeps an on-disk fingerprint and performs an **atomic save — temp-write → fsync → revalidate content-hash → atomic rename** (**D17** supersedes D6's CAS; documented rename-race residual + opt-in advisory locking). All `SaveEngine` Refuse cases evaporate (single-file per **D7**; no legacy nodes per **D8**; no `>` command layer per **D19**). | **`SaveEngine` byte-splice, `OverrideSpanIndex`** + the Mode-A/B model (D6/D17 fingerprint + atomic save retained, not the splice). |
| **P6** | Material/Media descriptor rewrite | **L** | `MaterialIntrospection`/`MediaIntrospection` become descriptor-driven (worth doing regardless). | the 25-branch `dynamic_cast` cascade. |
| **P7** | Agent surface on the CST | **L–XL** | `src/Library/Agent/` MCP server: read CST/text, propose patch, validate→derive→render, structured errors (Facet 5). GUI-as-just-another-agent unification. | nothing (net-new). |
| **P8** | Corpus fully on v7; **DELETE** the v6 path | **M** | Whole corpus migrated (§3.5) — single-file v7 (includes flattened, **D7**) — and render-equivalent. Then the v6 reader is dropped. | **the v6 `ParseAndLoadScene` read path + all macro/FOR/`$()`/`> load`/`> run` code + the version gate's v6 arm** (**D8** — the single deletion contract, not an optional window). |

**Why this order.** P0 establishes the seam everything needs. P1 de-risks the two
tar-pits on the smallest fixture phase *before* committing to the XL derivation
work. P2 (derivation) must precede P3/P4 (edit/UI) because they consume it. P3
before P4 (UI needs CST versioning to drive undo). Save (P5) can come after the UI
because the transitional v6 read covers old scenes until then. The agent (P7) and
the v6-path deletion (P8) are last because they depend on the full stack. **No phase
requires a flag day; deletions happen only after the replacement is RED-proven and
the equivalence harness is green. P8's deletion is the singular D8 contract — the v6
path is removed once the corpus is green, not kept behind an indefinite deprecation
window.**

### 3.4 Handling the embedded `>` commands (ALL removed in v7 — D19)

Scenes use embedded `> set`/`> remove`/`> load`/`> run` commands, and `SaveEngine`
emits managed `override_object` blocks + `> remove` tombstones (6 families only).
**Per D19, v7 has NO imperative command layer at all — every embedded `>` command is
removed.** The fates split three ways:

- **`> set` (accelerator / global-medium / …)** → **declarative chunks (D19).** The
  three surviving `> set` forms migrate to normal descriptor-driven chunks — e.g. an
  `acceleration { … }` chunk and a `global_medium { … }` chunk — so all engine config
  is a declarative *derivation input* (F2 needs this), not an imperative side effect.
- **`> set`/`> remove` overrides & tombstones on entities** collapse into ordinary CST
  edits: an override is a structured edit to the referenced node; a tombstone is a
  delete in the CST. The managed `override_object` side-car block disappears (it
  existed only because the source wasn't a mutable CST).
- **`> load`/`> run`** — per **D7** they are NOT first-class v7 nodes; the migrator
  **flattens** each include by inlining the referenced content into the consuming
  document. Consequently the entire **cross-file edit problem evaporates** (there is
  no second file to edit) — a *deletion*, not a "cross-file edits become normal"
  generalization.

The migrator removes **all** embedded `>` commands; the v7 runtime never sees an
imperative line (§2.1, §3.5 tier 0). *(If library-sharing demand returns, D7 reserves
a future declarative `import` chunk — explicitly out of core v7, designed as its own
feature, never the imperative `> load`.)* *Assumption flagged for Facet 3:* the
deletion-persistence gap (camera/media/shader tombstones don't exist today) is on the
critical path for "all-family create/delete through one pathway."

### 3.5 Scene-corpus migration tooling

**`tools/migrate_scenes_v6_to_v7.py`** — built on the proven migrator pattern
(regex line-rewriter, header-guarded idempotency, in-place with line-ending
preservation, recursive over `scenes/`, documented preserve-list; cross-file aware
like `migrate_scenes_iscalarpainter.py`). **This migrator is the *only* place the
v6 reader, the FOR/DEFINE/`$()` expansion logic, and the embedded-`>`-command
parsing survive (D8/D19);** its output is self-contained single-file v7 (D7) with
**no imperative `>` layer at all** (D19). It handles these tiers:

0. **Multi-file flatten (~204 scenes, D7):** **inline** every `> load`/`> run`
   include into the consuming document, producing a self-contained v7 file. Shared
   content (e.g. the `standard_colors` painters) is **duplicated into each consumer**
   that referenced it. The migrator already resolves includes (the
   `migrate_scenes_iscalarpainter.py` v3 cross-file precedent), so this is a
   resolve-then-splice over the existing resolver. **Accepted cost:** some
   duplication across scenes — cheap and diff-visible in a generated/migrated corpus
   the owner controls (D7). After flatten, `> load`/`> run` no longer appear in any
   v7 scene, and the multi-file machinery is DELETE in the runtime (§2.1).
0b. **Embedded `>`-command removal (D19):** rewrite the remaining embedded commands so
   v7 has **no imperative layer**. `> set` of the **accelerator** and **global-medium**
   (and any other surviving `> set` form) → a declarative **`acceleration { … }`** /
   **`global_medium { … }`** chunk; `> set`/`> remove` **overrides/tombstones** on
   entities → fold the override into the referenced chunk (or drop the tombstoned
   entity). After this tier no `>` line appears in any v7 scene, and the embedded-`>`
   parsing is DELETE in the runtime (§2.1, §3.4). (`> load`/`> run` were already
   removed by tier 0.)
1. **Declarative scenes (~347):** rewrite only the version header `6`→`7` (and any
   trivially-renamed keyword). Idempotent; a no-op on already-v7 files. *This is
   ~92% of the corpus, fully automated, zero render risk* (the chunks are identical;
   only the header changes the parse path). Multi-file declarative scenes also get
   tier-0 flatten first.
2. **DEFINE / `$(...)` scenes (~16):** **constant-fold.** The migrator *reuses the
   engine's own evaluator* (`MathExpressionEvaluator` + the macro table) to evaluate
   each `$(...)` and `@NAME`/`%NAME` reference to a literal, then inlines it. This is
   the safest possible fold because it's the same code the parser uses — the
   migrated literal is by-construction what the parser would have computed.
   `DEFINE`d constants that are referenced once are inlined; constants referenced
   many times *may* be preserved as a declarative `define` node if Facet 1 provides
   one (else inlined everywhere).
3. **FOR-loop scenes (13):** **desugar by unrolling.** The migrator runs the
   existing FOR-expansion logic (the seekg/macro-injection loop) but instead of
   feeding chunks to a Job, it *emits the expanded chunk text* with `!I` name
   interpolation resolved (`box0`, `box1`, …) and `$(@I*…)` folded. Output =
   explicit, separately-editable `standard_object` chunks (Charter L3: "typing-
   shortcut loops desugar into explicit, separately-editable entities at author
   time"). **Homogeneous instancing** that *should* stay declarative (large arrays)
   is a hand-decision: convert to a declarative instancer node (the
   `path_instances_geometry`/guilloché-generator precedent) rather than unrolling
   thousands of chunks. Flag these for human review; auto-unroll only small loops
   (say ≤ 64 iterations).
4. **`hal()` scenes (2):** **hand-migrate to `halton(dim,idx)` (D8).** The stateful
   sampler-in-the-parser (each call advances a hidden sequence) is replaced by the
   pure, explicitly-indexed `halton(dim,idx)` form D8 prescribes — equivalent values,
   no hidden state. Two scenes — trivial manual cost; can also be baked to literals
   where the index is fixed.

**Verification: render-equivalence is the acceptance test.** For every migrated
scene, render the v6 original (legacy path) and the v7 output (CST path) at fixed
seed/samples and assert pixel-identical (or within a tight epsilon for any FP
reassociation). This reuses the existing variance/HDR-compare tooling
(`bin/tools/HDRVarianceTest`, `variance-measurement` skill) and the 116 render
tests as oracles. **A migrated scene that doesn't render-match is a migrator bug, not
an accepted loss.** The harness runs in CI over the whole corpus (sequentially —
renders take all cores, `feedback_render_sequentially.md`).

**Migration is incremental and per-scene reversible *during* the transition, but
the end-state is singular (D8):** while the transitional v6 read path is still in
the binary, the corpus can be migrated in waves (e.g. `Tests/` first as the oracle,
then `FeatureBased/`, then `Internal/`), and a bad migration is just `git checkout`
of that scene. Once the **whole** corpus is migrated and render-equivalent, the v6
read path is **DELETED** (P8) — there is no permanent two-format runtime. "Open an
old scene later" means "run the migrator on it," not "the v6 reader is still there."

### 3.6 The render-equivalence harness (the safety net for the whole migration)

The single most valuable piece of migration infrastructure. Because both the legacy
and CST paths converge at `IJob::Add*`, **the same scene through both paths must
produce the same `Scene` and therefore the same render.** The harness:
- parses a scene through path A (legacy) and path B (CST→derive), renders both at
  fixed seed/samples/`oidn_denoise FALSE` (denoise off for fair A/B,
  `feedback_oidn_denoise_always_on.md` exempts diagnostics), and asserts equality;
- runs over the macro-free corpus from P0 onward (it's the P0 exit criterion);
- gates every subsequent phase (a phase that breaks equivalence doesn't ship).

This converts "did we faithfully replicate the parser" from a hope into a
mechanically-checked invariant (INV-2), and it's the test that lets us delete
Model-A scaffolding with confidence.

---

## 4. Hard problems & open questions

### 4.1 Tar-pit #1 — incremental-derivation latency under the interactive bar (INV-3, O2)

**The risk.** A localized CST edit must re-derive *only* the affected subgraph, fast
enough for the interactive cadence. Today there is no incremental derivation at all
— the parser rebuilds the whole world. If the derivation engine (Facet 2) can't
isolate the affected subgraph, every slider drag rebuilds the scene and the bar
stutters.

**Why it's hard.** (a) The dependency graph is real: a painter feeds a material
feeds an object; editing the painter must re-derive its dependents but *nothing
else*. (b) Some edits are cheap (a color value → re-derive one painter, no spatial
change → `DirtyScope::Camera`-class re-render) and some are catastrophic (geometry
displacement → re-realize + rebuild TLAS + re-shoot photons). The engine must
classify edits by blast radius — RISE *already has this taxonomy* in
`SceneEditor`'s `DirtyScope` enum (`ObjectTransform`/`Camera`/`Time`/`TimeAndPhotons`)
and the realize/TLAS/photon seam; Facet 2 must reuse it, not reinvent it.

**Mitigation (D1 gesture model; D11 closure-COW; D12 seal; G2 latency gate).** **D1**
pins the gesture model: each pointer-move advances the **uncommitted head** and derives
a **cheap, ephemeral preview snapshot** (debounced per O2); at gesture-end the
intermediate roots **coalesce into ONE committed version** (one undo unit). Preview
snapshots derive the **uncommitted head** (not just committed versions). The COW
snapshot is a **reverse-dependency-closure copy (D11)**: it re-derives the changed node
**plus every node that transitively references it up to the roots** (because the
engine's scene is a raw-pointer graph — a shared *referrer* of a changed node would
otherwise keep pointing at the old node) and shares everything *outside* the closure by
refcount. Cost is **O(closure / fan-in)**, not O(scene) — a transformed object is
{object} + the TLAS spine path-copy (O(log N)); a widely-shared painter is its full
referrer closure (larger but bounded by fan-in, still dwarfed by the render). The
snapshot is **built → phase-B (realize/TLAS/light-samplers/photon-maps) → sealed →
published, adopted only at a PASS boundary (D12)** — never mutated after publication.
So a drag is "one debounced closure re-derive of one forward cone" (G3), not "re-derive
at frame rate." **The gesture preview is CST state (the uncommitted head), not a
side-channel of non-CST mutable state** — so it does *not* reintroduce the INV-1 "sync
two things" hazard the old text feared. **First-slice P1 measures this directly** under
gate **G2** (a single-parameter edit re-derives in < 50 ms on a Sponza-class scene);
the §D18 phase-1 leaf (`sphere_geometry`) is the cheapest surface to fail fast on
*before* the XL derivation work. If even the cheapest edit can't meet G2, the
interactive premise needs rethinking first.

### 4.2 Tar-pit #2 — lossless CST formatting / comment preservation (INV-4, O1)

**The risk.** A structured edit must not gratuitously reformat untouched text; a
hand-edited file must survive parse→serialize unchanged. This is the single
highest-risk *correctness* requirement of the pivot. `TRANSACTION_MODEL.md` already
documents that re-serialize-from-introspection **cannot** reproduce unsupported
chunks, `>` commands, macros, or FOR loops — which is exactly the silent-drop
failure Model B must *not* inherit.

**Why it's hard.** A lossless CST must retain *everything* the current proto-CST
discards: inter-token whitespace, blank lines, comment placement (own-line vs
trailing), indentation style (tabs — RISE uses hard tabs,
`feedback_tab_indented_edits.md`), and line endings (CRLF/LF). It must round-trip
constructs the structured editor *can't* mutate (in v6 transitional CSTs: FOR, `$()`
— preserved opaquely via the `isSymbolic` flag the proto-CST already has). And a
*structured* edit to one parameter must rewrite only that value's token, leaving the
rest of the file byte-identical — exactly what `SaveEngine`'s Mode-A splice does
today, which is the proof-of-concept that this is achievable at span granularity.

**Mitigation.** (a) **Reuse the proven trivia capture, not the offset index.**
`RawTokenCapture` already retains the lossless token/whitespace/comment/`isSymbolic`
content — that *content* seeds the green tree's typed nodes. But per **D2** the green
node stores **relative width** (byte length incl. trivia), **not** an absolute span,
and per **D15** the stable `NodeId` lives in the **red layer / a side-map** (a shared
green node is reused at many occurrences, so it can't carry one id), not in the green
node itself; `SourceSpanIndex`/`ApplyOffsetDeltas` are **DELETE** because a length
change would force O(document) offset-shifting and break structural sharing. Per
**D16**, a node's child sequence is a **persistent balanced sequence / rope** caching
each subtree's aggregate byte-width + newline counts, so absolute positions for
diagnostics/edits come from the **red cursor over the rope in O(log N)** (not O(depth)
— a `Document` with 10 000 chunks would be O(N) with vector children). (b) **The
lossless-CST pivot is decided (D2), not an open fork.** "Save = serialize the CST" is
lossless *by construction* because the green tree holds the trivia and the red cursor
recomputes positions; a structured edit produces a new root by **path-copy** (O(depth)
new green nodes; siblings shared) with the rope giving **O(log N)** child insert/remove
and lookup, so an edit to one parameter rewrites only that node's content and
re-serializes byte-identically elsewhere. The previously-flagged "text-canonical /
buffer-is-truth" alternative is **not** the chosen model — the red-green tree gives
lossless round-trip *and* O(log N) structured edits without the splice machinery, which
is why D2 supersedes the fork. This fork is **resolved by D2 + D16** (red-green tree
over a rope), so `SaveEngine`'s byte-splice is unambiguously **DELETE** (only the
D6/D17 fingerprint + atomic-save contract is retained, §2.4). (c) **The acceptance
test is mechanical:** parse→serialize every corpus file and assert byte-identical (a
"CST loss test," gate **G1**; the evolution of `RawTokenCaptureTest`).

### 4.3 Order-independence vs eager name resolution (INV-2)

**The problem.** INV-2 requires scene = f(CST) with *no hidden order-dependence*,
but the apply layer resolves dependencies eagerly by name and hard-fails on missing
deps (file order = assembly order). A CST node can reference a node declared *later*
in the file — legal in a declarative document, illegal in today's apply order.

**Mitigation (Facet 2 owns this; edges are *traced*, D4).** The derivation engine
emits `Add*` calls in **dependency-topological order**. Per **D4** the edges are
**recorded as a by-product of derivation** (the reactive/tracing model), *not*
pre-computed statically from `referenceCategories` — because dynamic references
(`timeline.element` resolved via `element_type`, `timeline.animation`) are plain
strings invisible to a static scheme but captured automatically when `derive()`
actually performs the resolution. Each resolved reference is recorded as a
**`ReferenceUse { sourceValueNodeId, targetNodeId }`** (D14), which is what **rename**
rewrites referrers from; `referenceCategories` is demoted to a **UI-picker hint only**
(D14), *not* the rename or dependency source of truth. The traced edges form a DAG;
cycles are a structured validation error before any `Add*`. This removes
document-level order-dependence while reusing the eager apply layer unchanged. A
corpus scan during P0 confirms the reference graph is acyclic (expected — RISE has no
recursive material references today).

### 4.4 Identity stability (INV-5, L5 — resolved by D9, refined by D14/D15)

**D9** resolves this with **dual identity**, and **D15** separates three concepts that
were conflated: a **content hash** (green, lossless, trivia-*sensitive*) drives
structural sharing/dedup and carries **no** identity (so identical subtrees share one
green node); a **derivation key** (semantic, trivia-*insensitive*: typed values + child
structure + traced-input versions, D4) is the memo cache key (a whitespace-only edit is
a cache hit); and the **lineage identity `NodeId`** is a per-**occurrence** stable id
living in the **red layer / a side-map, NOT in the shared green node** (a shared green
node is reused at many occurrences, so it cannot carry one id). The `NodeId` is what
undo lineage, UI widget bindings, and durable agent references key on; **name-path**
(`objects/sphere.material`) is the human/agent **addressing scheme**, resolved to a
`NodeId` *within a given version* (it changes on rename, by design). **Rename** is a
`NodeId`-preserving edit: it rewrites the name token in place (same `NodeId`) and
**rewrites all referrers from the traced `ReferenceUse { sourceValueNodeId, targetNodeId }`
set (D14)** — *not* `referenceCategories`, which is only a UI-picker hint and is blind
to dynamic refs (`timeline.element`/`.animation`); for referrers in nodes that didn't
derive, fall back to descriptor-provided reference resolvers, and **flag any referrer
that cannot be resolved — never silently rename it**. NodeId-keyed bindings survive
automatically. On a text edit, **a structured edit preserves `NodeId` exactly** (it
targets a known node); a **whole-region reparse** matches new green nodes to prior
`NodeId`s by position + content (rust-analyzer-style reuse) **but this is best-effort
(D15)** — identical repeated rows are genuinely ambiguous, so **unmatched durable
references are INVALIDATED (flagged), not silently remapped**. The managers' per-name
identity serials remain the substrate, but identity is no longer name-fragile. (The
charter's round-4 "name-reuse identity serial" was the Model-A patch D9 makes
first-class.)

### 4.5 The 5× build tax during transition

Every new CST/derivation/agent source file must be added to all 5 build projects;
the Xcode pbxproj is especially painful (~8 refs × 2 targets). During an XL effort
that adds many files, this is real friction and a real source of "forgot to add it
to project N → green on macOS, broken on Windows" errors. *Mitigation in §6.*

### 4.6 Open questions — now resolved by 01-DECISIONS.md

The open questions this section previously carried are **closed** by review rounds 1 & 2;
recorded here so a re-reviewer sees the resolution rather than a live fork:

- **O1 (canonical form) → resolved by D2, refined by D15/D16.** Red-green tree
  (lossless green nodes storing relative width + content hash; `NodeId` in the red
  layer/side-map; derivation key for the memo) over a **persistent rope** (O(log N)
  red cursor and structural edit); `SaveEngine` byte-splice is DELETE, not a fork.
  §4.2, §2.4.
- **O2 (interactive bar) → resolved by D1, refined by D11/D12.** Gesture =
  uncommitted-head + ephemeral preview snapshot, coalescing to one undo unit; the
  snapshot is a **reverse-dependency-closure COW (D11)** built→sealed→published at a
  **pass boundary (D12)**; latency bounded by gate **G2** (< 50 ms incremental
  derive). §4.1.
- **O3 (format coexistence) → resolved by D8.** A *transitional* header-gated read of
  v6 during migration, then a singular **DELETE** of the v6 path — no permanent
  coexistence, no legacy runtime nodes. §3.1.
- **Declarative `define`/expression nodes → resolved by D8/D19.** The migrator emits
  `let`/`expr(...)`/`halton(dim,idx)` (and `instance_array` for FOR, and
  `acceleration{}`/`global_medium{}` for `> set`); the runtime CST carries first-class
  v7 nodes, never legacy ones, and has **no imperative `>` layer at all** (D19). §3.5.
- **Reuse of `DirtyScope` + realize/TLAS/photon seam → assumed reuse, consistent with
  D1/D4/D11/D12.** The closure-COW snapshot (D11) + traced-input invalidation (D4)
  drive the blast radius; Facet 2 reuses the existing seam, run as **phase B on the
  builder before seal** so the snapshot owns geometry/TLAS/light-samplers/photon-maps
  (D12; §2.2). The remaining genuine unknown is whether the corpus reference graph is
  fully acyclic (a P0 scan, §4.3) — an empirical check, not a design fork.

---

## 5. Cross-facet dependencies & assumptions

This facet reconciles 1–5. The cross-facet questions are now **pinned by D1–D20**;
each row below cites the deciding decision so a re-reviewer can confirm there is no
live fork. (The sibling docs `10`–`50` are being conformed to the same decisions.)

| Assumed of | Now pinned by | Decision |
|---|---|---|
| **Facet 1 (CST)** | The v7 CST grammar subsumes v6's declarative chunks 1:1 (shared descriptors) and retains *all* trivia losslessly. Overrides/tombstones are CST edits; **ALL embedded `>` commands are removed — `> set` → `acceleration{}`/`global_medium{}` chunks (D19); `> load`/`> run` flattened (D7)**. FOR→`instance_array`, DEFINE→`let`, `$()`→`expr(...)`. `RawTokenCapture`'s trivia seeds the **green tree**; nodes store **relative width** (no offset) and child sequences are a **persistent rope** (O(log N), **D16**); the `NodeId` lives in the **red layer / side-map** (**D15**), not the shared green node; `SourceSpanIndex`/`ApplyOffsetDeltas` are deleted (red cursor over the rope). Three distinct hashes/ids: content hash (sharing) / derivation key (memo) / `NodeId` (lineage), **D15**. | **D2, D7, D8, D15, D16, D19** |
| **Facet 1** | Canonical form = lossless red-green CST. The "text-canonical" alternative is **not** taken; `SaveEngine` byte-splice is DELETE (the external-conflict guard retained as the **atomic save** — temp-write→fsync→revalidate→rename, **D17**). | **D2** (closes the former "biggest fork") |
| **Facet 2 (derivation)** | CST→Scene is incremental/memoized/deterministic; the memo key = the **derivation key** (trivia-INsensitive typed values + child structure, **D15**) + **traced**-input versions over `(CST, AssetManifest)`; emits `Add*` in topological order from traced edges (§4.3); reuses the `IJob` apply layer, `DirtyScope` taxonomy, and realize/TLAS/photon seam; publishes **reverse-dependency-closure COW** snapshots (**D11**) **built → sealed → swapped at a PASS boundary** (**D12**), the sealed snapshot owning geometry/TLAS/light-samplers/photon-maps; the derivation cache is version-scoped/persistent with an explicit edge lifecycle (**D20**). | **D1, D4, D5, D11, D12, D15, D17, D20** |
| **Facet 3 (edit model)** | Undo/redo = CST version-DAG; a gesture = one undo unit; the `SceneEditor` mutation surface re-points onto the CST; invariant chain + manager-resolution survive as the apply path; **lineage identity is `NodeId` (in the red layer/side-map, D15), addressed by name-path (D9)**; **rename rewrites referrers from traced `ReferenceUse` records (D14), not `referenceCategories`**; the session exposes **both `headVersion` and `derivedVersion`** (D13); deletion-persistence grows to all families. | **D1, D9, D13, D14, D15** |
| **Facet 4 (dynamic UI)** | UI is a pure function of CST + descriptors; widgets bind to **`NodeId`** (the stable lineage identity, D15) addressed by name-path; the 5 descriptor-driven introspection files rebind to CST nodes; hand-built accordions retire; the C-ABI boundary gains CST/text entry points (and surfaces `headVersion` vs `derivedVersion`, D13). | **D9, D13, D15** |
| **Facet 5 (agent)** | `src/Library/Agent/` is net-new on the CST (read/propose-patch/validate→derive→render); GUI is "just another agent"; the `docs/gui/` agent specs survive as text-level. | (L2; no contradicting decision) |

**No conflict with D1–D20 (or L1–L7) remains in this facet.** The former hardest
fork — O1 (canonical form) — is closed by **D2**, so `SaveEngine`'s fate is no longer
contingent. The only remaining empirical unknown is the corpus reference-graph
acyclicity check (a P0 scan, §4.3), which is a verification step, not a design
decision.

---

## 6. First-slice implications & risk register

### 6.1 This facet's first-slice contribution

Facet 6 doesn't build a chunk type — it builds **the safety net that makes the first
slice (and every phase) shippable**:

1. **The render-equivalence harness (§3.6)** — the P0 exit criterion and the gate on
   every later phase. Without it, "we faithfully replicated the parser" is a hope;
   with it, it's a CI invariant. **This is the single most important migration
   artifact and should be built first, even before the first CST node.**
2. **`tools/migrate_scenes_v6_to_v7.py` (§3.5)** — at first-slice scope it need only
   handle tier-0 flatten (D7 include-inlining, if the fixture is multi-file) + the
   declarative header bump so the §D18 phased-fixture scenes can be flipped to v7 for
   the equivalence run.
3. **The transitional dual-path seam (§3.1)** — the header-gated v6-read-vs-v7-CST
   path that lets the first slice ship behind a flag without touching the 376-scene
   corpus. **Transitional only:** per D8 the v6 read is deleted once the corpus is
   green (P8), not kept as a permanent coexistence.
4. **A build-tax mitigation** — *recommendation:* a `tools/sync_build_projects.py`
   that, given a source file added to the canonical `Filelist`, propagates the entry
   to the other 4 projects (the inverse of the existing per-project file lists).
   Not in scope for this doc to build, but it's a cheap force-multiplier for an XL
   effort that adds many files, and it directly de-risks the §4.5 / R6 tax.

### 6.2 Risk register (ranked by likelihood × impact)

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| **R1** | **Lossless CST round-trip is incomplete** — a structured edit reformats untouched text, or a hand-edited file doesn't survive parse→serialize (silent loss of comments/whitespace/unsupported chunks). The inherited `TRANSACTION_MODEL.md` failure. | **High** | **High** | Reuse `RawTokenCapture`'s lossless trivia into the **green tree** (relative width, **D2**; `NodeId` in the red layer/side-map, **D15**; positions via the **red cursor over a persistent rope**, **O(log N)**, **D16**); mechanical byte-identical round-trip test over the whole corpus as gate **G1**; the red-green pivot makes save lossless by construction and edits **O(log N)** path-copies over the rope. **Note (D8):** v7 scenes contain *no* FOR/`$()` constructs (the migrator emits first-class v7 nodes), so there are no opaque legacy spans to preserve at runtime — round-trip only has to be lossless over the v7 grammar. **The #1 correctness risk — see §4.2.** |
| **R2** | **Incremental-derivation latency** — a localized edit rebuilds too much; the interactive bar stutters. | **Med-High** | **High** | **D1** gesture model (uncommitted head + ephemeral preview snapshot, debounced; no 60 Hz) + **reverse-dependency-closure COW re-derive (D11)**, **built→sealed→published at a pass boundary (D12)**; reuse `DirtyScope` blast-radius taxonomy + realize/TLAS/photon seam; the derivation cache is version-scoped/persistent (D20); **gate G2 measures this on the P1 phase-1 fixture (cheapest edit) before the XL derivation work** — fail fast. (First impl may full-rebuild per D11, then add closure-tracking.) §4.1. |
| **R3** | **Big-bang scope creep** — the XL derivation + UI rewrite tempts a flag-day cut-over; the tree goes red for weeks; the 376-scene corpus / 146 tests can't all be kept green simultaneously. | **High** | **High** | The strangler-fig phasing (§3.3): the transitional v6-read and v7 paths coexist *during migration only* (D8); deletions only after the replacement is RED-proven and the equivalence harness is green; every phase independently shippable; the terminal v6-path deletion (P8) is the singular D8 contract. **Discipline, not architecture, is the mitigation** — and the user's own multi-agent worker discipline + adversarial-review-as-gate (`feedback_multiagent_worker_discipline.md`, `adversarial-code-review` skill) apply. |
| **R4** | **Agent-edit safety** — an agent (or GUI-as-agent) commits a CST edit that is syntactically valid but semantically destructive (deletes a referenced node, writes an invalid material), and undo/version-history is the only firewall. | **Med** | **High** | The validate→derive→render pipeline (Facet 5) gates every agent patch *before* commit; reference-safe deletion (block/cascade, from `ENTITY_CREATION.md`) at the CST level; CST version history makes every agent action atomically revertable; the `AI_SECURITY_MODEL.md` threat model carries over. CST diffs make agent actions diff-reviewable (a Model B *advantage*). |
| **R5** | **Order-independence breaks a real scene** — a corpus scene relies on an order-sensitive construct or a reference cycle that topological emission can't reproduce. | **Low-Med** | **Med** | Corpus reference-graph scan during P0 (expected acyclic); the equivalence harness catches any divergence per-scene; cycles become a structured validation error, not a silent failure. §4.3. |
| **R6** | **Cross-platform UI cost + the 5× build tax** — the dynamic UI must land on Mac/Windows/Android shells; new files must hit all 5 build projects; "green on macOS, broken on Windows" from a forgotten pbxproj/vcxproj entry. | **Med-High** | **Med** | Maximize shared C++ (the dynamic-UI *generation* is shared; shells render schema-widgets — `CROSS_PLATFORM_ARCHITECTURE.md`); `tools/sync_build_projects.py` (§6.1); CI builds on all platforms per phase; bridge-enum drift addressed by Facet 4 (`feedback_bridge_enum_translation_audit.md`). Android is Tier-tolerant (`project_gui_roadmap_initiative.md`). |
| **R7** | **Test-coverage gaps during transition** — a Model-A test is deleted (its subject is gone) before its Model-B replacement exists, opening a window where a regression slips. | **Med** | **Med** | Rule: **never delete a Model-A test until its Model-B replacement is RED-proven** (§2.9); the 116 render tests + equivalence harness are model-agnostic oracles throughout; `SourceHygieneTest`/`red_prove.sh` guardrails stay; `feedback_fix_all_bugs_regardless_of_provenance.md` (fix every real bug, no provenance excuse). |
| **R8** | **The two engine prerequisites (side-effect-free parse + scene-text retention) are deeper than estimated** — P0 (rated L) is really XL because the parser's build-as-it-parses coupling (`VALIDATION_ARCHITECTURE.md`'s impossibility proof) is pervasive. | **Med** | **Med** | These are shared by both models (not Model-B-specific cost); the proto-CST already retains byte offsets, so the seam is partly built; scope P0 to the *declarative subset* first (defer macro handling to the migrator). If P0 balloons, it's the right place to discover it (before any deletion). |
| **R9** | **(RESOLVED) O1/O2/O3 decided late.** Was: synthesis defers canonical-form / interactive-bar / coexistence past P2, forcing rework. | **Closed** | — | **Resolved by review round 1:** O1→**D2** (red-green tree; `SaveEngine` splice DELETE), O2→**D1**+G2 (gesture model; < 50 ms derive), O3→**D8** (transitional read then singular delete). No live fork remains; kept for traceability. |
| **R10** | **Material/Media descriptor rewrite is underestimated** — the hardcoded 25-branch introspection cascade is load-bearing in subtle ways (painter reverse-lookup by pointer identity); making it descriptor-driven leaks bugs. | **Low-Med** | **Med** | It's an EVOLVE worth doing regardless of Model B; sequence it as its own phase (P6) with the `*IntrospectionTest` suite as guard; not on the first-slice critical path (the §D18 fixture's phases 1–3 use `sphere_geometry`/`uniformcolor_painter`/`lambertian_material`/`standard_object`, all already descriptor-driven). |
| **R11** | **Asset-change invalidation missed (D5/D17)** — a referenced texture/mesh/spectral/glTF changes on disk without the CST or filename changing; a clean derive then disagrees with a cache hit (stale render, or a migrated scene that silently diverges from its v6 original). **Bytes can change with `(size,mtime)` unchanged**, so mtime alone is not deterministic. | **Med** | **Med-High** | **D5+D17**: derivation input is `(CST, AssetManifest)`; asset identity = **`(size,mtime)` fast prefilter → content hash** (authoritative; memo keys use the content hash). The fingerprint is **traced** (D4) into the memo keys of the nodes that consumed it, so a change invalidates exactly those nodes. A file watcher (or re-stat on focus/render) drives invalidation. Output paths excluded. Gate **G5** exercises this on the first slice (§D18 phase 6's asset-backed node). |
| **R12** | **External-file conflict / silent overwrite (D6/D17)** — git or another editor changes the `.RISEscene` on disk; in-process version-id dirtiness can't see it; autosave clobbers the external change. **D6's stat-then-write CAS itself has a TOCTOU race** (the file can change between the check and the write). | **Med** | **High** | **D17 (supersedes D6's CAS mechanism): atomic save** — write to a **temp file in the target dir → `fsync` → revalidate the target's content hash == the loaded fingerprint → atomic `rename()` over the target.** On a fingerprint mismatch, surface reload / diff-merge / force (the D6 conflict UX); never a silent overwrite. **Documented residual:** a non-cooperating concurrent writer can still race the final rename (last-writer-wins at the FS layer) → **opt-in advisory file locking** for shared storage. The background watcher (D5's mechanism) can flag "changed on disk" proactively. This is the `FileIdentity` guard's intent retained after the byte-splice mechanism is deleted (§2.4). Gate **G5**. |

### 6.3 Effort & sequencing summary

| Phase | T-shirt | Gates / depends on |
|---|---|---|
| **(pre-P0)** Render-equivalence harness | **M** | none — build first |
| P0 — parse seam + CST scaffold (declarative subset) | **L** (R8: maybe XL) | harness green on macro-free corpus |
| P1 — first slice (the §D18 phased fixture) | **M** | P0; **measures G2 latency (R2) + G1 round-trip (R1); phase 6 exercises G5 (R11)** |
| P2 — derivation engine (incremental, traced, topological) | **XL** | P1 (O1/D2 already resolved, R9 closed) |
| P3 — CST versioning replaces edit model | **L** | P2 |
| P4 — dynamic UI from descriptors | **L–XL** | P2, P3 |
| P5 — save = serialize CST (+ D17 atomic save) | **M** | P4 (transitional v6 read covers old scenes until P8) |
| P6 — Material/Media descriptor rewrite | **L** | independent; pairs with P4 |
| P7 — agent surface on CST | **L–XL** | P2–P5 |
| P8 — corpus on v7, **DELETE** v6 path (D8) | **M** | all; equivalence harness green corpus-wide |

**Total: one XL (derivation), two L–XL (UI, agent), the rest L/M.** The shape is a
long-thin spine (harness → seam → first slice → derivation) before the wide work
(UI, save, agent) fans out. The recommended **first thing to build is the
render-equivalence harness**, and the **first slice is the corrected §D18 phased
fixture** (`sphere_geometry` → `+uniformcolor_painter`+`lambertian_material` →
`+standard_object` chain → `+expr` → `+instance_array` → `+image_painter`/mesh), gated
by **G1–G5** — its early phases exercise both tar-pits (R1 round-trip, R2 latency) on
the smallest possible surface and its phase 6 exercises asset invalidation (R11/G5,
D17), before either the XL derivation investment or any Model-A deletion. **D8 makes
the terminal P8 deletion of the v6 path a single contract, not an optional deprecation
window.**

---

### Appendix — fate index (alphabetical, for quick lookup)

`acceleration{}`/`global_medium{}` chunks NEW(D19; migrated from `> set`) ·
`ApplyOffsetDeltas` DELETE(D2/D16; red cursor over rope) · `AsciiCommandParser` EVOLVE
(CLI) / DELETE from scene path(D19; all embedded `>` migrator-only) · `AsciiScriptParser`
EVOLVE/narrow · `ChunkDescriptor*` REUSE · `ChunkDescriptorRegistry` REUSE ·
`CameraIntrospection` EVOLVE · `DAGObjectManager` likely DELETE · derivation cache
version-scoped/persistent NEW(D20) · `DerivedScene` closure-COW + sealed-owns-
geometry/TLAS/light-samplers/photon-maps NEW(D11/D12) · `DirtyTracker` DELETE ·
`EditHistory` DELETE(head+derived version, D13) · `Film/Light/Object/RasterizerIntrospection`
EVOLVE · FOR/DEFINE/`$()`/`hal` DELETE · `GenericManager` REUSE · `IJob::Add*` REUSE ·
`Job::InitializeContainers` REUSE · light samplers EVOLVE(ownership→snapshot, D12) ·
embedded `>` commands (load/run/set/remove) DELETE(D7/D19; migrator-only) ·
`MaterialIntrospection` EVOLVE(rewrite) · `MathExpressionEvaluator` DELETE ·
`MediaIntrospection` EVOLVE(rewrite) · `NodeId` red-layer/side-map (lineage), name-path
addressing (D9/D15) · `ObjectManager` TLAS/realize UNCHANGED(phase-B-then-seal, D12) ·
`OverrideSpanIndex` DELETE · `ParseAndLoadScene` drive-loop DELETE/lexer-EVOLVE ·
`RawTokenCapture` trivia seeds green tree (index not retained) · `ReferenceUse` traced
records NEW(D14; rename source) · realize/freeze/photon seam UNCHANGED(seal-before-
publish, D12) · `Reference.h` UNCHANGED · `RISE_API_Create*` REUSE · rope child sequences
NEW(D16; O(log N)) · `SaveEngine` DELETE(byte-splice; D6/D17 fingerprint + atomic save
retained) · `SceneEdit` DELETE · `SceneEditController` EVOLVE(split) · `SceneEditor`
EVOLVE(gut) · `SourceSpanIndex` DELETE(D2/D16; red cursor over rope) · `src/Library/Agent/`
NEW · `TransformSnapshot` DELETE · v6 parser/preprocessor + version-gate v6 arm
DELETE(D8; migrator-only) · 5 build lists UNCHANGED(pay 5× tax) · ~30 edit tests
EVOLVE/repurpose · 116 render tests UNCHANGED.
