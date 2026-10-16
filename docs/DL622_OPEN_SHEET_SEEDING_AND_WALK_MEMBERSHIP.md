# DL-622: Walk-Carried Medium Membership and Open-Sheet Seeding Architecture

**Status:** Implemented; revised by the 2026-10-16 review round (section 5 -- read it first: it supersedes parts of sections 2 and 3)  
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
   *(Superseded in part by section 5: contexts are recorded for OPEN-SHEET entries only and stored out of line; the walk-history exit rule of 3.1.4 was removed.)*
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
*(Section 5.1: `EntryContext` is no longer embedded in `IORDATA`; it lives in a parallel vector that is empty unless an entry carries one.)*
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
*(Section 5.1: only the open-sheet pushes below still record a context -- `SeedFromPoint`'s confirmed sheets, `ResolveOpenSheetCrossing`, and the SMS walks' sheet pushes (`SnellContinueChain`, `SMSDomainReplay::BuildStack`).  `ProbeEntry`, `NewTransmittedStack`, the SPFs, `CompositeSPF`, `TranslucentSPF`, `MediaAgreeWithRay`, `RayCaster::WalkShadowSegment` and `SMSDomainReplay::Cross` are back to plain pushes.)*
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
*(Section 5.3: `SeedCompanionLane` re-indexes only a hero stack holding an open-sheet entry; the camera paths are back to per-lane `SeedFromPoint`; `ReindexStackForWavelength` now takes a probe anchor and keeps graded entries at the walk's tracked value.)*
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
*(REMOVED by the review round, section 5.2: the third stage made exit membership depend on the walk's history and split PT from BDPT.)*
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
*(Section 5.4: Segment 1 now blocks on every surface with a side, and all detour rays of one seed share a 256-ray budget.)*
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
- ~~`ClippedPlaneGeometry::RegenerateData()` calls `GeometricUtilities::BumpPatchCertificateEpoch()` and `IORStackSeeding::InvalidateOpenSheetCatalogs()`.~~ Reverted (section 5.5).
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

---

## 5. Review round (2026-10-16): cost contract, crossing-only exits, bounded detours

The first landing (commit `8ab4124e7`) slowed scenes with NO open sheet by
9-13 % user CPU (supervisor-measured, n = 3: `cornellbox_bdpt_materials_pt`
+13 %, `csg_comprehensive` +10 %, `hwss_prism_dispersion_pt` +9 %), made an
unpushed exit's membership depend on the walk's history, and let the detour
certificate cross glass sideways and grow as O(N^2) scene rays.  This round
keeps every DL-622 gap fix that has a red-proof and changes the following.

### 5.1 `EntryContext` cost contract

- `IORStack::IORDATA` is back to the pre-DL-622 `{pObj, ior, bOpenSheet}`
  record (24 bytes, trivially copyable).  Contexts live in
  `MyIORStack::ctxs`, a SPARSE COPY-ON-WRITE list: (stack index, context)
  pairs for the entries that carry one, in an immutable list shared by every
  copy through an intrusive atomic count (round 3: a `std::shared_ptr`
  member made the inlined copy/destroy bodies non-trivial, and the head
  profile showed `ScatteredRay::~ScatteredRay` and `find_and_destroy` as new
  out-of-line frames on a scene with no open sheet; now a null pointer is
  the whole no-context cost, and every count, allocation and re-index sits
  in `RISE_IORSTACK_COLD` (noinline) helpers).  Copying a stack copies the
  pointer -- nothing more when it is null, one atomic increment otherwise
  -- never the ~200-byte contexts (review round 2: the round-1 pointer to a
  parallel vector deep-copied every context on every copy of a stack whose
  walk had crossed a sheet).  Only a push WITH a context, an erase at or
  below a context-carrying index (`find_and_destroy` re-indexes the pairs
  above it) or `SetContextAt` builds a new list.  `std::stack` is now a
  PROTECTED base of `MyIORStack`, so no caller can push or erase around
  `pushEntry` / `find_and_destroy` and desynchronise the indices.  A42 pins
  copy independence and re-indexing (green before and after; red under a
  mutation that writes through the shared list).
- Only OPEN-SHEET pushes record a context: `SeedFromPoint`'s confirmed
  sheets, `ResolveOpenSheetCrossing` (no `IsObjectInManager` lookup per push:
  a null material is resolved by the consumer, which checks the key first),
  `ManifoldSolver::SnellContinueChain` and `SMSDomainReplay::BuildStack` on a
  sheet.  The closed-medium recordings in `DielectricSPF`,
  `PerfectRefractorSPF`, `TranslucentSPF`, `CompositeSPF`,
  `PathTransportUtilities::MediaAgreeWithRay`, `RayCaster::WalkShadowSegment`,
  `SMSDomainReplay::Cross`, `NewTransmittedStack` and
  `RefreshLegacySeedFallbackNM`'s chain walk are reverted;
  `RefreshLegacySeedFallbackNM`'s local replay stack also pushes its
  open-sheet entries WITHOUT a context (nothing reads it; the commit message
  of `38ab3355c` overstated this as "SMS walks' sheet pushes").
- `ProbeEntry` is back to its pre-DL-622 layout (no contexts), so
  `SeedFromPointImpl`'s six probe tables are the pre-DL-622 ~6 KB of stack
  with no constructor calls (the first landing put ~68 KB and 384 context
  constructions on EVERY seed -- every PT camera sample and BDPT root -- and
  ate into DL-315's stack-guard headroom).  The tables stay on the stack
  while `MaxSceneWalkSteps` fits `kStackProbeEntries` = 64 (up to 16 open
  sheets in the scene; ~12 KB) and are heap-sized to the step budget
  otherwise -- review round 2: with a 32-entry threshold ANY scene with one
  sheet took the heap path, seven vector allocations per seed; `TallyProbe` takes the table capacity and never
  walks more steps than it holds, so a met object is never silently dropped
  (A39: a seed inside a tank behind 33 beads, with 20 far sheets raising the
  budget to 72 steps, was left in air by the 32-entry cap).

### 5.2 Unpushed exits are resolved by the crossing alone

`OpenSheetEnclosesCrossing`'s third stage (keep a walk-carried sheet when the
point past the crossing is outside its normal prism but can still see the
walk's entry point or the sheet's `p0`) is removed, with its
`pWalkEntryCtx` parameter.  The stage made the index at a point depend on
where the walk entered the sheet: a PT eye walk that entered the sea above
the camera kept the sea past an unrelated pane outside the sea's prism,
while a BDPT light walk seeded there (not in the sea, by the convention) did
not, and the two integrators priced the same crossing with different
indices.  B5 (camera in a sea's prism viewing a receiver outside it, lit
only through a pane by an emitter in air): PT/BDPT **0.7905 +/- 0.0046** at
`8ab4124e7`, **0.9969 +/- 0.0103** after (n = 4 salted renders per
integrator, 16x base spp; the parent `b1bedbf90` reads the same 0.9969).
A27's off-prism check asserts agreement with a seed past the crossing from
two different walk histories (red at `8ab4124e7`: 2 checks).

Review round 2 found that round 1's FIRST stage still disagreed with a seed:
it walked along the WALK's direction for up to 64 steps, skipping every
surface that was not pY or a same-material sibling -- opaque walls included
-- and answered "encloses" on reaching pY's back face.  It is gone too.

Review round 3 tightened the replacement: the answer is now the SEED's own
sheet confirmation at the point just past the crossing --
`ConfirmOpenSheetsAt`, the function `SeedFromPointImpl` itself calls,
run once per crossing (`CrossingMedia`, shared by every entry
`ResolveUnpushedExit` considers popping), at the walk's wavelength, with
the sheet just crossed left out.  pY stays iff that confirmation holds pY,
or holds a sheet of pY's MATERIAL (identity; review round 4 dropped an
exact index match, which a dispersive RGB SPF's per-channel exit resolution
failed against the nm-0 entry index -- A50) that the walk's stack does NOT
already carry (the seed keeps one entry for same-material siblings, so pY
standing for that medium prices the same index; round 2 also counted a
same-material sheet pY is NESTED in, which is already on the stack -- A44:
a stale inner pane kept alive by its own outer pane, so the walk read 1.5
after leaving the outer one).  The seed's declines pop pY.  Round 2's
version also ran one confirmation per popped entry and one single-sheet
certificate per same-material sheet: A45 (an exit under a rock wider than
a 50-pane stack, popping all 50) cost **68750** scene rays at `1340f1ddb`
and **52** now.  A stack key that is not (or no longer) a
certified open sheet bounds a volume and is decided as `ResolveUnpushedExit`
decides a closed entry (`ClosedObjectEnclosesCrossing`).  A41: a walk in a sea's medium leaving a pane
under an opaque roof that spans the sea's prism, going straight up at the
sea's back face, kept the sea under round 1 (walk depth 2) where a seed
there is in air (depth 1); it now leaves.  The DL-382 two-sheet slab falls
out of the convention (the near sheet's normal ray meets the far sheet's
front face): `OpenSheetIndexConventionTest`, `OpenSheetConsumerScopeTest`,
`ExteriorIndexInvarianceTest` and `SMSDomainReplayTest` are unchanged.

**Reopened residual (for the ledger):** gap (2)'s mid-walk sub-item "a walk
that moved outside a sheet's normal prism loses the sheet at an unrelated
sheet's unpushed exit" is NOT fixed.  Fixing it consistently needs the seed
convention itself to change (so that a seed and a walk agree), not a
walk-history exception.

### 5.3 Companion lanes and camera lanes

- `SeedCompanionLane` keeps the pre-DL-622 rule (seed at the segment start
  when it reproduces the hero's membership, else at the midpoint) for every
  hero stack WITHOUT an open-sheet entry, so HWSS output on closed-medium
  scenes -- uncertified meshes, translucent re-pushes, DL-370 neighbour
  crossings -- is the parent's code path (A37: a stale closed-medium hero
  stack is re-seeded, red at `8ab4124e7`).  A hero stack WITH an open-sheet
  entry is re-indexed (`StackHasOpenSheet` then
  `ReindexStackForWavelength`), falling back to the seeding if an entry
  cannot be evaluated.
- The camera paths (`PathTracingIntegrator::IntegrateRayHWSS`,
  `PixelBasedSpectralIntegratingRasterizer::TakeSingleSampleHWSS`) are back
  to per-lane `SeedFromPoint`: the hero there is itself a seed at the same
  point, so a re-index bought no membership and changed nothing but cost and
  the evaluation point of closed entries.  (This gives up the "camera lanes
  in 0 walks" part of gap 5.)  No red-proof exists for this revert: at the
  camera `8ab4124e7`'s re-index read every entry from the context its own
  seed recorded at the same probe hit a per-lane seed evaluates, so the two
  produce the same lane stacks; the revert is a cost and simplicity change
  (B7/B8 render identically at all three builds).
- Review round 3 (HWSS cost): lanes that provably share their membership
  are seeded with ONE probe walk.  The probes' paths and every membership
  decision depend on the wavelength only through each hit's classification
  (`ClassifyMembershipHit`: trackable or not, refracting or a re-push) and
  the index it reports; `SeedLanesFromPoint` classifies every hit at each
  lane's wavelength during the primary seed (`LaneSeed`, threaded through
  `TallyProbe` as a parameter -- no thread-local state) and, when every
  classification agrees, builds each lane's stack from the primary's
  membership with the index that lane's own seed would have taken at the
  same hit; otherwise (or in a scene with any open sheet, whose
  confirmation compares indices between sheets) it seeds each lane on its
  own.  A49 pins exact equality with four separate seeds (objects, order,
  per-lane indices) and the walk count: 6 vs 24 for a point inside three
  nested closed media (dispersive, translucent, constant), 1 vs 4 in air,
  and the per-lane fallback for a box whose ior reads -1 at one lane.
  `SeedCompanionLanes` (every HWSS hand-off in `PathTracingIntegrator`, via
  its `HWSSLaneSeeds`) and the PT camera's lane check use it; the legacy
  `PixelBasedSpectralIntegratingRasterizer` keeps per-lane seeds.
- `ReindexStackForWavelength( source, dest, nm, anchor, scene )`: a GRADED
  entry (`GradedIndexMedium::FieldOf`) keeps `source`'s recorded value --
  the field is wavelength-independent and that value is the one the walk
  tracked (DL-09 Advance, DL-335 `RecordEnclosingAt`); the first landing
  re-evaluated it at the recorded entry point (A36: 1.5 instead of the
  tracked 1.35, red at `8ab4124e7` and at the parent).  An entry with a
  context is evaluated at it; an entry without one at a hit from the bounded
  object-only `ProbeObjectSurfaceHit` from `anchor` (the first landing
  evaluated it at the world ORIGIN when no eval point was passed); a key
  that is not a scene object declines.  A `hasInterior`-only entry takes
  its refreshed parent's index from the query stack (DL-46).
- `RefreshLegacySeedFallbackNM`: an entry nothing resolves stands in the
  query stack as its parent's index again (the first landing pushed the
  hero-wavelength value, which a DL-46 translucent child above it would then
  re-push).  A43 red-proves it: a translucent entry above a key that is not a scene
  object re-pushed the hero 1.7 at `8ab4124e7` where the refreshed ancestor
  reads 1.8 at 400 nm.

