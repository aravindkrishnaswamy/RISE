//////////////////////////////////////////////////////////////////////
//
//  SimulatedPointsSource.cpp - Read-only points.v1 simulation cache
//  source for render-time sampling (metres / m/s), shutter-interval
//  velocity extrapolation, and predictive cache preflight.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

// M7 prerequisites: see SimulatedPointsSource.h.
#include "pch.h"
#include "SimulatedPointsSource.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace RISE { namespace Simulation {
namespace {

constexpr std::size_t kManifestBudget = std::size_t( 64 ) * 1024 * 1024;

bool Fail( std::string& error, const std::string& message )
{
	error = message;
	return false;
}

double FromBits( const std::uint32_t bits )
{
	float value;
	std::memcpy( &value, &bits, sizeof( value ) );
	return double( value );
}

void ToDoubles( const std::vector<std::uint32_t>& bits, std::vector<double>& out )
{
	out.resize( bits.size() );
	for( std::size_t i = 0; i < bits.size(); ++i ) out[i] = FromBits( bits[i] );
}

std::string Hex( const double v )
{
	char buffer[64];
	std::snprintf( buffer, sizeof( buffer ), "%a", v );
	return buffer;
}

const CacheOutput* FindOutput( const CacheDescription& d, const std::string& name )
{
	for( const CacheOutput& o : d.outputs ) if( o.name == name ) return &o;
	return nullptr;
}

} // namespace

// Validates options, opens and binds the points.v1 cache snapshot, runs full-cache
// predictive preflight when requested, and verifies the initial bind frame.
bool PointsCacheSource::Open( const std::filesystem::path& root, const PointsSourceOptions& options,
	std::unique_ptr<PointsCacheSource>& out, std::string& error )
{
	if( !std::isfinite( options.timeOffset ) || !std::isfinite( options.timeScale ) || options.timeScale == 0.0 )
		return Fail( error, "simulated points time offset/scale must be finite and the scale nonzero" );
	if( !std::isfinite( options.radius ) || options.radius < 0.0 || !std::isfinite( options.radiusScale ) ||
		options.radiusScale < 0.0 )
		return Fail( error, "simulated points radius and radius scale must be finite and nonnegative" );
	std::unique_ptr<PointsCacheSource> source( new PointsCacheSource() );
	source->options_ = options;
	source->root_ = root;
	auto disk = std::make_shared<CacheSnapshot>();
	if( !CacheSnapshot::Open( root, kManifestBudget, *disk, error ) ) return false;
	source->bound_ = std::move( disk );
	const CacheDescription& d = source->bound_->Description();
	const CacheOutput* output = FindOutput( d, options.output );
	if( !output ) return Fail( error, "simulation cache has no output named `" + options.output + "`" );
	if( output->type != PointsCodec )
		return Fail( error, "simulation cache output `" + options.output + "` is " + output->type + ", not points.v1" );
	if( d.count == 0 || !( d.interval > 0.0 ) || !std::isfinite( d.interval ) )
		return Fail( error, "simulation cache has no frame support" );
	source->end_ = options.end == MeshEndPolicy::Cache ? ( d.hold ? MeshEndPolicy::Hold : MeshEndPolicy::Error ) : options.end;

	if( options.predictive ) {
		if( source->bound_->Span( options.output ).count != d.count )
			return Fail( error, "predictive simulated points require every frame of the output" );
		PointsCacheSource* self = source.get();
		IReadOnlyCacheSnapshot::Qualification q;
		q.snapshot = [&options]( const IReadOnlyCacheSnapshot& s, std::string& e ) {
			const CacheOutput* o = FindOutput( s.Description(), options.output );
			return ( o && o->type == PointsCodec ) || Fail( e, "predictive cache output is not points.v1" );
		};
		q.frame = [self, &options]( const CacheEntry& entry, const Bytes& bytes, std::string& e ) {
			if( entry.output != options.output ) return true;
			PointsFrame frame;
			return DecodePoints( bytes, options.frameBudget, frame, e ) && self->CheckFrame( frame, e );
		};
		if( !source->bound_->PreflightPredictive( d.key, q, options.frameBudget, error ) )
			return Fail( error, "predictive simulated points refused: " + error );
	}
	const OutputSpan span = source->bound_->Span( options.output );
	if( span.count == 0 ) return Fail( error, "simulation cache has no frame of output `" + options.output + "` yet" );
	PointsFrame frame;
	std::string digest;
	if( source->ReadFrame( *source->bound_, span.first, frame, digest, error ) != FrameStatus::Ready )
		return Fail( error, "simulated points bind frame refused: " + error );
	source->bindIndex_ = span.first;
	out = std::move( source );
	return true;
}

