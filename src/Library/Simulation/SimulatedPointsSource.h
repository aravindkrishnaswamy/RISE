// M7 prerequisites: the renderer's reader for a points.v1 output of a
// simulation cache (liquid spray and marker particles, imported particle
// data).  It maps a render's time support to the cache frame exactly as the
// simulated mesh does (MapCacheFrameTime), reads and verifies the payload,
// resolves each point's radius and derives the sphere centres at the nominal
// time and at the shutter keys.  See docs/SIMULATION_SOLVER_CONTRACT.md.
#ifndef RISE_SIMULATION_SIMULATED_POINTS_SOURCE_H
#define RISE_SIMULATION_SIMULATED_POINTS_SOURCE_H

#include "SimulatedMeshSource.h"
#include "SimulationPoints.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace RISE { namespace Simulation {

struct PointsSourceOptions
{
	std::string output = "particles";
	double timeOffset = 0.0;	//!< scene seconds subtracted from the render time
	double timeScale = 1.0;		//!< consumer playback rate applied after the offset
	MeshEndPolicy end = MeshEndPolicy::Cache;
	MeshMissingPolicy missing = MeshMissingPolicy::Hold;
	bool predictive = false;	//!< request predictive fidelity: refuse anything less, fail closed
	std::size_t frameBudget = std::size_t( 512 ) * 1024 * 1024;	//!< per payload read and per decode
	double radius = 0.01;		//!< metres, for a frame that carries no radius
	double radiusScale = 1.0;	//!< multiplies every radius (frame or default)
	std::string colorAttribute;	//!< a 1- or 3-component attribute exposed as the hit's vertex colour; empty = none
};

//! One render frame of points, ready to install: centres (3 per point,
//! object-space metres), radii (1 per point), and the shutter keys when the
//! shutter moves the points.
struct PointsFrameSample
{
	enum class State { Ready, Missing };
	State state = State::Ready;
	std::int64_t index = 0;
	bool held = false;
	bool velocitiesFromCache = false;
	std::vector<double> nominal;		//!< centres at the nominal time
	std::vector<double> open, close;	//!< centres at shutter open / close (motion only)
	std::vector<double> radii;
	std::vector<float> colors;			//!< 3 per point when PointsSourceOptions::colorAttribute is set
	std::string identity;
};

class PointsCacheSource
{
public:
	//! Opens the cache, checks the output is points.v1, applies the
	//! predictive admission when requested (complete, predictive label,
	//! every frame decodes in metres), and reads the first frame present.
	static bool Open( const std::filesystem::path& root, const PointsSourceOptions& options,
		std::unique_ptr<PointsCacheSource>& out, std::string& error );

	const PointsSourceOptions& Options() const { return options_; }
	const CacheDescription& Description() const { return bound_->Description(); }
	const std::filesystem::path& Root() const { return root_; }
	std::int64_t BindIndex() const { return bindIndex_; }
	//! The generation Sample would read now, and the same when it costs no I/O
	//! (null for a partial cache).
	bool CurrentSnapshot( std::shared_ptr<const IReadOnlyCacheSnapshot>& snapshot, std::string& error ) const;
	std::shared_ptr<const IReadOnlyCacheSnapshot> CheapSnapshot() const;

	bool MapTime( double sceneTime, double deltaScene, std::int64_t& index, double& simOffset, bool& held,
		std::string& error ) const;

	//! Reads, verifies and decodes the frame for a render at `nominal` with
	//! the shutter [open, close].  With `motion`, the keys come from the
	//! frame's velocities, or without them from the adjacent frames by id
	//! (central difference; points without ids get no keys).  A frame a
	//! partial cache does not have yet is State::Missing; anything else that
	//! goes wrong is an error.  Never modifies the source.
	bool Sample( double nominal, double open, double close, bool motion, PointsFrameSample& out,
		std::string& error ) const;

private:
	PointsCacheSource() = default;
	FrameStatus ReadFrame( const IReadOnlyCacheSnapshot& snapshot, std::int64_t index, PointsFrame& frame,
		std::string& digest, std::string& error ) const;
	bool CheckFrame( const PointsFrame& frame, std::string& error ) const;

	PointsSourceOptions options_;
	std::filesystem::path root_;
	std::shared_ptr<const IReadOnlyCacheSnapshot> bound_;
	MeshEndPolicy end_ = MeshEndPolicy::Error;
	std::int64_t bindIndex_ = 0;
};

//! The scene time of a points binding's frame `index` (as MeshFrameSceneTime).
double PointsFrameSceneTime( const PointsCacheSource& source, std::int64_t index );

} }
#endif