### 5.4 Detour certificate

- Segment 1 (`pos -> waypoint`) must be FREE space: every surface with a true
  side blocks it, including a closed transparent object and another sheet's
  back face.  Along the sheet's normal those stay membership-transparent
  (the convention); sideways they separate `pos` from the waypoint.  A34: a
  camera in a sealed habitat (opaque roof and walls, one wall a closed glass
  box) under a sea was seeded 1.33 through the window at `8ab4124e7`; it is
  not now, and the same habitat with that wall removed is still seeded.
- Cost (review round 2 replaces round 1's shared ray budget, which declined
  EVERY open sheet once it ran out -- including a sheet confirmed by a clear
  normal ray -- and gave `PointInOpenSheetMedium` a fresh budget of its own,
  so a seed and an exit resolution could disagree): the certificate is a
  PURE function of (seed point, sheet), bounded by construction (one column
  walk plus at most eight waypoints, each a lateral and a normal walk of at
  most `MaxSceneWalkSteps` steps), and a co-normal group whose shared
  normal ray is blocked runs ONE certificate for all of its remaining sheets
  (`HasFreeSpaceDetourToSheets`): the single-sheet certificate's every
  decision before sheet k is a property of the prefix of one shared column
  walk, so k's verdict, obstacle box and passed/crossed lists are a snapshot
  of that walk; sheets reached with the same box share waypoints and
  lateral legs, and one normal walk per waypoint confirms each candidate at
  its back face, the first confirming waypoint winning as before.  A35: 50
  co-normal panes over a rock cost **2652** scene rays per seed at
  `8ab4124e7`, 258 under round 1 (which then declined: depth 1), and **104**
  after round 2, seeded 51 deep, every sampled pane agreeing with its own
  single-sheet certificate.  A40: a tilted sea confirmed by a clear normal
  ray over a 50-pane stack blocked by a rock is seeded (round 1 declined it),
  and seed and `PointInOpenSheetMedium` agree for the sea and the panes.
  `Diagnostics::SceneRayCounter` counts every scene ray the seeding header
  makes.  There is no exhaustion to log.
