# Thin-Film Interference for Heat-Colored Metals — Design Doc

**Status:** DESIGN (no code landed yet) · **Created:** 2026-06-07 · **Owner:** master-controller session
**Goal artifact:** a rendered guilloché titanium watch dial — engraved rose-engine pattern,
torch-gradient oxide coloring, physically-based iridescence — plus a general thin-film
BRDF that also covers steel, tantalum, and niobium heat-tint/anodize colors.

This doc is the single source of truth for the feature. It is written so the effort can be
picked up cold. Update it as phases land.

---

## 1. Goal & scope

Implement physically-based thin-film interference (heat-tint / anodization color) as a
first-class material capability in RISE's hero-wavelength spectral path tracer, validated
against ground-truth optics, and use it to render a guilloché watch dial.

Three phases:

1. **Ground truth + theory** (standalone, *no renderer integration*): a transfer-matrix
   reference + Airy single-film closed form, cross-checked; curated spectral n,k data for
   four substrate/oxide pairs; a swatch validation gate against the known anodization chart.
2. **Renderer integration**: a thin-film Fresnel on the existing GGX microfacet conductor
   BSDF — exact on the spectral path, LUT-based on the RGB path — with scene-language
   surfacing, energy-compensation handling, and a canonical validation+stress test scene.
3. **The dial**: procedural guilloché heightfield, spatially-varying oxide thickness from a
   torch-heat model, final studio render + turntable.

**Generality requirement:** the model must be substrate- and film-agnostic. Adding steel,
tantalum, niobium (or any metal + its oxide) must be **data only — no code**. The optical
math is N-layer from day one; a single film is the shipped material, multi-layer is a
documented extension point.

---

## 2. Background — what exists, what's missing

- **No thin-film model exists.** The only "iridescent" class,
  [IridescentPainter.h](../src/Library/Painters/IridescentPainter.h), is an unrelated
  color-lerp `IPainter` — *not* a physical model. Do not mistake it for one.
- Conductor reflectance is real complex-IOR (n+ik) Fresnel, computed in exactly one place.
- Thin-film is a known-TODO: [PHYSICALLY_BASED_PIPELINE_PLAN.md:193](PHYSICALLY_BASED_PIPELINE_PLAN.md)
  lists `KHR_materials_iridescence` (the glTF name for thin-film) as a planned extension,
  with Belcour-Barla 2017 cited as the intended approach (lines ~960-968).

---

## 3. Codebase survey — load-bearing facts (with file refs)

