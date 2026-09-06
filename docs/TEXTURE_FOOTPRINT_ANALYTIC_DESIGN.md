# Texture Footprint on Analytic Geometry — and the Exact Instance-Scale Fold

## 1. The question, and the answer

**Question.** `RayIntersectionGeometric::txFootprint` is the renderer's only
per-hit filter-width channel: it drives `TexturePainter`'s mip LOD, the
expression VM's `fw` (which fades `fbm` / `turbulence` / `ridged` octaves the
pixel cannot resolve), `WeaveBRDF`'s coverage fade, and `ReliefModifier`'s
`s = max(step, fw)` step rule. Today it is populated by **triangle meshes and
nothing else**. Can every other geometry get one, and can the object→world
fold that lands it in world units be made *exact* rather than the
`|det M|^(1/3)` geometric-mean approximation it is now?

**Answer.** Yes to both, and they are the same edit.

1. `ComputeTextureFootprint`'s first half — the Igehy plane-projection that
   produces `dpdx` / `dpdy` — needs only the hit point, a normal, and the
   ray's differentials. It does **not** need `dpdu`/`dpdv`; only the
   *second* half (the 2×2 solve for `dudx…dvdy`) does. Split the helper at
   that seam and every geometry can have a `worldWidth`, whether or not it
   has a UV parameterisation.
2. Move the single call site from the two mesh intersectors up to
   `Object::IntersectRay`, immediately after `pGeometry->IntersectRay`
   returns and **before** the normal/derivative promotion block — at that
   point the ray, the hit range, the normal and `derivatives.dpdu/dpdv` are
   all still object-space, exactly the frame the helper already assumes.
3. Carry `dpdx` / `dpdy` as **vectors** on `TextureFootprint`, and replace
   the `worldWidth *= m_worldLinearScale` fold in `Object::IntersectRay` /
   `CSGObject::IntersectRay` with `dpdx_w = M · dpdx_o` (forward linear map)
   and `worldWidth = ½(|dpdx_w| + |dpdy_w|)`. Because the transform is
   affine, the object-space auxiliary *line* maps onto the world-space
   auxiliary line and the tangent plane maps onto the tangent plane, so the
   plane-intersection point commutes with `M` — the transformed vector is
   the **exact** world footprint, for any linear map, non-uniform scale and
   shear included.

Result: analytic primitives, SDFs (hence sweeps and skeletons), CSG results,
boxes, disks, planes, patches and hair all fade at distance the way meshes
already do; and the `scale 4 0.05 4` panel's 4.31× under-count disappears
on the mesh path too.

## 2. Facts, with symbols

