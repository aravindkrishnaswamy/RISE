# Facet 6 — Supersession Inventory, Migration Path & Risk Register

> **Status:** design-in-progress. One of six parallel facet docs under the
> [Agentic Redesign Charter](00-CHARTER.md). This facet is **cross-cutting**: it
> inventories the *entire* current architecture, assigns each component a fate
> under Model B (delete / evolve / reuse / unchanged), lays out a phased,
> non-big-bang migration, specs the scene-corpus migration tooling, and carries
> the risk register (including the two tar-pits). It reconciles Facets 1–5.
>
> **Design only. No source, build, or scene changes are made by this doc.**

**Reconciliation note (sibling facets not yet written).** At the time of writing,
none of the sibling facet docs (`10`–`50`) exist on disk. This doc therefore
designs from the [Charter](00-CHARTER.md) + the current code, and **flags every
assumption it makes about a neighbor** in §5 so synthesis can reconcile. Where
this doc claims "Facet N deletes X," read it as *"this facet's plan assumes Facet
N owns the deletion of X; confirm when 10–50 land."*

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
  declarative chunk lists.
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
| `AsciiCommandParser` imperative verbs (`set/remove/rasterize/load/run/…`) | `AsciiCommandParser.cpp` | **EVOLVE** | CLI command surface stays; the `>`-embedded-in-scene override/tombstone commands become CST nodes (see §3.4). | 1/5 |
| Scene version gate (`CURRENT_SCENE_VERSION 6`) | `:156`, `:10561` | **EVOLVE** | Bump to declare v7; v6 routes to a compatibility path (O3, §3.2). | 1 |

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
| `ObjectManager` TLAS + realize | `ObjectManager.{h,cpp}` | **UNCHANGED** | Render mechanics, assembly-agnostic. | 2 |
| Realize-from-roots + `RenderParallelScope` freeze guard | `RayCaster.cpp`, `*::Realize`, `RenderParallelScope.{h,cpp}` | **UNCHANGED** | The assembled→render-ready seam; CST-derived scene produces the same graph. | 2 |
| Photon-pass gate (`ConsumesScenePhotonMaps`) | `IRasterizer.h:142` | **UNCHANGED** | Rasterizer-capability gate, assembly-agnostic. | 2 |
| `DAGObjectManager` (alternate, no-TLAS) | `DAGObjectManager.{h,cpp}` | **EVALUATE → likely DELETE** | Legacy alternate; verify it's wired anywhere before relying on it. | 2 |

### 2.3 The edit subsystem (the supersession centroid)

| Component | Files | Lines | Fate | Why | Facet |
|---|---|---|---|---|---|
| `SceneEdit.h` (27-op tagged union + prev-state) | `SceneEdit.h` | 421 | **DELETE** | Command-with-captured-prev-state subsumed by CST version diffs; the `Op` taxonomy survives *conceptually* as the structured-edit vocabulary (Facet 3), but the value-record form is dead. | 3 |
| `EditHistory.{h,cpp}` | | 416 | **DELETE** | Linear bounded undo stack → CST version history (atomic, correct by construction). The cap/trim/rollback-snapshot complexity is all Model-A compensation. | 3 |
| `SceneEditor.{h,cpp}` — Apply/Undo/Redo + the 5 walks | | 2217 | **EVOLVE (gut)** | Delete the walks + history ownership + transaction rollback; **keep the invariant chain** (`RunObjectInvariantChain`, light-gen bumps, spatial invalidation) + manager-resolution as part of the CST→engine apply layer (any "apply current CST state to engine" path still needs these). | 2/3 |
| `SceneEditController.{h,cpp}` | | 5239 | **EVOLVE (split in two)** | **Keep** the render-thread / preview-scaling / gizmos / selection / pointer-dispatch / panel-cache *view* plumbing (genuine GUI, orthogonal to the doc model). **Rewrite** the mutation/undo/transaction/save surface (`SetProperty`→`SceneEdit`, `Undo`/`Redo`, `Begin/Rollback/EndTransaction`, `RequestSave`) to mutate the CST and read panel state *from* the CST. | 3/4 |
| `DirtyTracker.{h,cpp}` | | 239 | **DELETE** | "Dirty" → "CST ≠ on-disk version" (a pure comparison). | 3 |
| `SourceSpanIndex.{h,cpp}` | | 443 | **EVOLVE → CST** | Already retains per-param byte-spans + `AuthorMode` + FOR-revisit + `isSymbolic`; generalize span maps into the canonical full-trivia tree. | 1 |
| `OverrideSpanIndex.{h,cpp}` | | 314 | **DELETE** | The `override_object` side-car + Mode-A/Mode-B routing exists *only* because the source isn't a mutable CST; editing a FOR-generated entity becomes a normal CST edit. | 1/3 |
| `TransformSnapshot.{h,cpp}` | | 104 | **DELETE** | Base/loaded diff baselines subsumed by the CST. | 3 |
| `SaveEngine.{h,cpp}` (byte-splice, EditOp ordering, Mode-A/B, refuse cases) | | 2009 | **DELETE** | 85 KB of splice logic → CST lossless serialization. **Every Refuse case (FOR-generated, symbolic, cross-file, non-decomposable transform) becomes a normal structured edit.** The single biggest "subsumed by CST" deletion. *Carry forward:* its FOR/`$(...)`/`AuthorMode` handling is the catalog of hard cases a CST serializer must also get right. | 1/3 |
| `CameraIntrospection` (descriptor-driven read/write/clone) | | 1161 | **EVOLVE** | Keystone reuse: rebind `engine-getter → CameraProperty` to `CST-node ↔ CameraProperty`; the 5 descriptor-driven files port directly. | 3/4 |
| `Film/Light/Object/RasterizerIntrospection` | | ~1100 | **EVOLVE** | Same descriptor-driven port (Rasterizer lacks live-value getters — fine, the CST node *is* the value source under Model B). | 3/4 |
| `MaterialIntrospection` (hardcoded 25-type cascade) | | 1060 | **EVOLVE (rewrite)** | The big rewrite: make material chunks descriptor-driven so readback is uniform. Worth doing regardless of Model B. | 1/4 |
| `MediaIntrospection` (hardcoded slot matching) | | 225 | **EVOLVE (rewrite)** | Same — descriptor-driven medium chunks. | 1/4 |
| `ChunkDescriptorRegistry.{h,cpp}` (lazy keyword→descriptor map) | | 77 | **REUSE** | The cleanest reusable component; exactly what a descriptor-driven CST UI + validator need. Keep verbatim, ABI-clean. | 1/4 |

