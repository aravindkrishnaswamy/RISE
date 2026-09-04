# RISE Materials & BSDFs

How surface scattering is modelled in RISE: what an `IMaterial` is, the
BRDF / SPF / Emitter triad pattern, the sampling protocol that lets PT,
BDPT, and VCM all share the same materials, the SSS pathways, and a
catalogue of every material chunk the parser exposes.

This is the orientation doc. Source of truth for behaviour is the code in
[../src/Library/Materials/](../src/Library/Materials/) and the interfaces
in [../src/Library/Interfaces/](../src/Library/Interfaces/); this doc
points at the right files and explains the conventions that aren't
obvious from any single one.

## 1. `IMaterial` is an aggregate, not a leaf

[IMaterial.h](../src/Library/Interfaces/IMaterial.h) is a thin wrapper
around three optional sub-interfaces:

| Sub-interface | Role | Returned by |
|---|---|---|
| [`IBSDF`](../src/Library/Interfaces/IBSDF.h) | Closed-form scattering function `f_r(ω_i, ω_o)` for **explicit evaluation** (NEE, BDPT vertex connection, MIS weights, OIDN albedo AOV). | `IMaterial::GetBSDF()` |
| [`ISPF`](../src/Library/Interfaces/ISPF.h) | Scattering Probability Function — the **importance sampler**. Produces `ScatteredRay`s with sampling PDFs, lobe types, and IOR-stack updates. | `IMaterial::GetSPF()` |
| [`IEmitter`](../src/Library/Interfaces/IEmitter.h) | Surface emission profile for luminaire materials. | `IMaterial::GetEmitter()` |

A typical material returns non-null for `BSDF` and `SPF`; luminaire materials
also return an `Emitter`; some specialised materials (perfect mirror,
perfect refractor) return `BSDF=NULL` because no continuum BRDF exists for
a delta lobe — the SPF carries the entire scattering behaviour and the
`SpecularInfo` reported through `GetSpecularInfo()` tells the integrator
to evaluate that lobe via specular cascade rather than via NEE / BSDF
sampling.

## 2. The triad pattern: `XxxMaterial` + `XxxBRDF` + `XxxSPF`

Every continuum-lobe material in [Materials/](../src/Library/Materials/)
follows the same three-file layout:

```
GGXMaterial.{h,cpp}    -- thin aggregator: holds painters, news up the BRDF & SPF
GGXBRDF.{h,cpp}        -- IBSDF implementation: closed-form value() / valueNM()
GGXSPF.{h,cpp}         -- ISPF implementation: Scatter() + Pdf() + EvaluateKrayNM()
```

Why both? They satisfy different integrator needs:

- **`Scatter`** is used when the integrator generates a continuation ray
  (path tracing, light-subpath construction, photon tracing). It emits
  a sample with its associated PDF and lobe-type tag.
- **`value`** is used when the integrator already knows both directions
  and needs the BRDF magnitude (NEE shadow rays, BDPT connection terms,
  MIS denominator computation). The same material must answer both
  consistently for MIS to balance.

[`SPFBSDFConsistencyTest.cpp`](../tests/SPFBSDFConsistencyTest.cpp) and
[`SPFPdfConsistencyTest.cpp`](../tests/SPFPdfConsistencyTest.cpp) lock in
the contract: a sampled direction's BRDF (via the BRDF) and its sampling
PDF (via the SPF) must agree with each other.

## 3. Spectral variants: every method has an `NM` form

Spectral rasterizers (`bdpt_spectral_rasterizer`, `vcm_spectral_rasterizer`,
`pixelintegratingspectral_rasterizer`, `pathtracing_spectral_rasterizer`,
`mlt_spectral_rasterizer`) call the wavelength-tagged variants
`valueNM(...)` / `ScatterNM(...)` / `PdfNM(...)` / `GetSpecularInfoNM(...)` /
`GetRandomWalkSSSParamsNM(...)`. RGB rasterizers call the un-suffixed
form. The default implementation of `*NM` typically delegates to the
RGB form for materials that don't disperse (everything except
`DielectricMaterial`'s wavelength-dependent IOR and `BioSpecSkin`'s
spectral RW SSS).

When adding a new BSDF, implement both forms. If the lobe is achromatic,
the `NM` form is one line that delegates.

## 4. Delta vs. continuum lobes — `SpecularInfo`

`IMaterial::GetSpecularInfo` reports whether the surface scatters via a
delta distribution (perfect mirror / refraction) and, if so, the IOR at
the interface. Three integrator subsystems read this:

1. **MIS weight machinery** (`MISWeight`, `EvaluateNEE`, `EvaluateMerges`):
   delta lobes contribute zero to NEE and zero to merge density, so the
   numerator and denominator must skip them. Getting this wrong produces
   the "delta-position-light vs delta-surface-scatter" trap captured in
   [skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md).
2. **SMS solver** (`ManifoldSolver`): builds a chain of specular vertices
   between an eye vertex and a light, using `SpecularInfo` to decide
   constraint type at each vertex (reflection / refraction / TIR).
