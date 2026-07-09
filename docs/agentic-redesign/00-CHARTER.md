# RISE Agentic Redesign — Design Charter

> **Status:** design-in-progress. This charter is the shared foundation for a set of
> parallel design documents. It is NOT yet implemented and is intended to **supersede
> the current GUI/edit/scene-construction architecture** once ratified. Nothing here
> changes engine behavior yet — this is a design effort to be reviewed for holes before
> any code is written.

## 1. Thesis & positioning

RISE is repositioning as **"the 3D package for nerds"** — agent-forward by design, with the
entire product thesis being *agentic in nature*. Two tenets drive the architecture:

1. **The UI is always a live expression of the scene document.** The UI is a human-readable,
   quickly-editable *view* of the canonical scene; the two never diverge. (The user's words:
   "UI is just a human readable and quickly editable expression of the scene file.")
2. **The UI is dynamic** — built adaptively from the scene's structure and mutating as a human
   works *with an agent* to build the scene.

The differentiated place in the world: **a production spectral renderer whose scene is a
canonical, diff-able, version-controllable program — edited by humans and agents through the
same structural/textual interface, with a UI that is a pure projection of that program.** Think
"OpenSCAD's ergonomics + a real renderer + agent-native," not "Maya with a script console."

## 2. The architectural decision: Model A → Model B

- **Model A (today):** a rich, live, mutable in-memory `Scene` is the working state. User/agent
  actions are *commands that mutate the model*; undo/redo is reconstructed from inverse-edits;
  the file is a (lossy) serialization of the model.
- **Model B (go-forward):** the **document is canonical**; the scene is *derived* from it
  (parse → evaluate). Actions *edit the document*; undo/redo is document versioning; the UI is a
  *view* of the document.

**We are committing to Model B.** Rationale (validated by the 8-round editor/transaction
de-brittling post-mortem in `docs/gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md` and the
companion memory): nearly every defect in that arc was a symptom of *multiple mutable
representations that must be kept consistent* (live scene ↔ inverse-edit history ↔ dirty set ↔
selection ↔ save). Model B dissolves that class of bug by having **one source of truth**.

## 3. The canonical pivot (read this carefully — it governs every facet)

```
   Text (.RISEscene)  ⟷  Lossless CST   →   Derived Scene   →   Render
        ▲                  (CANONICAL,        (engine objs:
        │                   retained)          managers, BVH,
   humans + agents          ▲   │  bind         realized geom)
   edit text                │   ▼
   (diffs)            Dynamic UI  ──structured edits──┐
                      (widgets per CST node)          │
                                                      ▼
                                            (both text + structured edits
                                             mutate the ONE canonical CST)
```

- The practical canonical object is a **retained, lossless Concrete Syntax Tree (CST)** —
  comments, whitespace, and formatting preserved. **Text** is its faithful serialization;
  the **derived scene** is its evaluation.
- Both the GUI and the agent edit *through the CST* (one edit pathway). The GUI is "just another
  agent."
- Undo/redo = CST version history (atomic, correct by construction).

## 4. Locked decisions vs. open decisions

**Locked (do not re-litigate; design around these):**
- L1. Model B: document canonical, scene is a *pure, deterministic derivation* of the CST.
- L2. Single edit pathway: GUI structured-edits and human/agent text-edits both mutate the one CST.
- L3. **Iteration is declarative.** Imperative `FOR`/`DEFINE` macro-expansion is removed. Repetition
  is expressed as declarative primitives (instancers / function expressions, à la the guilloché
  kinematic generator and the sweep/instancer/disk technique-features). Distinguish: *homogeneous
  instancing* → declarative generator (instances derived, not authored); *"typing-shortcut" loops*
  → desugared into explicit, separately-editable entities at author time.
- L4. Document state (in the CST) vs. session/view state (ephemeral: selection, active tool,
  orbit-preview camera, render-in-progress) are cleanly split. Only document state is canonical.
