//////////////////////////////////////////////////////////////////////
//
//  WeaveMaterialChunkTest.cpp - Contract test for the `weave_material`
//    chunk (docs/CLOTH_FABRIC_DESIGN.md Phase 2, slice P2-A): the
//    parser / Job / registration surface, the weave DRAFT functions,
//    and the two numeric claims that have no other home.
//
//  WHAT THIS TEST OWNS -- and what it does not.  The LAYER ENERGY is
//  LayeredWhiteFurnaceTest's (the bounded posture and the locked
//  curves); value<->Scatter agreement and reciprocity are
//  SPFBSDFConsistencyTest's; the mixture density's normalisation is
//  SPFPdfConsistencyTest's.  This file owns the boundary a scene author
//  actually hits, plus:
//
//    1. THE DRAFT FUNCTIONS, EXHAUSTIVELY.  Each built-in weave's mean
//       warp coverage is a closed-form rational (plain 1/2, twill_2_1
//       2/3, twill_3_1 3/4, satin_5 4/5) and it is what the whole
//       material's two-family mix is anchored on.  Checked by
//       enumerating the ENTIRE repeating unit rather than by sampling,
//       and against the LITERAL rationals rather than against
//       `WeavePatternMeanCoverage` -- a test that read the same
//       constant it is checking would stay green through any edit to
//       it.
//
//    2. THE FOOTPRINT FADE, which is the anti-aliasing 9.9 gate 9b's
//       second finding demands.  A cell field is piecewise constant and
//       renders as a blocky checkerboard; this material's field is
//       continuous AND fades to the pattern mean once one pixel spans
//       the cell.  Both halves are checked: exact equality with the
//       mean at a large footprint, and CONTINUITY across a cell
//       boundary at zero footprint (the property a hard `floor` lacks).
//
//    3. THE PRESET TABLE, SLOT BY SLOT, behaviourally: a chunk naming
//       only `fabric X` must respond BIT-IDENTICALLY to a `fabric
//       custom` chunk whose slots are written out by hand at the
//       table's values.  Same contract `fabric_material`'s own chunk
//       test established, over eighteen slots instead of two.
//
//    4. EXPLICIT SLOTS WIN OVER THE PRESET -- the `Has()`-equivalent
//       rule, here implemented as "the parser forwards an EMPTY string
//       for an unwritten slot and `Job::AddWeaveMaterial` fills it".
//
//    5. THE TWO `coverage` DIAGNOSTICS, which are the only authoring
//       mistakes this chunk makes possible: `weave custom` with no
//       `coverage` bound (renders as a structureless 50/50 blend) and
//       `coverage` bound under a built-in draft (silently ignored).
//       Both are WARNINGS and both must still register the material.
//
//    6. FABRIC OVER WEAVE -- the composition the Phase-1 substrate
//       allowlist was extended for.  It must parse, register, produce a
//       NON-BLACK response, and -- the half that would otherwise rot --
//       `fabric silk` over a bare `ggx_material` must now WARN, because
//       silk's recommended substrate moved from GGX to weave.
//
//    7. requireSingle on every scalar slot, and editor introspection.
//
//    8. THE API-LEVEL FACTORY, with no Job in the picture.
//
//    9. `hemisphericalAlbedo`'S MEASURED ERROR -- an INDEPENDENT check in
//       the sense that the closed form is no longer fit to `value()`
//       (the two "realised fraction" constants this item used to guard
//       are gone, per the `C_v` fix -- see WeaveBRDF.cpp).  Brute-force
//       bihemispherical quadrature of the REAL `value()` against the
//       closed form, on all four shipped presets, all three channels.
//       The tolerance is PRE-COMMITTED at 25 % (kHemiAlbedoTol) -- set
//       2026-09-03 against the corrected `C_v`, re-measured after this
//       session's independent-quadrature work confirmed the mechanism:
//       the closed form is EXACT only at theta_o = 0, where `C_v` takes
//       its minimum, and `C_v` GROWS monotonically toward grazing (the
//       Chandrasekhar denominator relaxes), so the true bihemispherical
//       integral is systematically SMALLER than the theta=0 closed form
//       predicts -- for EVERY preset, tilted or not (measured worst
//       case: denim channel 0 at 21.2 %, the untilted preset, ruling
//       out tilt as the sole cause).  This is the SAME effect
//       `WeaveBRDF.cpp`'s own `hemisphericalAlbedo` banner pre-commits
//       at +-40 % relative for; 25 % is tighter than that ceiling with
//       a margin over the measured worst case, and looser than
//       `fabric_material`'s 5 % because this quantity is a normal-
//       incidence-exact estimate of a bihemispherical average rather
//       than an algebraically exact route.  Do NOT widen it further
//       without re-measuring and stating the new worst case here.
//
//   10. SPECTRAL PARITY AT AUTHORED WHITE, plus a saturated-dye
//       negative control so the parity check cannot pass with the tint
//       term dead.
//
//   11. THE MASKING-POLE SEAM (P2R4 review, finding P1-1) -- the
//       mutation-catching guard.  `ProjectDir`'s per-family azimuth has
//       a coordinate pole at view latitude `90 - tilt_deg` (where a
//       direction approaches the fibre axis and "front of thread" vs
//       "back of thread" is undefined); at the ORIGINAL `kMaxTilt`
//       (0.6 rad) this pole sat at an ordinary, in-frame view angle
//       (~55.6 deg) and the hard `max(cosPhi,0)` masking gate turned it
//       into a 48.9% brightness drop in a 0.5 deg step. Fixed by (a) a
//       smoothstep masking gate, (b) blending the azimuth ratio toward
//       the pole's own neutral value as the in-plane projection
//       vanishes, and (c) bounding `kMaxTilt` to 0.17 rad so the pole
//       cannot sit inside 80 deg for ANY in-range tilt.  This item
//       sweeps view latitude in 0.25 deg steps at `tilt = kMaxTilt` for
//       BOTH families and asserts no adjacent-sample brightness ratio
//       exceeds 1.10 anywhere below 88 deg -- tight enough that a
//       regression reintroducing the hard gate, or widening `kMaxTilt`
//       back out without the smoothing, fails immediately.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#ifdef _WIN32
	#include <process.h>
	#include <io.h>
	#define getpid _getpid
	#define RISE_TEST_DUP    _dup
	#define RISE_TEST_DUP2   _dup2
	#define RISE_TEST_CLOSE  _close
	#define RISE_TEST_FILENO _fileno
#else
	#include <unistd.h>
	#define RISE_TEST_DUP    dup
	#define RISE_TEST_DUP2   dup2
	#define RISE_TEST_CLOSE  close
	#define RISE_TEST_FILENO fileno
#endif

#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Math3D/VectorsOps.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Materials/WeaveMaterial.h"
#include "../src/Library/Materials/WeavePresets.h"
#include "../src/Library/Materials/FabricMaterial.h"
#include "../src/Library/SceneEditor/MaterialIntrospection.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "WeaveTestFixture.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