3. **PT firefly suppression**: a path that has passed through specular
   AND landed on non-specular shading must suppress emission to avoid
   double-counting the SMS / connect strategies.

The default implementation returns `SpecularInfo()` (non-specular). Only
`DielectricMaterial`, `PerfectReflectorMaterial`, `PerfectRefractorMaterial`,
and (selectively) `PolishedMaterial` override.

## 5. Subsurface scattering: three pathways, one material flag set

A single material can route SSS through three different transports
depending on which method it overrides:

| Pathway | Trigger | Where the work happens | Use when |
|---|---|---|---|
| **Diffusion-profile BSSRDF** | `GetDiffusionProfile()` returns non-null | Integrator does importance-sampled disk-projection probes; `ISubSurfaceDiffusionProfile::Rd(r)` weights each entry. | Skin, marble, jade — slabs where the diffusion approximation is accurate. |
| **Random-walk SSS** | `GetRandomWalkSSSParams()` returns non-null (RGB) and/or `GetRandomWalkSSSParamsNM` returns true (spectral) | Integrator traces a volumetric random walk inside the mesh using `sigma_a` / `sigma_s` / HG `g`. `IsVolumetric()` returns true so BDPT uses `kray` for throughput instead of `BSDF·cos/pdf`. | Highly scattering media, dispersive skin (`BioSpecSkin` overrides only the NM form so RGB renderers fall back to the diffusion path), arbitrary non-flat geometry. |
| **Empty SPF + Composite outer layer** | A material with an `IBSDF` only, layered under a transmissive top via `composite_material`. | The composite layered model (§6) handles the boundary; the inner material need only provide the diffuse interior. | Layered surfaces where SSS is one component of a stack. |

The two SSS modes are **mutually exclusive**: a material picks one. The
companion test for energy conservation is the
[BSSRDFFurnace](../scenes/Tests/BSSRDFFurnace/) scene set; the unit test
is [`BSSRDFSamplingTest.cpp`](../tests/BSSRDFSamplingTest.cpp).

## 6. Layered materials via `composite_material`

[`CompositeMaterial`](../src/Library/Materials/CompositeMaterial.h)
synthesises a single `IMaterial` from a `top` and `bottom` material plus
a thickness, an extinction scalar painter (inline scalar or `scalar_painter`
reference — a colour painter is rejected), and per-lobe recursion budgets. The
construction merges sub-interfaces independently:

