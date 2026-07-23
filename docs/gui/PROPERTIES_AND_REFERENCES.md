# RISE GUI — Generic Typed Properties, Geometry Category & Jump-to-Definition

**Status: SHIPPED 2026-07-22** (core + macOS; Windows code complete, review-verified only — the repo's standing Windows-parity convention).

This is the record of the GUI-redesign slice that made (1) geometry a
first-class outliner/inspector citizen, (2) the typed "customized UI"
property surface generic across element families, and (3) reference
parameters navigable ("right-click → Jump to Definition").

Companion specs: [ENTITY_CREATION.md](ENTITY_CREATION.md) (outliner +
CRUD; its §3.1 anticipated the `Category::Geometry` extension),
[CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md).

---

## 1. The one architectural move

Everything in this slice rides a single generalization: the shipped
`PainterIntrospection` pattern (descriptor + CST → typed rows → generic
undoable CST edit route) became **`CstIntrospection`**
([../../src/Library/SceneEditor/CstIntrospection.h](../../src/Library/SceneEditor/CstIntrospection.h)),
usable for ANY named chunk:

- **Rows** come from the chunk's `ChunkDescriptor` (the SAME descriptor
  the parser validates against — single source of truth) with CURRENT
  values read from the retained CST chunk (or `defaultValueHint` when
  the scene omitted the param).  Typed: `ValueKind` drives each shell's
  editor dispatch (bool pill / vec3 wells / filename+browse / enum
  combo / reference chip / scrubbable number well).
- **Edits** route through `ApplyAgentParamEdit` (entityKind = the
  role-kind suffix), the generic CST param-edit path — cancel-and-park,
  prior-value capture for undo/redo, dirty marking, light-generation
  bump, re-derive, re-render.  No per-family setter surface.
- **Enum rows** fold the descriptor's `enumValues` into `presets` when
  no explicit presets exist — both shells drive their enum UI off
  `presets` (the wire surface has no separate enumValues field).
- **Reference rows** carry the descriptor's `referenceCategories`
  (jump metadata, §4) and their presets are the LIVE candidate names
  enumerated from the referenced categories' managers
  (`CandidateNamesForChunkCategory`) — pick-from-what-exists.

`PainterIntrospection.{h,cpp}` was **deleted**; the painter panel is now
`CstIntrospection::Inspect(doc, job, name, "painter", ...)`.

## 2. Geometry category (`Category::Geometry = 11`)

Geometry was fully name-managed (`IGeometryManager`,
`IJob::EnumerateGeometryNames`) but deliberately excluded from the UI
category set.  Now first-class, end-to-end:

| Layer | Change |
|---|---|
| Core enum | `SceneEditController::Category::Geometry = 11` (append-only; `kNumCategories = 12`) |
| Enumeration | `CategoryEntityCount/Name` arms over `EnumerateGeometryNames` (the Medium pattern) |
| Addressing | `RoleKindSuffixForCategory → "geometry"` (matches every `*_geometry` keyword) — Duplicate / Remove / Reveal / source-trace all light up through the existing suffix-generic machinery |
| Text↔UI | `CategoryForChunkKeyword: ChunkCategory::Geometry → Category::Geometry` (reveal + reverse-select) |
| Panel | `buildRowsFor` → generic `CstIntrospection` rows; `SetProperty` → `ApplyAgentParamEdit(name, "geometry", ...)`; an edit re-derives the geometry AND every object referencing it |
| C-ABI | `SceneEditCategory_Geometry = 11` (mirror enum; existing category-parameterized functions need no new exports) |
| macOS | `RISEViewportCategoryGeometry = 11` + int→enum map case; `kOutlinerCategories` "Geometry / GEO" row |
| Windows | `ViewportBridge::Category::Geometry = 11` + translation-switch case; both `OutlinerWidget` category lists |

