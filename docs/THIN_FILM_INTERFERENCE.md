# Thin-Film Interference for Heat-Colored Metals — Design Doc

**Status:** Phase 3 COMPLETE (+ extensions) — the guilloché watch dial ships on `feature/thin-film-interference`: a Cartesian-grid relief mesh (kills the polar-singularity centre wash), scene-tunable heat-tint, dark-hero 2-softbox lighting, dispersive sapphire + data-based MgF₂ AR coating, a blue-leather-strap hero on a polished surface, **GUI-switchable base metal (Ti/Nb/Ta/steel — per-metal n,k + oxide + nm window + dose shape), torch-pattern + colour-palette oxide variants, and a native-timeline turntable+dolly animation (with new keyframeable camera azimuth/phi)**.  Phases 1+2 (optics + material) complete; full suite 130/130, `make` + Xcode-arm64 warning-free · **Created:** 2026-06-07 · **Updated:** 2026-06-09 · **Owner:** master-controller session
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

## Phase 1 outcome (2026-06-07) — COMPLETE

All three Phase-1 pieces landed on `feature/thin-film-interference` (workers implemented +
adversarially reviewed; controller verified + committed; nothing pushed):

- **P1-A reference** — `tests/thinfilm/{TmmReference,AiryReference,ThinFilmStack}.h` +
  `tests/ThinFilmTMMTest.cpp` (commit `2d8fbc89`). N-layer TMM + single-film Airy; **6086
  assertions, 0 failures**. Airy≡TMM to 1.3e-15; quarter-wave AR exact; bare-limit ≡
  `Optics::CalculateConductorReflectance`; energy R∈[0,1]; TIR/evanescent + Hecht closed form.
  This cross-check **caught the §5 sign-convention bug** (fixed in `953c4eb7`); §5 above is now
  the validated convention.
- **P1-B data** — `colors/thinfilm/` (commit `42a4b516`). Real, cited n,k for Ti/Steel/Ta/Nb +
  TiO₂/Fe₃O₄/Ta₂O₅/Nb₂O₅; provenance + 9 caveats in the README.
- **P1-C swatch + gate** — `tests/thinfilm/{OpticalConstants,AnodizeSwatch}.h` +
  `tests/ThinFilmAnodizeSwatchTest.cpp` (commit `92ace6fa`). **24 assertions, 0 failures**,
  warning-free. Ti ladder reproduces the anodization sequence; D65 white point spot-on
  (X=0.9504 Y=1.0 Z=1.0885); Airy≡TMM and bare≡Optics re-confirmed in-gate; swatch grid
  (`rendered/thinfilm_anodize_swatches.png`) shows Ti/Ta/Nb vivid, steel dull/brown.
  Adversarial review fixed a `-ffast-math`-defeated finiteness check, a tautological
  desaturation assertion, and a Windows-build-breaking POSIX `mkdir`/`realpath`.

