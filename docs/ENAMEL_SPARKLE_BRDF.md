# Sparkle / Glint BRDF — discrete microfacets for vitreous-enamel flecks

## Motivation

Vitreous / grand-feu enamel twinkles: as the piece tilts, tiny bright **flecks**
appear and vanish. Physically these are *discrete* specular micro-facets — the
fire-polished glass surface's micro-topology, sub-surface crystallites, and (for
fumé/basse-taille over metal) the **bright metal substrate's fine texture
glinting through the transparent enamel**. A smooth GGX lobe cannot produce them
(it is a *continuous* facet distribution → a single smooth highlight); neither
can the smooth Worley height-field we tried (it gives soft colour mottling, not
sharp glints). Flecks require a *discrete* microfacet distribution.

This spec defines a physically-based **procedural discrete-microfacet (glint)
BRDF** for RISE, targeted first at the enamel watch's silver substrate.

## Why it's tractable in RISE (offline PT)

Real-time glint methods (Jakob 2014 *Discrete Stochastic Microfacet Models*, Yan
2014/2016, Zirr–Kaplanyan 2016, Chermain 2020) spend most of their machinery
**anti-aliasing** the glints — integrating the pixel footprint against a discrete
NDF at 1 sample/pixel. RISE renders **offline at high spp**, so:

- The pixel-footprint integral is performed automatically by **Monte Carlo**: the
  many camera samples per pixel land in different micro-cells; the fraction whose
  local flake aligns with the half-vector *is* the glint density. No closed-form
  footprint-NDF is required for a correct result — only enough spp.
- RISE already carries what a footprint-aware LOD refinement would need, so the
  refinement is an optional later slice, not a prerequisite:
  - `RayIntersectionGeometric::ptObjIntersec` — stable object-space hit point (spatial hash key).
  - `ptCoord` (UV), `onb` (tangent frame), `vGeomNormal` (unperturbed normal).
  - `dudx,dudy,dvdx,dvdy` and `dpdu,dpdv` — the pixel footprint (already used for texture mip LOD).

## Model

Surface = a smooth **base** lobe + a sparse set of **flakes** (discrete microfacets):

    f(wi,wo) = f_base(wi,wo)  +  f_glint(wi,wo)

- **f_base**: the existing GGX conductor lobe (silver) at the macro-normal, with a
  small base roughness. Unchanged transport.
