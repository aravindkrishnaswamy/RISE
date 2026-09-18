# DL-96 — BSSRDF entry on an open double-sided sheet's second face

**Status: CLOSED 2026-09-18** (slice `debt-dl96`, branched from `master`
`3b88fd68`).  Regression guard: `tests/BSSRDFOpenSheetEntryTest.cpp`
(52 checks / 0 failures).  DL-97 (a sibling row opened by the same
`debt-dl70` slice) is closed in the same slice as a consistency pin —
see its own section below.

---

## 1. The defect, in one sentence

DL-70's BSSRDF front-face gate (`PathTracingIntegrator.cpp` x2,
`BDPTIntegrator.cpp` x4) used `RayIntersectionGeometric::TrueGeomFacing()`
to recover a double-sided hit's single TRUE outward normal — the right
answer for a CLOSED solid (exactly one face is "outside"), but it
silently drops BSSRDF entry from the SECOND face of an OPEN sheet (a
leaf, a cloth card), where both faces are legitimate physical entry
points.

## 2. Why DL-70's own fix produced this row

Pre-DL-70 the gate read the raw, ray-facing `vGeomNormal` directly,
which is unconditionally positive on a double-sided hit — an
unconditional PASS from EITHER face, but one that then fed
`BSSRDFSampling::SampleEntryPoint` a wrong-hemisphere normal on the
disagreeing face (which DL-71's own fix there turns into an
away-facing frame).  So the pre-DL-70 "admits both faces" behaviour was
never correct second-face SSS either — DL-70's fix (admit only the
TRUE outward face) was strictly better for a closed solid and
strictly worse for an open sheet, which is why this is filed as its
own row rather than a DL-70 regression.

## 3. The fix

A new field, `RayIntersectionGeometric::bOpenSheet`, is stamped by the
geometry that produced a hit — the same "per-surface identity"
category `bGeomNormalOrientedToRay`/`bGeomNormalRayDerived` already
established in DL-70/DL-75:

```cpp
//! true when a double-sided hit is on an OPEN 2-D SHEET (both faces
//! legitimate) rather than a closed, watertight solid (one true
//! outward face).  Default false.
bool bOpenSheet;
```

and a new facing function scoped to exactly the gate DL-96 fixes:

```cpp
inline Scalar BSSRDFEntryFacing( const Vector3& d ) const
{
    return bOpenSheet
        ? Vector3Ops::Dot( vGeomNormal, d )   // ray-facing: both faces admitted
        : TrueGeomFacing( d );                // closed-solid: DL-70's fix, unchanged
}
```

The six gate sites (`PathTracingIntegrator.cpp` :2835/:3099,
`BDPTIntegrator.cpp` :2358/:2478/:6417/:6536) now call
`ri.geometric.BSSRDFEntryFacing( wo )` instead of `TrueGeomFacing( wo )`.
Everything downstream of the gate — `BSSRDFSampling::SampleEntryPoint`,
its DL-54 projection cosines, the Fresnel/cosine sampling at the entry
point — is unchanged; see §5 for why.

## 4. Which geometries set `bOpenSheet`

| Geometry | Rule | Rationale |
|---|---|---|
| `TriangleMeshGeometryIndexed` | `bDoubleSided && !m_bWatertight` | DL-143's build-time position-weld watertightness certification is exactly "does this mesh have one true outside" |
| `TriangleMeshGeometry` (non-indexed) | `bDoubleSided` unconditionally | this class has no watertightness certification at all |
| `ClippedPlaneGeometry` | on a back-face hit (`isBackFaceHit`, same condition as `bGeomNormalOrientedToRay`) | a plane never encloses a volume — front-face hits never flip, so the flag is a no-op there either way |
| `BezierPatchGeometry` | on a back-face hit (`bDidFlip`, same condition as `bGeomNormalOrientedToRay`) | same reasoning as `ClippedPlaneGeometry` |
| `HairGeometry` | never sets it | already excluded from the DL-70 recovery via the orthogonal `bGeomNormalRayDerived` reason (no genuine two-sided winding at all, not "two legitimate sides of one winding") |
| `CSGObject::AdoptCsgSurfacePayload` | forwards whichever operand's surface is actually being reported | same category as the two sibling fields it already forwards |
| `DisplacedGeometry` | inherited for free | delegates its `IntersectRay` straight into its internal `TriangleMeshGeometryIndexed` |

`TriangleMeshGeometryIndexed` sets the flag UNCONDITIONALLY inside its
`if( ri.bHit && bDoubleSided )` block — not only on a flipped hit —
because it is a property of the whole mesh's certification, not of
which face this particular ray struck.

## 5. What was checked but NOT changed, and why

The task brief for this row explicitly asked to audit every downstream
consumer of the entry normal.  Two were checked and left alone:

