# BVH Cleanup + Next-Steps Menu

**Date:** 2026-04-27
**Machine:** macOS arm64 (Apple Silicon, 10 logical cores)
**Reference:** [BVH_TIER1_REPORT.md](BVH_TIER1_REPORT.md)

This pass excises the architectural workarounds and debug knobs accumulated through Phases 1–3 + Tier 1, then lays out the menu for what's next.

---

## Cleanup delivered

| # | Item | Outcome |
|---|---|---|
| 1 | Fix `RayElementIntersection` closest-hit semantics directly | `if (h.bHit && h.dRange < ri.range)` guard now native on both `TriangleMeshGeometry` and `TriangleMeshGeometryIndexed` overloads. Removes the BSP-pattern workaround that's been a latent bug since Phase 1 §6.2. |
| 2 | Remove BVH `myRI` per-element copy/compare | All 4 leaf-intersection sites (BVH2 geometric, BVH2 full, BVH4 leaf-geometric, BVH4 leaf-full) now call `ep.RayElementIntersection( ri, ...)` directly. Saves a ~120-byte struct copy per leaf-prim test. |
| 3 | Remove env-var escape hatches | `RISE_DISABLE_SBVH`, `RISE_DISABLE_BVH_FILTER`, `RISE_DISABLE_BVH4`, `RISE_USE_LEGACY_BSP` (4 sites) all gone. The `cfg.buildSBVH` flag remains as the (false-by-default) toggle for future SBVH investigation. |
| 4 | Excise dead BSP/octree branches | `IntersectRay`, `IntersectRay_IntersectionOnly`, `GenerateBoundingBox`, and the build paths in `DoneIndexedTriangles` / `DoneTriangles` / `Deserialize`-rebuild now BVH-only. `pPtrBSPtree` / `pPtrOctree` / `pPolygonsBSPtree` / `pPolygonsOctree` members preserved on the classes for v1/v2 `.risemesh` deserialize compat (data read into them, then ignored). |

**Code surface:** `BVH.h` (~50 lines deleted), `TriangleMeshGeometry.{h,cpp}` (~30 deleted), `TriangleMeshGeometryIndexed.{h,cpp}` (~80 deleted), `TriangleMeshGeometryIndexedSpecializations.h` (~5 changed), `TriangleMeshGeometrySpecializations.h` (~5 changed). Net: ~165 lines removed, codebase notably simpler.

**Tests:** 64/64 still green throughout.

**Perf no-regression** (3 runs each, post-cleanup):

| Scene | Pre-cleanup (Tier 1) | Post-cleanup | Δ |
|---|---|---|---|
| `sss_comparison_dragon` (PT, 47.8K) | 12.943 ± 0.140s | 12.871 ± 0.086s | −0.6% (within noise) |
| `bdpt_sss_dragon` (BDPT, 47.8K) | 21.576 ± 0.564s | 21.660 ± 0.128s | +0.4% (within noise) |

The closest-hit fix's expected ~1–2% recovery from saving the per-element copy is **lost in MC noise on these scenes** — the copy was a small fraction of total render time. On a triangle-intersection-dominated micro-benchmark the savings would be more visible; for end-to-end render the win is real but unobservable without a profiler.

---

## Next-Steps Menu

Organized by tier and ROI. Each item is independently committable; pick whichever matches the session's appetite.

### Tier A — Production polish (low risk, near-term value)

**A1. Production `.risemesh` re-bake to v3.** [Tier 1 §2 deferred work]
Run the rebake helper across `models/risemesh/` so users get the v3 cold-load win (−25% on xyzdragon-class meshes) without manual intervention. Likely 5–10 min of compute, plus a script + commit. **ROI**: large (every dragon-class scene loads ~5s faster). **Risk**: low (v3 format proven; rebake helper proven).

