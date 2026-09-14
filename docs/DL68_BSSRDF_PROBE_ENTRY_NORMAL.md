# DL-68: BSSRDF probe entry-normal inversion on near-half chord hits

Status: **CLOSED 2026-09-13** — source repair `e416d3bd`.

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

`tests/BSSRDFPlanarProbeReachTest.cpp`'s DL-68 rows add a coplanar
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
FAIL: double-sided quad mesh: entry normal must face outward (agree with the exit point's own normal) -- DL-68: the single-direction chord reports an INVERTED normal for near-half hits on geometry that orients normals toward the incoming ray
FAIL: double-sided quad mesh: BSSRDFEntryBSDF must give a positive value for an exterior light -- DL-68: an inverted entry normal zeroes NEE at every entry point
Double-sided clipped plane (normal-axis forced): valid 500/500  coplanar 500/500  outward 0/500  neePositive 0/500
FAIL: double-sided clipped plane: entry normal must face outward (agree with the exit point's own normal) -- DL-68 inverted-normal regression
FAIL: double-sided clipped plane: BSSRDFEntryBSDF must give a positive value for an exterior light -- DL-68 inverted-normal regression zeroes NEE
Failures: 4
```

Fixed:

```
Double-sided quad mesh (normal-axis forced): valid 500/500  coplanar 500/500  outward 500/500  neePositive 500/500
Double-sided clipped plane (normal-axis forced): valid 500/500  coplanar 500/500  outward 500/500  neePositive 500/500
Failures: 0
```

## Sibling audit

`SampleEntryPoint` is the sole disk-projection probe implementation
(same as DL-52); every consumer (PT's Pel/NM loop, BDPT's eye/light
generators, VCM/MLT via BDPT) reads its `SampleResult` fields without
re-implementing the probe, so fixing it here fixes every consumer.

`RayIntersectionGeometric::bGeomNormalOrientedToRay` is currently set
by four geometry types: `TriangleMeshGeometry` and
`TriangleMeshGeometryIndexed` (double-sided flip), `ClippedPlaneGeometry`
(back-face flip, double-sided by construction), and `BezierPatchGeometry`
(front/back flip via `!bRawFront`). The red-proof exercises the first
two directly (mesh and clipped-plane); a Bezier-patch case was not
added (construction overhead — control-point authoring, `Prepare()`,
BSP/Octree setup — was judged not worth it given the fix is generic
over `bGeomNormalOrientedToRay` and does not special-case the geometry
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
| `src/Library/Utilities/BSSRDFSampling.cpp` | Modified: near-half orientation correction in the probe loop (`e416d3bd`). |
| `src/Library/Utilities/BSSRDFSampling.h` | Modified: Step 6 comment documents the DL-68 rule. |
| `tests/BSSRDFPlanarProbeReachTest.cpp` | Modified: DL-68 red-proof rows (double-sided mesh + clipped plane, `a28c7a67` before the fix, `e416d3bd` reference). |
| `tests/README.md` | Modified: DL-68 note on the existing row. |
| `docs/DEBT_LEDGER.md` | Modified: new DL-68 row, closed. |
| `docs/DL04_SSS_RADIANCE_DECISION.md` | Modified: forward-pointer noting the `SSSRadianceScalingTest` guard-count baseline moved with DL-52 and is unaffected by DL-68. |
| `docs/DL68_BSSRDF_PROBE_ENTRY_NORMAL.md` | Added: this file. |

Gate: `BSSRDFPlanarProbeReachTest` (0 failures), `BSSRDFProjectionNormalTest`,
`BSSRDFNormalizationTest`, `BSSRDFSamplingTest`, `SSSRadianceScalingTest`
(574017 checks, 0 failures — unchanged by this fix), `RandomWalkSurvivalTest`,
`RayCasterEnvEscapeMISTest` (79/79) all pass; `make -C build/make/rise -j8
all` clean (zero warnings).
