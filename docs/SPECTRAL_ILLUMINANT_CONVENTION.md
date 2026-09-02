# Stage C — the reference illuminant belongs in the spectral forward model

**Date:** 2026-09-02
**Status:** slice 1 (LUT convention + runtime illuminant normalisation + tests) landed; slice 2 (routing every SOURCE term onto the illuminant path) landed — see §7
**Related:** [JH_LUT_GAMUT.md](JH_LUT_GAMUT.md) (LUT quality history), [COLOR_SPACE_MIGRATION.md](COLOR_SPACE_MIGRATION.md) (Stage A = LUT target → Rec.709, Stage B = `RISEPel` → Rec.709)

---

## TL;DR

The Jakob-Hanika uplift LUT used to be trained under a **flat (E) illuminant**. Under
flat E a *flat* spectrum is not neutral — it resolves to `(1.2048, 0.9483, 0.9089)` in
Rec.709 — so the solver could only reach the white cell by fitting a **non-flat** sigmoid
whose flat-integral has D65 chromaticity, and since a reflectance is capped at 1 the only
way to do that is to **suppress red**. That is the measured white-corner collapse
(1.28e-5 at 660 nm) and the reason every grey uplifted red-poor (0.5 grey → 0.36 at
660 nm), compounding ~19 % of the red channel per throughput multiply in spectral renders.

Stage C puts the **target colour space's reference illuminant into the LUT's forward
model** — PBRT-v4's convention. The LUT now converges on **100 % of cells** (was 96.1 %),
authored white uplifts to ≥ 0.99999 at every wavelength, and greys uplift **flat to
~1e-4**.

---

## 1. Diagnosis

`tools/JakobHanikaLUTGen.cpp::IntegrateToTarget` computed, for the rec709 target:

```
rgb = M_XYZ→709 · ( ∫ S(λ)·cmf(λ) dλ ) / ( ∫ ȳ(λ) dλ )        # flat illuminant, identity adapt
```

The old comment justified the flat illuminant like this:

> The runtime spectral integrator computes `∫ S · L · CIE dλ` where L is whatever the
> scene's lights emit — there is no fixed reference illuminant baked into the integrator
> itself. Inverting that forward model would require the LUT to know L at training time.

That reasoning is the bug. **The LUT does not invert the runtime's integral; it defines
what a reflectance MEANS.** An RGB albedo is authored as "what this surface looks like
under white light", and that statement is not well-posed until you say *which* white.

Because `Σx̄ = Σȳ = Σz̄` for the CIE 1931 2° observer, a flat spectrum under flat E has
equal-energy chromaticity, not D65 chromaticity:

| forward model | flat `S = 1` resolves to |
|---|---|
| flat E (pre-Stage-C) | `(1.20485, 0.94827, 0.90894)` |
| D65 (Stage C) | `(0.99991, 1.00002, 1.00005)` |

So under flat E the white cell `(1,1,1)` was only reachable by a sigmoid that *loses* red
— and with `S ≤ 1` the solver could not reach it at all, which is where the 3.9 % of
unconverged cells and the red-end collapse came from.

Two knock-on facts fell out of the same mistake:

* `RGBIlluminantSpectrum` (`sigmoid · D65`) was **double-applying D65** — the sigmoid
  already carried a whitepoint-correcting slope. It is used by no production site today
  (`eSpectrumKind_Illuminant` is selectable but unreferenced outside the painter
  plumbing), which is why nobody noticed.
* Its D65 table was **peak-normalised** at 560 nm (÷ 100), leaving an authored-white
  source ~1.1 % dim relative to the RGB pipe.

---

## 2. The new convention

A reflectance spectrum is trained so that **ρ viewed under the target's reference
illuminant** round-trips:

```
rgb = M_XYZ→RGB · ( ∫ S(λ)·I(λ)·cmf(λ) dλ ) / ( ∫ I(λ)·ȳ(λ) dλ )        I = D65 for rec709
```

Consequences:

* A flat `S = c` is **exactly** the neutral grey `(c, c, c)` (to 9e-5, the CMF/matrix
  rounding floor).
* White is representable at the sigmoid's asymptote — no collapse, no unreachable corner.
* **Light sources must carry the illuminant shape.** `RGBIlluminantSpectrum =
  sigmoid × D65` is now *correct* rather than double-applied.
