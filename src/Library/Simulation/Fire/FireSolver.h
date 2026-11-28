//////////////////////////////////////////////////////////////////////
//
//  FireSolver.h - The production methane pool fire behind the generic
//  solver contract (simulation M2, track C)
//
//  Registered as "fire.methane".  The solver runs the production temporal
//  capstone's driver (FireRun.h) on its own thread and hands control across
//  at every published frame: Advance returns once the frame is in the sink,
//  with the driver blocked inside its accepted-step rendezvous.  The legacy
//  run directory (VDB frames, sealed sequence manifest, budgets, summary) is
//  written as before into the side-output directory "legacy" of the cache;
//  each native frame is that VDB converted by the legacy importer's own code
//  path, so fire_medium reads the cache directly.  Checkpoint/Restore carry
//  the driver's own checkpoint bytes plus the run values its format omits,
//  and a restored run continues the same window exactly (frame numbering,
//  frame clock, statistics, manifests).
//
//  One fire solver per process: the fire library keeps process-wide state
//  (environment-derived option caches, the fast-mode counters, the check
//  handler, the Metal contexts).  A second live instance refuses to
//  configure, and a process applies one set of result-affecting options.
//
//  Parameters and their legacy environment names: docs/SIMULATION_SOLVER_CONTRACT.md.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_FIRE_SOLVER_H
#define RISE_SIMULATION_FIRE_SOLVER_H

#include "../SimulationSolver.h"
#include <string>
#include <utility>
#include <vector>

namespace RISE { namespace Simulation {

//! Registers the fire solver factory under "fire.methane".  Idempotent: a
//! second call succeeds without registering again.  Builds without OpenVDB or
//! Metal still register; such a solver refuses to configure or start with an
//! error naming what is missing.
bool RegisterFireSolver(std::string& error);

//! (parameter name, legacy environment variable) of every result-affecting
//! option, for tests and documentation.  "mode" maps to RISE_FIRE_FAST_MODE.
std::vector<std::pair<std::string,std::string>> FireSolverEnvironmentOptions();

//! M4 (not part of the solver contract): each result-affecting option as the
//! solver reads it -- its parameter name, legacy variable, kind ("flag",
//! "level" or "text"), the largest level, the value that means "the library
//! default" and the validator of a text value -- so the fire parameter schema
//! (FireParameterSchema.h) is generated from the solver's own table.
struct FireSolverOptionInfo {
	std::string parameter, environment, kind, defaultText;
	unsigned maxLevel = 0;
	bool (*valid)(const std::string&) = nullptr;
};
std::vector<FireSolverOptionInfo> FireSolverOptionInfos();

} }

#endif
