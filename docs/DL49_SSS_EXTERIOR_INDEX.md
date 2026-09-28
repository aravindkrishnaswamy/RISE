# DL-49: the SSS boundary is an interface against the live exterior

Slice `debt-dl49`, branched from `master` `6b91fd19`, 2026-09-28.
Regression: `tests/SSSExteriorIndexInvarianceTest.cpp`.

## 1. The defect

An SSS body's boundary event is priced by two halves that must describe
one interface:

- `SubSurfaceScatteringSPF`'s surface reflection has always read the real
  exterior, `ior_stack.top()`, against the material index.
- Every other boundary quantity of the SAME event used the material's
  ABSOLUTE index against a hardcoded `1.0` (air):
  the diffusion profiles' `FresnelTransmission` and the Sw normalization
  built from `GetIOR`; `RandomWalkSSS::SampleExit`'s entry and exit Snell
  refraction, exit Fresnel and Sw factor; the PT/BDPT random-walk entry
  coin; the `BSSRDFEntryAdapters.h` NEE adapters (PT's entry NEE and
  BDPT's DL-207 zero-exitance sweep); `PathVertexEval`'s BSSRDF-entry
  re-evaluation (BDPT/VCM/MLT connections); and the rough
  `SubSurfaceScatteringBSDF`'s NEE lobe.

So an SSS body inside water, inside glass, or seen by a submerged camera
priced two different interfaces within one event, and a common scaling of
the exterior and interior indices -- which leaves every relative-index
quantity unchanged -- changed the image.

## 2. Ruling

**The exterior index of an SSS boundary event is
`RayIntersectionGeometric::ambientIOR`**, the incident-medium index that
PT, BDPT (hence VCM and MLT), the ray caster and the photon tracers already
stamp from `IORStack::top()` at every hit (the G6 plumbing GGX and
`coated_material` use), and that `PathVertexEval::PopulateRIGFromVertex`
replays from `BDPTVertex::mediumIOR`.  It is the same value the SPF's
reflection reads, so the two halves now agree.  A stackless caller (a
freshly constructed record) sees the field's default `1.0`: air, exactly as
before.  No new plumbing was needed; three records that were built fresh
now inherit the exit hit's exterior (the entry record inside
`SampleEntryPoint`, PT's two entry NEE records, and the four BDPT
`entryV` vertices' `mediumIOR`).

Every boundary quantity uses the RELATIVE index `eta = n_s / n_e`
(`BSSRDFSampling::RelativeBoundaryIOR`):

- **eta >= 1** (every in-air material, and any body denser than its
  surroundings): the Schlick law and its normalization
  `c = 20(1-F0)/21` are the pre-DL-49 ones with `eta` relative.  In air
  `n_s / 1.0 == n_s` exactly, so an in-air evaluation is **bit-identical**
  to the old code.
- **eta < 1** (a denser exterior -- the regime DL-49 makes reachable for
  the first time): Schlick's approximation is evaluated at the cosine on
  the RARER side (the transmitted cosine), and directions past the
  critical angle are totally reflected (`Ft = 0`).  The normalization is
  re-derived for that law: with `t` the transmitted cosine,
  `mu^2 = 1 - eta^2 (1 - t^2)`, so `mu dmu = eta^2 t dt` over `t in [0,1]`
  and `c = 2 * integral Ft(mu) mu dmu = eta^2 * 20(1-F0)/21`.  The two
  branches agree at `eta = 1`.  (`BSSRDFSampling::SchlickBoundaryCosine`,
  `SchlickTransmissionNormalization`.)

**DL-04's convention is untouched.**  Sw is still normalized to a unit
exterior cosine-hemisphere integral at every relative index (A2 below), so
the complete event carries no unmatched eta^2 at any exterior index.
`SSSRadianceScalingTest` reads 574014/3 before and after (the 3 are the
pre-existing DL-284 `explicit_dielectric_volume/air_no_enclosure` rows),
and both deliberate eta^2 mutations (multiply / divide the PT diffusion and
RW complete-event weights by the material index squared, re-executed on this
branch) still fail all six SSS air-furnace channels: 574008/9 each
(6 + the 3 pre-existing).

A useful structural fact for anyone testing this: for `eta >= 1` the
normalized `Sw(mu) = (1-F0)(1-(1-mu)^5) / ((20/21)(1-F0) pi)` does not
depend on the index at all.  So the adapters' and `PathVertexEval`'s Sw
are invariant under ANY index for `eta >= 1`, before and after the fix;
only the unnormalized factors (`FtExit`, the random-walk entry coin, the
Snell refraction, the exit Fresnel, the rough BSDF) carried the defect
there, and the `eta < 1` cone is the one place Sw itself moves.

## 3. Sites changed

| Site | Change |
|---|---|
| `BSSRDFSampling.h` | new `ExteriorIOR`, `RelativeBoundaryIOR`, `SchlickBoundaryCosine`, `RandomWalkSchlickTransmission`; `SchlickTransmissionNormalization` gains the `eta < 1` form |
| `ISubSurfaceDiffusionProfile.h` | contract: `FresnelTransmission` is relative to `ri.ambientIOR`; `GetIOR` stays the ABSOLUTE material index |
| `BurleyNormalizedDiffusionProfile::FresnelTransmission`, `DonnerJensenSkinDiffusionProfile::FresnelTransmission` | relative index, transmitted-cosine Schlick for `eta < 1` |
| `BSSRDFSampling::SampleEntryPoint` | entry record inherits the exit's `ambientIOR`; Sw normalization at the relative index |
| `RandomWalkSSS::SampleExit` (RGB and NM lanes) | entry refraction `n_e -> n_s`, exit refraction and exit Fresnel `n_s -> n_e`, Sw factor at the relative index |
| `BSSRDFEntryAdapters.h` | `BSSRDFEntryBSDF` normalizes at the relative index; `RandomWalkEntryBSDF` stores the absolute index and prices the relative one per evaluation record (covers PT NEE and BDPT's DL-207 sweep) |
| `PathTracingIntegrator.cpp` | random-walk entry Ft relative; both entry NEE records carry the exit's `ambientIOR` |
| `BDPTIntegrator.cpp` | eye and light random-walk entry coin relative (TIR aware, `R` kept bit-identical in air); all four BSSRDF `entryV` vertices carry the exit vertex's `mediumIOR` |
| `PathVertexEval.h` | BSSRDF-entry `EvalBSDFAtVertex{,NM}`: diffusion and random walk at the relative index against `mediumIOR` |
| `SubSurfaceScatteringBSDF::value{,NM}` | rough-lobe Fresnel against `ri.ambientIOR` instead of `1.0` |

HWSS needs nothing of its own: PT's HWSS path already falls back to the
per-wavelength NM integrator at any SSS vertex.  VCM and MLT reach every
BDPT site through the shared subpath generator and `PathVertexEval`.
No library source file was added or removed.

## 4. Red-proof and gate: `SSSExteriorIndexInvarianceTest`

Isolated A/B against committed state: the fix files were reverted with
`git checkout 6b91fd19 -- <the 12 fix files>` (the new test kept), rebuilt,
run, and restored with `git checkout HEAD -- <files>`.

**Part A, deterministic** (pre-fix failures / post-fix all pass):

| Check | pre-fix | what it pins |
|---|---|---|
| A1 profile Ft scale-invariant, matched index Ft(1) = 1, TIR past critical | 7 fail (worst 9.2 %, matched read 0.96, `1.33 in 1.5` Ft(0.3) read 0.815) | profile relative index + `eta < 1` law |
| A2 Sw unit integral at eta 0.887 / 1 / 1.33 / 0.6 | pass (consistency pin; the absolute law also integrates to 1) | the new normalization |
| A3 adapters scale-invariant; `1.33 in 1.5` Sw(0.3) = 0; equals its air twin | 2 fail (Sw(0.3) read 0.278) | adapters at `eta < 1` |
| A4 rough BSDF scale-invariant; matched index value 0 | 5 fail (worst relative diff 4.5x) | BSDF exterior |
| A5 `SampleEntryPoint` seeded twins identical | 2 fail (weights 9.2 % apart) | entry record + Sw |
| A6 `RandomWalkSSS::SampleExit` seeded twins identical | 2 fail (0/512 agree) | walk refraction/Fresnel |
| A7 matched index, vanishing medium: exit on the incident chord | 2 fail (15/64 straight) | no Snell bend, no exit Fresnel |
| A8 `PathVertexEval` entry vertex; honours `mediumIOR` at `eta < 1` | 2 fail | BDPT/VCM re-evaluation |

**Part B, rendered.**  Camera, a spherical area light and a unit SSS
sphere inside a black absorbing room; the "enclosed" twin wraps all of it in
an ideal non-reflecting `perfectrefractor_material` of index 1.5 (outside
the room, so no path ever reaches it; its only effect is the seeded
exterior, asserted per row via `IORStackSeeding::SeedFromPoint`) and scales
the interior index by 1.5.  An earlier draft without the black room read
**+46 %** on the Lambertian CONTROL under PT: the enclosure wall totally
reflects grazing escapes back in, adding real indirect light the air scene
lacks.  Pairs share one libc seed (common random numbers), so the printed
independent-sides sd is conservative; BDPT is deterministic per seed, which
is why its diffusion rows read exactly 1 after the fix.  n = 4 renders per
side (the gate default), final runs:

| Row | spp | pre-fix enclosed/air | post-fix | band |
|---|---:|---:|---:|---:|
| lambertian control / PT | 16 | 0.9975 +/- 0.0019 | 0.9975 +/- 0.0019 | 0.02 |
| diffusion smooth / PT | 64 | **0.9092** +/- 0.0019 | 0.9994 +/- 0.0015 | 0.02 |
| diffusion rough / PT | 64 | **0.9832** +/- 0.0016 | 0.9994 +/- 0.0011 | 0.01 |
| random walk / PT | 64 | **0.6453** +/- 0.0073 | 0.9907 +/- 0.0094 | 0.04 |
| diffusion smooth / BDPT | 32 | **0.8886** +/- 0.0103 | 1 (exact) | 0.02 |
| diffusion rough / BDPT | 32 | **0.9929** +/- 0.0055 | 1 (exact) | 0.006 |
| random walk / BDPT | 128 | **0.6196** +/- 0.0221 | 1.0027 +/- 0.0488 | 0.10 |
| diffusion smooth / PT spectral | 64 | **0.9054** +/- 0.0142 | 0.9950 +/- 0.0205 | 0.04 |
| random walk / PT spectral | 64 | **0.6561** +/- 0.0110 | 0.9935 +/- 0.0207 | 0.05 |
| diffusion smooth, eta < 1 / PT | 256 | **0.9837** +/- 0.0007 | 1.0005 +/- 0.0010 | 0.008 |
| diffusion smooth, eta < 1 / BDPT | 32 | **0.9839** +/- 0.0078 | 1 (exact) | 0.005 |
| random walk, eta < 1 / PT | 64 | **0.8999** +/- 0.0037 | 0.9953 +/- 0.0096 | 0.04 |

(`eta < 1` rows: 1.33 inside 1.5 versus 1.33/1.5 in air.)  Totals: pre-fix
**106/33** (every render row except the control fails), post-fix
**139/0**.  An n = 8 run of each side reproduced these to within the
quoted sd.

Two design notes the numbers explain:

- **The rough row needs its own row and a tighter band.**  Pre-fix, the
  rough BSDF's air Fresnel OVER-reflects in the enclosure while the
  air-index profile UNDER-transmits, so the two errors partially cancel in
  a whole-image mean (0.983 against the smooth row's 0.909).  This is the
  ledger recipe's warning ("observer-interface changes cannot cancel the
  defect") in measured form; the rough lobe's own defect is pinned exactly
  by A4.
- **The `eta < 1` BDPT row is what pins `entryV.mediumIOR`.**  A targeted
  mutation that removed only the four `entryV.mediumIOR = ...` lines read
  0.9756 on that row (fails its 0.005 band) while every other row passed:
  for `eta >= 1` the re-evaluated Sw does not depend on the index, so
  nothing else can see a missing vertex exterior.

## 5. Other gate suites

`SSSRadianceScalingTest` 574014/3 before and after (the 3 are DL-284).
Its WATER rows are the first ones in the tree with an SSS body in a non-air
medium, and they moved toward the explicit-volume reference:

| Row | explicit volume | diffusion pre -> post | random walk pre -> post |
|---|---:|---:|---:|
| water, camera outside (conservative furnace, ideal 1) | 0.9800 | 0.9333 -> **0.9679** | 0.9332 -> **0.9684** |
| water inside / air | 1.8056 | 1.6577 -> **1.7225** | 1.6492 -> **1.7130** |

The observer ratio-of-ratios stayed inside its 10 % wiring band
(diffusion 0.9998 -> 1.0017, random walk 0.9992 -> 1.0010).

All green on a clean rebuild: `BSSRDFNormalizationTest`,
`RandomWalkSurvivalTest`, `SubSurfaceExitIORTest` 301/0,
`BSSRDFProjectionNormalTest`, `RandomWalkFallbackProposalTest`,
`RandomWalkDensityCutoffTest`, `BSSRDFPlanarProbeReachTest`,
`BSSRDFOpenSheetEntryTest` 73/0, `BSSRDFEntrySignalsTest` 31/0,
`BDPTZeroExitanceBSSRDFTest` 41/0, `EnvLightBalanceTest` 123/0 (topology J
included), `BDPTStrategyBalanceTest` 170/0, `CstDeriveGoldenTest` 454 MATCH
/ 0 DRIFT, `SourceHygieneTest` 167/0, plus the touched-class suites
`BSSRDFSamplingTest`, `BSSRDFEntryPointTest`, `HairSSSEntryNormalTest`,
`MatchedIndexGrazingConsumerTest` 60/0, `OptimalMISTrainingSitesTest`,
`RandomWalkSSSTest`, `SPFBSDFConsistencyTest`,
`SubsurfaceScatteringSpectralTest` 8/0, `BlenderBridgeSSSTest`,
`SSSBuildDeterminismTest`, `BDPTVertexRIGRebuildTest`, `PathValueOpsTest`.
Library and every test target built: 0 warnings on a clean rebuild.

## 6. Shipped scenes

All twelve shipped scenes binding `subsurfacescattering_material` or
`randomwalk_sss_material` render in air: none contains a refracting
material, and an instrumented build (never committed) counting every SSS
boundary exterior read found **0 non-air reads out of ~48 million** across
reduced-spp renders of all twelve.  With the exterior exactly 1.0 every
changed expression computes the pre-DL-49 value bit-for-bit, so these scenes
are unchanged by construction.  Renders are not deterministic run to run
(the PT pixel order is shuffled), so the image check is statistical: base
and fix binaries, interleaved, n = 3 per side at reduced spp -- every
fix/base mean ratio within noise (0.9993 ... 1.0023, |t| <= 1.6), except
`rwsss_thin_slab` at t = -3.0 which re-measured at n = 8 as 0.99997
(t = -0.07); `vcm_sss_dragon`'s per-render sd is ~28 % (VCM fireflies at 4
spp) and its 0.87 ratio is t = -0.8.

## 7. Cost

Interleaved base/fix, n = 6, user CPU at 128 spp: `pt_sss_wax_sphere`
55.45 +/- 1.22 s -> 55.59 +/- 0.82 s (+0.26 %, paired t = 0.22);
`rwsss_sphere` 71.05 +/- 2.29 s -> 69.09 +/- 0.90 s (-2.8 %, t = -1.8).
No measurable cost (one division and one branch per boundary evaluation).

## 8. Sibling audit (pattern: "an absolute index against 1 where a relative index is meant")

| Site | Verdict |
|---|---|
| `SubSurfaceScatteringSPF` (front reflection, DL-51 exit) | Relative via the IOR stack -- the reference side of this row. |
| `DielectricSPF`, `PolishedSPF`, `PerfectRefractorSPF` | Relative via `ior_stack.top()` -- confirmed. |
| `TranslucentSPF` / `TranslucentBSDF` | No refractive index at all (painter weights; `containsCurrent()` is only a side flag; entry re-pushes the stack top) -- not applicable. |
| `GGXBRDF`/`GGXSPF` conductor and thin-film modes, `CoatedBRDF` | Relative via `ri.ambientIOR` (G6) -- confirmed. |
| `CookTorranceBRDF` / `CookTorranceSPF` conductor Fresnel and Kulla-Conty `F_avg` | **Pattern present**: incident index hardcoded `1` / `RISEPel(1,1,1)`; G6 was never propagated here.  Filed **DL-290**. |
| `HairBSDF` (Chiang) | **Pattern present by construction**: `eta` is the fibre's index against the surrounding medium in the model, read as an absolute painter value and never divided by the exterior -- correct in air, wrong for immersed hair.  Filed under **DL-290**. |
| `BioSpecSkinSPF` | **Pattern present**: the outside/stratum-corneum boundary passes a literal `1.0` for the outside index on entry and exit.  Filed under **DL-290**. |
| `GenericHumanTissueSPF` | No Fresnel or Snell (straight-through / cosine perturbation) -- not applicable. |
| `DonnerJensenSkinDiffusionProfile` multipole `Rd`, legacy `DiffusionApproximationExtinction` | Boundary Fresnel/Sw fixed here, but the multipole's own boundary condition `Fdr(ior_epidermis)` is baked at construction against air (a dummy record), and the legacy shader-op's `relative_ior` is an authored constant.  Filed **DL-291**. |
| `BurleyNormalizedDiffusionProfile` `Rd` | Empirical fit with no boundary term -- nothing to make relative (model limit, by design). |

Also noted, not filed: the SPF's surface reflection uses the EXACT
dielectric Fresnel while the profile transmission uses Schlick, so
`R + Ft != 1` by a few percent even in air.  That is a pre-existing model
approximation, independent of DL-49 (it exists at `n_e = 1`).

## 9. Residuals

- **DL-290** -- the same pattern outside SSS: Cook-Torrance conductor
  Fresnel, Chiang hair `eta`, BioSpec skin outside index.
- **DL-291** -- precomputed / authored SSS boundary conditions: the
  Donner-Jensen multipole `Rd` and the legacy point-set shader-op.

## 10. DL-290: the same pattern outside SSS (slice `debt-dl290`, 2026-09-28)

Branched from `master` `c190163c`.  Regression:
`tests/ExteriorIndexInvarianceTest.cpp` (the SSS-named suite above is
unchanged, 139/0).

### 10.1 Ruling

The same one as §2, applied to every remaining boundary model that priced
an interface against a literal air: **the exterior index is
`RayIntersectionGeometric::ambientIOR`** (the G6 IOR-stack top; 1.0 for a
stackless record), and every boundary quantity is a function of the
RELATIVE index.  In air `x / 1.0 == x` and `RISEPel(1.0) == RISEPel(1)`,
so every changed expression computes its pre-fix value bit for bit.

| Site | Ruling | Change |
|---|---|---|
| `CookTorranceBRDF` `value`/`valueNM`/`albedo`, `CookTorranceSPF` specular and multiscatter `kray` (RGB, NM), Kulla-Conty `F_avg` | **fixed** | incident index `CookTorranceBRDF::AmbientIOR(ri)` (was `1` / `RISEPel(1,1,1)`); the complex conductor Fresnel is a function of `(n/n_e, k/n_e)` only.  The aggregate `Pdf` selects lobes by specular COLOUR, not Fresnel, so no replay moved (pinned: Pdf twins equal). |
| `HairBSDF` (Chiang): `Resolve`'s `etaRef`, `EvalFsum`'s per-wavelength eta, the albedo AOV's `fAvg` | **fixed** | `FibreLobeMath::RelativeFibreEta(authored, ambientIOR)`.  Chiang/Marschner (and PBRT-v4's HairBxDF) write every Snell and Fresnel term of the fibre surface in the fibre-vs-SURROUNDING ratio; the colour fit (C <-> sigma_a) and the beta remaps do not involve eta, so the model accepts a relative index without changing anything else, and wet / submerged hair now prices the medium it is in.  **Documented limitation:** a fibre less dense than its surroundings (relative < 1) is outside the model (gamma_t = asin(h/eta') needs eta' >= 1); it is clamped to exactly 1, an index-MATCHED fibre (`FrDielectric` returns 0, straight transmission).  The authored-index guard (`<= 1 -> 1.55`) still runs first, so in air the clamp never binds. |
| `WeaveBRDF` thread lobes (found by the sibling audit: same `FrDielectric(eta)` against air) | **fixed** | the clamped authored index is divided by the exterior and re-clamped to `[kMinIOR, kMaxIOR]` (a no-op in air). |
| `BioSpecSkinSPF` outside / stratum-corneum boundary, entry and exit | **fixed** | `SkinParams::ior_outside = OutsideIOR(ri)` replaces the two literal `1.0`s.  `Boundary_Refraction` is the EXACT dielectric Fresnel plus Snell with TIR on both indices, so it is already a function of the ratio and already correct for a denser exterior -- none of DL-49's Schlick-at-the-transmitted-cosine re-derivation is needed here. |
| `GenericHumanTissueSPF` | **refuted** | no Fresnel, no Snell, no index (straight-through / cosine perturbation), as §8 said. |
| SMS evaluation rigs (`ManifoldSolver.cpp`): `ComputeTrialContribution{,NM}` and the snell main loops of `EvaluateAtShadingPoint{,NM}` -- FOUR rigs, not the three the row named (the NM helper was missed) | **fixed** | each rig stamps `ambientIOR` from the receiver's stack top (`SMSReceiverAmbientIOR`).  At the same sites: the two NM rigs called `valueNM` while their RGB twins call `valueStateful(.., pIorStack)` -- now `valueStatefulNM`; and the uniform-mode dispatch DROPPED the stack (`EvaluateAtShadingPoint{,NM}Uniform` were called without it, and so were all six helper calls inside them), so uniform mode had no stack at all -- now forwarded. |
| SMS seed walk (`BuildSeedChain` / `BuildSeedChainBranching`; found while red-proving the rigs) | **fixed** | the walk that produces every chain vertex's `etaI`/`etaT` (Newton's constraint, the chain Fresnel) started at `IORStack(1.0)`; it now starts from the receiver's live stack when one is given (all 14 snell / uniform / NM call sites pass it; null = air as before). |
| SMS photon-seeded chains (`ReversePhotonChainForSeed` and the three photon-aided sites) | **residual, not fixed** | photon-reversed chains carry no `etaI`/`etaT` and take `GetEffectiveEtas`' "air on the other side" back-compat path; fixing it means walking a stack along the reversed chain (or storing the etas in `SMSPhotonChainVertex`) and changes nested-dielectric photon-aided scenes in air as well.  Measured and pinned by a NON-gated row (below). |

### 10.2 Red-proof and gate

Isolated A/B against committed state: every library file the slice
touches was reverted with `git checkout c190163c -- <10 files>` (the test
kept), rebuilt, run, and restored with `git checkout HEAD -- <files>`.
**Pre-fix 123/44** (24 deterministic unit checks + 20 render rows),
**post-fix 167/0**.

Part A (deterministic): Cook-Torrance BSDF twins worst relative difference
0.467 -> 1.9e-16; seeded SPF twins 477/2048 -> 2048/2048 (RGB and NM);
matched index (1.5 inside 1.5, k = 0) max value 0.2200 -> 2.4e-17 and max
SPF kray 0.367 -> 4.0e-17.  Hair BSDF twins 0.892 -> 0; SPF twins 0/2048 ->
2048/2048; an opaque fibre at matched index max value 0.1925 -> 0.  Weave
0.351 -> 2.2e-16; 0/2048 -> 2048/2048.  BioSpec seeded SPF twins (every
skin index scaled, dermis thick enough that the collagen Rayleigh term
saturates in both twins -- see the test's note) 4268/8192 -> 8192/8192, and
with the outer interface index MATCHED 2030/8192 -> 8192/8192.  SMS
`ComputeTrialContribution{,NM}` with a recording BSDF: receiver
`ambientIOR` 1 -> the stack top (1.33, 1.5; air with no stack) and the NM
helper's stateful calls 0 -> 1.

Part B, rendered (DL-49's black-room harness; the enclosure is now present
on BOTH sides, at index 1 in the air scene, because SMS uniform seeding
enumerates every specular caster and a one-sided enclosure read 0.988 on
the uniform rows under common random numbers).  n = 4 renders per side,
enclosed/air mean ratio +/- the (conservative) independent-sides sd:

| Row | spp | pre-fix | post-fix | band |
|---|---:|---:|---:|---:|
| lambertian control / PT | 16 | 1.0009 | 1.0009 +/- 0.0042 | 0.02 |
| Cook-Torrance / PT | 32 | **1.2487** | 1.0000 +/- 0.0008 | 0.02 |
| Cook-Torrance / BDPT | 16 | **1.2405** | 1.0034 +/- 0.0035 | 0.02 |
| Cook-Torrance / PT spectral | 32 | **1.2538** | 1.0000 +/- 0.0082 | 0.03 |
| Cook-Torrance / PT HWSS | 32 | **1.2466** | 1.0000 +/- 0.0086 | 0.03 |
| hair / PT | 64 | **3.2399** | 0.9955 +/- 0.0079 | 0.03 |
| hair / BDPT | 16 | **3.2164** | 1.0000 +/- 0.0114 | 0.03 |
| hair / PT HWSS | 256 | **3.1088** | 0.9996 +/- 0.0084 | 0.03 |
| hair / BDPT HWSS | 64 | **3.2088** | 0.9902 +/- 0.0110 | 0.03 |
| weave / PT | 32 | **1.0395** | 1.0000 +/- 0.0003 | 0.03 |
| weave / BDPT | 16 | **1.0371** | 1.0000 +/- 0.0012 | 0.03 |
| BioSpec skin / PT | 256 | **1.0430** | 1.0014 +/- 0.0037 | 0.015 |
| BioSpec skin / BDPT | 512 | **1.0296** | 1.0001 +/- 0.0033 | 0.02 |
| SMS GGX conductor via mirror / PT snell | 16 | **2.4468** | 1.0000 +/- 0.0005 | 0.03 |
| same / PT uniform | 16 | **2.4464** | 1.0000 +/- 0.0005 | 0.03 |
| same / PT spectral snell | 16 | **2.4485** | 0.9977 +/- 0.0082 | 0.05 |
| same / PT spectral uniform | 16 | **2.4446** | 1.0000 +/- 0.0122 | 0.05 |
| SMS Lambertian via glass sphere, k = 1 / PT snell | 16 | **0.9530** | 1.0000 +/- 0.0022 | 0.01 |
| same, k = 1 / PT uniform | 16 | **0.9862** | 1.0000 +/- 0.0010 | 0.01 |
| same, k = 1 / PT spectral snell | 64 | **0.9325** | 1.0000 +/- 0.0034 | 0.03 |
| same, k = 1 / PT spectral uniform | 64 | 0.9877 | 0.9992 +/- 0.0030 | 0.03 |
| same, k = 2 / PT uniform | 64 | **1.0720** | 0.9987 +/- 0.0012 | 0.02 |
| same, k = 2 / PT snell + photons (NOT gated) | 16 | 0.9673 | **1.0208** +/- 0.0074 | -- |

The spectral-uniform k = 1 glass row passes pre-fix too (the NM path
re-derives the caster side of each vertex per wavelength, so only the
surrounding side was wrong and the whole-image effect is 1.2 %): it is a
consistency pin, the other four glass rows are the red-proof of the seed
walk.  The SMS mirror rows isolate the rigs (a delta light through a delta
mirror reaches the GGX floor only through SMS; the light sits in a
black open-topped box).  The BioSpec rows use a large emitter: that material
has no evaluable BSDF, so only BSDF-sampled continuations find the light.
The photon row's residual is +2.1 % (an n = 10 run read +2.0 % +/- 0.3 %).

Other gates (all built and run from a clean rebuild; library 374 TUs and
36 test targets, 0 warnings): `SSSExteriorIndexInvarianceTest` 139/0,
`SSSRadianceScalingTest` 576220/0, `CookTorranceMultiscatterTest` 17/0,
`CookTorranceSchlickGlossyFilterConsistencyTest` all pass,
`CookTorranceHWSSTest` 7/0, `SPFPdfConsistencyTest` pass,
`SPFBSDFConsistencyTest` pass, `SchlickKrayBRDFConsistencyTest` 1449/0,
`GGXDiffuseTransmissionTest` pass, `HWSSCompanionKrayTest` 189/0,
`HairBSDFTest` 0 failed, `HairRenderTest` 29/0, `HairDirectionalBacklitTest`
7/0, `HairMaterialChunkTest` 44/0, `BlenderBridgeHairTest` pass,
`HairSSSEntryNormalTest` 0 failures, `FibreLobeMathTest` 39/39,
`WeaveMaterialChunkTest` 296/0, `FabricMaterialChunkTest` 178/0,
`FabricRenderTest` 60/0, `MirroredUVWeaveDirectionTest` 24/0,
`TextureFootprintTest` 192/0, `GenericHumanTissueInteriorScatterTest` 9/0,
`ManifoldSolverTest` pass, `ManifoldNormalDerivativeTest` 141/0,
`GrazingSnellFresnelTest` 38/0, `GrazingFresnelThroughputTest` 44/0,
`LayeredWhiteFurnaceTest` 0/58, `BDPTStrategyBalanceTest` 208/0,
`VCMStrategyBalanceTest` 74/0, `MLTSpectralHWSSNormalizationTest` 8/0,
`SchlickWardBackfaceEnergyTest` pass, `CstDeriveGoldenTest` 454 MATCH /
0 DRIFT, `SourceHygieneTest` 167/0; `RefractiveRadianceScalingTest` 40/1,
the pre-existing row C of DL-308.

### 10.3 Shipped scenes: in-air bit identity

A temporary harness (never committed; `std::srand(seed)`,
`force_number_of_threads 1`, FNV-1a over the float RGBA image) rendered
every shipped scene binding `cooktorrance_material`, `hair_material`,
`weave_material` or `biospec_skin_material` (native rasterizer, and swapped
to PT, BDPT and PT spectral) plus every `sms_enabled` scene (native), cut to
48 px wide at <= 4 spp without OIDN: 134 variants, seeds 42 and 1234, base
`c190163c` versus the final build.  Serial re-runs are deterministic.

- **129/134 bit-identical at both seeds.**  (15 of those are canonical
  variants that render black because swapping the rasterizer chunk drops the
  scene's environment; the chunk-coverage `cc_hair_*` files have no camera
  and are not counted.)
- **5 differ, all SMS, and all have non-air receivers by the renderer's own
  stack**: `sms_k1_botonly`, `sms_k1_refract`, `sms_k2_flatslab`,
  `sms_k2_glassblock`, `sms_teapot_close_sms`.  An instrumented build
  (never committed) counting seed walks whose receiver stack top is not 1.0
  read 411-451 of ~15 000 walks on the first four and 228 of 16 260 on the
  teapot, and **0** on every one of the 15 matching SMS scenes that run a
  seed walk at all (two more, a VCM scene and one whose SMS never seeds, make
  none) --
  the change is confined to receivers the IOR stack places inside glass
  (camera paths through an open refracting sheet, or into a block resting on
  the floor), where the walk now exits the caster instead of entering it.
  Whole-image means moved -1.0 % .. +1.3 % at 4 spp.
- An earlier parallel (8-process) run showed `fabric_presets` (pixelpel) and
  `glass_pavilion` differing; serial re-runs of both binaries were 8/8 and
  6/6 identical -- load-induced nondeterminism of that rasterizer, not the
  fix.
- `spectral_skin_fast` / `spectral_skinmodel` read NaN under BDPT in BOTH
  builds -- not BioSpec: an `infiniteplane_geometry` luminaire with
  `scale` above 1 turns the whole image NaN under PT and BDPT (filed
  **DL-311**).

### 10.4 Cost

Interleaved base / final, n = 8 (two batches in opposite order), user CPU:
`cornellbox_bdpt_materials_pt` (Cook-Torrance) 9.002 -> 9.053 s (+0.6 %,
paired t = 1.2); `hair_styled` 22.91 -> 22.98 s (+0.3 %, t = 1.0);
`sms_k2_glasssphere` 2.001 -> 2.031 s (+1.5 %, t = 1.9).  Not
distinguishable from noise.

### 10.5 Sibling audit (pattern: "an absolute index against 1 where a relative index is meant")

| Site | Verdict |
|---|---|
| GGX conductor / thin-film, `CoatedBRDF` | relative via `ambientIOR` (G6) -- confirmed |
| `DielectricSPF`, `PolishedSPF`, `PerfectRefractorSPF`, SSS SPF, `RayCaster` shadow transmittance | relative via the IOR stack -- confirmed |
| `TranslucentSPF`, `GenericHumanTissueSPF`, `FabricBRDF`/`SheenBRDF` (Charlie sheen), `DataDrivenBSDF` | no index / no Fresnel -- not applicable |
| `WeaveBRDF` | **pattern present -- fixed here** |
| SMS seed walk | **pattern present -- fixed here**; photon-seeded chains remain (above) |
| Schlick / Ward / Phong / Ashikhmin, GGX `schlick_f0`, `pbr_metallic_roughness` (F0 = 0.04) | an authored normal-incidence REFLECTANCE, not an index: nothing to make relative (the model's F0 is an in-air datum by construction) |
| glTF `KHR_materials_ior` | imported as `dielectric_material` (stack-relative) -- confirmed |
| BioSpec dermis Rayleigh term (`ComputeBeta`: collagen 1.5 against the dermis index) | an INTRA-tissue contrast with a hardcoded collagen constant, not an exterior interface -- not this pattern (it is why the invariance fixture needs a saturating dermis) |
| `BioSpecSkinSPF::Epidermis_Outside_Boundary_Refraction` | declared, never called |
| Donner-Jensen multipole `Rd`, legacy point-set SSS | DL-291 (unchanged) |

Found while measuring, genuinely separate, filed:
**DL-311** (infinite-plane luminaire with `scale` > 1 -> NaN image under
PT/BDPT; `averageRadiantExitance * GetArea()` with `GetArea() ==
RISE_INFINITY` overflows the light power) and **DL-312** (the NM snell SMS
path lacks the RGB path's pure-mirror caster supplemental seeds: on the
mirror fixture the spectral snell row renders 0.021 against RGB snell 0.046
and both uniform rows 0.041-0.042).