**A2. Remove vestigial BSP/octree members from triangle mesh classes.** [Cleanup §3+§4 follow-on]
Once we're confident no v1/v2 `.risemesh` files remain in active use (after A1), excise `pPtrBSPtree`, `pPtrOctree`, `pPolygonsBSPtree`, `pPolygonsOctree`, `nMaxPerOctantNode`, `nMaxRecursionLevel`, `bUseBSP` from the classes; bump `cur_version` to 4 (drop legacy BSP/octree bytes from the on-disk format). Touches more files (parser, scene-file serialization, etc.). **ROI**: medium (cleaner class layout, smaller mesh files). **Risk**: medium (format change; needs compat plan).

**A3. Public API + parser sweep.** [Plan §4.1, §4.4 deferred]
Replace constructor signatures (`maxpolygons`, `maxrecursion`, `bsp`) with `AccelerationConfig`. Sweep scene files to drop the legacy keys. **ROI**: medium (cleaner API; `bvh_max_leaf_size`, `bvh_build_sbvh` etc. as scene-file-tunable). **Risk**: medium (touches every scene file, every API caller).

### Tier B — SBVH rescue attempts (uncertain ROI)

The SBVH code is in place but disabled; Tier 1 §1 found it +0% on small dragons and −15% on xyzdragon. Three plausible fixes that could rescue it:

**B1. Lower duplication budget** (try 0.05 or 0.10 instead of 0.30).
Quick experiment: change one constant + bench. If 0.05 still doesn't help on xyzdragon, SBVH is structurally not a fit for this codebase's other cost factors and we can excise the SBVH path entirely. **ROI uncertain; cost <30 min.**

**B2. Sutherland-Hodgman polygon clipping** instead of axis-aligned bbox clipping for SBVH straddlers.
The plan §5.3 originally called for this; my Tier 1 implementation took the simpler bbox-clip path. True polygon clipping gives tighter per-bin AABBs, which makes the spatial-split SAH cost more accurately reflect actual leaf overlap. **ROI: 30 min implementation; could rescue SBVH; could not.**

**B3. Duplicate-aware SAH cost** (penalise duplicates beyond the area term).
Current SAH formula treats duplicates as independent prims, but a duplicate adds per-ray traversal cost without adding hit information. Adjust cost to: `base_cost + dup_penalty × num_straddlers`. Could prevent SBVH from over-duplicating on cache-bound large meshes. **ROI uncertain; cost ~30 min.**

If all three fail to deliver, **excise SBVH code entirely** as a fourth option (saves ~250 lines + the dual builder branch).

### Tier C — Performance frontier (real wins, more work)

**C1. Cache-line-aligned float filter.** [xyzdragon shootout §9.5 deferred]
The 260 MB filter on xyzdragon overflows L3. Restructuring to either (a) drop filter on huge meshes above a primitive-count threshold, or (b) interleave filter data with BVH4 nodes (saves cache misses), or (c) compress to 18 B/triangle (half-precision floats), would directly target the cache-bound regime. **ROI: 5–15% on xyzdragon-class meshes. Cost: half a day.**

**C2. Persistent triangle precompute in v3 cache.** [Tier 1 §3 deferred]
The Tier 1 §3 refit bench showed `BuildFastFilter` re-derive (260 MB) dominates refit cost at 7.22M tris. Caching it on disk in v3 would make refit primarily the AABB walk (~285 ms vs 488 ms today). Trades ~250 MB more disk for ~40% faster refit on huge meshes. **ROI: animation-only. Cost: medium (format extension + cache-load path).**

**C3. SAH-degradation safeguard for refit.** [Plan §4.6 deferred]
After many keyframes of high-amplitude displacement, refit-only can degrade tree quality below useful. Track `currentSAH / originalSAH`; if > 2.0, full rebuild instead of refit. **ROI: specific to high-amplitude animation; defensive. Cost: ~30 min.**

**C4. Compressed BVH4 nodes.** [Plan §5 deferred]
6-byte quantised child AABBs (vs current 32 bytes). ~5× node memory reduction. Important for 7M-tri-class meshes that overflow L3 today. **ROI: 5–10% on huge meshes via cache; bigger headroom for SBVH if we revisit. Cost: 1–2 days.**

### Tier D — Architectural completion (significant scope)

