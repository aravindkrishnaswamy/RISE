# r264 — adopt centered momentum; full tier-8 verification

The owner accepts r263 and authorizes centered momentum in production.
CHARM is not adopted. The wall treatment is a minor contributor and does
not explain the in-flame deficit; no wall-function implementation is included.

## Design revision

In the active resident projected-Heun Metal owner, separate the interior
momentum gas-mass carrier from the scalar reconstruction. For a primal face,

    F_g,momentum = 0.5 (rho_g,L + rho_g,R) u_face + F_g,physical

Gas density is the sum of species components1..6, excluding particulate carbon.
The physical gas-diffusion flux is retained exactly once. At domain boundaries,
including prescribed burner faces and pressure-open faces, use the existing
accepted total gas-mass carrier. Apply the existing arithmetic MAC restriction
to dual faces, multiply by the existing centered transported velocity, and
retain the existing conservative flux-difference operator. Scalars continue
using MC reconstruction and shared FCT acceptance.

Both the active R0/R1 momentum rates and the R0 term recomputed during Heun
R1 use the centered interior carrier. The scalar accepted alpha still governs
boundary carriers. The interior centered carrier has zero antidiffusive delta;
therefore scalar alpha no longer directly blends the interior primal momentum
carrier. Boundary carriers remain accepted and can enter nearby dual restrictions. Source,
physical diffusion, stresses/SGS, forcing, EOS-tail correction, all projections,
burner/ambient boundaries and timestep/event rules remain unchanged.

This adopts the exact arithmetic tested externally in r260/r262, with production
kernel naming/comments. It is not a port of FDS's complete rotational momentum
formulation. Separating momentum from the scalar accepted mass flux removes
the previous same-carrier scalar/momentum compatibility identity; do not claim
the unchanged scalar commuting diagnostic certifies the new momentum pair.
Centered velocity transport still has the familiar semidiscrete flux identity
for its own mass carrier, but this does not assert exact kinetic-energy
conservation of the full reacting/projected Heun method.

The change is scoped to the active resident production owner, used by the
macOS production solver. Historical CPU/oracle/legacy diagnostic operators
are not a second production implementation and retain their reference behavior.
No new source file or public configuration switch is added.

## Measured basis for adoption

[r260/r262](FIRE_SMOKE_COMBINED_FLAME_R262.md), same15s checkpoint and4s:

|Quantity|MC baseline|Centered momentum|LES FDS reference|
|---|---:|---:|---:|
|Net flux1m kg/s|0.24316|0.24468|0.30253|
|Net flux2m kg/s|0.47883|0.54879|0.70139|
|Net flux3m kg/s|0.71846|0.91309|1.23815|

Centered recovers37.5% of the3m gap and improves2/3m net flux14.6/27.1%.
The first two accepted physical states differ from MC. Adding CHARM reduces
3m flux to0.86829kg/s, so CHARM is not adopted. The historical FDS/CHARM
reference interval is3.981891s; MC/centered/combined intervals are4s.

Centered from-zero onset checks completed tier8 through3.5s, maximum
14.4426m/s at1.1646s, and tier10 through3s, maximum14.7126m/s at1.3858s.
This avoids the previous63.96m/s tier8 runaway at2.699s and tier10's15m/s
crossing at2.131s within the tested windows. It does not establish full-window
combustion, centerline, entrainment or long-time stability acceptance.

## Required full-window verification

Run the production binary from zero at tier8,0.30m methane,33kW, through
25.091s with the existing14.625s statistics interval and0.0625s snapshots.
Use the existing32GiB graceful memory protection. No checkpoint migration.
Report fuel consumed/supplied and closure, top methane export, mean flame
height by the existing FDS Validation Guide definition (H99 separately),
net and upward entrainment at native1/2/3m against **no-slip FDS r263**,
the same McCaffrey station rows as r237/r252, and maximum accepted-step velocity.
Do not conflate net plume mass flux with upward-only flux, or short A/B
windows with the full statistics window.

Native launch command:

    bin/tests/FireSequenceTest --fire-production-temporal-capstone 8 0.0625 /Volumes/SSD/RISE/fire_production_calibration/r264_centered_production/run

Status: implementation and independent review complete; zero P1 on the final
production change and launch/reduction path. Clean make and Xcode Deployment/
Opto builds succeeded. No RISE source warnings; excluded third-party Homebrew
dylib deployment-target linker warnings and AppIntents metadata notices are
recorded in the build manifest. FireProductionSolverTest reports13 historical
style timing/2GiB/source-topology failures (status previously recorded12),
not a passing whole-suite result. No new centered-physics failure was found.

The production two-step replay from the15s checkpoint changed every checked
physical field versus MC and exactly reproduced the accepted r260 differing
counts/maxima for all16 field snapshots. The actual full-window verification
started from zero at19:30 PDT on2026-10-28, not from that replay.
Initial steps are accepted with the existing32GiB ceiling active. Results pending; external
`r264_centered_production/run_status.json` records current state.

Review found one launcher P1: precreating the run folder conflicted with the
native capstone. It was fixed; a fresh final review found no P1. No production
arithmetic repair was needed after porting the accepted experiment.
A separate read-only r265 analysis compares retained r251/no-slip FDS flame
structure and eligibility categories. Stop for owner review with both results;
no full tier10 run is authorized.
