# Hair / Fur System Design — Scattering Model, Strand Geometry, and Phased Plan

**Status:** PHASE 1 IMPLEMENTED (2026-08-27) — §7 Phase 1 complete through render validation;
Phases 2-4 remain proposed.
**Date:** 2026-08-25 (design); Phase 1 landed 2026-08-26/27.
**Nature:** Decision document + phased execution plan, in the mold of
[UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md) (survey → scored candidates →
recommendation) and [SMS_UNIFORM_SEEDING_PLAN.md](SMS_UNIFORM_SEEDING_PLAN.md) (gated phases).
**Inputs:** [MATERIALS.md](MATERIALS.md) (material contract + §9 new-BSDF checklist),
[ARCHITECTURE.md](ARCHITECTURE.md) (BVH/TLAS, scene immutability, `Realize()` hook),
[ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md) (scalar-vs-color pipe discipline),
[RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) (integrator routing map), [OIDN.md](OIDN.md)
(AOV contract), plus the external literature in §10.

**Phase 1 slice history (2026-08-26/27), oldest to newest:**
- `7dfe12e5` — Slice A: hair BSDF core (Chiang 2016 lobes, three colour tiers).
- `ce22a7a2` — Slice B: `hair_material` registration end-to-end (API/Job/parser).
- `d579bcfa` — Slice C1: `HairGeometry` curve primitive (strand storage, segment BVH, ribbon
  intersection).
- `27b33aed` — Slice C2: geometry-supplied fibre tangent honoured in `Object`/`CSGObject` ONB.
- `bddb2307` — Slice D: painter-driven groom generator + `hair_geometry` chunk.
- `fb092712` — Slice D review-round fixes (UV-derived comb frame, stream keying, suite gaps).
- Slice E (this closeout) — render-level regression scenes (`scenes/Tests/Hair/`),
  `tests/HairRenderTest.cpp`, and this docs closeout.

**Standing residuals carried forward (not blocking Phase 1, tracked for Phase 2+):**
- NEE cannot reach hair's transmissive hemisphere (`LightSampler` rejects
  `dot(wToLight, vNormal) <= 0`; TT-lobe energy from a delta/point light is only reachable via
  BSDF sampling) — §4/§6.2, `HairBSDF.h` section 5.
- `NormalMap`/`BumpMap` modifiers unconditionally rebuild the ONB from the perturbed normal alone,
  discarding any incoming fibre tangent — hair is incompatible with `bump_map`/`normal_map` until
  those two modifiers are fixed the way `GlintModifier` already is (§4.1).
- Windows (VS2022), Android (NDK/Gradle), and Xcode build-project files were updated with every
  new source file across all slices but have not been compiled this arc — only the Linux/macOS
  `make` build and its test suite have been gated.

---

## 1. The question, and the answer

**Question.** RISE has no hair or fur capability at any layer: no fiber scattering model
(`src/Library/Materials/` has no hair BSDF; the Blender exporter has zero `Hair` references), no
strand/curve primitive (`src/Library/Geometry/` has analytic quadrics, patches, meshes, and SDFs —
nothing curve-shaped), and no grooming surface. What scattering model, what geometry
representation, and what authoring surface should a hair/fur system use, and in what order should
it be built?

**Answer (recommendation, argued in §2–§6):**

