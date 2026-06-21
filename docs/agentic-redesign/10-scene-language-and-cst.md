# Facet 1 — Scene Language & Canonical CST

> **Status:** design-in-progress. Part of the RISE agentic redesign (Model B). Read
> [`00-CHARTER.md`](00-CHARTER.md) first — this doc inherits its locked decisions (L1–L7),
> open decisions (O1–O3), and invariants (INV-1…INV-6) and does not re-litigate them.
>
> **This facet owns:** the lossless Concrete Syntax Tree (CST); the parser's evolution from
> one-way (`text → Job::Add*` fire-and-forget) to a retained, round-trippable tree; the
> text↔CST round-trip and formatting/comment preservation (INV-4); node identity by
> name-path (L5); declarative iteration replacing `FOR`/`DEFINE`/`hal`/`$(...)`/macros (L3);
> coverage of all ~138 chunk types; the scene-format version bump (O3).
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
from a save-time side-table to the canonical object.**

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
   .RISEscene bytes                Derived Scene (engine)
        ▲  │                              ▲
  serialize │ Lex+Parse             derive │ (Facet 2)
        │  ▼                              │
   ┌─────────────────────┐  Evaluate  ┌──────────────┐
   │   CST  (CANONICAL)   │──────────▶│  EvalTree /  │
   │  lossless, retained  │            │  Scene objs  │
   │  spans + descriptors │◀──────────│              │
   └─────────────────────┘  diagnostics back-annotate nodes
        ▲          ▲
   text edit   structured edit  (both mutate the ONE CST — Facet 3)
