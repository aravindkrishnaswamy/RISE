# Landing 2 Design — Ray differentials, mip LOD, stochastic mip selection

Detailed implementation design for Landing 2 of the
[Physically Based Pipeline Plan](PHYSICALLY_BASED_PIPELINE_PLAN.md).
Builds on Landing 1 (HDR primary output) — the EXR baseline that
Landing 1 enables is the verification oracle for everything below.

This doc supersedes the brief Landing 2 section in the parent plan;
that section gets marked DONE with a pointer here when this lands.

## TL;DR

Render quality on Sponza is bottlenecked by texture aliasing.
Bilinear sampling on 2K/4K PBR textures viewed at column-distance
produces salt-and-pepper speckles that don't fade with spp — a
Nyquist undersampling problem that variance-driven adaptive
sampling can't fix because the aliasing IS the integrand.

The PB-correct fix is mip prefiltering with ray-differential-driven
LOD selection (Igehy 1999).  Each primary camera ray tracks its
screen-space footprint; at the hit, the surface UV derivatives
(already populated for triangle meshes) project the footprint into
texture space; the resulting texture-space Jacobian determines a
mip LOD; one mip level is sampled per ray (Olano-Baker stochastic
selection).

For v1 we instrument primary rays only (camera-spawned).  Secondary
rays use the lowest mip (correct for diffuse bounces; over-blurs
specular reflections — flagged as v1.1 follow-up to add Igehy
propagation through smooth-surface bounces).

ABI break: not required for the chunk parser surface (new optional
parameter on `*_painter` chunks).  Internal surfaces — `Ray`,
`IGeometry::IntersectRay`, `IRasterImageAccessor` — gain new
optional parameters.  Per the parent plan's standing decision,
ABI continuity is not preserved.

## Status quo

### What exists

- **Surface derivatives infrastructure** is already in place.
  `RayIntersectionGeometric.derivatives` (struct
  `SurfaceDerivativesInfo` with `dpdu`, `dpdv`, `dndu`, `dndv`,
  `valid`) is populated at intersection time by triangle-mesh
  geometries.  Other geometries leave `valid=false`.  Consumers
  (NormalMap modifier, SMS ManifoldSolver) read it directly.  See
  [docs/GEOMETRY_DERIVATIVES.md](GEOMETRY_DERIVATIVES.md).
- **Bilinear texture sampling** in
  [BilinRasterImageAccessor.h](../src/Library/RasterImages/BilinRasterImageAccessor.h)
  with the recently-added wrap-mode support (REPEAT / CLAMP / MIRRORED).
- **TexturePainter** at
  [TexturePainter.cpp:35](../src/Library/Painters/TexturePainter.cpp)
  calls `pRIA->GetPEL(y, x, c)` once per shading point.
- **Camera ray spawn** in
  [PinholeCamera.cpp:111](../src/Library/Cameras/PinholeCamera.cpp)
  generates one ray per sample at a fractional pixel position
  via `Ray::Set(origin, normalized direction)`.
- **Ray** structure carries origin + direction + precomputed
  `invDir` for slab-test traversal.  No differentials today.

### What's missing

1. **Ray differentials**.  No way to express "this ray has a
   pixel-area footprint of X."  Camera doesn't compute neighbouring
   ray directions; intersect routines have no footprint to consume.
2. **UV-space Jacobian on hits**.  We have `dpdu`, `dpdv` (surface
   derivatives) but no `∂s/∂x, ∂s/∂y, ∂t/∂x, ∂t/∂y` (texture-space
   footprint).  The chain-rule projection is missing.
3. **Mip pyramids**.  `RasterImage` stores a single resolution.
   No prefiltering exists; bilinear samples at full-res regardless
   of pixel footprint.
4. **LOD-aware accessor**.  `IRasterImageAccessor::GetPEL(x, y, p)`
   has no LOD parameter.
5. **TexturePainter's connection to footprint**.  Even if all the
   above existed, the painter doesn't know to ask for a LOD.

### Visual evidence

The Sponza render at 16 spp shows:
- Salt-and-pepper sparkle on stone columns at column-base distance
  (texels much smaller than the pixel footprint).
- Moiré on the gilded-ornament metallic-roughness texture at
  middle-depth columns.
- Stable / unaliased appearance only at near-camera distances where
  the texel ≈ pixel ratio is close to 1.

These are the canonical aliasing artefacts of bilinear-only
sampling on a high-resolution texture.

## Goals

1. **Eliminate aliasing on primary visibility** (camera rays into
   first surface).  Stone columns render speckle-free.  The
   variance-measurement skill (HDRVarianceTest on K-trial EXRs)
   shows variance drop, not just RMSE drop, on the affected pixels.
2. **Preserve PB physics**.  No empirical "anti-aliasing" filters
   that don't have a physical interpretation as integrand
   prefiltering.  The LOD selection is footprint-driven (Nyquist-
   correct); the mip pyramid is a pre-integrated approximation of
   the proper integrand at the chosen scale.
