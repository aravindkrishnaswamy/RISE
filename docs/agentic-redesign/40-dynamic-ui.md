# Facet 4 — Dynamic Schema-Driven UI

> **Status:** design-in-progress. One of the parallel facet docs under the
> [Agentic Redesign Charter](00-CHARTER.md). DESIGN ONLY — no code yet.
> **Updated per [`01-DECISIONS.md`](01-DECISIONS.md) (rounds 1 & 2):** binding keys on the immutable
> **NodeId** (lineage identity, D9/D15) and addresses by name-path; positions via the red cursor over
> the persistent rope, not stored spans (D2/D16).
> This facet owns: **the UI as a pure function of (CST + descriptor schema)**,
> widget-per-node, adaptive/growing panels, two-way binding widget↔CST node,
> the split form/source live view, reactive propagation, and the shared-C++ +
> thin-platform-shell contract. It **supersedes** the hand-built accordion
> categories in `SceneEditController` and the per-platform enum-translation
> bridges.

The one sentence: **the UI is `render(view_model(CST, Schema))`, where
`view_model` is a pure, total function written once in shared C++; the only
per-platform code is `render`; and every widget edit is a CST patch routed
through Facet 3's single edit pathway.** No hand-built panel, no fixed
category list, no second mutable representation.

---

## 1. Current-state grounding (what exists today)

The current GUI is a genuinely good ~8.5–9k-LOC SwiftUI/Qt app pair over one
shared C++ controller — but it embodies *Model A* (a live mutable `Scene` plus
half a dozen sidecar representations), and the dynamic-UI primitives it already
has are the seed of what this facet generalizes.

### 1.1 The descriptor schema (the L6 source of truth) — already exists

`ChunkDescriptor` ([src/Library/Parsers/ChunkDescriptor.h](../../src/Library/Parsers/ChunkDescriptor.h))
is the schema. Each of the **152** chunk parsers (`grep -c "ChunkDescriptor& Describe"
src/Library/Parsers/AsciiSceneParser.cpp`) returns one via
`IAsciiChunkParser::Describe()`
([src/Library/Parsers/IAsciiChunkParser.h](../../src/Library/Parsers/IAsciiChunkParser.h)).
A descriptor carries:

- `keyword` ("pinhole_camera"), `category` (`ChunkCategory`, 18 values:
  Painter, Function, Material, Camera, Film, Geometry, Modifier, Medium,
  Object, ShaderOp, Shader, Rasterizer, RasterizerOutput, Light, PhotonMap,
  PhotonGather, IrradianceCache, Animation), `description`.
- a vector of `ParameterDescriptor`, each with: `name`, `kind` (`ValueKind`),
  `required`, `repeatable`, `enumValues` (for `Enum`), `referenceCategories`
  (for `Reference` — *a param may accept refs from several categories*, e.g.
  `ior` accepts Painter **or** Function), `tupleKinds` (whitespace-separated
  typed tuple, e.g. `shaderop foo 0 5 +`), `presets` (`{label, value}`
  quick-picks), `description`, `defaultValueHint`, `unitLabel` ("mm", "°",
  "scene units").

`ValueKind` is the 10-value spine the widget vocabulary maps from:
`Bool, UInt, Double, DoubleVec3, DoubleVec4, DoubleMat4, String, Filename,
Enum, Reference`.

This same descriptor already drives **three** consumers with zero drift —
parsing (`ParseChunk` validates each input line against `Describe().parameters`,
unknown names hard-fail), the suggestion engine / syntax highlighters, and the
properties panel (below). That triple-use is the existing, working proof of
L6. The facet extends it to a **fourth** consumer (the dynamic UI generator)
and tightens the schema with the §10.5/§16 `ParameterSemantics` structure.

### 1.2 The introspection layer (descriptor → panel rows) — already exists, but Model-A-shaped

`CameraIntrospection`, `MaterialIntrospection`, `LightIntrospection`,
`ObjectIntrospection`, `FilmIntrospection`, `MediaIntrospection`,
`RasterizerIntrospection`
([src/Library/SceneEditor/](../../src/Library/SceneEditor/)) each implement the
same pattern: look up the `ChunkDescriptor` that *loaded* the entity, then emit
a `CameraProperty`-shaped row list `{name, kind, value-as-string, description,
editable, presets, unitLabel}` by **reading the live engine object's current
state**, and apply edits via `SetProperty(entity, name, valueStr)` which
parses + dispatches to a setter + calls `RegenerateData()`
([CameraIntrospection.h](../../src/Library/SceneEditor/CameraIntrospection.h)).

This is exactly the right idea (descriptor-driven rows, single source of
truth) implemented against the wrong substrate: it introspects the *derived
engine object*, not the canonical document, and writes back through bespoke
per-entity-family setters. There are **seven** parallel introspection classes —
one per `Category` — each ~3–50 KB. The facet collapses these into **one**
generator that walks CST nodes, because the CST node already *is* the
descriptor-shaped data the seven classes laboriously reconstruct from engine
objects.

### 1.3 The fixed accordion + selection tuple (what we delete)

`SceneEditController` ([.h](../../src/Library/SceneEditor/SceneEditController.h),
[.cpp](../../src/Library/SceneEditor/SceneEditController.cpp), ~56 KB header /
~165 KB impl) hard-codes the UI shape:

- a fixed `enum class Category : int { None=0, Camera=1, Rasterizer=2, Object=3,
  Light=4, Film=5, Material=6, Medium=7, Animation=8 }` (`kNumCategories = 9`)
  and a numerically-locked-in-lockstep `enum class PanelMode`. Comments warn
  "Numeric values are part of the C-API surface … don't reorder."
- per-category arrays `mSelectionByCategory[9]`, `mSectionExpanded[9]`,
  `mPropertiesByCategory[9]`, plus a "primary" `(mSelectionCategory,
  mSelectionName)` tuple.
- bespoke cross-section coherence logic: `ResyncObjectBoundSections_()`
  re-derives the auto-synced Material/Medium selection from the pinned
  Object's bound material after Undo/Redo; `DropStaleSelection_()` clears a
  selection whose entity no longer resolves.
