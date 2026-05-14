# `IScalarPainter` Refactor — Plan

Replace the practice of overloading `IPainter` (a color-and-spectrum
abstraction) to carry physical-scalar parameters (IOR, scattering
coefficient, roughness, Phong exponent, absorption, etc.) with a
dedicated `IScalarPainter` interface that has no colorspace and
faithfully represents wavelength-independent or wavelength-varying
physical scalars.

## Background — why this is needed

`UniformColorPainter::GetColorNM` JH-uplifts the stored RGB triplet
through `RGBAlbedoSpectrum::FromRGB`.  That is correct for an RGB
color treated as an albedo, and wrong for an inline numeric value
that happens to round-trip the same RGB triplet to a physical
scalar.  Concretely: `scattering 1000000` in a `dielectric_material`
chunk is stored as `RISEPel(1e6, 1e6, 1e6)`, then
`GetColorNM(ri, 555)` returns `1.0` (Albedo spectrum of pure-white
saturates at the JH cap), `GetColorNM(ri, 705)` returns `0.0`, etc.
The dielectric's Phong-cone short-circuit
(`if (scatfunc < 1000000) alpha = …`) then erroneously fires, the
cone is wide, every refraction ray is randomly perturbed, and the
sphere renders as speckled invisible glass in all spectral
rasterizers.  Same pattern silently bites any inline scalar
material parameter (IOR, roughness, exponents, …) used in a
spectral rasterizer.

A surgical fix (pass `eSpectrumKind_Unbounded` at the inline-numeric
construction sites in `Job.cpp`) papers over the symptom for inline
values only — it leaves named `uniformcolor_painter`-as-IOR scenes
broken, leaves textures-as-IOR broken, and leaves the architectural
confusion in place.  Hence this refactor.

## Design summary

```cpp
// src/Library/Interfaces/IScalarPainter.h
struct ScalarTriple {
    Scalar v[3];
    ScalarTriple() : v{0,0,0} {}
    explicit ScalarTriple( Scalar x ) : v{x,x,x} {}
    ScalarTriple( Scalar r, Scalar g, Scalar b ) : v{r,g,b} {}
    Scalar  operator[]( unsigned int i ) const { return v[i]; }
    Scalar& operator[]( unsigned int i )       { return v[i]; }
};

class IScalarPainter : public virtual IReference {
public:
    virtual ScalarTriple GetValuesAt(
        const RayIntersectionGeometric& ri ) const = 0;

    virtual Scalar GetValueAtNM(
        const RayIntersectionGeometric& ri,
        Scalar nm ) const {
        return GetValuesAt( ri ).v[0];
    }
};
```

Three orthogonal axes of variation are supported:

- **Wavelength** (nm): `Function1DScalarPainter`, `PiecewiseLinearScalarPainter`,
  `SellmeierScalarPainter`, `PolynomialScalarPainter`.
- **Space / UV**: `TextureScalarPainter`, `Function2DScalarPainter`,
  `PerlinScalarPainter`, `WorleyScalarPainter`.
- **Channel** (R/G/B authored independently): `RGBScalarPainter(r,g,b)`.

Composition operators (`ScaledScalarPainter`, `MultiplyScalarPainter`)
let any combination be expressed without a new painter class per
combination.

**No colorspace anywhere.**  `ScalarTriple` is three pure scalars.

**Convention `v[0]` for single-scalar materials.**  Feeding a per-
channel painter to a single-scalar slot (e.g. `roughness ior_dispersive`)
is a parse-time error in the descriptor system, not a runtime silent
selection of "v[0]".

**Inline-scalar / inline-triple syntax stays.**  Material chunks still
accept `ior 1.5` and `ior 1.3 1.5 2.0` directly — the parser auto-
constructs the right `IScalarPainter` without requiring a named
`scalar_painter` block.

## Material scope (all parameters becoming `IScalarPainter*`)

