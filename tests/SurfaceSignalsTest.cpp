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
//        overlapping spheres meet -- a genuine concave wedge.  (A shallow
//        SPHERICAL pit reads unoccluded, and that is a property of the
//        estimator worth knowing rather than a bug: a ray at angle phi
//        from the inward normal crosses a chord of 2*rho*cos(phi), so
//        nothing is blocked until the query radius approaches the pit's
//        own diameter -- `occlusion = 1 - (R/2rho)^2`, which (k) pins.
//        Creases, corners and folds are what it sees, which is what a
//        cavity mask is for.)
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
//    (j) HEIGHTFIELD MODE REFUSES both signals (global-vs-local Lipschitz
//        bound, Phase-2 fix round P1) rather than reporting the
//        systematically wrong ~1/m_hfLip on a flat, unoccluded point.
//    (k) CSG_SUBTRACTION re-pairs signals.nObject with the flipped
//        composite normal (Phase-2 fix round P1) -- a pocket floor reads
//        occluded and a thin remaining wall reads thin, where pre-fix both
//        read as if standing on the untouched convex/thick exterior.
//    (l) a DYNAMIC (computed) radius that lands POSITIVE is genuinely
//        answered on the SDF family -- the live-evaluation capability
//        the baked mesh family structurally cannot have.  (d) covers
//        the computed-and-non-positive case; this covers the common one.
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
#include "../src/Library/Interfaces/IFunction2D.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/CSGObject.h"
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

//! A heightfield field that is FLAT (f == 0) everywhere except one narrow,
//! steep ridge near u = 0.85 -- built to reproduce the bug this test guards:
//! `Map()` in heightfield mode divides by a single GLOBAL Lipschitz bound
//! (`m_hfLip`, sized to this ridge's steep slope), so a flat point far from
//! it reads a shortfall the Evans estimator misreads as occlusion, even
//! though the true field there is perfectly flat and unoccluded.
class SteepRidgeFunction2D : public virtual IFunction2D, public virtual Reference
{
public:
	Scalar Evaluate( const Scalar u, const Scalar /*v*/ ) const
	{
		const Scalar d = std::fabs( u - Scalar( 0.85 ) );
		const Scalar w = Scalar( 0.02 );
		return d < w ? ( Scalar(1) - d / w ) : Scalar( 0 );
	}
};

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

