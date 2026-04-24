# Specular Manifold Sampling (SMS)

Implementation notes for RISE's SMS solver, based on Zeltner, Georgiev,
and Jakob, "Specular Manifold Sampling," SIGGRAPH 2020.

## Overview

SMS finds valid light transport paths through chains of perfect-specular
(delta) surfaces by solving a nonlinear constraint system via Newton
iteration.  It fills the gap that standard BDPT cannot cover: caustic
paths where light refracts/reflects through glass or mirrors before
hitting a diffuse receiver.

## Key Files

| File | Role |
|------|------|
| `src/Library/Utilities/ManifoldSolver.h` | Solver interface, config, result structs |
| `src/Library/Utilities/ManifoldSolver.cpp` | Full Newton solver, constraint, Jacobian, contribution eval |
| `src/Library/Utilities/SMSPhotonMap.h/cpp` | Photon-aided seeding; kd-tree of proven-good SMS seeds queried by `ManifoldSolver::EvaluateAtShadingPoint` |
| `src/Library/Shaders/SMSShaderOp.h/cpp` | Unidirectional PT integration |
| `src/Library/Shaders/BDPTIntegrator.cpp` | BDPT integration (`EvaluateSMSStrategies`) |
| `src/Library/Rendering/BDPTPelRasterizer.cpp` | Rendering loop: SMS added to BDPT result |
| `src/Library/Interfaces/SpecularInfo.h` | Material query for specular properties |

## Algorithm Pipeline

For each pixel sample:

1. **Light sampling** — pick a point on a luminaire.
2. **Seed chain** — trace a ray from the shading point toward the light,
   collecting intersections with specular objects.  The ray follows
   physical refraction/reflection at each surface (Snell's law),
   naturally discovering multi-object chains.
3. **Newton iteration** — adjust vertex positions on specular surfaces
   until the half-vector constraint `C(x) = 0` is satisfied at every
   vertex.  Uses an analytical block-tridiagonal Jacobian with
   backtracking line search.
4. **Contribution evaluation** — Fresnel transmittance, geometric
   coupling, Jacobian determinant, BSDF at the shading point,
   emitted radiance, and light-sampling PDF are assembled into the
   final path contribution.
5. **Visibility** — shadow rays test the two external segments
   (shading point to first specular vertex, last specular vertex
   to light).

## Constraint Formulations

Two constraint formulations are implemented:

### Half-vector projection (used for Newton iteration)

At each specular vertex, construct the generalized half-vector:
- Reflection: `h = wi + wo`
- Refraction: `h = -(wi + eta_eff * wo)`

Normalize h and project onto the tangent plane:
```
C[2i]   = dot(s, h)
C[2i+1] = dot(t, h)
```

When Snell's law / reflection law is exactly satisfied, h is parallel
to the surface normal and both projections are zero.  The analytical
Jacobian for this formulation (`BuildJacobian`) is well-conditioned
for seeds near the solution, giving 82% convergence on displaced-mesh
test scenes.

### Angle-difference (available but not currently used for Newton)

Converts actual and specular-scattered outgoing directions to spherical
coordinates (theta, phi) in the local tangent frame and computes:
```
C[2i]   = theta_actual - theta_specular
C[2i+1] = wrap_to_pi(phi_actual - phi_specular)
```

Implemented as `EvaluateConstraintAtVertex` with a numerical Jacobian
(`BuildJacobianNumerical`).  The paper argues this formulation has
larger convergence basins for distant initial guesses, but empirical
testing in RISE showed worse convergence (42%) than the half-vector
formulation with its analytical Jacobian.  The angle-difference
infrastructure is retained for potential future use with random
seed sampling, where the distant-seed advantage would apply.

## SMS and BDPT: MIS Analysis

**Standard BDPT and SMS occupy disjoint path spaces.  No cross-MIS is
needed between them for perfect-specular (delta) materials.**

### Why no overlap exists

SMS paths traverse a chain of delta (specular) vertices between a
diffuse surface and a light:

```
Camera -> ... -> Diffuse -> [Specular_1 -> ... -> Specular_k] -> Light
```

BDPT connection strategies explicitly skip delta vertices — the
MIS weight walk skips any vertex where `isDelta == true`, and
connections are only formed between non-delta endpoints.  A light
subpath that randomly scatters through specular surfaces has zero
probability of hitting the exact delta direction, so BDPT cannot
generate the same path.

Therefore:
- **No double-counting** — simple addition of SMS and BDPT
  contributions is correct.
- **No cross-MIS denominator terms** — neither strategy can
  produce the other's paths.
- The current implementation (separate `EvaluateSMSStrategies`
  added to `EvaluateAllStrategies` results) is mathematically
  sound.

### When cross-MIS would be needed

If SMS is extended to **glossy (non-delta) specular** materials,
the path spaces would overlap.  BDPT could connect through a
glossy vertex with nonzero probability, and SMS would also find
paths through the same vertex.  In that case:

1. SMS paths would need forward and reverse PDFs expressible in
   the BDPT vertex framework.
2. The MIS weight walk would need an additional term for the
   SMS strategy at each non-delta eye vertex.
3. The SMS internal PDF estimation would need to include the
   probability of the standard BDPT strategy finding the same
   path.

This is left as a future exercise; the current delta-only SMS
does not require it.

### Internal SMS MIS (biased vs. unbiased)

Within SMS itself, multiple valid specular paths may exist for
the same (shading point, light point) pair (e.g., multiple caustic
lobes on a curved surface).  The solver finds one solution per
sample via Newton iteration.

- **Biased mode** (`sms_biased TRUE`, the default): `pdf = 1.0`.
  Each solution is weighted equally.  If multiple solutions exist,
  contributions are proportionally too large.  This is fast (no
  extra solves) and acceptable when the scene has few multi-solution
  configurations.

- **Unbiased mode** (`sms_biased FALSE`): Uses Bernoulli trials to
  estimate the probability of converging to each solution.  Requires
  up to `sms_bernoulli_trials` additional Newton solves per successful
  path, which is expensive but removes the multi-solution bias.

## Scene Syntax

```
pathtracing_pel_rasterizer
{
    sms_enabled           TRUE
    sms_max_iterations    30       # Newton iteration limit
    sms_threshold         1e-4    # Convergence threshold on ||C||
    sms_max_chain_depth   10       # Max specular vertices in chain
    sms_biased            TRUE     # Skip Bernoulli PDF estimation
    sms_bernoulli_trials  100      # Trials for unbiased mode
}
```

Same parameters apply to `pathtracing_spectral_rasterizer`.

## Implementation Notes

### Surface derivatives

The solver uses central finite differences (`eps = 5e-4`) to compute
dpdu, dpdv, dndu, dndv at each specular vertex.  This is geometry-
agnostic (works with any shape that supports ray intersection) but
introduces O(eps^2) error and requires 4 probe ray casts per vertex.
Tangent vectors are projected into the tangent plane and
Gram-Schmidt orthogonalized for Jacobian consistency.

### Jacobian determinant and geometric coupling

The SMS contribution weight is (Zeltner 2020, Eq. 15-17):

```
weight = cos(θ_x) × cos(θ_y) × ∏_j cos(θ_in_j) / (p_A(y) × |det(∂C/∂x)|)
```

The Jacobian determinant `|det(∂C/∂x)|` alone provides the measure
conversion from light-area density to path-space density through
the specular chain.  The direction derivatives in `BuildJacobian`
(`dwi/du ∝ 1/dist_i`, `dwo/du ∝ 1/dist_o`) encode the 1/dist²
geometric coupling factors.  The explicit factors are:

- `cos(θ_x)`: cosine at the shading point
- `cos(θ_y)`: cosine at the light
- `∏ cos(θ_in_j)`: incoming cosines at specular vertices (area →
  solid-angle conversion at each surface)

The 1/dist² terms are NOT separate factors — they are inside the
Jacobian.  An earlier version computed `chainGeom / jacobianDet`
where `chainGeom = ∏[cos/dist²] × 1/dist²`, double-counting the
distance scaling.  This was stable on flat surfaces but caused
fireflies on displaced meshes where the Jacobian's internal
distance scaling diverged from the world-space distances.

The Jacobian `det(∂C/∂(u,v))` is in surface-parameter coordinates.
The conversion to tangent-plane world coordinates divides by the
surface metric (product of tangent vector magnitudes):

```
|det(∂C/∂x)| = |det(∂C/∂(u,v))| / ∏_i |dpdu_i| × |dpdv_i|
```

### Minimum segment distance

Chains with very short inter-vertex segments (< 0.05 world units)
are rejected.  The surface derivatives use finite-difference probes
with radius ~0.01; when two vertices are closer than ~5× this radius,
their derivative stencils overlap and the Jacobian determinant becomes
unreliable.  This primarily affects k=2 paths that graze the edge of
a displaced slab (entering and exiting very close together).  Improving
the finite-difference accuracy (adaptive probe radius, analytic
derivatives for known geometry types) would lower this threshold.

### Visibility bias

Shadow rays from specular vertices on displaced meshes can be
blocked by nearby displacement bumps.  The solver uses a normal-
direction bias of 5e-2 world units to clear local geometry before
testing for external occlusion.  Adjust if displacement amplitude
is significantly larger or smaller.

### Fresnel

Exact dielectric Fresnel (s- and p-polarization averaged) is used
for the chain throughput, matching the `DielectricFresnel` function
in the SSS materials.

## References

- Zeltner, Georgiev, Jakob. "Specular Manifold Sampling."
  SIGGRAPH 2020.
- Hanika, Droske, Fascione. "Manifold Next Event Estimation."
  CGF 2015.
- Fan et al. "Manifold Path Guiding." SIGGRAPH Asia 2023.
