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
the same `PathTracingShaderOp`, so the same rule is correct for them.  (The
spectral BDPT guiding probe casts at eye depth + 2, so probes at eye depth
>= 9 used to be refused by the old cap and now run: a guided closed-room
BDPT-spectral render changes pattern -- review, single thread 0.134135 ->
0.136931 -- with no bias, salted n = 6 0.134123 +/- 0.00055 vs 0.133704 +/-
0.00045, and no cost change.)  The
RayCaster's own medium phase continuation (`rs.depth < MaxRecursions( rc )
&& rs.volumeBounces < 64`) uses the same helper.

**Stack.**  Raising the depth cap exposes a stack limit the old 10 hid:
every nested cast is a C++ recursion.  Measured with a temporary instrument
at `CastRay` entry: one nested SSS level costs **13,232 B** (RGB) /
**12,960 B** (NM) at -O3 and **34.5 KB** at -O0 (review), a legacy
shader-op bounce about 6.2 KB at -O3, and the first level sits at
18.6-25.8 KB.  Before this slice render workers ran on the platform-default
secondary-thread stack (512 KB on macOS): `riseCreateThread` IGNORED its
`initial_stack_size` argument on pthreads and `ThreadPool` passed 0.

Round 1 of this slice bounded that with a fixed count of 16 active casts
per thread.  The external review showed it was wrong both ways: at -O0
(Xcode's `Development` configuration, the RISE-GUI scheme's Run action) 16
levels need 563 KB and the room scene below died with SIGBUS on 3 of 3
seeds, and in a release build it cut legacy chains with an authored
`max_recursion` above 15 that had always been stack-safe (hall of mirrors,
section 3).  The shipped design is:

1. **8 MB worker stacks.**  `riseCreateThread` honours `initial_stack_size`
   (`pthread_attr_setstacksize`, page-rounded; on Win32 the existing
   `CreateThread` size now passes `STACK_SIZE_PARAM_IS_A_RESERVATION`, so it
   reserves rather than commits), and `ThreadPool` requests
   `ThreadPool::kWorkerStackBytes` = 8 MB, the main thread's default.  That
   covers any nesting the depth cap allows (at most 64 SSS levels under the
   default cap of 128, 64 x 34.5 KB = 2.2 MB even at -O0).
2. **A remaining-stack guard instead of a count.**
   `Threading::riseRemainingStackBytes()` (new, `Threads.h`: macOS
   `pthread_get_stackaddr_np`/`pthread_get_stacksize_np`, Linux/Android
   `pthread_getattr_np`, Windows `VirtualQuery` on a local; the low bound
   is cached per thread, so a call is a thread-local read and a subtraction)
   and `CastRay{,NM,HWSS}` refuse a cast when less than
   `kCastStackMarginBytes` = **128 KB** remains.  This protects a thread the
   pool did not create -- `ThreadPool::ParallelFor` makes the CALLING thread
   drain tiles too, and a GUI render thread is a 512 KB `std::thread` on
   macOS -- and anything with no upper bound (`max_recursion` is unbounded).
   A refusal returns no radiance, like the depth cap, and is now COUNTED
   (`RayCaster::StackGuardRefusals()`) and LOGGED (the 1st, 2nd, 4th, ...
   refusal, so a pathological scene cannot flood the log).
3. **The margin is measured, and only a stack-size SWEEP measures it.**
   Round 2's worker measured one stack size (512 KB, margins 0-128 KB, 3
   seeds each) and read "16 KB already survives" -- an accident of how the
   SSS levels happened to line up in that one stack.  The round-2 external
   review swept it properly: -O0 library, driver thread and workers both at
   S = 512, 528, ..., 640 KB (9 sizes, 16 KB steps), stacks painted for the
   high-water mark, the thick-walled random-walk room at 16 spp:

   | margin | result over the 9 sizes |
   |---:|---|
   | 16 KB | SIGBUS at 528/560/592/608/624/640 KB (6 of 9); survivors hit the painted floor |
   | 32 KB | SIGBUS at 624 KB; survivors at the floor |
   | 48 KB | 9/9 survive; 640 KB reaches the floor |
   | 64 KB | 9/9; at least 35.4 KB left |
   | 128 KB | 9/9; at least 94.4 KB left |

   The worst excursion below the last allowed cast entry is about 34 KB
   at -O0, and `riseRemainingStackBytes` over-reports by about 12-16 KB (the
   guard page is inside the region it measures), so the smallest safe margin
   for this scene is about 50 KB.  The shipped **128 KB is about 2.5x that
   (3.5x the worst excursion)**, not the "8x" round 2 of this doc claimed.
   Every -O0 scene the review tried at 512 KB survived it, with worst
   excursions PT room 34.2 KB, `pt_sss_dragon` 36.0, HWSS room 30.6,
   PT-spectral room 30.6, BDPT-spectral guiding probe 30.6, legacy hall of
   mirrors at `max_recursion` 200 11.1, SMS + random-walk floor 3.7 (a
   composite over a random-walk wall makes no nested cast at all).  ASan
   builds were not measured.  The guard itself is correct on the main
   thread, a `std::thread`, a GCD thread, 16 MB and 200 KB pthreads and the
   8 MB pool workers, costs 0.37 ns per call, and resident memory is
   unchanged (191.4-192.4 MB).  128 KB also lets a 512 KB -O0 thread nest
   about as deep (~10 levels) as the pre-fix cap of 10 allowed (11).
   **Log limitation:** the refusal log fires on the 1st, 2nd, 4th, ...
   refusal of a PROCESS-WIDE count that is never reset, so in a long-lived
   process (the GUI) a later render whose refusals fall between two powers
   of two logs nothing; `RayCaster::StackGuardRefusals()` still counts
   every refusal.

**Truncation.**  With 8 MB workers the guard never fires in any scene
measured here (refusals 0 everywhere below), so the round-1 ceiling's
silent bias is gone: the thick-walled room reads 0.70337 +/- 0.00098
(salted n = 4, -O3), against the review's 0.70153 at the old 16-cast
ceiling and 0.70312 at ceiling 300.  It can still fire on a small CALLING
thread: rendering that room from a 512 KB `std::thread` at -O0 refuses
about 1,050 casts per render (0.699-0.701 vs 0.702-0.705), counted and
logged, never a crash.

## 3. Red-proof and measurements (every A/B against committed state)

A/B builds replace the library files with the merge base's by `git
checkout <commit> -- <files>` after committing all work, and restore with
`git checkout HEAD -- <files>`.  Every render in the two gate suites now
carries its own Sobol value salt (`SobolSamplerTestHooks::ValueSalt`,
DL-308's template) so repeats are independent randomized-QMC replicates:
unsalted, every render of a scene reuses ONE Sobol' pattern, the repeat sd
omits the QMC error (5e-5 against a true per-render 0.16 % on the slab),
and a pattern offset reads as a significant bias (section 6, the withdrawn
row).

**`SSSExteriorIndexInvarianceTest` Part E** (32x32, n = 4, salted; paired
rows share a salt; ratio = enclosed / open image mean):

| row | master | fix |
|---|---:|---:|
| E1 random walk PT, index-1.0 enclosure | **0.930358** | 0.999893 / 1.00012 |
| E1 diffusion PT, index-1.0 enclosure | **0.970626** | 0.999586 / 0.999801 |
| E1 random walk PT-spectral | **0.929624** | 1.00017 / 0.999395 |
| E1 random walk BDPT (control) | 1.00013 | 1.00005 / 1.00012 |
| E1 Lambertian PT (control) | 0.999612 | 0.999677 / 0.999677 |
| E3 legacy DT(4)/DT(1), translucent | **0.956135** | 1.00044 |
| E3 legacy DT(4)/DT(1), dielectric | **0.824781** | 0.998985 |
| E2 random-walk cluster furnace (image mean) | **0.965548** | 1.00144 / 1.00136 |
| E2 Lambertian cluster (control) | 0.998037 | 0.998037 |

(fix: two full Part E runs.)  Salted n = 8 per-pair ratio sds, from which
the bands are set: Lambertian 0.0008, random walk 0.0005, diffusion
0.0011, PT-spectral 0.0015, BDPT 0.0004.  The round-2 review's between-run
sd of the n = 4 mean over 15 base seeds puts every row's band at >= 6 sd
except the Lambertian control's 0.002, which is ~3.5 sd (sd 0.00057); E2
Lambertian one-render sd 0.0019 (band widened 0.004 -> 0.006), E2 random
walk 0.0012.  Suite: **248/6** on the master library, **254/0** (227 checks pre-existed) with
the fix.  E1 isolates defect (1) (a single convex sphere never re-enters,
so the cap is irrelevant); E2 isolates defect (2) (open air, so no
enclosure is ever in the stack).

**The review's own rigs** (PT pel, 16x16 orthographic, 256 spp, n = 3
interleaved, UNSALTED -- read these as reproductions of the row's numbers,
not as bias estimates; builds: master / fix (1) only / both):

| rig | master | fix (1) | both |
|---|---:|---:|---:|
| slab RW 1.5, open air | 0.999778 | 1.001441 | 1.000570 |
| slab RW 1.5 in index-1.0 box | **0.959910** | 0.999673 | 0.999756 |
| slab RW 1.0 in index-1.0 box | 1.000002 | 1.000002 | 1.000002 |
| slab RW 2.0 in index-1.0 box | **0.888754** | 1.000331 | 1.000152 |
| slab diffusion 1.5 in index-1.0 box | **0.983124** | 0.998655 | 0.998652 |
| slab RW 1.5 in water (1.33) | **0.969205** | 0.973269 | 0.999805 |
| slab diffusion 1.5 in water (1.33) | **0.968987** | 0.970001 | 0.995197 |

The row's 0.9603 / 0.9831 (box), 1.000 / 0.960 / 0.889 (index sweep) and
0.9694 / 0.9687 (water) all reproduce.  In water fix (1) alone recovers
0.4 % and the cap the remaining 2.6 %, as the row said.

**The open-air dragon** (`bdpt_sss_dragon` with its rasterizer swapped to
`pathtracing_pel_rasterizer`, 64 spp, `oidn_denoise FALSE`, 480x360; n = 5
interleaved, unsalted): master 0.214274 +/- 0.000109, fix (1) only
0.214179 +/- 0.000023, both **0.215870** +/- 0.000110, +0.745 %, all of it
defect (2).  Salted, the review reads +0.55 % (1/16 area, n = 4: 0.213957
+/- 0.00050 vs 0.215124 +/- 0.00052); the row quoted 0.214115 at cap 10
and 0.215864 raised.

**`SSSRadianceScalingTest`** (default run, 256 spp, K = 4, salted): each
SSS model's water rows gated against the explicit volume's, 1 % for
diffusion and 0.3 % for random walk.  This is a GENERAL energy gate on the
water rows, not an eta^2 discriminator -- any constant error larger than
the band fails it (both DL-04 `eta^2` mutations do, 576238/18, as would
any other).  Bands from four full salted runs (seeds 1000/2000/3000/4000):

| water row / explicit | master (unsalted) | fix, four salted runs | sd of the K = 4 mean |
|---|---:|---:|---:|
| diffusion, camera outside | 0.96854 | 1.00075 / 0.99981 / 0.99977 / 0.99844 | 0.10 % |
| diffusion, camera inside | 0.96961 | 1.00234 / 0.99712 / 1.00202 / 0.99983 | 0.24 % |
| random walk, camera outside | 0.96981 | 0.99980 / 0.99992 / 1.00003 / 1.00015 | 0.015 % |
| random walk, camera inside | 0.96877 | 0.99975 / 0.99989 / 1.00004 / 1.00005 | 0.014 % |

Diffusion's 1 % is about 4 sd of its noisier row, random walk's 0.3 %
about 20 sd; the pre-fix 3 % deficit is far outside both.  Suite:
**576244/12** (salted master water ratios: diffusion 0.97262 out / 0.97257 in, random walk 0.96959 / 0.96867) on the master library, **576256/0** with the fix (default
seed; the guard count varies with the seed because the coverage probe's
accepted samples do).  Round 1's claim that diffusion reads "0.9956 in
air, a Burley-on-a-finite-slab property" was the fixed Sobol' pattern:
salted, diffusion in air reads 1.0021 / 1.0017 / 0.99998 / 0.99951.

**Hall of mirrors** (the review's legacy P2-1 rig: pixelpel, two 0.97
mirrors over a grey floor, `[Emission, DirectLighting, Reflection]`,
single-threaded, seed 77; master / round-1 16-cast ceiling / fix):

| max_recursion | master | round 1 | fix |
|---:|---:|---:|---:|
| 10 | 0.16910333 | 0.16910333 | 0.16910333 (hash equal) |
| 15 | 0.16976419 | 0.16976419 | 0.16976419 (hash equal) |
| 20 | 0.17010738 | 0.16998702 | 0.17010738 (hash equal) |
| 30 | 0.17018263 | 0.16998702 | 0.17018263 (hash equal) |
| 60 | 0.17023368 | 0.16998702 | 0.17023368 (hash equal) |

Refusals 0 at every row.  (The review's absolute values, 0.1683-0.1695,
use a different reduction of the same images.)

**-O0 crash scene** (make build with `CXXFLAGS="-O0 -g"`, final code,
default 8 MB workers): `room_rw_pt` seeds 1-3 0.70218 / 0.70545 / 0.70466,
HWSS (`room_rw_hwss16`) 0.70331 / 0.70406 / 0.70200, PT-spectral
0.70471 / 0.71067 / 0.72596 -- 9/9 rendered, 0 refusals.  Render driven
from a 512 KB `std::thread` (the GUI shape): 4/4 rendered, about 1,050
refusals each on `room_rw_pt`, 0 on the HWSS room.

## 4. Caller audit (bug pattern: "a nested cast changes state the caller reads afterwards")

After the fix no caller CAN be affected: a `const IORStack&` no longer
admits a current-object write, and the compiler enumerated the eight sites
that made one (the review re-ran the census: no ninth writer, no
`const_cast`, no stack pointer in `RAY_STATE`/`RayIntersection`).  Every
`const IORStack&` parameter in `src/Library` is now documented `[in]`
(83 files, comments only).  The table records which callers WERE affected
before the fix.

| caller | stack handed to the nested cast / recursion | reads it afterwards? | pre-fix | evidence |
|---|---|---|---|---|
| PT diffusion SSS continuation (RGB, NM; HWSS via NM) | the vertex's own `iorStack` | yes: PART 3 `Scatter` (`containsCurrent`) | **affected** | E1; slab 0.983 |
| PT random-walk SSS continuation (RGB, NM; HWSS via NM) | same | same | **affected** | E1; slab 0.960 / 0.889 |
| `DistributionTracingShaderOp` (RGB, NM) | the shade stack (rays whose `scat.ior_stack` is null) | yes: re-`Scatter` per sample, `Pdf` MIS partner, `RadianceEtaScale` | **affected** | E3; shipped `blurry_glass` +13.6 % |
| `FinalGatherShaderOp` | the shade stack (gather rays; null-stack lobes) | yes: `valueStateful(..., &ior_stack)` after each gather cast, per-sample re-`Scatter` | **affected** (proof, same shape as DT) | shipped `cornellbox_fg` -0.41 % (t = -0.6, n = 2: not resolved) |
| `ReflectionShaderOp` / `RefractionShaderOp` | the shade stack for a null-stack lobe | only through LATER ops at the same vertex (the shader's `Scatter` runs once, before any op) | affected only where a stack-reading op follows (e.g. `[DefaultReflection, dt]`) | proof |
| `AlphaTestShaderOp` / `TransparencyShaderOp` | the shade stack (pass-through) | only through later ops | same as above | proof |
| `DirectVolumeRenderingShader` | the shade stack | no (returns) | unaffected | proof |
| Top-level rasterizers (`PixelBasedPel`, `PixelBasedSpectral{,RGB}`, HWSS) | a camera stack | only by the next `CastRay`, which sets the current object before any read | unaffected | proof |
| `AOVBuffers` OIDN guide | a fresh local stack | no | unaffected | proof; review: BDPT with OIDN on bit-identical |
| BDPT spectral guiding-training probe (`RecordGuidingTrainingSampleNM`) | the continuation stack | only after the next hit's own `SetCurrentObject` on a local | unaffected by defect (1); CHANGED by defect (2) (probes at eye depth >= 9 now run; section 2) | review: pattern change, no bias |
| Photon tracers (caustic pel/spectral, global pel/spectral, translucent pel) | their own recursion wrote the caller's stack | siblings set the current object before reading; the parent never reads after a child returns | unaffected (now copies) | proof; review: photon maps bit-identical |
| `LightSampler` transparent-shadow walk (`RayCaster::WalkShadowSegment`) | a local copy | -- | unaffected | review: bit-identical |
| `MediumTransport` in-scatter NEE, `BSSRDFEntryAdapters.h`, `BSSRDFSampling` | never call `CastRay` | -- | unaffected | grep |
| SMS (`ManifoldSolver.cpp` `seedIor`, `SMSPhotonMap.cpp`) | local stacks, no `CastRay` | -- | unaffected | compiles unchanged; not edited |
| BDPT / VCM / MLT subpath walks, `PathVertexEval`, `PathTransportUtilities`, `InteractivePelRasterizer` | local stacks | -- | unaffected | compiles unchanged |

The GUI's beauty-variant and material-look pipelines
(`InteractivePelRasterizer.cpp`) already built their caster with the
integrator's own path cap (`variantMaxBounces`, `kMaterialLookMaxBounces`),
i.e. the rule in section 2; they are unchanged.

## 5. Appearance, determinism and cost

**Bit-identity.**  Renders are deterministic only single-threaded
(`force_number_of_threads 1`).  Master and fix hash identically at seeds
8300/8301 on 1/16-area `bdpt_sss_dragon`, `vcm_sss_dragon`, `rwsss_bdpt`
(its last rasterizer, MLT), `bdpt_sss_different_bsdf` (VCM), a non-SSS PT
Cornell box and `pillow`, and the review adds shapes (pixelpel 800x800),
`cornellbox_pathtracer`, `pt_guiding_stress_guided`,
`cornellbox_bdpt_materials_pt`, and `bdpt_sss_dragon` with OIDN on and
off.  The exception is BDPT-SPECTRAL WITH PATH GUIDING, whose training
probe now runs at eye depths the old cap refused (section 2).

**Shipped SSS scenes under PT** (1/16 area, `oidn_denoise FALSE`,
furnaces at 1/4 area and 512 spp; n = 3 interleaved, unsalted):
`pt_sss_dragon` (Cornell box) **+0.88 %** (t = 4.6; the review's salted
n = 4 reads +0.55 %); `composite_wacky_creature` +0.15 % (t = 0.6),
`pt_sss_wax_sphere` +0.022 % (t = 1.9), `rwsss_thin_slab` +0.023 %,
`rwsss_sphere` +0.008 %, `sss_comparison_dragon` +0.005 %,
`furnace_sss_zero_absorption` +0.009 %, `furnace_sss_absorption` -0.003 %.

**Shipped legacy-chain scenes** (every scene with a DT / final-gather /
ambient-occlusion op; 1/64 area, n = 2): `blurry_glass` (a dielectric
under a depth-1 `dt` op, 16 samples) **+13.6 %**; every other scene within
noise or below 0.1 % (`kaleidoscope_atrium` +0.047 %, `cornellbox_fg`
-0.41 % t = -0.6, `showroom`, `irradiance_cache_torture`, `tidepools`,
`gi_spheres`, `ambocc_ibl`, `sss_gi_dragon`, `spotlight_drama`,
`blurry_floor`, `dt_with_irrcache`, `dielectric_dispersion`; `pillow`,
`different_rmaps`, `simple_dispersion`, `sss_ibl` bit-identical).
`blurry_glass` is the E3 mechanism at shipped scale; linear capture,
salted (review): master DT(1) / DT(4) / DT(16) 0.136014 / 0.123042 /
0.119764 (1/4 area, n = 4), fix 0.135986 / 0.136094 / 0.136057, DT16/DT1
1.0005 +/- 0.0007 (0.9996 +/- 0.005 at 1/64 area, n = 6).  DT(1), DT(4) and
DT(16) now agree.  (Round 1's "+2.2 % DT(16)/DT(1) residual" was an
n = 2 unsalted measurement of a non-linear output and does not reproduce.)

**Cost** (user CPU, single thread, interleaved master / fix builds, images
bit-identical in each pair): a cast-bound legacy DT(1) scene (400 spp,
32x32 translucent sphere, n = 7) costs **+1.6 % to +3.1 %** across three
measurements (round 1's copy + count: +3.1 %, t = 4.2; the review's
+2.5 %; the final copy + remaining-stack guard: +1.6 %, t = 1.7); a PT
Cornell box with no nested cast +0.9 % (t = 0.3, n.s.); `pillow` +0.3 %
(n.s.).  The copy is one small `std::vector` allocation per shaded nested
cast; the guard is a thread-local read and a compare.

## 6. Residuals

- **DL-344** (new): `transparent_shadows TRUE` double-counts through a
  delta transmitter: in the same index-1.0 box the NEE shadow ray passes
  the wall with Fresnel transmittance 1 AND the BSDF-sampled continuation
  that crosses the delta wall escapes at full weight (its MIS partner was
  reset to "none" at the delta vertex).  White Lambertian slab 1.174841
  (open air 1.000001), random walk 1.151, diffusion 1.153; the review
  reproduces it on a sphere (1.138177 vs 0.99986).  Independent of DL-315.
  It is the same double count DL-05 declined for area/env NEE (forcing
  that arm through a weave gap read +103 % on the closed-form area row,
  which is why DL-05 kept area/env NEE binary).
- **Withdrawn: the round-1 "DL-343"** (diffusion reading 0.30 % higher
  with env NEE blocked than in open air).  Not a bias: salted n = 6 per
  side the slab reads open 1.000319 +/- 0.00065, boxed 1.000320 +/-
  0.00081 (difference 0.0000 +/- 0.0010), and the E1 sphere salted n = 8
  0.999565 +/- 0.00041 vs 0.999586 +/- 0.00029 (review).  The unsalted
  repeats were one fixed Sobol' pattern.
- **DL-370 (filed at merge): a closed room of thick random-walk walls
  reads ~0.70, not 1**, under PT AND BDPT alike (PT 0.70337 +/- 0.00098
  salted here, BDPT 0.7037), 0 refusals.  The round-2 review attributed it
  to a COINCIDENT-FACE loss between TOUCHING SSS objects: the random walk
  loses energy exiting through a face shared with a neighbouring box
  (PT salted 64 spp, absorption 0): one wall 1.000, two parallel walls
  0.999, four touching walls (open tube) 0.914, the same with 0.02 corner
  gaps 1.000, the closed room touching 0.706, with 0.02 gaps 1.000; BDPT
  gapped 1.000, and BDPT at eye/light depth 32/128/512/2048 all ~0.70 with
  0 refusals.
- **The stack guard can still truncate on a small calling thread** (a
  512 KB GUI render thread): counted and logged, section 2.  A render
  thread with an 8 MB stack, or not draining tiles on the caller, would
  remove it.
- `VolumeAbsorptionAttenuationTest` row O (heterogeneous blue, a +/-9 %
  band on a stochastic ratio-tracking transmittance) failed 2 of 4 runs on
  this branch while the machine was loaded and 0 of 8 unloaded; same
  distribution on master (blue sd 0.0034 master / 0.0025 fix, n = 8).
  Pre-existing flake.
- `SSSExteriorIndexInvarianceTest`'s `B: diffusion_rough/BDPT` row failed
  once in round 1 (DL-332, bands below 3 sd); master's DL-307 has since
  moved that row to 512 spp.

## 7. Gate

On the tree with `master` `4c286bd5` merged in: clean library rebuild and
44 test targets built, **0 warnings** (also a clean -O0 library build for
the crash scene).  `SSSRadianceScalingTest` 576256/0 (both DL-04 `eta^2`
mutations 576238/18), `SSSExteriorIndexInvarianceTest` 254/0,
`ExteriorIndexInvarianceTest` 231/0, `RefractiveRadianceScalingTest` 60/0,
`MediumInsideOutsideInvariantTest` 30/0, `EnvLightBalanceTest` 123/0,
`BDPTStrategyBalanceTest` 242/0, `CstDeriveGoldenTest` 456 MATCH / 0 DRIFT,
`SourceHygieneTest` 167/0, `BSSRDFNormalizationTest` pass,
`BSSRDFSamplingTest` pass, `BSSRDFPlanarProbeReachTest` pass,
`BSSRDFOpenSheetEntryTest` 73/0, `BDPTZeroExitanceBSSRDFTest` 49/0,
`SubsurfaceScatteringSpectralTest` 8/0, `VolumeEnvFurnaceTest` 32/0,
`TransparentShadowTest` 40/0, `WeaveGapShadowTransmittanceTest` 132/0,
`RayCasterEnvEscapeMISTest` 91/0, `OptimalMISTrainingSitesTest` pass,
`IORStackTest` pass, `IORStackBehaviorTest` pass,
`TranslucentIORStackTest` pass, `LegacyChainMISPartnerTest` pass,
`LegacyPhotonTransportTest` 136/0, `TranslucentPhotonEnergyTest` pass,
`TranslucentInitialContainmentTest` 43/0, `SubSurfaceExitIORTest` 301/0,
`PTGuidingMISPartitionTest` 185/0, `BDPTGuidedContinuationTest` 164/0,
`RayCasterVolumeAbsorptionTest` pass, `AmbientOcclusionCastsShadowsTest`
10/0, `HairInteriorMediumSkipTest` 24/0, `ManifoldSolverTest` pass,
`AgentViewModeRenderTest` 687/0, `AreaLightShaderOpScalarNTest` 11/0,
`LightBVHTest` 20/0, `GeomNormalOrientationSitesTest` 72/0,
`BDPTEyeDepthConsistencyTest` pass, `SobolDimensionBudgetTest` pass,
`GradedIndexInteriorFactorTest` 55/0, `VolumeAbsorptionAttenuationTest`
pass; the -O0 crash scene 9/9 and the hall-of-mirrors table (section 3).
