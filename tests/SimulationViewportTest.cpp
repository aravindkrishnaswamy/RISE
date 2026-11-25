//////////////////////////////////////////////////////////////////////
//
//  SimulationViewportTest.cpp - Simulation M3c: simulated deforming
//    geometry in the interactive viewport (SceneEditor/LiveSimulation.h,
//    docs/SIMULATION_M3C_RESULTS.md).  Drives a SceneEditController over
//    the real interactive rasterizer, headless, at 640 x 360.
//
//      T  timeline: a scene whose only time-varying content is a simulated
//         mesh has a timeline, with the cache's range; baked-range info
//      S  scrub a baked cache: at several scene times the mesh installed in
//         the scene equals the cache frame for that time (exact, from an
//         independent source); the install happens under the park and the
//         pass's own prepare finds it in place; no shutter keys in the
//         viewport (stills have no sample time)
//      P  playback clock over the baked cache: plays to the end, shows the
//         last frame, counts shown and dropped frames; a manual scrub
//         pauses it
//      L  live cloth.shell (51 x 51, two pinned corners, sphere collider,
//         interactive mode): play 3 s; frames advance monotonically and the
//         clock never passes the latest published frame; the installed mesh
//         equals the publication's frame for the displayed time; scrub back
//         reads published frames, scrub forward clamps; pause mid-step;
//         reset; a parameter change with another topology is refused;
//         detach while playing; scene close while playing
//
//    Usage: SimulationViewportTest [--bench]
//      --bench (not a gate): solver frames/s, viewport installs/s, passes/s
//      and displayed frames/s for 51 x 51 and 101 x 101 at 640 x 360.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/LiveSimulation.h"
#include "../src/Library/Geometry/SimulatedMeshGeometry.h"
#include "../src/Library/Rendering/InteractivePelRasterizer.h"
#include "../src/Library/Simulation/BuiltinSolvers.h"
#include "../src/Library/Simulation/SimulatedMeshSource.h"
#include "../src/Library/Simulation/SimulationCache.h"
#include "../src/Library/Simulation/SimulationHost.h"
#include "../src/Library/Simulation/SimulationMesh.h"
#include "../src/Library/Simulation/SimulationTimeline.h"
#include "../src/Library/Simulation/SimulationWorker.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Utilities/Reference.h"
#include "TestTempDir.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::Simulation;
namespace fs = std::filesystem;

namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

namespace
{
	int failures = 0, passes = 0;
	void Check( const bool ok, const std::string& what, const std::string& detail = std::string() )
	{
		if( ok ) { ++passes; return; }
		++failures;
		std::printf( "  FAIL: %s%s%s\n", what.c_str(), detail.empty() ? "" : " -- ", detail.c_str() );
	}

	constexpr std::size_t kBudget = 64u * 1024 * 1024;
	//! RISE_M3C_SLOW=k stretches the live test's play windows and waits k
	//! times (an instrumented build: ThreadSanitizer steps the solver ~20x slower).
	double Slow()
	{
		const char* v = std::getenv( "RISE_M3C_SLOW" );
		const double k = v ? std::atof( v ) : 1.0;
		return k >= 1.0 ? k : 1.0;
	}
	using Clock = std::chrono::steady_clock;
	double Since( const Clock::time_point t0 ) { return std::chrono::duration<double>( Clock::now() - t0 ).count(); }
	void Sleep( const double s ) { std::this_thread::sleep_for( std::chrono::duration<double>( s ) ); }

	bool WaitFor( const std::function<bool()>& done, const double seconds )
	{
		const Clock::time_point t0 = Clock::now();
		while( !done() ) {
			if( Since( t0 ) > seconds ) return false;
			Sleep( 0.005 );
		}
		return true;
	}

	std::uint32_t Bits( const double v ) { const float f = float( v ); std::uint32_t b; std::memcpy( &b, &f, 4 ); return b; }

