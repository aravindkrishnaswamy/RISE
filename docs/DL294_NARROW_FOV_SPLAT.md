# DL-294: the light-tracing splat covered a half-pixel-short film

**Status:** fixed on branch `debt-dl294` (2026-09-28), row left open for the
supervisor to strike after a fresh review.

## 1. The symptom, and why it looked like a field-of-view defect

`tests/WeaveGapShadowTransmittanceTest.cpp` (DL-05) lights a Lambertian
patch with a narrow spot light through a weave gap, so the only estimator
BDPT and VCM have for that light is the t = 1 light-tracing splat.  At a
pinhole field of view of 2 degrees on a 16 x 16 film BDPT read the closed
form -6.05 %, deterministically, at any sample count (-6.14 % at 16384 spp),
under every pixel sampler, while PT was exact; VCM's own no-sheet render
read -1.45 %.  A field-of-view sweep made it look angular: -6.2 / -6.1 /
-2.5 / +0.03 / +0.1 % at fov 1 / 2 / 3 / 5 / 10 degrees.

It is not angular.  The spot fills a 1-2 degree frame edge to edge, fills
all but the top and bottom rows of a 3 degree frame, and leaves the edges
of a 5-10 degree frame dark.  The defect lives only at the film edges.

## 2. Mechanism

(Historical: section 8 below moved the convention to PBRT's under DL-368;
the mechanism is described here in the convention that was live at the
time.)

Every rasterizer draws pixel (x, image row y) at screen position
`(x + u - 0.5, H - y + v - 0.5)`, u, v in [0, 1) -- so the film the eye
subpaths sample is screen x in [-0.5, W - 0.5), y in [0.5, H + 0.5) -- and
every splat site (`SplatFilm::SplatFiltered`'s box fast path, the unfiltered
fallbacks in `BidirectionalRasterizerBase`, `MLTRasterizer`,
`MLTSpectralRasterizer` and `VCMIntegrator`) rounds a splat to the NEAREST
pixel centre in that same convention.

The camera side did not agree.  `BDPTCameraUtilities::Rasterize{Pinhole,
Fisheye,Orthographic}` and `ThinLensCamera::RasterFromLensPoint` returned
FALSE outside the camera's NOMINAL film, raster x in [0, W), y in [0, H) --
half a pixel off the rasterizers' convention on both axes.  So:

- the strips x in [-0.5, 0) (left half of image column 0) and y in
  [H, H + 0.5) (upper half of image row 0) are ON the eye film and were
  rejected by the camera: a t = 1 splat from there never existed;
- the strips x in [W - 0.5, W) and y in [0, 0.5) are OFF the eye film and
  were accepted by the camera; the box path rounded them to pixel W / row H
  and dropped them (correct), the filtered path (any non-box
  `pixel_filter`, the default gaussian included) renormalised them onto
  the last column / row.

Box filter: the splat layer covers (W - 0.5) x (H - 0.5) of the film, with
column 0 and row 0 at half radiance: 1 - (15.5/16)^2 = 6.15 % on a uniformly
lit 16 x 16 frame (measured -6.05 .. -6.25 %).  Gaussian (default) filter:
the mean survives, but the splat layer is MISREGISTERED by half a pixel --
column 0 / row 0 at ~0.49 of the interior, the last column / row at ~1.49.

## 3. What was ruled out, by measurement

- **The projection itself.**  `GenerateRay` followed by `RasterizeThrough`
  round-trips to <= 8.5e-14 px at the DL-294 framing (pinhole fov 2 deg,
  16 x 16) and at ordinary framings, pinhole, thin lens (at a fixed lens
  point) and fisheye alike (`CameraImportanceTest` Test 9).  There is no
  angular or world-space epsilon anywhere in the inverse.
- **`We` / `PdfDirection`.**  Already pinned to the closed form at 1e-12
  (`CameraImportanceTest` Test 5); the fix does not touch them, and the
  closed-form rows close to within a few hundredths of a percent with them
  unchanged.
- **The MIS weight.**  VCM with merging OFF reproduces BDPT's number to the
  last digit pre-fix (-6.171 / -6.051 / -2.513 % at fov 1 / 2 / 3), so the
  loss is in the splat's reach, not in how a strategy is weighted.
- **The weave.**  The no-weave control (the same framing, a plain
  Lambertian floor under the spot) and a mirror caustic with no weave
  (topology W) both show it; see section 5.

The pre-fix `CameraImportanceTest` film-response integrals (Tests 6 and 8,
"a uniformly radiating plane filling the frustum integrates to exactly 1")
were blind to this because they integrated over the camera's OWN accept
region; asked of the film's region they read 0.984 on a 64 x 64 film,
which is (63.5/64)^2.

## 4. Fix

Film membership is decided in ONE place, by the film:

- `SplatFilm::NearestPixel( fx, fy, W, H, ix, iy )` (static, in
  `SplatFilm.h`) is the rule -- the nearest pixel centre in the rasterizers'
  convention; FALSE off the film or for a non-finite position.  The box fast
  path, the broken-filter fallback, the NEW home-pixel check at the top of
  the filtered path, and the four unfiltered fallbacks all call it (they
  used to carry their own copies of the rounding).
