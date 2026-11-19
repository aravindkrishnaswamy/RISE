//////////////////////////////////////////////////////////////////////
//
//  PhotonTemporalStrataTest.cpp - DL-457 time-stratified photon shooting
//  really moves the scene during a Job animation render.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-11-18
//  Tabs: 4
//  Comments:
//
//    PhotonTracer / SpectralPhotonTracer::TracePhotons (bAtTime, camera
//    exposure > 0, temporal_samples > 1) shoots each time stratum after
//    evaluating the animator at a time inside it.  That evaluation runs
//    inside Job::RasterizeAnimation, so a render-wide scene-mutation freeze
//    (Job::BeginPreparedRequest) that refused it would silently shoot every
//    stratum from the frame-time pose.  Nothing else exercised this path.
//
//    Scene: an emitter 0.1 above a lambertian floor (an omni light for the
//    pel tracer, a small luminaire sphere for the spectral one), keyframed to slide
//    x = -2 -> +2 over t in [0, 1]; frame time 0.5 (pose x = 0), exposure 1,
//    so the strata cover the whole slide.  A floor point at x = -1.8 sees
//    the light pass overhead only when the strata move it: its photon count
//    is then comparable to the count under the frame-time pose, and almost
//    zero (density ~ h / r^3) when every photon leaves from x = 0.  The
//    temporal_samples 1 control shows the measure separates the two.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined( _WIN32 )
	#include <process.h>
	#define RISE_TEST_GETPID _getpid
#else
	#include <unistd.h>
	#define RISE_TEST_GETPID getpid
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IPhotonMap.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int g_failures = 0;
static std::vector<std::string> g_tmpScenes;

static void Check( const bool cond, const std::string& what )
{
	std::cout << ( cond ? "  [ok] " : "  [FAIL] " ) << what << "\n";
	if( !cond ) ++g_failures;
}

static std::string WriteScene( const std::string& text, const char* tag )
{
	const char* tempDir = std::getenv( "TMPDIR" );
	if( !tempDir || !*tempDir ) tempDir = ".";
	std::string path( tempDir );
	if( path.back() != '/' && path.back() != '\\' ) path += '/';
	char name[256];
	std::snprintf( name, sizeof(name), "photon_temporal_%s_%d.RISEscene", tag,
		static_cast<int>( RISE_TEST_GETPID() ) );
	path += name;
	std::ofstream ofs( path.c_str() );
	if( !ofs.is_open() ) return std::string();
	ofs << text;
	g_tmpScenes.push_back( path );
	return path;
}

static std::string SceneText( const bool spectral, const unsigned int temporalSamples )
{
	const std::string temporal = "\ttemporal_samples " + std::to_string( temporalSamples ) +
		"\n\tregenerate TRUE\n";
	std::string s =
		"RISE ASCII SCENE 7\n\n"
		"film\n{\n\twidth 4\n\theight 4\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n\texposure 1.0\n}\n\n";
	if( spectral ) {
		s += "advanced_shader\n{\n\tname global\n\tshaderop DefaultGlobalSpectralPhotonMap 1 100 +\n}\n\n"
			"pixelintegratingspectral_rasterizer\n{\n\tsamples 1\n\tlum_samples 1\n\tnmbegin 405\n\tnmend 705\n"
			"\tnum_wavelengths 4\n\tmax_recursion 1\n\toidn_denoise FALSE\n}\n\n";
	} else {
		s += "advanced_shader\n{\n\tname global\n\tshaderop DefaultGlobalPelPhotonMap 1 100 +\n}\n\n"
			"pixelpel_rasterizer\n{\n\tmax_recursion 1\n\tsamples 1\n\tlum_samples 1\n\toidn_denoise FALSE\n}\n\n";
	}
	// The pel tracer shoots from the omni light; the spectral tracer shoots
	// only from luminaires, so there the moving emitter is a small sphere.
	if( spectral ) {
		s += "uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
			"lambertian_luminaire_material\n{\n\tname lum\n\texitance white\n\tscale 10.0\n\tmaterial none\n}\n\n"
			"sphere_geometry\n{\n\tname lum_geo\n\tradius 0.03\n}\n\n"
			"standard_object\n{\n\tname l_sphere\n\tgeometry lum_geo\n\tposition 0 0 0.1\n\tmaterial lum\n}\n\n";
	} else {
		s += "omni_light\n{\n\tname l_omni\n\tpower 10.0\n\tcolor 1 1 1\n\tposition 0 0 0.1\n}\n\n";
	}
	s +=
		"uniformcolor_painter\n{\n\tname albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\n\tname matte\n\treflectance albedo\n}\n\n"
		"infiniteplane_geometry\n{\n\tname floor_geo\n}\n\n"
		"standard_object\n{\n\tname floor\n\tgeometry floor_geo\n\tposition 0 0 0\n\tmaterial matte\n\tshader global\n}\n\n";
	if( spectral ) {
		s += "global_spectral_photonmap\n{\n\tnum 20000\n\tnmbegin 405\n\tnmend 705\n\tnum_wavelengths 4\n"
			"\tmax_recursion 1\n" + temporal + "}\n\n"
			"global_spectral_gather\n{\n\tmax_photons 50\n\tradius 0.5\n}\n\n";
	} else {
		s += "global_pel_photonmap\n{\n\tnum 20000\n\tmax_recursion 1\n" + temporal + "}\n\n"
			"global_pel_gather\n{\n\tmax_photons 50\n\tradius 0.5\n}\n\n";
	}
	s += std::string( "timeline\n{\n\telement_type " ) + ( spectral ? "object\n\telement l_sphere" :
		"light\n\telement l_omni" ) + "\n\tparam position\n\tinterpolator linear\n"
		"\ttime 0\n\tvalue -2 0 0.1\n\ttime 1\n\tvalue 2 0 0.1\n}\n";
	return s;
}

