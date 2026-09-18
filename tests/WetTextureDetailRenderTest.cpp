//////////////////////////////////////////////////////////////////////
//
//  WetTextureDetailRenderTest.cpp - DL-25's own closure recipe, at
//  RENDER level: "a render regression showing the texture detail
//  surviving under the wet recipe".
//
//  DL-25 gave the texture-expression VM `sample(name)` so
//  `add_wetness` could darken a TEXTURED substrate per-texel instead of
//  refusing it.  Every other gate on that arc is a unit assertion about
//  chunk text or about one VM evaluation; none of them renders, and the
//  two things that can go wrong are both only visible in an image:
//
//    (A) THE SAMPLED READ LOSES THE FILTER.  `sample()`'s first
//        implementation evaluated the bound painter at a SYNTHETIC hit
//        record carrying five fields, so `ri.txFootprint.valid` was
//        false and TexturePainter::SampleTextured skipped its whole mip
//        path -- a mip-filtered albedo became an UNFILTERED base-level
//        point sample.  Section (A) renders an IDENTITY
//        `expr sample(tex)` against the same texture bound directly and
//        compares the frames.  Measured against the pre-review dispatch:
//        the frame means are IDENTICAL and the normalized CONTRAST is
//        19.8 % higher through `sample()` -- aliasing, not a brightness
//        shift, which is why this row is judged on contrast.  (DL-25
//        review P1-1; the unit red-proof is TextureExpressionVMTest
//        Test 78 row (a).)
//
//        TWO THINGS MAKE THAT GAP VISIBLE AT ALL, and both were arrived
//        at by measuring a row that passed on a build with the bug in
//        it: the receiver must be geometry whose hit carries a UV
//        JACOBIAN (an analytic sphere -- a `clippedplane_geometry` has a
//        footprint WIDTH and no Jacobian, so both paths point-sample and
//        agree), and the render must be at 1 spp (at 24 spp the pixel
//        filter averages 24 rays across 24 different texels and
//        supersampling hides the whole effect: the contrast gap
//        collapses from 19.8 % to 0.7 %).
//
//    (B) THE DARKENING FLATTENS THE TEXTURE.  A wetness recipe that
//        read one representative colour and multiplied by it would
//        darken correctly ON AVERAGE and destroy the texture.  Section
//        (B) drives the REAL `add_wetness` verb on a textured material
//        and compares the rendered CONTRAST, not just the mean.
//
//        ⚠ SECTION (B) IS A CONSISTENCY PIN, NOT A RED-PROOF, and saying
//        so is the point: it is GREEN against the pre-review synthetic
//        record too, because the emitted recipe is
//        `mix(base, pow(base, k), damp)` whichever record the substrate
//        was read at -- the per-texel structure survives a dropped
//        footprint, only the filtering does.  Section (A) is the
//        red-proof.  What (B) does pin, and nothing else in the arc
//        does, is that the verb's OWN emitted recipe darkens per texel
//        rather than by one representative colour -- the property DL-25's
//        closure recipe asked a render to demonstrate.
//
//  MEASUREMENT HYGIENE (COMMON_RULES.md, docs/skills/variance-
//  measurement.md): `oidn_denoise FALSE` (the default TRUE would have
//  IRasterizerOutput::OutputDenoisedImage forward post-denoise pixels
//  to the capture sink), `pixel_filter box`, and an explicit
//  `std::srand` per render invocation -- RISE seeds from an
//  unsynchronized libc `rand()`, so repeats are not deterministic and
//  every figure here is a mean over n = 3 with its sigma.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IWriteBuffer.h"
#include "../src/Library/Interfaces/IRasterImageWriter.h"
#include "../src/Library/Job.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Log/Log.h"
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
	if( condition ) { passCount++; }
	else { failCount++; std::cout << "  FAIL: " << testName << std::endl; }
}

//! Seeded explicitly per render (see the header note on seeding).
static unsigned int g_renderSeed = 91001u;

//////////////////////////////////////////////////////////////////////
// Capture sink -- same shape as EmitterUVSampleTest / EnvLightBalanceTest.
//////////////////////////////////////////////////////////////////////
class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	unsigned int width;
	unsigned int height;

	CapturingRasterizerOutput() : width(0), height(0) {}

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

//! One render's per-pixel Rec.709 luminance.
struct Frame
{
	std::vector<double> lum;
	unsigned int width = 0;
	unsigned int height = 0;
	bool ok = false;
};

