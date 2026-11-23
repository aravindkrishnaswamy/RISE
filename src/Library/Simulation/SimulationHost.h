// Simulation hosts (M2): the in-process host, its in-memory publisher, and the
// co-stepping group-host interface (implemented in M6). The worker host is
// SimulationWorker.h. Hosts own files, threads and time; solvers own none.
#ifndef RISE_SIMULATION_HOST_H
#define RISE_SIMULATION_HOST_H

#include "SimulationSolver.h"
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>

namespace RISE { namespace Simulation {

// Host-side completion of a configured description: validates it, computes the
// cache key from the effective parameters and input identities, derives the
// epoch, and writes producer identity, fidelity (EffectiveFidelity) and the
// evidence digests into description.cache.
bool PrepareCacheDescription(SolverDescription& description, const ISceneInputs& inputs,
	const std::string& parentDigest, std::string& error);
// Deterministic epoch for a bake of `key` replacing the generation `parentDigest`
// (empty for a new cache): reruns are reproducible, rebakes always differ.
std::string DeriveEpoch(const std::string& key, const std::string& parentDigest);

// Immutable in-memory generation. Readers keep it as long as they like; the
// publisher never mutates a snapshot it has handed out.
class MemoryCacheSnapshot final : public IReadOnlyCacheSnapshot {
public:
	const CacheDescription& Description() const override { return *description_; }
	const std::vector<CacheEntry>& Entries() const override { return entries_; }
	// Chain digest over the publication history (not a disk manifest digest).
	const std::string& Digest() const override { return digest_; }
	bool Complete() const override { return complete_; }
	FrameStatus Read(std::int64_t index, const std::string& output, std::size_t byteBudget,
		std::shared_ptr<const Bytes>& bytes, std::string& error) const override;
private:
	friend class MemoryPublisher;
	std::shared_ptr<const CacheDescription> description_;
	std::vector<CacheEntry> entries_;
	std::vector<std::shared_ptr<const Bytes>> payloads_; // parallel to entries_
	std::string digest_;
	bool complete_ = false;
};

// Thread-safe publisher of MemoryCacheSnapshot generations. A republished
// (index, output) replaces the payload in later generations only.
class MemoryPublisher {
public:
	void Reset(const CacheDescription& description);
	bool Publish(std::int64_t index, const std::string& output, double time,
		std::shared_ptr<const Bytes> payload, const Bytes& receipt, std::string& error);
	std::shared_ptr<const MemoryCacheSnapshot> Current() const;
private:
	mutable std::mutex mutex_;
	std::shared_ptr<const MemoryCacheSnapshot> current_;
};

// Steps one solver inside the application. Stepping happens either on the
// caller's thread (Step, StepToIndex) or on the host's own worker thread
// (Play, Pause, WaitIdle). Frames go to the in-memory publisher and, when
// recording, through a CacheWriter. Not copyable.
class InProcessHost {
public:
	struct Checkpoint {
		std::string key;
		SolverPosition position;
		Bytes state;
	};
	explicit InProcessHost(std::unique_ptr<ISimulationSolver> solver);
	~InProcessHost();
	InProcessHost(const InProcessHost&) = delete;
	InProcessHost& operator=(const InProcessHost&) = delete;

	bool Configure(const RISECBOR64::Value& parameters, const ISceneInputs& inputs, std::string& error);
	const SolverDescription& Description() const { return description_; }
	// Optional, after Configure and before Start: also write every frame to a
	// new epoch of the disk cache at `root`, sealing it when the run finishes.
	bool Record(const std::filesystem::path& root, std::size_t budget, std::string& error);
	void SetPorts(IPortExchange* ports) { ports_ = ports; }     // not owned; set while idle
	bool Start(std::string& error);

	// Caller-thread stepping; refused while the worker thread is playing.
	bool Step(const AdvanceRequest& request, AdvanceResult& result, std::string& error);
	bool StepToIndex(std::int64_t index, std::string& error);  // until index is published or the run ends

