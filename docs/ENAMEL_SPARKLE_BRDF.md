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
   (RayCaster ×3, PathTracingIntegrator RGB+NM, BDPT eye+light — both
   subpaths store the PERTURBED frame in their vertices, and connection-time
   reconstruction copies it verbatim — all six photon tracers, SMS solver +
   photon map, SSS walks; verified 2026-07-03, re-verified by review round
   1), so the existing materials deliver the glints with:
   - **BSDF sampling of the glint lobe for free** (GGXSPF samples VNDF around
     the perturbed `onb`; DielectricSPF reflects about it).  This is
     *decisive*, not merely convenient: the enamel mandates
     `transparent_shadows FALSE`, so NEE shadow rays from any buried surface
     are occluded by the glaze — **BSDF sampling is the only transport that
     delivers buried glints at all**, and the original SparkleBRDF v1
     explicitly did not sample its glint lobe.
   - **MIS consistency by construction**: value/Scatter/Pdf all see the same
     perturbed frame, including the shared FlipW convention (the BRDF design
     had a quantifiable glint deficit `1 − w_NEE` under env/area lighting
     because `Pdf()` omitted the glint lobe while `value()` included it).
     Unit-tested as pointwise GGX SPF/BRDF pdf + albedo consistency at
     perturbed frames, RGB and NM — the ingredient the integrators' MIS
     relies on, not a full NEE-vs-BSDF MIS proof.
   - **G6 ambient-IOR Fresnel, Kulla-Conty multiscatter, HWSS, thin-film**
     inherited untouched from the production GGX; dielectric facets conserve
     energy trivially (R+T = 1 per facet at any tilt), and the dielectric's
     inside/outside classification is IOR-stack-based
     (`ior_stack.containsCurrent()`), so a facet tilt can never unbalance
     the IOR stack.
   - **SMS caveat**: `Modify` fires on the SMS path and stays UNBIASED there
     (`ValidateChainPhysics` gates on the facet-independent `vGeomNormal`),
     but the discrete facet field is not Newton-tractable — the manifold
     Jacobian differentiates the smooth-base `dndu/dndv`, which the facet
     field's piecewise-constant normals don't satisfy, so SMS will mostly
     fail to converge on facets (missed caustics, never wrong energy).  SMS
     does not deliver glint caustics; use PT/VCM for glinted specular
     casters.
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
- **Denoiser interaction**: the OIDN GUIDE buffers are safe by default —
  in the DEFAULT `OidnPrefilter::Fast` mode both the normal and albedo AOVs
  are captured from the camera-ray first hit BEFORE the modifier hook runs
  (`PathTracingIntegrator.cpp` writes them right after `IntersectRay`;
  `Modify` fires later inside `IntegrateFromHit`), so the guides carry the
  SMOOTH frame (verified in review round 2, correcting an inverted round-1
  claim).  In `Accurate` mode the record happens at the first non-delta
  scatter, post-Modify, so facet frames CAN reach the guides there (e.g. a
  glinted buried conductor); measure before using Accurate with glints.
  The real risk is unchanged from the original design: OIDN smooths
  high-variance glints in the BEAUTY pass.  The Slice-4 erosion measurement
  is therefore a **required gate before Slice-3 presentation renders ship**
  (denoise is always on for those); a glint AOV excluded from denoise and
  composited is the fallback plan.

## The `GlintModifier` (parameters)

