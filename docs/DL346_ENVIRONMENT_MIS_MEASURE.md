# Environment endpoints and selected-light MIS

DL-346 concerns an environment-lit scattering medium. On the current baseline,
three salted, interleaved trials gave BDPT/PT 0.95421; a conservative white
furnace gave BDPT 0.95204 against the independent unit-radiance oracle. Vacuum
and absorption controls did not show that deficit. These observations identify
the transport regime; they do not define a correction factor.

## Common path measure

Use sky solid angle `dω` at an environment endpoint, surface area `dA` at a
surface, and volume `dV` at a collision. Let `q` select the environment,
`pω(w)` sample its direction, and `pD = 1/(πR²)` sample a transverse coordinate
on the bounding disc. Conditioned on `w`, emitted rays are parallel. Therefore
the first finite target has density `pD J(x)`, where `J=|n·w|` for a surface
and `J=σt` for a collision. There is no inverse distance squared in this
parallel projection. Subsequent finite edges retain their ordinary
`pω |n·w|/r²` or `pω σt/r²` densities. Transmittance factors common to the
existing competing MIS partners remain under the established convention.

The environment root marginal is `q pω`; its traced joint emission density
is `q pω pD`. An eye escape stores its outgoing scatter density directly in
solid angle. Reverse sampling into that root also preserves solid angle.
For an eye escape, the NEE alternative installs `q pω` at the root and
`pD J(pred)` at the preceding vertex. For NEE, the reverse root density is
the eye's scatter angular density, and the reverse eye-end density is
`pD J(eyeEnd)`. Generated light paths use those same factors. A synthetic
sphere/disc position defines geometry and a direction; it cannot introduce
an extra finite-distance endpoint Jacobian.

The root's guiding throughput is `Le/(q pω)`, so `throughput*pdfFwd` still
recovers `Le`. Traced emission throughput remains `Le/(q pω pD)`; no radiance
scale changes. Camera-visible sky has no admissible NEE or root-to-camera
connection in these integrators and receives weight one.

## VCM and light selection

VCM keeps the balance recurrence. For an environment root,
`dVCM = (q pω)/(q pω pD) = 1/pD` and `dVC = 1/(q pω pD)`.
Its first geometric update divides by `J` and skips `r²`. Reconstruction of
an angular root density skips the finite-edge Jacobian. NEE uses direct
angular density `pω` and conditional target factor `pD J`; eye escape uses
selected marginal `q pω` and selected joint emission `q pω pD`.

Light selection cancels between alternatives that both choose the same
light, but does **not** cancel against an eye-generated emitter hit. The
finite-light sibling therefore also retains its selected joint emission
density in `dVC` and in the eye-hit comparison. For example, the adjacent
NEE/eye-hit ratio is `q pPosition/(pBSDF |nLight·w|/r²)`; dividing the entire
ratio by `q` would remove a real selection event. The former geometric-only
selection convention was inconsistent once the environment measure became
consistent. A mixed environment/mesh control exposed the mismatch: VCM/PT
0.9493 before the joint-selection correction, 0.9989 after it.

## Consumers and evidence

Pel and NM share the templated BDPT generators and connection code. HWSS
uses NM bookkeeping and replays companion spectral throughput. MLT consumes
the same BDPT integrator. VCM consumes the generated vertices and reconstructs
their angular PDFs in its light, eye, medium, surface, and BSSRDF-entry
recurrence paths. OpenPGL recovers emitted radiance from the unchanged
`root.throughput*root.pdfFwd` identity. Delta/null-boundary transparency,
medium caps, SSS entry density, and the power-2 BDPT versus balance VCM
heuristic distinction remain in their existing formulations.

`MediumInsideOutsideInvariantTest` inspects real Pel/NM light paths under
nonunit environment selection. Independent projected-disc and full joint
probability products check root/first-target densities and VCM recurrence
ratios. An independent full-strategy probability product checks every BDPT
split and the partition of unity. Salted render controls cover vacuum,
scattering, absorption, inside/outside conservative furnaces, an internal
light, Beer-Lambert absorption, and the one-scatter integral in a sphere.
The public spectral variants use both NM and HWSS rasterizers.

For the centered radius-2 sphere with isotropic scattering, `σs=σt=0.7`,
and at most one collision, the reference is

`exp(-σR) + σ ∫₀ᴿ exp(-σt) (1/2) ∫₋₁¹ exp(-σℓ(t,μ)) dμ dt`,

where `ℓ=-tμ+sqrt(R²-t²(1-μ²))`. Independent midpoint quadrature checks its
own refinement before comparing transport. Absorption alone is
`exp(-σa R)`. Zero absorption and unit boundary/environment radiance give
the exact multiple-scatter furnace solution `L=1`.

## Regression and precision checks

The final tests were clean-built and relinked against both baseline and
corrected production. The baseline fails seven generated-density checks
and the unit-radiance furnace; its six-trial Pel furnace mean is 0.949079
with Student 95% half-width 0.002670. The corrected mean is 0.999823 with
half-width 0.004910, retaining the same 2% physical band. NM and HWSS have
the same baseline deficit and pass the corrected eight-control matrix.

Real generated paths use environment selection probability 0.796392.
Pel and NM first-target density errors are zero; VCM joint recurrence
errors are below 3.4e-16. The root's independent direction query differs
by at most 2.4e-8 because the environment CDF uses floats; the oracle
accounts for that storage precision explicitly. First-target and
joint-strategy density tolerances remain 1e-12. All four admissible BDPT
split weights match independently multiplied path densities and sum to one.

The physical-reference environment suite retains its original RGB/NM/HWSS
mean bands and each estimator's peak cap. Cross-estimator p99 equality is
removed because the estimators have different variances. Three independently
salted full-suite runs each pass 107 checks; their largest paired-ratio
95% half-width is 1.69%, below the smallest unchanged 3% comparison band.
