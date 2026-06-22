# RISE Agentic Redesign — Synthesis & Overview

> **Status:** **review rounds 1–6 complete** (no P0s in any). The reviews found 8 P1 + 2 P2 (r1),
> 8 P1 + a P2 batch (r2), 8 P1 (r3), 9 P1 (r4), 7 P1 (r5), and 7 P1 (r6), all resolved authoritatively
> in [`01-DECISIONS.md`](01-DECISIONS.md) (**D1–D51**), which **supersedes the reconciliations in §3
> and the first-slice in §6 below**, overrides the facet docs where they conflict, (r5/D44)
> **overrides the charter's L5/INV-5**, and (r6/D47/D51) **supersedes the legacy `docs/gui/`
> RENDER_COORDINATOR/AI_SECURITY specs** where they conflict. This document synthesizes the six facet
> designs and their seams; read [`00-CHARTER.md`](00-CHARTER.md) for the locked/open decisions, then
> [`01-DECISIONS.md`](01-DECISIONS.md) for the resolutions (later rounds amend earlier decisions; r3
> layered the derived-scene model and gated its complexity; r4 added full Derived/Prepared **stamps**
> keying **artifacts** off the immutable Version; r5 made runtime semantics precise; r6 fixed the
> runtime/coordinator details — config resolved **post-DerivedScene** (auto-route may probe), asset
> identity = **transitive byte closure** (pinned for queued jobs), **one render slot** (previews yield
> to pinned renders), `status:ok` needs **completion** not just stamp-match, pinned renders get
> **`RenderJobId`** identity, and commit is honestly **semantic-only** with opt-in awaited validation;
> motion blur preserved as a
> time-interval scene — and **D37 corrects a factual error** in r3's command census).

## 0. Reading guide (for the reviewer)

| Doc | Owns | Scrutinize for |
|---|---|---|
| [`00-CHARTER.md`](00-CHARTER.md) | Thesis, Model B decision, locked/open decisions, invariants | Are the locked decisions actually right? |
| [`01-DECISIONS.md`](01-DECISIONS.md) | **Authoritative** round-1 resolutions (D1–D10) | Read FIRST after the charter; it overrides §3/§6 here and the facet docs |
| [`10-scene-language-and-cst.md`](10-scene-language-and-cst.md) | The CST, parser→retained-tree, NodeId identity (name-path = addressing), declarative iteration (FOR→`instance_array`/`let`/`expr`) | **R1** (tree representation), lossless round-trip gate, generator expressiveness |
| [`20-derivation-engine.md`](20-derivation-engine.md) | Incremental `derive(CST)→Scene`, memo+dep-graph, apply-layer reuse | **Latency budget** (the second tar-pit), order-independence audit |
| [`30-edit-model-and-history.md`](30-edit-model-and-history.md) | CST versioning replacing the whole edit/transaction subsystem | **R1** (depends on persistent CST), deletion inventory completeness |
| [`40-dynamic-ui.md`](40-dynamic-ui.md) | Schema-driven reactive UI, widget↔CST binding, split form/source | **R2** (derived-overlay contract), cross-platform thin-bridge |
| [`50-agentic-surface.md`](50-agentic-surface.md) | Thin MCP server, edit→validate→derive→render loop, git-native scenes | Latency dependence, agent-edit safety, branch/PR mode |
| [`60-supersession-and-migration.md`](60-supersession-and-migration.md) | Component-fate inventory, phased migration, risk register | Migration realism, coexistence, the two tar-pits |

## 1. The one-paragraph synthesis

All six facets converge on a single, coherent architecture and — importantly — agree that **this
is a fulfillment of trends already in the tree, not a rewrite.** A proto-CST already exists
(`RawTokenCapture` + `SourceSpanIndex`, built for round-trip save and then discarded); the
realize→TLAS→light-sampler→photon derivation pipeline in `RayCaster::AttachScene` is *already*
incremental and correctly staged; the descriptor-driven parser (`Describe()`) is *already* the
schema for parsing, highlighting, and suggestions; the in-place "apply layer"
(`ApplyObjectOpForward` + introspection setters) already does fine-grained scene updates; and the
GUI roadmap *already* declares "text is the source of truth" while implementing the opposite. Model
B wires these together: **CST is canonical; the scene is `derive(CST)`; edits are CST patches;
undo is CST versioning; the UI is `render(CST, descriptors)`; the agent and the GUI are two clients
of one CST-edit pathway.** The supersession is concentrated — ~40% of `src/Library/SceneEditor/`
(the edit/history/transaction/save scaffolding that exists *because* there is no canonical
document) is deleted; the engine, managers, apply layer, and realize seam are untouched.

## 2. The unified architecture

```
        ┌──────────────────────────────────────────────────────────────┐
        │                         CST  (CANONICAL)                       │
        │   immutable · persistent · NodeId identity (name-path=addr) · lossless trivia│
        │   nodes reference ChunkDescriptor (the schema, L6)             │
        └───┬───────────────┬───────────────────────────┬──────────────┘
   serialize│           edit │ (CstPatch: structured OR  │ derive (memo + dep graph)
    (verbatim│            text, ONE pathway — Facet 3)    │  Facet 2
     untouched)               │                           ▼
        ▼                     ▼                  ┌──────────────────────┐
   .RISEscene          version DAG (undo/redo,   │  Derived Scene       │
   (git-diffable)      O(1) snapshot, branches)  │  managers·geom·BVH·   │
        ▲                     ▲                   │  lights·photons      │──▶ Render
        │                     │                   └──────────────────────┘
   humans + agents       Dynamic UI (Facet 4: render(CST,descriptors))
   (text patches,        widgets ↔ CST nodes by name-path; split form/source
    Facet 5 MCP)         agent + GUI = two clients of the ONE edit pathway (L2)
```

Per-facet headline:
- **F1 (CST):** promote the existing span machinery to a retained, lossless, descriptor-bound tree;
  `FOR`→`instance_array` (homogeneous, instances *derived*) or one-shot desugar (heterogeneous);
  `DEFINE`→`let`; `$(...)`/`hal`/macros→one `expr(...)` sublanguage over the existing
  `ExpressionProgram`; v6 is migrated to v7 by a one-shot migrator, then the v6 reader is **deleted**
  (time-bounded, no runtime legacy nodes — D8).
- **F2 (Derivation):** `derive(CST, CST', cache)` is *one* function (full = incremental vs empty
  cache, killing P-WALK); memo cache + dependency graph front two existing backends —
  `Finalize→Job::Add*` (rebuild) and the apply layer (value-fast-path); existing phase-B dirty
  flags (`BumpLightTopologyGeneration`, TLAS-valid, photon-pending) set precisely (killing
  P-INVALIDATE); memo keys are the node's trivia-insensitive **derivation key** — semantic structure
  + traced-input versions (D4/D15) — not text spans and not raw resolved values (so `expr(A)` ≠ `5`).
  **Layered (r3):** `DerivedScene = f(CST, assets, t)` (config-independent: geometry + TLAS) then
  `PreparedRenderState = prepare(scene, RenderConfig)` (light samplers + photon maps) — D22; animation
  is per-frame derivation (time `t` is an input) and irradiance caching is render-local mutable — D21;
  TLAS is full-rebuild-v1 and the O(closure)/O(log N) headline is gated on persistent containers —
  D24/D23. **Stamped (r4):** a `DerivedStamp {cstVersion, assetGen, animationName, shutterInterval}`
  and a `PreparedStamp` (+ renderConfig, cameraOverride, **samplingSeed** → deterministic) key
  **artifacts** that hold the cache — off the immutable `Version {greenRoot, identityRoot, metadata}`
  (D29/D30); derive→prepare→render run **async + cancellable on the render arbiter** (D34), off the
  edit thread; motion blur is preserved as a **time-interval** immutable scene (D31, gated). Staleness
  is cstVersion **DAG ancestry, not `<`**. **Runtime-precise (r5):** status shows **requested vs
  published** stamps (`ok` ⟺ full-stamp equality, D38); derivation has a **bounded sync semantic
  phase** (validate/rename) + the async expensive phase (D39); the seed makes *prepare* deterministic
  but the *render* is **reproducible-within-tolerance, not bit-identical** (D40); assets bind by
  **content digest of the loaded buffer** (D41); the `PreparedStamp` carries a resolved
  **EffectiveRenderConfig** hash + a **view-pose** hash (D42); and **previews are latest-wins while
  explicit renders are stamp-pinned** (D43). **Coordinator-precise (r6):** the effective config is
  resolved **after `DerivedScene`** (auto-route may probe-render, D45); asset identity is the
  **transitive byte closure** digest (glTF main+buffers+textures), pinned for queued jobs (D46);
  there is **one render slot** — pinned renders survive a head change and previews **suspend** while
  one owns the slot (D47/D48); `status:ok` ⟺ stamp-equality **AND phase==complete** (D49); pinned
  renders are **`RenderJobId`-keyed** with targeted control (D50); and commit is **semantic-only**
  (broken-but-valid heads possible; opt-in awaited full validation, D51).
- **F3 (Edit/History):** an edit is `CstPatch → new immutable root`; undo/redo is a pointer move
  over a structurally-shared version DAG; a gesture coalesces patches and commits *one* version
  (dissolving composites/transactions/rollback/atomicity/identity-serial); lossless round-trip is
  automatic (untouched nodes shared ⇒ byte-identical serialize ⇒ no SaveEngine).
- **F4 (UI):** `render(UiModel::Build(CST, descriptors))` — a widget per parameter dispatched on
  each chunk's `Describe()`; adaptive for free (all ~138 chunks get UI with zero new code);
  binding is name-path→`EditIntent`→F3's pathway; split form/source via spans; the bridge
  boundary-enum becomes generated (kills `case 5:` drift).
- **F5 (Agent):** thin surface — one mutating verb (`propose_patch`, structured or text, both →
  one validated CST version) + `validate`/`render`/`read_*`; the edit→validate→derive→render loop
  maps 1:1 onto the coding-agent loop; git-native diff/PR/bisect on INV-4; headless autonomy emits
  a branch/PR, not a commit.
- **F6 (Migration):** concentrated supersession; phased P0–P8 strangler-fig with v6/v7 dual-path
  converging at `IJob::Add*`; corpus migrator (only 29/376 scenes use macros; 13 use `FOR`);
  risk register R1–R10.

## 3. Cross-facet reconciliations (decisions, with recommendations)

> **Superseded by [`01-DECISIONS.md`](01-DECISIONS.md) (review round 1).** The recommendations
> R1–R5 below were the pre-review synthesis; the external review then found 8 P1 + 2 P2
> contradictions, resolved authoritatively as **D1–D10**. Mapping: R1 (persistent CST) → **D2**
> (red-green tree) + **D1** (COW snapshot); R3 (`expr` syntax) → retained; R4 (PR mode) → retained;
> R5 (`halton`) → retained; plus new decisions D4 (traced dependencies), D5 (AssetManifest), D6
> (external-file CAS), D7 (single-file / deprecate `>load`/`>run`), D8 (time-bounded v6), D9 (dual
> NodeId + name-path identity), D10 (one phased first slice). The R1–R5 text is kept for history.

The facets were designed in parallel against the shared charter; these are the seams where their
assumptions meet. **R1 is load-bearing for the whole design; the rest are contained.**

### R1 — The CST is an *immutable, persistent* tree (THE decision) — recommend: YES
- **F3** requires it: its O(1)-snapshot, free-branch, and *automatic* byte-exact round-trip wins
  all assume mutation = "new root sharing untouched subtrees" (a red-green / persistent tree).
- **F1** delivers the compatible machinery (typed nodes, stable `NodeId`s, spans, an explicit
  **changed-NodeId-set contract** that already satisfies F2's diff need) — but §2.8.5 describes a
  structured edit as *mutating the node in place*, and punts the version model to F3 without
  committing to persistence.
- **F2** is satisfied either way (it keys on the trivia-insensitive derivation key — semantic
  structure + traced-input versions, D4/D15 — and consumes the changed-NodeId set), so
  it does **not** force the choice — but it warned that if F1 *can't* give a cheap node diff, F2
  must add its own per-node hash cache, leaking derivation state into the CST (an INV-1 violation).
  F1's explicit changed-NodeId contract removes that worry **provided** the representation makes the
  diff cheap.
- **Recommendation:** adopt a **persistent (structurally-shared, red-green-style) CST**. F1's
  span/NodeId/changed-set machinery is compatible; only §2.8.5's "mutate in place" becomes "path-
  copy to a new root." This single choice is what makes undo/round-trip/branching fall out *for
  free* instead of needing snapshot-per-commit. It is the refinement of charter **O1** (the real
  question isn't "text- vs CST-canonical" abstractly — it's "is the CST persistent"). **This is the
  #1 thing for the reviewer to rule on and the #1 thing the first slice must validate.**

### R2 — Per-node "derived summary + generation counter" contract (F2 → F4) — recommend: YES, additive
- **F4** has three widgets that need *derived* data, not just CST data: generator instance previews,
  reference-picker validity, inline diagnostics. It proposes these be **strictly additive overlays
  keyed by name-path**, never altering tree structure, to avoid Model-A coupling creeping back.
- **F2** already has the substrate: a per-node memo cache and generation counters (light-topology,
  TLAS-valid). 
- **Recommendation:** F2 exposes a small **read-only `DerivedSummary(name-path) → {value/preview,
  generation, diagnostics}`** view; F4 consumes it as overlays. Confirm F2 adds this to its public
  contract. Low risk; preserves INV-1 (the CST stays the only *mutable* truth).

### R3 — Expression surface syntax: `expr(...)` + `ExpressionProgram` unification (F1 Q6) — recommend: `expr(...)`
- Reusing `$(...)`'s exact spelling would shrink migration ("only convert FOR/DEFINE"), but `$(...)`
  has no slot for instance-vars / name-path refs and uses a separate evaluator. `expr(...)` unifies
  on the existing `ExpressionProgram` (the `expression_function2d` engine) and reads cleanly.
- **Recommendation:** `expr(...)` + unification. It's "the most defensible thing to reverse," so
  low risk to commit now and revisit if the corpus rebels.

### R4 — Headless autonomy emits a branch/PR, not a commit (F5) — recommend: YES
- Turns the MCP-auth caveat (interactive servers absent in headless/cron) into the *native* mode:
  an autonomous agent run produces a reviewable branch/PR ("PR-review a lighting change"). Aligns
  perfectly with the git-native thesis. Adopt as the headless deployment shape.

### R5 — `hal()` becomes pure `halton(dim, index)` (F1 Q2, INV-2) — recommend: YES, with render-diff
- The current process-global `MultiHalton` makes derivation order-dependent (violates INV-2). Make
  it index-explicit; verify the two scenes that use it (`painters.RISEscene`,
  `diamond_teapot_pour.RISEscene`) via render-diff during migration. Genuine but low-blast-radius
  semantic change.

**No facet conflicts with a Locked decision.** The only true *open* representation choice is R1.

## 4. Open decisions for the reviewer

1. **R1 — persistent CST?** (refines O1). The load-bearing one. Everything in F3 (and the headline
   simplification) rests on it.
2. **O2 — interactive latency bar.** Design assumes *debounced-commit* manipulation (aligned with
   the agentic edit→preview cadence), not 60Hz gizmo. F2 owns the budget; the first slice measures
   it. If 60Hz direct-manipulation is a hard product requirement, F2's granularity must go finer
   (cost flagged).
3. **The two tar-pits are the same two things the first slice proves** (see §6): lossless round-trip
   (R1/F1 §4.1) and incremental-derivation latency (O2/F2).
4. **O3 — resolved by D8 (time-bounded):** migrate the corpus to v7 with a one-shot migrator, then
   **delete the v6 path** — no permanent coexistence, no runtime legacy nodes. Reviewer to confirm the
   coexistence window is acceptable.
5. **Generator expressiveness (F1 Q3):** confirm no scene needs a *retained imperative loop*
   (data-driven counts / iteration depending on the prior iteration's output). If one does, the
   fallback is desugar-to-explicit (always possible, verbose) — but it dents the "for nerds" thesis.

## 5. What Model B deletes (roll-up)

Concentrated in `src/Library/SceneEditor/` and the parser preprocessing layer:
- **Whole edit/history/transaction subsystem:** `SceneEdit`, `EditHistory`, transactions/rollback,
  composite atomicity, the round-4 identity-serial, `DropStaleSelection_`, the
  Capture/ApplyRevert/ApplyForward *tracking* (the *apply* logic survives as F2's value-fast-path).
  → CST versioning (F3).
- **`SaveEngine` Mode-A/B byte-splice + managed-override block + `OverrideSpanIndex` + dirty
  diffing** → `SerializeCst` (F1/F3): serialize the canonical tree, verbatim for untouched nodes.
- **`DirtyTracker` / "unsaved changes"** → dissolves (head-version-id ≠ flushed-version-id).
- **Fixed accordion `Category`/`PanelMode`, the nine per-category state arrays, the 7 introspection
  classes, `ResyncObjectBoundSections_`** → one schema walk (F4).
- **`FOR`/`ENDFOR`/`DEFINE`/`UNDEF`/`@`/`%`/`$(...)`/global `MultiHalton`** → `instance_array` /
  `let` / `expr(...)` (F1); legacy spellings round-trip read-only until migrated.
- **`Job` order-dependence** (e.g. `SetPrimaryAcceleration` discarding prior objects) → declarative
  config + topo-ordered derivation (F2).

Untouched: the renderer/integrators, `GenericManager` + the managers, the apply layer, the
realize→TLAS→light-sampler→photon seam, `ChunkDescriptor`/`Describe()`.

## 6. The first slice (the falsifiable vertical)

> **Superseded by [`01-DECISIONS.md`](01-DECISIONS.md) §D18** (which amends §D10), defining the
> single canonical phased fixture: 1 `sphere_geometry` → 2 `+uniformcolor_painter` **+
> `lambertian_material{reflectance→painter}`** (the first real *reference* → ref-picker) → 3
> `+standard_object{geometry,material}` (the true geometry→material→object chain + rename) → 4
> `+expr` → 5 `+instance_array` → **6 `+image_painter`/mesh (asset-backed, so G5 is exercisable)**.
> Shared gates **G1–G5** (round-trip byte-identity; incremental-derive < 50 ms on a Sponza-class
> scene; minimal invalidation; version-DAG undo; asset/file-conflict). The earlier, divergent text
> below is retained for context.

All facets independently converged on "one simplest chunk, full vertical." Two candidates were
named — F1: `sphere_geometry` (2 params); F6: `uniformcolor_painter`. **Recommendation:** start with
**`sphere_geometry`** (no references, no color-space subtlety) and add `uniformcolor_painter` as
slice 1b (introduces a `Reference`/color value + the picker widget). The vertical:

1. `bytes → CST` (`ParseToCst`, reusing the `RawTokenCapture` lexer; comments/blanks/trivia as nodes).
2. `CST → bytes` (`SerializeCst`) — **GATE:** parse→serialize is byte-for-byte identity on a
   hand-formatted chunk (tabs, trailing comment, blank line). *This is the single most important
   correctness gate; a "looks done" implementation is usually broken here.*
3. `CST → scene` via the **unchanged** `pJob.AddSphereGeometry(...)` apply layer (proves the
   `Finalize`-relocation against untouched engine code).
4. One schema-generated widget (`radius` ↔ `geometry/s.radius` via name-path + descriptor `kind`).
5. **Persistent edit + incremental re-derive:** editing the widget or the text produces a new CST
   root (R1), the changed-NodeId set `{geometry/s.radius}` re-derives only that geometry, round-trip
   preserves the comment. **MEASURE** edit→preview latency here (O2).

This slice exercises every load-bearing claim — persistent lossless CST, span-based two-way mapping,
descriptor-driven binding, NodeId identity (name-path = addressing), apply-layer reuse, incremental re-derive — and
directly tests the **two tar-pits** (round-trip identity; derivation latency). If both hold on the
smallest chunk, the architecture is sound to scale; if either fails, we learn it cheaply.

## 7. Recommended sequence

1. Reviewer rules on **R1** (+ confirms R2–R5, O2 bar, O3 window) — gate before any code.
2. Build the **first slice** (`sphere_geometry`) to the two gates (round-trip identity; latency).
3. If green: proceed along F6's phased P0–P8 strangler-fig (add `Reference`/rename, `RepeatGroup`,
   `expr(...)`, then `instance_array` replacing `loops.RISEscene`), deleting the superseded
   subsystems phase-by-phase behind the v6/v7 dual path — never a flag-day rewrite.
