//////////////////////////////////////////////////////////////////////
//
//  PhotonMapDeferralTest.cpp - Phase-3 proof that the deferred photon-map
//  shoot is GATED on the active rasterizer actually consuming photon maps.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-06-13
//  Tabs: 4
//  Comments:
//
//    The scene defines a caustic photon map (a pending shoot) plus a
//    pixelpel_rasterizer whose advanced_shader consumes it via a
//    DefaultCausticPelPhotonMap shaderop.  Scene::GetPhotonShootCount()
//    counts photon-map shoot passes that did real work.  We assert the
//    exact behaviour the user asked for:
//
//      1. Render with BDPT (own transport, never reads the maps)  -> shoot
//         count stays 0; the pending shoot is PRESERVED (not consumed).
//      2. Switch the active rasterizer to the pixelpel (a "legacy" shader-
//         graph rasterizer that consumes the maps) and render -> the pending
//         shoot fires (count == 1).
//      3. Switch back to BDPT and render -> count is UNCHANGED (1); BDPT
//         renders correctly (the maps are already built and simply ignored).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Scene.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_failures = 0;

static void Check( bool cond, const char* what )
{
	if( cond ) {
		std::cout << "  [ok] " << what << "\n";
	} else {
		std::cout << "  [FAIL] " << what << "\n";
		++g_failures;
	}
}

class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	double maxLum;
	CapturingRasterizerOutput() : maxLum( 0.0 ) {}
protected:
	virtual ~CapturingRasterizerOutput() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage& pImage, const Rect*, const unsigned int ) override
	{
		maxLum = 0.0;
		for( unsigned int y = 0; y < pImage.GetHeight(); ++y ) {
			for( unsigned int x = 0; x < pImage.GetWidth(); ++x ) {
				const RISEColor c = pImage.GetPEL( x, y );
				const double lum = c.base.r + c.base.g + c.base.b;
				if( lum > maxLum ) maxLum = lum;
			}
		}
	}
};

static std::string WriteSceneToTempFile( const char* sceneText )
{
	char path[512];
	std::snprintf( path, sizeof(path), "/tmp/photon_deferral_%d.RISEscene", static_cast<int>(::getpid()) );
	std::ofstream ofs( path );
	if( !ofs.is_open() ) return std::string();
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

// A caustic photon map (pending shoot) + a pixelpel rasterizer whose
// advanced_shader consumes it.  The bdpt_pel_rasterizer is lazy-built by name.
static const char* kSceneText =
	"RISE ASCII SCENE 6\n"
	"\n"
	"film\n{\n\twidth 48\n\theight 48\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 -8\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"advanced_shader\n{\n\tname global\n"
	"\tshaderop DefaultDirectLighting        1 100 +\n"
	"\tshaderop DefaultCausticPelPhotonMap   1 100 +\n}\n\n"
	"pixelpel_rasterizer\n{\n\tmax_recursion 4\n\tsamples 1\n\tlum_samples 1\n}\n\n"
	"directional_light\n{\n\tname sun\n\tpower 3.14159\n\tcolor 1 1 1\n\tdirection 0 0 -1\n}\n\n"
	"uniformcolor_painter\n{\n\tname albedo\n\tcolor 0.7 0.7 0.7\n}\n\n"
	"lambertian_material\n{\n\tname matte\n\treflectance albedo\n}\n\n"
	"sphere_geometry\n{\n\tname ball\n\tradius 1.5\n}\n\n"
	"standard_object\n{\n\tname obj_ball\n\tgeometry ball\n\tposition 0 0 0\n\tmaterial matte\n\tshader global\n}\n\n"
	"caustic_pel_photonmap\n{\n\tnum 500\n\tmax_recursion 4\n\tmin_importance 0.001\n\tpower_scale 1.0\n\tbranch FALSE\n\treflect TRUE\n\trefract TRUE\n}\n\n"
	"caustic_pel_gather\n{\n\tmax_photons 50\n}\n";

static bool RenderWith( IJobPriv* pJob, const char* rasterizerName, CapturingRasterizerOutput* pCap )
{
	if( !pJob->SetActiveRasterizer( rasterizerName ) ) {
		return false;
	}
	pJob->RemoveRasterizerOutputs();
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );
	return pJob->Rasterize();
}

static void TestPhotonShootGatedOnConsumer()
{
	std::cout << "Test: deferred photon-map shoot is gated on the active rasterizer...\n";

	const std::string scenePath = WriteSceneToTempFile( kSceneText );
	if( scenePath.empty() ) { Check( false, "scene temp file written" ); return; }

	Scene::ResetPhotonShootCount();

	IJobPriv* pJob = 0;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) { Check( false, "Job created" ); return; }
	Check( pJob->LoadAsciiScene( scenePath.c_str() ), "scene loaded (defines caustic map + pixelpel)" );

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "photon-deferral test capture" );
	pCap->addref();

	// 1. BDPT: own transport, never reads the maps -> the pending shoot must NOT
	//    fire, and must remain pending for a later consuming rasterizer.
	const bool bdptOk = RenderWith( pJob, "bdpt_pel_rasterizer", pCap );
	Check( bdptOk, "BDPT render ran (bdpt_pel_rasterizer lazy-built + active)" );
	const unsigned int afterBDPT = Scene::GetPhotonShootCount();
	Check( afterBDPT == 0,
		( "BDPT did NOT shoot the photon map (count=" + std::to_string( afterBDPT ) + ", want 0)" ).c_str() );

	// 2. Switch to the pixelpel shader-graph rasterizer (consumes the maps) ->
	//    the pending shoot fires exactly once.
	const bool pelOk = RenderWith( pJob, "pixelpel_rasterizer", pCap );
	Check( pelOk, "pixelpel render ran (switched active rasterizer by name)" );
	const unsigned int afterPel = Scene::GetPhotonShootCount();
	Check( afterPel == 1,
		( "pixelpel triggered the deferred shoot (count=" + std::to_string( afterPel ) + ", want 1)" ).c_str() );

	// 3. Switch back to BDPT -> count unchanged; BDPT still renders correctly
	//    (maps already built, simply ignored by BDPT's transport).
	const bool bdptOk2 = RenderWith( pJob, "bdpt_pel_rasterizer", pCap );
	Check( bdptOk2, "BDPT render ran again after switching back" );
	const unsigned int afterBDPT2 = Scene::GetPhotonShootCount();
	Check( afterBDPT2 == 1,
		( "no extra shoot on switch back to BDPT (count=" + std::to_string( afterBDPT2 ) + ", want 1)" ).c_str() );
	Check( pCap->maxLum > 0.0,
		( "final BDPT image is non-black (maxLum=" + std::to_string( pCap->maxLum ) + ")" ).c_str() );

	safe_release( pCap );
	safe_release( pJob );
}

