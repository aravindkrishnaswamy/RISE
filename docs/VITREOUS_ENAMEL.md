# Vitreous Enamel — Physics, Optics, and a RISE Rendering Plan

**Status:** Planning / research. No code written. Reference footage received
(two lighting-bracketed frames, 2026-06-30); the captured-watch target spec is
in §9.

**Hero target:** a genuine **anOrdain Model 1** — grand-feu fumé
(smoked-gradient) **red / oxblood↔raspberry** vitreous-enamel dial with a strong
**crystalline orange-peel surface texture**; sector chapter ring, gold leaf
hands. Full captured-watch spec in §9. The model must stay **general enough to
also do flinqué / basse-taille** (translucent enamel over engine-turned
guilloché); those two are the *same physics* (see §2.1).

This document is the analogue, for vitreous enamel, of the guilloché-watch
design arc (`THIN_FILM_INTERFERENCE.md`, `GUILLOCHE_3WAY_RESULTS.md`). It
captures: (1) the physics/optics of enamel a renderer must reproduce, (2) what
already exists in RISE to build on, (3) the principled material model, (4) the
gaps that decide "compose existing" vs "new feature", and (5) a phased plan.

The guiding constraint from the user: **simulate the physics and optics of
vitreous enamel as accurately and as generally as possible** — name the feature
by its physics (a finite-slab absorbing/scattering medium between a dielectric
interface and a substrate), not by the specific watch.

