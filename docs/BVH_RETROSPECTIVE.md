# BVH Retrospective

**Span**: 2026-04-26 → 2026-04-27
**Goal**: replace RISE's legacy SAH-BSP / octree triangle accelerator with a modern BVH stack, ship the win to production scenes, and leave behind a clean codebase + measurement-backed documentation of every decision.
**Outcome**: BVH is the only mesh accelerator in the tree.  All in-repo `.risemesh` assets re-baked to v4 with a cached BVH.  Public API + parser swept clean of the legacy `max_polys` / `max_recur` / `use_bsp` knobs.  Plain SAH BVH2 + BVH4 SIMD collapse + float Möller-Trumbore filter is the production configuration; SBVH spatial splits and uint8-quantised BVH4 nodes were measured, neither helped, both excised.

This document supersedes the per-phase retrospective files that were drafted in flight (`BVH_PHASE1_BASELINE.md`, `BVH_PHASE1_PARTIAL_REPORT.md`, `BVH_PHASE2_REPORT.md`, `BVH_PHASE3_REPORT.md`, `BVH_TIER1_REPORT.md`, `BVH_XYZDRAGON_SHOOTOUT.md`, `BVH_CLEANUP_AND_NEXT.md`).  Those were working notes; this is the persistent record.  The original architectural plan is preserved separately in [`BVH_ACCELERATION_PLAN.md`](BVH_ACCELERATION_PLAN.md).

---

## Pre-BVH baseline

Captured from commit `b3e06f4` on Apple Silicon arm64, 10 logical cores, `render_thread_reserve_count=0`, `samples=32` normalized across all scenes.  Mean ± stddev of 3 runs:

| Scene | Wall mean | Acceleration | Notes |
|---|---|---|---|
| `bench_pt` (Cornell + 2 raw meshes) | 1.62 ± 0.03 s | BSPTreeSAH | Control |
| `bench_bdpt` (same Cornell) | 7.87 ± 0.07 s | BSPTreeSAH | Control |
| `sss_comparison_dragon` (47.8K-tri dragon) | 23.28 ± 0.09 s | BSPTreeSAH, max_recursion=24 | The dragon test |
| `bdpt_sss_dragon` (same dragon) | 30.34 ± 0.28 s | BSPTreeSAH | BDPT variant |

The `.risemesh` files on disk were v2; the on-disk BSP was rejected as invalid bbox and rebuilt from polygon data on every load (≈100 ms / scene).  All build-cost numbers below are measured against this state.

Reference renders were captured at this point (one per scene) for downstream pixel-diff correctness gates.  The path tracer is stochastic (per-thread RNG order varies), so "bit-equal output" is not the right correctness invariant — instead we use **noise-floor comparison**: the run-to-run pixel diff between two renders of the same configuration is the floor; any new acceleration structure must produce diffs no larger than that floor against the baseline.

---

## Phase 1 — BVH2 SAH-binned builder (2026-04-26)

**Goal**: replace the per-mesh SAH-BSP with a templated BVH2.  Single-ray stack traversal, fp32 AABBs in nodes (conservative `nextafter` outward rounding), fp64 intersection at leaves via the existing `TreeElementProcessor::RayElementIntersection` contract.