| # | Fact | Symbol |
|---|---|---|
| F1 | Differentials live on the ray: `hasDifferentials`, `diffs.{rxOrigin,ryOrigin,rxDir,ryDir}`, offsets from the central origin/direction | `Ray::hasDifferentials`, `Ray::diffs`, `RayDifferentials` |
| F2 | **Only** `PinholeCamera::GenerateRay` sets them. `ThinLensCamera`, `OrthographicCamera`, `FisheyeCamera` do not — no `diffs` reference in any of those files | `PinholeCamera::GenerateRay` |
| F3 | `Ray::Set` / `Ray::SetDir` clear `hasDifferentials` — any freshly-`Set` ray is differential-free by construction | `Ray::Set`, `Ray::SetDir` |
| F4 | **Differentials are never propagated through a scattering bounce.** `RayDifferentials.h`'s own header advertises "`PropagateThroughReflection` / `PropagateThroughRefraction` apply Igehy's closed-form formulas" — **those functions do not exist anywhere in `src/`.** The struct has four `Vector3` members and a default ctor, nothing else | `RayDifferentials` (header comment vs. body) |
| F5 | The only two transfers are straight-line: `RayCaster`'s x-ray continuation (`rxOrigin += t·rxDir`) and `CSGObject`'s reversed exit probe (same transfer, then `rxDir` negated) | `RayCaster::ResolveXrayView_`, `CSGObject::AdoptCsgExitFacePayloadViaProbe` |
| F6 | Both object layers already transform differentials into object space correctly — origins by the linear part, directions by reconstruct → transform → renormalise → re-difference | `Object::IntersectRay`, `CSGObject::IntersectRay` (the `if( orig.hasDifferentials )` blocks) |
| F7 | `ComputeTextureFootprint` early-outs on `!ray.hasDifferentials \|\| !ri.derivatives.valid`; recovers the hit point as `ri.ray.PointAtLength( ri.range )` (never reads `ptIntersection`); projects onto the plane `(P0, ri.vNormal)`; then solves the 2×2 for `dudx…dvdy` | `ComputeTextureFootprint` |
| F8 | `worldWidth = ½(\|dpdx\| + \|dpdy\|)` — a *full-pixel-step* average, not a radius | `ComputeTextureFootprint`, final lines |
| F9 | Two call sites, both inside per-candidate mesh element intersection — so it runs once per accepted closer hit, not once per ray | `TriangleMeshGeometry::RayElementIntersection`, `TriangleMeshGeometryIndexed::RayElementIntersection` |
| F10 | The object→world fold is `worldWidth *= m_worldLinearScale`, `m_worldLinearScale = \|det\|^(1/3)`; its own comment concedes the `scale 4 0.05 4` panel is 4.31× too small | `Object::IntersectRay`, `CSGObject::IntersectRay`, `Object::m_worldLinearScale` |
| F11 | CSG composes by applying its own factor once to the child's already-folded value | `CSGObject::IntersectRay`, `AdoptCsgSurfacePayload` |
| F12 | `fw` is consumed **unscaled**, in the same units as the position argument the expression passes to `fbm` | `ExprProgram::CallFunc` cases 43–45 |
| F13 | The fade band is `lo = 0.2`, `hi = 0.6`, smoothstep, weight 1 below `lo`, 0 at/above `hi`; empirically chosen, deliberately earlier than classic Nyquist | `OctaveFadeWeightImpl` |
| F14 | `worldWidth` consumers: `ExpressionPainter::BuildContext` (×2, colour and scalar pipes), `ReliefModifier::Modify`, `PainterPreview`'s `MakePreviewRi` | as named |
| F15 | `dudx…dvdy` consumers: `TexturePainter::SampleTextured` (mip LOD / supersample), `WeaveBRDF` (`fpUV`) — both gate on `txFootprint.valid` | as named |
| F16 | Every geometry sets `ri.vNormal` and `ri.vGeomNormal`; only meshes have a shading-vs-geometric difference | e.g. `SphereGeometry::IntersectRay` (`vGeomNormal = vNormal; // analytical`) |
| F17 | `Object::IntersectRay` is the sole gateway — BSP/octree/BVH traversal is internal to a geometry, and `CSGObject` reaches its operands through their own `Object::IntersectRay` | `Object::IntersectRay`, `CSGObject::IntersectRay` |

### 2.1 Classification of every geometry

| Geometry | `derivatives.valid`? | Footprint today | After |
|---|---|---|---|
| `TriangleMeshGeometry` / `…Indexed` | yes (`useUVJacobian` gated for the tangent, `valid` set regardless) | **yes** | unchanged values, one call site moved |
| `DisplacedGeometry` | delegates to `m_pMesh` | yes | unchanged |
| `BezierPatchGeometry`, `BilinearPatchGeometry` | via tessellated mesh in BSP/octree | yes | unchanged |
| `SphereGeometry` | **yes**, ungated (`ComputeSurfaceDerivatives`) | no | **(a)** full UV + width |
| `EllipsoidGeometry` | yes, ungated | no | **(a)** full UV + width |
| `CylinderGeometry` (both hit branches) | yes, ungated | no | **(a)** full UV + width |
| `TorusGeometry` | yes, ungated | no | **(a)** full UV + width |
| `ClippedPlaneGeometry` | **no** — sets `vShadingTangent` only; its own comment says it "never populated `ri.derivatives` at all … and still does not" | no | **(b)** width only |
| `InfinitePlaneGeometry` | no | no | **(b)** width only |
| `BoxGeometry` | no | no | **(b)** width only |
| `CircularDiskGeometry` | no | no | **(b)** width only |
| `HairGeometry` | no (`vShadingTangent` = fibre tangent) | no | **(b)** width only |
| `SDFGeometry` — incl. **sweep / skeleton / parts / heightfield** (all one class) | no by design (`curvatureValid` without `valid`) | no | **(b)** width only |
| `CSGObject` result | inherits the winning operand's | inherits | inherits, exact fold per level |

