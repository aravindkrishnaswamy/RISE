//////////////////////////////////////////////////////////////////////
//
//  LegacyTierRenderIdentityTest.cpp - legacy-deprecation Phase 1
//    (2026-10-09, docs/LEGACY_DEPRECATION_ASSESSMENT.md §10.3 / §11):
//    every FROZEN and DEPRECATED chunk family still renders
//    BIT-IDENTICALLY to the build before the tiers existed.
//
//  Phase 1 only marks chunk types (ChunkDescriptor::frozen /
//  deprecated, applied by a descriptor-copying wrapper whose Finalize
//  forwards unchanged).  This test pins that no render path moved: it
//  renders one small shipped scene per legacy family that has a REAL
//  scene (the ChunkCoverage-only chunks -- alpha_test_shaderop, the
//  global-spectral / translucent / shadow photon maps,
//  phong_luminaire_material, onb_pinhole_camera, 3dsmesh_geometry,
//  sms_shaderop, the mis_pathtracing_shaderop alias -- have no scene that
//  renders them; their derive is pinned by DeprecatedMaterialWarningTest and
//  CstDeriveGoldenTest; MLT is NOT hash-pinned because its render is not
//  reproducible run to run even on the parent commit -- two parent-build
//  runs of mlt_deep_fog read different hashes) (film shrunk to at
//  most 48 px on the long side, everything else as shipped), hashes the
//  PRE-DENOISE pixels (FNV-1a over each pixel's four doubles), and
//  compares to constants measured on the PARENT commit 4694f511c (before
//  any tier code) by building that commit's library and running this
//  very file against it with --print.
//
//  Determinism: one render per PROCESS (render-global RNG state carries
//  over between in-process renders), so the default mode re-executes
//  itself once per scene with --hash.  Each child forces ONE render
//  worker, pins std::srand and the Sobol' value salt to 0, and hashes
//  the raw (pre-OIDN) image -- OIDN's Auto quality is time-dependent
//  (DL-360) and is not what this test is about.
//
//  Usage: (no args) gate; --print print every scene's hash; --hash <scene>
//  print one scene's hash (internal).  RISE_MEDIA_PATH must point at the
//  repository root (run_all_tests.sh sets it).
//
//  Author: Claude (Opus 4.8)
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
	#define popen _popen
	#define pclose _pclose
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
#include "../src/Library/Utilities/MediaPathLocator.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

//! One shipped scene per legacy family, with the hash measured on 4694f511c.
struct IdentityCase { const char* scene; const char* families; unsigned long long parentHash; };
static const IdentityCase kCases[] = {
	{ "scenes/Tests/Shaders/arealight_shaderop.RISEscene",                 "pixelpel_rasterizer, arealight_shaderop",                      0x93e83b7e2a654a25ull },
	{ "scenes/Tests/Spectral/uniform_green_spectral.RISEscene",            "pixelintegratingspectral_rasterizer",                          0xa04a111006083383ull },
	{ "scenes/Tests/Materials/dielectric_dispersion.RISEscene",            "distributiontracing_shaderop",                                 0x66f9ef03248c7df6ull },
	{ "scenes/Tests/GlobalIllumination/cornellbox_fg.RISEscene",           "finalgather_shaderop, global photon map",                      0x37e78f37d18c1a38ull },
	{ "scenes/Tests/SubsurfaceScattering/sss_ibl.RISEscene",               "simple_sss_shaderop, ambientocclusion_shaderop",               0x63dccf2811bd3196ull },
	{ "scenes/Tests/SubsurfaceScattering/sss.RISEscene",                   "diffusion_approximation_sss_shaderop",                         0xd1caa85dfbf4ea61ull },
	{ "scenes/Tests/Shaders/transparency_shaderop.RISEscene",              "transparency_shaderop",                                        0x418bd9f01d1c0b81ull },
	{ "scenes/Tests/Caustics/rgb_dispersive_caustic.RISEscene",            "caustic_pel_photonmap / gather",                               0xb898fe392a08ef1eull },
	{ "scenes/FeatureBased/Combined/showroom.RISEscene",                   "irradiance_cache, iridescent_painter, polished_material",      0x7c020159f47bb489ull },
	{ "scenes/Tests/Materials/composite_material.RISEscene",               "composite_material",                                           0x04c301c05241164dull },
	{ "scenes/FeatureBased/Shaders/visiblehuman.RISEscene",                "directvolumerendering_shader",                                 0x7df386e9e94b9a43ull },
	{ "scenes/Tests/Geometry/gltf_box.RISEscene",                          "ambient_light",                                                0xf20722df16b8a405ull },
	{ "scenes/FeatureBased/Animation/translucent_bunny.RISEscene",         "directlighting_shaderop, iridescent_painter",                  0x1513c77a209e3928ull },
	{ "scenes/Tests/Materials/materials.RISEscene",                        "cooktorrance, schlick, ward x2, ashikminshirley materials",   0x01ae464015b8402eull },
	{ "scenes/Tests/Shaders/blurry_floor.RISEscene",                       "isotropic_phong_material",                                     0xf8dba1adcd1f03a9ull },
	{ "scenes/FeatureBased/Geometry/teapot.RISEscene",                     "polished_material, bezierpatch_geometry",                      0x747dd21312bf8410ull },
};

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& what )
{
	if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", what.c_str() ); }
}