- The `BSDF` is taken from whichever of `top` / `bottom` provides one
  (top wins when both do — there's no per-lobe blend at the BSDF level).
- The `SPF` becomes a `CompositeSPF` that stochastically transmits the
  ray through the top, evaluates the bottom, and returns. Per-lobe
  recursion budgets bound the cost.
- The `Emitter` similarly composites if both layers emit.

This is the classic "varnished wood" / "skin over fat" / "metal flake
under clearcoat" idiom. `composite_material` is the general
layer-stacking primitive; PBR-style additive lobes (sheen + base)
are expressed as their own dedicated materials (`sheen_material`).

**But for a transparent film over an opaque substrate — water, oil,
lacquer, clearcoat — reach for `coated_material` (§6.1) instead, and for
cloth — a sheen lobe over a diffuse or microfacet weave — reach for
`fabric_material` (§6.2). Both exist because the composite route does
not merely approximate those cases, it evaluates the wrong BSDF.**

## 6.1 `coated_material` — a film with *coverage*, and a real layered BSDF

[`CoatedMaterial`](../src/Library/Materials/CoatedMaterial.h) +
[`CoatedBRDF`](../src/Library/Materials/CoatedBRDF.h) +
[`CoatedSPF`](../src/Library/Materials/CoatedSPF.h), with the layer
algebra in [`CoatedLayer.h`](../src/Library/Materials/CoatedLayer.h).
Full design: [WETNESS_COAT_DESIGN.md](WETNESS_COAT_DESIGN.md) §7.

**Why it exists rather than reusing §6.** Both existing routes to a
coated surface answer direct lighting with the *wrong* BSDF:
`composite_material` forwards one sub-material's BSDF (top wins), and
`polished_material` returns a bare `LambertianBRDF` for a
Fresnel-coated surface. So NEE, BDPT vertex connections, and MIS
denominators all saw an **uncoated** substrate.
`CoatedBRDF::value`/`valueNM` is the closed-form *combined* response,
which is the point of the material.

| parameter | pipe | notes |
|---|---|---|
| `base` | material ref | **allowlisted** — see below |
| `coat_weight` | `IScalarPainter` | [0,1] sub-pixel **coverage** fraction; the spatially varying slot |
| `coat_ior` | `IScalarPainter` | 1.33 water, 1.5 varnish; clamped to [1, 3] |
| `coat_roughness` | `IScalarPainter` | coat lobe's GGX α; clamped to [1e-3, 1] |
| `coat_thickness` | `IScalarPainter` | world length; feeds Beer–Lambert |
| `coat_absorption` | `IScalarPainter` | 1/length; **scalar pipe deliberately** (a large coefficient on the colour pipe would be clamped by the JH uplift) |
| `coat_tint` | `IPainter` | colour of one normal-incidence traversal; genuinely a colour |

Names follow the OpenPBR surface spec so a future MaterialX/OpenPBR
import is a rename rather than a redesign.

**The substrate allowlist.** `base` accepts only `lambertian_material`,
`orennayar_material`, `ggx_material`, and
`pbr_metallic_roughness_material` (which resolves to a `ggx_material`
at scene-build time), and refuses anything that emits. Two independent
reasons: the layered model needs the substrate's *directional albedo*
to run its interreflection series, which a luminaire / BSSRDF /
volumetric random walk cannot supply; and §7.5's lobe budget only
closes because the substrate's lobe count is known. Refusal happens at
**parse time** with a diagnostic naming the allowlist, in both
`Job::AddCoatedMaterial` and `RISE_API_CreateCoatedMaterial`.

**Three things worth knowing before extending it:**

1. **`coat_weight` is coverage, not gloss.** At coverage `c` the
   response is the mixture `c·(coated) + (1−c)·(bare)`, and the bare
   branch reaches the substrate through *air* — no Fresnel
   transmission, no interreflection. Both branches conserve energy, so
   partial coverage does not darken a white surface.

2. **The wet darkening is transport, not a fitted exponent.** The
   Saunderson recycling `1/(1 − r_i·R(λ))` is evaluated *per
   wavelength*, so the channels where the substrate is bright are
   amplified most — darkening and chroma boost fall out together. This
   is why there is no `substrate_wet_exponent`: an additional `R^k`
   would double-count the same physics.

3. **Anything shared by both directions must be view-independent.** The
   recycling term multiplies `f_base` symmetrically, so it reads
   `IBSDF::hemisphericalAlbedo` (whose contract forbids touching
   `ri.ray`) and *not* `IBSDF::albedo`, which is the OIDN AOV and is
   legitimately view-dependent. Using the latter made the BRDF
   non-reciprocal by ~28 % at grazing — invisible to every
   self-consistency check, which is why
   `SPFBSDFConsistencyTest`'s Part E sweeps `f(a→b)` against `f(b→a)`.

Guards: `LayeredWhiteFurnaceTest` configs 11–18 (energy, including a
red-proof pair that measures the ~45 % loss when the interreflection
term is removed), `SPFBSDFConsistencyTest` (value↔Scatter, pointwise,
and reciprocity), `SPFPdfConsistencyTest` (the real mixture PDF),
`CoatedMaterialChunkTest` (parser, allowlist, editor introspection),
and [`scenes/Tests/Materials/coated_material.RISEscene`](../scenes/Tests/Materials/coated_material.RISEscene).

## 6.2 `fabric_material` — sheen with its energy paid for, and a weave direction

[`FabricMaterial`](../src/Library/Materials/FabricMaterial.h) +
[`FabricBRDF`](../src/Library/Materials/FabricBRDF.h) +
[`FabricSPF`](../src/Library/Materials/FabricSPF.h), with the preset
table in [`FabricPresets.h`](../src/Library/Materials/FabricPresets.h)
and the baked directional-albedo tables in
[`SheenDirectionalAlbedo.h`](../src/Library/Materials/SheenDirectionalAlbedo.h).
Full design: [CLOTH_FABRIC_DESIGN.md](CLOTH_FABRIC_DESIGN.md) §9.

**Why it exists rather than reusing `sheen_material` + `composite_material`.**
That stack has two independent defects, and the second was *measured*
rather than assumed:

1. `composite_material` forwards ONE sub-material's BSDF, so NEE and
   BDPT/VCM connections see a bare substrate — the same defect §6.1
   opens with.
2. **The substrate is never reached at all.** `SheenSPF` is
   reflection-only (a cosine-hemisphere draw about the ray-facing
   normal), so it never hands `CompositeSPF`'s random walk a downward
   ray. `LayeredWhiteFurnaceTest`'s downward-ray probe measures **0 %**
   downward emission at both θ = 0 and θ = 80, and config 6
   (sheen-over-PBR) consequently reproduces config 2 (bare sheen) to MC
   noise. It is the same structural defect config 7 documents for a GGX
   top layer.

And a third that neither route could fix: nothing anywhere subtracts
the sheen's energy from the base, so a white sheen over a white diffuse
returns more light than it receives at grazing.

**The model.** A **Kulla-Conty multiple-bounce coupling** between the
fuzz layer and the substrate:

```
f(l, v) = sheenColor · D_Charlie · V_Charlie / N(l, v)  +  f_base(l, v) · scale(l, v)

N(l, v)     = max( 1, E(α, n·v), E(α, n·l) )                            m = max3(sheenColor)
scale(l, v) = (1 − m·Ê(α, n·v)) · (1 − m·Ê(α, n·l)) / (1 − m·Ê̄(α))      Ê = min(E, 1)
```

`E` is RISE's own bake of the Charlie lobe's directional albedo, on a
**grazing-warped** cos θ axis, and `Ê̄` the hemispherical mean of the
clamped lobe (`tools/SheenDirectionalAlbedoGen.cpp` →
`SheenDirectionalAlbedo_LUTData.cpp`), on the medulla table's
re-derive-rather-than-vendor precedent.

`N` is the energy bound: the Charlie fit exceeds 1 near grazing (E()
reaches ~1.196 even above the roughness floor), so the sheen term is
divided by a **symmetric** normaliser and the base scaled by the clamped
`Ê`. Both are exactly 1 / exactly `E` outside that sliver, so nothing
else moves. The table's cos θ axis is additionally **floored at its
first node** (μ₁ = 1/3969 since round 9, 2026-09-04's 32→64 rebake; was
1/961) with constant extrapolation below — without that, the first cell
interpolates E up from zero across the lobe's peak and a white fabric
returns ρ = 1.71.

**Exactness class: energy-bounded, and nearly conserving.** Worst ρ
measured over the whole reachable domain (α ∈ [0.04, 1], white
Lambertian base, m = 1), on the round-9 64×64 table: **1.0019** for
n·v ≥ 0.0349, **1.0075** between there and μ₁, **1.0018** below μ₁ —
global max **1.0075**, nothing reaching 1.01. The outer and inner bands
sit at **α ≈ 0.95**: E at cos θ node 1 is concave in α, so the log-α
chord in the last (widest) cell under-reads the true lobe. The middle
band's worst has relocated to **α ≈ 0.065**, near the roughness floor —
doubling the table resolution roughly halved the other two bands but
only reduced this one by about half as much, because its dominant
driver moved once the α ≈ 0.9 mechanism shrank below it. Closing the
middle band's residual further wants a fresh root-cause hunt near the
floor, not another blind resolution doubling (see [CLOTH_FABRIC_DESIGN.md](CLOTH_FABRIC_DESIGN.md)
§9.2 and §15 debt 18).

**Why this form and not glTF's.** The design doc originally specified
the glTF `KHR_materials_sheen` scaling with the two arms combined by a
`min`. Implementation-time measurement rejected it, and the arithmetic
is worth keeping because the trap recurs:

- glTF's own form is the **single arm** `1 − m·E(α, n·v)`. It conserves
  energy *exactly* over a white Lambertian base — `ρ = m·E(v) + (1 −
  m·E(v)) = 1` — and is **not reciprocal**.
- `min(1 − m·E(v), 1 − m·E(l))` restores reciprocity and destroys the
  energy identity: near normal incidence `E(v) → 0` while the `min`
  still picks `1 − m·E(l)` for every `l`, so the base loses `Ē`'s worth
  of energy the sheen never returns. Measured at ρ = 0.863 at normal
  incidence for α = 0.5, against a required 1.000.
- The **product** form above is both. It is the closed form of the
  adding-doubling inter-reflection series between a lossless fuzz layer
  and the base — energy the fuzz intercepts on the way in is
  re-scattered onto the substrate rather than deleted — and
  `1/(1 − m·Ê̄)` is that series' sum. For a white Lambertian base at
  **any** m and **any** α — wherever the normaliser `N` is 1, i.e.
  n·v ≥ 0.03 — `ρ(v) = m·E(v) + (1 − m·E(v))·(1 − m·Ê̄)/(1 − m·Ê̄) = 1`
  exactly.

**Two consequences worth stating up front.** The denominator
*brightens* the base away from grazing — its supremum is `1/(1 − m·Ê̄)`,
measured 1.081 at α = 0.08 rising to 1.434 at α = 1 —
because that is where the intercepted energy is re-emitted. It is
exactly balanced by the darkening at grazing, which is what
`LayeredWhiteFurnaceTest`'s Lambertian fabric rows landing on ρ = 1.000
prove. And the sheen roughness is clamped to **[0.04, 1]**, tighter than
`sheen_material`'s 1e-3. Its criterion is *"the smallest α with
max over μ ≥ 0.03 of E(α, μ) ≤ 1"*; the generator prints the scan on
every bake, and 0.04 clears it.

**Migrating from `sheen_material`.** That floor is not arbitrary and it
is not going to move: it is the baked `SheenDirectionalAlbedo` table's
own exactness criterion above, which the energy-compensation term needs
to stay energy-bounded (§ above). `sheen_material` carries no such
compensation term, so it has nothing forcing the same floor and keeps
its much looser `1e-3`. A material re-pointed from `sheen_material` onto
`fabric_material` — by hand, or via `add_wetness`'s glTF import path
(docs/GLTF_IMPORT.md §15) — with a `sheen_roughness` authored below 0.04
therefore renders visibly differently: what was a tight, near-mirror
highlight under `sheen_material` clamps to the `0.04` floor under
`fabric_material`, i.e. a BROADER, SOFTER highlight, not a bug in the
migration. If a material genuinely needs `sheen_roughness` below 0.04,
keep it on a standalone `sheen_material` (uncompensated, but unfloored)
rather than wrapping it as `fabric_material`.

**Weave direction is the substrate's job.** The sheen lobe is strictly
isotropic (Charlie's `D` normaliser and its Λ visibility are both
isotropic-only fits, and a 2-D `E(α, cosθ)` table cannot compensate a 4-D
anisotropic albedo). `weave_rotation` instead rotates the tangent frame
handed to the substrate, via the same `MicrofacetUtils::RotateTangent`
helper GGX's own `tangent_rotation` uses — so the two simply add, and
`ward_*` / `ashikminshirley_*` gain a rotation input they do not have on
their own.

**Presets.** `fabric cotton|linen|denim|wool|silk|satin|velvet|custom`
seeds every slot the author did not write — RISE's first *multi-slot*
preset. It cannot configure the substrate (the chunk holds a reference
it can neither retype nor re-parameterise), so a preset bound over the
wrong substrate class logs a **warning** and still builds; contrast
`coated_material`, which *errors* on a substrate outside its allowlist.
The allowlist itself is the same one: `lambertian_material`,
`orennayar_material`, `ggx_material`, `pbr_metallic_roughness_material`
transitively — and, since Phase 2, **`weave_material`** (§6.3), which is
the physical stack: surface fuzz is loose fibre ends standing off the
woven cloth underneath. `silk`, `satin` and `denim` now *recommend* that
substrate rather than an anisotropic GGX one, and a GGX base under those
three warns; the GGX numbers stay in `FabricPresets.h` as the documented
Phase-1 fallback, and the composition is still legal.

**`make_fabric`'s warp-only mint.** The agent verb that mints a
`weave_material` substrate for `denim`/`silk`/`satin` writes the author's
re-homed colour painter onto `warp_color` **only** — `weft_color` is left
unwritten, so it resolves to the chunk's own default (the preset's weft
colour where one is set, white/undyed otherwise). This is deliberate, not
an omission: denim's look *is* an indigo warp floating over an undyed
weft, and the draft decides how much of each shows. The verb's own
success message names it; bind `weft_color` explicitly afterward for a
single uniform dye across both families — or pass the verb's OPTIONAL
`weft_color` argument (round 9, reviewer P2.4) and it binds that chunk's
`weft_color` for you as part of the same mint: `"match"` binds the SAME
painter `warp_color` took, or name any existing COLOUR painter chunk to
bind that one instead — the gate is colour-PIPE-aware, so a
`scalar_painter` name is refused even though it is Painter-category too
(round 9, reviewer P2.4 fix: it used to pass a category-only check and
fail later, opaquely, inside `Job::AddWeaveMaterial`). The argument only
has somewhere to land on the MINT half of this path — it refuses,
changing nothing, when the preset does
not mint a weave at all, or when the bound base was already a
`weave_material` and so REUSED rather than minted (bind `weft_color` on
that existing chunk directly instead).