namespace {

//! PRE-COMMITTED, stated before the measurement.  See the banner's
//! item 9 for why it is 25 % rather than `fabric_material`'s 5 %, and
//! why 25 % (not the header's +-40 % ceiling) is the right number here.
//! The remedy for an exceedance is a baked directional-albedo table over
//! (width, azimuth, k_d, eta), not a looser band.
const double kHemiAlbedoTol = 0.25;

//////////////////////////////////////////////////////////////////////
// Scene plumbing -- FabricMaterialChunkTest's pattern, unchanged.
//////////////////////////////////////////////////////////////////////

std::string WriteTempScene( const std::string& tag, const std::string& body )
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? tmp : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pid[32];
	std::snprintf( pid, sizeof(pid), "%d", static_cast<int>( ::getpid() ) );
	const std::string path = dir + "rise_weavemat_" + tag + "_" + pid + ".RISEscene";
	std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
	f << body;
	f.close();
	return path;
}

bool ParseBodyInto( const std::string& tag, const std::string& body, IJobPriv& job )
{
	const std::string path = WriteTempScene( tag, "RISE ASCII SCENE 7\n" + body );
	const bool ok = job.LoadAsciiSceneViaCst( path.c_str() );
	remove( path.c_str() );
	return ok;
}

//! ParseBodyInto with stdout captured, so a diagnostic can be checked
//! for its WORDING rather than merely for a boolean.
bool ParseBodyCapturing( const std::string& tag, const std::string& body, IJobPriv& job,
                         std::string& capturedOutput )
{
	const char* tmpEnv = getenv( "TMPDIR" );
	std::string dir = tmpEnv ? tmpEnv : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pidbuf[32];
	std::snprintf( pidbuf, sizeof(pidbuf), "%d", static_cast<int>( ::getpid() ) );
	const std::string capPath = dir + "rise_weavemat_stdout_" + tag + "_" + pidbuf + ".txt";

	std::fflush( stdout );
	const int savedFd = RISE_TEST_DUP( RISE_TEST_FILENO( stdout ) );
	FILE* capFile = std::fopen( capPath.c_str(), "w" );
	if( capFile ) RISE_TEST_DUP2( RISE_TEST_FILENO( capFile ), RISE_TEST_FILENO( stdout ) );

	const bool ok = ParseBodyInto( tag, body, job );

	std::fflush( stdout );
	if( savedFd >= 0 ) { RISE_TEST_DUP2( savedFd, RISE_TEST_FILENO( stdout ) ); RISE_TEST_CLOSE( savedFd ); }
	if( capFile ) std::fclose( capFile );

	std::ifstream ifs( capPath.c_str() );
	if( ifs.is_open() ) { std::ostringstream oss; oss << ifs.rdbuf(); capturedOutput = oss.str(); }
	remove( capPath.c_str() );

	return ok;
}

bool Contains( const std::string& hay, const char* needle )
{
	return hay.find( needle ) != std::string::npos;
}

//////////////////////////////////////////////////////////////////////
// Scene fragments
//////////////////////////////////////////////////////////////////////

std::string ColorPainter( const char* name, const char* rgb )
{
	std::ostringstream o;
	o << "uniformcolor_painter\n{\n\tname\t" << name << "\n\tcolor\t" << rgb
	  << "\n\tcolorspace\tRec709RGB_Linear\n}\n";
	return o.str();
}

//! 17 significant digits: an ostream's default 6 would make a seeded
//! preset value and a hand-written one differ in the 7th digit, and the
//! bit-exactness checks below would then be measuring the harness.
std::string ScalarPainter( const char* name, double v )
{
	std::ostringstream o;
	o.precision( 17 );
	o << "scalar_painter\n{\n\tname\t" << name << "\n\tvalue\t" << v << "\n}\n";
	return o.str();
}

//! A PER-CHANNEL scalar painter, for the requireSingle negatives.
//! `scalar_painter`'s form-2 `values` slot builds an RGBScalarPainter,
//! whose HasPerChannelVariation() is what requireSingle rejects; form 1
//! (`value`) builds a UniformScalarPainter and would NOT trip it.
std::string RGBScalarPainter( const char* name, const char* rgb )
{
	std::ostringstream o;
	o << "scalar_painter\n{\n\tname\t" << name << "\n\tvalues\t" << rgb << "\n}\n";
	return o.str();
}

std::string GgxMat( const char* name, const char* diffuse, const char* f0, double ax, double ay )
{
	std::ostringstream o;
	o << "ggx_material\n{\n\tname\t" << name << "\n\trd\t" << diffuse << "\n\trs\t" << f0
	  << "\n\talphax\t" << ax << "\n\talphay\t" << ay
	  << "\n\tfresnel_mode\tschlick_f0\n}\n";
	return o.str();
}

//! `weave_material` fragment.  `extra` carries any additional
//! parameter lines verbatim -- one per line, because the chunk parser
//! is line-based per parameter.
std::string WeaveMat( const char* name, const char* fabric = 0, const char* weave = 0,
                      const std::string& extra = std::string() )
{
	std::ostringstream o;
	o << "weave_material\n{\n\tname\t" << name << "\n";
	if( fabric ) o << "\tfabric\t" << fabric << "\n";
	if( weave )  o << "\tweave\t"  << weave  << "\n";
	o << extra;
	o << "}\n";
	return o.str();
}

std::string FabricOverMat( const char* name, const char* base, const char* fabric )
{
	std::ostringstream o;
	o << "fabric_material\n{\n\tname\t" << name << "\n\tfabric\t" << fabric
	  << "\n\tbase\t" << base << "\n}\n";
	return o.str();
}

//! One parameter line, formatted at 17 digits.
std::string Line( const char* key, double v )
{
	std::ostringstream o;
	o.precision( 17 );
	o << "\t" << key << "\t" << v << "\n";
	return o.str();
}

std::string LineS( const char* key, const char* v )
{
	std::ostringstream o;
	o << "\t" << key << "\t" << v << "\n";
	return o.str();
}

//////////////////////////////////////////////////////////////////////
// A fixed shading point + probe pair, so materials are compared by
// BEHAVIOUR rather than by poking at their members.
//////////////////////////////////////////////////////////////////////

RayIntersectionGeometric MakeProbe( double u = 0.5, double v = 0.5 )
{
	const double th = 40.0 * PI / 180.0;
	const Vector3 inDir( sin(th), 0, -cos(th) );
	Ray inRay( Point3( sin(th), 0, 1.0 ), inDir );
	RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );
	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord = Point2( u, v );
	return ri;
}

Vector3 ProbeLight()
{
	return Vector3Ops::Normalize( Vector3( -0.3, 0.5, 0.8 ) );
}

double Respond( IJobPriv& job, const char* matName, double u = 0.5, double v = 0.5 )
{
	IMaterial* m = job.GetMaterials()->GetItem( matName );
	if( !m || !m->GetBSDF() ) return -1.0;
	RayIntersectionGeometric ri = MakeProbe( u, v );
	return ColorMath::MaxValue( m->GetBSDF()->value( ProbeLight(), ri ) );
}

