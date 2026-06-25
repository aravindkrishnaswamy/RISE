//////////////////////////////////////////////////////////////////////
//
//  ExpressionFieldFinitenessTest.cpp - the nan/inf guard on the in-scene
//  expression field (ExpressionEval / expression_function2d) must SURVIVE
//  the production build's -O3 -flto -ffast-math.
//
//  Background: fast-math licenses the optimizer to assume every double is
//  finite, which folds isnan/isinf -- and a naive union/memcpy exponent
//  test -- to a constant `true`.  That made ExpressionProgram::IsFinite a
//  DEAD guard: a non-finite expression-field value could flow straight into
//  a displacement bake (corrupting a vertex) or a procedural texture.  The
//  fix launders the value through a `volatile` load before the exponent
//  test, stripping the finiteness assumption so the test cannot be folded.
//
//  This test pins that behaviour at four levels:
//    1. ExpressionProgram::IsFinite directly rejects genuine runtime nan/inf
//       (incl. a value PRODUCED by fast-math FP overflow) and accepts finite.
//    2. ExpressionFunction2DPainter (built through the public API) clamps a
//       non-finite field value to 0 in Evaluate / GetColorNM / GetColor.
//    3. A real DisplacedGeometry bake driven by a non-finite field bakes the
//       plain (un-displaced) sphere instead of nan/inf vertices.
//    4. The parser rejects a non-finite `param` (expression_function2d -- a
//       String param guarded by the hardened IsFinite at the Finalize site) and
//       a non-finite `disp_scale` (displaced_geometry -- a Double param guarded
//       by the descriptor's compiler-opaque string scan).  These are the two
//       inputs to the bake multiply, each protected by its appropriate layer.
//
//  Regression guard: with the pre-fix union-based IsFinite, tests 1, 2 and 3
//  FAIL (the un-laundered test reports the nan/inf as finite) -- verified by
//  temporarily reverting IsFinite during development.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/ISceneParser.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IFunction2D.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Geometry/DisplacedGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Utilities/BoundingBox.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

// The test's OWN finiteness oracle -- the SAME volatile-launder trick the fix
// uses, replicated here so the assertions do not call the very function under
// test.  A naive bit test here would fold under -ffast-math just like the bug;
// the Test-1 sanity assertion below is a tripwire if this ever stops working.
static bool TestFinite( double x )
{
	volatile double vd = x;
	const double d = vd;
	std::uint64_t bits;
	std::memcpy( &bits, &d, sizeof( bits ) );
	return ( ( bits >> 52 ) & 0x7FFull ) != 0x7FFull;
}

// Materialise a genuine runtime non-finite from a known IEEE-754 bit pattern,
// laundered through a volatile so -ffast-math cannot constant-fold the setup.
static double FromBits( std::uint64_t bits )
{
	volatile std::uint64_t vb = bits;
	const std::uint64_t b = vb;
	double d;
	std::memcpy( &d, &b, sizeof( d ) );
	return d;
}

static const std::uint64_t QNAN = 0x7FF8000000000000ull;
static const std::uint64_t PINF = 0x7FF0000000000000ull;
static const std::uint64_t NINF = 0xFFF0000000000000ull;

//====================================================================
//  Test 1: ExpressionProgram::IsFinite under the production flags
//====================================================================
static void TestIsFiniteDirect()
{
	std::cout << "Test 1: ExpressionProgram::IsFinite survives -O3 -flto -ffast-math" << std::endl;

	// Sanity / tripwire: the constructed inputs really ARE non-finite (test's own oracle).
	Check( !TestFinite( FromBits( QNAN ) ) && !TestFinite( FromBits( PINF ) ) && !TestFinite( FromBits( NINF ) ),
		"oracle: constructed qNaN / +Inf / -Inf are genuinely non-finite" );

	// Non-finite must be REJECTED.  The pre-fix union version folds these to
	// `finite` and FAILS here -- this is the core regression guard.
	Check( !ExpressionProgram::IsFinite( FromBits( QNAN ) ), "qNaN rejected" );
	Check( !ExpressionProgram::IsFinite( FromBits( PINF ) ), "+Inf rejected" );
	Check( !ExpressionProgram::IsFinite( FromBits( NINF ) ), "-Inf rejected" );

	// A value PRODUCED by fast-math FP arithmetic (the real hot-path scenario):
	// the optimizer would otherwise stamp the multiply result `finite`.
	{
		volatile double a = 1e300, b = 1e300;
		const double prod = (double)a * (double)b;	// -> +Inf at runtime
		Check( !ExpressionProgram::IsFinite( prod ), "fast-math overflow (1e300*1e300) rejected" );
	}

	// Finite must be ACCEPTED -- no over-rejection of legitimate field values.
	{ volatile double z = 0.0;     Check( ExpressionProgram::IsFinite( (double)z ),   "0 accepted" ); }
	{ volatile double h = 3.5;     Check( ExpressionProgram::IsFinite( (double)h ),   "3.5 accepted" ); }
	{ volatile double n = -1.0;    Check( ExpressionProgram::IsFinite( (double)n ),   "-1 accepted" ); }
	{ volatile double big = 1e300; Check( ExpressionProgram::IsFinite( (double)big ), "1e300 (large but finite) accepted" ); }
	Check( ExpressionProgram::IsFinite( FromBits( 1ull ) ), "smallest denormal accepted" );
}

