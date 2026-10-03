//////////////////////////////////////////////////////////////////////
//
//  EmitterWorldPositionTest.cpp - DL-298 regression: every record a
//    light evaluation builds BY HAND at a SAMPLED point must carry the
//    physical world position `P` (the expression VM's `P`,
//    RayIntersectionGeometric::ptIntersection), exactly as the record a
//    camera ray's real intersection would.
//
//    THE BUG: `LightSampler::SampleLight`'s own RGB emitted-radiance
//    record, PT's RGB and NM mesh NEE arms (`EvaluateDirectLighting{,NM}`)
//    and the legacy RGB / spectral / SMS photon-direction samplers left
//    `ptIntersection` at its default-constructed (0,0,0), while BDPT's NM
//    hero / HWSS companion rebuilds and VCM's NEE rebuild supplied it --
//    so the input contract differed per path, and a luminaire whose
//    exitance or Phong exponent is a world-position expression read the
//    ORIGIN at a sampled nonzero point under exactly the paths that did
//    not.  Fixed by one shared `LightSampler::FillEmitterRecord` that
//    every one of those sites calls.
//
//    THE SCENE: the DL-44 wall-and-camera rig (a 2x2 diffuse wall at z=0
//    seen by a pinhole camera at z=3.5; a 2x2 emitter quad at z=6, BEHIND
//    the camera, facing the wall; single-bounce direct light, so the wall
//    is lit ONLY through the light-sampling strategy).  The emitter's
//    exitance is
//        M(P) = 0.25 + 1.125 (P.x^2 + P.y^2)   (area average exactly 1.0)
//    keyed on WORLD POSITION in one scene and on the emitter's own UV
//    ((2u-1)^2 + (2v-1)^2, the SAME field -- a clipped plane's UV is the
//    unit square over its x,y extent, and the field is invariant under
//    its sign/axis conventions) in a control scene.  A UV key is read
//    correctly by every path since DL-44, so the control is the
//    reference-free oracle for the position key; a deterministic
//    quadrature of the closed form gives an independent absolute check.
//    At the origin M = 0.25 (a quarter of the area average 1.0), so the
//    unfixed paths render the P-keyed wall ~4x too dark.  (The field is nonzero at the
//    origin so this test isolates DL-298 alone: the emitter's own `averageRadiantExitance`
//    was estimated at P = 0 until DL-431, and a field that vanishes there was never
//    sampled -- EmitterAverageExitanceTest covers that.)
//
//    A second topology keys a PHONG exponent N(P) = 2 + 3(P.x^2+P.y^2)
//    the same way (emitted radiance = (N+1) cos^N / (2 pi)), which is
//    what the legacy photon-direction samplers read: AlphaPhotonEmission-
//    Test pins the photon records directly.
//
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#include <algorithm>
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
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Utilities/Color/Color_Template.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	unsigned int width, height;
	CapturingRasterizerOutput() : width( 0 ), height( 0 ) {}
protected:
	virtual ~CapturingRasterizerOutput() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage& pImage, const Rect*, const unsigned int ) override
	{
		width = pImage.GetWidth();
		height = pImage.GetHeight();
		pixels.resize( width * height );
		for( unsigned int y = 0; y < height; y++ ) {
			for( unsigned int x = 0; x < width; x++ ) {
				pixels[y * width + x] = pImage.GetPEL( x, y );
			}
		}
	}
};

static double MeanLuminance( const CapturingRasterizerOutput& cap )
{
	if( cap.pixels.empty() ) return -1.0;
	double sum = 0;
	for( const RISEColor& c : cap.pixels ) {
		sum += 0.2126 * c.base.r + 0.7152 * c.base.g + 0.0722 * c.base.b;
	}
	return sum / double( cap.pixels.size() );
}

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	char path[512];
	const char* tmpBase = std::getenv( "TMPDIR" );
	if( !tmpBase || !*tmpBase ) tmpBase = "/tmp";
	std::snprintf( path, sizeof(path), "%s/emitter_world_position_%s_%d.RISEscene",
		tmpBase, tag, static_cast<int>( ::getpid() ) );
	std::ofstream ofs( path );
	if( !ofs.is_open() ) return std::string();
	ofs << sceneText;
	return std::string( path );
}

static double RenderMean( const char* scenePath, unsigned salt )
{
	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) return -1.0;
	if( !pJob->LoadAsciiSceneViaCst( scenePath ) ) {
		safe_release( pJob );
		return -1.0;
	}
	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );
	std::srand( 298000u + salt );
	SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( 0x298u, salt ) );
	const bool bRendered = pJob->Rasterize();
	SobolSamplerTestHooks::ValueSalt().store( 0u );
	const double mean = bRendered ? MeanLuminance( *pCap ) : -1.0;
	safe_release( pCap );
	safe_release( pJob );
	return mean;
}