bool PointsCacheSource::CheckFrame( const PointsFrame& frame, std::string& error ) const
{
	if( frame.lengthUnits != "m" ) return Fail( error, "simulated points are not in metres (`" + frame.lengthUnits + "`)" );
	if( !frame.velocities.empty() && frame.velocityUnits != "m/s" )
		return Fail( error, "simulated points velocities are not in m/s (`" + frame.velocityUnits + "`)" );
	if( !options_.colorAttribute.empty() ) {
		const auto a = std::find_if( frame.attributes.begin(), frame.attributes.end(),
			[&]( const PointAttribute& x ) { return x.name == options_.colorAttribute; } );
		if( a == frame.attributes.end() )
			return Fail( error, "simulated points frame has no attribute `" + options_.colorAttribute + "`" );
		if( a->components != 1 && a->components != 3 )
			return Fail( error, "simulated points colour attribute `" + options_.colorAttribute + "` must have 1 or 3 components" );
	}
	return true;
}

FrameStatus PointsCacheSource::ReadFrame( const IReadOnlyCacheSnapshot& snapshot, const std::int64_t index,
	PointsFrame& frame, std::string& digest, std::string& error ) const
{
	std::shared_ptr<const Bytes> bytes;
	const FrameStatus status = snapshot.Read( index, options_.output, options_.frameBudget, bytes, error );
	if( status != FrameStatus::Ready ) return status;
	PointsFrame decoded;
	if( !DecodePoints( *bytes, options_.frameBudget, decoded, error ) ) return FrameStatus::Corrupt;
	if( !CheckFrame( decoded, error ) ) return FrameStatus::Unsupported;
	CacheEntry entry;
	digest = snapshot.FindEntry( index, options_.output, entry ) ? entry.digest : std::string();
	frame = std::move( decoded );
	return FrameStatus::Ready;
}

// Returns the pinned snapshot for complete/predictive caches, or reloads from
// disk for live partial caches while verifying the cache binding has not changed.
bool PointsCacheSource::CurrentSnapshot( std::shared_ptr<const IReadOnlyCacheSnapshot>& snapshot, std::string& error ) const
{
	if( bound_->Complete() || options_.predictive ) {
		snapshot = bound_;
		return true;
	}
	auto fresh = std::make_shared<CacheSnapshot>();
	if( !CacheSnapshot::Open( root_, kManifestBudget, *fresh, error ) ) return false;
	if( !SameCacheBinding( fresh->Description(), bound_->Description() ) )
		return Fail( error, "simulation cache was replaced since the scene loaded; reload the scene" );
	snapshot = std::move( fresh );
	return true;
}

std::shared_ptr<const IReadOnlyCacheSnapshot> PointsCacheSource::CheapSnapshot() const
{
	return bound_->Complete() || options_.predictive ? bound_ : nullptr;
}

bool PointsCacheSource::MapTime( const double sceneTime, const double deltaScene, std::int64_t& index,
	double& simOffset, bool& held, std::string& error ) const
{
	return MapCacheFrameTime( bound_->Description(), options_.timeOffset, options_.timeScale, end_, "simulated points",
		sceneTime, deltaScene, index, simOffset, held, error );
}

