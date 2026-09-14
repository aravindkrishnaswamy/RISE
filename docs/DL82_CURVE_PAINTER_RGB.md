# DL-82: Sellmeier/Polynomial/Function1D curve painters go per-channel under RGB

Status: **CLOSED** 2026-09-14 (debt-dl82 slice). Fix `3f8f3c69`, red-proof
test `6577afb4`, golden regen `c326b22b`. Full row: [DEBT_LEDGER.md](DEBT_LEDGER.md).

## The bug

`SellmeierScalarPainter`, `PolynomialScalarPainter`, and
`Function1DScalarPainter` each implement `IScalarPainter::GetValuesAt` by
evaluating their wavelength-varying curve/formula at ONE representative
wavelength (587.6nm, 550nm, 555nm respectively — the glass-spec d-line,
an arbitrary mid-visible point, and another arbitrary mid-visible point)
and broadcasting that single number into all three RGB channels:

```cpp
// pre-fix
ScalarTriple GetValuesAt( const RayIntersectionGeometric& /*ri*/ ) const override
{
    return ScalarTriple( EvalAtNM( kRepresentativeNm ) );
}
bool HasPerChannelVariation() const override { return false; }
```

`DielectricSPF`/`PolishedSPF`/`PerfectRefractorSPF` gate their per-channel
RGB dispersion loop on `pRIndex->HasPerChannelVariation()`. A painter that
unconditionally reports `false` never engages that loop, however strongly
its underlying formula actually varies with wavelength — a Sellmeier
dispersion formula (literally `n(λ)`, the textbook definition of a
dispersive glass) bound to `ior` therefore rendered perfectly achromatic
and non-dispersive under every RGB rasterizer.

This is exactly the pattern DL-29 (2026-09-14, earlier the same day) fixed
in the sibling class `PiecewiseLinearScalarPainter`. DL-82 was filed
alongside DL-29 and deliberately NOT fixed in that slice: fixing it would
flip these materials onto the RGB dispersion path, and DL-81 (open at the
time) showed that path carried a ~6-12% per-channel systematic error from
a Sobol sampler bug unrelated to these painters. DL-81 closed later the
same day (`docs/DL81_SOBOL_DIMENSION_PARITY.md`), discharging that
prerequisite and leaving DL-82's own work — the fix, its red-proof, a cost
measurement, and re-rendering the four in-tree scenes that bind these
painters — for this slice.

## The fix

All three classes now follow `PiecewiseLinearScalarPainter`'s DL-29
convention exactly:

- `GetValuesAt` evaluates the curve/formula at the three representative
  wavelengths `ScalarPainterRGB::kChannelNM` = {611, 549, 465} nm — one
  per channel — cached once at construction.
- `HasPerChannelVariation()` returns whether those three cached samples
  differ, via `ScalarTriple::IsUniform()` (an EXACT `==` comparison, not a
  fuzzy tolerance — the three samples come from evaluating ONE curve, so a
  genuinely flat response gives bit-identical samples with no ambiguity).
  A constant polynomial (e.g. the in-tree `polynomial 1.5 0 0` example)
  therefore correctly stays reported as non-dispersive; nothing about this
  fix makes a flat curve dispersive.
