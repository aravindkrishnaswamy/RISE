//////////////////////////////////////////////////////////////////////
//
//  LiveSimulation.cpp - Interactive in-process simulation lifecycle
//    (mesh & transform.v1 solvers), scene-time simulated geometry
//    installation, and viewport PlaybackClock with pass coalescing.
//
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LiveSimulation.h"
#include "../Geometry/SimulatedMeshGeometry.h"
#include "../Geometry/SimulatedPointsGeometry.h"
#include "../Interfaces/IJobPriv.h"
#include "../Interfaces/ILog.h"
#include "../Simulation/SimulationHost.h"
#include "../Simulation/SimulationTimeline.h"
#include "../Simulation/SimulationTransform.h"
#include "../Simulation/SimulationWorker.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	constexpr std::size_t kParameterBudget = std::size_t( 16 ) * 1024 * 1024;
	constexpr std::size_t kRecordBudget = std::size_t( 1024 ) * 1024 * 1024;

	bool Fail( std::string& error, const std::string& message )
	{
		error = message;
		return false;
	}
}

// ---------------------------------------------------------------- install

// Prepares every scene-bound SimulatedMeshGeometry and SimulatedPointsGeometry
// at exact time t (zero-width shutter) and invalidates the scene TLAS.
bool RISE::Implementation::InstallSimulatedMeshesAtTime( IJobPriv& job, const double t,
	unsigned int& meshes, std::string& error )
{
	meshes = 0;
	const IScene* scene = job.GetScene();
	IGeometryManager* geometries = job.GetGeometries();
	if( !scene || !scene->GetObjects() || !geometries ) return true;
	// The meshes some object renders (an unbound geometry needs no install),
	// as Job::ActiveTimeVaryingAssets finds them.
	struct Objects : public IEnumCallback<IObject>
	{
		std::set<const IGeometry*> bound;
		bool operator()( const IObject& object ) override
		{
			if( dynamic_cast<const SimulatedMeshGeometry*>( object.GetGeometry() ) ||
				dynamic_cast<const SimulatedPointsGeometry*>( object.GetGeometry() ) ) bound.insert( object.GetGeometry() );
			return true;
		}
	} objects;
	scene->GetObjects()->EnumerateObjects( objects );
	if( objects.bound.empty() ) return true;
	struct Names : public IEnumCallback<const char*>
	{
		std::vector<std::string> names;
		bool operator()( const char* const& name ) override { names.push_back( name ); return true; }
	} names;
	geometries->EnumerateItemNames( names );
	bool ok = true;
	const RenderTimeSupport support{ t, t, t };
	for( const std::string& name : names.names ) {
		IGeometry* geometry = geometries->GetItem( name.c_str() );
		if( !geometry || !objects.bound.count( geometry ) ) continue;
		// M7 prerequisites: simulated points install the same way.
		ITimeVaryingAssetProvider* mesh = dynamic_cast<SimulatedMeshGeometry*>( geometry );
		if( !mesh ) mesh = dynamic_cast<SimulatedPointsGeometry*>( geometry );
		if( !mesh ) continue;
		std::string meshError;
		if( !mesh->PrepareAssetForRender( support, meshError, nullptr ) ) {
			if( ok ) error = "`" + name + "`: " + meshError;
			ok = false;
			continue;
		}
		++meshes;
	}
	scene->GetObjects()->InvalidateSpatialStructure();
	return ok;
}

// ---------------------------------------------------------------- LiveSimulation

//! The provider's view of a run's host: cleared before the host is destroyed,
//! so a source that outlives its run reads "gone" instead of a dead host.
struct LiveSimulation::Feed
{
	std::mutex						mutex;
	Simulation::InProcessHost*		host = nullptr;
};