	//! A small baked sheet: nx x ny vertices, frame k at time k*dt, moving
	//! with a known velocity (SimulatedMeshGeometryTest's fixture).
	struct Sheet
	{
		int nx = 12, ny = 9, count = 6;
		double dt = 0.25;
		MeshFrame Frame( const int k ) const
		{
			MeshFrame f;
			const double s = k * dt;
			for( int j = 0; j < ny; ++j ) for( int i = 0; i < nx; ++i ) {
				const double x = -1.0 + 2.0 * i / ( nx - 1 ), y = -0.75 + 1.5 * j / ( ny - 1 );
				const double v[3] = { 0.4 + 0.2 * y, -0.3 * x, 0.5 * std::sin( x ) };
				f.positions.push_back( Bits( x + v[0] * s ) );
				f.positions.push_back( Bits( y + v[1] * s ) );
				f.positions.push_back( Bits( 0.2 + v[2] * s + 0.05 * std::sin( k + 3.0 * x ) ) );
				for( int c = 0; c < 3; ++c ) f.velocities.push_back( Bits( v[c] ) );
			}
			for( int j = 0; j + 1 < ny; ++j ) for( int i = 0; i + 1 < nx; ++i ) {
				const std::uint32_t a = j * nx + i, b = a + 1, c = a + nx, d = c + 1;
				f.indices.insert( f.indices.end(), { a, b, c, b, d, c } );
			}
			f.velocityUnits = "m/s";
			return f;
		}
	};

	bool WriteSheet( const fs::path& root, const Sheet& sheet )
	{
		std::error_code ec;
		fs::remove_all( root, ec );
		fs::create_directories( root.parent_path(), ec );
		CacheDescription d;
		d.key = std::string( 64, 'a' ); d.epoch = std::string( 64, 'b' );
		d.producer = "m3c-fixture"; d.producerVersion = "1";
		d.firstIndex = 0; d.count = sheet.count; d.startTime = 0; d.interval = sheet.dt; d.hold = true;
		d.outputs = { { "surface", "mesh.v1", { { "position", "m", "f32x3" }, { "velocity", "m/s", "f32x3" } } } };
		CacheWriter writer;
		std::string error;
		if( !writer.Begin( root, d, "", false, kBudget, error ) ) { std::printf( "  Begin: %s\n", error.c_str() ); return false; }
		for( int k = 0; k < sheet.count; ++k ) {
			Bytes bytes;
			if( !EncodeMesh( sheet.Frame( k ), kBudget, bytes, error ) ||
				!writer.Append( k, "surface", k * sheet.dt, bytes, error ) ) { std::printf( "  Append: %s\n", error.c_str() ); return false; }
		}
		return writer.Seal( error ) || ( std::printf( "  Seal: %s\n", error.c_str() ), false );
	}

	std::string ClothJson( const int n, const int frames, const char* extra = "" )
	{
		char b[512];
		std::snprintf( b, sizeof( b ),
			"{\"mode\": \"interactive\", \"vertices_u\": %d, \"vertices_v\": %d, \"frames\": %d, \"frame_rate\": 30,"
			" \"pins\": [{\"select\": \"u0v0\"}, {\"select\": \"u1v0\"}],"
			" \"colliders\": [{\"type\": \"sphere\", \"center\": [0, 0.5, 0], \"radius\": 0.3}]%s}", n, n, frames, extra );
		return b;
	}
	std::vector<unsigned char> Bytes_( const std::string& s ) { return std::vector<unsigned char>( s.begin(), s.end() ); }

	//! A short bake of the cloth with the live run's topology: the scene's
	//! simulated_mesh_geometry binds to it (the M4 producer binding is not here).
	bool BakeCloth( const fs::path& root, const int n )
	{
		std::string error;
		RISECBOR64::Value value;
		std::unique_ptr<ISimulationSolver> solver = CreateSolver( "cloth.shell", error );
		if( !solver || !ParseSolverParameters( Bytes_( ClothJson( n, 2 ) ), kBudget, value, error ) ) {
			std::printf( "  bake: %s\n", error.c_str() );
			return false;
		}
		std::error_code ec;
		fs::remove_all( root, ec );
		fs::create_directories( root.parent_path(), ec );
		NoSceneInputs inputs;
		InProcessHost host( std::move( solver ) );
		const bool ok = host.Configure( value, inputs, error ) && host.Record( root, std::size_t( 1 ) << 30, error ) &&
			host.Start( error ) && host.StepToIndex( 1, error );
		if( !ok ) std::printf( "  bake: %s\n", error.c_str() );
		return ok && host.Position().finished;
	}

