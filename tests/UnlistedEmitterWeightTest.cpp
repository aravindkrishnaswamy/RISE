//////////////////////////////////////////////////////////////////////
//
//  UnlistedEmitterWeightTest.cpp - DL-633: an emitter hit that light
//    sampling could not have made keeps FULL weight.
//
//    THE BUGS.  A scattered ray that hits an emitter is weighted against
//    the light-sampling strategy that could have produced the same path.
//    Three ways that strategy does not exist were weighted as if it did:
//
//    (1) The light table (LightSampler::Prepare) leaves out a luminary
//        whose average exitance reads zero.  That average is 100
//        stratified samples, so an emitter whose exitance is nonzero
//        only BETWEEN them -- an emission map with sparse lit texels --
//        is dropped while it still emits.  The path tracer substituted
//        a selection probability of 1 for the zero it read back (and
//        zeroed the hit outright under light RIS); BDPT kept phantom
//        light-rooted strategies in its MIS walk; with SMS on the hit
//        was suppressed for a caustic SMS never supplies, since SMS
//        draws its emitter through the same table.
//    (2) Light sampling never selects the luminary it is shading, so a
//        non-convex emitter's light on itself is found only by the ray
//        that returns to the same object.  That hit was weighted with
//        the object's full selection probability (zeroed under RIS).
//    (3) EmissionShaderOp left the selection probability out of its
//        partner density and knew neither (1) nor (2).  It shades an
//        emitter hit for the legacy shader-op chain, and ALSO for the
//        path tracer's subsurface (and RayCaster's volume) continuation
//        whenever the hit object's shader does not name
//        `pathtracing_shaderop`.  The supported continuations are fixed
//        (they mark their rays, RAY_STATE::emitterHitFollowsLightTable);
//        the legacy chain is frozen and its own rays keep their weight
//        (RayCaster's volume continuation is shared and does not), so
//        its rows here are printed and NOT gated, and run only with
//        `--legacy`.
//
//    SECTIONS
//      0. The premise: light sampling alone lights the wall from the
//         "kept" emitter and not at all from the "dropped" one.  If the
//         light table ever keeps the dropped emitter, this fails rather
//         than letting the rest go vacuous.
//      1. A 2x2 emitter behind the camera facing a diffuse wall (the
//         DL-431 rig).  Its exitance is a disc of radius 0.6 cells
//         about every CORNER of the 10x10 sample grid: 95% of the quad
//         emits and every sample reads zero.  The control is the same
//         discs about the cell CENTRES.  Both have a closed form.
//         An emitter with no BSDF takes the HWSS loop's hand-off to the
//         NM body, so the HWSS loop's own emitter-hit block is exercised
//         by the rows whose emitter has a (black) base material.
//      2. Two emitters, one hidden behind the other: the selection
//         probability is a real factor of the visible one's partner
//         density.  The path tracer against a closed form (a control:
//         it was right before), and the legacy chain, printed.
//      3. The dropped emitter reached through a specular caster, SMS on
//         against SMS off: through a glass slab (the Pel/NM gate), and
//         by reflection in a polished plate (the HWSS loop's own gate,
//         and its no-BSDF hand-off when there is no chain record).  The
//         suppression is known to arm in each rig because on the
//         parent commit SMS-on reads 0.0010..0.0014 of SMS-off through
//         the slab and exactly zero by reflection in the plate; nothing
//         in the test proves it.
//      4. An emitter lighting itself: two facing emissive walls as ONE
//         object against the same walls as TWO objects.  The one-object
//         scene has a single light entry; the two-object scene builds
//         the light BVH by default, and from a wall the BVH almost never
//         draws the other wall, so that variant is also a scattered-ray
//         estimate.  The alias-table rows (`light_bvh FALSE`) draw the
//         other wall half the time, a real second estimator.
//      5. The emission op reached by the path tracer: a subsurface wall
//         under the two emitters of section 2, with the emitters'
//         shader naming `DefaultDirectLighting` (emission op) against
//         `DefaultPathTracing` (the integrator's own block).
//    NOT COVERED by a row: the HWSS loop's subsurface hand-off; the
//    HWSS loop's volume walk (its hand-off once passed the previous
//    surface vertex's object, a double count of about +1.2..1.7% on an
//    emissive sphere with a base material in fog, measured by a same-seed
//    A/B because the spectral reference is too noisy to gate it); the
//    marking of RayCaster's volume continuation; a self hit or a dropped
//    emitter reached through the emission op or a path-tracing-shader
//    re-entry; the random-walk continuation after a DL-370 neighbour
//    crossing; the emission op's RIS and solo branches.
//
//    Rows of one section reuse the same salts, so their offsets from
//    the closed form are correlated; each row is a separate site, not a
//    separate draw.
//
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

