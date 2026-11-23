// rise-sim: the simulation worker host (docs/SIMULATION_M2_DESIGN.md).
// Usage and exit codes: src/Library/Simulation/SimulationWorker.h.
// Solvers are found only through the registry; library solvers register
// themselves, and test programs link their own solvers and call WorkerMain.
#include "../src/Library/Simulation/SimulationWorker.h"

int main(int argc, char** argv)
{
	return RISE::Simulation::WorkerMain(argc, argv);
}