* **There is no chromatic-adaptation stage any more.** Integrating under `I` yields XYZ
  already referred to `I`'s whitepoint, which *is* the target's whitepoint; the old
  Bradford stage existed only to move the flat-E integral into that frame, and applying
  one now would be a second, erroneous shift. A target whose whitepoint ≠ D65 supplies
  **its own SPD**, not a Bradford matrix. `romm` (D50) and `acescg` (~D60) therefore
  **refuse** with an explicit message until someone adds their SPDs — the generator will
  not silently train them under the wrong white. (ACES's own scene-referred encoding is
  actually defined via a chromatic-adaptation transform to the ACES white point, not by
  integrating reflectance under an ACES-white SPD — so "source the target's SPD" is a
  defensible convention choice here, not the whole story. A future ACEScg implementer
  still has to pick which convention to follow, not just drop in a table.)
* **The film is unchanged.** It integrates radiance × CMF with a uniform Y normalisation
  (`mYNormalization = (b−a)/k_y`, `k_y = ∫ȳ dλ`) and applies the un-adapted
  XYZ(D65)→Rec709(D65) matrix. A white light on a white wall therefore lands on
  `(1, 1, 1)`.

### Runtime normalisation change

`RGBIlluminantSpectrum`'s D65 table went from peak-at-560 (÷ 100) to **Y-normalised**:

```
kD65YNorm = ∫D65·ȳ dλ / ∫ȳ dλ = 98.89248
```

so `∫D65norm·ȳ dλ = ∫ȳ dλ` and an authored-white illuminant of scale 1 resolves to film
`Y = 1` — identical to the RGB pipe. The constant is *computed from the tables* at first
use (`ComputeD65YNorm` in `RGBSpectra.cpp`) rather than hardcoded, so it cannot drift from
the SPD or the CIE observer.

`RGBAlbedoSpectrum` and `RGBUnboundedSpectrum` needed **no code change** — their meaning
changed underneath them (the sigmoid is now reflectance-under-D65).

### Which kind belongs on which slot

| kind | means | use for |
|---|---|---|
| `Albedo` | reflectance ∈ [0,1] under D65 | every multiplicative slot: baseColor, tints, specular colour, transmission colour, alpha, coat tint |
| `Unbounded` | reflectance-*shaped*, magnitude may exceed 1 | HDR texture reads feeding a reflectance slot; BRDF-value uplift |
| `Illuminant` | **radiance source**: `scale · sigmoid · D65norm` | anything that EMITS — light SPDs authored as RGB, emissive material slots, radiance maps, shader ops that uplift computed radiance |

---

## 3. LUT stats, before and after

Both files are `extlib/jakob-hanika-luts/rec709.coeff`, 64³ × 3 sub-tables, 9 437 204 bytes.

| | pre-Stage-C (flat E) | Stage C (D65) |
|---|---|---|
| unconverged cells | **3.9 %** (as recorded in JH_LUT_GAMUT.md) | **0.0 %** (0 of 774 144 non-black cells) |
| mean residual | 1.2 × 10⁻³ | **2.554 × 10⁻⁵** |
| max residual | 9.5 × 10⁻² | **1.003 × 10⁻⁴** on the shipped float32 coefficients (worst cell rgb ≈ (0.001, 0, 0); see verification note below) |
| where failures cluster | deep-blue / saturated gamut corners, `z ≥ 0.67` | nowhere — every (maxC, z-band) bucket is 0.00 % |
| generator wall time | ~30–60 s (documented) | **0.96 s** — the old cost was the 3.9 % of cells burning 200 iterations + a cold restart each |
| md5 | `575b809b5f1198698e69327f72911177` | `b8dabc9548d1cba876c3a32a54712689` |

Of the 786 432 total cells (64³ resolution × 3 max-channel sub-tables), 12 288 are the
z = 0 "perfect black" cells — one full 64×64 (x, y) plane at iz = 0 in each of the 3
sub-tables — that the generator assigns `c = (0, 0, -100)` by fiat with residual forced to
0 rather than solved (`tools/JakobHanikaLUTGen.cpp`, the `z < 1e-9` branch). The 0.0 %
unconverged figure above is over the remaining 774 144 solved cells; the black cells were
never at risk of the flat-E collapse because they were never solved under either forward
model.

