// Simulation M5: the bake scheduler and monitor the apps and the agent use
// (docs/SIMULATION_SOLVER_CONTRACT.md).
//
// A bake is a separate `rise-sim` process (SimulationWorker.h) run on the
// scene FILE: `rise-sim bake <scene> <simulation> --no-upstream`, one
// process per simulation, so a bake survives the app quitting and never
// touches the loaded scene.  The manager
//   - queues requested simulations in dependency order (upstream first),
//   - starts them under the resource rules (one GPU-class bake at a time in
//     this manager, the machine-wide GPU lock taken by the worker itself,
//     a cap on concurrent CPU bakes, a memory budget over the solvers'
//     declared estimates),
//   - monitors each from its cache's rise-sim.status.cbor (a modest poll, no
//     busy loop) and reports state, progress, ETA, errors and the pid,
//   - stops (stop file + SIGINT -> checkpoint -> resumable), resumes,
//     cancels queued jobs,
//   - adopts workers already running for a scene's caches (verified by pid
//     AND process start stamp AND the cache writer lock, never by a pid
//     alone) and reports interrupted bakes as resumable.
//
// One process-wide instance (Instance()) with per-document use, like the
// render coordinator (docs/gui/RENDER_COORDINATOR.md §2.1): the resource
// rules are process- and machine-wide, so per-document managers would each
// need to coordinate anyway.  A document talks to it with a
// BakeSceneSnapshot (its scene path, whether it has unsaved changes, and its
// simulations' states); jobs are keyed by (scene path, simulation).
//
// Thread-safe.  Listeners run on the manager's monitor thread (or the
// calling thread of a request), never under the manager's lock.  Nothing
// here is solver-specific.
#ifndef RISE_SIMULATION_SIMULATION_BAKE_MANAGER_H
#define RISE_SIMULATION_SIMULATION_BAKE_MANAGER_H

#include "SceneSimulation.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace RISE { namespace Simulation {

enum class BakeJobState {
	Queued,        // waiting for its upstream, a CPU slot, this manager's GPU slot or memory (see waitingFor)
	WaitingForGpu, // the worker runs and waits for the machine-wide GPU lock
	Running,
	Stopping,      // stop requested; the worker checkpoints at its next safe point
	Stopped,       // stopped at a checkpoint, or interrupted: resumable when the cache is partial
	Finished,      // the cache is sealed (or was already complete)
	Failed,
	Cancelled      // removed from the queue before it started
};
//! "queued", "waiting-gpu", "running", "stopping", "stopped", "finished", "failed", "cancelled".
const char* BakeJobStateName(BakeJobState state);
bool BakeJobTerminal(BakeJobState state);

struct BakeJob {
	std::uint64_t id = 0;                 // unique in this process, increasing (the queue order)
	std::string scene;                    // the scene file the worker bakes (canonical absolute path)
	std::string simulation, solverId;
	std::string backend;                  // Capabilities::backend as planned ("" = not stated)
	std::filesystem::path cacheDir;
	BakeJobState state = BakeJobState::Queued;
	bool resume = false;                  // runs `rise-sim resume`
	bool force = false;                   // runs `rise-sim bake --force`
	bool adopted = false;                 // a worker this manager did not start
	bool interrupted = false;             // the worker ended without a clean stop (killed, crashed, machine restarted)
	bool resumable = false;               // terminal with a partial cache of the current key: Resume continues it
	bool stoppable = true;                // false for an adopted worker that records no pid (an older rise-sim)
	bool cacheMissing = false;            // Finished or Stopped, but the cache was deleted from disk since (VerifyCaches); bake again
	std::vector<std::uint64_t> dependsOn; // jobs that must finish first (upstream simulations)
	std::string waitingFor;               // queued: why it has not started yet
	double simulatedTime = 0, endTime = 0;// solver time; end = last output time
	std::uint64_t framesWritten = 0, framesTotal = 0;
	double wallSeconds = 0;               // the worker's wall time over bake and resumes
	double etaSeconds = -1;               // remaining wall seconds, estimated from progress; -1 = unknown
	std::string detail;                   // the worker's detail (progress, waiting for the GPU, ...)
	std::string lastError;
	std::uint64_t pid = 0;                // the worker process (0 = not started, or unknown)
	int exitCode = -1;                    // a worker this manager started: its exit code (rise-sim's codes)
	std::uint64_t memoryEstimate = 0;     // declared peak bytes (0 = unknown)
	std::filesystem::path logFile;        // the worker's stdout/stderr (jobs this manager started)
	std::uint64_t changeCounter = 0;      // the manager's counter at this job's last change
};

