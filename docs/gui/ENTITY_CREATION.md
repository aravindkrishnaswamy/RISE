# RISE GUI — Entity Creation, Outliner & Reference-Safe Deletion

**Status:** DESIGN. **Hardened 2026-06-20** after a *second* adversarial review,
against the two now-CONFIRMED decisions below. That pass found five mechanical
gaps (deletion persistence was undesigned and contradicted the CAMERAS spec; the
"every family" claim silently excluded geometry / painters / shaders / modifiers;
the reference graph was object-only; bulk glTF import bypassed the one mutation
path entirely; and "session visibility" was misnamed and load-bearing in a way it
can't support). This revision closes all five — §5.4 (managed-tombstone deletion),
§3.1 + §4 (all-families coverage), §5.1 (descriptor/introspection-driven
dependency graph), §10 (import-as-transaction), §3.3 (preview-isolation rename).
**Owner:** Aravind Krishnaswamy

> **Confirmed decisions (2026-06-20) — the frame this spec is mechanically correct against.**
> 1. **Deletion persistence is a managed tombstone.** A delete that must survive
>    reload emits an ordered `> remove {family} <name>` command into the managed
>    override block (the same sentinel-bracketed, wholesale-rendered block the
>    round-trip save engine already owns). It is **not** a new scene-format
>    construct — `> remove` is an existing parser command (§5.4).
> 2. **Coverage is all families + a descriptor/introspection-driven dependency
>    graph + glTF import as one atomic transaction.** The outliner and the
>    create/delete ops span objects, lights, materials, **painters**, media,
>    geometry, shaders, modifiers, and cameras; reference-safety is derived
>    generically from descriptor reference fields + introspection, not hand-coded
>    per object; and a bulk import commits as a single transaction (§3.1, §5.1, §10).
>    **Scope caveat (§5.1):** the *descriptor* half (which families can reference
>    which) exists today, but the *introspection* half (live bound names) covers only
>    the seven families that have an `*Introspection` surface — so **painter→painter
>    and composed/sugar-material edges are TO-BUILD** (`PainterIntrospection` +
>    composed-material reference-readback must be added + tested); the graph is
>    drift-proof only once those land, not today.

**Scope:** The GUI workflow for *building* a scene — a scene-graph **outliner**
panel; **add / duplicate / delete** for **every** entity family (objects, lights,
materials, painters, media, geometry, shaders, modifiers, cameras); **reference-safe
deletion** (a descriptor/introspection-driven dependency graph, block-or-cascade
policy); **managed-tombstone deletion persistence** so a deleted file-authored
entity stays deleted across reload; stable **naming** + rename; **glTF model /
texture import** wired into an "Add Model" action **as one atomic import
transaction**; and extending **Phase C created-entity persistence beyond cameras**
so non-camera creations round-trip to the `.RISEscene`. This spec owns the
roadmap's Phase 0 entity-creation gap ([GUI_ROADMAP.md](../GUI_ROADMAP.md) §11
Phase 0, §14). It does **not** cover material *graph* editing
([MATERIAL_EDITOR.md](MATERIAL_EDITOR.md)), camera *optics* / named views
([CAMERAS_AND_VIEWS.md](CAMERAS_AND_VIEWS.md)), or the AI tool surface
([MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md)) — each consumes the creation/deletion
ops this spec defines.

> **Ground-truth basis.** Every capability claim below is reconciled against
> [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) (code-verified, supersedes
> plan-doc `Status:` headers) and cited to `file:line`. Plan-doc status headers
> were treated as suspect.

> **Transaction-model basis.** Every add / duplicate / delete / rename / import
> here is a **transaction** as defined by [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md)
> (the authoritative owner of what a transaction is, the `(documentUUID, revision)`
> precondition, the one-shot whole-payload `Propose` / `Commit` surface, and undo
> attribution). This spec defines the *ops*; TRANSACTION_MODEL owns their
> epoch / precondition / snapshot-publication / attribution semantics. Where this
> spec says "composite transaction" it means the `SceneTransaction` of
> [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §4.1 — an ordered `std::vector<SceneEdit>`
> committed atomically under the controller's single-writer `mMutex`, never a
> bespoke write path.

---

## 1. The gap, precisely

The roadmap markets RISE as moving "from render viewer to creative tool," yet a
creative tool that cannot create is the core hole an adversarial review found:
**there is no GUI path to build a scene from nothing** — no outliner, no add,
no delete, no import. The audit pins this exactly:

- **The only creation path anywhere is `CloneActiveCamera`**
  ([SceneEditController.cpp:3103](../../src/Library/SceneEditor/SceneEditController.cpp)),
  whose sole creation op is `SceneEdit::AddCamera`
  ([SceneEdit.h:212](../../src/Library/SceneEditor/SceneEdit.h)). The
  `SceneEdit::Op` enum has **no** `AddObject` / `AddLight` / `AddMaterial` /
  `AddMedium` / `Remove*` / `Import*` value — every other op mutates an
  *existing* entity (audit §2).
- **Persistence substrate is half-built.** Round-trip save *is* implemented —
  Mode A/B transforms + Phase B property re-emit for camera/light/material/
  medium + Phase C created-entity emit ([SaveEngine.cpp](../../src/Library/SceneEditor/SaveEngine.cpp),
  audit §1). But **Phase C is cameras-only**: `SaveEngine.cpp:1351`
  ("`V1: only cameras are creatable`"), and a non-camera chunk inside the
  managed block is a hard **Refused** at `SaveEngine.cpp:1373` (audit §2). So a
  created light/material/object has *nowhere to be saved* today.
- **The factories already exist.** `Job::AddObject`
  ([Job.cpp:5180](../../src/Library/Job.cpp)), `AddPointOmniLight` /
  `AddPointSpotLight` / `AddAmbientLight` / `AddDirectionalLight`
  ([IJob.h:1405-1436](../../src/Library/Interfaces/IJob.h)), the 20+ material
  factories, the media factories, and `RemoveObject` / `RemoveLight` /
  `RemoveMaterial` / `RemoveGeometry`
  ([Job.cpp:9144-9185](../../src/Library/Job.cpp)) are all present on `IJob`.
  They are simply **not surfaced** as controller ops, as `SceneEdit::Op`
  transactions, or in any GUI.

So the work is four layered pieces, all but the widget shared:

| Layer | What's new | Where |
|---|---|---|
| **Outliner model** (shared C++) | scene-graph tree across **all entity families** (objects, lights, materials, **painters**, media, geometry, shaders, modifiers, cameras) + the singleton rows, selection sync, preview isolation | extend `SceneEditController` (the `CategoryEntityCount/Name` surface already exists for the five list categories; **painters / geometry / shaders / modifiers need new `Category` values** — §3.1) |
| **Creation/deletion ops** (shared C++) | new `SceneEdit::Op` values + controller entry points + the **descriptor/introspection-driven** dependency-graph query for safe delete (§5.1; the painter→painter and composed-material edges need **new** `PainterIntrospection` + composed-material ref-readback — TO-BUILD, §5.1) + the managed-tombstone delete-persistence path (§5.4) | `SceneEdit.h`, `SceneEditor.cpp`, `SceneEditController.{h,cpp}`, `ReferenceGraph.{h,cpp}`, **new `PainterIntrospection.{h,cpp}`** |
| **Phase-C persistence** (shared C++) | per-family chunk re-emit so non-camera creations save, plus the managed-tombstone `> remove` emit | `SaveEngine.cpp`, `*Introspection.{h,cpp}` |
| **Outliner widget** (platform) | the tree view + add/delete buttons + drag | macOS SwiftUI, Windows Qt, Android Compose |

> **Coverage is genuinely *all* families — not "the five list categories."** A
> second-review finding: "Add Material" cannot work without painters (the default
> material binds a `uniformcolor_painter`, §4.2), yet the existing controller
> surface (`Category::Camera/Light/Object/Material/Medium`,
> [SceneEditController.h:207-223](../../src/Library/SceneEditor/SceneEditController.h))
> has **no** painter / geometry / shader / modifier category, and the audit's
> operation table omits painters entirely (audit §2). This spec therefore extends
> the `Category` enum and every create/delete op to **objects, lights, materials,
> painters, media, geometry, shaders, modifiers, cameras** — the same nine families
> the managers already expose (`IPainterManager` / `IScalarPainterManager` /
> `IGeometryManager` / `IShaderManager` / `IModifierManager` each `: public virtual
> IManager<T>`, [IPainterManager.h:22](../../src/Library/Interfaces/IPainterManager.h)
> et al.). Adding a `Category` value triggers the bridge-enum-translation audit
> ([MEMORY: bridge-enum-translation-audit] — grep `case 5:` in both bridges).

---

## 2. Principles honored

From [GUI_ROADMAP.md](../GUI_ROADMAP.md) §1, the load-bearing ones for this spec:

- **§1.1 Text is the source of truth.** Creation **and deletion** round-trip to
  the `.RISEscene`; the GUI is an authoring surface, not a parallel store. A
  created entity is durable only once it persists (§7); a **deleted** file-authored
  entity is durably gone only once a managed tombstone persists (§5.4) — otherwise
  it returns on reload.
- **§1.2 Maximize shared C++.** The outliner *model*, the creation/deletion ops,
  the descriptor-driven dependency graph, the tombstone emit, and the save logic
  are all `src/Library/`. Only the tree *widget* and the file/import dialog are
  platform code (§9).
- **§1.6 Everything routes through one mutation path.** Every add / duplicate /
  delete / rename / **import** is a `SceneTransaction` through `SceneEditor::Apply`
  → `SceneEditController` ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md)), so it is
  undoable, diff-reviewable, attributed, and persistent — the same path GUI edits,
  hand-edits, and AI edits converge on. **Bulk glTF import is the one place this is
  currently violated** (`Job::ImportGLTFScene` mutates the managers directly with no
  transaction, §10.0) — this spec routes the interactive import through the one path
  as a single import transaction.
- **§16 decisions.** Android scene-save is **Tier A** (wire `nativeSaveAs` to
  real `.RISEscene`); creation/deletion ride that. Avoid the bare `MCP` token
  (irrelevant here — no new agent types).

Per the [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) companion spec (the
authoritative model for editor transactions, `(documentUUID, revision)` document
identity, published immutable snapshots, and preconditions;
[GUI_ROADMAP.md](../GUI_ROADMAP.md) §14), **creations, deletions, and imports are
transactions**: each carries a `(UUID, baseRevision)` precondition, a forward
apply, attribution, and a captured-prev for undo. This spec defines the
creation/deletion/import ops; TRANSACTION_MODEL owns their precondition /
snapshot-publication / attribution / reconciliation semantics. Two hard
preconditions specific to this spec, drawn from the audit:

1. A creation/deletion that mutates the manager set or an object's bbox **must
   run under cancel-and-park** — the same gate `CloneActiveCamera` already takes
   ([SceneEditController.cpp:3120-3128](../../src/Library/SceneEditor/SceneEditController.cpp))
   — because manager mutation and TLAS invalidation race the render thread.
2. After an object add/delete the **TLAS must be invalidated and rebuilt**
   (`IObjectManager::InvalidateSpatialStructure` / `PrepareForRendering`,
   [IObjectManager.h:70-74](../../src/Library/Interfaces/IObjectManager.h)) — see
   §8.5.

---

## 3. The outliner panel

A persistent left-rail **scene-graph tree** — the navigation spine the GUI
lacks. It is the read view that the add/delete actions and the property panel
hang off.

### 3.1 What it shows

One top-level group per entity family. The shipped controller has only five list
categories — `Category::Camera/Light/Object/Material/Medium`
([SceneEditController.h:207-223](../../src/Library/SceneEditor/SceneEditController.h)) —
plus the singleton/active-selection **Rasterizer**, **Film**, **Animation** rows.
A second-review finding: that set **omits geometry, painters, shaders, and
modifiers**, so the "outliner across every family" claim was false against the
code. This spec **extends the `Category` enum** with `Geometry`, `Painter`,
`Shader`, and `Modifier` (the managers already exist — `IGeometryManager` /
`IPainterManager` / `IScalarPainterManager` / `IShaderManager` /
`IModifierManager`, all `: public virtual IManager<T>`). The full outliner is then:

| Group | Backed by | List-creatable? |
|---|---|---|
| **Objects** | `IObjectManager` (`Category::Object`) | yes (§4.2) |
| **Lights** | `ILightManager` (`Category::Light`) | yes |
| **Materials** | `IMaterialManager` (`Category::Material`) | yes |
| **Painters** | `IPainterManager` + `IScalarPainterManager` (**new** `Category::Painter`) | yes — required by the default material (§4.2) |
| **Media** | media managers (`Category::Medium`) | yes (Homogeneous only) |
| **Geometry** | `IGeometryManager` (**new** `Category::Geometry`) | via import or built-in primitives (§4.2, §10) |
| **Shaders** | `IShaderManager` (**new** `Category::Shader`) | yes |
| **Modifiers** | `IModifierManager` (**new** `Category::Modifier`) | yes |
| **Cameras** | `ICameraManager` (`Category::Camera`) | clone (§4) |
| Rasterizer / Film / Animation | singleton/active rows | not list-creatable |

Each group lists its entities by name; the active camera and active
rasterizer/animation are badged. **Painters span two managers** (`IPainter` colour
pipe + `IScalarPainter` physical-scalar pipe — the [ISCALARPAINTER_REFACTOR.md](../ISCALARPAINTER_REFACTOR.md)
distinction); the Painters group unions both, and the per-painter row carries its
pipe so the create/bind UI routes a colour painter vs a scalar painter correctly.

The model for the **five existing** categories is already present — the controller
exposes:

- `CategoryEntityCount(cat)` ([SceneEditController.cpp:1747](../../src/Library/SceneEditor/SceneEditController.cpp))
- `CategoryEntityName(cat, idx)` ([:1811](../../src/Library/SceneEditor/SceneEditController.cpp))
- `CategoryActiveName(cat)` ([SceneEditController.h:389](../../src/Library/SceneEditor/SceneEditController.h))
- `SetSelection(cat, name)` ([:2060](../../src/Library/SceneEditor/SceneEditController.cpp))

These already back the per-platform accordion dropdowns (audit cross-platform
matrix; macOS `PropertiesPanel.swift`, Windows `ViewportBridge`, Android
`ViewportPane.kt`). For the **four new** categories the same accessors are
extended to enumerate the corresponding manager via `IManager::EnumerateItemNames`
([IManager.h:47](../../src/Library/Interfaces/IManager.h)). The outliner is then a
**tree presentation over the (extended) accessors** plus the new add/delete
affordances — not a new data model. Every new `Category` value triggers the
bridge-enum-translation audit ([MEMORY: bridge-enum-translation-audit]).

### 3.2 Selection sync (already wired — reuse it)

Selection is a single `(Category, entityName)` tuple
([SceneEditController.h:316-364](../../src/Library/SceneEditor/SceneEditController.h)).
Picking in the viewport already routes through the **shared** `PickAt`
([SceneEditController.cpp:2322](../../src/Library/SceneEditor/SceneEditController.cpp)):
cast ray → `IObjectManager::IntersectRay` → resolve object name →
`SetSelection(Object, name)`, which auto-expands the Objects section and
auto-tracks the bound Material via `GetSelectionNameForCategory`
([SceneEditController.h:~390](../../src/Library/SceneEditor/SceneEditController.h)).
The outliner must be **bidirectionally bound** to this tuple:

- Click a row in the outliner → `SetSelection(cat, name)` → viewport highlights
  + property panel populates.
- `PickAt` in the viewport → outliner row highlights + scrolls into view.

Both directions go through the *one* selection tuple; no new selection state.
(The audit corrects the stale "picking is irreducibly per-platform" claim — only
raw pointer-event capture is platform code; `PickAt` is shared, audit §6.)

### 3.3 Preview isolation (the eye toggle) — owner-private, NOT durable visibility

A per-object eye toggle. A second-review finding: the v1 name "render visibility"
/ "session-visibility" was **misleading** — it implies the object won't render,
but the toggle only hides the object from the *owner's interactive preview*; it is
not persisted, AI/diff tooling can't see it, and a **production render restores the
object** (it was never removed from the manager or the file). Calling that
"render visibility" sets a false expectation. **Renamed "preview isolation."**

- **Preview isolation (ships first):** owner-private, UI-only. An isolated object
  is skipped from the **interactive preview** by the controller (it is *not*
  removed from the manager and *not* written to the file). This maps cleanly onto
  the TRANSACTION_MODEL notion of **owner-private state that is never published and
  never snapshotted** ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §3.3) — it
  bumps no revision, publishes no snapshot, and therefore is invisible to every
  other reader: background production renders, thumbnails, RMSE, the AI's scene
  context, and the "show me the code" diff all read a published snapshot and so
  **see the object as present**. It needs no scene-format change and is
  **explicitly non-persisted** (read-only-by-design,
  [MEMORY: roundtrip-scope-prefer-readonly]). The UI must label it as a viewport
  affordance ("hidden in viewport"), never "hidden from render," so the
  production-render-restores-it behaviour is not a surprise.
- **Durable visibility is a separate real scene feature (out of v1).** Genuinely
  hiding an object from *all* renders — durably, visibly to AI and to the diff —
  is **not** what preview isolation does. It would need a real `visible` object
  flag (a scene-format addition, with its own round-trip-save and introspection
  surface) so that the hidden state is part of a published snapshot and persists.
  That is a separate feature deferred to a later phase; do **not** retrofit it onto
  preview isolation. (Note `castsShadows FALSE` + zero-contribution is *not*
  hiding — the object still occludes and still appears in primary rays.)

A camera/light/material/painter/medium/geometry/shader/modifier has no "preview
isolation" — the eye column is objects-only; other categories show an
active/inactive badge instead.

### 3.4 Android tier

**Tier B, touch-adapted** ([GUI_ROADMAP.md](../GUI_ROADMAP.md) §10.4): the
outliner is a collapsible Compose list (the accordion already renders on
Android, audit matrix). Long-press a row → context menu (duplicate/delete/
rename); a `+` FAB per group for add. No drag-reparent on mobile (RISE's object
model is flat anyway — see §3.5).

### 3.5 Flat, not hierarchical

RISE's `standard_object` has **no parent/child hierarchy** — glTF node trees are
*flattened* at import ([GLTF_IMPORT.md](../GLTF_IMPORT.md) §5, node-transform
flattening; the importer emits per-primitive flat objects). The outliner tree is
therefore **two levels**: category → entity. There is no reparenting, no group
nodes. (If hierarchy is ever wanted it is a scene-model change far outside this
spec.)

---

## 4. Add / duplicate / delete — as transactions

Every action is a `SceneTransaction` ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md)
§4.1) — an ordered `std::vector<SceneEdit>` with a `(documentUUID, baseRevision)`
precondition + attribution, committed atomically under the controller's
single-writer `mMutex` (TRANSACTION_MODEL §4.2). A single-edit action (add one
light) is a one-edit transaction; a composite action (delete with cascade
rebinds, §5.2; bulk import, §10) is a multi-edit transaction committed as **one
unit**. The C-ABI surface is the one-shot whole-payload `Propose` / opaque
per-client `TxnBuilder` (TRANSACTION_MODEL §12) — **not** a controller-global
builder, so the AI agent and external MCP clients drive these ops through the same
path without an edit-interleaving race.

The `AddCamera` op
([SceneEdit.h:204-213](../../src/Library/SceneEditor/SceneEdit.h)) is the
template: it carries the new name in `objectName`, captures the prior
active-camera in `prevPropertyValue` for undo, and embeds a full state snapshot
(`cameraSnapshot`) so **Redo recreates the entity deterministically** even if the
source changed. New ops follow that shape.

### 4.1 New `SceneEdit::Op` values

Coverage spans **all families** (the second-review finding that painters were
omitted — yet creating the default material requires one — is closed here):

