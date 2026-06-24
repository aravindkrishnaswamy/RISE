# Material Editor — Instance-First, then Node Graph, then MaterialX Import

**Status:** DESIGN. No code. Deep-dive spec spun off from [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §7 (Direction C) and §11 Phases 3–4. Owns the material slice of the §16 confirmed decisions — in particular the **adopted `ParameterSemantics` structure on `ParameterDescriptor`** (a structure with **separate fields** — `pipe`, cardinality, `requireSingle`, units, colour space, spatial-vs-spectral — **not** a single overloaded `pipe` enum, per [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §16).
**Owner:** Aravind Krishnaswamy
> **Ground-truth basis.** Code claims are reconciled against [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) (code-verified, 2026-06-19; supersedes any plan-doc `Status:` header where they disagree). Plan-doc status headers were treated as suspect. `.claude/worktrees/` ignored. Two facts that reshape this spec versus its first draft: (1) **round-trip save IS shipped**, including Phase-B property re-emit for **material entities** — so C2 Phase B's genuinely new work is graph-*topology* serialization **plus painter-property persistence**, not material-*entity*-property persistence (audit §1, punch-list Theme 1). **⚠ Phase B does NOT cover `Painter`** (tracked families: Camera/Light/Material/Medium + object bindings), so painter-node value persistence — which the C1 colour picker needs — is **TO-BUILD** (`PainterIntrospection` + painter edit ops + painter dirty tracking + painter save re-emit; [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §13a #7, §2.1 note); (2) the **`ParameterSemantics` structure is adopted**, not proposed — and it is a structure with **separate fields** (`pipe`, cardinality, `requireSingle`, units, colour space, spatial-vs-spectral), **not** a single overloaded `pipe` enum ([../GUI_ROADMAP.md](../GUI_ROADMAP.md) §16).
**Scope:** A material-authoring surface for the RISE desktop GUIs (macOS SwiftUI + Obj-C++ bridge, Windows Qt6 + C++ bridge) and a staged Android tier. Three staged deliverables: **C1** an instance-first flat editor over today's descriptor material slots (cheap 80%, *no new serialization*); **C2** a node-graph editor with an OpenPBR-shaped default uber-node, per-node live thumbnails, pipe-typed sockets, and **new scene-language graph serialization** (the heavy lift, gated behind "Advanced"); **C3** MaterialX / OpenPBR *import* (RGB→spectral at the boundary, never native storage). Honors the [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §1 principles: text is canonical, max shared C++, progressive disclosure, one mutation path, differentiate on spectral, Android not left behind.

This doc owns the material slice of the roadmap. It cross-references the **spectral material widgets** (curve editor, measured-metal n,k picker, thin-film slider, gamut warning) to the sibling [SPECTRAL_DIFFERENTIATORS.md](SPECTRAL_DIFFERENTIATORS.md) rather than re-specifying them; here they appear only as the *socket/row editors* the material surfaces invoke.

---

## 0. The one-paragraph thesis

RISE already has the two hard pieces a material editor needs and most renderers fake: (1) a **descriptor-driven property system** ([ChunkDescriptor.h](../../src/Library/Parsers/ChunkDescriptor.h)) where every material parameter is already a typed, self-describing, editable row surfaced through the bridge as a `RISEViewportProperty` (panel mode `RISEViewportPanelModeMaterial = 6` is already wired), and (2) a **CPU spectral path tracer with no shader-compile step**, so every parameter is a live knob and a per-node preview is just another (small) render. The work is therefore an *interaction layer*, not an engine project. C1 is almost free — it is the existing material property rows given a first-class panel — **except for the painter-editing path** (the colour swatch/picker of §2.1), which is **TO-BUILD**: shipped Phase B does **not** cover `Painter` (only Camera/Light/Material/Medium + object bindings — CURRENT_STATE_AUDIT §1, `DirtyTracker` `EntityCategory`), so a *material's own* scalar-literal slot round-trips today but a *painter chunk's* value does not. C2's genuinely hard part is **serializing a node graph's *topology* — new chunks and re-wiring — into the descriptor-driven `.RISEscene` grammar** (per-slot *material-property* round-trip already ships via the SaveEngine Phase-B re-emit for the entity families above, audit §1; **painter-node** property round-trip is TO-BUILD; the rest — canvas widget, thumbnails, pipe-typed sockets — is bounded UI work). C3 is a one-way importer whose single load-bearing responsibility is **encoding the color-vs-scalar routing** that [../ISCALARPAINTER_REFACTOR.md](../ISCALARPAINTER_REFACTOR.md) exists to enforce.

---

## 1. The load-bearing concept: `IPainter` (color pipe) vs `IScalarPainter` (scalar pipe)

This distinction is the spine of socket typing (C2) and import routing (C3). It is not cosmetic — getting it wrong is the exact bug [../ISCALARPAINTER_REFACTOR.md](../ISCALARPAINTER_REFACTOR.md) was written to kill (a `scattering 1000000` or an IOR routed through `IPainter` gets JH-uplifted through the Jakob–Hanika LUT and silently mangled in every spectral rasterizer — glass renders invisible).

| | `IPainter` (color pipe) | `IScalarPainter` (scalar pipe) |
|---|---|---|
| **Carries** | A per-wavelength **color/spectrum** sample (reflectance, emission, tau-as-tint) | A per-wavelength **physical magnitude** (IOR, roughness, scattering, absorption, thickness, exponent, phase `g`) |
| **Colorspace?** | Yes — `GetColorNM` does JH spectral uplift via the Rec.709 LUT | **None.** `ScalarTriple` is three pure scalars; the interface deliberately has *no* `GetColorNM` |
| **Socket color (C2)** | **Yellow** (Blender convention for a 3-channel color) | **Grey** (Blender convention for a single value) |
| **Resolved by** | `IPainterManager` | `IScalarPainterManager` (parser helper `ResolveOrDiagnoseScalar`, [Job.cpp](../../src/Library/Job.cpp)) |

**The oddball: `tangent_rotation` (anisotropy rotation, radians).** It is an angle — a physical scalar by meaning — but it is plumbed through the **color pipe** (`IPainter`) so that an `expression_function2d` painter can drive a *spatially varying* groove direction (and so a bare scalar literal works too). The GGX descriptor for `tangent_rotation` explicitly notes "a `scalar_painter` does NOT bind here — use `expression_function2d` or a scalar" ([AsciiSceneParser.cpp](../../src/Library/Parsers/AsciiSceneParser.cpp), GGX descriptor). The editor must special-case it: present it as an **angle row** (degrees in the UI, radians in the file) and, in C2, draw it as a **yellow socket that accepts only color-pipe sources** even though its row editor is a scalar angle. Flag any future scalar-but-color-pipe slot the same way (this is the one today). This mismatch is itself the cautionary illustration for why C2 sockets must be typed from the *true pipe*, not guessed from the parameter's apparent meaning.

> **Subtlety the editor must respect (uncertainty flagged):** the parser *descriptor* does not currently encode the pipe distinction cleanly. The GGX `ior` / `film_ior` parameters are declared `ValueKind::Reference` with `referenceCategories = {Painter}` (workstream #2 dropped the phantom Function) — now identical to a true colour slot's `{Painter}` set — and the IPainter-vs-IScalarPainter routing happens *downstream* in `Job::ResolveOrDiagnoseScalar`, which emits the three-way diagnostic (per-channel-in-single-slot / legacy-IPainter-binding / unknown-name). So **the descriptor alone is necessary but not sufficient to color a socket.** C2 needs a per-slot pipe tag. The cheapest source of truth is the material's introspection getters, whose C++ *types* already carry it (`const IScalarPainter&` vs `const IPainter&` — see [GGXMaterial.h](../../src/Library/Materials/GGXMaterial.h) `GetIOR()` returns `IScalarPainter&`, `GetDiffuse()` returns `IPainter&`). See §6.4 for the **adopted `ParameterSemantics` structure** on the descriptor (whose `pipe` field colors the socket, with cardinality / `requireSingle` / units / colour space / spatial-vs-spectral as **separate sibling fields**, per [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §16) so the two sources can't drift.

---

## 2. C1 — Instance-first editor (the cheap 80%, no new serialization)

**What it is.** A flat, scrollable panel of slider / swatch / preset rows over a *single material instance's* existing slots — the Unreal "material instance" view (override a parameter without recompiling), except RISE has **no recompile cost at all** because it is a CPU path tracer: every edit is live the instant the progressive viewport restarts ([../GUI_ROADMAP.md](../GUI_ROADMAP.md) §3, §7). This is the default surface most users ever touch.

**Why it is nearly free.** It already exists in skeleton. The bridge has:
- `RISEViewportPanelModeMaterial = 6` and `RISEViewportCategoryMaterial = 6` ([RISEViewportBridge.h](../../build/XCode/rise/RISE-GUI/Bridge/RISEViewportBridge.h)).
- `propertySnapshotFor:(RISEViewportCategory)` / `setPropertyForCategory:name:value:` — per-category read + write that already routes a Material-section edit to the object's bound material even when an Object is the primary selection.
- `RISEViewportProperty` rows carrying `name / value / describing / kind (ValueKind) / editable / presets / unitLabel`.
- The thin-film material already surfaces its slots and a **read-only `fresnel_mode` row** through `MaterialIntrospection` (per [../THIN_FILM_INTERFERENCE.md](../THIN_FILM_INTERFERENCE.md) adversarial-review commit `48d99f04`).

So C1 is "promote the Material panel mode to a first-class, well-laid-out surface," not "build a property system."

### 2.1 What C1 adds on top of today's descriptor infra (little)

| Need | Today | C1 delta |
|---|---|---|
| Numeric **slider** with sane range | `kind = Double`, plain line edit + `unitLabel` | Add an optional `range`/`softRange` hint to `ParameterDescriptor` (min/max/step); render a slider when present, fall back to line edit. |
| **Color swatch** + picker | `Reference` → Painter (string name) | Detect a uniform color painter; show a swatch that opens the **spectral color picker** ([SPECTRAL_DIFFERENTIATORS.md](SPECTRAL_DIFFERENTIATORS.md) D1). Inline RGB literal stays editable as text. **⚠ Persistence is TO-BUILD** — this row *edits a painter chunk*, and shipped Phase B does **not** cover `Painter` (audit §1); see the note below. |
| **Preset** quick-picks | `presets` already on the descriptor | Populate material presets (roughness "Mirror/Satin/Matte"; IOR "Water 1.33 / Glass 1.5 / Diamond 2.42"; metal n,k named picks → [SPECTRAL_DIFFERENTIATORS.md](SPECTRAL_DIFFERENTIATORS.md) D2). |
| **Enum** rows | `kind = Enum` with `enumValues` | `fresnel_mode` (conductor / schlick_f0 / thinfilm) becomes an editable combo; switching to `thinfilm` reveals the film rows (progressive disclosure, see §2.2). |
| **Material swatch** at panel top | `panelHeader` string | A small async sphere/shaderball preview of the *whole* material (same renderer; §4 thumbnail engine, reused). |
| **Assign / duplicate** | object→material binding exists | "Duplicate material" (clone the chunk under a new name) + drag-to-assign hook ([../GUI_ROADMAP.md](../GUI_ROADMAP.md) A3). Duplicate is the instance-first idiom: branch a named material, tweak its knobs. |

> **⚠ Painter editing is TO-BUILD — not shipped (audit §1; [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §13a #7).** The §2 framing "C1 is the existing rows given a panel, *no new serialization*" holds **only for a material's own slots** (scalar literals, enums) that round-trip through the SaveEngine Phase-B re-emit for the tracked entity families (Camera/Light/Material/Medium + object bindings). It does **NOT** hold for the **Color swatch + picker** row above, which edits a *painter* chunk: `Painter` is **not** an `EntityCategory` in `DirtyTracker`, is not introspected, and is not re-emitted on save (`SaveEngine.cpp`). Making the colour picker (and any painter-value edit) persist therefore needs **four net-new pieces, all TO-BUILD**:
> 1. **`PainterIntrospection`** — a read/edit surface for painter chunks (there is none today; only `MaterialIntrospection` exists, and it surfaces a painter *binding name*, not the painter's own value).
> 2. **Painter edit operations** — `SceneEdit` ops to set a painter's value (no `Set*PainterProperty` op exists; the only painter touch is binding a material slot to a painter *name*).
> 3. **Painter dirty tracking** — add `Painter` to `DirtyTracker`'s `EntityCategory` so an edited painter is flagged for save.
> 4. **Save re-emit for painters** — extend the SaveEngine Phase-B pass to diff + Mode-A-splice painter chunk values (today painters are not in the Phase-B dispatch).
>
> Until these land, the colour picker is a **live-viewport-only** edit (it restarts the render, like any in-memory change) that **does not survive save/reload**. C1 still ships its non-painter rows on today's infra; the painter rows are the TO-BUILD delta.

### 2.2 Progressive disclosure inside C1

Per principle 4 (≤2 levels). Each material shows a **Basic** band (the 4–6 parameters that matter — base color, roughness, metalness/IOR) and an **Advanced** disclosure (anisotropy, extinction, film slots, SSS coefficients). The `fresnel_mode` combo is the gate: `thinfilm` reveals `film_ior / film_extinction / film_thickness` (which `GetFresnelMode()` already lets the panel show only for a thin-film material); `schlick_f0` hides `ior`/`extinction`. No node graph is ever required for a good result (avoid the Houdini trap, §12).

### 2.3 C1 non-negotiables (anti-patterns avoided)

- **No synchronous swatch stalls.** Maya's Hypershade is the cautionary tale — users report ~10 s *per swatch* and ~30 s to open the window ([Autodesk forum](https://forums.autodesk.com/t5/maya-forum/super-slow-hypershade-in-maya-2022-3-solution/td-p/11132437), [Chaos forum](https://forums.chaos.com/forum/chaos-common/chaos-common-public/29969-extremely-slow-swatch-generation-in-the-hypershade)). The material swatch is async + cancellable + cached, never blocking the edit (§4).
- **One inspector, not mode-swapped panels** (§12; Maya anti-pattern). C1 is *the* material view; C2 is a toggle from it, not a parallel world.

---

## 3. C2 — Node graph (heavy lift, gated "Advanced")

A visual node canvas for users who outgrow flat instance knobs (layered/composite materials, painter graphs, spatially varying inputs). **Gated behind an explicit "Advanced" toggle, never on the default path** ([../GUI_ROADMAP.md](../GUI_ROADMAP.md) §12; the KeyShot model — graph is opt-in/Pro, drag-a-material is the default). RISE's advantage over Unreal/Blender is structural: **no shader permutation/recompile** ([Epic docs](https://dev.epicgames.com/documentation/unreal-engine/instanced-materials-in-unreal-engine); [Hoffman, shader permutations](https://medium.com/@lordned/unreal-engine-4-rendering-part-5-shader-permutations-2b975e503dd4)) and Substance's signature per-node live thumbnail comes **for free** because the renderer *is* the preview ([Adobe, Designer 2020.2 thumbnails](https://www.cgchannel.com/2020/10/adobe-unveils-substance-designer-2020-2/)).

### 3.1 Node taxonomy

Nodes are 1:1 with RISE's existing chunk vocabulary so the graph round-trips to text (§5). Categories:

| Node category | Maps to | Examples | Output pipe |
|---|---|---|---|
| **Surface / uber** (the default node) | a `*_material` chunk | **OpenPBR-shaped uber node** (§3.2) → resolves to GGX + dielectric + thin-film; plus first-class `lambertian`, `dielectric`, `ggx`, `sheen`, SSS nodes | Material |
| **Layer / composite** | `composite_material` | Top/Bottom + thickness + extinction (varnished wood, metal-flake-under-clearcoat) | Material |
| **Color painter** | `IPainter` chunks | `uniformcolor`, `spectral` (→ [SPECTRAL_DIFFERENTIATORS.md](SPECTRAL_DIFFERENTIATORS.md) D1), `blackbody`, `texture`, `checker`, `blend`, procedural noise, `expression_function2d` | **Color (yellow)** |
| **Scalar painter** | `IScalarPainter` chunks | `uniform`, `rgb`, `piecewise_linear` (measured n/k drop-in), `sellmeier` (named-glass IOR), `polynomial`, `texture_scalar`, `scaled`, `multiply` | **Scalar (grey)** |
| **Texture / input** | image + UV | `texture` / `texture_scalar`, `uv_transform`, `tex_coord` | Color or Scalar by slot |
| **Bridge** | `PainterToScalarAdapter` | grey-input from a color graph (PBR-MR anisotropy chains); explicit, see §3.5 | Scalar (grey) |

**`tangent_rotation`** is the documented oddball: a **yellow** socket (color pipe) whose row editor is an angle (§1).

### 3.2 The default node = an OpenPBR-shaped uber-material

The node you get when you create a material with no other choice. Its parameter layout follows **OpenPBR Surface** ([Academy Software Foundation OpenPBR](https://academysoftwarefoundation.github.io/OpenPBR/)) — the industry's converging uber-shader — so authors arriving from Blender Principled / Arnold Standard Surface / Substance feel at home, but it **resolves down to RISE's actual materials** (GGX `fresnel_mode {conductor|schlick_f0|thinfilm}` + `dielectric_material` for transmission + the thin-film film slots), exactly as `pbr_metallic_roughness_material` already resolves at scene-build time into a painter graph + a single GGX ([MATERIALS.md](../MATERIALS.md) §8). The uber node is a **front-end convenience that lowers to existing chunks** — not a new C++ material.

OpenPBR-input → RISE mapping (the C2 default node's socket list). Pipe column drives socket color:

| OpenPBR input | OpenPBR type | RISE target | RISE pipe / socket |
|---|---|---|---|
| `base_color` | color3 | GGX `rd` (diffuse) / baseColor | **IPainter — yellow** |
| `base_metalness` | float | routes to GGX `fresnel_mode` + F0=baseColor (the PBR-MR lowering) | **IScalarPainter — grey** |
| `specular_weight` | float | `specular_factor` (KHR_materials_specular) | IScalarPainter — grey |
| `specular_color` | color3 | GGX `rs` / `specular_color` tint | **IPainter — yellow** |
| `specular_roughness` | float | GGX `alphax`/`alphay` (roughness²) | IScalarPainter — grey |
| `specular_roughness_anisotropy` | float | `anisotropy_factor` → αx≠αy | IScalarPainter — grey |
| `specular_ior` | float | GGX `ior` *(scalar!)* | **IScalarPainter — grey** |
| (anisotropy rotation) | float | `tangent_rotation` *(radians)* | **IPainter — yellow (oddball, §1)** |
| `transmission_weight` | float | lower to `dielectric_material` branch | IScalarPainter — grey |
| `transmission_color` | color3 | dielectric `tau` (tint) | **IPainter — yellow** |
| `transmission_dispersion_abbe_number` / `_scale` | float | dielectric dispersive IOR (`sellmeier`/per-channel scalar) | IScalarPainter — grey · **spectral superset, see §3.6** |
| `subsurface_weight` / `_radius` / `_anisotropy` | float | `subsurfacescattering` / `randomwalk_sss` slots | IScalarPainter — grey |
| `subsurface_color` / `subsurface_radius_scale` | color3 | SSS albedo / radius tint | IPainter — yellow |
| `coat_weight` / `coat_roughness` | float | `composite_material` clearcoat layer | IScalarPainter — grey |
| `thin_film_thickness` | float (µm) | GGX `film_thickness` *(nm in RISE — unit convert ×1000)* | **IScalarPainter — grey** |
| `thin_film_ior` | float | GGX `film_ior` | IScalarPainter — grey |
| `thin_film_weight` | float | gates `fresnel_mode thinfilm` | IScalarPainter — grey |
| `geometry_normal` / `geometry_opacity` | vector / float | normal map painter / alpha | per-slot |

Notes the editor must encode:
- **Unit conversion is real.** OpenPBR `thin_film_thickness` is **µm**; RISE `film_thickness` is **nm** ([../THIN_FILM_INTERFERENCE.md](../THIN_FILM_INTERFERENCE.md)). The uber node converts (×1000) and labels the row "nm".
- **`base_metalness`/`transmission_weight` are *routing* knobs**, not direct slots — they pick which RISE material(s) the node lowers to (metal → GGX conductor with F0=baseColor; transmission → dielectric). This is the same lowering `Job::AddPBRMetallicRoughnessMaterial` already does; the node graph just exposes the dials.

### 3.3 Per-node live thumbnails (RISE path-traces for free; async; disableable)

The signature feature. Each node renders a small preview of its output on a shaderball (surface nodes) or a swatch tile (painter/scalar nodes), using the same engine — Substance's per-node thumbnail, except we don't bolt on a separate PBR-render node ([CG Channel](https://www.cgchannel.com/2020/10/adobe-unveils-substance-designer-2020-2/)).

Design rules, all from the **Hypershade anti-pattern** (icon-less nodes, synchronous swatch stalls, manual recompute):
- **Async + cancellable.** A node-preview render runs on a worker at low spp / tiny resolution, off the interactive path; navigating or editing cancels in-flight previews. Never block an edit (the Maya 10 s/swatch failure).
- **Cached + invalidation-driven.** Cache keyed on the node's resolved parameters + upstream graph hash; recompute only the dirty subtree on edit (avoid Substance's "only computes on first open / needs manual *Compute Node Thumbnails*" footgun — [Adobe community](https://community.adobe.com/t5/substance-3d-designer-discussions/hotkey-for-compute-node-thumbnails/td-p/12363560)).
- **Globally disableable + budgeted.** A "Live previews" toggle and a max-concurrent-preview budget; on a big graph, previews degrade gracefully (stale thumbnail kept, "↻" affordance) rather than stalling. Reuses the **same thumbnail engine** as the C1 material swatch and the asset library (§4).
- **Spectral-honest.** Previews are real spectral renders, so a soap-bubble thin-film node *looks* iridescent and a dispersive-glass node *shows* color fringing in its thumbnail — impossible in an RGB tool. (Whether a *hover-preview* during drag is cheap enough is the open spike in [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §13.1 — default to commit-on-release if not.)

### 3.4 Socket color = pipe; connection legality from descriptor metadata

- **Socket color encodes the pipe** (§1): **yellow = `IPainter`**, **grey = `IScalarPainter`**, **green = Material** output (Blender's convention: grey = 1 value, yellow = color/3-value, green = shader — [Blender shader sockets](https://docs.blender.org/manual/en/latest/render/shader_nodes/shader/principled.html)).
- **Connection legality is enforced from existing metadata, not invented.** A drag from an output to an input is legal iff the input slot's pipe accepts the output's pipe. The truth comes from two existing sources that must be reconciled (§6.4): the descriptor's `referenceCategories` (Painter / Function / Material) *and* the per-slot pipe tag (IPainter vs IScalarPainter). The validation reuses the **same `ResolveOrDiagnoseScalar` logic** that the parser already runs — so the canvas rejects a color-painter-into-scalar-slot drop with the *same* message a hand-edit would get ("bound to `IPainter` chunk … this slot now requires a `scalar_painter`"). One validation authority, three consumers (parser, GUI canvas, MCP `validate`).
- **The bridge node** (`PainterToScalarAdapter`) is the *only* sanctioned way to feed a grey slot from a yellow graph; the canvas offers to insert it automatically when the user attempts an otherwise-illegal color→scalar drop *and* the intent is a grayscale spatial input (PBR-MR anisotropy pattern, [../ISCALARPAINTER_REFACTOR.md](../ISCALARPAINTER_REFACTOR.md) Phase 4). It must **warn** that the adapter does not protect against JH uplift on a *spectral* input — it is for spatially-varying grayscale only.

### 3.5 Organization from day one (frames / comments / search)

Not deferred — the lesson from every node editor that grew unmanageable (Maya Hypershade auto-destroys layout; Substance only recently added node Groups for search — [Substance 15.1 cleanup](https://digitalproduction.com/2025/12/15/substance-designer-cleans-up-its-nodes/), [graph instances/subgraphs](https://helpx.adobe.com/substance-3d-designer/substance-compositing-graphs/graph-instances-sub-graphs.html)):
- **Frames** (labeled, colored bounding boxes grouping related nodes) and **sticky comments**.
- **Search/add palette** (type-to-add, filtered by what can legally connect to the selected socket).
- **Node groups / subgraphs** — collapse a reusable cluster (this is also the natural serialization unit for a *named reusable material*, §5).
- **Deterministic layout persistence.** Node positions/frames are **UI/session state**, not scene semantics — stored in a sidecar layout file or a comment-block annotation (honors the §1 "prefer read-only / don't extend the scene language for UI state" instinct, and the [round-trip prefer-read-only](../../docs/) memory). Never auto-destroy a user's layout (the Hypershade sin).

### 3.6 Spectral is a superset of OpenPBR — surfaced, not hidden

The C2 uber node *defaults* to the OpenPBR RGB-shaped inputs, but each color/scalar socket can be **promoted to its spectral form** in place — a color swatch → a spectral curve / measured SPD ([SPECTRAL_DIFFERENTIATORS.md](SPECTRAL_DIFFERENTIATORS.md) D1), an IOR float → a `sellmeier` named-glass node or a measured n,k `piecewise_linear` ([SPECTRAL_DIFFERENTIATORS.md](SPECTRAL_DIFFERENTIATORS.md) D2), thin-film thickness → the angle-reactive thin-film row ([SPECTRAL_DIFFERENTIATORS.md](SPECTRAL_DIFFERENTIATORS.md) D3). OpenPBR's `transmission_dispersion_abbe_number` is the seam: RISE expresses true per-wavelength dispersion (a `sellmeier`/per-channel scalar) that the RGB interchange can only approximate. This is the "differentiate on spectral, don't replicate" mandate realized inside the node editor.

### 3.7 The hard part — new scene-language graph **topology** serialization ("Phase B")

**Material-entity** *property* round-trip is **already shipped** (SaveEngine Phase-B re-emit, audit §1) — editing a *material node's* own slot value (a scalar literal, an enum) persists today. **Two caveats narrow this:** (1) **painter-node** property round-trip is **TO-BUILD** — `Painter` is not a tracked `EntityCategory` and painter values are not re-emitted (§2.1 note; audit §1; [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §13a #7), so editing a painter node's value does *not* persist yet; (2) graph **topology** is the larger genuinely-new work: there is no general material-*graph* serialization that survives a save/reload of brand-new nodes and re-wired **edges** (which painter feeds which slot of which material). Two layers:

**Phase A — read-only graph view (no new serialization).** Build the canvas as a *visualizer* over the already-parsed scene: every `*_material` chunk and every painter/scalar-painter chunk becomes a node; every name-reference (`rs ggx_specular`, `film_thickness oxide_thk`) becomes an edge. This is pure introspection over the existing chunk graph + a new bridge call returning `{nodes, edges}`. Ships value (see your material's structure visually, jump to a node) before any serialization risk. Edits in Phase A still go through the C1 per-slot `setProperty` path.

**Phase B — graph mutation + round-trip serialization.** The editor creates/deletes nodes and rewires edges, and that must serialize. **The chunk vocabulary already IS the serialization** — RISE materials and painters are named, reference-by-name chunks, so a graph is *already* expressible as a set of chunks + name-references. The design therefore is **NOT a new `node`/`graph` chunk type** (that would fork the format and fight the descriptor-driven parser). Instead:

- **Serialize the graph AS the chunks it already lowers to.** Creating a node = emitting its `*_material` / `*_painter` chunk with a generated unique name; wiring an edge = writing that name into the consumer slot. Round-trip save ([../ROUND_TRIP_SAVE_PLAN.md](../ROUND_TRIP_SAVE_PLAN.md)) is the engine that rewrites these chunks in place, preserving comments / whitespace / `FOR`-loops (so a hand-authored material the user opens in the graph and tweaks doesn't get clobbered). **What is already shipped vs. what is genuinely new** (audit §1, punch-list Theme 1; [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §13a #7): the SaveEngine's **Phase-B property re-emit** round-trips edits to a **material entity's** *existing* slot values (Mode-A splice, diffed vs the parse-time snapshot) and is shipped and tested — so editing a *material node's* parameter in the graph persists today through the same path C1's per-slot `setProperty` uses. **But Phase B does NOT cover `Painter`** (the tracked families are Camera/Light/Material/Medium + object bindings — audit §1), so editing a *painter node's* value does **not** persist; painter-property round-trip is **TO-BUILD** (the four pieces in the §2.1 note: `PainterIntrospection` + painter edit ops + painter dirty tracking + painter save re-emit). The other **genuinely new** work is graph-**topology** serialization: emitting brand-new `*_material`/`*_painter` chunks for added nodes and rewriting reference tokens for re-wired edges. **C2 Phase B therefore depends on graph-topology emit + reference-rewrite + the TO-BUILD painter-property path, NOT on "round-trip save landing"** (the material-entity half has landed). Where a topology edit can't yet be expressed as a surgical chunk insert/reference-rename, the graph editor falls back to rewriting the affected material's **ownership closure** — defined precisely in §3.7a — never to "the material block," because arbitrary scenes have no contiguous block boundary (graph chunks may be scattered, shared between materials, or `FOR`/`DEFINE`-generated). When that closure is ambiguous, the editor **REFUSES** the topology edit with a clear diagnostic rather than guessing a block to clobber.

#### 3.7a — The ownership-closure rewrite rule (what "wholesale" actually means)

"Rewrite the affected material block wholesale" is **undefined** for real scenes: a material's painter graph is a set of named, reference-by-name chunks that the scene file can lay out *anywhere* — adjacent, scattered across the file, **shared** with another material's graph (one painter referenced from two materials, §3.8 copy-vs-link), or **macro/`FOR`-generated** (no single editable source line). There is no guaranteed contiguous text span to "rewrite wholesale." The fallback must therefore be scoped by *ownership*, not by text adjacency:

1. **Compute the ownership closure.** Using the controller's dependency graph (the same `SceneEditController::FindReferencesTo` query and on-demand reference scan that reference-safe deletion relies on — [ENTITY_CREATION.md](ENTITY_CREATION.md) §5, §5.1), walk the chunks reachable from the edited material's roots and keep exactly the set that is **solely owned** by this material's graph: reachable from this material, **not shared** with any other material/object/light (no second referrer), and **not macro/`FOR`-generated** (has a single editable source span the SaveEngine can address — audit §1's FOR-generated Refusal condition). That solely-owned, single-source, unshared set is the closure.
2. **Rewrite only the closure.** Re-emit precisely those chunks (and the consumer reference tokens that point into them); leave every other byte of the file — including shared painters, neighbouring unrelated chunks, and `FOR`/`DEFINE` regions — untouched. This preserves the byte-identity guarantee everywhere outside the closure.
3. **REFUSE when the closure is ambiguous.** If the edit would require rewriting a chunk that is **shared** (≥2 referrers), **scattered across a span the engine can't address as a unit**, or **macro/`FOR`-generated**, the editor returns a `Status::Refused`-shaped diagnostic ("this topology edit touches a painter shared with material *X* / generated by a `FOR` loop — edit the source, or duplicate the node to fork an owned copy") rather than rewriting a guessed block and risking clobbering another material or a macro region. This is the same block-or-refuse discipline ENTITY_CREATION applies to reference-safe deletion (§5.2) and the same FOR-generated Refusal the SaveEngine already enforces for property edits (audit §1).

The user's escape hatch when REFUSED is **"Duplicate node"** (§3.8 copy-vs-link): forking the shared painter into a new, solely-owned chunk makes the closure unambiguous, after which the topology edit proceeds. This keeps the fallback *safe-by-default* — it never silently rewrites a span it doesn't fully own.

The remaining Phase-B mechanics (resuming the §3.7 list):

- **Canvas layout is a sidecar** (§3.5) — never in the semantic chunks.
- **The reconciliation problem (the genuinely new work):** mapping graph edits ↔ minimal chunk-text edits. Adding a node between two existing ones must (a) emit a new chunk, (b) rename one reference, (c) leave everything else byte-identical. This is the same "structured edit vs wholesale rewrite" question the roadmap flags as an open spike ([../GUI_ROADMAP.md](../GUI_ROADMAP.md) §13.6). **Recommendation:** Phase B uses round-trip save's structured-edit path for *single-slot* rewires (cheap, surgical) and falls back to the **ownership-closure rewrite** (§3.7a — re-emit only the solely-owned, single-source, unshared chunks; REFUSE when the closure is ambiguous) for *topology* changes (add/remove node), which are rarer and where preserving inter-line formatting matters less. The fallback is never "rewrite the material block" — that boundary doesn't exist in an arbitrary scene.

> **Open question (flagged):** whether a **named reusable material library** (a material graph saved once, instanced across scenes — the Unreal master/instance split at the *file* level) needs a light wrapper chunk (e.g. an `#include`-style material import) or is served by copy-in + the C3 importer. Lean toward **no new chunk**; a reusable material is a `.RISEscene` fragment the asset library copies in (consistent with §12 "MaterialX not native, import only" and the no-new-format instinct). Decide in Phase B.

### 3.8 Graph-model completeness — what "the chunks ARE the graph" leaves unspecified

The "named `*_material`/`*_painter` chunks ARE the graph" model (§3.7) is the right *serialization*, but it is **not a complete editing model**. The chunk vocabulary expresses a DAG of name-references; it says nothing about how the editor handles shared nodes, cycles, deletion, generated names, macro-expanded graphs, reusable clusters, or edits whose text spans are scattered. Each is specified here so the canvas behaves predictably and `Phase B` knows what it must reject.

- **Shared-node copy-vs-link semantics.** A painter referenced by name from two material slots is **one node with two out-edges** (a *link*), not two nodes — RISE chunks are reference-by-name, so the default on the canvas is **link** (edit once, both consumers change). Dragging a node's output to a second input *links*; the user must explicitly choose **"Duplicate node"** to fork an independent copy (a new chunk with a deduped name, §3.8 stable-names). This mirrors ENTITY_CREATION §4.3: object duplicate **shares** geometry/material refs by addref (not deep-copy) — "clone Sponza's wall material 1000×" is never the intent. Surface the share visually (a fan-out badge on a multiply-referenced node) so an "edit once" doesn't silently change a node the user thought was private.
- **Cycle detection / prevention.** The chunk graph **must stay acyclic** — the parser resolves names at chunk-finalize time and the renderer evaluates painters by walking references; a cycle (painter A blends B, B blends A) is non-terminating. The canvas rejects any wire whose addition would create a cycle (a forward reachability check from the proposed target back to the source before committing the edge), with the same `Diagnostic` shape the validator uses, so a hand-edited cyclic scene and a canvas-attempted cycle report identically. This is a *graph* invariant the flat-chunk format can express but never enforces — the editor owns enforcing it.
- **Deletion / orphan handling.** Deleting a node is exactly the **reference-safe deletion** problem ENTITY_CREATION §5 owns, and its sharpest hazard applies here: **objects (and material slots) hold raw `const` pointers to their painters with manual addref/release and do NOT register through the manager's `RequestItemUse` deletion-callback path** ([ENTITY_CREATION.md](ENTITY_CREATION.md) §5; [Object.cpp](../../src/Library/Objects/Object.cpp) `AssignMaterial`/`AssignGeometry`). So deleting a painter that a still-bound slot references does **not** dangle to freed memory (the addref keeps refcount ≥ 1) but **orphans** it — gone from the manager and the file, still pointed at, un-re-bindable. The material node editor therefore must **compute the reference graph itself** before a delete (`SceneEditController::FindReferencesTo`, ENTITY_CREATION §5.1) and apply the **block-or-cascade** policy (block a painter delete that has consumers; offer reassign-to-neutral or cascade-clear) — it never silently removes a referenced node. Deletion is a transaction (TRANSACTION_MODEL); the whole cascade (rebinds + removal) is **one composite** so undo restores the node *and* re-wires every consumer atomically.
- **Stable generated names.** Created nodes need durable, predictable names so the serialized graph re-opens to the same topology and round-trips byte-stably. Reuse the **exact dedup algorithm** `CloneActiveCamera` uses, generalized to `UniqueName(IManager&, base)` ([ENTITY_CREATION.md](ENTITY_CREATION.md) §6.1) — `material_1`, `painter_1`, … lowest free index, run **under the same lock as the add** so two concurrent creates can't collide. A generated name is stable for the session and persists verbatim on save; the user may rename, but a *rename of a file-authored* node is Refused by the save engine in V1 (it would have to rewrite the declaration *and* every reference site, ENTITY_CREATION §6.3) — surface that limit in the rename affordance rather than silently clobbering references.
- **Macro / `FOR`-generated graphs are read-only.** A material or painter emitted inside a `FOR` loop or a `DEFINE` macro has no single editable source line — the same reason the SaveEngine **Refuses** a property edit on a FOR-generated entity (audit §1, "FOR-generated / chunk-revisited entity property edit"). The canvas shows such nodes (Phase A introspection sees the expanded result) but marks them **read-only** with a "generated by `FOR`/macro — edit the source loop" affordance; it never attempts a structured edit against them. This honors the parser-quirks reality (FOR/DEFINE expansion) without pretending the graph is freely mutable.
- **Reusable subgraphs.** A collapsed node-group (§3.5) is the natural unit for a *named reusable material* (the §3.7 open question). It serializes as the cluster of chunks it contains; **linking** the same subgraph into two places follows the copy-vs-link rule above (one cluster, multiple consumers) unless the user duplicates it. A cross-scene reusable library leans **copy-in** (a `.RISEscene` fragment the asset library pastes, no new chunk type — §12 non-goal) rather than an `#include`-style wrapper; decide in Phase B.
- **Topology edits spanning non-contiguous source.** Inserting a node "between" two existing ones, or re-wiring an edge, can touch **scattered, non-adjacent** lines — emit a new chunk in one place, rewrite a reference token elsewhere, possibly across the *declaration-order dependency* (a referent must be declared before its referrer, ENTITY_CREATION §7.3 topological-order rule). The Phase-B reconciliation (§3.7) handles single-slot rewires surgically (Mode-A-style splice — shipped) and falls back to the **ownership-closure rewrite** (§3.7a) when the edit spans non-contiguous source or crosses declaration order — re-emitting only the solely-owned, single-source, unshared chunks and **REFUSING** when the closure is ambiguous (shared / scattered-beyond-addressable / macro-generated), never blindly rewriting a "material block" that has no contiguous boundary in an arbitrary scene. This is the GUI analogue of the structured-edit-vs-wholesale spike ([../GUI_ROADMAP.md](../GUI_ROADMAP.md) §13.6).

All seven are **graph-level invariants the flat-chunk serialization can represent but cannot enforce** — they live in the shared graph model (§7), not in the scene format. Every graph mutation is a transaction ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md)); create/delete and reference-safe deletion route through the ops ENTITY_CREATION defines.

---

## 4. Shared thumbnail / preview engine (used by C1 swatch, C2 node thumbnails, asset library)

One engine, written once in the shared library, consumed by all three surfaces and by the asset library ([../GUI_ROADMAP.md](../GUI_ROADMAP.md) A5):
- Input: a material name (or a transient material), a preview scene (shaderball + studio HDRI + Lambertian floor), a budget (resolution, spp, deadline).
- Renders via the existing progressive integrator (Auto/PT) into an off-screen film; returns an HDR tile + a tonemapped preview.
- **Async, cancellable, cached, budgeted** (§3.3). Cache invalidation keyed on resolved-parameters hash. OIDN on for the final swatch, off/low during scrub (consistent with the "denoise on for finals" rule).
- This is the antidote to every Hypershade swatch complaint and the enabler of Substance-grade live thumbnails — and it is *generic* (a material → an image) so it serves C1, C2, and the library identically.

---

## 5. C3 — MaterialX / OpenPBR import (Phase 2 of Direction C)

**One-way import only. Native storage stays RISE's spectral superset** ([../GUI_ROADMAP.md](../GUI_ROADMAP.md) §3, §12). Adopting MaterialX as RISE's *storage* format would cap RISE at RGB and forfeit its reason to exist — MaterialX has **no spectral type** (the spec says color "is considered to represent a linear RGB color space," and only notes a renderer *may* reinterpret it — [MaterialX PBR spec](https://github.com/AcademySoftwareFoundation/MaterialX/blob/main/documents/Specification/MaterialX.PBRSpec.md)), and **dispersion is unimplemented in the reference backends** (even Unreal Substrate omits OpenPBR transmission dispersion — [4D Pipeline](https://blog.4dpipeline.com/customer-insights-where-are-openpbr-and-material-x-supported); [OpenPBR site](https://academysoftwarefoundation.github.io/OpenPBR/)). RGB-at-the-boundary / spectra-internal is the PBRT/Mitsuba posture RISE already takes via the JH LUT.

### 5.1 The importer's single load-bearing job: encode color-vs-scalar routing

This is the exact bug class [../ISCALARPAINTER_REFACTOR.md](../ISCALARPAINTER_REFACTOR.md) documents, re-arising at the import boundary. The naive rule — "MaterialX `color3` → IPainter, MaterialX `float` → IScalarPainter" — is **almost right but has a documented trap**:

| MaterialX input | MaterialX type | Naive routing | **Correct RISE routing** |
|---|---|---|---|
| `base_color`, `specular_color`, `transmission_color`, emission | `color3` | IPainter (JH LUT uplift) | ✅ IPainter — `color3`→`uniformcolor_painter` (or `texture`); JH-uplifted on the spectral path |
| `specular_roughness`, `coat_weight`, `thin_film_thickness`, dielectric `ior` | `float` | IScalarPainter | ✅ IScalarPainter — `float`→`scalar_painter { value … }` (no colorspace) |
| **`conductor_bsdf` `ior` / `extinction`** | **`color3`** | ❌ IPainter (WRONG — would JH-uplift a complex refractive index) | ✅ **IScalarPainter** — RISE's GGX `ior`/`extinction` are scalar slots; route the per-channel `color3` to an `rgb` scalar painter, **never** through the color pipe |

The third row is why **the importer must route on the RISE *destination slot's* pipe, not on the MaterialX *source* type.** MaterialX types a metal's complex IOR as `color3` ([MaterialX spec: `conductor_bsdf` uses two `color3` inputs `ior` and `extinction`](https://github.com/AcademySoftwareFoundation/MaterialX/blob/main/documents/Specification/MaterialX.PBRSpec.md)); RISE types it as `IScalarPainter` ([GGXMaterial.h](../../src/Library/Materials/GGXMaterial.h)). Routing a metal's `color3` ior through `uniformcolor_painter` would JH-uplift it and mangle the Fresnel — the identical failure mode the refactor killed for inline scalars. **The importer carries the same pipe table as §1/§3.4 and consults it per *RISE slot*.** Concretely it should call the same `ResolveOrDiagnoseScalar`/manager split the parser uses, so an importer mis-route produces the same diagnostic rather than a silent wrong render.

### 5.2 Import mapping (OpenPBR/MaterialX node graph → RISE)

- **OpenPBR Surface node → RISE uber-material lowering** (§3.2 table, run in reverse): `base_metalness`/`transmission_weight` decide whether to emit GGX-conductor / GGX-schlick / `dielectric_material` / `composite_material`.
- **`color3` constants/textures → `uniformcolor_painter` / `texture`** (yellow pipe).
- **`float` constants/textures → `scalar_painter { value }` / `texture_scalar`** (grey pipe).
- **Thin-film** (`KHR_materials_iridescence` / OpenPBR `thin_film_*`) → GGX `fresnel_mode thinfilm` + `film_*` slots (µm→nm convert). RISE's thin-film already aligns ~1:1 with the glTF/OpenPBR model ([../THIN_FILM_INTERFERENCE.md](../THIN_FILM_INTERFERENCE.md) §4).
- **Dispersion** (`transmission_dispersion_abbe_number`) → a `sellmeier` scalar painter (RISE *exceeds* the source here; the Abbe number seeds a Sellmeier fit, or maps to a named-glass pick).
- **Unsupported MaterialX nodes** → a parse-time diagnostic listing the dropped node, never a silent skip (same posture as the parser's unknown-parameter hard-fail).

### 5.3 Import is reuse, not a parallel path

C3 leans on the existing **glTF importer** precedent — `GLTFSceneImporter` already lowers PBR-metallic-roughness + `KHR_materials_transmission` + iridescence into RISE chunks and already learned the IScalarPainter lesson (it switched to inline numeric literals for dielectric scalar slots after Phase 3 broke its `UniformColorPainter` round-trip — [../ISCALARPAINTER_REFACTOR.md](../ISCALARPAINTER_REFACTOR.md) Phase 3). C3 is "a MaterialX front-end that emits the same chunk stream," in the shared library, behind the same `IJob::Add*Material` surface.

---

## 6. The IPainter / IScalarPainter mapping table (editor concept → internal type → painter class)

The canonical table the editor (rows + sockets), the serializer, and the importer all consult. **Concept** is what the user sees; **pipe** drives socket color and import routing; **painter class** is what gets emitted.

| Editor concept (row/socket) | Internal pipe | Painter class (chunk) | Notes |
|---|---|---|---|
| Base/diffuse color, specular tint, transmission tint, emission | `IPainter` (yellow) | `UniformColorPainter` (`uniformcolor_painter`) | JH-uplifted on spectral path |
| Spectral color curve / measured SPD | `IPainter` (yellow) | `SpectralColorPainter` (`spectral_painter`) | → [SPECTRAL_DIFFERENTIATORS.md](SPECTRAL_DIFFERENTIATORS.md) D1 |
| Blackbody / Kelvin color | `IPainter` (yellow) | `BlackBodyPainter` (`blackbody_painter`) | temperature → SPD |
| Image texture (color) | `IPainter` (yellow) | `TexturePainter` (`texture`) | sRGB-decoded |
| Procedural color (checker/noise/expression) | `IPainter` (yellow) | `CheckerPainter`, Perlin/Worley/Voronoi, `ExpressionFunction2DPainter` | |
| Blend of two colors | `IPainter` (yellow) | `BlendPainter` | PBR-MR uses these chains |
| **Anisotropy rotation (radians)** | **`IPainter` (yellow) — oddball** | `expression_function2d` or inline scalar | §1; angle row, color pipe; `scalar_painter` does NOT bind |
| Roughness / metalness / weight / coat / SSS weight | `IScalarPainter` (grey) | `UniformScalarPainter` (`scalar_painter { value }`) | no colorspace |
| Per-channel scalar (RGB-authored IOR/roughness) | `IScalarPainter` (grey) | `RGBScalarPainter` (`scalar_painter { values r g b }`) | rejected by single-scalar slots via `requireSingle` |
| Measured IOR/extinction n,k curve | `IScalarPainter` (grey) | `PiecewiseLinearScalarPainter` (`scalar_painter { file … }`) | refractiveindex.info drop-in; → [SPECTRAL_DIFFERENTIATORS.md](SPECTRAL_DIFFERENTIATORS.md) D2 |
| Named-glass IOR (BK7/SF11/diamond) | `IScalarPainter` (grey) | `SellmeierScalarPainter` (`scalar_painter { sellmeier … }`) | → [SPECTRAL_DIFFERENTIATORS.md](SPECTRAL_DIFFERENTIATORS.md) D1 |
| Polynomial scalar(λ) | `IScalarPainter` (grey) | `PolynomialScalarPainter` (`scalar_painter { polynomial … }`) | |
| Scalar texture / map (roughness map, thickness map) | `IScalarPainter` (grey) | `TextureScalarPainter` (`scalar_painter { file <img> }`) | grayscale UV |
| Scaled / multiplied scalar | `IScalarPainter` (grey) | `ScaledScalarPainter` / `MultiplyScalarPainter` | spatial × spectral compose |
| Function-wrapped scalar | `IScalarPainter` (grey) | `Function1DScalarPainter` / `Function2DScalarPainter` | wraps `IFunction1D/2D` |
| **Color graph → scalar slot (grayscale spatial)** | **bridge** | `PainterToScalarAdapter` | §3.4; warns: no protection vs JH uplift on a *spectral* input |
| Whole surface (the node output) | Material (green) | `*_material` chunk | GGX / dielectric / composite / SSS / luminaire |

### 6.4 The adopted descriptor field — a `ParameterSemantics` structure with separate fields

The **`ParameterSemantics` structure on `ParameterDescriptor` is ADOPTED** ([../GUI_ROADMAP.md](../GUI_ROADMAP.md) §16), not proposed — and it is deliberately **a structure with separate fields, NOT a single overloaded `pipe` enum** (the review's correction, recorded in §16). The `pipe` field alone is too weak to validate a connection or generate a faithful schema, so the cardinality / `requireSingle` / units / colour-space / spatial-vs-spectral constraints are **sibling fields on the same structure**, not values folded into the `pipe` discriminator. `ParameterSemantics` is the declared source of truth for socket coloring (this doc) and MCP schema typing, with `Job::ResolveOrDiagnoseScalar` ([Job.cpp:2718](../../src/Library/Job.cpp)) staying as the runtime **enforcement** (the schema/descriptor is a first-pass filter; the parser remains the authority — [VALIDATION_ARCHITECTURE.md](VALIDATION_ARCHITECTURE.md) §5.3). This doc **owns the shape** of the structure per §16; sibling specs consume it. Today the pipe is implicit in `ResolveOrDiagnoseScalar` and recoverable only from the material's C++ getter types ([CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) §4, §7) — surfacing it on the descriptor makes socket color and connection legality *structural* (the GUI canvas, the importer, and the parser can't drift — the same anti-drift discipline that keeps the highlighter honest). The structure:
- lets the C1 panel pick a swatch vs a slider deterministically,
- lets the C2 canvas color the socket and validate a drop with no special-casing,
- lets C3 route by destination slot,
- and is the natural home for the `tangent_rotation` annotation ("Color pipe, but semantically an angle").

**A single `pipe` enum is too weak on its own — hence a structure, not one overloaded field.** The simple `Color | Scalar | Material | Either` discriminator (sufficient for socket color) does **not** carry enough to validate a connection or generate a faithful schema — the adversarial review flagged it as under-specified, and the §16 decision is to make `ParameterSemantics` **a structure with separate fields** rather than overload everything onto `pipe`. Each `Reference`-kind / scalar-binding parameter declares the following **distinct fields on the `ParameterSemantics` structure** (per §16, MATERIAL_EDITOR owns the shape):

| Field (on `ParameterSemantics`) | What it encodes | Why it's needed (consumer) |
|---|---|---|
| **`pipe`** | `Color` / `Scalar` / `Material` / `Either` | socket color (C2); coarse drop legality |
| **cardinality** | how many channels the slot accepts (1 = single-scalar, 3 = RGB-triple, N) | a single-scalar `ior` slot must **reject** a 3-channel `rgb` scalar painter; a schema emits `minItems`/`maxItems` only where the parser actually enforces count ([VALIDATION_ARCHITECTURE.md](VALIDATION_ARCHITECTURE.md) §5.1 — the descriptor must say where count is real vs. tolerated) |
| **`requireSingle`** | this slot demands exactly one scalar (the per-channel-in-single-slot reject) | the exact `ResolveOrDiagnoseScalar` three-way diagnostic case (per-channel scalar in a single-scalar slot → reject); §6 mapping table already references `requireSingle` on the RGB-scalar row |
| **semantic units** | the physical unit/meaning (radians, nm, µm, dimensionless) | the `tangent_rotation` angle annotation; the µm→nm thin-film convert (§3.2); the editor's row label + the OpenPBR import unit map |
| **colour space** | for `Color`-pipe slots, the expected interpretation (JH-uplifted vs verbatim) | distinguishes a JH-uplifted reflectance from a verbatim-stored normal-map vector ([../COLOR_SPACE_MIGRATION.md](../COLOR_SPACE_MIGRATION.md) `color_space Rec709RGB_Linear` verbatim idiom) |
| **spatial-vs-spectral data** | whether the slot expects spatially-varying input (texture/expression) vs. spectral magnitude(λ) | the `tangent_rotation` oddball (color pipe, but the data is a spatial angle field, *not* spectral); the `PainterToScalarAdapter` bridge warning that it protects spatial-grayscale but **not** spectral input (§3.4) |

**How the schema consumes it.** [VALIDATION_ARCHITECTURE.md](VALIDATION_ARCHITECTURE.md) §5.3 generates a JSON Schema from the descriptor **plus these richer constraints**, held honest by the `tests/SchemaConformanceTest.cpp` corpus test (§5.4 there) that asserts the schema neither over- nor under-accepts versus the real parser — so a constraint that's stricter than the parser (e.g. a cardinality cap the parser tolerates) becomes a *red test*, not a silent over-rejection. The **runtime accept/reject authority stays `Job::ResolveOrDiagnoseScalar`**; the descriptor field never replaces it, it makes the same decision predictable to the editor/importer ahead of the parser run.

This is small and high-leverage. **It is the one genuinely new shared-C++ piece C1 wants** (C1 can ship without it using getter-type introspection, but the `ParameterSemantics` structure is the clean long-term source of truth). Adding the `ParameterSemantics` structure to `ParameterDescriptor` is an additive struct extension; the `pipe` enum and the other constraint enums (cardinality kind, units, colour space) each feed the bridge-enum mirror generator like every other cross-platform enum (per the `abi-preserving-api-evolution` skill — audit the enum-translation getters in both bridges when a new value is added to any of them).

---

## 7. Shared C++ vs platform-specific (per deliverable)

Per principle 2 (max shared C++; platforms are thin shells) and the §10.2 split in the roadmap.

| Deliverable | Shared C++ (library) | Platform-specific (thin shell) |
|---|---|---|
| **C1 instance editor** | Material introspection + descriptor rows + the `ParameterSemantics` structure (§6.4); the `setProperty`/`propertySnapshotFor` dispatch; presets; **thumbnail engine** (§4); duplicate-material | The slider / swatch / combo *widgets*; panel layout (SwiftUI / Qt) |
| **C2 node graph** | **Graph model** (nodes/edges from chunk introspection); **serialization + round-trip reconciliation** (§3.7); connection-legality validator (reuses `ResolveOrDiagnoseScalar`); layout-sidecar read/write; thumbnail engine | The **node-canvas widget** (draw nodes, drag wires, frames, search palette) — the one genuinely large per-platform UI piece |
| **C3 MaterialX import** | The whole importer (parse MaterialX, lower to `IJob::Add*Material`, color-vs-scalar routing); shares the glTF importer's chunk-emit surface | File-open dialog only |

The pay-off mirrors the roadmap's MCP argument: the graph model, serialization, validator, importer, and thumbnail engine are written **once**; only the node-canvas widget and the row widgets are per-platform. The canvas is the material editor's analogue of "the one big platform widget" (like the viewport's GPU present surface).

---

## 8. Android tier note (§10.4)

Per the roadmap's staged tiering (Android consumes the same C++ via JNI; the UI shell is mobile-first):

- **C1 instance sliders → Tier B (touch-adapted).** Material *instance* sliders/swatches are explicitly Tier B in [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §10.4. The shared introspection + `setProperty` + thumbnail engine come for free over JNI; Android supplies touch-sized slider/swatch widgets and a single-column layout. The spectral picker rows degrade to a compact mobile form (curve view read-mostly; Kelvin/λ sliders touch-native).
- **C2 node-graph editor → Tier C (deferred / desktop-first).** A wire-dragging canvas is a poor touch target; present it as **"edit the node graph on desktop"** gracefully (show the read-only Phase-A graph *view* — pinch/pan is fine for inspection — but route topology edits to desktop), never a broken half-canvas. This matches §10.4's "node-graph editor … Tier C, edit on desktop."
- **C3 import → desktop-first** (file-management heavy), but the importer itself, being shared C++, *runs* on Android if a file is handed to it; the UI just isn't a priority.
- **The LLM path stays Tier A on Android** ([../GUI_ROADMAP.md](../GUI_ROADMAP.md) §10.4): "make this gold more like brushed titanium" via chat is *more* natural on mobile than wire-dragging — the agent edits the same material chunks the graph would. The material editor and the agent are two authoring surfaces over the same text (principle 1).

---

## 9. Phasing

Aligns with [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §11: C1 + spectral-widget subset = roadmap **Phase 3**; C2 + C3 = roadmap **Phase 4** (depends on new graph-**topology** serialization **plus** the TO-BUILD painter-property path — material-*entity* property round-trip already ships, but `Painter` is **not** covered by Phase B: audit §1, [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §13a #7).

| Phase | Deliverable | Depends on |
|---|---|---|
| **C1.0** | Promote Material panel mode to first-class; sliders (descriptor `range`), swatches, presets, `fresnel_mode` disclosure | Descriptor infra (shipped); thin-film introspection (shipped). **Painter swatch *editing* persistence is TO-BUILD** (§2.1 note: `PainterIntrospection` + edit ops + dirty tracking + save re-emit; until then the colour picker is live-viewport-only and does not survive save). |
| **C1.1** | Shared **thumbnail engine** (§4) → C1 material swatch; duplicate-material; drag-to-assign hook | Progressive viewport (Phase 1) |
| **C1.2** | Descriptor **`ParameterSemantics` structure** — `pipe` + separate cardinality / `requireSingle` / units / colour-space / spatial-vs-spectral fields (§6.4, adopted §16); spectral row editors wired (→ [SPECTRAL_DIFFERENTIATORS.md](SPECTRAL_DIFFERENTIATORS.md) D1–D4) | Spectral differentiators (Phase 3) |
| **C2.A** | Read-only **graph view** (introspection → nodes/edges); node thumbnails reuse §4; frames/search | C1.1 thumbnail engine |
| **C2.B** | Graph **topology** serialization (new chunks + edge rewrite, §3.7/§3.8); **painter-property round-trip (TO-BUILD: `PainterIntrospection` + painter edit ops + dirty tracking + save re-emit, §2.1 note)**; cycle/orphan/copy-vs-link enforcement (§3.8); connection-legality validator; layout sidecar | Graph-topology emit + reference-rewrite + the painter-property path (round-trip save's **material-entity** property re-emit already ships — audit §1; **NOT** blocked on "Phase 0 save landing", but Painter is **not** covered — [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §13a #7); reference-safe deletion ([ENTITY_CREATION.md](ENTITY_CREATION.md) §5) |
| **C3** | MaterialX / OpenPBR **importer** (color-vs-scalar routing); reuse glTF chunk-emit surface | C2.A (to *see* imported graphs); independent of C2.B |

---

## 10. Non-goals / deliberately NOT doing

- **MaterialX is NOT RISE's native storage format.** Import only; native stays RISE's spectral superset ([../GUI_ROADMAP.md](../GUI_ROADMAP.md) §12). Adopting it as storage caps RISE at RGB and discards dispersion / measured-spectral / thin-film exactness.
- **The node editor is NOT on the default path.** Gated behind "Advanced," KeyShot-style ([../GUI_ROADMAP.md](../GUI_ROADMAP.md) §12). A good material must be achievable with C1 instance knobs alone — never gate a basic result behind a graph (the Houdini trap).
- **No second mutation path.** Graph edits, instance edits, hand edits, and AI edits all converge on `SceneEditController` + round-trip save + undo/redo (principle 6). The node graph does not get a private writer.
- **No new `node`/`graph` chunk type.** A material graph serializes as the named `*_material` + `*_painter` chunks it already lowers to (§3.7); layout is a sidecar. Don't fork the descriptor-driven format.
- **No synchronous swatch/thumbnail generation** (the Maya Hypershade failure). Async, cancellable, cached, budgeted, disableable — always.
- **No mode-swapped dual panels for one material** (Maya anti-pattern, §12). One canonical inspector; C2 is a toggle from C1, not a parallel world.
- **No auto-destroyed graph layout** (Hypershade sin). User layout/frames persist as read-only sidecar state.
- **No bespoke per-material UI code.** Rows come from the descriptor; new materials surface automatically (the descriptor-driven force multiplier).

---

## 11. Open questions / spikes (flagged uncertainties)

1. **`tangent_rotation` annotation form (§6.4).** The `ParameterSemantics` structure itself is **decided — adopted, with separate fields** ([../GUI_ROADMAP.md](../GUI_ROADMAP.md) §16); what remains open is the exact encoding of the oddball within that structure: `pipe = Color` + a `semanticUnit = angle` field + the `spatial` data-flag, vs. a one-off flag. Also confirm no parser/highlighter regression when the structure lands. *No longer blocks C2 socket coloring (the structure is committed); this is the residual annotation detail.*
2. **Topology-reconciliation granularity (§3.7/§3.7a/§3.8).** Surgical chunk-insert + reference-rename vs the **ownership-closure rewrite** (§3.7a) for topology changes — the same spike as [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §13.6. The save engine **already exists** (audit §1), so this is now settled empirically against the shipped Mode-A splice path rather than gated on it. The open detail is only *how often* a real topology edit lands in the REFUSE bucket (ambiguous/shared/macro-generated closure) and so needs the Duplicate-node escape hatch — not whether a "material block" exists to rewrite (it doesn't).
3. **Spectral hover-preview cost** ([../GUI_ROADMAP.md](../GUI_ROADMAP.md) §13.1) — is a live thumbnail-on-drag affordable on a spectral PT, or commit-on-release? Shared with A3/D3; the thumbnail engine's budget knob is where this lands.
4. **Named reusable material library** — light wrapper chunk vs copy-in via the asset library + C3 (§3.7, §3.8 reusable-subgraphs). Lean copy-in (no new format); decide in C2.B.
5. **Composed-material editing depth.** `pbr_metallic_roughness` and GGX-emissive are *composed* and gated read-only per-slot upstream (`IJob::IsMaterialComposed` — the editor refuses `SetSlot` on them, per [GGXMaterial.h](../../src/Library/Materials/GGXMaterial.h) comments). In the node graph, do we *decompose* them into their lowered painter graph (editable) or show them as an opaque node with the high-level dials only? Recommend: show the high-level uber-node dials (editable, re-lower on change); offer "unpack to graph" as an explicit, irreversible-ish action. *Flagged — affects how faithfully a glTF-imported material is editable.*
6. **MaterialX coverage scope** — which OpenPBR/MaterialX node subset C3 targets first (Surface + standard texture/constant nodes, per §5.2) vs the long tail of MaterialX procedural nodes. Start with the OpenPBR Surface graph + image/constant inputs; diagnose-and-drop the rest.

---

## 12. Acceptance criteria (GUI_ROADMAP §15 template, filled in)

Per the [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §15 template every deliverable must fill in. Scoped per deliverable (C1 / C2 / C3) where they differ.

- **Tests.**
  - *C1 introspection rows:* the Material panel surfaces every editable slot for each of the 20+ material types with the correct `pipe`-driven editor (swatch for color, slider for scalar), and read-only slots (composed `pbr_metallic_roughness`/`ggx_emissive`, audit §3) render disabled — extend `Phase4DescriptorDriven` / `ThinFilmIntrospectionTest`.
  - *Pipe/constraint conformance:* `tests/SchemaConformanceTest.cpp` ([VALIDATION_ARCHITECTURE.md](VALIDATION_ARCHITECTURE.md) §5.4) asserts the schema generated from the `ParameterSemantics` structure's separate fields (`pipe` + cardinality, `requireSingle`, units, colour space, spatial-vs-spectral) neither over- nor under-accepts versus the real parser; a single-scalar slot rejects a 3-channel `rgb` painter; a color-painter-into-scalar-slot drop reproduces the `ResolveOrDiagnoseScalar` diagnostic verbatim.
  - *C2 graph round-trip:* a graph topology edit (add node, rewire edge) saves and **re-opens to the identical graph**, byte-stable on the second save (the SaveEngine byte-identity signal, audit §1; extend the created-entity round-trip pattern of `SaveEngineTest.cpp`). Cycle/orphan/copy-vs-link invariants (§3.8) each have a crafted-input test: a wire that would cycle is rejected; deleting a referenced painter blocks-or-cascades and never orphans; duplicate forks, link shares.
  - *C2 ownership-closure fallback (§3.7a):* a topology edit whose closure is **solely owned** rewrites only that closure and leaves every other byte (a *shared* painter, a neighbouring unrelated chunk, a `FOR` region) byte-identical; a topology edit whose closure is **ambiguous** — touches a painter with ≥2 referrers, a macro/`FOR`-generated chunk, or a span the engine can't address as a unit — returns `Status::Refused` with a diagnostic and **does not modify the file**; the Duplicate-node escape hatch forks the shared painter and unblocks the same edit. Guards "never rewrite a guessed material block."
  - *C3 import routing:* a MaterialX `conductor_bsdf` with `color3` ior/extinction routes to the **scalar** pipe (not JH-uplifted), proven by a render-equality check against the hand-authored RISE equivalent (the §5.1 trap).
  - **Correctness invariant (engine-touching discipline):** the `ParameterSemantics` descriptor structure + the graph model are pure authoring metadata — **integrators stay byte-identical** (no RMSE change on any reference scene); the parser's accept/reject set is unchanged for non-graph scenes (the schema is a filter, the parser stays authority, [VALIDATION_ARCHITECTURE.md](VALIDATION_ARCHITECTURE.md) §5.3).
- **Platform parity.** Shared C++: introspection rows + `pipe`/constraints + the graph model + serialization/reconciliation + connection-legality validator + thumbnail engine (§7). macOS / Windows: full C1 panel + the C2 node canvas + C3 import. **Android (per §8):** C1 instance sliders **Tier B** (touch-adapted, JNI-shared core); C2 node graph **Tier C** (read-only Phase-A view; topology edits routed to desktop — never a broken half-canvas); C3 import desktop-first. The LLM "make it brushed titanium" path stays **Tier A** on Android (§16 Android posture). No surface degrades below read-only.
- **Performance budget.** Node/material thumbnails are **async, cancellable, cached, budgeted** (§3.3/§4) — never a synchronous swatch stall (the Maya 10 s/swatch failure); a live preview-on-drag defaults to commit-on-release if the spectral-PT hover cost exceeds budget (§11.3). The dependency-graph scan for reference-safe deletion is O(painters) on demand, off the render-hot path. **No production-render regression** (integrators untouched; cite the L8 ~0.4% bar).
- **Memory budget.** One shared thumbnail cache with a max-concurrent-preview budget + a cache-size cap (§4); invalidation keyed on resolved-parameters + upstream-graph hash. The graph model + layout sidecar are bounded by the material count; no per-frame allocation.
- **Accessibility.** Every row is keyboard-reachable; socket pipe is conveyed by **shape/label as well as colour** (yellow/grey/green is not the only signal — no colour-only dependence); the node canvas exposes a keyboard path to add/connect/delete and a focus order. Diagnostics carry text `message` + `code` + location (shared with the validator, [VALIDATION_ARCHITECTURE.md](VALIDATION_ARCHITECTURE.md) §6).
- **Packaging.** No new runtime assets. The `ParameterSemantics` structure is an additive `ParameterDescriptor` extension; any **new** `.cpp`/`.h` for the graph model / thumbnail engine must be added to **all five build projects** per [../../CLAUDE.md](../../CLAUDE.md) (none auto-discovers files). The bridge-enum mirror for the `pipe` enum and the other constraint enums is generated, not hand-maintained.
- **Migration.** **No scene-format change** for C1/C2 — a serialized graph is the existing named `*_material`/`*_painter` chunks (no new `node`/`graph` chunk, §10 non-goal); layout is a sidecar. The `ParameterSemantics` structure + new controller/graph methods are **ABI-additive** (new non-virtual methods + appended `RISE_API_*` C exports; no shipped signature changes, no new virtuals on frozen interfaces — per the `abi-preserving-api-evolution` skill). C3 is import-only; native storage is unchanged.
- **Rollback.** C2/C3 are gated behind the "Advanced" toggle (default-off path leaves C1 instance editing intact, §10 non-goal); disabling the node editor cannot affect a saved scene (the chunks it would emit are ordinary materials/painters). The `ParameterSemantics` structure degrades to getter-type introspection if unread (§6.4). Per-family graph-topology emit lands behind the byte-identity tests; an unimplemented topology edit stays the **ownership-closure rewrite** fallback (§3.7a) — or a clean REFUSE when the closure is ambiguous — rather than corrupting a file.

### Android tier note
Mirrors §8: **C1 → Tier B** (touch slider/swatch over the shared introspection + thumbnail engine via JNI), **C2 → Tier C** (read-only graph *view* is fine for inspection; topology edits are "edit on desktop", never a broken canvas), **C3 → desktop-first** (the importer *runs* on Android as shared C++ if handed a file; the UI isn't a priority). The agent/chat authoring path is **Tier A** on Android and edits the same material chunks the graph would (principle 1).

---

## 13. Cross-references

- Umbrella vision + phasing: [../GUI_ROADMAP.md](../GUI_ROADMAP.md) (§7 Direction C, §11 Phases 3–4, §10.2/§10.4, §12).
- **Ground truth** (code-verified; supersedes plan-doc `Status:` headers): [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) (§1 round-trip save shipped incl. Phase-B material re-emit; §4/§7 pipe-not-in-descriptor; punch-list Theme 1).
- **Spectral material widgets** (curve editor, measured n,k, thin-film slider, gamut warning) — fully specified in the sibling [SPECTRAL_DIFFERENTIATORS.md](SPECTRAL_DIFFERENTIATORS.md) (D1–D4); this doc references them as socket/row editors and does **not** re-specify them.
- **Graph edits are transactions** (epoch/precondition/conflict/undo-attribution; create-delete-rewire each commit through the one mutation path): [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md).
- **Material create / delete + reference-safe deletion** (the ops, the dependency graph, block-or-cascade, stable names, the raw-pointer orphan hazard): [ENTITY_CREATION.md](ENTITY_CREATION.md) (§4–§7).
- **`ParameterSemantics` schema consumption + the conformance test** (how the structure's separate fields feed the generated schema; the parser stays the authority): [VALIDATION_ARCHITECTURE.md](VALIDATION_ARCHITECTURE.md) (§5.3, §5.4).
- Material taxonomy + "adding a BSDF" checklist: [../MATERIALS.md](../MATERIALS.md).
- The color-vs-scalar pipe (the spine of §1/§3.4/§5/§6): [../ISCALARPAINTER_REFACTOR.md](../ISCALARPAINTER_REFACTOR.md).
- Thin-film material the editor must expose: [../THIN_FILM_INTERFERENCE.md](../THIN_FILM_INTERFERENCE.md).
- Round-trip persistence (C2 Phase B depends on **topology** serialization **plus** the TO-BUILD painter-property path — **material-entity** property re-emit ships, but `Painter` is not covered: audit §1, [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §13a #7): [../ROUND_TRIP_SAVE_PLAN.md](../ROUND_TRIP_SAVE_PLAN.md).
- Descriptor-driven parser (the C1 substrate + C2 serialization target): [../../src/Library/Parsers/README.md](../../src/Library/Parsers/README.md), [ChunkDescriptor.h](../../src/Library/Parsers/ChunkDescriptor.h).
- Bridge property rows (the C1 mechanism): [RISEViewportBridge.h](../../build/XCode/rise/RISE-GUI/Bridge/RISEViewportBridge.h).
- Painter classes: [../../src/Library/Painters/](../../src/Library/Painters/).

---

## 14. External references (web research)

- Unreal master material / material instance (no shader recompile; scalar/vector/texture parameters): [Epic — Instanced Materials in UE5](https://dev.epicgames.com/documentation/unreal-engine/instanced-materials-in-unreal-engine); [Epic — Making Material Parameters](https://dev.epicgames.com/documentation/en-us/unreal-engine/making-material-parameters); [Hoffman — UE4 Shader Permutations](https://medium.com/@lordned/unreal-engine-4-rendering-part-5-shader-permutations-2b975e503dd4) (why RISE's CPU path tracer has the model *better* — no permutation/recompile cost).
- Substance Designer per-node live thumbnails (and the manual-recompute footgun to avoid): [CG Channel — Designer 2020.2 thumbnails](https://www.cgchannel.com/2020/10/adobe-unveils-substance-designer-2020-2/); [Adobe — Compute Node Thumbnails hotkey thread](https://community.adobe.com/t5/substance-3d-designer-discussions/hotkey-for-compute-node-thumbnails/td-p/12363560); [Substance 15.1 node cleanup / Groups](https://digitalproduction.com/2025/12/15/substance-designer-cleans-up-its-nodes/); [Adobe — graph instances / subgraphs](https://helpx.adobe.com/substance-3d-designer/substance-compositing-graphs/graph-instances-sub-graphs.html).
- Blender Principled BSDF uber-shader + color-coded sockets (grey=value, yellow=color, green=shader): [Blender — Principled BSDF](https://docs.blender.org/manual/en/latest/render/shader_nodes/shader/principled.html).
- MaterialX / OpenPBR (conceptual model + interchange; RGB-centric, no spectral type, dispersion unimplemented in reference backends): [OpenPBR Surface](https://academysoftwarefoundation.github.io/OpenPBR/); [MaterialX PBR spec — color3 is linear RGB; `conductor_bsdf` ior/extinction are color3](https://github.com/AcademySoftwareFoundation/MaterialX/blob/main/documents/Specification/MaterialX.PBRSpec.md); [4D Pipeline — OpenPBR/MaterialX support (UE Substrate lacks transmission dispersion)](https://blog.4dpipeline.com/customer-insights-where-are-openpbr-and-material-x-supported).
- Maya Hypershade anti-patterns (synchronous swatch stalls, slow window open): [Autodesk — slow Hypershade solution (~30 s open)](https://forums.autodesk.com/t5/maya-forum/super-slow-hypershade-in-maya-2022-3-solution/td-p/11132437); [Chaos — extremely slow swatch generation (~10 s/swatch)](https://forums.chaos.com/forum/chaos-common/chaos-common-public/29969-extremely-slow-swatch-generation-in-the-hypershade); [Autodesk — pause swatch generation to improve performance](https://knowledge.autodesk.com/support/maya/learn-explore/caas/CloudHelp/cloudhelp/2022/ENU/Maya-LightingShading/files/GUID-9BFB8717-DED0-431F-8F2B-F3A57D59386E-htm.html).
- KeyShot drag-to-assign default + opt-in/Pro graph (the gating model): [KeyShot — Assigning Materials](https://manual.keyshot.com/manual/materials/assigning-materials/); [KeyShot — Material Graph UI](https://manual.keyshot.com/manual/materials/material-graph/material-graph-user-interface/).