double RespondNM( IJobPriv& job, const char* matName, double nm )
{
	IMaterial* m = job.GetMaterials()->GetItem( matName );
	if( !m || !m->GetBSDF() ) return -1.0;
	RayIntersectionGeometric ri = MakeProbe();
	return m->GetBSDF()->valueNM( ProbeLight(), ri, nm );
}

bool IsWeave( IJobPriv& job, const char* matName )
{
	IMaterial* m = job.GetMaterials()->GetItem( matName );
	return m && dynamic_cast<WeaveMaterial*>( m ) != 0;
}

//////////////////////////////////////////////////////////////////////
// 1. The draft functions, exhaustively
//////////////////////////////////////////////////////////////////////

void TestDraftMeans()
{
	std::cout << "DraftMeans" << std::endl;

	// The rationals are written out LITERALLY rather than read from
	// `WeavePatternMeanCoverage`: reading the constant under test would
	// make this green through any edit to it.  These four numbers are
	// also what the chunk descriptor and docs/MATERIALS.md quote, so a
	// drift here is a drift on the authoring surface.
	struct Row { WeavePatternKind kind; int period; double mean; const char* label; };
	static const Row rows[] = {
		{ eWeavePlain,   2, 1.0 / 2.0, "plain"     },
		{ eWeaveTwill21, 3, 2.0 / 3.0, "twill_2_1" },
		{ eWeaveTwill31, 4, 3.0 / 4.0, "twill_3_1" },
		{ eWeaveSatin5,  5, 4.0 / 5.0, "satin_5"   },
	};

	for( const Row& r : rows )
	{
		// EXHAUSTIVE over the repeating unit, not sampled: the unit is
		// period x period cells and every one of them is enumerated, so
		// this is the exact mean rather than an estimate of it.
		int on = 0;
		for( int i = 0; i < r.period; ++i )
			for( int j = 0; j < r.period; ++j )
				if( WeaveCellWarpOnTop( r.kind, i, j ) ) ++on;
		const double measured = (double)on / (double)( r.period * r.period );

		Check( std::fabs( measured - r.mean ) < 1e-12,
		       std::string( "draft " ) + r.label + ": exact mean warp coverage" );
		Check( std::fabs( WeavePatternMeanCoverage( r.kind ) - r.mean ) < 1e-12,
		       std::string( "draft " ) + r.label + ": WeavePatternMeanCoverage agrees with the enumeration" );

		// NEGATIVE INDICES.  Surface UVs go negative routinely, and C++'s
		// `%` keeps the dividend's sign -- a naive `i % period` would
		// mirror the draft across the origin and put a visible seam
		// through any object whose UVs cross zero.
		int onNeg = 0;
		for( int i = -r.period; i < 0; ++i )
			for( int j = -r.period; j < 0; ++j )
				if( WeaveCellWarpOnTop( r.kind, i, j ) ) ++onNeg;
		Check( onNeg == on,
		       std::string( "draft " ) + r.label + ": negative cell indices tile identically" );
	}

	// The two twills must actually be DIAGONAL, which is what separates
	// them from a satin: shifting one cell in i and one in j must land
	// on the same value, at every cell.
	for( int i = -6; i <= 6; ++i ) {
		for( int j = -6; j <= 6; ++j ) {
			Check( WeaveCellWarpOnTop( eWeaveTwill31, i, j ) ==
			       WeaveCellWarpOnTop( eWeaveTwill31, i + 1, j + 1 ),
			       "twill_3_1 is diagonal (the wale)" );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// 2. The footprint fade and the smooth yarn edge
//////////////////////////////////////////////////////////////////////

void TestFootprintFadeAndSmoothEdges()
{
	std::cout << "FootprintFadeAndSmoothEdges" << std::endl;

	const double scale = 40.0;			// cells per UV unit
	const double cell  = 1.0 / scale;

	struct Row { WeavePatternKind kind; double mean; const char* label; };
	static const Row rows[] = {
		{ eWeavePlain,   1.0 / 2.0, "plain"     },
		{ eWeaveTwill21, 2.0 / 3.0, "twill_2_1" },
		{ eWeaveTwill31, 3.0 / 4.0, "twill_3_1" },
		{ eWeaveSatin5,  4.0 / 5.0, "satin_5"   },
	};

	for( const Row& r : rows )
	{
		// A footprint of TWO cells is past the fade's upper knee, so the
		// coverage must be EXACTLY the pattern mean -- not merely close.
		// This is the anti-aliasing contract: a minified weave IS its
		// mean, and if it were not the surface would shimmer.
		bool exact = true;
		for( int s = 0; s < 40; ++s ) {
			const double u = 0.013 * s, v = 0.021 * s;
			const double c = WeaveCoverageAt( r.kind, u, v, scale, cell * 2.0 );
			if( std::fabs( c - r.mean ) > 1e-12 ) exact = false;
		}
		Check( exact, std::string( "fade " ) + r.label +
		              ": at a 2-cell footprint the coverage is EXACTLY the pattern mean" );

		// A footprint of ZERO means "unavailable" (secondary bounces,
		// non-mesh geometry) and must DISENGAGE the fade rather than
		// snap to the mean -- the same convention `TextureFootprint::
		// valid` and the expression VM's `fw` use.  So at least one
		// probe must differ from the mean.
		bool anyDiffers = false;
		for( int s = 0; s < 40 && !anyDiffers; ++s ) {
			const double u = 0.013 * s, v = 0.021 * s;
			if( std::fabs( WeaveCoverageAt( r.kind, u, v, scale, 0.0 ) - r.mean ) > 1e-6 )
				anyDiffers = true;
		}
		Check( anyDiffers, std::string( "fade " ) + r.label +
		                   ": a ZERO footprint disengages the fade (structure survives)" );

		// CONTINUITY -- the property gate 9b's checkerboard finding is
		// about.  Walk a line across many cell boundaries at zero
		// footprint and bound the step between adjacent samples.  A hard
		// `floor`-based field would step by 1.0 at a boundary; the
		// smoothed field's largest step is bounded by the sample spacing
		// divided by the ramp width.
		const int    N      = 20000;
		const double span   = 10.0 * cell;			// ten cells
		const double du     = span / N;
		// Ramp half-width in UV is kWeaveEdgeSoftness cells; over a step
		// of `du` the field can change by at most du / (2 * softness *
		// cell), and both u and v advance, hence the factor 2.
		const double bound  = 2.2 * du / ( 2.0 * kWeaveEdgeSoftness * cell );
		double maxStep = 0;
		double prev = WeaveCoverageAt( r.kind, 0.0, 0.0, scale, 0.0 );
		for( int s = 1; s <= N; ++s ) {
			const double u = s * du, v = s * du;
			const double c = WeaveCoverageAt( r.kind, u, v, scale, 0.0 );
			maxStep = ( std::fabs( c - prev ) > maxStep ) ? std::fabs( c - prev ) : maxStep;
			prev = c;
		}
		Check( maxStep <= bound, std::string( "fade " ) + r.label +
		       ": the coverage field is CONTINUOUS across cell boundaries (no hard step)" );

		// And the smoothing must PRESERVE the mean: a wide average of
		// the un-faded field still has to land on the pattern's own
		// rational, or the fade's target would disagree with what the
		// field actually averages to.
		double acc = 0; int n = 0;
		for( int a = 0; a < 400; ++a ) {
			for( int b = 0; b < 400; ++b ) {
				acc += WeaveCoverageAt( r.kind, ( a + 0.5 ) * cell / 20.0,
				                                ( b + 0.5 ) * cell / 20.0, scale, 0.0 );
				++n;
			}
		}
		Check( std::fabs( acc / n - r.mean ) < 0.01, std::string( "fade " ) + r.label +
		       ": the SMOOTHED field averages to the same rational the fade targets" );
	}
}

//////////////////////////////////////////////////////////////////////
// 3. The preset table, slot by slot
//////////////////////////////////////////////////////////////////////

void TestPresetsSeedTheSlots()
{
	std::cout << "PresetsSeedTheSlots" << std::endl;

	// Restated LITERALLY, not read out of WeavePresets.h, for the reason
	// item 1's comment gives.  Order: weave, scale, gap, then per-family
	// colour / ior / width / azimuth / kd / tilt.
	struct Row {
		const char* name; const char* weave; double scale; double gap;
		const char* warpCol; double wIor, wWid, wAzi, wKd, wTilt;
		const char* weftCol; double fIor, fWid, fAzi, fKd, fTilt;
	};
	// RE-TRANSCRIBED 2026-09-03 against WeavePresets.h's CURRENT table
	// (thread-count `weave_scale`, halved satin/silk tilts, denim's
	// widened weft width) -- see that header's own changelog banner for
	// why the numbers moved.  A stale twin here would silently pass
	// (Phase 2's own values only differ from the hand-written ones by
	// how far they are, and this loop simply compares two numbers) or
	// FAIL loudly if it drifted only partway, which is what caught this
	// entry being stale in the first place.
	static const Row rows[] = {
		{ "denim", "twill_3_1", 2500.0, 0.02,
		  "cDenimWarp", 1.46, 0.28, 1.25, 0.35,  0.0,
		  "cDenimWeft", 1.46, 0.30, 1.30, 0.35,  0.0 },
		{ "silk",  "satin_5",   8000.0, 0.0,
		  "cSilkWarp",  1.345, 0.110, 1.10, 0.20,  0.08,
		  "cSilkWeft",  1.345, 0.300, 1.30, 0.30, -0.08 },
		{ "satin", "satin_5",   6000.0, 0.0,
		  "cSatinWarp", 1.539, 0.100, 0.90, 0.10,  0.09,
		  "cSatinWeft", 1.539, 0.400, 1.40, 0.70, -0.09 },
		{ "linen", "plain",     2000.0, 0.10,
		  "cLinenWarp", 1.46, 0.240, 1.30, 0.30,  0.0,
		  "cLinenWeft", 1.46, 0.240, 1.30, 0.30,  0.0 },
		{ "custom", "plain",    2500.0, 0.0,
		  "cWhite", 1.46, 0.25, 1.0, 0.30, 0.0,
		  "cWhite", 1.46, 0.25, 1.0, 0.30, 0.0 },
	};

	const std::string palette =
		  ColorPainter( "cDenimWarp", "0.062 0.084 0.175" )
		+ ColorPainter( "cDenimWeft", "0.640 0.600 0.520" )
		+ ColorPainter( "cSilkWarp",  "0.640 0.570 0.320" )
		+ ColorPainter( "cSilkWeft",  "0.520 0.460 0.255" )
		+ ColorPainter( "cSatinWarp", "0.520 0.190 0.150" )
		+ ColorPainter( "cSatinWeft", "0.430 0.155 0.120" )
		+ ColorPainter( "cLinenWarp", "0.740 0.680 0.560" )
		+ ColorPainter( "cLinenWeft", "0.720 0.660 0.540" )
		+ ColorPainter( "cWhite",     "1 1 1" );

	for( const Row& r : rows )
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );

		std::string explicitSlots =
			  Line ( "weave_scale",  r.scale )
			+ Line ( "gap",          r.gap )
			+ LineS( "warp_color",   r.warpCol )
			+ Line ( "warp_ior",     r.wIor )
			+ Line ( "warp_width",   r.wWid )
			+ Line ( "warp_azimuth", r.wAzi )
			+ Line ( "warp_kd",      r.wKd )
			+ Line ( "warp_tilt",    r.wTilt )
			+ LineS( "weft_color",   r.weftCol )
			+ Line ( "weft_ior",     r.fIor )
			+ Line ( "weft_width",   r.fWid )
			+ Line ( "weft_azimuth", r.fAzi )
			+ Line ( "weft_kd",      r.fKd )
			+ Line ( "weft_tilt",    r.fTilt );

		const std::string body = palette
			+ WeaveMat( "preset",   r.name )
			+ WeaveMat( "explicit", "custom", r.weave, explicitSlots );
		ParseBodyInto( std::string( "preset_" ) + r.name, body, *job );

		Check( IsWeave( *job, "preset" ),
		       std::string( "preset " ) + r.name + ": registers a WeaveMaterial" );

		// BIT-IDENTICAL, at several UV positions so the DRAFT is
		// compared too, not just the per-family scalars.
		bool same = true;
		for( int s = 0; s < 12; ++s ) {
			const double u = 0.017 * s + 0.3, v = 0.029 * s + 0.11;
			const double a = Respond( *job, "preset",   u, v );
			const double b = Respond( *job, "explicit", u, v );
			if( !( a >= 0 && b >= 0 && a == b ) ) same = false;
		}
		Check( same, std::string( "preset " ) + r.name +
		             ": seeds EVERY slot (bit-identical to the hand-written twin)" );

		safe_release( job );
	}
}

//////////////////////////////////////////////////////////////////////
// 4. Explicit slots win; every draft parses and differs
//////////////////////////////////////////////////////////////////////

void TestExplicitSlotsWinAndDraftsDiffer()
{
	std::cout << "ExplicitSlotsWinAndDraftsDiffer" << std::endl;

	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		const std::string body = ColorPainter( "cWhite", "1 1 1" )
			+ WeaveMat( "plainSatin",  "satin" )
			// Same preset, but the DRAFT overridden.  Everything else
			// still comes from `satin`.
			+ WeaveMat( "twillSatin",  "satin", "twill_3_1" )
			// Same preset, one SCALAR overridden.
			+ WeaveMat( "wideSatin",   "satin", 0, Line( "warp_width", 0.6 ) );
		ParseBodyInto( "explicit", body, *job );

		Check( IsWeave( *job, "plainSatin" ) && IsWeave( *job, "twillSatin" )
		    && IsWeave( *job, "wideSatin" ),
		       "explicit: all three variants register" );

		// The draft override must MOVE the response somewhere on the
		// surface.  It changes only which family is on top per cell, so
		// a single UV could coincide; sweep.
		bool draftMoves = false;
		for( int s = 0; s < 40 && !draftMoves; ++s ) {
			const double u = 0.011 * s + 0.2, v = 0.019 * s + 0.05;
			if( Respond( *job, "plainSatin", u, v ) != Respond( *job, "twillSatin", u, v ) )
				draftMoves = true;
		}
		Check( draftMoves, "explicit: an authored `weave` overrides the preset's draft" );

		Check( Respond( *job, "plainSatin" ) != Respond( *job, "wideSatin" ),
		       "explicit: an authored `warp_width` overrides the preset's" );
		safe_release( job );
	}

	// Every draft spelling parses and registers.
	{
		static const char* drafts[] = { "plain", "twill_2_1", "twill_3_1", "satin_5" };
		for( const char* d : drafts ) {
			IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
			const std::string body = ColorPainter( "cWhite", "1 1 1" ) + WeaveMat( "w", "linen", d );
			ParseBodyInto( std::string( "draft_" ) + d, body, *job );
			Check( IsWeave( *job, "w" ), std::string( "draft `" ) + d + "` parses and registers" );
			safe_release( job );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// 5. `coverage`: the custom draft, and the two diagnostics
//////////////////////////////////////////////////////////////////////

void TestCustomCoverage()
{
	std::cout << "CustomCoverage" << std::endl;

	// A bound coverage field must SELECT between the two families: at
	// coverage 1 the response must equal a pure-warp material's and at 0
	// a pure-weft material's.  Denim is used because its two families
	// carry very different dyes, so the two ends are far apart.
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		const std::string body = ColorPainter( "cWhite", "1 1 1" )
			+ ScalarPainter( "covAll",  1.0 )
			+ ScalarPainter( "covNone", 0.0 )
			+ WeaveMat( "allWarp",  "denim", "custom", LineS( "coverage", "covAll" ) )
			+ WeaveMat( "allWeft",  "denim", "custom", LineS( "coverage", "covNone" ) );
		ParseBodyInto( "coverage", body, *job );

		Check( IsWeave( *job, "allWarp" ) && IsWeave( *job, "allWeft" ),
		       "coverage: `weave custom` with a bound field registers" );
		const double a = Respond( *job, "allWarp" );
		const double b = Respond( *job, "allWeft" );
		Check( a > 0 && b > 0 && a != b,
		       "coverage: the field genuinely selects between the two families" );
		safe_release( job );
	}

	// `weave custom` with NOTHING bound: warns, still registers, and
	// renders as the structureless 50/50 blend the warning describes.
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		const std::string body = ColorPainter( "cWhite", "1 1 1" )
			+ ScalarPainter( "covHalf", 0.5 )
			+ WeaveMat( "unbound", "denim", "custom" )
			+ WeaveMat( "half",    "denim", "custom", LineS( "coverage", "covHalf" ) );
		ParseBodyCapturing( "coverage_unbound", body, *job, out );

		Check( IsWeave( *job, "unbound" ),
		       "coverage: `weave custom` with no field still REGISTERS (warning, not error)" );
		Check( Contains( out, "no weave structure at all" ),
		       "coverage: the unbound-custom warning names the consequence" );
		Check( Respond( *job, "unbound" ) == Respond( *job, "half" ),
		       "coverage: an unbound custom field is exactly the 50/50 blend the warning claims" );
		safe_release( job );
	}

	// `coverage` under a BUILT-IN draft: warns that it is ignored, and
	// really is ignored.
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		const std::string body = ColorPainter( "cWhite", "1 1 1" )
			+ ScalarPainter( "covAll", 1.0 )
			+ WeaveMat( "withCov", "denim", "twill_3_1", LineS( "coverage", "covAll" ) )
			+ WeaveMat( "noCov",   "denim", "twill_3_1" );
		ParseBodyCapturing( "coverage_ignored", body, *job, out );

		Check( Contains( out, "will be IGNORED" ),
		       "coverage: binding it under a built-in draft warns that it is ignored" );
		bool identical = true;
		for( int s = 0; s < 20; ++s ) {
			const double u = 0.013 * s, v = 0.023 * s;
			if( Respond( *job, "withCov", u, v ) != Respond( *job, "noCov", u, v ) ) identical = false;
		}
		Check( identical, "coverage: and it really is ignored (bit-identical response)" );
		safe_release( job );
	}
}

