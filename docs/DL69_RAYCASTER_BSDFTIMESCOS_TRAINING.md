# DL-69: BSSRDF/volume continuations never trained RayCasterEnvEscapeMISWeight's optimal-MIS arm

Status: **CLOSED 2026-09-13** — source repair `0c9eccc4`.

## Mechanism

`RayCaster::CastRay`/`CastRayNM`/`CastRayHWSS` resolve an explicit
`pRadianceMap` escape (the SSS/volume-continuation call shape) through
`RayCasterEnvEscapeMISWeight` (DL-53, [DL53_RAYCASTER_ENV_ESCAPE_MIS.md](DL53_RAYCASTER_ENV_ESCAPE_MIS.md))
with `/*trainOptimalMIS=*/true` at every one of its own call sites
(`RayCaster.cpp` ~:1421, ~:1437, ~:1983, ~:2004). That helper's
training arm only accumulates a sample when `rs.bsdfTimesCos` is
nonzero (`fLum = MaxValue(envRadiance * rs.bsdfTimesCos); ... if( f2 >
0 && rs.bsdfPdf > 0 )`), but two continuation sites left
`RAY_STATE::bsdfTimesCos` at its zero default, so training could never
fire for them even though the call site asked for it:

- `PathTracingIntegrator.cpp`'s two BSSRDF continuations (the
  diffusion-profile site and its `RandomWalkSSS` twin) only assigned
  `rs2.bsdfTimesCos` inside `if constexpr ( Traits::is_nm )` —
  exactly backwards. `rc.pOptimalMIS` is constructed only for the
  Pel/RGB tag at runtime (spectral renders never build one, per the
  existing comment at the site), so the NM assignment was a genuine
  structural no-op, while the ONE tag that could actually use a
  trained accumulator (Pel) never got a value.
- `RayCaster.cpp`'s own internal volume phase-scatter continuation
  (both the RGB `CastRay` copy at ~:1252 and the NM `CastRayNM` copy
  at ~:1859) never assigned `rs2.bsdfTimesCos` at all.

This does not change any WEIGHT that was applied — `PowerHeuristic` is
the fallback whenever `rc.pOptimalMIS` is absent or not yet ready, and
that fallback is unconditionally correct — it only means the optimal-MIS
accumulator's alpha estimate, once trained from other (non-SSS,
non-volume) samples, never incorporated these paths' contribution to
its own training set. The effect is a training-input gap, not a
render-time correctness bug in the weight itself.

## Repair

`PathTracingIntegrator.cpp`: replace the `if constexpr (Traits::is_nm)`
gate with an unconditional call to the same `PTBsdfTimesCos`/
`PTRayStateBsdfTimesCos` helper pair the main (non-SSS) scatter
continuation already uses a few hundred lines below
(`PTBsdfTimesCos(scatterThroughput, effectiveBsdfPdf)`). Those helpers
are already overloaded per `Value` type (`RISEPel` for Pel, `Scalar`
for NM, taking `fabs()` for NM) so the same call compiles and behaves
correctly for both tags without an `if constexpr`: a structural no-op
for NM (`rc.pOptimalMIS` is still null there at runtime) and the actual
fix for Pel. The `AccumulateCount` call is likewise unconditional now
(it was already internally gated on `rc.pOptimalMIS` being non-null).

`RayCaster.cpp`: the two local `PTBsdfTimesCos`/`PTRayStateBsdfTimesCos`
helpers live in `PathTracingIntegrator.cpp`'s anonymous namespace and
are not visible here, so the two volume-continuation sites get the
equivalent inline: `rs2.bsdfTimesCos = throughput * phasePdf;` for the
RGB `CastRay` copy (`throughput` is already `RISEPel` there), and
`rs2.bsdfTimesCos = RISEPel( std::fabs( throughput ) * phasePdf );` for
the NM `CastRayNM` copy (`throughput` is `Scalar` there), mirroring the
Pel/NM convention exactly.

No behavior change for HWSS: its escape branch never trained the
accumulator at this site before this fix (per DL-53's doc) and still
does not — `trainOptimalMIS` is not asked for at the HWSS call site.

## Sibling audit

- **Main (non-SSS, non-volume) surface scatter continuation**
  (`PathTracingIntegrator.cpp` ~:3388-3396): already correct — this is
  the site the fix mirrors, unaffected by this change.
- **BDPT/VCM/MLT**: none of these integrators call through
  `RayCaster::CastRay`'s `pRadianceMap`-explicit escape branch for
  their own continuations (they resolve environment radiance through
  their own MIS machinery), so they are not siblings of this pattern.
- **HWSS**: confirmed out of scope above; matches pre-existing,
  documented behavior.
- **`RecordGuidingTrainingSampleNM`** (BDPT's guiding-training helper,
  out of this slice's scope): calls `CastRayNM` with a per-object
  `pRadianceMap` (typically null) and a positive `samplePdf`; the
  pointer-identity gate in `RayCasterEnvEscapeMISWeight` means it does
  not reach the global-map branch in the common case, and its
  `bsdfTimesCos` wiring (if any) is unrelated to the two sites fixed
  here.

## Residual: volume-guiding bsdfPdf composition (DL-70, open)

Auditing the two fixed volume sites surfaced a separate, pre-existing
question this fix does not touch: when OpenPGL volume guiding fires,
`wi` is drawn from `combinedPdf` (a phase/guide mixture), and
`guidingMISWeight = phasePdf / combinedPdf` is folded into
`rs2.importance` — correct for the immediate contribution. But
`rs2.bsdfPdf` is then stored as the raw, un-combined `phasePdf`, not
`effectivePdf`/`combinedPdf`. This is INCONSISTENT with the main
surface scatter continuation (`PathTracingIntegrator.cpp` ~:3411/~:5507),
which stores the COMBINED `effectiveBsdfPdf` in the equivalent
situation. Whenever a guided volume continuation escapes to the
environment map, `RayCasterEnvEscapeMISWeight`'s `w_bsdf =
PowerHeuristic(rs.bsdfPdf, envPdf)` therefore uses an understated pdf
relative to the true sampling density. This is a ruling, not a
red-proved defect (no render-level measurement was taken, and the
composite condition — trained+active volume guiding AND the guided
ray escaping to the global env map — is narrow); filed as **DL-70**,
left OPEN.

## File status

| File | Status |
|---|---|
| `src/Library/Shaders/PathTracingIntegrator.cpp` | Modified: unconditional `bsdfTimesCos` assignment at both BSSRDF continuation sites (`0c9eccc4`). |
| `src/Library/Rendering/RayCaster.cpp` | Modified: `bsdfTimesCos` assignment at both volume phase-scatter continuation sites (`0c9eccc4`). |
| `docs/DEBT_LEDGER.md` | Modified: new DL-69 row, closed. |
| `docs/DL69_RAYCASTER_BSDFTIMESCOS_TRAINING.md` | Added: this file. |

Gate: `MISWeightsTest`, `OptimalMISAccumulatorTest`,
`RasterizerDefaultsConsistencyTest`, `RayCasterEnvEscapeMISTest` (79/79),
`SSSRadianceScalingTest` (574017 checks, 0 failures — unchanged),
`RandomWalkSurvivalTest` all pass; `make -C build/make/rise -j8 all`
clean (zero warnings).
