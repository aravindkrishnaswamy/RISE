//////////////////////////////////////////////////////////////////////
//
//  RigidParameterSchema.h - Simulation M6: the parameter schema of
//  rigid.bodies (SimulationParameterSchema.h; not part of the solver
//  contract).  The `rigid_simulation` chunk descriptor is generated from
//  it, and it turns the chunk's structured fields (bodies, scene-object
//  bodies, planes) into the solver's parameter map.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_RIGID_PARAMETER_SCHEMA_H
#define RISE_SIMULATION_RIGID_PARAMETER_SCHEMA_H

#include "../SimulationParameterSchema.h"

namespace RISE { namespace Simulation {

//! The schema (process-lifetime); its defaults are RigidSolver's.
const ParameterSchema& RigidParameterSchema();

} }

#endif
