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
clamp, altered MIS weight, or material-specific normalization is introduced.
Exactly zero denominator cosines are zero-measure directions and return zero.
The reported ray-facing geometric normal is deliberately preserved (DL-70);
absolute projected measures are invariant to its sign.

## Three defects and separating witnesses

1. **Endpoint response.** BDPT and VCM used raw `fs` with geometric `G`, so
   the constant-tilt plane's response did not change at all with tilt.
   `PathValueOps::EvalAreaBSDFAtVertex` now supplies `fA` at NEE, interior
   connections, light-to-camera splats and VCM merges. Raw material
   evaluations remain available to sampling and guiding. Medium vertices
   retain unit conversion. Density Jacobians and `geomNormal` are untouched.
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
