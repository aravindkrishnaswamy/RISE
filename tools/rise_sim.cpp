// rise-sim: the simulation worker host (docs/SIMULATION_M2_DESIGN.md).
// Usage and exit codes: src/Library/Simulation/SimulationWorker.h.
// Solvers are found only through the registry. The shipped library solvers
// are registered here (a static library drops self-registering objects that
// nothing references); test programs link their own solvers and call
// WorkerMain.
#include "../src/Library/Simulation/SimulationWorker.h"
#include "../src/Library/Simulation/Fire/FireSolver.h"
#include "../src/Library/Simulation/Cloth/ClothSolver.h"
#include "../src/Library/Simulation/BuiltinSolvers.h"
#include <iostream>

int main(int argc, char** argv)
{
	std::string error;
	if (!RISE::Simulation::RegisterFireSolver(error)) std::cerr << "rise-sim: fire solver unavailable: " << error << "\n";
	if (!RISE::Simulation::RegisterClothSolver(error)) std::cerr << "rise-sim: cloth solver unavailable: " << error << "\n";
	// M4: the solvers' parameter schemas too (a scene bake keys and completes
	// fire parameters through them); idempotent after the two calls above.
	error.clear();
	if (!RISE::Simulation::RegisterBuiltinSolvers(error)) std::cerr << "rise-sim: " << error << "\n";
	return RISE::Simulation::WorkerMain(argc, argv);
}