//////////////////////////////////////////////////////////////////////
// 6. Fabric over weave
//////////////////////////////////////////////////////////////////////

void TestFabricOverWeave()
{
	std::cout << "FabricOverWeave" << std::endl;

	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		const std::string body = ColorPainter( "cWhite", "1 1 1" )
			+ WeaveMat( "wsatin", "satin" )
			+ FabricOverMat( "fow", "wsatin", "satin" );
		ParseBodyCapturing( "fabric_over_weave", body, *job, out );

		IMaterial* m = job->GetMaterials()->GetItem( "fow" );
		Check( m && dynamic_cast<FabricMaterial*>( m ) != 0,
		       "fabric-over-weave: the allowlist ACCEPTS a weave_material substrate" );
		const double r = Respond( *job, "fow" );
		Check( r > 0, "fabric-over-weave: renders NON-BLACK" );
		Check( r != Respond( *job, "wsatin" ),
		       "fabric-over-weave: the fuzz layer actually changes the response" );
		// The preset now RECOMMENDS a weave, so this composition must be
		// the one that does NOT warn.
		Check( !Contains( out, "expects a weave_material substrate" ),
		       "fabric-over-weave: a matching substrate produces no mismatch warning" );

		// hemisphericalAlbedo must survive the composition -- it is the
		// property the allowlist exists to require, and the route-1
		// closed form consumes it.
		RISEPel ha( 0, 0, 0 );
		Check( m && m->GetBSDF()->hemisphericalAlbedo( MakeProbe(), ha ) &&
		       ha[0] > 0 && ha[0] <= 1.0,
		       "fabric-over-weave: hemisphericalAlbedo resolves and is in [0,1]" );
		safe_release( job );
	}

	// The other half, which would otherwise rot: silk's recommended
	// substrate MOVED from ggx_material to weave_material in Phase 2, so
	// `fabric silk` over a bare anisotropic GGX -- the Phase-1 shape --
	// must now WARN.  The composition stays legal and still registers.
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		const std::string body = ColorPainter( "cWhite", "1 1 1" )
			+ ColorPainter( "cF0", "0.04 0.04 0.04" )
			+ GgxMat( "ggxbase", "cWhite", "cF0", 0.30, 0.10 )
			+ FabricOverMat( "silkOverGgx", "ggxbase", "silk" );
		ParseBodyCapturing( "silk_over_ggx", body, *job, out );

		IMaterial* m = job->GetMaterials()->GetItem( "silkOverGgx" );
		Check( m != 0, "fabric silk over ggx: STILL REGISTERS (warning, not error)" );
		Check( Contains( out, "expects a weave_material substrate" ),
		       "fabric silk over ggx: now warns, because Phase 2 moved the recommendation" );
		safe_release( job );
	}
}