// OIDN-DENOISED-OK: shipped scenes are rendered as authored (some with OIDN
// on); RawCapture hashes only the PRE-denoise image (OutputPreDenoisedImage),
// never the denoised final, so OIDN cannot enter the hash.
//! Captures the RAW image: OutputImage when OIDN is off, OutputPreDenoisedImage
//! when it is on; the denoised final is ignored (its default forward to
//! OutputImage is overridden away).
class RawCapture
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	bool havePre = false;
	RawCapture() {}
protected:
	virtual ~RawCapture() {}
	void Grab( const IRasterImage& img )
	{
		const unsigned int w = img.GetWidth(), h = img.GetHeight();
		pixels.resize( w * h );
		for( unsigned int y = 0; y < h; y++ )
			for( unsigned int x = 0; x < w; x++ )
				pixels[y * w + x] = img.GetPEL( x, y );
	}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override { if( !havePre ) Grab( img ); }
	virtual void OutputPreDenoisedImage( const IRasterImage& img, const Rect*, const unsigned int ) override { Grab( img ); havePre = true; }
	virtual void OutputDenoisedImage( const IRasterImage&, const Rect*, const unsigned int ) override {}
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

static std::string TempPath( const std::string& name )
{
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += '/';
	return dir + name;
}

//! Shrink the first `width N` / `height N` pair (the film chunk) so the long
//! side is at most 48 px.  Applied identically on the parent and this build.
static std::string Shrink( const std::string& text )
{
	std::istringstream in( text );
	std::ostringstream out;
	std::string line;
	long W = -1, H = -1;
	std::vector<std::string> lines;
	while( std::getline( in, line ) ) lines.push_back( line );
	int wi = -1, hi = -1;
	for( size_t i = 0; i < lines.size(); i++ ) {
		const std::string& l = lines[i];
		size_t p = l.find_first_not_of( " \t" );
		if( p == std::string::npos ) continue;
		if( wi < 0 && l.compare( p, 6, "width " ) == 0 ) { W = std::atol( l.c_str() + p + 6 ); wi = int( i ); }
		else if( wi < 0 && l.compare( p, 6, "width\t" ) == 0 ) { W = std::atol( l.c_str() + p + 6 ); wi = int( i ); }
		if( hi < 0 && ( l.compare( p, 7, "height " ) == 0 || l.compare( p, 7, "height\t" ) == 0 ) ) { H = std::atol( l.c_str() + p + 7 ); hi = int( i ); }
	}
	if( wi >= 0 && hi >= 0 && W > 0 && H > 0 ) {
		const double s = double( W > H ? W : H ) / 48.0;
		if( s > 1.0 ) {
			const long w2 = long( W / s ) > 0 ? long( W / s ) : 1, h2 = long( H / s ) > 0 ? long( H / s ) : 1;
			lines[wi] = "\twidth " + std::to_string( w2 );
			lines[hi] = "\theight " + std::to_string( h2 );
		}
	}
	for( const std::string& l : lines ) out << l << "\n";
	return out.str();
}

