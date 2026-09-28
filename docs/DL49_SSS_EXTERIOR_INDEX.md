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
approximation, independent of DL-49 (it exists at `n_e = 1`).  (Filed at merge as DL-306 and fixed on `debt-dl306`, 2026-09-28: the
transmission now uses the exact law too -- see
[DL306_SSS_FRESNEL_PARTITION.md](DL306_SSS_FRESNEL_PARTITION.md).  The
`eta < 1` Schlick branch and its `eta^2 * 20(1-F0)/21` normalization
described in §2 were replaced by the exact law's closed-form normalization,
which obeys the same `c(eta) = eta^2 c(1/eta)` identity.)

## 9. Residuals

- **DL-290** -- the same pattern outside SSS: Cook-Torrance conductor
  Fresnel, Chiang hair `eta`, BioSpec skin outside index.
- **DL-291** -- precomputed / authored SSS boundary conditions: the
  Donner-Jensen multipole `Rd` and the legacy point-set shader-op.

## 10. DL-291: the precomputed / authored boundary conditions (2026-09-28)

Slice `debt-dl291`, branched from `master` `7bbb434f`, two rounds (the
second answers an external review: two P1s, three P2s).  Regression:
`tests/SSSExteriorIndexInvarianceTest.cpp` Part D and seven new Part B rows
(Part C is DL-306's furnace, merged from `master` `d694244e`).

### 10.1 The defect

DL-49 made the boundary Fresnel and Sw relative to the live exterior, but
the diffusion solution's OWN boundary condition -- the extrapolation term
`A = (1 + Fdr) / (1 - Fdr)` that sets the virtual-source depth
`z_v = z_r + 4 A D` -- is a function of the relative index too, and three
models baked it against air:

- `DonnerJensenSkinDiffusionProfile` (`donner_jensen_skin_bssrdf_material`,
  every integrator) fits its Sum-of-Gaussians once, at construction, from a
  two-layer multipole whose per-slab `A` came from
  `ComputeFdr( layer.ior )` -- the ABSOLUTE index against 1.
- `DonnerJensenSkinSSSShaderOp` (`donner_jensen_skin_sss_shaderop`,
  `pixelpel_rasterizer`) tabulates the same multipole (plus a 480-point LUT
  when offset painters are bound) the same way.
- `DiffusionApproximationExtinction` (`diffusion_approximation_sss_shaderop`,
  `pixelpel_rasterizer`) computes the Jensen dipole's `A` from its authored
  `ior`, whose chunk description is "Index of refraction" -- the material's
  index, i.e. against air.

**Which multipole faces move, and why that is not all physics.**  The
inter-layer coupling (`ComputeCompositeProfileHankel`'s `Ft_down`/`Ft_up`)
uses the RATIO of adjacent layer indices -- internal, already relative,
unchanged.  But RISE's multipole gives each slab ONE symmetric `A` for both
of its faces, evaluated against the medium around the stack, so
re-expressing each layer's index relative to the exterior moves the
INTERNAL faces (epidermis bottom, dermis top) as well as the outer ones.
That is the only reading under which the whole multipole is a function of
relative indices (and it is exactly a no-op in air), but the internal-face
part of the move is an artifact of the symmetric-slab model, which is
**DL-313**.  The external review measured how much: with each face
extrapolated at its own relative index (an asymmetric multipole that
reproduces the shipped composite to 6 digits), total diffuse reflectance
at the chunk defaults is

| nm | shipped, air | per-face, air | shipped, water 1.33 | per-face, water | shipped, glass 1.5 | per-face, glass |
|---:|---:|---:|---:|---:|---:|---:|
| 615 | 0.3062 | 0.3958 | 0.3857 | 0.3957 | 0.3937 | 0.3957 |
| 550 | 0.1883 | 0.1951 | 0.2196 | 0.2202 | 0.2271 | 0.2279 |
| 465 | 0.1527 | 0.1549 | 0.1820 | 0.1823 | 0.1910 | 0.1912 |

(reproduced on this branch by re-running the review's harness).  At
615 nm the epidermis is thinner than the multipole's thin-layer threshold
(`d = 0.025 < z_r = 0.0287`), so its `A` is unused, and the only term
DL-291 moves in red is the DERMIS `A`, whose extrapolated face is the
internal interface: **DL-291's +26 % red change air -> water is entirely
the DL-313 artifact** (the per-face model moves by -0.01 %).  Green and
blue move mostly for real (per-face +12.8 % / +17.7 %, shipped +16.7 % /
+19.2 %).  Net, DL-291 still brings the immersed body closer to the
per-face answer in every channel (red error -22.6 % in air-baked tables vs
-2.5 % after), but for the wrong reason in red.  Consequently the scale-
invariance rows below prove that the exterior is plumbed as a RELATIVE
index -- a property the per-face model shares -- and say nothing about
whether the multipole's exterior dependence is physically right; that is
DL-313's to fix and measure.

### 10.2 Fix

| Site | Change |
|---|---|
| `MultipoleDiffusion::RelativizeLayersToExterior` (new) | divides each layer index by the exterior and recomputes the derived parameters; exactly a no-op at `n_e == 1` |
| `DonnerJensenSkinDiffusionProfile` | tables grouped as `ProfileTables`; the constructor builds air (`m_air`, the pre-DL-291 tables bit for bit); `TablesFor( ri )` (inline, one compare in air) serves every other exterior from a per-exterior cache; `EvaluateProfile{,NM}`, `SampleRadius`, `PdfRadius` read it; the fit's active set moves with the exterior (a 1.2 body at default melanin: widest active Gaussian 2.8x the air variance in water), so the entry-point cutoff follows it via the new `ISubSurfaceDiffusionProfile::GetMaximumDistanceForErrorAt( error, ri )` (default: the exterior-independent value; `BSSRDFSampling::SampleEntryPoint`'s probe length and cutoff call it) |
| `DonnerJensenSkinSSSShaderOp` | uniform table and (when offset painters exist) the LUT built per exterior on first use; `PerformOperation` evaluates against `ri.geometric.ambientIOR` |
| `DiffusionApproximationExtinction` | new `ComputeTotalExtinctionForExterior` evaluates the dipole at `A( ior / n_e )`; `BoundaryA` keeps the class's own Egan-Hilgeman fit for a relative index >= 1 and uses the separate eta < 1 fit (`ComputeFdr`) below 1 -- review P1-1: the >= 1 polynomial goes NEGATIVE there (-0.051 at 1.3 inside water, -0.375 inside 1.5 glass, against the Fresnel integral's 0.0069 / 0.0359), which put `A` at 0.904 / 0.455 instead of 1.014 / 1.074 and read the dipole's reflectance 1.7-2.7 % high in water and 10-17 % high in glass.  The two fits meet at 1 to 1e-4 in Fdr.  An `ior` AUTHORED below 1 in air moves too (10.4). |
| `ISubSurfaceExtinctionFunction` | new defaulted `ComputeTotalExtinctionForExterior( distance, exteriorIOR )` (default ignores the exterior: `SimpleExtinction` has no boundary term) |
| `PointSetOctree::Evaluate` | takes the exterior; the air loops are the original code verbatim and the non-air loops call the new virtual; **also forwards the IOR stack into the recursion** -- DL-223 plumbed `const IORStack*` through the octree, but the child-node call dropped it, so every node below the root priced a stateful BSDF (`translucent_material` under `multiplybsdf TRUE`) stacklessly |
| `SubSurfaceScatteringShaderOp` | passes `ExteriorIOR( ri.geometric )` |
| `ExteriorIndexCache.h` (new, header-only) | lock-free-read cache of per-exterior tables, exact key, 32 entries, builds serialized by a mutex while there is room; once full, a miss is served by a LOCK-FREE nearest-key scan with a one-shot warning (review P2-1, below) |

**Cost of an exterior.**  A multipole fit costs ~5-7 ms per distinct
exterior (34 wavelength fits); the legacy skin op's uniform table is of
the same order; with offset painters its LUT is an 11.8 MB, seconds-long
build per exterior (up to 377 MB at 32 entries -- reachable only with more
than three exteriors, which the RGB `pixelpel_rasterizer` that op runs
under does not produce).  A scene with a handful of media builds a handful
of tables.  A DISPERSIVE enclosure under HWSS or MLT spectral presents a
new exterior on nearly every hit: the first 32 are built, every later one
reuses the nearest cached table (bounded by the key spacing: for BK7,
~0.0226/32 in index (1.5337-1.5112 over 380-780 nm), `dA ~ 1e-3`).  The first round took the build mutex on
every such miss; the review measured the lock alone at 2.4x wall clock.
Re-measured after the lock-free fix (skin sphere inside a BK7 Sellmeier
sphere, `pathtracing_spectral_rasterizer hwss TRUE`, 64x64, 256 spp,
default threads, the CLI, interleaved, n = 3):

| Build | Scene | Wall | User | Sys |
|---|---|---|---|---|
| round 1 (`7fcecac7`) | dispersive | 22.45 / 22.31 / 23.01 s | 34.8-35.9 s | 34.9-36.9 s |
| round 2 | dispersive | 8.46 / 7.89 / 7.84 s | 30.1-30.5 s | 0.14-0.20 s |
| round 2 | same scene, constant 1.5168 | 5.67 / 6.16 / 6.09 s | 22.0-23.9 s | 0.12-0.14 s |

The remaining gap to the constant-index control is real dispersive
transport plus 32 builds, not locking (system time 0.2 s).

### 10.3 Red-proof and gate

Every A/B was an isolated revert against committed state (WIP committed
first; restored with `git checkout HEAD -- src tests`).

**Against the pre-DL-291 code** (the 13 fix files at `7bbb434f`, the test
at its red-proof revision `e52c0781` with the final row settings added):
**170/14** -- the six Part D checks that compile there, the seven DL-291
render rows below, and one DL-49 row (`diffusion_smooth_dense/BDPT` read
1.0062 against its 0.005 band: see 10.7).

**Against round 1** (`DiffusionApproximationExtinction.h` alone at
`7fcecac7`): the new D3 Fresnel-integral checks read **102/3** (dipole `A`
0.9035 / 0.4547 / 0.0539 against the integral's 1.0139 / 1.0744 / 1.1424
at relative index 0.977 / 0.867 / 0.75).  The rendered below-1 dipole row
reads exactly 1 there: the invariant is BLIND to a wrong-but-consistent
fit (both sides evaluate the same function), which is why the defect
survived round 1 and why D3 now checks the fit against an independent
quadrature of the unpolarized Fresnel equations rather than against a
twin.  Its image mean shows the size of the defect: the 1.3-in-1.5 air
twin read 0.00763 in round 1 and 0.00674 now (+13 % over).

Part D, deterministic (final): multipole twins at a common scale 1.3e-11,
matched and eta < 1 twins exact (the matched case -- 1.4 in 1.4 against
the relative-1 body in air -- is the boundary constant at relative index
1 equalling the air value, bit for bit); same body water vs air 0.58 apart
(pre-DL-291: 0, the profile ignored the exterior); octree 2000/0
evaluations with the live stack and the right exterior (pre: 0/2000);
legacy dipole twins exact and its `A` within 0.2 % of the Fresnel integral
below relative index 1; multipole `A` likewise.

Part B, rendered (interior index scaled 1.5 with the enclosure; n = 4
renders per side; ratio enclosed/air, independent-sides sd; bands >= 3 sd):

| Row | spp | pre-DL-291 | final (range over 5 full-suite runs) | band |
|---|---:|---:|---:|---:|
| skin multipole / PT | 64 | **0.9047** +/- 0.0011 | 0.9979-1.0006 | 0.02 |
| skin multipole / BDPT | 128 | **0.9018** +/- 0.0127 | 0.9849-0.9987 (sd 0.008-0.015) | 0.06 |
| skin multipole / PT spectral | 256 | **0.9168** +/- 0.0090 | 0.9870-1.0021 | 0.04 |
| skin multipole, eta < 1 / PT | 64 | **0.8284** +/- 0.0025 | 0.9963-1.0020 | 0.02 |
| legacy dipole op / pixelpel | 4 | **0.8030** +/- 0.0064 | 1 within 0.03 (1.00502 once in the review's five runs) | 0.03 |
| legacy dipole op, eta < 1 (1.3 in 1.5) / pixelpel | 4 | **0.7143** +/- 0.0077 | 1 within 0.03 (exactly 1 in every run seen) | 0.03 |
| legacy skin op / pixelpel | 4 | **0.7437** +/- 0.0010 | 0.9983-1 | 0.02 |

**The skin BDPT row (review P1-2).**  Round 1 ran it at 32 spp with a
0.02 band; its independent-sides sd was 0.032, and in the FULL suite a
BDPT thread race decorrelates the pair's shared seed, so it failed 2 of 5
full runs (0.9714, 0.9737).  Measured sd 0.032 / 0.019 / 0.0099 at
32 / 128 / 512 spp (BDPT fireflies through the multipole); 512 spp costs
~34 s for the row alone, so the row runs at 128 spp with a 3-sd band
(0.06), which the pre-DL-291 0.90 still clears by ~8 sd.  **Five full
suite runs: 200/0, 200/0, 200/0, 200/0, 200/0.**  After merging `master`
`d694244e` (DL-306's Part C furnace rows join the suite): **227/0 in three
of three full runs** (~85 s), the skin BDPT row reading 1.0210 / 0.9754 /
1.0049.

Gate (clean rebuild, 0 warnings, library and all 18 test targets):
`SSSRadianceScalingTest` 576220/0, `BSSRDFNormalizationTest`,
`SubsurfaceScatteringSpectralTest` 8/0, `SSSBuildDeterminismTest` 2/0,
`BlenderBridgeSSSTest`, `CstDeriveGoldenTest` 454 MATCH / 0 DRIFT,
`SourceHygieneTest` 167/0, `AgentReadValidateTest` 341/0,
`CSGNullGeometryLuminaireCrashTest` (constructs the legacy skin op),
`BSSRDFEntryPointTest`, `BSSRDFSamplingTest`, `BSSRDFProjectionNormalTest`,
`BSSRDFPlanarProbeReachTest`, `HairSSSEntryNormalTest`,
`RandomWalkDensityCutoffTest`, `OptimalMISTrainingSitesTest` 111/0,
`RefractiveRadianceScalingTest` 40/1 or 41/0 (row C, the pre-existing
DL-308, at its band edge).  **DL-04 is not gated for skin**:
`SSSRadianceScalingTest` binds no Donner-Jensen class, and the scale
invariant cannot see an eta^2 factor.  DL-291 does not touch the eta^2
convention, so this is unverified coverage rather than a regression.

The gate also caught a defect in round 1's own intermediate commit: the
octree's air/non-air helper called itself on its non-air arm (no compiler
warning; the build evaluated the air dipole instead), and the octree
check plus the legacy-dipole row went red (186/2) until it was corrected.

### 10.4 In air: identical except for one intended consistency change

Every changed expression computes the pre-DL-291 value at `n_e == 1`:
`RelativizeLayersToExterior` returns early, `TablesFor` returns the
constructor tables, the octree's air loops and the dipole's constructor
and `ComputeTotalExtinction` are the original code VERBATIM, and
`GetMaximumDistanceForErrorAt` reads the air variance.  "Verbatim" is
load-bearing: RISE's macOS build uses `-ffast-math`, and two refactors
that were algebraically identical in air -- an inline air/non-air helper
in the octree loop, and a ternary around the dipole's Egan-Hilgeman
polynomial -- changed the generated code enough to move 4 of 57,600 image
values by up to 8.6e-8 / 1.2e-7 relative; both were rewritten to keep the
original expressions.

**One in-air change is intended, and it is visible only at shipped
resolution.**  The octree recursion now forwards the live IOR stack to
every child node (10.2); before, only the root node read it.  The second
external review found the two shipped `multiplybsdf TRUE` scenes
(`translucent_material` on meshes) move by exactly that:

| Scene (shipped resolution) | Pixels that differ | Mean |
|---|---:|---|
| `sss_colorvariation` 500x375 | 69,574 of 187,500 (largest 0.54 abs) | 0.2790208 -> 0.2790117 (-3.3e-5) |
| `translucent_bunny` 512x512 | 95,897 of 262,144 | +2.3e-7 relative |

Isolated cause: reverting ONLY the forwarded `pIorStack` argument restores
both base hashes exactly.  Mechanism: one (`sss_colorvariation`) / two
(`translucent_bunny`) SSS shading points where the stack says OUTSIDE but
the geometry says BACK FACE -- the stackless `TranslucentBSDF::value`,
which infers the side geometrically, disagrees with the stack there, so
"exact on a closed object" does not hold for these meshes -- and every
other differing pixel is the single-threaded RNG stream shifting after
that point.  The child nodes now agree with the root, which already read
the stack before this slice: a consistency fix, not a bias.  At reduced
resolution (160x120 up to 400x300, and this slice's own 48-pixel-wide
copies) neither scene reaches such a point, which is why both this
slice's round-1 hashes and the first review saw "identical".  **Hash at
shipped resolution.**

Evidence, all single-threaded in-process renders (`force_number_of_threads
1`), fixed libc seed, OIDN off, FNV-1a over the image, base = the fix files
at `7bbb434f`:

- The second review, at SHIPPED resolution: ten scenes bit-identical --
  six shipped (`sss`, `spotlight_drama`, `caustic_sss`, `pt_sss_wax_sphere`,
  `rwsss_sphere`, `rwsss_bdpt`) and four DL-291 air scenes (skin under PT,
  BDPT and PT spectral; the legacy skin op); the two above differ; and a
  reduced `sss` copy differs in 34 of 43,200 doubles by <= 7.1e-14 (the
  compiler output for `DiffusionApproximationExtinction.h` alone; it
  vanishes at float precision).
- This slice, at REDUCED resolution (every film scaled to 48 pixels on its
  long side), float precision: 28/28 identical -- all 21 shipped scenes
  binding `subsurfacescattering_material`, `randomwalk_sss_material`,
  `simple_sss_shaderop` or `diffusion_approximation_sss_shaderop` (PT,
  BDPT, VCM and pixelpel), plus seven air scenes for the classes no shipped
  scene binds.  The 13 shipped scenes outside the review's list have only
  this reduced-resolution evidence.

Hashes are per process: a process that rendered a non-air scene first can
differ (static RNG state).

**Census.**  No shipped scene binds `donner_jensen_skin_bssrdf_material`
or `donner_jensen_skin_sss_shaderop`; `diffusion_approximation_sss_shaderop`
appears in `sss.RISEscene` and `spotlight_drama.RISEscene`, both in air
(`caustic_sss.RISEscene` has a `perfectrefractor_material` but binds only
`simple_sss_shaderop`, which has no boundary term).  So the boundary-term
fix changes no shipped image; the only shipped images that move are the
two octree-consistency scenes above.  The boundary-term fix is a
correctness gap for user scenes with skin or legacy SSS inside a
refracting medium -- and for any `diffusion_approximation_sss_shaderop`
that AUTHORS an `ior` below 1 in air, whose dipole the >= 1 polynomial
used to put out of range (virtual-source depth at `ior 0.9`: 0.8668 ->
1.1734, a correction; no shipped scene authors one, and the parser has no
range check).

### 10.5 Cost

Interleaved base/final binaries, single thread, user CPU, air scenes:
`donner_jensen_skin_bssrdf_material` PT 160x160 at 128 spp -0.05 %
(t = -0.09, n = 4); `pt_sss_wax_sphere` (Burley) -0.26 % (t = -0.51);
legacy skin op +0.45 % (t = 0.66); legacy dipole op: a first n = 4 pass
read +7.2 % (t = 9.8) that did not reproduce in a second interleaved pass
of four binaries at n = 6 (round-1 base 9.97 s, final base 10.03 s, final
10.08 s, round-1 fix 10.12 s, sd 0.5-1.1 s each -- other agents were
loading the machine), i.e. within noise.  The air paths are the original
code, so no cost is expected.  Round 1's first version, which routed every
octree evaluation through the new virtual and did the air test out of
line, measured +11.9 % / +3.3 % on the skin op / skin material and was
replaced.

### 10.6 Sibling audit (pattern: "a boundary quantity baked against air")

| Site | Verdict |
|---|---|
| `DonnerJensenSkinDiffusionProfile`, `DonnerJensenSkinSSSShaderOp` (uniform + LUT), `DiffusionApproximationExtinction` | Fixed here. |
| Multipole inter-layer `Ft_down`/`Ft_up` | Ratio of layer indices -- relative, unchanged. |
| Multipole symmetric-slab `A` on the INTERNAL faces | Pre-existing model deviation, -22.6 % red reflectance in air, and the source of DL-291's red-channel exterior dependence -- **DL-313**. |
| Multipole `ComputeFdr` below relative index 1 | Correct: a separate eta < 1 fit (Jensen 2001), within ~1e-3 of the Fresnel integral and continuous with the >= 1 branch at 1. |
| `BurleyNormalizedDiffusionProfile` `Rd` | Empirical fit, no boundary term -- nothing to make relative (confirmed, untouched). |
| `SimpleExtinction` (`simple_sss_shaderop`) | No index at all -- the new virtual's default is right. |
| `SSSCoefficients` / `RandomWalkSSSMaterial` coefficients | Pure sums (`sigma_t = sigma_a + sigma_s`), no albedo inversion and no boundary term; `m_rwParams.ior` is the ABSOLUTE index and `RandomWalkSSS` already prices it relative (DL-49). |
| `CoatedLayer::InternalDiffuseFresnel` | Not SSS, but the same kind of quantity: already evaluated at the relative `eta` from `ri.ambientIOR` (G6) -- confirmed. |
| Blender bridge (`exporter.py`) SSS conversion | Index-free; `Subsurface IOR` exported verbatim as the absolute index -- assumes air at export by construction, now said so in `exporter.py` and `docs/BLENDER_MATERIAL_TRANSLATION.md`; RISE applies the live exterior at render time.  Not changed. |
| Painters read at a dummy record at construction (`DonnerJensenSkinDiffusionProfile`: all nine; `RandomWalkSSSMaterial`: absorption, scattering, ior, and again in `SetIOR`) | A DIFFERENT pattern found while auditing: a spatially varying painter bound to these slots is silently flattened to its value at the origin -- **DL-314**. |

### 10.7 Residuals

- **DL-313** -- symmetric-slab extrapolation on the multipole's internal
  faces (model fidelity; in air too; see 10.1 for its size).
- **DL-314** -- construction-time flattening of spatially varying SSS
  painters.
- A scene presenting more than 32 distinct exteriors to one multipole
  body (a dispersive enclosure) reuses the nearest cached table after the
  32nd, with a warning; the approximation is bounded by the cached keys'
  spacing and depends on which exteriors arrived first.
- The legacy ops' rasterizer-state cache (`cache TRUE`) keys on object and
  raster state, not exterior -- pre-existing and unchanged.
- DL-04 (no unmatched eta^2) is not gated for the Donner-Jensen classes.
- **DL-332** (filed at merge): the DL-49 rows share the pattern review
  P1-2 found in this slice's skin BDPT row -- a band below 3 sd that holds
  only while the pair's shared seed survives the full-suite thread race.
  The second review measured band/sd (decorrelated): `diffusion_rough/BDPT`
  0.90, `diffusion_smooth_dense/BDPT` 0.48, `diffusion_smooth/BDPT` 2.6,
  `random_walk/BDPT` 2.3 (reads 0.9487 .. 1.0349 over five runs),
  `diffusion_smooth/PT-spectral` 1.97, `random_walk/PT-spectral` 2.96;
  `diffusion_smooth_dense/BDPT` went red once (1.0062) in this slice's
  pre-DL-291 A/B.  Not changed here (DL-49's gate).
