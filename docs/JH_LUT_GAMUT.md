# Jakob-Hanika LUT — Gamut Failures & Why Better Solver ≠ Better LUT

**Date:** 2026-05-09
**Status:** known-limitation, current LUT shipped at commit `a763141` (Landing 3 v2)
**Related:** [PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_3.md](PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_3.md) (the spectral-uplift work the LUT supports), [COLOR_SPACE_MIGRATION.md](COLOR_SPACE_MIGRATION.md) (the proper long-term fix)

---

## TL;DR

The current `extlib/jakob-hanika-luts/romm.coeff` ships with **22 % of cells unconverged** (mean residual 1.6 × 10⁻², max 0.39 in ROMM RGB units). Visible quality on real test scenes (avocado, helmet, skin, chromaticity test) is unaffected because the failures cluster at **gamut corners that real textures essentially never hit**.

The 22 % is **not a solver budget problem**. It is a **structural mismatch between ROMM's RGB gamut and the spectral locus of human vision**: ROMM's green and blue primaries lie OUTSIDE the visible spectral locus, so the corner cells of ROMM gamut correspond to colours no physical (reflectance × illuminant) can produce. The Gauss-Newton solver has nothing to converge to.

The right fix is **not** a smarter solver, more iterations, or a different training illuminant. The right fix is **a different LUT target colour space** — see [COLOR_SPACE_MIGRATION.md](COLOR_SPACE_MIGRATION.md) for that workstream.

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