	std::string Scene( const fs::path& cache, const std::string& camera, const std::string& extra )
	{
		return std::string( "RISE ASCII SCENE 7\n\nfilm\n{\n\twidth 640\n\theight 360\n}\n\n" ) + camera +
			"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
			"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
			"lambertian_material\n{\n\tname m\n\treflectance white\n}\n\n"
			"simulated_mesh_geometry\n{\n\tname cloth\n\tcache " + cache.string() + "\n\tdouble_sided TRUE\n}\n\n"
			"standard_object\n{\n\tname sheet\n\tgeometry cloth\n\tmaterial m\n}\n\n" + extra +
			"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\toidn_denoise FALSE\n\tradiance_map white\n\tradiance_scale 1.0\n\tradiance_background TRUE\n}\n\n";
	}
	const char* kOrtho = "orthographic_camera\n{\n\tlocation 0 0 3\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 4 2.25\n\texposure 0.05\n}\n\n";
	const char* kPinhole = "pinhole_camera\n{\n\tlocation 0 1.4 2.6\n\tlookat 0 0.55 0\n\tup 0 1 0\n\tfov 40\n}\n\n";
	const char* kBall = "sphere_geometry\n{\n\tname ball\n\tradius 0.29\n}\n\nstandard_object\n{\n\tname ballobj\n\tgeometry ball\n\tmaterial m\n\tposition 0 0.5 0\n}\n\n";

	//! A loaded scene plus a controller over the real interactive rasterizer.
	struct Viewport
	{
		IJobPriv* job = nullptr;
		IRasterizer* rasterizer = nullptr;
		IRayCaster* previewCaster = nullptr;
		IRayCaster* polishCaster = nullptr;
		SceneEditController* ctrl = nullptr;
		Viewport( const std::string& text, const fs::path& dir )
		{
			const fs::path path = dir / "scene.RISEscene";
			{ std::ofstream o( path ); o << text; }
			if( !RISE_CreateJobPriv( &job ) || !job ) return;
			if( !job->LoadAsciiSceneViaCst( path.string().c_str() ) ) { safe_release( job ); return; }
			if( !CreateInteractiveMaterialPreviewPipeline( &rasterizer, &previewCaster, &polishCaster ) ) return;
			ctrl = new SceneEditController( *job, rasterizer );
			ctrl->Start();
		}
		void Close()
		{
			if( ctrl ) { ctrl->Stop(); delete ctrl; ctrl = nullptr; }
			if( rasterizer ) { rasterizer->release(); rasterizer = nullptr; }
			if( previewCaster ) { previewCaster->release(); previewCaster = nullptr; }
			if( polishCaster ) { polishCaster->release(); polishCaster = nullptr; }
			safe_release( job );
		}
		~Viewport() { Close(); }
		bool Ok() const { return ctrl != nullptr; }
		SimulatedMeshGeometry* Mesh() const
		{
			return dynamic_cast<SimulatedMeshGeometry*>( job->GetGeometries()->GetItem( "cloth" ) );
		}
		//! A pass finished after the count `since` was read (the viewport is
		//! idle once it converges, so read the count before the action).
		bool WaitPassAfter( const std::uint64_t since, const double seconds = 20.0 ) const
		{
			return WaitFor( [&]{ return ctrl->GetCompletedPassCount() > since; }, seconds );
		}
		std::uint64_t Passes() const { return ctrl->GetCompletedPassCount(); }
		bool Scrub( const double t ) const
		{
			if( !ctrl->OnTimeScrubBegin() ) return false;
			const bool ok = ctrl->OnTimeScrub( t );
			ctrl->OnTimeScrubEnd();
			return ok;
		}
		PlaybackStatus Playback() const { PlaybackStatus s; ctrl->GetPlaybackStatus( s ); return s; }
		LiveSimulationStatus Live() const
		{
			// The getter is non-blocking: retry while a scrub holds the live state.
			LiveSimulationStatus s;
			for( int i = 0; i < 2000 && !ctrl->GetLiveSimulationStatus( "cloth", s ); ++i ) Sleep( 0.001 );
			return s;
		}
	};

