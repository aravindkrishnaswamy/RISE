# DL-224: shading-normal transport measures

Status: **CLOSED 2026-09-20** for implementation and measured gates; independent
campaign review is required before merge. DL-239 records the separately
confirmed legacy photon-map pipeline; DL-223's nonreciprocal translucent
model and connection-distance limitations are not closed by this work.

## Independent derivation

Let `wi` point toward incident light and `wo` toward the sensor. The material
BSDF is expressed in the shading frame. Radiance transport is
`Lo(wo) = integral fs(wi,wo) Li(wi) |Ns.wi| dwi`, over the material's supported
hemisphere(s). This is a shading-normal model, not a general energy
conservation theorem. A tilted Lambertian under unit head-on irradiance has
`Lo = rho*cos(tilt)/pi`; a white furnace is not a valid unity oracle for
arbitrary shading-normal perturbations.

A connection's geometric coupling and its area-density Jacobians involve
`Ng`, because geometry fixes the actual differential surface area. Therefore
its response must be `fA = fs*|Ns.wi|/|Ng.wi|`. Substituting gives the same
radiance integral without altering any geometric density. A light walk
samples `wo`, and its SPF weight is `fs*|Ns.wo|/pdf(wo)`. The corresponding
importance weight needs
`C = |Ns.wi|*|Ng.wo| / (|Ng.wi|*|Ns.wo|)`.

