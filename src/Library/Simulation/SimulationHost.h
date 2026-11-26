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

// Host-side completion of a configured description: validates it (ports,
// inputs, side output), computes the cache key from the effective parameters
// and input identities, derives the epoch, and writes producer identity,
// fidelity (EffectiveFidelity), the evidence conditions and scope, and the
// scoped evidence digests (ScopedEvidenceDigest) into description.cache.
bool PrepareCacheDescription(SolverDescription& description, const ISceneInputs& inputs,
	const std::string& parentDigest, std::string& error);
// Deterministic epoch for a bake of `key` replacing the generation `parentDigest`
// (empty for a new cache): reruns are reproducible, rebakes always differ.
std::string DeriveEpoch(const std::string& key, const std::string& parentDigest);

namespace detail {
// Persistent (path-copying) 32-way radix vector: generations share all
// unchanged nodes, so push_back, set and truncate cost O(log32 n) and never
// copy the whole list. Elements are immutable once stored.
template<class T> class PersistentVector {
public:
	std::size_t size() const { return size_; }
	const T& operator[](std::size_t i) const {
		const Node* node=root_.get();
		for (unsigned shift=shift_; shift>0; shift-=Bits) node=node->children[(i>>shift)&Mask].get();
		return node->items[i&Mask];
	}
	PersistentVector push_back(T value) const {
		PersistentVector next=*this;
		if (!next.root_) next.shift_=0;
		else if (next.size_==(Width<<next.shift_)) {
			auto grown=std::make_shared<Node>();
			grown->children.push_back(next.root_);
			next.root_=std::move(grown);
			next.shift_+=Bits;
		}
		next.root_=Set(next.root_,next.shift_,next.size_,std::move(value));
		++next.size_;
		return next;
	}
	PersistentVector set(std::size_t i, T value) const {
		PersistentVector next=*this;
		next.root_=Set(next.root_,next.shift_,i,std::move(value));
		return next;
	}
	PersistentVector truncate(std::size_t n) const {
		if (n>=size_) return *this;
		PersistentVector next;
		if (n==0) return next;
		next.root_=Cut(root_,shift_,n-1);
		next.shift_=shift_;
		next.size_=n;
		while (next.shift_>0 && next.root_->children.size()==1) {
			next.root_=next.root_->children[0];
			next.shift_-=Bits;
		}
		return next;
	}
private:
	static constexpr unsigned Bits=5;
	static constexpr std::size_t Width=std::size_t(1)<<Bits, Mask=Width-1;
	struct Node {
		std::vector<std::shared_ptr<const Node>> children; // inner nodes
		std::vector<T> items;                              // leaves
	};
	using Ptr=std::shared_ptr<const Node>;
	static Ptr Set(const Ptr& node, unsigned shift, std::size_t i, T&& value) {
		auto copy=node ? std::make_shared<Node>(*node) : std::make_shared<Node>();
		if (shift==0) {
			const auto k=i&Mask;
			if (copy->items.size()<=k) copy->items.resize(k+1);
			copy->items[k]=std::move(value);
		} else {
			const auto k=(i>>shift)&Mask;
			if (copy->children.size()<=k) copy->children.resize(k+1);
			copy->children[k]=Set(copy->children[k],shift-Bits,i,std::move(value));
		}
		return copy;
	}
	static Ptr Cut(const Ptr& node, unsigned shift, std::size_t last) {
		auto copy=std::make_shared<Node>(*node);
		if (shift==0) copy->items.resize((last&Mask)+1);
		else {
			const auto k=(last>>shift)&Mask;
			copy->children.resize(k+1);
			copy->children[k]=Cut(copy->children[k],shift-Bits,last);
		}
		return copy;
	}
	Ptr root_;
	std::size_t size_=0;
	unsigned shift_=0;
};
} // namespace detail