//! What a document tells the manager about itself.
struct BakeSceneSnapshot {
	std::filesystem::path scene;          // its scene file; empty when it was never saved
	bool unsavedChanges = false;          // the document differs from that file
	std::vector<SceneSimulationStatus> simulations; // Job::DescribeSimulations of the loaded document, in dependency order
};

struct BakeRequest {
	std::vector<std::string> simulations; // the simulations to bake (their out-of-date upstreams are added)
	bool all = false;                     // every simulation with a solver
	bool staleOnly = false;               // with `all`: only those not complete with their current key ("bake all stale")
	bool force = false;                   // rebake the named ones even when complete (not their upstreams)
};

struct BakeEvent {
	enum class Kind { JobChanged, FramesLanded };
	Kind kind = Kind::JobChanged;
	BakeJob job;                          // after the change
	std::uint64_t changeCounter = 0;
};
using BakeListener = std::function<void(const BakeEvent&)>;

struct BakeManagerSettings {
	//! The worker's argv prefix (e.g. {"/path/rise-sim"}, or a test program
	//! and its worker flag).  Empty: SetWorkerOverride, then LocateWorker.
	std::vector<std::string> workerCommand;
	//! Passed to every worker as --gpu-lock; empty: the worker's own default
	//! (RISE_SIM_GPU_LOCK or DefaultGpuLockPath()).
	std::filesystem::path gpuLock;
	//! Job logs; empty: DefaultBakeStateDirectory().
	std::filesystem::path stateDirectory;
	unsigned maxCpuBakes = 2;             // concurrent bakes on a CPU backend (>= 1)
	//! Budget for the declared memory estimates of running bakes; 0 = half
	//! the physical memory (or unlimited where the platform cannot say).
	std::uint64_t memoryBudget = 0;
	unsigned pollMilliseconds = 250;      // monitor cadence (status records are small; >= 20)
	double checkpointSeconds = -1;        // < 0: the worker's default; else --checkpoint-seconds
};

class SimulationBakeManager {
public:
	//! The process-wide manager (GUI documents and agent sessions share it).
	static SimulationBakeManager& Instance();

	SimulationBakeManager();
	explicit SimulationBakeManager(const BakeManagerSettings& settings);
	//! Stops monitoring.  Workers keep running (a new manager adopts them).
	~SimulationBakeManager();
	SimulationBakeManager(const SimulationBakeManager&) = delete;
	SimulationBakeManager& operator=(const SimulationBakeManager&) = delete;

	BakeManagerSettings Settings() const;
	void SetSettings(const BakeManagerSettings& settings);
	//! The rise-sim executable to run ("" = search; see LocateWorker).
	void SetWorkerOverride(const std::string& path);
	//! The argv prefix a job would run with now, or false with the places searched.
	bool ResolveWorker(std::vector<std::string>& command, std::string& error) const;
	//! Search order: `override`, $RISE_SIM_WORKER, then beside the running
	//! executable: rise-sim, rise_sim, tools/rise_sim, ../tools/rise_sim
	//! (the make tree: bin/rise, bin/tests/*), ../Helpers/rise-sim and
	//! ../Resources/rise-sim (a macOS app bundle's Contents/MacOS), then $PATH.
	//! ".exe" is appended on Windows.
	static bool LocateWorker(const std::string& override, std::filesystem::path& found,
		std::vector<std::filesystem::path>* searched = nullptr);

	//! Adopts workers already running for the snapshot's caches and reports
	//! interrupted or stopped bakes as resumable jobs.  Idempotent; cheap
	//! (reads the status records of caches it has no job for).
	bool Attach(const BakeSceneSnapshot& scene, std::string& error);
	//! Queues bakes (see BakeRequest).  Save rule: the scene must have a file;
	//! every simulation whose key the document knows is baked with
	//! --expect-key, so the worker refuses (before touching the cache) when
	//! the file plans another key; with unsaved changes, a simulation whose
	//! key cannot be known yet (upstream not baked, key known only after a
	//! bake) is refused here, "save first".  An active job for the same
	//! simulation is reused.  `jobs` gets one id per simulation in the set,
	//! in queue order.
	bool RequestBake(const BakeSceneSnapshot& scene, const BakeRequest& request, std::vector<std::uint64_t>& jobs,
		std::string& error);
	//! Continues a stopped, interrupted or failed bake of the current key
	//! (`rise-sim resume`).
	bool Resume(const BakeSceneSnapshot& scene, const std::string& simulation, std::uint64_t& job, std::string& error);
	//! A queued job is cancelled; a running one is asked to stop at its next
	//! safe point (stop file, and SIGINT on POSIX): it checkpoints and ends
	//! Stopped, resumable.  `kill` ends the process at once (no checkpoint:
	//! a resume continues from the last periodic checkpoint) -- only on an
	//! explicit request.
	bool Stop(std::uint64_t job, bool kill, std::string& error);
	//! The active job of (scene, simulation), stopped as Stop does.
	bool StopSimulation(const std::string& scene, const std::string& simulation, bool kill, std::uint64_t* job,
		std::string& error);
	//! Cancels every queued job of `scene` ("" = every scene); the count.
	unsigned CancelQueued(const std::string& scene);