//! Average of a signal over MANY hits SLID ALONG a feature -- the form the
//! quadrature-dependent closed forms below take since 2026-09-07.
//!
//! WHY THE CLOSED FORMS ARE NO LONGER POINTWISE.  Both SDF estimators pick
//! one of 32 pre-built ROTATIONS of their sample set per hit, keyed on a
//! hash of the hit position (SDFGeometry.cpp, `kSignalRotations`).  That is
//! what let the sample counts come down 2x-2.5x without losing accuracy --
//! a FIXED set has to carry each closed form at whatever orientation the
//! feature happens to present, and it was the orientation, not the count,
//! that the old counts were paying for.  The consequence for a test is
//! precise: a value that depends on the QUADRATURE is now a random variable
//! whose EXPECTATION is the closed form, so it must be checked as an
//! expectation, over hits that differ in position and therefore in rotation.
//!
//! WHAT IS **NOT** WEAKENED.  Every POINTWISE identity still holds at every
//! hit and keeps its 1e-9 / 1e-12 tolerance, because it holds for every
//! rotation individually rather than on average: a plane and every convex
//! feature read occlusion EXACTLY 1 (every outward direction escapes, at
//! any count and any spin), and a plane reads convexity EXACTLY 0 (the
//! rotated point set is still centrally symmetric, so A is still exactly
//! 1/2).  Cases (a), (m), (n) and the `1e-9` lines in (o) and (p) are
//! untouched, and are the reason this change cannot hide a real regression
//! behind an average.
//!
//! `slide` is a SMALL step along the feature -- the arris' own direction,
//! the crease's own direction, a tangent of the sphere -- chosen so the
//! closed form is CONSTANT across the probes and only the rotation varies.
//! It has to be small relative to the query radius (these use 1e-3 against
//! radii of 0.5 and up) and large relative to nothing at all: any change in
//! the low mantissa bits re-rolls the hash.
static bool EvalAtHitMean( const Object* obj, const Point3& origin, const Vector3& dir,
	const Vector3& slide, const int nSamples, const std::string& body,
	Scalar& outMean, Scalar* outSpread = 0 )
{
	ExpressionProgram prog = ExpressionProgram::Invalid();
	if( !CompileWithContext( body, prog ) ) return false;

	std::vector<ParamSpec> specs;
	ExpressionScalarPainter* painter = new ExpressionScalarPainter( prog, specs );

	Scalar sum = 0, lo = Scalar( 1e30 ), hi = Scalar( -1e30 );
	int n = 0;
	for( int i = 0; i < nSamples; ++i ) {
		// Centred on the nominal probe, so the mean is not biased to one
		// side of the feature by the slide itself.
		const Scalar f = Scalar( i ) - Scalar( nSamples - 1 ) * Scalar( 0.5 );
		const Point3 o( origin.x + slide.x*f, origin.y + slide.y*f, origin.z + slide.z*f );
		RayIntersection ri = MkRI( o, dir );
		if( !HitObject( obj, ri ) ) continue;
		const Scalar v = painter->GetValuesAt( ri.geometric ).v[0];
		sum += v;
		if( v < lo ) lo = v;
		if( v > hi ) hi = v;
		++n;
	}
	painter->release();

	// EVERY probe must land, or the mean is over a different set of hits
	// than the one the caller reasoned about.
	if( n != nSamples ) return false;
	outMean = sum / Scalar( n );
	if( outSpread ) *outSpread = hi - lo;
	return true;
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

		Scalar ao = 0, th = 0, cx = 0;
		Check( EvalAtHit( o, Point3( 0.3, 0.4, 30 ), Vector3( 0, 0, -1 ), "occlusion(0.2)", ao ),
			"(d) occlusion evaluates on a provider-less hit" );
		Check( EvalAtHit( o, Point3( 0.3, 0.4, 30 ), Vector3( 0, 0, -1 ), "thickness(0.2)", th ),
			"(d) thickness evaluates on a provider-less hit" );
		Check( EvalAtHit( o, Point3( 0.3, 0.4, 30 ), Vector3( 0, 0, -1 ), "convexity(0.2)", cx ),
			"(d) convexity evaluates on a provider-less hit" );
		CheckClose( ao, 1.0, 1e-12, "(d) NO PROVIDER -> occlusion 1 (unoccluded)" );
		CheckClose( th, 1.0, 1e-12, "(d) NO PROVIDER -> thickness 1 (thick: nothing lights up)" );
		// Convexity's neutral is the OPPOSITE end of its range, and that
		// asymmetry is the point: an absent edge-wear signal must mean "no
		// edge here", never "knife edge everywhere".
		CheckClose( cx, 0.0, 1e-12, "(d) NO PROVIDER -> convexity 0 (flat: nothing lights up)" );

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

		Scalar good = 0, zero = 0, neg = 0, thNeg = 0, cxNeg = 0, cxZero = 0;
		Check( EvalAtHit( o, Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ), "occlusion(0.2)", good ),
			"(d) crease hit, usable radius" );
		Check( good < Scalar( 0.9 ), "(d) ... and it really is occluded there (non-vacuous)" );

		Check( EvalAtHit( o, Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ), "occlusion(u*0)", zero ),
			"(d) zero radius evaluates" );
		Check( EvalAtHit( o, Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ), "occlusion(0-0.1)", neg ),
			"(d) negative radius evaluates" );
		Check( EvalAtHit( o, Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ), "thickness(0-0.1)", thNeg ),
			"(d) negative radius evaluates (thickness)" );

		Check( EvalAtHit( o, Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ), "convexity(0-0.1)", cxNeg ),
			"(d) negative radius evaluates (convexity)" );
		Check( EvalAtHit( o, Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ), "convexity(u*0)", cxZero ),
			"(d) zero radius evaluates (convexity)" );

		CheckClose( zero, 1.0, 1e-12, "(d) radius 0 -> neutral occlusion 1" );
		CheckClose( neg, 1.0, 1e-12, "(d) negative radius -> neutral occlusion 1" );
		CheckClose( thNeg, 1.0, 1e-12, "(d) negative radius -> neutral thickness 1" );
		CheckClose( cxNeg, 0.0, 1e-12, "(d) negative radius -> neutral convexity 0" );
		CheckClose( cxZero, 0.0, 1e-12, "(d) radius 0 -> neutral convexity 0" );

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
	ExpectCompileFailure( "convexity()", "convexity() with no argument" );
	ExpectCompileFailure( "convexity(0.1, 0.2)", "convexity() with two arguments" );
	ExpectCompileFailure( "convexity(0)", "a ZERO literal radius (convexity)" );
	ExpectCompileFailure( "convexity(-0.3)", "a negative literal radius (convexity)" );
	ExpectCompileFailure( "convexity(N)", "a vec3 radius argument (convexity)" );

	// The positive control: the same shapes that must be accepted.
	{
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( CompileWithContext( "occlusion(0.05)", prog ), "(e) occlusion(0.05) COMPILES" );
		Check( CompileWithContext( "thickness(0.05) * occlusion(0.2) + convexity(0.02)", prog ),
			"(e) all three builtins in one body COMPILE" );
		Check( CompileWithContext( "convexity(0.05)", prog ), "(e) convexity(0.05) COMPILES" );
		Check( CompileWithContext( "convexity(u * 0.2 + 0.01)", prog ),
			"(e) a computed convexity radius COMPILES" );
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
		Check( !CompileUvOnly( "convexity(0.1)", prog ),
			"(e) convexity() REJECTED on the UV-only surface" );
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
		// Convexity registers on the SAME record, with its own id -- the
		// baked mesh path keys its table on (kind, radius), so a
		// misrecorded id would bake or read the wrong signal entirely.
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( CompileWithContext( "convexity(0.02) + occlusion(0.05)", prog ),
			"(f) convexity + occlusion body compiles" );
		Check( prog.SurfaceSignalCalls().size() == 2, "(f) two call sites recorded (convexity first)" );
		if( prog.SurfaceSignalCalls().size() == 2 ) {
			Check( prog.SurfaceSignalCalls()[0].fn == ExpressionProgram::kFnConvexity,
				"(f) first call site recorded as CONVEXITY" );
			Check( prog.SurfaceSignalCalls()[0].radiusIsLiteral,
				"(f) convexity LITERAL radius detected" );
			CheckClose( prog.SurfaceSignalCalls()[0].radiusLiteral, 0.02, 1e-12,
				"(f) convexity literal value captured" );
			Check( prog.SurfaceSignalCalls()[1].fn == ExpressionProgram::kFnOcclusion,
				"(f) second call site is still the occlusion one" );
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
// (j) heightfield mode REFUSES occlusion/thickness (global-vs-local
//     Lipschitz bound) rather than reporting the systematically wrong
//     ~1/m_hfLip on a flat, unoccluded point
//======================================================================

static void TestHeightfieldRefusesSignals()
{
	std::cout << "(j) heightfield mode: occlusion/convexity/thickness REFUSE rather than fabricate" << std::endl;

	const Scalar R = 2.0, S = 0.5;
	SteepRidgeFunction2D* field = new SteepRidgeFunction2D();
	SDFGeometry* g = new SDFGeometry( field, R, S, 512, Scalar( 1e-5 ) );
	safe_release( field );	// SDFGeometry addref'd its own ref (Reference starts at refcount 1)

	// Direct provider-level refusal, independent of any hit machinery: a
	// point far from the ridge (u = v = 0.5, the field's flat region) at a
	// query radius that would, pre-fix, read the ridge's global Lipschitz
	// bound as if it were the local one.
	Scalar ao = Scalar( 12345 ), th = Scalar( 12345 ), cx = Scalar( 12345 );
	SurfaceSignalInfo hf;
	hf.pProvider = g;
	hf.ptObject = Point3( 0, 0, S );
	hf.nObject = Vector3( 0, 0, 1 );
	Check( !g->ComputeOcclusion( hf, Scalar( 0.1 ), true, ao ),
		"(j) ComputeOcclusion REFUSES on a heightfield" );
	Check( !g->ComputeThickness( hf, Scalar( 0.1 ), true, th ),
		"(j) ComputeThickness REFUSES on a heightfield" );
	// Convexity shares occlusion's estimator, so it inherits the refusal for
	// the same reason: the smoothed indicator's transition band is expressed
	// in FIELD units, and heightfield mode scales those by a single GLOBAL
	// Lipschitz bound, so the band would be an unrelated multiple of the
	// query radius.  (The SIGN of the field is fine there -- it is only the
	// band that is not -- so this refusal is narrower than the retired
	// estimator's, whose whole magnitude was unusable.)
	Check( !g->ComputeConvexity( hf, Scalar( 0.1 ), true, cx ),
		"(j) ComputeConvexity REFUSES on a heightfield" );
	Check( ao == Scalar( 12345 ), "(j) ComputeOcclusion leaves outValue untouched on refusal" );
	Check( th == Scalar( 12345 ), "(j) ComputeThickness leaves outValue untouched on refusal" );
	Check( cx == Scalar( 12345 ), "(j) ComputeConvexity leaves outValue untouched on refusal" );

	// End-to-end through a real hit + the expression VM: a FLAT point far
	// from the field's one steep ridge must read the NEUTRAL fallback
	// exactly (occlusion 1 = unoccluded, thickness 1 = thick), not the
	// ~1/m_hfLip a global-bound estimator would report on a perfectly flat,
	// unoccluded point (the reviewer's numerical repro: ~0.15 for a ridge
	// steep enough to force m_hfLip ~= 6.5).
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	Scalar aoHit = 0, thHit = 0, cxHit = -1;
	Check( EvalAtHit( o, Point3( 0, 0, 10 ), Vector3( 0, 0, -1 ), "occlusion(0.1)", aoHit ),
		"(j) heightfield flat-point hit evaluates occlusion" );
	Check( EvalAtHit( o, Point3( 0, 0, 10 ), Vector3( 0, 0, -1 ), "thickness(0.1)", thHit ),
		"(j) heightfield flat-point hit evaluates thickness" );
	Check( EvalAtHit( o, Point3( 0, 0, 10 ), Vector3( 0, 0, -1 ), "convexity(0.1)", cxHit ),
		"(j) heightfield flat-point hit evaluates convexity" );
	CheckClose( aoHit, 1.0, 1e-12, "(j) heightfield occlusion == 1 (refusal -> neutral), not ~1/m_hfLip" );
	CheckClose( thHit, 1.0, 1e-12, "(j) heightfield thickness == 1 (refusal -> neutral)" );
	CheckClose( cxHit, 0.0, 1e-12, "(j) heightfield convexity == 0 (refusal -> ITS neutral, the other end)" );

	// Sanity: the same object DOES publish a provider (heightfield hits
	// stamp `signals.pProvider` unconditionally -- only the estimators
	// refuse) -- otherwise this test would vacuously pass via the (d)
	// no-provider fallback path instead of exercising the m_isHeightfield
	// refusal this fix adds.
	RayIntersection riCheck = MkRI( Point3( 0, 0, 10 ), Vector3( 0, 0, -1 ) );
	Check( HitObject( o, riCheck ), "(j) flat-point hit lands" );
	Check( riCheck.geometric.signals.pProvider != 0,
		"(j) heightfield DOES publish a provider (the refusal is in the estimator, not the stamp)" );

	o->release();
}

//======================================================================
// (k) CSG_SUBTRACTION re-pairs signals.nObject with the flipped normal --
//     occlusion/thickness must tap into the EMPTY region the composite's
//     viewer actually stands in (operand B's interior), not back into B's
//     own solid.  Two independent cavity shapes, because occlusion's and
//     thickness's discriminating geometry differ (see the comments below).
//======================================================================

static void TestCsgSubtractionRepairsSignalNormal()
{
	std::cout << "(k) CSG_SUBTRACTION: signals re-paired -- flipped nObject (thickness) AND complemented field sense (occlusion/convexity)" << std::endl;

	// --- occlusion: a spherical pocket carved out of a big sphere. -------
	// The pocket FLOOR (deepest point, farthest from the carved-out
	// sphere's own rim) is where the "inside A, not inside B" branch
	// reports B's own record: exactly the case Fix 2 repairs.  The
	// composite's OWN untouched convex exterior (far from the pocket, hit
	// without ever touching B) is the convex-body control -- same object,
	// same provider FAMILY, no CSG boundary logic involved at all.
	{
		SDFGeometry* gA = BuildSdfSphere( 4.0 );
		Object* oA = new Object( gA );
		safe_release( gA );
		oA->FinalizeTransformations();

		std::vector<SDFGeometry::Part> partsB;
		partsB.push_back( SDFGeometry::MakePart(
			SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3( 0, 0, 2.0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 1.5, 0, 0, 0 ) );
		SDFGeometry* gB = new SDFGeometry( partsB, 512, Scalar( 1e-5 ) );
		Object* oB = new Object( gB );
		safe_release( gB );
		oB->FinalizeTransformations();

		CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
		Check( csg->AssignObjects( oA, oB ), "(k) occlusion: composite takes A(sphere)/B(sphere) operands" );
		csg->FinalizeTransformations();

		// Origin (0,0,-3): inside A (dist 3 < 4), outside B (dist to B's
		// centre (0,0,2) is 5 > 1.5) -- the "inside A, not inside B" branch.
		// Fires straight at B's near (floor) wall at world (0,0,0.5).
		RayIntersection riFloor = MkRI( Point3( 0, 0, -3.0 ), Vector3( 0, 0, 1 ) );
		Check( HitObject( csg, riFloor ), "(k) occlusion: pocket-floor ray hits the composite" );
		CheckClose( riFloor.geometric.range2, 0.0, 1e-9,
			"(k) occlusion: (sanity) range2 == 0 confirms the \"inside A, not inside B\" branch" );
		Check( riFloor.geometric.signals.pProvider != 0, "(k) occlusion: pocket-floor hit carries a provider" );

		// The untouched convex exterior, well away from the pocket: never
		// touches B at all (falls straight to `ri = riObjA`), so it is a
		// control on the SAME object with none of the CSG re-pairing code
		// in play.
		RayIntersection riConvex = MkRI( Point3( 0, 0, 30.0 ), Vector3( 0, 0, -1 ) );
		Check( HitObject( csg, riConvex ), "(k) occlusion: convex-exterior ray hits the composite" );
		Check( riConvex.geometric.signals.pProvider != 0, "(k) occlusion: convex-exterior hit carries a provider" );

		if( riFloor.geometric.signals.pProvider && riConvex.geometric.signals.pProvider ) {
			// 0.4 of B's own bbox diagonal (3*sqrt(3) = 5.196) is a query
			// radius of 2.078 -- comfortably more than the 1.5-radius
			// pocket, so the pocket really does enclose the query.
			Scalar aoFloor = 2, aoConvex = 2;
			Check( riFloor.geometric.signals.pProvider->ComputeOcclusion(
				riFloor.geometric.signals, Scalar( 0.4 ), true, aoFloor ),
				"(k) occlusion: pocket-floor ComputeOcclusion answers" );
			Check( riConvex.geometric.signals.pProvider->ComputeOcclusion(
				riConvex.geometric.signals, Scalar( 0.4 ), true, aoConvex ),
				"(k) occlusion: convex-exterior ComputeOcclusion answers" );

			// MONEY ASSERTION: the pocket floor reads more occluded than
			// the untouched convex exterior.
			//
			// THE MARGIN IS A CLOSED FORM, not a taste.  Occlusion is the
			// cosine-weighted fraction of outward directions that escape
			// distance R.  From a point on the inner wall of a spherical
			// pocket of radius rho, a ray at angle phi from the inward
			// normal crosses a chord of 2*rho*cos(phi) before reaching the
			// far wall, so it is BLOCKED exactly when cos(phi) <= R/(2 rho);
			// the cosine-weighted measure of that set is (R/(2 rho))^2.
			// Hence occlusion = 1 - (R/(2 rho))^2, and with rho = 1.5,
			// R = 2.078 that is 1 - 0.480 = 0.520.
			Check( aoFloor < aoConvex - Scalar( 0.3 ),
				"(k) MONEY -- pocket-floor occlusion is well below the convex exterior's" );
			CheckClose( aoFloor, 0.520, 0.06,
				"(k) occlusion: ... and by the closed form for a spherical pocket, 1 - (R/2rho)^2" );
			CheckClose( aoConvex, 1.0, 0.05, "(k) occlusion: convex exterior reads ~unoccluded" );

			// Simulate the PRE-FIX bug directly and confirm the money
			// assertion above would NOT have held.
			//
			// WHAT "PRE-FIX" MEANS HERE GREW A SECOND HALF.  The original
			// repair negated `signals.nObject`, and that flip is still
			// load-bearing -- occlusion marches from a normal-lifted origin
			// into the outward hemisphere, and thickness marches along -n.
			// But BOTH estimators also read the SIGN of the field to decide
			// what is solid, and no normal flip can reverse a sign.  The
			// sense therefore travels separately, as `bComplementedField`,
			// and this is the bit whose absence must be shown to break the
			// result.
			//
			// Without it, "solid" means the inside of B's carving sphere --
			// which is the POCKET, i.e. exactly the empty region -- so every
			// outward ray reads as blocked at its first sample and the floor
			// collapses to a fully sealed 0.  Not merely wrong: wrong in the
			// direction that looks plausible, since a cavity floor reading
			// black is what one expects to see.
			Scalar aoFloorPreFix = 2;
			SurfaceSignalInfo preFixHit = riFloor.geometric.signals;
			Check( preFixHit.bComplementedField,
				"(k) occlusion: the pocket-floor hit really is flagged complemented (non-vacuous)" );
			preFixHit.bComplementedField = false;
			Check( riFloor.geometric.signals.pProvider->ComputeOcclusion(
				preFixHit, Scalar( 0.4 ), true, aoFloorPreFix ),
				"(k) occlusion: pre-fix-simulated ComputeOcclusion answers" );
			Check( aoFloorPreFix < Scalar( 0.05 ),
				"(k) REGRESSION GUARD -- without the complement flag the floor collapses to a sealed 0" );
			Check( aoFloor - aoFloorPreFix > Scalar( 0.4 ),
				"(k) REGRESSION GUARD -- ... which is nowhere near the closed-form 0.52" );

			// And the same bit governs convexity, in the opposite direction:
			// un-complemented, B's own convex sphere wall reads as an EDGE.
			Scalar cxFloor = -1, cxFloorPreFix = -1;
			Check( riFloor.geometric.signals.pProvider->ComputeConvexity(
				riFloor.geometric.signals, Scalar( 0.4 ), true, cxFloor ),
				"(k) convexity: pocket-floor ComputeConvexity answers" );
			Check( riFloor.geometric.signals.pProvider->ComputeConvexity(
				preFixHit, Scalar( 0.4 ), true, cxFloorPreFix ),
				"(k) convexity: pre-fix-simulated ComputeConvexity answers" );
			CheckClose( cxFloor, 0.0, 1e-9,
				"(k) convexity: a CONCAVE pocket floor reads 0 (it is not an edge)" );
			Check( cxFloorPreFix > Scalar( 0.1 ),
				"(k) REGRESSION GUARD -- without the flag the same wall would be reported as a convex EDGE" );
		}

		csg->release();
		oA->release();
		oB->release();
	}

	// --- thickness: two adjacent pockets sharing one carving operand B, --
	// leaving a genuinely thin membrane of the composite's real solid
	// between them.  This is representable purely from B's own (two-part)
	// field -- no cross-operand knowledge needed -- because the membrane
	// sits entirely OUTSIDE both of B's spheres, bounded on both sides by
	// B's own boundary, exactly like BuildSdfCreasedPair's crease but
	// approached from the subtraction side.
	{
		SDFGeometry* gA = BuildSdfSphere( 5.0 );
		Object* oA = new Object( gA );
		safe_release( gA );
		oA->FinalizeTransformations();

		// Two spheres, radius 1.0, centres 2.1 apart -> a 0.1-wide gap
		// between them (the membrane), both fully inside A (radius 5).
		std::vector<SDFGeometry::Part> partsB;
		partsB.push_back( SDFGeometry::MakePart(
			SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3( -1.05, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 1.0, 0, 0, 0 ) );
		partsB.push_back( SDFGeometry::MakePart(
			SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3( 1.05, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 1.0, 0, 0, 0 ) );
		SDFGeometry* gB = new SDFGeometry( partsB, 512, Scalar( 1e-5 ) );
		Object* oB = new Object( gB );
		safe_release( gB );
		oB->FinalizeTransformations();

		CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
		Check( csg->AssignObjects( oA, oB ), "(k) thickness: composite takes A(sphere)/B(two spheres) operands" );
		csg->FinalizeTransformations();

		// Origin (0,0,0): inside A, outside both of B's spheres (the gap
		// between them) -- "inside A, not inside B" again.  Fires at
		// sphere1's near wall (its side FACING sphere2, world x=-0.05),
		// where post-fix nObject points back toward sphere1's own pocket,
		// so the thickness march (-nObject) crosses the membrane TOWARD
		// sphere2 -- the physically real thin wall.
		RayIntersection riThin = MkRI( Point3( 0, 0, 0 ), Vector3( -1, 0, 0 ) );
		Check( HitObject( csg, riThin ), "(k) thickness: membrane-side ray hits the composite" );
		CheckClose( riThin.geometric.range2, 0.0, 1e-9,
			"(k) thickness: (sanity) range2 == 0 confirms the \"inside A, not inside B\" branch" );

		// Far side of sphere1 (world x=-2.05), isolated from sphere2 in the
		// march direction -- the "thick body" control, same operand B.
		RayIntersection riThick = MkRI( Point3( -4, 0, 0 ), Vector3( 1, 0, 0 ) );
		Check( HitObject( csg, riThick ), "(k) thickness: isolated-side ray hits the composite" );

		if( riThin.geometric.signals.pProvider && riThick.geometric.signals.pProvider ) {
			const Scalar RF = Scalar( 0.05 );
			Scalar thThin = 2, thThick = 2;
			Check( riThin.geometric.signals.pProvider->ComputeThickness(
				riThin.geometric.signals, RF, true, thThin ),
				"(k) thickness: membrane ComputeThickness answers" );
			Check( riThick.geometric.signals.pProvider->ComputeThickness(
				riThick.geometric.signals, RF, true, thThick ),
				"(k) thickness: isolated-side ComputeThickness answers" );

			// MONEY ASSERTION: the thin remaining wall reads (much)
			// thinner than the thick, isolated body.
			Check( thThin < thThick - Scalar( 0.3 ),
				"(k) MONEY -- membrane thickness is well below the isolated body's" );
			CheckClose( thThick, 1.0, 1e-9, "(k) thickness: isolated side saturates at 1 (thick)" );

			// Pre-fix simulation: the un-repaired direction marches INTO
			// sphere1's own solid (through its centre, out the FAR side) --
			// a chord far longer than the query radius, so it saturates to
			// the SAME 1 the thick-body control reads, entirely missing the
			// real membrane a few hundredths of a unit away.
			const Vector3 preFixN(
				-riThin.geometric.signals.nObject.x,
				-riThin.geometric.signals.nObject.y,
				-riThin.geometric.signals.nObject.z );
			Scalar thThinPreFix = 2;
			SurfaceSignalInfo preFixHit = riThin.geometric.signals;
			preFixHit.nObject = preFixN;
			Check( riThin.geometric.signals.pProvider->ComputeThickness(
				preFixHit, RF, true, thThinPreFix ),
				"(k) thickness: pre-fix-simulated ComputeThickness answers" );
			Check( !( thThinPreFix < thThick - Scalar( 0.3 ) ),
				"(k) REGRESSION GUARD -- the un-repaired (pre-fix) direction would NOT have detected the thin membrane" );
		}

		csg->release();
		oA->release();
		oB->release();
	}
}

//======================================================================
// (l) a DYNAMIC (computed) radius that lands positive is really answered
//     on the SDF family -- the capability the mesh family cannot have
//======================================================================

static void TestDynamicRadiusAnsweredOnSdf()
{
	std::cout << "(l) SDF: a COMPUTED radius is evaluated, not refused" << std::endl;

	// (d) already pins that a computed radius which lands NON-POSITIVE reads
	// the neutral value.  What was never pinned is the far more common case:
	// a computed radius that lands positive must be genuinely ANSWERED here.
	// It matters because it is exactly where the two provider families
	// diverge -- the SDF evaluates live and can take any expression, while a
	// baked mesh commits to one radius per table and refuses anything the
	// compiler did not prove literal (MeshSignalBakeTest (g)).  Without this
	// check, a regression that made the SDF adopt the mesh's refusal rule
	// would leave every test green while silently flattening every
	// expression-driven cavity mask in the wild to 1.
	SDFGeometry* g = BuildSdfCreasedPair( 0.7 );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	// Prime: the crease hit must publish a provider, or every reading below
	// would be the (d) no-provider neutral and this test would pass on
	// nothing at all.
	RayIntersection riPrime = MkRI( Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ) );
	Check( HitObject( o, riPrime ), "(l) the crease ray hits" );
	Check( riPrime.geometric.signals.pProvider != 0,
		"(l) the crease hit publishes a provider (not the no-provider fallback path)" );

	// `u` at that hit, so the dynamic expression's value is knowable and the
	// comparison below is against a NUMBER rather than against itself.
	Scalar uAtHit = -1;
	Check( EvalAtHit( o, Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ), "u", uAtHit ), "(l) u evaluates at the crease" );

	Scalar aoDynamic = -1;
	Check( EvalAtHit( o, Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ), "occlusion(u*0.2+0.1)", aoDynamic ),
		"(l) the dynamic-radius query evaluates" );
	Check( aoDynamic < Scalar( 0.9 ),
		"(l) MONEY -- a computed radius gets a REAL occluded answer on an SDF crevice, not the neutral 1" );

	// And it is the answer the SAME radius spelled as a literal gives: the
	// dynamic path is a different route to one number, not a different
	// estimator.
	const Scalar radius = uAtHit * Scalar( 0.2 ) + Scalar( 0.1 );
	Scalar aoLiteral = -1;
	Check( EvalAtHit( o, Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ),
		"occlusion(" + std::to_string( radius ) + ")", aoLiteral ), "(l) the literal-radius twin evaluates" );
	CheckClose( aoDynamic, aoLiteral, 1e-6, "(l) dynamic and literal spellings of one radius agree" );

	o->release();
}

//======================================================================
// (m) THE PLANAR REFERENCE IS EXACT
//
// The load-bearing property of the accessibility estimator
// (docs/OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md §3a), asserted at 1e-12
// rather than at a tolerance band because it is a SYMMETRY, not a
// convergence: the ball point set is centrally symmetric about the query
// point, so its two halves straddle ANY plane through it and the empty
// fraction comes out at exactly 1/2 whatever the orientation, whatever
// the sample count.  If a future change reintroduces a tangent frame, or
// breaks the odd symmetry of the smoothed indicator, this is the check
// that says so -- and it is the ONLY reason occlusion can promise "a
// merely convex edge reads exactly the neutral 1".
//======================================================================

//! One big axis-aligned box, so its faces are genuinely planar over any
//! query radius the tests below use.
static SDFGeometry* BuildSdfBox( const Scalar hx, const Scalar hy, const Scalar hz,
	const Scalar round )
{
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart(
		( round > 0 ) ? SDFGeometry::ePrimRoundBox : SDFGeometry::ePrimBox,
		SDFGeometry::eOpUnion, 0,
		Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), hx, hy, hz, round ) );
	return new SDFGeometry( parts, 512, Scalar( 1e-5 ) );
}

