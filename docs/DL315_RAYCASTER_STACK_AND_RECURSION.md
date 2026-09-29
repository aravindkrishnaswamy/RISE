# DL-315: the RayCaster's caller-stack write and its recursion cap

Slice `debt-dl315`, branched from `master` `a5d94241`, 2026-09-28.
Regressions: `tests/SSSExteriorIndexInvarianceTest.cpp` Part E (rendered,
reference-free), `tests/SSSRadianceScalingTest.cpp` water rows (gated to the
explicit volume).

Two independent PT defects, both in how `RayCaster::CastRay{,NM,HWSS}` nests
a cast inside the shading of another hit, made SSS read low.  The row
(filed by the `debt-dl306` slice's external review) isolated them; this
slice reproduced every number it quoted, fixed both at the root, and found
that defect (1) also reached the legacy shader-op chain, where it was much
larger.

## 1. Defect (1): `CastRay` wrote the CALLER's `const IORStack&`

`CastRay` received its stack as `const IORStack& ior_stack` and did
`ior_stack.SetCurrentObject( ri.pObject )` before shading the hit.  That
compiled because `IORStack::pCurrentObject` was `mutable` and
`SetCurrentObject` was a `const` method -- the escape hatch
[const-correctness-over-escape-hatches](skills/const-correctness-over-escape-hatches.md)
warns about: the current object is OBSERVABLE state (`containsCurrent()`,
`push()` and `pop()` all read it), so the write was a real side effect on
the caller's stack.

Two consumer shapes read the caller's stack after a nested cast:

- **PT's SSS continuations** (`PathTracingIntegrator.cpp`, diffusion and
  random walk, RGB and NM; HWSS falls back to NM at an SSS vertex).  The
  continuation cast from the entry vertex hits the enclosure's inner wall
  and names the enclosure as the caller stack's current object.  The
  enclosure IS in that stack, so PART 3's
  `SubSurfaceScatteringSPF::Scatter` at the same vertex sees
  `containsCurrent()`, takes its inside/absorb branch, and the surface
  reflection is lost -- an `F0`-sized loss for ANY SSS object inside ANY
  containment-tracking enclosure, including one of index 1.0 (no interface
  at all).
- **The legacy shader-op chain.**  `DistributionTracingShaderOp` re-runs
  the SPF's `Scatter` for EVERY sample on the stack it was handed (and
  `FinalGatherShaderOp` does the same, and also evaluates
  `valueStateful(..., &ior_stack)` right after each gather cast).  Sample
  k's continuation cast rewrote that stack's current object to whatever
  sample k hit, so sample k+1's `Scatter` pushed its ENTRY onto a new
  stack keyed on the WRONG object (`push()` keys on `pCurrentObject`).
  At the interior exit the refractor was then not in its own stack, the
  exit read as a second entry, and the path was mispriced.  One sample per
  hit is immune by construction (one `Scatter` precedes every cast), which
  is what makes the defect measurable reference-free.

**Fix.**  `pCurrentObject` is no longer `mutable` and `SetCurrentObject` is
no longer `const`, so no code can change a const stack's current object --
the compiler enumerated every site that did.  There were eight: the three
`RayCaster` shade sites (`CastRay`, `CastRayNM`, `CastRayHWSS`) and the five
photon tracers' `TracePhoton` (caustic pel/spectral, global pel/spectral,
translucent pel).  Each now does

    IORStack hitStack( ior_stack );
    hitStack.SetCurrentObject( ri.pObject );

and shades / scatters / recurses with `hitStack`.  Every other
`SetCurrentObject` in the tree (PT, BDPT, SMS, `PathVertexEval`,
`PathTransportUtilities`, `InteractivePelRasterizer`, the shadow-walk
segment in `RayCaster::WalkShadowSegment`, and every test) already acted on
a local, and still compiles unchanged.  The `[in/out]` doc on the three
`CastRay*` stack parameters is corrected to `[in]`.

## 2. Defect (2): a hard-coded recursion cap of 10

`Job.cpp` built every integrator-owned RayCaster -- PT pel/spectral, Auto
pel/spectral, BDPT pel/spectral, VCM pel/spectral, MLT pel/spectral (ten
sites) -- with `maxR = 10`, and `CastRay` returns nothing once
`rs.depth > nMaxRecursions`.  Each SSS event nests its continuation cast at
`depth + 2`, and the nested `PathTracingShaderOp` integrator keeps counting
from there, so a path that returns to an SSS object (total internal
reflection at an enclosure, a concave object, a cluster of objects, a
Cornell box) was cut after a few events.  The legacy pixel-based
rasterizers were never affected by this half: their caster's cap is the
scene's authored `max_recursion`.

