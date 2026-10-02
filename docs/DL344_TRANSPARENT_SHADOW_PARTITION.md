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
| omni / spot, `sms_enabled TRUE` | any specular caster | SMS (real refracted chain) | walk OFF for this light: SMS and the walk estimate the same transport |
| directional, `sms_enabled TRUE` | any | NEE only: a directional light is not in `LightSampler`'s `SampleLight` table, so SMS never samples it | walk ON (round 2; round 1 turned it off and the light went black) |

Implementation:

- `RayCaster::CastShadowRayAuto` / `CastShadowRayAutoSampled`: the
  dielectric walk runs only when `bTransparentShadows && bDeltaLight` --
  the same `bDeltaLight` argument DL-05 introduced.  Delta arms (the
  `LightSampler` delta-light arm, `PointLight`, `SpotLight`,
  `DirectionalLight`) already pass `true`; the mesh-luminary and env arms
  pass `false`.
- The SMS gate is PER LIGHT: `RayCaster::DielectricShadowWalk` is
  `bTransparentShadows && bDeltaLight && !(bSMSEnabled && bLightSampledBySMS)`.
  `bLightSampledBySMS` is a new trailing argument of
  `CastShadowRayAuto{,Sampled}`: true from `LightSampler`'s arms (every
  light there is in the `SampleLight` table SMS draws from) and from
  `PointLight` / `SpotLight`, false (the default) from `DirectionalLight`;
  `AmbientLight` casts no shadow ray.  `Job`
  (`WireTransparentShadows`) sets `bSMSEnabled` from `sms_enabled` and logs
  that the flag then applies to directional lights only.
- The `transparent_shadows` parameter description states both rules.

Other integrators: BDPT, VCM and MLT never wire the flag (`Job` sets it
on the PT casters only), so their connections stay binary; BDPT's
zero-exitance sweep calls `CastShadowRayAuto` on its own caster, whose
flag is false.  The legacy shader-op chain (`DirectLightingShaderOp`)
would honour the flag through `LightSampler`, but no legacy rasterizer
sets it.  No change needed in either.

## 4. Results

`TransparentShadowPartitionTest` (salted, n = 4, 30 checks): 30/0 post-fix;
22/8 with every touched source file at `8dcf20d69`; 28/1 at the round-1
head `2b4ce0a9f` (row F).

| row | pre-fix | post-fix ratio (per-pair sd) | band |
|---|---|---|---|
| A Lambertian / RW / diffusion | 1.139 / 1.112 / 1.113 | 0.99943 / 0.99931 / 0.99986 (0.0002 / 0.0011 / 0.0003) | 0.02 |
| A Lambertian, spectral | 1.138 | 0.99882 (0.0034) | 0.03 |
| B area | 1.985 | 1.00382 (0.0060, 256 spp) | 0.02 |
| C glass, env | 1.046 | 1.00000 | 0.02 |
| D omni control | 1.000 | 0.99981 (0.0004) | 0.01 |
| E1 PT+SMS omni, index-1.0 box / open air | 1.501 | 0.50059 (0.0008) -- pinned at 0.5, DL-413 | 0.02 |
| E2 PT+SMS omni, glass TRUE / FALSE | 2.459 | 0.99896 (0.0027) | 0.02 |
| F PT+SMS / PT, directional through glass | 1.000 (0 at round 1) | 1.00005 (0.0008) | 0.02 |

E2 is equal by construction once the walk is off for SMS-sampled lights;
it keeps the pre-fix red.  E1 is the row that checks what SMS returns
against a reference, and it found DL-413 (section 5).

### Shipped scenes

Three scenes set the flag (round 1 reported one: its grep was
case-sensitive and missed `transparent_shadows true`).  Salted n = 4,
OIDN off, linear, 8 x 8 blocks, master `8dcf20d69` vs this head:

| scene | lights through the glass | whole frame | glass region | elsewhere |
|---|---|---|---|---|
| `tidal_stones` | one omni, no SMS | bit-identical (scenehash `25ce34c672b1d931`) | -- | -- |
| `watch_dial` (200 x 200, 128 spp) | env + two softboxes (directional key unaffected) | -2.12 % | dial under the crystal (8 blocks below -5 %) -8.20 %, worst block -13.78 % | within 1.45 % |
| `enamel_depth_glints` (240 x 104, 1024 spp) | env + two panels | -0.30 % | under-slab blocks -2.52 / -4.59 / -4.96 / -1.51 % | within 0.68 % |

User ruling (2026-10-02): re-tune both scenes' lighting scalars to the
pre-fix look within ~1-2 %.  Not applied, because it is infeasible: the
deficit is local to the glass and every lighting scalar also lights the
rest of the frame.  Per-light contributions (half-strength renders,
linear) give, for the watch, whole AND dial within 0.03 % only at
softboxes x1.165 with env + directional x0.71, which moves the blocks
outside the crystal by -26 .. +14 %; the frame-wide minimax is softboxes
x1.05 (whole +1.25 %, dial -5.11 %, worst dial block -10.3 %, outside up
to +4.6 %).  For the enamel the minimax (env x1.025, key x1.015) was
rendered (radiance_scale 1.18, key scale 203): whole +1.64 %, under-slab
-2.95 / -3.69 %, but the untouched blocks go from rms 0.20 % to 1.96 %
(max 2.77 %).  The scenes' comments are corrected; their lighting is
unchanged pending the user.

## 5. Residuals

- **DL-413 (opened)**: PT with SMS delivers HALF an omni light through a
  caster.  Row E1 reads 0.4994 (dielectric wall 0.4988, ior 1.01
  0.5051, `sms_biased FALSE` 0.4991; `sms_seeding uniform` 0.064), and
  the 1.5-box geometry reads PT+SMS 0.0514 against a VCM reference of
  ~0.105.  Plain PT with the walk through the index-1.0 box reads 1.000.

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
