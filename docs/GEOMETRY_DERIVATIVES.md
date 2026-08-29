# Surface Derivative Conventions

Contract for `IGeometry::ComputeSurfaceDerivatives` and how consumers (SMS
`ManifoldSolver`, future derivative-aware code) must interpret its output.

## The contract

```cpp
struct SurfaceDerivatives {
    Vector3 dpdu;   // position partial derivative w.r.t. surface parameter u
    Vector3 dpdv;   // position partial derivative w.r.t. surface parameter v
    Vector3 dndu;   // normal partial derivative w.r.t. u
    Vector3 dndv;   // normal partial derivative w.r.t. v
    Point2  uv;     // surface parameters at this point
    bool    valid;  // true on success
};
```

**Inputs.** Object-space intersection point and object-space normal. The normal
must be consistent with the one `IntersectRay` fills into
`RayIntersectionGeometric::vNormal` (object-space, interpolated for smooth
meshes, face for flat meshes).

**Invariants every implementation must satisfy.**

1. **Tangency.** `dpdu · n ≈ 0` and `dpdv · n ≈ 0`. The position derivatives
   live in the tangent plane.
2. **Normal stays unit.** `dndu · n ≈ 0` and `dndv · n ≈ 0`, because `|n| = 1`
   identically implies `2·n·(dn/d*) = 0`.
3. **Handedness.** `(dpdu × dpdv) · n > 0`. The local `(dpdu, dpdv, n)` frame
   is **right-handed** with the outward normal. Consumers may rely on this
   sign without inspecting the specific geometry **for any per-`IGeometry`
   output** — every `IGeometry` implementation must satisfy it at every
   point; the one documented exception is downstream of them, at the
   CSG-composition layer, on a **final `CSGObject` subtraction cavity-wall
   hit** (a composition-layer negation, not a per-geometry one) — see
   "CSG-subtraction exception" under the Handedness audit below.
4. **Finite values.** No NaN / Inf at any valid surface point. Degenerate
   parameter values (poles, caps) must return `valid = true` with a
   well-defined tangent frame (pick an arbitrary but deterministic frame
   at a singular point — e.g. `(1,0,0)`/`(0,0,1)`).
5. **Finite-difference agreement.** For small `eps`, a central FD of
   position/normal in tangent directions must match the analytical result
   to O(eps²). This is the load-bearing test per geometry.

## Magnitudes and parameter scaling

Implementations are **free to pick their own natural parameterization**. The
Jacobian-determinant consumer (SMS `ManifoldSolver`) converts from parameter
to world tangent-plane coordinates by dividing by `|dpdu| × |dpdv|` per
vertex, so the absolute magnitude does not matter as long as it is consistent
with the derivative scale.

Do **not** artificially unit-normalize `dpdu` / `dpdv` if the natural
parameterization has non-unit metric. Doing so would corrupt the
metric-conversion step. Specifically:

- A parametric surface like a sphere should return `|dpdu| = r·sin θ` (NOT
  unit) — the metric is `r sin θ` for the azimuth parameter and `r` for the
  polar parameter.
- A flat unit-metric surface (disk, box face, plane) returns unit vectors —
  the parameterization IS world coordinates.

## Per-geometry conventions

