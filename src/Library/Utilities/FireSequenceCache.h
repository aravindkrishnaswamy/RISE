// Compatibility layer between sealed fire sequences and the simulation cache.
#ifndef RISE_FIRE_SEQUENCE_CACHE_H
#define RISE_FIRE_SEQUENCE_CACHE_H

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace RISE { namespace Implementation {

// The app resolves an original sealed sequence through its bundled import worker.
// A requested ordinal materializes one frame; no ordinal verifies the entire sequence.
using FireCacheResolver = std::function<bool(const std::string& sourceManifest,
	const std::string& expectedEnvelopeDigest, std::optional<std::int64_t> requestedOrdinal,
	std::string& cacheDirectory, std::string& verifiedGenerationDigest, std::string& error)>;
void SetFireCacheResolver(FireCacheResolver resolver);

// Shared by the CLI and desktop bridge. Both supply their own installed helper path.
bool ResolveFireCacheWithHelper(const std::string& helperPath, const std::string& cacheDirectory,
	const std::string& source, const std::string& digest, std::optional<std::int64_t> ordinal,
	std::string& generation, std::string& error);

bool ImportFireSequenceCache(const std::string& sourceManifest, const std::string& cacheDirectory,
	std::string& error, std::optional<std::int64_t> requestedOrdinal = std::nullopt,
	const std::string& expectedEnvelopeDigest = {}, std::string* verifiedGenerationDigest = nullptr);

} }
#endif
