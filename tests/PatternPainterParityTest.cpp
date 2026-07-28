//////////////////////////////////////////////////////////////////////
//
//  PatternPainterParityTest.cpp - Unit tests for CheckerPainter and
//  LinesPainter cell parity on signed texture coordinates.
//
//  Tests:
//    1. Checker alternates along +x, -x, +y, -y
//    2. Checker parity is consistent across the origin (diagonal
//       neighbours match, edge neighbours differ)
//    3. Lines alternate with uniform stripe width, including the
//       stripes adjacent to the origin (no double-width center)
//    4. Lines parity for deep-negative coordinates
//
//  Guards the negative-parity defect family: C++ % on a negative
//  quotient yields 0 or -1 (never +1), so parity tests written for
//  positive-modulo semantics classified every negative cell "odd"
//  and the checker collapsed to a solid in the negative quadrant;
//  int-truncation (instead of floor) doubled the origin-straddling
//  stripe in LinesPainter.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: July 28, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>

#include "../src/Library/Painters/CheckerPainter.h"
#include "../src/Library/Painters/LinesPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// Sample a painter at texture coordinate (x, y) and report which of
	// the two bound colors it returned: 0 for black, 1 for white.
	int WhichColor( const IPainter& p, const Scalar x, const Scalar y )
	{
		Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		RasterizerState rast;
		RayIntersectionGeometric ri( ray, rast );
		ri.ptCoord = Point2( x, y );
		const RISEPel c = p.GetColor( ri );
		return c[0] > 0.5 ? 1 : 0;
	}
}

bool TestCheckerAlternation()
{
	std::cout << "  Test 1: Checker alternates along each axis..." << std::endl;

	UniformColorPainter* black = new UniformColorPainter( RISEPel( 0, 0, 0 ) );
	UniformColorPainter* white = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	CheckerPainter* checker = new CheckerPainter( 1.0, *black, *white );

	bool passed = true;
	// walk cell centers along both axes, far into the negative range
	for( int axis = 0; axis < 2; axis++ ) {
		int prev = -1;
		for( int i = -8; i <= 8; i++ ) {
			const Scalar t = i + 0.5;
			const int cur = axis == 0 ? WhichColor( *checker, t, 0.5 )
									  : WhichColor( *checker, 0.5, t );
			if( prev >= 0 && cur == prev ) {
				std::cout << "    FAIL: axis=" << axis << " no alternation at cell " << i << std::endl;
				passed = false;
			}
			prev = cur;
		}
	}

	checker->release();
	white->release();
	black->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestCheckerQuadrantConsistency()
{
	std::cout << "  Test 2: Checker parity consistent across the origin..." << std::endl;

	UniformColorPainter* black = new UniformColorPainter( RISEPel( 0, 0, 0 ) );
	UniformColorPainter* white = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	CheckerPainter* checker = new CheckerPainter( 1.0, *black, *white );

	bool passed = true;
	// the four cells meeting at the origin: diagonal pairs match,
	// edge-adjacent pairs differ
	const int pp = WhichColor( *checker,  0.5,  0.5 );
	const int np = WhichColor( *checker, -0.5,  0.5 );
	const int pn = WhichColor( *checker,  0.5, -0.5 );
	const int nn = WhichColor( *checker, -0.5, -0.5 );
	if( pp != nn || np != pn || pp == np ) {
		std::cout << "    FAIL: origin cells (++, -+, +-, --) = "
			<< pp << " " << np << " " << pn << " " << nn << std::endl;
		passed = false;
	}

	// deep in the negative quadrant the pattern must still alternate
	// (old code: every cell there classified "odd" -> solid color)
	const int a1 = WhichColor( *checker, -5.5, -5.5 );
	const int a2 = WhichColor( *checker, -4.5, -5.5 );
	const int a3 = WhichColor( *checker, -4.5, -4.5 );
	if( a1 == a2 || a1 != a3 ) {
		std::cout << "    FAIL: negative quadrant not alternating: "
			<< a1 << " " << a2 << " " << a3 << std::endl;
		passed = false;
	}

	checker->release();
	white->release();
	black->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestLinesUniformWidth()
{
	std::cout << "  Test 3: Lines uniform stripe width across the origin..." << std::endl;

	UniformColorPainter* black = new UniformColorPainter( RISEPel( 0, 0, 0 ) );
	UniformColorPainter* white = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	LinesPainter* lines = new LinesPainter( 1.0, *black, *white, true );

	bool passed = true;
	// stripe k covers [k, k+1); sample each stripe center from -8 to 7
	int prev = -1;
	for( int i = -8; i <= 7; i++ ) {
		const int cur = WhichColor( *lines, i + 0.5, 0.0 );
		if( prev >= 0 && cur == prev ) {
			std::cout << "    FAIL: no alternation at stripe " << i
				<< " (old code doubled the origin stripe)" << std::endl;
			passed = false;
		}
		prev = cur;
	}

	lines->release();
	white->release();
	black->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestLinesDeepNegative()
{
	std::cout << "  Test 4: Lines parity deep in the negative range..." << std::endl;

	UniformColorPainter* black = new UniformColorPainter( RISEPel( 0, 0, 0 ) );
	UniformColorPainter* white = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	LinesPainter* lines = new LinesPainter( 0.25, *black, *white, false );

	bool passed = true;
	// with dSize 0.25, stripe k covers [k*0.25, (k+1)*0.25); check a
	// matched positive/negative pair of even-parity stripes agrees
	const int deepNeg = WhichColor( *lines, 0.0, -100.0 + 0.125 );	// stripe -400 (even)
	const int deepPos = WhichColor( *lines, 0.0,  100.0 + 0.125 );	// stripe  400 (even)
	if( deepNeg != deepPos ) {
		std::cout << "    FAIL: even stripes at -100 and +100 disagree" << std::endl;
		passed = false;
	}

	lines->release();
	white->release();
	black->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

int main()
{
	std::cout << "=== PatternPainterParity Tests ===" << std::endl;
	bool allPassed = true;

	allPassed &= TestCheckerAlternation();
	allPassed &= TestCheckerQuadrantConsistency();
	allPassed &= TestLinesUniformWidth();
	allPassed &= TestLinesDeepNegative();

	std::cout << std::endl;
	if( allPassed )
		std::cout << "ALL TESTS PASSED" << std::endl;
	else
		std::cout << "SOME TESTS FAILED" << std::endl;

	return allPassed ? 0 : 1;
}