| Fact | Where | Consequence |
|---|---|---|
| **One conductor-Fresnel chokepoint**, templated on `T ∈ {Scalar, RISEPel}` | `Optics::CalculateConductorReflectance<T>` [Optics.hpp:34](../src/Library/Utilities/Optics.hpp:34) | Thin-film is a sibling evaluator + a new `FresnelMode`, not a refactor. Called from GGX ([GGXSPF.cpp:225/427](../src/Library/Materials/GGXSPF.cpp:225), [GGXBRDF.cpp:177/283](../src/Library/Materials/GGXBRDF.cpp:177)) and 5 Cook-Torrance sites. |
| **A BSDF sees its wavelength on the spectral path** | `valueNM(vLightIn, ri, const Scalar nm)` [IBSDF.h:61](../src/Library/Interfaces/IBSDF.h:61); `ScatterNM(ri, sampler, nm, …)` [ISPF.h:141](../src/Library/Interfaces/ISPF.h:141) | We evaluate Airy reflectance **directly per hero wavelength** — no spectral antialiasing needed on the primary path. The single most important fact for the feature. |
| All 4 hero wavelengths evaluated independently; HWSS companions via `EvaluateKrayNM` else `valueNM` | [PathTracingIntegrator.cpp:4046/4053](../src/Library/Shaders/PathTracingIntegrator.cpp:4046); `EvaluateKrayNM` [ISPF.h:206](../src/Library/Interfaces/ISPF.h:206) | Must implement the thin-film term identically in `ScatterNM`, `valueNM`, and `EvaluateKrayNM` (RGB/NM twin hazard — [audit-by-bug-pattern](skills/audit-by-bug-pattern.md)). |
| **Tabulated `(nm, value)` asset path already exists** | `PiecewiseLinearScalarPainter` [PiecewiseLinearScalarPainter.h](../src/Library/Painters/PiecewiseLinearScalarPainter.h); `scalar_painter { file … }` [AsciiSceneParser.cpp:1180](../src/Library/Parsers/AsciiSceneParser.cpp:1180) | refractiveindex.info n/k drops straight in (2-col, bare numeric, nm; split n and k files). |
| n, k, roughness are `IScalarPainter` (no JH uplift); reflectance/tint are `IPainter` | GGXBRDF/GGXSPF slot types | New film slots (`film_ior`, `film_extinction`, `film_thickness`) are `IScalarPainter`. Routing IOR through `IPainter` mangles it via JH uplift. |
| GGX has anisotropy (`alphax`/`alphay` + tangent rotation) | GGXBRDF.h | Guilloché cut-direction micro-roughness is already supported — no new aniso work. |
| **Kulla-Conty geometric tables baked at F=1** (Fresnel-agnostic) | `E_ss`/`E_avg` [MicrofacetEnergyLUT.h](../src/Library/Utilities/MicrofacetEnergyLUT.h) | No table regeneration. Only `ComputeFresnelAvg` (LUT.h:146) hardwires conductor Fresnel → needs a thin-film-aware hemispherical average for the multiscatter term. |
| Tests are standalone `tests/*.cpp`, auto-discovered (top-level glob) | `tests/README.md`; `make`/CMake globs | New test = **no build-file edits**. Subdir headers (`tests/thinfilm/*.h`) are glob-safe. Template: `HosekWilkieReferenceTest.cpp`. |
| PNG out from a test: `RISE_API_CreatePNGWriter(…, 8, eColorSpace_sRGB)` + `RISE_API_CreateDiskFileWriteBuffer` | [RISE_API.h](../src/Library/RISE_API.h) | No `RISE_MEDIA_PATH`, no extra link deps. |
| Color path: `ColorUtils::XYZFromNM` + `XYZtoRec709RGB` | [ColorUtils.cpp:313](../src/Library/Utilities/Color/ColorUtils.cpp:313), [ColorConversion.h:26](../src/Library/Utilities/Color/ColorConversion.h:26) | Reuse the renderer's CMFs + matrix so swatch ≡ render. **No samplable per-nm D65 SPD exists** → embed the standard CIE D65 table (the only piece we add). |
| Adding `src/Library` files touches FIVE build projects | CLAUDE.md "touch ALL five" | Phase 2 only (Phase 1 is `tests/`-only). |

---

## 4. The material model — what kind of material is it?

**It is not a new material type. It is the existing GGX microfacet conductor with a third
Fresnel mode.** Physically, heat-tint/anodize iridescence *is* the specular reflectance of an
air–oxide–metal stack, so it belongs exactly where the bare-conductor Fresnel already lives:
on the GGX specular lobe.

```
enum FresnelMode { eFresnelConductor, eFresnelSchlickF0, eFresnelThinFilmConductor };  // + the third
```

This is also the **industry-standard surfacing**: glTF calls it `KHR_materials_iridescence`
— a base metal plus an iridescence layer with `iridescenceIor` (film) + `iridescenceThickness`
(+ texture). Our slots map ~1:1, so the material later imports/exports glTF iridescence.

What it is **not**:
- **Not a clearcoat.** Clearcoat is a thick, *incoherent* dielectric layer (a separate lobe).
  Thin-film is a thin, *coherent* interface effect — modifying the conductor Fresnel is the
  faithful model.
- **Not a new painter.** `IridescentPainter` is untouched.

### Scene syntax (illustrative — final names settle in Phase 2)

