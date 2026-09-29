# DL-306: one Fresnel law on both sides of the SSS boundary

Slice `debt-dl306`, branched from `master` `f7e212d4`, 2026-09-28.
Regressions: `tests/BSSRDFNormalizationTest.cpp` Test E (deterministic),
`tests/SSSExteriorIndexInvarianceTest.cpp` Part C (rendered furnace).

## 1. The defect

An SSS boundary event is priced by two halves that describe ONE interface:

- `SubSurfaceScatteringSPF` prices the surface REFLECTION with the exact
  dielectric Fresnel law (`Optics::CalculateDielectricReflectance` against
  `ior_stack.top()`).
- Every TRANSMISSION into (and out of) the subsurface event -- the diffusion
  profiles' `FresnelTransmission`, the random walk's entry coin (PT and both
  BDPT subpaths) and its exit Sw, the `BSSRDFEntryAdapters.h` NEE adapters and
  `PathVertexEval`'s BSSRDF-entry re-evaluation -- used Schlick's
  approximation, normalized by DL-48/DL-49's `20(1-F0)/21` (and its
  `eta^2` twin for a denser exterior).

Schlick is not `1 - F_exact`, so `R(mu) + T(mu) != 1`.  Measured on the
real SPF and the real Burley profile (Test E, 4096-bin cosine quadrature;
`<.>` is the cosine-weighted hemispherical mean):

| relative index | `<R>` | `<T>` Schlick | `<R>+<T>` pre | worst pointwise `|R+T-1|` pre | `<R>+<T>` post | worst post |
|---|---:|---:|---:|---:|---:|---:|
| 1.05 (air) | 0.013886 | 0.951814 | **0.965700** | 0.3175 | 1 | 2.2e-16 |
| 1.128 (air) | 0.030858 | 0.948935 | **0.979794** | 0.1739 | 1 | 2.2e-16 |
| 1.33 (air) | 0.065931 | 0.933277 | **0.999208** | 0.0599 | 1 | 2.8e-16 |
| 1.5 (air) | 0.091778 | 0.914286 | **1.006064** | 0.0357 | 1 | 3.3e-16 |
| 1.5 in water (1.1278) | 0.030823 | 0.948944 | **0.979767** | 0.1741 | 1 | 4.4e-16 |
| 1.33 in 1.5 glass (0.8867) | 0.238052 | 0.746040 | **0.984092** | 0.1741 | 1 | 7.5e-14 |
| 1.5 in 1.575 (0.9524) | 0.105565 | 0.863324 | **0.968889** | 0.3175 | 1 | 5.4e-14 |

The pointwise mismatch is largest at grazing (Schlick keeps reflecting
`(1-mu)^5` there even at a matched index -- pre-fix `BSSRDFSamplingTest`
read `R + Ft = 0.049` at `eta = 1, mu = 0.01`).

## 2. Ruling

**Exact dielectric Fresnel on both sides.**  The reflection side is the
physically exact law and is kept; the transmission side becomes
`Ft(mu) = 1 - F_exact(mu; 1 -> eta)` at the RELATIVE index (DL-49),
evaluated with the same `Optics::CalculateDielectricReflectanceCosine`
the rest of the tree uses.  A denser exterior (`eta < 1`) needs no special
case: the Fresnel function evaluates the transmitted cosine and returns
total reflection past the critical angle.  Schlick on the reflection was
rejected: it would make the partition hold by degrading the one side that
is already right, and it would still disagree with every other dielectric
in the tree.

The Sw normalization is re-derived for the new law.  For `eta >= 1`,
`c(eta) = 2 * integral_0^1 Ft(mu) mu dmu = 1 - r(eta)` where `r` is the
classical hemispherical (diffuse) Fresnel reflectance, which HAS an exact
closed form (logs and rationals of `n`), so no table was needed:

    r(n) = 1/2 + (n-1)(3n+1)/(6(n+1)^2)
               + n^2 (n^2-1)^2/(n^2+1)^3 * ln((n-1)/(n+1))
               - 2 n^3 (n^2+2n-1)/((n^2+1)(n^4-1))
               + 8 n^4 (n^4+1)/((n^2+1)(n^4-1)^2) * ln(n)

