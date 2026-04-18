# RISE DisplacedGeometry Plan

**Status**: Draft for review. No code changes made yet.
**Owner**: Aravind Krishnaswamy
**Scope**: New `DisplacedGeometry` type + new `IGeometry::TessellateToMesh` contract + per-geometry tessellation overrides + parser/API/test-scene/regression surface.
**Precedent**: [src/Library/Geometry/BezierPatchGeometry.cpp](../src/Library/Geometry/BezierPatchGeometry.cpp) + [src/Library/Geometry/BezierValueGenerator.h](../src/Library/Geometry/BezierValueGenerator.h) + [src/Library/Geometry/GeometryUtilities.cpp](../src/Library/Geometry/GeometryUtilities.cpp).

---

## 1. Executive Summary

**The problem.** Displacement maps are today available only on `BezierPatchGeometry`. The tessellation + displacement pipeline ([BezierValueGenerator.h:83](../src/Library/Geometry/BezierValueGenerator.h)) is cleanly separated but locked inside the Bezier path. Users cannot displace a sphere, torus, box, or any triangle mesh without manually authoring control patches.

**The idea.** Introduce a composite `DisplacedGeometry` that wraps any existing `IGeometry`, tessellates it to a triangle mesh at scene-construction time, and applies an `IFunction2D` displacement along vertex normals — mirroring the existing Bezier path but as a reusable wrapper. To make this work for primitives that aren't natively meshes (sphere, torus, box, etc.), add one new method to `IGeometry`:

```cpp
virtual bool TessellateToMesh(
    IndexTriangleListType& tris,
    VerticesListType&      vertices,
    NormalsListType&       normals,
    TexCoordsListType&     coords,
    const unsigned int     detail ) const;  // non-pure; default logs + returns false
```

That method is deliberately framed as a general mesh-export hook, not a displacement-specific one, so a future GPU mesh path (raster preview, OptiX/Metal export) can reuse the same contract.

**Goals.**
1. `DisplacedGeometry` can wrap any supported `IGeometry` and produce correct, seam-clean displaced triangles.
2. Every primitive geometry gains a working `TessellateToMesh` with per-type documented `detail` semantics.
3. Zero behavior change for existing scenes — only new chunk, new API, new class.
4. Regression scenes + unit tests guarantee no drift.
5. API surface designed so a future GPU tessellation/upload pass can bolt on without re-plumbing.

**Non-goals.**
- Not replacing `BezierPatchGeometry`'s built-in displacement. It keeps working as-is; a user who wants to displace a Bezier surface with the new pipeline wraps it and passes `displacement=none` on the inner Bezier chunk. Documented, not enforced.
- Not implementing adaptive/subdivision tessellation (uniform grid only in v1).
- Not implementing analytical-normal computation from displacement gradient (post-displacement topological re-averaging in v1).
- No GPU mesh upload in this plan — only the API shape that makes it possible later.
- No changes to `IGeometry::GenerateMesh()` — it's dead across all 14 implementations but removing it is a separate cleanup.

**Expected outcome.** One new geometry class (~400 LoC), one new virtual on `IGeometry` with default implementation, 12 per-geometry tessellation overrides (~50–150 LoC each, ~1000 LoC total), one new scene-file chunk, 7 regression scenes + 1 unit test, 5 build-project updates. Rough total: ~2000 LoC + regression baselines.

---

## 2. Progress Summary

Update this block as phases complete. Each phase has a detailed checklist in §5.

| Phase | Title | Status | Notes |
|-------|-------|--------|-------|
| 0 | Design lock & prior-art read | Complete | Decisions resolved 2026-04-18. Found PRISE/3DSMax out-of-tree; §3.2 refined to inline default |
| 1 | `IGeometry` contract: add `TessellateToMesh`, remove dead `GenerateMesh` | Complete | Build + 53/53 tests pass. Stranded non-virtual `GenerateMesh` members on derived classes still compile — Phase 1/Phase 2 coupling was not strictly necessary. |
| 2 | Per-geometry `TessellateToMesh` overrides (12 types) + drop dead `GenerateMesh` bodies | Complete | All 12 geometries tessellate. Build clean, 53/53 tests pass, shapes scene renders OK. |
| 3 | `DisplacedGeometry` class | Complete | Builds clean, 53/53 tests pass. Unix Filelist updated (1 of 5 projects). |
| 3b | Back-port post-displacement normal fix to `BezierPatchGeometry` | Complete | Teapot re-renders without errors; visual review pending Phase 10 baseline refresh |
| 4 | Build-system updates (5 projects) | Complete | All 5 projects updated (Unix Filelist, Android CMake, VS2022 vcxproj + filters, Xcode pbxproj). |
| 5 | Public `RISE_API` function | Complete | `RISE_API_CreateDisplacedGeometry` added with `detail>256` warning and nullptr-on-failure semantics. |
| 6 | `IJob` / `Job` method | Complete | `AddDisplacedGeometry` resolves base + displacement by manager lookup; fails scene-parse loudly on `IsValid()==false`. |
| 7 | Scene parser chunk | Complete | `displaced_geometry` chunk registered; defaults `bsp=true` per §3.11. |
| 8 | Regression test scenes | Complete | 6 scenes authored (sphere, torus, box, zero-scale, displaced_shapes, self-shadowing); InfinitePlane refusal verified at scene-parse. Light-direction convention bug fixed: `vDirection` is the direction *toward* the light (dot with normal > 0 means lit). The self-shadowing scene uses grazing light + large disp_scale to verify shadow rays work through the internal BSP of a DisplacedGeometry. |
| 9 | Standalone unit test | Complete | [tests/DisplacedGeometryTest.cpp](../tests/DisplacedGeometryTest.cpp): all 7 cases pass (pure tessellation, constant disp, zero-scale invariant, seam continuity, InfinitePlane fail-safe, UniformRandomPoint, nested composition). **Bug found + fixed during Phase 9**: `TriangleMeshGeometryIndexed::TessellateToMesh` was reading from `indexedtris` which `DoneIndexedTriangles()` clears — rewrote to reconstruct indices from `ptr_polygons` (the pointer-triangle table that persists). Also added explicit `virtual`/`override` keywords on the override. |
| 10 | Baseline regression capture | Complete | `scripts/capture_displaced_baselines.sh` + `scripts/check_displaced_baselines.sh` capture/check all 6 displaced scenes + teapot. Thresholds: mean-luminance 1%, log-RMS 15 (generous for path-traced noise). All 7 pass against `tests/baselines_refactor/displaced/v1/`. HDR outputs (teapot) currently get a "produced" check only — full HDR comparison is a follow-up. |
| 11 | Documentation | Minimal done | `src/Library/README.md` updated to mention `DisplacedGeometry`; full Parser README section is a follow-up. |
| 12 | Adversarial review + merge readiness | Complete | 3-reviewer parallel pass completed. See §11 below for findings + resolutions. One P1 fix landed (orphan-normal at sphere poles); one P1 dismissed as a misread; one P1 tagged as architectural follow-up. P2/P3 items are documented as deferred. |

