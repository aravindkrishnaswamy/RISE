# RISE BVH Acceleration Plan

**Status**: Draft for review. No code changes made yet.
**Owner**: Aravind Krishnaswamy
**Scope**: Replace octree / BSPTreeSAH for **triangle mesh** acceleration with a modern BVH stack: SBVH builder, compile-time width selection (BVH2 / BVH4 / BVH8), float traversal + double intersect, beefed-up triangle intersection.

---

## 1. Executive Summary

**The problem.** RISE's triangle meshes currently use either an octree ([src/Library/Octree.h](../src/Library/Octree.h)) or a SAH BSP tree ([src/Library/BSPTreeSAH.h](../src/Library/BSPTreeSAH.h)) as their per-mesh acceleration structure. Both are several factors slower than a modern wide BVH on the kinds of meshes RISE renders (BioSpec faces, dense PLY scans, character meshes with non-uniform density). Ray-triangle intersection cost dominates wall-clock time on these scenes; a 3–5× win on intersection translates to roughly **1.5–2× end-to-end** on representative renders.

**The plan.** Four phases ending with an evaluation gate.

| Phase | Deliverable | Target speedup vs. octree |
|---|---|---|
| 1 | BVH2 + SBVH builder, replaces octree/BSP for triangle meshes | 2–4× |
| 2 | Precomputed Möller-Trumbore + optional Woop watertight intersection | +20–40% on top of Phase 1 |
| 3 | BVH4 + AVX2 / NEON SIMD bbox tests | +1.5–2× on top of Phase 2 |
| 4 | BVH8 (AVX-512 native, AVX2 via 2×4) | +1.3–1.6× on top of Phase 3 (AVX-512) |

After Phase 4 we evaluate before deciding on Phase 5 (compressed nodes) or Phase 6 (TLAS BVH replacing object-manager octree).

**Goals.**
1. Single modern acceleration structure for triangle meshes — no backward-compatibility cruft.
2. Cleaner parser surface and public API.
3. Bit-equal first-hit results vs. octree on regression scenes (at BVH width 2); statistical equivalence at wider widths.
4. SMS / BDPT / VCM caustic correctness preserved (firefly count within 5% of pre-BVH baseline at equal SPP).
5. Apple Silicon and Android arm64 first-class (NEON BVH4).
6. `.risemesh` binary format migrates cleanly: v1/v2 files auto-rebuild BVH on load, `meshconverter` produces v3 files with baked BVH, and the v3 format stays stable through Phase 4.
7. Animation (keyframed-painter-driven `DisplacedGeometry` rebuilds) runs **faster** than the current octree path, not slower, by exploiting topology stability via BVH refit (~10× faster than full rebuild on a 100K-tri displaced mesh).

**Non-goals (this initiative).**
- ObjectManager (TLAS) acceleration. Stays octree/BSP for now; revisited as Phase 6 if Phase 4 evaluation says so.
- BezierPatchGeometry acceleration. Bezier patch BLAS is a different leaf primitive; out of scope.
- SSS `PointSetOctree`. It's a point lookup, not ray intersection. Stays.
- PRISE legacy code under `src/PRISE/`. Untouched.
- TLAS BVH (deferred decision, Phase 6).
- Compressed nodes (deferred decision, Phase 5).
- Animation rebuild. Static scenes only. H-PLOC etc. not needed.