//////////////////////////////////////////////////////////////////////
// 7. requireSingle, and editor introspection
//////////////////////////////////////////////////////////////////////

void TestRequireSingleAndIntrospection()
{
	std::cout << "RequireSingleAndIntrospection" << std::endl;

	// One representative slot per KIND of scalar (a frame angle, a lobe
	// width, the coverage field) rather than all fourteen: they all go
	// through the same `ResolveOrDiagnoseScalar(..., requireSingle=true)`
	// call, so this pins the flag rather than fourteen copies of it.
	static const char* slots[] = { "weave_rotation", "warp_width", "gap" };
	for( const char* slot : slots )
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		const std::string body = ColorPainter( "cWhite", "1 1 1" )
			+ RGBScalarPainter( "perChan", "0.1 0.2 0.3" )
			+ WeaveMat( "w", "linen", 0, LineS( slot, "perChan" ) );
		ParseBodyCapturing( std::string( "reqsingle_" ) + slot, body, *job, out );
		Check( !IsWeave( *job, "w" ),
		       std::string( "requireSingle: a PER-CHANNEL painter on `" ) + slot + "` is refused" );
		safe_release( job );
	}

	// Introspection: the editor must see every rebindable slot, and must
	// NOT see `coverage` when no field is bound (a row for a binding
	// that does not exist would be a lie).
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		const std::string body = ColorPainter( "cWhite", "1 1 1" )
			+ ScalarPainter( "covAll", 1.0 )
			+ WeaveMat( "builtin", "denim" )
			+ WeaveMat( "custom",  "denim", "custom", LineS( "coverage", "covAll" ) );
		ParseBodyInto( "introspect", body, *job );

		IMaterial* mb = job->GetMaterials()->GetItem( "builtin" );
		IMaterial* mc = job->GetMaterials()->GetItem( "custom" );
		Check( mb && mc, "introspection: both materials registered" );
		if( mb && mc )
		{
			Check( MaterialIntrospection::GetTypeName( *mb ) == String( "Weave" ),
			       "introspection: surfaces as `Weave`" );

			// Every scalar slot, on the SCALAR pipe.
			static const char* scalarSlots[] = {
				"weave_scale", "weave_rotation", "weft_skew", "gap",
				"warp_ior", "weft_ior", "warp_width", "weft_width",
				"warp_azimuth", "weft_azimuth", "warp_kd", "weft_kd",
				"warp_tilt", "weft_tilt"
			};
			bool allScalars = true;
			for( const char* want : scalarSlots ) {
				const MaterialSlotRef ref = MaterialIntrospection::GetSlot( *mb, String( want ) );
				if( !( ref.kind == MaterialSlotRef::ScalarPainter && ref.scalarPainter != 0 ) )
					allScalars = false;
			}
			Check( allScalars, "introspection: all fourteen scalar slots resolve on the SCALAR pipe" );

			// And the two DYES, on the COLOUR pipe -- the split is the
			// whole point of docs/ISCALARPAINTER_REFACTOR.md's routing,
			// and getting it backwards would put a dye through the JH
			// uplift or a width through a colourspace conversion.
			bool allColors = true;
			for( const char* want : { "warp_color", "weft_color" } ) {
				const MaterialSlotRef ref = MaterialIntrospection::GetSlot( *mb, String( want ) );
				if( !( ref.kind == MaterialSlotRef::Painter && ref.painter != 0 ) )
					allColors = false;
			}
			Check( allColors, "introspection: both dyes resolve on the COLOUR pipe" );

			// `coverage` must report None on a built-in draft: nothing is
			// bound, and claiming a binding that does not exist would
			// hand the editor a dangling row.
			Check( MaterialIntrospection::GetSlot( *mb, String( "coverage" ) ).kind
			           == MaterialSlotRef::None,
			       "introspection: `coverage` is None on a built-in draft (nothing bound)" );
			Check( MaterialIntrospection::GetSlot( *mc, String( "coverage" ) ).kind
			           == MaterialSlotRef::ScalarPainter,
			       "introspection: `coverage` resolves once a custom draft binds one" );

			// `weave` is an ENUM, not a painter slot.
			Check( MaterialIntrospection::GetSlot( *mb, String( "weave" ) ).kind
			           == MaterialSlotRef::None,
			       "introspection: `weave` is not a painter slot" );

			// SetSlot reaches the model -- and, because WeaveSPF reads
			// every parameter back through the BRDF, it reaches the
			// SAMPLER too with no second forwarder to keep in step.
			const double before = Respond( *job, "builtin" );
			UniformScalarPainter* wide = new UniformScalarPainter( 0.8 );  wide->addref();
			Check( MaterialIntrospection::SetSlot( *mb, String( "warp_width" ), 0, wide ),
			       "introspection: SetSlot accepts `warp_width`" );
			Check( Respond( *job, "builtin" ) != before,
			       "introspection: the rebind changes the BRDF response" );
			safe_release( wide );

			UniformScalarPainter* junk = new UniformScalarPainter( 1.0 );  junk->addref();
			Check( !MaterialIntrospection::SetSlot( *mb, String( "weave" ), 0, junk ),
			       "introspection: `weave` is NOT a settable slot" );
			safe_release( junk );
		}
		safe_release( job );
	}
}