**Validated for Phase 2:** the spectral path is exact (per-hero-wavelength Airy — no Belcour
spectral AA needed); the swatch integrator is the RGB-LUT generator; the §5 convention (and
P1-A's `AiryReference.h`) is what lifts into `src/Library/Utilities/ThinFilm.h`.

## Phase 2 outcome (2026-06-07) — COMPLETE

Full renderer integration landed on `feature/thin-film-interference` (controller-committed after
independent verification; workers adversarially reviewed; nothing pushed):

- **P2-A** `src/Library/Utilities/ThinFilm.h` (`b75f69fd`) — production Airy evaluator
  (`ReflectanceConductor{,RGB}`, N-layer-capable), registered in the header-tracking build projects
  (VS + Xcode; `plutil` clean). Production ≡ Phase-1 oracle to 3.3e-16 (3271 asserts). A grazing
  `cosθ=0` NaN was caught + fixed (input clamp).
- **P2-B** GGX `eFresnelThinFilmConductor` + `film_*` slots (`fab1d8e8`) — **exact** spectral path
  (`ScatterNM`/`valueNM`; HWSS companions route through `valueNM`); RGB albedo-basis integral
  (von-Kries E→D65; the 2D LUT was **deferred** — §13.1). Conductor/Schlick paths byte-identical
  (3.1e-16). Caught + fixed an illuminant-tinted RGB basis.
- **P2-C** scene language + ABI (`de7337e8`) — `fresnel_mode thinfilm` + `film_ior/film_extinction/
  film_thickness`; new `RISE_API_CreateGGXMaterialThinFilm` overload (old symbol preserved).
  Diagnostics: thinfilm-needs-film-params; Cook-Torrance rejects thinfilm.
- **P2-D** thin-film Kulla-Conty `F_avg` (`7371e2d7`) — `ThinFilm::FresnelAvgConductor{,RGB}` (shared
  21-pt GL rule) routed into the four GGX multiscatter sites. **Measured** the substrate average was
  off by up to 0.50 / ~13% rough-surface albedo (`tests/ThinFilmFurnaceTest.cpp`) → implemented, not
  accepted. Energy conserved; the change invalidated 2 P2-B assertions, repurposed honestly (one
  adversarially proven to catch a substrate-`F_avg` regression at 88%).
- **P2-E** canonical scenes (`e3451b18`) — `scenes/Tests/Materials/thinfilm_ladder.RISEscene` (Ti
  ladder) + `scenes/FeatureBased/Materials/thinfilm_heattint_showcase.RISEscene` (Ti ladder +
  Ti/Steel/Ta/Nb row). Spectral renders confirm iridescence (angle-dependent hue) + the absorbing-
  steel-vs-vivid-Ti/Ta/Nb generality predicted by the Phase-1 swatch grid.

**State:** the material ships as a GGX `fresnel_mode thinfilm` (= glTF `KHR_materials_iridescence`),
substrate/oxide data-driven. Spectral exact; energy-conserved; ABI-preserving. All thin-film + GGX
suites green, warning-free `make`. **Deferred (documented):** RGB 2D LUT (per-shade integral ships,
§13.1); automated in-renderer image-compare gate (qualitative render + 1e-16 unit tests stand in);
clean Xcode-GUI rebuild check; Cook-Torrance thin-film. **Next — Phase 3:** the guilloché dial,
driving `film_thickness` with a spatially-varying oxide map (the spatial hook this material was built
around) + a rose-engine normal map with anisotropic cut-aligned roughness.

## Adversarial review outcome (2026-06-07)

Full multi-reviewer adversarial CORRECTNESS review of the whole branch before merge (per
[adversarial-code-review](skills/adversarial-code-review.md)): 3 rounds, 9 reviewers across
orthogonal axes (optics math, BSDF/spectral integration, API/ABI/lifecycle, blast-radius/tests/data,
thread-safety, post-fix re-review, "what's left").

- **P1 — fixed (`59dd62b8`).** The GGX BSDF dereferenced `pFilmExtinction` unconditionally at 9
  sites, so a documented-supported scene (omit the *optional* `film_extinction`) segfaulted on the
  first shade. Guarded null→k=0 (the documented transparent default) at every site; Test F added.
- **P2 — fixed (review commit).** The sibling (audit-by-bug-pattern): `RISE_API_CreateGGXMaterialThinFilm`
  / its emissive twin (the direct-API path) didn't reject thinfilm + null `film_ior`/`film_thickness`
  (which have no sensible default and are dereferenced unconditionally) → the same crash class. Both
  ThinFilm factories now reject the invalid combination; Test G added.
- **P2 — fixed (`48d99f04`).** `MaterialIntrospection` now surfaces the `film_*` slots (`film_thickness`
  live-editable) + a read-only `fresnel_mode` row, via `GGXMaterial`/`GGXBRDF`/`GGXSPF` film
  getters/setters; `tests/ThinFilmIntrospectionTest.cpp` (29 asserts). The GUI/Blender property panels
  can now view + edit a thin-film material's defining parameters.
- **Cleared with no findings:** the §5 optics convention + numerics (incl. the principled-fix audit
  of the grazing-`cosθ` clamp — it has a derivable bound), spectral `ScatterNM`≡`valueNM` twins +
  HWSS companions + BDPT/VCM/MLT safety, energy conservation, reference-counting of the new slots,
  ABI preservation, thread-safety/reentrancy (the RGB integral uses load-init CMF tables, not a lazy
  singleton), MIS lobe classification (`isDelta=false`, glossy), parser diagnostics, n/k data, and
  the 5-build-project registration.

**Round 3** (post-fix guard re-review + a final "what's left" sweep covering HWSS companion-termination,
`ComputeFms` numerics as `F_avg→1`, the `albedo()` AOV cosine, and path-guiding/auto-rasterizer
interaction) returned **no new P1/P2** — the stop rule (every finding fixed or rejected-with-reason,
plus a post-fix round with no new P1/P2) is satisfied. The two real bugs (a P1 crash + its P2 API
sibling) were fixed, and the deferred GUI-introspection P2 was subsequently implemented (`48d99f04`),
so ALL review findings are resolved. The thin-film branch is correctness-clean AND editor-complete.

## Phase 3 outcome (2026-06-08) — guilloché watch dial COMPLETE

The guilloché thin-film watch landed on `feature/thin-film-interference`
(`scenes/FeatureBased/GuillocheWatch/watch_dial.RISEscene`; commits `b2dbbb0b` (AR feature) +
`72e85c6f` (watch scene + tools); nothing pushed). The implementation **diverged from the §12
P3-A/B/C plan** in one load-bearing way — the Cartesian rebuild below.

**Phase 3 extensions (later sessions; all in `scenes/FeatureBased/GuillocheWatch/`, see its README + AGENTS).**
- **Switchable base metal** Ti/Nb/Ta/steel — each `tf_dial_<metal>` carries the metal's substrate n,k + its oxide (TiO₂/Nb₂O₅/Ta₂O₅/Fe₃O₄) + a per-metal oxide-thickness nm window (Airy/CIE oracle, `ThinFilmSwatchOracle(substrate, oxide)`) + a per-metal radial dose SHAPE (each metal's parabolic-oxidation activation energy, `thermal_oxide_sim.METAL_KINETICS`).  Switch via the dial object's `material`.
- **Torch-pattern + colour-palette oxide variants** — uniform / lightning-zigzag (`apply_torch_pattern` + petal `lightning_mask`) and temper-window palettes (warm/vivid/cool/wide); switch via the material's `film_thickness` scalar-painter reference.
- **Native-timeline animation** — 45° turntable + subtle dolly on cam_high34 (`timeline` keyframes); added keyframeable camera `azimuth`/`phi` (+`theta`/`elevation`) orbit params to `CameraCommon`.
- **Brand references scrubbed** — the design is original.