This independent derivation agrees with the shading-normal adjoint in
[PBRT's path-space measurement equation](https://pbr-book.org/3ed-2018/Light_Transport_III_Bidirectional_Methods/The_Path-Space_Measurement_Equation).
Its direction names differ from RISE's light-walk convention. No epsilon,
clamp, or material-specific normalization is introduced. The separate VCM
recurrence correction below restores the densities used by MIS.
Exactly zero denominator cosines are zero-measure directions and return zero.
The reported ray-facing geometric normal is deliberately preserved (DL-70);
absolute projected measures are invariant to its sign.

## Four defects and separating witnesses

1. **Endpoint response.** BDPT and VCM used raw `fs` with geometric `G`, so
   the constant-tilt plane's response did not change at all with tilt.
   `PathValueOps::EvalAreaBSDFAtVertex` now supplies `fA` at NEE, interior
   connections, light-to-camera splats and VCM merges. Raw material
   evaluations remain available to sampling and guiding. Medium vertices
   retain unit conversion. The geometric Jacobian contract and `geomNormal` are preserved.
2. **Importance continuation.** The shared BDPT light generator priced its
   selected SPF lobe exactly as a radiance walk. It now applies `C` before
   roulette, including the local guiding weight and all HWSS companions.
   Companion replay rescales an already corrected base throughput by
   wavelength ratios; `C` is wavelength-independent and cancels from those
   ratios. This fixes BDPT, VCM and MLT consumers of the generator.
3. **PT grazing frame.** Ordinary two-sided BRDFs/SPFs flip the shading
   hemisphere to face the incoming view ray. NEE used the stored normal's
   signed cosine instead. When strong relief put both light and view below
   Ns but above Ng, the BSDF supported the path while NEE rejected it.
   `RayIntersectionGeometric::RayFacingShadingCosine` makes the signed
   cosine agree with that frame. Opposite-hemisphere rejection remains;
   full-sphere materials retain their explicit absolute-cosine opt-in.
   Modern Pel/NM (and HWSS callers) and legacy point/spot/directional
   Pel/NM direct-light consumers share the correction.

4. **VCM density recurrence.** `ConvertEyeSubpath`, `ConvertLightSubpath`
   and their BSSRDF entry helper passed `|Ns.wo|` into the onward sampling
   update. That update transports geometric area-density ratios, whose
   Jacobian is `|Ng.wo|`. The stored incoming `cosAtGen` and PDF inversions
   already used Ng, so mixing in Ns changed MIS weights even with every
   path density held fixed. The three surface cosine producers now use
   `AreaToSolidAngleFactor`; the two medium callers still use `sigma_t`.
   Delta and diffuse branches share the corrected producers. Pel/NM/HWSS
   share the same postpass. No sampling distribution is changed.

The direct-only legacy shader is a suitable independent estimator only for
these delta-light single-plane fixtures; it is not used as a GI reference.

## Red evidence

All test sources were committed before their corresponding correction, and
library plus exact test target were separately built before each run.
The first red fixture's two setup revisions corrected native quad parameter
names and explicit linear reflectance; those parse failures are not physics
red proofs.

At `4f8ee051`, the flat-plane test measured the following, 32×32, 64 spp,
three repeats per estimator. PT and legacy direct agreed with the independent
expected values to 0.0034%; sample SD was below 5e-10 at the printed precision.
The finite point source is 1000 units away; the residual off-axis correction
is smaller than 0.004% and is not the source of the bidirectional error.

| Tilt | Expected | BDPT | BDPT relative error |
|---:|---:|---:|---:|
| 0° | 0.254647909 | 0.254647790 | −0.000047% |
| 10° | 0.250779235 | 0.254647790 | +1.543% |
| 20° | 0.239290761 | 0.254647790 | +6.418% |
| 30° | 0.220531558 | 0.254647790 | +15.470% |
| 45° | 0.180063263 | 0.254647790 | +41.421% |

VCM stayed at 0.254651300 throughout. The 2% rendered gate catches 20/30/45°;
separate analytic factor assertions cover 1° and 10° to 1e-14. Red count:
**74 passed / 6 failed**. The original sphere reproduced at `af276131`:
200×200, 256 spp, central 80×80, n=3, scale −0.20: PT 0.040806370
(SD 0.000002361), BDPT 0.024928433 (SD 0.000042998), VCM 0.026148534
(SD 0.000020685). Sphere red: **29 passed / 4 failed**.

The live light-walk witness at `56c5ea43` samples an actual point-light
subpath through a 30° Lambertian plane, with an enclosing sphere catching
the continuation. Of 2048 generated paths, 530 expose the post-scatter
throughput; worst relative error against projected power was **74.8137388**.
After the adjoint correction, the fixture's six-digit serialization of
`tan(30°)` limited the oracle to 3.25e-5. Writing 17 digits instead yields
**7.70872255e-12**, without changing the gate or excluding grazing samples.

The grazing witness at `cae8c3d1` has 45° Ns tilt and both camera and point
light below Ns but above Ng. Expected `.8*sin(15°)/pi = .065907728631`:
PT **0**, BDPT **.0659077747726**, VCM **.0659087067273**. The view-facing
cosine makes PT match; a second light direction in the opposite shading
hemisphere remains dark. This rules out an unconditional fabs repair.

The density witness was committed at `9254607c` before the recurrence fix.
Three vertices are at x=0,2,5, with geometric normals along x and fixed
area PDFs .1/.2 at the two surfaces. Rotating only the middle shading
normal to 60° must leave every density ratio unchanged. Instead dVC/dVM
halved: eye diffuse dVC **11111.1111111 → 5555.55555556**, light diffuse
**9.77777777778 → 4.88888888889**, light delta **8 → 4**, and eye/light
BSSRDF entries **5.55555555556 → 2.77777777778**. The eye diffuse number
is independently `10000/2 * 4 / (.2*9)`; no BSDF response participates.
Red **16/5**, green at `1782074c` **21/0**, all discrepancies exactly zero.
The seven production recurrence callers consist of those five surface
branches and two medium branches; the latter retain their extinction
measure.

## Matched spectral and sibling controls

The 30° plane is independently `.8*cos30/pi = .220531558169`. Both hero
and four-lane HWSS scene strings explicitly use `num_wavelengths 160`.
All raw repeats are retained, with n=3 per cell and sample SD:

| Integrator | HWSS | 64 spp mean (SD) | 128 spp mean (SD) |
|---|---|---:|---:|
| pathtracing | 0 | 0.219665600 (0.000804886) | 0.220738584 (0.000502942) |
| pathtracing | 1 | 0.220494410 (7.65972e-05) | 0.220481427 (0.000159535) |
| bdpt | 0 | 0.220365129 (0.000745295) | 0.220821538 (0.000335325) |
| bdpt | 1 | 0.220633499 (0.000204996) | 0.220564044 (0.000118784) |
| vcm | 0 | 0.221154825 (0.000705303) | 0.220853951 (0.000699238) |
| vcm | 1 | 0.220624485 (7.07256e-05) | 0.220621012 (7.0091e-05) |

Every cell passes the 2% closed-form gate; the isolated spectral invocation
has **69 passed / 0 failed**. A constant 45° normal map gives PT/BDPT/VCM
`.180057149/.180057148/.180059612`; a truly displaced planar field gives
`.180028873/.180028872/.180031372`, against `.180063263231` in both cases.
These are single-render closed-form controls, not estimates of run-to-run
noise. The slight finite-source variation is inside the derived bound.
The corrected sibling fixture has **33/0**; an earlier declaration-order
setup failure is retained separately and is not transport evidence.
The legacy bump API is an ABI shim onto relief's UV mode; `ReliefModifierTest`
checks the exact scale/window fold against the removed bump implementation.

## Sphere density A/B

Separate executables from committed `9254607c` (before the density correction)
and `1782074c` (after) were alternated AB/BA, at 200×200, 512 spp, central
80×80, n=3, no denoising, box filter, linear RGB EXR. The scene body is extracted
verbatim from the committed sphere fixture. Each output path is unique and
new. The interactive CLI deliberately returns **1** after `quit` (see the
last return in `src/RISE/commandconsole.cpp`); fresh EXR creation, finite
pixels and completion messages establish successful rendering. Two earlier
output-directory setup failures are preserved outside the repository and
excluded from the successful batch for that explicit setup reason.

| Scale | Estimator | Before mean (SD) | After mean (SD) |
|---:|---|---:|---:|
| +0.00 | pathtracing | 0.050307289 (5.05957e-07) | 0.050305023 (1.36489e-06) |
| +0.00 | bdpt | 0.050310541 (4.26497e-06) | 0.050312806 (2.70363e-06) |
| +0.00 | vcm | 0.050335349 (3.10384e-06) | 0.050333668 (2.93846e-06) |
| -0.20 | pathtracing | 0.042495581 (2.2955e-06) | 0.042495902 (9.51549e-06) |
| -0.20 | bdpt | 0.042508298 (1.6929e-05) | 0.042483153 (2.85365e-05) |
| -0.20 | vcm | 0.041598046 (1.97042e-05) | 0.042489958 (1.08875e-05) |
| +0.20 | pathtracing | 0.042167064 (6.19759e-06) | 0.042162837 (2.43314e-06) |
| +0.20 | bdpt | 0.042168297 (1.0253e-05) | 0.042148227 (1.49396e-05) |
| +0.20 | vcm | 0.041287226 (2.20025e-05) | 0.042152183 (7.00636e-06) |

The VCM discrepancy collapses under the independently proved density fix.
The earlier 256-spp in-process PT value at scale −.20 is `.042136507`
(SD `2.03024e-5`). Matched CLI256 reproduces it with both minimal options
`.042145846` (SD `1.91790e-5`) and shipped global options `.042122270`
(SD `1.63687e-5`), n=3 each. The serialized geometry/material/light/camera
and rasterizer inputs are identical after whitespace, scale formatting and
sample-count normalization. Central-region EXR alpha is one. This excludes
an output/harness/options explanation; the density fix cannot change PT.

The sample-count effect is **finite fixed-QMC integration error**, not an
error bound given by the tiny render-repeat SD. `PathTracingPelRasterizer.cpp`
derives the Owen scramble solely from pixel coordinates (around lines305–329),
and ZSobol selects a different Morton sample block as `log2(spp)` changes
(around lines413–421). Calling `srand` randomizes camera jitter and other
random work, but does not randomize that transport quadrature. The following
controls retain every raw result, n=3, and report conditional repeat SD:

| PT sampler / spp | Scale −.20 mean (SD) | Scale +.20 mean (SD) |
|---|---:|---:|
| sobol256 | 0.042493443 (1.4464e-05) | 0.042265137 (2.07785e-05) |
| z1024 | 0.042504855 (9.7126e-06) | 0.042148820 (1.05095e-06) |
| z4096 | 0.042493665 (3.78006e-06) | 0.042150197 (2.73585e-06) |

The larger budgets converge to the BDPT/VCM512 values; changing the sampler
also removes the negative-scale discrepancy at256. The positive-scale
Sobol256 cell still has finite quadrature error and is not discarded.
No RNG API, test band or estimator normalization was changed. The original
256-spp broad parity gate remains, backed by the closed-form plane and these
higher-budget controls, not by treating repeat SD as the full integration error.

The full new regression is **213 passed / 0 failed**. A subsequent test-only
logging change preserves all530 accepted live-light samples (direct invocation
**24/0**): projected SPF weight min `.01055217595376469`, mean `.7112194679039587`,
sample SD `.672095478272167`, max `7.714286818685685`; three exceed4. No sample
is clipped. Each row records wi, wo, independently expected weight and actual
weight with17 digits.


## Synthetic BSSRDF entry frames (2026-09-20)

The view-facing NEE correction exposed two pre-existing malformed synthetic
records. A RIG's ray is incoming. PT's diffusion and random-walk entry
constructors instead stored the outgoing sampled continuation. BDPT's
zero-exitance sweep used the chord toward the nonlocal diffusion exit as
its local view direction. Neither represents the entry adapter's contract:
`Sw(wi)` has fixed outward support `Ns.wi > 0`, independent of that chord.
`BSSRDFAdapters::EntryEvaluationRay(position, Ns)` now supplies incoming
`-Ns` at those three consumers only. Ordinary BDPT vertices retain `-wo`.
Geometric normals, entry area densities, continuation rays and weights are
unchanged.

The trained NEE moment for a fixed sample is `(Rd * K / p)^2`; doubling
`Rd` must multiply it by four when the sampling and other factors are held
fixed. At committed `a423ea7c`, `OptimalMISTrainingSitesTest` was **110/1**:
BSDF moments 1325.8→5302.7 (3.99962), but NEE 5.15149→5.15149 (1).
The separately rebuilt committed raw-cosine diagnostic `ee55e57b` was
**111/0**, NEE ratio **3.99567**. Restoring the final view-facing helper and
correcting the two PT producers (`bc227c9b`) was also **111/0**. Thus this
was a producer-frame regression, not a tolerance or training-law change.

Separately, `BDPTZeroExitanceBSSRDFTest` at `bc227c9b` was **21/4**:
diffusion PT mean luminance **1.37202**, BDPT **0**, and MLT zero-luminance
bootstrap; random-walk PT **0.288554**, BDPT **0**, MLT likewise zero.
The ambient and ordinary Lambertian controls remained lit. The fixed-frame
helper is checked directly for incoming direction and outward/inward
signed cosines, then against real diffusion and random-walk adapters in
Pel and NM at 400/500/600/700 nm. These assertions do not depend on render
noise. The PT Pel/NM producers share the template; HWSS entry and mid-path
SSS fallback delegate each surviving wavelength to NM. BDPT's tagged
zero-exitance sweep shares the adapter construction across Pel/NM and
HWSS companion evaluation. Legacy SSS and Donner-Jensen irradiance sampling already construct
`Ray(samplePosition, -normal)` and need no change. Other legacy direct-light
callers use live hit records; volume callers retain the explicit volume
cosine of one.

At final production `d68e5674`, the frame suite is **41/0**, including
12 exact assertions: diffusion PT/BDPT/MLT luminance
**1.37330 / 1.37581 / 1.36713**; random walk
**0.288907 / 0.289501 / 0.288304** (one gate run, not a repeated-mean claim).
Optimal MIS is **111/0**, NEE moment **3565.15→14245.1**, ratio **3.99567**.
The initially failed test-launch command addressed the make target rather
than the `bin/tests` executable; its orchestration failure is retained
separately from these actual successful runs.

## Legacy photon sibling: DL-239

This is a separate, confirmed pipeline gap; none of the bidirectional
adapter changes affects it. Reconstruct the independent direct witness:
create `CausticPelPhotonMap(4096,nullptr)`; store unit RGB-power photons at
`(0.01*x,0.01*y,0)` for integer x,y in [−25,25], all incident directions
`(0,0,1)`; balance; set gather parameters `(0.2,0.05,10,400,nullptr)`.
Use Lambertian reflectance .8, query point zero, ray `(0,0,1)→(0,0,−1)`,
Ng=+Z. Evaluate once at Ns=+Z, then at Ns=(sqrt(.5),0,sqrt(.5)), rebuilding
the ONB each time. Both radiances are **2038.42827292**, ratio **1**;
independently, the common photon density and kernel cancel and the correct
ratio is **sqrt(.5)**. No random rendering is involved.

`PhotonMap.h::RadianceEstimateFromSearch` multiplies stored flux density by
raw `brdf.value`, shared by caustic and non-precomputed global Pel gathers.
`GlobalSpectralPhotonMap` and `CausticSpectralPhotonMap` duplicate the raw
value/valueNM pattern. `GlobalPelPhotonMap::PrecomputeIrradiance` collapses
incident directions, so the later cached gather cannot apply a
per-direction correction without a revised cache contract.
`TranslucentPelPhotonMap` also has direction-free storage/gather, but its
SSS transport model requires its own oracle. The photon tracer family's
raw-kray continuation is another audit candidate; the direct-gather witness
does not independently prove its transport-mode correction.

External evidence is under `/private/tmp/rise-debt-codex-20260919/dl224`;
`photon-proof.cpp`, build log and run log preserve the direct witness.

## Final build and gate provenance

The final production source is `d68e5674`; `0d1dde5b` adds only documentation
and a test-header correction. The clean make library build and 14 selected
named relinks all returned **0**, with **zero compiler warnings** in each
log (`final2-build-status.json`). The supervisor's isolated clean macOS
Deployment and Opto builds both returned **0**, each with two documented
build-system exclusions and **zero actionable warnings**; their raw logs
and `results.json` are in the adjacent `dl224_xcode_final` evidence directory.
The earlier `a423ea7c` platform run is historical, superseded by this gate
after the synthetic BSSRDF frame correction.

Before that narrow frame correction, the recovered broad batch passed
BDPTStrategyBalance **170/0**, VCMStrategyBalance **74/0**, EnvLightBalance
**123/0**, PTGuidingMISPartition **101/0**, VolumeEnvFurnace **32/0**,
HairDirectionalBacklit **7/0**, DirectionalFog **14/0**, HairRender **29/0**,
TransparentShadow **40/0**, LegacyChainMISPartner **20/0**,
BidirectionalTextureFootprintParity **7/0**, and GeomNormalOrientationSites
**72/0**. The only failure was OptimalMIS **110/1**, retained and resolved
by the committed frame A/B above. The source-sensitive core gates are run
again on the final library; the earlier raw logs are not overwritten.

Other direct gates on the corrected transport source passed MISWeights
**59/0**, ConnectionLegality **319/0**, ReliefModifier **158/0**,
SourceHygiene **167/0**, BDPTVertexRIGRebuild **68/0**,
GlintModifierSceneParse **19/0**, LightColorSpace **133/0**,
HWSSCompanionKray **169/0**, TranslucentLobeConsistency **1222/0**,
VCMRecurrence **75/0**, eye postpass **31/0**, light postpass **34/0**, and
spectral recurrence **38/0**. PathValueOps, GlintModifier, DisplacedGeometry
and LightExitance reported all passed. CstDeriveGolden reported **452
MATCH / 0 DRIFT**, **459** corpus files, **0** uncovered and **0** stale.
Their named build/run logs and manifests retain exact source-stage
provenance. No new library source file was added.

On the final library, the complete relief suite is **213/0**. The repeated
strategy/caller batch is also green: BDPT **170/0**, VCM **74/0**, OptimalMIS
**111/0**, BSSRDF frame **41/0**, EnvLightBalance **123/0**, PTGuiding
**101/0**, VolumeEnvFurnace **32/0**, footprint parity **7/0**, normal
orientation **72/0**, SourceHygiene **167/0**, HWSS companion **169/0**, and
Translucent lobe **1222/0**. The four explicitly required direct targets
were additionally relinked against this library: MISWeights **59/0**,
ConnectionLegality **319/0, 0 skipped**, ReliefModifier **158/0**, and CST
**452 MATCH / 0 DRIFT** with the same 459/0/0 coverage counts. All returned
zero; every named relink had zero warnings. See `mandatory-final-status.json`
and `final2-run-status-ReliefBidirectionalConsistencyTest.json`.

The historical first full unfiltered SignalIntegratorConsistency run at
`98eb3998` returned **0**,
**2971 passed / 0 failed**. Counters: blow-up skips **0**, masked precision
skips **0**, reference-incomplete rows **2**, no-complete-reference skips
**1**, whole-image-insensitive showcases **4**, mask coverage drops **0**.
Its log reported all six asserted adaptive masked rows at **12** sub-renders.
The following are historical reported aggregates, not independently
reconstructable masked statistics from that log:

| Showcase | Integrator | Mean masked contrast | Standard error |
|---|---|---:|---:|
| plank | BDPT | .0273271 | .0258162 |
| plank | VCM | .00965227 | .00508000 |
| bunny | BDPT | .00737977 | .000887211 |
| bunny | VCM | .00655691 | .00109975 |
| pavilion | BDPT | −.00308900 | .00397448 |
| pavilion | VCM | −.00214093 | .00370744 |

**Tidal's masked transport is unvalidated by this suite.** Its neutral
masked means were PT **.00127229**, BDPT **.00311716**, VCM **.054681**;
BDPT/VCM **.0570062** triggers the existing symmetric incomplete-reference
rule. This is the documented delta-light-through-dielectric-water caustic
case. [RENDERING_INTEGRATORS.md debt 30](RENDERING_INTEGRATORS.md)
diagnoses the structural gap: the E–S–D–S–L path has no connectible BDPT
split, while VCM merges at the diffuse stone. That debt is resolved as a
strategy limitation (with its separate radiance-scaling defect already
fixed), not an open DL224 MIS defect. The older reference-rule account is
in [SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §6.2](SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md). Neither
that skip nor four insensitive whole-image rows proves showcase correctness.
The first log and `signal-adaptive-summary.txt` retain whole-image means
and reported masked aggregates, but **not the raw masked operands**. The
earlier raw-retention claim is withdrawn: that first run cannot independently
reconstruct masked mean/SD/SE or stopping counts. No retry or threshold change
was used in that historical run. The corrective evidence below is separate.

## Review follow-up: independently reconstructable masked evidence

The supervisor relayed the independent contracts review's **P2 evidence
finding** on `98eb3998`; this was an external review finding, not an author
measurement. It correctly identified the missing raw masked operands in
the historical 2971/0 record above. Logging-only test commit `350c2638` adds
round-trip (17-digit) `MASK_AUDIT` records for the complete fixed mask pixel
indices/dimensions, every masked numerator E/B and paired PT denominator,
zero-based sample and attempt indices, explicit null/failure records,
and each adaptive row's actual count/mean/SD/SE, limits and stopping reason.
The alternate PT-independent self-contrast loop has the same tracing.
Its denominator fields are null because that estimator has no PT pair.

The exact source patch is `mask-audit-source.patch`. Removing only the
explicitly added serializers, mask-serialization block, logging calls and
diagnostic counters reproduces the original test text ignoring whitespace
(`mask-audit-semantic-check.json`). Sampling, RNG calls, failure increments,
thresholds, return paths and stop predicates are unchanged. No production
`src/build` file changed; the Deployment/Opto source identity remains valid.

The exact named target was relinked with **rc0 / zero warnings**. One new run
was made, with **no retries**, using:

```sh
SIGNAL_CONSISTENCY_FILTER='plank,bunny,pavilion' ./bin/tests/SignalIntegratorConsistencyTest
```

The filter uses substring matching (`find(keyword)`), not a comma-list
parser; that value nevertheless selects exactly the three named showcases.
The optional positional argument sets the seed base only. It was omitted,
retaining default **1000**. Resolution **160×120**, **32 spp**, minimum **12**,
attempt cap **48**, band **.2**, SE target **.05**, mask threshold **.2** and
coverage floor **.01** are unchanged. Filtering changes the render-index
sequence relative to the first full run, so these are separately labelled
measurements. Unit fixtures and tidal were excluded from this corrective
run; their first full-run coverage and limitations remain historical.

Result: **rc0, 2836 passed / 0 failed**, 141.85 seconds. All six adaptive rows
were asserted, **12 samples / 12 attempts / 0 failures**, each stopping at
`precision_target`. Whole-image-insensitive count **3**; all other skip
counters **0**. The raw log contains **72 paired sample records**, **36 PT-pool
records**, **three complete mask index lists**, and **six stopping records**.
No failure occurred in this run; failure paths now explicitly serialize
missing operands as null rather than losing the attempt.

The separate Python reader `recompute-mask-audit.py` computes each contrast
as `E*PT_B/(B*PT_E)-1`, then sample SD and SE from all samples. It checks
mask indices/count/dimensions, exact shared PT-pool operand identity,
sequential attempt/sample indices, failure counts, and every prefix against
the unchanged stopping rule: no row continues after its first eligible
precision stop. It also recomputes each operand stream's mean/sample SD.
All six rows reconstructed successfully; the largest difference from a
logged aggregate is below **6e-17**. Full operands, derived contrasts,
prefix decisions and mask identities are retained in
`mask-audit-records.json` and `mask-audit-recomputed.json`.

| Showcase | Mode | n | Recomputed mean contrast | Sample SD | SE |
|---|---|---:|---:|---:|---:|
| plank | BDPT | 12 | -0.0380110817792 | 0.0648754060914 | 0.0187279165853 |
| plank | VCM | 12 | 0.00952887794583 | 0.0165876829349 | 0.00478845160386 |
| bunny | BDPT | 12 | 0.00554136808178 | 0.00335845350708 | 0.000969502018187 |
| bunny | VCM | 12 | 0.00730307776415 | 0.00481832117635 | 0.0013909295141 |
| pavilion | BDPT | 12 | 0.00490249241503 | 0.0125225420684 | 0.00361494651707 |
| pavilion | VCM | 12 | 0.00334451516647 | 0.0142366022136 | 0.00410975306018 |

The earlier six aggregate values are not retroactively treated as verified
by these new samples. Their missing raw-data limitation is preserved.

## Every shipped relief scene: before/after

All ten scenes declaring `relief_modifier` were rendered in both PT and
BDPT, **128×96, 32 spp, n=3 per state/integrator/scene**, OIDN off, box
filter, 32-bit EXR `Rec709RGB_Linear`. Each table reports the arithmetic
mean of stored linear RGB, averaged over pixels, **mean ± sample SD**
across the three renders. The center region is the central half-width and
half-height (64×48); four quadrant means/SD, per-channel means and every
raw value are retained in `ship-raw.jsonl` / `ship-summary.json` alongside
all 120 EXRs. This low-budget sweep is a descriptive change measurement,
not an independent correctness oracle or a claim of converged radiance.
Repeated SD is conditional on fixed transport QMC, as discussed above.

Separate binaries were interleaved AB/BA/AB within each scene/integrator.
The baseline `rise-base` was built at committed `af276131`, whose `src` and
`build` trees are identical to original `8ea9cd8d`; final
`rise-final-d68e5674` is the clean final production build. SHA256 values and
source identity are in `binary-provenance.json`. The runner refused an
existing output, checked clean scene derivation (no partial-scene fallthrough),
expected dimensions, render completion and finite freshly written EXRs.
All **120/120** passed; the runner returned **0**. Interactive CLI `quit`
returns **1** by design, retained for each successful child rather than
silently normalized. No sample or setup failure was discarded in this sweep.

Scene geometry, materials, camera and environment are preserved; the
rasterizer is replaced by plain PT or BDPT at the stated budget, retaining
its environment/default-shader settings. Engine-specific options such as
SMS are not carried into that plain-integrator comparison. The two
`ChunkCoverage` files are grammar fragments without camera/light: their
unchanged objects are placed in an explicit common harness, camera
(0,0,5) looking at the origin, fov40, and a unit-white point light of power100
at (0,3,4), with `DefaultDirectLighting` as the legacy fallback shader.
Those rows are labelled as fragment measurements, not original showcases.

### Whole image

| Scene | Mode | Before mean ± SD | After mean ± SD | Change |
|---|---|---:|---:|---:|
| [sculptors_studio](../scenes/FeatureBased/Combined/sculptors_studio.RISEscene) | PT | 0.17830557 ± 5.24e-05 | 0.17845589 ± 0.000482 | +0.0843% |
| [sculptors_studio](../scenes/FeatureBased/Combined/sculptors_studio.RISEscene) | BDPT | 0.17268845 ± 0.00291 | 0.17465479 ± 0.00154 | +1.1387% |
| [velvet_cushion](../scenes/FeatureBased/Materials/velvet_cushion.RISEscene) | PT | 0.10558953 ± 2.54e-05 | 0.10561862 ± 1.23e-05 | +0.0276% |
| [velvet_cushion](../scenes/FeatureBased/Materials/velvet_cushion.RISEscene) | BDPT | 0.1094287 ± 1.53e-05 | 0.10933319 ± 1.71e-05 | -0.0873% |
| [plank_closeup](../scenes/FeatureBased/Textures/plank_closeup.RISEscene) | PT | 0.18813113 ± 8.41e-05 | 0.18819344 ± 9.3e-05 | +0.0331% |
| [plank_closeup](../scenes/FeatureBased/Textures/plank_closeup.RISEscene) | BDPT | 0.19473284 ± 0.00143 | 0.19524236 ± 0.000779 | +0.2616% |
| [weathered_workbench](../scenes/FeatureBased/Textures/weathered_workbench.RISEscene) | PT | 0.3242208 ± 7.71e-05 | 0.32420709 ± 8.49e-05 | -0.0042% |
| [weathered_workbench](../scenes/FeatureBased/Textures/weathered_workbench.RISEscene) | BDPT | 0.33230456 ± 4.32e-05 | 0.33227614 ± 0.000151 | -0.0086% |
| [cc_modifier_stack](../scenes/Tests/ChunkCoverage/cc_modifier_stack.RISEscene) | PT | 0.10796264 ± 7.89e-06 | 0.10794664 ± 6.41e-06 | -0.0148% |
| [cc_modifier_stack](../scenes/Tests/ChunkCoverage/cc_modifier_stack.RISEscene) | BDPT | 0.10757548 ± 5.25e-06 | 0.10794438 ± 7.49e-06 | +0.3429% |
| [cc_relief_modifier](../scenes/Tests/ChunkCoverage/cc_relief_modifier.RISEscene) | PT | 0.10728028 ± 6.88e-06 | 0.10727424 ± 9.06e-06 | -0.0056% |
| [cc_relief_modifier](../scenes/Tests/ChunkCoverage/cc_relief_modifier.RISEscene) | BDPT | 0.10690722 ± 1.37e-05 | 0.10727179 ± 8.92e-06 | +0.3410% |
| [displaced_plus_relief_shared_field](../scenes/Tests/Painters/displaced_plus_relief_shared_field.RISEscene) | PT | 0.1379224 ± 1.57e-05 | 0.13792691 ± 2.2e-05 | +0.0033% |
| [displaced_plus_relief_shared_field](../scenes/Tests/Painters/displaced_plus_relief_shared_field.RISEscene) | BDPT | 0.13792676 ± 2.25e-06 | 0.13791193 ± 1.11e-05 | -0.0108% |
| [relief_crackle_glaze](../scenes/Tests/Painters/relief_crackle_glaze.RISEscene) | PT | 0.058832375 ± 9.15e-05 | 0.058793613 ± 2.48e-05 | -0.0659% |
| [relief_crackle_glaze](../scenes/Tests/Painters/relief_crackle_glaze.RISEscene) | BDPT | 0.058806328 ± 4.07e-05 | 0.058821628 ± 5.92e-05 | +0.0260% |
| [relief_sphere_no_uv](../scenes/Tests/Painters/relief_sphere_no_uv.RISEscene) | PT | 0.12870221 ± 1.08e-05 | 0.12868684 ± 1.71e-05 | -0.0119% |
| [relief_sphere_no_uv](../scenes/Tests/Painters/relief_sphere_no_uv.RISEscene) | BDPT | 0.12870065 ± 1.28e-05 | 0.12868881 ± 6.75e-06 | -0.0092% |
| [sms_veach_egg_bumpmap](../scenes/Tests/SMS/sms_veach_egg_bumpmap.RISEscene) | PT | 0.76127026 ± 0.0124 | 0.75892208 ± 0.0042 | -0.3085% |
| [sms_veach_egg_bumpmap](../scenes/Tests/SMS/sms_veach_egg_bumpmap.RISEscene) | BDPT | 0.76893112 ± 0.00571 | 0.7689226 ± 0.00165 | -0.0011% |

### Center region

| Scene | Mode | Before mean ± SD | After mean ± SD | Change |
|---|---|---:|---:|---:|
| sculptors_studio | PT | 0.49376857 ± 0.000551 | 0.49458035 ± 0.00196 | +0.1644% |
| sculptors_studio | BDPT | 0.46719271 ± 0.0115 | 0.47510182 ± 0.00636 | +1.6929% |
| velvet_cushion | PT | 0.056692581 ± 4.93e-05 | 0.056707634 ± 3.6e-05 | +0.0266% |
| velvet_cushion | BDPT | 0.061479709 ± 5.71e-05 | 0.061275264 ± 7.69e-05 | -0.3325% |
| plank_closeup | PT | 0.27039662 ± 0.000158 | 0.27044171 ± 0.000278 | +0.0167% |
| plank_closeup | BDPT | 0.27683549 ± 0.00542 | 0.28084211 ± 0.00196 | +1.4473% |
| weathered_workbench | PT | 0.26630252 ± 5.91e-05 | 0.26638091 ± 8.05e-05 | +0.0294% |
| weathered_workbench | BDPT | 0.27819556 ± 0.000102 | 0.27802797 ± 0.000281 | -0.0602% |
| cc_modifier_stack | PT | 0.42033142 ± 2.57e-05 | 0.42030186 ± 2.16e-05 | -0.0070% |
| cc_modifier_stack | BDPT | 0.41883227 ± 2.63e-05 | 0.42029138 ± 6.35e-06 | +0.3484% |
| cc_relief_modifier | PT | 0.41765297 ± 2.21e-05 | 0.41763705 ± 3.05e-05 | -0.0038% |
| cc_relief_modifier | BDPT | 0.41613394 ± 3.72e-05 | 0.41761437 ± 2.54e-05 | +0.3558% |
| displaced_plus_relief_shared_field | PT | 0.28262998 ± 5.41e-06 | 0.28263137 ± 3.03e-05 | +0.0005% |
| displaced_plus_relief_shared_field | BDPT | 0.28262243 ± 1.41e-05 | 0.28262656 ± 1.91e-05 | +0.0015% |
| relief_crackle_glaze | PT | 0.19919133 ± 0.000338 | 0.19905131 ± 4.41e-05 | -0.0703% |
| relief_crackle_glaze | BDPT | 0.19913186 ± 0.000112 | 0.19915158 ± 0.000227 | +0.0099% |
| relief_sphere_no_uv | PT | 0.25088377 ± 8.18e-07 | 0.25088423 ± 5.24e-07 | +0.0002% |
| relief_sphere_no_uv | BDPT | 0.25088306 ± 1.24e-06 | 0.25088238 ± 1.36e-06 | -0.0003% |
| sms_veach_egg_bumpmap | PT | 1.881016 ± 0.00953 | 1.8619568 ± 0.023 | -1.0132% |
| sms_veach_egg_bumpmap | BDPT | 1.8954616 ± 0.0206 | 1.8884196 ± 0.00456 | -0.3715% |

The controlled 45° plane's 41.4% pre-fix error does not imply a comparable
whole-image change in every showcase. For example, sculptor BDPT's +1.139%
change is small against its run-to-run SD; plank's +.262% is likewise
noise-sized. The two fragment BDPT means move +.341–.343% toward PT, whose
means remain nearly unchanged. No significance or all-scene integrator
parity claim is inferred from the low-budget sweep. Its timing fields are
retained as raw provenance only; isolated cost is measured separately below.

## Isolated cost

`plank_closeup`, **256×192, 128 spp, n=3 per state/mode**, the same OIDN-off,
box-filter, linear EXR settings and separate committed binaries as the
shipped sweep. The supervisor and other worker explicitly confirmed CPU
idle before this batch. Order was AB/BA/AB for each integrator. Each sample
starts a new CLI process; wall time and child user+system CPU include
startup, scene loading, rendering and EXR writing. No warmup or outlier
sample was removed. All **12/12** clean-load/fresh-output checks passed;
runner return code **0**, expected interactive-child return code **1**.

| Mode | Before wall, s | After wall, s | Wall change | Before CPU, s | After CPU, s | CPU change |
|---|---:|---:|---:|---:|---:|---:|
| PT | 8.940530 ± 0.346139 | 9.026567 ± 0.157538 | +0.962% | 118.178464 ± 4.462684 | 118.781658 ± 2.184177 | +0.510% |
| BDPT | 10.064183 ± 0.097408 | 10.023340 ± 0.166401 | -0.406% | 142.016799 ± 1.134015 | 140.566649 ± 2.026149 | -1.021% |

Numbers are mean ± sample SD. These end-to-end observations show no material
slowdown in this fixture; three samples cannot establish zero overhead or
a precise sub-percent cost change. The complete raw timing and image values
are in `cost-raw.jsonl`, with aggregation in `cost-summary.json` and the
process convention in `cost-status.json`. The source remains identical to
the final Deployment/Opto gate's `d68e5674` production tree.