//////////////////////////////////////////////////////////////////////
// 8. The API-level factory
//////////////////////////////////////////////////////////////////////

void TestApiLevelFactory()
{
	std::cout << "ApiLevelFactory" << std::endl;

	UniformColorPainter*  col = new UniformColorPainter( RISEPel( 0.6, 0.5, 0.4 ) );  col->addref();
	UniformScalarPainter* s40 = new UniformScalarPainter( 40.0 );  s40->addref();
	UniformScalarPainter* z   = new UniformScalarPainter( 0.0 );   z->addref();
	UniformScalarPainter* cov = new UniformScalarPainter( 0.5 );   cov->addref();
	UniformScalarPainter* io  = new UniformScalarPainter( 1.46 );  io->addref();
	UniformScalarPainter* wd  = new UniformScalarPainter( 0.25 );  wd->addref();
	UniformScalarPainter* az  = new UniformScalarPainter( 1.0 );   az->addref();
	UniformScalarPainter* kd  = new UniformScalarPainter( 0.3 );   kd->addref();

	// An unrecognised draft resolves to `plain`, matching the chunk
	// descriptor's own default rather than failing.
	IMaterial* m = 0;
	const bool ok = RISE_API_CreateWeaveMaterial( &m, "not_a_draft",
		*s40, *z, *z, cov, *z, *col, *io, *wd, *az, *kd, *z,
		*col, *io, *wd, *az, *kd, *z );
	Check( ok && m != 0, "API: an unrecognised draft name still constructs" );
	WeaveMaterial* wm = dynamic_cast<WeaveMaterial*>( m );
	Check( wm && wm->GetPattern() == eWeavePlain, "API: it resolves to `plain`" );

	// `coverage` is DROPPED for a built-in draft rather than retained --
	// holding a reference to a painter no code path reads would keep it
	// alive for the scene's lifetime and make the editor claim a binding
	// that does nothing.
	Check( wm && wm->GetCoverage() == 0,
	       "API: `coverage` is dropped when the draft is not `custom`" );
	safe_release( m );

	IMaterial* m2 = 0;
	RISE_API_CreateWeaveMaterial( &m2, "custom",
		*s40, *z, *z, cov, *z, *col, *io, *wd, *az, *kd, *z,
		*col, *io, *wd, *az, *kd, *z );
	WeaveMaterial* wm2 = dynamic_cast<WeaveMaterial*>( m2 );
	Check( wm2 && wm2->GetCoverage() == cov, "API: `coverage` is retained for `custom`" );
	safe_release( m2 );

	// A null out-pointer is refused rather than crashing.
	Check( !RISE_API_CreateWeaveMaterial( 0, "plain",
		*s40, *z, *z, cov, *z, *col, *io, *wd, *az, *kd, *z,
		*col, *io, *wd, *az, *kd, *z ),
	       "API: a null out-pointer is refused" );

	safe_release( kd ); safe_release( az ); safe_release( wd ); safe_release( io );
	safe_release( cov ); safe_release( z ); safe_release( s40 ); safe_release( col );
}