static void TestPlanarReferenceIsExact()
{
	std::cout << "(m) planar reference: occlusion EXACTLY 1 and convexity EXACTLY 0 on a flat face" << std::endl;

	// Deliberately NOT symmetric in its extents, and queried on three
	// different faces, so a lattice that happened to be exact for only one
	// orientation could not pass.
	SDFGeometry* g = BuildSdfBox( 10.0, 3.0, 7.0, 0.0 );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	struct Face { Point3 from; Vector3 dir; Vector3 slide; const char* name; };
	const Face faces[3] = {
		{ Point3( 0.7, 40, -1.3 ), Vector3( 0, -1, 0 ), Vector3( 1e-5, 0, 0 ), "+Y" },
		{ Point3( 40, 0.4, 1.1 ),  Vector3( -1, 0, 0 ), Vector3( 0, 1e-5, 0 ), "+X" },
		{ Point3( -2.1, 0.9, 40 ), Vector3( 0, 0, -1 ), Vector3( 1e-5, 0, 0 ), "+Z" }
	};
	// Several radii: the identity must not depend on how much of the box the
	// ball covers, only on the face being locally planar.
	const char* radii[3] = { "0.005", "0.02", "0.05" };

	// SWEPT ACROSS ROTATIONS, and the ZERO SPREAD is the real assertion.
	// Since 2026-09-07 each hit draws one of 32 pre-built rotations of the
	// sample set by a hash of its position, so a single probe would pin the
	// identity for ONE rotation and say nothing about the other 31.  128
	// probes slid along the face by 1e-5 -- geometrically nothing, but every
	// one a fresh hash -- cover the table many times over, and requiring the
	// max-minus-min to be EXACTLY zero is a stronger statement than the mean
	// being 1: it says no rotation moved the answer at all, which is what
	// "every outward direction escapes over a plane" and "the rotated point
	// set is still centrally symmetric" actually claim.
	for( int f = 0; f < 3; ++f ) {
		for( int r = 0; r < 3; ++r ) {
			Scalar ao = -1, cx = -1, aoSpread = -1, cxSpread = -1;
			const std::string face( faces[f].name ), rad( radii[r] );
			Check( EvalAtHitMean( o, faces[f].from, faces[f].dir, faces[f].slide, 128,
				"occlusion(" + rad + ")", ao, &aoSpread ),
				"(m) occlusion evaluates on face " + face + " at r=" + rad );
			Check( EvalAtHitMean( o, faces[f].from, faces[f].dir, faces[f].slide, 128,
				"convexity(" + rad + ")", cx, &cxSpread ),
				"(m) convexity evaluates on face " + face + " at r=" + rad );
			CheckClose( ao, 1.0, 1e-12, "(m) EXACT: occlusion == 1 on face " + face + " at r=" + rad );
			CheckClose( cx, 0.0, 1e-12, "(m) EXACT: convexity == 0 on face " + face + " at r=" + rad );
			CheckClose( aoSpread, 0.0, 0.0,
				"(m) POINTWISE, NOT ON AVERAGE: occlusion identical across 128 rotations on face " + face + " at r=" + rad );
			CheckClose( cxSpread, 0.0, 0.0,
				"(m) POINTWISE, NOT ON AVERAGE: convexity identical across 128 rotations on face " + face + " at r=" + rad );
		}
	}

	o->release();
}

