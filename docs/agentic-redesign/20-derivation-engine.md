# Facet 2 — Derivation Engine (CST → Scene, incremental)

> **Updated per [`01-DECISIONS.md`](01-DECISIONS.md) (rounds 1–4).** The
> decision record is authoritative; where this doc once conflicted it now conforms
> and points to the relevant decision. The decisions that reshaped this facet:
> **D1** — the derived scene is an *immutable COW snapshot* the renderer swaps at a
> pass boundary (no parking-for-safety); **D4** — dependency edges are *traced
> during derivation* and the memo key includes traced-input versions; **D5** — the
> formal input is *(CST, AssetManifest)*, not CST alone. See also D9 (NodeId is the
> lineage identity; name-path is addressing). **Round 2 amends several of these:**
> **D11** — COW is a *reverse-dependency-closure copy* (the engine scene is a
> raw-pointer graph, so you cannot share a referrer of a changed node); **D12** —
> derivation builds into a mutable `DerivedSceneBuilder`, runs phase B on it, then
> *seals* to an immutable value, and adoption is at a *pass* boundary only; **D13** —
> the derivation exposes both `headVersion` and `derivedVersion` (render/graph stamp
> the latter, which may lag — *refined to full stamps + DAG-ancestry by D29, round 4
> below*); **D15** — three distinct concepts (lossless content hash
> / trivia-insensitive *derivation key* / `NodeId` lineage), and the memo key is the
> derivation key; **D16** — wide child sequences are a persistent balanced rope
> (O(log N), not O(depth)); **D17** — asset identity is a (size,mtime) prefilter →
> content hash; **D20** — the derivation cache is persistent with an explicit
> dependency-edge lifecycle (*re-homed from the Version onto stamp-keyed artifacts by
> D30, round 4 below*). **Round 3
> reshapes the layering:** **D22** — the render-ready state splits into two sealed
> immutable layers, **`DerivedScene = f(CST, AssetManifest, t)`** (config-INDEPENDENT:
> realized geometry, materials, lights-as-emitters, **TLAS**) and
> **`PreparedRenderState = prepare(DerivedScene, RenderConfig)`** (config-DEPENDENT:
> **light samplers, photon maps**, integrator-specific structures), with `RenderConfig`
> an explicit third input and a render-time integrator override re-running only
> `prepare`; **D21** — animation is *per-frame derivation* (**time `t` is a derivation
> input** — *refined to a time INTERVAL + animation name, preserving motion blur, by D31,
> round 4 below*) and caches *populated DURING a pass* (irradiance cache, accumulation)
> are **render-local mutable** state owned by the render pass, not in either immutable
> layer; **D23** — the O(closure)/O(log N) headline is **gated on persistent immutable
> containers** (HAMT/persistent tree) for the manager roots + derivation cache +
> identity side-map; the honest v1 fallback is copy-on-snapshot mutable maps =
> O(N_entities)/snapshot; **D24** — **v1 fully rebuilds the TLAS** (O(N log N) « the
> render); incremental/refit/persistent-BVH is a named future; **D28** — history
> preserves the **CST only**, re-deriving an old version uses **current** asset bytes
> (a content-addressed asset store is a named future, not core). **Round 4 makes identity,
> determinism, and threading precise:** **D29** — the cache key is a full
> **`DerivedStamp = {cstVersion, assetManifestGen, animationName, shutterInterval}`**
> (identifies a `DerivedScene`) / **`PreparedStamp = DerivedStamp + {renderConfig,
> cameraOverride, samplingSeed}`** (identifies a `PreparedRenderState`); cache lookups
> are **full-stamp equality** and "stale vs head?" is **`cstVersion` version-DAG
> ancestry**, never numeric `<` (this replaces the informal keys and the single
> `derivedVersion` of D13/D22); **D30** — the memo/dependency cache lives on a
> **`DerivedArtifact` keyed by `DerivedStamp`** (and a `PreparedArtifact` by
> `PreparedStamp`) held in a stamp-keyed LRU, **NOT on the immutable `Version`**
> (`Version = {greenRoot, identityRoot, metadata}` only); one `Version` → many
> artifacts; **D31** — motion blur is **preserved**: a motion-blurred frame's
> `DerivedScene` is **time-INTERVAL-parameterized** (animated quantities baked as
> immutable functions/samples over the shutter `[t0,t1]`, PBRT-style `AnimatedTransform`;
> the renderer evaluates `at(τ)` per sample read-only) and the TLAS becomes a **motion
> BVH** — **gated work** (v1 = single-time / no motion blur; motion blur is a named
> follow-on like the TLAS-refit gate); **D32** — `prepare()` reads the sealed
> `DerivedScene` through **non-mutating (const) input APIs** and writes a separate
> mutable **`PreparedRenderStateBuilder`**, then seals (named prerequisite: refactor
> `BuildPendingPhotonMaps` + light-sampler construction from "mutate the Scene" to
> `build(const DerivedScene&, PreparedRenderStateBuilder&)`); **D33** — `prepare()` is
> **deterministic**: `RenderConfig` carries a **sampling seed / RNG-stream id** used by
> stochastic prep (photon tracing) instead of `rand()`, and the seed is in
> `PreparedStamp` → prepare is pure/cacheable/reproducible; **D34** — derive → seal →
> prepare → seal → render run as **cancellable phases of the render arbiter's job, off
> the edit thread** (the edit thread only commits a cheap CST `Version`); a newer head
> **cancels + restarts** the in-flight phases — this is the source of the head-vs-derived
> lag. (D35–D37 touch rename / identity-propagation / migration owned by neighbor facets;
> §2.6/§7 note conformance.)
>
> **Status:** design-in-progress. One facet of the [Agentic Redesign](00-CHARTER.md).
> Read the [charter](00-CHARTER.md) and [`01-DECISIONS.md`](01-DECISIONS.md) first —
> this doc assumes the charter's locked decisions (L1–L7), invariants (INV-1…6), the
> canonical-CST pivot, and the round-1 decisions (D1–D10). **Design only; no
> code/build/scene changes.**
>
> **This facet owns:** the two-layer function **`DerivedScene = derive(CST,
> AssetManifest, t)`** (D5/D21/D22) followed by **`PreparedRenderState =
> prepare(DerivedScene, RenderConfig)`** (D22) — both made *pure*, *deterministic*,
> *incremental*, and *memoized*, each producing an **immutable sealed snapshot via
> copy-on-write** (D1/D12); the **traced** CST→engine dependency graph (D4); memo
> granularity; the derive/apply seam (reuse of the surviving apply-layer **on COW
> copies**); interaction with deferred realization / TLAS / photon passes; the
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

### 2.1 Shape of the function — two layers (D22)

Round 3 (D22) splits the render-ready state into **two sealed immutable layers**: a
config-**independent** `DerivedScene` and a config-**dependent** `PreparedRenderState`.
The reason: light samplers and photon-map preparation depend on the rasterizer /
integrator settings, and `render` permits an integrator override — so the render-ready
scene is **not** purely `f(CST, AssetManifest, t)`.

**Round 4 (D29/D30) makes the identity of each layer precise, and moves the cache off
the immutable `Version`.** Each layer is identified by a **stamp** — its full input
identity — and the memo/dependency cache lives on an **artifact** keyed by that stamp,
held in a stamp-keyed LRU, **not** on the `Version`:

```
Version          = { greenRoot, identityRoot, metadata }            // CST + occurrence identity ONLY — no cache (D30)
DerivedStamp     = { cstVersion, assetManifestGen, animationName, shutterInterval }   // identifies a DerivedScene (D29)
DerivedArtifact  = { derivedStamp, derivedScene, derivationCache }   // the cache lives HERE (D30)
PreparedStamp    = DerivedStamp + { renderConfig, cameraOverride, samplingSeed }      // identifies a PreparedRenderState (D29/D33)
PreparedArtifact = { preparedStamp, preparedRenderState, prepareCache }
```

