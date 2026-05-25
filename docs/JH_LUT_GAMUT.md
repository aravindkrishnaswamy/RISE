# Jakob-Hanika LUT — Gamut Failures & Why Better Solver ≠ Better LUT

**Date:** 2026-05-09 (Stage A update: 2026-05-24)
**Status:** known-limitation greatly mitigated by Stage A migration; current LUT shipped is `extlib/jakob-hanika-luts/rec709.coeff`
**Related:** [PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_3.md](PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_3.md) (the spectral-uplift work the LUT supports), [COLOR_SPACE_MIGRATION.md](COLOR_SPACE_MIGRATION.md) (Stage A done, Stage B in progress)

---

## TL;DR (Stage A update)

**Stage A migration (2026-05-24)** retrained the LUT against Rec.709 Linear (D65). The current LUT (`extlib/jakob-hanika-luts/rec709.coeff`, baked into `src/Library/Utilities/Color/RGBToSpectrumTable_LUTData.cpp`) ships with **3.9 % of cells unconverged** (mean residual 1.2 × 10⁻³, max 9.5 × 10⁻²). A **5–6× quality improvement** over the prior ROMM LUT.

The residual ~4% failures are at the deep-blue gamut corner where the JH sigmoid model itself has limited expressive power — not at primaries-outside-locus cells (Rec.709 primaries are all inside the locus). Tightening this further would require a richer spectral model (more coefficients) rather than colour-space gymnastics.

The historical 22 % failure rate below applies to the **pre-Stage-A ROMM LUT** (`romm.coeff`, no longer shipped). It is preserved below for diagnostic context.

---

## Pre-Stage-A history (ROMM LUT, 22 % failure rate)

The pre-Stage-A `romm.coeff` shipped with **22 % of cells unconverged** (mean residual 1.6 × 10⁻², max 0.39 in ROMM RGB units). Visible quality on real test scenes (avocado, helmet, skin, chromaticity test) was unaffected because the failures clustered at **gamut corners that real textures essentially never hit**.

The 22 % was **not a solver budget problem**. It was a **structural mismatch between ROMM's RGB gamut and the spectral locus of human vision**: ROMM's green and blue primaries lay OUTSIDE the visible spectral locus, so the corner cells of ROMM gamut corresponded to colours no physical (reflectance × illuminant) could produce. The Gauss-Newton solver had nothing to converge to.

The right fix was **not** a smarter solver, more iterations, or a different training illuminant. The right fix was **a different LUT target colour space** — done in Stage A (2026-05-24). See [COLOR_SPACE_MIGRATION.md](COLOR_SPACE_MIGRATION.md) for that workstream.

---

## What was tried during the diagnostic

A diagnostic CSV mode (`--residuals` flag) was added to `tools/JakobHanikaLUTGen.cpp` during the investigation and reverted before commit. Three LUT regeneration experiments ran to bound the problem:

| Experiment | Forward model | Illuminant | Solver budget | Failure rate | Mean residual |
|---|---|---|---|---|---|
| Baseline (committed) | adapt + matrix | flat E | 200 iter, 1 cold restart | 22.1 % | 1.68 × 10⁻² |
| D65 illuminant | adapt + matrix | D65 | 200 iter, 1 cold restart | 19.8 % | 1.37 × 10⁻² |
| Bumped solver | adapt + matrix | D65 | 1000 iter, 7 cold restarts | (~similar) | (similar) |

Marginal improvement at best. Switching the illuminant from flat E to D65 just **rotated which channel suffers** — failures shifted from maxC=B (70 % in z=hi) to maxC=R (57 % in z=hi). Adding more solver budget didn't move the needle further. The failures aren't about solver — they're about target reachability.

## Why solver tuning can't help

The diagnostic CSV showed the failure pattern is **structural**:

```
Failure rate by (maxC, z-band):
  maxC=0 z=hi  total= 86016 failed= 30840 = 35.9%   <- saturated, max-bright R
  maxC=1 z=hi  total= 86016 failed= 51011 = 59.3%   <- saturated, max-bright G
  maxC=2 z=hi  total= 86016 failed= 60507 = 70.3%   <- saturated, max-bright B
  maxC=*  z=lo total=          failed= 2-3%         <- dim cells, fine
  maxC=*  z=mid total=         failed= 8-11%        <- mid-bright cells, mostly fine

Top-10 worst failures all at z ≥ 0.95 with x ∈ {0, 1} and y ∈ {0, 1}
(the gamut corners — pure-saturated colours at full brightness).
```

82 % of all failures live in `z ≥ 0.67` (high-brightness saturated cells). The `z=low` failures (dim cells) are 2–3 % and trivially fixable; the `z=high` failures dominate and aren't.

## The geometric reason — ROMM's primaries

ROMM RGB has primaries at these xy chromaticities:

| Primary | xy | Inside CIE 1931 visible spectral locus? |
|---|---|---|
| R | (0.7347, 0.2653) | **Yes** — exactly at 700 nm monochromatic |
| G | (0.1596, 0.8404) | **No** — more saturated than any green light |
| B | (0.0366, 0.0001) | **No** — more saturated than any blue light |

A target ROMM colour like `(0, 0, 1)` (pure blue at full brightness) demands an integrated XYZ at the ROMM blue primary chromaticity — outside the spectral locus. **No reflectance × illuminant combination can produce XYZ outside the spectral locus**, so no sigmoid coefficients exist that the solver could converge to. The solver returns "best effort" coefficients instead — typically a sigmoid that integrates to the nearest reachable XYZ along the gamut edge — and reports residual ≈ 0.3 (the ROMM-space distance from the nearest reachable point to the unreachable target).

