# Facet 2 — Derivation Engine (CST → Scene, incremental)

> **Updated per [`01-DECISIONS.md`](01-DECISIONS.md) (review round 1).** The
> decision record is authoritative; where this doc once conflicted it now conforms
> and points to the relevant decision. The three decisions that reshaped this facet:
> **D1** — the derived scene is an *immutable COW snapshot* the renderer swaps at a
> tile/pass boundary (no parking-for-safety); **D4** — dependency edges are *traced
> during derivation* and the memo key includes traced-input versions; **D5** — the
> formal input is *(CST, AssetManifest)*, not CST alone. See also D9 (NodeId is the
> lineage identity; name-path is addressing).
>
> **Status:** design-in-progress. One facet of the [Agentic Redesign](00-CHARTER.md).
> Read the [charter](00-CHARTER.md) and [`01-DECISIONS.md`](01-DECISIONS.md) first —
> this doc assumes the charter's locked decisions (L1–L7), invariants (INV-1…6), the
> canonical-CST pivot, and the round-1 decisions (D1–D10). **Design only; no
> code/build/scene changes.**
>
> **This facet owns:** the function `DerivedScene = derive(CST, AssetManifest)`
> (D5) — made *pure*, *deterministic*, *incremental*, and *memoized*, producing an
> **immutable snapshot via copy-on-write** (D1); the **traced** CST→engine
> dependency graph (D4); memo granularity; the derive/apply seam (reuse of the
> surviving apply-layer **on COW copies**); interaction with deferred realization /
> TLAS / photon passes; the order-independence audit of `Job`; perf targets;
> partial/error derivation.
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
derive : (CST, AssetManifest)  →  DerivedScene            (pure, deterministic — INV-2, D5)
derive : (CST, CST', AssetManifest, DerivationCache)
              →  (DerivedScene', DerivationCache')         (incremental — INV-3)
```

The formal input is **`(CST, AssetManifest)`** (D5), not the CST alone:
`Scene = f(CST)` is false when a referenced texture/mesh/spectral/glTF changes on
disk without the CST or filename changing. The **`AssetManifest`** maps each
referenced asset path → `{resolved absolute identity, content fingerprint}`
(size+mtime, upgradable to a content hash); **output paths are excluded** (sinks,
not sources — §2.9). It is part of the derivation environment, versioned alongside
the CST head, and refreshed by a file watcher / re-stat (D5, §2.9).

`DerivedScene` is **an immutable snapshot (D1)** of today's render-ready state: the
`Scene` + its populated managers + realized geometry + TLAS + light samplers +
(conditionally) photon maps — but produced via **copy-on-write with structural
sharing** rather than mutated in place. A new CST/manifest version derives a **new
snapshot**: only the changed subgraph's engine objects are re-derived (reusing the
apply-layer on *copies*); unchanged objects (meshes, BVH leaves, materials, the
TLAS where untouched, light samplers where untouched) are **shared by reference**
with the prior snapshot. The renderer holds a **refcounted pointer to one
snapshot** and **atomically swaps it at a tile/pass boundary** (§2.8). The
incremental form takes the previous CST, the new CST, the manifest, and the memo
cache, and re-derives **only the affected subgraph**. There is **one**
implementation: the full derive is the incremental derive against an empty cache.
(P-WALK: no second path.)

Determinism (INV-2) means: the result is a pure function of the (CST, manifest)
*content*, independent of node *visit order*. §4 audits where today's `Job`
violates this and what must change.

**Why COW, not mutate-in-place (D1, resolves the F2↔F3 split):** an earlier draft
of this facet mutated one long-lived `Job`/`Scene` in place and *parked the
renderer for safety*. D1 unifies F2's incremental apply-layer reuse with F3's
immutable no-park model: the apply-layer **is** reused, but on COW copies, so
immutability and structural sharing both hold. Immutability removes the data race
outright, so **parking-for-safety is gone**; cancel-and-park survives only as an
optional latency/coalescing optimization (§2.8). This is the red-green discipline
(D2) extended from the CST to the derived scene: a new snapshot is a path-copy of
the changed spine over a shared tail.

### 2.2 The dependency / dataflow graph (traced — D4)

The derivation is a DAG from **CST nodes** (chunks + their parameter values, and
the inline/expression sub-nodes Facet 1 defines) **and AssetManifest entries** to
**derived engine objects**. **The edges are not pre-computed from a static schema —
they are *recorded as a by-product of derivation* (D4).** When `derive()` resolves
a name, reads another node, evaluates an expression, or loads an asset, it
**records the edge it traversed** (the reactive-build / "tracing" model). Nodes and
the edges that carry invalidation:

```
              ┌─────────── CST node (chunk) ───────────┐
              │  e.g.  ggx_material { name=glass ...}   │
              └───────────────┬────────────────────────┘
   value edges (params,       │ name-ref edges RECORDED when derive resolves the name
    incl. expr(...) inputs    │   (traced — D4; not read from a static schema)
    traced by D4)             ▼
                         DerivedEntity  (a manager entry: IMaterial* "glass", version V)
                              │
   AssetManifest entry ──────►│  (asset edge RECORDED when derive loads the file — D5)
   {identity, fingerprint}    │
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
| L0 *value* | a resolved parameter value (literal / evaluated `expr(...)` / inline sub-chunk) | Facet-1 evaluator | its source text changes, OR any **traced** expression input's version changes (D4) |
| L1 *entity* | a manager entry (painter, material, geometry, shader, shaderop, modifier, function, camera, medium, light) | `Finalize → pJob.AddX` | any L0 value it consumes changes, any **traced** name-ref target's *version* changes (§2.6), OR a **traced** AssetManifest fingerprint changes (D5) |
| L2 *object* | an `IObject` (geometry+material+modifier+shader+transform bind) | `AddObject`/`AddObjectMatrix`/`AddCSGObject` | any bound L1 entity rebinds, or its transform params change |
| L3 *realized geom* | baked mesh for a deferred geometry (displaced/SDF/CSG/bezier) | `obj.Realize()` cascade (phase B) | its source geometry entity or any tessellation-driving param changes |
| L4 *TLAS* | top-level BVH over objects | `PrepareForRendering` (phase B) | any L2 object's **world bbox** changes (transform or geometry swap) |
| L5 *light topology* | `LightSampler` + `EnvironmentSampler` | `RebuildLightSamplers` (phase B) | light add/remove, emissive-material bind/unbind, exitance edit, **spatial change of an emissive object**, env map replace/scale |
| L6 *photon maps* | caustic/global/shadow/translucent maps | `BuildPendingPhotonMaps` (phase B) | any L2/L3/L5 change **and** the active rasterizer consumes them |

**All four edge families above are recorded by *tracing the derivation* (D4)** —
none is statically pre-computed. The reason D4 mandates tracing over a static
schema: a previous draft claimed the name-ref edges were derivable from the
descriptor's `referenceCategories` (plus three engine relations the schema doesn't
express). That is wrong for **dynamic references** — `timeline.element` is a plain
string whose target *category* is chosen at derive time by the sibling
`element_type` value (it can resolve to geometry, a material, a light, …), and
`timeline.animation` likewise. A static `referenceCategories` lookup is blind to
which category a given `element` actually resolves to. Because `derive()` *performs*
the resolution, the edge it records is the **actual** one taken this version —
dynamic refs are captured automatically and for free.