### 2.4 Save / round-trip (subsumed)

See `SaveEngine` and the span indices above. **Net:** the entire round-trip-save
machinery (`SaveEngine` + `OverrideSpanIndex` + `TransformSnapshot` + `DirtyTracker`
+ the Mode-A/B/refuse model) is **DELETE — subsumed by CST canonicality**, because
"save" becomes "serialize the CST" (lossless because the CST retained the trivia).
`SourceSpanIndex` + `RawTokenCapture` **evolve into** the CST itself.

### 2.5 Panels / bridges / cross-platform UI

| Component | Files / anchor | Fate | Why | Facet |
|---|---|---|---|---|
| Descriptor-driven properties panel (auto-row per `ChunkDescriptor` entry) | per-platform GUI + introspection | **EVOLVE** | The "any descriptor param → editable row" mechanism is exactly the dynamic UI (Facet 4); re-point its source from engine-introspection to CST nodes. | 4 |
| Hand-built accordion / category panels (`mSelectionByCategory[9]`, section-expanded) | `SceneEditController` + platform shells | **EVOLVE → mostly DELETE** | Static category panels → adaptive widgets generated per CST node (Charter L7 explicitly supersedes the hand-built accordions). | 4 |
| The bridge enum-translation getters (`panelMode`, `selectionCategory`, switch-on-int → None on miss) | Mac `.mm` / Windows bridges | **EVOLVE** | A known drift hazard (`feedback_bridge_enum_translation_audit.md`); Facet 4 should generate these or eliminate the enum boundary. | 4 |
| The C-ABI boundary (`RISE_API_SceneEditController_*`, ~70 entry points) | `RISE_API.h`, the 3 bridges | **EVOLVE (boundary moves one level out)** | Survives structurally; the library becomes CST-canonical instead of in-memory-canonical, so the boundary gains `GetSceneText`/`ValidateSceneText`/`ApplySceneText`/CST-diff entry points. `CROSS_PLATFORM_ARCHITECTURE.md` already reserves these. | 4/5 |
| Per-platform shells (Mac SwiftUI+ObjC++, Windows Qt6, Android JNI) | `build/XCode/…/RISE-GUI`, `build/VS2022/RISE-GUI`, Android | **EVOLVE (thin)** | Stay thin sinks; the dynamic-UI generation is shared C++. Cross-platform cost bounded (each shell renders schema-generated widgets). | 4 |

### 2.6 FOR / DEFINE / hal / `$(...)` macros

| Construct | Fate | Migration |
|---|---|---|
| `FOR`/`ENDFOR` | **DELETE** | Homogeneous instancing → declarative instancer/function node; typing-shortcut loops → desugar to explicit entities at migration time (§3.5). |
| `DEFINE`/`!`/`@`/`%` | **DELETE** | Constant-folding (inline the value) at migration time; named constants that *should* persist → a declarative `define`/parameter node if Facet 1 provides one. |
| `$(...)` arithmetic | **DELETE** | Evaluate to a literal at migration time; expressions that must stay dynamic → declarative function-expression node (Facet 1). |
| `hal()` | **DELETE** | Stateful sampler-in-the-parser is removed outright; any scene relying on it is hand-migrated to authored values. |

### 2.7 The scene corpus

| Bucket | Count | Fate |
|---|---|---|
| Fully-declarative scenes (no macros) | ~347 / 376 | **EVOLVE (automated)** — version-bump only (or no-op) via `migrate_scenes_v6_to_v7.py`. |
| FOR-loop scenes | 13 | **EVOLVE (automated desugar + spot-check)** — loop-unroll into explicit chunks (§3.5). |
| DEFINE / `$(...)` only | ~16 | **EVOLVE (automated constant-fold)** — inline values. |
| `hal()` scenes | 2 | **HAND-MIGRATE** — bake sampled values. |

### 2.8 The build projects

