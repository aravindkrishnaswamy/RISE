# Facet 1 — Scene Language & Canonical CST

> **Updated per [`01-DECISIONS.md`](01-DECISIONS.md) (rounds 1 & 2).** Sections affected by
> D2, D3, D5, D7, D8, and D9 (round 1) and by D14, D15, D16, and D19 (round 2) have been
> rewritten to conform; this doc now points to the decision record as authoritative and
> contradicts none of D1–D20.
>
> **Status:** design-in-progress. Part of the RISE agentic redesign (Model B). Read
> [`00-CHARTER.md`](00-CHARTER.md) and [`01-DECISIONS.md`](01-DECISIONS.md) first — this doc
> inherits the charter's locked decisions (L1–L7), open decisions (O1–O3), invariants
> (INV-1…INV-6), and the decisions (D1–D20), and does not re-litigate them. Where this
> doc once conflicted with a decision, **`01-DECISIONS.md` wins** and the text below has been
> conformed.
>
> **This facet owns:** the lossless Concrete Syntax Tree (CST); the parser's evolution from
> one-way (`text → Job::Add*` fire-and-forget) to a retained, round-trippable tree; the
> text↔CST round-trip and formatting/comment preservation (INV-4); node identity — the
> three-way separation of **content hash (green, lossless, for sharing) / derivation key
> (semantic, trivia-insensitive, for memoization) / `NodeId` (per-occurrence lineage, in the
> red layer)** plus **name-path (addressing)** (D15/D9, refining L5); declarative iteration
> replacing `FOR`/`DEFINE`/`hal`/`$(...)`/macros (L3);
> coverage of all ~138 chunk types; the **single-file** scene-format version bump (O3 / D7).
>
> **Design only.** No source, build, or scene files are modified by this document.

---

## 1. Current-state grounding

### 1.1 The scene file today: two layers

A `.RISEscene` file (header `RISE ASCII SCENE 6`) is processed by
[`AsciiSceneParser::ParseAndLoadScene`](../../src/Library/Parsers/AsciiSceneParser.cpp)
(`AsciiSceneParser.cpp:10431`). There are **two distinct language layers**, and only one of
them is descriptor-driven:

- **Top-level preprocessing layer** (hand-rolled, line-oriented, in the main parse loop
  ~`AsciiSceneParser.cpp:10610–10810`): comments (`#`, `/* */`), embedded commands
  (`> load`, `> run`, `> set accelerator`), macro definition (`!`, `define`, `DEFINE`) and
  removal (`~`, `undef`, `UNDEF`), `FOR`/`ENDFOR` loops with `in.seekg()` body re-reads,
  inline math functions (`sin cos tan sqrt hal`) via `evaluate_first_function_in_expression`
  (`:191`), and `$(...)` arithmetic via `evaluate_expression` (`:298`). Macro substitution
  rewrites tokens *in place* (`@NAME` numeric / `%NAME` textual; `substitute_macro` `:10999`)
  **before** the chunk body is assembled.
- **Chunk layer** (descriptor-driven since 2026-04, per L6): every chunk parser derives from
  [`IAsciiChunkParser`](../../src/Library/Parsers/IAsciiChunkParser.h) and overrides exactly
  `Describe()` (returns a `ChunkDescriptor`) and `Finalize(const ParseStateBag&, IJob&)`. The
  default `ParseChunk` (`AsciiSceneParser.cpp:9861`) validates every input line against
  `Describe().parameters` via `DispatchChunkParameters` (`:697`), stores typed values in a
  `ParseStateBag`, then calls `Finalize`, which emits `pJob.AddX(...)`. **The descriptor is
  the schema** ([`ChunkDescriptor.h`](../../src/Library/Parsers/ChunkDescriptor.h)).

The two layers do not share a representation. The preprocessing layer is opaque to the
schema; the schema layer never sees a comment, a macro, or a `$(...)` (those are gone by the
time `DispatchChunkParameters` runs).

### 1.2 What is one-way and lossy

`Finalize` calls `pJob.AddSphereGeometry(...)`, `pJob.AddObjectMatrix(...)`, etc. — and *the
text is discarded*. The engine `Scene` is the only retained product. Re-serializing the scene
(`SaveEngine`) cannot reproduce the original file: comments, whitespace, parameter order,
`FOR` loops, macros, and `$(...)` expressions are all gone. This is precisely the lossy
serialization the charter's Model A diagnoses (§2), and the reason the round-trip
[`SaveEngine`](../../src/Library/SceneEditor/SaveEngine.cpp) had to grow the elaborate
byte-splice machinery described next.

### 1.3 The proto-CST already in the tree (critical — we build on this)

The round-trip-save effort already built **two-thirds of a CST** to survive the one-way loss.
This facet formalizes and *retains* what those phases compute transiently:

- **[`RawTokenCapture`](../../src/Library/Parsers/RawTokenCapture.h)** (Phase 0): captures,
  per source line, every raw token's `text` + `[byteBegin, byteEnd)` + `isSymbolic` flag
  (set when the token contains `$`), plus the trailing-comment byte range. It already gets the
  hard lexing right: `"..."` quoted strings are one token; `$(...)` balanced expressions are
  one token with internal whitespace/nesting preserved. It explicitly does **not** macro-
  substitute. Its FOR-loop note documents that `in.seekg()` re-reads produce duplicate
  `RawLine` entries with identical byte offsets, deduped downstream by `(byteBegin, byteEnd)`.
- **[`SourceSpanIndex`](../../src/Library/SceneEditor/SourceSpanIndex.h)** (Phase 6.1): per
  entity, the chunk's byte range, `{` / `}` offsets, per-parameter `ParameterSpan`
  (`lineBeginOffset`, `valueBegin/valueEnd`, `commentBegin`, `isSymbolic`), an `AuthorMode`
  (Euler/Quaternion/Matrix — mirroring `standard_object` precedence), a `chunkRevisited` flag
  for FOR-body entities, an `insideManagedBlock` flag, and `loadedPropertyValues`
  (descriptor-introspected loaded values for diffing). `ApplyOffsetDeltas` already shifts all
  stored offsets after a length-changing splice.

In other words: the engine already knows where every chunk, parameter, value-token, and
comment *lives in the bytes*, and which tokens are symbolic (macro/expression). It throws
that away after the save. **The CST is the retained, structured form of this data, promoted
from a save-time side-table to the canonical object.** (Per D2, §2.4, the retained form stores
**relative widths** in immutable green nodes, *not* the absolute `byteBegin/byteEnd` these
save-side structures carry — absolute positions come from a red cursor, and `ApplyOffsetDeltas`
is dropped. The positional *information* is retained; its representation changes.)

### 1.4 Declarative-generator precedents (the model for L3)

RISE already expresses repetition declaratively in several chunks — these are the existence
proof that L3 is viable, not speculative:

- **`expression_function2d`** (`AsciiSceneParser.cpp:5453`): a real function-expression
  sublanguage — repeatable `param <name> <number>`, repeatable `def <name> <expr>`
  (let-bindings), and a final `expr`, compiled to an
  [`ExpressionProgram`](../../src/Library/Painters/ExpressionEval.h). Example from
  `watch_dial.RISEscene`:
  ```
  expression_function2d {
      name   oxide_fn
      param  R 20.6
      param  Ea 160000
      def    rho  clamp(hypot((2*u-1)*R,(2*v-1)*R)/R,0,1)
      def    heat rho*rho
      def    Tk   700+heat*200
      expr   clamp((exp(-Ea/(2*8.314*Tk))-...)/...,0,1)
  }
  ```
- **`sweep_geometry`** / **`path_instances_geometry`** (`:5565` / `:5697`): repeatable
  `profile_point` / `point` / `point_width` control points; `path_instances_geometry` stamps
  a named template geometry along a Catmull-Rom path at arc-length `pitch` (homogeneous
  instancing — instances are *derived*, not authored).
- **`sdf_geometry`** (`:5368`): repeatable inline `part` lines compose primitives; or a
  heightfield `expression_function2d`. `watch_dial.RISEscene` uses **zero** `FOR` loops and
  **zero** `DEFINE` macros — every repeat is a declarative generator or a repeatable param.

`FOR`/`DEFINE`/`hal`/`$(...)` are used only by older scenes
(`scenes/Tests/Parser/loops.RISEscene`, `shapes.RISEscene`, a handful of FeatureBased/Parser
and Internal scenes). The go-forward corpus already prefers declarative form.

### 1.5 Name-path identity is already half-live

