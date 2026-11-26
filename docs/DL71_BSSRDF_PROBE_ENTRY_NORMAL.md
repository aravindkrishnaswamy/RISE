# DL-71: BSSRDF probe entry-normal inversion on near-half chord hits

Status: **CLOSED 2026-09-13** — source repair `e416d3bd`; **round-2
correction 2026-09-13** (same date, later slice) — the round-1 repair's
positional gate was ITSELF a distinct defect on far-half hits and has
been replaced with an unconditional rule; see "Round-2 correction"
below. The row stays CLOSED (the underlying near-half inversion this
row named is fixed and stays fixed) but the mechanism/repair sections
below describe the ORIGINAL (round-1) fix for the historical record —
read the round-2 section for what actually ships.

## Mechanism

DL-52's single-chord probe fix (`00cd6723`, [DL52_BSSRDF_PLANAR_PROBE_ORIGIN.md](DL52_BSSRDF_PLANAR_PROBE_ORIGIN.md))
replaced `BSSRDFSampling::SampleEntryPoint`'s Step 6 two-half-line probe
with a single chord that travels in ONE fixed direction (`+probeAxis`)
for its entire length, starting `probeMaxDist` before the projection
plane and running through it. That fixed the coplanar near-hit that
DL-52 diagnosed, but introduced a new, distinct defect: a hit on the
near (`-probeAxis`) side of `probeCenter` — i.e. before the chord has
travelled `probeMaxDist` from `chordStart` — is the analogue of what
the pre-DL-52 code's SEPARATE `-probeAxis`-direction probe would have
found, approaching the surface from the OPPOSITE physical direction.
The single-chord code reaches that same physical point with
`ray.Dir()==+probeAxis` instead.

A geometry that re-orients its reported normal to face the incoming
ray (`RayIntersectionGeometric::bGeomNormalOrientedToRay` — currently
double-sided triangle meshes, `TriangleMeshGeometryIndexed`,
`ClippedPlaneGeometry`, and `BezierPatchGeometry`) therefore reported,
on every such near-half hit, a normal flipped relative to the correct
(`-probeAxis`-approach) convention: facing INTO the solid instead of
out of it. Since the probe's entire purpose on a broad flat/near-flat
face is to find exactly this near-coplanar entry point — the case
DL-52 exists to reach — this made every near-half BSSRDF entry point
on a double-sided surface report an inverted normal:

- `BSSRDFEntryAdapters.h`'s NEE adapters (`BSSRDFEntryBSDF::value`/
  `valueNM`) gate on `cosTheta = Dot(vLightIn, vNormal) <= 0`, zeroing
  direct lighting at the entry point for every exterior light.
- `BSSRDFSampling.cpp`'s cosine-weighted continuation direction is
  built from `entryNormal`, so the scattered ray was sent INTO the
  object instead of back out through it.
- The stored offset (`entryPoint + entryNormal * BSSRDF_RAY_EPSILON`)
  moved the ray origin below the surface instead of above it.