```

The CST is the **single source of truth** (INV-1). Text is its serialization; the derived
scene is `Scene = f(CST)`, pure and deterministic (INV-2, Facet 2 owns the function). This
facet specifies the CST data structure, the parser that produces it, identity, and the
declarative-iteration grammar. Facet 2 consumes the CST; Facets 3/4 edit it.

### 2.2 CST node kinds

A CST is a tree of typed nodes. Every node carries a `SourceSpan` (§2.4) and a stable
`NodeId` (§2.5). Node kinds (proposed `enum class CstNodeKind`):

| Kind | Represents | Children | Schema link |
|------|-----------|----------|-------------|
| `Document` | the whole file | header + ordered top-level items | — |
| `Header` | `RISE ASCII SCENE 7` | — | format version (O3) |
| `Chunk` | one chunk block (`sphere_geometry { … }`) | ordered `Parameter` + `RepeatGroup` + `Comment` + `Blank` children | `ChunkDescriptor` (by `keyword`) |
| `Parameter` | one `name value` line | a `Value` | `ParameterDescriptor` (by `name`) |
| `RepeatGroup` | the *set* of a repeatable parameter (all `part` lines, all `cp`) | ordered `Parameter` (one per occurrence) | `ParameterDescriptor` with `repeatable=true` |
| `Value` | a typed value | `ValueAtom`+ (scalar) or `Expr` (symbolic) | `ValueKind` / `tupleKinds` |
| `ValueAtom` | one literal token (`0.6`, `gold`, `TRUE`) | — | — |
| `Expr` | a symbolic value: a function-expression AST (§2.7) | operator/var/call sub-nodes | expression grammar |
| `Command` | an embedded `> load/run/set` line | tokens | command grammar (§2.8.4) |
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
- **Order is preserved.** `Chunk.children` and `Document.items` are ordered vectors. A
  structured edit that changes a value does not reorder; an edit that adds a parameter inserts
  at a descriptor-suggested or end-of-block position (Facet 3's policy).
- **Repeatable params get an explicit `RepeatGroup`** so the `cp`/`part`/`point`/`shaderop`
  lists are addressable and editable per-element (`geometry/dial_sdf.part[3]`), matching the
  `ParseStateBag::GetRepeatable` model but giving each occurrence identity.
- **A symbolic value is an `Expr` node, not a pre-evaluated number.** `position 0 $(@J*0.02-0.1) 0`
  retains the expression in the CST; evaluation happens in derivation (Facet 2), so the CST
  round-trips the author's intent (INV-4), not the computed float.

### 2.3 How the CST maps to the descriptor schema (L6)

The CST does **not** invent a parallel schema (L6 forbids it). Each `Chunk` node references
its `ChunkDescriptor` by keyword; each `Parameter`/`RepeatGroup` references its
`ParameterDescriptor` by name. The descriptor is consulted for:

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

### 2.4 Source spans (text ↔ structured-edit mapping)

Every CST node carries a `CstSpan` — the generalization of the existing `SourceSpan` /
`ParameterSpan` / `RawToken` byte ranges:

```cpp
struct CstSpan {
    FileId      file;          // which source file (supports `> load` children, §2.8.4)
    std::size_t byteBegin;     // first byte of this node's text (incl. its leading trivia? no — trivia is its own node)
    std::size_t byteEnd;       // one past last byte
    std::uint32_t line, col;   // 1-based, for diagnostics surfaced to the agent (Facet 5)
    bool        isSymbolic;    // OR of contained ValueAtom/Expr symbolic flags (carried from RawToken)
};
```

Spans give the two mapping directions the charter's pivot diagram requires:

- **byte-offset → node** (a click in the text editor / an agent's line-anchored diagnostic
  maps to the smallest enclosing node): a binary search over the document's span tree.
- **node → byte-range** (a structured edit on `objects/sphere.material` must splice the right
  bytes): read the `Value` node's span. This is exactly what `SaveEngine`'s Mode-A splice does
  today via `ParameterSpan::valueBegin/valueEnd` — but now driven from the retained CST, so
  the save engine's bespoke span side-table (and the Mode-A/Mode-B duality) collapses into
  "serialize the CST" (see §3).

Spans are *recomputed on serialize* (a CST→text pretty-print assigns fresh byte offsets) and
*incrementally shifted on edit* (reusing `ApplyOffsetDeltas`-style delta application so an
edit doesn't require a full reparse to keep spans valid). INV-4's "don't reformat untouched
text" falls out: serialization emits each node's **retained trivia/text verbatim** unless the
node was edited, in which case only that node's bytes are re-rendered.

### 2.5 Node identity (L5): name-path

Identity currency is the **name-path** — a stable string address built from RISE's name-keyed
managers:

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
managers key them and how animation's `element_type` already works).

**Stability across edits:**

- **Named entities** are stable as long as the name is unchanged. A *rename* is an explicit
  identity-change edit (Facet 3 emits a rename op that rewrites the name node **and** all
  reference values pointing at it — discoverable via `referenceCategories`). Renames are the
  only operation that changes a name-path; ordinary value edits never do.
- **Anonymous / positional nodes** get **synthesized stable identity**:
  - *Unnamed chunks* (a `pinhole_camera` with no `name`) already receive an auto-name in the
    current parser (`AllocateCameraName` → `"default"`, `"default_1"`, …,
    `AsciiSceneParser.cpp:601`). The CST **persists that synthesized name as the node's
    identity** and, on first structured edit, materializes it into the text as an explicit
    `name` parameter (so the identity becomes author-visible and round-trip-stable). Until
    edited, the synthesized name lives only in the CST (text stays untouched — INV-4).
  - *Repeat-group occurrences* are addressed positionally (`part[3]`). Positional identity is
    stable under value edits to that element; an *insert/delete* in the group shifts indices of
    later elements (semantically correct — the 4th part really did become the 5th). Facet
    3/4's binding layer treats a repeat-group edit as targeting "the group + index", and the
    UI re-binds after the structural change (INV-5 is honored for the *group*; per-element
    bindings are index-relative by design).
  - *Sub-value atoms* (`position`'s `y` component) are addressed by `param.atomIndex` only
    transiently for editing; they are not first-class identities (a vec3 is one `Value`).

**This subsumes the round-4 "name-reuse identity serial."** That patch existed because Model
A's live scene could not distinguish "the user renamed X to Y" from "the user deleted X and
created a new Y with X's old name," so it stapled a monotonic serial onto reused names. In
Model B there is no separate live model to reconcile: the CST node *is* the identity, edits
are explicit operations on it (rename vs delete+create are different edit ops with different
intent), and undo is CST version history (Facet 3). The serial is **deleted** — name-path +
explicit-rename-op fully replaces it. (Stated as the L5 mandate; Facet 3 owns the deletion
inventory.)

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
ParseToCst(bytes, fileId) -> Cst, Diagnostics            (this facet)
DeriveScene(Cst) -> Scene, Diagnostics                   (Facet 2; calls the surviving
                                                           Job::Add* "apply layer")
SerializeCst(Cst) -> bytes                                (this facet)
```

`ParseToCst` is **lex → parse → bind-to-descriptor**, producing a tree, never touching `IJob`:

1. **Lex** (reuse `RawTokenCapture`'s lexer, promoted from save-side to front-line): tokens
   with byte spans, `$`-symbolic flags, quoted-string and `$(...)`/`expr(...)` balancing,
   trailing-comment ranges. Comments and blanks become `Comment`/`Blank`/`Trivia` tokens
   rather than being stripped.
2. **Parse structure**: recognize header, chunks (`keyword` + `{` … `}`), embedded `>`
   commands, `let` blocks, `instance_array`/generator chunks, and the *legacy*
   `FOR`/`DEFINE`/macro constructs **as their own node kinds** during a transition window
   (§2.8.3, §6 of this doc). Build the ordered tree with spans.
3. **Bind to descriptors**: for each `Chunk`, look up its `ChunkDescriptor`; for each
   parameter line, look up its `ParameterDescriptor`, type the value into `ValueAtom`s/`Expr`,
   group repeatables into `RepeatGroup`s. Unknown chunk keyword / unknown parameter / type
   mismatch produce **error nodes** (§2.9), not a parse abort.

The descriptor-driven binding step is the direct successor to `DispatchChunkParameters` +
`Finalize`, **minus** the `pJob.AddX` emission. `Finalize`'s knowledge ("how to turn typed
values into engine objects") moves wholesale into Facet 2's derivation, where it calls the
same `Job::Add*` apply layer — so the engine-construction code is *reused*, not rewritten
(charter row 2 "apply-layer reuse"). The split is clean: `Describe()` → CST shape (here);
`Finalize` body → derivation (Facet 2).

#### 2.8.2 `Describe()` → CST → derive, end to end

Worked example for `sphere_geometry { name s  radius 0.6  # main ball }`:

1. `Describe()` (unchanged) advertises params `name:String`, `radius:Double`.
2. **CST**: `Chunk(keyword=sphere_geometry, id=geometry/s)` with children
   `Parameter(name, Value[ValueAtom "s"])`, `Parameter(radius, Value[ValueAtom 0.6],
   trailing Comment "# main ball")`, each with spans.
3. **Derive** (Facet 2): reads the CST `Chunk`, fills a `ParseStateBag`-equivalent (or calls
   the relocated `Finalize` logic directly), emits `pJob.AddSphereGeometry("s", 0.6)`.
4. **Round-trip**: `SerializeCst` re-emits byte-for-byte (untouched nodes verbatim, including
   the comment). A structured edit `geometry/s.radius = 0.8` rewrites only the `radius`
   `Value` span → `radius 0.8  # main ball` (comment preserved — INV-4).

#### 2.8.3 Transition: legacy constructs as CST nodes

To honor O3 (v6 coexistence) without a big bang, the parser recognizes the *legacy*
preprocessing constructs and represents them as explicit, lossless CST node kinds for a
transition window: `LegacyMacroDef`, `LegacyForLoop`, `LegacyExprValue` (`$(...)`),
`LegacyMacroRef` (`@X`/`%X`). These nodes:

- **round-trip verbatim** (INV-4 holds for v6 files), and
- **derive by running the legacy expansion** (the existing `FOR` seek-back / `substitute_macro`
  / `evaluate_expression` logic, relocated into a derivation pre-pass that operates on the CST
  rather than the byte stream) so v6 scenes render identically.

They are **not editable structurally** (the UI shows them read-only / "legacy" — consistent
with the memory's "round-trip scope: prefer read-only" rule). The migration tool (§6 of this
doc) converts them to `instance_array`/`let`/`expr(...)` form; once a file is upgraded to v7
it contains no legacy nodes. This lets v6 and v7 scenes coexist indefinitely while steering
new authoring to the declarative forms.

#### 2.8.4 Embedded commands & `> load` (multi-file CST)

`> load other.RISEscene` / `> run colors.RISEscript` / `> set accelerator B 10 8` are
`Command` nodes. `load`/`run` pull in another file — the CST models this as a **child
sub-document** referenced by `FileId` (each `CstSpan` already carries `file`). The derived
scene is the evaluation of the document graph (root + included children). This matches the
existing `> run scenes/colors.RISEscript` → `load scenes/standard_colors.RISEscene` pattern
that defines the shared `color_*` named painters. **Note the known hazard** (parser README:
`> load` resets `scene_options`/`camera_defaults` thread-local state): in the CST model,
included documents are *nested scopes*, so this becomes a well-defined scoping rule
(parent state visible to children, children's `let`/options don't leak back) rather than the
current accidental thread-local reset. Cross-file edits remain refused/read-only at first
(matching `SaveEngine`'s cross-file guard), surfaced as a `FileId`-tagged span.

#### 2.8.5 Incrementality (INV-3 handoff)

A localized text edit reparses only the touched chunk's byte range into a sub-CST and splices
it (the lexer is line/brace-oriented, so a chunk's bounds are recoverable cheaply); a
structured edit mutates the target node directly. Either way the changed subtree is handed to
Facet 2's incremental derivation. This facet's contract to Facet 2: **a CST edit reports the
set of changed `NodeId`s** so derivation can recompute only the affected subgraph. (Span
offset-shifting reuses the `ApplyOffsetDeltas` delta model.)

### 2.9 Parse errors localize to CST nodes (feeds the agent surface, Facet 5)

Today an unknown parameter aborts the entire parse with a single log line
(`DispatchChunkParameters` → `return false`). For the agent surface this is hostile: one typo
nukes the whole derivation with no structured location. The CST changes this to
**error-recovering parse with node-local diagnostics**:

- Each diagnostic is `{ severity, NodeId, CstSpan, code, message, fix-hint }`. Unknown
  parameter, unknown chunk keyword, type mismatch, missing-required, unresolved reference,
  bad expression — each attaches to the smallest enclosing node and **does not stop** parsing
  the rest of the file.
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
`time`/`value`/`interpolator` on `timeline`) → `RepeatGroup` nodes with per-occurrence
addressing (`…part[3]`).

**Composite/tuple values** (`advanced_shader`'s `shaderop foo 0 5 +`, the
`tupleKinds`-described values; `homogeneous_medium`'s `phase hg 0.5`) → a `Value` with
multiple typed `ValueAtom`s, typed per `ParameterDescriptor::tupleKinds`. The descriptor
already models these; the CST reads the model.

**The macro constructs** (`FOR`/`ENDFOR`, `DEFINE`/`UNDEF`/`!`/`~`, `@`/`%`, `$(...)`,
`sin/cos/tan/sqrt/hal`) → covered by §2.6 (`instance_array` + desugar), §2.6.3 (`let` +
reserved built-ins), and §2.7 (`expr(...)` sublanguage). Legacy spellings round-trip as
read-only legacy nodes (§2.8.3) until migrated.

**Embedded commands** (`> load/run/set`) → `Command` nodes / sub-document references (§2.8.4).

#### What resists — opaque assets

The CST is a faithful tree of the **scene text**. It is *not* a container for binary assets
the text *points at*. These resist inclusion and are handled by reference, not value:

- **Mesh files** — `3dsmesh/rawmesh/rawmesh2/risemesh/plymesh/gltfmesh_geometry`,
  `gltf_import`, `bezierpatch/bilinearpatch_geometry` (all via a `file` param,
  `ValueKind::Filename`).
- **Texture/image files** — `png/jpg/hdr/exr/tiff_painter` `file`.
- **Data files** — `spectral_painter` `file`, `piecewise_linear_function` `file`,
  `datadriven_material` `filename` (MERL BRDF), `voronoi{2,3}d_painter` `file`,
  `sdf_geometry` `file` (the large-SDF sidecar form).
- **Output paths** — `file_rasterizeroutput` `pattern`.

For these, the CST holds the **filename `ValueAtom` plus its `CstSpan`** — fully editable as
text/structured (you can repoint a texture path), but the *content* is loaded by derivation
(Facet 2's deferred-realization seam) and is **outside** the round-trip/version-control story.
Implication for Facet 5/6: a scene is diff-able and git-native *for its text*; binary assets
are referenced artifacts (the same way source code references images it doesn't inline). The
`gltf_import` chunk is the notable case where an *opaque asset expands into many engine
entities* — the CST keeps it as a single `Chunk` (one import directive); the expanded
sub-scene is a derivation product, not CST content. (This mirrors `instance_array`: the CST
holds the directive, derivation holds the expansion.)

---

## 3. Delete / Evolve / Reuse

| Component (current) | Fate | Notes |
|---|---|---|
| [`RawTokenCapture`](../../src/Library/Parsers/RawTokenCapture.h) (lexer + spans + symbolic flags) | **Evolve → reuse** | Promote from save-side Phase 0 to the front-line CST lexer. Lexing rules (quoted strings, `$(...)`/`expr(...)` balancing, comment ranges) are exactly what the CST needs. Add comment/blank emission instead of stripping. |
| [`SourceSpanIndex`](../../src/Library/SceneEditor/SourceSpanIndex.h) (per-entity/param byte ranges, `AuthorMode`, `ApplyOffsetDeltas`) | **Evolve → subsumed by CST spans** | Its data *is* the CST's span layer. The standalone side-table is no longer separately populated; `CstSpan` carries it on the nodes. `ApplyOffsetDeltas`'s delta-shift algorithm is reused for incremental span maintenance. `chunkRevisited`/FOR-dedup logic is subsumed by generator/legacy-node modeling. `insideManagedBlock` becomes obsolete (no managed override block — see SaveEngine row). |
| Descriptor schema ([`ChunkDescriptor.h`](../../src/Library/Parsers/ChunkDescriptor.h), `IAsciiChunkParser::Describe()`) | **Reuse verbatim** | L6. The CST references it; no structural change required for this facet. It gains a fourth consumer (CST binding) alongside parser/highlighter/suggestions. |
| `DispatchChunkParameters` / generic `ParseChunk` (`AsciiSceneParser.cpp:697`, `:9861`) | **Evolve** | Becomes the descriptor-binding step of `ParseToCst` (validate names → typed `ValueAtom`s), **minus** the abort-on-error behavior (→ error nodes, §2.9) and **minus** `Finalize` invocation. |
| `IAsciiChunkParser::Finalize` bodies (the `pJob.AddX` emission) | **Evolve → move to derivation (Facet 2)** | The "typed values → engine objects" logic relocates into Facet 2, which calls the **same `Job::Add*` apply layer**. The apply layer (`Job.cpp`, `RISE_API`) is **reused unchanged**. |
| Top-level preprocessing (`FOR`/`ENDFOR` seek-back, `substitute_macro`, `evaluate_expression`, `evaluate_first_function_in_expression`, the `static MultiHalton mh` global, `macros`/`loops` parser state) | **Evolve → legacy derivation pre-pass; new authoring uses §2.6/2.7** | The expansion logic is retained *only* to derive v6 legacy nodes (§2.8.3). v7 authoring never produces these constructs. The Halton global is replaced by index-explicit `halton(dim, idx)` (INV-2). |
| `AllocateCameraName` auto-naming (`:601`) | **Evolve → CST synthesized identity** (§2.5) | Generalized to all unnamed chunks; synthesized name persisted in CST, materialized to text on first edit. |
| [`SaveEngine`](../../src/Library/SceneEditor/SaveEngine.cpp) Mode-A/Mode-B byte-splice + managed-override-block machinery | **Delete (this facet's contribution to the deletion)** | Replaced by `SerializeCst(Cst)` — pretty-printing the canonical tree, verbatim for untouched nodes, re-rendered for edited nodes. The whole Mode-A-vs-Mode-B duality, the sentinel-bracketed managed block, `OverrideSpanIndex`, `override_object`, the load-time `FileIdentity` external-mod guard's *splice* rationale, and `loadedPropertyValues` diffing exist **only** because the text wasn't retained. With a retained CST they vanish. (Facet 3/6 own the full SaveEngine deletion inventory; this facet supplies the replacement: CST serialization.) |
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

5. **Reference integrity under rename (L5).** A rename rewrites all reference *values* pointing
   at the old name (found via `referenceCategories`). **Open:** references inside `expr(...)`
   (the future `expr( materials/gold.scale )` extension) and references in *opaque assets*
   (a glTF that names a RISE material?) complicate "find all referrers." First slice scopes
   renames to direct `ValueKind::Reference` atoms only; cross-expression/cross-asset reference
   rewriting is deferred and flagged.

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
  "changed-NodeId set" from edits, (c) the descriptor for defaults. I assume *I* own
  expression/generator **parsing into AST/generator nodes**; *Facet 2* owns their
  **evaluation/expansion** (where instance vars and `let` constants are bound, and v6 legacy
  nodes are expanded). If Facet 2 would rather the CST pre-expand generators, that conflicts
  with INV-3/INV-4 (expansion in the CST bloats it and loses authoring intent) — flag for
  synthesis.
- **→ Facet 3 (Edit model).** I assume structured edits are *operations on CST nodes* keyed by
  name-path, that **rename is a distinct edit op** (not a value edit), and that Facet 3 owns
  undo/redo as CST version history (subsuming the round-4 identity-serial — I only mandate L5,
  Facet 3 deletes the serial). I provide: span-based byte↔node mapping, the changed-NodeId
  contract, and error-node round-tripping. The one-shot FOR-desugar (§2.6.2) is a Facet-3 edit
  that I define the *shape* of.
- **→ Facet 4 (Dynamic UI).** I assume the UI binds widgets to CST nodes via name-path and
  chooses widget type from `ParameterDescriptor::kind`/`enumValues`/`referenceCategories`/
  `presets`/`unitLabel` (all already present — no descriptor change needed from me). Legacy
  nodes (§2.8.3) and opaque-asset references render read-only-ish per existing convention.
- **→ Facet 5 (Agentic surface).** I provide the structured-diagnostic shape
  (`{severity, NodeId, span, code, message, fix-hint}`, §2.9) the MCP error channel needs, and
  the name-path addressing scheme agents use to read/patch nodes. Diff-ability/git-nativeness
  rests on INV-4 holding (my §4.1 gate).
- **→ Facet 6 (Migration).** I define the v6↔v7 coexistence mechanism (legacy nodes, §2.8.3)
  and the *target* forms (`instance_array`/`let`/`expr`); Facet 6 owns the corpus migration
  tool and risk register. The v6→v7 migrator follows the `migrate_scenes_v5_to_v6.py` pattern.

**No conflicts with Locked decisions.** I design *to* L1–L7. Open-decision stances: O1 — I
design for lossless-CST-pivot (the working assumption); where text-canonical (O1 alt) would
differ, it's only that "trivia nodes" become "the buffer is truth and the tree is a parsed
view" — my span layer supports either, but I assume CST-canonical so structured edits are
first-class. O3 — I propose the v6→v7 bump (§6) and indefinite coexistence via legacy nodes.

---

## 6. First-slice implications & format version (O3)

### 6.1 Format version bump

Bump the header to `RISE ASCII SCENE 7`. **Coexistence stance:**

- **v6 files load unchanged**, forever. The parser recognizes both versions; v6 files parse to
  a CST that *may contain legacy nodes* (`FOR`/`DEFINE`/`$(...)`/`@`/`%`), which round-trip
  verbatim and derive via the relocated legacy expansion (§2.8.3). A v6 file that uses *no*
  macro constructs (the common modern case, e.g. `watch_dial.RISEscene`) parses to a
  legacy-node-free CST and could be re-saved as v7 with zero semantic change.
- **v7 is the authoring target**: no `FOR`/`DEFINE`/macro-substitution/`$(...)`; repetition
  via `instance_array` + the existing generator chunks; constants via `let`; expressions via
  `expr(...)`. The v7 grammar is a *strict subset* of constructs (drops the imperative
  preprocessing layer) plus the small additions (`instance_array`, `let`, `expr(...)`
  value-syntax) — all descriptor-driven (L6).
- **Migration tool** `tools/migrate_scenes_v6_to_v7.py` (Facet 6 owns it; this facet specifies
  behavior): idempotent; converts FOR→`instance_array` (homogeneous) or explicit-entity
  expansion (heterogeneous, §2.6.2); DEFINE→`let`; `$(...)`/`@`/`%`→`expr(...)`/`let`-refs;
  `hal()`→`halton(dim,idx)` with render-diff verification (§4.2). Mirrors the
  `migrate_scenes_v5_to_v6.py` idiom (idempotent, in-place, with a hard-fail message on
  un-migratable constructs for hand review). v6 files left as-is render identically without
  running it — coexistence is the default; migration is opt-in.

### 6.2 First-slice (one chunk type, full vertical)

To prove the pivot end-to-end with minimal surface, the first slice is **`sphere_geometry`**
(2 params, no references, no repeatables, no expressions — the simplest non-trivial chunk):

1. **bytes → CST**: `ParseToCst` on a one-`sphere_geometry`-chunk file produces a `Document`
   → `Chunk(geometry/s)` → two `Parameter` nodes with spans + any trailing comment, reusing
   the `RawTokenCapture` lexer.
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

This slice exercises every load-bearing claim — retained lossless CST, span-based two-way
mapping, descriptor-driven binding, name-path identity, apply-layer reuse, incremental
re-derive — on the smallest possible chunk, with the parse→serialize identity test as the
non-negotiable correctness gate. Subsequent slices add: a chunk with a `Reference`
(`standard_object.material` → rename integrity, L5), a chunk with a `RepeatGroup`
(`sdf_geometry.part[]`), the `expr(...)` sublanguage (one `Double` param), and finally
`instance_array` (replacing `loops.RISEscene`'s nested `FOR`).