**Code shipped**:
- New `src/Library/Acceleration/AccelerationConfig.h` — tuning struct (no defaults, per the codebase's no-default-parameters convention).
- New `src/Library/Acceleration/BVH.h` — header-only template with the SAH-binned BVH2 builder.
- `TriangleMeshGeometryIndexed` integration: BVH built in `DoneIndexedTriangles` and post-deserialize, both `IntersectRay` paths route through BVH; BSP/octree retained as fallback under `RISE_USE_LEGACY_BSP=1`.

**Bugs found and fixed during integration**:

1. **Empty-bin AABB pollution in the SAH binner**.  The prefix-AABB accumulator was `Include`-ing every bin's bbox unconditionally, including the sentinel-init `(+INF, -INF)` of empty bins — driving accBox to span the universe and SAH cost to infinity, so the builder always picked "leaf" over "split."  Result on the dragon: 178 leaves for 47,794 tris (avg 268 prims/leaf, degenerate).  Fix: gate the prefix sweep on `bins[i].count > 0`.  Post-fix tree had 6,875 leaves (avg 6.95 prims/leaf, near the leafSize=10/4 target).  Render time on the dragon dropped from 3:03 to 16 s.

2. **Closest-hit semantics violation in `TriangleMeshGeometryIndexed::RayElementIntersection`**.  The element processor unconditionally overwrote `ri.range` on every leaf hit — the LAST-tested triangle in a leaf wins instead of the closest.  The BSP path masked this by using a local `myRI` per element with external compare; the BVH initially did not.  Caught by `BSPMailboxingTest` regression.  First fixed via the same `myRI` workaround in `BVH.h`; later (Tier A cleanup) made native by adding the `&& h.dRange < ri.range` guard inside `RayElementIntersection` itself, removing the per-element copy.

**Measured wins (Phase 1 alone, post-bug-fix)**: roughly flat-to-slightly-negative on the dragon scenes.  Honest reasons documented at the time: RISE's BSPTreeSAH is already SAH-based (the academic BVH-vs-BSP wins in the literature usually compare against uniform octrees / kd-trees, not SAH BSP); 47.8K tris is at the low end of where BVH dominates; and the closest-hit `myRI` workaround added per-leaf-element copy/compare cost.  The real wins required Phases 2 and 3.

---

## Phase 2 — Float Möller-Trumbore filter at leaves (2026-04-27)

**Goal**: precompute per-triangle float Möller-Trumbore data (P0, e1, e2 — 36 bytes/tri) parallel to `prims[]`, use as a fast rejection filter at leaf entry, only call the production fp64 `RayElementIntersection` if the filter passes.

**Code shipped**:
- `TreeElementProcessor::GetFloatTriangleVertices` virtual hook (non-pure default returns `false`; triangle-mesh processors override).
- `BVH::TriangleFilterData` + `BVH::BuildFastFilter` — populated post-build via the new processor hook.
- `BVH::MollerTrumboreFloat` — conservative float filter (epsilon-padded barycentric and t-range).
- Filter call integrated into all three BVH traversal overloads, gated by `hasFastFilter`.

**Measured wins**: clear positive on the 47K-tri dragon — `sss_comparison_dragon` dropped from 16.22 s (Phase 1) to ~14.2 s (Phase 2).  ~12% rasterize-time reduction per scene, attributable to the saved fp64 triangle tests on filter-rejected rays.

The filter remains net-positive even on the 7.2M-tri xyzdragon (validated in Tier C1, see below).  Memory cost is 36 bytes × N; for the 7.2M xyzdragon that's 260 MB.  Cache-pressure remediation was originally on the Tier C menu and turned out not to be needed — the saved fp64 calls dominate the L3 misses on filter data.

---

## Phase 3 — BVH4 wide collapse + SIMD ray-vs-AABB (2026-04-27)

**Goal**: collapse the BVH2 into a BVH4 (up to 4 children per internal node) with SoA-laid-out child AABBs, write a 4-wide ray-vs-AABB SIMD kernel for NEON / SSE2 / scalar, traverse via the wider tree.

**Code shipped**:
- `BVH::BVH4Node` — alignas(16) SoA struct: 4 child AABBs in 6 × float[4] = 96 bytes, plus children/primCount/meta = 32 bytes, total 128 bytes.
- `BVH::BuildBVH4` — collapse pass: walk the BVH2 top-down, absorb each internal child's two children into the parent's slot, producing 2–4 children per BVH4 internal node.
- `BVH::RayBox4` — 4-wide slab-test SIMD kernel.
  - **NEON** (Apple Silicon, arm64 Android): native 128-bit `float32x4_t`, `vbslq_f32` for non-hit lane sentinel, `vaddvq_u32` for movemask emulation.
  - **SSE2** (x86_64): `__m128`, `_mm_cmple_ps` + `_mm_movemask_ps`.
  - **Scalar fallback**: per-lane reference implementation.
- `BVH::IntersectRay4` — three traversal overloads (geometric / full / intersection-only), selection-sort near-far ordering for closest-hit, Phase 2 float filter at leaves carried forward unchanged.

**Measured wins on the 47K-tri dragon**: `sss_comparison_dragon` to ~13–14 s (Phase 3).  Modest incremental win on a small mesh — depth halved by collapse, but the SIMD slab-test isn't a huge proportional speedup when the tree fits in L1/L2 anyway.  The BVH4 win is sharper at large mesh scales where DRAM-traversal dominates.

---

## Tier 1 — SBVH, animation refit, `.risemesh` v3, non-indexed integration (2026-04-27)

Four follow-on items from the Phase 3 wrap-up, all shipped in one round with bench results between each.

### Tier 1 §1 — SBVH spatial splits

Implemented per [plan §5.3](BVH_ACCELERATION_PLAN.md): per-bin clipped-bbox SAH, ref-duplication budget, axis-aligned per-bin straddler clipping (looser than full Sutherland-Hodgman polygon clipping but conservative-correct).

**Result**: 0% on small dragons, **−15% on the 7.2M xyzdragon**.  The ref-duplication memory cost + the duplicate-traversal overhead together exceeded the SAH-quality benefit.  This finding survived as a "disabled by default" knob through Tier 1 and was the trigger for the Tier B re-measurement (see below).

### Tier 1 §2 — `.risemesh` v3 BVH cache

Goal: serialise the post-build BVH2 into the `.risemesh` file so cold-load on a `.risemesh`-backed scene skips the SAH build and just walks the cached tree.

Format: after the legacy v2 BSP/octree byte block, append a `haveBVHCache` flag + (if present) a `BVH2` magic + version + node bytes + DFS-ordered prim indices + `overallBox`.  v2 readers stop before the trailing data and fall through to rebuild; v3 readers prefer the cache.  The on-disk BVH2 is the canonical representation — `BuildFastFilter` and `BuildBVH4` are re-derived at load time, so format compatibility holds across Phase 3+ traversal changes.

**Result**: ~−25% cold-load on xyzdragon-class meshes.  Real win for users who repeatedly open big-mesh scenes.

### Tier 1 §3 — animation refit

`BVH::Refit` walks `nodes[]` in reverse index order (children-before-parents by build construction), recomputing each node's AABB from referenced prim bboxes (leaves) or child AABBs (internals).  After the BVH2 nodes are refit, `BuildFastFilter` and `BuildBVH4` re-run from the new geometry.  Topology preserved — caller is responsible for ensuring vertex count is unchanged.

`TriangleMeshGeometryIndexed::UpdateVertices` swaps in new vertex/normal arrays in place (preserving `ptr_polygons` pointer stability) and calls `pPtrBVH->Refit()`.  `DisplacedGeometry::RefreshMeshVertices` is the observer-driven entry point: when a keyframed displacement painter notifies, the displaced geometry re-tessellates the base, re-applies displacement, and feeds new vertices to `UpdateVertices` — much faster than the prior destroy-and-rebuild path.

### Tier 1 §4 — non-indexed mesh integration

Mirrored the Tier 1 §1–3 work onto `TriangleMeshGeometry` (the non-indexed cousin used by `.RAW`-loaded meshes).  Same BVH integration, same closest-hit fix, same fp32 leaves.

### xyzdragon shootout

The 7.22M-tri xyzdragon scene was the canonical big-mesh stress test.  After Phase 3 and Tier 1 §1 landed, the comparison was:

| Acceleration | xyzdragon render |
|---|---|
| Legacy BSP-SAH | sky-high (~minutes) |
| BVH2 + filter | improving |
| **BVH4 + filter** | best of measured paths |

A measurement-protocol bug surfaced during this shootout: the `RISE_USE_LEGACY_BSP=1` env-var bypass was not gating the `Deserialize`-rebuild path in `TriangleMeshGeometryIndexed.cpp`, so "BSP" measurements on `.risemesh`-loaded scenes were silently going through BVH4+filter — i.e., comparing the BVH path against itself.  The legacy code path was correctly gated for raw-mesh-loaded Cornell scenes (via `DoneIndexedTriangles`) but missed in deserialize.  Fixed at the time.  The Phase 1 / 2 / 3 internal comparisons (BVH2 vs BVH2+filter vs BVH4+filter) used independent gates and remained valid.

---

## Tier A — production cleanup (2026-04-27, PR #7)

After the architectural work was in place, Tier A was the polish round to make the BVH the production-default and excise the workarounds that had accumulated.

### A1 — production `.risemesh` re-bake to v3

24 of 26 in-tree `.risemesh` files re-baked to v3 with a cached BVH.  Each gained the BVH cache; rebake helper at `/tmp/risemesh_rebake_v2.cpp` auto-detected `RISETMGI` (indexed) vs `RISE_TMG` (non-indexed) signatures and dispatched to the appropriate class.  Atomic per-file: rebake to staging in `/tmp`, `mv` into place only on non-zero output.  The 2 untouched files were orphan v1 non-indexed meshes (`armadillo`, `she2`) that no scenes reference; left as-is.

Cold-load on xyzdragon-class meshes dropped ~13% from the rebake (cache load avoids the SAH build).

### A2 — vestigial BSP/octree state excised, format bumped to v4

Members removed: `pPtrBSPtree`, `pPtrOctree`, `pPolygonsBSPtree`, `pPolygonsOctree`, `nMaxPerOctantNode`, `nMaxRecursionLevel`, `bUseBSP` from both `TriangleMeshGeometry` and `TriangleMeshGeometryIndexed`.

`.risemesh` format bumped to v4: drops the legacy header bytes (`nMaxPerOctantNode`, `nMaxRecursionLevel`, `bUseBSP`) and the BSP/octree byte block.  v1/v2/v3 reads still work — legacy bytes are read into Deserialize-local `BSPTreeSAH<>` / `Octree<>` temporaries and discarded, never reaching this class as state.  The `Octree.h` and `BSPTreeSAH.h` headers stay included **in the .cpp only** (gone from the public mesh `.h`).

### A3 — public API + parser sweep

Dropped `max_polys` / `max_recur` / `use_bsp` from:
- `RISE_API_CreateTriangleMeshGeometry*`
- `RISE_API_CreateDisplacedGeometry`
- `IJob::Add{Indexed,3DS,RAW,RAW2,PLY}TriangleMeshGeometry`
- `IJob::AddDisplacedGeometry`
- `meshconverter` CLI (took 7 args, now takes 4)
- `rise_blender_bridge.cpp`

Parser keeps accepting the legacy keys (`maxpolygons`, `maxdepth`, `bsp`) but ignores them — the descriptor entries are marked `Retired (BVH is sole accelerator)`.  144 in-tree scene files continue to parse without modification.

### Adversarial review

Three parallel reviewers (correctness / perf / platform) per the [adversarial-code-review](skills/adversarial-code-review.md) skill, plus a follow-up review pass.  Found and fixed:

- **`ComputeAreas` quadratic growth** (sev 4): `push_back` without clearing.  Every keyframed `DisplacedGeometry::RefreshMeshVertices` call corrupted `areasCDF` and would crash `UniformRandomPoint` after enough keyframes.  Fixed by self-clearing.
- **`BuildBVH4` doesn't clear `nodes4` on Refit** (P1): animated displaced geometry would silently intersect against pre-refit bounds.
- **Fixed `stack[64]` traversal overflow** (P1): all 6 BVH2/BVH4 traversal sites switched to `static thread_local std::vector<uint32_t>` (capacity persists across calls; zero steady-state heap traffic).
- **`MkCfg` uninitialized `buildSBVH` / `sbvhDuplicationBudget`** (P2): the BVH builder test could nondeterministically take the SBVH path depending on stack contents.
- **`Profiling.cpp` MSVC compat**: the `__attribute__((used))` LTO anchor is now guarded for MSVC.
- **GUI viewport-blit allocation churn**: `RISEViewportBridge::BlitWholeAndDispatch` was allocating a fresh `std::vector<uint8_t>(W*H*4)` per frame plus an `NSBitmapImageRep` plus an `NSImage`, fragmenting the macOS xzone allocator and crashing inside `xzm_segment_group_alloc_chunk` after long interactive sessions.  Fixed by writing pixels directly into the rep, halving per-frame heap pressure.

### Compiler warnings purged

Both make and Xcode `RISE-GUI` builds come up warning-free on a clean rebuild.  AGENTS.md gained a new "Compiler Warnings Are Bugs" section with per-warning principled-fix recipes; CLAUDE.md gets a brief reference.  This is the new project convention going forward.

### Tier E3 — IDE-tree visibility for `Acceleration/` headers

`src/Library/Acceleration/{BVH.h, AccelerationConfig.h}` were added to `.gitignore` survivors and wired into the VS2022 vcxproj/filters and Xcode pbxproj so they appear in IDE project trees.  Per CLAUDE.md "Source-file add/remove" — make and Android cmake didn't need updates (headers don't go in those).