The generator now prints a per-(max-channel, z-band) failure histogram at the end of a run
so a future retrain can see clustering without a diagnostic build.

Independently verified outside the solver: 400 randomly chosen cells were re-evaluated
through the forward model from the **float-rounded coefficients as written to disk**;
worst residual 9.93e-5. Corner cells: white 8.3e-5, blue 2.8e-5, green 8.0e-6, red 2.4e-5.
A subsequent full sweep of all 786 432 float32-rounded cells (not just this 400-cell
sample) found the honest worst case reported in the table above: max residual 1.003e-4,
with 43 cells marginally above the 1×10⁻⁴ solver tolerance — float32 rounding of
double-precision solutions that were already under tolerance at solve time, not a
re-emergence of the flat-E collapse.

---

## 4. Probe table — `GetColorNM` on an albedo-kind `uniformcolor_painter`

| authored | 450 nm | 550 nm | 620 nm | 640 nm | 660 nm | 700 nm | 780 nm |
|---|---|---|---|---|---|---|---|
| white (1,1,1) | 0.999929 | 0.999990 | 0.999999 | 1.000000 | 1.000000 | 1.000000 | 1.000000 |
| 0.95 grey | 0.949936 | 0.950037 | 0.950097 | 0.950112 | 0.950127 | 0.950154 | 0.950200 |
| 0.5 grey | 0.499974 | 0.499999 | 0.500031 | 0.500042 | 0.500054 | 0.500081 | 0.500146 |
| 0.2 grey | 0.199833 | 0.199816 | 0.199833 | 0.199842 | 0.199853 | 0.199880 | 0.199959 |
| saturated (0.8,0.2,0.2) | 0.203265 | 0.224346 | 0.736478 | 0.862396 | 0.927188 | 0.975251 | 0.994698 |

Documented **pre-Stage-C** values for comparison: white `0.99998 @ 620` → `0.5189 @ 640`
→ `1.28e-5 @ 660` → `1.43e-7 @ 780`; 0.5 grey → `0.36 @ 660`.

Greys are now flat to ~1.5e-4 across the whole visible (the goal was ~1e-3). Full-gamut
albedo round-trip over 200 random `[0,1]³` triples: mean L2 1.4e-4, max 9.7e-4.

At 6+ significant digits white is `0.999999526 @ 640`, `0.999999700 @ 660`,
`0.99999015 @ 550` — i.e. **1 − ε, with ε wavelength-dependent**. That is why
`IsUntintedWhite` / `GuardedGetColorNM` stay: a multiplicative slot at authored white must
be a *bit-exact* no-op so the NM path matches the RGB path, and `1 − ε ≠ 1`. Every
exactness tripwire in `tests/JHWhiteGuardSpectralTest.cpp` is still live (verified:
`raw(550) != raw(660)` and neither equals 1.0).

---

## 5. What slice 2 must do

Slice 1 deliberately touched **no** emitter, light, radiance-map, shader-op, or material
code path. The convention now says a radiance source must carry the illuminant shape, and
today most of them do not. In rough priority order:

1. **Emission slots → Illuminant uplift.** `eSpectrumKind_Illuminant` is currently
   referenced by *no* production site (only the painter plumbing in
   `UniformColorPainter.h`, `TexturePainter.cpp`, `ExpressionPainter.cpp` implements it).
   Emissive material slots that today request `eSpectrumKind_Unbounded` should request
   `Illuminant`.

2. **Omni / spot / directional lights collapse colour to a Rec.709 luma scalar.**
   `ComputeDirectLightingNM` in `PointLight.cpp` (~143), `SpotLight.cpp` (~165) and
   `DirectionalLight.cpp` (~170) all compute
   `lightLum = 0.2126·r + 0.7152·g + 0.0722·b` and use that one number at every
   wavelength — a coloured light is spectrally *grey* in NM renders. Each needs a real
   illuminant spectrum built from the authored colour.

3. **Radiance maps → Illuminant.** Including the AgentSession studio dome
   (`src/Library/Agent/AgentSession.cpp` ~18300, two `eSpectrumKind_Unbounded` painters).

4. **glTF emissive textures → Illuminant.** `GLTFSceneImporter.cpp` ~669 and ~2911 select
   `eSpectrumKind_Unbounded` for the `emissive` role.

