# Facet 2 — Derivation Engine (CST → Scene, incremental)

> **Status:** design-in-progress. One facet of the [Agentic Redesign](00-CHARTER.md).
> Read the [charter](00-CHARTER.md) first — this doc assumes its locked decisions
> (L1–L7), invariants (INV-1…6), and the canonical-CST pivot. **Design only; no
> code/build/scene changes.**
>
> **This facet owns:** the function `Scene = derive(CST)` — made *pure*,
> *deterministic*, *incremental*, and *memoized*; the CST→engine dependency graph;
> memo granularity; the derive/apply seam (reuse of the surviving in-place
> apply-layer); interaction with deferred realization / TLAS / photon passes; the
> order-independence audit of `Job`; perf targets; partial/error derivation.
>
> **Neighbors:** consumes the CST shape from [Facet 1](10-scene-language-and-cst.md);
> shares the apply-layer reuse story with [Facet 3](30-edit-model-and-history.md)
> (which deletes the history/dirty/transaction wrapper around that apply-layer);
> feeds rendered output + structured validation errors to
> [Facet 5](50-agentic-surface.md).

---

## 1. Current-state grounding (what exists today)

RISE today is **Model A**: the parser drives a one-shot, whole-world assembly into
a mutable `Job`, and interactive edits mutate the resulting live `Scene` in place.
There is no "derivation function" — there is a *loader* and a separate *in-place
mutator*. Facet 2 unifies them. The raw materials already in the tree:

### 1.1 The loader (`CST→Scene`, today: whole-file, non-incremental)

`AsciiSceneParser::ParseAndLoadScene(IJob&)`
(`src/Library/Parsers/AsciiSceneParser.cpp:10431`, ~560 lines) is the entire
"derivation" today. It is **strictly whole-file**: it reads the file into a
buffer, tokenizes, expands `FOR`/macros/`$(...)`, then for each chunk looks up an
`IAsciiChunkParser` in the registry and calls `ParseChunk → Finalize`, which emits
one or more `pJob.AddX(...)` / `pJob.SetX(...)` calls
(`AsciiSceneParser.cpp:10804-10911`). There is **no partial-parse / no incremental
re-derive** path. A one-character edit re-reads and re-dispatches the whole file.

The chunk parsers are **descriptor-driven** (L6): each overrides only `Describe()`
(the schema) and `Finalize(const ParseStateBag&, IJob&)` (the emit). The default
`ParseChunk` validates input lines against `Describe().parameters` and dispatches.
This is the seam Facet 1 turns into a retained CST; **the `Finalize → pJob.AddX`
mapping is exactly the per-node derivation rule** Facet 2 will memoize.

### 1.2 The assembly target — `Job` and its managers

`Job::InitializeContainers()` (`src/Library/Job.cpp:265`) creates the `Scene`,
the named managers (`GeometryManager`, `MaterialManager`, `PainterManager`,
`ScalarPainterManager`, `ShaderManager`, `ShaderOpManager`, `CameraManager`,
`LightManager`, `ModifierManager`, `Function1D/2DManager`), the `"none"` null
material/painter defaults, the default shader-ops (`DefaultReflection`,
`DefaultDirectLighting`, `DefaultPathTracing`, …), and the default Film.

Every manager is a `GenericManager<T>` = `std::map<String, (T*, refs)>`
(`src/Library/Managers/GenericManager.h:56`). Two facts matter enormously for
Facet 2:

- **Entries are name-keyed and enumerated in lexicographic order** — so the
  *managed-entry set* is already order-independent (INV-2) *for storage and
  enumeration*. Order-dependence lives in the **build order**, not the store.
- **Each entry carries a monotonic identity serial** (`m_serials`,
  `GetItemSerial`, `GenericManager.h:62,150`), bumped on every `AddItem`. This is
  RISE's existing answer to L5/INV-5 at the *engine-object* level; Facet 2 maps a
  CST node's stable name-path onto a manager entry + serial.

The construction surface is `IJob` (`src/Library/Interfaces/IJob.h`, ~3200 lines,
~150 `AddX`/`SetX` methods) implemented by `Job` (`src/Library/Job.cpp`,
~10k lines). `RISE_API.h` is the lower-level factory layer the managers' items are
built through.

### 1.3 The realize / acceleration / photon seam (single-threaded, pre-parallel)

The derived `Scene` is **not** render-ready when assembly finishes. A second,
single-threaded pass materializes deferred work at render time. In
`PixelBasedRasterizerHelper::RasterizeScene`
(`src/Library/Rendering/PixelBasedRasterizerHelper.cpp`):

1. `pCaster->AttachScene(&pScene)` (`:994`) → runs the **realize-from-roots
   cascade** (`RayCaster::AttachScene`, `src/Library/Rendering/RayCaster.cpp:140`):
   enumerates objects, calls `obj.Realize()` on each (`:129-137`), which bakes
   deferred geometry — `DisplacedGeometry::Realize()` tessellates+bakes its mesh
   and cascades to its base; `CSGObject::Realize()` cascades into operands. This is
   the [deferred-realization](../../CLAUDE.md) Phase-1 root set. `Realize()` is
   `const` + idempotent.
2. `AttachScene` also rebuilds the **light samplers** — but *only if* the scene's
   **light-topology generation** advanced since the last build
   (`SceneLightGeneration` / `GetLightTopologyGeneration`, `RayCaster.cpp:114-119,
   175-194`). Same-pointer + same-generation = O(1) fast path.
3. `pScene.GetObjects()->PrepareForRendering()` (`:998`) → builds the **TLAS**
   (top-level BVH4 over objects) from current world-space bboxes.
4. `if (ConsumesScenePhotonMaps()) BuildPendingPhotonMaps(...)` (`:1006-1010`) →
   shoots photon maps, **gated** on the active rasterizer consuming them
   (`IRasterizer::ConsumesScenePhotonMaps`, `IRasterizer.h:142` — `true` for the
   shader-graph rasterizers, `false` for PT/BDPT/VCM/MLT which own transport).
5. **Then** the parallel pixel loop runs under `RenderParallelScope`
   (`PixelBasedRasterizerHelper.cpp:1362`), the RAII freeze-guard
   (`src/Library/Utilities/RenderParallelScope.h`) that asserts (DEBUG) no
   `Realize()` happens mid-render. **The scene is immutable during this bracket.**

