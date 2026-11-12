//////////////////////////////////////////////////////////////////////
//
//  CliOverridesTest.cpp - Unit and integration tests for post-load CLI
//    overrides (--width, --height, --pixel-ar, --denoise, --no-denoise,
//    --camera <name> / --camera=<name>) across concrete rasterizers and
//    AutoRasterizer.
//
//  OIDN-DENOISED-OK: this test suite explicitly verifies both raw
//  OutputImage and denoised OutputDenoisedImage dispatch under
//  --denoise and --no-denoise.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
inline int RiseTestPid() { return _getpid(); }
#else
#include <unistd.h>
inline int RiseTestPid() { return static_cast<int>( ::getpid() ); }
#endif

#include "../src/RISE/CliOptions.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int g_pass = 0;
static int g_fail = 0;

static void Check( bool ok, const std::string& label )
{
	if( ok ) {
		++g_pass;
	} else {
		++g_fail;
		std::cout << "  FAIL: " << label << std::endl;
	}
}

static std::string TempScenePath( const char* tag )
{
	std::error_code ec;
	std::filesystem::path dir = std::filesystem::temp_directory_path( ec );
	if( ec || dir.empty() ) dir = ".";
	char fname[256];
	std::snprintf( fname, sizeof( fname ), "cli_overrides_%s_%d.RISEscene", tag, RiseTestPid() );
	return ( dir / fname ).generic_string();
}

class DenoiseTrackingOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	bool sawRaw = false;
	bool sawPreDenoised = false;
	bool sawDenoised = false;
	unsigned int width = 0;
	unsigned int height = 0;
	std::vector<RISEColor> rawPixels;

protected:
	virtual ~DenoiseTrackingOutput() {}

public:
	void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

	void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		sawRaw = true;
		width = img.GetWidth();
		height = img.GetHeight();
		rawPixels.resize( static_cast<std::size_t>( width ) * height );
		for( unsigned int y = 0; y < height; ++y ) {
			for( unsigned int x = 0; x < width; ++x ) {
				rawPixels[static_cast<std::size_t>( y ) * width + x] = img.GetPEL( x, y );
			}
		}
	}

	void OutputPreDenoisedImage( const IRasterImage&, const Rect*, const unsigned int ) override
	{
		sawPreDenoised = true;
	}

	void OutputDenoisedImage( const IRasterImage&, const Rect*, const unsigned int ) override
	{
		sawDenoised = true;
	}
};

static bool RunParse(
	const std::vector<std::string>& args,
	Cli::PostLoadCliOverrides& outOv,
	std::string& outErr )
{
	std::vector<char*> argv;
	std::string prog = "rise";
	argv.push_back( &prog[0] );
	std::vector<std::string> copy = args;
	for( std::string& s : copy ) {
		argv.push_back( &s[0] );
	}
	const int argc = static_cast<int>( argv.size() );
	std::ostringstream err;
	for( int ai = 1; ai < argc; ++ai ) {
		const Cli::FlagMatch m = Cli::TryConsumePostLoadCliFlag( argc, argv.data(), ai, outOv, err );
		if( m == Cli::FlagMatch::MatchedError ) {
			outErr = err.str();
			return false;
		}
	}
	outErr = err.str();
	return true;
}

