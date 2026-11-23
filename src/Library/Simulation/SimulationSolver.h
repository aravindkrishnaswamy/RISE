// Solver contract (M2, docs/SIMULATION_M2_DESIGN.md). Open to change until M3.
//
// A solver advances a state through time and publishes typed frames. It knows
// nothing about files, UI, threads or the timeline: a host owns all of those.
// Frames cross this boundary as encoded payloads in the cache codecs
// ("grid.v1", "mesh.v1", ...), so the cache writer, the in-process publisher
// and the renderer share one representation. Nothing here is solver-specific.
//
// Call order: Configure, then Start or Restore, then any number of Advance
// calls. Checkpoint is valid between Advance calls (including after an Advance
// that returned because of RequestStop). RequestStop may be called from any
// thread at any time. All other members are called from one thread at a time.
#ifndef RISE_SIMULATION_SOLVER_H
#define RISE_SIMULATION_SOLVER_H

#include "SimulationCache.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RISE { namespace Simulation {

// ---------------------------------------------------------------- description

enum class BackendStatus { Supported, Experimental, Unsupported };
const char* BackendStatusName(BackendStatus status);

struct BackendInfo {
	std::string name;                 // "cpu", "metal", ...
	BackendStatus status = BackendStatus::Unsupported;
	bool available = false;           // usable on this machine now
	std::string detail;
};

struct Capabilities {
	std::vector<BackendInfo> backends;
	bool interactive = false;         // cheap enough to step live in an app
	bool deterministic = false;       // identical inputs give identical payload bytes
	bool resumable = false;           // Checkpoint/Restore reproduce an uninterrupted run
	bool externalMacroStep = false;   // honours AdvanceRequest::Mode::MacroStep
	std::vector<std::string> fidelityModes; // labels this solver can earn: "preview", "predictive"
	double costEstimate = 0;          // seconds per simulated second on the reference machine; 0 = unknown
	std::uint64_t memoryEstimate = 0; // peak bytes; 0 = unknown
};

// One validation case. The solver computes error from an actual check; the
// host derives the label from error/tolerance, never from `passed` alone.
struct EvidenceCase {
	std::string id, description;
	std::string digest;               // SHA-256 hex binding the case, its configuration and its result
	double error = 0, tolerance = 0;
	bool passed = false;
};
struct Evidence {
	std::string fidelity = "preview"; // label the solver requests: "preview" | "predictive"
	std::vector<EvidenceCase> cases;
};

// "predictive" only when requested and every case is present, consistent and
// within tolerance (finite error <= finite tolerance, passed, valid digest);
// otherwise "preview". Hosts write this label, not the requested one.
std::string EffectiveFidelity(const Evidence& evidence);
// Digest for a case: SHA-256 of canonical CBOR of (id, configuration, error, tolerance, passed).
std::string EvidenceDigest(const std::string& id, const RISECBOR64::Value& configuration,
	double error, double tolerance, bool passed);

// A coupling port. Offered ports are written by the solver through
// IPortExchange::Offer; accepted ports are read through Accept. `output`
// names the cache output carrying the same payloads (empty when the port has
// no cached counterpart), so a cached run can stand in for a live solver.
struct PortDeclaration {
	std::string name, quantity, units, codec;
	bool offers = false;              // true: offers; false: accepts
	std::string output;
};
// Offer -> accept binding check: codec, quantity and units must match exactly.
bool PortsCompatible(const PortDeclaration& offer, const PortDeclaration& accept, std::string& error);

struct SolverDescription {            // filled by Configure
	std::string solverId, version;
	// Canonical effective parameters: defaults applied, values that do not
	// change results removed. Configure(effectiveParameters) must reproduce
	// the same description. The cache key is
	// MakeCacheKey(solverId, version, effectiveParameters, input identities).
	RISECBOR64::Value effectiveParameters;
	// The solver fills time support (firstIndex, count, startTime, interval,
	// hold), outputs and physicalRecords. The host owns key, epoch, producer,
	// producerVersion, fidelity and evidence and overwrites them.
	CacheDescription cache;
	std::vector<std::string> inputs;  // scene inputs read, in key order
	std::vector<PortDeclaration> ports;
	Capabilities capabilities;
	Evidence evidence;
	// Solver-owned side outputs (exports in a solver's own format beside the
	// cache, e.g. a legacy file sequence): a plain directory leaf name, empty
	// for none. A host that keeps the cache on disk creates <cache>/<sideOutput>
	// and passes it to SetSideOutputDirectory before Start or Restore; a new
	// epoch starts with an empty one. Not part of the cache key.
	std::string sideOutput;
	// Human-readable configuration notes (for example which parameter values
	// came from legacy environment defaults). Not part of the cache key.
	std::vector<std::string> notes;
};
// Side-output leaf rule: nonempty, no separators, not "." or "..", no leading
// dot, and none of the cache's or the worker's own names.
bool ValidSideOutputName(const std::string& name);

// ---------------------------------------------------------------- host services

// Scene elements a solver reads. Identity is the content hash of the named
// input over the whole simulated span (it enters the cache key); Sample is
// the encoded input at one solver time.
class ISceneInputs {
public:
	virtual ~ISceneInputs() = default;
	virtual bool Identity(const std::string& name, std::string& hash, std::string& error) const = 0;
	virtual bool Sample(const std::string& name, double time, std::shared_ptr<const Bytes>& encoded,
		std::string& codec, std::string& error) const = 0;
};
// A scene with no inputs (every lookup fails).
class NoSceneInputs final : public ISceneInputs {
public:
	bool Identity(const std::string& name, std::string& hash, std::string& error) const override;
	bool Sample(const std::string& name, double time, std::shared_ptr<const Bytes>& encoded,
		std::string& codec, std::string& error) const override;
};

