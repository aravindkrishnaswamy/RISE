# DL-70 — double-sided `vGeomNormal` orientation in sign-sensitive consumers

**Status: CLOSED 2026-09-14** (slice `debt-dl70`, branched from `master`
`32824325`).  Regression guard: `tests/GeomNormalOrientationSitesTest.cpp`
(72 checks / 0 failures).

---

## 1. The defect, in one sentence

A triangle mesh built `double_sided` — and `ClippedPlaneGeometry` /
`BezierPatchGeometry` on a back-face hit, and `HairGeometry`
unconditionally — flips the geometric normal it reports so that it
**opposes the incoming ray**, recording that in
`RayIntersectionGeometric::bGeomNormalOrientedToRay`.  A consumer that
then asks *"am I entering this solid?"* by testing
`Dot( ri.vGeomNormal, rayDir ) < 0` gets a **constant `true`** on such a
hit — the predicate can no longer distinguish the very thing it exists
to distinguish.

Fourteen consumers did exactly that.  A fifteenth (`Object.cpp`'s
override UV generator) was found by this slice's sibling audit.

## 2. The shared recovery

`RayIntersectionGeometric` now carries the recovery as three inline
helpers, so no site spells it by hand:

```cpp
Vector3 UnflippedGeomNormal() const;      // (oriented && !rayDerived) ? -vGeomNormal : vGeomNormal
Scalar  TrueGeomFacing( const Vector3& d ) const;  // Dot( UnflippedGeomNormal(), d )
bool    HasTrueGeomSide() const;          // !bGeomNormalRayDerived
```

Three properties matter and are what make the fix safe to apply
uniformly:

* **No-op where nothing flipped.**  Single-sided meshes and every
  analytic primitive leave `bGeomNormalOrientedToRay` false, so the
  recovery returns the raw value unchanged.
* **Identity on a RAY-DERIVED normal.**  `HairGeometry` fabricates its
  normal from the ray (`bGeomNormalRayDerived`), so there is no flip to
  undo; un-flipping it would report "always facing away from the ray",
  i.e. **exit at every strand**.  The helpers therefore return it as
  reported, which preserves each hair site's pre-DL-70 behaviour exactly.
  A site whose question genuinely has no answer for hair must **skip**,
  gated on `HasTrueGeomSide()` — which the two medium walks now do.
* **It answers a different question from `vGeomNormal` itself.**  "Which
  way does this surface face the ray" still wants the reported value.
  Only "which side of the real surface am I on" wants the recovery.

## 3. Site table

`->` = the expression after the fix.  "no-op on" lists the geometry
classes for which the change is bit-identical.

