# DL-20 and DL-116: patch-geometry curvature, and sphere/ellipsoid pole welding

Status: **CLOSED** 2026-09-17 (slice `debt-geom2`, branched from `master` `a4495f94`;
DL-116 review follow-up the same day, same slice, closes `EllipsoidGeometry` and
corrects the tallies below after merging `master` `1c8fcbcf`).

## DL-116: `SphereGeometry::TessellateToMesh` pole welding

### The bug, in one sentence

The north and south pole ROWS of a tessellated sphere are bit-identical
positions (theta is canonicalized to 0 for every column at a pole) but
the tessellator emitted `detail+1` separate coincident vertex indices
for each pole anyway, so the pole's triangle fan degenerated into
zero-area triangles whose surviving, non-degenerate wedge triangles
shared their "radiating" pole-to-equator edges with only ONE neighbour
instead of two — a genuinely closed sphere read as an OPEN SHEET under
an index-keyed edge-manifold watertightness check, purely as an artifact
of how the tessellator indexed its poles.

### Two edge-count models, and why they disagree

This row's own red-proof (`tests/PoleWeldingWatertightnessTest.cpp`)
implements two different edge-adjacency models, because they see
different parts of the bug and conflating them hides evidence:

- **Raw-index** (`RawIndexEdgeCounts`): keys the edge map by the RAW
  per-triangle vertex-array index, no position weld at all. This matches
  what `TriangleMeshGeometryIndexed::ComputeWatertightness()` (DL-31,
  debt-prox, not yet on `master` at the time this row closed) actually
  does in production — it recovers each triangle corner's index via
  pointer arithmetic into `pPoints`, which is populated 1:1 from the
  tessellator's own output vertex list with **no** dedup pass. Under
  this model, a detail=71 sphere reported **284 boundary edges** pre-fix:
  142 from the two degenerate pole fans, and 142 from the ORDINARY,
  UNRELATED u=0/u=1 texcoord seam (see below) — exactly matching DL-116's
  originally-cited evidence.

- **Exact-weld** (`ExactWeldEdgeCounts`): additionally welds vertex
  POSITIONS by bit-for-bit equality before counting edges. This collapses
  a pole row to one canonical position even on the **unfixed**
  tessellator (every column at a pole is bit-identical to every other
  one), so it cannot see the pole bug at all — it stayed at 142 both
  before and after the fix. What it isolates instead is the ordinary
  seam: `sin(2*pi) != sin(0)` in double precision (off by ~2.4e-16 *
  radius), so the seam's two sides are NOT bit-identical positions and
  the weld does not merge them.

The **fix** (below) removes exactly the 142 pole-fan edges under the
raw-index model — the number that matters for the real, production
watertightness check — leaving the 142 seam-only edges at their
unavoidable baseline.

### The seam residual is NOT this row's bug, and is NOT fixable here