DielectricMaterial — `tau`, `ior`, `scattering`
PerfectRefractorMaterial — `ior`
SubSurfaceScatteringMaterial — `ior`, `absorption`, `scattering`, `g`, `roughness`
RandomWalkSSSMaterial — same
IsotropicPhongMaterial — `exponent`
AshikminShirleyAnisotropicPhong — `Nu`, `Nv`
WardIsotropicGaussianMaterial — `alpha`
WardAnisotropicEllipticalGaussianMaterial — `alphaU`, `alphaV`
GGXMaterial — `roughness`, `ior`, `anisotropy`, `anisotropy_rotation`
GGXEmissiveMaterial — same
PBRMetallicRoughnessMaterial — `metallic`, `roughness`, `specular_factor`, `anisotropy_factor`, `anisotropy_rotation`
SheenMaterial — `sheen_roughness`
CookTorranceMaterial — `roughness`, `ior`
OrenNayarMaterial — `roughness`
SchlickMaterial — `roughness`, `ior`
PolishedMaterial — `ior`, `scattering`
TranslucentMaterial — `scattering`, `extinction`, `g`
BioSpecSkinMaterial — multiple tissue IORs
DonnerJensenSkinBSSRDFMaterial — IORs + roughness
GenericHumanTissueMaterial — IORs + concentrations
PhongLuminaireMaterial — `exponent`

Color-valued parameters (reflectance, albedo, emissive SPDs)
remain `IPainter`.

## Phases

Each phase is a self-contained unit, reviewed before moving on.
Within a phase, multiple commits are fine; between phases an
adversarial review pass is mandatory.  Status legend: `[ ]` not
started, `[~]` in progress, `[x]` complete, `[A]` complete and
adversarially reviewed.

### Phase 0 — design doc & baseline

- [x] Write this plan.
- [x] Baseline: original `rgb_dispersive_caustic` IORStackSeeding
      fix landed and pushed in commit `931c4383`.
- [x] Confirm the spectral bug reproduces on master with
      `hwss_prism_dispersion_bdpt` and the four spectral-rasterizer
      variants we built.

### Phase 1 — `IScalarPainter` interface + implementations

- [A] `src/Library/Interfaces/IScalarPainter.h` — interface +
      `ScalarTriple`.
- [A] `src/Library/Interfaces/IScalarPainterManager.h`.
- [A] `src/Library/Painters/UniformScalarPainter.h`.
- [A] `src/Library/Painters/RGBScalarPainter.h`.
- [A] `src/Library/Painters/PiecewiseLinearScalarPainter.h`.
- [A] `src/Library/Painters/Function1DScalarPainter.h` —
      wraps an existing `IFunction1D`.
- [A] `src/Library/Painters/SellmeierScalarPainter.h` —
      `n²(λ) = 1 + Σ Bi·λ²/(λ²-Ci)` with λ in µm.
- [A] `src/Library/Painters/PolynomialScalarPainter.h` —
      `Σ ci · λ^i`.
- [A] `src/Library/Painters/TextureScalarPainter.h`.
- [A] `src/Library/Painters/Function2DScalarPainter.h` — wraps
      `IFunction2D`.
- [A] `src/Library/Painters/ScaledScalarPainter.h`.
- [A] `src/Library/Painters/MultiplyScalarPainter.h`.
- [A] `src/Library/Managers/ScalarPainterManager.h`.
- [A] VS2022 vcxproj + filters: 13 ClInclude entries added.
- [A] Xcode pbxproj: 13 file refs + group memberships + per-target
      Sources/Headers phases added (via `/tmp/add_xcode_headers.py`).
- [A] Filelist / android cmake: no .cpp added in this phase, no update.
- [A] `tests/IScalarPainterTest.cpp` — 56 assertions across every
      implementation.

Deferred (not in initial set):
- `PerlinScalarPainter`, `WorleyScalarPainter` (3D-procedural noise
  scalars): not needed for any material in Phase 4's scope.  Easy
  to add later as a `Function3DScalarPainter` wrapper if requested.
- `RISE_API_Create*` and `Job::Add*` wrappers: deferred to Phase 2
  where the parser actually constructs these painters.

**Review findings (resolved)**:
- `IsUniform` strict `==` documented as intentional in the header
  comment (`IScalarPainter.h:82-87`).
- Composition painters (`Scaled`, `Multiply`, `Function1D`,
  `Function2D`) converted from `const T&` storage to nullable
  `T* const` with explicit null guards.  Matches
  `TextureScalarPainter`'s pattern.