struct Stat { double mean, sd; int n; };

//! Salted repeats (unsalted repeats of a Sobol' render reuse identical points).
static Stat SaltedMean( const std::string& scene, const char* tag, int n )
{
	const std::string path = WriteSceneToTempFile( scene, tag );
	Stat s = { -1.0, 0.0, 0 };
	if( path.empty() ) return s;
	double sum = 0, sumSq = 0;
	for( int i = 0; i < n; i++ ) {
		const double m = RenderMean( path.c_str(), unsigned( i ) + 1u );
		if( m < 0 ) { std::remove( path.c_str() ); return s; }
		sum += m;  sumSq += m * m;
	}
	std::remove( path.c_str() );
	s.n = n;
	s.mean = sum / n;
	s.sd = n > 1 ? std::sqrt( std::max( 0.0, ( sumSq - n * s.mean * s.mean ) / ( n - 1 ) ) ) : 0.0;
	return s;
}

//////////////////////////////////////////////////////////////////////
// Scene text.
//////////////////////////////////////////////////////////////////////
static const char* kGeometry =
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.8 0.8 0.8\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad\n\tpta -1 -1 0\n\tptb 1 -1 0\n\tptc 1 1 0\n\tptd -1 1 0\n}\n\n"
	"standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_diffuse\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_white\n\tcolor 1.0 1.0 1.0\n}\n";

static const char* kEmitterObject =
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -1.0 1.0 6.0\n\tptb 1.0 1.0 6.0\n\tptc 1.0 -1.0 6.0\n\tptd -1.0 -1.0 6.0\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

// Lambertian luminaire, exitance keyed on P or on the equivalent UV field.
static const char* kLambertP =
	"expression_painter\n{\n\tname pnt_ex\n\texpr 0.25+1.125*(P.x*P.x+P.y*P.y)\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_ex\n\tscale 10.0\n\tmaterial none\n}\n\n";
static const char* kLambertUV =
	"expression_painter\n{\n\tname pnt_ex\n\texpr 0.25+1.125*((2.0*u-1.0)*(2.0*u-1.0)+(2.0*v-1.0)*(2.0*v-1.0))\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_ex\n\tscale 10.0\n\tmaterial none\n}\n\n";
// Phong luminaire (exitance 1), exponent keyed on P or on the UV field.
static const char* kPhongP =
	"scalar_painter\n{\n\tname pnt_n\n\texpression 2.0+3.0*(P.x*P.x+P.y*P.y)\n}\n\n"
	"phong_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_white\n\tN pnt_n\n\tscale 10.0\n\tmaterial none\n}\n\n";
static const char* kPhongUV =
	"scalar_painter\n{\n\tname pnt_n\n\texpression 2.0+3.0*((2.0*u-1.0)*(2.0*u-1.0)+(2.0*v-1.0)*(2.0*v-1.0))\n}\n\n"
	"phong_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_white\n\tN pnt_n\n\tscale 10.0\n\tmaterial none\n}\n\n";

static const char* kPT =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 256\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
static const char* kBDPT =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"bdpt_pel_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples 256\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
static const char* kVCM =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"vcm_pel_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples 256\n\toidn_denoise FALSE\n"
	"\tpixel_filter box\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n}\n";
static const char* kPTSpectral =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
	"pathtracing_spectral_rasterizer\n{\n\tsamples 512\n\toidn_denoise FALSE\n\tpixel_filter box\n\tnmbegin 380\n\tnmend 720\n"
	"\tnum_wavelengths 8\n\tspectral_samples 1\n\thwss false\n\tmax_diffuse_bounce 3\n}\n";
static const char* kPTSpectralHWSS =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
	"pathtracing_spectral_rasterizer\n{\n\tsamples 256\n\toidn_denoise FALSE\n\tpixel_filter box\n\tnmbegin 380\n\tnmend 720\n"
	"\tnum_wavelengths 8\n\tspectral_samples 1\n\thwss true\n\tmax_diffuse_bounce 3\n}\n";
static const char* kBDPTSpectral =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"bdpt_spectral_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples 512\n\tnmbegin 380\n\tnmend 720\n"
	"\tnum_wavelengths 8\n\tspectral_samples 1\n\thwss false\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n";

