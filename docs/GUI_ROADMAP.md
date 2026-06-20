# RISE GUI Roadmap — From Render Viewer to AI-Operable Creative Tool

**Status:** IDEATION / DESIGN. No code. This is the umbrella vision + phasing doc; the per-stage deep-dive specs under [gui/](gui/) carry the detailed designs.
**Owner:** Aravind Krishnaswamy
**Scope:** Make the RISE GUI (macOS, Windows, and — staged — Android) materially more capable and more *approachable*, without cloning Blender/Maya. Three pillars: (1) an approachability foundation, (2) creative-power features (cameras/views, material editor, spectral-only differentiators), and (3) a first-class **LLM/agent integration** that can fully operate scenes and the engine. All three rest on one architectural spine: **the `.RISEscene` text file is the canonical source of truth, and the shared C++ library owns everything that isn't a native widget.**

This doc supersedes nothing — it sits alongside [INTERACTIVE_EDITOR_PLAN.md](INTERACTIVE_EDITOR_PLAN.md) (the viewport that shipped), [CAMERAS_ROADMAP.md](CAMERAS_ROADMAP.md) (optics), and [ROUND_TRIP_SAVE_PLAN.md](ROUND_TRIP_SAVE_PLAN.md) (persistence — design record; **largely implemented**, see §2), and pulls their GUI-facing threads into one picture.

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

**Correction (2026-06-19 review): round-trip save is largely IMPLEMENTED, not pending.** An earlier draft of this section trusted [ROUND_TRIP_SAVE_PLAN.md](ROUND_TRIP_SAVE_PLAN.md)'s stale *"Design — pending"* header instead of reading the code — the repo's documented "read the status, not the plan" trap. Reality: [../src/Library/SceneEditor/SaveEngine.cpp](../src/Library/SceneEditor/SaveEngine.cpp) (1,898 lines, "Phase 6.4") implements transform round-trip (Mode A in-place splice + Mode B managed `override_object`), **property** round-trip for cameras/lights/materials/media (Phase B), and **created-camera** persistence (Phase C), all covered by [../tests/SaveEngineTest.cpp](../tests/SaveEngineTest.cpp) (2,372 lines, incl. *"cloned camera persisted through save → reload"*). **The genuine gap is creation, not persistence:** created *non-camera* entities don't persist yet, and there is no GUI entity-creation workflow at all — `SceneEditController::CloneActiveCamera` is the only creation path (no outliner / add / delete / import). See §11 Phase 0 (re-scoped) and the new entity-creation spec (§14).

**Not built on either desktop platform yet:** canonical axis views (top/front/side), split/quad viewport, entity *creation* from the GUI (only mutation of existing entities), material editing beyond raw text, region render, asset libraries, sensible-default scene, any AI integration.

