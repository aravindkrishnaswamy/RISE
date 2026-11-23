// Solver contract, frozen at the end of M3 (kSolverContractVersion 1). The
// specification is docs/SIMULATION_SOLVER_CONTRACT.md; this header states the
// rules briefly next to the declarations.
//
// A solver advances a state through time and publishes typed frames. It knows
// nothing about files, UI, threads or the timeline: a host owns all of those.
// Frames cross this boundary as encoded payloads in the cache codecs
// ("grid.v1", "mesh.v2", "field.v1", ...), so the cache writer, the
// in-process publisher and the renderer share one representation. Nothing
// here is solver-specific.
//
// Call order: Configure, then Start or Restore, then any number of Advance
// calls. Checkpoint, SaveState and RestoreState are valid between Advance
// calls (including after an Advance that returned because of a stop).
// RequestStop may be called from any thread at any time. All other members
// are called from one thread at a time.
#ifndef RISE_SIMULATION_SOLVER_H
#define RISE_SIMULATION_SOLVER_H

#include "SimulationCache.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RISE { namespace Simulation {

// Bumped only for a change an existing solver or host could observe:
// removing or renaming a member, changing a signature, a default or the
// meaning of a field or call. Additive, defaulted members leave it unchanged
// (docs/SIMULATION_SOLVER_CONTRACT.md "Versioning").
constexpr std::uint32_t kSolverContractVersion = 1;

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
	bool interactive = false;         // cheap enough to step live in an app (this configuration)
	bool deterministic = false;       // identical inputs give identical payload bytes
	bool resumable = false;           // Checkpoint/Restore reproduce an uninterrupted run
	bool externalMacroStep = false;   // honours AdvanceRequest::Mode::MacroStep
	bool rollback = false;            // SaveState/RestoreState repeat a macro step exactly
	bool threadCountInvariant = false;// payload bytes do not depend on the solver's thread count
	std::vector<std::string> modes;   // operating modes the solver offers: "certified"/"fast", "bake"/"interactive"
	std::vector<std::string> earnableFidelity; // labels this configuration can earn: "preview", "predictive"
	std::string fidelityLimit;        // why "predictive" is not earnable here (empty when it is)
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
	// What the label covers: machine-readable conditions (e.g. mode, mesh
	// family, contact kinds; EvidenceConditions rule) and one sentence. Both
	// are recorded in the cache description and bound into the evidence
	// digests the host writes. "predictive" requires a scope sentence.
	EvidenceConditions conditions;
	std::string scope;
};

// "predictive" only when requested, a scope is stated, the conditions are
// valid, and every case is present, consistent and within tolerance (finite
// error <= finite tolerance, passed, valid digest); otherwise "preview".
// Hosts write this label, not the requested one.
std::string EffectiveFidelity(const Evidence& evidence);
// Digest for a case: SHA-256 of canonical CBOR of (id, configuration, error, tolerance, passed).
std::string EvidenceDigest(const std::string& id, const RISECBOR64::Value& configuration,
	double error, double tolerance, bool passed);
// The digest a host records for a case: the case digest itself when the
// evidence states no conditions and no scope, else SHA-256 of canonical CBOR
// of (case digest, conditions, scope).
std::string ScopedEvidenceDigest(const std::string& caseDigest, const EvidenceConditions& conditions,
	const std::string& scope);

// One named channel of a port payload: a grid channel, a field.v1 channel or
// a mesh per-vertex array, with its physical quantity, units and components.
struct PortChannel {
	std::string name, quantity, units;
	std::uint8_t components = 1;
};

