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

namespace RISE { namespace Simulation {

const ParameterSchema& FireParameterSchema();

} }

#endif