//======================================================================
// (n) RED-PROOF: the convex-edge residual the old estimator had
//
// This is the regression that must never come back.  The retired Evans
// normal-line estimator read a SHORTFALL of |Map| along the normal, which
// requires Map to be the exact Euclidean distance.  RISE composes CSG
// `intersect` / `subtract` with a hard `max`, and the max of two half-space
// distances is NOT the distance to their intersection anywhere outside the
// solid: at a convex edge whose two faces' normals are 2*gamma apart,
// Map(p + h*n) = h*cos(gamma) exactly, so the old estimator returned
// `ao = cos(gamma)` -- 0.7071 at a plain 90-degree arris, for every query
// radius and every fillet radius, on surface with no cavity at all.  Worse,
// it returned that SAME 0.7071 in a concave 90-degree valley, so convex and
// concave were literally the same number.
//
// Both halves are asserted: the arris must now read the neutral 1, AND it
// must be well separated from the concave valley (the old estimator's
// separation was exactly ZERO).
//======================================================================

//! A 90-degree convex arris built the way the bug requires: as a CSG
//! INTERSECTION of two slabs, so the field is a hard `max` at the edge.
//! (A single `roundbox` part would NOT reproduce it -- its own field is
//! exact outside, which is precisely why plank_closeup's board never showed
//! the artefact its header once blamed for it.)
static SDFGeometry* BuildSdfIntersectedArris()
{
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 3.0, 1.0, 3.0, 0 ) );
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpIntersect, 0,
		Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 3.0, 3.0, 1.0, 0 ) );
	return new SDFGeometry( parts, 512, Scalar( 1e-5 ) );
}