Creation templates for standalone geometry are NOT in this slice
(`TemplatesFor(Geometry)` is empty — geometry still enters via Object
templates' bundled `*_geo` chunks or the scene text; ENTITY_CREATION
§4.2's import/primitive-set design remains the follow-up).

**`kNumCategories` is now PUBLIC** on `SceneEditController`: the free
helper `PropsForCat` had mirrored it as a literal that went stale TWICE
(9 → silently excluding SceneVariant rows; 11 → would have silently
excluded Geometry).  Public visibility removes the mirroring hazard.

## 3. Coverage changes per family ("customized UI for all element types")

- **Geometry** — none → full typed editable panel (this slice).
- **Painter** — unchanged surface, now served by the generic inspector;
  PLUS the pre-existing gap where selecting a painter in the OUTLINER
  showed an empty panel is fixed: `RefreshProperties` now routes
  Painter/Geometry per-category rows into the PRIMARY snapshot
  (PanelMode has no value for them; the shells' single-panel path reads
  the primary snapshot).  Deliberately NOT extended to
  Animation/SceneVariant, whose empty primary drives the shells'
  "selecting activates…" message.
- **Material** — `CstIntrospection::AugmentWithCstRows` MERGES the
  generic rows into the live-introspection rows: read-only live rows
  with a CST-backed param become EDITABLE, descriptor params the live
  module never surfaced appear (e.g. dielectric `ar_film_ior`), and
  reference rows gain jump metadata.  Live-only specials (thin-film
  `ar_layer` repeatable surfaces, computed rows) are left byte-untouched;
  a programmatic/legacy scene (no CST chunk) keeps pure live rows.
  **Edit routing (external-review P1):** the `SetProperty` Material arm
  classifies the target — a painter/scalar SLOT (`GetSlot != None`)
  rebinds through `SceneEdit::SetMaterialProperty` (undo/redo + composite
  history); a non-slot VALUE param (`ar_film_ior`, `ior`, …) routes
  through the generic `ApplyAgentParamEdit` CST path, exactly like
  Painter/Geometry.  Without the split, `SetMaterialProperty` rejected
  every value param ("no slot named …") despite the row showing editable.
- **Object / Light / Medium** — `AnnotateReferenceRows` (annotate-ONLY:
  jump metadata on existing reference rows, zero behavior change).
- **Camera / Rasterizer / Film** — untouched (already fully
  descriptor-driven + live-value-backed).
- **Windows editor parity** — Enum rows with a pick list render a real
  `QComboBox` (was: free-text well; the Mac `EnumChipCell` equivalent).
  Filename-browse and preset menus already existed.
- **Shader / Modifier** — still no UI category (no `IJob` enumeration
  surface; adding one is an IJob vtable append).  With the generic
  inspector in place each is a one-arm addition once that lands — the
  documented follow-up from ENTITY_CREATION §3.1.

## 4. Jump-to-definition

The CST descriptors declare, per Reference param, WHAT it may name
(`ParameterDescriptor::referenceCategories` — the same metadata
`BuildReferenceGraph`/`ComputeChunkRefs` resolve reference edges with).
This slice plumbs it to the shells:

- `CameraProperty::referenceCategories` — populated by the generic
  inspector (and by Augment/Annotate for the live-module families).
- `SceneEditController::PropertyJumpTarget{,For}` — resolves a row's
  value against the LIVE managers **first-wins across the declared
  categories** (the `ComputeChunkRefs` order, so the jump lands on the
  element the renderer actually bound), then maps `ChunkCategory → UI
  Category` (`Function → Painter`: colour painters dual-register as
  functions and functions have no UI section).  False for non-Reference
  rows, empty values, inline numerics (`tau 0.9`), the `none` sentinel
  (the registered null painter/geometry, not an authored binding), and
  dangling references — the shells simply don't show the menu item.
  The live-manager enumeration is guarded by a `try_lock` on the
  controller mutex (external-review P1): an agent CST edit on another
  thread swaps the managers under `mMutex` mid re-derive, so the jump
  either enumerates a stable set or bails for this click — it never
  blocks (a production/agent render holds `mMutex` for its whole
  duration; a blocking guard would freeze the UI).
- C-ABI: `RISE_API_SceneEditController_PropertyJumpTarget{,ForCategory}`.
- **macOS**: the property row's existing context menu (next to "Reveal
  in Scene File") gains *Jump to Definition of "name"* — resolution at
  menu-build time; navigation via `RenderViewModel.jumpToEntity`, which
  reuses the reverse-select epoch so the outliner highlight + inspector
  re-snapshot through the already-wired flow.
- **Windows**: Reference-row line-edits get a custom context menu
  (standard edit menu preserved via `createStandardContextMenu`) with
  the same item; navigation = `setSelection` + panel `refresh()` (the
  outliner poll follows).

## 5. Tests

[`tests/GeometryPanelJumpTest.cpp`](../../tests/GeometryPanelJumpTest.cpp)
(T1–T6): enumeration; typed rows (radius = Double) on both the
per-category AND primary snapshots; edit round-trip through the CST +
Undo; jump resolution for Object→Geometry and Material→Painter;
inline-numeric negative case; material augment (`ar_film_ior` appended
editable + typed); **T6 (external-review P1): a material value-param edit
(`ior`) SUCCEEDS and round-trips through the retained CST** — both the
success bool and the persisted value are mutation-verified (reverting the
Material-arm slot-vs-value classification fails exactly those two).  The
Object→Geometry jump assertion is likewise mutation-verified (breaking
the `ChunkCategory::Geometry → Category` mapping fails that check).
`tests/EntityTemplatesTest.cpp` migrated to the `CstIntrospection` API.

## 6. External-review round (2026-07-22)

A post-implementation review found 2 P1 + 6 P2 + 1 P3, all fixed:

| Sev | Issue | Fix |
|---|---|---|
| P1 | Material value params (`ar_film_ior`) showed editable but `SetMaterialProperty` rejected every non-slot field — the edit silently failed | `SetProperty` Material arm classifies slot-vs-value; value params route through `ApplyAgentParamEdit` (see §3).  Test T6. |
| P1 | Jump resolution enumerated LIVE managers with no lock → UAF vs a concurrent agent CST re-derive | `try_lock` on `mMutex` in `ResolveRowJumpTarget_` — stable-set-or-bail, never blocks (see §4). |
| P2 | `CandidateNamesForChunkCategory(Function)` offered the painter union — omitted real Function1D/2D names, offered scalar painters that can't bind | Split: `Function` enumerates the `GetFunction1Ds`/`GetFunction2Ds` managers (dual-registered painters live there too). |
| P2 | The `none` sentinel resolved as a jump target | `ResolveRowJumpTarget_` skips `value == "none"`. |
| P2 | Rasterizer Reference rows (`radiance_map`) had no jump — `RasterizerIntrospection` dropped `referenceCategories` | Copy `cp.referenceCategories = p.referenceCategories` in the descriptor loop. |
| P2 | N-up layouts left Inspector/Outliner stale after a jump — only the single-viewport branch observed `reverseSelectEpoch` | Added the `.onChange(of: reverseSelectEpoch)` refresh to the `MultiPaneViewportView` branch. |
| P2 | C-ABI jump exports returned success after silently truncating the target name into the 128-byte shell buffer | Both exports `return false` when `strlen(name) >= bufLen` (the name is a lookup key). |
| P2 | `SelectionStillResolves` omitted `Geometry` → a removed/undone geometry left a stale selection | Added the `Cat::Geometry` case (GetItem on `GetGeometries`). |
| P3 | Windows inspector showed geometry as "Scene / •" | Added the `Geometry` title (`Geometry`) + glyph (`◯`) cases. |

### Second review round (2026-07-22) — 3 more P1s

| Sev | Issue | Fix |
|---|---|---|
| P1 | `SetSelection` indexed the fixed `mSelectionByCategory` / `mSectionExpanded` arrays by a raw C-ABI `int` with no bounds check -> OOB write / memory corruption for `-1` or `>= kNumCategories` | Reject `[0, kNumCategories)`-out-of-range before any indexing. |
| P1 | `RefreshProperties` (and the material-slot classification) read live scene/CST/manager state with no lock, racing an agent commit's D2 manager swap -> UAF | `RefreshProperties` serializes under `mMutex` (early-returns during a render via `mRenderOwnsScene` so it never blocks the UI); the classification read moved under a brief lock. |
| P1 | GUI reference edits persisted through `RouteCstParamEdit_` without validating the target resolves -> binding a runtime-only entity (no CST chunk) wrote a **dangling reference** that renders now but fails on save/reload | General guard `WouldPersistDanglingReference_` at the `SetProperty` boundary: rejects ONLY the precisely-diagnosable runtime-only-registered-but-not-CST case; inline literals, CST-backed names, and `none` all pass. Covers every reference-carrying category uniformly. Test T7. |

The dangling-reference guard is intentionally conservative — it rejects a value only when it resolves in a live manager of an allowed `referenceCategory` **and** no persistable CST target exists (see the round-3 refinement below). A value that resolves in neither (an inline literal like `ior 1.7`, or a not-yet-created name) is never rejected, so no legitimate edit is blocked. It is scoped to the GUI `SetProperty` path (not agent edits, which the user drives separately).

### Third review round (2026-07-22) — 4 more P1s

| Sev | Issue | Fix |
|---|---|---|
| P1 | The outliner enumeration getters (`CategoryEntityCount/Name/ActiveName`) and `SetSelection`'s Object auto-sync still read live managers with no lock → UAF vs an any-thread agent D2 re-derive | Locked-public / unlocked-private split: the public getters take `mMutex` (with the `mRenderOwnsScene` early-return); the `*Locked_` twins serve the two internal under-mutex callers (`SourceRefAtByteOffset`'s addressability probe, `InstantiateEntityTemplate`'s name-pick scope). The auto-sync resolves both names under a brief hold. Blocking is safe: GUI edits run on the UI thread itself, so a poll only ever contends brief background holds. |
| P1 | `RefreshProperties`' blocking lock could deadlock: the dirty-changed listener fires **under the held `mMutex`** (`ApplyAgentParamEdit → MarkCstHeadDirty`), so a synchronous listener that refreshed the panel would self-deadlock — and even the shells' deferred listeners could freeze the UI for a background commit's duration | `try_lock`; on contention keep the prior rows (self-heals on the next frame/epoch refresh). The listener threading contract is now documented on `SetDirtyChangedListener` (both shells already defer: `dispatch_async(main)` / `Qt::QueuedConnection`). |
| P1 | The guard's CST probe accepted ANY same-named chunk — a chunk of the wrong category (a shader named like the runtime painter) or a later-declared target vouched for a value that still fails on reload | Mirror the resolver: the target chunk's registry category must satisfy one of the param's `referenceCategories` (including `BuildReferenceNamespace`'s two dual-registers: colour painter ⇒ Function2D, `piecewise_linear_function` ⇒ colour painter) **and** be declared before the referencing chunk (`DeriveToJob` is sequential — forward references fail at bind time). Tests T8 (category, mutation-verified) + T9 (order, mutation-verified). |
| P1 | Rasterizer property edits were runtime-only: `SetRasterizerParameter` rebuilt the instance but nothing recorded the change → `samples 8 → 16` lost on save/reload, and `HasUnsavedChanges` never flipped | New `Job::ApplyCstRasterizerParamEdit(kind, param, value)` — the kind-addressed twin of `ApplyCstEnvironmentEdit` (Document-only patch of the unique chunk of that kind; absent chunk = loud soft no-op — synthesizing one is unsafe, rasterizer chunks carry shader wiring). The `SetProperty` Rasterizer arm records + marks dirty. **Sibling audit**: the Film arm, the Film-preset pick, and the Environment arm had the same dirty gap (Document-only edits never flipped `HasUnsavedChanges`, leaving Save disabled) — all three now `MarkCstHeadDirty`. Test T10. |

