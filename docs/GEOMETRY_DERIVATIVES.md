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
   sign without inspecting the specific geometry.
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

## Consumer expectations (SMS `ManifoldSolver`)

Given the conventions above, the solver:

1. Reads `sd.dpdu, sd.dpdv, sd.dndu, sd.dndv` from the geometry.
2. Transforms them to world space via the object transform (tangent
   directions transform by the inverse-transpose for correctness, but
   for a rigid transform the upper-left 3×3 of the transform works).
3. Projects `dpdu` into the tangent plane (removes any tiny n-component
   from numerical noise), then Gram-Schmidts `dpdv` against `dpdu`.
4. Computes `ds_du`, `dt_du`, etc. via the Cycles product-rule formula
   ([ManifoldSolver.cpp](../src/Library/Utilities/ManifoldSolver.cpp) in
   `BuildJacobian`).
5. Includes the per-vertex metric `|dpdu| × |dpdv|` in the final metric
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
