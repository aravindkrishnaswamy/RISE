// General time-indexed simulation cache. File paths are confined to a cache root.
// Frames are time-indexed payloads; static payloads are immutable,
// content-addressed blobs declared once in the manifest that frame payloads
// may reference by digest (docs/SIMULATION_SOLVER_CONTRACT.md, "Cache").
#ifndef RISE_SIMULATION_CACHE_H
#define RISE_SIMULATION_CACHE_H

#include "../Utilities/RISECBOR64.h"
#include "SimulationMesh.h"
#include <filesystem>
#include <functional>
#include <memory>

namespace RISE { namespace Simulation {

using Bytes = RISECBOR64::Bytes;

struct CacheChannel {
	std::string name, units, representation;
};
struct CacheOutput {
	std::string name, type; // native codecs: "grid.v1", "mesh.v1", "mesh.v2", "field.v1" (IsNativePayloadType)
	std::vector<CacheChannel> channels;
};
// What a fidelity label's evidence covers, machine-readable: condition names
// (strictly increasing, nonempty NFC text), each with a nonempty strictly
// increasing set of values, e.g. {"contact", {"barrier"}}, {"mode", {"bake"}}.
using EvidenceConditions = std::vector<std::pair<std::string, std::vector<std::string>>>;
struct CacheDescription {
	std::string key, epoch, producer, producerVersion;
	std::int64_t firstIndex = 0;
	std::uint64_t count = 0;
	double startTime = 0, interval = 1, sceneOrigin = 0, timeScale = 1;
	bool hold = true;
	std::vector<CacheOutput> outputs;
	std::string fidelity = "preview";
	std::vector<std::string> evidence; // content digests tied to producer and configuration
	Bytes physicalRecords; // opaque producer-specific metadata
	// Scope of the evidence (both recorded only when nonempty; a manifest that
	// carries either is format v2). The host binds both into `evidence`.
	EvidenceConditions evidenceConditions;
	std::string evidenceScope;           // one human-readable sentence
};
// A static payload: written once, immutable, addressed by its SHA-256.
struct CacheStatic {
	std::string digest, type;            // type: a static payload codec, e.g. "mesh.topology.v1"
	std::uint64_t size = 0;
};
struct CacheEntry {
	std::int64_t index = 0;
	std::string output;
	double time = 0;
	std::uint64_t size = 0;
	std::string digest;
	Bytes receipt; // opaque producer provenance for this output at this index
};
enum class FrameStatus { Ready, Unavailable, Corrupt, Unsupported, BudgetExceeded };
enum class CacheState { Missing, Baking, Partial, Complete, Corrupt };

// Size and allocation preflight only. Call DecodeCanonical afterwards for actual
// schema and canonicality validation. Use on each separately encoded record.
bool CanDecodeCanonicalRecord(const Bytes& encoded, std::size_t budget);

// Payload types a cache reader serves; any other declared type reads as Unsupported.
bool IsNativePayloadType(const std::string& type);
// Static payload types a native frame type may reference ("mesh.topology.v1").
bool IsNativeStaticType(const std::string& type);
// Static payload digests a native frame payload references (none for most types).
bool NativePayloadReferences(const std::string& type, const Bytes& payload, std::vector<std::string>& digests,
	std::string& error);
// Evidence conditions rule (see EvidenceConditions).
bool ValidEvidenceConditions(const EvidenceConditions& conditions);
// The identity, time-support, output and fidelity rules CacheWriter::Begin enforces.
bool ValidateCacheDescription(const CacheDescription& description, std::string& error);

// Durably replace one small sidecar file in a directory: stage, sync, rename,
// sync the directory. `name` is a plain leaf (no separators) and may not be a
// cache-owned name (manifest, lock, payloads). Callers serialize writers of the
// same name (a hosting worker does so under the cache writer lock).
bool ReplaceFileDurably(const std::filesystem::path& directory, const std::string& name,
	const Bytes& bytes, std::string& error);
// The same, writing the concatenation of `parts` without assembling it.
bool ReplaceFileDurably(const std::filesystem::path& directory, const std::string& name,
	const std::vector<std::pair<const std::uint8_t*, std::size_t>>& parts, std::string& error);
// Bounded read of such a sidecar; refuses symlinks and nonregular files.
bool ReadSidecarFile(const std::filesystem::path& directory, const std::string& name,
	std::size_t budget, Bytes& bytes, std::string& error);

// ---- cache location policy (POSIX; see docs/SIMULATION_SOLVER_CONTRACT.md "Cache location policy")
//
// The writer refuses a cache directory with an ancestor that another user
// could rename or replace (group- or world-writable without the sticky bit,
// owned by someone other than this user or root, or carrying an ACL that
// grants rights), and creates every missing directory owner-only (0700). The
// cache directory itself must be a real directory (not a symlink) owned by
// this user and writable by nobody else. A trusted root stops the ancestor
// check: for a directory below a trusted root only the components below the
// root are checked; the root and its ancestors are trusted as they are.
// Trusted roots come from RISE_SIMULATION_TRUSTED_ROOTS (colon-separated
// absolute paths; other entries are ignored) and from AddTrustedCacheRoot.
constexpr const char* TrustedCacheRootsEnvironment = "RISE_SIMULATION_TRUSTED_ROOTS";
// Adds an existing absolute directory (canonicalised now) for this process.
bool AddTrustedCacheRoot(const std::filesystem::path& root, std::string& error);
void ClearTrustedCacheRoots();                        // the API-added ones; the environment is read on use
std::vector<std::filesystem::path> TrustedCacheRoots(); // canonical: environment, then API
// Would the writer accept (and, with `create`, create) this cache directory?
bool CheckCacheLocation(const std::filesystem::path& root, bool create, std::string& error);

bool MakeCacheKey(const std::string& solver, const std::string& version,
	const RISECBOR64::Value& effectiveParameters,
	const std::vector<std::pair<std::string,std::string>>& orderedInputHashes,
	std::string& key, std::string& error);

// Disk and in-process publishers pass a shared immutable snapshot across the
// render boundary. Read's budget is the remaining allowance for this frame.
class IReadOnlyCacheSnapshot {
public:
	virtual ~IReadOnlyCacheSnapshot() = default;
	virtual const CacheDescription& Description() const = 0;
	virtual const std::vector<CacheEntry>& Entries() const = 0;
	virtual const std::string& Digest() const = 0;
	virtual bool Complete() const = 0;
	virtual FrameStatus Read(std::int64_t index, const std::string& output, std::size_t byteBudget,
		std::shared_ptr<const Bytes>& bytes, std::string& error) const = 0;
	// Declared static payloads, sorted by digest (none by default).
	virtual const std::vector<CacheStatic>& Statics() const;
	// A declared static payload, verified against its digest like a frame.
	virtual FrameStatus ReadStatic(const std::string& digest, std::size_t byteBudget,
		std::shared_ptr<const Bytes>& bytes, std::string& error) const;
	// One entry by (index, output); the default searches Entries().
	virtual bool FindEntry(std::int64_t index, const std::string& output, CacheEntry& entry) const;
	// Resolves frame references through ReadStatic (the snapshot must outlive it).
	StaticPayloadResolver Resolver() const;
	bool Stale(const std::string& expectedKey) const { return expectedKey != Description().key; }
	bool MapTime(double sceneTime, std::int64_t& index, std::string& error) const;
	// The consumer supplies trusted qualification checks: bind this snapshot to
	// the expected producer/input evidence, then validate each verified payload.
	struct Qualification {
		std::function<bool(const IReadOnlyCacheSnapshot&,std::string&)> snapshot;
		std::function<bool(const CacheEntry&,const Bytes&,std::string&)> frame;
	};
	bool PreflightPredictive(const std::string& expectedKey,const Qualification& qualification,
		std::size_t byteBudget,std::string& error) const;
};

class CacheSnapshot final : public IReadOnlyCacheSnapshot {
public:
	static bool Open(const std::filesystem::path& root, std::size_t budget,
		CacheSnapshot& snapshot, std::string& error);
	const CacheDescription& Description() const override { return description_; }
	const std::vector<CacheEntry>& Entries() const override { return entries_; }
	const std::string& Digest() const override { return digest_; }
	bool Complete() const override { return complete_; }
	FrameStatus Read(std::int64_t index, const std::string& output, std::size_t byteBudget,
		std::shared_ptr<const Bytes>& bytes, std::string& error) const override;
	const std::vector<CacheStatic>& Statics() const override { return statics_; }
	FrameStatus ReadStatic(const std::string& digest, std::size_t byteBudget,
		std::shared_ptr<const Bytes>& bytes, std::string& error) const override;
private:
	friend class CacheWriter;
	CacheDescription description_;
	std::vector<CacheEntry> entries_;
	std::vector<CacheStatic> statics_;
	std::filesystem::path root_;
	std::string digest_;
	bool complete_ = false;
};

CacheState InspectCache(const std::filesystem::path& root, std::size_t budget, std::string& error);

class CacheWriter {
public:
	CacheWriter();
	~CacheWriter();
	CacheWriter(const CacheWriter&) = delete;
	CacheWriter& operator=(const CacheWriter&) = delete;
	// expectedParent is empty only when no manifest exists. A new epoch must differ
	// from the old one. Resume uses persisted descriptors, checks key and epoch.
	bool Begin(const std::filesystem::path& root, const CacheDescription& description,
		const std::string& expectedParent, bool resume, std::size_t budget, std::string& error);
	// Serialize demand imports, creating the first generation or extending the
	// compatible current one. Existing immutable payloads are verified on use.
	// Calling this on a matching sealed cache permits idempotent entry checks.
	bool PrepareExtend(const std::filesystem::path& root, const CacheDescription& description,
		std::size_t budget,std::string& error);
	// A frame of a native type may only reference declared static payloads.
	bool Append(std::int64_t index, const std::string& output, double actualTime,
		const Bytes& frame, std::string& error, const Bytes& receipt = {});
	// Declares and writes a static payload once (idempotent for identical bytes,
	// also on a sealed cache when already declared); returns its digest.
	bool AppendStatic(const std::string& type, const Bytes& payload, std::string& digest, std::string& error);
	bool Seal(std::string& error);
	const CacheSnapshot& Snapshot() const { return snapshot_; }
private:
	enum class OpenMode { NewEpoch, ResumeVerified, ExtendCurrent };
	bool Open(const std::filesystem::path& root,const CacheDescription& description,
		const std::string& expectedParent,OpenMode mode,std::size_t budget,std::string& error);
	bool Publish(CacheSnapshot& next, std::string& error);
	struct Lock;
	std::unique_ptr<Lock> lock_;
	CacheSnapshot snapshot_;
	std::size_t budget_ = 0;
};

} }
#endif
