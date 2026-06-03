# Integrator Bug-Fix Findings (Phase-1 matrix by-products)

**Date:** 2026-06-03
**Scope:** The three integrator anomalies surfaced as by-products of the
Phase-1 measurement ([UNIFIED_INTEGRATOR_BASELINES.md](UNIFIED_INTEGRATOR_BASELINES.md)
§6/§7/§9): `glass_pavilion` Inf fireflies (Bug 1), `sculptors_studio` BDPT
near-black (Bug 2), `prism_dispersion` spectral-BDPT −36 % (Bug 3).
**Status:** Bug 1 FIXED (working tree, pending commit). Bug 2 FIXED (working
tree, pending commit). Bug 3 — see §3.
**Discipline:** every claim below is from instrumented renders, not intuition.
All changes are uncommitted for review.

---

## Bug 1 — `glass_pavilion` "Inf firefly" — **MISDIAGNOSED; it is an FP16 EXR-write overflow, not an integrator 1/0. FIXED at the writer layer.**

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

### Note for the matrix
The `glass_pavilion` BDPT/VCM `Inf⚠` cells are a **measurement artifact** of the
FP16 EXR + a genuinely noisy scene, not an integrator defect. With this fix a
re-measure yields large-but-finite variance (the scene IS pathologically noisy
for all three integrators — a legitimate §7 "highest-leverage" target for a new
technique, but not a correctness bug).

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

### Suggested regression (not yet added)
A `BDPTStrategyBalanceTest` topology with an orthographic camera would lock this
invariant; deferred to keep this change minimal — noted for follow-up.

---

## Bug 3 — `prism_dispersion` spectral-BDPT −36 % — **REAL, and it is the deep HWSS spectral-bundle bias. DOCUMENTED & DEFERRED (multi-week).**

### Confirmed real?
Yes. Reproduced: spectral-PT lum **1.24401**, spectral-BDPT lum **0.79641**
(64.0 % of PT, **−36.0 %**), matching the matrix.

### Root cause (instrumented via the hwss / dispersion discriminators)
The deficit is **entirely HWSS-specific and BDPT-specific**, and it has two
additive components:

| variant | PT-spectral lum | BDPT-spectral lum | BDPT / PT |
|---|---|---|---|
| dispersive (`ior 1.3 1.5 2`), **hwss=true** | 1.24401 | 0.79641 | **64 %** |
| dispersive, **hwss=false** | 1.21949 | 1.23157 | **101 %** ✓ |
| non-dispersive (`ior 1.5 1.5 1.5`), hwss=true | 1.24526 | 1.03217 | **83 %** |

Reading the table:
1. **hwss=false closes the gap entirely** — BDPT matches PT within 1 %. So the
   bug is in the **hero-wavelength bundle machinery**, not in BDPT's spectral
   transport per se.
2. **PT is ~hwss-invariant** (1.244 vs 1.219). HWSS *sampling* is sound; the
   defect is in **BDPT's HWSS bundle handling**.
3. **Two components.** A general BDPT-HWSS bias (~17 % under even with a uniform
   IOR — no dispersion) **plus** a dispersion-specific loss (a further ~19 %,
   83 %→64 %, when bundle wavelengths refract at different angles through the
   dispersive dielectric).

The dispersion-specific component is mechanistically intuitive: an HWSS bundle
carries a hero + companion wavelengths along **one** shared path. At a dispersive
refraction each wavelength bends differently, so companions cannot follow the
hero's geometric path — BDPT's bundle generation / connection drops (or
mis-weights) that companion energy. The general component is the same
spectral-bundle bias CLAUDE.md already records for HWSS env-IBL ("hwss=true
env-only ~8–18 % under PT… a separate spectral-bundle workstream").

### Suspected code region (not a confirmed line-level fix)
The BDPT HWSS per-wavelength throughput is tracked in `hwssBetaNM[]` through
subpath generation ([BDPTIntegrator.cpp ~2475-2515](../src/Library/Shaders/BDPTIntegrator.cpp)
scatter update; ~4987 emission seed) and combined at the HWSS connection. A
principled fix needs the bundle's companion-wavelength throughput to survive a
dispersive (per-wavelength-direction) vertex and be MIS-weighted consistently
with PT's per-wavelength estimator.

### Decision: DOCUMENT & STOP
This is the deep HWSS spectral-bundle bias the task flagged as a multi-week
workstream, and CLAUDE.md independently records as "a separate spectral-bundle
workstream that must precede any SA migration." Per the task's stop rule, no
integrator code was changed for Bug 3 — a rushed bundle edit here risks biasing
every HWSS render. A crisp root-cause characterization (HWSS-specific +
BDPT-specific + general/dispersion split, with the discriminator data above) is
the deliverable.

### Sibling note
VCM-spectral merging uses a luminance proxy (`RISEPelToNMProxy`), a *separate*
documented HWSS correctness gap ([SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md)
§3) — not the same mechanism, but in the same HWSS-bundle family and worth
fixing alongside.

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
during Bug 1 diagnosis and is worth a follow-up (add `-fno-finite-math-only` to
Config.OSX to re-enable the existing guards) — flagged, not fixed here.