Animation already references entities by a `(category, name, param)` triple:
`timeline { element strap_screw  element_type object  param position ... }`
(`AsciiSceneParser.cpp:9513`), and `element_type` already includes `geometry` and `painter`
to reach *intrinsic* params (e.g. an `sdf_geometry`'s part fields). `SourceSpanIndex` keys
non-object spans by `DirtyEntity = (EntityCategory, name)`. L5's name-path
(`objects/strap_screw.position`, `geometry/dial_sdf.part[3].round`) is the generalization of
an addressing scheme the engine *already* uses for keyframes and dirty-tracking.

---

## 2. The Model-B design

### 2.1 Overview: three representations, one canonical

```
   .RISEscene bytes                  Derived Scene (engine)
        ▲  │                                ▲
  serialize │ Lex+Parse               derive │ (Facet 2)
        │  ▼                                │
   ┌──────────────────────────┐  Evaluate  ┌──────────────┐
   │      CST  (CANONICAL)     │──────────▶│  EvalTree /  │
   │   lossless, retained;     │            │  Scene objs  │
   │   red-green tree (green    │◀──────────│              │
   │   widths + NodeId) +       │  diagnostics back-annotate nodes
   │   descriptors              │
   └──────────────────────────┘
        ▲          ▲
   text edit   structured edit  (both mutate the ONE CST — Facet 3)
```

The CST is the **single source of truth** (INV-1). Text is its serialization; the derived
scene is `Scene = f(CST)`, pure and deterministic (INV-2, Facet 2 owns the function). This
facet specifies the CST data structure, the parser that produces it, identity, and the
declarative-iteration grammar. Facet 2 consumes the CST; Facets 3/4 edit it.

### 2.2 CST node kinds

The CST is a **red-green tree** (D2, §2.4): immutable, structurally shared **green nodes**
carry typed content, a **relative width**, and a stable **`NodeId`** (D9, §2.5) — never an
absolute byte offset. Absolute positions come from a version-specific **red cursor** (§2.4).
Node kinds (proposed `enum class CstNodeKind`, all are green-node kinds):

| Kind | Represents | Children | Schema link |
|------|-----------|----------|-------------|
| `Document` | the whole file | header + ordered top-level items | — |
| `Header` | `RISE ASCII SCENE 7` | — | format version (O3) |
| `Chunk` | one chunk block (`sphere_geometry { … }`) | a single **document-ordered** list of `Parameter` + `Comment` + `Blank` + `Trivia` children (every repeatable occurrence is an ordinary ordered `Parameter`) | `ChunkDescriptor` (by `keyword`) |
| `Parameter` | one `name value` line (including each occurrence of a repeatable param) | a `Value` | `ParameterDescriptor` (by `name`) |
| `Value` | a typed value | `ValueAtom`+ (scalar) or `Expr` (symbolic) | `ValueKind` / `tupleKinds` |
| `ValueAtom` | one literal token (`0.6`, `gold`, `TRUE`) | — | — |
| `Expr` | a symbolic value: a function-expression AST (§2.7) | operator/var/call sub-nodes | expression grammar |
| `Generator` | a declarative iteration block (§2.6) | a count/domain spec + a per-instance `Chunk` template | generator descriptor |
| `Comment` | `# …` or `/* … */` (standalone or trailing) | raw text | — |
| `Blank` | a run of blank lines | count | — |
| `Trivia` | indentation/inter-token whitespace, attached to the node it precedes | raw bytes | — |

**Key shape decisions:**

- **Comments, blanks, and whitespace are first-class child nodes / attached trivia**, not
  discarded (this is what makes INV-4 structural rather than best-effort). A trailing comment
  on a parameter line is a child of that `Parameter`; a standalone comment between two chunks
  is a `Document`/`Chunk`-level `Comment` child in document order. This generalizes the
  `commentBegin` field `SourceSpanIndex` already records.
- **Order is preserved, and document order is the canonical structure (D3).**
  `Chunk.children` and `Document.items` are **persistent balanced sequences (ropes)** for wide
  lists and may stay plain vectors for narrow ones (D16, §2.4). *Every* repeatable occurrence
  (every `part`, `cp`, `point`, `value`, `time`, `interpolator`, `shaderop`) is an ordinary
  ordered `Parameter` node living in `Chunk.children` in document order, interleaved with
  `Comment`/`Blank`/`Trivia` exactly as written. There is **no `RepeatGroup` container** that
  pulls occurrences out of document order — doing so would lose the interleaving and break
  order-dependent semantics. A structured edit that changes a value does not reorder; an edit
  that adds a parameter inserts at a descriptor-suggested or end-of-block position (Facet 3's
  policy). Per **D16**, the `Document`'s chunk list and any large `RepeatGroup` (e.g. a
  10 000-`part` SDF heightfield) use the rope so position lookup *and* insert/remove are
  O(log N); a chunk's handful of params may remain a small vector.
- **`RepeatGroup` is a *derived read-through view*, not a node kind (D3).** "All occurrences
  of param `X`" is computed on demand over the ordered children for binding/validation/UI
  convenience; it does not replace or reorder the underlying `Parameter` nodes. Per-element
  addressing (`geometry/dial_sdf.part[3]` = "the 4th occurrence of `part` in document order")
  resolves the index through this view to the corresponding ordered-children node (and its
  `NodeId`). Because document order is preserved, `timeline`'s order-paired `value`/`time`
  semantics and its sticky `interpolator` semantics are preserved automatically — derivation
  reads the ordered children directly.
- **A symbolic value is an `Expr` node, not a pre-evaluated number.** `position 0 expr( j*0.02-0.1 ) 0`
  retains the expression in the CST; evaluation happens in derivation (Facet 2), so the CST
  round-trips the author's intent (INV-4), not the computed float. (The v6 `$(@J*0.02-0.1)`
  spelling is migrated to this `expr(...)` form by the one-shot migrator, D8 — the v7 runtime
  CST never holds a `$(...)` node.)
- **There is no imperative command layer in v7 (D19).** v7 has **no `Command` node kind** and
  no embedded `>` directives: `> load`/`> run` are gone per D7, and the three surviving
  `> set` forms become ordinary descriptor-driven chunks (e.g. `acceleration { … }`,
  `global_medium { … }`, §2.8.4). Everything in a v7 document is a declarative chunk; the `>`
  syntax exists only as an *input to the one-shot migrator* (§2.8.3/§2.8.4).

### 2.3 How the CST maps to the descriptor schema (L6)

The CST does **not** invent a parallel schema (L6 forbids it). Each `Chunk` node references
its `ChunkDescriptor` by keyword; each `Parameter` references its `ParameterDescriptor` by
name (the derived `RepeatGroup` view, D3, reuses the same descriptor's `repeatable=true`
flag — it is not a separate schema). The descriptor is consulted for:

- **Validation** — a `Parameter` whose name isn't in the descriptor is a *recoverable* error
  node (§2.9), not a hard parse abort. This is the behavioral change from today's
  `DispatchChunkParameters`, which `return false`s the whole parse on the first unknown name.