This is intrinsic to wide-gamut RGB spaces with primaries outside the locus. Changing the training illuminant changes WHICH gamut edge is reachable but doesn't expand the achievable set beyond the spectral locus.

## What this means for real renders

| Scene class | Visible impact |
|---|---|
| glTF PBR with sRGB-encoded textures | None — sRGB gamut is fully inside the spectral locus, all texels' uplifts hit converged cells |
| ROMM-linear textures or measured spectral data | Almost none — Pointer's gamut of real surface colours sits well inside the achievable region |
| HDR EXR with extreme saturation | Some chromatic shift at gamut-corner pixels (RGBUnboundedSpectrum scales the chromaticity-normalized RGB through the LUT, so the same gamut-edge cells get hit) |
| Synthetic scenes targeting ROMM primaries directly | Visible — but no real-world content does this |

The 22 % isn't impacting any production scene. It's an LUT-quality metric, not a render-quality metric.

## Worst-case residual translation

Mean residual 1.6 × 10⁻² in ROMM RGB units = ~1.6 % chromatic error per channel after round-trip. Max 0.39 = ~39 % off in the worst single cell, but those cells are at `(R=1, G=0, B=0)`-style corners. An 8-bit display quantum is ~4 × 10⁻³, so:

- p50 residual (4.7 × 10⁻⁵) — well below quantum, sub-pixel-error
- p75 (9.7 × 10⁻⁵) — still below quantum
- p90 (7.1 × 10⁻²) — visible if a pixel's reflectance landed exactly on a failed cell, but those reflectances correspond to non-physical surface colours
- p99 (2.2 × 10⁻¹) — purely gamut-corner, never hit by real content

## Why we shipped it as-is

1. The fix isn't a solver tweak — it's a target-colour-space change, which is a separate, larger workstream tracked in [COLOR_SPACE_MIGRATION.md](COLOR_SPACE_MIGRATION.md).
2. Real scenes (avocado, helmet, skin) all rendered correctly with the Landing 3 v2 LUT despite the 22 %.
3. Tightening the solver before fixing the underlying gamut would just hide the diagnosis without improving anything visible.

## What NOT to do

- **Don't try to make the solver converge harder on the failed cells.** The targets aren't reachable; you'd be fitting noise.
- **Don't loosen the acceptance tolerance.** The current 1 × 10⁻⁴ threshold is correct (well below the 8-bit quantum). Loosening it would mark genuinely-near-converged cells as "passed" while the gamut-corner cells stay unreachable.
- **Don't switch back to flat-E training.** The previous 5 %-failure rate under matrix-only forward model was a *different* convention and required `IntegratorXYZtoROMMRGB` at runtime, which broke physical-spectrum scenes (BioSpec under blackbody → lavender). Landing 3 v2's adapt+matrix forward + standard runtime resolve is correct; the gamut limitation is the cost.

## Forward path

See [COLOR_SPACE_MIGRATION.md](COLOR_SPACE_MIGRATION.md) for the principled fix: migrate RISE's internal `RISEPel` from ROMM RGB to a colour space whose primaries lie inside the spectral locus (sRGB Linear or ACES AP1). All LUT cells become reachable by construction; the BioSpec / JH conflict at film resolve also disappears more elegantly.

---

## Regenerating the LUT (cross-platform recipe)

When the colour-space migration lands (or any other reason to regen the LUT), run these from the project root:

```sh
# 1. Build the generator — pick ONE of these:
#    Mac / Linux (fastest, single-file standalone):
c++ -O3 -std=c++17 -o bin/tools/JakobHanikaLUTGen tools/JakobHanikaLUTGen.cpp -lm

#    Mac / Linux (via main Makefile, builds every tool):
make -C build/make/rise tools

#    Windows (VS2022):
"$msbuild" build\VS2022\Tools\JakobHanikaLUTGen.vcxproj /p:Configuration=Release /p:Platform=x64

# 2. Regenerate the binary LUT (~30-60 sec on modern hardware for the
#    default 64-resolution grid).  Default target is rec709 since 2026-05.
#    Output: extlib/jakob-hanika-luts/rec709.coeff
./bin/tools/JakobHanikaLUTGen --target rec709 \
    --output extlib/jakob-hanika-luts/rec709.coeff
#    On Windows: ./build/bin/tools/JakobHanikaLUTGen.exe ...

# 3. Bake the binary into the source tree.  Output:
#    src/Library/Utilities/Color/RGBToSpectrumTable_LUTData.cpp (~32 MB)
python3 tools/GenerateSpectrumLUTHeader.py --target rec709

# 4. Rebuild Library — every platform's build system already knows about
#    RGBToSpectrumTable_LUTData.{h,cpp} via the 5-build-system update.
#    No build-file changes needed.
make -C build/make/rise -j8 all                 # Mac / Linux
# OR Xcode: build the Library scheme in build/XCode/rise/rise.xcodeproj
# OR VS2022: rebuild Library.vcxproj
```

`tools/JakobHanikaLUTGen.cpp` accepts `--target {rec709|romm|acescg}` so a future migration to ACES AP1 is a one-line change at LUT bake time.  The runtime
`RGBToSpectrumTable::operator()` hardcodes the boundary conversion into Rec.709 — switching targets in the LUT alone without extending the runtime's conversion type is a silent footgun, so the bake script refuses anything other than `rec709` until that runtime extension lands (see comment in `tools/GenerateSpectrumLUTHeader.py`).