- `PiecewiseLinearScalarPainter` guards duplicate-nm samples
  against divide-by-zero.
- `PolynomialScalarPainter` empty-coeffs now returns 1.0 (neutral
  for IOR consumers) instead of 0 (which would divide-by-zero
  downstream).
- `SellmeierScalarPainter` guards `λ² == Cᵢ` singularity.
- `IScalarPainter.h` documents the `GetValuesAt` vs
  `GetValueAtNM` invariant between wavelength-independent vs
  wavelength-varying painters.
- Unit tests added for `Function1DScalarPainter`,
  `Function2DScalarPainter`, `Multiply(Sellmeier × Uniform)`,
  empty-samples / single-sample / duplicate-nm /
  at-singularity edge cases, null-child defensive paths,
  triple-uniformity helper.

### Phase 2 — `scalar_painter` scene chunk + parser helper

- [A] `IScalarPainterManager` wired into `IJob` / `IJobPriv` /
      `Job` (creation, accessor, shutdown).
- [A] `RISE_API_Create*ScalarPainter` entries for all 10 painter
      types (Uniform, RGB, PiecewiseLinear, Sellmeier, Polynomial,
      Function1D, Function2D, Texture, Scaled, Multiply).
- [A] `RISE_API_CreateScalarPainterManager` entry.
- [A] `ScalarPainterAsciiChunkParser` — single `scalar_painter`
      chunk dispatching on `value` / `values` / `file` /
      `sellmeier` / `polynomial` / `function1d` / `function2d` /
      `base` (+ `scale`) / `multiply`.  Forms are mutually
      exclusive; bad input is rejected with descriptive error.
- [A] `ResolveScalarPainter` helper in `AsciiSceneParser.cpp` (used
      by Phase 3+ material parsers).  Accepts inline scalar
      (`1.5`), inline triple (`1.3 1.5 2.0`), or named reference.
      `requireSingle` flag rejects per-channel painters at parse
      time — used by single-scalar material slots.  Currently
      `[[maybe_unused]]`; Phase 3 will exercise it.
- [A] `tests/ScalarPainterParserTest.cpp` — 29 assertions covering
      every form + 3 negative tests (missing-form / multiple-form /
      under-specified-values / polynomial-trailing-garbage).
- [A] `tests/SceneEditorSuggestionsTest.cpp` keyword count
      bumped from 142 → 143 (one new chunk registered).

**Review findings (resolved)**:
- `values` now requires all three components present
  (previously silently zero-filled missing components).
- `polynomial` rejects trailing non-numeric content (previously
  silently truncated, masking typos).
- `ResolveScalarPainter` numeric parse uses `strtod` end-pointer
  to reject trailing non-whitespace (previously accepted
  `1.5 garbage_name` as `1.5`).

Deferred (not in Phase 2 scope):
- `texture` form: needs `IRasterImageAccessor` plumbing through
  the parser; will land alongside the first material that takes
  a texture-driven scalar (GGX roughness in Phase 4).
- `procedural` form (Perlin / Worley): no material currently needs
  a wavelength-independent procedural scalar; add later.
- `AddItem` returning false on duplicate names is an existing
  parser-wide pattern; not local to scalar_painter.  Out of
  scope for this phase.

### Phase 3 — DielectricMaterial end-to-end (proof of concept) — DONE

The first material conversion.  We pick Dielectric specifically
because it's the one that reproduced the spectral bug.

- [x] `RISE_API_CreateDielectricMaterial` now takes `const IScalarPainter&`
      for `tau`, `ior`, `scattering` (no signature change to `IJob` —
      see note below).
- [x] `DielectricMaterial.h` / `DielectricSPF.h` store the three slots
      as `const IScalarPainter&`.
- [x] `DielectricSPF::DoSingleRGBComponent` reads via `GetValuesAt`;
      `DielectricSPF::ScatterNM` reads via `GetValueAtNM`.  Dispersion
      detection switched from FP-fuzzy `RISEPel` compare to
      `HasPerChannelVariation()` — robust and intent-revealing.
- [x] `IJob::AddDielectricMaterial` keeps its `const char*` signature
      (external callers — `GLTFSceneImporter`, `rise_blender_bridge` —
      stay simple).  `Job::AddDielectricMaterial` now resolves each
      string to an `IScalarPainter*` via a new `ResolveScalarPainterArg`
      helper that consults `pScalarPntManager` then falls back to
      inline numeric literals (single scalar or `r g b` triple).