**D1. TLAS (top-level BVH).** [Plan deferred]
ObjectManager currently uses BSPTreeSAH for scene-level acceleration. With BVH proven on the BLAS side, TLAS swap is the natural next step. Modest win for typical RISE scenes (10s of objects), bigger for instance-heavy scenes if RISE ever supports instancing. **ROI: small for current workloads; foundational for future. Cost: 2–3 days.**

**D2. Phase 4 BVH8 + AVX-512.** [Plan §8 deferred]
None of the project's three primary dev machines support AVX-512 (Apple M-series NEON-only, Core Ultra 9 185H = Meteor Lake = no AVX-512, Galaxy Fold = Snapdragon Oryon NEON). This is opt-in for workstation/server hardware. **ROI: 1.3–1.6× over BVH4 on AVX-512 hardware; 0% on dev hardware. Cost: 2–3 days + AVX-512 access.**

**D3. SBVH animation+refit interaction** (if SBVH gets rescued in Tier B).
Refit assumes static topology; SBVH-with-spatial-splits has duplicates per leaf which need careful refit handling (refit each duplicate's clipped sub-bbox, not the whole triangle's bbox). Defer until SBVH itself proves valuable.

### Tier E — Cross-platform + process

**E1. Windows i9 (Core Ultra 9 185H) build + bench.** [Plan §10.7]
The SSE2 `RayBox4` path is written but never compiled outside macOS. CI runner or local Windows build to verify + bench. **ROI: validation; surfaces any clang-vs-MSVC differences early. Cost: half a day.**

**E2. Galaxy Fold 7 / Android arm64 NDK build.** [Plan §10.7]
Same NEON path as Apple Silicon; should "just work". **ROI: validation. Cost: 1–2 hours setup + bench.**

**E3. Build-system cosmetics.** Add new `Acceleration/` headers to VS2022 vcxproj+filters and Xcode pbxproj for IDE-tree visibility. **ROI: minor (auto-discover already works for compiles). Cost: 30 min.**

**E4. Adversarial code review.** [adversarial-code-review skill]
3 parallel reviewers across the BVH stack: correctness/precision, perf/SAH-quality, platform/build. Likely surfaces edge-case bugs we haven't tripped over yet. **ROI: defect-prevention; cost: 1 review pass × 3 reviewers.**

**E5. Formal unit tests.** Replace the `/tmp/*_bench` ad-hoc helpers with proper `tests/*.cpp` files: `BVHRefitTest`, `BVHSerializationTest`, `RISEMeshLegacyLoadTest`, `MeshConverterTest`, `AnimatedDisplacedGeometryTest`. The plan §5.5/§5.6 listed all of these; they'd carry forward as gates for any future BVH work. **ROI: regression-protection; cost: 1–2 days.**

---

## My recommendation, given the work already done

If picking ONE: **Tier A1 (production .risemesh re-bake)**. Lowest risk, highest user-visible value (every BVH user gets a faster cold-load), cheapest to ship.

If picking THREE: **A1 → C1 (cache-line filter) → A3 (API+parser sweep)**. The first ships the v3 win to users; the second targets the cache-bound regime that's holding back the 7M+ meshes; the third makes the BVH config user-tunable from scene files.

If targeting "what would Embree do": Tier C and D are where the comparable production renderers live. SBVH usually lives, with C1+C4 as standard practice; TLAS is universal at the architectural level; AVX-512 is ISA-conditional but present.

Honest red flag: Tier B (SBVH rescue) is uncertain ROI. The xyzdragon -15% regression is the kind of result where the right answer might be "abandon SBVH for this codebase, the cache-bound regime makes duplication a net negative." Worth 30 minutes of B1 to know for sure, but not worth multi-day investment without that signal.

---

## Tier A — delivered (2026-04-27)

### A1: production `.risemesh` re-bake to v3

| | Result |
|---|---|
| Files rebaked | 24 of 26 (`models/risemesh/*.risemesh` + `internal/`) |
| Skipped | 2 orphan v1 non-indexed files (`armadillo`, `she2`) — no scenes reference them; restored from iCloud after first-pass corruption discovered the dual-format need |
| Cold-load delta | dragon_small `12.16s` wall, xyzdragon `18.52s` wall (−13% vs pre-rebake) |
| Tool | `/tmp/risemesh_rebake_v2.cpp` — auto-detects `RISETMGI` (indexed) vs `RISE_TMG` (non-indexed) signature and dispatches |
| Atomicity | `/tmp/rebake_all.sh` → staging via `/tmp/rebake_staging_*` → `mv` only on non-zero output |