#include "TestTempDir.h"

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
	char path[1024];
	const std::string scratchFallback = RISE::TestSupport::ScratchBaseDir();
	const char* tmpBase = scratchFallback.c_str();
	std::snprintf( path, sizeof(path), "%s/unlisted_emitter_weight_%s_%d.RISEscene",
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
	std::srand( 9100u + salt );
	SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( 0x91u, salt ) );
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



static const char* kGeometry =
	"film\n{\n\twidth 12\n\theight 12\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.8 0.8 0.8\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_black\n\tcolor 0 0 0\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"lambertian_material\n{\n\tname mat_black\n\treflectance pnt_black\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad\n\tpta -1 -1 0\n\tptb 1 -1 0\n\tptc 1 1 0\n\tptd -1 1 0\n}\n\n"
	"standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_diffuse\n}\n\n";

static const char* kEmitterObject =
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -1.0 1.0 6.0\n\tptb 1.0 1.0 6.0\n\tptc 1.0 -1.0 6.0\n\tptd -1.0 -1.0 6.0\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

// Discs of radius 0.6 cells (squared: 0.36) about the grid CORNERS: no cell
// centre is inside one (a centre is 0.707 cells from its corners).
static const char* kCornerDiscs =
	"expression_painter\n{\n\tname pnt_ex\n\texpr 1-step(0.36,(u*10-floor(u*10+0.5))*(u*10-floor(u*10+0.5))+(v*10-floor(v*10+0.5))*(v*10-floor(v*10+0.5)))\n}\n\n";
// The same discs about the cell CENTRES: every sample reads 1.
static const char* kCentreDiscs =
	"expression_painter\n{\n\tname pnt_ex\n\texpr 1-step(0.36,(u*10-floor(u*10)-0.5)*(u*10-floor(u*10)-0.5)+(v*10-floor(v*10)-0.5)*(v*10-floor(v*10)-0.5))\n}\n\n";

//! The emitter material: `dropped` picks the pattern the light table leaves
//! out; `blackBase` gives it a BSDF (that reflects nothing), which keeps an
//! HWSS path in the HWSS loop at the emitter instead of handing it to the
//! NM body.
static std::string Emitter( const bool dropped, const bool blackBase )
{
	std::string s( dropped ? kCornerDiscs : kCentreDiscs );
	s += "lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_ex\n\tscale 20.0\n\tmaterial ";
	s += blackBase ? "mat_black" : "none";
	s += "\n}\n\n";
	return s;
}

static const char* kPT =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 64\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
// Light sampling ALONE: the legacy rasterizer with only the direct-lighting
// op casts no scattered ray.  (The path tracer's `max_diffuse_bounce 0` does
// not serve: it still traces the BSDF-sampled ray that finds the emitter.)
static const char* kLightSamplingOnly =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
	"pixelpel_rasterizer\n{\n\tmax_recursion 1\n\tsamples 64\n\tlum_samples 1\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
// Light RIS switches itself on with more than 8 lights and no light BVH.
static const char* kPTRIS =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 64\n\toidn_denoise FALSE\n\tpixel_filter box\n\tlight_bvh FALSE\n}\n";
static const char* kBDPT =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"bdpt_pel_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples 64\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
static const char* kVCM =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"vcm_pel_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples 64\n\toidn_denoise FALSE\n"
	"\tpixel_filter box\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n}\n";
#define SPECTRAL "\tnmbegin 380\n\tnmend 720\n\tnum_wavelengths 8\n\tspectral_samples 1\n"
static const char* kPTHWSS =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
	"pathtracing_spectral_rasterizer\n{\n\tsamples 64\n\toidn_denoise FALSE\n\tpixel_filter box\n" SPECTRAL "\thwss true\n}\n";