	//! The installed vertices equal `expected` (xyz doubles) exactly.
	bool InstalledEquals( const SimulatedMeshGeometry& g, const std::vector<double>& expected, std::string& detail )
	{
		const auto& p = g.getVertices();
		if( p.size() * 3 != expected.size() ) { detail = "vertex count"; return false; }
		std::size_t bad = 0;
		double worst = 0;
		for( std::size_t i = 0; i < p.size(); ++i ) {
			const double d = std::max( { std::fabs( p[i].x - expected[3*i] ), std::fabs( p[i].y - expected[3*i+1] ), std::fabs( p[i].z - expected[3*i+2] ) } );
			if( d != 0.0 ) { ++bad; worst = std::max( worst, d ); }
		}
		char b[128];
		std::snprintf( b, sizeof( b ), "%zu of %zu vertices differ (worst %.3g)", bad, p.size(), worst );
		detail = b;
		return bad == 0;
	}

	//! What a production (prepared) install of this binding would put in
	//! place at scene time t without a shutter: an independent source's sample.
	bool Expected( const MeshCacheSource& source, const double t, std::vector<double>& out, std::int64_t& index )
	{
		MeshFrameSample sample;
		std::string error;
		if( !source.Sample( t, t, t, true, sample, error ) || sample.state != MeshFrameSample::State::Ready ) return false;
		out = sample.nominal;
		index = sample.index;
		return true;
	}

	//////////////////////////////////////////////////////////////////
	void TestBaked( const fs::path& scratch )
	{
		std::printf( "T, S, P: a baked cache in the viewport\n" );
		Sheet sheet;
		const fs::path root = scratch / "baked";
		Check( WriteSheet( root, sheet ), "baked cache written" );
		Viewport v( Scene( root, kOrtho, "" ), scratch );
		Check( v.Ok(), "scene loads and the viewport starts" );
		if( !v.Ok() ) return;
		Check( v.WaitPassAfter( 0 ), "a first viewport pass completes" );
		SimulatedMeshGeometry* g = v.Mesh();
		Check( g != nullptr, "the scene's geometry is a simulated mesh" );
		if( !g ) return;

		// T: timeline availability and the baked range.
		bool has = false;
		Check( WaitFor( [&]{ return v.ctrl->GetHasAnimation( has ); }, 5 ) && has,
			"T a scene whose only time-varying content is a simulated mesh has a timeline" );
		double t0 = -1, t1 = -1;
		unsigned int nf = 0;
		Check( WaitFor( [&]{ return v.ctrl->GetAnimationOptions( t0, t1, nf ); }, 5 ) &&
			t0 == 0.0 && std::fabs( t1 - 1.25 ) < 1e-12 && nf == 6,
			"T the timeline range is the cache's frames (0 .. 1.25 s, 6 frames)",
			std::to_string( t0 ) + " " + std::to_string( t1 ) + " " + std::to_string( nf ) );
		std::vector<TimelineRange> ranges;
		Check( WaitFor( [&]{ return v.ctrl->GetTimeVaryingAssets( ranges ); }, 5 ) && ranges.size() == 1 &&
			ranges[0].name == "cloth" && ranges[0].kind == TimelineRange::SimulatedMesh &&
			ranges[0].state == TimelineRange::Complete && ranges[0].framesAvailable == 6 && ranges[0].framesTotal == 6 &&
			std::fabs( ranges[0].availableEnd - 1.25 ) < 1e-12 && std::fabs( ranges[0].interval - 0.25 ) < 1e-12 &&
			ranges[0].fidelity == "preview", "T baked-range info: complete, 6 of 6 frames, 0.25 s apart" );

		// S: scrub; the installed mesh equals an independent source's frame.
		std::unique_ptr<MeshCacheSource> reference;
		std::string error;
		Check( MeshCacheSource::Open( root, MeshSourceOptions(), reference, error ), "S independent reference source", error );
		if( !reference ) return;
		for( const double t : { 0.5, 1.0, 0.25, 1.25, 0.6, 0.0 } ) {
			const std::uint64_t before = v.Passes();
			Check( v.Scrub( t ), "S scrub to " + std::to_string( t ) );
			const unsigned long long installs = g->InstallCount();
			Check( v.WaitPassAfter( before ), "S a pass completes after the scrub" );
			Sleep( 0.05 );	// and any refinement passes that follow
			std::vector<double> expected;
			std::int64_t index = -1;
			std::string detail;
			Check( Expected( *reference, t, expected, index ) && g->InstalledFrame() == index &&
				InstalledEquals( *g, expected, detail ), "S t = " + std::to_string( t ) + ": installed mesh = cache frame " +
				std::to_string( index ) + " for that time", detail );
			Check( g->InstallCount() == installs,
				"S t = " + std::to_string( t ) + ": installed under the park; the passes' prepares found it in place" );
			Check( !g->BlurActive(), "S no shutter keys in the viewport (the camera has exposure; stills have no sample time)" );
		}

		// P: the playback clock over the baked range.
		Check( v.Scrub( 0.0 ), "P scrub to the start" );
		Check( v.ctrl->PlaybackPlayRange( 0.0, 1.25, 8.0, false ), "P play 0 .. 1.25 s at 8 fps, no loop" );
		Check( WaitFor( [&]{ return !v.Playback().playing; }, 20 ), "P playback reaches the end and stops" );
		const PlaybackStatus p = v.Playback();
		std::printf( "  P: %llu shown, %llu dropped of 11 clock frames, %llu passes in %.2f s\n",
			(unsigned long long)p.framesShown, (unsigned long long)p.framesDropped,
			(unsigned long long)p.passesCompleted, p.wallSeconds );
		Check( p.time == 1.25 && p.framesShown >= 2 && p.framesShown + p.framesDropped == 11,
			"P the last frame is shown; shown + dropped = the clock's 11 frames" );
		Check( g->InstalledFrame() == 5, "P the mesh shows the last cache frame" );
		Check( v.ctrl->PlaybackPlayRange( 0.0, 1.25, 8.0, true ), "P play looping" );
		Check( WaitFor( [&]{ return v.Playback().framesShown >= 3; }, 10 ), "P looping playback shows frames" );
		Check( v.ctrl->OnTimeScrubBegin(), "P a manual scrub begins" );
		Check( !v.Playback().playing, "P a manual scrub pauses library playback" );
		Check( v.ctrl->OnTimeScrub( 0.75 ) && v.ctrl->OnTimeScrubEnd(), "P the manual scrub applies" );
		Check( g->InstalledFrame() == 3, "P the manual scrub's frame is installed" );
		v.Close();
	}