// Immutable in-memory generation. Readers keep it as long as they like; the
// publisher never mutates a snapshot it has handed out. Generations share
// storage (detail::PersistentVector), so a publish is O(log n) in time and
// space; Entries() materialises the list once per generation (O(n)), so
// readers that follow a live run should use FindEntry and Count instead.
class MemoryCacheSnapshot final : public IReadOnlyCacheSnapshot {
public:
	const CacheDescription& Description() const override { return *description_; }
	const std::vector<CacheEntry>& Entries() const override;
	// Chain digest over the publication history (not a disk manifest digest).
	const std::string& Digest() const override { return digest_; }
	bool Complete() const override { return complete_; }
	FrameStatus Read(std::int64_t index, const std::string& output, std::size_t byteBudget,
		std::shared_ptr<const Bytes>& bytes, std::string& error) const override;
	const std::vector<CacheStatic>& Statics() const override;
	FrameStatus ReadStatic(const std::string& digest, std::size_t byteBudget,
		std::shared_ptr<const Bytes>& bytes, std::string& error) const override;
	bool FindEntry(std::int64_t index, const std::string& output, CacheEntry& entry) const override;
	std::size_t Count() const { return items_.size(); }  // entries, without materialising them
	std::size_t EntryCount() const override { return items_.size(); }
	OutputSpan Span(const std::string& output) const override;
private:
	friend class MemoryPublisher;
	struct Item { CacheEntry entry; std::shared_ptr<const Bytes> payload; };
	struct StaticSet {
		std::vector<CacheStatic> list;                     // sorted by digest
		std::vector<std::shared_ptr<const Bytes>> payloads; // parallel to list
	};
	std::size_t LowerBound(std::int64_t index, const std::string& output) const;
	std::shared_ptr<const CacheDescription> description_;
	detail::PersistentVector<Item> items_;
	std::vector<std::pair<std::string,OutputSpan>> spans_; // per output, O(outputs) per generation
	std::shared_ptr<const StaticSet> statics_;
	std::string digest_;
	bool complete_ = false;
	mutable std::once_flag materialised_;
	mutable std::vector<CacheEntry> entries_;
};

// Thread-safe publisher of MemoryCacheSnapshot generations. A republished
// (index, output) replaces the payload in later generations only. A native
// frame may only reference static payloads already published.
class MemoryPublisher {
public:
	void Reset(const CacheDescription& description);
	bool Publish(std::int64_t index, const std::string& output, double time,
		std::shared_ptr<const Bytes> payload, const Bytes& receipt, std::string& error);
	bool PublishStatic(const std::string& type, std::shared_ptr<const Bytes> payload, std::string& digest,
		std::string& error);
	// Later generations no longer hold frames with index >= `index`.
	void DiscardFrom(std::int64_t index);
	std::shared_ptr<const MemoryCacheSnapshot> Current() const;
private:
	std::shared_ptr<MemoryCacheSnapshot> Next(const char* event, const RISECBOR64::Value& row, std::string& error) const;
	mutable std::mutex mutex_;
	std::shared_ptr<const MemoryCacheSnapshot> current_;
};

// Steps one solver inside the application. Stepping happens either on the
// caller's thread (Step, StepToIndex) or on the host's own worker thread
// (Play, Pause, WaitIdle). Frames go to the in-memory publisher and, when
// recording, through a CacheWriter. Not copyable. The scene inputs passed to
// Configure must outlive the host (the solver may sample them while stepping).
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
	// A solver that declares a side output then gets <root>/<sideOutput>
	// (emptied, owner-only) unless SetSideOutputDirectory chose another.
	bool Record(const std::filesystem::path& root, std::size_t budget, std::string& error);
	// Optional, after Configure and before Start or RestoreCheckpoint: the
	// directory for a solver that declares SolverDescription::sideOutput, so
	// such a solver can also run in-process. Created owner-only if missing.
	bool SetSideOutputDirectory(const std::filesystem::path& directory, std::string& error);
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

	// While idle. Restore discards the frames published at or after the
	// checkpoint's next index from later snapshots (they are republished as
	// the run continues); a recording keeps its committed frames and refuses
	// conflicting bytes on republication.
	bool SaveCheckpoint(Checkpoint& checkpoint, std::string& error);
	bool RestoreCheckpoint(const Checkpoint& checkpoint, std::string& error);

private:
	class Sink;
	bool StepLocked(const AdvanceRequest& request, AdvanceResult& result, bool worker, std::string& error);
	bool PrepareSideOutput(std::string& error);
	void WorkerLoop();
	std::unique_ptr<ISimulationSolver> solver_;
	SolverDescription description_;
	MemoryPublisher publisher_;
	std::unique_ptr<CacheWriter> recorder_;
	std::filesystem::path recordRoot_, sideOutput_;
	IPortExchange* ports_ = nullptr;
	bool configured_ = false, started_ = false, sideOutputReady_ = false;
	SolverPosition position_;
	// Worker thread state, guarded by mutex_. stepMutex_ serializes solver calls.
	mutable std::mutex mutex_;
	std::mutex stepMutex_;
	std::condition_variable wake_;
	std::thread worker_;
	bool playing_ = false, stepping_ = false, shutdown_ = false, failed_ = false;
	bool stopPending_ = false;  // Pause requested a stop no Advance has consumed yet
	std::int64_t until_ = 0;
	std::string workerError_;
};

// ---------------------------------------------------------------- group host (interface; implemented in M6: SimulationGroup.h)

