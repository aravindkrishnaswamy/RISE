//////////////////////////////////////////////////////////////////////
//
//  BezierClippingUnitsTest.cpp - Deterministic unit tests for the
//  parameter-space Bezier-clipping primitives used by the analytic
//  ray/bicubic-patch intersection path.
//
//  Covers:
//    - DeCasteljauU / DeCasteljauV continuity at the split seam
//    - ExtractSubRangeU / ExtractSubRangeV identity on full range
//    - ExtractSubRangeU on a linearly-varying grid against analytic truth
//    - ConvexHullClipU rejection when no zero crossing exists
//    - ConvexHullClipU bracket shape on a straddling grid
//    - Cross-axis symmetry (V helpers behave like U helpers, transposed)
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Intersection/BezierClipping.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using RISE::Scalar;
using RISE::BezierClip::DistanceGrid;
using RISE::BezierClip::ParamBox;
using RISE::BezierClip::DeCasteljauU;
using RISE::BezierClip::DeCasteljauV;
using RISE::BezierClip::ExtractSubRangeU;
using RISE::BezierClip::ExtractSubRangeV;
using RISE::BezierClip::ConvexHullClipU;
using RISE::BezierClip::ConvexHullClipV;
using RISE::BezierClip::GridMaxAbs;

namespace {

// Small helpers ------------------------------------------------------------

bool approx_equal( Scalar a, Scalar b, Scalar tol = 1e-12 )
{
	return std::fabs( a - b ) <= tol;
}

void MakeGrid_Linear( DistanceGrid& g, Scalar base, Scalar du, Scalar dv )
{
	// g.d[i][j] = base + du*(i/3) + dv*(j/3)    for i, j in [0,3]
	// A degree-(1,1) bilinear function represented in Bernstein basis — the
	// Bernstein coefficients ARE the function values at the control grid.
	for( int i = 0; i < 4; ++i ) {
		for( int j = 0; j < 4; ++j ) {
			g.d[i][j] = base + du * ( i / 3.0 ) + dv * ( j / 3.0 );
		}
	}
}

void MakeGrid_AllPositive( DistanceGrid& g )
{
	// All values strictly > 0 — the cubic sample F has no root.
	for( int i = 0; i < 4; ++i ) {
		for( int j = 0; j < 4; ++j ) {
			g.d[i][j] = 1.0 + 0.1 * i + 0.05 * j;
		}
	}
}

void MakeGrid_Straddling( DistanceGrid& g )
{
	// F(u, v) linear in u, zero at u = 0.5.
	// d[i][j] = (i/3 - 0.5) for all j  → envelope crosses zero at x = 0.5.
	for( int i = 0; i < 4; ++i ) {
		const Scalar v = ( i / 3.0 ) - 0.5;
		for( int j = 0; j < 4; ++j ) {
			g.d[i][j] = v;
		}
	}
}

// Tests --------------------------------------------------------------------

// Continuity of the split: at parameter t, the left sub-patch's right edge
// must coincide exactly with the right sub-patch's left edge.
void Test_DeCasteljauU_Continuity()
{
	DistanceGrid g;
	MakeGrid_Linear( g, 1.0, 3.0, -2.0 );

	DistanceGrid left, right;
	DeCasteljauU( g, 0.5, left, right );

	for( int j = 0; j < 4; ++j ) {
		assert( approx_equal( left.d[3][j], right.d[0][j] ) );
	}
	std::printf( "  Test_DeCasteljauU_Continuity  OK\n" );
}

void Test_DeCasteljauV_Continuity()
{
	DistanceGrid g;
	MakeGrid_Linear( g, -0.5, 1.0, 4.0 );

	DistanceGrid left, right;
	DeCasteljauV( g, 0.25, left, right );

	for( int i = 0; i < 4; ++i ) {
		assert( approx_equal( left.d[i][3], right.d[i][0] ) );
	}
	std::printf( "  Test_DeCasteljauV_Continuity  OK\n" );
}

// Full-range extraction must be the identity.
void Test_ExtractSubRange_FullRangeIsIdentity()
{
	DistanceGrid g;
	MakeGrid_Linear( g, 2.0, -1.5, 0.75 );

	DistanceGrid out;
	ExtractSubRangeU( g, 0.0, 1.0, out );
	for( int i = 0; i < 4; ++i ) {
		for( int j = 0; j < 4; ++j ) {
			assert( approx_equal( out.d[i][j], g.d[i][j] ) );
		}
	}

	ExtractSubRangeV( g, 0.0, 1.0, out );
	for( int i = 0; i < 4; ++i ) {
		for( int j = 0; j < 4; ++j ) {
			assert( approx_equal( out.d[i][j], g.d[i][j] ) );
		}
	}
	std::printf( "  Test_ExtractSubRange_FullRangeIsIdentity  OK\n" );
}

// Extract a sub-range of a linearly-varying grid and check against truth.
// For d[i][j] = base + du*(i/3) + dv*(j/3), extracting u in [u_lo, u_hi]
// yields a new grid whose (bilinear, hence Bernstein-at-nodes) control
// points are d'[i'][j'] = base + du*(u_lo + (u_hi-u_lo)*(i'/3)) + dv*(j'/3).
void Test_ExtractSubRangeU_LinearMatchesAnalytic()
{
	DistanceGrid g;
	const Scalar base = 5.0, du = 6.0, dv = -3.0;
	MakeGrid_Linear( g, base, du, dv );

	const Scalar u_lo = 0.25, u_hi = 0.75;
	DistanceGrid out;
	ExtractSubRangeU( g, u_lo, u_hi, out );

	for( int i = 0; i < 4; ++i ) {
		for( int j = 0; j < 4; ++j ) {
			const Scalar u = u_lo + ( u_hi - u_lo ) * ( i / 3.0 );
			const Scalar expected = base + du * u + dv * ( j / 3.0 );
			assert( approx_equal( out.d[i][j], expected, 1e-10 ) );
		}
	}
	std::printf( "  Test_ExtractSubRangeU_LinearMatchesAnalytic  OK\n" );
}

// Grid with every control point strictly > 0 → clipping returns false.
void Test_ConvexHullClipU_RejectsAllPositive()
{
	DistanceGrid g;
	MakeGrid_AllPositive( g );

	Scalar lo, hi;
	const Scalar eps = 1e-6 * GridMaxAbs( g ) + 1e-10;
	const bool ok = ConvexHullClipU( g, eps, lo, hi );
	assert( !ok );
	std::printf( "  Test_ConvexHullClipU_RejectsAllPositive  OK\n" );
}

void Test_ConvexHullClipV_RejectsAllPositive()
{
	DistanceGrid g;
	MakeGrid_AllPositive( g );

	Scalar lo, hi;
	const Scalar eps = 1e-6 * GridMaxAbs( g ) + 1e-10;
	const bool ok = ConvexHullClipV( g, eps, lo, hi );
	assert( !ok );
	std::printf( "  Test_ConvexHullClipV_RejectsAllPositive  OK\n" );
}

// Grid with d[i][j] = (i/3 - 0.5): zero crossing at u = 0.5 for every v.
// The clip range should include 0.5 and be reasonably tight around it.
void Test_ConvexHullClipU_BracketsZeroCrossing()
{
	DistanceGrid g;
	MakeGrid_Straddling( g );

	Scalar lo, hi;
	const Scalar eps = 1e-6 * GridMaxAbs( g ) + 1e-10;
	const bool ok = ConvexHullClipU( g, eps, lo, hi );
	assert( ok );
	assert( lo <= 0.5 && 0.5 <= hi );
	// For a linear function the pair-based convex-hull clip gives a
	// conservative but non-trivial bracket — should be well below full range.
	// The straddling envelope's pair intercepts include 0.5 exactly (from
	// opposite-sign pairs spanning it); the clip therefore collapses to a
	// narrow sliver around 0.5.
	assert( hi - lo < 0.5 );
	std::printf( "  Test_ConvexHullClipU_BracketsZeroCrossing  "
	             "OK  (bracket = [%.4f, %.4f])\n", lo, hi );
}

// Round-trip: DeCasteljauU at t then ExtractSubRangeU on the combined
// halves' bounds should reproduce the input grid to within FP epsilon.
// Specifically: the [0, t] half extracted from the full grid matches
// DeCasteljauU's `left` output.
void Test_ExtractSubRangeU_MatchesDeCasteljau()
{
	DistanceGrid g;
	// Non-linear grid to really exercise the subdivision math.
	for( int i = 0; i < 4; ++i ) {
		for( int j = 0; j < 4; ++j ) {
			g.d[i][j] = std::sin( i * 1.3 ) + std::cos( j * 0.7 + 0.3 );
		}
	}

	const Scalar t = 0.375;
	DistanceGrid cast_left, cast_right;
	DeCasteljauU( g, t, cast_left, cast_right );

	DistanceGrid extracted;
	ExtractSubRangeU( g, 0.0, t, extracted );

	for( int i = 0; i < 4; ++i ) {
		for( int j = 0; j < 4; ++j ) {
			assert( approx_equal( extracted.d[i][j], cast_left.d[i][j], 1e-12 ) );
		}
	}
	std::printf( "  Test_ExtractSubRangeU_MatchesDeCasteljau  OK\n" );
}

void Test_ParamBox_Map()
{
	ParamBox box = { 0.25, 0.75, 0.10, 0.50 };
	Scalar u, v;
	box.Map( 0.0, 0.0, u, v );
	assert( approx_equal( u, 0.25 ) && approx_equal( v, 0.10 ) );
	box.Map( 1.0, 1.0, u, v );
	assert( approx_equal( u, 0.75 ) && approx_equal( v, 0.50 ) );
	box.Map( 0.5, 0.5, u, v );
	assert( approx_equal( u, 0.50 ) && approx_equal( v, 0.30 ) );
	assert( approx_equal( box.UWidth(), 0.5 ) );
	assert( approx_equal( box.VWidth(), 0.4 ) );
	assert( approx_equal( box.Area(),   0.2 ) );
	std::printf( "  Test_ParamBox_Map  OK\n" );
}

} // anonymous namespace

int main()
{
	std::printf( "BezierClippingUnitsTest\n" );
	Test_DeCasteljauU_Continuity();
	Test_DeCasteljauV_Continuity();
	Test_ExtractSubRange_FullRangeIsIdentity();
	Test_ExtractSubRangeU_LinearMatchesAnalytic();
	Test_ExtractSubRangeU_MatchesDeCasteljau();
	Test_ConvexHullClipU_RejectsAllPositive();
	Test_ConvexHullClipV_RejectsAllPositive();
	Test_ConvexHullClipU_BracketsZeroCrossing();
	Test_ParamBox_Map();
	std::printf( "All BezierClippingUnitsTest checks PASSED.\n" );
	return 0;
}