| Op | Payload (forward) | Undo route | Notes |
|---|---|---|---|
| `AddObject` | `objectName` = new name; refs in `propertyValue`/`propertyName` (geometry, material) + `prevTransform` (initial transform) | `RemoveObject(name)` | needs geometry + material refs to exist (§8); TLAS rebuild |
| `RemoveObject` | `objectName`; full object snapshot captured in prev fields | re-`AddObject` from snapshot | TLAS rebuild; reference-safe only matters *into* the object, see §5 |
| `AddLight` | `objectName` = name; `propertyName` = light type; snapshot of light params | `RemoveLight(name)` | must recompute light sampler (§8.2) |
| `RemoveLight` | `objectName`; light-param snapshot | re-`AddLight` | recompute sampler |
| `AddMaterial` | `objectName` = name; `propertyName` = material type; slot painter names | `RemoveMaterial(name)` | created unbound; binding is a separate `SetObjectMaterial`; **requires a painter to exist** (§4.2, §8.4) |
| `RemoveMaterial` | `objectName`; slot snapshot | re-`AddMaterial` + rebind | **reference-safe**: blocked/cascaded (§5) |
| `AddPainter` | `objectName` = name; `propertyName` = painter type + **pipe** (colour `IPainter` vs scalar `IScalarPainter`); value snapshot | `RemovePainter(name)` | the prerequisite for `AddMaterial`; routes to `IPainterManager` or `IScalarPainterManager` by pipe ([ISCALARPAINTER_REFACTOR.md](../ISCALARPAINTER_REFACTOR.md)) |
| `RemovePainter` | `objectName`; painter snapshot + pipe | re-`AddPainter` | **reference-safe**: painters are referenced by materials *and* by other painters (blend chains) — see the generic graph (§5.1) |
| `AddMedium` | `objectName` = name; `propertyName` = type; coeffs | `RemoveMedium(name)` | Homogeneous only creatable (Heterogeneous needs a baked grid, audit §3) |
| `RemoveMedium` | `objectName`; coeff snapshot | re-`AddMedium` + rebind interiors | **reference-safe** (§5) |
| `AddGeometry` (import) | `objectName` = name; descriptor of the geometry chunk | `RemoveGeometry(name)` | glTF/PLY/etc.; see §7, §10 |
| `RemoveGeometry` | `objectName`; chunk descriptor | re-`AddGeometry` | **reference-safe** (objects reference geometry, §5) |
| `AddShader` | `objectName` = name; `propertyName` = shader type; op-list snapshot | `RemoveShader(name)` | created unbound; bound via `SetObjectShader` |
| `RemoveShader` | `objectName`; shader snapshot | re-`AddShader` + rebind | **reference-safe** (objects reference shaders, §5) |
| `AddModifier` | `objectName` = name; `propertyName` = modifier type; params | `RemoveModifier(name)` | created unbound; bound via the object's modifier slot |
| `RemoveModifier` | `objectName`; modifier snapshot | re-`AddModifier` + rebind | **reference-safe** (objects reference modifiers, §5) |

The underlying factories all exist on `IJob`: `RemovePainter` / `RemoveMaterial` /
`RemoveGeometry` / `RemoveObject` / `RemoveLight` / `RemoveModifier`
([Job.cpp:9135-9192](../../src/Library/Job.cpp)); the corresponding `Add*` factories
likewise. They are surfaced as ops here, not invented.

`SceneEdit` is **trivially-copyable** by design (snapshots embedded, not via
`shared_ptr` — see the `cameraSnapshot` comment at
[SceneEdit.h:312-315](../../src/Library/SceneEditor/SceneEdit.h)). Embedding a
full *object* or *material* snapshot inline would bloat the struct and break that
contract. **Resolution:** for the heavier entity snapshots, store an owned
descriptor blob behind a reference-counted handle in a *side table* keyed by edit
id (the history already owns the lifetime), keeping `SceneEdit` itself
trivially-copyable. The forward op carries only the name + a side-table key.

### 4.2 What a freshly-created chunk needs (sensible defaults)

A created entity must be immediately valid and render a recognizable result
(principle §1.4, approachable-by-default). Per family:

| Entity | Sensible default on create |
|---|---|
| **Camera** | clone of active camera (existing `CloneActiveCamera` path) — already correct |
| **Light** | `omni_light` at the viewport-orbit center, white, unit power; or `directional_light` pointing **toward** the surface from camera-ward `+Z` (the [SCENE_CONVENTIONS.md](../SCENE_CONVENTIONS.md) FROM-surface-TO-light trap — get this right at creation, not as a user surprise) |
| **Material** | neutral 0.5 Lambertian (`lambertian_material` with a `uniformcolor_painter 0.5 0.5 0.5`) — the roadmap's "neutral material" default scene posture (§5 A1). **Creating it composites two ops** (`AddPainter` for the `uniformcolor_painter` + `AddMaterial` referencing it) in one transaction — see §8.4 |
| **Painter** | colour pipe → `uniformcolor_painter 0.5 0.5 0.5`; scalar pipe → `scalar_painter` with a neutral magnitude. Pipe chosen at create time so the binding target (an `IPainter` colour slot vs an `IScalarPainter` physical-scalar slot) is unambiguous ([ISCALARPAINTER_REFACTOR.md](../ISCALARPAINTER_REFACTOR.md)) |
| **Object** | bound to the just-created/selected geometry + the neutral material; identity transform at the orbit center |
| **Medium** | `homogeneous_medium` with low scattering, zero absorption; created **unbound** (binding is a separate action, §8.3) |
| **Geometry** | only via import (§7, §10) or a small built-in primitive set (sphere/box/plane) so "Add Object" has something to reference |
| **Shader** | the scene's default shader op-list (RISE always has a `"none"` default, [Job::InitializeContainers](../../src/Library/Job.cpp)); created **unbound** |
| **Modifier** | identity/no-op modifier; created **unbound** |

A "neutral material" (which itself needs a neutral **painter**) and a built-in
primitive set are prerequisites for "Add Object" to be usable from nothing —
without them the only object you could add must reference a geometry the scene
already has, and the only material you could add must reference a painter the scene
already has.

### 4.3 Duplicate

Duplicate = snapshot the selected entity's introspected properties (the same
`Inspect` surface, §3-of-audit) → create a new entity of the same type → apply
the snapshot → dedup the name (§6). For objects, duplicate **shares** the
geometry/material refs (addref, not deep-copy) — cloning Sponza's wall material
1000× is not what the user means. For materials/media, duplicate deep-copies the
slot painter *bindings* (the painter objects themselves are shared by ref). This
mirrors `CloneActiveCamera`'s snapshot-then-recreate flow
([SceneEditController.cpp:3103](../../src/Library/SceneEditor/SceneEditController.cpp)),
generalized.

### 4.4 Per-platform exposure

Add/duplicate/delete buttons on the outliner toolbar + context menu. Wired into
the existing bridge pattern that already exposes `addCameraFromActive` (macOS
`RISEViewportBridge.mm`, Windows `ViewportBridge.cpp`, audit §2). New C-ABI
bridge entry points: `addEntity(category, type)`, `duplicateEntity`,
`deleteEntity(category, name)`. **Audit the bridge enum-translation getters when
adding any new category/op** — both bridges switch-on-int and silently fall
through to `None` ([MEMORY: bridge-enum-translation-audit]).

---

## 5. Reference-safe deletion

This is the sharpest correctness hazard, and the audit surfaced *why*:

> **Objects hold raw `const` pointers to their material/geometry/modifier/
> shader/interior-medium with manual `addref`/`release`
> ([Object.cpp](../../src/Library/Objects/Object.cpp), `AssignMaterial`
> `:103`, `AssignGeometry` `:113`, `AssignModifier` `:129`, `AssignShader`
> `:139`, `AssignInteriorMedium` `:175`) — they do NOT register through the
> manager's `RequestItemUse` callback path
> ([IManager.h:62-73](../../src/Library/Interfaces/IManager.h)).**

The manager *has* a deletion-callback mechanism (`RequestItemUse` registers an
`IDeletedCallback`; `RemoveItem` fires every registered callback before
releasing, [GenericManager.h](../../src/Library/Managers/GenericManager.h)
`RemoveItem` callback loop), **but objects don't use it.** And
`Job::RemoveMaterial` / `RemoveGeometry` / `RemoveMedium` just call
`pManager->RemoveItem` ([Job.cpp:9144-9185](../../src/Library/Job.cpp)) with **no
scan of the object set.** The object's `addref` on the material keeps the
material's refcount ≥ 1, so `RemoveItem`'s release does **not** destroy it — the
object's pointer does not dangle *to freed memory*, but the material is
**orphaned**: gone from the manager (and the file), still bound to the object,
unreachable by name, un-re-bindable. That is a silent, confusing state.

**Therefore the controller must compute the dependency graph itself** before any
delete — the managers won't tell it.

### 5.1 The dependency graph — descriptor/introspection-driven, not object-only

A second-review finding: the v1 edge table was **object-only** (every referrer was
an `Object`), so it could not see **painter→painter** (blend chains, the
`PBRMetallicRoughness` anisotropy graph, [ISCALARPAINTER_REFACTOR.md](../ISCALARPAINTER_REFACTOR.md)
`PainterToScalarAdapter`) or **material→painter / material→material** edges. With
an object-only graph, deleting a painter that another *painter* or *material*
references — but no object references — would report "0 references" and silently
orphan the dependent. Hand-coding one edge table per object type also rots the
moment a new referencing chunk is added.