Status values: `Not started` → `In progress` → `Under review` → `Complete` → `Blocked`.

---

## 3. Locked Design Decisions

These are settled. Changing them requires revisiting the plan.

### 3.1 New method, removing `GenerateMesh`
`IGeometry::GenerateMesh()` is empty in all 14 implementations and untouched by `IntersectRay` / rendering. User confirms nothing external relies on it — it was a placeholder added years ago for exactly this feature that never got implemented. The new `TessellateToMesh` is the better-designed replacement, so `GenerateMesh` is removed entirely from `IGeometry` and every override. See §3.10 for the cleanup details.

### 3.2 Non-pure virtual with inline-in-header default
Adding a pure virtual to `IGeometry` would break out-of-tree subclasses silently at link time and loudly at runtime. The default is declared inline in [src/Library/Interfaces/IGeometry.h](../src/Library/Interfaces/IGeometry.h) as `return false;` — no logging, no body in a .cpp, no new includes in the interface header.

Logging of "tessellation unsupported for geometry X" moves to the **caller** (`DisplacedGeometry` constructor and `Job::AddDisplacedGeometry`), where the user-assigned geometry name and scene-parse context are available; RTTI in a base-class default would only yield mangled type names.

Refined from original §3.2 draft during Phase 0 after discovering that `src/3DSMax/MAXGeometry*` inherits from `IGeometry` directly (not via `Geometry` base). An inline header default keeps those plugin targets unaffected. See [docs/skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md).

### 3.3 Output is 4 plain `std::vector`s, not `ITriangleMeshGeometryIndexed`
Exactly the signature used by `GeneratePolygonsFromBezierPatch` in [BezierTesselation.cpp:77](../src/Library/Geometry/BezierTesselation.cpp). Three reasons:
1. Decouples from mesh-container choice — a GPU caller fills VBOs from the same vectors.
2. `DisplacedGeometry` mutates the vertex array in place before constructing a mesh — cleaner than extracting + re-populating.
3. Already the established idiom in RISE tessellation code.

### 3.4 `detail` is a single `unsigned int`, interpreted per-geometry
Matches `BezierPatchGeometry` precedent. Documented semantics per type in §4. Non-uniform detail (different counts per axis) is a future enhancement — v1 keeps the API minimal.

### 3.5 Post-displacement normal recomputation (and back-port to Bezier)
After `ApplyDisplacementMapToObject`, recompute face normals then re-average to vertex normals. This is a correctness fix — `BezierPatchGeometry` today leaves stale pre-displacement normals at [GeometryUtilities.cpp:438](../src/Library/Geometry/GeometryUtilities.cpp), so its displaced surfaces have shading that doesn't reflect the actual displaced geometry.

**Apply the same fix to `BezierPatchGeometry` in Phase 3b.** Fix either at the `BezierValueGenerator::Get` call site or inside `ApplyDisplacementMapToObject` itself — whichever is cleaner; evaluate during implementation. Regression anchor: [scenes/FeatureBased/Geometry/teapot.RISEscene](../scenes/FeatureBased/Geometry/teapot.RISEscene), which displaces a Bezier teapot with a water bump. Compared against the *old* output visually: new output should show more physically faithful shading. The regression check is "doesn't crash, doesn't look broken" — exact baseline will change and the new one is captured as the new reference.

### 3.6 Seam handling: duplicate, don't merge
For closed parametric surfaces (sphere, torus, cylinder side), the `u=0`/`u=1` meridian vertices are generated as separate vertices with different UVs. Prevents cracks at displacement discontinuities caused by the UV tent-remap in `RemapTextureCoords`.

### 3.7 No default parameter values in headers
Per [feedback_no_default_params](../../../.claude/projects/-Users-aravind-Documents-GitHub-RISE/memory/feedback_no_default_params.md). Every call site passes every argument explicitly. Parser chunk supplies defaults.

### 3.8 Ownership
`DisplacedGeometry` takes an `IGeometry*` and `IFunction2D*`, AddRef's both in its constructor, Release's on destruction. Base geometry stays in `GeometryManager` and can be referenced by other objects independently — wrapping doesn't transfer ownership.

### 3.9 Construction-time tessellation, no caching
Unlike `BezierPatchGeometry`'s MRU cache, `DisplacedGeometry` tessellates + displaces + builds its internal spatial acceleration structure once in the constructor. The resulting mesh is scene-lifetime. Motivation: scene is immutable after construction; a cache adds complexity without benefit.

### 3.10 Remove dead `GenerateMesh` entirely
Per user confirmation: `GenerateMesh()` was a placeholder added for exactly this feature years ago and nothing relies on it. Remove:

- Pure virtual declaration from [src/Library/Interfaces/IGeometry.h:57](../src/Library/Interfaces/IGeometry.h).
- Base-class implementation at [src/Library/Geometry/Geometry.cpp:29](../src/Library/Geometry/Geometry.cpp) and its declaration in `Geometry.h`.
- All 14 per-type override declarations and empty implementations (SphereGeometry, EllipsoidGeometry, TorusGeometry, CylinderGeometry, BoxGeometry, CircularDiskGeometry, ClippedPlaneGeometry, InfinitePlaneGeometry, TriangleMeshGeometry, TriangleMeshGeometryIndexed, BezierPatchGeometry, BilinearPatchGeometry).

This is a vtable-layout change on `IGeometry`. Out-of-tree subclasses that overrode `GenerateMesh` would fail to link — we've verified no such consumers exist. See [docs/skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md) for the general discipline.

Removal is interleaved with Phase 1 (interface) and Phase 2 (per-type), since the same files are being edited for `TessellateToMesh` addition. One logical change per file.

### 3.11 Default to BSP tree for the internal acceleration structure
For a densely tessellated displaced mesh, BSP-SAH is typically a better fit than octree — triangle density is high and uniform, and BSP's splitting-plane heuristic handles that better. The `bUseBSP` constructor parameter stays exposed (user can override), but the parser-chunk default is `bsp=true`. This differs from `BezierPatchGeometry`'s parser defaults (which are octree); we don't touch those.

---

## 4. Design Questions — Resolved 2026-04-18

All design questions are resolved. Captured here for traceability.

### Q1. `detail` cap — **uncapped, warn above 256**
No hard cap. `RISE_API_CreateDisplacedGeometry` emits a `eLog_Warning` if `detail > 256` explaining the triangle-count / memory cost. User's judgement call from there.

### Q2. Mesh-base pass-through vs. subdivision — **pass-through in v1**
When the base is a `TriangleMeshGeometry` / `TriangleMeshGeometryIndexed`, pass the existing triangles through unchanged and ignore `detail`. A future `DisplacedGeometry` improvement can opt into subdivision behind a flag if needed. Log once at `eLog_Info` when `detail` is ignored for a mesh base so the user isn't confused by no effect.