| Project | Fate | Why |
|---|---|---|
| All five build file-lists | **UNCHANGED (mechanism); pay the 5× tax per new file** | New CST/derivation sources must be added to all 5; no structural change to the build system itself. *Mitigation in §6.* |
| `run_all_tests.sh` phased build | **UNCHANGED** | Already parallel; the new tests are globbed (`tests/*.cpp`). |

### 2.9 The test suite

| Bucket | Fate | Why |
|---|---|---|
| ~30 edit/save/parser tests | **EVOLVE / repurpose** | The Model-A-specific ones (`SaveEngineTest` splice paths, `SceneEditTransactionTest` rollback, `SceneSnapshotTest`/`SceneRestoreTest` clone) become the **golden render-equivalence + round-trip guard** during transition, then are retired/rewritten against CST semantics as their subjects are deleted. *(Do not delete a Model-A test until its Model-B replacement is RED-proven — §6 coverage-gap risk.)* |
| `RawTokenCaptureTest`, `SourceSpanIndexBuilderTest` | **EVOLVE → CST tests** | Their subjects evolve into the CST; the tests grow into CST round-trip/loss tests. |
| `SourceHygieneTest`, `tools/red_prove.sh` | **UNCHANGED (keep as guardrail)** | The P-FFMATH guardrail and RED-prove discipline apply to all new code (`feedback_fix_all_bugs_regardless_of_provenance.md`). |
| The ~116 rendering/integrator tests | **UNCHANGED** | Engine behavior is untouched (INV-2: scene = f(CST) must produce the *same* scene). These ARE the render-equivalence oracle. |

### 2.10 The MCP / agent surface

| Component | Fate | Why |
|---|---|---|
| `src/Library/Agent/` (MCP server, LLM runtime, providers, credential store) | **NEW (no supersession)** | Does not exist; built on the CST from the start (Facet 5). Model B is the cleanest substrate (read CST/text, propose patch, validate→derive→render). |
| `docs/gui/MCP_TOOL_SURFACE.md` / `LLM_AGENT_RUNTIME.md` / `MATERIAL_EDITOR.md` / `SPECTRAL_DIFFERENTIATORS.md` / `CROSS_PLATFORM_ARCHITECTURE.md` | **SURVIVE (text-level by design)** | Already document/text-centric; Model B *enforces* their stated intent. The `apply_scene_text` wholesale-rewrite fallback becomes the canonical path. | 5 |
| `docs/gui/TRANSACTION_MODEL.md` + `EDITOR_STATE_AND_TRANSACTION_HARDENING.md` | **SUPERSEDED** | The authoritative owners of the Model-A authority/transaction/undo machinery the charter replaces. | 3 |
| `docs/gui/VALIDATION_ARCHITECTURE.md` (Tier-1 parse-only IR) / `RENDER_COORDINATOR.md` / `ENTITY_CREATION.md` | **MOSTLY SURVIVE (re-layer onto CST)** | Tier-1 IR ≈ the CST seam; render scheduling neutral; entity-creation concepts (outliner = CST view, reference graph, safe deletion) survive — the `SceneEdit`-op *mechanism* dies. | 2/3/4 |

### 2.11 Subsystem-level summary

```
                       DELETE              EVOLVE                 REUSE / UNCHANGED
Parser/lang     FOR/DEFINE/$()/hal,   ParseAndLoadScene loop,   ChunkDescriptor model,
                MathExprEval          RawTokenCapture→CST,       153 Describe(), TokenizeString
                                      SourceSpanIndex→CST,
                                      153 Finalize()
Assembly        (DAGObjectManager?)   eager-order resolution,    RISE_API factories, IJob Add*,
                                      RequestItemUse?            GenericManager, Reference,
                                                                 realize/TLAS/photon seam
Edit subsystem  SceneEdit, EditHistory, SceneEditor (gut to     ChunkDescriptorRegistry
                DirtyTracker,         invariant-chain),
                OverrideSpanIndex,    SceneEditController (split),
                TransformSnapshot,    7 Introspection (rebind),
                SaveEngine            Material/Media (rewrite)
Panels/UI       hand-built accordions Properties panel→dynamic,  —
                                      bridges, C-ABI boundary,
                                      per-platform shells
Agent/MCP       —                     (specs survive)            — (Agent is NEW)
Build/tests     —                     ~30 edit tests (repurpose) 5 build lists, run_all_tests,
                                                                 116 render tests, SourceHygiene
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

### 3.1 Coexistence model (the heart of "non-big-bang")

Can v6 (Model A) and v7 (Model B) coexist at runtime during transition? **Yes — via
a single adapter seam at the lex boundary, because both models share the descriptor
schema and the `IJob` apply layer.**

```
   .RISEscene  ──► lexer (RawTokenCapture, shared) ──┐
   (v6 or v7)                                         │
                                          ┌───────────┴───────────┐
                                  v6 path │                       │ v7 path
                          (Model-A,       ▼                       ▼  (Model-B,
                           legacy)  ParseAndLoadScene        Build CST   canonical)
                                    (one-way, today)        (retained tree)
                                          │                       │
                                          ▼                       ▼
                                  IJob::Add* (apply)  ◄── derive ──┘
                                          │            (same Finalize→Add*)
                                          ▼
                                   Scene → Render  (identical downstream)
