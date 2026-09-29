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