5. **Shader ops that uplift COMPUTED RADIANCE.** `DonnerJensenSkinSSSShaderOp.cpp` (~997),
   `SubSurfaceScatteringShaderOp.cpp` (~356) and `FinalGatherShaderOp.cpp` (~770) all call
   `RGBUnboundedSpectrum::FromRGB(c).Eval(nm)` on an outgoing radiance; they need the
   Illuminant kind. **`DataDrivenBSDF.cpp` (~214) does NOT** — it uplifts a BRDF *value*,
   which is reflectance-shaped, so `Unbounded` is correct there and must stay.

Note that (2) is a bigger change than a kind swap: those three call sites have no spectrum
object at all, just a scalar.

---

## 6. Files changed in slice 1

* `tools/JakobHanikaLUTGen.cpp` — reference illuminant in `IntegrateToTarget`; D65 table
  copied from `RGBSpectra.cpp`; Bradford stage removed; `romm`/`acescg` refuse; failure
  histogram.
* `extlib/jakob-hanika-luts/rec709.coeff`, `src/Library/Utilities/Color/RGBToSpectrumTable_LUTData.cpp` — retrained + rebaked.
* `src/Library/Utilities/Color/RGBSpectra.{h,cpp}` — Y-normalised reference illuminant;
  `RGBIlluminantSpectrum::ReferenceIlluminant` accessor; kind semantics documented.
* `src/Library/Interfaces/IPainter.h`, `src/Library/Materials/CoatedLayer.h` — collapse
  measurements dated as historical; guard restated as a bit-exactness guarantee.
* `tests/JakobHanikaRoundTripTest.cpp`, `tests/RGBPainterSpectralRoundTripTest.cpp`,
  `tests/JHWhiteGuardSpectralTest.cpp` — forward models corrected, tolerances tightened,
  real illuminant-source round-trip added. These tests' D65 weights are read from the
  runtime's `RGBIlluminantSpectrum::ReferenceIlluminant` — the same table production
  uses — so a uniform-scale drift between the generator's private D65 table and the
  runtime's would cancel out of the ratio-based forward model and go undetected here; a
  shape drift (a change to the SPD's relative curve) IS detected.

---

## 7. Slice 2 — routing source terms onto the illuminant path

**Status:** landed 2026-09-02.

### 7.1 The mechanism: route by SLOT, not by painter kind

`SpectrumKind` is fixed when a painter is *constructed*, but a named painter is routinely
bound to a reflectance slot on one material and an emissive slot on another. So the
construction kind cannot decide whether a sample means "reflectance" or "radiance" — the
**consumer** must.

`IPainter` therefore gained a second per-wavelength entry point:

| call | means | who calls it |
|---|---|---|
| `GetColorNM( ri, nm )` | REFLECTANCE at λ (sigmoid alone) | every multiplicative slot — BRDF/BSDF/SPF colours, tints, transmittance, alpha |
| `GetRadianceNM( ri, nm )` | EMITTED RADIANCE at λ (sigmoid × D65norm) | every source term — emitters, area lights, radiance maps, radiance-uplifting shader ops |

`GetRadianceNM`'s **default** (out-of-line in `Painters/Painter.cpp`, so `RGBSpectra.h`
stays out of the ~300 TUs that include `IPainter.h`) uplifts the painter's **composed**
`GetColor` per call. That is the slow path *and the correct semantics for a composite
painter*: what a blend / ramp / checker / noise / mapping painter emits is its composed
colour, so the composed colour is what must carry the illuminant shape. Forwarding to the
children's reflectance spectra would sum reflectance-shaped spectra and never produce D65.
`UniformColorPainter` (cached), `TexturePainter` and `ExpressionPainter` (per-sample, same
cost as their `GetColorNM`) override it to skip a redundant `GetColor` virtual.

**The other overrides are load-bearing, not an optimisation.** `SpectralColorPainter`
(`spectral_painter`), `BlackBodyPainter` (`blackbody_painter`) and
`Function1DSpectralPainter` (the painter a `piecewise_linear_function` chunk registers
under its own name, `Job::AddPiecewiseLinearFunction`) carry a **physical SPD**, not a
Jakob-Hanika uplift of an RGB triple. For those three `GetRadianceNM` forwards to
`GetColorNM` verbatim, exactly as `HosekWilkieSpectralRadianceMap` stays off the uplift
path. Without the override the default would discard the measured spectrum and re-uplift
its RGB projection. Two distinct failure modes, and it matters which is which:

* **Re-uplift (wrong spectrum, not zero).** `SpectralColorPainter::GetColor` returns the
  spectrum's own integrated XYZ (`SpectralColorPainter.cpp`, ctor), so it is **non-black**.
  The classic physically-authored spectral Cornell box
  (`scenes/Tests/Spectral/cornellbox_spectral.RISEscene`) binds `spectral_painter`
  straight to a luminaire's `exitance`, and that is this case: without the override it
  would have emitted a *re-uplifted RGB projection of the measured SPD* — a wrong
  spectrum, silently, not a black render.
* **Zero emission.** Only `Function1DSpectralPainter::GetColor` returns literal
  **black** (`Function1DSpectralPainter.h`). It is reached by binding a
  `piecewise_linear_function` chunk's name to an emissive slot — that chunk registers a
  `Function1DSpectralPainter` in the painter manager alongside the `IFunction1D`. Such a
  binding would have emitted exactly zero on the spectral path.

> **Correction (2026-09-02).** The `234156a5` commit message, and an earlier revision of
> this section, attributed the zero-emission case to `cornellbox_spectral.RISEscene`.
> That is wrong for the reason above — `spectral_painter` resolves to
> `SpectralColorPainter`, whose `GetColor` is non-black, so that scene is the *re-uplift*
> case. Git history is not rewritten; the commit message still carries the
> misattribution. The defensive-design conclusion is unchanged: all three physical-SPD
> painters forward `GetRadianceNM` → `GetColorNM` verbatim regardless of which failure
> mode a given binding would have hit.

**Single-source forwarders forward too (added in the slice-2 follow-up).** The
"composite emits its composed colour" rule applies to painters that genuinely BLEND
several sources. A painter that composes nothing — it re-parameterises `ri` or SELECTS
one child — must forward `GetRadianceNM` to the same child at the same transformed `ri`,
for exactly the two failure modes above: `MappingPainter`, `UVTransformPainter`,
`TexCoord1Painter`, `CheckerPainter`, `LinesPainter`, `Voronoi2DPainter`,
`Voronoi3DPainter`, `ScatterPainter` (coverage compose — alpha is which painter is
visible, 0 or 1 away from the stamp edge), `StochasticTilePainter` (one source, three
hash-offset UVs, reconstructed about an illuminant-shaped mean), and
`HosekWilkieSpectralRadianceMap`'s internal `HWAdapterPainter` (a physical sky SPD).
`tests/PainterRadianceForwardingTest.cpp` pins the contract, with `BlendPainter` as the
negative control that must KEEP the composed default.

**Known limitation of the composition rule:** a physical-SPD painter nested inside a
genuine BLEND (e.g. a `blend_painter` or a Perlin-interpolated `a`/`b` pair of
blackbodies) bound to an emissive slot resolves through the composite's default — its
composed RGB, uplifted — rather than through the children's measured spectra. That is
the price of the "composite emits its composed colour" semantics; no in-tree scene does
it (every `spectral_painter` / `blackbody_painter` emissive binding is direct, or behind
one of the forwarders listed above). If one ever needs to, that composite needs its own
`GetRadianceNM` blending its children's `GetRadianceNM`.

`ILight` gained the analogous `emittedRadianceNM( vLightOut, nm )` — default uplifts
`emittedRadiance()` per call, overridden by Point / Spot / Directional / Ambient with a
spectrum cached at construction.

**The film is unchanged and must stay unchanged.** It integrates radiance × CMF
normalised by `∫ȳ` with no illuminant weighting, because the D65 shape rides in the
sources. Putting D65 in both places double-applies it. A flat unit spectrum through the
film is `(1.20485, 0.94827, 0.90894)`; a D65norm-shaped unit source is `(1, 1, 1)`.

### 7.2 What changed

* **Painters** — `IPainter::GetRadianceNM` (new virtual + default in `Painter.cpp`);
  overrides in `UniformColorPainter` (always builds `illuminantSpec`, whatever the
  construction kind), `TexturePainter`, `ExpressionPainter`.