### A2: vestigial BSP/octree state excised

| Removed from class state | Indexed mesh | Non-indexed mesh |
|---|---|---|
| `pPtrBSPtree` / `pPolygonsBSPtree` | yes | yes |
| `pPtrOctree` / `pPolygonsOctree` | yes | yes |
| `nMaxPerOctantNode` | yes | yes |
| `nMaxRecursionLevel` | yes | yes |
| `bUseBSP` | yes | yes |

The `Octree.h` and `BSPTreeSAH.h` headers stay included **in the .cpp only** so legacy v1/v2/v3 byte streams can be consumed via Deserialize-local temporaries; the headers are gone from the public mesh `.h`.

**On-disk `.risemesh` format bumped to v4:**

| Field | v1 | v2 | v3 | v4 |
|---|---|---|---|---|
| `signature` | RISETMGI / RISE_TMG | same | same | same |
| `version` | 1 | 2 | 3 | 4 |
| `nMaxPerOctantNode` (uint) | yes | yes | yes | **gone** |
| `nMaxRecursionLevel` (char) | yes | yes | yes | **gone** |
| `bUseFaceNormals` (char, indexed) | yes | yes | yes | yes |
| polygon data | yes | yes | yes | yes |
| `bDoubleSided` (char) | yes | yes | yes | yes |
| `bUseBSP` (char) | yes | yes | yes | **gone** |
| `bptrXX` flag + tree bytes | yes | yes | yes (often empty after A1) | **gone** |
| `haveBVHCache` (char, indexed only) | — | — | yes | yes |
| BVH cache bytes (indexed only) | — | — | optional | optional |

v1/v2/v3 reads still work — legacy bytes are read into Deserialize-local `BSPTreeSAH<>`/`Octree<>` temporaries (gated on `legacyHaveTree`), validated, and discarded. After this lands, a future v4 rebake pass would shrink files by 7 bytes/file (negligible) but mostly serves to drop the legacy block from the read path on common files.

### A3: public API + parser sweep

| Surface | Before | After |
|---|---|---|
| `RISE_API_CreateTriangleMeshGeometry` | `(ppi, max_polys, max_recur, double_sided, use_bsp)` | `(ppi, double_sided)` |
| `RISE_API_CreateTriangleMeshGeometryIndexed` | `(ppi, max_polys, max_recur, double_sided, use_bsp, face_normals)` | `(ppi, double_sided, face_normals)` |
| `RISE_API_CreateDisplacedGeometry` | `(..., max_polys, max_recur, double_sided, use_bsp, face_normals)` | `(..., double_sided, face_normals)` |
| `IJob::AddIndexedTriangleMeshGeometry` | 5 mesh-add methods | same with the 3 legacy params dropped |
| `IJob::AddDisplacedGeometry` | 10 args | 7 args |

**Scene-file backward compat:** the parser's mesh chunks (`3dsmesh_geometry`, `rawmesh_geometry`, `rawmesh2_geometry`, `displaced_geometry`) keep accepting `maxpolygons`, `maxdepth`, `bsp` keys via descriptor entries, but ignore them at Finalize time. 144 in-tree scene files continue to parse without modification.

**Patch geometries unchanged:** bezier-patch and bilinear-patch APIs still take `max_patches`/`max_recur`/`use_bsp` because those drive a *patch-internal* acceleration structure that wasn't touched by the BVH work.

