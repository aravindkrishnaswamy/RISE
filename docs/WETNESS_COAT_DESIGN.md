# Wetness and Coat Layering — Water, Dust, and Snow as an Authorable, Summonable Material System

**Status:** **DESIGN — proposed, not ratified.** No code has been written; this
document proposes. Phase 1 is costed and specified to the point where an
implementation brief can be cut from it directly; Phase 2 is specified to the
architecture-decision level with the layering-model choice argued but the
per-lobe algebra left to the implementation; Phase 3 is deliberately left as a
gated sketch and **may be declined**, per the repo's observed-need convention.
**Date:** 2026-08-31.
**Inputs:** a three-report source survey of the RISE tree — the materials and
layering stack
([CompositeSPF.cpp](../src/Library/Materials/CompositeSPF.cpp),
[CompositeMaterial.h](../src/Library/Materials/CompositeMaterial.h),
[PolishedSPF.cpp](../src/Library/Materials/PolishedSPF.cpp),
[GGXSPF.cpp](../src/Library/Materials/GGXSPF.cpp),
[DielectricSPF.cpp](../src/Library/Materials/DielectricSPF.cpp)); the geometry
signals, verb machinery and measured adoption record
([GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md),
`docs/agentic-redesign/88-procedural-texture-expressiveness-candidates.md`,
`docs/agentic-redesign/90-iteration-ratchet.md`,
[AgentSession.cpp](../src/Library/Agent/AgentSession.cpp),
[AgentMcpAdapter.cpp](../src/Library/Agent/AgentMcpAdapter.cpp)); the painter
stack, coverage-mask surface and pooling idioms
([ExpressionEval.h](../src/Library/Painters/ExpressionEval.h),
[ExpressionPainter.h](../src/Library/Painters/ExpressionPainter.h),
[BlendPainter.h](../src/Library/Painters/BlendPainter.h),
[PiecewiseLinearScalarPainter.h](../src/Library/Painters/PiecewiseLinearScalarPainter.h),
[ChunkParserRegistry.cpp](../src/Library/Parsers/ChunkParserRegistry.cpp),
`scenes/FeatureBased/Combined/tidepools.RISEscene`). Plus the prior-art check
that turned up the load-bearing measurement this design rests on:
[PHYSICALLY_BASED_PIPELINE_PLAN.md](PHYSICALLY_BASED_PIPELINE_PLAN.md) §"Landing
6" and [tests/LayeredWhiteFurnaceTest.cpp](../tests/LayeredWhiteFurnaceTest.cpp).
Contract conformance targets: [MATERIALS.md](MATERIALS.md),
[ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md). External prior art:
Ångström 1925, Lekner & Dorf 1988, Jensen–Legakis–Dorsey 1999,
Pope & Fry 1997, Weidlich & Wilkie 2007, Belcour 2018, and the OpenPBR surface
specification — enumerated in §15.
**Nature:** research + design recommendation, in the mold of
[UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md) (survey →
candidates against evidence → recommendation) and
[GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md)
(observed-need-gated phases with per-phase exit gates). **The user drives
implementation; this document proposes.**

---

## 1. The question, and the answer

Wetness is the single most requested "make it look real" surface state after
wear, and it is the one lookdev state that is *not* a texture: rain darkens and
saturates a substrate, lays a mirror-flat film over it, pools in the low places,
and leaves the ridges dry. Every part of that is geometry-conditioned. RISE
today can render a puddle — `tidepools.RISEscene` does — but only by hand-placing
a separate water object, and it cannot make one surface be wet in the crevices
and dry on the ridges at all.

**The answer: RISE has every physical piece and no assembled system, and the
assembly divides cleanly into a cheap authoring win now and one principled
material later.**

Three specific gaps, each verified in source:

1. **`composite_material` cannot carry a coat.** It is a stochastic random-walk
   compositor, not a layered BSDF. Its `Pdf` is an admitted flat 50/50 average
   of the two sub-SPF PDFs
   ([CompositeSPF.cpp:297-309](../src/Library/Materials/CompositeSPF.cpp)), and
   its `GetBSDF()` takes the top layer's BSDF whole and discards the bottom's
   ([CompositeMaterial.h:57-63](../src/Library/Materials/CompositeMaterial.h)),
   so NEE and BDPT connections through a composite never see the combined layer
   response. This is not a theoretical objection: RISE has **measured** the
   consequence. The dielectric-over-Lambertian configuration — exactly the
   wet-film-over-substrate geometry — retains **ρ(0°) = 0.040** in the white
   furnace ([PHYSICALLY_BASED_PIPELINE_PLAN.md:647](PHYSICALLY_BASED_PIPELINE_PLAN.md),
   [tests/LayeredWhiteFurnaceTest.cpp](../tests/LayeredWhiteFurnaceTest.cpp)
   config 3). It loses **96 % of the energy at normal incidence.**
2. **There is no spatial material-blend primitive.** A grep for `mix_material`,
   `blend_material`, `MaskMaterial` across `src/Library/` returns nothing, and
   `standard_object` binds exactly one `material`
   ([ChunkParserRegistry.cpp:8674,8871](../src/Library/Parsers/ChunkParserRegistry.cpp)).
   Partial wetness — the thing that makes wet look wet — cannot switch BSDFs by
   mask today.
3. **`polished_material` is the closest precedent and is genuinely close.** Its
   own header describes it as "a diffuse substrate with a thin dielectric
   covering" ([PolishedMaterial.h:3-4](../src/Library/Materials/PolishedMaterial.h)),
   and its energy split is a real per-direction Fresnel split — coat lobe
   `tau · Rs` ([PolishedSPF.cpp:191](../src/Library/Materials/PolishedSPF.cpp)),
   substrate lobe `Rd · (1 − Rs)`
   ([PolishedSPF.cpp:242](../src/Library/Materials/PolishedSPF.cpp)) — not an
   additive double-count. What it lacks is coat absorption, coat roughness as a
   microfacet lobe, and a first-class coverage weight.

**Decision (recommended): a two-track plan, verb first, material second.**

- **Phase 1 — ship an `add_wetness` verb and its recipe on existing primitives.**
  Zero new material classes, zero build-project edits beyond
  `ChunkParserRegistry.cpp`-adjacent agent files. The verb rewrites a qualifying
  material into a wetness composition built from `polished_material` +
  `expression_painter` + `scalar_painter { expression }`, with a two-mask
  (damp / wet) prelude driven by `curv`, `occlusion()` and `fbm`. Ships alongside
  a water-absorption spectrum data file and a documented geometry-level pooling
  recipe.
- **Phase 2 — a real `coated_material`**, gated on Phase-1 evidence: one new
  Material+BRDF+SPF triad with its own closed-form `IBSDF::value`/`valueNM`, an
  OpenPBR-shaped parameterization whose `coat_weight` is a spatially varying
  `IScalarPainter` — which **subsumes** the missing material-mix primitive for
  the wet case, because a coverage-weighted coat *is* a statistical mixture of
  coated and uncoated states. Second paying customer: the glTF importer's
  clearcoat/sheen layering, sitting `#if 0`-disabled at
  [GLTFSceneImporter.cpp:1341](../src/Library/Importers/GLTFSceneImporter.cpp)
  waiting for exactly this.
- **Phase 3+ — dust, snow, oil-slick sheen — observed-need gated and may be
  declined**, reusing Phase 1's coverage machinery unchanged.

**The order is not arbitrary.** The measured adoption laws (§10) say a design
note or a documentation recipe converts approximately zero times, and a named
zero-argument verb converts measurably. Building the principled material first
and hoping authors find it inverts the only causality RISE has actually measured.

---

## 2. The physics, distilled

This section is the survey the rest of the document argues from. Where RISE can
express something today it is noted inline; where it cannot, §3 picks it up.

### 2.1 Wet darkening — why a wet substrate goes dark and saturated

A dry porous substrate — brick, unglazed stone, cloth, soil, concrete — scatters
strongly at its first interface because the refractive contrast between air
(n ≈ 1.0) and the substrate (n ≈ 1.5) is large, and because its micro-relief
presents that interface at every orientation. Wet it, and two things happen.

**(i) Index matching.** Water (n ≈ 1.33) fills the pores and the roughness. The
first interface a photon meets is now air→water (contrast 1.33/1.0) rather than
air→substrate (1.5/1.0), and the second is water→substrate (1.5/1.33 — a much
weaker contrast than 1.5/1.0). Less light is turned around at the surface; more
of it is driven into the substrate, where each additional scattering event is
another chance to be absorbed.

**(ii) Total internal reflection at the top.** This is the dominant mechanism,
and it is the one Lekner & Dorf 1988 made analytic. Light that has scattered
inside the substrate and is trying to leave must cross the water→air interface.
For n = 1.33 the critical angle is **48.6°**, so *every* ray arriving at that
interface beyond 48.6° from the normal is totally internally reflected back into
the substrate for another absorption round. Integrated over a diffuse
(Lambertian) internal radiance distribution, roughly **half** the exitant flux is
recycled. Repeat, and the substrate's effective albedo falls well below its dry
value.

**The analytic form, and the fit renderers actually use.** Summing the recycling
series gives a Saunderson-type rational expression — this is Lekner & Dorf's
actual result, not the exponent:

```
R_wet(λ)  ≈  (1 − r_e)(1 − r_i) · R(λ)  /  (1 − r_i · R(λ))
```

where `r_e` is the external Fresnel reflectance at the air–water interface
(≈ 0.02–0.06 depending on the incident distribution) and `r_i` is the
**hemispherically averaged internal reflectance** at the water–air interface —
for n = 1.33, `r_i ≈ 0.44–0.47`, of which the pure TIR fraction alone is
`1 − 1/n² = 0.435`. That `r_i` is §2.1(ii)'s recycling coefficient, and it is
the single most important number in this document: it reappears in §7.4 as the
reason a layered coat model *must* carry a multiple-scattering term.

The exponent form is a **fit** to that curve, not a derivation of it:

```
albedo_wet(λ) ≈ albedo_dry(λ) ^ k,     k ≈ 1.0 … 2.0
```

**The implied `k` is not constant — it varies with the base albedo.** Solving the
rational form for `k` at `r_i = 0.47` **and `r_e = 0`** gives `k ≈ 1.33` at
`R = 0.2` and `k ≈ 1.73` at `R = 0.8`. Carrying a realistic external term instead
(`r_e ≈ 0.02–0.06`) shifts both up, to roughly `k ≈ 1.35` and `k ≈ 1.82–2.01` —
the spread is what matters here, not the endpoints, but quote the `r_e` whenever
quoting the numbers. Two consequences for the recipe: a single `k` is only right over a
narrow albedo range, and applying a **fixed per-channel `k`** over-boosts
saturation on a substrate that is already saturated — the dark channel of a
strongly coloured base gets pushed further than the rational form would push it.
§6.6's table and the verb's default exist inside those limits, not outside them.

Ångström 1925 is the empirical origin of the exponent form; Lekner & Dorf 1988 is
the analytic model above; Jensen, Legakis & Dorsey 1999 is the two-layer
subsurface treatment; Twomey, Bohren & Mergenthaler 1986 is the independent
atmospheric-optics derivation of the same recycling argument.

**Darkening requires a porous, dielectric, scattering substrate.** Both
mechanisms are about light *entering* the substrate and being absorbed over extra
path length. A metal has no subsurface to enter — a wet metal gets the film's
gloss and essentially none of the darkening. The recipe must therefore **gate the
darkening on non-metallic bases** (§6.4), or it will invent an albedo drop that
the physics does not produce.

**Why the exponent form matters for a spectral renderer.** Exponentiating a
*spectrum* rather than an RGB triple does two things at once: it darkens (every
sample below 1 goes down) and it **increases saturation** (samples near the
substrate's reflectance peak fall proportionally less than samples in its
absorption troughs, so contrast across the spectrum grows). That chroma boost is
the second half of what makes a wet surface look wet, and in a spectral renderer
it is the *same operation* as the darkening rather than a separate hand-tuned
saturation control.

> **Honest caveat — RISE cannot do the spectral form in Phase 1.**
> `expression_painter` evaluates its program in **Rec.709 linear RGB and then
> uplifts the result per-sample** through the JH table
> ([ExpressionPainter.h:175-181](../src/Library/Painters/ExpressionPainter.h)),
> and the expression VM has no `nm` context variable — the nine names it resolves
> are `u`, `v`, `P`, `Po`, `N`, `fw`, `time`, `curv`, `curvR`
> ([ExpressionEval.h:675-692](../src/Library/Painters/ExpressionEval.h)). So
> `pow(base, k)` written in an `expression_painter` is a **per-channel RGB
> approximation**, exactly as it would be in an RGB renderer. It still produces
> darkening and *a* chroma boost, but the boost is the RGB-gamut one, not the
> spectral one. **True per-wavelength wet-darkening is a Phase-2 capability**,
> reached inside `coated_material`'s own `ScatterNM`/`valueNM` where the
> substrate's `IPainter::GetColorNM(ri, nm)` is available and can be raised to
> `k` at the hero wavelength. This is a genuine and defensible Phase-2
> differentiator; it is *not*, as one might assume, free today.

### 2.2 The water film

Above the darkened substrate sits the film itself: a smooth dielectric layer at
n ≈ 1.33.

- **Only *pooled* water levels; a wetting film conforms.** This distinction is
  load-bearing and easy to get wrong. A thin wetting film follows the substrate's
  relief wherever that relief is larger than the film is thick — a damp cobble is
  a *shiny bumpy* cobble, not a mirror. Only water deep enough to submerge the
  relief lies flat and reflects like a mirror. So the coat's roughness should be
  keyed on the **pooling** term, not on wetness generally: damp → the base's own
  relief with a sharper lobe; pooled → genuinely smooth. Practical film α ≈
  0.01–0.05 in pools, tending back toward the base's α as the film thins.
- **Fresnel is low at normal incidence and steep at grazing.** F₀ for n = 1.33 is
  ((1.33−1)/(1.33+1))² ≈ **0.020**, rising toward 1 near 90°. This is the whole
  reason a wet street reads as a mirror down the road and merely dark underfoot,
  and it is why an angle-*dependent* energy split matters more here than in most
  materials (§6.2).
- **Its thickness is optically irrelevant except for tint.** A real water film is
  micrometres to millimetres thick — orders of magnitude beyond the coherence
  length of daylight — so there is **no thin-film interference.**

> **Do not conflate the film with iridescence.** Interference colours need films
> around 100 nm. RISE's GGX `film_ior` / `film_extinction` / `film_thickness`
> Airy-stack slots are the right tool for that regime and the wrong tool here;
> [THIN_FILM_INTERFERENCE.md:87-89](THIN_FILM_INTERFERENCE.md) already draws the
> distinction in RISE's own words — "Clearcoat is a thick, *incoherent*
> dielectric layer (a separate lobe). Thin-film is a thin, *coherent* interface
> effect". Oil-on-water sheen *is* the interference case and belongs to Phase 3
> (§8.3), reusing those existing slots.

### 2.3 Partial wetness needs two masks, not one

Coverage is a spatially varying fraction c ∈ [0,1], and the physically observed
drying asymmetry is the design constraint:

**The gloss disappears before the darkening does.** A surface that has stopped
being glossy is still damp — the film has evaporated or soaked in, but the pore
water that drives the TIR recycling is still there. Anyone who has watched a
patio dry has seen this: a shrinking bright patch inside a much larger dark
patch, and the dark patch outlives it by minutes.

Therefore the design carries **two masks**, not one:

| mask | drives | semantics |
|---|---|---|
| `damp` | reflectance darkening / saturation | pore water present |
| `wet` | film gloss (coat weight, coat roughness) | free-surface film present |

with the invariant **`wet ⊆ damp`** — that is, `wet ≤ damp` pointwise. It is
**enforced by emitting one byte-identical prelude into every consumer chunk** and
deriving `wet = clamp(damp · film_amount, 0, damp)` inside it, with
`film_amount ∈ [0,1]`. Both halves of that sentence are load-bearing: painters
cannot reference each other's `def` names, so the prelude must be *duplicated*
verbatim rather than shared (§6.1), and the outer `clamp(…, 0, damp)` is what
makes the invariant hold for *every* parameter setting rather than merely the
default one. A single scalar `dryness` knob then sweeps a believable drying
sequence from one control, which is the difference between a parameter and a toy.

**Meniscus edges are sub-pixel physics.** The dark ring at a puddle's rim comes
from the thickening water wedge and the surface-tension curvature; both are far
below a pixel at any sane resolution. Represent them with mask breakup noise plus
a slight darkening term keyed on the mask gradient. Do not simulate them.

### 2.4 Pooling — water finds level, and finds *down*

Water accumulates in concavities and rests horizontal, flat, and mirror-smooth.
Two honest treatment levels, not interchangeable:

- **Shading-level (cheap, approximate).** Drive coverage up and roughness down in
  cavities using an occlusion signal. Works for damp cobbles, wet pavement, a
  rain-slicked wall. It does **not** produce a flat surface: the "puddle" follows
  the underlying geometry's normals, so a lumpy substrate gives a lumpy mirror
  instead of a level one. Acceptable at cobblestone scale; visibly wrong at pond
  scale.
- **A cavity term alone is orientation-blind, and that is a visible bug.**
  `occlusion()` answers "is this point sheltered", not "does water stay here" —
  it is just as high on the underside of an arch or a coffered ceiling as in a
  floor hollow, so a cavity-only mask **pools water on ceilings**. Gravity has to
  enter the mask explicitly, as an up-facing term multiplying the cavity term
  (§6.3). Worth naming the asymmetry: the dust sibling (§8.1) needs the *same*
  up-facing term for the same reason, and it is the more obvious case only
  because nobody expects dust on a ceiling either.
- **Geometry-level (correct, more setup).** A real flat dielectric surface object
  in the concavity — reflections, refraction, and the level horizon are then
  correct by construction, because the renderer is tracing a water surface.
  **Already RISE's live idiom**, in two shipped scenes (§3.8).

Complementary, not competing; both are worth shipping.

### 2.5 Deep water tint

Water's absorption coefficient rises steeply through the visible. Using Pope &
Fry 1997 (the standard reference for pure-water absorption over 380–700 nm):
a(450) ≈ **0.009–0.015 m⁻¹**, a(650) ≈ **0.34 m⁻¹**, a(700) ≈ **0.624 m⁻¹** — a
blue-to-red ratio near **two orders of magnitude**, not one. That is why depth
reads blue-green, and why a centimetre of water on a pavement is visually
colourless while a metre of it is not. In RISE this is Beer–Lambert through a
dielectric's `tau`, an `IScalarPainter` bindable to a measured curve without JH
uplift (§3.5, §6.7).