// How a co-stepping group orders the port exchange inside one macro step.
enum class ExchangeOrder {
	Jacobi,      // every member advances on the values exchanged at the start of the step
	GaussSeidel  // members advance in `order`; later members see earlier members' new offers
};
struct GroupPolicy {
	double macroStep = 0;                 // seconds of simulated time per exchange
	ExchangeOrder order = ExchangeOrder::GaussSeidel;
	std::vector<std::string> memberOrder; // for GaussSeidel; empty = insertion order
	unsigned subIterations = 1;           // >1: implicit coupling, repeat the step after RestoreState
	                                      // (every live member must report Capabilities::rollback)
	double convergenceTolerance = 0;      // stop sub-iterating once port change is below this; 0 = fixed count
	// M6, additive (contract Appendix C; the defaults reproduce the fields above alone):
	double relaxation = 1;                // initial under-relaxation factor omega in (0, 1] of the served values
	bool aitken = false;                  // Aitken's dynamic relaxation from the second iterate on
	double absoluteTolerance = 0;         // converged when |r| <= convergenceTolerance |value| + absoluteTolerance
	enum class NonConvergence { Fail, Halve };
	NonConvergence onNonConvergence = NonConvergence::Fail; // after subIterations without convergence
	double minimumMacroStep = 0;          // Halve: the floor (0 = macroStep / 64)
};

// Converts an offered port's payload into what an accepted port declares
// (unit conversion, resampling onto another index space or lattice, channel
// mapping). Solvers require exact matches (PortsCompatible); everything else
// is an adapter the group host inserts on a connection. Adapters are pure:
// the same input bytes give the same output bytes. Implemented in M6.
class IPortAdapter {
public:
	virtual ~IPortAdapter() = default;
	virtual bool Handles(const PortDeclaration& offer, const PortDeclaration& accept) const = 0;
	// `resolve` reads static payloads the offered payload references; the
	// result must be self-contained or reference only statics also offered.
	virtual bool Convert(const PortDeclaration& offer, const PortDeclaration& accept, const Bytes& offered,
		const StaticPayloadResolver& resolve, std::shared_ptr<const Bytes>& accepted, std::string& error) const = 0;
	// M6, additive (contract Appendix C), defaults keep a plain Convert-only adapter working:
	//! Name, version and canonical parameters as one digest; it enters the cache key of the
	//! member fed through the adapter (empty: the registry id is used).
	virtual std::string Identity() const { return std::string(); }
	//! The label the adapter's output may carry: "predictive" only for an exact transfer whose
	//! own evidence covers it (unit factor, relabel, rigid placement); a model is "preview".
	virtual std::string Fidelity() const { return "preview"; }
	//! One sentence: what the conversion is and what it does not cover.
	virtual std::string Scope() const { return std::string(); }
	//! Convert with the payloads of other offered ports the connection names
	//! (PortConnection::auxiliary, in order; null when nothing was offered yet).
	virtual bool ConvertWithAuxiliary(const PortDeclaration& offer, const PortDeclaration& accept, const Bytes& offered,
		const std::vector<std::shared_ptr<const Bytes>>& auxiliary, const StaticPayloadResolver& resolve,
		std::shared_ptr<const Bytes>& accepted, std::string& error) const
	{
		(void)auxiliary;
		return Convert(offer, accept, offered, resolve, accepted, error);
	}
};
// Process-wide, thread-safe adapter registry; a duplicate id is refused.
bool RegisterPortAdapter(const std::string& id, std::shared_ptr<const IPortAdapter> adapter, std::string& error);
// The first registered adapter that handles the pair, or null.
std::shared_ptr<const IPortAdapter> FindPortAdapter(const PortDeclaration& offer, const PortDeclaration& accept);
struct PortConnection {
	std::string fromMember, fromPort;     // an offered port (or a cached member's output)
	std::string toMember, toPort;         // an accepted port
	// M6, additive (contract Appendix C):
	std::string adapter;                  // registered adapter (factory) id; empty: none, or the first that Handles
	RISECBOR64::Value adapterParameters = RISECBOR64::Value::MapValue({}); // the adapter's parameters
	std::vector<std::pair<std::string,std::string>> auxiliary; // (member, offered port) the adapter also reads
};

// Runs solvers that are two-way coupled with a shared macro step. Every live
// member must report Capabilities::externalMacroStep; a cached member stands in
// for a live one through CachedPortFeed. Exchange order and sub-iteration are
// the group's policy, never a member's. Connections whose declarations are
// not PortsCompatible go through a registered IPortAdapter or are refused.
// While sub-iterating, frames a member publishes during a repeated step are
// held back until the step is accepted.
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