static const char* kPTHWSSRIS =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
	"pathtracing_spectral_rasterizer\n{\n\tsamples 64\n\toidn_denoise FALSE\n\tpixel_filter box\n" SPECTRAL "\thwss true\n\tlight_bvh FALSE\n}\n";
static const char* kBDPTSpectral =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"bdpt_spectral_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples 64\n" SPECTRAL "\thwss false\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n";

// The legacy shader-op chain: a BSDF-sampled continuation (distribution
// tracing) beside light sampling, weighted against each other by
// EmissionShaderOp.  PRINTED, NOT GATED: the chain is frozen
// (docs/SCENE_CONVENTIONS.md "Legacy tiers") and its rays keep the
// pre-DL-633 weight, which has both defects.
static const char* kLegacy =
	"distributiontracing_shaderop\n{\n\tname dt\n\tsamples 8\n\tdiffuse true\n}\n\n"
	"standard_shader\n{\n\tname global\n\tshaderop dt\n\tshaderop DefaultDirectLighting\n}\n\n"
	"pixelpel_rasterizer\n{\n\tmax_recursion 2\n\tsamples 64\n\tlum_samples 1\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n";

// Dim point lights far behind the wall: they light nothing the camera sees
// and exist only to put entries in the light table.
static std::string DimLights( const int n )
{
	std::string s;
	for( int i = 0; i < n; i++ ) {
		char buf[256];
		std::snprintf( buf, sizeof(buf),
			"omni_light\n{\n\tname dim%d\n\tpower 0.000001\n\tcolor 1 1 1\n\tposition %d 0 -50\n}\n\n", i, i - 4 );
		s += buf;
	}
	return s;
}

static std::string Build( const std::string& emitter, const char* rasterizer, const int dimLights )
{
	std::string s( "RISE ASCII SCENE 7\n" );
	s += kGeometry;
	s += emitter;
	s += kEmitterObject;
	s += "\n";
	s += DimLights( dimLights );
	s += rasterizer;
	return s;
}

// Mean over the frame's pixel centres of the wall radiance
//   L(p) = (0.8/pi) * integral L_e cos_p cos_e / d^2 dA_e,  L_e = 20 * pattern / pi.
// The pattern is symmetric under u -> 1-u, v -> 1-v and u <-> v, so the
// quad's UV orientation does not enter.
static double ClosedForm( const bool corners )
{
	const int W = 12, G = 80;
	const double kScale = 20.0, rho = 0.8, PI_ = 3.14159265358979323846;
	const double half = 3.5 * std::tan( 15.0 * PI_ / 180.0 );
	std::vector<double> lit( size_t( G ) * G );
	for( int b = 0; b < G; b++ ) for( int a = 0; a < G; a++ ) {
		// Coverage of the quadrature cell by the discs, 8x8 sub-samples: a
		// point-sampled disc edge under-reads the emissive area by 0.36%.
		const int S = 8;
		int inside = 0;
		for( int sb = 0; sb < S; sb++ ) for( int sa = 0; sa < S; sa++ ) {
			const double u = ( a + ( sa + 0.5 ) / S ) / G * 10.0, v = ( b + ( sb + 0.5 ) / S ) / G * 10.0;
			const double du = corners ? u - std::floor( u + 0.5 ) : u - std::floor( u ) - 0.5;
			const double dv = corners ? v - std::floor( v + 0.5 ) : v - std::floor( v ) - 0.5;
			if( du * du + dv * dv < 0.36 ) inside++;
		}
		lit[size_t( b ) * G + a] = double( inside ) / ( S * S );
	}
	double acc = 0;
	for( int j = 0; j < W; j++ ) for( int i = 0; i < W; i++ ) {
		const double px = half * ( 2.0 * ( i + 0.5 ) / W - 1.0 );
		const double py = half * ( 2.0 * ( j + 0.5 ) / W - 1.0 );
		double E = 0;
		for( int b = 0; b < G; b++ ) for( int a = 0; a < G; a++ ) {
			if( lit[size_t( b ) * G + a] == 0.0 ) continue;
			const double ex = -1.0 + 2.0 * ( a + 0.5 ) / G;
			const double ey = -1.0 + 2.0 * ( b + 0.5 ) / G;
			const double dx = px - ex, dy = py - ey, dz = 6.0;
			const double d2 = dx * dx + dy * dy + dz * dz;
			const double cs = dz / std::sqrt( d2 );
			E += lit[size_t( b ) * G + a] * ( kScale / PI_ ) * cs * cs / d2 * ( 4.0 / ( double( G ) * G ) );
		}
		acc += rho / PI_ * E;
	}
	return acc / ( W * W );
}