A `Version` can have **many** `DerivedArtifact`s (one per time-interval / asset-manifest
generation / animation) and each of those many `PreparedArtifact`s (one per
`RenderConfig` / camera override / seed) — one Version → many artifacts (D30). **Cache
lookups match the full stamp by equality** (D29). The only **ordered** comparison is
"is the rendered scene stale vs head?", which is checked on the **`cstVersion` axis
alone, by version-DAG ancestry** (the rendered `cstVersion` is an ancestor-or-equal of
head's) — **never numeric `<`**, because the version DAG branches and the other stamp
axes (`assetManifestGen`, `shutterInterval`, `renderConfig`, …) are equality-matched,
not ordered (D29). The layered model:

```
// "t" below is the shutter INTERVAL [t0,t1] of the DerivedStamp (a single instant when no
// motion blur — D31), plus the active animationName; both are DerivedStamp axes (D29).
derive  : (CST, AssetManifest, t, animationName)  →  DerivedScene  (pure, deterministic — INV-2, D5/D21/D22/D31)
prepare : (DerivedScene, RenderConfig)  →  PreparedRenderState                    (D22; RenderConfig carries the seed — D33)

derive  : (CST, CST', AssetManifest, t, animationName, DerivationCache)
              →  (DerivedScene', DerivationCache')          (incremental — INV-3; cache held on the DerivedArtifact — D30)
prepare : (DerivedScene, DerivedScene', RenderConfig, PrepareCache)
              →  (PreparedRenderState', PrepareCache')      (incremental — INV-3; cache held on the PreparedArtifact — D30)
```

The formal `derive` input is **`(CST, AssetManifest, t)`** (D5/D21), not the CST alone.
`Scene = f(CST)` is false when a referenced texture/mesh/spectral/glTF changes on disk
without the CST or filename changing (the **`AssetManifest`** closes that — D5), and it
is false again for animation, where the same CST yields a different scene at a different
**time `t`** (D21: time is a derivation input; §2.10). The `AssetManifest` maps each
referenced asset path → `{resolved absolute identity, fingerprint}`, where the
fingerprint is a **`(size, mtime)` prefilter → content hash** (the content hash is the
authoritative identity used in memo keys — D17, §2.9); **output paths are excluded**
(sinks, not sources — §2.9). It is part of the derivation environment, versioned
alongside the CST head, and refreshed by a file watcher / re-stat (D5, §2.9).

**`DerivedScene` (config-INDEPENDENT — D22)** is **an immutable, sealed snapshot
(D1, D12)** of the config-independent render-ready state: the `Scene` + its populated
managers + realized/tessellated geometry + **lights-as-emitters** + the **TLAS** — but
produced via **copy-on-write with structural sharing** rather than mutated in place. It
does **NOT** own the light samplers or photon maps; those are config-dependent and live
in `PreparedRenderState` (D22, below). A new CST/manifest/time version derives a **new
`DerivedScene`** by copying the **reverse-dependency closure** of each changed node
(D11) — the changed node *plus every node that transitively references it, up to the
roots* (managers / spatial index) — repointing the copies, and **sharing everything
outside the closure by reference** with the prior snapshot. You **cannot** share a
referrer of a changed node: the engine scene is a raw-pointer graph (materials hold
direct painter pointers, objects hold direct material/geometry pointers —
`LambertianBRDF.h:37`, `Object.h:34`), so a shared material kept pointing at the
*old* painter would be wrong (D11; the corrected example is in §6.1).

**`PreparedRenderState = prepare(DerivedScene, RenderConfig)` (config-DEPENDENT —
D22)** is the second sealed immutable layer: **light samplers** (their construction
depends on the integrator's light-sampling strategy), **photon maps** (built only for
photon-consuming integrators), and any integrator-specific structures. **`RenderConfig`**
— the rasterizer/integrator selection **plus the render-time override**, a **camera
override**, and a **sampling seed / RNG-stream id** (D33) — is an **explicit third
input** (its three non-config-shared axes are exactly the `PreparedStamp` additions over
`DerivedStamp` — D29). Because the split is clean, the **render-time integrator override
re-runs only `prepare`, never scene derivation**: the geometry/materials/TLAS in
`DerivedScene` are reused by reference and only the samplers/photon maps are rebuilt for
the new integrator. The light samplers and photon maps are RayCaster-owned today
(`Scene.h:405`); D22 moves them *into* `PreparedRenderState` (not into `DerivedScene`),
so a *prepared* state is fully render-ready and self-contained (§2.8).

**`prepare` does not mutate the `DerivedScene` (D32).** It reads the sealed
`DerivedScene` through **non-mutating (const) input APIs** and writes into a separate
mutable **`PreparedRenderStateBuilder`**, which it then **seals** → `PreparedRenderState`
— exactly mirroring how `derive` builds into a `DerivedSceneBuilder` and seals (§2.4).
This is a **named prerequisite refactor**: photon maps are `Scene`-owned today and
`BuildPendingPhotonMaps` *mutates* pending flags / maps / gather params
(`Scene.cpp:750`), and the light samplers are `RayCaster`-owned — you cannot build either
by mutating a sealed immutable `DerivedScene`. Both must be re-expressed as
`build(const DerivedScene&, PreparedRenderStateBuilder&)` (§2.8). **`prepare` is also
deterministic (D33):** stochastic preparation (photon tracing, any sampled prep) draws
from the `RenderConfig` seed / RNG-stream id, **not** `rand()` (photon tracers seed with
`rand()` today — `RandomNumbers.h:32` — so the same `(DerivedScene, RenderConfig)`
currently yields *different* photon maps). With the seed in `PreparedStamp`, `prepare` is
a pure function of its key → **cacheable and reproducible** (deterministic renders, a win
for the git-native/agentic thesis).

The renderer holds a **refcounted pointer to one `PreparedRenderState`** (which in turn
references its `DerivedScene`) and **atomically swaps it at a pass boundary** (§2.8).
The incremental forms take the previous inputs + the respective memo cache and
re-derive/re-prepare **only the affected closure**. There is **one** implementation per
layer: the full derive/prepare is the incremental one against an empty cache. (P-WALK:
no second path.)

Determinism (INV-2) means: each layer's result is a pure function of its inputs'
*content* — `DerivedScene` of `(CST, manifest, t, animationName)` (its `DerivedStamp`),
`PreparedRenderState` of `(DerivedScene, RenderConfig)` **including the sampling seed**
(its `PreparedStamp` — so even stochastic photon prep is reproducible, D33) — independent
of node *visit order*. §4 audits where today's `Job` violates this and what must change.

**Why COW, not mutate-in-place (D1, resolves the F2↔F3 split):** an earlier draft
of this facet mutated one long-lived `Job`/`Scene` in place and *parked the
renderer for safety*. D1 unifies F2's incremental apply-layer reuse with F3's
immutable no-park model: the apply-layer **is** reused, but on COW copies, so
immutability and structural sharing both hold. Immutability removes the data race
outright, so **parking-for-safety is gone**; cancel-and-park survives only as an
optional latency/coalescing optimization (§2.8). This is the red-green discipline
(D2) extended from the CST to the derived scene — but the derived scene is a
**raw-pointer DAG**, not a tree, so the analogue of the CST's path-copy is a
**reverse-dependency-closure copy** (D11): a new snapshot copies the closure of
referrers of each changed node and shares everything outside it by refcount. The
design-target cost is **O(closure / fan-in of the edited node), not O(scene)** (§6.1) —
**but that headline is explicitly gated on persistent immutable containers (D23):** the
manager roots (name→entity maps), the derivation cache (§2.3), and the per-`Version`
identity side-map must be **HAMT / persistent-tree** structures to give O(log N) update
+ structural sharing. An **honest v1 fallback** (D23) **may** use copy-on-snapshot
mutable maps — **O(N_entities) per snapshot** — acceptable while entity counts are
modest and commits are debounced; **in that case the cost is O(N), not
O(closure)/O(log N)**, and the headline must not be claimed before the persistent-
container work lands. Separately, a first implementation **may full-rebuild** (closure =
everything) for correctness, then add closure-tracking — the design *target* is
closure-copy. (On the **CST** side the path-copy is **O(log N)**, not O(depth): wide
child sequences — the `Document`'s chunk list, a large `RepeatGroup`/heightfield — are a
persistent balanced **rope** with cached aggregate widths, so locating child *k* and
inserting/removing a child are both O(log N) — D16. The **TLAS** is **not** path-copied:
**v1 fully rebuilds it** on any geometry/transform/structural change — O(N log N), «
the render — D24; §2.7/§6.3.)

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
           geometry/bbox)   LightSampler / EnvironmentSampler  (prepare — config-dependent, D22)
   RealizedGeometry
        │
        ▼ (world bbox)
      TLAS  ──────────────►  PhotonMaps (prepare — only if RenderConfig rasterizer ConsumesScenePhotonMaps, D22)
   (DerivedScene; full rebuild v1 — D24)
```

In this diagram, everything down to `RealizedGeometry`/`TLAS` is the **`DerivedScene`**
(config-independent — D22); `LightSampler`/`EnvironmentSampler` and `PhotonMaps` are the
**`PreparedRenderState`** built by `prepare(DerivedScene, RenderConfig)`.

**Node taxonomy** (memo nodes — see §2.3 for granularity):

| Layer | Memo node | Built by | Invalidated when |
|---|---|---|---|
| L0 *value* | a resolved parameter value (literal / evaluated `expr(...)` / inline sub-chunk) | Facet-1 evaluator | its source text changes, OR any **traced** expression input's version changes (D4) |
| L1 *entity* | a manager entry (painter, material, geometry, shader, shaderop, modifier, function, camera, medium, light) | `Finalize → pJob.AddX` | any L0 value it consumes changes, any **traced** name-ref target's *version* changes (§2.6), OR a **traced** AssetManifest **content hash** changes (D5/D17) |
| L2 *object* | an `IObject` (geometry+material+modifier+shader+transform bind) | `AddObject`/`AddObjectMatrix`/`AddCSGObject` | any bound L1 entity rebinds, or its transform params change |
| L3 *realized geom* | baked mesh for a deferred geometry (displaced/SDF/CSG/bezier) | `obj.Realize()` cascade (phase B) | its source geometry entity or any tessellation-driving param changes |
| L4 *TLAS* | top-level BVH over objects (a **motion BVH** over the shutter interval for a motion-blurred frame — D31, gated; v1 = single-time BVH) | `PrepareForRendering` (phase B) | any L2 object's **world bbox** changes (transform or geometry swap) — **v1 full rebuild** (D24, §2.7/§6.3) |
| L5 *light topology* | `LightSampler` + `EnvironmentSampler` | `RebuildLightSamplers` (`prepare`) | light add/remove, emissive-material bind/unbind, exitance edit, **spatial change of an emissive object**, env map replace/scale, **OR the `RenderConfig` light-sampling strategy changes** |
| L6 *photon maps* | caustic/global/shadow/translucent maps | `BuildPendingPhotonMaps` (`prepare`) | any L2/L3/L5 change **and** the active rasterizer (a `RenderConfig` input) consumes them |

**Layer split (D22):** L0–L4 (values, entities, objects, realized geometry, **TLAS**)
are **config-independent** and live in `DerivedScene`; **L5 (light topology) and L6
(photon maps) are config-dependent** and live in `PreparedRenderState` — they are
built by `prepare(DerivedScene, RenderConfig)`, not by `derive`, because the
light-sampling strategy and the photon-consuming gate are `RenderConfig` inputs (§2.1).
A render-time integrator override therefore re-runs only L5/L6 (`prepare`), reusing
L0–L4 by reference.

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

`referenceCategories` is therefore **demoted to a UI ref-picker *hint*** (D4): it
drives ref-pickers in the dynamic UI — it is **not** the source of truth for the
dependency graph, **and (D14) it is no longer used for rename's referrer search
either**. Rename rewrites referrers from the **traced `ReferenceUse { sourceValueNodeId,
targetNodeId }` records** the dependency trace records (D4/D14), which capture dynamic
refs (`timeline.element` via `element_type`) automatically; for references in nodes
that did not derive (e.g. inside an error subtree), it falls back to descriptor
**reference resolvers**, and any referrer it cannot resolve is **surfaced/flagged,
never silently renamed**. (No parallel schema is invented; the hint
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

**The memo key is the node's *derivation key* — a trivia-INsensitive semantic hash
(typed values + child structure, *excluding* comments / whitespace / trivia) +
traced-input versions (D4) — NOT the green node's lossless content hash (D15).**
D15 separates three concepts that an earlier draft conflated: the green node's
**content hash** is lossless and trivia-*sensitive* (it hashes the exact bytes incl.
trivia, so byte-identical subtrees share one green node — its job is structural
sharing/dedup, and it carries no identity); the **derivation key** here is its
trivia-*insensitive* sibling whose job is the *memo cache* (so a whitespace-only edit
is a derivation cache **hit**); the **`NodeId`** is the per-occurrence lineage
identity (§2.6). For an L1 entity node:

```
key(entity) = H( derivation_key(node),                       // typed values + child STRUCTURE, NO trivia (D15)
                 { (param_name, resolved_value) for each literal param },
                 { (input_NodeId, input_version) for each TRACED CST input },   // exprs + name-refs (D4)
                 { (asset_path, content_hash)    for each TRACED asset input } )  // D5/D17
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

