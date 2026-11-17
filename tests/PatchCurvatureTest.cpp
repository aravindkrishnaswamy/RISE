//////////////////////////////////////////////////////////////////////
//
//  PatchCurvatureTest.cpp
//
//    DL-20 red-proof.  BezierPatchGeometry and BilinearPatchGeometry
//    used to report FLAT (H=0) curvature at every hit -- either via a
//    fabricated ONB with dndu=dndv=0 reported as `valid=true`
//    (`ComputeSurfaceDerivatives(point, normal)`), or via total absence
//    (no signal-stamping block at all in `RayElementIntersection`, so
//    `ri.derivatives.valid` stayed at its default `false`) -- while
//    genuinely curved.  This test fires real rays at real patches
//    (a saddle and a dome for Bezier, a hyperbolic paraboloid for
//    bilinear) through the PRODUCTION signal path (IntersectRay ->
//    RayElementIntersection -> ri.derivatives), and compares the mean
//    curvature `RISE::SurfaceCurvature::MeanCurvatureFromDerivatives`
//    computes from THAT (exactly what ExpressionPainter::PopulateCurvature
//    does for `curv`/`curvR`) against an INDEPENDENT oracle:
//
//      - Bilinear hyperbolic paraboloid: a hand-derived CLOSED FORM
//        (Pu, Pv, Puu=0, Puv=const, Pvv=0 from the corner positions
//        directly, re-derived here rather than calling
//        BilinearPatchSecondDerivUV) fed through an independent,
//        locally-written shape-operator computation (NOT
//        SurfaceCurvature::ShapeOperatorFromSecondDerivatives).
//
//      - Bezier saddle: the control grid is built PRODUCT-SEPARABLE
//        (x(u), y(v) each an evenly-spaced Bezier control polygon --
//        exactly reproducing the LINEAR parametrization X(u)=-1.5+3u,
//        Y(v)=-1.5+3v, a standard Bernstein-basis identity -- with
//        z(u,v) = k*X(u)*Y(v)), which makes the WHOLE bicubic patch
//        EXACTLY a hyperbolic paraboloid: a second closed form,
//        independent of the bilinear one above.
//
//      - Bezier dome: NOT product-separable (z = A - B*(x^2+y^2) sampled
//        at the control points is only an approximate cubic fit to a
//        quadratic), so its oracle is a FINITE DIFFERENCE of the
//        patch's own public evaluator (GeometricUtilities::
//        EvaluateBezierPatchAt / BezierPatchTangentU / BezierPatchTangentV)
//        at h=1e-4, central difference, independent of the new
//        BezierPatchSecondDeriv{UU,UV,VV} functions under test.
//
//    Run:   ./bin/tests/PatchCurvatureTest
//    Build: make -C build/make/rise build-test/PatchCurvatureTest
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <string>
#include <cmath>
#include <cstdio>

#include "../src/Library/Geometry/BezierPatchGeometry.h"
#include "../src/Library/Geometry/BilinearPatchGeometry.h"
#include "../src/Library/Utilities/GeometricUtilities.h"
#include "../src/Library/Utilities/SurfaceCurvature.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_Passed = 0;
static int g_Failed = 0;

