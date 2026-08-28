# PT env-MIS double-count — a dead MIS arm made live

**Status:** SHIPPED (2026-08-27).
**Date:** 2026-08-27.
**Found by:** the hair Phase-1 furnace test (`HairRenderTest.cpp`,
[docs/HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md) §7 render validation) — a white
furnace with `sigma_a 0.0` held a stable +4% over-unity reading that
background dilution could not explain.
**Commits:** `e4f36607` (F1, surface fix), `afb2d64e` (F2, medium-scatter
siblings + OpenPGL), `ff92ab91` (F3, `EnvLightBalanceTest` re-derivation),
plus F4 (residual wave 4c, unshadowed volume-vertex NEE — closes
residual-ledger item 8; uncommitted at the time of writing).

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
pre-fix `0.58848 / 0.58948 / 0.58857` per channel (R/G/B) in
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
then-pre-existing truncation bug (residual ledger item 1, since closed)
cannot cancel against it. Red-proving the fix (reverting it and rebuilding) reproduces
the predicted `+0.5 * (1 - e^-tau) * E[Tr_escape] ≈ +16%` excess: RGB
`+15.73%`, spectral hwss=false `+16.39%`, spectral hwss=true `+10.90%`
([tests/VolumeEnvFurnaceTest.cpp:107-118](../tests/VolumeEnvFurnaceTest.cpp)).
Six of that file's checks fail with the fix reverted; all pass with it in
place. (The file has since grown from nine checks to twenty-nine — cells
4-6, the fog-box furnace, were added when residual-ledger item 1 was
closed, and cells 7-9, the fog box with a white floor, when items 7 and 8
were.)

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

- **F4 (residual wave 4c), unshadowed volume-vertex NEE.** All three NEE
  rows in `LightSampler::EvaluateDirectLighting{,NM}` — delta light, mesh
  area light, environment — gated their shadow ray on
  `if( pShadingObject && pShadingObject->DoesReceiveShadows() )`.
  `pShadingObject` is NULL at a **medium scatter vertex**
  (`MediumTransport::EvaluateInScattering{,NM}` is the only caller that
  passes `0`, and it always pairs the `0` with `isVolumeScatter == true`),
  so the gate was false and **no shadow ray was ever cast there**: every
  volume vertex integrated light it could not see. Step 1 of the same two
  functions had always spelled the test `pShadingObject ?
  pShadingObject->DoesReceiveShadows() : true` — "a NULL means nobody
  opted out", not "skip the visibility test" — and the fix hoists that
  reading into a single `bReceivesShadows` local used by all six sites.
  [LightSampler.cpp:1543-1572](../src/Library/Lights/LightSampler.cpp)
  (RGB, with the full derivation) and
  [LightSampler.cpp:2094-2101](../src/Library/Lights/LightSampler.cpp)
  (NM twin). Fixes residual-ledger item 8; see there for the per-strategy
  tally and the red-prove. RGB, NM and HWSS all reach these two functions
  through `MediumTransport`, so one fix closes all three; the legacy
  `RayCaster` volume walk
  ([RayCaster.cpp:1055](../src/Library/Rendering/RayCaster.cpp),
  [RayCaster.cpp:1678](../src/Library/Rendering/RayCaster.cpp)) is fixed by
  the same edit. BDPT and VCM have their own volume NEE and are untouched.
  **Cost:** the shadow rays are new work that was previously skipped —
  `scenes/Tests/Volumes/pt_chromatic_fog.RISEscene` goes from 17.1 s to
  20.8 s (**+21 %**, mean of 3 runs each). That is the price of the
  visibility test, not a regression to optimize away.

## 4. The fallout and re-calibration

`EnvLightBalanceTest` had never had an independent ground truth for its
uniform-env topologies; it asserted that BDPT and VCM agree with PT to
within a tolerance family. Once F1/F2 corrected PT, the suite's own
reference moved, and BDPT/VCM — genuinely unchanged — fell outside bands
that had only ever passed because the PT they were compared against was
`+14` to `+17.7%` too bright (the ln(17)/16 excess applies only to the
once-bounced env term, so `+17.7%` is the attainable maximum).

F3 restructures the suite into three kinds of check
([tests/EnvLightBalanceTest.cpp:28-65](../tests/EnvLightBalanceTest.cpp)):

