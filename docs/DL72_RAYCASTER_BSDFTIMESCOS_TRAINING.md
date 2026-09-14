# DL-72: BSSRDF/volume continuations never trained RayCasterEnvEscapeMISWeight's optimal-MIS arm

Status: **CLOSED 2026-09-13** — source repair `0c9eccc4`; **REOPENED and
re-resolved by REVERTING the training wiring, round-2 (P2-B) same day** —
the round-1 fix's wiring itself had a count-pairing defect (RayCaster.cpp
sites) and fed the accumulator a quantity that does not match its
documented contract (all four sites). See "Round-2 correction (P2-B)"
below for what actually ships; the Mechanism/Repair sections immediately
below describe the ORIGINAL (round-1) fix for the historical record.

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

## Residual: volume-guiding bsdfPdf composition — DL-73 RULED NOT A DEBT (round-2)

The original (round-1) text of this section filed DL-73 on the theory
that the volume continuation's `rs2.bsdfPdf = phasePdf` (raw,
un-combined) was inconsistent with the main surface continuation's
`effectiveBsdfPdf` (combined) convention, and should probably be
"fixed" to match it.

**Round-2 review (P2-C, this pass) derived that this is BACKWARDS.**
Env-NEE at a volume vertex weights against `MediumScatterMaterial::Pdf`
(`MediumTransport.cpp`'s `EvaluateInScattering` constructs this adapter
and calls into `LightSampler::EvaluateDirectLighting`'s env arm,
`LightSampler.cpp` ~:2652), and `MediumScatterMaterial::Pdf` simply
returns `m_pPhase->Pdf(vToLight, m_wo)` — the RAW, un-guided phase pdf,
with NO guiding mixture involved. The escape side (`RayCaster.cpp`'s
`rs2.bsdfPdf = phasePdf`) uses that SAME raw `phasePdf`. Both sides feed
`PathTransportUtilities::PowerHeuristic` the identical pair
`(phasePdf, envPdf)` (in opposite argument order), which is exactly
`PowerHeuristic(a,b) + PowerHeuristic(b,a) == 1` — the two weights are
complementary halves of ONE power-heuristic pair and sum to exactly 1,
UNBIASED. The `guidingMISWeight = phasePdf / combinedPdf` factor
(folded into `rs2.importance` at both volume-continuation sites) already
applies the ENTIRE guiding correction to this sample's contribution;
substituting `combinedPdf` for `bsdfPdf` in the MIS weight ALSO, as the
round-1 ruling proposed, would apply that correction a SECOND time to
the weight alone and break the partition (`w_bsdf + w_nee != 1`) —
literally the opposite of a fix.

**DL-73 is therefore STRUCK as "ruled consistent (derivation)"**, not a
debt — see `docs/DEBT_LEDGER.md`'s Not-a-debt section. The code at
`RayCaster.cpp`'s two volume-continuation `rs2.bsdfPdf = phasePdf`
assignments is CORRECT AS WRITTEN and must not be changed to match the
surface convention.

**The real, opposite-signed asymmetry is on the SURFACE path, filed as
DL-74**: `PathTracingIntegrator.cpp` ~:3411/~:5507 stores the COMBINED
`effectiveBsdfPdf` (`= combinedPdf` under guiding, `= risEffectivePdf`
under RIS) in `rs2.bsdfPdf` for the surface escape weight, but surface
env-NEE (`LightSampler.cpp` ~:2641-2652, the SAME env arm the volume
case above resolves through) weights against `pMaterial->Pdf(envDir,
ri, defaultIOR)` — the RAW material pdf, with no guiding mixture. Here
the two sides feed the power heuristic DIFFERENT pdf values whenever
guiding fires (`combinedPdf` on escape vs. the raw material pdf on
NEE), so `w_bsdf + w_nee != 1` on the surface path — the partition
violation the round-1 DL-73 ruling was looking for, just on the wrong
side of the surface/volume split. See DL-74's own ledger row for the
recipe.

## Round-2 correction (P2-B, 2026-09-13)

Independent review this pass found the round-1 training wiring at BOTH
kinds of site (RayCaster.cpp's two volume sites, PathTracingIntegrator.cpp's
two BSSRDF sites) has its own defects, distinct from the DL-73/DL-74
finding above:

1. **RayCaster.cpp volume sites — moment/count mismatch.**
   `RayCasterEnvEscapeMISWeight`'s training arm calls
   `OptimalMISAccumulator::Accumulate` (adds to the tile's moment SUM)
   whenever `rs.bsdfTimesCos` is nonzero and the escape reaches the env
   map, but NOTHING in `RayCaster.cpp` ever calls `AccumulateCount` for
   this continuation (grep confirms zero call sites in that file) —
   `Solve()` divides `Mbsdf = rawBsdf / nBsdf`, so training a moment
   with no matching count inflates every affected tile's `Mbsdf` and
   depresses the solved `alpha` wherever volume scattering occurs. The
   rendered radiance stays unbiased (it still divides by the REAL
   sampling pdf via `throughput`/`importance`, never by `alpha`
   directly), but the alpha estimate — and hence every OTHER strategy's
   applied weight in that tile once `OptimalMIS2Weight` is in use — is
   skewed. A variance regression, not a bias.
2. **Wrong-shaped quantity at all four sites.** `IRayCaster.h` documents
   `RAY_STATE::bsdfTimesCos` as "BSDF * cos at the SCATTER point" — a
   DIRECTIONAL quantity evaluated at the continuation ray's origin,
   paired with `bsdfPdf` (the pdf of THAT SAME direction) so that
   `Accumulate`'s `(f/p)^2` moment estimator sees a coherent
   numerator/denominator pair. Neither the volume value
   (`throughput * phasePdf`, matching the `PTBsdfTimesCos(throughput,
   pdf)` convention used elsewhere and structurally plausible) NOR the
   PT SSS value (`sssThroughput * bssrdf.cosinePdf`) is actually that:
   `sssThroughput` carries the disk-projection profile's SPATIAL
   transport weight (`Rd * FtExit / pdfSurface`, an area-measure
   quantity over the entry/exit projection, not a direction-measure
   BSDF value), so its product with `bssrdf.cosinePdf` is not the
   integrand the moment estimator assumes either.

**Conservative fix**: `rs2.bsdfTimesCos` is set to zero (`RISEPel(0,0,0)`
/ `Traits::zero()`) at all FOUR sites — `RayCaster.cpp`'s RGB and NM
volume continuations, and `PathTracingIntegrator.cpp`'s two BSSRDF
continuation sites (both tags, so the structural if-constexpr-gate fix
from round 1 stays — it is unaffected and correct; only the trained
VALUE reverts to zero). At `PathTracingIntegrator.cpp`'s sites the
paired `AccumulateCount` call is ALSO removed (leaving both sides of
the pair absent, rather than reintroducing the opposite mismatch — a
count with no matching moment would deflate `Mbsdf` instead of
inflating it, the same species of bug in the other direction).
**Training at these four sites is therefore NOT WIRED** — matching the
pre-round-1 state functionally, though for different, now-understood
reasons (round 1 diagnosed a backwards `if constexpr` gate as the whole
story; round 2 found the wiring itself, once ungated, was unsound). The
render-time weight is UNCHANGED either way: `PowerHeuristic` is the
correct fallback whenever `rc.pOptimalMIS` is absent or not ready, which
is the only configuration these four sites' zeroed training affects.

A properly wired fix would need (a) a real paired `AccumulateCount` at
the two `RayCaster.cpp` sites, and (b) the actual directional BSDF*cos
quantity at the SSS continuation's origin (not the profile's spatial
weight) — neither is derived or implemented in this pass; a future
slice should do both together with a test that checks the accumulator's
count and moment pairing directly (`OptimalMISAccumulatorTest`'s "AccumulateCount/Accumulate split" test is the pattern to extend).

### Round-2 impact wording correction (P3-3)

The row's original "not user-visible" impact wording assumed the
round-1 fix was harmless because it only affected `alpha`. That framing
undersold the actual mechanism even at round 1: a trained, non-fallback
`alpha` DOES change every subsequent frame's per-strategy weights
(`OptimalMIS2Weight`), so a training-input change is never strictly
"not user-visible" once the accumulator is `IsReady()` — it changes
which alpha gets solved, hence which weights get applied, while
remaining UNBIASED (the two weights still sum to 1 for any alpha in
range). Since round 2 reverts training to unwired (moot) at these four
sites, the impact is now correctly "not applicable" rather than
"not user-visible" — there is no trained contribution from these sites
to have an impact at all.

## File status

| File | Status |
|---|---|
| `src/Library/Shaders/PathTracingIntegrator.cpp` | Round 1: unconditional `bsdfTimesCos` assignment at both BSSRDF continuation sites (`0c9eccc4`). Round 2: reverted to `Traits::zero()` at both sites, `AccumulateCount` call removed; the round-1 if-constexpr-gate structural fix (both tags computed identically) is retained. |
| `src/Library/Rendering/RayCaster.cpp` | Round 1: `bsdfTimesCos` assignment at both volume phase-scatter continuation sites (`0c9eccc4`). Round 2: reverted to `RISEPel(0,0,0)` at both sites; `rs2.bsdfPdf = phasePdf` (DL-73's subject) is UNCHANGED and confirmed correct. |
| `docs/DEBT_LEDGER.md` | Round 1: new DL-72 row, closed. Round 2: DL-73 struck as not-a-debt, DL-74 filed for the real surface-path asymmetry, DL-72's evidence and impact wording updated for the revert. |
| `docs/DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md` | Added round 1; round-2 correction sections added this pass. |

Gate: `MISWeightsTest`, `OptimalMISAccumulatorTest`,
`RasterizerDefaultsConsistencyTest`, `RayCasterEnvEscapeMISTest` (91/91,
P3-2: the suite grew from 79 to 91 checks the same day via `0eb7a47e`),
`SSSRadianceScalingTest` (574017 checks, 0 failures — unchanged),
`RandomWalkSurvivalTest` all pass; `make -C build/make/rise -j8 all`
clean (zero warnings).

## Tag errata (P3-7)

This row and this file were originally authored under the id **DL-69**
(commit `0c9eccc4` and its immediate predecessors/successors still say
DL-69 in their trailers, which cannot be edited retroactively); it was
renamed to DL-72 in a later same-branch commit because DL-69 had since
been claimed by an unrelated, already-merged slice. The volume-guiding
residual this file originally filed as **DL-70** is likewise now
DL-73 (subsequently struck as ruled-consistent, see above) — any
`DL-69`/`DL-70` string in this repository's commit history refers to
what this document now calls DL-72/DL-73 respectively.