struct LiveSimulation::Run
{
	std::vector<unsigned char>						parameters;
	std::filesystem::path							record;
	Simulation::NoSceneInputs						inputs;		//!< outlives the solver (an API attach has none)
	std::shared_ptr<const Simulation::ISceneInputs>	sceneInputs;	//!< M4: a scene simulation's (outlives the solver)
	std::shared_ptr<Feed>							feed;
	std::unique_ptr<Simulation::InProcessHost>		host;
	std::unique_ptr<Simulation::MeshCacheSource>	monitor;	//!< timing queries (the clock thread)
	std::unique_ptr<Simulation::MeshCacheSource>	bind;		//!< until bound into the geometry
	std::string										lastError;
	std::string										output;		//!< a transform run: the transform.v1 output it publishes

	~Run()
	{
		if( host ) {
			host->Pause();
			std::string ignored;
			host->WaitIdle( ignored );
		}
		if( feed ) {
			std::lock_guard<std::mutex> lock( feed->mutex );
			feed->host = nullptr;
		}
		host.reset();	// joins the worker thread
	}
};

//! A transform run's stable address for its publication: restarts swap the feed.
struct LiveSimulation::Hub
{
	std::mutex					mutex;
	std::shared_ptr<Feed>		feed;
};

// Starts an interactive transform.v1 solver run, steps the initial frame
// synchronously, and wires up its Feed so TransformProvider can read snapshots.
bool LiveSimulation::StartTransformRun( const std::string& solverId, const std::vector<unsigned char>& parameters,
	const std::filesystem::path* record, Run& run, std::string& error )
{
	RISECBOR64::Value value;
	if( !Simulation::ParseSolverParameters( parameters, kParameterBudget, value, error ) ) return false;
	std::unique_ptr<Simulation::ISimulationSolver> solver = Simulation::CreateSolver( solverId, error );
	if( !solver ) return false;
	auto host = std::make_unique<Simulation::InProcessHost>( std::move( solver ) );
	if( !host->Configure( value, run.sceneInputs ? *run.sceneInputs : static_cast<const Simulation::ISceneInputs&>( run.inputs ),
		error ) ) return false;
	if( !host->Description().capabilities.interactive )
		return Fail( error, "solver `" + solverId + "` is not interactive; bake it with rise-sim instead" );
	for( const auto& o : host->Description().cache.outputs )
		if( o.type == Simulation::TransformCodec ) { run.output = o.name; break; }
	if( run.output.empty() ) return Fail( error, "solver `" + solverId + "` publishes no transform.v1 output" );
	if( record && !host->Record( *record, kRecordBudget, error ) ) return false;
	if( !host->Start( error ) ) return false;
	Simulation::AdvanceResult result;
	if( !host->Step( Simulation::AdvanceRequest{}, result, error ) ) return false;
	run.feed = std::make_shared<Feed>();
	run.feed->host = host.get();
	run.host = std::move( host );	// from here ~Run clears the feed before the host goes
	run.parameters = parameters;
	if( record ) run.record = *record;
	return true;
}

std::unique_ptr<LiveSimulation> LiveSimulation::CreateForTransformSimulation( const std::string& simulation,
	const std::string& consumerName, const std::string& solverId, const std::vector<unsigned char>& parameters,
	std::shared_ptr<const Simulation::ISceneInputs> inputs, const std::string& fingerprint, std::string& error )
{
	std::unique_ptr<LiveSimulation> live( new LiveSimulation() );
	live->m_name = consumerName;
	live->m_solverId = solverId;
	live->m_simulation = simulation;
	live->m_fingerprint = fingerprint;
	live->m_inputs = std::move( inputs );
	live->m_transform = true;
	std::unique_ptr<Run> run( new Run() );
	run->sceneInputs = live->m_inputs;
	if( !StartTransformRun( solverId, parameters, nullptr, *run, error ) ) return nullptr;
	live->m_hub = std::make_shared<Hub>();
	live->m_hub->feed = run->feed;
	live->m_run = std::move( run );
	return live;
}

