//////////////////////////////////////////////////////////////////////
//
//  SurfaceSignalsTest.cpp - Phase-2 exit gate for the geometry-derived
//  shading signals arc (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §13).
//
//  Phase 2 is `occlusion(radius)` and `thickness(radius)`: two
//  arg-taking expression builtins, answered per hit through the
//  ISurfaceSignalProvider channel on RayIntersectionGeometric, by the
//  SDF family's own distance field.
//
//  What this pins, in the order the design doc's exit gate lists it:
//
//    (a) occlusion ~= 1 EVERYWHERE on an isolated convex SDF sphere --
//        the estimator's zero point.  A convex body occludes nothing,
//        and an SDF's own field says so exactly (map(p + h*n) == h),
//        so this is a real check, not a tolerance-shaped one.
//    (b) occlusion < 1 inside a modeled CREVICE, and ~= 1 on the same
//        object's convex body.  The crevice is the crease where two
//        overlapping spheres meet -- a genuine concave wedge.  (A
//        SPHERICAL pit would NOT darken, and that is a property of the
//        Evans estimator worth knowing rather than a bug: inside a
//        sphere the distance to the wall is exactly the step, so the
//        field reports no shortfall.  Creases, corners and folds are
//        what it sees, which is what a cavity mask is for.)
//    (c) thickness MONOTONE in a slab's width across three widths, and
//        saturating at 1 once the slab is at least as thick as the
//        query radius.
//    (d) the NEUTRAL FALLBACKS: an analytic-primitive hit publishes no
//        provider, and an unusable (computed, non-positive) radius is
//        refused -- both must read occlusion 1 and thickness 1, the
//        do-nothing end of each range.
//    (e) PARSE-TIME rejection: wrong arity, a non-positive literal
//        radius, a vec3 argument, and the builtins on the frozen
//        UV-only surface (expression_function2d) that must not grow
//        them.
//    (f) the CONSTANT-RADIUS record Phase 3's baked mesh path will read
//        (design doc §7.1) -- literal detected, computed not claimed,
//        `def`-bound deliberately NOT resolved (Phase-3 work).
//    (g) END TO END through the painter API:
//        `scalar_painter { expression "occlusion(...)" }`.
//    (h) VM CONCURRENCY: one compiled program, many threads, each with
//        its own context carrying the live provider -- same answer as
//        the serial evaluation, every time.
//    (i) the RADIUS-FRACTION semantics: two instances of ONE geometry
//        at different world scales read the SAME occlusion at
//        corresponding points.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <vector>
#include <string>
#include <thread>
#include <atomic>

#include "../src/Library/Interfaces/ISurfaceSignalProvider.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionPainter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

static void CheckClose( Scalar got, Scalar want, Scalar tol, const std::string& name )
{
	if( std::fabs( got - want ) <= tol ) { ++passCount; }
	else {
		++failCount;
		std::cout.precision( 12 );
		std::cout << "  FAIL: " << name << "  got " << got << "  want " << want
			<< "  |d| " << std::fabs( got - want ) << "  (tol " << tol << ")" << std::endl;
	}
}

//======================================================================
// Helpers
//======================================================================

static RayIntersection MkRI( const Point3& origin, const Vector3& dir )
{
	return RayIntersection( Ray( origin, dir ), nullRasterizerState );
}

static bool HitObject( const Object* obj, RayIntersection& ri )
{
	obj->IntersectRay( ri, RISE_INFINITY, true, true, false );
	return ri.geometric.bHit;
}

//! One SDF sphere, alone in its own field: the canonical convex body.
static SDFGeometry* BuildSdfSphere( const Scalar radius )
{
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), radius, 0, 0, 0 ) );
	return new SDFGeometry( parts, 512, Scalar( 1e-5 ) );
}

//! TWO overlapping unit spheres offset along X.  Their union has a
//! genuine concave CREASE around the circle x = 0, |(y,z)| = sqrt(1-off^2)
//! -- a wedge, which is exactly the feature a cavity mask exists to find.
//! The far tips (+-(1+off), 0, 0) stay purely convex, so one geometry
//! carries both the positive and the negative control.
static SDFGeometry* BuildSdfCreasedPair( const Scalar off )
{
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3( -off, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 1.0, 0, 0, 0 ) );
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3( off, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 1.0, 0, 0, 0 ) );
	return new SDFGeometry( parts, 512, Scalar( 1e-5 ) );
}