Guards: `LayeredWhiteFurnaceTest` configs 21–36 (energy: the Lambertian
rows `kPosturePass` on the exact identity, the Oren-Nayar and GGX rows
pinned to the exact directional albedo built from independently measured
bare-substrate reference rows 19–20 and re-derived from the baked table
rather than from the material's own helpers, plus a **grazing check** at
θ ∈ {80, 85, 88, 89} — the band the angle columns cannot reach, and where
a uniform cos θ axis once broke the identity by +1.05 absolute), `SPFBSDFConsistencyTest` (value↔Scatter pointwise, and
reciprocity — which now also covers bare `sheen_material`, a
pre-existing hole), `SPFPdfConsistencyTest` (the real mixture PDF, RGB
and NM, with an explicit check that the configuration under test can
*discriminate* a branch-local density), `FabricMaterialChunkTest`
(parser, presets, allowlist, the `hemisphericalAlbedo` error
measurement — decomposed into fabric's own uncorrelated-response
residual and the substrate's pre-existing `hemisphericalAlbedo` debt —
and spectral parity), and
[`scenes/Tests/Materials/fabric_presets.RISEscene`](../scenes/Tests/Materials/fabric_presets.RISEscene).

## 6.3 `weave_material` — cloth with a pattern scale

[`WeaveMaterial`](../src/Library/Materials/WeaveMaterial.h) +
[`WeaveBRDF`](../src/Library/Materials/WeaveBRDF.h) +
[`WeaveSPF`](../src/Library/Materials/WeaveSPF.h), with the draft
functions and the preset table in
[`WeavePresets.h`](../src/Library/Materials/WeavePresets.h) and the
fibre-lobe primitives shared with `hair_material` in
[`FibreLobeMath.h`](../src/Library/Materials/FibreLobeMath.h).
Full design: [CLOTH_FABRIC_DESIGN.md](CLOTH_FABRIC_DESIGN.md) §10.

