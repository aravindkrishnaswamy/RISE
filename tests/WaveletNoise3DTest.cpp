//////////////////////////////////////////////////////////////////////
//
//  WaveletNoise3DTest.cpp - Unit tests for WaveletNoise3D
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

#include "../src/Library/Noise/WaveletNoise.h"
#include "ProceduralGridTestHelpers.h"
#include "ProceduralTestHelpers.h"

using namespace RISE;
using namespace RISE::Implementation;
using ProceduralGridTestHelpers::AllCloseOnGrid3D;
using ProceduralTestHelpers::IsClose;
using ProceduralTestHelpers::IsInUnitInterval;

bool TestOutputRange()
{
	std::cout << "  Test 1: Output range [0,1]..." << std::endl;
	WaveletNoise3D* noise = new WaveletNoise3D( 32, 0.5, 4 );
	bool passed = true;
	for( int i = -30; i < 30; i++ ) {
		for( int j = -5; j < 5; j++ ) {
			Scalar val = noise->Evaluate( i * 0.37, j * 0.41, (i+j) * 0.29 );
			if( !IsInUnitInterval( val ) ) {
				std::cout << "    FAIL: out of range " << val << std::endl;
				passed = false; break;
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
	WaveletNoise3D* noise = new WaveletNoise3D( 32, 0.5, 4 );
	Scalar minVal = 1e20, maxVal = -1e20;
	for( int i = 0; i < 100; i++ ) {
		Scalar val = noise->Evaluate( i * 0.7, i * 1.3, i * 0.5 );
		if( val < minVal ) minVal = val;
		if( val > maxVal ) maxVal = val;
	}
	bool passed = (maxVal - minVal) > 0.05;
	noise->release();
	if( !passed ) std::cout << "    FAIL: range=" << (maxVal-minVal) << std::endl;
	else std::cout << "    PASSED (range=" << (maxVal-minVal) << ")" << std::endl;
	return passed;
}

bool TestDeterministic()
{
	std::cout << "  Test 3: Deterministic..." << std::endl;
	WaveletNoise3D* noise = new WaveletNoise3D( 32, 0.65, 4 );
	bool passed = true;
	for( int i = 0; i < 20; i++ ) {
		Scalar x = i * 3.7, y = i * 2.1, z = i * 1.3;
		if( !IsClose( noise->Evaluate(x,y,z), noise->Evaluate(x,y,z) ) ) {
			passed = false; break;
		}
	}
	noise->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	else std::cout << "    FAIL" << std::endl;
	return passed;
}

bool TestTiling()
{
	std::cout << "  Test 4: Tiling periodicity..." << std::endl;
	WaveletNoise3D* noise = new WaveletNoise3D( 32, 0.65, 1 );  // 1 octave to test raw tile
	const bool passed = AllCloseOnGrid3D( 5, 2, 1, 2.7, 1.3, 0.9,
		[&]( Scalar x, Scalar y, Scalar z ) { return noise->Evaluate( x + 0.5, y + 0.3, z + 0.7 ); },
		[&]( Scalar x, Scalar y, Scalar z ) { return noise->Evaluate( x + 32.5, y + 32.3, z + 32.7 ); },
		1e-4 );
	noise->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	else std::cout << "    FAIL: not periodic at shift 32" << std::endl;
	return passed;
}

bool TestAxisWrappedPeriodicity()
{
	std::cout << "  Test 5: Per-axis wrapped periodicity..." << std::endl;
	WaveletNoise3D* noise = new WaveletNoise3D( 32, 0.65, 4 );

	const bool passedX = AllCloseOnGrid3D( 4, 3, 1, 1.9, 0.8, 1.1,
		[&]( Scalar x, Scalar y, Scalar z ) { return noise->Evaluate( x + 0.35, y + 0.15, z + 0.65 ); },
		[&]( Scalar x, Scalar y, Scalar z ) { return noise->Evaluate( x + 32.35, y + 0.15, z + 0.65 ); },
		1e-4 );
	const bool passedY = AllCloseOnGrid3D( 4, 3, 1, 1.9, 0.8, 1.1,
		[&]( Scalar x, Scalar y, Scalar z ) { return noise->Evaluate( x + 0.35, y + 0.15, z + 0.65 ); },
		[&]( Scalar x, Scalar y, Scalar z ) { return noise->Evaluate( x + 0.35, y + 32.15, z + 0.65 ); },
		1e-4 );
	const bool passedZ = AllCloseOnGrid3D( 4, 3, 1, 1.9, 0.8, 1.1,
		[&]( Scalar x, Scalar y, Scalar z ) { return noise->Evaluate( x + 0.35, y + 0.15, z + 0.65 ); },
		[&]( Scalar x, Scalar y, Scalar z ) { return noise->Evaluate( x + 0.35, y + 0.15, z + 32.65 ); },
		1e-4 );
	const bool passed = passedX && passedY && passedZ;

	noise->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	else std::cout << "    FAIL: per-axis wrapped periodicity mismatch" << std::endl;
	return passed;
}

bool TestNegativeCoordinatesWrapConsistently()
{
	std::cout << "  Test 6: Negative coordinates wrap consistently..." << std::endl;
	WaveletNoise3D* noise = new WaveletNoise3D( 32, 0.65, 4 );

	const Scalar x = -10.5;
	const Scalar y = -20.3;
	const Scalar z = -5.7;
	const Scalar wrappedX = x + 32.0;
	const Scalar wrappedY = y + 32.0;
	const Scalar wrappedZ = z + 32.0;

	const Scalar val = noise->Evaluate( x, y, z );
	const Scalar wrapped = noise->Evaluate( wrappedX, wrappedY, wrappedZ );
	bool passed = IsInUnitInterval( val ) && IsClose( val, wrapped, 1e-4 );

	noise->release();
	if( passed ) {
		std::cout << "    PASSED (val=" << val << ", wrapped=" << wrapped << ")" << std::endl;
	} else {
		std::cout << "    FAIL: val=" << val << " wrapped=" << wrapped << std::endl;
	}
	return passed;
}

int main()
{
	std::cout << "=== WaveletNoise3D Tests ===" << std::endl;
	bool allPassed = true;
	allPassed &= TestOutputRange();
	allPassed &= TestSpatialVariation();
	allPassed &= TestDeterministic();
	allPassed &= TestTiling();
	allPassed &= TestAxisWrappedPeriodicity();
	allPassed &= TestNegativeCoordinatesWrapConsistently();
	std::cout << std::endl;
	if( allPassed ) std::cout << "ALL TESTS PASSED" << std::endl;
	else std::cout << "SOME TESTS FAILED" << std::endl;
	return allPassed ? 0 : 1;
}
