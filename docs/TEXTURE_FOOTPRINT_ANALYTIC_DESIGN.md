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
| F12 | **(updated 2026-09-06)** `fw` reaches `fbm`/`turbulence`/`ridged` **rescaled into that call site's own position-argument domain**: the compiler differentiates the position argument w.r.t. `P` (forward-mode, over the already-emitted postfix code) and folds the Jacobian's largest singular value into a per-call-site multiplier on `fw`, so `fbm(P*40, …)` is filtered at `40·fw`. **Superseded reading:** `fw` used to be consumed *unscaled*, in the same units as the position argument — which made the fade inert for every real body (measured domain scales 7 … 820). The multiplier is exactly `1.0`, an IEEE identity, whenever the domain scale is not provable (an argument built from `u`/`v`/`Po`, or through `%`/`^`/a comparison/any `kFunc`), so an un-analysable body still behaves exactly as it did | `Builder::NoiseFwScale`, `Builder::LinAddSub`, `Builder::JacobianSpectralNorm`; consumed at `ExprProgram::RunAny`'s `kFunc` case → `CallFunc` cases 43–45 |
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
| `SphereGeometry` | **yes**, ungated (`ComputeSurfaceDerivatives`) | no | **(a\*)** full UV + width |
| `EllipsoidGeometry` | yes, ungated | no | **(a\*)** full UV + width |
| `CylinderGeometry` (both hit branches) | yes, ungated | no | **(a\*)** full UV + width |
| `TorusGeometry` | yes, ungated | no | **(a\*)** full UV + width |
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

**(a\*) is CONDITIONAL, and the condition was missed in the first
landing.** `derivatives.valid` is necessary but not sufficient: the four
analytic primitives differentiate their *own* parameters (radians, axial
world coordinates, and with the two axes swapped on the cylinder and the
torus), while `ri.ptCoord` is the normalised `[0, 1]²` chart the matching
`GeometricUtilities::*TextureCoord` emits. Publishing the derivative-chart
Jacobian as though it were the texcoord one is a pure scale-and-transpose
error, and mip LOD's `log2` turns it into whole levels of blur — measured
at +1.65 (sphere), +2.33 (ellipsoid, unequal semi-axes), +1.59 (cylinder,
height 3) and +2.65 (torus) levels too blurry, mesh control unmoved.

