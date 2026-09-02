# Stage C — the reference illuminant belongs in the spectral forward model

**Date:** 2026-09-02
**Status:** slice 1 (LUT convention + runtime illuminant normalisation + tests) landed; slice 2 (routing emissive slots onto the Illuminant kind) not started
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
  not silently train them under the wrong white.
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
| unconverged cells | **3.9 %** (as recorded in JH_LUT_GAMUT.md) | **0.0 %** (0 of 786 432) |
| mean residual | 1.2 × 10⁻³ | **2.554 × 10⁻⁵** |
| max residual | 9.5 × 10⁻² | **< 1.0 × 10⁻⁴** (worst cell rgb ≈ (0.001, 0, 0)) |
| where failures cluster | deep-blue / saturated gamut corners, `z ≥ 0.67` | nowhere — every (maxC, z-band) bucket is 0.00 % |
| generator wall time | ~30–60 s (documented) | **0.96 s** — the old cost was the 3.9 % of cells burning 200 iterations + a cold restart each |
| md5 | `575b809b5f1198698e69327f72911177` | `b8dabc9548d1cba876c3a32a54712689` |

The generator now prints a per-(max-channel, z-band) failure histogram at the end of a run
so a future retrain can see clustering without a diagnostic build.

Independently verified outside the solver: 400 randomly chosen cells were re-evaluated
through the forward model from the **float-rounded coefficients as written to disk**;
worst residual 9.93e-5. Corner cells: white 8.3e-5, blue 2.8e-5, green 8.0e-6, red 2.4e-5.

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
  real illuminant-source round-trip added.