static void TestFlagParsing()
{
	std::cout << "1: CLI flag parser validation\n";
	{
		Cli::PostLoadCliOverrides ov;
		std::string err;
		const bool ok = RunParse(
			{ "--width", "320", "--height", "180", "--pixel-ar", "1.25", "--denoise", "--camera", "cam_red" },
			ov, err );
		Check( ok && err.empty(), "valid full flag set parses cleanly" );
		Check( ov.filmWidth == 320 && ov.filmHeight == 180 && std::fabs( ov.filmPixelAR - 1.25 ) < 1e-12,
			"film dimensions parsed" );
		Check( ov.denoiseMode == 1, "--denoise sets denoiseMode=1" );
		Check( ov.hasCamera && ov.cameraName == "cam_red", "--camera space form sets cameraName" );
	}
	{
		Cli::PostLoadCliOverrides ov;
		std::string err;
		const bool ok = RunParse( { "--no-denoise", "--camera=cam_blue" }, ov, err );
		Check( ok && ov.denoiseMode == 0 && ov.hasCamera && ov.cameraName == "cam_blue",
			"--no-denoise and --camera=cam_blue parse cleanly" );
	}
	{
		Cli::PostLoadCliOverrides ov;
		std::string err;
		const bool ok = RunParse( { "--denoise", "--no-denoise" }, ov, err );
		Check( !ok && err.find( "mutually exclusive" ) != std::string::npos,
			"--denoise combined with --no-denoise fails loudly" );
	}
	{
		Cli::PostLoadCliOverrides ov;
		std::string err;
		const bool ok = RunParse( { "--no-denoise", "--denoise" }, ov, err );
		Check( !ok && err.find( "mutually exclusive" ) != std::string::npos,
			"--no-denoise combined with --denoise fails loudly" );
	}
	for( const std::vector<std::string>& bad : std::vector<std::vector<std::string> >{
		{ "--denoise=true" },
		{ "--no-denoise=0" },
		{ "--camera" },
		{ "--camera", "--width" },
		{ "--camera=" },
		{ "--width", "0" },
		{ "--width", "-8" },
		{ "--width", "3000000000" },
		{ "--width", "4294967297" },
		{ "--height", "99999999999" },
		{ "--height", "abc" },
		{ "--pixel-ar", "0" },
		{ "--pixel-ar", "-1.5" }
	} ) {
		Cli::PostLoadCliOverrides ov;
		std::string err;
		const bool ok = RunParse( bad, ov, err );
		Check( !ok && !err.empty(), "malformed flag rejected loudly: " + bad[0] );
	}
}

static std::string MultiCamScene( const char* rasterizerChunk )
{
	std::ostringstream s;
	s << "RISE ASCII SCENE 7\n\n"
	  << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	  << "film\n{\n\twidth 16\n\theight 16\n}\n\n"
	  << "pinhole_camera\n{\n\tname cam_red\n\tlocation -5 0 4\n\tlookat -5 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
	  << "pinhole_camera\n{\n\tname cam_blue\n\tlocation 5 0 4\n\tlookat 5 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
	  << rasterizerChunk << "\n\n"
	  << "uniformcolor_painter\n{\n\tname pnt_red\n\tcolor 1.0 0.0 0.0\n}\n\n"
	  << "uniformcolor_painter\n{\n\tname pnt_blue\n\tcolor 0.0 0.0 1.0\n}\n\n"
	  << "lambertian_luminaire_material\n{\n\tname lum_red\n\texitance pnt_red\n\tscale 4.0\n\tmaterial none\n}\n\n"
	  << "lambertian_luminaire_material\n{\n\tname lum_blue\n\texitance pnt_blue\n\tscale 4.0\n\tmaterial none\n}\n\n"
	  << "sphere_geometry\n{\n\tname sph\n\tradius 2.5\n}\n\n"
	  << "standard_object\n{\n\tname obj_red\n\tgeometry sph\n\tmaterial lum_red\n\tposition -5 0 0\n}\n\n"
	  << "standard_object\n{\n\tname obj_blue\n\tgeometry sph\n\tmaterial lum_blue\n\tposition 5 0 0\n}\n";
	return s.str();
}

static void SumRGB( const std::vector<RISEColor>& px, double& r, double& b )
{
	r = 0.0;
	b = 0.0;
	for( const RISEColor& c : px ) {
		r += c.base.r;
		b += c.base.b;
	}
}

