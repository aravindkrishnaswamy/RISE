//////////////////////////////////////////////////////////////////////
//
//  EllipsoidProximityBoundTest.cpp - DL-15 red-proof + regression.
//
//  docs/DEBT_LEDGER.md DL-15: EllipsoidGeometry::DistanceToSurface (the
//  object-space half of `proximity(r)`'s ellipsoid support) mapped the
//  query point into the unit-sphere frame, measured the sphere distance,
//  and scaled the answer back out by the LARGEST semi-axis.  That is a
//  valid upper bound (the safe direction -- see
//  docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 5.2), but it is only TIGHT
//  along the largest-semi-axis direction; everywhere else it over-reads
//  by up to the full semi-axis ratio.  The design doc's own cited
//  number: a 4:1 ellipsoid (semi-axes (4,1,1)) queried from a point at
//  true distance 2.75 off the MINOR axis reported 11.0 -- exactly
//  2.75 x 4.
//
//  THE FIX (EllipsoidGeometry.cpp) replaces the crude scaled-sphere
//  answer, for EXTERIOR points, with a near-exact one: the classic
//  Lagrange-multiplier construction for the nearest point on a triaxial
//  ellipsoid (solve the monotone scalar equation for the Lagrange
//  parameter t by safeguarded Newton/bisection, then read off the
//  candidate surface point).  The candidate is then rescaled to land
//  marginally OUTSIDE the true surface (a relative pad far above double
//  rounding noise) before the distance is measured, so the reported
//  number is -- BY CONSTRUCTION, independent of how well Newton
//  converged -- the distance to a point that is genuinely off (or on)
//  the ellipsoid: any such point's distance to the query can only be
//  >= the true minimum, so the safe-upper-bound contract holds even if
//  the iteration were cut short.  The interior clamp-to-zero convention
//  ("interpenetration is contact") is untouched.
//
//  WHY THE GROUND TRUTH BELOW IS TRUSTWORTHY, NOT CIRCULAR.  This suite
//  does not re-run the production Newton solver to check itself.  Two
//  independent sources of truth are used instead:
//
//    (1) ON-AXIS CLOSED FORM.  A point on a principal axis has its
//        nearest ellipsoid point on that SAME axis by the ellipsoid's
//        own mirror symmetry (reflecting the other two coordinates
//        negates them without changing the objective) -- PROVIDED the
//        query is not so far out that the axis vertex's evolute cusp
//        has been passed (the classic "ellipse nearest-point
//        bifurcation": beyond the vertex's radius of curvature, the
//        true nearest point splits off-axis).  Radius of curvature at
//        a semi-axis-A vertex, treating the other axis as a single
//        semi-axis B (degenerate to that if the remaining two are
//        equal, as every fixture below is): rho = B^2 / A.  Every
//        on-axis probe point here is chosen well inside that bound, and
//        the comment at each one says why.
//    (2) AN INDEPENDENT NUMERICAL OFF-AXIS ORACLE for a point that
//        isn't on any axis: a coordinate-descent search directly over
//        the ellipsoid's own (theta, phi) parametrization, sharing no
//        code with EllipsoidGeometry's Lagrange-multiplier solver.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <iostream>
#include <string>

#include "../src/Library/Geometry/EllipsoidGeometry.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( const bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

static void CheckClose( const double got, const double want, const double tol, const std::string& name )
{
	if( std::fabs( got - want ) <= tol ) { ++passCount; }
	else {
		++failCount;
		std::cout << "  FAIL: " << name << "  got " << got
			<< " want " << want << " (tol " << tol << ")" << std::endl;
	}
}

//! Independent numerical oracle: nearest point on the ellipsoid
//! {(a sin(theta)cos(phi), b sin(theta)sin(phi), c cos(theta))} to
//! `p`, by coarse grid search followed by coordinate-descent
//! refinement with step halving.  Deliberately a different algorithm
//! (no Lagrange multiplier, no Newton on the constraint) from
//! EllipsoidGeometry's own solver, so it cannot share a bug with it.
static double BruteForceEllipsoidDistance( double a, double b, double c, double px, double py, double pz )
{
	const int gridN = 240;
	double best = 1e300, bestTheta = 0.0, bestPhi = 0.0;
	for( int i = 0; i <= gridN; ++i ) {
		const double theta = PI * (double)i / (double)gridN;
		const double st = std::sin( theta ), ct = std::cos( theta );
		for( int j = 0; j < gridN; ++j ) {
			const double phi = TWO_PI * (double)j / (double)gridN;
			const double x = a * st * std::cos( phi ) - px;
			const double y = b * st * std::sin( phi ) - py;
			const double z = c * ct - pz;
			const double d2 = x*x + y*y + z*z;
			if( d2 < best ) { best = d2; bestTheta = theta; bestPhi = phi; }
		}
	}

	double theta = bestTheta, phi = bestPhi;
	double step = PI / (double)gridN;
	auto eval = [&]( double th, double ph ) -> double {
		const double st = std::sin( th ), ct = std::cos( th );
		const double x = a * st * std::cos( ph ) - px;
		const double y = b * st * std::sin( ph ) - py;
		const double z = c * ct - pz;
		return x*x + y*y + z*z;
	};
	for( int iter = 0; iter < 80 && step > 1e-12; ++iter ) {
		bool improved = false;
		for( double dt : { -step, step } ) {
			const double d2 = eval( theta + dt, phi );
			if( d2 < best ) { best = d2; theta += dt; improved = true; }
		}
		for( double dp : { -step, step } ) {
			const double d2 = eval( theta, phi + dp );
			if( d2 < best ) { best = d2; phi += dp; improved = true; }
		}
		if( !improved ) { step *= 0.5; }
	}
	return std::sqrt( best );
}

