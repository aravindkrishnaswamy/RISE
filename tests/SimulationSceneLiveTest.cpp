// Simulation M4: a scene simulation running live in the viewport from the scene
// description alone (docs/SIMULATION_M4_RESULTS.md section 6).
//
//   * a cloth_simulation with no cache: its consumer loads unbound and hidden;
//     LiveSimulationAttachSimulation runs it and the consumer takes its topology
//     from the first published frame (no pre-bake);
//   * the live run's key is the scene's expected key; its scene input (an
//     animated collider object) is sampled from its own thread while the
//     viewport scrubs and plays the scene;
//   * a parameter edit through the agent edit path restarts the run with the
//     new key against the re-derived scene; detach unbinds the consumer again.
//
// Simulation M5c (docs/SIMULATION_M5C_RESULTS.md):
//   * the controller's Simulation category: enumeration, descriptor-driven
//     rows (with editable occurrence rows of repeatable fields), an edit
//     through SetPropertyForCategory marks the simulation stale with its
//     reason and starts no bake, undo restores it, an edit resyncs a live run;
//   * the playback rule: PlaybackPlay(0) steps an ordinary animation at 30
//     authored frames per wall second, and plays simulation assets in real time.
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/LiveSimulation.h"
#include "../src/Library/Geometry/SimulatedMeshGeometry.h"
#include "../src/Library/Rendering/InteractivePelRasterizer.h"
#include "../src/Library/Simulation/BuiltinSolvers.h"
#include "../src/Library/Simulation/SceneSimulation.h"
#include "../src/Library/Simulation/SimulationWorker.h"
#include "../src/Library/Simulation/SimulationCache.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Utilities/Reference.h"
#include "TestTempDir.h"