static void Check( bool cond, const std::string& msg )
{
	if( cond ) {
		g_Passed++;
	} else {
		g_Failed++;
		std::cout << "FAILED: " << msg << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// Independent (re-derived, NOT calling SurfaceCurvature::
// ShapeOperatorFromSecondDerivatives) shape-operator-from-second-
// derivatives oracle, for the two closed-form cases.
//////////////////////////////////////////////////////////////////////
static bool OracleShapeOperator(
	const Vector3& dpdu, const Vector3& dpdv,
	const Vector3& Puu, const Vector3& Puv, const Vector3& Pvv,
	Vector3& outDndu, Vector3& outDndv )
{
	const Vector3 Q = Vector3Ops::Cross( dpdu, dpdv );
	const Scalar qlen = Vector3Ops::Magnitude( Q );
	if( qlen < 1e-12 ) return false;
	const Vector3 N = Q * ( 1.0 / qlen );

	const Vector3 dQdu = Vector3Ops::Cross( Puu, dpdv ) + Vector3Ops::Cross( dpdu, Puv );
	const Vector3 dQdv = Vector3Ops::Cross( Puv, dpdv ) + Vector3Ops::Cross( dpdu, Pvv );

	outDndu = ( dQdu - N * Vector3Ops::Dot( N, dQdu ) ) * ( 1.0 / qlen );
	outDndv = ( dQdv - N * Vector3Ops::Dot( N, dQdv ) ) * ( 1.0 / qlen );
	return true;
}

static bool OracleMeanCurvature(
	const Vector3& dpdu, const Vector3& dpdv,
	const Vector3& Puu, const Vector3& Puv, const Vector3& Pvv,
	Scalar& outH )
{
	Vector3 dndu, dndv;
	if( !OracleShapeOperator( dpdu, dpdv, Puu, Puv, Pvv, dndu, dndv ) ) return false;
	return SurfaceCurvature::MeanCurvatureFromDerivatives( dpdu, dpdv, dndu, dndv, outH );
}

// The PRODUCTION path: read the mean curvature exactly the way
// ExpressionPainter::PopulateCurvature does (curvatureValid is never set
// by these two geometries, so it always falls to the `derivatives.valid`
// branch here).
static bool ProductionMeanCurvature( const RayIntersectionGeometric& ri, Scalar& outH )
{
	if( !ri.derivatives.valid ) return false;
	return SurfaceCurvature::MeanCurvatureFromDerivatives(
		ri.derivatives.dpdu, ri.derivatives.dpdv,
		ri.derivatives.dndu, ri.derivatives.dndv, outH );
}

//////////////////////////////////////////////////////////////////////
// Bilinear hyperbolic paraboloid: P(u,v) = (2u-1, 2v-1, (2u-1)(2v-1))
//////////////////////////////////////////////////////////////////////
static void TestBilinearHyperbolicParaboloid()
{
	std::cout << "-- Bilinear hyperbolic paraboloid --" << std::endl;

	BilinearPatchGeometry* g = new BilinearPatchGeometry( 10, 8, false );
	BilinearPatch patch;
	// RISE's own corner convention: pts[0]->(0,0), pts[1]->(0,1),
	// pts[2]->(1,0), pts[3]->(1,1).  x=2u-1, y=2v-1, z=x*y=(2u-1)(2v-1).
	patch.pts[0] = Point3( -1, -1,  1 );	// u=0,v=0
	patch.pts[1] = Point3( -1,  1, -1 );	// u=0,v=1
	patch.pts[2] = Point3(  1, -1, -1 );	// u=1,v=0
	patch.pts[3] = Point3(  1,  1,  1 );	// u=1,v=1
	g->AddPatch( patch );
	g->Prepare();

	// Sample away from the exact saddle center (u=v=0.5, where H=0 by
	// symmetry -- a degenerate check) so the sign/magnitude are both
	// exercised: probe near (u,v) = (0.25, 0.75) i.e. (x,y) = (-0.5, 0.5).
	const Scalar u = 0.25, v = 0.75;
	const Point3 target( 2.0*u - 1.0, 2.0*v - 1.0, (2.0*u-1.0)*(2.0*v-1.0) );

	RayIntersectionGeometric ri( Ray( Point3Ops::mkPoint3(target, Vector3(0,0,5)), Vector3(0,0,-1) ), nullRasterizerState );
	g->IntersectRay( ri, true, true, false );
	Check( ri.bHit, "bilinear hyperbolic paraboloid: ray hits" );
	if( !ri.bHit ) { g->release(); return; }

	Check( ri.derivatives.valid, "bilinear hyperbolic paraboloid: ri.derivatives.valid (production signal path)" );

	Scalar Hproduction = 0.0;
	bool haveProd = ProductionMeanCurvature( ri, Hproduction );
	Check( haveProd, "bilinear hyperbolic paraboloid: production H computed" );

	// Closed form, re-derived independently from the corner positions:
	// Pu = (1-v)*(pts[2]-pts[0]) + v*(pts[3]-pts[1])
	// Pv = (1-u)*(pts[1]-pts[0]) + u*(pts[3]-pts[2])
	// Puu = Pvv = 0;  Puv = pts[0]-pts[1]-pts[2]+pts[3]
	const Vector3 Pu = ( Vector3Ops::mkVector3(patch.pts[2],patch.pts[0]) * (1.0-v) )
	                  + ( Vector3Ops::mkVector3(patch.pts[3],patch.pts[1]) * v );
	const Vector3 Pv = ( Vector3Ops::mkVector3(patch.pts[1],patch.pts[0]) * (1.0-u) )
	                  + ( Vector3Ops::mkVector3(patch.pts[3],patch.pts[2]) * u );
	const Vector3 zero(0,0,0);
	const Vector3 Puv(
		patch.pts[0].x - patch.pts[1].x - patch.pts[2].x + patch.pts[3].x,
		patch.pts[0].y - patch.pts[1].y - patch.pts[2].y + patch.pts[3].y,
		patch.pts[0].z - patch.pts[1].z - patch.pts[2].z + patch.pts[3].z );

	Scalar Horacle = 0.0;
	bool haveOracle = OracleMeanCurvature( Pu, Pv, zero, Puv, zero, Horacle );
	Check( haveOracle, "bilinear hyperbolic paraboloid: closed-form oracle H computed" );

	std::printf( "  u=%.2f v=%.2f  H_production=% .6f  H_oracle=% .6f  |diff|=%.3e\n",
		u, v, Hproduction, Horacle, std::fabs(Hproduction - Horacle) );
	Check( std::fabs( Hproduction - Horacle ) < 1e-6 * std::max(1.0, std::fabs(Horacle)),
		"bilinear hyperbolic paraboloid: production H matches closed-form oracle (tight)" );
	// The stub-regression control this replaces: a genuinely non-flat
	// saddle must not read H=0.
	Check( std::fabs( Horacle ) > 1e-3, "bilinear hyperbolic paraboloid: oracle H is genuinely nonzero (sanity)" );

	g->release();
}

//////////////////////////////////////////////////////////////////////
// Bezier "saddle": product-separable control grid, EXACTLY a hyperbolic
// paraboloid z = k*X(u)*Y(v), X(u)=-1.5+3u, Y(v)=-1.5+3v.
//////////////////////////////////////////////////////////////////////
static void TestBezierSaddle()
{
	std::cout << "-- Bezier saddle (exact closed form) --" << std::endl;

	const Scalar k = 0.4;
	BezierPatchGeometry* g = new BezierPatchGeometry( 10, 8, false );
	BezierPatch patch;
	for( int i = 0; i < 4; i++ ) {
		const Scalar X = -1.5 + Scalar(i);	// evenly spaced -1.5..1.5
		for( int j = 0; j < 4; j++ ) {
			const Scalar Y = -1.5 + Scalar(j);
			patch.c[i].pts[j] = Point3( X, Y, k * X * Y );
		}
	}
	g->AddPatch( patch );
	g->Prepare();

	const Scalar u = 0.7, v = 0.3;	// away from the center
	const Scalar X = -1.5 + 3.0*u, Y = -1.5 + 3.0*v;
	const Point3 target( X, Y, k*X*Y );

	// Camera placed so Cross(TangentU,TangentV) already faces it (no
	// ray-facing flip): TangentU=(3,0,3kY), TangentV=(0,3,3kX), their
	// cross = (0*3kX-3kY*3, 3kY*0-3*3kX, 3*3-0*0) = (-9kY, -9kX, 9).
	// At (X,Y) with small k this points mostly +Z, so approach from +Z.
	RayIntersectionGeometric ri( Ray( Point3Ops::mkPoint3(target, Vector3(0,0,5)), Vector3(0,0,-1) ), nullRasterizerState );
	g->IntersectRay( ri, true, true, false );
	Check( ri.bHit, "bezier saddle: ray hits" );
	if( !ri.bHit ) { g->release(); return; }

	Check( ri.derivatives.valid, "bezier saddle: ri.derivatives.valid (production signal path, no ray-facing flip needed)" );

	Scalar Hproduction = 0.0;
	bool haveProd = ProductionMeanCurvature( ri, Hproduction );
	Check( haveProd, "bezier saddle: production H computed" );

	// Exact closed form: Pu=(3,0,3kY), Pv=(0,3,3kX), Puu=Pvv=0, Puv=(0,0,9k).
	const Vector3 Pu( 3.0, 0.0, 3.0*k*Y );
	const Vector3 Pv( 0.0, 3.0, 3.0*k*X );
	const Vector3 zero(0,0,0);
	const Vector3 Puv( 0.0, 0.0, 9.0*k );

	Scalar Horacle = 0.0;
	bool haveOracle = OracleMeanCurvature( Pu, Pv, zero, Puv, zero, Horacle );
	Check( haveOracle, "bezier saddle: closed-form oracle H computed" );

	std::printf( "  u=%.2f v=%.2f  H_production=% .6f  H_oracle=% .6f  |diff|=%.3e\n",
		u, v, Hproduction, Horacle, std::fabs(Hproduction - Horacle) );
	Check( std::fabs( Hproduction - Horacle ) < 1e-6 * std::max(1.0, std::fabs(Horacle)),
		"bezier saddle: production H matches closed-form oracle (tight)" );
	Check( std::fabs( Horacle ) > 1e-3, "bezier saddle: oracle H is genuinely nonzero (sanity)" );

	g->release();
}

//////////////////////////////////////////////////////////////////////
// Bezier saddle, ORIGINAL (untransposed) winding -- the exact control-grid
// indexing `tests/GeometrySurfaceDerivativesTest.cpp`'s own Bezier fixture
// used before the DL-20 closure's "Pre-existing test fixture corrections"
// transposed it to `patch.c[k].pts[j]` specifically to AVOID triggering
// BezierPatchGeometry::RayElementIntersection's ray-facing-flip
// conservative-reject branch (see docs/DL20_DL116_PATCH_CURVATURE_AND_
// POLE_WELDING.md "The flip case: conservative, not swapped" and
// "Pre-existing test fixture corrections").  That branch (`if(!bDidFlip)`
// in BezierPatchGeometry.cpp) is exactly what makes curvature reporting
// conservative rather than fabricated on a flipped hit -- untested by
// EITHER file until now, since both deliberately dodge it.  This test
// swaps `c[]`/`pts[]` back to the ORIGINAL indexing (`patch.c[j].pts[i]`
// instead of `patch.c[i].pts[j]`, same corner positions otherwise), which
// swaps which physical axis is "u" and which is "v" and so negates
// `Cross(TangentU, TangentV)` -- with the SAME camera as TestBezierSaddle
// (approaching from +Z, which the FIXED winding's normal already faces),
// this ORIGINAL winding's normal points AWAY from the camera, forcing
// `RayElementIntersection`'s `dotND > 0` branch and `bDidFlip = true`.
// Asserts the conservative-reject contract directly: the ray still hits
// and `bGeomNormalOrientedToRay` is set (confirming the flip really
// happened, not just assumed from the winding swap), but
// `ri.derivatives.valid` stays honestly `false` rather than reporting a
// left-handed (dpdu, dpdv, n) frame as valid. Guards against a future
// change to that gate (e.g. "just re-derive dndu/dndv for the flipped
// case") silently regressing to a wrong-handedness `valid=true` answer
// with no test noticing.
//////////////////////////////////////////////////////////////////////
static void TestBezierSaddleFlippedWinding()
{
	std::cout << "-- Bezier saddle, original (untransposed) winding -- ray-facing-flip conservative-reject --" << std::endl;

	const Scalar k = 0.4;
	BezierPatchGeometry* g = new BezierPatchGeometry( 10, 8, false );
	BezierPatch patch;
	for( int i = 0; i < 4; i++ ) {
		const Scalar X = -1.5 + Scalar(i);	// evenly spaced -1.5..1.5
		for( int j = 0; j < 4; j++ ) {
			const Scalar Y = -1.5 + Scalar(j);
			// UNTRANSPOSED (the original, pre-DL-20-fixture-correction
			// indexing): c[] and pts[] swapped relative to TestBezierSaddle
			// above -- same corner positions, opposite u/v assignment.
			patch.c[j].pts[i] = Point3( X, Y, k * X * Y );
		}
	}
	g->AddPatch( patch );
	g->Prepare();

	// Same target point and same +Z camera as TestBezierSaddle -- only the
	// winding differs, so any handedness change is attributable to that.
	const Scalar u = 0.7, v = 0.3;
	const Scalar X = -1.5 + 3.0*u, Y = -1.5 + 3.0*v;
	const Point3 target( X, Y, k*X*Y );

	RayIntersectionGeometric ri( Ray( Point3Ops::mkPoint3(target, Vector3(0,0,5)), Vector3(0,0,-1) ), nullRasterizerState );
	g->IntersectRay( ri, true, true, false );
	Check( ri.bHit, "bezier saddle (flipped winding): ray hits" );
	if( !ri.bHit ) { g->release(); return; }

	Check( ri.bGeomNormalOrientedToRay,
		"bezier saddle (flipped winding): the winding swap actually triggered the ray-facing flip (bGeomNormalOrientedToRay)" );
	Check( !ri.derivatives.valid,
		"bezier saddle (flipped winding): MONEY -- conservative-reject holds on a flipped hit "
		"(ri.derivatives.valid stays false rather than reporting a left-handed frame as valid)" );

	g->release();
}

//////////////////////////////////////////////////////////////////////
// Bezier "dome": z = A - B*(x^2+y^2) SAMPLED at the control grid (not
// product-separable -- the bicubic patch only approximately reproduces
// the paraboloid).  Oracle: finite difference of the patch's own public
// evaluator, independent of the code under test.
//////////////////////////////////////////////////////////////////////
static void TestBezierDome()
{
	std::cout << "-- Bezier dome (finite-difference oracle) --" << std::endl;

	const Scalar A = 1.5, B = 0.25;
	BezierPatch patch;
	for( int i = 0; i < 4; i++ ) {
		const Scalar X = -1.0 + Scalar(i) * (2.0/3.0);	// evenly spaced -1..1
		for( int j = 0; j < 4; j++ ) {
			const Scalar Y = -1.0 + Scalar(j) * (2.0/3.0);
			patch.c[i].pts[j] = Point3( X, Y, A - B*(X*X + Y*Y) );
		}
	}

	BezierPatchGeometry* g = new BezierPatchGeometry( 10, 8, false );
	g->AddPatch( patch );
	g->Prepare();

	const Scalar u = 0.65, v = 0.4;
	const Point3 target = GeometricUtilities::EvaluateBezierPatchAt( patch, u, v );

	RayIntersectionGeometric ri( Ray( Point3Ops::mkPoint3(target, Vector3(0,0,5)), Vector3(0,0,-1) ), nullRasterizerState );
	g->IntersectRay( ri, true, true, false );
	Check( ri.bHit, "bezier dome: ray hits" );
	if( !ri.bHit ) { g->release(); return; }

	Check( ri.derivatives.valid, "bezier dome: ri.derivatives.valid (production signal path, no ray-facing flip needed)" );

	Scalar Hproduction = 0.0;
	bool haveProd = ProductionMeanCurvature( ri, Hproduction );
	Check( haveProd, "bezier dome: production H computed" );

	// Finite-difference oracle, h=1e-4, central differences, using ONLY
	// the public evaluator (GeometricUtilities::EvaluateBezierPatchAt /
	// BezierPatchTangentU / BezierPatchTangentV) -- never the new
	// BezierPatchSecondDeriv{UU,UV,VV} functions under test.
	const Scalar h = 1e-4;
	const Vector3 dpdu = GeometricUtilities::BezierPatchTangentU( patch, u, v );
	const Vector3 dpdv = GeometricUtilities::BezierPatchTangentV( patch, u, v );
	const Vector3 TuPlus  = GeometricUtilities::BezierPatchTangentU( patch, u+h, v );
	const Vector3 TuMinus = GeometricUtilities::BezierPatchTangentU( patch, u-h, v );
	const Vector3 TvAtUp  = GeometricUtilities::BezierPatchTangentV( patch, u+h, v );
	const Vector3 TvAtUm  = GeometricUtilities::BezierPatchTangentV( patch, u-h, v );
	const Vector3 TuAtVp  = GeometricUtilities::BezierPatchTangentU( patch, u, v+h );
	const Vector3 TuAtVm  = GeometricUtilities::BezierPatchTangentU( patch, u, v-h );
	const Vector3 TvPlus  = GeometricUtilities::BezierPatchTangentV( patch, u, v+h );
	const Vector3 TvMinus = GeometricUtilities::BezierPatchTangentV( patch, u, v-h );

	const Vector3 Puu = ( TuPlus - TuMinus ) * ( 1.0 / (2.0*h) );
	// Two independent central-difference routes to the mixed partial
	// (d(Tu)/dv and d(Tv)/du) should agree to O(h^2) if BezierPatchTangentU/V
	// are themselves consistent (Clairaut) -- average them for the oracle.
	const Vector3 PuvA = ( TuAtVp - TuAtVm ) * ( 1.0 / (2.0*h) );
	const Vector3 PuvB = ( TvAtUp - TvAtUm ) * ( 1.0 / (2.0*h) );
	const Vector3 Puv  = ( PuvA + PuvB ) * 0.5;
	const Vector3 Pvv = ( TvPlus - TvMinus ) * ( 1.0 / (2.0*h) );

	std::printf( "  Clairaut cross-check |PuvA-PuvB|=%.3e (should be ~O(h^2)=%.1e)\n",
		Vector3Ops::Magnitude( PuvA - PuvB ), h*h );
	Check( Vector3Ops::Magnitude( PuvA - PuvB ) < 1e-3, "bezier dome: FD mixed-partial Clairaut cross-check" );

	Scalar Horacle = 0.0;
	bool haveOracle = OracleMeanCurvature( dpdu, dpdv, Puu, Puv, Pvv, Horacle );
	Check( haveOracle, "bezier dome: FD oracle H computed" );

	std::printf( "  u=%.2f v=%.2f  H_production=% .6f  H_oracle(FD)=% .6f  |diff|=%.3e\n",
		u, v, Hproduction, Horacle, std::fabs(Hproduction - Horacle) );
	// Looser, STATED tolerance for the FD oracle (per the recipe): FD
	// truncation error at h=1e-4 on a smooth analytic surface is tiny, but
	// this is a numerical (not exact) comparison.
	Check( std::fabs( Hproduction - Horacle ) < 1e-2 * std::max(1.0, std::fabs(Horacle)),
		"bezier dome: production H matches finite-difference oracle (1% stated tolerance)" );
	// Sign is whatever the patch's own (Cross(dpdu,dpdv))-derived normal
	// convention gives at this hit (not independently re-derived here);
	// the sanity check is only that curvature is genuinely nonzero, same
	// as the other two fixtures.
	Check( std::fabs( Horacle ) > 1e-3, "bezier dome: oracle H is genuinely nonzero (sanity)" );

	g->release();
}

int main()
{
	std::cout << "=== DL-20: patch-geometry curvature red-proof ===" << std::endl;

	TestBilinearHyperbolicParaboloid();
	TestBezierSaddle();
	TestBezierSaddleFlippedWinding();
	TestBezierDome();

	std::cout << "Passed: " << g_Passed << "  Failed: " << g_Failed << std::endl;
	return g_Failed == 0 ? 0 : 1;
}
