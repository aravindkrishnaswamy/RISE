//////////////////////////////////////////////////////////////////////
//
//  ClothParameterSchema.h - Simulation M4: the parameter schema of
//  cloth.shell (SimulationParameterSchema.h; not part of the solver
//  contract).  The `cloth_simulation` chunk descriptor is generated from
//  it, and it turns the chunk's structured fields (material, pins,
//  colliders, scene objects) into the solver's parameter map.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_CLOTH_PARAMETER_SCHEMA_H
#define RISE_SIMULATION_CLOTH_PARAMETER_SCHEMA_H

#include "../SimulationParameterSchema.h"

namespace RISE { namespace Simulation {

//! The schema (process-lifetime).  Its defaults are the ones ClothSolver's
//! Configure applies; SimulationSceneTest checks them against Configure.
const ParameterSchema& ClothParameterSchema();

//! Tessellation detail of a `collider_object` (segments per parametric axis,
//! IGeometry::TessellateToMesh); part of the scene input's identity.
constexpr unsigned int ClothColliderTessellationDetail = 24;

} }

#endif