- BDPT stores the inverted normal on the BSSRDF entry vertex
  (`BDPTIntegrator.cpp`'s `entryV.normal = bssrdf.entryNormal`).

The disk-to-surface area Jacobian (`pdfSurface`, `BSSRDFSampling.cpp`
~:243-245) is immune — it takes `fabs()` of every projection cosine —
which is why no existing test caught the sign error: only a directional
consumer (NEE, the continuation direction, or an orientation-aware
assertion) can see it.

## Repair

In the probe hit loop, negate `h.normal`/`h.geomNormal` for a hit on
the near (`-probeAxis`) side of `probeCenter` (`distFromChordStart =
traveled + probeRI.geometric.range < probeMaxDist`) whenever
`probeRI.geometric.bGeomNormalOrientedToRay` is set, and rebuild the
per-hit ONB around the corrected normal via `CreateFromWU(h.normal,
h.onb.u())` — keeping the existing tangent as the seed so the basis
stays a genuine orthonormal triple rather than a mismatched W-vs-U/V
pair (downstream consumers overwrite `vNormal`/`vGeomNormal`/`onb`
together, e.g. `PathTracingIntegrator.cpp`'s `entryRI.onb =
bssrdf.entryONB`).

Far-half hits (`distFromChordStart >= probeMaxDist`) were already
reached with `ray.Dir()==+probeAxis`, exactly matching the pre-DL-52
"+axis" probe's convention, so they need no correction. Geometries
that never set the flag — the vast majority: any consistently-wound
single-sided mesh, and every analytical primitive (sphere, plane,
quadric, etc.) — are untouched either way, since
`bGeomNormalOrientedToRay` defaults `false` for them and the recovery
formula is a no-op.

## Red-proof

`tests/BSSRDFPlanarProbeReachTest.cpp`'s DL-71 rows add a coplanar
DOUBLE-SIDED `TriangleMeshGeometry` quad and a coplanar DOUBLE-SIDED
`ClippedPlaneGeometry`, both authored with a winding whose raw normal
matches the exit point's own outward normal (the common, realistic
authoring case — the one that actually triggers the ray-facing flip
on a near-half hit). `RunOrientationTrial` checks that the recovered
`entryNormal` agrees with the exit point's outward normal, and drives
the REAL production NEE adapter (`BSSRDFAdapters::BSSRDFEntryBSDF`)
with an exterior light straight above the surface, on the exact
record shape production wires up at a BSSRDF entry point.

```
Double-sided quad mesh (normal-axis forced): valid 500/500  coplanar 500/500  outward 0/500  neePositive 0/500
FAIL: double-sided quad mesh: entry normal must face outward (agree with the exit point's own normal) -- DL-71: the single-direction chord reports an INVERTED normal for near-half hits on geometry that orients normals toward the incoming ray
FAIL: double-sided quad mesh: BSSRDFEntryBSDF must give a positive value for an exterior light -- DL-71: an inverted entry normal zeroes NEE at every entry point
Double-sided clipped plane (normal-axis forced): valid 500/500  coplanar 500/500  outward 0/500  neePositive 0/500
FAIL: double-sided clipped plane: entry normal must face outward (agree with the exit point's own normal) -- DL-71 inverted-normal regression
FAIL: double-sided clipped plane: BSSRDFEntryBSDF must give a positive value for an exterior light -- DL-71 inverted-normal regression zeroes NEE
Failures: 4
```

Fixed:

```
Double-sided quad mesh (normal-axis forced): valid 500/500  coplanar 500/500  outward 500/500  neePositive 500/500
Double-sided clipped plane (normal-axis forced): valid 500/500  coplanar 500/500  outward 500/500  neePositive 500/500
Failures: 0
```

## Round-2 correction (2026-09-13)

The round-1 repair above applied its correction ONLY to hits on the
near (`-probeAxis`) side of `probeCenter` (`distFromChordStart <
probeMaxDist`), reasoning that far-half hits were already reached
exactly like the pre-DL-52 "+axis" probe and needed no correction.
That reasoning conflated the flip predicate (a per-hit fact about
approach side and surface winding, evaluated fresh by each setter --
e.g. `TriangleMeshGeometry::IntersectRay`'s `bFlipGeomNormal =
Dot(vGeomNormal, ray.Dir()) > 0`) with chord POSITION. The two are
independent: nothing about the flip predicate depends on which half
of the chord a hit falls in, so a second, differently-approached
surface anywhere in the far half can be flipped too, and round-1's
gate silently left it uncorrected. The near/far boundary itself was
also fragile -- it sits at the initial `BSSRDF_RAY_EPSILON` advance
from `chordStart`, so a hit a few ulps on either side of that boundary
took a different, discontinuous code path for no physical reason.

**Fix**: apply `oriented ? -raw : raw` UNCONDITIONALLY to
`h.geomNormal` whenever `bGeomNormalOrientedToRay` is set, regardless
of position along the chord -- with one exception (DL-75, see below).

**P2-A (shading-normal hemisphere, found in the same review round)**:
the round-1 repair negated `h.normal` (the SHADING normal) in
lockstep with `h.geomNormal`, but the two have INDEPENDENT flip
predicates (`TriangleMeshGeometry::IntersectRay` tests
`Dot(vNormal, ray.Dir())` and `Dot(vGeomNormal, ray.Dir())`
separately) -- at a grazing crossing they can disagree, so lockstep
negation can leave the corrected geometric normal and the shading
normal in OPPOSITE hemispheres. Fixed by orienting the (unflipped)
shading normal into the geometric normal's hemisphere AFTER the
geometric correction: `if( Dot(h.normal, h.geomNormal) < 0 )
h.normal = -h.normal;`.

**DL-75 (HairGeometry exception, filed not fixed)**: `HairGeometry`
sets `bGeomNormalOrientedToRay = true` unconditionally on every hit,
but its reported normal is FABRICATED (ray-derived) -- a hair ribbon
has no back side, so there is no genuine "other side" for `oriented ?
-raw : raw` to recover. Applying the correction to hair would just
report the ray-OPPOSITE direction, not a physically meaningful
outward normal. A new field, `RayIntersectionGeometric::
bGeomNormalRayDerived` (set `true` only by `HairGeometry`), gates the
correction off for hair; SSS on hair geometry therefore has NO
defined outward entry normal, tracked as its own row (DL-75, S,
coverage/precision) rather than fixed in this pass.

Corrected sibling count: **FIVE** geometry types now set
`bGeomNormalOrientedToRay` (see the "Sibling audit" section below for
the updated list, superseding the "four geometry types" count in the
Sibling audit section as originally written for round 1).

### Round-2 red-proof

Two new fixtures in `tests/BSSRDFPlanarProbeReachTest.cpp`:
- `MakeDoubleSidedQuadMeshAtZ`: a flip-oriented double-sided quad
  placed entirely in the far half (at half the profile's probe
  reach). Round-1 code leaves its entry normal INVERTED (the
  positional gate never fires there).
- `MakeDoubleSidedWallMeshAtX` + `MakeExitRecordOnPlaneWithTangent` +
  a sampler forced into the tangent-axis branch (`axisSample` in
  [0.5, 0.75), never exercised by ANY round-1 fixture, all of which
  forced the normal-axis branch exclusively) -- also placed in the far
  half, so it demonstrates the SAME positional-gate defect on a
  different world axis while simultaneously closing the axis-coverage
  gap.

Red (round-1 code, i.e. `BSSRDFSampling.cpp` at `e416d3bd`, exercised
by the round-2 test additions):

```
Far-half double-sided quad (z=46.0517, normal-axis forced): valid 500/500  coplanar 500/500  outward 0/500  neePositive 0/500
FAIL: far-half double-sided quad: entry normal must face outward -- P1 round-2: round-1's DL-71 fix only corrected NEAR-half hits (distFromChordStart < probeMaxDist); a far-half hit on flip-oriented geometry was left INVERTED
FAIL: far-half double-sided quad: BSSRDFEntryBSDF must give a positive value for an exterior light -- P1 round-2: an inverted far-half entry normal zeroes NEE
Far-half tangent-axis double-sided wall (x=46.0517, axis forced 0.6): valid 500/500  coplanar 500/500  outward 0/500  neePositive 0/500
FAIL: far-half tangent-axis double-sided wall: entry normal must face outward -- P1 round-2: the positional gate bug and its fix are axis-independent, and this branch was never exercised at all before this slice
FAIL: far-half tangent-axis double-sided wall: BSSRDFEntryBSDF must give a positive value for an exterior light on the tangent-axis probe branch
Failures: 4
```

Fixed (round-2 code, unconditional correction + P2-A hemisphere fix):

```
Far-half double-sided quad (z=46.0517, normal-axis forced): valid 500/500  coplanar 500/500  outward 500/500  neePositive 500/500
Far-half tangent-axis double-sided wall (x=46.0517, axis forced 0.6): valid 500/500  coplanar 500/500  outward 500/500  neePositive 500/500
Failures: 0
```

All pre-existing rows (control sphere, flat face, the two round-1
double-sided coplanar fixtures) are unaffected: 500/500 valid/outward/
neePositive throughout, both before and after the round-2 change --
the unconditional rule is a strict generalization of the round-1 rule
on every case round-1 already handled correctly.

## Sibling audit

`SampleEntryPoint` is the sole disk-projection probe implementation
(same as DL-52); every consumer (PT's Pel/NM loop, BDPT's eye/light
generators, VCM/MLT via BDPT) reads its `SampleResult` fields without
re-implementing the probe, so fixing it here fixes every consumer.

`RayIntersectionGeometric::bGeomNormalOrientedToRay` is set by FIVE
geometry types as of the round-2 correction (DL-75 audit):
`TriangleMeshGeometry` and `TriangleMeshGeometryIndexed` (double-sided
flip), `ClippedPlaneGeometry` (back-face flip, double-sided by
construction), `BezierPatchGeometry` (front/back flip via
`!bRawFront`) — these four flip a genuine, ray-independent
winding-order normal that has two real sides, and the unconditional
correction recovers that true side — and `HairGeometry` (a FIFTH
setter, unconditional on every hit), whose reported normal is instead
FABRICATED/ray-derived and has no second side to recover; see the
round-2 correction section above and `RayIntersectionGeometric::
bGeomNormalRayDerived`'s doc comment, which gates the correction off
for hair (DL-75, filed not fixed). The red-proof exercises the first
two of the four winding-order types directly (mesh and clipped-plane,
both now at near- and far-half positions and on both the normal and
tangent probe axes); a Bezier-patch case was not added (construction
overhead — control-point authoring, `Prepare()`, BSP/Octree setup —
was judged not worth it given the fix is generic over
`bGeomNormalOrientedToRay` and does not special-case the geometry
type), but the fix applies to it identically since it reads the same
flag through the same `RayIntersectionGeometric` field.

`RandomWalkSSS.cpp`'s `SampleExit` does not use this probe — its own
free-flight volumetric walk stores `entryNormal`/`entryGeomNormal`
directly from its own exit-point boundary crossing (`exitNormal`/
`exitGeomNormal`, see `RandomWalkSSS.cpp:433-435`), never from a
disk-projection chord — so it is not a sibling of this bug pattern.

## File status

| File | Status |
|---|---|
| `src/Library/Utilities/BSSRDFSampling.cpp` | Round 1: near-half orientation correction in the probe loop (`e416d3bd`). Round 2: unconditional correction (no positional gate), gated off for `bGeomNormalRayDerived` geometry (hair), plus the P2-A shading-normal hemisphere fix. |
| `src/Library/Utilities/BSSRDFSampling.h` | Round 1: Step 6 comment documents the near-half rule. Round 2: comment rewritten for the unconditional rule + the hair exception. |
| `src/Library/Intersection/RayIntersectionGeometric.h` | Round 2: new `bGeomNormalRayDerived` field (DL-75) + rewritten `bGeomNormalOrientedToRay` doc comment (vector-recovery form, five setters). |
| `src/Library/Objects/CSGObject.cpp` | Round 2: `AdoptCsgSurfacePayload` propagates `bGeomNormalRayDerived` alongside `bGeomNormalOrientedToRay`. |
| `src/Library/Geometry/HairGeometry.{h,cpp}` | Round 2: sets `bGeomNormalRayDerived = true`; header comment documents why hair is the fifth, ray-derived setter. |
| `tests/BSSRDFPlanarProbeReachTest.cpp` | Round 1: DL-71 red-proof rows (double-sided mesh + clipped plane, `a28c7a67` before the fix, `e416d3bd` reference). Round 2: far-half quad + far-half tangent-axis wall fixtures, `RunOrientationTrial` generalized to take an outward direction / coplanarity axis / forced axis-selection sample. |
| `tests/README.md` | Modified: DL-71 note on the existing row, round 2 addendum. |
| `docs/DEBT_LEDGER.md` | Modified: DL-71 row evidence updated for the round-2 correction (still CLOSED). |
| `docs/DL04_SSS_RADIANCE_DECISION.md` | Modified: forward-pointer noting the `SSSRadianceScalingTest` guard-count baseline moved with DL-52 and is unaffected by DL-71 (round 1 or round 2). |
| `docs/DL71_BSSRDF_PROBE_ENTRY_NORMAL.md` | Added round 1; round-2 correction section added this pass. |
| `docs/DEBT_LEDGER.md` (new rows) | DL-74, DL-75 filed this pass (see their own recipes). |

Gate: `BSSRDFPlanarProbeReachTest` (0 failures), `BSSRDFProjectionNormalTest`,
`BSSRDFNormalizationTest`, `BSSRDFSamplingTest`, `SSSRadianceScalingTest`
(574017 checks, 0 failures — unchanged by this fix), `RandomWalkSurvivalTest`,
`RayCasterEnvEscapeMISTest` (91/91, P3-2: the suite grew from 79 to 91
checks the same day via `0eb7a47e` — quote 91, not 79) all pass;
`make -C build/make/rise -j8 all` clean (zero warnings).

## Tag errata (P3-7)

This row and this file were originally authored under the id **DL-68**;
the commit trailer/message of `e416d3bd` (and every earlier commit
referencing "DL-68") cannot be edited retroactively and still says
DL-68. The row was renamed to DL-71 in a later same-branch commit
(`chore(debt-ledger): renumber sssenv-slice ids...`) because DL-68 had
since been claimed by an unrelated, already-merged slice. Treat any
`DL-68` string inside a commit message/hash reference in this
repository's history as referring to what this document now calls
DL-71.