### Fourth review round (2026-07-22) — 3 more P1s, and the class-level fixes

| Sev | Issue | Fix |
|---|---|---|
| P1 | An UNRECORDABLE rasterizer/environment edit (no unique chunk of that kind — default/lazily-created rasterizer) applied live, failed the record, and STILL marked dirty — Save then cleared the flag while serializing no change; reload reverted the edit ("saved but lost") | **Transactional ordering**: validate recordability BEFORE the live edit and refuse with a diagnostic if the Document cannot carry it (the same validate-and-reject policy as the dangling-reference guard); every dirty mark (rasterizer, environment, both film sites) is now gated on the record actually landing. Legacy scenes (no retained Document) keep live-only editing. Test T11. |
| P1 | The round-3 blocking enumeration getters could DEADLOCK a C-ABI dirty-changed callback that re-enters the controller (the callback fires on the mutating thread with `mMutex` held; the C contract cannot forbid out-of-tree re-entry) | **Snapshot getters** (the class fix): `CategoryEntityCount/Name/ActiveName` serve a per-category snapshot refreshed under a `try_lock` — on contention (including same-thread re-entry) they serve the prior snapshot. Never blocks, never deadlocks, never races a manager swap, never flickers empty (the outliner now keeps its last list during production renders too). The C-ABI callback contract is documented in `RISE_API.h`. Test T12 (calls a getter INSIDE the under-lock listener; hangs on regression). |
| P1 | `ApplyCstRasterizerParamEdit` landed mid-`IJob`-vtable (next to its semantic sibling), shifting every later slot — an ABI break for out-of-tree binaries | Relocated to the IJob tail per the append-only convention — and the convention is now MECHANICAL: `SourceHygieneTest` compares IJob.h's virtual declaration order against `tests/IJobVtableManifest.txt` (a tail append = one new manifest line in the same commit; a mid-insert/reorder/removal fails the suite with an actionable message). Mutation-verified. |

