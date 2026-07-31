# Integrator Bug-Fix Findings (Phase-1 matrix by-products)

**Date:** 2026-06-03
**Scope:** The three integrator anomalies surfaced as by-products of the
Phase-1 measurement ([UNIFIED_INTEGRATOR_BASELINES.md](UNIFIED_INTEGRATOR_BASELINES.md)
§6/§7/§9): `glass_pavilion` Inf fireflies (Bug 1), `sculptors_studio` BDPT
near-black (Bug 2), `prism_dispersion` spectral-BDPT −36 % (Bug 3).
**Status:** Bug 1 FIXED (both halves at the EXR layer; integrators are correct).
BDPT was an FP16 EXR-*write* overflow, FIXED at the writer. The VCM "Inf"
(claimed a "GENUINE integrator bug" in an intermediate note) was **re-diagnosed
2026-06-03 as the read-side twin: `EXRReader` read FLOAT EXRs via `Imf::Rgba`
(FP16), overflowing finite fireflies > 65504 to `+Inf` on READ** — NOT an
integrator defect. FIXED at the reader (`EXRReader` now reads FLOAT). See §1's
final "CORRECTED AGAIN" note for the evidence (pyOpenEXR shows the EXRs are
finite; synthetic `R=100000` reads `inf` via `EXRReader`, finite via pyOpenEXR;
deterministic sweep 8/16 "inf" → 0/16 after the fix). Bug 2 FIXED (working tree,
pending commit). **Bug 3 FIXED 2026-06-04** — three root causes in the SHARED
spectral-HWSS companion path (`RecomputeSubpathThroughputNM` direction flip +
`EvalEmitterRadiance<NMTag>` missing `pLuminary` fallback + `totalActive`
terminated-companion over-count); restores the BDPT **and** VCM HWSS bundle
invariant (uniform + dispersive) and also closes the documented HWSS env-IBL
deficit; one separate **PT** HWSS env bias remains open (out of scope). See §3.
**Discipline:** every claim below is from instrumented renders, not intuition.
All changes are uncommitted for review.

---

## Bug 1 — `glass_pavilion` "Inf firefly" — **BOTH halves are FP16 EXR artifacts (integrators correct): write-side overflow (FIXED at `EXRWriter`) + read-side overflow (FIXED at `EXRReader`). The "GENUINE VCM integrator Inf" verdict was a misdiagnosis — see the final "CORRECTED AGAIN" note in this section.**

### Confirmed real?
Yes — but **not** the hypothesized degenerate-pdf 1/0 in a caustic connection,
and **not an integrator bug at all**. The integrator (PT/BDPT/VCM) is correct.

### Root cause (instrumented, definitive)
The `Inf` pixels are **red-channel-only with normal G,B** (e.g. measured
`(216,125) → R=Inf, G=9.95, B=2.95`). That pattern is impossible from a
multiplicative firefly (which scales all channels) and impossible from a scalar
1/0 (which hits all channels). A non-finite trap placed at **every** integrator
strategy (S0 / NEE / interior / merge / splat), at subpath-generation
throughput, and at the eye+splat film resolve **never fired** even at a 1e30
threshold — yet the EXR still contained `Inf`.

A trap at the **EXR write boundary** (`EXRWriter::WriteColor`) caught it:
```
WRITECOLOR px=(251,181) c=(151722, 9.20, 3.74)
```
`c.base.r = 151722` is a **finite double** — a legitimate (heavy) caustic
firefly, real Monte-Carlo noise for this pathological dielectric-caustic scene
(the matrix itself records PT σ/μ = 6099 % here). The EXR is written as
**`HALF` (FP16, max 65504)** via `Imf::RgbaOutputFile`, so any pixel above 65504
in a channel is stored as **`+Inf`**. Red crosses 65504 first (warm lights +
bronze), giving the red-only signature. Every measured finite max sat just under
65504 (≈5e4), the FP16 ceiling — the tell.

**The `bpp 32` requested by the measurement harness was silently ignored** —
`EXRWriter` hardcoded the half-float `Imf::Rgba` path and never received a bit
depth. So RISE's EXR output is lossy for *any* HDR pixel > 65504 (caustics,
specular highlights, bright emitters in linear radiance), not just this scene.

Why it appears "shared between BDPT and VCM" (the §9 hint): both produce heavy
finite fireflies on this scene; both write through the same FP16 EXR path. PT is
rarer-Inf only because its fireflies less often exceed 65504 at this SPP.

### Fix (writer layer — NOT integrator code)
Honor `bpp ≥ 32` → write **32-bit FLOAT** EXR channels (`Imf::OutputFile` with
explicit `FLOAT` `R/G/B[/A]`), preserving the full linear range. `bpp < 32`
(the default 8, the common 16) keeps the historical half output **byte-for-byte**.

- [src/Library/RasterImages/EXRWriter.h](../src/Library/RasterImages/EXRWriter.h) — `write_float` member, interleaved float buffer, `Imf::OutputFile*`, branched `WriteColorToEXRBuffer`, new ctor param (default `false`).
- [src/Library/RasterImages/EXRWriter.cpp](../src/Library/RasterImages/EXRWriter.cpp) — `BeginWrite`/`EndWrite`/dtor float branches (`Imf::OutputFile` + `FrameBuffer`/`Slice`).
- [src/Library/RISE_API.cpp](../src/Library/RISE_API.cpp) / [.h](../src/Library/RISE_API.h) — **ABI-safe 6-arg overload** `RISE_API_CreateEXRWriter(..., write_float)`; the 5-arg form is unchanged (so out-of-tree callers and `HDRRoundTripTest`/`FrameEncoderTest` keep the half path).
- [src/Library/Rendering/FrameEncoders.cpp](../src/Library/Rendering/FrameEncoders.cpp) — `EXRFrameEncoder::CreateWriter` passes `opts.bpp >= 32`.

### Verification
- 8 fresh `glass_pavilion` VCM renders (bpp 32) → **inf=0** on all; fireflies up
  to **624997** (10× the FP16 max) now stored as finite floats. EXR channel type
  confirmed `FLOAT` (was `HALF`).
- No-regression: the byte-identity contract (`FrameEncoderTest`),
  `HDRRoundTripTest`, `FileRasterizerOutputShimTest` all use `bpp < 32` → half
  path unchanged → **pass** (full-suite result below).

### Sibling audit (audit-by-bug-pattern)
The pattern is "EXR output is FP16-only regardless of requested bit depth." Sole
producer is `EXRWriter` (one site); all EXR output flows through it, so the fix
is global. The harness's `prism_dispersion`/`spectral_caustic` EXRs share the
same path and equally benefit. No integrator twins to audit (not an integrator
bug).

### Note for the matrix — CORRECTED 2026-06-03 (independent K=16 re-measure)

A re-measure with the FLOAT-EXR writer splits `glass_pavilion` `Inf⚠` into **two
distinct phenomena**. The single-cause "FP16 artifact, not an integrator defect"
diagnosis above holds for **BDPT** but is **WRONG for VCM**:

- **BDPT — FP16 artifact, FIXED.** Re-measure is finite (`mean_var` 1.10e3,
  `lum` 1.096), no Inf across 16 renders. The half-float-overflow diagnosis was
  correct here.