```
scalar_painter { name Ti_n   file colors/thinfilm/substrates/Ti.n }
scalar_painter { name Ti_k   file colors/thinfilm/substrates/Ti.k }
scalar_painter { name TiO2_n file colors/thinfilm/oxides/TiO2.n }
scalar_painter { name TiO2_k file colors/thinfilm/oxides/TiO2.k }
scalar_painter { name oxide_thk file textures/dial_thickness.png scale 250 }  # 0..250 nm

ggx_material {
  name ti_heattint
  fresnel_mode    thinfilm          # NEW mode
  ior  Ti_n   extinction Ti_k       # substrate n,k — reuse existing conductor slots
  film_ior TiO2_n  film_extinction TiO2_k    # NEW film slots (IScalarPainter)
  film_thickness  oxide_thk         # NEW — spatially varying, nm (IScalarPainter)
  alphax 0.08  alphay 0.02          # anisotropy → guilloché cut direction (already supported)
}
```

Optional future slots (defaulted, not built now): `ambient_ior` (default 1.0; e.g. a sapphire
crystal over the dial), multi-layer stack.

---

## 5. Physics — TMM + Airy, cross-checked

Implement **both** the characteristic-matrix TMM (general, N-layer) and the closed-form Airy
single-film sum, and assert they agree. The two-implementation cross-check is the point: it
catches the classic p-polarization sign-convention bugs a single implementation can't.