The derivation key's trivia-insensitivity gives INV-4 for free in the other
direction: a whitespace-only or comment-only edit (which Facet 1's CST preserves
losslessly, so the green node's *content* hash changes) leaves the **derivation key**
unchanged and touches no traced input → memo hit → zero re-derivation. Likewise
"typed the same literal value back" is a hit. What is *not* a hit is a value that
arrived through a different traced dependency — that is the point.

The memo table is keyed and indexed by **`NodeId`** — D9; the `NodeId` lives in the
red layer / a side-map, **not** in the shared green node — D15 — with name-path
resolved to NodeId per version. **Where the cache LIVES is settled by D30: it is *not*
on the immutable `Version` (which is `{greenRoot, identityRoot, metadata}` — CST +
occurrence identity only) but on a `DerivedArtifact` keyed by the `DerivedStamp`** (the
`DerivationCache` below), with the prepare-layer cache on a `PreparedArtifact` keyed by
the `PreparedStamp` (the `PrepareCache` below). Artifacts are held in a **stamp-keyed
LRU**, so **one `Version` → many cache-holding artifacts** (per time-interval / asset
generation / `RenderConfig` / seed). A `Version` commits *before* async derivation
completes and can spawn many artifacts, which is exactly why the cache cannot hang off
it (D30). **The cache is still version-scoped and persistent (D20)** in the sense that
each artifact's cache is keyed by its full stamp and **structurally shared across
artifacts** like the green tree (a re-derive at a new stamp shares unchanged entries),
**NOT a single global mutable `DerivationCache<NodeId,…>`** — divergent branches and a
simultaneous committed-vs-preview pair each see their own consistent cache view. **The
"structurally shared" property is gated on persistent immutable containers (D23):** the
maps below must be **HAMT / persistent-tree** structures for cross-stamp sharing at
O(log N) update. An **honest v1 fallback** (D23) **may** use the `std::unordered_map`s
shown literally, **copy-on-snapshot** — **O(N_entities) per snapshot** — which is
acceptable at modest entity counts with debounced commits; **in that case the cache is
NOT structurally shared and the O(closure)/O(log N) claims downgrade to O(N)** until the
persistent-container work lands:

```cpp
// Held on the DerivedArtifact, keyed by its DerivedStamp = {cstVersion, assetManifestGen,
//   animationName, shutterInterval} (D29/D30) — NOT on the immutable Version.
// TARGET: a persistent (HAMT) map structurally shared with prior stamps' artifacts (D20+D23).
// v1 fallback: plain maps, copy-on-snapshot = O(N) (D23).
struct DerivationCache {
    // NodeId  →  derived record (the engine object + its key + its version)
    std::unordered_map<NodeId, EntityRecord> entities;   // L1   (→ persistent map under D23)
    std::unordered_map<NodeId, ObjectRecord> objects;    // L2   (→ persistent map under D23)
    // Reverse dependency index, populated by TRACING (D4): which derived records
    // depend on a given producer / asset, so an invalidation pushes forward in
    // O(out-degree), not O(scene). Producers are NodeIds; asset producers are
    // AssetManifest keys. Edge lifecycle (D20): on re-deriving a node, ATOMICALLY
    // replace that node's OUTGOING edge set (drop stale, add freshly-traced); on
    // deleting a node, purge its edges + its cache entry (and surface any now-dangling
    // ReferenceUse, D14).
    std::unordered_multimap<NodeId, NodeId> dependents;        // producer node → consumers
    std::unordered_multimap<AssetKey, NodeId> assetDependents; // asset → consumers (D5)
    // Config-INDEPENDENT phase-B coarse flag (DerivedScene layer, D22/D24):
    bool     tlasValid;                     // false ⇒ PrepareForRendering FULLY REBUILDS the TLAS (D24)
    // NOTE (D22): the light-topology generation and photon generation are
    // config-DEPENDENT and belong to the prepare-layer cache (PrepareCache below),
    // since they depend on RenderConfig (the light-sampling strategy / photon gate).
};

// Held on the PreparedArtifact, keyed by its PreparedStamp = DerivedStamp +
//   {renderConfig, cameraOverride, samplingSeed} (D29/D30/D33). Same D23 gating:
//   persistent map = shared; v1 fallback = copy-on-snapshot O(N).
struct PrepareCache {
    uint64_t builtLightTopologyGeneration; // == Scene::GetLightTopologyGeneration() at last L5 build
    PhotonMapGeneration photonGen;          // bumped by any L2/L3/L5 change (L5/L6 live here, D22)
};

struct EntityRecord {
    InputHash      key;         // §2.3 derivation-key (trivia-insensitive) + traced-input-version key (D15)
    uint64_t       version;     // manager identity version at build time (§2.6)
    EntityKind     kind;
    bool           isEmissive;  // drives the L1→L5 edge
    // (the engine object itself lives in the snapshot's manager, shared by reference
    //  with the prior snapshot when this record is a memo hit — D1 structural sharing)
};
```

**Identity (D9/D15):** the cache keys on the immutable, process-stable **`NodeId`**
(the per-occurrence lineage token, living in the **red layer / a side-map, not in the
shared green node** — D15, since a shared green node is reused at many occurrences and
cannot carry one id), not on the name-path. **name-path**
(`objects/sphere`, `materials/glass`, `materials/glass.reflectance` for a slot) is
the human/agent *addressing* scheme, resolved to a `NodeId` *within a given
version*; it changes on rename (by design). A rename is a `NodeId`-preserving edit,
so cache records and UI/agent bindings survive it (D9). The `GenericManager` entry
is still name-keyed (that is how the engine stores it), but the *derivation*'s
stable handle is the NodeId, and the manager's identity counter (§2.6) is the
freshness token; the two are orthogonal and both already exist.

### 2.4 How a localized edit re-derives only the affected subgraph (INV-3)

Given `(CST, CST', AssetManifest, t, cache)`:

1. **Seed the dirty set** from three sources: (a) the **CST diff** CST→CST′ at node
   granularity (Facet 1 supplies the structural diff, or we recompute keys per node
   and compare to `cache`) → dirty nodes {added, removed, changed}; (b) any
   **AssetManifest fingerprint change** (D5) → seed every consumer via
   `assetDependents`. The manifest delta is produced by the watcher / re-stat
   (§2.9); (c) a **change of the frame's time interval** (`shutterInterval`, a
   `DerivedStamp` axis — D29/D31) for an animation frame (D21/D31) → seed every node
   whose value depends on time (the keyframed/`timeline` params, traced like any other
   input). Each frame derives its own immutable `DerivedScene` over that interval (a
   single instant in the v1 no-motion-blur path; an AnimatedTransform-over-`[t0,t1]` in
   the D31 follow-on — §2.10); the diff between adjacent frames is exactly the
   time-dependent forward cone (§2.10).
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
4. **Build into a mutable builder, run the config-independent phase B on it, *seal*
   `DerivedScene`; then `prepare` builds `PreparedRenderState` and publish (D12/D22).**
   **This whole step 4 runs as cancellable phases of the render arbiter's job, off the
   edit thread (D34)** — the edit thread only committed the cheap CST `Version` (steps
   1–3's diff/dirty seeding is likewise the arbiter's, against the new head); if a newer
   head arrives mid-`derive`/`prepare`/render, the in-flight phases **cancel and restart**
   at the new stamp (§2.8). The COW re-derive produces a **mutable `DerivedSceneBuilder`** (a closure-copy view
   per D11) that shares all untouched engine objects by reference with the prior
   snapshot. **The config-independent phase B (realize/tessellate + TLAS) runs on the
   *builder*, before any publication** — there is **no publish-before-phase-B** (an
   earlier draft published the snapshot and *then* ran phase B, mutating after
   publication; D12 forbids that). The phase-B machinery is *unchanged from today* and
   needs no new code: `AttachScene` (running over the builder) re-walks `Realize()`
   idempotently — only the re-marked L3 geometries do work — and `PrepareForRendering`
   **fully rebuilds the TLAS** (v1 — D24; no path-copy). Then **`seal()` →** an
   immutable **`DerivedScene`** value that **owns** the realized geometry and the TLAS
   (config-independent — D22). **`prepare(DerivedScene, RenderConfig)` then builds
   `PreparedRenderState`** — the config-dependent layer — by **reading the sealed
   `DerivedScene` through non-mutating (const) input APIs and writing into a separate
   mutable `PreparedRenderStateBuilder` (D32)**, never mutating the `DerivedScene`: it
   rebuilds the **light samplers** iff `liveGen != builtLightGeneration`, and re-shoots
   the **photon maps** iff pending + the active rasterizer (a `RenderConfig` input)
   consumes them — with all stochastic prep drawing from the `RenderConfig` **sampling
   seed** rather than `rand()`, so the result is reproducible (D33); it **seals** an
   immutable `PreparedRenderState` that **owns** the light samplers AND photon maps
   (moved *into* the prepared state from the RayCaster — D22, §2.8). (This is the named
   prerequisite refactor of `BuildPendingPhotonMaps` / light-sampler construction from
   "mutate the Scene" to `build(const DerivedScene&, PreparedRenderStateBuilder&)` —
   §2.1/§2.8.) **Only
   the sealed `PreparedRenderState` is published**, by an atomic pointer swap the render
   loop adopts **at a PASS boundary** (never mid-frame / per-tile — D12, §2.8); the old
   prepared state (and the `DerivedScene` it referenced) drains by refcount. A
   render-time integrator override re-runs **only `prepare`** (reusing the sealed
   `DerivedScene` by reference — D22). **Facet 2's job is to set those existing dirty
   flags precisely, from the traced graph, instead of from scattered hand-bumps** — and
   to ensure all phase-B / prepare work mutates the *builder's closure copies*, never an
   object shared with a snapshot a render thread may still hold. This is the deepest
   reuse in the design: the entire phase-B + prepare machinery already exists and is
   correct; we run it on the builder / over the sealed `DerivedScene`, then seal each
   layer, and let structural sharing keep the snapshots cheap.

**Worked example — `param → object → TLAS`:** user edits `sphere`'s `position`.
Diff → one dirty L0 (the position value) → its owning L2 object record's key
changes → apply-layer runs `SetObjectPosition` + `RunObjectInvariantChain`
(`FinalizeTransformations`, `InvalidateSpatialStructure`) → `tlasValid=false`. No
L1 entity touched, no other object touched, light topology untouched (sphere
non-emissive). On seal (config-independent phase B on the builder, before publish —
D12): realize no-ops everything, `PrepareForRendering` **fully rebuilds the TLAS**
(v1 — D24; unavoidable — one bbox moved) into the sealed `DerivedScene`; `prepare`
re-uses the prior light samplers/photon maps by reference (no light-topology change),
seals `PreparedRenderState`, which then publishes and pixels re-render. **Cost ≈ one
matrix finalize + one full TLAS rebuild** (D24).

**Worked example — `emissive material → light topology`:** user raises an area
light material's `exitance`. Diff → one dirty L0 → owning L1 material record's key
changes, `isEmissive` stays true → apply-layer runs
`MaterialIntrospection::SetSlotValue` → L1→L5 edge fires →
`BumpLightTopologyGeneration`. The `DerivedScene` reseals (the emissive material is a
closure copy; geometry/TLAS untouched). On `prepare`: it sees
`liveGen != builtLightGeneration` → `RebuildLightSamplers` (rebuilds the alias table
with the new weight, sealed into the **`PreparedRenderState`** which now owns it — D22);
photons untouched unless L6 live. This is exactly the existing
`BumpSceneLightGenerationIfMaterialEmits` path — Model B just reaches it from a graph
edge instead of a hand-coded call site, and the rebuilt sampler lands in the
config-dependent layer.

### 2.5 The derive/apply seam — incremental update vs. rebuild-from-scratch

Two backends produce L1/L2 records; the **decision rule** picks per dirty node.
**Both backends operate on the `DerivedSceneBuilder`'s closure copies (D11/D12)**,
sealed before publish — REBUILD adds into the builder's manager; APPLY mutates a
*closure-copied* engine object, never one shared (by reference) with the prior
snapshot:

```
For a CHANGED node N:
  if N is structural  (added node, removed node, or a change that alters the
                       node's TYPE / its ref-edge SHAPE — e.g. material kind
                       changed, a new param appeared that adds a dependency,
                       composed-material slot, geometry kind changed)
        → REBUILD: run the full Finalize-equivalent → AddX into the BUILDER's
                   manager (remove the old entry first; this bumps the identity
                   version, which §2.6 propagates to dependents).
  else if N is a VALUE-ONLY change to a slot the apply-layer can mutate
       (object transform / material slot / light prop / medium prop / camera prop /
        object material|shader|geometry|medium REBIND)
        → APPLY: closure-copy the target engine object into the builder (D11), then
                 call the surviving mutator (ApplyObjectOpForward / the Introspection
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

### 2.6 Identity & the traced name-ref edge (INV-5, D4, D9, D15)

Three identity concepts, kept distinct per D9 + D4 + D15:

- **`NodeId`** (D9/D15) — the immutable, process-stable **per-occurrence** lineage
  token, living in the **red layer / a side-map, NOT in the shared green node** (D15:
  a shared green node is reused at many occurrences and cannot carry one id). The memo
  cache keys on it; it survives rename and value edits. **Reparse identity is
  best-effort (D15):** a *structured* edit preserves `NodeId` exactly (it targets a
  known node); a *whole-region reparse* matches new green nodes to prior NodeIds by
  structural position + content, **but identical repeated rows are genuinely ambiguous
  — unmatched durable references are INVALIDATED (flagged), not silently remapped.**
  This is what durable UI/agent bindings and undo lineage key on.
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
  apply-layer on a closure-copy — D11) or rebuild. This is how "material `glass` was
  replaced
  by a different-kind material under the same name" correctly invalidates every
  object bound to `glass`, without those objects' own CST nodes having changed — and
  the consumers are reached via the traced `dependents` index, not a static schema
  walk.

`NodeId` is the *stable* identity for UI/agent/selection/cache; name-path is the
*addressing* over it; the manager version is the *freshness* token for derivation.
The three are orthogonal and all already exist (or, for NodeId, are introduced by
D9/Facet 1).

### 2.7 Interaction with realization, `CanTessellate`, the freeze guard, photons

- **Deferred realization + TLAS (config-independent phase B):** Facet 2 does **not**
  realize during the assembly phase (A). It marks L3 nodes dirty; the existing
  single-threaded realize pass in `AttachScene` bakes them — **on the
  `DerivedSceneBuilder`'s closure copies, before `seal()` and therefore before
  publish** (D12) — i.e. before the freeze bracket. **The TLAS is fully rebuilt in v1**
  on any geometry/transform/structural change (`PrepareForRendering` — O(N log N), «
  the render — D24); there is **no** O(log N) TLAS path-copy in v1 (incremental/refit/
  persistent-BVH is a named future prerequisite — §6.3). The realized geometry + TLAS
  are sealed into the **`DerivedScene`** (config-independent — D22). The freeze guard
  (`RenderParallelScope`, `g_renderParallelDepth`) is honored *for free* because the
  in-flight render reads its own immutable snapshot (D1) and the re-derive only ever
  mutates COW copies in the *new* snapshot — it never touches an object the running
  workers hold. **Re-derive therefore does NOT require parking the render for safety**
  (D1): immutability removes the data race that the old "assert
  `g_renderParallelDepth == 0`" gate guarded against. The freeze guard remains a useful
  DEBUG assertion that no `Realize()` runs *on the live snapshot* mid-render, but it is
  no longer a correctness prerequisite for starting a re-derive (see §2.8 for the
  optional cancel-and-park latency optimization).
- **`CanTessellate` / parse-refusal:** a `displaced_geometry` over a
  non-tessellatable base (e.g. `InfinitePlaneGeometry`) must fail derivation, not
  render black. Today `AddDisplacedGeometry` returns false and the whole parse
  fails. In Model B this is a **node-local derivation error** (§5): the L1
  geometry node derives to a *hole*, the L2 objects binding it derive to holes,
  and the rest of the scene derives normally. The error attaches to the offending
  CST node (Facet 5 surfaces it). `CanTessellate` is checked at derive time (phase
  A), not realize time, so the error is reported before any render is attempted.
- **Photon-pass gating (config-dependent — `prepare`):** L6 is part of the
  **`PreparedRenderState`** *iff* the active rasterizer's `ConsumesScenePhotonMaps()`
  is true (D22: photon maps are config-dependent, built by `prepare`, not `derive`).
  The active rasterizer is a **`RenderConfig` input** to `prepare`. Changing it from
  `pixelpel_rasterizer` (consumes) to `pathtracing_pel_rasterizer` (doesn't) **removes
  L6 from the prepared state** — no photon shoot, and any pending photon dirtiness is
  irrelevant — and re-runs **only `prepare`** (the `DerivedScene` is unchanged and
  reused by reference — D22). Changing back re-adds L6 and marks it pending. This makes
  the existing runtime gate a *prepare-layer membership* decision driven by
  `RenderConfig` plus the traced photon dirtiness.

### 2.8 Threading: derive → seal → prepare → seal → render as cancellable arbiter phases; pass-boundary swap, not parking (D1, D12, D22, D34)

**The edit/agent thread does only one cheap thing: commit a CST `Version` (D34).**
Everything expensive — **derive → seal → prepare → seal → render** — runs **as
cancellable phases of the render arbiter's job, off the edit thread** (D34). This is
non-negotiable because photon-map construction and a full TLAS rebuild take seconds and
all cores; running them synchronously on the UI/agent thread would freeze it (and would
contradict the single-render-arbiter promise). The arbiter picks up each **debounced CST
head (O2)** — both a **commit** *and* a **gesture preview of the uncommitted head** (D1,
§5.3) — at its `DerivedStamp` (D29), builds into a **mutable `DerivedSceneBuilder`**,
runs the **config-independent phase B on the builder** (realize/tessellate + TLAS),
**seals** it to a **new immutable `DerivedScene`** value (D12/D22); then
**`prepare(DerivedScene, RenderConfig)`** — reading the sealed `DerivedScene` through
**non-mutating const input APIs** and building into a **`PreparedRenderStateBuilder`**
(D32), seeded deterministically from `RenderConfig` (D33) — builds and **seals** a
**`PreparedRenderState`** (light samplers + photon maps), and the session **publishes
only the sealed `PreparedRenderState` by an atomic pointer swap at a PASS boundary**
(D12) — it does **not** park the render thread for safety, and it **never mutates after
publication**. **When a newer head arrives, the in-flight phases cancel and restart at
the new stamp (D34)** — this is exactly the source of the head-vs-derived lag (D13/D29):
the arbiter is mid-derive/prepare/render on an *older* stamp while head has moved on. A
preview's snapshots are **ephemeral** (not history versions); at gesture end the
intermediate roots coalesce into one committed version (D1).

**Ownership (D22):** the two sealed layers split ownership. The **`DerivedScene`**
**owns** the realized geometry, spatial index, lights-as-emitters, **AND the TLAS**
(config-independent). The **`PreparedRenderState`** **owns** the **light samplers AND
photon maps** (config-dependent). Today the light samplers are **RayCaster-owned**, not
part of `Scene` (`Scene.h:405`); D22 **moves them into the `PreparedRenderState`** (not
the `DerivedScene`) so a *prepared* state is fully render-ready and self-contained.
The config-independent phase B (realize/tessellate, **full TLAS rebuild** — D24) runs
**on the builder** and seals into `DerivedScene`; the config-dependent prepare (light
samplers, photon maps) runs **over the sealed `DerivedScene`** and seals into
`PreparedRenderState` — neither is ever built into a published value.

**The model (D1, D12, D22):**
- The renderer holds a **refcounted pointer to one immutable `PreparedRenderState`**
  (which references its `DerivedScene`) and reads it freely; because both layers are
  immutable, no lock is needed on the read side.
- A commit/preview builds a new `DerivedScene` on the builder, seals it (sharing all
  untouched engine objects by reference — meshes, BVH leaves, materials, the TLAS where
  untouched), then `prepare`s a new `PreparedRenderState` (sharing the light
  samplers/photon maps by reference where the light topology and `RenderConfig` are
  unchanged), and **atomically swaps the renderer's pointer at a PASS boundary** (never
  mid-frame / per-tile — D12). The render loop checks the published pointer at that
  boundary; the in-flight pass finishes against the *old* prepared state, which (with
  its `DerivedScene`) stays alive on its refcount until it drains, then is freed.
- **Render-time integrator override re-runs only `prepare`** (D22): the same sealed
  `DerivedScene` is reused by reference; only a fresh `PreparedRenderState` is built for
  the new `RenderConfig` and swapped in.
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

**Coherent version status (D13, refined by D29).** Because derive/prepare/render are
**async, cancellable arbiter phases (D34)** and may serve a last-good snapshot, the head
version and the *rendered* scene can differ; a single shared `documentId` would falsely
claim them equal. The session therefore publishes **one coherent status value** alongside
each snapshot:
`{ headVersion, derivedStamp, preparedStamp, snapshot, status ∈ {deriving, ok, error},
diagnostics }`. **`read_graph` / `render` / `derive_preview` are stamped with the
`DerivedStamp` / `PreparedStamp`** (D29) — *what the scene actually reflects* — whose
`cstVersion` axis **may lag head** (or be last-good on error) and is **never** claimed
equal to head. The "is this rendered scene stale vs head?" check is on the **`cstVersion`
axis alone, by version-DAG ancestry** (rendered `cstVersion` is an ancestor-or-equal of
head's), **never numeric `<`** (the DAG branches; D29) — the other stamp axes
(`assetManifestGen`, `shutterInterval`, `renderConfig`, `samplingSeed`, …) are
equality-matched, not ordered. (`read_document` is stamped with `headVersion`, the CST
truth — Facet 1/5; a patch's optimistic-concurrency precondition is checked against
`headVersion`.) `status` + `diagnostics` explain any lag or failure so a consumer reading
a lagging stamp knows why.

The re-kick that follows a swap is the same `RasterizeScene`, now running against the
newly published `PreparedRenderState`. **No second *mutable* representation is
introduced** (INV-1): the memo caches are derived state, not an independent source of
truth — they can be dropped and rebuilt from `(CST, AssetManifest, t)` + `RenderConfig`
at any time; the two sealed layers are immutable values, not a mutable mirror of the
CST. (The *render-local* caches of §2.10 — irradiance cache, accumulation — are a
deliberate, separately-owned **mutable** exception that lives in the render pass, not in
either immutable layer; they too are reconstructible and never a source of truth.)

### 2.9 The AssetManifest and external-input invalidation (D5)

The second formal input to `derive` is the **`AssetManifest`** (D5): a map from each
**referenced asset path** → `{resolved absolute identity, fingerprint}`. **The asset
identity is a two-stage fingerprint (D17): a `(size, mtime)` *prefilter* → on a
prefilter change (or whenever determinism is required) compute a *content hash*, which
is the authoritative identity.** `(size, mtime)` alone is not deterministic — bytes
can change with size/mtime unchanged — so it is only a fast prefilter, never the
recorded identity. It covers every externally-loaded asset a chunk references —
textures (`png_painter`/image painters), meshes (`bezier`/mesh loaders), spectral data
(`scalar_painter { file … }`), glTF, environment maps, `.sdf` sidecars (where still
used), etc.

- **Asset reads are traced (D4); memo keys use the content hash (D17).** When `derive`
  loads a file for a param, it records an edge from the consuming L1/L3 node to that
  manifest entry (the `assetDependents` index, §2.3), and the entry's **content hash**
  participates in the consumer's memo key (§2.3). A content-hash change therefore
  invalidates exactly the nodes that consumed that asset — `Scene = f(CST)` becomes
  `DerivedScene = f(CST, AssetManifest, t)` (D5/D21), closing the "texture changed on
  disk, clean derive disagrees with a cache hit" hole.
- **History preserves the CST only; re-derivation uses *current* asset bytes (D28).**
  The manifest records each asset's identity + content-hash **fingerprint, not its
  bytes**. Undo/redo/branch history therefore preserves the **CST (the source), not
  historical rendered output**: re-deriving an old version **re-stamps** the manifest
  from the **current** filesystem, so if an external asset changed since, the old
  version's *render* may differ — **explicitly documented**, exactly like git versioning
  source while large build inputs are the user's responsibility. The
  `DerivedScene = f(CST, AssetManifest, t)` purity holds *within a manifest*; across
  time, the manifest is the live filesystem. A **content-addressed asset store**
  (snapshotting asset bytes by hash for fully reproducible historical renders) is a
  **named future option, not core** — the git-LFS analogue, layered at the VCS
  boundary, not the editor.
- **Output paths are excluded** (D5): `file_rasterizeroutput.pattern` and any sink
  path are **sinks, not sources** — they never appear in the input dependency set and
  never trigger invalidation.
- **Invalidation is watcher-driven** (D5): a background file watcher (or an explicit
  re-stat on focus / before render) refreshes the manifest via the prefilter, hashing
  on a prefilter change. A changed content hash seeds the dirty set (§2.4 step 1b) and
  re-derives the consumers. The manifest is versioned alongside the CST head and is
  part of the derivation environment.
- This is the same mechanism Facet 1/3 use for the **external-file conflict guard**:
  D17 makes the save **atomic** — write to a temp file in the target dir → fsync →
  revalidate the target's content hash == the loaded fingerprint → atomic `rename()`
  over the target (replacing D6's stat-then-write CAS, which had a TOCTOU race) — and a
  revalidate mismatch routes to the D6 conflict UX (reload / diff / force), never a
  silent overwrite. The manifest's content hashes and the save-time revalidation share
  the watcher and the fingerprint definition.

### 2.10 Animation = per-frame derivation over a time INTERVAL; motion blur preserved (gated); render-populated caches are render-local mutable (D21, D31)

Existing animation **mutates** scene objects / TLAS / photon maps / irradiance caches
*during* rendering (`Scene.cpp:561`, `PixelBasedRasterizerHelper.cpp:1887`) — which is
incompatible with the sealed immutable layers (D12/D22). D21 keeps **both** capabilities
(animation **and** irradiance caching) by re-expressing them; **D31 then preserves a
third — motion blur — by making the per-frame scene time-INTERVAL-parameterized rather
than frozen at a single instant:**

- **Animation is per-frame derivation; the time *interval* + the active animation name
  are derivation inputs** (alongside CST + AssetManifest — D5/D21/D31, §2.1; both are
  `DerivedStamp` axes — D29). A `timeline` keyframes a param and derivation evaluates it
  over time. **No mutation-during-render** — an animation render is a **sequence of
  sealed snapshots**, each prepared and adopted at a pass boundary exactly like an
  interactive edit. This matches the CST model directly (keyframes are CST; the time
  axis selects the evaluation point/interval).
- **Motion blur is preserved, not retired (D31).** Rasterizers/photon-tracers evaluate
  animation at a **random time per sample** within the shutter for motion blur
  (`PixelBasedPelRasterizer.cpp:636`); a single frozen `DerivedScene(t)` would destroy
  that. So a **motion-blurred frame's `DerivedScene` is time-INTERVAL-parameterized**:
  animated quantities are baked as **immutable functions/samples over the shutter
  `[t0,t1]`** (a PBRT-style `AnimatedTransform`), and the renderer evaluates **`at(τ)`
  per sample, read-only** (no mutation of the sealed scene). The "time" axis of the
  `DerivedStamp` is therefore the **shutter interval** (a single instant when there is
  no motion blur). The frame's TLAS becomes a **motion BVH** (time-interval) under D31.
- **Gating (D31).** This is **named follow-on work, like the TLAS-refit gate (D24):
  v1 supports single-time frames (no motion blur)** — the `DerivedStamp`'s time axis is a
  point and the TLAS is the single-time BVH; the **AnimatedTransform-in-`DerivedScene` +
  motion-BVH** path lands later. Motion blur is **not** dropped from the design — it is
  deferred to that follow-on.
- **Caches populated DURING a pass are render-local MUTABLE state, owned by the render
  pass — NOT in any immutable layer.** The **irradiance cache** and **accumulation
  buffers** are filled *as the pass runs*; they cannot be sealed-before-publish because
  they do not exist until the render produces them. They are owned by the render pass,
  **may persist across passes/frames for temporal coherence** (the renderer's concern),
  are keyed to the snapshot they accelerate, and are invalidated when the scene changes.
  This is the one deliberate mutable exception, and it is *outside* `DerivedScene` and
  `PreparedRenderState` — neither immutable layer is mutated during a render.
- **Caches built BEFORE a pass stay immutable, in the layer that owns them:** the
  **TLAS** (built at seal — D24) in **`DerivedScene`**; the **light samplers** and
  **photon maps** (built by `prepare` — D22) in **`PreparedRenderState`**. These are
  computed-once *inputs* to a pass, not during-render-populated, so they are sealed and
  shared by refcount across frames where unchanged.

So the layered render-time picture is: `DerivedScene` over the frame's time interval
(immutable, config-independent; a single instant in the v1 no-motion-blur path, an
AnimatedTransform-over-`[t0,t1]` + motion BVH in the D31 follow-on) → `PreparedRenderState`
(immutable, config-dependent) → the render pass, which evaluates `at(τ)` read-only per
sample and owns the **render-local mutable** irradiance/accumulation caches. Animation is
**not** deprecated; it is per-frame derivation over a sequence of sealed snapshots, and
**motion blur is preserved** (gated — D31).

---

## 3. Delete / Evolve / Reuse

| Component (file) | Fate | Rationale |
|---|---|---|
| `AsciiSceneParser::ParseAndLoadScene` whole-file loader (`AsciiSceneParser.cpp:10431`) | **Evolve** | Becomes the *full-derive special case* (incremental derive vs empty cache). The chunk-walk + `Finalize→AddX` is retained as the **REBUILD backend**, driven from the CST instead of re-tokenizing text. The `FOR`/`DEFINE`/`hal`/`$(...)` macro front-end is removed by Facet 1 (L3); Facet 2 consumes the post-evaluation CST. |
| `IAsciiChunkParser::Finalize` per chunk | **Reuse (re-homed)** | This *is* the per-node L1 derivation rule. Keep verbatim as the rebuild emitter; it now runs per dirty node, not per file. |
| `IJob::AddX/SetX` surface + `Job.cpp` | **Reuse, with order-independence fixes** | The construction API stays; §4 lists the specific ordering effects to remove. |
| `GenericManager<T>` (name-key + identity version/serial) | **Reuse** | Already the order-independent store + freshness token Facet 2 needs (§2.6). It uses a mutable `std::map`, so the O(closure)/O(log N) snapshot claims are **gated on making the manager roots persistent immutable containers (D23)**; the honest v1 fallback (copy-on-snapshot) is O(N_entities)/snapshot (§2.1/§2.3/§6.1). |
| `SceneEditor::ApplyObjectOpForward` / `ApplyForwardMutation` (`SceneEditor.cpp`) | **Reuse (re-homed as the APPLY backend)** | The fine-grained mutators for value-only edits — now invoked **on the builder's closure copies, sealed before publish** (D11/D12), not on a shared live object. Strip the `SceneEdit`-record coupling; drive from `(NodeId, slot, value)` (D9). |
| `*Introspection::SetSlotValue` (Material/Media/Light/Camera) | **Reuse** | The per-domain slot setters the APPLY backend calls (on COW copies — D1). |
| `BumpLightTopologyGeneration` + the `…IfEmitterSetChanged`/`…IfMaterialEmits` helpers + the `LightManager` self-invalidate callback | **Reuse (as the L1/L2→L5 traced-edge implementation)** | The emissive→light-topology dependency edge already exists imperatively; Facet 2 makes the **traced** graph (D4) drive it. The coarse generation counter stays as L5's dirty flag. |
| Phase-B reconciliation — derive half (`RayCaster::AttachScene` realize, `PrepareForRendering`, `RenderParallelScope`) | **Reuse, unchanged** | Already incremental + correctly staged: realize + the **full TLAS rebuild** (D24) run on the `DerivedSceneBuilder` before `seal()` (→ `DerivedScene`, config-independent — D22). Facet 2 only feeds a precise dirty set. Under D1 the freeze guard is a DEBUG assertion, not a parking prerequisite (§2.7–2.8). |
| Phase-B reconciliation — prepare half (`RebuildLightSamplers`, `BuildPendingPhotonMaps`) | **Evolve (named prerequisite — D32/D33)** | Runs in `prepare` (→ `PreparedRenderState`, which **owns** the samplers/photon maps, moved in from the RayCaster — D22), but **cannot be reused unchanged**: `BuildPendingPhotonMaps` *mutates* the `Scene`'s pending flags/maps/gather params (`Scene.cpp:750`) and light samplers are `RayCaster`-owned — you can't build either by mutating a **sealed immutable** `DerivedScene`. Refactor both to `build(const DerivedScene&, PreparedRenderStateBuilder&)` reading through **non-mutating const input APIs** (D32), and make stochastic prep draw from the `RenderConfig` **seed**, not `rand()` (`RandomNumbers.h:32`), so `prepare` is deterministic/cacheable (D33). |
| `SceneEdit` value record (`SceneEdit.h`) | **Delete (owned by Facet 3)** | The edit carrier becomes the CST diff. The *op semantics* are absorbed into §2.5's apply-table; the struct goes. |
| `EditHistory` (`EditHistory.h`) | **Delete (Facet 3)** | Undo/redo = CST version history. |
| `DirtyTracker` (`DirtyTracker.h`) | **Delete (Facet 3)** | The memo cache's reverse-dependency index + phase-B generations replace the dirty channels; round-trip-save dirtiness is moot once text↔CST is lossless (Facet 1/3). |
| Transaction snapshot/rollback machinery in `SceneEditor`/`EditHistory` | **Delete (Facet 3)** | Rollback = restore a prior CST version + drop the memo cache. |
| `SaveEngine` text-splicing (`SaveEngine.cpp`) | **Delete (Facet 1/3)** | Serialization is the CST's faithful text projection (INV-4); no diff-and-splice needed. |
| `Job`'s round-trip metadata (`SourceSpanIndex`, `TransformSnapshot`, `OverrideSpanIndex`, `Job.cpp:276-279`) | **Delete (Facet 1/3)** | Span-tracking existed to splice text edits back; obsolete under canonical-CST. |

**Net for Facet 2:** delete *zero* engine code; **reuse** the loader's emit rules,
the managers, the apply-layer mutators, and the **derive-half** phase-B pipeline
(realize + TLAS) unchanged; **refactor** the **prepare-half** phase-B pipeline
(light samplers + photon maps) to a **non-mutating builder** (`build(const
DerivedScene&, PreparedRenderStateBuilder&)`) and a **seeded** stochastic path — the one
named engine-side prerequisite (D32/D33); **add** the memo cache + dependency graph +
diff-driven dirty propagation as a new thin layer *above* `IJob`, run as **cancellable
phases of the render arbiter** (D34). The deletions are all in the *edit/history/save*
wrapper, owned by Facets 1/3.

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
   (`referenceCategories` is only a UI ref-picker hint — D4; rename's referrer rewrite
   uses the traced `ReferenceUse` records — D14.) (b) Dangling refs (a name
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

- The new snapshot is a COW **reverse-dependency-closure copy** (D11): the affected
  records (and their referrer closure) become holes (or fallbacks), while **every node
  outside the closure is shared by reference with the prior snapshot** (D11 structural
  sharing). An unaffected object trivially survives because the new snapshot *points at
  the same object* — there is no in-place mutation to disturb it.
- Rendering proceeds against the partially-holed new snapshot (holes as null-material
  / not-rendered). The preview stays live; the diagnostics list shows what's broken.
- If a commit is *catastrophic* (e.g. the active-rasterizer node — a `RenderConfig`
  input to `prepare`, D22 — is holed so no valid `PreparedRenderState` can be built),
  **do not swap** — keep rendering the retained **last-good `PreparedRenderState`** (the
  most recent that derived *and prepared* without a hard error, §2.8) and surface the
  error. Because both layers are immutable and refcounted, this is just "don't publish
  the new pointer." Never a black frame with no explanation.

### 5.3 Distinguishing "incomplete mid-edit" from "wrong"

Derivation is **not** commit-only (D1). A gesture (drag, an agent emitting a burst of
patches) derives its **uncommitted head** — each pointer-move advances the uncommitted
CST head (debounced per O2) and derives a **cheap, ephemeral preview snapshot** (D1,
§2.8); only at **gesture end** do the intermediate roots **coalesce into one committed
version** (one undo unit). So previews routinely derive uncommitted state. What is
*debounced away* is the per-keystroke re-parse of a half-typed text token (a syntactic
incompleteness that never produces a parsed CST to derive), **not** semantic
derivation. A transiently-dangling ref inside an otherwise-parseable preview head is
**expected** and derives to a hole with a **suppressed/transient diagnostic**; a hole
that survives to the **committed** version at gesture end is a *real* semantic error
worth surfacing. (At a tighter 60Hz cadence — see §6 — that "tentative hole, suppressed
diagnostic during active typing" state simply becomes the steady state for every
preview frame; flagged as the cost of that path.)

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
   - **Editing a shared painter used by 500 materials.** This is the worked example
     that **D11 corrects** — the earlier draft's "copy the painter, share the 500
     materials, fan-out is O(1)" claim is **wrong**. The engine scene is a raw-pointer
     graph: each material holds a **direct pointer** to the painter
     (`LambertianBRDF.h:37`), so a material *shared* into the new snapshot would keep
     pointing at the **old** painter. **You cannot share a referrer of a changed
     node** (D11). The correct COW (D11) copies the painter's **reverse-dependency
     closure**: the painter + all 500 materials that reference it + their referrers up
     to the objects that bind those materials, repointing the copies; everything
     **outside** that closure is shared by refcount. (The **TLAS is not part of the
     copied closure** — under D24 v1 **fully rebuilds** it from the new objects, O(N log
     N), rather than path-copying leaves; §6.3.) Cost is therefore **O(closure / fan-in
     of the painter) + one full TLAS rebuild** — for a 500-fan-in painter the closure is
     genuinely large, but it is **bounded by fan-in and still dwarfed by the ensuing
     render**, not O(scene). **Render stays direct-pointer** (the closure copies hold
     correct pointers into the new snapshot's immutable objects).
     **Gating (D23):** the "O(closure)/O(log N)" half of this claim assumes the manager
     roots + derivation cache are **persistent immutable containers** — the honest v1
     fallback uses copy-on-snapshot mutable maps and is **O(N_entities)** per snapshot
     (§2.1/§2.3). **Optimization (named, deferred — D11):** for very-high-fan-in classes
     (painters, materials), a **per-snapshot indirection table** (objects reference by
     id, resolved through the snapshot's table) would collapse such an edit back to
     O(log N) at the cost of a render-time lookup — adopt **only if** profiling shows
     closure-copy is a bottleneck. A **first implementation may full-rebuild** (closure
     = everything) for correctness, then add closure-tracking; the design *target* is
     closure-copy. **Open:** are all painter kinds mutable via a copy-then-set (so the
     APPLY backend can re-derive just the painter copy inside the closure), or do some
     require a full rebuild? Audit needed; likely add painter slot-setters mirroring
     the material introspection setters.

2. **Memo-key hashing cost vs. diff-from-Facet-1.** Recomputing input-hashes for
   every node on every commit is O(scene). For a 10k-object scene that may itself
   exceed the budget. **Preferred:** Facet 1 emits a *structural diff* (changed
   node set) so Facet 2 hashes only changed nodes + their forward cone. **Open
   dependency on Facet 1:** does the CST diff give node-level granularity cheaply?
   If not, Facet 2 needs its own per-node hash cache (an extra field per CST node).

3. **TLAS rebuild is full in v1 — DECIDED (D24), not open.** `PrepareForRendering`
   rebuilds the whole top-level BVH when *any* object bbox moves
   (`InvalidateSpatialStructure` is coarse). D24 **accepts this for v1**: a full rebuild
   is O(N log N) and **« the render**, edits are debounced, and today's interactive
   editor already eats it on every transform edit (a known-tolerable cost at current
   scene sizes). The D11 "O(log N) TLAS path-copy" is therefore **withdrawn** — the TLAS
   is **not** part of the reverse-dependency closure copy (§6.1); it is rebuilt
   wholesale into the new `DerivedScene` at seal (§2.4/§2.7/§2.8). **Named future
   prerequisite (not v1):** an **incremental TLAS** — a persistent BVH with path-copy,
   or refit-with-periodic-rebuild — for cheap transform edits on very large scenes.
   Until that lands, a transform edit's cost includes a full TLAS rebuild (still « the
   render); Model B invites much larger scenes, which is what would justify the future
   work.

4. **Light-sampler rebuild granularity (a `prepare`-layer concern — D22).** L5 is a
   single coarse generation counter — *any* light-topology change rebuilds the *entire*
   `LightSampler` alias table (`RebuildLightSamplers`, now run in `prepare`, sealing into
   `PreparedRenderState` — D22). Editing one of 1000 area lights rebuilds all 1000. The
   env-IBL continuous-PMF architecture (CLAUDE.md) makes this correct but not
   incremental. **Open:** incremental alias-table update vs. full rebuild — likely out of
   scope for v1 (matches today), flag for a later perf pass. (A `RenderConfig`
   light-sampling-strategy change also re-runs this, since the samplers are
   config-dependent — D22.) **Prerequisite (D32/D33):** the rebuild must read the sealed
   `DerivedScene` through **non-mutating const input APIs** into a
   `PreparedRenderStateBuilder` (light samplers are `RayCaster`-owned today), and any
   stochastic prep that feeds it must draw from the `RenderConfig` seed, not `rand()` —
   §2.1/§2.8.

5. **`-ffast-math` and "unchanged" detection.** Memo equality must be byte/hash
   equality of the §2.3 key (the trivia-insensitive *derivation key* — D15 — +
   traced-input versions + *resolved literal-value* bit patterns), never a NaN-sentinel
   "not set" test
   (P-FFMATH; memory `ffast-math: no infinity`). The literal-value component of the
   key must hash floating-point params **by bit pattern**, and "absent param" must be
   an explicit presence bit (the descriptor default), not a NaN. Mechanically enforce
   via the existing `SourceHygieneTest` pattern.

6. **Determinism vs. the constant prelude (§4.3).** The honest statement is
   `DerivedScene = derive(prelude ⊕ CST, AssetManifest, t)` (D5/D21). **Open for
   synthesis:** should the prelude (null defaults, default shader-ops) be an *explicit*
   part of the canonical document (visible, diff-able, agent-editable) or an *implicit*
   engine constant? The thesis ("everything is in the diff-able program") argues for
   explicit; UX argues for implicit-but-introspectable. Recommend implicit +
   introspectable.

7. **O2 working-assumption (debounced-commit).** §2.8 designs for debounced-commit.
   Note that **D1 already removes the old "60Hz needs no-park, which needs a second
   representation" tension**: the no-park snapshot-swap is now the *baseline* model
   at any cadence, and an **immutable COW snapshot is explicitly NOT the
   "second mutable representation" hazard** INV-1 forbids — it is an immutable value
   with structural sharing, derivable from `(CST, AssetManifest, t)` (+ `RenderConfig`
   for the prepared layer) and droppable at will, not a mutable mirror of the CST that
   must be kept in sync. So the remaining deltas if synthesis chose 60Hz incremental
   derivation are narrower: (a) §5.3's "tentative hole, suppressed diagnostic" state
   becomes mandatory during active typing; (b) the memo-key recompute must be truly
   O(changed), not O(scene), every frame; (c) snapshot allocation/refcount churn per
   frame must stay cheap (structural sharing makes each snapshot's CST path-copy
   O(log N) — D16 — and the derived-scene copy O(closure / fan-in) — D11 — **once the
   persistent containers of D23 are in place; the v1 fallback is O(N_entities) and the
   full TLAS rebuild of D24 is O(N log N)** — so 60Hz × large scenes wants measurement
   either way). **Recommendation: keep debounced-commit** — 60Hz re-derivation
   of a production spectral renderer's scene graph buys little (the render itself is
   the latency floor). This is now a *latency/throughput* preference, not a
   correctness/INV-1 argument, since D1's snapshot model is safe at either cadence — and
   since **derive/prepare/render run as cancellable phases on the render arbiter, off the
   edit thread (D34)**, the edit thread never blocks regardless of cadence; a faster
   cadence just means more in-flight phases get cancelled-and-restarted before they
   publish.

---

## 7. Cross-facet dependencies & assumptions

- **From Facet 1 (CST):** I assume (a) **immutable, process-stable per-occurrence
  `NodeId` identity** (D9), living in the **red layer / a side-map, not the shared
  green node** (D15), surviving rename/value-edit and **best-effort** across reparse
  (structured edits exact; whole-region reparse matches by position+content, unmatched
  durable refs invalidated/flagged — D15) — the memo cache keys on it; **name-path**
  (resolved to NodeId per version) is the addressing layer,
  and the manager entry is still name-keyed (D9, §2.6); (b) a **node-level structural
  diff** (or cheap per-node identity) so §2.4 hashes only changed nodes, not the
  whole scene (load-bearing for the latency budget — see §6.2); (c) **a
  trivia-insensitive *derivation key*** (D15) is available for the memo key (distinct
  from the green node's lossless, trivia-*sensitive* content hash — D15 — which does the
  structural sharing/dedup), wide child sequences are a persistent **rope** giving
  O(log N) position/child lookup and edit (D16), and the descriptor's
  `referenceCategories` is exposed **as a UI ref-picker hint only** (D4; rename uses
  traced `ReferenceUse` records — D14) — I do **not** build the dependency graph from
  it; the graph is **traced during derivation** (D4); (d) the **AssetManifest** (D5/
  D17) — resolved identity + (size,mtime)-prefilter→content-hash fingerprint per
  referenced asset — is supplied as a derivation input (its generation is the
  `assetManifestGen` stamp axis — D29), and **time** — a **shutter interval `[t0,t1]`**
  plus the **active animation name** (D21/D31; both `DerivedStamp` axes — D29) — is a
  further derivation input for animation (Facet 1/6 own the manifest's construction +
  the watcher; the timeline/time evaluation is CST-driven); (e) declarative iteration
  (L3) is already expanded into either *derived instances* (a generator node I treat as
  one L3 producer) or *explicit separately-editable entities* (N independent L1/L2
  nodes) — Facet 2 treats each per its kind; (f) the manager roots + the
  **artifact-scoped** derivation cache (held on the `DerivedArtifact`/`PreparedArtifact`,
  not the `Version` — D30) + the per-`Version` `identityRoot` side-map are **persistent
  immutable containers** (D23) for the O(closure)/O(log N) headline — a **named
  infrastructure prerequisite** (the honest v1 fallback is copy-on-snapshot O(N);
  §2.1/§2.3/§6.1). **Conflict flag:** if Facet 1
  cannot cheaply give node-level diffs, §6.2's mitigation (per-CST-node hash cache) adds
  state to the CST that Facet 1 must own.
- **To/with Facet 3 (edit model):** Facet 3 deletes `SceneEdit`/`EditHistory`/
  `DirtyTracker`/transactions/`SaveEngine`; I **reuse the mutation primitives**
  under them (`ApplyObjectOpForward`, the `*Introspection` setters) as my APPLY
  backend, re-homed to take `(NodeId, slot, resolved-value)` from the CST diff (D9)
  and to run **on closure-copies in the builder, sealed before publish** (D11/D12).
  **Assumption:** Facet 3's "apply a CST edit" commits a cheap CST `Version` (D34); the
  **render arbiter** then calls my
  `derive(CST, CST', AssetManifest, t, animationName, cache)` (D5/D21/D31), and a
  render/integrator change calls `prepare(DerivedScene, RenderConfig)` (D22, with the
  seed in `RenderConfig` — D33) — i.e. Facet 3 owns *producing* the new CST + version
  history; I own *turning the diff into a new sealed immutable `DerivedScene`, then a
  `PreparedRenderState`*, **asynchronously on the arbiter as cancellable phases** (D34).
  The **derive→seal→prepare→seal→render / pass-boundary atomic-swap** seam (§2.8,
  D12/D22/D34) is shared mechanism; assume Facet 3/4 drive the commit *and gesture
  previews of the uncommitted head* (D1, §5.3) and supply `RenderConfig` (incl. the
  render-time integrator override, which re-runs only `prepare` — D22). Identity flows
  through edits: staged-edit state (`GestureBuffer`, working head) carries
  **`{greenRoot, identityRoot}`**, and `Version = {greenRoot, identityRoot, metadata}`
  with **no cache** (D30/D36) — the cache hangs off the stamp-keyed artifacts I own, not
  the `Version`. History is **CST-only** (D28); re-deriving an old version uses current
  asset bytes. (No parking-for-safety; cancel-and-park is an optional coalescing
  optimization Facet 3/4 may invoke.)
- **To Facet 4 (dynamic UI):** the memo cache's per-node derived records + holes +
  diagnostics are what the UI binds to (a `Widget`/`ViewNode` binds by a real `NodeId`
  field, and selection / an `EditIntent` carry the `NodeId` — D9/D36; name-path is kept
  for display/addressing only — reflecting its node's derived state / error). I expose a
  read API over the cache; I assume the UI does not mutate derived state directly
  (INV-1) — it edits the CST, which re-derives a new snapshot.
- **To Facet 5 (agentic surface):** my structured per-node diagnostics (§5.1) are
  the "structured errors" the MCP `validate→derive→render` loop returns. I assume
  the agent edits land as CST diffs through Facet 3, then trigger my derive.
- **Decision-conformance check (D1–D37):** the design honors **D1** (immutable COW
  snapshot + atomic swap; no parking-for-safety — §2.1/§2.8), **D4** (traced
  dependency graph; memo key = derivation key + traced-input versions, so `expr(A)` ≠
  literal `5` — §2.2/§2.3), **D5** (input = `(CST, AssetManifest)`; output paths
  excluded — §2.9), **D9** (NodeId = lineage identity, name-path = addressing —
  §2.3/§2.6); the **round-2 amendments**: **D11** (COW is reverse-dependency-closure
  copy, not "share a referrer of a changed node"; O(closure/fan-in); first cut may
  full-rebuild — §2.1/§5.2/§6.1), **D12** (build into a `DerivedSceneBuilder` → phase B
  on the builder → *seal* → publish; adopt at a **pass** boundary only — §2.1/§2.4/§2.8;
  ownership now split per D22), **D13** (expose `headVersion` + the derived/prepared
  stamps; render/graph stamp the latter, whose `cstVersion` may lag/last-good —
  **refined to stamps + DAG-ancestry by D29** — §2.8), **D14** (rename uses
  traced `ReferenceUse` records, not `referenceCategories` — §2.2/§4.2/§8), **D15**
  (three concepts: lossless content hash / trivia-insensitive *derivation key* = the
  memo key / `NodeId` lineage in the red layer — §2.3), **D16** (wide child sequences are
  a persistent rope, O(log N) — §2.1/§6.7), **D17** (asset identity = (size,mtime)
  prefilter → content hash, the authoritative memo-key input; atomic
  temp+fsync+revalidate+rename save — §2.1/§2.9), **D18** (corrected first-slice fixture
  with a real reference chain + asset phase — §8), **D20** (persistent cache, now
  **artifact-scoped — keyed by full stamp per D30**, not Version-carried; atomic
  outgoing-edge replace on re-derive, purge on delete — §2.3); and the **round-3
  amendments**: **D21** (animation = per-frame derivation, **time is a derivation
  input** — refined to a time *interval* by D31; caches *populated during* a pass —
  irradiance cache, accumulation — are **render-local mutable**, not in any immutable
  layer; caches built *before* a pass stay immutable — §2.1/§2.10), **D22** (two-layer
  split: **`DerivedScene = f(CST, AssetManifest, t)`** owns geometry/materials/emitters/
  **TLAS**, config-INDEPENDENT; **`PreparedRenderState = prepare(DerivedScene,
  RenderConfig)`** owns **light samplers + photon maps**, config-DEPENDENT; `RenderConfig`
  is an explicit third input; a render-time integrator override re-runs only `prepare` —
  §2.1/§2.2/§2.4/§2.7/§2.8), **D23** (the O(closure)/O(log N) headline is **gated on
  persistent immutable containers** for the manager roots + derivation cache + identity
  side-map; honest v1 fallback = copy-on-snapshot O(N_entities) — §2.1/§2.3/§6.1), **D24**
  (**v1 fully rebuilds the TLAS**, O(N log N) « the render; the D11 O(log N) TLAS
  path-copy is withdrawn; incremental/refit/persistent-BVH is a named future — §2.1/§2.7/
  §6.1/§6.3), **D28** (history preserves the **CST only**; re-derivation uses **current**
  asset bytes — the manifest is re-stamped, an old render may differ; content-addressed
  asset store is a named future, not core — §2.9); and the **round-4 amendments**:
  **D29** (full **`DerivedStamp = {cstVersion, assetManifestGen, animationName,
  shutterInterval}`** / **`PreparedStamp = DerivedStamp + {renderConfig, cameraOverride,
  samplingSeed}`**; cache = full-stamp equality, staleness = `cstVersion` **DAG
  ancestry**, never `<` — §2.1/§2.3/§2.8), **D30** (cache lives on a **`DerivedArtifact`
  keyed by `DerivedStamp`** / **`PreparedArtifact` by `PreparedStamp`** in a stamp-keyed
  LRU; **`Version = {greenRoot, identityRoot, metadata}`** has no cache; one Version →
  many artifacts — §2.1/§2.3), **D31** (motion blur **preserved**: a motion-blurred
  frame's `DerivedScene` is **time-INTERVAL-parameterized** — AnimatedTransform over
  `[t0,t1]`, renderer evaluates `at(τ)` read-only per sample; TLAS → **motion BVH**;
  **gated**, v1 = single-time/no-motion-blur, motion blur a named follow-on; animationName
  + shutterInterval are derivation inputs — §2.1/§2.2/§2.4/§2.10), **D32** (`prepare`
  reads the sealed `DerivedScene` through **non-mutating const input APIs** into a
  **`PreparedRenderStateBuilder`**, then seals; **named prerequisite** refactor of
  `BuildPendingPhotonMaps`/light-sampler construction from "mutate the Scene" to
  `build(const DerivedScene&, PreparedRenderStateBuilder&)` — §2.1/§2.4/§2.8/§3/§6.4),
  **D33** (`prepare` **deterministic**: `RenderConfig` carries a **sampling seed /
  RNG-stream id** used by stochastic prep instead of `rand()`; seed in `PreparedStamp` →
  pure/cacheable/reproducible — §2.1/§2.4/§2.8), **D34** (derive→seal→prepare→seal→render
  run as **cancellable phases of the render arbiter, off the edit thread**; the edit
  thread only commits a cheap `Version`; a newer head cancels+restarts — the source of
  the head-vs-derived lag — §2.4/§2.8/§6.7); and it references **D35** (rename reuses the
  **one** derivation resolver / derive-to-head — Facet 3's concern, no conflict here),
  **D36** (identity propagates through edits: `GestureBuffer`/working head carry
  `{greenRoot, identityRoot}`; `Widget`/`ViewNode`/`EditIntent`/selection store the
  `NodeId` — Facet 3/4; the cache keying on NodeId is unchanged — §2.3/§2.6/§7), **D37**
  (migration is comment/token-aware — Facet 1/6's migrator; §4 reads only declarative
  config); and it references **D2** (green-node
  content hashing for sharing), **D6** (shared fingerprint / conflict UX — §2.9), **D10**
  (the first-slice gates — §8), **D25** (rename runs against a head-stamped trace —
  Facet 3/§4.2's concern, **superseded by D35**: rename reuses the one derivation
  resolver, no separate tracing pass), **D26** (the per-`Version` `identityRoot` is the
  home for the NodeId records the artifact-scoped cache keys on; **completed by D36** —
  `{greenRoot, identityRoot}` propagate through edits — and **note D30 drops
  `derivationCacheRoot` from `Version`**: the cache is artifact-scoped, not Version-held;
  §2.3, depends on D23), **D27** (all `>` commands migrated/retired — Facet 1/6's
  migrator; **corrected by D37**, comment/token-aware; §4 reads only declarative config). The **residual conformance sweep** items are addressed: (i) the memo key is
  the structural/traced-input *derivation key*, never resolved-value (§2.3); (ii)
  derivation is **not** commit-only — gesture previews derive the uncommitted head into
  ephemeral preview snapshots, committing once at gesture end (§2.8/§5.3); (iii)
  name-path is *addressing*, NodeId is the stable lineage identity (§2.3/§2.6). It honors
  the charter's locked decisions L1 (pure derivation), L2 (one pathway — the apply/rebuild
  split is *one* derive function, not two clients; `prepare` is the *one* config-dependent
  pass, not a second derive client), L3 (consumes post-expansion CST), L5 (identity is
  first-class — realized as NodeId + name-path per D9, homed in the D26 side-map), L6
  (descriptor-as-schema; `referenceCategories` is a UI ref-picker hint, not a parallel
  edge schema). **No conflicts with D1–D37 or the locked decisions.** Open decisions
  touched: O1 (designed for CST-canonical; the memo key's derivation-key +
  traced-input-version form makes the text-canonical delta small — a text edit that
  re-parses to the same green tree with the same traced inputs is a memo hit either way),
  O2 (designed for debounced-commit; §6.7 argues to keep it, now as a latency preference
  since D1 is safe at either cadence).

---

## 8. First-slice implications (minimal end-to-end vertical)

Facet 2 contributes to the **single canonical phased fixture + shared gate set in
D10, as corrected by D18** (which supersedes the four formerly-nominated first slices;
D18 fixes D10's fixture — `uniformcolor_painter` has no reference, and phase 3 had
omitted the material node). The relevant fixture phases for this facet are **D18 phase
2** (`+ uniformcolor_painter` **and** `+ lambertian_material { reflectance <the uniform
painter> }` — the material is the **first reference**, exercising the ref-picker + the
dependency edge) and **D18 phase 3** (`+ standard_object { geometry <sphere> material
<lambertian> }` — the real **geometry→material→object** chain, exercising cross-node
references, rename integrity (D14), and the dependency graph end-to-end), with phase 4
(`+ expr(...)`) exercising traced-input invalidation, phase 5 (`+ instance_array`) the
generator path, and **D18 phase 6** (`+ image_painter` / a mesh-backed geometry) adding
the **asset-backed node** that makes **G5** (AssetManifest content-hash invalidation,
D17) and the external-file-conflict path (D6/D17) testable. Facet 2's contributions to
that fixture:

1. **Take D18 phase 3's real chain** (`sphere_geometry` + `uniformcolor_painter` →
   `lambertian_material` → `standard_object`) — the simplest
   producer→producer→consumer chain that exercises a traced name-ref edge (the
   material's `reflectance` → painter, and the object's `material`/`geometry` →
   entities) and a transform. All four already have descriptor + `Finalize` + `AddX`.
2. **Build the minimal memo cache + traced graph for these node kinds:** L0 values,
   L1 entities (geometry, painter, material), L2 object, the name-ref edges
   **recorded by tracing the derivation** (D4 — *not* read from
   `referenceCategories`, which is only the UI ref-picker hint; rename uses traced
   `ReferenceUse` records — D14), and the L2→L4 (TLAS, **full-rebuild v1** — D24) edge.
   Both layers are immutable + sealed (D1/D12/D22): a config-independent `DerivedScene`
   (geometry/material/object/TLAS) and a config-dependent `PreparedRenderState` (here
   trivial — the sphere is non-emissive, so the light sampler is empty and no photons).
   The cache keys on NodeId (D9/D15, homed in the per-`Version` `identityRoot` — D26/D36)
   and lives on a **`DerivedArtifact` keyed by the `DerivedStamp`** (and a
   `PreparedArtifact` by the `PreparedStamp`) in a stamp-keyed LRU, **not on the
   `Version`** (D29/D30) — **structurally shared across stamps once the persistent
   containers of D23 land; the v1 fallback is copy-on-snapshot O(N)**. (For this slice the
   stamps are trivial: one CST version, one asset generation, a single-time interval, one
   `RenderConfig` + seed — but the keying is the real round-4 shape.)
3. **Wire the two backends:** REBUILD = the existing `Finalize→AddObject`/
   `AddSphereGeometry`/`AddLambertianMaterial`; APPLY = `SetObjectPosition` +
   `RunObjectInvariantChain` for a transform edit, `MaterialIntrospection::
   SetSlotValue` for the material's `reflectance` — both **on closure-copies in the
   builder, sealed before publish** (D11/D12).
4. **Demonstrate the core behaviors (mapping to D10's gates G2/G3/G4):**
   - edit the sphere's `position` → APPLY path → new snapshot shares everything but
     the copied object; only TLAS rebuilds (prove no entity re-derived; assert via
     the existing `GetSamplerRebuildCount` staying flat and a TLAS-rebuild counter
     ticking once) — **G3 minimal invalidation**;
   - edit the material's `reflectance` to a new color → APPLY path → no
     light-topology rebuild (sphere non-emissive), no TLAS rebuild;
   - rename the material the object binds → `NodeId`-preserving rename (D9); referrers
     are rewritten from the traced `ReferenceUse` records (D14), and the producer's
     manager **version bumps** → object rebinds via the version-staleness check (§2.6)
     — **G4 versioning** (round-trip byte-identical after undo, G1).
   - *(phase 4)* edit a `Double` fed by `expr(A)`, then change `A` → the consumer's
     traced-input version bumps and it re-derives, while a sibling literal `5` stays
     a memo hit (D4).
5. **Prove the memo hit (G1/G3):** add a comment / reformat whitespace in the CST →
   diff shows a text change → the green node's *content* hash changes (D15) but the
   node's **derivation key (trivia-insensitive) and traced inputs are unchanged** (D15)
   → **zero** re-derivation (the headline INV-3/INV-4 win, measurable as "derive did
   nothing"; the new snapshot shares 100% by reference).

This slice stands up the cache, the **traced** graph build, the diff→dirty→propagate
loop, the derive→seal→prepare→seal→render path **run as cancellable arbiter phases**
(D12/D22/D34), and both backends — the skeleton every other chunk type plugs into by
virtue of being descriptor-driven. It reuses the engine wholesale (managers, apply-layer,
the **derive-half** phase-B realize+TLAS pipeline) — the **one** engine-side change is the
named **prepare-half** refactor to a non-mutating `PreparedRenderStateBuilder` + seeded
prep (D32/D33), trivial here (empty sampler, no photons) but real shape — and adds only
the thin memo+graph + two-layer-snapshot layer. All five D10 gates (G1 round-trip,
G2 latency, G3 minimal invalidation, G4 versioning, G5 external inputs once the
asset-backed node enters at D18 phase 6) apply.

---

### Appendix: one-paragraph mental model

Today RISE *loads* a scene whole-file and then *mutates* it in place through a
separate editor with its own record/history/dirty machinery. Model B keeps the two
existing engines — the loader's `Finalize→AddX` (now the **rebuild** backend) and
the editor's `ApplyObjectOpForward`+introspection setters (now the **apply** backend,
run on **COW copies** — D1) — and puts a **memo cache + traced dependency graph** in
front of them, in **two sealed immutable layers** (D22). The first function is
`derive(CST, AssetManifest, t, animationName)` (D5/D21/D31 — **the time *interval* and
the animation name are derivation inputs**, so animation is per-frame derivation, not
mutation-during-render, and **motion blur is preserved** as a time-interval
AnimatedTransform the renderer reads `at(τ)` per sample — gated, v1 single-time),
producing a config-**independent** **`DerivedScene`** (geometry/materials/
lights-as-emitters/**TLAS**); the second is `prepare(DerivedScene, RenderConfig)` (D22),
which reads the sealed `DerivedScene` through **non-mutating const APIs** into a
**`PreparedRenderStateBuilder`** (D32) and draws stochastic prep from `RenderConfig`'s
**seed** (D33), producing a config-**dependent** **`PreparedRenderState`** (light
samplers + photon maps) — so a render-time integrator override re-runs only `prepare`.
Each layer is identified by a **stamp** (`DerivedStamp` / `PreparedStamp` — D29) and the
memo cache lives on a stamp-keyed **artifact** (`DerivedArtifact` / `PreparedArtifact`),
**not** on the immutable `Version = {greenRoot, identityRoot, metadata}` (D30). A
CST/manifest/time edit becomes
a node-diff; the **traced** graph (D4) turns the diff into the *minimal* set of rebuilds
(structural changes) and copy-then-apply value changes, materializes them in a mutable
`DerivedSceneBuilder` that **copies the reverse-dependency closure** of each changed node
and shares every node outside the closure by reference (D11), runs the config-independent
**phase B on the builder** (realize + a **full TLAS rebuild** — D24; no path-copy), then
**seals** to an immutable `DerivedScene` that **owns** its geometry/TLAS; `prepare` then
seals a `PreparedRenderState` that **owns** the light samplers/photon maps (D22) —
setting the *already-existing* phase-B/prepare dirty flags (light-topology generation,
TLAS-valid, photon-pending) precisely instead of via scattered hand-bumps. This whole
pipeline runs as **cancellable phases of the render arbiter, off the edit thread** (the
edit thread only commits a cheap CST `Version`; a newer head cancels+restarts — D34); the
renderer **atomically swaps to the sealed `PreparedRenderState` at a PASS boundary** — no
parking-for-safety; the old layers drain by refcount (D1, D12, D22). Caches *populated
during* a render (irradiance cache, accumulation) are **render-local mutable** state
owned by the pass, outside both immutable layers (D21). Determinism comes from deriving
in dependency order (discovered by tracing, fixing `Job`'s load-order landmines) and
keying the memo on the **derivation key** (trivia-insensitive semantic hash) **+
traced-input versions** (so `expr(A)` ≠ literal `5` — D4/D15) — and from a **seeded,
reproducible `prepare`** (D33; cache = full-stamp equality, staleness = `cstVersion` DAG
ancestry not `<` — D29); incrementality comes from the **artifact-scoped, stamp-keyed**
memo cache (D20/D30) + structural sharing — **with the O(closure)/O(log N) headline gated
on persistent immutable containers (D23); the honest v1 fallback is copy-on-snapshot
O(N_entities)**; safety comes from immutability plus deriving with holes
so one bad node never blanks the scene. History is **CST-only** (D28).