> **Project principle (2026-06-30, overrides all "defer / out-of-scope /
> v1-approximation" calls below): improve RISE along the way — more physically
> accurate, more flexible — and *never cut corners or accept a limitation/weakness
> for the sake of rendering the scene.*** The scene is the *forcing function*, not
> the deliverable. When the render exposes a RISE weakness (hardcoded air IOR,
> luminance-collapsed spectral medium, hero-only HWSS volume path, a BSDF-eval
> interface that can't carry the IOR stack), the correct response is the **general
> fix in RISE**, not a scene-local workaround. A *physically exact* composition of
> existing primitives is fine; an *approximation that merely gets the look* is not.
> "Out of scope" is legitimate only for genuinely unrelated features, never as a
> license to ship known-wrong physics on the path the scene exercises. **§11
> re-examines every deferral below against this principle.**

**Decisions locked (2026-06-30):** build the spectral medium-coefficients
feature (G1) and the conductor ambient-IOR fix (G6); stand up the layerlab +
analytic-slab oracles; model flexible for both grand-feu and flinqué with
grand-feu fumé as the hero; the crystalline texture is authored **procedurally**
(generated, not baked from the footage) — but as **real microgeometry on the
physical layer first classified in §10.2**, not a fixed top-interface
normal-map. **Status: holding at planning for user review of this doc before any
Phase-2 code.**

---

## 1. What vitreous enamel is (physics)

Vitreous ("fired", "grand feu") enamel is **powdered glass frit fused onto a
metal substrate at 750–850 °C**. Optically it is a **colored glass slab on
metal**, a few tens to a few hundred µm thick (watch dials: 6–9 fired layers,
enamel total a few hundred µm over a thin metal disc; whole dial ≈ 1 mm).

The layer stack (bottom → top) — *this is the model*:

1. **Metal substrate** — gold / silver / copper (precious metals for
   transparent work; copper blackens under clear enamel). For watch dials it is
   often **engine-turned (guilloché)** or hand-engraved low relief.
2. **Enamel glass body** — a silicate/borosilicate glass (often leaded/borated)
   carrying either *dissolved-ion colorants* (homogeneous absorber, no
   scattering) and/or *suspended opacifier particles* (Mie scatterers).
3. **Polished/fire-finished top surface** — a smooth glassy Fresnel interface,
   sometimes with subtle "orange-peel" undulation from firing.

A hidden **counter-enamel** on the back equalizes thermal stress; it is never
visible and is ignored for rendering.

### Colorants are genuinely spectral (not RGB tints)

Color comes from **wavelength-selective absorption** by transition-metal ions or
colloidal nanoparticles — d–d ligand-field transitions, charge-transfer bands,
or plasmon resonance. Measured absorption-band centers in silicate glass
(positions shift ±10–30 nm with composition):

| Colorant | Color | Mechanism | Absorption band centers (nm, MEASURED) |
|---|---|---|---|
| Cobalt Co²⁺ (tetra) | deep blue | d–d | **triple band ≈ 530, 590, 645** |
| Copper Cu²⁺ | turquoise/green | d–d (Jahn-Teller) | **broad ~780** (+minor ~400) |
| Manganese Mn³⁺ | purple | d–d | **broad ~490** (450–540) |
| Chromium Cr³⁺ | green | d–d | **~450 and ~650** (transmits between) |
| Iron Fe³⁺ / Fe²⁺ | amber / blue-green | LMCT / d–d | **Fe³⁺ 225–435; Fe²⁺ broad ~1050 w/ visible tail** |
| Nickel Ni²⁺ | brown / violet | d–d | 6-coord ~430; 4-coord ~500–650 |
| Gold ruby (Au⁰ NP 20–50 nm) | red | plasmon | **~520–530** |
| Copper ruby (Cu₂O/Cu⁰ NP) | red | plasmon | **~560** |
| Cd(S,Se) selenium ruby | orange→red | semiconductor band edge | **sharp transmission cutoff**, edge tuned by Se fraction |
| Uranium (uranyl) | yellow-green | absorption + fluorescence | absorbs UV 200–400, **emits ~500–550** |

Host glass: **n ≈ 1.50–1.60** (default 1.55; plain soda-lime 1.52, leaded
"jewel" enamel up to ~1.6+). Dispersion: a crown/BK7-like Abbe **V≈64** for fine
enamel (subtle fringing); leaded enamels trend toward flint **V≈30**
(pronounced). Recommended dispersion model: **BK7 Sellmeier** for the host, scale
toward flint for leaded.

> **Renderer implication:** colorants → a **per-wavelength absorption
> coefficient σ_a(λ)** in a clear glass (Beer-Lambert). Plasmon/band-edge colors
> are also spectral (single peak / step). This is exactly why the spectral
> rasterizers, not RGB, are the right pipeline — and why σ_a must be a **physical
> scalar** (no JH-uplift color pipe). For the *medium* specifically it is a
> **coordinate-free spectral curve `σ_a(λ)`** — *not* a surface `IScalarPainter`
> (which is queried at a surface hit, so it can't serve a volume event; §10.7), and
> not RGB.

### Opacifiers → scattering (opaque/white and translucent)

Opacity is **Mie scattering** from insoluble micro-particles suspended in the
glass; the driver is particle size ≈ λ/2 × refractive-index contrast with the
matrix:

| Opacifier | n (particle) | optimal diameter | loading |
|---|---|---|---|
| TiO₂ rutile | **2.7** | 0.2–0.3 µm (~0.25) | strongest white per mass |
| TiO₂ anatase | 2.55 | ~0.3 µm | — |
| SnO₂ (cassiterite) | 2.04 | ~0.2–0.5 µm | 5–10 wt% |
| ZrSiO₄ (zircon) | 1.93–1.96 | 0.6–0.75 µm | 10–20 wt% |

Matrix n ≈ 1.5–1.7. White opacifiers are **non-absorbing in the visible →
single-scattering albedo ω ≈ 0.98–1.0**; color comes only when colored
pigments/ions are added. Spectrally σ_s is **near-flat (gray)** for white, with a
slight blue tilt as size drops toward the Rayleigh end.

> **Renderer implication (no-corners — see §10.5):** the **translucent ↔ opaque**
> axis is the **magnitude of σ_s** (× layer thickness), not absorption: translucent
> = low σ_s (mm mean free path), opaque = high σ_s (sub-100 µm). The σ_s(λ), σ_a(λ),
> and the full angular phase function `p(cosθ,λ)` should be **derived from Mie theory**
> (particle size distribution, particle n(λ), matrix n, volume loading — the table
> above), **not** a fitted Henyey-Greenstein `g`. Appearance calibration against the
> reference may tune the one genuinely-unknown input — the **opacifier loading /
> concentration** — but the coefficient and phase *model* comes from the particle
> physics, not from a hand-picked `g≈0.5` or a "gray σ_s fit." HG-`g` is only a
> labelled preview fallback. (K-M↔RTE bridges S ≈ ¾·σ_s·(1−g), K ≈ 2·σ_a if K-M S/K
> data surfaces; the moderate Mie forward peak for these sizes is g≈0.4–0.7, used only
> to sanity-check the tabulated lobe, not as the model.)

---

## 2. Enamel techniques and their optical behavior

Every technique is the same stack with four knobs: enamel **opacity**, whether
there is a **metal backing**, the **microgeometry of that metal**, and **layer
thickness variation**.

| Technique | Opacity | Dominant transport | Substrate role |
|---|---|---|---|
| **Basse-taille** | translucent | refract → reflect off polished engraved relief → out; **absorption ∝ local thickness** | central: mirror + depth=tone/color map |
| **Flinqué** | translucent | basse-taille over **engine-turned (guilloché)** periodic facets | central: periodic facet field = the shimmer |
| **Cloisonné** | mixed per cell | opaque: surface+diffuse; translucent cells: refract→flat-metal→out | base + back-reflector; wires = visible specular ridges |
| **Champlevé** | opaque only | glossy colored-dielectric surface; rough recess floor kills metal return | thick base, recess floor optically inert |
| **Plique-à-jour** | transparent | **transmission** (stained-glass); best backlit | **none** — only thin metal cell-wall struts |
| **Grisaille** | translucent white over dark ground | thickness-modulated white scattering over dark absorber | hidden; dark enamel ground is the "substrate" |
| **Painted / Geneva** | opaque ground + thin pigment + clear glaze | diffuse off white ground, tinted, under specular clear-coat | hidden under opaque white |
| **Paillon / foil** | translucent over embedded foil | refract → buried specular foil flecks → out → angle glints | base + suspended foil = buried mirrors at depth |

**The central effect to nail (flinqué/basse-taille over guilloché):** as the
dial or light tilts, only the subset of engraved facets whose orientation
satisfies the reflection geometry returns a bright glint; the lit set sweeps
across the periodic pattern → the surface **shimmers**. Simultaneously, deeper
engraving = longer in-glass path = more absorption = **darker/more saturated**;
high points = thinner glass = paler. **Both behaviors fall out of a correct
layered model — no special shimmer term is needed**, only (a) a smooth colored
dielectric over (b) an engraved conductor, with (c) absorption integrated along
the refracted path length.

### 2.1 Fumé = basse-taille's thickness mechanism, driven by a dome (HERO)

The anOrdain Model 1 fumé (MEASURED, from anOrdain's own development notes):

- The base is **silver** (their fumé was *discovered* by firing on silver
  instead of the usual copper; "the alchemy of each enamel's reaction to silver"
  produces the dusky, sometimes crystalline gradient).
- The silver blank is **stamped flat on the bottom, domed on top**, with
  proportions tuned "to allow the enamel to gradate at the correct rate."
- Because the silver bulges up at the centre and the polished enamel top is
  flatter, the **enamel is thin at the centre and thick at the rim** → by
  Beer-Lambert, the colour is **light/solid at the centre and dark at the rim**.
  "Fumé" = smoked: solid hue centre → dark rim.

> **This is the same physics as basse-taille** — thickness-coupled absorption of
> a translucent colored enamel over a reflective substrate. The *only*
> difference is what modulates the thickness:
>
> | | substrate shape | thickness field | colorant conc. |
> |---|---|---|---|
> | **Fumé (hero)** | smooth **dome** (silver) | radial, smooth | constant |
> | **Flinqué/basse-taille** | **engraved/guilloché** (silver/gold) | periodic / relief | constant |
>
> So **one architecture renders both**: translucent spectral medium (constant
> σ_a) over a reflective conductor, with a **substrate height field** that is a
> dome for fumé or a guilloché relief for flinqué. The radial fumé gradient is a
> *geometry* effect, not a graded-concentration effect — though authoring
> support for spatially-varying σ_a (G1) still buys cloisonné cells, painted
> work, and concentration-graded fumé variants.

The captured hero's colour (§9) is a **red gold-ruby/selenium** band, not a
manganese-purple primary; the purple-black rim is the thick-glass absorption tail.
anOrdain's "textured / crystalline" fumé character is a **dominant** surface
feature (§9), **not** a minor refinement — and which physical layer it lives on
(top glass relief vs buried near-substrate crystallization) is **unresolved and
must be classified before modeling** (§10.2), because the two produce optically
different results (uncolored Fresnel sparkle vs colour-filtered glints).

---

## 3. What already exists in RISE (capability map)

### Materials / interfaces (`src/Library/Materials`, `src/Library/Interfaces`)

- **`DielectricMaterial`** — refractive glass. `ior` (wavelength-dependent via
  `SellmeierScalarPainter` / `PolynomialScalarPainter` / `Function1DScalarPainter`),
  `tau` (scalar Beer-Lambert distance attenuation `pow(tau,d)` — **author explicit
  `tau 1.0` with `interior_medium`; never omit or use `none`, §10.9**), `scattering` (Phong/HG
  width — high value = perfect/delta refraction, lower = rough-ish), optional
  single-layer AR film (`arN`/`arK`/`arThickness`). This is the **air→enamel
  interface**.
- **`GGXMaterial`** — PBR microfacet conductor/dielectric. `fresnel_mode`
  ∈ {`conductor` (complex n,k), `schlick_f0`, `thinfilm` (Airy oxide-on-metal)};
  anisotropic `alphaX/alphaY`; spectral n,k via scalar painters. This is the
  **engraved metal substrate** (gold/silver/copper). Thin-film mode handles
  anodized/heat-tint iridescence (reused from the guilloché watch).
- **`PolishedMaterial`** — dielectric coat over **Lambertian** base
  (Weidlich-Wilkie). Hardcoded diffuse base — useful for grisaille/painted
  (opaque ground under glaze), not for metal-backed translucent enamel.
- **`CompositeMaterial`** — recursive layer composition: `top`/`bottom` SPFs,
  `thickness`, `extinction` (Beer-Lambert between layers), per-lobe recursion
  caps. Low-level but **can** stack dielectric-over-conductor.
- **`HomogeneousMedium`** — uniform participating medium: `absorption` (σ_a),
  `scattering` (σ_s), optional emission, `phase` (`isotropic` / `hg <g>`).
  Bound to a geometry interior via `standard_object { interior_medium <name> }`,
  or scene-wide via `global_medium`. IOR-stack handles nesting. PT samples
  free-flight + Beer-Lambert transmittance. **This is the enamel body.**
- **SSS:** `subsurfacescattering_material` (Burley **BSSRDF**, semi-infinite
  diffusion) and `randomwalk_sss_material` (brute-force volumetric walk, handles
  thin geometry). See §5 for why BSSRDF is the *wrong* body model here.
- **Painters:** `IPainter` (color, JH-uplifted) vs **`IScalarPainter`**
  (physical scalar, **no** uplift) — IOR/roughness/σ/g must use the scalar pipe.
  Spatial color via `png_painter`/image painters, procedural
  (`ExpressionFunction2DPainter`, Perlin/Worley/Gerstner), `BlendPainter`,
  UV-transform. Spectral input via `.spectra` files (`spectral_painter`, being
  folded into `scalar_painter`) and `.n`/`.k` files.

### Geometry / scene scaffolding (from the guilloché watch)

`scenes/FeatureBased/GuillocheWatch/watch_dial.RISEscene` is directly reusable:
SDF case/crystal/hands (`sdf_geometry` inline `part` lines), `sweep_geometry`
strap + `path_instances_geometry` stitching, `cartesian_disk_geometry` +
`displaced_geometry`/`sdf_geometry` heightfield dial, six `expression_function2d`
guilloché fields (proven == retired C++ field to 1e-6), two-softbox studio
lighting + dark Fresnel floor, multi-camera + named-animation timelines, dual
EXR+PNG output, `render_watch_views.py`, the sapphire crystal with AR coat.
**The enamel watch reuses the case/crystal/hands/strap/lighting/camera/pipeline
wholesale and swaps the dial material+geometry.**

### Spectral pipeline & integrators

- `RISEPel = Rec709RGBPel` (D65); JH spectral uplift LUT (`rec709.coeff`,
  ~3.9 % deep-blue corner failures). Spectral rasterizers: `*_spectral_*` (NM
  per-wavelength + HWSS hero-wavelength).
- **Integrator: `pathtracing_spectral_rasterizer` (PT-spectral).** Glass-over-metal
  is *short* transport (eye → surface → medium → metal → medium → surface → out),
  which PT handles well and cheaply (PT wins σ²·T on 10/13 classes). **Avoid
  `vcm_spectral_rasterizer`** — known photon-store luminance-proxy bug on
  *dispersive* caustics. **SMS** (PT-only) available if sharp caustic focusing
  onto the substrate needs it.
- **Wavelength sampling — NOT decided in favour of HWSS (review P1).** RISE's
  HWSS volume path samples free-flight at the **hero wavelength**
  ([PathTracingIntegrator.cpp:3586,4256](src/Library/Shaders/PathTracingIntegrator.cpp:3586));
  once G1 makes σ_a/σ_s **wavelength-dependent**, the per-wavelength event and
  survival probabilities diverge from the hero's, so HWSS is **not assumed
  physically exact** for colored media. **Ground truth is the analytic Beer slab,
  not any render (§10.3)**; non-HWSS independent-NM is the **candidate reference
  path *after* it passes the analytic-slab gate** (the suspected pure-absorber
  double-attenuation bug lives in the NM/PT medium path itself). HWSS is a further
  *optimization* gated on matching NM+analytic within MC noise; fixing its
  free-flight reweighting is part of G1's scope. Until those gates pass, render the
  hero with the validated non-HWSS NM path.
- `auto_spectral_rasterizer` exists and would route this to PT.

---

## 4. The material model (principled)

The physically-correct model is a **bounded three-layer finite slab** — the same
structure PBRT-v4's `CoatedConductorBxDF` (Guo–Hašan–Zhao 2018) / Jakob et al.
2014 adding-doubling encapsulate in a *single* layered BSDF. RISE has **no such
layered material**; we approximate it by *explicit geometry + media* (a dielectric
interface, an interior medium, and a conductor floor as separate scene elements).
That composition is physically faithful **only if the bottom boundary is the
conductor itself, with no spurious glass→air interface beneath the medium** — see
the boundary-mechanism decision in §10.1 (review P1):

```
            incoming
               │
   ┌───────────▼────────────┐  (1) air → enamel interface:
   │   smooth dielectric     │      Fresnel, IOR≈1.55, optional Sellmeier
   │   (fire-polished, flat) │      dispersion; optional "orange-peel" bump
   ├─────────────────────────┤ ← flat polished top
   │ σ_a(λ) σ_s(λ) p(cosθ,λ) │  (2) enamel body = participating medium:
   │ thin centre │ thick rim │      spectral Beer-Lambert colorant bands;
   │   ╲         │        ╱   │      Mie opacifier σ_s(λ)+p(cosθ,λ) (ω≈1), 0=clear;
   │     ╲_______│______╱     │      thickness = (flat top − substrate height)
   └──────╲╲╲╲╲╲╲╲╲╲╲╲──────┘  (3) reflective conductor substrate:
        domed silver (fumé)         GGX conductor n,k; height field is a
        └ OR engraved (flinqué)     DOME (fumé) or guilloché RELIEF (flinqué)
```

The thickness field — `(flat enamel top) − (substrate height)` — is what makes
the colour gradate. A dome gives the smooth fumé gradient; a guilloché relief
gives the basse-taille tonal/shimmer pattern. **Same model, swap the substrate
height field.**

Key correctness point from the literature: for **thin enamel over a reflective
floor**, use the **bounded medium-between-interfaces** formulation, **not** a
semi-infinite BSSRDF (Burley/dipole assume a thick half-space, which a thin slab
over a mirror violates). In RISE terms: a `DielectricMaterial` surface + an
**`interior_medium` `HomogeneousMedium`** filling the slab + a `GGXMaterial`
conductor bottom — *not* `subsurfacescattering_material`. **The bottom boundary
must be the conductor, not a dielectric face** (§10.1): `DielectricSPF` pops the
IOR stack and refracts out at any dielectric exit
([DielectricSPF.cpp:97](src/Library/Materials/DielectricSPF.cpp:97)), so a naïvely
closed dielectric slab would add a fake glass→air interface in front of the
silver. **Hard authoring rule (review P1):** author the top dielectric with an
**explicit `tau 1.0`** (never omit it, never `none` — those fail construction;
§10.9) — dielectric `tau` is applied as Beer-Lambert distance attenuation
`pow(tau, distance)` on exit ([DielectricSPF.cpp:218](src/Library/Materials/DielectricSPF.cpp:218)),
so a non-unity `tau` *plus* the medium's Beer transmittance **double-counts
absorption**. **All colour absorption lives in `HomogeneousMedium` σ_a(λ); the
dielectric interface is colour-neutral (`tau 1.0`).** (§10.9)

**Why this gives the look for free:**
- Translucency/depth/inner-glow ← multiple scattering + absorption in the body.
- Basse-taille tone map ← path length to the engraved floor varies with relief →
  thickness-coupled absorption.
- Guilloché shimmer ← specular reflection off the per-facet metal normals seen
  through the refracting interface; the lit facet set sweeps with view/light.
- Wet/deep surface ← IOR≈1.55 Fresnel highlight over a bright substrate.
- Opaque vs translucent ← **Mie-derived** σ_s(λ) magnitude + p(cosθ,λ) (§10.5). White
  ← ω≈1, near-flat σ_s(λ). Colored ← σ_a(λ). (HG-`g` is only a labelled preview.)

**Substrate material rule:** the silver is GGX **`diffuse = black`, `specular =
unity`, `fresnel_mode conductor`, measured silver n/k** — its ambient IOR comes
**automatically from the IOR stack** (`n_enamel(λ)`) via G6, no slot to author. GGX
adds its diffuse lobe unconditionally, so a non-black diffuse would graft a fake
Lambertian backing under the glass (§10.10).

### Validation / ground truth (mirrors the guilloché 3-way study)

- **layerlab** (Jakob 2014 adding-doubling, `github.com/wjakob/layerlab`) is a
  **flat, local, 1D plane-parallel BSDF oracle** — it assumes infinite parallel
  layers and a single entry/exit point. So it validates the **flat-patch,
  constant-thickness** material (the BSDF), **not** the dome gradient, guilloché
  relief, or displaced dimples (review P2). Use it only to check the local layered
  reflectance against RISE on a flat sample.
- **Finite geometry** (dome gradient, dimple sparkle, case reflections) is
  validated by **actual RISE renders compared to the two clips**, not layerlab.
- The **reference video** is the appearance target: palette, the dim↔bright
  centre/rim bracket, translucency, lighting, proportions.
- Lambertian control-sphere + the `effective-rise-scene-authoring` checklist
  before touching material internals.

---

## 5. Gaps — what's compose-existing vs needs new code

| # | Gap | Load-bearing? | Resolution |
|---|---|---|---|
| **G1** | **Spectral mode collapses the medium to gray luminance.** `HomogeneousMedium::GetCoefficientsNM()` returns `Luminance(m_sigma_*)` ([HomogeneousMedium.cpp:101](src/Library/Materials/HomogeneousMedium.cpp:101)) — so a red enamel renders **achromatic** under `*_spectral_*` today; it's not merely "RGB-approximated." | **YES — the core fidelity item (LOCKED: build it).** A green-absorbing gold-ruby red, cobalt's triple notch, etc. cannot exist at all in spectral mode without this. | **Cross-cutting fix — see the "Net" paragraph + §11, not a one-liner:** spectral **storage + distance sampling + majorant + transmittance** in `HomogeneousMedium`; the **pure-absorber double-attenuation + HWSS free-flight reweighting** estimator work (§10.3); and a **new spectral-curve authoring path** — the chunk advertises `DoubleVec3` ([AsciiSceneParser.cpp:5996](src/Library/Parsers/AsciiSceneParser.cpp:5996)) and `IJob`/`RISE_API` take RGB, so all must gain a λ-curve binding — plus tests (analytic-slab gate). Hero needs only a **position-independent λ-curve** (gradient is geometry, §2.1). Spatial-×-spectral is a **separate, bigger build** (§10.7, §11): a volume-coordinate mapping (`IMedium` queries by `Point3` [IMedium.h:83](src/Library/Interfaces/IMedium.h:83), not a surface painter) **plus** fixing `HeterogeneousMedium`'s luminance collapse — **built, sequenced after the hero**, not abandoned. |
| G2 | **No rough-dielectric GGX transmission (BTDF).** | No for the hero (fire-polished); surface relief is real displacement, not a rough BTDF. | Sequenced on need (§11): if any enamel reads satin, **build a proper GGX dielectric BTDF**, not the `scattering`-Phong-widening heuristic. Distinct from the §10.2 crystalline relief (real microgeometry). |
| G3 | **BSSRDF is semi-infinite** (wrong for thin slab over mirror). | N/A — design decision, not a code gap. | Use bounded `interior_medium`, per §4. |
| G4 | Thin-film is **single-layer** (>1-layer interference absent). | No — only for multi-stack optical coatings / strong opalescence. | Genuinely out of scope (§11 "S"). The *single*-layer thin-film **ambient-IOR** under enamel is covered by G6/§10.8, not this. |
| **G5** | Geometry: enamel must be the **volume between the flat top and the substrate height field** (a **dome** for fumé, a **guilloché relief** for flinqué). | Yes (modeling). | Model the enamel as a heightfield/SDF slab: underside = substrate height field (dome or guilloché), top = flat/gently-domed; assign `interior_medium`. The SDF heightfield path already does both. Silver substrate ⇒ GGX conductor with **silver n,k** + G6. |
| **G6** | **GGX conductor Fresnel hardcodes air incidence** (Ni = `RISEPel(1,1,1)` [GGXSPF.cpp:266](src/Library/Materials/GGXSPF.cpp:266); `1.0` NM [GGXSPF.cpp:509](src/Library/Materials/GGXSPF.cpp:509), and GGXBRDF). The buried silver is seen **through glass**, so its Fresnel must use **Ni = n_enamel(λ)**, not air. | **YES** — silver-under-glass reflects differently than silver-in-air; without it the substrate is physically wrong even with correct geometry. | `Optics::CalculateConductorReflectance` already takes an explicit `Ni` ([Optics.h:53](src/Library/Utilities/Optics.h:53)). **General fix: read `ior_stack.top()`** as the ambient IOR (= `n_enamel(λ)` under the burial convention — automatic + dispersion-correct, no authoring). **SPF/Pdf already receive the stack** ([ISPF.h:135](src/Library/Interfaces/ISPF.h:135)) → zero-interface edit (stop hardcoding air); **`IBSDF::value()`/`valueNM()` do not** ([IBSDF.h:55](src/Library/Interfaces/IBSDF.h:55)) → thread ambient IOR into the BSDF-eval interface (vtable/ABI event). Cover **all** sites: SPF Fresnel, BRDF value, **Kulla-Conty `F_avg`** ([GGXBRDF.cpp:288](src/Library/Materials/GGXBRDF.cpp:288)/[:435](src/Library/Materials/GGXBRDF.cpp:435)), albedo/AOV. Scene `incident_ior` = **optional override only**. See §10.6. |
| — | Foil/paillon flecks, plique-à-jour backlight, uranium fluorescence. | No. | Explicit out-of-scope extensions; note for later. |

**Net (do not under-scope — see §11):** v1 reuses the watch scaffolding for geometry
and lighting, but its *physics* requires two coordinated, cross-cutting RISE changes,
not "two small features":
- **G1 (chromatic medium) spans the whole construction stack**, because medium
  coefficients are RGB-only end to end today: the scene chunk advertises
  `DoubleVec3` `absorption`/`scattering` ([AsciiSceneParser.cpp:5996](src/Library/Parsers/AsciiSceneParser.cpp:5996)),
  `IJob::AddHomogeneousMedium` takes `double[3]` ([IJob.h:1450](src/Library/Interfaces/IJob.h:1450)),
  `RISE_API_CreateHomogeneousMedium` takes `RISEPel` ([RISE_API.h:3216](src/Library/RISE_API.h:3216)),
  and `GetCoefficientsNM` collapses to luminance. So G1 = **spectral storage +
  spectral distance sampling/majorant/transmittance in `HomogeneousMedium`**, the
  **HWSS free-flight reweighting + the pure-absorber double-attenuation fix** (§10.3),
  a **new *coordinate-free* spectral-curve authoring path** (a `λ→value` abstraction,
  **not** a surface `IScalarPainter` — §10.7) **through the parser descriptor, `Job.cpp`,
  `IJob`, and `RISE_API`**, and **tests** (analytic-slab gate). It plausibly adds
  files → confirm the five-build-project rule (CLAUDE.md).
- **G6** = the **IOR-stack reads** at the conductor Fresnel sites (SPF/Pdf:
  zero-interface) **plus one `IBSDF::value()`/`valueNM()` interface change** to carry
  the ambient IOR for the MIS/`F_avg` path (§10.6) — a vtable/ABI event.
Later-sequenced improvements (heterogeneous spectralization, spectral Mie phase
function + an `IPhaseFunction` wavelength arg, RGB per-channel ambient IOR) are in §11.

---

## 6. Phased plan

Each implementation phase ends with the **implementation-review-loop** (build
clean + warning-free on both `make` and Xcode, tests pass, then fresh
adversarial reviewers until a round finds zero P1s) per the standing rule.

- **Phase 0 — Research consolidation (this doc). DONE.**

- **Phase 1 — Reference study. DONE** — target spec in §9 (anOrdain Model 1, red
  gold-ruby/selenium fumé, crystalline surface, sector layout, two clips bracket
  dim↔bright).

- **Phase 1.5 — Pre-build design decisions (review-driven; before any feature
  code).** Resolve the §10 items: (1) the **boundary mechanism** so the medium is
  bottomed by the conductor with no fake dielectric exit, including the **G6
  glass-incidence conductor Fresnel** (§10.1, §10.6); (2) **classify the
  crystalline texture's physical layer** — top-surface glass relief vs buried
  near-substrate crystallization — from the footage, then choose real
  microgeometry for that layer (§10.2); (3) the **HWSS-vs-NM slab test** (§10.3).
  §10.4 (units) is already resolved (inverse scene units). Output: decisions
  appended to §10. Small probes only.

- **Phase 2 — Material foundation.** Implement **G1** (wavelength-dependent
  σ_a(λ)/σ_s(λ) on `HomogeneousMedium`, **with correct/HWSS-validated free-flight
  sampling**) **and G6** (conductor ambient-IOR so the buried silver is shaded
  glass→metal). Author the colorant library from **measured transmittance/absorbance
  curves → Beer-Lambert → σ_a(λ)** (inverse scene units, §10.4): the **red
  gold-ruby/selenium band first** (the hero), then cobalt/copper/chromium/iron;
  white opacifier from **Mie-derived σ_s(λ), σ_a(λ), and a tabulated/spectral phase
  function `p(cosθ, λ)`** (HG-`g` only as a labelled preview fallback, §10.5).
  Validate the flat-patch material against the §10.1 analytic oracle **and** the
  **layerlab 1D oracle**.

- **Phase 3 — Dial geometry + surface.** Build the finite-slab enamel volume
  between a flat polished top and a **substrate height field** (G5) via the §10.1
  boundary mechanism: a **silver dome** for the hero fumé gradient (tune curvature
  to the reference); the **guilloché generator** for the flinqué variant. Apply
  the §10.2 crystalline texture as **real microgeometry** on the chosen layer.

- **Phase 4 — Watch assembly.** Reuse case / crystal+AR / hands / strap / cameras
  / dual-output pipeline from `watch_dial.RISEscene`; reshape slim downturned lugs,
  sector + even-Arabic dial, gold leaf hands + central seconds, engraved caseback,
  caviar-grain strap. Light with a **window-shaped soft area light** + environment
  to match the clips (not the guilloché two-softbox rig).

- **Phase 5 — Render & calibrate.** **Non-HWSS NM PT-spectral** as the reference
  render (HWSS only if §10.3 passes), **`transparent_shadows false`** for **every kept
  render including the final beauty shot** (preview-only otherwise, §10.11); calibrate
  σ_a scale, dome curvature, and lighting against the dim↔bright bracket. EXR→PNG via the watch recipe (anchor
  exposure on highlights, ACES filmic + sRGB). Optional tilt **animation** showing
  the gradient + sparkle via the HDR-ProRes pipeline (denoise stays **on** for
  presentation renders).

### Sequencing (not "accepted limitations" — see the project principle / §11)
**Built for the hero v1:** the bounded layered slab (G1 spectral medium incl. the
estimator-correctness fix + HWSS reweighting; G6 conductor ambient-IOR incl. the
BSDF-eval interface fix and the thin-film Airy path), the silver dome, the
classified surface microgeometry, window lighting.
**Built but sequenced *after* the hero (not excluded):** spatially-varying spectral
medium for cloisonné/painted (§10.7 volume-coordinate mapping **+ fixing
`HeterogeneousMedium`'s luminance collapse: spectral coeffs, majorants, distance PDFs,
transmittance**); opaque/white opacifier from **Mie-derived σ_s(λ), σ_a(λ), and a
tabulated/spectral phase function `p(cosθ,λ)`** — needing an `IPhaseFunction`
wavelength arg (§10.5); rough-dielectric GGX **transmission** (BTDF) if any enamel
surface reads satin rather than fire-polished (replacing the `scattering`-Phong-widening
approximation properly).
**Genuinely different features, not part of this material's physics path** (the
only legitimate "out of scope"): foil/paillon sparkle, plique-à-jour backlit
transmission, fluorescent uranium glass, multi-layer (>1) interference (G4).

---

## 7. Decisions (resolved 2026-06-30)

1. **Technique target** — model **flexible for both** grand-feu and flinqué;
   **hero = grand-feu fumé** (anOrdain Model 1 Plum Fumé). §2.1 shows both reduce
   to one architecture (thickness-modulated translucent spectral medium over a
   reflective conductor; dome vs guilloché substrate). Exact palette/proportions
   confirmed from the video in Phase 1.
2. **New-feature appetite** — **build G1** (spectral medium coefficients). For
   the hero it can be spatially uniform; spatial-×-spectral generalizes later.
3. **Validation** — **build the layerlab ground-truth rig** (guilloché-study
   discipline) in addition to calibrating against the video.

---

## 8. Sources

Composition/firing/opacifiers: Wikipedia *Vitreous enamel* / *Opacifier* /
*Glass coloring*; ScienceDirect *Enamel (Vitreous)*; frit patents US3216847A /
US2466043A; anOrdain & Monochrome & Phillips enamel-dial guides; Springer
tin-opacifier review; Ceramic Arts Network opacity; digitalfire zircon.
Colorant spectra: ResearchGate/ScienceDirect UV-Vis of Co/Cu/Cr/Mn/Ni/Fe in
glass; gold-ruby plasmon (ScienceDirect S1296207408001040); selenium ruby;
Bamford *Colour Generation and Control in Glass* (canonical quantitative ref).
IOR/dispersion: RP Photonics crown/flint; Schott N-BK7 Sellmeier (TIE-29).
Layered-material rendering: Weidlich & Wilkie 2007; **Jakob, d'Eon, O. Jakob &
Marschner 2014** (adding-doubling, layerlab); **Guo, Hašan & Zhao 2018**
(position-free MC = PBRT-v4 `LayeredBxDF`/`CoatedConductorBxDF`); Belcour 2018;
de Dinechin & Belcour 2022; OpenPBR `coat`; PBR Book §14.3. Measured BRDF:
MERL, RGL-EPFL (no labeled enamel); Jensen et al. 2001 BSSRDF (marble stand-in).
Notable: Otto Jakob, co-author of the 2014 layered-materials paper, is himself a
cloisonné-enamel goldsmith — the foundational layered-rendering work is tied to
this exact material class, though its published demos are generic.
Hero reference / fumé mechanism: anOrdain — *The Model 1 Fumé: The Development of
Fumé Enamel* (https://anordain.com/blogs/news/the-model-1-fume-the-development-of-fume-enamel)
and *Model 1 Plum Fumé* (https://anordain.com/products/model-1-plum-fume):
silver blank, flat-bottom/domed-top, enamel deeper (and darker) at the rim.

---

## 9. Phase 1 reference study — the captured watch (target spec)

From two 4K60 clips of the user's actual watch (`IMG_0349.MOV` 15.7 s dim/window
light; `IMG_0350.MOV` 23.6 s bright/diffuse; received 2026-06-30; hand-held,
rotated in azimuth + polar; on-crystal reflections are the window). Frames at
1 fps in the session scratchpad. **Watch identified: a genuine anOrdain Model 1**
— dial reads "anOrdain" (under 12) and "VITREOUS ENAMEL" (above 6); closed steel
caseback engraved with anOrdain + the map of Scotland. Known specs: **38 mm
diameter, 20 mm lugs, domed crystal, hand-wound.**

### Dial — colour & fumé gradient (HERO behavior)
- **Dim window light** (clip 349): deep **oxblood/burgundy-red** centre → maroon →
  **near-black rim**, faint purple at the edge.
- **Bright/diffuse light** (clip 350): centre blooms to a vivid **raspberry /
  cerise red**, still rolling off through burgundy to near-black at the rim.
- That large luminance + saturation swing with lighting is the **translucent
  enamel over a reflective (silver) dome** behaviour (§2.1): light punches
  through the thin centre and reflects off the metal; the thick rim stays dark.
  The two clips conveniently bracket the centre value and rim roll-off for
  calibration.
- **Colorant = red, gold-ruby / selenium type** (absorbs green ~520–560 nm,
  transmits red strongly + a little blue → the oxblood↔raspberry). The
  purple-black rim is **thick-glass absorption tail**, *not* a manganese-purple
  primary. (Corrects the earlier plum-manganese guess.) Author this red band in
  Phase 2; a faint manganese/blue-transmitting component for the rim cast is
  optional polish.

### Dial — surface texture (HERO feature, confirmed dominant)
- In bright light the dial is a field of **hundreds of tiny specular orange-peel
  / crystalline dimples** that sparkle, with the colour glowing between them
  (anOrdain's "textured / crystalline" fumé). This is the **single most
  distinctive visual signature** — not a subtle accent. ⇒ **in scope for v1**,
  but modeled as **real microgeometry on the layer first classified in §10.2**
  (top-surface glass relief vs buried near-substrate crystallization), *not* an
  assumed top-interface normal map. Whatever the layer, the **dome sets the
  colour gradient and the dimple field sets the sparkle** — two separate features.

### Dial furniture (gold)
- **Sector/railroad chapter ring** with periodic numerals (…5/10/15/20/25…) and
  a fine 1/5 sub-scale.
- Large gold **even Arabic numerals** 12·2·4·6·8·10.
- Printed **"anOrdain"** under 12 and **"VITREOUS ENAMEL"** above 6.
- **Gold hands:** slim **leaf/lance hour + minute** (minute reaches the chapter
  ring) **plus a thin central seconds**.

### Crystal, case, crown, caseback
- **Domed** (box-ish) crystal, light/no AR (window reflection wraps the dome).
- **Polished steel** round case (38 mm), slim polished bezel, modest caseband;
  **slim, downturned, gently faceted/beveled lugs** (profile frames 349_01/04
  give the curvature); small **fluted/coin-edge crown at 3**. Closed **engraved
  steel caseback** (anOrdain + Scotland map). Reshape — but reuse — the
  watch_dial SDF case.

### Strap
- Oxblood/burgundy **caviar / fine-pebble-grain leather**, **tonal-red stitching**
  near the edges, dark painted edges, tapered, keeper loop, **steel pin buckle**.
  Prominent raised grain ⇒ bump/normal on the existing `sweep_geometry` strap.

### What this changes in the plan
1. **Colorant** = a **red gold-ruby/selenium spectral σ_a** (green-absorbing),
   rim purple-black from path-length, NOT a manganese-purple primary. Author this
   band first in Phase 2 and calibrate centre/rim against the two clips.
2. **Surface texture** is **in-scope v1** and high priority — the crystalline
   sparkle is the hero's signature — but it is modeled as **real microgeometry on
   the layer first classified in §10.2**, *not* a fixed cellular bump/normal on
   the top interface (that classification is open; see §10.2).
3. **Lighting:** model a **window-shaped soft area light** + environment to
   reproduce the on-crystal reflection and the centre punch-through; the two
   clips give the dim/bright targets.
4. Geometry: reuse case/crystal/hands/strap scaffolding; **reshape slim
   downturned lugs**, author the **sector + even-Arabic** dial layout, gold leaf
   hands + central seconds, engraved caseback.
5. Substrate stays a **smooth (silver) dome** for the gradient; the dimple field
   is a separate top-surface feature; no guilloché for the hero (that's the
   flinqué generalization the architecture still supports).

---

## 10. Review-driven design decisions (2026-06-30)

An adversarial review of this plan (against the actual RISE source) surfaced
issues that must be resolved **before Phase-2 feature code** (the new "Phase 1.5"
in §6). All code citations below were verified. None invalidates the core
direction (finite spectral absorbing/scattering glass over reflective silver, not
BSSRDF); they harden the boundary semantics, texture layer, HWSS claim, units, and
opacifier data so the result is *hack-free*, not appearance-fit.

### 10.1 Boundary mechanism — the medium must be bottomed by the conductor (P1)
RISE tracks object media via the IOR stack
([MediumTracking.h:45](src/Library/Utilities/MediumTracking.h:45)), and
`DielectricSPF` **pops the stack and refracts out at any dielectric exit**
([DielectricSPF.cpp:97](src/Library/Materials/DielectricSPF.cpp:97)). A naïvely
*closed* dielectric enamel slab therefore has a **bottom dielectric face that
refracts/Fresnels into whatever is beneath it** — a fake glass→air interface in
front of the silver, which is unphysical (the enamel is fused *to* the metal).
Options, in increasing generality:
- **(A) Burial convention — recommended for v1, uses existing materials.** Model
  the silver as a closed **conductor** solid (GGX) and the enamel as a dielectric
  solid whose **bottom face is buried *inside* the opaque silver** (enamel extends
  below the silver's top surface). From inside the enamel the first downward hit
  is then always the **conductor** surface (reflect, no IOR pop); the enamel's own
  bottom face sits inside opaque metal and is never reached. IOR stack stays
  balanced: push on top entry → conductor reflects (no push/pop) → pop on top
  exit. No spurious interface, no air gap.
- **(B) Coated-conductor layered BSDF — the general answer, larger feature.** A
  real `CoatedConductorBxDF` analogue (one surface; dielectric interface + interior
  medium + conductor bottom handled internally). This is the literature model
  (§4) and the right long-term "general enamel material," but a substantial build.
- **(C) Per-face materials** (dielectric top, conductor bottom on one object) —
  if RISE gains per-face material assignment.
- **Decision:** start with **(A)** and **prove it** with the test below; escalate
  to **(B)** only if (A) shows artifacts or when a first-class general enamel
  material is wanted. **(A) also depends on G6** — the buried conductor's Fresnel
  must use **glass** incidence, `R_glass→metal(θ_t,λ)`, not air.
- **Validation oracle (leading-order, single substrate pass).** A flat slab
  (dielectric top + colored medium of thickness `d` + silver bottom via the
  convention), with incidence angle θ_i, refracted in-medium angle θ_t (Snell:
  `sin θ_i = n_enamel(λ)·sin θ_t`):
  - **Top specular term:** `F_air→glass(θ_i, λ)` (the surface reflection).
  - **Substrate-return term:**
    `(1 − F_air→glass(θ_i,λ)) · exp(−σ_t(λ)·d/|cos θ_t|) · R_glass→metal(θ_t,λ) · exp(−σ_t(λ)·d/|cos θ_t|) · (1 − F_glass→air(θ_t,λ))`
    — note: **transmission factors** `(1−F)` at entry/exit (not bare Fresnel
    reflectance), **in-medium path length** `d/|cos θ_t|` (not `d/cos θ_i`), and a
    **conductor reflectance evaluated for glass incidence** (G6).
  - Higher-order **internal reflections** at the top interface (the geometric
    series that **layerlab** sums exactly) are the next term; assert the rendered
    slab matches this leading-order oracle plus shows **no extra interface event**
    (path-length / interface-count probe), and matches **layerlab** for the full
    series.

### 10.2 Classify the crystalline texture's physical layer before modeling (P1)
Top-surface glass dimples and buried near-substrate crystallization are optically
**different**: a bumpy air-glass interface gives **mostly uncolored Fresnel
sparkle** (plus secondary colour mottling from refraction-angle/path-length
variation), whereas buried crystals/relief give **colour-filtered glints** (light
crosses the colored glass before and after). **Protocol:**
1. From the footage, judge whether the sparkle highlights are **colour-neutral/
   white** (→ top-surface relief) or **tinted red** (→ buried), and whether they
   **track the surface** on tilt (top) or show **parallax at depth** (buried).
2. Leading hypothesis from the bright clip = **top-surface orange-peel relief**
   (fired-glass crystallization of the surface) — confirm or refute.
3. Model the chosen layer with **real microgeometry**: top → micro-displacement of
   the dielectric top surface (heightfield/SDF on the air-glass interface);
   buried → a thin scattering sub-layer / micro-relief at the silver. A
   **Worley/cellular normal map is a labelled preview only**, never the physical
   model.

### 10.3 The analytic Beer slab is the ground truth — NM is itself under test (P1)
**Ground truth = the closed-form Beer-Lambert slab**, `T(λ)=exp(−σ_a(λ)·d)` — **not
NM.** NM and HWSS are *both* estimators to validate against it. This matters because
the base medium estimator has a credible **double-attenuation risk for pure
absorbers**: PT samples a free-flight distance from σ_t
([HomogeneousMedium.cpp:132](src/Library/Materials/HomogeneousMedium.cpp:132)) —
so reaching the surface already happens with probability `exp(−σ_t·d)` — and then
the no-scatter surface-hit branch **multiplies Beer transmittance again**
([PathTracingIntegrator.cpp:1665](src/Library/Shaders/PathTracingIntegrator.cpp:1665)).
If those are not algebraically cancelling (analog estimator should weight the
survived path by 1, not by `exp(−σ_t·d)`), an absorbing enamel renders **too dark**.
**Gate (part of G1):** (1) validate the **base non-HWSS NM** estimator against the
analytic slab for a **pure absorber** (σ_s=0) across thicknesses/angles — fix the
estimator if it fails; (2) only then compare **HWSS** against NM+analytic. RISE's
HWSS volume path samples free-flight at the **hero wavelength**
([PathTracingIntegrator.cpp:3586,4256](src/Library/Shaders/PathTracingIntegrator.cpp:3586)),
so with wavelength-dependent σ (G1) the per-wavelength survival diverges from the
hero's. **Per the project principle (§11) we *fix* the HWSS reweighting for
wavelength-dependent media** (spectral-MIS / residual-ratio reweighting of the
hero free-flight — a genuine RISE accuracy improvement), not "permanently fall back
to NM." NM is the *validated reference* while the fix is built; correct HWSS
(matching NM+analytic within MC noise) is the goal. (Consistent with the HWSS
spectral-bundle-bias notes in CLAUDE.md.)

### 10.4 Units & scene-scale for σ — RESOLVED: inverse scene units (P2)
**Settled by the code:** `HomogeneousMedium::EvalTransmittance` evaluates
`exp(−σ_t · dist)` on the **raw** ray distance ([HomogeneousMedium.cpp:200](src/Library/Materials/HomogeneousMedium.cpp:200)),
`SampleDistance` likewise; `scene_unit` is parser/camera state and is **not**
consumed by the medium path ([AsciiSceneParser.cpp:564](src/Library/Parsers/AsciiSceneParser.cpp:564)).
Therefore **all σ values are in inverse scene units** (the doc-comment "[1/m]" is
nominal). **Authoring rule:** convert a physical coefficient to renderer units by
`σ[1/scene-unit] = σ[1/m] × scene_unit[m/unit]`. For watch_dial
(`scene_unit = 0.00079167`): e.g. a measured `σ_a = 5000 1/m` → `≈ 3.96` per scene
unit; an enamel thickness of 300 µm = `0.0003 m / 0.00079167 ≈ 0.379` scene units
→ optical depth `≈ 1.5` (sane).

**Convert measured data to a *volume* coefficient correctly (review):** a measured
*external* transmittance includes the sample's **front+back surface Fresnel losses**
(~4 % per air→glass face at n≈1.5). RISE applies that air→glass Fresnel **separately**
at the dielectric interface, so feeding `−ln(T_external)/d` into σ_a would
**double-count** the surface losses. Use the **internal transmittance** `T_int`
(Fresnel-corrected: `T_int = T_external / T_surface`, or take vendor "internal
transmittance" data directly), then `σ_a(λ) = −ln T_int(λ) / d`. And mind the
**decadic vs natural** convention: if the source is **absorbance `A`** (usually base-10),
`σ_a(λ) = ln(10)·A(λ) / d ≈ 2.303·A(λ)/d`, not `A/d`. Then convert to inverse scene
units (×`scene_unit`) and **fit concentration × thickness** to the dial. Parametric/
Gaussian bands only as **fits to a (Fresnel-corrected) measured curve**, never freehand.
(True SI authoring — scaling σ by `scene_unit` in the medium path — is a separate
convenience feature.) **Sanity test still required:** a slab of known thickness must
attenuate by the predicted amount **with the surface Fresnel modeled, not baked in.**

### 10.5 Opacifier scattering — Mie-derived, HG labelled as fallback (P2)
The general/physical path for opaque/white enamel derives **σ_s(λ), σ_a(λ), and the
angular phase function from Mie theory** (particle radius distribution, particle
n(λ), matrix n, volume loading — §1 has TiO₂/SnO₂/zircon sizes & indices). **A single
Henyey-Greenstein `g` is itself an approximation to the Mie angular lobe** (which is
forward-peaked with structured side/back lobes), and RISE's `IPhaseFunction` has **no
wavelength parameter** today — `Evaluate(wi,wo)`/`Sample`/`Pdf` only
([IPhaseFunction.h:63](src/Library/Interfaces/IPhaseFunction.h:63)). So the **no-corners
fix is a tabulated/spectral Mie phase function** (per-wavelength angular distribution),
which **requires extending `IPhaseFunction` with a wavelength argument** (a vtable/ABI
event — same discipline as G6). HG-`g` remains an explicitly *labelled* fallback for
quick previews.

**The interface arg is the small part — the renderer plumbing is the real work
(review).** Phase `Sample`/`Pdf`/`Evaluate` are called **without a wavelength** at ~31
sites across PT ([PathTracingIntegrator.cpp:1544,4313](src/Library/Shaders/PathTracingIntegrator.cpp:1544)),
BDPT ([BDPTIntegrator.cpp:1723](src/Library/Shaders/BDPTIntegrator.cpp:1723)), and the
RayCaster shader-dispatch path; and BDPT's stored-subpath machinery **explicitly assumes
a wavelength-independent medium phase** — "phase function is wavelength-independent
(ratio = 1.0 for scattering)" ([BDPTIntegrator.cpp:6089](src/Library/Shaders/BDPTIntegrator.cpp:6089)).
So a spectral phase function **silently degrades to wavelength-independent in every
non-hero path** unless we thread `nm` through: **(a)** every phase `Sample`/`Pdf`/
`Evaluate` call (PT, BDPT, MLT-via-BDPT, RayCaster); **(b)** the **stored medium-subpath
vertices and the HWSS companion-wavelength recomputation** (the `ratio = 1.0`
assumption breaks — companion vertices must re-evaluate the phase at *their* wavelength);
and **(c)** the **reverse PDFs / MIS** (the dVCM/dVC recurrence carries per-wavelength
phase). This is a substantial cross-integrator change, **sequenced with the opaque/white
work** (the hero is PT-spectral with σ_s≈0, so the phase function is rarely invoked and
this isn't a hero blocker) — but when built it is threaded **everywhere**, not just on
the hero path, or it's a corner cut.

**Authoring path is also missing, not just the interface (review).** Today a medium's
phase is a **string** `phase = isotropic | hg <g>` ([AsciiSceneParser.cpp:5998](src/Library/Parsers/AsciiSceneParser.cpp:5998));
`IJob::AddHomogeneousMedium` takes only `(phase_type, phase_g)` ([IJob.h:1454](src/Library/Interfaces/IJob.h:1454));
and `Job::AddHomogeneousMedium` constructs **only** isotropic/HG ([Job.cpp:5371](src/Library/Job.cpp:5371)).
There is **no way to bind a tabulated/measured phase function to a medium.** So the
no-corners item also includes a **constructible phase-function authoring path**: either
**named phase-function chunks + a manager** (a `phase_function` chunk one can reference,
like painters), or a **richer medium phase reference** that accepts a phase-function
object — threaded through the parser descriptor, `IJob`/`RISE_API`, `Job.cpp`, with
tests. Priority: **low for the hero** (translucent red fumé → low σ_s,
absorption-dominated) but **load-bearing for opaque/white** enamel — sequenced after
the hero, built properly (real Mie + a real authoring path, not a fitted `g`) per §11.

### 10.6 G6 — conductor Fresnel must use the IOR stack as the ambient index (P1)
RISE's GGX conductor Fresnel hardcodes the incident medium as air: `Ni =
RISEPel(1,1,1)` ([GGXSPF.cpp:266](src/Library/Materials/GGXSPF.cpp:266)), `1.0` in
the NM path ([GGXSPF.cpp:509](src/Library/Materials/GGXSPF.cpp:509)), and the same
in `GGXBRDF`. The buried silver substrate is viewed **through the enamel**, so its
reflectance must be evaluated for **glass incidence** `Ni = n_enamel(λ)` — silver
under n≈1.55 glass reflects measurably *less* than silver in air.

**The general, correct source of the ambient IOR is the IOR stack** — `ior_stack.top()`
at the conductor hit. With the burial convention the ray is *inside* the enamel
dielectric when it strikes the silver, so `ior_stack.top()` **is** `n_enamel`, and
because `DielectricSPF` pushes the IOR evaluated at the current wavelength, the NM
path's `ior_stack.top()` is `n_enamel(λ)` — so reading the stack gives the right value
**automatically and with dispersion**, no authoring required. This is strictly better
than a scene-authored constant. So the fix is **"read `ior_stack.top()`"**, applied at
every conductor ambient-IOR site:
- **(i) `GGXSPF` sampling Fresnel (RGB + NM)** and **(ii) `Pdf`/`PdfNM`** — these
  **already receive `const IORStack&`** ([ISPF.h:135,162](src/Library/Interfaces/ISPF.h:135);
  `GGXSPF::Scatter`/`ScatterNM` take it [GGXSPF.cpp:127,384](src/Library/Materials/GGXSPF.cpp:127)),
  so this is a **zero-interface-change** edit: stop hardcoding air, read the top.
- **(iii) `GGXBRDF::value()`/`valueNM()` Fresnel** and **(iv) the Kulla-Conty
  multiscatter `F_avg`** computed there (today hardcoded air —
  `ComputeFresnelAvg(n, RISEPel(1,1,1), …)` [GGXBRDF.cpp:288](src/Library/Materials/GGXBRDF.cpp:288),
  `(n, 1.0, …)` [GGXBRDF.cpp:435](src/Library/Materials/GGXBRDF.cpp:435)) — the **one
  real gap**: `IBSDF::value()`/`valueNM()` receive **no IOR stack** ([IBSDF.h:55](src/Library/Interfaces/IBSDF.h:55)).
  The integrator *does* hold the stack at the `value()` call site (NEE/MIS at a known
  hit), so the fix is to **thread the ambient IOR (`ior_stack.top()`, a single
  scalar/`Ni(λ)`) into the BSDF-evaluation interface.** That keeps `Scatter`, `Pdf`,
  and `value` consistent (required for correct MIS) and makes *any conductor in any
  medium* correct — a real flexibility win. It is a vtable/ABI event (see the
  accuracy/ABI note below).
- **(v) any albedo/AOV reflectance helper.** Also **verify
  `MicrofacetEnergyLUT::ComputeFresnelAvg` honours a non-air `Ni`** (computes it, not a
  LUT keyed on air); if the energy LUT is air-baked, that's extra work.

`Optics::CalculateConductorReflectance` already exposes `Ni`
([Optics.h:53](src/Library/Utilities/Optics.h:53)), so the per-site change is just
passing `ior_stack.top()` instead of `1.0`. A scene-authored **`incident_ior`
`IScalarPainter` is kept only as an *optional override*** (default = read the stack)
for media not modeled as a nested dielectric, or for diagnostics — **not** required for
the enamel. **Accuracy note — NM is dispersion-correct, RGB is representative-grade.**
`IORStack::top()` returns a **single `Scalar`** ([IORStack.h:184](src/Library/Utilities/IORStack.h:184)).
In the NM path `DielectricSPF` pushes `GetValueAtNM(nm)` ([DielectricSPF.h:127](src/Library/Materials/DielectricSPF.h:127)),
so the stack top is the true `n_enamel(λ)` — **exact** for the spectral hero. In the
RGB/Pel path the stack carries one representative scalar IOR, so the conductor's
ambient index is **representative-grade** (no per-channel ambient dispersion). The
enamel scene renders in **spectral NM** (it must, for the colorants), so RGB is only a
preview path — acceptable, but flagged. The **no-corners RGB option** is a per-channel
ambient-IOR carrier (store a triple in the stack / push per-channel); sequenced, not
needed for the hero.

This is **G6** in §5; prerequisite for the §10.1 oracle's `R_glass→metal` term. The
`value()` interface change is a **vtable/ABI event**: adding a parameter to a pure
virtual on an abstract interface (`IBSDF`) changes the vtable layout and **breaks
out-of-tree subclasses that override it**, so it follows the **`abi-preserving-api-evolution`
skill** (invoke via the Skill tool; no `docs/skills/` companion exists — the playbook
lives in the skill itself). Practically: add the ambient-IOR parameter with a
default-valued overload or a new virtual that the base implements by delegating, so
existing overriders keep compiling; audit every `IBSDF` subclass and every call site.
(The SPF-side reads are **not** ABI events — the stack is already a parameter; just
read it.)

### 10.7 Medium coefficients need a coordinate-free curve, then a volume mapping (P2)
**Even the homogeneous hero cannot ride a surface `IScalarPainter`.** `IMedium`
queries coefficients at a **world-space `Point3`** ([IMedium.h:90](src/Library/Interfaces/IMedium.h:90)),
but `IScalarPainter::GetValueAtNM` requires a **surface `RayIntersectionGeometric`**
([IScalarPainter.h:133](src/Library/Interfaces/IScalarPainter.h:133)) — a medium event
has no surface hit. So the G1 authoring path must be a **coordinate-free spectral-curve
abstraction** (a `λ → value` interface that takes *no* `ri` — e.g. an `ISpectralCurve`
backed by piecewise-linear/`.spectra`/Sellmeier-like data), and the medium chunk must
**reject surface/UV/spatial scalar painters** for its coefficients. Otherwise an
implementer will either **synthesize a fake surface hit** to call `GetValueAtNM` (a
hack, and meaningless UVs) or **silently allow a UV-driven painter in a volume** (wrong
by construction). The hero is exactly this coordinate-free λ-curve.

**Spatial variation is the further step.** For **spatially-varying** enamel (cloisonné
cells, painted work, concentration-graded fumé), the coordinate-free curve gains a
**position argument** — but mapped from the medium's `Point3`, not a surface UV:
`IScalarPainter` evaluates at a **surface `RayIntersectionGeometric`** (hit
UVs/normal) ([IScalarPainter.h:124](src/Library/Interfaces/IScalarPainter.h:124)).
A medium event has no surface UV. So spatial-×-spectral coefficients require a
**volume-coordinate or object-local mapping** (e.g. project the `Point3` into the
dial's local frame / a 2D dial coordinate, or a 3D field), authored as a distinct
abstraction — **not** a surface painter.

**But coordinate mapping is only half the gap (review).** The spatial path is
`HeterogeneousMedium`, which **also collapses spectral to luminance** — in *three*
places: the per-point coefficients `c.sigma_* = Luminance(m_max_sigma_*)·density`
([HeterogeneousMedium.cpp:236](src/Library/Materials/HeterogeneousMedium.cpp:236)),
the **delta-tracking majorant** ([:379](src/Library/Materials/HeterogeneousMedium.cpp:379)),
and the **distance-sampling σ_t** ([:598](src/Library/Materials/HeterogeneousMedium.cpp:598)).
So even with a correct coordinate map, cloisonné/painted enamel would render
**spectrally gray** — the same class of bug as G1, but in the heterogeneous path and
its *tracking* machinery. **No-corners scope for the spatial medium therefore = (a)
the coordinate mapping + (b) true `σ_a(x,λ)`/`σ_s(x,λ)` + (c) spectral majorants for
delta/ratio tracking + (d) spectral distance PDFs + (e) a spectral transmittance
validation** (the §10.3 analytic gate, extended to a heterogeneous slab). The hero
needs only the homogeneous λ-curve, so this is **sequenced after** it — but per §11 it
is **built properly**, not a permanently-accepted limitation, and not mis-scoped as
"just a painter" or "just a coordinate map."

### 10.8 Buried thin-film/oxide accents — fixed via G6, sequenced after the hero (P3)
G6 fixes the **bare-conductor** Fresnel ambient IOR, but the **thin-film** GGX paths
*also* hardcode an **air ambient** (`1.0, 0.0`) — [GGXSPF.cpp:498](src/Library/Materials/GGXSPF.cpp:498),
`GGXBRDF` likewise, with a comment stating "ambient = air (1+0i)". So a
**heat-tinted / oxide / iridescent accent placed *under* the enamel** (e.g. a
flinqué dial with an anodized substrate) would be shaded with the wrong ambient.
Per the project principle (§11) the **G6 ambient-IOR fix extends to the thin-film
Airy stack too** — so buried thin-film becomes correct rather than carved out. It's
not *needed* for the hero (no buried oxide), so it's sequenced after the bare-
conductor path, but it is **fixed, not excluded**. Thin-film accents in **air**
(above the crystal, exposed bezel) are already correct.

### 10.9 Dielectric `tau` must be white when using `interior_medium` (P1)
`DielectricMaterial::tau` is **not** a flat interface tint — it is applied as
**Beer-Lambert distance attenuation** `kray = pow(tau, distance)` on the exit ray
([DielectricSPF.cpp:218](src/Library/Materials/DielectricSPF.cpp:218) RGB,
[:315](src/Library/Materials/DielectricSPF.cpp:315) NM). The `interior_medium`
applies its **own** Beer transmittance over the same path. So a colored top-dielectric
`tau` **double-counts** the enamel's absorption (renders too dark / wrong hue).
**Hard rule:** author the top dielectric's `tau` as an **explicit `tau 1.0`** (or a
named `scalar_painter { value 1.0 }`); **all colour absorption lives in
`HomogeneousMedium` σ_a(λ)**. **Do NOT omit `tau` and do NOT use `none`:** omitting
it defaults to `"none"` ([AsciiSceneParser.cpp:3223](src/Library/Parsers/AsciiSceneParser.cpp:3223)),
which resolves to the **black legacy `IPainter`** that the scalar resolver **rejects**
([Job.cpp:2745](src/Library/Job.cpp:2745)) → the material **fails to construct**; and
`tau` is a **scalar** applied as `pow(tau, distance)`, so any value < 1 attenuates and
**0 would render the glass fully black**. Ideally add a scene-lint warning when a
non-unity `tau` co-occurs with an `interior_medium`.

### 10.10 Silver substrate GGX must set `diffuse = black` (P1)
RISE's GGX adds a **diffuse lobe unconditionally** — `diffuse = pDiffuse->GetColor(ri)
· INV_PI` ([GGXBRDF.cpp:299](src/Library/Materials/GGXBRDF.cpp:299)); only the
Schlick-F0 *modulation* is conditional, so **conductor mode still adds whatever
diffuse painter is bound**. A silver substrate with a non-black `diffuse` would
become "glass over conductor **plus** a fake Lambertian backing." **Hard rule for
the substrate material:** `diffuse = black`, `specular = white/unity`,
`fresnel_mode conductor`, **measured silver n/k**; the ambient IOR is supplied
**automatically by G6 from the IOR stack** (`n_enamel(λ)`), nothing to author (§10.6).
(A non-zero diffuse is only ever wanted for the opaque/painted-enamel *ground*,
never for the reflective metal substrate.)

### 10.11 `transparent_shadows` is preview-only — never in final/reference renders (P1)
`transparent_shadows` is an explicit **approximation**: the shadow ray passes
**straight through** dielectrics (no refractive bend), **ignores internal
multi-bounce**, and uses one representative eta ([RayCaster.cpp:1421](src/Library/Rendering/RayCaster.cpp:1421)).
It's a NEE-variance shortcut — exactly the kind of hack this plan's principle forbids on
the scene's transport path. **Rule (no escape hatch):** **all reference, validation,
*and final/beauty* renders run with `transparent_shadows false`.** It is permitted
**only** for a quick interactive *preview*, never for any image we keep or measure. If
direct lighting *through* the crystal/enamel shell is too noisy, fix it the right way —
proper path sampling, **SMS**, light/path **guiding**, more samples — **not** by
re-enabling straight-through shadow rays. (The guilloché watch used
`transparent_shadows true` for its crystal; for enamel we do **not** inherit that, even
for the hero beauty shot — a "spot-check says it's close" is still accepting a known
approximation, which the principle rejects.)

---

## 11. Re-examination against the project principle (2026-06-30)

The principle (top of doc): **improve RISE — more physically accurate, more
flexible — and never cut corners or accept a limitation for the sake of rendering
the scene.** Every "defer / approximate / out-of-scope" call above was re-checked
against it. Each is now one of three kinds: **(F)** a real RISE fix we commit to,
**(E)** a physically-*exact* composition (acceptable, not a corner-cut), or **(S)**
a genuinely separate feature legitimately out of this material's scope.

| Earlier framing | Verdict | Resolution under the principle |
|---|---|---|
| G1: medium gray in spectral mode | **F** | Build genuine σ_a(λ)/σ_s(λ) — committed. |
| **Medium pure-absorber double-attenuation** (§10.3) | **F** | **Fix the estimator** so non-HWSS NM matches the analytic Beer slab; do not validate against a biased reference. |
| **HWSS "fall back to NM"** (§10.3) | **F** | **Fix HWSS free-flight reweighting** for wavelength-dependent σ (spectral-MIS / residual ratio). NM is the reference *while* fixing, not the permanent answer. |
| **G6 "scene-authored fixed `incident_ior`, interface change out of scope"** (§10.6) | **F** | **Read the IOR stack** (`ior_stack.top()` = `n_enamel(λ)`, automatic + dispersion-correct). SPF/Pdf already have the stack (zero-interface); thread the ambient IOR into `IBSDF::value()` (the one ABI event). Cover SPF + BRDF-value + Kulla-Conty `F_avg` + AOV. `incident_ior` demoted to optional override. |
| **Buried thin-film "excluded"** (§10.8) | **F** | **The G6 fix extends to the thin-film Airy stack** — buried oxide accents become correct; merely *sequenced* after the bare-conductor path. |
| **Spatial×spectral medium "deferred"** (§10.7) | **F** | **Build the spatial medium properly: volume-coordinate mapping + fix `HeterogeneousMedium`'s luminance collapse** (coefficients, tracking majorants, distance PDFs — §10.7) + spectral transmittance validation. Coordinate mapping alone leaves it gray. Sequenced after the homogeneous hero, not abandoned. |
| Opacifier HG + gray σ_s (§10.5) | **F** | HG-`g` is a *labelled fallback*; the no-corners fix is Mie-derived σ_s(λ)/σ_a(λ) + a **tabulated/spectral Mie phase function** — needing **(a)** an `IPhaseFunction` wavelength arg (vtable/ABI), **(b)** a **constructible phase-function authoring path** (today only `isotropic\|hg <g>` string), **and (c)** **threading `nm` through ~31 phase call sites + the BDPT stored-vertex/HWSS-companion `ratio=1.0` assumption + reverse-PDF/MIS** ([BDPTIntegrator.cpp:6089](src/Library/Shaders/BDPTIntegrator.cpp:6089)). Not "derive a `g`." Calibration tunes only the unknown loading. |
| Rough-dielectric "use `scattering`-Phong-widening" (G2) | **F (if needed)** | If any enamel reads satin, add a **proper GGX dielectric BTDF** rather than the widening heuristic. Hero is fire-polished, so gated on need — but the real fix, not the heuristic, is the answer. |
| Surface texture as a normal-map (§9 old) | **F** | **Real microgeometry** on the §10.2-classified layer; normal map is preview-only. |
| Enamel/conductor boundary = **burial convention** (§10.1 A) | **E** | Physically *exact* composition of existing primitives (correct transport: dielectric interface → medium → conductor). Acceptable. The full `CoatedConductorBxDF` (§10.1 B) remains the more *flexible* long-term form and stays on the roadmap. |
| Units = inverse scene units (§10.4) | **E** | Exact; SI authoring would be a *convenience* feature, not a correctness fix. |
| `tau 1.0` / `diffuse black` / `transparent_shadows false` rules | **E** | Authoring invariants that keep the physics exact (not approximations). Ideally enforced by scene-lint. |
| Foil/paillon, plique-à-jour, uranium fluorescence, multi-layer (>1) interference | **S** | Different physics/features, not on this material's path — legitimately out of scope (but never claimed *impossible*). |

**Net effect on scope:** the principle converts the "deferrals/approximations"
into **committed RISE improvements** — medium estimator correctness; HWSS colored-media
reweighting; the BSDF-eval ambient-IOR interface (read the IOR stack); the thin-film
ambient path; the **spatial spectral medium *including* fixing `HeterogeneousMedium`'s
luminance collapse**; and a **tabulated/spectral Mie phase function (extending
`IPhaseFunction` with a wavelength arg + a constructible phase-function authoring path
through parser/`IJob`/`Job`/`RISE_API` + threading `nm` through every phase call site,
the BDPT stored-vertex/HWSS-companion recomputation, and reverse-PDF/MIS)**. Two of these (the `IBSDF::value` ambient-IOR
param and the `IPhaseFunction` wavelength arg) are **interface/vtable changes** done
under the `abi-preserving-api-evolution` skill; one (per-channel RGB ambient IOR) is a
flagged preview-grade limitation since the hero renders spectral. v1 for the hero ships
first, but each improvement is *built properly*, not worked around. Only genuinely
separate features (S) and one convenience (SI units) stay out.