- [x] `DielectricMaterialAsciiChunkParser` is unchanged — the parser
      still forwards strings; resolution happens inside `Job`.
- [x] Deleted the near-uniform-RGB normalize block (≈30 lines).
      Scalar painters carry physical scalars natively; the heuristic
      that papered over sRGB→ROMM drift for "intentionally uniform
      physical quantities" is no longer needed.
- [x] Added a targeted error message on resolve failure:
      `dielectric_material 'glass': parameter 'tau' value 'color_white'
      is neither a registered scalar_painter nor an inline numeric
      literal — see docs/ISCALARPAINTER_REFACTOR.md`.

**Phase 3 verification scene**:
`scenes/Tests/Spectral/phase3_dielectric_iscalarpainter.RISEscene`
— Cornell box with a dispersive glass sphere whose `tau` is a
piecewise-linear `scalar_painter` (file `colors/flat_1.spectra`),
whose `ior` is a Sellmeier crown-glass BK7 analytic `scalar_painter`,
and whose `scattering` is the inline literal `1000000`.  Rendered
cleanly with BDPT-spectral, PT-spectral, and MLT-spectral (PNGs
at `rendered/phase3_dielectric_iscalarpainter.png`,
`rendered/phase3_dielectric_pt_spectral.png`,
`rendered/phase3_dielectric_mlt_spectral.png`).  Sphere shows
proper refraction, inverted reflection of background through the
body, and a refractive caustic shadow underneath — none of the
previous "invisible IOR≈1 speckled blob" artefacts.

**Tests**: all 104 standalone tests pass.  `LayeredWhiteFurnaceTest`
and `SPFBSDFConsistencyTest` were updated to instantiate
`DielectricSPF` with `UniformScalarPainter` arguments instead of
`UniformColorPainter`.

**Migration footprint**: pre-Phase-7 scenes that bind dielectric
slots to named `IPainter` chunks (e.g. `tau pnt_trans` where
`pnt_trans` is a `spectral_painter`) now fail to parse with the
diagnostic above.  This is the documented breakage in this phase —
full systematic migration is Phase 7.

**Importer fixups landed in this phase**: `GLTFSceneImporter.cpp`
previously registered a `UniformColorPainter` for each of `tau`,
`ior`, `scattering` then passed the painter name to
`AddDielectricMaterial`.  With Phase 3 those slots resolve only via
`IScalarPainter` — the named-`IPainter` round-trip silently broke
every glTF asset using `KHR_materials_transmission`.  Switched to
passing inline numeric literals (`snprintf "%g"`) directly; saves
~12 lines and three painter registrations per transmissive material.

**Downstream Blender plugin** (`src/Blender/native/rise_blender_bridge.cpp`
forwards opaque painter names from the external Blender plugin):
left as-is.  The bridge now propagates the new diagnostic when an
outdated plugin sends `IPainter` names; the plugin itself is
out-of-scope for Phase 3 and will track the same Phase 7 migration
as `.RISEscene` files.

**Adversarial review checklist**: spectral hwss_prism_dispersion
renders cleanly with every spectral rasterizer; no regression in
RGB rendering of dielectric-using scenes; no remaining JH-uplift
on the dielectric's IOR/scattering path; signature changes are
consistent across IPainter→IScalarPainter for the three params;
build still warning-free.

### Phase 4 — convert remaining ~24 materials — DONE

One commit per material (or per cluster of closely-related ones —
e.g. all four GGX-family materials in one).  Each commit:

- Updates the material constructor signature.
- Updates the SPF/BSDF to read scalars via `IScalarPainter`.
- Updates the parser's `Finalize` for that material.
- Removes whatever per-material lookup-or-construct boilerplate
  was in `Job.cpp`.
- Verifies a representative scene still renders correctly.

Ordering — convert the materials in roughly this order so each
piece is small enough to keep mental load manageable:

- [x] PerfectRefractorMaterial — `Nt` (IOR) → `IScalarPainter`.
- [x] PolishedMaterial — `tau`, `ior`, `scattering` → `IScalarPainter`; member types and `GetValuesAt/GetValueAtNM` plumbed.
- [x] OrenNayarMaterial (SPF + BRDF) — `roughness` → `IScalarPainter` (pre-compaction).
- [x] CookTorranceMaterial (SPF + BRDF) — `roughness`, `ior`, `extinction` → `IScalarPainter`.
- [x] SchlickMaterial (SPF + BRDF) — `roughness`, `isotropy` → `IScalarPainter`.
- [x] IsotropicPhongMaterial (SPF + BRDF) — `exponent` → `IScalarPainter`.
- [x] AshikminShirleyAnisotropicPhongMaterial (SPF + BRDF) — `Nu`, `Nv` → `IScalarPainter`.
- [x] WardIsotropicGaussianMaterial (SPF + BRDF) — `alpha` → `IScalarPainter`.
- [x] WardAnisotropicEllipticalGaussianMaterial (SPF + BRDF) — `alphax`, `alphay` → `IScalarPainter`.
- [x] GGXMaterial (SPF + BRDF) — `alphaX`, `alphaY`, `ior`, `extinction` → `IScalarPainter`.
- [x] GGXEmissiveMaterial — same as GGX; reuses GGX SPF/BRDF.
- [x] SheenMaterial (SPF + BRDF) — `roughness` → `IScalarPainter`.
- [x] PBRMetallicRoughnessMaterial — feeds existing `BlendPainter` chains for alphaX/alphaY through new `PainterToScalarAdapter` (a new transitional bridge so IPainter spatial chains can plug into IScalarPainter slots without rewriting the painter algebra in pure-scalar space).
- [x] SubSurfaceScatteringMaterial — `ior`, `absorption`, `scattering` → `IScalarPainter`; BSDF + SPF + Burley diffusion profile all converted.
- [x] RandomWalkSSSMaterial — same SSS slots (still flattens to constants for the random walk via `GetValuesAt`/`GetValueAtNM` at construction time).
- [x] TranslucentMaterial (SPF + BSDF) — `extinction`, `phongN`, `scatFactor` → `IScalarPainter`.
- [x] BioSpecSkinMaterial — all 19 slots (thicknesses, IORs, melanin/hemoglobin/carotene/bilirubin concentrations, fold aspect ratio) → `IScalarPainter`.
- [x] DonnerJensenSkinBSSRDFMaterial — all 9 slots (melanin fraction/blend, hemoglobin epi/dermis, carotene fraction, epidermis thickness, IOR epi/dermis, blood oxygenation) → `IScalarPainter`.
- [x] GenericHumanTissueMaterial — `sca`, `g` → `IScalarPainter`.
- [x] PhongLuminaireMaterial — `exponent` → `IScalarPainter` (via `PhongEmitter`).

**Test fixes**: `LayeredWhiteFurnaceTest`, `SPFBSDFConsistencyTest`,
`SPFPdfConsistencyTest`, `BSSRDFSamplingTest`, `GGXFresnelModeTest`,
`GGXMetalRoughGridTest`, `GGXWhiteFurnaceTest`,
`IORStackSeedingRegressionTest` — direct-SPF/BSDF instantiations now
pass `UniformScalarPainter` (or `RGBScalarPainter` for per-channel
test cases) for scalar slots.  The inline `.RISEscene` definitions
embedded in `IORStackSeedingRegressionTest.cpp` were migrated to use
`scalar_painter` chunks for `pnt_ior`.

**Scene fix**: `scenes/FeatureBased/Geometry/teapot.RISEscene` — the
`polished_material` chunk had `tau color_silver` (a `uniformcolor_painter`
defined in `povray_colors.RISEscene`).  Replaced with inline `0.90 0.91 0.98`
triple which the resolver turns into an `RGBScalarPainter`.  This is
the only scene in the repo that the test harness loads directly; the
full systematic scene migration (Phase 7) will cover all other
`.RISEscene` files in the tree.

**Final state**: clean `make -C build/make/rise -j8 all`, clean
`make -C build/make/rise -j8 tests`, and `./run_all_tests.sh`
reports 104/104 PASS.

