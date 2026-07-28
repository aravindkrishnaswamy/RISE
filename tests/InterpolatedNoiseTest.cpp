//////////////////////////////////////////////////////////////////////
//
//  InterpolatedNoiseTest.cpp - Unit tests for the classic lattice
//  noise (InterpolatedNoise1D/2D/3D and the NoiseUtils.h header
//  functions), focused on signed-coordinate correctness.
//
//  Tests:
//    1. 1D matches manual floor-based lattice interpolation for
//       negative and positive coordinates
//    2. 1D continuity across integer boundaries (incl. negative)
//    3. 2D continuity across integer boundaries (incl. negative)
//    4. 3D continuity across integer boundaries (incl. negative)
//    5. 2D/3D linear interpolation stays within lattice corner bounds
//       (extrapolation from a negative fraction violates this)
//    6. NoiseUtils.h InterpolatedNoise1/2 continuity across negative
//       integer boundaries
//
//  Guards the negative-fraction modf defect: modf(x)/int-cast on a
//  negative coordinate yields a fraction in (-1,0] and a lattice cell
//  shifted by one, so interpolation becomes extrapolation from the
//  wrong cell and the noise field is discontinuous at every negative
//  integer boundary.
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

#include "../src/Library/Noise/InterpolatedNoise.h"
#include "../src/Library/Noise/NoiseUtils.h"
#include "../src/Library/Utilities/SimpleInterpolators.h"
#include "ProceduralTestHelpers.h"

using namespace RISE;
using namespace RISE::Implementation;
using ProceduralTestHelpers::IsClose;

namespace
{
	// Boundary points spanning negative, zero, and positive lattice lines
	const int kBoundaries[] = { -7, -3, -1, 0, 1, 5 };
	const int kNumBoundaries = int( sizeof(kBoundaries) / sizeof(kBoundaries[0]) );

	// Small offset for boundary-continuity checks.  The lattice values are
	// O(1) so the interpolated field's slope is O(1); 1e-6 in gives ~1e-6
	// out for linear interpolation, tighter for cosine.
	const Scalar kEps = 1e-6;
	const Scalar kContinuityTol = 1e-4;
}

