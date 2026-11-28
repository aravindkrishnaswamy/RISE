# RISE GUI Roadmap — From Render Viewer to AI-Operable Creative Tool

**Status:** **SUBSTANTIALLY EXECUTED UMBRELLA ROADMAP.** On macOS and Windows, the shared `SceneEditController`, CST-native lossless persistence (`SaveEngine::Save` / `Cst::SerializeCst`) with **exact-byte CST undo** (`Cst::DocReplaceChunkIfText`), redesigned workspaces, three-path Start Screen (`empty_starter.RISEscene`), entity creation (`EntityTemplates` + `OwnershipClosure`), environment editing, camera/named-view tools (`NamedViewsPanel`), render modes + N-up viewports, **Material Editor Slices 1–6 ALL SHIPPED** (C1 descriptor sliders & inline slot editors, C2 editable `NodeGraphCanvas` with Slice 4 pipe-coloured sockets & scalar adapters, Slice 5 shared 3D `MaterialPreview` shaderball, and Slice 6 Spectral Differentiators D1–D3), sibling **`ObjectGraphCanvas`**, macOS **`SimulationsPanel`** ([SIMULATION_GUI_STATUS.md](SIMULATION_GUI_STATUS.md)), and the **5-provider `src/Library/Agent/`** chat/MCP/loopback stack (`Anthropic`, `Gemini`, `OpenAI`, `XAI`, `Local`; 54 MCP / 49 chat tools including 11 `sim_*` verbs) have all shipped. Full Windows VS2022 + Qt6 `RISE-GUI` compilation is verified green on MSVC v145 (`b6663f1af`). Remaining backlog: `default_studio.RISEscene` + CC0 asset library (A1/A5) & drag-to-assign (A3), C2 group frames/uber-node polish, C3 MaterialX import, spectral scopes/heatmaps/Light Mix (D5–D7, plus remaining D1 curve/prism widgets), Windows Qt6 `SimulationsPanel` parity, Film/Environment transaction-arm cleanup + bridge enum codegen (`tools/gen_bridge_enums.py`), standalone `Category::Geometry` templates + `Shader`/`Modifier` categories, multi-document `RenderCoordinator` / security jailing, and Android parity gaps (§10.4). Consult [gui/MATERIAL_EDITOR.md](gui/MATERIAL_EDITOR.md), [gui/NODE_GRAPH_CANVAS.md](gui/NODE_GRAPH_CANVAS.md), [gui/SPECTRAL_DIFFERENTIATORS.md](gui/SPECTRAL_DIFFERENTIATORS.md), and [gui/RENDER_MODES.md](gui/RENDER_MODES.md) before treating any phase below as pending.
**Owner:** Aravind Krishnaswamy
**Scope:** Make the RISE GUI (macOS, Windows, and — staged — Android) materially more capable and more *approachable*, without cloning Blender/Maya. Three pillars: (1) an approachability foundation, (2) creative-power features (cameras/views, material editor, spectral-only differentiators), and (3) a first-class **LLM/agent integration** that can fully operate scenes and the engine. All three rest on one architectural spine: **the `.RISEscene` text file is the canonical source of truth, and the shared C++ library owns everything that isn't a native widget.**

This doc supersedes nothing — it sits alongside
[INTERACTIVE_EDITOR_PLAN.md](INTERACTIVE_EDITOR_PLAN.md) (the viewport
execution record), [CAMERAS_ROADMAP.md](CAMERAS_ROADMAP.md) (optics), and
[gui/ADR_DOCUMENT_FIRST_EDITOR.md](gui/ADR_DOCUMENT_FIRST_EDITOR.md) (historical persistence
design plus current CST-save header), and pulls their GUI-facing threads into
one picture.

---

## 0. The one-paragraph thesis