static Frame RenderFrame( const std::string& scenePath )
{
	Frame f;
	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) { return f; }
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { safe_release( pJob ); return f; }

	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	std::srand( g_renderSeed++ );
	const bool bRendered = pJob->Rasterize();
	if( bRendered ) {
		f.width = pCap->width;
		f.height = pCap->height;
		f.lum.reserve( pCap->pixels.size() );
		for( const RISEColor& c : pCap->pixels ) {
			f.lum.push_back( 0.2126 * c.base.r + 0.7152 * c.base.g + 0.0722 * c.base.b );
		}
		f.ok = !f.lum.empty();
	}
	safe_release( pCap );
	safe_release( pJob );
	return f;
}

//! Mean and sample sigma of a small set of per-render statistics.
static void MeanSigma( const std::vector<double>& v, double& mean, double& sigma )
{
	mean = 0; sigma = 0;
	if( v.empty() ) return;
	for( double x : v ) mean += x;
	mean /= double( v.size() );
	if( v.size() < 2 ) return;
	double s2 = 0;
	for( double x : v ) s2 += ( x - mean ) * ( x - mean );
	sigma = std::sqrt( s2 / double( v.size() - 1 ) );
}

//! The two statistics every row below is judged on.  `mean` is the
//! frame's mean luminance (does the wet recipe darken at all?) and
//! `contrast` is the COEFFICIENT OF VARIATION, sigma/mean -- normalized
//! deliberately, so that darkening the whole frame by a constant factor
//! leaves it UNCHANGED and only a loss of texture can move it.  That is
//! exactly the "detail survived" question, expressed as a number that a
//! uniform multiply cannot fake.
struct FrameStats { double mean = 0; double contrast = 0; };

static FrameStats StatsOverLitPixels( const Frame& f )
{
	FrameStats s;
	if( !f.ok ) return s;
	double sum = 0; std::size_t n = 0;
	for( double L : f.lum ) { if( L > 1e-6 ) { sum += L; ++n; } }
	if( n == 0 ) return s;
	s.mean = sum / double( n );
	double s2 = 0;
	for( double L : f.lum ) { if( L > 1e-6 ) { const double d = L - s.mean; s2 += d * d; } }
	const double sd = std::sqrt( s2 / double( n ) );
	s.contrast = ( s.mean > 0 ) ? ( sd / s.mean ) : 0.0;
	return s;
}

static std::string TempPath( const char* tag, const char* ext )
{
	const char* t = getenv( "TMPDIR" );
	std::string dir = ( t && t[0] ) ? t : "/tmp/";
	if( dir[dir.size()-1] != '/' ) dir += '/';
	char buf[512];
	std::snprintf( buf, sizeof(buf), "%swet_texdetail_%s_%d.%s",
		dir.c_str(), tag, static_cast<int>( ::getpid() ), ext );
	return std::string( buf );
}

static std::string WriteScene( const std::string& text, const char* tag )
{
	const std::string path = TempPath( tag, "RISEscene" );
	std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
	f << text;
	f.close();
	return path;
}

