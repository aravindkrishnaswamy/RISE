# PT env-MIS double-count — a dead MIS arm made live

**Status:** SHIPPED (2026-08-27).
**Date:** 2026-08-27.
**Found by:** the hair Phase-1 furnace test (`HairRenderTest.cpp`,
[docs/HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md) §7 render validation) — a white
furnace with `sigma_a 0.0` held a stable +4% over-unity reading that
background dilution could not explain.
**Commits:** `e4f36607` (F1, surface fix), `afb2d64e` (F2, medium-scatter
siblings + OpenPGL), `ff92ab91` (F3, `EnvLightBalanceTest` re-derivation).

---

## TL;DR

1. **PT's BSDF-sampled environment escape was added at full weight on top
   of an already MIS-weighted env-NEE sample.** The two strategies summed to
   `1 + w_nee` instead of `1`. Every PT render with a global radiance map and
   a non-delta BSDF read too bright by the env's MIS share — measured
   `+17.7%` on a white furnace, `+17.7%` on `EnvLightBalanceTest`'s env-only
   Lambertian topology, `+15.7pp` on a thin-column volumetric furnace.
2. **The dead arm was reachable only through an `if`/`else if` ordering
   bug**, not a missing MIS implementation — the MIS-weighted code path
   already existed and was correct; it just never ran, because every
   production PT rasterizer passes the *global* radiance map through the
   same parameter (`pRadianceMap`) that the *per-object* branch tested
   first.
3. **HWSS's surface-escape path never had this bug** — it happens to test
   the global map first — but HWSS's *medium-scatter* escape path had the
   identical defect and needed its own fix.
4. **The fix moved only PT.** BDPT and VCM are byte-for-byte unchanged.
   Because `EnvLightBalanceTest` had been asserting BDPT/VCM agreement with
   PT, fixing PT's inflated reference *exposed* — did not create — BDPT/VCM
   biases of `+24-50%` on the same topologies. The suite is re-derived
   against closed forms in F3; see "Fallout and re-calibration" below.
5. **The PT-default integrator decision is unaffected.** It was made on
   wall-clock-normalized variance (σ²·T), not on absolute luminance — see
   [UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md). Nothing
   here changes which integrator is cheaper per unit of variance.

---

## 1. The bug

`PathTracingIntegrator::IntegrateFromHitTemplated` (shared by RGB PT and
non-HWSS spectral PT) resolves which radiance map an escaping ray should
read against two possibilities: a per-object map bound via the hit
material, or the scene's global map. Before the fix this was written

```
if( pRadianceMap ) { /* no MIS weight */ }
else if( global )  { /* MIS-weighted against env-NEE */ }
```

Every `pathtracing_pel_rasterizer` / `pathtracing_spectral_rasterizer`
entry point constructs its integrator call with `pRadianceMap =
pScene.GetGlobalRadianceMap()` — i.e. the *global* map is passed in through
the *per-object* parameter slot. The first arm therefore always matched,
the MIS-weighted `else if` arm was structurally unreachable, and every
BSDF-sampled ray that escaped to the environment added the env radiance at
weight 1 — on top of an env-NEE sample that had already, correctly,
applied its own MIS weight `w_nee`. The two complementary strategies of a
single two-technique MIS partition summed to `1 + w_nee` instead of `1`.

The live code and the caveats that survive it are in
[src/Library/Shaders/PathTracingIntegrator.cpp:1983-2089](../src/Library/Shaders/PathTracingIntegrator.cpp)
(comment block headed "MIS PARTNER RULE", surface-escape site). The
volume-scatter twin — the camera ray's inline medium-scatter escape, a
structurally separate code path that shares the same bug independently —
is documented at
[PathTracingIntegrator.cpp:3812-3852](../src/Library/Shaders/PathTracingIntegrator.cpp)
(RGB/NM) and
[PathTracingIntegrator.cpp:5198-5213](../src/Library/Shaders/PathTracingIntegrator.cpp)
(HWSS twin).

