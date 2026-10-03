# DL-298: hand-built emitter records omitted the physical world position

Status: **FIXED 2026-10-03** (`debt-dl298`, pending review/strike) -- one shared
`LightSampler::FillEmitterRecord`, fix commit `f8f6e249c`.

## Mechanism

Every site that evaluates an emitter at a SAMPLED point builds its
`RayIntersectionGeometric` by hand, because no ray was traced to that point.
Each built its own subset of the fields the sampled point already determines,
and the subsets drifted:

| site | `ptIntersection` (`P`) before |
|---|---|
| `LightSampler::SampleLight` -- the RGB `LightSample::Le` evaluation | **default (0,0,0)** |
| `LightSampler::EvaluateDirectLighting` RGB mesh arm (PT, legacy shader ops) | **default (0,0,0)** |
| `LightSampler::EvaluateDirectLightingNM` mesh arm | **default (0,0,0)** |
| `PhotonTracer.h` (RGB legacy photon direction `getEmmittedPhotonDir`) | **default (0,0,0)**, and `Po` default too |
| `SpectralPhotonTracer.h` (spectral photon direction) | **default (0,0,0)**, `Po` default |
| `SMSPhotonMap.cpp` (SMS seed-photon direction) | **default (0,0,0)**, `Po` default |
| BDPT `GenerateLightSubpathImpl` NM hero `rig`, HWSS companion `rigW` | supplied |
| VCM `EvaluateNEEImpl` rig (Pel and NM) | supplied (but no `onb`) |
| BDPT LIGHT root vertex -> `PathVertexEval::PopulateRIGFromVertex` consumers | supplied |
| camera-ray / BSDF-sampled emitter hit (real intersection) | supplied |

So the input contract differed per path, and an emission or Phong-exponent
painter keyed on world position (`expression_painter` / `scalar_painter`
`expression` reading `P`, or any 3-D procedural painter) read the ORIGIN at a
nonzero sampled point under exactly the unsupplied paths.  Two of the paths
that do supply it still disagreed with the others on `onb` (VCM never set it).

## Repair

`LightSampler::FillEmitterRecord(rig, position, normal, coord, ptObjIntersec)`
is the one place that knows what a sampled point determines: `bHit`, world
`ptIntersection`, `vNormal` / `vGeomNormal` (mirrored: no modifier runs on an
emitter record), the shading `onb` built from that normal, `ptCoord`, and the
ungated, ray-free object-space point `ptObjIntersec` (callers pass
`EmitterObjectPoint`; `LightSample::ptObjIntersec` for the five
`LightSample` consumers).  The GATED shading payload
(`EmitterSurfacePayload`: derivatives, signals, footprint) stays separate
(`ProbeEmitterSurface` / `ApplyEmitterSurface`) -- a probe costs a ray and is
process-gated, the fill is free.

Every site above now calls it: `SampleLight`, both NEE arms, BDPT's NM hero and
HWSS companion rebuilds, VCM's NEE rebuild (it also gets its missing `onb`,
re-derived for the face toward the eye when a double-sided emitter flips),
`PhotonTracer.h`, `SpectralPhotonTracer.h`, `SMSPhotonMap.cpp`.  The photon
tracers' alpha-only copies (DL-214) keep working unchanged: they copy the
now-complete record and `AcceptEmitterAlpha` re-stamps the same position.

Emission geometry and every pdf stay as they were (`FillEmitterRecord` writes
nothing a pdf reads); what moves is only what a world-position painter returns.

## Red-proof (`tests/EmitterWorldPositionTest.cpp`)

The wall-and-camera DL-44 rig (a 2x2 diffuse wall, a 2x2 emitter BEHIND the
camera facing it; single-bounce direct light).  Emission is keyed on world `P`
in one scene and on the emitter's own UV in a control (`(2u-1)^2+(2v-1)^2`, the
same field over a clipped plane; UV is read correctly by every path since
DL-44):

* Lambertian: exitance `M = 0.25 + 1.125 (P.x^2 + P.y^2)` (area mean 1.0; the
  origin value is 0.25, deliberately nonzero -- see DL-431);
* Phong: exponent `N = 2 + 3 (P.x^2 + P.y^2)`, radiance `(N+1) cos^N/(2 pi)`.