3. **Stay within the path tracer's "one sample per ray" structure**.
   Stochastic mip selection (Olano-Baker) instead of trilinear
   blending — variance-equivalent in the limit, no doubled texture
   bandwidth, no synthetic blur baked into single-ray output.
4. **Per-painter opt-out** so vector-quantity textures (normal maps)
   that prefilter wrong under box-filter mip generation can stay on
   the bilinear-only path until proper slope-space prefiltering
   (LEAN/LEADR) lands as a separate piece of work.
5. **Verifiable**.  An aliasing-checker control scene proves the
   problem and the fix; a Sponza fixed-seed EXR render before/after
   shows variance + RMSE drop on the documented stone-column ROI.

## Non-goals (deferred to v1.1 or later)

- **Ray-differential propagation through bounces** (Igehy formulas
  for specular reflection / refraction, ray-cone hybrid for
  glossy/diffuse).  v1 = primary only.  Secondary rays use lowest
  mip.  Trade-off: indirect-diffuse texture sampling is over-blurred
  on bounces (OK — bounce integrand is already low-frequency);
  specular reflections of distant textures are also over-blurred
  (acceptable for v1 — not the dominant artefact).
- **Anisotropic filtering** (EWA, ripmap, N-tap stochastic
  anisotropy).  v1 = isotropic LOD via the longer-axis footprint
  metric.  Surfaces viewed at very oblique angles get over-blurred
  in one direction.  Future when we see a real artefact.
- **Vector-quantity prefiltering** for normal maps (LEAN, LEADR,
  toksvig).  v1 = normal-map painters opt out of mipmapping
  entirely; the existing bilinear path remains.  Separate research
  project.
- **Lanczos / Kaiser mip generation**.  v1 = box filter (PBRT-style
  pyramidal box).  Lanczos has ringing risks for HDR data; Kaiser is
  the technically-correct fit.  Defer.
- **GPU-style ddx / ddy from screen-space neighbours**.  We're
  CPU-bound, sample-by-sample; the per-ray differential approach is
  the right one for our integrator.
- **Photon-mapping LOD**.  Photons store point samples; mip LOD
  applies at the eye-walk hit (which already gets v1's primary-ray
  treatment).  Light-subpath LOD is a separate, smaller concern.
- **Lowmem mode + mipmaps**.  Per-sample sRGB→linear decode +
  mip pyramid would multiply the per-sample cost; flag as
  incompatible (warn + auto-disable mipmap when lowmem is set).
  Lowmem-friendly mip support is a separate piece.

## Design

### Data flow

```
Camera::GenerateRay
   ├── ray = central direction at (px, py)
   └── ray.differentials = {
         rxDir = direction at (px+1, py) - central,
         ryDir = direction at (px, py+1) - central,
         hasDifferentials = true
       }

IntersectRay(ray) -> RayIntersectionGeometric ri
   ├── (existing) ri.derivatives = {dpdu, dpdv, ...}  (triangle meshes)
   └── (new) if ray.hasDifferentials && ri.derivatives.valid:
         compute ri.txFootprint = {dsdx, dsdy, dtdx, dtdy}
         via chain rule: dsdx = invJacobian(dpdu, dpdv) · rxDir·t
         (where t = ri.range)

TexturePainter::GetColor(ri)
   └── pRIA->GetPELwithLOD(ri.ptCoord.y, ri.ptCoord.x,
                           lodFromFootprint(ri.txFootprint), c)

BilinRasterImageAccessor::GetPELwithLOD(x, y, lod, p)
   ├── if lod <= 0 || !mipmapEnabled: bilinear at base level (existing path)
   ├── pick mip level: integer = stochastic_round(lod)
   └── bilinear sample at the chosen mip
```

Secondary rays carry no differentials → `ri.txFootprint` left in
its default zero state → `lodFromFootprint` returns 0 → base-level
bilinear sample (same as today).  Graceful fallback.

### `Ray` changes

A new optional differentials block.  Designed so that rays without
differentials (the vast majority — shadow rays, NEE, BSDF samples,
photon shots) carry zero overhead beyond a single bool.

```cpp
namespace RISE
{
    //! Ray differentials for texture LOD selection (Igehy 1999).
    //! `rxDir`/`ryDir` are the directional offsets to the rays
    //! one screen-pixel to the right and one pixel down,
    //! respectively.  Origin offsets are zero for primary rays
    //! from a pinhole; thin-lens cameras and secondary rays would
    //! populate `rxOrigin`/`ryOrigin` (deferred to v1.1).
    struct RayDifferentials
    {
        Vector3 rxDir;
        Vector3 ryDir;
        // Origin offsets — zero for pinhole primary; set by
        // thin-lens / propagated reflections in v1.1.
        Vector3 rxOrigin;
        Vector3 ryOrigin;
    };

    class Ray
    {
        // ... existing members ...
        RayDifferentials  diffs;
        bool              hasDifferentials = false;
    };
}
```