std::function<std::shared_ptr<const Simulation::IReadOnlyCacheSnapshot>()> LiveSimulation::TransformProvider() const
{
	const std::shared_ptr<Hub> hub = m_hub;
	return [hub]() -> std::shared_ptr<const Simulation::IReadOnlyCacheSnapshot> {
		if( !hub ) return nullptr;
		std::shared_ptr<Feed> feed;
		{
			std::lock_guard<std::mutex> lock( hub->mutex );
			feed = hub->feed;
		}
		if( !feed ) return nullptr;
		std::lock_guard<std::mutex> lock( feed->mutex );
		if( !feed->host ) return nullptr;
		return feed->host->Snapshot();
	};
}

double LiveSimulation::FirstSceneTime() const
{
	std::lock_guard<std::mutex> lock( m_mutex );
	if( !m_run || !m_run->host ) return 0;
	return m_run->host->Description().cache.startTime;
}

// Starts an interactive mesh solver run, steps the first frame synchronously,
// opens live monitor/bind MeshCacheSources, and verifies topology matches any bound mesh.
bool LiveSimulation::StartRun( const std::string& solverId, const std::vector<unsigned char>& parameters,
	const Simulation::MeshSourceOptions& options, const Simulation::MeshCacheSource* topology,
	const std::filesystem::path* record, Run& run, std::string& error )
{
	RISECBOR64::Value value;
	if( !Simulation::ParseSolverParameters( parameters, kParameterBudget, value, error ) ) return false;
	std::unique_ptr<Simulation::ISimulationSolver> solver = Simulation::CreateSolver( solverId, error );
	if( !solver ) return false;
	auto host = std::make_unique<Simulation::InProcessHost>( std::move( solver ) );
	if( !host->Configure( value, run.sceneInputs ? *run.sceneInputs : static_cast<const Simulation::ISceneInputs&>( run.inputs ),
		error ) ) return false;
	if( !host->Description().capabilities.interactive )
		return Fail( error, "solver `" + solverId + "` is not interactive; bake it with rise-sim instead" );
	if( record && !host->Record( *record, kRecordBudget, error ) ) return false;
	if( !host->Start( error ) ) return false;
	// The first frame, on this thread: the live source binds to it.
	Simulation::AdvanceResult result;
	if( !host->Step( Simulation::AdvanceRequest{}, result, error ) ) return false;
	run.feed = std::make_shared<Feed>();
	run.feed->host = host.get();
	std::shared_ptr<Feed> feed = run.feed;
	const Simulation::MeshCacheSource::SnapshotProvider provider =
		[feed]() -> std::shared_ptr<const Simulation::IReadOnlyCacheSnapshot> {
			std::lock_guard<std::mutex> lock( feed->mutex );
			if( !feed->host ) return nullptr;
			return feed->host->Snapshot();
		};
	run.host = std::move( host );	// from here ~Run clears the feed before the host goes
	if( !Simulation::MeshCacheSource::OpenLive( provider, options, run.monitor, error ) ||
		!Simulation::MeshCacheSource::OpenLive( provider, options, run.bind, error ) ) return false;
	// An unbound mesh (M4: no bake yet) takes the run's topology.
	if( topology && ( run.bind->VertexCount() != topology->VertexCount() || run.bind->Indices() != topology->Indices() ) )
		return Fail( error, "solver `" + solverId + "` publishes a surface whose topology (" +
			std::to_string( run.bind->VertexCount() ) + " vertices, " + std::to_string( run.bind->Indices().size() / 3 ) +
			" triangles) differs from the bound mesh (" + std::to_string( topology->VertexCount() ) + " vertices, " +
			std::to_string( topology->Indices().size() / 3 ) + " triangles); changing topology needs a reload (M7)" );
	run.parameters = parameters;
	if( record ) run.record = *record;
	return true;
}