This staged pipeline is the single most important constraint on Facet 2:
**derivation has two phases — (A) a cheap, incremental, order-independent
*assembly* of the manager graph, and (B) an expensive *realization* (bake → TLAS →
light samplers → photons) that must complete single-threaded before the freeze
bracket.** Incrementality must apply to *both*, with B re-run only for the parts A
invalidated.

### 1.4 The surviving in-place apply-layer (to be reused)

The interactive editor already does incremental, in-place mutation of the live
`Scene` — this is exactly the "incremental backend" Facet 2 needs, minus the
Model-A bookkeeping. The reusable core:

- **`SceneEditor::ApplyObjectOpForward(IObjectPriv&, const SceneEdit&)`**
  (`src/Library/SceneEditor/SceneEditor.cpp:718`) — the per-op forward mutation:
  transform set/translate/rotate/scale, material/shader/geometry/medium rebind,
  shadow flags. Each binding op resolves the *new* dependency by name through the
  manager (`mMaterialManager->GetItem(...)`, `mJob->GetGeometry/GetMedium`) and
  rebinds; returns `false` if the target name no longer resolves.
- **`SceneEditor::ApplyForwardMutation(const SceneEdit&)`**
  (`SceneEditor.cpp:1586`) — the dispatcher: object ops → `ApplyObjectOpForward` +
  the **invariant chain** (`RunObjectInvariantChain`, `:851`:
  `FinalizeTransformations` → `ResetRuntimeData` →
  `InvalidateSpatialStructure` → conditional light-gen bump); camera ops; property
  ops (`SetMaterialProperty`, `SetLightProperty`, `SetMediumProperty`,
  `SetCameraProperty`) routed through the per-domain **introspection setters**
  (`MaterialIntrospection::SetSlotValue`, `MediaIntrospection`,
  `LightIntrospection`, `CameraIntrospection` in `src/Library/SceneEditor/`).
- **The light-topology invalidation helpers**: `BumpSceneLightGeneration`,
  `BumpSceneLightGenerationIfEmitterSetChanged`,
  `BumpSceneLightGenerationIfMaterialEmits` (`SceneEditor.cpp:63-106`) — plus the
  self-invalidating `LightManager` callback installed in `Job::InitializeContainers`
  (`Job.cpp:320`). These encode the **emissive-material → light-topology**
  dependency edge (the H3 chokepoint from the post-mortem).

**What wraps this today (and is owned/deleted by Facet 3, not us):** the
`SceneEdit` value-record vocabulary (`SceneEdit.h`), `EditHistory`
(undo/redo/transaction stacks, `EditHistory.h`), `DirtyTracker`
(round-trip-save dirty channels, `DirtyTracker.h`), the transaction
snapshot/rollback machinery, and the `SaveEngine` text-splicing path
(`SaveEngine.cpp` — patches byte-spans of the original file). Facet 2 keeps the
**mutation primitives** and the **invalidation edges**; it discards the *edit
record* as the carrier (CST diffs become the carrier) and the *dirty/history*
side-state (CST versioning + the memo table replace it).

### 1.5 The de-brittling post-mortem — the design lessons that bind us

`docs/gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md` is the *why* behind Model B.
Its four root patterns map directly onto Facet 2 design obligations:

| Post-mortem pattern | Facet 2 obligation |
|---|---|
| **P-STATE** (hand-assembled captured state, perpetually incomplete) | The memo table must be **derived, not hand-listed** — keyed structurally off the CST + dependency graph, so "another field forgotten" is impossible. |
| **P-WALK** (edit logic duplicated across 5 walks) | There must be **one** derivation/apply path. No "incremental path" that can drift from a "full path." The full derive is the incremental derive over the whole CST. |
| **P-INVALIDATE** (light-topology bump scattered across mutators) | Invalidation must be **edges in the dependency graph**, traversed by the engine — not hand-bumped per mutator. The existing `BumpLightTopologyGeneration` becomes a *derived consequence* of an `emissive-material → light-set` edge. |
| **P-FFMATH** (`-ffast-math` folds NaN sentinels) | Memo keys / "unchanged" tests must not use NaN/Inf sentinels (RISE is `-O3 -flto -ffast-math`; see memory `ffast-math: no infinity`). Use content hashes + explicit presence bits. |

---

## 2. The Model-B design

### 2.1 Shape of the function

```
derive : CST  →  DerivedScene            (pure, deterministic — INV-2)
derive : (CST, CST', DerivationCache)  →  (DerivedScene', DerivationCache')   (incremental — INV-3)
```

`DerivedScene` is exactly today's render-ready state: the `Scene` + its populated
managers + realized geometry + TLAS + light samplers + (conditionally) photon
maps. The incremental form takes the previous CST, the new CST, and the memo cache,
and re-derives **only the affected subgraph**. There is **one** implementation:
the full derive is the incremental derive against an empty cache. (P-WALK: no
second path.)

Determinism (INV-2) means: the result is a pure function of the CST's *content*,
independent of node *visit order*. §4 audits where today's `Job` violates this and
what must change.

### 2.2 The dependency / dataflow graph

The derivation is a DAG from **CST nodes** (chunks + their parameter values, and
the inline/expression sub-nodes Facet 1 defines) to **derived engine objects**.
Nodes and the edges that carry invalidation:

```
              ┌─────────── CST node (chunk) ───────────┐
              │  e.g.  ggx_material { name=glass ...}   │
              └───────────────┬────────────────────────┘
   value edges (params)       │ name-ref edges (referenceCategories, from the descriptor — L6)
        (literals,            ▼
         $(...) exprs)   DerivedEntity  (a manager entry: IMaterial* "glass", serial S)
                              │
        ┌─────────────────────┼─────────────────────────────┐
        ▼ (object binds mat)   ▼ (mat is emissive)            ▼ (mat used by N objects)
   DerivedObject          LightTopology                  [fan-out set]
        │                      │
        ▼ (object transform/   ▼
           geometry/bbox)   LightSampler / EnvironmentSampler  (realize phase B)
   RealizedGeometry
        │
        ▼ (world bbox)
      TLAS  ──────────────►  PhotonMaps (only if rasterizer ConsumesScenePhotonMaps)
```

