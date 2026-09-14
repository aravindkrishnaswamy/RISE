//////////////////////////////////////////////////////////////////////
//
//  PiecewiseLinearScalarPainterRGBTintTest.cpp - render regression for
//    DL-29 (docs/DEBT_LEDGER.md): a `tau` transmittance curve bound to
//    a `scalar_painter { file ... }` (PiecewiseLinearScalarPainter)
//    must produce a genuinely TINTED transmitted colour under RGB
//    rendering, not the pre-fix grey broadcast of a single 555nm
//    sample.
//
//    SCENE.  A dielectric slab (uniform ior 1.5, no dispersion -- this
//    row is about `tau`, not `ior`) whose `tau` is a 2-column file:
//        380 0.05     (blue heavily absorbed)
//        720 0.95     (red mostly transmitted)
//    Camera looks straight through the slab at a bright white
//    Lambertian luminaire behind it. `dielectric_material`'s
//    DoSingleRGBComponent multiplies tau^distance per channel
//    (Beer's law), so the slab thickness amplifies whatever tint
//    `GetValuesAt` reports for the curve -- pre-fix that tint was
//    exactly (v,v,v) for the 555nm sample (visually grey slab);
//    post-fix it is a genuine per-channel triple.
//
//    ASSERTION.  Rather than a closed form (Fresnel + Beer's law +
//    the CMF-integrated RGB triple compound in a way not worth
//    re-deriving here), this pins the qualitative claim the ledger
//    row states directly: the transmitted pixel's R channel reads
//    substantially brighter than its B channel (R/B ratio well above
//    what MC noise or a merely-grey slab could produce), and neither
//    a grey (R==G==B) reading nor a red-per-channel COLLAPSE (a
//    regression that clamped/dropped channels) would pass.
//
//  Author: Claude (debt-precision slice, DL-29)
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
	unsigned int width;
	unsigned int height;

	CapturingRasterizerOutput() : width(0), height(0) {}

protected:
	virtual ~CapturingRasterizerOutput() {}

public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

	virtual void OutputImage(
		const IRasterImage& pImage,
		const Rect*,
		const unsigned int ) override
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

static std::string WriteToTempFile( const std::string& text, const char* suffix )
{
	char path[512];
	std::snprintf( path, sizeof(path), "/tmp/pwl_tint_%s_%d", suffix, static_cast<int>(::getpid()) );
	std::ofstream ofs( path );
	if( !ofs.is_open() ) return std::string();
	ofs << text;
	ofs.close();
	return std::string( path );
}

int main()
{
	std::cout << "=== PiecewiseLinearScalarPainterRGBTintTest ===" << std::endl;

	// The tau curve file: strongly red-transmitting, blue-absorbing.
	const std::string tauPath = WriteToTempFile( "380 0.05\n720 0.95\n", "tau.txt" );
	Check( !tauPath.empty(), "tau curve file written" );

	const std::string scene =
		std::string( "RISE ASCII SCENE 7\n" ) +
		"film\n{\n\twidth 24\n\theight 24\n}\n\n"
		"pinhole_camera\n{\n"
		"\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 10\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"lambertian_luminaire_material\n{\n"
		"\tname mat_emit\n\texitance pnt_emit\n\tscale 1.0\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname quad\n"
		"\tpta -1 1 -2\n\tptb 1 1 -2\n\tptc 1 -1 -2\n\tptd -1 -1 -2\n}\n\n"
		"standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_emit\n}\n\n"
		"scalar_painter\n{\n\tname tau_tinted\n\tfile " + tauPath + "\n}\n\n"
		"dielectric_material\n{\n\tname mat_slab\n\tior 1.5\n"
		"\ttau tau_tinted\n\tscattering 1000000\n}\n\n"
		"box_geometry\n{\n\tname geo_slab\n\twidth 4\n\theight 4\n\tdepth 2\n}\n\n"
		"standard_object\n{\n\tname slab\n\tgeometry geo_slab\n"
		"\tmaterial mat_slab\n\tposition 0 0 0\n}\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 64\n"
		"\toidn_denoise FALSE\n\tpixel_filter box\n}\n";

	const std::string scenePath = WriteToTempFile( scene, "scene.RISEscene" );
	Check( !scenePath.empty(), "scene file written" );

	IJobPriv* pJob = nullptr;
	Check( RISE_CreateJobPriv( &pJob ) && pJob, "job created" );
	if( pJob ) {
		const bool loaded = pJob->LoadAsciiSceneViaCst( scenePath.c_str() );
		Check( loaded, "scene loads" );
		if( loaded ) {
			pJob->RemoveRasterizerOutputs();
			CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
			GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
			pJob->GetRasterizer()->AddRasterizerOutput( pCap );

			std::srand( 1000u );
			const bool rendered = pJob->Rasterize();
			Check( rendered, "render produced output" );
			if( rendered && !pCap->pixels.empty() ) {
				double meanR = 0, meanG = 0, meanB = 0;
				for( const RISEColor& c : pCap->pixels ) {
					const double cov = c.a;
					meanR += c.base.r * cov;
					meanG += c.base.g * cov;
					meanB += c.base.b * cov;
				}
				const double n = double( pCap->pixels.size() );
				meanR /= n; meanG /= n; meanB /= n;

				std::cout << "    mean R=" << meanR << " G=" << meanG << " B=" << meanB
				          << "  R/B=" << ( meanB > 1e-9 ? meanR / meanB : -1.0 ) << std::endl;

				Check( meanR > 1e-6, "transmitted signal is non-trivial" );
				// "Tinted, not grey": R must read substantially brighter
				// than B.  The pre-fix bug broadcasts ONE 555nm sample
				// (a value between the curve's 0.05/0.95 endpoints) into
				// every channel, giving R == G == B exactly (up to MC
				// noise); this threshold (2x) is far above any noise
				// floor a 64-spp/24x24 PT render produces on a bright,
				// unoccluded direct-luminaire path.
				Check( meanR > 2.0 * meanB,
					"R substantially brighter than B (tinted, not grey)" );
				// Sanity against a channel-collapse regression: G must
				// sit between R and B (the curve is monotonic in tau),
				// not near zero or near R.
				Check( meanG > meanB && meanG < meanR,
					"G sits between R and B (monotonic tau curve)" );
			}
			safe_release( pCap );
		}
		safe_release( pJob );
	}

	std::remove( tauPath.c_str() );
	std::remove( scenePath.c_str() );

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