## 7. Why these kept happening — the fragility analysis

Four review rounds produced 12 P1s on this slice. They are not twelve
independent mistakes; they cluster into three structural classes:

**A. Dual-representation coherence (6 of 12).** The editor holds the scene
THREE ways — live object graph, retained CST Document, dirty tracker — and
every bespoke edit arm hand-rolls its own live+record+dirty sequence.
Every hand-rolled sequence is a fresh chance to miss a leg: editable-but-
unsavable (live said yes, CST said no), dangling references (live accepted
what the CST couldn't carry), runtime-only rasterizer edits (live moved,
CST didn't), dirty-but-unrecorded (flag moved, CST didn't), film/environment
dirty gaps (CST moved, flag didn't).  Telling datapoint: the arms that
route through `ApplyAgentParamEdit` (painter, geometry, material value
params) have had ZERO of these — that path IS a transactional chokepoint.
**Direction: finish the Model-B migration for the panel — move the
remaining bespoke arms (Rasterizer, Film, Environment, Material slot
rebinds) onto the CST-first route and delete the hand-rolled sequences.**
This round's pre-flight + gated marks make the bespoke arms coherent;
migration would make them disappear.

**B. Advisory locking on live-state reads (5 of 12).** `mMutex` guards the
managers but nothing made readers hold it, so each surface picked its own
discipline (none, blocking, try) and each new discipline minted a new
deadlock/UAF edge.  The class fix (this round): UI-facing reads never
require the lock — they serve snapshots refreshed opportunistically under
`try_lock` (enumeration getters + `RefreshProperties`); live managers are
only ever touched with the lock held.  Any future UI-read surface should
follow the snapshot pattern, not invent a locking discipline.

**C. Comment-enforced conventions (3 of 12 + the historical
`kNumCategories` drift).** Append-only vtables, mirrored enum literals,
bridge switches — all "remember to do X" rules that fail exactly when
nobody re-reads the right comment.  Where possible, make them mechanical:
`kNumCategories` became a public constant (compiler-enforced), the vtable
order is now a manifest the suite diffs (SourceHygieneTest).  The bridge
enum translation switches remain convention-guarded — a candidate for the
same manifest treatment.

### Fifth review round (2026-07-22) — 1 P0 + 3 P1s on the phase-1–3 landing

| Sev | Issue | Fix |
|---|---|---|
| P0 | `RequestSave` serializes the borrowed live Document during its unlocked step 2, and only the render thread honored `mSaving` — an agent commit could free/replace the Document mid-serialize (UAF), or land after serialization and be baselined "clean" | `mSaving` now gates every Document-mutating entry point: agent param/CRUD entries reject retriable ("save in flight"); `SetProperty`/`Undo`/`Redo` refuse with a log.  Belt: step 1 captures `headAtSerialize`, and step 3 baselines clean ONLY if the head is still that version — a slipped mutation can never be silently marked saved. |
| P1 | The round-4 derived-dirty wiring read the non-atomic head/saved versions from the lock-free C-API dirty query — torn-read UB | The floor is served from a cached `std::atomic<bool>`, recomputed at every mutation's Fire (under the mutator's own lock → race-free Job read), at baseline capture, and by a per-frame self-heal in `RefreshProperties`' `try_lock` region. |
| P1 | Direct mutation APIs (`SetEnvironmentScale`…) marked dirty but never drained — headless/C-API users could miss the notification indefinitely; and a clean→dirty→clean burst nets to no callback | Drain added at `SetEnvironmentRadianceParam_`'s post-unlock tail.  The coalescing is now the DOCUMENTED contract (SceneEditor + C-ABI): the listener reports the current value when it differs from the last report — consumers key on the value, never count calls. |
| P1 | `DrainDirtyNotification` invoked the listener while holding `mNotifyMutex` — a mutating callback's nested drain self-deadlocks, contradicting ADR rule 5 | Single-deliverer drain: state comparison under the leaf lock, listener invoked with NO lock held; racing/nested drains re-flag pending and the active deliverer loops, keeping delivery ordered and re-entry deadlock-free. |

