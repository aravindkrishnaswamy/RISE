//////////////////////////////////////////////////////////////////////
//
//  FireParameterSchema.h - Simulation M4: the parameter schema of
//  fire.methane (SimulationParameterSchema.h; not part of the solver
//  contract).  The `fire_simulation` chunk descriptor is generated from it.
//  The case fields are FireSolver::Configure's; the result-affecting
//  options come from the solver's own option table (FireSolverOptionInfos),
//  so their names, kinds, defaults and validators have one source.
//
//  The fire solver is never configured inside an application process (one
//  configuration per process; Configure writes the process environment and
//  runs GPU capability checks).  The schema therefore also completes a
//  scene's parameters (every option explicit, so the legacy RISE_FIRE_*
//  environment never reaches a scene bake) and predicts the effective
//  parameters Configure returns, from which the scene computes the cache key.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_FIRE_PARAMETER_SCHEMA_H
#define RISE_SIMULATION_FIRE_PARAMETER_SCHEMA_H

#include "../SimulationParameterSchema.h"
#include <array>
#include <cstddef>

namespace RISE { namespace Simulation {

struct Capabilities;   // SimulationSolver.h

const ParameterSchema& FireParameterSchema();

//! M5c: the Capabilities estimates of a fire configuration (not keyed; never
//! runs the solver or touches the GPU).
//!  - memoryEstimate: the production driver's own host-memory admission
//!    reserve for the case grid -- FireProductionProjectedHeunMetalObserver-
//!    WorkingSetBytes (the owner certificate, which counts the certified
//!    24-level EOS prefix table, about 2.1 GB) plus the 2 GiB host scratch of
//!    FireRunHostMemory::ProductionPolicy -- less that table in fast mode,
//!    which builds only the 10-level one.  The driver refuses a run whose
//!    reserve does not fit min(RAM/2, 32 GiB), so this is the number a
//!    scheduler should count.  An upper bound: measured footprints are 2-3x
//!    lower (docs/SIMULATION_M5C_RESULTS.md).
//!  - costEstimate: wall seconds per simulated second, approximate: tier 8
//!    measured (docs/FIRE_SMOKE_SPEED_R274.md, r275 table: certified 2324,
//!    fast 1121, quiet M1 Max); tier 10 scaled by the measured full-window
//!    tier-10 / tier-8 ratio (1.29); tier 6 unknown (0).
//! `grid` is the case's (nx, ny, nz) when known (Configure's launch case);
//! zeros use the tier's capstone grid (52x52x80, 69x69x106, 86x86x132).
void FireEstimateCapabilities(double tier, bool fast, std::array<std::size_t, 3> grid, Capabilities& caps);

} }

#endif
