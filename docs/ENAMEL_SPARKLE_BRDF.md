# Sparkle / Glint — discrete microfacets for vitreous-enamel flecks

**Status (2026-07-03): design RATIFIED with a mechanism + layer pivot after a
3-reviewer adversarial gate (see §Ratification).  The feature is a general
discrete-facet GLINT NORMAL MODIFIER (`GlintModifier`, scene chunk
`glint_modifier`) composing with the EXISTING materials — NOT the
`SparkleBRDF`/`SparkleSPF`/`sparkle_material` stack originally sketched here.
The discrete-microfacet physics below stands unchanged; only the integration
point and the target layer moved.**

## Motivation

Vitreous / grand-feu enamel twinkles: as the piece tilts, tiny bright **flecks**
appear and vanish.  Physically these are *discrete* specular micro-facets.  A
smooth GGX lobe cannot produce them (it is a *continuous* facet distribution →
a single smooth highlight); neither can a smooth Worley height-field (soft
colour mottling, not sharp glints).  Flecks require a *discrete* microfacet
distribution.

### What the reference footage actually shows (measured 2026-07-03)

Frame-level measurement of the anOrdain Model 1 reference clips
(IMG_0349/0350; zoom crops in the session scratchpad) established:

- **Fleck colour is NEUTRAL-to-cool white** (cluster mean R≈G≈B within ~10 %)
  against a strongly red-dominant field.  Light returning from the buried
  silver would exit maroon-tinted (it traverses the gold-ruby medium twice,
  like the body colour does) — **no ruby-tinted glint population is
  resolvable**.  ⇒ The visible flecks are **top glaze-surface dielectric
  Fresnel glints**, not substrate glints.  This *corrects* the original
  spec's "target the silver substrate first".
- **Size** ~100–350 µm equivalent diameter (mean ≈ 180 µm ≈ 0.5 % of the
  38 mm dial); some 2:1–3:1 elongated streaks (a lay direction — silver
  spinning marks telegraphing through, or elongated glaze ridge crests).
- **Density** ~0.07–0.1 flecks/mm² *inside the lit specular lobe* at any one
  tilt; near-zero outside it.  The areal facet population is much denser —
  only the H≈Nf slice lights up.
- **Angle-gating is strong**: two of three tilt frames show essentially no
  flecks; the third shows a dense field.  Twinkle sensitivity is single-digit
  degrees.
- **Brightness**: clipped/near-clipped highlights, ≥3–5× the local field —
  true sharp specular, not bloom.
- The coarse orange-peel dimples (already REAL SDF micro-displacement in the
  scene) are a *separate, coarser* population: the flecks are a finer discrete
  facet field riding on top.  Two-scale structure, modeled at two levels.

## Model (ratified)

Surface = a smooth base surface + a sparse set of **discrete facets**:

- The object-space hit point is hashed into a cell (3D hash on
  `ptObjIntersec`; object-space so flecks are pinned to the dial — they
  twinkle because the *view/light* moves, not because the pattern swims).
- Per cell, a Bernoulli **existence** draw decides whether the cell holds a
  facet; a hash-jittered **facet centre** + **radius test** decides whether
  this hit lies on the facet (Atanasov & Koylazov 2016 discipline — this is
  what breaks grid tiling and gives a real areal coverage fraction; the
  3×3×3 cell neighbourhood is searched so facet discs never clip at cell
  boundaries).
- A hit on a facet gets a pseudo-random **facet normal** `Nf` — a Rayleigh
  tilt about the smooth shading normal within an angular scale
  `spread` (single-digit degrees), constant across the facet.
- The facet normal replaces the shading normal (`ri.vNormal` + `ri.onb`
  rebuilt) **before the material sees the hit** — so the EXISTING
  DielectricSPF flashes its delta reflection off the facet (a glaze fleck),
  and the EXISTING GGX conductor's VNDF sampler samples the tilted lobe (a
  metal-flake fleck), with no new BRDF code.