static std::string Build( const char* emitter, const char* rasterizer )
{
	std::string s( "RISE ASCII SCENE 7\n" );
	s += kGeometry;
	s += "\n";
	s += emitter;
	s += kEmitterObject;
	s += rasterizer;
	return s;
}

//////////////////////////////////////////////////////////////////////
// Closed form: mean over the frame's pixel centres of the wall radiance
//   L(p) = (0.8/pi) * integral L_e(e -> p) cos_p cos_e / d^2 dA_e
// over the z=6 emitter [-1,1]^2 (cos_p = cos_e = 6/d).
//////////////////////////////////////////////////////////////////////
static double ClosedForm( bool phong )
{
	const int W = 24, G = 160;
	const double kScale = 10.0, rho = 0.8, PI_ = 3.14159265358979323846;
	const double half = 3.5 * std::tan( 15.0 * PI_ / 180.0 );
	double acc = 0;
	for( int j = 0; j < W; j++ ) for( int i = 0; i < W; i++ ) {
		const double px = half * ( 2.0 * ( i + 0.5 ) / W - 1.0 );
		const double py = half * ( 2.0 * ( j + 0.5 ) / W - 1.0 );
		double E = 0;
		for( int b = 0; b < G; b++ ) for( int a = 0; a < G; a++ ) {
			const double ex = -1.0 + 2.0 * ( a + 0.5 ) / G;
			const double ey = -1.0 + 2.0 * ( b + 0.5 ) / G;
			const double dx = px - ex, dy = py - ey, dz = 6.0;
			const double d2 = dx * dx + dy * dy + dz * dz;
			const double cs = dz / std::sqrt( d2 );
			const double r2 = ex * ex + ey * ey;
			double Le;
			if( phong ) {
				const double N = 2.0 + 3.0 * r2;
				Le = kScale * ( N + 1.0 ) * std::pow( cs, N ) / ( 2.0 * PI_ );
			} else {
				Le = kScale * ( 0.25 + 1.125 * r2 ) / PI_;
			}
			E += Le * cs * cs / d2 * ( 4.0 / ( G * G ) );
		}
		acc += rho / PI_ * E;
	}
	return acc / ( W * W );
}

struct Row { const char* name; const char* rast; };

static void RunTopology( const char* title, bool phong, const char* emitP, const char* emitUV )
{
	std::cout << title << "\n";
	const double truth = ClosedForm( phong );
	std::printf( "  closed form (RGB wall mean) = %.6f\n", truth );
	static const Row rows[] = {
		{ "PT RGB",            kPT },
		{ "BDPT RGB",          kBDPT },
		{ "VCM RGB",           kVCM },
		{ "PT spectral",       kPTSpectral },
		{ "PT spectral HWSS",  kPTSpectralHWSS },
		{ "BDPT spectral",     kBDPTSpectral },
	};
	for( const Row& r : rows ) {
		const bool rgb = std::string( r.name ).find( "RGB" ) != std::string::npos;
		const Stat p = SaltedMean( Build( emitP,  r.rast ), "p",  4 );
		const Stat c = SaltedMean( Build( emitUV, r.rast ), "uv", 4 );
		const double ratio = c.mean > 1e-9 ? p.mean / c.mean : -1.0;
		std::printf( "  %-17s P-keyed %.6f (sd %.6f)  UV control %.6f (sd %.6f)  P/UV = %.4f",
			r.name, p.mean, p.sd, c.mean, c.sd, ratio );
		if( rgb ) std::printf( "  P/closed = %.4f  UV/closed = %.4f", p.mean / truth, c.mean / truth );
		std::printf( "\n" );
		const std::string label = std::string( title ) + " / " + r.name;
		Check( c.mean > 1e-6, label + ": UV control is lit" );
		Check( p.mean > 1e-6, label + ": P-keyed emitter is lit (not read at the origin)" );
		if( c.mean > 1e-6 ) {
			Check( ratio > 0.97 && ratio < 1.03, label + ": P-keyed agrees with the UV-keyed control" );
		}
		if( rgb ) {
			Check( std::fabs( p.mean / truth - 1.0 ) < 0.05, label + ": P-keyed matches the closed form" );
		}
	}
}

int main()
{
	std::cout << "=== EmitterWorldPositionTest (DL-298) ===\n";
	RunTopology( "Lambertian luminaire, exitance M(P) = 0.25 + 1.125 (P.x^2 + P.y^2)", false, kLambertP, kLambertUV );
	RunTopology( "Phong luminaire, exponent N(P) = 2 + 3 (P.x^2 + P.y^2)",     true,  kPhongP,   kPhongUV );
	std::cout << "\nPassed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