- L5. **Identity is an immutable `NodeId`** (lineage identity — survives rename + reparse; lives in
  each Version's persistent `identityRoot`). **`name-path`** (e.g. `objects/sphere.material`) is the
  **addressing** scheme — human/agent-readable, built on RISE's name-keyed managers, resolving to a
  NodeId within a version, and changing on rename. *(Refined by [`01-DECISIONS.md`](01-DECISIONS.md)
  D9/D15/D26/D36/D44 — the earlier "name-path is the identity currency" wording is superseded; the
  round-4 "name-reuse identity serial" was a Model-A patch that NodeId + addressing replace.)*
- L6. Descriptors are the schema. The 2026-04 descriptor-driven parser (`IAsciiChunkParser::Describe()`)
  already declares each chunk's parameters + types; this same schema drives (a) the dynamic UI,
  (b) agent-edit validation, (c) CST node shape. Do not invent a parallel schema.
- L7. This design **supersedes** the current `SceneEditor`/`SceneEdit`/`EditHistory`/transaction/
  rollback subsystem, the round-trip `SaveEngine` gap, and the hand-built accordion panels. Facet
  docs must state precisely what they delete.

**Open (design around the working assumption, but flag explicitly for the human reviewer):**
- O1. **Canonical form = lossless-CST pivot** (working assumption) vs. pure text-canonical (buffer is
  truth). Design for lossless-CST-pivot; note where text-canonical would differ.
- O2. **Interactive bar = debounced-commit direct manipulation** (working assumption; aligned with the
  agentic edit→preview cadence) vs. 60Hz incremental derivation. Design for debounced-commit; note the
  delta if 60Hz were required.
- O3. Scene-file format version bump strategy & coexistence with v6 scenes.

## 5. Cross-cutting invariants (every facet must honor)

- INV-1 **One source of truth.** No facet may introduce a second mutable representation of scene state
  that must be sync'd with the CST.
- INV-2 **Scene = f(CST), pure & deterministic.** Same CST ⇒ same scene, no hidden order-dependence.
- INV-3 **Incremental.** A localized CST edit must derive only the affected subgraph (no whole-world
  rebuild per edit). Latency budget is a first-class design output.
- INV-4 **Lossless text round-trip.** A structured edit must not gratuitously reformat untouched text;
  a hand-edited file must survive parse→serialize unchanged.
- INV-5 **Stable identity.** Durable references — selection, agent references, UI bindings, undo
  lineage — key on the immutable **`NodeId`** (not the name-path), so they survive edits and renames
  (D9/D15/D36/D44). name-path is addressing only.
- INV-6 **One edit pathway** (L2). Two clients (GUI, agent); one mechanism.

## 6. Facet map (avoid overlap; note your neighbors)

| # | Doc | Owns | Hands off to |
|---|-----|------|--------------|
| 1 | `10-scene-language-and-cst.md` | Lossless CST, parser evolution (one-way→retained tree), text↔CST round-trip + formatting/comments, node identity (NodeId lineage + name-path addressing, L5/D44), declarative iteration (L3) replacing FOR/DEFINE/hal/macros, coverage of all chunk types, format-version bump | CST shape → 2,3,4 |
| 2 | `20-derivation-engine.md` | CST→Scene as incremental/memoized/deterministic function; dependency graph; granularity; reuse of the surviving "apply-layer"; interaction with deferred realization / TLAS / photon passes; order-independence audit of `Job`; perf targets | consumes CST(1); apply-layer reuse ↔ 3 |
| 3 | `30-edit-model-and-history.md` | Supersede `SceneEdit`/`EditHistory`/transactions/rollback/identity-serial with CST versioning; unify structured + text edits; gesture debouncing; document-vs-session split (L4); undo/redo/branch semantics; **explicit deletion inventory** of the current edit subsystem | edits target CST(1); triggers derivation(2); drives UI(4) |
| 4 | `40-dynamic-ui.md` | UI as pure function of CST + descriptors (L6); widget-per-node; adaptive/growing panels; two-way binding widget↔CST node; split form/source live view; reactive propagation as agent edits; supersede current accordion/category/`SceneEditController` panels; shared-C++ + Mac/Windows/Android-tier | consumes CST(1)+descriptors; edits via 3 |
| 5 | `50-agentic-surface.md` | RISE MCP server (read CST/text, propose patch, validate→derive→render, structured errors); diff-able/git-native/reviewable scenes; GUI-as-just-another-agent unification; product framing & differentiation; agent-edit safety/validation | uses edit pathway(3), validation from 1, render from 2 |
| 6 | `60-supersession-and-migration.md` | Comprehensive inventory of the CURRENT architecture & each component's fate (delete/evolve/reuse); phased non-big-bang migration & v6-scene coexistence; scene-corpus migration tooling; risk register (incl. the two tar-pits: derivation latency, lossless-format preservation) | reconciles 1–5 |

## 7. Deliverable spec (each facet doc)

Each doc must contain, in this order:
1. **Current-state grounding** — what exists in the tree today for this facet (cite real files), so the
   design is reality-based, not abstract.
2. **The Model-B design** — concrete enough that an engineer could start building. Data structures,
   interfaces, control flow, the seams into existing code. Diagrams welcome.
3. **Delete / Evolve / Reuse** — explicit fate of current components this facet touches.
4. **Hard problems & open questions** — be honest; name the tar-pits and the unknowns.
5. **Cross-facet dependencies & assumptions** — what you assumed about neighbors (for synthesis to
   reconcile). Flag any conflict with a Locked/Open decision.
6. **First-slice implications** — what this facet contributes to a minimal end-to-end vertical (one
   chunk type, text⟷CST⟷derived-scene, one schema-generated widget, live incremental re-derive).

## 8. Grounding pointers (read before designing)

- Repo guide: `AGENTS.md`, `README.md`, `docs/README.md`, `CLAUDE.md`.
- Parser (descriptor-driven, the schema source): `src/Library/Parsers/README.md`,
  `src/Library/Parsers/AsciiSceneParser.cpp`, the `IAsciiChunkParser` pattern.
- Construction API & assembly: `src/Library/RISE_API.h`, `src/Library/Interfaces/IJob.h`,
  `src/Library/Job.cpp`, `Job::InitializeContainers/SetPrimaryAcceleration`.
- Current GUI/edit subsystem (to be superseded): `src/Library/SceneEditor/` (`SceneEditController`,
  `SceneEditor`, `SceneEdit`, `EditHistory`), `src/Library/SceneEditor/SaveEngine.cpp`.
- The de-brittling post-mortem (the *why*): `docs/gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md`,
  `docs/gui/` specs, `docs/GUI_ROADMAP.md`.
- Declarative-feature precedents: the guilloché kinematic generator, `sdf_geometry`, `sweep_geometry`,
  `instancer`/disk features; deferred scene realization (realize-from-roots + cascade at
  `RayCaster::AttachScene`).
- Scene language quirks to be removed/migrated: FOR/DEFINE, `hal()`, `$(...)`, macros.

> **Design only. Do NOT modify any source, build, or scene files. Write design docs only.**