**Why it exists.** §9.9 gate 9b measured `fabric_material`'s limit and
named it. An isotropic Charlie sheen over an anisotropic GGX substrate
preserves **95 % (silk) / 99 % (satin)** of the substrate's highlight
anisotropy — so §9.5's delegation works almost losslessly — and the frame
*still* reads as brushed metal. The deficit is the absence of a **pattern
scale**: a single elliptical lobe with a painted rotation field cannot
make discrete floats with their own orientation and mutual shadowing, and
painting the rotation harder produces a checkerboard rather than cloth.

**The model.** Two thread families, warp and weft, each with its own
tangent, dye and pair of lobes, mixed by a weave-draft coverage field:

    f(i,o) = (1 − gap) · Σ_k a_k · M_k(i,o) · [ f_surf,k + A_k · f_vol,k ]

- The **surface lobe** is Sadeghi et al. 2013's microcylinder R-lobe
  built on RISE's *hair* primitives: a Kim-2002 cylinder-geometry
  Fresnel, d'Eon's energy-conserving longitudinal `Mp` in place of
  Sadeghi's plain Gaussian, and a trimmed logistic in place of his bare
  `cos(φ_d/2)`. It is **untinted** — a dielectric's specular reflection
  preserves the incident spectrum, so a coloured fabric keeps a white
  highlight, which is one of the four places Sadeghi demonstrates
  Irawan-Marschner failing.
