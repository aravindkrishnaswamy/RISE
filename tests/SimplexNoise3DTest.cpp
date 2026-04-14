//////////////////////////////////////////////////////////////////////
//
//  SimplexNoise3DTest.cpp - Unit tests for SimplexNoise3D
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

#include "../src/Library/Noise/SimplexNoise.h"

using namespace RISE;
using namespace RISE::Implementation;

static bool IsClose( double a, double b, double tol = 1e-6 )
{
	return fabs( a - b ) < tol;
}

bool TestOutputRange()
{
	std::cout << "  Test 1: Output range [0,1]..." << std::endl;

	SimplexNoise3D noise( 0.5, 4 );

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

	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestSpatialVariation()
{
	std::cout << "  Test 2: Spatial variation..." << std::endl;

	SimplexNoise3D noise( 0.5, 4 );

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

	SimplexNoise3D noise( 0.65, 4 );

	bool passed = true;
	for( int i = 0; i < 20; i++ ) {
		Scalar x = i * 3.7, y = i * 2.1, z = i * 1.3;
		if( !IsClose( noise.Evaluate(x,y,z), noise.Evaluate(x,y,z) ) ) {
			std::cout << "    FAIL: non-deterministic" << std::endl;
			passed = false;
			break;
		}
	}

	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestDifferentPersistence()
{
	std::cout << "  Test 4: Different persistence values differ..." << std::endl;

	SimplexNoise3D noiseA( 0.3, 4 );
	SimplexNoise3D noiseB( 0.9, 4 );

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

bool TestDifferentOctaves()
{
	std::cout << "  Test 5: Different octave counts differ..." << std::endl;

	SimplexNoise3D noiseA( 0.5, 1 );
	SimplexNoise3D noiseB( 0.5, 6 );

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
	std::cout << "  Test 6: Negative coordinates..." << std::endl;

	SimplexNoise3D noise( 0.65, 4 );

	Scalar val = noise.Evaluate( -10.5, -20.3, -5.7 );
	bool passed = val >= -1e-10 && val <= 1.0 + 1e-6;
	if( !passed )
		std::cout << "    FAIL: val=" << val << std::endl;
	else
		std::cout << "    PASSED (val=" << val << ")" << std::endl;
	return passed;
}

bool TestSymmetry()
{
	std::cout << "  Test 7: No axis-aligned symmetry..." << std::endl;

	SimplexNoise3D noise( 0.65, 4 );

	// Simplex noise should NOT be symmetric along axes
	int differCount = 0;
	for( int i = 1; i < 20; i++ ) {
		Scalar x = i * 1.7;
		Scalar vPos = noise.Evaluate( x, 0.5, 0.5 );
		Scalar vSwap = noise.Evaluate( 0.5, x, 0.5 );
		if( !IsClose( vPos, vSwap, 1e-4 ) ) differCount++;
	}

	bool passed = differCount > 10;
	if( !passed )
		std::cout << "    FAIL: too many axis-aligned symmetries (" << differCount << " differ)" << std::endl;
	else
		std::cout << "    PASSED (" << differCount << "/19 points differ across axes)" << std::endl;
	return passed;
}

int main()
{
	std::cout << "=== SimplexNoise3D Tests ===" << std::endl;
	bool allPassed = true;

	allPassed &= TestOutputRange();
	allPassed &= TestSpatialVariation();
	allPassed &= TestDeterministic();
	allPassed &= TestDifferentPersistence();
	allPassed &= TestDifferentOctaves();
	allPassed &= TestNegativeCoordinates();
	allPassed &= TestSymmetry();

	std::cout << std::endl;
	if( allPassed )
		std::cout << "ALL TESTS PASSED" << std::endl;
	else
		std::cout << "SOME TESTS FAILED" << std::endl;

	return allPassed ? 0 : 1;
}
