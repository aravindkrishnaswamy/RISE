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

1. **`src/Library/Acceleration/{BVH.h,AccelerationConfig.h}` untracked + absent from IDE projects.** `make` works (auto-discovery), VS2022/Xcode users see them as orphan headers.  Per CLAUDE.md "Source-file add/remove" they need `<ClInclude>` in `Library.vcxproj` + `.vcxproj.filters` and `PBXFileReference`+`PBXGroup` entries in `project.pbxproj`. **Cost: ~30 min** (E3 in this doc's menu). [Resolved in PR #7.]
2. **Non-indexed `.risemesh` doesn't cache its BVH on disk.** Every load rebuilds. Pre-existing. Negligible for current asset library (no large non-indexed meshes), would matter if RISE ever ships a 1M+ tri non-indexed mesh.
3. **v2 `.risemesh` cold-load cost** (with `bptrbsptree=1` and a real stored tree): allocates+parses+discards a full BSPTreeSAH on read. Pre-existing — A2 didn't change it; just made the discard explicit. The just-rebaked v3 files have `bptrbsptree=0` so this cost only applies to externally-sourced v2 files.

---

## Tier B — investigated and excised (2026-04-27)

### Question

Tier 1 §1 originally measured SBVH at **−15% on xyzdragon-class meshes** (7.22M triangles).  Tier B was three plausible rescue attempts (B1 lower duplication budget, B2 Sutherland-Hodgman polygon clipping, B3 duplicate-aware SAH cost), with "excise" as the fourth option if none helped.  This section captures the decision and the data behind it.

### Method

`scenes/Internal/xyzdragon_bench.RISEscene` (1280×720 pinhole, 9 SPP, max_recursion=2, the canonical big-mesh stress).  All measurements with `render_thread_reserve_count 0` per [`AGENTS.md`](../AGENTS.md) thread-priority guidance.  Metric: `Total Rasterization Time` from `RISE_Log.txt` (excludes parse + BVH build, isolates the structural quality of the accelerator).  N=5 per config after one warmup pass.  Wall-clock and `parse+build` captured separately for the SBVH @ 0.30 case to quantify the build-time tax.

A temporary env-var harness (`RISE_BENCH_SBVH`, `RISE_BENCH_SBVH_BUDGET`, `RISE_BENCH_FORCE_REBUILD`) was added to `TriangleMeshGeometryIndexed.cpp` so the four rebuild configs and the cache-baseline could share one binary.  Removed before the excision commit; not committed.

### Results

| Config | rasterize mean ± stddev (N=5) | Δ vs control | wall | parse+build |
|---|---|---|---|---|
| Cache, SBVH off (production) | 15.043 ± 0.244s | (different code path — uses v3 cache) | n/a | n/a |
| Rebuild, SBVH off (apples-to-apples control) | 13.993 ± 0.808s | baseline | ~19.9s | ~5.9s |
| Rebuild, **SBVH @ 0.30** (Tier 1 default) | 13.175 ± 0.582s | **−5.8%** (within 1σ) | ~24.4s | ~10.9s |
| Rebuild, **SBVH @ 0.10** (B1 probe) | 14.097 ± 0.741s | +0.8% | — | — |
| Rebuild, **SBVH @ 0.05** (B1 probe) | 13.932 ± 0.662s | −0.4% | — | — |

Rasterize-time deltas at every duplication budget tested are **within 1σ** of the rebuild control — i.e., not statistically significant.  The ostensibly-best result (SBVH @ 0.30, −5.8%) sits in the noise band, not above it.

### Correctness invariant

Path-traced output is stochastic, so a strict pixel-equality check is meaningless.  Instead we use a **noise-floor comparison**: render the scene twice with the exact same configuration, measure the run-to-run pixel diff, and compare that floor to the SBVH-vs-non-SBVH diff.  If the SBVH diff is comparable to or smaller than the run-to-run floor, SBVH is functionally correct.

| diff | max | mean | rmse | %pixels >1e-5 |
|---|---|---|---|---|
| SBVH off — run vs run (noise floor) | 0.846 | 0.0066 | 0.0247 | 29.70% |
| SBVH off vs SBVH @ 0.30 | 0.952 | 0.0047 | 0.0197 | 29.69% |

The two distributions are statistically indistinguishable.  SBVH produces correct images; ~30% of pixels visibly differ across any two renders simply because traversal-order changes cascade through the path tracer's branching, and one-bounce-different paths can blow up a pixel's contribution.

### Why Tier 1's −15% didn't reproduce

Most likely closed by intervening cleanup work between Tier 1 and now:

- **Closest-hit native** in `RayElementIntersection` (Cleanup §1) — was a workaround in BSP days; now native, removes per-element copy/compare overhead.
- **BVH4 collapse** (Phase 3) — fewer nodes traversed per ray.
- **Float Möller-Trumbore filter** (Phase 2) — gates the expensive double-precision intersection.
- **`BuildBVH4` clear-on-rebuild fix** (PR #7 review finding) — prior runs may have been double-collapsing on Refit, biasing measurements.
- **Bounded-dynamic traversal stack** (PR #7 review finding) — `thread_local std::vector` instead of fixed `stack[64]`; removes any depth-related correctness/perf cliff.

In short, the BVH path's cost factors that SBVH was meant to ameliorate are now small enough that spatial splits don't have a useful margin to recover.

### Cost of keeping SBVH

| Axis | Penalty |
|---|---|
| **Build time** | +5s on xyzdragon (~2× the SAH builder); +22% wall-time per render |
| **Memory** | Up to +30% primitive references at 0.30 budget; permanent overhead |
| **Code surface** | ~250 lines of dual-builder logic in `BVH.h` (`BuildSBVH`, `BuildSBVHNode`, `Ref` struct, spatial-split SAH path, ref-budget tracking) |
| **Config surface** | Two fields on `AccelerationConfig` (`buildSBVH`, `sbvhDuplicationBudget`) and seven init sites that have to keep them in sync |

### Decision

**Excise SBVH.**  B2 (Sutherland-Hodgman polygon clipping) and B3 (duplicate-aware SAH) might tighten the result by a few percent more, but neither would close the gap from "null" to "worth the build/memory/code cost".  The structural answer is that with the closest-hit-native intersection and BVH4 collapse already in place, plain SAH is extracting most of the locatable benefit on this codebase's geometry.  Rather than carry a dual-builder branch that's known-no-help on the hardest workload, we revert to a single-path SAH BVH and reclaim the simplicity.

The excision lands as a follow-up PR after this one — see commit history for the diff.

---

## Tier C — partial outcome (2026-04-27)

### C1 — Cache-line-aligned float filter: **no-op**

The original C1 framing assumed the 260 MB float filter on xyzdragon was a net cost (cache pressure exceeding L3, hurting traversal).  Three remedies were proposed: (a) drop on huge meshes, (b) interleave with BVH4 nodes, (c) compress to fp16 (18 B/tri).  All three only make sense if the filter is hurting.

| Mesh | Filter ON | Filter OFF | Δ (filter helps by) |
|---|---|---|---|
| xyzdragon (7.2M tris) | 14.27 ± 0.67s | 15.05 ± 0.73s | **+5.5%** |
| sss_comparison_dragon (47.8K tris) | 43.90 ± 0.87s | 46.71 ± 0.36s | **+6.4%** |

(N=5 each, `Total Rasterization Time` from RISE_Log.txt, env-var `RISE_BENCH_FILTER_OFF=1` flips the filter off at construction.)

The filter is **+5–7% beneficial uniformly** across mesh sizes.  Option (a) "drop on huge meshes" would lose 5.5% on xyzdragon — the opposite of what we want.  (b) and (c) only matter if the filter's memory pressure was the bottleneck, but the filter is already winning despite that pressure — the saved double-precision `RayElementIntersection` calls more than compensate for the L3 misses.

**Decision: no remedy.**  The filter is doing its job.  If a future workload appears where the filter is genuinely costly (e.g. a 100M-tri mesh where DRAM bandwidth becomes the limiter), revisit option (c) — fp16 compression has the cleanest correctness story (the conservative filter accepts all true hits regardless of precision; double-precision cert handles false positives at the leaf).

### C2 — Persistent triangle precompute in v3 cache: **deferred**

C2 would serialise the 260 MB float filter to disk in the v4 `.risemesh` format, saving the rebuild step on cold load.  Cost-benefit on xyzdragon:

- **Saved**: ~280 ms of `BuildFastFilter` work on every cold load
- **Paid**: +260 MB on disk per file (xyzrgb_dragon goes from 521 MB to ~781 MB, +50%)
- **Wall-time impact**: ~280 ms / ~5900 ms parse+build = **4.7% cold-load reduction**

This is not a great trade.  Animation refit (the originally-cited beneficiary) is still subject to the same `BuildFastFilter` walk on every Refit, but Refit on a 7.2M-tri mesh is already animation-only territory and rare in practice.

**Decision: deferred.**  Reconsider only if a specific user workload makes the cold-load cost significant — and even then, prefer making `BuildFastFilter` faster (parallelise it across cores) over caching it on disk.

### C3 — SAH-degradation safeguard for refit: **delivered**

After many keyframes of high-amplitude vertex displacement, `Refit()` preserves topology but per-node bboxes can grow until ray traversal expected cost exceeds the freshly-built tree's by a wide margin.  `Refit()` alone can't fix that — only a full rebuild from polygon data can.  C3 adds the detection + caller-driven rebuild trigger.

Implementation (~30 min, in this PR):

1. **`Scalar BVH::originalSAH`** captured once at construction (after Build/BuildFastFilter/BuildBVH4 finish).  `ComputeSAH()` returns the SAH cost of the current tree normalised by root surface area — a single O(N) walk.
2. **`Scalar BVH::SAHDegradationRatio()`** = `ComputeSAH() / originalSAH`, public getter.
3. **`Refit()`** logs a warning at >2.0× and an info log otherwise — but doesn't force a rebuild itself (it doesn't own the input prim list).
4. **`TriangleMeshGeometryIndexed::UpdateVertices`** checks the ratio post-Refit; if >2.0, frees and rebuilds the BVH from `ptr_polygons`.

Defensive: kicks in only on truly-pathological keyframed animation.  Static renders and modest-amplitude animation never hit the threshold.  The 2.0× threshold is conservative — the BVH's actual cost-vs-rebuild tradeoff probably favours rebuild even at 1.5×, but at 2.0× the case is unambiguous.

### C4 — Compressed BVH4 nodes: **next up**

Defer to its own PR.  ~1–2 days to implement 6-byte quantised child AABBs (vs current 32-byte fp32) — a ~5× node-memory reduction.  More important than C1 because the BVH4 node count on xyzdragon is ~2.3M × 128 B = ~295 MB, which is bigger than the float filter and the dominant L3-overflow source on huge meshes.