Conventions: complex index `N = n + i·k`, `k ≥ 0` (absorbing); time convention so absorbing
media decay (`e^{−iωt}`; Born & Wolf / Macleod). Take the **forward-travelling** `cosθ` root —
`Re(N·cosθ) > 0`, tie-broken by `Im(N·cosθ) > 0` — which keeps `cosθ = +1` at normal incidence
even for an absorbing medium. **The cosθ-branch, the matrix off-diagonal sign, and the Airy
round-trip exponent below must be mutually consistent**: pairing a forward-root branch with the
opposite-convention `+i sinδ` matrix / `e^{−2iδ}` Airy factor produces a *growing* wave and
`R ≫ 1` for absorbing films — exactly what the TMM↔Airy cross-check (and §9's energy invariant)
is built to catch. (Verified by the P1-A reference; see `tests/thinfilm/TmmReference.h`.)

```
Media: 0 = ambient (air, N₀=1), 1..M = films, s = substrate (semi-infinite conductor).
Snell:        N₀ sinθ₀ = Nⱼ sinθⱼ ;  cosθⱼ = sqrt(1 − (N₀ sinθ₀/Nⱼ)²), forward root Re(Nⱼcosθⱼ)>0 (tie Im>0)
Admittance:   ηⱼ(s) = Nⱼ cosθⱼ ;   ηⱼ(p) = Nⱼ / cosθⱼ        (per-polarization)
Phase:        δⱼ = (2π/λ) Nⱼ dⱼ cosθⱼ                        (complex if film absorbs)

TMM (per pol):  Mⱼ = [[cosδⱼ, −i sinδⱼ/ηⱼ], [−i ηⱼ sinδⱼ, cosδⱼ]] ;  M = Π Mⱼ
                [B;C] = M·[1; η_s] ;  Y = C/B ;  r = (η₀ − Y)/(η₀ + Y) ;  R = |r|²
Airy (1 film):  r = (r₀₁ + r₁s e^{+2iδ₁}) / (1 + r₀₁ r₁s e^{+2iδ₁}) ;  R = |r|²
                with r_{ab} = (η_a − η_b)/(η_a + η_b)  (per-pol admittances)
Unpolarized:    R = ½(R_s + R_p)
```

TMM with M=1 reduces algebraically to the Airy result → they must match to ~machine epsilon.

**Reference anchors (independent, published / closed-form):**
- **Bare-substrate limit** (no film / d→0): R must equal
  `Optics::CalculateConductorReflectance(air, substrate)` — ties the new code to RISE's own Fresnel.
- **Quarter-wave AR (exact):** lossless `n₁` on lossless `n₂` in air, `d = λ₀/(4n₁)` at normal
  incidence → `R = ((n₀n₂ − n₁²)/(n₀n₂ + n₁²))²`, and `R = 0` when `n₁ = √(n₀n₂)`. Unambiguous.
- **Energy / physical sanity:** `R ∈ [0,1]` ∀(λ,θ,d); correct branch cuts for absorbing media.

Reference code uses `std::complex<double>` (clarity over speed; this is the oracle). Written
N-layer so graded/multi-oxide is a future special-case, not a rewrite.

---

## 6. Generality — substrate & film are data

The substrate is the bottom medium's complex index (read from `ior`/`extinction`); the film is
another data pair (`film_ior`/`film_extinction`). A new metal is **pure data**. Generality
requires *both* layers to vary, because each metal grows a different oxide:

| Substrate | Oxide (heat-tint) | Oxide character |
|---|---|---|
| Titanium | TiO₂ (rutile / anatase / amorphous) | n≈2.4–2.9; transparent in visible — canonical case |
| Steel (≈Fe) | Fe₃O₄ / Fe₂O₃ ("temper colors") | mixed; magnetite is **absorbing** (k≠0) → duller, browner ladder |
| Tantalum | Ta₂O₅ | n≈2.1–2.2, very transparent → vivid clean anodize colors |
| Niobium | Nb₂O₅ | n≈2.3–2.4, transparent → classic anodized-jewelry palette |

Steel's absorbing oxide exercises the complex-film-index path that TiO₂ (k≈0) barely touches —
good coverage. The Phase-1 validation swatch is therefore a **grid** (rows = metal, columns =
thickness), proving generality in one image and stress-testing transparent vs absorbing films.

### Data layout

```
colors/thinfilm/
  substrates/  Ti.n Ti.k   Steel.n Steel.k   Ta.n Ta.k   Nb.n Nb.k
  oxides/      TiO2.n TiO2.k  Fe3O4.n Fe3O4.k  Ta2O5.n Ta2O5.k  Nb2O5.n Nb2O5.k
  README.md    # provenance + citations, metal↔oxide pairings, caveats
```

---

## 7. Integration architecture (Phase 2)

**Math isolation.** Lift the validated Airy into `src/Library/Utilities/ThinFilm.h`: a small
self-contained evaluator — a stack struct (ambient / N films / substrate, complex index +
thickness) with `Reflectance(cosThetaI, nm)`. GGX's conductor branch constructs/queries it.
Keeps the math in one tested place, trivially N-layer / multi-substrate, and avoids a full
`IFresnelModel` virtual refactor (which we can wrap later if more Fresnel models appear).

**Spectral path — primary, exact.** In `ScatterNM` / `valueNM` / `EvaluateKrayNM`, evaluate
`ThinFilm::Reflectance(cosθ, nm)` directly per hero wavelength. No approximation. The only
residual is *variance* (fast λ-oscillation of R at thick films / grazing angles aliasing into
4 samples/path) — an HWSS-sample-count knob, not bias. **The RGB/NM twins must stay identical.**

**RGB path — convenience, LUT.** No wavelength on the RGB path; naive per-channel thin-film
looks wrong (3 point samples can't represent an interference *integral*). Bake a per-material
**2D LUT `(cosθ × thickness) → RGB`** by integrating `R(λ)` against RISE's CMF **albedo** basis
(white-normalized, illuminant-independent — see §8). **Box-filtering each LUT cell over its
thickness/angle span is the antialiasing.** The Phase-1 swatch integrator *is* this generator.

**Belcour-Barla 2017 assessment (the theory survey conclusion).** Their spectral antialiasing
exists to fold interference into an *RGB* renderer (they integrate in OPD space because they
can't carry wavelength). We can, per hero wavelength, so the **spectral path needs none of it**.
Their role reappears only on our RGB path, where we replace it with a simpler offline
pre-integration (the box-filtered LUT). Conclusion: spectral = direct Airy; RGB = LUT.

**Kulla-Conty (energy compensation).** Geometric `E_ss`/`E_avg` are F=1 → unchanged. Only
`ComputeFresnelAvg` (the hemispherical Fresnel average feeding the multiscatter term) hardwires
conductor Fresnel. Plan: **measure** the white-furnace energy error of reusing the bare-substrate
`F_avg` first; if material, add a thin-film-aware `F_avg(thickness)` 1D LUT; document either way.

**Importance sampling — unchanged.** VNDF samples the microfacet exactly as before; only the
per-sample Fresnel weight changes. Reciprocity + white-furnace are Phase-2 exit gates.

**ABI discipline.** `RISE_API_CreateGGXMaterial` grows parameters → follow
[abi-preserving-api-evolution](skills) (append/overload; don't break the existing signature).
Adding `ThinFilm.h` (+ any `.cpp`, + the LUT if a `.cpp`) touches the five build projects.

**Cook-Torrance:** reject `fresnel_mode thinfilm` on the legacy Cook-Torrance with a clear
parse diagnostic. `ThinFilm.h` makes adding real support a one-liner later if wanted.

---

## 8. Color pipeline notes (a real subtlety)

Two *different* spectral→color integrations, both reusing `ColorUtils::XYZFromNM`:

- **Phase-1 swatch (preview):** `XYZ = ∫ R(λ)·D65(λ)·cmf(λ)dλ / ∫ D65(λ)·ȳ(λ)dλ` → "what the
  surface looks like under a D65 viewer." This is what we compare to anodize charts (charts are
  photographed in daylight). Embed the CIE D65 SPD.
- **Phase-2 RGB-path reflectance:** integrate `R(λ)` against the CMF **albedo** basis,
  white-normalized so `R(λ)≡1 → RGB (1,1,1)` — illuminant-independent (the renderer multiplies
  this reflectance by incident RGB light). **Not** D65-weighted.

Conflating these is exactly the kind of color-space bug the codebase has been bitten by
(cf. the 2026-05 colour-space migration notes). The spectral path has no such ambiguity — it
returns `R(λ)` at the hero wavelength and the film's spectral→XYZ→RGB handles the rest.

---

## 9. Validation strategy

**Phase 1 (standalone, automated `Check()` asserts + human swatch):**
- Airy ≡ TMM across a λ×θ×d grid (tol ~1e-9).
- Bare-substrate limit d→0 ≡ `Optics::CalculateConductorReflectance(air, substrate)`.
- Quarter-wave AR exact closed form (incl. R=0 when n₁=√(n₀n₂)).
- Energy: R∈[0,1] ∀(λ,θ,d); branch-cut correctness for absorbing media.
- Anodize **hue sequence**: dominant-wavelength/hue-angle advances monotonically through first
  order; second-order chroma < first-order at matched hue (the robust optical signature).
- Human gate: the swatch **grid** PNG, eyeballed against cited charts (per metal).

**Phase 2 (in-renderer, several automatable):**
- In-renderer ladder ≡ Phase-1 swatch (spectral path) — direct oracle cross-check, ties P1↔P2.
- Sphere rim-vs-center hue ≡ TMM at matching angles.
- `film_thickness → 0` ≡ a `fresnel_mode conductor` render of the same metal.
- White furnace: no energy gain. Reciprocity holds.

**Decoupling rule:** thickness→color (optics, P1/P2) and heat→thickness (growth law, P3) stay
strictly separate, so a calibration mismatch never masquerades as an optics bug.

---

## 10. Canonical test scene — one scene, two zones

**`scenes/FeatureBased/Materials/thinfilm_heattint_showcase.RISEscene`** (canonical):
- **Validation zone:** flat plate / tile row with a 5→250 nm thickness gradient, near-normal,
  known illuminant. Rendered spectrally, its scanline matches the Phase-1 swatch within
  tolerance → the P1↔P2 cross-check.
- **Stress zone:** a row of spheres (Ti / steel / Ta / Nb, each with its oxide, same thickness)
  under a studio HDRI. Spheres give the full normal→grazing sweep (angle-dependent hue —
  the iridescence money-shot), substrate variety side-by-side, curvature + anisotropic roughness.

**`scenes/Tests/Materials/thinfilm_ladder.RISEscene`** (regression): flat gradient only, fixed
low spp, deterministic — what the image-compare gate consumes.

Primary integrator: `pathtracing_spectral_rasterizer` with HWSS (thin-film is spectral).
Per the integrator matrix, iridescent *caustics* are VCM regime — validate there if a caustic
scene is added later, but the lit-dial path is PT/auto.

---

## 11. File layout

**Phase 1 (standalone — zero build-project edits):**
```
tests/
  ThinFilmTMMTest.cpp          # standalone test, auto-discovered
  thinfilm/                    # header-only reference (included by the test; glob-safe subdir)
    TmmReference.h             #   N-layer characteristic-matrix TMM (std::complex, s/p)
    AiryReference.h            #   single-film Airy sum (cross-check; Phase-2-bound candidate)
    ThinFilmStack.h            #   stack description + air/film/substrate assembly
    OpticalConstants.h         #   load colors/thinfilm/*.{n,k}; embedded CIE D65; (metal,oxide) keyed
    AnodizeSwatch.h            #   thickness×metal sweep → XYZ → sRGB → PNG grid (RISE_API writers)
colors/thinfilm/ …             # data set (see §6)
docs/THIN_FILM_INTERFERENCE.md # this doc
```
Swatch output → CWD/`rendered/` (gitignored); inline-attached in this doc, not committed binary.

**Phase 2 (renderer — touches five build projects):**
```
src/Library/Utilities/ThinFilm.h        # lifted, RISE Scalar/complex, N-layer evaluator
src/Library/Utilities/ThinFilmRGBLUT.*  # per-material (cosθ×thickness)→RGB bake (if a .cpp)
src/Library/Materials/GGX{BRDF,SPF,Material}.*   # eFresnelThinFilmConductor + film_* slots
src/Library/Interfaces/IMaterial.h      # enum value
src/Library/Parsers/AsciiSceneParser.cpp; Job.cpp; RISE_API.{h,cpp}   # scene language + factory
tests/ThinFilmBRDFTest.cpp              # production evaluator ≡ Phase-1 oracle; furnace; reciprocity
scenes/{FeatureBased,Tests}/Materials/thinfilm_*.RISEscene
```

---

## 12. Phased plan — deliverables, tests, work breakdown

Each lettered item is a candidate worker unit. Dependencies noted.

### Phase 1 — Ground truth + theory (standalone, no integration)
- **P1-A** Reference math: N-layer TMM + single-film Airy, cross-checked. Header-only in
  `tests/thinfilm/`. *Independent.*
- **P1-B** Optical-constants data: 4 substrates + 4 oxides from refractiveindex.info → `colors/thinfilm/`
  with provenance README + caveats. *Independent.* (Needs web fetch.)
- **P1-C** Color/swatch pipeline + `ThinFilmTMMTest.cpp`: invariants (§9) + hue-sequence + swatch
  grid PNG. *Depends on P1-A, P1-B.*
- **P1-D** Theory writeup (Belcour assessment, RGB-LUT decision): folded into this doc.
- **Exit:** all P1 asserts green; swatch grid reproduces the anodize chart per metal.

### Phase 2 — Renderer integration
- **P2-A** `ThinFilm.h` production evaluator (lift P1-A, RISE conventions) + unit test vs oracle.
  Touches 5 build projects.
- **P2-B** GGX `eFresnelThinFilmConductor` + `film_*` slots; exact spectral path
  (`ScatterNM`/`valueNM`/`EvaluateKrayNM`); RGB 2D LUT (albedo basis, box-filtered);
  Cook-Torrance reject-with-diagnostic. *Depends on P2-A.*
- **P2-C** Scene language (`ggx_material` descriptor + `Job::AddGGXMaterial` + `RISE_API`, ABI
  discipline) + parser test. *Depends on P2-B.*
- **P2-D** Kulla-Conty: measure furnace error, then `F_avg(thickness)` LUT or documented accept;
  white-furnace + reciprocity tests. *Depends on P2-B.*
- **P2-E** Canonical showcase scene + deterministic regression scene + in-renderer-ladder ≡ oracle
  image-compare gate. *Depends on P2-B/C.*
- **Exit:** ladder ≡ oracle (spectral); rim/center ≡ TMM; thickness→0 ≡ conductor; furnace clean;
  reciprocity holds; clean warning-free build on `make` + Xcode RISE-GUI.

### Phase 3 — The dial
- **P3-A** Procedural guilloché heightfield/normal map (rose-engine "lightning" zigzag) +
  anisotropic micro-roughness aligned to cut direction.
- **P3-B** Oxide-thickness map: torch heat-diffusion (path splines) → parabolic oxide growth law
  → thickness field driving the `film_thickness` painter; geometry modulates heat (ridges hotter).
- **P3-C** Final dial under studio HDRI + turntable sequence; calibrate growth law to a real
  heat-tint chart (decoupled from optics per §9).
- **Exit:** turntable shows correct angle-dependent hue shift; dial reads as physically plausible.

---

## 13. Locked decisions (2026-06-07)

1. **RGB path = precomputed per-material 2D LUT** (cosθ×thickness, albedo basis, box-filtered).
   Spectral-path correctness is the priority; RGB is preview-grade. *(Confirmed by owner.)*
2. **Cook-Torrance: reject `fresnel_mode thinfilm` with a diagnostic** (not wired now). *(Confirmed.)*
3. **Single-film material now; N-layer kept in the math as a documented extension point**
   (no multi-layer scene syntax this round). *(Confirmed.)*

---

## 14. Open questions / future

- Multi-layer scene syntax (`thinfilm_stack` chunk) — when a use case appears.
- `ambient_ior` painter (sapphire-crystal-over-dial) — defaulted to 1.0 for now.
- glTF `KHR_materials_iridescence` import/export — design already aligns; wire later.
- ACES working space — the LUT bake follows whatever `RISEPel`/LUT target is current.

---

## 15. Data provenance & caveats (to be finalized by P1-B)

Intended refractiveindex.info sources (worker confirms exact entries + license, records in
`colors/thinfilm/README.md`):
- **Ti:** Rakić 1998 (LD) / Johnson-Christy / Werner 2009. **TiO₂:** Devore 1951 (rutile,
  ordinary ray); note anatase/amorphous alternatives (lower n) and rutile birefringence (use
  ordinary ray, document the approximation).
- **Steel:** optically ≈ Fe (Johnson-Christy / Querry) — document the steel≈Fe approximation.
  **Fe₃O₄/Fe₂O₃:** Querry 1985 etc.; magnetite **absorbs** (k≠0).
- **Ta:** Werner 2009 / Ordal. **Ta₂O₅:** Gao 2012 / Bright.
- **Nb:** Golovashkin / Weaver. **Nb₂O₅:** published thin-film data.

Caveats that shift thickness↔color (documented, **not** fudged in optics — absorbed by P3's
growth-law calibration): anatase vs rutile vs amorphous oxide index; rutile birefringence;
substrate microroughness desaturates real heat-tint (later captured by GGX roughness); steel's
mixed/absorbing oxides; RII µm→nm conversion; strip any header lines (loader is bare `fscanf`).

---

## 16. Process & governance (working agreement)

- **Master controller** (the orchestrating session) decomposes work into pieces and dispatches
  worker subagents.
- **Each worker** implements its piece and runs **multiple rounds of adversarial review**
  (see [adversarial-code-review](skills/adversarial-code-review.md)) before declaring completion,
  or escalates a blocker it cannot resolve back to the controller.
- **No subagent ever commits.** Only the master controller commits — after reviewing/integrating
  the worker's result — on a dedicated feature branch (never on `master`).
- **No one pushes except the human owner.** The controller and workers never push.

---

## 17. References

- Belcour & Barla 2017, "A Practical Extension to Microfacet Theory for the Modeling of Varying
  Iridescence" (SIGGRAPH).
- Macleod, "Thin-Film Optical Filters" (characteristic-matrix method).
- Born & Wolf, "Principles of Optics" (Airy summation, complex Fresnel).
- glTF `KHR_materials_iridescence` extension spec.
- refractiveindex.info (n,k data; cite exact datasets in the data README).
- RISE: [Optics.hpp](../src/Library/Utilities/Optics.hpp), [GGXBRDF.cpp](../src/Library/Materials/GGXBRDF.cpp),
  [PiecewiseLinearScalarPainter.h](../src/Library/Painters/PiecewiseLinearScalarPainter.h),
  [MicrofacetEnergyLUT.h](../src/Library/Utilities/MicrofacetEnergyLUT.h).