- The **volume lobe** is his `f_r,v` with the Chandrasekhar
  `1/(cos θ_i + cos θ_o)` form, a second `Mp` at **twice** the surface
  width (Table II's γ_v ≈ 2 γ_s recurs in all six of its rows, so it is
  derived rather than authored), and the dye.
- **Masking** is his Eq. 7–9 verbatim, correlation width fixed at 20°.
  His Eq. 15 normaliser `Q` is *not* used: it reads the view direction
  and nothing else, so it cannot be reciprocal, and the gap is carried
  instead as a direction-independent energy factor.

**Drafts.** `weave plain|twill_2_1|twill_3_1|satin_5|custom`, with exact
mean warp coverages 1/2, 2/3, 3/4, 4/5. The field is **continuous**, not
a cell lookup: yarn edges are smooth ramps and the whole pattern fades to
its own mean as the pixel footprint outgrows the cell, which is both the
correct minification limit and the anti-aliasing. `custom` binds a
`coverage` scalar painter instead.

**Sampling** is Zhu et al. 2024 §5.1's attenuation-proportional
lobe-selection sampler — family by coverage, then surface-vs-volume by a
Fresnel-vs-dye split — with the sample **repriced against the full
four-lobe mixture**, because the lobes' supports overlap. The surface
lobe is drawn in the fibre frame with d'Eon's exact longitudinal inverse
and the trimmed logistic's exact azimuthal one, and the logistic is
**trimmed to the visible azimuth range at each fibre latitude**, which is
what keeps the density normalised over the hemisphere rather than over
the sphere without a view-dependent acceptance factor.

**Energy** is *bounded*, not conserving, and the model says so: the
source model ignores inter-thread multiple scattering and the masking
term removes energy nothing puts back. The volume lobe's normaliser
`C_v` is the *exact* hemispherical integral of its bracket at zero tilt
(not a loose bound — an earlier revision's `C_v = 2(1+k_d)` WAS a loose
bound used as if exact, and was ~2-2.3× too large, the session's one
substantive fix; see §10.3). `hemisphericalAlbedo` is now a closed form
with **no fitted constants**, exact at normal incidence, with a stated
±40% exactness class for its bihemispherical approximation.

**Presets.** `fabric denim|silk|satin|linen|custom` seeds every slot
including the draft.

**Transmission (slice P2-B, shipped).** `transmission none|thin` (default
`none`; linen/silk/satin default to `thin`, denim/custom stay `none`).
Under `thin`, `ScattersFullSphere` and `CouldLightPassThrough` both become
`true` and two lobes activate: a DELTA transmission through the open gaps
(`f_delta = gap(x)·δ(i+o)/(i·n_s)`, weighted by the SAME `gap` field P2-A
shipped — the chunk also accepts `sheer` as an alias for it — reported
`isDelta = true`, `kray = 1`, since its coefficient and its selection
probability cancel exactly) and a Lambertian diffuse transmission through
the yarn itself (`f_t,d,k = (1-gap)·transmit_k·T_k/π` per family, reusing
the family's own dye; `warp_transmit`/`weft_transmit`, preset defaults
linen 0.25 / silk 0.35 / satin 0.15 / denim 0.0). The diffuse lobe's
budget is carved out of the SAME volume lobe rather than added on top —
the reflect-side volume term is scaled by `(1 - transmit_k)` — so the
total never exceeds the pre-P2-B budget. `transmission none` is
bit-identical to the original slice-A code: every new expression is
reached only through a `thin`-gated branch. Full detail, including a
disclosed BDPT limitation for this material class (a flat, zero-thickness
`ScattersFullSphere()` geometry — RISE's first — appears to trip a
near-singular BDPT connection; PT is unaffected and independently
verified), is in CLOTH_FABRIC_DESIGN.md §10.1a.