1. **Absolute closed-form checks on PT.** Topology D (env-only Lambertian,
   RGB) asserts exactly `0.5`, measured `+0.0046%` on all three channels
   (bit-identical run to run). Topology E
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
   ([tests/EnvLightBalanceTest.cpp](../tests/EnvLightBalanceTest.cpp) carries
   the topology-F commentary tying this number to
   [VCM_ENV_MIS_PARTITION_INVESTIGATION.md](VCM_ENV_MIS_PARTITION_INVESTIGATION.md)).
3. **Firefly caps**, replacing the old "within 1.5× of PT max": `max <=
   cap * own mean`, per integrator, immune to the mean bias and targeted at
   the historical t=1 white-firefly regression
   ([tests/EnvLightBalanceTest.cpp](../tests/EnvLightBalanceTest.cpp)).

Net: **116 checks** (up from 101), worst band utilisation 0.735, 4
consecutive clean runs on the final calibration (10 runs pooled for the
band derivation).

### 4a. De-OIDN recalibration (residual wave 4b, 2026-08-27)

Wave 4 (ledger item 5) found the suite was capturing OIDN's output and
fixed **topology J only**, recording the other six rasterizer strings as
open work and warning that every non-closed-form number above was
denoiser-inclusive. That work is now done: **all seven strings set
`oidn_denoise FALSE`** and every band was re-derived from six de-OIDN'd
runs. What actually moved, measured OIDN-on vs OIDN-off on the same tree:

| check kind | movement | note |
| --- | --- | --- |
| PT vs closed form, topology D | `-0.040/+0.117/-0.025 %` → `+0.0046 %` on all three channels | ≤ `0.12 pp` |
| PT increment, topology E | `+0.18/+0.10/-0.12 %` → `-0.009 %` on all three | ≤ `0.30 pp` |
| PT increment, topology F | `-0.35/-0.06/-0.06 %` → `-0.129/-0.124/-0.128 %` | ≤ `0.30 pp` |
| PT spectral luminance, topology G | `-1.40 / -1.43 %` → `-1.62 / -1.32 %` | ≤ `0.23 pp` |
| BDPT/VCM **mean** centres | ≤ `0.08 pp` on every RGB row, ≤ `0.9 pp` on spectral rows | inside each spectral row's own run spread |
| BDPT/VCM **p99** centres | up to `+47 pp` | topology G `hwss=true`, BDPT green: `1.4459 → 1.9208` |
| **peak caps** | moved in *both* directions | see below |

The headline **bias figures are unchanged in substance** — the largest
mean-centre movement anywhere is `0.9 pp`, and on the RGB rows it is under
`0.1 pp`. VCM env+mesh reads `1.5000` de-OIDN'd against `1.5003` denoised,
which is the strongest single piece of evidence that these biases are
transport and not denoise. **The pre-recalibration figures in §4 above and
in the ledger were denoiser-inclusive; the deltas were under `0.1 pp` on
every RGB mean and under `0.9 pp` on every spectral mean, so the numbers
stand as written.**

Two things genuinely changed:

- **p99 bands.** OIDN was flattening the very tail this statistic
  measures, most severely under HWSS, where the per-wavelength bundle
  makes BDPT/VCM's tail far heavier than PT's. Two tolerances widened to
  satisfy the file's own `tol >= 3 × worst measured spread` rule (topology
  G `hwss=false` p99 `0.15 → 0.18`, `hwss=true` p99 `0.10 → 0.12`), plus
  two mean tolerances (`0.07 → 0.08` on the two spectral `hwss=false`
  rows) where the de-OIDN'd spread left under `1.1 pp` of margin.
- **Firefly caps split two → four, and every one got TIGHTER.** OIDN
  *raised* peakiness on the near-deterministic RGB rows (topology D PT
  `1.010` true vs `1.074` denoised — CNN ringing manufacturing a max the
  transport never produced) and *suppressed* it on the noisy spectral rows
  (topology G `hwss=true` BDPT `2.021` true vs `1.386` denoised). One pair
  of caps could no longer be both tight on the RGB rows and true on the
  spectral ones:

  | group | worst measured | old cap | new cap | utilisation |
  | --- | --- | --- | --- | --- |
  | uniform RGB (D,E,F) | `1.3522` | `2.20` | `1.90` | `71.2 %` |
  | uniform spectral (G×2) | `2.0207` | `2.20` | `2.75` | `73.5 %` |
  | non-uniform RGB (H) | `1.2230` | `2.60` | `1.70` | `71.9 %` |
  | non-uniform spectral (I×2) | `1.7629` | `2.60` | `2.40` | `73.5 %` |

  All four stay below `3.5`, so all four still fail the historical
  Phase-A t=1 white-firefly signature.