//////////////////////////////////////////////////////////////////////
// Section 2: two uniform 8x8 emitters, the second hidden behind the first.
// Light sampling divides its draws between them, so the selection
// probability is a real factor (below 1) of the visible emitter's partner
// density; the hidden one lights nothing, and the closed form is the first
// emitter's.
//////////////////////////////////////////////////////////////////////
static const char* kTwoEmitters =
	"uniformcolor_painter\n{\n\tname pnt_ex\n\tcolor 1 1 1\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_ex\n\tscale 1.0\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -4.0 4.0 4.0\n\tptb 4.0 4.0 4.0\n\tptc 4.0 -4.0 4.0\n\tptd -4.0 -4.0 4.0\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_hidden\n\tpta -4.0 4.0 5.0\n\tptb 4.0 4.0 5.0\n\tptc 4.0 -4.0 5.0\n\tptd -4.0 -4.0 5.0\n}\n\n"
	"standard_object\n{\n\tname obj_hidden\n\tgeometry quad_hidden\n\tmaterial mat_emit\n}\n\n";

static double ClosedFormTwoEmitters()
{
	const int W = 12, G = 80;
	const double rho = 0.8, PI_ = 3.14159265358979323846;
	const double half = 3.5 * std::tan( 15.0 * PI_ / 180.0 );
	double acc = 0;
	for( int j = 0; j < W; j++ ) for( int i = 0; i < W; i++ ) {
		const double px = half * ( 2.0 * ( i + 0.5 ) / W - 1.0 );
		const double py = half * ( 2.0 * ( j + 0.5 ) / W - 1.0 );
		double E = 0;
		for( int b = 0; b < G; b++ ) for( int a = 0; a < G; a++ ) {
			const double ex = -4.0 + 8.0 * ( a + 0.5 ) / G;
			const double ey = -4.0 + 8.0 * ( b + 0.5 ) / G;
			const double dx = px - ex, dy = py - ey, dz = 4.0;
			const double d2 = dx * dx + dy * dy + dz * dz;
			const double cs = dz / std::sqrt( d2 );
			E += ( 1.0 / PI_ ) * cs * cs / d2 * ( 64.0 / ( double( G ) * G ) );
		}
		acc += rho / PI_ * E;
	}
	return acc / ( W * W );
}

//////////////////////////////////////////////////////////////////////
// Section 3: the dropped emitter reached through a specular caster.  With
// SMS on, the path tracer suppresses an emitter hit that follows a specular
// chain after a diffuse vertex, on the premise that SMS supplies it.  SMS
// draws its emitter through the light table, so it never supplies a dropped
// one: SMS on must render what SMS off does.
//////////////////////////////////////////////////////////////////////

// (a) A glass slab between the wall and the emitter.  The slab has no BSDF,
// so even an HWSS path is handed to the NM body there: this is the Pel/NM
// gate in all three rows.
static const char* kGlassSlab =
	"uniformcolor_painter\n{\n\tname pnt_glass\n\tcolor 1 1 1\n}\n\n"
	"perfectrefractor_material\n{\n\tname mat_glass\n\trefractance pnt_glass\n\tior 1.5\n}\n\n"
	"box_geometry\n{\n\tname slab\n\twidth 6\n\theight 6\n\tdepth 0.3\n}\n\n"
	"standard_object\n{\n\tname obj_slab\n\tgeometry slab\n\tmaterial mat_glass\n\tposition 0 0 4.75\n}\n\n";