	std::vector<BakeJob> Jobs(const std::string& scene = std::string()) const; // "" = every scene; id order
	bool Job(std::uint64_t id, BakeJob& job) const;
	//! The newest job of (scene, simulation).
	bool LatestJob(const std::string& scene, const std::string& simulation, BakeJob& job) const;
	//! Blocks up to `timeoutMs` for the job to reach a terminal state; true
	//! when it did (or was already).  `job` gets its state either way.
	bool Wait(std::uint64_t id, unsigned timeoutMs, BakeJob& job) const;

	//! Bumped on every change (state, progress, frames); for a GUI's ~2 Hz poll.
	std::uint64_t ChangeCounter() const { return counter_.load(); }
	//! Bumped when new frames land in a cache of `scene` (a controller compares
	//! it with what it last refreshed; SceneEditController::RefreshSimulationFrames).
	std::uint64_t FramesGeneration(const std::string& scene) const;
	std::uint64_t AddListener(BakeListener listener);
	//! After it returns, the listener is not called again (unless it is the
	//! caller: a listener may remove itself).
	void RemoveListener(std::uint64_t id);

	//! A cheap existence check (one stat of the manifest, nothing opened or
	//! verified) of the cache each finished or stopped job last produced: one
	//! something other than this manager deleted is marked `cacheMissing`
	//! (not resumable, a lastError says so), the change counter and the
	//! scene's frames generation move, so a document refreshes its status
	//! ("missing") and drops the frames it still shows.  The monitor does this
	//! about once a second, Attach (a panel refresh) at once.  Returns how many
	//! jobs it marked.  Never touches a cache a job is writing.
	unsigned VerifyCaches();
	//! Runs one monitor pass now (tests; the monitor thread does this on its cadence).
	void PollNow();
	//! The key a scene path is filed under (canonical absolute).
	static std::string SceneKey(const std::filesystem::path& scene);

private:
	struct Entry;
	struct Pending;
	using Lock = std::unique_lock<std::mutex>;
	void EnsureMonitor_(Lock& lock);
	void MonitorLoop_();
	void Tick_(Lock& lock, std::vector<BakeEvent>& events);
	void Update_(Entry& e, std::vector<BakeEvent>& events, bool framesLanded = false);
	bool StartJob_(Entry& e, std::string& error);
	void Finalize_(Entry& e, bool haveExit, int exitCode, std::vector<BakeEvent>& events);
	void ReadProgress_(Entry& e, std::vector<BakeEvent>& events);
	bool AttachLocked_(const BakeSceneSnapshot& scene, std::vector<BakeEvent>& events);
	Entry* Active_(const std::string& scene, const std::string& simulation);
	Entry& NewEntry_(const std::string& scene, const SceneSimulationStatus& s);
	bool StopLocked_(Entry& e, bool kill, std::vector<BakeEvent>& events, std::string& error);
	void Dispatch_(std::vector<BakeEvent>& events);
	unsigned VerifyCachesLocked_(std::vector<BakeEvent>& events);
	std::chrono::steady_clock::time_point lastVerify_{};
	std::uint64_t MemoryBudget_() const;

	mutable std::mutex mutex_;
	mutable std::condition_variable changed_;   // jobs changed (Wait)
	std::condition_variable wake_;              // monitor wake-up
	BakeManagerSettings settings_;
	std::string workerOverride_;
	std::map<std::uint64_t, std::unique_ptr<Entry>> jobs_;
	std::map<std::string, std::uint64_t> framesGeneration_;
	std::atomic<std::uint64_t> counter_{0};
	std::thread monitor_;
	bool quit_ = false;

	std::mutex listenerMutex_;
	std::recursive_mutex dispatchMutex_;      // held while listeners run (recursive: a listener may remove itself)
	std::map<std::uint64_t, BakeListener> listeners_;
	std::uint64_t nextListener_ = 1;
};

} }
#endif
