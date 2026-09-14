# DL-72: BSSRDF/volume continuations never trained RayCasterEnvEscapeMISWeight's optimal-MIS arm

Status: **CLOSED 2026-09-14 (round 3)** — correct wiring landed this pass
(debt-guiding2 slice), see "Round 3" below. History: CLOSED 2026-09-13 —
source repair `0c9eccc4`; REOPENED and re-resolved by REVERTING the
training wiring, round-2 (P2-B) same day — the round-1 fix's wiring
itself had a count-pairing defect (RayCaster.cpp sites) and fed the
accumulator a quantity that does not match its documented contract (all
four sites). See "Round-2 correction (P2-B)" for that ruling and "Round
3" for the fix that supersedes it; the Mechanism/Repair sections
immediately below describe the ORIGINAL (round-1) fix for the historical
record.

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

## Round 3 (2026-09-14, debt-guiding2 slice): correct wiring, CLOSED

> **SUPERSEDED IN PART by round 4 (same slice, same day) — read this
> section with the substitution below.**  Everywhere this section says the
> volume sites set `rs2.bsdfPdf = phasePdf`, or that the `AccumulateCount`
> gate is `phasePdf > 0`, round 4 changed it: the raw `phasePdf` moved to
> the NEW `rs2.bsdfMisPdf` field (the MIS-partner role, which is what
> DL-73's ruling was actually about), `rs2.bsdfPdf` now carries
> `effectivePdf` (the density the direction was really drawn from, which
> is what the moment must be divided by), and the count gate follows
> `effectivePdf > 0`.  Under guiding the two differ; with guiding off they
> are the same number and this section reads correctly as written.  The
> trained ratio `bsdfTimesCos / bsdfPdf` is therefore `phaseValue /
> effectivePdf` — this continuation's own per-sample weight — rather than
> identically 1.  See "Round 4" below and
> docs/DL74_ENV_NEE_GUIDING_PARTITION.md §2.6.

Round 2 left training unwired at all four sites because (a) `RayCaster.cpp`'s
two volume sites called `Accumulate` with no paired `AccumulateCount`, and
(b) neither the volume nor the BSSRDF trained quantity actually matched
`IRayCaster.h`'s "BSDF*cos at the scatter point" contract. Round 3 derives
the correct directional quantity for each site and pairs every `Accumulate`
with exactly one `AccumulateCount`, per attempt, matching the ordinary
surface continuation's own pattern.

**Volume phase-scatter continuation** (`RayCaster.cpp`'s `CastRay`/
`CastRayNM`, both RGB and NM copies): `IPhaseFunction::Pdf()`'s own
contract ("For normalized phase functions this equals Evaluate()") means
the phase VALUE at the sampled direction is exactly `phasePdf`, and a phase
function has no separate cosine term (no surface normal in free space) —
so the volume analogue of "BSDF*cos" is just `phasePdf`, broadcast to all
three channels (`rs2.bsdfTimesCos = RISEPel(phasePdf, phasePdf, phasePdf)`,
replacing `RISEPel(0,0,0)`), paired with a new `AccumulateCount` call
immediately after (gated on `phasePdf > 0`, mirroring `rs2.bsdfPdf =
phasePdf`, DL-73's confirmed-correct raw pdf; round 4 moved that raw value
to `rs2.bsdfMisPdf` and put `effectivePdf` in `rs2.bsdfPdf` — see the
callout at the top of this section). With guiding OFF this makes
`bsdfTimesCos / bsdfPdf == 1` identically — correct, since a perfectly
importance-sampled phase function contributes no variance of its own to
the single-technique estimator; the trained second moment reduces to
`(envRadiance)^2`, independent of the actual `phasePdf` value (verified
directly in `OptimalMISAccumulatorTest`'s new Test 12, see below).

**BSSRDF continuations** — **SUPERSEDED BY ROUND 5 (§5 below).** The
quantity this paragraph derives, `Sw * cos`, is the exit's integrand with
the area-measure `weightSpatial` DROPPED; the accumulator's moment is of
the integrand, so the numerator is `weight * cosinePdf` and the helper is
now `PTBssrdfTrainedBsdfTimesCos`. The paragraph is kept for the record;
its algebra is correct, its conclusion is not.

(`PathTracingIntegrator.cpp`, both the diffusion-
profile site and its `RandomWalkSSS` twin, both tags): recovered
algebraically from the ALREADY-COMPUTED `bssrdfWeight`/`bssrdfWeightSpatial`
split, no new BSSRDF-file code needed. `BSSRDFSampling.h`'s own header
comment gives `weight = Rd*Ft(exit)*Ft(entry)/(c*pdfSurface)`,
`weightSpatial = Rd*Ft(exit)/pdfSurface` (no entry Sw), and
`Sw = Ft(entry)/(c*PI)` (`EvaluateSwWithFresnel`), so
`weight/weightSpatial = Ft(entry)/c = Sw*PI`, independent of `Rd` and
`pdfSurface` (both cancel). The exit direction is cosine-weighted
(`cosinePdf = cos/PI`), so `Sw*cos = (weight/weightSpatial)*cosinePdf` —
the PI cancels. A new helper pair, `PTBssrdfSwTimesCos` (one overload for
`RISEPel`, one for `Scalar`), computes this; the `RISEPel` overload works
off `ColorMath::MaxValue()` rather than a per-channel divide because `Sw`
is documented achromatic ("Sw value (scalar, achromatic)") and a
per-channel divide would produce a spurious NaN/Inf on any channel where
the diffusion profile's `Rd` happens to be exactly zero even though the
ratio is channel-independent. Paired with a new `AccumulateCount` call
placed BEFORE Russian roulette (mirroring the main surface continuation's
placement — an RR-killed attempt is still counted, per
`OptimalMISAccumulator.h`'s "regardless of whether the sample contributed
non-zero radiance" contract), gated on `PTSurvivalMagnitude(sssThroughput)
> NEARZERO`.

**Red-proof**: `OptimalMISAccumulatorTest`'s new Test 12
(`TestDL72PairedTrainingFires`) drives the shared `OptimalMISAccumulator`
contract directly through three scenarios built on identical base traffic
(200 ordinary BSDF hits f2=4/pdf=1, 200 NEE hits f2=4/pdf=1, plus 50
"SSS/volume" hits f2=16/pdf=1): **unwired** (round-2's conservative fix —
the 50 extra hits contribute nothing) solves `alpha=0.5`; **buggy**
(round-1's exact pattern — `Accumulate` with no `AccumulateCount` for the
50 extra hits) solves `alpha=1/3` (Mbsdf inflated to 1600/200=8.0,
matching the ledger's own "inflating Mbsdf... and depressing alpha"
wording exactly); **fixed** (round 3 — paired) solves `alpha=4/10.4≈0.3846`,
the theoretically correct value (Mbsdf=1600/250=6.4). All three numbers
are hand-derived and checked exactly (not just "differs"), and a fourth
sub-check confirms the phasePdf/cosinePdf-invariance property the volume
and BSSRDF fixes both rely on (three different pdfs, same
`bsdfTimesCos/bsdfPdf` ratio of 1, identical trained moment). This proves
the CONTRACT-level bug and fix (PathTracingIntegrator.cpp / RayCaster.cpp
only ever consume this same public accumulator API); it does not itself
render a BSSRDF/volume scene and inspect the trained `alpha`, since no
public accessor exposes `OptimalMISAccumulator`'s per-tile state after a
real render — the existing render-level suites below instead confirm NO
REGRESSION in actual rendered radiance from wiring this training (the
render-time weight was already `PowerHeuristic`-correct before AND after,
since training only ever affects `Solve()`'s `alpha`, never the raw
sampling-pdf division), matching round 1's own validation precedent.

## Round 4 (2026-09-14, debt-guiding2 review round 2): two corrections

Round 3's wiring is kept. Review found two things wrong ABOUT it.

### 4.1 Round 3's "red-proof" was not one (P2-4)

The DL-72 ledger row cited `OptimalMISAccumulatorTest`'s Test 12 as the
red-proof. Test 12 drives `OptimalMISAccumulator`'s PUBLIC API by hand
and hand-derives the three alphas. That pins the ACCUMULATOR'S
ARITHMETIC — an unpaired `Accumulate` really does inflate `Mbsdf` — but
it executes no line of `RayCaster.cpp` or `PathTracingIntegrator.cpp`,
so it is structurally blind to whether either file calls the API at all.
Which is exactly what both DL-72 regressions were: round 1 accumulated a
moment with no count; round 2 removed both.

`tests/OptimalMISTrainingSitesTest.cpp` is the real red-proof. It runs
both production sites — an optically thin global fog under an
environment (thin on purpose: a thick fog re-scatters until the volume
bounce limit ends the walk and the escape arm, the arm under test, is
never reached), and a real `SubSurfaceScatteringMaterial` on a real
sphere `Object` driven through production `IntegrateFromHit` in an
object-free environment-lit scene — and reads `Solve()`'s branch table
back out. The NEE half of the tile is supplied synthetically so the
solved alpha is a function of the BSDF half alone: clampMin when the
BSDF count is short or its moment is zero, clampMax when the BSDF moment
is absent or negligible beside NEE's, and an interior value only when
both advanced.

With `RayCaster.cpp` and `PathTracingIntegrator.cpp` reverted to
`ddf05c6c` — round 2's "leave the arm UNWIRED" state — the volume site
reads exactly **0.001** (clampMin) and the BSSRDF site exactly **0.999**
(clampMax): 5 passed, 2 failed. With the round-3 wiring in place they
read **0.857** and **0.929**: 7 passed, 0 failed.

Test 12's own header has been corrected to say what it does and does not
cover, and the ledger row no longer calls it the red-proof.

### 4.2 The volume moment was divided by the wrong density (P2-5)

Round 3 set `rs2.bsdfPdf = phasePdf` at both volume sites and paired the
moment with it. But under guiding the direction is drawn from
`effectivePdf` (the guided mixture `combinedPdf`), and `Li` is rescaled
by `guidingMISWeight = phasePdf / combinedPdf` afterwards, so the
continuation's real contribution is `phaseValue * Li / effectivePdf`.
Dividing the trained moment by `phasePdf` instead therefore biased the
moment whenever guiding was active — variance-only, like the rest of
this row, but wrong.

The obstacle was that DL-73 requires that same `phasePdf` to stay the
volume vertex's MIS PARTNER (env-NEE at a volume vertex weights with the
raw `MediumScatterMaterial::Pdf`), and one field cannot be two
quantities. DL-74's repair splits them: `RAY_STATE::bsdfPdf` is now the
TRUE sampling density and the new `RAY_STATE::bsdfMisPdf` is the MIS
partner. So both volume sites now set

```
rs2.bsdfMisPdf = phasePdf;      // DL-73's requirement, unchanged in effect
rs2.bsdfPdf    = effectivePdf;  // what the direction was really drawn from
```

and the trained ratio `bsdfTimesCos / bsdfPdf` becomes
`phaseValue / effectivePdf` — exactly this continuation's own per-sample
weight (1 with guiding off, `guidingMISWeight` with it on). The paired
`AccumulateCount` gate moves from `phasePdf > 0` to `effectivePdf > 0`
for the same reason. The round-3 comment claiming the ratio is
"exactly 1" was true only with guiding off and has been corrected in
place.

Rejected alternative: skipping training entirely whenever the direction
was guided. It is simpler, but it biases the solved alpha toward the
unguided regime on exactly the scenes where guiding is doing work, and
it discards samples for no reason once the two roles are in two fields.

See [DL74_ENV_NEE_GUIDING_PARTITION.md](DL74_ENV_NEE_GUIDING_PARTITION.md)
§2.6 for the surface-side half of the same split.

## Round 5 (2026-09-14, debt-guiding2 review round 4): the BSSRDF pair trains the FULL integrand (P2-3)

Round 3 derived the BSSRDF exit's trained numerator as `Sw * cos` and
argued it from `IRayCaster.h`'s "BSDF*cos at the scatter point" wording.
That wording describes the SURFACE continuation, where `BSDF*cos` IS the
whole vertex-local contribution.  At a BSSRDF exit it is not.

### 5.1 The rule, restated from the accumulator's own derivation

`OptimalMISAccumulator.h` ("CORRECT MOMENT ESTIMATION", Kondapaneni 2019):

    M_i = E_{x~p_i}[ (f(x) / p_i(x))^2 ]

with `f` **the integrand** — the entire vertex-local contribution of the
sample — and `p_i` the density it was drawn from.  Every other site in
the tree obeys that reading:

| Site | trained numerator | is it the whole vertex-local contribution? |
|---|---|---|
| surface continuation | `scatterThroughput * effectiveBsdfPdf` | yes — the whole `BSDF*cos` |
| volume continuation | `phaseValue` (`= phasePdf`) | yes — a phase function has no other local factor |
| NEE (both arms) | `contrib` / `envContrib * envPdf` | yes — geometry factor included |
| BSSRDF exit (round 3) | `Sw * cos` | **no** — `weightSpatial` dropped |
| BSSRDF entry NEE (all rounds) | `contrib` | **no** — the caller applies `weightSpatial` afterwards |

### 5.2 What the BSSRDF exit's integrand is

From `BSSRDFSampling.h`'s own header:

    weight        = Rd * Ft(exit) * Ft(entry) / (c * pdfSurface)
    weightSpatial = Rd * Ft(exit) / pdfSurface
    Sw            = Ft(entry) / (c * PI)

so `weight = weightSpatial * Sw * PI`; the exit direction is cosine
sampled, `cosinePdf = cos/PI`; and the contribution the continuation adds
is `weightSpatial * Sw(w) * cos(w) * L(w) / cosinePdf`, which is why the
code multiplies the escaped radiance by `weight`.  Therefore

    f(w) = weightSpatial * Sw(w) * cos(w) = weight * cosinePdf     (exact)
    f/p  = weight

`weightSpatial = Rd * Ft(exit) / pdfSurface` is an **area-measure**
quantity, not an O(1) directional factor, so dropping it scaled this
site's moments by an arbitrary amount and skewed the solved alpha of
every tile an SSS surface touches.  Alpha cannot bias a
partition-of-unity weight, so the damage is variance only — but it is
the same failure family this row keeps producing.

### 5.3 Why not "leave it untrained"

The review offered that alternative (as PT's in-loop volume site is left,
DL-84).  Rejected: it silences the BSDF half while the NEE half goes on
training the same mis-scaled moment UNPAIRED.  The NEE half lives inside
`LightSampler` and is governed by a per-scene accumulator pointer, not a
per-call flag, so there is no cheap way to silence it alongside.  Fixing
both is both smaller and more correct than silencing one.

### 5.4 Implementation

* `PTBssrdfSwTimesCos` -> `PTBssrdfTrainedBsdfTimesCos( weight, cosinePdf )`,
  a multiply rather than a ratio.  Consequences: the Rd==0 guard the
  ratio form needed is gone, and the Pel form stays **per-channel**
  instead of collapsing to `MaxValue()`.
* `LightSampler::EvaluateDirectLighting{,NM}` take a trailing
  `neeTrainingScale` (default 1).  It multiplies the TRAINING integrand
  at all four `Accumulate` sites and **nothing else** — the returned
  radiance is bit-identical at any value.  The two BSSRDF/random-walk
  entry-NEE call sites in `PathTracingIntegrator.cpp` pass
  `PTSurvivalMagnitude(bssrdfWeightSpatial)`; every other call site takes
  the default.

### 5.5 Red-proof: the spatial-weight scaling law

`OptimalMISTrainingSitesTest`'s existing rows cannot see this defect: the
`L_env^2` row scales the RADIANCE, which right and wrong numerators both
carry, and `Solve()`'s alpha is a ratio that cannot see a constant scale
at all.  The new row scales the diffusion profile's **`Rd`** by 2 through
a decorator that forwards `SampleRadius` / `PdfRadius` /
`FresnelTransmission` / `GetIOR` verbatim, so no sampling decision moves;
it hands the walk a large path importance so `RayCaster`'s own
`rs.importance < RC_RR_THRESHOLD` survival test (which reads the scaled
BSSRDF throughput) cannot fire for either run; and it wires the
accumulator into the `LightSampler` so the NEE half is measured too.
Both runs then visit identical vertices and make identical attempts, and
every trained integrand must scale by exactly 2, every moment by 4.  The
tile also holds the SSS SURFACE vertex's own Rd-independent events, so
the bound is `1 < S(2)/S(1) < 4`, approached from below.

    before (145c9259)
    BSSRDF site: Rd x2  BSDF sum 432.6 -> 432.6 (attempts 1028 / 1028) ,  NEE sum 1149.65 -> 1149.65 (attempts 1028 / 1028)
    BSSRDF site: moment ratios  BSDF 1 , NEE 1  (bounded 1 < r < 4)
      FAIL: BSSRDF site: the BSDF moment carries weightSpatial (scales as Rd^2)
      FAIL: BSSRDF site: the entry-NEE moment carries the same weightSpatial (scales as Rd^2)
        21 passed, 2 failed

    after (e55308b9)
    BSSRDF site: Rd x2  BSDF sum 1325.8 -> 5302.7 (attempts 1028 / 1028) ,  NEE sum 3565.15 -> 14245.1 (attempts 1028 / 1028)
    BSSRDF site: moment ratios  BSDF 3.99962 , NEE 3.99567  (bounded 1 < r < 4)
        23 passed, 0 failed

Both pre-fix ratios are EXACTLY 1 — neither numerator depended on `Rd` at
all — while the attempt counts confirm the two runs walked the same
paths.  The `L_env^2` rows are unchanged at 9 (exact) on both sites.

## File status

| File | Status |
|---|---|
| `src/Library/Shaders/PathTracingIntegrator.cpp` | Round 1: unconditional `bsdfTimesCos` assignment at both BSSRDF continuation sites (`0c9eccc4`). Round 2: reverted to `Traits::zero()` at both sites, `AccumulateCount` call removed. Round 3: `PTBssrdfSwTimesCos` helper added; both sites wired to a directional quantity, `AccumulateCount` reinstated (paired, before Russian roulette). Round 5: that quantity replaced by the FULL vertex-local integrand — `PTBssrdfTrainedBsdfTimesCos(weight, cosinePdf)` — and the two entry-NEE calls pass `neeTrainingScale` so the pair's other half carries the same factor (§5). |
| `src/Library/Interfaces/IRayCaster.h` | Round 4: `RAY_STATE` gains `bsdfMisPdf` + `MisPartnerPdf()` (DL-74), which is what lets the volume sites carry the true sampling density and the MIS partner at the same time. |
| `src/Library/Rendering/RayCaster.cpp` | Round 1: `bsdfTimesCos` assignment at both volume phase-scatter continuation sites (`0c9eccc4`). Round 2: reverted to `RISEPel(0,0,0)` at both sites; `rs2.bsdfPdf = phasePdf` (DL-73's subject) is UNCHANGED and confirmed correct. Round 3: `rs2.bsdfTimesCos = RISEPel(phasePdf,phasePdf,phasePdf)` at both sites, `AccumulateCount` added (paired, gated `phasePdf > 0`). Round 4: `rs2.bsdfPdf = effectivePdf` (the true sampling density) with the raw `phasePdf` moved to `rs2.bsdfMisPdf`; the count gate follows `effectivePdf > 0`. |
| `tests/OptimalMISAccumulatorTest.cpp` | Round 3: new Test 12 (`TestDL72PairedTrainingFires`), 7 new checks. Round 4: header corrected — Test 12 pins the accumulator's arithmetic, not the production call sites. |
| `tests/OptimalMISTrainingSitesTest.cpp` | Round 4: added. The row's actual red-proof; drives both production sites. Round 5: the Rd-scaling-law row (`ScaledRdProfile` / `ScaledRdMaterial`), which pins the trained QUANTITY on both halves of the BSSRDF pair; 23 checks. |
| `src/Library/Lights/LightSampler.{h,cpp}` | Round 5: `EvaluateDirectLighting{,NM}` take `neeTrainingScale` (default 1); all four `Accumulate` sites apply it. Training-only — the returned radiance is unaffected at any value. |
| `docs/DEBT_LEDGER.md` | Round 1: new DL-72 row, closed. Round 2: DL-73 struck as not-a-debt, DL-74 filed for the real surface-path asymmetry, DL-72's evidence and impact wording updated for the revert. Round 3: DL-72 CLOSED again, this time with correct wiring. |
| `docs/DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md` | Added round 1; round-2 correction sections added same day; this "Round 3" section added 2026-09-14. |

Gate (round 3): `MISWeightsTest` (59/0), `OptimalMISAccumulatorTest`
(34/0, was 27/0 — the 7 new Test 12 checks), `RasterizerDefaultsConsistencyTest`
(164/0), `RayCasterEnvEscapeMISTest` (91/91 — this file's earlier "91/91,
P3-2" note stands; `tests/README.md`'s stale "79/79" line for this suite
was also corrected this pass), `SSSRadianceScalingTest` (574017/0,
unchanged), `PTGuidedSelectProbTest` and `TranslucentIORStackTest` (ALL
TESTS PASSED, unaffected — these do not exercise BSSRDF/volume training),
`AgentLiveCommitTest` (884/0, DL-66's own gate), `EnvLightBalanceTest`
(116/116); `make -C build/make/rise -j8 all` clean (zero warnings) after
every edit in this pass.

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