> **Honest caveat — this is spectral only under a spectral rasterizer.**
> `PiecewiseLinearScalarPainter` (the class a `scalar_painter { file … }`
> produces) serves the spectral path through `GetValueAtNM(nm)` → `EvalAtNM(nm)`,
> but its RGB entry point `GetValuesAt` **broadcasts a single representative
> sample** — `EvalAtNM(kRepresentativeNm)` with `kRepresentativeNm = 555.0`
> ([PiecewiseLinearScalarPainter.h:52,89-93](../src/Library/Painters/PiecewiseLinearScalarPainter.h)),
> and reports `HasPerChannelVariation() = false` (`:105`). So under
> `pathtracing_pel_rasterizer` — the default, and the one the worked example and
> the census use — a measured water curve collapses to **one grey number** and the
> depth tint vanishes entirely. §6.7 gives the honest RGB idiom.

### 2.6 Dust

A thin layer of high-albedo, strongly-forward-scattering particulate that becomes
optically thick enough to hide the substrate well before it is thick enough to
matter geometrically. Appearance signature: desaturate and lighten toward grey,
raise roughness sharply, kill the specular highlight, weight by up-facing (N·up)
and by shelter (cavities and undersides rain never washes). **Dust needs no coat
physics at all** — it is param modulation on the existing BSDF, and the *cheapest*
of the three siblings, not the hardest.

### 2.7 Snow

High albedo (0.8–0.95), whose whiteness comes from dense multiple scattering
between ice grains rather than a high single-scatter albedo, which is why it
resists a convincing flat-BRDF treatment. Shadowed snow reads blue for **two**
reasons and the everyday one is the illuminant: a shadowed patch is lit by blue
skylight rather than by the sun, and snow's near-unity albedo reports that
illuminant faithfully. Ice's own absorption spectrum is the *second* cause and
dominates only at depth — glacier ice, crevasses, deep drifts. It also sparkles:
individual facets
catch the sun as discrete glints. Anything beyond a dusting has **real
thickness** — it fills concavities, rounds edges, casts its own shadow — so it is
a geometry problem. Thin frost reuses the dust machinery with different
constants; everything else is deferred (§8.2).

---

## 3. What RISE has today

### 3.1 Prior-art check — the doc-90 discipline, applied

`docs/agentic-redesign/90-iteration-ratchet.md` §2 records a design doc that
specified a mechanism which had already shipped, been measured harmful, and been
deleted; the implementation worker refused it with evidence, and the lesson
recorded was that the doc's author had not checked. This survey therefore
explicitly searched before designing.

**What was found:**

- **No prior wetness feature, verb, scene, skill, or design doc exists.** A grep
  for `wetness`, `add_wetness`, `coated_material`, `coat_weight`, `puddle` across
  `docs/` and `src/Library/` returns nothing but incidental prose.
- **The glTF importer's layered-material block exists and is compiled out** —
  [GLTFSceneImporter.cpp:1341](../src/Library/Importers/GLTFSceneImporter.cpp)
  (`#if 0 // Phase 5 — preserved for the layered-composite work that comes next.`)
  through `:1469`: a sheen top and a `schlick_f0` GGX clearcoat top, each wrapped
  in a `CompositeMaterial`. Waiting on precisely the material Phase 2 proposes.
- **The layering problem has already been measured**, and the reports this design
  was briefed from did not surface it.
  [PHYSICALLY_BASED_PIPELINE_PLAN.md](PHYSICALLY_BASED_PIPELINE_PLAN.md)
  §"Landing 6" (`:607`) holds a shipped white-furnace audit of eight layered
  configurations at 100 000 samples per (config, angle), with
  [tests/LayeredWhiteFurnaceTest.cpp](../tests/LayeredWhiteFurnaceTest.cpp) as the
  living harness. Findings A and D are exactly this design's problem, stated
  months earlier and with numbers. §3.2 uses them.
- **Two shipped scenes already build water as geometry** (§3.8), and one carries a
  comment recording the partial-wetness gap in an author's own words (§3.6).

The material conclusion of the check: **this design is not greenfield the way the
signals design was.** It is the assembly step on top of an existing, measured,
partially-diagnosed layering problem — and the measurement changes the
recommendation (it rules out one whole option class outright, §4a).

### 3.2 `composite_material` — a random walk, measured broken for this case

**What it is.** `CompositeSPF::Scatter` dispatches to `ProcessTopLayer` or
`ProcessBottomLayer` by which side the ray arrives from
([CompositeSPF.cpp:252-272](../src/Library/Materials/CompositeSPF.cpp)); each
processor calls the corresponding sub-SPF's `Scatter`, forwards upward-exiting
rays out, and recursively hands downward rays to the other layer after a
Beer–Lambert attenuation across `thickness`
([CompositeSPF.cpp:111-114](../src/Library/Materials/CompositeSPF.cpp)),
budget-gated per `ScatteredRay` type. A genuine stochastic transport model between
two boundary SPFs with an absorbing gap — but not a layered BSDF.

**Three disqualifying properties for a coat:**

1. **`Pdf` is an admitted heuristic.** The code says so in its own comment — "the
   composite SPF uses a random walk between top and bottom layers, making exact
   lobe weights impractical to compute analytically. Equal weighting is the best
   approximation for this material" — and returns
   `0.5 · (pdf_top + pdf_bottom)`
   ([CompositeSPF.cpp:297-309](../src/Library/Materials/CompositeSPF.cpp), with
   `PdfNM` identical at `:311-322`). That is flat 50/50 regardless of Fresnel
   weight, thickness, or extinction. MIS built on it is not weighting what it
   thinks it is weighting.
2. **`GetBSDF()` is top-wins.** The constructor takes the top material's BSDF if
   it has one, else the bottom's, and never combines
   ([CompositeMaterial.h:57-63](../src/Library/Materials/CompositeMaterial.h)).
   Every NEE connection and every BDPT/VCM connection into a composite surface
   evaluates **one layer's** closed-form `f_r`. For a wet surface — a dielectric
   coat whose `GetBSDF()` returns `0`
   ([DielectricMaterial.h:48](../src/Library/Materials/DielectricMaterial.h)) over
   a Lambertian — the composite's BSDF falls through to the substrate's, so
   direct lighting sees a *dry* surface while path-traced indirect sees a wet
   one. That is not a subtle bias; it is two different materials in one image.
3. **It loses 96 % of the energy in the exact configuration wetness needs.** From
   the shipped furnace audit
   ([PHYSICALLY_BASED_PIPELINE_PLAN.md:647](PHYSICALLY_BASED_PIPELINE_PLAN.md)):

   | config | ρ(0°) | ρ(30°) | ρ(60°) | ρ(80°) |
   |---|---|---|---|---|
   | 3 — Composite: dielectric / Lambertian | **0.040** | **0.042** | **0.089** | 0.388 |
   | 7 — Composite: clearcoat / red GGX-PBR | **0.040** | **0.040** | **0.068** | **0.205** |

   Root cause, per Finding A: "the current `CompositeSPF` random walk loses ~96 %
   of energy at normal incidence because dielectric refractions emerge as delta
   rays whose recursion budget collapses before the diffuse below-layer
   scattering bubbles out"
   ([PHYSICALLY_BASED_PIPELINE_PLAN.md:655-662](PHYSICALLY_BASED_PIPELINE_PLAN.md)),
   with `kMaxRecur = 4`, `kMaxDiffuseRecur = 2`. Finding D confirms the same
   failure appears for a near-delta *GGX* top over a coloured diffuse base — so
   it is not a dielectric-specific quirk, it is the whole "smooth coat over
   diffuse" regime, which is the whole of wetness.

**Routing hazard, separately.** `composite_material`'s `extinction` parameter is
typed `const IPainter&`
([CompositeSPF.h:41](../src/Library/Materials/CompositeSPF.h)) and consumed via
`GetColor`/`GetColorNM`. A measured water-absorption curve bound there would be
**JH-uplifted and behave as a colorant, not an absorption coefficient** — the
exact class of bug [ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md) was
written to eliminate. Any new coat must put its absorption on an
`IScalarPainter`.

### 3.3 `polished_material` — the closest existing precedent, and genuinely close

Its header states its purpose: "A polished material is a diffuse substrate with a
thin dielectric covering"
([PolishedMaterial.h:3-4](../src/Library/Materials/PolishedMaterial.h)). Its
`Scatter` is a **single-interface Fresnel energy split**, not an additive
double-count:

- coat lobe: `kray = tau · Rs`
  ([PolishedSPF.cpp:191](../src/Library/Materials/PolishedSPF.cpp)), where `Rs` is
  the dielectric Fresnel reflectance computed from the **incident/view**
  direction and the surface normal — `vIn = ri.ray.Dir()`, then
  `CalculateDielectricReflectance(vIn, vRefracted, normal, …)`
  ([PolishedSPF.cpp:90-96](../src/Library/Materials/PolishedSPF.cpp)) — evaluated
  *before* any Phong/HG cone perturbation of the outgoing lobe, not at the
  sampled direction;
- substrate lobe: `kray = Rd · (1 − Rs)`
  ([PolishedSPF.cpp:242](../src/Library/Materials/PolishedSPF.cpp)), the
  complementary energy, cosine-sampled;
- spectral twins at `:312` and a real `EvaluateKrayNM` for HWSS companion
  throughput at `:335`.

Parameters ([ChunkParserRegistry.cpp:3336-3361](../src/Library/Parsers/ChunkParserRegistry.cpp)):
`reflectance` (`IPainter`, substrate `Rd`), `tau` (`IScalarPainter`, default
`"0.0"`), `ior` (`IScalarPainter`, default `"1.0"`), `scattering`
(`IScalarPainter`, coat lobe cone exponent / HG asymmetry, default `64`),
`henyey-greenstein` (bool).

**Two properties make it the right Phase-1 target and are worth stating
explicitly.**

- **`tau = 0` is exactly "no coat".** The parser's own comment says so: "kray =
  tau*Rs = 0, so the dielectric coat contributes no specular lobe and the
  material reads as plain Lambertian (`reflectance`) until `tau` is set"
  ([ChunkParserRegistry.cpp:3352-3355](../src/Library/Parsers/ChunkParserRegistry.cpp)).
  A `[0,1]` mask bound to `tau` therefore fades a coat in and out spatially.
- **Every knob wetness needs is already on the scalar pipe.** `tau`, `ior`, and
  `scattering` are all `IScalarPainter`, so a `scalar_painter { expression … }`
  can drive all three from geometry signals with no JH hazard.

**What it lacks:** coat absorption (no Beer–Lambert through the film — `tau` is a
flat multiplier on the coat lobe, not a path-length attenuation); a microfacet
coat lobe (`scattering` is a Phong cone / HG lobe, not GGX, so it has no
principled roughness parameterization and no multiple-scattering compensation);
a coverage weight distinct from `tau`; and — the serious one —

> **Honest caveat: `polished_material` carries the same NEE defect used to
> disqualify `composite_material` in §3.2.** Its constructor builds
> `pBRDF = new LambertianBRDF(Rd_)`
> ([PolishedMaterial.h:49](../src/Library/Materials/PolishedMaterial.h)) and
> `GetBSDF()` returns exactly that (`:57`). **The BSDF is the bare substrate
> reflectance — no coat lobe, and not even the `(1 − Rs)` factor the SPF applies.**
> So every NEE connection and every BDPT/VCM connection into a polished surface
> evaluates a *dry Lambertian*, while BSDF-sampled indirect paths see the wet
> Fresnel split. The two disagree by up to the full coat share: at 80° incidence
> `Rs` for n = 1.33 is roughly 0.4, so direct lighting reports the substrate about
> **1.6× brighter** than the sampled transport does, and the discrepancy grows
> toward grazing where wetness is most visible.
>
> This is a **pre-existing defect of `polished_material`**, not something wetness
> introduces — but wetness makes it matter, because a wet surface is precisely one
> whose grazing energy split is the point. It bounds what Phase 1 can honestly
> claim (§6.2, §6.9) and is the third independent justification for Phase 2 (§7).

### 3.4 `ggx_material` — no coat concept, and its diffuse deduction is angle-flat

A single microfacet layer with a three-lobe mixture (diffuse, anisotropic VNDF
specular, Kulla-Conty multiscatter). **There is no clearcoat concept anywhere in
it** — a grep for `coat_weight` / `coat_ior` / `coat_roughness` across
`src/Library/` finds nothing but the dead glTF field names. `fresnel_mode
thinfilm` changes how the *existing* specular lobe's reflectance is computed; it
does not add a lobe. Three details matter for wetness:

- **In `schlick_f0` mode the diffuse lobe's deduction is `(1 − max F0)`**, a flat
  scalar ([GGXSPF.cpp:210-220](../src/Library/Materials/GGXSPF.cpp), spectral twin
  at `:496-504`). **Be precise about what is and is not flat here:** GGX's
  *specular* lobe has a real per-direction Fresnel, so its highlight does brighten
  toward grazing. What is angle-independent is the **energy the diffuse lobe gives
  up** — a constant deduction, applied only in `schlick_f0` mode. The true
  complaint is therefore energy conservation, not "no grazing response": as the
  specular takes more at grazing, the diffuse does not correspondingly give more
  back, so the pair does not trade energy the way §2.2's film does.
- **`rs` (the F0 slot) is an `IPainter`** — colour pipe, so a spatially varying F0
  mask would be JH-uplifted. Roughness (`alphax`/`alphay`) is `IScalarPainter` and
  is safely mask-drivable.
- GGX already occupies **3 of the 6** `ScatteredRayContainer` slots
  ([ISPF.h:80](../src/Library/Interfaces/ISPF.h): `mutable ScatteredRay rays[6];`).

Where GGX *is* the right answer: when the base is already a `ggx_material` or
`pbr_metallic_roughness_material`, wetness is param modulation on it (roughness
down, reflectance darkened), not a class rewrite. §6.2 makes that the verb's
second branch.

### 3.5 `dielectric_material` — the geometry-level water surface, already correct