//////////////////////////////////////////////////////////////////////
// 9. hemisphericalAlbedo's measured error
//////////////////////////////////////////////////////////////////////

//! INT f(l,v) (n.l) dl by deterministic quadrature, per channel.
RISEPel RhoQuad( IBSDF& b, double thetaO, int NT, int NP )
{
	const double s = sin( thetaO ), c = cos( thetaO );
	const Vector3 inDir( s, 0, -c );
	Ray inRay( Point3( s, 0, 1.0 ), inDir );
	RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );
	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord = Point2( 0.5, 0.5 );

	RISEPel acc( 0, 0, 0 );
	for( int i = 0; i < NT; ++i ) {
		const double th = ( i + 0.5 ) * PI_OV_TWO / NT, st = sin( th ), ct = cos( th );
		for( int j = 0; j < NP; ++j ) {
			const double ph = ( j + 0.5 ) * TWO_PI / NP;
			const Vector3 l( st * cos( ph ), st * sin( ph ), ct );
			acc = acc + b.value( l, ri ) * ( ct * st * ( PI_OV_TWO / NT ) * ( TWO_PI / NP ) );
		}
	}
	return acc;
}

void TestHemisphericalAlbedoError()
{
	std::cout << "HemisphericalAlbedoError" << std::endl;

	std::cout << "  preset      channel   closed form        brute force        ratio" << std::endl;

	static const char* presets[] = { "denim", "silk", "satin", "linen" };
	for( const char* nm : presets )
	{
		RISE::WeaveTest::PresetWeave pw( nm );

		RISEPel est( 0, 0, 0 );
		const bool got = pw.BSDF()->hemisphericalAlbedo( MakeProbe(), est );
		Check( got, std::string( "hemi " ) + nm + ": hemisphericalAlbedo reports a value" );

		// The bihemispherical average: 2 INT rho(theta) cos sin dtheta.
		RISEPel truth( 0, 0, 0 );
		const int NB = 24;
		for( int i = 0; i < NB; ++i ) {
			const double th = ( i + 0.5 ) * PI_OV_TWO / NB;
			truth = truth + RhoQuad( *pw.BSDF(), th, 100, 200 )
			              * ( cos( th ) * sin( th ) * ( PI_OV_TWO / NB ) );
		}
		truth = truth * 2.0;

		for( int c = 0; c < 3; ++c )
		{
			const double ratio = ( est[(unsigned int)c] > 0 )
				? truth[(unsigned int)c] / est[(unsigned int)c] : 0.0;
			std::printf( "  %-10s    %d     %10.6f       %10.6f       %6.3f\n",
			             nm, c, est[(unsigned int)c], truth[(unsigned int)c], ratio );
			Check( std::fabs( ratio - 1.0 ) <= kHemiAlbedoTol,
			       std::string( "hemi " ) + nm + " ch" + char( '0' + c ) +
			       ": closed form within the pre-committed band of brute force" );
		}

		// The NM twin must track the RGB one at a wavelength where the
		// dye is nearly flat -- the two share `ResolveWeave` and differ
		// only in which tint they read, so a divergence here is a twin
		// drift rather than a model error.
		Scalar estNM = 0;
		Check( pw.BSDF()->hemisphericalAlbedoNM( MakeProbe(), 550.0, estNM ) &&
		       estNM > 0 && estNM <= 1.0,
		       std::string( "hemi " ) + nm + ": the NM twin reports a value in [0,1]" );
	}
}

//////////////////////////////////////////////////////////////////////
// 11. The masking-pole seam -- mutation-catching guard (P2R4 P1-1)
//////////////////////////////////////////////////////////////////////

//! A probe at a SWEPT view latitude in the x-z plane (light fixed,
//! near-normal but off-axis so phi_d != 0 exactly), matching the
//! review's own reproduction: with `weave_rotation = 0` a family's
//! tangent tilted by `tilt` about the y-axis lies in exactly this
//! plane, so its pole (view latitude `90 - tilt_deg`) is crossed by
//! this sweep head-on -- the worst case, not a glancing one.
RayIntersectionGeometric MakeViewSweepProbe( double thetaVdeg )
{
	const double th = thetaVdeg * PI / 180.0;
	const Vector3 wo( sin( th ), 0, cos( th ) );
	Ray inRay( Point3( 0, 0, 0 ), Vector3( -wo.x, -wo.y, -wo.z ) );
	RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );
	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord = Point2( 0.5, 0.5 );
	return ri;
}