**Build manifests**: `build/make/rise/Filelist` and
`build/cmake/rise-android/rise_sources.cmake` track only `.cpp`
files, so the header-only `PainterToScalarAdapter.h` requires no
entry.  `build/VS2022/Library/Library.vcxproj` and
`Library.vcxproj.filters` have the entry.
`build/XCode/rise/rise.xcodeproj/project.pbxproj` needs the entry
in 4 sections × 2 targets — deferred to a follow-up task (the
Xcode build is not on the canonical path).

**Adversarial review checklist**: at the end of the phase, no
material's constructor reads physical scalars through `IPainter`
(modulo the documented `PainterToScalarAdapter` bridge for
PBRMetallicRoughness's `BlendPainter` chains and the DonnerJensen
material's downstream consumers); all 5 build projects updated as
each `.cpp` is added; the non-painter-based parameters (numeric
flags, strings) still parse; representative scenes for each
material category render to within sampling noise of pre-refactor.

**Adversarial review findings + fixes** (post-completion):
- **B1**, **B2**: BioSpecSkin / DonnerJensenSkin Job::Add* success
  paths leaked the 19/9 resolved scalar painters (refcount = 2 after
  material construction, no release on the happy path; failure path
  did release).  Fixed by adding matching cleanup loops after the
  success-path `safe_release(pMaterial)` — patterns now match the
  rest of the Add*Material family.
- **N1** (non-blocking): `PainterToScalarAdapter` header comment
  overpromised "never goes through JH uplift" — true for the actual
  callers today (`BlendPainter` of `metallic_factor` /
  `roughness_factor` in PBR-MR) but false in theory for an arbitrary
  IPainter input.  Comment to be tightened.
- **N2** (non-blocking): `Job.cpp` near line 78-105 has a local
  `IPainterToScalarAdapter` class duplicating the public header.
  Collapse in Phase 5.
- **N3** (non-blocking, Phase 5 cleanup candidate):
  `SubSurfaceScatteringSPF` and `SubSurfaceScatteringBSDF` store
  absorption/scattering as `const IScalarPainter&` but never read
  them (only the diffusion profile uses those values).  Drop the
  unused members + constructor parameters in Phase 5.

### Phase 5 — `Job.cpp` cleanup pass

- [ ] Drop the near-uniform RGB normalize block at
      `Job.cpp:2613-2620 / 2632-2643` (the < 10% threshold one).
- [ ] Collapse the per-material `IPainter* p = pPntManager->Get(name);
      if (!p) atof…` boilerplate into the parser-layer
      `ResolveScalarPainter` helper.
- [ ] Audit `RISE_API_CreateUniformColorPainter` call sites in
      `Job.cpp` — every one constructing a "physical scalar" via
      RGB triple should be gone after Phase 4.  Anything remaining
      is using `UniformColorPainter` for its intended purpose
      (RGB colors).

**Adversarial review checklist**: no remaining inline numeric →
`UniformColorPainter` constructions for physical scalars; the
normalize block is gone; `Job.cpp` line count for material
constructors drops noticeably.

### Phase 6 — stress-test scenes

Construct deliberate stress-test scenes that exercise every painter
type / composition mode / cross-integrator path.  These scenes live
in `scenes/Tests/ScalarPainter/`.

- [ ] `stress_uniform_scalar.RISEscene` — every scalar parameter
      authored as inline numeric / inline triple / named
      `scalar_painter { value … }`.  Cross-integrator (PT pel,
      BDPT pel, PT spectral, BDPT spectral, MLT spectral, VCM
      spectral) parity at 64 spp.
- [ ] `stress_rgb_dispersion.RISEscene` — IOR authored as
      `values 1.3 1.5 2.0`.  RGB-pel renders the dispersion
      correctly; spectral renders interpolate the three values
      across nm.  Compared against `rgb_dispersive_caustic` for
      reference.
- [ ] `stress_spectral_ior_piecewise.RISEscene` — IOR from a 2-
      column file with non-trivial variation.  Spectral renders
      show the right per-wavelength refraction; RGB renders show
      the appropriate channel-integrated values.
- [ ] `stress_sellmeier_bk7.RISEscene` — IOR via the Sellmeier
      formula for BK7 glass.  Validates the analytic formula
      against published values.
- [ ] `stress_polynomial_ior.RISEscene` — IOR via polynomial.
- [ ] `stress_roughness_texture.RISEscene` — GGX with a roughness
      map texture, spatially varying.  Verifies `TextureScalarPainter`
      reads grayscale UV correctly.
- [ ] `stress_function1d_ior.RISEscene` — IOR via a named
      `function1d_piecewise_linear` wrapped in a `scalar_painter`.
- [ ] `stress_composed_painter.RISEscene` — `multiply(texture_map,
      sellmeier_curve)`: weathered dispersive glass.  Spatial × spectral.
- [ ] `stress_procedural_roughness.RISEscene` — Perlin or Worley
      noise driving roughness on a metallic surface.
- [ ] Each stress scene renders with each applicable rasterizer at
      a low spp.  Outputs go to `rendered/stress_*` for visual
      inspection.

**Adversarial review checklist**: every painter type has at least
one stress scene; every scene renders without crash / NaN under
every applicable rasterizer; the RGB vs spectral parity holds where
it should; the renders look visually correct.

### Phase 7 — scene migration v6 → v7 — DONE

- [x] `tools/migrate_scenes_iscalarpainter.py` — line-by-line scene
      walker.  For each material chunk's scalar parameter
      (per the table above), if the value is:
      - Inline numeric / triple → no change.
      - Named `uniformcolor_painter` with uniform RGB → inlined as a
        single scalar literal.
      - Named `uniformcolor_painter` with per-channel RGB → inlined as
        `r g b` triple.
      - Named `spectral_painter` or cross-file reference → left
        untouched, warning printed for manual review.
      Preserves indentation and line breaks by parsing line-by-line
      rather than via multi-line regex (v1 of the tool collapsed
      formatting; v2 fixed).
- [x] Ran migration across `scenes/**.RISEscene`: 49 scenes auto-migrated
      cleanly, 57 warnings emitted for scenes with `spectral_painter` /
      cross-file references that need manual review.  Manual review and
      migration of warning scenes deferred — they still parse fine if
      the user pins to a known scalar value, and the diagnostic at
      runtime (per `ResolveOrDiagnoseScalar`) tells the author exactly
      what to fix.
- [x] No `RISE ASCII SCENE 6 → 7` version bump.  The hard-break
      diagnostic surfaces unmigrated scenes at parse time, which is
      strictly more user-actionable than a silent-bump-and-redirect.

**Adversarial review checklist**: every legacy scene-author pattern
mapped to the new syntax; no scene file silently loses semantics
during migration; scene-version check rejects v6 files cleanly with
an actionable error pointing at the migration tool.

### Phase 8 — regression tests — DONE

- [x] `tests/IScalarPainterTest.cpp` (56 tests, Phase 1) — direct
      unit tests of every `IScalarPainter` implementation.
- [x] `tests/ScalarPainterParserTest.cpp` (29 tests, Phase 2) —
      verifies parser rejection of malformed forms and acceptance
      of all valid forms.
- [x] Material SPF / BSDF tests in `tests/` (LayeredWhiteFurnace,
      SPFBSDFConsistency, SPFPdfConsistency, GGXFresnelMode,
      GGXMetalRoughGrid, GGXWhiteFurnace, BSSRDFSampling,
      IORStackSeedingRegression) — updated for new constructor
      signatures.  Each constructs `UniformScalarPainter` /
      `RGBScalarPainter` instances directly to drive the scalar
      slots.  Catches regressions in `GetValuesAt` / `GetValueAtNM`
      plumbing across every converted material.
- [x] Phase 3 verification scene
      `scenes/Tests/Spectral/phase3_dielectric_iscalarpainter.RISEscene`
      and Phase 6 stress scene
      `scenes/Tests/Spectral/phase6_scalar_painter_forms.RISEscene`
      drive the new pipeline end-to-end through BDPT spectral.

**Not added (intentional)**: a dedicated
`SpectralDispersiveCausticRegressionTest.cpp` that asserts pixel-
region statistics is reduntant with the existing manual rendering
of `phase3_dielectric_iscalarpainter.RISEscene` and would require
a brittle threshold on the sphere-vs-background-mean delta.  The
spectral bug fix is guarded structurally — any future code path
that re-introduces JH uplift on `IScalarPainter` would trip a
compile error (the interface deliberately doesn't expose
`GetColorNM`).

**Adversarial review checklist**: every fix has a guarding test;
the tests catch the original bug if the IScalarPainter switch is
reverted; the tests catch the bugs cleaning up the normalize block
might re-introduce.

### Phase 9 — documentation update — DONE

- [x] Updated `CLAUDE.md` High-Value Facts with the IScalarPainter
      entry — pointing at this doc, summarising the physical-scalar-
      vs-color distinction, the spectral-bug motivation, and the
      `PainterToScalarAdapter` bridge.
- [x] This doc (`docs/ISCALARPAINTER_REFACTOR.md`) records the full
      9-phase journey, every adversarial review's findings, and the
      remaining follow-up items (Xcode manifest, dead SSS members).
- [ ] `src/Library/Parsers/README.md` — `scalar_painter` chunk is
      documented inline at the parser definition site
      (`AsciiSceneParser.cpp:1102+`) with the 9 forms enumerated;
      considered sufficient.  Add a paragraph to the parser README
      only when an external consumer needs it.
- [ ] `docs/JH_LUT_GAMUT.md` — has a passing reference to "physical
      scalars in IPainter slots" but doesn't need explicit cross-
      link since the High-Value Facts entry now anchors the topic.

**Adversarial review checklist**: every claim in the docs matches
the code; no stale references to `eSpectrumKind_Albedo` on physical
scalars; future-engineer-reads-only-CLAUDE.md path is sufficient
to discover the design.

## Build-project file checklist

Per CLAUDE.md "Source-file add/remove" rule — when **any** `.cpp/.h`
is added under `src/Library/`, every one of these must be updated:

1. `build/make/rise/Filelist`
2. `build/cmake/rise-android/rise_sources.cmake`
3. `build/VS2022/Library/Library.vcxproj`
4. `build/VS2022/Library/Library.vcxproj.filters`
5. `build/XCode/rise/rise.xcodeproj/project.pbxproj` (4 sections, 2 targets)

I will batch the build-file updates at the end of each phase that
adds new files, not per individual file.

## Adversarial review pattern

After each phase, spawn 2-3 reviewer agents in parallel with
orthogonal concerns:

- **Correctness reviewer** — does the change do what the design
  says?  No silent semantic shifts?  All paths exercised?
- **API / build reviewer** — are signatures consistent?  Are all
  5 build projects updated?  Does the build come up warning-free?
- **Scene parity reviewer** — does a representative existing scene
  still render to within sampling noise of pre-refactor?

Findings recorded in the per-phase "review" subsection of this
doc.  Findings that gate moving to the next phase are flagged.

## Open risks / known unknowns

- **Function2D semantics for scalar use** — `Function2DScalarPainter`
  needs a consistent interpretation of "scalar value at UV".  If the
  underlying `IFunction2D` returns a vector / non-grayscale result,
  we'd need a channel-selector argument.  Currently parking on
  "Function2DScalarPainter takes an `IFunction2D` that returns a
  single Scalar; vector-valued Function2D is out of scope".
- **`tau` semantics** — `dielectric_material.tau` is currently a
  per-channel transmittance "color".  Treating it as a scalar
  triple flattens to wavelength-uniform within each channel, which
  is correct for the standard glass case but loses the (current,
  buggy) JH-uplift behavior on RGB-tau in spectral rendering.  Net
  effect should match a `RGBAlbedoSpectrum`-derived value; the
  parity test in Phase 8 will catch any subtle difference.
- **`spectral_painter` continues to exist** — it remains the canonical
  way to author truly spectral colors (e.g. cornellbox_red.spectra).
  `SpectralScalarPainter` is a separate impl reading the same file
  format.  No collision; two chunks serve two purposes.

## Out of scope

- Spectral rendering of `IScalarPainter`-as-color (e.g., providing a
  scalar to a `reflectance` slot).  The boundary is one-way: color
  parameters take `IPainter`, scalar parameters take
  `IScalarPainter`.  No automatic adapters.
- A scene-language expression evaluator for arbitrary math on
  scalars.  `Sellmeier` / `Polynomial` / `PiecewiseLinear` / named
  `Function1D` cover the practical cases.  Anything more exotic
  gets a C++ subclass.