//! Old (pre-fix) formula, kept here byte-for-byte as it read in
//! EllipsoidGeometry.cpp before this row's fix, purely so this suite can
//! print the contrast -- it is NOT used as a pass/fail oracle.
static double OldScaledSphereBound( double a, double b, double c, double px, double py, double pz )
{
	const double qx = px / a, qy = py / b, qz = pz / c;
	const double rq = std::sqrt( qx*qx + qy*qy + qz*qz );
	const double dUnit = ( rq > 1.0 ) ? ( rq - 1.0 ) : 0.0;
	const double sigmaMax = std::max( a, std::max( b, c ) );
	return dUnit * sigmaMax;
}

struct Ratio
{
	const char* label;
	double a, b, c;   // semi-axes; every fixture here is a spheroid of
	                  // revolution about x (b == c), so "the ratio" is
	                  // unambiguous.
};

static void TestOnAxisTipsAndWaist()
{
	std::cout << "(1) on-axis closed form -- tips (major axis) and waist (minor axis), "
		"ratios 1:1, 2:1, 4:1, 10:1" << std::endl;

	const Ratio ratios[] = {
		{ "1:1",  2.0, 2.0, 2.0 },
		{ "2:1",  2.0, 1.0, 1.0 },
		{ "4:1",  4.0, 1.0, 1.0 },
		{ "10:1", 10.0, 1.0, 1.0 },
	};

	for( const Ratio& r : ratios ) {
		EllipsoidGeometry* g = new EllipsoidGeometry( Vector3( r.a, r.b, r.c ) );

		// --- WAIST (minor axis, +y).  Safe out to y < b + a^2/b (the
		// vertex's own radius of curvature); the doc's own 4:1 example is
		// this exact configuration at delta = 2.75.  a^2/b is >= 2 for
		// every ratio here (it is exactly 2 for the a==b sphere, where
		// the sphere's full rotational symmetry makes the on-axis answer
		// exact at ANY distance regardless of the formula's degenerate
		// limit), so delta = 2.75 is safely inside for every fixture
		// except the a=b=2 sphere needs its own argument -- given above.
		{
			const double delta = 2.75;
			const Point3 pObj( 0.0, r.b + delta, 0.0 );
			Scalar dOld = 0.0;
			const bool ok = g->DistanceToSurface( pObj, Scalar( 1000 ), dOld );
			Check( ok, std::string( "(1) waist " ) + r.label + " answers" );
			const double got = (double)dOld;
			const double oldBound = OldScaledSphereBound( r.a, r.b, r.c, pObj.x, pObj.y, pObj.z );
			std::cout << "    " << r.label << " waist: true " << delta
				<< ", old-formula bound " << oldBound
				<< ", reported " << got << std::endl;
			Check( got >= delta - 1e-6,
				std::string( "(1) MONEY -- " ) + r.label + " waist bound never reads BELOW the true distance (2.75)" );
			CheckClose( got, delta, 1e-4,
				std::string( "(1) MONEY -- " ) + r.label + " waist bound is now NEAR-EXACT, not the old " +
				std::to_string( oldBound ) );
		}

		// --- TIP (major axis, +x).  The major-axis vertex's radius of
		// curvature is b^2/a, which SHRINKS as the ratio grows -- the
		// ellipsoid gets pointier there, not flatter.  Each delta below
		// is chosen at roughly a tenth of that vertex's own rho, so the
		// on-axis closed form is honest for every ratio (this is a
		// regression check: the OLD formula was already exact in this
		// direction -- it IS the top singular vector -- so the point is
		// to confirm the new solver doesn't regress it).
		{
			const double rho = ( r.b * r.b ) / r.a;
			const double delta = 0.1 * rho;
			const Point3 pObj( r.a + delta, 0.0, 0.0 );
			Scalar dOld = 0.0;
			const bool ok = g->DistanceToSurface( pObj, Scalar( 1000 ), dOld );
			Check( ok, std::string( "(1) tip " ) + r.label + " answers" );
			const double got = (double)dOld;
			std::cout << "    " << r.label << " tip: true " << delta << ", reported " << got
				<< " (vertex rho = " << rho << ")" << std::endl;
			Check( got >= delta - 1e-6,
				std::string( "(1) tip " ) + r.label + " bound never reads below the true distance" );
			CheckClose( got, delta, std::max( 1e-6, 1e-4 * delta ),
				std::string( "(1) tip " ) + r.label + " stays near-exact (regression: this direction was already tight)" );
		}

		// --- INSIDE.  Unaffected by this fix: "interpenetration is
		// contact" clamps the unsigned query at 0 regardless of how deep.
		{
			const Point3 pObj( 0.5 * r.a, 0.0, 0.0 ); // (0.5)^2 = 0.25 < 1: interior
			Scalar dIn = -1.0;
			const bool ok = g->DistanceToSurface( pObj, Scalar( 1000 ), dIn );
			Check( ok, std::string( "(1) inside " ) + r.label + " answers" );
			CheckClose( (double)dIn, 0.0, 1e-12,
				std::string( "(1) inside " ) + r.label + " still clamps to 0 (unchanged convention)" );
		}

		g->release();
	}
}