std::unique_ptr<LiveSimulation> LiveSimulation::Create( const std::string& geometryName,
	SimulatedMeshGeometry& geometry, const std::string& solverId,
	const std::vector<unsigned char>& parameters, std::string& error )
{
	std::unique_ptr<LiveSimulation> live( new LiveSimulation() );
	live->m_name = geometryName;
	live->m_solverId = solverId;
	// The live binding keeps the scene's time map; a frame not published yet
	// holds the installed one, and times past the run hold its ends.
	live->m_options = geometry.Source().Options();
	live->m_options.end = Simulation::MeshEndPolicy::Hold;
	live->m_options.missing = Simulation::MeshMissingPolicy::Hold;
	live->m_options.predictive = false;
	std::unique_ptr<Run> run( new Run() );
	if( !StartRun( solverId, parameters, live->m_options, &geometry.Source(), nullptr, *run, error ) ) return nullptr;
	geometry.addref();
	live->m_geometry = &geometry;
	live->m_run = std::move( run );
	return live;
}

std::unique_ptr<LiveSimulation> LiveSimulation::CreateForSimulation( const std::string& simulation,
	const std::string& geometryName, SimulatedMeshGeometry& geometry, const std::string& solverId,
	const std::vector<unsigned char>& parameters, std::shared_ptr<const Simulation::ISceneInputs> inputs,
	const Simulation::MeshSourceOptions& options, const std::string& fingerprint, std::string& error )
{
	std::unique_ptr<LiveSimulation> live( new LiveSimulation() );
	live->m_name = geometryName;
	live->m_solverId = solverId;
	live->m_simulation = simulation;
	live->m_fingerprint = fingerprint;
	live->m_inputs = std::move( inputs );
	live->m_options = options;
	live->m_options.end = Simulation::MeshEndPolicy::Hold;
	live->m_options.missing = Simulation::MeshMissingPolicy::Hold;
	live->m_options.predictive = false;
	std::unique_ptr<Run> run( new Run() );
	run->sceneInputs = live->m_inputs;
	if( !StartRun( solverId, parameters, live->m_options, geometry.HasSource() ? &geometry.Source() : nullptr,
		nullptr, *run, error ) ) return nullptr;
	geometry.addref();
	live->m_geometry = &geometry;
	live->m_run = std::move( run );
	return live;
}

LiveSimulation::~LiveSimulation()
{
	std::unique_ptr<Run> run, pending;
	{
		std::lock_guard<std::mutex> lock( m_mutex );
		run = std::move( m_run );
		pending = std::move( m_pending );
	}
	pending.reset();
	run.reset();
	m_original.reset();
	if( m_geometry ) m_geometry->release();
}

std::unique_ptr<Simulation::MeshCacheSource> LiveSimulation::TakeBindSource()
{
	std::lock_guard<std::mutex> lock( m_mutex );
	Run* run = m_pending ? m_pending.get() : m_run.get();
	return run ? std::move( run->bind ) : nullptr;
}

void LiveSimulation::KeepOriginalSource( std::unique_ptr<Simulation::MeshCacheSource> source )
{
	m_original = std::move( source );
}

std::unique_ptr<Simulation::MeshCacheSource> LiveSimulation::TakeOriginalSource()
{
	return std::move( m_original );
}

bool LiveSimulation::Play( std::string& error )
{
	std::lock_guard<std::mutex> lock( m_mutex );
	if( !m_run || !m_run->host ) return Fail( error, "live simulation has no run" );
	if( m_run->host->Position().finished ) return true;
	return m_run->host->Play( error );
}

void LiveSimulation::Pause()
{
	std::lock_guard<std::mutex> lock( m_mutex );
	if( m_run && m_run->host ) m_run->host->Pause();
}

bool LiveSimulation::WaitIdle( std::string& error )
{
	Simulation::InProcessHost* host = nullptr;
	{
		std::lock_guard<std::mutex> lock( m_mutex );
		if( !m_run || !m_run->host ) return true;
		host = m_run->host.get();
	}
	// The run is replaced only by CommitRestart, which the controller
	// serializes with this call (its live-operation mutex).
	const bool ok = host->WaitIdle( error );
	std::lock_guard<std::mutex> lock( m_mutex );
	if( m_run ) m_run->lastError = ok ? std::string() : error;
	return ok;
}