- **VCM — a GENUINE Inf firefly; a real integrator bug, NOT an FP16 artifact.**
  The FLOAT EXR (max ~3.4e38) still records an actual red-channel `Inf` in **9 of
  16** re-measure renders. This is exactly the "degenerate-pdf firefly in a VCM
  caustic connection" hypothesis — REAL for VCM, **missed by the 8-render
  spot-check above**, which happened to sample only finite-big fireflies (max
  624997). It is **not** a dead-guard symptom: an A/B of flag-off (guards DCE'd)
  vs flag-on (`-fno-finite-math-only`, guards live) shows the Inf at the same
  rate (6/10 vs 8/10), so it is an *unguarded* unbounded contribution that needs
  a degenerate-pdf guard/clamp at its source in the VCM connection/merge code.
  **OPEN — a real fix (apply `docs/skills/sms-firefly-diagnosis.md` +
  `precision-fix-the-formulation.md`); do NOT just clamp the symptom.** The
  earlier "8 renders → inf=0" claim was a sampling fluke, not a clean bill.

### Note for the matrix — CORRECTED AGAIN 2026-06-03 (read-side FP16): the VCM "Inf" is an **EXRReader artifact, NOT an integrator bug** — FIXED at the reader

The "GENUINE integrator Inf" verdict immediately above is **WRONG**. It trusted
`HDRVarianceTest`'s `inf` report — but `HDRVarianceTest` reads the EXR through
**RISE's own `EXRReader`, which was still reading via `Imf::RgbaInputFile`
(half / FP16)**. Reading a 32-bit FLOAT EXR through the half path makes OpenEXR
convert every channel to `half` on read, so any value **> 65504 overflows to
`+Inf` on READ** — regardless of the (now-fixed) FLOAT *writer*. So the writer
fix alone did NOT close the round-trip: the reader re-clipped it.

**The integrator is correct. It produces FINITE caustic fireflies** (heavy but
bounded MC variance on this pathological dielectric scene — the matrix records
PT σ/μ = 6099 % here). Decisive evidence:

- **Direct EXR inspection (pyOpenEXR, independent of RISE):** every
  `glass_pavilion` VCM render that `HDRVarianceTest` flagged `inf` has **0
  non-finite pixels**; the red channel maxes at a finite **40 700 – 485 496**
  across 16 renders. A finite mean of those (≈ 6.6–13.9) is impossible to be Inf.
- **The red-only signature is explained:** the warm-light caustic makes red the
  largest channel, so red is the one that crosses 65504 first; G,B stay < 65504
  and read back finite. It was never a per-channel integrator term — it is the
  reader's per-channel half overflow.
- **Synthetic isolation (no integrator involved):** a hand-written FLOAT EXR with
  one pixel `R = 100000` reads back as `100000` in pyOpenEXR but as **`inf`** via
  RISE's `EXRReader`. After the fix it reads `100000`.
- **The flag A/B "same rate (6/10 vs 8/10)" corroborates this:** the flag toggles
  integrator `isfinite` guards, which are irrelevant to the *reader's* half
  overflow — so of course the rate is unchanged. It is not an unguarded
  integrator contribution; it is a read-side conversion.

**Root cause:** [`EXRReader.cpp`](../src/Library/RasterImages/EXRReader.cpp) used
`Imf::RgbaInputFile` + `Imf::Array2D<Imf::Rgba>` (FP16). This is the **read-side
twin** of the write-side FP16 bug the prior chip fixed in `EXRWriter`.

**Fix (right layer = the reader; NO clamp, NO integrator change):** read via the
general `Imf::InputFile` + a FLOAT `Imf::FrameBuffer` (interleaved R,G,B,A float
slices, missing-channel fill 0/1), mirroring the committed `EXRWriter` FLOAT
path. Half-stored EXRs convert half→float losslessly; FLOAT EXRs read at full
range. `EXRReader.{h,cpp}` only — `git diff` touches nothing else, so **every
integrator/render path is byte-identical to HEAD and energy is preserved by
construction** (the caustic firefly is finite and intact; nothing was clamped).
This also closes a **latent production bug**: an EXR HDRI with a value > 65504
(e.g. a bright sun) previously read as `Inf`, corrupting env lighting.

**Verification:** deterministic block-seed sweep (single-thread, seeds 1–16) went
from **8/16 `HDRVarianceTest` "inf" → 0/16** with the fix; the same renders that
read `inf` now read finite means (6.6–13.9) with finite red maxima up to 485 496
(all 0 non-finite in pyOpenEXR). Synthetic `R=100000` EXR: `inf → 1563.48`.
seed-1 capture: `inf → 10.15`. Sole EXR-read path (`EXRReader`) → fix is global;
`FrameEncoderTest` (writer byte-identity) unaffected; `HDRRoundTripTest`
(half-written values ≤ 2000) reads identically half→float.
**Full gate (clean warning-free rebuild of `make all` + `make tests`):
`./run_all_tests.sh` → 116 passed / 0 failed / 0 skipped; `EnvLightBalanceTest`
standalone → Passed 80 / Failed 0 (lax 80/80); `VCMStrategyBalanceTest` → PASS;
the three EXR-file tests (`HDRRoundTripTest`, `FileRasterizerOutputShimTest`,
`FrameEncoderTest`) → PASS.** No-regression on rendering is by construction:
`git diff` is `EXRReader.{h,cpp}` ONLY, so every integrator / rasterizer path is
byte-identical to HEAD — no scene's radiance can shift; the reader change only
affects how an EXR is read back (now correct for values > 65504).
**Real-scene (full-res 800×600, VCM+EXR copy of `glass_pavilion`), 16 renders →
0/16 `HDRVarianceTest` "inf"**, finite red maxima up to **10 757 707** (10.7 M),
all 0 non-finite — pre-fix every one (all > 65504) read as `inf`. The real scene
ships with a `pixelpel_rasterizer` + PNG bpp 8 output (tonemapped, so the Inf is
not observable there); the VCM+EXR config that exhibits it is exactly the
`var_test/scenes/glass_pavilion_vcm.RISEscene` template. **No-regression VCM
scenes** (`pool_caustics`, `diamond_teapot`, `torus_chain`, `spectral_caustic`,
`gi_spheres`, `cloister`) re-rendered: all finite, sensible means, 0 non-finite
(integrator byte-identical → unchanged). The mean-R variance across renders is
the scene's documented heavy caustic firefly noise (PT σ/μ = 6099 %), now read
correctly instead of overflowing to Inf — it is **not** clamped away, confirming
energy preservation.

**Meta-lesson:** do not trust a measurement tool's `inf`/`nan` verdict when that
tool reads through the component under suspicion. Cross-check with an independent
reader (pyOpenEXR here). Two sessions chased a non-existent VCM degenerate-pdf
Inf because both trusted `HDRVarianceTest`, which routed through the buggy
`EXRReader`.

---

## Bug 2 — `sculptors_studio` BDPT near-black — **REAL BDPT bug: orthographic camera is delta-direction and was mishandled in MIS. FIXED.**

### Confirmed real?
Yes. Reproduced: PT lum **0.18818**, BDPT lum **0.00227** (1.2 % of PT, −98.8 %),
matching the matrix. VCM is fine (0.1755).

### Root cause (instrumented, definitive)
The culprit is the **orthographic camera**, not the spot lights. Decisive test —
swap the orthographic camera for a perspective (thin-lens) camera (PT and BDPT
both): BDPT recovers to **0.319** vs PT **0.238** (no near-black). So BDPT-on-
orthographic is the failure.

Per-(s,t) strategy instrumentation shows the mechanism — the MIS weights are
**exactly inverted** vs the working perspective case:

| strategy | Perspective (works) | Orthographic (broken) |
|---|---|---|
| s=1 t=2 (eye NEE, direct light) | meanMis **0.99966** ✓ | meanMis **0.0107** ✗ |
| s≥2 t=1 (light→camera splat) | meanMis **5.7e-05** ✓ | meanMis **0.999** ✗ |

An orthographic camera emits a **single parallel direction per pixel** — a Dirac
delta in *direction*, the importance-side analogue of a directional light. The
t=1 light-tracing strategy (a non-specular light vertex scattering into the
camera) therefore has **zero density**. But `PdfDirectionOrthographic` returns a
finite `1/A_image` (not a delta), and the camera vertex was hardcoded
`isDelta=false`, so BDPT (a) kept the phantom t=1 strategy in the MIS denominator
— giving it weight ≈0.999 and crushing the eye-path NEE to ≈0.01 — and (b)
splatted t=1 with a geometrically meaningless `G = cos/dist²` (the orthographic
"camera position" is not where the parallel rays converge). Net: the working
eye-path strategies are MIS-suppressed to 1 %, and the dominant t=1 splat carries
wrong-magnitude energy → near-black.

This is the camera-side twin of the documented **delta-light vs delta-surface
trap** ([skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md)).

### Fix (right layer: recognize the delta-direction camera, mirror the delta-light treatment)
1. New `BDPTCameraUtilities::IsDeltaDirection(cam)` → `true` for `OrthographicCamera`, `false` for pinhole/thin-lens/fisheye ([CameraUtilities.h](../src/Library/Cameras/CameraUtilities.h) / [.cpp](../src/Library/Cameras/CameraUtilities.cpp)).
2. `GenerateEyeSubpath` marks the camera vertex `isDelta = IsDeltaDirection(cam)` (was hardcoded `false`) — so `MISWeight`'s eye-side walk's existing `eyeVerts[j-1].isDelta → continue` gate at j==1 **excludes the phantom t=1 term** from every other strategy's denominator, restoring full weight to the eye-path strategies ([BDPTIntegrator.cpp:~1486](../src/Library/Shaders/BDPTIntegrator.cpp)).
3. The t=1 connection (`ConnectAndEvaluateImpl`, `if(t==1)`) early-returns invalid when the camera is delta-direction — no misdirected splat, no double-count ([BDPTIntegrator.cpp:~3723](../src/Library/Shaders/BDPTIntegrator.cpp)).

The skip is consistent across all strategies (the same `isDelta` gate), so the
partition of unity holds — the remaining strategies (s=0, s=1 NEE, interior)
re-sum to 1 without t=1, exactly as the delta-light fix does on the light side.

### Verification
- **PT-vs-BDPT (orthographic):** BDPT **0.17772** vs PT **0.18818** → within
  **5.6 %** (was 1.2 %). The residual is MC noise (PT σ/μ = 5373 % on this
  spot-lit scene). s=1 NEE meanMis recovered to **0.99921**; t=1 rows gone.
- **No-regression (perspective, non-ortho):** BDPT **0.31906** (was 0.31990 pre-
  fix) — unchanged. The fix triggers *only* for orthographic cameras.
- **VCM (shares `GenerateEyeSubpath`):** **0.17550** (matrix 0.1742) — unchanged;
  the camera-`isDelta` change does not perturb VCM.

### Sibling audit (audit-by-bug-pattern)
Pattern: "a delta-DIRECTION camera (orthographic) treated as finite-direction in
the t=1 light-tracing strategy + MIS."
- **VCM** has its own t=1 splat (`SplatLightSubpathToCamera`) and balance-
  heuristic MIS. Empirically VCM is **correct** on the orthographic scene (0.175 ≈
  PT, conv ✓, before and after this change) — its balance-heuristic weights do
  not invert the way BDPT's power heuristic does. Left untouched (working code;
  changing it risks a real regression). **Flagged for a future check** that VCM's
  ortho t=1 weight/geometry is *robust* rather than merely lucky.