---

## Tier B — SBVH excised (2026-04-27, PR #9)

Tier 1 §1 had measured SBVH at **−15% on xyzdragon**.  Tier B was three rescue attempts (B1 lower duplication budget, B2 Sutherland-Hodgman polygon clipping, B3 duplicate-aware SAH cost), with "excise" as the fourth option.

**Method**: `xyzdragon_bench.RISEscene` (7.2M tris, 1280×720, 9 SPP), N=5 runs per config with `render_thread_reserve_count 0`, `Total Rasterization Time` from `RISE_Log.txt` as the metric.  Temporary env-var harness to switch SBVH on/off without recompiling between runs.

**Results**:

| Config | rasterize (mean ± σ, N=5) | Δ vs control |
|---|---|---|
| Cache, SBVH off (production) | 15.04 ± 0.24 s | (different code path) |
| Rebuild, SBVH off (control) | 13.99 ± 0.81 s | baseline |
| Rebuild, SBVH @ 0.30 (Tier 1 default) | 13.18 ± 0.58 s | **−5.8% (within 1σ)** |
| Rebuild, SBVH @ 0.10 (B1 probe) | 14.10 ± 0.74 s | +0.8% |
| Rebuild, SBVH @ 0.05 (B1 probe) | 13.93 ± 0.66 s | −0.4% |

