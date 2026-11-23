// General time-indexed simulation cache. File paths are confined to a cache root.
#ifndef RISE_SIMULATION_CACHE_H
#define RISE_SIMULATION_CACHE_H

#include "../Utilities/RISECBOR64.h"
#include <filesystem>
#include <functional>
#include <memory>

namespace RISE { namespace Simulation {

using Bytes = RISECBOR64::Bytes;

struct CacheChannel {
	std::string name, units, representation;
};
struct CacheOutput {
	std::string name, type; // native grid codec: "grid.v1"
	std::vector<CacheChannel> channels;
};
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
private:
	friend class CacheWriter;
	CacheDescription description_;
	std::vector<CacheEntry> entries_;
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
	bool Append(std::int64_t index, const std::string& output, double actualTime,
		const Bytes& frame, std::string& error, const Bytes& receipt = {});
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