- **Other cameras** (pinhole / thin-lens / fisheye) are finite-direction →
  `IsDeltaDirection` false → t=1 remains valid; confirmed unchanged.
- Orthographic is the only delta-direction camera and the only scene using it in
  the corpus, so blast radius is minimal.

### Regression test ADDED 2026-06-03; residual RESOLVED 2026-06-04 — it was an alpha-convention MEASUREMENT artifact, not a transport bias, and not ortho-specific
`TestOrthographicCamera()` (orthographic camera + mesh emitter, PT-vs-BDPT) was
added to `tests/BDPTStrategyBalanceTest.cpp`. It confirmed the near-black fix but
surfaced a **~10% BDPT-vs-PT MEAN deficit** (in-harness, 32×32/32 spp): raw RGB
mean PT 0.0407 vs BDPT 0.0367 (−9.6%), with median/p99/max agreeing — flagged
OPEN as a possible delta-direction-camera MIS imperfection.

**Root cause (2026-06-04, instrumented — NOT an MIS/transport bug, NOT
ortho-specific): a film-resolve ALPHA-CONVENTION difference between the two
rasterizers, surfaced by comparing UNPREMULTIPLIED RGB on the first test scene
that has a visible silhouette.** Decisive evidence (per-pixel in-harness
diagnostic, box filter on BOTH integrators to remove the gaussian-vs-box
confounder — which is itself only ~0.06%):

| quantity (BDPT/PT, box filter) | ORTHO | PINHOLE fov45 (same silhouette, NON-delta camera) |
|---|---|---|
| raw RGB-sum (`base`, what the test compared) | **0.905** | **0.927** |
| composited radiance `base × alpha` (over black) | **0.9997** | **0.9985** |
| alpha-sum (1024 px) | PT 656 (=0.64·1024, coverage), BDPT 1024 (all α=1) | PT 487, BDPT 1024 |

Reading the table:
1. **The composited radiance — the per-pixel value the sensor integrates and the
   beauty shows — agrees to <0.2%.** The transport (MIS, per-(s,t) strategies) is
   correct; interior full-coverage pixels match to <1% everywhere (the spatial
   diff is a clean `>0.97`-ratio block with a thin `<0.5`-ratio silhouette ring).
2. **The deficit lives entirely in the unpremultiplied-RGB convention at
   partial-coverage silhouette pixels.** `PixelBasedPelRasterizer` (PT)
   increments `alphaSum` on a surface HIT only ([PixelBasedPelRasterizer.cpp](../src/Library/Rendering/PixelBasedPelRasterizer.cpp) ~662)
   and `weightSum` on every sample, so [`ProgressiveFilm::Resolve`](../src/Library/Rendering/ProgressiveFilm.h) (~136) yields
   RGB = `colorSum/alphaSum` = the *unpremultiplied* surface radiance L and
   alpha = `alphaSum/weightSum` = coverage c. `BDPTPelRasterizer`
   ([BDPTPelRasterizer.cpp](../src/Library/Rendering/BDPTPelRasterizer.cpp) ~415) increments `alphaSum` on EVERY sample, so
   it yields RGB = c·L (coverage baked in) and alpha = 1. Compositing over the
   black background (`base × alpha`) gives c·L for BOTH (L·c vs c·L·1) — the
   rendered image is identical; only the straight-alpha vs baked-alpha
   intermediate differs.
3. **It is NOT delta-direction-camera-specific.** A pinhole camera framed so the
   2×2 quad does NOT fill the frame (fov 45) shows the SAME deficit (0.927). The
   ortho scene merely exposed it first: the three pre-existing topologies put the
   quad OUTSIDE the frame on all sides (quad fills the view → every pixel is
   full-coverage → alpha = 1 → `base × alpha = base`), so they never had a
   partial-coverage pixel to diverge on.
4. **It is bias-shaped, not variance** (4× spp 32→128 leaves it at 0.904→0.908) —
   exactly because it is a deterministic convention difference, not noise.