`tau`, `ior`, and `scattering` are all `IScalarPainter`
([DielectricSPF.h:6-11](../src/Library/Materials/DielectricSPF.h) records that
this refactor is what stopped glass rendering as "speckled invisible blobs in
every spectral rasterizer"). `tau` is applied as `pow(tau, distance)` per segment
travelled *inside* the medium
([DielectricSPF.cpp:311-324](../src/Library/Materials/DielectricSPF.cpp), spectral
at `:415`) — a genuine per-unit-distance Beer–Lambert base, accumulating across
internal bounces. Binding a measured water-absorption curve to `tau` via
`scalar_painter { file … }` is therefore both available and physically correct
today (§6.7).

`GetBSDF()` returns `0` and `GetSpecularInfo` reports `isSpecular=true,
canRefract=true` — integrators route it through the specular cascade, never NEE.
That is right for a real water surface and is one more reason it is the wrong
class to use as a *coat* on an otherwise-diffuse material.

### 3.6 The painter stack, and the coverage-mask gap

- **`blend_painter` is the only spatial mask-blend primitive and it is colour-pipe
  only** — `a`, `b`, and `mask` are all `const IPainter&`
  ([BlendPainter.h:43-45](../src/Library/Painters/BlendPainter.h)), with
  `GetColor` computing `combined·mask + b·(1−mask)` at `:124-136` and a
  per-wavelength twin at `:138-145`. No `IScalarPainter` masked-blend class
  exists.
- **That gap does not need a new class.** `scalar_painter { expression … }`
  compiles the full VM, and `mix` is a parser special-form
  ([ExpressionEval.h:1138-1140](../src/Library/Painters/ExpressionEval.h)) —
  `mix(dry_alpha, wet_alpha, wet_mask)` is a masked blend on the scalar pipe,
  written as one line. The `add_wear` verb already emits exactly this shape. **A
  `blend_scalar_painter` class would buy nothing and cost the five-build-project
  tax; it is REJECTED in §4c.**
- **The expression VM's function set covers the recipe.** `pow`, `min`, `max`,
  `step` (arity 2); `clamp`, `smoothstep`, `select` (arity 3); `dot`, `length`,
  `fbm`, `turbulence`, `worley_*` (vec3 domain); `occlusion`, `thickness`
  ([ExpressionEval.h:728-757](../src/Library/Painters/ExpressionEval.h)); plus
  `mix` and `vec3` as special forms (`:1138-1140`).
- **The author-observed gap, in an author's own words.** `tidepools.RISEscene`
  carries this comment above its wet-rock material: *"Tau (transmittance) does
  not vary spatially with the perlin diffuse base under IScalarPainter (no
  `texture` form on scalar_painter), so we lock tau to unity here — the spatial
  colour variation still lives in the perlin painter via `reflectance`"*
  (`scenes/FeatureBased/Combined/tidepools.RISEscene:400-404`). That is exactly
  the partial-wetness problem, hit by a real author, and worked around by giving
  up on it.

  > **The comment is now stale, and that is load-bearing.** `scalar_painter` has
  > since gained both a `texture` form (raster painters only —
  > `PainterChannelScalarPainter`'s doc notes a `checker_painter` or
  > `blend_painter` is Color-pipe but *not* a `TexturePainter` and is rejected)
  > and a **`painter` form** — the P2.1 any-painter bridge
  > ([PainterChannelScalarPainter.h](../src/Library/Painters/PainterChannelScalarPainter.h),
  > descriptor at
  > [ChunkParserRegistry.cpp:1605](../src/Library/Parsers/ChunkParserRegistry.cpp))
  > which accepts **any** of the 36 colour painters and channel-selects R/G/B/A.
  > A `perlin2d_painter` bound to `tau` through the `painter` form works today.
  > **Spatially varying coat coverage on `polished_material` is available in the
  > current tree** — nobody has assembled it. That is precisely the thesis of §1,
  > confirmed by a scene comment written before the capability landed and never
  > revisited. Phase 1 should fix this comment as it passes.

### 3.7 The geometry signals — the mask machinery, already shipped

Phases 1–3 of [GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md)
shipped 2026-08-29. Wetness consumes them unchanged:

- **`curv`** — dimensionless mean curvature, **+ convex / − concave / 0 flat**,
  normalized by the hit geometry's bbox diagonal so it reads O(1) at any scene
  scale. Keeps ridges dry.
- **`occlusion(radius)`** — `[0,1]`, **1 = unoccluded**, radius a *fraction* of
  the geometry's own bbox diagonal. Finds the cavities water pools in.
- **`thickness(radius)`** — `[0,1]`, 1 = thick. Not needed by wetness; listed for
  completeness.

**Three traps the recipe must respect, all documented in the descriptor at
[ChunkParserRegistry.cpp:1601](../src/Library/Parsers/ChunkParserRegistry.cpp):**

1. **The radius must be a numeric literal on meshes.** A computed radius (a
   `param`, or any expression) compiles to the dynamic-radius twin, which on an
   indexed mesh reads the **neutral fallback** rather than borrowing a table
   baked at a different scale. `add_wear` writes `occlusion(0.08)` as a literal
   for exactly this reason.
2. **Heightfield-mode `sdf_geometry` returns neutral occlusion.** So does every
   analytic primitive and every non-indexed mesh. The fallback is deliberately
   the *do-nothing* end of the range (occlusion 1 = unoccluded), so a mask built
   on unsupported geometry lights nothing up rather than everything. **This is a
   hard constraint on the pooling recipe** and §6.8 flags it explicitly.
3. **BDPT / VCM / MLT read these signals as neutral in parts of their transport.**
   The descriptor states it plainly and the design doc's §14 item 11 scopes it.
   Wetness inherits this whole. §9 states the visible consequence honestly.

### 3.8 Geometry-level water is already RISE's live idiom

Two shipped scenes build water as an object:

- `scenes/FeatureBased/Combined/tidepools.RISEscene` — a
  `dielectric_material { name water  tau 0.85 0.92 0.95  ior 1.33  scattering
  5000.0 }` (`:392-399`) on a flat squashed `ellipsoid_geometry { radii 1.25 0.04
  1.25 }` (`:451-456`), placed as a separate `standard_object` (`:458-463`) over
  a `polished_material` wet-rock floor. Three objects, three materials, no mask —
  literally the "dry surface plus separate flat water surface" pattern.
- `scenes/FeatureBased/VCM/vcm_sdf_luminaire_jellyfish.RISEscene:496-519` — `displaced_geometry` over a
  flat `box_geometry` driven by a wave `IFunction2D`, `disp_scale 0.25`, with
  `dielectric_material` on top. `scenes/FeatureBased/Textures/receding_pier.RISEscene:129-142` uses the same
  shape.

**Nothing needs to be built for geometry-level pooling. It needs to be written
down and pointed at** (§6.8).

### 3.9 The three gaps, named

| # | Gap | Evidence |
|---|---|---|
| **G1** | No principled coat/layer material. `composite_material` is a random walk with a 50/50 `Pdf`, a top-wins `GetBSDF`, and a **measured 96 % energy loss** in the coat-over-diffuse regime. | [CompositeSPF.cpp:297-309](../src/Library/Materials/CompositeSPF.cpp), [CompositeMaterial.h:57-63](../src/Library/Materials/CompositeMaterial.h), [PHYSICALLY_BASED_PIPELINE_PLAN.md:647,655-662](PHYSICALLY_BASED_PIPELINE_PLAN.md) |
| **G2** | No spatial material-blend primitive; one object binds one material. | grep null for `mix_material`/`blend_material`/`MaskMaterial`; [ChunkParserRegistry.cpp:8674,8871](../src/Library/Parsers/ChunkParserRegistry.cpp) |
| **G3** | The pieces that *do* exist have never been assembled into a wetness idiom, and no verb summons one. | §3.1 prior-art check; the stale `tidepools.RISEscene:400-404` comment |

---

## 4. Architectural options considered

### (a) Fix `CompositeSPF`'s recursion budget, then build wetness on `composite_material` — **DEFERRED, not rejected**

Finding A's own A1 option: patch the random walk's recursion accounting so
delta-mediated paths to the bottom layer are not clipped by the diffuse-recursion
budget ([PHYSICALLY_BASED_PIPELINE_PLAN.md:663-667](PHYSICALLY_BASED_PIPELINE_PLAN.md)).

This is real work with real value — it would unblock the glTF importer's sheen
path too — and it is **not rejected**. But it is not a wetness prerequisite and
it does not by itself make `composite_material` a wetness foundation, because it
leaves the two *architectural* defects untouched: the `Pdf` would still be
50/50, and `GetBSDF()` would still be top-wins, so NEE would still see one
layer. Finding A itself lists A2 ("replace with an analytic layered BSDF") as the
alternative and notes it "produces cleaner BRDF semantics". Phase 2 is A2, scoped
to the coat case. **Deferred: it belongs to whoever owns Landing 6, and if it
lands first Phase 2 benefits but is not made unnecessary.**

### (b) A generic `mix_material { a  b  mask }` spatial-blend material — **REJECTED for v1**

Superficially the cleanest fix for G2, and it would serve dust and snow as well
as wetness. Rejected on three counts:

1. **`coat_weight` subsumes it for the wet case — *conditional on Phase 2
   shipping*.** A coverage-weighted coat is a statistical mixture of coated and
   uncoated states, which is precisely OpenPBR's `coat_weight` semantics; building
   both is building the same thing twice. But this reason evaporates if Phase 2 is
   declined at its gate, because then nothing delivers `coat_weight` at all.
   **If Phase 2 is declined, re-open §4b** — G2 would remain fully open, and a
   generic mix becomes the only route to spatially varying BSDF behaviour.
2. **A correct generic mix is harder than it looks.** It must combine two
   arbitrary BSDFs' `value`, `Pdf`, `Scatter`, `GetSpecularInfo`, `albedo`, and
   `EvaluateKrayNM`, reconcile a delta sub-lobe on one side with a continuum on
   the other, and fit both lobe sets into the 6-slot `ScatteredRayContainer`
   ([ISPF.h:80](../src/Library/Interfaces/ISPF.h)) — a GGX pair alone needs 6.
   (This is unconditional and survives a Phase-2 decline.)
3. **No observed demand outside wetness.** Per the repo's convention, that gates
   it. Revisit in Phase 3 if dust or snow demonstrably need it (§8.4).

### (c) A `blend_scalar_painter` (`IScalarPainter` masked mix) — **REJECTED**

The obvious symmetric completion of `blend_painter`. Rejected because
`scalar_painter { expression mix(a, b, m) }` already does it in one line
([ExpressionEval.h:1138-1140](../src/Library/Painters/ExpressionEval.h)), and the
class would cost the full five-build-project tax
([CLAUDE.md](../CLAUDE.md) "Source-file add/remove") for zero new capability.
Additionally, the P2.1 `painter` form
([PainterChannelScalarPainter.h](../src/Library/Painters/PainterChannelScalarPainter.h))
already bridges any colour painter — including an existing `blend_painter` — onto
the scalar pipe, so even the "reuse my colour mask" case is covered.

### (d) Ship a documented wetness recipe only, no verb — **REJECTED by measurement**

The tempting cheap option: write the recipe into
`materials-and-media-basics.md`, add a design-note diagnostic, done.

**This is measured to convert approximately zero times.** Law **C-ADV** (doc 88
§2): "Design notes / Info diagnostics naming `scalar_painter` fired up to
30×/session, were demonstrably read, and drove **0/24 lifetime adoptions** across
two models and multiple framings. Voluntary/opt-in tools: 0/64." Law **C-TYPE**:
worked examples lift painter *diversity* (models copy examples) but moved
spatially-varying roughness **0/24** — because the failure is a slot-typing prior,
not ignorance. Law **C-READ**: agents pull 2–5 skills by hook line, and
`procedural-textures.md` was read **0/6** in one probe batch.

Documentation is necessary and insufficient. It ships *with* the verb, in the
proven read-set, never instead of it.

### (e) Build `coated_material` first, add the verb once it exists — **REJECTED on ordering**

Intellectually the tidier sequence, and the reason to reject it is empirical
rather than aesthetic. The one causality RISE has measured on this exact
instrument is **C-VERB**: shipping the `add_wear` verb moved curv-driven wear
from **1/6 → 3/6** runs, occlusion from **0/6 → 3/6**, position-proxy fakes from
**3 → 0/6**, and produced gpt's first-ever strict-gate pass on
`rich_material_closeup` (0 % → 33.3 % report-level pass@1)
([GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md) §11
VERB-EFFECT CENSUS). Building the material first means paying the largest cost in
the plan before learning whether the capability gets summoned at all.

Verb-first also *derisks* the material: Phase 1's census tells us which wetness
knobs authors actually reach for, which is the parameterization Phase 2 should
have.

### (f) **Verb first on existing primitives; `coated_material` second, gated — RECOMMENDED**

Phase 1 is buildable today against `polished_material` + the scalar-pipe
expression VM + the shipped geometry signals, with **zero new material classes**.
Phase 2 is the principled upgrade, scoped by Phase 1's evidence, and carries a
second paying customer in the glTF importer. Phase 3 is gated and may be
declined. §5 states it; §6–§8 specify it.

### (g) A painter-sampling route for the darkening — **ADOPTED as a Phase-1.5 candidate, scoped**

Phase 1's darkening is written as `pow(base_r, k)` over **literal constants** the
verb reads out of a `uniformcolor_painter`. That works only because the base is a
flat colour. **The expression VM cannot sample another painter** — its function
table ([ExpressionEval.h:728-757](../src/Library/Painters/ExpressionEval.h)) has
arithmetic, noise, and the two geometry-signal builtins, and nothing that takes a
painter reference or a texture. So a *textured* substrate — cobbles from a
`png_painter`, wood grain from an `expression_painter`, anything procedural — gets
the coat and **no darkening at all**, which is the more visible half of wetness.
That is a real scope hole in Phase 1 (§6.9), not a nicety.

Two shapes could close it, both far below Phase 2's cost:

- **A `pow`/exponent painter op** — a colour painter taking a source painter and
  an exponent, evaluating `GetColor(ri)^k` and `GetColorNM(ri,nm)^k`. Small,
  composable, and note that its `GetColorNM` form would be **genuinely
  per-wavelength**, which is exactly §2.1's principled operation.
- **A painter-sample builtin in the VM** — richer, but it means threading painter
  references into `CallFunc`, which is the same `const`-context plumbing Phase 2 of
  the signals work already did once.

**Adopted as a candidate, deliberately not scheduled here.** The first shape is
the better bet (smaller, and it kills debt 6 as a side effect for the darkening
term specifically).

**Its decision criterion is the textured-albedo refusal rate** — §10.3's dedicated
counter, added to Phase 1's exit gate. The obvious criterion ("ship it if wetness
on textured substrates reads wrong") is **unreachable by construction**, because
§6.4 clause 2 makes a textured albedo a *hard refusal*: no wrong-looking render
can ever be produced, so no render can ever motivate the fix. What is observable
is how often authors ask and get turned away. If that count is materially
non-zero, (g) is the cheapest fix in this document; if authors only ever wet flat
colours, it is not needed.

### (h) Expose `nm` as a tenth expression context variable — **REJECTED**

The reviewer's alternative to (g): give the VM a wavelength, so a painter body
could write per-wavelength expressions directly and debt 6 would close without
Phase 2. Attractive in the abstract, rejected on three grounds:

1. **It contradicts `ExpressionPainter`'s architecture, not just its
   implementation.** The colour pipe is defined as "evaluate in Rec.709 RGB, then
   uplift the result per sample"
   ([ExpressionPainter.h:175-181](../src/Library/Painters/ExpressionPainter.h)).
   An `nm`-aware body would make `GetColor` and `GetColorNM` compute *different
   functions*, so the RGB and spectral renders of one scene would diverge by
   construction — with no way for the painter to report that they should.
2. **It invites exactly the class of bug
   [ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md) removed.** Hand-written
   per-wavelength colour maths in a slot that then gets JH-uplifted is how
   physical magnitudes ended up mangled the first time.
3. **It does not actually serve the use case.** Wet darkening needs the
   *substrate's* spectrum raised to a power; `nm` alone gives a wavelength but
   still no way to sample the substrate painter (that is (g)'s problem, unsolved).

`nm` on the **scalar** pipe is a narrower and more defensible idea — no uplift, no
RGB/spectral divergence — but nothing in this design needs it, so it stays
unproposed.

---

## 5. The recommendation

**Track 1 — `add_wetness`, a zero-required-argument mutating verb, plus its
recipe, its data file, and its documentation, on existing primitives.**
It rewrites one qualifying material into a two-mask wetness composition and
refuses byte-identical when it cannot. Cost: the eight-surface verb-plumbing
checklist, no new `src/Library/` classes, no five-build-project tax. Delivers:
damp darkening, film gloss, cavity pooling, dry ridges, and a documented
geometry-level pooling recipe pointing at the two shipped scenes that already do
it.

**Track 2 — `coated_material`, gated on Phase-1 evidence.**
One Material+BRDF+SPF triad with its own closed-form `IBSDF::value`/`valueNM`, an
OpenPBR-shaped parameterization, spatially varying `coat_weight` on the scalar
pipe, coat absorption via Beer–Lambert through a real thickness, a GGX coat lobe
with roughness, and Weidlich–Wilkie-style analytic energy accounting plus a
Kulla-Conty-style compensation term. Closes G1 and G2-for-the-wet-case, unblocks
the glTF `#if 0` block, and is the first place RISE can do §2.1's spectral
exponentiation properly.

**Track 1.5 — a painter-`pow` op (§4(g)), adopted as a candidate, not scheduled.**
Phase 1 can only darken a *flat-colour* substrate, because the expression VM
cannot sample a painter. A small colour-painter exponent op would lift that limit
and, in its `GetColorNM` form, deliver §2.1's per-wavelength operation early. Its
go/no-go signal is the **textured-albedo refusal rate** measured in Phase 1
(§10.3), not a render.

**Track 3 — dust, snow, oil sheen — observed-need gated, may be declined.**

---

## 6. Phase 1 — the `add_wetness` verb

### 6.1 What it emits

The verb follows `add_wear`'s mechanical pattern exactly: operate on the
`RISE::Cst::Document`, resolve the target material's `NodeId` *before* any
structural change, rebind the slots, then splice the new field chunks in at the
material's own item index so declarations precede consumers — one composite
document swap, one head-version bump, one undo step
([AgentSession.cpp:35695-35711](../src/Library/Agent/AgentSession.cpp)).

For a Lambertian base it emits, in document order, **three painters and one
material**:

1. an **`expression_painter`** for the darkened, saturation-boosted
   **reflectance**, ending in the `damp` mask;
2. a **`scalar_painter { expression … }`** for **`tau`** — the coat coverage,
   ending in the `wet` mask;
3. a **`scalar_painter { expression … }`** for **`scattering`** — the coat cone
   exponent, ending in the *pooling* term (§2.2: only pooled water levels);
4. the rewritten **`polished_material`** binding those three, with a literal
   `ior 1.33` (a constant needs no painter).

There is deliberately **no standalone "damp mask" chunk**: nothing would consume
it. **Painters cannot reference one another's `param` / `def` names** — the
expression VM's environment is per-program — so the shared prelude must be
**duplicated byte-identically into all three** consumers rather than declared
once. That duplication is not redundancy to be optimized away; it is the
mechanism that makes §2.3's `wet ⊆ damp` invariant hold, and `add_wear` makes the
same choice for the same reason: "Byte-identical `param` and `def` lines, one
`seed`, so the two chunks agree by construction rather than by careful editing"
([AgentSession.cpp:35428-35433](../src/Library/Agent/AgentSession.cpp)).

Every knob is a named `param` with `min`/`max`/`step`/`label` plus a `seed`, so
retuning is one `propose_patch` on a named line — the idiom `add_wear` already
establishes and that authors are meant to copy.

### 6.2 The emitted target — `polished_material`, and why not GGX

This is the load-bearing choice in Phase 1, so it is argued rather than asserted.

**Recommendation: rewrite Lambertian bases to `polished_material`; do *not*
rewrite GGX/PBR bases at all — modulate their existing params in place.**

**Why `polished_material` for the diffuse case:**

1. **Its documented purpose is the wet-film geometry.** "A diffuse substrate with
   a thin dielectric covering"
   ([PolishedMaterial.h:3-4](../src/Library/Materials/PolishedMaterial.h)).
2. **Its SPF energy split is per-direction Fresnel — for BSDF-sampled paths.**
   The substrate lobe carries `Rd·(1−Rs)` with `Rs` computed from the incident
   direction ([PolishedSPF.cpp:242](../src/Library/Materials/PolishedSPF.cpp),
   `Rs` at `:90-96`), so **indirect and BSDF-sampled transport** reproduces §2.2's
   grazing rise: the substrate gives up energy toward grazing as the coat takes
   more. GGX in `schlick_f0` mode instead deducts a constant `(1 − max F0)` from
   its diffuse lobe ([GGXSPF.cpp:210-220](../src/Library/Materials/GGXSPF.cpp)),
   so the pair never trades energy with angle at all.
   **This claim is explicitly scoped and is *not* decisive on its own**: it holds
   for the sampled path only. Direct lighting through `GetBSDF()` sees a bare
   `LambertianBRDF(Rd)` with no `(1−Rs)` factor
   ([PolishedMaterial.h:49,57](../src/Library/Materials/PolishedMaterial.h)),
   so NEE gets no grazing rise from either candidate. The honest form of the
   argument is: polished gets the angular behaviour right on the half of transport
   where any existing RISE material could, and Phase 2 is what gets it right on
   the other half.
3. **Every wetness knob is already on the scalar pipe.** `tau`, `ior`,
   `scattering` are all `IScalarPainter`
   ([ChunkParserRegistry.cpp:3348-3356](../src/Library/Parsers/ChunkParserRegistry.cpp)),
   so masks reach them with no JH hazard. GGX's F0 (`rs`) is `IPainter`.
4. **`tau = 0` is exactly "dry".** Parser comment at
   [ChunkParserRegistry.cpp:3352-3355](../src/Library/Parsers/ChunkParserRegistry.cpp).
   The coverage mask has a physically meaningful zero.
5. **It has a real `EvaluateKrayNM`**
   ([PolishedSPF.cpp:335](../src/Library/Materials/PolishedSPF.cpp)), so the HWSS
   companion path is correct. **`GetBSDF()` is non-null but is *not* correct for a
   coated surface** — it is a bare `LambertianBRDF(Rd)`
   ([PolishedMaterial.h:49,57](../src/Library/Materials/PolishedMaterial.h)),
   so NEE has something to call but what it calls is the dry substrate (§3.3's
   caveat). Counted here as "no worse than the alternatives", not as a strength:
   `ggx_material` would give NEE a genuinely combined lobe, which is the one axis
   on which GGX wins and is why §7 lists a correct `value()` as a Phase-2
   justification in its own right.

**Why *not* rewrite GGX/PBR bases:** it would discard the microfacet lobe, the
anisotropy, and the Kulla-Conty compensation, replacing a better specular model
with a Phong cone. For those bases the honest edit is param modulation — but
**the slot and the painter pipe both differ by material kind, and getting either
wrong fails silently**:

| base kind | microsurface slot(s) | mask painter to emit |
|---|---|---|
| `ggx_material` | `alphax` **and** `alphay` (anisotropy is these two directly) | `scalar_painter { expression … }` — both are `IScalarPainter` |
| `pbr_metallic_roughness_material` | **`roughness`** (there is no `alphax`/`alphay` here; anisotropy is `anisotropy_factor`) | **`expression_painter`** — see the trap below |

Drive the chosen slot toward 0.02–0.05 where wet, and darken the base colour via
the same `expression_painter`.

> **Trap — PBR-MR's `roughness` and `metallic` are Colour-pipe by construction,
> and binding a `scalar_painter` to them silently yields ZERO.** The descriptor
> says so in its own semantics note: *"Color pipe by construction, not by meaning:
> `Job::AddPBRMetallicRoughnessMaterial`'s `resolveOrSynth()` checks
> `pPntManager->GetItem()` and falls back to `atof()` + a synthesized
> uniform-colour painter on a miss — a `scalar_painter` name here is NOT found
> (wrong manager) and silently synthesizes a ZERO-valued painter rather than
> binding"*
> ([ChunkParserRegistry.cpp:4162-4163](../src/Library/Parsers/ChunkParserRegistry.cpp),
> which points at [docs/gui/MATERIAL_EDITOR.md](gui/MATERIAL_EDITOR.md) §6.4's "pbr's colour-manager roughness"
> oddball). So the PBR branch **must emit an `expression_painter`** for its
> roughness mask. A `scalar_painter` there does not error — it produces a
> mirror-smooth material everywhere, wet and dry alike, which looks like a
> spectacularly successful wetness effect and is in fact the mask never binding
> at all.

> **State the in-place branch's limits plainly — it is an approximation of a
> different shape, not a lesser version of the polished one.** Sharpening a GGX
> base's own lobe is **not** adding a water film. It sharpens the *substrate's*
> specular, carrying the substrate's own F0 and its tint, where a real film would
> add a **white, n = 1.33, F₀ ≈ 0.02** lobe on top. Three consequences: a wet
> copper reads as *polished copper* rather than copper-under-water; **anisotropy
> is destroyed** where the mask drives `alphax`/`alphay` together on a
> `ggx_material` (brushed metal loses its streak exactly where it gets wet, which
> is backwards); and it
> **couples the film's roughness to the substrate's**, which §2.2 says is the one
> thing water decouples. It is also restricted to **non-metallic** bases, since
> §2.1's darkening has no mechanism on a metal. This branch is a stopgap that
> looks plausible on rough dielectric PBR and wrong on anything else — **"wetness
> over a GGX/PBR base" is therefore an explicit Phase-2 gate criterion** (§13):
> if the census shows authors reaching for it, that alone justifies
> `coated_material`, whose coat lobe is a real separate film.

**Oren-Nayar bases get damp-only.** `PolishedSPF`'s substrate lobe is strictly
Lambertian, so an Oren-Nayar → polished rewrite silently drops the retroreflective
term the author chose. For v1 the verb applies **darkening only** to an Oren-Nayar
base and says so in its message — which is less a compromise than the honest
reading of §2.3: darkening-without-film is the damp state, and a rough matte
substrate is where the damp state is most convincing.

> **Honest caveat — using `tau` as a coverage weight loses a little energy at
> intermediate coverage.** `tau` multiplies only the coat lobe
> ([PolishedSPF.cpp:191](../src/Library/Materials/PolishedSPF.cpp)); the substrate
> lobe's weight stays `(1 − Rs)`
> ([:242](../src/Library/Materials/PolishedSPF.cpp)) rather than becoming
> `(1 − tau·Rs)`. Writing the deficit out: a true coverage mixture would return
> `Rd·(1 − c·Rs)` from the substrate, the material returns `Rd·(1 − Rs)`, so the
> missing term is **`Rd · Rs · (1 − c)`** — carrying `Rd`, which the shorthand
> "`(1−c)·Rs`" drops. At normal incidence with n = 1.33, `Rs ≈ 0.020`, so the
> deficit is **≤ 2 % of `Rd`** — invisible. At grazing `Rs → 1` and a half-wet
> pixel loses up to ~50 % of `Rd`. **That expression is an upper bound on this
> one term** (it is largest at `c = 0`, `Rs = 1`, `Rd = 1`); it becomes a *lower
> bound on the total* loss only once the geometric-horizon lobe drop below is
> added. Two things make this acceptable for Phase 1: the loss is
> bounded, angle-localized, and *darkens* (never brightens, never fireflies); and
> it is three orders of magnitude better than the 96 % normal-incidence loss the
> `composite_material` route measures at
> ([PHYSICALLY_BASED_PIPELINE_PLAN.md:647](PHYSICALLY_BASED_PIPELINE_PLAN.md)).
> **`coated_material`'s `coat_weight` exists to fix precisely this, and this
> caveat is one of the three concrete reasons Phase 2 is worth building.**
>
> The `Rd·Rs·(1−c)` term does not exhaust the grazing loss: `PolishedSPF`
> additionally *drops* a non-delta coat lobe outright when it
> falls below the ray-anchored geometric horizon — "plain rejection, no fallback"
> ([PolishedSPF.cpp:206-207](../src/Library/Materials/PolishedSPF.cpp)) — while
> the substrate lobe has already been debited its `(1 − Rs)`
> ([:242](../src/Library/Materials/PolishedSPF.cpp)). That is a second,
> grazing-concentrated loss on top. The §13 furnace configuration is what will put
> a number on the total; do not quote the analytic bound as if it were the
> measurement.

### 6.3 The mask prelude

Copying `add_wear`'s composition shape
([GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md) §13
Phase 4), the shared prelude is:

```
param  dryness       0.15  min 0 max 1 step 0.01 label "Dryness"
param  pool_gain     1.60  min 0 max 4 step 0.05 label "Pooling in cavities"
param  ridge_shed    2.20  min 0 max 6 step 0.1  label "Ridges shed water"
param  base_wetness  0.55  min 0 max 1 step 0.01 label "Overall wetness"
param  gravity_bias  0.80  min 0 max 1 step 0.01 label "Water needs up-facing"
param  film_amount   1.00  min 0 max 1 step 0.01 label "Film vs merely damp"
param  film_gloss_lo 220.0 min 60 max 400 step 5 label "Damp-film gloss (Phong n)"
param  breakup_amp   0.18  min 0 max 1 step 0.01 label "Mask breakup"
param  breakup_scale 6.0   min 0.1 max 40 step 0.1 label "Breakup scale"
seed   0

def jitter      vec3(seed, seed*1.7, seed*2.3)
def up_facing   clamp(dot(N, vec3(0,1,0)), 0, 1)
def gravity     mix(1.0, up_facing, gravity_bias)
def cavity      (1.0 - occlusion(0.08)) * gravity
def pooling     clamp(pool_gain*cavity, 0, 1)
def ridge       clamp(curv * ridge_shed, 0, 1)
def breakup     breakup_amp * (fbm(P*breakup_scale + jitter, 4, 0.5, 2.0) - 0.5)
def damp_raw    clamp(base_wetness + pooling + breakup - ridge, 0, 1)
def damp        (1.0 - dryness) * smoothstep(0.0, 1.0, damp_raw)
def wet         clamp(damp * film_amount, 0, damp)
```

Reading it against §2:

- `cavity` implements §2.4's shading-level pooling, **gated by `up_facing`** so
  water does not pool on ceilings and overhang undersides (§2.4). `N` is the
  world-space shading normal and the term assumes **+Y up**; a Z-up scene needs
  the axis vector changed, and the verb should say which axis it wrote.
- **`occlusion(0.08)`'s radius is a numeric literal, never a `param`** — a
  computed radius degrades to the neutral fallback on indexed meshes
  ([ChunkParserRegistry.cpp:1601](../src/Library/Parsers/ChunkParserRegistry.cpp)).
  Inherited verbatim from `add_wear`; must be repeated correctly.
- `base_wetness` is the **uniform term**, and it is what makes a flat wet street
  expressible at all: on planar geometry `curv` and `occlusion` are both inert, so
  without it the whole mask would collapse to zero (§6.4 clause 3, §6.9).
- `ridge` implements "convex ridges shed water" via `curv > 0`.
- `breakup` is §2.3's meniscus stand-in — noise, not simulation. The `jitter`
  vec3 follows `add_wear`'s idiom (a per-axis decorrelated offset) rather than
  broadcasting one scalar into all three noise axes.
- **`dryness` multiplies `damp` directly**, so `dryness = 1` drives `damp` — and
  therefore `wet` — to exactly zero for every setting of every other knob. That
  is what makes §6.4's no-op refusal well-founded; the earlier additive form did
  not have this property, because `pooling` and `breakup` survived it.
- **`wet ≤ damp` holds unconditionally**, because `film_amount ∈ [0,1]` *and* the
  outer `clamp(…, 0, damp)` caps it regardless. Both guards are deliberate: a
  bare multiplier is only safe while its bound is respected, and the clamp makes
  the invariant true by construction even if a later edit widens the `param`.

The three consumers each append one line:

```
# reflectance (expression_painter, colour pipe) -- vec3 out
expr  mix( vec3(base_r, base_g, base_b), vec3( pow(base_r, k), pow(base_g, k), pow(base_b, k) ), damp )

# tau (scalar_painter) -- coat coverage
expression  wet

# scattering (scalar_painter) -- Phong exponent, keyed on POOLING not wetness
expression  mix( film_gloss_lo, 200000.0, clamp(pooling * wet, 0, 1) )
```

with `k` a `param` defaulting to **1.55** (§2.1, §6.6).

**AMENDED (2026-08-31) — one physical line per parameter.** An earlier draft of
this listing wrapped the `expr` value across two physical lines for readability.
Execution validation (the §13 step-2 hand-authored scene) proved that form does
not parse: RISE's chunk parser is line-based per-parameter, so a continuation
line reads as an unknown parameter and hard-fails
(`Failed to parse parameter name 'vec3(' (not declared in 'expression_painter'
descriptor)`). Every `expr` / `expression` / `def` value the verb emits — and
every listing in this document — must sit on one physical line, however long.

**The `scattering` line keys on `pooling`, not on `wet`, and that is the §2.2
correction**: a thin wetting film conforms to the substrate's relief, so damp-flat
regions must stay comparatively broad-lobed; only water deep enough to submerge
the relief levels into a mirror. Keying the sharpening on wetness alone would make
every damp surface a mirror — the classic "wet asphalt looks like plastic"
failure.

> **Getting the low end of that band right requires knowing what `scattering`
> *is*, and it is easy to get wrong by two orders of magnitude.**
> `polished_material`'s `scattering` is a **Phong cone exponent**, sampled as
> `alpha = acos(pow(u, 1/(n+1)))` with pdf `(n+1)/2π · cosⁿα`
> ([PolishedSPF.cpp:123-129](../src/Library/Materials/PolishedSPF.cpp)); the
> parser default is **64**
> ([ChunkParserRegistry.cpp:3355](../src/Library/Parsers/ChunkParserRegistry.cpp)).
> The rough correspondence to a GGX roughness is
>
> ```
> α ≈ sqrt( 2 / (n + 2) )        n ≈ 2/α² − 2
> ```
>
> so **§2.2's pooled range α ≈ 0.01–0.05 is n ≈ 20000 down to 800** — and
> `n = 2000` is already α ≈ 0.032, a ~1.8° lobe, i.e. *inside* the pooled range.
> A band running `mix(2000, 200000, …)` is therefore **sharp → sharper**: it never
> visits the damp end at all, and delivers exactly the plastic-looking mirror the
> paragraph above warns against. The low end has to sit near the material's own
> default — hence `film_gloss_lo` defaulting to **220** (α ≈ 0.095), tunable over
> 60–400 (α ≈ 0.18–0.07), with 200 000 reserved for the pooled end. For
> calibration, the one shipped water surface in the tree uses `scattering 5000`
> (α ≈ 0.02) for a *free water surface*
> (`scenes/FeatureBased/Combined/tidepools.RISEscene:398`) — appropriately far
> sharper than a damp film on stone.

**Defaults, stated so they can be audited.** With the values above:
on **planar** geometry (`curv = 0`, occlusion neutral so `pooling = 0`)
`damp_raw = base_wetness = 0.55`, `smoothstep(0,1,0.55) ≈ 0.575`, so
`damp = wet ≈ 0.49` — a uniformly wet street, which is §1's opening image and the
case §6.4 clause 3 exists to admit. In a **pooled cavity** `pooling → 1` saturates
`damp_raw`, giving `damp = wet = 0.85`. `dryness` and `film_amount` are the two
drying knobs: `film_amount` below 1 produces §2.3's damp-but-matte state (gloss
gone, darkening intact), and `dryness → 1` returns the surface to dry. The earlier
draft's `dryness 0.35` / `film_amount 0.85` defaults capped `wet` at 0.55
everywhere and left a flat street at `damp ≈ 0.10` — reading dry, and sitting near
the worst of §6.2's coverage dip by construction.

> **Honest caveat — the darkening is RGB, not spectral.** As established in §2.1,
> `expression_painter` evaluates in Rec.709 linear RGB and JH-uplifts the result
> per sample ([ExpressionPainter.h:175-181](../src/Library/Painters/ExpressionPainter.h)),
> and the VM has no `nm` ([ExpressionEval.h:675-692](../src/Library/Painters/ExpressionEval.h)).
> `pow(base_r, k)` etc. is therefore the per-channel approximation, identical to
> what an RGB renderer would do. It looks right; it is not the principled form.
> Phase 2's layered transport is where RISE gets the principled form (§7.1).

> **Honest caveat — the darkening works on flat-colour albedos only.** Notice what
> `base_r`, `base_g`, `base_b` are: **literal numbers the verb copied out of the
> base `uniformcolor_painter` at rewrite time.** They have to be, because the
> expression VM **cannot sample another painter** — there is no texture or
> painter-reference builtin in its function table
> ([ExpressionEval.h:728-757](../src/Library/Painters/ExpressionEval.h)). So a
> substrate whose albedo is a texture or a procedural painter — patterned cobbles,
> wood grain, anything an author is likely to have already textured — can receive
> the **coat** but **no darkening whatsoever**, which is the more visible half of
> the effect. §6.4 clause 2 turns this into an honest refusal rather than a silent
> half-result, and §4(g) is the cheap fix if the census shows it biting.

### 6.4 Qualifying predicate and refusals

Mirroring `WearMaterial_`
([AgentSession.cpp:3331](../src/Library/Agent/AgentSession.cpp)) and its selector
([:4506](../src/Library/Agent/AgentSession.cpp)), a material qualifies when:

1. it is `lambertian_material`, `orennayar_material`, `ggx_material`, or
   `pbr_metallic_roughness_material`, **and is not metallic** — a constant
   `metallic > ~0.5` (or a conductor-mode GGX) either refuses or, if the author
   named it explicitly, applies the coat and **skips the darkening**, saying so.
   §2.1's mechanisms require a dielectric scattering substrate; a wet metal is a
   filmed metal, not a darkened one;
2. its primary colour slot reads a constant bound to a plain
   `uniformcolor_painter` (not blackbody, not spectral, not a non-default
   colorspace) — so the verb can *derive* the darkened band from a real value.
   **This clause is what makes §6.3's textured-substrate hole an honest refusal:**
   a textured albedo fails it and the verb declines, rather than silently
   delivering gloss with no darkening;
3. it is bound to at least one object with geometry the verb can wet. **This is
   deliberately weaker than `add_wear`'s clause**, which refuses every
   `CurvBarrenGeometryKind_` — a list that includes `infiniteplane_geometry`,
   `circulardisk_geometry`, `cartesian_disk_geometry`, `clippedplane_geometry`
   and `box_geometry`
   ([AgentSession.cpp:3388-3399](../src/Library/Agent/AgentSession.cpp)).
   **That clause must not transfer, and the reason is physical:** wear *is* an
   edge-and-crevice phenomenon, so a curv-barren surface genuinely has nothing to
   wear; wetness is not. A flat road slab is uniformly wet, and a rain-wet street
   is this document's own opening image. With `base_wetness` in the prelude
   (§6.3), planar geometry gets a correct uniform sheet of wetness — only the
   *pooling* and *ridge* variation is inert. So clause 3 requires only a
   non-degenerate bound object, and the verb's message says when the result will
   be uniform rather than varied;
4. it is not already wet (no existing reference to `polished_material` with a
   varying `tau`, and no slot already reading the wetness prelude's `def`
   names).

**Refuse byte-identical, changing nothing**, when: nothing qualifies; a named
material fails a specific clause (say which); the material is already wet; the
base colour is unreadable **or is a painter rather than a constant** (clause 2);
the base is metallic and darkening is the point; a minted chunk name would
collide; or `dryness` resolves to 1 so the composition is a no-op (§6.3's
multiplicative form is what makes that check exact). Every refusal path is
`ok=false` with an empty `status`, so callers branch on `applied` — the convention
`add_wear`'s tool description already teaches
([AgentMcpAdapter.cpp:1848-1901](../src/Library/Agent/AgentMcpAdapter.cpp)).

> **Collision: `add_wetness` and `add_wear` mutually exclude each other, both
> ways — and worn-and-wet is the flagship subject.** `add_wear`'s qualifying
> predicate refuses a material whose slots already reference the geometry signals
> ([AgentSession.cpp:3331](../src/Library/Agent/AgentSession.cpp) clause (d)), so
> a wet material cannot subsequently be worn. And `add_wetness`'s clause 2 above
> requires a plain `uniformcolor_painter` albedo, which a *worn* material no longer
> has — `add_wear` has already rebound it to an `expression_painter`. **Whichever
> verb runs first locks the other out**, and "a weathered bronze door in the rain"
> is precisely the thing an author asks for.
>
> **v1 resolution: accept the exclusion, but make it loud.** Each verb's refusal
> message must **name the other verb** and state that the two cannot currently be
> combined on one material, so the agent gets a real explanation instead of an
> opaque decline — and both verbs' refusal text has to be written and guarded
> together, since the text is duplicated across `AgentMcpAdapter.cpp` and
> `AgentChatCodecs.cpp` by discipline rather than code sharing. The census (§10.3)
> then **counts how often both are wanted on one material**, which is the observed
> need that would justify the follow-up.
>
> **The follow-up, sketched not scheduled: prelude extension in place.** Both
> verbs write the same shape — a `param`/`def` prelude plus a final expression —
> so the second verb could *extend* the existing chunks (append its `param`s and
> `def`s, wrap the existing final expression in its own `mix`) rather than refuse.
> That is a genuinely different and larger mechanic than either verb has today
> (it must parse and re-emit an expression body it did not write), which is why it
> is a follow-up and not v1.

**Paired design-note condition.** One new condition in
[AgentDiagnostic.h](../src/Library/Agent/AgentDiagnostic.h) alongside
`DESIGN_UNWORN_MATERIALS` (`:328`) — call it `DESIGN_DRY_RAIN_SCENE` — sharing
the verb's qualifying scan, firing when the scene's own language (its name, an
element description, an env/lighting cue) implies rain, damp, or a water body
while every qualifying material reads bone-dry. **The note must name the verb**,
not ask for the rewrite: that naming is what the `add_wear` census attributes the
conversion to ("condition L's note lands at trajectory line 3, the `add_wear`
call follows at line 97").

### 6.5 Worked example — rain-wet cobblestones

One example, execution-validated, per the master triad. It ships as
`scenes/FeatureBased/Materials/rainwet_cobbles.RISEscene` and is what the skill
text points at.

**The `param` / `def` prelude below is byte-identical in all three chunks** —
same names, same values, same order, same `seed` — per §6.1. Only the final
`expression` / `expr` line differs. The albedo chunk additionally declares
`k` and `base_*`, which the two scalar chunks do not consume and so do not carry.

```
# --- rain-wet cobbles: pooled joints, damp flats, dry crowns ---
scalar_painter
{
	name			cobble_wet
	param			dryness       0.10  min 0 max 1 step 0.01 label "Dryness"
	param			base_wetness  0.45  min 0 max 1 step 0.01 label "Overall wetness"
	param			pool_gain     2.20  min 0 max 4 step 0.05 label "Pooling"
	param			ridge_shed    2.60  min 0 max 6 step 0.1  label "Crowns shed"
	param			gravity_bias  0.80  min 0 max 1 step 0.01 label "Water needs up-facing"
	param			film_amount   1.00  min 0 max 1 step 0.01 label "Film vs merely damp"
	param			film_gloss_lo 260.0 min 60 max 400 step 5 label "Damp-film gloss (Phong n)"
	param			breakup_amp   0.14  min 0 max 1 step 0.01 label "Breakup"
	param			breakup_scale 9.0   min 0.1 max 40 step 0.1 label "Breakup scale"
	seed			3
	def			jitter    vec3(seed, seed*1.7, seed*2.3)
	def			up_facing clamp(dot(N, vec3(0,1,0)), 0, 1)
	def			gravity   mix(1.0, up_facing, gravity_bias)
	def			cavity    (1.0 - occlusion(0.06)) * gravity
	def			pooling   clamp(pool_gain*cavity, 0, 1)
	def			ridge     clamp(curv * ridge_shed, 0, 1)
	def			breakup   breakup_amp * (fbm(P*breakup_scale + jitter, 4, 0.5, 2.0) - 0.5)
	def			damp_raw  clamp(base_wetness + pooling + breakup - ridge, 0, 1)
	def			damp      (1.0 - dryness) * smoothstep(0.0, 1.0, damp_raw)
	def			wet       clamp(damp * film_amount, 0, damp)
	expression		wet
}

scalar_painter
{
	name			cobble_gloss
	# ... prelude repeated verbatim ...
	expression		mix(film_gloss_lo, 200000.0, clamp(pooling * wet, 0, 1))
}

expression_painter
{
	name			cobble_albedo
	param			k             1.70  min 1 max 2.5 step 0.05 label "Wet exponent"
	param			base_r        0.42  min 0 max 1 step 0.01 label "Base R"
	param			base_g        0.40  min 0 max 1 step 0.01 label "Base G"
	param			base_b        0.37  min 0 max 1 step 0.01 label "Base B"
	# ... prelude repeated verbatim ...
	expr			mix( vec3(base_r, base_g, base_b), vec3(pow(base_r,k), pow(base_g,k), pow(base_b,k)), damp )
}

polished_material
{
	name			cobble_wet_stone
	reflectance		cobble_albedo
	tau			cobble_wet
	ior			1.33
	scattering		cobble_gloss
}
```

**What each clause buys, and what it does not:**

- **Pooled mirror in the joints** — `pooling` saturates `damp_raw`, so `tau`
  reaches **0.90** and `scattering` runs to ≈180 000 (the `mix` lands at
  `pooling·wet = 0.9`, not 1.0; α ≈ 0.003 either way, effectively a
  mirror): the joints get a sharp, near-full-strength coat lobe. The crowns and
  flats keep `film_gloss_lo = 260` (α ≈ 0.087) — glossy but still reading the
  stone's relief, per §2.2. This is the §2.4 shading-level approximation: the
  "pool" follows the cobble geometry's own normals rather than lying level. At
  cobblestone scale that is correct enough to be invisible; at pond scale it is
  not, and §6.8 is the answer there.
- **Damp-dark on the flats** — `base_wetness` gives a baseline `damp` everywhere,
  so the whole street darkens and saturates even where there is no standing
  water. It is also what lets this recipe work at all on a flat slab (§6.4
  clause 3).
- **Dry crowns** — `curv > 0` on the cobble tops subtracts from `damp`.
- **Water stays down** — `up_facing` keeps the pooling term off vertical faces and
  undersides (§2.4). This scene is **+Y up**; a Z-up scene needs that axis vector
  changed, and the verb must report which axis it wrote.
- **Scale trap — this reads correctly only because the cobbles are their own
  geometry.** `occlusion(r)`'s radius is a fraction of the **hit geometry's own**
  bounding-box diagonal. On a single 40 m street mesh, `0.06` is a **~2.5 m**
  query: it would report the street's overall bowl, not the 3 cm joints between
  stones, and the pooled-joint look would simply not appear. The recipe assumes
  per-cobble objects or a tiled patch instanced across the street; an author
  wetting one large slab must scale the radius down by roughly the ratio of
  feature size to mesh size, or accept a large-scale pooling read instead.
  Calibration target when sizing the radius: the **cobble pitch**
  (centre-to-centre spacing), not the joint's literal gap width — a query radius
  a few times the gap but under half the pitch still discriminates joint from
  crown correctly (execution-verified on the shipped 17-part patch: bbox
  diagonal ≈ 2.23, `occlusion(0.06)` ≈ 0.134-unit query vs ≈ 0.04 gap /
  0.35 pitch).
- **"Dry under an overhang" is the honest limitation.** `occlusion()` is
  **self-occlusion only**; cross-object AO was DECLINED for v1 on principle
  ([GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md) §8).
  A doorway keeping the pavement dry beneath it is *not* expressible from
  geometry signals. The two available routes are (a) model the overhang and the
  pavement as one geometry so its concavity is self-occlusion, or (b) author the
  dry patch as a texture mask through the P2.1 `painter` bridge. **Do not claim
  the verb does this.** The verb's message should say so when the target object's
  scene has other objects above it.

### 6.6 Where the darkening exponent comes from

`k` is a per-substrate porosity number, and the verb should ship a small table in
its documentation rather than a single default:

| substrate | `k` |
|---|---|
| glazed tile, sealed concrete, painted metal, varnished wood | **1.0 – 1.1** |
| fired brick, dressed stone, cobble | 1.5 – 1.8 |
| unsealed concrete, plaster, dry soil, unglazed terracotta | 1.8 – 2.0 |
| cloth, canvas, raw wood | 1.7 – 2.0 |

**The first row is `k ≈ 1`, i.e. essentially no darkening, and that is the
physics, not a conservative default.** A non-porous substrate has no pores for
water to fill and no accessible subsurface for the recycled light to be absorbed
in (§2.1); the film sits on top of a sealed interface. What changes when you wet a
glazed tile is almost entirely **the coat** — the gloss, the grazing sheen, the
reflected world. An author who reports "my wet tile doesn't look darker" is
observing correct behaviour, and the verb's message should say so rather than
inviting them to crank `k`.

Default **1.55**. These are the practical renderer range from the Ångström
exponent form as it is used in production; they are **not** measured in RISE and
the document should not imply they are.

### 6.7 The water-absorption spectrum data file

Ship `colors/water_absorption.spectra` — a two-column `(nm value)` plain-text table
of water's spectral absorption over **380–700 nm** (Pope & Fry 1997's measured
range; extend to 780 nm only with a second named source such as Kou et al. 1993 or
Segelstein 1981, since Pope & Fry stops at 700), loaded by `scalar_painter`'s
`file` form into a `PiecewiseLinearScalarPainter`
([ChunkParserRegistry.cpp:1314-1338](../src/Library/Parsers/ChunkParserRegistry.cpp)),
resolved through `GlobalMediaPathLocator`.

```
scalar_painter { name water_tau  file colors/water_absorption.spectra }
# AMENDED (2026-08-31): the directory prefix is required -- GlobalMediaPathLocator
# resolves "colors/water_absorption.spectra" but not the bare filename (matches
# every existing .spectra reference in the tree; execution-verified).
dielectric_material { name deep_water  tau water_tau  ior 1.333  scattering 1000000 }
```

**Three things to get right.**

1. **`tau` is a transmittance base, not an absorption coefficient.** It is applied
   as `pow(tau, distance)`
   ([DielectricSPF.cpp:311-324](../src/Library/Materials/DielectricSPF.cpp)), so
   the shipped file must be pre-converted to `exp(−σ_a)` per wavelength at unit
   distance. Without that, an author pasting a published σ_a table gets the curve
   inverted — blue heavily absorbed, red passing freely.
2. **"Unit distance" bakes a world-scale assumption: 1 world unit = 1 metre.**
   `pow(tau, distance)` uses whatever the scene's units are, so a file converted
   from m⁻¹ values is only correct in a metre-scaled scene. A centimetre-scaled
   scene would absorb 100× too little per unit. Say this in the doc *and* in the
   data file's header, and give the rescaling rule (`tau_new = tau^(1/s)` for a
   scene where one world unit is `s` metres).