| Geometry | `u` | `v` | `|dpdu|` | `|dpdv|` | `u` range | `v` range | Status |
|---|---|---|---|---|---|---|---|
| `SphereGeometry` | φ azimuth | θ polar from +Y | `r·sin θ` | `r` | `(-π, π]` | `[0, π]` | ✅ curvature-aware |
| `TorusGeometry` | v on cross-section (tube) | u around major axis (ring) | `r` | `R + r·cos v_tube` | `(-π, π]` | `(-π, π]` | ✅ curvature-aware (swapped from natural order for right-handedness) |
| `CylinderGeometry` | axial coord | θ around axis | `1` | `r` | cylinder height | `(-π, π]` | ✅ curvature-aware (swapped from natural order for right-handedness) |
| `EllipsoidGeometry` | φ azimuth | θ polar | non-trivial | non-trivial | `(-π, π]` | `[0, π]` | ✅ curvature-aware (gradient-based normal) |
| `BoxGeometry` | face-local u | face-local v | `1` | `1` | `[0, 1]` per face | `[0, 1]` per face | ✅ correctly flat |
| `CircularDiskGeometry` | world X on face | world Y on face | `1` | `1` | `[-1, 1]` scaled by radius | `[-1, 1]` | ✅ correctly flat |
| `ClippedPlaneGeometry` | normalized along edge 0→1 | n × dpdu | `1` | `1` | barycentric-like | barycentric-like | ✅ correctly flat |
| `InfinitePlaneGeometry` | world X | world Y | `1` | `1` | `(-∞, ∞)` | `(-∞, ∞)` | ✅ correctly flat |
| `TriangleMeshGeometry` | stored per-vertex UV | stored per-vertex UV | set by UV-Jacobian inversion | set by UV-Jacobian inversion | `[0, 1]` typical | `[0, 1]` typical | ✅ implemented (UV-Jacobian invert + tangent-plane projection, barycentric-edge fallback) |
| `TriangleMeshGeometryIndexed` | stored per-vertex UV | stored per-vertex UV | set by UV-Jacobian inversion | set by UV-Jacobian inversion | `[0, 1]` typical | `[0, 1]` typical | ✅ implemented (shared with non-indexed via `ComputeTriangleDerivatives`) |
| `BilinearPatchGeometry` | patch u | patch v | non-trivial | non-trivial | `[0, 1]` | `[0, 1]` | ❌ **STUB** — flat tangent frame only |
| `BezierPatchGeometry` | patch u | patch v | non-trivial | non-trivial | `[0, 1]` | `[0, 1]` | ❌ **STUB** — flat tangent frame only (analytical Bezier intersection supplies tangents via `GeometricUtilities::BezierPatchTangentU/V` but they are not wired into `ComputeSurfaceDerivatives` yet) |
| `DisplacedGeometry` | forwards to wrapped mesh | forwards | forwards | forwards | forwards | forwards | 🔁 forwarder |

### Handedness audit

`(dpdu × dpdv) · n` must be positive at every non-degenerate point.  The torus and cylinder originally returned left-handed frames under their natural `(ring, cross-section)` / `(θ, axial)` parameterisation.  **Resolved**: both implementations swap their natural `u`/`v` ordering so that `(dpdu × dpdv) · n > 0` at every point.  See the code comments in [TorusGeometry::ComputeSurfaceDerivatives](../src/Library/Geometry/TorusGeometry.cpp) and [CylinderGeometry::ComputeSurfaceDerivatives](../src/Library/Geometry/CylinderGeometry.cpp) — the per-axis cylinder permutation is worked out case-by-case so that the swap produces right-handed frames for x-, y-, and z-axis cylinders.

Current sign check at representative non-degenerate points (all must be **+**):

- **SphereGeometry**, point at `(r, 0, 0)`, n=`(1,0,0)`: `dpdu = (0,0,r)`, `dpdv = (0,-r,0)` → cross dot n = **+r²** ✅
- **TorusGeometry** (post-swap), u=tube v=ring, u=π/2 v=0 (top of ring): right-handed by construction ✅
- **CylinderGeometry** (post-swap, per-axis), u=axial v=θ: right-handed by construction ✅
- **BoxGeometry**, +X face: **+1** ✅
- **EllipsoidGeometry**: **+** (gradient-based normal, same handedness as sphere) ✅
- **CircularDiskGeometry**, z-axis: **+1** ✅
- **ClippedPlaneGeometry**: `dpdv = n × dpdu` by construction, cross dot n = **+** ✅
- **InfinitePlaneGeometry**: **+1** ✅

Handedness is now uniform across all implemented geometries.  Consumers that care about orientation sign (texture-space normal mapping, future glossy SMS) can rely on `(dpdu × dpdv) · n > 0` without per-geometry conditioning.

### CSG-subtraction exception (invariant 3 does not hold on a carved cavity wall)

