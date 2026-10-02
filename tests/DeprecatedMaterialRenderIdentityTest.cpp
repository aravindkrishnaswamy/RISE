//////////////////////////////////////////////////////////////////////
//
//  DeprecatedMaterialRenderIdentityTest.cpp - DL-323 follow-through
//    (debt-deprecate, 2026-10-02): a scene that uses EVERY deprecated
//    legacy material still renders bit-identically to the pre-deprecation
//    build.
//
//  The ruling this guards (user, 2026-10-02): the legacy
//  non-physically-based materials are DEPRECATED, not retrofitted --
//  they keep rendering exactly as they did.  The deprecation mechanism
//  (a descriptor flag consumed by DeriveToJob to log one warning per
//  chunk type) touches no render path, and this test pins that: it
//  renders a tiny single-threaded scene that binds each of the seven
//  deprecated chunk types plus controls (translucent_material -- NOT
//  deprecated -- ggx_material, lambertian_material) and compares an
//  FNV-1a hash of every output pixel's four doubles to a constant
//  measured on the PARENT commit (f03359223, before any deprecation
//  code existed), by rebuilding that commit's library and running this
//  very file against it.  A drift in ANY pixel changes the hash.
//
//  Determinism: main() forces ONE render worker (a multithreaded render
//  seeds each worker's RNG from libc rand() in thread-start order),
//  std::srand is pinned per render, and the Sobol' value salt is 0.
//  Set DEPRECATED_RENDER_PRINT=1 to print the measured hash.
//
//  Author: Claude (Sonnet 5.5)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/SobolSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* what )
{
	if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", what ); }
}

class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	CapturingRasterizerOutput() {}
protected:
	virtual ~CapturingRasterizerOutput() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		const unsigned int w = img.GetWidth(), h = img.GetHeight();
		pixels.resize( w * h );
		for( unsigned int y = 0; y < h; y++ )
			for( unsigned int x = 0; x < w; x++ )
				pixels[y * w + x] = img.GetPEL( x, y );
	}
};

static unsigned long long PixelHash( const std::vector<RISEColor>& px )
{
	unsigned long long h = 1469598103934665603ull;
	for( const RISEColor& c : px ) {
		const double v[4] = { c.base.r, c.base.g, c.base.b, c.a };
		const unsigned char* b = reinterpret_cast<const unsigned char*>( v );
		for( size_t i = 0; i < sizeof( v ); i++ ) { h ^= b[i]; h *= 1099511628211ull; }
	}
	return h;
}

//! A row of ten spheres, one material each: the seven deprecated
//! chunk types, then translucent_material (NOT deprecated), ggx_material
//! and lambertian_material as controls.  16 spp of direct+indirect
//! path tracing under an omni light; small enough to render in about a
//! second on one worker.
static std::string SceneText()
{
	std::ostringstream ss;
	ss << "RISE ASCII SCENE 7\n"
		"film\n{\n\twidth 40\n\theight 16\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 1.5 9\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_d\n\tcolor 0.6 0.3 0.2\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_s\n\tcolor 0.5 0.5 0.5\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_r\n\tcolor 0.3 0.3 0.3\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_t\n\tcolor 0.4 0.4 0.4\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.5 0.5 0.5\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		// the seven deprecated chunk types
		"cooktorrance_material\n{\n\tname m0\n\trd pnt_d\n\trs pnt_s\n\tfacets 0.2\n\tior 1.5\n\textinction 0.0\n}\n\n"
		"isotropic_phong_material\n{\n\tname m1\n\trd pnt_d\n\trs pnt_s\n\tN 32\n}\n\n"
		"ashikminshirley_anisotropicphong_material\n{\n\tname m2\n\trd pnt_d\n\trs pnt_s\n\tnu 20\n\tnv 80\n}\n\n"
		"schlick_material\n{\n\tname m3\n\trd pnt_d\n\trs pnt_s\n\troughness 0.2\n\tisotropy 1\n}\n\n"
		"ward_isotropic_material\n{\n\tname m4\n\trd pnt_d\n\trs pnt_s\n\talpha 0.15\n}\n\n"
		"ward_anisotropic_material\n{\n\tname m5\n\trd pnt_d\n\trs pnt_s\n\talphax 0.1\n\talphay 0.2\n}\n\n"
		"polished_material\n{\n\tname m6\n\treflectance pnt_d\n\ttau 0.9\n\tior 1.5\n\tscattering 100\n}\n\n"
		// controls
		"translucent_material\n{\n\tname m7\n\tref pnt_r\n\ttau pnt_t\n\text 0.1\n\tN 10\n\tscattering 0.5\n}\n\n"
		"ggx_material\n{\n\tname m8\n\trd pnt_d\n\trs pnt_s\n\talphax 0.2\n\talphay 0.2\n}\n\n"
		"lambertian_material\n{\n\tname m9\n\treflectance pnt_d\n}\n\n"
		"lambertian_material\n{\n\tname m_floor\n\treflectance pnt_floor\n}\n\n"
		"sphere_geometry\n{\n\tname geo_s\n\tradius 0.5\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_floor\n\tpta -8 -0.5 8\n\tptb 8 -0.5 8\n\tptc 8 -0.5 -8\n\tptd -8 -0.5 -8\n\tdoublesided TRUE\n}\n\n"
		"standard_object\n{\n\tname obj_floor\n\tgeometry geo_floor\n\tmaterial m_floor\n}\n\n";
	for( int i = 0; i < 10; i++ ) {
		const double x = ( i - 4.5 ) * 1.2;
		ss << "standard_object\n{\n\tname obj" << i << "\n\tgeometry geo_s\n\tmaterial m" << i
		   << "\n\tposition " << x << " 0 0\n}\n\n";
	}
	ss << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"omni_light\n{\n\tname lgt\n\tposition 2 6 6\n\tcolor 1 1 1\n\tpower 600\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 16\n\trr_min_depth 4\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n"
		"file_rasterizeroutput\n{\n\tpattern rendered/deprecated_material_identity_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
	return ss.str();
}