An early attempt at this fix tried to ALSO weld the general u=0/u=1 seam
by giving `vertices[]`/`normals[]` a SEPARATE, welded index stream from
`coords[]` (using `IndexedTriangle`'s independent `iVertices`/`iCoords`
arrays). This is WRONG: `GeometryUtilities::ApplyDisplacementMapToObject`
/ `ApplyScalarHeightToObject` (`DisplacedGeometry`'s per-vertex
displacement pipeline) look up `vCoords[poly.iVertices[j]]` — i.e. they
assume `iVertices[k]` and `iCoords[k]` index the SAME combined
(position, normal, texcoord) record. Splitting them corrupted every
non-pole vertex's texcoord lookup during displacement, discovered by
rendering `scenes/Tests/Geometry/displaced_sphere.RISEscene` at 256spp
before/after: the split-index attempt moved the mean pixel difference to
**1.75** (RMSE 10.5, 15797/65536 pixels changed) against a same-code
run-to-run noise floor of **0.019** (RMSE 0.94, 207 pixels) — a real,
large regression, not noise. The corrected fix (single combined index,
pole rows collapsed to ONE entry each, matching
`MakeIndexedTriangleSameIdx`'s existing contract) re-measured at **0.027**
mean diff (RMSE 1.50, 208 pixels) — statistically indistinguishable from
the noise floor.

This is why the general seam is deliberately left duplicated: its two
sides carry genuinely different texcoords (u=0 vs u=1, a real
discontinuity needed for correct texture wrapping), and welding it
without a from-scratch decoupled-index redesign (auditing EVERY consumer
that currently assumes `MakeIndexedTriangleSameIdx`'s one-index contract,
not just `DisplacedGeometry`) is out of scope. Any residual seam-only
"boundary edge" a watertightness check reports on a tessellated
sphere/torus/cylinder is a job for that check's OWN position weld
(tolerance or topology aware), not for these tessellators. Filed as
**DL-136** (below).

### The fix

`SphereGeometry::TessellateToMesh` (`src/Library/Geometry/SphereGeometry.cpp`):
emit exactly ONE shared (position, normal, texcoord) entry per pole row
instead of `detail+1` coincident ones (the pole's texcoord is already
canonicalized to `(0, v)` for every column, so nothing is lost), and skip
the ONE wedge triangle per pole cell that entry now makes degenerate
(the OTHER triangle in that wedge, which was always the real, non-zero-area
one, is emitted unchanged). Implemented as a small `Index(j, i)` lambda
that returns the shared pole index for `j==0`/`j==nV` and the ORIGINAL
per-column index everywhere else, offset by the two pole rows now
contributing one vertex each instead of `detail+1`.

### Sibling audit (per docs/skills/audit-by-bug-pattern.md)

**Bug pattern**: a tessellator represents a geometric feature that
genuinely collapses to ONE point (in position, normal, AND texcoord) as
`N` separate coincident vertex indices instead of one shared index,
turning its triangle fan's radiating edges into index-keyed
false-positive boundary edges.

| Geometry | Has a genuine "whole row collapses to one point, same uv too" feature? | Verdict |
|---|---|---|
| `SphereGeometry` (owned) | Yes — north/south poles | **FIXED**, this row |
| `TorusGeometry` (owned) | No — both parametric directions wrap fully; no row is ever degenerate (a full donut has no poles) | Not affected (confirmed empirically: raw-index and exact-weld boundary counts agree exactly, residual is the ordinary u-seam + v-seam, `4*detail`, DL-136) |
| `CylinderGeometry` (owned) | No — the open side has no poles; the two end CAPS already fan from ONE shared center vertex (pre-existing code, not something this row needed to add) | Not affected (confirmed empirically: 0 degenerate triangles either model; residual is the side wall's own u-seam PLUS the side-to-cap crease, a genuine load-bearing normal discontinuity between the cap's normal and the side's normal at the same position — not this bug pattern) |
| `EllipsoidGeometry` | Yes, structurally identical to Sphere (its own comment says "see SphereGeometry::TessellateToMesh for the reasoning") | **FIXED** (review follow-up, same slice, 2026-09-17 continuation). Originally excused here as "out of scope — owned by the concurrent `debt-prox` slice"; that was WRONG — `debt-prox` only ever touched `EllipsoidGeometry::DistanceToSurface`/`SignedDistanceLower` (DL-15's near-exact neighbour bound), never `TessellateToMesh`, confirmed by diffing `a4495f94..master` on that file, and `debt-prox` has since merged to `master` as `1c8fcbcf` without touching it. `EllipsoidGeometry::TessellateToMesh`'s pole is provably lossless to weld by the same argument as Sphere's: at a pole row `sinPhi == 0`, so `pos = (0, ±b, 0)` and the gradient normal `(pos.x*ooA2, pos.y*ooB2, pos.z*ooC2)` normalizes to `(0, sign(b), 0)` — both independent of `i` — and `u` was already forced to `0.0` for every column at a pole (pre-existing `atPole` ternary), so the texcoord was already canonicalized too; position, normal, AND texcoord agree across every column, exactly Sphere's precondition. Fixed with the identical `Index(j,i)` lambda + degenerate-wedge-skip construction. `tests/PoleWeldingWatertightnessTest.cpp`'s `TestEllipsoid` (scalene semi-axes `1.5/0.75/2.0`, not secretly a sphere): red (pre-fix) 284 raw-index boundary edges at detail=71 (bit-for-bit the same figure as Sphere's own pre-fix evidence); green (post-fix) 0 degenerate, 142 boundary edges, matching the seam-only exact-weld baseline exactly. `tests/MeshInteriorSignalTest.cpp` gained a matching `(f2)` section: a tessellated ellipsoid now answers `SignedDistanceLower` (closed 2-manifold via DL-31/DL-143's own position-weld) where it used to refuse on the same degenerate-pole pattern as Sphere's own `(f)`. |
| `CircularDiskGeometry` (owned) | **Yes** — a disk's center (`v=0, r=0`) is the SAME "whole row collapses to one point, same normal AND same canonicalized `u=0` texcoord" pattern as a sphere pole (its own `atCenter` comment: "canonicalise u at the center so every collapsed vertex gets the same displacement") | **FIXED**, this row (found during the sibling audit, not in the original recipe's target list). `tests/PoleWeldingWatertightnessTest.cpp`'s `TestDisk`: red (pre-fix) 8 degenerate triangles + `raw != exactWeld` at both tested details; green (post-fix) 0 degenerate, `raw == exactWeld` at both. A disk's OUTER RIM is a genuine open boundary (a 2D sheet, not a closed solid) and correctly still reports positive boundary edges either way — that is not a false positive. |
| `TriangleMeshGeometryIndexed` / `TriangleMeshGeometry` | N/A — these hold whatever topology their author/importer produced; there is no tessellator-side pole convention to weld | Not applicable |
| `BezierPatchGeometry` / `BilinearPatchGeometry` | Their `TessellateToMesh` grids have no analogous degenerate-row convention (no pole concept for a patch) | Not applicable |
| `BoxGeometry` / `ClippedPlaneGeometry` | Quad-only tessellation grids (no radial/spherical parametrization, hence no pole-like collapse point) — confirmed by reading both `TessellateToMesh` bodies | Not applicable |
| Cone / round-cone | No dedicated `ConeGeometry`/`RoundConeGeometry` class exists in `src/Library/Geometry` at this HEAD (grep found none) | Not applicable — nothing to check |

### Verification

Red (pre-fix, `RawIndexEdgeCounts`, detail=71): **284 boundary edges**
(matches the ledger's own cited evidence exactly). Green (post-fix):
**0 degenerate triangles**, raw-index boundary count drops to **142**
— exactly the seam-only baseline `ExactWeldEdgeCounts` reports on the
SAME (fixed) mesh, i.e. the pole's contribution is fully and only
eliminated. These per-mesh figures (284, 142, 0 degenerate) reproduce
at every stage of this row's history (Sphere-only, Sphere+CircularDisk,
and the final Sphere+CircularDisk+Ellipsoid file below) and are the same
for Sphere and Ellipsoid bit-for-bit, since both collapse to a pole via
the identical `sinPhi==0` construction.

**Pass/fail tallies, corrected (2026-09-17 review follow-up).** The
`32/0 (was 29/3)` figure originally recorded here described
`tests/PoleWeldingWatertightnessTest.cpp` at the Sphere-only stage of
this fix, BEFORE the CircularDisk and Ellipsoid sections existed (32
total checks = 29 + 3) — accurate for that narrower file, but the
ledger and `CLAUDE.md` went on to cite the SAME `29/3` figure again
later, as if it still described a Sphere-only revert against the
LARGER, later file that already included CircularDisk (42 checks) —
it does not; an isolated re-measurement against that 42-check file
reads a Sphere-only revert as 39/3, not 29/3 (`3` failed is stable
across both files — CircularDisk's own checks are what changes the
denominator — but the corresponding PASS count is not, and citing
`29/3` next to a `42/0` post-fix total invites exactly that
misreading). Re-measured here against the FINAL file (now including
Ellipsoid, 57 checks total), each via
`git checkout a4495f94 -- src/Library/Geometry/<File>.cpp && make -C build/make/rise -j8 all && make -C build/make/rise -j8 build-test/PoleWeldingWatertightnessTest && ./bin/tests/PoleWeldingWatertightnessTest`,
then `git checkout <slice-HEAD> -- <File>` to restore:

| Revert scope | Result | Note |
|---|---|---|
| None (post-fix, current HEAD) | **57/0** | supersedes the historical `42/0` |
| Sphere + CircularDisk + Ellipsoid (full, all three to `a4495f94`) | **47/10** | `57-3-4-3=47`: exactly additive, no interaction between the three fixes' checks |
| Sphere only | **54/3** | 284 boundary edges reproduces |
| CircularDisk only | **53/4** | 8 degenerate triangles reproduces |
| Ellipsoid only | **54/3** | same shape as Sphere-only (284 boundary edges) |

`tests/PoleWeldingWatertightnessTest.cpp` (final): **57/0**.

Regression gates (all run at this slice's final HEAD): `ProceduralMeshTest`
440/0, `TessellatedShapeDerivativesTest` PASS (0 shapes failed),
`GeometryShadingTangentTest` 12606/0, `DisplacedGeometryTest` all
passed, `GeometrySurfaceDerivativesTest` ALL TESTS PASSED. Render
comparison of `scenes/Tests/Geometry/displaced_sphere.RISEscene` at
256spp, `oidn` output not consulted (compared the raw `_hi.png`, not
`_hi_denoised.png`): fixed-vs-unfixed mean diff 0.027 / RMSE 1.50 vs a
same-code two-run noise floor of mean 0.019 / RMSE 0.94 — within noise.

---

## DL-20: patch-geometry flat curvature

### The bug, in one sentence

`BezierPatchGeometry` and `BilinearPatchGeometry` reported FLAT (H=0)
mean curvature at every hit for two independent reasons: their
`RayElementIntersection` (the actual per-hit callback, called with the
patch and the exact `(u, v)` already in hand) never stamped
`ri.derivatives` at all, so `ri.derivatives.valid` stayed at its default
`false`; and their standalone `ComputeSurfaceDerivatives(point, normal)`
override (reachable only from OUTSIDE the render path — no in-tree
caller reaches it for these two geometries) FABRICATED an arbitrary
tangent frame (`OrthonormalBasis3D::CreateFromW(objSpaceNormal)`) with
`dndu=dndv=(0,0,0)` and reported it as `valid=true` — a flat curvature
answer for a genuinely curved surface, presented as legitimate, worse
than the honest-absence convention `docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md`
§5.4 documents (`ExpressionPainter::PopulateCurvature`'s `curv`/`curvR`
default to 0 only when `derivatives.valid` is FALSE).

### The fix

1. **New second-derivative math** (`src/Library/Utilities/GeometricUtilities.h/.cpp`):
   `BezierPatchSecondDerivUU/UV/VV` (second derivatives of the cubic
   Bernstein basis, same Bernstein-sum convention as the existing
   `BezierPatchTangentU/V`) and `BilinearPatchSecondDerivUV` (the
   bilinear "saddle term" `pts[0]-pts[1]-pts[2]+pts[3]` — a bilinear
   surface's `d2P/du2` and `d2P/dv2` are IDENTICALLY zero, so this one
   constant vector is the entire second-derivative data the shape
   operator needs). Also promoted the pre-existing file-local
   `BilinearTanU`/`BilinearTanV` helpers to public
   `GeometricUtilities::BilinearPatchTangentU/V` wrappers so
   `BilinearPatchGeometry.cpp` can call them.

2. **New shared shape-operator helper**
   (`src/Library/Utilities/SurfaceCurvature.h`,
   `ShapeOperatorFromSecondDerivatives`): derives `dndu`/`dndv` (the raw
   Weingarten map, one level below `MeanCurvatureFromDerivatives`, which
   only ever sees `dndu`/`dndv` already built) from `dpdu`, `dpdv`, and
   the three second derivatives, via the standard quotient-rule
   derivation of `N = Cross(dpdu,dpdv)/|Cross(dpdu,dpdv)|`. Same relative
   degeneracy gate as `MeanCurvatureFromDerivatives` (they test the same
   quantity by Lagrange's identity: `|Cross(dpdu,dpdv)|^2 = E*G - F*F`).

3. **`BezierPatchGeometry::RayElementIntersection` /
   `BilinearPatchGeometry::RayElementIntersection`**: stamp
   `ri.derivatives.{dpdu,dpdv,dndu,dndv,valid}` at intersection time,
   where the patch and `(u, v)` are already known for free — UNGATED,
   matching the Sphere/Ellipsoid precedent ("publish the closed-form
   Weingarten map this primitive already knows; gate only `scaleHint`,
   which nothing but `curv` reads").

4. **`ComputeSurfaceDerivatives(point, normal)` conservative-reject**:
   both geometries' standalone point-only override now returns the
   default (`valid=false`) `SurfaceDerivatives` instead of the fabricated
   ONB/flat-curvature stand-in. A geometry holding MANY patches cannot
   invert a bare point back to "which patch, which of its `(u, v)`
   preimages" without an ambiguous, expensive re-solve the way the real
   ray-hit path gets both for free — reporting honest absence here is
   strictly better than the fabricated `valid=true` this replaces, and
   is the documented "conservative-reject" convention this row's own
   ledger evidence calls for.

### The flip case: conservative, not swapped

`BezierPatchGeometry::RayElementIntersection` flips its reported normal
to face the incoming ray on a back-face hit (Utah-teapot-style patches
have inconsistent CW/CCW winding). `dpdu`/`dpdv` are the patch's OWN,
un-flipped parametric tangents — by construction `Cross(dpdu, dpdv)`
points exactly the way `N` did BEFORE that flip. Pairing the un-flipped
`dpdu`/`dpdv` with a FLIPPED `ri.vNormal` would violate the standard
"(dpdu, dpdv, n) right-handed" invariant (confirmed by
`tests/GeometrySurfaceDerivativesTest.cpp`'s own generic 5-invariant
check, which caught exactly this when first tried). Re-deriving a
consistent frame needs a genuine reparametrization (e.g. swapping which
axis is called "u" and which "v", which ALSO swaps `Puu`/`Pvv` and would
disagree with `ptCoord`'s own, un-swapped `(u, v)` unless every other
`(u, v)`-consuming site were audited too) — out of this row's scope.
The fix therefore reports curvature ONLY on a hit that did NOT need the
ray-facing flip (`if (!bDidFlip)`), and leaves `ri.derivatives.valid`
honestly `false` on a flipped one — the same conservative-reject
convention as the point-only query, applied to a second, narrower case.
`BilinearPatchGeometry::RayElementIntersection` never flips its normal at
all, so no such gate is needed there.

### Sign convention

No new sign convention was introduced. `ShapeOperatorFromSecondDerivatives`
derives `N` from `Cross(dpdu, dpdv)` with no orientation correction of
its own; `SurfaceCurvature.h`'s existing, previously-pinned convention
("positive = convex, sign fixed by the orientation of the normal FIELD
`dndu`/`dndv` differentiate") applies unchanged — verified by this row's
own closed-form oracles (`tests/PatchCurvatureTest.cpp`) matching
production `H` to machine precision on both an exact bilinear saddle and
an exact (product-separable) Bezier saddle.

### What was NOT done, and why (conservative, per the recipe)

`ComputeAnalyticalDerivatives(uv, smoothing, ...)` — the SMS Newton
solver's OWN parametric-derivative query, separate from
`ComputeSurfaceDerivatives(point, normal)` — was NOT implemented for
either patch geometry. It is unrelated to the `curv` signal path this
row's recipe actually gates on (`ExpressionPainter::PopulateCurvature`
never calls it), no in-tree SMS scene exercises SMS on a bare
`bezier_patch_geometry`/`bilinear_patch_geometry` object today, and
implementing it correctly needs the SAME "smoothing" contract
`EllipsoidGeometry`/`DisplacedGeometry` already honour (out of scope:
those two files are owned by the concurrent `debt-prox` slice). Left as
a documented gap, not silently dropped.

### Sibling audit

**Bug pattern**: a parametric geometry with closed-form first
derivatives reports flat/fabricated second derivatives (curvature)
instead of either computing them or honestly reporting absence.

| Geometry | Overrides `ComputeSurfaceDerivatives` / stamps `ri.derivatives`? | Verdict |
|---|---|---|
| `SphereGeometry` | Yes, real closed form (pre-existing, Phase 1) | Not affected |
| `EllipsoidGeometry` | Yes, real closed form (pre-existing, Phase 1) | Not affected (also confirmed NOT the same pattern as DL-116's pole issue: its curvature path is independent of tessellation) |
| `TorusGeometry` / `CylinderGeometry` | Yes, real closed form (pre-existing, Phase 1 — confirmed by grep, not modified this slice) | Not affected |
| `BoxGeometry` / `CircularDiskGeometry` / `ClippedPlaneGeometry` / `InfinitePlaneGeometry` | Flat by construction (genuinely planar primitives) | Not a bug — `curv` reading 0 there is physically correct |
| `TriangleMeshGeometry(Indexed)` | Topology-derived (per-triangle averaged second derivatives from adjacent faces) | Pre-existing, unrelated mechanism, not modified |
| `BezierPatchGeometry` / `BilinearPatchGeometry` | **FIXED**, this row | — |
| `DisplacedGeometry` | Delegates to its internal tessellated `TriangleMeshGeometryIndexed` | Out of scope (owned by `debt-prox`); unaffected either way since it never wraps a Bezier/Bilinear patch's OWN curvature path (it re-tessellates and uses the mesh's topology-derived curvature instead) |
| `SDFGeometry` | Direct-curvature path (`div n_hat` by finite difference), independent mechanism | Not affected |

### Verification

Red (pre-fix): `tests/PatchCurvatureTest.cpp` did not exist; the
PRE-EXISTING `tests/GeometrySurfaceDerivativesTest.cpp` already asserted
`sd.valid` via the point-only query for these two geometries and PASSED
before this row (green-on-a-lie, since the fabricated ONB was reported
`valid=true`) — the real red-proof is `PatchCurvatureTest.cpp`'s own
build against a temporarily-reverted library: `ri.derivatives.valid` was
`false` at every hit (no signal-stamping block existed), so all three
fixtures' production-vs-oracle comparisons were unreachable (the tests
would need `SKIP`, not `PASS`, on unfixed code — confirmed by reverting
`BezierPatchGeometry.cpp`/`BilinearPatchGeometry.cpp`/
`GeometricUtilities.{h,cpp}`/`SurfaceCurvature.h` and re-running).

Green (post-fix), `tests/PatchCurvatureTest.cpp`: **19/0** —

- Bilinear hyperbolic paraboloid (exact closed form): `H_production =
  H_oracle = -0.136083` at `(u,v)=(0.25,0.75)`, `|diff|=0`.
- Bezier saddle (exact closed form, product-separable control grid):
  `H_production = H_oracle = -0.019564` at `(u,v)=(0.7,0.3)`,
  `|diff|=6.9e-18`.
- Bezier dome (finite-difference oracle, `h=1e-4`, Clairaut
  cross-check `|PuvA-PuvB|=1.3e-12`): `H_production = H_oracle(FD) =
  0.328596`, `|diff|=6.4e-14`, well inside the stated 1% tolerance.

Regression gates: `GeometrySurfaceDerivativesTest` ALL TESTS PASSED
(two pre-existing fixtures were corrected in the same commit — see
"Pre-existing test fixture corrections" below), `SignalIntegratorConsistencyTest`,
`ProximitySignalTest` 491/0, `CstDeriveGoldenTest`, `SourceHygieneTest` —
all run at this slice's final HEAD, clean rebuild, zero warnings.

### Pre-existing test fixture corrections (found while gating, not new bugs)

`tests/GeometrySurfaceDerivativesTest.cpp` had two latent problems in its
own Bezier/Bilinear fixtures, both invisible until `ComputeSurfaceDerivatives`
stopped fabricating a `valid=true` answer:

1. **The bilinear fixture was secretly planar.** Its four corners
   `(0,0,0),(1,0,0.3),(0,1,0.3),(1,1,0.6)` have `pts[3].z` EXACTLY equal
   to `pts[1].z+pts[2].z-pts[0].z`, i.e. the bilinear "saddle" term `D`
   was `(0,0,0)` — despite the fixture's own "bent corner" / "corners not
   coplanar" comments, it was a flat, tilted quad. `dndu=dndv=0` was
   therefore the CORRECT answer for it, not evidence of a stub. Fixed by
   changing `pts[3].z` to `1.0` (`D.z = 0.4 != 0`, a genuine saddle).
2. **The Bezier fixture triggered `BezierPatchGeometry`'s ray-facing
   flip**, landing on the conservative-reject branch above (documented,
   not a bug) rather than exercising the real curvature path. Fixed by
   storing the control grid transposed (`patch.c[k].pts[j]` instead of
   `patch.c[j].pts[k]`), which reverses the patch's natural winding to
   already face the fixture's fixed camera position with no flip needed.

Both fixtures' own `CheckInvariantsAt` call sites were also switched from
the point-only `g->ComputeSurfaceDerivatives(...)` query (now honestly
`valid=false`, per this row's design) to reading `ri.derivatives`
directly off the real ray hit — exactly the migration the file's own
pre-existing `TODO(SMS stage 1.1)` comments anticipated ("This will be
fixed... by populating surface derivatives during IntersectRay... At
that point the assertions below should be uncommented").