//! A wide slab of thickness `w` centred on the origin, its faces normal
//! to Z.  Wide in X/Y on purpose: the bounding-box diagonal (which the
//! radius fraction is taken OF) is then dominated by the 10x10 face and
//! barely moves as `w` varies, so a monotonicity check across widths is
//! comparing thicknesses at essentially one query radius.
static SDFGeometry* BuildSdfSlab( const Scalar w )
{
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 5.0, 5.0, w * Scalar(0.5), 0 ) );
	return new SDFGeometry( parts, 512, Scalar( 1e-5 ) );
}

//! Compile one expression body with the full 3D context enabled -- the
//! configuration expression_painter / scalar_painter{expression} use.
static bool CompileWithContext( const std::string& body, ExpressionProgram& out )
{
	ExpressionProgram::Builder b;
	b.EnableContextVars( true );
	return b.Finalize( body, out );
}

//! Compile WITHOUT the 3D context -- the frozen expression_function2d
//! surface.
static bool CompileUvOnly( const std::string& body, ExpressionProgram& out )
{
	ExpressionProgram::Builder b;
	return b.Finalize( body, out );
}

//! Evaluate `body` at the first hit of a world-space ray against `obj`,
//! through the SAME ExprEvalContext the painters build.  Returns false
//! when the ray misses or the body doesn't compile.
static bool EvalAtHit( const Object* obj, const Point3& origin, const Vector3& dir,
	const std::string& body, Scalar& outValue )
{
	ExpressionProgram prog = ExpressionProgram::Invalid();
	if( !CompileWithContext( body, prog ) ) return false;

	std::vector<ParamSpec> specs;
	ExpressionScalarPainter* painter = new ExpressionScalarPainter( prog, specs );

	RayIntersection ri = MkRI( origin, dir );
	const bool hit = HitObject( obj, ri );
	if( hit ) {
		outValue = painter->GetValuesAt( ri.geometric ).v[0];
	}
	painter->release();
	return hit;
}

//======================================================================
// (a) An isolated convex SDF sphere is unoccluded everywhere
//======================================================================

static void TestConvexSphereIsUnoccluded()
{
	std::cout << "(a) isolated convex SDF sphere: occlusion ~= 1 everywhere" << std::endl;

	SDFGeometry* g = BuildSdfSphere( 2.0 );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	// Six directions, none down a symmetry-free axis by accident: the
	// estimator must read the same on all of them.
	const Vector3 dirs[6] = {
		Vector3( 0, 0, -1 ), Vector3( 0, 0, 1 ),
		Vector3( -1, 0, 0 ), Vector3( 1, 0, 0 ),
		Vector3( 0, -1, 0 ), Vector3( 0, 1, 0 )
	};
	const Point3 origins[6] = {
		Point3( 0.3, 0.4, 30 ), Point3( 0.3, 0.4, -30 ),
		Point3( 30, 0.5, 0.2 ), Point3( -30, 0.5, 0.2 ),
		Point3( 0.2, 30, 0.4 ), Point3( 0.2, -30, 0.4 )
	};

	for( int i = 0; i < 6; ++i ) {
		Scalar ao = 0;
		Check( EvalAtHit( o, origins[i], dirs[i], "occlusion(0.15)", ao ),
			"(a) convex sphere hit " + std::to_string( i ) );
		CheckClose( ao, 1.0, 0.02, "(a) occlusion ~= 1 on the convex sphere, ray " + std::to_string( i ) );
	}

	// And a much larger radius fraction must not change the verdict -- a
	// convex body has nothing to occlude at ANY scale.  This is what
	// separates "correctly 1" from "the taps were too short to notice".
	Scalar aoWide = 0;
	Check( EvalAtHit( o, Point3( 0.3, 0.4, 30 ), Vector3( 0, 0, -1 ), "occlusion(0.6)", aoWide ),
		"(a) convex sphere hit, wide radius" );
	CheckClose( aoWide, 1.0, 0.02, "(a) occlusion ~= 1 at a 0.6 radius fraction too" );

	o->release();
}

