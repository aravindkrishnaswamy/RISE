# RISE UI Redesign — Design Brief

**Status:** **EXECUTED DESIGN INPUT.** The macOS and Windows workspace redesign
shipped from this brief; it remains the product-principles and requirements
record, while `REDESIGN_IMPLEMENTATION.md` describes the delivered UI.

**Purpose of this document:** input to a ground-up UI redesign of the RISE desktop application (macOS + Windows). It describes what RISE is, who uses it, what the UI does today, what the redesigned UI must cover (current features plus the committed roadmap), the design principles the product has already adopted, hard technical constraints, and explicit non-goals.

**Owner:** Aravind Krishnaswamy
**Date:** 2026-07-10
**Scope of the redesign:** the desktop GUI (macOS and Windows, which ship at near-parity from one shared C++ core). Android and the Blender add-on are out of scope for this pass, but the design system should not preclude a later mobile adaptation.

---

## 1. What RISE is

RISE (**Realistic Image Synthesis Engine**) is a physically-based **spectral path tracer** — a renderer that simulates light transport wavelength-by-wavelength rather than in RGB, producing physically accurate images of 3D scenes. It is a long-lived C++ codebase recently modernized into a cross-platform interactive application.

Engine capabilities that matter to the UI:

- **Progressive CPU rendering.** RISE renders on the CPU and converges: the image starts noisy/coarse and refines over seconds to minutes. There is no instant "final" frame. The GPU is used only to *display* the CPU-computed framebuffer (Metal on macOS, DXGI on Windows) and for OIDN denoising.
- **Multiple integrators, auto-routed.** Path Tracing (PT), Bidirectional Path Tracing (BDPT), VCM, and MLT, in both RGB ("pel") and spectral variants. A shipped **auto-router** (`auto_rasterizer`) picks the best integrator per scene via a fast probe and reports its decision — the GUIs surface "Auto → PT/BDPT/VCM" after a render. Users should rarely need to pick an integrator manually.
- **True spectral features no RGB renderer has:** measured spectral power distributions, blackbody/Kelvin color, measured-metal n,k conductor Fresnel, thin-film interference, dispersion through named glasses (BK7, SF11, diamond), and honest gamut handling via the Jakob–Hanika spectral uplift.
- **HDR/EDR output end-to-end.** The viewport displays extended-dynamic-range content (Metal EDR on macOS, scRGB fp16 on Windows). Exports include EXR stills and ProRes 4444 / HDR10 movies.
- **Always-on OIDN denoising** (GPU-accelerated where available) so even early passes look presentable.
- **Animation:** camera/object timelines with named paths, scrubbing, and movie export.
- **Specular Manifold Sampling (SMS), photon mapping, volumetrics, subsurface scattering** — a deep feature set aimed at hard light transport (caustics, glints, participating media).

**The defining architectural fact: the scene is a text file.** Every scene is a human-readable, diffable `.RISEscene` document with a documented, machine-introspectable grammar. The GUI does not own an opaque binary scene — it is an *authoring surface over text*. GUI edits round-trip into the file preserving comments and formatting; hand edits and GUI edits coexist. This is treated as the product's biggest differentiator (git-friendly scenes, reproducibility, and the ideal substrate for AI operation) and the redesign must lean into it, not hide it.

---

## 2. Product positioning

RISE is **not** trying to be Blender, Maya, or a real-time engine. The positioning:

1. **KeyShot-grade approachability** for the first thirty seconds: open the app → a good image with zero clicks; drag a material onto an object; drag an HDRI onto the viewport to relight.
2. **Spectral differentiation** for depth: the advanced features are ones RGB tools physically cannot replicate (live spectral curve editing, thin-film sliders, measured metals, "explain the auto-router").
3. **The first AI-operable renderer.** A first-class in-app LLM agent (plus an MCP server for external clients like Claude Desktop) can read, validate, edit, and render scenes because the scene language is text with a self-describing grammar. The AI and the GUI are two parallel authoring surfaces over the same file.
4. **Honest about being a converging renderer.** Never marketed as "real-time"; the UI celebrates progressive refinement (convergence indicators, pause/resume) instead of faking instant results.

