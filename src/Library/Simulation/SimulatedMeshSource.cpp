// Simulation M3b: see SimulatedMeshSource.h and docs/SIMULATION_M3B_RESULTS.md.
#include "pch.h"
#include "SimulatedMeshSource.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>

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

bool MeshType( const std::string& type )
{
	return type == "mesh.v1" || type == "mesh.v2";
}

//! The parts of a description a reader binds to: a partial cache may grow,
//! but a different key, epoch, producer, time map or output set is a
//! different cache.
bool SameBinding( const CacheDescription& a, const CacheDescription& b )
{
	if( a.key != b.key || a.epoch != b.epoch || a.producer != b.producer ||
		a.producerVersion != b.producerVersion || a.firstIndex != b.firstIndex || a.count != b.count ||
		a.startTime != b.startTime || a.interval != b.interval || a.sceneOrigin != b.sceneOrigin ||
		a.timeScale != b.timeScale || a.hold != b.hold || a.fidelity != b.fidelity ||
		a.outputs.size() != b.outputs.size() ) return false;
	for( std::size_t i = 0; i < a.outputs.size(); ++i )
		if( a.outputs[i].name != b.outputs[i].name || a.outputs[i].type != b.outputs[i].type ) return false;
	return true;
}

} // namespace

bool MeshCacheSource::Open( const std::filesystem::path& root, const MeshSourceOptions& options,
	std::unique_ptr<MeshCacheSource>& out, std::string& error )
{
	return OpenImpl( root, nullptr, options, out, error );
}

bool MeshCacheSource::OpenLive( const SnapshotProvider& provider, const MeshSourceOptions& options,
	std::unique_ptr<MeshCacheSource>& out, std::string& error )
{
	if( !provider ) return Fail( error, "simulated mesh live binding needs a snapshot provider" );
	if( options.predictive ) return Fail( error, "a live simulated mesh binding is preview only" );
	return OpenImpl( std::filesystem::path(), &provider, options, out, error );
}

bool MeshCacheSource::OpenImpl( const std::filesystem::path& root, const SnapshotProvider* provider,
	const MeshSourceOptions& options, std::unique_ptr<MeshCacheSource>& out, std::string& error )
{
	if( !std::isfinite( options.timeOffset ) || !std::isfinite( options.timeScale ) || options.timeScale == 0.0 )
		return Fail( error, "simulated mesh time offset/scale must be finite and the scale nonzero" );
	std::unique_ptr<MeshCacheSource> source( new MeshCacheSource() );
	source->options_ = options;
	source->root_ = root;
	if( provider ) {
		source->provider_ = *provider;
		source->bound_ = source->provider_();
		if( !source->bound_ ) return Fail( error, "simulated mesh live publication has no snapshot" );
	} else {
		auto disk = std::make_shared<CacheSnapshot>();
		if( !CacheSnapshot::Open( root, kManifestBudget, *disk, error ) ) return false;
		source->bound_ = std::move( disk );
	}
	const CacheDescription& d = source->bound_->Description();
	const CacheOutput* output = FindOutput( d, options.output );
	if( !output ) return Fail( error, "simulation cache has no output named `" + options.output + "`" );
	if( !MeshType( output->type ) )
		return Fail( error, "simulation cache output `" + options.output + "` is " + output->type + ", not mesh.v1 or mesh.v2" );
	if( d.count == 0 || !( d.interval > 0.0 ) || !std::isfinite( d.interval ) )
		return Fail( error, "simulation cache has no frame support" );
	source->end_ = options.end == MeshEndPolicy::Cache ?
		( d.hold ? MeshEndPolicy::Hold : MeshEndPolicy::Error ) : options.end;

	if( options.predictive ) {
		// Predictive admission (fail closed): a complete cache labelled
		// predictive whose every frame of this output is present, verifies
		// against its manifest digest and decodes to one topology.
		if( source->bound_->Span( options.output ).count != d.count )
			return Fail( error, "predictive simulated mesh requires every frame of the output" );
		MeshCacheSource* self = source.get();
		IReadOnlyCacheSnapshot::Qualification q;
		q.snapshot = [&options]( const IReadOnlyCacheSnapshot& s, std::string& e ) {
			const CacheOutput* o = FindOutput( s.Description(), options.output );
			return ( o && MeshType( o->type ) ) || Fail( e, "predictive cache output is not a mesh" );
		};
		q.frame = [self, &options]( const CacheEntry& entry, const Bytes& bytes, std::string& e ) {
			if( entry.output != options.output ) return true;
			MeshFrame frame;
			if( !DecodeMesh( bytes, options.frameBudget, self->bound_->Resolver(), frame, e ) ) return false;
			MeshTopology topology;
			if( !PeekMeshTopology( bytes, topology, e ) ) return false;
			if( self->topology_.empty() ) {
				self->topology_ = topology.digest;
				self->vertices_ = topology.vertices;
			}
			if( topology.digest != self->topology_ || topology.vertices != self->vertices_ )
				return Fail( e, "predictive simulated mesh frames do not share one topology" );
			return self->CheckFrame( frame, e );
		};
		if( !source->bound_->PreflightPredictive( d.key, q, options.frameBudget, error ) )
			return Fail( error, "predictive simulated mesh refused: " + error );
		source->topology_.clear();
		source->vertices_ = 0;
	}

	// The bind frame: the first frame of the output present in the cache.
	const OutputSpan firstSpan = source->bound_->Span( options.output );
	if( firstSpan.count == 0 ) return Fail( error, "simulation cache has no frame of output `" + options.output + "` yet" );
	MeshFrame frame;
	std::string digest;
	const FrameStatus status = source->ReadFrame( *source->bound_, firstSpan.first, frame, digest, error );
	if( status != FrameStatus::Ready ) return Fail( error, "simulated mesh bind frame refused: " + error );
	if( frame.indices.empty() || frame.positions.empty() )
		return Fail( error, "simulated mesh needs a nonempty fixed-topology surface" );
	source->vertices_ = static_cast<std::uint32_t>( frame.positions.size() / 3 );
	source->indices_ = frame.indices;
	source->topology_ = MeshTopologyDigest( source->vertices_, frame.indices );
	if( !source->CheckFrame( frame, error ) ) return false;
	source->hasNormals_ = !frame.normals.empty();
	ToDoubles( frame.positions, source->bindPositions_ );
	ToDoubles( frame.normals, source->bindNormals_ );
	// mesh.v2 texture coordinates: the named UV set, or the first one.
	for( const MeshUVSet& uv : frame.uvSets ) {
		if( !options.uvSet.empty() && uv.name != options.uvSet ) continue;
		ToDoubles( uv.values, source->bindUVs_ );
		source->uvSetName_ = uv.name;
		break;
	}
	if( !options.uvSet.empty() && source->uvSetName_.empty() )
		return Fail( error, "simulated mesh has no UV set named `" + options.uvSet + "`" );
	source->bindIndex_ = firstSpan.first;
	out = std::move( source );
	return true;
}

