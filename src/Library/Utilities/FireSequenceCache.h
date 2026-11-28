//////////////////////////////////////////////////////////////////////
//
//  FireSequenceCache.h - Sealed fire sequence to native cache bridge
//
//  Bridges legacy sealed OpenVDB fire sequences and RISE's native
//  Simulation::CacheSnapshot (grid.v1) store: process-wide import
//  resolver registration, out-of-process helper invocation, and
//  incremental/full VDB-to-cache import with cryptographic receipts.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
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
// Registers (or clears) the process-wide resolver used by FireSequenceManifest
// when importing legacy sealed sequences on demand (thread-safe).
void SetFireCacheResolver(FireCacheResolver resolver);

// Shared by the CLI and desktop bridge. Both supply their own installed helper path.
// Spawns the helper on desktop POSIX, verifies exit status, and extracts the
// trailing 64-hex verified cache generation digest; fails on unsupported platforms.
bool ResolveFireCacheWithHelper(const std::string& helperPath, const std::string& cacheDirectory,
	const std::string& source, const std::string& digest, std::optional<std::int64_t> ordinal,
	std::string& generation, std::string& error);

// Imports one frame (when requestedOrdinal is set) or the full sealed sequence
// from sourceManifest into cacheDirectory as grid.v1 entries with CBOR receipts,
// sealing the cache once all authored frames are present. Fails if the manifest
// digest changed or any existing sealed frame bytes differ from the VDB preimage.
bool ImportFireSequenceCache(const std::string& sourceManifest, const std::string& cacheDirectory,
	std::string& error, std::optional<std::int64_t> requestedOrdinal = std::nullopt,
	const std::string& expectedEnvelopeDigest = {}, std::string* verifiedGenerationDigest = nullptr);

} }
#endif