// Two-phase restart: PrepareRestart boots the replacement Run off-lock into
// m_pending; CommitRestart swaps m_run (and m_hub->feed) and destroys the old Run.
bool LiveSimulation::PrepareRestart( const std::vector<unsigned char>* parameters,
	const std::filesystem::path* record, std::string& error )
{
	std::vector<unsigned char> current;
	{
		std::lock_guard<std::mutex> lock( m_mutex );
		if( !m_run ) return Fail( error, "live simulation has no run" );
		if( m_pending ) return Fail( error, "a live simulation restart is already pending" );
		current = m_run->parameters;
	}
	std::unique_ptr<Run> run( new Run() );
	run->sceneInputs = m_inputs;
	if( m_transform ) {
		if( !StartTransformRun( m_solverId, parameters ? *parameters : current, record, *run, error ) ) return false;
	} else if( !StartRun( m_solverId, parameters ? *parameters : current, m_options,
		m_geometry->HasSource() ? &m_geometry->Source() : nullptr, record, *run, error ) )
		return false;
	std::lock_guard<std::mutex> lock( m_mutex );
	m_pending = std::move( run );
	return true;
}

void LiveSimulation::CommitRestart()
{
	std::unique_ptr<Run> retired;
	{
		std::lock_guard<std::mutex> lock( m_mutex );
		if( !m_pending ) return;
		retired = std::move( m_run );
		m_run = std::move( m_pending );
		++m_restarts;
		if( m_hub ) {
			std::lock_guard<std::mutex> hubLock( m_hub->mutex );
			m_hub->feed = m_run->feed;
		}
	}
	retired.reset();
}

void LiveSimulation::AbortRestart()
{
	std::unique_ptr<Run> dropped;
	{
		std::lock_guard<std::mutex> lock( m_mutex );
		dropped = std::move( m_pending );
	}
	dropped.reset();
}

bool LiveSimulation::Latest( double& sceneTime, bool& finished ) const
{
	std::lock_guard<std::mutex> lock( m_mutex );
	if( m_transform ) {
		if( !m_run || !m_run->host ) return false;
		const std::shared_ptr<const Simulation::IReadOnlyCacheSnapshot> snapshot = m_run->host->Snapshot();
		if( !snapshot ) return false;
		const Simulation::OutputSpan span = snapshot->Span( m_run->output );
		Simulation::CacheEntry entry;
		if( span.count == 0 || !snapshot->FindEntry( span.last, m_run->output, entry ) ) return false;
		sceneTime = entry.time;
		const Simulation::CacheDescription& d = snapshot->Description();
		finished = span.last >= d.firstIndex + static_cast<std::int64_t>( d.count ) - 1 || !m_run->host->Busy();
		return true;
	}
	if( !m_run || !m_run->monitor ) return false;
	const Simulation::MeshCacheSource& monitor = *m_run->monitor;
	std::int64_t latest = 0;
	std::string error;
	if( !Simulation::LatestContiguousMeshFrame( monitor, latest, error ) ) return false;
	const Simulation::CacheDescription& d = monitor.Description();
	if( latest < d.firstIndex ) return false;
	sceneTime = Simulation::MeshFrameSceneTime( monitor, latest );
	// A run that stopped stepping (failed, or paused by its host) cannot
	// publish more: the clock must not wait for it.
	finished = latest >= d.firstIndex + static_cast<std::int64_t>( d.count ) - 1 || !m_run->host->Busy();
	return true;
}