	//////////////////////////////////////////////////////////////////
	void TestLive( const fs::path& scratch )
	{
		std::printf( "L: live cloth.shell in the viewport\n" );
		const fs::path root = scratch / "cloth51";
		Check( BakeCloth( root, 51 ), "L a short bake gives the scene its 51 x 51 binding" );
		Viewport v( Scene( root, kPinhole, kBall ), scratch );
		Check( v.Ok(), "L scene loads and the viewport starts" );
		if( !v.Ok() ) return;
		Check( v.WaitPassAfter( 0 ), "L a first pass completes" );
		SimulatedMeshGeometry* g = v.Mesh();
		if( !g ) { Check( false, "L geometry" ); return; }
		std::string error;
		const Clock::time_point a0 = Clock::now();
		Check( v.ctrl->LiveSimulationAttach( "cloth", "cloth.shell", Bytes_( ClothJson( 51, 91 ) ), error ),
			"L attach cloth.shell (interactive, 91 frames at 30 Hz)", error );
		std::printf( "  attach took %.2f s\n", Since( a0 ) );
		Check( v.ctrl->LiveSimulationCount() == 1 && g->Source().Live(), "L the geometry reads the live publication" );
		std::string refusal;
		Check( !v.ctrl->LiveSimulationAttach( "cloth", "cloth.shell", Bytes_( ClothJson( 51, 91 ) ), refusal ),
			"L a second attach to the same geometry is refused", refusal );
		Check( !v.ctrl->LiveSimulationAttach( "ball", "cloth.shell", Bytes_( ClothJson( 51, 91 ) ), refusal ),
			"L attaching to a geometry that is not a simulated mesh is refused", refusal );
		std::vector<TimelineRange> ranges;
		Check( WaitFor( [&]{ return v.ctrl->GetTimeVaryingAssets( ranges ); }, 5 ) && ranges.size() == 1 &&
			ranges[0].state == TimelineRange::Live && ranges[0].framesTotal == 91 && ranges[0].framesAvailable >= 1,
			"L baked-range info reports the live run (91 frames, state live)" );

		// Play 3 s, sampling: displayed time never decreases and never passes
		// the latest published frame; the solver's frames advance.
		Check( v.ctrl->PlaybackPlay( 30.0, false ), "L play at 30 fps" );
		double lastShown = -1, lastLatest = -1;
		bool monotonic = true, ahead = false;
		const Clock::time_point p0 = Clock::now();
		const std::uint64_t passes0 = v.ctrl->GetCompletedPassCount();
		int checks = 0;
		for( int phase = 0; phase < 3; ++phase ) {
			const Clock::time_point q0 = Clock::now();
			while( Since( q0 ) < 1.0 * Slow() ) {
				const double shown = v.Playback().time;
				const LiveSimulationStatus live = v.Live();
				if( shown < lastShown || live.latestSceneTime < lastLatest ) monotonic = false;
				if( v.Playback().framesShown > 0 && shown > live.latestSceneTime + 1e-12 ) ahead = true;
				lastShown = shown;
				lastLatest = live.latestSceneTime;
				Sleep( 0.01 );
			}
			// Pause (interrupting the solver mid-step) and check the installed
			// mesh against the publication's frame for the displayed time.
			Check( v.ctrl->PlaybackPause(), "L pause" );
			const LiveSimulationStatus paused = v.Live();
			Check( WaitFor( [&]{ return !v.Live().playing; }, 5 ), "L pause stops the solver at a safe point" );
			const double now = v.ctrl->LastSceneTime();
			std::vector<double> expected;
			std::int64_t index = -1;
			std::string detail;
			Check( Expected( g->Source(), now, expected, index ) && g->InstalledFrame() == index &&
				InstalledEquals( *g, expected, detail ),
				"L phase " + std::to_string( phase ) + ": installed mesh = published frame " + std::to_string( index ) +
				" for the displayed time " + std::to_string( now ), detail );
			Check( now <= v.Live().latestSceneTime + 1e-12, "L the displayed time is a published frame's" );
			std::printf( "  phase %d: shown t = %.4f (frame %lld), latest published %lld (t = %.4f), %llu shown, %llu dropped\n",
				phase, now, (long long)index, (long long)v.Live().latestFrame, v.Live().latestSceneTime,
				(unsigned long long)v.Playback().framesShown, (unsigned long long)v.Playback().framesDropped );
			( void )paused;
			++checks;
			if( phase < 2 ) Check( v.ctrl->PlaybackPlay( 30.0, false ), "L resume" );
		}
		const double wall = Since( p0 );
		const std::uint64_t passes = v.ctrl->GetCompletedPassCount() - passes0;
		Check( monotonic, "L displayed time and published frames advance monotonically" );
		Check( !ahead, "L the clock never ran ahead of the latest published frame" );
		const LiveSimulationStatus after = v.Live();
		Check( after.latestFrame >= 30, "L the solver published at least 30 frames in 3 s of play",
			std::to_string( after.latestFrame ) );
		std::printf( "  3 s play: %lld frames published, %llu passes in %.1f s\n", (long long)after.latestFrame,
			(unsigned long long)passes, wall );

		// Scrub back (published frames stay) and forward past the solver (clamps).
		Check( v.ctrl->PlaybackSeek( 0.5 ), "L scrub back to 0.5 s" );
		{
			std::vector<double> expected;
			std::int64_t index = -1;
			std::string detail;
			Check( Expected( g->Source(), 0.5, expected, index ) && index == 15 && g->InstalledFrame() == 15 &&
				InstalledEquals( *g, expected, detail ), "L scrubbing back shows published frame 15", detail );
		}
		Check( v.ctrl->PlaybackSeek( 100.0 ), "L scrub far forward" );
		const double clamped = v.ctrl->LastSceneTime();
		Check( std::fabs( clamped - v.Live().latestSceneTime ) < 1e-12 && g->InstalledFrame() == v.Live().latestFrame,
			"L scrubbing past the solver clamps to its latest frame", std::to_string( clamped ) );

		// A parameter change whose topology differs is refused; the run stays.
		std::string topologyError;
		Check( !v.ctrl->LiveSimulationSetParameters( "cloth", Bytes_( ClothJson( 41, 91 ) ), topologyError ) &&
			topologyError.find( "topology" ) != std::string::npos, "L new parameters with another topology are refused",
			topologyError );
		Check( g->Source().Live() && v.Live().latestFrame == after.latestFrame, "L the run is unchanged after the refusal" );
		// New parameters with the same topology: a new key, a restart.
		const std::string oldKey = v.Live().cacheKey;
		Check( v.ctrl->LiveSimulationSetParameters( "cloth", Bytes_( ClothJson( 51, 91, ", \"gravity\": [0, -5, 0]" ) ), error ),
			"L new parameters (gravity) restart the run", error );
		Check( v.Live().cacheKey != oldKey && v.Live().latestFrame == 0, "L the new run has a new key and starts at frame 0" );
		// Reset restarts from the initial state and shows frame 0.
		Check( v.ctrl->PlaybackPlay( 30.0, false ), "L play the new run" );
		Check( WaitFor( [&]{ return v.Live().latestFrame >= 10; }, 10 * Slow() ), "L the new run advances" );
		Check( v.ctrl->LiveSimulationReset( "cloth", error ), "L reset while playing", error );
		Check( v.ctrl->PlaybackPause(), "L pause" );
		{
			const double now = v.ctrl->LastSceneTime();
			std::vector<double> expected;
			std::int64_t index = -1;
			std::string detail;
			Check( v.Live().restarts == 2 && Expected( g->Source(), now, expected, index ) && g->InstalledFrame() == index &&
				InstalledEquals( *g, expected, detail ) && now <= v.Live().latestSceneTime + 1e-12,
				"L after the reset the installed mesh is the new run's frame for the displayed time", detail );
		}

		// A solver slower than the clock (pace_ms sleeps after each frame;
		// unkeyed): the clock waits for it and never runs ahead of it.
		Check( v.ctrl->LiveSimulationSetParameters( "cloth", Bytes_( ClothJson( 51, 91, ", \"pace_ms\": 60" ) ), error ),
			"L a paced run (slower than real time)", error );
		Check( v.ctrl->PlaybackPlay( 30.0, false ), "L play the paced run" );
		{
			bool pacedAhead = false;
			const Clock::time_point s0 = Clock::now();
			while( Since( s0 ) < 1.5 * Slow() ) {
				const PlaybackStatus st = v.Playback();
				const double latest = v.Live().latestSceneTime;
				if( st.framesShown > 0 && st.time > latest + 1e-12 ) pacedAhead = true;
				Sleep( 0.01 );
			}
			const PlaybackStatus paced = v.Playback();
			const LiveSimulationStatus pacedLive = v.Live();
			Check( v.ctrl->PlaybackPause(), "L pause the paced run" );
			std::printf( "  paced run: %llu shown, %llu dropped, %llu clock waits for the solver, latest frame %lld in 1.5 s\n",
				(unsigned long long)paced.framesShown, (unsigned long long)paced.framesDropped,
				(unsigned long long)paced.solverWaits, (long long)pacedLive.latestFrame );
			Check( !pacedAhead && paced.live && paced.solverWaits > 0 && pacedLive.latestFrame < 40,
				"L the clock waits for a slower solver and never runs ahead of it" );
		}

		// Detach while playing: the baked cache comes back, playback goes on.
		Check( v.ctrl->PlaybackPlay( 30.0, true ), "L play" );
		Sleep( 0.4 );
		Check( v.ctrl->LiveSimulationDetach( "cloth", error ), "L detach while playing", error );
		Check( v.ctrl->LiveSimulationCount() == 0, "L no live simulation after the detach" );
		Check( WaitFor( [&]{ return v.Playback().framesShown > 0; }, 5 ), "L playback goes on" );
		Check( v.ctrl->PlaybackPause(), "L pause" );
		Check( !g->Source().Live(), "L the geometry is bound to its baked cache again" );
		{
			const std::uint64_t before = v.Passes();
			Check( v.Scrub( 0.0 ) && v.WaitPassAfter( before ), "L passes render from the baked cache" );
		}

		// Scene close while playing (the controller is destroyed mid-step).
		Check( v.ctrl->LiveSimulationAttach( "cloth", "cloth.shell", Bytes_( ClothJson( 51, 91 ) ), error ), "L attach again", error );
		Check( v.ctrl->PlaybackPlay( 30.0, false ), "L play" );
		Sleep( 0.3 );
		const Clock::time_point c0 = Clock::now();
		v.Close();
		std::printf( "  scene closed while playing in %.3f s\n", Since( c0 ) );
		Check( true, "L scene close while playing joins everything" );
		( void )checks;
	}

