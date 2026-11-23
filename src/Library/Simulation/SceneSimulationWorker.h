// Simulation M4: `rise-sim bake|resume|status <scene.RISEscene>`.  Loads the
// scene privately (no rendering), plans its simulations against it
// (SceneSimulation.h), and bakes the requested ones in dependency order,
// upstream first, feeding each its scene inputs.  Up-to-date simulations are
// skipped; a stale cache is rebaked as a new epoch; a partial cache of the
// current configuration is resumed.  Baking runs one simulation at a time,
// so at most one GPU solver runs (Capabilities::backend is reported).
#ifndef RISE_SIMULATION_SCENE_SIMULATION_WORKER_H
#define RISE_SIMULATION_SCENE_SIMULATION_WORKER_H

#include "SimulationWorker.h"
#include <iosfwd>
#include <string>
#include <vector>

namespace RISE { namespace Simulation {

//! A path naming a scene file (".RISEscene", any case).
bool IsSceneFilePath(const std::string& path);

//! `command` is bake, resume or status; `names` the simulations named on the
//! command line (bake needs names or options.all; resume without names
//! resumes every partial one).  Exit codes as WorkerMain.
int SceneWorkerMain(const std::string& command, const std::string& scene, const std::vector<std::string>& names,
	const WorkerOptions& options, std::ostream& out, std::ostream& err);

} }
#endif