//! A CONCAVE 90-degree valley along X: two boxes unioned so their faces meet
//! in an inside corner along the line y = 0, z = 0.
static SDFGeometry* BuildSdfValley()
{
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3( 0, -1, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 4.0, 1.0, 4.0, 0 ) );
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3( 0, 1.0, -4.0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 4.0, 4.0, 4.0, 0 ) );
	return new SDFGeometry( parts, 512, Scalar( 1e-5 ) );
}

static void TestConvexCsgEdgeHasNoOcclusionResidual()
{
	std::cout << "(n) RED-PROOF: a CSG convex arris reads occlusion 1 (was 0.7071), and is separated from a concave valley" << std::endl;

	// --- the convex arris
	SDFGeometry* gi = BuildSdfIntersectedArris();
	Object* oi = new Object( gi );
	gi->release();
	oi->FinalizeTransformations();

	// The solid is 6 x 2 x 2, diagonal sqrt(36+4+4) = 6.6332; this radius
	// fraction makes the query radius 0.5 -- a quarter of the solid's
	// thickness, so the arris really is locally a 90-degree wedge.
	const std::string rf = "0.0753778";	// 0.5 / 6.6332

	Scalar aoFace = -1, aoArris = -1, cxFace = -1, cxArris = -1;
	Check( EvalAtHit( oi, Point3( 0.4, 30, 0.3 ), Vector3( 0, -1, 0 ), "occlusion(" + rf + ")", aoFace ),
		"(n) intersected-box FACE hit" );
	Check( EvalAtHit( oi, Point3( 0.4, 30, 30 ), Vector3( 0, -1, -1 ), "occlusion(" + rf + ")", aoArris ),
		"(n) intersected-box ARRIS hit" );
	Check( EvalAtHit( oi, Point3( 0.4, 30, 0.3 ), Vector3( 0, -1, 0 ), "convexity(" + rf + ")", cxFace ),
		"(n) intersected-box FACE convexity" );
	Check( EvalAtHit( oi, Point3( 0.4, 30, 30 ), Vector3( 0, -1, -1 ), "convexity(" + rf + ")", cxArris ),
		"(n) intersected-box ARRIS convexity" );

	CheckClose( aoFace, 1.0, 1e-9, "(n) the flat face reads occlusion 1" );
	// THE MONEY ASSERTION.  Pre-fix this was cos(45 deg) = 0.70711.
	Check( aoArris >= Scalar( 0.99 ),
		"(n) MONEY -- a merely CONVEX CSG arris reads occlusion ~1 (the old estimator returned cos45 = 0.7071)" );
	CheckClose( cxFace, 0.0, 1e-9, "(n) the flat face reads convexity 0" );
	Check( cxArris > Scalar( 0.4 ),
		"(n) ... and the arris is REPORTED as an edge by convexity instead (> 0.4)" );

	// --- the concave valley: the discrimination the old estimator lacked
	SDFGeometry* gv = BuildSdfValley();
	Object* ov = new Object( gv );
	gv->release();
	ov->FinalizeTransformations();

	// Valley bbox spans x[-4,4], y[-2,5], z[-8,4] -> 8 x 7 x 12, diagonal
	// 16.031; this fraction gives the same 0.5 query radius.
	const std::string rfv = "0.0311896";	// 0.5 / 16.031
	Scalar aoCrease = -1, cxCrease = -1;
	Check( EvalAtHit( ov, Point3( 0.5, 30, 0.02 ), Vector3( 0, -1, 0 ), "occlusion(" + rfv + ")", aoCrease ),
		"(n) valley crease hit" );
	Check( EvalAtHit( ov, Point3( 0.5, 30, 0.02 ), Vector3( 0, -1, 0 ), "convexity(" + rfv + ")", cxCrease ),
		"(n) valley crease convexity" );

	// Truth for a 90-degree concave valley is 0.5: the empty opening is a
	// quarter turn, so the accessibility is 1/4 and occlusion is 2*A.
	CheckClose( aoCrease, 0.5, 0.10, "(n) the concave 90-degree valley reads occlusion ~0.5" );
	CheckClose( cxCrease, 0.0, 1e-9, "(n) ... and convexity 0 there (occlusion owns that half of the range)" );

	// THE SECOND MONEY ASSERTION.  The retired estimator returned 0.7071 at
	// BOTH of these, so this difference was exactly 0 and the two features
	// were indistinguishable.
	Check( aoArris - aoCrease > Scalar( 0.4 ),
		"(n) MONEY -- convex arris and concave valley now DIFFER by > 0.4 (the old estimator's difference was 0)" );

	ov->release();
	oi->release();
}