Legend: **(a)** `ComputeTextureFootprint` runs to completion — `dudx…dvdy`
*and* `worldWidth`. **(b)** the UV solve is skipped; `worldWidth` is still
exact. Note the sweep and skeleton geometries are **not** separate classes —
they are `sdf_geometry` parts, so the SDF row covers them.

## 3. The design

### 3.1 Split `TextureFootprint`'s validity into two flags

```cpp
struct TextureFootprint {
    Scalar  dudx, dudy, dvdx, dvdy;
    Vector3 dpdx, dpdy;      // NEW: plane-projected pixel-step vectors, current frame
    Scalar  worldWidth;
    bool    valid;           // UNCHANGED MEANING: the UV Jacobian is usable
    bool    widthValid;      // NEW: dpdx/dpdy/worldWidth are usable
};
```

Invariant: `valid ⇒ widthValid`. Keeping `valid` as the *UV* flag is
deliberate — `TexturePainter::SampleTextured` and `WeaveBRDF` (F15) read it
and would otherwise be handed a zero Jacobian on a UV-free hit, which
`ComputeLODFromTexelFootprint` would read as LOD 0 (finest) instead of the
honest base-level fallback. `worldWidth`'s consumers (F14) switch to
`widthValid`.

### 3.2 Split the helper at the seam it already has

`ComputeTextureFootprint` becomes a thin composite over two new statics in
the same header:

* `ComputeFootprintVectors( ri, ray )` — the plane projection. Requires
  `ray.hasDifferentials` and `ri.bHit`; reads `ri.ray`, `ri.range`,
  `ri.vNormal`. Writes `dpdx`, `dpdy`, `worldWidth`, `widthValid`. Same
  parallel-auxiliary guard (`|den| < 1e-20`).
* `SolveFootprintUV( ri )` — the existing 2×2 solve. Requires
  `ri.derivatives.valid` and `widthValid`. Writes `dudx…dvdy`, `valid`.

Use `ri.vNormal` (the shading normal), **not** `vGeomNormal`: that is what
the shipped mesh path uses, and switching would move every mesh pixel. The
plane math is sign-invariant, so `ClippedPlaneGeometry`'s back-face normal
flip is harmless. Recorded as a residual in §7.

### 3.3 One call site, at the Object layer

Delete both mesh call sites (F9). In `Object::IntersectRay`, inside
`if( ri.geometric.bHit )` and **before** the `vNormalWorldUnnorm` promotion:

```cpp
if( ri.geometric.ray.hasDifferentials ) {
    ComputeFootprintVectors( ri.geometric, ri.geometric.ray );   // any geometry
    SolveFootprintUV( ri.geometric );                            // no-op unless derivatives.valid
}
```

Everything it reads is object-space at that instant. This is also a small
win over the status quo: once per ray instead of once per accepted candidate.

### 3.4 The exact fold replaces the geometric mean

Replace the `worldWidth *= m_worldLinearScale` blocks in both
`Object::IntersectRay` and `CSGObject::IntersectRay` with:

```cpp
if( ri.geometric.txFootprint.widthValid ) {
    Vector3 dx = Vector3Ops::Transform( m_mxFinalTrans, ri.geometric.txFootprint.dpdx );
    Vector3 dy = Vector3Ops::Transform( m_mxFinalTrans, ri.geometric.txFootprint.dpdy );
    ri.geometric.txFootprint.dpdx = dx;
    ri.geometric.txFootprint.dpdy = dy;
    ri.geometric.txFootprint.worldWidth =
        Scalar(0.5) * ( Vector3Ops::Magnitude(dx) + Vector3Ops::Magnitude(dy) );
}
```