// A coupling port. Offered ports are written by the solver through
// IPortExchange::Offer; accepted ports are read through Accept. `output`
// names the cache output carrying the same payloads; empty means the port is
// live-only: it works between live solvers but cannot be fed from a cache.
// `domain` is the index-space digest of a field.v1 port when it is fixed by
// the configuration (empty: decided per payload).
struct PortDeclaration {
	std::string name, quantity, units, codec;
	bool offers = false;              // true: offers; false: accepts
	std::string output;
	std::vector<PortChannel> channels;
	std::string domain;
};
// Offer -> accept binding check: codec, quantity, units and the channel list
// must match exactly, and the domains when both are stated. Anything else
// needs a port adapter in the group host (SimulationHost.h).
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
	// producerVersion, fidelity, evidence and the evidence scope fields and
	// overwrites them.
	CacheDescription cache;
	std::vector<std::string> inputs;  // scene inputs read (payloads or transforms), in key order, unique
	std::vector<PortDeclaration> ports;
	Capabilities capabilities;
	Evidence evidence;
	// Solver-owned side outputs (exports in a solver's own format beside the
	// cache, e.g. a legacy file sequence): a plain directory leaf name, empty
	// for none. A host that keeps the cache on disk creates <cache>/<sideOutput>
	// (owner-only) and passes it to SetSideOutputDirectory before Start or
	// Restore; a new epoch starts with an empty one. Not part of the cache key.
	std::string sideOutput;
	// Human-readable configuration notes (for example which parameter values
	// came from legacy environment defaults). Not part of the cache key.
	std::vector<std::string> notes;
};
// Side-output leaf rule: nonempty, no separators, not "." or "..", no leading
// dot, and none of the cache's or the worker's own names.
bool ValidSideOutputName(const std::string& name);

// ---------------------------------------------------------------- host services

// A rigid map x -> rotation * x + translation (rotation row-major, proper
// orthonormal; metres).
struct RigidTransform {
	std::array<double,9> rotation{{1,0,0, 0,1,0, 0,0,1}};
	std::array<double,3> translation{{0,0,0}};
};

// Scene elements a solver reads. Identity is the content hash of the named
// input over the whole simulated span (it enters the cache key); Sample is
// the encoded input payload at one solver time; SampleTransform is a named
// rigid motion at any solver time (inside a step too). Const members may be
// called concurrently from the solver's own threads. The object passed to
// Configure stays valid until the solver is configured again or destroyed.
class ISceneInputs {
public:
	virtual ~ISceneInputs() = default;
	virtual bool Identity(const std::string& name, std::string& hash, std::string& error) const = 0;
	virtual bool Sample(const std::string& name, double time, std::shared_ptr<const Bytes>& encoded,
		std::string& codec, std::string& error) const = 0;
	virtual bool SampleTransform(const std::string& name, double time, RigidTransform& transform,
		std::string& error) const;
	// A static payload a sampled payload references (e.g. a mesh.v2 topology),
	// verified against its digest; default: none.
	virtual bool ResolveStatic(const std::string& digest, std::size_t budget, std::shared_ptr<const Bytes>& payload,
		std::string& error) const;
};
// A scene with no inputs (every lookup fails).
class NoSceneInputs final : public ISceneInputs {
public:
	bool Identity(const std::string& name, std::string& hash, std::string& error) const override;
	bool Sample(const std::string& name, double time, std::shared_ptr<const Bytes>& encoded,
		std::string& codec, std::string& error) const override;
};
// A process-lifetime empty scene, for hosts that bake without one.
const ISceneInputs& EmptySceneInputs();

// Receives each published frame. Ownership of the payload passes to the sink
// (shared, immutable): the solver must not modify it afterwards. Each
// (index, output) pair is published once per run; a resumed run may publish
// an already committed pair again, and must then produce identical bytes.
// A frame may reference static payloads (by content digest) only after they
// were published to the same sink in the same run (after Start or Restore);
// publishing a static payload again is a no-op.
class IFrameSink {
public:
	virtual ~IFrameSink() = default;
	virtual bool Publish(std::int64_t index, const std::string& output, double time,
		std::shared_ptr<const Bytes> payload, const Bytes& receipt, std::string& error) = 0;
	virtual bool PublishStatic(const std::string& type, std::shared_ptr<const Bytes> payload, std::string& digest,
		std::string& error);
};

enum class PortStatus { Ready, Unbound, Unavailable, Failed };