//======================================================================
// (o) convexity's canonical values -- every one of them a closed form
//
// The whole point of defining the signal as an accessibility is that the
// numbers are derivable rather than tuned.  On a wedge of solid dihedral
// angle theta the empty ball fraction is A = 1 - theta/(2*pi), so:
//   90-degree arris   theta = pi/2 -> A = 3/4 -> convexity = 0.50
//   three-face corner theta = pi/4 -> A = 7/8 -> convexity = 0.75
//======================================================================

static void TestConvexityCanonicalValues()
{
	std::cout << "(o) convexity closed forms: 0.5 at a 90-degree arris, 0.75 at a three-face corner" << std::endl;

	// A rounded cube: 4 x 4 x 4 with a 0.05 fillet, so its arris is a real
	// (renderable) edge rather than an infinitely sharp one.
	SDFGeometry* g = BuildSdfBox( 2.0, 2.0, 2.0, 0.05 );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	// diagonal = 4*sqrt(3) = 6.9282; 0.5 / 6.9282 = 0.0721688
	const std::string rf = "0.0721688";

	// EXPECTATIONS, over 128 hits slid along each feature -- see
	// EvalAtHitMean's own doc for why the quadrature-dependent readings can
	// no longer be checked at a single point.  The slides are 1e-5, six
	// orders below the 0.5 query radius and four below the 0.05 fillet, so
	// the closed form is constant across the probes and only the rotation
	// moves.  The arris slides ALONG the arris (it runs in x); the corner is
	// a point, so its probes crawl a fraction of a fillet-width around it.
	const Vector3 slideX( 1e-5, 0, 0 );
	Scalar cxFace = -1, cxArris = -1, cxCorner = -1;
	Scalar aoFace = -1, aoArris = -1, aoCorner = -1;
	Scalar aoFaceSpread = -1, aoArrisSpread = -1, aoCornerSpread = -1;
	Check( EvalAtHitMean( o, Point3( 0.3, 30, -0.2 ), Vector3( 0, -1, 0 ), slideX, 128, "convexity(" + rf + ")", cxFace ),
		"(o) rounded-box face" );
	Check( EvalAtHitMean( o, Point3( 0.3, 30, 30 ), Vector3( 0, -1, -1 ), slideX, 128, "convexity(" + rf + ")", cxArris ),
		"(o) rounded-box arris" );
	Check( EvalAtHitMean( o, Point3( 30, 30, 30 ), Vector3( -1, -1, -1 ), slideX, 128, "convexity(" + rf + ")", cxCorner ),
		"(o) rounded-box corner" );
	Check( EvalAtHitMean( o, Point3( 0.3, 30, -0.2 ), Vector3( 0, -1, 0 ), slideX, 128, "occlusion(" + rf + ")", aoFace, &aoFaceSpread ),
		"(o) rounded-box face occlusion" );
	Check( EvalAtHitMean( o, Point3( 0.3, 30, 30 ), Vector3( 0, -1, -1 ), slideX, 128, "occlusion(" + rf + ")", aoArris, &aoArrisSpread ),
		"(o) rounded-box arris occlusion" );
	Check( EvalAtHitMean( o, Point3( 30, 30, 30 ), Vector3( -1, -1, -1 ), slideX, 128, "occlusion(" + rf + ")", aoCorner, &aoCornerSpread ),
		"(o) rounded-box corner occlusion" );

	CheckClose( cxFace, 0.0, 1e-9, "(o) face convexity is 0" );
	// The band is 0.08, not 0.02, and the reason is geometry rather than
	// estimator error: the query point sits on the FILLET, inset from the
	// ideal sharp corner along the bisector by round*(sqrt(2)-1), which puts
	// marginally more solid inside the ball than a sharp wedge would and
	// pulls the reading a few percent below the sharp-wedge closed form.
	CheckClose( cxArris, 0.5, 0.08, "(o) 90-degree arris convexity ~= 0.5 (closed form, expectation over 128 rotations)" );
	CheckClose( cxCorner, 0.75, 0.10, "(o) three-face corner convexity ~= 0.75 (closed form, expectation over 128 rotations)" );
	Check( cxCorner > cxArris + Scalar( 0.1 ),
		"(o) ORDERING -- a corner is more convex than an arris, by a clear margin" );

	// RED-PROOF for the two expectations above: an estimator that had drifted
	// onto the WRONG closed form would still sit inside its own band, so pin
	// that each reading is FAR from the other feature's answer.  Without this
	// a corner reading the arris' 0.5 (or the reverse) passes both lines.
	Check( std::fabs( cxArris - Scalar( 0.75 ) ) > Scalar( 0.10 ),
		"(o) RED-PROOF: the arris expectation does NOT also satisfy the corner's closed form" );
	Check( std::fabs( cxCorner - Scalar( 0.5 ) ) > Scalar( 0.08 ),
		"(o) RED-PROOF: the corner expectation does NOT also satisfy the arris' closed form" );

	// Every one of these is convex, so NONE of them may darken occlusion.
	// This is (n)'s claim restated on an EXACT-field primitive, which matters
	// because that is the shape the old estimator got right -- so it pins
	// that the rewrite did not break the case that already worked.  POINTWISE
	// still, not on average: the spread across the 128 rotations must be
	// exactly zero (see (m)).
	CheckClose( aoFace, 1.0, 1e-9, "(o) face occlusion is 1" );
	CheckClose( aoArris, 1.0, 1e-9, "(o) arris occlusion is 1" );
	CheckClose( aoCorner, 1.0, 1e-9, "(o) corner occlusion is 1" );
	CheckClose( aoFaceSpread, 0.0, 0.0, "(o) face occlusion is 1 under EVERY rotation" );
	CheckClose( aoArrisSpread, 0.0, 0.0, "(o) arris occlusion is 1 under EVERY rotation" );
	CheckClose( aoCornerSpread, 0.0, 0.0, "(o) corner occlusion is 1 under EVERY rotation" );

	o->release();
}

//======================================================================
// (p) convexity(r) on an SDF sphere, as a function of r/R
//
// The strongest pin available, because the answer is a LINE.  For a query
// ball of radius R centred on a sphere of radius rho (R <= 2*rho), the
// two-sphere lens volume at centre distance rho gives
//   A = 1/2 + 3R/(16*rho),  hence  convexity = 2A - 1 = 3R/(8*rho).
// A regression in the sample weighting, in the radius mapping
// (rho = u^(1/3), i.e. uniform BY VOLUME) or in the band width shows up
// here as a slope or offset error, none of which the wedge cases catch.
//======================================================================

