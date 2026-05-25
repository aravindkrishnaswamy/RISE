# RISE Internal Colour Space — Migration Brief

**Date:** 2026-05-09 (Stage A landed 2026-05-24)
**Status:** **Stage A done — LUT retrained for Rec.709 + parameterized toolchain.  Stage B in progress.**
**Triggered by:** Landing 3 v2 spectral pipeline correctness work (commit `a763141`) surfaced two structural costs of the current ROMM-RGB-as-`RISEPel` choice that a different working space would resolve cleanly.
**Related:** [JH_LUT_GAMUT.md](JH_LUT_GAMUT.md), [PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_3.md](PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_3.md), [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md)

---

## Stage A — Landed 2026-05-24

- The JH LUT generator (`tools/JakobHanikaLUTGen.cpp`) takes `--target {rec709|romm|acescg}` selecting the target colour space + per-target Bradford adapt at training time.  ACEScg is pre-staged for future migration with verified XYZ↔AP1 + Bradford D65→D60 constants.
- The bake script (`tools/GenerateSpectrumLUTHeader.py`, renamed from `GenerateROMMSpectrumLUTHeader.py`) parameterized by `--target`; refuses anything other than `rec709` until the runtime boundary-conversion type matches.
- Baked source files renamed `RGBToSpectrumTable_ROMMData.{h,cpp}` → `RGBToSpectrumTable_LUTData.{h,cpp}` with target-agnostic symbol names (`kSpectrumLUTFloats`, `kSpectrumLUTResolution`, `kSpectrumLUTNumFloats`).  Build files updated in 5 places.
- `RGBToSpectrumTable::ROMM()` accessor renamed `Get()`.
- Added typed `RGBToSpectrumTable::operator()(const Rec709RGBPel&)` overload that skips the boundary conversion — used by `RGBUnboundedSpectrum::FromRGB` / `RGBIlluminantSpectrum::FromRGB` to compute their scale + normalize in the LUT-target space (fixes a wide-gamut HDR-painter chromatic shift surfaced by the Stage A adversarial review).
- `RISEPel`-taking overload converts → Rec.709 at the call boundary.  When `RISEPel == ROMMRGBPel` (Stage A transition) this is one matrix multiply; when `RISEPel == Rec709RGBPel` (post-Stage-B) it collapses to identity.
- New `rec709.coeff` LUT: **3.9 % failure rate vs the ROMM LUT's 22 %**, mean residual 1.2 × 10⁻³ (well below 8-bit display quantum 4 × 10⁻³).  See [JH_LUT_GAMUT.md](JH_LUT_GAMUT.md).
- Old `extlib/jakob-hanika-luts/romm.coeff` deleted.
- `tests/JakobHanikaRoundTripTest.cpp` rewritten with the Rec.709 forward model; passes 8/8 with mean L2 error 0.0001 on the in-gamut interior sweep.

Net effect: the LUT-quality issue documented in [JH_LUT_GAMUT.md](JH_LUT_GAMUT.md) is largely resolved; spectral pipeline routes through the new LUT correctly; `RISEPel` is still `ROMMRGBPel` so no rendered output changes visibly.  Stage B follows immediately.

---

## Why we're considering a change

`RISEPel = ROMMRGBPel` (declared in [src/Library/Utilities/Color/Color.h:45](../src/Library/Utilities/Color/Color.h)) propagates ROMM RGB through every layer:

- texture decode (sRGB / Rec.709 sources → ROMM internally)
- BSDF math (diffuse / specular / Fresnel multiplications happen in ROMM)
- light emission (RGB-coded lights stored as ROMM)
- pixel accumulation (ROMM sums; XYZ for spectral integrators after Landing 3 v2)
- output (ROMM → sRGB / display target)

The original rationale was sensible:

1. **Wide gamut** — ROMM covers more colour than sRGB, so highly saturated phenomena don't clip in intermediate math.
2. **Linear** — ROMM RGB Linear has no embedded gamma, math composes correctly.
3. **D50 whitepoint** — matches the print-workflow heritage of the codebase.

Two costs surfaced concretely during the spectral-uplift work:

### Cost 1: ROMM has primaries outside the spectral locus

The ~22 % structural failure rate in the JH LUT (see [JH_LUT_GAMUT.md](JH_LUT_GAMUT.md)) is caused by ROMM's green and blue primaries lying outside the CIE 1931 visible spectral locus. Cells at the gamut corners correspond to colours **no physical (reflectance × illuminant) integration can produce**. The solver can't converge to non-physical targets.