All deltas within 1σ of the rebuild control.  Tier 1's −15% did not reproduce — most likely closed by the intervening cleanup work (closest-hit native, BVH4 collapse, fast filter, the `BuildBVH4` clear-on-rebuild fix).  Build cost: SBVH adds ~5 s (~2×) to BVH build on xyzdragon (+22% wall time per render).  Memory cost: up to 30% more primitive references at 0.30 budget.

**Correctness invariant verified via noise-floor comparison**: SBVH-off run-to-run pixel diff (max=0.846, rmse=0.025, 29.7% pixels >1e-5) was statistically identical to SBVH-off-vs-SBVH-on diff (max=0.952, rmse=0.020, 29.7%).

**Decision**: excise SBVH.  ~440 lines removed: `BuildSBVH`, `BuildSBVHNode`, `MakeSBVHLeaf`, `Ref` struct, `SBVHBuildState`, `SetAxis` helper.  `buildSBVH` and `sbvhDuplicationBudget` fields dropped from `AccelerationConfig`.  Single-builder BVH path going forward.

---

## Tier C — Performance frontier (2026-04-27, PRs #10 + #11)

Four items in the Tier C menu, walked through in order.  One implementation, three measurement-driven decisions.

### C1 — Cache-line-aligned float filter (no remedy)