// (b) A polished plate behind the camera, facing the wall, and a one-sided
// emitter beside the view that faces the plate: the wall is lit only by the
// plate's mirror reflection of the emitter.  The plate has a BSDF and a
// delta coat, so an HWSS path stays in the HWSS loop through the specular
// bounce and its own SMS guard is the one that closes the gate.
static const char* kPolishedPlate =
	"polished_material\n{\n\tname mat_plate\n\treflectance pnt_black\n\ttau 1.0\n\tior 3.0\n\tscattering 1000000\n}\n\n"
	"clippedplane_geometry\n{\n\tname plate\n\tpta -4.0 4.0 6.0\n\tptb 4.0 4.0 6.0\n\tptc 4.0 -4.0 6.0\n\tptd -4.0 -4.0 6.0\n}\n\n"
	"standard_object\n{\n\tname obj_plate\n\tgeometry plate\n\tmaterial mat_plate\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_side\n\tpta 1.5 -1.0 3.0\n\tptb 3.5 -1.0 3.0\n\tptc 3.5 1.0 3.0\n\tptd 1.5 1.0 3.0\n\tdoublesided FALSE\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_side\n\tmaterial mat_emit\n}\n\n";

// A half-covered card far outside the view.  Alpha coverage anywhere in the
// scene keeps SMS from recording the chain, so the HWSS loop has no record
// to hand its NM body: the hand-off itself must reopen the gate.
static const char* kAlphaCard =
	"lambertian_material\n{\n\tname mat_alpha\n\treflectance pnt_albedo\n\talpha_coverage 0.5\n\talpha_mode blend\n}\n\n"
	"clippedplane_geometry\n{\n\tname card\n\tpta 40 -1 0\n\tptb 42 -1 0\n\tptc 42 1 0\n\tptd 40 1 0\n}\n\n"
	"standard_object\n{\n\tname obj_card\n\tgeometry card\n\tmaterial mat_alpha\n}\n\n";

static std::string SMSRasterizer( const char* kind, const char* extra, const bool sms )
{
	std::string s( "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n" );
	s += kind;
	s += "\n{\n\tsamples 48\n\toidn_denoise FALSE\n\tpixel_filter box\n";
	s += extra;
	s += sms ? "\tsms_enabled TRUE\n" : "\tsms_enabled FALSE\n";
	s += "}\n";
	return s;
}

//////////////////////////////////////////////////////////////////////
// Section 4: an emitter lighting itself.  Two facing 2x2 walls, emissive
// with a diffuse base, 1 apart, seen edge-on by the camera through the gap.
// As ONE object light sampling excludes it (the shading object), so the
// light each wall receives from the other is found only by scattered rays.
// As TWO objects light sampling finds it.  The image must not depend on
// which.
//////////////////////////////////////////////////////////////////////
static const char* kSelfCamera =
	"film\n{\n\twidth 12\n\theight 12\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 4.0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.8 0.8 0.8\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_ex\n\tcolor 1 1 1\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_ex\n\tscale 1.0\n\tmaterial mat_diffuse\n}\n\n";

//! One wall of the pair as mesh text: x = `x`, facing `nx`, single-sided so
//! only the face towards the other wall emits.
static std::string WallMesh( const double x, const int nx, const int firstVertex )
{
	char buf[512];
	std::string s;
	// Counter-clockwise seen from the side the normal points to.
	if( nx > 0 ) {
		std::snprintf( buf, sizeof(buf),
			"\tvertex %g -1 -1\n\tvertex %g 1 -1\n\tvertex %g 1 1\n\tvertex %g -1 1\n", x, x, x, x );
	} else {
		std::snprintf( buf, sizeof(buf),
			"\tvertex %g -1 -1\n\tvertex %g -1 1\n\tvertex %g 1 1\n\tvertex %g 1 -1\n", x, x, x, x );
	}
	s += buf;
	for( int k = 0; k < 4; k++ ) {
		std::snprintf( buf, sizeof(buf), "\tnormal %d 0 0\n", nx );
		s += buf;
	}
	s += "\tuv 0 0\n\tuv 1 0\n\tuv 1 1\n\tuv 0 1\n";
	std::snprintf( buf, sizeof(buf), "\ttriangle %d %d %d\n\ttriangle %d %d %d\n",
		firstVertex, firstVertex + 1, firstVertex + 2, firstVertex, firstVertex + 2, firstVertex + 3 );
	s += buf;
	return s;
}