3. **Follow the convention that already exists — RISE ships plenty of measured
   optical data.** An earlier draft of this document claimed it ships none, on the
   strength of a `find . -iname "*.spd"` that returns nothing. That grep was
   literally true and substantively false, which is precisely the failure
   `docs/agentic-redesign/90-iteration-ratchet.md` §2 warns about, committed here
   against RISE's own tree. What actually ships under `colors/` is a body of
   two-column `(nm value)` tables: `cornellbox_{white,red,green,light}.spectra`,
   `flat_1.spectra`, `nikon_sb16_xenon_flash.spectra`, `kodak_DCS420_response`,
   `conductors/{Ag,Au}.{n,k}`, the `thinfilm/` oxide and substrate stacks, and
   `.ior` curves — consumed directly by scene chunks (e.g.
   `scenes/Tests/VCM/cornellbox_vcm_spectral.RISEscene:58-62` loads
   `colors/cornellbox_green.spectra` into a `spectral_painter`). So this file
   **follows an established convention rather than establishing one**: same
   directory, same two-column format, `.spectra` extension, source named in the
   header.

> **And the honest RGB idiom, because the default renderer is RGB.** Per §2.5,
> `PiecewiseLinearScalarPainter::GetValuesAt` broadcasts one 555 nm sample
> ([PiecewiseLinearScalarPainter.h:52,89-93](../src/Library/Painters/PiecewiseLinearScalarPainter.h)),
> so under `pathtracing_pel_rasterizer` the measured file yields **grey water with
> no depth tint at all**. The RGB idiom is three explicit per-channel numbers —
> exactly what the shipped scene already does:
> `tau 0.85 0.92 0.95` (`tidepools.RISEscene:395`), red attenuating fastest. The
> documentation must give **both** forms and say which rasterizer each is for;
> shipping only the spectral file would hand every RGB author a silently
> colourless pool.