**Rule.**  An integrator-owned caster's depth cap is the path tracer's own
path-vertex cap, `kDefaultPathTracingMaxDepth` (128, one constant in
`RuntimeContext.h`, now also used by `PathTracingIntegrator`,
`PathTracingPelRasterizer` and the `RuntimeContext` default), and
`RayCaster::MaxRecursions( rc )` raises it to a larger runtime cap
(`rc.pathTracingMaxDepth`, what `PathTracingPelRasterizer::SetMaxPathDepth`
installs).  **Why it cannot truncate a legal path:** the nested
continuation is shaded by the scene's `PathTracingShaderOp`, whose
integrator loop is `for( depth = startDepth; depth < maxDepth; ... )` with
`startDepth = rs.depth` and the same `maxDepth`; a cast CastRay now rejects
(`rs.depth > cap >= maxDepth`) is one whose first vertex that loop would
itself refuse to process.  BDPT / VCM / MLT reach `CastRay` only through the
guiding-training probe and the OIDN AOV guide, both of which shade through
the same `PathTracingShaderOp`, so the same rule is correct for them.  The
RayCaster's own medium phase continuation (`rs.depth < MaxRecursions( rc )
&& rs.volumeBounces < 64`) uses the same helper.

**Stack ceiling.**  Raising the depth cap exposes a stack limit the old 10
hid: every nested cast is a C++ recursion, and render workers run on
default-size thread stacks (512 KB for a secondary pthread on macOS).
Measured on a macOS release build with a temporary instrument at `CastRay`
entry: one nested SSS level costs **13,232 bytes** (RGB) / **12,960 bytes**
(NM) of stack, the first level sits at 18.6-25.8 KB, and the shipped
`pt_sss_dragon` reaches 11 nested levels.  A per-thread count of active
casts, `kMaxCastNesting = 16` (`RayCaster.cpp`, RAII `CastNestingGuard`),
is the hard ceiling: 16 levels measured at **217 KB** on the worst scene
below.  The pre-fix depth cap allowed at most 11 nested casts, and no
shipped legacy scene authors `max_recursion` above 10, so the ceiling never
cuts a path the old code kept and never binds a shipped legacy render.  It
DOES cut PT paths that chain more than 16 nested casts (15 SSS events);
the cluster furnace below (every path returns 1, thirteen touching spheres)
measures that residual at **-0.08 %** (0.999169 +/- 0.00097 sd of one
render, n = 4).

## 3. Red-proof (every A/B against committed state)

The tests were committed first (`369d0c8c`, `E3` in a later commit), then
run against the master library by `git checkout a5d94241 -- <library files>`
and rebuilt, then restored with `git checkout HEAD -- <files>`.

**`SSSExteriorIndexInvarianceTest` Part E** (32x32, paired libc seeds,
n = 4; ratio = enclosed / open image mean):

| row | master | fix (1) only | both |
|---|---:|---:|---:|
| E1 random walk PT, index-1.0 enclosure | **0.931015** | 1.00041 | 1.00027 |
| E1 diffusion PT, index-1.0 enclosure | **0.972065** | 1.00122 | 1.00103 |
| E1 random walk PT-spectral, index-1.0 enclosure | **0.930399** | 1.00048 | 1.00056 |
| E1 random walk BDPT (control) | 0.999684 | 0.999749 | 1.00003 |
| E1 Lambertian PT (control) | 1.00006 | 0.999976 | 1.00006 |
| E3 legacy DT(4)/DT(1), translucent | **0.956156** | 0.998543 | 0.998543 |
| E3 legacy DT(4)/DT(1), dielectric | **0.825114** | 0.999746 | 0.999746 |
| E2 random-walk cluster furnace, PT (image mean) | **0.966429** | 0.96544 | 0.999169 |
| E2 Lambertian cluster (control) | 1.00054 | 1.00043 | 1.00054 |

Suite: **248/6** on the master library, **254/0** with the fix (227 of
those checks pre-existed).  E1 isolates defect (1) (a single convex sphere
never re-enters, so the cap is irrelevant); E2 isolates defect (2) (open
air, so no enclosure is ever in the stack).

**The review's own rigs, reproduced** (PT pel, 16x16 orthographic,
256 spp, n = 3 interleaved, seeds 7000-7002; builds: master / fix (1) only /
both):

| rig | master | fix (1) | both |
|---|---:|---:|---:|
| slab RW 1.5, open air | 0.999778 +/- 0.00070 | 1.001441 | 1.000570 |
| slab RW 1.5 in index-1.0 box | **0.959910 +/- 0.00041** | 0.999673 | 0.999756 |
| slab RW 1.0 in index-1.0 box | 1.000002 | 1.000002 | 1.000002 |
| slab RW 2.0 in index-1.0 box | **0.888754 +/- 0.00025** | 1.000331 | 1.000152 |
| slab RW 2.0, open air | 1.000405 | 1.000282 | 1.000608 |
| slab diffusion 1.5, open air | 0.995668 +/- 0.00004 | 0.995649 | 0.995673 |
| slab diffusion 1.5 in index-1.0 box | **0.983124** | 0.998655 | 0.998652 |
| slab RW 1.5 in water (1.33) | **0.969205** | 0.973269 | 0.999805 |
| slab diffusion 1.5 in water (1.33) | **0.968987** | 0.970001 | 0.995197 |

The row's numbers were 0.9603 / 0.9831 (box), 1.000 / 0.960 / 0.889 (index
sweep) and 0.9694 / 0.9687 (water); all reproduce.  In water fix (1) alone
recovers 0.4 % and the cap the remaining 2.6 %, as the row said.

**The open-air shipped dragon** (`bdpt_sss_dragon` with its rasterizer
swapped to `pathtracing_pel_rasterizer`, 64 spp, `oidn_denoise FALSE`,
full 480x360; n = 5 interleaved, seeds 5000-5004):

| build | image mean | sd |
|---|---:|---:|
| master | 0.214274 | 0.000109 |
| fix (1) only | 0.214179 | 0.000023 |
| both | **0.215870** | 0.000110 |

+0.745 % (t = 23), all of it defect (2) (fix (1) alone: -0.04 %, t = -1.9).
The row quoted 0.214115 at cap 10 and 0.215864 raised.

**`SSSRadianceScalingTest`** (default convention run, 256 spp, K = 4):
new checks gate each SSS model's water rows within 0.8 % of the explicit
volume's.  Master **576244/12** (all twelve new checks red), fix
**576256/0**.

| water row / explicit | master | fix (1) only | both |
|---|---:|---:|---:|
| diffusion, camera outside | 0.96854 | 0.97033 | **0.99480** |
| diffusion, camera inside | 0.96961 | 0.97191 | **0.99890** |
| random walk, camera outside | 0.96981 | 0.97325 | **1.00031** |
| random walk, camera inside | 0.96877 | 0.97266 | **1.00006** |

Diffusion's camera-outside 0.9948 is its own open-air residual (0.9956
against the explicit 1.0000 in air: Burley on a finite slab), not an index
effect.

**DL-04 still discriminates.**  Both deliberate `eta^2` mutations (the PT
diffusion and random-walk complete-event weights, continuation and spatial,
multiplied / divided by `1.5^2`) read **576238/18** each: the six air
convention checks the DL-04 pin always fails, plus all twelve new water
checks.

## 4. Caller audit (bug pattern: "a nested cast changes state the caller reads afterwards")

After the fix no caller CAN be affected: a `const IORStack&` no longer
admits a current-object write, and the compiler enumerated the eight sites
that made one.  The table records which callers WERE affected before it.

| caller | stack handed to the nested cast / recursion | reads it afterwards? | pre-fix | evidence |
|---|---|---|---|---|
| PT diffusion SSS continuation (RGB, NM; HWSS via NM) | the vertex's own `iorStack` | yes: PART 3 `Scatter` (`containsCurrent`) | **affected** | E1 0.972 / 0.930 (spectral); slab 0.983 |
| PT random-walk SSS continuation (RGB, NM; HWSS via NM) | same | same | **affected** | E1 0.931; slab 0.960 / 0.889 |
| `DistributionTracingShaderOp` (RGB, NM) | the shade stack (rays whose `scat.ior_stack` is null) | yes: re-`Scatter` per sample, `Pdf` MIS partner, `RadianceEtaScale` | **affected** | E3 0.956 (translucent), 0.825 (dielectric); shipped `blurry_glass` +13.6 % |
| `FinalGatherShaderOp` | the shade stack (gather rays; null-stack lobes) | yes: `valueStateful(..., &ior_stack)` after each gather cast, per-sample re-`Scatter` | **affected** (proof, same shape as DT) | shipped `cornellbox_fg` -0.41 % (t = -0.6, n = 2: not resolved) |
| `ReflectionShaderOp` / `RefractionShaderOp` | the shade stack for a null-stack lobe | only through LATER ops at the same vertex (the shader's `Scatter` runs once, before any op) | affected only in a chain where a stack-reading op follows (e.g. `[DefaultReflection, dt]`) | proof; a `[DefaultReflection, DefaultDirectLighting]` translucent/enclosure rig read bit-identical before and after (translucent has no reflection-typed lobe) |
| `AlphaTestShaderOp` / `TransparencyShaderOp` | the shade stack (pass-through) | only through later ops | same as above | proof |
| `DirectVolumeRenderingShader` | the shade stack | no (returns) | unaffected | proof |
| Top-level rasterizers (`PixelBasedPel`, `PixelBasedSpectral{,RGB}`, HWSS) | a camera stack | only by the next `CastRay`, which sets the current object before any read | unaffected | proof |
| `AOVBuffers` OIDN guide | a fresh local stack | no | unaffected | proof |
| BDPT guiding-training probe (`RecordGuidingTrainingSampleNM`) | the continuation stack | only after the next hit's own `SetCurrentObject` on a local | unaffected | proof; BDPT/VCM/MLT single-thread renders bit-identical (section 5) |
| Photon tracers (caustic pel/spectral, global pel/spectral, translucent pel) | their own recursion wrote the caller's stack | siblings set the current object before reading; the parent never reads after a child returns | unaffected (now copies) | proof |
| `LightSampler` transparent-shadow walk (`RayCaster::WalkShadowSegment`) | a local copy | -- | unaffected | compiles unchanged (acts on a local) |
| `MediumTransport` in-scatter NEE, `BSSRDFEntryAdapters.h`, `BSSRDFSampling` | never call `CastRay` | -- | unaffected | grep |
| SMS (`ManifoldSolver.cpp` `seedIor`, `SMSPhotonMap.cpp`) | local stacks, no `CastRay` | -- | unaffected | compiles unchanged; not edited (owned by `debt-dl290`) |
| BDPT / VCM / MLT subpath walks, `PathVertexEval`, `PathTransportUtilities`, `InteractivePelRasterizer` | local stacks | -- | unaffected | compiles unchanged |

## 5. Appearance, determinism and cost

**BDPT / VCM / MLT and non-SSS PT are bit-identical.**  With
`force_number_of_threads 1` (renders are only deterministic
single-threaded: the same binary at the same libc seed gives different
hashes multi-threaded), 1/16-area copies with `oidn_denoise FALSE`, seeds
8300 and 8301, master and fix hash identically on `bdpt_sss_dragon`,
`vcm_sss_dragon`, `bdpt_sss_different_bsdf` (VCM), `rwsss_bdpt` (its last
rasterizer), and a non-SSS PT control
(`cornellbox_bdpt_materials_pt`).  (With OIDN on, BDPT/VCM/MLT's AOV guide
cast now follows the raised depth cap; not measured.)

**Shipped SSS scenes under PT** (1/16 area, `oidn_denoise FALSE`, furnaces
at 1/4 area and 512 spp; n = 3 interleaved, seeds 8100-8102; ratio fix /
master):

| scene | master | fix | change |
|---|---:|---:|---:|
| `pt_sss_dragon` (Cornell box) | 0.214481 +/- 0.00060 | 0.216362 +/- 0.00036 | **+0.88 %** (t = 4.6) |
| `composite_wacky_creature` | 0.042411 | 0.042476 | +0.15 % (t = 0.6) |
| `pt_sss_wax_sphere` | 0.411465 | 0.411556 | +0.022 % (t = 1.9) |
| `rwsss_thin_slab` | 0.679654 | 0.679813 | +0.023 % (t = 0.7) |
| `rwsss_sphere` | 0.418927 | 0.418962 | +0.008 % (t = 0.9) |
| `sss_comparison_dragon` | 1.457414 | 1.457489 | +0.005 % (t = 0.1) |
| `furnace_sss_zero_absorption` | 0.101333 | 0.101342 | +0.009 % (t = 1.6) |
| `furnace_sss_absorption` | 0.097862 | 0.097859 | -0.003 % (t = -0.9) |

plus the full-size open-air `bdpt_sss_dragon`-as-PT above (+0.745 %,
t = 23).  Everything brightens or stays; only the Cornell-box dragon
resolves at n = 3.

**Shipped legacy-chain scenes** (every scene with a DT / final-gather /
ambient-occlusion op; 1/64 area, n = 2 interleaved, seeds 4400-4401;
`sss_gi_dragon` 1/256 area): `blurry_glass` (a dielectric under a
depth-1 `dt` op, 16 samples) **+13.6 %** (t = 118); every other scene is
within noise or below 0.1 %: `kaleidoscope_atrium` +0.047 % (t = 5.6),
`cornellbox_fg` -0.41 % (t = -0.6), `showroom` +1.3 % (t = 0.1, its
8 spp noise is 10 %), `irradiance_cache_torture` -0.25 % (t = -1.7),
`tidepools` +0.05 %, `gi_spheres` +0.06 %, `ambocc_ibl` +0.39 % (t = 1.2),
`sss_gi_dragon` +0.05 %, `spotlight_drama` -0.02 %, `blurry_floor`
+0.02 %, `dt_with_irrcache` +0.008 %, `dielectric_dispersion` -0.008 %,
`pillow` -0.09 % (n = 5, t = -0.7; bit-identical single-threaded),
`different_rmaps`, `simple_dispersion` and `sss_ibl` bit-identical.

`blurry_glass` is the E3 mechanism at shipped scale, and the fix is
checked the same way: DT samples 1 / 4 / 16 read master 0.12899 / 0.11741
/ 0.11627 and fix 0.12902 / 0.13016 / 0.13189 (n = 2 each).  Master loses
10 % between 1 and 16 samples; the fix restores all but a +2.2 % trend,
which a delta-glass variant (`scattering 1000000`) reads at +1.3 % with a
noise sd of 0.7 %.  That residual is NOT a stack effect (none can remain,
by construction) and is recorded unattributed in section 6.

**Cost** (user CPU, `force_number_of_threads 1`, interleaved master /
fix builds; the image is bit-identical in each pair, so this is the cost of
the copy and the nesting guard alone):

| workload | master | fix | change |
|---|---:|---:|---:|
| legacy DT(1), 400 spp, 32x32 translucent sphere (cast-bound) | 1.380 +/- 0.015 s | 1.423 +/- 0.022 s | **+3.1 %** (t = 4.2, n = 7) |
| PT open-air dragon, 1/16 area (fix (1) only: image bit-identical) | 5.100 +/- 0.244 s | 5.223 +/- 0.281 s | +2.4 % (t = 0.9, n = 7) |
| `pillow` (legacy DT, 1/16 area) | 0.899 +/- 0.013 s | 0.901 +/- 0.012 s | +0.3 % (t = 0.4, n = 7) |
| PT Cornell box, no SSS (no nested cast) | 4.646 +/- 0.043 s | 4.650 +/- 0.039 s | +0.09 % (t = 0.2, n = 5) |

The copy is one small `std::vector` allocation per shaded nested cast; it
shows only on a scene whose whole cost is casts against one sphere.

## 6. Residuals

- **DL-343** (new): diffusion SSS reads 0.30 % HIGHER when env NEE is
  blocked (an index-1.0 box, transparent shadows off, so the estimator is
  continuation-only) than in open air (NEE + continuation under MIS):
  slab 0.998652 +/- 0.000004 vs 0.995673 +/- 0.000020 (n = 3); sphere
  furnace E1 ratio 1.0010-1.0014 (paired sd 0.0008).  The random walk
  agrees (slab -0.08 % +/- 0.04, sphere +0.03 %).  Two estimators of one
  image disagree, so one of them is biased; DL-306's review refuted the
  entry-NEE / continuation partition only on the WATER rows, where the
  enclosure blocks NEE, so it could not have seen this.  Visible only after
  DL-315 (the stack defect masked it by 1.5-2 %).  E1's diffusion band
  (0.004) holds it.
- **DL-344** (new): `transparent_shadows TRUE` double-counts through a
  delta transmitter: in the same index-1.0 box the NEE shadow ray passes
  the wall with Fresnel transmittance 1 AND the BSDF-sampled continuation
  that crosses the delta wall escapes at full weight (its MIS partner was
  reset to "none" at the delta vertex): a white Lambertian slab reads
  **1.174841** (open air 1.000001), random walk 1.151, diffusion 1.153.
  Independent of DL-315 (NEE, not a nested cast).
- **Unfiled (no id left in this slice's reservation):** `blurry_glass`'s
  DT(16)/DT(1) = 1.022 after the fix (section 5).  Stack effects are
  excluded by construction; a sampler-dimension interaction in the legacy
  DT loop is the first suspect.
- **The stack ceiling** truncates PT paths that nest more than 16 casts
  (-0.08 % in the thirteen-sphere cluster furnace, inside noise).  A deeper
  ceiling needs either bigger worker stacks (the GUI's render thread is a
  `std::thread`, 512 KB on macOS) or an iterative SSS continuation.
- `VolumeAbsorptionAttenuationTest` row O (heterogeneous blue, a +/-9 %
  band on a stochastic ratio-tracking transmittance) failed 2 of 4 runs on
  this branch while the machine was loaded and 0 of 8 unloaded; its value
  distribution is the same on master (blue sd 0.0034 master / 0.0025 fix,
  n = 8 interleaved).  Pre-existing flake, not this slice.
- `SSSExteriorIndexInvarianceTest`'s `B: diffusion_rough/BDPT` row failed
  once (0.9927, band 0.006, its own printed sd 0.0062) and passed in three
  other full runs: DL-332 (bands below 3 sd), open, BDPT bit-identical to
  master.

The GUI's beauty-variant and material-look pipelines
(`InteractivePelRasterizer.cpp`) already built their caster with the
integrator's own path cap (`variantMaxBounces`, `kMaterialLookMaxBounces`),
i.e. the rule above; they are unchanged.

## 7. Gate

Clean library rebuild and 41 test targets built: **0 warnings**.
`SSSRadianceScalingTest` 576256/0, `SSSExteriorIndexInvarianceTest` 254/0
(one of four full runs 253/1 on DL-332's row, section 6),
`RefractiveRadianceScalingTest` 60/0, `MediumInsideOutsideInvariantTest`
30/0, `EnvLightBalanceTest` 123/0, `BDPTStrategyBalanceTest` 227/0,
`CstDeriveGoldenTest` 456 MATCH / 0 DRIFT, `SourceHygieneTest` 167/0,
`BSSRDFNormalizationTest` pass, `BSSRDFSamplingTest` pass,
`BSSRDFPlanarProbeReachTest` pass, `BSSRDFOpenSheetEntryTest` 73/0,
`BDPTZeroExitanceBSSRDFTest` 41/0, `SubsurfaceScatteringSpectralTest` 8/0,
`VolumeEnvFurnaceTest` pass, `TransparentShadowTest` 40/0,
`WeaveGapShadowTransmittanceTest` 132/0, `RayCasterEnvEscapeMISTest` 91/0,
`OptimalMISTrainingSitesTest` 111/0, and the touched-class suites
`IORStackTest`, `IORStackBehaviorTest`, `TranslucentIORStackTest`,
`LegacyChainMISPartnerTest`, `LegacyPhotonTransportTest` 136/0,
`TranslucentPhotonEnergyTest`, `TranslucentInitialContainmentTest`,
`SubSurfaceExitIORTest` 301/0, `PTGuidingMISPartitionTest` 185/0,
`BDPTGuidedContinuationTest` 164/0, `RayCasterVolumeAbsorptionTest` 9/0,
`AmbientOcclusionCastsShadowsTest` 10/0, `HairInteriorMediumSkipTest` 24/0,
`ManifoldSolverTest` pass, `AgentViewModeRenderTest` 687/0,
`AreaLightShaderOpScalarNTest` 11/0, `LightBVHTest` 20/0,
`GeomNormalOrientationSitesTest` 72/0, `BDPTEyeDepthConsistencyTest` 12/0,
`SobolDimensionBudgetTest` pass, `GradedIndexInteriorFactorTest` 55/0,
`VolumeAbsorptionAttenuationTest` (flake, section 6).  Every
`SetCurrentObject`-calling test source (45 files) also compiles.