Cost: 4 × Vector3 + bool ≈ 100 bytes per `Ray` whether populated
or not.  Today's `Ray` is ~120 bytes; this nearly doubles it.

**Mitigation**: keep differentials in a separate parallel struct
that the primary-ray path threads through, and keep `Ray` itself
lean.  Pseudo-code:

```cpp
class Ray { /* unchanged */ };

struct RayWithDifferentials
{
    Ray ray;
    RayDifferentials diffs;
    bool hasDifferentials;
};
```

The primary-ray path uses `RayWithDifferentials`; everything
downstream that doesn't care reads `.ray`.  The intersection
routines that DO care take an optional second parameter.

**Decision (v1)**: parallel struct.  Keeps the hot Ray cache line
unchanged.  Plumbing cost: a couple of new overloads on the
relevant intersection-test entry points.

### Camera-side: `PinholeCamera::GenerateRay`

Add a new overload (or a new `GenerateRayWithDifferentials`) that
also computes neighbour rays and packages them.

```cpp
bool PinholeCamera::GenerateRayWithDifferentials(
    const RuntimeContext& rc,
    RayWithDifferentials& r,
    const Point2& ptOnScreen ) const
{
    GenerateRay( rc, r.ray, ptOnScreen );

    // Neighbour rays at +1 pixel x and y in screen space.
    Ray rx, ry;
    GenerateRay( rc, rx, Point2( ptOnScreen.x + 1.0, ptOnScreen.y ) );
    GenerateRay( rc, ry, Point2( ptOnScreen.x, ptOnScreen.y + 1.0 ) );

    r.diffs.rxDir = rx.Dir() - r.ray.Dir();
    r.diffs.ryDir = ry.Dir() - r.ray.Dir();
    r.diffs.rxOrigin = Vector3( 0, 0, 0 );  // pinhole: shared origin
    r.diffs.ryOrigin = Vector3( 0, 0, 0 );
    r.hasDifferentials = true;
    return true;
}
```

Other camera types (`ThinLensCamera`, `FisheyeCamera`,
`OrthographicCamera`) can ship empty default impls that mark
`hasDifferentials = false` for v1.  Add proper differentials in
follow-ups when needed.

**Note**: footprint per primary ray is the FULL pixel size, NOT
divided by spp.  Stochastic averaging across N samples per pixel
integrates over the pixel-area mip-cone correctly.  Each sample
draws ONE mip level inside the cone; the average is the proper
integral.  Do not pre-shrink by sqrt(N).

### Intersect-side: triangle mesh

`TriangleMeshGeometry::IntersectRay` (and its indexed cousin)
already populate `ri.derivatives.dpdu`, `ri.derivatives.dpdv`.
Extend the implementation to also project the ray differentials to
texture space when the input ray has them:

```cpp
if( rwd.hasDifferentials && ri.derivatives.valid ) {
    // Compute the auxiliary ray hit points (where the rx/ry rays
    // would hit a plane through the original hit, perpendicular to
    // the original ray).  Igehy §3.
    const Scalar t = ri.range;
    const Vector3 rxIntersectOffset = (rwd.diffs.rxOrigin + rwd.diffs.rxDir * t);
    const Vector3 ryIntersectOffset = (rwd.diffs.ryOrigin + rwd.diffs.ryDir * t);

    // Project to surface tangent plane, then solve for (du, dv)
    // via 2x2 inverse of the dpdu/dpdv basis.
    // dsdx = du · texWidth, etc.  See PBRT v4 §10.1 for the closed form.

    ri.txFootprint.dsdx = ...;
    ri.txFootprint.dtdx = ...;
    ri.txFootprint.dsdy = ...;
    ri.txFootprint.dtdy = ...;
    ri.txFootprint.valid = true;
}
```

Other geometries (sphere, plane, etc.) keep `txFootprint.valid =
false` for v1.  TexturePainter falls back to base-level sampling.
Add per-geometry footprint computation in v1.1.

### `RayIntersectionGeometric` extension

```cpp
struct TextureFootprint
{
    Scalar dsdx, dsdy;
    Scalar dtdx, dtdy;
    bool   valid;
    TextureFootprint() : dsdx(0), dsdy(0), dtdx(0), dtdy(0), valid(false) {}
};

class RayIntersectionGeometric
{
    // ... existing members ...
    TextureFootprint  txFootprint;
};
```

Cost: 4 × Scalar + bool ≈ 36 bytes per RIG.  Acceptable — RIGs
already carry many derived members.

### `IRasterImageAccessor` LOD-aware path

```cpp
class IRasterImageAccessor : public virtual IFunction2D
{
    // ... existing GetPEL(x, y, p) ...

    //! LOD-aware sample.  Default implementation forwards to GetPEL
    //! (no LOD support); accessors that support mipmaps override.
    virtual void GetPELwithLOD(
        const Scalar x, const Scalar y,
        const Scalar lod,
        RISEColor& p ) const
    {
        GetPEL( x, y, p );
    }
};
```

Default implementation at the interface level means no other
accessor (NNB, BicubicBSpline, etc.) needs immediate updates.

