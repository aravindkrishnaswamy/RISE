// Worker host (`rise-sim`, M2): bakes one registered solver into a disk cache,
// checkpoints, stops on SIGINT/SIGTERM at a safe point, resumes, reports status.
// The entry point lives in the library so test programs can register extra
// solvers and run the same main; tools/rise_sim.cpp is a thin wrapper.
//
//   rise-sim bake <solver id> <parameters.json|.cbor> <cache dir> [options]
//   rise-sim resume <cache dir> [options]
//   rise-sim status <cache dir>
//   options: --checkpoint-seconds S (default 300; 0 = after every output index)
//            --budget-mb N          (default 1024; per-record memory budget)
//            --stop-after-frames N  (stop as if signalled after N output indices)
//            --force                (bake only: rebake an existing cache as a new epoch)
//
// Sidecars in the cache dir, all replaced durably: rise-sim.job.cbor (solver,
// version, effective parameters, key, epoch), rise-sim.checkpoint (opaque
// solver state behind a small header) and rise-sim.status.cbor. A solver that
// declares SolverDescription::sideOutput also owns <cache dir>/<sideOutput>
// (emptied when a bake starts a new epoch, kept by resume).
#ifndef RISE_SIMULATION_WORKER_H
#define RISE_SIMULATION_WORKER_H

#include "SimulationSolver.h"
#include <iosfwd>

namespace RISE { namespace Simulation {

enum WorkerExitCode : int {
	WorkerFinished = 0, // cache sealed (or already complete)
	WorkerFailed = 1,   // error; the last committed generation and checkpoint remain usable
	WorkerUsage = 2,    // bad command line
	WorkerStopped = 3   // stopped at a safe point; checkpoint written; `resume` continues
};

struct WorkerStatus {
	std::string state;          // "running", "stopped", "finished", "failed"
	std::string solver;
	double simulatedTime = 0, endTime = 0;
	std::uint64_t framesWritten = 0, framesTotal = 0; // output indices committed / declared
	double wallSeconds = 0;     // cumulative over bake and resumes
	std::string lastError;
	std::string detail;         // the solver's last progress detail while running (optional field)
};
bool ReadWorkerStatus(const std::filesystem::path& cache, WorkerStatus& status, std::string& error);

int WorkerMain(const std::vector<std::string>& arguments, std::ostream& out, std::ostream& err);
int WorkerMain(int argc, char** argv); // arguments after argv[0]; std::cout / std::cerr

} }
#endif