```

- **The convergence point is `IJob::Add*`.** Both paths emit the *same* apply calls,
  so the engine, managers, realize seam, and all 116 render tests are a **shared
  oracle** — a v6 scene and its v7 migration must produce byte-identical renders.
  This is the equivalence harness (§3.6).
- **Format coexistence (O3).** The version header selects the path:
  `RISE ASCII SCENE 6` → legacy one-way parse (unchanged, kept alive through the
  transition); `RISE ASCII SCENE 7` → CST path. **v6 is never auto-upgraded at load**
  — it parses through the legacy path verbatim, so old scenes keep working with zero
  risk. Migration to v7 is an explicit `tools/` step (§3.5). The two parsers
  coexist in-binary until the corpus is fully migrated *and* a deprecation window
  passes; only then is the v6 path deleted.
- **The CST can be built for v6 scenes too** (read-only): the lexer is shared, so a
  v6 scene can be *displayed/edited* through the CST even before it's converted —
  but FOR/DEFINE/`$()` nodes in a v6 CST are **read-only / opaque** (the CST
  preserves them losslessly but the structured editor refuses to mutate inside them,
  exactly as `SaveEngine` flags `isSymbolic` spans today). This gives "open any
  scene in the new UI" without forcing migration. *(Assumption flagged for Facet 1:
  the v7 CST grammar must subsume v6's declarative chunks 1:1, which it does because
  the descriptors are shared.)*

### 3.2 The first slice (the minimal end-to-end vertical)

Per Charter §7.6, pick **one chunk type and drive it end-to-end**:
text ⟷ CST ⟷ derived-scene ⟷ one schema-generated widget ⟷ live incremental re-derive.

**Recommended first chunk: `uniformcolor_painter`.** Rationale:
- Leaf node — no dependencies to resolve (sidesteps the order-dependence problem
  for the *first* slice, isolating the CST/derivation mechanics).
- Tiny descriptor (`color` Vec3 + `colorspace` enum) — one schema-generated widget
  exercises Vec3 + Enum `ValueKind`s.
- Already descriptor-driven on both parse and introspection sides (no Material-style
  rewrite needed for slice 1).
- Appears in ~every scene — high coverage for the equivalence harness.
- A color edit is the canonical "debounced-commit direct manipulation" gesture (O2)
  — perfect to pressure-test the interactive-bar latency tar-pit.

The slice proves, in one vertical: lossless round-trip (INV-4), scene = f(CST)
(INV-2), incremental re-derive of one node (INV-3), one edit pathway (INV-6),
name-path identity (INV-5), and the debounced-commit cadence (O2). It touches
Facets 1 (CST node), 2 (derive one painter), 3 (CST edit + undo), 4 (one widget),
and 5 (agent can patch it) — a true cross-facet vertical.

### 3.3 The phase sequence

Each phase is independently shippable and leaves the tree green.

| Phase | Name | T-shirt | What ships | What it deletes (only when proven) |
|---|---|---|---|---|
| **P0** | Side-effect-free parse seam + CST scaffold | **L** | Lexer (`RawTokenCapture`-derived) produces a retained CST; a CST→`IJob` derivation walk for the **declarative subset**; v7 header recognized; both paths converge at `Add*`. Equivalence harness (§3.6) green on the macro-free corpus (~347 scenes). | nothing yet |
| **P1** | First slice: `uniformcolor_painter` end-to-end | **M** | Text⟷CST⟷derive⟷one schema-widget⟷incremental re-derive⟷CST-version undo⟷agent-patch, behind a feature flag in the GUI. The interactive-bar (O2) and lossless round-trip (O1/INV-4) tar-pits are pressure-tested here. | nothing yet |
| **P2** | Derivation engine: dependency graph + incremental + order-independence | **XL** | CST→Scene as a memoized, deterministic, incremental function (Facet 2); topological emission solves the eager-order constraint; localized edit re-derives only the affected subgraph. Hooks into the existing realize/TLAS/photon seam. | nothing yet |
| **P3** | CST versioning replaces the edit model | **L** | Undo/redo = CST version history; `SceneEditor`/`SceneEditController` mutation surface re-pointed onto the CST (Facet 3). | **`SceneEdit`, `EditHistory`, `DirtyTracker`, `TransformSnapshot`** (their subjects gone). |
| **P4** | Dynamic UI from descriptors | **L–XL** | Properties/outliner generated per CST node from descriptors (Facet 4); the 5 descriptor-driven introspection files rebind to CST nodes; hand-built accordions retired. | hand-built accordion panels; engine-introspection-as-UI-source. |
| **P5** | Save = serialize CST | **M** | "Save" writes the CST losslessly (Facet 1). All `SaveEngine` Refuse cases become normal edits. | **`SaveEngine`, `OverrideSpanIndex`** + the Mode-A/B model. |
| **P6** | Material/Media descriptor rewrite | **L** | `MaterialIntrospection`/`MediaIntrospection` become descriptor-driven (worth doing regardless). | the 25-branch `dynamic_cast` cascade. |
| **P7** | Agent surface on the CST | **L–XL** | `src/Library/Agent/` MCP server: read CST/text, propose patch, validate→derive→render, structured errors (Facet 5). GUI-as-just-another-agent unification. | nothing (net-new). |
| **P8** | Corpus fully on v7; deprecate v6 path | **M** | Whole corpus migrated (§3.5) and render-equivalent; deprecation window passes. | **the legacy `ParseAndLoadScene` one-way path + all macro/FOR/`$()` code** + the v6 compatibility branch. |

**Why this order.** P0 establishes the seam everything needs. P1 de-risks the two
tar-pits on the smallest possible surface *before* committing to the XL derivation
work. P2 (derivation) must precede P3/P4 (edit/UI) because they consume it. P3
before P4 (UI needs CST versioning to drive undo). Save (P5) can come after the UI
because the legacy `SaveEngine` keeps working for v6/transitional scenes until then.
The agent (P7) and the corpus deprecation (P8) are last because they depend on the
full stack. **No phase requires a flag day; deletions happen only after the
replacement is RED-proven and the equivalence harness is green.**

### 3.4 Handling the `>` override/tombstone commands

A subtle coexistence item: scenes use embedded `> set`/`> remove`/`> load`/`> run`
commands, and `SaveEngine` emits managed `override_object` blocks + `> remove`
tombstones (6 families only). Under Model B these are **CST nodes**, not imperative
side-effects: an override is a structured edit to the referenced node; a tombstone
is a delete in the CST. *Assumption flagged for Facet 1:* the v7 CST must represent
`> load`/`> run` includes as first-class nodes (so cross-file edits, today a
`SaveEngine` Refuse case, become normal). *Assumption flagged for Facet 3:* the
deletion-persistence gap (camera/media/shader tombstones don't exist today) is on
the critical path for "all-family create/delete through one pathway."

### 3.5 Scene-corpus migration tooling

**`tools/migrate_scenes_v6_to_v7.py`** — built on the proven migrator pattern
(regex line-rewriter, header-guarded idempotency, in-place with line-ending
preservation, recursive over `scenes/`, documented preserve-list; cross-file aware
like `migrate_scenes_iscalarpainter.py`). It handles three tiers:

1. **Declarative scenes (~347):** rewrite only the version header `6`→`7` (and any
   trivially-renamed keyword). Idempotent; a no-op on already-v7 files. *This is
   ~92% of the corpus, fully automated, zero render risk* (the chunks are identical;
   only the header changes the parse path).
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
4. **`hal()` scenes (2):** **hand-migrate.** A stateful sampler-in-the-parser can't
   be constant-folded meaningfully (each call advances the sequence); bake the
   intended values by hand. Two scenes — trivial manual cost.

**Verification: render-equivalence is the acceptance test.** For every migrated
scene, render the v6 original (legacy path) and the v7 output (CST path) at fixed
seed/samples and assert pixel-identical (or within a tight epsilon for any FP
reassociation). This reuses the existing variance/HDR-compare tooling
(`bin/tools/HDRVarianceTest`, `variance-measurement` skill) and the 116 render
tests as oracles. **A migrated scene that doesn't render-match is a migrator bug, not
an accepted loss.** The harness runs in CI over the whole corpus (sequentially —
renders take all cores, `feedback_render_sequentially.md`).

**Migration is opt-in and reversible during transition:** v6 scenes keep working
(§3.1), so the corpus can be migrated incrementally (e.g. `Tests/` first as the
oracle, then `FeatureBased/`, then `Internal/`), and a bad migration is just
`git checkout` of that scene.

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

**Mitigation / working assumption (O2 = debounced-commit).** The charter's working
assumption (debounced-commit direct manipulation, aligned with the agentic
edit→preview cadence) makes this **tractable**: we do *not* need 60 Hz incremental
derivation. A drag updates an ephemeral *session-state* preview (the live viewport
already does this via `ResizeFilm`/preview-resolution), and the **CST commit happens
on gesture-end (debounced)**, triggering one incremental re-derive. This is exactly
how the live editor behaves today (cancel-restart-on-edit), so the latency budget is
"one debounced re-derive of one subgraph," not "re-derive at frame rate."
**First-slice P1 measures this directly** on `uniformcolor_painter` (the cheapest
possible edit) — if a single leaf re-derive + re-render-kick isn't sub-100ms, the
whole interactive premise needs rethinking *before* the XL derivation work.
**Delta if 60 Hz were required:** the session-state preview would need to bypass the
CST entirely for the duration of the gesture and reconcile on commit — a second
(ephemeral, non-canonical) representation that INV-1 forbids for *document* state
but permits for *session* state (L4). Designable, but it reintroduces a
sync-two-things hazard for the gesture window; avoid unless measurement forces it.

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

**Mitigation.** (a) **Build on the proven proto-CST.** `RawTokenCapture` +
`SourceSpanIndex` already retain byte ranges, `isSymbolic`, `AuthorMode`, and FOR-
revisit flags and already do lossless Mode-A splices — generalize them to retain
*all* trivia rather than rebuilding from scratch. (b) **O1 = lossless-CST pivot**
(the working assumption) makes "save = serialize the CST" lossless *by construction*
because the CST holds the trivia; the alternative (O1 text-canonical, buffer-is-
truth) would make the buffer authoritative and the CST a derived index that must be
kept consistent with edits — *simpler losslessness* (the bytes never move unless
edited) but a harder structured-edit story (every structured edit is a text splice,
and the "tree" is re-derived). **Delta if text-canonical (O1-alt):** the round-trip
risk largely evaporates (text *is* the truth), but Facets 3/4 inherit a
splice-against-text edit model much closer to today's `SaveEngine` — i.e. we'd keep
more of the splice machinery and the CST becomes a navigation/validation overlay.
This is a genuine fork; **flag for synthesis** — Facet 1 owns the call, and it
materially changes how much of `SaveEngine` is DELETE vs EVOLVE. (c) **The
acceptance test is mechanical:** parse→serialize every corpus file and assert
byte-identical (a "CST loss test," the evolution of `RawTokenCaptureTest`).

### 4.3 Order-independence vs eager name resolution (INV-2)

**The problem.** INV-2 requires scene = f(CST) with *no hidden order-dependence*,
but the apply layer resolves dependencies eagerly by name and hard-fails on missing
deps (file order = assembly order). A CST node can reference a node declared *later*
in the file — legal in a declarative document, illegal in today's apply order.

**Mitigation (Facet 2 owns this).** The derivation engine emits `Add*` calls in
**dependency-topological order** computed from the CST's reference edges (every
`Reference`-kind parameter is an edge). This is a standard topo-sort over a DAG;
cycles are a validation error (reportable at the CST level, structured, before any
`Add*`). This *removes* the order-dependence at the document level while reusing the
eager apply layer unchanged. **Open question:** are there *legitimate* cycles or
order-sensitive constructs in the corpus (e.g. a material referencing a painter that
references back)? A corpus scan during P0 should confirm the reference graph is
acyclic (expected — RISE has no recursive material references today).

### 4.4 Identity stability (INV-5, L5)

Name-path identity (`objects/sphere.material`) must survive edits so selection,
agent references, and UI bindings don't break. The managers already track per-name
identity serials, but **a rename is an identity break today** (remove + re-add =
new instance). Facet 1/3 must decide whether the CST gives nodes a *stable internal
id* independent of name (so a rename preserves identity for selection/undo) or
whether name-path *is* identity and a rename is modeled as such. The charter's
round-4 "name-reuse identity serial" was a Model-A patch over exactly this; Model B
should make it first-class. *Flag for Facets 1 and 3* — this facet assumes name-path
is the *currency* but a stable node-id underlies it for edit/selection survival.

### 4.5 The 5× build tax during transition

Every new CST/derivation/agent source file must be added to all 5 build projects;
the Xcode pbxproj is especially painful (~8 refs × 2 targets). During an XL effort
that adds many files, this is real friction and a real source of "forgot to add it
to project N → green on macOS, broken on Windows" errors. *Mitigation in §6.*

### 4.6 Open questions for synthesis

- **O1 (canonical form)** materially changes the `SaveEngine` fate (DELETE under
  lossless-CST vs EVOLVE under text-canonical). §4.2.
- **O2 (interactive bar)** sets the derivation-latency budget. §4.1.
- **O3 (format coexistence)** — this doc proposes header-gated dual-path with no
  auto-upgrade; confirm with Facet 1.
- Does Facet 1 provide a declarative `define`/constant node and a function-expression
  node? The corpus migrator's constant-fold-vs-preserve choice (§3.5) depends on it.
- Does Facet 2 reuse the `DirtyScope` taxonomy and the realize/TLAS/photon seam, or
  introduce its own blast-radius model? (This doc assumes reuse.)

---

## 5. Cross-facet dependencies & assumptions

This facet reconciles 1–5, so its assumptions about each are the synthesis
checklist. **None of 10–50 existed when this was written; all of the below are
assumptions to confirm.**

| Assumed of | Assumption | If wrong… |
|---|---|---|
| **Facet 1 (CST)** | The v7 CST grammar subsumes v6's declarative chunks 1:1 (shared descriptors), retains *all* trivia losslessly, represents `>` includes/overrides/tombstones as nodes, and provides declarative replacements for FOR (instancer/function) + optionally DEFINE (constant node). The proto-CST (`RawTokenCapture`+`SourceSpanIndex`) is the seed, not a rebuild-from-scratch. | The coexistence model (§3.1) and the migrator (§3.5) break; round-trip risk (§4.2) reopens. |
| **Facet 1** | O1 = lossless-CST pivot (working assumption). | If text-canonical instead, `SaveEngine` becomes EVOLVE not DELETE (§2.4, §4.2) and more splice machinery survives. **Biggest single fork.** |
| **Facet 2 (derivation)** | CST→Scene is incremental/memoized/deterministic, emits `Add*` in topological order (solving §4.3), and **reuses** the `IJob` apply layer, the `DirtyScope` taxonomy, and the realize/TLAS/photon seam unchanged. | The "REUSE" verdicts on the apply layer / managers / realize seam (§2.2) weaken; latency tar-pit (§4.1) worsens. |
| **Facet 3 (edit model)** | Undo/redo = CST version history; the `SceneEditor` mutation surface re-points onto the CST; the invariant chain + manager-resolution survive as the apply path; deletion-persistence grows to all families. | The DELETE verdicts on `SceneEdit`/`EditHistory`/`DirtyTracker`/`TransformSnapshot` (§2.3) don't all hold; P3 slips. |
| **Facet 4 (dynamic UI)** | UI is a pure function of CST + descriptors; the 5 descriptor-driven introspection files rebind to CST nodes; hand-built accordions are retired; the C-ABI boundary gains CST/text entry points. | The introspection EVOLVE verdicts (§2.3) and the panel verdicts (§2.5) shift; cross-platform cost (§6 risk) grows. |
| **Facet 5 (agent)** | `src/Library/Agent/` is net-new on the CST (read/propose-patch/validate→derive→render); the GUI is "just another agent" through the one edit pathway; the `docs/gui/` agent specs survive as text-level. | If the agent needs Model-A hooks, the "no supersession / clean win" claim (§2.10) weakens. |

**No conflict with a Locked decision is introduced by this facet.** It operates
entirely within L1–L7 and designs around O1–O3 with the working assumptions, flagging
the deltas. The one place it pushes hardest is **O1**: the choice between lossless-CST
and text-canonical is the largest downstream-cost lever in the whole migration, and
this doc recommends synthesis resolve it *before* P2 (derivation) starts, because it
sets how much of `SaveEngine` is deletable.

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
   handle the declarative tier (header bump) so `uniformcolor_painter`-bearing
   scenes can be flipped to v7 for the equivalence run.
3. **The coexistence seam (§3.1)** — the header-gated dual-path that lets v6 and v7
   coexist so the first slice ships behind a flag without touching the 376-scene
   corpus.
4. **A build-tax mitigation** — *recommendation:* a `tools/sync_build_projects.py`
   that, given a source file added to the canonical `Filelist`, propagates the entry
   to the other 4 projects (the inverse of the existing per-project file lists).
   Not in scope for this doc to build, but it's a cheap force-multiplier for an XL
   effort that adds many files, and it directly de-risks the §4.5 / R6 tax.

### 6.2 Risk register (ranked by likelihood × impact)

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| **R1** | **Lossless CST round-trip is incomplete** — a structured edit reformats untouched text, or a hand-edited file doesn't survive parse→serialize (silent loss of comments/whitespace/FOR/`$()`/unsupported chunks). The inherited `TRANSACTION_MODEL.md` failure. | **High** | **High** | Build on the proven proto-CST (`RawTokenCapture`+`SourceSpanIndex` already do lossless Mode-A splices); mechanical byte-identical round-trip test over the whole corpus as a CI gate; O1=lossless-CST pivot makes save lossless by construction; preserve `isSymbolic`/FOR spans opaquely (proto-CST already does). **The #1 correctness risk — see §4.2.** |
| **R2** | **Incremental-derivation latency** — a localized edit rebuilds too much; the interactive bar stutters. | **Med-High** | **High** | O2=debounced-commit (no 60 Hz needed); reuse `DirtyScope` blast-radius taxonomy + realize/TLAS/photon seam; **measure on the P1 first slice (cheapest edit) before the XL derivation work** — fail fast. §4.1. |
| **R3** | **Big-bang scope creep** — the XL derivation + UI rewrite tempts a flag-day cut-over; the tree goes red for weeks; the 376-scene corpus / 146 tests can't all be kept green simultaneously. | **High** | **High** | The strangler-fig phasing (§3.3): both paths coexist; deletions only after the replacement is RED-proven and the equivalence harness is green; every phase independently shippable. **Discipline, not architecture, is the mitigation** — and the user's own multi-agent worker discipline + adversarial-review-as-gate (`feedback_multiagent_worker_discipline.md`, `adversarial-code-review` skill) apply. |
| **R4** | **Agent-edit safety** — an agent (or GUI-as-agent) commits a CST edit that is syntactically valid but semantically destructive (deletes a referenced node, writes an invalid material), and undo/version-history is the only firewall. | **Med** | **High** | The validate→derive→render pipeline (Facet 5) gates every agent patch *before* commit; reference-safe deletion (block/cascade, from `ENTITY_CREATION.md`) at the CST level; CST version history makes every agent action atomically revertable; the `AI_SECURITY_MODEL.md` threat model carries over. CST diffs make agent actions diff-reviewable (a Model B *advantage*). |
| **R5** | **Order-independence breaks a real scene** — a corpus scene relies on an order-sensitive construct or a reference cycle that topological emission can't reproduce. | **Low-Med** | **Med** | Corpus reference-graph scan during P0 (expected acyclic); the equivalence harness catches any divergence per-scene; cycles become a structured validation error, not a silent failure. §4.3. |
| **R6** | **Cross-platform UI cost + the 5× build tax** — the dynamic UI must land on Mac/Windows/Android shells; new files must hit all 5 build projects; "green on macOS, broken on Windows" from a forgotten pbxproj/vcxproj entry. | **Med-High** | **Med** | Maximize shared C++ (the dynamic-UI *generation* is shared; shells render schema-widgets — `CROSS_PLATFORM_ARCHITECTURE.md`); `tools/sync_build_projects.py` (§6.1); CI builds on all platforms per phase; bridge-enum drift addressed by Facet 4 (`feedback_bridge_enum_translation_audit.md`). Android is Tier-tolerant (`project_gui_roadmap_initiative.md`). |
| **R7** | **Test-coverage gaps during transition** — a Model-A test is deleted (its subject is gone) before its Model-B replacement exists, opening a window where a regression slips. | **Med** | **Med** | Rule: **never delete a Model-A test until its Model-B replacement is RED-proven** (§2.9); the 116 render tests + equivalence harness are model-agnostic oracles throughout; `SourceHygieneTest`/`red_prove.sh` guardrails stay; `feedback_fix_all_bugs_regardless_of_provenance.md` (fix every real bug, no provenance excuse). |
| **R8** | **The two engine prerequisites (side-effect-free parse + scene-text retention) are deeper than estimated** — P0 (rated L) is really XL because the parser's build-as-it-parses coupling (`VALIDATION_ARCHITECTURE.md`'s impossibility proof) is pervasive. | **Med** | **Med** | These are shared by both models (not Model-B-specific cost); the proto-CST already retains byte offsets, so the seam is partly built; scope P0 to the *declarative subset* first (defer macro handling to the migrator). If P0 balloons, it's the right place to discover it (before any deletion). |
| **R9** | **O1/O2/O3 decided late** — synthesis defers the canonical-form (O1), interactive-bar (O2), and coexistence (O3) calls past P2, forcing rework of the derivation/save design. | **Med** | **Med-High** | This doc recommends resolving **O1 before P2** (it sets `SaveEngine`'s fate), validating **O2 on P1** (cheapest measurable surface), and locking **O3 at P0** (the header-gated dual-path). Surface these as explicit synthesis decisions, not drift. |
| **R10** | **Material/Media descriptor rewrite is underestimated** — the hardcoded 25-branch introspection cascade is load-bearing in subtle ways (painter reverse-lookup by pointer identity); making it descriptor-driven leaks bugs. | **Low-Med** | **Med** | It's an EVOLVE worth doing regardless of Model B; sequence it as its own phase (P6) with the `*IntrospectionTest` suite as guard; not on the first-slice critical path (slice 1 uses `uniformcolor_painter`, already descriptor-driven). |

### 6.3 Effort & sequencing summary

| Phase | T-shirt | Gates / depends on |
|---|---|---|
| **(pre-P0)** Render-equivalence harness | **M** | none — build first |
| P0 — parse seam + CST scaffold (declarative subset) | **L** (R8: maybe XL) | harness green on macro-free corpus |
| P1 — first slice (`uniformcolor_painter`) | **M** | P0; **measures O2 (R2) + O1 round-trip (R1)** |
| P2 — derivation engine (incremental, topological) | **XL** | P1; **resolve O1 first (R9)** |
| P3 — CST versioning replaces edit model | **L** | P2 |
| P4 — dynamic UI from descriptors | **L–XL** | P2, P3 |
| P5 — save = serialize CST | **M** | P4 (legacy SaveEngine covers v6 until here) |
| P6 — Material/Media descriptor rewrite | **L** | independent; pairs with P4 |
| P7 — agent surface on CST | **L–XL** | P2–P5 |
| P8 — corpus on v7, deprecate v6 path | **M** | all; equivalence harness green corpus-wide |

**Total: one XL (derivation), two L–XL (UI, agent), the rest L/M.** The shape is a
long-thin spine (harness → seam → first slice → derivation) before the wide work
(UI, save, agent) fans out. The recommended **first thing to build is the
render-equivalence harness**, and the recommended **first slice to pressure-test is
`uniformcolor_painter` end-to-end** — because it exercises both tar-pits (R1, R2) on
the smallest possible surface, before either the XL derivation investment or any
Model-A deletion.

---

### Appendix — fate index (alphabetical, for quick lookup)

`AsciiCommandParser` EVOLVE · `AsciiScriptParser` EVOLVE/narrow · `ChunkDescriptor*`
REUSE · `ChunkDescriptorRegistry` REUSE · `CameraIntrospection` EVOLVE ·
`DAGObjectManager` likely DELETE · `DirtyTracker` DELETE · `EditHistory` DELETE ·
`Film/Light/Object/RasterizerIntrospection` EVOLVE · FOR/DEFINE/`$()`/`hal` DELETE ·
`GenericManager` REUSE · `IJob::Add*` REUSE · `Job::InitializeContainers` REUSE ·
`MaterialIntrospection` EVOLVE(rewrite) · `MathExpressionEvaluator` DELETE ·
`MediaIntrospection` EVOLVE(rewrite) · `ObjectManager` TLAS/realize UNCHANGED ·
`OverrideSpanIndex` DELETE · `ParseAndLoadScene` drive-loop DELETE/lexer-EVOLVE ·
`RawTokenCapture` EVOLVE→CST · realize/freeze/photon seam UNCHANGED · `Reference.h`
UNCHANGED · `RISE_API_Create*` REUSE · `SaveEngine` DELETE · `SceneEdit` DELETE ·
`SceneEditController` EVOLVE(split) · `SceneEditor` EVOLVE(gut) · `SourceSpanIndex`
EVOLVE→CST · `src/Library/Agent/` NEW · `TransformSnapshot` DELETE · 5 build lists
UNCHANGED(pay 5× tax) · ~30 edit tests EVOLVE/repurpose · 116 render tests UNCHANGED.
