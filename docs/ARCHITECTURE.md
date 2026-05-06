# RISE Architecture: Scene Immutability and Thread Safety

This is a focused architecture deep dive, not the full repo map. For the overall contributor entry points, start with [../README.md](../README.md), [../AGENTS.md](../AGENTS.md), and [../src/Library/README.md](../src/Library/README.md).

## Core Principle

**During active multi-threaded rendering, no scene object may be mutated by the renderer.**

The scene graph (geometry, materials, lights, camera, photon maps, spatial acceleration structures) is shared read-only across all rendering threads. Any per-thread mutable state must be owned by the rasterizer or thread runner, not by scene objects.

This principle is enforced through:
- `const IScene*` passed to rendering code (shaders, integrators, ray casters)
- `IScenePriv` providing non-const accessors only for setup/photon-tracing code
- Const-correct return types on `IScene` getters

## Rendering Pipeline Phases

### 1. Setup Phase (mutable)

Scene construction via `IJob`/`IScenePriv`: objects, lights, materials, cameras, and photon maps are created and configured. All `Set*()` methods on `IScenePriv` operate here.

### 2. Prepare Phase (build acceleration structures)

`ObjectManager::PrepareForRendering()` is called by each rasterizer after scene attach (and, for animation, after each frame's time evaluation). This eagerly builds the top-level BVH (or octree, for legacy callers) and shadow caches from current world-space bounding boxes. After this call, the spatial structures are immutable for the duration of that render pass.

### 3. Render Phase (immutable)

Multiple threads rasterize pixels concurrently. All scene access is through `const IScene*`. Each thread owns its own `RuntimeContext` containing:
- Random number generator
- Low-discrepancy sampler (optional)
- Stability configuration
- Path guiding field (optional, RISE_ENABLE_OPENPGL)
- Rasterizer state caches

## Per-Thread State Model

### RuntimeContext

`RuntimeContext` (`src/Library/Utilities/RuntimeContext.h`) is allocated per-thread by the rasterizer. Its `mutable` fields are correct because each thread has its own instance. It is not part of the shared scene graph.

### Thread-Local Mailboxing

Triangle mesh intersection uses `thread_local` mailbox state (`src/Library/Geometry/TriangleMeshGeometryIndexed.cpp`) to avoid redundant intersection tests during BSP traversal. Each thread maintains its own ray ID counter and stamp vector per geometry, eliminating the data race that would occur with shared mutable mailbox state.

## Acceptable Mutable Patterns

| Pattern | Location | Rationale |
|---------|----------|-----------|
| Reference counting | `IReference` / `Reference` | Atomic increment/decrement, thread-safe by design |
| Per-thread RuntimeContext | `RuntimeContext` | Each thread owns its own instance |
| Thread-local mailboxes | `TriangleMeshGeometryIndexed` | Per-thread, no sharing |
| Path guiding training stats | `PathGuidingField` | Atomic counters, updated between passes (not during) |
| BDPT integrator atomics | `BDPTIntegrator` | Atomic counters for cross-pass statistics |
| Irradiance cache | `IrradianceCache` | Mutex-guarded insert/query; cache is populated during a dedicated irradiance pass before the main render pass |
| FilteredFilm row locks | `FilteredFilm` | Per-scanline mutex; threads lock one row at a time during Splat(). Resolve() runs single-threaded after all render threads complete |
| AOV buffer accumulation | `AOVBuffers` | Written per-pixel by the owning thread during block rasterization; no cross-thread contention within a block |

## Known Exceptions

### SSS Pointset Lazy Initialization

Two subsurface scattering shader ops lazily build per-object point set octrees on first access:
- `SubSurfaceScatteringShaderOp`
- `DonnerJensenSkinSSSShaderOp`

These use `mutable PointSetMap pointsets` guarded by a mutex with double-checked locking. The lazy pattern is necessary because octree construction requires ray tracing the scene (evaluating irradiance at sample points), making pre-build during the prepare phase impractical. The mutex serialization makes this thread-safe.

### Animation / Temporal Sampling (pre-existing data race)

`IAnimator::EvaluateAtTime()` mutates keyframed scene elements (camera transform, object transforms, painter values) through stored pointers. During temporal sampling with motion blur, rasterizers call `EvaluateAtTime()` per-sample from multiple threads (`PixelBasedPelRasterizer`, `BDPTPelRasterizer`, etc.). This is a **pre-existing data race** that predates the immutability work.

`IScene::GetAnimator()` intentionally returns non-const `IAnimator*` to make this mutation visible rather than hiding it behind `const`. A proper fix would require per-thread interpolated state snapshots, which is a significant architectural change.

**Impact**: In practice, temporal sampling with multi-threaded animation is rarely used in production scenes. When it is used, the race condition typically manifests as minor temporal jitter rather than crashes, because the mutations are simple scalar writes to transform/camera parameters.

### Spatial Acceleration and Animation

The top-level BVH / octree in `ObjectManager` is built from world-space bounding boxes (`Object::getBoundingBox()` applies `m_mxFinalTrans` to geometry bounds). When objects have keyframed transforms, animation evaluation recomputes these transforms via `RegenerateData()` → `FinalizeTransformations()`, which can move objects outside their tree node placement.

**Per-frame animation** (`RasterizeAnimation`): The spatial structure is invalidated via `InvalidateSpatialStructure()` and rebuilt via `PrepareForRendering()` after each frame's `EvaluateAtTime()` + `SetSceneTime()`, before multi-threaded rendering begins. This ensures the top-level BVH/octree reflects current transforms for each frame.

**Per-sample temporal sampling** (motion blur within a single frame): `EvaluateAtTime()` is called per-sample from within threaded pixel rasterizers. Rebuilding the spatial structure per-sample is prohibitively expensive, so the BVH is built once for the frame's base time. This means per-sample object transform variations are not reflected in the spatial structure — a pre-existing limitation. In practice, the motion blur exposure window is typically small enough that objects don't move far outside their base-time bounds.

### Top-Level Acceleration (TLAS) — BVH default since 2026-05

`ObjectManager`'s default top-level structure is the same `BVH<>` template (`src/Library/Acceleration/BVH.h`) that `TriangleMeshGeometry{,Indexed}` uses per-mesh — with `Element = const IObjectPriv*`. Same SAH-binned BVH2 builder, BVH4 SoA collapse, and SIMD ray-vs-AABB kernel. Pre-2026-05 the default was no top-level structure at all (linear loop over every IObject); a `BSPTreeSAH<>` path existed but was never wired in by default. Migrating Sponza-class scenes (155 mesh objects) from no-TLAS through BSP to BVH4 produced a measured 7×+ wall-clock speedup.

`Job::InitializeContainers` constructs `ObjectManager(true, false, 4, 32)` — flag name `bUseBSPtree` is historical; semantically it now means "build a top-level BVH". The leaf cap of 4 (vs the per-mesh 4 used for triangles) is small because each top-level "leaf primitive" is a whole IObject — a leaf hit means descending into a per-mesh sub-BVH, much more expensive than a single triangle test, so the SAH builder is biased toward smaller leaves via `sahIntersectionCost = 8.0`. Tiny scenes (≤4 objects) skip the build via the `items.size() > nMaxObjectsPerNode` gate and use the linear-loop fallback path — overhead amortisation flips at very small N.

`ObjectManager::RayElementIntersection` requires native closest-hit semantics (only commit when the new hit is strictly closer than `ri.range`) because the BVH<> contract calls leaf processors with a shared `ri` and expects each call to be defensive about overwriting it. `BSPTreeSAHNode` had this guard externally (per-element local `myRI` + compare), so the per-prim `RayElementIntersection` could be sloppy; the BVH's per-node SoA SIMD design relies on the processor itself doing the closest-hit check inline. This is the same fix pattern as Tier-A cleanup §2 in the BVH retrospective for `TriangleMeshGeometryIndexed::RayElementIntersection`.

## Filtered Film And OIDN Denoiser Interaction

### FilteredFilm

`FilteredFilm` (`Rendering/FilteredFilm.h`) accumulates filter-weighted sample contributions across pixel boundaries for wide-support pixel filters (Mitchell-Netravali, Lanczos, etc.). `PixelBasedRasterizerHelper::UseFilteredFilm()` returns true when the pixel filter's support exceeds half a pixel (>0.501). When active, each sample is splatted to all pixels within the filter's support via `FilteredFilm::Splat()`, and the inline pixel estimate uses box weighting (`weight=1.0`). After the render pass, `FilteredFilm::Resolve()` overwrites the image with `colorSum/weightSum`.

When no pixel filter is specified, the default is **Mitchell-Netravali (1/3, 1/3)** with support 2.0, so the filtered film is active by default for multi-sample rasterizers.

### OIDN Denoiser Bypass

**Critical invariant**: When OIDN denoising is enabled, the filtered film resolve is skipped. OIDN is trained on raw Monte Carlo noise patterns and produces poor results on filter-reconstructed images — negative lobes and ringing from MN/Lanczos filters change the noise character in ways OIDN cannot handle. The denoiser receives the inline box-filtered estimate instead.

This is enforced in `PixelBasedRasterizerHelper::RasterizeScene()`:
```cpp
if( pFilteredFilm ) {
    if( !bDenoisingEnabled ) {
        pFilteredFilm->Resolve( *pImage );
    }
}
// ... then OIDN runs on the (box-filtered) pImage ...
```

BDPT is not affected by this because `BDPTRasterizerBase::RasterizeScene()` is a complete override that never uses `pFilteredFilm`.

### AOV Pipeline

AOV (Arbitrary Output Variable) buffers for OIDN are managed by `AOVBuffers` (`Rendering/AOVBuffers.h`). The base class `PixelBasedRasterizerHelper` owns a `pAOVBuffers` member allocated before the render pass when `bDenoisingEnabled` is true.

Two AOV collection strategies exist:
1. **Per-sample accumulation** (preferred): Rasterizers that use `PathTracingIntegrator` (e.g., `PathTracingPelRasterizer`) populate a `PixelAOV` struct per sample during integration. The rasterizer accumulates these into `pAOVBuffers` per pixel, weighted by the sample weight, and normalizes after the sample loop. This sets `AOVBuffers::HasData()` to true.
2. **Post-render retrace** (fallback): If `HasData()` is false after the render pass (e.g., standard shader-dispatch rasterizers), `OIDNDenoiser::CollectFirstHitAOVs()` fires camera rays to collect first-hit albedo and normals in a separate single-threaded pass.

The `PixelAOV` struct is shared between PT and BDPT rasterizers, defined in `AOVBuffers.h`.

## Design Decisions

### Why `IScenePriv` exists

`IScenePriv` extends `IScene` with non-const `*Mutable()` accessors and `Set*()` methods. This separates the setup API (used by `Job`, parsers, photon tracers) from the rendering API (const-only `IScene`). Rendering code never receives an `IScenePriv*`.

### Why `PrepareForRendering()` and `InvalidateSpatialStructure()` are const

The object manager is stored as `const IObjectManager*` in the scene. Both methods are declared const because they're called through the const scene pointer by rasterizers. The spatial acceleration structures and shadow cache are `mutable` members. `PrepareForRendering()` builds only if the structure doesn't exist yet. `InvalidateSpatialStructure()` destroys it so the next `PrepareForRendering()` rebuilds with current transforms. Both are called single-threaded between render passes, never during concurrent rendering. A warning is logged if lazy fallback triggers during rendering.