So: spectrally correct depth tint is free *under a spectral rasterizer*, and a
three-number approximation otherwise.

### 6.8 The geometry-level pooling recipe

Documentation, not code. It goes in the proven read-set and the verb's design
note points at it.

**The recipe:** author the pool as its own object with `dielectric_material`
(`ior 1.33`, `scattering 1000000` for a delta-flat surface, `tau` from
§6.7), on one of:

- a squashed `ellipsoid_geometry` for a simple round pool — the
  `tidepools.RISEscene:451-463` idiom;
- a `displaced_geometry` over a flat `box_geometry` or `cartesian_disk_geometry`
  driven by a `gerstnerwave_painter` or an `IFunction2D`, for a rippled surface —
  the `scenes/FeatureBased/VCM/vcm_sdf_luminaire_jellyfish.RISEscene:496-519` idiom;
- a `csg_object` intersection of the terrain with a half-space, to carve a level
  cap that follows an irregular basin.

**Reach for this route whenever the terrain is one large mesh**, not only when
the water must be level. `occlusion(r)`'s radius is a fraction of the *hit
geometry's* bbox diagonal (§6.5), so on a single big ground mesh the
shading-level mask can only see basin-scale hollows — small puddles are below its
resolution no matter what radius is chosen. Small pools on large terrain are a
geometry problem, and this recipe is the answer.

> **Trap, flag it loudly: heightfield-mode `sdf_geometry` cannot drive the
> occlusion pooling mask.** `occlusion()` and `thickness()` return their
> **neutral fallback** in heightfield mode — the descriptor gives the reason
> ("whose global Lipschitz bound would make a local answer systematically wrong",
> [ChunkParserRegistry.cpp:1601](../src/Library/Parsers/ChunkParserRegistry.cpp)).
> So the natural instinct — build a terrain as an SDF heightfield, then pool
> water in its hollows with `occlusion()` — **silently produces a uniformly dry
> terrain**, because neutral occlusion is 1 (unoccluded) everywhere. The fallback
> is deliberately do-nothing, so there is no error and no warning; the mask just
> never lights up. Terrain that must drive occlusion-based puddles has to be a
> **mesh**, a **`displaced_geometry`** (which bakes to an indexed mesh), or a
> **part-based `sdf_geometry`** — never heightfield mode.

### 6.9 Phase-1 caveats, collected

1. **NEE sees a dry Lambertian.** `polished_material`'s `GetBSDF()` is a bare
   `LambertianBRDF(Rd)` with no coat lobe and no `(1−Rs)` factor
   ([PolishedMaterial.h:49,57](../src/Library/Materials/PolishedMaterial.h)),
   so direct lighting and BDPT/VCM connections disagree with sampled transport by
   up to ~1.6× at grazing. The most serious Phase-1 limitation (§3.3, §6.2).
2. **Darkening only works on flat-colour albedos.** The expression VM cannot
   sample a painter ([ExpressionEval.h:728-757](../src/Library/Painters/ExpressionEval.h)),
   so a textured substrate would get gloss and no darkening — turned into an
   honest refusal by §6.4 clause 2, and fixable by §4(g).
3. Darkening is RGB-per-channel, not spectral (§2.1, §6.3).
4. **A single per-channel `k` is a fit, not the physics** — the implied exponent
   varies with base albedo, and a fixed `k` over-boosts saturation on already
   saturated substrates (§2.1).
5. Intermediate coverage loses `Rd·Rs·(1−c)` from the substrate term — ≤ 2 % of
   `Rd` at normal incidence, up to ~50 % at grazing — **plus** an additional
   grazing-concentrated loss from the geometric-horizon coat-lobe drop, so the
   total is bounded below by that expression and is not known in closed form
   (§6.2).
6. Shading-level pools are not level, and only *pooled* water levels at all — a
   thin film conforms to relief (§2.2, §2.4, §6.5).
7. **`occlusion()`'s radius is relative to the hit geometry's own size**, so the
   pooled-joint look needs per-feature objects or tiled patches; small puddles on
   one large terrain mesh are out of reach and belong to §6.8's geometry route.
8. "Dry under an overhang" needs self-concavity or an authored mask (§6.5).
9. Heightfield SDF terrain cannot drive the pooling mask (§6.8).
10. Signal-driven masks read neutral on parts of BDPT/VCM/MLT transport (§9).
11. Oren-Nayar bases get darkening only, no film; metallic bases get film only, no
    darkening (§2.1, §6.2, §6.4).
12. **The GGX/PBR in-place branch is not a water film** — it sharpens the base's
    own tinted lobe, destroys anisotropy where the mask bites, and couples what
    §2.2 says water decouples (§6.2).
13. **Wetness and wear are mutually exclusive on one material** in v1; each verb
    refuses a material the other has already written (§6.4).
14. **The verb rewrites a material chunk, so every object bound to it becomes
    wet.** A material shared across ten objects wets all ten in one call — usually
    what an author wants for a paving material, occasionally not — and **each
    bound mesh pays its own `occlusion` bake** at first production shading. The
    verb's result message must report the bound-object count so the blast radius
    is visible before the render.
15. **Water depth tint is grey under an RGB rasterizer** unless authored as three
    explicit per-channel `tau` values (§2.5, §6.7).

---

## 7. Phase 2 — `coated_material`

Gated on Phase-1 evidence. Specified here to the architecture-decision level.

### 7.1 What it is, and the contract it must satisfy

A single new Material+BRDF+SPF triad per [MATERIALS.md](MATERIALS.md) §2 and §9:
`CoatedMaterial` + `CoatedBRDF : IBSDF` + `CoatedSPF : ISPF`, with **both RGB and
NM forms of every method** (§3 of that doc), `GetSpecularInfo`/`GetSpecularInfoNM`
overridden if the coat lobe can go delta, a closed-form `IBSDF::albedo` for the
OIDN AOV, and an `EvaluateKrayNM` for HWSS companion throughput —
`PolishedSPF::EvaluateKrayNM`
([PolishedSPF.cpp:335](../src/Library/Materials/PolishedSPF.cpp)) is the worked
precedent.