static void TestConvexityOnSphereClosedForm()
{
	std::cout << "(p) convexity on an SDF sphere follows 3R/(8 rho)" << std::endl;

	const Scalar rho = 1.0;
	SDFGeometry* g = BuildSdfSphere( rho );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	// A unit sphere's bounding box is 2 x 2 x 2, diagonal 2*sqrt(3).
	const Scalar diag = Scalar( 2 ) * std::sqrt( Scalar( 3 ) );
	const Scalar Rs[3] = { Scalar( 0.25 ), Scalar( 0.5 ), Scalar( 1.0 ) };

	// EXPECTATIONS again (EvalAtHitMean's doc): 128 probes slid 1e-5 along a
	// tangent of the sphere, where the closed form is constant by symmetry
	// and only the rotation moves.  The tolerance stays 0.02 -- rotation
	// averaging does not need a looser band, it needs a mean: modelled over
	// 64 rotations the reading is 0.185 / 0.178 / 0.183 / 0.181 at 8 / 16 /
	// 24 / 40 pairs against 0.1875, i.e. flat in the count to well inside
	// this band, where the FIXED 32-pair set read 0.159 on this fixture.
	const Vector3 slideX( 1e-5, 0, 0 );
	Scalar got[3] = { -1, -1, -1 };
	for( int i = 0; i < 3; ++i ) {
		const std::string rf = std::to_string( Rs[i] / diag );
		Check( EvalAtHitMean( o, Point3( 0.0, 30, 0.0 ), Vector3( 0, -1, 0 ), slideX, 128,
			"convexity(" + rf + ")", got[i] ),
			"(p) sphere convexity evaluates at R/rho=" + std::to_string( Rs[i] ) );
		const Scalar want = Scalar( 3 ) * Rs[i] / ( Scalar( 8 ) * rho );
		CheckClose( got[i], want, 0.02,
			"(p) convexity == 3R/(8 rho) at R/rho=" + std::to_string( Rs[i] ) );
		// RED-PROOF: the band is 0.02 and the three closed forms are 0.094,
		// 0.188, 0.375 apart, so a reading that had collapsed onto a
		// NEIGHBOURING radius' answer -- the failure a slope or offset error
		// in the radius mapping actually produces -- must be rejected.
		if( i > 0 ) {
			const Scalar wrong = Scalar( 3 ) * Rs[i-1] / ( Scalar( 8 ) * rho );
			Check( std::fabs( got[i] - wrong ) > Scalar( 0.02 ),
				"(p) RED-PROOF: R/rho=" + std::to_string( Rs[i] )
				+ " does NOT also satisfy the previous radius' closed form" );
		}
	}

	// Strict monotonicity: a bigger query ball sees more of the sphere fall
	// away, so it must read more convex.  Independent of the constant above.
	Check( got[1] > got[0] + Scalar( 0.05 ), "(p) MONOTONE: convexity(0.5) > convexity(0.25)" );
	Check( got[2] > got[1] + Scalar( 0.10 ), "(p) MONOTONE: convexity(1.0) > convexity(0.5)" );

	// The sphere is convex everywhere, so occlusion must be a flat 1 across
	// the whole series -- the pairing (o) makes on wedges, made here on a
	// smoothly curved surface.
	for( int i = 0; i < 3; ++i ) {
		Scalar ao = -1, spread = -1;
		const std::string rf = std::to_string( Rs[i] / diag );
		Check( EvalAtHitMean( o, Point3( 0.0, 30, 0.0 ), Vector3( 0, -1, 0 ), slideX, 128,
			"occlusion(" + rf + ")", ao, &spread ),
			"(p) sphere occlusion evaluates at R/rho=" + std::to_string( Rs[i] ) );
		CheckClose( ao, 1.0, 1e-9,
			"(p) a convex sphere reads occlusion 1 at R/rho=" + std::to_string( Rs[i] ) );
		CheckClose( spread, 0.0, 0.0,
			"(p) ...under EVERY rotation, at R/rho=" + std::to_string( Rs[i] ) );
	}

	o->release();
}

//======================================================================
// (q) occlusion stayed MONOTONE with the estimator it replaced
//
// The correction re-bases the scale; it must not reorder cavities.  On the
// concave-wedge family both estimators have closed forms in the empty
// opening angle alpha (docs/OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md §2.1),
// each at its own canonical query configuration:
//
//     occ_new = sin^2( alpha/2 )              (cosine-weighted visibility,
//                                              face-normal axis; derived below)
//     occ_old = cos( (pi - alpha)/2 )         (bisector-normal shortfall)
//             = sin( alpha/2 )
//  => occ_new = occ_old^2
//
// x^2 is a strictly increasing bijection of [0,1] onto itself, so the ORDER
// of cavities is preserved exactly, both endpoints are fixed (a plane still
// reads 1, a sealed pocket still 0), and every intermediate cavity now reads
// DARKER, never lighter -- the right-angle valley that read 0.707 now reads
// 0.500.  Both claims are measured here rather than asserted in prose.
//
// NOTE the two forms are each estimator's reading on a wedge of opening
// alpha at the configuration where it has a closed form, so `occ_new =
// occ_old^2` compares characteristic values; it is not a pointwise
// conversion applicable hit by hit.
//======================================================================

//! A CONCAVE WEDGE of controlled EMPTY OPENING ANGLE `openDeg`, built as
//! a floor plus one wall.
//!
//! The floor's top face lies on y = 0.  The wall is a slab whose own face
//! contains the origin line and whose body lies behind it, tilted about X
//! so that the angle between the two empty-side face normals is exactly
//! `180 - openDeg` -- i.e. the empty region between them is a wedge of
//! opening `openDeg`.  `openDeg = 180` is a bare plane, `90` a right-angle
//! valley, and anything below 90 an overhang.
//!
//! The wall part is placed by its FACE, not by its centre: a box rotated
//! about X by `beta` maps its local +Y to (0, cos beta, sin beta), so
//! putting the centre at -h*(0, cos beta, sin beta) leaves the +Y face
//! passing through the origin with the body behind it.
static SDFGeometry* BuildSdfOpenWedge( const Scalar openDeg )
{
	const Scalar beta = ( Scalar(180) - openDeg ) * Scalar( PI ) / Scalar( 180 );
	const Scalar h    = 6.0;

	std::vector<SDFGeometry::Part> parts;
	// Floor: a slab whose top face is y = 0.
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3( 0, -h, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 10.0, h, 10.0, 0 ) );
	// Wall.
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3( 0, -h * std::cos( beta ), -h * std::sin( beta ) ),
		( Scalar(180) - openDeg ), 0, 0, Vector3( 1, 1, 1 ), 10.0, h, 10.0, 0 ) );
	return new SDFGeometry( parts, 512, Scalar( 1e-5 ) );
}