LiveSimulationStatus LiveSimulation::GetStatus() const
{
	LiveSimulationStatus status;
	std::lock_guard<std::mutex> lock( m_mutex );
	status.solverId = m_solverId;
	status.restarts = m_restarts;
	if( m_transform && m_run && m_run->host ) {
		const Simulation::InProcessHost& host = *m_run->host;
		const Simulation::SolverPosition position = host.Position();
		const std::shared_ptr<const Simulation::IReadOnlyCacheSnapshot> snapshot = host.Snapshot();
		status.playing = host.Busy();
		status.finished = position.finished;
		status.simulatedTime = position.time;
		status.recording = !m_run->record.empty();
		status.error = m_run->lastError;
		status.failed = !status.error.empty();
		if( snapshot ) {
			const Simulation::CacheDescription& d = snapshot->Description();
			status.cacheKey = d.key;
			status.framesTotal = d.count;
			status.firstSceneTime = d.startTime;
			status.lastSceneTime = d.startTime + d.interval * static_cast<double>( d.count > 0 ? d.count - 1 : 0 );
			const Simulation::OutputSpan span = snapshot->Span( m_run->output );
			status.framesPublished = span.count;
			Simulation::CacheEntry entry;
			if( span.count > 0 && snapshot->FindEntry( span.last, m_run->output, entry ) ) {
				status.latestFrame = span.last;
				status.latestSceneTime = entry.time;
			}
		}
		return status;
	}
	if( !m_run || !m_run->host || !m_run->monitor ) return status;
	const Simulation::InProcessHost& host = *m_run->host;
	const Simulation::MeshCacheSource& monitor = *m_run->monitor;
	const Simulation::CacheDescription& d = monitor.Description();
	const Simulation::SolverPosition position = host.Position();
	status.playing = host.Busy();
	status.finished = position.finished;
	status.simulatedTime = position.time;
	status.recording = !m_run->record.empty();
	status.cacheKey = d.key;
	status.error = m_run->lastError;
	status.failed = !status.error.empty();
	status.framesTotal = d.count;
	status.firstSceneTime = Simulation::MeshFrameSceneTime( monitor, d.firstIndex );
	status.lastSceneTime = Simulation::MeshFrameSceneTime( monitor, d.firstIndex + static_cast<std::int64_t>( d.count ) - 1 );
	std::shared_ptr<const Simulation::IReadOnlyCacheSnapshot> snapshot = host.Snapshot();
	if( snapshot ) {
		status.framesPublished = snapshot->Span( m_options.output ).count;
	}
	std::int64_t latest = d.firstIndex - 1;
	std::string ignored;
	if( Simulation::LatestContiguousMeshFrame( monitor, latest, ignored ) && latest >= d.firstIndex ) {
		status.latestFrame = latest;
		status.latestSceneTime = Simulation::MeshFrameSceneTime( monitor, latest );
	}
	return status;
}

// ---------------------------------------------------------------- PlaybackClock

PlaybackClock::PlaybackClock( Hooks hooks ) :
  m_hooks( std::move( hooks ) )
{
}

PlaybackClock::~PlaybackClock()
{
	Shutdown();
}

bool PlaybackClock::OnClockThread() const
{
	return std::this_thread::get_id() == m_threadId.load( std::memory_order_acquire );
}

bool PlaybackClock::Playing() const
{
	std::lock_guard<std::mutex> lock( m_mutex );
	return m_playing;
}

PlaybackStatus PlaybackClock::GetStatus() const
{
	std::lock_guard<std::mutex> lock( m_mutex );
	PlaybackStatus status = m_status;
	if( m_playing ) {
		status.wallSeconds = std::chrono::duration<double>( std::chrono::steady_clock::now() - m_playWall ).count();
		if( m_hooks.completedPasses ) status.passesCompleted = m_hooks.completedPasses() - m_passesAtPlay;
	}
	return status;
}