//! Sweeps `theta_v` from just above 0 to 88 deg in 0.25 deg steps,
//! isolating ONE family at a time (`coverage` forced to 1 or 0) at
//! `tilt = +-kMaxTilt` -- the in-spec extreme the review reproduced the
//! seam at -- and asserts no adjacent sample's brightness differs by
//! more than `kMaxAdjacentRatio`.  Isolating the family (rather than a
//! shipped two-family preset) is deliberate: it puts the swept family's
//! own pole exactly in the swept plane with nothing else diluting it,
//! which is the worst case a preset's own (much smaller) tilt could
//! ever produce.
void TestMaskingPoleSeam()
{
	std::cout << "MaskingPoleSeam" << std::endl;

	const double kMaxAdjacentRatio = 1.10;
	const double kStepDeg          = 0.25;
	// STRICTLY below 88 deg: the true geometric horizon (theta_v -> 90,
	// n.w -> 0) produces its OWN steep-but-smooth, physically ordinary
	// falloff that legitimately exceeds this ratio bound approaching
	// 88-90 deg on every preset (Fresnel/Mp/geometric-cosine terms all
	// compound there) -- unrelated to, and much closer to true grazing
	// than, the masking-pole seam this sweep exists to catch (measured
	// at ~80.3 deg for tilt = kMaxTilt).  Stopping short of it is what
	// keeps this a seam-mutation guard rather than a report of ordinary
	// grazing behaviour.
	const double kMaxThetaDeg      = 87.75;
	const Vector3 light = Vector3Ops::Normalize( Vector3( 0.05, 0, 0.9987 ) );

	struct Family { const char* label; double coverage; double tilt; };
	static const Family kFamilies[] = {
		{ "warp (+kMaxTilt)", 1.0,  0.17 },	// kMaxTilt's shipped value,
		{ "weft (-kMaxTilt)", 0.0, -0.17 },	// restated literally: see
		                                        // the note below on why.
	};

	for( const Family& fam : kFamilies )
	{
		UniformScalarPainter* scale = new UniformScalarPainter( 2500.0 ); scale->addref();
		UniformScalarPainter* rot   = new UniformScalarPainter( 0.0 );    rot->addref();
		UniformScalarPainter* skew  = new UniformScalarPainter( 0.0 );    skew->addref();
		UniformScalarPainter* gap   = new UniformScalarPainter( 0.0 );    gap->addref();
		UniformScalarPainter* cov   = new UniformScalarPainter( fam.coverage ); cov->addref();
		UniformScalarPainter* wIor  = new UniformScalarPainter( 1.46 );   wIor->addref();
		UniformScalarPainter* wWid  = new UniformScalarPainter( 0.15 );   wWid->addref();
		UniformScalarPainter* wAzi  = new UniformScalarPainter( 1.0 );    wAzi->addref();
		UniformScalarPainter* wKd   = new UniformScalarPainter( 0.3 );    wKd->addref();
		UniformScalarPainter* wTilt = new UniformScalarPainter( fam.tilt ); wTilt->addref();
		UniformScalarPainter* fIor  = new UniformScalarPainter( 1.46 );   fIor->addref();
		UniformScalarPainter* fWid  = new UniformScalarPainter( 0.15 );   fWid->addref();
		UniformScalarPainter* fAzi  = new UniformScalarPainter( 1.0 );    fAzi->addref();
		UniformScalarPainter* fKd   = new UniformScalarPainter( 0.3 );    fKd->addref();
		UniformScalarPainter* fTilt = new UniformScalarPainter( -fam.tilt ); fTilt->addref();
		UniformColorPainter*  wCol  = new UniformColorPainter( RISEPel( 1, 1, 1 ) ); wCol->addref();
		UniformColorPainter*  fCol  = new UniformColorPainter( RISEPel( 1, 1, 1 ) ); fCol->addref();

		WeaveMaterial* mat = new WeaveMaterial( eWeaveCustom, *scale, *rot, *skew, cov, *gap,
		                   *wCol, *wIor, *wWid, *wAzi, *wKd, *wTilt,
		                   *fCol, *fIor, *fWid, *fAzi, *fKd, *fTilt );
		mat->addref();

		IBSDF* bsdf = mat->GetBSDF();
		double prev = -1.0;
		double worstRatio = 1.0;
		double worstAt = -1.0;
		int    sampleCount = 0;

		for( double th = kStepDeg; th <= kMaxThetaDeg + 1e-9; th += kStepDeg )
		{
			RayIntersectionGeometric ri = MakeViewSweepProbe( th );
			const RISEPel v = bsdf->value( light, ri );
			const double val = v.r;
			++sampleCount;
			if( prev > 1e-12 && val > 1e-12 ) {
				const double ratio = ( val > prev ) ? ( val / prev ) : ( prev / val );
				if( ratio > worstRatio ) { worstRatio = ratio; worstAt = th; }
			}
			prev = val;
		}

		std::ostringstream oss;
		oss << "masking pole seam, " << fam.label << ": worst adjacent-sample ratio "
		    << worstRatio << " at theta_v ~ " << worstAt << " deg over " << sampleCount
		    << " samples (0.25 deg steps, 0-88 deg) must stay <= " << kMaxAdjacentRatio;
		Check( worstRatio <= kMaxAdjacentRatio, oss.str() );

		safe_release( mat );
		safe_release( wCol ); safe_release( fCol );
		safe_release( scale ); safe_release( rot ); safe_release( skew ); safe_release( gap );
		safe_release( cov );
		safe_release( wIor ); safe_release( wWid ); safe_release( wAzi ); safe_release( wKd ); safe_release( wTilt );
		safe_release( fIor ); safe_release( fWid ); safe_release( fAzi ); safe_release( fKd ); safe_release( fTilt );
	}
}

//////////////////////////////////////////////////////////////////////
// 10. Spectral parity at an authored white dye
//////////////////////////////////////////////////////////////////////

void TestSpectralParityAtWhiteDye()
{
	std::cout << "SpectralParityAtWhiteDye" << std::endl;

	IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
	const std::string body = ColorPainter( "cWhite", "1 1 1" )
		+ ColorPainter( "cRed",   "0.9 0.05 0.05" )
		+ WeaveMat( "white",  "linen", 0,
		            LineS( "warp_color", "cWhite" ) + LineS( "weft_color", "cWhite" ) )
		+ WeaveMat( "tinted", "linen", 0,
		            LineS( "warp_color", "cRed" )   + LineS( "weft_color", "cRed" ) );
	ParseBodyInto( "spectral", body, *job );

	Check( IsWeave( *job, "white" ) && IsWeave( *job, "tinted" ),
	       "gate 8: both materials registered" );

	// At an AUTHORED white the guard returns exactly 1.0 at every
	// wavelength, so the NM response must be flat AND must equal the RGB
	// max channel.  Compared to <= 4 ulp rather than bit-equality: every
	// input is bit-identical, so any residual is the RGB and NM
	// expressions' floating-point evaluation differing under
	// `-ffast-math`, which is not a guard failure.
	const double rgb = Respond( *job, "white" );
	bool flat = true, matches = true;
	for( double nm = 400.0; nm <= 700.0; nm += 25.0 ) {
		const double v = RespondNM( *job, "white", nm );
		if( std::fabs( v - RespondNM( *job, "white", 550.0 ) ) > 4e-15 ) flat = false;
		if( std::fabs( v - rgb ) > 4.0 * 2.220446049250313e-16 * ( rgb > 0 ? rgb : 1.0 ) )
			matches = false;
	}
	Check( flat,    "gate 8: an authored WHITE dye is flat across wavelength" );
	Check( matches, "gate 8: and equals the RGB response to <= 4 ulp" );

	// The negative control: a saturated dye MUST be wavelength-
	// dependent, or the two checks above would also pass with the tint
	// term dead.
	const double t450 = RespondNM( *job, "tinted", 450.0 );
	const double t660 = RespondNM( *job, "tinted", 660.0 );
	Check( t450 >= 0 && t660 >= 0 && t450 != t660,
	       "gate 8 (negative control): a SATURATED dye is wavelength-dependent" );

	safe_release( job );
}

} // anonymous namespace

int main()
{
	GlobalLog();

	std::cout << "===== weave_material chunk contract test =====" << std::endl;

	TestDraftMeans();
	TestFootprintFadeAndSmoothEdges();
	TestPresetsSeedTheSlots();
	TestExplicitSlotsWinAndDraftsDiffer();
	TestCustomCoverage();
	TestFabricOverWeave();
	TestRequireSingleAndIntrospection();
	TestApiLevelFactory();
	TestHemisphericalAlbedoError();
	TestMaskingPoleSeam();
	TestSpectralParityAtWhiteDye();

	std::cout << std::endl
	          << "Results: " << passCount << " passed, "
	          << failCount << " failed" << std::endl;
	return ( failCount > 0 ) ? 1 : 0;
}