One prior claim was **refuted** by the recalibration: topology D's
`0.116 %` green-channel offset had been attributed to "a colour-pipe
round-trip constant, not MC noise". It was the denoiser. With it off, the
three channels agree to `6e-6` and all three sit `+0.004 %` from `0.5`;
there is no colour-pipe offset on that path. (The topology-G *spectral*
per-channel offsets — red `-4.8 %`, green `-1.1 %`, blue `+2.9 %` — really
are the Jakob–Hanika round-trip: they moved `≤ 0.4 pp`.)

**Reproducibility, re-verified post-de-OIDN over six runs.** Topology J is
bit-identical run to run for PT, BDPT *and* VCM (wave 4 claimed it only
for PT). PT is bit-identical on topologies D and E, and moves `≤ 0.013 %`
on F and `≤ 0.010 %` on H; BDPT/VCM mean-ratios move `≤ 0.04 %` and
p99-ratios `≤ 0.39 %` on the RGB rows. The spectral rows are *less*
reproducible than the denoised buffers suggested: PT mean up to `1.37 %`,
BDPT/VCM mean-ratio up to `2.21 %` (was `1.27 %`), p99-ratio up to
`5.46 %` (was `4.48 %`), peak up to `6.9 %`.

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

1. ~~**Medium-scatter continuation truncation loss.**~~ **CLOSED
   2026-08-27 (residual wave 4).** After a camera ray's one inline medium
   scatter, a continuation that missed all geometry was terminated with a
   deterministic Beer-Lambert factor and the "scatter again" branch was
   dropped — an energy LOSS of order `τ·τ'`. All three sites
   (`IntegrateRayTemplated`'s Pel and NM instantiations, and the
   `IntegrateRayHWSS` twin's per-wavelength loop) now run the ordinary
   volumetric random walk, bounded by `stabilityConfig.maxVolumeBounce`
   and Russian roulette exactly as `IntegrateFromHitTemplated`'s main loop
   bounds its own volume bounces, with this arc's env MIS partition
   re-closed at **every** scatter vertex rather than only the first. The
   replacement escape term has the **same expectation** as the
   deterministic one it replaces — for a bounded medium the no-scatter
   event has probability `Tr` and weight `Tr/pSurvival == 1` — so the
   whole of the fix is that the complementary event now continues the walk
   instead of being discarded. At the bounce cap the walk falls back to the
   old deterministic escape, so the cap degrades to the previous behaviour
   rather than to black. Guarded by `VolumeEnvFurnaceTest` cells 4–6, a
   cubical `τ = 0.4` fog-box furnace that could not be written while the
   truncation existed (`0.880 / 0.883 / 0.842` before, `0.996 / 1.000 /
   0.955` after; the column cells 1–3 move only `0.3–0.4 pp`, which is why
   they never caught it). The remaining `hwss=true` `~4.5 %` deficit is the
   pre-existing spectral-bundle env deficit, unchanged by this fix and
   identical on both geometries.
   [tests/VolumeEnvFurnaceTest.cpp](../tests/VolumeEnvFurnaceTest.cpp).
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
5. ~~**Topology J noise-free PT max reads `1.135`.**~~ **DIAGNOSED AND
   CLOSED 2026-08-27 (residual wave 4). Cause: the test harness was
   capturing OIDN's output, not the integrator's.**
   `CapturingRasterizerOutput` overrides only `OutputImage`, and
   `IRasterizerOutput::OutputDenoisedImage`'s **default** implementation
   forwards the POST-denoise pixels to `OutputImage`. Denoising is on by
   default and **no rasterizer string in `EnvLightBalanceTest` set
   `oidn_denoise FALSE`**, so every render in the suite was captured after
   OIDN had rewritten it. Evidence, in the order it settles the question:
   - The two suspects named in the original entry are both **refuted**.
     `pixel_filter box` was **already** set on every rasterizer string in
     the file, so no Mitchell-class negative lobe was ever in play; and the
     capture path is `RISEColor` doubles straight off
     `IRasterImage::GetPEL`, with no fp16/RGBE stage.
   - The three colour channels **disagree** (`1.110 / 1.064 / 1.135`) on a
     scene whose three channels are identical by construction. No filter
     and no transport effect can separate the channels here; a CNN mixing
     them can.
   - **Zero run-to-run jitter** is explained rather than mysterious: OIDN
     is deterministic.
   - It is **not PT-specific** — BDPT and VCM read `max ≈ 1.05–1.075` on
     the same closed form.
   - Decisive: setting `oidn_denoise FALSE` makes PT read
     `mean == p99 == max == 1.0` **bit-exactly**, BDPT to `2e-6` and VCM to
     `1.4e-4`.

   **Fixed, scoped to topology J only.** The PT-submerged rasterizer gains
   `oidn_denoise FALSE`, and BDPT/VCM get submerged-specific twins of the
   shared strings so all three read the closed form. `maxOk` tightens from
   `<= 1.5` to `<= 1.05`, which still clears the measured deviation by
   ~350× and red-proves the artifact (the denoised buffer's `1.135` and
   `1.075` both fail it).

   ~~**Left open, deliberately: the other six rasterizer strings in the
   file are still denoised.**~~ **CLOSED 2026-08-27 (residual wave 4b).**
   All seven strings now set `oidn_denoise FALSE` and every band in the
   file was re-derived from de-OIDN'd runs — see §4a for what moved. The
   warning that stood here (*"every non-closed-form number this suite
   prints has passed through OIDN and must not be quoted as an integrator
   measurement"*) is **withdrawn**: it no longer applies, and the figures
   it applied to turned out to be denoiser-inclusive by under `0.1 pp` on
   every RGB mean and under `0.9 pp` on every spectral mean. The two
   checks wave 4 predicted would fail were exactly the two that did
   (BDPT and VCM `p99`-ratio on env-only Lambertian, spectral
   `hwss=true`), and re-centring them was the largest single band move in
   the recalibration. This remains the same class of measurement artifact
   CLAUDE.md's Phase-1 lesson warns about: never trust a measurement
   harness that reads through a component it does not intend to measure.
   [tests/EnvLightBalanceTest.cpp](../tests/EnvLightBalanceTest.cpp)
   (the topology-J block's comment carries the full evidence).
6. **Spectral-rasterizer non-bit-reproducibility.** RGB (Pel) rasterizer
   output is bit-identical run-to-run on the measuring machine for PT on
   topologies D, E and J (and for BDPT/VCM on J), and moves `<= 0.013 %`
   elsewhere; spectral (HWSS and non-HWSS) rasterizer output moves up to
   `2.2 %` on mean-ratios and `5.5 %` on `p99`-ratios run-to-run at equal
   sample counts, which is why every spectral tolerance band in this arc's
   tests is wider than its RGB twin. Recorded, not root-caused. (Figures
   re-measured post-de-OIDN, wave 4b; the denoised numbers understated the
   spectral jitter.)
   [tests/EnvLightBalanceTest.cpp](../tests/EnvLightBalanceTest.cpp).
7. ~~**HWSS main-loop volume-scatter continuation.**~~ **CLOSED
   2026-08-27 (residual wave 4b).** Item 1 fixed the truncation on CAMERA
   rays; the same loss class survived one bounce further along.
   `IntegrateFromHitHWSS` — the shared bounce loop for the hero bundle —
   evaluated NEE at a volume-scatter vertex and then `break`, so a ray
   that had already scattered off a **surface** and then scattered in a
   medium lost every subsequent scatter, the surface hand-off and the
   escape. The RGB and NM twins share `IntegrateFromHitTemplated`, which
   always looped; that was **verified, not assumed** (see the red-prove
   below). The HWSS site now runs the same per-wavelength walk wave 4
   built for `IntegrateRayHWSS` — the hero bundle splits at the scatter,
   each wavelength walks at its own λ, the surface hand-off is
   `IntegrateFromHitNM` — adapted for a mid-path start: throughput seeds
   from `throughputComp[w]`, `walkDepth`/`walkVolumeBounces` both advance
   per scatter so Russian roulette sees exactly the `depth + volumeBounces`
   schedule the RGB/NM loop produces, `importance` is the carried path
   importance, the walk stops at `maxDepth` exactly as the enclosing loop
   would, the hand-off carries the live per-type bounce counters and
   `smsHadNonSpecularShading=true`, and the escape mirrors the enclosing
   loop's own `!bHit` env branch including its `pRadianceMap` fallback.
   Guarded by `VolumeEnvFurnaceTest` cells 7–9, a new fog-box-plus-white-
   floor furnace. Red-prove: with the `break` restored, `floor hwss=true`
   reads `0.7258` against `1.0138` fixed (ratio to the RGB reference
   `0.683 → 0.955`, where `0.955` is the pre-existing spectral-bundle
   deficit the no-geometry cells measure independently at `0.959`), while
   the RGB and NM cells are unchanged within MC noise — `1.0626 → 1.0621`
   and `1.0690 → 1.0670`. Those absolutes were measured **before** item 8
   was fixed and all carry its `~+6 %` over-count; post-item-8 the same
   revert also fails cell 9's new absolute `-9 %` mean band (`0.726`,
   3.0× outside).
   [tests/VolumeEnvFurnaceTest.cpp](../tests/VolumeEnvFurnaceTest.cpp).
8. ~~**Surface-bounce-through-medium env excess (`+6 %`).**~~ **CLOSED
   (residual wave 4c, 2026-08-27) — it was never a surface-bounce defect
   at all: NEE at a volume-scatter vertex cast NO SHADOW RAY.** See F4 in
   §3 for the fix. The symptom needed a *medium vertex with geometry
   around it*, and every prior volume test in the tree is a bare medium in
   empty space, which is why nothing had reached it before — and why cells
   1–6 stayed at `1.0` throughout: with no geometry, an unshadowed volume
   NEE is exactly right.
   - **Diagnosis method.** A compile-time per-strategy tally on cell 7
     (RGB, 16×16×4096) plus a 2-bit A/B that can make SURFACE and/or
     VOLUME vertices *analog* (env-NEE off there, the partner's MIS weight
     forced to 1). A fully analog unidirectional estimator must read the
     furnace value with no MIS anywhere, so any mode reading higher
     localizes the broken partition:

     | mode | surf-NEE | vol-NEE | esc/surf | esc/vol | TOTAL |
     |---|---|---|---|---|---|
     | 0 both MIS | 0.1194 | 0.2356 | 0.5396 | 0.1673 | **1.0620** |
     | 1 volume MIS only | 0.0000 | 0.2370 | 0.6586 | 0.1679 | **1.0636** |
     | 2 surface MIS only | 0.1201 | 0.0000 | 0.5375 | 0.3357 | **0.9933** |
     | 3 fully analog | 0.0000 | 0.0000 | 0.6583 | 0.3349 | **0.9932** |

     Modes 2 and 3 agreeing at `0.993` prove the transport *and* the
     surface partition exact; mode 1 sitting `+7 pp` above mode 3 pins the
     whole defect on the VOLUME partition. Within mode 1 the volume
     vertices' env-NEE reads `0.412` unweighted against its phase-sampled
     partner's `0.298` — two strategies for the same integral disagreeing
     by **+38 %**, which is exactly the solid angle the floor blocks.
     Post-fix all four modes agree at `0.993` and vol-NEE / esc-vol split
     `0.1488 / 0.1496`, an exact half each, as `w_nee = w_phase = 0.5`
     demands for an isotropic phase in a uniform environment.
   - **Every recorded symptom follows from the one cause.** It vanished as
     `sigma_s → 0` because that is the volume-vertex count going to zero;
     it scaled with the *bounce* rays' optical depth because that is how
     many volume vertices sit where the floor blocks a large solid angle
     (floor at `y=-20` `+6.25 %`, `y=-80` `+8.68 %`, outside the fog at
     `y=-150` `+9.41 %`); it was common-mode across RGB/NM/HWSS because
     all three reach the same two `LightSampler` functions through
     `MediumTransport`; and it was shader-op independent for the same
     reason.
   - **The earlier refutation stands and was not the miss.** The 1.75M
     instrumented calls with mean `Tr = 0.604` were `isVolumeScatter ==
     false` — surface calls — and the surface rows were never broken. The
     `(1 - Tr) × w_nee` coincidence was exactly that.
   - **Red-prove.** All six gates restored to `pShadingObject && …`:
     cell 7 `1.06205` (fixed `0.99322`) and cell 8 `1.06916` (fixed
     `0.99794`) both fail their `+2 %` mean band by ~3×; cells 1–6 are
     unchanged to within MC noise (`0.99849` / `0.99633` reverted vs
     `0.99777` / `0.99522` fixed), which is the evidence that the fix is
     scoped to volume vertices that actually have geometry around them.
     Cell 9 reads `+1.4…+2.1 %` across runs — it straddles the cap
     because the hero bundle's own `-5 %` deficit masks most of the
     over-count — so cells 7 and 8 are the guard, not cell 9.
   - **Cells 7–9 are now ABSOLUTE furnace assertions** (measured over 12
     runs: `0.9922…0.9950`, `0.9950…1.0005`, `0.9479…0.9560`), bands
     `[-2 %, +2 %]` for 7 and 8 and `[-9 %, +2 %]` for 9, with the two
     cross-variant ratios kept as the sharper item-7 guard. The suite is
     29 checks, up from 26.
   [tests/VolumeEnvFurnaceTest.cpp](../tests/VolumeEnvFurnaceTest.cpp)
   (file header, "WHAT IS GUARDED - FIX 4").
9. **HWSS floor-cell heavy tail.** With items 7 and 8 fixed, cell 9 shows
   `max/mean` between `1.09` and `1.79` over twelve runs while its mean
   stays within `±0.4 %` (`0.9479–0.9560`; ratio to RGB `0.9535–0.9577`).
   It is variance, not bias: the floor's bounce rays can run nearly
   parallel to the floor and traverse `tau ~ 1.6`, and the escape weight
   `Tr / pSurvival` with `pSurvival = e^-tau` legitimately reaches several.
   The RGB cell does not show it (`1.04–1.05`) because its per-channel
   weights stay correlated where the hero bundle's per-wavelength walks do
   not. Cell 9's per-pixel check is one-sided (low side only) for this
   reason, and its mean band is `9 %` rather than the `7 %` cells 3 and 6
   use — same bundle deficit, `0.7 pp` deeper and a `0.81 pp` run-to-run
   spread because this scene runs more bounces per path. (Pre-item-8 the
   same tail measured `1.05–5.31` with a `±1.9 %` mean spread; both
   narrowed once the over-count went away, but the tail's cause is
   unchanged.)
   [tests/VolumeEnvFurnaceTest.cpp](../tests/VolumeEnvFurnaceTest.cpp).

10. **Step-1 lights (ambient, directional) are shaded as if a medium
    vertex were a surface, and their shadow ray is not medium-attenuated.
    NEW, UNFIXED — found while fixing item 8.**
    `MediumTransport::EvaluateInScattering{,NM}` builds a synthetic
    `RayIntersectionGeometric` whose `vNormal` is set to the OUTGOING
    direction `wo`, precisely so that the light rows which multiply by
    `Dot(vToLight, vNormal)` produce something sane. The light-table and
    environment rows sidestep the issue properly — they test
    `isVolumeScatter` and force `cos = 1`, leaving the phase function
    (via `MediumScatterBSDF::value`) to supply the angular term. **Step 1
    does not.** `DirectionalLight::ComputeDirectLighting` computes
    `fDot = Dot(vDirection, ri.vNormal)` and returns early on
    `fDot <= 0` ([DirectionalLight.cpp:60-66](../src/Library/Lights/DirectionalLight.cpp)),
    so at a volume vertex a directional light illuminates only the
    hemisphere within 90° of `wo` and is scaled by that cosine — neither
    of which has any physical meaning for an isotropic or HG phase
    function. Separately, Step 1's shadow ray goes through
    `ILight::ComputeDirectLighting`'s own `CastShadowRayAuto` and never
    reaches `EvalShadowTransmittance`, so a directional light's
    contribution to a medium is not attenuated by the medium it crosses
    (the light-table and env rows both do apply it). Item 8's fix at
    least makes Step 1 *cast* the ray at volume vertices, so fog behind an
    occluder is no longer lit by a directional light; the cosine gate and
    the missing transmittance remain. Not reachable from
    `VolumeEnvFurnaceTest` (env-only scenes) — a guard would need a
    fog-plus-directional-light furnace, and the fix likely means an
    `isVolumeScatter` parameter on `ILight::ComputeDirectLighting{,NM}`,
    which is a virtual on every light implementation.

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