// Samples points at nominal and optional shutter open/close times, resolving
// radii/colours, velocity extrapolation (cached or ID-differenced), and identity.
bool PointsCacheSource::Sample( const double nominal, const double open, const double close, const bool motion,
	PointsFrameSample& out, std::string& error ) const
{
	if( !std::isfinite( nominal ) || !std::isfinite( open ) || !std::isfinite( close ) || open > close )
		return Fail( error, "simulated points render time support is invalid" );
	std::shared_ptr<const IReadOnlyCacheSnapshot> snapshot;
	if( !CurrentSnapshot( snapshot, error ) ) return false;
	std::int64_t index = 0, keyIndex = 0;
	double dNominal = 0.0, dOpen = 0.0, dClose = 0.0;
	bool held = false, keyHeld = false;
	if( !MapTime( nominal, 0.0, index, dNominal, held, error ) ) return false;
	const bool shutter = motion && close > open;
	if( shutter && ( !MapTime( nominal, open - nominal, keyIndex, dOpen, keyHeld, error ) ||
		!MapTime( nominal, close - nominal, keyIndex, dClose, keyHeld, error ) ) ) return false;
	PointsFrameSample sample;
	sample.index = index;
	sample.held = held;
	PointsFrame frame;
	std::string digest;
	const FrameStatus status = ReadFrame( *snapshot, index, frame, digest, error );
	if( status == FrameStatus::Unavailable ) {
		if( snapshot->Complete() ) return Fail( error, "simulated points frame is missing from a complete cache" );
		error.clear();
		sample.state = PointsFrameSample::State::Missing;
		sample.identity = "missing|" + std::to_string( index );
		out = std::move( sample );
		return true;
	}
	if( status != FrameStatus::Ready ) return Fail( error, "simulated points frame refused: " + error );
	const std::size_t n = frame.Count();
	ToDoubles( frame.positions, sample.nominal );
	// Radii: per point, uniform, or the binding's default; then the scale.
	sample.radii.assign( n, options_.radius );
	if( !frame.radii.empty() ) ToDoubles( frame.radii, sample.radii );
	else if( frame.hasUniformRadius ) sample.radii.assign( n, FromBits( frame.uniformRadius ) );
	if( options_.radiusScale != 1.0 ) for( double& r : sample.radii ) r *= options_.radiusScale;
	if( !options_.colorAttribute.empty() ) {
		for( const PointAttribute& a : frame.attributes ) {
			if( a.name != options_.colorAttribute ) continue;
			sample.colors.resize( 3 * n );
			for( std::size_t i = 0; i < n; ++i ) for( int c = 0; c < 3; ++c ) {
				float v;
				std::memcpy( &v, &a.values[a.components == 3 ? 3 * i + c : i], 4 );
				sample.colors[3 * i + c] = v;
			}
		}
	}
	std::string identity = digest + "|" + std::to_string( index );

	const bool extrapolate = motion && ( dNominal != 0.0 || dOpen != 0.0 || dClose != 0.0 );
	if( extrapolate ) {
		std::vector<double> velocity;
		if( !frame.velocities.empty() ) {
			ToDoubles( frame.velocities, velocity );
			sample.velocitiesFromCache = true;
		} else if( !frame.ids.empty() ) {
			// No velocities: central difference of the adjacent frames, points
			// matched by id (both lists are strictly increasing); one-sided
			// where a neighbour lacks the point, zero where both do.
			const CacheDescription& d = snapshot->Description();
			velocity.assign( 3 * n, 0.0 );
			std::vector<double> span( n, 0.0 ), a( sample.nominal ), b( sample.nominal );
			std::string prevDigest, nextDigest, ignored;
			const auto neighbour = [&]( const std::int64_t at, std::vector<double>& target, std::string& d0 ) {
				PointsFrame f;
				if( ReadFrame( *snapshot, at, f, d0, ignored ) != FrameStatus::Ready || f.ids.empty() ) { d0.clear(); return; }
				std::size_t j = 0;
				for( std::size_t i = 0; i < n; ++i ) {
					while( j < f.ids.size() && f.ids[j] < frame.ids[i] ) ++j;
					if( j == f.ids.size() || f.ids[j] != frame.ids[i] ) continue;
					for( int c = 0; c < 3; ++c ) target[3 * i + c] = FromBits( f.positions[3 * j + c] );
					span[i] += d.interval;
				}
			};
			if( index > d.firstIndex ) neighbour( index - 1, a, prevDigest );
			if( index + 1 < d.firstIndex + static_cast<std::int64_t>( d.count ) ) neighbour( index + 1, b, nextDigest );
			for( std::size_t i = 0; i < n; ++i )
				if( span[i] > 0.0 ) for( int c = 0; c < 3; ++c ) velocity[3 * i + c] = ( b[3 * i + c] - a[3 * i + c] ) / span[i];
			identity += "|adjacent|" + prevDigest + "|" + nextDigest;
		}
		if( !velocity.empty() ) {
			const auto at = [&]( const double offset, std::vector<double>& p ) {
				p.resize( sample.nominal.size() );
				for( std::size_t i = 0; i < p.size(); ++i ) p[i] = sample.nominal[i] + velocity[i] * offset;
			};
			if( shutter && dOpen != dClose ) {
				at( dOpen, sample.open );
				at( dClose, sample.close );
			}
			if( dNominal != 0.0 ) {
				std::vector<double> p;
				at( dNominal, p );
				sample.nominal.swap( p );
			}
		}
	}
	identity += "|" + Hex( extrapolate ? dNominal : 0.0 ) + "|" + Hex( sample.open.empty() ? 0.0 : dOpen ) + "|" +
		Hex( sample.open.empty() ? 0.0 : dClose );
	sample.identity = identity;
	out = std::move( sample );
	return true;
}

// Converts a cache frame index back to scene time through cache and source scales/offsets.
double PointsFrameSceneTime( const PointsCacheSource& source, const std::int64_t index )
{
	const CacheDescription& d = source.Description();
	const PointsSourceOptions& o = source.Options();
	const double s = d.startTime + double( index - d.firstIndex ) * d.interval;
	const double u = d.sceneOrigin + ( s - d.startTime ) / d.timeScale;
	return o.timeOffset + u / o.timeScale;
}

} }
