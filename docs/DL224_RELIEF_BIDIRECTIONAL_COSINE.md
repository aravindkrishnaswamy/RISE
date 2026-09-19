# DL-224: shading-normal transport measures

Status: implementation and validation in progress, 2026-09-19. Independent
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
The earlier 256-spp in-process PT table differs from this CLI table;
that harness/sample-count distinction is being characterized separately.
The density correction cannot causally change PT, and this table does not
claim it did.

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