static void TestCameraAndFilmOverrides()
{
	std::cout << "2: --camera and --width/--height post-load application\n";
	const std::string sceneText = MultiCamScene(
		"pathtracing_pel_rasterizer\n{\n\tsamples 1\n\toidn_denoise FALSE\n}" );
	const std::string path = TempScenePath( "multicam" );
	{
		std::ofstream ofs( path );
		ofs << sceneText;
	}

	// Default camera (last in file = cam_blue):
	{
		IJobPriv* pJob = nullptr;
		Check( RISE_CreateJobPriv( &pJob ) && pJob, "created job (default camera)" );
		if( pJob ) {
			const bool loaded = pJob->LoadAsciiSceneAuto( path.c_str() );
			Check( loaded && pJob->GetRasterizer() != nullptr, "loaded multicam scene" );
			if( loaded && pJob->GetRasterizer() ) {
				DenoiseTrackingOutput* out = new DenoiseTrackingOutput();
				pJob->GetRasterizer()->AddRasterizerOutput( out );
				Check( pJob->Rasterize(), "rendered default camera" );
				double sumR = 0.0, sumB = 0.0;
				SumRGB( out->rawPixels, sumR, sumB );
				Check( sumB > 1.0 && sumR < 1e-6, "default active camera is cam_blue (sees blue sphere)" );
				safe_release( out );
			}
			safe_release( pJob );
		}
	}

	// Override to cam_red + 24x12 film:
	{
		IJobPriv* pJob = nullptr;
		Check( RISE_CreateJobPriv( &pJob ) && pJob, "created job (--camera cam_red)" );
		if( pJob ) {
			const bool loaded = pJob->LoadAsciiSceneAuto( path.c_str() );
			Check( loaded && pJob->GetRasterizer() != nullptr, "loaded multicam scene for override" );
			if( loaded && pJob->GetRasterizer() ) {
				DenoiseTrackingOutput* out = new DenoiseTrackingOutput();
				// Attach BEFORE override to verify SetActiveCamera + SetFilm + SetDenoisingOverride
				// preserve existing rasterizer outputs (unlike rebuilding the rasterizer).
				pJob->GetRasterizer()->AddRasterizerOutput( out );

				Cli::PostLoadCliOverrides ov;
				ov.hasCamera = true;
				ov.cameraName = "cam_red";
				ov.filmWidth = 24;
				ov.filmHeight = 12;
				std::ostringstream err;
				Check( Cli::ApplyPostLoadCliOverrides( pJob, ov, err ) && err.str().empty(),
					"ApplyPostLoadCliOverrides(--camera cam_red --width 24 --height 12) succeeded" );
				Check( pJob->Rasterize(), "rendered overridden camera" );
				Check( out->sawRaw && out->width == 24 && out->height == 12,
					"attached output preserved and received 24x12 image" );
				double sumR = 0.0, sumB = 0.0;
				SumRGB( out->rawPixels, sumR, sumB );
				Check( sumR > 1.0 && sumB < 1e-6, "--camera cam_red switched view to red sphere" );
				safe_release( out );
			}
			safe_release( pJob );
		}
	}

	// Unknown camera lists available cameras:
	{
		IJobPriv* pJob = nullptr;
		if( RISE_CreateJobPriv( &pJob ) && pJob ) {
			Check( pJob->LoadAsciiSceneAuto( path.c_str() ), "loaded multicam scene for unknown camera" );
			Cli::PostLoadCliOverrides ov;
			ov.hasCamera = true;
			ov.cameraName = "nonexistent_cam";
			std::ostringstream err;
			const bool ok = Cli::ApplyPostLoadCliOverrides( pJob, ov, err );
			const std::string msg = err.str();
			Check( !ok, "unknown --camera fails ApplyPostLoadCliOverrides" );
			Check( msg.find( "cam_red" ) != std::string::npos && msg.find( "cam_blue" ) != std::string::npos,
				"unknown --camera diagnostic lists available camera names: " + msg );
			safe_release( pJob );
		}
	}

	std::remove( path.c_str() );
}