**No backward compatibility.** Per project decision:
- Old parser keys (`bsp`, `maxpolygons`, `maxrecursion`) for triangle meshes are **removed**, not deprecated. Scene files get swept.
- Public C++ API for mesh construction in [RISE_API.h](../src/Library/RISE_API.h) gets new signatures; old ones are **removed**, not kept as overloads.
- Octree and BSPTreeSAH remain in the codebase (they're still used by ObjectManager and BezierPatchGeometry), but the triangle-mesh integration to them is **deleted**.

---

## 2. Pinned Design Decisions

These were settled in discussion before this plan was written. Recorded here so we don't re-litigate them mid-implementation.

| # | Decision | Rationale |
|---|---|---|
| D1 | **Float traversal, double intersect.** AABBs and inner traversal math run in `float`. Final ray-triangle intersection runs in `Scalar` (double). | Halves traversal memory, enables full-width SIMD, keeps SMS/BDPT/VCM precision. See §3 for the careful documentation. |
| D2 | **Compile-time SIMD width selection.** `RISE_BVH_WIDTH` ∈ {2, 4, 8}, auto-detected from the active SIMD ISA, overridable. No runtime dispatch. Default 4 on AVX2/NEON/SVE2-128, 8 only on AVX-512, 2 elsewhere. **In practice, all three of the project's primary dev machines (Apple Silicon, modern Intel i9, Galaxy Fold 7) default to BVH4** — see §10.7. | Single binary per platform, no dispatch overhead. Simpler. |
| D3 | **Apple Silicon stays at NEON BVH4.** No 2×NEON BVH8. | Phase 4 stops; evaluation precedes any wider Apple Silicon work. |
| D4 | **No backward compatibility.** Remove old parser keys, old API signatures, and the octree/BSP integration in triangle-mesh classes. Sweep scene files. | Clean codebase end-state. |
| D5 | **`AccelerationConfig` struct**, no defaults in headers. Per [feedback memory rule](../../../.claude/projects/-Users-aravind-Documents-GitHub-RISE/memory/feedback_no_default_params.md), all parameters explicit at the call site. | Project convention. |
| D6 | **Stop at Phase 4 and evaluate.** Phases 5 (compressed nodes) and 6 (TLAS BVH) are deferred until we measure Phase 4 results. | Keeps initiative bounded. |
| D7 | **BLAS-only.** Object-level acceleration (ObjectManager) keeps its current octree/BSP. | TLAS cost is small relative to BLAS cost given typical RISE scene object counts. |
| D8 | **Templated BVH** mirroring existing `Octree<Element>` + `TreeElementProcessor<Element>` contract. | Mesh classes drop in via the same callbacks. Indexed and non-indexed mesh types share the template. |
| D9 | **No external dependencies.** SIMD wrappers are in-tree, hand-written. No xsimd, no highway, no Embree. | Keeps the build simple and portable. |
| D10 | **Watertight intersection is a compile-time switch** (`RISE_BVH_WATERTIGHT`), default OFF. Enabled per-build only after SMS regression scenes pass. | Watertight (Woop) costs ~5% on standard scenes; pay only when caustic-sensitive paths need it. |

---

## 3. Float Traversal / Double Intersect — Precision Policy (Careful Documentation)

This is the single most important design decision and the one most likely to cause subtle correctness regressions if mis-implemented. Documented here so the rationale, the proof obligations, and the implementation rules are explicit.

### 3.1 What's stored in float vs. double

| Datum | Type | Where it lives |
|---|---|---|
| Triangle vertex positions (authoritative) | `Scalar` (double) | `Triangle` struct, mesh polygon vector |
| Triangle vertex positions (precomputed for traversal) | `float` (3×3) | BVH leaf array, parallel to triangle index |
| Triangle precomputed edges P0/e1/e2 (Phase 2) | `float` (3×3) | BVH leaf array |
| Triangle Woop transform (Phase 2, optional) | `float` (4×3) | BVH leaf array |
| BVH internal node child AABBs | `float` (6 per child) | BVH node array |
| BVH leaf bbox (implicit in traversal AABB) | `float` | BVH node array |
| Ray origin / direction | `Scalar` (double) | `Ray` struct (unchanged) |
| Ray origin / direction (cached for traversal) | `float` (3+3) | Per-traversal stack frame |
| Ray inverse direction | `float` (3) | Per-traversal stack frame |
| Final hit `t`, `α`, `β` | `Scalar` (double) | `TRIANGLE_HIT` struct (unchanged) |
| Final hit point, normal | `Scalar` (double) | `RayIntersectionGeometric` (unchanged) |

### 3.2 Two-stage intersection: filter then certify

Every ray-triangle intersection runs in two stages:

1. **Filter (float).** Inside the BVH leaf, run the float Möller-Trumbore (or float Woop) on the precomputed triangle data. This is the SIMD-friendly fast path. It produces a candidate `t`, `α`, `β` in float. If the filter rejects (no hit, or `t` worse than current best), bail.
2. **Certify (double).** If the filter accepts, re-evaluate the intersection in `Scalar` (double) using the **authoritative** double-precision vertices from the `Triangle` struct, not the float copy. Replace the float candidate with the double-certified result. Compare against the running best `t` in double.

This is what Embree does. The float pass cuts the candidate set; the double pass guarantees the surviving hit is computed at double precision for downstream shading, manifold-walk perturbation (SMS), and BDPT/VCM connection geometry.

### 3.3 Conservative AABB padding

Float AABBs in BVH nodes must be **conservative** — they must contain the true (double-precision) triangle bounds, otherwise the traversal can miss valid hits at silhouettes. The build process:

1. Compute each triangle's bounding box in `Scalar` (double).
2. Convert to `float`, rounding **min downward** and **max upward** (use `nextafter` toward `±INFINITY`, or equivalently a 2-ULP pad).
3. When merging child AABBs into a parent AABB, do the merge in float — already conservative because the children are conservative.
4. SBVH spatial splits: clip the **double** triangle, then convert clipped sub-bounds to float with the same conservative rounding.

A 2-ULP float pad in single precision is roughly `2 × 2^-23 ≈ 2.4e-7` relative. For RISE scene scales (typically 0.001 to 100 in object space), this is tens of nanometers — well below any meaningful surface feature. Self-intersection epsilons (`NEARZERO` in [src/Library/Intersection/RayPrimitiveIntersections.h](../src/Library/Intersection/RayPrimitiveIntersections.h)) operate at much larger scales, so the padding is invisible.

### 3.4 Proof obligations

To call this design correct, we must demonstrate:

1. **No missed hits at silhouettes.** Render a scene with grazing rays across a tessellated curved surface (sphere mesh, cylinder mesh) and confirm no pixels show holes vs. octree reference at multiple ray jitter offsets.
2. **No double-counted hits at shared edges.** For Woop watertight only — verify that exactly one of two triangles sharing an edge is hit by a ray exactly grazing the shared edge, and the chosen one is consistent. Standard Möller-Trumbore can hit both or neither at the edge; that's why Woop is offered as the watertight alternative.
3. **SMS firefly count preserved.** Glass-on-skin BSSRDF scene rendered to convergence, firefly count within 5% of pre-BVH baseline at equal SPP. The certifier double pass is what makes this safe — manifold-walk re-intersections need the full-precision `t`.
4. **BDPT/VCM caustic energy preserved.** Caustic-heavy scene (water glass, caustic ring) — total radiance in caustic regions within 2% of pre-BVH baseline.

### 3.5 Implementation rules (rules-of-the-road for contributors)

- **Never** compare a float `t` against a double `t` for the running best. Always promote both to double, or keep the running best in float for the duration of one leaf's filter pass and certify before returning.
- **Never** use the float vertex copy for shading-quality outputs (point, normal, derivatives). Always go back to the `Triangle` struct.
- **Never** skip the certify step "as an optimization." The float filter is a candidate generator, not a verdict.
- The float copy of a triangle is a **derived cache**, not authoritative. Re-deriving it from the double `Triangle` must be possible at any time (e.g., after deserialization) without consulting the BVH file format.

### 3.6 Why not all-double or all-float?

- **All-double traversal:** doubles the BVH node memory, halves the SIMD width, costs ~1.5× the traversal time. Doesn't measurably improve correctness — the float filter is conservative.
- **All-float intersection:** loses precision in ray-marching SMS scenes, in BDPT connection lengths beyond ~100m of scene scale, and in narrow-aperture caustics. Not acceptable for this codebase.

The two-stage approach is the only option that keeps both the perf win and the precision RISE needs.

---

## 4. Public API Changes

### 4.1 Removed

The following are **deleted**, not deprecated:

- All triangle-mesh constructor signatures in [RISE_API.h](../src/Library/RISE_API.h) that take `maxpolygons`, `maxrecursion`, or `bsp` parameters. Replaced by signatures taking `AccelerationConfig`.
  - `Add3DSTriangleMeshGeometry`
  - `AddRAWTriangleMeshGeometry`
  - `AddRAW2TriangleMeshGeometry`
  - `AddPLYTriangleMeshGeometry`
  - `AddPSurfTriangleMeshGeometry`
  - `Add3DSTriangleMeshGeometryIndexed` (and indexed variants of the above)
- `TriangleMeshGeometry::nMaxPerOctantNode`, `nMaxRecursionLevel`, `bUseBSP` fields.
- `pPolygonsOctree` and `pPolygonsBSPtree` members from `TriangleMeshGeometry` and `TriangleMeshGeometryIndexed`.
- The corresponding `Octree<const Triangle*>` and `BSPTreeSAH<const Triangle*>` instantiations (the templates remain; only these specializations are removed).
- `DisplacedGeometry` constructor signature [DisplacedGeometry.h](../src/Library/Geometry/DisplacedGeometry.h): `max_polys_per_node`, `max_recursion_level`, `bUseBSP` parameters removed. The new constructor takes no acceleration-tuning args; it builds its internal mesh with a hardcoded animation-friendly `AccelerationConfig` (plain SAH, not SBVH — see §4.6 rationale). The `displaced_geometry` parser chunk drops `poly_bsp`, `maxpolygons`, `maxpolydepth` from its accepted-parameter set.

### 4.2 Added

```cpp
// src/Library/Acceleration/AccelerationConfig.h
namespace RISE {
    struct AccelerationConfig {
        unsigned int  maxLeafSize;          // max triangles per BVH leaf
        Scalar        sbvhDuplicationBudget; // [0,1], 0 = no spatial splits, ~0.30 typical
        unsigned int  binCount;              // SAH binning resolution, 16 or 32
        bool          buildSBVH;             // true = SBVH, false = plain SAH BVH
        bool          doubleSided;           // moved here from old constructor
    };
}
```

New constructors take `const AccelerationConfig&` as the acceleration parameter (single struct, all fields explicit, no defaults).

### 4.3 Parser surface

Per-mesh chunks (3DS, RAW, RAW2, PLY, PSurf, indexed variants) accept new keys, with the old keys removed:

| Old (removed) | New |
|---|---|
| `bsp = TRUE` | *(removed)* |
| `maxpolygons = N` | `bvh_max_leaf_size = N` |
| `maxrecursion = N` | *(removed — SBVH self-terminates)* |
| — | `sbvh_duplication_budget = 0.30` |
| — | `bvh_bin_count = 32` |
| — | `bvh_build_sbvh = TRUE` |

Acceleration is always BVH for triangle meshes; there's no longer a per-mesh "which structure" knob. The schema reflection ([AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp) lines ~5018, 5081, 5146 etc.) gets updated to advertise the new keys.

### 4.4 Scene file sweep

A scripted pass over `scenes/` rewrites every triangle-mesh chunk:

- Strip `bsp`, `maxrecursion` keys.
- Rename `maxpolygons` → `bvh_max_leaf_size`.
- Insert defaults for the new keys if absent.

The sweep ships in the same commit series as Phase 1 so master is never broken.

---

### 4.5 `.risemesh` File Format Migration

The `.risemesh` binary format ([src/Library/Geometry/TriangleMeshGeometryIndexed.cpp:438](../src/Library/Geometry/TriangleMeshGeometryIndexed.cpp), `cur_version = 2`) bakes a serialized octree or BSP tree alongside the polygon data. The whole reason `.risemesh` exists is to skip acceleration-structure build cost on scene load. With BVH replacing both octree and BSP, the format must evolve — and we **must** serialize the BVH (not rebuild on every load), because rebuilding a 1M-tri SBVH costs 5–60 s and would tank benchmark times.

**Version bump.** `cur_version = 3`. Behavior on read:

| Read version | Acceleration block in file | Action |
|---|---|---|
| 1 | not present (legacy) | Build BVH from polygons. Log: "Legacy v1 .risemesh, building BVH". |
| 2 | octree or BSP serialized | **Discard** the serialized structure (skip the relevant bytes), build BVH from polygons. Log: "Legacy v2 .risemesh, rebuilding BVH". |
| 3 | BVH2 serialized | Deserialize BVH2 directly. Collapse to BVH4/BVH8 at load time per `RISE_BVH_WIDTH`. |
| ≥ 4 (future) | unknown | Reject with error message naming the required RISE version. User must re-bake via meshconverter. |

**v3 layout** (in serialization order):

| Block | Size | Notes |
|---|---|---|
| Signature `"RISE"` | 4 B | unchanged from v1/v2 |
| Version | 4 B | `3` (`uint32_t`) |
| Vertex / normal / texcoord arrays | unchanged from v2 | double precision (authoritative) |
| `PointerTriangle` array | unchanged | indices into vertex/normal/texcoord arrays |
| `bDoubleSided` flag | 1 B | unchanged |
| BVH-present flag | 1 B | `0` = no BVH (rebuild on load), `1` = BVH follows |
| BVH magic `"BVH2"` | 4 B | format identifier (only BVH2 is on disk; wider widths derived) |
| BVH triangle reference count `M` | 4 B | `M ≥ N` due to SBVH duplication |
| BVH triangle reference array | 4·M B | `uint32_t` indices into `ptr_polygons` |
| BVH node count | 4 B | |
| BVH node array | sizeof(BVHNode)·numNodes | float AABBs, child indices, leaf marker |
| Overall BVH bbox (sanity) | 48 B | min/max as `Scalar` (double); validated on load — corrupt → trigger rebuild |

**Forward compatibility within the BVH initiative.** The disk format stores **BVH2 only**. Phase 3 BVH4 and Phase 4 BVH8 collapse at load time — disk format unchanged. Phase 2 precomputed triangle data (Möller edges, optional Woop transform) is also **derived at load time** from the authoritative double vertices, not stored. This means **`.risemesh` files baked with Phase 1 stay readable through Phase 4** without further version bumps.

If a future change does need a format bump (e.g., compressed nodes in deferred Phase 5), follow this convention: increment `cur_version`, add a legacy reader for the previous version that either migrates or rebuilds-from-polygons, and document the change here.

**Cross-platform determinism.** Same input mesh + same `AccelerationConfig` must produce byte-identical `.risemesh` files on macOS, Windows, and Linux. Achieved by:
- Explicit `uint32_t` / `uint64_t` field sizes (never `size_t`).
- Little-endian on disk (project convention from existing `Serialize` calls).
- Deterministic SAH binning: tie-break sort keys are `(centroid_axis_value, primitive_index)`, never centroid alone.
- Deterministic spatial-split tie-breaking: `(sah_cost, axis, primitive_index)`.
- Float-AABB rounding mode set explicitly (round-min-down, round-max-up via `nextafter`, never via FPU rounding mode which varies by OS).

Verified by a CI test that builds `dragon_small.risemesh` on macOS / Windows i9 / Linux runners and compares SHA-256 hashes. Bit-equal mismatch fails the build.

**`meshconverter` tool changes.** [src/RISE/meshconverter.cpp](../src/RISE/meshconverter.cpp) is rewritten:

| Old CLI | New CLI |
|---|---|
| `meshconverter <in> <out> <use_bsp> <max_polys> <max_levels> <invert_faces> <face_normals>` | `meshconverter <in> <out> [--max-leaf N] [--sbvh-budget F] [--bins N] [--no-sbvh] [--invert-faces] [--face-normals] [--watertight]` |

- Defaults: `--max-leaf=4`, `--sbvh-budget=0.30`, `--bins=32`, SBVH ON, watertight OFF.
- Accepts `.3ds`, `.ply`, **and `.risemesh`** as input. Passing a v1 or v2 `.risemesh` triggers in-place re-bake to v3 with BVH.
- Per the project rule on no header defaults, all flags must be passed explicitly *or* the CLI's argument-parsing layer applies the defaults at the entry point — the public API itself takes a fully-populated `AccelerationConfig`.

**Re-baking existing models.** Models in `models/risemesh/` (`bunny.risemesh`, `creature.risemesh`, `dragon_small.risemesh`, `she_ears.risemesh`, `she_eyes_pupil.risemesh`, `she_eyes_white.risemesh`, `she_head.risemesh`, `she_lips.risemesh`, `torusknot.risemesh`, plus any others) get re-baked in Phase 1's PR series. Two paths considered:

1. **Run `meshconverter` against each existing v1/v2 file** — chosen. Uses already-loaded triangle data (no need for source `.3ds` / `.ply`). Ship upgraded files in the same commit series.
2. Rely on auto-rebuild on first load — rejected. Adds 5–60 s to every cold scene-load — bad for benchmarks and bad for the user experience.

A small driver script `tools/rebake_risemesh.sh` enumerates `models/risemesh/*.risemesh`, runs the new `meshconverter` on each, and verifies the output deserializes cleanly. Lives in the same PR.

---

### 4.6 Animation Support — Keyframed `DisplacedGeometry` and BVH Refit

RISE supports keyframe animation via [IKeyframable](../src/Library/Interfaces/IKeyframable.h). Most keyframable parameters (camera transform, light intensity, material colors) don't change geometry. **One parameter type does:** a Painter2D used as the displacement function of a [DisplacedGeometry](../src/Library/Geometry/DisplacedGeometry.h). When the painter's keyframed state advances, the painter notifies via its `Observable` mixin, and DisplacedGeometry's subscription fires `DestroyMesh(); BuildMesh()` — re-tessellating the base, re-applying displacement, and rebuilding the internal mesh's acceleration structure.

This is **the only path in RISE where a triangle mesh's vertex positions change at runtime.** Static meshes from `.3ds`, `.ply`, `.risemesh`, etc., are never mutated post-load.

#### 4.6.1 Topology stability — the key observation

Reading `DisplacedGeometry::BuildMesh` ([DisplacedGeometry.cpp:98](../src/Library/Geometry/DisplacedGeometry.cpp)):

- `m_pBase->TessellateToMesh(tris, vertices, normals, coords, m_detail)` — `m_detail` is a constructor argument, **not** keyframable.
- `ApplyDisplacementMapToObject(...)` — mutates vertex positions and normals only; doesn't add or remove vertices, doesn't change triangle indices.
- The new `TriangleMeshGeometryIndexed` is built from the same triangle list each time.

Across every painter-notification rebuild for a given DisplacedGeometry instance:

| Property | Behavior across rebuilds |
|---|---|
| Triangle count | Identical |
| Triangle connectivity (vertex indices) | Identical |
| Vertex array length | Identical |
| **Vertex positions** | **Change** |
| **Vertex normals (derived)** | **Change** |
| Texture coordinates | Identical |

This is a structural fact about how DisplacedGeometry is currently implemented. The plan relies on it. If a future change makes `m_detail` keyframable, that whole assumption collapses and a different code path is needed — flagged in §4.6.6.

#### 4.6.2 Implication: refit, not rebuild

When topology is stable, a BVH can be **refit** — walk the tree bottom-up and recompute every node's AABB from the current triangle vertices — instead of rebuilt from scratch. Refit is O(N), parallelizable, with no SAH binning, no spatial-split work, no allocation. Empirical estimates for a 100K-tri displaced mesh:

| Operation | Cost | Relative |
|---|---|---|
| Octree rebuild (current animation behavior) | ~150 ms | baseline |
| SBVH rebuild | ~500 ms | 3.3× slower |
| Plain SAH BVH rebuild | ~200 ms | 1.3× slower |
| **BVH refit** | **~5–15 ms** | **10–30× faster** |

For a 30-frame animation that re-displaces every frame, refit converts BVH from "10–15 s of overhead" to "0.3 s of overhead." This is the difference between a regression vs. octree and a substantial speedup.

#### 4.6.3 Phase 1 design

1. **`BVH::Refit()`** — bottom-up walk. For each leaf, recompute AABB from the leaf's referenced triangles' current vertex positions, in `Scalar` then conservatively round to float (same padding rule as §3.3). For each internal node, take union of child AABBs in float (already conservative). Parallelizable per sub-tree at internal-node depth ~3–4 (gives 8–16 work units, matches typical core counts).

2. **`TriangleMeshGeometryIndexed::UpdateVertices(const VerticesListType&, const NormalsListType&)`** — replaces the vertex and normal arrays in place (count must match), invalidates Phase 2's precomputed triangle data (re-derived on next traversal), and triggers `BVH::Refit()`. Triangle indices are unchanged. New public method on `ITriangleMeshGeometryIndexed`. Identical method on `TriangleMeshGeometry` (non-indexed) for symmetry, though DisplacedGeometry only uses the indexed form.

3. **`DisplacedGeometry`'s observer subscription is rewritten:**
   - **First call** (initial `DisplacedGeometry` construction): full `BuildMesh()` — tessellate, displace, create `m_pMesh`, build BVH.
   - **Subsequent calls** (painter notification): re-tessellate base + re-apply displacement (same as today, cheap relative to BVH ops), then call `m_pMesh->UpdateVertices(vertices, normals)`. Mesh and BVH are kept alive across the rebuild — no `DestroyMesh()` / `new TriangleMeshGeometryIndexed`. The internal `m_displacementSubscription` lambda becomes:
     ```cpp
     [this]{
         if( m_pMesh && TopologyStableSinceConstruction() ) {
             RetessellateAndUpdateVertices();   // refit path
         } else {
             DestroyMesh();
             BuildMesh();                        // full rebuild fallback
         }
     }
     ```
     `TopologyStableSinceConstruction()` returns true unless something we don't currently support has changed (placeholder for the future-proofing in §4.6.6).

4. **SAH degradation safeguard.** Refit preserves topology, but if displacement amplitude varies heavily across frames, AABB overlap can grow and traversal cost can degrade. The BVH stores `originalSAHCost` at build time and recomputes `currentSAHCost` after each refit. If `currentSAHCost > 2.0 × originalSAHCost`, the next observer-notification triggers a full SBVH rebuild instead of refit. The threshold is a `static constexpr Scalar` in `BVH.h`, not exposed in `AccelerationConfig` — it's an implementation detail of the heuristic, not a user-facing tuning knob.

5. **`DisplacedGeometry` uses plain SAH BVH, not SBVH.** Rationale: even with refit doing most of the work between frames, the *first* build and any *triggered rebuild* should be fast. SBVH is a 2–3× slower builder, and its spatial-split duplication doesn't carry forward to refit (refit just updates AABBs; duplicated leaves cost the same as non-duplicated). So SBVH's investment is wasted in the animation case. Internal `AccelerationConfig` for DisplacedGeometry is hardcoded:
   ```cpp
   AccelerationConfig animConfig{
       .maxLeafSize             = 4,
       .sbvhDuplicationBudget   = 0.0,    // disabled
       .binCount                = 16,     // smaller than static (32) — faster build
       .buildSBVH               = false,  // plain SAH
       .doubleSided             = m_bDoubleSided,
   };
   ```
   Static meshes still get full SBVH per the user's `AccelerationConfig` from the parser.

6. **Refit thread-safety.** The refit walk runs on the scene-load / keyframe-evaluation thread (between frames), not concurrently with intersection. The render-pass entry [PixelBasedRasterizerHelper.cpp](../src/Library/Rendering/PixelBasedRasterizerHelper.cpp) already has a per-frame barrier where keyframe evaluation happens; refit fits in the same window. Phase 1 documents this contract in `BVH.h`.

#### 4.6.4 Memory

Refit needs each leaf to know which triangle vertex indices to read for its AABB recompute. For BVH2 stored as a triangle-pointer array, this is already implicit (the leaf's range of triangle indices). One additional cached field per node — `originalSAHCost` contribution — adds 4 B/node × ~3N nodes = ~12 B/triangle on a 1M-tri mesh. Negligible.

#### 4.6.5 Cross-phase invariance

Refit is part of Phase 1. It must continue to work through Phases 2/3/4:

- **Phase 2** adds precomputed triangle data (Möller edges, Woop transform). These are derived from vertex positions, so they're invalidated by `UpdateVertices` and re-derived on next traversal (or eagerly recomputed at refit time — TBD by Phase 2 perf measurement).
- **Phase 3** collapses BVH2 → BVH4 at load time, but the original BVH2 leaf-to-triangle mapping survives. Refit walks the BVH2 representation and re-collapses the affected sub-trees, OR refits the BVH4 representation directly (cheaper). The Phase 3 design doc will pick.
- **Phase 4** same as Phase 3 but for BVH8.

The animation regression test (§4.6.7 below, in §5.7 Tests) carries forward as a gate in every phase.

#### 4.6.6 Out of scope for this initiative

- **Keyframable `m_detail` on DisplacedGeometry.** Would change topology per frame. Currently not supported, won't be added. If demand arises later, that's its own design — likely "rebuild from scratch every frame" with the animation `AccelerationConfig`, no refit.
- **Keyframable base geometry on DisplacedGeometry.** Same as above.
- **Future animatable static meshes.** If RISE ever supports loading a mesh from a per-frame `.risemesh` (e.g., Alembic-style animated mesh sequences), that's also a topology-may-change case — out of scope.
- **GPU-side refit.** RISE is CPU-only; no concern.

---

## 5. Phase 1 — BVH2 + SBVH Builder

**Goal.** Land a working SBVH-built BVH2 acceleration structure for both `TriangleMeshGeometry` and `TriangleMeshGeometryIndexed`, replacing octree and BSP. End the phase with all regression scenes rendering bit-equal first-hit to the pre-BVH baseline.

### 5.1 New files

All added to **all five** build projects per [CLAUDE.md](../CLAUDE.md):
- `build/make/rise/Filelist`
- `build/cmake/rise-android/rise_sources.cmake`
- `build/VS2022/Library/Library.vcxproj`
- `build/VS2022/Library/Library.vcxproj.filters`
- `build/XCode/rise/rise.xcodeproj/project.pbxproj`

| Path | Purpose |
|---|---|
| `src/Library/Acceleration/AccelerationConfig.h` | Config struct (§4.2) |
| `src/Library/Acceleration/BVH.h` | Templated 2-wide BVH: nodes, traversal, serialization |
| `src/Library/Acceleration/BVHNode.h` | Node layout (float AABBs, child indices, leaf marker) |
| `src/Library/Acceleration/BVHBuilder.h` | Templated SAH binned builder |
| `src/Library/Acceleration/BVHBuilder.cpp` | Builder implementation (non-templated helpers) |
| `src/Library/Acceleration/SBVHBuilder.h` | Spatial-split layer over BVHBuilder |
| `src/Library/Acceleration/SBVHBuilder.cpp` | Spatial-split implementation, duplication budget |
| `src/Library/Acceleration/BVHTraversal.h` | Single-ray traversal for BVH2 (float bbox, double certify) |
| `tests/BVHBuilderTest.cpp` | Build correctness, hit equivalence vs. naive |
| `tests/SBVHBuilderTest.cpp` | Spatial split engagement, SAH cost reduction |
| `tests/BVHRegressionTest.cpp` | End-to-end mesh hit equivalence vs. octree |
| `src/Library/Acceleration/BVHSerialization.h` | Serialize/deserialize routines for BVH; v3 `.risemesh` reader/writer; legacy v1/v2 detection |
| `src/Library/Acceleration/BVHSerialization.cpp` | Serialization implementation (cross-platform endian, deterministic field ordering) |
| `tests/BVHSerializationTest.cpp` | Build → write → read → render roundtrip; cross-platform byte-identical disk output |
| `tests/RISEMeshLegacyLoadTest.cpp` | Load every existing v1/v2 `.risemesh` from `models/risemesh/`; verify auto-rebuild produces hit-equivalent results to a fresh-built BVH |
| `tools/rebake_risemesh.sh` | Driver script to re-bake all `models/risemesh/*.risemesh` to v3 |
| `src/Library/Acceleration/BVHRefit.h` | `BVH::Refit()` walk: bottom-up AABB recompute, conservative float padding, parallel at internal-node depth ~3–4 |
| `tests/BVHRefitTest.cpp` | Refit correctness: build → mutate vertices → refit → render → compare to fresh-built BVH on same mutated geometry |
| `tests/AnimatedDisplacedGeometryTest.cpp` | 30-frame animation, sphere displaced by keyframed noise painter; per-frame pixel-equivalent to pre-BVH octree path. **Carry-forward regression for Phases 2/3/4.** |

### 5.2 Files modified

| Path | Change |
|---|---|
| [src/Library/Geometry/TriangleMeshGeometry.h](../src/Library/Geometry/TriangleMeshGeometry.h) | Drop `pPolygonsOctree`, `pPolygonsBSPtree`. Add `BVH<const Triangle*>* pBVH` and `AccelerationConfig accelConfig`. Constructor takes `AccelerationConfig`. |
| [src/Library/Geometry/TriangleMeshGeometry.cpp](../src/Library/Geometry/TriangleMeshGeometry.cpp) | `IntersectRay` and `IntersectRay_IntersectionOnly` go through BVH. `DoneTriangles` builds BVH instead of octree/BSP. Serialization updated. |
| [src/Library/Geometry/TriangleMeshGeometrySpecializations.h](../src/Library/Geometry/TriangleMeshGeometrySpecializations.h) | Add SBVH-needed callbacks: triangle-vs-plane clipping (for spatial splits), centroid for SAH binning |
| [src/Library/Geometry/TriangleMeshGeometryIndexed.h](../src/Library/Geometry/TriangleMeshGeometryIndexed.h) and `.cpp` | Mirror changes |
| [src/Library/Geometry/TriangleMeshGeometryIndexedSpecializations.h](../src/Library/Geometry/TriangleMeshGeometryIndexedSpecializations.h) | Mirror |
| [src/Library/RISE_API.h](../src/Library/RISE_API.h) | Replace mesh constructor signatures (§4.1, §4.2) |
| [src/Library/Job.cpp](../src/Library/Job.cpp) | Update `Add*TriangleMesh*` implementations |
| [src/Library/Parsers/AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp) | Replace mesh chunks (~lines 5018, 5081, 5146 for 3DS/RAW/RAW2; corresponding indexed and PLY/PSurf). Update schema reflection. Update reaction-diffusion / displaced / bezier sites that reference removed keys, only where they propagate |
| `scenes/**/*.RISEscene` | Sweep for `bsp =`, `maxpolygons =`, `maxrecursion =` in mesh chunks; rewrite per §4.4 |
| [src/RISE/meshconverter.cpp](../src/RISE/meshconverter.cpp) | Rewrite per §4.5: new CLI, accepts `.3ds`/`.ply`/`.risemesh` as input, uses `AccelerationConfig` |
| [models/risemesh/*.risemesh](../models/risemesh/) | Re-bake from v1/v2 → v3 via `tools/rebake_risemesh.sh` (bunny, creature, dragon_small, she_*, torusknot, plus any others) |
| [src/Library/Geometry/DisplacedGeometry.h](../src/Library/Geometry/DisplacedGeometry.h) and `.cpp` | Constructor signature change (drop `max_polys_per_node` / `max_recursion_level` / `bUseBSP`); rewrite observer lambda to use refit path with rebuild fallback; introduce `TopologyStableSinceConstruction()` predicate; hardcode internal animation `AccelerationConfig` (plain SAH, not SBVH) per §4.6 |
| [src/Library/Interfaces/ITriangleMeshGeometryIndexed.h](../src/Library/Interfaces/ITriangleMeshGeometryIndexed.h) | Add `virtual void UpdateVertices(const VerticesListType&, const NormalsListType&) = 0;` per §4.6.3. Pure-virtual addition is an ABI break — acceptable per [no-back-compat decision D4]; check [skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md) for reasoning still applicable to in-tree subclasses |
| [src/Library/Interfaces/ITriangleMeshGeometry.h](../src/Library/Interfaces/ITriangleMeshGeometry.h) | Same `UpdateVertices` addition for non-indexed (symmetry; not used by DisplacedGeometry today, but kept consistent) |
| [src/Library/Job.cpp](../src/Library/Job.cpp) | Update `AddDisplacedGeometry` to construct with new signature |
| [src/Library/RISE_API.h](../src/Library/RISE_API.h) | `RISE_API_AddDisplacedGeometry` signature change |
| [src/Library/Parsers/AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp) (`displaced_geometry` chunk, lines ~3441+) | Drop `poly_bsp`, `maxpolygons`, `maxpolydepth` from accepted-parameter set; update schema reflection |

### 5.3 SBVH builder details

Implementation follows Stich/Friedrich/Dietrich (2009).

- **SAH binned object split.** For each axis, project triangle centroids into 32 bins, evaluate SAH cost for each bin boundary, pick best.
- **Spatial split.** For the same 32 bins, count triangles whose AABB straddles each bin boundary. For each potential split, compute SAH cost assuming straddlers are duplicated. If the spatial-split cost beats the object-split cost AND duplication budget is not exceeded, take the spatial split.
- **Duplication budget.** Tracked globally as `total_refs_after_split / total_refs_before_any_split - 1`. Default 0.30 (= 30% additional references allowed). Exceeded → fall back to object split.
- **Termination.** Recursion ends when a leaf has ≤ `maxLeafSize` triangles OR when SAH cost says splitting is worse than not splitting.
- **Triangle clipping for spatial splits.** Sutherland-Hodgman polygon clipping against the split plane in `Scalar`, then conservative AABB conversion to float.

### 5.4 BVH2 traversal details

- Stack-based, fixed-capacity stack (depth 64 covers any realistic tree).
- Float ray + float inv-ray cached on entry.
- At each internal node: ray-vs-2-AABB float test, push hit children in near-far order based on ray sign on the split axis (cached at build time per node).
- At each leaf: iterate triangles, run float Möller-Trumbore filter, then double certify on hits.
- Single-ray only. No packets, no streams.

### 5.5 `.risemesh` Serialization Pipeline

This sub-section ties the format spec from §4.5 into Phase 1 implementation work, since both the writer (used by `meshconverter`) and the reader (used by every scene load that references a `.risemesh`) live in this phase.

**Reader flow** (`TriangleMeshGeometryIndexed::Deserialize` and `TriangleMeshGeometry::Deserialize`):

1. Verify signature `"RISE"`. Mismatch → fail.
2. Read `version`. Dispatch:
   - **v1**: read polygon data. No acceleration block. Build BVH from polygons via Phase 1's builder. Log info-level message.
   - **v2**: read polygon data. Read `bUseBSP` flag. **Skip** the serialized octree/BSP bytes (use a counted-skip helper that knows the v2 layout, OR — simpler — fully deserialize into the legacy structures, then immediately drop them and build BVH from the polygons we just loaded). Log info-level message.
   - **v3**: read polygon data. Read BVH-present flag. If `1`, read magic `"BVH2"`, deserialize BVH directly. Validate sanity bbox (finite, extents `< 1e10`); on failure, drop the deserialized BVH and rebuild from polygons. If BVH-present is `0`, build from polygons.
   - **v ≥ 4**: error out, name the required RISE version.
3. After load: regardless of source version, verify the BVH has `numNodes ≥ 1` and at least one leaf references each polygon at least once.

**Writer flow** (`TriangleMeshGeometryIndexed::Serialize` and `TriangleMeshGeometry::Serialize`):

1. Write signature, version `3`, polygon arrays, double-sided flag (unchanged from v2 layout).
2. Write BVH-present flag = `1`.
3. Write `"BVH2"` magic.
4. Write triangle reference count, then references.
5. Write node count, then nodes (deterministic field order, explicit `uint32_t`, little-endian).
6. Write overall sanity bbox in `Scalar` (double).

**Why "skip and rebuild" for v2 instead of "migrate".** v2's serialized octree/BSP encodes a *different* spatial partition than what SBVH would produce. There's no information-preserving migration — we'd just be guessing at SBVH parameters from octree leaf sizes. Rebuilding from polygons gets the right answer, costs 5–60 s on first load, and is a one-time cost (the file gets re-baked to v3 by `tools/rebake_risemesh.sh` and never paid again).

**Backwards compatibility flag for the legacy reader.** v1 and v2 reading code stays in the codebase (one path each, well-isolated in `BVHSerialization.cpp`). Removed in a future version after we're confident every shipped `.risemesh` is v3.

### 5.6 Animation: Refit & DisplacedGeometry Integration

This sub-section ties the design from §4.6 into Phase 1 implementation work. Animation flow is the only runtime mesh-mutation path in RISE; refit is what keeps it from regressing under BVH.

**Sequence of operations in the observer-notify path:**

1. Painter (animator-driven) emits a notify event.
2. DisplacedGeometry's subscription lambda fires.
3. Lambda calls `TopologyStableSinceConstruction()` — true except for never-supported cases (see §4.6.6).
4. **Stable path** (the always-taken path today):
   1. Re-tessellate base geometry into local `vertices`, `normals`, `coords`, `tris` buffers.
   2. Re-apply `ApplyDisplacementMapToObject` (mutates `vertices` and `normals`).
   3. If `!m_bUseFaceNormals`, recompute vertex normals from topology.
   4. Call `m_pMesh->UpdateVertices(vertices, normals)`. Internally:
      - Replace `pPoints` and `pNormals` arrays (count assertions: must match prior call).
      - Invalidate Phase 2's precomputed triangle cache (cleared lazily on next traversal, or eagerly here — Phase 2 decides).
      - Compute `currentSAHCost` of the (still-topologically-correct) BVH against the new vertex positions.
      - If `currentSAHCost > 2.0 × originalSAHCost`: full SBVH rebuild via animation `AccelerationConfig`, then update `originalSAHCost`.
      - Otherwise: `BVH::Refit()` — bottom-up AABB recompute.
5. **Fallback path** (full rebuild, equivalent to today's behavior): `DestroyMesh(); BuildMesh();` with the animation `AccelerationConfig`. Reachable only if `TopologyStableSinceConstruction()` returns false — currently impossible, kept for future-proofing.

**Concurrency contract.** `UpdateVertices` and `Refit` MUST be called between frames, never during ray traversal. The render-pass entry already has a per-frame barrier where keyframes evaluate ([PixelBasedRasterizerHelper.cpp](../src/Library/Rendering/PixelBasedRasterizerHelper.cpp)); refit happens in that window. Documented in `BVH.h` and `ITriangleMeshGeometryIndexed.h`. Violation is a programming error — not currently checked at runtime, optionally guarded by a debug-mode `assert(!m_inTraversal)` flag.

**Memory continuity.** `m_pMesh` is **not** destroyed across observer notifications in the stable path. Same `TriangleMeshGeometryIndexed*` lifetime. Same `BVH*` lifetime. Same `pPoints` storage backing (re-filled, not reallocated, when count matches). Reference counts on the `m_pMesh` from any IObjectPriv that holds it are preserved.

**SAH-cost tracking.** Stored on the BVH:
```cpp
class BVH {
    Scalar originalSAHCost;       // set at build time
    Scalar currentSAHCost;        // recomputed after each Refit
    static constexpr Scalar kRefitToRebuildThreshold = 2.0;
    bool ShouldRebuild() const { return currentSAHCost > kRefitToRebuildThreshold * originalSAHCost; }
};
```

`originalSAHCost` is computed during the initial SAH walk (free — already part of the build). `currentSAHCost` is computed during refit by accumulating per-node SAH contributions in the same walk that updates AABBs.

**Future-proofing for Phases 2–4.**
- Phase 2 precomputed triangle data: invalidate on `UpdateVertices`, re-derive lazily.
- Phase 3 BVH4 collapse: refit operates on the BVH2 storage; collapse re-runs after refit if the tree is held in BVH4 form. Or: refit operates directly on BVH4 nodes (one design choice, deferred to Phase 3).
- Phase 4 BVH8: same.

The carry-forward regression test `AnimatedDisplacedGeometryTest` re-runs as a gate in every phase.

### 5.7 Tests

| Test | Mechanism | Pass criterion |
|---|---|---|
| `BVHBuilderTest` | Random triangle sets, build BVH, intersect 1000 random rays, compare hit `(triIndex, t)` to brute-force | Set equality up to 1 ULP `t` |
| `SBVHBuilderTest` | Pathological non-uniform mesh (long thin triangles), verify SAH cost lower than non-SBVH BVH | SAH cost reduction ≥ 15% |
| `BVHRegressionTest` | Build BVH and Octree on the same mesh, intersect identical rays, compare hits | Bit-equal first hit |
| `BVHSerializationTest::Roundtrip` | Build mesh → serialize to memory → deserialize → render 1000 rays | Hit `(triIndex, t)` set-equal between in-memory original and roundtripped |
| `BVHSerializationTest::CrossPlatform` | Build `dragon_small` BVH on macOS, Windows, Linux runners; SHA-256 the resulting `.risemesh` bytes | All three hashes equal |
| `BVHSerializationTest::FormatStability` | Bake a v3 file with Phase 1 builder, load with subsequent Phase 2/3/4 readers | Hit-equivalent across phases (this test rolls forward; reused as a regression in Phases 2–4) |
| `RISEMeshLegacyLoadTest::V1` | Load each v1 `.risemesh` (if any survive in the repo) → expect auto-rebuild log → render | Hit-equivalent to fresh-built BVH from the same polygon data |
| `RISEMeshLegacyLoadTest::V2` | Load each v2 `.risemesh` from `models/risemesh/` *before* the rebake commit → expect auto-rebuild log → render | Hit-equivalent. **Important: this test must run against pre-rebake v2 files, so we keep one or two v2 fixtures in `tests/fixtures/risemesh_legacy/` permanently.** |
| `RISEMeshLegacyLoadTest::V3` | Load each post-rebake v3 file → render | Hit-equivalent and **does not log "rebuilding"** (i.e., the BVH came from disk, not a build) |
| `RISEMeshLegacyLoadTest::Corrupted` | Truncate, scramble bbox, flip BVH-present byte | Falls through to rebuild path; renders correctly |
| `MeshConverterTest::CLI` | Run `meshconverter` against a `.3ds`, `.ply`, v2 `.risemesh`, v3 `.risemesh`; check output is v3 and renders correctly | All four input types produce valid v3 |
| Render regressions | 5 scenes: implicit-only, low-tri (1K), medium-tri (50K), BioSpec face (200K), glass + SMS | Pixel-equal to octree baseline |
| `RayTriangleIntersectionTest` (existing) | Add silhouette / edge-grazing / degenerate cases | All pass under both Möller and Woop |
| `BVHRefitTest::Equivalence` | Build BVH on mesh M0 → mutate vertices to M1 → `Refit()` → intersect rays. Compare to fresh-built BVH on M1 directly. | Hit `(triIndex, t)` set-equal between refit BVH and fresh BVH |
| `BVHRefitTest::SAHTracking` | Build BVH, mutate vertices with increasing displacement amplitude over many iterations, `Refit()` each time | `currentSAHCost / originalSAHCost` grows monotonically with amplitude; rebuild threshold (2.0×) trips when expected |
| `BVHRefitTest::Determinism` | Same M0 → mutate to M1 → refit; do this twice from independent starting BVHs | Both refit results bit-equal |
| `BVHRefitTest::Performance` | 100K-tri displaced sphere, time refit vs. fresh build | Refit ≥ 10× faster than fresh build (sanity guard, not a correctness gate) |
| `AnimatedDisplacedGeometryTest::PerFrameEquivalence` | 30-frame animation of a sphere displaced by a noise painter with keyframed `time`. Render each frame on the BVH path and on the pre-BVH octree path. | Per-frame pixel-equivalent (deterministic scenes) or statistically equivalent (path-traced). **Carry-forward regression for Phases 2/3/4.** |
| `AnimatedDisplacedGeometryTest::TopologyInvariant` | Across the 30 frames, query `m_pMesh` triangle count, vertex count, index buffer | All identical across all frames; confirms refit-eligibility holds |
| `AnimatedDisplacedGeometryTest::ObserverIntegration` | Watch the GlobalLog during the animation; verify the "rebuilt from scratch" log line appears exactly once (initial build), and "refit" appears 29 times | Exact count match — guards against accidental fall-through to rebuild path |

The serialization and legacy-load tests are designated as **regression carry-forward tests** — they re-run as gates in Phases 2, 3, and 4. Any phase that breaks v3 read or breaks legacy v1/v2 auto-rebuild fails its gate.

### 5.8 Validation gates (must pass to close Phase 1)

- [ ] All new tests green.
- [ ] All existing tests green (`./run_all_tests.sh`).
- [ ] 5 regression render scenes produce pixel-equal output to octree baseline.
- [ ] BioSpec face render ≥ 2× faster wall-clock than octree baseline.
- [ ] No new fireflies in glass + SMS scene; firefly count within 5% of baseline.
- [ ] All five build projects compile clean on macOS, Linux, Windows, Android, Xcode.
- [ ] Every `.risemesh` file in `models/risemesh/` (v2) loads via the legacy auto-rebuild path and renders hit-equivalent to a fresh-built BVH.
- [ ] `tools/rebake_risemesh.sh` runs to completion against `models/risemesh/`; every output file is v3, deserializes cleanly, and renders identically to its pre-rebake counterpart in scenes that reference it.
- [ ] At least one v2 `.risemesh` is preserved in `tests/fixtures/risemesh_legacy/` and the `RISEMeshLegacyLoadTest::V2` regression continues to pass — guards that legacy load survives Phases 2–4.
- [ ] Cross-platform determinism: `dragon_small.risemesh` baked on macOS / Windows i9 / Linux produces SHA-256-identical bytes.
- [ ] `meshconverter` accepts `.3ds`, `.ply`, v2 `.risemesh`, and v3 `.risemesh` as input; all four produce valid v3 output (`MeshConverterTest::CLI` passes).
- [ ] No scene file or test file in the repo references the old meshconverter CLI flags (`use_bsp` numeric, etc.) — the old CLI is fully removed.
- [ ] Animated displaced-sphere scene (30 frames, keyframed noise painter) renders pixel-equivalent to the pre-BVH octree path on every frame.
- [ ] BVH refit is invoked exactly 29 times in the 30-frame animation; full rebuild fires only at frame 0 (or on threshold trip, which the test scene doesn't trigger).
- [ ] Refit on a 100K-tri displaced sphere is ≥ 10× faster than fresh BVH build.
- [ ] SAH-degradation safeguard (rebuild trigger at `currentSAH > 2.0 × originalSAH`) fires correctly on a synthetic high-amplitude-noise scene.
- [ ] `DisplacedGeometry::m_pMesh` lifetime is preserved across observer notifications in the stable-topology path (no `safe_release` / re-`new` in the inner loop).

### 5.9 Phase 1 risks

| Risk | Mitigation |
|---|---|
| Float bbox precision causes silhouette holes | Conservative 2-ULP padding (§3.3); silhouette test in regressions |
| SBVH duplication blows up memory on huge meshes | Hard duplication budget; fallback to plain SAH if exceeded |
| Polygon clipping for spatial splits introduces numerical noise | Clip in `Scalar`, then convert; unit-test clip on degenerate cases |
| Build time regression on huge meshes | Bench against octree build time; SBVH is ~2× SAH, ~3–5× octree on huge meshes — acceptable for offline |
| Serialization format changes | New BVH file format; bump scene-file version if BVH is serialized; otherwise rebuild on load |

---

## 6. Phase 2 — Triangle Intersection Beef-Up

**Goal.** Replace per-call edge computation with precomputed per-triangle data stored in BVH leaves. Add optional Woop watertight path under compile-time flag. Validate caustic correctness.

### 6.1 New files

| Path | Purpose |
|---|---|
| `src/Library/Acceleration/BVHTriangleData.h` | Precomputed triangle layouts: standard (P0/e1/e2 float) and Woop (4×3 affine float) |
| `src/Library/Acceleration/BVHTriangleIntersect.h` | Float-filter and double-certify intersection routines, header-inlined |
| `tests/BVHTriangleIntersectTest.cpp` | Standard + Woop correctness, watertight invariants |

### 6.2 Files modified

| Path | Change |
|---|---|
| `BVH.h` / `BVH.cpp` | Leaves carry `BVHTriangleData[]` parallel to triangle pointer array |
| `BVHTraversal.h` | Leaf intersection switches on `RISE_BVH_WATERTIGHT` to pick filter |
| `RayTriangleIntersection.cpp` | Stays as the double-precision certifier (called from BVH leaf after filter) |
| `Profiling.h` | Add counters: `nFloatFilterTests`, `nDoubleCertifyTests`, `nFilterRejects` |

### 6.3 Standard precomputed triangle (default)

```cpp
struct BVHTriangleStd {
    float p0[3];    // P0
    float e1[3];    // P1 - P0
    float e2[3];    // P2 - P0
};
```

36 bytes per triangle. Float Möller-Trumbore uses these directly. Double certifier reads from `Triangle` struct.

### 6.4 Woop watertight (opt-in via `RISE_BVH_WATERTIGHT`)

```cpp
struct BVHTriangleWoop {
    float m[12];    // 4x3 affine transform to ray space
};
```

48 bytes per triangle. Implements Woop/Benthin/Wald 2013. Watertight at shared edges; consistent edge ownership. Costs ~5% over standard on typical scenes; pays for itself on caustic-heavy scenes (BDPT, VCM, SMS) by eliminating edge-leak fireflies.

### 6.5 Tests

| Test | Pass criterion |
|---|---|
| `BVHTriangleIntersectTest::Std` | Hit set equality with naive Möller |
| `BVHTriangleIntersectTest::Woop` | Hit set equality with naive Woop, edge-grazing rays produce exactly one hit |
| Caustic regression (glass on table) | BDPT: total caustic radiance within 2% of baseline; VCM: same |
| SMS regression (chocolate fountain or equivalent) | Manifold walk converges at same rate; firefly count within 5% |

### 6.6 Validation gates

- [ ] Float-filter tests + double certifier pass full BVHRegressionTest from Phase 1.
- [ ] Standard intersection ≥ 1.2× faster than baseline Möller (on top of Phase 1 speedup).
- [ ] Woop intersection within 5% of standard wall-clock; passes watertight test.
- [ ] Caustic and SMS regression scenes within 2% / 5% radiance tolerance.

### 6.7 Phase 2 risks

| Risk | Mitigation |
|---|---|
| Float filter false-rejects valid hits | Pad filter epsilon by `~1e-5` relative; double certifier catches near-edge hits the float filter rejects (re-test in double) |
| Woop transform construction numerically unstable on near-degenerate triangles | Detect degenerate triangles at build, store as null leaf entries (skip filter, go straight to double Möller as fallback) |
| Memory blowup on dense meshes | 36 B/tri (or 48 B Woop) on a 1M-tri mesh is 36–48 MB — large but acceptable |

---

## 7. Phase 3 — BVH4 + AVX2 / NEON SIMD

**Goal.** Wide BVH (4-wide) with SIMD ray-vs-4-AABB traversal. Build via collapse pass over BVH2. Apple Silicon NEON path is first-class.

### 7.1 New files

| Path | Purpose |
|---|---|
| `src/Library/Utilities/SIMD.h` | Internal SIMD abstraction: `Vec4f`, `Vec4i`, ops we need |
| `src/Library/Utilities/SIMD_x86.h` | SSE2 / AVX2 intrinsic implementations |
| `src/Library/Utilities/SIMD_NEON.h` | ARM NEON implementation |
| `src/Library/Utilities/SIMD_Scalar.h` | Scalar fallback for non-SIMD platforms |
| `src/Library/Acceleration/BVH4.h` | 4-wide node layout (SoA child AABBs), traversal |
| `src/Library/Acceleration/BVH4Collapse.h/.cpp` | BVH2 → BVH4 collapse pass |
| `src/Library/Acceleration/RayBox4.h` | Single-ray vs. 4-AABB SIMD test |
| `tests/SIMDTest.cpp` | SIMD wrapper correctness across platforms |
| `tests/BVH4Test.cpp` | BVH4 hit equivalence with BVH2 |

### 7.2 Files modified

| Path | Change |
|---|---|
| `BVHTraversal.h` | Switches on `RISE_BVH_WIDTH` between BVH2 and BVH4 traversal |
| `BVH.h` | Build pipeline: SBVH → BVH2 → optional BVH4 collapse |
| Build files (×5) | Conditional inclusion of SIMD_x86 vs SIMD_NEON based on platform |

### 7.3 BVH4 node layout

```cpp
struct BVH4Node {
    float bbMinX[4], bbMinY[4], bbMinZ[4];   // SoA min
    float bbMaxX[4], bbMaxY[4], bbMaxZ[4];   // SoA max
    int32_t children[4];                      // child indices, leaf encoded with sign bit
    uint8_t numChildren;                      // 2..4
    uint8_t splitAxis;                        // dominant split axis (for ordering)
    uint8_t pad[2];
};
```

128 bytes per node. SoA layout enables one SSE/AVX2/NEON op to test ray vs. 4 child AABBs.

### 7.4 Collapse pass

Standard Ernst-Greiner collapse over BVH2:

- Walk BVH2 top-down.
- At each node, if both children are internal, "promote" their children to make 4 children. Repeat until 4 children or a leaf is encountered.
- Set `numChildren` accordingly (2, 3, or 4).
- Leaves stay leaves; internal nodes get up to 4 children.

The collapse is a build-time post-process; the SBVH builder is unchanged.

### 7.5 SIMD abstraction scope

The wrapper exposes only what we need for BVH traversal:

```cpp
struct Vec4f {
    Vec4f(float a, float b, float c, float d);
    Vec4f(float v);  // broadcast
    static Vec4f load(const float*);
    void store(float*);
    Vec4f operator+(Vec4f), operator-(Vec4f), operator*(Vec4f);
    Vec4f min(Vec4f), max(Vec4f);
    int movemask() const;  // 4-bit mask of sign bits
    Vec4f cmp_le(Vec4f) const;  // mask
};
```

x86: `__m128`. NEON: `float32x4_t`. Scalar: `float[4]`. Same interface, three implementations selected by `#if`.

**See [§10.7](#107-target-hardware-matrix) before writing the wrappers** — it documents the per-platform build flags (MSVC `/arch:AVX2` quirks, NDK `armv8.2-a` minimum, Apple Clang `-mcpu` selection), MSVC macro detection gotchas, subnormal flush-to-zero requirements, and which compile-time macros to gate on.

### 7.6 Ordered traversal for BVH4

Permutation table indexed by `(ray_sign_x, ray_sign_y, ray_sign_z, split_axis)`:

```cpp
constexpr uint8_t bvh4_order[2][2][2][3][4] = { /* precomputed at build time */ };
```

8 sign permutations × 3 axes × 4 children = 96 bytes. At each node, one table lookup yields the near-to-far child order.

### 7.7 Tests

| Test | Pass criterion |
|---|---|
| `SIMDTest` | All ops produce same results across x86, NEON, scalar |
| `BVH4Test` | Hit set equality with BVH2 |
| Render regressions | Pixel-equal to BVH2 on deterministic scenes; statistically equal on stochastic |
| Performance | BVH4 ≥ 1.5× BVH2 on benchmark mesh (50K+ tri face) |

### 7.8 Validation gates

- [ ] x86 (AVX2), Apple Silicon (NEON), Linux/Windows AVX2 all green.
- [ ] BVH4 ≥ 1.5× BVH2 on the BioSpec benchmark scene.
- [ ] All Phase 1 + Phase 2 regression criteria still pass.
- [ ] No new SMS / caustic regressions.

### 7.9 Phase 3 risks

| Risk | Mitigation |
|---|---|
| Wider traversal changes hit ordering, breaks deterministic tie-breaks for SMS | Tie-break in BVH4 by (`tEntry`, `childIndex`); document and unit-test |
| NEON path has subtle differences from AVX2 (denormals, NaN handling) | SIMD wrapper specifies behavior; cross-platform `SIMDTest` covers edge cases |
| Compile-time width selection misfires on cross-compile | Explicit `RISE_BVH_WIDTH` override; CI builds all widths |
| SoA conversion costs traversal time for 2- or 3-child nodes | Profile; if real, special-case 2-child nodes back to BVH2 traversal kernel |

---

## 8. Phase 4 — BVH8

**Goal.** 8-wide BVH on AVX-512 (native) and AVX2 (via 2×4). NEON stays at BVH4 per design decision D3.

**Audience reality check.** Per §10.7, **none of the project's three primary dev machines support AVX-512**: Apple Silicon and Galaxy Fold are NEON-only; modern consumer Intel i9s (Alder Lake onward) had AVX-512 disabled at the silicon level. BVH8 is therefore an **opt-in path for workstation/server hardware** — i9-11900 Rocket Lake (11th gen, the last consumer chip with AVX-512), Xeon W, Xeon Sapphire Rapids, AMD EPYC Genoa, Threadripper. The AVX2 2×4 fallback exists for cross-platform binary uniformity, not as a perf target. Phase 4 ships gated on this expectation: BVH4 stays the production default; BVH8 is validated on a designated AVX-512 test box and benchmarked there.

### 8.1 New files

| Path | Purpose |
|---|---|
| `src/Library/Utilities/SIMD8.h` | `Vec8f` abstraction |
| `src/Library/Utilities/SIMD8_x86.h` | AVX-512 native + AVX2 (2×`__m128`) implementations |
| `src/Library/Acceleration/BVH8.h` | 8-wide node layout, traversal |
| `src/Library/Acceleration/BVH8Collapse.h/.cpp` | BVH2 → BVH8 collapse |
| `src/Library/Acceleration/RayBox8.h` | Ray-vs-8-AABB SIMD test |
| `tests/BVH8Test.cpp` | Hit equivalence |

### 8.2 Files modified

| Path | Change |
|---|---|
| `BVHTraversal.h` | Adds BVH8 path under `RISE_BVH_WIDTH == 8` |
| Platform build files | Conditional AVX-512 detection / fallback to AVX2-2×4 |

### 8.3 BVH8 node layout

```cpp
struct BVH8Node {
    float bbMinX[8], bbMinY[8], bbMinZ[8];   // SoA
    float bbMaxX[8], bbMaxY[8], bbMaxZ[8];
    int32_t children[8];
    uint8_t numChildren;                      // 2..8
    uint8_t splitAxis;
    uint8_t pad[2];
};
```

256 bytes per node. AVX-512 tests 8 children in one `_mm512_cmp_ps`. AVX2 uses two `_mm256_cmp_ps` (or two `__m128` pairs, depending on what benchmarks better).

### 8.4 Ordered traversal for BVH8

Same permutation-table approach as BVH4 but indexed for 8 children. Larger table (~768 bytes) but still cache-resident.

### 8.5 Tests / gates

| Item | Pass criterion |
|---|---|
| `BVH8Test` | Hit set equality with BVH4 / BVH2 |
| Performance (AVX-512) | BVH8 ≥ 1.3× BVH4 on benchmark scene |
| Performance (AVX2 fallback) | BVH8 within 10% of BVH4 (no regression) |
| All prior regressions | Still pass |

### 8.6 Phase 4 risks

| Risk | Mitigation |
|---|---|
| AVX-512 thermal throttling on consumer CPUs | Document; include AVX-512-off comparison in benchmarks |
| AVX2-via-2×4 fallback not faster than BVH4 | Acceptable; BVH4 stays the AVX2 default. BVH8 is opt-in for AVX-512 |
| AVX-512 detection fragile across compilers | CI matrix; explicit override |

---

## 9. Phase 4 Evaluation Gate

After Phase 4 ships and passes its gates, write a results report:

| Metric | Target | Result |
|---|---|---|
| BioSpec face, BVH8 vs. octree baseline | ≥ 5× wall-clock | _to fill_ |
| Memory: peak heap on 1M-tri mesh | within 1.5× octree | _to fill_ |
| Build time, 1M-tri | ≤ 5× octree | _to fill_ |
| SMS firefly count, glass+skin | within 5% | _to fill_ |
| BDPT caustic radiance | within 2% | _to fill_ |
| Apple Silicon BVH4 vs. octree | ≥ 4× wall-clock | _to fill_ |

If targets are met, the initiative closes. Decisions on Phase 5 (compressed nodes) and Phase 6 (TLAS) are made based on whichever metric was the bottleneck:

- Memory bottleneck → Phase 5 (compressed nodes).
- Object-traversal bottleneck (rare unless scenes have thousands of objects) → Phase 6 (TLAS BVH).
- Apple Silicon bottleneck → potentially BVH8 NEON via 2×4 (D3 revisit).

---

## 10. Cross-Phase Concerns

### 10.1 Build files

Per [CLAUDE.md](../CLAUDE.md), every `.cpp` / `.h` add or remove must touch all five build projects:

1. `build/make/rise/Filelist`
2. `build/cmake/rise-android/rise_sources.cmake`
3. `build/VS2022/Library/Library.vcxproj` (`<ClCompile>` / `<ClInclude>`)
4. `build/VS2022/Library/Library.vcxproj.filters` (`<Filter>` tags)
5. `build/XCode/rise/rise.xcodeproj/project.pbxproj` (four sections, two targets)

A new "Acceleration" filter group should be added in VS2022 filters and the Xcode group structure to keep the tree clean.

### 10.2 Determinism

Every BVH width must produce deterministic, reproducible hits given the same scene + same ray. Tie-breaking rule: `(t, primitiveIndex)`. Documented in `BVHTraversal.h` header comments and unit-tested.

### 10.3 Documentation deliverables

In addition to this plan:
- Update [docs/PERFORMANCE.md](PERFORMANCE.md) "Acceleration structures" section after Phase 1 ships.
- Update [docs/ARCHITECTURE.md](ARCHITECTURE.md) to reference BVH instead of octree/BSP for triangle meshes.
- Update [src/Library/README.md](../src/Library/README.md) and [src/Library/Geometry/README.md](../src/Library/Geometry/README.md) (if it exists) to point at the new acceleration directory.
- Update [CLAUDE.md](../CLAUDE.md) "High-Value Facts" section to mention BVH for triangle meshes.

### 10.4 Profiling

Counters added (`Profiling.h`):
- `nBVHNodesVisited` — total internal nodes traversed
- `nBVHLeavesVisited` — total leaves entered
- `nBVHFloatFilterTests` — float Möller / Woop calls
- `nBVHDoubleCertifyTests` — double certifications
- `nBVHFilterRejects` — float filter rejected before certify

These let us answer "is the float filter pulling its weight" and "is the tree shape good" empirically.

### 10.5 Memory accounting

Track (in tests / via `RISE_LOG` at info level on build):
- BVH node count
- Triangle reference count (post-SBVH duplication)
- Total bytes (nodes + triangle data + pointers)
- Build time

Report at scene load. Compare against pre-BVH octree baseline in the regression report.

### 10.6 Skill applicability

- [adversarial-code-review](skills/adversarial-code-review.md) — fire after each phase lands. Three orthogonal reviewers: correctness (precision split, hit equivalence), performance (SAH metrics, SIMD utilization), platform (compile and run on macOS/Linux/Windows/Android).
- [performance-work-with-baselines](skills/performance-work-with-baselines.md) — every phase needs a documented baseline before code, single-variable measurements, stddev, correctness invariant, numeric report.
- [abi-preserving-api-evolution](skills/abi-preserving-api-evolution.md) — even though we're explicitly breaking the ABI, the reasoning in this skill applies to any further changes that try to layer on top mid-flight. Keep the new API tight from day one.
- [const-correctness-over-escape-hatches](skills/const-correctness-over-escape-hatches.md) — BVH build mutates the tree; traversal is logically const. Use the decision tree, not `mutable`.

### 10.7 Target Hardware Matrix

This initiative is developed against three reference machines. The compile-time SIMD width selection (D2) is calibrated to these targets. Any deviation from this matrix during execution should be flagged in the corresponding phase PR.

| Machine | OS | CPU / SoC | SIMD ISA | `RISE_BVH_WIDTH` |
|---|---|---|---|---|
| MacBook (Apple Silicon) | macOS | M-series (Firestorm/Avalanche/Everest cores) | NEON Advanced SIMD 128-bit; SME on M4 (limited) | **4** |
| Windows laptop | Windows 11 | **Intel Core Ultra 9 185H** (Meteor Lake, Core Ultra Series 1, 2024) — 6 P-cores (Redwood Cove) + 8 E-cores (Crestmont) + 2 LP E-cores, 16C/22T, 45 W H-class | AVX2, FMA, BMI2, AVX-VNNI; **no AVX-512** | **4** |
| Samsung Galaxy Fold 7 | Android 14+ | Snapdragon 8 Elite (Qualcomm Oryon 2nd-gen, ARMv9.2-A) — or Exynos 2500 in some regions | NEON 128-bit + SVE2 (typically 128-bit width on this generation) + FP16/BF16 | **4** |

**i9 generation gotcha.** The user's i9 is the *only* one of the three machines where SIMD capability depends on the exact part:

| i9 generation | Code name | AVX-512? | BVH8 native? |
|---|---|---|---|
| 9th, 10th gen (i9-9900K, i9-10900K) | Coffee/Comet Lake | ✗ | ✗ |
| **11th gen (i9-11900K)** | **Rocket Lake** | **✓** | **✓** |
| 10980XE | Cascade Lake-X | ✓ | ✓ |
| 12th gen (i9-12900K) | Alder Lake | ~ (disabled in microcode) | ✗ |
| 13th, 14th gen (i9-13900K, i9-14900K) | Raptor Lake | ✗ | ✗ |
| **Core Ultra 9 185H (this project's box)** | **Meteor Lake (Series 1)** | **✗** | **✗** |
| Core Ultra 9 285K (desktop, 2024) | Arrow Lake (Series 2) | ✗ | ✗ |
| Core Ultra 9 288V (thin-and-light) | Lunar Lake (Series 2) | ✗ | ✗ |

**Project owner's chip confirmed.** Core Ultra 9 185H = Meteor Lake = no AVX-512. BVH4 via AVX2 is the production Windows path. Any collaborator on a different chip should run `wmic cpu get name` (Windows), `cat /proc/cpuinfo | grep avx512` (Linux/WSL), or check System Profiler (macOS Intel) before assuming BVH4 — if AVX-512 is present, override to BVH8.

**Practical consequence.** With the matrix as stated above:
- BVH4 is the production default on **all three** primary dev machines.
- BVH8 is validated on a separate AVX-512-capable test box (CI runner or remote workstation) for correctness; benchmarked there for the perf claims in §8.5.
- BVH8 in the AVX2-via-2×4 fallback is exercised on the Core Ultra 9 185H build only to confirm no regression vs. BVH4.

**Meteor Lake-specific notes for the Windows build.**

- **Hybrid asymmetry.** Crestmont E-cores execute 256-bit AVX2 as 2× 128-bit micro-ops, so per-core BVH4 traversal throughput on E-cores is ~half a P-core's. The existing work-stealing thread pool ([src/Library/Utilities/ThreadPool.cpp](../src/Library/Utilities/ThreadPool.cpp), [src/Library/Utilities/CPUTopology.cpp](../src/Library/Utilities/CPUTopology.cpp)) handles this; no BVH-side change required. Document the per-core counter spread in the Phase 4 evaluation report.
- **LP E-core cluster.** Meteor Lake's SoC tile carries 2 low-power E-cores running at lower clocks. RISE's `render_thread_reserve_count` policy (per [CLAUDE.md](../CLAUDE.md) "Thread priority policy") already reserves one E-core for UI; verify whether the LP cores are included or excluded by current `CPUTopology` logic before the Phase 4 perf claims are made. If they're included, the long-tail tile finishing time can hide BVH4 wins behind scheduler noise.
- **Thermal envelope.** 45W H-class. Sustained AVX2 heavy load will throttle. Phase 1 / 3 / 4 perf measurements on this box must use the [performance-work-with-baselines](skills/performance-work-with-baselines.md) skill protocol: warm-up render, thermal-stable measurement window, stddev across runs, ambient temp noted in the report. Don't trust a single cold-start render number.
- **Memory bandwidth.** LPDDR5X (typical 7467 MT/s on Meteor Lake H-class) — BVH traversal is bandwidth-sensitive once the tree exceeds L3 (24 MB on this chip). A 1M-tri SBVH at ~256 B/node is ~16 MB just for nodes; fits in L3, but spilling triangle data + ray state will hit DRAM. Worth a `vtune` or `pmu-tools` capture during the Phase 4 evaluation if perf disappoints.

### 10.7.1 Build configuration per platform

| Platform | Compiler | SIMD flags | Macros to expect |
|---|---|---|---|
| macOS arm64 | Apple Clang | `-mcpu=apple-m1` (or later) | `__ARM_NEON`, `__aarch64__` |
| Windows x64 (i9, AVX2) | MSVC `cl.exe` | `/arch:AVX2 /favor:INTEL64` | `__AVX2__`, `__AVX__`, `_M_AMD64` |
| Windows x64 (i9, AVX2) | Clang-cl | `-mavx2 -mfma -mbmi2` | `__AVX2__`, `__FMA__`, `__BMI2__` |
| Linux x86_64 (AVX2) | GCC / Clang | `-mavx2 -mfma -mbmi2` | `__AVX2__`, `__FMA__`, `__BMI2__` |
| Workstation AVX-512 (CI / Xeon / 11th-gen i9) | GCC / Clang | `-mavx512f -mavx512vl -mavx512bw -mavx512dq` | `__AVX512F__`, `__AVX512VL__`, `__AVX512BW__` |
| Workstation AVX-512 | MSVC | `/arch:AVX512` | `__AVX512F__` |
| Android arm64 (Galaxy Fold) | Android NDK Clang | `-march=armv8.2-a+fp16+dotprod` | `__ARM_NEON`, `__ARM_FEATURE_FMA`, `__aarch64__` |

**MSVC quirks to watch on the Windows i9 build:**

- `/arch:AVX2` enables AVX2 + FMA + AVX-VNNI but **does not define `__BMI2__`** even when the chip supports it. If we use BMI2 intrinsics (`_pext_u32`, `_pdep_u32`, `_bzhi_u32`), guard with explicit MSVC checks or skip them entirely. Avoid them if possible — they're not on the BVH critical path.
- MSVC doesn't define `__SSE2__`, `__SSE4_2__`, etc. Use `_M_IX86_FP`, `_M_AMD64`, or wrap in `defined(_MSC_VER) && defined(_M_AMD64)`.
- MSVC needs `<intrin.h>` for some intrinsics (`__cpuid`, `_BitScanForward`) that GCC/Clang put in `<immintrin.h>` or builtins.
- **Subnormal flush-to-zero must be set per-thread** on the BVH worker pool: `_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON)` and `_MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON)`. Otherwise the float filter can hit subnormals on grazing rays and stall the pipeline. The double-precision certifier is unaffected (denormals are rarer at 64-bit).
- MSVC's auto-vectorizer is conservative; the `Vec4f` / `Vec8f` wrappers must use explicit intrinsics, not "trust the compiler."

**NDK quirks on the Galaxy Fold build:**

- Default arm64 target is `armv8.0-a`, which omits FP16. Bump to `armv8.2-a+fp16+dotprod` minimum.
- `vminq_f32` / `vmaxq_f32` are the NEON equivalents of `_mm_min_ps` / `_mm_max_ps`. Wrapper layer in `SIMD_NEON.h` maps cleanly.
- SVE2 is available on Snapdragon 8 Elite but at 128-bit width — same as NEON for our purposes. Don't add a separate SVE2 path in this initiative; document as future work if 256-bit SVE2 ever ships on consumer Android.
- Apple Silicon NEON and ARM Android NEON are bit-identical for the ops we use; one `SIMD_NEON.h` covers both.

### 10.7.2 CI matrix

Minimum required:

| Runner | Build | Tests | BVH widths exercised |
|---|---|---|---|
| macOS arm64 | Apple Clang, `-mcpu=apple-m1` | Full unit + render regression | 2, 4 |
| Windows x64 | MSVC, `/arch:AVX2` | Full unit + render regression | 2, 4 |
| Windows x64 | MSVC, `/arch:AVX2`, `RISE_BVH_WIDTH=8` override | Unit only (BVH8 fallback path) | 8 (AVX2 2×4) |
| Linux x64 AVX2 | GCC, `-mavx2 -mfma -mbmi2` | Full unit + render regression | 2, 4 |
| Linux x64 AVX-512 | GCC, `-mavx512*` | Full unit + render regression + BVH8 perf bench | 2, 4, 8 |
| Android arm64 | NDK Clang, `armv8.2-a+fp16+dotprod` | Build-only (no render regression on phone) | 2, 4 |

If a Linux AVX-512 runner is unavailable in CI, the AVX-512 build can be smoke-tested manually on a workstation per release; not PR-gating but tracked in the Phase 4 evaluation report.

### 10.7.3 Performance expectations per machine

End-to-end BioSpec face render after Phase 4, BVH8 vs. octree baseline:

| Machine | Expected speedup | Width used |
|---|---|---|
| Apple Silicon M-series | 4–6× | NEON BVH4 |
| Core Ultra 9 185H (Meteor Lake mobile) | 4–6× | AVX2 BVH4 |
| Galaxy Fold 7 (Snapdragon 8 Elite) | 3–5× | NEON BVH4 |
| AVX-512 workstation (CI / 11th-gen i9 / Xeon-W) | 7–10× | AVX-512 BVH8 |

These are working estimates; Phase 4 evaluation (§9) records actuals.

---

## 11. Sequencing Rules

- One phase merges to master before the next begins.
- Each phase's validation gates are non-negotiable. A 4% SMS firefly increase is not "close enough."
- Within a phase, prefer one PR per logical step (builder, then mesh integration, then scene sweep, then test) over a single mega-PR.
- The scene-file sweep ships in the same PR as the parser change, so master never has dangling references to removed keys.
- Build-file updates (all five projects) ship in the same PR as the new files.

---

## 12. Open Items Pending Decision

| # | Item | Default if not decided |
|---|---|---|
| O1 | ~~Should we serialize the built BVH to disk (faster scene reload) or always rebuild on load?~~ | **Resolved: serialize.** See §4.5 — `.risemesh` exists specifically to avoid acceleration-structure build cost, and a 1M-tri SBVH rebuild on every scene load (5–60 s) is unacceptable for benchmarks. New v3 format is documented. |
| O2 | Should `bvh_build_sbvh` be controllable per-scene, or compile-time-fixed to ON? | Per-scene, default ON. Some users may want plain SAH for build-time-sensitive workflows. |
| O3 | Should triangle precomputed data live in BVH leaves or in a parallel array indexed by triangle? | BVH leaves (better cache). |
| O4 | Should NEON BVH4 be the default Apple Silicon binary, or BVH2 with NEON ops? | NEON BVH4 (per D3). |
| O5 | Any benchmark scene we should add to the repo specifically for tracking BVH perf? | Yes — designate an existing BioSpec face scene as the canonical bench, document in `bench.sh`. |
