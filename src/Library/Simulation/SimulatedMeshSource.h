// Simulation M3b: the renderer's reader for a mesh.v1 / mesh.v2 output of
// a simulation cache (M7 prerequisites: the topology may change from frame to
// frame, and a frame may be empty).  It maps a render's time support to the cache frame,
// reads and verifies the payload, and derives the positions a deforming-mesh
// geometry installs at the nominal time and at the shutter keys.
// See docs/SIMULATION_M3B_RESULTS.md.
#ifndef RISE_SIMULATION_SIMULATED_MESH_SOURCE_H
#define RISE_SIMULATION_SIMULATED_MESH_SOURCE_H

#include "SimulationCache.h"
#include "SimulationMesh.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace RISE { namespace Simulation {

//! What happens when the mapped time leaves the cache's frames.  `Cache`
//! follows the cache's own time map (`hold` when it declares hold, else
//! `Error`).  `Hold` clamps to the first / last authored frame (no motion
//! there); `Loop` repeats the frames with period count x interval; `Error`
//! refuses a nominal time outside the cache support [start, start + count x
//! interval) and clamps shutter keys to it.
enum class MeshEndPolicy { Cache, Hold, Loop, Error };

//! What a PREVIEW binding does with a frame a partial cache does not have
//! yet: keep the previously installed frame, or hide the surface.  Both warn.
//! A predictive binding fails closed instead.
enum class MeshMissingPolicy { Hold, Hide };

struct MeshSourceOptions
{
	std::string output = "surface";
	double timeOffset = 0.0;	//!< scene seconds subtracted from the render time
	double timeScale = 1.0;		//!< consumer playback rate applied after the offset
	MeshEndPolicy end = MeshEndPolicy::Cache;
	MeshMissingPolicy missing = MeshMissingPolicy::Hold;
	bool predictive = false;	//!< request predictive fidelity: refuse anything less, fail closed
	std::size_t frameBudget = std::size_t( 512 ) * 1024 * 1024;	//!< per payload read and per decode
	std::string uvSet;			//!< mesh.v2 UV set used as texture coordinates; empty = the first by name
};

//! One render frame's worth of mesh data, ready to install.  Positions are
//! object-space doubles converted from the payload's binary32 samples, three per
//! vertex.  `open` / `close` are empty unless the shutter moves the surface.
struct MeshFrameSample
{
	enum class State { Ready, Missing };
	State state = State::Ready;
	std::int64_t index = 0;			//!< the cache frame the positions come from
	bool held = false;				//!< the time was clamped by the hold policy
	bool velocitiesFromCache = false;
	std::vector<double> nominal;	//!< at the render's nominal time
	std::vector<double> open, close;//!< at shutter open / close (motion only)
	std::vector<double> normals;	//!< the payload's normals at the frame, or empty
	std::string identity;			//!< what was read and how it was keyed (stable id)
	//! M7 prerequisites: the frame's topology digest.  When it is not the
	//! bind frame's (`ownTopology`), the frame's connectivity and its UV set
	//! (MeshSourceOptions::uvSet, or the first; empty when it has none) come
	//! with it; otherwise the bind frame's (MeshCacheSource::Indices, BindUVs)
	//! apply.  The vertex count is nominal.size() / 3 either way.
	std::string topology;
	bool ownTopology = false;
	std::vector<std::uint32_t> indices;
	std::vector<double> uvs;
};

//! The time map of a cache frame binding (MeshCacheSource::MapTime; shared by
//! the points source, M7 prerequisites): scene time to the nearest frame and
//! the simulation-time offset from it.  `endPolicy` must be resolved (not
//! Cache); `what` names the consumer in errors.
bool MapCacheFrameTime( const CacheDescription& d, double timeOffset, double timeScale,
	MeshEndPolicy endPolicy, const char* what, double sceneTime, double deltaScene,
	std::int64_t& index, double& simOffset, bool& held, std::string& error );
//! A partial cache's re-read is still the cache a binding opened: same key,
//! epoch, producer, time map and outputs.
bool SameCacheBinding( const CacheDescription& a, const CacheDescription& b );

class MeshCacheSource
{
public:
	//! Simulation M3c: the current generation of an in-memory publication (an
	//! InProcessHost's MemoryPublisher), called on the render thread at each
	//! frame.  Thread-safe; returns null once the publication is gone.
	using SnapshotProvider = std::function<std::shared_ptr<const IReadOnlyCacheSnapshot>()>;