**Why HWSS's surface path was immune.** `IntegrateFromHitHWSS`'s escape
block tests the *global* map first (`if( pRadianceMap )` at
[PathTracingIntegrator.cpp:5344](../src/Library/Shaders/PathTracingIntegrator.cpp),
where `pRadianceMap` is again the global map passed in through that slot)
and applies the MIS weight unconditionally on that arm — the ordering that
happened to be correct for MIS is the mirror image of the RGB/NM bug (and
is itself wrong for per-object map *selection*, see the residual ledger
below). That accidental correctness is exactly why `hwss=true` and
`hwss=false` disagreed on the same furnace scene before the fix: `0.947`
vs `1.046`. HWSS's medium-scatter escape path was a distinct call site
with its own copy of the buggy ordering and was not protected by this
accident — it needed the same fix as the RGB/NM volume twin (see Fix 2
below).

## 2. The proof

**Closed form.** For a white furnace (Lambertian albedo 1, uniform `L = 1`
environment, cosine-weighted BSDF sampling against a uniform-over-sphere
env pdf), the excess added by carrying the BSDF-sampled escape at weight 1
instead of `w_bsdf = 1 - w_nee` is the hemisphere integral of the power-2
env-NEE weight itself:

```
excess = ln(17) / 16 = 0.177076
```

Measured pre-fix: `0.17722` (furnace), matching to four significant
figures. At albedo 0.5 the same derivation predicts `0.588538`; measured
pre-fix `0.58848 / 0.58948 / 0.58857` across three runs of
`EnvLightBalanceTest`'s env-only Lambertian topology (expected closed-form
mean `0.5` exactly). Post-fix the furnace reads `1.000668`; the albedo-0.5
topology reads `0.49994 / 0.50076 / 0.49997`
([PathTracingIntegrator.cpp:2038-2043](../src/Library/Shaders/PathTracingIntegrator.cpp)).
Two independently-shaped scenes, two independently-derived closed forms,
both corroborating the same `ln(17)/16` mechanism at two different
albedos — this is what "closed form + two-albedo corroboration" means in
the commit messages.

**The `EnvironmentSampler` pdf cancellation.** `pES->Pdf(currentRay.Dir())`
is the density the env-NEE strategy imports the environment through; for a
uniform env this is `1/(4π)`, constant over the sphere. The BSDF-sampled
direction carries the cosine-weighted hemisphere pdf `cosθ/π`. The power-2
weight the env-NEE sample already applies is
`w_nee(θ) = envPdf² / (envPdf² + bsdfPdf(θ)²)`; its BSDF-sampled
complement is `w_bsdf(θ) = 1 - w_nee(θ)`. Adding the BSDF-sampled escape at
weight 1 rather than `w_bsdf(θ)` double-counts exactly `w_nee(θ)` on every
escaping direction, and integrating `w_nee(θ)` against the cosine-weighted
BSDF sampling density over the hemisphere is the `ln(17)/16` closed form
above — the pdf ratio that makes the power heuristic well-defined here is
also what makes the excess integral closed-form solvable. The full
algebraic derivation lives in the commit proof (`e4f36607`), not
reproduced here; this section states the mechanism, not a re-derivation.

**Volumetric corroboration.** `VolumeEnvFurnaceTest.cpp` (new in F2,
`afb2d64e`) isolates the same mechanism on the camera-ray medium-scatter
path with a thin-column geometry chosen specifically so a *different*,
pre-existing truncation bug (see residual ledger, item 1) cannot cancel
against it. Red-proving the fix (reverting it and rebuilding) reproduces
the predicted `+0.5 * (1 - e^-tau) * E[Tr_escape] ≈ +16%` excess: RGB
`+15.73%`, spectral hwss=false `+16.39%`, spectral hwss=true `+10.90%`
([tests/VolumeEnvFurnaceTest.cpp:107-118](../tests/VolumeEnvFurnaceTest.cpp)).
All nine of that file's checks fail with the fix reverted and pass with it
in place.

## 3. The fixes