**Parity note to verify:** a Windows-side audit guessed macOS already has axis/split views; a direct read of the macOS code says it does not. Treat both desktop platforms as lacking these until confirmed by eye (spike in §11).

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
- **A4. Region / ROI render** — modifier+drag a box; auto-clear for finals (Blender's #1 footgun is the region leaking into the final).
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
- **D4. Jakob-Hanika gamut warning** — warn on color picks landing in the failing blue corner ([JH_LUT_GAMUT.md](JH_LUT_GAMUT.md)); a uniquely-spectral honesty feature.
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
Android keeps the *core abilities* (it consumes the same C++ via JNI) but gets a **mobile-first UI shell** with different layout + touch interactions, and a feature **tiering**:
- **Tier A (must-have, incl. LLM):** scene browse/open, render view (HDR where supported), basic parameter edit, timeline scrub, **full LLM chat + agent operation** (chat is mobile-native — a plausible Android headline).
- **Tier B (touch-adapted):** gizmo transforms, named views, camera list, material *instance* sliders.
- **Tier C (deferred / desktop-first):** node-graph editor, EDR scopes, split view. Present as "edit on desktop" gracefully, never a broken control.

Every deep-dive spec must include an "Android tier + interaction" note.

### 10.5 Net-new shared-library components surfaced by the specs
The deep-dives converged on a small set of genuinely-new shared-C++ pieces that more than one direction depends on — building these once unblocks several features:

- **A `ParameterSemantics` structure on `ParameterDescriptor`** (not a single overloaded `pipe` enum — the review's correction). The descriptor does **not** currently encode the `IPainter`-vs-`IScalarPainter` distinction — GGX `ior`/`film_ior` are declared identically to true color slots, with routing downstream in `Job::ResolveOrDiagnoseScalar`. The structure carries **separate fields**: pipe (`color`/`scalar`/`either`), cardinality, `requireSingle`, semantic units, colour space, spatial-vs-spectral. Both the material editor (socket coloring, [gui/MATERIAL_EDITOR.md](gui/MATERIAL_EDITOR.md)) and the MCP schema (typing for the LLM, [gui/MCP_TOOL_SURFACE.md](gui/MCP_TOOL_SURFACE.md)) consume it.
- **Side-effect-free `validate(scene_text)` pass.** The parser has no no-op validation mode today (`ParseAndLoadScene` mutates / logs to the global log). Design: parse into an **isolated throwaway real `Job`** + a command policy (a faithful capturing mock would need 217 pure virtuals, and the real accept/reject logic lives in `Job::ResolveOrDiagnoseScalar`); end-state is `parse → IR → validate IR`. Keystone for AI self-correction ([gui/MCP_TOOL_SURFACE.md](gui/MCP_TOOL_SURFACE.md)); also powers inline editor diagnostics.
- **Scene-text retention on `Job`.** The `Job` does not retain source text after parse. Buffer-on-parse so the GUI "show me the code" panel, the MCP scene-text resource, and the AI read path share one accessor.
- **Generated bridge enum mirror** (`tools/gen_bridge_enums.py`). Emit the C-ABI enums (panel mode, category, tool) to Kotlin constants + Obj-C `static_assert` mirrors so the recurring bridge-enum-drift bug (the `case 5:` fall-through) becomes structurally impossible ([gui/CROSS_PLATFORM_ARCHITECTURE.md](gui/CROSS_PLATFORM_ARCHITECTURE.md)).

---

## 11. Phasing (with the AI spine woven in)

Desktop-shared work lands on macOS + Windows simultaneously (principle 2); Android follows per its tiering (§10.4).

**Foundation critical path (must precede the feature phases below).** The 2026-06 review established that the feature work rests on foundations that have to land first, roughly in order:

1. **Document identity + immutable snapshot model** — `(UUID, revision)`; RCU-style published snapshot ([gui/TRANSACTION_MODEL.md](gui/TRANSACTION_MODEL.md)). Everything that reads scene state concurrently (render jobs, thumbnails, external clients) depends on it.
2. **Transaction correctness** — single-writer, commit-under-lock, drag-as-preview, precondition conflict ([gui/TRANSACTION_MODEL.md](gui/TRANSACTION_MODEL.md)).
3. **Render coordination** — one process-wide pool lease, priority + anti-starvation, snapshot-backed isolated jobs ([gui/RENDER_COORDINATOR.md](gui/RENDER_COORDINATOR.md)).
4. **Validation + security** — legacy-tolerant `validate` (IR gutter + sandboxed deep `Job`) + the AI threat model ([gui/VALIDATION_ARCHITECTURE.md](gui/VALIDATION_ARCHITECTURE.md), [gui/AI_SECURITY_MODEL.md](gui/AI_SECURITY_MODEL.md)).
5. **Entity creation** — outliner + all-family create/delete + tombstone persistence + dependency graph ([gui/ENTITY_CREATION.md](gui/ENTITY_CREATION.md)).

The table below is the *feature* sequencing; it assumes the foundation above is in place (A0's `validate` needs #4; A2's edit tools need #2; thumbnails/previews across Dir A/C/D need #1 + #3).

| Phase | Theme | Depends on |
|---|---|---|
| **0** | **Re-scoped** (round-trip save already ships — transforms/properties/created-cameras): build the **entity-creation / outliner** workflow + Phase-C persistence for non-camera entities | new `gui/ENTITY_CREATION.md` (§14) |
| **1** | Approachability foundation (Dir A) | A2 leans on `ResizeFilm`; A1 on auto-router + OIDN |
| **2** | Cameras & views (Dir B) | multi-camera infra (shipped) |
| **3** | Material instance editor + spectral widgets (C1 + Dir D subset) | descriptor infra (shipped) |
| **4** | Full node-graph editor (C2); MaterialX/OpenPBR import (C3) | C2 = new graph-*topology* serialization (material *property* round-trip already ships). **C3 is independent of C2** — RGB→painter via the JH boundary; can land alongside Phase 3. |
| **AI spine (parallel, starts early)** | | |
| **A0** | RISE agent server (MCP protocol): read scene + grammar resource + framebuffer + `validate` + render-control | **substantial net-new** — JSON-RPC, 3 transports, schema-gen, safe `validate`, source-text retention, framebuffer encoding, headless lifecycle. The *scene-read/render* capabilities it wraps mostly exist; the protocol layer does not. No round-trip save needed for the text-level loop. |
| **A1** | In-app chat panel + provider adapters (Claude / Gemini / local); L0→L1 diff-review | A0 |
| **A2** | Edit/create tools via `SceneEditController`; L2 operate-with-guardrails + scoped permissions | edit/property/created-camera persistence ships **today**; only **non-camera creation** persistence is gated on Phase 0 |
| **A3** | Vision feedback loop + curated external MCPs (HDRI/asset, spectral n,k, web); L3 autonomous tasks | A1, A2 |
| **Ongoing** | Marquee spectral: auto-router heatmap (D5), EDR scopes (D6), Light Mix (D7) | D5 → expose the private `ProbeResult` (additive readback); D6 → XYZ-at-film-resolve retention + scope compute; D7 → per-light AOV infra + attribution/denoise contract. All ride the RENDER_COORDINATOR snapshot / isolated-job path (#1, #3). |

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

The deep-dives resolved most of the original open questions. Remaining items needing your decision are under "Still open."

1. **Spectral hover-preview cost** — *Resolved.* Flat spectral swatches (Kelvin/λ/CSV/metal) are a `SpectralPacket::GetXYZ → XYZtoRec709RGB` compute — microseconds, live on drag, no path trace. Only the prism preview and full-object thin-film rim need a commit-time cached thumbnail. Baseline for drag-to-assign is **commit-on-release**; live full-object hover-preview is spike-gated and falls back to RGB-only if it misses budget. ([gui/APPROACHABILITY_FOUNDATION.md](gui/APPROACHABILITY_FOUNDATION.md), [gui/SPECTRAL_DIFFERENTIATORS.md](gui/SPECTRAL_DIFFERENTIATORS.md))
2. **Axis/split-view ground truth** — *Resolved (static search).* Neither desktop platform has axis or split views today; Direction B is greenfield and lands symmetrically. Recommend an eyeball confirm before building. ([gui/CAMERAS_AND_VIEWS.md](gui/CAMERAS_AND_VIEWS.md))
3. **MCP transport** — *Resolved.* Three transports behind one interface: in-process (in-app client), loopback-HTTP (external clients on a running GUI), `rise --mcp-stdio` (headless/CI). ([gui/MCP_TOOL_SURFACE.md](gui/MCP_TOOL_SURFACE.md))
4. **Provider-adapter abstraction** — *Resolved.* `ILLMProvider` normalizes Claude tool-use / Gemini function-calling / OpenAI-compatible-local onto one type set, with `ProviderCaps` for capability differences. ([gui/LLM_AGENT_RUNTIME.md](gui/LLM_AGENT_RUNTIME.md))
5. **`validate` dry-run** — *Resolved.* No side-effect-free pass exists today; design parses into an **isolated throwaway real `Job`** + a command policy (capturing-mock rejected — 217 pure virtuals), with a `parse → IR → validate IR` end-state ([gui/VALIDATION_ARCHITECTURE.md](gui/VALIDATION_ARCHITECTURE.md)).
6. **Round-trip vs wholesale-rewrite for AI** — *Resolved.* MCP tools are tagged "works today via rewrite-and-reload" (the cheap-start MVP) vs "needs round-trip save" (structured in-place edits). Structured transform/property/created-camera saves ship **today**; only **non-camera creation** persistence remains a Phase-0 item. ([gui/MCP_TOOL_SURFACE.md](gui/MCP_TOOL_SURFACE.md))

**Still open — need a decision:**
- **Seed-asset license + repo-size budget.** A shipped studio HDRI + starter material library is required for drag-to-assign to be useful (A1/A5); needs a license-clean source and a repo-size budget. Proposed home: `scenes/Default/assets/`.

(The `pipe`-descriptor-field and cloud-auth questions are now **decided** — see §16.)

### 13a. Open architectural blockers (code-backed review, 2026-06-20) — NOT implementation-ready

A third, code-grounded review found several round-3 hardenings are not yet *mechanically true against the codebase*. **The foundation is not implementation-ready until these are resolved — and the two ★ items cannot be settled on paper; they need a code prototype.**

1. **◑ SUBSTANTIALLY RESOLVED — increments A + B landed + independently verified (a genuinely-immutable, render-faithful snapshot for the common case); bounded residuals + #2 (restore/publish) remain.** On `feature/gui-snapshot-prototype`:
   - **✅ Material + CSG immutability FIXED & test-pinned (increment A).** Mutable leaves are now **cloned, not addref'd** (`SnapshotLeafClone.{h,cpp}` — an ABI-safe free function with `dynamic_cast` dispatch, *not* a new `IMaterial` virtual); `Object::CloneSnapshot()` is **virtual** with a `CSGObject` override that recursively clones operands; the snapshot is **frozen** (mutators private, `friend Scene`). A's standalone test count was **41/0** (break-it-first: material-rebind + CSG-not-sliced; 35/4 pre-fix → 41/0). `CSGObjectIdentityTest` + the material/editor suite pass.
   - **⚠ Residual immutability gaps (honestly bounded, NOT fixed).** **SSS shaders + interior media stay addref-shared** — a genuine `ResetRuntimeData`-vs-`Shade` UAF for subsurface scattering (the only *stateful* shaders); and **5 exotic materials** (SubSurfaceScattering, RandomWalkSSS, GenericHumanTissue, 2 luminaires) still bleed (not faithfully ctor-reconstructable). The 15 standard reflectance/BRDF materials + all stateless shaders/media are clean. **Increment B adds bounded residuals too:** **ONB-constructed cameras are refused** (clone returns null → not render-faithful, per the existing `CameraCommon` warning); the **heterogeneous global medium** is addref'd (editor can't edit it); object-level interior-media share the SSS residual; unknown out-of-tree light/medium/camera/material types fall back to addref. Closing the SSS gap + an ONB-aware camera rebuild is a separate, narrowly-scoped follow-up.
   - **✅ Render-faithful capture DONE & verified (increment B).** Clones the Phase-B-mutable **lights** + the active **camera** (per-type — thin-lens fstop/focal/blades preserved, not pose-only) + **film** + **homogeneous global medium**; addref's the (editor-immutable) **environment**; luminaries follow from the cloned objects. **`SceneSnapshotTest` = 97/0** on my own run (adds break-it-first light-edit-vs-live, thin-lens-params, scene-complete); `FilmIntrospectionTest` 128/0, `MultipleCamerasTest` 264/0; clean build. **Cost ~48 µs warm for 100 objects + lights + film + camera.**
   - **#2 restore/publish:** primitives now built (`Scene::RestoreFromSnapshot` + sampler rebuild + transaction rollback — #2a/#2b) **but with P1 correctness gaps found in the 5th review** (see §2's P1 register). Not yet a safe foundation. See [gui/TRANSACTION_MODEL.md](gui/TRANSACTION_MODEL.md) §3.5.
2. **⚠ NOT COMPLETE — primitives exist + pass *narrow* tests, but a 5th code-backed review (2026-06-20) found P1 correctness bugs; NOT a safe foundation yet (P1 register at the end of this item).** **#2a built + committed (`f92cc9bf`):** `Scene::RestoreFromSnapshot` — clone-on-restore swap of objects/lights/camera/film/medium + `InvalidateSpatialStructure` so the TLAS rebuilds. `tests/SceneRestoreTest.cpp` = **37/0** (round-trip, snapshot-reusable, publish-over-divergent-live, and render-valid: post-restore `IntersectRay` hits the restored object with correct identity); ~16 µs / 6 obj. **#2b(a) DONE & verified:** sampler rebuild on restore/edit via an **O(1) Scene light-topology generation gate** (`Scene::mLightTopologyGeneration`, bumped by `RestoreFromSnapshot` + the 5 `SceneEditor` light-mutation sites; `RayCaster::AttachScene` rebuilds the samplers when the generation advanced, keeping the same-pointer fast path). This **fixed a live latent bug** — any in-place light edit on a reused caster previously kept a STALE sampler (wrong NEE), not mitigated anywhere. `tests/SamplerRebuildOnRestoreTest.cpp` = **18/0** (5 assertions fail pre-fix); full suite (**145 tests**) + clean Xcode `RISE-GUI` arm64. **#2b(b) DONE & verified (`5e4fe915`):** investigation found the alleged drag double-apply / redoable-after-reject is **NOT** in the shipping editor (the live drag is already record-only → one undo entry; no gesture cancel/reject path existed) — so this **added** the transactional-rollback foundation: `SceneEditController::{Begin,Rollback,End}Transaction` via `SceneEditController::{Begin,Rollback,End}Transaction`. **⚠ The initial mechanism (snapshot baseline + `RestoreFromSnapshot` + `DiscardUndoTo`, 52/0) was SUPERSEDED by the 5th-review remediation (`3b32b7ba`): rollback now uses identity-safe inverse-edit undo — no snapshot, no `DiscardUndoTo` — and `tests/SceneEditTransactionTest.cpp` = 85/0 incl. the multi-camera + shared-material adversarial cases.** Commit is record-only; the interactive drag path is byte-unchanged; full editor suite + the snapshot/restore/sampler trio green. **Not yet wired into a UI gesture or an AI-staging/external-client path — that's feature integration (follow-up).** Residuals: moving a mesh luminaire doesn't yet bump the light generation (object path already invalidates the TLAS); restore inherits the snapshot leaf residuals (ONB camera, SSS/medium leaves).

   **P1 register (5th code-backed review, 2026-06-20).** *Remediation (commit `3b32b7ba`): the editor's **rollback was re-based on inverse-edit undo**, resolving **P1-1 / P1-2 / P1-5 for the rollback path** (the deep-clone snapshot/restore that still carries them is now **EXPERIMENTAL**, off the rollback path); **P1-4's crash case is fixed** (an object material-binding emitter-set change bumps the light generation). **Still open (by design / documented, NOT bugs):** **P1-3** (film/rasterizer/animation aren't history-backed → rollback can't revert them — documented contract) and **P1-6** (snapshot concurrency safety — deferred with the isolated-render path). *(The **P1-4 exitance-slot** sub-case and a **separate pre-existing absolute-transform-undo** bug were also surfaced and are now **FIXED** — exitance-slot by 6th-review fix B (`9f1d0c91`, the `MarkEditEntityDirty` bump fires on any slot edit of an emissive material); transform-undo by `c0face11` via component-decomposition capture/restore, full suite 145/145.)* Verified: `SceneEditTransactionTest` 85/0 (multi-camera + shared-material adversarial cases) + full editor suite. **6th-review (re-review, 2026-06-20) verdict: HOLDS at the code level — no P1** (no live crash / shipping-path data loss); rollback is sound for the v1 non-concurrent scope; the multi-camera + shared-material tests genuinely assert instance-identity survival. It found 4 P2 polish items: **(A, latent — unreachable today, no UI/C-API caller)** `BeginTransaction` opened *inside* an open SceneEditor composite can undershoot the baseline → guard `mCompositeDepth > 0`; **(B, variance not crash)** geometry/transform edits on a *mesh luminaire* (and exitance-slot edits on an emissive material) don't bump the generation → stale light-SELECTION pmf (estimator stays **unbiased** — per-sample area/Le are live); **(C1/C2 docs — FIXED here)** the TRANSACTION_MODEL §3.5 stale lines now carry a supersession banner and this bullet's superseded 52/0 snapshot-mechanism text is corrected. **A + B are now FIXED (commit `9f1d0c91`, test-first, RED proven pre-fix): A → a composite-open guard in `BeginTransaction` via new `SceneEditor::IsCompositeOpen()`; B → a light-generation bump on spatial/exitance edits to a luminaire via the shared `RunObjectInvariantChain` + `MarkEditEntityDirty` choke points (new `BumpSceneLightGenerationIfMaterialEmits`). `SceneEditTransactionTest` 102/0; C1/C2 fixed in `18aaa366`. All 6th-review items closed.** Original findings below:*
   - **P1 multi-camera data loss.** Snapshot captures only the *active* camera, but `RestoreFromSnapshot` clears the **whole** camera manager and re-adds that one → any rollback in a multi-camera document **deletes the other cameras**. Fix: capture the full camera collection + active selection.
   - **P1 identity / sharing not preserved.** Deep-clone gives each object its **own private** material — objects that shared one named material no longer do (a later edit to the named material doesn't reach the restored objects). CSG operands are cloned both as manager entries **and** recursively inside the CSG, so the restored CSG references a different instance than the named entry. Fix: rebuild the **named-reference graph**, don't deep-clone per consumer.
   - **P1 film / rasterizer / animation rollback incomplete.** Film edits go through `Job::SetFilm` (reallocates the canonical frame store + controller dim caches); restore calls only `Scene::SetFilm`, leaving Job/controller caches at the rejected dims. Rasterizer params + animation frame-count are **not captured at all**.
   - **P1 sampler generation misses emitter-affecting object edits (crash risk).** Only *direct light* edits bump `mLightTopologyGeneration`; an object material-binding / slot edit that adds/removes an emitter or changes exitance does **not** — a cached luminary switched to non-emissive can later hit `pEmitter->emittedRadiance()` with a **null** emitter.
   - **P1 can't represent absence / failure.** Env + global-medium are only *assigned* when present (the setters can't **clear** them → restoring "no env" over a scene that has env leaves it); unknown objects skipped on capture; unknown lights deleted+skipped on restore; `AddItem` results ignored; `RestoreFromSnapshot` returns void and `RollbackTransaction` reports success even on partial failure.
   - **P1 published-snapshot safety unresolved.** Shaders / object interior-media / 5 exotic materials / heterogeneous media / ONB cameras / out-of-tree types remain shared-or-refused; the **SSS `ResetRuntimeData`-vs-snapshot-render race** is incompatible with the eventual RCU published-render use — so the snapshot is **not yet safe for concurrent render-off-snapshot** (the original goal).

   **Doc reconciliation also owed:** TRANSACTION_MODEL §14.2/§3.5.4/§3.7 + several lines still say restore "NOT YET BUILT" / "18/18" / "no new source file" / "pose-only camera" (all stale post A/B/#2a/#2b); `Scene.h` keeps the obsolete pre-#2b-a "recreate the rasterizer" sampler caveat.
3. **External-client authority** — propose-only vs handlers that directly commit + return `newEpoch`; headless stdio has no owner to approve. Needs separate committed/proposed contracts + a headless autonomy policy.
4. **Source-text + redaction contract** — `•` is 3 UTF-8 bytes (not 1); `apply_scene_text` of redacted text would overwrite real secrets; source text must share the document revision. Needs a single-byte mask + no-wholesale-when-redacted / span-edits / crypto-bound restore.
5. **Validation error taxonomy** — `BARRIER_COMMAND` is mislabeled "parser rejects" (the parser *accepts and runs* those); split `parser_error` / `validation_policy_error` / `warning`. "No crash/OOM" requires *actual* out-of-process construction.
6. **Render anti-starvation not executable** — jobs are non-resumable yet the policy "resumes" a cancelled job (restart, never finish); the K×33ms bound ignores a running non-preemptible production render and an unproven 128²/256² thumbnail fit. Needs resumable checkpoints or schedule-to-completion with an honest bound.
7. **Persistence still overstated + cross-spec fixes** — Phase B does **not** cover Painter, so C1 painter edits + A3 "round-trippable" material/HDRI don't persist yet; the all-family dependency graph assumes painter/geometry/shader/modifier introspection that doesn't exist. Plus: camera full-optics via `CameraSnapshot`; `> remove camera` doesn't exist (camera-delete must refuse); camera thumbnail key still uses scene-epoch not `(UUID,revision)`; Light-Mix Top-K needs a pilot pass; undo-attribution conflates history + audit; the stale LLM "reload = undoable transaction" paragraph; save/load/export need `DocumentId`; the cross-platform boundary must add a GPU-render/compute backend for B5/D6.

**Recommended closure order:** #1+#2 (snapshot/COW + transaction atomicity, via a **prototype**) → #3+#4 (external-client + source-text) → #6 (render scheduling/lifetime) → #7 (persistence + cross-spec) → feature-level camera + Light-Mix. The doc set must **not** claim closure until these resolve.

---

## 14. Deep-dive spec index (`docs/gui/`)

Each is a focused design doc spun off from this roadmap. Each must honor §1 principles and include an Android-tier note (§10.4).

- [gui/MCP_TOOL_SURFACE.md](gui/MCP_TOOL_SURFACE.md) — RISE MCP server: full tool/resource catalog, schema auto-generation from `SceneGrammar`, `validate`, framebuffer resource, permissions/scopes, transport.
- [gui/LLM_AGENT_RUNTIME.md](gui/LLM_AGENT_RUNTIME.md) — agent loop, provider adapters (Claude/Gemini/local), chat UX, staged autonomy, guardrails/diff-review, "show me the code" panel.
- [gui/APPROACHABILITY_FOUNDATION.md](gui/APPROACHABILITY_FOUNDATION.md) — default scene, progressive viewport, drag-to-assign, region render, asset libraries, disclosure.
- [gui/CAMERAS_AND_VIEWS.md](gui/CAMERAS_AND_VIEWS.md) — named views, axis-ball gizmo, camera list, fly-then-stamp, split view; scene-language vs UI-only split.
- [gui/MATERIAL_EDITOR.md](gui/MATERIAL_EDITOR.md) — instance-first view, node graph, serialization, MaterialX import, IPainter/IScalarPainter mapping.
- [gui/SPECTRAL_DIFFERENTIATORS.md](gui/SPECTRAL_DIFFERENTIATORS.md) — spectral picker, measured metal, thin-film, gamut warning, auto-router heatmap, EDR scopes, Light Mix.
- [gui/CROSS_PLATFORM_ARCHITECTURE.md](gui/CROSS_PLATFORM_ARCHITECTURE.md) — shared-core strategy, bridge consolidation, Android staged fallback/tiering.

**Foundation & correction docs (2026-06-19 review round):**

- [gui/CURRENT_STATE_AUDIT.md](gui/CURRENT_STATE_AUDIT.md) — **code-first ground truth** (implemented-in-library vs wired-into-GUI vs absent, per platform, cited to `file:line`). Read this before trusting any other GUI doc's status.
- [gui/TRANSACTION_MODEL.md](gui/TRANSACTION_MODEL.md) — authoritative state, scene-revision epochs, transactions + preconditions, AI staging, multi-client reconciliation, undo attribution, external-edit reconciliation.
- [gui/RENDER_COORDINATOR.md](gui/RENDER_COORDINATOR.md) — one coordinator for all render consumers (viewport / production / thumbnails / node previews / probe / agent): priority, exclusivity, cancellation, stale-generation rejection, isolated preview jobs.
- [gui/VALIDATION_ARCHITECTURE.md](gui/VALIDATION_ARCHITECTURE.md) — safe side-effect-free validation (isolated `Job` + command policy → parse-to-IR); why "parser-identical AND side-effect-free" was impossible as first specified.
- [gui/ENTITY_CREATION.md](gui/ENTITY_CREATION.md) — outliner + add/delete/duplicate/import + reference-safe deletion + Phase-C persistence beyond cameras (the real Phase-0 gap).
- [gui/AI_SECURITY_MODEL.md](gui/AI_SECURITY_MODEL.md) — threat model: prompt injection, cloud-data consent, SSRF, path-root enforcement, secret redaction, destructive-command policy, curated external-MCP ownership.

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