It agrees with an independent quadrature to < 1e-11 for `n - 1 >= 1e-3`
(checked at 1.001 ... 2.5).  Below that its log terms cancel
catastrophically (1.4e-9 off at `1 + 1e-4`, 1.1e-5 at `1 + 1e-6`), so
within `1e-3` of a matched index `c` is interpolated linearly between the
exact `c(1) = 1` and the closed form at `1.001`; Test E measures the worst
error over ten sampled offsets `|eta - 1| in [1e-6, 3e-3]`, both sides,
at **6.3e-7**; the external review's dense scan (2000 points per side)
puts the true in-band worst at **6.67e-7, at eta = 1.00048**, inside the
test's 1e-6 gate.  For
`eta < 1`, Fresnel's symmetry in the two sides plus Snell
(`mu dmu = eta^2 t dt`) give `c(eta) = eta^2 * c(1/eta)` exactly -- the
same identity DL-49 derived for the Schlick law.  Sw = `Ft/(c pi)` keeps a
unit exterior cosine-hemisphere integral at every index, so **DL-04's
complete-event convention is unchanged**.

| relative index | `c` Schlick (pre) | `c` exact (post) |
|---|---:|---:|
| 1.05 | 0.951814 | 0.986114 |
| 1.128 | 0.948935 | 0.969142 |
| 1.33 | 0.933277 | 0.934069 |
| 1.5 | 0.914286 | 0.908222 |
| 0.8867 | 0.746040 | 0.761946 |

## 3. Sites

| Site | Change |
|---|---|
| `BSSRDFSampling.h` | new `BoundaryTransmission` (exact `1-F`), `ExactExternalDiffuseFresnelReflectance`, `BoundaryTransmissionNormalization`; removed `SchlickBoundaryCosine`, `RandomWalkSchlickTransmission`, `SchlickTransmissionNormalization`; `EvaluateSwWithFresnel` normalizes with the new constant |
| `BurleyNormalizedDiffusionProfile::FresnelTransmission` | exact law; its private `SchlickFresnel` removed |
| `DonnerJensenSkinDiffusionProfile::FresnelTransmission` | exact law (**body only** -- the file's now-unused private `SchlickFresnel` is left in place to keep this slice's footprint in that file minimal while `debt-dl291` edits its precompute; delete it after both merge) |
| `BSSRDFSampling::SampleEntryPoint` | Sw normalization |
| `RandomWalkSSS::SampleExit` | exit Sw law and normalization |
| `BSSRDFEntryAdapters.h` `RandomWalkEntryBSDF` | law and normalization (the diffusion adapter already read the profile) |
| `PathVertexEval.h` (RGB, NM) | random-walk entry Sw |
| `PathTracingIntegrator.cpp` | random-walk entry coin |
| `BDPTIntegrator.cpp` (eye and light subpaths) | inline Schlick coin replaced by the helper; `R = 1 - Ft`, so the reflect branch's `R_spf / R` compensation is exactly 1 |

No library source file added or removed.  PT HWSS falls back to NM at SSS
vertices; VCM and MLT reach every BDPT site through the shared generator
and `PathVertexEval`.

## 4. Red-proof (isolated A/B against committed state)

Tests committed first (`7ab2a2cf`), fix committed (`841852b6`), then the
ten fix files reverted with `git checkout f7e212d4 -- <files>` (tests kept
at HEAD), library and tests rebuilt, run, restored with
`git checkout HEAD -- <files>` and rebuilt.

| Suite | reverted fix | fix |
|---|---|---|
| `BSSRDFNormalizationTest` | **41 failures** (Test E 28: all 7 indices pointwise, hemispherical and both adapter shapes; the near-`eta = 1` normalization sweep 1, off by 0.048; the diffusion and random-walk RGB/NM sample-ratio groups 12, whose independent oracle now integrates the exact law) | all pass |
| `BSSRDFSamplingTest` | abort in Test C (`R + Ft = 0.049` at `eta = 1, mu = 0.01`) | all pass |
| `SSSExteriorIndexInvarianceTest` | **158/8** | 166/0 |

**Part C (rendered white furnace).**  A conservative SSS sphere (zero
absorption) under a uniform environment of radiance 1, orthographic camera
framing its disk exactly; the environment level `B` is read from pixels
wholly outside the disk, and `H = 1 - (1 - m/B)/(pi/4)` recovers
`<R>+<T>` (the orthographic disk is cosine-weighted).  n = 4 renders per
row, sd of one render:

| Row | spp | pre-fix `H` | post-fix `H` | band |
|---|---:|---:|---:|---:|
| Lambertian control / PT | 64 | 1.00076 +/- 0.00026 | 1.00055 +/- 0.00009 | 0.01 |
| random walk / PT, eta 1.05 | 64 | **0.97202** +/- 0.00050 | 1.00042 +/- 0.00089 | 0.01 |
| random walk / PT, eta 1.128 | 64 | **0.98294** +/- 0.00214 | 1.00041 +/- 0.00066 | 0.01 |
| random walk / PT, eta 1.5 | 64 | **1.00725** +/- 0.00034 | 1.00052 +/- 0.00047 | 0.004 |
| random walk / PT, eta 0.8867 | 64 | **0.98438** +/- 0.00092 | 0.99992 +/- 0.00060 | 0.01 |
| random walk / BDPT, eta 1.05 | 64 | **0.97156** +/- 0.00082 | 1.00005 +/- 0.00012 | 0.01 |
| random walk / BDPT, eta 0.8867 | 64 | **0.98505** +/- 0.00185 | 1.00085 +/- 0.00345 | 0.01 |
| random walk / PT spectral, eta 1.128 | 512 | **0.98397** +/- 0.00260 | 0.99807 +/- 0.00359 | 0.01 |
| diffusion (sigma_s 200) / PT, eta 1.05 | 64 | **0.97441** +/- 0.00061 | 1.00244 +/- 0.00127 | 0.01 |

The pre-fix values sit on the Test E predictions (0.9657 / 0.9798 / 1.0061
/ 0.9841) to within ~0.006; the random walk is used because its albedo is
exactly one; the diffusion row carries Burley's own small finite-sphere
gain.  A BDPT Lambertian control was measured and deliberately NOT gated:
it reads **1.357** in both builds -- the known BDPT uniform-environment
bias (`EnvLightBalanceTest`'s banded BDPT env-only row), unrelated to SSS;
the BDPT diffusion row carries the same bias (1.34 pre-fix), so only BDPT
random-walk rows are gated.

## 5. DL-04 and the other gates

`SSSRadianceScalingTest` **576220/0** after the fix.  Both deliberate
`eta^2` mutations (multiply / divide the PT diffusion and random-walk
complete-event weights, continuation and spatial, by the material index
squared), re-executed on this branch: **576214/6** each -- all six SSS
air-furnace channels, as before.

**The water rows did NOT move to the explicit-volume reference, and the
filing's prediction that they would is refuted.**  (The filing's
"explicit 0.980" is itself stale: DL-284 moved the explicit row to 1.0000.)

| Row | explicit | diffusion pre -> post | random walk pre -> post |
|---|---:|---:|---:|
| water, camera outside (ideal 1) | 0.99999 | 0.96816 -> 0.96874 | 0.96917 -> 0.96944 |
| water inside / air | 1.76892 | 1.72202 -> 1.72338 | 1.71173 -> 1.71364 |

The reason is geometric: that rig's orthographic camera sees the slab at
normal incidence, where Schlick and exact Fresnel coincide (both `F0`), so
the R+T mismatch enters only through light re-hitting the slab obliquely.
The ~3 % residual has a different cause.  This slice's own discriminators
(PT, 256 spp, RW slab of index 1.5 in the same water rig unless noted)
showed it is not the boundary law and is PT-specific:

- a **matched** index (1.33 in water -- no interface at all) still reads
  0.9733 (random walk) / 0.9693 (diffusion); index 2 reads 0.934 / 0.957;
- a white **Lambertian** slab in the same rig reads **0.99992**;
- **BDPT** on the same random-walk scene reads **0.99826**;
- handing the live IOR stack to PT's two SSS entry NEE calls (they pass
  none) leaves it at 0.9698.

The external review of this slice then isolated the mechanism, and it is
NOT "a non-air exterior": **an enclosure of index 1.0 -- no interface at
all -- is WORSE than water** (slab 1.5 in a 1.0 box: random walk 0.9603,
diffusion 0.9831, against 1.0003 / 0.9956 in open air; slab-index sweep in
that box: 1.0 -> 1.00000, 1.5 -> 0.960, 2.0 -> 0.889, i.e. a loss the size
of the normal-incidence reflectance `F0`).  Two INDEPENDENT PT defects:

1. **`RayCaster::CastRay` sets the current object on the CALLER's IOR
   stack** (`RayCaster.cpp:1496`, `ior_stack.SetCurrentObject(ri.pObject)`
   through a `const IORStack&`, `pCurrentObject` being `mutable`).  After
   PT's SSS continuation (`PathTracingIntegrator.cpp:3057` diffusion,
   `:3336` random walk) hits the enclosure, the caller's stack names the
   box as the current object; the box IS in that stack, so the same
   vertex's `SubSurfaceScatteringSPF::Scatter` sees `containsCurrent()`,
   takes the inside/absorb branch, and the surface reflection is lost.
   Passing a COPY of the stack at the two continuation sites restores the
   index-1.0 box exactly (random walk 1.0004 / 0.99998, diffusion 0.9964 /
   0.9988).
2. **PT's RayCaster is created with a hard-coded maximum recursion of 10**
   (`Job.cpp:10357`; `ENABLE_MAX_RECURSION` is on at `RayCaster.cpp:37`),
   and each SSS event nests its continuation cast at `depth + 2`, so paths
   that TIR inside the enclosure and come back to the slab are cut off.
   Raising the cap alone recovers 2.6 % of the water rows' 3.1 % deficit
   (random walk 0.99553, diffusion 0.99406), and it reaches OPEN-AIR
   shipped content: `bdpt_sss_dragon` under PT reads **+0.82 %** with the
   cap raised (0.214115 +/- 0.000169, n = 5, vs 0.215864 +/- 0.000271,
   n = 3, t about 10).