- **P3-A/B generators.** `scenes/FeatureBased/GuillocheWatch/thermal_oxide_sim.py` (torch heat → Arrhenius/parabolic
  oxide-growth → radial thickness profile, gold centre → blue rim, calibrated against the
  Phase-1 Ti/TiO₂ swatch oracle). `scenes/FeatureBased/GuillocheWatch/guilloche_gen.py` (polar rose-engine height/normal/angle
  maps) — **superseded for the dial** by `scenes/FeatureBased/GuillocheWatch/dial_mesh_gen.py`, kept as a general polar generator.

- **The Cartesian rebuild — the load-bearing lesson.** The §12 plan assumed a guilloché
  normal/height map on a `circulardisk` (polar UV: u=angle, v=radius). That **fails structurally**:
  a guilloché micro-cell spans a fixed slice of ANGLE, so its world width = r·Δangle → collapses to
  ZERO at the centre. Tessellated + displaced, the inner ~40% of the dial loses all relief → a
  smooth gold blob **no resolution can fix** (diagnosed with 5 named diagnostic cameras: the blob
  sat at a FIXED dial-radius in every view ⇒ geometry, not a specular wash). Fix:
  `scenes/FeatureBased/GuillocheWatch/dial_mesh_gen.py` bakes the relief into a **Cartesian-grid circular triangle mesh** (RAW2:
  position + analytic normal + linear UV) — every cell is a fixed world size at the rim and the
  dead-centre alike. The woven micro-grid is laid in a per-sector-rotated frame so it runs radially
  and flips tilt at each jagged "lightning" seam but never shrinks; the petals are the swirl=0
  radial organizer. **2026-06 update:** the bake is now NATIVE — `guilloche_dial_geometry` chunks
  evaluate the same field in C++ at scene-parse time (`src/Library/Painters/GuillocheField.h`;
  golden-tested against the Python in `tests/GuillocheFieldTest.cpp` + `tests/ProceduralMeshTest.cpp`),
  so there is no mesh file at all; `dial_mesh_gen.py` remains as the reference implementation.