- a whole transaction/rollback/dirty subsystem (`EditorStateSnapshot`,
  `CaptureEditorState`/`RestoreEditorState`, `DirtyTracker`,
  `SourceSpanIndex`/`OverrideSpanIndex`, `SaveEngine`) that exists to keep the
  live scene, inverse-edit history, dirty set, selection, and on-disk text
  consistent — the exact multi-representation coupling the charter's §2
  post-mortem indicts.

Each new entity *kind* that wants a panel today needs a new `Category` enum
value (re-typed in three bridges), a new introspection class, new per-category
state, and new resync logic. **That is the cost the facet eliminates: a new
chunk type should get UI for free.**

### 1.4 The thin-bridge contract + the enum-drift bug (what we structurally fix)

The C-ABI (`RISE_API_SceneEditController_*`) is *already* the bridge core; the
three shells (macOS Obj-C++ `RISEViewportBridge.mm`, Windows Qt
`ViewportBridge.cpp`, Android JNI `RiseBridge.cpp` + `rise_jni.cpp`) are
mechanical marshaling. But each shell re-declares the `Category`/`PanelMode`/
`Tool` enums and translates ints with switch-on-int getters that **silently
fall through to `None` on a missing case** — the documented recurring bug
(memory `feedback_bridge_enum_translation_audit`: "grep `case 5:` in both
bridges when adding a new enum value";
[GUI_ROADMAP.md §10.5](../GUI_ROADMAP.md) `tools/gen_bridge_enums.py`;
[CROSS_PLATFORM_ARCHITECTURE.md §3](../gui/CROSS_PLATFORM_ARCHITECTURE.md)). The
marshaling idioms are already codified: **string-out** (`bool f(p, char* buf,
unsigned len)`), **indexed-list** (`unsigned countF(p)` + `bool itemF(p, idx,
buf, len)`), **struct-out** (fill caller fields). The facet keeps those idioms
and makes the *vocabulary* (enums, widget kinds) generated, never hand-mirrored.

### 1.5 What already maps text ↔ node (reusable)

`SourceSpanIndex` ([SourceSpanIndex.h](../../src/Library/SceneEditor/SourceSpanIndex.h))
already records, per entity, byte ranges for the chunk, the body braces, and
**each parameter's value/comment span** (`ParameterSpan{valueBegin, valueEnd,
commentBegin, …}`), plus `loadedPropertyValues`. This is the embryo of the
CST↔text mapping Facet 1 will make canonical, and the foundation that makes the
split form/source view (§2.4) able to **highlight the exact source span** a
widget edits.

---

## 2. The Model-B design

### 2.0 The shape in one diagram

```
        CANONICAL (Facet 1)              SHARED C++ (this facet)            PER-PLATFORM
                                                                            (this facet)
  ┌──────────────────────────┐    ┌───────────────────────────────┐   ┌──────────────────┐
  │  Lossless CST            │    │  UiModelBuilder                │   │ SwiftUI  (macOS) │
  │   node = (kind, name-    │───▶│   walk CST nodes               │──▶│ Qt6      (Win)   │
  │   path, params[],        │    │   + Schema (descriptors)       │   │ Compose  (Andr)  │
  │   spans, children)       │    │   ⇒ UiTree (ViewNode/Widget)   │   │  = render()      │
  └──────────────────────────┘    └───────────────────────────────┘   └──────────────────┘
            ▲                                  │  ▲                              │
            │ CST patch (Facet 3,              │  │ UiPatch (diff)               │ widget edit
            │ ONE edit pathway)                │  │ on re-derive                 ▼
            └──────────────────────────────────┴──┴──────  EditIntent ───────────┘
                                            (name-path + param + new value
                                             → Facet 3 → CST patch → re-derive)
```

Two pure functions and one event channel:

1. **`UiModelBuilder : CST × Schema → UiTree`** — total, deterministic, no
   engine access. *(This is the generalization of the 7 introspection classes.)*
2. **diff `UiTree_old × UiTree_new → UiPatch`** — for reactive, minimal UI
   updates.
3. **`EditIntent` channel** — a widget gesture becomes a structural intent
   (name-path + parameter + value), handed to **Facet 3's one edit pathway**;
   the resulting CST change re-runs (1) incrementally and emits (2). The UI
   never mutates the CST directly and never invents a second write path
   (INV-1, INV-6, L2).

### 2.1 UI = pure function of (CST + descriptor schema)

**The generator walks the CST and emits one `ViewNode` per CST node, one
`Widget` per parameter, by reading the node's descriptor.** It is the inverse
of the parser: where the parser maps `text → CST → engine`, the builder maps
`CST → view-model`.

```cpp
// src/Library/UI/UiModel.h   (new; shared C++)
namespace RISE::UI {

  // A widget bound to one CST parameter: holds the immutable NodeId (lineage
  // identity, D9) and addresses/displays by name-path.
  struct Widget {
    std::string  namePath;       // "objects/sphere.material" — identity + bind key (INV-5)
    std::string  label;          // descriptor param name ("focal_length")
    WidgetKind   kind;           // §2.5 vocabulary, derived from ValueKind + ParameterSemantics
    std::string  value;          // canonical text of the value token(s) — same form the parser reads
    bool         editable;       // false for derived/const-bound (e.g. pixelAR) or expression-driven
    bool         isDefaulted;    // true if absent from CST (showing the descriptor default) — render ghosted
    // Pure presentation hints, all from the descriptor (zero per-platform logic):
    std::string                  unitLabel;       // "mm", "°"
    std::vector<ParameterPreset> presets;         // quick-pick combo
    std::vector<std::string>     enumValues;      // for Enum → segmented/menu
    std::vector<ChunkCategory>   refCategories;   // for Reference → picker's candidate set
    ParameterSemantics           semantics;       // pipe(color/scalar/either), cardinality, colorspace …
    std::string                  description;     // tooltip / help
    std::string                  diagnostic;      // inline validation message (from Facet 5 validate), or empty
    SourceSpan                   span;            // byte range in canonical text → drives split-view highlight (§2.4)
  };