The Slice-1 constructor takes all seven parameters explicitly (no defaults
exist yet).  Slice 2 WILL choose parser-level defaults so an unconfigured
`glint_modifier` chunk is a no-op-adjacent gentle sparkle, and MUST validate
inputs string-level at parse time (loud rejection of non-finite/garbage
tokens).  The ctor additionally forces the modifier INERT on any non-finite
parameter as a silent backstop — but only via the **volatile-laundered**
bit test: round 3 first implemented a plain memcpy/integer finiteness check
and the production `-ffast-math` build **deleted it** (clang tags double
function parameters `nofpclass(nan inf)`, so the NaN is poison at the call
boundary; a runtime-bit NaN coverage rendered a fully-lit facet field with
that guard compiled in, even from a strict-FP caller TU).  The laundered
form — store through `volatile double`, re-load, then test the exponent
bits — is opaque to the value-range analysis and probe-verified to catch
the same NaN from both caller types (the repo ffast-math rule's canonical
detector, as used by `IsFiniteOpaque` in FilmIntrospection.cpp).

- `density`   — cells per object-space unit.  Anchor to physical fleck pitch:
  fleck pitch ≈ 1/density object units.  For the watch scene (1 unit =
  38/48 mm ⇒ 1.263 units/mm; the 20.6-unit dial radius = 16.3 mm, the
  anOrdain dial proper), density 5 ⇒ 158 µm fleck pitch — inside the
  measured 100–350 µm band (exact-180 µm would be density ≈ 4.4).
  [Corrected 2026-07-03: an earlier revision wrote "dial radius ≈ 19 mm ⇒
  ~1.08 units/mm", wrongly equating dial and case radius.]
- `coverage`  — per-cell facet existence probability in [0,1].  Combined
  with the radius test this sets the areal facet fraction (measured
  empirically by the unit test, documented, not assumed).
- `fill`      — facet disc radius as a fraction of the half-cell (0,1]; the
  3D sphere∩surface test yields naturally varying apparent facet sizes.
  Values ≤ 0 make the modifier inert (like density/spread); values > 1
  clamp to 1 (the 3×3×3 neighbourhood guarantee requires radius ≤ half-cell).
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
- Tests: `tests/GlintModifierTest.cpp` (class),
  `tests/GlintModifierSceneParseTest.cpp` (chunk plumb-through).

## Phasing (each slice ends with the implementation-review-loop → 0 P1s)

1. **Modifier core** — `GlintModifier::Modify`: cell hash → existence →
   jittered centre + radius test (3×3×3) → facet normal → guarded normal/ONB
   replacement.  Unit test: (a) determinism + object-space stability (same
   point ⇒ same facet under any ray); (b) coverage statistics track
   `coverage`×`fill` (measured, revert-proven); (c) tilt distribution matches
   the Rayleigh scale; (d) no facet clipping at cell boundaries; (e) side
   guards (never flips past the geometric tangent plane); (f) GGX SPF/BRDF
   pointwise pdf + albedo consistency at modifier-perturbed hits, RGB and
   NM; (g) seed/scale/shift semantics; (h) DielectricSPF delta consistency
   at perturbed hits (mirror about the facet normal, Snell, R+T=1);
   (i) hostile-coordinate fold determinism.
2. **Parser + API/Job plumbing**; `glint_modifier` parses, validates, and
   attaches; scene-parse test.
3. **Wire into the enamel glaze** (the dimpled dielectric top; neutral
   flecks per the reference evidence) + a small localized glint key added to
   the glare-free env lighting (small source ⇒ flecks, not a band); calibrate
   density/spread/coverage against the measured targets (≈5 cells/unit,
   single-digit-degree spread, ~0.1 flecks/mm² peak in-lobe); render the
   tilt set vs the d349/d350 clips.  **The key's ANGULAR SIZE is the
   strongest density lever** (round-3 simulation at density 5 / coverage
   0.3 / fill 0.6 / spread 3°: key radius 0.5° ⇒ 0.083 flecks/mm² — on
   target; 1° ⇒ 0.28; 2° ⇒ 1.36 — fleck count scales roughly with the
   key's solid angle), so calibrate the key size FIRST, then coverage.
   The same simulation confirmed 47–58 % fleck turnover per 2° tilt
   (matches the reference's angle-gating) and ~5.8 distinct facets/mm²
   areal population with only the H≈Nf slice lit.  Optionally audition a
   silver-substrate glint pass (the general feature makes it one scene
   line) and keep it only if the footage comparison wants it.
4. **Denoise interaction + LOD**: measure OIDN fleck erosion in the BEAUTY
   pass (denoise stays ON for presentation renders per standing rule) —
   this measurement is a **required gate** for any Slice-3 presentation
   render; the guide buffers are smooth-by-default (Fast prefilter records
   pre-Modify; see §Costs) so erosion, if any, comes from the denoiser
   treating resolved flecks as residual noise.  If material, exclude a
   glint AOV from denoise and composite; assess macro-zoom behavior (cells
   spanning pixels) and add footprint-aware response only if a real shot
   needs it.

## Slice-3 outcome (2026-07-03, measured)

Wired into `enamel_watch.RISEscene`: `glint_glaze` (density 5, coverage 0.5,
fill 0.6, spread 4°, seed 1) on the glaze object + the `glint_key` panel
(6×6 units at 350 distance ≈ 0.5° angular radius, dial-plane mirror of the
cam_high34 base pose, `scale 400`).  All measurements at 380×380, spectral
PT, dial-region luminance on the sRGB PNGs:

- **No glare band** — the env-only look survives; the key adds only its two
  small physical crystal reflections (outer + inner surface, AR-dimmed).
- **Feature attribution**: an absurd-parameter render (coverage 1, fill 1,
  spread 40°) transforms the dial (dense grain, mean −0.03) — the modifier
  unambiguously fires through parse→attach→SDF-object→spectral-PT.  At
  production parameters the facet layer contributes ~44 exclusive bright
  pixels — **consistent with the reference-calibrated fleck count**
  (0.083/mm² × ~280 mm² lit ≈ 25–40, one fleck spanning 1–2 px); the
  pre-existing orange-peel dimples carry the
  denser base shimmer, as in the real dial (two-scale structure).
- **Twinkle**: +3° camera tilt (well within the Wobble's ±12° elev /
  ±5° azim envelope) turns over ~37 % of
  bright dial pixels (550 gone, 450 new of ~1490) — matching the
  reference's strong angle-gating.
- **OIDN fleck survival** (Slice-4 gate, first measurement): plain→denoised
  keeps 98/145 fleck pixels (68 %) at 512 spp but only 8/35 (23 %) at
  128 spp — the denoiser needs the noise floor below the fleck structure,
  so the scene now carries `samples 512` with the rationale inline.  The
  guide buffers were not the issue (scene runs `oidn_prefilter accurate`;
  record lands on the smooth buried silver).
- Side-by-side against the d349 reference frames: sparkle scale/character
  match; the reference's brighter lobe is its physically larger window —
  the key's angular size stays the calibration lever for taste passes.

## Slice-4 outcome (2026-07-03): denoise + LOD decisions

- **OIDN erosion — measured, mitigated by spp, no AOV composite needed.**
  The required gate ran during Slice-3 tuning: fleck survival through the
  denoiser is 68 % at 512 spp vs 23 % at 128 spp (the denoiser needs the
  noise floor below the fleck structure; flecks are deterministic per-pixel
  so they READ as structure once resolved).  The hero scene carries
  `samples 512` with the rationale inline; the denoised hero visibly
  retains the sparkle field, so the glint-AOV-composite fallback is NOT
  exercised (it remains the documented plan if a future shot needs fleck
  fidelity at low spp).
- **Guide buffers**: non-issue for the hero — the scene runs
  `oidn_prefilter accurate`, which records at the first non-delta scatter
  (the smooth buried silver), and Fast mode would record pre-Modify anyway
  (§Costs).
- **Macro-zoom LOD**: assessed, not needed for any real shot.  At the hero
  framing a facet spans 1-2 px; the tightest committed framing (cam_macro,
  100 mm) keeps facets small; a hypothetical deeper zoom would resolve
  facets into their true disc shapes (physically honest — they ARE discs).
  Footprint-aware response stays a documented non-goal until a real shot
  needs it.
- **Glint-lobe BSDF sampling**: satisfied by construction — the modifier
  pivot made the existing SPFs sample the perturbed frame (the reason the
  Slice-1 mechanism review chose it), so the original Slice-4 sampling
  work item is moot.

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

### Implementation review, round 1 (2026-07-03, Slice 1)

Three fresh adversarial reviewers on commit a8f7b3c8 (math/statistics;
engine integration; test strength + claims fidelity):
- Math/statistics: ZERO P1 — hash lanes, Rayleigh inverse-CDF, solid-angle
  density (finite at θ→0), fold continuity at negative coordinates, true
  jittered-grid coverage (~0.46, above the Poisson 0.41 bound), and the
  preserve-u right-handedness all verified with independent numerical
  experiments.
- Engine integration: ZERO P1 — all 23 `Modify` call sites walked; BDPT
  stores the perturbed frame on BOTH subpaths and reconstructs it verbatim;
  the dielectric's IOR-stack-based inside/outside test is facet-immune;
  GGX Scatter/value/Pdf share the FlipW convention.  P2s folded above (SMS
  Newton-tractability caveat; Slice-4 OIDN measurement promoted to a
  required gate — NOTE this reviewer's Fast-mode AOV-contamination claim
  was later found INVERTED by round 2 and corrected in §Costs: Fast-mode
  guides are captured pre-Modify and stay smooth).
- Test strength: ONE P1 — the tangent-coherence check used a canonical base
  frame, so it could not discriminate preserve-u from a `CreateFromW`
  rebuild (mutation survived).  Fixed: the check now uses a non-canonical
  geometry-defined base tangent and asserts the rebuilt u equals the
  projection of THAT tangent (mutation now fails 0/2000).  P2s fixed: seed
  and scale/shift semantics now tested; NM twin of the GGX consistency test
  added; coverage band tightened to (0.40, 0.48); photon-tracer count
  corrected to six; MIS wording scoped to what is actually tested.

### Implementation review, rounds 2–3 (2026-07-03, Slice 1)

- Round 2 (fresh correctness + claims reviewers): ONE P1 — the round-1
  claim that Fast-mode OIDN AOVs carry the perturbed first-hit normal was
  INVERTED (Fast captures normal+albedo right after `IntersectRay`, before
  `Modify` runs inside `IntegrateFromHit`); corrected everywhere, Slice-4
  gate re-derived from beauty-pass erosion.  P2s: nearest-wins tie-break,
  composed scale+shift order, and the geomSign flipped branch were claimed
  but untested — all three now have discriminating tests (each mutation
  revert-proven); parameter section moved to future tense.
- Round 3 (fresh full-sweep + render-realism/API reviewers): full-sweep
  ZERO P1 (round-2 OIDN text re-verified to ground truth incl. the hero's
  spectral-rasterizer path; all new tests' logic validated).  Render
  realism: the model is structurally capable of the measured fleck targets
  (0.083/mm² at a 0.5° key radius; 47–58 % turnover per 2° tilt; ~5.8
  facets/mm² areal) — calibration guidance folded into Slice 3.  API axis:
  ONE P1 — non-finite parameters are silent glitter corruption.  The first
  attempted fix (plain memcpy bit guard) was DELETED by the production
  build (nofpclass parameter poison, verified from a strict-FP caller
  too); the repo's ffast-math memory then supplied the working
  formulation — the **volatile-laundered** exponent test — which was
  probe-verified to survive the parameter boundary from both caller types
  and now backstops the ctor (inert on garbage), with string-level parser
  rejection remaining the loud Slice-2 gate.  P2: zero `vGeomNormal` would
  have silently disabled the modifier on any future geometry that forgets
  to populate it — guard now falls back to the shading normal (tested).
  Cost note added; key-size coupling documented.