| # | Site (by symbol) | Before | After | User-visible consequence of the bug |
|---|---|---|---|---|
| 1 | `LightSampler.cpp` `EvalShadowTransmittance` (RGB) and `EvalShadowTransmittanceNM` — medium push/pop on the NEE shadow walk | `Dot(ri.geometric.vGeomNormal, ray.Dir())` | `!HasTrueGeomSide()` → skip the crossing; else `ri.geometric.TrueGeomFacing(ray.Dir())` | Every crossing pushed, nothing ever removed → the interior medium attenuated the **whole remaining distance to the light**.  Measured: a directional light behind a double-sided medium box went to **exactly 0**. |
| 2 | `BDPTIntegrator.cpp` `EvalConnectionTransmittance`-family connection walk | same | same | Same, on BDPT/VCM connection rays. |
| 3 | `RayCaster.cpp` `CastShadowRayTransmittance` — `bEntering` selecting the Fresnel `(Ni, Nt)` pair and the IOR-stack update | `Dot(dir, ri.geometric.vGeomNormal) < 0` | `Dot(dir, ri.geometric.UnflippedGeomNormal()) < 0` | The exit face read "entering", so `(Ni, Nt)` became glass→glass (matched indices, `F = 0`, `T = 1`): a double-sided pane transmitted **one** interface's Fresnel instead of two, and the IOR stack grew a second push instead of popping.  Measured: `T = 0.96` instead of `0.9216` at `eta = 1.5`. |
| 3b | `RayCaster.cpp` `ResolveXrayView_`'s local `trueGeomFacing` lambda | hand-spelled `oriented ? -raw : raw` | forwards to `g.TrueGeomFacing(d)` | No behaviour change on any reachable input (hair cannot reach this walk — `HairMaterial` does not report `CouldLightPassThrough`); the helper makes the hair exclusion structural rather than incidental. |
| 4 | `DirectVolumeRenderingShader.cpp` `Shade` and `ShadeNM` — the entering-vs-leaving test | `-Dot(ri.geometric.vGeomNormal, ray.Dir())` | `-ri.geometric.TrueGeomFacing(ray.Dir())` | `cosine` was unconditionally positive, the "we are leaving" branch never fired, and **the volume was never shaded at all** through a double-sided container mesh. |
| 5 | `SMSPhotonMap.cpp` `TraceSMSPhoton` — the `bEntering` stamp on a reflection vertex | `Dot(ray.Dir(), ri.geometric.vGeomNormal)` | `ri.geometric.TrueGeomFacing(ray.Dir())` | A back-face reflection vertex on a double-sided specular caster was stamped "entering", propagating a wrong side bit and therefore a wrong Fresnel `etaI`/`etaT` pair into the photon record. |
| 6 | The BSSRDF front-face gate `cosInGeom > NEARZERO`: `PathTracingIntegrator.cpp` ×2, `BDPTIntegrator.cpp` ×4 | `Dot(ri.geometric.vGeomNormal, wo)` | `ri.geometric.TrueGeomFacing(wo)` | An unconditional PASS: a BACK-face (interior) hit was admitted into BSSRDF entry sampling, handing `BSSRDFSampling::SampleEntryPoint` — whose own DL-71 correction already works in true-normal space — a shading point on the wrong side of the surface.  **Deliberately not a hair skip**: rejecting there would silently remove SSS from hair, a combination DL-75 left undefined-but-permitted and `HairSSSEntryNormalTest` characterises as producing well-defined output. |
| 7 | `TransparencyShaderOp.cpp` `PerformOperation` / `PerformOperationNM` — the `bOneSided` cull | `Dot(ray.Dir(), ri.geometric.vGeomNormal) > 0` | `ri.geometric.TrueGeomFacing(ray.Dir()) > 0` | The cull never fired on exactly the geometry classes alpha cards use.  **Intent** (since "double-sided geometry + a one-sided shader op" is a contradiction someone has to resolve): `bOneSided` is an explicit, per-shader-op author request to discard the back face and is the more specific of the two statements; honouring it is also what makes the op mean the same thing on a single-sided card, a double-sided card and a Bezier patch. |
| 8 | `Object.cpp::IntersectRay` — the override UV generator's normal argument (**sibling audit, not in the original row**) | `ri.geometric.vGeomNormal` | `ri.geometric.UnflippedGeomNormal()` | `BoxUVGenerator` picks its box side from the **sign** of the normal's dominant component, so the same surface point charted onto the opposite box side depending on which side the ray arrived from — a **view-dependent texture chart**.  Measured on a double-sided clipped plane: `u = 0.65` from the front, `u = 0.35` from the back. |

### The adjacent hazard: plumbing that dropped the flag

The row recorded `ManifoldSolver.cpp` and `SMSPhotonMap.cpp` copying
`ri.geometric.vGeomNormal` into their own vertex structs, and later
republishing it into synthetic `RayIntersectionGeometric` rigs whose
`bGeomNormalOrientedToRay` defaults **false** — "no sign error today".
Two corrections:

* **There was a sign error today.**  `ManifoldSolver::BuildSeedChain`'s
  `cosI = Dot(dir, sideN); bEntering = sameObjectAgain ? false : (cosI < 0)`
  reads the stored normal's sign.  Its own comment says the IOR-stack
  `containsCurrent()` override exists to catch the thin-double-sided case
  "where both crossings have the normal pointing the same way" — the site
  already knew the sign test was blind there.  That override cannot fire
  for a walk that **starts inside** the solid (nothing pushed yet), and
  that case was simply wrong.
