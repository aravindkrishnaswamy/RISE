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
| omni / spot, `sms_enabled TRUE`, at a SURFACE point where SMS runs (PT's PART-2 NEE) | any specular caster | SMS (real refracted chain) | walk OFF for this shadow ray: SMS and the walk estimate the same transport |
| omni / spot, `sms_enabled TRUE`, at a volume vertex or a BSSRDF / random-walk entry point | any | NEE only: SMS is not evaluated there | walk ON |
| directional, `sms_enabled TRUE` | any | NEE only: a directional light is not in `LightSampler`'s `SampleLight` table, so SMS never samples it | walk ON |

The SMS rule is a statement about an EVALUATION POINT, not a light: SMS
covers "this light through a caster" only where it is actually run.  Two
earlier revisions got that wrong -- round 1 turned the walk off for every
light under SMS (a directional light went black), round 2 for every
SMS-sampled light at every point (SSS-entry and fog receivers went black).

Implementation:

- `RayCaster::CastShadowRayAuto` / `CastShadowRayAutoSampled`: the
  dielectric walk runs only when `DielectricShadowWalk( bDeltaLight,
  bSMSCoversLight )` = `bTransparentShadows && bDeltaLight &&
  !bSMSCoversLight`.  `bDeltaLight` is DL-05's argument (delta arms pass
  true); `bSMSCoversLight` is a new trailing argument, default false.