Twinkle falls out: as wo/wi sweep, the set of facets whose `Nf` mirrors the
light to the eye changes.  MC at high spp performs the pixel-footprint
integration that real-time glint papers (Jakob 2014, Yan 2014/16,
Zirr–Kaplanyan 2016, Chermain 2020) spend their machinery on — the many
camera samples per pixel land in different cells, and the fraction whose
facet aligns IS the glint density.  Within one facet the response is
deterministic, so a resolved fleck is *structure, not noise*.

## Why a MODIFIER and not a new BRDF (the ratification, 2026-07-03)

Three independent reviewers (physics; prior-art + in-engine composition;
reference characterization) converged:

1. **Physics level**: sub-pixel discrete facets belong at the
   shading-statistics level, not explicit geometry (the guilloché 19–43 GB
   mesh-bake memory wall is the in-house precedent; a hard mesh also aliases
   where a stochastic per-cell field integrates).  The discrete-microfacet
   *model* is ratified.
2. **Mechanism**: `IRayIntersectionModifier::Modify(ri)` perturbs
   `ri.vNormal`/`ri.onb` at hit-production time in **every** transport path
   (RayCaster ×3, PathTracingIntegrator RGB+NM, BDPT eye+light, all five
   photon tracers, SMS solver + photon map, SSS walks — verified 2026-07-03),
   so the existing materials deliver the glints with:
   - **BSDF sampling of the glint lobe for free** (GGXSPF samples VNDF around
     the perturbed `onb`; DielectricSPF reflects about it).  This is
     *decisive*, not merely convenient: the enamel mandates
     `transparent_shadows FALSE`, so NEE shadow rays from any buried surface
     are occluded by the glaze — **BSDF sampling is the only transport that
     delivers buried glints at all**, and the original SparkleBRDF v1
     explicitly did not sample its glint lobe.
   - **MIS consistency by construction**: value/Scatter/Pdf all see the same
     perturbed frame.  (The BRDF design had a quantifiable glint deficit
     `1 − w_NEE` under env/area lighting because `Pdf()` omitted the glint
     lobe while `value()` included it.)
   - **G6 ambient-IOR Fresnel, Kulla-Conty multiscatter, HWSS, thin-film**
     inherited untouched from the production GGX; dielectric facets conserve
     energy trivially (R+T = 1 per facet at any tilt).
3. **Layer**: the reference flecks are neutral top-surface glints (see
   measurements above) — a conductor-based `sparkle_material` could not have
   expressed the hero phenomenon.  The modifier attaches to any object: the
   glaze now, the silver substrate / car paint / snow / glitter later.

Costs owned honestly (not hidden):
- **Shading-normal energy asymmetry** (the classic normal-map caveat): the
  perturbed normal makes cosine factors inconsistent with the geometric
  surface at grazing.  Bounded by single-digit-degree tilts, a tilt clamp,
  and a geometric-side guard (`vGeomNormal`-based rejection to the
  unperturbed normal).  Same accepted physics as the in-tree
  BumpMap/NormalMap; the delta-dielectric hero path has no such issue.
- **One modifier per object** (`Object::AssignModifier` REPLACES).  The
  enamel glaze and silver objects carry no modifier today; a compound
  modifier is the named upgrade path if a scene ever needs bump+glint.
- **AOV interaction**: per-cell normals make the OIDN normal/albedo aux
  buffers locally non-smooth.  Tilts are a few degrees (Fresnel/albedo
  change is second-order); measured in Slice 4, with a glint AOV composite
  as the fallback plan.  `albedo()` continues to come from the material and
  stays noise-free at first order.

## The `GlintModifier` (parameters)

All defaults chosen so an unconfigured modifier is a no-op-adjacent gentle
sparkle; validation is string-level (ffast-math-safe) at parse time.

- `density`   — cells per object-space unit.  Anchor to physical fleck pitch:
  fleck pitch ≈ 1/density object units.  For the watch (dial radius 20.6
  units ≈ 19 mm ⇒ ~1.08 units/mm), 180 µm flecks ⇒ density ≈ 5 /unit.
- `coverage`  — per-cell facet existence probability in [0,1].  Combined
  with the radius test this sets the areal facet fraction (measured
  empirically by the unit test, documented, not assumed).
- `fill`      — facet disc radius as a fraction of the half-cell (0,1]; the
  3D sphere∩surface test yields naturally varying apparent facet sizes.