static bool RenderHash( unsigned long long& hash, double& mean )
{
	char path[512];
	std::snprintf( path, sizeof( path ), "/tmp/deprecated_material_identity_%d.RISEscene", static_cast<int>( ::getpid() ) );
	{
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return false;
		ofs << SceneText();
	}
	std::srand( 4242 );
	SobolSamplerTestHooks::ValueSalt().store( 0u );

	bool ok = false;
	IJobPriv* pJob = nullptr;
	if( RISE_CreateJobPriv( &pJob ) && pJob ) {
		if( pJob->LoadAsciiSceneViaCst( path ) ) {
			pJob->RemoveRasterizerOutputs();
			CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
			GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
			pJob->GetRasterizer()->AddRasterizerOutput( pCap );
			if( pJob->Rasterize() && !pCap->pixels.empty() ) {
				hash = PixelHash( pCap->pixels );
				double s = 0;
				for( const RISEColor& c : pCap->pixels ) s += 0.2126 * c.base.r * c.a + 0.7152 * c.base.g * c.a + 0.0722 * c.base.b * c.a;
				mean = s / double( pCap->pixels.size() );
				ok = true;
			}
			safe_release( pCap );
		}
		safe_release( pJob );
	}
	std::remove( path );
	return ok;
}

//! Measured on the parent commit f03359223 (no deprecation code).
//! Measured by building f03359223 verbatim (library + this test) and running
//! the test in a fresh process three times: mean 0.607363772, hash identical.
static const unsigned long long kParentHash = 0x13c3910b9964ab63ull;

static char g_optPath[512] = { 0 };

int main()
{
	std::printf( "DeprecatedMaterialRenderIdentityTest (DL-323 follow-through)\n" );

	if( !std::getenv( "RISE_OPTIONS_FILE" ) ) {
		std::snprintf( g_optPath, sizeof( g_optPath ), "/tmp/deprecated_material_options_%d.txt", static_cast<int>( ::getpid() ) );
		std::atexit( []() { std::remove( g_optPath ); } );
		std::ofstream opt( g_optPath );
		opt << "force_number_of_threads 1\n";
		opt.close();
#ifdef _WIN32
		_putenv_s( "RISE_OPTIONS_FILE", g_optPath );
#else
		setenv( "RISE_OPTIONS_FILE", g_optPath, 1 );
#endif
	}

	// ONE render per process: a second in-process render is not bit-identical
	// (render-global RNG state carries over), so determinism is a property of
	// a fresh process, which is also how the parent-commit hash was taken.
	unsigned long long h1 = 0;
	double m1 = 0;
	const bool ok1 = RenderHash( h1, m1 );
	Check( ok1, "the all-deprecated-materials scene loads and renders" );
	Check( m1 > 1e-4, "the render is not black (mean luminance > 1e-4)" );
	if( std::getenv( "DEPRECATED_RENDER_PRINT" ) )
		std::printf( "  mean %.9f hash %016llx\n", m1, h1 );
	if( kParentHash != 0ull ) {
		char msg[160];
		std::snprintf( msg, sizeof( msg ), "render hash %016llx == the pre-deprecation parent's %016llx", h1, kParentHash );
		Check( h1 == kParentHash, msg );
	}

	std::printf( "%d passed, %d failed\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