`referenceCategories` is therefore **demoted to a UI/rename *hint*** (D4): it drives
ref-pickers in the dynamic UI and rename's referrer search — it is **not** the
source of truth for the dependency graph. (No parallel schema is invented; the hint
and the traced graph are different jobs, per L6 / INV.)

The four engine-level relations the trace records as graph edges:

1. **name-ref → producer** (L1↔L1, L2→L1). Recorded when `derive` resolves a
   parameter's name through a manager (`GetMaterial`/`GetGeometry`/…). Captures both
   static refs (`standard_object.material`) and dynamic ones
   (`timeline.element` via `element_type`, `timeline.animation`).
2. **emissive-material → light-topology** (L1→L5). Recorded when `derive` observes a
   material is emissive (today's `BumpSceneLightGenerationIfMaterialEmits` /
   `…IfEmitterSetChanged` become consequences of this traced edge): an edit that
   flips is-emissive, or edits an emissive slot, marks L5.
3. **object-spatial → {TLAS, light-topology-if-emissive}** (L2→L4, L2→L5). Recorded
   for any object whose transform/geometry feeds a world bbox (today's
   `RunObjectInvariantChain` + `OpNeedsSpatialRebuild`, `SceneEdit.h:391`).
4. **rasterizer → photon-pass gate** (active-rasterizer → L6). Recorded from the
   `ConsumesScenePhotonMaps()` observation. In Model B the active-rasterizer CST
   node is a traced input to whether L6 is part of the derived snapshot at all
   (§2.9).

Plus the **asset edges** (D5): recorded when `derive` loads a file referenced by a
param, linking the consuming L1/L3 node to that AssetManifest entry's fingerprint
(§2.9).

### 2.3 Memoization keys & granularity

**Granularity: per-CST-node, at the chunk level for entities (L1) and per-object
for L2.** Not per-subtree (too coarse — a one-param edit to one of 200 objects
would re-derive the subtree) and not per-token (too fine — sub-chunk inline values
are L0 and roll up into their owning chunk's L1 key).

**Key = (the node's green-node structural hash) + (the identities & versions of
every *traced* input), not its text span (D4).** For an L1 entity node:

```
key(entity) = H( green_node_structural_hash(node),          // structure: keyword + param shape (D2)
                 { (param_name, resolved_value) for each literal param },
                 { (input_NodeId, input_version) for each TRACED CST input },   // exprs + name-refs (D4)
                 { (asset_path, fingerprint)     for each TRACED asset input } )  // D5
```

Including **traced-input identities/versions** — not just the resolved value — is
the D4 correctness requirement. It means **`expr(A)` ≠ literal `5` even when both
currently evaluate to 5**: `expr(A)` keys on "structure = expr + input `A@version`",
while literal `5` keys on "structure = literal 5, no inputs". When `A` changes,
`expr(A)`'s key changes (its input version bumped) and it re-derives; the literal
`5` stays a memo hit. **Equal *current values* never collapse unequal *dependency
identities*** — the earlier "`$(2+3)` and `5` hash identically, refactoring to the
same value is a no-op" claim is *wrong* and is replaced by this rule (it would have
hidden the differing future-invalidation dependency).