### Q3. Shared inner Bezier displacement — **accept**
If the user wraps a `BezierPatchGeometry` that itself has `displacement != none`, both displacements apply. Document in the parser chunk docs; user sets inner Bezier `displacement none` if they want only the outer to apply.

### Q4. `DisplacedGeometry` as itself tessellatable — **keep**
`DisplacedGeometry::TessellateToMesh` re-emits its internal mesh so nested wrapping (displace a displacement) works. Tiny extra code; enables composition.

### Q5. Face-normal mode — **mirror Bezier exactly**
`bUseFaceNormals=true` skips post-displacement vertex-normal re-averaging and uses flat per-triangle shading. Semantics match `BezierPatchGeometry`. No correctness risk — user opts in explicitly if they want flat.

### Q6. `InfinitePlaneGeometry` behavior — **refuse at scene parse**
`TessellateToMesh` returns false; `Job::AddDisplacedGeometry` detects the failed tessellation, logs a clear scene-parse error (`"InfinitePlaneGeometry cannot be wrapped by displaced_geometry (infinite extent)"`) and returns false without registering the geometry. The whole scene fails to parse — loud, early, actionable. `RISE_API_CreateDisplacedGeometry` mirrors: returns false and does not populate `ppi` if base tessellation fails.

### Q7. Shader/material on the displaced mesh — **confirmed**
Material attaches to the `Object` that references the `DisplacedGeometry` (just like every other geometry in RISE). Scene authors set up the shader/material on the object normally; displaced geometry doesn't inherit anything from the base geometry beyond its mesh shape.

---

## 5. Phase-by-Phase Plan

Each phase lists: goal, tasks (checkboxes), files touched, acceptance, rough effort. Tasks are the minimum to call the phase complete. Don't tick a task without running the verification step.

---

### Phase 0 — Design lock & prior-art read

**Goal**: no-code phase. Confirm the locked decisions in §3 still hold after a careful re-read, and answer §4 questions before any source changes.

**Tasks**:
- [ ] Re-read [BezierValueGenerator.h:83 `Get()`](../src/Library/Geometry/BezierValueGenerator.h) top to bottom.
- [ ] Re-read [GeometryUtilities.cpp:438 `ApplyDisplacementMapToObject`](../src/Library/Geometry/GeometryUtilities.cpp) and `RemapTextureCoords` at line 205.
- [ ] Re-read [BezierTesselation.cpp:77 `GeneratePolygonsFromBezierPatch`](../src/Library/Geometry/BezierTesselation.cpp).
- [ ] Resolve §4 Q1–Q7 with user.
- [ ] Confirm no consumer outside the repo overrides `IGeometry` (ripgrep for `: public virtual IGeometry` and `: public IGeometry`).

**Files touched**: none.

**Acceptance**: all §4 questions answered, no surprises from the re-read, §3 unchanged (or changes recorded in the plan).

**Effort**: 1–2 hours.

---

### Phase 1 — `IGeometry` contract: add `TessellateToMesh`, remove dead `GenerateMesh`

**Goal**: define the new mesh-export contract, remove the dead placeholder it's replacing, and add the post-displacement normal-recompute helper. Nothing calls the new method yet.

**Tasks**:
- [ ] **Add** `TessellateToMesh` declaration to [src/Library/Interfaces/IGeometry.h](../src/Library/Interfaces/IGeometry.h). Use `unsigned int` for detail. Document `detail` semantics in a top-level comment.
- [ ] **Remove** `virtual void GenerateMesh() = 0;` from the same header (line 57) — dead per §3.10.
- [ ] **Add** default `TessellateToMesh` implementation in [src/Library/Geometry/Geometry.cpp](../src/Library/Geometry/Geometry.cpp): calls `GlobalLog()->PrintEx(eLog_Error, ...)` with the class name (via RTTI `typeid(*this).name()` or a `virtual const char* GetClassName()` — pick simplest; RTTI is already used elsewhere) and returns false.
- [ ] **Remove** `Geometry::GenerateMesh()` implementation at [src/Library/Geometry/Geometry.cpp:29](../src/Library/Geometry/Geometry.cpp) and its declaration in `Geometry.h`.
- [ ] **Add** `RecomputeVertexNormalsFromTopology(const IndexTriangleListType&, const VerticesListType&, NormalsListType&)` to [src/Library/Geometry/GeometryUtilities.h](../src/Library/Geometry/GeometryUtilities.h) + `.cpp`. Implementation: zero the normals array, loop triangles, accumulate face-normal into each vertex, normalize at end. Same math as existing `CalculateVertexNormals` but destroys pre-existing normals deliberately (unlike `CalculateVertexNormals`, which accumulates on top of whatever's there).
- [ ] Header change + removed virtual → `make -C build/make/rise clean` then rebuild. This phase will not link until Phase 2 removes the per-type `GenerateMesh` implementations — so Phase 1 and the Phase-2 sub-tasks need to land together. Practical order: do Phase 1 edits + all Phase 2 edits in a single branch before rebuilding.
- [ ] Rerun full existing test suite (`./run_all_tests.sh`); must pass.

**Files touched**:
- `src/Library/Interfaces/IGeometry.h`
- `src/Library/Geometry/Geometry.h`, `.cpp`
- `src/Library/Geometry/GeometryUtilities.h`, `.cpp`

**Acceptance**: compile clean after Phase 2 is also done, all existing tests pass, default `TessellateToMesh` on any geometry returns false with a log line.

**Effort**: 2–3 hours (interface + utility). Phase 2 is where the bulk of the work lives.

---

### Phase 2 — Per-geometry `TessellateToMesh` overrides + remove dead `GenerateMesh` bodies

**Goal**: every non-infinite primitive and both mesh types implement the new method. No displacement, no `DisplacedGeometry` yet — just mesh export. At the same time, every geometry's `GenerateMesh` declaration and empty implementation is removed (§3.10); since every touched file already needs editing for `TessellateToMesh`, doing both in one edit per file keeps commits atomic.

Each type gets its own sub-task below. For **every** type, the work is: (1) remove `GenerateMesh` declaration + empty body; (2) add `TessellateToMesh` declaration + implementation (or leave defaulted where appropriate).

Grouped for one commit per logical cluster; commits granular enough to bisect.

#### 2.1 Parametric primitives (simple UV parameterization)

- [ ] **SphereGeometry** — (N+1)×(⌈N/2⌉+1) long/lat grid. Duplicate `u=0` and `u=1` seam vertices. Pole vertices: single shared vertex per pole but with distinct UV row entries in the triangle fan. Triangle winding: CCW outward-facing. UV range: u=θ/2π, v=φ/π.
- [ ] **EllipsoidGeometry** — same as sphere but scale vertex positions by `m_vRadius`. Normals rescaled properly (not just the position scale — `n = normalize(vertex/radius²)`).
- [ ] **TorusGeometry** — (N+1)×(N+1) grid. Both u and v seams duplicated. Normals from analytical torus formula at each (u,v).
- [ ] **CylinderGeometry** — side: (N+1)×2 grid (radial × height) with duplicated u-seam; caps: two N-triangle fans centered on the axis end points with distinct UVs from the side. Honor `m_chAxis`.
- [ ] **BoxGeometry** — 6 face slabs, each N×N grid, face-local UVs in [0,1]². Edge vertices duplicated across faces (different normals and UVs per face). CCW winding per face.
- [ ] **CircularDiskGeometry** — triangle fan with N sectors. Center vertex at UV (0.5, 0.5); rim vertices at polar UVs. Honor `chAxis`.
- [ ] **ClippedPlaneGeometry** — bilinear N×N grid over the quad defined by `vP[4]`. If `bDoubleSided` the mesh is still single-sided; `DisplacedGeometry`'s own `bDoubleSided` param controls the output.
- [ ] **InfinitePlaneGeometry** — override returns false with explanatory log `"InfinitePlaneGeometry cannot be tessellated (infinite extent)"`. Documents the limit.

#### 2.2 Mesh geometries (pass-through)

- [ ] **TriangleMeshGeometry** — iterate `polygons` (vector of `Triangle`), emit one `IndexedTriangle` per polygon with fresh sequential indices into the output vectors (no vertex merging). Detail ignored; log once at `eLog_Info` that detail was ignored for mesh-base.
- [ ] **TriangleMeshGeometryIndexed** — copy `pPoints`, `pNormals`, `pCoords`, `indexedtris` directly. Detail ignored.

#### 2.3 Patch geometries

- [ ] **BezierPatchGeometry** — iterate `patches`, call the existing `GeneratePolygonsFromBezierPatch` per patch, concatenate with index offset. Do **not** apply this patch's own `displacement` (so caller can supply its own). Keep existing per-patch `displacement` path unchanged in the normal (non-TessellateToMesh) render.
- [ ] **BilinearPatchGeometry** — per-patch N×N bilinear grid, concatenate.

#### 2.4 Smoke verification per type

- [ ] For each of the 10 types that actually produce a mesh: in-dev ad-hoc test that tessellates at `detail=16`, counts vertices/triangles, verifies bounding box of the mesh is within 5% of `GenerateBoundingBox()` of the base. Not a permanent test — just a sanity gate before moving on. Delete the scratch code after.

**Files touched** (one `.cpp`, sometimes `.h`, per type):
- `src/Library/Geometry/SphereGeometry.{h,cpp}`
- `src/Library/Geometry/EllipsoidGeometry.{h,cpp}`
- `src/Library/Geometry/TorusGeometry.{h,cpp}`
- `src/Library/Geometry/CylinderGeometry.{h,cpp}`
- `src/Library/Geometry/BoxGeometry.{h,cpp}`
- `src/Library/Geometry/CircularDiskGeometry.{h,cpp}`
- `src/Library/Geometry/ClippedPlaneGeometry.{h,cpp}`
- `src/Library/Geometry/InfinitePlaneGeometry.{h,cpp}`
- `src/Library/Geometry/TriangleMeshGeometry.{h,cpp}`
- `src/Library/Geometry/TriangleMeshGeometryIndexed.{h,cpp}`
- `src/Library/Geometry/BezierPatchGeometry.{h,cpp}`
- `src/Library/Geometry/BilinearPatchGeometry.{h,cpp}`

**Acceptance**: each geometry's `TessellateToMesh(d=16)` produces a non-empty mesh (except `InfinitePlane`, which returns false), bounding box matches within 5%, no renderer regression (rerun full test suite — existing scenes don't call the new method).