- **Heat-tint is scene-tunable, no re-bake** (the §9 decoupling, realised — and since 2026-06 fully
  native). The normalised radial heat SHAPE (0=centre..1=rim) is a `guilloche_oxide_painter`
  IFunction2D (`oxide_fn`; was `oxide_cart.png`); the `oxide_thk` scalar_painter consumes it via the
  `function2d` form, whose `scale`/`bias` set the absolute torch start/end nm (bias=centre,
  bias+scale=rim) — presets in the scene's `>>> HEAT-TINT TORCH CONTROL <<<` block. The painter
  emits a dose, the renderer does thickness→colour, the scene dials the absolute nm.

- **Dark-hero lighting.** Two top/bottom softboxes with a dark gap (soft gradient reflection bands,
  not one harsh panel; rakes the relief from two sides) + a dim Uffizi HDRI fill (the even thin-film
  COLOUR shift per facet is what renders the texture). `scenes/FeatureBased/GuillocheWatch/render_watch_views.py` (5 named
  cameras + torch/AR sweep overrides); `scenes/FeatureBased/GuillocheWatch/render_watch_turntable.py` (+Z-orbit dark-hero GIF →
  `rendered/turntable.gif`).

- **Crystal — dispersive sapphire + a NEW data-based AR coating.** The dome is a
  `dielectric_material` with the Malitson 1962 ordinary-ray sapphire Sellmeier (n(589)=1.768, Abbe
  72.3, verified to <0.001 vs the Fraunhofer lines). Real watch crystals are AR-coated, but RISE's
  dielectric had no thin-film slot (GGX thin-film is reflection-only), so it reflected bare
  7.7%/surface. **Added a data-based AR path to the renderer core** (commit `b2dbbb0b`):
  `DielectricSPF::GenerateScatteredRay` now computes the surface Fresnel via the SAME
  `ThinFilm::ReflectanceConductor` Airy evaluator (air / AR-film / substrate stack) when
  `ar_film_ior`/`ar_film_thickness`/`ar_film_extinction` are set — BOTH the spectral `ScatterNM`
  (per-hero-λ) and RGB `Scatter` (forces per-channel at representative λ {611,549,465}) paths;
  default 0 = bit-identical bare dielectric. MgF₂ quarter-wave at 550 nm (≈99.6 nm) →
  **7.70% → 0.53% mean visible (~14×)** with the characteristic AR purple bloom. **Key physics:** the
  wavelength dependence is from the phase δ=2πnd/λ, so a CONSTANT film index already gives the
  correct AR spectrum (scalar params, no dispersive n needed). Threaded through
  `RISE_API_CreateDielectricMaterial` + `Job::AddDielectricMaterial` (3 defaulted scalar params,
  back-compat; no new `src/Library` files → the five build projects are untouched). The single
  `ref =` site in `GenerateScatteredRay` is the hook for any future coated-dielectric work.

- **Verification.** `tests/DielectricARTest.cpp` — **18 assertions, 0 failures** (the AR reflectance
  data + the SPF integration: reflection-weight == AR reflectance with the coating on, == bare
  air/sapphire Fresnel off, strictly dimmer). **Full suite 130/130, 0 failures** (zero regression
  from the renderer-core AR change). `make` clean (0 warnings). **Xcode RISE-GUI (arm64) BUILD
  SUCCEEDED, 0 source warnings** — this **closes the "clean Xcode-GUI rebuild check"** deferred since
  Phase 2. (The default-config Xcode build fails to *link* on an x86_64/arm64 homebrew-lib mismatch —
  a pre-existing environment quirk, not the code; `-arch arm64` is the working invocation.)

**Deferred-list update:** "clean Xcode-GUI rebuild check" → **DONE** (this phase). Still deferred:
RGB 2D LUT (§13.1); automated in-renderer image-compare gate (qualitative renders + the 18-assert AR
unit test + 1e-16 thin-film tests stand in); Cook-Torrance thin-film.

## 13. Locked decisions (2026-06-07)

1. **RGB path = albedo-basis spectral integral** (white-normalized so R≡1 → neutral). **Shipped in P2-B as a per-shade 32-λ integral (von-Kries E→D65), NOT the originally-specified 2D LUT** — the LUT needs spatially-constant stack indices, but they arrive as possibly-varying `IScalarPainter`s with no construction-time hit point (a correctness hazard); the per-shade integral is always correct, the constant-stack LUT deferred as an optimization.
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