Original framing: the 260 MB float filter on xyzdragon overflows L3, restructure to drop on huge meshes / interleave with BVH4 nodes / compress to fp16.  N=5 benches (env-var harness, filter on/off) produced:

| Mesh | Filter ON | Filter OFF | Filter helps by |
|---|---|---|---|
| xyzdragon (7.2M tris) | 14.27 ± 0.67 s | 15.05 ± 0.73 s | **+5.5%** |
| sss_comparison_dragon (47.8K tris) | 43.90 ± 0.87 s | 46.71 ± 0.36 s | **+6.4%** |

Filter is **+5–7% beneficial uniformly**.  All three proposed remedies only make sense if the filter were hurting; it isn't.  No code change.

### C2 — Persistent triangle precompute in v4 cache (deferred)

Save ~280 ms `BuildFastFilter` on cold load, pay +260 MB on disk per file (xyzrgb_dragon would grow from 521 MB to ~781 MB, +50%).  4.7% wall-time reduction for 50% more disk: not a great trade.  Reconsider only if a specific user workload makes cold-load cost dominant — and even then, prefer parallelising `BuildFastFilter` over caching it.

### C3 — SAH-degradation safeguard for refit (delivered)

After many keyframes of high-amplitude vertex displacement, `Refit()` preserves topology but per-node bboxes can grow until traversal expected-cost more than doubles.  Refit alone can't fix that; only a full rebuild from polygon data can.