//======================================================================
// (b) A modeled crevice reads occluded; the same object's convex body
//     does not
//======================================================================

static void TestCreviceDarkensAndConvexDoesNot()
{
	std::cout << "(b) modeled crevice: occlusion < 1 in the crease, ~= 1 on the convex body" << std::endl;

	const Scalar off = 0.7;
	SDFGeometry* g = BuildSdfCreasedPair( off );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	// Straight down +Y at x = 0: lands on the crease circle.
	Scalar aoCrease = 0;
	Check( EvalAtHit( o, Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ), "occlusion(0.2)", aoCrease ),
		"(b) crease hit" );
	// The far tip of the +X lobe: purely convex, and part of the SAME field,
	// so this is a within-object control rather than a different scene.
	Scalar aoTip = 0;
	Check( EvalAtHit( o, Point3( 30, 0, 0 ), Vector3( -1, 0, 0 ), "occlusion(0.2)", aoTip ),
		"(b) convex tip hit" );

	Check( aoCrease < Scalar( 0.9 ), "(b) crease reads OCCLUDED (< 0.9)" );
	Check( aoCrease > Scalar( 0.0 ), "(b) crease occlusion stays in range (> 0)" );
	CheckClose( aoTip, 1.0, 0.02, "(b) convex tip reads UNOCCLUDED (~= 1)" );
	Check( aoTip - aoCrease > Scalar( 0.1 ),
		"(b) the signal DISCRIMINATES: tip minus crease > 0.1" );

	// A crevice mask is `1 - occlusion(r)`; sanity-check that the composed
	// form an author would actually write comes out the right way round.
	Scalar mask = 0;
	Check( EvalAtHit( o, Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ), "clamp(1 - occlusion(0.2), 0, 1)", mask ),
		"(b) crevice mask evaluates" );
	Check( mask > Scalar( 0.1 ), "(b) crevice mask LIGHTS UP in the crease" );

	o->release();
}

//======================================================================
// (c) thickness is monotone in a slab's width, saturating at 1
//======================================================================

static void TestThicknessMonotoneInSlabWidth()
{
	std::cout << "(c) thickness monotone in slab width, saturating at 1" << std::endl;

	const Scalar widths[3] = { 0.25, 0.6, 1.2 };
	Scalar got[3] = { 0, 0, 0 };

	for( int i = 0; i < 3; ++i ) {
		SDFGeometry* g = BuildSdfSlab( widths[i] );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();

		Check( EvalAtHit( o, Point3( 0.3, -0.2, 30 ), Vector3( 0, 0, -1 ), "thickness(0.1)", got[i] ),
			"(c) slab hit, width " + std::to_string( i ) );
		Check( got[i] > Scalar( 0 ) && got[i] < Scalar( 1 ),
			"(c) slab " + std::to_string( i ) + " reads strictly between 0 and 1" );
		o->release();
	}

	Check( got[0] < got[1], "(c) thickness(0.25-wide) < thickness(0.6-wide)" );
	Check( got[1] < got[2], "(c) thickness(0.6-wide) < thickness(1.2-wide)" );

	// The query radius here is 0.1 * diag, and diag ~= sqrt(10^2+10^2+w^2)
	// ~= 14.15, so R ~= 1.415 and the reported value should be w/R.
	CheckClose( got[0], widths[0] / Scalar( 1.415 ), 0.03, "(c) thin slab reads ~ w/R" );
	CheckClose( got[2], widths[2] / Scalar( 1.415 ), 0.03, "(c) thick slab reads ~ w/R" );

	// A slab at least as thick as the query radius saturates at exactly 1 --
	// "no far side within the radius" is a MEASUREMENT of thickness, not an
	// absence, so it must report the saturated value rather than fall back.
	{
		SDFGeometry* g = BuildSdfSlab( 4.0 );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();

		Scalar sat = 0;
		Check( EvalAtHit( o, Point3( 0.3, -0.2, 30 ), Vector3( 0, 0, -1 ), "thickness(0.1)", sat ),
			"(c) thick slab hit" );
		CheckClose( sat, 1.0, 1e-9, "(c) width >= query radius saturates at exactly 1" );

		// ... and the SAME slab read at a much larger radius is no longer
		// saturated, which proves the saturation above came from the ratio
		// and not from a stuck constant.
		Scalar unsat = 0;
		Check( EvalAtHit( o, Point3( 0.3, -0.2, 30 ), Vector3( 0, 0, -1 ), "thickness(0.9)", unsat ),
			"(c) thick slab hit, wide radius" );
		Check( unsat < Scalar( 0.9 ), "(c) the same slab is NOT saturated at a wide radius" );
		o->release();
	}
}