**The single most important requirement — and the thing neither
`composite_material` *nor* `polished_material` does — is its own closed-form
`value`/`valueNM` accounting for the combined coat-plus-substrate response**, so
that NEE and BDPT/VCM connections evaluate the *layered* BRDF. Note this is now a
**doubly-motivated** requirement: `composite_material` forwards one sub-material's
BSDF (§3.2 defect 2) and `polished_material` returns a bare `LambertianBRDF`
(§3.3), so **every** existing route to a coated surface in RISE mis-evaluates
direct lighting. This is Phase 2's third independent justification, alongside the
`coat_weight` energy fix (§6.2) and the glTF customer (§7.6).

**Second requirement — the spectral darkening, and how it is actually obtained.**
Phase 1's `pow` is an RGB fit applied to a painter (§2.1). Phase 2 does not need
to reproduce that operation at all: **the principled spectral wet-darkening *is*
the layered model's own per-wavelength transport.** Evaluate the substrate at the
hero wavelength (`GetColorNM(ri, nm)`), send the light in through the coat, let
the internal-reflection recycling of §7.4 act on `R(λ)` per wavelength, and the
Saunderson form of §2.1 falls out — darkening and chroma boost together, from
transport rather than from a fitted exponent. That is the differentiator, and it
is a property of getting §7.4 right rather than a separate feature.

> **Consequently `substrate_wet_exponent` is demoted, and possibly dropped.**
> Once the layered model performs the recycling, an *additional* `R^k` on the
> substrate **double-counts the same physics** — the exponent was a stand-in for
> the recycling, not an independent effect. If it is kept at all it must be
> labelled in the descriptor as an **art-direction override, default 1.0 (off),
> that double-counts when non-unity** — not as a physical parameter. It is also
> ill-defined in general: `base` is an arbitrary `IMaterial`, and only a
> painter-backed diffuse exposes a reflectance to exponentiate; `f_r^k` on a
> general BRDF is dimensionally wrong (an exponentiated density is not a density).
> **Recommendation: drop it from the shipping parameter set** and let the
> transport do the work; re-add only if art direction demonstrably asks.

### 7.2 Parameterization — OpenPBR-shaped

| parameter | pipe | notes |
|---|---|---|
| `base` | material ref | the substrate material — **restricted, see below** |
| `coat_weight` | `IScalarPainter` | **[0,1] coverage — the first-class spatially varying slot; this is the primitive that closes G2 for the wet case** |
| `coat_ior` | `IScalarPainter` | 1.33 water, 1.5 varnish/lacquer, 1.4–1.6 oil |
| `coat_roughness` | `IScalarPainter` | GGX α on the coat lobe; 0.01–0.05 for water |
| `coat_thickness` | `IScalarPainter` | world length, feeds Beer–Lambert |
| `coat_absorption` | `IScalarPainter` | **scalar pipe, deliberately** — §3.2's routing hazard |
| `coat_tint` | `IPainter` | genuinely a colour (tinted lacquer); colour pipe is correct here |

**`base` must be restricted, not "any `IMaterial`".** Two independent reasons.
*Correctness:* the layered model needs the substrate's directional albedo to run
its recycling series, which a diffuse or microfacet base can supply and an
arbitrary material (a luminaire, a BSSRDF, a volumetric random-walk) cannot.
*Budget:* §7.5's lobe arithmetic only closes because the substrate's lobe count is
known — an unrestricted `base` could exhaust the 6-slot container on its own.
**Recommended v1 allowlist: `lambertian`, `orennayar`, `ggx`,
`pbr_metallic_roughness`** — refuse anything else at parse time with a message
naming the allowlist, rather than rendering something quietly wrong.

Aligning the names with the OpenPBR surface specification is deliberate: it is the
industry's converging interchange model, and
[GUI_ROADMAP.md:82](GUI_ROADMAP.md) already records RISE's posture — adopt it as
the *conceptual model and import format*, not as native storage. Matching its
coat parameter names costs nothing and makes the eventual MaterialX/OpenPBR import
a rename rather than a redesign.

### 7.3 Why `coat_weight` subsumes the missing mix primitive

A surface with a coat over fraction `c` of its area, at sub-pixel scale, has
expected BRDF `c·f_coated + (1−c)·f_bare`. That is a statistical mixture of two
BSDF states — precisely what a `mix_material` would compute — but with two
advantages: the two states share a substrate (so there is no arbitrary pair of
BSDFs to reconcile), and the mixture weight has a physical name and a physical
range. OpenPBR's `coat_weight` carries exactly these semantics.

**Consequence, scoped carefully: G2 does not need a generic `mix_material` for
the *film-forming* effects — wetness, oil, lacquer, varnish.** Each of those is
genuinely "a coverage-weighted transparent layer over a substrate", which is what
`coat_weight` models.

**It does not extend to the siblings, and this document should not pretend it
does.** §2.6 says dust is param modulation with **no coat**, and §8.2 says
explicitly *do not attempt snow as a coat* — an opaque high-albedo scattering
layer is a different object from a transparent film, and forcing it through
`coat_weight` would be modelling error, not reuse. What dust and snow reuse from
this design is the **coverage-mask machinery** (§6.3), not the coat. §4b's
rejection therefore stands for wetness on this reasoning, and for dust and snow on
its reason 3 (no observed demand) instead.

### 7.4 The layering model — three candidates, one recommendation

- **Weidlich & Wilkie 2007, "Arbitrarily Layered Micro-Facet Surfaces."** Analytic
  single-bounce: Fresnel in at the coat, attenuate through the coat's thickness
  along the refracted path, scatter off the substrate, attenuate out, Fresnel out.
  Cheap, closed-form, evaluates in both `Scatter` and `value` with no LUT and no
  stochastic inner loop. Ignores multiple inter-reflections between coat and
  substrate, so it under-brightens a high-albedo substrate under a high-IOR coat.
- **Belcour 2018, "Efficient Rendering of Layered Materials using an Atomic
  Decomposition with Statistical Operators."** Tracks energy and angular variance
  through the stack with statistical operators; accurate including multiple
  inter-reflections; the reference-quality answer, and the one
  [PHYSICALLY_BASED_PIPELINE_PLAN.md:663-670](PHYSICALLY_BASED_PIPELINE_PLAN.md)
  names as Finding A's A2 option. Heavier: per-lobe variance propagation and a
  roughness-remapping step on every evaluation.
- **Heitz et al. 2017 LTC-Layered** — also named in Finding A. Its payoff is
  **closed-form integration against polygonal area lights**, which RISE's NEE
  already handles by sampling; a path tracer buys little from the LTC fit while
  paying its layered-fit complexity. Rejected on that basis, not on quality.

**Recommendation: Weidlich–Wilkie as the analytic core, plus a Kulla-Conty-style
multiple-scattering compensation term on the coat–substrate interreflection —
where the compensation term is REQUIRED, not an optional refinement.**

> **Do not reach for F₀ ≈ 0.02 to argue the interreflection is negligible — that
> is the wrong coefficient, and it inverts §2.1(ii).** F₀ ≈ 0.02 is the
> **external** reflectance at the air→water interface. The coat↔substrate
> interreflection is governed by the **internal** hemispherically averaged
> reflectance at the water→air boundary, `r_i ≈ 0.44–0.47` for n = 1.33 (the pure
> TIR fraction alone is `1 − 1/n² = 0.435`). That is not a small correction: it is
> **the dominant wet-darkening mechanism** this document opened with. A
> Weidlich–Wilkie single-bounce coat that omits the recycling term throws away
> roughly **45 %** of the light that should return to the substrate.
>
> **What that looks like on screen — and the sign is worth being exact about.**
> The correct response carries the recycling series, `C·R/(1 − r_i·R)`; WW's
> single bounce is `C·R`. Since `1/(1 − r_i·R) > 1` always, omitting it renders
> the surface **too dark**. And because that amplification is *largest in the
> high-albedo channels* (where `r_i·R` is closest to 1), dropping it suppresses
> exactly the channels that produce the wet chroma boost — so the result is also
> **under-saturated**. Concretely, at `r_i = 0.47` a substrate with channel
> reflectances `R = {0.8, 0.2}` should come out at a channel ratio of **5.81**;
> WW-single-bounce returns **4.0**, which is precisely the *dry* ratio. The tell
> is unmistakable once you know to look for it: **WW-without-recycling produces a
> uniformly dimmed dry surface, not a wet one.**
>
> **Sanity check for the implementer:** *"the coat looks like darkened-dry with no
> chroma boost ⇒ the recycling term is missing."*

The practical consequence is a gate condition: **WW-without-compensation cannot
pass §13's `kPosturePass` furnace requirement at high substrate albedo**, because
the missing recycling shows up directly as lost energy where `R` is large (the
`r_i·R` denominator of §2.1's Saunderson form is exactly the series being
dropped). The compensation term is therefore load-bearing and must land with the
core, not after it.

