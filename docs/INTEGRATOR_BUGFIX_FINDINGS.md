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