//======================================================================
// (d) neutral fallbacks: no provider, and an unusable radius
//======================================================================

static void TestNeutralFallbacks()
{
	std::cout << "(d) neutral fallbacks (no provider; unusable radius)" << std::endl;

	// --- No provider: an analytic primitive publishes none.
	{
		SphereGeometry* g = new SphereGeometry( 2.0 );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();

		RayIntersection ri = MkRI( Point3( 0.3, 0.4, 30 ), Vector3( 0, 0, -1 ) );
		Check( HitObject( o, ri ), "(d) analytic sphere hit" );
		Check( ri.geometric.signals.pProvider == 0,
			"(d) an analytic primitive publishes NO signal provider" );

		Scalar ao = 0, th = 0;
		Check( EvalAtHit( o, Point3( 0.3, 0.4, 30 ), Vector3( 0, 0, -1 ), "occlusion(0.2)", ao ),
			"(d) occlusion evaluates on a provider-less hit" );
		Check( EvalAtHit( o, Point3( 0.3, 0.4, 30 ), Vector3( 0, 0, -1 ), "thickness(0.2)", th ),
			"(d) thickness evaluates on a provider-less hit" );
		CheckClose( ao, 1.0, 1e-12, "(d) NO PROVIDER -> occlusion 1 (unoccluded)" );
		CheckClose( th, 1.0, 1e-12, "(d) NO PROVIDER -> thickness 1 (thick: nothing lights up)" );

		o->release();
	}

	// --- Unusable radius, on geometry that DOES have a provider and DOES
	//     read occluded at a good radius.  Written as a computed expression
	//     (`0-0.1`) rather than a literal, because a literal is a compile
	//     error -- see (e).
	{
		SDFGeometry* g = BuildSdfCreasedPair( 0.7 );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();

		Scalar good = 0, zero = 0, neg = 0, thNeg = 0;
		Check( EvalAtHit( o, Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ), "occlusion(0.2)", good ),
			"(d) crease hit, usable radius" );
		Check( good < Scalar( 0.9 ), "(d) ... and it really is occluded there (non-vacuous)" );

		Check( EvalAtHit( o, Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ), "occlusion(u*0)", zero ),
			"(d) zero radius evaluates" );
		Check( EvalAtHit( o, Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ), "occlusion(0-0.1)", neg ),
			"(d) negative radius evaluates" );
		Check( EvalAtHit( o, Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ), "thickness(0-0.1)", thNeg ),
			"(d) negative radius evaluates (thickness)" );

		CheckClose( zero, 1.0, 1e-12, "(d) radius 0 -> neutral occlusion 1" );
		CheckClose( neg, 1.0, 1e-12, "(d) negative radius -> neutral occlusion 1" );
		CheckClose( thNeg, 1.0, 1e-12, "(d) negative radius -> neutral thickness 1" );

		o->release();
	}
}

//======================================================================
// (e) parse-time diagnostics
//======================================================================

static void ExpectCompileFailure( const std::string& body, const std::string& what )
{
	ExpressionProgram prog = ExpressionProgram::Invalid();
	const bool ok = CompileWithContext( body, prog );
	Check( !ok, "(e) REJECTED at compile time: " + what );
	if( !ok ) {
		Check( !prog.Error().empty(), "(e) ... with a non-empty diagnostic: " + what );
	}
}

