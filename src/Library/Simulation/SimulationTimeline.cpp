// Simulation M3c: see SimulationTimeline.h.
#include "pch.h"
#include "SimulationTimeline.h"
#include "SimulatedMeshSource.h"

#include <algorithm>
#include <cmath>

namespace RISE { namespace Simulation {

double MeshFrameSceneTime( const MeshCacheSource& source, const std::int64_t index )
{
	// MapTime: u = (t - offset) * scale; s = start + cacheScale * (u - sceneOrigin).
	const CacheDescription& d = source.Description();
	const MeshSourceOptions& o = source.Options();
	const double s = d.startTime + double( index - d.firstIndex ) * d.interval;
	const double u = d.sceneOrigin + ( s - d.startTime ) / d.timeScale;
	return o.timeOffset + u / o.timeScale;
}

bool LatestContiguousMeshFrame( const MeshCacheSource& source, std::int64_t& index, std::string& error )
{
	std::shared_ptr<const IReadOnlyCacheSnapshot> snapshot;
	if( !source.CurrentSnapshot( snapshot, error ) ) return false;
	const std::string& output = source.Options().output;
	const std::int64_t firstIndex = snapshot->Description().firstIndex;
	const OutputSpan span = snapshot->Span( output );
	std::int64_t next = firstIndex;
	if( span.count > 0 && span.first == firstIndex &&
		static_cast<std::uint64_t>( span.last - span.first ) + 1 == span.count ) {
		next = span.last + 1;   // distinct indices filling [first, last]: no gap, no scan
	} else if( span.count > 0 && span.first <= firstIndex ) {
		CacheEntry entry;       // a gap (rare): walk the contiguous prefix
		while( snapshot->FindEntry( next, output, entry ) ) ++next;
	}
	index = next - 1;
	return true;
}

bool DescribeMeshSource( const MeshCacheSource& source, const bool availability,
	TimelineRange& range, std::string& error )
{
	const CacheDescription& d = source.Description();
	range.kind = TimelineRange::SimulatedMesh;
	range.fidelity = d.fidelity;
	range.framesTotal = d.count;
	const std::int64_t last = d.firstIndex + static_cast<std::int64_t>( d.count ) - 1;
	const double a = MeshFrameSceneTime( source, d.firstIndex );
	const double b = MeshFrameSceneTime( source, last );
	range.start = std::min( a, b );
	range.end = std::max( a, b );
	range.interval = std::fabs( d.interval / ( d.timeScale * source.Options().timeScale ) );
	range.state = source.Live() ? TimelineRange::Live : TimelineRange::Complete;
	range.availableEnd = range.end;
	range.framesAvailable = d.count;
	range.availabilityKnown = false;
	if( !availability ) return true;

	std::shared_ptr<const IReadOnlyCacheSnapshot> snapshot;
	if( !source.CurrentSnapshot( snapshot, error ) ) return false;
	range.framesAvailable = snapshot->Span( source.Options().output ).count;
	std::int64_t contiguous = d.firstIndex - 1;
	if( !LatestContiguousMeshFrame( source, contiguous, error ) ) return false;
	range.availableEnd = contiguous < d.firstIndex ? range.start : MeshFrameSceneTime( source, contiguous );
	if( !source.Live() && !snapshot->Complete() ) {
		std::string ignored;
		range.state = InspectCache( source.Root(), std::size_t( 64 ) * 1024 * 1024, ignored ) == CacheState::Baking ?
			TimelineRange::Baking : TimelineRange::Partial;
	}
	range.availabilityKnown = true;
	return true;
}

} }