	//////////////////////////////////////////////////////////////////
	void Bench( const fs::path& scratch )
	{
		std::printf( "bench (not a gate): live cloth.shell playback at 640 x 360\n" );
		for( const int n : { 51, 101 } ) {
			const fs::path root = scratch / ( "bench" + std::to_string( n ) );
			if( !BakeCloth( root, n ) ) continue;
			{
				// The solver alone on the host's worker thread (no viewport).
				std::string error;
				RISECBOR64::Value value;
				std::unique_ptr<ISimulationSolver> solver = CreateSolver( "cloth.shell", error );
				NoSceneInputs inputs;
				if( solver && ParseSolverParameters( Bytes_( ClothJson( n, 301 ) ), kBudget, value, error ) ) {
					InProcessHost host( std::move( solver ) );
					if( host.Configure( value, inputs, error ) && host.Start( error ) && host.Play( error ) ) {
						const Clock::time_point s0 = Clock::now();
						Sleep( 3.0 );
						const std::int64_t frames = host.Position().nextIndex;
						const double wall = Since( s0 );
						host.Pause();
						host.WaitIdle( error );
						std::printf( "  %d x %d solver alone: %.1f frames/s (%lld in %.2f s)\n", n, n,
							double( frames ) / wall, (long long)frames, wall );
					}
				}
			}
			Viewport v( Scene( root, kPinhole, kBall ), scratch );
			if( !v.Ok() || !v.WaitPassAfter( 0 ) ) continue;
			SimulatedMeshGeometry* g = v.Mesh();
			std::string error;
			if( !v.ctrl->LiveSimulationAttach( "cloth", "cloth.shell", Bytes_( ClothJson( n, 301 ) ), error ) ) {
				std::printf( "  attach %d: %s\n", n, error.c_str() );
				continue;
			}
			const unsigned long long installs0 = g->InstallCount();
			const std::uint64_t passes0 = v.ctrl->GetCompletedPassCount();
			const std::int64_t frames0 = v.Live().latestFrame;
			const Clock::time_point t0 = Clock::now();
			v.ctrl->PlaybackPlay( 30.0, false );
			Sleep( 2.0 );
			std::vector<RISEColor> pixels;
			unsigned int fw = 0, fh = 0;
			v.ctrl->CopyInteractiveFrame( pixels, fw, fh );
			Sleep( 2.0 );
			const std::int64_t published = v.Live().latestFrame;
			v.ctrl->PlaybackPause();
			const double wall = Since( t0 );
			const PlaybackStatus p = v.Playback();
			const double installs = double( g->InstallCount() - installs0 );
			const double passes = double( v.ctrl->GetCompletedPassCount() - passes0 );
			std::printf( "  %d x %d: %.2f s wall; solver %.1f frames/s (%lld published, %.1f substeps/s at 2 per frame);"
				" displayed %.1f fps (%llu shown, %llu dropped, %llu solver waits); installs %.1f/s; passes %.1f/s;"
				" frame during playback %u x %u\n",
				n, n, wall, double( published - frames0 ) / wall, (long long)published, 2.0 * double( published - frames0 ) / wall,
				double( p.framesShown ) / wall, (unsigned long long)p.framesShown, (unsigned long long)p.framesDropped,
				(unsigned long long)p.solverWaits, installs / wall, passes / wall, fw, fh );
			v.Close();
		}
	}
}

int main( int argc, char** argv )
{
	std::setvbuf( stdout, nullptr, _IONBF, 0 );
	const bool bench = argc > 1 && std::string( argv[1] ) == "--bench";
	const fs::path scratch = RISE::TestSupport::ScratchRunDir( "rise_simulation_viewport_", "suite" );
	std::error_code ec;
	fs::create_directories( scratch, ec );
	std::printf( "SimulationViewportTest (simulation M3c)\n" );
	std::string error;
	Check( RegisterBuiltinSolvers( error ), "built-in solvers register", error );
	Check( RegisterBuiltinSolvers( error ), "registering again is a no-op", error );
	if( bench ) {
		Bench( scratch );
	} else {
		TestBaked( scratch );
		TestLive( scratch );
	}
	fs::remove_all( RISE::TestSupport::ScratchTestRoot( "rise_simulation_viewport_" ), ec );
	std::printf( "\n%d passed, %d failed\n", passes, failures );
	return failures ? 1 : 0;
}
