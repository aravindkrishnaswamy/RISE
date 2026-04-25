//////////////////////////////////////////////////////////////////////
//
//  GaborNoise3DTest.cpp - Unit tests for GaborNoise3D
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

#include "../src/Library/Noise/GaborNoise.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "ProceduralTestHelpers.h"

using namespace RISE;
using namespace RISE::Implementation;
using ProceduralTestHelpers::CountDifferentSamples;
using ProceduralTestHelpers::IsClose;
using ProceduralTestHelpers::IsInUnitInterval;

bool TestOutputRange()
{
	std::cout << "  Test 1: Output range [0,1]..." << std::endl;

	GaborNoise3D* noise = new GaborNoise3D( 4.0, 1.0, Vector3(0,1,0), 4.0, 42 );

	bool passed = true;
	for( int i = -30; i < 30; i++ ) {
		for( int j = -5; j < 5; j++ ) {
			Scalar val = noise->Evaluate( i * 0.37, j * 0.41, (i+j) * 0.29 );
			if( !IsInUnitInterval( val ) ) {
				std::cout << "    FAIL: out of range " << val << std::endl;
				passed = false;
				break;
			}
		}
		if( !passed ) break;
	}

	noise->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestSpatialVariation()
{
	std::cout << "  Test 2: Spatial variation..." << std::endl;

	GaborNoise3D* noise = new GaborNoise3D( 4.0, 1.0, Vector3(0,1,0), 4.0, 42 );

	Scalar minVal = 1e20, maxVal = -1e20;
	for( int i = 0; i < 100; i++ ) {
		Scalar val = noise->Evaluate( i * 0.3, i * 0.7, i * 0.5 );
		if( val < minVal ) minVal = val;
		if( val > maxVal ) maxVal = val;
	}

	bool passed = (maxVal - minVal) > 0.05;
	noise->release();
	if( !passed )
		std::cout << "    FAIL: range=" << (maxVal-minVal) << std::endl;
	else
		std::cout << "    PASSED (range=" << (maxVal-minVal) << ")" << std::endl;
	return passed;
}

bool TestDeterministic()
{
	std::cout << "  Test 3: Deterministic..." << std::endl;

	GaborNoise3D* noise = new GaborNoise3D( 4.0, 1.0, Vector3(0,1,0), 4.0, 42 );

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
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestDifferentOrientations()
{
	std::cout << "  Test 4: Different orientations differ..." << std::endl;

	GaborNoise3D* noiseA = new GaborNoise3D( 4.0, 1.0, Vector3(1,0,0), 4.0, 42 );
	GaborNoise3D* noiseB = new GaborNoise3D( 4.0, 1.0, Vector3(0,1,0), 4.0, 42 );

	const int differCount = CountDifferentSamples( 30,
		[&]( int i ) { return noiseA->Evaluate( i * 0.5 + 0.1, i * 0.3, i * 0.7 ); },
		[&]( int i ) { return noiseB->Evaluate( i * 0.5 + 0.1, i * 0.3, i * 0.7 ); },
		1e-4 );

	bool passed = differCount > 10;
	noiseA->release();
	noiseB->release();
	if( !passed )
		std::cout << "    FAIL: only " << differCount << "/30 differ" << std::endl;
	else
		std::cout << "    PASSED (" << differCount << "/30 differ)" << std::endl;
	return passed;
}

bool TestDifferentFrequencies()
{
	std::cout << "  Test 5: Different frequencies differ..." << std::endl;

	GaborNoise3D* noiseA = new GaborNoise3D( 2.0, 1.0, Vector3(0,1,0), 4.0, 42 );
	GaborNoise3D* noiseB = new GaborNoise3D( 8.0, 1.0, Vector3(0,1,0), 4.0, 42 );

	const int differCount = CountDifferentSamples( 30,
		[&]( int i ) { return noiseA->Evaluate( i * 0.5, i * 0.3, i * 0.7 ); },
		[&]( int i ) { return noiseB->Evaluate( i * 0.5, i * 0.3, i * 0.7 ); },
		1e-4 );

	bool passed = differCount > 10;
	noiseA->release();
	noiseB->release();
	if( !passed )
		std::cout << "    FAIL: only " << differCount << "/30 differ" << std::endl;
	else
		std::cout << "    PASSED (" << differCount << "/30 differ)" << std::endl;
	return passed;
}

bool TestZeroOrientationFallsBackToXAxis()
{
	std::cout << "  Test 6: Zero orientation falls back to X axis..." << std::endl;

	GaborNoise3D* fallback = new GaborNoise3D( 4.0, 1.0, Vector3(0,0,0), 4.0, 42 );
	GaborNoise3D* xAxis = new GaborNoise3D( 4.0, 1.0, Vector3(1,0,0), 4.0, 42 );

	bool passed = true;
	for( int i = 0; i < 20; i++ ) {
		Scalar x = i * 0.4 + 0.15;
		Scalar y = i * 0.6 + 0.25;
		Scalar z = i * 0.8 + 0.35;

		const Scalar vFallback = fallback->Evaluate( x, y, z );
		const Scalar vXAxis = xAxis->Evaluate( x, y, z );
		if( !IsClose( vFallback, vXAxis, 1e-6 ) ) {
			std::cout << "    FAIL: fallback=" << vFallback << " xAxis=" << vXAxis << std::endl;
			passed = false;
			break;
		}
	}

	fallback->release();
	xAxis->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestNegativeCoordinates()
{
	std::cout << "  Test 7: Negative coordinates..." << std::endl;

	GaborNoise3D* noise = new GaborNoise3D( 4.0, 1.0, Vector3(0,1,0), 4.0, 42 );

	Scalar val = noise->Evaluate( -10.5, -20.3, -5.7 );
	bool passed = IsInUnitInterval( val );
	noise->release();
	if( !passed )
		std::cout << "    FAIL: val=" << val << std::endl;
	else
		std::cout << "    PASSED (val=" << val << ")" << std::endl;
	return passed;
}

int main()
{
	std::cout << "=== GaborNoise3D Tests ===" << std::endl;
	bool allPassed = true;

	allPassed &= TestOutputRange();
	allPassed &= TestSpatialVariation();
	allPassed &= TestDeterministic();
	allPassed &= TestDifferentOrientations();
	allPassed &= TestDifferentFrequencies();
	allPassed &= TestZeroOrientationFallsBackToXAxis();
	allPassed &= TestNegativeCoordinates();

	std::cout << std::endl;
	if( allPassed )
		std::cout << "ALL TESTS PASSED" << std::endl;
	else
		std::cout << "SOME TESTS FAILED" << std::endl;

	return allPassed ? 0 : 1;
}