#include <chrono>
#include <cmath>
#include <sstream>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

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
	using Clock = std::chrono::steady_clock;
	void Sleep( const double s ) { std::this_thread::sleep_for( std::chrono::duration<double>( s ) ); }
	bool WaitFor( const std::function<bool()>& done, const double seconds )
	{
		const Clock::time_point t0 = Clock::now();
		while( !done() ) {
			if( std::chrono::duration<double>( Clock::now() - t0 ).count() > seconds ) return false;
			Sleep( 0.005 );
		}
		return true;
	}

	std::string SceneText( const std::string& gravity, const bool cloth = true )
	{
		if( !cloth )   // M5c: an ordinary keyframed scene (10 frames over 3 s), no simulation
			return std::string( "RISE ASCII SCENE 7\n\nanimation_options\n{\n\ttime_start 0\n\ttime_end 3\n\tnum_frames 10\n}\n\n" )
				+ "film\n{\n\twidth 64\n\theight 36\n}\n\n"
				"pinhole_camera\n{\n\tlocation 0 1.4 2.6\n\tlookat 0 0.55 0\n\tup 0 1 0\n\tfov 40\n}\n\n"
				"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
				"sphere_geometry\n{\n\tname geo_ball\n\tradius 0.25\n}\n\n"
				"standard_object\n{\n\tname ball\n\tgeometry geo_ball\n\tposition 0 0.4 0\n}\n\n"
				"timeline\n{\n\telement_type object\n\telement ball\n\tparam position\n\n\ttime 0\n\tvalue 0 0.4 0\n\n"
				"\ttime 3\n\tvalue 0.3 0.4 0\n}\n\n"
				"pathtracing_pel_rasterizer\n{\n\tsamples 1\n\toidn_denoise FALSE\n}\n\n";
		return std::string( "RISE ASCII SCENE 7\n\nfilm\n{\n\twidth 320\n\theight 180\n}\n\n" )
			+ "pinhole_camera\n{\n\tlocation 0 1.4 2.6\n\tlookat 0 0.55 0\n\tup 0 1 0\n\tfov 40\n}\n\n"
			"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
			"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
			"lambertian_material\n{\n\tname m\n\treflectance white\n}\n\n"
			"sphere_geometry\n{\n\tname geo_ball\n\tradius 0.25\n}\n\n"
			"standard_object\n{\n\tname ball\n\tgeometry geo_ball\n\tmaterial m\n\tposition 0 0.4 0\n}\n\n"
			"timeline\n{\n\telement_type object\n\telement ball\n\tparam position\n\n\ttime 0\n\tvalue 0 0.4 0\n\n"
			"\ttime 3\n\tvalue 0.3 0.4 0\n}\n\n"
			"cloth_simulation\n{\n\tname flag\n\tmode interactive\n\tvertices_u 21\n\tvertices_v 21\n\tframes 91\n"
			"\torigin -0.5 0.8 0.5\n\tpin u0v0\n\tpin u1v0\n\tcontact barrier\n\tcollider_object ball\n" + gravity + "}\n\n"
			"simulated_mesh_geometry\n{\n\tname cloth\n\tsource flag.surface\n\tdouble_sided TRUE\n}\n\n"
			"standard_object\n{\n\tname sheet\n\tgeometry cloth\n\tmaterial m\n}\n\n"
			"pathtracing_pel_rasterizer\n{\n\tsamples 2\n\toidn_denoise FALSE\n\tradiance_map white\n\tradiance_scale 1.0\n"
			"\tradiance_background TRUE\n}\n\n";
	}

	struct Viewport
	{
		IJobPriv* job = nullptr;
		IRasterizer* rasterizer = nullptr;
		IRayCaster* previewCaster = nullptr;
		IRayCaster* polishCaster = nullptr;
		SceneEditController* ctrl = nullptr;
		fs::path path;
		Viewport( const std::string& text, const fs::path& dir )
		{
			path = dir / "live.RISEscene";
			{ std::ofstream o( path ); o << text; }
			if( !RISE_CreateJobPriv( &job ) || !job ) return;
			if( !job->LoadAsciiSceneViaCst( path.string().c_str() ) ) { safe_release( job ); return; }
			if( !CreateInteractiveMaterialPreviewPipeline( &rasterizer, &previewCaster, &polishCaster ) ) return;
			ctrl = new SceneEditController( *job, rasterizer );
			ctrl->Start();
		}
		~Viewport()
		{
			if( ctrl ) { ctrl->Stop(); delete ctrl; }
			if( rasterizer ) rasterizer->release();
			if( previewCaster ) previewCaster->release();
			if( polishCaster ) polishCaster->release();
			safe_release( job );
		}
		SimulatedMeshGeometry* Mesh() const
		{
			return dynamic_cast<SimulatedMeshGeometry*>( job->GetGeometries()->GetItem( "cloth" ) );
		}
		LiveSimulationStatus Live() const
		{
			LiveSimulationStatus s;
			for( int i = 0; i < 2000 && !ctrl->GetLiveSimulationStatus( "cloth", s ); ++i ) Sleep( 0.001 );
			return s;
		}
		std::vector<SceneSimulationStatus> Statuses() const
		{
			std::vector<SceneSimulationStatus> statuses;
			std::string error;
			for( int i = 0; i < 2000 && !ctrl->GetSimulations( statuses, error ); ++i ) Sleep( 0.002 );
			return statuses;
		}
		std::string ExpectedKey() const
		{
			std::vector<SceneSimulationStatus> statuses;
			std::string error;
			for( int i = 0; i < 2000 && !ctrl->GetSimulations( statuses, error ); ++i ) Sleep( 0.002 );
			return statuses.size() == 1 ? statuses[0].expectedKey : std::string();
		}
	};

	void TestLiveFromScene( const fs::path& scratch )
	{
		std::printf( "live from the scene, no pre-bake\n" );
		Viewport v( SceneText( "" ), scratch );
		Check( v.ctrl != nullptr, "a scene whose simulation has no cache loads and the viewport starts" );
		if( !v.ctrl ) return;
		SimulatedMeshGeometry* g = v.Mesh();
		Check( g && !g->HasSource() && g->Hidden(), "its consumer is unbound and hidden" );
		std::string error;
		Check( !v.ctrl->LiveSimulationAttachSimulation( "nonesuch", error ), "an unknown simulation is refused" );
		Check( v.ctrl->LiveSimulationAttachSimulation( "flag", error ), "LiveSimulationAttachSimulation( flag )", error );
		g = v.Mesh();
		Check( g && g->HasSource() && g->TopologyBuilt() && !g->Hidden() && g->Source().VertexCount() == 441,
			"the consumer took its topology (21 x 21) from the first published frame" );
		std::string refusal;
		Check( !v.ctrl->LiveSimulationAttachSimulation( "flag", refusal ), "a second attach is refused" );
		const std::string expected = v.ExpectedKey();
		LiveSimulationStatus s = v.Live();
		Check( !expected.empty() && s.cacheKey == expected, "the live run's key is the scene's expected key" );
		// Play while scrubbing: the solver samples the animated collider from its
		// own detached copy while the viewport poses the scene's.
		Check( v.ctrl->PlaybackPlay( 30.0, false ), "play" );
		for( int i = 0; i < 20; ++i ) {
			Sleep( 0.05 );
			if( v.ctrl->OnTimeScrubBegin() ) { v.ctrl->OnTimeScrub( 0.01 * i ); v.ctrl->OnTimeScrubEnd(); }
			v.ctrl->PlaybackPlay( 30.0, false );
		}
		Check( WaitFor( [&]{ return v.Live().framesPublished >= 20; }, 30.0 ), "the live run publishes frames",
			std::to_string( v.Live().framesPublished ) );
		v.ctrl->PlaybackPause();
		s = v.Live();
		Check( !s.failed && s.error.empty(), "the scene input samples without error", s.error );
		// Edit a parameter through the agent edit path: the run restarts with the new key.
		const SceneEditController::AgentCommitResult r =
			v.ctrl->ApplyAgentParamEdit( String( "flag" ), String( "" ), String( "gravity" ), String( "0 -5 0" ), nullptr );
		Check( r.applied, "the gravity edit applies", std::string( r.message.c_str() ) );
		const std::string edited = v.ExpectedKey();
		s = v.Live();
		Check( !edited.empty() && edited != expected && s.cacheKey == edited && v.ctrl->LiveSimulationCount() == 1,
			"the live run restarted with the edited key" );
		g = v.Mesh();
		Check( g && g->HasSource() && !g->Hidden(), "the re-derived scene's consumer is bound to the new run" );
		Check( v.ctrl->PlaybackPlay( 30.0, false ), "play the new run" );
		Check( WaitFor( [&]{ return v.Live().framesPublished >= 5; }, 30.0 ), "the new run publishes frames" );
		v.ctrl->PlaybackPause();
		// Detach: the consumer is unbound again (it has no cache).
		Check( v.ctrl->LiveSimulationDetach( "cloth", error ), "detach", error );
		g = v.Mesh();
		Check( g && !g->HasSource() && g->Hidden() && v.ctrl->LiveSimulationCount() == 0, "detached: unbound and hidden" );
		// Attach again, then close the scene while it plays.
		Check( v.ctrl->LiveSimulationAttachSimulation( "flag", error ), "attach again (same topology)", error );
		Check( v.ctrl->PlaybackPlay( 30.0, true ), "play" );
		Sleep( 0.2 );
	}

	// M5c: the Simulation category of the properties panel, end to end below the shells.
	void TestPropertiesCategory( const fs::path& scratch )
	{
		std::printf( "properties: the Simulation category\n" );
		using Cat = SceneEditController::Category;
		const fs::path dir = scratch / "props";
		std::error_code ec;
		fs::create_directories( dir, ec );
		fs::permissions( dir, fs::perms::owner_all, fs::perm_options::replace, ec );
		std::string trustError;
		Check( AddTrustedCacheRoot( dir, trustError ), "trust the private test directory", trustError );
		Viewport v( SceneText( "" ), dir );
		if( !v.ctrl ) { Check( false, "the scene loads" ); return; }
		Check( v.ctrl->CategoryEntityCount( Cat::Simulation ) == 1 &&
			v.ctrl->CategoryEntityName( Cat::Simulation, 0 ) == String( "flag" ), "the category lists the simulation" );
		Check( v.ctrl->SetSelection( Cat::Simulation, String( "flag" ) ), "select it" );
		v.ctrl->RefreshProperties();
		auto row = [&]( const char* name ) -> int {
			for( unsigned int i = 0; i < v.ctrl->PropertyCountFor( Cat::Simulation ); ++i )
				if( v.ctrl->PropertyNameFor( Cat::Simulation, i ) == String( name ) ) return static_cast<int>( i );
			return -1;
		};
		const unsigned int rows = v.ctrl->PropertyCountFor( Cat::Simulation );
		const int type = row( "chunk_type" ), vu = row( "vertices_u" ), substeps = row( "substeps" ), collider = row( "collider_object[0]" ),
			pin1 = row( "pin[1]" );
		Check( rows > 20 && type >= 0 && v.ctrl->PropertyValueFor( Cat::Simulation, type ) == String( "cloth_simulation" ),
			"descriptor-driven rows for the cloth_simulation chunk", std::to_string( rows ) );
		Check( vu >= 0 && v.ctrl->PropertyValueFor( Cat::Simulation, vu ) == String( "21" ) &&
			v.ctrl->PropertyEditableFor( Cat::Simulation, vu ), "an authored numeric field shows its value, editable" );
		Check( substeps >= 0 && v.ctrl->PropertyEditableFor( Cat::Simulation, substeps ), "an omitted field shows, editable" );
		bool ballOffered = false;
		if( collider >= 0 )
			for( unsigned int k = 0; k < v.ctrl->PropertyPresetCountFor( Cat::Simulation, collider ); ++k )
				if( v.ctrl->PropertyPresetValueFor( Cat::Simulation, collider, k ) == String( "ball" ) ) ballOffered = true;
		Check( collider >= 0 && v.ctrl->PropertyValueFor( Cat::Simulation, collider ) == String( "ball" ) &&
			v.ctrl->PropertyKindFor( Cat::Simulation, collider ) == static_cast<int>( ValueKind::Reference ) && ballOffered,
			"collider_object is an editable reference row offering the scene's objects" );
		Check( pin1 >= 0 && v.ctrl->PropertyValueFor( Cat::Simulation, pin1 ) == String( "u1v0" ), "a repeatable pin's occurrence row" );

		// Bake once (in process, as rise-sim does), so there is a cache to go stale.
		std::ostringstream out, err;
		const int code = WorkerMain( { "bake", v.path.string(), "flag" }, out, err );
		Check( code == WorkerFinished, "bake the scene's cache", out.str() + err.str() );
		const std::string baked = v.ExpectedKey();
		auto status = [&]() { const auto st = v.Statuses(); return st.size() == 1 ? st[0] : SceneSimulationStatus(); };
		Check( status().state == SceneSimulationState::Complete, "complete before the edit" );
		Check( status().memoryEstimate > 0 && status().costEstimate > 0 && status().wallEstimate > 0 &&
			!FormatSimulationEstimate( status() ).empty(), "the status carries cloth's estimates",
			FormatSimulationEstimate( status() ) );

		Check( v.ctrl->SetPropertyForCategory( Cat::Simulation, String( "substeps" ), String( "3" ) ), "edit substeps through the panel path" );
		SceneSimulationStatus st = status();
		Check( st.state == SceneSimulationState::Stale && st.staleReason == "parameters changed (substeps)" && v.ExpectedKey() != baked,
			"the edit makes the simulation stale: parameters changed (substeps)", st.staleReason );
		Check( st.framesAvailable == 6 || st.framesAvailable > 0, "the stale cache still plays" );
		Check( !fs::exists( st.cacheDir / "rise-sim.status.cbor" ) || status().state != SceneSimulationState::Baking,
			"the edit started no bake" );
		Check( v.ctrl->Undo(), "undo" );
		st = status();
		Check( st.state == SceneSimulationState::Complete && v.ExpectedKey() == baked && st.staleReason.empty(),
			"undo restores the configuration: complete again", st.staleReason );

		// An occurrence row: validated, routed to that line.
		Check( !v.ctrl->SetPropertyForCategory( Cat::Simulation, String( "pin[7]" ), String( "u1v1" ) ), "an absent occurrence is refused" );
		Check( v.ctrl->SetPropertyForCategory( Cat::Simulation, String( "pin[1]" ), String( "u1v1" ) ), "edit the second pin" );
		v.ctrl->RefreshProperties();
		const int pinAgain = row( "pin[1]" ), pin0 = row( "pin[0]" );
		Check( pinAgain >= 0 && v.ctrl->PropertyValueFor( Cat::Simulation, pinAgain ) == String( "u1v1" ) &&
			pin0 >= 0 && v.ctrl->PropertyValueFor( Cat::Simulation, pin0 ) == String( "u0v0" ),
			"only that occurrence changed" );
		Check( status().staleReason == "parameters changed (pins)", "stale: parameters changed (pins)", status().staleReason );
		Check( v.ctrl->Undo() && v.ExpectedKey() == baked, "undo" );

		// A collider edit (the scene input) is reported as such.
		const SceneEditController::AgentCommitResult r =
			v.ctrl->ApplyAgentParamEdit( String( "geo_ball" ), String( "" ), String( "radius" ), String( "0.3" ), nullptr );
		st = status();
		Check( r.applied && st.state == SceneSimulationState::Stale && st.staleReason == "parameters changed (colliders)",
			"resizing the collider: parameters changed (colliders)", st.staleReason + " " + std::string( r.message.c_str() ) );
		Check( v.ctrl->Undo() && status().state == SceneSimulationState::Complete, "undo" );

		// A live run follows a panel edit (the library resyncs; the shells need not).
		std::string error;
		Check( v.ctrl->LiveSimulationAttachSimulation( "flag", error ), "go live", error );
		Check( v.ctrl->SetPropertyForCategory( Cat::Simulation, String( "gravity" ), String( "0 -5 0" ) ), "edit gravity while live" );
		const std::string edited = v.ExpectedKey();
		Check( !edited.empty() && edited != baked && v.Live().cacheKey == edited, "the live run restarted with the edited key" );
		Check( v.ctrl->Undo() && v.Live().cacheKey == baked, "undo resyncs it back" );
		Check( v.ctrl->LiveSimulationDetach( "cloth", error ), "detach", error );
	}

	// M5c: the Play button's rule, and the clock's time scale.
	void TestPlaybackRule( const fs::path& scratch )
	{
		std::printf( "playback: frame stepping for ordinary animation, real time for simulations\n" );
		{
			// The clock alone: 10 frames per scene second at 2 scene s per wall s.
			std::vector<double> shown;
			std::uint64_t passes = 0;
			PlaybackClock::Hooks hooks;
			hooks.apply = [&]( double t ) { shown.push_back( t ); ++passes; return true; };
			hooks.completedPasses = [&]() { return ++passes; };   // the viewport always keeps up
			PlaybackClock clock( hooks );
			std::string error;
			const Clock::time_point t0 = Clock::now();
			Check( clock.Play( 0.0, 1.0, 0.0, 10.0, false, error, 2.0 ), "play at time scale 2", error );
			Check( WaitFor( [&]{ return !clock.Playing(); }, 3.0 ), "it ends" );
			const double wall = std::chrono::duration<double>( Clock::now() - t0 ).count();
			Check( wall > 0.4 && wall < 0.8 && !shown.empty() && shown.back() == 1.0 && shown.size() >= 8,
				"1 s of scene time in about 0.5 s of wall time, on the 0.1 s grid",
				std::to_string( wall ) + " s, " + std::to_string( shown.size() ) + " frames" );
			Check( !clock.Play( 0.0, 1.0, 0.0, 10.0, false, error, 0.0 ), "time scale 0 is refused" );
			clock.Shutdown();
		}
		const fs::path dir = scratch / "playback";
		std::error_code ec;
		fs::create_directories( dir, ec );
		{
			Viewport v( SceneText( "", false ), dir );
			if( !v.ctrl ) { Check( false, "the keyframed scene loads" ); return; }
			Check( v.ctrl->PlaybackPlay( 0.0, false ), "Play an ordinary animation" );
			PlaybackStatus s;
			v.ctrl->GetPlaybackStatus( s );
			Check( std::fabs( s.fps - 3.0 ) < 1e-9 && std::fabs( s.timeScale - 10.0 ) < 1e-9,
				"its 10 authored frames (3 per scene second) step at 30 per wall second",
				std::to_string( s.fps ) + " fps, scale " + std::to_string( s.timeScale ) );
			const Clock::time_point t0 = Clock::now();
			Check( WaitFor( [&]{ v.ctrl->GetPlaybackStatus( s ); return !s.playing; }, 5.0 ), "it ends" );
			const double wall = std::chrono::duration<double>( Clock::now() - t0 ).count();
			Check( wall < 2.0 && s.time == 3.0, "3 s of animation in about 0.3 s, as the 30 Hz shell timer played it",
				std::to_string( wall ) + " s" );
		}
		{
			Viewport v( SceneText( "" ), dir );
			if( !v.ctrl ) return;
			Check( v.ctrl->PlaybackPlay( 0.0, false ), "Play a scene with a simulation" );
			PlaybackStatus s;
			v.ctrl->GetPlaybackStatus( s );
			Check( s.timeScale == 1.0, "simulation assets play in real time",
				std::to_string( s.fps ) + " fps, scale " + std::to_string( s.timeScale ) );
			v.ctrl->PlaybackPause();
		}
	}
}

int main()
{
	std::setvbuf( stdout, nullptr, _IONBF, 0 );
	const fs::path scratch = RISE::TestSupport::ScratchRunDir( "rise_simulation_scene_live_", "suite" );
	std::error_code ec;
	fs::create_directories( scratch, ec );
	std::printf( "SimulationSceneLiveTest (simulation M4)\n" );
	std::string error;
	Check( RegisterBuiltinSolvers( error ), "built-in solvers register", error );
	TestLiveFromScene( scratch );
	TestPropertiesCategory( scratch );
	TestPlaybackRule( scratch );
	fs::remove_all( RISE::TestSupport::ScratchTestRoot( "rise_simulation_scene_live_" ), ec );
	std::printf( "\n%d passed, %d failed\n", passes, failures );
	return failures ? 1 : 0;
}