Why it is exact: for affine `M`, the object-space auxiliary ray reconstructed
by F6 is the *image line* of the world auxiliary ray, and the object-space
tangent plane is the image of the world tangent plane; a line∩plane point
therefore commutes with `M`, so `P_x^world = M · P_x^obj` and
`dpdx^world = M_linear · dpdx^obj`. No approximation, no `det`, no
degenerate-transform special case needed beyond what `Transform` already
does. CSG nesting composes for the free reason F11 already relies on: each
level applies its own forward map once to what the level below produced.

`m_worldLinearScale` keeps its other two jobs (`scaleHint`, `curvature`) —
this design does not touch them.

### 3.5 `fw` semantics — no unit change

`worldWidth` stays the same quantity it is today (F8): the mean magnitude of
the two one-pixel-step, plane-projected displacements — a *diameter*-like
measure, in the same length units as `ctx.P`. The analytic path shares the
helper, so it is consistent by construction, and the `0.2 / 0.6` band (F13)
keeps meaning what it means on meshes. The known `fbm(P*10, …)` mismatch
(F12) is unchanged and out of scope.

## 4. Alternatives considered

* **Give every analytic primitive a `dpdu`/`dpdv` and reuse the helper
  unchanged.** Rejected: `SDFGeometry` has no natural `(u,v)` for which
  partials mean anything (the reason `derivatives.valid` is deliberately
  false there while `curvatureValid` is true), and boxes/disks would need
  fabricated charts. The UV-free path is the honest answer for those.
* **Fix only the non-uniform-scale fold, leave analytic geometry unfootprinted.**
  Rejected: it is the smaller half of the defect, and the vectors needed for
  the exact fold are exactly the vectors the analytic path needs anyway.
* **Estimate `worldWidth` as `range · θ_pixel / |cos θ_incidence|`.** Rejected:
  same arithmetic cost, needs the camera's pixel solid angle threaded to the
  intersector, and it is *only* correct for a pinhole.
* **Reuse `valid` for both meanings.** Rejected — F15.
* **Clamp the footprint at grazing incidence.** Deferred to §7: any clamp in
  the shared helper moves mesh pixels, which this arc forbids.

## 5. Tests

New suite `TextureFootprintTest.cpp` (add to all five build projects per
CLAUDE.md), driving real `Object::IntersectRay` calls with a
`PinholeCamera`-generated ray.

Closed form for the oracle. `PinholeCamera::ComputeScaleFromFOV` stretches
by `h/height` on the y axis with `h = 2·tan(fov/2)`, so the on-axis pixel
angular size is `θ = 2·tan(fov/2) / height` (with `pixelAR = 1` the x axis
matches). On a surface facing the ray at distance `d`, `worldWidth ≈ d·θ`.

1. **Distance scaling.** Sphere on the axis, camera at `d` and `2d`:
   `worldWidth` ratio `== 2` within 1 %.