static void TestDenoiseOverrides()
{
	std::cout << "3: --denoise and --no-denoise across concrete and auto rasterizers\n";

	// Case A: authored oidn_denoise TRUE, overridden with --no-denoise on pathtracing_pel_rasterizer
	{
		const std::string path = TempScenePath( "pt_nodenoise" );
		{
			std::ofstream ofs( path );
			ofs << MultiCamScene( "pathtracing_pel_rasterizer\n{\n\tsamples 1\n\toidn_denoise TRUE\n}" );
		}
		IJobPriv* pJob = nullptr;
		if( RISE_CreateJobPriv( &pJob ) && pJob ) {
			Check( pJob->LoadAsciiSceneAuto( path.c_str() ), "loaded PT scene with oidn_denoise TRUE" );
			DenoiseTrackingOutput* out = new DenoiseTrackingOutput();
			pJob->GetRasterizer()->AddRasterizerOutput( out );

			Cli::PostLoadCliOverrides ov;
			ov.denoiseMode = 0; // --no-denoise
			std::ostringstream err;
			Check( Cli::ApplyPostLoadCliOverrides( pJob, ov, err ), "--no-denoise applied on PT rasterizer" );
			Check( pJob->GetRasterizer()->GetDenoisingOverride() == 0, "GetDenoisingOverride() == 0 after --no-denoise" );
			Check( pJob->Rasterize(), "PT render succeeded after --no-denoise" );
			Check( out->sawRaw && !out->sawPreDenoised && !out->sawDenoised,
				"--no-denoise suppressed OutputPreDenoisedImage/OutputDenoisedImage while dispatching OutputImage" );
			safe_release( out );
			safe_release( pJob );
		}
		std::remove( path.c_str() );
	}

#ifdef RISE_ENABLE_OIDN
	// Case B: authored oidn_denoise FALSE, overridden with --denoise on pathtracing_pel_rasterizer
	{
		const std::string path = TempScenePath( "pt_denoise" );
		{
			std::ofstream ofs( path );
			ofs << MultiCamScene( "pathtracing_pel_rasterizer\n{\n\tsamples 1\n\toidn_denoise FALSE\n}" );
		}
		IJobPriv* pJob = nullptr;
		if( RISE_CreateJobPriv( &pJob ) && pJob ) {
			Check( pJob->LoadAsciiSceneAuto( path.c_str() ), "loaded PT scene with oidn_denoise FALSE" );
			DenoiseTrackingOutput* out = new DenoiseTrackingOutput();
			pJob->GetRasterizer()->AddRasterizerOutput( out );

			Cli::PostLoadCliOverrides ov;
			ov.denoiseMode = 1; // --denoise
			std::ostringstream err;
			Check( Cli::ApplyPostLoadCliOverrides( pJob, ov, err ), "--denoise applied on PT rasterizer" );
			Check( pJob->GetRasterizer()->GetDenoisingOverride() == 1, "GetDenoisingOverride() == 1 after --denoise" );
			Check( pJob->Rasterize(), "PT render succeeded after --denoise" );
			Check( out->sawPreDenoised && out->sawDenoised && !out->sawRaw,
				"--denoise dispatched OutputPreDenoisedImage + OutputDenoisedImage on scene authored with oidn_denoise FALSE" );
			safe_release( out );
			safe_release( pJob );
		}
		std::remove( path.c_str() );
	}

	// Case C: AutoRasterizer both pre-resolve (--denoise) and post-resolve (--no-denoise)
	{
		const std::string path = TempScenePath( "auto_denoise" );
		{
			std::ofstream ofs( path );
			ofs << MultiCamScene( "auto_rasterizer\n{\n\tsamples 1\n\toidn_denoise FALSE\n}" );
		}
		IJobPriv* pJob = nullptr;
		if( RISE_CreateJobPriv( &pJob ) && pJob ) {
			Check( pJob->LoadAsciiSceneAuto( path.c_str() ), "loaded auto_rasterizer scene with oidn_denoise FALSE" );
			DenoiseTrackingOutput* out = new DenoiseTrackingOutput();
			pJob->GetRasterizer()->AddRasterizerOutput( out );

			Cli::PostLoadCliOverrides ovOn;
			ovOn.denoiseMode = 1;
			std::ostringstream err;
			Check( Cli::ApplyPostLoadCliOverrides( pJob, ovOn, err ),
				"--denoise applied on AutoRasterizer before lazy delegate resolution" );
			Check( pJob->Rasterize(), "AutoRasterizer first render (with --denoise)" );
			Check( out->sawPreDenoised && out->sawDenoised && !out->sawRaw,
				"AutoRasterizer forwarded pre-resolve --denoise to lazily built delegate" );

			out->sawRaw = false;
			out->sawPreDenoised = false;
			out->sawDenoised = false;
			Cli::PostLoadCliOverrides ovOff;
			ovOff.denoiseMode = 0;
			Check( Cli::ApplyPostLoadCliOverrides( pJob, ovOff, err ),
				"--no-denoise applied on AutoRasterizer after delegate resolution" );
			Check( pJob->Rasterize(), "AutoRasterizer second render (with --no-denoise)" );
			Check( out->sawRaw && !out->sawPreDenoised && !out->sawDenoised,
				"AutoRasterizer forwarded post-resolve --no-denoise to live delegate" );

			safe_release( out );
			safe_release( pJob );
		}
		std::remove( path.c_str() );
	}

	// Case D: AutoRasterizer (spectral mode: auto_spectral_rasterizer) both pre-resolve and post-resolve
	{
		const std::string path = TempScenePath( "auto_spec_denoise" );
		{
			std::ofstream ofs( path );
			ofs << MultiCamScene( "auto_spectral_rasterizer\n{\n\tsamples 1\n\toidn_denoise FALSE\n}" );
		}
		IJobPriv* pJob = nullptr;
		if( RISE_CreateJobPriv( &pJob ) && pJob ) {
			Check( pJob->LoadAsciiSceneAuto( path.c_str() ), "loaded auto_spectral_rasterizer scene with oidn_denoise FALSE" );
			DenoiseTrackingOutput* out = new DenoiseTrackingOutput();
			pJob->GetRasterizer()->AddRasterizerOutput( out );

			Cli::PostLoadCliOverrides ovOn;
			ovOn.denoiseMode = 1;
			std::ostringstream err;
			Check( Cli::ApplyPostLoadCliOverrides( pJob, ovOn, err ),
				"--denoise applied on auto_spectral_rasterizer before lazy delegate resolution" );
			Check( pJob->Rasterize(), "auto_spectral_rasterizer first render (with --denoise)" );
			Check( out->sawPreDenoised && out->sawDenoised && !out->sawRaw,
				"auto_spectral_rasterizer forwarded pre-resolve --denoise to lazily built spectral delegate" );

			out->sawRaw = false;
			out->sawPreDenoised = false;
			out->sawDenoised = false;
			Cli::PostLoadCliOverrides ovOff;
			ovOff.denoiseMode = 0;
			Check( Cli::ApplyPostLoadCliOverrides( pJob, ovOff, err ),
				"--no-denoise applied on auto_spectral_rasterizer after delegate resolution" );
			Check( pJob->Rasterize(), "auto_spectral_rasterizer second render (with --no-denoise)" );
			Check( out->sawRaw && !out->sawPreDenoised && !out->sawDenoised,
				"auto_spectral_rasterizer forwarded post-resolve --no-denoise to live spectral delegate" );

			safe_release( out );
			safe_release( pJob );
		}
		std::remove( path.c_str() );
	}
#endif
}

int main()
{
	std::cout << "=== CliOverridesTest ===\n";
	TestFlagParsing();
	TestCameraAndFilmOverrides();
	TestDenoiseOverrides();
	std::cout << "\n" << g_pass << " passed, " << g_fail << " failed\n";
	return g_fail == 0 ? 0 : 1;
}
