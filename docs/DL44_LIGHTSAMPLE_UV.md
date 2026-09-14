# DL-44: sampled emitter UV omitted from `LightSample`

Status: **CLOSED 2026-09-14** — fix `18892254`, red-proof test `eb405609`.

## Mechanism

`LightSampler::SampleLight`'s mesh-luminary branch draws a UV `coord` from
`IObject::UniformRandomPoint( &position, &normal, &coord, prand )` and uses
it immediately, locally, to build a `RayIntersectionGeometric` and evaluate
the sample's own RGB emission (`sample.Le = pEmitter->emittedRadiance(rig,
...)`, with `rig.ptCoord = coord`). That local `rig` and its `ptCoord` were
never carried anywhere — `LightSample` had no UV field.

Five other sites rebuild their *own* `RayIntersectionGeometric` (or, for the
BDPT `LIGHT`-type root vertex, a `BDPTVertex`) from a `LightSample` rather
than reading `sample.Le` directly, so a UV-keyed emission painter
(`checker_painter`, an image-based exitance map, `expression_painter`'s
`u`/`v`) evaluated at whatever `RayIntersectionGeometric` happened to
default-construct its `ptCoord` to — `(0,0)` — regardless of where
`UniformRandomPoint` actually sampled:

- `BDPTIntegrator.cpp::GenerateLightSubpathImpl`'s NM hero `rig` (the NM
  twin of `SampleLight`'s own RGB emission evaluation).
- The same function's HWSS companion-wavelength `rigW`.
- The same function's `BDPTVertex::LIGHT` root vertex (`v`), which is not
  itself a `RayIntersectionGeometric` but is later rebuilt into one by
  `PathVertexEval::PopulateRIGFromVertex` at every consumer of that
  vertex — `LuminaryRadiance<Tag>` (BDPT's own s=0/t=1 splat and every
  s=1 connection strategy that reaches the light via `ConnectAndEvaluate`)
  and VCM's own light-to-camera splat (`SplatLightSubpathToCameraImpl`,
  which skips `i==0`/`LIGHT`-typed vertices for splatting but reuses the
  same generated subpath's root for `EvaluateS0Impl`'s s=0-side accounting
  and for any interior connection reaching back to it).
- `VCMIntegrator.cpp::EvaluateNEEImpl`'s `rig` (VCM's own bespoke
  light-sampling NEE strategy — VCM does not reuse
  `LightSampler::EvaluateDirectLighting{,NM}`; it needs its own MIS
  bookkeeping, so it re-implements the sample-a-light-then-evaluate step
  and rebuilds its own record from the `LightSample`).

MLT shares BDPT's light-subpath generation (`GenerateLightSubpath{,NM}`),
so it inherited the same defect with no separate code to fix.

**What was NOT affected.** `LightSampler::EvaluateDirectLighting{,NM}` — the
NEE strategy PT uses directly and BDPT's `DirectLightingShaderOp` legacy
path uses — samples its own local `lumCoord` via `UniformRandomPoint` and
sets `lumri.ptCoord = lumCoord` right there, never touching `LightSample`
at all; it was never part of this bug. `VCMIntegrator::EvaluateS0Impl` (a
BDPT eye-path vertex directly hitting an emitter) and BDPT's own s=0
emission strategy rebuild from a `BDPTVertex` that was populated from a
*real* ray intersection (`v.ptCoord = ri.geometric.ptCoord` in the eye
subpath generator), not from a `LightSample`, so their UV was always
correct. `ManifoldSolver.cpp` (Specular Manifold Sampling's light-directed
seeding) reads `LightSample::Le` directly — the RGB value `SampleLight`
already evaluated correctly — and derives its own spectral approximation
via `RGBIlluminantSpectrum::FromRGB(Le)`, never re-evaluating
`emittedRadianceNM` against a rebuilt record, so it was unaffected too
(a pre-existing, separately documented approximation gap of its own, not
this bug).

## Repair

Added `LightSample::ptCoord` (`Point2`, `LightSampler.h`), given the same
treatment as the pre-existing `ptObjIntersec` field: ungated and
ray-free — it costs nothing beyond the coordinate `UniformRandomPoint`
already computed, so it must not depend on the process-wide emitter-signal
probe gate (`EmitterSurfacePayload`/`ApplyEmitterSurface`'s gate is a
separate, independent concern from a plain UV coordinate).

`LightSampler::SampleLight` now stores `sample.ptCoord = coord` alongside
its existing `sample.ptObjIntersec = EmitterObjectPoint(...)` line. Every
site enumerated above now carries `ls.ptCoord` onto its own rebuilt record:

- `GenerateLightSubpathImpl`'s NM hero `rig` and HWSS companion `rigW` each
  gained one line, `rig.ptCoord = ls.ptCoord;` / `rigW.ptCoord =
  ls.ptCoord;`.
- The `BDPTVertex::LIGHT` root vertex gained `v.ptCoord = ls.ptCoord;`
  beside its existing `v.ptObjIntersec = ls.ptObjIntersec;` line — this one
  change also fixes `LuminaryRadiance`'s and VCM's light-to-camera splat's
  `PopulateRIGFromVertex` rebuilds, since they consume the vertex rather
  than `LightSample` directly. `v.ptCoord1` is left at its default `(0,0)`:
  `IObject::UniformRandomPoint` returns exactly one `Point2`, so there is no
  second UV channel to carry, matching a real surface vertex on an object
  with no `TEXCOORD_1`.
- `VCMIntegrator.cpp::EvaluateNEEImpl`'s `rig` gained `rig.ptCoord =
  ls.ptCoord;`. This function is templated over `Tag`, so the one line
  fixes both the Pel (`EvaluateNEE`) and NM (`EvaluateNEENM`)
  instantiations.

No change to `EmitterSurfacePayload`, `ApplyEmitterSurface`, the
normal-aligned emitter probe, or any signal/derivative/footprint field —
those were already threaded correctly by the prior
`SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md` §5 slice (S3) that first found
this gap and opened this row (see `DL36_EMITTER_NEIGHBOUR_PIN.md`'s
"Shared producer and downstream audit" table). `ptCoord` was the one field
that slice's own audit named as a separate, not-yet-fixed defect.

## ABI note

`LightSample` (`RISE::Implementation::LightSampler::LightSample`) is an
internal renderer type: it is not part of `RISE_API.h`'s public
construction surface, is not serialized, and has no Blender-bridge or
agent-tool mirror. Adding a field to it is source-only and carries no ABI
or scene-file compatibility concern.

## Red-proof

`tests/EmitterUVSampleTest.cpp` renders a diffuse wall lit *only* via each
integrator's light-sampling strategy (small `max_eye_depth`/
`max_light_depth`, a single mesh luminary with `material none` so it cannot
itself reflect anything) by a small checker-textured luminary
(`checker_painter(colora=white, colorb=black, size=0.5)`) positioned
geometrically behind the camera (so it can never appear in frame or occlude
the wall, regardless of FOV) but oriented to shine through the camera
position onto the wall. `ClippedPlaneGeometry::UniformRandomPoint` sets
`coord = (u,v) = (prand.x, prand.y)` directly, so the geometry's native UV
domain is exactly `[0,1]x[0,1]`; a checker of `size 0.5` over that domain is
an exact 2x2 board whose true area-weighted average exitance is `0.5`
(half white, half black), while `UV(0,0)` — every broken rebuild's
substituted value — lands in the white cell (`ceil(0/0.5)=0` on both axes,
an even sum), exitance `1.0`. The defect's signature is therefore a clean,
closed-form `2.0x` factor, not a percentage of MC noise.

Committed against unfixed master (`943d353b`) in `eb405609`, then run:

```text
PT                       mean-luminance = 0.042145
BDPT                     mean-luminance = 0.084241
VCM                      mean-luminance = 0.081820
ratio BDPT/PT = 1.9989, ratio VCM/PT = 1.9414
FAIL: BDPT/PT ratio within MC-noise band of 1.0 (DL-44 fixed)
FAIL: VCM/PT ratio within MC-noise band of 1.0 (DL-44 fixed)

