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
#include "ProceduralTestHelpers.h"

using namespace RISE;
using namespace RISE::Implementation;
using ProceduralTestHelpers::CountDifferentSamples;
using ProceduralTestHelpers::IsClose;
using ProceduralTestHelpers::IsInUnitInterval;

bool TestOutputRange()
{
	std::cout << "  Test 1: Output range [0,1]..." << std::endl;

	SimplexNoise3D* noise = new SimplexNoise3D( 0.5, 4 );

	bool passed = true;
	for( int i = -50; i < 50; i++ ) {
		for( int j = -10; j < 10; j++ ) {
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

	SimplexNoise3D* noise = new SimplexNoise3D( 0.5, 4 );

	Scalar minVal = 1e20, maxVal = -1e20;
	for( int i = 0; i < 100; i++ ) {
		Scalar val = noise->Evaluate( i * 0.7, i * 1.3, i * 0.5 );
		if( val < minVal ) minVal = val;
		if( val > maxVal ) maxVal = val;
	}

	bool passed = (maxVal - minVal) > 0.1;
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

	SimplexNoise3D* noise = new SimplexNoise3D( 0.65, 4 );

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

bool TestDifferentPersistence()
{
	std::cout << "  Test 4: Different persistence values differ..." << std::endl;

	SimplexNoise3D* noiseA = new SimplexNoise3D( 0.3, 4 );
	SimplexNoise3D* noiseB = new SimplexNoise3D( 0.9, 4 );

	const int differCount = CountDifferentSamples( 30,
		[&]( int i ) { return noiseA->Evaluate( i * 1.3, i * 0.9, i * 2.1 ); },
		[&]( int i ) { return noiseB->Evaluate( i * 1.3, i * 0.9, i * 2.1 ); },
		1e-4 );

	bool passed = differCount > 15;
	noiseA->release();
	noiseB->release();
	if( !passed )
		std::cout << "    FAIL: only " << differCount << "/30 differ" << std::endl;
	else
		std::cout << "    PASSED (" << differCount << "/30 differ)" << std::endl;
	return passed;
}

bool TestZeroPersistenceMatchesSingleOctave()
{
	std::cout << "  Test 5: Zero persistence matches single octave..." << std::endl;

	SimplexNoise3D* noiseZeroPersistence = new SimplexNoise3D( 0.0, 6 );
	SimplexNoise3D* noiseSingleOctave = new SimplexNoise3D( 0.5, 1 );

	bool passed = true;
	for( int i = 0; i < 25; i++ ) {
		Scalar x = i * 0.8 + 0.1;
		Scalar y = i * 1.1 + 0.2;
		Scalar z = i * 0.6 + 0.3;
		const Scalar a = noiseZeroPersistence->Evaluate( x, y, z );
		const Scalar b = noiseSingleOctave->Evaluate( x, y, z );
		if( !IsClose( a, b, 1e-6 ) ) {
			std::cout << "    FAIL: zeroPersistence=" << a << " singleOctave=" << b << std::endl;
			passed = false;
			break;
		}
	}

	noiseZeroPersistence->release();
	noiseSingleOctave->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestDifferentOctaves()
{
	std::cout << "  Test 6: Different octave counts differ..." << std::endl;

	SimplexNoise3D* noiseA = new SimplexNoise3D( 0.5, 1 );
	SimplexNoise3D* noiseB = new SimplexNoise3D( 0.5, 6 );

	const int differCount = CountDifferentSamples( 30,
		[&]( int i ) { return noiseA->Evaluate( i * 1.3, i * 0.9, i * 2.1 ); },
		[&]( int i ) { return noiseB->Evaluate( i * 1.3, i * 0.9, i * 2.1 ); },
		1e-4 );

	bool passed = differCount > 15;
	noiseA->release();
	noiseB->release();
	if( !passed )
		std::cout << "    FAIL: only " << differCount << "/30 differ" << std::endl;
	else
		std::cout << "    PASSED (" << differCount << "/30 differ)" << std::endl;
	return passed;
}

bool TestOriginIsNeutralMidpoint()
{
	std::cout << "  Test 7: Origin evaluates to neutral midpoint..." << std::endl;

	SimplexNoise3D* noise = new SimplexNoise3D( 0.65, 4 );

	const Scalar val = noise->Evaluate( 0.0, 0.0, 0.0 );
	bool passed = IsClose( val, 0.5, 1e-8 );

	noise->release();
	if( !passed )
		std::cout << "    FAIL: val=" << val << " expected=0.5" << std::endl;
	else
		std::cout << "    PASSED (val=" << val << ")" << std::endl;
	return passed;
}

bool TestNegativeCoordinates()
{
	std::cout << "  Test 8: Negative coordinates..." << std::endl;

	SimplexNoise3D* noise = new SimplexNoise3D( 0.65, 4 );

	Scalar val = noise->Evaluate( -10.5, -20.3, -5.7 );
	bool passed = IsInUnitInterval( val );
	noise->release();
	if( !passed )
		std::cout << "    FAIL: val=" << val << std::endl;
	else
		std::cout << "    PASSED (val=" << val << ")" << std::endl;
	return passed;
}

bool TestSymmetry()
{
	std::cout << "  Test 9: No axis-aligned symmetry..." << std::endl;

	SimplexNoise3D* noise = new SimplexNoise3D( 0.65, 4 );

	// Simplex noise should NOT be symmetric along axes
	const int differCount = CountDifferentSamples( 19,
		[&]( int i ) { const Scalar x = (i + 1) * 1.7; return noise->Evaluate( x, 0.5, 0.5 ); },
		[&]( int i ) { const Scalar x = (i + 1) * 1.7; return noise->Evaluate( 0.5, x, 0.5 ); },
		1e-4 );

	bool passed = differCount > 10;
	noise->release();
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
	allPassed &= TestZeroPersistenceMatchesSingleOctave();
	allPassed &= TestDifferentOctaves();
	allPassed &= TestOriginIsNeutralMidpoint();
	allPassed &= TestNegativeCoordinates();
	allPassed &= TestSymmetry();

	std::cout << std::endl;
	if( allPassed )
		std::cout << "ALL TESTS PASSED" << std::endl;
	else
		std::cout << "SOME TESTS FAILED" << std::endl;

	return allPassed ? 0 : 1;
}