`CSG_SUBTRACTION`'s three entry-flip branches ([CSGObject.cpp](../src/Library/Objects/CSGObject.cpp), P1-1) report the carving operand B's normal NEGATED (`ri.geometric.vNormal = -riObjB.geometric.vNormal`) — the ray is leaving A's solid into the void B carved out of it, so the reported shading normal points the opposite way from B's own outward normal. `dpdu`/`dpdv` are left UNCHANGED (still B's own, un-negated, object-space tangent frame at that surface point) — flipping them would reparameterize the surface and break UV-keyed re-queries such as `ManifoldSolver`'s SMS two-stage-solver `ComputeAnalyticalDerivatives(vertex.uv)` path, which expects `uv` to keep meaning the same thing regardless of which CSG operand exposed the point. `dndu`/`dndv` ARE negated (alongside `vNormal`), because they must stay derivatives of whichever normal FIELD is actually being reported (see the `CSGObject::IntersectRay` bullet above).

Net effect: on these cavity-wall hits, `(dpdu × dpdv) · n` is **negative** — the local `(dpdu, dpdv, n)` frame is intentionally LEFT-handed, because `n` flipped and `dpdu`/`dpdv` deliberately did not. This is not a bug to fix; it's the direct consequence of leaving `dpdu`/`dpdv` at their un-reparameterized values while reporting the physically-correct (into-the-cavity) shading normal. Weingarten-map / signed-curvature consumers (`e = dndu · dpdu`, etc.) stay self-consistent under this convention — both `n` and `dndu`/`dndv` flipped together, so the signed curvature correctly reports the concave cavity (opposite sign from the same mesh surface hit as a stand-alone convex object) — but any consumer that assumes invariant 3 unconditionally (e.g. to infer front/back-facing from the frame's handedness alone, independent of `n`) must special-case a CSG-subtraction cavity wall.

## World-space transform

`IGeometry::ComputeSurfaceDerivatives` / `IGeometry::ComputeAnalyticalDerivatives`
and a mesh geometry's own `IntersectRay` all report `dpdu, dpdv, dndu, dndv`
in OBJECT space, per the contract above.  Promotion to WORLD space happens at
exactly three call sites, each applying **its own** object transform once:

- `Object::IntersectRay`'s derivatives block
  ([Object.cpp](../src/Library/Objects/Object.cpp), immediately after the
  block that promotes `vNormal`/`vGeomNormal`) — fires when the wrapped
  geometry populated `ri.geometric.derivatives.valid` during its own
  `IntersectRay` (currently only `TriangleMeshGeometry` /
  `TriangleMeshGeometryIndexed`; analytic primitives like `SphereGeometry`
  never set it there).
- `Object::ComputeAnalyticalDerivatives` ([Object.cpp](../src/Library/Objects/Object.cpp))
  — the `(u, v)`-keyed analytical query used by the SMS two-stage solver;
  wraps `IGeometry::ComputeAnalyticalDerivatives` (currently implemented by
  `EllipsoidGeometry` and `DisplacedGeometry`, which forwards to its base).