static void TestOcclusionMonotoneAcrossWedgeAngles()
{
	std::cout << "(q) occlusion == sin^2(alpha/2) across a wedge sweep, and a narrow SLOT still goes dark" << std::endl;

	// 180 degrees is a bare plane and must read exactly 1 -- the same planar
	// identity (m) pins, reached from the wedge family so a regression in
	// the family's construction shows up here rather than as a silently
	// shifted baseline for the rest of the sweep.
	//
	// THE CLOSED FORM.  Occlusion is the COSINE-weighted fraction of outward
	// directions that escape.  For a point on one face of a wedge whose
	// empty opening is alpha, with the face's own normal as the axis, that
	// integral is
	//     (1/pi) * int_gamma int_psi (cos gamma sin psi) cos gamma dpsi dgamma
	//   = (1/2)(1 - cos alpha) = sin^2(alpha/2).
	// 180 -> 1, 150 -> 0.933, 120 -> 0.750, 90 -> 0.500.
	//
	// The sweep stops at 90 on purpose: below it the wall OVERHANGS the
	// floor, and this fixture's query point sits close enough to the crease
	// that the normal-lifted march origin ends up inside the wall -- a
	// property of the fixture, not of the estimator, and not worth building
	// a second fixture to chase.
	const Scalar openings[4] = { 180.0, 150.0, 120.0, 90.0 };
	Scalar occ[4] = { -1, -1, -1, -1 };

	for( int i = 0; i < 4; ++i ) {
		SDFGeometry* g = BuildSdfOpenWedge( openings[i] );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();

		// Approach the crease ALONG THE WEDGE'S BISECTOR.  Straight down
		// would work only while the opening is >= 90 degrees; the bisector
		// of the two empty-side face normals is inside the opening by
		// construction, at every angle in the sweep.
		const Scalar beta = ( Scalar(180) - openings[i] ) * Scalar( PI ) / Scalar( 180 );
		const Vector3 mWall( 0, std::cos( beta ), std::sin( beta ) );
		Vector3 bis( 0, Scalar(1) + mWall.y, mWall.z );
		const Scalar bl = std::sqrt( bis.y*bis.y + bis.z*bis.z );
		bis = Vector3( 0, bis.y/bl, bis.z/bl );
		const Point3 target( 1.3, 0, 0.02 );
		// AN EXPECTATION over 128 hits slid 1e-5 ALONG THE CREASE (it runs in
		// x, and the fixture is translation-invariant along it, so the closed
		// form is identical at every probe and only the sample set's rotation
		// varies).  See EvalAtHitMean.  The number this replaces was a
		// single hit, and at 12 directions a single hit is a draw from
		// [0.417, 0.583] for this very fixture -- the 24-direction set landed
		// on 0.5 exactly, but by where the branchless ONB happened to put
		// `u`, not by accuracy.  Azimuth-averaged the reading is 0.5000 at
		// 8, 12, 16 and 24 directions alike.
		Scalar spread = -1;
		Check( EvalAtHitMean( o, Point3( target.x + 40*bis.x, target.y + 40*bis.y, target.z + 40*bis.z ),
			Vector3( -bis.x, -bis.y, -bis.z ), Vector3( 1e-5, 0, 0 ), 128, "occlusion(0.06)", occ[i], &spread ),
			"(q) crease hit at opening " + std::to_string( (int)openings[i] ) );

		const Scalar a = openings[i] * Scalar( PI ) / Scalar( 180 );
		const Scalar want = std::sin( a * Scalar(0.5) ) * std::sin( a * Scalar(0.5) );
		CheckClose( occ[i], want, 0.06,
			"(q) occlusion == sin^2(alpha/2) at " + std::to_string( (int)openings[i] ) + " degrees" );
		// RED-PROOF: an estimator that had drifted onto the NEIGHBOURING
		// opening's answer -- exactly what a mis-scaled cosine weight or a
		// lost sign produces -- must be rejected.  From the 120-degree step
		// on, where consecutive closed forms are 0.183 and 0.250 apart
		// against the 0.06 band; the 180 -> 150 step is only 0.067 apart and
		// the two bands genuinely overlap, so there is no claim to make there
		// and the test does not pretend to one.
		if( i > 1 ) {
			const Scalar aPrev = openings[i-1] * Scalar( PI ) / Scalar( 180 );
			const Scalar wrong = std::sin( aPrev * Scalar(0.5) ) * std::sin( aPrev * Scalar(0.5) );
			Check( std::fabs( occ[i] - wrong ) > Scalar( 0.06 ),
				"(q) RED-PROOF: " + std::to_string( (int)openings[i] )
				+ " degrees does NOT also satisfy the previous opening's closed form" );
		}
		// The 180-degree case is a bare PLANE, so it is pointwise, not an
		// average: no rotation may move it off 1 (see (m)).
		if( i == 0 ) {
			CheckClose( spread, 0.0, 0.0,
				"(q) the 180-degree 'wedge' reads 1 under EVERY rotation, not on average" );
		}
		o->release();
	}

	CheckClose( occ[0], 1.0, 1e-12, "(q) the 180-degree 'wedge' is a plane and reads EXACTLY 1" );
	for( int i = 1; i < 4; ++i ) {
		Check( occ[i] < occ[i-1],
			"(q) MONOTONE: a narrower opening reads darker ("
			+ std::to_string( (int)openings[i] ) + " vs " + std::to_string( (int)openings[i-1] ) + " degrees)" );
	}

	// ---- THE NARROW-SLOT GUARD, and the reason it is in this test ----
	//
	// The requirement occlusion has to keep is "a cavity that read dark
	// still reads dark".  A wedge sweep does not exercise it: the case that
	// actually broke, in a render, was a NARROW SLOT -- plank_closeup's end
	// check, 2.6 mm wide, queried at 18.6 mm.  An intermediate draft of this
	// work measured occlusion as the fraction of the query BALL lying
	// outside the solid, which is a beautiful definition and gives the wrong
	// answer here: the ball reaches up out of the slot into open air and the
	// slot removes only ~5 % of a ball that much bigger than it, so the slot
	// WALL came out at 0.55 (and a half-ball variant at 0.81), the crack
	// read unoccluded, and its dirt vanished from the image.  Only a
	// DIRECTIONAL test knows that none of that open air is reachable.
	//
	// So: a slot narrow relative to the query radius, sampled ON ITS WALL,
	// must read strongly occluded.  Scaled to match the shipped scene --
	// a 1-unit slot queried at ~7 units.
	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
			Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 20.0, 10.0, 20.0, 0 ) );
		// A 1-unit-wide, 8-unit-deep slot down the middle of the top face.
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpSubtract, 0,
			Point3( 0, 12, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 15.0, 10.0, 0.5, 0 ) );
		SDFGeometry* g = new SDFGeometry( parts, 512, Scalar( 1e-5 ) );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();

		// Down INTO the slot at a shallow angle, so the hit lands on its
		// WALL a few units below the top face rather than on its floor: the
		// ray enters the 1-wide mouth at z = -0.45 and crosses to the far
		// wall (z = +0.5) after descending 6.3, i.e. at y = 3.7, which is
		// still 1.7 clear of the slot floor at y = 2.
		const Point3  slotFrom( 3.0, 60.0, -7.95 );
		const Vector3 slotDir ( 0.0, -1.0, 0.15 );

		// NON-VACUITY: confirm the hit really is on a slot WALL before
		// believing anything the estimator says about it.  A ray that
		// clipped the top face or bottomed out on the slot floor would make
		// the money assertion below meaningless in either direction.
		RayIntersection riWall = MkRI( slotFrom, slotDir );
		Check( HitObject( o, riWall ), "(q) slot ray hits the composite" );
		if( riWall.geometric.bHit ) {
			const Point3& hp = riWall.geometric.signals.ptObject;
			Check( std::fabs( std::fabs( hp.z ) - 0.5 ) < Scalar( 0.05 ),
				"(q) ... on a slot WALL (|z| ~ 0.5), not the floor or the top face" );
			Check( hp.y < Scalar( 8.0 ) && hp.y > Scalar( 2.5 ),
				"(q) ... well inside the slot's depth" );
		}

		Scalar occWall = -1, occOpen = -1;
		Check( EvalAtHit( o, slotFrom, slotDir, "occlusion(0.15)", occWall ),
			"(q) slot-wall ray evaluates" );
		Check( EvalAtHit( o, Point3( 3.0, 60, 8.0 ), Vector3( 0, -1, 0 ), "occlusion(0.15)", occOpen ),
			"(q) open-top-face ray evaluates" );

		CheckClose( occOpen, 1.0, 1e-12,
			"(q) the open top face beside the slot reads EXACTLY 1 (the slot is a void, it cannot occlude)" );
		Check( occWall < Scalar( 0.35 ),
			"(q) MONEY -- the wall of a NARROW SLOT reads strongly occluded (the ball-volume draft read 0.55-0.81 here)" );
		Check( occOpen - occWall > Scalar( 0.6 ),
			"(q) ... and is separated from the open face beside it by more than 0.6" );
		o->release();
	}
}

//======================================================================

int main()
{
	std::cout << "=== SurfaceSignalsTest (occlusion / thickness / convexity) ===" << std::endl;

	TestConvexSphereIsUnoccluded();
	TestCreviceDarkensAndConvexDoesNot();
	TestThicknessMonotoneInSlabWidth();
	TestNeutralFallbacks();
	TestParseTimeRejection();
	TestConstantRadiusRecord();
	TestEndToEndThroughPainter();
	TestConcurrentEvaluation();
	TestRadiusFractionScaleInvariance();
	TestHeightfieldRefusesSignals();
	TestCsgSubtractionRepairsSignalNormal();
	TestDynamicRadiusAnsweredOnSdf();
	TestPlanarReferenceIsExact();
	TestConvexCsgEdgeHasNoOcclusionResidual();
	TestConvexityCanonicalValues();
	TestConvexityOnSphereClosedForm();
	TestOcclusionMonotoneAcrossWedgeAngles();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