//====================================================================
//  Test 2: ExpressionFunction2DPainter clamps a non-finite field to 0
//====================================================================
static IPainter* MakeExprPainter( const char* finalExpr )
{
	ExpressionProgram prog = ExpressionProgram::Invalid();
	ExpressionProgram::Builder b;
	if( !b.Finalize( finalExpr, prog ) || !prog.IsValid() ) return 0;
	IPainter* p = 0;
	if( !RISE_API_CreateExpressionFunction2D( &p, prog ) ) return 0;
	return p;
}

static void TestPainterClamp()
{
	std::cout << "Test 2: ExpressionFunction2DPainter clamps a non-finite field value to 0" << std::endl;

	// nan, +inf, -inf -- all produced at RUNTIME by the stack machine (not folded).
	const char* expr[3]  = { "log(-1)",      "1e300 * 1e300",     "log(0)" };
	const char* label[3] = { "log(-1)->nan", "1e300*1e300->+inf", "log(0)->-inf" };

	for( int i = 0; i < 3; ++i ) {
		IPainter* p = MakeExprPainter( expr[i] );
		Check( p != 0, std::string( "painter compiles: " ) + label[i] );
		if( !p ) continue;

		// Evaluate(): the exact per-vertex call the displacement bake makes.
		const double e = p->Evaluate( 0.3, 0.7 );
		Check( TestFinite( e ) && e == 0.0, std::string( "Evaluate clamps " ) + label[i] );

		// GetColorNM(): the spectral / scalar-painter sampling path.
		RayIntersectionGeometric ri( Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), nullRasterizerState );
		ri.ptCoord = Point2( 0.3, 0.7 );
		const double nm = p->GetColorNM( ri, 550.0 );
		Check( TestFinite( nm ) && nm == 0.0, std::string( "GetColorNM clamps " ) + label[i] );

		// GetColor(): the RGB procedural-texture path (R==G==B==Safe value).
		const RISEPel col = p->GetColor( ri );
		Check( TestFinite( col[0] ) && col[0] == 0.0, std::string( "GetColor clamps " ) + label[i] );

		p->release();
	}

	// Control: a FINITE field must pass through UNCHANGED (no over-clamping).
	{
		IPainter* p = MakeExprPainter( "0.5" );
		Check( p != 0, "finite painter compiles" );
		if( p ) {
			const double e = p->Evaluate( 0.3, 0.7 );
			Check( TestFinite( e ) && std::fabs( e - 0.5 ) < 1e-12, "finite field 0.5 passes through (no over-clamp)" );
			p->release();
		}
	}
}

//====================================================================
//  Test 3: the real displacement bake clamps a non-finite field
//====================================================================
static BoundingBox BakeSphereBBox( const Scalar R, const unsigned int detail, const IFunction2D* field, const Scalar scale, bool& builtOut )
{
	SphereGeometry* sph = new SphereGeometry( R );
	DisplacedGeometry* disp = new DisplacedGeometry( sph, detail, field, scale, false, false );
	disp->Realize();	// bakes the mesh (single-threaded path, as the render pipeline does in AttachScene)
	builtOut = disp->IsValid();
	const BoundingBox bb = disp->GenerateBoundingBox();
	disp->release();
	sph->release();
	return bb;
}