**Resolution: a generic, descriptor/introspection-driven dependency graph — but
honestly scoped to the introspection that exists, with the rest marked TO-BUILD.**
The graph has two halves, and a third code-backed review (GUI_ROADMAP §13a #7: "the
all-family dependency graph assumes painter/geometry/shader/modifier introspection
that doesn't exist") found the v2 text over-claimed the second half as already
derivable. The corrected design separates *what the descriptors already give us*
from *the introspection that must be added*:

1. **Reference fields come from the descriptors (this half exists).** Every chunk
   parser declares its parameters via `Describe()`, and a reference parameter is
   `ValueKind::Reference` with a populated `referenceCategories`
   ([ChunkDescriptor.h:303](../../src/Library/Parsers/ChunkDescriptor.h) — "populated
   when kind == Reference; a param can accept references from multiple categories
   (e.g. any Painter or Function)"). The descriptor set is therefore the
   *authoritative, drift-proof* list of which families *can* reference which — the
   same descriptor-as-truth principle the parser already runs on (CLAUDE.md, chunk
   parsers are descriptor-driven). The graph builder reads `referenceCategories`
   per family rather than carrying a hand-maintained edge table. **This tells you
   the *shape* of the graph; it does not tell you the *live bound names* — for that
   you need introspection (half 2), and that is where the gap is.**
2. **Live edges come from introspection — which today covers only SEVEN families,
   so the painter / geometry / shader / modifier surfaces are TO-BUILD.** Reading
   an entity's *actual* bound reference names needs an `*Introspection::Inspect`
   surface for that entity's family. Those surfaces exist for **Camera, Light,
   Material, Media, Object, Film, Rasterizer** (the seven the audit enumerates,
   [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) §3/§8 — verified: the only
   `*Introspection.{h,cpp}` files in `src/Library/SceneEditor/` are
   `Camera/Film/Light/Material/Media/Object/Rasterizer`). `MaterialIntrospection`
   already yields a regular material's bound painter name + pipe
   (`MaterialIntrospection::MaterialSlotRef {None, Painter, ScalarPainter}`,
   [MaterialIntrospection.h:53](../../src/Library/SceneEditor/MaterialIntrospection.h)),
   so the **Material → Painter / ScalarPainter** edges of a non-composed material
   are derivable today. **But the following live-edge surfaces do NOT exist and must
   be built + tested before the graph can claim "all-family" coverage:**
   - **`PainterIntrospection` (TO-BUILD).** There is no painter introspection, so
     **painter → painter** edges (blend chains, the `PBRMetallicRoughness` anisotropy
     graph, the `PainterToScalarAdapter`, [ISCALARPAINTER_REFACTOR.md](../ISCALARPAINTER_REFACTOR.md))
     **cannot be derived today** — deleting a painter referenced only by another
     *painter* would report 0 references and silently orphan the dependent (the exact
     hazard §5.1 opens with). Add + test before relying on it.
   - **Composed / sugar-material slot-ref introspection (TO-BUILD).** Composed
     materials (`PBRMetallicRoughness`, `GGXEmissive`) are **read-only in
     `MaterialIntrospection` today** ([CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md)
     §3 — composed materials all-read-only), so their **material → painter** and
     **material → material** edges are not introspectable. Add a composed-material
     reference-readback (it need not be *editable* introspection — just enumerate the
     bound refs) before the graph covers them.
   - **`GeometryIntrospection` / `ShaderIntrospection` / `ModifierIntrospection`
     (TO-BUILD).** These three families have managers but no introspection surface
     (audit §3/§8). The **Object →** edges *into* geometry/shader/modifier are
     readable from the **referrer** side (`ObjectIntrospection` already exposes the
     object's bound geometry/shader/modifier slots), which is the direction
     reference-safe delete actually needs (you scan *referrers of* the target). So
     deleting a geometry/shader/modifier is coverable via `ObjectIntrospection`
     today **as long as objects are the only referrers** — but if any *new*
     referencing family is added whose introspection doesn't exist, its edges go
     unseen. Treat geometry/shader/modifier *as referents* of objects as covered;
     treat any geometry/shader/modifier that can itself *hold* a reference (none
     today, but assert it) as needing its own introspection.

   Walking each manager via `IManager::EnumerateItemNames`
   ([IManager.h:47](../../src/Library/Interfaces/IManager.h)) and reading each
   entity's introspected reference slots yields the **actual** edges **only for the
   families whose introspection exists** — which is why the painter / composed-material
   surfaces above are prerequisites, not free.

The shared query becomes
`ReferenceGraph::FindReferencesTo(Category, name) → list of (Category, referrerName, edgeKind, slotLabel)`,
backed by a new `ReferenceGraph.{h,cpp}` (a candidate for the five-build-project
add, §11 Packaging). Each edge below is tagged **EXISTING** (derivable from today's
descriptors + introspection) or **TO-BUILD** (needs the introspection surface named
above added + tested first); citations show where the binding lives in code:

| Referrer → Referent | Status | Source of the edge | Citation |
|---|---|---|---|
| Object → {Geometry, Material, Modifier, Shader, interior Medium} | **EXISTING** | `ObjectIntrospection` ref slots; descriptor `referenceCategories` | `Object::Assign*` [Object.cpp:103,113,129,139,175](../../src/Library/Objects/Object.cpp) |
| Material (non-composed) → Painter (colour slots) | **EXISTING** | `MaterialIntrospection` slot pipe = `Painter` | [MaterialIntrospection.h:53](../../src/Library/SceneEditor/MaterialIntrospection.h) |
| Material (non-composed) → ScalarPainter (IOR/roughness/scattering/…) | **EXISTING** | `MaterialIntrospection` slot pipe = `ScalarPainter` | [Job.cpp:2718](../../src/Library/Job.cpp) `ResolveOrDiagnoseScalar` |
| Material → Material (composed: `PBRMetallicRoughness`, `GGXEmissive`) | **TO-BUILD** (composed materials read-only today, no ref-readback) | composed-material reference introspection — *add* | audit §3 (composed materials all-read-only) |
| Composed material → Painter (slots inside a composed/sugar material) | **TO-BUILD** (same gap) | composed-material reference introspection — *add* | [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) §3 |
| **Painter → Painter** (blend chains / adapter graph) | **TO-BUILD** (no `PainterIntrospection`) | `PainterIntrospection` ref slots — *add + test*; descriptor `referenceCategories` | [ISCALARPAINTER_REFACTOR.md](../ISCALARPAINTER_REFACTOR.md) `PainterToScalarAdapter`, `BlendPainter` |
| Object (interior) → Medium | **EXISTING** | `ObjectIntrospection`; `Object::AssignInteriorMedium` addrefs | [Object.cpp:175](../../src/Library/Objects/Object.cpp) |
| Scene → active Camera | **EXISTING** | `Scene::activeCameraName` | [Scene.cpp:274](../../src/Library/Scene.cpp) (`RemoveCamera`) |
| LightSampler → all Lights | **EXISTING** | rebuilt from `scene.GetLights()` in `Prepare` | [LightSampler.cpp](../../src/Library/Lights/LightSampler.cpp) (`Prepare`) |

So the dependency graph ships in two stages: **stage 1 (EXISTING rows)** is the
object-referrer + non-composed-material-→-painter coverage derivable from today's
introspection — enough for reference-safe delete of geometry / material / medium /
shader / modifier / light / camera *where objects (and non-composed materials) are
the referrers*. **Stage 2 (TO-BUILD rows)** adds `PainterIntrospection` and
composed-material reference-readback so painter-→-painter and composed-material
edges stop silently orphaning. Until stage 2 lands, deleting a painter must be
**conservative**: if `PainterIntrospection` is absent, a painter delete that the
EXISTING (material-side) scan reports as unreferenced may still be referenced by
another painter — so V1 either (a) blocks painter delete behind a "painter
introspection not yet available — delete in text" notice, or (b) ships stage 2
first. Do **not** present the painter/composed-material coverage as drift-proof
today; it is drift-proof only *once those introspection surfaces exist*.

**Direction matters and the audit pinned it:** the referrer holds the referent
(the **Object holds the Medium**, the **Material holds the Painter**) — the
referent has no back-pointer. `AssignInteriorMedium` addrefs the medium
([Object.cpp:175](../../src/Library/Objects/Object.cpp)); `IMedium` /
`IPainter` have no back-reference. So "delete painter X" / "delete medium X" must
scan **all potential referrers** (every material + every painter for painter X;
every object for medium X) and introspection-compare — which is exactly why the
**painter-referrer** half needs `PainterIntrospection` (TO-BUILD): the medium scan
is covered by `ObjectIntrospection` today, but the painter-referrer scan is not.
The descriptors tell you the referrer *families* to scan; introspection tells you
their *live bound names*, and the scan is only as complete as the introspection
surfaces that exist.

The graph is computed **on demand at delete time** (and to render the outliner's
"N references" badge), not maintained incrementally — RISE scenes are small enough
(Sponza ≈ 155 objects, audit `Job::SetPrimaryAcceleration` note) that an
O(entities) introspection scan per delete is free, and an incremental index would
be one more thing to keep coherent against hand-edits. The same graph powers
**reference-safe deletion across all families** (§5.2) and the **tombstone refusal**
(§5.4): you may not tombstone an entity that still has live referrers any more than
you may hot-delete one.

### 5.2 Block-or-cascade policy

Per entity family, when the user deletes an entity that has referrers:

| Target | Default policy | Rationale |
|---|---|---|
| **Material** | **Block** if any object *or composed material* binds it; offer **Reassign** (rebind referrers to a chosen/neutral material) then delete | a material with no referrer is harmless but deleting one *in use* silently orphans — make the user choose |
| **Painter** | **Block** if any material *or other painter* binds it; offer **Reassign** to a chosen/neutral painter (pipe-matched) then delete | the new family the object-only graph missed (§5.1) — a painter referenced only by another painter/material must still block |
| **Geometry** | **Block** if any object binds it | an object with a freed/cleared geometry can't render; reassign or delete the objects first |
| **Medium** | **Cascade-clear** (call `ClearInteriorMedium` on each referrer, [Object.cpp:185](../../src/Library/Objects/Object.cpp)) then delete | clearing an interior medium is a benign, well-defined no-spatial-impact op (`SetObjectInteriorMedium` with empty value, [SceneEdit.h:140-156](../../src/Library/SceneEditor/SceneEdit.h)) |
| **Shader / Modifier** | **Cascade to default** (rebind to the scene's default shader/none-modifier) then delete | RISE always has a `"none"` default shader/modifier ([Job::InitializeContainers](../../src/Library/Job.cpp)) |
| **Object** | **Delete freely** — no entity references *an object*; its outgoing refs are released by the object's dtor (`safe_release` on all six pointers, [Object.cpp](../../src/Library/Objects/Object.cpp)) | objects are graph *leaves* (referrers, never referents) except light-sampler mesh-light participation (§8.2) |
| **Camera** | **Delete freely**; if it was active, `Scene::RemoveCamera` auto-promotes the lexicographically-first remaining camera ([Scene.cpp:295-326](../../src/Library/Scene.cpp)) — the one dangling case already handled in code | the renderer's null-camera guard covers an emptied manager |
| **Light** | **Delete freely**, then **recompute the light sampler** (§8.2) | nothing references a light by name; the sampler is rebuilt wholesale |

The whole delete (cascade rebinds + the removal + — when it must survive reload —
the tombstone emit, §5.4) is **one composite `SceneTransaction`**
([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §4.1; the underlying
`CompositeBegin`/`CompositeEnd` bracket, [SceneEdit.h:248-251](../../src/Library/SceneEditor/SceneEdit.h))
committed atomically under the single-writer `mMutex`, so one Undo restores the
entity *and* re-binds every referrer (TRANSACTION_MODEL §4.2 partial-failure
unwind: any inner `Apply` failure closes the composite and `Undo`s it as one unit).
The controller surfaces the referrer list to the UI so the dialog can say "Delete
'bronze'? It is used by 3 objects — [Reassign to…] [Cancel]". The delete is
attributed to its origin (`InApp` / `Agent` / owner-approved `ExternalMCP`,
TRANSACTION_MODEL §8) like any transaction.

### 5.3 Why not just use `RequestItemUse`?

It is tempting to retrofit objects onto the manager callback path so deletion
auto-cascades. **Deliberately not** in this phase: (a) it changes the
object-construction ABI across every object/material/medium binding site (a
large, separate refactor), and (b) the callback fires *during* `RemoveItem` on
whatever thread calls it — the controller already serializes deletes under
cancel-and-park, so an explicit pre-scan is simpler to reason about and to
present in a confirm dialog than an in-the-middle-of-removal callback storm. The
on-demand graph (§5.1) gives the same safety without the ABI churn.

### 5.4 Deletion persistence — the managed tombstone

A second-review finding, and a **contradiction with CAMERAS_AND_VIEWS B4a**: this
spec described deletion but never said how a delete *persists*. The hot-delete in
§5.2 only mutates the in-memory managers. For a **session-created** entity that is
fine — Phase C re-renders the managed block wholesale (§7), so an entity created
this session that no longer exists is simply not emitted (it never had a source
chunk to leave behind; cf. the camera path's "created then undone → nothing to
emit", [SaveEngine.cpp:1338-1402](../../src/Library/SceneEditor/SaveEngine.cpp)).
But a **file-authored** entity has a `standard_object` / material / painter /
light chunk *in the source bytes*. Round-trip save preserves those bytes
verbatim (Mode A/B never delete a source chunk) — so on reload the parser
re-creates the deleted entity, and **it comes back**. Deletion of file-authored
entities was undesigned and would silently not stick.

**Design: a managed tombstone.** A delete that must survive reload emits an
ordered destructive command into the **same managed override block** the save
engine already owns:

```
# >>> RISE MANAGED OVERRIDES — do not edit; regenerated on save <<<
...
> remove material bronze
> remove painter bronze_albedo
> remove object sphere_7
# >>> END RISE MANAGED OVERRIDES <<<
```

This reuses the **existing** managed-override-block machinery, not a new
construct:

- **`> remove` is a real parser command.**
  `> remove {painter|material|geometry|object|light|modifier}` `<name>` dispatches
  to `Job::Remove*`
  ([AsciiCommandParser.cpp:524-565](../../src/Library/Parsers/AsciiCommandParser.cpp)
  `ParseRemove`; [Job.cpp:9135-9192](../../src/Library/Job.cpp)). The tombstone is
  just that command, authored by the save engine instead of by hand. No
  scene-format addition; older scenes parse unchanged.
- **Family coverage of `> remove` is exactly six — and it is the binding
  constraint.** `ParseRemove` accepts only
  `painter | material | geometry | object | light | modifier`
  ([AsciiCommandParser.cpp:534-565](../../src/Library/Parsers/AsciiCommandParser.cpp)).
  There is **no** `> remove medium`, `> remove shader`, or `> remove camera`
  sub-command today (even though `Job::RemoveCamera`
  [Job.cpp:724](../../src/Library/Job.cpp) and the media/shader managers exist). So
  for V1 the **tombstonable families are those six**; deleting a *file-authored*
  **medium / shader / camera** has no tombstone command and is therefore a
  **refusal-to-persist** (the in-memory delete stands for the session; the entity
  returns on reload — surfaced loudly, never silent). Two clean follow-ups (each
  one line in `ParseRemove` + the existing `Job::RemoveCamera` / a new
  `Job::RemoveMedium` / `Job::RemoveShader`): add `> remove camera`, `> remove
  medium`, `> remove shader`. Until then, *session-created* media/shaders/cameras
  still delete-persist fine (Phase C just doesn't re-emit them — no tombstone
  needed); only *file-authored* ones in these three families hit the refusal.
- **The block is the same sentinel-bracketed, wholesale-rendered region** the
  round-trip save plan defines (`kManagedBlockSentinelOpen`/`Close`,
  [OverrideSpanIndex.h:43-45](../../src/Library/SceneEditor/OverrideSpanIndex.h);
  managed-block contract pinned 2.3 / 2.7, [ROUND_TRIP_SAVE_PLAN.md](../ROUND_TRIP_SAVE_PLAN.md)).
  Tombstones live in it alongside `override_object` chunks and created-entity
  chunks; the block is regenerated in canonical form on every save (pinned 2.7).
  This *mirrors the existing managed-override-block pattern* — the tombstone is a
  new *line kind* in a block whose lifecycle (scan, erase, re-render, atomic write)
  already exists.
- **Persistence ride-along.** `EntityCategory` (the dirty-tracker key,
  [DirtyTracker.h:46](../../src/Library/SceneEditor/DirtyTracker.h)) gains a
  per-family **tombstone set** alongside the created-entity set; Phase C emits one
  `> remove {family} <name>` line per live tombstone, sorted for deterministic
  layout, exactly as it emits created chunks (§7).

**Ordering — the tombstone must come AFTER the chunk it removes.** `> remove` is a
runtime mutator: it can only remove a name the manager already contains, so it must
appear *after* that name's declaration in scene-file order (the same
"target-exists-at-parse-time" invariant `override_object` already relies on,
[ROUND_TRIP_SAVE_PLAN.md](../ROUND_TRIP_SAVE_PLAN.md) §9.6). The managed block's
placement rule (pinned 2.19 — *after* all managed targets' declarations and
*before* the first barrier `>` command) already satisfies this: the deleted
entity's `standard_object` (or material/painter/…) chunk is upstream of the block,
and `> remove` fires once the block is reached. The two destructive-command
refusals the save plan already defines now read as **consistency conditions on our
own tombstones** rather than only on hand-written commands:

- A `> clearall` after any managed target's declaration is still a **refusal**
  (pinned 2.24; the destructive-erase guard already lives at
  [SaveEngine.cpp:1424](../../src/Library/SceneEditor/SaveEngine.cpp)) — it would
  erase our created entities/overrides before they apply.
- A hand-written `> remove` of a name we *also* manage-override is a refusal for
  the same reason. Our *own* tombstone of a name we do **not** override is the
  benign, intended case.

This closes the contradiction CAMERAS_AND_VIEWS B4a names: that spec already defers
camera-delete persistence to "the managed tombstone [ENTITY_CREATION.md] §5/§7.5
designs" and notes the save engine *refuses destructive erasure today*
([SaveEngine.cpp:1424](../../src/Library/SceneEditor/SaveEngine.cpp)) — this §5.4 is
that design, generalized to all six tombstonable families (deleting a file-authored
camera persists the same way, modulo the `> remove camera` sub-command gap above).

**Refusal — when a target can't be safely tombstoned.** A tombstone is only
correct when the `> remove` is guaranteed to run, against an existing target,
without erasing managed state. The save engine **refuses the delete-persist**
(returns `Status::Refused`, original file untouched, the in-memory delete stands
only for the session) when:

0. **The family has no `> remove` sub-command** — a file-authored medium / shader /
   camera (see the six-family constraint above). The interactive delete still works
   for the session; persistence refuses with "delete this in text, or delete the
   chunk" until the corresponding `> remove` sub-command lands.
1. **The target is inside a `FOR` loop.** A `FOR`-generated entity has no unique
   removable identity that a single `> remove sphere_7` can address without the
   loop re-creating it on the next reload's loop expansion — `> remove` runs once,
   the loop body runs N times. (RISE's parser visits a `FOR` body's bytes once per
   iteration via `seekg`-back, [ROUND_TRIP_SAVE_PLAN.md](../ROUND_TRIP_SAVE_PLAN.md)
   §4.2; the entity is `chunkRevisited`, pinned 2.12.) V1 cannot statically prove
   the loop won't regenerate the name, so it refuses — the same conservatism the
   save plan applies to `FOR`-shared lines (Mode B) and cross-file targets (pinned
   2.25). The UI surfaces "deleting a FOR-generated entity won't persist; edit the
   FOR block in text."
2. **The target's declaration is in a child file** (`> load` / `> run`,
   `GetCreationFilePath(name) != filePath`, pinned 2.25) — the top-level file's
   tombstone can't be ordered relative to a chunk it doesn't contain.
3. **No valid block placement exists** (a barrier `>` command sits before any legal
   block offset, pinned 2.19) — the block, and therefore the tombstone, has nowhere
   to live.
4. **The target still has live referrers** (§5.1) — block/cascade first (§5.2);
   tombstoning a still-referenced entity is the same orphan hazard as hot-deleting
   one.

Refusal is **loud, never silent** (the existing refusal philosophy, audit §1) — a
deleted file-authored entity that can't be tombstoned tells the user to edit the
text, rather than reappearing mysteriously on reload.

**Each tombstone is a transaction.** The delete is the same composite
`SceneTransaction` as §5.2 (cascade rebinds + manager `RemoveItem` + recording the
tombstone in the dirty-tracker set), committed under `mMutex`, attributed, and
**undoable**: Undo pops the tombstone from the set (so the next save no longer
emits the `> remove`) and re-adds the entity from its captured snapshot (the
`Remove*` op's undo route, §4.1). One Undo restores both the live entity and its
persistence.

---

## 6. Naming

### 6.1 Generated names

Every manager rejects duplicate names (`GenericManager::AddItem` refuses an
existing name, [GenericManager.h](../../src/Library/Managers/GenericManager.h)),
so creation must produce a unique name. Reuse the **exact dedup algorithm**
`CloneActiveCamera` already uses — `UniqueCameraName`
([SceneEditController.cpp:~3085](../../src/Library/SceneEditor/SceneEditController.cpp)):
take the proposed base, append `_2`, `_3`, … until the manager has no such item,
with a timestamp fallback. Generalize it to `UniqueName(IManager&, base)`. Stable
generated bases per family: `light_1`, `material_1`, `painter_1`, `object_1`,
`medium_1`, `geometry_1`, `shader_1`, `modifier_1` (lowest free index, so names
stay short and predictable). For painters the dedup must check **both** painter
managers (`IPainterManager` + `IScalarPainterManager`) so a colour painter and a
scalar painter can't collide on a name.

The dedup **must run under the same lock** that does the add (so two concurrent
creates can't pick the same suffix — the comment at
[SceneEditController.cpp:3122](../../src/Library/SceneEditor/SceneEditController.cpp)
makes this point for cameras).

### 6.2 Rename

Rename is a new op `RenameEntity(category, oldName, newName)`. Implementation
hazard: `GenericManager` is keyed by name with **no rename primitive** — rename =
remove-under-old + add-under-new on the *same* item pointer (addref it across the
gap so it isn't destroyed). Crucially, **rename must rewrite every referrer's
binding**: an object that bound material "bronze" stores the *pointer*, not the
name (Object.cpp), so the pointer is fine — but the **scene-file re-emit** keys
on the name, and any *other* name-keyed state (the active-camera name in
`Scene::activeCameraName`, the dirty-tracker entity keys keyed by
`(EntityCategory, name)` in [DirtyTracker.h:46](../../src/Library/SceneEditor/DirtyTracker.h),
the source-span index) must be updated. Rename is therefore a **composite**
transaction touching the manager key + active-name pointers + dirty/span
bookkeeping.

### 6.3 Round-trip impact of rename

A rename of a **parse-time entity** can't be done as a Mode-A in-place splice in
general — the name token appears at the chunk's declaration *and* at every
reference site (`material bronze` lines on objects). Renaming would need to
rewrite all of them. **Policy for V1:** a renamed *created* (engine-owned) entity
re-emits cleanly under its new name (Phase C renders the whole chunk wholesale,
§7). A rename of a *file-authored* entity is **Refused** by the save engine in V1
(consistent with its existing refusal of edits it can't splice safely —
property-edit-with-no-editable-chunk is already `Refused` at
[SaveEngine.cpp:1164](../../src/Library/SceneEditor/SaveEngine.cpp)). Surface that
limitation in the rename UI ("rename persists for entities you created this
session; renaming a scene-file entity is in-memory only until a future save
mode"). This keeps rename safe and honest rather than silently clobbering
references.

---

## 7. Extending Phase C persistence beyond cameras

Today `SaveEngine` Phase C emits a fresh chunk only for created cameras
([SaveEngine.cpp:1338-1402](../../src/Library/SceneEditor/SaveEngine.cpp)); the
non-camera case is the hard **Refused** at `:1373`. The **camera path is the
template** to generalize.

### 7.1 The camera template, decomposed

`RenderCreatedCameraChunk`
([SaveEngine.cpp:544](../../src/Library/SceneEditor/SaveEngine.cpp)) does exactly
three things, all of which generalize:

1. **Resolve the chunk keyword** from the entity:
   `CameraIntrospection::GetDescriptorKeyword(cam)`
   ([CameraIntrospection.h:120](../../src/Library/SceneEditor/CameraIntrospection.h)).
2. **Introspect editable rows**: `CameraIntrospection::Inspect(cam, …)` →
   `std::vector<CameraProperty>`.
3. **Emit `keyword { name … editable rows … }`**, skipping read-only/synthetic
   rows and the duplicate `name` row.

The Phase C driver loop ([SaveEngine.cpp:1338-1402](../../src/Library/SceneEditor/SaveEngine.cpp))
unions session-created entities (`mDirty.SessionCreatedSnapshot()`,
[DirtyTracker.h](../../src/Library/SceneEditor/DirtyTracker.h)) with
previously-emitted in-block entities, dedups, sorts, and re-renders each into the
managed `override_object` block on every save.

### 7.2 What each family's re-emit needs

The `Inspect` API is **already uniform** across families — `LightIntrospection`,
`MaterialIntrospection`, `MediaIntrospection`, `ObjectIntrospection` all expose a
`static std::vector<CameraProperty> Inspect(...)`
([LightIntrospection.h:40](../../src/Library/SceneEditor/LightIntrospection.h),
[MaterialIntrospection.h:72](../../src/Library/SceneEditor/MaterialIntrospection.h),
[MediaIntrospection.h:55](../../src/Library/SceneEditor/MediaIntrospection.h),
[ObjectIntrospection.h:50](../../src/Library/SceneEditor/ObjectIntrospection.h)).
So the property-row loop transfers directly. **The missing piece is a
`GetDescriptorKeyword` per family** — it exists only on `CameraIntrospection`
today. Add `GetDescriptorKeyword(const ILightPriv&)`, `(const IMaterial&)`,
`(const IPainter&)` / `(const IScalarPainter&)`, `(const IMedium&)`,
`(const IObjectPriv&)`, `(const IShader&)`, `(const IRayIntersectionModifier&)` to
the respective introspection surfaces, then write
`RenderCreated{Light,Painter,Material,Medium,Object,Shader,Modifier}Chunk` helpers
mirroring `RenderCreatedCameraChunk`.

Order of difficulty (do them in this order):

| Family | Difficulty | Why |
|---|---|---|
| **Light** | Easy | Self-contained chunk; all params are scalars/vec3/painter-names already in `Inspect`. One color caveat: emit `color` as **sRGB** via `LightColorPelToSRGB` ([SaveEngine.cpp:~595](../../src/Library/SceneEditor/SaveEngine.cpp)) — the engine stores linear RISEPel; the parser re-linearizes on load. The camera path already proves the round-trip. |
| **Painter** | Easy–Medium | Self-contained leaf for a `uniformcolor_painter` / `scalar_painter` (the §4.2 defaults); a *blend* painter references **other painters** (§5.1 painter→painter), so it must be topo-ordered after its inputs (§7.3). Colour vs scalar pipe selects the chunk keyword and which manager owns it ([ISCALARPAINTER_REFACTOR.md](../ISCALARPAINTER_REFACTOR.md)). **The prerequisite for re-emitting a created material** (a created material that binds a created painter is un-round-trippable until the painter is). |
| **Material** | Medium | Slots are **painter references by name**. Re-emit must (a) emit the material chunk referencing painter names, and (b) ensure the referenced painters themselves exist in the file. If a created material binds a *created* painter, that painter chunk must be emitted **before** the material chunk in the block (declaration-order dependency, §7.3). **Composed** materials (PBRMetallicRoughness, GGXEmissive) are read-only/un-introspectable for slot edits (audit §3) — refuse to re-emit a *created* composed material in V1, or emit it via its own authoring-sugar chunk if `GetDescriptorKeyword` can name it. |
| **Medium** | Medium | Homogeneous only (Heterogeneous's majorant grid is baked at construction and not re-emittable, audit §3 — refuse created Heterogeneous). Coeffs are scalars; straightforward. |
| **Shader / Modifier** | Medium | Self-contained chunks; a shader is an op-list. Created unbound, so no inbound dependency at emit time; the object that binds them is topo-ordered after (§7.3). |
| **Object** | **Hardest** | An object chunk **references a geometry name and a material name** (`Job::AddObject` resolves both, [Job.cpp:5197,5211](../../src/Library/Job.cpp)), and optionally a shader/modifier/interior-medium. Re-emit must guarantee every referent is present in the file: an imported geometry (§7.4) and any created material/painter/shader/modifier must be emitted (or already exist) before the `standard_object` chunk. Transforms already round-trip via Mode A/B; the *new* part is the geometry/material/shader/modifier binding + the geometry chunk itself. |

### 7.3 Intra-block declaration order

The managed block is rendered wholesale, so the re-emit must **topologically
order** created entities by their reference edges — and the order must come from
the **same descriptor/introspection-driven graph** that powers reference-safe
deletion (§5.1), not a second hand-maintained ordering. A correct topological
order respecting every edge in §5.1 is: painters (inputs before blends) → geometry
→ shaders → modifiers → materials → media → objects → cameras. The scene parser
resolves names at chunk-finalize time
([AsciiSceneParser.cpp](../../src/Library/Parsers/AsciiSceneParser.cpp), descriptor
dispatch), so a forward reference to a not-yet-declared name fails to parse. The
Phase C driver loop (currently name-sorted,
[SaveEngine.cpp:~1356](../../src/Library/SceneEditor/SaveEngine.cpp)) must switch
from lexicographic sort to a **topological sort over the §5.1 graph** for
non-camera entities (a cycle is impossible — RISE references are acyclic by
construction; a defensive cycle check refuses rather than emitting an unparseable
block). Cameras have no outgoing refs so their existing sort is unaffected.

### 7.4 Geometry / imported assets in the block

A created object usually references an **imported geometry**. Re-emitting a
`gltfmesh_geometry` / `gltf_import` chunk is feasible (the chunk descriptor is
known) — emit it pointing at the **same asset path** the import used. The asset
file itself is *not* embedded; the `.RISEscene` references it by path (§7.5).
This is the one created-entity whose persistence depends on an external file
staying put — document it as a caveat (the scene is portable only with its asset
sidecars, exactly as hand-authored glTF scenes already are).

### 7.5 Save-engine refusal updates

Once a family is re-emittable, **relax** the `:1373` refusal for that family (the
`if( kv.first.first != EntityCategory::Camera )` guard,
[SaveEngine.cpp:1369-1383](../../src/Library/SceneEditor/SaveEngine.cpp), becomes a
per-family-capability check). Until a family is implemented it stays Refused with a
clear message — never silently dropped (the existing refusal philosophy, audit §1
"What is refused / deferred"). `EntityCategory`
([DirtyTracker.h:46](../../src/Library/SceneEditor/DirtyTracker.h)) today enumerates
`Object, Camera, Light, Material, Medium` — it must gain **`Painter`, `Geometry`,
`Shader`, `Modifier`** to dirty-track and Phase-C-emit the families this spec adds
(these are the same families the `Category` enum gains, §3.1; the dirty-tracker and
outliner enums must stay aligned). All additions are backward-compatible: older
scenes parse unchanged, and the managed block only ever grows entities/tombstones
the engine can emit.

### 7.6 Tombstone emit pass (delete persistence)

Alongside the created-entity pass (§7.1), Phase C gains a **tombstone pass** that
emits the managed `> remove {family} <name>` lines (§5.4). It reads a per-family
**tombstone set** the dirty tracker carries next to `SessionCreatedSnapshot()`
([DirtyTracker.h](../../src/Library/SceneEditor/DirtyTracker.h)), sorts for
deterministic layout, and emits one `> remove` line per live tombstone into the
managed block — after the created/override chunks (a `> remove` is a command, not a
chunk, and removing a name we *also* created in the same block would be incoherent,
so the engine asserts a tombstoned name is not simultaneously a created/overridden
name). Two refinements to the existing block machinery:

- **The `:1373` non-camera-in-block refusal must not fire on our own tombstones.**
  That refusal exists because a hand-pasted non-camera *chunk* can't be
  re-rendered (audit §2). A `> remove` line is **not** a chunk and is engine-authored,
  so the block scanner classifies managed `> remove` lines as a known managed line
  kind (like `override_object`), not as an unknown chunk. A hand-pasted unknown
  chunk inside the sentinels stays Refused.
- **Refusal conditions (§5.4) gate the emit**, evaluated against the source bytes
  (`FOR`-membership, cross-file `GetCreationFilePath`, placement feasibility,
  live referrers). A tombstone that fails any gate returns `Status::Refused` with
  the original file untouched — the in-memory delete persists only for the session.

---

## 8. Dependency insertion (creating something that needs a ref)

Creation often requires a *referent* to exist first. The controller orchestrates
the dependency, as a **composite transaction**.

### 8.1 Object needing material + geometry

`Job::AddObject` **resolves geometry and material by name and refuses if
geometry is missing** ([Job.cpp:5197](../../src/Library/Job.cpp), error; material
missing is a warning at `:5211`). So "Add Object" must, in one composite:

1. ensure a geometry exists (import one, or pick an existing one, or create a
   built-in primitive — §4.2),
2. ensure a material exists (default neutral material if none chosen),
3. `AddObject(name, geom, material, …)`,
4. **invalidate + rebuild the TLAS** (§8.5).

The UI flow: "Add Object" first prompts for geometry (import or pick), defaults
the material to neutral, then creates. This is why a **built-in primitive set +
neutral material** are §4.2 prerequisites — otherwise the first object in an
empty scene has nothing to bind.

### 8.2 Light needing the manager + the sampler

Adding a light is two steps the audit makes explicit:

1. `pLightManager->AddItem` (via `Job::AddPointOmniLight` etc.,
   [Job.cpp](../../src/Library/Job.cpp) light factories) — registers the light.
2. **The light sampler must be rebuilt.** `LightSampler::Prepare`
   ([LightSampler.cpp](../../src/Library/Lights/LightSampler.cpp)) builds its
   alias table by enumerating `scene.GetLights()` and is called from
   `RayCaster::AttachScene` — it is **not** auto-rebuilt on a hot add/remove. A
   light added after `Prepare` is **not sampled** until the table is
   recomputed. So `AddLight`/`RemoveLight` must trigger a sampler rebuild on the
   next render pass (the controller already re-prepares on render after
   structural edits — extend that to cover the light set). This is the
   light-side analogue of the TLAS rebuild, and missing it is the classic
   "added light does nothing" bug. (Cross-reference the
   [bdpt-vcm-mis-balance](../skills/bdpt-vcm-mis-balance.md) skill's step-0:
   light-set/sampler-density bugs masquerade as MIS bugs.)

### 8.3 Medium needing an object to bind to

A created medium is inert until bound as some object's interior medium. Creation
and binding are **separate** actions (create the medium, then in the object's
property panel set `interior_medium`). Binding is the existing
`SceneEdit::SetObjectInteriorMedium` op
([SceneEdit.h:140-156](../../src/Library/SceneEditor/SceneEdit.h)) — a benign
pointer swap, **no spatial invalidation** (its comment says so explicitly). The
outliner can offer "Create medium and bind to selected object" as a one-click
composite that chains `AddMedium` + `SetObjectInteriorMedium`.

### 8.4 Material needing a painter

The dependency the object-only mindset missed: **a material cannot be created
without a painter.** Every material slot is a painter reference by name
(`MaterialIntrospection` slot pipe `{Painter, ScalarPainter}`,
[MaterialIntrospection.h:53](../../src/Library/SceneEditor/MaterialIntrospection.h)),
and `Job::Add*Material` resolves those names — a colour slot via
`IPainterManager`, a physical-scalar slot via `Job::ResolveOrDiagnoseScalar` /
`IScalarPainterManager` ([Job.cpp:2718](../../src/Library/Job.cpp)). So "Add
Material" (the neutral default, §4.2) is **two ops in one composite transaction**:

1. `AddPainter` — create the `uniformcolor_painter 0.5 0.5 0.5` (colour pipe);
2. `AddMaterial` — create the `lambertian_material` referencing that painter name;

committed atomically so Undo removes both, and so the material is never momentarily
present with a dangling painter reference. This is the material-side analogue of
§8.1's object-needs-geometry+material composite, and the reason **Painter is a
first-class create family** (§3.1, §4.1) rather than an implementation detail of
"Add Material": a painter is independently creatable, deletable (reference-safely,
§5.1), and persistable (§7.2).

### 8.5 Object add/delete → TLAS

Adding or removing an object changes the world-space set the top-level BVH was
built from. `SceneEditor` already runs a spatial-invariant chain for
bbox-changing edits — `RunObjectInvariantChain` calls
`IObjectManager::InvalidateSpatialStructure`
([SceneEditor.cpp:~765](../../src/Library/SceneEditor/SceneEditor.cpp)), gated by
`SceneEdit::OpNeedsSpatialRebuild` ([SceneEdit.h:~356](../../src/Library/SceneEditor/SceneEdit.h)),
and the controller calls `PrepareForRendering`
([SceneEditController.cpp:~1681,2345](../../src/Library/SceneEditor/SceneEditController.cpp))
before the next render. **`OpNeedsSpatialRebuild` must be extended to return true
for `AddObject` and `RemoveObject`** (today it covers transforms +
`SetObjectGeometry`). Material/light/medium/camera creations do **not** need a
TLAS rebuild.

---

## 9. Shared-C++ vs platform split

Per principle §1.2 and the [CROSS_PLATFORM_ARCHITECTURE.md](CROSS_PLATFORM_ARCHITECTURE.md)
rule.

| Concern | Shared C++ (`src/Library/`) | Platform (thin shell) |
|---|---|---|
| Outliner tree **model** (all-family categories, entities, active badges, preview-isolation state) | all of it — `SceneEditController` accessors (extend `CategoryEntityCount/Name` + the four new `Category` values, §3.1) | tree **widget** rendering, expand/collapse, icons |
| Create/duplicate/delete **ops** (all families) | all — new `SceneEdit::Op` + controller entry points + dedup naming, committed as `SceneTransaction` (one-shot `Propose`/builder) | toolbar buttons / context menu / FAB |
| **Dependency graph** (descriptor/introspection-driven) + block-or-cascade decision | all — `ReferenceGraph::FindReferencesTo` (§5.1) + cascade composites | the confirm/reassign **dialog** |
| **Deletion persistence** (managed tombstones) | all — `SaveEngine` `> remove` emit + dirty-tracker tombstone set (§5.4, §7.6) | — |
| **Phase-C save** (per-family chunk re-emit, topo order over the §5.1 graph) | all — `SaveEngine` + `*Introspection::GetDescriptorKeyword` | — |
| Reference counting / lifetime | all — `IReference`/`GenericManager` is the substrate | — |
| **Import** orchestration (as one atomic transaction, §10) | all — `Job::ImportGLTFScene` ([GLTF_IMPORT.md](../GLTF_IMPORT.md)) wrapped in an import transaction | native **file dialog** + path hand-off |
| Preview isolation (owner-private, §3.3) | controller skip-list (never published/snapshotted) | eye-icon widget |

The pay-off: macOS/Windows/Android get creation, deletion, and persistence from
one C++ implementation; only the tree widget, the dialogs, and the file picker
are per-platform (Android at Tier B/C per §3.4, §10).

---

## 10. Geometry / model + texture import

Wire the **already-implemented glTF importer** ([GLTF_IMPORT.md](../GLTF_IMPORT.md))
into an outliner **"Add Model…"** action. The importer is real and shipped
(uncommitted-branch caveat aside): `Job::ImportGLTFScene`
([Job.cpp:4635](../../src/Library/Job.cpp)) / `RISE_API_ImportGLTFScene` walk the
scene tree and emit per-primitive geometries,
`pbr_metallic_roughness_material` materials, `KHR_lights_punctual` lights, and
cameras ([GLTF_IMPORT.md](../GLTF_IMPORT.md) Implementation-status table;
`GLTFSceneImporter`). Single-mesh import flows through
`Job::AddGLTFTriangleMeshGeometry` → `GLTFSceneImporter::ImportPrimitive`
([Job.cpp:4688](../../src/Library/Job.cpp)).

### 10.0 Import is a first-class transaction — it currently bypasses the mutation path

A second-review finding, and the sharpest hazard in this section.
`Job::ImportGLTFScene` calls `GLTFSceneImporter::ImportScene( *this, opts )`
([Job.cpp:4635-4680](../../src/Library/Job.cpp)), which creates **many** entities
(geometries, materials, lights, cameras) by calling `Job::Add*` factories directly.
It produces **no `SceneEdit` ops, no transaction, no atomic rollback, no undo
snapshot, no attribution, and no partial-failure handling** — it is a bulk
side-effecting mutation that **bypasses the one mutation path** (GUI_ROADMAP §1.6;
TRANSACTION_MODEL §1 item 2, "`SceneEditor::Apply` is the only sanctioned
mutator"). Consequences if wired into the GUI as-is: an import is not undoable
(Undo can't reverse 200 entities it never recorded); a mid-import failure (a
corrupt primitive, an out-of-memory texture decode) leaves a **half-imported
scene** with no rollback; and the AI/diff tooling sees a pile of entities appear
with no attribution and no reviewable transaction.

**Redesign: a first-class import transaction / provenance object.** The import
becomes a single `SceneTransaction` ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md)
§4.1) that **owns the complete generated set** and commits as **one unit**:

- **Provenance object.** An `ImportProvenance` value (asset path, `name_prefix`,
  options, and the list of *every* generated entity name keyed by family) is
  produced by the importer in a **collect-only / dry pass** — it enumerates what it
  *would* create without committing to the live managers. (Concretely: run
  `GLTFSceneImporter` against a *staging* `Job`/collector, or extend the importer
  with a "plan" mode that returns the entity set instead of calling `Job::Add*`
  in place.) This is the import analogue of the AI's *proposed transaction*
  (TRANSACTION_MODEL §7) — nothing touches the live document until commit.
- **One-shot whole-payload commit.** The planned set is flattened into one
  `SceneTransaction`'s `std::vector<SceneEdit>` (an `AddGeometry` / `AddMaterial` /
  `AddPainter` / `AddLight` / `AddCamera` per generated entity, topo-ordered per
  §7.3) and submitted via the one-shot `Propose` / `Commit` payload
  (TRANSACTION_MODEL §12 — the whole-payload path exists precisely so a large edit
  set commits atomically without per-edit round-trips). The commit applies under
  `mMutex` as a single composite; one bump, one published snapshot, one attribution
  record, **one undo entry** ("Undo Import model.glb").
- **Rollback (all-or-nothing).** Reuse the existing composite partial-failure
  unwind (TRANSACTION_MODEL §4.2 / §11.2): open the composite, and on any inner
  `Apply` failure close it and `Undo` once — `EditHistory` collapses the composite
  to a single unit ([EditHistory.cpp:158-187](../../src/Library/SceneEditor/EditHistory.cpp)),
  so a failed import leaves the scene **exactly as it was before**. No half-import.
- **Partial-failure policy (explicit).** The collect pass classifies each planned
  entity as *importable* or *skipped-with-reason* (animation/skinning/morph are
  already warn-and-skip, §10.4; an unreadable primitive or texture is a per-entity
  skip). The policy is **all-importable-or-nothing for the committed transaction**,
  with the skip list surfaced to the user *before* commit (the import dialog shows
  "187 objects, 3 lights, 1 camera; skipping 2 morph-target meshes"). A *hard*
  failure (the file won't parse — `importer.IsValid()` is false,
  [Job.cpp:4656](../../src/Library/Job.cpp)) aborts with nothing committed; a
  *soft* skip (a feature RISE doesn't support) proceeds with the importable set and
  the documented skips. There is never a silently truncated import.
- **TLAS + sampler once.** Because the whole set commits as one transaction, the
  object-add TLAS rebuild (§8.5) and the light-sampler rebuild (§8.2) happen **once
  after the commit**, not per generated entity.

The legacy direct `Job::ImportGLTFScene` stays as the **headless / scene-file**
path (the `gltf_import` chunk parser calls it at parse time, where there is no
controller, no undo, and the whole file is the transaction). The *interactive*
"Add Model…" action routes through the import transaction instead. The single-mesh
`Add Mesh` surface (§10.1) is the degenerate case: a one-entity import transaction
(`AddGeometry`) followed by the §8.1 object-create composite.

### 10.1 The two import surfaces

- **Add Model (bulk)** → `gltf_import`: instantiates many named objects +
  materials + lights + cameras from one file, prefixed by `name_prefix`
  ([GLTF_IMPORT.md](../GLTF_IMPORT.md) §7 surface; `on_name_collision suffix`
  maps to our dedup, §6). This is the "drop a glTF into the scene" flow.
- **Add Mesh (single)** → `gltfmesh_geometry`: one geometry the user then binds
  to a new object (the §8.1 flow).

### 10.2 Where assets land

- **Imported geometry/materials/lights become real managed entities** and appear
  in the outliner immediately (the importer calls the same `Job::Add*` factories
  this spec uses).
- **Textures:** embedded `.glb` images go through `Job::AddInMemoryPNG/JPEG
  TexturePainter` (no disk round-trip, [GLTF_IMPORT.md](../GLTF_IMPORT.md) Phase 3);
  external-URI textures stay referenced by path. The proposed seed-asset home
  `scenes/Default/assets/` ([GUI_ROADMAP.md](../GUI_ROADMAP.md) §13 still-open) is
  the natural drop zone for user-imported sidecar assets, but the **license +
  repo-size decision is still open** and out of scope here.
- **Persistence:** a bulk-imported subtree persists by re-emitting the
  `gltf_import` chunk pointing at the asset path (§7.4) — *not* by re-emitting
  every generated object individually (that would be enormous and would lose the
  importer's provenance). The `ImportProvenance` object (§10.0) is exactly what
  Phase C emits: **one** `gltf_import` chunk carrying the asset path + `name_prefix`
  + options, so reload re-runs the importer deterministically. This is a
  meaningfully cheaper Phase-C target than re-emitting 155 objects, and it is the
  recommended persistence form for imports. (A *deleted* imported subtree
  tombstones as `> remove`-ing the prefix's entities, §5.4 — or, more cheaply, by
  dropping the created `gltf_import` chunk if the whole import was session-created.)

### 10.3 Caveats to surface

- glTF **animation / skinning / morph** are warn-and-skip
  ([GLTF_IMPORT.md](../GLTF_IMPORT.md) §6) — the import dialog should say so.
- `alphaMode = MASK/BLEND` are **PT-only** (BDPT/VCM/MLT treat them as opaque,
  [GLTF_IMPORT.md](../GLTF_IMPORT.md) alpha rows) — surface when the active
  integrator is non-PT.
- **Android import is Tier C** ([GUI_ROADMAP.md](../GUI_ROADMAP.md) §10.4): mobile
  file access + large mesh memory make import a "do it on desktop" graceful
  degrade, not a broken button.

---

## 11. Acceptance criteria

Filled-in per the [GUI_ROADMAP.md](../GUI_ROADMAP.md) §15 template.

- **Tests.**
  - *Controller unit (all families):* add/duplicate/delete for **every** family —
    objects, lights, materials, **painters**, media, geometry, shaders, modifiers,
    cameras — creates/removes the manager entry; undo restores it; redo recreates
    it deterministically from the snapshot (the `SceneEditControllerSaveTest`
    pattern, audit §1). Invariant: manager `getItemCount` returns to baseline after
    add→undo.
  - *Reference-safe delete (across families) — REQUIRED:* exercise the
    descriptor/introspection-driven graph (§5.1) on **non-object** edges, not just
    object→material. **EXISTING-coverage rows (testable today):** delete a painter
    referenced only by a **material** (no object) → blocked; delete a material bound
    to N objects → blocked + referrer list correct; reassign-then-delete rebinds all
    N and removes; delete a bound medium → cascade-clears N interiors; delete a
    shader/modifier → cascade-to-default. **TO-BUILD-coverage rows (gated on stage 2,
    §5.1):** delete a painter referenced by another **painter** (blend chain) →
    blocked + correct referrer list, and delete a painter/material reached only
    through a **composed/sugar material** → blocked — **these two require the new
    `PainterIntrospection` + composed-material reference-readback to land first**;
    until then the painter delete must take the conservative branch (§5.1: block with
    "painter introspection not yet available — delete in text", asserted), never a
    silent 0-reference delete. Invariant: **no orphaned entity** (every manager item
    is name-resolvable; no referrer's bound pointer is absent from its manager) for
    *any* family whose introspection exists — proving the graph is not object-only,
    and proving the painter/composed-material cases are *guarded* (not silently
    wrong) before their introspection lands.
  - *Tombstone survives reload — REQUIRED:* author a scene with a file-authored
    entity (a `standard_object` / material / painter / light in the source bytes);
    delete it in the controller; save; **reload**; assert the entity is **gone**
    (the managed `> remove {family} <name>` line, §5.4, kept it deleted) and that a
    second save is byte-identical. A companion: deleting a **`FOR`-generated** entity
    returns `Status::Refused` (not a silently-non-persisting delete), file
    byte-identical, and the in-memory delete is dropped on reload. Mirror the
    camera managed-block "survives reload+resave" test
    ([SaveEngineTest.cpp:2368](../../tests/SaveEngineTest.cpp)).
  - *Phase-C round-trip per family:* a created
    light/painter/material/medium/object/shader/modifier survives
    save→reload→resave **byte-identically** on the second save (extend the camera
    `SaveEngineTest.cpp:1690` created-entity test; the audit flags a missing named
    material Phase-B test — add the created-entity twins). Include the
    *material-needs-painter composite* (§8.4): a created material + its created
    painter both round-trip, painter declared before material (topo order, §7.3).
    Invariant: byte-identity (the save engine's own correctness signal, audit §1).
  - *Import rollback — REQUIRED:* an "Add Model…" import of a valid glTF commits as
    **one** transaction (one undo entry undoes the whole set; manager counts return
    to baseline after Undo); an import whose Nth entity fails `Apply` mid-commit
    leaves the scene **exactly as before** (composite unwind, §10.0 / TRANSACTION_MODEL
    §11.2 — assert byte-identity on save vs the pre-import save and zero leftover
    managed entities); a *hard* parse failure (`importer.IsValid()` false) commits
    nothing; a *soft* skip (morph-target mesh) commits the importable set and
    reports the skip. Invariant: never a half-imported scene.
  - *Light sampler:* after `AddLight` + a render pass, the new light contributes
    (sampler rebuilt) — a render-of-known-scene RMSE-vs-reference check.
  - *TLAS:* after `AddObject`/`RemoveObject`, a render reflects the changed set
    (no stale BVH); correctness via image compare.
- **Platform parity.** Outliner + add/duplicate/delete + import ship on
  **macOS + Windows** simultaneously (shared core). **Android: outliner Tier B**
  (touch-adapted list + long-press menu + FAB), **creation Tier B**, **import
  Tier C** (desktop-first; graceful "edit on desktop"). Android scene-save is
  wired (Tier A, [GUI_ROADMAP.md](../GUI_ROADMAP.md) §16) so created entities can
  persist on Android too.
- **Performance budget.** Dependency-graph scan is O(objects) on demand — target
  < 1 ms for ≤ 1000 objects; not on any render-hot path. Creation/deletion run
  under cancel-and-park (one interactive-frame stall, identical to the existing
  `CloneActiveCamera` cost). **No production-render regression** (integrators are
  byte-untouched; cite the ~0.4 % L8 bar).
- **Memory budget.** Per-edit snapshot side-table (§4.1) bounded by history
  depth × largest entity descriptor; cap the history as today. No per-frame
  allocation.
- **Accessibility.** Full keyboard path: outliner arrow-key navigation,
  Enter=select, Delete=delete (with confirm), F2=rename; focus order
  outliner→properties. No numpad-only and no colour-only state (the active badge
  and the preview-isolation eye carry an icon + text, not just colour).
- **Packaging.** No new runtime deps (cgltf already vendored,
  [GLTF_IMPORT.md](../GLTF_IMPORT.md) §8). The five build projects must each gain
  any **new** `.cpp`/`.h` (the `ReferenceGraph.{h,cpp}` of §5.1, and any
  `ImportProvenance` unit of §10.0 if extracted) per the
  [CLAUDE.md](../../CLAUDE.md) five-project rule. Seed-asset provenance for
  `scenes/Default/assets/` remains the open §13 license decision.
- **Migration.** **No scene-format change** for creation, deletion, or tombstones:
  creations emit existing chunks, and the managed tombstone is the **existing**
  `> remove` command (§5.4), not a new construct — older scenes parse unchanged.
  Additive only: the new `Category` / `EntityCategory` values (`Painter`,
  `Geometry`, `Shader`, `Modifier`,
  [DirtyTracker.h:46](../../src/Library/SceneEditor/DirtyTracker.h)) and the relaxed
  per-family `SaveEngine.cpp:1369-1383` refusal are backward-compatible (the managed
  block only ever grows entities/tombstones the engine can emit). **ABI-additive
  posture (no break for out-of-tree callers):** new `SceneEdit::Op` values are
  additive; the `IJob` `Add*`/`Remove*` factories already exist; the import
  transaction and `ReferenceGraph` are new non-virtual surface; the create/delete
  ops ride the existing TRANSACTION_MODEL `Propose`/`Commit` C-ABI
  (per the `abi-preserving-api-evolution` discipline). New enum values trigger the
  bridge-enum-translation audit in both bridges ([MEMORY: bridge-enum-translation-audit]).
- **Rollback.** Feature-flag the outliner + creation behind a default-on toggle
  that, when off, leaves the GUI at today's mutate-only behaviour. Created-entity
  persistence and tombstones degrade safely: a family not yet implemented (or a
  delete that can't be tombstoned, §5.4) stays **Refused** (never silent drop), so
  disabling a family's re-emit cannot corrupt a file.

---

## 12. Open questions

- **Built-in primitive set.** "Add Object" needs *some* geometry to bind in an
  empty scene. Ship a minimal sphere/box/plane built-in geometry set, or require
  import-first? (Recommend a tiny built-in set; it is the cheapest unblock for
  §8.1 and aligns with the approachable-default posture.)
- **Material rename of file-authored entities** (§6.3) is Refused in V1. Is a
  reference-rewriting save mode worth a later phase, or is in-memory-only rename
  acceptable indefinitely?
- **Seed-asset license + repo-size budget** for `scenes/Default/assets/`
  ([GUI_ROADMAP.md](../GUI_ROADMAP.md) §13) — shared with the approachability
  spec; blocks import-as-drop-zone polish but not core import.

---

## Related specs

- [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) — code-verified ground truth (read first).
- [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) — transaction/epoch/precondition model these ops obey.
- [GUI_ROADMAP.md](../GUI_ROADMAP.md) — §1 principles, §11 Phase 0, §15 acceptance template, §16 decisions.
- [GLTF_IMPORT.md](../GLTF_IMPORT.md) — the importer wired into "Add Model".
- [MATERIAL_EDITOR.md](MATERIAL_EDITOR.md) — material graph (consumes created materials).
- [CAMERAS_AND_VIEWS.md](CAMERAS_AND_VIEWS.md) — camera promotion (consumes created cameras).
- [CROSS_PLATFORM_ARCHITECTURE.md](CROSS_PLATFORM_ARCHITECTURE.md) — shared-core / bridge rule.
- [SCENE_CONVENTIONS.md](../SCENE_CONVENTIONS.md) — default-light direction trap (§4.2).