* **Emitters** — `LambertianEmitter` / `PhongEmitter` `emittedRadianceNM` read
  `GetRadianceNM`; their `RefreshAverages` build `averageSpectrum` from per-bin
  `GetRadianceNM` instead of `GetSpectrum`, so spectral **photon power** carries the same
  shape direct NEE does. `CompositeEmitter` needed no edit — it forwards to its leaves.
* **Lights** — `PointLight` / `SpotLight` / `DirectionalLight` / `AmbientLight` cache an
  `RGBIlluminantSpectrum` built from their colour, rebuilt in `SetIntermediateValue`
  (the *only* colour-write path: keyframes and the SceneEditor / agent light-edit tools
  both funnel through `KeyframeFromParameters` + `SetIntermediateValue`; there is no
  `SetColor`, and `SnapshotLeafClone` rebuilds rather than mutates).
* **Consumers of light emission at a wavelength** — `LightSampler`'s NM delta-light NEE
  row, `VCMIntegrator::EvalLightRadiance<NMTag>`, and four BDPT sites (`LightRadiance`,
  `EvalEmitterRadiance`, the light-subpath `Le`, and the HWSS companion-wavelength `LeW`)
  all dropped their Rec.709 luma projections.
* **Radiance maps** — `RadianceMap::GetRadianceNM` reads `GetRadianceNM`. This covers
  every painter-backed environment, including the AgentSession studio dome.
  `HosekWilkieSpectralRadianceMap` (a physical SPD) is untouched.
* **Area lights** — `AreaLightShaderOp`'s `emm` slot.
* **glTF** — the `emissive` texture role became `eSpectrumKind_Illuminant` at both the
  slow path and the `PreDecodeTextures` fast path (they MUST agree).
* **Shader ops that uplift a computed radiance** — `FinalGatherShaderOp`,
  `SubSurfaceScatteringShaderOp`, `DonnerJensenSkinSSSShaderOp` moved from
  `RGBUnboundedSpectrum` to `RGBIlluminantSpectrum`. `DataDrivenBSDF` deliberately did
  **not**: it uplifts a BRDF *value*, which is reflectance-shaped.
* **Perf** — `RGBIlluminantSpectrum` folds `1/kD65YNorm` into `scale` at construction, so
  the now-per-sample `Eval` performs no function-local-static acquire load.

### 7.3 Measured (64² / 128² A/B renders, `oidn_denoise FALSE`)

spectral ÷ RGB per-channel mean, before → after. Scene `num_wavelengths` in the last
column — it matters, see §7.4.

| scene | before | after | N |
|---|---|---|---|
| white luminaire on 0.9 floor | 1.200 / 0.947 / 0.903 | 0.995 / 1.002 / 0.996 | 80 |
| grey-0.95 luminaire on 0.9 floor | 1.200 / 0.955 / 0.901 | 1.005 / 1.006 / 0.993 | 80 |
| white luminaire on WHITE floor | 1.209 / 0.948 / 0.908 | 0.984 / 0.999 / 1.005 | 80 |
| grey-0.5 box, 2 bounces | 1.199 / 0.950 / 0.908 | 1.003 / 0.998 / 0.995 | 80 |
| omni light (1.0, 0.2, 0.2) | 0.286 / 6.856 / 6.544 (**grey!**) | 1.000 / 0.987 / 1.002 | 80 |
| furnace, white perfect reflector | 1.240 / 0.938 / 0.896 | 1.032 / 0.993 / 1.002 | 10 (default) |
| furnace, grey-0.95 perfect reflector | 1.224 / 0.939 / 0.883 | 1.048 / 1.000 / 1.013 | 10 (default) |

Scenes are in `_abtest2/` (`gen.py` writes (c)/(d)/(e); the s1/s6 pairs are copies of the
slice-1 `_abtest/` scenes with their output paths redirected). `analyze.py` prints the
table.

### 7.4 Known residual: the wavelength-grid bias is NOT closed

The two `N = 10` rows above are the ones still 3–5 % red-heavy, and that is **not** a
slice-2 residual — it is the film's left-Riemann wavelength grid, which has a chroma bias
of its own independent of the uplift convention. Re-rendering the *same* scenes at
`num_wavelengths 80` collapses it:

| scene / ROI | N = 10 (default) | N = 80 |
|---|---|---|
| white panel on white floor | 1.045 / 0.993 / 1.010 | 0.984 / 0.999 / 1.005 |
| furnace, white sphere | 1.032 / 0.993 / 1.002 | 0.999 / 1.010 / 0.988 |
| furnace, grey-0.95 sphere | 1.048 / 1.000 / 1.013 | 1.010 / 1.002 / 0.995 |

Averaged across the white/grey N = 10 rows above, a D65-normalised source resolves to
≈(1.038, 0.989, 1.008) — consistent with a grid-resolution bias rather than a per-scene
anomaly.

The default was deliberately **not** changed in this slice. Any spectral-vs-RGB comparison
must state its `num_wavelengths`, and a future slice should either raise the default or
stratify / jitter the wavelength samples.

### 7.5 Slice-2 follow-up — the source terms the first sweep missed

Landed 2026-09-02, immediately after §7.2. Review found five more sites that produce or
carry a SOURCE term on the NM path. They were not *newly* wrong — they had always been
approximate — but §7.2 made them **differentially** wrong: with every other source now
emitting D65, a site still projecting to a flat Rec.709 luma scalar renders on the
flat-spectrum chromaticity `(1.205, 0.948, 0.909)`, and a coloured source there comes out
grey.