- `spread`    — facet tilt Rayleigh scale, DEGREES (single-digit typical).
- `scale`/`shift` — anisotropic cell stretch + offset (Worley3DPainter
  convention); elongated cells give the observed streak lay.
- `seed`      — hash seed (flecks differ between two otherwise identical
  objects).

Tangent coherence: the rebuilt ONB uses a *preserve-u* construction
(project the pre-perturbation tangent onto the new tangent plane) rather
than `CreateFromW`'s canonical-axis pick, so geometry-defined tangents
(`bShadingTangentFromGeometry`, anisotropic `tangent_rotation` consumers)
stay coherent across facets.

## Files to touch

- `src/Library/Modifiers/GlintModifier.{h,cpp}` (NEW ⇒ update ALL FIVE build
  projects — CLAUDE.md "Change Checklist").
- `Parsers/AsciiSceneParser.cpp` — `glint_modifier` descriptor + Finalize;
  `RISE_API.{h,cpp}`, `IJob.h`, `Job.{h,cpp}` — factory + adder (Slice 2).
- Tests: `tests/GlintModifierTest.cpp`.

## Phasing (each slice ends with the implementation-review-loop → 0 P1s)

1. **Modifier core** — `GlintModifier::Modify`: cell hash → existence →
   jittered centre + radius test (3×3×3) → facet normal → guarded normal/ONB
   replacement.  Unit test: (a) determinism + object-space stability (same
   point ⇒ same facet under any ray); (b) coverage statistics track
   `coverage`×`fill` (measured, revert-proven); (c) tilt distribution matches
   the Rayleigh scale; (d) no facet clipping at cell boundaries; (e) side
   guards (never flips past the geometric tangent plane); (f) GGX SPF/BRDF
   pointwise consistency at modifier-perturbed hits (sampling and NEE eval
   agree — the MIS-consistency claim, tested not asserted).
2. **Parser + API/Job plumbing**; `glint_modifier` parses, validates, and
   attaches; scene-parse test.
3. **Wire into the enamel glaze** (the dimpled dielectric top; neutral
   flecks per the reference evidence) + a small localized glint key added to
   the glare-free env lighting (small source ⇒ flecks, not a band); calibrate
   density/spread/coverage against the measured targets (≈5 cells/unit,
   single-digit-degree spread, ~0.1 flecks/mm² peak in-lobe); render the
   tilt set vs the d349/d350 clips.  Optionally audition a silver-substrate
   glint pass (the general feature makes it one scene line) and keep it only
   if the footage comparison wants it.
4. **Denoise interaction + LOD**: measure OIDN fleck erosion (denoise stays
   ON for presentation renders per standing rule); if material, exclude a
   glint AOV from denoise and composite; assess macro-zoom behavior (cells
   spanning pixels) and add footprint-aware response only if a real shot
   needs it.

## Ratification record (2026-07-03)

- Reviewer A (physics): ratify discrete-microfacet model; flagged
  displaced-NDF normalization + ambient-IOR Fresnel + NEE/MIS accounting as
  the BRDF-form's correctness cliffs (all dissolved by the modifier form);
  hierarchy argument for BRDF-statistics over explicit geometry.
- Reviewer B (prior art + engine): proposal is the Atanasov & Koylazov 2016
  cell-flake family (NOT Jakob/Yan — their machinery is footprint-AA for
  1 spp real-time, legitimately replaced by offline MC); non-negotiables
  adopted: in-cell jitter + radius test, existence probability, coverage
  separate from lobe weight, good 3D hash with independent lanes;
  recommended the modifier composition, verified `Modify()` fires on all
  transport paths and `AssignModifier` replaces.
- Reviewer C (reference measurement): flecks are neutral top-surface glints
  (no resolvable substrate population); size/density/angle-gating targets
  recorded above.
- Supervisor cross-checks: MIS bias direction corrected (glint **deficit**
  `1−w_NEE`, not over-weight); `transparent_shadows FALSE` ⇒ NEE-occlusion
  at buried surfaces makes BSDF-sampled delivery mandatory; DielectricSPF
  consumes `ri.onb.w()` and BumpMap's Modify contract (perturb `vNormal`,
  rebuild `onb`) verified as the integration seam.