**Node taxonomy** (memo nodes — see §2.3 for granularity):

| Layer | Memo node | Built by | Invalidated when |
|---|---|---|---|
| L0 *value* | a resolved parameter value (literal / evaluated `$(...)` / inline sub-chunk) | Facet-1 evaluator | its source text or any expression input changes |
| L1 *entity* | a manager entry (painter, material, geometry, shader, shaderop, modifier, function, camera, medium, light) | `Finalize → pJob.AddX` | any L0 value it consumes changes, OR a name-ref target's *identity* changes (see §2.6) |
| L2 *object* | an `IObject` (geometry+material+modifier+shader+transform bind) | `AddObject`/`AddObjectMatrix`/`AddCSGObject` | any bound L1 entity rebinds, or its transform params change |
| L3 *realized geom* | baked mesh for a deferred geometry (displaced/SDF/CSG/bezier) | `obj.Realize()` cascade (phase B) | its source geometry entity or any tessellation-driving param changes |
| L4 *TLAS* | top-level BVH over objects | `PrepareForRendering` (phase B) | any L2 object's **world bbox** changes (transform or geometry swap) |
| L5 *light topology* | `LightSampler` + `EnvironmentSampler` | `RebuildLightSamplers` (phase B) | light add/remove, emissive-material bind/unbind, exitance edit, **spatial change of an emissive object**, env map replace/scale |
| L6 *photon maps* | caustic/global/shadow/translucent maps | `BuildPendingPhotonMaps` (phase B) | any L2/L3/L5 change **and** the active rasterizer consumes them |

The edges are **derivable from the descriptor schema (L6) plus three engine-level
relations** that the schema does not express and which Facet 2 must own as
explicit graph edges:

1. **emissive-material → light-topology** (L1→L5). Already encoded imperatively in
   `BumpSceneLightGenerationIfMaterialEmits` / `…IfEmitterSetChanged`. Model B
   makes it a graph edge: a material entity exposes "is-emissive"; an edit that
   flips it (or edits an emissive slot) marks L5.
2. **object-spatial → {TLAS, light-topology-if-emissive}** (L2→L4, L2→L5). Encoded
   in `RunObjectInvariantChain` + `OpNeedsSpatialRebuild` (`SceneEdit.h:391`).
3. **rasterizer → photon-pass gate** (active-rasterizer → L6). Encoded in the
   `ConsumesScenePhotonMaps()` runtime check. In Model B the active-rasterizer CST
   node is an input to whether L6 is part of the derived scene at all.