// Receives each published frame. Ownership of the payload passes to the sink
// (shared, immutable): the solver must not modify it afterwards. Each
// (index, output) pair is published once per run; a resumed run may publish
// an already committed pair again, and must then produce identical bytes.
class IFrameSink {
public:
	virtual ~IFrameSink() = default;
	virtual bool Publish(std::int64_t index, const std::string& output, double time,
		std::shared_ptr<const Bytes> payload, const Bytes& receipt, std::string& error) = 0;
};

enum class PortStatus { Ready, Unbound, Unavailable, Failed };

// Coupling exchange at macro-step boundaries. Accept returns the latest value
// at or before `time`. A port can be backed by a live solver (PortRelay) or a
// cached output (CachedPortFeed); the solver cannot tell which.
class IPortExchange {
public:
	virtual ~IPortExchange() = default;
	virtual bool Offer(const std::string& port, double time, std::shared_ptr<const Bytes> encoded,
		std::string& error) = 0;
	virtual PortStatus Accept(const std::string& port, double time, std::shared_ptr<const Bytes>& encoded,
		std::string& error) = 0;
};

class IProgress {
public:
	virtual ~IProgress() = default;
	// Returning false requests a stop at the next safe point.
	virtual bool Report(double simulatedTime, double endTime, const std::string& detail) = 0;
};

// ---------------------------------------------------------------- stepping

struct AdvanceRequest {
	enum class Mode {
		NextOutput, // advance until the next output index has been published (or the end)
		MacroStep   // advance exactly to macroEnd, publishing every output index reached on the way
	};
	Mode mode = Mode::NextOutput;
	double macroEnd = 0;
};

struct SolverPosition {
	double time = 0;               // current simulated time
	std::int64_t nextIndex = 0;    // next output index to publish
	bool finished = false;         // every output index has been published
};

struct AdvanceResult {
	enum class Status { Advanced, Finished, Stopped };
	Status status = Status::Advanced;
	SolverPosition position;
	std::uint64_t framesPublished = 0; // (index, output) payloads published by this call
};

class ISimulationSolver {
public:
	virtual ~ISimulationSolver() = default;
	virtual bool Configure(const RISECBOR64::Value& parameters, const ISceneInputs& inputs,
		SolverDescription& description, std::string& error) = 0;
	virtual bool Start(std::string& error) = 0;                       // initial state
	virtual bool Restore(const Bytes& checkpoint, std::string& error) = 0; // instead of Start
	// May return Stopped at a safe point before reaching the target; the state
	// is then valid for Checkpoint and a later Advance continues the same
	// sequence of substeps. `ports` and `progress` may be null.
	virtual bool Advance(const AdvanceRequest& request, IFrameSink& sink, IPortExchange* ports,
		IProgress* progress, AdvanceResult& result, std::string& error) = 0;
	virtual bool Checkpoint(Bytes& state, std::string& error) = 0; // opaque, solver-versioned
	virtual SolverPosition Position() const = 0;                  // after Start/Restore/Advance
	// The directory for SolverDescription::sideOutput, given by a disk host
	// after Configure and before Start or Restore. Solvers without side outputs
	// keep the default; a solver that needs one refuses Start without it.
	virtual bool SetSideOutputDirectory(const std::filesystem::path& directory, std::string& error)
	{
		(void)directory; (void)error; return true;
	}
	// Thread-safe. The Advance in progress returns Stopped at its next safe
	// point. A request is cleared when an Advance begins, so hosts that stop
	// between calls check their own flag (or return false from IProgress).
	virtual void RequestStop() = 0;
};

// ---------------------------------------------------------------- registry

using SolverFactory = std::function<std::unique_ptr<ISimulationSolver>()>;
// Process-wide and thread-safe. Ids are NFC text; a duplicate id is refused.
bool RegisterSolver(const std::string& id, SolverFactory factory, std::string& error);
std::unique_ptr<ISimulationSolver> CreateSolver(const std::string& id, std::string& error);
std::vector<std::string> RegisteredSolvers();

// ---------------------------------------------------------------- ports

// Live relay: Offer stores payloads per port, Accept returns the latest at or
// before the requested time. Optional name mapping connects offered port names
// to accepted port names (accepted -> offered).
class PortRelay final : public IPortExchange {
public:
	void Connect(const std::string& acceptedPort, const std::string& offeredPort);
	bool Offer(const std::string& port, double time, std::shared_ptr<const Bytes> encoded,
		std::string& error) override;
	PortStatus Accept(const std::string& port, double time, std::shared_ptr<const Bytes>& encoded,
		std::string& error) override;
private:
	struct Sample { double time; std::shared_ptr<const Bytes> bytes; };
	std::vector<std::pair<std::string,std::string>> routes_;
	std::vector<std::pair<std::string,std::vector<Sample>>> offers_;
};

// Feeds accepted ports from outputs of an immutable cache snapshot (one-way
// coupling from a baked or imported simulation). Offers are discarded.
class CachedPortFeed final : public IPortExchange {
public:
	CachedPortFeed(std::shared_ptr<const IReadOnlyCacheSnapshot> snapshot, std::size_t byteBudget);
	void Bind(const std::string& acceptedPort, const std::string& output);
	bool Offer(const std::string& port, double time, std::shared_ptr<const Bytes> encoded,
		std::string& error) override;
	PortStatus Accept(const std::string& port, double time, std::shared_ptr<const Bytes>& encoded,
		std::string& error) override;
private:
	std::shared_ptr<const IReadOnlyCacheSnapshot> snapshot_;
	std::size_t budget_;
	std::vector<std::pair<std::string,std::string>> bindings_;
};

} }
#endif
