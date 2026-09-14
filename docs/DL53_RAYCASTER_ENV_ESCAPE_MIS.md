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
  `CastRayNM` with `ri.pRadianceMap` for their continuation ray.
  **Correction (P3-4, round-2 review, this slice)**: the original claim
  here — "none of them set `RAY_STATE::bsdfPdf` (default 0), so the new
  weighting is a no-op for them" — is WRONG on the facts.  These shader
  ops do not construct a fresh `RAY_STATE`; they forward the CALLER's
  `rs` argument verbatim (e.g. `TransparencyShaderOp.cpp:52`,
  `AlphaTestShaderOp.cpp:71`, `DirectVolumeRenderingShader.cpp:349` all
  pass `rs`, not a zero-initialized `RAY_STATE`), so `bsdfPdf` is
  whatever the CALLER carried, not necessarily 0.  The correct argument
  is narrower: the only production call chain that reaches one of these
  shader ops with a positive `bsdfPdf` already set is a PT SSS
  continuation's `rs2` being passed onward as the `rs` of a nested
  shader-op dispatch (e.g. a BSSRDF exit ray that next hits a
  transparent or alpha-tested surface) — and in that case the inherited
  `bsdfPdf` is exactly the value the SSS continuation itself intended,
  so `RayCasterEnvEscapeMISWeight`'s weight is CORRECT, not a bug: this
  is a legitimate pass-through of an already-meaningful value, not an
  accidental default. No sibling defect follows from this correction;
  it only fixes the doc's factual claim about how these ops behave.
- **Legacy `pixelpel_rasterizer` / `pixelintegratingspectral_rasterizer`
  top-level camera rays**: call `CastRay`/`CastRayNM`/`CastRayHWSS` with
  `pRadianceMap = nullptr` explicitly, so they already reached the
  (always-correct) global-map branch and are unaffected by this change.

No sibling site required an independent fix.

## What this gate does not cover

Two pre-existing residuals, both out of this fix's scope (neither is a
regression this fix introduced; both were true before and after
`b3de184d`):

- **Light-solo path.** `LightSampler.cpp`'s env-NEE (~:2567, "LIGHT-SOLO
  Stage 3") only runs when solo is inactive or the environment IS the
  soloed target — every other solo target (an explicit light or a mesh
  luminary) sees zero env-NEE contribution. `RayCaster::CastRay`'s
  escape path has no equivalent solo-suppression: a BSDF-sampled ray
  that escapes to the global env map while some OTHER light is soloed
  still returns its (now correctly MIS-weighted, post-this-fix)
  partner-weighted radiance. Solo isolation for a non-environment
  target is therefore not perfectly complete — a small `w_bsdf`
  fraction of the env's contribution leaks through the escape path
  even when the environment itself is not the soloed light. This
  predates this fix (the escape previously returned FULL, unweighted
  radiance in the same situation, an even larger leak) and is a
  solo-mode preview/debug concern, not a production render bug.
- **Override-map asymmetry.** `LightSampler`'s env-NEE always samples
  and MIS-weights against the SCENE's global environment map,
  regardless of whether the current shading point's escape path would
  actually resolve through a different, per-object override map. If a
  shading point has such an override, its BSDF-sampled escape (per the
  MIS PARTNER RULE above) correctly keeps FULL weight — there is no
  partner for a distinct override map — but the SAME point's env-NEE
  still discounts the global env's contribution by `w_nee < 1`,
  assuming a BSDF-escape partner that, at this specific point, does
  not exist (it goes to the override map instead). The global env's
  `(1 - w_nee)` share is under-counted at such points, not
  double-counted. This is a pre-existing asymmetry between the two
  independent NEE/escape code paths, not something this fix's
  pointer-identity gate could have addressed (the gate correctly
  identifies "does THIS escape's map have a partner", which is exactly
  right for the escape side; the gap is that env-NEE does not
  symmetrically ask "does THIS shading point's escape actually go to
  the map I'm NEE-ing against").

Neither residual has a dedicated regression or ledger row as of this
writing; both are recorded here for a future reader who traces either
symptom back to this fix.

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