bool MeshCacheSource::CheckFrame( const MeshFrame& frame, std::string& error ) const
{
	if( frame.positionUnits != "m" ) return Fail( error, "simulated mesh positions are not in metres (`" + frame.positionUnits + "`)" );
	if( !frame.velocities.empty() && frame.velocityUnits != "m/s" )
		return Fail( error, "simulated mesh velocities are not in m/s (`" + frame.velocityUnits + "`)" );
	if( vertices_ && frame.positions.size() != std::size_t( vertices_ ) * 3 )
		return Fail( error, "simulated mesh frame vertex count differs from the bound topology" );
	return true;
}

FrameStatus MeshCacheSource::ReadFrame( const IReadOnlyCacheSnapshot& snapshot, const std::int64_t index,
	MeshFrame& frame, std::string& digest, std::string& error ) const
{
	std::shared_ptr<const Bytes> bytes;
	// Read verifies the payload against the manifest's SHA-256 (per-frame
	// payload verification, as fire_medium's native reader does).
	const FrameStatus status = snapshot.Read( index, options_.output, options_.frameBudget, bytes, error );
	if( status != FrameStatus::Ready ) return status;
	MeshTopology topology;
	if( !PeekMeshTopology( *bytes, topology, error ) ) return FrameStatus::Corrupt;
	if( !topology_.empty() && ( topology.digest != topology_ || topology.vertices != vertices_ ) ) {
		error = "simulated mesh frame topology differs from the bound topology (changing topology is M7)";
		return FrameStatus::Unsupported;
	}
	MeshFrame decoded;
	// mesh.v2 static topology resolves through the same snapshot (verified,
	// budgeted) once, then from the memo: a static payload never changes.
	const StaticPayloadResolver resolve = [&]( const std::string& d, const std::size_t budget,
		std::shared_ptr<const Bytes>& b, std::string& e ) {
		{
			std::lock_guard<std::mutex> hold( staticMutex_ );
			for( const auto& s : statics_ ) if( s.first == d ) {
				if( s.second->size() > budget ) return Fail( e, "simulated mesh static payload exceeds the frame budget" );
				b = s.second;
				return true;
			}
		}
		if( snapshot.ReadStatic( d, budget, b, e ) != FrameStatus::Ready ) return false;
		std::lock_guard<std::mutex> hold( staticMutex_ );
		if( statics_.size() < 4 ) statics_.emplace_back( d, b );
		return true;
	};
	if( !DecodeMesh( *bytes, options_.frameBudget, resolve, decoded, error ) ) return FrameStatus::Corrupt;
	CacheEntry entry;
	digest = snapshot.FindEntry( index, options_.output, entry ) ? entry.digest : std::string();
	frame = std::move( decoded );
	return FrameStatus::Ready;
}

