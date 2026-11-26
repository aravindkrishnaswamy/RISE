// Worker host (`rise-sim`, M2): bakes one registered solver into a disk cache,
// checkpoints, stops on SIGINT/SIGTERM at a safe point, resumes, reports status.
// The entry point lives in the library so test programs can register extra
// solvers and run the same main; tools/rise_sim.cpp is a thin wrapper.
//
//   rise-sim bake <solver id> <parameters.json|.cbor> <cache dir> [options]
//   rise-sim resume <cache dir> [options]
//   rise-sim status <cache dir>
//   rise-sim bake <scene.RISEscene> [simulation ...] [--all] [options]   (M4, SceneSimulationWorker.h)
//   rise-sim resume <scene.RISEscene> [simulation ...] [options]
//   rise-sim status <scene.RISEscene>
//   options: --checkpoint-seconds S (default 300; 0 = after every output index)
//            --budget-mb N          (default 1024; per-record memory budget)
//            --stop-after-frames N  (stop as if signalled after N output indices)
//            --force                (bake only: rebake an existing cache as a new epoch)
//            --gpu-lock PATH        (M5: the GPU lock file; default RISE_SIM_GPU_LOCK or
//                                    DefaultGpuLockPath(), SimulationProcess.h)
//            --no-upstream          (M5, scene bake: bake only the named simulations; an
//                                    upstream that is not complete is an error)
//            --expect-key NAME=KEY  (M5, scene bake/resume: refuse, before touching any cache,
//                                    when the scene file plans NAME with another key -- the
//                                    loaded document and the file on disk differ: save first)
//
// M5: a configuration whose Capabilities::backend needs the GPU (stated and
// not "cpu") waits for the GPU lock before Start/Restore, with worker state
// "waiting-gpu"; one GPU solver runs at a time across every process of this
// user that takes the same lock.  A file `rise-sim.stop` in the cache dir is
// honoured like SIGINT (the portable stop, and the only one on Windows).
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
	std::string state;          // "running", "stopped", "finished", "failed"; M5: "waiting-gpu"
	std::string solver;
	double simulatedTime = 0, endTime = 0;
	std::uint64_t framesWritten = 0, framesTotal = 0; // output indices committed / declared
	double wallSeconds = 0;     // cumulative over bake and resumes
	std::string lastError;
	std::string detail;         // the solver's last progress detail while running (optional field)
	// M5 (optional fields, absent in records of older workers): the worker
	// process, so a bake manager can tell a live worker from a stale record.
	std::uint64_t pid = 0;
	std::uint64_t pidStart = 0; // ProcessStartStamp(pid) (SimulationProcess.h); 0 = unknown
	std::string host;
};
//! M5: the stop file a worker polls in its cache directory.
extern const char* const kWorkerStopFileName;   // "rise-sim.stop"
//! M5: the prefix of the error a scene bake reports when --expect-key fails.
extern const char* const kWorkerSceneMismatch;  // "scene mismatch"

bool ReadWorkerStatus(const std::filesystem::path& cache, WorkerStatus& status, std::string& error);
//! M6: writes rise-sim.status.cbor (this process stamped in it), for the scene group bake.
bool WriteWorkerStatus(const std::filesystem::path& cache, const WorkerStatus& status, std::string& error);
//! M4: the job sidecar of a cache a worker baked (solver, version, the
//! effective parameters and the key of the bake).
bool ReadWorkerJobRecord(const std::filesystem::path& cache, std::string& solver, std::string& version,
	RISECBOR64::Value& effectiveParameters, std::string& key, std::string& error);

// Solver parameters as `rise-sim bake` reads them: a JSON object (integral
// numbers become integers) or a canonical CBOR map. Also used by the
// in-process host's callers (the live viewport, M3c).
bool ParseSolverParameters(const Bytes& bytes, std::size_t budget, RISECBOR64::Value& parameters,
	std::string& error);

struct WorkerOptions {
	double checkpointSeconds = 300;
	std::size_t budget = std::size_t(1024)<<20;
	std::uint64_t stopAfter = 0;
	bool force = false;
	bool all = false;           // scene bake: every simulation
	// M5 (additive, defaulted):
	std::filesystem::path gpuLock;            // empty: RISE_SIM_GPU_LOCK or DefaultGpuLockPath()
	bool noUpstream = false;                  // scene bake: only the named simulations
	std::vector<std::pair<std::string, std::string>> expectKeys; // scene bake/resume: (simulation, key)
};

//! M4: bakes one configuration with scene inputs into `cache`, as `rise-sim
//! bake <solver id>` does with none (the same sidecars, key and epoch rules).
//! `replace` rebakes a cache holding another configuration as a new epoch
//! (the scene bake's rule for a stale cache; `bake <solver id>` needs
//! --force for that); a partial cache of the same configuration is resumed.
//! `inputs` must outlive the call.
int WorkerBakeConfiguration(const std::string& solverId, const RISECBOR64::Value& parameters,
	const ISceneInputs& inputs, const std::filesystem::path& cache, const WorkerOptions& options, bool replace,
	std::ostream& out, std::ostream& err);
//! M4: resumes a partial cache whose configuration reads scene inputs.
int WorkerResumeConfiguration(const std::filesystem::path& cache, const ISceneInputs& inputs,
	const WorkerOptions& options, std::ostream& out, std::ostream& err);

int WorkerMain(const std::vector<std::string>& arguments, std::ostream& out, std::ostream& err);
int WorkerMain(int argc, char** argv); // arguments after argv[0]; std::cout / std::cerr

} }
#endif