| Working space | R primary xy | G primary xy | B primary xy | All inside spectral locus? |
|---|---|---|---|---|
| ROMM RGB (current) | (0.7347, 0.2653) | (0.1596, 0.8404) | (0.0366, 0.0001) | **No** — G & B outside |
| ACES AP0 | (0.7347, 0.2653) | (0.0000, 1.0000) | (0.0001, -0.0770) | **No** — even further out |
| ACES AP1 | (0.713, 0.293) | (0.165, 0.830) | (0.128, 0.044) | **Yes** |
| sRGB / Rec.709 Linear | (0.64, 0.33) | (0.30, 0.60) | (0.15, 0.06) | **Yes** |

A working space whose primaries are all inside the locus has a 100 %-physical gamut. The JH LUT trained against it converges everywhere by construction.

### Cost 2: D50 vs D65 chromatic-adapt friction

ROMM's D50 whitepoint is unusual for rendering — the CIE 1931 standard observer is most commonly referred to D65, and most physical light sources (daylight, blackbody @ 6500K) are D65-ish. Every conversion between RISE's internal ROMM and a D65-referred quantity (CMF integrals, sRGB display, blackbody spectra) goes through a Bradford D65 ↔ D50 adapt step.

The Landing 3 v2 work made this concrete: the BioSpec-skin-vs-JH-uplift conflict that took most of a session to diagnose was at heart "the JH LUT was trained for matrix-only, but the runtime needs adapt+matrix because BioSpec's integrated XYZ is in D65 reference." Aligning the LUT generator with the runtime's standard `XYZtoROMMRGB` (adapt + matrix) fixed it but at the cost of the 22 % gamut-corner failures.

A working space referred to D65 — sRGB Linear, Rec.709 Linear, ACES AP1, ACEScg — eliminates this whole class of friction. CMF integrations, light SPDs, and the working space all share a whitepoint; no Bradford in the math.

## What the alternatives look like

### Option 1: sRGB Linear (Rec.709 Linear, D65)

- **Simplest, smallest gamut.** All primaries inside the spectral locus.
- Matches what almost every PBR tool / DCC pipeline / display target uses.
- Exactly what PBRT-v4 uses internally + spectral upsampling.
- **Loses ~5–15 % of the natural surface-colour gamut** (some skin, some fluorescents, some print-only inks fall outside sRGB). For typical CG content this is fine; for high-end print/cinema/medical it's not.
- HDR EXR sources with values > 1 keep working through `RGBUnboundedSpectrum`.
- D65 whitepoint everywhere — chromatic adapts disappear.

### Option 2: ACES AP1 (D60-ish whitepoint)

- **Best of both worlds for CG/VFX.** All primaries inside the spectral locus, gamut is roughly Rec.2020-class (significantly wider than sRGB, narrower than ROMM).
- Industry standard for VFX and increasingly for film mastering.
- ~D60 whitepoint (not strict D65) — there's still a small chromatic adapt to D65 / sRGB at output, but it's a tiny one and well-understood.
- Slightly more complex than sRGB option, materially better gamut coverage.

### Option 3: ACES AP0 (D60, "full visible gamut")

- AP0's primaries are placed to enclose the entire visible gamut (which means **outside** the spectral locus on G and B, similar to ROMM but more extreme).
- Has the same gamut-corner-non-physical issue as ROMM. Doesn't fix the JH LUT problem.
- Used as an interchange / archival format, not as a rendering working space.
- **Not a candidate** for RISE's internal `RISEPel`.

### Option 4: Pure XYZ internally

- Most physically grounded. No "RGB" abstraction at all in the renderer; convert to display RGB only at output.
- BSDF math becomes more awkward — most BRDF formulations are written assuming RGB-like channels (diffuse, specular, etc. are per-channel vectors). XYZ channels are physical but not naturally aligned with material parameters.
- Major refactor; may be overkill given Option 1/2 cover 99 % of the wins.

### Option 5: Spectral everywhere

- Store a full spectrum per pixel (e.g. 32 wavelength bins) instead of a 3-channel pel.
- Maximally accurate; eliminates JH uplift entirely.
- Memory cost scales linearly with bin count; ~10× memory pressure on the framebuffer.
- Compute cost similar (BRDFs evaluate per-bin).
- Major architectural shift. Likely the "right" long-term answer for the spectral integrator path, but a much larger commitment than Options 1 / 2.

## Recommendation

Two-stage migration, sized to spread the risk:

**Stage A — switch the JH LUT target colour space (small)**