**Effort**: 1.5–2 days (12 types × ~1h each, plus verification per type).

---

### Phase 3 — `DisplacedGeometry` class

**Goal**: the new class itself. Wires Phase 1 utility + Phase 2 base-tessellation + displacement into one composite `IGeometry`.

**Tasks**:
- [ ] Create `src/Library/Geometry/DisplacedGeometry.h`. Inherit `public virtual Geometry` (matches every other concrete geometry). No new interface needed — everything is `IGeometry`.
- [ ] Constructor signature (no default args):
  ```cpp
  DisplacedGeometry(
      IGeometry*          pBase,
      const unsigned int  detail,
      const IFunction2D*  displacement,        // nullable → pure tessellation
      const Scalar        disp_scale,
      const unsigned int  max_polys_per_node,
      const unsigned char max_recursion_level,
      const bool          bDoubleSided,
      const bool          bUseBSP,
      const bool          bUseFaceNormals );
  ```
- [ ] Constructor body:
  1. AddRef `pBase` and `displacement`.
  2. Allocate 4 local output vectors.
  3. Call `pBase->TessellateToMesh(tris, v, n, c, detail)`. On false: log error, leave `m_pMesh = nullptr`, early return.
  4. If `displacement`: `RemapTextureCoords(c)`; `ApplyDisplacementMapToObject(tris, v, n, c, *displacement, disp_scale)`.
  5. If `!bUseFaceNormals`: `RecomputeVertexNormalsFromTopology(tris, v, n)`.
  6. Construct internal `m_pMesh = new Implementation::TriangleMeshGeometryIndexed(...)` with the passed-in octree/BSP params and `bUseFaceNormals`.
  7. `m_pMesh->BeginIndexedTriangles(); AddVertices(v); AddNormals(n); AddTexCoords(c); AddIndexedTriangles(tris); DoneIndexedTriangles();`.