bool PlaybackClock::Play( const double start, const double end, const double from, const double fps,
	const bool loop, std::string& error, const double timeScale )
{
	if( !std::isfinite( start ) || !std::isfinite( end ) || !std::isfinite( from ) || end < start )
		return Fail( error, "playback range is invalid" );
	if( !std::isfinite( fps ) || !( fps > 0.0 ) || fps > 1000.0 ) return Fail( error, "playback rate must be in (0, 1000] fps" );
	if( !std::isfinite( timeScale ) || !( timeScale > 0.0 ) || timeScale > 1000.0 || fps * timeScale > 1000.0 )
		return Fail( error, "playback time scale must be in (0, 1000] and show at most 1000 frames per second" );
	{
		std::lock_guard<std::mutex> lock( m_mutex );
		if( m_quit ) return Fail( error, "playback is shut down" );
		if( m_playing ) {
			m_status.start = start; m_status.end = end; m_status.fps = fps; m_status.loop = loop;
			m_status.timeScale = timeScale;
			return true;
		}
	}
	if( m_hooks.begin && !m_hooks.begin() )
		return Fail( error, "the viewport refused the scrub (a production or agent render owns the scene)" );
	{
		std::lock_guard<std::mutex> lock( m_compositeMutex );
		m_compositeOpen = true;
	}
	if( m_hooks.livePlay ) m_hooks.livePlay( true );
	{
		std::lock_guard<std::mutex> lock( m_mutex );
		m_status = PlaybackStatus();
		m_status.playing = true;
		m_status.start = start; m_status.end = end; m_status.fps = fps; m_status.loop = loop;
		m_status.timeScale = timeScale;
		m_status.time = from;
		m_clock = std::min( std::max( from, start ), end );
		m_lastFrame = -1;
		m_resync = true;
		m_playing = true;
		m_playWall = m_lastApply = std::chrono::steady_clock::now();
		m_passesAtPlay = m_passesAtApply = m_hooks.completedPasses ? m_hooks.completedPasses() : 0;
		if( !m_thread.joinable() ) m_thread = std::thread( &PlaybackClock::Loop, this );
	}
	m_cv.notify_all();
	return true;
}

void PlaybackClock::CloseComposite()
{
	{
		std::lock_guard<std::mutex> lock( m_compositeMutex );
		if( !m_compositeOpen ) return;
		m_compositeOpen = false;
	}
	if( m_hooks.end ) m_hooks.end();
}

void PlaybackClock::Pause()
{
	bool was = false;
	{
		std::lock_guard<std::mutex> lock( m_mutex );
		was = m_playing;
		m_playing = false;
		m_status.playing = false;
		m_status.waitingForSolver = false;
		if( was ) m_status.wallSeconds = std::chrono::duration<double>( std::chrono::steady_clock::now() - m_playWall ).count();
		if( was && m_hooks.completedPasses ) m_status.passesCompleted = m_hooks.completedPasses() - m_passesAtPlay;
	}
	m_cv.notify_all();
	// Let an apply in flight finish before the composite closes (never on
	// the clock thread itself, which holds the apply mutex then).
	if( !OnClockThread() ) { std::lock_guard<std::mutex> drain( m_applyMutex ); }
	CloseComposite();
	if( was && m_hooks.livePlay ) m_hooks.livePlay( false );
}

bool PlaybackClock::Seek( const double t )
{
	if( !std::isfinite( t ) ) return false;
	{
		std::lock_guard<std::mutex> lock( m_mutex );
		if( !m_playing ) return false;
		m_clock = std::min( std::max( t, m_status.start ), m_status.end );
		m_lastFrame = -1;
		m_resync = true;
	}
	m_cv.notify_all();
	return true;
}

void PlaybackClock::Shutdown()
{
	Pause();
	{
		std::lock_guard<std::mutex> lock( m_mutex );
		m_quit = true;
	}
	m_cv.notify_all();
	if( m_thread.joinable() && !OnClockThread() ) {
		m_thread.join();
		// A later Play (the controller restarted) starts a new thread.
		std::lock_guard<std::mutex> lock( m_mutex );
		m_quit = false;
		m_threadId.store( std::thread::id(), std::memory_order_release );
	}
}

