//////////////////////////////////////////////////////////////////////
//
//  LiquidParameterSchema.h - Simulation M7: the parameter schema of
//  liquid.mac (SimulationParameterSchema.h; not part of the solver
//  contract).  The `liquid_simulation` chunk descriptor is generated from
//  it, and it turns the chunk's structured fields (liquid regions, solids,
//  scene-object solids) into the solver's parameter map.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_LIQUID_PARAMETER_SCHEMA_H
#define RISE_SIMULATION_LIQUID_PARAMETER_SCHEMA_H

#include "../SimulationParameterSchema.h"

namespace RISE { namespace Simulation {

//! The schema (process-lifetime); its defaults are LiquidSolver's.
const ParameterSchema& LiquidParameterSchema();

} }

#endif