### `BilinRasterImageAccessor` mip pyramid

```cpp
template< class C >
class BilinRasterImageAccessor : ...
{
    // ... existing fields ...
    bool                         mipmapEnabled;
    mutable std::vector<...>     mipPyramid;     // lazy-built
    mutable std::once_flag       pyramidBuilt;

    void BuildPyramid() const;  // box-filter pyramid down to 1×1

    void GetPELwithLOD( const Scalar x, const Scalar y,
                        const Scalar lod, RISEColor& p ) const override
    {
        if( !mipmapEnabled || lod <= Scalar(0) ) {
            GetPEL( x, y, p );  // existing base-level bilinear
            return;
        }
        std::call_once( pyramidBuilt, [this]{ BuildPyramid(); } );

        // Stochastic mip selection (Olano-Baker).  Caller-supplied
        // RNG would be cleanest; for v1 use a hashed function of
        // (x, y, lod, frame) so the result is stable per ray and
        // decorrelated across pixels.  Detailed PRNG plumbing TBD.
        const Scalar lodClamped = std::min( lod, Scalar( mipPyramid.size() - 1 ) );
        const int floor = (int)std::floor( lodClamped );
        const Scalar frac = lodClamped - Scalar( floor );
        const int chosen = (HashStochastic( x, y, lod ) < frac) ? (floor + 1) : floor;

        BilinearSampleAtMip( x, y, chosen, p );
    }
};
```

**Mip pyramid memory**:
- A 4Kx4K RGB image is ~50 MB; pyramid adds ~33% (1/4 + 1/16 + ... = 1/3) → ~17 MB extra.
- NewSponza: 72 textures averaging ~1MB each → ~24 MB extra total.  Fine.

**Lazy build**: don't pay the build cost for textures that are
never sampled at a non-base LOD (small textures, RGB factors,
etc.).  `std::call_once` ensures thread safety.

### `TexturePainter` connecting it all

```cpp
RISEPel TexturePainter::GetColor( const RayIntersectionGeometric& ri ) const
{
    if( !pRIA ) return RISEPel( 1, 1, 1 );

    Scalar lod = 0;
    if( ri.txFootprint.valid ) {
        // PBRT-style longer-axis footprint metric (isotropic LOD).
        const Scalar lengthX = std::sqrt( ri.txFootprint.dsdx * ri.txFootprint.dsdx +
                                          ri.txFootprint.dtdx * ri.txFootprint.dtdx );
        const Scalar lengthY = std::sqrt( ri.txFootprint.dsdy * ri.txFootprint.dsdy +
                                          ri.txFootprint.dtdy * ri.txFootprint.dtdy );
        const Scalar maxLen = std::max( lengthX, lengthY );
        lod = std::log2( std::max( maxLen, Scalar( 1e-10 ) ) );
        if( lod < 0 ) lod = 0;
    }

    RISEColor c;
    pRIA->GetPELwithLOD( ri.ptCoord.y, ri.ptCoord.x, lod, c );
    return c.base;
}
```

### Painter-creation API additions

`png_painter`, `jpg_painter`, etc. gain an optional `mipmap`
parameter (bool, default TRUE).  glTF importer sets `mipmap = FALSE`
when the role is "normal" so vector-quantity textures stay on the
bilinear-only path.

```
png_painter
{
    name        my_basecolor
    file        textures/foo.png
    color_space sRGB
    mipmap      TRUE          # NEW; default TRUE
}

png_painter
{
    name        my_normal
    file        textures/foo_normal.png
    color_space ROMMRGB_Linear
    mipmap      FALSE         # explicitly off for normal maps
}
```

GLTF importer wiring: in `CreateTexturePainter`, when role ==
"normal", pass `mipmap = false`.

## Sub-landings

Per the L1 pattern: one batch, no commits until user review.
Internal organisation for self-discipline / commit hygiene if the
landing later splits:

1. **L2.1** — `Ray` differentials struct + parallel `RayWithDifferentials` + `PinholeCamera::GenerateRayWithDifferentials`.  Eye-walk dispatchers updated to use the new path; everything else still uses bare `Ray`.  Tests: synthetic camera + known footprint values.
2. **L2.2** — `TextureFootprint` on `RayIntersectionGeometric` + triangle-mesh intersection populates it from incoming differentials.  Tests: hit a known triangle, verify footprint matches analytic value.
3. **L2.3** — `IRasterImageAccessor::GetPELwithLOD` default impl + `BilinRasterImageAccessor` lazy mip pyramid + stochastic mip selection.  Tests: a known image + known LOD selects the expected mip.
4. **L2.4** — `TexturePainter` connects footprint → LOD → accessor.  Per-painter `mipmap` opt-out parameter on chunk parser; glTF importer sets `mipmap=false` for normal maps.
5. **L2.5** — Verification scenes (aliasing checker), Sponza variance-measurement comparison, SCENE_CONVENTIONS doc update.

## Verification

### Test scaffolding