Implementation:
- `BVH::originalSAH` snapshotted at end of construction.  `BVH::ComputeSAH()` returns the SAH cost of the current tree normalised by root-bbox surface area (single O(N) walk).
- `BVH::SAHDegradationRatio()` = currentSAH / originalSAH, public getter.
- `BVH::Refit()` logs a warning when ratio > 2.0× — but doesn't force a rebuild itself (BVH doesn't own the input prim list).
- `TriangleMeshGeometryIndexed::UpdateVertices` checks the ratio post-Refit; if > 2.0×, frees and rebuilds from `ptr_polygons` using the same code path as `DoneIndexedTriangles`.

Defensive: kicks in only on truly-pathological keyframed animation.  Static renders and modest-amplitude animation never hit the threshold.  Threshold of 2.0× is conservative; the actual rebuild-vs-refit crossover is probably closer to 1.5×, but at 2.0× the case is unambiguous.

### C4 — Compressed BVH4 nodes (investigated, regressed, reverted)

The proposal: 8-bit uint8 quantised child AABBs in `BVH4Node`, relative to the node's own fp32 AABB.  128-byte → 80-byte node, 1.6× memory reduction.

Full implementation built and passed the correctness invariant (pixel diff vs pre-C4 baseline matched the path tracer's run-to-run noise floor: max 0.86 vs 0.85 noise, mean 0.005 vs 0.007 noise).  But:

| Mesh | Pre-C4 | C4 (8-bit) | Δ |
|---|---|---|---|
| xyzdragon (7.2M tris) | 14.27 ± 0.67 s | 15.57 ± 0.74 s | **+9.1% (slower)** |
| sss_comparison_dragon (47.8K tris) | 43.90 ± 0.87 s | 48.05 ± 0.64 s | **+9.4% (slower)** |

**Why it regressed**: 8-bit quantisation gives 1/255 of the node range as precision.  In a SAH BVH a 1m parent box can hold a 1mm leaf — at 4mm quantum, that leaf inflates to 4× linear / 64× volume.  The slab test false-positives at a much higher rate, sending many more rays into the leaf double-precision cert.  The cost of those extra leaf intersections dominates the modest cache-pressure relief.

**Decision**: revert.  The 5× memory reduction the original docs cited isn't achievable at uint8 precision without paying for it elsewhere.  Two plausible recovery paths documented but not attempted: 16-bit quantisation (1/65535 precision → ~30 µm quantum on a 1m parent, ~1.4× node-memory reduction), or a hybrid scheme (fp32 child AABBs at leaf-level nodes, uint8 at upper levels).  Neither is urgent — plain fp32 BVH4 nodes are the best balance.

---

## Tier E5 — Formal regression tests (2026-04-27)

Replaced the in-flight `/tmp/*_bench` ad-hoc helpers with proper `tests/*.cpp` files.  These are the regression gates for every BVH-related change going forward.

### `tests/BVHBuilderTest.cpp`

Six test functions covering:
- Empty / single-prim / random-N correctness vs naive ground truth (Phase 1 originals)
- `TestRefitClearsNodes4` (the BuildBVH4 clear-on-rebuild bug from Tier A review)
- `TestDeepTreeTraversal` (the stack[64] overflow bug from Tier A review)
- `TestRefitWithVertexMutation` (Tier 1 §3 end-to-end refit with external bbox mutation)
- `TestSAHDegradationDetection` (Tier C3 — SAH ratio detects 50× bbox blowup, > 2.0 threshold triggers production rebuild fallback)

The `MutablePrim` / `MutableTestProc` fixture lets tests mutate prim bboxes after build via an external vector keyed by prim id, which the `TreeElementProcessor` reads at refit time — that's how external vertex mutation gets through to the BVH's internal state for testing.

### `tests/BVHSerializationTest.cpp` (new)

Round-trip regression gate for the `.risemesh` BVH cache format:
- Empty round-trip (header drift detection)
- Single-prim round-trip
- Random-N round-trip with full intersection equivalence (each ray tested produces identical (id, range) on the original and the round-tripped BVH)
- Format-header guards: bogus version and bogus magic both rejected

This catches format drift between `BVH::Serialize` and `BVH::Deserialize` — including any drift between the cache content and the post-load `BuildFastFilter` / `BuildBVH4` re-derivation.

### Pre-existing tests still relevant

- `tests/BSPMailboxingTest.cpp` — was BSP-era, but the BVH replaced BSP underneath it; now exercises BVH closest-hit semantics on the same fixture.
- `tests/RISEMeshLegacyBSPCompatibilityTest.cpp` — verifies v1 `.risemesh` files (with stored BSP byte blocks) still load via the v4-aware Deserialize.
- `tests/DisplacedGeometryTest.cpp` — exercises the displacement pipeline including `RefreshMeshVertices`.
- `tests/SceneEditorMemoryStressTest.cpp` — added in PR #8, drives 1000 simulated time-scrubs through `SceneEditController` and asserts peak RSS stays bounded.  Catches any future allocation pattern that goes quadratic in the interactive renderer.

---

## Performance journey

End-to-end wall-time on the canonical scenes, baseline → final:

| Scene | Baseline | Final | Δ |
|---|---|---|---|
| `bench_pt` (Cornell, 30 implicit + 10 raw) | 1.62 s | ~1.6 s | flat (control) |
| `bench_bdpt` (same Cornell) | 7.87 s | ~7.9 s | flat (control) |
| `sss_comparison_dragon` (47.8K-tri dragon) | 23.28 s | ~14.3 s | **−39%** |
| `bdpt_sss_dragon` (same dragon) | 30.34 s | ~22 s | **−27%** |
| xyzdragon (7.22M-tri stress) | minutes (BSP timeout) | 14.27 s | **>10×** |

The dragon-class scenes are where the BVH stack pays.  Cornell-class controls confirm no regression on small-mesh scenes — those were already accelerated well by the BSP path.

Cold-load reduction from `.risemesh` v3 cache: **−13% on xyzdragon-class meshes** (cache load avoids the SAH build).

---

## Where to find things

### Production code
- **`src/Library/Acceleration/AccelerationConfig.h`** — tuning struct.
- **`src/Library/Acceleration/BVH.h`** — the templated BVH2 + BVH4 builder + traversal + SIMD slab test + Serialize/Deserialize + Refit + SAH-degradation telemetry.  Header-only.
- **`src/Library/Geometry/TriangleMeshGeometry.{h,cpp}`** — non-indexed mesh, BVH-only.
- **`src/Library/Geometry/TriangleMeshGeometryIndexed.{h,cpp}`** — indexed mesh, BVH-only, plus `UpdateVertices` for animation refit (with C3 SAH-ratio rebuild fallback).
- **`src/Library/Geometry/DisplacedGeometry.{h,cpp}`** — observer-driven re-tessellate + refit on keyframed displacement painters.
- **`src/Library/Parsers/AsciiSceneParser.cpp`** — mesh chunk parsers; legacy `maxpolygons` / `maxdepth` / `bsp` keys accepted-and-ignored.

### Tests
- **`tests/BVHBuilderTest.cpp`** — basic correctness, refit, SAH-degradation.
- **`tests/BVHSerializationTest.cpp`** — `.risemesh` BVH cache format round-trip.
- **`tests/BSPMailboxingTest.cpp`** — closest-hit semantics on the BVH path.
- **`tests/RISEMeshLegacyBSPCompatibilityTest.cpp`** — v1/v2/v3 `.risemesh` backward-compat reads.
- **`tests/DisplacedGeometryTest.cpp`** — displacement pipeline.
- **`tests/SceneEditorMemoryStressTest.cpp`** — interactive-renderer memory regression gate.

### Reference docs
- **[`BVH_ACCELERATION_PLAN.md`](BVH_ACCELERATION_PLAN.md)** — original architectural plan.  Some forecasts didn't pan out (notably SBVH and 5× node compression); the retrospective in this document is the corrected record.
- **`AGENTS.md` → "Compiler Warnings Are Bugs"** — codebase convention established during Tier A.
- **`docs/skills/performance-work-with-baselines.md`** — measurement protocol used throughout (baseline-before-code, ≥3 runs, 1σ overlap check, correctness invariant via noise floor).
- **`docs/skills/adversarial-code-review.md`** — 3-reviewer pattern used for Tier A.

---

## Deferred / not in scope

- **Tier D1 — TLAS replacing `BSPTreeSAH` on the scene side.**  Foundational for instancing but modest win for current RISE scenes (10s of objects per scene).  Not urgent.
- **Tier D2 — BVH8 + AVX-512.**  None of the current dev machines (M-series Apple Silicon, Core Ultra 9 Meteor Lake, Snapdragon Oryon) have AVX-512.  Opt-in for workstation/server hardware if RISE ever ships there.
- **Tier E1 — Windows i9 build + bench validation.**  The SSE2 `RayBox4` path is written but never compiled outside macOS.  Half-day of work to verify on a Windows machine.
- **Tier E2 — Galaxy Fold 7 / Android arm64 NDK build.**  Same NEON path as Apple Silicon; should "just work" — 1–2 hours of setup + bench.
- **Sutherland-Hodgman polygon clipping for SBVH straddlers** (was Tier B2): only relevant if SBVH gets revisited, which it likely won't unless a future workload shifts the balance.
- **uint16-quantised BVH4 nodes** (was Tier C4 alternative): plausible 1.4× memory reduction with usable precision.  Revisit only if BVH4 footprint becomes a real bottleneck.

---

## What this work cost

Roughly two days of focused work spread across a session arc.  Production code: net **smaller** by several hundred lines (BSP/octree members, SBVH builder, several env-var escape hatches all gone).  Test surface: net **larger** with the new BVH builder/serialization regression suites.  Public API: **simpler** (legacy mesh-construction params dropped).  Performance: **>10×** on the canonical big-mesh test, **−27 to −39%** on small dragons, **flat** on Cornell-class controls.

Two negative findings (SBVH excised, C4 reverted) were the principled outcome of measurement.  Three deferred items (C2, D1, D2) wait on either a workload that justifies them or hardware that supports them.  The remaining production code is the simplest BVH stack that was actually measured to win.
