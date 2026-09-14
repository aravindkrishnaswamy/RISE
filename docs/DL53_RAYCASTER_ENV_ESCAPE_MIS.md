# DL-53: recursive explicit-global-map MIS bypass

Status: **CLOSED 2026-09-13** — source repair `b3de184d`.

## Mechanism

`RayCaster::CastRay`/`CastRayNM`/`CastRayHWSS` each resolve a ray-miss
escape with:

```
if( pRadianceMap )                    { c = raw radiance; /* no MIS weight */ }
else if( pScene->GetGlobalRadianceMap() ) { c = raw radiance * MIS weight vs env-NEE; }
```

Every production `pathtracing_*` rasterizer's BSSRDF / random-walk SSS
continuation (`PathTracingIntegrator.cpp`'s `PTCastRay` helper) and the
internal volume phase-scatter continuation (inside `RayCaster.cpp` itself)
forward the SAME global radiance map through the explicit `pRadianceMap`
parameter, carrying a real positive cosine `bsdfPdf`. The first arm above
therefore always won for them, and the MIS-weighted second arm was
unreachable: the BSDF-sampled environment escape summed to `1 + w_nee` on
top of an env-NEE sample the entry point had already added at its own MIS
weight — a double count, on any complete SSS event or volume-scatter
continuation that happens to see the global map. This is the `RayCaster`
analogue of the PT-integrator bug already fixed in
[PT_ENV_MIS_DOUBLECOUNT.md](PT_ENV_MIS_DOUBLECOUNT.md) (F1/F2); that arc
explicitly did not touch `RayCaster.cpp`, which is a separate, still-shared
implementation used by SSS continuations, the internal volume walk, and the
legacy shader-dispatch rasterizers.

## Repair

The MIS-partner weight computation is factored into a shared helper,
`RayCasterEnvEscapeMISWeight` (anonymous namespace in `RayCaster.cpp`, next
to the existing `RayCasterSurvivalWeight`), and applied in the explicit
`pRadianceMap` branch too — gated on `pRadianceMap` being pointer-identical
to `pScene->GetGlobalRadianceMap()` (the MIS PARTNER RULE: only the map
env-NEE actually imports through has a partner to correct against; a
genuinely distinct per-object override map has none and keeps full weight,
unchanged from before). `bsdfPdf == 0` (delta) is unaffected. HWSS's branch
never trained the optimal-MIS accumulator at this site and still doesn't
(matching its pre-existing behaviour); RGB/NM continue to train it as
before.

No call site needed editing: `PathTracingIntegrator.cpp`'s SSS continuation
already passed the global map and a positive `bsdfPdf` (that is precisely
the shape that exposed the bug), and the fix lives entirely in the shared
`RayCaster.cpp` layer both map-selection branches go through.

## Red-proof

`tests/RayCasterEnvEscapeMISTest.cpp` (committed `acbb1073` against
unfixed `RayCaster.cpp`) drives real `RayCaster::CastRay`/`CastRayNM`/
`CastRayHWSS` instances (via a zero-object scene + one cheap render to
trigger `AttachScene`, reached through the same
`PixelBasedRasterizerHelper::GetRayCaster()` accessor
`Job::SetActiveRasterizerRadianceScale` uses in production) with a
manufactured miss ray and positive `RAY_STATE::bsdfPdf`, comparing
`pRadianceMap == nullptr` against `pRadianceMap ==` the scene's global map
passed explicitly, against an independently recomputed power-heuristic
weight (not calling `PathTransportUtilities::PowerHeuristic` or the new
helper).

```
FAIL: RGB: explicit-global-map matches analytic MIS weight @ bsdfPdf=0.1  (a=0.7 b=0.474006 ...)
FAIL: RGB: explicit-global-map matches analytic MIS weight @ bsdfPdf=1    (a=0.7 b=0.696678 ...)
FAIL: NM: explicit-global-map matches analytic MIS weight @ bsdfPdf=0.1   (a=0.550371 b=0.372685 ...)
FAIL: HWSS: explicit-global-map matches analytic MIS weight @ bsdfPdf=0.1 lambda[0]  (a=0.445799 b=0.301873 ...)
Passed: 31   Failed: 48
```

Fixed: `Passed: 79   Failed: 0`. The `bsdfPdf==0` (delta) and
distinct-per-object-override-map invariants already passed on the unfixed
library (31/31 of the non-comparison checks), isolating the fix to the
pointer-identity gate.

## Sibling audit

- **Internal volume phase-scatter continuation** (`RayCaster.cpp`'s own
  recursive `CastRay`/`CastRayNM` call with `rs2.bsdfPdf = phasePdf`,
  forwarding the same `pRadianceMap` it received): automatically covered,
  since it is the same function being fixed.
- **BDPT's guiding-training helper**
  (`RecordGuidingTrainingSampleNM` in `BDPTIntegrator.cpp`, a guiding
  branch out of this slice's scope) calls `CastRayNM` with
  `ri.pRadianceMap` (a per-object map, typically null) and a positive
  `samplePdf`. It passes through the same fixed `RayCaster.cpp` branch
  automatically; no guiding-branch code was touched.
- **Reflection/Refraction/Transparency/AlphaTest/DistributionTracing/
  FinalGather/DirectVolumeRendering shader ops**: all call `CastRay`/
  `CastRayNM` with `ri.pRadianceMap` for their continuation ray, but none
  of them set `RAY_STATE::bsdfPdf` (default 0), so the new weighting is a
  no-op for them — confirmed by grep (no `bsdfPdf =` assignment in any of
  those files).
- **Legacy `pixelpel_rasterizer` / `pixelintegratingspectral_rasterizer`
  top-level camera rays**: call `CastRay`/`CastRayNM`/`CastRayHWSS` with
  `pRadianceMap = nullptr` explicitly, so they already reached the
  (always-correct) global-map branch and are unaffected by this change.

No sibling site required an independent fix.

## File status

| File | Status |
|---|---|
| `src/Library/Rendering/RayCaster.cpp` | Modified: shared `RayCasterEnvEscapeMISWeight` helper; applied in all three explicit-map branches (`b3de184d`). |
| `tests/RayCasterEnvEscapeMISTest.cpp` | Added: DL-53 red-proof (committed `acbb1073` before the fix). |
| `tests/README.md` | Modified: new row. |
| `docs/DEBT_LEDGER.md` | Modified: closure and counts. |
| `docs/DL53_RAYCASTER_ENV_ESCAPE_MIS.md` | Added: this file. |

Gate: `RayCasterEnvEscapeMISTest`, `EnvLightBalanceTest` (116/116),
`SSSRadianceScalingTest`, `RandomWalkSurvivalTest` all pass; `make -C
build/make/rise -j8 all` clean (zero warnings). `src/Library/Shaders/
PathTracingIntegrator.cpp` and `src/Library/Shaders/BDPTIntegrator.cpp`
are byte-identical to HEAD — the fix required no call-site edits in
either file.