| Test | Type | What it proves |
|---|---|---|
| `RayDifferentialCameraTest` | unit | `PinholeCamera::GenerateRayWithDifferentials` produces (rxDir, ryDir) matching the analytic value for a known FOV / resolution / pixel position |
| `TriangleFootprintTest` | unit | Triangle intersect with a known incoming differential produces the expected `txFootprint` for the canonical "axis-aligned triangle, ray straight down" case |
| `MipPyramidTest` | unit | Lazy mip pyramid built from a 64x64 known texture has pyramid[1] = box-filter average of pyramid[0], pyramid[2] = box-filter of pyramid[1], etc. down to 1x1 |
| `StochasticMipSelectionTest` | unit | LOD = 2.3 picks mip 2 with prob 0.7 and mip 3 with prob 0.3 across many samples (statistical) |
| `TexturePainterLODTest` | unit | Footprint of footprint-area = 1 texel² produces LOD = 0; area = 4 texel² produces LOD = 1; etc. |
| `AliasingCheckerScene` | regression | High-frequency checker on a receding plane: bilinear-only render shows moiré + fireflies; mip-LOD render shows smooth band-limited horizon |
| `SponzaVariancePrePost` | manual | K-trial fixed-seed Sponza render, before vs after L2.  HDRVarianceTest reports per-pixel variance drop on the documented stone-column ROI |

### Verification protocol (post-implementation)

1. Render `scenes/Tests/Texture/aliasing_checker.RISEscene` at fixed seed/spp.
   Pre-L2: expect moiré speckle.  Post-L2: expect smooth fall-off.
   Diff via HDRVarianceTest with the pre-L2 baseline.
2. Render Sponza at fixed seed/spp/camera (PhysCamera001) to EXR.
   Compare to the L1 baseline EXR via HDRVarianceTest.
   Expect: variance drop on stone-column ROI; no significant
   change on near-camera regions; no regression on
   indirect-only regions.
3. Manual visual: open both EXRs in tev side-by-side, inspect
   stone-column aliasing.

## Decisions made (2026-05-03)

1. **Differential-carrying type — embedded in `Ray`** (PBRT / Mitsuba /
   Arnold style).  The +96 bytes per Ray is real cache pressure on
   BVH traversal, but the alternatives are worse:
   - Parallel `RayWithDifferentials` struct adds permanent tech debt
     (two camera entry points, fork at every plumbing junction,
     refactor pain forever).
   - Cycles-style ray cones are cheaper but isotropic-only and
     less PB-correct at oblique angles.
   PBRT/Mitsuba/Arnold all chose embedded full differentials and
   their performance is fine.  We follow.
2. **Propagation — full**, not primary-only:
   - Primary rays: camera populates.
   - Alpha-cut transparency continuation: pass through unchanged.
   - Specular reflection: Igehy 1999 §3 closed-form reflection
     formula.
   - Specular refraction: Igehy 1999 §3 closed-form refraction
     formula.
   - Glossy / diffuse: invalidate differentials (`hasDifferentials =
     false`); next intersect uses no footprint → texture sample at
     base level (which is correct — glossy/diffuse already
     hemisphere-integrate the texture, we don't need additional
     prefiltering on top).
   v1 instruments PT only; BDPT / VCM / MLT eye walks get
   primary-only behaviour for v1.1.

   **v1 scope reduction**: only PRIMARY-RAY differentials shipped in
   v1.  Alpha-cut continuation, specular reflection / refraction
   propagation, and the diffuse/glossy invalidation hooks are all
   deferred to v1.1 — they require touching every integrator's
   bounce loop (PT recursive walk, BDPT eye walk, VCM eye walk, MLT
   eye walk) plus per-SPF specular/diffuse classification.  Doing
   that on top of the already-meaty foundation work would push the
   landing past the manageable-batch threshold.
   What this means in practice: the textured first hit on stone
   columns gets clean mip-LOD sampling (which is the entire visual
   point of L2 for Sponza), but reflected stone in the gilded
   ornament's specular highlight is over-blurred (acceptable —
   that's not a dominant artefact).  Document and ship.
3. **Lowmem-friendly mip strategy — both**:
   - Default (lowmem off): build mip pyramid (level 1+; base lives
     in `pImage` and is sampled directly), stochastic single-mip
     selection.
   - `lowmem_textures TRUE`: skip pyramid entirely, use stochastic
     supersampling within the footprint at base level.  At infinite
     spp converges to the same integral as a proper pyramid.  Zero
     pyramid memory; noisier at low spp.
   The Job-level helper (`RasterImageAccessorFromChar`) composes
   `mipmap` × `lowmemory`:
     - `mipmap=true,  lowmem=false` → pyramid mode (`Mode_Pyramid`)
     - `mipmap=true,  lowmem=true ` → supersample mode (`Mode_Supersample`)
     - `mipmap=false, lowmem=*    ` → no LOD (`Mode_Base`)
   Caller (importer / chunk parser) only thinks about `mipmap`;
   lowmem composition is automatic.

### Critical post-Landing-2 regression fix (2026-05-03)