Since fix round 1 (§10.8) the geometry must also state a **texcoord chart
map** (`SurfaceDerivatives::dsdu…dtdv` + `texChartValid`, contract in
[GEOMETRY_DERIVATIVES.md](GEOMETRY_DERIVATIVES.md) § "The texcoord chart
map"); `SolveFootprintUV` solves in the derivative chart, multiplies
through, and **declines** — leaving `valid` false, `widthValid` untouched —
when no map is stated. So row **(a\*)** reads: full UV + width *given a
stated chart map*, which all four primitives and the mesh
`useUVJacobian` path now provide, and which the mesh barycentric-edge
fallback deliberately does not.

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
keeps meaning what it means on meshes.

**The `fbm(P*10, …)` mismatch (F12) was out of scope for this arc and has
since been closed separately (2026-09-06).** As shipped here, `fw` went into
the noise unscaled, so the `0.2 / 0.6` band was tested against a world length
while the octaves it gates live in the *argument's* domain — a body scaling by
10 set its Nyquist threshold 10× too low, and the fade never engaged on any
real scene. `Builder::NoiseFwScale` now recovers that domain scale at compile
time and multiplies `fw` by it per call site, so the band means the same thing
in both domains. Nothing in the plan above changes: `worldWidth` is still the
same world-space quantity produced by the same helper, and the rescale happens
strictly downstream of it, inside the expression VM.

**A DIAMETER, so a consumer that wants a symmetric stencil needs ±half**
(fix round 2 — this section did not say so, and one consumer got it
wrong). `relief_modifier`'s auto step is a central-difference HALF-step,
so "difference over one pixel footprint" is `s = fw/2`, not `s = fw`; the
rule shipped as `max(step, fw)` and therefore filtered over TWO
footprints. That is invisible on a self-band-limiting field (which is
every field the mesh-only era exercised) and visible the moment this arc
put a `worley` crackle at footprint scale on a sphere. Corrected in
`ReliefModifier.cpp`'s `RELIEF_AUTO_STEP_FOOTPRINT_FRACTION`; measurements
in §10.9 and in
[RELIEF_MODIFIER_DESIGN.md](RELIEF_MODIFIER_DESIGN.md) §3.3. §6's render
plan below already flagged the risk in its own words — "check the
half-vs-full-pixel-step convention first" — and no one did until a scene
showed it.

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
| `scenes/Tests/Painters/relief_crackle_glaze.RISEscene` | sphere (relief step becomes live; **added fix round 2** — this is the scene the half-vs-full-step question below was settled on) |
| `scenes/Tests/ChunkCoverage/cc_relief_modifier.RISEscene` | sphere (**added fix round 2**; its explicit `step 0.002` is below the footprint, so the footprint now wins — see §10.9) |
| `scenes/Tests/ChunkCoverage/cc_modifier_stack.RISEscene` | sphere (**added fix round 2**; same explicit `step 0.002`, same consequence) |

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

## 10. Implementation record (2026-09-06)

Implemented on branch `relief-followups` from `913436cd`. All four phases
landed; the phase boundaries in §8 were preserved as a review structure but
NOT as commit boundaries, because phases 2 and 3 both edit
`Object::IntersectRay` and a pathspec commit cannot split hunks within one
file — splitting them would have produced a non-building intermediate. The
tree was gated as a whole instead (§10.4).

### 10.1 What landed, against the §9 brief

| Brief item | Where | Note |
|---|---|---|
| 1. `dpdx`/`dpdy`/`widthValid` on `TextureFootprint` | `RayIntersectionGeometric.h`, `struct TextureFootprint` | Doc comment rewritten around the two-flag contract and the `valid ⇒ widthValid` invariant; frame paragraph states the forward-map promotion |
| 2. Split the helper | `TextureFootprintCompute.h`, `ComputeFootprintVectors` / `SolveFootprintUV` / `ComputeTextureFootprint` | The `ri.range`-not-`ptIntersection` warning is kept verbatim; it is still load-bearing at the new call site |
| 3. Delete both mesh call sites | `TriangleMeshGeometrySpecializations.h`, `TriangleMeshGeometryIndexedSpecializations.h` | Their `#include "../Intersection/TextureFootprintCompute.h"` in `TriangleMeshGeometry.cpp` / `TriangleMeshGeometryIndexed.cpp` was moved to `Object.cpp` rather than left dangling |
| 4. One call site at the Object layer | `Object::IntersectRay` | First statement inside `if( ri.geometric.bHit )`, before `pUVGenerator` and before `vNormalWorldUnnorm`; commented with the frame argument |
| 5. Exact fold in `Object.cpp` | `Object::IntersectRay`, the `WORLD-MEASURE PROMOTION` block | `m_worldLinearScale` no longer read there; the `scaleHint`/`curvature` block above untouched |
| 6. Mirror in `CSGObject.cpp` | `CSGObject::IntersectRay` | Nesting invariant restated as composing forward maps; `AdoptCsgSurfacePayload` unchanged (its whole-struct copy already carries `dpdx`/`dpdy`) |
| 7. `ExpressionPainter` ×2 → `widthValid` | `ExpressionPainter::BuildContext`, `ExpressionScalarPainter::BuildContext` | The "the only geometry that currently populates txFootprint" sentence is the one this arc invalidated; replaced |
| 8. `ReliefModifier` → `widthValid` | `ReliefModifier::Modify`, surface-domain branch | Step-rule comment updated |
| 9. `PainterPreview::MakePreviewRi` → `widthValid` | `PainterPreview.cpp` | `valid` left alone, per the brief |
| 10. Do not touch `TexturePainter` / `WeaveBRDF` | — | Untouched; both correctly key on `valid` |
| 11. Tests | `tests/TextureFootprintTest.cpp` (new), `ReliefModifierTest`, `TextureExpressionVMTest` | See §10.2 |
| 12. Docs | `RayDifferentials.h` header, `RELIEF_MODIFIER_DESIGN.md` §2 table + §3.3, this section | See §10.3 |

Two sites beyond the brief, both contract-clarity rather than behaviour:
`DisplacedGeometry.cpp`'s `EvalHeightField` and
`GeometryUtilities.cpp`'s displacement bake both spell out
`txFootprint.valid = false` explicitly (their comments say the contract is
written out rather than inherited from the default ctor); each gained the
matching `widthValid = false`. Both were already false by construction.

**Deliberately NOT changed:** the ~25 `ri2.txFootprint.valid = false`
invalidations in `MappingPainter`, `TexCoord1Painter`,
`StochasticTilePainter` and `ScatterPainter`. Those exist because the
wrapper remaps the UV *domain*, which invalidates the UV Jacobian and
nothing else. The world-space footprint width is unaffected by a UV remap,
so leaving `widthValid` set is both correct and strictly better: an `fbm`
under a `mapping_painter` keeps its octave fade. `TextureExpressionVMTest`
test 39's `valid`-flag assertions therefore stand unchanged.

### 10.2 Tests, and what they cost

`tests/TextureFootprintTest.cpp` is **auto-discovered** — `build/make/rise/
Makefile` builds `$(wildcard ../../../tests/*.cpp)` and `run_all_tests.sh`
enumerates `tests/*.cpp` directly. §9 item 11's "register it in all five
build projects" is over-cautious: `ReliefModifierTest` appears in none of
`run_all_tests.sh`, `run_all_tests.ps1`, `build/make/rise/Filelist`,
`build/cmake/rise-android/rise_sources.cmake`, the two VS2022 files or the
Xcode project. The five-project rule in CLAUDE.md is about `src/Library/`,
not `tests/`. Nothing was registered.

10 tests / 54 checks, all green. Counts on the gate suites moved as
follows: `ReliefModifierTest` 118 → **123** (test 4d, the non-uniform
companion, adds 5); `TextureExpressionVMTest` 685 (unchanged — the
hand-stamped fixtures gained `widthValid = true` and one assertion was
widened to check both flags on a default-constructed record).

Two fixture-level notes worth keeping:

* **Test 3 needed a rotation to stay discriminating.** With the mesh sphere
  on the tessellation lattice the on-axis ray lands exactly on the
  `(0, 0, R)` vertex, the interpolated normal is exactly the analytic one,
  and mesh-vs-analytic agreement came out at rel = 0 — an identity, not a
  measurement. `RotateObjectYAxis( 1.3° )` puts the hit mid-facet; the
  agreement is then rel = 2.2 × 10⁻⁴, comfortably inside the 3% tolerance
  and actually absorbing the mesh's normal-interpolation error.
* **`ClippedPlaneGeometry` could not carry test 5**, for a reason that is
  NOT about this arc — see §10.5.

### 10.3 Red-proofs

1. **The exact fold (§3.4).** Restoring `worldWidth *= m_worldLinearScale`
   in `Object::IntersectRay` (the retired form, gated on `widthValid`),
   rebuilt and run:
   * `TextureFootprintTest` 54/0 → **51 passed, 3 failed** — test 5
     reports `2.42912105383567e-3` against the truth `1.04667653293407e-2`,
     i.e. **4.309× too small**, matching §3.4's predicted 4.31× exactly;
     test 8's CSG composition also breaks (2×), because the Object level
     then applies the geometric mean while the CSG level still applies the
     exact map.
   * `ReliefModifierTest` 123/0 → **121 passed, 2 failed** — test 4d
     reports `0.0464158883361` against `0.2`, the same 4.309×.
   * Test 4 (uniform `scale 10`) and `ReliefModifierTest` 4c stayed
     **green** under the mutation. That is the point of 4d: a uniform
     scale cannot separate the two operators.
   Reverted; both suites back to 54/0 and 123/0.
2. **The call-site hoist (§3.3).** Deleting the
   `ComputeFootprintVectors` / `SolveFootprintUV` block from
   `Object::IntersectRay`: `TextureFootprintTest` 54/0 → **29 passed, 25
   failed**, with every analytic, UV-free and CSG assertion red and the
   mesh-sphere `widthValid` assertion red too (the mesh call sites are
   gone). Reverted; `git diff --stat` confirmed the file was back to the
   intended change only.

### 10.4 Gate

Clean-of-warnings `make -C build/make/rise -j8 all` after touching every
`.cpp` whose translation unit changed (`Object.cpp`, `CSGObject.cpp`,
`ExpressionPainter.cpp`, `ReliefModifier.cpp`, `PainterPreview.cpp`,
`DisplacedGeometry.cpp`, `GeometryUtilities.cpp`,
`TriangleMeshGeometry.cpp`, `TriangleMeshGeometryIndexed.cpp`): **0
warnings**. Test-binary builds: 0 warnings.

| Suite | Result |
|---|---|
| `TextureFootprintTest` | 54 passed, 0 failed |
| `ReliefModifierTest` | 123 passed, 0 failed |
| `TextureExpressionVMTest` | 685 passed, 0 failed |
| `PainterPreviewTest` | 87 passed, 0 failed |
| `SourceHygieneTest` | 164 passed, 0 failed (291 test files scanned) |
| `GeometryUVRoundtripTest` | all passed |
| `CsgSurfacePayloadTest` | 348 passed, 0 failed |
| `SDFGeometryTest` | 685 passed, 0 failed |
| `BilinMipLODTest` | 28 passed, 0 failed |
| `GeometryShadingTangentTest` | 12606 passed, 0 failed |

The Xcode `RISE-GUI` warning gate was **not** run (no Xcode build in this
session); no Objective-C++ or GUI source was touched, and the only header
whose layout changed (`TextureFootprint` gains two `Vector3` and one
`bool`) is consumed identically by both build systems.

### 10.5 Render evidence

Renders are at reduced settings in the session scratchpad
(`…/scratchpad/tfp/`). Because RISE seeds each render from the wall clock,
no two renders of the same scene are bit-identical; every comparison below
is therefore quoted against a same-binary A/A control that bounds the seed
noise.

**`receding_pier` — unchanged, as required (§6's gate).** 640×360,
`samples 4`, `oidn_denoise FALSE`, EXR / `Rec709RGB_Linear`. Four renders:
two on the `913436cd` binary (`pier_A`, `pier_A2`), two after (`pier_B`,
`pier_C`, `pier_D`).

| Pair | mean-luminance rel Δ | worst 8×8 block |
|---|---|---|
| BEFORE vs BEFORE (control) | +0.0001% | 3.79% |
| AFTER vs AFTER (control) | +0.0055% | 6.14% |
| AFTER vs AFTER (control) | −0.0015% | 5.05% |
| **BEFORE vs AFTER** | −0.0016% | 3.50% |
| **BEFORE vs AFTER** | +0.0037% | 4.33% |
| **BEFORE vs AFTER** | +0.0022% | 4.62% |

Every cross-binary delta sits strictly inside the same-binary spread, on
both the mean and the worst-block metric. The pier deck is a
`displaced_geometry`-tessellated mesh at `scale 1`, so the fold changed
from geometric-mean to exact with no numeric effect, and the call-site move
is value-preserving; its `box_geometry` water prop newly gains a footprint
but carries a `uniformcolor_painter`, so it cannot show one.

**`scenes/Tests/Painters/relief_sphere_no_uv.RISEscene` — the analytic
demonstrator.** 256×256, scene's own `samples 16`,
`pathtracing_pel_rasterizer`, `oidn_denoise FALSE` already in the scene. An
`fbm(P*7.0, 5, 0.5, 2.0)` `scalar_painter` driving a `relief_modifier` on an
**analytic sphere** (radius 1, camera at `0 0 4`, fov 30) — the §6 table's
"sphere (relief `max(step, fw)` becomes live)" row. BEFORE was rendered
with a `913436cd`-built binary preserved in the scratchpad; AFTER with the
current `bin/rise`. Two renders each.

High-frequency energy (mean |∇²luminance|) by annulus, background and the
silhouette edge excluded:

| annulus | BEFORE | AFTER | change | A/A control |
|---|---|---|---|---|
| centre `r < 0.23` (face-on) | 6.8173 | 4.5688 | **−33.0%** | −0.2% |
| `r 0.23–0.47` | 7.1239 | 4.8023 | **−32.6%** | +0.1% |
| `r 0.47–0.70` | 8.6504 | 5.8452 | **−32.4%** | −0.1% |
| limb `r 0.70–0.93` (grazing) | 5.6499 | 3.2885 | **−41.8%** | −0.3% |

Read by eye: the large-scale relief structure is preserved and the fine
sparkle is gone, most strongly at the limb. Run-to-run noise also drops
(18.8% of pixels differ between two BEFORE renders, 10.4% between two
AFTERs) — the wider stencil is doing antialiasing, which is the point.

One honest qualification of §6's "far side fades, near field unchanged"
prediction: on THIS scene the near field is not unchanged, and should not
be. At `d − R = 3` the footprint is `3θ ≈ 6.3 × 10⁻³`, already 6× the
relief auto-step floor of `1e-3`, so the step rule goes live over the whole
disc and merely goes *further* live at the limb. The prediction holds in
its intended form — the fade tracks the footprint monotonically — but "near
field unchanged" is only true where the footprint is below the floor, i.e.
much closer than this scene puts the camera.

`scenes/FeatureBased/Textures/oxidized_copper.RISEscene` was also rendered
before/after and is a **poor** demonstrator, recorded here so the next
reader does not repeat it: its `fbm` runs at `patina_freq 1.6` on spheres
~3.4 units away (`fw ≈ 5 × 10⁻³`, far below `OctaveFadeWeightImpl`'s
`lo = 0.2`), and its receding `infiniteplane_geometry` floor — which is
exactly the geometry that would show a horizon fade — carries a
`uniformcolor_painter`. Its before/after difference is not separable from
seed noise.

### 10.6 Deviations, and one bug found in passing

* **Phase boundaries were not commit boundaries** — see the preamble.
* **§9 item 11's build-project registration was skipped**, deliberately —
  see §10.2.
* **§5's test 5 uses a `circular_disk`, not a `clipped_plane`.** Not a
  preference: **`ClippedPlaneGeometry` misses any ray travelling exactly
  along ±Y.** `RayBilinearPatchIntersection` uses the Ramsey–Potter–Hansen
  elimination hard-coded on the ray direction's *z* component
  (`A1 = ax*qz − az*qx`, and the same for B/C/D). When `dir.x == 0` **and**
  `dir.z == 0`, every one of `A1`, `B1`, `C1`, `D1` is identically zero, so
  all three quadratic coefficients vanish, `SolveQuadricWithinRange`
  returns no roots, and the patch is missed. Measured: a ray from
  `(0, 10, 0)` along `(0, −1, 0)` misses a 10×10 quad centred on the origin
  in `y = 0`; tilting the direction by 0.01 makes it hit at `t = 10`. This
  affects `clipped_plane` and `bilinear_patch` (and the disk/plane
  geometries only insofar as they route through the same helper — they do
  not). It is a **pre-existing bug, not introduced here**, and it is out of
  scope; the fix is the PBRT-style largest-component axis pick instead of a
  hard-coded `z`. Test 5 therefore uses a `CircularDiskGeometry` (a plain
  plane test) so its 1e-9 exactness assertion is not entangled with it, and
  test 6 covers `clipped_plane`'s footprint on a deliberately tilted ray,
  with the reason written at the fixture.

  **FIXED 2026-09-06, commit `14e0f45d`.** `RayBilinearPatchIntersection`
  now picks the elimination axis `w` as the largest `|q|` component — the
  same choice `computet` in that file already made for the `t` recovery —
  and forms `A1`/`A2` (and `B`/`C`/`D`) from the other two axes crossed
  with it, taken cyclically so that `w = z` reproduces the reference
  algebra textually and nothing changes for the rays that already worked.
  The degeneracy was rank-deficiency, not round-off, so per
  [precision-fix-the-formulation](skills/precision-fix-the-formulation.md)
  no threshold was involved on either side of the fix: with `q.z == 0` the
  two eliminated equations are literally the same equation up to scale, and
  no epsilon rescues a rank-1 system. The `t` recovery, the `u`/`v` range
  checks, the scale-relative self-hit floor (debt 21) and
  `SolveQuadricWithinRange`'s `a == 0` linear branch — which the
  parallelogram case still takes — are all untouched. Every caller
  benefits: `ClippedPlaneGeometry` (both entry points) and
  `BilinearPatchGeometry`. (`RayTriangleIntersectionWithDisplacement`, a 2004 stub that also called the helper, never compiled and was never invoked; it was deleted 2026-09-06 together with its VS2022 project entries and header declaration.)

  Guarded by `GeometryUVRoundtripTest::TestBilinearEliminationAxis`: 12
  axis-aligned closed-form cases across all three elimination branches and
  both signs (six on the raw patch, six through `clippedplane_geometry`),
  19 rays at curved patches checked against a brute-force grid + 3×3 Newton
  oracle sharing no code with the analytic solver, and 1000 random
  directions asserting both the on-ray invariant and agreement **to
  FP-contraction noise** (tolerance 1e-12; measured max delta 8.9 × 10⁻¹⁶)
  with a verbatim copy of the pre-fix fixed-`z` solver wherever `|q.z|` is
  dominant — bit equality is deliberately not asserted, because the test
  necessarily carries its own copy of the solver and `-ffast-math` + LTO
  are free to fuse the multiply-subtract pairs differently in each. That
  copy also serves as an in-test oracle asserting the
  pre-fix solver misses **iff** `q.z == 0`, so the guard is discriminating
  by construction. Red-proof: forcing the axis back to a hard-coded `w = 2`
  turns the suite red with 22 failed assertions (exactly the ±X and ±Y
  rays, not one ±Z ray), and an `orthographic_camera` at `(0, 6, 0)`
  looking down at a 3×3 `clipped_plane` in `y = 0` renders a **fully black
  frame** before the fix and the lit quad after — the cleanest rendered
  demonstration, because an orthographic camera makes *every* primary ray
  exactly `(0, −1, 0)` where a pinhole only degenerates on the single
  centre row.

  Sibling audit ([audit-by-bug-pattern](skills/audit-by-bug-pattern.md)):
  the other two routines that reduce a ray/surface system to 2D were
  already direction- or normal-adaptive and needed no change —
  `RayBezierPatchIntersection`'s `MakePlanes` picks its helper axis as the
  *smallest* `|Dir|` component, and `GeometricUtilities::BilinearInverse`
  picks its axis pair from the patch normal at the centre.  `computet` and
  `RayDistanceToPoint` were already largest-`|Dir|`.  The per-axis DDA
  walks (`HeterogeneousMedium`, `MajorantGrid`) treat all three axes
  symmetrically behind their own zero guards.

  **Fix round 1 on the axis-pick landing (2026-09-06).** Two reviewers on
  `14e0f45d` returned no P1 correctness findings on the axis pick itself
  and one P1 on this document's prose; the round below closes both, plus
  two latent bugs the review surfaced in the code the fix newly reaches.

  | Finding | Where | Commit |
  |---|---|---|
  | `SolveQuadricWithinRange`'s exact double root returned `-b/a`, not `-b/(2a)` — and, on audit, `SolveQuadric`'s two-root branch multiplied by `0.5 * a` where it wanted `0.5 / a` | `src/Library/Functions/Polynomial.cpp` | `500f9708` |
  | A root that satisfies neither eliminated row was accepted without anyone checking it lies on the ray — phantom hits for a ray exactly parallel to a planar, **non-parallelogram** patch | `src/Library/Intersection/RayBilinearPatchIntersection.cpp` | `a3db90ce` |
  | Prose P1: "BIT FOR BIT" / "bit-level agreement" contradicted the test's own 1e-12 tolerance; the "part (c) stays green BY CONSTRUCTION" claim was wrong; `RayTriangleIntersectionWithDisplacement` named as a caller that benefits | this file §10.6, `tests/GeometryUVRoundtripTest.cpp` | (this commit) |

  The reviewers' measurement of the axis-pick landing itself, for the
  record: the fixed solver finds **3842** true hits on planar patches and
  **9356** on general ones that the hard-coded-`z` code missed, and loses
  **0** true hits — the change is strictly additive on the rays it was
  meant to reach. The phantom class the third row above closes is a
  *subset* of what the axis pick newly reaches, which is why it appeared
  only now: the pre-fix code missed those rays by accident, for the same
  rank-deficiency reason it missed the genuine ones.

  Behavioural note worth stating plainly: with the residual gate in
  place, a `clipped_plane` / `bilinear_patch` now correctly **occludes**
  rays travelling in what used to be its blind plane. Before the axis
  pick those rays passed straight through it; between the axis pick and
  the gate a non-parallelogram quad could stop them at a point off its
  own plane; now they are stopped only where the quad actually is.

  The gate is a residual check, not a threshold widening
  ([precision-fix-the-formulation](skills/precision-fix-the-formulation.md)):
  it reconstructs `P(u,v)`, differences it against `origin + t*q`, and
  compares against `NEARZERO * (1 + coordScale + |t|·|q|₁)` — the
  scale-relative idiom `GeometricUtilities::BilinearInverse` and the debt
  21 self-hit floor in the same function already use. Guarded by
  `GeometryUVRoundtripTest` part (d),
  `TestBilinearOffRayRootRejection`: the reviewer's exact reproduction as
  both a raw patch and a `clippedplane_geometry`, a companion crossing
  ray that must still hit at the closed-form `(u, v, t)`, and a seeded
  200k randomized sweep on dyadic coordinates across three decades of
  world scale (×1, ×256, ×65536, so an absolute epsilon would fail at one
  end or the other) — 85156 constructed-to-hit rays found with **0 lost**
  and 331 brute-force-oracle cross-checks agreeing, and 98779
  cannot-possibly-hit rays with **0 phantoms**. Red-proof: making
  `RootLiesOnRay` return true unconditionally turns the suite red with
  three failures, the reviewer's case reporting the point `(−1, 0.333, 1)`
  a full unit off the patch's plane and the sweep finding 41 phantoms
  with a worst off-ray distance of 245760.
* **The §7 residual list is unchanged and still accurate**: primary rays
  only, shading-vs-geometric normal, no grazing clamp. Test 10 measures the
  grazing case rather than clamping it, per §4 — every hit from the optical
  axis out to the silhouette reports a finite, non-negative `worldWidth`.

  **P2 closure round (2026-09-06).** Six review P2s against this section
  and its neighbouring code, none behavioural except one:

  * The `SolveQuadricWithinRange` `d == 0` branch comment's "only in-tree
    consumer" claim (right above, and in `src/Library/Functions/
    Polynomial.cpp`) was false: `GeometricUtilities::BilinearInverse`
    reaches the same branch too — its caller is
    `ClippedPlaneGeometry::ComputeSurfaceDerivatives`. Comment corrected
    with the measured split: of 400000 dyadic on-surface inversions, 2311
    land exactly on the double-root branch, and 1007 of those the pre-fix
    `-b/a` silently turned into a false reject (the doubled root falling
    outside `BilinearInverse`'s `[-1e-4, 1+1e-4]` acceptance window). New
    test `GeometryUVRoundtripTest::TestBilinearInverseExactDoubleRoot`
    constructs a dyadic patch/point (`c00=(0,0,0) c10=(1,0,0)
    c11=(-0.5,0,0) c01=(0,1,0)`, `P = BilinearForward(u=0.25, v=0.5)`)
    whose reduced quadratic hits `d == 0.0` bit-exact — verified against a
    scratch harness linked against `bin/librise.a` before the test was
    written. Red-proofed by hand: reverting to `-b/a` and rebuilding turns
    it red (`u=0 v=0` instead of the true `0.25/0.5`), nothing else in the
    suite regresses; reverted back before committing. Commit `264c988a`
    (comment), `bfbcd001` (test).
  * This section's dead-caller sentence about
    `RayTriangleIntersectionWithDisplacement` was ALSO wrong: it was
    registered in `build/VS2022/Library/Library.vcxproj` (+ `.filters`) but
    absent from Filelist / the Android CMake list / Xcode. Resolved by
    DELETING the stub (uncompilable, uncalled) and its project entries and
    extern declaration on 2026-09-06 (branch cleanup-displacement-stub).

  Gate: zero warnings on a clean `make -C build/make/rise -j8 all`;
  `PolynomialTest`, `GeometryUVRoundtripTest` (85156 hit / 0 lost, 98779
  miss / 0 phantoms, plus the new exact-double-root case recovering
  `u=0.25 v=0.5`), `PrimitiveSelfHitTest` (45/45), `GeometrySurfaceDerivativesTest`,
  `TextureFootprintTest` (118/118), `ClippedPlaneGeometryTest`,
  `SourceHygieneTest` (164/164) all pass.

### 10.7 Self-audit

The five failure modes this edit was most likely to have, and what rules
each one out.

1. **Wrong frame for `dpdx` at the Object call site.** The block is the
   *first* statement inside `if( ri.geometric.bHit )`. `Object::IntersectRay`
   transformed `ri.geometric.ray` — origin, direction *and* differentials —
   into object space on entry, the geometry wrote `ri.geometric.range` and
   `ri.geometric.vNormal` in that same frame, and nothing has run since. One
   statement later `vNormalWorldUnnorm` moves the normal to world and the
   three inputs would disagree. Any such mismatch shows up immediately as a
   non-integer promotion ratio; `TextureFootprintTest` 4 measures exactly
   10, 5 measures rel 1.7 × 10⁻¹⁶, and 8 measures exactly 3.
2. **Double-promotion under CSG.** `CSGObject::IntersectRay` is a separate
   function from `Object::IntersectRay`, so the new call site does not fire
   for a CSG composite; the footprint is produced once, by the operand's own
   `Object::IntersectRay`, promoted once into the CSG's local frame there,
   copied verbatim by `AdoptCsgSurfacePayload`, and promoted once more by the
   CSG level. `TextureFootprintTest` 8 pins both halves, and it went red
   under red-proof 1 (2×), which is what makes it discriminating rather than
   merely green. The exit-probe path
   (`AdoptCsgSurfacePayloadViaProbe` → `operand->IntersectRay`) follows the
   identical route and is covered by `CsgSurfacePayloadTest`'s 348 checks.
   No geometry in `src/Library/Geometry/` calls an `IObject`'s
   `IntersectRay`, so there is no third nesting layer to double-count.
3. **`valid` set on a UV-free hit.** `SolveFootprintUV` early-returns unless
   `ri.derivatives.valid`, and separately unless the det guard passes.
   `TextureFootprintTest` 6 asserts `valid == false` on `box_geometry`,
   `sdf_geometry` and `clipped_plane` while asserting `widthValid` and a
   positive width on each — the assertion pair that would catch a widened
   flag. `TexturePainter::SampleTextured` and `WeaveBRDF` are untouched.
4. **The plane-intersection degenerate at grazing incidence.** The
   `|den| < 1e-20` guard is unchanged; it now leaves `widthValid` false where
   it used to leave `valid` false, with `worldWidth` still 0 either way, so
   the fallback behaviour is identical. `TextureFootprintTest` 10 walks the
   optical axis out to the silhouette one pixel at a time and asserts a
   finite, non-negative `worldWidth` at every hit (via `RISE::IsFiniteDouble`,
   not a fast-math-foldable self-comparison). No clamp was added: §4 defers
   it, because any clamp in the shared helper would move mesh pixels.
5. **Mesh values changed by moving the call site.** Checked three ways.
   (a) The winning candidate's `ri.vNormal` / `ri.range` at the Object layer
   are exactly what the old per-candidate call last saw — the mesh
   intersectors use a strict closest-hit guard, so the last *accepted*
   candidate is the final one. (b) `derivatives.valid = true` is set
   **unconditionally** on the mesh hit path — `useUVJacobian` only selects
   between the UV and the barycentric (A, B) parameterisation, it does not
   gate the flag — so there is no mesh hit that used to early-out of
   `ComputeTextureFootprint` and now gets a width it did not have. (c)
   Empirically: `ReliefModifierTest` 4c still reads exactly 10,
   `TextureExpressionVMTest` is unchanged at 685, and `receding_pier`'s
   before/after difference is strictly inside its same-binary seed noise
   (§10.5).

### 10.8 Fix round 1 (2026-09-06)

Reviewers on the landing above found one P1 in the code and three stale
"mesh-only" claims in prose that this arc had itself invalidated.

#### The P1: the analytic primitives' Jacobian was in the wrong chart

`SolveFootprintUV` began setting `valid` on sphere / ellipsoid / cylinder
/ torus because they populate `ri.derivatives`. But `derivatives.valid`
turned out to be **necessary and not sufficient**: those primitives
differentiate their own natural parameters, while `ri.ptCoord` is the
normalised chart their `GeometricUtilities::*TextureCoord` emits, and
`TexturePainter::SampleTextured` / `WeaveBRDF` combine the two as ONE
chart. See §2.1's `(a*)` note for the statement of the bug and
[GEOMETRY_DERIVATIVES.md](GEOMETRY_DERIVATIVES.md) § "The texcoord chart
map" for the contract that fixes it.

Fixed at the root — the geometry now **states** the map — rather than by
special-casing four primitives inside the footprint helper. Rationale:
the helper cannot derive the map (it sees neither the geometry class nor
its TextureCoord function), a per-primitive `if` chain in a shared header
would rot the moment a fifth primitive publishes derivatives, and the
same knowledge is exactly what a future consumer of `dpdu` + `ptCoord`
(anisotropic filtering, UV-space differentials for SMS) will need.
`dpdu`/`dpdv` themselves are untouched, as the brief required — SMS's
`ManifoldSolver` and the curvature code keep consuming them as-is.

The sibling this surfaced, which the original §10.7 self-audit item 5(b)
got backwards: it argued that because `derivatives.valid` is set
**unconditionally** on the mesh hit path, no mesh hit could lose a
footprint. True for `widthValid`, but it also meant the
**barycentric-edge fallback** (`useUVJacobian == false`: a degenerate or
absent UV triangle) was publishing a Jacobian in an *edge* chart as
though it were a texcoord one — the same class of bug as the analytic
primitives, pre-existing and unnoticed. That path now states no chart map
and `SolveFootprintUV` declines. The width half is unaffected on every
path.

Measured LOD error against the finite-difference oracle, before (chart
map forced to identity, i.e. the shipped behaviour of §10) and after:

| Geometry | before | after |
|---|---|---|
| mesh sphere (control) | −0.000018 | −0.000018 |
| analytic sphere | **+1.651** | −0.0000011 |
| analytic ellipsoid (1, 1.6, 0.7) | **+2.330** | +0.00000008 |
| analytic cylinder, x / y / z (h = 3) | **+1.585** | +0.0000008 |
| analytic cylinder, +y end cap | 0.000 | +0.000056 |
| analytic torus (R = 2, r = 0.6) | **+2.651** | −0.00082 |

Positive = too blurry. The end cap is the instructive row: its error is a
**transposition**, not a mis-scale, so LOD — which takes a max over two
row norms — is blind to it while the component-wise oracle reads
`rel = 1.0`. Anisotropic filtering and `Mode_Supersample` are not blind
to it, which is why the oracle compares components rather than the LOD
scalar the fix was motivated by.

`TextureFootprintTest` grew from 10 tests / 54 checks to 12 / 108: test
11 is the oracle (a **central** difference of `ptCoord` across the pixel's
own ray differentials — central, not forward, because a forward
difference on a curved primitive carries an O(h·curvature) bias of the
same order as the 1e-3 tolerance), test 12 pins the paths that must
publish nothing (degenerate-UV mesh, box) plus the sphere's pole and its
−X seam (decline or finite, never NaN). Red-proof: forcing the map to the
identity turns 7 of test 11's checks red, in the magnitudes tabulated
above, with the mesh control unmoved — which is what proves the change is
a genuine no-op for meshes.

#### The three stale claims

All three said "mesh-only" about a footprint that §10 had just made
universal:

| Where | Was | Now |
|---|---|---|
| `Object::m_worldLinearScale` doc comment | "Also folded into `txFootprint.worldWidth` … since the relief-modifier arc" | It is **not**; §3.4 replaced that fold with the exact forward map. Clause removed, and the comment now says why the geometric mean was retired (4.31× on a `scale 4 0.05 4` panel) |
| `relief_modifier`'s `step` descriptor | "Only TRIANGLE-MESH geometry populates one today … with NO distance fade" | Every geometry populates one; the real restriction is **primary hits only**, since no ray carries screen-space differentials after a scatter |
| `skills/agent/procedural-textures.md`, the `fw` builtin and the relief `step` section | "real on primary hits against mesh geometry, 0.0 … on non-mesh geometry"; "**Mesh-only, though**" | Same correction, twice; the surviving caveat is the secondary-bounce one |

A sweep for the same claim shape across `src`, `skills` and `docs` found
no other live instance — the remaining hits are historical rows in this
document and in `RELIEF_MODIFIER_DESIGN.md` §12 (correctly past-tense),
`observe-modes.md`'s wireframe limitation (a different, real, mesh-only
thing), and unrelated glTF / BVH / VCM prose.

#### P2/P3 also cleared

* `tests/CstRecordDeriveTest.cpp`'s `displacement:` comment said
  `{Painter}`-declared; `904cd326` made it `{Painter, Function}` +
  `ParameterPipe::Function2D`, so the comment now names the pipe.
* `Cst.cpp`'s `FunctionSubNamespace` doc comment described a null-`pd`
  caller. There is exactly one caller and it dereferences `pd` before
  calling, so the wording now says the `pd &&` guard is defensive only.
* Fixtures that set `txFootprint.valid = true` without `widthValid`
  violated the record's own invariant in-fixture:
  `TextureExpressionVMTest` (6 sites — the mapping / triplanar / world /
  object / stochastic-tile / scatter wrapper cases) and
  `TexCoord1PainterTest::MakeRi` now set both, the latter tracking its
  `footprintValid` parameter so the false case stays genuinely
  footprint-free.

#### Commits

| Commit | What |
|---|---|
| `5bc02109` | the chart map: contract on `SurfaceDerivatives` / `SurfaceDerivativesInfo`, per-primitive maps, `SolveFootprintUV` change of chart, tests 11–12 |
| *(this record)* | the three stale claims, the P2/P3 comment and fixture fixes, `GEOMETRY_DERIVATIVES.md` § "The texcoord chart map", §2.1's `(a*)`, and this section |

### 10.9 Fix round 2 (2026-09-06)

Review round 2 on branch `relief-followups`. One correctness finding
(the step fraction), one numbering/citation sweep, one new oracle row.

**(A) The auto step spanned TWO footprints, not one.** `ReliefModifier`'s
surface-domain rule shipped as `s = max(step, fw)` while `s` is the
*half*-step and the difference spans `2s`. Nothing in the mesh-only era
could show it: the only in-tree scene exercising the rule
(`receding_pier`) drives it with `fbm`, which band-limits itself against
the same `fw`, so the extra factor of two lands on octaves already faded
to zero. §3.4 of this document made `fw` real on analytic primitives, and
`relief_crackle_glaze` — a `worley_f2f1` crackle, no octave structure, on
a sphere — put a *non*-self-fading field at footprint scale under the
rule for the first time.

Measured at reduced settings, high-frequency shading energy as
mean `|∇²luminance|` over the subject, against the pre-footprint renders
preserved in the session scratchpad. Renders seed from the wall clock, so
each row is quoted against a same-binary control:

| scene / region | pre-footprint | `s = max(step, fw)` | `s = max(step, fw/2)` | control spread |
|---|---|---|---|---|
| `relief_crackle_glaze`, sphere disc (256², 32 spp) | 41.50 | 33.21 (**−20.0 %**) | 40.97 (**−1.3 %**) | 0.1 % |
| `weathered_workbench`, bench top (320×240, 64 spp) | 26.47 | 22.71 (**−14.2 %**) | 23.67 (**−10.6 %**) | 0.1 % |

And the fade the rule exists for, on the design's own demonstrator
`relief_sphere_no_uv` (the §10.5 measurement re-run; the full-footprint
column reproduces §10.5 to three digits, which is the check that the
harness is the same one):

| annulus | vs pre-footprint, `fw` | vs pre-footprint, `fw/2` | A/A control |
|---|---|---|---|
| centre `r < 0.23` | −32.9 % | −10.8 % | −0.1 % |
| `r 0.23–0.47` | −32.7 % | −10.8 % | −0.0 % |
| `r 0.47–0.70` | −32.6 % | −10.9 % | +0.2 % |
| limb `r 0.70–0.93` | −41.8 % | −16.7 % | −0.1 % |

The fade is a factor, not a threshold, so halving the step scales it
rather than switching it off, and it stays monotone in the footprint
(the limb still fades hardest). **Adopted `fw/2`**, as
`RELIEF_AUTO_STEP_FOOTPRINT_FRACTION` in `ReliefModifier.cpp`.

The alternative the review offered — keep `max(step, fw)` and set an
explicit `step` on the two showcase scenes — was **measured and is not
available**. `step` is a FLOOR inside a `max`, so a value below the
footprint is inert: `relief_crackle_glaze` rendered with `step 0.002`
spelled out is indistinguishable from `step 0` (mean 8-bit channel delta
0.233 over the frame, versus 2.309 between the two step rules, and the
scene's own seed noise is the former). That is by design — there is no
way to ask for a sub-footprint stencil on a primary hit — but it does mean
the fraction was the only thing that could be fixed.

Honest residual: on `weathered_workbench` the halving recovers about a
quarter of the lost grain energy, not all of it. The bench top's grain
features are wider relative to `fw` than the crackle's fissures, so the
correct answer there genuinely includes *some* footprint averaging; the
render is legibly crisper than at `fw` and softer than pre-footprint, and
that is the expected shape of an antialiasing rule that is now doing what
its own sentence says. Renders:
`…/scratchpad/r2/{cg_a,cg_b,cg_c_step0002,wb_a,wb_b,sph_a,sph_b}.png`,
zooms `z_cg_*`, `z2_cg_*`, `z_wb_*`.

**(B) Numbering and citations.**

| Finding | Fix |
|---|---|
| §10.6 appeared **twice** — "Deviations" and "Fix round 1" | Fix round 1 renumbered to **§10.8**; the two references to it (§2's chart-map sentence, `RELIEF_MODIFIER_DESIGN.md` §12's arc table) follow |
| "§10.5 self-audit item 5(b)" — the self-audit is §10.7 | Corrected |
| `RayIntersectionGeometric.h`'s chart-map comment said a cylinder mipped "1.02 levels" too blurry (and a sphere 1.67, a torus 2.64), disagreeing with this document's 1.59 / 1.65 / 2.65 | **Re-measured** by replacing `SolveFootprintUV`'s chart multiply-through with the raw derivative-chart differentials and reading `TextureFootprintTest` test 11's own per-geometry LOD error: sphere **1.651**, ellipsoid **2.330**, cylinder **1.585**, torus **2.651**, mesh control unmoved at 1.8e-5. This document was right; the header comment and the same figures echoed in `TextureFootprintTest.cpp`'s file header were corrected to match, both with a note saying they were re-measured |
| "the `−axis` cap" is the cylinder's transposed branch — in `GEOMETRY_DERIVATIVES.md`'s table, `TextureFootprintTest.cpp`'s cap-case comment, and `CylinderGeometry.cpp`'s own comment at the swap site | **Backwards for one axis of three.** The swap fires where the outward normal opposes `dpdu × dpdv`, and the `(ra, rb)` pairs are not consistently cyclic (`x → (y, z)`, `y → (x, z)`, `z → (x, y)`; the cyclic choice for `y` would be `(z, x)`). So `dpdu × dpdv` is `+X`, `−Y`, `+Z` and the transposed cap is `−x`, **`+y`**, `−z`. Measured by instrumenting the swap site and driving all six caps through the chart oracle. All three sites corrected, and `CylinderGeometry.cpp` now carries the per-axis table. Incidentally the oracle's existing cap case is the `+y` cylinder, so the swapped branch was under test all along — the comment was the only thing wrong |

**(C) A mirrored-UV mesh row for test 11.** The mesh path's `dtdv = −1`
branch (right-handedness fix-up negates `dpdv`, the chart map has to say
so) was reachable in production by any mirrored-UV asset and covered by
nothing: the oracle's mesh control never fires it, and no analytic
primitive can. `BuildMeshSphere` gained a `mirrorU` flag that writes the
`u` texcoord backwards while leaving geometry and winding alone, and test
11 gained a row for it. Red-proof: deleting `ri.derivatives.dtdv = −1.0;`
from **both** `TriangleMeshGeometry{,Indexed}Specializations.h` leaves the
control and all six analytic rows green and fails the new row alone, at a
scale-relative error of exactly **2.0**.

**Scenes added to §6's list**, all newly footprint-bearing on a sphere:
`relief_crackle_glaze` (the scene (A) was settled on) and the two
chunk-coverage fixtures `cc_relief_modifier` / `cc_modifier_stack`, whose
explicit `step 0.002` is below the footprint and is therefore now
overridden by it.

**Gate.** `make -C build/make/rise -j8 all` warning-free from clean;
`TextureFootprintTest` 113/113, `ReliefModifierTest` 127/127,
`BlenderBridgeHairTest` 133 checks / 0 failures, `TextureExpressionVMTest`
685/685, `SourceHygieneTest` 164/164, `CstDeriveGoldenTest` 442 MATCH /
1 DRIFT (`bdpt_crystal_garden`, pre-existing and out of scope — no golden
regeneration was needed, since only comments changed in any scene).

| Commit | What |
|---|---|
| *(this record)* | the step fraction, the renumbering, the three re-measured/corrected claims, the mirrored-UV oracle row, and this section |

**(D) P2 follow-up (2026-09-06): the identity branch was unpinned.** (B)'s
per-axis table means the cap chart map has two live branches — the
handedness swap, and the pre-swap identity `dsdu=1, dsdv=0, dtdu=0,
dtdv=1` assignment — but test 11's only cap row (the `+y` cylinder) takes
the swapped branch, per (B) above. Test 11 gained a second cap row,
`"analytic cylinder (+z end cap)"` (a `z`-axis capped cylinder viewed
straight down its axis, which per the same per-axis table takes the
un-swapped identity branch), red-proofed by transposing the pre-`if`
identity assignment (the new row failed at scale-relative error 1.000,
the existing `+y` row stayed green; reverted). `TextureFootprintTest` is
now 118/118.