The descriptor's `referenceCategories` (`ParameterDescriptor`, per
`Parsers/README.md`) already declares every *name-ref* edge (e.g. a
`standard_object`'s `material` param references `ChunkCategory::Material`). Facet 2
reads those to build L1↔L1 and L2→L1 edges **for free** — no parallel schema
(INV / L6). The three engine relations above are the only hand-authored edge
rules, and there are exactly three, each already existing imperatively.

### 2.3 Memoization keys & granularity

**Granularity: per-CST-node, at the chunk level for entities (L1) and per-object
for L2.** Not per-subtree (too coarse — a one-param edit to one of 200 objects
would re-derive the subtree) and not per-token (too fine — sub-chunk inline values
are L0 and roll up into their owning chunk's L1 key).

**Key = a content hash of the node's *resolved inputs*, not its text span.** For an
L1 entity node:

```
key(entity) = H( chunk_keyword,
                 { (param_name, resolved_value) for each param },   // L0 values, post-$()-eval
                 { identity_serial(target) for each name-ref param } )  // §2.6
```

Hashing *resolved values* (not raw text) gives INV-4 for free in the other
direction: a whitespace-only or comment-only edit (which Facet 1's CST preserves
losslessly) changes the text span but **not** the resolved-input hash → memo hit →
zero re-derivation. Conversely `$(2+3)` and `5` hash identically once evaluated, so
refactoring an expression that yields the same value is a no-op derive.

The memo table:

```cpp
struct DerivationCache {
    // name-path  →  derived record (the engine object + its key + its serial)
    std::unordered_map<NamePath, EntityRecord> entities;   // L1
    std::unordered_map<NamePath, ObjectRecord> objects;    // L2
    // Reverse dependency index: which derived records depend on a given producer,
    // so an invalidation can be pushed forward in O(out-degree), not O(scene).
    std::unordered_multimap<NamePath, NamePath> dependents; // producer → consumers
    // Phase-B coarse generations (mirror the existing engine counters):
    uint64_t builtLightTopologyGeneration; // == Scene::GetLightTopologyGeneration() at last L5 build
    bool     tlasValid;                     // false ⇒ PrepareForRendering re-run next render
    PhotonMapGeneration photonGen;          // bumped by any L2/L3/L5 change
};

struct EntityRecord {
    InputHash      key;         // §2.3 content hash
    uint64_t       serial;      // manager identity serial at build time (§2.6)
    EntityKind     kind;
    bool           isEmissive;  // drives the L1→L5 edge
    // (the engine object itself lives in the manager, keyed by the same name-path)
};
```

`NamePath` is the L5 identity currency (`objects/sphere`, `materials/glass`,
`materials/glass.reflectance` for a slot). It indexes both the CST node (Facet 1
guarantees stable name-paths, INV-5) and the manager entry — the two are joined by
name, which is *already* how `GenericManager` keys everything.

### 2.4 How a localized edit re-derives only the affected subgraph (INV-3)

Given `(CST, CST', cache)`:

1. **Diff** CST→CST′ at node granularity (Facet 1 supplies the structural diff, or
   we recompute input-hashes per node and compare to `cache`). Result: a set of
   **dirty nodes** {added, removed, changed}.
2. **Recompute keys** for dirty nodes' L1/L2 records. For each, compare new
   `key` to the cached `key`:
   - **equal** → memo hit, skip (covers comment/whitespace/expression-refactor
     edits, and "typed the same value back" — the same short-circuit
     `Job::SetFilm` already does manually at `Job.cpp:489`).
   - **changed** → re-run that node's `Finalize`-equivalent **via the apply-layer
     where possible** (§3), else rebuild that one entity.
3. **Push invalidation forward** along `dependents` edges (the reverse index), a
   worklist BFS:
   - L1 changed → mark dependent L2 objects' binds dirty (rebind, not rebuild).
   - L1 emissive-ness changed, or an emissive L1's slot changed → mark **L5**
     (`BumpLightTopologyGeneration`).
   - L2 spatial change (transform/geometry) → mark **L4** (`tlasValid=false`) and,
     if the object is emissive, **L5**.
   - L3 source changed → re-`Realize()` that geometry (idempotent; the realize
     pass already re-walks and no-ops the unchanged).
   - any L2/L3/L5 change → bump `photonGen` (consumed only if L6 is live).
4. **Phase-B reconciliation at next render** is *unchanged from today* and needs no
   new code: `AttachScene` already (a) re-walks `Realize()` idempotently — only the
   re-marked L3 geometries do work; (b) rebuilds light samplers iff
   `liveGen != builtLightGeneration`; `PrepareForRendering` rebuilds the TLAS;
   `BuildPendingPhotonMaps` re-shoots iff pending+consumed. **Facet 2's job is to
   set those existing dirty flags precisely, from the graph, instead of from
   scattered hand-bumps.** This is the deepest reuse in the design: the entire
   phase-B incremental machinery already exists and is correct; we just feed it a
   graph-derived dirty set.

**Worked example — `param → object → TLAS`:** user edits `sphere`'s `position`.
Diff → one dirty L0 (the position value) → its owning L2 object record's key
changes → apply-layer runs `SetObjectPosition` + `RunObjectInvariantChain`
(`FinalizeTransformations`, `InvalidateSpatialStructure`) → `tlasValid=false`. No
L1 entity touched, no other object touched, light samplers untouched (sphere
non-emissive). Next render: realize no-ops everything, `PrepareForRendering`
rebuilds the TLAS (unavoidable — one bbox moved), pixels re-render. **Cost ≈ one
matrix finalize + one TLAS build.**

**Worked example — `emissive material → light topology`:** user raises an area
light material's `exitance`. Diff → one dirty L0 → owning L1 material record's key
changes, `isEmissive` stays true → apply-layer runs
`MaterialIntrospection::SetSlotValue` → L1→L5 edge fires →
`BumpLightTopologyGeneration`. Next render: `AttachScene` sees
`liveGen != builtLightGeneration` → `RebuildLightSamplers` (rebuilds the alias
table with the new weight); realize/TLAS/photons untouched (unless L6 live). This
is exactly the existing `BumpSceneLightGenerationIfMaterialEmits` path — Model B
just reaches it from a graph edge instead of a hand-coded call site.

### 2.5 The derive/apply seam — incremental update vs. rebuild-from-scratch

Two backends produce L1/L2 records; the **decision rule** picks per dirty node:

```
For a CHANGED node N:
  if N is structural  (added node, removed node, or a change that alters the
                       node's TYPE / its ref-edge SHAPE — e.g. material kind
                       changed, a new param appeared that adds a dependency,
                       composed-material slot, geometry kind changed)
        → REBUILD: run the full Finalize-equivalent → AddX into the manager
                   (remove the old entry first; this bumps the identity serial,
                   which §2.6 propagates to dependents).
  else if N is a VALUE-ONLY change to a slot the apply-layer can mutate in place
       (object transform / material slot / light prop / medium prop / camera prop /
        object material|shader|geometry|medium REBIND)
        → APPLY: call the surviving in-place mutator (ApplyObjectOpForward /
                 the Introspection SetSlotValue setters) + its invariant chain.
  else → REBUILD (safe default).
```

**Why this split.** The apply-layer (§1.4) is *precisely* a set of fine-grained,
in-place, render-thread-coherent mutations the engine already supports between
passes (pointer swaps the workers read coherently). Reusing it for value-only edits
gives the cheapest possible re-derive (no manager churn, no serial bump, no
dependent cascade beyond the known invariant edges) and inherits all the hard-won
correctness (the invariant chain, the conditional light-gen bumps). Structural
edits *must* rebuild because the apply-layer has no "change a material's *type*" or
"add a dependency edge" operation — and trying to add one would re-introduce P-WALK
(a second mutation vocabulary that drifts from the loader). **The loader's
`Finalize → AddX` IS the rebuild path; the apply-layer IS the fast path; there is
no third path.**

This also resolves a charter tension cleanly: Facet 3 deletes the `SceneEdit`
*record* and the history/dirty wrapper, but the **mutation primitives** under
`ApplyObjectOpForward` and the `*Introspection` setters survive and are *re-homed*
under Facet 2 as the "APPLY backend." The carrier changes from a `SceneEdit` value
to a `(NamePath, slot, resolved-value)` derived from the CST diff; the primitive it
calls is the same.

**Mapping the existing apply ops to derive-time fast paths:**

| CST value-only change | Apply primitive (reused) | Invariant edge fired |
|---|---|---|
| object `position/orientation/scale` | `SetObjectPosition`/`…Orientation`/`…Scale` + `RunObjectInvariantChain` | L4 (+L5 if emissive) |
| object `material`/`shader`/`geometry`/`interior_medium` (rebind to existing entity) | `ApplyObjectOpForward` rebind arm | L5-if-emitter-set-changed; L4 if geometry |
| material slot (`reflectance`, `ior`, `roughness`, …) | `MaterialIntrospection::SetSlotValue` | L5 if material emissive |
| light prop (`position`/`energy`/`color`/`target`) | `LightIntrospection` setter | L5 (always — it's a light) |
| medium prop (`absorption`/`scattering`/`emission`) | `MediaIntrospection::SetSlotValue` (Homogeneous only) | none (volume re-derives sigma_t internally) |
| camera prop / transform | `CameraIntrospection` / camera ops | none (camera is not in the spatial/light graph) |

Anything not in this table → REBUILD.

### 2.6 Identity & the name-ref edge (INV-5)

Two derived entities can share a name across a remove+re-add that changes the
*thing* under the name. The existing `GenericManager` identity serial
(`GenericManager.h:62`) already detects this; the interactive editor uses
`capturedTargetSerial` for exactly this guard (`SceneEditor.cpp:1370`). Facet 2
adopts it as the **name-ref edge's freshness token**:

- A dependent record stores the serial of each name-ref target at build time.
- When a producer is REBUILT (structural change), its manager serial bumps.
- A consumer whose stored target-serial ≠ the live serial is **stale** even if its
  own params are unchanged → it must re-resolve the binding (cheap: rebind via the
  apply-layer) or rebuild. This is how "material `glass` was replaced by a
  different-kind material under the same name" correctly invalidates every object
  bound to `glass`, without those objects' own CST nodes having changed.

Name-path (L5 currency) is the *stable* handle for UI/agent/selection; the serial
is the *freshness* token for derivation. They are orthogonal and both already
exist.

### 2.7 Interaction with realization, `CanTessellate`, the freeze guard, photons

- **Deferred realization (phase B):** Facet 2 does **not** realize during the
  assembly phase (A). It marks L3 nodes dirty; the existing single-threaded realize
  pass in `AttachScene` bakes them at render time, before the freeze bracket. The
  freeze guard (`RenderParallelScope`, `g_renderParallelDepth`) is honored *for
  free* because Facet 2 never mutates the manager graph during the parallel pass —
  all derivation happens at the debounced commit (§4 / O2), strictly outside any
  render. **Facet 2 must assert `g_renderParallelDepth == 0` at the top of any
  incremental re-derive** (mirrors the apply-layer's existing park gate; see §2.8)
  — re-deriving while a render is live would race the workers.
- **`CanTessellate` / parse-refusal:** a `displaced_geometry` over a
  non-tessellatable base (e.g. `InfinitePlaneGeometry`) must fail derivation, not
  render black. Today `AddDisplacedGeometry` returns false and the whole parse
  fails. In Model B this is a **node-local derivation error** (§5): the L1
  geometry node derives to a *hole*, the L2 objects binding it derive to holes,
  and the rest of the scene derives normally. The error attaches to the offending
  CST node (Facet 5 surfaces it). `CanTessellate` is checked at derive time (phase
  A), not realize time, so the error is reported before any render is attempted.
- **Photon-pass gating:** L6 is part of the derived scene *iff* the active
  rasterizer's `ConsumesScenePhotonMaps()` is true. Changing the active-rasterizer
  CST node from `pixelpel_rasterizer` (consumes) to `pathtracing_pel_rasterizer`
  (doesn't) **removes L6 from the graph** — no photon shoot, and any pending
  photon dirtiness is irrelevant. Changing back re-adds L6 and marks it pending.
  This makes the existing runtime gate a *graph-membership* decision.

### 2.8 Threading & the commit gate

Derivation runs on the **commit** of a debounced CST edit (O2), on the UI/agent
thread, with the render thread **parked** — reusing the exact cancel-and-park gate
the interactive editor already implements (`SceneEditController`:
`mCancelProgress.RequestCancel()` → `mCV.wait(!mRendering)` → mutate →
`mEditPending=true` → `notify_one`, per the controller's documented protocol).
Model B's only change to that gate: the thing applied under the lock is
"incremental re-derive of the CST diff" instead of "`SceneEditor::Apply(edit)`".
The re-kick that follows is the same `RasterizeScene` on the same Job/Scene. **No
new concurrency primitive is introduced** (INV-1: no second mutable representation;
the memo cache is derived state, not an independent source of truth — it can be
dropped and rebuilt from the CST at any time).

---

## 3. Delete / Evolve / Reuse

| Component (file) | Fate | Rationale |
|---|---|---|
| `AsciiSceneParser::ParseAndLoadScene` whole-file loader (`AsciiSceneParser.cpp:10431`) | **Evolve** | Becomes the *full-derive special case* (incremental derive vs empty cache). The chunk-walk + `Finalize→AddX` is retained as the **REBUILD backend**, driven from the CST instead of re-tokenizing text. The `FOR`/`DEFINE`/`hal`/`$(...)` macro front-end is removed by Facet 1 (L3); Facet 2 consumes the post-evaluation CST. |
| `IAsciiChunkParser::Finalize` per chunk | **Reuse (re-homed)** | This *is* the per-node L1 derivation rule. Keep verbatim as the rebuild emitter; it now runs per dirty node, not per file. |
| `IJob::AddX/SetX` surface + `Job.cpp` | **Reuse, with order-independence fixes** | The construction API stays; §4 lists the specific ordering effects to remove. |
| `GenericManager<T>` (name-key + identity serial) | **Reuse** | Already the order-independent store + freshness token Facet 2 needs. |
| `SceneEditor::ApplyObjectOpForward` / `ApplyForwardMutation` (`SceneEditor.cpp`) | **Reuse (re-homed as the APPLY backend)** | The fine-grained in-place mutators for value-only edits. Strip the `SceneEdit`-record coupling; drive from `(NamePath, slot, value)`. |
| `*Introspection::SetSlotValue` (Material/Media/Light/Camera) | **Reuse** | The per-domain slot setters the APPLY backend calls. |
| `BumpLightTopologyGeneration` + the `…IfEmitterSetChanged`/`…IfMaterialEmits` helpers + the `LightManager` self-invalidate callback | **Reuse (as the L1/L2→L5 edge implementation)** | The emissive→light-topology dependency edge already exists imperatively; Facet 2 makes the graph drive it. The coarse generation counter stays as L5's dirty flag. |
| Phase-B reconciliation (`RayCaster::AttachScene` realize+sampler rebuild, `PrepareForRendering`, `BuildPendingPhotonMaps`, `RenderParallelScope`) | **Reuse, unchanged** | Already incremental + correctly staged + freeze-guarded. Facet 2 only feeds it a precise dirty set. |
| `SceneEdit` value record (`SceneEdit.h`) | **Delete (owned by Facet 3)** | The edit carrier becomes the CST diff. The *op semantics* are absorbed into §2.5's apply-table; the struct goes. |
| `EditHistory` (`EditHistory.h`) | **Delete (Facet 3)** | Undo/redo = CST version history. |
| `DirtyTracker` (`DirtyTracker.h`) | **Delete (Facet 3)** | The memo cache's reverse-dependency index + phase-B generations replace the dirty channels; round-trip-save dirtiness is moot once text↔CST is lossless (Facet 1/3). |
| Transaction snapshot/rollback machinery in `SceneEditor`/`EditHistory` | **Delete (Facet 3)** | Rollback = restore a prior CST version + drop the memo cache. |
| `SaveEngine` text-splicing (`SaveEngine.cpp`) | **Delete (Facet 1/3)** | Serialization is the CST's faithful text projection (INV-4); no diff-and-splice needed. |
| `Job`'s round-trip metadata (`SourceSpanIndex`, `TransformSnapshot`, `OverrideSpanIndex`, `Job.cpp:276-279`) | **Delete (Facet 1/3)** | Span-tracking existed to splice text edits back; obsolete under canonical-CST. |

**Net for Facet 2:** delete *zero* engine code; **reuse** the loader's emit rules,
the managers, the apply-layer mutators, and the entire phase-B pipeline; **add**
the memo cache + dependency graph + diff-driven dirty propagation as a new thin
layer *above* `IJob`. The deletions are all in the *edit/history/save* wrapper,
owned by Facets 1/3.

---

## 4. Order-independence audit of `Job` (what must change for INV-2)

INV-2 demands `derive` be visit-order-independent. The *managers* are already
order-independent (lexicographic `std::map`). The order-dependence is in `Job`'s
*build-time* behavior. Enumerated:

1. **`SetPrimaryAcceleration` discards prior objects.** `Job.cpp:437-454` shuts
   down + recreates the `ObjectManager`, **silently dropping every object added so
   far** (documented in `IJob.h:88-89`: "Call this before adding objects,
   otherwise you will LOSE them!"). This is a load-order landmine: a `scene_options`
   /acceleration chunk appearing *after* objects wipes them.
   **Fix:** acceleration config becomes a *declarative scene-level node* consumed
   when the TLAS is built (phase B), never a mid-stream imperative reset. Derivation
   reads the final acceleration params from the CST regardless of position; the
   `ObjectManager` is created once. (If acceleration params change, mark L4 dirty —
   a TLAS rebuild — never discard objects.)

2. **Name-resolution-at-add-time requires producers before consumers.**
   `Job::AddObject` (`Job.cpp:5234-5288`) resolves `geom`/`material`/`modifier`/
   `shader` by name *immediately* and **fails** if the name isn't registered yet.
   Same for every `AddX` that takes a name-ref (materials referencing painters,
   shaders referencing shaderops at `Job::AddStandardShader` `:6177`, etc.). So a
   scene that declared an object before its material fails today — pure load-order
   dependence.
   **Fix (two layers):** (a) Facet 2 **topologically orders the derive by the
   dependency graph** (producers before consumers) regardless of CST order — the
   graph is built from the descriptor's `referenceCategories` *before* any `AddX`
   runs, so derivation visits in dependency order. (b) Dangling refs (a name with
   no producer node) derive to a **hole** (§5) rather than failing the whole scene.
   Cycles in name-refs (illegal) are detected at graph-build and reported as
   node-local errors.

3. **Default shader-ops / null defaults are injected imperatively in
   `InitializeContainers`.** `Job.cpp:347-373` adds `"none"` material/painter and
   `DefaultReflection`/`DefaultDirectLighting`/`DefaultPathTracing`/… **before** any
   scene content. These are fine (they're constant, position-independent), but they
   mean the manager contents are *not* a pure function of the CST alone — they're
   `CST + a fixed prelude`. **Fix:** model the prelude as an implicit, constant
   "prelude CST" prepended to every document (or a fixed set of synthetic root
   nodes). `derive` stays pure: `Scene = derive(prelude ⊕ CST)`. No behavior change;
   it just makes the determinism statement honest.

4. **"Last-added-wins" active-camera policy** (`IJob.h:108-124`): each `AddXxxCamera`
   makes itself active. Under order-independent derivation, "which camera is active"
   can't depend on add order. **Fix:** active-camera is an explicit scene-level node
   (it already round-trips via `SetActiveCamera`/`GetActiveCameraName`); derivation
   sets it from that node, not from add order. (Session/view camera — the
   orbit-preview camera — is ephemeral per L4 and not part of `derive`.)

5. **`camera_defaults` / `scene_options` "last write wins" + `> load` reset**
   (`Parsers/README.md` §"Multiple `camera_defaults`"): these are
   thread-local-parser state with documented order traps (a `> load` silently
   resets `scene_unit`). **Fix:** Facet 1 removes `> load` include-time state;
   scene-level config nodes are resolved once from the final CST. Cameras read the
   resolved config, not "config as of that line."

6. **Per-field "last write wins" for any duplicate scene-level chunk.** Generally:
   derivation must define a deterministic resolution for duplicate singletons
   (film, active rasterizer, global medium, …). **Rule:** last node in document
   order wins (matches today's last-write-wins and is itself order-defined by the
   CST's node order, which *is* canonical — this is determinism w.r.t. *content*,
   and document order is content). This is the one place "order matters" is
   legitimate: the CST's node ordering is part of its content, so a deterministic
   last-wins over duplicates is INV-2-compliant. Flag for synthesis: prefer making
   singletons *unique* (a second `film` node is a validation error) over silent
   last-wins, to match the "diff-able, reviewable" thesis.

**Summary:** items 1, 2, 4, 5 are real order-*dependence* bugs that Model B must
fix (mostly by making config declarative + topo-ordering the derive); item 3 is a
purity-statement fix (model the prelude); item 6 is legitimate content-order with a
hardening recommendation.

---

## 5. Error / partial derivation

A CST can be **syntactically valid but semantically broken**: a dangling name-ref
(`object → material "galss"` typo), a `CanTessellate` violation, a cyclic ref, an
out-of-range value. Model B's thesis ("the UI is a live expression; agents edit
continuously") makes **derive-with-holes the default**, not fail-whole-scene:

### 5.1 Derive-with-holes

- A node that cannot derive produces a **hole**: a typed placeholder in the memo
  cache (`EntityRecord{ kind, error, derived=null }`) plus a **structured
  diagnostic** attached to the CST node (node name-path, the offending param, a
  message — exactly the per-parameter diagnostic `Job::Add*Material` already emits
  distinguishing "bound to an IPainter chunk" vs "name unknown", per the
  `IScalarPainter` refactor note in CLAUDE.md).
- **Cascade:** consumers of a hole derive to holes (an object bound to a holed
  material is itself holed and not rendered), but **independent subgraphs derive
  and render normally**. A typo in one material does not blank the scene — it blanks
  the objects using that material, and the agent/user sees a precise error on the
  exact node. This is the single biggest UX win of Model B over today's
  "one bad chunk fails the whole parse."
- Holes use the existing engine fallbacks where they exist (`"none"` material/
  painter), so a holed object can render as the null material rather than crash —
  preferable for live preview; the diagnostic still fires so it's never *silently*
  wrong.

### 5.2 Last-good scene for rendering

The memo cache holds the **last successfully-derived `DerivedScene`**. On a commit
whose diff introduces holes:

- The affected records are replaced with holes (or fallbacks); **the rest of the
  cache — and the live `Scene` — retain their last-good derived objects.** Because
  derivation is incremental and in-place via the apply-layer, an unaffected object's
  engine state is *never touched*, so it trivially survives.
- Rendering proceeds against the partially-holed scene (holes as null-material /
  not-rendered). The preview stays live; the diagnostics list shows what's broken.
- If a commit is *catastrophic* (e.g. the active-rasterizer node itself is holed),
  fall back to **rendering the last fully-valid `DerivedScene`** (kept as the prior
  cache snapshot) and surface the error — never a black frame with no explanation.

### 5.3 Distinguishing "incomplete mid-edit" from "wrong"

While a human/agent is mid-edit (debounce window), a transiently-dangling ref is
expected. Derivation is **only triggered on commit** (O2), so mid-keystroke
incompleteness never reaches `derive`. A hole at commit time is a *real* semantic
error worth surfacing. (If O2 were tightened to 60Hz incremental — see §6 — the
engine would need a "tentative hole, suppressed diagnostic" state during active
typing; flagged as the cost of that path.)

---

## 6. Hard problems & open questions

1. **Derivation latency is a named tar-pit (charter risk register).** The §2
   memo+graph design targets *typical* edits at well under the O2 debounce, but the
   worst cases are real:
   - **A param feeding a 10k-instance declarative generator** (L3: guilloché /
     instancer / sweep). One param change re-bakes the whole generated mesh +
     rebuilds the TLAS. There is no sub-generator incrementality today.
     **Mitigation:** memoize the generator's *output mesh* keyed on its input
     params (a value-only param that doesn't change the mesh — e.g. a material
     swap on the instances — skips the re-bake); for params that *do* change the
     mesh, accept the re-bake but keep it off the render thread (phase A marks
     dirty; phase B bakes once). **Open:** is per-instance incremental baking worth
     it, or is "re-bake the generator, it's one object" acceptable at 10k? Needs a
     measured baseline (per `performance-work-with-baselines`).
   - **Editing a shared painter used by 500 materials.** The painter L1 changes →
     500 dependent materials' keys *don't* change (they reference the painter by
     name/serial, and the serial only bumps on a structural rebuild). A *value-only*
     painter edit (e.g. a uniform color) should propagate via the apply-layer to
     the painter object in place, and the 500 materials see the new color **with no
     per-material work** because they hold a pointer. **This is the happy path** —
     the fan-out is O(1) at the producer, not O(consumers) — *provided* painter
     value edits are apply-able in place. **Open:** are all painter kinds in-place
     mutable, or do some require rebuild (which *would* bump the serial and force
     500 rebinds)? Audit needed; likely add painter slot-setters mirroring the
     material introspection setters.

2. **Memo-key hashing cost vs. diff-from-Facet-1.** Recomputing input-hashes for
   every node on every commit is O(scene). For a 10k-object scene that may itself
   exceed the budget. **Preferred:** Facet 1 emits a *structural diff* (changed
   node set) so Facet 2 hashes only changed nodes + their forward cone. **Open
   dependency on Facet 1:** does the CST diff give node-level granularity cheaply?
   If not, Facet 2 needs its own per-node hash cache (an extra field per CST node).

3. **TLAS rebuild is not incremental.** `PrepareForRendering` rebuilds the whole
   top-level BVH when *any* object bbox moves (`InvalidateSpatialStructure` is
   coarse). For a 155-mesh scene this is the measured 2.3× speedup structure, but a
   full rebuild per single-object drag may dominate the latency budget on large
   scenes. **Open:** is a refit / incremental-update TLAS in scope, or is full
   rebuild acceptable within O2? (Today's interactive editor already eats this on
   every transform edit, so it's a known-tolerable cost at current scene sizes —
   but Model B invites much larger scenes.)

4. **Light-sampler rebuild granularity.** L5 is a single coarse generation counter
   — *any* light-topology change rebuilds the *entire* `LightSampler` alias table
   (`RebuildLightSamplers`). Editing one of 1000 area lights rebuilds all 1000.
   The env-IBL continuous-PMF architecture (CLAUDE.md) makes this correct but not
   incremental. **Open:** incremental alias-table update vs. full rebuild — likely
   out of scope for v1 (matches today), flag for a later perf pass.

5. **`-ffast-math` and "unchanged" detection.** Memo equality must be byte/hash
   equality of *resolved values*, never a NaN-sentinel "not set" test (P-FFMATH;
   memory `ffast-math: no infinity`). Floating-point param values must hash by bit
   pattern, and "absent param" must be an explicit presence bit (the descriptor
   default), not a NaN. Mechanically enforce via the existing `SourceHygieneTest`
   pattern.

6. **Determinism vs. the constant prelude (§4.3).** The honest statement is
   `Scene = derive(prelude ⊕ CST)`. **Open for synthesis:** should the prelude
   (null defaults, default shader-ops) be an *explicit* part of the canonical
   document (visible, diff-able, agent-editable) or an *implicit* engine constant?
   The thesis ("everything is in the diff-able program") argues for explicit; UX
   argues for implicit-but-introspectable. Recommend implicit + introspectable.

7. **O2 working-assumption (debounced-commit).** §2.8 designs for debounced-commit.
   If synthesis chooses 60Hz incremental derivation instead, the deltas are: (a)
   derivation must run *without* parking the render thread (needs a lock-free
   double-buffered manager graph or a copy-on-write `Scene` — a **second
   representation**, in tension with INV-1); (b) §5.3's "tentative hole" state
   becomes mandatory; (c) the memo-key recompute must be truly O(changed), not
   O(scene), every frame. **Strong recommendation: keep debounced-commit.** 60Hz
   incremental re-derivation of a production spectral renderer's scene graph buys
   little (the render itself is the latency floor) and reintroduces exactly the
   multi-mutable-representation hazard Model B exists to kill.

---

## 7. Cross-facet dependencies & assumptions

- **From Facet 1 (CST):** I assume (a) **stable name-path identity** per node
  (INV-5) so the memo cache and managers join by name; (b) a **node-level
  structural diff** (or cheap per-node identity) so §2.4 hashes only changed nodes,
  not the whole scene (this is load-bearing for the latency budget — see §6.2); (c)
  the **descriptor `referenceCategories`** are exposed on CST nodes so I can build
  name-ref edges without a parallel schema (L6); (d) declarative iteration (L3) is
  already expanded into either *derived instances* (a generator node I treat as one
  L3 producer) or *explicit separately-editable entities* (N independent L1/L2
  nodes) — Facet 2 treats each per its kind. **Conflict flag:** if Facet 1 cannot
  cheaply give node-level diffs, §6.2's mitigation (per-CST-node hash cache) adds
  state to the CST that Facet 1 must own.
- **To/with Facet 3 (edit model):** Facet 3 deletes `SceneEdit`/`EditHistory`/
  `DirtyTracker`/transactions/`SaveEngine`; I **reuse the mutation primitives**
  under them (`ApplyObjectOpForward`, the `*Introspection` setters) as my APPLY
  backend, re-homed to take `(NamePath, slot, resolved-value)` from the CST diff.
  **Assumption:** Facet 3's "apply a CST edit" calls into my `derive(CST, CST',
  cache)` — i.e. Facet 3 owns *producing* the new CST + version history; I own
  *turning the diff into engine mutations*. The park-and-rerender gate (§2.8) is
  shared mechanism; assume Facet 3/4 drive it.
- **To Facet 4 (dynamic UI):** the memo cache's per-node derived records + holes +
  diagnostics are what the UI binds to (a widget reflects its node's derived state
  / error). I expose a read API over the cache; I assume the UI does not mutate
  derived state directly (INV-1).
- **To Facet 5 (agentic surface):** my structured per-node diagnostics (§5.1) are
  the "structured errors" the MCP `validate→derive→render` loop returns. I assume
  the agent edits land as CST diffs through Facet 3, then trigger my derive.
- **Locked-decision check:** the design honors L1 (pure derivation), L2 (one
  pathway — the apply/rebuild split is *one* derive function, not two clients), L3
  (consumes post-expansion CST), L5 (name-path + serial), L6 (descriptor-as-schema,
  no parallel edge schema). **No conflicts with locked decisions.** Open decisions
  touched: O1 (designed for CST-canonical; memo keys on *resolved values* make the
  text-canonical delta small — a text edit that re-parses to the same CST is a memo
  hit either way), O2 (designed for debounced-commit; §6.7 argues to keep it).

---

## 8. First-slice implications (minimal end-to-end vertical)

For the charter's minimal vertical — **one chunk type, text⟷CST⟷derived-scene, one
schema-generated widget, live incremental re-derive** — Facet 2 contributes:

1. **Pick `sphere_geometry` + `lambertian_material` + `standard_object`** (the
   simplest producer→producer→consumer chain that exercises a name-ref edge and a
   transform). All three already have descriptor + `Finalize` + `AddX`.
2. **Build the minimal memo cache + graph for these three node kinds:** L0 values,
   L1 entities (geometry, material), L2 object, the `material`/`geometry` name-ref
   edges (from `referenceCategories`), and the L2→L4 (TLAS) edge.
3. **Wire the two backends:** REBUILD = the existing `Finalize→AddObject`/
   `AddSphereGeometry`/`AddLambertianMaterial`; APPLY = `SetObjectPosition` +
   `RunObjectInvariantChain` for a transform edit, `MaterialIntrospection::
   SetSlotValue` for the material's `reflectance`.
4. **Demonstrate the three core behaviors:**
   - edit the sphere's `position` → APPLY path → only TLAS rebuilds (prove no
     entity re-derived; assert via the existing `GetSamplerRebuildCount` staying
     flat and a TLAS-rebuild counter ticking once);
   - edit the material's `reflectance` to a new color → APPLY path → no
     light-topology rebuild (sphere non-emissive), no TLAS rebuild;
   - rename the material the object binds (structural) → REBUILD the material
     (serial bumps) → object rebinds via the serial-staleness check.
5. **Prove the memo hit:** add a comment / reformat whitespace in the CST → diff
   shows a text change → resolved-input hash unchanged → **zero** re-derivation
   (the headline INV-3/INV-4 win, measurable as "derive did nothing").

This slice stands up the cache, the graph-build-from-descriptor, the diff→dirty→
propagate loop, and both backends — the skeleton every other chunk type plugs into
by virtue of being descriptor-driven. It reuses 100% of the engine (managers,
apply-layer, phase-B pipeline) and adds only the thin memo+graph layer.

---

### Appendix: one-paragraph mental model

Today RISE *loads* a scene whole-file and then *mutates* it in place through a
separate editor with its own record/history/dirty machinery. Model B keeps the two
existing engines — the loader's `Finalize→AddX` (now the **rebuild** backend) and
the editor's `ApplyObjectOpForward`+introspection setters (now the **apply**
backend) — and puts a **memo cache + dependency graph** in front of them. A CST
edit becomes a node-diff; the graph turns the diff into the *minimal* set of
rebuilds (structural changes) and in-place applies (value changes), then sets the
*already-existing* phase-B dirty flags (light-topology generation, TLAS-valid,
photon-pending) precisely instead of via scattered hand-bumps. The render-time
realize→TLAS→light-sampler→photon pipeline is unchanged and already correctly
staged behind the freeze guard. Determinism comes from topo-ordering the derive by
the dependency graph (fixing `Job`'s load-order landmines) and hashing *resolved
values*; incrementality comes from the memo cache; safety comes from deriving with
holes so one bad node never blanks the scene.
