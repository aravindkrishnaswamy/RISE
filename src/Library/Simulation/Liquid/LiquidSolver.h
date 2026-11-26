//////////////////////////////////////////////////////////////////////
//
//  LiquidSolver.h - "liquid.mac": a free-surface liquid on a staggered
//  grid on the solver contract (simulation M7, docs/SIMULATION_M7A_RESULTS.md)
//
//  Level-set interface (WENO5 + SSP-RK3, gradient-triggered
//  reinitialisation, optional marker particles, volume correction), WENO5
//  momentum with a two-stage projection, ghost-fluid free surface and
//  cut-cell static solids in one symmetric pressure system solved by
//  multigrid-preconditioned conjugate gradients.  Outputs `surface`
//  (mesh.v2, changing topology), optionally `fields` (grid.v1) and
//  `particles` (points.v1).  Deterministic and independent of the thread
//  count; streamed checkpoints; rollback; external macro steps.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_LIQUID_SOLVER_H
#define RISE_SIMULATION_LIQUID_SOLVER_H

#include "../SimulationSolver.h"

namespace RISE { namespace Simulation {

constexpr const char* LiquidSolverId = "liquid.mac";

//! Registers "liquid.mac" (idempotent).
bool RegisterLiquidSolver(std::string& error);

} }

#endif