// Dedicated playback thread: advances scaled wall clock, clamps to live solver
// progress, coalesces viewport applies to completed render passes (1 s watchdog),
// and handles loop wraparound or one-shot completion.
void PlaybackClock::Loop()
{
	m_threadId.store( std::this_thread::get_id(), std::memory_order_release );
	using clock = std::chrono::steady_clock;
	std::unique_lock<std::mutex> lk( m_mutex );
	for( ;; ) {
		m_cv.wait( lk, [&]{ return m_quit || m_playing; } );
		if( m_quit ) break;
		const clock::time_point now = clock::now();
		if( m_resync ) { m_lastTick = now; m_resync = false; }
		m_clock += std::chrono::duration<double>( now - m_lastTick ).count() * m_status.timeScale;
		m_lastTick = now;
		const double fps = m_status.fps;

		// A live simulation bounds the clock: never ahead of its latest frame.
		double limit = 0.0;
		bool finished = true, live = false;
		if( m_hooks.liveLimit ) {
			lk.unlock();
			live = m_hooks.liveLimit( limit, finished );
			lk.lock();
			if( m_quit || !m_playing ) continue;
		}
		m_status.live = live;
		m_status.waitingForSolver = false;
		double end = m_status.end;
		if( live ) {
			if( m_clock > limit && !finished ) {
				m_clock = limit;
				m_status.waitingForSolver = true;
				++m_status.solverWaits;
			}
			if( finished ) end = std::min( end, limit );
		}
		const long long last = static_cast<long long>( std::floor( ( end - m_status.start ) * fps + 1e-6 ) );
		long long frame = static_cast<long long>( std::floor( ( m_clock - m_status.start ) * fps + 1e-6 ) );
		bool stop = false;
		if( frame > last ) {
			if( m_status.loop && last >= 0 ) {
				const double period = double( last + 1 ) / fps;
				m_clock = m_status.start + std::fmod( m_clock - m_status.start, period );
				frame = static_cast<long long>( std::floor( ( m_clock - m_status.start ) * fps + 1e-6 ) );
				frame = std::min( std::max( frame, 0LL ), last );
			} else {
				frame = std::max( last, 0LL );
				m_clock = m_status.start + double( frame ) / fps;
				stop = true;
			}
		}
		if( frame < 0 ) frame = 0;

		if( frame != m_lastFrame ) {
			// Coalescing: the next frame waits until the viewport finished a
			// pass of the last one (or a second went by without one).
			const std::uint64_t passes = m_hooks.completedPasses ? m_hooks.completedPasses() : 0;
			const bool caughtUp = m_lastFrame < 0 || passes != m_passesAtApply ||
				now - m_lastApply > std::chrono::seconds( 1 );
			if( caughtUp ) {
				const double t = m_status.start + double( frame ) / fps;
				lk.unlock();
				bool ok = false;
				{
					std::lock_guard<std::mutex> apply( m_applyMutex );
					ok = m_hooks.apply && m_hooks.apply( t );
				}
				const std::uint64_t after = m_hooks.completedPasses ? m_hooks.completedPasses() : 0;
				lk.lock();
				if( ok ) {
					if( m_lastFrame >= 0 ) {
						const long long gap = frame > m_lastFrame ? frame - m_lastFrame - 1 :
							( last - m_lastFrame ) + frame;	// wrapped
						if( gap > 0 ) m_status.framesDropped += static_cast<std::uint64_t>( gap );
					}
					++m_status.framesShown;
					m_status.time = t;
					m_lastFrame = frame;
					m_passesAtApply = after;
					m_lastApply = clock::now();
				}
			}
		}
		if( stop && frame == m_lastFrame ) {
			// Not looping: the last frame is shown; playback ends here.
			m_playing = false;
			m_status.playing = false;
			m_status.wallSeconds = std::chrono::duration<double>( clock::now() - m_playWall ).count();
			if( m_hooks.completedPasses ) m_status.passesCompleted = m_hooks.completedPasses() - m_passesAtPlay;
			lk.unlock();
			CloseComposite();
			if( m_hooks.livePlay ) m_hooks.livePlay( false );
			lk.lock();
			continue;
		}
		// Tick a few times per frame so a finished pass is noticed promptly.
		const double tick = std::min( std::max( 0.25 / ( fps * m_status.timeScale ), 0.002 ), 0.02 );
		m_cv.wait_until( lk, now + std::chrono::duration_cast<clock::duration>( std::chrono::duration<double>( tick ) ) );
	}
}