2. **Closed form.** Same hit vs. `d·θ` within 3 % (the residual is the
   sphere's curvature over one pixel).
3. **Mesh/analytic agreement.** A sphere and a triangle-mesh quad of the
   same size at the same distance, both facing the camera: `worldWidth`
   agrees within 3 %. This is the test that would have caught a units or
   half-vs-full-pixel-step mismatch between the two paths.
4. **Uniform instance scale is exact.** `scale 10` sphere → ratio exactly
   10 (tight, 1e-9). Mirrors `ReliefModifierTest` test 4c, which must keep
   passing unchanged.
5. **Non-uniform scale exactness — the headline regression.** A
   `clipped_plane` (or box face) under `scale 4 0.05 4`, viewed along the
   flattened axis so the footprint lies in the 4× plane: `worldWidth` must
   be **4×** the unscaled value, not `|det|^(1/3) = 0.9283×`. Red-proof by
   restoring the `m_worldLinearScale` multiply — it must fail at 4.31×.
6. **UV-free geometries report a width and no Jacobian.** An
   `sdf_geometry` sphere and a `box_geometry`: `widthValid == true`,
   `worldWidth > 0`, `valid == false`.
7. **CSG composes.** A `csg_object` union of two spheres, one operand
   scaled: the surviving hit's `worldWidth` matches the same-scaled plain
   sphere within 1e-9.
8. **No differentials, no footprint.** A `Ray` built by hand (F3):
   `widthValid == false`, `worldWidth == 0`.
9. **Grazing guard.** A near-tangent sphere hit must not produce a non-finite
   `worldWidth`.

Existing suites:

* `ReliefModifierTest` — test 4c (uniform `scale 10` fold) must stay green
  *unchanged*; add one non-uniform companion asserting the in-plane factor.
  Tests that stamp `txFootprint` by hand need `widthValid = true` added.
* `TextureExpressionVMTest` — the `fw == worldWidth` cases stamp the struct
  directly; they gain `widthValid = true`. Values unchanged.
* `PainterPreviewTest` — `MakePreviewRi` sets `valid = true` today; it must
  set `widthValid = true` (setting `valid` alone would stop feeding `fw`).
  Preview images must be byte-identical.
* `BilinMipLODTest` — **unaffected.** It exercises
  `BilinRasterImageAccessor::GetPELwithLOD` and the pyramid guards directly,
  never `txFootprint`.
* `ExpressionFieldFinitenessTest` — **unaffected.** Its subject is
  `ExpressionProgram::IsFinite` and the displacement bake, neither of which
  reads a footprint.
* `SourceHygieneTest` — new file must satisfy the usual header/tab rules.

## 6. Scenes that change look, and the render plan

`receding_pier` (mesh, the showcase for the fade) **must not change** — the
mesh path's values are untouched except where an instance is non-uniformly
scaled. Verify pixel-identity first; it is the gate for the whole arc.

Scenes carrying `fbm` / `turbulence` / `ridged` expressions on geometry that
gets a footprint for the first time — these **will** gain a distance fade,
and that is the fix, not a regression:

| Scene | Geometry gaining a footprint |
|---|---|
| `scenes/FeatureBased/Textures/oxidized_copper.RISEscene` | sphere |
| `scenes/FeatureBased/Textures/weathered_workbench.RISEscene` | box |
| `scenes/FeatureBased/Materials/lacquer_and_rain_still_life.RISEscene` | box, SDF |
| `scenes/FeatureBased/Materials/rainwet_cobbles.RISEscene` | SDF |
| `scenes/FeatureBased/Materials/velvet_cushion.RISEscene` | SDF |
| `scenes/FeatureBased/Materials/rainwet_courtyard_night.RISEscene` | box, cylinder, SDF |
| `scenes/FeatureBased/Hair/cottontail_dusk.RISEscene` | sphere, ellipsoid, hair |
| `scenes/FeatureBased/Hair/dandelion_clock.RISEscene` | sphere, hair |
| `scenes/FeatureBased/GeometrySignals/weathered_reliquary.RISEscene` | SDF |
| `scenes/Tests/Painters/expression_painter_marble.RISEscene` | sphere |
| `scenes/Tests/Painters/ramp_painter_terrain.RISEscene` | sphere, box |
| `scenes/Tests/Painters/relief_sphere_no_uv.RISEscene` | sphere (relief `max(step, fw)` becomes live) |
| `scenes/Tests/Painters/displaced_plus_relief_shared_field.RISEscene` | sphere half only |
| `scenes/Tests/Materials/wetness_prelude_validation.RISEscene` | SDF |
| `scenes/FeatureBased/Textures/receding_pier.RISEscene` | box props only — the pier mesh is unchanged |

Separately, **textured spheres/ellipsoids/cylinders/tori start mip-mapping**
(class (a) sets `valid`, so `TexturePainter::SampleTextured` leaves
`Mode_Base`). Any scene with an image painter on those primitives changes,
in the same "was aliasing, now filtered" direction.

Render plan: before/after EXR pairs at the scene's own settings for
`receding_pier` (expect identical), `oxidized_copper`, `rainwet_cobbles`,
`relief_sphere_no_uv` and `cottontail_dusk`; eyeball for the intended
"sparkle at distance disappears, near field unchanged", and confirm no
darkening in the near field (a uniform darkening would mean `fw` is
over-large — check the half-vs-full-pixel-step convention first).

## 7. Cost, and residuals

Cost per hit **that carries differentials**: two dot products, two divides,
two 3-vector scaled adds, and (for the fold) two matrix-vector transforms
plus two `sqrt`. Nothing on a ray with `hasDifferentials == false`, which by
F2/F3/F4 is **every shadow ray, every NEE ray, every photon, every ray after
the first scattering bounce, and every ray from the thin-lens, orthographic
and fisheye cameras**. In practice this is a primary-visibility-only cost,
and it is strictly cheaper than today on meshes (F9: once per ray, not once
per accepted candidate).

Residuals, disclosed rather than fixed here:

* **Primary rays only.** F4 is the reason: the propagation helpers
  `RayDifferentials.h` advertises were never written. Reflections and
  refractions see `fw == 0` and point-sample. Fixing that is a separate,
  larger arc (Igehy §3.2/§3.3 plus a `dndu`/`dndv` requirement at each
  specular vertex); this design does not pretend to.
* **`RayDifferentials.h`'s header comment is wrong** and should be corrected
  in the same commit to say the propagation helpers do not exist.
* **Shading vs. geometric normal.** §3.2 keeps `vNormal` for mesh
  bit-identity; on a strongly Phong-interpolated mesh the projection plane
  is slightly off the true face.
* **No grazing clamp.** At silhouettes the auxiliary rays are near-parallel
  to the tangent plane and `worldWidth` grows without bound, which now fades
  analytic-primitive silhouettes to flat the way it already can on meshes.
  Measure before clamping (§4).

## 8. Phases

1. **Phase 1 — struct + helper split, no behaviour change.** Add `dpdx`,
   `dpdy`, `widthValid`; split the helper; keep the two mesh call sites and
   the `m_worldLinearScale` multiply. Gate: `ReliefModifierTest`,
   `TextureExpressionVMTest`, `PainterPreviewTest` green; `receding_pier`
   pixel-identical.
2. **Phase 2 — move the call site to `Object::IntersectRay`.** Analytic and
   UV-free geometries light up. New `TextureFootprintTest` tests 1–3, 6, 8, 9.
3. **Phase 3 — exact fold** in both object layers, `m_worldLinearScale`
   multiply deleted from the `txFootprint` block only. Tests 4, 5, 7, and
   the non-uniform companion in `ReliefModifierTest`.
4. **Phase 4 — scene sweep**, the §6 render plan, and a `docs/` note plus a
   correction to `RELIEF_MODIFIER_DESIGN.md` §3.3's "the fade is mesh-only"
   paragraph, which this arc retires.

## 9. Implementation brief

Enough to implement without re-research.

1. **`src/Library/Intersection/RayIntersectionGeometric.h`**, `struct
   TextureFootprint`: add `Vector3 dpdx, dpdy;` and `bool widthValid;`,
   initialise `dpdx(0,0,0)`, `dpdy(0,0,0)`, `widthValid(false)` in the ctor.
   Rewrite the doc comment: `valid` = "the UV Jacobian is usable";
   `widthValid` = "`dpdx`/`dpdy`/`worldWidth` are usable"; state the
   invariant `valid ⇒ widthValid`; state that the fields are in whatever
   frame the record currently lives in and that the object layers promote
   them by the forward map.
2. **`src/Library/Intersection/TextureFootprintCompute.h`**: split the body
   at the `// Solve [dpdu | dpdv] · …` comment into
   `ComputeFootprintVectors( RayIntersectionGeometric&, const Ray& )` (guard
   `!ray.hasDifferentials` only — drop the `derivatives.valid` half; store
   `dpdx`, `dpdy`, `worldWidth`, set `widthValid`) and
   `SolveFootprintUV( RayIntersectionGeometric& )` (guard `!widthValid ||
   !derivatives.valid`; the existing axis pick, `det` guard and four
   assignments; set `valid`). Keep `ComputeTextureFootprint` as a composite
   calling both, so nothing else breaks mid-refactor. Replace the "only
   triangle-mesh geometry calls this" paragraph with the new contract, and
   keep the `ri.range`-not-`ptIntersection` warning verbatim — it is still
   load-bearing at the new call site for the same reason.
3. **`TriangleMeshGeometrySpecializations.h` / `…IndexedSpecializations.h`**:
   delete the `ComputeTextureFootprint( ri, ri.ray );` call and its comment
   block from `RayElementIntersection`.
4. **`src/Library/Objects/Object.cpp`, `Object::IntersectRay`**: inside
   `if( ri.geometric.bHit )`, as the **first** statement (before
   `pUVGenerator` and before `vNormalWorldUnnorm`), add the §3.3 block.
   Comment it with the frame argument: everything read is object-space
   because this function transformed the ray on entry and has not yet
   promoted the normals.
5. **Same file**, the `// WORLD-MEASURE FOLD for txFootprint.worldWidth`
   block: replace with §3.4's exact transform, gated on `widthValid`.
   `m_worldLinearScale` is no longer read here; leave the `scaleHint` /
   `curvature` block above untouched. Rewrite the comment: state that the
   fold is now exact for any linear map, cite the affine line∩plane
   commutation argument, and delete the `scale 4 0.05 4` concession
   (replacing it with a pointer to this doc).
6. **`src/Library/Objects/CSGObject.cpp`**: mirror step 5 at its own
   `// WORLD-MEASURE FOLD for txFootprint.worldWidth` block. Keep the CSG
   nesting comment — the invariant is unchanged, only the per-level operator
   is. No change is needed at `AdoptCsgSurfacePayload` (a verbatim copy still
   works) or at the exit-probe differential transfer.
7. **`src/Library/Painters/ExpressionPainter.cpp`**: both `ctx.fw =
   ri.txFootprint.valid ? …` lines → `widthValid`. Update the comment that
   currently says "triangle-mesh geometry (the only geometry that currently
   populates txFootprint)" — it is the sentence this whole arc invalidates.
