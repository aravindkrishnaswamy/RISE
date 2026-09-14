# DL-52: BSSRDF planar probe-origin omission

Status: **CLOSED 2026-09-13** — source repair `00cd6723`.

## Mechanism

`BSSRDFSampling::SampleEntryPoint`'s disk-projection probe (Step 6) started
BOTH probe rays (`+probeAxis` and `-probeAxis`) AT `probeCenter` — a point
that lies IN the projection plane through the exit point — and advanced
`BSSRDF_RAY_EPSILON` before the first intersection test.

On a broad flat face, a lateral tangent/bitangent offset never leaves the
plane of the surface, so `probeCenter` is itself coplanar with the nearby
surface. Advancing `+probeAxis` (the outward normal, by construction 50% of
samples) moves the ray origin to the far side of that surface — tracing
further outward never crosses it again. Advancing `-probeAxis` (inward)
moves the origin to the near side — already past the very surface the probe
was centered on, so tracing further inward never finds it either. The
near-coplanar entry point — the entire point of the disk-projection scheme
— was skipped by construction on every normal-axis probe, on any flat or
nearly-flat surface, regardless of scene content. Only a distant, unrelated
surface crossed later along either half-line (e.g. the far side of a slab)
could restore any profile mass; on a surface with no such distant fallback
(a single flat face), the probe found nothing at all.

## Repair

`BSSRDFSampling.cpp`'s Step 6 now traces ONE continuous chord per axis,
starting `probeMaxDist` back along `-probeAxis` from `probeCenter` and
travelling forward through a total length of `2*probeMaxDist` — the
standard separable-BSSRDF probe (Christensen & Burley 2015; PBRT's
`SeparableBSSRDF::Sample_Sp` traces the same start-before/travel-through
chord). A surface coplanar with (or arbitrarily close to) the projection
plane is now crossed mid-chord like any other intersection, never skipped
by an epsilon offset anchored ON the plane. The single monotonic sweep (the
bounce loop only ever advances `traveled` forward) cannot hit the same
physical point twice, and it subsumes what the old two-direction trace
covered on both sides of `probeCenter` — no separate `+axis`/`-axis` loop is
needed. No change to the disk-to-area PDF (`pdfSurface`), the projection
cosines, the Sw normalization, or the complete-event eta convention (DL-04);
DL-54's geometric-normal projection fix is untouched.

**`maxProbeHits` scope change.** The `maxProbeHits = 64` bounce cap
(`BSSRDFSampling.cpp` ~:154) is unchanged in VALUE but now applies to
the whole single chord, where before it applied per DIRECTION (64 on
the `+axis` half-line, 64 more on the `-axis` half-line) — i.e. the
effective cap on total collected hits per `SampleEntryPoint` call
dropped from 128 to 64. This is not expected to matter in practice
(64 intersections along one probe direction through the profile's
effective range is already far more than any realistic scene's local
geometry produces), but is a real behavior change worth naming for
anyone tuning dense/thin-shell scenes against this cap. See also
[DL68_BSSRDF_PROBE_ENTRY_NORMAL.md](DL68_BSSRDF_PROBE_ENTRY_NORMAL.md)
for a follow-on defect this single-chord repair introduced (entry-normal
orientation on near-half hits), found gating this row's own closure.

## Red-proof

`tests/BSSRDFPlanarProbeReachTest.cpp` (committed `459305de` against
unfixed `BSSRDFSampling.cpp`) forces the axis-selection draw below 0.5
(the normal-axis branch) via a deterministic `NormalAxisForcingSampler`,
against a real `InfinitePlaneGeometry` object with a real
`SubSurfaceScatteringMaterial` diffusion profile — the starkest flat-face
case, since there is no distant surface anywhere for a broken probe to
fall back onto. A curved-geometry control (a small sphere, where the
tangent plane visibly curves away from the surface) isolates the flat-face
failure from a broken harness.

```
Control (sphere, normal-axis forced): valid 372/500
Flat face (plane, normal-axis forced): valid 0/500  coplanar 0/0
Failures: 1
```

Fixed:

```
Control (sphere, normal-axis forced): valid 372/500
Flat face (plane, normal-axis forced): valid 500/500  coplanar 500/500
Failures: 0
```

## Sibling audit

`SampleEntryPoint` is the sole disk-projection probe implementation; PT's
Pel/NM loop and BDPT's eye/light RGB/NM generators (VCM/MLT reuse BDPT's
generators) all consume its `SampleResult` fields without re-implementing
the probe. `RandomWalkSSS.cpp` reuses `BSSRDF_RAY_EPSILON` only as an offset
constant for its own, unrelated free-flight volumetric walk — it has no
analogous "probe from a plane-anchored center" structure, so it is not a
sibling of this bug pattern.

The one place the OLD two-direction algorithm was duplicated outside
production is `tests/BSSRDFProjectionNormalTest.cpp`'s `SelectedProbeHit`
oracle (built to independently retrace the same selected physical hit from
a fixed sampler-draw prefix, for DL-54's geometric-normal check). It is
updated to the same single-chord shape in this fix so its paired-RNG-stream
lockstep with production is preserved; its own regression numbers
(active=48/128, axes=33/10/5, all PDF/weight checks) are unchanged on its
sphere fixture, since a sphere's tangent plane is not exactly coplanar with
the sphere.

## File status

| File | Status |
|---|---|
| `src/Library/Utilities/BSSRDFSampling.cpp` | Modified: single-chord probe trace (`00cd6723`). |
| `src/Library/Utilities/BSSRDFSampling.h` | Modified: Step 6 algorithm-overview comment. |
| `tests/BSSRDFPlanarProbeReachTest.cpp` | Added: DL-52 red-proof (committed `459305de` before the fix). |
| `tests/BSSRDFProjectionNormalTest.cpp` | Modified: `SelectedProbeHit` oracle mirrors the new chord shape. |
| `tests/README.md` | Modified: new row + note on the updated oracle. |
| `docs/DEBT_LEDGER.md` | Modified: closure and counts. |
| `docs/DL52_BSSRDF_PLANAR_PROBE_ORIGIN.md` | Added: this file. |

Gate: `BSSRDFPlanarProbeReachTest`, `BSSRDFProjectionNormalTest`,
`BSSRDFNormalizationTest`, `BSSRDFSamplingTest`, `SSSRadianceScalingTest`,
`RandomWalkSurvivalTest` all pass; `make -C build/make/rise -j8 all` clean
(zero warnings).