- Retrain the LUT for sRGB Linear (or ACES AP1) — both are inside the locus, both give 100 % LUT convergence.
- At lookup time, gamut-clip incoming ROMM RGB to the target's gamut before the table fetch. Out-of-gamut inputs (rare, only from highly-saturated EXR or hand-authored uniformcolor painters) get desaturated to the gamut boundary, which is the physically-correct thing anyway.
- Runtime stays ROMM-internal. **~50–100 lines changed, no API break.**
- Result: clean LUT, BioSpec / JH still resolve through the same path, no gamut-corner failures.

**Stage B — switch `RISEPel` typedef (large)**

- Move `RISEPel` from `ROMMRGBPel` to `Rec709RGBPel` (or AP1Pel if we add that type).
- Audit every site that uses ROMM-specific math, hard-coded ROMM matrices, or D50 whitepoint assumptions. Notable surfaces:
  - [src/Library/Utilities/Color/Color.cpp](../src/Library/Utilities/Color/Color.cpp) — every conversion matrix, all the chromatic-adapt scaffolding
  - [src/Library/Utilities/Color/Color_Template.h](../src/Library/Utilities/Color/Color_Template.h) — conversion constructors
  - All loader / writer sites in [src/Library/RasterImages/](../src/Library/RasterImages/) — they currently call sRGB/Rec.709 → ROMM at decode and ROMM → sRGB at encode; they'd switch to identity (or near-identity) decode/encode
  - The JH LUT generator (Stage A's retrained LUT becomes the production LUT directly)
  - Every BSDF — most are colour-space-agnostic but a few may have hard-coded ROMM-D50 assumptions worth checking (e.g. [BioSpecSkinSPF.cpp](../src/Library/Materials/BioSpecSkinSPF.cpp) calibrations)
  - All scene files using `colorspace ROMMRGB_Linear` painters — these need to stay supported (decode → new internal space at load)
  - Output sites — the `file_rasterizeroutput`'s `color_space` parameter stays the user's choice (sRGB / Rec.709 / ROMM / ProPhoto), the writer just converts from new internal to whatever was requested
- Re-validate every material / scene-file calibration. Some will need re-tuning if their authored reflectance values were intuitive in ROMM but read differently in the new space.
- **Sized as a 1–2 week dedicated workstream**, not bundled with anything else.
- Result: cleanest possible pipeline. CMF integrations, light SPDs, working space, display all share D65 (or a single small adapt to D60 for AP1). No Bradford in hot paths. JH LUT generator's forward model collapses to "integrate × CMF / k_y → matrix" with no adapt step at all.

**Defer Stage C (full spectral, Option 5) until / unless the spectral pipeline becomes the dominant rendering path.**

## What this work is NOT

- Not a perceptual / colour-management overhaul. RISE has no concept of view transforms, ACES Output Transforms, ICC profiles, etc. We're talking about the *linear working space* the math runs in, nothing more.
- Not changing user-visible scene-file syntax. The `colorspace` parameter on painters and `color_space` on `file_rasterizeroutput` keep working — they describe the SOURCE / TARGET space the user wants, the renderer converts at the boundary.
- Not changing what scenes look like, except where the change is the actual fix (e.g. JH-uplifted highly-saturated colours become physically reachable).

## Open questions for the kickoff session

1. **sRGB Linear vs ACES AP1?** sRGB is simpler and matches displays 1:1; AP1 is wider and matches industry CG / VFX practice. Both are physically clean.
2. **Stage A only, or commit to A + B together?** Stage A alone closes the LUT-quality issue. Stage B is the structural cleanup.
3. **Calibration validation strategy for Stage B.** Which scenes / materials need before/after comparison renders to confirm we haven't shifted any production reference?
4. **Scope of `colorspace ROMMRGB_Linear` user-painter support.** Keep supporting it as a source format (convert-on-load), drop it, or warn?
5. **Texture decode — gamma + space conversion.** sRGB-encoded textures currently decode through `sRGBPel → Rec709 → ROMM`. With Stage B they'd decode `sRGB → Rec709` and stop there (one fewer conversion per texel). Worth measuring.

## What lives in `extlib/jakob-hanika-luts/` after this lands

The current `romm.coeff` becomes either:
- **Stage A**: replaced by `srgb.coeff` (or `acescg.coeff`), with a new entry baked into `RGBToSpectrumTable_ROMMData.cpp` (rename the file accordingly).
- **Stage B**: same as Stage A, the runtime never sees ROMM at the LUT lookup.

The OLD `romm.coeff` may be kept in extlib for a while as a reference / for any tests that still target ROMM directly, then deleted.