- `CSGObject::IntersectRay`'s derivatives block
  ([CSGObject.cpp](../src/Library/Objects/CSGObject.cpp)) — mirrors
  `Object::IntersectRay`'s block exactly, applying THIS CSG level's own
  transform to whatever the operand below (a plain `Object`, or a
  further-nested `CSGObject`) already promoted one level.  CSG nesting
  composes correctly because each level applies its own transform to the
  level-local normal field in turn — the same pattern the `vNormal` /
  `vTangent` promotions in that function already use — **provided
  `ri.geometric.derivatives` is already paired with the same normal FIELD
  `ri.geometric.vNormal` reports at the point this block runs**. That
  pairing is not automatic: `CSG_SUBTRACTION`'s three entry-flip branches
  (the two "we're inside one/both operands" branches and the "outside
  both, B's entry is the visible boundary" branch) report a NEGATED
  operand normal (`ri.geometric.vNormal = -riObjB.geometric.vNormal`) while
  starting from a whole-record `ri = riObjB` copy that still carries B's
  UN-negated `dndu`/`dndv` — so each of those three branches explicitly
  negates `ri.geometric.derivatives.dndu`/`dndv` to re-pair them with the
  negated normal (P1-1, 2026-08-29) before this promotion block ever runs.
  Without that per-branch negation the composition claim above would be
  false for every subtraction cavity wall a mesh operand carves, not just
  imprecise. See the CSG-subtraction exception in the Handedness section
  below for the follow-on frame-orientation consequence.

**The two field kinds transform differently, and dndu/dndv are NOT a plain
inverse-transpose:**

- `dpdu`, `dpdv` are tangent VECTORS (directions along the surface) — the
  forward transform `m_mxFinalTrans`'s linear part, same as `vTangent`.
- The normal `n` is transformed by the inverse-transpose
  `m_mxInvTranspose`'s linear part and then RENORMALIZED (`n_w =
  normalize(M⁻ᵀ n_obj)`) — this renormalization is the detail that matters
  for what comes next.
- `dndu`, `dndv` are derivatives of that RENORMALIZED normal FIELD, not of
  the raw inverse-transpose output.  Differentiating a quotient (the
  normalize) rather than just the linear map gives the quotient rule:
  ```
  n_w(u,v)  = M⁻ᵀ n_obj(u,v) / ‖M⁻ᵀ n_obj(u,v)‖
  dn_w/du   = (I − n_w n_wᵀ) · (M⁻ᵀ dndu_obj) / ‖M⁻ᵀ n_obj‖
  dn_w/dv   = (I − n_w n_wᵀ) · (M⁻ᵀ dndv_obj) / ‖M⁻ᵀ n_obj‖
  ```
  A plain inverse-transpose (skipping both the projection and the divide by
  `‖M⁻ᵀ n_obj‖`) is correct only for a RIGID transform, where
  `‖M⁻ᵀ n_obj‖ == 1` identically and the projection removes a component
  that's already ~0 by invariant 2 above (`dndu · n ≈ 0`).  Under any
  non-rigid transform (scale, non-uniform stretch, shear) it under-corrects:
  for a uniform scale `s`, the plain inverse-transpose gives world mean
  curvature `H_obj / s²` instead of the correct `H_obj / s` (2026-08-29 fix;
  see [tests/GeometryUVRoundtripTest.cpp](../tests/GeometryUVRoundtripTest.cpp)'s
  `TestObjectWorldDerivatives()` for the regression coverage, including a
  direct check that the old `s²` behaviour is excluded).
- The degenerate case (`‖M⁻ᵀ n_obj‖` below `NEARZERO`, i.e. the transform is
  singular along the normal direction) has no well-defined unit world
  normal to differentiate against; both fixed call sites mark the
  derivatives invalid there rather than dividing by ~0.

## Consumer expectations (SMS `ManifoldSolver`)

Given the conventions above, the solver:

1. Reads `sd.dpdu, sd.dpdv, sd.dndu, sd.dndv` — already in WORLD space by
   the time the solver sees them.  The solver itself does NOT transform
   these fields; the promotion happened in `Object::IntersectRay` /
   `Object::ComputeAnalyticalDerivatives` / `CSGObject::IntersectRay` per
   the "World-space transform" section above.  (The solver also has its
   own geometry-agnostic central-finite-difference fallback path — see
   [docs/SMS.md](SMS.md) "Surface derivatives" — for geometries that don't
   populate `derivatives.valid` and don't implement
   `ComputeAnalyticalDerivatives`; that path never touches `M⁻ᵀ` at all,
   since it differences world-space ray-cast hits directly.)
2. Projects `dpdu` into the tangent plane (removes any tiny n-component
   from numerical noise), then Gram-Schmidts `dpdv` against `dpdu`
   (`OrthonormalizeTangentFrame`), which also applies the matching
   correction to `dndv` so it stays consistent with the re-orthogonalized
   `v` direction.
3. Computes `ds_du`, `dt_du`, etc. via the Cycles product-rule formula
   ([ManifoldSolver.cpp](../src/Library/Utilities/ManifoldSolver.cpp) in
   `BuildJacobian`).
4. Includes the per-vertex metric `|dpdu| × |dpdv|` in the final metric
   conversion from parameter-space Jacobian to world tangent-plane
   Jacobian.

## Test battery requirements

Every `ComputeSurfaceDerivatives` implementation must pass:

1. **Finite-difference agreement**: 5+ test points per geometry, analytical
   vs `(pos(u+ε,v) - pos(u-ε,v)) / 2ε`, tolerance 1e-3 with ε=1e-4.
2. **Tangency**: `|dpdu·n| < 1e-6` and `|dpdv·n| < 1e-6`.
3. **Normal unit preservation**: `|dndu·n| < 1e-6`, `|dndv·n| < 1e-6`.
4. **Handedness**: `(dpdu × dpdv)·n > 0` at a representative point.
5. **Validity**: `sd.valid == true` for surface-interior points.
6. **Degenerate-point graceful handling**: No NaN / Inf at poles / caps.

See [tests/GeometrySurfaceDerivativesTest.cpp](../tests/GeometrySurfaceDerivativesTest.cpp)
(created in Stage 1) for the concrete test harness.