bool MeshCacheSource::Snapshot( std::shared_ptr<const IReadOnlyCacheSnapshot>& snapshot, std::string& error ) const
{
	// Simulation M3c: a live binding reads the publication's current
	// generation (immutable; frames only ever accumulate in it).
	if( provider_ ) {
		std::shared_ptr<const IReadOnlyCacheSnapshot> current = provider_();
		if( !current ) return Fail( error, "simulated mesh live publication is gone" );
		if( !SameBinding( current->Description(), bound_->Description() ) )
			return Fail( error, "simulated mesh live publication was reconfigured; rebind it" );
		snapshot = std::move( current );
		return true;
	}
	// A sealed (or predictive-qualified) generation is immutable: keep it.  A
	// partial cache may have grown since the last frame (follow mode), so it is
	// re-read, and must still be the cache this binding opened.
	if( bound_->Complete() || options_.predictive ) {
		snapshot = bound_;
		return true;
	}
	auto fresh = std::make_shared<CacheSnapshot>();
	if( !CacheSnapshot::Open( root_, kManifestBudget, *fresh, error ) ) return false;
	if( !SameBinding( fresh->Description(), bound_->Description() ) )
		return Fail( error, "simulation cache was replaced since the scene loaded; reload the scene" );
	snapshot = std::move( fresh );
	return true;
}

std::shared_ptr<const IReadOnlyCacheSnapshot> MeshCacheSource::CheapSnapshot() const
{
	if( provider_ ) {
		std::shared_ptr<const IReadOnlyCacheSnapshot> current = provider_();
		return current && SameBinding( current->Description(), bound_->Description() ) ? current : nullptr;
	}
	return bound_->Complete() || options_.predictive ? bound_ : nullptr;
}

bool MeshCacheSource::MapTime( const double sceneTime, const double deltaScene, std::int64_t& index,
	double& simOffset, bool& held, std::string& error ) const
{
	const CacheDescription& d = bound_->Description();
	if( !std::isfinite( sceneTime ) || !std::isfinite( deltaScene ) )
		return Fail( error, "simulated mesh cannot map an invalid scene time" );
	const double rate = d.timeScale * options_.timeScale;	// d(sim time)/d(scene time)
	const double u = ( sceneTime - options_.timeOffset ) * options_.timeScale;
	const double s = d.startTime + d.timeScale * ( u - d.sceneOrigin );
	const double dt = d.interval;
	const double n = double( d.count );
	const double s0 = d.startTime, sLast = s0 + ( n - 1.0 ) * dt, end = s0 + n * dt;
	const double sKeyRaw = s + rate * deltaScene;
	if( !std::isfinite( s ) || !std::isfinite( sKeyRaw ) )
		return Fail( error, "simulated mesh mapped time overflowed" );
	double sp = s, sKey = sKeyRaw;
	held = false;
	switch( end_ ) {
	case MeshEndPolicy::Error:
	case MeshEndPolicy::Cache:
		// The nominal time must lie in the support; a shutter reaching past
		// either end is clamped there (no motion beyond the data), so the
		// first frame of a cache can still be rendered with a centred shutter.
		if( s < s0 || s >= end )
			return Fail( error, "simulated mesh time is outside the cache support" );
		sKey = std::min( std::max( sKeyRaw, s0 ), end );
		break;
	case MeshEndPolicy::Hold:
		sp = std::min( std::max( s, s0 ), sLast );
		sKey = std::min( std::max( sKeyRaw, s0 ), sLast );
		held = sp != s || sKey != sKeyRaw;
		break;
	case MeshEndPolicy::Loop: {
		const double period = n * dt;
		double x = std::fmod( s - s0 + 0.5 * dt, period );
		if( x < 0.0 ) x += period;
		sp = s0 + x - 0.5 * dt;
		sKey = sp + rate * deltaScene;
		break; }
	}
	// The NEAREST frame: positions extrapolate from it by the frame velocity
	// both ways, so a shutter centred on a frame time reads one frame.
	const double position = std::floor( ( sp - s0 ) / dt + 0.5 );
	const double k = std::min( std::max( position, 0.0 ), n - 1.0 );
	index = d.firstIndex + static_cast<std::int64_t>( k );
	simOffset = sKey - ( s0 + k * dt );
	return true;
}