bool TestManualInterpolation1D()
{
	std::cout << "  Test 1: 1D matches manual floor-based interpolation..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	InterpolatedNoise1D* noise = new InterpolatedNoise1D( *interp );
	SmoothedNoise1D* smoothed = new SmoothedNoise1D();

	bool passed = true;
	const Scalar samples[] = { -7.75, -3.25, -0.5, -0.001, 0.001, 0.5, 3.25, 7.75 };
	for( unsigned int i = 0; i < sizeof(samples)/sizeof(samples[0]); i++ ) {
		const Scalar x = samples[i];
		const Scalar fx = floor( x );
		const Scalar t = x - fx;
		const int n = int( fx );
		const Scalar expected = smoothed->Evaluate( n ) * (1.0 - t) + smoothed->Evaluate( n + 1 ) * t;
		const Scalar actual = noise->Evaluate( x );
		if( !IsClose( actual, expected, 1e-9 ) ) {
			std::cout << "    FAIL: x=" << x << " expected=" << expected << " actual=" << actual << std::endl;
			passed = false;
		}
	}

	smoothed->release();
	noise->release();
	interp->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestBoundaryContinuity1D()
{
	std::cout << "  Test 2: 1D continuity across integer boundaries..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	InterpolatedNoise1D* noise = new InterpolatedNoise1D( *interp );

	bool passed = true;
	for( int i = 0; i < kNumBoundaries; i++ ) {
		const Scalar n = Scalar( kBoundaries[i] );
		const Scalar at = noise->Evaluate( n );
		const Scalar below = noise->Evaluate( n - kEps );
		const Scalar above = noise->Evaluate( n + kEps );
		if( fabs( at - below ) > kContinuityTol || fabs( at - above ) > kContinuityTol ) {
			std::cout << "    FAIL: n=" << n << " at=" << at
				<< " below=" << below << " above=" << above << std::endl;
			passed = false;
		}
	}

	noise->release();
	interp->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestBoundaryContinuity2D()
{
	std::cout << "  Test 3: 2D continuity across integer boundaries..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	InterpolatedNoise2D* noise = new InterpolatedNoise2D( *interp );

	bool passed = true;
	for( int i = 0; i < kNumBoundaries; i++ ) {
		const Scalar n = Scalar( kBoundaries[i] );
		// walk the boundary along both axes at an off-lattice ordinate
		const Scalar at1 = noise->Evaluate( n, -2.63 );
		const Scalar below1 = noise->Evaluate( n - kEps, -2.63 );
		const Scalar above1 = noise->Evaluate( n + kEps, -2.63 );
		const Scalar at2 = noise->Evaluate( -2.63, n );
		const Scalar below2 = noise->Evaluate( -2.63, n - kEps );
		const Scalar above2 = noise->Evaluate( -2.63, n + kEps );
		if( fabs( at1 - below1 ) > kContinuityTol || fabs( at1 - above1 ) > kContinuityTol ||
			fabs( at2 - below2 ) > kContinuityTol || fabs( at2 - above2 ) > kContinuityTol ) {
			std::cout << "    FAIL: n=" << n << std::endl;
			passed = false;
		}
	}

	noise->release();
	interp->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestBoundaryContinuity3D()
{
	std::cout << "  Test 4: 3D continuity across integer boundaries..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	InterpolatedNoise3D* noise = new InterpolatedNoise3D( *interp );

	bool passed = true;
	for( int i = 0; i < kNumBoundaries; i++ ) {
		const Scalar n = Scalar( kBoundaries[i] );
		const Scalar probes[3][3] = {
			{ n, -2.63, -4.37 },
			{ -2.63, n, -4.37 },
			{ -2.63, -4.37, n },
		};
		for( int axis = 0; axis < 3; axis++ ) {
			Scalar pAt[3], pBelow[3], pAbove[3];
			for( int c = 0; c < 3; c++ ) {
				pAt[c] = probes[axis][c];
				pBelow[c] = probes[axis][c];
				pAbove[c] = probes[axis][c];
			}
			pBelow[axis] -= kEps;
			pAbove[axis] += kEps;
			const Scalar at = noise->Evaluate( pAt[0], pAt[1], pAt[2] );
			const Scalar below = noise->Evaluate( pBelow[0], pBelow[1], pBelow[2] );
			const Scalar above = noise->Evaluate( pAbove[0], pAbove[1], pAbove[2] );
			if( fabs( at - below ) > kContinuityTol || fabs( at - above ) > kContinuityTol ) {
				std::cout << "    FAIL: n=" << n << " axis=" << axis << std::endl;
				passed = false;
			}
		}
	}

	noise->release();
	interp->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestCornerBounds()
{
	std::cout << "  Test 5: 2D/3D linear interpolation within corner bounds..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	InterpolatedNoise2D* noise2 = new InterpolatedNoise2D( *interp );
	InterpolatedNoise3D* noise3 = new InterpolatedNoise3D( *interp );
	SmoothedNoise2D* smoothed2 = new SmoothedNoise2D();
	SmoothedNoise3D* smoothed3 = new SmoothedNoise3D();

	bool passed = true;
	for( int i = -12; i < 12; i++ ) {
		const Scalar x = i * 0.73 - 0.31;
		const Scalar y = i * -0.57 + 0.11;
		const Scalar z = i * 0.39 - 0.83;
		const int nx = int( floor( x ) );
		const int ny = int( floor( y ) );
		const int nz = int( floor( z ) );

		// 2D: value must lie within [min, max] of the 4 cell corners
		{
			Scalar mn = 1e20, mx = -1e20;
			for( int dx = 0; dx <= 1; dx++ ) {
				for( int dy = 0; dy <= 1; dy++ ) {
					const Scalar v = smoothed2->Evaluate( nx + dx, ny + dy );
					if( v < mn ) mn = v;
					if( v > mx ) mx = v;
				}
			}
			const Scalar val = noise2->Evaluate( x, y );
			if( val < mn - 1e-9 || val > mx + 1e-9 ) {
				std::cout << "    FAIL 2D: (" << x << "," << y << ") val=" << val
					<< " bounds=[" << mn << "," << mx << "]" << std::endl;
				passed = false;
			}
		}

		// 3D: value must lie within [min, max] of the 8 cell corners
		{
			Scalar mn = 1e20, mx = -1e20;
			for( int dx = 0; dx <= 1; dx++ ) {
				for( int dy = 0; dy <= 1; dy++ ) {
					for( int dz = 0; dz <= 1; dz++ ) {
						const Scalar v = smoothed3->Evaluate( nx + dx, ny + dy, nz + dz );
						if( v < mn ) mn = v;
						if( v > mx ) mx = v;
					}
				}
			}
			const Scalar val = noise3->Evaluate( x, y, z );
			if( val < mn - 1e-9 || val > mx + 1e-9 ) {
				std::cout << "    FAIL 3D: (" << x << "," << y << "," << z << ") val=" << val
					<< " bounds=[" << mn << "," << mx << "]" << std::endl;
				passed = false;
			}
		}
	}

	smoothed3->release();
	smoothed2->release();
	noise3->release();
	noise2->release();
	interp->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestNoiseUtilsContinuity()
{
	std::cout << "  Test 6: NoiseUtils InterpolatedNoise1/2 boundary continuity..." << std::endl;

	bool passed = true;
	for( int i = 0; i < kNumBoundaries; i++ ) {
		const Scalar n = Scalar( kBoundaries[i] );

		const Scalar at1 = InterpolatedNoise1( n );
		const Scalar below1 = InterpolatedNoise1( n - kEps );
		const Scalar above1 = InterpolatedNoise1( n + kEps );
		if( fabs( at1 - below1 ) > kContinuityTol || fabs( at1 - above1 ) > kContinuityTol ) {
			std::cout << "    FAIL 1D: n=" << n << " at=" << at1
				<< " below=" << below1 << " above=" << above1 << std::endl;
			passed = false;
		}

		const Scalar at2 = InterpolatedNoise2( n, -2.63 );
		const Scalar below2 = InterpolatedNoise2( n - kEps, -2.63 );
		const Scalar above2 = InterpolatedNoise2( n + kEps, -2.63 );
		const Scalar at3 = InterpolatedNoise2( -2.63, n );
		const Scalar below3 = InterpolatedNoise2( -2.63, n - kEps );
		const Scalar above3 = InterpolatedNoise2( -2.63, n + kEps );
		if( fabs( at2 - below2 ) > kContinuityTol || fabs( at2 - above2 ) > kContinuityTol ||
			fabs( at3 - below3 ) > kContinuityTol || fabs( at3 - above3 ) > kContinuityTol ) {
			std::cout << "    FAIL 2D: n=" << n << std::endl;
			passed = false;
		}
	}

	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

int main()
{
	std::cout << "=== InterpolatedNoise Tests ===" << std::endl;
	bool allPassed = true;

	allPassed &= TestManualInterpolation1D();
	allPassed &= TestBoundaryContinuity1D();
	allPassed &= TestBoundaryContinuity2D();
	allPassed &= TestBoundaryContinuity3D();
	allPassed &= TestCornerBounds();
	allPassed &= TestNoiseUtilsContinuity();

	std::cout << std::endl;
	if( allPassed )
		std::cout << "ALL TESTS PASSED" << std::endl;
	else
		std::cout << "SOME TESTS FAILED" << std::endl;

	return allPassed ? 0 : 1;
}