static void TestDisplacementBake()
{
	std::cout << "Test 3: displacement bake with a non-finite field -> plain (un-displaced) mesh" << std::endl;

	const Scalar R = 1.0;
	const unsigned int DETAIL = 8;

	// Reference: the plain tessellated sphere (NULL displacement).  A non-finite
	// field clamped to 0 must bake EXACTLY this (zero displacement), so we compare
	// against it rather than a coarse magnitude bound -- the latter would silently
	// depend on the BVH's RISE_INFINITY seed turning a leaked NaN into an out-of-
	// range bbox (CLAUDE.md "ffast-math: no infinity" warns that seed may change).
	bool refBuilt = false;
	const BoundingBox ref = BakeSphereBBox( R, DETAIL, 0, 0.0, refBuilt );
	Check( refBuilt && ref.ur.x > ref.ll.x, "reference plain sphere baked + non-degenerate bbox" );

	// Non-finite field everywhere -> Safe() clamps to 0 -> zero displacement ->
	// the baked mesh must match the plain tessellated sphere.
	IPainter* badField = MakeExprPainter( "log(-1)" );	// nan everywhere
	Check( badField != 0, "bad-field painter compiles" );
	if( badField ) {
		bool built = false;
		const BoundingBox bb = BakeSphereBBox( R, DETAIL, badField, 0.3, built );
		Check( built, "displaced geometry valid after bake" );
		Check( bb.ur.x > bb.ll.x, "baked bbox is non-degenerate (a real mesh was built, not a null/empty bake)" );

		const bool finite =
			TestFinite( bb.ll.x ) && TestFinite( bb.ll.y ) && TestFinite( bb.ll.z ) &&
			TestFinite( bb.ur.x ) && TestFinite( bb.ur.y ) && TestFinite( bb.ur.z );
		Check( finite, "baked bbox is finite (non-finite field did NOT corrupt vertices)" );

		// Decoupled from the BVH seed: a leaked nan/inf bakes a DBL_MAX/Inf bbox
		// that cannot match the unit-sphere reference, regardless of seeding.
		const Scalar tol = 1e-6;
		const bool matchesPlain =
			std::fabs( bb.ll.x - ref.ll.x ) < tol && std::fabs( bb.ur.x - ref.ur.x ) < tol &&
			std::fabs( bb.ll.y - ref.ll.y ) < tol && std::fabs( bb.ur.y - ref.ur.y ) < tol &&
			std::fabs( bb.ll.z - ref.ll.z ) < tol && std::fabs( bb.ur.z - ref.ur.z ) < tol;
		Check( matchesPlain, "non-finite field bakes the PLAIN sphere (clamp to 0 = no displacement)" );

		badField->release();
	}

	// Control: a FINITE field of 0.5 with scale 0.4 displaces OUTWARD ~0.2,
	// proving the bake path is genuinely exercised (the bbox grows past the ref).
	IPainter* goodField = MakeExprPainter( "0.5" );
	Check( goodField != 0, "good-field painter compiles" );
	if( goodField ) {
		bool built = false;
		const BoundingBox bb = BakeSphereBBox( R, DETAIL, goodField, 0.4, built );
		Check( TestFinite( bb.ur.x ) && TestFinite( bb.ll.x ), "control: finite-field baked bbox is finite" );
		Check( bb.ur.x > ref.ur.x + 0.05, "control: finite field actually displaced the surface outward (bake exercised)" );
		goodField->release();
	}
}

//====================================================================
//  Test 4: the parser rejects non-finite `param` and `disp_scale`
//          (the two production callers of IsFinite + the bake's
//           two multiply inputs), and accepts the finite forms.
//====================================================================
namespace {
	std::string WriteTempScene( const std::string& tag, const std::string& body )
	{
		const char* tmp = getenv( "TMPDIR" );
		std::string dir = tmp ? tmp : "/tmp/";
		if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
		const std::string path = dir + "rise_field_finiteness_" + tag + ".RISEscene";
		std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
		f << body;
		f.close();
		return path;
	}

	// Parse an inline scene body through the REAL AsciiSceneParser; returns
	// ParseAndLoadScene's verdict (Job is Reference-counted: heap + release).
	bool ParseBody( const std::string& tag, const std::string& body )
	{
		const std::string path = WriteTempScene( tag, "RISE ASCII SCENE 6\n" + body );
		Job* job = new Job();
		job->addref();
		ISceneParser* parser = 0;
		bool ok = false;
		if( RISE_API_CreateAsciiSceneParser( &parser, path.c_str() ) && parser ) {
			parser->addref();
			ok = parser->ParseAndLoadScene( *job );
			parser->release();
		}
		job->release();
		remove( path.c_str() );
		return ok;
	}
}

static void TestParserGuards()
{
	std::cout << "Test 4: parser rejects non-finite param / disp_scale (errors below are EXPECTED)" << std::endl;

	// expression_function2d `param` -- the AsciiSceneParser finite check that was
	// previously dead under -ffast-math.  A non-finite literal must be refused.
	Check( !ParseBody( "param_inf",
		"expression_function2d\n{\nname f\nparam k inf\nexpr u + k\n}\n" ),
		"param: non-finite `param k inf` is REJECTED" );
	Check( ParseBody( "param_ok",
		"expression_function2d\n{\nname f\nparam k 2.0\nexpr u + k\n}\n" ),
		"param: finite `param k 2.0` is accepted (control)" );

	// displaced_geometry `disp_scale` -- the other input to the bake multiply
	// `field * scale`.  It is a ValueKind::Double param, so the descriptor-driven
	// parser rejects a non-finite value via its compiler-opaque string scan
	// (AllTokensAreFiniteNumbers), which is robust under -ffast-math by construction.
	// Pin that end-to-end so the bake's scale input stays protected too.
	const char* kSphere = "sphere_geometry\n{\nname s\nradius 1\n}\n";
	Check( !ParseBody( "scale_inf",
		std::string( kSphere ) + "displaced_geometry\n{\nname d\nbase_geometry s\ndetail 4\ndisp_scale inf\n}\n" ),
		"disp_scale: non-finite `disp_scale inf` is REJECTED" );
	Check( ParseBody( "scale_ok",
		std::string( kSphere ) + "displaced_geometry\n{\nname d\nbase_geometry s\ndetail 4\ndisp_scale 0.3\n}\n" ),
		"disp_scale: finite `disp_scale 0.3` is accepted (control)" );
}

int main( int, char** )
{
	std::cout << "ExpressionFieldFinitenessTest -- nan/inf field guard under -ffast-math" << std::endl << std::endl;
	TestIsFiniteDirect();
	TestPainterClamp();
	TestDisplacementBake();
	TestParserGuards();
	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