**Fix (right layer = the test's COMPARISON, not the integrator): compare the
convention-independent composited radiance `base × alpha`** — the radiance the
sensor measures — instead of the unpremultiplied surface RGB. `ComputeStats` now
multiplies each channel by the coverage alpha before mean/median/p99/max. This is
a **no-op for the three full-coverage topologies** (alpha = 1 there; their
numbers are unchanged within MC noise) and makes the ortho comparison measure
what the two integrators must actually agree on. **No integrator code changed —
`git diff` touches only [`tests/BDPTStrategyBalanceTest.cpp`](../tests/BDPTStrategyBalanceTest.cpp) + this doc; every
`src/**` path is byte-identical to HEAD, so no scene's radiance can shift.**

**Tolerance change:** `TestOrthographicCamera` now uses the **strict**
`{meanTol 0.08, p99 0.25, max 1.00}` (was the near-black-guard `0.20`). The
near-black regression the `IsDeltaDirection` fix resolved (pre-fix BDPT 1.2% of
PT) is still caught with a >10× margin (composited near-black is still near-black).

**Verification (composited comparison, in-harness):** ortho PT mean 0.03681 vs
BDPT 0.03675 (−0.2%), median/p99/max agree; **all four topologies pass at strict
0.08 (24/24 checks)**. The three pre-existing topologies are unchanged within MC
noise. Full suite 116/116; EnvLightBalanceTest 80/80 lax; warning-free clean
rebuild.

**Flagged separate (minor, NOT a transport bug):** BDPT reports `alpha = 1` for
partial-coverage pixels (it loses the coverage matte) where PT reports the true
coverage. This only matters for compositing a BDPT render over a NON-black
background or exporting a matte — the over-black beauty is correct. Aligning
BDPT's alpha to PT's coverage convention would mean threading a hit/coverage
signal out of `BDPTPelRasterizer::IntegratePixelRGB`, handling splat-only pixels
(eye-miss + light-tracing splat), the denoiser AOV alpha, and the shared VCM
path — a broader rasterizer-film change than this residual warrants. Left for a
future matte-correctness pass.

---

## Bug 3 — `prism_dispersion` spectral-BDPT −36 % — **REAL HWSS-companion bug. FIXED 2026-06-04 (3 root causes, all in the SHARED spectral-HWSS companion path). NOT the feared multi-week per-wavelength path-split.**

### Confirmed real?
Yes. Reproduced: dispersive spectral-PT lum **~1.243**, spectral-BDPT lum
**~0.796** (64 % of PT, −36 %), matching the matrix. The deficit is **entirely
HWSS-specific and BDPT-specific** with two additive components (general +
dispersion), confirmed by the hwss / dispersion discriminators below.

### Diagnosis (instrumented bisection, not intuition)
The discriminator that localised the bug: in HWSS the **hero** is uniform over
[λ_lo, λ_hi], so a **hero-only** estimator (skip companions, normalise by hero
count) is itself unbiased and must equal hwss=false. An env-gated hero-only build
gave **1.196 ≈ hwss=false 1.187** → the hero is correct, **the entire deficit is
in the COMPANIONS**. A second bisection (evaluate companions at the HERO
wavelength, so the throughput-recompute is mathematically identity) gave **1.018,
NOT 1.196** — i.e. *calling* `RecomputeSubpathThroughputNM` lost ~19 % **even
when it should be a no-op**. Direct instrumentation showed it was driving
`cumulativeRatio → 0` (zeroing throughput) at heroNM==companionNM, which is
impossible unless a re-evaluated BSDF / emitter returns ~0 and trips the
`heroF/heroLe ≤ NEARZERO` guard.

### Root causes (three, all in the shared HWSS-companion machinery used by BDPT **and** VCM spectral)

1. **`RecomputeSubpathThroughputNM` companion-direction flip** — the dominant
   share of the general component.
   [BDPTIntegrator.cpp](../src/Library/Shaders/BDPTIntegrator.cpp) Phase-3
   reconstructed `dirFromPrev = mkVector3(v.position, verts[i-1].position)` =
   `v − prev` (the *travel* direction, pointing toward the surface) and passed it
   as `wo` (eye subpath) / `wi` (light subpath). But `EvalBSDFAtVertex` wants both
   `wi` and `wo` pointing **away from the surface** (it negates `wo` internally to
   build the incoming ray — see the BDPTIntegrator.h DIRECTION CONVENTIONS block),
   exactly as `GenerateEye/LightSubpath` pass them (`scatDir, −currentRay.Dir()`).
   The flipped `wo`/`wi` made the Lambertian BSDF evaluate the wrong hemisphere
   → exactly `0` → the `heroF ≤ NEARZERO` guard zeroed the companion throughput
   from that vertex downstream. **Fix:** `dirToPrev = mkVector3(verts[i-1].position,
   v.position)` = `prev − v`, matching generation and every connection-site
   reconstruction (which were already correct — this function was the lone outlier).

2. **`EvalEmitterRadiance<NMTag>` missing `pLuminary` fallback** — the remaining
   general residual (mesh-emitter light subpaths).
   A light-**subpath endpoint** vertex (sampled by the light sampler) stores its
   emissive material on `pLuminary` with `pMaterial == 0`; the NM emitter re-eval
   only handled `pEnvLight` / `pLight` / `pMaterial->GetEmitter()` and hit
   `if(!pMaterial) return 0`, so the Phase-1 emission ratio re-evaluated `Le == 0`
   and zeroed the **whole** light subpath's companion throughput on every
   mesh-emitter scene. **Fix:** fall back to `pLuminary->GetMaterial()` (mirrors
   `GenerateLightSubpath`'s own seed and the s=0 eye-hit path). Env lights are
   untouched (handled by the earlier `pEnvLight` branch); the s=0 eye-hit path is
   untouched (`pMaterial` is set there) → no Pel / non-HWSS change.

3. **`totalActive` counted dispersive-terminated companions** — the
   dispersion-specific component (NOT a per-wavelength-geometry problem).
   [BDPTSpectralRasterizer.cpp](../src/Library/Rendering/BDPTSpectralRasterizer.cpp)
   (+ sibling [VCMSpectralRasterizer.cpp](../src/Library/Rendering/VCMSpectralRasterizer.cpp))
   incremented `totalActive` for a companion terminated at a dispersive interface
   even though it contributes 0, dividing the bundle mean by `N` instead of by the
   surviving count → biasing through-glass pixels toward 0. The PT reference
   (`PixelBasedSpectralIntegratingRasterizer::TakeSingleSampleHWSS`) excludes
   terminated wavelengths from both the sum **and** the count. **Fix:** match PT —
   don't count a terminated companion. **No multi-week change was needed:** the
   hero (uniform over λ) unbiasedly represents the bundle for terminated cases;
   the only defect was the denominator over-count. (Companions that *can* follow
   the hero's path — uniform IOR — were already handled correctly once root causes
   1 & 2 were fixed.)

### Verification (64 spp, lum = mean(R,G,B)/3; box filter off → default; EXR FLOAT)

**The bundle invariant restored (the load-bearing property: hwss=true must equal hwss=false):**

| BDPT hwss=T / hwss=F | master | fixed |
|---|---|---|
| uniform IOR (general) | **0.861** | **1.009** ✓ |
| dispersive IOR | **0.674** | **1.008** ✓ |

| BDPT-hwss=T / PT | master | fixed |
|---|---|---|
| uniform | 0.821 | 0.961 |
| dispersive | 0.642 | 0.965 |

The residual ~4 % BDPT/PT is **present in hwss=false too** (0.953–0.957) → a
pre-existing BDPT-spectral-vs-PT difference (the committed PT repro uses
`pixelintegratingspectral_rasterizer` + default filter), **not** an HWSS bug.

**Sibling (VCM) bundle invariant** (same shared fixes + the `totalActive` sibling):
uniform **1.012**, dispersive **1.000** ✓.

**HWSS env-IBL deficit — ALSO CLOSED (the bonus CLAUDE.md hoped for).** This is
the same `RecomputeSubpathThroughputNM` companion bug. Master env-only Lambertian
spectral **hwss=true**: BDPT mean ~0.39–0.47 (**−30 %** vs its own hwss=false
~0.61–0.64), VCM ~0.35–0.41 (**−35 %**); **post-fix** BDPT ~0.54–0.65, VCM
~0.51–0.61 → hwss-invariant, matching the hwss=false ground truth ~0.57–0.64.

**No regression:** `./run_all_tests.sh` → **116/116**; `EnvLightBalanceTest`
**80/80 lax**; warning-free clean `make all`+`make tests` rebuild; PT integrator
**byte-identical** (no `PathTracingIntegrator` change); Pel and non-HWSS NM paths
**byte-identical** (the three fixes live only in the HWSS-companion code).

### Open remainder (out of scope, documented) — a SEPARATE pre-existing **PT** HWSS env-IBL bias
On the uniform env-only Lambertian scene **PT itself violates the bundle
invariant**: PT-hwss=true renders ~20 % under PT-hwss=false (PT's forward
`PathTracingIntegrator::IntegrateFromHitHWSS`, a *different* mechanism from the
shared `RecomputeSubpathThroughputNM`). This is the "separate spectral-bundle
workstream" CLAUDE.md already flags; **this fix does not touch PT.** Because the
old `EnvLightBalanceTest` compared spectral integrators against PT at the *same*
hwss, master "passed" the env-only hwss=true row only by coincidence — BDPT/VCM's
own (larger) companion bias landed near PT's bias (all three biased low together).
With the companion fix BDPT/VCM are now correct and diverge from the still-biased
PT-hwss=true. The test was corrected to reference the **unbiased PT-hwss=false**
ground truth for the env-only spectral row (a one-line change in
[tests/EnvLightBalanceTest.cpp](../tests/EnvLightBalanceTest.cpp) `TestEnvOnlySpectral`,
no tolerance loosened — and the new comparison *would have caught* the master
companion bug the old one missed). Fixing PT's own HWSS env-IBL bias remains the
documented separate workstream.

### Sibling note
VCM-spectral **merging** still uses a luminance proxy (`RISEPelToNMProxy`), a
*separate* documented HWSS correctness gap ([SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md)
§3) — a different mechanism from the three fixes above, still open.

---

## Full-suite & gate verification (Bugs 1 + 2)

- **`make all` + `make tests`**: warning-free clean rebuild (`-O3 -flto
  -ffast-math` production flags restored after the diagnostic builds).
- **`./run_all_tests.sh` → 116 passed, 0 failed, 0 skipped (116/116).**
- **EnvLightBalanceTest**: passed (the fragile env-MIS gate — neither fix touches
  env MIS).
- **EXR tests** (`FrameEncoderTest` byte-identity, `HDRRoundTripTest`,
  `FileRasterizerOutputShimTest`): passed — all use `bpp < 32` → the unchanged
  half path.
- **At-risk no-regression (Bug 2 shares `GenerateEyeSubpath`):** jewel_vault BDPT
  **10.68371** (matrix 10.6764), gi_spheres BDPT **0.39136** (matrix 0.3914),
  perspective sculptors BDPT **0.31906** (pre-fix 0.31990), VCM sculptors
  **0.17550** (matrix 0.1742) — all within MC noise; the fix is orthographic-only.

## Diagnostic note for future agents — macOS `-ffast-math`

> **⚠ SUPERSEDED — do not act on anything in this section without first
> reading §"SUPERSEDED 2026-07-29 — `-fno-finite-math-only` is now ON
> everywhere" below.  `std::isfinite` / `std::isnan` WORK now; the
> `volatile`-laundering instruction in this section is no longer required,
> and this section's measured table has the two workloads' verdicts
> backwards.**

[build/make/rise/Config.OSX](../build/make/rise/Config.OSX) builds with
`-ffast-math` (⇒ `-ffinite-math-only`) and `-flto`. Consequently **every
`std::isfinite` / `std::isnan` guard in the integrators is folded to a constant
and dead-code-eliminated on macOS** (the Android build pointedly adds
`-fno-finite-math-only` with the comment "infs/nans are used in intersection
math"; the make build does not). Inf/NaN-detecting instrumentation or guards on
macOS must launder the value through `volatile` (or compare against a finite
threshold), or `std::isfinite` silently returns `true`. This cost real time
during Bug 1 diagnosis.

**Investigated 2026-06-03 (measured, NOT applied).** A global
`-fno-finite-math-only` (the Android approach) was measured with a
drift-controlled **interleaved A/B** — rebuild both binaries, alternate them
sample-by-sample so each pair sees identical machine conditions (necessary:
sequential baseline-vs-after showed ±8 % session drift that swamped the signal):

| workload | scene | paired Δ (flag-on vs flag-off) |
|---|---|---|
| BVH-traversal-heavy | aphrodite mesh @ 700 spp | **+3.23 %** (±1.36 SE, ~2.4σ — real) |
| shading-heavy | irradiance_cache_torture | +0.90 % (±0.70 SE, ~1.3σ — not significant) |

The ~3 % BVH cost is the NaN-aware ray-box / ray-tri min/max the flag re-enables.
Per the no-perf-regression constraint the flag was **reverted, not committed**.
The correctness debt (dead guards + the optimizer assuming finiteness in
intersection math) stays open; candidate fixes that avoid the BVH cost:
(a) **bit-pattern guards** — rewrite the ~48 `isfinite` / `isnan` sites as integer
IEEE-bit checks that `-ffinite-math-only` cannot DCE (re-arms the guards at ~0 %
perf; does not remove the optimizer's finiteness assumption); or (b) a
**targeted per-TU** flag (uncertain — the intersection TUs that need it are also
the costly ones). **DECISION 2026-06-04: leave as-is (documented).** The glass_pavilion "Inf" was root-caused to the EXRReader FP16 read bug, not an integrator Inf, so the concrete motivation to re-arm the guards evaporated; the debt is latent with no confirmed production harm. Revisit only if a real integrator Inf/NaN ever surfaces (instrument with finite-threshold checks, not isfinite, per the note above).

### ⚠ SUPERSEDED 2026-07-29 — `-fno-finite-math-only` is now ON everywhere

**The 2026-06-04 "leave as-is" decision above is reversed, and its measured
table and its cost attribution are both WRONG.** `-fno-finite-math-only` now
ships in every macOS build configuration; `std::isfinite` / `std::isnan` work
again, and the `volatile`-laundering / finite-threshold workarounds described
above are no longer required. [FiniteMath.h](../src/Library/Utilities/FiniteMath.h)'s
`IsFiniteDouble` stays in tree with its ~73 call sites, kept for uniformity and
`-Ofast` resistance rather than for correctness — but it is **not** free:
measured **~0.38 ns/call vs ~0.21 ns for `std::isfinite` (~2×)** — an earlier
draft claimed ~35–39× from a microbenchmark whose `std::isfinite` loop had been
optimised away (0.01 ns/call is ~0.04 cycles); that figure is retracted —
because the `volatile` barrier forces a stack round-trip and blocks
vectorization of the enclosing loop. At current call-site density that is well
under the +2.7 % measured below, but **part of that 2.7 % is recoverable** by
reverting per-sample sites to plain `std::isfinite`
(e.g. `AOVBuffers.cpp:120`, `FrameStore.cpp:838`, `BDPTIntegrator.cpp:333`,
`PathGuidingField.cpp:474`). Not done here — it is a separate change.

**What was wrong with the old measurement.** Re-measured with the same
interleaved-ABBA protocol, **10 pairs per workload** for every number in the
table below, plus a **CPU-time** metric (`ru_utime + ru_stime`). CIs are
Student-t at n = 10 (t₉ = 2.262), not normal:

| workload | wall Δ | CPU Δ | 95 % CI (CPU) | verdict |
|---|---|---|---|---|
| BVH-traversal-heavy (aphrodite mesh @ 700 spp) | −0.02 % (±1.10 SE) | −0.48 % (±1.39 SE, 0.3σ) | **[−3.63 %, +2.66 %]** | **no RESOLVABLE cost — a weak null, not a measured zero: the data bound the cost only below about +2.7 %, which does not exclude the shading workload's own +2.7 %** |
| shading-heavy (irradiance_cache_torture) | +2.61 % (±0.34 SE) | **+2.69 %** (±0.25 SE, 11.0σ) | [+2.14 %, +3.25 %] | **real, ~2.7 %** |

**Instrument caveats — read before reusing either workload.** CPU time is the
better instrument only where the work is *balanced*. On aphrodite it is
actually the NOISIER of the two (paired SD 4.40 vs wall 3.47; per-arm CV 3.6–4.3 %
vs 2.8–3.3 %) because CPU time *absorbs* lock contention as spin rather than
rejecting it — a profile of that render puts ~28 % of samples in
`__psynch_cvwait` / mutex waits. It behaves as intended on torture (CV 1.0 %
vs 1.3–1.7 %). Aphrodite also carries ~5.7 s of serial setup inside the timed
region (≈17 % of wall) and reaches only ~8.4 of 10 effective cores, and
traversal is ~36 % of its samples — so its whole-run CI bounds the
*traversal-code* cost only to roughly ±7 %. It is a low-sensitivity
instrument for the question it is used to answer. Both figures are one scene,
one integrator (`pixelpel_rasterizer`, RGB PT), one machine, and the **make**
build; the release DMG ships from Xcode **Opto**, which was never timed.

The old table has the two workloads' verdicts **backwards**. The claim that
"the ~3 % BVH cost is the NaN-aware ray-box / ray-tri min/max the flag
re-enables" is false: the hot BVH4 traversal uses hand-written SIMD
intrinsics (`vminq_f32` / `_mm_min_ps`, [BVH.h](../src/Library/Acceleration/BVH.h)
`RayBox4`) whose NaN semantics are fixed in hardware and **cannot** be
altered by `-ffinite-math-only`, so the flag was never buying anything in that
kernel — verified by extracting `RayBox4` and compiling it both ways: the NEON
output is **byte-identical** (55 instructions each). The real cost is scalar
shading math. **Scope, however:** the BVH2-fallback scalar slab test
(`RayBoxF`) DOES change — without the flag clang SLP-vectorizes it to
`fminnm.4s`/`fmaxnm.4s`, and with the flag it stays scalar. Scenes on the BVH2
path (failed BVH4 collapse, or the ≤4-object linear TLAS fallback) were never
measured and could pay more than the aphrodite figure implies. The old +3.23 % was almost
certainly session drift — an 8-pair wall-clock run on the same scene
reproduced +4.60 % ±3.46 (1.3σ) and then −0.02 % ±1.10 on replication.

**Why the cost is worth paying.** `-ffast-math` does not merely disable the
*guards*; it changes arithmetic the code depends on. Direct probe, runtime
values the compiler cannot fold:

| build | `isnan(0/0)` | `isfinite(1/0)` | `isnan(0.0 * inf)` |
|---|---|---|---|
| `-ffast-math` | 0 | 1 | **0** |
| `+ -fno-finite-math-only` | 1 | 0 | **1** |

These are properties of the **toolchain**, not of RISE's traversal code. An
earlier draft of this section (and of `Config.OSX`) claimed column 3 was "the
BVH slab test's own expression, since `invDir` is ±Inf for an axis-parallel
ray" — that is **FALSE and has been corrected**: RISE never produces an Inf
`invDir`. All four prep sites substitute a finite sentinel *because*
`-ffinite-math-only` made real Infs unsafe — `BVH::PrepRayFloat` uses
±`FLT_MAX` ([BVH.h:1222](../src/Library/Acceleration/BVH.h), and `:1502`,
`:1587`, `:1661`), `Ray::RecomputeInvDir` uses ±1e30
([Ray.h:33](../src/Library/Utilities/Ray.h)), and `RISE_INFINITY` is DBL_MAX,
not `inf`. So `0.0 * inf` never arises in the slab test and the kernel never
"relied" on the flag. The real justification is the one above plus the next
paragraph: guards and tests that are live rather than folded, the optimiser no
longer assuming finiteness in code that *can* produce Inf at runtime, and NEW finite-sentinel workarounds ceasing to be needed. The **existing**
sentinels stay load-bearing for an independent reason: a real `±inf` `invDir`
makes `(bound − origin) * invDir` = `0*inf` = NaN for a ray parallel to and on a
slab plane, and the rejection `tmin > tymax || tymin > tmax` is FALSE for NaN,
so the slab would be silently ACCEPTED. Do **not** restore `infinity()`.

**It caught a real bug within one test run.** `ThinFilmTMMTest` went red
immediately (3/3 FAIL with the flag, 3/3 PASS without — deterministic): at
EXACTLY the critical angle the transmitted medium's cosθ is exactly 0, so
η_p = N/cosθ is infinite and `(η0 − Y)/(η0 + Y)` evaluates Inf/Inf = NaN.
Note this is a *different* degeneracy from the grazing-incidence one that
`ThinFilm.h`'s `kGrazingCosFloor` already guarded — at the critical angle the
*incident* cosine is ~0.745, nowhere near that floor. Fixed by reformulation
(not a clamp): the reflectance is assembled as `r = (η0·B − C)/(η0·B + C)`
multiplied through by `cos0·cosS`, which clears every `1/cosθ`; the common
factor cancels, so it is algebraically identical wherever the old form was
finite and yields |r| = 1 exactly at the critical angle. See
`AdmittanceScale` / `ScaledAdmittance` (this line used to also name
`InterfaceReflection`, which has never existed in that header; the interface
helper is `InterfaceTerms`) in
[ThinFilm.h](../src/Library/Utilities/ThinFilm.h) and the mirrored oracles in
`tests/thinfilm/`.

**Bonus, measured after the fact:** the same reformulation also closes the
*grazing* degeneracy that `TmmReference.h` had documented as permanent ("R_p is
NaN at θ = 90° precisely"). At exactly θ = 90° the scaled assembly now returns
R_s = R_p = 1 (finite, and the correct limit) for a bare interface in either
direction, a dielectric film, a frustrated-TIR gap and an absorbing film. This
makes `ThinFilm.h`'s `kGrazingCosFloor` clamp no longer load-bearing *for NaN
avoidance* — it is retained as belt-and-braces and for its second, separate
purpose (keeping the Snell invariant strictly below 1, a representability
property of `SnellInvariant` itself). The clamp was **not** removed; that would
be an unrelated behaviour change.

**Degeneracy — CLOSED 2026-07-30 (b231efe1).**

The residual class was **any medium other than the ambient/substrate endpoints
having cos θ == 0 exactly**: a *film* at its own critical angle, and also
exactly grazing whenever a film index equals the ambient's. Both evaluators
returned NaN there, by two different mechanisms — the p-polarization admittance
`N/cos` was infinite, while the s-polarization one was **zero**, making
`−i·sinδ/η` a 0/0. (The 2026-07-29 version of this paragraph got all of that
wrong: it claimed only the N-layer `ar_layer` path was affected, prescribed a
remedy that is a no-op for s, and asserted a fix would need a running scale
factor. The numerical-review lens refuted all three.)

**Closed by two changes:**
1. **A cos-free layer matrix.** `δ = kd·N·cos` carries exactly one factor of
   `cos`, so `δ/η` and `δ·η` are finite for *both* polarizations
   (`s: kd`, `kd·N²·cos²`; `p: kd·cos²`, `kd·N²`). This removed the admittance
   from the layer loop entirely, and `detail::Admittance()` was deleted for
   want of callers. (As first written this used `sinδ = δ·sinc(δ)`; the
   2026-07-30 exponent-factored form below replaced `sinc` with the same
   `ExpM1OverZ` the Airy path uses. The cos-clearing argument is unchanged —
   only the exponential handling differs.)
2. **A re-derived, FACTORED Airy form** (2026-07-30). Keeping each interface as
   a cleared (numerator, denominator) pair lets the vanishing `2·N1·cos1` factor
   out analytically, so the single-film form is total at `cos_film == 0` too.
   ⚠ The 2026-07-29 version of this item claimed "the cancellation is structural
   and no factoring removes it" and closed the degeneracy with an
   `isfinite`-triggered Airy/TMM pairing. **Both were wrong.** The claim was
   disproved constructively; and `isfinite` is the wrong predicate, because with
   the unfactored quotient and any absorbing medium below the film the
   degenerate case is finite and exactly **1.0** — a perfect mirror, wrong by up
   to 0.477 on Ti — so the fallback never fires and the error is SILENT. The
   pairing survives only as defence in depth and must never be relied on as the
   degeneracy trigger.
   Airy stays **primary** deliberately — but the reason recorded here
   ("the TMM overflows on thick absorbers ~1.0e4 nm at k = 3 where Airy is
   finite past 1e6 nm") **expired on 2026-07-30**, when the exponent-factored
   layer matrix removed that overflow. Airy is primary for **cost** now: one
   complex `exp` plus one `ExpM1OverZ`, versus a 2×2 matrix build and multiply
   per film. See the P1-B entry below.

All three shipped entry points are covered: `ReflectanceConductor` (both
overloads) and `ReflectanceConductorRGBSpectral`. The RGB one was **missed by
b231efe1 and fixed on 2026-07-30 after round-2 review** — it still called the
bare Airy form and returned `(nan, nan, nan)` from `GGXBRDF`/`GGXSPF`, so that
commit's "BOTH thin-film paths" title was false when written.

Pinned by `ThinFilmTMMTest` [8/8] (exactly 90° across six topologies; the
film-at-own-critical geometry finite, in range **and continuous** with its
two-sided limit; the p-sign against the signed closed form) and
`ThinFilmProductionTest`'s [Degenerate] block (the shipped scalar path, plus
thick-absorber assertions).

⚠ **`ThinFilmTMMTest` binds the ORACLE ONLY** — it includes `tests/thinfilm/*`
under `using namespace RISE::ThinFilmReference` and never includes
`src/Library/Utilities/ThinFilm.h`. Any production claim citing it as a pin was
citing the wrong binary; see the P1-C entry below. Production-binding
assertions live in `ThinFilmProductionTest`.


**Undisclosed regression found by the same review (now disclosed — and since
CLOSED, see the P1-B entry below, which removed the overflow entirely):** the
pre-existing thick-absorber overflow caveat is **NOT** unchanged by this work,
and the "verified finite at 10 µm and NaN from 50 µm, exactly the documented
threshold" line was a two-point check that could not resolve the cliff. By
80-step bisection the cliff is at **~10.25 µm**, not 50 µm, and it is
**flag-dependent**: under the shipped flags `-ffast-math` implies
`-fcx-limited-range`, so `std::complex` division uses the unscaled `c²+d²`, and
the new form's final denominator `|η0·B ± C|` is larger than the old form's
`|B|` by roughly a factor `|η0|`. A random hunt over 800k stacks found **37
cases where the NEW form is NaN and the OLD form was finite** (none the other
way); under strict IEEE both give 0. All 37 sit inside the already-documented
thick-absorber regime and the shipped single-film path uses Airy, so this is a
bounded headroom loss rather than a new production defect — but it is a real
cost of the reformulation and was previously claimed as "did not change it".

---

### Three open P1s from the 2026-07-29/30 review loop — ALL CLOSED 2026-07-30

The three defects that round 3 disclosed rather than fixed. Every number below
is measured; the independent reference throughout is an **mpmath evaluation at
120 decimal digits** of the plain textbook Airy quotient and the plain textbook
characteristic matrix (at 120 dps the quantities that make the double evaluator
degenerate are merely small, not zero, so the naive forms are accurate — which
is exactly what makes it independent). Its two algebras self-agree to 3.1e-119,
and it reproduces the reviewer's independently-obtained figures exactly.

**P1-A — `PickForwardCos` selected the GROWING evanescent root.** The rule
tested `Re(η) > 0` first, with an `Re(η) == 0` tie-break for the evanescent
case. That tie-break is **dead code where it is needed**: `std::sqrt` of a negative
real is evaluated as `polar(r, ±π/2)`, leaving `Re = r·cos(π/2) ≈ 6.1e-17` — 0
firings for an evanescent medium in 4,000,000 evaluations (it fires only when
`η` is exactly 0, at a medium's own critical angle, where both roots are 0 and
the choice is immaterial). So the branch was decided by the sign of a round-off
residue, and it picked the root with `Im(δ) < 0`, i.e. `|e^{+2iδ}| → ∞` —
contradicting `ThinFilm.h`'s own stated invariant.

The fix is the **decaying** root: `Im(η) > 0`, tie-broken by `Re(η) > 0`. The
order is load-bearing and the justification is a floating-point asymmetry, not
a physical one — `Im(η) == 0` is **exact** (real arithmetic propagates exact
zero imaginary parts; the one step that manufactures a component out of nothing
manufactures the *real* one), measured 1,224,959 exact firings in the same
4,000,000. It is also the portable order: under a `std::sqrt(complex)` that
returns an exact `(0, ±r)` instead of the polar form, `Re == 0` fires too and
Re-first appears to work — which is why this broke silently.

Measured on the shipped evaluator over 3,000,000 adversarial stacks, before →
after: non-finite per-polarization Airy **206,884 → 0**; out-of-range
`ReflectanceConductor` **141,728 → 0**; the silent class where a p-polarization
Airy returns exactly 0 **21 → 0**. On a 4,000-stack grid scored against the
120-dps reference: non-finite **138 → 0**, and — the number that matters —
worst error among the *finite* values **0.3276 → 7.8e-13**. Worked example:
ambient 2.2636308289185867, evanescent film 0.44278123514951256,
d = 6884.8552408452115 nm, substrate 2.9724385231768848 + 5.140684523956045i
returned **0.49999999999999994** where the truth is exactly **1**.

Applied **globally**, not only to films. The task framing suggested films alone
need the decaying rule (the Airy quotient and the layer matrix are both
invariant under `cos_j → −cos_j` — true, and verified). But the measurement
shows the global form is the *correct* rule rather than a safe over-reach: for
a passive stack the two rules never disagree (0 disagreements in 4,000,000 with
a non-absorbing ambient), and where they do disagree — an absorbing ambient —
the decaying rule is the one that keeps the evaluator total. Role-dependence
would have bought nothing and cost a branch on medium role.

**P1-B — `ReflectanceConductorStack` was non-total and its equivalence claim
was false.** The doc comment claimed "with `nFilms == 1` this is algebraically
the Airy single-film result". Measured: **329,823 NaN of the same 3,000,000**,
and **52 finite cases disagreeing by up to 0.74**. Reachable through
parser-legal `ar_layer`: the 3-layer coating `1.38/100 nm`,
`2.1+0.6i/100000 nm`, `1.46/90 nm` on air→glass returned NaN, which survives
the `[0,1]` clamp (`NaN < 0` and `NaN > 1` are both false).

Root cause was **not** the missing Airy pairing the disclosure suggested — it
was overflow. `cosδ` and `sinδ` each grow like `e^{|Im δ|}`. Fixed by factoring
that exponential out of the layer **analytically**:

    Mⱼ = (e^{−iδⱼ}/2)·M̂ⱼ ,   M̂ⱼ = [[1+φ, (1−φ)/ηⱼ], [ηⱼ(1−φ), 1+φ]] ,  φ = e^{+2iδⱼ}

`|φ| ≤ 1` because the forward root decays (P1-A is a prerequisite), and the
dropped scalar cancels **exactly**, because `r = (η₀B − C)/(η₀B + C)` with
`[B;C] = M[1;η_s]` is homogeneous of degree 0 in `M`. `M̂` is not a rescaling
heuristic — it is a different, equally valid representative of the same
projective quantity. Writing `(1−φ) = −z·E(z)` at `z = 2iδ` with
`E = ExpM1OverZ` reuses the same cleared `δ/η`, `δ·η` products, so `M̂` stays
finite at `cosθ = 0` too, and `Sinc` drops out of the layer loop (it survives
inside `ExpM1OverZ`).

After: **0 non-finite of 3,000,000**; single-vs-stack worst disagreement
**0.744 → 7.6e-12** (typical ~1e-16; the worst cases are ~100 µm films whose
phase exceeds 1e6 rad, where both forms lose the same argument-reduction
precision and each is within 6e-12 of the 120-dps truth). The cited
counter-example now returns 0.35449755010853973 against a truth of
0.35449755010854007, and the 3-layer `ar_layer` stack 0.014823504562343591
against 0.014823504562343579. So the equivalence claim is now **true**, stated
with its measured bound rather than asserted.

**This also retires the thick-absorber caveat above.** Over 4,000,000 stacks
with `|N|` in 1e-3..1e3 and thickness to **1e12 nm**, both forms are finite
everywhere (0 non-finite, versus 947,323 for Airy and 1,553,854 for the TMM on
the same inputs pre-fix). The "cliff at ~10.25 µm", the "37 of 800k regressions
from the scaled assembly", and "prefer Airy for extreme thick-absorber queries"
are all **history**. Airy remains the single-film production form for **cost**.

**P1-C — `ThinFilmTMMTest` is ORACLE-ONLY, so its cited pins bound nothing in
production.** It includes only `tests/thinfilm/*` and runs under
`using namespace RISE::ThinFilmReference`, so every `detail::` name resolves to
the oracle copy. Production comments nevertheless cited `[8/8](d)` as pinning
the production `Sinc` / `ExpM1OverZ`. **Demonstrated by mutation:** stubbing the
*production* `Sinc` to a constant 1 below its cut passed all five
production-header binaries *and* `ThinFilmTMMTest` (6138/0).

Fixed by adding four production-binding blocks to `ThinFilmProductionTest`
(which does include the production header):

| block | binds |
|---|---|
| `[Helpers]` | production `Sinc` / `ExpM1OverZ` against **mpmath constants at 50 dps**, across both branch switches, plus explicit stub-mutant kills |
| `[Branch]` | the decay invariant `Im(N cosθ) ≥ 0` over 31,059 media; the tie-break liveness measurement; the p-polarization sign against the signed closed form; a film whose index equals the ambient's at grazing |
| `[Thick]` | both forms finite to d = 1e12 nm, and multi-layer `ar_layer` stacks with thick absorbers — this is the **gate** for the "the `isfinite` fallback is dead" and "the N-layer path is total" claims, which would otherwise be prose |
| `[Truth]` | absolute values against the 120-dps reference, including the P1-A silent-wrong case and the P1-B counter-examples |

Mutation-verified after the fix: the production `Sinc` stub now **dies** (8
failures), as does reverting `PickForwardCos` to Re-first (6) and reverting the
layer matrix to the overflowing `cosδ`/`sincδ` form (6).

**A second, unreported instance of the same defect was found while fixing
this:** the *oracle's* own `PickForwardCos` was equally unpinned — reverting it
to Re-first passed `ThinFilmTMMTest` 6138/0, because `[6/7]`'s frustrated-TIR
sweep stops at 1000 nm and the branch bug does not bite until ~4 µm. Since the
oracle is the ground truth `ThinFilmProductionTest` compares against, a
silently-wrong oracle is the more dangerous of the two. `[6/7]` now sweeps
evanescent gaps to 1e7 nm over three substrates and asserts the decay
invariant; both oracle mutants die.

**Also corrected in `[8/8](d)`:** the "long-double reference" is not higher
precision on arm64 macOS, where `sizeof(long double) == 8` — the same type as
the value under test. That cross-check pins the *formula* and the branch
continuity, never accuracy beyond double. The genuinely-higher-precision pin is
`[Helpers]`, against mpmath at 50 dps. The claimed margin has been removed
rather than restated.

**Residual, disclosed and bounded.** With an **absorbing ambient** (`k₀ > 0`)
the incident wave is inhomogeneous and the stack is not passive: the 120-dps
reference itself returns `R > 1` — measured 1.093 and 1.287 on the first two
stacks sampled, but **288 of 400 exceed 1 and the excess reaches 59.4**, so the
error the clamp conceals is *unbounded*, not the ~30 % those two figures
suggest. The evaluator is **total** there — the decaying rule keeps every
exponential of modulus ≤ 1 — and the `[0,1]` clamp reports the saturated 1, but
that is a saturation, not a physical answer. This is a domain limit, not a
defect: Byrnes' `tmm` rejects an absorbing incident medium outright. It is also
**unreachable**: all four GGX call sites pass a literal `0.0` for `k0`, and
`DielectricSPF.cpp` builds its ambient with `MakeIndex(nIncident, 0.0)`. It is
documented on the `k0` parameter of `ReflectanceConductor` and in
`PickForwardCos`.

**Scope of the "before" figures.** The fuzz counts above are **macOS / libc++ /
arm64** measurements. The old rule's *manifestation* was implementation-
dependent: it decided the evanescent branch on the ~6.1e-17 real residue that
libc++ leaves because it evaluates `std::sqrt` of a negative real as
`polar(r, ±π/2)`. A standard library returning an exact `(0, ±r)` would make the
old `Re == 0` tie-break fire and the old rule work. The new rule is correct
under **both** conventions, so this is a strict portability improvement — but
do not read "206,884 non-finite" as a claim about Linux or MSVC, which were not
measured.

---

### Review round 2 (2026-07-30): four fresh reviewers, 5 P1s

Axes: numerical correctness, doc/comment fidelity, test binding by mutation,
consumer/API impact. The **three headline fixes above survived all four** —
correctness found no P1 in them, mutation confirmed every algebraic mutant dies
in `ThinFilmProductionTest`, and consumer impact measured the shipped regime
unchanged (max 4.2e-15 over 564,300 evaluations across all six entry points).
What the round found was **claims** and **unpinned guards**.

**A derivation replaced a measurement.** The correctness reviewer supplied the
proof the branch rule was missing: for a real Snell invariant,
`η² = N² − s²`, so `Im(η²) = 2nk ≥ 0` for a passive medium, and a number whose
square lies in the closed upper half-plane lies in quadrant I or III — never II
or IV. Hence `Re(η)` and `Im(η)` always share a sign, the two rules *cannot*
disagree on a passive stack, and the tie-break decides only the axis cases. It
also predicts exactly where they diverge: an absorbing ambient makes `s` complex,
`Im(η²)` can go negative, and the root leaves quadrant I. That derivation is now
in `PickForwardCos`.

**Two guards were entirely unpinned** (found by mutation; both mutants passed
all seven thin-film binaries):
- `kGrazingCosFloor` — the header spends ~25 lines arguing it must be `1e-6` and
  specifically not `NEARZERO`, yet mutating it to `1e-12` **or to `0`** changed
  nothing any test could see, while measurably collapsing `R(cos=1e-9)` from
  `0.99999235932627695` to exactly `1`. Now bracketed from both sides by
  `[Invariant]`(c).
- `MakeIndex`'s `|k|` fold — and this one was worse than untested: the **Complex
  overloads bypassed `MakeIndex` entirely**, so a negative extinction reached the
  math unnormalized and produced per-polarization `R_s = 4.69`, `R_p = 17.69`,
  laundered by the `[0,1]` clamp into a plausible saturated `1.0`. That is the
  same silent finite-but-wrong class this whole arc is about. Normalization moved
  into `detail::PhysicalIndex`, applied at every entry point.

**Input domain closed properly.** The round-1 `PhaseCoefficient` normalized
*thickness* and claimed thickness was "the ONE input that can defeat the
decaying-root guarantee". **False**: `lambda_nm` is equally unvalidated, and
`λ < 0` flips the sign of `kd` just as `d < 0` does (measured `Im(δ) = −2.84`,
`|e^{+2iδ}| = 295.6`, `R_p = 17.69` → saturated `1.0`); `λ = 0` and `d = +inf`
both gave NaN, while `d = NaN` happened to normalize. The condition is now on
`kd` itself, covering all six cases with one rule. Likewise the **zero-index
NaN** that round 1 merely disclosed is fixed — it affected the **ambient and
substrate as well as the film**, and reached the BRDF two different ways,
neither benign: the RGB path yields a NaN pixel, the spectral path goes
*silently black*, because `GGXBRDF`'s `if( Rfilm > 0 )` is false for NaN.

**Residual that remains open, measured and deliberately not closed:** a merely
*tiny* index is still wrong, and it fails **differently under the two flag
sets** — bisected in-repo, not quoted. Under the shipped `-ffast-math` (which
implies `-fcx-limited-range`, so complex division is the naive
`(ac+bd, bc−ad)/(c²+d²)`) it is NaN below `n1 = 1e-77.03`, by **overflow** —
the p-polarization interface products grow like `1/n1` until `c²+d²` leaves the
double range. Under strict IEEE it is **not** NaN: it returns the plausible
bare-stack reflectance, the film silently vanishing, from `1e-78` down to about
`1e-154`. Round 2 recorded this as "`≲1e-100`" and "underflows"; both were
wrong, and neither had been measured here. No threshold was introduced, because any threshold here is exactly the
magic epsilon this file refuses. The named refinement is to reformulate
`CosThetaInMedium` around `η² = N² − s²` — finite and well conditioned for tiny
`N`, and the same identity the branch-rule proof rests on — instead of dividing
by `N`. Judged out of proportion: it touches every `cosθ` in the file, and
`1e-100` is ~100 decades below any refractive index that is scene data, whereas
exactly `0` — a black texel through an unvalidated `IScalarPainter` — is not,
and that case *is* fixed.

**The same `-fcx-limited-range` mechanism also bounds the thick-absorber
claim.** Round 1 wrote that the exponent-factored matrix has "no cliff and no
headroom to lose". Within the measured envelope (`|N| ∈ 1e-3..1e3`, `d ≤ 1e12`
nm) that reproduces exactly — 0 non-finite of 4,000,000, both forms, both flag
sets. Outside it a cliff remains, and round 3 showed my first characterisation
of it was wrong in three ways. Re-measured in-repo, 1,000,000 stacks per row:

| layers | \|N\| range | shipped flags | strict IEEE |
|---|---|---|---|
| 8 | 1e-20..1e20 | 26,931 | 1,055 |
| 4 | 1e-20..1e20 | 1,055 | 521 |
| 2 | 1e-20..1e20 | 486 | 244 |
| 8 | 1e-12..1e12 | 1 | 0 |
| 8 | 1e-3..1e3 | 0 | 0 |

So it is **not** confined to eight layers, **not** confined to `|N| > 1e12`, and
**not** absent under strict IEEE — all three of which I had asserted, quoting a
review rather than re-deriving. Worse, the failure outside the envelope is
sometimes **silent**: under strict IEEE a film index below ~1e-78 returns the
bare-stack reflectance (the layer simply vanishes) and only becomes NaN below
~1e-154. A NaN count is the wrong instrument for that region — which is this
arc's own recurring lesson, applied to itself one round too late. The envelope
is the claim.

**Cost, previously undisclosed** (min of 30 × 200,000 calls, arm64, shipped
flags):

| entry point | 345566ff | now | Δ |
|---|---|---|---|
| `ReflectanceConductor` | 149.9 ns | 154.2 ns | **+2.9 %** |
| `ReflectanceConductorStack` n=1 | 202.7 ns | 209.4 ns | **+3.3 %** |
| `ReflectanceConductorStack` n=2 | 267.4 ns | 280.2 ns | **+4.8 %** |

The N-layer figures were **+12.5 % / +17.9 %** before hoisting the per-film
normalization out of the polarization loop. A select-form branch rule was tried
and is *slower* (161.9 ns); the exponent-factored layer matrix is marginally
free (per-additional-layer cost unchanged within noise). Recorded in the
`ThinFilm.h` file header so it is not rediscovered.

**A real cost of keeping the oracle "in step".** The correctness reviewer
flagged that rewriting `tests/thinfilm/TmmReference.h` into the *identical*
exponent-factored form has made oracle-vs-production close to a tautology: the
two now share `PickForwardCos`, `CosThetaInMedium`, `PhaseCoefficient`,
`PhysicalIndex`, `InterfaceTerms`, `ExpM1OverZ` and `Sinc`, differing only in
top-level algebra. `ThinFilmTMMTest`'s Airy↔TMM cross-check still compares two
algebras, but over a much larger shared substrate. Mitigated rather than
reversed — the independent checks are now (a) the mpmath 120-dps constants in
`[Truth]` and `[Helpers]`, and (b) a new `[Invariant]` block asserting
**branch-rule-free** physics: TIR gives `R == 1` exactly for a bare dense→rare
interface past critical (worst `2.2e-16` over 20 configurations), frustrated TIR
is monotone in gap thickness and reaches `1` only in the thick limit, and `R_p`
vanishes at the Brewster angle (worst `1.4e-32`). Those hold whatever cos-root
convention is in force.

**Test-claim corrections.** `[Branch]`(b) claimed to catch an order swap in
`PickForwardCos`; mutation showed it cannot — it recomputes the principal root
inline and never calls the function. It is kept (it is a genuine
platform-assumption guard on `std::sqrt`) with an honest comment, and a real
pin `[Branch]`(b2) was added that feeds `PickForwardCos` constructed candidates.
That pin deliberately does **not** obtain its candidate from `std::sqrt`: which
root `sqrt` returns for an evanescent medium depends on the sign of a signed
zero in `Im(1 − sin²θ)`, and that sign is **constant-folding dependent** — `+0`
when the compiler folds literals, `−0` at run time. That is a large part of why
the original bug hid from a test suite full of literals while misbehaving in
every render. `[Truth]`'s tolerance was `1e-9` against a measured `3.3e-16`,
which let a `1e-10`-scale error through a pin whose stated job is absolute
accuracy; it and `[Thick]`'s agreement gate are now `1e-13`.

**Also corrected:** the round-1 claim that the old rule "decided the branch by
the sign of a round-off residue" — implying intermittency. The residue's sign is
invariably positive (0 negative of 775,041), so `Re(η) > 0` held for *every*
evanescent medium and the rule deterministically accepted whatever root
`std::sqrt` produced. Systematic, not flaky. And `ThinFilmTMMTest [2/7]`
asserted the `e^{−2iδ}` convention five times, including in a test-name string —
the opposite convention, the one the Airy↔TMM cross-check exists to catch. It
passed only because those stacks are lossless at normal incidence, where `δ` is
real and the two coincide at `±1`.

**Flag surface (all four sites).** `-ffast-math` had only ever been in the
make build and the two Xcode **Opto** configurations, so the picture was more
uneven than the note above implied: Linux (`Config.Linux`) and MSVC never
enabled fast-math at all, and Android already used
`-ffast-math -fno-finite-math-only`. Now updated:

- [Config.OSX](../build/make/rise/Config.OSX) (`Config.specific` symlinks to it)
- all **8** target-level `OTHER_CPLUSPLUSFLAGS` blocks in
  [project.pbxproj](../build/XCode/rise/rise.xcodeproj/project.pbxproj) — stated
  explicitly in every configuration, not just the two that had `-ffast-math`,
  so no config can pick finite-math-only up from an inherited setting or the
  Xcode "Relax IEEE Compliance" checkbox (`OTHER_CPLUSPLUSFLAGS` lands after
  setting-derived flags, so it wins)
- the three **project-level** configs were `GCC_OPTIMIZATION_LEVEL = fast`
  (i.e. `-Ofast`, which *implies* `-ffast-math`) — inert because both targets
  override it, but a landmine; pinned to `3`
- `Config.SGI` / `Config.Solaris` are **deliberately NOT changed**: they target
  g++ 2.95.4 and 3.0, which predate `-ffinite-math-only` (~GCC 3.3), so the flag
  would be an unrecognized-option hard error. An earlier pass added it "for
  consistency"; that was reverted.

**Process gap this exposed:** the release DMG is built from configuration
**Opto** ([create_macos_release.sh](../scripts/create_macos_release.sh)) while
the documented warning gate in [AGENTS.md](../AGENTS.md) builds **Deployment**.
Before this change those two configs had *different* FP semantics — Deployment
was IEEE-correct and Opto was not — so an Inf/NaN bug could pass every local
check and ship. Worth keeping in mind for any future per-configuration flag.

**Verification:** make build warning-free; `run_all_tests.sh` **220/220**;
Xcode `RISE-GUI`/Deployment, `RISE-GUI-Opto`/Opto and `rise`/Deployment all
BUILD SUCCEEDED with 0 warnings; rendered output unchanged (an A-vs-B image
diff is **indistinguishable from** the same-binary A-vs-A control — mean |Δ|
0.03606 vs 0.03626 against one control frame and 0.03680 against the other, so
the sign of the difference is a coin-flip — i.e. within the
renderer's own run-to-run nondeterminism). `AgentRenderAsyncTest` failed once
during this work and is a **pre-existing flake**, not flag-related (passes 3/3
both with and without the flag, and in the final full suite).