	// Worker-thread stepping. Play returns at once; the worker advances output
	// by output until `untilIndex` is published, the run ends, Pause is called
	// or a step fails. Pause interrupts the current Advance at a safe point.
	bool Play(std::string& error, std::int64_t untilIndex = (std::numeric_limits<std::int64_t>::max)());
	void Pause();
	bool WaitIdle(std::string& error);                         // false if the worker failed
	bool Busy() const;

	std::shared_ptr<const IReadOnlyCacheSnapshot> Snapshot() const { return publisher_.Current(); }
	SolverPosition Position() const;

	// While idle. Restore keeps already published frames; republished frames
	// replace them in later snapshots (a recording refuses conflicting bytes).
	bool SaveCheckpoint(Checkpoint& checkpoint, std::string& error);
	bool RestoreCheckpoint(const Checkpoint& checkpoint, std::string& error);

private:
	class Sink;
	bool StepLocked(const AdvanceRequest& request, AdvanceResult& result, std::string& error);
	void WorkerLoop();
	std::unique_ptr<ISimulationSolver> solver_;
	SolverDescription description_;
	MemoryPublisher publisher_;
	std::unique_ptr<CacheWriter> recorder_;
	IPortExchange* ports_ = nullptr;
	bool configured_ = false, started_ = false;
	SolverPosition position_;
	// Worker thread state, guarded by mutex_. stepMutex_ serializes solver calls.
	mutable std::mutex mutex_;
	std::mutex stepMutex_;
	std::condition_variable wake_;
	std::thread worker_;
	bool playing_ = false, stepping_ = false, shutdown_ = false, failed_ = false;
	std::int64_t until_ = 0;
	std::string workerError_;
};

// ---------------------------------------------------------------- group host (interface only; M6)

// How a co-stepping group orders the port exchange inside one macro step.
enum class ExchangeOrder {
	Jacobi,      // every member advances on the values exchanged at the start of the step
	GaussSeidel  // members advance in `order`; later members see earlier members' new offers
};
struct GroupPolicy {
	double macroStep = 0;                 // seconds of simulated time per exchange
	ExchangeOrder order = ExchangeOrder::GaussSeidel;
	std::vector<std::string> memberOrder; // for GaussSeidel; empty = insertion order
	unsigned subIterations = 1;           // >1: implicit coupling, repeat the step from a checkpoint
	double convergenceTolerance = 0;      // stop sub-iterating once port change is below this; 0 = fixed count
};
struct PortConnection {
	std::string fromMember, fromPort;     // an offered port (or a cached member's output)
	std::string toMember, toPort;         // an accepted port
};

// Runs solvers that are two-way coupled with a shared macro step. Every live
// member must report Capabilities::externalMacroStep; a cached member stands in
// for a live one through CachedPortFeed. Exchange order and sub-iteration are
// the group's policy, never a member's.
class ISimulationGroupHost {
public:
	virtual ~ISimulationGroupHost() = default;
	virtual bool AddMember(const std::string& name, std::unique_ptr<ISimulationSolver> solver,
		const RISECBOR64::Value& parameters, std::string& error) = 0;
	virtual bool AddCachedMember(const std::string& name,
		std::shared_ptr<const IReadOnlyCacheSnapshot> snapshot, std::string& error) = 0;
	virtual bool Connect(const PortConnection& connection, std::string& error) = 0;
	virtual bool Configure(const GroupPolicy& policy, const ISceneInputs& inputs, std::string& error) = 0;
	virtual bool Start(std::string& error) = 0;
	virtual bool StepMacro(std::string& error) = 0;                // one exchange plus one macro step for all members
	virtual void RequestStop() = 0;
	virtual bool Checkpoint(Bytes& state, std::string& error) = 0;  // all members at one macro boundary
	virtual bool Restore(const Bytes& state, std::string& error) = 0;
	virtual std::shared_ptr<const IReadOnlyCacheSnapshot> Snapshot(const std::string& member) const = 0;
};

} }
#endif
