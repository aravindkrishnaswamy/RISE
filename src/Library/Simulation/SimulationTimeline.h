// Simulation M3c: what the timeline knows about a scene's time-varying assets
// (simulated meshes, fire media): the scene-time range of each one's frames and
// how much of it is baked.  See docs/SIMULATION_M3C_RESULTS.md.
#ifndef RISE_SIMULATION_TIMELINE_H
#define RISE_SIMULATION_TIMELINE_H

#include <cstdint>
#include <string>

namespace RISE { namespace Simulation {

class MeshCacheSource;

//! One time-varying asset's frames on the scene timeline.  Times are scene
//! seconds; `start` / `end` are the first and last frame of the cache's time
//! support, `availableEnd` the last frame of the run of frames present from
//! the first one (end of the drawable baked range).
struct TimelineRange
{
	enum Kind { SimulatedMesh = 0, FireMedium = 1 };
	enum State { Complete = 0, Partial = 1, Baking = 2, Live = 3 };
	std::string name;
	Kind kind = SimulatedMesh;
	State state = Complete;
	std::string fidelity = "preview";	//!< the cache's label ("preview" | "predictive")
	double start = 0, end = 0, availableEnd = 0;
	double interval = 0;				//!< scene seconds between frames
	std::uint64_t framesAvailable = 0, framesTotal = 0;
	bool availabilityKnown = false;		//!< false when only the time map was read
};

//! Scene time of cache frame `index` of a mesh binding (the inverse of
//! MeshCacheSource::MapTime, ignoring the end policy).
double MeshFrameSceneTime( const MeshCacheSource& source, std::int64_t index );

//! The binding's range.  With `availability`, also reads the current
//! generation (a partial disk cache re-reads its manifest, and asks the
//! writer lock whether a bake is running); without it only the time map.
bool DescribeMeshSource( const MeshCacheSource& source, bool availability,
	TimelineRange& range, std::string& error );

//! The last frame index of the run of present frames starting at the
//! binding's first frame in the source's current generation, or
//! firstIndex - 1 when that frame is absent.  Cheap for a live binding.
bool LatestContiguousMeshFrame( const MeshCacheSource& source, std::int64_t& index, std::string& error );

} }
#endif