RISE already owns the hard parts every "easy" renderer is proud of: a progressive spectral integrator, an auto-router that picks PT/BDPT/VCM, GPU OIDN, and true HDR/EDR output. Both desktop GUIs are real ~8.5–9k-LOC apps at near parity, sharing one C++ `SceneEditController`. **The gap to "more capable" is an interaction-and-approachability layer, not an engine gap** — and the property that makes RISE feel old-fashioned (it's scene-*file* driven) is in fact its single biggest differentiator, because a documented, validatable, diffable **text** scene language is the ideal substrate both for git-style workflows *and* for an LLM to operate. The strategy is therefore: **keep text as the canonical truth; make the GUI and an embedded AI agent two parallel, non-exclusive ways to author that text; and build all of it in the shared C++ library so macOS, Windows, and Android stay in lock-step by construction.**

---

## 1. Guiding principles

These are load-bearing. Every deep-dive spec must honor them.

1. **Text is the source of truth (lean into it, don't apologize for it).** The `.RISEscene` file stays canonical: human-readable, hand-editable, git-friendly, diffable, reproducible. The GUI and the AI agent are *authoring surfaces* that round-trip to text; neither replaces it. This is the answer to "is scene-file-driven a weakness?" — it is a **differentiating factor to lean into** (see §4).

2. **Maximize shared C++; go platform-specific only where forced.** *(New, elevated to a top-level principle.)* All non-UI logic lives in `src/Library/`: the scene model, `SceneEditController`, round-trip save, the MCP server, the LLM agent loop and provider adapters, asset-library logic, the named-views model, the material-graph model, render orchestration. Platform code owns only what the OS forces: native widgets, the **display/present surface** (Metal compositing on macOS, DXGI on Windows, CPU-bitmap on Android — RISE renders on the CPU, so this only *presents* the rendered buffer; it is not a GPU render path), file dialogs, secure-credential storage, and input-event translation. The existing `RISEViewportBridge` (Obj-C++) and `ViewportBridge` (Qt) are already structurally identical — that is the pattern, and new work should push even more behind a single C-ABI surface. See §10.

3. **Android is not left behind.** *(New.)* Android is not a priority, but it must keep the *core abilities* — especially the LLM integration. Because the core is shared C++ (consumed via JNI), Android gets new capability largely for free; what differs is the *UI shell* (mobile layout, touch interactions) and a **staged feature tiering** (§10.4). Notably, the LLM chat surface is *more* natural on mobile than on desktop — "talk to your renderer from your phone" is a legitimate Android headline.

4. **Approachable by default, powerful on demand (progressive disclosure, ≤2 levels).** A freshly opened scene should produce a good image with zero clicks (Auto integrator + default studio HDRI + OIDN on + auto-exposure). Depth (spectral curves, BDPT/VCM knobs, node graphs) lives one obvious toggle away, never on the default path. Never gate a basic result behind a node graph (the Houdini trap) or behind mode-swapped menus (the Maya trap).

5. **Don't replicate — differentiate on spectral.** The features that make RISE *unique* are the ones an RGB tool physically cannot do: live spectral-curve editing, measured-metal n,k, real thin-film interference, Jakob-Hanika gamut honesty, "explain the auto-router," spectral Light Mix, EDR cinematography scopes. These are mostly thin UIs over painters RISE already has (§8).

6. **Everything routes through one mutation path.** GUI edits, AI edits, and hand-edits all converge on `SceneEditController` + round-trip save + undo/redo. No parallel write paths. This is what makes AI actions undoable, diff-reviewable, and persistent.

---

## 2. Current state (audit summary)

Full audit lives in the deep-dive specs; the load-bearing facts:

**Desktop GUIs are real and near-parity.** macOS = SwiftUI + Obj-C++ bridge (`build/XCode/rise/RISE-GUI/`); Windows = Qt6 + C++ bridge (`build/VS2022/RISE-GUI/`). Both: live ~30 Hz interactive viewport, orbit/pan/zoom + object translate/rotate/scale gizmos, descriptor-driven properties panel, multi-camera select + clone, syntax-highlighted scene editor with autocomplete, animation timeline + named paths, HDR/EDR display with tone curves, ProRes/HDR10 export, auto-rasterizer surfacing ("Auto → PT/BDPT/VCM"), always-on OIDN.

**The descriptor-driven properties panel is a force multiplier.** Any scene parameter with a `ChunkDescriptor` entry auto-appears as an editable row — no per-parameter UI code. The MCP tool schemas and the AI's grammar view are generated from the same `SceneGrammar::Describe()` source — but the descriptor is *tolerant* (it checks finite-numeric tokens, not cardinality / enum membership), so a generated schema can **over-reject** the real parser; it is a first-pass filter held honest by a conformance test, not a drift-proof oracle ([gui/VALIDATION_ARCHITECTURE.md](gui/VALIDATION_ARCHITECTURE.md)).

**Persistence and undo are now CST-native and byte-exact.** The July 2026 cutover
deleted the interim byte-splice/managed-override engine described by the older
correction that used to live here, and the October 2026 undo slice landed
exact-byte CST undo ([`Cst::DocReplaceChunkIfText`](../src/Library/Cst/Cst.h) +
per-object gizmo spelling journal). A `Job` retains the canonical CST `Document`;
edits, entity insertion/removal (`EntityTemplates` for Light, Object, Material,
Painter, Medium + camera CRUD), and agent mutations operate on that document,
and [`SaveEngine`](../src/Library/SceneEditor/SaveEngine.cpp) serializes it
byte-exactly when unedited (or undone back to loaded text) and minimal-diff when
edited.

**Major roadmap slices have shipped on both desktop platforms (macOS + Windows):**
redesigned workspaces and start screen (`empty_starter.RISEscene`), outliner/entity
creation (`EntityTemplates` + `OwnershipClosure`), environment editing, canonical
and named camera views (`NamedViewsPanel.swift` on macOS and `NamedViewsPanel.{h,cpp}`
on Windows Qt — Windows Qt6 widgets and `<wincred.h>` `CredentialStore` are code-complete
and review-verified on macOS, gated on a Windows MSVC/Qt6 compile/runtime pass), render modes, N-up panes,
**Material Editor Slices 1–6 ALL SHIPPED** — C1 inspector (`PropertiesPanel.swift` /
`ViewportProperties.cpp`) + C2 node-graph canvas ([gui/NODE_GRAPH_CANVAS.md](gui/NODE_GRAPH_CANVAS.md))
with 2D `PainterPreview`, Slice 4 pipe-coloured sockets/wires and scalar-to-colour
adapter insertion, Slice 5 shared 3D [`MaterialPreview`](../src/Library/SceneEditor/MaterialPreview.h)
shaderball (56×56 in C1 headers, 40×40 on C2 Material nodes), and Slice 6 Spectral
Differentiators ([gui/SPECTRAL_DIFFERENTIATORS.md](gui/SPECTRAL_DIFFERENTIATORS.md):
D1 Blackbody Kelvin strip & Sellmeier glass presets with computed $n_d$/Abbe $V_d$,
D2 CC0 Au/Ag/Cu/Al/Cr/Ti measured-metal presets, D3 0°–89° thin-film iridescence
strip) — plus the sibling **`ObjectGraphCanvas`** (Objects + Geometry + CSG/instancing
wires on macOS & Windows), the **5-provider `src/Library/Agent/` stack**
(`AgentChatLoop`, `AgentChatCodecs` for Anthropic/Gemini/OpenAI/XAI/Local,
`AgentMcpAdapter`, `AgentLoopbackHttpServer`), and editable region rendering +
explicit region-only final renders. Current gaps include C3 MaterialX import,
`default_studio.RISEscene` + A3 drag-to-assign + A5 CC0 asset library, remaining
D1/D4–D7 spectral/scope overlays, Android `.RISEscene` save / missing inspector
categories / agent JNI, and process-wide multi-document security/orchestration
hardening. ([GUI_ROADMAP.md](GUI_ROADMAP.md)
is a frozen June 2026 pre-implementation baseline — do **not** read it as
present-day capability authority.) The sections below remain the umbrella
rationale and backlog; their original phase language is not a present-day audit.

---

## 3. Competitive findings (condensed)

Web-researched audit of Blender, Substance Designer, Unreal, Houdini/Karma+MaterialX, Maya Hypershade, Material Maker, KeyShot, Octane, V-Ray, Arnold, Cycles, D5/Lumion, SketchUp, Rhino, AutoCAD/Fusion. Full citations in the deep-dive specs. Distilled:

- **Approachability formula (universal):** default uber-shader + live feedback + *separate the graph author from the knob-turner*. KeyShot is the gold standard: "drag a material onto the object, with hover-preview, commit on release"; "drag an HDRI onto the viewport = relight"; the viewport *is* the renderer; thumbnail libraries are the primary navigation; the node graph is opt-in and gated to Pro.
- **Node-editor lessons:** Unreal's master/instance split is the most important model (and RISE gets it *better* — a CPU path tracer has no shader-recompile cost, so every parameter can be a live instance knob). Substance's per-node live thumbnails are the signature feature Blender/Unreal make you work for — RISE can render them for free. Maya Hypershade is the cautionary tale (icon-less nodes, auto-destroyed layout, synchronous swatch stalls).
- **MaterialX/OpenPBR:** the industry's converging interchange model — but RGB-centric (no spectral type; dispersion unimplemented in *every* reference backend). Adopt it as the *conceptual model + import format*, NOT as RISE's native storage; that would cap RISE at RGB and forfeit its reason to exist. RGB-at-the-boundary / spectra-internal is exactly the PBRT/Mitsuba posture RISE already takes via the JH LUT.
- **"Multiple camera views" reframed:** nobody renders four final-quality panes — Maya/Max quad view is 3 cheap GL panes + 1 shaded. For a path tracer, the high-value version is *camera/named-view management* (named views + thumbnails, axis-ball gizmo, camera list, "fly then stamp the camera," promote-to-scene-camera), not a literal four-up live render.
- **IPR patterns:** progressive viewport that pauses-on-navigation and restarts-on-edit; region/ROI render (highest sample-efficiency-per-effort); start/pause/restart/stop; convergence indicator + stop condition; V-Ray Light Mix (re-balance per-light contributions after render). Don't market as "real-time" or chase D5/Lumion 30fps DXR — different (non-converging) product.

---

## 4. Strategic answer: scene-file-driven is a differentiator — lean in

Direct answer to the question that prompted this section of the roadmap.

**It is a differentiating factor, and it is the foundation of the AI strategy.** Reasoning:

- Every GUI-only tool (Blender/Maya/KeyShot) has no durable, human-legible source of truth; the scene is an opaque binary and automation means driving a fragile binding or simulating clicks. RISE's scene is documented, descriptor-validated, **diffable text**.
- That property delivers, for free, what software engineering already values and 3D is adopting: **version control / git / code review** of scenes, **diffability** of every change (human/GUI/AI), **reproducibility & scriptability** (a scene is a build artifact). The industry trend is *toward* this — USD won by being declarative/text-first.
- **A text scene language is the ideal LLM substrate.** LLMs emit and edit validatable structured text far better than they drive GUIs or Python bindings. RISE's AI would speak the native language; a Blender LLM plugin fights the tool. This makes RISE arguably the best-positioned renderer in existence for genuine AI operation.

**The one real downside** — hand-editing is unforgiving for non-technical users (you can write an invalid scene) — is already being neutralized by the descriptor-driven grammar + suggestion engine, by round-trip save (which preserves comments/whitespace/FOR-loops so GUI edits never clobber hand-edits), and now by the AI itself. So:

> **Posture:** text is canonical and durable; the GUI and the AI agent are two parallel, non-exclusive authoring surfaces over it. Make the text *more* central, not less.

---

## 5. Direction A — Approachability foundation

Highest leverage; mostly interaction-layer; leans on engine pieces RISE already owns. Deep-dive: [gui/APPROACHABILITY_FOUNDATION.md](gui/APPROACHABILITY_FOUNDATION.md).

- **A1. Sensible default scene** — open/import → good image, zero clicks (studio HDRI + neutral material + auto-exposure + integrator = Auto + OIDN on). Leans on the auto-router + always-on OIDN.
- **A2. The viewport *is* the renderer** — progressive, with pause-refine-on-navigation (drop to ½-res/checkerboard while moving via `ResizeFilm`, refine on idle) and restart-on-edit; start/pause/restart/stop; a convergence indicator + stop condition.
- **A3. Drag-to-assign** — drag a thumbnailed material/HDRI onto an object (HDRI onto the viewport = relight), hover-preview, commit on release. *Feasibility spike:* is a spectral hover-preview cheap enough, or do we fall back to commit-on-release? (§11)
- **A4. Region / ROI render** — discoverable Draw Region mode + shortcut, editable measured overlay, ordinary finals always full-frame while preserving the viewport box, and a separate explicit Render Active Region command.
- **A5. Thumbnail asset libraries + search + shipped seed set** — materials and HDRIs out of the box (an empty library defeats A3).
- **A6. Progressive disclosure + layout presets** — Basic vs Advanced on every panel; "Simple"/"Full" workspace layouts defaulting to Simple.

---

## 6. Direction B — Cameras & views (reframed)

Deep-dive: [gui/CAMERAS_AND_VIEWS.md](gui/CAMERAS_AND_VIEWS.md). Built on the shipped multi-camera infrastructure ([CAMERAS_ROADMAP.md](CAMERAS_ROADMAP.md) Phase 1.3).

- **B1. Named Views layer** — save current viewport pose + thumbnail, restore on click. Kept as *UI/session state* distinct from scene cameras (honors the "prefer read-only" instinct); explicit **"promote to scene camera"** writes a real camera into the `.RISEscene`.
- **B2. Axis snaps + axis-ball nav gizmo** — 6 big face targets (not the 26-region ViewCube), turntable orbit default, Home / "set as home." Costs zero render time. Don't hard-depend on a numpad.
- **B3. "Fly then stamp"** — cheap navigation, then commit the angle to a camera (Blender's Lock/Align-camera-to-view). Perfect path-tracer fit: decouples cheap nav from expensive rendering.
- **B4. Camera list panel** — thumbnails + click-to-activate; bind-camera-to-time-range on the timeline.
- **B5. Right-sized split view (later)** — 3 cheap GL/wireframe orientation panes + 1 render pane; deterministic single↔quad toggle. Explicitly *not* four live path-traced panes.

---

## 7. Direction C — Material editor

Deep-dive: [gui/MATERIAL_EDITOR.md](gui/MATERIAL_EDITOR.md). Two staged deliverables.

- **C1. Instance-first editor (cheap 80%)** — a flat slider/swatch/preset view over existing material slots, driven by today's descriptor infrastructure. No new serialization. This is the Unreal "material instance" view and is what most users need.
- **C2. Node graph (heavy lift, gated "Advanced")** — visual node canvas; default node = an OpenPBR-shaped uber-material; per-node live thumbnails (RISE path-traces for free); socket color encodes the pipe (**yellow = `IPainter`** color, **grey = `IScalarPainter`** scalar); connection legality enforced from existing descriptor / `ResolveOrDiagnoseScalar` metadata; organization (frames/comments/search) from day one. Requires new scene-language graph serialization + material round-trip ("Phase B").
- **C3. MaterialX/OpenPBR import (Phase 2 of C)** — RGB `color3` → JH LUT, `float` → `IScalarPainter`; the importer must itself encode color-vs-scalar routing (the exact bug class [ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md) documents). Import only; native storage stays RISE's superset.

---

## 8. Direction D — Spectral differentiators ("don't replicate")

Deep-dive: [gui/SPECTRAL_DIFFERENTIATORS.md](gui/SPECTRAL_DIFFERENTIATORS.md). Mostly thin UIs over existing painters; impossible in RGB tools.

- **D1. Live spectral color picker / curve editor** — Kelvin slider, 380–780 nm monochromatic slider, drag-a-CSV measured SPD with the curve plotted beside the swatch, named-glass dropdown (BK7/SF11/diamond) → auto-fill Abbe/IOR with live prism preview. Backed by `spectral_painter` / `PiecewiseLinearScalarPainter` / `blackbody_painter` / `SellmeierScalarPainter`.
- **D2. Measured-metal n,k picker** (refractiveindex.info-backed) — gold's color from physics, into GGX `ior`/`ext` with `fresnel_mode conductor`.
- **D3. Spectral thin-film slider** — thickness in nm + angle-reactive swatch (soap bubble / anodized titanium); RISE already has `fresnel_mode thinfilm`.
- **D4. Jakob-Hanika gamut warning** — warn on color picks landing in the failing blue corner ([JH_LUT_GAMUT.md](JH_LUT_GAMUT.md)); a uniquely-spectral honesty feature. **Stale (2026-09-02):** Stage C ([SPECTRAL_ILLUMINANT_CONVENTION.md](SPECTRAL_ILLUMINANT_CONVENTION.md)) closed the failing blue corner (0 % unconverged cells), so this trigger no longer exists — see [gui/SPECTRAL_DIFFERENTIATORS.md](gui/SPECTRAL_DIFFERENTIATORS.md) §4A for the full note and possible replacement signal.
- **D5. "Explain the auto-router"** — surface the variance probe as a per-region heatmap + rationale ("Auto → BDPT here because glossy/indirect variance is high"). No competitor can replicate. Pair with RMSE view (σ²·T rewards dark images).
- **D6. EDR cinematography overlays** — false color (ARRI/SmallHD IRE legend), zebra, waveform, a spectral vectorscope plotting true xy chromaticity. Re-query EDR headroom on display change.
- **D7. Spectral Light Mix** — per-light AOVs rendered once, re-balanced live by color temperature / SPD swap.

---

## 9. Direction E — LLM / agent integration (operate scenes + control the engine)

Deep-dives: [gui/MCP_TOOL_SURFACE.md](gui/MCP_TOOL_SURFACE.md) and [gui/LLM_AGENT_RUNTIME.md](gui/LLM_AGENT_RUNTIME.md). This is the marquee differentiator and the reason §4 matters.

### 9.1 Why RISE is unusually suited
The scene is text *and* the grammar is machine-introspectable (`SceneGrammar::Describe()` already self-describes every chunk/parameter). The AI can be handed a generated schema (a first-pass filter — the descriptor is tolerant, so it can over-reject; see [gui/VALIDATION_ARCHITECTURE.md](gui/VALIDATION_ARCHITECTURE.md)) and validate its output against the real parser via an isolated `Job` — validity-by-construction, the hard part of agentic tooling, is well-supported.

### 9.2 The layered design — one tool surface, consumed three ways
1. **A first-party RISE MCP server** (in the shared C++ library) — the single source of truth for "what an LLM can do to RISE." MCP because it is the emerging standard for LLM tool use, it is **provider-agnostic** (decouples from Claude vs Gemini vs local), and it gives external-client reuse for free. Tool schemas + a grammar resource are **generated from `SceneGrammar::Describe()`** and kept honest by a conformance test (the descriptor is tolerant, so the schema can over-reject — [gui/VALIDATION_ARCHITECTURE.md](gui/VALIDATION_ARCHITECTURE.md)).
2. **An in-app chat panel** = an MCP *client* + a thin **provider adapter** (also in the library). The adapter handles auth/streaming and translates the MCP tool catalog into each model's function-call dialect:
   - **Cloud:** sign in with Claude (Messages API + tool use) or Gemini (function calling); tokens on the user's account.
   - **Local:** point at any OpenAI-compatible endpoint (Ollama / LM Studio / llama.cpp) — scenes never leave the machine (privacy story). Expect weaker tool-use; lean on the schema + `validate` tool.
3. **External MCP clients for free** — Claude Desktop / IDEs / Claude Code can drive RISE with zero extra work because the server is standard MCP.

### 9.3 Tool & resource surface (sketch — full catalog in the deep-dive)
**Resources (read):** full scene text + individual chunks; the grammar/descriptor schema; scene-graph introspection (objects/cameras/lights/materials, transforms, bboxes); **the rendered framebuffer + AOVs** (so a vision model can *see* its output); render stats / log / variance / auto-router decision; asset-library listings.
**Tools (act):** mutate property/transform; add/remove/clone entity; apply material; load HDRI; camera control (frame, named view, lens, create); render control (start/stop/pause, integrator-or-Auto, quality, region, resolution); save/load; export image/movie; **`validate(scene_text)`** dry-run returning descriptor diagnostics.

Two tools separate a toy from something that "fully operates" the engine: **`validate` before commit** (self-correction loop) and **returning the framebuffer to a vision model** (the see→adjust→render→see loop — a spectral renderer that converges on "believable sunset through the window" is impossible to fake in RGB).

### 9.4 Guardrails & staged autonomy
All AI actions route through `SceneEditController` + round-trip save + undo/redo (principle 6) — undoable, visible in the "show me the code" panel, persistent.
- **L0 Advisor** — chat, read-only, suggests text.
- **L1 Propose-and-confirm** — generates a scene *diff*; user approves/rejects (code-review gate).
- **L2 Operate-with-guardrails** — applies + renders within scoped permissions (read / edit / render-and-spend).
- **L3 Autonomous tasks** — "make 5 lighting variations and render thumbnails."

Caveats baked in: cloud tokens + repeated renders cost real money/time (cache previews, cap loops); server stays loopback-only with scoped permissions and no shell; the diff-review gate makes L1+ safe.

### 9.5 The cheap-start insight
The MVP does **not** require round-trip save. Like a human hand-editor, the AI can rewrite scene text wholesale and reload: `read scene + read grammar + propose diff + validate + reload + render + look` is a complete, powerful loop on *scene/render* infrastructure that mostly exists today (the agent-server protocol layer itself is net-new — see A0). That same affordance is exactly why text-as-truth (§4) is the differentiator. The "show me the code" panel and the AI spine are the same surface.

---

## 10. Cross-platform architecture & code-sharing strategy

*(Principle 2 + 3, made concrete.)* Deep-dive: [gui/CROSS_PLATFORM_ARCHITECTURE.md](gui/CROSS_PLATFORM_ARCHITECTURE.md).

### 10.1 The rule
Everything that is not a native widget, a display/present surface, a file/credential dialog, or input-event translation lives in `src/Library/` behind a C-ABI surface (the `SceneEditController` / bridge pattern). Platforms are thin shells.

### 10.2 What's shared vs platform-specific (target state)

| Capability | Shared C++ (library) | Platform-specific (thin shell) |
|---|---|---|
| Scene model, edit history, round-trip save | all of it | — |
| Interactive render orchestration (CPU path tracer) | all of it | display/EDR **present** surface only (Metal / DXGI; Android CPU-bitmap, SDR today) |
| Properties / descriptor reflection | all of it | widget rendering |
| **MCP server + tool dispatch** | **all of it** | — |
| **LLM agent loop + provider adapters** | **all of it** | OAuth/token UI + secure storage (Keychain / Credential Manager / Keystore) |
| Named-views model, asset-library index | all of it | thumbnail widget, drag-drop hit-testing |
| Material-graph model + serialization | all of it | node-canvas widget |
| Spectral pickers / scopes (math) | all of it | the actual slider/curve/scope widget |
| Chat transcript model | all of it | chat bubble UI |

The principle's pay-off: the **MCP server and the LLM runtime are written once** and all three platforms get AI control. Only the chat *shell* and credential storage are per-platform.

### 10.3 Bridge consolidation opportunity
`RISEViewportBridge.mm` and `ViewportBridge.cpp` are already structurally identical. New subsystems should expose one C-ABI header consumed by both, rather than re-implementing per platform. Candidate: extract a shared bridge core so the Obj-C++ / Qt layers shrink to marshaling.

### 10.4 Android staged fallback
Android keeps the *core abilities* (it consumes the same C++ via JNI) with a **mobile-first UI shell** and a staged feature **tiering**:
- **Tier A (must-have, incl. LLM):** scene browse/open, render view (HDR where supported), basic parameter edit, timeline scrub, `.RISEscene` save, **full LLM chat + agent operation** (chat is mobile-native — a plausible Android headline).
- **Tier B (touch-adapted):** gizmo transforms, named views, camera list, material *instance* sliders.
- **Tier C (deferred / desktop-first):** node-graph editor, EDR scopes, split view. Present as "edit on desktop" gracefully, never a broken control.

> **Current Android shipping status (honest audit):** Shipped today in `android/app/` + `RiseBridge.cpp`: interactive `Lit` preview viewport, touch camera navigation + gizmo transforms, 6 inspector categories (`Camera`, `Rasterizer`, `Object`, `Light`, `Medium`, `Output`), EV + tone-curve display controls, timeline scrub, and rendered-image export (`nativeSaveAs` writes TGA/PPM/EXR/PNG/HDR images). **Still unbuilt on Android:** `.RISEscene` scene-save UI/JNI (`SaveEngine` exists in shared C++ but has no JNI/Compose save flow yet), the remaining 5 desktop categories (`Material`, `Painter`, `Film`, `SceneVariant`, `Geometry`), non-`Lit` viewport render modes, Named Views UI, and the LLM chat Compose UI / JNI bindings / mobile TLS & Keystore credential plumbing (`AgentLoopbackHttpServer` / `AgentTlsClient` are compiled out under `__ANDROID__`).

Every deep-dive spec must include an "Android tier + interaction" note.

### 10.5 Net-new shared-library components surfaced by the specs
The deep-dives converged on a small set of genuinely-new shared-C++ pieces that more than one direction depends on:

- **A `ParameterSemantics` structure on `ParameterDescriptor` (Core SHIPPED in S17).** Defined in [`ChunkDescriptor.h`](../src/Library/Parsers/ChunkDescriptor.h) (`struct ParameterSemantics`), carrying `pipe` (`ParameterPipe`: `Color`, `Scalar`, `Material`, `Function1D`, `Function2D`, `Geometry`, `Other`), `requireSingle`, `keywordAllowlist`, `colorAlsoAcceptsScalar` (`DL-16`/`DL-17` dual-pipe rotation slots), and `note`. Consumed by [`ConnectionLegality`](../src/Library/SceneEditor/ConnectionLegality.h) and the C2 node-graph canvas ([gui/MATERIAL_EDITOR.md](gui/MATERIAL_EDITOR.md), [gui/NODE_GRAPH_CANVAS.md](gui/NODE_GRAPH_CANVAS.md)). Fine-grained `cardinality`, semantic `units`, `colourSpace`, and `spatialVsSpectral` fields remain optional future enrichments.
- **Side-effect-free `validate(scene_text)` pass (SHIPPED in `AgentSession::ValidateText`; sandbox hardening OPEN).** `Cst::ParseToCst` provides construction-free syntax parsing, and [`AgentSession::ValidateText`](../src/Library/Agent/AgentSession.cpp) runs `ParseToCst` + throwaway-`Job` `DeriveToJob` (with `>`-directive refusal and structured `AgentDiagnostic` line/column reporting) without mutating the live scene ([gui/VALIDATION_ARCHITECTURE.md](gui/VALIDATION_ARCHITECTURE.md)). Inline editor squiggle gutters, `ProjectRootJail` / validation resource caps, and out-of-process asset decode remain open hardening items.
- **Scene-text retention on `Job` (SHIPPED in the CST cutover).** Every CST-loaded `Job` retains its canonical `Cst::Document` (`GetCstDocument()`), serialized losslessly via `Cst::SerializeCst` so the GUI scene-text editor, `SaveEngine`, and `AgentSession` share one authoritative source of truth.
- **Generated bridge enum mirror** (`tools/gen_bridge_enums.py` — **OPEN**). Emit the C-ABI enums (panel mode, category, tool) to Kotlin constants + Obj-C `static_assert` mirrors so bridge-enum drift becomes structurally impossible ([gui/CROSS_PLATFORM_ARCHITECTURE.md](gui/CROSS_PLATFORM_ARCHITECTURE.md)). Today `SourceHygieneTest` pins `IJobVtableManifest.txt` and `kNumCategories` is public, while `gen_bridge_enums.py` remains unbuilt.

---

## 11. Phasing (with shipped status & remaining backlog)

Desktop-shared work lands on macOS + Windows simultaneously (principle 2); Android follows per its tiering (§10.4).

**Foundation status:** Single-document CST authority (`Cst::CstHeadVersion` CAS + exact-byte undo `Cst::DocReplaceChunkIfText` + `EditorUiSnapshot` `try_lock` reads, [gui/ADR_DOCUMENT_FIRST_EDITOR.md](gui/ADR_DOCUMENT_FIRST_EDITOR.md)), single-document FIFO render admission in `SceneEditController` (`SubmitAgentRenderAsync/Sync`, `SubmitProductionRenderSync`), side-effect-free `AgentSession::ValidateText`, loopback auth/proposal gates, and `EntityTemplates` + `OwnershipClosure` CRUD have shipped. Multi-document `SceneSnapshot` RCU / process-wide `RenderCoordinator` and `ProjectRootJail` / `IOutboundUrlPolicy` / C++ `ICredentialStore` remain open v2 hardening ([gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md](gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md), [gui/RENDER_COORDINATOR.md](gui/RENDER_COORDINATOR.md), [gui/AI_SECURITY_MODEL.md](gui/AI_SECURITY_MODEL.md)).

| Phase | Theme | Status |
|---|---|---|
| **0** | **Entity-creation / outliner** workflow + CST persistence for non-camera entities ([gui/ENTITY_CREATION.md](gui/ENTITY_CREATION.md)) | **PARTIALLY SHIPPED:** `EntityTemplates` (Light, Object, Material, Painter, Medium), `OwnershipClosure` reference-safe delete/duplicate/rename, macOS + Windows Outliner CRUD, and agent chunk CRUD shipped. Standalone `Category::Geometry` templates, `Shader`/`Modifier` categories, atomic glTF-into-open-scene import, and Android CRUD UI remain open. |
| **1** | **Approachability foundation** (Dir A, [gui/APPROACHABILITY_FOUNDATION.md](gui/APPROACHABILITY_FOUNDATION.md)) | **PARTIALLY SHIPPED:** Three-path Start Screen + `empty_starter.RISEscene`, Pause/Resume/Stop progressive viewport (A2), Draw Region / Render Active Region (A4), Environment HDRI editor, and Basic/Advanced disclosure shipped on desktop. `default_studio.RISEscene` (A1), drag-to-assign (A3), and CC0 asset library (A5) remain open. |
| **2** | **Cameras, views & render modes** (Dir B, [gui/CAMERAS_AND_VIEWS.md](gui/CAMERAS_AND_VIEWS.md), [gui/RENDER_MODES.md](gui/RENDER_MODES.md)) | **MOSTLY SHIPPED:** Transient `ViewportPose` + fly-then-stamp, axis-ball nav gizmo, `.RISEviews` sidecar + macOS/Windows `NamedViewsPanel` (MSVC v145 compile verified `b6663f1af`), P1–P3 render modes + desktop N-up panes shipped. B4b animation camera-cuts and B5 GPU wireframe panes remain open. |
| **3** | **Material instance editor + spectral widgets** (C1 + Dir D subset, [gui/MATERIAL_EDITOR.md](gui/MATERIAL_EDITOR.md), [gui/SPECTRAL_DIFFERENTIATORS.md](gui/SPECTRAL_DIFFERENTIATORS.md)) | **SHIPPED (Slices 1–3, 5, 6):** C1 soft-range sliders, inline 18×18 `PainterPreview` swatches, `Shared (N)`/`Fork`, 56×56 3D `MaterialPreview` shaderball header, D1 Blackbody Kelvin strip & Sellmeier optical glass presets/$n_d$/Abbe $V_d$, D2 CC0 Au/Ag/Cu/Al/Cr/Ti measured metals, and D3 0°–89° thin-film iridescence strip shipped on macOS + Windows. D1 monochromatic/CSV/prism preview remain open. |
| **4** | **Node-graph editors & MaterialX import** (C2 + sibling Object Graph + C3, [gui/NODE_GRAPH_CANVAS.md](gui/NODE_GRAPH_CANVAS.md)) | **C2 + Object Graph SHIPPED (S11–S23 + Slices 4–5); C3 OPEN:** Editable `NodeGraphCanvas` with pipe-coloured sockets/wires, 1-click `scalar_painter` adapter insertion, and 40×40 3D `MaterialPreview` swatches + sibling `ObjectGraphCanvas` (Objects + Geometry + CSG/instancing) shipped on macOS + Windows. C2 group frames/uber-node polish and C3 MaterialX import remain open. |
| **5** | **Simulation UI** ([SIMULATION_GUI_STATUS.md](SIMULATION_GUI_STATUS.md)) | **SHIPPED ON MACOS; WINDOWS PARITY OPEN:** macOS `SimulationsPanel` / `SimulationsModel` / `SimulationProbes` shipped with live playback, bake queue, and timeline ranges. Windows Qt6 mirror and VS2022 `rise-sim` target remain open. |
| **AI spine** | | |
| **A0** | **RISE agent server** (JSON-RPC / `--mcp` stdio / `--agent-http` loopback): 54 MCP / 49 chat tools, schema-gen, `AgentSession::ValidateText`, framebuffer/viewport read, render & simulation control ([gui/MCP_TOOL_SURFACE.md](gui/MCP_TOOL_SURFACE.md)) | **SUBSTANTIALLY SHIPPED:** 54 MCP / 49 chat tools (including 11 `sim_*` simulation verbs), integer `headVersion` CAS, proposals, and transports shipped. `resources/subscribe`, `prompts/list`, `export_movie`, `ProjectRootJail`, `SecretRedactor`, and `SchemaConformanceTest` remain open. |
| **A1–A3** | **In-app chat + 5 providers + vision loop** ([gui/LLM_AGENT_RUNTIME.md](gui/LLM_AGENT_RUNTIME.md)) | **CORE SHIPPED (macOS + Windows):** `AgentChatLoop` + 5-provider `AgentChatCodecs` (`Anthropic`, `Gemini`, `OpenAI` Responses API, `XAI`, `Local`), `ChatPanel` + `ProposalCard`, macOS Keychain & Windows Credential Manager (`CredentialStore`, MSVC v145 compile verified `b6663f1af`), tool calling, vision/imagine loop, prompt caching, and compaction shipped. `ExternalServerHub`, shared C++ `ICredentialStore`, OAuth, Linux TLS, and Android JNI chat remain open. |
| **Ongoing** | **Marquee spectral diagnostics & Light Mix** (D5–D7, [gui/SPECTRAL_DIFFERENTIATORS.md](gui/SPECTRAL_DIFFERENTIATORS.md)) | **OPEN** (top-bar resolved integrator + reason and EDR display + EV + 5 tone curves shipped; D5 probe heatmap/card, D6 cinema scopes, and D7 PT Light Mix remain open). |

---

## 12. Non-goals / deliberately NOT doing

- Four-up live path-traced quad view (cost non-starter; do 3 GL panes + 1 render pane, or named-view management instead).
- Node editor on the default path (gate behind "Advanced," KeyShot-style).
- Maya-style mode-swapped menus or dual property panels for one object; one canonical inspector.
- Marketing RISE as "real-time" / chasing D5/Lumion 30fps DXR (different, non-converging product).
- Fragmenting the interactive story into multiple modes (V-Ray IPR/Vision/Vantage confusion) — one progressive viewport.
- Hard-depending on a numpad for axis views.
- Showing the fast interactive denoise as if it were final (keep full OIDN for finals — the "denoise always on for final" rule).
- Making MaterialX RISE's native storage format (RGB-capping; import only).
- A second, parallel mutation path for AI edits (principle 6).

---

## 13. Spikes & resolutions

The deep-dives and 2026-07..2026-11 implementation slices resolved the original architectural questions. Remaining items needing follow-up:

1. **Lossless CST save & spectral preview cost** — **RESOLVED.** Every scene loads into a retained CST `Document` serialized losslessly by `SaveEngine::Save` (`Cst::SerializeCst` — byte-exact when unedited, minimal-diff when edited) with exact-byte undo via `Cst::DocReplaceChunkIfText`. For previews, flat spectral swatches (D1 Kelvin, D2 measured metals, D3 thin-film 0°–89° strip) and headless 2D/3D inspector/node swatches (`PainterPreview`, `MaterialPreview`) evaluate deterministically in shared C++ without touching the live film; drag-to-assign (A3, still open) keeps **commit-on-release** as its baseline. ([gui/ADR_DOCUMENT_FIRST_EDITOR.md](gui/ADR_DOCUMENT_FIRST_EDITOR.md), [gui/MATERIAL_EDITOR.md](gui/MATERIAL_EDITOR.md), [gui/SPECTRAL_DIFFERENTIATORS.md](gui/SPECTRAL_DIFFERENTIATORS.md))
2. **Axis/split-view ground truth** — **RESOLVED & SHIPPED.** Direction B landed symmetrically across shared C++ + desktop bridges (axis-ball gizmo, transient `ViewportPose`, `.RISEviews` sidecar, macOS/Windows `NamedViewsPanel`, and P3 N-up panes). ([gui/CAMERAS_AND_VIEWS.md](gui/CAMERAS_AND_VIEWS.md), [gui/RENDER_MODES.md](gui/RENDER_MODES.md))
3. **MCP transport** — **RESOLVED & SHIPPED.** Three transports share one `AgentRpc` / `AgentSession` core: in-process (`ChatPanel`), loopback HTTP (`AgentLoopbackHttpServer` / `--agent-http`), and stdio (`--mcp`). ([gui/MCP_TOOL_SURFACE.md](gui/MCP_TOOL_SURFACE.md))
4. **Provider-adapter abstraction** — **RESOLVED & SHIPPED.** `AgentChatLoop` + `AgentChatCodecs` normalize **5 providers** (`Anthropic`, `Gemini`, `OpenAI`, `XAI`, `Local`) over one tool-call and streaming transcript model. ([gui/LLM_AGENT_RUNTIME.md](gui/LLM_AGENT_RUNTIME.md))
5. **Side-effect-free `validate` dry-run** — **RESOLVED & SHIPPED.** Implemented in `AgentSession::ValidateText`: runs construction-free `Cst::ParseToCst` plus an isolated throwaway-`Job` `DeriveToJob` pass with `>`-directive refusal and structured `AgentDiagnostic` output, never mutating the live scene. (Sandbox hardening — `ProjectRootJail`, resource caps, out-of-process asset decode — remains open in [gui/VALIDATION_ARCHITECTURE.md](gui/VALIDATION_ARCHITECTURE.md).)
6. **Structured CST edits vs wholesale-rewrite for AI** — **RESOLVED & SHIPPED.** Both GUI and `src/Library/Agent/` mutate the retained CST `Document` directly via param edits and chunk CRUD (`ApplyAgentParamEdit`, `insert_chunk`, `remove_chunk`, `EntityTemplates`), persisting uniformly through `SaveEngine::Save`. ([gui/MCP_TOOL_SURFACE.md](gui/MCP_TOOL_SURFACE.md), [gui/ENTITY_CREATION.md](gui/ENTITY_CREATION.md))

**Remaining tracked GUI follow-ups:**
- **Seed-asset license + repo-size budget:** A shipped studio HDRI + starter material library is required for A1/A5/A3 (`default_studio.RISEscene`).
- **Film & Environment transaction-arm cleanup (ADR §4 phase 3b):** Migrate Film, Film-preset, and Environment (`radiance_map` erase) arms onto the unified undoable transaction path.
- **Bridge enum codegen:** Replace convention-guarded C++/Swift/Qt/Kotlin bridge enum switches with a manifest check or `tools/gen_bridge_enums.py`.
- **Standalone Geometry templates & Shader/Modifier UI categories:** Populate `TemplatesFor(Geometry)` and add `IJob` enumeration + inspector arms for `Shader` and `Modifier` categories ([gui/ENTITY_CREATION.md](gui/ENTITY_CREATION.md)).
- **Windows Qt6 `SimulationsPanel` parity:** Mirror macOS `SimulationsPanel` / timeline baked ranges / live playback controls in `build/VS2022/RISE-GUI/` ([SIMULATION_GUI_STATUS.md](SIMULATION_GUI_STATUS.md)).

(The `pipe`-descriptor-field and cloud-auth questions are **decided** — see §16.)

---

## 14. Sub-specifications index (`docs/gui/` — 15 active specs)

Each is a focused design or implementation doc spun off from this roadmap. Each honors §1 principles and carries an Android-tier note (§10.4) where applicable.

**Direction & UX specifications:**

- [gui/APPROACHABILITY_FOUNDATION.md](gui/APPROACHABILITY_FOUNDATION.md) — Start Screen (`empty_starter.RISEscene` shipped; `default_studio.RISEscene` open), Environment HDRI editor, progressive viewport, drag-to-assign (A3), region render (A4), asset libraries (A5), disclosure (A6).
- [gui/CAMERAS_AND_VIEWS.md](gui/CAMERAS_AND_VIEWS.md) — Named Views (`.RISEviews` sidecar + macOS/Windows `NamedViewsPanel`), axis-ball gizmo, camera list, fly-then-stamp, split view.
- [gui/RENDER_MODES.md](gui/RENDER_MODES.md) — interactive viewport shading/debug modes (`Lit`, `Wireframe`, `Normals`, `Depth`, `Albedo`, `ObjectID`) and desktop N-up panes.
- [gui/MATERIAL_EDITOR.md](gui/MATERIAL_EDITOR.md) — C1 instance-first inspector, C2 node graph, shared 3D `MaterialPreview` shaderball, C3 MaterialX import (open), `IPainter`/`IScalarPainter` pipe mapping (Slices 1–6 shipped).
- [gui/NODE_GRAPH_CANVAS.md](gui/NODE_GRAPH_CANVAS.md) — shared `GraphLayout`/`GraphLayoutSidecar`/`ConnectionLegality`, C2 Material & Painter `NodeGraphCanvas`, and sibling `ObjectGraphCanvas` (Objects + Geometry + CSG/instancing).
- [gui/SPECTRAL_DIFFERENTIATORS.md](gui/SPECTRAL_DIFFERENTIATORS.md) — Slice 6 D1 Kelvin/Sellmeier, D2 measured metals (Au/Ag/Cu/Al/Cr/Ti), D3 0°–89° thin-film iridescence strip (shipped), plus D4 EDR/gamut, D5 auto-router probe/heatmap, D6 scopes, D7 Light Mix.
- [gui/MCP_TOOL_SURFACE.md](gui/MCP_TOOL_SURFACE.md) — RISE agent/MCP server (`AgentRpc`, `AgentMcpAdapter`, `--mcp`, `--agent-http`): 54 MCP / 49 chat tools (including 11 `sim_*` verbs), `SceneGrammar` schemas, `validate`, framebuffer observation, permissions/scopes.
- [gui/LLM_AGENT_RUNTIME.md](gui/LLM_AGENT_RUNTIME.md) — `AgentChatLoop`, 5-provider `AgentChatCodecs` (`Anthropic`, `Gemini`, `OpenAI`, `XAI`, `Local`), `ChatPanel` + `ProposalCard`, vision/imagine loop, staged autonomy.
- [gui/CROSS_PLATFORM_ARCHITECTURE.md](gui/CROSS_PLATFORM_ARCHITECTURE.md) — shared-core boundary (`src/Library/`), thin desktop/Android bridges, present surfaces, Android staged tiering.

**Architecture, state, security & tracking docs:**

- [gui/ADR_DOCUMENT_FIRST_EDITOR.md](gui/ADR_DOCUMENT_FIRST_EDITOR.md) — architectural decision record: retained CST `Document` as single source of truth, `EditorUiSnapshot` reads, derived dirty state, exact-byte CST undo.
- [gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md](gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md) — H1–H3 hardening record, single-writer authority, `headVersion` CAS, staged `AgentProposal` queue, and unbuilt v2 multi-doc `SceneSnapshot`.
- [gui/RENDER_COORDINATOR.md](gui/RENDER_COORDINATOR.md) — single-document FIFO render admission in `SceneEditController` (`RenderJobId`, `RenderClass`) vs unbuilt process-wide multi-document coordinator & thumbnail queue.
- [gui/VALIDATION_ARCHITECTURE.md](gui/VALIDATION_ARCHITECTURE.md) — side-effect-free `AgentSession::ValidateText` + `AgentDiagnostic` + `ParameterPipe`, vs unbuilt inline squiggle gutters & `ProjectRootJail`.
- [gui/ENTITY_CREATION.md](gui/ENTITY_CREATION.md) — `EntityTemplates` (Light/Object/Material/Painter/Medium), `OwnershipClosure` reference-safe CRUD, vs open `Geometry` templates & atomic glTF import.
- [gui/AI_SECURITY_MODEL.md](gui/AI_SECURITY_MODEL.md) — threat model: loopback auth, proposal gates, `>`-directive refusal, Keychain/Credential Manager, vs unbuilt `IOutboundUrlPolicy`, `ProjectRootJail`, `ICredentialStore`, `SecretRedactor`, `ExternalServerHub`.

---

## 15. Acceptance-criteria template (every spec must fill this in)

Each direction/phase carries a filled-in version of this block before it is implementation-ready:

- **Tests** — unit/integration tests + the invariant each guards; for engine-touching work, a correctness invariant (RMSE-vs-reference or byte-identity) per the repo's testing discipline.
- **Platform parity** — what ships on macOS / Windows / Android (which tier), and what degrades gracefully where.
- **Performance budget** — frame-time / latency target for interactive paths; no production-render regression (cite the L8 ~0.4% bar).
- **Memory budget** — peak RSS delta; any per-feature cap (e.g., thumbnail cache size).
- **Accessibility** — keyboard path, focus order, no numpad-only / colour-only dependence.
- **Packaging** — app-bundle / installer / Android-asset implications; seed-asset provenance.
- **Migration** — scene-format / ABI impact; auto-migration tool if any.
- **Rollback** — feature flag / default-off path; how to disable without breaking saved scenes.

---

## 16. Confirmed decisions (2026-06-19)

- **`ParameterSemantics` structure on `ParameterDescriptor`**: **adopt.** **Separate fields** — pipe (`color`/`scalar`/`either`), cardinality, `requireSingle`, semantic units, colour space, spatial-vs-spectral — **not** a single overloaded `pipe` enum (the review's correction). Declared source of truth for socket coloring + MCP schema typing; `Job::ResolveOrDiagnoseScalar` stays as enforcement. Owned by [gui/MATERIAL_EDITOR.md](gui/MATERIAL_EDITOR.md) + [gui/VALIDATION_ARCHITECTURE.md](gui/VALIDATION_ARCHITECTURE.md).
- **Credential interface:** one reference-counted `ICredentialStore : IReference` in `src/Library/Agent/`; **drop `ISecretStore`**. Matches RISE's `IReference` convention.
- **Agent subsystem name:** `src/Library/Agent/`, presented as "the RISE agent tool surface (MCP protocol)." **Avoid the bare `MCP` token** in type/dir names — `src/DRISE/MCPClientConnection` already means *Master Control Program* (distributed render), unrelated to Model Context Protocol.
- **Cloud auth:** ship **API-key paste first** (no redirect infra), OAuth/PKCE second.
- **Android scene-save:** **Tier A** — wire `nativeSaveAs` to real `.RISEscene` save (today it saves only the rendered image).
- **Still open:** seed-asset license + repo-size budget (§13).