* **Un-flipping is safe at the producer**, which is why the fix goes
  there rather than at the one consumer.  Every other consumer of
  `ManifoldVertex::geomNormal` is invariant under a global sign flip:
  `ValidateChainPhysics` and the two-stage `failIdx` scan test a sign
  **product** `(wi·n)(wo·n)`; `EvaluateChainGeometry`,
  `EvaluateChainCosineProduct` and `cosV1atX` take `fabs`; and the
  synthetic-rig consumers reach the field through the ray-anchored
  `geomNRaw` idiom, which re-anchors to the rig's own (synthetic) ray and
  is invariant in the stored sign.

`ManifoldVertex::geomNormal` therefore now carries the documented
invariant **"the TRUE, ray-independent outward normal"**, and both
`ManifoldSolver`'s five capture sites and `SMSPhotonMap`'s chain-vertex
capture store `UnflippedGeomNormal()`.  The republished rigs now tell the
truth.

**`BDPTVertex::geomNormal` deliberately does NOT get this treatment.**
Its consumers are `fabs`/sign-product (the MIS `cosAtEye`/`cosAtLight`
rows, `VCMIntegrator`'s `sideN`) plus the **emitter** rows
(`emittedRadiance(rig, dir, v.geomNormal)`, `cosAtEmitter`), which need
the ray-facing value for exactly the reason the DL-70 row gives for
`EmissionShaderOp.cpp` :56/:131: passing the ray-facing normal is what
makes a double-sided emitter emit toward the viewer.

## 4. Red-proofs

`tests/GeomNormalOrientationSitesTest.cpp`.  Every "control" row below is
the same fixture on geometry that does not flip, and passes on the
unfixed library — which is what makes the double-sided rows a defect
rather than a fixture artifact.

**Sites 1/2 — the medium walk.**  `LightSampler::EvaluateDirectLighting`
Step 1 (a directional light along `+Z`), a **closed** cube of clear
dielectric (`eta = 1.5`, delta pass-through `scattering 1000000`)
carrying a homogeneous absorber `sigma_a = 0.25`, chord 2, receiver at
`z = -3`, transparent shadows on.  Reference measured live from the same
scene with the medium removed:

```
Sub-test 3: NEE shadow-walk medium stack through a double-sided medium cube (sites 1/2)
  FAIL: (double-sided) MONEY: the medium attenuates by exactly exp(-sigma_a*chord)
        over the CHORD, not beyond it  (got 0, want 0.185342 +/- 0.00185342)
```

Single-sided control on the same commit: `L = 0.177928` against
`L_clear * exp(-0.5) = 0.293354 * 0.606531 = 0.177931`.  Post-fix both
variants pass.

**Sites 1/2, the RAY-DERIVED half.**  A hair strand carrying an interior
medium, measured with only the `HasTrueGeomSide()` skip removed:

```
Sub-test 4: the shadow walk skips a RAY-DERIVED (hair) crossing (sites 1/2)
  FAIL: (hair medium) MONEY: a ray-derived crossing does not leak its interior
        medium down the rest of the shadow ray
```

**Site 3 — the transmissive-shadow Fresnel pair.**
`RayCaster::CastShadowRayTransmittance` straight through the same closed
cube at normal incidence, against the closed form `(1 - F)^2` with
`F = ((eta-1)/(eta+1))^2 = 0.04`:

```
Sub-test 2: transmissive-shadow Fresnel pair through a double-sided dielectric cube (site 3)
  FAIL: (double-sided) MONEY: transmittance == (1-F)^2 (BOTH interfaces read their
        real Fresnel pair)  (got 0.96, want 0.9216 +/- 0.004608)
```

**Site 8 — the UV chart.**  A double-sided `ClippedPlaneGeometry` with a
2×2×2 `BoxUVGenerator`, the point `(0.3, 0.1, 1)` reached from `+Z` and
from `-Z`:

```
Sub-test 5: an override UV generator charts the surface, not the viewing side (sibling audit)
  FAIL: (clipped plane) MONEY: the UV chart is a property of the surface,
        not of the viewing side
  [front uv = (0.65, 0.45)   back uv = (0.35, 0.45)]
```

Post-fix both read `(0.65, 0.45)` — the independently-known answer for
the plane's true `+Z` outward normal (box side 5, `u = (x + w/2)/w`).

**Sites 4, 5, 6, 7 and the ManifoldSolver `bEntering` — consistency
pins, honestly labelled.**  Their enclosing functions are not reachable
from a test: two file-static shadow-walk helpers, an anonymous-namespace
SMS photon tracer, the BSSRDF gates buried in the PT/BDPT bounce loops,
a volume shader op that needs a volume file on disk, and a transparency
op that needs a full shading rig.  Sub-test 1 pins the **mechanism** on
records the production geometry really produced (a closed double-sided
cube hit from inside, at normal incidence AND at a 30-degree tilt; a
single-sided cube; an analytic sphere; a hair strand): for each of those
sites' predicate forms it asserts that the pre-fix spelling takes the
wrong branch at a true exit, the post-fix spelling takes the right one,
and both spellings agree on the non-flipping controls.  Sub-test 1 is
green before and after the fix, by construction — it is a pin, not a
red-proof.

## 5. Sibling audit

Every `vGeomNormal` read in `src/Library` was enumerated
(`grep -rn vGeomNormal src/Library`, 200 non-comment lines outside
`RayIntersectionGeometric.h`) and classified.

| Class | Count | Sites | Verdict |
|---|---|---|---|
| Ray-anchored `geomNRaw` composite — `geomN = Dot(geomNRaw, ray.Dir()) < 0 ? geomNRaw : -geomNRaw` | 59 | every material SPF/BRDF (`GGX`, `CookTorrance`, `Schlick`, `Ward` ×2, `OrenNayar`, `Lambertian`, `IsotropicPhong`, `AshikminShirley`, `Sheen`, `Fabric`, `Weave`, `Coated`, `SubSurfaceScattering` ×7, `PerfectReflector`, `PerfectRefractor`, `Dielectric`), `GlintModifier` | **IMMUNE** — re-anchors to the ray, so the composite is the same vector whether or not the geometry flipped.  Do not "fix" these. |
| Joint-flip shading composite — `side = Dot(vGeomNormal, dir); n = side > 0 ? -vNormal : vNormal` | 12 | `IsotropicPhongSPF` ×4, `PolishedSPF` ×7 (`bBackface`/`gateBackface`, each consumed by exactly that one line), `RandomWalkSSS.cpp:84` | **IMMUNE** — the flip moves `vGeomNormal` and `vNormal` together, so the ray-facing result is unchanged. |
| Joint-flip lockstep pair | 1 | `RandomWalkSSS.cpp:311` (`if (Dot(exitGeomNormal, dir) < 0) { flip both }`) | **IMMUNE** — same argument; the composite produces a pair oriented along `dir` either way. |
| `fabs` / symmetric band | 12 | `EmissionShaderOp` :101/:163, `PhotonMap.h:699`, `GlobalSpectralPhotonMap` ×2, `CausticSpectralPhotonMap` ×2, `InteractivePelRasterizer:657`, `PathTracingIntegrator` :2549/:5668, `CSGObject` ×3 via `AdoptCsgExitFacePayloadViaProbe` (all uses `fabs`/magnitude-normalised) | **IMMUNE** |
| Ray-facing BY INTENT (a double-sided emitter must emit toward the viewer) | 2 + the `BDPTVertex` emitter rows | `EmissionShaderOp.cpp` :56/:131; `BDPTIntegrator` `EvalEmitterRadiance`/`cosAtEmitter`, `VCMIntegrator` :1049/:1092/:1751 | **DO NOT FIX** (the DL-70 row's own ruling; extended here to the `BDPTVertex` mirrors of the same rows) |
| Not a which-side test — irradiance/AO cache key or continuity heuristic | 9 | `FinalGatherShaderOp` :396/:402/:422/:721 and :229 (`sample.vNormal`, a same-surface continuity test whose only failure mode is a conservative REJECT), `AmbientOcclusionShaderOp` ×3, `DistributionTracingShaderOp` ×3 | **NOT FIXED** — no wrong value is produced; a flipped normal can only make the cache decline to reuse a neighbour. |
| Sign-invariant chain consumers | 7 | `ValidateChainPhysics` `nForTest`, `VCMIntegrator:521` `sideN`, `ManifoldSolver` `EvaluateChainGeometry` / `EvaluateChainCosineProduct` / `cosV1atX` / the two-stage `failIdx` scan | **IMMUNE** (sign product or `fabs`) — and re-verified against the new `ManifoldVertex::geomNormal` invariant. |
| Comparison against ITSELF | 1 | `PathTransportUtilities.h:282` (DL-03's guided-continuation resolver compares two dots against the SAME normal) | **IMMUNE** |
| Producers | 5 geometry types | `TriangleMeshGeometry{,Indexed}` (`bFlipGeomNormal`), `ClippedPlaneGeometry`, `BezierPatchGeometry`, `HairGeometry` | not consumers |
| **IN PATTERN — fixed here** | 15 sites + 6 captures | the table in §3, plus `ManifoldSolver` ×5 and `SMSPhotonMap` ×1 captures | fixed |

The original 14-site enumeration was **complete for the classes it
considered** but missed `Object.cpp`'s UV-generator argument, which it
had classified as "not a which-side test at all".  It is one (see §3
row 8).

## 6. New debt opened

**DL-95** — `Object::IntersectRay` calls `pUVGenerator->GenerateUV` with
`ri.geometric.ptIntersection` **before that field is written** (the write
is ~380 lines further down, after the normals are transformed back).
Analytic primitives happen to survive because several of them stamp an
object-space `ptIntersection` inside their own `IntersectRay`; triangle
meshes stamp none, so an override UV generator on a mesh reads whatever
the caller's record happened to hold — `(0,0,0)` in a fresh record, and
the **previous object's** intersection point in a real render loop.
Measured: a 2×2×2 `BoxUVGenerator` on a double-sided cube mesh returned
`(0.5, 0.5)` at every hit, the value for `ptIntersection == (0,0,0)`.
Found by this slice's sibling audit while red-proofing §3 row 8; the
fixture was switched to `ClippedPlaneGeometry` (which does stamp the
point) so that the DL-70 assertion is not confounded by it.  Not fixed
here: it is an independent, pre-existing ordering defect, not the DL-70
pattern.

## 7. Files touched

| File | Change |
|---|---|
| `src/Library/Intersection/RayIntersectionGeometric.h` | the three shared helpers + contract |
| `src/Library/Lights/LightSampler.cpp` | sites 1 (RGB + NM) |
| `src/Library/Shaders/BDPTIntegrator.cpp` | site 2, site 6 ×4 |
| `src/Library/Rendering/RayCaster.cpp` | site 3, site 3b |
| `src/Library/Shaders/DirectVolumeRenderingShader.cpp` | site 4 ×2 |
| `src/Library/Utilities/SMSPhotonMap.cpp` | site 5, chain-vertex capture |
| `src/Library/Shaders/PathTracingIntegrator.cpp` | site 6 ×2 |
| `src/Library/Shaders/TransparencyShaderOp.cpp` | site 7 ×2 |
| `src/Library/Objects/Object.cpp` | site 8 |
| `src/Library/Utilities/ManifoldSolver.{h,cpp}` | the `geomNormal` invariant + 5 captures |
| `src/Library/Materials/TranslucentSPF.cpp`, `src/Library/Utilities/IORStackSeeding.h` | migrated to the shared helper (pure refactor) |
| `tests/GeomNormalOrientationSitesTest.cpp` | new |

`src/Library/Utilities/BSSRDFSampling.cpp` was **not** migrated: its
recovery is entangled with the P2-A shading-normal re-orientation inside
the same branch, so forwarding to the helper would not be a pure
refactor.