- Review round 3: a group sheet that is not trackable at the seed's
  wavelength (classified as a blocker: its ior reads <= 0 there) is an
  opaque obstacle to the sheets beyond it -- its box joins theirs -- as in
  each sheet's own certificate, instead of ending the shared column (A47,
  red at `1340f1ddb`).  And one group certificate spends at most
  `kGroupDetourRayBudget` = 1024 scene rays after its column walk (review
  round 4: the single-sheet certificate spends the same budget; bounded
  by `MaxSceneWalkSteps`): when waypoints keep failing, trying every
  obstacle box costs O(N * 8 * (32 + 2N)) rays (interleaved obstacles give
  each sheet its own box).  On exhaustion the sheets it has not confirmed
  are refused -- only this group's detour candidates, never a sheet its
  shared normal ray reached directly -- deterministically in (point, group),
  and since seeds and exit resolutions both decide through
  `ConfirmOpenSheetsAt` they refuse alike (A48, budget forced to 60: a
  partial seed, and an exit resolution at the same point keeping exactly
  the seeded panes).  A46, 20 glass shelves each carrying an opaque vase
  over the camera's column (every shelf its own box): **287** scene rays per
  seed, all 20 shelves seeded (a 4-box cap, tried first, read 109 rays but
  refused 15 shelves, so it was replaced by the ray budget).  Ungrouped
  sheets (no co-normal partner) still run one single-sheet certificate each
  (at most 17 walks of `MaxSceneWalkSteps` steps).