Competitive touchstones the team has already studied and adopted lessons from: KeyShot (drag-to-assign, library-first navigation, opt-in node graph), Unreal (master material / instance split), Substance (per-node live thumbnails), Arnold RenderView (IPR controls, region render), Blender (region-leak footgun to avoid), Maya Hypershade (cautionary tale — icon-less nodes, destroyed layouts). NN/g progressive-disclosure research caps disclosure at **two levels**.

---

## 3. Users

1. **The curious newcomer / design-adjacent user.** Wants a beautiful, physically accurate image of a model without learning a scene language. Must reach a "win" in zero clicks (default scene) and do their first real work by dragging thumbnails, not typing. Never forced into a node graph or an integrator decision.
2. **The technical 3D artist / lighting person.** Comfortable in Blender/KeyShot-class tools. Wants camera management, material editing with live preview, region renders for iteration, HDRI swaps, animation, and export. Uses Basic panels most of the time, opens Advanced when needed.
3. **The rendering engineer / researcher / power user.** Reads and writes `.RISEscene` text directly, cares about integrator selection and MIS behavior, wants scopes, false color, variance heatmaps, RMSE views, and full parameter access. The text editor and log are their home. Today's primary user is this persona; the redesign must widen the funnel without alienating them.
4. **The AI agent (and its human supervisor).** An LLM operating the scene through chat and tools. The human needs to see what the AI did (scene diffs), approve or reject changes, and undo anything. AI actions share the same undo/history as human actions.

---

## 4. The UI today (what exists and ships)

Both desktop apps are real (~9k lines each), near-identical in capability, and thin shells over one shared C++ `SceneEditController`. macOS = SwiftUI + Metal EDR view; Windows = Qt6 + DXGI HDR widget.

**Current main-window anatomy:**

- **Viewport** (center): live progressive render at ~30 Hz interactivity. Orbit/pan/zoom/roll camera tools; click-to-pick objects; drag gizmos for object translate/rotate/scale. During camera motion the render drops to a coarse preview scale (up to 1/32 resolution) and refines back to full quality over ~500 ms of idle, with a denoised polish pass. The viewport displays HDR/EDR content with tone-curve and exposure controls, and an "EDR Preview" toggle on macOS.
- **Toolbar**: Select, Orbit, Pan, Zoom (+ Roll) tool buttons with cursor binding. Deliberately pared down; object transform tools exist in the core but are currently hidden.
- **Right-side properties panel**: an accordion over the scene's entity categories (Camera, Rasterizer/Integrator, Objects, Lights, plus Film/Materials/Media). Selecting an entity (by list or by clicking it in the viewport) expands its section and shows **descriptor-driven property rows** — every parameter the scene grammar declares auto-appears as an editable row (text/number/vector/enum/reference) with description tooltips, presets, and unit labels. Edits are undoable and round-trip to the scene file. Composed materials and some entity types are read-only.
- **Camera controls**: multi-camera scenes; a camera list/dropdown to switch the active camera; "Add Camera" (clone the active one). Camera edits (orbit, roll, lens parameters) round-trip to the file.
- **Scene text editor**: syntax-highlighted `.RISEscene` editor with grammar-driven autocomplete and diagnostics. The text is the source of truth; the editor is a first-class surface, not a debug view.
- **Animation timeline** (bottom bar): scrub through the scene's timeline; named animation paths; render-animation export to ProRes 4444 / HDR10 movies.
- **Render controls**: start/stop interactive and production renders; production renders disable editing while running; post-render the UI surfaces the auto-router's decision ("Auto → BDPT").
- **Log panel**: engine log output.
- **Save/Save-As**: full round-trip scene save — byte-exact when nothing changed, minimal-diff on edits, refuses to clobber external modifications, atomic writes. Unsaved-changes tracking gates the Save button.