8. **`src/Library/Modifiers/ReliefModifier.cpp`**: `if( ri.txFootprint.valid
   && … )` → `widthValid`. Update the step-rule comment.
9. **`src/Library/SceneEditor/PainterPreview.cpp`**, `MakePreviewRi`: set
   `widthValid = true` (leave `valid` as is — the preview has no UV Jacobian
   to offer and `valid = true` there is arguably already wrong).
10. **Do not touch** `TexturePainter::SampleTextured` or `WeaveBRDF` — both
    correctly key on `valid`.
11. **Tests**: `tests/TextureFootprintTest.cpp` per §5; register it in
    `run_all_tests.sh` / `run_all_tests.ps1` and in **all five** build
    projects (`build/make/rise/Filelist`,
    `build/cmake/rise-android/rise_sources.cmake`, the two VS2022 files, the
    Xcode `project.pbxproj`) if the suite convention requires it. Patch the
    hand-stamped `txFootprint` fixtures in `TextureExpressionVMTest`,
    `ReliefModifierTest` and `PainterPreviewTest` to set `widthValid`.
12. **Docs**: correct `RayDifferentials.h`'s propagation-helpers claim (§7);
    amend `RELIEF_MODIFIER_DESIGN.md` §3.3's "the fade is mesh-only" and
    "geometric-mean under-count" paragraphs to point here.

Gate for the arc, per the implementation-review-loop skill: both warning
gates clean on a **clean** rebuild, `TextureFootprintTest`,
`ReliefModifierTest`, `TextureExpressionVMTest`, `PainterPreviewTest`,
`SourceHygieneTest`, `GeometryUVRoundtripTest`, `CsgSurfacePayloadTest` and
`SDFGeometryTest` green, `receding_pier` pixel-identical, then adversarial
review to zero P1s.