- Stack depth (review round 3, P3-2): `SeedFromPointImpl`'s ~12.8 KB frame
  (six 64-entry probe tables) exists only while a seed runs and is a LEAF:
  every caller (camera and light roots, photon emission, HWSS lane
  delegation, `SMSCaptureUncached`'s membership probe) gets control back
  before tracing further, so the frame can sit on top of a deep recursion
  (an SMS capture at a vertex reached through PT's SSS re-entry into
  `CastRay`) but never underneath one, and it is ~10 % of the 128 KB
  DL-315 keeps free at every `CastRay` entry.  The function that does run
  inside a walk, `ConfirmOpenSheetsAt` (via `ResolveUnpushedExit` in a
  transmissive SPF), uses no probe tables (its storage is heap vectors).
- `ProbeObjectSurfaceHit` is bounded at `kMaxSurfaceProbeRays` = 70
  object-only rays (26 directions, 1 toward the centre, 16 Halton surface
  samples, a 3 x 3 grid per axis), down from up to 422.

### 5.5 Motion blur and the catalog

`ClippedPlaneGeometry::RegenerateData` no longer calls
`InvalidateOpenSheetCatalogs()` / `BumpPatchCertificateEpoch()`.  Under
DL-465 time-indexed motion blur it runs per pixel sample on per-thread
clones, so the first landing flushed every thread's catalog every sample.
A clipped plane's re-pose is already handled where it is used:
`PosedOpenSheet` (and the normal-group path's `SamePose`/`SameBox` check)
re-describe a sheet whose transform or bounds changed, and a real edit
between passes is picked up by `PrepareForRendering`'s epoch bump.  A
clipped plane is always provably open, so its patch certificate never
changes.  A38 pins that `RegenerateData` leaves the epoch unchanged (red at
`8ab4124e7`).  Residual (pre-existing, DL-618): a re-pose that keeps the
same transform AND the same world bounds (corners permuted within the box)
is not re-described.

### 5.6 Red/green record

`OpenSheetHalfSpaceSeedingTest` Part A at `b1bedbf90` (parent) / `8ab4124e7`
/ this round, same test file (built against each with a counter-only
instrumentation patch for A35):

| Row | parent | `8ab4124e7` | now |
|---|---|---|---|
| A14 nine sheets ordered (gap 3) | FAIL | pass | pass |
| A27 rock / canopy / stacked-rock detour (gap 1), 3 checks | FAIL x3 | pass | pass |
| A27 off-prism exit agrees with a seed, 2 checks | pass | FAIL x2 | pass |
| A28 L-fold, S-fold, C-tube (gap 2), 5 checks | **pass** | pass | pass |
| A29 companion lane re-index in 0 walks | FAIL | pass | pass |
| A30 twelve sheets (gap 3) | FAIL | pass | pass |
| A31 hollow frame catalogued + seeded (gap 6), 2 checks | FAIL x2 | pass | pass |
| A32 fifty panes in 2 walks (gap 5) | FAIL | pass | pass |
| A33 `RefreshLegacySeedFallbackNM` (gap 7), 3 checks | FAIL x3 | pass | pass |
| A34 sealed habitat | pass (no detour) | FAIL | pass |
| A34 open-habitat control | FAIL (no detour) | pass | pass |
| A35 detour cost (round 2: 104 rays, 51 deep, per-pane agreement) | -- | FAIL (2652 rays) | pass (104; round 1 code: FAIL x2) |
| A35 3-pane control | FAIL | pass | pass |
| A36 graded companion lane | FAIL | FAIL | pass |
| A37 closed-medium companion lane | pass | FAIL | pass |
| A38 catalog epoch | pass | FAIL | pass |
| A39 no silent table cap | FAIL | FAIL | pass |
| A40 clear sea over a blocked pane stack (round 2), 2 checks | -- | pass | pass (round 1: FAIL x2) |
| A41 exit under a roof agrees with a seed (round 2) | -- | FAIL | pass (round 1: FAIL) |
| A42 context copy-on-write independence (round 2), 2 checks | -- | pass | pass (red under a write-through mutation) |
| A43 unresolved entry stands as its parent's index (round 2) | -- | FAIL | pass |
| A44 nested same-material sheet not double counted (round 3) | -- | -- | pass (round 2 code: FAIL) |
| A45 one exit popping 50 panes, cost (round 3) | -- | -- | pass, 52 rays (round 2 code: FAIL, 68750) |
| A46 20 shelves with vases, cost (round 3) | -- | -- | pass, 287 rays |
| A47 untrackable group sheet is an obstacle (round 3) | -- | -- | pass (round 2 code: FAIL) |
| A48 spent group budget: seed and exit refuse alike (round 3) | -- | -- | pass (new mechanism) |
| Totals (round 1 file) | 113 / 16 | 121 / 8 | 129 / 0 |
| Totals (round 2 file, Part A) | -- | 133 / 10 | 143 / 0 (round 1 code: 138 / 5) |

A28 passes at the parent: gap (2)'s fold handling has no red-proof in this
suite (the parent's single-normal rule already seeded those points).

Part B (renders, n = 4 salted repeats per estimator, 16 x 16, one worker;
`HALF_SPACE_ONLY_NEW` invocations, so the three builds draw the same salts;
a full run draws later salts for these rows -- this round's full run reads
B5 0.9971, B6 1.0011 / 0.9983, B7 0.9941, B8 0.9931, 144 passed, 0 failed):

| Row | parent | `8ab4124e7` | now |
|---|---|---|---|
| B5 off-prism exit PT / BDPT | 0.9969 +/- 0.0103 | **0.7905 +/- 0.0046** | 0.9969 +/- 0.0103 |
| B6 rock + pane PT / rock + closed box PT | **0.6364 +/- 0.0022** | 0.9968 +/- 0.0089 | 0.9968 +/- 0.0089 |
| B6 rock + pane BDPT / PT | 0.9955 +/- 0.0079 | 0.9954 +/- 0.0093 | 0.9954 +/- 0.0093 |
| B7 dispersive ball under a water sheet, HWSS / independent | 0.9944 +/- 0.0221 | 0.9944 +/- 0.0221 | 0.9944 +/- 0.0221 |
| B8 dispersive ball in a closed water box (no sheet), HWSS / independent | 1.0013 +/- 0.0079 | 1.0013 +/- 0.0079 | 1.0013 +/- 0.0079 |

B7 and B8 are coverage: all three builds produce the same means to the
printed digits.  B8 is the "HWSS output on closed-medium scenes is
unchanged" measurement at `num_wavelengths 160`; `8ab4124e7` also matched
there, so the walk-dependent closed-medium cases it changed (A37) are not
reached by that scene.

### 5.7 Cost on scenes without open sheets

Interleaved user CPU, `oidn_denoise FALSE`, n = 4 per build, the
supervisor's three scenes (none has an open sheet), master `c97e0c17e`,
parent `b1bedbf90`, `8ab4124e7`, this round:

| Scene | master | parent | `8ab4124e7` | now |
|---|---|---|---|---|
| `cornellbox_bdpt_materials_pt` | 289.5 +/- 1.7 s | 288.4 +/- 2.6 (-0.4 %) | 323.0 +/- 5.6 (+11.6 %) | 286.8 +/- 4.3 (-0.9 %) |
| `csg_comprehensive` | 32.5 +/- 0.3 s | 32.2 +/- 0.2 (-0.9 %) | 36.8 +/- 0.3 (+13.3 %) | 32.3 +/- 0.4 (-0.6 %) |
| `hwss_prism_dispersion_pt` | 230.9 +/- 1.6 s | 233.4 +/- 3.7 (+1.1 %) | 242.1 +/- 2.2 (+4.8 %) | 237.7 +/- 1.9 (+2.9 %) |

`hwss_prism_dispersion_pt` reads now-minus-parent between +0.5 % and +3.8 %
over five interleaved rounds (n = 4-5 each, run-to-run sd 1-3 %).  The
seeding itself is at parity (a micro-benchmark on that scene:
`SeedFromPoint` 665 vs 662 ns, `SeedCompanionLane` 680 vs 676 ns, parent;
`8ab4124e7` 1436 ns per seed), and a build with the context storage
compiled out entirely renders that scene in the same time as this round
(244.6 +/- 7.4 vs 244.2 +/- 7.2 s, n = 4, parent 240.3 +/- 3.2 in that
round), so no remaining DL-622 mechanism accounts for the scene's
residual; it is reported, not explained.

`ExteriorIndexInvarianceTest` (a suite FULL of open sheets, 319/0 on every
build), user CPU, n = 1 per build, each pair run concurrently: round 1 --
master 2102 s vs 2234 s (+6.3 %), parent 1942 s vs 2029 s (+4.5 %); round 2
(copy-on-write contexts, group certificate, convention exits) -- parent
1951 s vs 2005 s (+2.8 %).  A 30 s `sample` of both processes ten minutes in
shows no hotspot: the seeding header's own self time is ~1000 of ~22000 busy
samples against ~830 at the parent (`OpenSheetNormalRayReaches`,
`TallyProbe`, `PosedOpenSheet`, `CheckSheetAloneAlongRay`), and no
`IORStack` copy or `shared_ptr` frame appears.

Round 2, the three scenes again (n = 4, interleaved), against master and
the merged head `5e9ddc16e` (round 1 plus the other workers' DL-621..625
merges, which touch the render loop's RNG and dispatch): cornellbox
master 291.5 +/- 4.7, merge 297.6 +/- 2.9, now 294.6 +/- 2.4 s; csg
33.4 +/- 1.0, 32.9 +/- 0.4, 33.8 +/- 1.1 s; hwss 233.4 +/- 3.2,
241.3 +/- 2.7, 241.3 +/- 3.7 s.  Round 2 adds nothing over the merged head;
the hwss scene's +3.4 % over master is present at that head (and the parent
read +1-3 % over master on that scene in every round).

Review round 3 (after the intrusive context list and one-walk HWSS lanes;
the branch now also carries the other workers' DL-621..625 merges, which
touch the render loop's RNG and dispatch):
`hwss_prism_dispersion_pt` interleaved n = 10 -- master 235.3 +/- 3.3 s,
`190beeb98` 241.8 +/- 4.5 s (+2.8 %), round 3 236.4 +/- 3.8 s (+0.5 %).
The three scenes, n = 4 -- cornellbox master 300.1 +/- 2.3 vs 306.0 +/- 7.2
(+2.0 %), csg 34.5 +/- 1.1 vs 33.6 +/- 1.0 (-2.6 %), hwss 240.8 +/- 4.5 vs
244.1 +/- 1.3 (+1.4 %); each inside about two of its run-to-run sds.
`ScatteredRay::~ScatteredRay` is still an out-of-line frame in a `sample`
of the round-3 build (372 samples against `~ScatteredRayContainer`'s 191 at
master, of ~50k busy samples): the remaining no-context cost of the
pointer member is the compiler's inlining decision for that destructor
(declared in `ISPF.h`, outside this change).

Review round 4: `ScatteredRay::~ScatteredRay` (ISPF.h) deletes its stack
through an out-of-line `DeleteStack`, so its inline body is two compares
and it inlines into `~ScatteredRayContainer` again (a prism `sample` shows
no `~ScatteredRay` frame; the container destructor 128 samples plus
`DeleteStack` 10, against master's 191).  `hwss_prism_dispersion_pt`,
interleaved n = 8: master 248.8 +/- 1.5 s, now 249.9 +/- 2.3 s (+0.5 %).