Guards: `LayeredWhiteFurnaceTest` configs 39–49 (energy: `kPostureBounded`
plus a locked measured curve per preset at two view sets, the
fabric-over-weave row, and an independent white-weave energy-floor check
built from a from-scratch quadrature rather than `value()`),
`SPFBSDFConsistencyTest` (value↔Scatter pointwise, and reciprocity at
~2e-15 / ~5e-15 with a non-zero weave rotation and non-zero float tilts
respectively), `SPFPdfConsistencyTest` (the four-lobe mixture PDF, RGB
and NM, with the hemisphere integral at ≥ 0.998), `WeaveMaterialChunkTest`
(the draft functions enumerated exhaustively, the footprint fade and the
yarn edge's continuity, the preset table slot by slot, the two `coverage`
diagnostics, fabric-over-weave, and the `hemisphericalAlbedo` error
measurement), `FibreLobeMathTest` (a deterministic value table pinning the
shared fibre-scattering primitives against both `hair_material` and this
material silently drifting), `LayeredWhiteFurnaceTest` rows 50–51 and the
new cross-hemisphere block in `SPFBSDFConsistencyTest`/`SPFPdfConsistencyTest`
and `WeaveMaterialChunkTest::TestThinTransmission` (P2-B transmission; see
§10.1a for what each pins), `tests/FabricRenderTest.cpp`'s backlit-curtain
render test, and
[`scenes/Tests/Materials/weave_presets.RISEscene`](../scenes/Tests/Materials/weave_presets.RISEscene) /
[`scenes/FeatureBased/Materials/sheer_curtain.RISEscene`](../scenes/FeatureBased/Materials/sheer_curtain.RISEscene).

## 7. Luminaires — materials that emit

Luminaire materials add an `IEmitter` to the triad:

- [`LambertianLuminaireMaterial`](../src/Library/Materials/LambertianLuminaireMaterial.h)
  — area emission from a (typically Lambertian) BRDF base.
- [`PhongLuminaireMaterial`](../src/Library/Materials/PhongLuminaireMaterial.h)
  — directional Phong-lobe emission.

Both work in tandem with `arealight_shaderop` and the area-light NEE
path. A surface is a luminaire iff its material returns non-null from
`GetEmitter()`; nothing else is special about the geometry. The
[SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md) section on `power` covers
the unit story.

## 8. Material catalogue (parser keywords)

These are the 25 material chunks registered in
[`CreateAllChunkParsers()`](../src/Library/Parsers/ChunkParserRegistry.cpp).
Grouped by primary lobe shape — see the corresponding `*Material.h`
file for parameter-by-parameter behaviour.

**Ideal / delta lobes** (`SpecularInfo` non-trivial):
- `perfectreflector_material` — perfect mirror (delta reflection).
- `perfectrefractor_material` — perfect refraction with Fresnel split.
- `dielectric_material` — full Fresnel dielectric with optional
  wavelength-dependent IOR for dispersion.
- `polished_material` — Lambertian base with a delta specular layer
  weighted by Fresnel.

**Lambertian / matte:**
- `lambertian_material` — diffuse only.
- `orennayar_material` — rough-diffuse Oren-Nayar generalisation.

**Microfacet:**
- `ggx_material` — anisotropic GGX with two Fresnel modes
  ([`FresnelMode`](../src/Library/Interfaces/IMaterial.h)):
  `eFresnelConductor` (default; multiplies real `ior`/`ext` Fresnel by a
  `specular` painter as a tint) and `eFresnelSchlickF0` (treats `specular`
  as F0; required by glTF metallicRoughness).
- `pbr_metallic_roughness_material` — glTF-spec composition. Not its own
  material class; resolved at scene-build time in
  [`Job::AddPBRMetallicRoughnessMaterial`](../src/Library/Job.cpp) into
  a painter graph + a single `ggx_material` in `eFresnelSchlickF0` mode.
- `cooktorrance_material` — Cook-Torrance microfacet.
- `ward_isotropic_material`, `ward_anisotropic_material` — Ward.
- `ashikminshirley_anisotropicphong_material` — Ashikhmin-Shirley.
- `isotropic_phong_material` — classic Phong.

**Layered / additive lobes:**
- `coated_material` — a transparent dielectric film over a
  **restricted** substrate, with the film's *coverage* as a first-class
  spatially varying slot (§6.1). The wetness / clearcoat / varnish /
  oil-film material.
- `fabric_material` — an **energy-compensated** Charlie sheen lobe over
  a **restricted** substrate, with the weave direction delivered as a
  rotation of the frame the *substrate* is evaluated in (§6.2). The
  cloth FUZZ material.  It also **forwards its substrate's
  transmission**: over a `weave_material` under `transmission thin` the
  stack stays see-through, with the sheen layer attenuating the light
  that passes rather than blocking it (CLOTH_FABRIC_DESIGN.md §15
  debt 22).
