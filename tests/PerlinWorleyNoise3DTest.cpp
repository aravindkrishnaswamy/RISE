//////////////////////////////////////////////////////////////////////
//
//  PerlinWorleyNoise3DTest.cpp - Unit tests for PerlinWorleyNoise3D
//
//  Tests:
//    1. Output is in [0, 1] range
//    2. Output varies spatially
//    3. Deterministic output
//    4. Blend=0 matches Perlin behavior
//    5. Blend=1 matches inverted Worley behavior
//    6. Different blend values produce different outputs
//    7. Negative coordinates work correctly
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>

#include "../src/Library/Noise/PerlinWorleyNoise.h"
#include "../src/Library/Noise/PerlinNoise.h"
#include "../src/Library/Noise/WorleyNoise.h"
#include "../src/Library/Utilities/SimpleInterpolators.h"

using namespace RISE;
using namespace RISE::Implementation;

static bool IsClose( double a, double b, double tol = 1e-6 )
{
	return fabs( a - b ) < tol;
}

bool TestOutputRange()
{
	std::cout << "  Test 1: Output range [0,1]..." << std::endl;

	RealLinearInterpolator interp;
	PerlinWorleyNoise3D noise( interp, 0.65, 4, 1.0, 0.5 );

	bool passed = true;
	for( int i = -50; i < 50; i++ ) {
		for( int j = -10; j < 10; j++ ) {
			Scalar val = noise.Evaluate( i * 0.37, j * 0.41, (i+j) * 0.29 );
			if( val < -1e-10 || val > 1.0 + 1e-6 ) {
				std::cout << "    FAIL: out of range " << val << std::endl;
				passed = false;
				break;
			}
		}
		if( !passed ) break;
	}

	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestSpatialVariation()
{
	std::cout << "  Test 2: Spatial variation..." << std::endl;

	RealLinearInterpolator interp;
	PerlinWorleyNoise3D noise( interp, 0.65, 4, 1.0, 0.5 );

	Scalar minVal = 1e20, maxVal = -1e20;
	for( int i = 0; i < 100; i++ ) {
		Scalar val = noise.Evaluate( i * 0.7, i * 1.3, i * 0.5 );
		if( val < minVal ) minVal = val;
		if( val > maxVal ) maxVal = val;
	}

	bool passed = (maxVal - minVal) > 0.1;
	if( !passed )
		std::cout << "    FAIL: range=" << (maxVal-minVal) << std::endl;
	else
		std::cout << "    PASSED (range=" << (maxVal-minVal) << ")" << std::endl;
	return passed;
}

bool TestDeterministic()
{
	std::cout << "  Test 3: Deterministic..." << std::endl;

	RealLinearInterpolator interp;
	PerlinWorleyNoise3D noise( interp, 0.65, 4, 1.0, 0.5 );

	bool passed = true;
	for( int i = 0; i < 20; i++ ) {
		Scalar x = i * 3.7, y = i * 2.1, z = i * 1.3;
		Scalar v1 = noise.Evaluate( x, y, z );
		Scalar v2 = noise.Evaluate( x, y, z );
		if( !IsClose( v1, v2 ) ) {
			std::cout << "    FAIL: non-deterministic" << std::endl;
			passed = false;
			break;
		}
	}

	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestBlendZeroMatchesPerlin()
{
	std::cout << "  Test 4: Blend=0 matches Perlin..." << std::endl;

	RealLinearInterpolator interp;
	PerlinWorleyNoise3D hybrid( interp, 0.65, 4, 1.0, 0.0 ); // blend=0 = pure Perlin
	PerlinNoise3D perlin( interp, 0.65, 4 );

	bool passed = true;
	for( int i = 0; i < 30; i++ ) {
		Scalar x = i * 1.7 + 0.5, y = i * 2.3 - 1.0, z = i * 0.9 + 2.0;
		Scalar hVal = hybrid.Evaluate( x, y, z );
		Scalar pVal = (perlin.Evaluate( x, y, z ) + 1.0) / 2.0;
		if( pVal < 0.0 ) pVal = 0.0;
		if( pVal > 1.0 ) pVal = 1.0;
		if( !IsClose( hVal, pVal, 1e-4 ) ) {
			std::cout << "    FAIL: hybrid=" << hVal << " perlin=" << pVal << std::endl;
			passed = false;
			break;
		}
	}

	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestBlendOneMatchesWorley()
{
	std::cout << "  Test 5: Blend=1 matches inverted Worley..." << std::endl;

	RealLinearInterpolator interp;
	PerlinWorleyNoise3D hybrid( interp, 0.65, 4, 1.0, 1.0 ); // blend=1 = pure Worley
	WorleyNoise3D worley( 1.0, eWorley_Euclidean, eWorley_F1 );

	bool passed = true;
	for( int i = 0; i < 30; i++ ) {
		Scalar x = i * 1.7 + 0.5, y = i * 2.3 - 1.0, z = i * 0.9 + 2.0;
		Scalar hVal = hybrid.Evaluate( x, y, z );
		Scalar wVal = 1.0 - worley.Evaluate( x, y, z );
		if( !IsClose( hVal, wVal, 1e-4 ) ) {
			std::cout << "    FAIL: hybrid=" << hVal << " invWorley=" << wVal << std::endl;
			passed = false;
			break;
		}
	}

	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestDifferentBlends()
{
	std::cout << "  Test 6: Different blends produce different outputs..." << std::endl;

	RealLinearInterpolator interp;
	PerlinWorleyNoise3D noiseA( interp, 0.65, 4, 1.0, 0.2 );
	PerlinWorleyNoise3D noiseB( interp, 0.65, 4, 1.0, 0.8 );

	int differCount = 0;
	for( int i = 0; i < 30; i++ ) {
		Scalar x = i * 1.3, y = i * 0.9, z = i * 2.1;
		if( !IsClose( noiseA.Evaluate(x,y,z), noiseB.Evaluate(x,y,z), 1e-4 ) )
			differCount++;
	}

	bool passed = differCount > 15;
	if( !passed )
		std::cout << "    FAIL: only " << differCount << "/30 differ" << std::endl;
	else
		std::cout << "    PASSED (" << differCount << "/30 differ)" << std::endl;
	return passed;
}

bool TestNegativeCoordinates()
{
	std::cout << "  Test 7: Negative coordinates..." << std::endl;

	RealLinearInterpolator interp;
	PerlinWorleyNoise3D noise( interp, 0.65, 4, 1.0, 0.5 );

	Scalar val = noise.Evaluate( -10.5, -20.3, -5.7 );
	bool passed = val >= -1e-10 && val <= 1.0 + 1e-6;
	if( !passed )
		std::cout << "    FAIL: val=" << val << std::endl;
	else
		std::cout << "    PASSED (val=" << val << ")" << std::endl;
	return passed;
}

int main()
{
	std::cout << "=== PerlinWorleyNoise3D Tests ===" << std::endl;
	bool allPassed = true;

	allPassed &= TestOutputRange();
	allPassed &= TestSpatialVariation();
	allPassed &= TestDeterministic();
	allPassed &= TestBlendZeroMatchesPerlin();
	allPassed &= TestBlendOneMatchesWorley();
	allPassed &= TestDifferentBlends();
	allPassed &= TestNegativeCoordinates();

	std::cout << std::endl;
	if( allPassed )
		std::cout << "ALL TESTS PASSED" << std::endl;
	else
		std::cout << "SOME TESTS FAILED" << std::endl;

	return allPassed ? 0 : 1;
}