Both fixed together: water camera-outside random walk **1.00019**,
diffusion **0.99550** (= diffusion's own open-air 0.99568); camera inside
1.76883 against explicit 1.76890; matched index random walk 1.00000.
BDPT and a Lambertian slab are immune because neither nests a cast per
event.  Suspects REFUTED by the review (so not to be chased again): the
entry-NEE / continuation MIS partition (forcing the continuation partner
to 0 leaves the water rows unchanged), integrator Russian roulette
(`rr_min_depth 100000`: unchanged), the random walk's exit weight
(`E[Ft*W] = 0.8889` per event in every topology, valid fraction 1), the
continuation's returned value, and a far-side exit.  Filed as **DL-315**
(both defects, one row), not fixed here -- library code outside this
slice's law change.  (**Fixed on `debt-dl315`, 2026-09-28**: salted, the water rows now read
0.997-1.002 of the explicit volume (random walk within 0.02 %), and the
same stack write turned out to reach the legacy distribution-tracing /
final-gather ops -- see [DL315_RAYCASTER_STACK_AND_RECURSION.md](DL315_RAYCASTER_STACK_AND_RECURSION.md).
The open-air diffusion "0.9956" / "0.99568" figures in this section are
one unsalted Sobol' pattern; salted, diffusion in air reads about 1.000.)

**Four more suites hard-coded the Schlick law as their oracle** and were
moved to the exact law in this slice.  Three failed against the fix with
their old oracle, as they should: `RandomWalkSurvivalTest` (6), `RandomWalkFallbackProposalTest`
(7) and `RandomWalkDensityCutoffTest` (7) -- all run at IOR 1, where the
exact law's angular weight is exactly 1 while Schlick's
`(1-(1-mu)^5)/(20/21)` still "reflects" -- and `BSSRDFEntrySignalsTest`,
whose live-vs-neutral Sw check passed only by luck (0.3189 vs its stale
Schlick 0.3238, inside a 0.005 band) and whose "distinct from neutral" check
compared against the stale Schlick neutral; it now tests at `cos = 0.2`
(live 0.2469 vs neutral 0.3183) against an independent exact Fresnel and
quadrature.

**Gate** (clean library rebuild and every test target built: 0 warnings):
`SSSExteriorIndexInvarianceTest` 166/0, `BSSRDFNormalizationTest` pass,
`BSSRDFSamplingTest` pass, `SSSRadianceScalingTest` 576220/0,
`RandomWalkSurvivalTest` pass, `SubSurfaceExitIORTest` 301/0,
`BSSRDFOpenSheetEntryTest` 73/0, `BDPTZeroExitanceBSSRDFTest` 41/0,
`MatchedIndexGrazingConsumerTest` 60/0, `BDPTStrategyBalanceTest` 208/0,
`CstDeriveGoldenTest` 456 MATCH / 0 DRIFT, `SourceHygieneTest` 167/0, and the
touched-class suites `BSSRDFEntryPointTest`, `RandomWalkSSSTest`,
`BSSRDFProjectionNormalTest`, `RandomWalkFallbackProposalTest`,
`RandomWalkDensityCutoffTest`, `BSSRDFPlanarProbeReachTest`,
`BSSRDFEntrySignalsTest` 31/0, `SubsurfaceScatteringSpectralTest` 8/0,
`BlenderBridgeSSSTest`, `HairSSSEntryNormalTest`, `SPFBSDFConsistencyTest`,
`SPFPdfConsistencyTest`, `PathValueOpsTest`, `OptimalMISTrainingSitesTest`,
`BDPTVertexRIGRebuildTest` 68/0, `SSSBuildDeterminismTest`,
`TransmissionPushGateTest` 416/0, `SobolDimensionBudgetTest`,
`BDPTGuidedContinuationTest`, `SignalIntegratorConsistencyTest` 3048/0,
`EnvLightBalanceTest` 123/0.  (`RefractiveRadianceScalingTest`, 40/1 on
master from DL-308, was not touched and not re-run.)

## 6. In-air appearance (a documented change)

Every in-air SSS render moves slightly: Sw's angular shape is now the
exact law and `<T>` changes by `c_exact - c_Schlick` (+0.08 % at 1.33, -0.66 %
at 1.5, larger near grazing).  All twelve shipped scenes that bind
`subsurfacescattering_material` / `randomwalk_sss_material` (all at
index 1.3), each re-rendered under BOTH PT and BDPT (their own rasterizer
chunk replaced; half resolution, 64 spp, no OIDN, box filter;
`composite_wacky_creature` quarter resolution, 32 spp), base and fix
binaries INTERLEAVED, n = 6 per side, a distinct libc seed per render;
whole-image mean, `t` = Welch:

| Scene | PT fix/base | t | BDPT fix/base | t |
|---|---:|---:|---:|---:|
| pt_sss_wax_sphere | 1.00028 +/- 0.00004 | 7.1 | 1.00030 +/- 0.00003 | 11.0 |
| rwsss_sphere | 1.00021 +/- 0.00007 | 3.2 | 1.00035 +/- 0.00014 | 2.4 |
| rwsss_thin_slab | **0.99655** +/- 0.00014 | -24.2 | **0.99670** +/- 0.00018 | -18.4 |
| rwsss_bdpt | 1.00046 +/- 0.00006 | 7.4 | 1.00041 +/- 0.00007 | 5.6 |
| pt_sss_dragon | 1.00093 +/- 0.00057 | 1.7 | 1.00408 +/- 0.00311 | 1.3 |
| bdpt_sss_dragon | 0.99994 +/- 0.00038 | -0.2 | **1.00983** +/- 0.00281 | 3.5 |
| sss_comparison_dragon | 1.00054 +/- 0.00023 | 2.4 | 1.00013 +/- 0.00023 | 0.6 |
| bdpt_sss_different_bsdf | 1.00014 +/- 0.00014 | 1.0 | 1.00005 +/- 0.00021 | 0.3 |
| vcm_sss_dragon | 1.00131 +/- 0.00085 | 1.6 | **1.01076** +/- 0.00268 | 4.0 |
| furnace_sss_absorption | 1.00059 +/- 0.00002 | 29.7 | 1.00059 +/- 0.00004 | 14.9 |
| furnace_sss_zero_absorption | 1.00065 +/- 0.00002 | 34.5 | 1.00071 +/- 0.00005 | 14.5 |
| composite_wacky_creature | 0.99835 +/- 0.00161 | -1.0 | 1.00115 +/- 0.00163 | 0.7 |

Whole-image means include background, so these dilute the effect on the
SSS surfaces themselves.  Typical in-air movement is **+0.02 ... +0.07 %**;
the thin slab (grazing-dominated) **-0.35 %**.  The three dragon scenes
under BDPT move **+0.4 ... +1.1 %** while PT does not; on those scenes
BDPT sits below PT pre-fix (e.g. `bdpt_sss_dragon` 0.20915 vs 0.21418,
-2.3 %) and the fix closes about 40 % of that gap (0.21121 vs 0.21416,
-1.4 %).  No BDPT-only code path is involved: the dragons use the
diffusion model, and the only BDPT-only edits in this slice are the two
random-walk coins, which diffusion never reaches.  The +1.2 % BDPT move is
BDPT's own pre-existing inconsistency responding to shared code (the
profile's `FresnelTransmission` and the Sw normalization); with the
dragon's surface made SMOOTH, BDPT reads **7.9 % ABOVE** PT in both
builds -- see DL-333 (filed at merge).  Part of PT's side of that gap is
DL-315's recursion cap (+0.82 % on this scene).

## 7. Cost

`pt_sss_wax_sphere` (diffusion) and `rwsss_sphere` (random walk), full
resolution, 128 spp, PT, base and fix binaries interleaved, n = 4, user
CPU: **105.46 +/- 0.95 s -> 107.24 +/- 2.36 s (+1.7 %, paired t = 1.6)**
and **131.19 +/- 3.10 s -> 131.89 +/- 2.32 s (+0.5 %, t = 0.3)** -- not
significant.  The closed form costs two logs per normalization; the exact
Fresnel a square root and two divisions per evaluation.

## 8. Sibling audit (pattern: "one interface priced with two Fresnel laws")

| Site | Verdict |
|---|---|
| `SubSurfaceScatteringSPF` smooth reflection vs profile / RW transmission | **Fixed** (this row). |
| `RandomWalkSSS` internal exit coin | Exact `CalculateDielectricReflectance` both ways (stochastic) -- consistent before and after. |
| `SubSurfaceScatteringSPF` ROUGH reflection (`F(wi.m) G1`) vs the macro-normal `Ft` | Same exact law now; the rough lobe's directional albedo is below `F(mu)` by the single-scatter microfacet loss, so `R + T < 1` there by that loss -- a microfacet energy question, not a law mismatch.  Recorded, not filed. |
| `TranslucentSPF` `pRefFront` vs its transmission | No refractive index at all (painter weights) -- not applicable. |
| `PolishedSPF` coat `Rs` vs diffuse `(1 - Rs)` | Same exact law on both sides -- confirmed consistent. |
| `CoatedBRDF` coat reflection, `Tin`/`Tout`, Saunderson `r_e`/`r_i` | All `CoatedLayer::Fresnel` (exact) and its GL-quadrature `r_e` table -- confirmed consistent. |
| GGX family | DL-37's coupled model (one interface Fresnel `A` on both sides) -- confirmed. |
| `AshikminShirleyAnisotropicPhong` | Schlick specular with the model's own Schlick-derived diffuse coupling -- one law by construction. |
| `CookTorranceBRDF` | Additive diffuse, no transmission partition (conductor Fresnel's exterior is DL-290) -- not this pattern. |
| `BioSpecSkinSPF` boundary | Exact Fresnel coin both ways (outside index is DL-290) -- consistent. |
| Donner-Jensen multipole `Fdr`, legacy point-set shader-op | Precomputed boundary terms -- DL-291's scope. |

## 9. Residuals

- **DL-315** -- two PT defects (§5): `RayCaster::CastRay` mutating the
  caller's IOR stack's current object, which drops the SSS reflection of
  any SSS object inside an enclosure by about `F0`; and PT's hard-coded
  RayCaster recursion cap of 10 against SSS continuations nested at
  `depth + 2` (the water rows' ~3 %, and +0.82 % on the open-air
  `bdpt_sss_dragon` under PT).
- **DL-333** (filed at merge) -- with the dragon's surface smooth, BDPT
  reads 7.9 % above PT in both builds; pre-existing, not from this branch.
- **DL-334** (filed at merge) -- in the spectral path the SSS REFLECTION
  (`SubSurfaceScatteringSPF::ScatterNM`) uses the index at the wavelength
  while the TRANSMISSION uses the RGB value (`GetValuesAt(ri).v[0]`), so
  R + T != 1 per wavelength for a wavelength-varying index painter; the
  "R + T = 1 to rounding" result here is measured at uniform index only.
- The Donner-Jensen profile's dead `SchlickFresnel` helper -- delete after
  `debt-dl291` merges.