- **Value typing** — `ParameterDescriptor::kind` (and `tupleKinds` for composites like
  `advanced_shader`'s `shaderop foo 0 5 +`) tells the CST how to split a `Value` into typed
  `ValueAtom`s and which atoms are references/enums (drives Facet 4 widget choice and Facet 5
  validation).
- **Defaults & presence** — the CST stores only what the file *says*; `bag.Has(key)` semantics
  carry over (a `Parameter` node present ⇒ explicitly set). Defaults remain a derivation-time
  concern (Facet 2 reads `defaultValueHint`/the `Defaults` structs), so the CST never
  fabricates parameter nodes the author didn't write — preserving INV-4.

Because the descriptor already feeds the parser, the syntax highlighters, and the suggestion
engine (per `README.md`), wiring it to the CST adds a *fourth consumer of the same source of
truth* rather than a new schema. The descriptor types
([`ChunkDescriptor.h`](../../src/Library/Parsers/ChunkDescriptor.h)) need **no structural
change** for this facet; everything required (`kind`, `enumValues`, `referenceCategories`,
`tupleKinds`, `repeatable`, `required`) is already present.

### 2.4 Red-green tree: relative widths in green nodes, absolute positions from a red cursor (D2, D16)

The CST is a **red-green tree** (the rust-analyzer / Roslyn persistent-syntax-tree model),
mandated by **D2**. This is the only design consistent with *both* INV-4 (lossless) and the
near-logarithmic structural-sharing cost model the rest of the redesign (Facet 2/3) depends on:
storing absolute byte offsets in immutable shared nodes is forbidden, because a single
length-changing edit would shift every later offset and force an O(document) copy, defeating
sharing.

**Wide child lists are ropes, not vectors (D16).** D2's original "O(depth)" cost claim is only
true if locating child *k* (or computing its byte offset) is O(1) — which it is **not** for a
plain vector of children, where the red cursor must scan all preceding siblings (a `Document`
with 10 000 chunks → O(N)), and a prefix-offset array would make *edits* O(N). Per **D16**, a
node's child list is therefore a **persistent balanced sequence / rope**, with each subtree
caching its **aggregate byte-width and newline counts**. This gives **O(log N)** position lookup
(byteBegin of child *k*, byte→node) **and O(log N)** structural edit (insert/remove a child),
while preserving structural sharing. The honest cost claim is **O(depth · log(width)) ≈
O(log N)**, not O(depth). Narrow lists (a chunk's handful of params) may stay plain vectors
(small N); the **`Document`'s chunk list and any large `RepeatGroup`/heightfield** (e.g. a
10 000-`part` SDF) use the rope.

**Green nodes (immutable, content-addressed, structurally shared) — and the three separated
identities (D15).** D15 mandates that three concepts the early draft conflated — content
addressing, the derivation memo key, and lineage identity — be kept **distinct**, because they
have incompatible requirements: a lossless content hash must include trivia (so a whitespace
edit changes it), a derivation key must *exclude* trivia (so a whitespace edit is a cache hit),
and a lineage id must be **per-occurrence** (so it cannot live on a green node that is shared
across many occurrences). The green node therefore **carries no `NodeId`**:

```cpp
struct GreenNode {
    CstNodeKind  kind;
    std::uint32_t width;       // RELATIVE: byte length of this node incl. its own trivia.
                               // No absolute offset is ever stored.
    bool         isSymbolic;   // OR of contained ValueAtom/Expr symbolic flags (carried from RawToken)
    // typed content (kind-specific): ValueAtom text, descriptor link, child handles, …
    // children are shared handles to other immutable green nodes (a rope for wide lists, D16).
    // NOTE: NO NodeId here — lineage identity lives in the RED layer / a side-map (D15, §2.5),
    //       because one shared green node is reused at many occurrences and cannot bear a single id.
    // NOTE: no absolute byteBegin/byteEnd, no line/col, no FileId — those are DERIVED on
    //       demand by the version-specific red cursor (below), never stored on the green node.
};
```

The three identities D15 separates are:

1. **Content hash (green, lossless, trivia-SENSITIVE)** — a hash of the green node's exact bytes
   *including* its trivia. Its sole purpose is **structural sharing / dedup**: two byte-identical
   green subtrees hash equal and are stored once. It carries **no identity**, which is precisely
   what lets identical subtrees share a single green node.
2. **Derivation key (semantic, trivia-INSENSITIVE)** — a hash of the node's *meaning* (typed
   values + child structure, **excluding** comments / whitespace / trivia) **+ the traced-input
   versions** (D4). This is **Facet 2's memo key**: a whitespace-only edit leaves the derivation
   key unchanged, so it is a derivation cache **hit** (the content hash *did* change, so the green
   node is new, but its meaning did not). This is distinct from the content hash (#1).
3. **Lineage identity (`NodeId`)** — a per-**occurrence**, immutable, process-stable token that
   lives in the **red layer / a side-map, NOT on the shared green node** (§2.5). Its purpose is
   UI binding, undo lineage, durable agent references, and rename.

A node's own `width` is intrinsic and edit-stable: it changes only when the node's *own* bytes
change. The width of an unedited node is identical across every version that shares it, which
is exactly what makes the green layer freely shareable by reference between snapshots.

**The red cursor (version-specific, cheap, not stored).** Absolute positions are **computed on
demand** by a *red cursor* that walks from a given version's root, accumulating the widths of
preceding siblings and ancestors. Because each child list is a rope caching aggregate widths
(D16), locating a child and summing the widths to its left is **O(log width)** per level, so the
cursor is **O(depth · log width) ≈ O(log N)** — *not* O(depth) with the per-level sibling scan a
plain vector would force. The red layer is a thin, throwaway overlay keyed to one version's root;
it is *never* stored in the shared green nodes. `byteBegin` for a node = sum of widths of
everything to its left in document order, computed by the cursor; `byteEnd = byteBegin + width`;
`line`/`col` for a diagnostic are derived by the cursor from the byte offset against the
document's newline table (newline counts are cached per rope subtree, D16). **There is no stored
`CstSpan` with absolute `byteBegin`/`byteEnd`, and no `FileId`** (v7 is single-file — D7, §2.8.4).
The red cursor is also where a node's per-occurrence **`NodeId` is resolved** (the side-map keyed
to this version's root, D15/§2.5) — the green node it points at carries none.

**Editing = path-copy (O(log N)), not offset-shifting.** An edit produces a *new root* by
copying only the nodes on the path from the root to the edited node, **plus the O(log width)
rope-spine rebalance at each level whose child list changed shape** (D16); all sibling subtrees
are **shared by reference** with the prior version. The edited node gets a new `width`; **no
other stored node's data changes**, so sharing is real and nothing has to be shifted. With the
rope, even inserting/removing a child in a 10 000-wide list is O(log N), not O(N). This is the
red-green discipline that D1 extends from the CST to the derived scene.

The two mapping directions the charter's pivot diagram requires both go through the red cursor:

- **byte-offset → node** (a click in the text editor / an agent's line-anchored diagnostic maps
  to the smallest enclosing node): the red cursor descends from the root, using each level's rope
  to locate in O(log width) the child whose width-interval contains the offset (O(log N) total).
- **node → byte-range** (a structured edit on `objects/sphere.material` must rewrite the right
  bytes): the red cursor accumulates preceding widths to the target node via the rope (O(log N)),
  yielding its absolute range for the splice. This replaces what `SaveEngine`'s Mode-A splice did
  via `ParameterSpan::valueBegin/valueEnd`: the save engine's bespoke absolute-offset side-table
  (and the Mode-A/Mode-B duality) collapses into "serialize the CST" (see §3).

INV-4's "don't reformat untouched text" falls out structurally: serialization walks the green
tree and emits each node's **retained trivia/text verbatim**, except an edited node whose bytes
are re-rendered. **`ApplyOffsetDeltas` is gone** — there are no stored absolute offsets to
shift; the red cursor recomputes positions for the new root in O(log N) whenever they are
needed.

### 2.5 Node identity (D9, D15): `NodeId` (lineage, in the red layer) + name-path (addressing)

**D9 refines L5 into a dual-identity model; D15 then pins *where* the lineage id lives.** L5
named "name-path" as the identity currency; D9 splits that into two distinct concepts because a
name-path *changes on rename* and shifts on insert, yet undo-lineage, UI bindings, and durable
agent references need something that does *not* move. **D15 further mandates that the lineage id
is borne by the RED layer, not the shared green node** — because a green node is content-addressed
and reused at every byte-identical occurrence, so it cannot carry one per-occurrence id (§2.4).
The two identities are:

- **`NodeId` — the lineage identity (red-layer / side-map, D15).** An immutable, process-stable
  per-**occurrence** token kept in a **side-map keyed to a version's root** (the red layer),
  **not** on the green node (§2.4). It is what undo lineage, UI widget bindings, and durable
  agent references key on. It **survives renames and value edits exactly**, and reparses on a
  **best-effort** basis (see "Reparse matching" below). It is *not* a textual address — it is
  never written to the file; it is an in-memory identity.
- **name-path — the addressing scheme.** A human/agent-readable string address built from
  RISE's name-keyed managers, which **resolves to a `NodeId` within a given version.** It is
  how a human or agent *names* a node; it **changes on rename** (by design — it is a name) and
  its positional indices reflect document order.

```
objects/strap_screw                  # the standard_object named "strap_screw"
objects/strap_screw.material         # its material parameter (a Value node)
geometry/dial_sdf.part[3].round      # 4th SDF part's `round` field (intrinsic, addressable)
materials/gold.reflectance           # a material parameter
rasterizers/main.samples
cameras/default.fov
animations/spin/timelines/3.value[2] # a keyframe value
```

**Path grammar:** `<category-segment>/<entity-name>` then a dotted/indexed param path.
`<category-segment>` derives from `ChunkCategory` (`Geometry → geometry`, `Object → objects`,
`Material → materials`, …) — a stable, descriptor-derived namespace, *not* the chunk keyword
(so `sphere_geometry` and `box_geometry` both live under `geometry/`, matching how the
managers key them and how animation's `element_type` already works). Resolving a name-path is a
lookup that returns the `NodeId` it currently addresses in the queried version.

**Rename is a `NodeId`-preserving op driven by traced `ReferenceUse` records (D14).** A rename
rewrites the name token **in place on the same `NodeId`** (the node's lineage identity is
unchanged) and rewrites all referrers. **Referrers are found from the traced reference graph, not
from `referenceCategories`.** Per D4, `referenceCategories` cannot represent dynamic references
(`timeline.element`/`.animation` are plain strings whose target category is chosen at derive time
by `element_type`), so using it to drive rename would silently leave those pointing at the old
name. Instead, derivation's dependency tracing (D4) records, for every resolved reference, a
**`ReferenceUse { sourceValueNodeId, targetNodeId }`**; **rename rewrites referrers from this
traced `ReferenceUse` set**, which captures dynamic references automatically. For references in
nodes that did *not* derive (e.g. inside an error subtree, so untraced), it falls back to the
descriptor-provided **reference resolver**; any referrer that **cannot be resolved is
surfaced/flagged, never silently renamed**. `referenceCategories` remains a **UI-picker hint
only** (D4/D14). Because UI/agent bindings key on `NodeId`, they **survive the rename
automatically**; only the *name-path* string changes. Rename is the one operation that changes a
name-path; ordinary value edits never do.

**Reparse identity is best-effort (D15).** Identity preservation across edits has two regimes. A
**structured edit preserves `NodeId` exactly** — it targets a known node, and the new green node
inherits that occurrence's id in the side-map. A **whole-region reparse** (a free-form text edit)
**matches new green nodes to prior `NodeId`s by structural position + content** (rust-analyzer-style
node reuse) — **but this is explicitly best-effort.** Identical repeated rows (e.g. two byte-equal
`part` lines) are genuinely ambiguous: position+content cannot tell which prior occurrence a new
one continues. Per D15, **unmatched durable references are INVALIDATED and flagged, not silently
remapped** — a binding/agent-ref whose occurrence cannot be re-matched is reported as broken rather
than quietly re-pointed at the wrong row. Genuinely new content gets fresh `NodeId`s.

**Addressing of anonymous / positional nodes:**

- *Unnamed chunks* (a `pinhole_camera` with no `name`) already receive an auto-name in the
  current parser (`AllocateCameraName` → `"default"`, `"default_1"`, …,
  `AsciiSceneParser.cpp:601`). The occurrence gets a `NodeId` (in the side-map, D15) from
  creation regardless; the synthesized name gives it an addressable *name-path*, and on first
  structured edit the name is materialized into the text as an explicit `name` parameter
  (author-visible, round-trip-stable). Until edited, the synthesized name lives only in the CST
  (text stays untouched — INV-4).
- *Repeatable-param occurrences* are **addressed** positionally (`part[3]` = the 4th occurrence
  of `part` in document order, D3) — but each occurrence is an ordinary `Parameter` node carrying
  its own stable `NodeId` in the side-map (D15). An *insert/delete* shifts the **name-path index**
  of later occurrences (semantically correct — the 4th part really did become the 5th), while each
  surviving occurrence keeps its `NodeId` under a structured edit (so UI bindings keyed on `NodeId`
  survive the structural change; INV-5). Under a *whole-region reparse* of identical repeated rows,
  re-matching is best-effort and unmatched ids are invalidated rather than remapped (§ reparse,
  D15).
- *Sub-value atoms* (`position`'s `y` component) are addressed by `param.atomIndex` only
  transiently for editing; they are not first-class identities (a vec3 is one `Value`).

**This subsumes the round-4 "name-reuse identity serial."** That patch existed because Model
A's live scene could not distinguish "the user renamed X to Y" from "the user deleted X and
created a new Y with X's old name," so it stapled a monotonic serial onto reused names. In
Model B the **`NodeId` *is* the lineage identity**: rename preserves it, delete-then-create
yields a fresh one, so the two cases are intrinsically distinguished without a side-channel
serial. The serial is **deleted** — `NodeId` (lineage) + name-path (addressing) +
explicit-rename-op fully replace it. (Stated here per D9; Facet 3 owns the deletion inventory.)

### 2.6 Declarative iteration (L3): replacing FOR/DEFINE

L3 splits repetition into two cases with two different mechanisms.

#### 2.6.1 Homogeneous instancing → `Generator` node

When the repeat is *truly homogeneous* (N copies of the same entity that differ only by a
parametric function of the index), it is a **declarative generator**: the instances are
**derived, not authored**. This is the `path_instances_geometry` / guilloché model promoted to
a first-class CST construct usable for *objects*, not just geometry.

Proposed concrete primitive — an **`instance_array` chunk** (a new descriptor-driven chunk,
so it gets validation/UI/suggestions for free, L6):

```
instance_array {
    name        sphere_grid
    template    spheregeom            # named geometry (or a nested inline geometry chunk)
    material    gold
    count_u     11                    # grid/linear/radial domains
    count_v     11
    # per-instance placement as a function-expression sublanguage (§2.7):
    #   i, j      = instance indices (0-based)
    #   u, v      = i/(count_u-1), j/(count_v-1)  in [0,1]
    position    expr( u*0.2 - 0.1 )  expr( v*0.2 - 0.1 )  0
    orientation expr( i*35 )  expr( j*60 )  0
}
```

Derivation (Facet 2) expands this to `count_u * count_v` engine objects, each named
`sphere_grid[i,j]` (a stable, addressable name-path under `objects/`). The CST stores the
**generator**, not the expansion — so the file stays small, the intent is explicit, and a
single edit to the placement expression re-derives the whole array (INV-3: only the array's
subgraph re-derives).

This directly replaces the canonical nested-`FOR` idiom. Before
(`scenes/Tests/Parser/loops.RISEscene:85–97`):

```
sphere_geometry { name spheregeom  radius 0.01 }

FOR I 0 10 1
  FOR J 0 10 1
    standard_object {
        name      sphere!I!J
        geometry  spheregeom
        position  $(@I*0.02-0.1) $(@J*0.02-0.1) 0
        material  gold
    }
  ENDFOR
ENDFOR
```

After:

```
sphere_geometry { name spheregeom  radius 0.01 }

instance_array {
    name      spheres
    template  spheregeom
    material  gold
    count_u   11
    count_v   11
    position  expr( i*0.02 - 0.1 )  expr( j*0.02 - 0.1 )  0
}
```

(`FOR I 0 10 1` is inclusive of 10 ⇒ 11 iterations; the migration tool, §6 of this doc /
Facet 6, computes `count = floor((end-start)/inc)+1`.) The `!I!J` name-concatenation idiom
(`sphere!I!J`) is replaced by the generator's `[i,j]` auto-naming.

For **path/radial/parametric** repeats, the existing `path_instances_geometry` and
`sweep_geometry` chunks already *are* the declarative form; under the CST they need only be
recognized as `Generator`-flavored chunks for UI/derivation purposes. `instance_array` covers
the grid/linear/transform-function case that `FOR` was used for; the three existing chunks
cover path/sweep/profile cases. The kinematic guilloché generator (an
`expression_function2d` feeding a heightfield `sdf_geometry`) is the parametric-field case —
already declarative, already covered.

#### 2.6.2 Typing-shortcut loops → desugar to explicit entities

When the author used `FOR` as a *typing shortcut* for entities that are conceptually distinct
and want to be edited separately (the `shapes.RISEscene` case: five different geometries laid
out in a row, each meant to be individually tweakable), the right move per L3 is **desugar at
author time into explicit, separately-editable entities**. There is no retained loop; the
loop was scaffolding.

The mechanism: a **one-shot expansion edit**. The agent or GUI (or the migration tool)
materializes the loop into N explicit `Chunk` nodes in the CST. Each becomes an independent,
named, individually-addressable entity. This is a *structured edit* (Facet 3), reversible via
undo, and the resulting CST has no generator — just explicit chunks. The heuristic for
"shortcut vs homogeneous" is **whether the bodies differ structurally** (different geometry
references, different materials per iteration ⇒ shortcut; identical body parameterized only by
index ⇒ homogeneous → `instance_array`). The migration tool (Facet 6) applies this heuristic;
in-session, the agent decides and the user sees the diff.

#### 2.6.3 What `DEFINE` becomes

`DEFINE LZ 0.1` / `@LZ` is a named constant reused across chunks. Two replacements, by scope:

- **Document-level named constants** become a **`let` block** (a new tiny descriptor-driven
  chunk): `let { LZ 0.1  POWER 2.3 }`. References use the expression sublanguage:
  `power expr( POWER )`, `position 0 0 expr( LZ )`. The constant is a CST node with a name-path
  (`lets/LZ`), so it is editable and a single edit re-derives all consumers (INV-3). This
  preserves the *reuse* semantics `DEFINE` provided without imperative, order-dependent macro
  state.
- **Built-in constants** (`PI`, `E`, currently seeded into the macro table at
  `AsciiSceneParser.cpp:9883`) become **reserved identifiers in the expression grammar**
  (§2.7) — `expr( 2*PI*r )` — not macros.

`UNDEF` has no equivalent and is dropped: a CST `let` binding has lexical, document-order
scope; there is no need to imperatively un-define it.

### 2.7 The function-expression sublanguage (what `hal()`/`$(...)`/arithmetic become)

`$(...)` arithmetic, the inline `sin/cos/tan/sqrt/hal` functions, and `@NAME` macro
substitution all collapse into **one** function-expression sublanguage — and RISE *already
has its implementation*: [`ExpressionEval` / `ExpressionProgram`](../../src/Library/Painters/ExpressionEval.h),
the engine behind `expression_function2d`. We reuse it; we do not write a new evaluator.

**Surface syntax in the CST:** a value may be a literal (`0.6`) or an expression wrapped in
`expr( … )`. An `expr(...)` value parses to an `Expr` AST node (operators, calls, identifiers,
literals) retained in the CST. Grammar:

- **Operators:** `+ - * / %`, parentheses, unary `-`. (Same as `MathExpressionEvaluator` today.)
- **Functions:** the `ExpressionProgram` set (`sin cos tan sqrt clamp hypot exp pow min max
  abs floor …` — whatever `expression_function2d` already supports). `hal(d)` — the Halton
  low-discrepancy generator (`AsciiSceneParser.cpp:256`) — becomes a **function in this set**
  if any scene needs deterministic quasi-random placement; it is wired as `halton(dim, index)`
  with an explicit index argument rather than relying on hidden global sequence state
  (`static MultiHalton mh` at `:159`). **Open point** (§4): `hal()`'s current statefulness
  violates INV-2 (determinism depends on call order) — the redesign must make it pure by
  passing the index explicitly. Most scenes don't use it; the two that do
  (`painters.RISEscene`, `diamond_teapot_pour.RISEscene`) can migrate to the explicit form.
- **Identifiers:** instance variables in a generator context (`i`, `j`, `u`, `v`),
  `let`-bound constants (`LZ`, `POWER`), built-ins (`PI`, `E`), and **name-path references to
  other parameters** (a future extension — `expr( materials/gold.scale * 2 )` — flagged as
  out-of-first-slice but enabled by name-path identity).

**Where expressions are legal:** any `ValueKind::Double` / `DoubleVec3` / `UInt` atom (the
kinds today guarded by `AllTokensAreFiniteNumbers`, `:658`). String/enum/reference/filename
atoms are not expression-valued. The `AllTokensAreFiniteNumbers` text-layer guard (which
rejects `nan`/`inf` spellings before `-ffast-math` can fold them, per the guilloché-dial
inf-seed miscompile lesson) **moves to expression-result validation**: an `expr(...)` that
evaluates non-finite is a derivation error, and literal `nan`/`inf` tokens stay rejected at
parse.

**Determinism (INV-2):** with `hal()` made index-explicit and `let`/instance-var scoping
lexical, expression evaluation is a pure function of (CST node, instance index, bound
constants). No `in.seekg` re-reads, no in-place token rewriting, no global macro table
mutated mid-parse.

### 2.8 The parser evolution: one-way → retained CST

#### 2.8.1 New control flow

```
ParseToCst(bytes) -> Cst, Diagnostics                    (this facet; single-file, D7)
DeriveScene(Cst) -> Scene, Diagnostics                   (Facet 2; calls the surviving
                                                           Job::Add* "apply layer")
SerializeCst(Cst) -> bytes                                (this facet)
```

`ParseToCst` is **lex → parse → bind-to-descriptor**, producing a tree, never touching `IJob`:

1. **Lex** (reuse `RawTokenCapture`'s lexer, promoted from save-side to front-line): tokens
   with relative widths, `$`-symbolic flags, quoted-string and `expr(...)` balancing,
   trailing-comment ranges. Comments and blanks become `Comment`/`Blank`/`Trivia` tokens
   rather than being stripped. (The lexer also balances the v6 `$(...)` spelling — but only the
   one-shot v6→v7 migrator consumes that; the v7 runtime CST holds `expr(...)`, §2.8.3.)
2. **Parse structure**: recognize header, chunks (`keyword` + `{` … `}`) — including the
   `acceleration` / `global_medium` / … chunks that replace the former `> set` forms (D19,
   §2.8.4) — `let` blocks, and `instance_array`/generator chunks. Build the document-ordered
   green tree (relative widths, D2; rope-backed child lists, D16). v7 has **no** `FOR`/`DEFINE`/
   macro node kinds **and no `Command`/`>` node kind** — those constructs exist only as inputs to
   the migrator (§2.8.3/§2.8.4, D8/D19).
3. **Bind to descriptors**: for each `Chunk`, look up its `ChunkDescriptor`; for each
   parameter line (including each repeatable occurrence, kept as an ordinary ordered
   `Parameter`, D3), look up its `ParameterDescriptor` and type the value into
   `ValueAtom`s/`Expr`. Unknown chunk keyword / unknown parameter / type mismatch produce
   **error nodes** (§2.9), not a parse abort.

The descriptor-driven binding step is the direct successor to `DispatchChunkParameters` +
`Finalize`, **minus** the `pJob.AddX` emission. `Finalize`'s knowledge ("how to turn typed
values into engine objects") moves wholesale into Facet 2's derivation, where it calls the
same `Job::Add*` apply layer — so the engine-construction code is *reused*, not rewritten
(charter row 2 "apply-layer reuse"). The split is clean: `Describe()` → CST shape (here);
`Finalize` body → derivation (Facet 2).

#### 2.8.2 `Describe()` → CST → derive, end to end

Worked example for `sphere_geometry { name s  radius 0.6  # main ball }`:

1. `Describe()` (unchanged) advertises params `name:String`, `radius:Double`.
2. **CST**: a green `Chunk(keyword=sphere_geometry)` (addressed by name-path `geometry/s`, with
   a `NodeId` in the side-map, D15) with ordered children `Parameter(name, Value[ValueAtom "s"])`,
   `Parameter(radius, Value[ValueAtom 0.6], trailing Comment "# main ball")`, each green node
   carrying its relative width (and each occurrence a side-map `NodeId`, not a green-borne one).
3. **Derive** (Facet 2): reads the CST `Chunk`, fills a `ParseStateBag`-equivalent (or calls
   the relocated `Finalize` logic directly), emits `pJob.AddSphereGeometry("s", 0.6)`.
4. **Round-trip**: `SerializeCst` walks the green tree and re-emits byte-for-byte (untouched
   nodes verbatim, including the comment). A structured edit `geometry/s.radius = 0.8`
   path-copies to a new root and re-renders only the `radius` `Value` node → `radius 0.8
   # main ball` (comment preserved — INV-4; the red cursor supplies the splice range, §2.4).

#### 2.8.3 v6 is time-bounded; there are NO legacy nodes in the v7 runtime (D8)

**D8 (enabled by the owner's migrate-everything permission) removes the legacy-node design
entirely.** There is no transition window in which the runtime CST carries
`FOR`/`DEFINE`/`$(...)`/`@`/`%` constructs, and there is no legacy-expansion derivation
pre-pass. The earlier `LegacyForLoop`/`LegacyMacroDef`/`LegacyExprValue`/`LegacyMacroRef` node
kinds are **deleted from this design** — the v7 runtime never derives a v6 construct.

Instead:

- The v6 parser + macro preprocessor are kept **only long enough to migrate the corpus** with
  the one-shot **v6→v7 migrator** (§6 of this doc / Facet 6). `FOR`/`DEFINE`/`$(...)`/`@`/`%`/
  `hal` exist **only as inputs to that migrator**, which emits `instance_array` / `let` /
  `expr(...)` / `halton(dim, idx)`.
- The migrator is a build/CI gate over `scenes/` with render-equivalence acceptance (the D5/D10
  gates). **Once it is green across the corpus, the v6 reader is DELETED.** After migration,
  **v7 is the sole runtime format** — same CST, same derivation, no dual parser, no
  legacy-derivation path.

This is a large simplification over the original §2.8.3: no legacy-node machinery, no second
derivation path, no "coexist indefinitely" carrying cost. v6 files are not loaded by the
runtime after migration; they are *converted once* and the v6 path retired.

#### 2.8.4 v7 is single-file and has NO embedded command layer (D7, D19)

**D7 makes a v7 document a single self-contained file; D19 removes the last imperative `>`
command from it.** The entire multi-file CST design — child sub-documents, a `FileId` on every
node, the document-graph evaluation, nested-scope rules for included files — is **removed from
v7** (D7). And the three surviving `> set` forms (accelerator / global-medium / …) become
**ordinary descriptor-driven chunks** (D19), so **v7 has no `> ` command syntax and no `Command`
node kind at all** — every directive is a declarative chunk.

- **`> load` and `> run` are deprecated and removed from v7 (D7).** The one-shot v6→v7 migrator
  (§6 / Facet 6) **flattens** every include/run by inlining the referenced content into the
  consuming document — e.g. the shared `standard_colors` painters (`> run
  scenes/colors.RISEscript` → `load scenes/standard_colors.RISEscene`) are inlined where used.
- **`> set` becomes declarative chunks (D19).** The migrator rewrites each `> set` form into a
  normal chunk — e.g. `> set accelerator B 10 8` → an `acceleration { … }` chunk; the global
  participating-medium `> set` → a `global_medium { … }` chunk — each descriptor-driven (so it
  gets validation / UI / suggestions for free, L6) and a first-class declarative **derivation
  input** (Facet 2 reads it as graph state, not as a side-effecting command). v7 therefore has
  **no imperative command layer**; `> set` exists only as an *input to the migrator*.
- This **dissolves** the whole multi-file problem set *and* the imperative-command layer: version
  identity is per single root; undo is single-rooted; save is one file (atomic save, D17); there are no
  include cycles, no shared-include fan-out, no cross-file edit semantics; and all configuration is
  declarative graph input. (The accidental `> load` thread-local reset of
  `scene_options`/`camera_defaults` noted in the parser README simply ceases to exist — there is
  no second document to reset against.)
- **Cost accepted:** some duplication of previously-shared content across scenes; the owner
  controls the corpus and prefers the simplification (duplication in a migrated corpus is cheap
  and diff-visible).

> **`FileId` / multi-file reading survives ONLY inside the one-shot v6→v7 migrator** (it must
> read the v6 include graph to flatten it). It is not part of the v7 runtime CST: green nodes
> carry no `FileId` (§2.4), and `ParseToCst` takes a single byte buffer (§2.8.1).

**Future option (explicitly OUT of core v7, per D7):** if library-sharing demand returns, it
would be a *declarative `import` chunk* with explicit lexical scoping, an AssetManifest-style
fingerprint (D5), and a defined versioning/undo story — designed as its own feature, never the
imperative thread-local-resetting `> load`. This is out of scope for this facet and the v7
core.

#### 2.8.5 Incrementality (INV-3 handoff)

A localized text edit reparses only the touched chunk's byte range into a sub-tree, which is
path-copied into a new root (the lexer is line/brace-oriented, so a chunk's bounds are
recoverable cheaply); a structured edit path-copies to the target node directly (§2.4). Either
way the changed subtree is handed to Facet 2's incremental derivation, and unchanged green
subtrees keep their **derivation key** (trivia-insensitive, D15) so Facet 2's memo sees them as
cache hits, while their prior `NodeId`s are re-matched **best-effort** (reparse matching, §2.5;
unmatched durable refs are invalidated, not remapped). This facet's contract to Facet 2: **a CST
edit reports the set of changed nodes (by `NodeId` where matched, and the set of invalidated
ids)** so derivation can recompute only the affected subgraph. There is **no offset-delta
shifting** — the red-green model needs none (D2): the new root shares every untouched green
node by reference (via the rope, D16), and absolute positions are recomputed on demand by the
red cursor.

### 2.9 Parse errors localize to CST nodes (feeds the agent surface, Facet 5)

Today an unknown parameter aborts the entire parse with a single log line
(`DispatchChunkParameters` → `return false`). For the agent surface this is hostile: one typo
nukes the whole derivation with no structured location. The CST changes this to
**error-recovering parse with node-local diagnostics**:

- Each diagnostic is `{ severity, NodeId, code, message, fix-hint }`, anchored to a node by
  its stable `NodeId` (D9); the byte range / line+col for surfacing it is derived on demand by
  the red cursor (§2.4), not stored on the diagnostic. Unknown parameter, unknown chunk
  keyword, type mismatch, missing-required, unresolved reference, bad expression — each
  attaches to the smallest enclosing node and **does not stop** parsing the rest of the file.
- An error node still round-trips its bytes verbatim (so a malformed file is never corrupted
  by load+save) and simply doesn't contribute to derivation (or contributes a clearly-marked
  invalid entity).
- The descriptor powers fix-hints directly: unknown parameter → nearest-name suggestion from
  `Describe().parameters`; bad reference → list candidates from `referenceCategories`; bad
  enum → list `enumValues`. This is the same data the suggestion engine already uses, now
  surfaced as structured diagnostics for both the GUI gutter (Facet 4) and the MCP error
  channel (Facet 5: "validate → derive → render, structured errors").

### 2.10 Coverage: does this cover everything?

**All ~138 chunk keywords** (the README's count; the Explore survey enumerates them across
Painters, Functions, Materials, scene-config, Cameras, Geometry, Modifiers, Media, Objects,
ShaderOps, Shaders, Rasterizers, Output, Lights, PhotonMaps/Gather, IrradianceCache,
Animation) are `Chunk` nodes — they are *already* uniform `Describe()`/`Finalize()` pairs, so
the CST treats them identically. No chunk needs a bespoke node kind; the variation is entirely
in the descriptor (which the CST references, not duplicates). New chunks added per the
README's "Adding A New Chunk Parser" recipe get CST/derivation/UI support **for free** — the
descriptor remains the single registration point.

**Repeatable sub-elements** (`cp` on `spectral_painter`/`piecewise_linear_function`; `gen` on
`voronoi{2,3}d_painter`; `part` on `sdf_geometry`; `profile_point`/`point`/`point_width` on
`sweep_geometry`; `point` on `path_instances_geometry`; `param`/`def` on
`expression_function2d`; `shaderop` on `standard_shader`/`advanced_shader`;
`time`/`value`/`interpolator` on `timeline`) → ordinary **document-ordered `Parameter`
nodes** (D3); a derived `RepeatGroup` **view** provides per-occurrence addressing
(`…part[3]` = the 4th occurrence in document order). `timeline`'s order-paired `value`/`time`
and sticky `interpolator` are preserved because document order is canonical. A **large**
repeat group (e.g. a 10 000-`part` SDF heightfield) holds its occurrences in a **rope-backed
child list** (D16) so addressing the *k*-th and inserting/removing occurrences stay O(log N).

**Composite/tuple values** (`advanced_shader`'s `shaderop foo 0 5 +`, the
`tupleKinds`-described values; `homogeneous_medium`'s `phase hg 0.5`) → a `Value` with
multiple typed `ValueAtom`s, typed per `ParameterDescriptor::tupleKinds`. The descriptor
already models these; the CST reads the model.

**The macro constructs** (`FOR`/`ENDFOR`, `DEFINE`/`UNDEF`/`!`/`~`, `@`/`%`, `$(...)`,
`sin/cos/tan/sqrt/hal`) → covered by §2.6 (`instance_array` + desugar), §2.6.3 (`let` +
reserved built-ins), and §2.7 (`expr(...)` sublanguage). These have **no v7 runtime
representation**: the one-shot v6→v7 migrator converts them to the declarative forms, then the
v6 reader is deleted (D8, §2.8.3). There are no legacy nodes.

**Embedded commands** — **none survive in v7 (D19).** `> load` / `> run` are deprecated and
**flattened by the migrator** (single-file v7, D7); the three `> set` forms are migrated to
**declarative chunks** (`acceleration { … }`, `global_medium { … }`, …). v7 has no `> ` syntax
and no `Command` node kind; all are migrator-only inputs (§2.8.4).

#### What resists — opaque assets (reference + AssetManifest, D5)

The CST is a faithful tree of the **scene text**. It is *not* a container for binary assets the
text *points at*. These are handled **by reference _and_ by an AssetManifest entry** (D5), not
by inlining their bytes:

- **Mesh files** — `3dsmesh/rawmesh/rawmesh2/risemesh/plymesh/gltfmesh_geometry`,
  `gltf_import`, `bezierpatch/bilinearpatch_geometry` (all via a `file` param,
  `ValueKind::Filename`).
- **Texture/image files** — `png/jpg/hdr/exr/tiff_painter` `file`.
- **Data files** — `spectral_painter` `file`, `piecewise_linear_function` `file`,
  `datadriven_material` `filename` (MERL BRDF), `voronoi{2,3}d_painter` `file`,
  `sdf_geometry` `file` (the large-SDF sidecar form).
- **Output paths** — `file_rasterizeroutput` `pattern` (a *sink*, see below).

For these, the CST holds the **filename `ValueAtom`** — fully editable as text/structured (you
can repoint a texture path), and its absolute position comes from the red cursor on demand
(§2.4), not a stored span. **Per D5, the formal derivation input is `(CST, AssetManifest)`:**
each referenced asset path also has an **`AssetManifest`** entry mapping it to a
**{resolved absolute identity, content fingerprint (size+mtime, upgradable to a content hash)}**.
The manifest is a **first-class derivation input** — asset reads are traced (Facet 2 / D4), so a
fingerprint participates in the memo keys of the nodes that consumed it; a texture changing on
disk (same path) invalidates exactly its consumers. This closes the `Scene = f(CST)` hole where
a changed-on-disk asset would otherwise let a clean derive disagree with a cache hit.

- **Output paths are explicitly EXCLUDED** from the input dependency set (D5):
  `file_rasterizeroutput.pattern` and any sink are *outputs*, not sources, so they get no
  AssetManifest entry and never invalidate derivation.

Implication for Facet 5/6: a scene is diff-able and git-native *for its text*; binary assets
are referenced artifacts tracked by manifest identity+fingerprint (the same way source code
references images it doesn't inline, with a lockfile-style record of what each path resolved
to). The `gltf_import` chunk is the notable case where an *opaque asset expands into many engine
entities* — the CST keeps it as a single `Chunk` (one import directive, one AssetManifest
entry); the expanded sub-scene is a derivation product, not CST content. (This mirrors
`instance_array`: the CST holds the directive, derivation holds the expansion.)

---

## 3. Delete / Evolve / Reuse

| Component (current) | Fate | Notes |
|---|---|---|
| [`RawTokenCapture`](../../src/Library/Parsers/RawTokenCapture.h) (lexer + token widths + symbolic flags) | **Evolve → reuse** | Promote from save-side Phase 0 to the front-line CST lexer; it produces green-node content (relative widths, not absolute spans). Lexing rules (quoted strings, `expr(...)` balancing, comment ranges) are exactly what the CST needs. Add comment/blank emission instead of stripping. (The `$(...)` balancing it already does is consumed only by the v6→v7 migrator, D8.) |
| [`SourceSpanIndex`](../../src/Library/SceneEditor/SourceSpanIndex.h) (per-entity/param **absolute** byte ranges, `AuthorMode`, `ApplyOffsetDeltas`) | **Delete (replaced by the red cursor, D2/D16)** | D2 forbids stored absolute offsets in shared nodes. Green nodes store **relative widths only** (the per-occurrence `NodeId` lives in the red-layer side-map, **not** on the green node — D15); absolute positions are computed on demand by the version-specific red cursor over the rope-cached widths (§2.4, O(log N), D16). The standalone absolute-offset side-table and **`ApplyOffsetDeltas`** (offset-shift-on-splice) are **deleted**, not evolved — there is nothing to shift. `chunkRevisited`/FOR-dedup logic is gone with v6 (no legacy nodes, D8). `insideManagedBlock` becomes obsolete (no managed override block — see SaveEngine row). `AuthorMode` (Euler/Quaternion/Matrix precedence) is retained as green-node content where needed. |
| Descriptor schema ([`ChunkDescriptor.h`](../../src/Library/Parsers/ChunkDescriptor.h), `IAsciiChunkParser::Describe()`) | **Reuse verbatim** | L6. The CST references it; no structural change required for this facet. It gains a fourth consumer (CST binding) alongside parser/highlighter/suggestions. |
| `DispatchChunkParameters` / generic `ParseChunk` (`AsciiSceneParser.cpp:697`, `:9861`) | **Evolve** | Becomes the descriptor-binding step of `ParseToCst` (validate names → typed `ValueAtom`s), **minus** the abort-on-error behavior (→ error nodes, §2.9) and **minus** `Finalize` invocation. |
| `IAsciiChunkParser::Finalize` bodies (the `pJob.AddX` emission) | **Evolve → move to derivation (Facet 2)** | The "typed values → engine objects" logic relocates into Facet 2, which calls the **same `Job::Add*` apply layer**. The apply layer (`Job.cpp`, `RISE_API`) is **reused unchanged**. |
| Top-level preprocessing (`FOR`/`ENDFOR` seek-back, `substitute_macro`, `evaluate_expression`, `evaluate_first_function_in_expression`, the `static MultiHalton mh` global, `macros`/`loops` parser state) | **Delete after migration (lives only in the one-shot migrator, D8)** | There is **no legacy derivation pre-pass** and no runtime legacy nodes (D8, §2.8.3). The expansion logic is used *only* by the v6→v7 migrator to convert `FOR`→`instance_array`, `DEFINE`→`let`, `$(...)`/`@`/`%`→`expr(...)`/`let`-refs, and `hal`→index-explicit `halton(dim, idx)` (INV-2). Once the corpus is migrated and the gate is green, this whole subsystem (and the v6 reader) is **deleted**; v7 is the sole runtime format. |
| Embedded `> set` command parsing (accelerator / global-medium / …) | **Delete from v7; migrate to declarative chunks (D19)** | v7 has **no `Command` node kind and no `> ` syntax** (D19). The migrator rewrites each `> set` form into a normal descriptor-driven chunk (`acceleration { … }`, `global_medium { … }`, …) — a declarative derivation input, not an imperative command. `> set` parsing survives only inside the one-shot v6→v7 migrator (alongside `> load`/`> run` flattening, D7). |
| `AllocateCameraName` auto-naming (`:601`) | **Evolve → CST synthesized identity** (§2.5) | Generalized to all unnamed chunks; synthesized name (with its side-map `NodeId`, D15) persisted in CST, materialized to text on first edit. |
| [`SaveEngine`](../../src/Library/SceneEditor/SaveEngine.cpp) Mode-A/Mode-B byte-splice + managed-override-block machinery | **Delete (this facet's contribution to the deletion)** | Replaced by `SerializeCst(Cst)` — pretty-printing the canonical tree, verbatim for untouched nodes, re-rendered for edited nodes. The whole Mode-A-vs-Mode-B duality, the sentinel-bracketed managed block, `OverrideSpanIndex`, `override_object`, the load-time `FileIdentity` external-mod guard's *splice* rationale, and `loadedPropertyValues` diffing exist **only** because the text wasn't retained. With a retained CST they vanish. **Note (D6):** the external-mod guard's *intent* is **not** deleted — it is retained as the load/flush content-fingerprint + compare-and-swap save contract (Facet 3 owns it); only its byte-*splice* mechanism goes away here. (Facet 3/6 own the full SaveEngine deletion inventory; this facet supplies the replacement: CST serialization.) |
| `tools/migrate_scenes_v5_to_v6.py` (and the v5→v6 hard-fail message) | **Reuse pattern** | The v6→v7 migrator (§6) follows this established idempotent-script pattern. |

**Net:** the parser stops being a fire-and-forget emitter and becomes a faithful `bytes ⇄
CST` codec; the descriptor stays the schema; the engine-construction code is reused via Facet
2; the byte-splice save machinery is deleted because retention makes it unnecessary.

---

## 4. Hard problems & open questions

1. **Lossless preservation is a tar-pit (charter risk).** "A hand-edited file survives
   parse→serialize unchanged" (INV-4) is easy to claim and hard to guarantee across *every*
   whitespace/comment/ordering oddity (tabs vs spaces — RISE uses hard tabs; trailing
   whitespace; CRLF; inline `# mm` comments glued to numbers like `36#mm` that the legacy
   `sscanf` tolerated; `/* */` blocks spanning chunk boundaries). **Mitigation:** trivia is a
   first-class node carrying raw bytes; the serializer emits untouched nodes byte-for-byte and
   only re-renders edited nodes. **Acceptance test:** a golden corpus (all ~scenes under
   `scenes/`) must satisfy `parse→serialize == identity` byte-for-byte before any structured
   edit. This is the single most important correctness gate for the facet, and the place a
   "looks done" implementation will actually be broken.

2. **`hal()` statefulness vs INV-2.** The current `hal(d)` draws from a process-global
   `MultiHalton` whose result depends on call count/order — non-deterministic under the "Scene
   = f(CST)" mandate. **Proposed:** redefine as pure `halton(dim, index)` with explicit index.
   **Open:** do the two scenes using it (`painters.RISEscene`,
   `diamond_teapot_pour.RISEscene`) rely on the *implicit sequencing* such that an explicit
   index changes their output? Needs a render-diff check during migration. Low blast radius
   but a genuine semantic change.

3. **Generator expressiveness vs. authoring escape hatch.** `instance_array` + the four
   existing generator chunks cover grid/linear/path/sweep/field repetition. **Open:** is there
   a class of `FOR`-expressible repetition that *neither* a declarative generator *nor*
   reasonable desugaring handles well (e.g. data-driven counts, nested heterogeneous loops,
   loops whose body references the previous iteration's output)? The `shapes.RISEscene` /
   `loops.RISEscene` corpus says no for the existing scenes, but the agentic thesis invites
   users to push harder. If such a class exists, the answer is *desugar to explicit entities*
   (always possible, just verbose) — but we should confirm no scene needs a retained
   imperative loop, or we under-deliver on "the 3D package for nerds."

4. **Incremental reparse boundary precision.** §2.8.5 claims a localized text edit reparses
   only the touched chunk. Chunk bounds are brace-delimited and recoverable, but an edit that
   *adds/removes a brace* or breaks chunk structure forces a wider reparse. **Open:** the exact
   reparse-widening policy (and its latency under O2's debounced-commit cadence) is shared with
   Facet 2/3; this facet must publish the "changed-NodeId set" contract precisely enough for
   Facet 2's memoization to be sound.

5. **Reference integrity under rename (D9/D14, refining L5).** A rename is a `NodeId`-preserving
   op (D9, §2.5): the renamed node keeps its `NodeId`, only its name-path string changes, and all
   reference *values* pointing at the old name are rewritten. Per **D14**, referrers are found from
   the **traced `ReferenceUse { sourceValueNodeId, targetNodeId }`** records recorded during
   derivation (which capture dynamic refs like `timeline.element` resolved via `element_type`),
   **not** from `referenceCategories` (a UI-picker hint only); untraced refs fall back to the
   descriptor reference resolver, and any unresolvable referrer is flagged, never silently renamed.
   **Open:** references inside `expr(...)` (the future `expr( materials/gold.scale )` extension)
   and references in *opaque assets* (a glTF that names a RISE material?) complicate "find all
   referrers" even with tracing — an asset-internal reference is never traced as a `ReferenceUse`.
   First slice scopes renames to direct `ValueKind::Reference` atoms captured by tracing;
   cross-expression/cross-asset reference rewriting is deferred and flagged.

6. **`expr(...)` vs `$(...)` surface-syntax bikeshed.** I chose `expr( … )` to (a) visually
   distinguish a retained expression from a literal, (b) avoid the `$`/`@`/`%` sigil zoo, and
   (c) match `expression_function2d`'s mental model. **Open for synthesis:** is reusing the
   *exact* `$(...)` spelling (and just *retaining* it instead of eagerly evaluating) lower-
   friction for the existing corpus and muscle memory? That would shrink the migration to "do
   nothing for expression syntax; only convert FOR/DEFINE." Trade-off: `$(...)` has no obvious
   slot for instance variables / name-path refs, and its current evaluator is a separate code
   path from `ExpressionProgram`. I lean `expr(...)` + `ExpressionProgram` unification, but
   this is the most defensible thing to reverse.

---

## 5. Cross-facet dependencies & assumptions

- **→ Facet 2 (Derivation).** I assume Facet 2 hosts the relocated `Finalize` logic and calls
  the **unchanged `Job::Add*` apply layer**, and that it consumes (a) the CST tree, (b) the
  "changed-NodeId set" from edits, (c) the descriptor for defaults, (d) the **AssetManifest**
  (D5) as a co-input, recording traced asset reads into memo keys. I assume *I* own
  expression/generator **parsing into AST/generator nodes**; *Facet 2* owns their
  **evaluation/expansion** (where instance vars and `let` constants are bound). There are **no
  v6 legacy nodes** for Facet 2 to expand (D8) — v6 constructs are converted once by the
  migrator, never derived at runtime. I also assume Facet 2 records dependency edges **by
  tracing during derivation** (emitting the `ReferenceUse` records D14's rename consumes) and
  keys its memo on the **derivation key = (the node's trivia-insensitive semantic hash +
  traced-input versions)** (D4/D15) — distinct from the green node's lossless content hash — so
  `expr(A)` and a literal with A's current value never collapse, and a whitespace-only edit is a
  cache hit. If Facet 2 would rather the CST pre-expand generators, that conflicts with INV-3/INV-4
  (expansion in the CST bloats it and loses authoring intent) — flag for synthesis.
- **→ Facet 3 (Edit model).** I assume structured edits are *operations on CST nodes* addressed
  by name-path but **identified by `NodeId`** (D9/D15, the id living in the red-layer side-map),
  that **rename is a distinct, `NodeId`-preserving edit op** (not a value edit) that rewrites
  referrers from the traced `ReferenceUse` set (D14), and that Facet 3 owns undo/redo as CST
  version history (subsuming the round-4 identity-serial — D9 makes `NodeId` the lineage
  identity that replaces the serial; Facet 3 deletes the serial). I provide: red-cursor
  byte↔node mapping (D2/D16, no stored spans, O(log N)), the changed-/invalidated-NodeId contract
  (structured edits preserve ids exactly, whole-region reparse re-matches best-effort and
  invalidates the unmatched — D15), and error-node round-tripping. The one-shot FOR-desugar
  (§2.6.2) is a Facet-3 edit that I define the *shape* of. I also assume Facet 3 owns the
  load/flush-fingerprint + atomic save contract (temp-write → fsync → revalidate content-hash →
  atomic rename, **D17**, superseding D6's TOCTOU-prone CAS) — the external-mod *intent* of the
  deleted `FileIdentity` splice guard.
- **→ Facet 4 (Dynamic UI).** I assume the UI binds widgets to CST nodes by **`NodeId`**
  (the red-layer/side-map lineage id, addressed by name-path, D9/D15), so bindings survive rename
  and value edits exactly and survive reparse on a best-effort basis (a binding whose occurrence
  cannot be re-matched is flagged broken, not silently re-pointed — D15), and chooses widget type
  from `ParameterDescriptor::kind`/`enumValues`/`referenceCategories`/`presets`/`unitLabel` (all
  already present — no descriptor change needed from me). Opaque-asset references render
  read-only-ish per existing convention. (There are no legacy nodes to render — D8; no `Command`
  nodes either — D19.)
- **→ Facet 5 (Agentic surface).** I provide the structured-diagnostic shape
  (`{severity, NodeId, code, message, fix-hint}`, anchored by `NodeId` with position resolved
  by the red cursor — D2, §2.9) the MCP error channel needs, and the name-path addressing
  scheme (resolving to `NodeId`, D9) agents use to read/patch nodes. Diff-ability/git-nativeness
  rests on INV-4 holding (my §4 gate) and on single-file v7 (D7).
- **→ Facet 6 (Migration).** I define the **one-shot, time-bounded** v6→v7 migration (D8): v6
  constructs are converted to the *target* forms (`instance_array`/`let`/`expr`/`halton`),
  `> load`/`> run` includes are **flattened** into single-file v7 (D7), and the three `> set`
  forms are rewritten to **declarative chunks** (`acceleration { … }` / `global_medium { … }`,
  D19); after the corpus is migrated and the gate is green, the v6 reader is **deleted** (no
  indefinite coexistence). Facet 6 owns the corpus migration tool and risk register. The migrator
  follows the `migrate_scenes_v5_to_v6.py` pattern.

**No conflicts with Locked decisions or the decisions (D1–D20).** I design *to* L1–L7
and to D1–D20. Open-decision stances: O1 — I design for lossless-CST-pivot (the working
assumption); where text-canonical (O1 alt) would differ, it's only that "trivia nodes" become
"the buffer is truth and the tree is a parsed view" — the red-green model (D2) supports either,
but I assume CST-canonical so structured edits are first-class. O3 — resolved per D7/D8: bump to
single-file v7, migrate the v6 corpus once with the migrator, then delete the v6 path (no
indefinite coexistence).

---

## 6. First-slice implications & format version (O3)

### 6.1 Format version bump

Bump the header to `RISE ASCII SCENE 7`. **Migrate-then-delete stance (D7 + D8 — NOT
indefinite coexistence):**

- **v6 is time-bounded.** The v6 parser + macro preprocessor are kept **only long enough to
  migrate the corpus** with the one-shot migrator, then **deleted** (D8). v7 becomes the **sole
  runtime format**; the runtime never derives a v6 construct and there are **no legacy nodes**.
  (A v6 file that uses no macro constructs and no includes — e.g. `watch_dial.RISEscene` — is
  essentially already v7-shaped, so its migration is a no-op header bump.)
- **v7 is single-file and the authoring target** (D7): no `FOR`/`DEFINE`/macro-substitution/
  `$(...)`, **no `> load`/`> run`** — includes are flattened into the one file — and **no
  embedded `>` command layer at all** (D19): the three `> set` forms become declarative chunks
  (`acceleration { … }`, `global_medium { … }`, …). Repetition via `instance_array` + the
  existing generator chunks; constants via `let`; expressions via `expr(...)`. The v7 grammar
  drops the imperative preprocessing layer, the imperative command layer, and the multi-file
  layer, plus the small additions (`instance_array`, `let`, `expr(...)` value-syntax, and the
  former-`> set` chunks) — all descriptor-driven (L6).
- **Migration tool** `tools/migrate_scenes_v6_to_v7.py` (Facet 6 owns it; this facet specifies
  behavior): idempotent; converts FOR→`instance_array` (homogeneous) or explicit-entity
  expansion (heterogeneous, §2.6.2); DEFINE→`let`; `$(...)`/`@`/`%`→`expr(...)`/`let`-refs;
  `hal()`→`halton(dim,idx)`; **flattens `> load`/`> run` includes** by inlining (D7); and
  **rewrites `> set` forms to declarative chunks** (D19) — all with render-equivalence
  verification (the D5/D10 gates). It also reads the v6 include graph (the **only** place
  `FileId`/multi-file reading survives, D7). Mirrors the `migrate_scenes_v5_to_v6.py` idiom
  (idempotent, in-place, with a hard-fail message on un-migratable constructs for hand review).
  The migrator is a **build/CI gate over `scenes/`**; once green across the corpus, the v6 reader
  is dropped — migration is the one-time path to the single runtime format, not an opt-in
  convenience.

### 6.2 First-slice (one chunk type, full vertical)

To prove the pivot end-to-end with minimal surface, the first slice is **`sphere_geometry`**
(2 params, no references, no repeatables, no expressions — the simplest non-trivial chunk):

1. **bytes → CST**: `ParseToCst` on a one-`sphere_geometry`-chunk file produces a `Document`
   → `Chunk` (addressed `geometry/s`) → two `Parameter` green nodes (each with a relative width;
   each occurrence's `NodeId` in the red-layer side-map, D2/D9/D15) + any trailing comment,
   reusing the `RawTokenCapture` lexer.
2. **CST → text (INV-4)**: `SerializeCst` reproduces the file byte-for-byte. **Gate:** the
   parse→serialize identity test passes on a hand-formatted sphere chunk (tabs, comment,
   blank line).
3. **CST → derived scene**: a minimal derivation (Facet 2 collaboration) reads the `Chunk`
   and calls the existing `pJob.AddSphereGeometry(name, radius)` apply layer — proving the
   `Finalize`-logic relocation against unchanged engine code.
4. **One schema-generated widget (Facet 4)**: a `radius` number field bound to
   `geometry/s.radius` via name-path + `ParameterDescriptor::kind == Double`.
5. **Live incremental re-derive (INV-3)**: editing the widget (or the text) mutates the one
   `Value` node; the changed-NodeId set `{geometry/s.radius}` triggers re-derivation of only
   that geometry. Round-trip back to text shows the comment preserved.

This slice exercises every load-bearing claim — retained lossless CST, red-green two-way
mapping (relative widths + rope-backed O(log N) red cursor, D2/D16), descriptor-driven binding,
the three separated identities (content hash / derivation key / red-layer `NodeId`) + name-path
(D9/D15), apply-layer reuse, incremental re-derive — on the smallest possible chunk, with the
parse→serialize identity test as the non-negotiable correctness gate. Subsequent slices add: a
chunk with a `Reference` (`standard_object.material` → rename integrity driven by traced
`ReferenceUse`, D9/D14), a chunk with repeated `Parameter` occurrences exposed via the derived
`RepeatGroup` view (`sdf_geometry.part[]`, D3), the `expr(...)` sublanguage (one `Double`
param, with traced-input invalidation, D4), and finally `instance_array` (replacing
`loops.RISEscene`'s nested `FOR`).

> This phased first slice and its gates are the canonical **D10** fixture/gate set, **as
> amended by D18** (the corrected fixture: the first reference arrives via a
> `lambertian_material` in phase 2, the geometry→material→object chain is complete in phase 3,
> and an `image_painter`/mesh asset phase makes G5 exercisable); the phases and gates G1–G5 are
> defined authoritatively in [`01-DECISIONS.md`](01-DECISIONS.md) §D10/§D18 — the steps above
> are this facet's contribution to that shared vertical.