static void TestOffAxisAgainstIndependentOracle()
{
	std::cout << "(2) off-axis, ratio 4:1, against an independent coordinate-descent oracle" << std::endl;

	// (4,1,1) queried from (5, 1.5, 0): (5/4)^2+(1.5/1)^2 = 1.5625+2.25
	// = 3.8125 > 1, exterior, and not aligned with any axis -- exactly
	// the case the old scaled-sphere formula has no special handling
	// for.
	const double a = 4.0, b = 1.0, c = 1.0;
	const double px = 5.0, py = 1.5, pz = 0.0;

	EllipsoidGeometry* g = new EllipsoidGeometry( Vector3( a, b, c ) );
	Scalar dNew = 0.0;
	const bool ok = g->DistanceToSurface( Point3( px, py, pz ), Scalar( 1000 ), dNew );
	Check( ok, "(2) off-axis ellipsoid answers" );

	const double trueDist = BruteForceEllipsoidDistance( a, b, c, px, py, pz );
	const double oldBound = OldScaledSphereBound( a, b, c, px, py, pz );
	const double got = (double)dNew;

	std::cout << "    (4,1,1) at (5,1.5,0): independent-oracle true " << trueDist
		<< ", old-formula bound " << oldBound << ", new reported " << got << std::endl;

	Check( got >= trueDist - 1e-3,
		"(2) MONEY -- off-axis bound never reads below the independent oracle's true distance" );
	Check( got <= trueDist * 1.01 + 1e-3,
		"(2) MONEY -- off-axis bound is now within 1% of the true distance, not the old ratio-loose one" );
	// The old formula's own over-report on this exact point, printed
	// rather than asserted (it depends only on arithmetic already
	// checked in EllipsoidProximityBoundTest's OldScaledSphereBound, not
	// on this row's fix), so a reader can see the improvement directly:
	// old-bound / true is the ratio the ledger row describes as "loose by
	// the semi-axis ratio".
	std::cout << "    old/true ratio = " << ( oldBound / trueDist )
		<< " (semi-axis ratio is " << ( a / b ) << ")" << std::endl;

	g->release();
}

static void TestOutExactNeverSetOnEllipsoid()
{
	std::cout << "(3) outExact stays honest -- an ellipsoid never claims exactness" << std::endl;

	EllipsoidGeometry* g = new EllipsoidGeometry( Vector3( 4.0, 1.0, 1.0 ) );
	Scalar outSigned = 0.0;
	bool outExact = true; // deliberately pre-set to catch a function that forgets to write it
	const bool ok = g->SignedDistanceLower( Point3( 0.0, 3.75, 0.0 ), Scalar( 1000 ), outSigned, outExact );
	Check( ok, "(3) SignedDistanceLower answers" );
	Check( !outExact, "(3) MONEY -- SignedDistanceLower on an ellipsoid never sets outExact "
		"(the DL-15 fix only tightens the UNSIGNED upper bound; the signed LOWER bound and its "
		"exactness contract are untouched, and unaffected by this row)" );
	g->release();
}

int main()
{
	std::cout << "=== EllipsoidProximityBoundTest (DL-15) ===" << std::endl;

	TestOnAxisTipsAndWaist();
	TestOffAxisAgainstIndependentOracle();
	TestOutExactNeverSetOnEllipsoid();

	std::cout << std::endl << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