- `LightSampler::EvaluateDirectLighting{,NM}` gains a trailing
  `bSMSCoversDeltaLights` (default false), forwarded to the delta arm's
  shadow test only.  `PathTracingIntegrator`'s two PART-2 surface NEE
  sites (pel/NM and HWSS) pass `pSolver != 0` -- SMS runs a few lines
  later at the same vertex; every other caller (PT's volume
  in-scattering and BSSRDF entry NEE, the legacy chain, BDPT, the
  lights' own `ComputeDirectLighting`) passes nothing.
- `Job` (`WireTransparentShadows`) logs the SMS interaction when both
  flags are on.
- The `transparent_shadows` parameter description states both rules.

Other integrators: BDPT, VCM and MLT never wire the flag (`Job` sets it
on the PT casters only), so their connections stay binary; BDPT's
zero-exitance sweep calls `CastShadowRayAuto` on its own caster, whose
flag is false.  The legacy shader-op chain (`DirectLightingShaderOp`)
would honour the flag through `LightSampler`, but no legacy rasterizer
sets it.  No change needed in either.

## 4. Results

`TransparentShadowPartitionTest` (salted, n = 4, 42 checks): 42/0
post-fix; 33/9 with every touched source file at `8dcf20d69`; 34/5 at the
round-2 head `5e7bc67cc` (G black, E1/E3 DL-413); 28/1 at the round-1
head `2b4ce0a9f` (F black).

| row | `8dcf20d69` | `5e7bc67cc` | post-fix ratio (per-pair sd) | band |
|---|---|---|---|---|
| A Lambertian / RW / diffusion (env) | 1.139 / 1.112 / 1.113 | ~1 | 0.99943 / 0.99958 / 0.99949 (0.0002 / 0.0009 / 0.0011) | 0.02 |
| A Lambertian, spectral | 1.141 | ~1 | 0.99748 (0.0017) | 0.03 |
| B area | 1.984 | ~1 | 1.00364 (0.0078, 256 spp) | 0.02 |
| C glass, env | 1.046 | 1.000 | 1.00000 | 0.02 |
| D omni control | 1.000 | 1.000 | 1.00009 (0.0002) | 0.01 |
| E1 PT+SMS omni, index-1.0 room / PT open air | 1.500 | 0.500 | 0.99972 (0.0004) | 0.02 |
| E3 PT+SMS spot, same | 1.924 | 0.924 | 1.00005 (0.0002) | 0.02 |
| G diffusion / random walk / fog, PT+SMS / PT, index-1.0 wall, omni | 1.001 / 0.998 / 0.999 | black | 1.00037 / 0.99910 / 1.00074 (0.0022 / 0.0090 / 0.0017) | 0.02 / 0.03 / 0.02 |
| E2 PT+SMS omni, glass TRUE / FALSE | 2.456 | 1.000 | 0.99966 (0.0005) | 0.02 |
| F PT+SMS / PT, directional through glass | 1.000 | 1.001 | 1.00158 (0.0022) | 0.02 |

E2 is equal by construction once the walk is off where SMS runs; it keeps
the pre-fix red.  E1 and E3 check what SMS returns against plain PT in
open air (not VCM: the round-2 review measured VCM at 0.886x PT in open
air in this orthographic setup, so it is no reference here); they found DL-413 (section 6).  G's fog
box holds no surface, so SMS has no anchor anywhere and every receiver is
a volume vertex.

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
(max 2.77 %).  The scenes' comments are corrected.  Final user ruling
(2026-10-02, ledger commit `b5ddd8562`): accept the darker look, no
re-tune.  Round 3's change does not touch these two scenes (no SMS).

## 5. Residuals

- The delta-light walk through a REFRACTING dielectric is the documented
  approximation of section 2 (`RayCaster.h`'s `CastShadowRayTransmittance`
  comment); `VITREOUS_ENAMEL.md` section 10.11 already treats the flag as
  preview-only.  (An earlier revision of this doc compared it against a
  VCM render; VCM is not a valid reference in this setup, see section 4.)
- Under SMS the walk is off at surface points even for chains SMS does
  not cover (beyond `sms_max_chain_depth`, roots its seed misses:
  DL-372); those lose the approximate transport they had.  A walk that
  saw through only non-SMS-casters would have nothing to see through:
  every surface the walk passes is an SMS caster.

## 6. DL-413 and one unfiled finding

**DL-413 (fixed here).**  PT + SMS delivered half an omni light's direct
light through a caster (E1 0.4994; dielectric, ior 1.01 and
`sms_biased FALSE` all ~0.50) and 0.92 of a spot light's.
`LightSampler::SampleLight` stores a delta light's random photon
direction in `LightSample::normal`; SMS's four contribution sites
(`ComputeTrialContribution{,NM}`, `EvaluateAtShadingPoint{,NM}`; every
seeding mode, photon-seeded included, prices through them) passed it to
`ComputeLightToFirstVertexJacobianDet` as the light-endpoint normal, so
the y-tangent plane sat at a random angle and every sample carried `|cos|`
of it -- 1/2 averaged over a sphere.  The estimator is
`f T L_e cos_x G_x_v1 |det dv1/dy| / p(y)` = `L_e dw_x/dA_y` over the
light's area measure.  A point light of intensity `I` is the limit of a
vanishing emitter of radiance `I/A` whose area element faces the
direction it emits along, so `dA_y` is perpendicular to the last segment,
`ComputeLastBlockLightJacobian`'s `(I - wo wo)` projection is the
identity on it and no cosine at `y` enters (as in a point light's plain
NEE), with `p(y) = 1` and `L_e = I(w)`.  `JacobianLightNormal` returns
`normalize(y - v_k)` for a delta light and the surface normal otherwise.
E1 0.500 -> 0.99972, E3 0.924 -> 1.00005.  Shipped `pool_caustics_vcm`
(its commented-out PT+SMS rasterizer, two spot lights, photon-seeded;
200 x 150, 64 spp, salted n = 4, OIDN off): whole frame +0.02 % against a
per-render sd of 2-2.7 % -- not resolvable at this cost; the change is
confined to SMS's caustic share (~+8 % of it for a spot cone).

**Unfiled (needs a ledger id): SMS prices its chain with NO participating
medium.**  `ManifoldSolver` has no transmittance anywhere in its
contribution, so a receiver inside fog lit through a caster is too
bright under SMS.  Measured: a Lambertian sphere inside an index-1.0
refractor box filled with isotropic fog (`scattering 0.5`), omni light
outside, PT + SMS / PT (flag TRUE, so PT's walk carries the fog
transmittance) = 1.207 (salted n = 4, per-pair sd 0.011).  Pre-existing,
independent of DL-344; row G's fog box therefore holds no surface.

**Gate note (round 3).**  `ExteriorIndexInvarianceTest`'s
`B: sms_lambertian_via_glass_sphere/PT/sms-uniform/k1` and `/k2` rows fail
at the suite's fixed seeds on the merged tree (229/2, a re-run 230/1),
with an UNPAIRED firefly: one air render near twice the others while its
enclosed twin is clean.  The same tree with `ManifoldSolver.cpp` reverted
to pre-DL-413 fails the same two rows (k1 0.934, k2 0.738), and both
builds show a heavy tail at n = 32 (per-render sd 0.40 before, 0.42-0.49
after DL-413).  The rows use an omni light and no `transparent_shadows`,
so neither of this slice's changes reaches them except DL-413's
rescaling; the flake is SMS uniform mode's point-light caustic tail and
needs its own row.
