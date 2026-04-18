//////////////////////////////////////////////////////////////////////
//
//  DomainWarpNoise3DTest.cpp - Unit tests for DomainWarpNoise3D
//
//  Tests:
//    1. Output is in [0, 1] range
//    2. Output varies spatially
//    3. Deterministic output
//    4. Warp levels=0 matches plain Perlin
//    5. Different warp amplitudes produce different outputs
//    6. Different warp levels produce different outputs
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

#include "../src/Library/Noise/DomainWarpNoise.h"
#include "../src/Library/Noise/PerlinNoise.h"
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

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	DomainWarpNoise3D* noise = new DomainWarpNoise3D( *interp, 0.65, 4, 4.0, 2 );

	bool passed = true;
	for( int i = -50; i < 50; i++ ) {
		for( int j = -10; j < 10; j++ ) {
			Scalar val = noise->Evaluate( i * 0.37, j * 0.41, (i+j) * 0.29 );
			if( val < -1e-10 || val > 1.0 + 1e-6 ) {
				std::cout << "    FAIL: out of range " << val << std::endl;
				passed = false;
				break;
			}
		}
		if( !passed ) break;
	}

	noise->release();
	interp->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestSpatialVariation()
{
	std::cout << "  Test 2: Spatial variation..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	DomainWarpNoise3D* noise = new DomainWarpNoise3D( *interp, 0.65, 4, 4.0, 2 );

	Scalar minVal = 1e20, maxVal = -1e20;
	for( int i = 0; i < 100; i++ ) {
		Scalar val = noise->Evaluate( i * 0.7, i * 1.3, i * 0.5 );
		if( val < minVal ) minVal = val;
		if( val > maxVal ) maxVal = val;
	}

	bool passed = (maxVal - minVal) > 0.1;
	noise->release();
	interp->release();
	if( !passed )
		std::cout << "    FAIL: range=" << (maxVal-minVal) << std::endl;
	else
		std::cout << "    PASSED (range=" << (maxVal-minVal) << ")" << std::endl;
	return passed;
}

bool TestDeterministic()
{
	std::cout << "  Test 3: Deterministic..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	DomainWarpNoise3D* noise = new DomainWarpNoise3D( *interp, 0.65, 4, 4.0, 2 );

	bool passed = true;
	for( int i = 0; i < 20; i++ ) {
		Scalar x = i * 3.7, y = i * 2.1, z = i * 1.3;
		if( !IsClose( noise->Evaluate(x,y,z), noise->Evaluate(x,y,z) ) ) {
			std::cout << "    FAIL: non-deterministic" << std::endl;
			passed = false;
			break;
		}
	}

	noise->release();
	interp->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestZeroLevelsMatchesPerlin()
{
	std::cout << "  Test 4: Warp levels=0 matches Perlin..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	DomainWarpNoise3D* warp = new DomainWarpNoise3D( *interp, 0.65, 4, 4.0, 0 );
	PerlinNoise3D* perlin = new PerlinNoise3D( *interp, 0.65, 4 );

	bool passed = true;
	for( int i = 0; i < 30; i++ ) {
		Scalar x = i * 1.7 + 0.5, y = i * 2.3 - 1.0, z = i * 0.9 + 2.0;
		Scalar wVal = warp->Evaluate( x, y, z );
		Scalar pVal = (perlin->Evaluate( x, y, z ) + 1.0) / 2.0;
		if( pVal < 0.0 ) pVal = 0.0;
		if( pVal > 1.0 ) pVal = 1.0;
		if( !IsClose( wVal, pVal, 1e-4 ) ) {
			std::cout << "    FAIL: warp=" << wVal << " perlin=" << pVal << std::endl;
			passed = false;
			break;
		}
	}

	warp->release();
	perlin->release();
	interp->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestDifferentAmplitudes()
{
	std::cout << "  Test 5: Different amplitudes..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	DomainWarpNoise3D* noiseA = new DomainWarpNoise3D( *interp, 0.65, 4, 1.0, 2 );
	DomainWarpNoise3D* noiseB = new DomainWarpNoise3D( *interp, 0.65, 4, 8.0, 2 );

	int differCount = 0;
	for( int i = 0; i < 30; i++ ) {
		Scalar x = i * 1.3, y = i * 0.9, z = i * 2.1;
		if( !IsClose( noiseA->Evaluate(x,y,z), noiseB->Evaluate(x,y,z), 1e-4 ) )
			differCount++;
	}

	bool passed = differCount > 15;
	noiseA->release();
	noiseB->release();
	interp->release();
	if( !passed )
		std::cout << "    FAIL: only " << differCount << "/30 differ" << std::endl;
	else
		std::cout << "    PASSED (" << differCount << "/30 differ)" << std::endl;
	return passed;
}

bool TestDifferentLevels()
{
	std::cout << "  Test 6: Different warp levels..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	DomainWarpNoise3D* noise1 = new DomainWarpNoise3D( *interp, 0.65, 4, 4.0, 1 );
	DomainWarpNoise3D* noise3 = new DomainWarpNoise3D( *interp, 0.65, 4, 4.0, 3 );

	int differCount = 0;
	for( int i = 0; i < 30; i++ ) {
		Scalar x = i * 1.3, y = i * 0.9, z = i * 2.1;
		if( !IsClose( noise1->Evaluate(x,y,z), noise3->Evaluate(x,y,z), 1e-4 ) )
			differCount++;
	}

	bool passed = differCount > 15;
	noise1->release();
	noise3->release();
	interp->release();
	if( !passed )
		std::cout << "    FAIL: only " << differCount << "/30 differ" << std::endl;
	else
		std::cout << "    PASSED (" << differCount << "/30 differ)" << std::endl;
	return passed;
}

bool TestNegativeCoordinates()
{
	std::cout << "  Test 7: Negative coordinates..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	DomainWarpNoise3D* noise = new DomainWarpNoise3D( *interp, 0.65, 4, 4.0, 2 );

	Scalar val = noise->Evaluate( -10.5, -20.3, -5.7 );
	bool passed = val >= -1e-10 && val <= 1.0 + 1e-6;
	noise->release();
	interp->release();
	if( !passed )
		std::cout << "    FAIL: val=" << val << std::endl;
	else
		std::cout << "    PASSED (val=" << val << ")" << std::endl;
	return passed;
}

int main()
{
	std::cout << "=== DomainWarpNoise3D Tests ===" << std::endl;
	bool allPassed = true;

	allPassed &= TestOutputRange();
	allPassed &= TestSpatialVariation();
	allPassed &= TestDeterministic();
	allPassed &= TestZeroLevelsMatchesPerlin();
	allPassed &= TestDifferentAmplitudes();
	allPassed &= TestDifferentLevels();
	allPassed &= TestNegativeCoordinates();

	std::cout << std::endl;
	if( allPassed )
		std::cout << "ALL TESTS PASSED" << std::endl;
	else
		std::cout << "SOME TESTS FAILED" << std::endl;

	return allPassed ? 0 : 1;
}