**Given that, the WW-vs-Belcour choice rests on implementation surface alone**,
which is a legitimate basis — though **not** for the reason an earlier draft of
this section gave. The compensation is **not** a Kulla-Conty LUT lookup:
`MicrofacetEnergyLUT::LookupEavg`/`LookupEss`
([GGXSPF.cpp:174-192](../src/Library/Materials/GGXSPF.cpp)) supply a *GGX lobe's*
multiple-scattering albedo, whereas the quantity needed here is the **dielectric
interface's internal hemispherical reflectance `r_i`**, which is closed-form
(§2.1's 0.44–0.47 for n = 1.33) and needs no table at all. The compensation is a
closed-form geometric-series sum, `1/(1 − r_i·R)` — cheaper than a LUT, not
more expensive. `MicrofacetEnergyLUT` stays relevant to this material for a
*separate* purpose: the coat lobe's **own** multiple-scattering term once
`coat_roughness` is non-trivial, exactly as it serves GGX today.

The real implementation-surface argument is simply that WW's closed form evaluates
in `value` and `Scatter` alike with no variance propagation and no
roughness-remapping step. **Belcour's operators
carry the recycling natively** — it is not something bolted on there — so Belcour
is the more principled object and the documented upgrade path; it is declined for
v1 on scope, not on correctness. Reach for it if the furnace configurations show
WW-plus-compensation failing at high albedo or high coat IOR, where the
single-bounce approximation is weakest. The harness to detect that already exists
(§7.6).

### 7.5 Fitting the sampling architecture

- **Lobe budget.** `ScatteredRayContainer` is a hard 6 slots
  ([ISPF.h:80](../src/Library/Interfaces/ISPF.h)). A GGX substrate uses 3; a coat
  adds **1** (its own specular lobe — the transmission through the coat is folded
  into the substrate lobes' throughput analytically, not emitted as a separate
  ray). **4 of 6. It fits**, with headroom for a future sheen lobe.
- **Selection.** One stochastic single-lobe pick with `kray / selectProb`
  correction, matching the convention every integrator uses since deterministic
  path-splitting was removed in 2026-05
  ([CLAUDE.md](../CLAUDE.md) "Path-tree branching ... excised"). The natural
  selection weight is `coat_weight · Fresnel(coat)` for the coat lobe and the
  complement, distributed across the substrate's own lobes by their existing
  weights.
- **`Pdf` must be the real mixture PDF** — `w_coat·pdf_coat + (1−w_coat)·pdf_base`
  with the *actual* weights, not `composite_material`'s 50/50 placeholder
  ([CompositeSPF.cpp:297-309](../src/Library/Materials/CompositeSPF.cpp)). This is
  the correctness line that separates the two, and
  `tests/SPFPdfConsistencyTest.cpp` is the guard.
- **Write the coverage mixture out explicitly, because the loose phrasing above
  collapses two different substrate throughputs.** At coverage `c` the response is
  a mixture of a *coated* state and a *bare* state, and the substrate is reached
  differently in each:

  ```
  f  =  c · [ F(θ)·f_coat  +  (1 − F(θ))·T_in · f_base · T_out · (1 + recycling) ]
     +  (1 − c) · f_base
  ```

  The `(1−c)` branch reaches `f_base` through **air**, with no Fresnel
  transmission and no recycling; the `c` branch reaches it through the coat, with
  both. Reading "coverage-weighted coat" as a single blend of one coat lobe
  against one substrate lobe silently identifies those two, which loses exactly
  the coverage semantics `coat_weight` exists to provide — and would make `c` a
  gloss knob rather than a coverage fraction. `T_in`/`T_out` are the coat's
  transmission factors and `recycling` is §7.4's required term.

### 7.6 Validation, and the second customer

**Validation is already built.**
[tests/LayeredWhiteFurnaceTest.cpp](../tests/LayeredWhiteFurnaceTest.cpp) drives
eight layered configurations at 100 000 samples per (config, angle) with a
per-config posture (`kPosturePass` / `kPostureBounded` / `kPostureKnownFailure`).
Phase 2 adds `coated_material` configurations mirroring the known-failing
composite ones (3 and 7) — same substrates, same coat parameters — and they must
land in `kPosturePass`. That is a direct, numeric, apples-to-apples improvement
claim against a shipped baseline, which is the strongest form of exit gate
available here. Alongside it: `SPFBSDFConsistencyTest` and
`SPFPdfConsistencyTest` (the `value`↔`Scatter` and `Pdf`↔`Scatter` agreement
guards, [MATERIALS.md:216-219](MATERIALS.md)).

**Second customer.** The glTF importer's `#if 0` block
([GLTFSceneImporter.cpp:1341-1469](../src/Library/Importers/GLTFSceneImporter.cpp))
implements `KHR_materials_clearcoat` and `KHR_materials_sheen` as
`CompositeMaterial` wraps and is disabled because that composition produces
near-black surfaces. `docs/GLTF_IMPORT.md:1091` states the requirement in RISE's
own words: "Phase 5 needs a proper additive layered material that respects
per-lobe albedo." A `coated_material` with a GGX coat lobe and a `coat_weight` is
that material for clearcoat directly, and the sheen case becomes the same shape
with a Charlie lobe. **Phase 2 should be scoped and reviewed with that customer
in the room**, because designing the coat lobe as "water only" and then
retrofitting clearcoat is how a parameterization gets bolted onto twice.

---

## 8. Phase 3+ — the siblings, observed-need gated

Per the repo's convention these are enumerated so the coverage machinery is
designed once, and **may be declined** if no observed demand appears.

### 8.1 Dust

`add_dust`, or an argument on `add_wetness`. Reuses almost all of the mask
prelude — dust's mask is `clamp(up_facing · shelter, 0, 1)`, and §6.3 already
declares `up_facing` as a named `def` (the gravity gate on pooling). `shelter`
is *not* yet a named def: the `1 − occlusion(r)` term is inlined into `cavity`,
so the dust variant needs **one added line** hoisting it out —
`def shelter 1.0 - occlusion(0.08)`, with `cavity` becoming `shelter * gravity`.
One line, not zero. `dot`, `vec3`, and `N` are all available
([ExpressionEval.h:739,685,1138](../src/Library/Painters/ExpressionEval.h)). The
two effects share machinery because they share a cause: rain rinses exposed
surfaces and dust settles in the same sheltered, upward places water would sit but
does not get washed from. What differs is the ridge term's sign and the
appearance edit, not the geometry.

Appearance edit is **pure param modulation, no coat** (§2.6): `mix` reflectance
toward a desaturated grey, raise roughness, drop any specular weight. **Dust is
the cheapest of the three and needs nothing from Phase 2.** If any sibling ships,
this is the one.

### 8.2 Snow

Split by depth, honestly. **Frost / first dusting** is shading-level: the dust
machinery with a high-albedo near-white target and raised roughness — cheap, and
convincing only at the "barely there" end. **Real snow** is geometry-level: it has
thickness, fills concavities, rounds edges, and its whiteness is a
multiple-scattering volume effect (§2.7). The honest RISE answer is a
displaced/CSG accumulation layer with a `random_walk_sss_material` or a dense
`homogeneous_medium`, plus a sparkle term — and `ENAMEL_SPARKLE_BRDF.md:155`
already names snow as a target for that BRDF's per-cell independent facet
normals. **Defer. Do not attempt snow as a coat.**

### 8.3 Oil slick / oiled metal

The genuine thin-film interference case (§2.2), and the one place the GGX
`film_ior` / `film_extinction` / `film_thickness` Airy stack is the *right* tool.
A documented recipe plus one showcase scene, not new code — `ggx_material` in
`fresnel_mode thinfilm` with a mask-driven `film_thickness` in the 200–600 nm
range over metal or over water. Zero new classes. Worth doing as a
documentation-plus-scene addition if the Phase-1 census shows authors reaching for
"oily" / "iridescent" language.

### 8.4 Generic `mix_material`

Revisit only if a *non-coat* demand appears — two genuinely unrelated materials
blended by a mask, which coverage-weighted coating does not express. §4b's
rejection is scoped to the coat case and does not generalize forever.

### 8.5 Automatic pooling geometry

Generating a level water surface in a mesh's concavities automatically. **Do not
build this speculatively.** Gate it on the Phase-1 census actually showing that
agents fail to author the §6.8 recipe by hand. The `collapse_to_instances`
precedent is instructive in the other direction: a verb was built because a note
demonstrably failed 5×. Wait for the equivalent evidence.

### 8.6 Dew and beading — a different phenomenon wearing the same word

**This document models *film* wetness throughout, and dew is not a film.** Dew,
rain beading on a waxed surface, and droplets on glass are **discrete lenses**:
each drop refracts, magnifies the substrate behind it, throws a bright caustic
spot, and carries its own sharp specular highlight. None of that is a BRDF
property of the surface underneath, and **no value of `coat_weight` produces it** —
a coverage fraction says how *much* of the surface is filmed, never that the
covered part is a collection of lenses.

**The mask has the wrong sign as well, which is the sharper warning.** Dew
condenses on **convex, up-facing, radiatively-cooled** surfaces — the crown of a
leaf, the top of a railing. §6.3's `ridge` term drives coverage *to zero* exactly
there, and its `cavity` term drives it *up* in the sheltered places dew avoids. So
applying the wetness recipe to a dewy leaf does not merely approximate it; it
inverts it. An author who reaches for `add_wetness` on foliage will get a
plausible-looking result that is wrong in the one way a viewer notices.

**If it is ever built** (observed-need gated, like the rest of §8), it is a
separate mask — `clamp(curv·k, 0, 1) · up_facing`, the *inverse* of the pooling
mask — feeding **geometry**: instanced spheres or SDF parts scattered on the
surface, or at minimum a normal map with droplet-shaped lobes. It is a scattering
/ instancing feature, not a coat parameter, and it should not be smuggled in as
one. Recorded as a Non-goal in §14 so the boundary is explicit.

---

## 9. Integrator and renderer implications

**PT is the default and the recipe must be PT-first.** The Phase-3 integrator
decision is unambiguous: PT is the runtime default because it wins
wall-clock-normalized variance on the converged bulk, with BDPT routed in for
strong-indirect/glossy and VCM only for caustics
([UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md) §2, §4). The
`auto_rasterizer` dispatcher applies that map automatically. Wetness must look
right under PT first, and its documentation should not steer authors off PT.

**The signal-neutrality debt is inherited whole, and its consequence is
visible.** `curv`, `occlusion()`, and `thickness()` evaluate to their **neutral
fallback** in parts of BDPT/VCM/MLT transport, because those integrators
hand-build `RayIntersectionGeometric` records that omit `derivatives` and
`signals` ([GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md)
§14 item 11; the renderer already emits a one-time warning, per the descriptor at
[ChunkParserRegistry.cpp:1601](../src/Library/Parsers/ChunkParserRegistry.cpp)).

**Say the consequence plainly: a signal-driven wet surface will not match between
a PT render and a BDPT or VCM render of the same scene.** Neutral occlusion is 1
(unoccluded), so on those transport paths the pooling term collapses and the
surface reads *drier*, in patches, mixed with correct values from the primary
path. And because the `auto_rasterizer` dispatcher can route a scene to BDPT or
VCM on its own, an author can hit this without ever selecting a non-PT
integrator. The verb's design note and the skill text must say so. This is not a
new debt wetness creates; it is an existing one wetness makes more visible,
because a wet/dry boundary is a higher-contrast artifact than a grime gradient.

**Mirror-flat pools are caustic-adjacent, but introduce no new chains.** A
near-delta coat lobe over a diffuse substrate creates specular-diffuse paths, not
specular-diffuse-specular ones; the shading-level recipe adds **no new dielectric
chains** beyond what the scene already has. **SMS is therefore irrelevant to
Phase 1** — there is no new specular manifold to solve. Geometry-level pools *are*
real dielectric surfaces and do add chains; for a scene whose hero effect is
light focused through a pool, the integrator map says VCM
([UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md) §2: VCM is the
only integrator that reaches caustic transport, where PT/BDPT miss 44–78 % of the
energy) — at the cost of its env/volume bias and its σ²·T penalty everywhere else.

**Validate wet-highlight scenes with `oidn_denoise FALSE`.** The measured OIDN
recalibration ([PT_ENV_MIS_DOUBLECOUNT.md](PT_ENV_MIS_DOUBLECOUNT.md) §4a) found
that OIDN *raises* peakiness on near-deterministic RGB rows — CNN ringing
manufacturing a maximum the transport never produced (topology D PT: 1.010 true
vs 1.074 denoised) — and *suppresses* it on noisy spectral rows (2.021 true vs
1.386 denoised), with p99 tail statistics moving up to +47 pp. A sharp wet
highlight on a low-variance dry base is exactly the near-deterministic
high-frequency content the first behaviour was measured on. **Any numeric claim
about wet-highlight intensity must be made against a `oidn_denoise FALSE`
render**, per the `EnvLightBalanceTest` lesson — that suite spent an entire arc
measuring OIDN's output rather than the transport.

**The material-look studio rig is already well-suited to this.** `finish_element`
renders each closed element under the `StudioRigRestoreGuard` canonical rig
([AgentSession.cpp:16917-17085](../src/Library/Agent/AgentSession.cpp)): a neutral
**checker environment dome** plus three fixed directional lights (key/fill/rim),
replacing the scene's own lighting. The checker is what makes a coat legible — the
sharpness or smear of the reflected check reads coat roughness directly, which is
the single hardest wetness parameter to judge from a beauty render. The rig's own
failure-diagnostic language already flags "a thing meant to be glass or membrane
or liquid that reads opaque"
([AgentSession.h:4514-4517](../src/Library/Agent/AgentSession.h)) — the vocabulary
a wetness feature wants, already present.

**Energy conservation is checkable today.** §7.6's furnace harness for Phase 2;
for Phase 1, a `polished_material` wetness composition can be furnace-tested by
the same file with a new configuration, which would additionally put a number on
§6.2's `Rd·Rs·(1−c)` coverage dip rather than leaving it as an analytic bound.

---

## 10. Adoption and measurement

### 10.1 The laws this plan is built on

From `docs/agentic-redesign/88-procedural-texture-expressiveness-candidates.md`
§2, and cited here because they are the reason §5's ordering is what it is:

- **C-ADV — advice ≈ 0.** Design notes and Info diagnostics naming
  `scalar_painter` fired up to 30×/session, were demonstrably read, and drove
  **0/24 lifetime adoptions** across two models and multiple framings. Voluntary
  opt-in tools: **0/64**.
- **C-TYPE — the failure is a slot-typing prior, not ignorance.** Worked examples
  lifted painter *diversity* (floor 1→3, because models copy examples); the same
  lever moved spatially-varying roughness **0/24**.
- **C-VERB — when advice fails, ship a verb.** A zero-required-argument mutating
  verb that does the rewrite itself and refuses unless it can prove non-no-op
  semantics.
- **C-READ — the read-set is the knowledge boundary.** Agents pull 2–5 skills by
  hook line; `procedural-textures.md` was read **0/6** in one probe batch.
- **C-MEAS — census, not vibes.** N=3 minimum, cross-provider before believing a
  null, pre-committed stop rules.

**The replication that makes C-VERB more than a hypothesis:** the `add_wear`
verb-effect census (2026-08-30,
`evals/runconfigs/addwear_census_gemini_gpt.json`, results in
`evals/runs/addwear_census/`) against its own pre-verb baseline (2026-08-29,
`evals/runconfigs/curv_census_gemini_gpt.json`), same instrument, same providers,
N=3 each, **the verb as the only variable**: `add_wear` called in 4/6 runs;
curv-driven wear **1/6 → 3/6**; occlusion **0/6 → 3/6**; position-proxy fakes
**3 → 0/6**; gpt compliance **0/3 → 1/3** and report-level pass@1 **0 % → 33.3 %**,
the first-ever strict-gate pass on that instrument by any provider.

### 10.2 The master triad, applied

*"Summoned category, price the inferior path, exactly one worked example that
parses, and let the census say whether it moved"*
([GEOMETRY_SHADING_SIGNALS_DESIGN.md:977](GEOMETRY_SHADING_SIGNALS_DESIGN.md);
origin `docs/agentic-redesign/CREATIVITY_JOURNAL.md:431-433`).

| leg | wetness instantiation |
|---|---|
| **Summoned category** | `add_wetness` — a named, zero-required-argument, refuse-on-no-op verb, registered as MUTATING on every surface, paired with `DESIGN_DRY_RAIN_SCENE` naming the verb. |
| **Price the inferior path** | The inferior path here is *a flat dark colour standing in for wet*. Doc 88's toll decision was that RISE tried "verb + note" first and never needed to price the naive path (`88-…md:731-733`). **Follow that precedent: do not price anything in Phase 1.** Reconsider only if the census shows the verb firing but flat-dark-colour wetness persisting alongside it. |
| **Exactly one worked example that parses** | `scenes/FeatureBased/Materials/rainwet_cobbles.RISEscene` (§6.5), execution-validated: parses, derives, renders, lands in its luma band. **One**, in the proven read-set (`materials-and-media-basics.md`, `object-modeling-recipes.md`) — not three, and not in `procedural-textures.md`, which is measured unread. |
| **Census** | §10.3. |

### 10.3 The measurement plan

**Pre/post census pair, the `add_wear` protocol exactly.** Same instrument, same
providers, N=3 each, the verb as the only variable between runs.

- **Instrument:** a new `evals/scenarios/rainwet_closeup.json`, built on
  `rich_material_closeup`'s shape ("macro product close-up") with the subject
  changed to a **rain-wet cobblestone close-up**. Gating checkpoints, adapted
  from that scenario's: `any_param_references_kind:scalar_painter` (weight 3);
  a chunk-kind checkpoint that must be a **disjunction, not a fixed list** —
  `expression_painter` AND any one of (a) `polished_material`, (b) a mask-driven
  **`alphax`/`alphay`** on `ggx_material`, or (c) a mask-driven **`roughness`** on
  `pbr_metallic_roughness_material` — weight 2. **The per-kind split in (b) vs (c)
  is not pedantry:** `pbr_metallic_roughness_material` has no `alphax`/`alphay` at
  all (its microsurface slot is `roughness`; anisotropy is `anisotropy_factor` —
  [ChunkParserRegistry.cpp:4163](../src/Library/Parsers/ChunkParserRegistry.cpp)),
  so a checkpoint written against `alphax` would score every correct PBR-base run
  as a failure. Likewise a checkpoint **requiring `polished_material` outright**
  would fail the §6.2 in-place branch, which is this design's own prescribed
  answer for a GGX/PBR base — the instrument would then be measuring which base
  material the agent happened to start from; a render band on
  `meanLuma` — **calibrated on the actual scene, not copied**, since wet surfaces
  are legitimately darker than the dry brass the existing band was set for.
- **Pre-verb run:** `evals/runconfigs/wetness_census_gemini_gpt.json` against
  today's tree, before any Phase-1 commit. **This must happen first**; a baseline
  measured after shipping is not a baseline.
- **Post-verb run:** identical config, after Phase 1.
- **Metrics, pre-committed:** `add_wetness` call rate; presence of a
  darkening-plus-gloss pair (not gloss alone, not darkening alone); occlusion- or
  curv-driven coverage vs. a flat constant; **position-proxy occurrences** (a
  `P.y` threshold standing in for "wet at the bottom" — the wetness analogue of
  the `P.z` wear stripe, and the specific failure the verb exists to prevent);
  per-provider compliance; report-level pass@1; **how often a run wants both
  `add_wetness` and `add_wear` on the same material** — the count that decides
  whether §6.4's mutual-exclusion follow-up (prelude extension in place) is worth
  building, since "weathered and rain-wet" is the obvious hero subject and the two
  verbs currently lock each other out; and **the textured-albedo refusal rate** —
  how often the verb is invoked on a material whose albedo is a painter rather
  than a constant and therefore refuses under §6.4 clause 2. **That last counter
  is the decision criterion for option §4(g)** and has to be collected
  deliberately: because clause 2 makes the textured case a hard refusal, a
  wrong-looking *render* can never appear, so the only observable signal that
  authors want wetness on textured substrates is the refusal count itself.
- **Stop rule, pre-committed:** if the post-verb call rate is 0/6 across both
  providers, the escalation is **not** more documentation (C-ADV) — it is to
  examine whether the paired design note fires at all, on the `add_wear`
  causality model (note at trajectory line 3 → call at line 97).

**Altar-stress element.** Insert one wetness element into an
`altar_stress`-shaped instrument (`evals/scenarios/altar_stress.json`, the
five-element "museum-vitrine altar" where each element quietly demands a shipped
capability). The natural element: *a votive bowl with a shallow standing pool*,
which demands the coverage mask, the cavity term, and — if the author reaches for
it — the geometry-level pooling recipe. Graded against a per-element scoreboard
narrative in the runconfig's `"//"` field, re-run after each hardening commit,
exactly as the signals work did p1→p4.

**Cross-provider before believing a null** (C-MEAS). A single-provider zero is
not a finding.

---

## 11. Cost

### 11.1 Phase 1 — structural counts, not measurements

| item | cost | basis |
|---|---|---|
| New material classes | **0** | Composes `polished_material` + existing painters |
| Five-build-project edits | **0** | No new `.cpp`/`.h` under `src/Library/`; verb code lands entirely in existing `src/Library/Agent/` files. **Phase 1 needs no chunk-parser change at all** — it composes chunks that already parse ([CLAUDE.md](../CLAUDE.md) "Source-file add/remove"; confirmed by commit `33aa5c6f`, which touched exactly the five files *because* it added `MeshSignalBake.cpp/.h`) |
| Verb plumbing surfaces | **8** | `AgentSession.h/.cpp` (result struct + impl + predicate); `AgentMcpAdapter.cpp` (schema + mutating-verb list); `AgentChatCodecs.cpp` (the hand-duplicated `kToolDefs` parallel description); `AgentChatLoop.cpp` (transcript + mechanical-loop check); `AgentRpc.cpp` (dispatch + exclusion from `IsProposeSafeVerb`); `AgentLoopbackHttpServer.cpp`; `AgentDiagnostic.h` (paired condition); `AgentEvalRunner.cpp` (census recognition) |
| New test file | 1 | `tests/AgentAddWetnessTest.cpp`, on `tests/AgentAddWearTest.cpp`'s pattern (~248 `Check(` sites) |
| Existing test edited | 1 | a `polished_material` wetness configuration added to [tests/LayeredWhiteFurnaceTest.cpp](../tests/LayeredWhiteFurnaceTest.cpp) — required by §13's exit gate to put a number on §6.2's coverage dip |
| Refusal-text edit to `add_wear` | 1 | §6.4's mutual-exclusion message, which must name `add_wetness` — duplicated across `AgentMcpAdapter.cpp` and `AgentChatCodecs.cpp` |
| Content | 3 | one scene, one data file, two skill-text edits in the proven read-set |
| Eval configs + committed results | 2 + 2 | pre/post census runconfig + scenario, plus both runs' outputs committed under `evals/runs/` (the `add_wear` census protocol commits results, not just configs) |

**Per-hit render cost: zero new mechanism.** The masks are expression-VM programs
on painter slots that were already evaluated. `occlusion()` on a mesh pays the
lazy per-vertex bake — measured at **78 ms for a 12k-vertex mesh (0.87× a 16-spp
render of the same scene)** and **1441 ms for 194k vertices (5.1×)**
([GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md) §12) —
but that is the shipped signals cost, incurred once per geometry, and wetness
neither adds to it nor avoids it.

**The one real new cost is variance, not time.** Driving `scattering` toward
200 000 makes the coat lobe near-delta in the pooled regions; a near-delta lobe
over a diffuse substrate is a higher-variance configuration than the matte
surface it replaced. Unmeasured. It should be measured on the worked example,
with `oidn_denoise FALSE` (§9), before the recipe's default `scattering` ceiling
is fixed.

### 11.2 Phase 2 — structural counts

| item | cost |
|---|---|
| New source files | 3 (`CoatedMaterial.h`, `CoatedSPF.h/.cpp`, `CoatedBRDF.h/.cpp` — count depends on header-only choices) |
| **Five-build-project tax** | **Yes** — `Filelist`, `rise_sources.cmake`, `Library.vcxproj`, `.vcxproj.filters`, `project.pbxproj` (4 sections × 2 targets) |
| API/job/parser surface | `RISE_API_CreateCoatedMaterial`, `IJob`/`Job::AddCoatedMaterial`, one nested chunk-parser struct in `ChunkParserRegistry.cpp` ([MATERIALS.md:226-234](MATERIALS.md)) |
| Test surface | new configurations in `LayeredWhiteFurnaceTest.cpp`; `SPFBSDFConsistencyTest` and `SPFPdfConsistencyTest` coverage |
| Verb rework | `add_wetness` re-targets its emission from `polished_material` to `coated_material`; the mask prelude is unchanged |
| glTF unblock | delete the `#if 0` at [GLTFSceneImporter.cpp:1341](../src/Library/Importers/GLTFSceneImporter.cpp), re-point at the new material, re-enable the layered import path |

All of the above are **structural counts, not measurements.** No implementation
timing exists because no implementation exists.

---

## 12. Correctness debts and open items

1. **BDPT/VCM/MLT read the geometry signals as neutral** in parts of their
   transport — inherited, not created, by this design. Wetness makes it more
   visible than grime did (§9). Fix is the `PathVertexEval.h:94-106` widening
   contract plus the `LightSampler` NEE/photon-emission records
   ([GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md) §14
   item 11). **Deferred; unchanged by this document.**
2. **`CompositeSPF`'s 96 % energy loss in the coat-over-diffuse regime**
   (Findings A and D). This design **routes around** it rather than fixing it.
   The fix belongs to Landing 6's owner; if it lands, `composite_material` becomes
   usable for coat-over-diffuse but still carries the 50/50 `Pdf` and top-wins
   `GetBSDF` architectural defects (§3.2). **Open.**
3. **`composite_material`'s `extinction` is `IPainter`**
   ([CompositeSPF.h:41](../src/Library/Materials/CompositeSPF.h)) — a
   wrong-pipe slot of the exact class
   [ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md) eliminated elsewhere.
   Binding a physical absorption curve there JH-uplifts it. **Open, out of scope,
   flagged so nobody does it.** Retyping it is an ABI-visible change to
   `RISE_API_CreateCompositeMaterial`.
4. **`tidepools.RISEscene:400-404`'s comment is stale** — it claims `tau` cannot
   vary spatially, which the P2.1 `painter` bridge and the `expression` form have
   since made false (§3.6). Fix it in passing during Phase 1; it is currently
   actively misleading an author reading the scene as an example.
5. **The Phase-1 coverage energy dip** — `Rd·Rs·(1−c)` from the substrate term
   (≤ 2 % of `Rd` at normal incidence, up to ~50 % at grazing), **plus** an
   additional grazing-concentrated loss from the geometric-horizon coat-lobe drop
   ([PolishedSPF.cpp:206-207](../src/Library/Materials/PolishedSPF.cpp)) (§6.2).
   Analytic lower bound only, unmeasured. Measured by the Phase-1 furnace
   configuration, and **closed by Phase 2's `coat_weight`.**
6. **Spectral wet-darkening is unreachable in Phase 1** (§2.1) because the
   expression VM has no `nm` and `expression_painter` uplifts a computed RGB
   ([ExpressionPainter.h:175-181](../src/Library/Painters/ExpressionPainter.h)).
   **Closed by Phase 2's layered transport** — the per-wavelength internal
   recycling *is* the principled darkening (§7.1), not a separate exponent
   parameter. §4(g)'s painter-`pow` op would close it earlier and more narrowly.
6a. **`polished_material::GetBSDF()` returns a bare `LambertianBRDF(Rd)`**
   ([PolishedMaterial.h:49,57](../src/Library/Materials/PolishedMaterial.h)) —
   no coat lobe, no `(1−Rs)`. A **pre-existing** defect, but Phase 1 relies on
   this material, so direct lighting and BDPT/VCM connections see a dry substrate
   while sampled transport sees the wet split (§3.3, §6.9 item 1). **Closed by
   Phase 2's combined `value`/`valueNM`** (§7.1); not otherwise fixable without
   changing `polished_material`'s own semantics, which is out of scope here.
6b. **Phase 1 cannot darken a textured substrate at all** — the expression VM has
   no painter-sampling builtin
   ([ExpressionEval.h:728-757](../src/Library/Painters/ExpressionEval.h)), so
   §6.4 clause 2 refuses rather than half-delivering. **Open**; §4(g) is the
   scoped candidate fix.
6c. **`add_wetness` and `add_wear` mutually exclude each other on one material**
   (§6.4), and worn-and-wet is the flagship subject. v1 accepts the exclusion with
   cross-naming refusal messages; the census counts the demand. **Open.**
7. **The wet-highlight variance cost is unmeasured** (§11.1). Measure with
   `oidn_denoise FALSE` before fixing the recipe's `scattering` ceiling.
8. **Heightfield-mode SDF returns neutral occlusion silently** — the pooling
   recipe's sharpest trap (§6.8). Not a bug (the fallback is deliberately
   do-nothing), but it produces a wrong-looking render with no diagnostic.
   *Possible small mitigation, out of scope here:* the verb could warn when the
   target's geometry family cannot answer `occlusion()`.