// Spectral twin of the scene above: a caustic_spectral photon map (pending
// shoot) + a pixelintegratingspectral_rasterizer whose advanced_shader consumes
// it.  Guards the Phase-3-review fix that classified the spectral shader-graph
// rasterizers as photon-map consumers (they were a silently-broken regression:
// scenes/Tests/Spectral/spectral_dispersive_caustic.RISEscene would render the
// dispersive caustic black because the deferred shoot was skipped).
static const char* kSpectralSceneText =
	"RISE ASCII SCENE 6\n"
	"\n"
	"film\n{\n\twidth 48\n\theight 48\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 -8\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"advanced_shader\n{\n\tname global\n"
	"\tshaderop DefaultDirectLighting           1 100 +\n"
	"\tshaderop DefaultCausticSpectralPhotonMap 1 100 +\n}\n\n"
	"pixelintegratingspectral_rasterizer\n{\n\tsamples 1\n\tlum_samples 1\n\tnmbegin 405\n\tnmend 705\n\tnum_wavelengths 4\n\tmax_recursion 3\n}\n\n"
	"directional_light\n{\n\tname sun\n\tpower 3.14159\n\tcolor 1 1 1\n\tdirection 0 0 -1\n}\n\n"
	"uniformcolor_painter\n{\n\tname albedo\n\tcolor 0.7 0.7 0.7\n}\n\n"
	"lambertian_material\n{\n\tname matte\n\treflectance albedo\n}\n\n"
	"sphere_geometry\n{\n\tname ball\n\tradius 1.5\n}\n\n"
	"standard_object\n{\n\tname obj_ball\n\tgeometry ball\n\tposition 0 0 0\n\tmaterial matte\n\tshader global\n}\n\n"
	"caustic_spectral_photonmap\n{\n\tnum 500\n\tnmbegin 405\n\tnmend 705\n\tnum_wavelengths 4\n}\n\n"
	"caustic_spectral_gather\n{\n\tmax_photons 50\n\tradius 0.5\n}\n";

static void TestSpectralConsumerShoots()
{
	std::cout << "Test: spectral shader-graph rasterizer is a photon-map consumer...\n";

	const std::string scenePath = WriteSceneToTempFile( kSpectralSceneText );
	if( scenePath.empty() ) { Check( false, "spectral scene temp file written" ); return; }

	Scene::ResetPhotonShootCount();

	IJobPriv* pJob = 0;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) { Check( false, "Job created" ); return; }
	Check( pJob->LoadAsciiScene( scenePath.c_str() ), "spectral scene loaded (caustic_spectral map + pixelintegratingspectral)" );

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "spectral photon-deferral capture" );
	pCap->addref();

	// Spectral NON-consumer (BDPT-spectral, own transport) -> no shoot, pending preserved.
	const bool bdptOk = RenderWith( pJob, "bdpt_spectral_rasterizer", pCap );
	Check( bdptOk, "BDPT-spectral render ran" );
	const unsigned int afterBDPT = Scene::GetPhotonShootCount();
	Check( afterBDPT == 0,
		( "BDPT-spectral did NOT shoot (count=" + std::to_string( afterBDPT ) + ", want 0)" ).c_str() );

	// Spectral CONSUMER (pixelintegratingspectral) -> the deferred shoot fires.
	// This is the regression guard for the misclassified-spectral-consumer bug.
	const bool specOk = RenderWith( pJob, "pixelintegratingspectral_rasterizer", pCap );
	Check( specOk, "pixelintegratingspectral render ran" );
	const unsigned int afterSpec = Scene::GetPhotonShootCount();
	Check( afterSpec == 1,
		( "spectral shader-graph rasterizer triggered the deferred shoot (count=" + std::to_string( afterSpec ) + ", want 1)" ).c_str() );

	safe_release( pCap );
	safe_release( pJob );
}

int main()
{
	TestPhotonShootGatedOnConsumer();
	TestSpectralConsumerShoots();

	if( g_failures == 0 ) {
		std::cout << "All PhotonMapDeferral tests passed.\n";
		return 0;
	}
	std::cout << g_failures << " PhotonMapDeferral check(s) FAILED.\n";
	return 1;
}