1. **Scattering:** the **Chiang et al. 2016 near-field hair BCSDF** (the Disney production model;
   PBRT-v4's `HairBxDF` is the reference implementation), implemented as a standard RISE
   material pair — `HairBRDF` (`IBSDF`) + `HairSPF` (`ISPF`) — with **spectral absorption evaluated
   per-wavelength in the NM path** and a three-tier color parameterization: melanin
   concentrations (physical), direct σ_a, or artist color inverted through Chiang's
   reflectance-to-σ_a fit. Marschner 2003 alone is rejected (energy loss, no exact importance
   sampling); d'Eon 2011 far-field is rejected as the primary model (azimuthal quadrature cost)
   but its longitudinal M_p term is *adopted* (it is a component of Chiang's model); Yan
   2015/2017 medulla lobes are **deferred to Phase 3** as an additive extension for animal fur.
2. **Geometry:** a **new runtime curve primitive `hair_geometry`** — flat-ribbon/cylinder-normal
   swept curve segments over shared control-point arrays, with an **embedded `BVH<>` over
   segments** (the same template `TriangleMeshGeometryIndexed` and the TLAS already use), and
   PBRT-`Curve`-style recursive-split intersection. Strand *generation* (grow-on-surface with
   painter-driven grooming maps, guide-strand interpolation) happens in the geometry's
   `Realize()` hook, following the `DisplacedGeometry` build-time-painter precedent. Baking
   strands to triangle tubes, `path_instances_geometry`, per-strand `standard_object`
   instancing, and SDF composition are all rejected at target scale (§5.2, with the memory
   arithmetic).
3. **Integrators:** **PT is the target and the correct default** — hair transport is the
   many-forward-bounce glossy regime where PT already wins the σ²·T matrix, and the
   `auto_rasterizer` Tier-1 fall-through (`else → PT`) routes hair scenes correctly with no new
   clause. The BSDF is nonetheless implemented as a genuine evaluable-anywhere `IBSDF` so BDPT/VCM
   connections work through `PathVertexEval.h` unchanged (with a documented reciprocity caveat,
   §6.2). **Dual scattering is an explicit non-goal** (§6.4).
4. **Phasing:** Phase 0 harness → Phase 1 ships visible value (Chiang BSDF + `hair_geometry` +
   basic painter grooming) → Phase 2 grooming depth + import/Blender → Phase 3 fur medulla →
   Phase 4 LOD/perf. §7 carries the per-phase file-touch accounting, including the five
   build-project surfaces per new source file.

---

## 2. Scattering models — survey

Notation: fibers are rough dielectric cylinders. A path through the fiber is classified by the
number of internal interface events *p*: **R** (*p*=0, surface reflection — the white primary
highlight), **TT** (*p*=1, two transmissions — the bright halo when backlit), **TRT** (*p*=2, one
internal reflection — the colored, shifted secondary highlight and glints). The BCSDF factors per
lobe into a longitudinal term M_p(θ) and an azimuthal term N_p(φ), with absorption along the
internal path segments supplying the color.

### 2.1 Marschner et al. 2003

The founding model: smooth dielectric cylinder + tilted cuticle scales (tilt α ≈ 3°) which offset
the three lobes' longitudinal peaks (−α, +α/2, +3α/2) so R and TRT visibly separate. M_p is a
Gaussian in the half-angle; N_p comes from 2D circular-cylinder ray optics (far-field: integrated
across the fiber width).

**Shortfalls, all repaired by successors:** (a) *not energy conserving* — the Gaussian M_p is not
normalized on the sphere, and N_p diverges near the TRT caustic (the paper smooths it ad hoc);
(b) *energy loss* — lobes p ≥ 3 are simply dropped, which matters exactly where multiple
scattering matters (light hair); (c) *no importance sampling* in the paper (bolted on later by
Ou et al. 2012 and Hery & Ramamoorthi 2012, but pdf/eval mismatch makes those variance-prone).
Verdict: historically essential, not a candidate for implementation.

### 2.2 d'Eon et al. 2011 (+ 2013 sampling)

Repairs energy conservation: M_p becomes a normalized longitudinal scattering function (the
"d'Eon M_p", built on the modified Bessel function I₀; numerically delicate at low roughness —
must be evaluated in log space, as PBRT's `Mp`/`LogI0` does), and N_p is computed by *numerical
quadrature across the fiber width* with a Gaussian detector, summing lobes to arbitrary order.
The 2013 follow-up adds per-lobe importance sampling. Passes the white furnace test.

**Shortfall:** the azimuthal quadrature makes every BSDF evaluation a small numerical
integration — the far-field model is the expensive way to get what Chiang's near-field
formulation gets in closed form. Verdict: adopt M_p (inside Chiang); reject the far-field
azimuthal machinery.

### 2.3 Chiang et al. 2016 — the recommendation

The Disney production model (Zootopia onward), and the model PBRT-v4 ships as `HairBxDF`; also
the basis of Blender Cycles' Principled Hair. Key ideas:

- **Near-field:** the BSDF is conditioned on the exact offset *h* ∈ [−1, 1] at which the ray
  crosses the fiber's width — available for free from a ribbon intersection's across-width
  coordinate (*h* = 2v − 1 in PBRT's `Curve` shape). At fixed *h*, each lobe's azimuthal exit
  direction Φ(p, h) is *deterministic* from Snell's law, so no quadrature is needed; azimuthal
  roughness is modeled by wrapping a **trimmed logistic distribution** around Φ(p, h) — chosen
  precisely because it is analytically normalizable *and* analytically invertible, giving exact
  importance sampling.
- **Longitudinal:** d'Eon's energy-conserving M_p, with per-lobe roughness (β, β/2, β·3/2… per
  the cuticle-tilt convention) and shifted means from cuticle tilt α.
- **Energy closure:** lobes p = 0..3 plus a **residual lobe** that lumps all p > 3 energy, so the
  model passes the white furnace test when σ_a = 0 at any roughness.
- **Sampling:** choose p by the discrete apparent-attenuation energies A_p(h) at the *actual* h,
  then sample M_p exactly (d'Eon 2013) and the trimmed logistic exactly. The sampling pdf equals
  the evaluation pdf term-for-term — `Sample_f`/`PDF`/`f` are mutually consistent by
  construction, which is exactly the property RISE's `SPFBSDFConsistencyTest` /
  `SPFPdfConsistencyTest` harness enforces ([MATERIALS.md](MATERIALS.md) §"Consistency").
- **Artist controls:** independent longitudinal roughness β_m and azimuthal roughness β_n (each
  remapped to the internal logistic scale by an empirical fit), cuticle tilt α, and a
  color-to-σ_a inversion (§3.2).

**Why this one:** it is the only surveyed model that is simultaneously energy-conserving, exactly
importance-sampled, quadrature-free per evaluation, and battle-tested in production — and it has
a first-class, license-compatible reference implementation (PBRT-v4, Apache-2.0) to validate
against term by term.

### 2.4 Yan et al. 2015 / 2017 — fur and the medulla

Animal fur differs from human hair structurally: fur fibers contain a large scattering **medulla**
(a cylindrical core of radius ratio κ up to ~0.9 in e.g. rabbit; small or absent in human hair).
Light that crosses the medulla is diffused, which is what gives fur its soft, saturated,
diffusive look — a Chiang-only fur render reads as "thin shiny hair" in close-up.

- **Yan 2015** models a double cylinder (absorbing cortex annulus + scattering medulla with
  anisotropic phase function) and expands the lobe alphabet with scattered variants; the full
  model has ~11 lobes driven by precomputed 2D medulla scattering profiles.
- **Yan 2017** is the practical near-field version: 5 lobes — R, TT, TRT plus **TTˢ and TRTˢ**
  (paths that scattered in the medulla) — with compressed precomputed medulla profiles, unified
  with the Chiang-style *h* parameterization, at roughly 2–3× Chiang's evaluation cost. The
  paper also ships measured parameter fits for 9 fur types.

**Placement:** Yan 2017 is *additive* over a Chiang implementation (same factorization, same
near-field h, two extra lobes + a tabulated profile). That is exactly the shape of a follow-on
phase, not a reason to complicate Phase 1 — hence **Phase 3** (§7). Many productions ship fur on
Chiang alone with tuned parameters; the medulla is the close-up-accuracy increment.

### 2.5 Candidate matrix

| Criterion | Marschner 03 | d'Eon 11/13 (far-field) | **Chiang 16 (near-field)** | Yan 17 (fur) |
|---|---|---|---|---|
| Energy conservation | No (lossy + caustic blowup) | Yes | **Yes (residual lobe)** | Yes |
| Exact importance sampling | No (retrofitted, mismatched) | Per-lobe, good | **Exact, pdf ≡ eval** | Good (tabulated lobes approximate) |
| Eval cost | Low | High (azimuthal quadrature) | **Low (closed-form)** | ~2–3× Chiang |
| Needs h (near-field) | No | No | **Yes** (from ribbon v-coord) | Yes |
| Medulla (fur realism) | No | No | No | **Yes** |
| Artist controls | Poor | Poor | **β_m/β_n/α + color inversion** | Chiang's + medulla κ, g, σ_m |
| Reference impl., license | — | (inside PBRT's Mp) | **PBRT-v4 `HairBxDF`, Apache-2.0** | Author release (research code) |
| Production track record | Legacy | Weta (era) | **Disney/PBRT/Cycles — the standard** | Close-up fur |

### 2.6 Multiple scattering

Blond/white hair gets most of its visible color from **tens of bounces** of forward scattering —
a single-scatter BCSDF alone renders light hair nearly black. Two strategies exist:

- **Brute-force path tracing with high bounce budgets.** The modern answer, and RISE's answer:
  hair lobes are strongly forward-scattering with near-unity throughput, so paths stay
  significant for 30–60 bounces in light hair. This is purely a matter of scene/rasterizer
  settings (`rr_min_depth`, `rr_threshold`, per-class bounce caps — §6.1), not new transport
  code. PBRT's hair furnace scenes use depth limits of this order.
- **Dual scattering (Zinke et al. 2008).** An approximation of multiple scattering as a
  forward-scattering transmittance along the shadow path plus a local backscatter term. It was
  the production answer in the rasterization/Reyes era and lives on in real-time engines — but
  in a path tracer it *approximates the very thing the integrator already computes*, requires
  new strand-density occlusion infrastructure (deep shadow maps or a voxelized density field),
  and introduces bias with no consistency guarantee. **Rejected as a non-goal** (§8); if a fast
  preview mode is ever wanted, it belongs in a hypothetical interactive path, not in the
  production integrators.

---

## 3. Spectral integration — RISE-specific design

RISE is a spectral renderer with the RGB/NM twin-method convention and hero-wavelength spectral
sampling. Three facts from the material architecture bind the design (all verified in source):

1. **One wavelength per scatter call.** `ISPF::ScatterNM` receives a single `nm`
   (`src/Library/Interfaces/ISPF.h:141-147`); under HWSS the integrator calls `ScatterNM` **once
   at the hero wavelength** (`PathTracingIntegrator.cpp:4615`) and obtains the chosen lobe's
   throughput at the 3 companion wavelengths via `ISPF::EvaluateKrayNM`
   (`PathTracingIntegrator.cpp:4707`; contract at `ISPF.h:206-215`), falling back to full BSDF
   re-evaluation when a material doesn't implement it.
   **Consequence:** `HairSPF` MUST override `EvaluateKrayNM`. Given the sampled lobe p, the
   offset h, and the directions, the companion-wavelength throughput is a cheap closed-form
   re-evaluation of Fresnel + the absorption term T(σ_a(λ), h) — melanin absorption varies
   strongly across the visible band, so the fallback path would be both slow and the single
   biggest spectral-noise lever in hair renders.
2. **Scalar-vs-color pipe discipline.** Physical scalars must be `IScalarPainter` (never
   JH-uplifted); colors must be `IPainter` ([ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md)).
   The hair parameter mapping is unambiguous (§3.1).
3. **In-tree melanin precedent.** `BioSpecSkinSPF` already implements exactly the pattern hair
   needs: `IScalarPainter` concentration slots (`pnt_concentration_eumelanin` /
   `pnt_concentration_pheomelanin`, `BioSpecSkinSPF.h:73-74`) driving tabulated spectral
   extinction curves loaded as `IFunction1D` (`BioSpecSkinData.h:60-113`, OMLC spectroscopy
   tables), combined as `exp(−ε(λ)·c·d)` (`ComputeMelaninTransmittance`,
   `BioSpecSkinSPF.h:230-258`). `HomogeneousMedium` provides the second precedent: optional true
   spectral σ_a(λ)/σ_s(λ) curves as `IFunction1D` overriding an RGB fallback
   (`HomogeneousMedium.h:83-84,112-119`).

### 3.1 Parameter → pipe mapping

| Parameter | Type | Pipe | Rationale |
|---|---|---|---|
| `eumelanin` / `pheomelanin` (concentration) | scalar ≥ 0 | `IScalarPainter` | physical scalar; drives σ_a(λ) via in-tree OMLC `IFunction1D` curves (BioSpec precedent) |
| `sigma_a` (direct absorption override) | scalar / spectral | `IScalarPainter` | physical coefficient; per-λ via `GetValueAtNM` |
| `color` (artist reflectance) | color | `IPainter`, `SpectrumKind::Albedo` | a genuine reflectance color — JH uplift is *correct* here; inverted per-λ (§3.2) |
| `beta_m` (longitudinal roughness) | scalar (0,1] | `IScalarPainter` | physical scalar |
| `beta_n` (azimuthal roughness) | scalar (0,1] | `IScalarPainter` | physical scalar |
| `alpha` (cuticle tilt, degrees) | scalar | `IScalarPainter` | physical scalar |
| `ior` | scalar (default 1.55) | `IScalarPainter` | physical scalar; `GetValueAtNM` gives dispersion for free, matching `DielectricSPF`'s pattern (`DielectricSPF.h:159`) |

Exactly one of {melanin pair, `sigma_a`, `color`} is the active color source; the parser
descriptor enforces mutual exclusion with a diagnostic (the descriptor-driven parser makes this a
`Describe()`-level rule).

### 3.2 The three color tiers, spectrally

- **Tier 1 — melanin (recommended default for realism):**
  σ_a(λ) = c_eu · ε_eu(λ) + c_ph · ε_ph(λ), with ε(λ) from the in-tree OMLC tables
  (`BioSpecSkinData.h`) re-exposed as shared `IFunction1D`s (do **not** duplicate the tables).
  This is naturally spectral, needs no uplift, and reproduces the full human range
  (black → brown → blond via c_eu; redheads via c_ph). The literature's analytic alternatives
  (power-law fits ε_eu ∝ λ^−3.33, ε_ph ∝ λ^−4.75, d'Eon 2011 after Donner) are a fallback if
  the OMLC tables prove awkward to share; note PBRT's RGB constants (eumelanin
  (0.419, 0.697, 1.37), pheomelanin (0.187, 0.4, 1.05) per unit concentration) are the
  RGB-projected versions of the same data — useful for cross-validation against PBRT renders.
- **Tier 2 — direct σ_a:** power users and measured data.
- **Tier 3 — artist color:** Chiang's inversion, generalized per-wavelength. PBRT's
  `SigmaAFromReflectance` maps a desired multiple-scattering-averaged reflectance C to
  σ_a = (ln C / D(β_n))², D(β_n) = 5.969 − 0.215β_n + 2.532β_n² − 10.73β_n³ + 5.574β_n⁴
  + 0.245β_n⁵, applied per RGB channel. In RISE's NM path, apply the identical formula to the
  **JH-uplifted albedo spectrum** of the bound `IPainter`: σ_a(λ) = (ln C(λ) / D(β_n))². The
  uplift is legitimate here because C *is* an albedo-class color (this is the one hair parameter
  that genuinely belongs on the `IPainter` pipe). The RGB path uses the per-channel formula
  directly, keeping RGB and spectral renders in visual agreement.

### 3.3 HWSS consistency

Lobe selection (choice of p, and the M_p/N_p direction sample) happens once at the hero
wavelength; companions reuse the *same* geometric path and re-evaluate only the
wavelength-dependent factors (Fresnel via IOR(λ), absorption via σ_a(λ)) in `EvaluateKrayNM`.
This matches the established HWSS contract and keeps hero/companion estimates consistent — the
same structure `DielectricSPF` uses for dispersion. When `ior` is bound to a dispersive curve,
companion wavelengths see slightly incorrect exit *directions* (the hero direction is reused);
this is the standard, accepted HWSS approximation for glossy dielectrics and is bounded by the
lobes' roughness. Note the pre-existing HWSS spectral-bundle caveat from the env-IBL arc
(CLAUDE.md High-Value Facts): validate hair scenes with `hwss=true ≡ hwss=false` as a
reference-free invariant, exactly as the integrator matrix work did.

---

## 4. Material architecture mapping

The material follows the `LambertianMaterial`/`GGXMaterial` pattern — a `HairMaterial : IMaterial`
owning **both** a `HairBRDF : IBSDF` and a `HairSPF : ISPF` built from the same painter set
(`LambertianMaterial.h:41-48` precedent), **not** the SPF-only `DielectricMaterial` pattern
(`GetBSDF()` returning `0`, `DielectricMaterial.h:48`): a null BSDF makes a material invisible to
every BDPT/VCM connection strategy (`PathVertexEval.h:215-227` returns black on `!pBSDF`), which
is correct for delta glass and wrong for a rough multi-lobe hair BSDF.

Contract items (from [MATERIALS.md](MATERIALS.md) §9 and the interface headers):

- `HairBRDF::value/valueNM(wi, ri)` — full Chiang evaluation at arbitrary direction pairs
  (needed by PT's NEE as well as BDPT/VCM connections). Direction convention per
  `PathVertexEval.h:18-26`.
- `HairBRDF::albedo(ri)` — closed-form multiple-scattering-averaged reflectance estimate so the
  OIDN albedo AOV is noise-free (required by the checklist; consumed at
  `PathTracingIntegrator.cpp:2703-2710`). Use the color-tier's target reflectance (Tier 3: C
  itself; Tiers 1–2: the inverse of the §3.2 mapping evaluated from σ_a) — cheap and stable.
- `HairSPF::Scatter/ScatterNM` — sample lobe p by A_p(h) energy, populate ONE `ScatteredRay`
  in the container with `kray`/`krayNM`, solid-angle `pdf`, `isDelta = false`. All hair lobes
  are rough (β floors, §6.3), so the default non-specular `SpecularInfo` is kept — hair never
  participates in SMS and never takes the delta paths.
- `HairSPF::Pdf/PdfNM` — the exact mixture pdf (Σ_p P(p|h) · pdf_p); nonzero, enabling MIS
  (materials returning 0 silently disable MIS, `IMaterial.h:195-207`).
- `HairSPF::EvaluateKrayNM` — §3, mandatory for HWSS performance.
- `IsVolumetric()` stays false — fiber-interior absorption is inside the lobe attenuation terms
  A_p, not a medium the integrator marches.

### 4.1 The tangent-frame and h plumbing

A hair BSDF is expressed in the **fiber frame**: u = fiber tangent, with θ measured from the
normal plane. RISE's existing anisotropic BRDFs (Ward, Ashikhmin-Shirley, GGX) read their frame
from `ri.onb.u()/v()` — but for meshes and patches that ONB is built by `CreateFromW(normal)`
with an **arbitrary** tangent (`Object.cpp:656` et al.), which is useless as a
fiber direction.

**[IMPLEMENTED — slice C1/C2, 2026-08-26/27.]** The historical gap this section described is
closed: `RayIntersectionGeometric` now carries a companion `vShadingTangent` +
`bHasShadingTangent` pair that `HairGeometry` writes in **object space** at each hit, and
`Object::IntersectRay` / `CSGObject::IntersectRay` promote it via the forward matrix (one level
per nesting, written back in place — the `vTangent` convention), project into the shading-normal
plane, and hand it to `CreateFromWU(n, t)`; a singular transform clears the flag and falls back.
When the pair is absent, the legacy behavior below is preserved bit-for-bit:
`bShadingTangentFromGeometry` alone yields the projected **world-X** axis (world-Y fallback). That is
exactly what its one current setter (`SDFGeometry` heightfield mode) wants: a *shared, stable*
base tangent so an anisotropic `tangent_rotation` rotates from the same place on an SDF as on the
`cartesian_disk` mesh. It is *not* a curve tangent — the projection is recomputed per hit from
that hit's own world-space shading normal, so it is not literally *one* global axis across a
model, but it is unrelated to (and has no way to recover) the curve's actual tangent, and a hair
render driven by it would still look wrong: every point on a given normal-orientation reads the
same world-X-derived axis regardless of which strand or where along it the hit lands.

The separate `vTangent` / `bitangentSign` / `bHasTangent` triple
(`RayIntersectionGeometric.h:224-235`) *is* a real interpolated per-vertex tangent in world
space — but it is populated only by glTF-loaded triangle meshes carrying a TANGENT array, and it
is consumed by the normal-map modifier, not by the ONB build.

`hair_geometry` landed option **(a) — IMPLEMENTED (Slice C2):** a `vShadingTangent` member sits
alongside `bShadingTangentFromGeometry` (paired with `bHasShadingTangent`,
`RayIntersectionGeometric.h`); `HairGeometry` writes the object-space curve tangent at the hit
(`HairGeometry.cpp`, the `RayElementIntersection` hit block); and `Object::IntersectRay`
(`Object.cpp`, the `bShadingTangentFromGeometry` branch) transforms it object→world with the same
forward-matrix idiom `vTangent` uses (a tangent transforms like a position, not inverse-transpose),
projects it into the world-space shading-normal plane, and hands it to `CreateFromWU` — falling
back to the legacy world-X projection when the supplied tangent is degenerate (near-parallel to
the normal) or simply absent (`bHasShadingTangent == false`, the `SDFGeometry` heightfield case),
so that path stays byte-identical to before this slice. `CSGObject::IntersectRay`'s byte-duplicate
branch got the identical treatment, plus a companion fix: `AdoptCsgSurfacePayload` (the per-surface
boundary-reattribution helper) now copies `vShadingTangent` / `bHasShadingTangent` alongside
`bShadingTangentFromGeometry`, closing a latent mixed-surface-payload gap the same helper already
guards against for `vTangent` / `bHasTangent`. `HairBSDF` now reads a real, geometry-derived fiber
tangent from `ri.onb.u()` for any hair hit that reaches it directly or through a CSG composite.

**One site remains open, found during the round-2 hair BSDF review — still owed:**

- **The normal-map / bump-map modifiers discard any tangent that reaches them.** Even with a real
  fiber tangent now landing in `ri.onb.u()`, `NormalMap::Modify` (`NormalMap.cpp:172`) and
  `BumpMap::Modify` (`BumpMap.cpp:69`) both rebuild the ONB with the unconditional
  `ri.onb.CreateFromW(ri.vNormal)` after perturbing the normal, which drops whatever tangent was
  there and replaces it with an arbitrary one derived from the (perturbed) normal alone.
  `GlintModifier` already gets this right — it rebuilds via `CreateFromWU`, projecting the OLD
  tangent into the new normal's plane, specifically to keep `bShadingTangentFromGeometry` /
  `tangent_rotation` consumers coherent across a perturbation (`GlintModifier.cpp:230-246`). Bump
  or normal-mapped hair needs the same treatment — either fix both modifiers to preserve the
  incoming tangent the `GlintModifier` way, or document that hair fibers are incompatible with
  `bump_map` / `normal_map` until that lands.

The near-field offset **h**, by contrast, needs no new plumbing at all: `hair_geometry` defines
`ri.ptCoord = (s, t)` with s = normalized arc-length root→tip and t ∈ [0,1] across the ribbon
width, so h = 2t − 1 — exactly PBRT's `Curve` convention. The second UV channel
`ri.ptCoord1` (`RayIntersectionGeometric.h:155-165`) carries the **strand's root UV on the base
surface**, baked per-strand at generation time, so any existing `IPainter` grooming/color map
authored in scalp space works on strands unmodified (e.g. a calico color map). This reuses two
existing record fields; no `RayIntersectionGeometric` growth is required for the h side.

Note the same slice owes the *default*: geometry with no UV channel leaves `ptCoord` at (0, 0),
so h resolves to the fiber edge everywhere, where the Fresnel term saturates and the model reads
as a colorless white mirror. `HairBSDF` clamps h just short of ±1 so such a hit still carries
some color, but that is damage control, not a fix.

### 4.2 Registration surface (per-material fixed cost)

Standard five-hop registration, verified against the `AddGGXMaterial` path: material class files
under `src/Library/Materials/`; `RISE_API.h/.cpp` factory; `IJob.h` + `Job.cpp`
`AddHairMaterial` (color slots via `pPntManager`, scalar slots via `ResolveOrDiagnoseScalar`,
`Job.cpp:4103-4157` pattern); a descriptor-driven `HairMaterialAsciiChunkParser`
(`Describe()`/`Finalize()`) registered in `CreateAllChunkParsers()` with `ParameterPipe::Scalar`
vs `::Color` per §3.1 so the parser diagnostics stay truthful.

---

## 5. Geometry representation — survey and recommendation

### 5.1 Requirements

Target scale: 100K strands (a groomed human scalp at render density, or a small-animal patch)
to 1M strands (full-body fur), each with ~8–16 control points, ~0.05–1 mm width, individually
visible in silhouette and specular glints. Every candidate is judged on: memory at 1M strands,
traversal cost, silhouette fidelity, tangent/h availability, and fit to RISE's immutable-scene +
`BVH<>`/TLAS architecture ([ARCHITECTURE.md](ARCHITECTURE.md)).

Baseline unit costs, from source: `Scalar` is `double` (`Math3D.h:26`); indexed-mesh vertices
cost 64 B (24 + 24 + 16), indexed triangles 44 B (`Polygon.h:57-75,144-147`); `BVH<>` nodes are
32 B (BVH2) / 128 B (BVH4 SoA), leaf cap 4 (`BVH.h:83-91,118-134`); a `standard_object`
instance costs ~1.4 KB of `Object` + `Transformable` fixed fields (ten `Matrix4` members alone =
1280 B, `Transformable.h:32-81`) plus a TLAS leaf.

### 5.2 Candidates

**A. Tessellated tubes (bake strands through `sweep_geometry`/meshes).** A 1M-strand groom at
even a crude 8-vertex ring × 16 rings is ~128M vertices (8.2 GB) + ~208M triangles (9.2 GB)
before BVH — **rejected** on arithmetic. Camera-facing ribbon meshes (2 tris/segment) still land
at ~3 GB and cannot actually face the camera (meshes are static). Additionally
`path_instances_geometry` — the closest existing "copies along a path" tool — *duplicates* the
template mesh per instance into one flat mesh with hard caps of 20M total vertices / 100K
instances (`RISE_API.cpp:3844-3846`); structurally unusable here.

**B. Per-strand `standard_object` instancing.** Real shared-geometry instancing exists (N objects
share one `IGeometry*` + its BVH, `Object.cpp:277`), but at 1M strands that is ~1.4 GB of
`Object`/`Transformable` overhead and a 1M-leaf TLAS of *interpenetrating long thin AABBs* — the
worst case for any BVH over whole-strand bounds. Fine for placing a dozen groom *patches*;
**rejected** as the strand representation.

**C. SDF composition.** `sdf_geometry` evaluates `Map()` as a **linear loop over all parts per
sphere-trace step** with no acceleration structure (`SDFGeometry.cpp:971-987`; the
`skeleton_geometry` doc itself warns at 30–70 parts, `ChunkParserRegistry.cpp:6301`). A
1M-capsule union is ~10⁶ × more work per step than budget. **Rejected** for fur; remains the
right tool for a handful of hero tentacles/whiskers today (already expressible via
`skeleton_geometry` roundcones).

**D. Shell/texture (concentric offset shells + alpha noise).** A rasterizer-era technique: no
true silhouettes, no parallax under ray tracing, no per-strand specular. **Rejected** — RISE has
no half-measure culture in materials (cf. the BioSpec skin model) and the payoff is not there
for a path tracer.

**E. NEW runtime curve primitive — `hair_geometry` (RECOMMENDED).** A `HairGeometry : IGeometry`
holding all strands of one groom in shared arrays:

- **Storage:** per-groom arrays of control points (Catmull-Rom, matching the interpolation
  convention `sweep_geometry`/`path_instances_geometry` already use, `RISE_API.cpp:909-912`),
  per-CP or root/tip-lerped widths, per-strand root UV + RNG seed. Control points stored as
  **float3** with widths float — a deliberate, argued exception to `Scalar = double`
  storage: strand CPs are sub-millimeter geometry where float precision is ~10⁴× below width,
  and the 2× memory saving is decisive at scale. Intersection math still runs in double after
  load (the same discipline as `BVH<>`'s float-stored, 1-ULP-padded AABBs feeding
  double-precision leaf tests, `BVH.h:1-22`). ~16M CPs (1M × 16) = 192 MB + widths + 16 B/strand
  metadata ≈ **0.3 GB**.
- **Acceleration:** an embedded `BVH<CurveSegmentRef>` over *segments* (a packed
  {strand, segment} 8-B record; ~13M segments → prims array ~104 MB + BVH4 nodes ~200 MB).
  Precedent: `TriangleMeshGeometryIndexed` embeds `BVH<const PointerTriangle*>`
  (`TriangleMeshGeometryIndexed.h:81`), and `BezierPatchGeometry` proves the
  geometry-owns-a-sub-primitive-accelerator + analytic-(u,v)-intersection pattern
  (`BezierPatchGeometry.h:53-56`). The element type implements the `TreeElementProcessor<T>`
  contract (`TreeElementProcessor.h:26-51`). Segments whose curvature × length exceeds a bound
  are split once more at build for tight AABBs (curve bounds are the known thin-diagonal BVH
  hazard; PBRT's `splitdepth` addresses the same issue). Whole-groom total at 1M strands:
  **≈ 0.6–0.7 GB** — feasible; 100K strands ≈ 60–70 MB — trivial.
- **Intersection:** PBRT-`Curve`-style recursive splitting in a ray-aligned coordinate system,
  double precision, ribbon test against the width-swept curve. Orientation mode: **flat-facing
  intersection + cylinder-mode shading normal** (PBRT's default for hair) — the ribbon always
  presents its width to the ray, while the shading normal curves across the width as a
  cylinder's would, which is what the Chiang h-parameterization expects. A `ribbon` mode with
  fixed orientation (grass/leaves) is a cheap variant flag later.
- **`IGeometry` contract:** `IntersectRay` / `IntersectRay_IntersectionOnly` (shadow fast path
  matters enormously in hair — self-shadowing dominates), `GenerateBoundingBox/Sphere` trivial;
  `UniformRandomPoint`/`GetArea` unsupported with `CanBeAreaLight() = false` (`IGeometry.h:207`)
  — emissive fur is out of scope; `CanTessellate() = false` (hair must not be eligible as a
  `displaced_geometry` base or glTF-export victim); supplies the curve tangent through whichever
  of §4.1's two options is taken, which includes the accompanying `Object::IntersectRay` change
  AND the identical change to `CSGObject::IntersectRay`'s byte-duplicate tangent branch
  (`CSGObject.cpp:1015-1024`, §4.1).

### 5.3 Generation and grooming — the authoring surface

Strand generation is **build-time procedural**, following two proven in-tree patterns: the
parse-time-generator chunks (`skeleton_geometry` expands to SDF parts at parse;
`sweep_geometry` bakes a mesh at parse) and — the better fit here — the
**`Realize()` build hook**: `DisplacedGeometry::Realize()` runs once, single-threaded, before
the parallel pass, evaluating a painter per mesh vertex to displace it
(`DisplacedGeometry.cpp:136-176`, `GeometryUtilities.cpp:500-527`; contract at
`IGeometry.h:209-224`). `hair_geometry` generation goes in `Realize()` because it needs the
*resolved* base geometry and painters, may be expensive (1M strands), and must not run on the
const hot path.

Sketch of the chunk (final grammar is a Phase-1 deliverable; the descriptor-driven parser makes
every parameter self-documenting):

```
hair_geometry
{
    name            scalp_groom
    base_geometry   head_mesh          // named geometry ref; strands grow on its surface
    count           150000             // strand budget (pre-density-mask)
    segments        12                 // CPs per strand
    length          0.06               // meters, scaled per-strand by length_painter
    width_root      0.00008
    width_tip       0.00002
    density_painter d_map              // IScalarPainter over base UV: rejection mask
    length_painter  l_map              // IScalarPainter: per-root length scale
    orient_painter  comb_map           // IPainter (vector-valued): comb direction field
    clump_size      0.004              // clump radius; 0 disables clumping
    clump_painter   c_map              // optional per-region clump strength
    curl_radius     0.0  curl_step 0.0 // helix parameters
    frizz           0.15               // per-CP jitter amplitude (seeded per-strand)
    guides          my_guides          // optional: named explicit guide set (Phase 2)
    seed            7
}
```

Root placement: area-weighted sampling of the base geometry's triangles (base must be a mesh or
tessellatable — the generic `TessellateToMesh` path the displacement system already leans on),
rejection-filtered by `density_painter` at the root UV; grow along the interpolated normal, bent
by the comb field, then clump toward the nearest clump-center strand and perturb by
frizz/curl — all deterministic from `seed` so animation re-`Realize()`s are stable frame to
frame. The painter system needs **zero changes**: `IPainter`/`IScalarPainter` already evaluate
from a full `RayIntersectionGeometric` (a synthetic `ri` at the root, the exact trick
`ApplyDisplacementMapToObject` uses), and the existing procedural painter zoo (perlin, voronoi,
domain-warp, expression VM — 56 painters in `src/Library/Painters/`) becomes the grooming
toolkit for free. This is the payoff of the painter architecture and the core of the
agent-authoring story: **a groom is a handful of scalar/color fields**, which is exactly what
agents already author well.

Explicit strands (`strand { point … point … }` repeated) are also accepted for hero/test cases
and as the guide-set input; `path_closed`-style Catmull-Rom conventions match `sweep_geometry`.

### 5.4 Level-of-detail (deferred, Phase 4)

Distance-tiered strand decimation with width compensation (render N/k strands at k× width and
√k-adjusted opacity/absorption), generated as extra tiers in `Realize()`. Not needed to ship;
noted so the storage layout (strand-major arrays) doesn't preclude it.

---

## 6. Integrator implications

### 6.1 PT (the default and the target)

Hair is the many-bounce forward-scattering glossy regime. The Phase-1 integrator matrix already
concluded PT wins σ²·T on 10/13 classes precisely because of per-sample cost, and hair adds no
caustic-class transport that would re-route it ([UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md),
routing map in [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) §2). The `auto_rasterizer`
Tier-1 static analysis routes unrecognized regimes `else → PT` — hair scenes land there
correctly with **no new clause**; an explicit "high fiber density + low absorption → deep-bounce
PT hint" heuristic is an optional Phase-4 nicety, not a requirement.

Practical knobs (all existing, `AddStabilityConfigParams`, `ChunkParserRegistry.cpp:772-782`):
light-hair scenes need `rr_min_depth` raised (≈ 16+) and `rr_threshold` loosened, plus a
generous glossy-class bounce cap. **Open mapping decision for implementation time:** hair lobes
must be tagged with a `ScatRayType` (`eRayDiffuse/eRayReflection/eRayRefraction/eRayTranslucent`,
`ISPF.h:36-74`); recommendation is to tag **all hair lobes `eRayReflection`** so one existing
knob (`max_glossy_bounce`) governs hair depth, rather than letting TT burn
`max_transmission_bounce` (which authors legitimately cap low for glass interiors). Verify
against the actual `StabilityConfig` bucket mapping before freezing; a dedicated fiber bucket is
a Phase-4 option if the shared bucket proves awkward.

### 6.2 BDPT / VCM

Because `HairBRDF` is a real evaluable-anywhere `IBSDF` with a real `Pdf`, BDPT and VCM work
through the shared `PathVertexEval.h` machinery with no integrator changes. Two honest caveats,
to be recorded in the material header:

- **Reciprocity.** The Chiang model as implemented in PBRT is not exactly reciprocal (near-field
  h-conditioning and the cuticle-tilt sign convention break strict symmetry); PBRT documents and
  accepts this. RISE's BDPT weights assume reciprocity (`IBSDF.h:35-39`), so bidirectional hair
  renders carry a small model-level bias. Given the matrix routes hair to PT anyway, this is a
  documented limitation, not a blocker — and it is one more datum for the standing PT-vs-X
  validation pattern ([skills/bdpt-vcm-mis-balance](skills/bdpt-vcm-mis-balance.md)): expect
  agreement to be *good but not MC-exact* on hair scenes, from the model, before suspecting MIS.
- **VCM merging on fibers** is structurally weak: photon density near 1D geometry is poor, and
  the auto-radius shrink makes fiber merges rarer still. VCM remains reserved for caustic
  transport per the matrix; hair does not change that.

### 6.3 Fireflies and SMS

Hair lobes are never delta (`isDelta = false`, default `SpecularInfo`), so SMS ignores hair
entirely — correct, since SMS targets smooth analytic caustic casters. The known hair firefly
source is TRT glints at very low azimuthal roughness: clamp **β_m, β_n ≥ 0.05** at the parser
boundary with a descriptor-documented floor (production models do the same), rather than
letting near-delta logistic lobes generate outliers that would then get misdiagnosed via the
SMS/BDPT firefly skills.

### 6.4 Dual scattering — rejected

See §2.6. Non-goal: it approximates what PT computes, needs new occlusion infrastructure, and
adds uncontrolled bias. Deep bounce budgets + RR tuning are the supported path.

### 6.5 OIDN

The AOV contract ([OIDN.md](OIDN.md)): albedo/normal are captured **once, at the first non-delta
hit** (`PathTracingIntegrator.cpp:2703-2710`), albedo from `IBSDF::albedo()`, normal from
`ri.geometric.vNormal`. For hair this means per-pixel aux values come from *one* fiber among the
dozens crossing the pixel — the aux buffers will be strand-noise-textured, which is the classic
hair-denoising failure mode (over-smoothing of fine strands or aux-noise passthrough). Phase-1
stance: implement a clean closed-form `albedo()` (§4), render hair test scenes through the
**accurate** AOV mode (whose auxiliary prefilters exist for exactly this), and *measure* —
add an OIDN-on/off comparison to the hair test scene set. If quality is unacceptable, the
follow-up (Phase 4 candidate) is a coverage-weighted multi-sample aux accumulation for
high-frequency geometry, which is an `AOVBuffers` change, not a hair change. Do not block
shipping on denoiser perfection; `oidn_denoise FALSE` remains available per scene.

---

## 7. Phased plan

Phases are gated in the [SMS_UNIFORM_SEEDING_PLAN.md](SMS_UNIFORM_SEEDING_PLAN.md) style: build
gate (all five build projects, warning-free per AGENTS.md), baseline gate (no regression on
existing suites), validation gate (phase-specific). Every phase ends with the
implementation-review-loop (zero P1s) before the next begins. Test `.cpp` files are
auto-discovered by the make build's `$(wildcard tests/*.cpp)` (`build/make/rise/Makefile:40`)
and the CMake test glob — **no build-file edits for tests**; every new *library* source file
costs the full five build surfaces (`Filelist`, `rise_sources.cmake`, `Library.vcxproj`,
`Library.vcxproj.filters`, `project.pbxproj` ×4 sections ×2 targets — CLAUDE.md).

### Phase 0 — reference harness (no product code)

**Goal:** a validation bed that exists before the first BSDF line.
**Work:** (a) a `HairBSDFTest` scaffold wired to the existing `SPFBSDFConsistencyTest` /
`SPFPdfConsistencyTest` patterns (sampling↔eval↔pdf consistency); (b) a **white furnace test**:
σ_a = 0 fiber in a uniform env sphere must return unit reflectance within MC tolerance at
several (β_m, β_n) — this is the energy-conservation regression guard for all later phases;
(c) golden-value cross-check vectors generated from PBRT-v4's `HairBxDF` (Apache-2.0; a small
standalone harness, not vendored into the library) for a grid of (h, θ, φ, β, σ_a);
(d) `scenes/Tests/Hair/` directory seeded with placeholder scene stubs.
**Files:** tests only (+ scene stubs) — zero build-project edits.
**Gate:** harness runs red against a stub, proving it tests something.

### Phase 1 — Chiang BSDF + `hair_geometry` + basic grooming (ships visible value)

**Goal:** an agent or user can author `hair_material` + `hair_geometry` in a scene and get
correct human-hair renders in PT (RGB, spectral, HWSS), with painter-driven density/length/comb
grooming on a base mesh.
**Work items:**
1. `HairBRDF`/`HairSPF`/`HairMaterial` (Chiang lobes, three color tiers, `EvaluateKrayNM`,
   `albedo()`, roughness floors).
2. `HairGeometry` (float CP arrays, segment `BVH<>`, recursive-split ribbon intersection,
   cylinder shading normal, h + dual-UV plumbing, and the curve-tangent plumbing of §4.1 —
   including the `Object::IntersectRay` change it requires).
3. `Realize()` grooming generator: surface sampling, density/length/orient painters, clump,
   frizz, seed-deterministic.
4. Registration: `AddHairMaterial` + `AddHairGeometry` through IJob/Job/RISE_API + two
   descriptor-driven chunk parsers; melanin `IFunction1D` sharing out of `BioSpecSkinData`.
5. Tests: Phase-0 harness goes green; `HairGeometryTest` (intersection/bounds/tangent
   invariants); `scenes/Tests/Hair/` regression scenes (single-strand lobes, sphere groom,
   backlit TT rim, black/brown/blond/red melanin ladder); a PT-vs-BDPT sanity scene with the
   §6.2 tolerance note.
**New library files (each × 5 build surfaces):** `HairBSDF.{h,cpp}` (BRDF+SPF can share a pair),
`HairMaterial.h`, `HairGeometry.{h,cpp}`, `HairGenerator.{h,cpp}` (groom growth) — ≈ 4 `.cpp` +
4 `.h` ≈ **8 new files → 40 build-project touch points**, plus edits to the six shared
surfaces (`RISE_API.h/.cpp`, `IJob.h`, `Job.cpp`, `ChunkParserRegistry.cpp`,
`src/Library/Parsers/README.md`). This is the honest fixed cost; it is one-time.
**Gates:** furnace test green at 4+ roughness pairs; PBRT cross-check within tolerance;
hwss=true ≡ hwss=false on the melanin ladder; no regression in the standing suites; clean-build
warning-free on make + Xcode.

### Phase 2 — grooming depth + interchange

**Goal:** production grooms, not just procedural fuzz.
**Work:** explicit guide-strand sets + guide interpolation (N nearest guides, painter-weighted);
curl/braid helpers; per-strand root-color inheritance scenes; a `.hair`
(Cem Yuksel format) importer under `src/Library/Importers/`; Blender add-on export of hair
Curves objects → `hair_geometry` and `ShaderNodeBsdfHairPrincipled` → `hair_material` in the
exporter's supported-node table (today zero hair references in `src/Blender/addons/rise_renderer/exporter.py`).
**Gate:** a Blender-groomed asset round-trips and renders.

### Phase 3 — fur accuracy (Yan 2017 medulla)

**Goal:** close-up animal fur.
**Work:** TTˢ/TRTˢ lobes + medulla parameters (`medulla_ratio` κ, `medulla_scatter`,
anisotropy g — all `IScalarPainter`) added to the same `HairBSDF` behind κ > 0; precomputed
medulla profile table shipped as a baked header (the JH-LUT bake pattern,
`tools/GenerateSpectrumLUTHeader.py` precedent); validation against Yan's published 9-fur-type
fits.
**Gate:** furnace still green with κ > 0 (energy bookkeeping is the risk); κ = 0 bit-identical
to Phase-1 output.

### Phase 4 — scale and polish (menu, prioritized by observed need)

Strand LOD tiers (§5.4); coverage-weighted AOV accumulation if §6.5 measurement demands it;
optional `auto_rasterizer` Tier-1 hair hint; optional dedicated fiber bounce bucket; elliptical
cross-sections (Khungurn/azimuthal eccentricity) if wavy-hair glint fidelity is requested.

---

## 8. Non-goals — what this plan ISN'T

- **No dual scattering** (§2.6/§6.4) and no real-time/preview hair path.
- **No hair dynamics/simulation** — grooms are static per frame; animation reuses the existing
  per-frame `Realize()`/invalidation flow.
- **No emissive hair** (`CanBeAreaLight() = false`).
- **No SMS involvement** — hair lobes are rough by construction.
- **No new painter machinery** — grooming maps are ordinary painters; if a groom needs a new
  *noise*, that's a painter-zoo addition governed by its own conventions.
- **No BDPT/VCM hair-specific strategies** — hair rides the existing vertex-eval machinery,
  with the §6.2 caveats documented rather than engineered around.

## 9. Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Curve intersection precision/perf (thin-diagonal AABBs, deep recursion) | Med | High | segment splitting at build; PBRT-validated algorithm; `IntersectRay_IntersectionOnly` fast path; bench scene in Phase 1 |
| Float CP storage diverges from `Scalar=double` culture | Low | Med | argued exception documented in header (mirrors `BVH<>` float AABBs); double math after load |
| d'Eon M_p numerical instability at low β | Med | Med | log-space `LogI0` evaluation (PBRT pattern); β floors at parser |
| HWSS spectral-bundle bias re-appears on hair (cf. env-IBL hwss residual) | Med | Med | hwss=true≡false gate in Phase 1; melanin ladder scenes |
| OIDN smears strands | High | Low–Med | measured, not assumed (§6.5); accurate AOV mode; escape hatch `oidn_denoise FALSE`; Phase-4 fix path named |
| Groom memory blowup from careless counts | Med | Low | descriptor-level count/segment caps with diagnostics (the `path_instances` budget-cap pattern) |
| Reciprocity bias flagged as a BDPT/VCM "bug" later | Med | Low | §6.2 recorded here + in the material header; PT-vs-X expectations pre-stated |

## 10. Open questions (to resolve during Phase 1, none blocking start)

1. `ScatRayType` bucket for hair lobes (§6.1 recommendation: all `eRayReflection`; verify
   against `StabilityConfig`'s actual mapping).
2. Exact Catmull-Rom vs B-spline basis for strands (Catmull-Rom recommended for consistency
   with `sweep_geometry`/`path_instances_geometry`; B-spline smooths guide noise better —
   decide when the generator lands).
3. Whether melanin `IFunction1D` sharing is a light refactor of `BioSpecSkinData` accessors or
   a copy of two tables into a shared header (prefer sharing; decide on read).
4. Whether `hair_geometry` participates in `ComputeSurfaceDerivatives` for texture-footprint
   LOD on strands (likely unnecessary — strand widths are sub-footprint).

## 11. References

**Papers.** Marschner, Jensen, Cammarano, Worley, Hanrahan, *Light Scattering from Human Hair
Fibers*, SIGGRAPH 2003. — d'Eon, François, Hill, Letteri, Aubry, *An Energy-Conserving Hair
Reflectance Model*, EGSR 2011. — d'Eon, Marschner, Hanika, *Importance Sampling for
Physically-Based Hair Fiber Models*, SIGGRAPH Asia 2013 TB. — Chiang, Bitterli, Tappan, Burley,
*A Practical and Controllable Hair and Fur Model for Production Path Tracing*, EGSR 2016. — Yan,
Tseng, Jensen, Ramamoorthi, *Physically-Accurate Fur Reflectance*, SIGGRAPH Asia 2015; Yan,
Jensen, Ramamoorthi, *An Efficient and Practical Near-Field Fur Reflectance Model*, SIGGRAPH
2017. — Zinke, Yuksel, Weber, Keyser, *Dual Scattering Approximation for Fast Multiple
Scattering in Hair*, SIGGRAPH 2008 (surveyed; rejected). — Hery & Ramamoorthi 2012; Ou, Xie,
Krishnamachari, Pellacini 2012 (Marschner sampling retrofits, historical).

**Implementations (license-compatible for reference/validation, not vendoring-by-copy without
attribution review).** PBRT-v4 (`github.com/mmp/pbrt-v4`, Apache-2.0): `HairBxDF` in
`src/pbrt/bxdfs.{h,cpp}`, `Curve` in `src/pbrt/shapes.{h,cpp}`, and the online book chapter
*Scattering from Hair* — the primary reference. Blender Cycles Principled Hair (Apache-2.0).
Mitsuba 3 hair plugin (BSD-3). Yan 2017 author release (research code; check license before any
table reuse — the medulla profiles may be re-derived instead).

**In-tree.** [MATERIALS.md](MATERIALS.md) §9 (new-BSDF checklist this plan instantiates);
[ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md); [ARCHITECTURE.md](ARCHITECTURE.md)
(BVH/TLAS, `Realize()`); [OIDN.md](OIDN.md); [UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md)
+ [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) (routing);
`src/Library/Materials/BioSpecSkinSPF.h` + `BioSpecSkinData.h` (melanin precedent);
`src/Library/Geometry/BezierPatchGeometry.h` (embedded-accelerator + analytic-(u,v) precedent);
`src/Library/Geometry/DisplacedGeometry.cpp` (build-time painter evaluation precedent).