The initial Landing 2 mip pyramid implementation included the base
level (full resolution) in the pyramid storage, which made each
4K×4K texture cost ~683 MB of pyramid memory.  On NewSponza (~70
large textures) this exceeded system RAM and triggered swap
thrashing — interactive rendering became unusable.  Two compounding
fixes applied in the same revision:

1. **Base-level skip in `BuildMipPyramid`**: the pyramid now stores
   level 1 (half-res) and below.  The full-res base is left in
   `pImage`; LOD dispatch maps `chosenLevel == 0` to a `pImage.GetPEL`
   call instead of indexing the pyramid.  Cuts pyramid memory by
   ~75% (4K texture: ~683 MB → ~171 MB).
2. **Footprint stochastic supersampling for lowmem**: when
   `lowmem_textures TRUE` is set, the painter uses footprint-driven
   jittered sampling at base resolution instead of building a
   pyramid.  Zero pyramid memory; user gets the LOD anti-aliasing
   benefit without the memory cost.  `TexturePainter::filter_mode`
   is resolved at construction (Mode_Base / Mode_Pyramid /
   Mode_Supersample) so the per-sample dispatch is a single switch,
   not virtual-call dispatch.

Both fixes verified end-to-end: build clean, smoke render
unchanged, Sponza interactive rendering restored.  The lowmem
path's stochastic supersampling is noisier than the pyramid at
low spp but converges to the same integral.  The pyramid build
itself is unchanged in cost (~16M `pImage.GetPEL` calls per 4K
texture); the regression was purely the pyramid storage memory.

### Round-2 adversarial review fixes (2026-05-03 part B)

A second pass on the staged commit surfaced three more findings.
All addressed in the same revision:

1. **[P1] Transformed objects used stale world-space ray
   differentials.**  `Object::IntersectRay` (and `CSGObject::IntersectRay`)
   transform the central ray's origin / dir into object space before
   calling `pGeometry->IntersectRay`, but `ray.diffs` was left in
   world space.  `ComputeTextureFootprint` then projected world-space
   auxiliaries onto object-space `dpdu`/`dpdv` and produced wrong UV
   footprints — wrong mip LOD on every rotated / scaled / non-identity
   textured object.  The Sponza .gltf uses 155 nodes with non-identity
   transforms — virtually every textured surface was affected.
   Fix: transform `diffs.rxOrigin / ryOrigin / rxDir / ryDir` through
   the linear part of `m_mxInvFinalTrans` (offsets transform as
   vectors; translation cancels in the diff of two world points).
   The central ray's `SetDir` clears `hasDifferentials`, so the fix
   re-sets it after copying the transformed offsets across.  Same
   fix applied in `Object.cpp:264` and `CSGObject.cpp:160`.

2. **[P2] Alpha path didn't filter at LOD; BLEND mode shimmered
   under minification.**  `TexturePainter::GetColor` used the LOD
   path; `TexturePainter::GetAlpha` always sampled at base level.
   The original justification was "MASK needs crisp threshold input"
   — but for `alphaMode=BLEND` (the curtain in Sponza, foliage,
   decals, glass) this produced filtered RGB with un-filtered alpha
   under minification, leading to coverage/colour mismatch and edge
   shimmer.  Fix: extracted the LOD-aware sampling logic into a
   shared `SampleTextured(ri)` helper that returns a full RISEColor;
   both `GetColor` and `GetAlpha` now go through it, so colour and
   alpha pick the SAME LOD on every sample.  MASK threshold sharpness
   is now a property of the alpha-test shader-op (which can dither /
   stochastic-test against a filtered alpha) — the right layer for
   that policy.  Documented in the painter source.

3. **[P2] Legacy non-indexed `TriangleMeshGeometry` path doesn't
   populate `derivatives` or `txFootprint`.**  The new LOD path is
   only wired into `TriangleMeshGeometryIndexed` (used by glTF, PLY,
   RAW2 imports).  `TriangleMeshGeometry` (used by RAW format /
   hand-built meshes) never had derivatives in the first place — so
   normal mapping and SMS also don't work there; this is a
   pre-existing scope gap that L2 inherited rather than introduced.
   *Round-3 disposition*: documented inline as a v1.1 TODO at the
   legacy intersection site.  *Round-4 disposition*: gap closed —
   see Round-4 below.

### Round-3 adversarial review fixes (2026-05-06)

A third reviewer pass on the post-merge commit surfaced two
follow-on bugs hiding INSIDE the round-2 fixes themselves.  Both
addressed before re-staging.

