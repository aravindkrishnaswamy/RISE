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