bool MeshCacheSource::Sample( const double nominal, const double open, const double close, const bool motion,
	MeshFrameSample& out, std::string& error ) const
{
	if( !std::isfinite( nominal ) || !std::isfinite( open ) || !std::isfinite( close ) || open > close )
		return Fail( error, "simulated mesh render time support is invalid" );
	std::shared_ptr<const IReadOnlyCacheSnapshot> snapshot;
	if( !Snapshot( snapshot, error ) ) return false;
	std::int64_t index = 0, keyIndex = 0;
	double dNominal = 0.0, dOpen = 0.0, dClose = 0.0;
	bool held = false, keyHeld = false;
	if( !MapTime( nominal, 0.0, index, dNominal, held, error ) ) return false;
	const bool shutter = motion && close > open;
	if( shutter && ( !MapTime( nominal, open - nominal, keyIndex, dOpen, keyHeld, error ) ||
		!MapTime( nominal, close - nominal, keyIndex, dClose, keyHeld, error ) ) ) return false;
	MeshFrameSample sample;
	sample.index = index;
	sample.held = held;
	MeshFrame frame;
	std::string digest;
	const FrameStatus status = ReadFrame( *snapshot, index, frame, digest, error );
	if( status == FrameStatus::Unavailable ) {
		if( snapshot->Complete() ) return Fail( error, "simulated mesh frame is missing from a complete cache" );
		error.clear();
		sample.state = MeshFrameSample::State::Missing;
		sample.identity = "missing|" + std::to_string( index );
		out = std::move( sample );
		return true;
	}
	if( status != FrameStatus::Ready ) return Fail( error, "simulated mesh frame refused: " + error );
	if( !CheckFrame( frame, error ) ) return false;
	ToDoubles( frame.positions, sample.nominal );
	ToDoubles( frame.normals, sample.normals );
	std::string identity = digest + "|" + std::to_string( index );

	const bool extrapolate = motion && ( dNominal != 0.0 || dOpen != 0.0 || dClose != 0.0 );
	if( extrapolate ) {
		std::vector<double> velocity;
		if( !frame.velocities.empty() ) {
			ToDoubles( frame.velocities, velocity );
			sample.velocitiesFromCache = true;
		} else {
			// No velocities in the payload: central difference of the adjacent
			// frames (one-sided at the ends, zero when neither neighbour exists).
			const CacheDescription& d = snapshot->Description();
			std::vector<double> prev, next;
			std::string prevDigest, nextDigest, ignored;
			MeshFrame neighbour;
			if( index > d.firstIndex &&
				ReadFrame( *snapshot, index - 1, neighbour, prevDigest, ignored ) == FrameStatus::Ready &&
				CheckFrame( neighbour, ignored ) ) ToDoubles( neighbour.positions, prev );
			if( index + 1 < d.firstIndex + static_cast<std::int64_t>( d.count ) &&
				ReadFrame( *snapshot, index + 1, neighbour, nextDigest, ignored ) == FrameStatus::Ready &&
				CheckFrame( neighbour, ignored ) ) ToDoubles( neighbour.positions, next );
			velocity.assign( sample.nominal.size(), 0.0 );
			const std::vector<double>& a = prev.empty() ? sample.nominal : prev;
			const std::vector<double>& b = next.empty() ? sample.nominal : next;
			const double span = d.interval * ( double( prev.empty() ? 0 : 1 ) + double( next.empty() ? 0 : 1 ) );
			if( span > 0.0 ) for( std::size_t i = 0; i < velocity.size(); ++i ) velocity[i] = ( b[i] - a[i] ) / span;
			identity += "|adjacent|" + prevDigest + "|" + nextDigest;
		}
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
	identity += "|" + Hex( extrapolate ? dNominal : 0.0 ) + "|" + Hex( sample.open.empty() ? 0.0 : dOpen ) + "|" +
		Hex( sample.open.empty() ? 0.0 : dClose );
	sample.identity = identity;
	out = std::move( sample );
	return true;
}

} }