  // One CST chunk → one collapsible card; nesting mirrors CST nesting.
  struct ViewNode {
    std::string            namePath;     // "objects/sphere"
    std::string            chunkKeyword; // "standard_object"
    ChunkCategory          category;     // grouping hint ONLY (not a fixed enum gate — see §2.3)
    std::string            title;        // name + keyword
    std::vector<Widget>    widgets;
    std::vector<ViewNode>  children;     // nested chunks / inline sub-objects (§2.5 "nested chunks")
    bool                   synthetic;    // true for declarative-generator preview rows (§2.5)
  };

  struct UiTree { std::vector<ViewNode> roots; /* + name-path index */ };

  // THE function. Pure: same (cst, schema) ⇒ same UiTree. No IScene, no engine.
  UiTree Build(const CstDocument& cst, const Schema& schema);
}
```

**Why this is adaptive for free.** The builder has no fixed list of node
kinds. It dispatches purely on `node.chunkKeyword → schema.Describe(keyword)`.
Therefore:

- **Panels grow/shrink as the agent edits the CST.** When Facet 3 commits an
  agent patch that adds a `sdf_geometry` chunk, the next `Build()` walk
  encounters that node, finds its descriptor (which already exists — every
  chunk has one), and emits a card with a widget per `sdf_geometry` parameter.
  No new panel code. When the agent deletes a light, its `ViewNode` simply
  isn't produced. The UI *is* the CST's shape.
- **A brand-new chunk type gets UI the moment its parser ships.** Because
  adding a chunk already *requires* a `Describe()` (the build fails without it
  — `IAsciiChunkParser::Describe()` is pure-virtual), the dynamic UI is
  automatically populated. This is the central payoff: the cost of "new chunk
  type → new panel" drops from "new Category enum + introspection class + 9
  per-category state slots + 3 bridge enum copies + resync logic" to **zero**.

**How this supersedes the fixed accordion.** The current `Category` enum is a
*closed* set of 9 panels, each hand-wired. The new model has no `Category`
gate: grouping (when the human wants the familiar "Cameras / Lights / Objects"
sectioning) becomes a **pure view transform over `UiTree`** keyed on
`ViewNode.category` (the descriptor's own `ChunkCategory`). The "accordion" is
just one optional `GroupBy(category)` lens; a flat outliner, a search-filtered
list, or an agent-authored focus view ("show me everything the last patch
touched") are other lenses over the same tree. The 18 `ChunkCategory` values
already exceed the 9 hand-built panels — so even the default grouping covers
chunk families (Painter, Function, ShaderOp, Modifier, PhotonMap, …) that have
**no panel today**.

### 2.2 Two-way binding widget ↔ CST node (via NodeId, addressed by name-path — D9/D15)

**Two-level identity (per [`01-DECISIONS.md`](01-DECISIONS.md) §D9).** Widgets bind to the
immutable internal **NodeId** (lineage identity — it survives rename and reparse via structural
matching, so a binding never breaks under an edit, INV-5); the **name-path**
(`objects/sphere.material`) is the human/agent-facing **address** that resolves to a NodeId within a
version. The widget displays + edits by name-path but *holds* the NodeId, so a rename (which changes
the name-path) does not drop the binding. This is the principled replacement for the Model-A
transient-pointer/array-index identity the round-4 "identity serial" patched over.

**Widget → CST (edit emits a patch through Facet 3's one pathway).**

```cpp
// A gesture produces an EditIntent — structural, not textual, not engine-level.
struct EditIntent {
  std::string namePath;   // "cameras/main.focal_length"
  enum Op { SetParam, AddParam, RemoveParam, AddChunk, RemoveChunk, RenameChunk } op;
  std::string value;      // canonical token text for SetParam/AddParam ("50")
  // AddChunk carries (keyword, proposedName, seedParams); RenameChunk carries newName.
};
```

The widget layer's *only* write action is `controller.Submit(EditIntent)`. The
controller forwards it to **Facet 3**, which is the sole owner of "turn an
intent into a CST patch, validate, version, derive." This facet **does not**
implement patching, undo, or text splicing — it asserts the dependency
(§5) and stops at the intent boundary. Concretely, `SetParam` on
`cameras/main.focal_length = "50"` becomes a Facet-3 CST patch that rewrites
exactly the value token at `Widget.span` (reusing the `SourceSpanIndex`
`ParameterSpan` machinery, INV-4: untouched text byte-preserved). This replaces
today's `SceneEditController::SetProperty` → `CameraIntrospection::SetProperty`
→ engine-setter → `RegenerateData()` chain *and* the `SaveEngine` round-trip:
there is no longer a "live edit now, serialize later" gap, because the CST is
canonical and the engine is re-derived from it (Facet 2).

**CST → widget (a change reactively updates the widget).** Any CST mutation —
from the GUI, from the agent (Facet 5), or from a raw text edit in the source
pane — triggers Facet 1 to publish a new CST revision. The controller responds:

```
on CstRevisionPublished(newCst):
    newTree = UiModel::Build(newCst, schema)          // pure re-derive (incremental, §2.6)
    patch   = Diff(currentTree, newTree)              // minimal widget delta
    currentTree = newTree
    emit UiPatch(patch)  →  platform render() applies it