// Coupling exchange (docs/SIMULATION_SOLVER_CONTRACT.md "Ports and coupling").
// A solver Accepts at the start of each macro step (time = the step start)
// and holds the value over the step; it Offers at the end of each step,
// stamped with the step-end time. Accept returns the latest value at or
// before `time`. A port can be backed by a live solver (PortRelay) or a
// cached output (CachedPortFeed); the solver cannot tell which. Payloads may
// reference static payloads: the offering solver offers them first
// (OfferStatic), the accepting solver resolves them (ResolveStatic).
class IPortExchange {
public:
	virtual ~IPortExchange() = default;
	virtual bool Offer(const std::string& port, double time, std::shared_ptr<const Bytes> encoded,
		std::string& error) = 0;
	virtual PortStatus Accept(const std::string& port, double time, std::shared_ptr<const Bytes>& encoded,
		std::string& error) = 0;
	virtual bool OfferStatic(const std::string& type, std::shared_ptr<const Bytes> payload, std::string& error);
	virtual PortStatus ResolveStatic(const std::string& digest, std::size_t budget,
		std::shared_ptr<const Bytes>& payload, std::string& error);
	// ResolveStatic as a codec resolver.
	StaticPayloadResolver Resolver();
};

class IProgress {
public:
	virtual ~IProgress() = default;
	// endTime is the time of the last output (startTime + (count-1) x interval).
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

// An in-memory snapshot of a solver's state for implicit coupling: cheap to
// take, valid only for the solver instance and configuration that made it.
class SolverStateToken {
public:
	virtual ~SolverStateToken() = default;
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
	// The directory for SolverDescription::sideOutput, given by a host after
	// Configure and before Start or Restore. Solvers without side outputs
	// keep the default; a solver that needs one refuses Start without it.
	virtual bool SetSideOutputDirectory(const std::filesystem::path& directory, std::string& error)
	{
		(void)directory; (void)error; return true;
	}
	// Thread-safe and sticky: the Advance in progress, or the next one if none
	// is, returns Stopped at its next safe point. The request is cleared only
	// when an Advance consumes it by returning Stopped.
	virtual void RequestStop() = 0;
	// Rollback (Capabilities::rollback): SaveState captures everything that
	// determines later payloads (position, state, held port values); after
	// RestoreState the solver continues exactly as from the moment of
	// SaveState, re-reading its accepted ports at the next macro step. A token
	// may be restored any number of times. Default: unsupported.
	virtual bool SaveState(std::shared_ptr<const SolverStateToken>& token, std::string& error)
	{
		(void)token; error = "this solver does not support rollback"; return false;
	}
	virtual bool RestoreState(const SolverStateToken& token, std::string& error)
	{
		(void)token; error = "this solver does not support rollback"; return false;
	}
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
// to accepted port names (accepted -> offered). Static payloads offered by any
// port resolve for every port.
class PortRelay final : public IPortExchange {
public:
	void Connect(const std::string& acceptedPort, const std::string& offeredPort);
	bool Offer(const std::string& port, double time, std::shared_ptr<const Bytes> encoded,
		std::string& error) override;
	PortStatus Accept(const std::string& port, double time, std::shared_ptr<const Bytes>& encoded,
		std::string& error) override;
	bool OfferStatic(const std::string& type, std::shared_ptr<const Bytes> payload, std::string& error) override;
	PortStatus ResolveStatic(const std::string& digest, std::size_t budget, std::shared_ptr<const Bytes>& payload,
		std::string& error) override;
private:
	struct Sample { double time; std::shared_ptr<const Bytes> bytes; };
	std::vector<std::pair<std::string,std::string>> routes_;
	std::vector<std::pair<std::string,std::vector<Sample>>> offers_;
	std::vector<std::pair<std::string,std::shared_ptr<const Bytes>>> statics_; // by digest
};

// Feeds accepted ports from outputs of an immutable cache snapshot (one-way
// coupling from a baked or imported simulation). Offers are discarded;
// static payloads resolve from the snapshot.
class CachedPortFeed final : public IPortExchange {
public:
	CachedPortFeed(std::shared_ptr<const IReadOnlyCacheSnapshot> snapshot, std::size_t byteBudget);
	void Bind(const std::string& acceptedPort, const std::string& output);
	bool Offer(const std::string& port, double time, std::shared_ptr<const Bytes> encoded,
		std::string& error) override;
	PortStatus Accept(const std::string& port, double time, std::shared_ptr<const Bytes>& encoded,
		std::string& error) override;
	PortStatus ResolveStatic(const std::string& digest, std::size_t budget, std::shared_ptr<const Bytes>& payload,
		std::string& error) override;
private:
	std::shared_ptr<const IReadOnlyCacheSnapshot> snapshot_;
	std::size_t budget_;
	std::vector<std::pair<std::string,std::string>> bindings_;
};

} }
#endif
