# DL-344: `transparent_shadows` and the delta-transmitter partition

Slice `debt-dl344`, 2026-10-02, branched from `master` `8dcf20d69`.
Ledger row: [DEBT_LEDGER.md](DEBT_LEDGER.md) DL-344.  Red-proof:
`tests/TransparentShadowPartitionTest.cpp`.

## 1. The defect

`transparent_shadows TRUE` (a PT-only opt-in) lets an NEE shadow ray pass
a clear specular dielectric (`GetSpecularInfo`: valid, specular,
refracting, clear transmission) STRAIGHT, attenuated by Fresnel
transmittance.  Until this slice it did so for every light arm.  PT's
BSDF-sampled continuation also crosses that dielectric (a delta vertex,
whose MIS partner is "none", weight 1) and reaches an area light or the
environment.  The NEE arm still takes its power-heuristic weight against
the pre-delta vertex's BSDF density, so the two weights sum above one.

Reproduced (salted, n = 4, paired seeds; `TransparentShadowPartitionTest`
with the fix reverted):

| row | fixture | ratio pre-fix |
|---|---|---|
| A | index-1.0 box around camera and white-furnace subject, env, Lambertian | 1.13911 (per-pair sd 0.00009) |
| A | same, random-walk SSS (ior 1.5) | 1.11214 (0.0019) |
| A | same, diffusion SSS (ior 1.5) | 1.11241 (0.0008) |
| A | same, Lambertian, PT spectral | 1.13883 (0.0028) |
| B | same box, 2 x 2 area emitter outside it, black env | 1.98327 (0.0050) |
| C | 1.5 refractor box around the subject only, env; flag TRUE / FALSE | 1.04641 (0.0002) |
| E | PT + SMS, omni light, 1.5 box around the subject; TRUE / FALSE | 2.46008 (0.0067) |
| D | index-1.0 box, OMNI light (control) | 0.99980 (0.0002) |

The ledger's 1.175 / 1.151 / 1.153 were a slab fixture; the sphere here
reads 1.139 / 1.112 / 1.112, matching the DL-315 review's 1.138 / 1.113 /
1.112.

## 2. Is a transparent-shadow NEE sample through a refracting dielectric a valid strategy?

No.  It is a biased approximation.  The shadow segment crosses an
interface without refracting, so it is not a sample of any physical light
path: the continuation that really reaches the light refracts, lands on a
different light point, and carries the focusing Jacobian the straight ray
omits.  It coincides with a real path only where the interface does not
bend (an index-matched transmitter), and for a parallel slab in front of a
distant light only in direction, not position.  It also omits every
internal Fresnel bounce.

Consequences for a partition:

- Recipe option (a), MIS between the transparent-shadow sample and the
  continuation, is defined only where the two estimate the SAME path
  space: the non-bending (index-matched) case.  Through a refracting
  dielectric they estimate different integrands, so "MIS weights" between
  them are not a partition of anything.  Implementing (a) would also mean
  carrying the pre-delta vertex's position and density through the delta
  chain to every emitter hit and env escape, for a configuration (an
  invisible index-1.0 wall) that has no use.  Declined.
- Recipe option (b), restrict the walk to paths the continuation cannot
  reach, is DL-05's ruling and is what this slice implements.

## 3. The partition

| light | transmitter | estimator(s) | ruling |
|---|---|---|---|
| area (mesh luminary, `rect_light`), env | index-matched, refracting, or a weave gap | PT continuation (unbiased, weight 1 after the delta vertex) | NEE binary; the walk would duplicate it |
| delta (omni, spot, directional) | weave gap (DL-05) | NEE only (no BSDF-sampled strategy hits a delta light) | NEE sees through; exact (non-bending) |
| delta | index-matched | NEE only | NEE sees through; exact |
| delta | refracting | NEE only | NEE sees through as an APPROXIMATION (section 2); the only estimator PT has |
| delta, `sms_enabled TRUE` | refracting / any specular caster | SMS (real refracted chain) | walk OFF: SMS and the walk estimate the same transport |

Implementation:

- `RayCaster::CastShadowRayAuto` / `CastShadowRayAutoSampled`: the
  dielectric walk runs only when `bTransparentShadows && bDeltaLight` --
  the same `bDeltaLight` argument DL-05 introduced.  Delta arms (the
  `LightSampler` delta-light arm, `PointLight`, `SpotLight`,
  `DirectionalLight`) already pass `true`; the mesh-luminary and env arms
  pass `false`.
- `Job` (`SetPathTracing{Pel,Spectral}Rasterizer`): `transparent_shadows`
  is ignored, with a logged warning, when `sms_enabled` is TRUE
  (`TransparentShadowsWithSMS`).
- The `transparent_shadows` parameter description states both rules.

Other integrators: BDPT, VCM and MLT never wire the flag (`Job` sets it
on the PT casters only), so their connections stay binary; BDPT's
zero-exitance sweep calls `CastShadowRayAuto` on its own caster, whose
flag is false.  The legacy shader-op chain (`DirectLightingShaderOp`)
would honour the flag through `LightSampler`, but no legacy rasterizer
sets it.  No change needed in either.

## 4. Results

`TransparentShadowPartitionTest` (salted, n = 4): 24/0 post-fix, 17/7
with `RayCaster.cpp` and `Job.cpp` reverted to `8dcf20d69`.

| row | post-fix ratio (per-pair sd) | band |
|---|---|---|
| A Lambertian / RW / diffusion | 0.99945 / 0.99963 / 0.99940 (0.0002 / 0.0007 / 0.0002) | 0.02 |
| A Lambertian, spectral | 1.0018 (0.0036) | 0.03 |
| B area | 1.00505 (0.0048, 256 spp) | 0.02 |
| C glass, env | 0.99999 (0.00001) | 0.02 |
| D omni control | 0.99988 (0.0008) | 0.01 |
| E PT + SMS | 1.00009 (0.0018) | 0.02 |

Appearance: the only shipped scene with `transparent_shadows TRUE`,
`scenes/FeatureBased/Textures/tidal_stones.RISEscene`, has one omni
light, no area/env light and no SMS, so it takes exactly the old path:
single-threaded pinned-seed hash (`WEAVE_GAP_FILTER=scenehash`, 4 spp)
`25ce34c672b1d931`, mean 0.088998844, bit-identical before and after.
A user scene that combined `transparent_shadows TRUE` with an area or
environment light behind glass gets darker where NEE used to see
through (the double count is removed) and noisier there (the transport
is now carried by the continuation alone, as with the flag off).

## 5. Residuals (no rows)

- The delta-light walk through a REFRACTING dielectric is the documented
  approximation of section 2 (`RayCaster.h`'s `CastShadowRayTransmittance`
  comment).  On row E's geometry without SMS, PT with the walk read
  0.0747 against a VCM 1024-spp reference of ~0.105 (n = 2); part of that
  gap is multi-bounce transport (internal reflections in the box) that no
  PT strategy reaches with a delta light, so it is not a clean measure of
  the approximation alone.  `VITREOUS_ENAMEL.md` section 10.11 already
  treats the flag as preview-only.
- With `sms_enabled TRUE` the walk is off even for chains SMS does not
  cover (beyond `sms_max_chain_depth`, roots its seed misses: DL-372);
  those lose the approximate transport they had.  The alternative -- a
  walk that sees through only non-SMS-casters -- has nothing to see
  through, because every surface the walk passes is an SMS caster.