- The camera projections stop clipping to the nominal film.  They reject
  only points outside `BDPTCameraUtilities::InRasterGuardBand` -- a
  convention-agnostic one-pixel band around [0, W) x [0, H), wide enough to
  contain the film under any half-pixel convention and narrow enough that
  no caller's integer cast can overflow.
- The filtered path now rejects a splat whose home pixel is off the film
  instead of renormalising it onto the edge pixels.

A splat that lands just outside the film is now evaluated (visibility, MIS)
and then dropped by the film; that is the only extra work.  It is NOT
confined to a symmetric one-pixel ring: the strip is 0.5 px on the left/top
(`InRasterGuardBand`'s [-1, 0) margin against the film's own [-0.5, 0)
strip) and 1.5 px on the right/bottom (the guard band's [W, W+1) against a
film that ends at W - 0.5) -- about 19 % extra t = 1 work on a 16 x 16
film, about 0.2 % at 1080p, and efficiency-only (the dropped splats do not
bias the image).

**DL-368 is not decided here.**  `NearestPixel` encodes the rasterizers'
CURRENT pixel convention; the fix removes the camera's second, different
encoding of it.  Whether `NearestPixel` needs to move with a future DL-368
change depends on WHICH side that change fixes: a RASTERIZER-side fix
(moving eye-ray sample placement to `x + u`, PBRT's convention) must move
`NearestPixel` in the same commit, since it is the splat side's copy of
that same convention; a CAMERA-side fix (changing every camera's film
translation and its analytic inverse to agree with the rasterizers'
CURRENT `x + u - 0.5`) leaves `NearestPixel` untouched, since the splat
side would already be right.

**Correction (external review, 2026-09-29): `CameraImportanceTest` Test 9,
the `fovsweep` edge fingerprints and topology W do NOT catch a future
ONE-SIDED convention change, contrary to what an earlier revision of this
section and of Test 9's own comment claimed.**  The reviewer moved the eye
sample placement to PBRT's `(x + u, H - y + v - 1)` in `BDPTPelRasterizer`,
`VCMPelRasterizer` and `BoxPixelFilter::warpOnScreen` (which also covers
PT's own box-filtered eye rays), left `SplatFilm::NearestPixel` untouched,
and all three named guards stayed green: `fovsweep` 48/0, BDPT topology W
+0.066 %, VCM W1/W2 -0.127 % / +0.103 %, `CameraImportanceTest` 984/0.  A
one-pixel stripe probe on the mutated build showed the real misregistration
instead: PT's and BDPT's direct-emitter hit lit raster column 16 alone
(0.159), while BDPT's t = 1 splat of the SAME physical stripe still split
across columns 16 and 17 (0.0405 / 0.0390).  Two reasons the guards miss
it: Test 9 hard-codes `x + offs - 0.5` in its own screen-coordinate
construction instead of reading what the mutated rasterizer actually does,
so it re-derives the very convention it exists to check rather than
observing it; and `fovsweep`'s and topology W's frames are UNIFORMLY lit,
so a half-pixel shift of the whole splat domain changes neither the mean
nor the edge-to-interior ratio the existing fingerprints read.  **The real
guard is `TestNarrowFovStripeGuard` (`BDPTStrategyBalanceTest.cpp`,
`--narrow-fov-only`)**: a 32 x 32 pinhole box-filter fixture with an
INTERIOR floor edge (not a whole-frame uniform field) rendered once via
ordinary NEE/hit (PT) and once via a caustic-only t = 1 splat (BDPT, VCM
with `merge_radius 0.0`), comparing the FRACTIONAL COVERAGE each side
reads at the transition column PT itself locates (never hard-coded).  On
the fixed build (n = 4 salted, three independent salt-base checks):
PT-direct 0.5001, BDPT-splat 0.4989/0.4934/0.4993, VCM-splat
0.5001/0.5012/0.4980 -- worst spread 0.0067, gated at a 0.03 band (a
~4.5x margin over the measured spread, still >15x below a one-sided-shift
defect).  **Red-proofed against the reviewer's own mutation** (moving
`BDPTPelRasterizer`/`VCMPelRasterizer`/`BoxPixelFilter::warpOnScreen`'s
eye-ray placement TO PBRT's `(x + u, H - y + v - 1)` convention, leaving
`SplatFilm::NearestPixel` on the CURRENT convention -- rebuilt, measured,
`git checkout HEAD -- <the three files>`, rebuilt again to confirm the
restore): PT-direct's OWN eye-ray sample placement moved with the
mutation too, so the physical floor edge (fixed in world space, at the
camera's optical centre) now falls INSIDE pixel 16 instead of straddling
its boundary -- PT-direct reads **0.0000** at column 16 (the fixture's own
sanity check, "PT locates a genuine partial-coverage transition column,"
correctly FAILS), while BDPT's splat -- unaffected, since a t = 1 splat's
raster position comes from `BDPTCameraUtilities::RasterizeThrough` and
`NearestPixel`, neither of which the mutation touches -- still reads
**0.4989**, exactly its green-build value.  VCM-splat reads **0.0070**, a
smaller residual (VCM's own eye-vertex generation shares the mutated
`ptOnScreen` formula, which perturbs its overall MIS mixture even though
its splat's raster position is unaffected the same way BDPT's is).  The
net effect: BDPT-splat vs PT-direct fails by 0.4989 (a whole column of
coverage misaligned), and the whole test goes red (9/2, two failing
checks) where the fixed build reads 11/0.

## 5. Evidence

**DL-390 correction (2026-10-02, debt-cheapbatch):** the old
`RenderSalted` helper had its explicit salt overwritten by `Render`.
The earlier spread table below is historical and is superseded for the
current helper by this n=4, seed-1000 audit (sample sd, percent):

| fov | BDPT gap error / sd | VCM gap error / sd | VCM no-merge gap error / sd |
|---|---|---|---|
| 1 | +0.362 / 0.946 | -0.174 / 0.394 | +0.237 / 0.274 |
| 2 | -0.165 / 0.128 | -0.242 / 0.073 | +0.145 / 0.186 |
| 3 | +0.036 / 0.068 | -1.103 / 0.075 | -0.053 / 0.090 |
| 5 | +0.037 / 0.017 | -0.201 / 0.166 | +0.002 / 0.033 |
| 10 | +0.019 / 0.030 | +0.175 / 0.140 | +0.040 / 0.025 |

The retained fov-1 BDPT band 3% is 3.17 sample sd; VCM's 2% and
no-merge's 3% are 5.08 and 10.95 sd. Other fovs have wider margins.
Corrected Gaussian-filter measurements and five full-suite gates are in
[the batch record](DL_CHEAPBATCH_VALIDATION.md). Edge fingerprints remain
individual salted captures. The transport fix and its red fingerprint
are unchanged.

### Historical evidence (before the DL-390 helper correction)

Isolated A/B: the fix files reverted to `82de1d42b` (the pre-fix commit
carrying the new tests), rebuilt, run, restored.  BDPT's render of these
fixtures is independent of the libc seed (its Sobol streams are keyed by
pixel and sample index), so repeats read identically and the residuals are
a fixed QMC pattern, not noise; VCM's light pass is seeded and carries a
real sd.

`WeaveGapShadowTransmittanceTest` `WEAVE_GAP_FILTER=dl294`, n = 4 (seeds
1000-1003), 2 x 2 patch, spot, 1024 spp, every cell relative to PT's no-sheet
render at the same fov:

| fov | BDPT gap 0.3 pre | post | VCM L0 pre | post | VCM gap 0.3 pre | post |
|---|---|---|---|---|---|---|
| 1 | -6.171 | +0.014 | -0.411 +/- 0.000 | +0.024 +/- 0.000 | -0.408 +/- 0.020 | -0.316 +/- 0.015 |
| 2 | -6.050 | +0.095 | -1.423 +/- 0.001 | +0.061 +/- 0.001 | -0.533 +/- 0.071 | -0.168 +/- 0.045 |
| 3 | -2.518 | -0.021 | -0.950 +/- 0.006 | +0.013 +/- 0.013 | -1.347 +/- 0.103 | -1.050 +/- 0.074 |
| 5 | +0.034 | +0.036 | +0.015 +/- 0.012 | +0.012 +/- 0.010 | -0.093 +/- 0.032 | -0.107 +/- 0.150 |
| 10 | +0.164 | +0.179 | +0.056 +/- 0.031 | +0.096 +/- 0.011 | +0.034 +/- 0.225 | +0.293 +/- 0.179 |

(percent; BDPT sd 0.000 at every cell.)  VCM with merging OFF, post-fix:
+0.014 / +0.095 / -0.020 / +0.024 / +0.152 % -- BDPT's numbers.  The residual
in full VCM's gap column (-1.05 % at fov 3) is its merge-radius blur of the
spot's penumbra, which reaches into a 3 degree frame's top and bottom rows;
it is absent with merging off and shrinks with VCM's progressive radius --
not a splat effect.

**"BDPT sd 0.000 at every cell" describes ONE FIXED QMC realisation, not
zero variance, and P2-2 (external review, 2026-09-29) is right to call it
fragile: the table above never salted the render, so a repeat is
bit-identical by construction and its "n = 4" gave no error bar at all.**
Salting the render (`SobolSamplerTestHooks::ValueSalt`, a genuinely
different QMC draw per repeat -- `TestNarrowFovSplat` and
`MeasureNarrowFovSplat` both do this now) exposes a real, fov-dependent
spread: BDPT gap sd is **0.344 %** at fov 1 against 0.240 / 0.089 / 0.035 /
0.033 % at fov 2 / 3 / 5 / 10 (n = 8, seed base 1000, this binary's own
salting scheme); VCM-merging-off gap sd is 0.616 % at fov 1 against
0.236 / 0.069 / 0.031 / 0.034 % at the other fovs.  The gated
`TestNarrowFovSplat`'s pre-review fov-1 band was 1 % on these rows -- a
~2.9 sd margin on BDPT and a band NARROWER than one VCM-merging-off sd --
so an unrelated change perturbing the Sobol' pattern had a real chance of
turning fov 1 red with no bias present.  Fixed: fov 1's gap band widened
to 3 % (an ~8.7 sd margin on 0.344 %, ~4.9 sd on 0.616 %) for the three
rows whose band was 1 % at other fovs; fov 2 and above keep their
original bands, whose margins over the salted sd above are all >= 20x.
Full n = 8 salted table (percent, mean +/- sd, BDPT gap / VCM-merging-off
gap; the doc's own earlier single-realisation numbers above -- e.g. fov 10
BDPT gap "+0.179 %" -- are ONE such draw, not an error bar):

| fov | BDPT gap | VCM-merging-off gap |
|---|---|---|
| 1 | -0.410 +/- 0.344 | -0.126 +/- 0.616 |
| 2 | +0.033 +/- 0.240 | +0.062 +/- 0.236 |
| 3 | +0.012 +/- 0.089 | -0.012 +/- 0.069 |
| 5 | +0.024 +/- 0.035 | +0.027 +/- 0.031 |
| 10 | +0.060 +/- 0.033 | +0.059 +/- 0.034 |

Every row's mean stays within the gated bands' margins above; none of
these salted means show a bias the single-realisation table's numbers
did not already suggest -- the finding is about the ERROR BAR, not the
mean.

Edge fingerprint of the fov 2 gap render (edge row/column mean over the
interior mean): box filter, BDPT column 0 **0.494 -> 0.998**, row 0
**0.521 -> 1.023**; gaussian filter, column 0 **0.501 -> 1.004**, last column
**1.484 -> 0.965**, row 0 **0.514 -> 1.017**, last row **1.470 -> 0.986**.

No-weave control (0.2 x 0.2 patch, fov 2, spot, no sheet, against
rho/pi * I/d^2): PT -0.023 %, BDPT -0.022 % in both builds (BDPT's own NEE
carries that frame; its splat's MIS share there is ~0); VCM **-1.447 ->
+0.039 %** (balance heuristic: the splat carries ~23 % of it).  The weave is
irrelevant.

Topology W (`BDPTStrategyBalanceTest` / `VCMStrategyBalanceTest`), no weave
anywhere, closed forms:

| row | pre | post |
|---|---|---|
| BDPT W: spot -> mirror -> floor caustic, t = 1 only | -6.246 % | +0.066 % |
| VCM W1: the same caustic (n = 3) | -0.731 +/- 0.036 % | -0.367 +/- 0.022 % |
| VCM W2: spot -> floor, no mirror | -1.463 % | +0.021 % |

(W1's post-fix residual is the same merge-radius blur as above; BDPT reads
+0.07 % on the scene.)

Red / green: `WeaveGapShadowTransmittanceTest` `fovsweep` 35/13 -> 48/0 (whole
suite 180/0); `BDPTStrategyBalanceTest --narrow-fov-only` 1/3 -> 4/0 (whole
suite 274/0); `VCMStrategyBalanceTest --narrow-fov-only` W2 red on every run
pre-fix (whole suite 87/0); `CameraImportanceTest` 972/12 -> 984/0 with only
the camera-side clip reverted.

## 6. Shipped exposure

No shipped scene renders under `bdpt_*`, `vcm_*` or `mlt_*` at a pinhole fov
below 20 degrees, and no scene anywhere in `scenes/` authors a `fov` of 5 or
less.  The narrow-fov MEAN loss therefore reaches no shipped scene.  The edge
misregistration reaches every BDPT/VCM/MLT render in proportion to how much
of its edge rows the splat layer carries.  Quarter-resolution (128 x 128),
16 spp, OIDN off, the scenes' own (default gaussian) filter, n = 6 per build,
two binaries interleaved:

| scene | image mean | column 0 | last column | row 0 | last row |
|---|---|---|---|---|---|
| `cornellbox_bdpt` | -0.02 % (z -1.4) | -0.03 % | +0.08 % | +0.93 % (z 2.6) | +0.04 % |
| `cornellbox_bdpt_caustics` | +0.10 % (z 1.9) | +0.07 % | -0.22 % | +2.53 % (z 1.0) | +1.01 % |
| `cornellbox_vcm_caustics` | +0.04 % (n.s.) | **+7.88 % (z 39)** | **-7.43 % (z -34)** | **+8.86 % (z 49)** | **-8.83 % (z -84)** |
| `cornellbox_mlt_fast` | 0.000 % | 0.00 % | 0.00 % | 0.00 % | 0.00 % |

VCM's balance heuristic gives the splat a large share of caustic scenes, so
its edge rows show the half-pixel misregistration at +/-8 %: the first
column and row were half-starved of splat energy and the last ones carried
the renormalised strip from beyond the film.  On `cornellbox_vcm_caustics`
specifically (128 x 128, 16 spp, OIDN off, n = 8 salted, single-threaded,
pre vs post; edge-to-neighbour ratio = column 0 / column 1, last column /
second-to-last, row 0 / row 1, last row / second-to-last): pre-fix
0.871 / 1.007 / 0.882 / 1.064, post-fix **0.940 / 0.938 / 0.961 / 0.974**,
against PT's own (unaffected) 0.934 / 0.943 / 0.951 / 0.977 on the same
scene -- the fixed BDPT/VCM edges now match PT's, closing the dark
left/top rim (95.3 -> 99.9 against a neighbour of 101.8 in an 8-bit
zoomed-corner check) and the bright right/bottom rim (213.3 -> 209.2
against 209.8).  MLT's bootstrap luminance is identical to six digits
(0.517736 in both builds, single-threaded) and its image moves at three
pixels by 3e-8 (its t = 1 share on a diffuse Cornell box is negligible).

**"BDPT/VCM CLI renders are not bit-reproducible run to run" is true only
MULTI-THREADED.**  With `force_number_of_threads 1`, PT, BDPT and VCM all
reproduce bit for bit across repeated runs of the SAME binary.  Under that
control, pre-fix vs post-fix images differ only at the film ring: interior
pixels differ by at most 6e-16 relative (floating-point summation-order
noise from the ring-adjacent code path, not a real change), and PT is
bit-identical pre vs post everywhere.  That is stronger evidence than the
multi-threaded non-reproducibility claim this paragraph used to lean on --
"interior untouched" is not an assumption, it is a measured 6e-16.

**The orthographic-camera guard-band edit is dead code for splats.**  BDPT
and VCM both skip the t = 1 strategy entirely for an orthographic camera
(every ray shares one direction, so there is no lens/aperture point for a
light-subpath vertex to connect toward), confirmed by rendering a
caustic-only floor under BDPT with an ortho camera in both builds: it
renders exactly 0.0 either way.  So `InRasterGuardBand`'s ortho branch is
exercised only by `CameraImportanceTest`'s direct projection round-trip
check, never by an actual splat -- the DL-294 fix's own reach does not
include the ortho topologies in `BDPTStrategyBalanceTest`/
`VCMStrategyBalanceTest`, and neither would a hypothetical ortho-specific
regression in that guard band.

## 7. Adjacent rows

- **DL-354** (BDPT small directly-visible emitter too dark at low
  resolution) is a different mechanism, unmoved: its emitter's image sits
  in the interior of the frame at every resolution the row quotes, and
  this fix changes only splats whose home pixel is on (or within the
  filter support of) the film border.  Measured on `sms_k2_flatslab`'s
  luminaire (100x75, box filter, OIDN off, window sum, n = 3): BDPT/PT
  window-sum ratio 0.578 pre-fix, **0.568** post-fix (PT 704.96 +/- 14.5,
  BDPT 407.2 +/- 9.3 pre / 400.3 +/- 3.3 post) -- the emitter's image sits
  at row 2, close to the top edge but outside the affected half-pixel
  strip, so the row needs no change.
- **DL-330** (BDPT 2-6 % low through a weave gap) does not move: the DL-05
  area closed-form row reads BDPT L/L0 0.29614 +/- 0.00282 pre and
  0.29521 +/- 0.00361 post (n = 10 seed bases each, t ~ 0.6).  Its fov-10
  frame is lit edge to edge, so the splat's MIS share there must be
  negligible (s = 0 through the gap carries it); DL-330 is a different
  mechanism.
- **DL-368** (the rasterizer's half-pixel sample placement vs the cameras'
  film offset) is left to its own row -- see section 4 for the
  rasterizer-side-vs-camera-side distinction on whether `NearestPixel`
  needs to move with it, and `SplatFilm::NearestPixel` /
  `BDPTCameraUtilities::InRasterGuardBand` are the two splat-side sites
  DL-368's own recipe should name.

## 8. Follow-ups: DL-368 (pixel convention) and DL-354 (the dead (1,1) strategy), 2026-10-02

Branch `debt-dl354`, from `master` `f03359223`.  Both rows left open for the
supervisor to strike after a fresh review.

### 8.1 DL-368 -- every rasterizer now samples the camera's nominal film

**Convention chosen: PBRT's.**  Pixel (x, image row y) covers screen
[x, x+1) x [H-1-y, H-y) -- the camera's nominal film [0, W) x [0, H) --
centre (x + 0.5, H - y - 0.5); in film coordinates (fx, fy = H - screen y)
it covers [x, x+1) x (y, y+1], centre (x + 0.5, y + 0.5).  The camera side
was left alone: every camera already translates screen space by
(-W/2, -H/2), so the optical axis sits on the corner shared by the four
centre pixels, and every analytic inverse (`Rasterize*`,
`RasterFromLensPoint`, the fisheye and orthographic maps) already returns
screen coordinates in that frame.  Moving the cameras instead would have
needed an ASYMMETRIC translation (x by -(W-1)/2, y by -(H+1)/2, because the
old rasterizer y mapping centred row y on H - y) in five cameras and five
inverses.

The convention lives in ONE place, `RasterConvention` in `ICamera.h`
(`kPixelCentre`, `PixelToScreen`, `PixelCentreToScreen`), and every
consumer reads it:

- eye-ray placement: PT pel / spectral (film mode and the no-filter
  fallback), pixelpel, pixelintegratingspectral (the `rasterize_pixel`
  single-ray entries too), BDPT pel / spectral, VCM pel / spectral and the
  VCM auto-radius pre-pass (`VCMRasterizerBase`), MLT pel / spectral (a
  uniform film sample now needs no offset at all), and the AOV guide pass
  (`AOVBuffers` -- which ALREADY sampled [x, x+1), i.e. its albedo / normal
  OIDN guides were half a pixel off the beauty pass before this change);
- filter warps: `IPixelFilter::warpOnScreen` now warps around the pixel
  CENTRE (x + 0.5, y + 0.5) of the lower-left corner it is handed, and every
  caller passes `(x, height - 1 - y)` (`PixelFilter`, `BoxPixelFilter`);
- reconstruction: `FilteredFilm::Splat` and `SplatFilm::SplatFiltered`
  measure filter offsets from (px + 0.5, py + 0.5), and
  `SplatFilm::NearestPixel` is now a floor (the pixel CONTAINING the
  position) -- moved in the same commit, as section 4 required.

Unchanged and already consistent: the camera projections and
`InRasterGuardBand` (convention-agnostic by construction),
`PixelBasedRasterizerHelper`'s whole-film timing samples
(`width * u`, already [0, W)), the scene editor's pick ray and world-to-
screen overlay projection (both continuous widget coordinates through the
camera's own matrix), ray differentials (`+1` pixel offsets).  The legacy
`PixelBasedSpectralIntegratingRasterizerRGB.cpp` is excluded from every
build and does not compile against the current filter interface; it is
not touched.  `MediumInsideOutsideInvariantTest`'s DL-286 MLT row
re-implements `MLTRasterizer::EvaluateSample`'s film mapping with the OLD
`- 0.5` offset; it compares two samplers through the same mapping, so it is
self-consistent and left alone (it is not a convention check).

**Red / green** (`tests/PixelCenterConventionTest.cpp`, new):

- Row A, the quadrant probe: four sub-pixel emitters touching the optical
  axis, one per quadrant, rendered through 11 rasterizers (PT, PT spectral,
  PT spectral HWSS, pixelpel, pixelintegratingspectral, BDPT, BDPT spectral
  HWSS, VCM, VCM spectral, MLT, MLT spectral) x 4 cameras (orthographic,
  pinhole, thin lens focused on the plane, fisheye).  Pre-fix every one of
  the 44 renders put ALL the energy in one pixel (block 0 / 0 / 0 / 1.0000);
  post-fix the 2 x 2 block around the axis holds 1.0000 of it, a quarter
  per pixel up to MC noise (worst block pixel 0.11 with 0.25 expected, MLT /
  VCM spectral at 64-256 spp).  The check is orientation-agnostic, so no
  camera's mirror convention can fake it.
- Row B, the row's own closed form: orthographic, view 1, 32 px, one small
  face-down quad at x = +2 / -2.  PT 0.9794 / 1.0209 and BDPT
  0.9794 / 1.0210 pre-fix; 1.0000 / 1.0000 for both post-fix.
- 48 checks: 0 / 48 on the reverted convention files, 48 / 0 fixed.

`CameraImportanceTest` Test 9 now builds its screen points with
`RasterConvention::PixelToScreen` -- the shared helper the rasterizers use
-- so it no longer carries a private copy of the convention (984 / 0).
`BDPTStrategyBalanceTest`'s `TestNarrowFovStripeGuard` stays the hit-vs-
splat guard, with its floor edge moved from world x = 0 to x = 0.00085
(half a pixel at the floor): the optical axis is now a pixel BOUNDARY, so
an edge on it split no pixel and PT, BDPT and VCM all read 0.0000 at the
transition column (the fixture-sanity check failed with all three in
agreement).  With the edge mid-pixel: PT 0.4990, BDPT splat 0.4984, VCM
splat 0.4986.

`BDPTStrategyBalanceTest`'s DL-381 rows compare against an independent MC
that bins photons by RISE's pixel centres; with the old centres the gate
read PT sphere +9.5 % and BDPT sphere +8.6 % (bands 4 %).  The tool
(`tools/DL381RandomWalkReciprocityMC.cpp`) now uses the new centres and
its constants were re-run (DL381_RANDOM_WALK_RECIPROCITY.md, "DL-368
re-baseline"); 9 / 0.

**What moves.**  Every render's image shifts by half a pixel right and half
a pixel down relative to the camera (equivalently: the camera now sits
where its parameters say).  Means are unchanged wherever the scene is
symmetric about the film or uniform near its edges; a lone off-axis feature
near the frame moves by its gradient times half a pixel (row B).  No stored
reference image needed regenerating (section 8.3).

### 8.2 DL-354 -- BDPT never evaluated the (1,1) strategy it MIS-weighted against

Re-measured on `f03359223` first (sms_k2_flatslab's luminaire, box filter,
OIDN off, window energy in 100 x 75 units, 64 spp, n = 3 salted): PT
668.5 / 684.7 / 687.6, BDPT 400.4 / 654.9 / 685.8 at 100 x 75 / 200 x 150 /
400 x 300 -- BDPT/PT **0.599 / 0.957 / 0.997**.  Not gone, and not DL-294's
(section 7 already measured it unmoved).  Re-aiming the camera so the
luminaire sits at frame centre made it WORSE (PT 560, BDPT 261, 0.465), so
it is not an edge effect either.

**Root cause.**  `ConnectAndEvaluateImplCore` tests `s == 1` before
`t == 1`, so (s, t) = (1, 1) -- the light vertex ON the emitter connected
straight to the camera -- entered the NEE case, found `eyeVerts[0]` (the
CAMERA) failing its surface / medium check, and returned invalid.  The
t == 1 case's LIGHT-vertex branch, written for exactly this strategy, was
unreachable.  `MISWeight(0, 2)` (the camera ray hitting the emitter)
nevertheless counted (1,1) in its denominator, so a directly visible
emitter lost the share `r^2 / (1 + r^2)`, r = (light area density) /
(per-pixel camera density at the emitter point).  The camera density is
PER PIXEL (`PdfDirection` = d^2 / (A_pixel cos^3)), so r falls as
1 / (W H): 40 % lost at 100 x 75 on a 0.08-unit luminaire, 4 % at
200 x 150, 0.3 % at 400 x 300 -- the measured curve.  Per-strategy
instrumentation (temporary, reverted) confirmed it directly: the t == 1
layer deposited exactly 0 in the emitter's window, and the s == 0 layer
alone reproduced BDPT's 400.

**Fix.**  The NEE case is entered only for `t >= 2`, so (1,1) reaches the
t == 1 case.  That branch had never run, and it lacked the connection's
medium transmittance the s == 0 strategy carries (a fogged view of a
visible emitter would have read high by the (1,1) share): it now applies
`EvalConnTr` from the light vertex, whose root now records its enclosing
medium (`GetCurrentMediumWithObject` after `SeedFromPoint`, the role a
surface vertex's medium fields play for s >= 2).  Delta lights are
unaffected (their root vertex is not connectible, the branch returns), and
so are environment roots (the branch returns for them, as before).  VCM is
separate code (its splat skips the light root, and the row measured it at
PT's level; 702 +/- 46 here, n = 3, 64 spp).

Measured (window energy, n = 3 salted): pure light tracing of the
luminaire (t == 1 only, weight 1) reads 687.98 with sd 0.0005 at all three
resolutions; BDPT 687.4 / 692.2 / 688.6; BDPT spectral `hwss FALSE`
419 -> 675 (n = 4, 256 spp), `hwss TRUE` 395 -> 686 (256 spp, sd 1.2 %),
against a real PT SPECTRAL render of 688.3 (sd 1.0 %, n = 4, 4096 spp).
(Review correction, 2026-10-02: the first revision quoted "PT spectral
689" from a probe whose "pts" kind fell through to the PT PEL rasterizer,
because `"pt"` is a prefix of `"pts"`; the pel figure happened to agree.)
MLT shares BDPT's strategy dispatch, so it is fixed by construction, but
its window energy on a few-pixel feature is not a usable measurement: one
unsalted render reads 286 (pre) and 609 (post) at 256 mutations / pixel,
and 241 (post) at 1024 -- chain allocation noise, not resolved here.

`BDPTStrategyBalanceTest --dl354-only` (`TestSmallVisibleEmitterResolution
Sweep`, also in the full suite): BDPT/PT 100 x 75 (n = 6) and 200 x 150
(n = 3), and BDPT `hwss TRUE` (256 spp) / PT SPECTRAL (4096 spp) at
100 x 75 (n = 4), bands 3 / 2.5 / 5 % (each >= 4.4 measured ratio sd; PT
spectral's per-render sd read 1.0 % and 2.7 % in two salt sets -- the hero
wavelength gives a tiny emitter a heavy tail).  Red on the reverted
integrator: 0.5753 / 0.9554 / 0.5826 (0 / 3); green 0.9873 / 0.9987 /
1.0074 (3 / 0).

**Review P3 (2026-10-02).**  The revived (1,1) branch now rejects an
environment-light root before the raster projection and the visibility
ray (the LIGHT branch already returned for it, after both).

**Cost.**  One more connection per BDPT sample (a raster projection and a
shadow ray when the light root is a luminary).  `cornellbox_bdpt` at
256 x 256, 64 spp, OIDN off, interleaved n = 3: user CPU 115.7 s before,
110.9 s after -- no resolvable cost.

**Not changed: the per-pixel camera density in the MIS ratios.**  BDPT's
MIS uses the per-pixel camera pdf on both sides of every ratio, so its
weights partition to one and the estimator is unbiased; the multi-sample
balance-heuristic optimum for one light subpath per eye sample uses the
whole-film density (PBRT; SmallVCM divides by the light path count), which
would give the light-tracing strategies W H times more weight.  That is a
variance question across every BDPT render, recorded as DL-402, not done
here (done since: section 9).

### 8.3 Stored references

`CstDeriveGoldenTest` compares derived scene state, not images: 457 MATCH /
0 DRIFT.  `AgentEvalCheckTest` read 2071 / 4 after the shift
(`image_reconstruct_multi` view2 / view3 RMSE 0.0133 / 0.0153 against a
0.012 max) and 2075 / 0 with only the convention files reverted, so the
shift was the sole cause; the four `evals/references` PNGs were
regenerated with `generate_references.sh` (2075 / 0).  The showcase suites
(Pavilion 57/0, ShelfBunny 63/0, TidalStones 122/0), AgentEvalReplay 272/0
and AgentEvalLiveTransport 452/0 needed nothing.

## 9. DL-402 -- the camera density in BDPT's MIS ratios is the whole-film one (2026-10-09)

**Change.**  BDPT/MLT trace ONE light subpath per eye sample and splat
its t == 1 connections anywhere on the film, so per pixel the
light-tracing strategies draw W H times as many samples as the eye
strategies.  The multi-sample balance / power heuristic (Veach 9.2)
therefore weights with the WHOLE-FILM camera density, per-pixel / (W H)
-- PBRT-v3/v4's `PdfWe` (film area, splats scaled by 1/spp) and
SmallVCM's division by the light-path count, which RISE's VCM already
applies in `InitCamera`.  `BDPTCameraUtilities::PdfDirectionMIS` returns
`PdfDirection / (W H)` (unscaled for a delta-direction orthographic
camera, which has no t == 1 strategy), and it is now the ONLY camera
density in BDPT's MIS: the first eye vertex's `pdfFwd`
(`GenerateEyeSubpathImpl`'s initial `pdfFwdPrev`) and every t == 1
connection's light-endpoint `pdfRev` (the s == 0 light-endpoint, the
(1,1) light-root and the general t == 1 cases, RGB and NM).  Because both
sides of every ratio change together the partition stays exact.
Contributions (`Importance`) and VCM's `emissionPdfW` keep the per-pixel
value.  MLT runs the same `BDPTIntegrator` and moves with it.  `MISWeight`
itself is untouched.

**Variance** (salted K-trial, both modes in one binary via a temporary
toggle, interleaved per salt; per-pixel variance summed over the image
divided by the summed squared mean; box filter, OIDN off; 128 x 128
unless noted, 8 spp):

| scene | K | old relVar | new relVar | old / new | t/render old -> new |
|---|---|---|---|---|---|
| DL-354 flat slab + small visible emitter, 100 x 75, 16 spp | 64 | 0.0808 | 0.0227 | 3.6x | 0.10 -> 0.11 s |
| same, 200 x 150, 16 spp | 64 | 0.0452 | 0.0134 | 3.4x | 0.24 -> 0.24 s |
| `cornellbox_bdpt_caustics` (glass sphere) | 24 | 0.00999 | 0.00384 | 2.6x | 0.24 -> 0.25 s |
| `cornellbox_bdpt` (diffuse) | 24 | 0.00261 | 0.00192 | 1.35x | 0.25 -> 0.26 s |
| `cornellbox_bdpt_glossy` | 24 | 0.00274 | 0.00171 | 1.6x | 0.24 -> 0.23 s |
| `cornellbox_bdpt_thinlens` | 24 | 0.1030 | 0.0826 | 1.25x | 0.26 -> 0.26 s |
| `cornellbox_bdpt_pointlight` | 24 | 0.00618 | 0.00534 | 1.16x | 0.35 -> 0.35 s |

No scene regressed; cost is unchanged, so the spp-matched figures are
also time-matched.  The DL-354 suite's BDPT window energy now reads
687.98 with sd 0.00 at 200 x 150 -- the small emitter is carried almost
entirely by light tracing, as it should be.

**Means.**  The image means agree within noise everywhere except the
point-light Cornell box at its scene `max_eye_depth 8 / max_light_depth
8`: new - old = -0.34 % (paired z = -17, concentrated around the tall
block, -3 % in one grid cell).  That is the depth-cap partition (DL-351),
not this change: at depth 30 old / new / PT read 0.60542 / 0.60535 /
0.60558 image means and agree cell by cell.  With caps, `MISWeight`
counts strategies the capped walks cannot generate, and how much of a
long path's weight lands on those phantom strategies depends on the
heuristic's inputs -- moving weight toward t == 1 moves the (pre-existing)
truncation loss.  It disappears when DL-351 lands.

**Suites.**  BDPTStrategyBalanceTest 370/0, VCMStrategyBalanceTest 165/0,
PixelCenterConventionTest 48/0, CameraImportanceTest 984/0,
BDPTSeeThroughMISPartitionTest 8/0, SpectralSplatIntegralNormalizationTest
21/0, WeaveGapShadowTransmittanceTest 281/0, CstDeriveGoldenTest 459 MATCH.
AnimationRasterizerParityTest reads 101/1: its MLT-spectral D row gates ONE
deterministic MLT realization (the per-render salts do not reach MLT's
chain RNG).  Varying only `bootstrap_samples` 20000..20010, the pre-change
build reads mean 0.940 and fails 8 of 11; this change reads mean 0.964
(0.924 .. 1.008), but its realization at the shipped 20000 is 0.924.
Recorded as DL-468, not widened here.