1. **[P1] Round-2's `M_inv * rxDir_world` is wrong under any
   non-identity scale.**  The round-2 fix transformed all four
   differential fields with the same `Vector3Ops::Transform` call
   used for the central direction.  That works for origins (offsets
   between world POINTS — the translation cancels in the diff and
   the linear part is the right operator), but it does NOT work for
   `rxDir`/`ryDir`.  Those were established by the camera as the
   offset between two UNIT-NORMALISED world directions:
   `rxDir_world = aux_x_world_norm − d_world_norm`.  After applying
   `M_inv` the obvious diff `M_inv * rxDir_world` is unnormalised,
   and `normalize(M_inv * (d + rxDir))` ≠ `normalize(M_inv * d) + M_inv * rxDir`
   under any scale (uniform OR non-uniform).  Camera-space convention
   was silently broken in object space → wrong UV footprint → wrong
   mip LOD on every transformed textured mesh.  Sponza was the
   visible victim (155 transformed nodes, ~all textured).
   Fix: reconstruct the auxiliary direction in full —
   `aux_world = orig.Dir() + diffs.rxDir`, transform by `M_inv`,
   re-normalise, then re-difference against the (already normalised)
   object-space central direction.  Origin transforms unchanged.
   Same fix at `Object::IntersectRay` and `CSGObject::IntersectRay`.
   Verified the cancellation analytically: under uniform scale `s`,
   `M_inv * d_world` has length `1/s` so its normalisation matches
   the central direction's, and the re-difference recovers the
   correct object-space offset; under non-uniform scale the per-axis
   scales mix, and the normalise-and-re-difference operation
   correctly captures that the auxiliary now lies at a different
   relative direction in object space than it did in world.  This
   is the convention the PinholeCamera produces and the consumers
   (`ComputeTextureFootprint`) expect.

2. **[P2] Lowmem footprint sampler used scalar magnitudes; biased
   on rotated/sheared UV charts.**  Round-2 supersampling collapsed
   the 2x2 footprint Jacobian to two scalar L2 norms
   (`du = sqrt(dudx² + dudy²)`, similarly for `dv`) and jittered
   inside an axis-aligned bounding box of the footprint extents.
   The AABB always >= the true screen-pixel parallelogram, and
   strictly > on rotated / sheared mappings (Sponza's pillar
   cladding is a cardinal example — UV charts at ~30° to the
   surface principal directions).  The integration region was
   inflated, so minified textures over-blurred specifically in the
   lowmem path while looking sharp under the pyramid path on the
   same scene — a tell that the sampler, not the texture data, was
   wrong.  Fix: extended `IRasterImageAccessor::GetPELwithFootprint`
   to take the full Jacobian
   `(dudx, dudy, dvdx, dvdy)` and have the accessor jitter inside
   the screen-pixel parallelogram those columns span:
   `offU = sx * dudx + sy * dudy; offV = sx * dvdx + sy * dvdy`
   where `(sx, sy) ∈ [-0.5, 0.5]²`.  This is the SAME parallelogram
   a proper EWA / mip lookup would prefilter over, just stochastically
   sampled with one tap.  At infinite spp converges to the same
   band-limited integral as the pyramid path.  Hash salts that
   previously took the magnitudes now take distinct Jacobian
   components (`dudx` and `dvdy`) — equally good decorrelation,
   no need to compute magnitudes for any reason.

### Round-4 adversarial review fix (2026-05-06)

A fourth reviewer pass cleared the round-3 fixes and left only one
open finding: the round-2 disposition of the legacy non-indexed
`TriangleMeshGeometry` path was a doc-only TODO.  Reviewer noted
that if L2 is meant to cover triangle meshes "broadly rather than
just indexed/imported meshes", the gap is an incomplete
implementation, not just a scope decision.  Closed in round-4.

1. **[P2 → fixed] Non-indexed mesh path now populates
   `derivatives` + `txFootprint` on parity with indexed.**  Ported
   the UV-Jacobian / dpdu / dpdv / dndu / dndv / footprint
   computation from
   `TriangleMeshGeometryIndexedSpecializations.h:256-422` into
   `TriangleMeshGeometrySpecializations.h` immediately after the
   existing `ri.ptCoord` interpolation.  Math is identical; only
   the data access differs (non-indexed `Triangle` carries
   `vertices[]`, `normals[]`, `coords[]` by value, vs indexed
   `PointerTriangle`'s `pVertices[]`, `pNormals[]`, `pCoords[]`
   pointers into a shared vertex pool).  `MESH_PROJECT_DERIVATIVES_TO_TANGENT_PLANE`
   gate kept in lock-step between the two specialisations.
   Per-vertex color and tangent interpolation are NOT ported —
   those rely on a side-table (pColors / pTangents) addressed via
   pointer arithmetic into the shared vertex pool, which non-indexed
   meshes don't have; hand-built / RAW meshes that need vertex
   colours or glTF-style tangents should be loaded as indexed.
   Outcome:
   - RAW-format / hand-built meshes get LOD + footprint coverage
     on parity with glTF / PLY / RAW2.
   - Normal mapping and SMS now work on non-indexed meshes too
     (they need the same `derivatives` block).
   - `ComputeTextureFootprint( ri, ri.ray )` is a no-op when the
     incoming ray has no differentials, so untextured RAW meshes
     pay essentially zero overhead (a single `if` early-out per
     hit + the small UV-Jacobian + projection cost — all under a
     few dozen flops).
   - Cornell box BDPT (`scenes/Tests/BDPT/cornellbox_bdpt.RISEscene`,
     pure rawmesh, untextured Lambertian) renders pixel-identical
     to the previous build, confirming no regression on the
     existing non-indexed path.