- **F1 (`e4f36607`), surface escape.** Resolve the applicable radiance map
  once (`pEnvForEscape = pRadianceMap ? pRadianceMap :
  scene.GetGlobalRadianceMap()`), then apply the power-2 (or optimal-MIS)
  weight *iff* the resolved map is pointer-identical to the scene's global
  map — the only map env-NEE ever importance-samples. A genuinely
  per-object map (unreachable from any production rasterizer today, see
  the residual ledger) keeps full weight, since nothing imports it via
  NEE. [PathTracingIntegrator.cpp:2073-2089](../src/Library/Shaders/PathTracingIntegrator.cpp).
- **F2 (`afb2d64e`), medium-scatter siblings + OpenPGL.** Both camera-ray
  medium-scatter escape sites — `IntegrateRayTemplated` (RGB/NM,
  [PathTracingIntegrator.cpp:3861-3882](../src/Library/Shaders/PathTracingIntegrator.cpp))
  and its HWSS twin
  ([PathTracingIntegrator.cpp:5223-5244](../src/Library/Shaders/PathTracingIntegrator.cpp))
  — now MIS-weight the phase-sampled env hit against env-NEE's partner,
  mirroring F1 and the already-correct main-loop continuation. Phase `Pdf`
  argument-order equivalence was verified: the NEE side evaluates
  `pPhase->Pdf(envDir, wo)`, this side evaluates `pPhase->Pdf(wo, wi)`, and
  both concrete phase functions (isotropic, Henyey-Greenstein) depend on
  the two directions only through `Dot(wi, wo)`, which is symmetric, so
  the densities agree exactly. Optimal-MIS training is deliberately not
  accumulated at this site (no tracked volume `bsdfTimesCos` analogue);
  the optimal-MIS *evaluation* branch is still mirrored so partition-of-
  unity holds once alpha solves. Also fixed: the OpenPGL background-segment
  recorder, previously reachable only through the (structurally dead)
  global arm, now stores RAW radiance and `miWeight` as two separate
  fields for every escaping ray, per `PGLPathSegmentData`'s emission-site
  convention — PT had never trained the guiding field on env background
  before this.
- **F3 (`ff92ab91`), suite re-derivation.** No integrator code changed
  beyond one supervisor-flagged comment correction (the HWSS medium-escape
  `envPdf` recomputation is load-bearing per-wavelength, not textual
  parallelism —
  [PathTracingIntegrator.cpp:5205-5213](../src/Library/Shaders/PathTracingIntegrator.cpp)).
  `EnvLightBalanceTest.cpp` was rewritten to assert PT against closed forms
  directly instead of asserting BDPT/VCM agreement with PT. See below.

## 4. The fallout and re-calibration

`EnvLightBalanceTest` had never had an independent ground truth for its
uniform-env topologies; it asserted that BDPT and VCM agree with PT to
within a tolerance family. Once F1/F2 corrected PT, the suite's own
reference moved, and BDPT/VCM — genuinely unchanged — fell outside bands
that had only ever passed because the PT they were compared against was
`+14` to `+22%` too bright.

F3 restructures the suite into three kinds of check
([tests/EnvLightBalanceTest.cpp:28-65](../tests/EnvLightBalanceTest.cpp)):

1. **Absolute closed-form checks on PT.** Topology D (env-only Lambertian,
   RGB) asserts exactly `0.5`, measured `-0.04%` to `+0.12%`. Topology E
   (env + omni) asserts a closed-form increment over D of `0.01230206`.
   Topology F (env + mesh emitter) asserts an increment of `0.01987740` —
   derived from the emitter *occluding* the env over its own solid angle
   (`L_out = 0.5 + (ρ/π)(L_e - L_env)·Ω_cos(p)`, checked to 8 significant
   figures against 800×800 brute-force quadrature at three sample points;
   [tests/EnvLightBalanceTest.cpp:1166-1214](../tests/EnvLightBalanceTest.cpp)).
   Topology G is the spectral twin of D, closed-form on luminance. Topology
   J (submerged camera / delta dielectric shell) was already exact and is
   unchanged by this arc.