9. **The water-absorption file must be pre-converted to a transmittance base**,
   because `dielectric_material`'s `tau` is `pow(tau, distance)` and not
   `exp(−σ·distance)` ([DielectricSPF.cpp:311-324](../src/Library/Materials/DielectricSPF.cpp)).
   A published σ_a table pasted in directly will be wrong, and "unit distance"
   additionally bakes **1 world unit = 1 metre** into the file. §6.7.
10. **A measured `tau` curve is grey under an RGB rasterizer.**
   `PiecewiseLinearScalarPainter::GetValuesAt` broadcasts one 555 nm sample and
   reports `HasPerChannelVariation() = false`
   ([PiecewiseLinearScalarPainter.h:52,89-93,105](../src/Library/Painters/PiecewiseLinearScalarPainter.h)),
   so the depth tint vanishes under the default `pathtracing_pel_rasterizer`.
   Mitigated by documenting the three-number RGB idiom alongside the file (§6.7);
   a real fix would be an RGB-aware evaluation of piecewise-linear scalar curves
   (integrate against the CMFs rather than point-sample), which is **open and out
   of scope**.
11. **A single per-channel darkening exponent is a fit whose implied `k` varies
   with base albedo** (§2.1), so it over-boosts saturation on already-saturated
   substrates. Inherent to the exponent form; **closed by Phase 2's transport**,
   which never forms an exponent.

---

## 13. Phasing

Each phase ships something usable on its own and has an explicit exit gate.
**Warning-free clean rebuild on both `make` and the Xcode `RISE-GUI` target is a
precondition for every phase** — incremental builds hide warnings.

### Phase 1 — the `add_wetness` verb and its recipe

1. **Measure the baseline first.** Author `evals/scenarios/rainwet_closeup.json`
   and `evals/runconfigs/wetness_census_gemini_gpt.json`; run the pre-verb census
   (N=3 × 2 providers) **against the current tree, before any Phase-1 commit**;
   commit results to `evals/runs/`.

   **AMENDED (2026-08-31) — baseline ordering, adapted for key availability.**
   The implementation session had no hosted-provider keys in its environment
   (`GEMINI_API_KEY`/`OPENAI_API_KEY` unset; the runner takes keys only by
   env-var indirection), so the census runs became user-run steps. The ordering
   guarantee is preserved differently: the eval instrument is the **first**
   Phase-1 commit, and the pre-verb baseline census is run **at that commit**
   (or any checkout of it) — the verb lands only in later commits, so a baseline
   run at the instrument commit is still strictly pre-verb, and the run manifest
   records the sha either way. Known pre-existing harness state, not introduced
   here: `AgentEvalCheckTest` T10 sweeps every `evals/scenarios/*.json` and
   already FAILS on the fixtureless `altar_stress.json` (verified live
   2026-08-31); `rainwet_closeup.json` carries the same fixtureless gap until a
   fixture is captured from the baseline run's own trajectory, the same way
   `rich_material_closeup`'s fixture was captured after its first live runs.
2. The mask prelude (§6.3), validated as a hand-authored scene first — literal
   `occlusion` radius, `up_facing` gravity gate, `damp` multiplied by
   `(1 − dryness)`, `wet` clamped to `damp`.
3. `scenes/FeatureBased/Materials/rainwet_cobbles.RISEscene` (§6.5) —
   execution-validated: parses, derives, renders, lands in its luma band.
4. `colors/water_absorption.spectra` (§6.7), pre-converted to a transmittance base,
   with a header comment naming Pope & Fry 1997, stating the conversion, and
   stating the 1-world-unit-is-1-metre assumption — **plus the three-number RGB
   idiom documented beside it**, since the file is inert under the default RGB
   rasterizer (debt 10).
5. `AgentSession` result struct, `AddWetness` implementation, and the qualifying
   predicate + selector, shared with the paired condition (§6.4). CST edits,
   splice-at-item-index, one head-version bump, one undo step; every refusal
   byte-identical.
6. Registration on all eight surfaces (§11.1), including exclusion from
   `IsProposeSafeVerb` — this verb commits one composite whole-document swap, the
   same reason `add_wear` and `vary_material` are excluded. Includes the
   **reciprocal refusal text in `add_wear`** naming `add_wetness` (§6.4), in both
   the MCP adapter and the chat codec.
7. `DESIGN_DRY_RAIN_SCENE` condition, **naming the verb**.
8. `tests/AgentAddWetnessTest.cpp` — every refusal clause (including the metallic
   gate, the textured-albedo refusal, and the `add_wear` cross-exclusion),
   name-collision minting, byte-identity on refusal, and **the `wet ⊆ damp`
   invariant checked two ways**: (a) *textually*, that the emitted `param`/`def`
   prelude is byte-identical across all three consumer chunks (the §6.1
   mechanism — a divergence here is the actual failure mode, and a numeric check
   alone would not catch it); and (b) *numerically*, that `wet ≤ damp ≤ 1` and
   both are ≥ 0 over a sweep of every `param` at its declared `min`/`max`, which
   is what pins the `film_amount` bound and the outer clamp.
9. Skill text in the **proven read-set only** (`materials-and-media-basics.md`,
   `object-modeling-recipes.md`): the recipe, the `k` table, the geometry-level
   pooling recipe with the heightfield trap called out, and the BDPT/VCM
   neutrality caveat.
10. Fix the stale `tidepools.RISEscene:400-404` comment (debt 4).

**Exit gate:** clean warning-free build on both toolchains;
`tests/AgentAddWetnessTest.cpp` green with every refusal clause covered and the
byte-identity assertions red-proved by mutation; `TextureExpressionVMTest` and the
agent suite green; the worked example parses, derives, renders, and lands in its
luma band under PT with `oidn_denoise FALSE`; a furnace configuration putting a
number on §6.2's coverage dip; and **the post-verb cross-provider census, N=3 × 2
providers, tabulated against the step-1 baseline in this document as a dated
`**CENSUS RUN (date)**` block** — never rewritten in place. **The census must
report the textured-albedo refusal rate**, which is the go/no-go signal for
option §4(g); without that counter the adopted option has no way to be decided
(§10.3).

### Phase 2 — `coated_material`

Gated on Phase 1's census. **Proceed if** the verb fires and authors are observed
reaching past what Phase 1 can express — spatially varying coat *roughness*
independent of coverage, a tinted or absorbing coat, **wetness over a GGX/PBR
base** (§6.2's in-place branch is a stopgap whose limits are structural, so demand
here is by itself sufficient), or grazing behaviour that the `tau`-as-coverage dip
visibly breaks. **The three correctness debts closed by Phase 2 — 5, 6 and 6a —
are standing justification independent of the census**, since 6a means every
existing route to a coated surface in RISE mis-evaluates direct lighting. **Reconsider
scope if** the verb converts and nobody asks for more, in which case Phase 2's
justification narrows to the glTF customer and the correctness debts (which is
still a real justification, just a different one, owned by a different roadmap).

1. `CoatedMaterial` + `CoatedBRDF` + `CoatedSPF` triad, **both RGB and NM forms
   of every method**; own closed-form `value`/`valueNM` (§7.1); `EvaluateKrayNM`;
   `albedo` for the OIDN AOV; `GetSpecularInfo`/`GetSpecularInfoNM` if the coat
   can go delta.
2. Weidlich–Wilkie analytic core + Kulla-Conty-style interreflection compensation
   (§7.4), reusing `MicrofacetEnergyLUT`.
3. The §7.2 parameter surface, **every physical scalar on `IScalarPainter`**;
   `coat_weight` as the spatially varying coverage slot.
4. **Per-wavelength wet-darkening as a property of the transport**, not as an
   exponent parameter (§7.1): `substrate_wet_exponent` is dropped from the
   shipping set, or shipped only as an explicitly-labelled art-direction override
   that double-counts when non-unity.
4a. The `base` allowlist (§7.2) — refuse a non-diffuse/non-microfacet substrate at
   parse time rather than rendering it wrong.
5. Real mixture `Pdf`/`PdfNM` — not a 50/50 placeholder (§7.5).
6. Five-build-project edits; `RISE_API_CreateCoatedMaterial`;
   `IJob`/`Job::AddCoatedMaterial`; chunk parser as a nested struct in
   `ChunkParserRegistry.cpp` with full descriptor text.
7. New `LayeredWhiteFurnaceTest.cpp` configurations mirroring the known-failing
   composite configs 3 and 7.
8. Re-target `add_wetness`'s emission to `coated_material`; mask prelude
   unchanged.
9. Re-enable the glTF layered path
   ([GLTFSceneImporter.cpp:1341-1469](../src/Library/Importers/GLTFSceneImporter.cpp))
   against the new material.

**Exit gate:** clean warning-free build on both toolchains; the new furnace
configurations in **`kPosturePass`** where their composite counterparts are
`kPostureKnownFailure` — the direct numeric claim against a shipped baseline —
including **at least one high-substrate-albedo configuration**, which is the case
that fails if §7.4's recycling compensation is omitted or under-weighted;
`SPFBSDFConsistencyTest` and `SPFPdfConsistencyTest` green for the new triad;
`hwss=true ≡ hwss=false` on a wetness scene (the reference-free bundle invariant
that proved the Bug-3 fix); the glTF clearcoat regression scene renders
non-black; every existing `polished_material` and `composite_material` scene
renders unchanged; and a zero-P1 review round on the current state per
[implementation-review-loop](skills/implementation-review-loop.md).

### Phase 3 — siblings, observed-need gated (may be declined)

No checklist is offered, deliberately. Each item in §8 is independently gated on
observed demand from the Phase-1 census and from real authoring sessions. If dust
converts, it is the cheapest of the three and reuses everything. If nobody asks,
**declining is the correct outcome and should be recorded as such**, in the same
way the signals design's Phase 4 items and the hair/fur LOD work were declined on
the observed-need rule.

---

## 14. Non-goals

- **Not a fluid simulation.** No flow, no puddle propagation, no drip. Water
  finds level via authored geometry or an occlusion mask, never via a solver.
- **Not wet-surface caustics as a new mechanism.** Light focused through a real
  pool is existing dielectric transport, routed to VCM by the existing map (§9).
  Nothing here adds a specular manifold or touches SMS.
- **Not thin-film interference.** Plain water films are micrometres thick and
  produce none. The interference case is oil, it is Phase 3, and it reuses the
  existing GGX `film_*` slots (§2.2, §8.3).
- **Not droplets.** Dew, beading on waxed paint, and rain on glass are discrete
  refracting lenses with their own magnification, caustics and highlights — no
  `coat_weight` value expresses them, and §6.3's ridge-shedding mask has the
  *wrong sign* for dew, which condenses on convex up-facing surfaces (§8.6).
  Droplets need geometry (instanced spheres or SDF parts) or at minimum a droplet
  normal map, and are out of scope for both phases.
- **Not a generic material-mix primitive.** `coat_weight` covers the coverage
  case; a general two-BSDF blend is §4b, rejected for v1 (§8.4).
- **Not cross-object wetness masking.** `occlusion()` is self-occlusion only, by
  the signals design's explicit v1 decision. "Dry under the overhang" is an
  authored mask or a modelling choice (§6.5).
- **Not deep snow.** Real snow accumulation is geometry with a scattering volume,
  not a coat (§8.2).
- **Not a fix for `composite_material`.** This design routes around it and
  documents why; Landing 6 owns the fix (§4a, debt 2).
- **Not a new integrator behaviour.** Nothing here touches a PDF outside the new
  material's own lobe mixture, no MIS weight changes, no routing changes.

---

## 15. References

**RISE.**
[MATERIALS.md](MATERIALS.md) (the triad contract, the `composite_material` framing,
the add-a-BSDF checklist),
[PHYSICALLY_BASED_PIPELINE_PLAN.md](PHYSICALLY_BASED_PIPELINE_PLAN.md) §"Landing 6"
(the white-furnace audit and Findings A/D — the measurement this design rests on),
[tests/LayeredWhiteFurnaceTest.cpp](../tests/LayeredWhiteFurnaceTest.cpp) (the
living harness and Phase 2's exit gate),
[GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md) (the
signals this consumes, the verb-effect census, the phased-gate form),
[UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md) (PT-default
routing; decision-doc form),
[ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md) (scalar-vs-colour pipe
discipline — the routing hazard in §3.2),
[THIN_FILM_INTERFERENCE.md](THIN_FILM_INTERFERENCE.md) (RISE's own
clearcoat-is-not-thin-film distinction),
[PT_ENV_MIS_DOUBLECOUNT.md](PT_ENV_MIS_DOUBLECOUNT.md) §4a (the measured OIDN
behaviour on near-deterministic speculars),
[GLTF_IMPORT.md](GLTF_IMPORT.md) §"Deferred to Phase 5" (the second customer),
[SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md),
[GUI_ROADMAP.md](GUI_ROADMAP.md) (the OpenPBR-as-conceptual-model posture),
`docs/agentic-redesign/88-procedural-texture-expressiveness-candidates.md` §2
(the adoption laws), `docs/agentic-redesign/90-iteration-ratchet.md` §2 (the
read-the-surface-before-designing lesson, applied in §3.1),
`docs/agentic-redesign/CREATIVITY_JOURNAL.md` (the master triad),
`src/Library/Parsers/README.md` (descriptor-driven chunk parsers).

**External prior art.** Only the facts this design argues from are listed.

- **Ångström, A. (1925), "The Albedo of Various Surfaces of Ground."** *Geografiska
  Annaler* 7. The empirical origin of the wet-albedo exponent form; §2.1's
  `albedo^k`.
- **Lekner, J. & Dorf, M. C. (1988), "Why some things are darker when wet."**
  *Applied Optics* 27(7). The analytic internal-reflection model: the
  critical-angle recycling argument that makes TIR at the water–air interface the
  dominant darkening mechanism. §2.1(ii).
- **Jensen, H. W., Legakis, J. & Dorsey, J. (1999), "Rendering of Wet Materials."**
  *Eurographics Rendering Workshop*. The two-layer subsurface treatment; the
  reference framing for a water layer over a scattering substrate. §2.1.
- **Twomey, S. A., Bohren, C. F. & Mergenthaler, J. L. (1986), "Reflectance and
  albedo differences between wet and dry surfaces."** *Applied Optics* 25(3). The
  independent derivation of the same recycling argument. §2.1.
- **Pope, R. M. & Fry, E. S. (1997), "Absorption spectrum (380–700 nm) of pure
  water. II. Integrating cavity measurements."** *Applied Optics* 36(33). The
  standard reference for the absorption values in §2.5 and the source the shipped
  `colors/water_absorption.spectra` header must name. **Its measured range is
  380–700 nm**; a table extending past 700 nm needs a second named source
  (Kou, Labrie & Chylek 1993, or Segelstein 1981), cited in the file header
  alongside it.
- **Weidlich, A. & Wilkie, A. (2007), "Arbitrarily Layered Micro-Facet
  Surfaces."** *GRAPHITE*. The analytic single-bounce layered model recommended as
  Phase 2's core — **with the §7.4 recycling compensation, which it does not
  itself provide**. §7.4.
- **Belcour, L. (2018), "Efficient Rendering of Layered Materials using an Atomic
  Decomposition with Statistical Operators."** *ACM TOG* 37(4). The accurate
  statistical-operator alternative; the documented upgrade path, and the model
  Finding A's A2 names. §7.4.
- **Heitz, E. et al. (2017), LTC-Layered.** Named in Finding A as the third
  option; declined because its payoff is closed-form polygonal-area-light
  integration, which RISE's NEE already covers by sampling (§7.4).
- **Kulla, C. & Conty, A. (2017), "Revisiting Physically Based Shading at Imageworks."**
  The multiple-scattering compensation RISE already implements for GGX and that
  Phase 2 reuses at the coat–substrate interface. §7.4.
- **OpenPBR Surface specification** (Academy Software Foundation / Adobe &
  Autodesk). Source of the `coat_weight` / `coat_roughness` / `coat_ior` /
  `coat_thickness` parameter shape and of the coverage-weight-as-statistical-mixture
  semantics §7.3 argues from. Adopted as a **naming and conceptual** alignment
  only, per [GUI_ROADMAP.md:82](GUI_ROADMAP.md).
- **glTF `KHR_materials_clearcoat` / `KHR_materials_sheen`.** The second
  customer's contract; the `#if 0` block already maps to them. §7.6.
- **arXiv 2409.00856** — linear text vs. graph-as-data for LLM authorship, 76 % vs
  53 % pass@1; the C-TEXT basis for keeping the whole recipe as one linear
  expression body rather than a painter graph.

**Not used, deliberately.** No production renderer's specific wetness-shader
parameter defaults were adopted — the `k` table in §6.6 is the practical range of
the Ångström exponent form as used in production, not a citation of any one
implementation's numbers, and it is labelled as such. No claim in this document
rests on them.