**Touchpoints:**
- `src/Library/RISE_API.{h,cpp}` (factory signatures)
- `src/Library/Interfaces/IJob.h` + `src/Library/Job.{h,cpp}` (5 mesh-add + AddDisplacedGeometry)
- `src/Library/Geometry/TriangleMeshGeometry{,Indexed}.{h,cpp}` (class state + format v4)
- `src/Library/Geometry/DisplacedGeometry.{h,cpp}` (passes through to inner mesh)
- `src/Library/Parsers/AsciiSceneParser.cpp` (4 chunk parsers)
- `src/RISE/meshconverter.cpp` (CLI sig: dropped `<bsp> <maxpolys> <maxrecur>` args)
- `src/Blender/native/rise_blender_bridge.cpp` (Blender → RISE indexed-mesh handoff)
- 5 test fixtures (`BSPMailboxingTest`, `DisplacedGeometryTest`, `GeometrySurfaceDerivativesTest`, `TessellatedShapeDerivativesTest`, `RISEMeshLegacyBSPCompatibilityTest`)

### Adversarial code review (3 parallel reviewers)

| Reviewer | Result |
|---|---|
| Correctness | Found 1 BUG (sev 4): `ComputeAreas` push_back's without clearing — every keyframed `DisplacedGeometry::RefreshMeshVertices` would corrupt `areasCDF` and crash `UniformRandomPoint` after a few frames. Fixed in both indexed + non-indexed by making `ComputeAreas` self-clearing. Also flagged 2 minor concerns (empty-mesh Serialize asymmetry — fixed; v1 octree-byte-block warn-and-skip — single-stream-only, harmless). |
| Perf / cache | No regressions. Class shrunk 24 B/instance (indexed 240→216 B, non-indexed 144→120 B). `bDoubleSided` ↔ `pPtrBVH` gap shrunk 32 B → 8 B; both fields now share a 64-byte cache line in `IntersectRay` — sub-1% but free win. |
| Platform / build / API | No blockers. 1 ABI concern: `IJob` virtual signatures changed (intentional source-break; only `Job` implements `IJobPriv`). 1 ABI concern: `ITriangleMeshGeometryIndexed::UpdateVertices` was added pure-virtual in Tier 1 §3 (pre-A2, flagged for changelog). 1 build concern: `src/Library/Acceleration/{BVH.h,AccelerationConfig.h}` are untracked + absent from VS2022/Xcode project files — pre-existing from the BVH work, out of scope here. |

### Verification

- **Build**: `make -C build/make/rise -j8 all` — clean. Library + 4 binaries (rise, meshconverter, imageconverter, biospecbsdfmaker) link.
- **Tests**: 64/64 pass before AND after the review fixes.
- **Render**: `sss_comparison_dragon.RISEscene` (47.8K-tri dragon) renders in ~48s; `RISE_Log.txt` confirms `Loaded BVH cache (31151 nodes, 47794 prims) — skipping SAH rebuild` — the v3 .risemesh files just rebaked in A1 still load correctly through the new v4-aware Deserialize.

### Net diff

```
23 files changed, 655 insertions(+), 509 deletions(-)
```

Excluding the 9 .risemesh model-file blobs (rebaked content, not code):

```
14 source files + 5 tests changed
~140 net lines deleted from class state + ctor params + format byte block
~50 net lines added for v1/v2/v3 backward-compat read paths
```

### Pre-existing issues surfaced (out of scope, flag for follow-up)

1. **`src/Library/Acceleration/{BVH.h,AccelerationConfig.h}` untracked + absent from IDE projects.** `make` works (auto-discovery), VS2022/Xcode users see them as orphan headers.  Per CLAUDE.md "Source-file add/remove" they need `<ClInclude>` in `Library.vcxproj` + `.vcxproj.filters` and `PBXFileReference`+`PBXGroup` entries in `project.pbxproj`. **Cost: ~30 min** (E3 in this doc's menu).
2. **Non-indexed `.risemesh` doesn't cache its BVH on disk.** Every load rebuilds. Pre-existing. Negligible for current asset library (no large non-indexed meshes), would matter if RISE ever ships a 1M+ tri non-indexed mesh.
3. **v2 `.risemesh` cold-load cost** (with `bptrbsptree=1` and a real stored tree): allocates+parses+discards a full BSPTreeSAH on read. Pre-existing — A2 didn't change it; just made the discard explicit. The just-rebaked v3 files have `bptrbsptree=0` so this cost only applies to externally-sourced v2 files.