2. **Bias-referenced regression bands on BDPT/VCM.** Rather than widen a
   parity band to hide a real bias, each check now carries the *measured*
   ratio-to-truth as its band centre, with a banner stating that a future
   MIS-accuracy fix is *expected* to fail these bands and must re-derive
   them. Measured, against the corrected (truth-referenced) PT: env-only
   Lambertian BDPT `+28.5%`, VCM `+24.3%`; env+mesh BDPT `+13.6%`, VCM
   `+50.0%` — the single largest bias in the suite
   ([tests/HairRenderTest.cpp:898-911](../tests/HairRenderTest.cpp) carries
   the topology-F commentary tying this number to
   [VCM_ENV_MIS_PARTITION_INVESTIGATION.md](VCM_ENV_MIS_PARTITION_INVESTIGATION.md)).
3. **Firefly caps**, replacing the old "within 1.5× of PT max": `max <=
   cap * own mean`, per integrator, immune to the mean bias and targeted at
   the historical t=1 white-firefly regression
   ([tests/EnvLightBalanceTest.cpp:426-441](../tests/EnvLightBalanceTest.cpp)).

Net: **116 checks** (up from 101), 6 consecutive clean runs measured, worst
band utilisation 0.756. RGB rows are bit-identical run-to-run on the
measuring machine; spectral rasterizers are not (see residual ledger,
item 6).

## 5. Implications for prior conclusions

The Session 9-13 investigation in
[VCM_ENV_MIS_PARTITION_INVESTIGATION.md](VCM_ENV_MIS_PARTITION_INVESTIGATION.md)
and [PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) ran entirely against the
inflated PT reference this arc fixes. Their **relative** findings — which
sites were bisected and ruled innocent (Session 10's `pdfSelect` plumbing,
splats, mesh-side strategies), the diagnosis that VCM's residual is a
partition-of-unity violation between the env-S0 and env-NEE strategies
(Session 11), the falsification of the minimal 2-function SA-MIS fix and
the "phantom s=2 term" analysis (Session 12), and the STOP recommendation
with its filter-confounder and HWSS-spectral-bundle findings (Session 13)
— all compared BDPT/VCM against the *same* PT baseline throughout each
experiment, so nothing about which sites are innocent or what the
architectural shape of the residual is has changed.

What needs re-reading through this lens is any **absolute** luminance
percentage those sessions quoted against PT — e.g. "VCM env+mesh = 122% of
PT" (`PRE_PHASE1_STATUS.md` Session 10) or "the 22% over VCM env+mesh...
share a single source" (`VCM_ENV_MIS_PARTITION_INVESTIGATION.md` TL;DR
item 1). Those percentages were computed against a PT that was itself
`+13.6-17.7%` over truth on the same topologies; the corrected, truth-
referenced figure for VCM env+mesh is `+50.0%`, not `+22%`. The
architectural diagnosis — disc-area-vs-solid-angle MIS partition mismatch
between env-S0 and env-NEE — is unaffected; its *size*, measured against
truth rather than against inflated PT, is larger than previously stated.

The PT-default integrator-selection decision in
[UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md) is
unaffected: that decision was made on wall-clock-normalized variance
(σ²·T), where PT's 3-7× lower per-sample cost was the deciding factor, not
on absolute luminance agreement with any other integrator. Nothing in this
arc touches σ²·T.

## 6. The residual ledger

Known-open items, each measured or diagnosed but **not** fixed in this
arc. Flagged so a future session does not re-discover them from scratch.

1. **Medium-scatter continuation truncation loss.** After a camera ray's
   one inline medium scatter, a continuation that misses all geometry is
   terminated with a deterministic Beer-Lambert factor; the "scatter
   again" branch is dropped. Pre-existing, out of scope for this arc.
   Worth `~0.4%` on `VolumeEnvFurnaceTest`'s thin-column geometry (chosen
   specifically to minimize this term so it wouldn't cancel against the
   MIS fix being measured) but would cost several percent on a cubical fog
   box: `correct = e^-τ + (1-e^-τ)·E[Tr_escape] ≈ 0.996` vs the actual
   truncated estimator.
   [tests/VolumeEnvFurnaceTest.cpp:34-71](../tests/VolumeEnvFurnaceTest.cpp).