**What the current UI is *not*:** it has no visual design language to speak of — it is a functional developer-built tool UI (default platform chrome, stock widgets, no iconography system, no empty states beyond a "Render to see the scene" placeholder, no onboarding). That is a large part of why this redesign is being commissioned.

**Known gaps in today's UI (all confirmed by a code-level audit):**

- No entity creation of any kind except camera-clone: no add/delete/duplicate/import for objects, lights, materials, or media; no outliner.
- Empty first-run: the app opens blank and needs a hand-authored scene file before it shows anything.
- No asset library, no thumbnails, no drag-and-drop of anything.
- No HDRI/environment swap UI (the engine supports it; nothing exposes it).
- No region/ROI render UI (the engine honors regions for all pixel integrators; no GUI draws a box).
- No named views, no axis snaps (front/top/side), no split viewport.
- No material editing beyond raw property rows / raw text — no swatches, no visual editor, no node graph.
- No progressive-disclosure tiers: every parameter row shows at once (parameter walls).
- No convergence indicator, no explicit pause/resume of refinement.
- No AI/chat surface (the MCP server and agent runtime are designed but net-new).

---

## 5. What the redesigned UI must cover

The redesign target is the **future-state desktop app**: everything in §4 that ships today, plus the five committed roadmap directions below. Treat them as the feature canvas; the redesign's job is a coherent information architecture, visual language, and interaction system that houses all of it without becoming Blender.

### Direction A — Approachability foundation

- **A1 Sensible default scene.** First launch is never blank: an empty-state with a primary action ("Start from the default studio") loads a shipped studio scene — neutral object on a ground plane, studio HDRI, Auto integrator, denoise on, auto-exposure — that renders a good image in zero clicks.
- **A2 The viewport is the renderer.** Explicit Start / Pause / Resume / Restart / Stop controls for progressive refinement; a convergence readout ("Pass 12 / 64") and a user stop-rule ("refine to N samples then stop"); pause-on-navigation and restart-on-edit are existing behaviors to surface legibly. A quality slider maps to samples-per-pass.
- **A3 Drag-to-assign.** Drag a material thumbnail from the library onto an object in the viewport — the object highlights under the drag, assignment commits on release (KeyShot semantics, with Shift/Alt/Cmd modifier variants). Drag an HDRI thumbnail onto the viewport background to relight the scene. Hover-preview of the material on the object is an opt-in enhancement, not the baseline.
- **A4 Region / ROI render.** A discoverable **Draw Region** control (plus an expert shortcut) turns the next viewport drag into a box; the viewport gives an in-canvas "Drag to choose an area · Esc to cancel" prompt instead of exposing the internal word "armed." Only the box refines at full resolution (huge iteration speedup), with a persistent editable overlay that reports its dimensions and share of the frame. Ordinary production renders always ignore and temporarily suspend the viewport region; **Render Active Region** is a separate, explicit production action. Under an integrator that ignores regions, both actions are disabled with an explanation rather than pretending to work.
- **A5 Thumbnail asset libraries.** A browsable, searchable grid of material and HDRI thumbnails (shipped seed set + user library + scene-local assets), the drag source for A3. Thumbnails are path-traced previews on a standard rig, cached.
- **A6 Progressive disclosure + workspace presets.** Every property panel splits into **Basic** (the few knobs that matter) and one **Advanced** disclosure (everything else) — exactly two levels, never more. Two workspace layouts: **Simple** (viewport + slim inspector + render controls; text editor and log hidden) and **Full** (everything). Default is Simple.

### Direction B — Cameras & views

- **B1 Named views**: save the current viewport pose with a thumbnail; restore on click. Session/UI state, distinct from scene cameras, with an explicit **"promote to scene camera"** action that writes a real camera into the file.
- **B2 Axis snaps + an axis-ball navigation gizmo**: six face targets (front/back/top/bottom/left/right), turntable orbit default, a Home view. No numpad dependence.
- **B3 "Fly then stamp"**: navigate freely with cheap preview rendering, then commit the current angle to a camera.
- **B4 Camera list panel**: thumbnails per camera, click-to-activate, bind-camera-to-time-range on the timeline.
- **B5 (later) Right-sized split view**: three cheap wireframe/GL orientation panes + one rendered pane, deterministic single↔quad toggle. Explicitly *not* four live path-traced panes.