static void TestParseTimeRejection()
{
	std::cout << "(e) parse-time arity / range / type / surface gating" << std::endl;

	ExpectCompileFailure( "occlusion()", "occlusion() with no argument" );
	ExpectCompileFailure( "thickness()", "thickness() with no argument" );
	ExpectCompileFailure( "occlusion(0.1, 0.2)", "occlusion() with two arguments" );
	ExpectCompileFailure( "occlusion(-0.1)", "a NEGATIVE literal radius" );
	ExpectCompileFailure( "occlusion(0)", "a ZERO literal radius" );
	ExpectCompileFailure( "thickness(-1)", "a negative literal radius (thickness)" );
	ExpectCompileFailure( "occlusion(P)", "a vec3 radius argument" );

	// The positive control: the same shapes that must be accepted.
	{
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( CompileWithContext( "occlusion(0.05)", prog ), "(e) occlusion(0.05) COMPILES" );
		Check( CompileWithContext( "thickness(0.05) * occlusion(0.2)", prog ),
			"(e) both builtins in one body COMPILE" );
		// A COMPUTED radius is legal -- only a literal is range-checked,
		// because only a literal can be checked without evaluating.
		Check( CompileWithContext( "occlusion(u * 0.2 + 0.01)", prog ),
			"(e) a computed radius COMPILES (checked at runtime instead)" );
	}

	// The frozen UV-only surface (expression_function2d) must not grow these.
	{
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( !CompileUvOnly( "occlusion(0.1)", prog ),
			"(e) occlusion() REJECTED on the UV-only surface" );
		Check( !CompileUvOnly( "thickness(0.1)", prog ),
			"(e) thickness() REJECTED on the UV-only surface" );
		// ... and the ordinary UV-only body still compiles there, so the
		// gate above rejected the builtin and not the whole surface.
		Check( CompileUvOnly( "sin(u) * cos(v)", prog ),
			"(e) an ordinary UV-only body still compiles" );
	}
}

//======================================================================
// (f) the constant-radius record Phase 3 will read (design doc §7.1)
//======================================================================

static void TestConstantRadiusRecord()
{
	std::cout << "(f) compile-time constant-radius record for the Phase-3 baked path" << std::endl;

	{
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( CompileWithContext( "u + v", prog ), "(f) plain body compiles" );
		Check( !prog.UsesSurfaceSignals(), "(f) a body with no signal call reports none" );
		Check( prog.SurfaceSignalCalls().empty(), "(f) ... and records no call sites" );
	}

	{
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( CompileWithContext( "occlusion(0.05)", prog ), "(f) literal-radius body compiles" );
		Check( prog.UsesSurfaceSignals(), "(f) UsesSurfaceSignals() true" );
		Check( prog.SurfaceSignalCalls().size() == 1, "(f) exactly one call site recorded" );
		if( prog.SurfaceSignalCalls().size() == 1 ) {
			const ExpressionProgram::SignalRadiusCall& c = prog.SurfaceSignalCalls()[0];
			Check( c.fn == ExpressionProgram::kFnOcclusion, "(f) recorded as occlusion" );
			Check( c.radiusIsLiteral, "(f) LITERAL radius detected" );
			CheckClose( c.radiusLiteral, 0.05, 1e-12, "(f) literal value captured" );
		}
	}

	{
		// Two calls, in parse order, one of each kind.
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( CompileWithContext( "occlusion(0.05) + thickness(0.3)", prog ), "(f) two-call body compiles" );
		Check( prog.SurfaceSignalCalls().size() == 2, "(f) two call sites recorded" );
		if( prog.SurfaceSignalCalls().size() == 2 ) {
			Check( prog.SurfaceSignalCalls()[0].fn == ExpressionProgram::kFnOcclusion,
				"(f) first call site is the occlusion one (parse order)" );
			Check( prog.SurfaceSignalCalls()[1].fn == ExpressionProgram::kFnThickness,
				"(f) second call site is the thickness one" );
			CheckClose( prog.SurfaceSignalCalls()[1].radiusLiteral, 0.3, 1e-12,
				"(f) second literal value captured" );
		}
	}

	{
		// A COMPUTED radius must NOT be claimed constant.
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( CompileWithContext( "occlusion(u * 0.2 + 0.01)", prog ), "(f) computed-radius body compiles" );
		Check( prog.SurfaceSignalCalls().size() == 1, "(f) one call site recorded" );
		if( prog.SurfaceSignalCalls().size() == 1 ) {
			Check( !prog.SurfaceSignalCalls()[0].radiusIsLiteral,
				"(f) a COMPUTED radius is not claimed literal" );
		}
	}

	{
		// A `def`-bound constant radius is genuinely constant, but resolving
		// a NAME back to a constant is explicitly Phase-3 work (design doc
		// §7.1) -- Phase 2 must report "not proven", never guess.
		ExpressionProgram::Builder b;
		b.EnableContextVars( true );
		Check( b.AddDef( "r", "0.05" ), "(f) def r compiles" );
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( b.Finalize( "occlusion(r)", prog ), "(f) def-bound-radius body compiles" );
		Check( prog.SurfaceSignalCalls().size() == 1, "(f) one call site recorded" );
		if( prog.SurfaceSignalCalls().size() == 1 ) {
			Check( !prog.SurfaceSignalCalls()[0].radiusIsLiteral,
				"(f) a def-bound radius is NOT resolved in Phase 2 (documented Phase-3 scope)" );
		}
	}

	{
		// A call inside a `def` STAGE is recorded too -- the record must
		// cover every body this builder compiled, not just the final one.
		ExpressionProgram::Builder b;
		b.EnableContextVars( true );
		Check( b.AddDef( "cav", "1 - occlusion(0.08)" ), "(f) def with a signal call compiles" );
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( b.Finalize( "cav * 0.5", prog ), "(f) final expr compiles" );
		Check( prog.UsesSurfaceSignals(), "(f) a def-stage call is seen by UsesSurfaceSignals()" );
		Check( prog.SurfaceSignalCalls().size() == 1, "(f) the def-stage call site is recorded" );
	}
}

