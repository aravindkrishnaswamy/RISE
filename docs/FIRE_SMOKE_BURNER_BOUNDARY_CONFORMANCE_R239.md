# r239 — restore the prescribed burner boundary, once

Status: pre-implementation amendment and source audit. No corrected-run result
or empirical pass is claimed. This entry precedes the implementation.

## Evidence and scope

r238's accepted R1 budget at retained tier-10 step 6489 localizes
8.2734249337153135e-5 kg/s of outward methane transport to the 120 canonical
source-footprint faces: 95.2676% of bottom export at that observed step.
Budget SHA256:
`d9014ea355e3c17037a5c2448479957530e81a75a481d97471c082c1cdb69ed1`.
Eight diagnostic accepted-state payload roots match the prior observation;
this is not an empirical continuation or a full-window flux integral.

The implementation audit identifies a boundary-construction mismatch, not
an EOS tolerance defect. `FireSequenceTest.cpp` installs the oracle's
`bottomFuelMask` and prescribed flux from the canonical source pattern.
Its production branch instead adds fuel to bottom-cell Q before constructing
the reaction packet, sets initial bottom-face momentum/velocity, then passes
an all-zero fuel mask and all-pressure-open sides to the resident owner.
Those starting face values are not a constraint on later projections.

The gap extends beyond the launcher: the resident physical-flux request has
no injected tuple or per-face prescribed mass-flux operand, and its kernels
implement wall/open/seam scalar fluxes. The resident projection request has
side-level boundary kinds but no per-face prescribed burner flux. Therefore
enabling the existing transport mask alone cannot implement the oracle
boundary. The transport mask currently accepts nonzero entries only on a
wall-class bottom side; changing that admission alone is also insufficient.

## Chosen formulation

Conform to the existing live oracle's prescribed inlet; do not tune heat
release, chemistry, domain dimensions, velocities, or empirical thresholds.
Only the masked bottom faces change class; the surrounding bottom plane
remains pressure-open. Keep the same source pattern and nominal fuel rate.

Let j be the nonnegative prescribed inward mass flux in kg/(m² s), Q_in
the injected conservative tuple, rho_in its constituent total density,
and s the outward-normal sign (-1 on the bottom). The prescribed scalar
outward tuple is F_out = -j Q_in/rho_in. Both low and high candidates carry
this same tuple; their antidiffusive difference is zero at that face. The
limiter must not attenuate the imposed source or replace it with ambient.

For the divergence-target split, use the oracle's existing identity:
f_N,out = F_out - u_out Q_cell. The interior carrier already participates in
the tangent projection. Using the full imposed tuple as f_N would count that
carrier twice. Validate imposed tuple, carrier, and difference separately,
with local termwise arithmetic bounds rather than relative cancellation
tests; their sum alone is not the test.

Projection uses the live `ProjectPressureOpenMACVelocity3D` and
`ProjectPressureOpenMACVelocity3DFinal` reference, not the historical
`ReferenceProjectPressureOpenMACVelocity3DEliminated` helper. The live paths
use rho_face = (rho_cell + rho_in)/2, hold M_normal = -s j, and hence
u_normal = -s j/rho_face at the burner. The historical eliminated helper
instead divides by rho_in; importing it would silently change the reference.
Prescribed faces have no pressure correction coefficient and no open-head
unknown, but their fixed velocity contributes to the divergence RHS and
post-projection validation. Masked faces must also be excluded from open
class/head updates and preserved through R2 and compatible momentum sweeps.
Unmasked open faces retain the current active-set and head treatment.

Fuel enters exactly once: remove volumetric fuel staging for the conformed
ported path when the imposed boundary is active. Build the frozen chemistry
packet from that step's actual Q0, following the oracle schedule, not from a
second fuel-enriched copy. Retain reaction/pilot/radiative source terms.
Accumulate accepted imposed injection in the source ledger only on acceptance;
a rejected trial contributes nothing. Keep the ordinary legacy path isolated
if still needed to reproduce historical evidence. Case-record semantics and
binary hashes must identify the changed production source/boundary schedule.

## Implementation boundaries and gates

Use immutable case operands through existing requests and private resident
buffers; no new authority class or certificate protocol. Extend existing
identity inputs and working-set accounting for the actual added operands.
An empty/no-inlet request must preserve the current arithmetic and old tests.

1. Mixed-boundary projection: a masked face surrounded by open faces;
   opposed provisional momentum and strong pressure forcing must not reverse
   its imposed velocity. Compare the live fp64 oracle per face, including
   arithmetic face density, fixed momentum, neighboring head and residual.
   Check coarse-grid coefficients and all post-residual consumers, not just
   the final velocity overwrite.
2. Scalar/target split: arbitrary interior mixtures, zero and nonzero j,
   inward and outward provisional velocities. Imposed low/high tuples are
   identical, correction zero, f_N matches the oracle independently. The
   old open-donor mutant must permit outward fuel in the reversing fixture.
3. End-to-end source ledger: a small reaction-disabled mixed-boundary case
   proves injected mass enters exactly once; the staged-plus-inlet mutant
   must be detected. Reaction-enabled matched inputs then check the changed
   source schedule. Existing owner gates and residency checks remain intact.
4. Run a short from-zero production prefix before another full window.
   Report accepted imposed injection and actual boundary flux separately;
   retain top export as a real measured loss, not something this fix erases
   by assumption. Reuse r238's accepted-flux observer at selected states.

This is a semantic boundary conformance change, not a bit-identical cost
promotion. Historical checkpoints remain untouched; no kernel migration.
Only a from-zero full window can revise the empirical rows or deficit claim.
Fresh implementation review and existing qualification gates precede launch.

## Rejected shortcuts

- Turning the entire bottom into a wall changes entrainment outside the
  burner and is not the specified boundary.
- Clamping an outward velocity after projection violates the solved
  divergence unless the operator/RHS already included the fixed face.
- Imposed scalar injection plus existing volumetric staging double-counts
  fuel; zero-flux sealing plus staged injection is a different split whose
  momentum/volume consistency has not been established.
- Raising nominal fuel to recover 33 kW hides the transport loss and changes
  the capstone input. Normalizing the McCaffrey comparison to measured HRR
  would hide a failed empirical row.

## Audited source identities at cc989d82

| Source | SHA256 |
|---|---|
| tools/fire_simulator_core.h | 21a5cf2c6177cc535d9ff8ced6835a7b0b09b2fe19dfed887c76d7e89c61eae1 |
| tests/FireSequenceTest.cpp | b223222ea17019a82f658942668f9c30d46d510f397ca9948d5d1c50b18cba26 |
| tests/FireProductionDyadicCalibrationFixture.h | ceecf4e7021779837796b6a54b6a2f1227edd08021579b68127fae2cf0ee7bff |
| src/Library/Utilities/FireProductionProjection.h | 263b98cb94622fea89229712cfb061280cebae9bc0a6543adfd729734c290e20 |
| src/Library/Utilities/FireProductionProjectionMac.mm | 741a680bbcc35037a86b1f566f023ba4fd75e6b7da7078c3fc8e97387ffbf158 |
| src/Library/Utilities/FireProductionAdvectionMac.mm | 2fee776af9ce97152fd7524c0609afe89ef89c4da17315e898f17addf1252f67 |
| src/Library/Utilities/FireProductionTransport.h | a8e41ff1a5145634943a3e8c6b485525cdecb15c2a8b24456bcc661889b00016 |