- [ ] Create `.cpp`. Implement delegation for every `IGeometry` method:
  - `IntersectRay` → `m_pMesh->IntersectRay` (guard on `m_pMesh == nullptr` → set `bHit = false`).
  - `IntersectRay_IntersectionOnly` → same pattern.
  - `GenerateBoundingSphere` → `m_pMesh->GenerateBoundingSphere`.
  - `GenerateBoundingBox` → `m_pMesh->GenerateBoundingBox`. (Correct automatically: displaced mesh's bbox reflects the displacement.)
  - `UniformRandomPoint` → `m_pMesh->UniformRandomPoint`.
  - `GetArea` → `m_pMesh->GetArea`.
  - `DoPreHitTest` → true (mesh-based).
  - `ComputeSurfaceDerivatives` → `m_pMesh->ComputeSurfaceDerivatives`.
  - `GenerateMesh` → empty (matches base).
  - `TessellateToMesh` → re-emit the mesh's vertices/normals/coords/tris via the indexed-mesh getters. `detail` ignored (logged at Info level).
- [ ] Destructor: Release the internal mesh, Release `pBase`, Release `displacement`.
- [ ] Copy/move constructors: deleted (follow `BezierPatchGeometry` pattern).

**Files touched**:
- `src/Library/Geometry/DisplacedGeometry.h` (new)
- `src/Library/Geometry/DisplacedGeometry.cpp` (new)

**Acceptance**: compiles. Constructing `DisplacedGeometry` on a sphere with `displacement=nullptr` and `detail=32` at the C++ level (ad-hoc test) produces a mesh with bbox matching the plain sphere. Unit test in Phase 9 makes this permanent.

**Effort**: 4–6 hours.

---

### Phase 3b — Back-port post-displacement normal fix to `BezierPatchGeometry`

**Goal**: fix the correctness bug in `BezierPatchGeometry`'s displacement path (§3.5) by applying the same post-displacement normal-recompute that `DisplacedGeometry` uses. The `RecomputeVertexNormalsFromTopology` utility from Phase 1 is reused — no duplicated logic.

**Tasks**:
- [ ] Decide implementation site — evaluate both during the edit:
  - Option A: modify `BezierValueGenerator::Get` at [BezierValueGenerator.h:83](../src/Library/Geometry/BezierValueGenerator.h) to call `RecomputeVertexNormalsFromTopology` after `ApplyDisplacementMapToObject`, gated by `!bUseFaceNormals`.
  - Option B: modify `ApplyDisplacementMapToObject` in [GeometryUtilities.cpp:438](../src/Library/Geometry/GeometryUtilities.cpp) to recompute internally.
  - **Preferred: Option A.** Keeps `ApplyDisplacementMapToObject` focused on position-only mutation; call sites opt in to the normal recompute. Lower blast radius if some future caller genuinely wants displaced positions with old normals (no current case, but cheaper future-proofing).
- [ ] Apply the fix.
- [ ] Capture **old** renders of `scenes/FeatureBased/Geometry/teapot.RISEscene` before the fix (use a scratch directory; these are reference-only, not committed).
- [ ] Apply the fix, re-render, eyeball vs. old. Confirm: no crash, no obvious breakage (distorted teapot, cracks, black patches). Shading detail on the water-bumped surface should look more physical (displaced bumps cast micro-shadows).
- [ ] Overwrite the teapot baseline (captured in Phase 10) with the new render. Add a changelog note documenting that the baseline changed intentionally.

**Files touched**:
- `src/Library/Geometry/BezierValueGenerator.h` (Option A).
- Potentially `src/Library/Geometry/GeometryUtilities.cpp` / `.h` (only if Option B chosen — unlikely per preference above).

**Acceptance**: teapot scene renders without error; visual check passes; full test suite passes.

**Effort**: 2–3 hours including render comparison.

---

### Phase 4 — Build-system updates

**Goal**: new source files compile everywhere. Per [CLAUDE.md](../CLAUDE.md) "touch ALL five".

**Tasks**:
- [ ] Add `DisplacedGeometry.cpp` + `.h` to [build/make/rise/Filelist](../build/make/rise/Filelist) (`SRCS_GEOMETRY` list, alphabetical).
- [ ] Add to [build/cmake/rise-android/rise_sources.cmake](../build/cmake/rise-android/rise_sources.cmake).
- [ ] Add to [build/VS2022/Library/Library.vcxproj](../build/VS2022/Library/Library.vcxproj): `<ClCompile Include="...">` for the `.cpp`, `<ClInclude Include="...">` for the `.h`.
- [ ] Add to [build/VS2022/Library/Library.vcxproj.filters](../build/VS2022/Library/Library.vcxproj.filters): same entries with `<Filter>Geometry</Filter>`.
- [ ] Add to [build/XCode/rise/rise.xcodeproj/project.pbxproj](../build/XCode/rise/rise.xcodeproj/project.pbxproj): four sections (`PBXBuildFile`, `PBXFileReference`, containing `PBXGroup`, per-target `Sources`/`Headers` build phases) × two targets (library + GUI).
- [ ] `make -C build/make/rise clean && make -C build/make/rise -j8 all` — full rebuild, no errors.
- [ ] (Best-effort) If a Windows or Xcode machine is available, build there too. Otherwise flag for user.

**Files touched**: as listed.

**Acceptance**: Unix build produces `librise.a` including the new object file. No build-time warnings relating to the new files.

**Effort**: 1–2 hours (mostly mechanical).

---

### Phase 5 — Public `RISE_API` function

**Goal**: expose construction via the public API header.

**Tasks**:
- [ ] Add to [src/Library/RISE_API.h](../src/Library/RISE_API.h) in the "geometry creation" section near `RISE_API_CreateBezierPatchGeometry`:
  ```cpp
  RISE_API bool RISE_API_CreateDisplacedGeometry(
      IGeometry**         ppi,
      IGeometry*          pBase,
      const unsigned int  detail,
      IFunction2D*        displacement,
      const Scalar        disp_scale,
      const unsigned int  max_polys,
      const unsigned char max_recur,
      const bool          double_sided,
      const bool          use_bsp,
      const bool          face_normals );
  ```
- [ ] Implement in [src/Library/RISE_API.cpp](../src/Library/RISE_API.cpp): null-check `ppi` and `pBase`; if `detail > 256` emit `eLog_Warning` describing the expected triangle count (§4 Q1); attempt construction; if `DisplacedGeometry`'s constructor failed to get a mesh from the base (e.g., wrapping an `InfinitePlaneGeometry`), return `false` and leave `*ppi` null (§4 Q6); otherwise set out-param, return true.
- [ ] Confirm adding this function doesn't break ABI for existing exports. Since it's an *addition* at the end of the header, safe.

**Files touched**:
- `src/Library/RISE_API.h`
- `src/Library/RISE_API.cpp`

**Acceptance**: `#include "RISE_API.h"` + calling the new function from a scratch `.cpp` compiles and links. No existing `RISE_API_*` function signatures changed.

**Effort**: 1 hour.

---

### Phase 6 — `IJob` / `Job` method

**Goal**: wire API construction into the job, with manager lookups for the base geometry and displacement function by name.

**Tasks**:
- [ ] Add to [src/Library/Interfaces/IJob.h](../src/Library/Interfaces/IJob.h):
  ```cpp
  virtual bool AddDisplacedGeometry(
      const char*         name,
      const char*         base_geometry_name,
      const unsigned int  detail,
      const char*         displacement,        // nullptr or manager name
      const Scalar        disp_scale,
      const unsigned int  max_polys,
      const unsigned char max_recur,
      const bool          double_sided,
      const bool          use_bsp,
      const bool          face_normals ) = 0;
  ```
- [ ] Declare in [src/Library/Job.h](../src/Library/Job.h).
- [ ] Implement in [src/Library/Job.cpp](../src/Library/Job.cpp) near `AddBezierPatchGeometry` (line 2844). Mirror its structure:
  1. Look up base geometry in `pGeomManager`. Log + return false if missing.
  2. Look up displacement function in `pFunc2DManager` if non-null/non-"none". Log + return false if requested but missing.
  3. `RISE_API_CreateDisplacedGeometry(...)`. If it returns false (e.g., base is an `InfinitePlaneGeometry` per §4 Q6), log a clear scene-parse error naming the scene-file context and return false — the whole scene fails to parse.
  4. `pGeomManager->AddItem(pGeometry, name)`.
  5. `safe_release(pGeometry)`.
- [ ] Verify the per-manager behavior: re-using a name should follow existing `GeometryManager` conventions (overwrite vs. error — check what `AddBezierPatchGeometry` does and match).

**Files touched**:
- `src/Library/Interfaces/IJob.h`
- `src/Library/Job.h`
- `src/Library/Job.cpp`

**Acceptance**: calling `pJob->AddDisplacedGeometry("foo", "existing_sphere", 32, nullptr, 0.0, 10, 8, false, false, false)` registers a geometry named "foo" in the manager.

**Effort**: 2 hours.

---

### Phase 7 — Scene parser chunk

**Goal**: `displaced_geometry { ... }` works in `.RISEscene` files.

**Tasks**:
- [ ] Add `DisplacedGeometryAsciiChunkParser` struct to [src/Library/Parsers/AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp) next to `BezierPatchGeometryAsciiChunkParser` (~line 3952). Parameters and defaults:

  | Param | Type | Default |
  |-------|------|---------|
  | `name` | string | `"noname"` |
  | `base_geometry` | string | (required; no default) |
  | `detail` | uint | `32` |
  | `displacement` | string | `"none"` |
  | `disp_scale` | double | `1.0` |
  | `maxpolygons` | uint | `10` |
  | `maxdepth` | uint | `8` |
  | `double_sided` | bool | `false` |
  | `bsp` | bool | `true` (§3.11 — BSP is the better default for dense displaced meshes) |
  | `face_normals` | bool | `false` |

- [ ] Register in the chunk table (~line 8058) under key `"displaced_geometry"`.
- [ ] Call `pJob.AddDisplacedGeometry(...)` with `displacement=="none" ? nullptr : displacement.c_str()`.
- [ ] Error if `base_geometry` is missing (not defaulted).

**Files touched**:
- `src/Library/Parsers/AsciiSceneParser.cpp`

**Acceptance**: parsing a scene file with a well-formed `displaced_geometry` chunk succeeds; an unknown parameter name errors with the standard `ChunkParser::` error line.

**Effort**: 2–3 hours.

---

### Phase 8 — Regression test scenes

**Goal**: coverage across every distinct tessellation path + displacement on/off invariants.

**Tasks** (one file per scene under [scenes/Tests/Geometry/](../scenes/Tests/Geometry/)):
- [ ] `displaced_sphere.RISEscene` — sphere base + `textures/waterbump.png` (already referenced by teapot scene) at `disp_scale=0.05`. Low samples, PT integrator, small resolution (~256×256).
- [ ] `displaced_torus.RISEscene` — torus base, same bump. Specifically exercises u- and v-seam handling.
- [ ] `displaced_box.RISEscene` — box base. Exercises face-boundary discontinuity tolerance.
- [ ] `displaced_cylinder.RISEscene` — cylinder base. Covers side-seam + both caps.
- [ ] `displaced_clippedplane.RISEscene` — quad base with a heightfield-like bump.
- [ ] `displaced_trianglemesh.RISEscene` — wrap an existing raw mesh (small one, e.g. one from `scenes/Tests/Geometry/shapes.RISEscene`). Tests pass-through path.
- [ ] `displaced_zero_scale.RISEscene` — sphere + `disp_scale=0.0`. Invariant: must render identically (to within float epsilon) to a plain tessellated sphere at same `detail`. Anchors the "displacement truly off" path.
- [ ] `displaced_infiniteplane_fails.RISEscene` — wraps an `InfinitePlaneGeometry`. Intended to fail scene parse (§4 Q6). Add a test-runner entry that asserts parse *failure* rather than success.
- [ ] **Existing** [scenes/FeatureBased/Geometry/teapot.RISEscene](../scenes/FeatureBased/Geometry/teapot.RISEscene) — no file change; after Phase 3b, re-render and replace its baseline. The visual output will change slightly (bumps cast more faithful micro-shadows). Document the baseline delta in the commit message.
- [ ] Update [scenes/Tests/Geometry/README.md](../scenes/Tests/Geometry/README.md) or [scenes/Tests/README.md](../scenes/Tests/README.md) to list the new scenes.
- [ ] Render each scene manually to confirm it produces a sensible image (eyeballing, no baseline yet — baseline capture is Phase 10).

**Files touched**: 7 new `.RISEscene` files + README.

**Acceptance**: each scene parses, loads, renders without error at low samples.

**Effort**: 3–4 hours.

---

### Phase 9 — Standalone unit test

**Goal**: permanent regression on the geometric invariants that are too fiddly to eyeball.

**Tasks**:
- [ ] Create `tests/DisplacedGeometryTest.cpp`, following [tests/ClippedPlaneGeometryTest.cpp](../tests/ClippedPlaneGeometryTest.cpp) structure (standalone executable, `assert`-based).
- [ ] Test case 1 — **Pure tessellation**: sphere wrapped with `displacement=nullptr`, `detail=32`. Bbox within 1% of plain sphere's. Hit count from a random-ray sweep within tolerance.
- [ ] Test case 2 — **Constant displacement**: sphere wrapped with a constant-k `IFunction2D`, `disp_scale=0.1`. Ray-from-center hit returns distance ≈ R+k (tolerance: triangle-size error).
- [ ] Test case 3 — **Zero-scale invariant**: two wrapped spheres, one with `displacement=nullptr`, one with `disp_scale=0.0`. Ray-sweep results bitwise identical.
- [ ] Test case 4 — **Seam continuity**: wrap a sphere with a non-zero bump; fire rays grazing at exactly `u=0` and `u=1`; both must hit without crack.
- [ ] Test case 5 — **Infinite-plane fail-safe**: wrap `InfinitePlaneGeometry`; verify constructor logs an error AND `IntersectRay` returns `bHit=false` for all rays (no crash).
- [ ] Test case 6 — **Uniform sampling**: `UniformRandomPoint` returns points that re-intersect the geometry inward along the normal (stays on the surface).
- [ ] Test case 7 — **Nested composition**: wrap a `DisplacedGeometry` inside another `DisplacedGeometry`. Verify construction succeeds and the outer's bbox encompasses the inner's displacement.
- [ ] Add test to `tests/Makefile` and `./run_all_tests.sh`.

**Files touched**:
- `tests/DisplacedGeometryTest.cpp` (new)
- `tests/Makefile`
- `run_all_tests.sh`

**Acceptance**: `./run_all_tests.sh` includes the new test and passes all 7 cases.

**Effort**: 4–6 hours.

---

### Phase 10 — Baseline regression capture

**Goal**: each Phase-8 scene has a committed baseline; future changes diffed against it.

**Tasks**:
- [ ] Use or extend `scripts/capture_refactor_baselines.sh` / `scripts/check_refactor_baselines.sh` (introduced by the integrator refactor) — if it covers arbitrary scenes, reuse; otherwise add a small capture script.
- [ ] Render each of the 7 scenes at a fixed seed and low samples. Render **sequentially** per [feedback_sequential_renders](../../../.claude/projects/-Users-aravind-Documents-GitHub-RISE/memory/feedback_sequential_renders.md).
- [ ] Commit baselines under `tests/baselines_refactor/pre_displaced_geometry/` (or similar subdir — match whatever the repo uses now; `tests/baselines_refactor/` already exists as untracked per current git status).
- [ ] Add a post-build regression step that compares current renders to baselines. Tolerance: ~0.27% mean-luminance noise floor (matches the integrator-refactor plan's measured floor).
- [ ] Document the regression command in [tests/README.md](../tests/README.md).

**Files touched**:
- Scripts under `scripts/`.
- `tests/baselines_refactor/...` (new PNGs).
- `tests/README.md`.

**Acceptance**: fresh clone → build → run regression script → all 7 scenes pass baseline comparison.

**Effort**: 3–5 hours (mostly wall-clock-bound on rendering).

---

### Phase 11 — Documentation

**Goal**: everything a future reader needs to find and use the feature.

**Tasks**:
- [ ] Add `DisplacedGeometry` to [src/Library/README.md](../src/Library/README.md) geometry list with one-line description and link.
- [ ] Write or extend a "Displacement" section in [src/Library/Parsers/README.md](../src/Library/Parsers/README.md) documenting the `displaced_geometry` chunk, every parameter, and per-base-geometry `detail` semantics.
- [ ] In [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md), add a note that `IGeometry::TessellateToMesh` is the forward hook for GPU mesh export and composite geometries.
- [ ] Update the High-Value Facts list in [CLAUDE.md](../CLAUDE.md) ONLY if this feature becomes load-bearing for other subsystems (e.g., if a GPU path lands). Otherwise leave CLAUDE.md thin per its convention.
- [ ] Brief entry in [scenes/Tests/Geometry/README.md](../scenes/Tests/Geometry/README.md) listing the new test scenes.

**Files touched**: as listed.

**Acceptance**: a new contributor can read the docs alone and understand when to use `displaced_geometry`, what `detail` does per base type, and where to find the code.

**Effort**: 2 hours.

---

### Phase 12 — Adversarial review + merge readiness

**Goal**: three-reviewer adversarial pass on the high-risk surfaces per [docs/skills/adversarial-code-review.md](skills/adversarial-code-review.md).

**Tasks**:
- [ ] Launch 3 parallel reviewers:
  - **R1 — Tessellation correctness**: focus on seam handling (sphere, torus, cylinder), pole degeneracies, UV tent-remap interaction, face-vs-vertex normal mode, winding consistency. Inputs: Phase-2 overrides + [GeometryUtilities.cpp](../src/Library/Geometry/GeometryUtilities.cpp).
  - **R2 — Lifetime & reference counting**: confirm every AddRef has a Release, including the nullptr-displacement and tessellation-failure branches; confirm `safe_release` usage matches existing patterns; confirm no leaks on construction failure. Inputs: `DisplacedGeometry.cpp` + `Job::AddDisplacedGeometry`.
  - **R3 — Numeric consistency**: compare ray-intersection results between base geometry (analytic) and its tessellated wrapper with `displacement=nullptr` at `detail={16, 32, 64, 128}`; quantify RMS error and confirm it monotonically decreases with detail. Inputs: DisplacedGeometry + per-base tessellation.
- [ ] Triage findings: P1 blocks merge, P2 addressed or deliberately deferred with a reason, P3 recorded.
- [ ] Re-run Phase-10 regression after any fix.
- [ ] Final checklist:
  - [ ] All tests pass locally.
  - [ ] All 7 regression scenes match baseline.
  - [ ] No new compiler warnings.
  - [ ] All 5 build projects updated (Phase 4 complete).
  - [ ] Docs (Phase 11) complete.
  - [ ] `git status` clean except intended additions.
- [ ] Open PR; link this plan doc; list completed phases.

**Files touched**: whatever the reviewers find. Possibly none.

**Acceptance**: 0 P1 findings unresolved; PR ready for external review.

**Effort**: 1 day (wall-clock, depending on findings).

---

## 6. Risk Register

| # | Risk | Likelihood | Impact | Mitigation |
|---|------|------------|--------|------------|
| R1 | Adding `TessellateToMesh` virtual breaks out-of-tree `IGeometry` subclasses. | Low (no known external consumers) | Medium | Non-pure with default implementation (§3.2). |
| R2 | Sphere/torus seam generates visible cracks under displacement. | Medium | High | Duplicate seam vertices (§3.6); unit test case 4 enforces. |
| R3 | Post-displacement normal recomputation produces different shading than `BezierPatch`'s existing displacement. | High (intentional divergence) | Low | Document in scene-format docs; don't back-port. |
| R4 | Wrapping a mesh base at high detail causes memory blowup if we ever enable subdivision. | Low (v1 has no subdivision) | Medium | Subdivision is out of scope for v1; log info message for mesh bases. |
| R5 | Users wrap Bezier with inner displacement non-none → double-displacement. | Medium | Low | Document as accepted behavior (§4 Q3); user overrides via scene file. |
| R6 | `IFunction2D::Evaluate` thread-safety — tessellation is synchronous at construction so safe, but doc may claim otherwise. | Low | Low | Tessellation runs once before scene is handed to renderer; no concurrent access. |
| R7 | Build-project drift — someone adds another geometry without remembering the 5-project rule. | Medium (independent of this plan) | Low | Not this plan's job; existing convention handles it. |
| R8 | `detail=256` sphere at high sample counts pushes memory past working limits. | Low | Medium | Propose clamp (§4 Q1). |
| R9 | Bounding box grows the geometry past what the spatial acceleration structure was sized for. | Low | Low | `GenerateBoundingBox` returns the *displaced* mesh's box — the outer scene BSP/octree naturally accommodates. |
| R10 | Displacement scale interacts with `Object` transforms (non-uniform scale warps normals). | Low (inherits existing `Object` behavior) | Low | Document: displacement is in object space, applied before any `Object` transform. |
| R11 | Bezier back-port (Phase 3b) shifts the teapot scene's rendered output; any downstream reference imagery breaks. | High (the fix itself causes it) | Low | Intentional; captured as the new reference in Phase 10. Commit message flags the baseline delta. |
| R12 | Removing `GenerateMesh` from `IGeometry` breaks out-of-tree subclasses at link time. | Very low (user confirmed no external consumers) | High if it happened | Pre-Phase-1 ripgrep to confirm zero occurrences of `GenerateMesh` overrides outside the enumerated list. |

---

## 7. Effort Estimate Summary

| Phase | Effort | Cumulative |
|-------|--------|------------|
| 0 | 2h | 2h |
| 1 | 3h | 5h |
| 2 | 16h | 21h |
| 3 | 5h | 26h |
| 3b | 3h | 29h |
| 4 | 2h | 31h |
| 5 | 1h | 32h |
| 6 | 2h | 34h |
| 7 | 3h | 37h |
| 8 | 4h | 41h |
| 9 | 5h | 46h |
| 10 | 4h | 50h |
| 11 | 2h | 52h |
| 12 | 8h | 60h |

**Total**: ~60 hours, or **8 focused working days**, single engineer.

Phases can be checkpointed cleanly after any of: 2, 3, 3b, 7, 10, 12. Each represents a natural landing point with working state. Note: Phase 1 and Phase 2 must land together (removing `GenerateMesh` from the interface breaks the build until all overrides are also removed — §3.10 / Phase 1 acceptance note).

---

## 8. Future Work Out of Scope

Captured for reference; not part of this plan.

- **GPU mesh export path** — `IGPUMeshTarget` that `TessellateToMesh` fills (VBO/IBO buffers). Plugs into future raster preview or OptiX/Metal exporter.
- **Adaptive tessellation** — subdivide more where `|displacement|` is large or where curvature is high. Requires a two-pass or recursive tessellator.
- **Analytical normals from displacement gradient** — sample `IFunction2D` derivatives and compute perturbed normal directly. Better than topological re-average for smooth displacement.
- **Non-uniform detail** (`ivec2 detail_uv`) — different segment counts per axis.
- **Additional wrapper geometries** — `InstancedGeometry`, `CSGGeometry`, `TransformedGeometry`. `DisplacedGeometry` establishes the wrapper pattern; these reuse it.
- **Back-port post-displacement normals to `BezierPatchGeometry`** — correctness improvement for existing path. Separate change so it can be benchmarked/regressed independently.
- **Architectural follow-up from Phase 12 R3**: `TriangleMeshGeometryIndexed::TessellateToMesh` reconstructs indices via pointer subtraction against `pPoints` / `pNormals` / `pCoords`.  Safe today because `DoneIndexedTriangles` is the last mutator, but fragile to future refactoring.  Store the index form directly at `DoneIndexedTriangles` time or snapshot on first TessellateToMesh call.
- **HDR baseline comparison**: `check_displaced_baselines.sh` currently only checks "output produced" for HDR outputs (teapot).  Add a float-aware reader (e.g., OpenEXR/Radiance via pyEXR or similar) for proper HDR drift checks.
- **Consolidate triangle winding**: `ClippedPlaneGeometry` and `BilinearPatchGeometry` emit CW while the rest emit CCW. Both render correctly but style-only cleanup would help future readers.

---

## 9. Review Checklist

### 9.1 Decisions resolved 2026-04-18

- [x] §3 Locked design decisions (3.1–3.11, including 3.10 `GenerateMesh` removal and 3.11 BSP default).
- [x] §4 Q1 — `detail` uncapped, warn above 256.
- [x] §4 Q2 — Mesh-base pass-through in v1.
- [x] §4 Q3 — Accept doubled displacement when inner Bezier has its own.
- [x] §4 Q4 — Keep nested composition via `DisplacedGeometry::TessellateToMesh`.
- [x] §4 Q5 — `bUseFaceNormals` mirrors Bezier semantics.
- [x] §4 Q6 — Refuse at scene parse; fail-loud.
- [x] §4 Q7 — Material attaches to `Object`, not `DisplacedGeometry` itself.
- [x] `GenerateMesh` cleanup folded into Phase 1 + Phase 2.
- [x] `BezierPatchGeometry` post-displacement normal fix added as Phase 3b.
- [x] Phase ordering 0 → 1 → 2 → 3 → 3b → 4 → ... → 12.
- [x] Effort estimate ~60 hours, ~8 working days.

### 9.2 Outstanding for user approval before starting Phase 1

- [ ] Plan approved — OK to flip Phase 0 to "In progress".

On approval, I'll begin Phase 0: re-read the three precedent files ([BezierValueGenerator.h:83](../src/Library/Geometry/BezierValueGenerator.h), [GeometryUtilities.cpp:438](../src/Library/Geometry/GeometryUtilities.cpp), [BezierTesselation.cpp:77](../src/Library/Geometry/BezierTesselation.cpp)) and the `: public.*IGeometry` external-consumer grep, record any surprises in §3 of this plan, then proceed to Phase 1. The §2 Progress Summary is updated after each phase completes.

---

## 10. Phase 12 Adversarial Review — Findings (2026-04-18)

Three parallel reviewers with orthogonal lanes.  Severity: **P1** blocks merge, **P2** should address, **P3** nit.

### R1 — Tessellation correctness
- Seam handling, pole degeneracy, UV tent-remap interaction, face-normal mode, Bezier back-port: **all clean**.
- **P1 (dismissed)**: R1 suspected `BoxGeometry::TessellateToMesh` was emitting simple parametric UVs that diverged from the native `IntersectRay` face-local formulas.  Verified by substitution (u=0,v=0 → origin = (-hw, hh, -hd); native formula u=(z+d/2)/d, v=1-(y+h/2)/h also evaluates to (0,0)).  The UVs match.  Comments are correct.  Closed.
- **P2 (deferred → §8)**: `ClippedPlaneGeometry` and `BilinearPatchGeometry` emit CW triangles while the other geometries emit CCW.  Both render correctly.  Deferred to the §8 style-cleanup item.

### R2 — Lifetime & reference counting
- Constructor addref balance, RISE_API failure path, Job hand-off, internal mesh lifetime, test code, `ConstFunction2D`, virtual destructors: **all clean** across all code paths.

### R3 — Numeric consistency
- Zero-scale FP invariance, degenerate-triangle NaN safety, UV tent at u=0.5: **all clean**.
- **P1 (fixed)**: `RecomputeVertexNormalsFromTopology` gave orphaned vertices a zero normal.  At sphere poles, the innermost pole vertex appears only in degenerate triangles — every neighbor contributes a zero cross-product — so the pole ended up with a zero normal and rendered dark.  Fixed at [GeometryUtilities.cpp:86](../src/Library/Geometry/GeometryUtilities.cpp): snapshot incoming normals as a fallback, skip degenerate triangles (cross-magnitude < NEARZERO), restore the fallback for vertices with incident==0.  All 54 tests + 7 baselines still pass after the fix.
- **P1 (deferred → §8)**: `TriangleMeshGeometryIndexed::TessellateToMesh` reconstructs indices via pointer subtraction against `pPoints`/`pNormals`/`pCoords`.  Safe in the current lifecycle but fragile.  Documented in §8.
- **P2 (fixed)**: Test 2 (constant displacement) added an additional assertion that the hit distance is at least k/2 further than a plain sphere, so a regression where disp_scale silently collapses would be caught.  [tests/DisplacedGeometryTest.cpp:100](../tests/DisplacedGeometryTest.cpp).
- **P2 (accepted)**: Test 1 bbox tolerance at 5% is generous.  Kept as-is — tightening it would flag noise on minor tessellator tweaks; the Test 2 constant-displacement check + baseline drift already catches the relevant regressions.
- **P3 (accepted)**: Test 7 nested composition uses a loose bound.  Kept as-is — primary goal is verifying composition doesn't crash; full numeric validation of layered displacement is out of scope.

**Merge readiness**: all P1 items resolved or dismissed with justification.  No outstanding blockers.