//======================================================================
// (g) end to end through the painter API
//======================================================================

static void TestEndToEndThroughPainter()
{
	std::cout << "(g) end to end: scalar_painter { expression \"occlusion(...)\" }" << std::endl;

	SDFGeometry* g = BuildSdfCreasedPair( 0.7 );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	ExpressionProgram prog = ExpressionProgram::Invalid();
	Check( CompileWithContext( "occlusion(0.2)", prog ), "(g) body compiles" );
	std::vector<ParamSpec> specs;
	ExpressionScalarPainter* painter = new ExpressionScalarPainter( prog, specs );

	RayIntersection crease = MkRI( Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ) );
	RayIntersection tip    = MkRI( Point3( 30, 0, 0 ), Vector3( -1, 0, 0 ) );
	Check( HitObject( o, crease ), "(g) crease hit" );
	Check( HitObject( o, tip ), "(g) tip hit" );

	const ScalarTriple tCrease = painter->GetValuesAt( crease.geometric );
	const ScalarTriple tTip    = painter->GetValuesAt( tip.geometric );

	Check( tCrease.v[0] < Scalar( 0.9 ), "(g) painter reports the crease occluded" );
	CheckClose( tTip.v[0], 1.0, 0.02, "(g) painter reports the tip unoccluded" );
	// A scalar-typed program broadcasts uniformly -- pin it, since a slot
	// that reads only .v[1] must see the same number.
	CheckClose( tCrease.v[1], tCrease.v[0], 1e-12, "(g) scalar program broadcasts uniformly (G)" );
	CheckClose( tCrease.v[2], tCrease.v[0], 1e-12, "(g) scalar program broadcasts uniformly (B)" );

	// The COLOUR pipe must agree with the physical-scalar pipe: both
	// BuildContext twins have to populate the channel, and this is the check
	// that catches only one of them being wired.
	ExpressionPainter* colour = new ExpressionPainter( prog, specs, Scalar(0) );
	const RISEPel c = colour->GetColor( crease.geometric );
	CheckClose( c[0], tCrease.v[0], 1e-12, "(g) the COLOUR pipe agrees with the scalar pipe" );
	colour->release();

	painter->release();
	o->release();
}

//======================================================================
// (h) VM concurrency against one compiled program
//======================================================================