static std::string SelfScene( const bool oneObject, const char* rasterizer )
{
	std::string s( "RISE ASCII SCENE 7\n" );
	s += kSelfCamera;
	if( oneObject ) {
		// The parser collects each kind of line across the chunk, so the two
		// walls' lines may simply follow one another.
		s += "indexedmesh_geometry\n{\n\tname walls\n\tdouble_sided FALSE\n";
		s += WallMesh( -0.5,  1, 0 );
		s += WallMesh(  0.5, -1, 4 );
		s += "}\n\nstandard_object\n{\n\tname obj_walls\n\tgeometry walls\n\tmaterial mat_emit\n}\n\n";
	} else {
		for( int w = 0; w < 2; w++ ) {
			char name[64];
			std::snprintf( name, sizeof(name), "wall%d", w );
			s += std::string( "indexedmesh_geometry\n{\n\tname " ) + name + "\n\tdouble_sided FALSE\n" +
				WallMesh( w == 0 ? -0.5 : 0.5, w == 0 ? 1 : -1, 0 ) + "}\n\n";
			s += std::string( "standard_object\n{\n\tname obj_" ) + name + "\n\tgeometry " + name + "\n\tmaterial mat_emit\n}\n\n";
		}
	}
	s += rasterizer;
	return s;
}

//////////////////////////////////////////////////////////////////////
// Section 5: the emission op reached by the path tracer.  The wall is a
// subsurface slab; its exit continuation is cast through RayCaster and the
// emitter it reaches is shaded by the emitter object's shader.  With
// `DefaultDirectLighting` that is EmissionShaderOp; with
// `DefaultPathTracing` it is the integrator's own emitter-hit block.  The
// emitters have no BSDF, so nothing else differs between the two.
//////////////////////////////////////////////////////////////////////
static const char* kSubsurfaceWall =
	"film\n{\n\twidth 12\n\theight 12\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
	"subsurfacescattering_material\n{\n\tname mat_sss\n\tior 1.3\n\tabsorption 0.2 0.2 0.2\n\tscattering 4 4 4\n\tg 0.0\n\troughness 0.3\n}\n\n"
	"box_geometry\n{\n\tname slab_sss\n\twidth 2\n\theight 2\n\tdepth 0.5\n}\n\n"
	"standard_object\n{\n\tname obj_wall\n\tgeometry slab_sss\n\tmaterial mat_sss\n\tposition 0 0 -0.25\n}\n\n";

static std::string SubsurfaceScene( const char* shaderOp, const char* rasterizerChunk )
{
	std::string s( "RISE ASCII SCENE 7\n" );
	s += kSubsurfaceWall;
	s += kTwoEmitters;
	s += std::string( "standard_shader\n{\n\tname global\n\tshaderop " ) + shaderOp + "\n}\n\n";
	s += rasterizerChunk;
	return s;
}

struct Row { const char* name; const char* rast; int dimLights; bool rgb; bool blackBase; bool gated; };

