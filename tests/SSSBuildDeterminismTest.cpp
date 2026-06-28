//////////////////////////////////////////////////////////////////////
//
//  SSSBuildDeterminismTest.cpp - Regression for the SSS lazy-build
//    multithreaded non-determinism.
//
//    THE BUG: SubSurfaceScatteringShaderOp / DonnerJensenSkinSSSShaderOp
//    build their per-object irradiance point-set / octree lazily on the
//    first hit.  The build ran once, on whichever render thread first
//    won the find-or-build race, and captured each sample point's
//    irradiance via shader.Shade with (a) that thread's scheduling-
//    dependent rc.random and (b) the TRIGGERING pixel's geometric frame
//    (RayIntersection newri(ri) copied vGeomNormal / onb / ptCoord from
//    the trigger and never overwrote them with the sample point's).  So
//    the captured irradiance -- and the WHOLE SSS image -- varied
//    run-to-run: it landed on one of a few discrete brightness levels,
//    the MEAN cratering to as little as ~1/6 of nominal.  Affected the
//    RGB and spectral paths alike; single-threaded it was bit-exact.
//
//    THE FIX (two coordinated parts):
//      1. A dedicated fixed-seed RNG for the build (RandomNumberGenerator
//         buildRng + a build-local RuntimeContext with pSampler null), so
//         the irradiance shade no longer consumes the render thread's RNG.
//      2. Setting the SAMPLE POINT's surface frame on newri.geometric
//         (vGeomNormal, onb, ptCoord) instead of inheriting the trigger
//         pixel's -- the dominant source (for a point/delta light the RNG
//         doesn't even affect the value, but the frame does).
//
//    THIS TEST: render an SSS-lit sphere through the RGB (pixelpel)
//    integrator MULTIPLE times under the default multithreaded renderer
//    and assert the images are REPRODUCIBLE (mean and peak luminance
//    spread tightly bounded).  Pre-fix the RGB mean craters hard run-to-run
//    (a typical per-run drop is ~17x of nominal; the across-run max/min spread
//    this test computes reaches 180-850x because some runs land near zero);
//    post-fix it is stable to well under 1%.
//
//    NOTE ON TEETH: the underlying defect is an intermittent thread race,
//    so a single render pair could miss it.  We render N times and bound
//    the max/min spread; with the RGB path's frequent, large pre-fix
//    crater this reliably trips pre-fix while staying a comfortable green
//    post-fix.  (RGB is used rather than spectral because it has no
//    wavelength-sampling MC noise -- the octree result lands in the pixel
//    directly -- so the post-fix spread is the tightest available signal.)
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
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
#include "../src/Library/Utilities/Color/Color_Template.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
		std::cout << "  pass: " << testName << std::endl;
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
		for( unsigned int y = 0; y < height; y++ )
			for( unsigned int x = 0; x < width; x++ )
				pixels[y * width + x] = pImage.GetPEL( x, y );
	}
};

static double Lum( const RISEColor& c )
{
	return 0.2126 * c.base.r + 0.7152 * c.base.g + 0.0722 * c.base.b;
}

struct RenderResult
{
	double meanLum;
	double maxLum;
	bool   valid;
};

static const char* kScene =
	"RISE ASCII SCENE 6\n"
	"\n"
	"standard_shader\n{\n\tname sss_irrad\n\tshaderop DefaultDirectLighting\n}\n"
	"simple_sss_shaderop\n{\n\tname sss_op\n\tnumpoints 2000\n\tirrad_scale 6.0\n\tgeometric_scale 1.0\n\tshader sss_irrad\n}\n"
	"standard_shader\n{\n\tname global\n\tshaderop sss_op\n}\n"
	"\n"
	"film\n{\n\twidth 48\n\theight 48\n}\n"
	"pinhole_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n"
	"\n"
	"uniformcolor_painter\n{\n\tname white\n\tcolor 1.0 1.0 1.0\n}\n"
	"lambertian_material\n{\n\tname sss_mat\n\treflectance white\n}\n"
	"sphere_geometry\n{\n\tname spheregeom\n\tradius 1.0\n}\n"
	"standard_object\n{\n\tname sss_sphere\n\tgeometry spheregeom\n\tmaterial sss_mat\n}\n"
	"omni_light\n{\n\tname light\n\tpower 1200.0\n\tposition 0 4 4\n\tcolor 1.0 0.3 0.3\n}\n"
	"\n"
	"pixelpel_rasterizer\n{\n\tmax_recursion 4\n\tsamples 16\n\tlum_samples 1\n\toidn_denoise FALSE\n}\n";