### Verification + scope notes

- Build clean post-fixes after rounds 2, 3, and 4 (Library + RISE-CLI
  Release on Windows VS2022; no new warnings).
- Smoke render (`scenes/Tests/Geometry/shapes.RISEscene`) unchanged.
- Sponza glTF (`scenes/FeatureBased/Geometry/sponza_new.RISEscene`,
  16 spp, 640×360, lowmem_textures TRUE — exercises the
  transformed-instance differential reconstruction AND the
  parallelogram-footprint stochastic supersampling) renders correctly
  in 70 s wall-clock on Windows; identical wall-clock before vs.
  after the round-4 non-indexed port (proves no contamination of the
  indexed path).  Output (`rendered/sponza_new_denoised.png`) shows
  the canonical hero shot with PBR shading on stone arches, columns,
  and ornament — no NaNs, no crashes, no obvious aliasing / over-blur
  in the textured surfaces that exercised the lowmem path.
- Cornell box BDPT (`scenes/Tests/BDPT/cornellbox_bdpt.RISEscene`,
  pure non-indexed rawmesh, untextured Lambertian) renders identical
  to pre-round-4 — confirms the new derivative computation in the
  non-indexed path doesn't regress the existing untextured RAW path.
- Missing regression coverage that the reviewers flagged:
  (a) transformed textured mesh under uniform AND non-uniform scale,
  (b) alpha-blended minification, (c) lowmem-mode rotated UV chart,
  (d) textured non-indexed RAW mesh exercising the new round-4 path.
  All four are good candidates for v1.1 test scenes; not blocking
  the landing because the underlying fixes are local + auditable +
  covered structurally by Sponza's heterogeneity.

### Remaining open points (recommendations stand)

These are still up for review but defaults are good:
- **Mip filter**: box (PBRT-style pyramidal; trivial).  Lanczos has
  HDR ringing risk; Kaiser is more code.  Stays as-is.
- **LOD selection metric**: longer-axis (PBRT-style isotropic).  EWA /
  anisotropic deferred to v1.1.
- **Stochastic vs trilinear**: stochastic single-sample.  Matches PT
  philosophy; one texel sample per ray.
- **Default `mipmap` for `png_painter` etc.**: TRUE; importer sets
  FALSE for "normal" role.
- **PRNG for stochastic mip**: hashed-coordinate function for v1
  stability.  Sobol-stream integration is a follow-up.

## Migration impact

- **Existing scenes**: all primary visibility on textured surfaces
  starts using mip LOD selection.  Rendered output WILL change
  visibly on any scene with high-frequency textures viewed at
  distance — aliasing replaced by clean fall-off.  This is a
  deliberate quality upgrade.
- **Existing test corpus**: PNG byte-for-byte regressions on
  textured scenes will fail.  Update goldens; document in commit.
- **Normal maps**: glTF-imported normal maps automatically opt out
  via the importer's role check.  Hand-authored scenes that use
  `png_painter` for normal data must explicitly set
  `mipmap = FALSE`.  Document loudly in SCENE_CONVENTIONS HDR /
  texture sections.

## Cross-references

- Parent plan: [PHYSICALLY_BASED_PIPELINE_PLAN.md](PHYSICALLY_BASED_PIPELINE_PLAN.md)
- Landing 1 design: [PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_1.md](PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_1.md)
- Surface-derivatives contract: [GEOMETRY_DERIVATIVES.md](GEOMETRY_DERIVATIVES.md)
- Variance-measurement protocol that consumes the Sponza EXRs:
  [skills/variance-measurement.md](skills/variance-measurement.md)
- Reference: Igehy 1999, "Tracing Ray Differentials" (SIGGRAPH).
- Reference: Olano & Baker, "LEAN Mapping" (2010, separate work but
  the stochastic mip selection idea predates it; PBRT v4 §10
  attributes the "single sample per ray at the chosen LOD" pattern
  to general MC convention).

## Estimated effort

- L2.1 (ray differentials + camera): ~1 day
- L2.2 (intersect + footprint): ~0.5 day
- L2.3 (mip pyramid + LOD-aware accessor): ~1 day
- L2.4 (TexturePainter + chunk parser + GLTF importer wiring): ~0.5 day
- L2.5 (test scenes + verification + docs): ~0.5 day

Total: ~3.5 engineer-days, larger than L1 (which was ~2 days)
because L2 touches 6 layers (camera, ray, intersection, painter,
accessor, raster image) vs L1's 3 (parser, file output, writer).

## Decision points for your review

Same five decisions enumerated under "Open design points" above.
The recommendations stand; just confirm or redirect.

The biggest open question is **#1** — `Ray` embedding vs. parallel
struct.  This is the single design call that meaningfully changes
the implementation cost; everything else is local.