| site | was | now |
|---|---|---|
| SMS, `ComputeTrialContributionNM` + `EvaluateAtShadingPointNM` (`Utilities/ManifoldSolver.cpp`; both seeding modes route through these) | `ColorMath::Luminance(Le)` | delta lights → `ILight::emittedRadianceNM`; mesh lights → illuminant uplift of `LightSample::Le` (`SMSLeNM`) |
| VCM NM merge, `LightVertexThroughput<NMTag>` (`Shaders/VCMIntegrator.cpp`) | `RISEPelToNMProxy` (Rec.709 luma) | illuminant uplift, `VCMIntegrator::LightThroughputRadianceNM` — **superseded 2026-09-02** (see below): production now reads the per-vertex cached `LightVertex::throughputSpectrum.Eval(nm)` |
| `HomogeneousMedium` / `HeterogeneousMedium` `GetCoefficientsNM().emission` | `ColorMath::Luminance(m_emission)` | `m_emissionSpectrum.Eval(nm)`, cached at construction / `SetEmission` |
| single-source forwarding painters (see §7.1's follow-up paragraph) | inherited the composed-`GetColor` default | forward `GetRadianceNM` to the chosen source at the transformed `ri` |
| `SubSurfaceScatteringShaderOp` / `DonnerJensenSkinSSSShaderOp` `PerformOperationNM` | `RGBIlluminantSpectrum::FromRGB(c)` unguarded | `ColorMath::EnsurePositve(c)` first, matching `FinalGatherShaderOp` |

Two things did **not** change, deliberately:

* **`sigma_t` / `sigma_s` keep the luminance fallback.** They are physical MAGNITUDES
  (1/m), not source terms; they carry no illuminant shape. The per-wavelength answer for
  them is an authored `IFunction1D` curve (G1, `absorption_spectral` /
  `scattering_spectral`), not an uplift.
* **`LightVertexStore` is still Pel-DERIVED, not per-wavelength.** The light pass is not
  wavelength-matched to the eye pass — a deposited vertex is merged against eye vertices
  at arbitrary wavelengths, so there is no single `nm` it could have been traced at.
  Storing a `throughputNM` needs a per-wavelength light pass, which is
  [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md) §3's still-open architectural item.
  The uplift is exact for a grey/white light with no coloured bounce and a
  chroma-preserving approximation otherwise. **Updated 2026-09-02**: `LightVertex` no
  longer projects `throughput` fresh per merge candidate. It carries a second field,
  `throughputSpectrum` (an `RGBIlluminantSpectrum`, one cached spectrum per vertex — still
  derived from the single Pel `throughput`, not a genuine per-wavelength quantity), built
  by `VCMIntegrator::LightThroughputSpectrum` at each of the three sites that write
  `throughput` (deposit in `ConvertLightSubpath`, `LightVertexStore::ClampOutlierThroughputs`,
  and the `VCMRasterizerBase` median clamp). The NM merge estimator (`EvaluateMerges`) reads
  `.Eval(nm)` off this cache instead of calling `LightThroughputRadianceNM` per candidate, so
  the JH LUT lookup happens once per stored vertex rather than once per merge candidate;
  `LightThroughputRadianceNM` itself is unchanged (`LightThroughputSpectrum(p).Eval(nm)`) and
  remains available (used by the unit tests to pin the cache), but has zero production
  callers now. Cost: `sizeof(RGBIlluminantSpectrum)` is 4 `Scalar`s (a 3-coefficient
  `RGBSigmoidPolynomial` + a scale) — +32 B/vertex on a `Scalar == double` build — against
  `VCMRasterizerBase.cpp`'s up-front `pLightVertexStore->Reserve(width * height *
  (maxLightDepth + 1))` (~412-415): at 1920x1080 and `maxLightDepth = 10` that reserve is
  1920 × 1080 × 11 ≈ 22.8 M vertices, so the cache adds roughly 22.8 M × 32 B ≈ 730 MB to
  the up-front reservation. Output is bit-identical to the pre-cache projection.

**Where the SMS mesh-light uplift is exact.** `LightSample` carries an RGB `Le` and no
`RayIntersectionGeometric` for the sampled point, so the emitter's own
`emittedRadianceNM` is not reachable without widening the struct and the sampler's fill
path. The uplift round-trips exactly for a grey/white emitter; a spectrally-authored
emitter behind SMS resolves through its RGB projection. Delta lights avoid this entirely
— they have `ILight::emittedRadianceNM` and the call sites use it.

**The one open black-`GetColor` hole (dated 2026-09-02).** A mesh luminaire whose
exitance is bound to a `piecewise_linear_function` (`Function1DSpectralPainter` — RGB
`GetColor` is black by construction; the physical spectrum only exists on the NM path)
has `LightSample::Le == (0,0,0)`, because `Le` is filled from the emitter's RGB
`emittedRadiance`. `SMSLeNM`'s illuminant uplift of a black RGB triple is exactly black,
so **an SMS caustic cast by that emitter is exactly BLACK on the NM path**, while direct
NEE lights the same emitter correctly — `LightSampler.cpp`'s spectral NEE loop (~line
2458) calls `pEmitter->emittedRadianceNM(lumri, -vToLight, lumNormal, nm)` with the full
sampled geometry, which reaches the authored spectral curve. This is **not a new
regression**: `ColorMath::Luminance(RISEPel(0,0,0))` was also `0`, so the pre-uplift code
produced the same black caustic. It is, however, the one case where "chroma-preserving
approximation" above is not merely approximate but wrong (`0` instead of the emitter's
true nonzero spectral `Le`). The exact fix is widening `LightSample` with the sampled
hit's geometry (or at least its normal plus the `IEmitter*`) and the emitter itself, so
`emittedRadianceNM` becomes reachable here too — the sampler already has both in hand at
fill time, so this is a struct-widening change, not a new capability, and is left for
follow-up (no code changed by this note).

Tests: `tests/PainterRadianceForwardingTest.cpp` (new — forwarder contract, with
`BlendPainter` as the negative control), `VolumeSpectralCoefficientsTest` case G
(emission is illuminant-shaped and `SetEmission` rebuilds the cache),
`VCMSpectralRecurrenceTest`'s `LightThroughputRadianceNM` block (white throughput tracks
the reference illuminant; red and blue throughputs of equal luma are no longer identical;
negative-component guard). **Updated 2026-09-02** for the `throughputSpectrum` cache above:
`VCMSpectralRecurrenceTest` also asserts, after `ConvertLightSubpath`, that every emitted
`LightVertex::throughputSpectrum.Eval(nm)` matches (1e-9 relative tolerance, NOT `==`: the
same formula compiled at two call sites is not guaranteed bit-identical under this repo's
`-ffast-math` macOS build — measured spread ~1e-13 relative, see CLAUDE.md's fast-math
entry) a fresh `LightThroughputRadianceNM(throughput, nm)` call (pins the deposit writer)
and that a default-constructed `LightVertex` evaluates to exactly 0 (an honest `==`: the
default's polynomial and scale are both the identically-zero result of `FromRGB(0,0,0)`,
so there is no cross-call-site rounding to tolerate — this documents the silent-zero
signature of a forgotten rebuild); `VCMLightVertexStoreTest` adds the same tolerance-based
pin after `ClampOutlierThroughputs` (pins the second writer, with a provably-rescaled
outlier vertex so the assertion is not vacuous).