* **`BSSRDFSampling.cpp`'s own DL-71/DL-75 probe recovery.**  Its per-hit
  loop unconditionally recovers the TRUE winding-order normal for any
  probe hit whose geometry sets `bGeomNormalOrientedToRay` (excluding
  only the ray-derived, hair case).  Gating that recovery on
  `bOpenSheet` too was IMPLEMENTED and then REVERTED after an isolated
  rebuild showed it collapsed `BSSRDFPlanarProbeReachTest`'s
  `outward`/`neePositive` checks from 500/500 to 0/500 on its real
  double-sided mesh/clipped-plane fixtures.  The reason is structural:
  those fixtures are flat, effectively-2-D sheets, and the probe's own
  invariant — "no legitimate reason for the entry side to disagree"
  (that test's own words) — is a SEPARATE, already-validated
  correctness property from the entry GATE's admission question.  The
  gate answers "should THIS exit hit attempt BSSRDF at all"; the probe
  answers "given that it did, what is the true local orientation of a
  NEARBY point on the same physical surface" — and for a thin,
  open-but-otherwise-ordinary sheet, treating the whole surface as one
  coherent 2-D manifold with a single winding-order normal (matching a
  Christensen-Burley/PBRT-style diffusion BSSRDF's actual physical
  model: light re-emerges from the SAME sheet, transported laterally,
  regardless of which face the viewer is on) is the right model, not a
  per-hit ray-facing one.  Confirmed by direct render: with only the
  entry gate fixed (this row), the money test's front and back camera
  views of a fully-flat, uniformly-lit-from-one-side open sheet read
  luma 0.559872 / 0.559194 (PT), 0.56792 / 0.572329 (PT spectral) and
  0.558779 / 0.558742 (BDPT) — front and back agree to within Monte
  Carlo noise, which is the physically expected result for a thin
  sheet's diffusion transport and would NOT hold if the probe's own
  recovery were also gated on `bOpenSheet` (verified: it collapses the
  orthogonal `BSSRDFPlanarProbeReachTest` invariant instead of fixing
  anything new).
* **The DL-54 projection cosines** (`cosN`/`cosT`/`cosB` in
  `BSSRDFSampling.cpp`) already wrap every dot product in `fabs()`, so
  they are invariant under a sign flip of the geometric normal either
  way — no `bOpenSheet` branch needed.

## 6. Red-proof

`tests/BSSRDFOpenSheetEntryTest.cpp`.  Part A is a red-proof BY
CONSTRUCTION: `bOpenSheet` and `BSSRDFEntryFacing()` do not exist on
the pre-fix header, so the whole file fails to COMPILE against it (15
`no member named 'bOpenSheet'`/`'BSSRDFEntryFacing'` errors, confirmed
by an isolated `git checkout 3b88fd68 -- <the ten fix files>` rebuild)
— the same red-proof shape DL-186's own brand-new-ABI tests use.

Part A also pins the GATE MECHANISM divergence directly, on a real
`ClippedPlaneGeometry` back-face hit (production `IntersectRay`, no
scene/render machinery):

```
BACK hit: BSSRDFEntryFacing=1  TrueGeomFacing (pre-fix behaviour)=-1
```

— `BSSRDFEntryFacing` admits (positive), `TrueGeomFacing` (the
pre-fix DL-70 recovery) would have rejected (negative), on the exact
same hit.

Part B is the END-TO-END render-level money test: a double-sided open
`clippedplane_geometry` sheet (`subsurfacescattering_material` at its
own physically-reasonable defaults) lit from only its +Z side by an
`omni_light`.  A FRONT camera views the lit face directly; a BACK
camera views the unlit face, reachable only via BSSRDF transport.
Measured with the FIX REVERTED (isolated `git checkout 3b88fd68 --`
on the ten fix files, `bin/rise` CLI, matching scene text, PT RGB, 128
spp):

```
front mean(0-255 scale) [47.0625, 47.0625, 47.0625]
back  mean(0-255 scale) [0.0, 0.0, 0.0]
```

— the back face reads EXACTLY zero pre-fix (the gate rejects every
attempt).  With the fix restored, the SAME scene (via the test's own
in-memory capture, PT RGB 128 spp, 16x16):

```
front mean=(0.186624,0.186624,0.186624)  luma=0.559872
back  mean=(0.186398,0.186398,0.186397)  luma=0.559194
```

— front and back now agree to within Monte Carlo noise.  The
identical front/back symmetry holds under PT spectral (hero
wavelength: 0.56792 / 0.572329) and under BDPT (0.558779 / 0.558742) —
confirming the fix lands correctly in both integrators' independent
gate sites (2 in `PathTracingIntegrator.cpp`, 4 in
`BDPTIntegrator.cpp`).  A SINGLE-SIDED control (`doublesided FALSE`)
reads luma 0 for its back view in every build — a single-sided plane
has no back face to hit at all, so this control is mechanically
unaffected by DL-96 either way.

### A note on the light source

The render-level scene originally used a `directional_light`.  While
building the BDPT half of the money test, that light type was found —
independently of this fix — to render EXACTLY ZERO under
`bdpt_pel_rasterizer`, even on a plain `lambertian_material` control
with no SSS material involved at all (isolated via `bin/rise` +
`imageconverter`, checking the CENTER pixel of the frame, not the
background corner).  This matches CLAUDE.md's already-documented "VCM
has no directional-light sampling at all" gap; BDPT appears to share
it.  Switched the render-level fixture to a delta-position `omni_light`
instead (which BDPT does support, confirmed the same way), which is
also a cleaner control point-light source: it has no line of sight
around an opaque sheet to its own back, so it illuminates only the +Z
face exactly like the directional light was meant to, and — being a
point with no surface — it can never accidentally block a camera's
view the way a mesh area emitter placed carelessly in front of a
camera can.  Filing a BDPT-directional-light gap as a new debt was
considered and declined: it is orthogonal to DL-96, already covered by
the existing VCM entry, and not exercised by any change in this slice.

## 7. Cost

None measured beyond the one extra branch (`bOpenSheet ? raw : recovered`)
inside `BSSRDFEntryFacing()`, evaluated only at the six BSSRDF gate call
sites (already gated behind `pProfile && pBRDF`, i.e. only on materials
with a diffusion profile) — negligible next to the disk-projection
probe's own cost.

## 8. Sibling audit (`docs/skills/audit-by-bug-pattern.md`)

The bug pattern is "a which-side-of-the-real-surface test assumes a
CLOSED solid's single true side, but the surface can be an OPEN sheet
with two legitimate sides."  Enumerated against DL-70's own 16-site
list:

* **BSSRDF front-face gate (6 sites)** — THIS row, fixed.
* **Medium-stack push/pop (`LightSampler.cpp` x2, `BDPTIntegrator.cpp`
  x1)** — NOT the same pattern: a medium occupies a VOLUME, and an
  open 2-D sheet has no interior for a medium to occupy at all (there
  is no "second face's volume" to admit) — closed-solid semantics are
  the only physically meaningful choice here, open or closed sheet
  alike. Confirmed no change needed.
* **Transmissive-shadow Fresnel pair (`RayCaster.cpp`)** — a Fresnel
  interface pair (Ni, Nt) is a property of a REFRACTIVE boundary
  between two volumes; an open sheet with a dielectric material is a
  physically different (and separately debt-tracked, DL-111/DL-112)
  question from BSSRDF diffusion transport. Not in scope for DL-96.
* **Volume-shading entering/leaving (`DirectVolumeRenderingShader.cpp`)**
  — same reasoning as the medium-stack case: no volume on an open
  sheet.
* **SMS photon `bEntering` stamp (`SMSPhotonMap.cpp`)** — a specular
  chain's Fresnel `etaI`/`etaT` classification, the same
  volume-boundary category as the Fresnel pair above.
* **Alpha-card `bOneSided` cull (`TransparencyShaderOp.cpp`)** — DL-70's
  own row already notes this may be a no-op-by-intent (a double-sided
  card arguably WANTS both sides visible); orthogonal to BSSRDF.
* **Override-UV-generator normal (`Object.cpp`)** — a UV chart
  question, unrelated to BSSRDF admission.

No sibling site needed the same fix.

## 9. Files touched

| File | Change | Commit |
|---|---|---|
| `src/Library/Intersection/RayIntersectionGeometric.h` | new `bOpenSheet` field (all three ctor/assignment sites) + `BSSRDFEntryFacing()` helper | `4b97e967` |
| `src/Library/Geometry/TriangleMeshGeometryIndexed.cpp` | stamps `bOpenSheet = !m_bWatertight` | `4b97e967` |
| `src/Library/Geometry/TriangleMeshGeometry.cpp` | stamps `bOpenSheet = true` unconditionally | `4b97e967` |
| `src/Library/Geometry/ClippedPlaneGeometry.cpp` | stamps `bOpenSheet = isBackFaceHit` | `4b97e967` |
| `src/Library/Geometry/BezierPatchGeometry.cpp` | stamps `bOpenSheet = bDidFlip` | `4b97e967` |
| `src/Library/Objects/CSGObject.cpp` | `AdoptCsgSurfacePayload` forwards `bOpenSheet` | `4b97e967` |
| `src/Library/Shaders/PathTracingIntegrator.cpp` | 2 gate sites: `TrueGeomFacing` -> `BSSRDFEntryFacing` | `4b97e967` |
| `src/Library/Shaders/BDPTIntegrator.cpp` | 4 gate sites: `TrueGeomFacing` -> `BSSRDFEntryFacing` | `4b97e967` |
| `src/Library/Utilities/BSSRDFSampling.cpp`/`.h` | comment-only: documents the checked-not-changed decision (§5) | `4b97e967` |
| `tests/BSSRDFOpenSheetEntryTest.cpp` | new, 52/0 | `4b97e967` |