static void TestConcurrentEvaluation()
{
	std::cout << "(h) many threads, ONE compiled program, live provider" << std::endl;

	SDFGeometry* g = BuildSdfCreasedPair( 0.7 );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	ExpressionProgram prog = ExpressionProgram::Invalid();
	Check( CompileWithContext( "occlusion(0.2) + thickness(0.15)", prog ), "(h) body compiles" );
	std::vector<ParamSpec> specs;
	ExpressionScalarPainter* painter = new ExpressionScalarPainter( prog, specs );

	RayIntersection crease = MkRI( Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ) );
	Check( HitObject( o, crease ), "(h) crease hit" );

	// The serial reference, computed before any thread starts.
	const Scalar reference = painter->GetValuesAt( crease.geometric ).v[0];
	Check( reference > Scalar( 0 ), "(h) the reference value is non-trivial" );

	const int kThreads = 8;
	const int kIters   = 400;
	std::atomic<int> mismatches( 0 );
	std::atomic<int> evaluations( 0 );

	std::vector<std::thread> pool;
	for( int t = 0; t < kThreads; ++t ) {
		pool.push_back( std::thread( [&]() {
			for( int i = 0; i < kIters; ++i ) {
				// Each thread builds its OWN record copy, exactly as each
				// render thread owns its own hit -- but they all share one
				// compiled program AND one SDFGeometry provider.
				RayIntersectionGeometric local = crease.geometric;
				const Scalar v = painter->GetValuesAt( local ).v[0];
				if( std::fabs( v - reference ) > Scalar( 1e-12 ) ) {
					mismatches.fetch_add( 1, std::memory_order_relaxed );
				}
				evaluations.fetch_add( 1, std::memory_order_relaxed );
			}
		} ) );
	}
	for( size_t i = 0; i < pool.size(); ++i ) {
		pool[i].join();
	}

	Check( evaluations.load() == kThreads * kIters, "(h) every concurrent evaluation ran" );
	Check( mismatches.load() == 0, "(h) every concurrent evaluation matched the serial value" );

	painter->release();
	o->release();
}

//======================================================================
// (i) radius-as-a-FRACTION: scale invariance across instances
//======================================================================

static void TestRadiusFractionScaleInvariance()
{
	std::cout << "(i) radius is a FRACTION: same value on two instances at different world scales" << std::endl;

	// ONE geometry, two objects -- the instancing case the fraction
	// semantics exist for.
	SDFGeometry* g = BuildSdfCreasedPair( 0.7 );

	Object* oA = new Object( g );
	oA->FinalizeTransformations();

	Object* oB = new Object( g );
	oB->SetScale( 4.0 );
	oB->FinalizeTransformations();
	g->release();

	Scalar aoA = 0, aoB = 0;
	Check( EvalAtHit( oA, Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ), "occlusion(0.2)", aoA ),
		"(i) instance A crease hit" );
	// Corresponding point on the 4x instance: the same object-space place.
	Check( EvalAtHit( oB, Point3( 0, 120, 0 ), Vector3( 0, -1, 0 ), "occlusion(0.2)", aoB ),
		"(i) instance B crease hit" );

	Check( aoA < Scalar( 0.9 ), "(i) instance A really is occluded (non-vacuous)" );
	CheckClose( aoB, aoA, 1e-9, "(i) occlusion IDENTICAL at scale 1 and scale 4" );

	// Thickness too -- it is normalized by the same fraction, so it must be
	// scale-invariant for the same reason.
	// 0.9 rather than 0.2: the pair is ~3.4 units through along X and its
	// bbox diagonal is ~4.4, so a 0.2 fraction would saturate at 1 and the
	// comparison would pass on a constant instead of on the signal.
	Scalar thA = 0, thB = 0;
	Check( EvalAtHit( oA, Point3( 30, 0, 0 ), Vector3( -1, 0, 0 ), "thickness(0.9)", thA ),
		"(i) instance A thickness" );
	Check( EvalAtHit( oB, Point3( 120, 0, 0 ), Vector3( -1, 0, 0 ), "thickness(0.9)", thB ),
		"(i) instance B thickness" );
	Check( thA > Scalar( 0 ) && thA < Scalar( 1 ), "(i) instance A thickness is non-saturated" );
	CheckClose( thB, thA, 1e-9, "(i) thickness IDENTICAL at scale 1 and scale 4" );

	oA->release();
	oB->release();
}

//======================================================================

int main()
{
	std::cout << "=== SurfaceSignalsTest (design doc Phase 2: occlusion / thickness) ===" << std::endl;

	TestConvexSphereIsUnoccluded();
	TestCreviceDarkensAndConvexDoesNot();
	TestThicknessMonotoneInSlabWidth();
	TestNeutralFallbacks();
	TestParseTimeRejection();
	TestConstantRadiusRecord();
	TestEndToEndThroughPainter();
	TestConcurrentEvaluation();
	TestRadiusFractionScaleInvariance();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