int main( int argc, char** argv )
{
	std::cout << "UnlistedEmitterWeightTest\n";
	// The legacy-chain rows only print (the chain is frozen); --legacy adds
	// them back.  The suite skips their ten renders.
	const bool printLegacy = ( argc > 1 && std::string( argv[1] ) == "--legacy" );
	const double truthDropped = ClosedForm( true );
	const double truthKept    = ClosedForm( false );
	std::printf( "  closed form (RGB wall mean): dropped-from-table pattern %.6f, kept pattern %.6f\n",
		truthDropped, truthKept );

	// Section 0.
	{
		const Stat d = SaltedMean( Build( Emitter( true,  false ), kLightSamplingOnly, 0 ), "p0d", 2 );
		const Stat k = SaltedMean( Build( Emitter( false, false ), kLightSamplingOnly, 0 ), "p0k", 2 );
		std::printf( "\n  premise, light sampling alone: dropped %.6f  kept %.6f (kept / closed form = %.4f)\n",
			d.mean, k.mean, k.mean / truthKept );
		Check( d.mean >= 0 && d.mean < 1e-3 * truthDropped, "premise: light sampling cannot reach the dropped emitter" );
		Check( std::fabs( k.mean / truthKept - 1.0 ) < 0.05, "premise: light sampling reaches the kept emitter" );
	}

	// Section 1.
	std::printf( "\n  section 1: a dropped emitter found by a scattered ray\n" );
	static const Row rows[] = {
		{ "PT RGB",                     kPT,           0, true,  false, true },
		{ "PT RGB, light RIS",          kPTRIS,        9, true,  false, true },
		{ "BDPT RGB, other lights",     kBDPT,         9, true,  false, true },
		{ "VCM RGB",                    kVCM,          0, true,  false, true },
		{ "PT HWSS -> NM hand-off",     kPTHWSS,       0, false, false, true },
		{ "PT HWSS body",               kPTHWSS,       0, false, true, true },
		{ "PT HWSS body, light RIS",    kPTHWSSRIS,    9, false, true, true },
		{ "BDPT spectral",              kBDPTSpectral, 0, false, false, true },
		{ "legacy chain RGB",           kLegacy,       0, true,  false, false },
	};
	for( const Row& r : rows ) {
		if( !r.gated && !printLegacy ) continue;
		const Stat u = SaltedMean( Build( Emitter( true,  r.blackBase ), r.rast, r.dimLights ), "u", 2 );
		const Stat k = SaltedMean( Build( Emitter( false, r.blackBase ), r.rast, r.dimLights ), "k", 2 );
		// Each render against its own closed form, then the two against each
		// other: the second is what a spectral row (no RGB closed form) gates.
		const double ratio = k.mean > 1e-9 ? ( u.mean / k.mean ) / ( truthDropped / truthKept ) : -1.0;
		std::printf( "  %-26s dropped %.6f (sd %.6f)  kept %.6f (sd %.6f)  dropped/kept vs closed form = %.4f",
			r.name, u.mean, u.sd, k.mean, k.sd, ratio );
		if( r.rgb ) std::printf( "  dropped/closed = %.4f  kept/closed = %.4f", u.mean / truthDropped, k.mean / truthKept );
		std::printf( "%s\n", r.gated ? "" : "  [frozen: not gated]" );
		if( !r.gated ) continue;
		const std::string label = std::string( r.name );
		Check( k.mean > 1e-6, label + ": the kept emitter lights the wall" );
		Check( ratio > 0.90 && ratio < 1.10, label + ": the dropped emitter lights the wall as the kept one does" );
		if( r.rgb ) {
			Check( std::fabs( k.mean / truthKept - 1.0 ) < 0.06, label + ": the kept emitter matches its closed form" );
			Check( std::fabs( u.mean / truthDropped - 1.0 ) < 0.10, label + ": the dropped emitter matches its closed form" );
		}
	}

	// Section 2.
	const double truthTwo = ClosedFormTwoEmitters();
	std::printf( "\n  section 2: two emitters, one hidden: closed form (RGB wall mean) %.6f\n", truthTwo );
	static const Row rows2[] = {
		{ "PT RGB",            kPT,     0, true, false, true },
		{ "legacy chain RGB",  kLegacy, 0, true, false, false },
	};
	for( const Row& r : rows2 ) {
		if( !r.gated && !printLegacy ) continue;
		std::string s( "RISE ASCII SCENE 7\n" );
		s += kGeometry;
		s += kTwoEmitters;
		s += r.rast;
		const Stat t = SaltedMean( s, "two", 2 );
		std::printf( "  %-26s %.6f (sd %.6f)  / closed form = %.4f%s\n", r.name, t.mean, t.sd, t.mean / truthTwo,
			r.gated ? "" : "  [frozen: not gated]" );
		if( r.gated ) Check( std::fabs( t.mean / truthTwo - 1.0 ) < 0.04,
			std::string( r.name ) + ": two emitters, one hidden, match the closed form" );
	}

	// Section 3.
	std::printf( "\n  section 3: a dropped emitter behind a specular caster, SMS on against SMS off\n" );
	struct SMSRow { const char* name; const char* kind; const char* extra; bool plate; bool blackBase; bool alphaCard; int repeats; };
	static const SMSRow rows3[] = {
		{ "slab, PT RGB",                       "pathtracing_pel_rasterizer",      "",                      false, false, false, 2 },
		{ "plate, PT HWSS body",                "pathtracing_spectral_rasterizer", SPECTRAL "\thwss true\n",  true,  true,  false, 2 },
		{ "plate, PT HWSS hand-off, no record", "pathtracing_spectral_rasterizer", SPECTRAL "\thwss true\n",  true,  false, true,  2 },
	};
	for( const SMSRow& r : rows3 ) {
		Stat on, off;
		for( int pass = 0; pass < 2; pass++ ) {
			std::string s( "RISE ASCII SCENE 7\n" );
			s += kGeometry;
			s += Emitter( true, r.blackBase );
			if( r.plate ) {
				s += kPolishedPlate;
			} else {
				s += kEmitterObject;
				s += "\n";
				s += kGlassSlab;
			}
			if( r.alphaCard ) s += kAlphaCard;
			// One light the table does hold, so that the scene has something
			// for SMS to draw.
			s += DimLights( 1 );
			s += SMSRasterizer( r.kind, r.extra, pass == 0 );
			( pass == 0 ? on : off ) = SaltedMean( s, pass == 0 ? "smson" : "smsoff", r.repeats );
		}
		const double ratio = off.mean > 1e-9 ? on.mean / off.mean : -1.0;
		std::printf( "  %-36s SMS on %.6f (sd %.6f)  SMS off %.6f (sd %.6f)  on/off = %.4f\n",
			r.name, on.mean, on.sd, off.mean, off.sd, ratio );
		Check( off.mean > 1e-3, std::string( r.name ) + ": the dropped emitter lights the wall (SMS off)" );
		Check( ratio > 0.85 && ratio < 1.15, std::string( r.name ) + ": SMS on renders what SMS off does" );
	}

	// Section 4.
	std::printf( "\n  section 4: an emitter lighting itself, one object against two\n" );
	static const Row rows4[] = {
		{ "PT RGB, light RIS",    kPTRIS,     9, true,  false, true },
		{ "PT HWSS, alias table", kPTHWSSRIS, 0, false, false, true },
		{ "BDPT RGB",             kBDPT,      0, true,  false, true },
	};
	for( const Row& r : rows4 ) {
		const Stat one = SaltedMean( SelfScene( true,  r.rast ) + DimLights( r.dimLights ), "one", 2 );
		const Stat two = SaltedMean( SelfScene( false, r.rast ) + DimLights( r.dimLights ), "two", 2 );
		const double ratio = two.mean > 1e-9 ? one.mean / two.mean : -1.0;
		std::printf( "  %-26s one object %.6f (sd %.6f)  two objects %.6f (sd %.6f)  one/two = %.4f\n",
			r.name, one.mean, one.sd, two.mean, two.sd, ratio );
		Check( two.mean > 1e-3, std::string( r.name ) + ": the walls are lit" );
		Check( ratio > 0.94 && ratio < 1.06, std::string( r.name ) + ": one self-lighting object renders as two objects do" );
	}

	// Section 5.
	std::printf( "\n  section 5: a subsurface wall, emitters shaded by the emission op against the path-tracing op\n" );
	struct SSSRow { const char* name; const char* chunk; };
	static const SSSRow rows5[] = {
		{ "PT RGB",      "pathtracing_pel_rasterizer\n{\n\tsamples 64\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n" },
	};
	for( const SSSRow& r : rows5 ) {
		const Stat op = SaltedMean( SubsurfaceScene( "DefaultDirectLighting", r.chunk ), "sssop", 2 );
		const Stat pt = SaltedMean( SubsurfaceScene( "DefaultPathTracing",    r.chunk ), "ssspt", 2 );
		const double ratio = pt.mean > 1e-9 ? op.mean / pt.mean : -1.0;
		std::printf( "  %-26s emission op %.6f (sd %.6f)  path-tracing op %.6f (sd %.6f)  ratio = %.4f\n",
			r.name, op.mean, op.sd, pt.mean, pt.sd, ratio );
		Check( pt.mean > 1e-3, std::string( r.name ) + ": the subsurface wall is lit" );
		Check( ratio > 0.94 && ratio < 1.06, std::string( r.name ) + ": the emission op weights the hit as the integrator does" );
	}

	std::cout << "\n" << passCount << " passed, " << failCount << " failed\n";
	return failCount == 0 ? 0 : 1;
}