- `weave_material` — a structured **two-thread-family** cloth BSDF: warp
  and weft, each with their own direction, dye and pair of fibre lobes,
  mixed by a weave-draft coverage field (§6.3). It is what makes satin's
  float sheen and denim's twill wale; it has no sheen term of its own,
  and it is an accepted `fabric_material` substrate, which is the
  physical stack (fuzz over weave).
- `composite_material` — top/bottom layered composition (§6).
- `sheen_material` — Charlie sheen lobe, **uncompensated and standalone**
  (intended to layer over a base via `composite_material`, which does
  not actually reach the base — see §6.2). Prefer `fabric_material`.
- `schlick_material` — Schlick approximation as a standalone material.

**Subsurface scattering:**
- `subsurfacescattering_material` — diffusion-profile BSSRDF (§5).
- `randomwalk_sss_material` — random-walk volumetric SSS (§5).
- `donner_jensen_skin_bssrdf_material` — Donner-Jensen multi-layer skin
  diffusion profile.
- `biospec_skin_material` — biophysically-based skin, **spectral-only**
  random walk (RGB renderers fall back to the diffusion variant via the
  `GetRandomWalkSSSParamsNM` override pattern from §3 / §5).
- `generic_human_tissue_material` — bulk-tissue scattering coefficients
  for human-body rendering.

**Translucency:**
- `translucent_material` — thin translucent surfaces (paper, leaves).

**Luminaires** (§7):
- `lambertian_luminaire_material`
- `phong_luminaire_material`

**Data-driven:**
- `datadriven_material` — measured BRDF (MERL / Matusik).

## 9. Adding a new BSDF — checklist

1. **Pick the right base.** If the new lobe layers on top of an existing
   material, it's a `*_material` whose constructor accepts a base
   material reference and is stacked via `composite_material`. If it's a
   standalone closed-form lobe, it's a fresh triad
   (`XxxMaterial` + `XxxBRDF` + `XxxSPF`).
2. **Implement the triad.** `XxxBRDF::value` and `XxxBRDF::valueNM` for
   evaluation; `XxxSPF::Scatter` and `XxxSPF::ScatterNM` for sampling;
   `XxxSPF::Pdf` and `XxxSPF::PdfNM` for MIS — the consistency tests
   ([`SPFBSDFConsistencyTest`](../tests/SPFBSDFConsistencyTest.cpp),
   [`SPFPdfConsistencyTest`](../tests/SPFPdfConsistencyTest.cpp))
   will catch a mismatched pair.
3. **Override `GetSpecularInfo` / `GetSpecularInfoNM`** if the lobe is
   delta, so the integrators handle it correctly (§4).
4. **Override `albedo`** with a closed-form directional-hemispherical
   reflectance estimate so OIDN's albedo AOV is noise-free.
5. **Override `IsVolumetric`** if the SPF embeds Beer-Lambert attenuation
   in `kray` (random-walk SSS, participating media bounded by surfaces).
6. **Wire up parser + API + Job:**
   - New `RISE_API_AddXxxMaterial` (preserve ABI;
     [AGENTS.md change checklist](../AGENTS.md))
   - New `Job::AddXxxMaterial` and `IJob::AddXxxMaterial`
   - New `XxxMaterialAsciiChunkParser` registered via `add(...)` in
     `CreateAllChunkParsers()` ([Parsers/README.md](../src/Library/Parsers/README.md)
     §"Adding A New Chunk Parser")
   - All five build projects updated ([../CLAUDE.md](../CLAUDE.md) §
     "Source-file add/remove — touch ALL five build projects")
7. **Add a test** under [tests/](../tests/) that exercises the closed
   form against a known reference (white furnace, energy conservation,
   reciprocity) and add a regression scene under
   [scenes/Tests/Materials/](../scenes/Tests/Materials/).

## 10. Cross-references

- Scene-language reference for material chunks:
  [src/Library/Parsers/README.md](../src/Library/Parsers/README.md)
- Scene-authoring traps (light direction, `power` units, transform
  precedence, alpha-mask gotchas):
  [SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md)
- Subsurface-scattering math (BSSRDF profiles, RW absorption boundary):
  source in [Materials/BurleyNormalizedDiffusionProfile.h](../src/Library/Materials/BurleyNormalizedDiffusionProfile.h),
  [Materials/MultipoleDiffusion.h](../src/Library/Materials/MultipoleDiffusion.h),
  [Materials/DonnerJensenSkinDiffusionProfile.h](../src/Library/Materials/DonnerJensenSkinDiffusionProfile.h).
- glTF PBR material composition: [GLTF_IMPORT.md](GLTF_IMPORT.md) §4.
- BDPT vertex protocol (medium vs surface vertices, area-measure PDF
  Jacobians) for materials with `IsVolumetric() = true`:
  [BDPTVertex.h](../src/Library/Shaders/BDPTVertex.h).
