//////////////////////////////////////////////////////////////////////
//  CstRenderPixelEquivalenceTest.cpp -- the Phase-B/0b RENDER spot-check.
//  The structural oracle (CstCorpusEquivalenceTest / DumpJob) verifies the CST derives the same scene
//  STRUCTURE + every cheaply+deterministically readable value. The fields it cannot reach -- material IOR
//  (an SPF scalar) + camera intrinsics (sensor/focal/fstop, not on ICamera) -- are covered "by construction"
//  (same Finalize, same ParamValue capture). This test CONFIRMS that empirically: for the same scene it
//  builds the legacy-parse Job and the CST-derive Job and compares PER-CHANNEL (R/G/B) mean coverage-
//  composited radiance. Per-channel (not a single luma) so a spectral/dispersive IOR HUE shift -- which can
//  move R against B while luma stays put -- is not buried. A wrong IOR (Fresnel/refraction) or DOF intrinsic
//  moves the image, so a render match is direct evidence those values derive correctly.
//
//  SCOPE + HONESTY:
//    * This is a one-time CUTOVER GATE + manual/CI check, NOT a standing suite regression guard -- the suite
//      has NO automated guard for IOR/intrinsics (they have no structural surface); they rest on the
//      by-construction argument + this manual check. Bare run SKIPS (renders are slow + MC-non-deterministic
//      across processes -> flaky in a suite). Run with scene path(s) for the compare.
//    * The legacy side renders the ORIGINAL v6 text; the CST side renders Migrate(v6). So this verifies the
//      MIGRATE+DERIVE path end-to-end (the right scope for the v6->v7 cutover), not the derive alone.
//    * The mean metric has teeth on the GROSS failure mode (a broken capture -> IOR defaulted/zeroed -> a
//      first-order change in transmitted energy), which is the mode CST value-capture actually produces; it
//      is weak on a SUBTLE energy-conserving redistribution (covered by the by-construction argument).
//    * RR threshold + accelerator are INVISIBLE to a mean compare on purpose: RR is unbiased (variance-only,
//      mean-neutral) and the accelerator is render-neutral (perf-only). NOTE the CST currently DROPS
//      `> set light_rr_threshold` (a known cutover gap, tracked for the dual-path) -- so meshlight_rr_test_pt
//      PASSING here confirms that drop is render-mean-neutral, NOT that the RR threshold derives.
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
	Cap() {}
protected:
	virtual ~Cap() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage& im, const Rect*, const unsigned int ) override
	{
		const unsigned int w = im.GetWidth(), h = im.GetHeight(); pixels.resize( w * h );
		for( unsigned int y = 0; y < h; y++ ) for( unsigned int x = 0; x < w; x++ ) pixels[y*w+x] = im.GetPEL( x, y );
	}
};

// Render a built Job into out[3] = per-channel mean coverage-composited radiance. Returns false on failure.
static bool RenderMean3( Job& job, double out[3] )
{
	out[0] = out[1] = out[2] = 0.0;
	IRasterizer* r = job.GetRasterizer();           // null-check BEFORE RemoveRasterizerOutputs (which derefs it)
	if( !r ) return false;
	job.RemoveRasterizerOutputs();
	Cap* cap = new Cap();
	GlobalLog()->PrintNew( cap, __FILE__, __LINE__, "render-equiv capture" );
	r->AddRasterizerOutput( cap );
	const bool ok = job.Rasterize();
	bool good = false;
	if( ok && !cap->pixels.empty() ) {
		double s[3] = { 0, 0, 0 };
		for( const RISEColor& c : cap->pixels ) { s[0] += c.base.r * c.a; s[1] += c.base.g * c.a; s[2] += c.base.b * c.a; }
		const double n = double( cap->pixels.size() );
		out[0] = s[0]/n; out[1] = s[1]/n; out[2] = s[2]/n; good = true;
	}
	safe_release( cap );
	return good;
}

int main( int argc, char** argv )
{
	if( argc <= 1 ) {
		std::printf( "CstRenderPixelEquivalenceTest: render spot-check for the by-construction fields (material IOR,\n"
		             "  camera intrinsics). SKIPPED in the suite (renders are slow + MC-non-deterministic; this is a\n"
		             "  cutover gate, not a standing regression guard). Run with scene path(s):\n"
		             "    ./bin/tests/CstRenderPixelEquivalenceTest <scene.RISEscene> ...\n" );
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
		double mL[3], mC[3];
		const bool rL = RenderMean3( *jL, mL );
		const bool rC = RenderMean3( *jC, mC );
		double maxrel = 0.0;
		for( int c = 0; c < 3; c++ ) { const double den = std::max( std::max( std::fabs(mL[c]), std::fabs(mC[c]) ), 1e-6 ); maxrel = std::max( maxrel, std::fabs(mL[c]-mC[c])/den ); }
		const bool ok = ( rL && rC && maxrel < 0.03 );   // 3%: MC render noise; a gross IOR/intrinsic capture bug is far larger
		std::printf( "  %-58s L=[%.5f %.5f %.5f] C=[%.5f %.5f %.5f] maxrel=%.4f %s\n",
		             path.c_str(), mL[0],mL[1],mL[2], mC[0],mC[1],mC[2], maxrel, ok ? "OK" : "DIVERGE" );
		if( !ok ) { fail++; for( const auto& dg : diags ) std::printf( "      derive-diag: %s\n", dg.c_str() ); }
		ran++;
		safe_release( jL ); safe_release( jC );
	}
	std::printf( "%d passed, %d failed (of %d run).\n", ran - fail, fail, ran );
	return fail ? 1 : 0;
}