```

This is the reactive propagation model: **one-way data flow** (CST → UiTree →
render) plus **a single upstream event** (EditIntent → Facet 3 → CST). A
widget never holds authoritative state; it renders `Widget.value` and emits
intents. Because the agent and the human both move the CST, the human's panels
visibly "fill in" as the agent works — the charter's tenet-2 ("the UI mutates
as a human works *with* an agent") is mechanical, not bolted-on.

### 2.3 Why there is no fixed `Category` / `PanelMode` enum anymore

`Category`/`PanelMode`/`kNumCategories` are **deleted**. Grouping is data:
`ViewNode.category` is the descriptor's `ChunkCategory`, used only as a sort/
header key by an optional grouping lens. Selection (§2.7) carries a name-path,
not a category int. This removes the entire class of "add a value, re-type it
in three bridges, audit `case 5:`" drift — there is no per-panel enum to drift,
and the one enum the bridges still need (`WidgetKind`) is **generated**
(§2.8). The `ResyncObjectBoundSections_` / `DropStaleSelection_` coherence
logic dissolves: cross-references (an object's bound material) are just
name-path links in the CST that the builder can render as either a navigable
chip ("jump to `materials/glass`") or, under a "show bound material inline"
lens, an embedded child `ViewNode` — and a stale selection is simply a
name-path the next `Build()` doesn't produce, so the selection layer drops it
by the same generic rule for every kind.

### 2.4 The split form/source live view ("devtools for scenes")

The signature surface, matching the charter's "nerds" positioning: **widgets
on one side, canonical `.RISEscene` text on the other, both live, both
synced.** This is browser-devtools' "Elements pane ↔ DOM" applied to a scene.

- Both panes are *projections of the one CST* — neither is authoritative over
  the other (INV-1). The text pane shows Facet 1's canonical serialization
  (lossless: comments, whitespace, formatting preserved, INV-4). The form pane
  shows `UiTree`.
- **Selection is bidirectional and span-driven.** `Widget.span` (a
  `SourceSpan` byte range, from §1.5) lets the form→source direction scroll +
  highlight the exact value token when a widget is focused, and the
  source→form direction map a cursor offset back to the owning
  `ViewNode`/`Widget` (reverse span lookup). Click a widget → the line glows;
  click in the text → the card highlights. *(This is precisely what
  `SourceSpanIndex::ParameterSpan` already records per parameter; the facet
  promotes it from a save-engine internal to a first-class UI binding.)*
- **Editing either side is the same edit.** A form edit emits an `EditIntent`
  (§2.2). A text edit is a raw buffer change that Facet 1 re-parses into a new
  CST revision; the form pane re-derives reactively (§2.2 reverse direction).
  Under the locked **lossless-CST pivot** (O1), the text pane is a *view* with
  a debounced commit (O2): keystrokes update a scratch buffer, and on
  debounce/blur Facet 1 attempts `text → CST`; on parse success it publishes a
  revision (both panes converge), on failure the source pane shows inline
  diagnostics (Facet 5's `validate`) and the form pane keeps the
  last-good tree with a "source has unparsed edits" banner. *(If the open O1
  decision instead lands on text-canonical — buffer-is-truth — the only delta
  is that the form pane becomes strictly downstream of the buffer and the
  debounce is the sole commit point; the rest of this facet is unchanged
  because the form pane already treats the CST as upstream.)*
- This is the same surface as the agent's "show me the code" panel
  ([GUI_ROADMAP.md §9.5](../GUI_ROADMAP.md)) and the diff-review gate (Facet
  5): an agent patch animates *both* panes live, and the human reviews the
  textual diff next to the structural change.

### 2.5 Widget vocabulary (descriptor parameter type → concrete widget)

The mapping is a pure function `WidgetKind(ParameterDescriptor,
ParameterSemantics)` evaluated in shared C++ (so all platforms agree by
construction). The platform `render()` switches on `WidgetKind` only.

| Descriptor signal | `WidgetKind` | Concrete widget | Notes |
|---|---|---|---|
| `Bool` | `Toggle` | switch / checkbox | value `"TRUE"`/`"FALSE"` |
| `UInt` | `IntStepper` | number field + stepper; `UINT_MAX` → "unlimited" affordance | `to_hint` already renders the sentinel |
| `Double` | `Scalar` | number field + drag-scrub; `unitLabel` suffix; `presets` → combo | scrub gesture = debounced `SetParam` stream (§2.7) |
| `Double` **+ `presets`** (e.g. sensor mm) | `ScalarPreset` | combo with editable line-edit | "Full-frame 35mm" → `36` |
| `DoubleVec3` **+ semantics.pipe=color** | `ColorWell` | color swatch + picker; **spectral-aware** (Kelvin/λ/CSV per [SPECTRAL_DIFFERENTIATORS.md](../gui/SPECTRAL_DIFFERENTIATORS.md)) | the IPainter color pipe |
| `DoubleVec3` (position/scale/normal) | `Vec3` | three linked number fields (x/y/z) | non-color vectors |
| `DoubleVec4` | `Vec4`/`Quat` | four fields; quaternion-tagged → optional Euler proxy | xyzw |
| `DoubleMat4` | `Matrix4` | 4×4 grid, read-mostly + "edit as TRS" affordance | transform precedence is `matrix>quat>orientation` |
| `String` | `Text` | single-line field | free identifier |
| `Filename` | `FilePath` | path field + native file picker | resolved via `RISE_MEDIA_PATH`; picker is platform file dialog |
| `Enum` (`enumValues`) | `EnumChoice` | segmented control (≤4) / dropdown (>4) | choices = `enumValues` verbatim |
| `Reference` (`referenceCategories`) | `RefPicker` | **named-entity picker** (see below) | candidate set = nodes whose category ∈ `referenceCategories` |
| `tupleKinds` non-empty | `Tuple` | inline row of per-token sub-widgets | each token uses its own kind (e.g. `Ref UInt UInt Enum`) |
| `repeatable` param | `List<kind>` | add/remove rows, each a sub-widget of the base kind | e.g. spectral `cp` control points; expression `param`/`def` |
| **semantics.pipe = scalar** on a `Reference`/`Double` slot | scalar-tinted variant | same widget, **grey** pipe accent (vs **yellow** for color) | the `IScalarPainter` vs `IPainter` distinction, surfaced visually |
| function-expression chunk (`expression_function2d`: `param`/`def`/`expr`) | `ExprEditor` | see below | the math-expression case |
| declarative generator (instancer / sweep / sdf / guilloché disk) | `GeneratorCard` | parameter form **+ synthetic read-only preview of derived instances** | see below |

**Name-ref renders as a picker.** Today a `Reference` "renders as text +
presets dropdown" (`MaterialIntrospection.cpp` comment). The facet upgrades it
to a real `RefPicker`: the candidate set is computed *from the CST itself* —
all `ViewNode`s whose `category` is in the param's `referenceCategories`. So
`ggx_material.ior` (accepts `Painter|Function`) offers every painter and
function chunk currently in the document, by name-path, plus "create new …"
(an `AddChunk` intent) and "none". Selecting an entry emits `SetParam` with
that name. Because candidacy is derived from the live CST, the picker is
**always correct and adaptive** — a painter the agent just added appears
immediately. The color/scalar pipe (`ParameterSemantics`) further filters/
flags candidates (an `IScalarPainter` slot greys out pure-color painters,
matching the §16 socket-coloring rule and preventing the
`ResolveOrDiagnoseScalar` JH-uplift bug class at *author* time).

**Function-expression renders as a mini-editor with edit-in-source escape.**
`expression_function2d` (and kin) carry repeatable `param <name> <number>`,
repeatable `def <name> <expr>` let-bindings, and a final `expr` over
`u,v,params,defs`. The `ExprEditor` widget is a small structured editor: a
table of `param` rows (name + number stepper), a table of `def` rows (name +
expression line), and the final `expr` line — each row a `SetParam`/`AddParam`/
`RemoveParam` intent on the `def`/`param` repeatable. The expression *strings*
themselves get a single-line code field with **syntax-aware affordances reusing
the existing grammar** (the function/operator list lives in the descriptor
`description` and the `MathExpressionEvaluator`), plus an **"edit in source"**
button that focuses the corresponding span in the source pane (§2.4) for heavy
edits. Rationale: a full expression IDE is out of scope; the structured row
editor handles the common case (tweak a constant, add a def), and anything
gnarlier drops to the always-present text pane — which is the whole point of
the split view. Other parameters that are *function references* (e.g. a slot
bound to a named `expression_function2d`) render as a `RefPicker` to that
function plus a "open" affordance, **read-only with edit-in-source**, never an
inline re-implementation.

**Declarative generators render as form + synthetic preview.** Per L3
(iteration is declarative; FOR/DEFINE removed), repetition is a generator chunk
(instancer, `sweep_geometry`, `sdf_geometry`, `guilloche_disk_geometry`,
`path_instances_geometry`). Its `GeneratorCard` shows the parameter form
normally (so editing the generator's knobs is ordinary `SetParam`), **plus** a
collapsed, **read-only** list of the instances it *derives* — emitted as
`synthetic: true` child `ViewNode`s the builder gets from Facet 2's derivation
(a count + per-instance transform summary), clearly badged "derived — edit the
generator above." This is how the UI represents "homogeneous instancing →
instances derived, not authored" (L3) without lying that each instance is
independently editable. *(A "typing-shortcut loop," L3's other case, is
desugared by Facet 1 into N separate CST nodes at author time, so it just
renders as N ordinary cards — no special widget.)*

**Nested chunks render as nested cards.** A chunk that contains inline
sub-chunks (e.g. an object with an inline geometry/material, or an
`sdf_geometry` with inline `part` lines) produces child `ViewNode`s; the form
pane shows them as indented sub-cards, matching the CST nesting exactly. The
generic recursion in `UiModel::Build` handles arbitrary depth with no
per-shape code.

### 2.6 Incremental re-derivation (latency is a first-class output, INV-3)

A localized CST edit must not rebuild the whole `UiTree`. The builder is
**memoized per CST node**: each `ViewNode`/`Widget` is a pure function of one
CST node (+ for `RefPicker`, the *set of node names* per referenced category;
+ for `GeneratorCard`, the derived-instance summary from Facet 2). On a CST
patch, only the nodes in the patch's name-path set (and `RefPicker`s whose
candidate category was added-to/removed-from, and `GeneratorCard`s whose
inputs changed) are rebuilt; everything else is reused by identity. The `Diff`
then yields a `UiPatch` touching only changed widgets. Two cache-invalidation
edges to respect:

- **reference-set edges:** adding/removing/renaming a node in category *C*
  invalidates every `RefPicker` whose `referenceCategories` contains *C* (its
  candidate list changed). This is a small reverse index `category → {widgets}`.
- **derivation edges:** a `GeneratorCard`'s synthetic children depend on Facet
  2's derived scene; the builder subscribes to Facet 2's per-node derivation
  generation (the same dependency graph Facet 2 owns) rather than recomputing.

Budget: an interactive scrub (O2 debounced-commit) targets a single-parameter
patch → single-widget rebuild → single-widget `UiPatch`, well under one frame.
The agent's coarser "rewrite-and-reload" patches re-derive their touched
subtrees only. *(If O2 instead requires 60 Hz incremental derivation, the
delta is: the scrub gesture commits a CST patch per pointer-move instead of
per-debounce, and the per-node memoization above is what keeps that cheap — the
architecture already supports it; only the commit cadence changes.)*

### 2.7 Selection & session state (L4: document vs ephemeral)

Per L4, **only the CST is canonical**; selection, the active tool, the orbit
camera, render-in-progress, and per-card expand/collapse are **ephemeral
session state** and live *outside* the CST.

- **Selection is a name-path** (or a set of them), held in a small session
  object — not a `(Category, name)` tuple, not nine per-category slots. It
  *resolves* to a `ViewNode` by name-path lookup each derive.
- **It survives edits** because name-path is stable (INV-5): after any patch,
  the selection re-resolves against the new `UiTree`; if the node still exists
  (even moved, even with changed params) the selection holds; if it was
  deleted, it drops by one generic rule (replacing `DropStaleSelection_`'s
  special-casing). Cross-entity coherence (selecting an object also surfaces
  its material) becomes "the inspector renders the selected node *and* renders
  its name-path references as navigable, optionally-inlined links" — no
  `ResyncObjectBoundSections_` resync, because there is no second copy of the
  selection to keep in sync.
- **Ephemeral view state** (which cards are expanded, scroll position, the
  active grouping lens) is platform-or-session-local and explicitly *not*
  serialized into the scene (matching the memory note
  `feedback_roundtrip_scope_prefer_readonly`: don't extend the scene language
  to store view state).

**Gizmos & tools relate to the CST as intent producers (Facet 3).** A
translate/rotate/scale gizmo, an orbit drag, a region-render box — each is a
*tool* that, on commit, produces a CST patch via the same `EditIntent`/Facet-3
pathway (a transform gizmo emits `SetParam` on the object's
`position`/`orientation`/`scale`/`matrix`). The gizmo's *handles* and the
mid-drag preview are ephemeral session state (the current `GizmoHandle` /
`GizmoDragState` machinery in `SceneEditController`, kept but re-pointed at the
intent boundary). Crucially, a tool never has a private write path: "tools
produce CST patches" (Facet 3) is the same rule as "widgets produce CST
patches," so a gizmo drag is undoable/diffable/agent-visible identically to a
typed value. The active-tool state, the Photoshop "last sub-tool per category"
memory, and preview-scale adaptation stay ephemeral and platform-thin.

### 2.8 Cross-platform: schema→view-model shared, render per-platform

**Shared C++ (the bulk):**

- `UiModel::Build` (the generator), `Diff`, the `WidgetKind` mapping, the
  `RefPicker` candidate computation, the selection-resolution rule, the
  `EditIntent` construction, and the reactive re-derive loop. **None of this is
  per-platform** — it is the view-*model*, and it is written once.
- The schema itself (descriptors + the new `ParameterSemantics`).

**Per-platform (`render()` only):** each shell consumes the `UiTree`/`UiPatch`
via the C-ABI and draws native widgets, switching solely on `WidgetKind`. The
shells own only the charter-allowed four categories (native widget rendering,
present surface, file/credential dialogs, input-event translation,
[CROSS_PLATFORM_ARCHITECTURE.md §1.2](../gui/CROSS_PLATFORM_ARCHITECTURE.md)).
SwiftUI builds a `List`/`Form` from the tree; Qt6 builds a `QFormLayout`;
Compose builds a `LazyColumn`. A new `WidgetKind` is rendered three times (the
*drawing* genuinely differs), but **defined once**.

**The thin-bridge contract (how we avoid the enum-drift the current bridges
suffer).** The C-ABI surfaces the `UiTree` through the three already-codified
idioms ([§3.2](../gui/CROSS_PLATFORM_ARCHITECTURE.md)) — no new shape:

- **indexed-list** for nodes/widgets: `unsigned UiNodeCount(p)`,
  `bool UiNodeAt(p, idx, UiNodeDesc* out)`, `unsigned UiWidgetCount(p, nodeIdx)`,
  `bool UiWidgetAt(p, nodeIdx, wIdx, UiWidgetDesc* out)`.
- **string-out** for the variable-length fields (`namePath`, `value`, `label`,
  `diagnostic`) and for indexed sub-lists (`presets`, `enumValues`,
  `refCandidates`).
- **struct-out** for the fixed scalar fields (`kind` int, `editable`,
  `isDefaulted`, `span.begin/end`).
- the single upstream call: `bool UiSubmitIntent(p, const UiIntent* intent)`.

The **one enum that crosses the boundary is `WidgetKind`** (plus the existing
`ChunkCategory` for grouping). It is **generated**, not hand-mirrored: extend
`tools/gen_bridge_enums.py` ([GUI_ROADMAP.md §10.5](../GUI_ROADMAP.md)) to emit
`WidgetKind` to a single canonical header + Kotlin constants + Obj-C
`static_assert` mirrors, so adding a `WidgetKind` is a one-place edit and drift
is a build break, not a silent `case N:` fall-through. Because grouping is now
data (`ChunkCategory`) and selection is a name-path string, the bridges have
**no per-panel enum to translate** — the entire `Category`/`PanelMode`
switch-on-int translation layer (the documented bug site) is deleted, not
re-implemented. This is the structural fix the memory note
`feedback_bridge_enum_translation_audit` and
[CROSS_PLATFORM_ARCHITECTURE.md §3.1](../gui/CROSS_PLATFORM_ARCHITECTURE.md)
call for.

**Android tiering** ([CROSS_PLATFORM_ARCHITECTURE.md §6](../gui/CROSS_PLATFORM_ARCHITECTURE.md)):
the generator is shared C++ consumed via JNI, so Android gets the dynamic form
for free (Tier A/B). The split form/source view is Tier B on a phone (a
segmented toggle between panes rather than side-by-side); the `ExprEditor` and
`GeneratorCard` preview degrade to "view + edit-in-source" gracefully (Tier C
heavy-edit affordances point to desktop). The `WidgetKind`→Compose render is
the only Android-specific UI code, and it is mechanical.

---

## 3. Delete / Evolve / Reuse

**Delete (this facet's explicit supersession inventory, L7):**

- The **fixed accordion model** in `SceneEditController`: the `Category` and
  `PanelMode` enums, `kNumCategories`/`kNumToolCategories` panel coupling,
  `mSelectionByCategory[9]`, `mSectionExpanded[9]`, `mPropertiesByCategory[9]`,
  the `(mSelectionCategory, mSelectionName)` primary tuple, and the accessors
  built on them (`CategoryEntityCount/Name`, `CurrentPanelMode`,
  `PropertyCountFor`/`PropertyNameFor`/… per-category snapshot family).
- The cross-section coherence special-casing: `ResyncObjectBoundSections_`,
  `DropStaleSelection_` (replaced by the generic name-path re-resolution rule,
  §2.7).
- The **per-platform enum-translation getters** (`viewportSelectionCategory`,
  `viewportPanelMode`, and their Mac/Win twins with switch-on-int + `None`
  fall-through) — there is no panel enum to translate.
- The seven introspection classes' role as *the* UI source
  (`CameraIntrospection`, `MaterialIntrospection`, `LightIntrospection`,
  `ObjectIntrospection`, `FilmIntrospection`, `MediaIntrospection`,
  `RasterizerIntrospection`) collapse into the single `UiModel::Build` walk.
  *(Their descriptor-lookup + value-formatting helpers are harvested — see
  Reuse — but the per-family class structure goes.)*
- The Model-A write-back surface this facet drove: `SceneEditController::
  SetProperty`/`SetPropertyForCategory` → `*Introspection::SetProperty` →
  engine-setter → `RegenerateData()`, and the **save-engine round-trip as a
  separate step** (`SaveEngine` Mode A/B splice driven by `DirtyTracker` +
  `SourceSpanIndex`/`OverrideSpanIndex`). In Model B, a widget edit is a CST
  patch and persistence is "the CST *is* the document" — the diff/dirty/splice
  machinery is owned by Facet 3's CST versioning, not re-implemented here.
  *(The transaction/rollback subsystem itself is Facet 3's deletion to
  inventory; this facet only deletes its UI-facing surface.)*

**Evolve:**

- **`SourceSpanIndex` → the canonical CST↔text span map.** Its `ParameterSpan`
  byte ranges become the `Widget.span` that powers the split-view bidirectional
  highlight (§2.4). Facet 1 owns the canonical form; this facet consumes the
  spans. (The save-engine-internal framing retires; the data shape lives on.)
- **`ParameterDescriptor` → gains `ParameterSemantics`** (pipe color/scalar/
  either, cardinality, `requireSingle`, units, colorspace, spatial-vs-spectral;
  [GUI_ROADMAP.md §16](../GUI_ROADMAP.md)). Drives `ColorWell` vs scalar-tinted
  widgets and `RefPicker` candidate filtering. Owned jointly with the Material
  Editor / Validation specs; this facet is a primary consumer.
- **The C-ABI marshaling idioms** (string-out / indexed-list / struct-out) are
  reused verbatim for the `UiTree`/`UiPatch`/`UiIntent` surface; only the
  payload changes.
- **`tools/gen_bridge_enums.py`** extends to emit `WidgetKind`.

**Reuse (unchanged):**

- The **152 `Describe()` descriptors** — the whole point; they are already the
  schema and need no per-UI change beyond the additive `ParameterSemantics`.
- The descriptor-lookup + value-as-string formatting logic inside the
  introspection classes (e.g. `to_hint`, the `ValueKind`→string conversions)
  — harvested into `UiModel::Build` as free helpers.
- The ephemeral viewport machinery: `GizmoHandle`/`GizmoDragState`, the
  preview-scale adaptive state machine, the cancel-restart render thread, the
  Photoshop tool-category memory — all stay (they are session state, L4), just
  re-pointed so tool commits emit `EditIntent`s.
- The thin-shell pattern itself (one C-ABI, three marshaling shells) — the
  facet pushes *more* behind it, consistent with the roadmap's principle 2.

---

## 4. Hard problems & open questions

1. **`UiModel::Build` reading from CST alone vs needing derived data.** Most
   widgets are a pure function of the CST node. But three need derivation
   results: `GeneratorCard`'s instance preview (needs Facet 2's derived
   instances), `RefPicker` candidate *validity* (a ref might parse but fail
   `ResolveOrDiagnoseScalar`'s color/scalar check — a derive-time diagnostic),
   and `Widget.diagnostic` (Facet 5's `validate`). The clean line: **the form
   is a function of the CST; *annotations* (derived previews, diagnostics) are
   a function of derivation and arrive as a second, additive overlay.** The
   tar-pit is letting derived data leak into the *structure* of the tree
   (re-introducing Model A's coupling). Mitigation: `synthetic` children and
   `diagnostic` strings are strictly additive overlays that never change which
   widgets exist; the base tree derives without the engine. Needs a crisp
   contract with Facet 2 on the per-node "derived summary" it exposes.

2. **The text pane's edit granularity under lossless-CST (O1).** A raw text
   edit that doesn't yet parse must not destroy the form pane's last-good
   state, and a *partial* edit (mid-keystroke) must not spam re-derives.
   Debounced-commit (O2) handles cadence, but the failure UX — "source has
   unparsed edits, form is stale" — needs careful design so it's informative,
   not alarming. The deeper risk is the lossless round-trip itself (charter
   names it a tar-pit): a structured edit must touch *only* the value token
   (INV-4), which the `ParameterSpan` machinery does today for a flat scene,
   but FOR/DEFINE removal (L3) and nested chunks change the span model. This
   facet depends on Facet 1 nailing it; flagged for synthesis.

3. **`RefPicker` candidate explosion + reference cycles.** In a large scene a
   `Painter`-typed ref could offer hundreds of candidates; the picker needs
   search/scoping (by name prefix, by "used near here"), and the candidate
   computation needs the reverse index (§2.6) to stay cheap. Reference cycles
   (A refs B refs A) are a derivation concern, but the UI should *show* them
   (a cycle badge) rather than silently produce a broken tree.

4. **Two-pane selection identity across a reformat.** If a text edit reorders
   or reformats chunks, NodeId identity holds (D9, good) and absolute byte
   positions are **not stored** — under the red-green tree (D2) the reverse
   lookup (source→form) derives positions on demand via the version's red
   cursor, so there are no stored spans to "shift" or republish. The only
   discipline needed is that the form pane reads positions from **one CST
   version snapshot** (never mid-edit), which the immutable-version model (D1)
   gives for free.

5. **Open (flag for human review):** does the **default lens** stay the
   familiar `GroupBy(ChunkCategory)` accordion (lower migration shock, but
   re-imposes a category feel), or lead with a **flat name-path outliner +
   search** (truer to "devtools," but a bigger UX jump)? Recommendation:
   ship `GroupBy(category)` as default with the flat outliner one toggle away
   (progressive disclosure), but this is a product call.

6. **Open:** how much expression editing belongs in `ExprEditor` vs
   edit-in-source? A richer inline editor (live error squiggles, function
   autocomplete from the descriptor) is appealing but risks rebuilding an IDE.
   Recommendation: structured rows + single-line fields + edit-in-source for
   v1; revisit after usage. Flagged.

---

## 5. Cross-facet dependencies & assumptions

- **Facet 1 (CST + descriptors)** — *hard dependency.* I assume: (a) each CST
  node carries `(chunkKeyword, NodeId, name-path, params[] with per-param value
  text, children)` — positions are derived via the red cursor, not stored (D2/D16);
  (b) the immutable **NodeId** is the stable lineage identity and **name-path is
  version-resolved addressing** (D9/D15, INV-5); (c)
  the schema (`Describe()` descriptors + `ParameterSemantics`) is queryable as
  `Schema::Describe(keyword)`; (d) Facet 1 publishes per-revision position lookup so the
  split view can map both directions; (e) declarative iteration (L3) means the
  builder never sees FOR/DEFINE — either a generator chunk (→ `GeneratorCard`)
  or pre-desugared N nodes. **Conflict to flag:** none with a Locked decision;
  this facet *relies on* L3/L5/L6 rather than bending them.
- **Facet 3 (edit model & history)** — *hard dependency, the one edit
  pathway.* I assume Facet 3 exposes `Submit(EditIntent)` (or equivalent) that
  turns an intent into a validated CST patch, versions it, and triggers
  derivation; and that gesture debouncing (drag → one undo entry) lives there.
  **I deliberately do not implement patching/undo/splice** (INV-6, L2) — the
  facet stops at the intent boundary. If Facet 3's intent vocabulary differs
  from §2.2's `{SetParam, AddParam, RemoveParam, AddChunk, RemoveChunk,
  RenameChunk}`, synthesis should reconcile names (the *shapes* are what
  matter).
- **Facet 2 (derivation engine)** — *soft dependency* for annotations only:
  the `GeneratorCard` instance summary and per-node derive diagnostics. The
  base tree must derive without Facet 2 (§4.1). I assume Facet 2 offers a
  per-node "derived summary + generation counter" the builder can subscribe to
  incrementally (INV-3).
- **Facet 5 (agentic surface)** — *peer.* The agent edits via the same Facet-3
  pathway, so its patches drive the reactive UI for free (§2.2). I assume
  Facet 5's `validate` produces per-parameter diagnostics addressable by
  name-path, which become `Widget.diagnostic`. The "show me the code" panel is
  literally this facet's source pane.
- **`ParameterSemantics`** is assumed adopted ([GUI_ROADMAP.md §16](../GUI_ROADMAP.md));
  the color-vs-scalar widget split degrades to "all refs look alike" without
  it, so it's a soft-but-strongly-wanted input.
- **Working assumptions on Open decisions:** O1 lossless-CST-pivot (the source
  pane is a view; §2.4 notes the text-canonical delta), O2 debounced-commit
  (§2.6 notes the 60 Hz delta).

---

## 6. First-slice implications (minimal end-to-end vertical)

The charter's first slice: **one chunk type, text⟷CST⟷derived-scene, one
schema-generated widget, live incremental re-derive.** This facet's
contribution to that slice, kept deliberately minimal:

1. **Pick `pinhole_camera`** (small descriptor; already has a working
   introspection class to validate against, so we can A/B the generated rows
   vs the legacy rows for confidence).
2. **`UiModel::Build` over a one-node CST** — walk the single camera node, look
   up its descriptor, emit one `ViewNode` with a `Scalar` widget for
   `focal_length` (carrying `unitLabel="mm"`, its `presets`, and its
   `SourceSpan`). This exercises the whole generator spine on one parameter.
3. **One generated widget on one platform** — render the `Scalar` widget in
   SwiftUI (or Qt) by switching on `WidgetKind`, via the indexed-list +
   string-out + struct-out C-ABI surface. Prove the thin shell needs *zero*
   panel-specific logic.
4. **Two-way binding on that one widget:** editing `focal_length` emits a
   `SetParam` `EditIntent` → Facet 3 patches the CST value token → Facet 2
   re-derives the camera → the viewport updates *and* `UiModel::Build`
   re-derives the single widget reactively (§2.2). Editing the value in the
   source pane (a raw text edit) round-trips the other direction.
5. **Live incremental re-derive proof:** assert that a `focal_length` patch
   rebuilds exactly one widget (the memoization of §2.6), not the tree.
6. **The vertical's headline demo:** the split form/source view on this one
   camera — type `50` in the field, watch the source line change; type `35` in
   the source, watch the field change; have the agent (Facet 5 stub) patch it,
   watch both move. That single loop demonstrates every tenet (UI is a live
   projection; UI mutates as human+agent work; one edit pathway) on the
   smallest possible surface, and every subsequent chunk type is *the same code
   path with a different descriptor* — which is the entire thesis of the facet.

---

### Acceptance-criteria seed (per [GUI_ROADMAP.md §15](../GUI_ROADMAP.md))

- **Tests:** golden-`UiTree` snapshot per descriptor (purity: same CST+schema
  ⇒ same tree); a **descriptor-coverage test** asserting every one of the 152
  chunk keywords produces a non-empty, fully-typed `UiTree` node (the
  "new chunk gets UI for free" invariant, enforced like the parser's
  `Describe()` pure-virtual); a round-trip test (form edit → CST patch → text
  → re-parse → CST → same `UiTree`); a `gen_bridge_enums` conformance test for
  `WidgetKind` (drift = build break).
- **Platform parity:** generator + view-model shared (all platforms); only the
  `WidgetKind`→native render differs. Android Tier A/B forms; split view Tier B
  (toggle); heavy expr/generator editing Tier C → edit-in-source.
- **Performance budget:** single-parameter edit → single-widget rebuild +
  `UiPatch` under one frame (INV-3); full-scene `Build` amortized by per-node
  memoization. No production-render impact (the builder never runs on render
  threads).
- **Migration:** the legacy accordion stays behind a flag during the
  transition; `GroupBy(category)` lens reproduces the familiar sectioning so
  the visible UX shift is opt-in.
