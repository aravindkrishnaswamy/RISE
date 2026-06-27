//////////////////////////////////////////////////////////////////////
//  CstRenderPixelEquivalenceTest.cpp -- the Phase-B/0b RENDER spot-check.
//  The structural oracle (CstCorpusEquivalenceTest / DumpJob) verifies the CST derives the same scene
//  STRUCTURE + the cheaply+deterministically readable values (geometry, objects, lights, media, radiance
//  maps, film, cameras, emitter exitance). The values it still cannot reach -- material IOR (an SPF scalar),
//  camera intrinsics (sensor/focal/fstop, not on ICamera) -- are covered "by construction" (same Finalize,
//  same ParamValue capture). This test CONFIRMS that empirically: it builds the legacy-parse Job and the
//  CST-derive Job for the SAME scene and compares mean luminance -- a wrong IOR (refraction) or a wrong DOF
//  intrinsic moves the image, so a render match is direct evidence the by-construction values derive right.
//  (Accelerator is render-neutral / perf-only; RR threshold is variance-only / mean-neutral -- both invisible
//  to a mean-luminance compare, correctly.)
//
//  SUITE-SAFE: bare run SKIPS (renders are slow + MC-non-deterministic -> flaky in a suite; the structural
//  gate is the suite guard). Run with scene path(s) for the compare:
//      ./bin/tests/CstRenderPixelEquivalenceTest scenes/Tests/SMS/sms_slab_close_nosms.RISEscene ...
//////////////////////////////////////////////////////////////////////
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include "CstRenderEquivalence.h"                         // ParseLegacy, Job, namespace risequiv
#include "../src/Library/Cst/Cst.h"                       // ParseToCst, DeriveToJob, Document
#include "../tools/CstMigrator.h"                         // Migrate, ReadFile
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/Reference.h"           // Reference, GlobalLog, safe_release

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::Cst;
using namespace risequiv;

// In-memory capturing output (no EXR round-trip) -- same pattern as AutoRasterizerTest.
class Cap : public virtual IRasterizerOutput, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	unsigned int width = 0, height = 0;
	Cap() {}
protected:
	virtual ~Cap() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage& im, const Rect*, const unsigned int ) override
	{
		width = im.GetWidth(); height = im.GetHeight(); pixels.resize( width * height );
		for( unsigned int y = 0; y < height; y++ ) for( unsigned int x = 0; x < width; x++ ) pixels[y*width+x] = im.GetPEL( x, y );
	}
};

// Render a built Job and return the mean coverage-composited luminance, or -1 on failure.
static double RenderMeanLum( Job& job )
{
	job.RemoveRasterizerOutputs();
	IRasterizer* r = job.GetRasterizer();
	if( !r ) return -1.0;
	Cap* cap = new Cap();
	GlobalLog()->PrintNew( cap, __FILE__, __LINE__, "render-equiv capture" );
	r->AddRasterizerOutput( cap );
	const bool ok = job.Rasterize();
	double mean = -1.0;
	if( ok && !cap->pixels.empty() ) {
		double sum = 0;
		for( const RISEColor& c : cap->pixels ) sum += ( 0.2126*c.base.r + 0.7152*c.base.g + 0.0722*c.base.b ) * c.a;
		mean = sum / double( cap->pixels.size() );
	}
	safe_release( cap );
	return mean;
}

int main( int argc, char** argv )
{
	if( argc <= 1 ) {
		std::printf( "CstRenderPixelEquivalenceTest: render spot-check for the by-construction fields (material IOR,\n"
		             "  camera intrinsics). SKIPPED in the suite (renders are slow + MC-non-deterministic). Run with\n"
		             "  scene path(s): ./bin/tests/CstRenderPixelEquivalenceTest <scene.RISEscene> ...\n" );
		return 0;
	}
	int fail = 0, ran = 0;
	for( int i = 1; i < argc; i++ ) {
		const std::string path = argv[i];
		const std::string text = ReadFile( path );
		if( text.empty() ) { std::printf( "  SKIP (unreadable/empty) %s\n", path.c_str() ); continue; }
		Job* jL = new Job(); GlobalLog()->PrintNew( jL, __FILE__, __LINE__, "jL" );
		const bool okL = ParseLegacy( text, *jL );
		if( !okL ) { std::printf( "  SKIP (legacy-fail) %s\n", path.c_str() ); safe_release( jL ); continue; }
		Job* jC = new Job(); GlobalLog()->PrintNew( jC, __FILE__, __LINE__, "jC" );
		std::vector<std::string> diags; Document d = ParseToCst( Migrate( text ) ); DeriveToJob( d, *jC, &diags );
		const double mL = RenderMeanLum( *jL );
		const double mC = RenderMeanLum( *jC );
		const double denom = std::max( std::max( std::fabs(mL), std::fabs(mC) ), 1e-6 );
		const double rel = std::fabs( mL - mC ) / denom;
		const bool ok = ( mL >= 0 && mC >= 0 && rel < 0.03 );   // 3%: MC render noise; a real IOR/intrinsic bug is far larger
		std::printf( "  %-60s legacy=%.6f cst=%.6f rel=%.4f %s\n", path.c_str(), mL, mC, rel, ok ? "OK" : "DIVERGE" );
		if( !ok ) fail++;
		ran++;
		safe_release( jL ); safe_release( jC );
	}
	std::printf( "%d passed, %d failed (of %d run).\n", ran - fail, fail, ran );
	return fail ? 1 : 0;
}