- **f_glint**: procedural discrete flakes.
  1. Hash the hit point into a cell: `cell = floor(ptObjIntersec * density)` (3D
     hash; object-space so it is stable under camera/animation motion — the
     flecks are fixed to the dial, they twinkle because the *view/light* moves,
     not because the pattern swims). `density` = flakes per unit (controls fleck
     size/count).
  2. Per cell, a hash → a pseudo-random **flake normal** `Nf`, sampled about the
     geometric normal within an angular spread `glint_spread` (small; a few
     degrees). This is the flake's tilt.
  3. Glint lobe: a **sharp** specular response around `Nf` — a GGX with a tiny
     `alpha_glint` (near-mirror) evaluated with `Nf` as the microfacet normal.
     It is bright only when `H = normalize(wi+wo) ≈ Nf`, i.e. when this cell's
     flake happens to mirror the light to the eye → a fleck. Weighted by the
     flake Fresnel (silver's `F(H)` — bright, ~0.9+) and a normalization so total
     energy is conserved vs. `glint_strength`.
  4. `glint_strength ∈ [0,1]` mixes base↔glint (fraction of the surface that is
     flakes). Energy: `f = (1-strength)·f_base + strength·f_glint`, both lobes
     Fresnel-weighted; validate white-furnace ≤ 1.

Twinkle falls out: as `wo`/`wi` sweep (watch tilts), the set of cells whose `Nf`
satisfies `H≈Nf` changes → flecks appear/disappear. Object-space hashing keeps
each fleck pinned to a dial location.

## Integration options (pick in Slice 2)

- **A — new `sparkle_material`** (recommended): a self-contained GGX-conductor +
  glint material. Cleanest; no risk to the hot GGX path. Params below.
- **B — a `glint` fresnel/roughness mode on GGXMaterial**: less new surface but
  entangles the GGX hot loop.

Params (either way): `ior/extinction` (conductor, e.g. measured Ag), base
`alphax/alphay`, `glint_density` (flakes per world unit), `glint_spread`
(flake-normal angular stddev, deg), `alpha_glint` (glint lobe sharpness),
`glint_strength` (0..1 flake fraction). All physical scalars via `IScalarPainter`.

## Importance sampling

- **NEE / direct light** evaluates `value()` with `wi` from the light → flecks
  render directly under a small bright key. This is the primary path for the
  enamel (a small glint key over env fill). **Slice 1–3 target.**
- **BSDF sampling** (generating bounce rays): v1 samples the **base** lobe only
  (glints contributed via NEE). Sampling the glint lobe (toward the local `Nf`)
  is a **later slice** — needed for glint inter-reflection / caustics, not for
  the primary fleck look. Document the omission (no silent gap).

## Denoise interaction (important)

Glints are high-frequency, high-variance specular — OIDN treats them as noise and
smooths them. Plan:
- v1: rely on **high spp** so glints are resolved pre-denoise; measure how much
  OIDN erodes them. If material, expose the glint contribution in an AOV excluded
  from denoise (composite glints over the denoised base), or tag the material so
  the denoiser albedo/aux keeps them. Decide in Slice 4 with measurements.
- The `albedo()` AOV override must stay noise-free (return the *base* albedo, not
  the glinty one) so OIDN `cleanAux` holds.

## Files to touch (NEW files ⇒ update ALL FIVE build projects — see CLAUDE.md)

- `src/Library/Materials/SparkleBRDF.{h,cpp}` + `SparkleSPF.{h,cpp}` +
  `SparkleMaterial.h` (Option A), or GGX edits (Option B).
- `RISE_API.{h,cpp}`, `IJob.h`, `Job.{h,cpp}` — factory + adder.
- `Parsers/AsciiSceneParser.cpp` — `sparkle_material` descriptor + Finalize.
- Build projects (for any NEW file): `build/make/rise/Filelist`,
  `build/cmake/rise-android/rise_sources.cmake`,
  `build/VS2022/Library/Library.vcxproj` (+ `.filters`),
  `build/XCode/rise/rise.xcodeproj/project.pbxproj`.
- Tests: `tests/SparkleBRDFTest.cpp` (+ register).

## Phasing (each slice ends with the implementation-review-loop → 0 P1s)

1. **BRDF core** — `SparkleBRDF::value/valueNM`: cell hash → flake normal → glint
   lobe + base. Unit test: (a) a flake fires only when `H≈Nf` (revert-prove by
   disabling the hash); (b) glint density scales with `glint_density`; (c) energy
   ≤ 1 (white furnace); (d) object-space stability (same point → same flake under
   a view change). Verify `ptObjIntersec`/`onb`/footprint fields are populated at
   shade time for camera + secondary rays.
2. **Material + parser + API/Job plumbing**; a `sparkle_material` parses and
   round-trips; scene-parse test.
3. **Wire into the enamel silver substrate**; author `glint_*`; render + tune
   density/spread/strength against the reference twinkle; add a small glint key
   to the otherwise glare-free env lighting (small source ⇒ flecks not a band).
4. **Footprint-LOD + denoise handling**; measure OIDN erosion; AOV/keep-glints if
   needed; BSDF-sampling of the glint lobe if inter-reflection matters.

## Status

Design only (this doc). No engine code yet. Enamel scene is at the committed
glare-free env-only look. Implement in reviewed slices — best done with a fresh
context budget so the mandated multi-round adversarial review is not shortchanged.