- `MakeSingleScalarSlotView()` is now implemented (previously fell through
  to the `IScalarPainter` base default, which returns `nullptr`): each
  class gained a `bSingleSlotView` construction flag whose `GetValuesAt`
  broadcasts the green (549nm) sample and whose `HasPerChannelVariation()`
  is `false`, while `GetValueAtNM` stays the unchanged curve. This plugs
  directly into `Job.cpp`'s existing `ResolveOrDiagnoseScalar` — the exact
  DL-29 mechanism that lets a per-channel curve keep binding to a
  `requireSingle` material slot (`coated_material`'s coat_ior,
  `hair_material`/`subsurfacescattering_material`'s ior, etc.) instead of
  hard-failing now that these curves report genuine per-channel variation.
  No `Job.cpp` change was needed.

`GetValueAtNM` (the function every spectral rasterizer calls) is
byte-unchanged in all three classes — the fix is purely about what the RGB
path reads through `GetValuesAt`/`HasPerChannelVariation()`.

The now-unused single-wavelength constants (`kRepresentativeNm` = 587.6 /
550.0 / 555.0) were removed.

## Red-proof

Unit level, `tests/IScalarPainterTest.cpp` (against the unfixed library):
`TestSellmeierScalarPainter`/`TestPolynomialScalarPainter`/
`TestFunction1DScalarPainter` — 105 passed, 16 **FAILED**: `GetValuesAt`
did not sample the curve at `kChannelNM`, `HasPerChannelVariation()` read
`false` for genuinely dispersive curves, `MakeSingleScalarSlotView()`
returned `nullptr` unconditionally. Fixed: 134/0.

Render level, new `tests/CurvePainterRGBDispersionTest.cpp` — the DL-81
box-slab fixture (lossless 2-deep dielectric box, white Lambertian
luminaire behind, pinhole camera, 1024spp) with `ior` bound to a
`scalar_painter` curve chunk instead of an inline triple:

| row | material | pre-fix (achromatic) | closed form | post-fix |
|---|---|---|---|---|
| A | `sellmeier` BK7 (in-tree coefficients) | R=G=B=0.292554/56/49 | R=0.292624 G=0.292418 B=0.292004 | R=0.292901 G=0.292777 B=0.29208 |
| B | `polynomial 2.0 -0.001*nm` (synthetic, ~3.6% spread) | R=G=B=0.297565/67/60 | R=0.301867 G=0.297461 B=0.291164 | R=0.302164 G=0.297692 B=0.291293 |
| C | `function1d` via `piecewise_linear_function` at kChannelNM (~10.6% spread) | R=G=B=0.292356/58/50 | R=0.278214 G=0.293825 B=0.30766 | R=0.278297 G=0.294035 B=0.308069 |
| D | `polynomial 1.5 0 0` (CONSTANT, in-tree control) | R=G=B=0.29384 | R=G=B=0.293825 | unchanged (control, unaffected by DL-82) |

Pre-fix: 11 passed, 4 FAILED. Post-fix: 15 passed, 0 failed.

BK7's own visible-band dispersion (row A) is physically mild — a ~0.02%
closed-form transmittance spread, the reason optical designers use BK7 as
a low-dispersion "crown" reference glass — so row A's gate is qualitative
("no longer float32-achromatic") rather than a tight quantitative band;
rows B/C use larger synthetic spreads (matching DL-81's own precedent of
choosing clearly-resolvable test values) so their post-fix render can be
gated directly against the closed form.

## Cost

Naive worst case: the RGB dispersion loop traces 3 refractions + 3
Fresnel reflections per dielectric hit instead of 1 (`ScatteredRayContainer::kCapacity`
= 12, so this fits with headroom to spare — see DL-29's row). Measured on
a controlled fixture (BK7-sellmeier dielectric box slab, 128x128, 512spp,
`pathtracing_pel_rasterizer`, 3 runs each via `bin/rise`'s own internal
"Total Rasterization Time" timer):

| | unfixed (non-dispersive) | fixed (dispersive) |
|---|---|---|
| run 1 | 2.185s | 2.648s |
| run 2 | 2.266s | 2.647s |
| run 3 | 2.393s | 2.945s |
| mean | 2.281s | 2.747s |

**+20.4% wall clock** — real, but well under the naive 3x upper bound,
because the per-hit dispersion loop is only one component of total
per-sample cost (camera ray setup, direct-light NEE against the luminaire,
non-dielectric-hit shading on the emitter quad all cost the same either
way).

## Visible change on the four in-tree scenes

`scenes/Tests/Spectral/phase6_scalar_painter_forms.RISEscene`,
`phase3_dielectric_iscalarpainter.RISEscene`,
`scenes/FeatureBased/EnamelWatch/enamel_watch.RISEscene`, and
`scenes/FeatureBased/GuillocheWatch/watch_dial.RISEscene` all bind a
Sellmeier (or, for phase6, also a polynomial) `scalar_painter` to `ior`.

**Under their SHIPPED settings, all four use a spectral rasterizer**
(`bdpt_spectral_rasterizer` / `pathtracing_spectral_rasterizer`), which
calls `GetValueAtNM` — byte-unchanged by this fix — never `GetValuesAt`.
Verified directly: `CstDeriveGoldenTest --dump` on `phase6_scalar_painter_forms.RISEscene`
before/after shows the derive dump's RGB-only diagnostic `probe` triple
for `ior_bk7` moving from `[1.51679844 1.51679844 1.51679844]`
(555nm-broadcast) to `[1.51587118 1.51857284 1.52401238]` (per-channel at
611/549/465nm), while the same line's spectral `@450/@550/@650`
(`GetValueAtNM`) samples are byte-identical. **The fix has zero effect on
these scenes' shipped, spectral renders** — the achromatic-RGB defect only
matters for a scene bound to `pathtracing_pel_rasterizer` /
`bdpt_pel_rasterizer` / etc.

To measure an actual RGB-render effect, `phase6_scalar_painter_forms.RISEscene`
was copied to a scratch file (not committed) and its rasterizer chunk
swapped to `pathtracing_pel_rasterizer` (256spp, `oidn_denoise FALSE`, EXR
`Rec709RGB_Linear`). The whole-frame mean moved by <1% before/after —
swamped by MC noise and fireflies at this sample count, because only ONE
of the scene's three glass spheres is actually affected: `sphere_bk7`
(the Sellmeier sphere) is the only per-channel-genuine one — `sphere_poly`
binds the CONSTANT `ior_poly` form (`polynomial 1.5 0 0`, unaffected by
design, see row D above) and `sphere_rgb` was already per-channel via
`RGBScalarPainter` before this fix. A whole-frame diff on this production
scene is therefore not a useful magnitude signal; the controlled box-slab
fixture above (same BK7 coefficients, closed-form-validated, no
competing geometry) is the reliable evidence.

All four scenes were re-rendered at their shipped settings with `samples`
cut to 4-8 (a pure smoke check, scratch scenes, not committed): all four
produced non-black PNG/EXR output (PIL-measured PNG nonzero-pixel fraction
0.43-1.00, mean intensity 60-139 of 255) and zero NaN/Inf pixels on direct
inspection of the EXR channel data.

## Golden

`CstDeriveGoldenTest`: 448 MATCH, 4 DRIFT (of 452) before regenerating —
exactly the four scenes above, confirmed by digest and by the `--dump`
diff shown. Regenerated: 452 covered, 0 UNCOVERED, 0 STALE (3
pre-existing skipped-diag scenes with missing media, 4 pre-existing
skipped-machine-specific — both unrelated to this fix).

## Sibling audit

Bug pattern in one sentence: *an `IScalarPainter::GetValuesAt` override
that has a wavelength dependence available to it collapses that
dependence to a single representative sample instead of reporting the
curve's actual per-channel variation.*

Every single-argument `ScalarTriple(...)` construction in
`src/Library/Painters/` was enumerated:

| file | broadcasts one value? | same pattern? |
|---|---|---|
| `SellmeierScalarPainter.h` | yes (pre-fix) | **yes — FIXED this slice** |
| `PolynomialScalarPainter.h` | yes (pre-fix) | **yes — FIXED this slice** |
| `Function1DScalarPainter.h` | yes (pre-fix) | **yes — FIXED this slice** |
| `PiecewiseLinearScalarPainter.h` | no (fixed by DL-29) | n/a, already fixed |
| `Function2DScalarPainter.h` | yes | no — `IFunction2D::Evaluate` takes `(x,y)` spatial coords, no wavelength argument at all; genuinely wavelength-independent by construction |
| `TextureScalarPainter.h` | yes | no — reads one grayscale texture channel per the interface's own documented "wavelength-independent by contract" convention |
| `UniformScalarPainter.h` | yes | no — a constant has no wavelength to sample in the first place |
| `PainterChannelScalarPainter.h` | yes | no — selects ONE channel of a spatial `IPainter::GetColor` read; explicitly documented as wavelength-independent by construction (`GetValueAtNM` falls to the default) |
| `ExpressionPainter.cpp` (`ExpressionScalarPainter::GetValuesAt`, scalar-result branch) | yes | no — a scalar-typed expression program genuinely produces ONE number (not three, not a curve); its `GetValueAtNM` is DERIVED FROM this triple via interpolation, the opposite relationship from the curve painters, so there is no discarded per-channel signal to recover |
| `PainterToScalarAdapter.h` | no (per-channel already) | no — reads three DIFFERENT channels of a wrapped `IPainter`; a separate, previously-documented limitation (does not protect against JH spectral uplift on a spectral-file-backed source) out of this row's scope |

No further instances of the DL-29/DL-82 pattern found. No new debt rows
opened.