static bool HashScene( const std::string& scenePath, unsigned long long& hash )
{
	const char* media = std::getenv( "RISE_MEDIA_PATH" );
	const std::string full = ( media ? std::string( media ) : std::string() ) + scenePath;
	std::ifstream ifs( full.c_str(), std::ios::binary );
	if( !ifs.is_open() ) { std::fprintf( stderr, "cannot open %s\n", full.c_str() ); return false; }
	std::stringstream ss; ss << ifs.rdbuf();
	if( media ) GlobalMediaPathLocator().AddPath( media );
	const std::string tmp = TempPath( "legacy_identity_" + std::to_string( static_cast<int>( ::getpid() ) ) + ".RISEscene" );
	{ std::ofstream o( tmp.c_str(), std::ios::binary ); o << Shrink( ss.str() ); }

	std::srand( 4242 );
	SobolSamplerTestHooks::ValueSalt().store( 0u );
	bool ok = false;
	IJobPriv* pJob = nullptr;
	if( RISE_CreateJobPriv( &pJob ) && pJob ) {
		if( pJob->LoadAsciiSceneViaCst( tmp.c_str() ) && pJob->GetRasterizer() ) {
			pJob->RemoveRasterizerOutputs();
			RawCapture* pCap = new RawCapture();
			GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
			pJob->GetRasterizer()->AddRasterizerOutput( pCap );
			if( pJob->Rasterize() && !pCap->pixels.empty() ) { hash = PixelHash( pCap->pixels ); ok = true; }
			safe_release( pCap );
		}
		safe_release( pJob );
	}
	std::remove( tmp.c_str() );
	return ok;
}

static char g_optPath[512] = { 0 };

int main( int argc, char** argv )
{
	if( !std::getenv( "RISE_OPTIONS_FILE" ) ) {
		std::snprintf( g_optPath, sizeof( g_optPath ), "%s", TempPath( "legacy_identity_options_" + std::to_string( static_cast<int>( ::getpid() ) ) + ".txt" ).c_str() );
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

	if( argc == 3 && std::strcmp( argv[1], "--hash" ) == 0 ) {
		unsigned long long h = 0;
		if( !HashScene( argv[2], h ) ) { std::printf( "HASH FAILED\n" ); return 2; }
		std::printf( "HASH %016llx\n", h );
		return 0;
	}

	const bool printOnly = ( argc == 2 && std::strcmp( argv[1], "--print" ) == 0 );
	std::printf( "LegacyTierRenderIdentityTest (legacy-deprecation Phase 1)\n" );
	for( const IdentityCase& c : kCases ) {
		const std::string cmd = std::string( "\"" ) + argv[0] + "\" --hash " + c.scene + " 2>&1";
		FILE* p = popen( cmd.c_str(), "r" );
		unsigned long long h = 0;
		bool got = false;
		if( p ) {
			char buf[4096];
			while( std::fgets( buf, sizeof( buf ), p ) )
				if( std::strncmp( buf, "HASH ", 5 ) == 0 && std::strncmp( buf, "HASH FAILED", 11 ) != 0 ) { h = std::strtoull( buf + 5, nullptr, 16 ); got = true; }
			pclose( p );
		}
		if( printOnly ) { std::printf( "  { \"%s\", 0x%016llxull },\n", c.scene, h ); continue; }
		Check( got, std::string( c.scene ) + " renders (" + c.families + ")" );
		char msg[512];
		std::snprintf( msg, sizeof( msg ), "%s [%s] hash %016llx == parent 4694f511c's %016llx", c.scene, c.families, h, c.parentHash );
		Check( got && h == c.parentHash, msg );
	}
	if( printOnly ) return 0;
	std::printf( "%d passed, %d failed\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
