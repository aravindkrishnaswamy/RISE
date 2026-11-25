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
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/LiveSimulation.h"
#include "../src/Library/Geometry/SimulatedMeshGeometry.h"
#include "../src/Library/Rendering/InteractivePelRasterizer.h"
#include "../src/Library/Simulation/BuiltinSolvers.h"
#include "../src/Library/Simulation/SceneSimulation.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Utilities/Reference.h"
#include "TestTempDir.h"

#include <chrono>
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

	std::string SceneText( const std::string& gravity )
	{
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
		Viewport( const std::string& text, const fs::path& dir )
		{
			const fs::path path = dir / "live.RISEscene";
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
	fs::remove_all( RISE::TestSupport::ScratchTestRoot( "rise_simulation_scene_live_" ), ec );
	std::printf( "\n%d passed, %d failed\n", passes, failures );
	return failures ? 1 : 0;
}
