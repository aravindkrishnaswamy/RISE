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

The dangling-reference guard is intentionally conservative — it rejects a value only when it resolves in a live manager of an allowed `referenceCategory` **and** no CST chunk of any role carries that name. A value that resolves in neither (an inline literal like `ior 1.7`, or a not-yet-created name) is never rejected, so no legitimate edit is blocked. It is scoped to the GUI `SetProperty` path (not agent edits, which the user drives separately).