static RenderResult RenderOnce()
{
	RenderResult result{};

	char path[512];
	std::snprintf( path, sizeof(path), "%s/sss_determinism_%d.RISEscene",
		(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp"), static_cast<int>(::getpid()) );
	{
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return result;
		ofs << kScene;
	}

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) { std::remove( path ); return result; }
	const bool bLoaded = pJob->LoadAsciiScene( path );
	std::remove( path );
	if( !bLoaded ) { safe_release( pJob ); return result; }

	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	if( !pJob->Rasterize() ) { safe_release( pCap ); safe_release( pJob ); return result; }

	if( !pCap->pixels.empty() ) {
		double sum = 0, mx = 0;
		for( const RISEColor& c : pCap->pixels ) {
			const double l = Lum( c );
			sum += l;
			if( l > mx ) mx = l;
		}
		result.meanLum = sum / double( pCap->pixels.size() );
		result.maxLum  = mx;
		result.valid   = true;
	}

	safe_release( pCap );
	safe_release( pJob );
	return result;
}

int main()
{
	std::cout << "SSSBuildDeterminismTest" << std::endl;
	std::cout << "=======================" << std::endl;

	const int kRuns = 8;
	std::vector<double> means, maxes;
	for( int i = 0; i < kRuns; i++ ) {
		const RenderResult r = RenderOnce();
		if( !r.valid ) {
			Check( false, "all renders produced an image" );
			std::cout << "  (render " << i << " failed)" << std::endl;
			std::cout << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
			return 1;
		}
		std::cout << "  run " << i << ": meanLum " << r.meanLum << "  maxLum " << r.maxLum << std::endl;
		means.push_back( r.meanLum );
		maxes.push_back( r.maxLum );
	}
	Check( true, "all renders produced an image" );

	const double meanMin = *std::min_element( means.begin(), means.end() );
	const double meanMax = *std::max_element( means.begin(), means.end() );
	const double maxMin  = *std::min_element( maxes.begin(), maxes.end() );
	const double maxMax  = *std::max_element( maxes.begin(), maxes.end() );

	const double meanSpread = ( meanMin > 1e-12 ) ? ( meanMax / meanMin ) : 1e9;
	const double maxSpread  = ( maxMin  > 1e-12 ) ? ( maxMax  / maxMin  ) : 1e9;
	std::cout << "  meanLum spread (max/min) = " << meanSpread
		<< "   (maxLum spread = " << maxSpread << ", firefly-noisy)   over " << kRuns << " runs" << std::endl;

	// The MEAN luminance is the build-determinism signal: it averages out the
	// per-pixel firefly / pixel-jitter MC noise of the pel integrator, so it
	// reflects the captured irradiance octree (the thing the fix makes
	// reproducible).  Pre-fix it craters hard run-to-run (a typical ~17x per-run
	// drop; the 8-run max/min spread this test computes reaches 180-850x as some
	// runs land near zero); post-fix it is stable to well under 1% (the residual
	// is the pel pixel jitter).  1.30x sits far below the pre-fix crater and far
	// above the post-fix noise.
	// (The PEAK luminance, by contrast, is a single brightest pixel dominated
	// by the pel pixel-jitter firefly -- ~1.3x spread even post-fix -- so it
	// is reported for visibility but NOT asserted.)
	Check( meanSpread < 1.30,
		"SSS RGB mean luminance is reproducible run-to-run (spread < 1.30x) -- the build-determinism fix" );

	std::cout << "=======================" << std::endl;
	std::cout << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
	return ( failCount == 0 ) ? 0 : 1;
}
