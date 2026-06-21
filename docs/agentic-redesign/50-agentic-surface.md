# Facet 5 — Agentic Surface & Product

> **Status:** design-in-progress. One of six parallel facet docs under
> [00-CHARTER.md](00-CHARTER.md). **Design only — no source, build, or scene changes.**
> **Updated per [`01-DECISIONS.md`](01-DECISIONS.md) (rounds 1–4, D1–D37):** added external-file
> conflict handling (atomic save + content-hash fingerprint, D6/D17); v7 is single-file (D7);
> the read/patch contracts expose the coherent version surface with **full
> `DerivedStamp`/`PreparedStamp`** rather than a single `derivedVersion`, and staleness is
> **cstVersion DAG ancestry, not numeric `<`** (D13/D29); name-path is addressing while the
> immutable NodeId is the stable lineage identity (D9/D14/D15); the render/derive tools take an
> explicit **`RenderConfig`** — which carries a **sampling seed / RNG-stream id** so `render`/
> `derive_preview` are **reproducible** (same `PreparedStamp` → same image, D33) — and an integrator
> override re-runs only the config-dependent `prepare` layer, not the config-independent scene
> derivation (D22); `derive → prepare → render` run **async + cancellable on the single render
> arbiter**, off the agent/edit thread (a newer patch cancels in-flight work — D34), which is exactly
> the head-vs-derived lag the agent observes; `rename` uses the **same resolver as derivation**
> (synchronously derives head; refuses if head can't be derived — D25/D35); a `render` of an
> animation frame derives a **time-INTERVAL** scene (motion blur gated v1-off) and the active
> animation name is part of the stamp (D31); and branch/PR history preserves the **CST only** — a
> re-derived old version uses *current* asset bytes (D28).
> This facet owns the **RISE MCP server**, the **edit→validate→derive→render→observe loop**,
> the **GUI-as-just-another-agent** unification (charter **L2**), **diff-able / git-native /
> reviewable scenes**, **agent-edit safety & validation**, **product framing & differentiation**,
> and **deployment modes**.
>
> **The pivot this doc lives or dies by:** the canonical object is now a **retained, lossless
> CST** (charter §3), edited through **one pathway** by two clients (GUI + agent, **L2**). That
> dissolves the single biggest assumption baked into the existing AI specs in `docs/gui/`
> ([MCP_TOOL_SURFACE.md](../gui/MCP_TOOL_SURFACE.md), [LLM_AGENT_RUNTIME.md](../gui/LLM_AGENT_RUNTIME.md),
> [AI_SECURITY_MODEL.md](../gui/AI_SECURITY_MODEL.md), [VALIDATION_ARCHITECTURE.md](../gui/VALIDATION_ARCHITECTURE.md),
> [TRANSACTION_MODEL.md](../gui/TRANSACTION_MODEL.md), [RENDER_COORDINATOR.md](../gui/RENDER_COORDINATOR.md)):
> namely the **"structured-save vs wholesale-text-rewrite" split**, which is a Model-A artifact
> and is **deleted** under Model B. §3 states precisely what survives, what is reframed, and what dies.

---

## 1. Current-state grounding — what exists today for this facet

The agentic surface is **greenfield in code** but **richly designed on paper**, and that paper
is Model-A-shaped. Grounding both, with real file citations:

### 1.1 In code (verified)

- **No `src/Library/Agent/` directory exists.** (`ls src/Library/Agent` → *No such file or directory*.)
  Every type the `docs/gui/` AI specs name (`AgentServer`, `SceneValidator`, `SchemaGen`,
  `ProjectRootJail`, `ICredentialStore`) is design-only. The agentic surface is a clean build, not
  a retrofit — which is fortunate, because Model B changes its foundation.
- **The descriptor-driven parser is the schema source (the load-bearing asset).** Every chunk
  parser overrides `Describe()` (returns a `ChunkDescriptor` of `ParameterDescriptor`s — `name`,
  `kind`/`ValueKind`, `required`, `repeatable`, `enumValues`, `referenceCategories`, `tupleKinds`,
  `presets`, `description`, `defaultValueHint`, `unitLabel`) and `Finalize()`
  ([src/Library/Parsers/README.md](../../src/Library/Parsers/README.md);
  `ChunkDescriptor.h:110` `enum class ValueKind`). `SceneGrammar::Instance()` aggregates all
  **138 chunk keywords**. **This is charter L6** — descriptors *are* the schema; the agentic
  surface consumes it, never re-declares it. Drift between "what the parser accepts" and "what the
  descriptor advertises" is structurally impossible *within the parser*; whether a *generated JSON
  Schema* matches the parser is a separate, weaker claim (see §5 / VALIDATION_ARCHITECTURE §5).
- **The headless CLI loop already exists** in `src/RISE/commandconsole.cpp`: the
  `printf "render\nquit\n" | ./bin/rise scene.RISEscene` harness in the charter. It drives the
  `AsciiCommandParser`, whose dispatch table includes `render`/`load`/`quit`
  (`AsciiCommandParser.cpp:43-48`) and which calls `exit(1)` on a malformed command
  (`:182`) — the barrier a `validate` dry-run must structurally avoid.
- **There is no side-effect-free validation pass.** `ISceneParser::ParseAndLoadScene(IJob&)`
  parses *and mutates the Job*, returning `bool`, logging errors to `GlobalLog()` — not a struct
  with locations. There is no string-input entry point. So actionable, localized validation is
  net-new code (small; §4.3).
- **The render path is `RayCaster::AttachScene` → realize-from-roots → integrators** (deferred
  realization is in tree, memory `project_deferred_realization`). The agentic surface submits
  render *requests*; it owns none of the render machinery.

### 1.2 On paper (Model-A-era; this facet supersedes the shape, reuses the threat model)

The five `docs/gui/` AI specs are a thorough first pass written **before** the Model A→B decision.
They assume the round-4-era world: a live mutable `Scene`, a `SceneEditController` "one mutation
path," `SceneEdit`/`EditHistory`/transactions/epochs, and a `SaveEngine` that persists *some*
edits structurally (transform/property/created-camera) and falls back to **wholesale text
rewrite + reload** for the rest (e.g. non-camera creation). The headline artifacts:

- [MCP_TOOL_SURFACE.md](../gui/MCP_TOOL_SURFACE.md): a ~25-tool catalog with a `committed`-vs-`proposed`
  contract, a `baseEpoch` `(uuidHi, uuidLo, revision)` precondition on every mutating tool,
  `apply_scene_text` as the *wholesale rewrite* fallback, `validate` as the keystone, the
  `rise://*` resource catalog, and three transports (in-process / loopback-HTTP / stdio).
- [LLM_AGENT_RUNTIME.md](../gui/LLM_AGENT_RUNTIME.md): `ILLMProvider` (Claude/Gemini/local adapters),
  the agent loop, the vision feedback loop, autonomy L0–L3, `ICredentialStore`.
- [AI_SECURITY_MODEL.md](../gui/AI_SECURITY_MODEL.md): the threat model — prompt-injection quarantine,
  `ProjectRootJail`, `IOutboundUrlPolicy` (SSRF), `SecretRedactor`, the always-confirm destructive
  set, loopback-only transport, Android adb-loopback reality. **This doc adopts the threat model
  almost verbatim** — it is orthogonal to the canonical-form decision and remains correct.

**What changes under Model B, in one sentence:** when the document *is* the canonical CST and a
structured edit and a text edit are **the same operation on that CST** (charter L2), the entire
"which edits persist structurally vs which need a wholesale rewrite" axis — and the `apply_scene_text`
tool that embodied it — **collapses**. There is exactly one apply: a CST patch. §2 builds the surface
on that fact; §3 inventories the supersession.

---

## 2. The Model-B agentic surface

### 2.1 Thesis: the agent's medium is the document, so the tool surface is thin

RISE is unusually suited to be an MCP server, and Model B sharpens *why*: the scene is a
**canonical, lossless, diff-able document**, the grammar is **machine-introspectable** (L6
descriptors), and editing is **one pathway** (L2). A coding agent already knows how to operate
exactly this shape of system — **read a file, propose a patch, run the checker, observe the
result, iterate.** RISE's job is to expose that loop honestly, not to invent a parallel
"3D-API-over-JSON-RPC."

This is the central design stance, and it is a deliberate rejection of the obvious alternative:

> **Thin, not fat. We do NOT ship 200 mutation RPCs** (`setLightIntensity`, `rotateObject`,
> `addSphere`, `setMaterialRoughness`, …). A fat per-operation API would (a) re-encode the grammar
> a second time — a guaranteed-to-drift parallel schema, violating L6; (b) recreate the Model-A
> "every mutator is a fresh place to forget a step" disease the editor-state post-mortem
> ([EDITOR_STATE_AND_TRANSACTION_HARDENING.md](../gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md))
> catalogued (P-STATE / P-WALK / P-INVALIDATE — ~17 defects, 4 root patterns, all "replicated
> instead of owned by one chokepoint"); and (c) be *worse for the agent*, which reasons fluently
> over documents and diffs, not over a sprawl of imperative verbs it must discover and sequence.

The whole surface is therefore organized around **the document and its derivations**:

```
  READ            PATCH                 VALIDATE          DERIVE+RENDER        OBSERVE
  ────            ─────                 ────────          ─────────────        ───────
  read CST text   propose a CST patch   re-parse the      derive scene +       read back the
  (whole / by     (structured node-op   patched CST →     render a preview     image, the
   name-path)      OR a text patch)      structured        (incremental         numeric/structural
  read the                               errors localized  re-derive)           diffs, the
  derived graph                          to CST nodes                           diagnostics
  read the
  descriptor
  schema
```

Everything below is a small number of tools serving those five verbs. **The agent's leverage
comes from the document being good, not from the API being large.**

### 2.2 The tool set (lean by construction)

The server lives in shared C++ at **`src/Library/Agent/`** (charter: maximize shared C++; the
GUI roadmap §16 naming rule — avoid the bare `MCP` token since `src/DRISE/MCPClientConnection`
is unrelated Master-Control-Program plumbing — still holds; the C++ types are `Agent*`/`Scene*`/
`Cst*`-prefixed). It is a JSON-RPC adapter over the CST store (Facet 1), the derivation engine
(Facet 2), the edit/version layer (Facet 3), and the descriptor schema (L6). The MCP primitive
mapping is unchanged from the prior design (Resources = read-only context; Tools = model-invoked
actions; Prompts = curated workflows), but the *tools* are re-cast onto the CST.

#### 2.2.1 Read tools / resources (pull, never push)

| Tool / resource | Contract | Backed by |
|---|---|---|
| `read_document` → `rise://cst/text` | Returns `{ text, version: {headVersion}, status, diagnostics, redactions: [{offset,length}] }` — the **whole canonical CST serialized to `.RISEscene` text** (comments + formatting preserved; this is what makes diffs reviewable, §5). Stamped with **`headVersion`** (the CST truth, §2.2.1.1): the text *is* the head, so it never lags. `headVersion` is the version a `propose_patch` precondition is checked against (optimistic concurrency, §2.4). Secrets length-preservingly masked with a **single-byte** ASCII mask (`*`) so every byte offset equals the canonical document's (the AI_SECURITY_MODEL §9 redaction contract — carried over verbatim because it is offset-faithfulness, not a Model-A artifact). | Facet 1 CST → serializer |
| `read_node` → `rise://cst/node/{name-path}` | Returns one node's source span **addressed by name-path** (charter **L5**: `objects/sphere`, `objects/sphere.material`, `materials/gold.roughness`) as `{ text, nodeId, version: {headVersion}, status, spanOffset, redactions }` — stamped with **`headVersion`** like `read_document` (it reads the CST). Name-path is *addressing*, version-resolved against the head; it is **not** the durable identity (D9/D15): it changes on rename. An agent that must hold a reference to a node **across edits** keeps the returned immutable **`nodeId`** (the lineage identity) and re-addresses by name-path within a version. | Facet 1 name-path index → span |
| `read_graph` → `rise://scene/graph` | Structured introspection of the **derived** scene: objects / cameras / lights / materials / media / film, each with type, resolved property rows, bound-material links, world-space transform + bbox. Stamped with the **`DerivedStamp = {cstVersion, assetManifestGen, animationName, shutterInterval}`** + `status` + `diagnostics` (§2.2.1.1, D29), **not** `headVersion` — when derivation lags or is broken this is what the scene actually reflects (its `cstVersion` may be a proper ancestor of head, or last-good on `status:error`). This is the **config-INDEPENDENT** `DerivedScene` (D22: `f(CST, AssetManifest, t)` — geometry, materials, lights-as-emitters, TLAS); it carries the full `DerivedStamp` but **no `RenderConfig`/`PreparedStamp`** because the graph does not depend on integrator/rasterizer selection (only the *rendered image* does, via the `PreparedRenderState` layer, §2.2.3). This is the *evaluated* view (what the CST *means*), complementary to the CST text (what the CST *says*); the head `cstVersion` and the stamp's `cstVersion` are surfaced so a client is **never** told they are equal when they are not (staleness is the **DAG-ancestry** check on `cstVersion`, D29, never numeric `<`). | Facet 2 derived scene + introspection |
| `read_schema` → `rise://grammar/schema` and `rise://grammar/node/{keyword}` | The whole grammar (or one chunk) as JSON Schema generated from `SceneGrammar::Instance()` (L6). **The agent's "what is the grammar" reference** — every keyword, its category, params with kind/enum/refs/defaults/units/descriptions. A *first-pass filter, not the parser* (§5). | `SchemaGen` over descriptors |
| `read_image` → `rise://framebuffer` (+ `…/aov/{aov}`) | The latest **completed** preview as tone-mapped sRGB PNG (so a vision model sees what a human sees), with `?exposure`/`?max_dim`/`?region` query params, a `generation` counter so the agent can tell "is this the frame I asked for?", and the full **`PreparedStamp = DerivedStamp + {renderConfig, cameraOverride, samplingSeed}`** the frame was rendered from (D29) — the `DerivedStamp` half is the derived snapshot (whose `cstVersion` may be a proper ancestor of head), and the `{renderConfig, cameraOverride, samplingSeed}` half fixes the pixels: two images sharing a `DerivedStamp` differ when their `renderConfig`/`cameraOverride`/`samplingSeed` differ, and **two images with the same `PreparedStamp` are bit-identical** (D33 — the seed makes the render reproducible). AOVs (albedo/normal/depth/objectid/alpha) visualized; raw float via `?format=exr`. Never *triggers* a render. | Facet 2 render output / FrameStore |
| `read_diagnostics` → `rise://render/stats`, `…/autoroute`, `rise://log` | Render telemetry (resolution, samples, ETA, progress), the auto-router's resolved integrator + one-line reason ("Auto → BDPT: glossy/indirect variance high"), and the redacted log tail. Stamped with the render's **`PreparedStamp`** + `status` (§2.2.1.1, D29) — it describes the render of a `PreparedRenderState`, whose `DerivedStamp.cstVersion` may be a proper ancestor of head. The numeric/structural channel the agent observes alongside the image. | counters / `AutoRasterizer` / log sink |

`read_*` tools carry **no precondition** and read a **published immutable snapshot** of the CST +
its derivation (never the live tree), so they answer concurrently with edits without tearing
(Facet 3 owns the snapshot contract).

##### 2.2.1.1 The coherent version surface — head vs derived via full stamps, compared by ancestry not `<` (per [`01-DECISIONS.md`](01-DECISIONS.md) §D13, **§D29**)

A single stamped `documentId` shared by the CST read and the graph/render reads **tears**: derivation
is async (and may serve a *last-good* snapshot on error), so when head N is broken or still deriving
while the graph/render reflects an ancestor, one id is a lie. **D29 sharpens D13's single
`derivedVersion`:** a `DerivedScene` depends not only on the CST version but on the asset-manifest
generation, the active animation, and the shutter interval — so two scenes at one `cstVersion` (t=0
vs t=1, or pre/post an asset change) must be distinguishable. The derived/rendered reads therefore
carry **full stamps**, not a lone `derivedVersion`, and staleness is a **DAG-ancestry** test on the
`cstVersion` axis (never numeric `<` — the version DAG branches; the other axes are equality-matched).
The session publishes **one coherent status value**, and the read/return contracts stamp the right
half of it:

```jsonc
{ "headVersion": { "uuidHi", "uuidLo", "revision" },   // the CST truth (what read_document returns)
  // DerivedStamp identifies a DerivedScene (config-INDEPENDENT view, e.g. read_graph):
  "derivedStamp": { "cstVersion":      { "uuidHi", "uuidLo", "revision" }, // may be a DAG-ancestor of head, or last-good
                    "assetManifestGen": 0,            // bumps when a traced asset's content hash changes (D5/D17)
                    "animationName":    "main",       // the active animation path (D31)
                    "shutterInterval":  [ 0.0, 0.0 ] }, // single-time in v1; an interval under motion blur (D31)
  // PreparedStamp = DerivedStamp + render-config axes; identifies a PreparedRenderState (the rendered image):
  "preparedStamp": { "derivedStamp":   { /* … as above … */ },
                     "renderConfig":    { /* integrator/quality/resolution/denoise … (§2.2.3) */ },
                     "cameraOverride":  null,
                     "samplingSeed":    "<rng-stream id>" }, // D33 — makes the render reproducible
  "snapshot":    "<opaque derived-snapshot handle>",
  "status":      "deriving | ok | error",
  "diagnostics": [ /* ValidationReport entries, §2.5 — explain a lag or failure */ ] }
```

- **CST reads** (`read_document`, `read_node`) are stamped with **`headVersion`** — the text *is* the
  head, so it cannot lag.
- **Derived reads** carry the right stamp: the **config-independent** view (`read_graph`, D22) carries
  the **`DerivedStamp`** alone; the **rendered** reads (`read_image`, `read_diagnostics`, and
  `render`'s/`derive_preview`'s result) carry the full **`PreparedStamp`** — because an image is a
  `PreparedRenderState = prepare(DerivedScene, RenderConfig)`, so the same `DerivedStamp` under two
  integrators (or two seeds) yields two distinguishable images, while two reads with the **same
  `PreparedStamp` are bit-identical** (D33). A stamp's `cstVersion` **may be a proper ancestor of
  head** (derivation in flight) or a **last-good** snapshot (`status:error`); `status` + `diagnostics`
  explain *why* it lags or failed.
- **Staleness = DAG ancestry, not `<` (D29).** "Is this derived/rendered view stale vs head?" is
  decided on the **`cstVersion` axis only**, by checking whether the stamp's `cstVersion` is an
  **ancestor-or-equal** of head's in the version DAG — **not** a numeric `revision` comparison (the
  DAG has branches, so `<` is meaningless across them). The other stamp axes
  (`assetManifestGen`/`animationName`/`shutterInterval`, and the render-config axes) are matched by
  **equality**, not order.
- **Clients are never told the two are equal when they are not.** A vision agent that reads an image
  whose `preparedStamp.derivedStamp.cstVersion` is a strict ancestor of head knows it is looking at a
  stale frame and can wait for `status:ok` at head's `cstVersion` (or re-`render`).
- A **`propose_patch` precondition** (optimistic concurrency, §2.4) is checked against
  **`headVersion`** — the CST is what a patch rebases on, not the (possibly-lagging) derived scene.

Every tool result shape below reflects whichever stamp it is stamped with; nothing emits a bare
`documentId` that straddles both. (Facet 3 owns publishing this status alongside each snapshot;
Facet 5 only consumes and re-exposes it.)

#### 2.2.2 The patch tool — one apply, two patch encodings

This is the heart of the redesign. There is **one mutating verb** with **two equivalent encodings**,
because the CST is the single canonical object and both encodings produce the same thing: a versioned
CST patch.

| Tool | Contract |
|---|---|
| `propose_patch` | `{ baseHeadVersion: {uuidHi,uuidLo,revision}, patch: <CstPatch \| TextPatch>, intent?: string }`. The precondition is **checked against `headVersion`** (the CST truth, §2.2.1.1), not the derived scene — a patch rebases on the head. **Validated (parse-the-result + derive-dry-run) before it is allowed to commit (§2.4).** On success the commit is cheap and returns immediately with the bumped `headVersion`; **`derive → prepare → render` then run async + cancellable on the arbiter, off this thread** (D34), so the coherent status fills in the `DerivedStamp`/`PreparedStamp` + `status`/`diagnostics` as derivation proceeds (§2.5/§2.6) and a newer `propose_patch` cancels the in-flight work. A lost-race precondition returns a `CONFLICT` carrying the live `headVersion`. |

The two `patch` encodings:

1. **Structured patch (`CstPatch`)** — a list of node operations addressed by **name-path** (L5):
   ```jsonc
   { "kind": "cst", "ops": [
     { "op": "set",    "path": "lights/key.power",    "value": "1500" },
     { "op": "set",    "path": "lights/key.color",    "value": "1.0 0.85 0.7" },
     { "op": "add",    "path": "objects/",            "chunk_text": "standard_object\n{\n  …\n}\n" },
     { "op": "remove", "path": "objects/old_crate" },
     { "op": "rename", "path": "materials/gold",      "to": "brass" }
   ] }
   ```
   `set` edits one parameter on an existing node; `add` inserts a new chunk (full chunk text,
   schema-shaped); `remove` deletes a node; `rename` rewrites a node's name **in place** — it is a
   **NodeId-preserving** op (D9), so it changes the node's name-path but not its lineage identity, and
   it fixes up referrers from the traced `ReferenceUse` records (D14), flagging any it cannot resolve
   rather than silently leaving a dangling reference. **The `ReferenceUse` trace `rename` rewrites
   from must be stamped for the exact head it renames against (D25), and it comes from the *one*
   resolution path — derivation's own resolver (D35):** because derivation can lag head (the
   `DerivedStamp.cstVersion` may be a proper DAG-ancestor of head, §2.2.1.1), a referrer added in
   head-but-not-yet-derived would otherwise be missed, silently leaving a dangling old name. So
   `rename` obtains head's reference set with **the exact same evaluator/resolver as derivation —
   there is no separate "reference-tracing pass" reimplementation that could drift** (D35 corrects
   D25's standalone tracing pass; D4 demoted static schema walks *precisely because* dynamic refs like
   `timeline.element` need real derivation). Concretely, if the derived view is not already at head the
   rename **synchronously derives head** (sharing derivation's resolution step) and reads the resulting
   traced `ReferenceUse` — rename is a deliberate, infrequent op, so a synchronous derive-to-head is
   acceptable. A `rename` **never** runs against a stale trace; **if head cannot be derived (a semantic
   error), the rename is refused** (not best-effort, not silently partial). This is the encoding the
   **GUI emits** for a
   slider drag or a panel edit (L2: the GUI is an agent that speaks `CstPatch`). It is the
   *preferred* agent encoding too, because it is minimal, name-path-anchored, and trivially
   diff-reviewable.

2. **Text patch (`TextPatch`)** — a unified-diff-style or span-replace edit against the CST's
   serialized text:
   ```jsonc
   { "kind": "text", "edits": [ { "offset": 1842, "length": 6, "replacement": "1500" } ] }
   // or { "kind": "text", "unified_diff": "--- a/scene\n+++ b/scene\n@@ … @@\n-  power 1000\n+  power 1500\n" }
   ```
   This is the encoding a **coding-style agent reaches for instinctively** (it just edited a file).
   The server applies it to the canonical text, **re-parses into a CST** (Facet 1's parser is the
   one that produces the canonical tree), and proceeds identically to the structured path.

**Both encodings converge to one internal operation:** *produce a candidate CST → validate it →
commit it as a new version*. There is no "structured-save can represent this but a text edit needs
a wholesale reload" fork (the Model-A `apply_scene_text` distinction) — under Model B **every** edit
is a CST patch, and a "wholesale" replacement is just a `TextPatch` whose span is the whole document.
A whole-document text patch is **not** a special undoable-history-destroying "document swap"; it is one
more versioned CST patch (Facet 3 owns whether a large text patch is diffed structurally or stored
as a coarse version — but it is *always* a version, never a re-root that loses history). The hazardous
"feeding back redacted text overwrites real secrets" problem is handled the same way as before
(§2.4 #4 / AI_SECURITY_MODEL §3.1 #4): a whole-document `TextPatch` derived from a read that carried
`redactions[]` is refused; the agent is steered to scoped `CstPatch` ops that never touch a masked span.

> **Why two encodings and not one?** The agent population is heterogeneous. A Claude-Code-style agent
> *thinks in file edits*; forcing it through a structured AST API fights its grain. A GUI *thinks in
> node operations*; forcing it to render-then-text-diff a slider drag is absurd. Offering both — over
> **one canonical CST and one commit path** — serves both clients without a second source of truth.
> The text encoding is the agent's escape hatch for edits awkward to express structurally (reordering
> blocks, bulk comment edits, large refactors); the structured encoding is the precise, reviewable
> default. Facet 1 guarantees they round-trip to the same canonical form (INV-4).

#### 2.2.3 Render & derivation tools (no scene mutation)

These tools take a **`RenderConfig`** and surface the **two-layer derivation** (D22): the
config-**independent** `DerivedScene = f(CST, AssetManifest, t)` (geometry, materials,
lights-as-emitters, TLAS — what `read_graph` reflects) is derived once and cached; the
config-**dependent** `PreparedRenderState = prepare(DerivedScene, RenderConfig)` (light samplers,
photon maps, integrator-specific structures) is what the *image* reflects. The agent reads a
`DerivedScene`; it renders a `PreparedRenderState`. An integrator/rasterizer override therefore
re-runs **only `prepare`**, never the scene derivation — so swapping integrators never invalidates
the graph the agent just read.

**Two Round-4 properties govern these tools (D33, D34):**

- **Deterministic & reproducible (D33).** `RenderConfig` carries a **sampling seed / RNG-stream
  identity**; all stochastic preparation (photon tracing) and sampling use it instead of `rand()`.
  The seed is part of the `PreparedStamp` (D29), so a render is a **pure function of its
  `PreparedStamp`** → cacheable **and** reproducible: **the same `PreparedStamp` always yields the
  same image.** This is a direct win for the git-native/agentic thesis (§2.7) — renders are diffable
  and re-runnable, a regression bisect re-renders deterministically, and "approve this exact frame"
  is meaningful because it can be reproduced bit-for-bit.
- **Async & cancellable on the render arbiter, off the edit thread (D34).** The edit/agent thread only
  **commits a CST `Version`** (cheap). The **single render arbiter** asynchronously runs
  `derive → seal → prepare → seal → render` as **cancellable phases of its render job**; when a newer
  head (a newer `propose_patch`) arrives, the in-flight phases **cancel and restart at the new stamp**.
  Nothing expensive (derive, photon-map build, prepare, render) ever runs on the agent/edit thread, so
  a `propose_patch` returns immediately while the heavy work proceeds asynchronously. **This async,
  cancellable pipeline is exactly the source of the head-vs-derived lag the agent observes** (§2.2.1.1):
  when a stamp's `cstVersion` is a proper ancestor of head, the arbiter is mid-`derive`/`prepare`/
  `render` on an older stamp (or was cancelled by a newer patch). The arbiter is also where the
  single-render-slot scheduling lives — this facet does **not** re-invent it.

| Tool | Contract |
|---|---|
| `render` | Takes a **`RenderConfig`** (D22, D33): `{ integrator?: enum[auto,pt,bdpt,vcm,…], quality?: enum[draft,preview,final] \| {samples}, resolution?, region?, denoise?: bool, seed?: <rng-stream id>, frame?: {animation?: name, time?} \| {animation?: name, shutter?: [t0,t1]}, return_image?: bool=true }` — the rasterizer/integrator selection (plus the render-time integrator override) **and the sampling seed / RNG-stream id (D33)** that, together with the `DerivedScene`, fix the rendered pixels and make the render **reproducible** (same `PreparedStamp` → same image). Submits a render of the **current derived scene** to the single render arbiter; the arbiter runs `derive → prepare → render` **asynchronously and cancellably** off the edit thread (D34 — a newer `propose_patch` cancels in-flight work) and owns the single render slot (RENDER_COORDINATOR semantics survive intact — preempt/queue/reject; this facet does **not** re-invent scheduling). **An integrator override re-runs only `prepare(DerivedScene, RenderConfig)` → `PreparedRenderState`** (the config-DEPENDENT layer — light samplers, photon maps), **not** the config-INDEPENDENT scene derivation (D22): the `DerivedScene` the agent reads via `read_graph` is unchanged by an integrator swap; only what the image reflects changes. **An animation frame is rendered against a time-INTERVAL `DerivedScene`** (D31): the `frame` selects the active **animation by name** (part of the `DerivedStamp`, D29) and a shutter interval `[t0,t1]`; animated quantities are baked as immutable read-only functions over the shutter so per-sample motion blur needs no scene mutation. **Motion blur is gated v1-off:** v1 renders **single-time** (`shutter` collapses to one instant, `[t,t]`); the interval scene + motion-BVH path is the named follow-on (D31), and motion blur is **not retired**. The result is stamped with the full **`PreparedStamp`** it ran against (§2.2.1.1, D29 — `DerivedStamp` + `{renderConfig, cameraOverride, samplingSeed}`). `integrator:"auto"` routes through the auto-rasterizer and the result echoes `{ resolved, reason }`. A `preview` runs against a private film so it never tears a live framebuffer. **OIDN on for finals** (memory: denoise always on); `denoise:false` only for diagnostic A/B. |
| `stop_render` / `pause_render` / `resume_render` | `{}` — cooperative cancel via the arbiter. **There is no process-exit tool** (AI_SECURITY_MODEL §5). |
| `derive_preview` (optional) | `{ baseHeadVersion, patch, render_config?: RenderConfig }` — derive a candidate patch's **config-INDEPENDENT** `DerivedScene` **without committing it** (D22), returning the graph diff and (optionally, when `render_config` is supplied) a preview render. The graph diff is **stamped with the candidate's `DerivedStamp` + `status`** (§2.2.1.1, D29: a preview is a derived view); an attached preview image is additionally stamped with the full **`PreparedStamp`** used — and because `render_config` carries the **sampling seed** (D33), the attached preview is **reproducible** (same `PreparedStamp` → same image; an integrator choice runs only `prepare` on the candidate `DerivedScene`, D22). The candidate derive/prepare/render run on the **arbiter, async + cancellable** (D34) like any other — a newer `propose_patch` cancels an in-flight `derive_preview`. The "what would this change look like" dry-run, decoupled from commit. Implemented atop Facet 2's incremental derivation against a throwaway derivation context. Lets an L1 agent show the user a *rendered* preview of a proposal before it lands. |

Render tools carry **no document precondition** (they don't mutate the CST); a render's result is
stamped with the full **`PreparedStamp`** it ran against (§2.2.1.1, D29 — so two renders sharing a
`DerivedStamp` are distinguishable when their `renderConfig`/`samplingSeed`/`cameraOverride` differ,
while two renders with the same `PreparedStamp` are bit-identical, D33). A render whose
`DerivedStamp.cstVersion` a newer head has since superseded (decided by **DAG ancestry**, not numeric
`<`, D29) is reconciled as *stale* — the stamp makes the staleness visible — not rejected; and because
the arbiter runs derive/prepare/render **async + cancellable** (D34), a newer `propose_patch` typically
**cancels** the in-flight render and restarts at the new stamp rather than leaving the agent to poll a
stale frame.

#### 2.2.4 The keystone — `validate`

| Tool | Contract |
|---|---|
| `validate` | `{ text: string }` (whole scene) **or** `{ node_text: string }` (one chunk) **or** `{ baseHeadVersion, patch }` (validate a patch applied to the current CST head). Returns a structured `ValidationReport` (§2.5). **No side effects.** |

`validate` is what makes the agent **self-correcting** — it is the `tsc`/`cargo check`/`pytest` of
RISE. The agent emits a patch, validates, reads precise descriptor diagnostics localized to CST
nodes, fixes its output, and only *then* commits. This is the same loop a careful human hand-editor
runs, and it is the reason text-as-truth is the differentiator. Net-new but small (§4.3).

#### 2.2.5 Prompts (curated workflows, no inherent authority)

A short set surfaced as slash commands (in-app and external): `relight{mood}`, `diagnose_dark_render`
(walks the [effective-rise-scene-authoring](../skills/effective-rise-scene-authoring.md) checklist —
directional-light sign, `power` semantics, colour space), `lighting_variants{n}`, `explain_autoroute`.
Prompts expand to messages the model executes via the tools above; all guardrails (§2.4) still apply.

**That is the entire mutating surface: `propose_patch` + `validate` + `render`/`stop`/`derive_preview`.**
Five-ish verbs, one canonical object. Compare the Model-A `docs/gui/` catalog's `set_property`,
`set_transform`, `apply_material`, `load_hdri`, `add_entity`, `remove_entity`, `clone_entity`,
`set_active_camera`, `frame_object`, `set_lens`, `create_camera`, `apply_scene_text`, … — every one
of those is now **a `CstPatch` op the agent expresses against the document**, not a bespoke RPC with
its own schema and its own persistence story. (Convenience *prompts* can still name common operations
in natural language; they desugar to patches.)

### 2.3 The edit→validate→derive→render→observe loop, mapped onto the coding-agent loop

The loop is **literally the coding-agent loop**, which is the product thesis. Side by side:

| Coding agent | RISE agent | RISE mechanism |
|---|---|---|
| read source file | `read_document` / `read_node` / `read_graph` | CST text + derived graph from a snapshot |
| know the language | `read_schema` | descriptor-generated JSON Schema (L6) |
| write an edit (diff) | build a `CstPatch` or `TextPatch` | one patch tool, two encodings (§2.2.2) |
| **`tsc` / `cargo check`** | **`validate`** | re-parse → structured errors at CST nodes (§2.5) |
| build / run | `propose_patch` (commit) → `render` | commit the CST patch → derive (Facet 2) → render |
| **read test output / logs** | **`read_image` + `read_diagnostics`** | framebuffer (vision) + stats/variance/log |
| iterate | loop | feed errors/diffs/image back; repeat |

```
            ┌──────────────────────────────────────────────────────────────┐
            │                       AGENT (or GUI)                           │
            └──────────────────────────────────────────────────────────────┘
   read_document/node/graph/schema │                          ▲ read_image + read_diagnostics
                                   ▼                          │ (image, graph-diff, stats, log)
            ┌───────────────┐  propose_patch   ┌──────────────────────────┐
            │  candidate    │ ───────────────► │  validate (re-parse +     │
            │  CstPatch /   │                  │  derive dry-run)          │
            │  TextPatch    │ ◄─────────────── │  → ValidationReport       │
            └───────────────┘   errors @nodes  └──────────────────────────┘
                                   │ ok
                                   ▼
                       ┌────────────────────────┐  commit   ┌───────────────────────┐
                       │  CST patch applied →    │ ────────► │  derive scene (Facet 2 │
                       │  NEW VERSION (Facet 3)  │           │  incremental)          │
                       └────────────────────────┘           └───────────┬───────────┘
                                   │ headVersion++                       │
                                   ▼                                     ▼
                       ┌────────────────────────┐           ┌───────────────────────┐
                       │  notifications/         │           │  render (arbiter) →    │
                       │  resources/updated      │ ◄──────── │  framebuffer + stats   │
                       └────────────────────────┘           └───────────────────────┘
```

**How errors come back actionable.** The MCP spec distinguishes *protocol errors* (malformed
request the model can't fix — kept rare by precise schemas) from *tool-execution errors*
(`{ content, isError:true }` — actionable). A `propose_patch` whose result fails to parse/derive
returns a `ValidationReport` (§2.5) in `structuredContent`: severity, stable `code`, the **CST
node-path** and line/column of the offending span, a human message, and a `suggestion`/`candidates`
fix from the existing `SuggestionEngine` ranking (nearest known parameter; nearest existing
reference of the right category). The agent doesn't get "parse failed" — it gets *"parameter
`roughnes` is not declared on `materials/gold` (line 42); did you mean `roughness`?"* and fixes it.
A stale-precondition commit returns a `CONFLICT` carrying the live `headVersion` ("re-read and
rebase") — never a silent clobber.

**How the agent "sees" the result.** Three observation channels, in increasing richness:

1. **Structural diff** — `read_graph` before/after (or the commit result's `changed[]`) tells the
   agent *what nodes changed* in the derived scene. Cheap, exact, vision-free.
2. **Numeric diagnostics** — `read_diagnostics` gives samples/ETA, the resolved integrator + reason,
   variance/RMSE (when a reference or adaptive sampling exists — never variance alone, per the σ²·T
   "rewards dark images" caveat), and the log tail. This is the **only** channel for a text-only
   local model (it is told it is "flying blind" and leans on numeric convergence + user confirmation).
3. **The image** — `read_image` returns the tone-mapped beauty pass to a vision model. This is the
   differentiator the prior specs identified: a **spectral** renderer converging on "believable
   sunset through the window" by *looking at its own output* — tonal/chromatic correctness that is
   impossible to fake in RGB, so the model can only critique what it can see correctly. The vision
   loop is cost-capped (each iteration is a render): prefer preview-quality + region renders, cache
   the last frame so "look again" without an edit is free.

**Reference-image compare (a Model-B affordance).** Because a scene is a versioned document, the
loop can be closed against a *target*: the agent renders version N, the user (or a `prompts/`
workflow) supplies a reference image, and `read_image?format=exr` + a future per-region variance/RMSE
read-back (the net-new accessor the prior specs flag, SPECTRAL_DIFFERENTIATORS D5) lets the agent
*measure* convergence-to-reference, not just eyeball it. This makes "match this look" a closed
numeric loop, not a vibe.

### 2.4 GUI-as-just-another-agent unification (charter L2) — one mechanism, two clients

This is the architectural payoff and the through-line of the whole redesign. **Both the human GUI
and the agent edit through the same CST-edit pathway (Facet 3).** Concretely:

```
   Human GUI client                     Agent client (in-app or external)
   ───────────────                      ─────────────────────────────────
   slider drag / panel edit             LLM emits a patch
        │                                    │
        ▼ emits CstPatch                     ▼ emits CstPatch or TextPatch
        └───────────────┬────────────────────┘
                        ▼
            ┌──────────────────────────┐
            │  ONE edit pathway (F3):   │   ← the single chokepoint
            │  validate → apply to CST  │
            │  → new version → derive   │
            └──────────────────────────┘
                        │
                        ▼  notifications/resources/updated
            ┌──────────────────────────┐
            │  BOTH clients re-read the  │   the GUI rebinds its widgets;
            │  CST + derived graph       │   the agent re-reads rise://cst/*
            └──────────────────────────┘
```

There is **no parallel write path**. A gizmo drag and an agent edit are the same kind of object — a
`CstPatch` committed as a CST version. This is not a slogan; it is the *fix* for the bug class the
editor-state post-mortem documented. That post-mortem found ~17 defects across 8 review rounds, all
four root patterns (P-STATE: "hand-assembled state perpetually incomplete"; P-WALK: "edit handling
duplicated across five parallel walks"; P-INVALIDATE: "invalidation scattered across every mutator")
reducing to one disease — **"state/logic/invalidation replicated instead of owned by one chokepoint."**
Model A had *two* mutable representations (live scene ↔ inverse-edit history) and a *second* implicit
one for the agent; L2 collapses them to **one canonical CST with one apply**. The agent cannot be "a
fresh place to forget a step" because it uses the *same* step the GUI uses. **This is the single
strongest argument for the whole Model-B pivot, and Facet 5 is where it pays off most visibly:** the
agent is not a bolt-on with its own mutation semantics; it is a peer client of the one pathway.

**Human + agent co-editing (turn-taking vs locking vs merge).** Three regimes; the recommendation
is **optimistic concurrency with structural conflict detection, not locking**:

- **v1 — single-writer, optimistic (recommended default).** One *committing* writer at a time: the
  in-app session (human + the in-process agent share the controller and the undo stack) commits
  directly; an **external** agent is **propose-only** — its patch stages as a proposal the in-app
  owner approves (the human sees the agent's diff and clicks Apply, or the agent runs at an autonomy
  level that auto-applies in scope). Every commit carries the `baseHeadVersion` it read at (checked
  against the live `headVersion`, §2.2.1.1); a commit whose precondition lost the race is **rejected
  with a `CONFLICT`** and the loser re-reads + rebases.
  This is exactly a coding agent racing a human in the same file: last-write-*wins* is forbidden;
  the stale writer rebases. (Carried from TRANSACTION_MODEL §4.4, now expressed against CST versions
  rather than Model-A epochs — same contract, cleaner substrate.)
- **v2 — structural three-way merge (the Model-B prize, deferred).** Because the canonical object is
  a CST with **immutable per-node identity** (the `NodeId` lineage, D9/D15) addressed by name-path
  (L5) and formatting-stable serialization (INV-4), two concurrent patches that touch *different
  nodes* can be **merged automatically** (the same way git merges non-overlapping hunks, but at AST
  granularity keyed on NodeId, so it is *semantic* not textual — two edits to `lights/key.power` and
  `materials/gold.roughness` never conflict even if textually adjacent). Only patches touching the
  *same* node conflict, and they conflict *precisely* (the node's identity), not "the file changed."
  (A `rename` is itself a NodeId-preserving op, D9 — it changes a name-path but not the node it
  identifies, which is exactly why merge must key on NodeId, not the mutable name-path; cf. open
  question #1.) This is strictly better than text merge and is uniquely enabled by the lossless-CST
  pivot. Deferred past v1 because it needs Facet 1/3's merge primitive, but it is
  the natural end state and the reason locking is the wrong model.
- **Locking — rejected.** Per-node locks would serialize the human and the agent into a stilted
  turn-taking dance and reintroduce lock-lifetime state (a P-STATE relapse). Optimistic + rebase
  (v1) → structural merge (v2) is the git-native answer and fits the agentic cadence.

**External-file conflict (per [`01-DECISIONS.md`](01-DECISIONS.md) §D6/§D17).** The above governs
*in-process* concurrent writers (CST versions). A distinct hazard is the file changing on disk under
the session — a `git checkout`, another editor, or the CI migrator. The save path records a load/flush
**content fingerprint** — **(size, mtime) as a fast prefilter, upgraded to a content hash** when the
prefilter trips or determinism is required (D17; the same fingerprint definition the AssetManifest
uses, §5) — and the save itself is **atomic**: write a temp file in the target directory → `fsync` →
**revalidate** that the target's current content hash still equals the loaded fingerprint → atomic
`rename()` over the target. If the revalidate finds the on-disk hash moved since load, the flush is
refused and the user/agent is offered reload / diff / force-overwrite — never a silent clobber. (This
replaces D6's looser stat-then-write "compare-and-swap," which had a TOCTOU window; the **documented
residual** is that a non-cooperating concurrent writer can still last-writer-win at the final rename,
for which D17 offers opt-in advisory file locking on shared storage.) A headless agent's natural
answer is to **emit a branch/PR rather than write in place** (§ deployment), turning the conflict
surface into the git-native review flow. (v7 is single-file per §D7, so this is one file per document —
no cross-file atomic-save problem.)

### 2.5 `ValidationReport` — structured errors localized to CST nodes

`validate` (and the pre-commit check inside `propose_patch`) returns:

```jsonc
{ "ok": false, "diagnostics": [
  { "severity": "error", "code": "UNKNOWN_PARAMETER",
    "nodePath": "materials/gold", "chunk": "ggx_material", "parameter": "roughnes",
    "line": 42, "column": 5,
    "message": "Parameter 'roughnes' is not declared on chunk 'ggx_material'.",
    "suggestion": "roughness" },
  { "severity": "error", "code": "UNRESOLVED_REFERENCE",
    "nodePath": "objects/crate.material", "chunk": "standard_object", "parameter": "material",
    "line": 88,
    "message": "References material 'gold' but no material node named 'gold' is defined.",
    "candidates": ["gold_rough", "brass"] } ] }
```

The Model-B addition over the prior design is **`nodePath`** — every diagnostic is anchored to a
**CST node by name-path** (L5), not only a line/column. This matters because (a) the agent edits by
name-path, so an error keyed to a name-path is directly actionable; (b) line/column shift on every
insertion edit, whereas a name-path re-resolves to the same node across such edits (a *rename*
changes it — the durable handle is the node's `NodeId`, D9/D15 — but for the read→validate→fix loop
the name-path the agent just used is the right currency to key the diagnostic on); (c) the GUI binds
the *same* diagnostic
to the *same* widget (the "problems gutter" and the panel field light up together — one report, two
clients, mirroring L2). Stable diagnostic `code`s (machine-matchable): `SYNTAX_ERROR`, `SCENE_VERSION`,
`UNKNOWN_CHUNK`, `UNKNOWN_PARAMETER`, `TYPE_MISMATCH`, `VECTOR_CARDINALITY`, `NON_INTEGER_UINT`,
`ENUM_VALUE_INVALID` (closed enums only), `MISSING_REQUIRED`, `DUPLICATE_NAME`, `UNRESOLVED_REFERENCE`,
`WRONG_PIPE` (the IScalarPainter color-vs-scalar failure), `FILE_NOT_FOUND`, `BARRIER_COMMAND`,
`CONFLICT` (stale precondition), `REDACTED_WHOLESALE_REPLACE`. `suggestion`/`candidates` reuse the
`SuggestionEngine` ranking.

### 2.6 Agent-edit safety & validation (guardrails)

The safety model is **"the engine, not the model, is the authority"** (AI_SECURITY_MODEL §0), which
survives the pivot wholesale because it never depended on the canonical form. The Model-B-specific
strengthening: validation and atomicity are now *cleaner* because there is one apply.

1. **Every agent edit is validated (parse + derive) before it commits.** `propose_patch` runs
   `validate` on the candidate CST and, in `full` mode, a derivation dry-run (does the scene actually
   *build* — references resolve, the IScalarPainter pipe is satisfied, files exist), and **refuses to
   commit on any error**, returning the diagnostics. The scene is **never left half-edited**: an
   invalid patch is rejected *before* it touches the canonical CST (see #5). This is the single most
   important guardrail and it is structurally enforced — there is no code path that commits an
   un-validated patch.
2. **Partial/invalid edits cannot corrupt the canonical CST (atomic apply).** A patch is
   **all-or-nothing**: the server builds a *candidate* CST off to the side (apply ops / re-parse text),
   validates it, and only on success does it become the new committed version (Facet 3's versioning is
   the commit primitive). A `CstPatch` with five ops where op 3 is invalid commits **zero** ops — not
   two-and-a-half. This is trivial under Model B (the candidate is a separate immutable tree) and was
   *hard* under Model A (a live mutable scene half-mutated mid-apply is exactly the P-WALK hazard).
   Parallel patches in one agent turn are serialized (reject-stale-then-reread) or composed into one
   atomic patch on explicit "apply all" intent — never silent last-write-wins.
3. **Sandboxing destructive ops (always-confirm set).** Independent of autonomy level, a fixed set
   requires a human confirmation **the model cannot satisfy**: `remove`-ing a node, replacing/loading
   a different document, overwriting a file on save/export, enabling an external MCP server, and the
   first cloud egress of new content. L2/L3 drop *per-edit* confirmation for ordinary in-scope edits
   and renders but **never** for this set — an injection can never escalate its own autonomy
   (AI_SECURITY_MODEL §2.2.3, §5). The diff-review gate (L1) is the human firewall: an injected
   "delete all lights" is a red CST diff the user rejects.
4. **No shell, no arbitrary filesystem, no arbitrary network.** The agent operates *RISE*, not the
   machine. There is no run-command tool and no general file read/write; **every file-path argument
   resolves through `ProjectRootJail`** (no model-supplied absolute/`..` paths; Save-As materialized
   only via the human OS dialog), and every outbound URL through `IOutboundUrlPolicy` (SSRF range-block,
   HTTPS-or-loopback, pinned-resolution, redirect re-validation). Untrusted content (scene comments,
   log tails, asset metadata, **external-MCP results**, framebuffer OCR text) is wrapped in
   `<rise:untrusted source=…>` markers — data, never instructions. **All carried verbatim from
   AI_SECURITY_MODEL — orthogonal to Model B, fully retained.**
5. **Secret-byte offset faithfulness (carried).** `read_document`/`read_node` length-preservingly
   mask secrets with a single-byte ASCII mask so the agent's byte offsets equal the canonical
   document's; a whole-document `TextPatch` derived from a redacted read is refused
   (`REDACTED_WHOLESALE_REPLACE`); scoped `CstPatch` ops never touch masked spans. (AI_SECURITY_MODEL
   §3.1; the only adjustment is that "wholesale replace" is now "a whole-document text patch," still
   refused.)

6. **Provenance / attribution of agent vs human edits.** Every CST version records **who authored the
   patch** — `human:gui`, `agent:in-app:<session>`, `agent:external:<client-id>`, `agent:headless` —
   as version metadata (Facet 3 owns the version-history model; this is one field on a commit). The
   payoffs: (a) the GUI's "show me the code" panel and the version timeline label each change by author
   (a human can see "the agent touched these 4 nodes"); (b) git-native review (§2.7) carries authorship
   into the commit trailer (an agent-authored scene change is a reviewable PR with a `Co-Authored-By`
   line, exactly like agent-authored source); (c) undo/blame across the document is author-aware. This
   is strictly richer than Model A's "attributed to originating session" because the attribution rides
   the *version*, which is the canonical thing, not a transient transaction.

### 2.7 Diff-able / git-native / reviewable scenes (the workflows the pivot unlocks)

This is a *product capability*, not a tool, and it is the cleanest dividend of "canonical text."
Because a scene is a **formatting-stable** (INV-4) `.RISEscene` document and the canonical object is
the CST, **a scene change is an ordinary text diff** — and everything git does to text, RISE scenes
inherit for free:

- **PR-review a lighting change.** An agent (or a junior artist) proposes "warm the key light, drop
  exposure ⅓ stop, add a rim." That is a 6-line diff on `lights/key.power`, `lights/key.color`,
  `cameras/hero.exposure`, and a new `lights/rim` chunk. A reviewer reads the diff, sees the rendered
  before/after (CI renders both versions — §7.2), and approves. The review artifact is the *same* CST
  diff the L1 gate shows in-app (one diff representation, every surface).
- **Bisect a regression.** "The render got muddy 30 commits ago." `git bisect` over the scene history,
  re-rendering at each step, pinpoints the exact CST version (and, via provenance §2.6, *who/what*
  made it). This is impossible when the file is a lossy serialization of a mutable model. **The
  bisect is sound because renders are deterministic (D33):** `RenderConfig` carries a sampling
  seed / RNG-stream id, so a render is a pure function of its `PreparedStamp` — the same scene version
  at the same `RenderConfig`+seed re-renders **bit-for-bit**, so a pixel difference across a bisect step
  is attributable to the CST change, not RNG. **Caveat (D28): re-rendering an old version uses
  *current* asset bytes** — history preserves the CST (the source), not the rendered output, so if a
  referenced texture/mesh/HDRI changed on disk since that version, the re-render may differ even though
  the CST and the seed are identical (see the note below).
- **Template / library scenes.** A scene is a program; programs compose. A studio keeps a library of
  reviewed lighting rigs, material packs, camera setups as scene fragments, branches a shot from a
  template, and cherry-picks an approved material change across shots. Declarative iteration (L3 —
  instancers/function-expressions, no `FOR`/`DEFINE` macro-expansion) keeps these fragments
  *parametric and diffable* rather than expanded-and-opaque.
- **Branch a look.** "Try a noir grade" = a branch. Three lighting variants = three branches a
  contact-sheet render compares. The agent's `lighting_variants` prompt becomes *N branches*, each a
  reviewable, mergeable document.

**What history preserves — the CST only (per [`01-DECISIONS.md`](01-DECISIONS.md) §D28).** The
branch/PR flow, `git bisect`, and any "re-derive an old version" path preserve the **CST (the
source)**, **not** the historical rendered output. Re-deriving an old version (`Scene =
f(CST, AssetManifest)`, D5) re-stamps the manifest against the **live filesystem**, so it uses the
**current** asset bytes: if a referenced texture/mesh/HDRI/spectral file changed since that version,
the old version's *render may differ* even though its CST is byte-identical. This is the deliberate
git framing — git versions source while large binary build-inputs are the user's responsibility — and
the `f(CST, AssetManifest)` purity holds *within* a manifest, not across time (where the manifest is
the live filesystem). **Bit-for-bit reproducible historical renders therefore need a
content-addressed asset store** (snapshotting asset bytes by hash — a git-LFS-style layer at the VCS
boundary), which is a **named future option, NOT the editor** (D28): RISE's history layer stores the
CST, and an asset CAS, if it lands, is a separate VCS-boundary feature.

**What makes formatting-stable diffs possible (ties to INV-4, owned by Facet 1):** the lossless CST
round-trips text unchanged, and a structured edit **re-serializes only the touched node's span**,
leaving every other byte (whitespace, comments, ordering) identical. So a one-parameter change is a
**one-line diff**, not a whole-file reflow. Without this, every agent edit would churn the entire file
and diffs would be useless — formatting stability is the load-bearing precondition for the entire
git-native story, which is *why* it is a charter invariant. Facet 5 is the primary consumer; Facet 1
is the guarantor.

### 2.8 Transports & deployment modes

Three transports behind one `AgentTransport` interface (carried from the prior design — the transport
layer is independent of the canonical-form decision):

| Transport | Used by | Notes |
|---|---|---|
| **In-process** | the in-app chat panel (desktop + Android via JNI) | Shares the address space + the live CST store; lowest latency for the see→edit→render→see loop; the **owner**/committing client by construction. |
| **Loopback HTTP** (127.0.0.1 + per-launch token + `Origin` check) | external clients attaching to a *running* GUI ("Claude Desktop drives my open RISE window") | The only way a second client reaches a running instance; **propose-only** (its patches stage as proposals the owner approves). Android: adb-forwarded loopback only — no same-LAN bind, ever. |
| **stdio** (`rise --agent-stdio scene.RISEscene`) | headless clients that *spawn* RISE (Claude Code, CI, cron) | OS process boundary is the trust boundary; **read+validate only by default**, committing only under an explicit operator launch grant (§7.2). |

**Deployment modes (the charter's explicit ask):**

- **In-GUI embedded agent.** The chat panel is an in-process MCP client; the human and the agent
  co-edit one CST (§2.4). This is the headline product experience and the Android Tier-A surface
  ("talk to your renderer from your phone").
- **Headless / CI / cron agent (respecting the auth caveat).** `rise --agent-stdio` (or the existing
  `printf "render\nquit\n" | rise` extended with an `--agent` mode) is a headless MCP server a script
  or CI job spawns. **The platform caveat from the charter is real and shapes the default:**
  *interactively-authenticated MCP servers (and cloud-LLM credentials) may be absent in headless/cron
  runs.* Therefore:
  - The **safe default headless posture is `read+validate`** — a spawned server can lint/validate/
    derive/render a scene with **no credentials and no autonomy grant**. This is the CI sweet spot:
    "validate every scene in the corpus," "render thumbnails of all branches," "fail the build if any
    scene doesn't derive" — none of which needs an LLM or a cloud key. The *engine-side* tools
    (`validate`, `render`, `read_*`) are useful to a CI pipeline **with no agent at all**.
  - A headless run that *does* drive an LLM must get its provider key from the environment / a
    secrets manager (never interactive paste), and committing requires `--agent-autonomy=commit`
    (the operator's launch act *is* the owner approval). The always-confirm destructive set still
    fails closed unless a separate explicit headless-destructive grant is given. (Carried from
    MCP_TOOL_SURFACE §4.0 / AI_SECURITY_MODEL §8.1.6.)
- **Local model vs hosted.** A loopback-LLM endpoint (Ollama / LM Studio / llama.cpp at
  `127.0.0.1`) is the **no-egress** path — scenes and framebuffers never leave the machine; the
  egress indicator reads "Local — no data leaves this machine." A hosted provider (Claude / Gemini)
  or a *non-loopback* "local" endpoint is treated as cloud egress (consent + indicator). Vision is
  gated by `supportsVision`; text-only local models degrade the loop to the numeric/structural
  observation channel (§2.3). (Carried from LLM_AGENT_RUNTIME §4.1 — the address class, not the
  label, sets the egress regime.)

### 2.9 Product framing & differentiation — what this makes RISE

**The crisp claim:** RISE is **the only production spectral renderer whose scene is a canonical,
diff-able, version-controllable program — operated by humans and coding agents through the same
read→patch→validate→render→observe loop, with a UI that is a pure projection of that program.**
"OpenSCAD's ergonomics + a real spectral renderer + agent-native," not "Maya with a script console"
(charter §1).

The concrete "nerd" workflows (CLI + agent + git + a dynamic UI), none of which any incumbent offers
as a coherent whole:

- *"Lint my scene in CI."* `rise --agent-stdio --validate scene.RISEscene` → structured diagnostics,
  exit code. No GUI, no LLM, no cloud. (Blender: no canonical text; OpenSCAD: a CSG language, not a
  renderer; Houdini: a binary `.hip`.)
- *"PR-review a lighting change."* The change is a CST diff; CI renders before/after; a reviewer
  approves. Scene review becomes code review.
- *"Get the sunset believable."* An agent runs the vision loop against the *spectral* render until it
  looks right — chromatic correctness an RGB renderer literally cannot judge.
- *"Bisect the regression / branch the look / template the rig."* git, applied to scenes.
- *"Operate my open window from Claude Desktop."* Loopback-HTTP, propose-only, every agent edit a
  reviewable diff and a single undo.

**Where it sits vs the field:**

| Tool | Canonical text? | Real renderer? | Agent-native? | Git-native scenes? |
|---|---|---|---|---|
| **OpenSCAD** | yes (its language) | no (CSG modeler) | no (no agent loop) | yes (text) |
| **Houdini** | no (binary `.hip`; HDA/VEX are scripting *within* a binary doc) | yes | partial (Python console) | no |
| **Blender + scripts** | no (binary `.blend`; `bpy` mutates a live model = Model A) | yes | partial (bolt-on script console) | no |
| **RISE (Model B)** | **yes (lossless CST)** | **yes (spectral)** | **yes (one read/patch/validate loop)** | **yes (formatting-stable CST diffs)** |

RISE is the only row that is *all four*. The differentiation is not "RISE has an AI feature" — every
DCC is bolting one on. It is that **RISE's canonical representation is the thing agents and git are
already good at**, so the agentic surface is thin, honest, and native rather than a translation layer
over a mutable model. That is a structural moat: a competitor with a binary canonical document cannot
match the diff/branch/review/bisect/CI-lint story without first rebuilding their foundation on text —
which is the Model A→B migration this whole charter is.

---

## 3. Delete / Evolve / Reuse

Explicit fate of every component this facet touches. The unifying move: **the Model-A "two ways to
persist an edit" (structured-save vs wholesale-rewrite) is deleted; there is one CST patch.**

| Component (today / on-paper) | Fate | Detail |
|---|---|---|
| **`apply_scene_text` / "wholesale rewrite + reload" tool** (MCP_TOOL_SURFACE §4.5) | **DELETE** | The Model-A fallback for edits structured-save couldn't represent. Under Model B every edit is a CST patch; a whole-document edit is just a `TextPatch` over the whole span — one more version, not a history-destroying document swap. The tool, the `REDACTED_WHOLESALE_REPLACE` *special-case-for-this-tool*, and the "swap re-roots a fresh UUID / loses undo" semantics all go. (The redaction *refusal* survives, attached to whole-document `TextPatch`.) |
| **The `today(structured)` vs `today(wholesale)` per-tool persistence taxonomy** (MCP_TOOL_SURFACE §4.0 legend, §4.1–§4.5) | **DELETE** | An entire axis of the prior catalog. There is no "this tool persists structurally, that one needs a wholesale reload" — Facet 1's lossless CST makes *every* patch persist structurally by re-serializing only the touched span. "Non-camera creation needs round-trip save" (the prior genuine gap) **evaporates**: creating any node is an `add` CstPatch op, persisted like any other. |
| **The ~25-RPC mutation catalog** (`set_property`, `set_transform`, `apply_material`, `load_hdri`, `add_entity`, `remove_entity`, `clone_entity`, `set_active_camera`, `frame_object`, `set_lens`, `create_camera`, …) | **EVOLVE → collapse into `propose_patch`** | Each becomes a `CstPatch` op (`set`/`add`/`remove`/`rename`) the agent expresses against the document, or a *prompt* that desugars to one. No per-operation schema, no per-operation persistence story. (Convenience: a thin set of named prompts may remain for discoverability.) |
| **`baseEpoch` `(uuidHi, uuidLo, revision)` precondition + the committed/proposed contract + conflict** (MCP_TOOL_SURFACE §4.0/§4-head; TRANSACTION_MODEL §4.4) | **EVOLVE → `baseHeadVersion` over CST versions** | The contract is *right* (optimistic concurrency, no last-write-wins, conflict-on-stale) but re-expressed against Facet 3's CST version identity rather than Model-A transaction epochs, and split into the coherent **`{headVersion, DerivedStamp, PreparedStamp, status}`** surface (§2.2.1.1, D13/**D29**) so a precondition keys on `headVersion` (compared by **DAG ancestry, not numeric `<`**) while derived reads stamp the **`DerivedStamp`** and rendered reads the **`PreparedStamp`**. Same shape, cleaner substrate. The committed-vs-proposed split (owner commits / external proposes) survives as the v1 concurrency model (§2.4). |
| **`SceneEditController` as "the one mutation path"** | **SUPERSEDE** (Facet 3 owns the replacement) | The prior specs route every tool through `SceneEditController::Commit`. Charter L7 supersedes the `SceneEditor`/`SceneEdit`/`EditHistory`/transaction subsystem with CST versioning. Facet 5 retargets "one mutation path" onto **Facet 3's one CST-edit pathway** — same principle (#6 → INV-6), new mechanism. This facet does not design the edit layer; it consumes it. |
| **`SceneValidator` / `validate` (isolated throwaway `Job`)** (VALIDATION_ARCHITECTURE §3) | **EVOLVE** | Keep `validate` as the keystone; keep "side-effect-free via an isolated derivation, not a mock." But the implementation now rides Facet 1's parser-to-CST + Facet 2's `derive` against a throwaway context, with diagnostics gaining `nodePath` (§2.5). The barrier-command policy (`quit`/`render`/`load`/`run` neutralized; `exit(1)` unreachable) survives. |
| **`SchemaGen` (descriptors → JSON Schema) + `SchemaConformanceTest`** (MCP_TOOL_SURFACE §5; VALIDATION_ARCHITECTURE §5) | **REUSE** | Unchanged by the pivot — descriptors are still the schema (L6). Still a *first-pass filter, not the parser*; still held honest by the conformance test (the generated schema can over-/under-accept vs the tolerant parser). `read_schema` exposes it. |
| **`rise://*` resource catalog** | **EVOLVE → CST-rooted** | `rise://scene/text` → `rise://cst/text`; `rise://scene/chunk/{name}` → `rise://cst/node/{name-path}` (L5). `rise://scene/graph`, `rise://framebuffer`, `rise://render/*`, `rise://log`, `rise://grammar/*` survive with the same contracts. The structured-envelope + single-byte-mask redaction contract survives (offset faithfulness). |
| **The threat model** (AI_SECURITY_MODEL: `ProjectRootJail`, `IOutboundUrlPolicy`, `SecretRedactor`, always-confirm set, loopback-only, untrusted-content quarantine, adb-loopback) | **REUSE wholesale** | Orthogonal to the canonical-form decision. Every mitigation holds. The *only* adjustment: "wholesale replace" is now "whole-document text patch" (still refused when redacted). |
| **Transports** (in-process / loopback-HTTP / stdio) + **the single `RenderCoordinator`** + **`ILLMProvider`/adapters/credentials/vision-loop** (LLM_AGENT_RUNTIME) | **REUSE** | Transport, render arbitration, and the LLM runtime are all independent of the canonical form. Carried forward as designed. (The runtime's "L1 staging = proposed transaction" re-expresses as "proposed CST patch.") |
| **`AsciiCommandParser` headless `> render`/`> load`/`exit(1)`** (`src/RISE/commandconsole.cpp`) | **REUSE + fence** | The existing CLI loop stays for back-compat; the agentic `--agent-stdio` mode fences barrier commands out of any `validate`/agent path (the prior barrier-shim design). |

---

## 4. Hard problems & open questions

1. **Structural three-way merge of CST patches (the v2 prize) is genuinely hard.** AST-granular merge
   keyed on the immutable **NodeId** (D9/D15 — *not* the mutable name-path) is *better* than text merge
   in principle, but defining "do these two patches conflict?" precisely — across `rename` (which moves
   a name-path but preserves the NodeId), declarative-iteration generators (where one logical edit fans
   out to many derived instances, L3), and reference rebinding — is a real research/engineering problem
   owned jointly with Facet 1/3. v1 ships single-writer-optimistic to avoid blocking on it, but the
   product story (§2.7) leans on merge eventually. **Open:** is NodeId disjointness a sufficient
   non-conflict predicate, or do reference edges create hidden conflicts (edit A renames
   `materials/gold`, edit B binds `objects/x.material gold`)?

2. **`validate`/`derive_preview` cost under the agentic cadence (the latency tar-pit, charter's named
   risk).** The loop's value is fast feedback. D34 moves the *expensive* phases (full derive, TLAS
   rebuild, photon passes, prepare, render) **off the edit thread onto the async, cancellable arbiter**,
   so a successful `propose_patch` *commit* returns immediately — but the **pre-commit validation gate**
   (§2.4 #1) still runs synchronously before the commit is allowed, and a full derive-dry-run there
   could be slow on a heavy scene (deferred realization, etc.). Facet 2 owns incremental derivation;
   Facet 5 *depends* on it being incremental enough that "validate this one-parameter patch" is
   interactive. **Open:** what is the latency budget for the synchronous `propose_patch` validation
   gate, and can a cheap `syntax`+reference tier (no full realize) gate the common case while a `full`
   derive is reserved for pre-render (and runs async on the arbiter post-commit, D34)? (Mirrors the
   GUI's gutter-vs-deep-validate split.)

3. **Vision-loop economics + reference-compare needs a backing accessor that doesn't exist.** The
   per-region variance/RMSE read-back the reference-compare loop (§2.3) wants is **net-new and not in
   master** (the auto-router's per-pixel `ProbeResult` is private and discarded; SPECTRAL_DIFFERENTIATORS
   D5). Until it ships, "measure convergence to a reference" degrades to whole-frame metrics or eyeballing.
   **Open:** sequence the D5 variance accessor early enough that the headline vision/reference loop is
   real, not aspirational.

4. **How thin is too thin? Discoverability of the patch surface.** A 5-verb surface is elegant, but an
   agent must *discover* that "make the light warmer" = a `set` op on `lights/key.color`. The schema
   (`read_schema`) + the graph (`read_graph`) + prompts carry this, but **open:** do we need a small set
   of *named convenience prompts* (not RPCs) as an affordance/onboarding layer so the model doesn't have
   to reconstruct common operations from first principles every time? (Leaning yes — prompts, which carry
   no authority, are the right place; they desugar to patches.)

5. **Text-patch ↔ CST-patch equivalence at the seams.** Both encodings must converge to the same
   canonical CST (§2.2.2). A `TextPatch` that edits a comment, reorders blocks, or touches whitespace
   has no `CstPatch` equivalent — fine, that's the text encoding's job — but the *diff the L1 gate shows*
   and the *provenance recorded* must be coherent for both. **Open:** when an agent sends a `TextPatch`,
   does the gate show the text diff (faithful to what the agent did) or a re-derived structured diff
   (faithful to the CST delta)? (Leaning: show the text diff for a `TextPatch`, the structured diff for
   a `CstPatch`, since each is what its author actually expressed.)

6. **Headless autonomy + the auth caveat is a sharp edge.** The safe default (read+validate, no creds)
   is clearly right for CI, but the moment a cron job wants an *autonomous* agent loop, it needs a cloud
   key in the environment and `--agent-autonomy=commit` — and the always-confirm destructive set has *no
   human to confirm it*. **Open:** is "fail closed on the destructive set unless a separate explicit
   grant" sufficient, or does headless autonomy need a fundamentally different (e.g. dry-run-only, or
   PR-emitting-not-committing) posture? (Leaning: headless autonomy should **emit a branch/PR**, not
   commit to the working document — turning the auth caveat into a feature: the headless agent proposes,
   a human merges. This makes §2.7's "PR-review a lighting change" the *native* headless mode.)

7. **Multi-document headless server.** A CI job rendering a directory wants one process, many scenes.
   v1 assumes one document per server (simplest). **Open:** multi-document session keying, or
   spawn-per-scene? (Leaning spawn-per-scene for isolation; multi-document is an optimization.)

---

## 5. Cross-facet dependencies & assumptions

What Facet 5 assumes about its neighbors (for synthesis to reconcile):

- **From Facet 1 (CST & scene language):** (a) the lossless CST round-trips text unchanged and a
  structured edit re-serializes **only the touched span** (INV-4) — *the* precondition for the entire
  git-native/diff story (§2.7); without it the agentic surface's headline differentiator collapses.
  (b) **Dual identity** (D9/D15): an **immutable `NodeId`** is the stable *lineage* identity (the
  durable handle an agent holds across edits/renames), and **name-path** (`objects/sphere.material`,
  L5) is the version-resolved *addressing* scheme (changes on rename) — every read tool, every
  `CstPatch` op, and every diagnostic `nodePath` addresses by name-path, while durable cross-turn
  references key on the NodeId Facet 1 exposes. (c) The parser produces the canonical CST from text
  (so a `TextPatch` → re-parse → CST is well-defined). (d) `rename` fixes up referrers from the
  **traced `ReferenceUse` records** (D14 — captures dynamic refs like `timeline.element`), with a
  descriptor-resolver fallback for un-derived subtrees and an explicit flag for any referrer it
  cannot resolve (never a silent dangling rename) — Facet 5 surfaces whatever Facet 1 reports. The
  trace `rename` rewrites from must be **stamped for the exact head** (D25) and must come from the
  **one resolution path — derivation's own resolver, not a separate tracing-pass reimplementation**
  (D35 corrects D25): if the derived view lags head, rename **synchronously derives head** (sharing
  derivation's resolution step) before rewriting referrers, and **refuses** if head cannot be derived
  (a semantic error) — Facet 5 never issues a `rename` against a stale trace or a second resolver.
- **From Facet 2 (derivation engine):** (a) `derive` is **incremental and fast enough** that
  pre-commit validation and `derive_preview` are interactive on common edits (open question #2). (b) A
  **throwaway/isolated derivation context** exists so `validate`/`derive_preview` have no side effects.
  (c) `derive` is pure & deterministic (INV-2), **and `prepare`/`render` are deterministic too** — the
  `RenderConfig`'s **sampling seed / RNG-stream id** (D33) replaces `rand()`-seeded stochastic prep
  (photon tracing), so a render is a pure function of its `PreparedStamp` and a rendered preview is
  **reproducible** (same `PreparedStamp` → same image; the `f(CST, AssetManifest)` purity holds within a
  manifest, D28). (c′) **`derive → prepare → render` run async + cancellable on the single render
  arbiter, off the edit thread** (D34) — Facet 5 commits a cheap CST version and the arbiter does the
  heavy work, a newer head cancelling the in-flight job; this is the source of the head-vs-derived lag.
  (d) **Two derivation layers** (D22): a config-**independent** `DerivedScene = f(CST, AssetManifest, t)`
  (what `read_graph` reflects) and a config-**dependent** `PreparedRenderState =
  prepare(DerivedScene, RenderConfig)` (light samplers, photon maps — what an image reflects), so an
  integrator override re-runs only `prepare`. (d′) **An animation frame derives a time-INTERVAL scene**
  (D31): animated quantities are immutable read-only functions over the shutter (per-sample motion blur,
  no scene mutation), the active **animation name** is part of the `DerivedStamp`, and **motion blur is
  gated v1-off** (single-time in v1; interval scene + motion BVH is the named follow-on). (e) **History
  is CST-only** (D28): re-deriving an old
  version re-stamps the `AssetManifest` against the live filesystem (current asset bytes), so a
  reproducible historical render is a future content-addressed-asset-store concern, not a derivation
  guarantee — `f(CST, AssetManifest)` purity holds within a manifest, not across time.
- **From Facet 3 (edit model & history):** (a) **the one CST-edit pathway** both clients commit through
  (L2/INV-6) — Facet 5 is a *client* of it, not its designer. (b) CST **versioning** is the commit
  primitive (atomic apply, the `baseHeadVersion` precondition, conflict-on-stale, undo/redo/branch).
  (c) **Version metadata carries provenance/authorship** (§2.6) — Facet 5 assumes a free-text-ish
  author field on each commit. (d) The eventual **structural merge** primitive (v2, §2.4).
- **From Facet 4 (dynamic UI):** the GUI emits `CstPatch` for its edits and binds widgets + diagnostics
  by name-path — i.e. the GUI is the "human client" of §2.4. Facet 5 and Facet 4 share the L2 mechanism;
  the *only* contract between them is "both emit patches to Facet 3 and re-read from Facet 1/2."

**Conflicts with Locked/Open decisions to flag for synthesis:**

- **No conflict with any Locked decision.** Facet 5 *embodies* L2 (one edit pathway, two clients), L5
  (name-path identity), L6 (descriptors are the schema), L7 (supersede `SceneEditController`/`SaveEngine`).
- **O1 (lossless-CST vs pure-text-canonical):** Facet 5 designs for lossless-CST-pivot. **If the reviewer
  chose pure-text-canonical instead,** the delta is *small and favorable* for this facet: the two patch
  encodings collapse toward one (text is the canonical thing; a `CstPatch` becomes sugar that the server
  expands to a text edit), `read_node`-by-name-path needs a text-span index over the buffer (Facet 1
  provides it either way), and provenance/diffs are *more* trivially git-native (the buffer *is* the
  artifact). The agentic surface is arguably *simpler* under text-canonical; this facet does not depend
  on the CST being the canonical object, only on (i) formatting-stable diffs and (ii) name-path
  addressing — both of which either model provides.
- **O2 (debounced-commit vs 60Hz):** Facet 5 assumes debounced-commit, which matches the agentic
  edit→preview cadence perfectly (an agent doesn't drag a slider at 60Hz). No delta for the agent path.

---

## 6. First-slice implications (the minimal end-to-end vertical)

The charter's first slice is **one chunk type, text⟷CST⟷derived-scene, one schema-generated widget,
live incremental re-derive.** Facet 5's contribution to that vertical — the smallest agentic surface
that proves the whole thesis end-to-end:

1. **`read_schema` for the one chunk** — `SchemaGen` emits the JSON Schema for that one chunk's
   descriptor (L6). Proves the agent can learn the grammar from the source of truth.
2. **`read_document` + `read_node`** — return the canonical CST text and the one node by name-path
   (with the redaction envelope). Proves read + stable addressing.
3. **`validate`** — re-parse a candidate text/patch → a `ValidationReport` with a `nodePath`-anchored
   diagnostic (the `UNKNOWN_PARAMETER` → "did you mean…" case is the canonical demo). This is the
   single highest-leverage net-new piece and the keystone of the loop; build it first.
4. **`propose_patch` (structured, `set` op only)** — change the one chunk's one parameter, validated,
   committed as a new CST version through **Facet 3's edit pathway** (the *same* call the Facet-4 widget
   makes — proving L2 with one mechanism, two clients, in the first slice).
5. **`render` + `read_image`** — submit a render of the derived scene, read back the framebuffer.
   Closes the observe end of the loop (vision optional in slice 0 — `read_diagnostics` text suffices).

That is the entire coding-agent loop — **read → validate → patch → derive → render → observe** — over a
single chunk, sharing one edit pathway with the GUI widget, with the agent's view generated from the
descriptor. It demonstrates the product thesis (canonical, diff-able, agent-operated, dynamic-UI) in
miniature, and every piece is small: `SchemaGen` (filter), `validate` (the one genuinely new pass),
and a JSON-RPC adapter over Facets 1–3. Transports for slice 0: **in-process** (proves the in-app
agent) + **stdio** (proves headless `rise --agent-stdio` validate/render, which needs no LLM and no
credentials — the CI sweet spot, and the cheapest way to exercise the surface in tests).

---

## 7. Appendix — quick reference

### 7.1 The whole mutating surface (vs the Model-A catalog)

```
Model B (this doc):                      Model A (docs/gui/, superseded):
  propose_patch  (CstPatch | TextPatch)    set_property, set_transform, apply_material,
  validate                                 load_hdri, add_entity, remove_entity,
  render / stop / pause / resume           clone_entity, set_active_camera, frame_object,
  derive_preview                           set_lens, create_camera, apply_scene_text,
                                           save_scene, load_scene, export_image, …
  + read_* (document/node/graph/           + the same read_* / render_* tools
    schema/image/diagnostics)              + the structured-vs-wholesale persistence axis
                                             (DELETED — one CST patch persists everything)
```

### 7.2 CI lint/render recipe (no LLM, no cloud, no GUI)

```sh
# Validate every scene in a corpus (structured diagnostics + exit code):
rise --agent-stdio --validate scenes/**/*.RISEscene

# Render before/after for a PR-reviewed lighting change (the §2.7 workflow):
git checkout main      && rise --agent-stdio --render hero.RISEscene -o before.exr
git checkout feature   && rise --agent-stdio --render hero.RISEscene -o after.exr
# CI posts before/after to the PR; reviewer approves the CST diff.
```

These use only the **engine-side** tools (`validate`, `render`, `read_*`) — the agentic surface is
useful to a pipeline *with no agent at all*, which is the cleanest proof that the foundation, not a
bolted-on LLM, is the product.