	//! Opens the cache, checks the output is mesh.v1 or mesh.v2, applies the
	//! predictive admission (complete, predictive label, every frame decodes
	//! and is in metres) when requested, and decodes the bind frame (the first
	//! frame present; it may be empty, M7).  Fails closed.
	static bool Open( const std::filesystem::path& root, const MeshSourceOptions& options,
		std::unique_ptr<MeshCacheSource>& out, std::string& error );
	//! Simulation M3c: binds to an in-memory publication instead of a disk
	//! cache (the live viewport).  The same checks as Open, minus predictive
	//! admission (a live binding is always preview).  Each Sample reads the
	//! provider's current generation, which must keep the bound identity.
	static bool OpenLive( const SnapshotProvider& provider, const MeshSourceOptions& options,
		std::unique_ptr<MeshCacheSource>& out, std::string& error );

	std::uint32_t VertexCount() const { return vertices_; }
	const std::vector<std::uint32_t>& Indices() const { return indices_; }
	const std::string& TopologyDigest() const { return topology_; }
	const MeshSourceOptions& Options() const { return options_; }
	const CacheDescription& Description() const { return bound_->Description(); }
	bool Live() const { return static_cast<bool>( provider_ ); }
	const std::filesystem::path& Root() const { return root_; }		//!< empty when live
	//! The generation Sample would read now (re-read for a partial disk cache).
	bool CurrentSnapshot( std::shared_ptr<const IReadOnlyCacheSnapshot>& snapshot, std::string& error ) const
	{
		return Snapshot( snapshot, error );
	}
	//! That generation when getting it costs no I/O (a sealed or predictive
	//! disk binding, or a live one); null for a partial disk cache.
	std::shared_ptr<const IReadOnlyCacheSnapshot> CheapSnapshot() const;
	bool PayloadHasNormals() const { return hasNormals_; }
	//! Positions / normals of the bind frame (3 per vertex).
	const std::vector<double>& BindPositions() const { return bindPositions_; }
	const std::vector<double>& BindNormals() const { return bindNormals_; }
	std::int64_t BindIndex() const { return bindIndex_; }
	//! mesh.v2: the bind frame's UV set chosen by MeshSourceOptions::uvSet
	//! (2 per vertex, as stored; texture coordinates do not follow later frames).
	bool HasUVs() const { return !bindUVs_.empty(); }
	const std::vector<double>& BindUVs() const { return bindUVs_; }
	const std::string& UVSetName() const { return uvSetName_; }

	//! Maps a scene time to the cache: the nearest frame, the simulation-time
	//! offset from it, and whether a hold clamp applied.  `deltaScene` is added
	//! to the time after the frame is chosen (shutter keys keep the nominal's
	//! frame).  False outside support under the Error policy.
	bool MapTime( double sceneTime, double deltaScene, std::int64_t& index,
		double& simOffset, bool& held, std::string& error ) const;

	//! Reads, verifies and decodes the frame for a render at `nominal` with the
	//! shutter [open, close]; when `motion` is set and the surface moves, fills
	//! the shutter keys from the frame's velocities (or, without velocities,
	//! from the adjacent frames).  Never modifies the source.  A frame a partial
	//! cache does not have yet comes back as State::Missing; every other problem
	//! (corrupt or unsupported payload, budget, units differ, cache replaced) is
	//! an error.  A frame whose topology differs from the bind frame's is not
	//! (M7): it carries its own connectivity, and without velocities its keys
	//! come only from neighbours of the same topology.
	bool Sample( double nominal, double open, double close, bool motion,
		MeshFrameSample& out, std::string& error ) const;

private:
	MeshCacheSource() = default;
	static bool OpenImpl( const std::filesystem::path& root, const SnapshotProvider* provider,
		const MeshSourceOptions& options, std::unique_ptr<MeshCacheSource>& out, std::string& error );
	bool Snapshot( std::shared_ptr<const IReadOnlyCacheSnapshot>& snapshot, std::string& error ) const;
	FrameStatus ReadFrame( const IReadOnlyCacheSnapshot& snapshot, std::int64_t index,
		MeshFrame& frame, std::string& digest, std::string& error, std::string* topology = nullptr ) const;
	bool CheckFrame( const MeshFrame& frame, std::string& error ) const;

	MeshSourceOptions options_;
	std::filesystem::path root_;
	SnapshotProvider provider_;		//!< live binding (M3c); empty for a disk cache
	std::shared_ptr<const IReadOnlyCacheSnapshot> bound_;	//!< the snapshot opened (and, if predictive, qualified) at bind
	MeshEndPolicy end_ = MeshEndPolicy::Error;
	std::uint32_t vertices_ = 0;
	std::vector<std::uint32_t> indices_;
	std::string topology_;
	bool hasNormals_ = false;
	std::vector<double> bindPositions_, bindNormals_, bindUVs_;
	std::string uvSetName_;
	//! mesh.v2 static payloads already read and verified (immutable, by digest).
	mutable std::mutex staticMutex_;
	mutable std::vector<std::pair<std::string, std::shared_ptr<const Bytes>>> statics_;
	std::int64_t bindIndex_ = 0;
};

} }
#endif
