# DL-622: Walk-Carried Medium Membership and Open-Sheet Seeding Architecture

**Status:** Implemented & Verified (design & code review rounds to 0 P1 on `debt-dl622`)  
**Ledger item:** `DL-622` (`docs/DEBT_LEDGER.md` line 384)  
**Primary files:**
- `src/Library/Utilities/IORStack.h`
- `src/Library/Utilities/IORStackSeeding.h`
- `src/Library/Utilities/ManifoldSolver.cpp`
- `src/Library/Geometry/ClippedPlaneGeometry.cpp`
- `src/Library/Materials/CompositeSPF.cpp`
- `src/Library/Shaders/PathTracingIntegrator.cpp`
- `src/Library/Rendering/PixelBasedSpectralIntegratingRasterizer.cpp`
- `tests/OpenSheetHalfSpaceSeedingTest.cpp`

---

## 1. Problem Statement

`DL-618` and `DL-619` introduced the sheet-normal half-space seeding convention for certified open refractive sheets (`RayIntersectionGeometric::bProvablyNoInterior`). Under that convention, `DL-622` recorded seven remaining gaps and a unifying root cause:

1. **Gap (1) — Submerged opaque body inside a sheet's half-space:** A seed point `pos` in free space (or on a separate luminaire) directly beneath an opaque body `B` (e.g., a rock or opaque canopy submerged under a water sheet `S`) is not seeded into `S`'s medium because the straight ray from `pos` along `S`'s normal hits `B`, even though a free-space walk around `B` carries `S`'s index to `pos` without crossing any opaque boundary.
2. **Gap (2) — Folded/overhanging sheets and points outside the sheet's normal prism:**
   - A sheet with an S-fold or overhanging wave crest (where the normal ray crosses `back -> front -> back` with net back-face parity $+1$) or a curved/folded multi-panel sheet (where points under a steep flank or fold lie inside the flank's local normal prism but miss the single global fitted-plane normal ray) is declined by `OpenSheetHoldsPoint` / `OpenSheetNormalRayReaches`.
   - Along a path walk that has already entered (or been seeded into) an open sheet's medium and then moves outside the sheet's normal prism (or under an opaque overhang):
     1. Mid-walk companion-lane delegation (`SeedCompanionLane` with `tryOrigin == false`) reconstructs membership from scratch at the segment midpoint and loses the walk-carried sheet entry.
     2. Unpushed-exit resolution (`ResolveUnpushedExit` / `OpenSheetEnclosesCrossing`) at a crossing of a *different-material* open sheet `pSheet` falls back to `PointInOpenSheetMedium(pY, pObjects, pAfter)` and pops the walk-carried sheet `pY` if `pAfter` has drifted outside `pY`'s normal prism while still in `pY`'s back half-space.
3. **Gap (3) — Hard cap of 8 open sheets (`kMaxSeededOpenSheets = 8`):** Confirming more than 8 open sheets at `pos` (or passing more than 8 sheets along a normal ray) hits a fixed stack-array limit in `SeedFromPoint` and declines *all* open sheets (`A14`).
4. **Gap (4) — Geometric own-body guess when `pSeedObject == nullptr`:** Callers that do not pass `pSeedObject` run a backward-ray heuristic (`Ray(pos, -normal)`) in `OpenSheetNormalRayReaches` that guesses `pos` lies on the surface of a closed opaque object if its opposite wall is within `selfTol` (`A22` residual).
5. **Gap (5) — Linear/quadratic cost growth with sheet count ($+15\%$ at 50 panes):** `SeedFromPoint` loops over all catalogued sheets on every call, running `PosedOpenSheet` virtual queries, individual object intersections (`alone`), and a separate full-scene ray walk (`OpenSheetNormalRayReaches`) per candidate sheet, while spectral camera rays repeat `SeedFromPoint` 4 times per pixel sample (once per HWSS wavelength).
6. **Gap (6) — Sheets missed by the catalog's 15 axis probe rays:** `DescribeOpenSheet` probes an object's world AABB with only 15 axis-aligned rays (`3 axes x 5 offsets`). An annular sheet with a central hole, a thin diagonal strip, or a perimeter/corner patch missed by all 15 rays is never listed in `OpenSheetCatalog`.
7. **Gap (7) — `RefreshLegacySeedFallbackNM` keeps the hero index off-axis:** When re-evaluating a specular chain's starting `IORStack` at companion wavelength `nm`, `RefreshLegacySeedFallbackNM` shoots 6 principal-axis rays from `start` to find a `RayIntersectionGeometric` for each object on `starting`. Any stack entry not hit by the 6 axis rays keeps its hero-wavelength index.

---

## 2. Design Overview

The design resolves all seven items through two complementary layers:

1. **Layer 1 — Walk-Carried Medium Membership & Surface-Hit Provenance on `IORStack` (`IORStack::EntryContext`):**
   Every medium entry pushed onto `IORStack` (at `SeedFromPoint`, `ResolveOpenSheetCrossing`, `NewTransmittedStack`, `TranslucentSPF`, `CompositeSPF::ToExternal`/`BelowExternalStack`, `PathTransportUtilities::MediaAgreeWithRay`, `RayCaster::WalkShadowSegment`, `ManifoldSolver::SnellContinueChain`, and `SMSDomainReplay::BuildStack`/`Cross`) records a compact `EntryContext` capturing the `const IMaterial*` and surface-intersection payload (`ptIntersection`, `ptObjIntersec`, `ptCoord`, `vNormal`, `vGeomNormal`, `rayDir`, `rast`, and geometric flags). Downstream consumers that already hold a walk's `IORStack` (`RefreshLegacySeedFallbackNM`, `SeedCompanionLane`, spectral camera-ray companion lanes, `SMSDomainReplay::Capture`, and `ResolveUnpushedExit` / `OpenSheetEnclosesCrossing`) preserve and re-evaluate indices directly from the walk-carried `IORStack` instead of discarding walk state and re-probing from a point.
2. **Layer 2 — Exact Catalog Discovery, Shared Normal-Group Seeding, Multi-Normal/Parity Fold Handling, and Constructive Detour Certificates in `IORStackSeeding`:**
   Where a path origin genuinely starts at a point with no prior walk (`SeedFromPoint` at a camera or light root), the seeding convention is generalized so that:
   - Every certified open sheet is discovered via deterministic surface sampling (`IObject::UniformRandomPoint` + perimeter-inclusive grid fallback) when the 15 axis probes miss (**Gap 6**).
   - Sheets sharing a normal direction are grouped in `OpenSheetCatalog` with 2D projected AABBs (`OpenSheetNormalGroup`) and confirmed by **one shared scene ray walk** per normal group (`ConfirmSharedNormalRay`) (**Gap 5**).
   - Confirmed sheets use dynamic storage with no 8-sheet cap and dynamic scene-walk step budget `MaxSceneWalkSteps` (**Gap 3**).
   - The `pSeedObject == nullptr` backward-ray own-body exit exemption is removed (**Gap 4**).
   - Non-planar (folded, overhanging, curved) sheets use signed multi-crossing parity $\Delta(S) = n_{\text{back}} - n_{\text{front}} > 0$ and facet-normal-verified local auxiliary normals `auxNormals` (**Gap 2**).
   - Non-enclosing opaque obstacles (both closed solids met from outside and open/uncertified opaque overhangs) inside a sheet's half-space are bypassed when a constructive 2-segment free-space detour (`pos -> waypoint -> sheet` back face) proves that a walk around the obstacle reaches the sheet's back face without crossing any opaque surface, while blocking self-reentry (`A18`) and closed rooms (`A1`, `B2`, `B4`, `A21`–`A26`) (**Gap 1**).

---

## 3. Detailed Design

### 3.1 Layer 1: `IORStack::EntryContext` and Walk-Carried Membership (Solves Gap 7 and Mid-Walk Gap 2)

#### 3.1.1 Data Structure in `src/Library/Utilities/IORStack.h`
We add `EntryContext` to `IORStack` and embed it in `IORStack::IORDATA`:

```cpp
struct EntryContext
{
    const IMaterial*    pMaterial;
    Point3              ptIntersection;
    Point3              ptObjIntersec;
    Point2              ptCoord;
    Vector3             vNormal;
    Vector3             vGeomNormal;
    Vector3             rayDir;
    RasterizerState     rast;
    bool                bHasContext;
    bool                bRepushParentIor;
    bool                bGeomNormalOrientedToRay;
    bool                bGeomNormalRayDerived;
    bool                bProvablyNoInterior;
    bool                bOpenSheet;

    static EntryContext FromGeometric(
        const RayIntersectionGeometric& rig,
        const IMaterial* pMat,
        const bool bRepushParent = false
        );

    void PopulateGeometric(
        RayIntersectionGeometric& rig
        ) const;
};
```

In `IORStack`:
- `push( const Scalar ior )` and `pushOpenSheet( const Scalar ior )` gain overloads `push( const Scalar ior, const EntryContext& ctx )` and `pushOpenSheet( const Scalar ior, const EntryContext& ctx )`.
- `IORStack` exposes:
  - `const EntryContext& ContextAt( const std::size_t i ) const`
  - `void SetContextAt( const std::size_t i, const EntryContext& ctx )`
  - `const EntryContext& TopContext() const`
  - `const EntryContext* FindEntryContext( const IObject* pObj ) const`
  - `void SetIORAt( const std::size_t i, const Scalar ior )`
- `SameInterfaces( const IORStack& other )` continues to compare `(pObj, ior)` (ignoring `EntryContext`) so `dispersiveContainment = !laneStack.SameInterfaces(iorStack)` in `PathTracingIntegrator.cpp` and `PixelBasedSpectralIntegratingRasterizer.cpp` detects wavelength-dependent IOR differences on re-indexed companion stacks.

#### 3.1.2 Recording `EntryContext` Along the Walk
1. **`SeedFromPoint` (`IORStackSeeding.h`):**
   - `ProbeEntry` stores `IORStack::EntryContext entryCtx` (first exit hit) and `firstEntryCtx` (first entry hit for inward-wound closed meshes normalized by `SignedDistanceLower`).
   - `ConfirmedSheet` stores `IORStack::EntryContext entryCtx`, captured at the confirming back-face hit on `pSheet`.
   - When `SeedFromPoint` pushes open sheets and closed objects onto `stack`, it passes their `entryCtx`.
2. **`ResolveOpenSheetCrossing` (`IORStackSeeding.h`):**
   - Accepts an optional `const IMaterial* pMat = 0`. When `bFront` is true and `dest.pushOpenSheet( sheetIOR, ... )` is called, `resolvedMat` is resolved via `pMat ? pMat : ((pScene && IsObjectInManager(pScene->GetObjects(), dest.currentObject())) ? dest.currentObject()->GetMaterial() : 0)` (never dereferencing synthetic keys such as `CompositeSPF::BottomKey`) and passed to `IORStack::EntryContext::FromGeometric( ri, resolvedMat, false )`.
3. **`NewTransmittedStack` (`IORStackSeeding.h`) and SPF / walk callers:**
   - `NewTransmittedStack` accepts optional `const RayIntersectionGeometric* pRi = 0` and `const IMaterial* pMat = 0`. When `!pOpenSheet && bEntering && pRi`, it resolves `resolvedMat = pMat ? pMat : ((pScene && IsObjectInManager(pScene->GetObjects(), p->currentObject())) ? p->currentObject()->GetMaterial() : 0)` and attaches `IORStack::EntryContext::FromGeometric( *pRi, resolvedMat, false )`.
   - `PerfectRefractorSPF` and `DielectricSPF` pass `&ri` to `NewTransmittedStack`; `CompositeSPF::ToExternal` (`src/Library/Materials/CompositeSPF.cpp`) attaches `internalSt.TopContext()` and `CompositeSPF::BelowExternalStack` attaches `IORStack::EntryContext::FromGeometric( ri, 0, false )`.
   - `TranslucentSPF` (`Scatter` / `ScatterNM` in `src/Library/Materials/TranslucentSPF.cpp`) attaches `IORStack::EntryContext::FromGeometric( ri, 0, true )` when repushing the parent IOR (`hasInterior`).
   - `PathTransportUtilities::MediaAgreeWithRay`, `RayCaster::WalkShadowSegment`, and `ManifoldSolver::SnellContinueChain` attach `IORStack::EntryContext::FromGeometric(...)` when pushing onto `IORStack`.
   - `SMSDomainReplay::BuildStack` attaches `IORStack::EntryContext::FromGeometric( entry.context, entry.material, false )` when pushing entries onto `replay`, and `SMSDomainReplay::Cross` attaches `IORStack::EntryContext::FromGeometric( hit, &material, false )`.
   - `SMSCaptureUncached` (`ManifoldSolver.cpp`): for an open-sheet key (`keyIsOpenSheet`), if `±sheet.normal`, the 6 principal axes (`directions`), and `toCenter` all miss `key`, `SMSCaptureUncached` checks `live.FindEntryContext( key )` (`pCtx->PopulateGeometric( rig )`) and then `IORStackSeeding::ProbeObjectSurfaceHit( key, anchor, hit )`.

#### 3.1.3 `ReindexStackForWavelength` and `SeedCompanionLane` (`IORStackSeeding.h`)
We add `ReindexStackForWavelength`:
```cpp
inline bool ReindexStackForWavelength(
    const IORStack& source,
    IORStack& dest,
    const Scalar nm,
    const Point3* pEvalPoint = 0,
    const IScene* pScene = 0
    )
```
- Copies `dest = source`, resolves `pProbeScene = pScene ? pScene : dest.MediumProbeScene()` (setting `dest.SetMediumProbeScene( pProbeScene )` when non-null) and `pObjects = pProbeScene ? pProbeScene->GetObjects() : 0`, and initializes `queryStack( dest.EnvironmentIOR() )` with `queryStack.SetMediumProbeScene( pProbeScene )`.
- For `i = 1 .. depth - 1`:
  - Retrieves `const IObject* pObj = dest.ObjectAt( i )`, checks `const bool inScene = pObjects && IsObjectInManager( pObjects, pObj );`, and reads `const IORStack::EntryContext& ctx = dest.ContextAt( i )`.
  - If `ctx.bHasContext`:
    - If `ctx.bRepushParentIor`: `newIdx = queryStack.top()`.
    - Else: resolves `pMat = ctx.pMaterial ? ctx.pMaterial : (inScene ? pObj->GetMaterial() : 0)` (falling back to a short ray along `ctx.rayDir` or `ProbeObjectSurfaceHit( pObj, ctx.ptIntersection, hitRi )` when `!pMat && inScene`), populates `rig` via `ctx.PopulateGeometric( rig )`, updates `rig.ptIntersection` and `rig.ptObjIntersec` when `pEvalPoint != 0`, and evaluates `pMat->GetSpecularInfoNM( rig, queryStack, nm )` (or `GetSpecularInfo` when `nm <= 0`).
  - Else (when `!ctx.bHasContext`): requires `inScene`, probes `ProbeObjectSurfaceHit( pObj, evalPos, hitRi )` or constructs a synthetic hit at `evalPos`, and evaluates `pMat->GetSpecularInfoNM( hitRi.geometric, queryStack, nm )`.
  - Calls `dest.SetIORAt( i, newIdx )` and pushes `(pObj, newIdx, dest.isOpenSheetAt(i), ctx)` onto `queryStack`.
- Calls `GradedIndexMedium::AdoptTrackedTop( dest, source )` so any tracked graded medium on `source` is preserved.

In `SeedCompanionLane`:
- First calls `if( ReindexStackForWavelength( hero, lane, nm, 0, &scene ) ) { lane.SetMediumProbeScene( &scene ); return; }` (0 scene ray walks).
- Only if `hero` lacks context/material on some entry does it run the geometric `SeedFromPoint` fallback.

In `PathTracingIntegrator.cpp` and `PixelBasedSpectralIntegratingRasterizer.cpp`:
- After seeding the hero camera stack at `ray.origin`, companion lanes call `IORStackSeeding::ReindexStackForWavelength( iorStack, laneStack, swl.lambda[w] )` in 0 scene ray walks instead of repeating `SeedFromPoint` 3 more times per pixel sample.

#### 3.1.4 Preserving Walk-Carried Sheet Membership in `ResolveUnpushedExit` / `OpenSheetEnclosesCrossing` (`IORStackSeeding.h`)
In `ResolveUnpushedExit(const RayIntersectionGeometric& ri, const IObject* pSheet, IORStack& dest)` and `OpenSheetEnclosesCrossing`:
1. `OpenSheetEnclosesCrossing` accepts `const IORStack::EntryContext* pWalkEntryCtx = 0` (passed by `ResolveUnpushedExit` via `&dest.TopContext()`).
2. In `OpenSheetEnclosesCrossing`:
   - First, run the forward sibling search along `dir` and the same-material slab-exit check `if (pSheet && pMat && pSheet->GetMaterial() == pMat) return false;`.
   - Second, check `if (PointInOpenSheetMedium(pY, pObjects, pAfter)) return true;`.
   - Third (**Walk-Carried Extra-Prism Continuation**):
     When `pY` is a walk-carried open sheet (`pSheet != pY` and `pSheet->GetMaterial() != pMat`), if `PointInOpenSheetMedium` returned `false`:
     - Look up `OpenSheetCatalogEntry sheetY` via `FindOpenSheet(pObjects, pY, sheetY)`.
     - Check whether `pAfter` is still inside `pY`'s primary or auxiliary normal prism (`CheckSheetAloneAlongRay(pY, pAfter, sheetY.normal, sheetY.isPlanar)` or along `sheetY.auxNormals[a]` with `auxCos >= kAuxNormalAlignCos`). If so, `PointInOpenSheetMedium` already tested the normal ray (and any detour) and found it blocked, so return `false`.
     - When `pAfter` is genuinely outside `pY`'s normal prism, test up to 2 candidate anchors (`anchors[0..1]`: `pWalkEntryCtx->ptIntersection` with unflipped geometric normal when `pWalkEntryCtx && pWalkEntryCtx->bHasContext`, and fallback `sheetY.p0` with `sheetY.normal`).
     - For each candidate anchor where `pAfter` is on the back side of `anchorNormal`, walk `Ray(pAfter, dirToAnchor)` toward `anchorPt` up to `MaxSceneWalkSteps(pObjects)`. If that ray reaches `pY`'s back face without being blocked by `kMembershipBlock` or `kMembershipSheetFront` (breaking to the next candidate anchor if blocked), return `true` so `ResolveUnpushedExit` keeps `pY` on `dest`.

#### 3.1.5 Closing Gap (7) and Unpushed Open-Sheet Exits in `RefreshLegacySeedFallbackNM` (`ManifoldSolver.cpp`)
In `RefreshLegacySeedFallbackNM(chain, nm, start, scene, starting)` (`src/Library/Utilities/ManifoldSolver.cpp`):
1. **Re-indexing every starting stack entry (`e = 1 .. replay.Depth() - 1`):**
   - **Primary path (walk-carried `EntryContext`):**
     If `entryCtx.bHasContext && entryCtx.pMaterial`:
     Populates `atReceiver` via `entryCtx.PopulateGeometric( atReceiver )`, updates `atReceiver.ptIntersection = start` and, if `inScene`, `atReceiver.ptObjIntersec = Point3Ops::Transform( obj->GetFinalInverseTransformMatrix(), start )`, and queries `entryCtx.pMaterial->GetSpecularInfoNM( atReceiver, query, nm )`.
   - **Fallback path for synthetic test stacks without `EntryContext`:**
     If `index <= 0` and `inScene`:
     Tries the 6 principal-axis rays `kAxes[0..5]` from `start`; if still not hit, calls `IORStackSeeding::ProbeObjectSurfaceHit( *obj, start, hit )`; if still not hit, queries `obj->GetMaterial()->GetSpecularInfoNM( atReceiver, query, nm )` directly at `start`.
2. **Consistent Unpushed Open-Sheet Exit Handling along the Chain:**
   In the chain walk loop of `RefreshLegacySeedFallbackNM`, when `v.isExiting` is true and `!replay.containsCurrent()` on an open sheet (`v.isOpenSheet`), constructs `RayIntersectionGeometric` along `v.position - prevPos` at `v.position` and calls `IORStackSeeding::ResolveUnpushedExit( rig, v.pObject, replay )`.

---

### 3.2 Layer 2: Closing the Six Open-Sheet Catalog and Seeding Gaps

#### 3.2.1 Gap (6) — Deterministic Surface-Point Discovery in `DescribeOpenSheet` (`IORStackSeeding.h`)
1. **Stage 1 (Fast 15-Ray Axis Probe):** Keeps the 15-ray axis probe (`3 axes x 5 offsets`).
2. **Stage 2 (`recordFallbackHit` when Stage 1 Misses):**
   If Stage 1 finds `!anyHit`:
   - When `object.GetGeometry() != 0`, loops `s = 1 .. kSurfaceSamples` (`kSurfaceSamples = 32`) with deterministic 3D Halton points `(HaltonRadicalInverse(s, 2), HaltonRadicalInverse(s, 3), HaltonRadicalInverse(s, 5))` via `object.UniformRandomPoint( &ptSample, &nrmSample, 0, prand )`, fires a ray along `d = Vector3Ops::Normalize( -nrmSample )` through `ptSample` first, and falls back to `+X, +Y, +Z` if that ray misses.
   - If `hits == 0` (e.g., `object.GetGeometry() == 0` or `UniformRandomPoint` yielded no hit), runs an $11 \times 11$ multi-axis grid probe spanning $[-0.45, +0.45]^2$ across all 3 axes.
3. **Stage 3 & Stage 4 (Plane Fit Supplementation & Facet Normal Discovery):**
   Supplements the $9 \times 9$ least-squares plane fit with `sampledPts` when the $9 \times 9$ grid collects fewer than 3 hits, and samples 16 Halton surface points (`kFacetSamples = 16`) to discover auxiliary facet normals.
   Also exposes `ProbeObjectSurfaceHit( const IObject& object, const Point3& anchor, RayIntersection& outRi )` (and pointer overload) for `SMSDomainReplay::Capture`, `RefreshLegacySeedFallbackNM`, and `ReindexStackForWavelength`.

---

#### 3.2.2 Gap (2) — Folded, Overhanging, and Curved Open Sheets (`IORStackSeeding.h`)
1. **Planarity Classification & Multi-Crossing Signed Parity (`CheckSheetAloneAlongRay`):**
   - `OpenSheetCatalogEntry` records `bool isPlanar` and `std::vector<Vector3> auxNormals` (up to `kMaxAuxNormals = 8`, clustered inline when `Vector3Ops::Dot(nm, bestNormal) < Scalar(0.99)` and `Vector3Ops::Dot(nm, out.auxNormals[cIdx]) < Scalar(0.98)`).
   - `CheckSheetAloneAlongRay( const IObject* pSheet, const Point3& pos, const Vector3& dir, const bool isPlanar = false, Scalar* outHitCos = 0, int* outTotalCrossings = 0 )` checks that the first hit on `pSheet` is a back face (`TrueGeomFacing(dir) > 0`) and, when `!isPlanar`, walks all crossings of `pSheet` along `Ray(pos, dir)` and requires net signed parity `netBackMinusFront > 0`.
   - In `OpenSheetNormalRayReaches`, `expectedSheetCrossings` stops the ray march as soon as all `expectedSheetCrossings` of `pSheet` have been seen, while any other sheet's front face (`kMembershipSheetFront` with `ri.pObject != pSheet`) blocks immediately.
2. **Facet-Normal-Verified Auxiliary Local Normals (`auxNormals`):**
   - In `OpenSheetHoldsPoint`, if `sheet.normal` misses or is blocked, each `nj = sheet.auxNormals[j]` is tested via `CheckSheetAloneAlongRay( sheet.pObj, pos, nj, false, &hitCos, &expectedCrossings )` with `hitCos >= kAuxNormalAlignCos` (`0.95`), preventing cross-facet oblique false positives.

---

#### 3.2.3 Gap (1) — Constructive Free-Space Detour Certificate Around Submerged Opaque Bodies (`HasFreeSpaceDetourToSheet` in `IORStackSeeding.h`)
When `OpenSheetNormalRayReaches` (`allowDetour == true`) encounters an opaque obstacle `B = ri.pObject` (`kMembershipBlock`) at $t > \text{selfTol}$ that is not `pSeedObject`:
1. **Immediate Rejection Conditions:**
   - If `B` is a non-open surface (`!ri.geometric.bProvablyNoInterior && !ri.geometric.bOpenSheet`) and `ri.geometric.TrueGeomFacing(normal) > 0` (an EXIT face of a closed solid), `pos` is inside `B`: return `false` immediately.
   - If `pSeedObject == nullptr`, fires a backward ray `Ray(Point3Ops::mkPoint3(pos, normal * selfTol), -normal)` against `B` alone over max distance `2 * selfTol`; if it hits `B` (`A18` torus hole self-reentry), returns `false` immediately.
2. **Vertical Scan to `pSheet` Along `normal` (`HasFreeSpaceDetourToSheet`):**
   Continues walking `vertProbe` along `normal` up to `pSheet` (`vertSheetCrossings >= expectedSheetCrossings`, `vertSheetParity > 0`), recording `vertPassed` (sheets passed before/between obstacles), `vertCrossed` (closed transparent objects crossed), unioning all encountered obstacles into `obsBox`, and tracking `OpaqueParity` in `opaqueObjs` so every closed solid returns to parity `0` before `pSheet` (and rejecting any other sheet's front face `kMembershipSheetFront`).
3. **Constructive 2-Segment Waypoint Certificate:**
   Projects `obsBox` onto `(U, V)` orthogonal to `normal` with margin `delta = std::max(Scalar(1e-3), Scalar(0.05) * span)` and constructs up to 8 lateral waypoints `waypoints[0..7]` (`wm`) around `[uMin - delta, uMax + delta] x [vMin - delta, vMax + delta]`:
   - **Segment 1 (`pos -> wm`):** Walks `Ray(pos, segDir)` up to `segLen`, rejecting `wm` on any `kMembershipBlock` or `kMembershipSheetFront`, while recording `segPassedSheets` and `segCrossedClosed`.
   - **Segment 2 (`wm -> pSheet` along `normal`):** Verifies `CheckSheetAloneAlongRay` at `wm` and calls `OpenSheetNormalRayReaches(..., allowDetour = false, countProbeTrace = false)`.
   - When a waypoint succeeds, merges `segPassedSheets`/`wPassed` into `passedSheets` and `vertCrossed`/`wCrossed` into `crossedClosed`, and returns `true`.

---

#### 3.2.4 Gap (3) — Lifting the 8-Sheet Cap and Dynamic Ray-Walk Step Budget (`IORStackSeeding.h`)
- Replaces the fixed 8-sheet arrays in `ConfirmedSheet` (`std::vector<const IObject*> passed`, `std::vector<const IObject*> crossed`) and `SeedFromPointImpl` (`std::vector<ConfirmedSheet> confirmed`, `std::vector<const ConfirmedSheet*> sheetOrder`) with dynamic `std::vector` containers.
- Defines `MaxSceneWalkSteps(const IObjectManager* pObjects)` as `kMaxProbeSteps + static_cast<int>(2 * nSheets)` and uses it in `OpenSheetNormalRayReaches`, `ConfirmSharedNormalRay`, `HasFreeSpaceDetourToSheet`, `OpenSheetEnclosesCrossing`, and `TallyProbe`.

---

#### 3.2.5 Gap (4) — Removing the Geometric Own-Body Guess (`IORStackSeeding.h`)
- In `SeedFromPointImpl`, removes the `!pSeedObject` `ownBodyFromGeometry` heuristic (`OpenSheetNormalRayReaches` only exempts `pSeedObject` when `pSeedObject != 0`).

---

#### 3.2.6 Gap (5) — Normal-Clustered Catalog & Shared Ray Walk (`OpenSheetNormalGroup` & `ConfirmSharedNormalRay` in `IORStackSeeding.h`)
- `OpenSheetsOf` groups planar co-normal sheets (`Dot >= 1.0 - 1e-10`) into `OpenSheetNormalGroup` (`normal`, `planeU`, `planeV`, `origin`, `sheetIndices`, `heights`, `uMin`, `uMax`, `vMin`, `vMax`, `maxHeight`).
- `ClippedPlaneGeometry::RegenerateData()` (`src/Library/Geometry/ClippedPlaneGeometry.cpp`) calls `GeometricUtilities::BumpPatchCertificateEpoch()` and `IORStackSeeding::InvalidateOpenSheetCatalogs()`.
- In `SeedFromPointImpl`, each `OpenSheetNormalGroup` first culls against `maxHeight` and `[uMin, uMax] x [vMin, vMax]`; when 2 or more group sheets pass `CheckSheetAloneAlongRay` with unchanged poses (`SamePose` and `SameBox`), `ConfirmSharedNormalRay` confirms all of them in **1 shared scene ray walk** (falling back to per-sheet `OpenSheetNormalRayReaches` if an obstacle requires a per-sheet detour).

---

## 4. Verification & Red-Green Test Plan

1. **Existing Gates (0 Regressions):**
   - `OpenSheetHalfSpaceSeedingTest`: **121 passed, 0 failed** (`111/0` Part A unit + `10/0` Part B rendered), including `A14` updated to assert that 9 nested sheets seed all 9 entries (`Depth() == 10`) in exact outer-to-inner order `s9..s1`, and `A22` asserting that a flush emitter on a closed room's floor returns `Depth() == 1` both without and with `pSeedObject` while a separate open luminaire (`sdflamp`) outside the room under `pond` returns `Depth() == 2` when `pSeedObject` is passed.
   - `SMSDomainReplayTest` (**10285 passed, 0 failed**), `ExteriorIndexInvarianceTest` (**319 passed, 0 failed**), `TranslucentInitialContainmentTest` (**43/0**), `TranslucentDoubleSidedTest` (**53/0**), `IORStackSeedingRegressionTest` (**15/0**), `GradedIndexInteriorFactorTest` (**PASS, 0 failures**), `SSSBoundaryFurnaceTest` (**560/0**).

2. **New Tests in `tests/OpenSheetHalfSpaceSeedingTest.cpp` (`A27`–`A33`):**
   - **`A27` (Gap 1 + Walk-Carried Extra-Prism Continuation):**
     Seeds a point beneath a submerged closed rock (`rock`), beneath a submerged open opaque canopy (`canopy`), and beneath a narrower inner sheet (`inner`) + two vertically stacked submerged rocks (`rock1`, `rock2`) under `sea` (`Depth() == 3`, `1.33` then `1.5`), and verifies `ResolveOpenSheetCrossing` preserves `sea` across an unrelated sheet exit (`pane`) outside `sea`'s normal prism.
   - **`A28` (Gap 2 — Non-planar folded & overhanging sheets):**
     Seeds `(-2.8, 2.5, 0)` under the horizontal lip of an L-folded overhanging sheet (`g_lfold`) outside the global normal prism and `(-1.5, 1.5, 0)` in the corner while declining `(1.0, 2.5, 0)` outside both facets' normal prisms, seeds under a 3-pleat S-folded accordion (`g_sacc`, 3 crossings `back -> front -> back`, net parity $+1$), and declines inside a 2-pleat C-folded air tube (`g_ctube`, `front -> back`, net parity $0$).
   - **`A29` (Walk-carried companion lane outside normal prism):**
     Verifies `SeedCompanionLane` re-indexes a dispersive sheet outside its normal prism in **0** probe traces (`DebugGetProbeTraceCount() == 0`).
   - **`A30` (Gap 3 — 12 nested open sheets):**
     Verifies 12 nested open sheets (`s12`, alternating `glass133`/`glass15`/`glass20`) seed all 12 entries (`Depth() == 13`) in exact outer-to-inner order `n12..n1`.
   - **`A31` (Gap 6 — Hollow picture-frame sheet missed by all 15 Stage-1 rays):**
     Verifies a hollow picture-frame mesh (`g_frame` in `[-4,4]^2 \setminus [-3.6,3.6]^2`) missed by all 15 Stage-1 axis probes is catalogued by Stage 2 and seeds behind its border (`(3.8, 0, 0)`, `Depth() == 2`) while not seeding in its central hole (`(0, 0, 0)`, `Depth() == 1`).
   - **`A32` (Gap 5 — 50 stacked glass panes):**
     Verifies 50 parallel glass panes (`s50`) seed all 50 entries (`Depth() == 51`) in **2** probe traces (`1` closed-parity `+Z` walk + `1` shared `ConfirmSharedNormalRay` walk).
   - **`A33` (Gap 7 — `RefreshLegacySeedFallbackNM` off-axis dispersive re-indexing & unpushed open-sheet exit):**
     Verifies `BuildSnellBaseSeed` (`RefreshLegacySeedFallbackNM`) re-indexes dispersive off-axis `card_off` at `(5, 5, 5)` across `400 nm` and `700 nm` (both with a bare synthetic stack `manualStart` and with walk-carried `EntryContext` on `ctxStart`) and resolves unpushed open-sheet exits through `slab_top`.