PT (spectral)            mean-luminance = 0.041631
BDPT (spectral)          mean-luminance = 0.083552
ratio BDPT/PT (spectral) = 2.0070
FAIL: BDPT/PT (spectral) ratio within MC-noise band of 1.0 (DL-44 fixed)

Passed: 7  Failed: 3
```

matching the predicted `2.0x` bias in both the RGB and the spectral (NM,
hwss=false, hero-only) rasterizer families, well outside Monte-Carlo noise.

After the fix (`18892254`):

```text
PT                       mean-luminance = 0.042143
BDPT                     mean-luminance = 0.042144
VCM                      mean-luminance = 0.042138
ratio BDPT/PT = 1.0000, ratio VCM/PT = 0.9999

PT (spectral)            mean-luminance = 0.041937
BDPT (spectral)          mean-luminance = 0.041946
ratio BDPT/PT (spectral) = 1.0002

Passed: 10  Failed: 0
```

## Gate

`EmitterUVSampleTest` (new, 10/0), plus the full touched-class gate:
`BDPTStrategyBalanceTest`, `VCMStrategyBalanceTest`, `EnvLightBalanceTest`,
`BDPTVertexRIGRebuildTest`, `SignalEmitterRecordTest`,
`BDPTPhantomStrategyWeightTest`, `VCMEyePostPassTest`,
`VCMLightPostPassTest`, `MISWeightsTest`, `PTGuidingMISPartitionTest`,
`AreaLightShaderOpScalarNTest`, `CstDeriveGoldenTest`, `SourceHygieneTest`
— see the slice report for the full counter table. Clean isolated rebuild
of the full library, zero warnings.
