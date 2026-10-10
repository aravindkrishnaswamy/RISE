# DL-475: single-medium-scatter strategy gap

The supported BDPT integrator has no strategy for
`C - S...S - M - S...S - L`, where C and the delta-position point light L
are outside a closed index-matched dielectric carrying fog, S is a delta
surface, and M is the only volume scatter. This batch pins the exact class;
it does not implement a new BDPT strategy or claim energy parity.

`tests/BDPTMediumDeltaStrategyTest.cpp` reuses the fog-box fixture and capture
from `VCMVolumeMergeTest.cpp`. `max_volume_bounce 1` excludes two-scatter paths
and their heavy-tailed medium-medium connection estimator. The point light
is outside the camera film. PT has dielectric transparent-shadow NEE enabled;
VCM can merge the two medium endpoints into one physical scatter.

## Exhaustion of BDPT strategies

- s=0: the eye walk cannot hit a geometric emitter representing a
  delta-position point light. The glass surfaces have no emission.
- s=1: NEE from M toward L crosses a delta wall. Ordinary visibility rejects
  it. The see-through fallback in `BDPTIntegrator.cpp` requires the live
  delta-pass-through domain. `PerfectRefractorMaterial` does not declare
  `HasDeltaPassThrough`; the fallback also requests the shadow walk with
  dielectric traversal disabled. Index matching alone does not enable it.
- t=1: a light-walk M-to-camera splat crosses the opposite delta wall and
  fails ordinary connection visibility.
- s>=2 and t>=2: a connection ending at S is non-connectible. Connecting
  M to another M uses two volume scatters and is excluded by the cap of one.

This is a support gap, not a repairable MIS weight on an existing strategy.
A possible exact subset is a straight connection through boundaries whose
LIVE relative index is exactly one, with transmittance, both direction
variants and MIS accounting shared by NEE and splatting. Declaring all
perfect refractors pass-through would be incorrect for general/nested IORs.

## Pin and residual

The default diagnostic requires each salted PT/VCM render to be finite and
positive, BDPT to be exactly zero, and VCM/PT to agree within paired 3SE.
`--strict-energy-pin` retains the failing BDPT/PT energy requirement.
At n=8 distinct ValueSalt seeds: PT mean 0.063577479, SD 0.000109929;
BDPT mean/SD 0/0; VCM mean 0.063351623, SD 0.002594918. VCM/PT is
0.996447556. Paired difference mean -0.000225855, SD 0.002551187,
within 3SE. The diagnostic passes 9/0 (rc0); strict energy parity fails
9/1 (rc1), replaying the same measured means. Related gates:
VCMRecurrenceTest103/0, VolumeEnvFurnaceTest32/0,
RayCasterVolumeAbsorptionTest9/0. All named build return codes are zero,
with no compiler warnings/errors. No production transport changes.
The test is an RGB witness. The visibility and strategy rejection code is
shared by spectral dispatch; this is not a separate measured spectral claim.
The separate two-scatter heavy-tail observation stays open in DL-475.