2. **`cosEnv > 0` env-NEE gate denies full-sphere BSDFs.** `LightSampler`'s
   env-NEE block gates on `cosEnv > 0`, so a transmissive full-sphere BSDF
   (hair's TT lobe transmits *through* the fibre) gets no NEE strategy for
   the below-normal hemisphere while BSDF sampling still applies `w_bsdf <
   1` there — the two strategies sum to less than 1. Confirmed by
   construction: relaxing the gate to `fabs(cosEnv)` (a no-op for ordinary
   BRDFs) moves a hair furnace from `0.98863` to `1.00181`. Not applied —
   needs a per-`IBSDF` audit of below-hemisphere `value()` behaviour plus
   the matching light-table gate (item 3).
   [tests/HairRenderTest.cpp:412-424](../tests/HairRenderTest.cpp).
3. **Light-table `cosSurface` break vs. backlit fibres.** A discrete
   `omni_light` hair scene renders PT and BDPT `4.3×` apart (PT `0.0132`
   vs BDPT `0.0575` at one light power). Root cause:
   `LightSampler`'s light-table NEE breaks out on `cosSurface <= 0`, so PT
   cannot light a hair fibre from behind — exactly the TT-lobe transport
   that carries most of hair's energy. BDPT's light subpath has no such
   gate. The same `fabs(...)` diagnostic used in item 2 closes roughly half
   the gap (`4.3× -> 2.3×`); the rest is uncharacterised. This is a
   full-sphere-NEE workstream, not a one-line fix.
   [tests/HairRenderTest.cpp:614-628](../tests/HairRenderTest.cpp).
4. **Per-object-map MIS-partner + HWSS map-selection caveats.** With F1's
   fix, a genuinely per-object radiance map (reachable only via shader-op
   rasterizers, not any `pathtracing_*` entry point today) has no NEE
   partner of its own — `LightSampler` doesn't know about per-object maps
   and always samples the global one — while HWSS's surface-escape
   ordering (map-selection-correct at the cost of always preferring the
   global map over a per-object one) is the mirror-image gap. Both are
   unreachable from any code path exercised today; recorded so neither is
   mistaken for a finished reference if a caller starts passing a genuine
   per-object map.
   [PathTracingIntegrator.cpp:1998-2036](../src/Library/Shaders/PathTracingIntegrator.cpp).
5. **Topology J noise-free PT max reads `1.135`.** Flagged, not chased:
   on a delta path with no MC noise and no MIS (every ray crosses exactly
   one lossless interface), the per-pixel max should track the mean at
   `1.0` closely; a value of `1.135` sits inside the check's existing
   `<= 1.5` cap but is higher than the noise-free geometry would predict.
   Filter normalisation or fp16 accumulation is suspect; the closed-form
   transport itself is exonerated (the mean check passes tightly). Not
   investigated further this arc — commit `ff92ab91`;
   [tests/EnvLightBalanceTest.cpp:1642-1659](../tests/EnvLightBalanceTest.cpp)
   (`CheckExactRadiance`'s `maxOk` threshold).
6. **Spectral-rasterizer non-bit-reproducibility.** RGB (Pel) rasterizer
   output is bit-identical run-to-run on the measuring machine; spectral
   (HWSS and non-HWSS) rasterizer output moves `1.3-3.6%` run-to-run at
   equal sample counts, which is why every spectral tolerance band in this
   arc's tests is wider than its RGB twin. Recorded, not root-caused.
   [tests/EnvLightBalanceTest.cpp:130-145](../tests/EnvLightBalanceTest.cpp).

---

## What this doc does not claim

The `ln(17)/16` closed form and its albedo-0.5 corroboration are
*measured* facts (rendered output compared against an independently
derived integral), not merely inferred from reading the code. The
mechanism section (§2) states *why* the two numbers should match, at the
level of detail the commit proof itself argues, but the full symbolic
derivation lives in the commit message and test-file comments cited above,
not restated line-by-line here. The residual-ledger items are exactly as
open as stated: measured symptoms with a named suspect, not fixes deferred
by choice.