## 8. The structural answer — the Document-First Editor (ADR)

The class-level cures for A and B are now the STATED ARCHITECTURE:
[ADR_DOCUMENT_FIRST_EDITOR.md](ADR_DOCUMENT_FIRST_EDITOR.md).  Phases 1–3
landed with this slice:

- **Phase 1 — derived dirty + deferred notification.**  `HasUnsavedChanges`
  is floored by `headVersion != savedHeadVersion` (a Document commit after
  the last save ALWAYS reads dirty); the dirty listener fires from a
  post-unlock drain (`DrainDirtyNotification`, with a per-frame catch-all
  in `RefreshProperties`), never under `mMutex`.
- **Phase 2 — one `EditorUiSnapshot`.**  The piecemeal panel/enumeration
  snapshots consolidated behind one leaf mutex with one publish function;
  every read accessor serves the struct.  New UI-visible data goes IN THE
  STRUCT (ADR rule 4), never an ad-hoc live read.
- **Phase 3 — singleton addressing + the Rasterizer arm migration.**  The
  CST transaction path's unique-in-kind fallback generalized from
  camera-only to any kind addressed with an empty name; the `SetProperty`
  Rasterizer arm became a plain `ApplyAgentParamEdit("", <kind>, …)` call.
  The bespoke live-rebuild + record + mark sequence (source of the round-3
  AND round-4 rasterizer P1s) is deleted, rasterizer edits gained
  undo/redo (T13), and round-3's `Job::ApplyCstRasterizerParamEdit` —
  subsumed by the transaction path before it ever shipped — was removed
  (manifest updated in the same commit).  Film / Film-preset / Environment
  are the documented phase-3b work list.