### Direction C — Material editor

- **C1 Instance-first editor (the default surface).** A flat, friendly view over a material's slots: color swatches, sliders, presets — driven by the same descriptor metadata as the property rows. This is what most users see when they "edit a material." No graph required.
- **C2 Node graph (gated behind "Advanced").** A visual node canvas for material authoring: default node is an OpenPBR-style uber-material; **per-node live path-traced thumbnails**; socket color encodes the type system (color-pipe vs physical-scalar-pipe — a real engine distinction: yellow = color, grey = scalar); organization tools (frames, comments, search) from day one. Never the default path to a basic result.
- **C3 MaterialX/OpenPBR import** (import only; native format stays RISE's spectral superset).

### Direction D — Spectral differentiators (the marquee widgets)

These are the features that make RISE visibly unlike any RGB renderer, and they deserve hero treatment in the design:

- **D1 Spectral color picker / curve editor**: alongside a normal color picker — a Kelvin temperature slider, a 380–780 nm monochromatic wavelength slider, drag-in a measured-SPD CSV and see the curve plotted next to the swatch, and a named-glass dropdown (BK7 / SF11 / diamond) with a live prism/dispersion preview.
- **D2 Measured-metal picker**: choose real metals (gold, copper, titanium…) whose color comes from measured n,k physics data, not an RGB approximation.
- **D3 Thin-film slider**: film thickness in nanometers with an angle-reactive swatch — soap-bubble and anodized-titanium iridescence, live.
- **D4 Gamut honesty warning**: when a picked color lands where the spectral-uplift model degrades (deep blue corner), the UI says so — a uniquely-spectral honesty feature.
- **D5 "Explain the auto-router"**: a per-region heatmap over the render plus a plain-language rationale ("Auto chose BDPT here: glossy indirect variance is high"). No competitor can replicate this.
- **D6 EDR cinematography overlays**: false color (IRE legend), zebra stripes, waveform monitor, and a spectral vectorscope plotting true chromaticity — pro-video-style scopes over a genuinely HDR viewport.
- **D7 Spectral Light Mix**: after one render with per-light AOVs, re-balance each light's contribution and color temperature live without re-rendering.

### Direction E — AI / agent integration (the marquee differentiator)

- **In-app chat panel** (dockable) where the user converses with an LLM (Claude, Gemini, or a local model — provider sign-in lives in settings) that can read the scene, read the grammar, see the rendered framebuffer, propose edits, validate them against the real parser, apply them, and render.
- **Staged autonomy the user can dial**: L0 advisor (chat only) → L1 propose-and-confirm (AI generates a **scene diff**; the user reviews/approves like a code review) → L2 operate-with-guardrails (applies + renders within scoped permissions) → L3 autonomous tasks ("make 5 lighting variations and render thumbnails").
- **Diff review is a first-class UI moment**: AI-proposed changes render as a readable text diff of the scene file with approve/reject. All AI actions land in the same undo history as human edits.
- **"Show me the code"**: any selection or AI action can reveal the corresponding scene-text chunk — the bridge between the GUI world and the text world, and the same surface the AI speaks.
- The MCP server also serves external clients (Claude Desktop, IDEs) — the in-app panel is one client of the same tool surface.

### Everything-else inventory (must exist somewhere in the IA)

Entity creation and an **outliner** (add/delete/duplicate/import for objects, lights, materials, media — the audited #1 workflow gap); HDRI/environment section in the inspector; render queue/production-render flow with progress and time estimates; animation timeline with named paths and camera binding; export (EXR/PNG stills, ProRes/HDR10 movies); scene text editor with diagnostics; log; undo history; save state and external-modification warnings; settings (providers/API keys, library locations, display/EDR options).

---

## 6. Design principles (already adopted by the product — honor them)

1. **Approachable by default, powerful on demand.** A fresh scene gives a good image with zero decisions. Depth is one disclosure level away, never on the default path. Never gate a basic result behind a node graph (Houdini trap) or mode-swapped menus (Maya trap).
2. **Two disclosure levels, maximum.** Basic / Advanced. Research-backed hard cap.
3. **One canonical inspector.** Never two property panels for the same object; no modes that relabel the same menus.
4. **The viewport is the renderer.** One progressive viewport — not separate "preview" and "IPR" and "final" modes (the V-Ray fragmentation the team explicitly rejects). The design should make convergence legible and beautiful (noise → clarity is the product's heartbeat).
5. **Text is the source of truth — celebrate it.** The scene file, diffs, and "show me the code" are features, not shame. Git-friendly, reproducible scenes are a headline.
6. **One mutation path.** GUI edits, AI edits, and hand edits all flow through the same controller, the same undo stack, the same save. The design can rely on *everything* being undoable — including AI actions.
7. **Honest states.** Never present a denoised draft as final; never show a region box the integrator will ignore; never claim real-time. Convergence indicators, badges, and rationale text keep the tool truthful.
8. **Desktop parity by construction.** macOS and Windows ship the same capabilities from one shared core. The design system must be expressible in both SwiftUI and Qt: platform-native chrome where it matters (menus, dialogs, shortcuts), shared visual language everywhere else. Avoid designs that depend on exotic single-platform chrome.

---

## 7. Hard constraints the design must respect

- **Progressive, converging images.** Every rendered pixel surface (viewport, thumbnails, camera previews, node previews) starts coarse/noisy and refines. Design for that: skeleton/blur-up states, convergence affordances, and never blocking the UI on "the render finishing." Renders are also *cancel-and-restart* on edit — rapid edits mean rapid restarts, which the UI should absorb gracefully.
- **CPU renderer, one render pool.** Thumbnails, previews, and the viewport share one CPU. A render coordinator serializes them by priority (interactive first). Expect thumbnail grids to populate progressively, not instantly.
- **HDR/EDR viewport.** The canvas can be brighter than white (EDR highlights). UI chrome around it should be a neutral, color-accurate dark surround (like pro video/photo tools) so it never competes with or distorts the image. Overlays (gizmos, region boxes, badges, scopes) must stay legible over arbitrary HDR content.
- **Descriptor-driven panels.** Property rows are *generated* from grammar metadata (name, type, description, presets, units, Basic/Advanced tag). Design a **row system** (per value type: bool, number+unit, vec3, enum, color/painter reference, filename…) rather than bespoke per-entity panels — that's how the implementation scales and how new engine parameters appear for free.
- **Long-running operations.** Production renders and movie exports take minutes to hours; editing is disabled during production renders today. Design the running/disabled/progress states deliberately.
- **Accessibility baselines** (from the team's acceptance template): full keyboard path, sensible focus order, no numpad-only interactions, no color-only signifiers.
- **Text editor is load-bearing.** Monospace scene text with syntax highlighting, autocomplete, and inline diagnostics must remain a first-class citizen (persona 3 and the AI both live there).

---

## 8. Visual identity notes

There is no existing brand system to preserve — the current apps use stock platform widgets, and the redesign may define the visual identity from scratch. Direction guidance:

- **Dark-first, image-forward.** The rendered image is the hero; chrome recedes. Neutral dark grays (not tinted) around the canvas for color-critical evaluation; a light theme is secondary.
- **Scientific-instrument character.** RISE's personality is "physics, honestly" — spectra, wavelengths, measured data. Motifs like spectral gradients (380–780 nm), waveform/scope aesthetics, and precise typography suit it better than playful/consumer styling. The D-series widgets (curve editors, scopes, heatmaps) are the natural expression of the brand.
- **An existing logo asset exists** ("RISE App Logo" PSD) but should be treated as a draft, not a constraint.
- **Iconography** needs a full system: tools (select/orbit/pan/zoom/roll), entity categories (camera/light/material/object/media/film/integrator), render states, autonomy levels, and node sockets (color vs scalar pipes).
- **Type**: a precise UI sans plus a good monospace (scene text, diffs, log, coordinates readouts).

---

## 9. Non-goals (explicitly rejected — do not design these)

- Four-up live path-traced quad views (three cheap orientation panes + one render pane is the accepted shape).
- A node editor on the default path; node graphs stay gated behind Advanced.
- Maya-style mode-swapped menu sets or duplicate property panels.
- "Real-time" positioning or chasing 30 fps GPU-raster previews.
- Multiple parallel interactive-render modes (one progressive viewport).
- Numpad-dependent navigation.
- Presenting the fast interactive denoise as final output.
- A second mutation path for AI edits that bypasses undo/history.
- Cloud asset marketplace / in-app purchases.

---

## 10. Success criteria

1. **Zero-click win:** a first-run user sees a beautiful rendered image within seconds of launch, with an obvious next step (drag a material, orbit the camera).
2. **Thirty-second workflow:** open model → drag material → drag HDRI → region-refine → export, without opening the text editor or choosing an integrator.
3. **No parameter walls:** default panels show ≤ ~7 controls per entity; everything else is exactly one "Advanced" away.
4. **Power preserved:** the text editor, log, full parameter set, and scopes are reachable in ≤2 interactions from the Full layout; scene-text users lose nothing.
5. **Spectral identity is visible:** a screenshot of the app is recognizably a *spectral* renderer (curve editors, Kelvin/λ pickers, scopes), not a generic DCC.
6. **AI legibility:** a user can always answer "what did the AI just change, and how do I undo it?" within one glance and one click.
7. **Honesty:** every progressive/denoised/region/auto-routed state is visibly labeled.

---

## 11. Reference documents (in the RISE repository)

For deeper detail, the repo carries full specs the redesign can mine:

- `docs/GUI_ROADMAP.md` — umbrella vision, principles, phasing, non-goals.
- `docs/gui/CURRENT_STATE_AUDIT.md` — code-verified ground truth of what exists.
- `docs/gui/APPROACHABILITY_FOUNDATION.md` — Direction A interaction specs (default scene, viewport controls, drag-to-assign, region render, asset library, disclosure).
- `docs/gui/CAMERAS_AND_VIEWS.md`, `docs/gui/MATERIAL_EDITOR.md`, `docs/gui/SPECTRAL_DIFFERENTIATORS.md` — Directions B/C/D deep dives.
- `docs/gui/LLM_AGENT_RUNTIME.md`, `docs/gui/MCP_TOOL_SURFACE.md`, `docs/gui/AI_SECURITY_MODEL.md` — Direction E (chat panel, autonomy levels, diff review, guardrails).
- `docs/gui/CROSS_PLATFORM_ARCHITECTURE.md` — shared-core strategy and what is platform-specific.
- `docs/INTERACTIVE_EDITOR_PLAN.md` — the shipped viewport/editor implementation record.

## 12. Glossary

- **`.RISEscene`** — the human-readable text scene format; the canonical source of truth.
- **Integrator / rasterizer** — the light-transport algorithm (PT/BDPT/VCM/MLT); "Auto" picks one per scene and explains why.
- **Spectral vs pel** — wavelength-based vs RGB rendering paths; spectral is the differentiator.
- **OIDN** — Intel Open Image Denoise; always-on denoising of progressive renders.
- **EDR** — extended dynamic range display (brighter-than-white highlights on capable monitors).
- **SPD** — spectral power distribution; a light/color described as a curve over wavelength.
- **IPainter / IScalarPainter** — the engine's two value pipes: colors vs physical scalars; surfaced in the node graph as socket types.
- **MCP** — Model Context Protocol; the tool surface AI agents use to operate RISE.
- **Descriptor** — grammar metadata that auto-generates property rows and AI tool schemas.