The structural-hash component still gives INV-4 for free in the other direction: a
whitespace-only or comment-only edit (which Facet 1's CST preserves losslessly)
changes the text span but **not** the green-node structural hash and touches no
traced input → memo hit → zero re-derivation. Likewise "typed the same literal value
back" is a hit. What is *not* a hit is a value that arrived through a different
traced dependency — that is the point.

The memo table (keyed and indexed by **`NodeId`** — D9 — with name-path resolved to
NodeId per version):

```cpp
struct DerivationCache {
    // NodeId  →  derived record (the engine object + its key + its version)
    std::unordered_map<NodeId, EntityRecord> entities;   // L1
    std::unordered_map<NodeId, ObjectRecord> objects;    // L2
    // Reverse dependency index, populated by TRACING (D4): which derived records
    // depend on a given producer / asset, so an invalidation pushes forward in
    // O(out-degree), not O(scene). Producers are NodeIds; asset producers are
    // AssetManifest keys.
    std::unordered_multimap<NodeId, NodeId> dependents;        // producer node → consumers
    std::unordered_multimap<AssetKey, NodeId> assetDependents; // asset → consumers (D5)
    // Phase-B coarse generations (mirror the existing engine counters):
    uint64_t builtLightTopologyGeneration; // == Scene::GetLightTopologyGeneration() at last L5 build
    bool     tlasValid;                     // false ⇒ PrepareForRendering re-run next render
    PhotonMapGeneration photonGen;          // bumped by any L2/L3/L5 change
};

struct EntityRecord {
    InputHash      key;         // §2.3 structural-hash + traced-input-version key
    uint64_t       version;     // manager identity version at build time (§2.6)
    EntityKind     kind;
    bool           isEmissive;  // drives the L1→L5 edge
    // (the engine object itself lives in the snapshot's manager, shared by reference
    //  with the prior snapshot when this record is a memo hit — D1 structural sharing)
};
```

**Identity (D9):** the cache keys on the immutable, process-stable **`NodeId`** (the
green-node-borne lineage token), not on the name-path. **name-path**
(`objects/sphere`, `materials/glass`, `materials/glass.reflectance` for a slot) is
the human/agent *addressing* scheme, resolved to a `NodeId` *within a given
version*; it changes on rename (by design). A rename is a `NodeId`-preserving edit,
so cache records and UI/agent bindings survive it (D9). The `GenericManager` entry
is still name-keyed (that is how the engine stores it), but the *derivation*'s
stable handle is the NodeId, and the manager's identity counter (§2.6) is the
freshness token; the two are orthogonal and both already exist.

### 2.4 How a localized edit re-derives only the affected subgraph (INV-3)

Given `(CST, CST', AssetManifest, cache)`:

1. **Seed the dirty set** from two sources: (a) the **CST diff** CST→CST′ at node
   granularity (Facet 1 supplies the structural diff, or we recompute keys per node
   and compare to `cache`) → dirty nodes {added, removed, changed}; (b) any
   **AssetManifest fingerprint change** (D5) → seed every consumer via
   `assetDependents`. The manifest delta is produced by the watcher / re-stat
   (§2.9).
2. **Recompute keys** for dirty nodes' L1/L2 records. For each, compare new
   `key` to the cached `key`:
   - **equal** → memo hit, skip (covers comment/whitespace edits and "typed the
     same value back" — the same short-circuit `Job::SetFilm` already does manually
     at `Job.cpp:489`). Note: an edit that changes *which traced input* a node
     reads (e.g. swapping `expr(A)` for `expr(B)`, or `5` for `expr(A)`) is **not**
     a hit even if the current value is unchanged — the traced-input identities in
     the key differ (D4).
   - **changed** → re-run that node's `Finalize`-equivalent **via the apply-layer
     where possible** (§3, on a COW copy — D1), else rebuild that one entity.
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
4. **Publish the new snapshot, then phase-B reconciliation at next render.** The
   COW re-derive produces a **new immutable snapshot** (D1) that shares all
   untouched engine objects by reference with the prior one. The renderer's
   refcounted snapshot pointer is **atomically swapped at a tile/pass boundary**
   (§2.8); the old snapshot drains by refcount. The phase-B machinery itself is
   *unchanged from today* and needs no new code: `AttachScene` (running over the new
   snapshot) (a) re-walks `Realize()` idempotently — only the re-marked L3
   geometries do work; (b) rebuilds light samplers iff
   `liveGen != builtLightGeneration`; `PrepareForRendering` rebuilds the TLAS;
   `BuildPendingPhotonMaps` re-shoots iff pending+consumed. **Facet 2's job is to
   set those existing dirty flags precisely, from the traced graph, instead of from
   scattered hand-bumps** — and to ensure phase-B work mutates the *new snapshot's
   copies*, never an object shared with a snapshot a render thread may still hold.
   This is the deepest reuse in the design: the entire phase-B incremental machinery
   already exists and is correct; we feed it a graph-derived dirty set and let
   structural sharing keep the snapshot cheap.

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

Two backends produce L1/L2 records; the **decision rule** picks per dirty node.
**Both backends operate on the new snapshot's COW copies (D1)** — REBUILD adds into
the new snapshot's manager; APPLY mutates a *copied* engine object, never one shared
(by reference) with the prior snapshot:

```
For a CHANGED node N:
  if N is structural  (added node, removed node, or a change that alters the
                       node's TYPE / its ref-edge SHAPE — e.g. material kind
                       changed, a new param appeared that adds a dependency,
                       composed-material slot, geometry kind changed)
        → REBUILD: run the full Finalize-equivalent → AddX into the NEW snapshot's
                   manager (remove the old entry first; this bumps the identity
                   version, which §2.6 propagates to dependents).
  else if N is a VALUE-ONLY change to a slot the apply-layer can mutate
       (object transform / material slot / light prop / medium prop / camera prop /
        object material|shader|geometry|medium REBIND)
        → APPLY: COW-copy the target engine object into the new snapshot, then call
                 the surviving mutator (ApplyObjectOpForward / the Introspection
                 SetSlotValue setters) + its invariant chain ON THE COPY.
  else → REBUILD (safe default).
```

**Why this split.** The apply-layer (§1.4) is *precisely* a set of fine-grained
mutations the engine already supports. Under Model A they ran in place between
passes; under D1 they run **on a COW copy** that becomes part of the new immutable
snapshot — same primitive, same invariant chain, but no shared object is mutated, so
the prior snapshot a render thread may still hold is never disturbed. Reusing the
apply-layer for value-only edits gives the cheapest possible re-derive (copy one
object, no manager churn, no version bump, no dependent cascade beyond the known
invariant edges) and inherits all the hard-won correctness (the invariant chain, the
conditional light-gen bumps). Structural edits *must* rebuild because the apply-layer
has no "change a material's *type*" or "add a dependency edge" operation — and trying
to add one would re-introduce P-WALK (a second mutation vocabulary that drifts from
the loader). **The loader's `Finalize → AddX` IS the rebuild path; the apply-layer
(on copies) IS the fast path; there is no third path.**

This also resolves a charter tension cleanly: Facet 3 deletes the `SceneEdit`
*record* and the history/dirty wrapper, but the **mutation primitives** under
`ApplyObjectOpForward` and the `*Introspection` setters survive and are *re-homed*
under Facet 2 as the "APPLY backend." The carrier changes from a `SceneEdit` value
to a `(NodeId, slot, resolved-value)` derived from the CST diff (addressed by
name-path, keyed by NodeId — D9); the primitive it calls is the same — now invoked
on a COW copy rather than the shared live object.

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

### 2.6 Identity & the traced name-ref edge (INV-5, D4, D9)

Three identity concepts, kept distinct per D9 + D4:

- **`NodeId`** (D9) — the immutable, process-stable green-node lineage token. The
  memo cache keys on it; it survives rename, value edits, and reparse (reparse
  matches new green nodes to prior NodeIds by structural position + content). This is
  what durable UI/agent bindings and undo lineage key on.
- **name-path** (D9) — the human/agent *addressing* scheme, resolved to a `NodeId`
  per version; changes on rename by design.
- **manager identity version** (the `GenericManager` identity counter,
  `GenericManager.h:62`, historically "serial") — the **traced name-ref edge's
  freshness token** for derivation.

Two derived entities can share a name across a remove+re-add that changes the
*thing* under the name. The interactive editor already guards this with
`capturedTargetSerial` (`SceneEditor.cpp:1370`). Facet 2 adopts the manager version
as the freshness token on the **traced** name-ref edges (D4):

- When `derive` resolves a name-ref, it **records the edge** (D4) and stores the
  resolved target's manager version at build time.
- When a producer is REBUILT (structural change), its manager version bumps.
- A consumer whose stored target-version ≠ the live version is **stale** even if its
  own params are unchanged → it must re-resolve the binding (cheap: rebind via the
  apply-layer on a COW copy) or rebuild. This is how "material `glass` was replaced
  by a different-kind material under the same name" correctly invalidates every
  object bound to `glass`, without those objects' own CST nodes having changed — and
  the consumers are reached via the traced `dependents` index, not a static schema
  walk.

`NodeId` is the *stable* identity for UI/agent/selection/cache; name-path is the
*addressing* over it; the manager version is the *freshness* token for derivation.
The three are orthogonal and all already exist (or, for NodeId, are introduced by
D9/Facet 1).

### 2.7 Interaction with realization, `CanTessellate`, the freeze guard, photons

- **Deferred realization (phase B):** Facet 2 does **not** realize during the
  assembly phase (A). It marks L3 nodes dirty; the existing single-threaded realize
  pass in `AttachScene` bakes them — into the **new snapshot's** copies — before the
  freeze bracket. The freeze guard (`RenderParallelScope`, `g_renderParallelDepth`)
  is honored *for free* because the in-flight render reads its own immutable snapshot
  (D1) and the re-derive only ever mutates COW copies in the *new* snapshot — it
  never touches an object the running workers hold. **Re-derive therefore does NOT
  require parking the render for safety** (D1): immutability removes the data race
  that the old "assert `g_renderParallelDepth == 0`" gate guarded against. The freeze
  guard remains a useful DEBUG assertion that no `Realize()` runs *on the live
  snapshot* mid-render, but it is no longer a correctness prerequisite for starting a
  re-derive (see §2.8 for the optional cancel-and-park latency optimization).
- **`CanTessellate` / parse-refusal:** a `displaced_geometry` over a
  non-tessellatable base (e.g. `InfinitePlaneGeometry`) must fail derivation, not
  render black. Today `AddDisplacedGeometry` returns false and the whole parse
  fails. In Model B this is a **node-local derivation error** (§5): the L1
  geometry node derives to a *hole*, the L2 objects binding it derive to holes,
  and the rest of the scene derives normally. The error attaches to the offending
  CST node (Facet 5 surfaces it). `CanTessellate` is checked at derive time (phase
  A), not realize time, so the error is reported before any render is attempted.
- **Photon-pass gating:** L6 is part of the derived snapshot *iff* the active
  rasterizer's `ConsumesScenePhotonMaps()` is true. The active-rasterizer CST node is
  a **traced input** (D4) to that decision. Changing it from `pixelpel_rasterizer`
  (consumes) to `pathtracing_pel_rasterizer` (doesn't) **removes L6 from the graph**
  — no photon shoot, and any pending photon dirtiness is irrelevant. Changing back
  re-adds L6 and marks it pending. This makes the existing runtime gate a
  *graph-membership* decision recorded by tracing.

### 2.8 Threading: snapshot swap, not parking (D1)

Derivation runs on the **commit** of a debounced CST edit (O2), on the UI/agent
thread. It produces a **new immutable `DerivedScene` snapshot** via copy-on-write
(D1, §2.1) and **publishes it by an atomic pointer swap at a tile/pass boundary** —
it does **not** park the render thread for safety.

**The model (D1):**
- The renderer holds a **refcounted pointer to one immutable snapshot** and reads it
  freely; because the snapshot is immutable, no lock is needed on the read side.
- A commit/preview builds a new snapshot (sharing all untouched engine objects by
  reference — meshes, BVH leaves, materials, the TLAS/light samplers where untouched)
  and **atomically swaps the renderer's pointer at a tile/pass boundary.** The render
  loop checks the published pointer at that boundary; the in-flight pass finishes
  against the *old* snapshot, which stays alive on its refcount until it drains, then
  is freed.
- **No parking-for-safety.** Immutability removes the data race that the Model-A /
  earlier-draft `SceneEditController` park gate guarded against (no thread reads an
  object another thread mutates — re-derive only ever writes COW copies in the new
  snapshot). The previous version of this section parked the renderer
  (`mCancelProgress.RequestCancel()` → `mCV.wait(!mRendering)` → mutate →
  `notify_one`) as a *correctness* requirement; under D1 that is removed.
- **Cancel-and-park survives only as an OPTIONAL latency / coalescing optimization**
  (D1): for a burst of rapid edits (a gesture, an agent emitting many patches) it can
  be cheaper to cancel the in-flight pass and coalesce, rather than swap a snapshot
  the next debounce will immediately supersede. This is a latency choice, never a
  correctness one; if omitted, the worst case is one wasted partial pass.

**Last-good snapshot** is trivially the most recent snapshot that derived without a
hard error (D1): it is immutable + refcounted, so the renderer simply keeps
rendering it while a broken head is edited (pairs with derive-with-holes, §5).
**Abort** of an in-flight pass needs no rollback — nothing was half-mutated.

The re-kick that follows a swap is the same `RasterizeScene`, now running against the
newly published snapshot. **No second *mutable* representation is introduced**
(INV-1): the memo cache is derived state, not an independent source of truth — it can
be dropped and rebuilt from `(CST, AssetManifest)` at any time; the snapshots are
immutable values, not a mutable mirror of the CST.

### 2.9 The AssetManifest and external-input invalidation (D5)

The second formal input to `derive` is the **`AssetManifest`** (D5): a map from each
**referenced asset path** → `{resolved absolute identity, content fingerprint}`,
where the fingerprint is `size+mtime` (upgradable to a content hash). It covers every
externally-loaded asset a chunk references — textures (`png_painter`/image painters),
meshes (`bezier`/mesh loaders), spectral data (`scalar_painter { file … }`), glTF,
environment maps, `.sdf` sidecars (where still used), etc.

- **Asset reads are traced (D4).** When `derive` loads a file for a param, it records
  an edge from the consuming L1/L3 node to that manifest entry (the
  `assetDependents` index, §2.3). A fingerprint change therefore invalidates exactly
  the nodes that consumed that asset — `Scene = f(CST)` becomes
  `Scene = f(CST, AssetManifest)`, closing the "texture changed on disk, clean derive
  disagrees with a cache hit" hole.
- **Output paths are excluded** (D5): `file_rasterizeroutput.pattern` and any sink
  path are **sinks, not sources** — they never appear in the input dependency set and
  never trigger invalidation.
- **Invalidation is watcher-driven** (D5): a background file watcher (or an explicit
  re-stat on focus / before render) refreshes the manifest. A changed fingerprint
  seeds the dirty set (§2.4 step 1b) and re-derives the consumers. The manifest is
  versioned alongside the CST head and is part of the derivation environment.
- This is the same mechanism Facet 1/3 use for the **external-file conflict guard**
  (D6: load/flush fingerprint + compare-and-swap save); the manifest's fingerprints
  and the save-time CAS share the watcher and the fingerprint definition.

---

## 3. Delete / Evolve / Reuse

| Component (file) | Fate | Rationale |
|---|---|---|
| `AsciiSceneParser::ParseAndLoadScene` whole-file loader (`AsciiSceneParser.cpp:10431`) | **Evolve** | Becomes the *full-derive special case* (incremental derive vs empty cache). The chunk-walk + `Finalize→AddX` is retained as the **REBUILD backend**, driven from the CST instead of re-tokenizing text. The `FOR`/`DEFINE`/`hal`/`$(...)` macro front-end is removed by Facet 1 (L3); Facet 2 consumes the post-evaluation CST. |
| `IAsciiChunkParser::Finalize` per chunk | **Reuse (re-homed)** | This *is* the per-node L1 derivation rule. Keep verbatim as the rebuild emitter; it now runs per dirty node, not per file. |
| `IJob::AddX/SetX` surface + `Job.cpp` | **Reuse, with order-independence fixes** | The construction API stays; §4 lists the specific ordering effects to remove. |
| `GenericManager<T>` (name-key + identity version/serial) | **Reuse** | Already the order-independent store + freshness token Facet 2 needs (§2.6). |
| `SceneEditor::ApplyObjectOpForward` / `ApplyForwardMutation` (`SceneEditor.cpp`) | **Reuse (re-homed as the APPLY backend)** | The fine-grained mutators for value-only edits — now invoked **on COW copies in the new snapshot** (D1), not on a shared live object. Strip the `SceneEdit`-record coupling; drive from `(NodeId, slot, value)` (D9). |
| `*Introspection::SetSlotValue` (Material/Media/Light/Camera) | **Reuse** | The per-domain slot setters the APPLY backend calls (on COW copies — D1). |
| `BumpLightTopologyGeneration` + the `…IfEmitterSetChanged`/`…IfMaterialEmits` helpers + the `LightManager` self-invalidate callback | **Reuse (as the L1/L2→L5 traced-edge implementation)** | The emissive→light-topology dependency edge already exists imperatively; Facet 2 makes the **traced** graph (D4) drive it. The coarse generation counter stays as L5's dirty flag. |
| Phase-B reconciliation (`RayCaster::AttachScene` realize+sampler rebuild, `PrepareForRendering`, `BuildPendingPhotonMaps`, `RenderParallelScope`) | **Reuse, unchanged** | Already incremental + correctly staged. Facet 2 only feeds it a precise dirty set and runs it over the **new snapshot** (D1). Under D1 the freeze guard is a DEBUG assertion, not a parking prerequisite (§2.7–2.8). |
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
   **Fix (two layers):** (a) Facet 2 derives in **dependency order regardless of CST
   order**, but the order falls out of **tracing (D4), not a statically pre-built
   `referenceCategories` graph.** Derivation resolves each node's name-refs *by
   performing the resolution*; when a consumer references a producer not yet derived,
   it derives the producer first (lazy/on-demand, memoized) and **records the traced
   edge** — equivalently a worklist that defers a consumer until its referenced
   producers are present. This captures dynamic refs (`timeline.element` via
   `element_type`) that a static `referenceCategories` scan would miss.
   (`referenceCategories` is only a UI/rename hint — D4.) (b) Dangling refs (a name
   with no producer node) derive to a **hole** (§5) rather than failing the whole
   scene. Cycles in name-refs (illegal) are detected when tracing revisits a node
   already on the active resolution stack, and reported as node-local errors.

3. **Default shader-ops / null defaults are injected imperatively in
   `InitializeContainers`.** `Job.cpp:347-373` adds `"none"` material/painter and
   `DefaultReflection`/`DefaultDirectLighting`/`DefaultPathTracing`/… **before** any
   scene content. These are fine (they're constant, position-independent), but they
   mean the manager contents are *not* a pure function of the CST alone — they're
   `CST + a fixed prelude`. **Fix:** model the prelude as an implicit, constant
   "prelude CST" prepended to every document (or a fixed set of synthetic root
   nodes). `derive` stays pure: `Scene = derive(prelude ⊕ CST, AssetManifest)` (D5).
   No behavior change; it just makes the determinism statement honest.

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

### 5.2 Last-good snapshot for rendering (D1)

The renderer retains the **last successfully-derived immutable snapshot** (D1, §2.8)
— refcounted, so it stays alive for free. On a commit whose diff introduces holes:

- The new snapshot is a COW path-copy: the affected records become holes (or
  fallbacks), while **every unaffected engine object is shared by reference with the
  prior snapshot** (D1 structural sharing). An unaffected object trivially survives
  because the new snapshot *points at the same object* — there is no in-place mutation
  to disturb it.
- Rendering proceeds against the partially-holed new snapshot (holes as null-material
  / not-rendered). The preview stays live; the diagnostics list shows what's broken.
- If a commit is *catastrophic* (e.g. the active-rasterizer node itself is holed),
  **do not swap** — keep rendering the retained **last-good snapshot** (the most
  recent that derived without a hard error, §2.8) and surface the error. Because that
  snapshot is immutable and refcounted, this is just "don't publish the new pointer."
  Never a black frame with no explanation.

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
     name, and the manager version only bumps on a structural rebuild — §2.6). A
     *value-only* painter edit (e.g. a uniform color) should propagate via the
     apply-layer to a **COW copy of the painter object** (D1), and the 500 materials
     in the new snapshot continue to point at that (now-updated) painter **with no
     per-material work**. **This is the happy path** — the fan-out is O(1) at the
     producer, not O(consumers) — *provided* painter value edits are apply-able.
     (Note the COW nuance: the 500 materials' *pointers* are unchanged, so the
     materials themselves need not be copied; only the painter is copied, and the
     materials in the shared tail still resolve it by name within the snapshot.)
     **Open:** are all painter kinds mutable via a copy-then-set, or do some require
     a full rebuild (which *would* bump the version and force 500 rebinds)? Audit
     needed; likely add painter slot-setters mirroring the material introspection
     setters.

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
   equality of the §2.3 key (green-node structural hash + traced-input versions +
   *resolved literal-value* bit patterns), never a NaN-sentinel "not set" test
   (P-FFMATH; memory `ffast-math: no infinity`). The literal-value component of the
   key must hash floating-point params **by bit pattern**, and "absent param" must be
   an explicit presence bit (the descriptor default), not a NaN. Mechanically enforce
   via the existing `SourceHygieneTest` pattern.

6. **Determinism vs. the constant prelude (§4.3).** The honest statement is
   `Scene = derive(prelude ⊕ CST, AssetManifest)` (D5). **Open for synthesis:**
   should the prelude (null defaults, default shader-ops) be an *explicit* part of
   the canonical document (visible, diff-able, agent-editable) or an *implicit*
   engine constant? The thesis ("everything is in the diff-able program") argues for
   explicit; UX argues for implicit-but-introspectable. Recommend implicit +
   introspectable.

7. **O2 working-assumption (debounced-commit).** §2.8 designs for debounced-commit.
   Note that **D1 already removes the old "60Hz needs no-park, which needs a second
   representation" tension**: the no-park snapshot-swap is now the *baseline* model
   at any cadence, and an **immutable COW snapshot is explicitly NOT the
   "second mutable representation" hazard** INV-1 forbids — it is an immutable value
   with structural sharing, derivable from `(CST, AssetManifest)` and droppable at
   will, not a mutable mirror of the CST that must be kept in sync. So the remaining
   deltas if synthesis chose 60Hz incremental derivation are narrower: (a) §5.3's
   "tentative hole, suppressed diagnostic" state becomes mandatory during active
   typing; (b) the memo-key recompute must be truly O(changed), not O(scene), every
   frame; (c) snapshot allocation/refcount churn per frame must stay cheap (the
   structural sharing makes each snapshot O(depth), but 60Hz × large scenes still
   wants measurement). **Recommendation: keep debounced-commit** — 60Hz re-derivation
   of a production spectral renderer's scene graph buys little (the render itself is
   the latency floor). This is now a *latency/throughput* preference, not a
   correctness/INV-1 argument, since D1's snapshot model is safe at either cadence.

---

## 7. Cross-facet dependencies & assumptions

- **From Facet 1 (CST):** I assume (a) **immutable, process-stable `NodeId`
  identity** per node (D9) that survives rename/value-edit/reparse — the memo cache
  keys on it; **name-path** (resolved to NodeId per version) is the addressing layer,
  and the manager entry is still name-keyed (D9, §2.6); (b) a **node-level structural
  diff** (or cheap per-node identity) so §2.4 hashes only changed nodes, not the
  whole scene (load-bearing for the latency budget — see §6.2); (c) **green-node
  structural hashing** is available (D2) for the memo key's structure component, and
  the descriptor's `referenceCategories` is exposed **as a UI/rename hint only** (D4)
  — I do **not** build the dependency graph from it; the graph is **traced during
  derivation** (D4); (d) the **AssetManifest** (D5) — resolved identity + fingerprint
  per referenced asset — is supplied as a second derivation input (Facet 1/6 own its
  construction + the watcher); (e) declarative iteration (L3) is already expanded into
  either *derived instances* (a generator node I treat as one L3 producer) or
  *explicit separately-editable entities* (N independent L1/L2 nodes) — Facet 2 treats
  each per its kind. **Conflict flag:** if Facet 1 cannot cheaply give node-level
  diffs, §6.2's mitigation (per-CST-node hash cache) adds state to the CST that Facet
  1 must own.
- **To/with Facet 3 (edit model):** Facet 3 deletes `SceneEdit`/`EditHistory`/
  `DirtyTracker`/transactions/`SaveEngine`; I **reuse the mutation primitives**
  under them (`ApplyObjectOpForward`, the `*Introspection` setters) as my APPLY
  backend, re-homed to take `(NodeId, slot, resolved-value)` from the CST diff (D9)
  and to run **on COW copies in the new snapshot** (D1). **Assumption:** Facet 3's
  "apply a CST edit" calls into my `derive(CST, CST', AssetManifest, cache)` (D5) —
  i.e. Facet 3 owns *producing* the new CST + version history; I own *turning the diff
  into a new immutable snapshot*. The **snapshot-publish / atomic-swap** seam (§2.8,
  D1) is shared mechanism; assume Facet 3/4 drive the commit. (No parking-for-safety;
  cancel-and-park is an optional coalescing optimization Facet 3/4 may invoke.)
- **To Facet 4 (dynamic UI):** the memo cache's per-node derived records + holes +
  diagnostics are what the UI binds to (a widget, bound by NodeId per D9, reflects its
  node's derived state / error). I expose a read API over the cache; I assume the UI
  does not mutate derived state directly (INV-1) — it edits the CST, which re-derives a
  new snapshot.
- **To Facet 5 (agentic surface):** my structured per-node diagnostics (§5.1) are
  the "structured errors" the MCP `validate→derive→render` loop returns. I assume
  the agent edits land as CST diffs through Facet 3, then trigger my derive.
- **Decision-conformance check (D1–D10):** the design honors **D1** (immutable COW
  snapshot + atomic swap; no parking-for-safety — §2.1/§2.8), **D4** (traced
  dependency graph; memo key = green-node structural hash + traced-input versions, so
  `expr(A)` ≠ literal `5` — §2.2/§2.3), **D5** (input = `(CST, AssetManifest)`; output
  paths excluded — §2.9), **D9** (NodeId = lineage identity, name-path = addressing —
  §2.3/§2.6), and references **D2** (green-node hashing), **D6** (shared fingerprint /
  CAS-save mechanism — §2.9), **D10** (the first-slice fixture/gates — §8). It honors
  the charter's locked decisions L1 (pure derivation), L2 (one pathway — the
  apply/rebuild split is *one* derive function, not two clients), L3 (consumes
  post-expansion CST), L5 (identity is first-class — realized as NodeId + name-path per
  D9), L6 (descriptor-as-schema; `referenceCategories` is a hint, not a parallel edge
  schema). **No conflicts with D1–D10 or the locked decisions.** Open decisions
  touched: O1 (designed for CST-canonical; the memo key's structural-hash + traced-
  input-version form makes the text-canonical delta small — a text edit that re-parses
  to the same green tree with the same traced inputs is a memo hit either way), O2
  (designed for debounced-commit; §6.7 argues to keep it, now as a latency preference
  since D1 is safe at either cadence).

---

## 8. First-slice implications (minimal end-to-end vertical)

Facet 2 contributes to the **single canonical phased fixture + shared gate set in
D10** (which supersedes the four formerly-nominated first slices). The relevant
fixture phases for this facet are D10's phase 3 (`sphere_geometry` +
`uniformcolor_painter` + `standard_object` — the geom+material+object three-node
chain, the first to exercise cross-node references, rename integrity, and the
dependency graph end-to-end), with phase 4 (`+ expr(...)`) exercising traced-input
invalidation and phase 5 (`+ instance_array`) the generator path. Facet 2's
contributions to that fixture:

1. **Take D10 phase 3's three-node chain** (`sphere_geometry` →
   `uniformcolor_painter` → `standard_object`) — the simplest
   producer→producer→consumer chain that exercises a traced name-ref edge and a
   transform. All three already have descriptor + `Finalize` + `AddX`.
2. **Build the minimal memo cache + traced graph for these node kinds:** L0 values,
   L1 entities (geometry, painter, material), L2 object, the name-ref edges
   **recorded by tracing the derivation** (D4 — *not* read from
   `referenceCategories`, which is only the picker/rename hint), and the L2→L4 (TLAS)
   edge. Snapshots are immutable + COW (D1); the cache keys on NodeId (D9).
3. **Wire the two backends:** REBUILD = the existing `Finalize→AddObject`/
   `AddSphereGeometry`/`AddLambertianMaterial`; APPLY = `SetObjectPosition` +
   `RunObjectInvariantChain` for a transform edit, `MaterialIntrospection::
   SetSlotValue` for the material's `reflectance` — both **on COW copies in the new
   snapshot** (D1).
4. **Demonstrate the core behaviors (mapping to D10's gates G2/G3/G4):**
   - edit the sphere's `position` → APPLY path → new snapshot shares everything but
     the copied object; only TLAS rebuilds (prove no entity re-derived; assert via
     the existing `GetSamplerRebuildCount` staying flat and a TLAS-rebuild counter
     ticking once) — **G3 minimal invalidation**;
   - edit the material's `reflectance` to a new color → APPLY path → no
     light-topology rebuild (sphere non-emissive), no TLAS rebuild;
   - rename the material the object binds → `NodeId`-preserving rename (D9); the
     producer's manager **version bumps** → object rebinds via the version-staleness
     check (§2.6) — **G4 versioning** (round-trip byte-identical after undo, G1).
   - *(phase 4)* edit a `Double` fed by `expr(A)`, then change `A` → the consumer's
     traced-input version bumps and it re-derives, while a sibling literal `5` stays
     a memo hit (D4).
5. **Prove the memo hit (G1/G3):** add a comment / reformat whitespace in the CST →
   diff shows a text change → the node's **green-node structural hash and traced
   inputs are unchanged** → **zero** re-derivation (the headline INV-3/INV-4 win,
   measurable as "derive did nothing"; the new snapshot shares 100% by reference).

This slice stands up the cache, the **traced** graph build, the diff→dirty→propagate
loop, the COW-snapshot publish, and both backends — the skeleton every other chunk
type plugs into by virtue of being descriptor-driven. It reuses 100% of the engine
(managers, apply-layer, phase-B pipeline) and adds only the thin memo+graph +
snapshot layer. All five D10 gates (G1 round-trip, G2 latency, G3 minimal
invalidation, G4 versioning, G5 external inputs once assets enter at phase ≥2) apply.

---

### Appendix: one-paragraph mental model

Today RISE *loads* a scene whole-file and then *mutates* it in place through a
separate editor with its own record/history/dirty machinery. Model B keeps the two
existing engines — the loader's `Finalize→AddX` (now the **rebuild** backend) and
the editor's `ApplyObjectOpForward`+introspection setters (now the **apply** backend,
run on **COW copies** — D1) — and puts a **memo cache + traced dependency graph** in
front of them. The function is `derive(CST, AssetManifest)` (D5), producing an
**immutable snapshot with structural sharing** (D1). A CST/manifest edit becomes a
node-diff; the **traced** graph (D4) turns the diff into the *minimal* set of rebuilds
(structural changes) and copy-then-apply value changes, materializes them in a new
snapshot that shares every untouched object by reference, then sets the
*already-existing* phase-B dirty flags (light-topology generation, TLAS-valid,
photon-pending) precisely instead of via scattered hand-bumps. The renderer
**atomically swaps to the new snapshot at a tile/pass boundary** — no
parking-for-safety; the old snapshot drains by refcount (D1). The render-time
realize→TLAS→light-sampler→photon pipeline is unchanged. Determinism comes from
deriving in dependency order (discovered by tracing, fixing `Job`'s load-order
landmines) and keying the memo on the **green-node structural hash + traced-input
versions** (so `expr(A)` ≠ literal `5` — D4); incrementality comes from the memo
cache + structural sharing; safety comes from immutability plus deriving with holes
so one bad node never blanks the scene.