//! A one-texel checkerboard of `n` x `n`, written through RISE's own
//! PNG writer.  ONE TEXEL PER CELL is the point, and the SIZE chooses
//! the regime:
//!
//!   n = 512 (section A) is far finer than the sphere's on-screen
//!   resolution, so many texels land in one pixel: a MIP-FILTERED read
//!   converges toward the checker's mean while an UNFILTERED base-level
//!   point sample keeps picking individual black/white texels and reads
//!   as aliasing noise.  That gap is what section A measures.
//!
//!   n = 16 (section B) is coarse enough that every cell covers several
//!   pixels, so the texture is genuinely RESOLVED and "did the
//!   darkening keep it" is a question about the recipe rather than
//!   about filtering.
static std::string WriteCheckerPNG( unsigned int n, const char* tag )
{
	const std::string path = TempPath( tag, "png" );
	IRasterImage* img = 0;
	RISE_API_CreateRISEColorRasterImage( &img, n, n, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
	for( unsigned int y = 0; y < n; ++y ) {
		for( unsigned int x = 0; x < n; ++x ) {
			const Scalar c = ( ( x + y ) & 1 ) ? Scalar( 1 ) : Scalar( 0 );
			img->SetPEL( x, y, RISEColor( RISEPel( c, c, c ), 1.0 ) );
		}
	}
	IWriteBuffer* buf = 0;
	RISE_API_CreateDiskFileWriteBuffer( &buf, path.c_str() );
	IRasterImageWriter* writer = 0;
	RISE_API_CreatePNGWriter( &writer, *buf, 8, eColorSpace_Rec709RGB_Linear );
	img->DumpImage( writer );
	if( writer ) writer->release();
	if( buf )    buf->release();
	if( img )    img->release();
	return path;
}

//! Camera + film + PT rasterizer, shared by both sections.  The
//! `file_rasterizeroutput` exists only because a rasterizer needs one
//! to be constructed; RenderFrame removes it and installs the capture
//! sink, the same way every other render suite in tests/ does.
static std::string Preamble( int width, int height, int samples,
	const char* camLoc, const char* camLookAt, double fov,
	const char* shaderOp = "DefaultPathTracing" )
{
	char buf[1024];
	std::snprintf( buf, sizeof(buf),
		"RISE ASCII SCENE 7\n"
		"\n"
		"film\n{\n\twidth %d\n\theight %d\n}\n"
		"\n"
		"pinhole_camera\n{\n\tlocation %s\n\tlookat %s\n\tup 0 1 0\n\tfov %.1f\n}\n"
		"\n"
		"standard_shader\n{\n\tname global\n\tshaderop %s\n}\n"
		"\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples %d\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n"
		"\n"
		"file_rasterizeroutput\n{\n\tpattern rendered/wet_texdetail_unused\n\ttype EXR\n"
		"\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n"
		"\n",
		width, height, camLoc, camLookAt, fov, shaderOp, samples );
	return std::string( buf );
}

static std::string Light()
{
	return
		"omni_light\n{\n\tname key\n\tcolor 1 1 1\n\tpower 300\n\tposition 0 6 6\n}\n\n";
}

//======================================================================
// (A) The sampled read keeps the texture filter.
//
// One steeply-tilted textured plane, rendered twice: once with the
// material's reflectance bound DIRECTLY to the png_painter, and once
// through an IDENTITY expression, `expr sample(tex)`.  Those two are
// the same function of the hit by construction, so the two frames must
// agree pixel for pixel -- and pre-DL-25-review they did not, because
// the sampled read saw no footprint and point-sampled the base level.
//
// The discriminator is the CONTRAST, not the mean: an unfiltered read
// of a one-texel checker has roughly the right average and far too much
// variance.
//======================================================================
static void TestSampledReadKeepsTheTextureFilter( const std::string& png )
{
	std::cout << "Test A: an identity `sample(tex)` renders the same frame as the texture bound directly (the mip filter survives the sampled read)" << std::endl;

	// A SPHERE, not a plane, and the choice is the whole row: the mip
	// path is gated on `ri.txFootprint.valid`, the UV-JACOBIAN flag, which
	// `TextureFootprintCompute::SolveFootprintUV` sets only where the hit
	// carries surface derivatives AND a texcoord chart.  An analytic
	// sphere does; a `clippedplane_geometry` does NOT (it has a perfectly
	// good footprint WIDTH and no Jacobian to go with it), so on a plane
	// BOTH the direct and the sampled read take the unfiltered path and
	// agree -- the row would pass on a build with the bug in it.  The
	// sphere's UV chart also compresses hard toward its poles and
	// silhouette, which is where many texels land in one pixel and the
	// two paths diverge most.
	const std::string geometry =
		"png_painter\n{\n\tname tex\n\tfile " + png + "\n\tcolor_space Rec709RGB_Linear\n}\n\n"
		"expression_painter\n{\n\tname tex_via_sample\n\texpr sample(tex)\n}\n\n"
		"sphere_geometry\n{\n\tname ground\n\tradius 1.4\n}\n\n";

	// ONE SAMPLE PER PIXEL, and direct lighting only.  This is the other
	// half of making the row discriminating: at 24 spp the pixel filter
	// averages 24 independent camera rays, each landing on a different
	// texel, so SUPERSAMPLING hides an unfiltered texture read almost
	// perfectly (measured: the contrast gap collapses to under 1 %).  At
	// 1 spp there is nothing between the texture fetch and the film, and
	// `DefaultDirectLighting` keeps the rest of the image near
	// deterministic so the contrast statistic is about the texture and
	// not about path-tracing noise.
	const std::string pre = Preamble( 96, 72, 1, "0 0 4.2", "0 0 0", 40.0, "DefaultDirectLighting" );

	const std::string direct = pre + geometry + Light() +
		"lambertian_material\n{\n\tname mat_direct\n\treflectance tex\n}\n\n"
		"standard_object\n{\n\tname obj\n\tgeometry ground\n\tmaterial mat_direct\n\tposition 0 0 0\n}\n";
	const std::string viaSample = pre + geometry + Light() +
		"lambertian_material\n{\n\tname mat_sample\n\treflectance tex_via_sample\n}\n\n"
		"standard_object\n{\n\tname obj\n\tgeometry ground\n\tmaterial mat_sample\n\tposition 0 0 0\n}\n";

	const std::string pDirect = WriteScene( direct, "a_direct" );
	const std::string pSample = WriteScene( viaSample, "a_sample" );

	std::vector<double> meansD, contrD, meansS, contrS;
	for( int i = 0; i < 3; ++i ) {
		const Frame fd = RenderFrame( pDirect );
		const Frame fs = RenderFrame( pSample );
		Check( fd.ok && fs.ok, "A: both renders produced pixels" );
		if( !fd.ok || !fs.ok ) break;
		const FrameStats sd = StatsOverLitPixels( fd );
		const FrameStats ss = StatsOverLitPixels( fs );
		meansD.push_back( sd.mean );  contrD.push_back( sd.contrast );
		meansS.push_back( ss.mean );  contrS.push_back( ss.contrast );
	}

	double md, sdm, mc, sdc, ms, ssm, msc, ssc;
	MeanSigma( meansD, md, sdm );   MeanSigma( contrD, mc, sdc );
	MeanSigma( meansS, ms, ssm );   MeanSigma( contrS, msc, ssc );

	std::cout.precision( 6 );
	std::cout << "    direct       mean " << md << " +/- " << sdm
	          << "   contrast " << mc << " +/- " << sdc << std::endl;
	std::cout << "    sample(tex)  mean " << ms << " +/- " << ssm
	          << "   contrast " << msc << " +/- " << ssc << std::endl;

	Check( md > 0 && ms > 0, "A: both frames are lit" );
	if( md > 0 && ms > 0 ) {
		// The two are the SAME function of the hit, so this is a tight
		// band -- it exists only to absorb the per-render MC noise the
		// sigmas above quantify, not to accommodate a real difference.
		const double meanRatio = ms / md;
		Check( std::fabs( meanRatio - 1.0 ) < 0.02,
		       "A MONEY: mean luminance through `sample(tex)` matches the direct bind within 2% "
		       "(ratio " + std::to_string( meanRatio ) + ")" );
		const double contrastRatio = ( mc > 0 ) ? ( msc / mc ) : 0.0;
		Check( std::fabs( contrastRatio - 1.0 ) < 0.03,
		       "A MONEY: the CONTRAST through `sample(tex)` matches the direct bind within 3% -- an "
		       "unfiltered base-level point sample of a 512-texel checker on a sphere this size reads "
		       "far noisier, so this is the statistic that catches a dropped txFootprint.  Measured "
		       "against the pre-review dispatch (the same library with `sample()` reverted to its "
		       "synthetic five-field record): 1.198, deterministic at 1 spp.  "
		       "(ratio " + std::to_string( contrastRatio ) + ")" );
	}

	std::remove( pDirect.c_str() );
	std::remove( pSample.c_str() );
}

//======================================================================
// (B) `add_wetness` on a textured substrate darkens PER TEXEL.
//
// Drive the REAL verb (AgentSession::AddWetness) on a textured
// ggx_material, render the document it emits against the dry original,
// and ask three questions of the pair:
//
//   1. did it darken at all?
//   2. did the texture survive -- normalized contrast, which a uniform
//      multiply leaves UNCHANGED, so this catches a flattening;
//   3. is the darkening actually PER TEXEL?  This is the one a
//      flattening cannot fake and a uniform multiply cannot pass: the
//      emitted recipe is `mix(base, pow(base, k), damp)` with k > 1,
//      which darkens DARK texels proportionally more than bright ones.
//      So the wet/dry ratio measured over the dry frame's DARKEST
//      quartile must be strictly below the ratio over its BRIGHTEST
//      quartile.  A recipe that read one representative colour and
//      scaled the whole surface by it would give the two quartiles the
//      SAME ratio.
//
// THE GEOMETRY IS A PLANE, NOT A SPHERE, and that is not cosmetic: the
// emitted mask subtracts `ridge = clamp(curv * ridge_shed, 0, 1)`, and
// on a sphere of this size `curv` (mean curvature x the bounding-box
// diagonal) is ~3.5, so `ridge` saturates at 1, `damp` clamps to 0, and
// the wet render is byte-for-byte the dry one -- the row would pass
// vacuously.  Ridges shedding water is the recipe working as designed;
// a flat receiver is where wetness actually lands.
//======================================================================
static void TestWetnessDarkensPerTexel( const std::string& png )
{
	std::cout << "Test B: add_wetness on a textured substrate darkens PER TEXEL -- the texture's contrast survives and dark texels darken more than bright ones" << std::endl;

	const std::string dry =
		Preamble( 96, 72, 24, "0 0.55 2.2", "0 0 -6", 45.0 ) +
		"png_painter\n{\n\tname tex\n\tfile " + png + "\n\tcolor_space Rec709RGB_Linear\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_spec\n\tcolor 0.04 0.04 0.04\n}\n\n"
		+ Light() +
		// `fresnel_mode schlick_f0` is load-bearing, not decoration: the
		// ggx_material descriptor's DEFAULT is `conductor`, which
		// `add_wetness` reads as metallic and then skips the whole
		// darkening half (§6.4 clause 1) -- leaving a "wet" document that
		// renders identically to the dry one and a row that proves nothing.
		"ggx_material\n{\n\tname mat_tex\n\trd tex\n\trs pnt_spec\n\talphax 0.25\n\talphay 0.25\n"
		"\tior 1.50\n\textinction 0.0\n\tfresnel_mode schlick_f0\n}\n\n"
		"clippedplane_geometry\n{\n\tname ground\n"
		"\tpta -4 0 -14\n\tptb 4 0 -14\n\tptc 4 0 1\n\tptd -4 0 1\n}\n\n"
		"standard_object\n{\n\tname obj\n\tgeometry ground\n\tmaterial mat_tex\n}\n";

	const std::string pDry = WriteScene( dry, "b_dry" );

	// The verb runs on its own Job; the document it produces is what we
	// render.  Deliberately the REAL verb rather than a hand-written
	// "wet" scene: the point of the row is that what add_wetness actually
	// emits keeps the texture.
	std::string wetText;
	{
		Job* pJob = new Job(); pJob->addref();
		const bool loaded = pJob->LoadAsciiSceneViaCst( pDry.c_str() );
		Check( loaded, "B: the dry fixture derives" );
		if( loaded ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness( "mat_tex" );
			Check( r.ok && r.applied, std::string( "B: add_wetness applied -- " ) + r.message );
			Check( r.texturedAlbedo, "B: ...on the TEXTURED branch (the one DL-25 opened)" );
			Check( !r.hasBaseColor,
			       "B: ...and it reports no literal base colour (review P1-2), because there is none" );
			if( r.ok && r.applied ) { wetText = sess->ReadDocument(); }
			sess.reset();
		}
		pJob->release();
	}
	Check( !wetText.empty(), "B: the verb produced a document to render" );
	Check( wetText.find( "sample(tex)" ) != std::string::npos,
	       "B: the emitted recipe really reads the substrate through `sample(tex)`" );
	if( wetText.empty() ) { std::remove( pDry.c_str() ); return; }

	const std::string pWet = WriteScene( wetText, "b_wet" );

	std::vector<double> meansDry, contrDry, meansWet, contrWet, darkRatios, brightRatios;
	for( int i = 0; i < 3; ++i ) {
		const Frame fd = RenderFrame( pDry );
		const Frame fw = RenderFrame( pWet );
		Check( fd.ok && fw.ok, "B: both renders produced pixels" );
		if( !fd.ok || !fw.ok ) break;
		Check( fd.lum.size() == fw.lum.size(), "B: the two frames are the same size" );
		const FrameStats sd = StatsOverLitPixels( fd );
		const FrameStats sw = StatsOverLitPixels( fw );
		meansDry.push_back( sd.mean );  contrDry.push_back( sd.contrast );
		meansWet.push_back( sw.mean );  contrWet.push_back( sw.contrast );

		// Quartile split, keyed on the DRY frame's own luminance so the
		// two bands are the same PIXELS in both renders.
		std::vector<double> sorted;
		for( double L : fd.lum ) if( L > 1e-6 ) sorted.push_back( L );
		if( sorted.size() < 8 ) continue;
		std::sort( sorted.begin(), sorted.end() );
		const double q1 = sorted[ sorted.size() / 4 ];
		const double q3 = sorted[ ( 3 * sorted.size() ) / 4 ];
		double dDark = 0, wDark = 0, dBright = 0, wBright = 0;
		std::size_t nDark = 0, nBright = 0;
		for( std::size_t p = 0; p < fd.lum.size(); ++p ) {
			const double L = fd.lum[p];
			if( L <= 1e-6 ) continue;
			if( L <= q1 )      { dDark   += L; wDark   += fw.lum[p]; ++nDark; }
			else if( L >= q3 ) { dBright += L; wBright += fw.lum[p]; ++nBright; }
		}
		if( nDark > 0 && nBright > 0 && dDark > 0 && dBright > 0 ) {
			darkRatios.push_back( wDark / dDark );
			brightRatios.push_back( wBright / dBright );
		}
	}

	double md, sdm, mc, sdc, mw, swm, mwc, swc, mdr, sdr, mbr, sbr;
	MeanSigma( meansDry, md, sdm );      MeanSigma( contrDry, mc, sdc );
	MeanSigma( meansWet, mw, swm );      MeanSigma( contrWet, mwc, swc );
	MeanSigma( darkRatios, mdr, sdr );   MeanSigma( brightRatios, mbr, sbr );

	std::cout.precision( 6 );
	std::cout << "    dry  mean " << md << " +/- " << sdm << "   contrast " << mc << " +/- " << sdc << std::endl;
	std::cout << "    wet  mean " << mw << " +/- " << swm << "   contrast " << mwc << " +/- " << swc << std::endl;
	if( md > 0 && mc > 0 ) {
		std::cout << "    wet/dry  mean " << ( mw / md ) << "   contrast " << ( mwc / mc ) << std::endl;
	}
	std::cout << "    wet/dry over the DRY frame's darkest quartile  " << mdr << " +/- " << sdr << std::endl;
	std::cout << "    wet/dry over the DRY frame's brightest quartile " << mbr << " +/- " << sbr << std::endl;

	Check( md > 0 && mw > 0, "B: both frames are lit" );
	if( md > 0 && mw > 0 && mc > 0 ) {
		Check( mw < md * 0.97,
		       "B: the wet render is meaningfully DARKER than the dry one (mean " + std::to_string( mw ) +
		       " vs " + std::to_string( md ) + ") -- a `damp` mask that clamped to 0 would leave them equal" );
		// A BAND, not a point: the emitted darkening is masked by
		// curv/occlusion/fbm and is nonlinear in the base, so the exact
		// factor is scene-dependent by design.  What is NOT
		// scene-dependent is that a per-texel darkening cannot collapse
		// the texture -- a recipe that read one representative colour and
		// multiplied by it would leave this ratio at 1 while sending the
		// quartile check below to 1 as well, and one that flattened the
		// substrate would send this toward 0.
		const double contrastRatio = mwc / mc;
		Check( contrastRatio > 0.70 && contrastRatio < 1.80,
		       "B: the wet render's normalized contrast stays within [0.70, 1.80] of the dry one's -- "
		       "the texture survived the darkening rather than being flattened into a constant "
		       "(ratio " + std::to_string( contrastRatio ) + ")" );
	}
	Check( !darkRatios.empty() && !brightRatios.empty(), "B: the quartile split found both bands" );
	if( !darkRatios.empty() && !brightRatios.empty() ) {
		Check( mdr < mbr - 0.02,
		       "B MONEY: the darkening is PER TEXEL -- wet/dry over the dry frame's darkest quartile (" +
		       std::to_string( mdr ) + ") is below the brightest quartile's (" + std::to_string( mbr ) +
		       "), which is what `mix(base, pow(base, k>1), damp)` does to a texture and what a single "
		       "representative colour scaled across the whole surface could not" );
	}

	std::remove( pDry.c_str() );
	std::remove( pWet.c_str() );
}

int main( int, char** )
{
	std::cout << "WetTextureDetailRenderTest -- DL-25: texture detail survives `sample()` and the add_wetness recipe" << std::endl << std::endl;

	const std::string pngFine   = WriteCheckerPNG( 512, "checker512" );
	const std::string pngCoarse = WriteCheckerPNG( 16,  "checker16" );

	TestSampledReadKeepsTheTextureFilter( pngFine );
	std::cout << std::endl;
	TestWetnessDarkensPerTexel( pngCoarse );

	std::remove( pngFine.c_str() );
	std::remove( pngCoarse.c_str() );

	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