//! Renders frame time 0.5 and returns the stored-photon counts within 0.25 of
//! the floor points under the frame-time pose (x = 0) and at x = -1.8.
static bool Measure( const bool spectral, const unsigned int temporalSamples,
	unsigned int& atPose, unsigned int& offPose )
{
	atPose = offPose = 0;
	const std::string path = WriteScene( SceneText( spectral, temporalSamples ),
		spectral ? "spectral" : "pel" );
	if( path.empty() ) return false;
	IJobPriv* pJob = 0;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) return false;
	bool ok = pJob->LoadAsciiSceneViaCst( path.c_str() ) &&
		pJob->RasterizeAnimation( 0.5, 0.5, 1, false, false );
	const IScene* pScene = pJob->GetScene();
	const IPhotonMap* pMap = !pScene ? 0 : spectral ?
		static_cast<const IPhotonMap*>( pScene->GetGlobalSpectralMap() ) : pScene->GetGlobalPelMap();
	ok = ok && pMap;
	if( ok ) {
		pMap->CountPhotonsAt( Point3( 0, 0, 0 ), 0.25, 1000000u, atPose );
		pMap->CountPhotonsAt( Point3( -1.8, 0, 0 ), 0.25, 1000000u, offPose );
	}
	safe_release( pJob );
	return ok;
}

static void RunCase( const bool spectral )
{
	const char* name = spectral ? "spectral" : "pel";
	unsigned int strataPose = 0, strataOff = 0, controlPose = 0, controlOff = 0;
	const bool strataOk = Measure( spectral, 16u, strataPose, strataOff );
	const bool controlOk = Measure( spectral, 1u, controlPose, controlOff );
	std::cout << "  " << name << ": 16 strata at-pose " << strataPose << " off-pose " << strataOff
		<< "; 1 stratum at-pose " << controlPose << " off-pose " << controlOff << "\n";
	Check( strataOk && controlOk, std::string( name ) + ": both renders ran and built a global photon map" );
	// Control: every photon from the frame-time pose leaves x = -1.8 nearly empty.
	Check( controlOk && controlPose > 1000u && controlOff * 50u < controlPose,
		std::string( name ) + ": single-stratum control shoots from the frame-time pose only" );
	// Strata: the light sweeps over x = -1.8, which then collects photons on the
	// order of the frame-time spot (uniform sweep: about equal).
	Check( strataOk && strataOff * 3u > strataPose && strataOff > 100u,
		std::string( name ) + ": time strata move the light across the exposure" );
}

int main()
{
	std::cout << "PhotonTemporalStrataTest\n";
	RunCase( false );
	RunCase( true );
	for( const std::string& f : g_tmpScenes ) std::remove( f.c_str() );
	if( g_failures == 0 ) {
		std::cout << "All PhotonTemporalStrata checks passed.\n";
		return 0;
	}
	std::cout << g_failures << " PhotonTemporalStrata check(s) FAILED.\n";
	return 1;
}