Each is rendered in PT RGB, BDPT RGB, VCM RGB, PT spectral, PT spectral HWSS and
BDPT spectral, 4 salted renders each (`SobolSamplerTestHooks::ValueSalt`),
against (a) the UV control in the same build and (b) an independent
deterministic quadrature of the closed form (RGB rows).  Mean P/UV ratios:

| row | master `c35591fb9` | fixed |
|---|---|---|
| Lambertian PT RGB | **0.2534** | 1.0000 |
| Lambertian PT spectral / HWSS | **0.2536 / 0.2534** | 0.9986 / 1.0003 |
| Lambertian VCM RGB | **0.9896** (sd 0.00003) | 1.0001 |
| Lambertian BDPT RGB / spectral | 1.0000 / 0.9993 (immune) | 1.0000 / 0.9990 |
| Phong PT RGB | **0.6325** | 1.0000 |
| Phong PT spectral / HWSS | **0.6313 / 0.6327** | 0.9985 / 0.9998 |
| Phong VCM RGB | **0.9950** | 1.0000 |
| Phong BDPT RGB / spectral | 1.0000 / 1.0002 (immune) | 1.0000 / 1.0012 |

Closed form (RGB, quadrature): Lambertian 0.083442, Phong 0.193755; fixed PT /
BDPT / VCM read P/closed 1.0000 / 1.0000 / 1.0002 and 1.0000 / 0.9999 / 1.0001.
Master: **34 passed / 8 failed** (42 checks); fixed 42/0.  The PT 0.25 is
exactly the origin-over-mean ratio (`M(0)/<M> = 0.25`): NEE read the field at
the origin.  VCM's ~1 % is the light-subpath throughput, which takes
`SampleLight().Le` (the RGB record) while its NEE rebuild was already correct.
BDPT is immune in this rig (attribution by inspection, not instrumented): its
eye-side connections rebuild the emitter record from a `BDPTVertex` that
carries `position`, so the `SampleLight().Le` the light subpath starts from
carries little of the image.

The photon-direction sites are pinned at the record level by
`tests/AlphaPhotonEmissionTest.cpp` (it already had a real Phong-exponent
recorder on the actual legacy RGB/NM emission loops): the exponent painter
`1 + P.x^2` read the baseline origin (n = 1, direction cosine 0.547723) on
master and reads the sampled point after the fix (n = 83 .. 110, cosine
0.9858 .. 0.9892); 12 of 242 checks failed on master, 242/0 after.
`AlphaEmitterRecordTest` likewise now pins that the original `SampleLight` and
NEE records carry the physical emitter `P` (`Le` 25.4648 vs 12.7324 at the
origin record, ratio 2; NEE `P.y` 3.99999).  The SMS seed-photon site calls the
same helper but has no direct recorder (same call, same arguments as the two
tracers; recorded as unmeasured).

## Not fixed here: DL-431

`LambertianEmitter` / `PhongEmitter` / `CompositeEmitter::RefreshAverages`
estimate `averageRadiantExitance()` (and the NM spectrum) at construction from a
10x10 UV grid over a DEFAULT-constructed record -- `P = Po = (0,0,0)` -- because
an emitter does not know its geometry.  That average is the light-selection
importance weight and the photon power.  A field that vanishes at the origin
gets zero importance and is never sampled (the first revision of the red-proof
used `1.5 (P.x^2+P.y^2)`: PT read 0.00009 against 0.0831 and BDPT 0.000006,
VCM 0.0837 because its light pass has a fallback); a field that does not
vanish gets the wrong importance/power, which for photon maps is a real bias.
Closing it needs a per-luminary average over the object's own surface samples
(`LuminaryManager` knows the object), which is a separate design -- filed as
DL-431.

## Shipped scenes

No shipped scene keys an emitter's exitance or Phong exponent on `P`/`Po`:
a census of all `lambertian_luminaire_material` / `phong_luminaire_material`
chunks in `scenes/**` (resolving the painter graph) finds only
`uniformcolor_painter` (244 + 1), `spectral_painter` (16), `blackbody_painter`
(3), a scalar `value` Phong exponent (1) and one `expression_painter`
(`scenes/Tests/Signals/emitter_louvres.RISEscene`, keyed on
`proximity(0.001)`, a gated signal read through the payload this change does not
touch).  No shipped render is expected to move.
