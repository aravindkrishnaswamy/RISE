//////////////////////////////////////////////////////////////////////
//
//  ReactionDiffusion3DTest.cpp - Unit tests for ReactionDiffusion3D
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

#include "../src/Library/Noise/ReactionDiffusion.h"
#include "ProceduralGridTestHelpers.h"
#include "ProceduralTestHelpers.h"

using namespace RISE;
using namespace RISE::Implementation;
using ProceduralGridTestHelpers::AllCloseOnGrid3D;
using ProceduralTestHelpers::CountDifferentSamples;
using ProceduralTestHelpers::IsClose;
using ProceduralTestHelpers::IsInUnitInterval;

bool TestOutputRange()
{
	std::cout << "  Test 1: Output range [0,1]..." << std::endl;
	// Small grid, few iterations for speed
	ReactionDiffusion3D* rd = new ReactionDiffusion3D( 16, 0.2, 0.1, 0.037, 0.06, 500 );
	bool passed = true;
	for( int i = 0; i < 20; i++ ) {
		for( int j = 0; j < 5; j++ ) {
			Scalar val = rd->Evaluate( i * 0.05, j * 0.05, 0.5 );
			if( !IsInUnitInterval( val ) ) {
				std::cout << "    FAIL: out of range " << val << std::endl;
				passed = false; break;
			}
		}
		if( !passed ) break;
	}
	rd->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestSpatialVariation()
{
	std::cout << "  Test 2: Spatial variation..." << std::endl;
	ReactionDiffusion3D* rd = new ReactionDiffusion3D( 16, 0.2, 0.1, 0.037, 0.06, 500 );
	Scalar minVal = 1e20, maxVal = -1e20;
	for( int i = 0; i < 50; i++ ) {
		Scalar val = rd->Evaluate( i * 0.02, i * 0.03, i * 0.01 );
		if( val < minVal ) minVal = val;
		if( val > maxVal ) maxVal = val;
	}
	bool passed = (maxVal - minVal) > 0.01;
	rd->release();
	if( !passed ) std::cout << "    FAIL: range=" << (maxVal-minVal) << std::endl;
	else std::cout << "    PASSED (range=" << (maxVal-minVal) << ")" << std::endl;
	return passed;
}

bool TestDeterministic()
{
	std::cout << "  Test 3: Deterministic..." << std::endl;
	ReactionDiffusion3D* rd = new ReactionDiffusion3D( 16, 0.2, 0.1, 0.037, 0.06, 500 );
	bool passed = true;
	for( int i = 0; i < 10; i++ ) {
		Scalar x = i * 0.1, y = i * 0.07, z = i * 0.05;
		if( !IsClose( rd->Evaluate(x,y,z), rd->Evaluate(x,y,z) ) ) {
			passed = false; break;
		}
	}
	rd->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	else std::cout << "    FAIL" << std::endl;
	return passed;
}

bool TestDifferentParameters()
{
	std::cout << "  Test 4: Different parameters produce different patterns..." << std::endl;
	// Spots pattern (f=0.037, k=0.06) vs stripes pattern (f=0.04, k=0.06)
	ReactionDiffusion3D* rdA = new ReactionDiffusion3D( 16, 0.2, 0.1, 0.037, 0.06, 500 );
	ReactionDiffusion3D* rdB = new ReactionDiffusion3D( 16, 0.2, 0.1, 0.04, 0.065, 500 );
	const int differCount = ProceduralGridTestHelpers::CountDifferentOnGrid3D( 5, 2, 2, 0.05, 0.2, 0.2,
		[&]( Scalar x, Scalar y, Scalar z ) { return rdA->Evaluate( x, y + 0.3, z + 0.3 ); },
		[&]( Scalar x, Scalar y, Scalar z ) { return rdB->Evaluate( x, y + 0.3, z + 0.3 ); },
		1e-4 );
	bool passed = differCount > 5;
	rdA->release();
	rdB->release();
	if( !passed ) std::cout << "    FAIL: only " << differCount << "/20 differ" << std::endl;
	else std::cout << "    PASSED (" << differCount << "/20 differ)" << std::endl;
	return passed;
}

bool TestNegativeCoordinates()
{
	std::cout << "  Test 5: Negative coordinates (wrapping)..." << std::endl;
	ReactionDiffusion3D* rd = new ReactionDiffusion3D( 16, 0.2, 0.1, 0.037, 0.06, 500 );

	const Scalar x = -0.3;
	const Scalar y = -0.5;
	const Scalar z = -0.1;
	const Scalar val = rd->Evaluate( x, y, z );
	const Scalar wrapped = rd->Evaluate( x + 1.0, y + 1.0, z + 1.0 );
	bool passed = IsInUnitInterval( val ) && IsClose( val, wrapped, 1e-6 );

	rd->release();
	if( passed ) std::cout << "    PASSED (val=" << val << ", wrapped=" << wrapped << ")" << std::endl;
	else std::cout << "    FAIL: val=" << val << " wrapped=" << wrapped << std::endl;
	return passed;
}

bool TestUnitPeriodicityPerAxis()
{
	std::cout << "  Test 6: Unit periodicity per axis..." << std::endl;
	ReactionDiffusion3D* rd = new ReactionDiffusion3D( 16, 0.2, 0.1, 0.037, 0.06, 500 );

	const bool passedX = AllCloseOnGrid3D( 4, 3, 3, 0.07, 0.05, 0.09,
		[&]( Scalar x, Scalar y, Scalar z ) { return rd->Evaluate( x + 0.03, y + 0.11, z + 0.19 ); },
		[&]( Scalar x, Scalar y, Scalar z ) { return rd->Evaluate( x + 1.03, y + 0.11, z + 0.19 ); },
		1e-6 );
	const bool passedY = AllCloseOnGrid3D( 4, 3, 3, 0.07, 0.05, 0.09,
		[&]( Scalar x, Scalar y, Scalar z ) { return rd->Evaluate( x + 0.03, y + 0.11, z + 0.19 ); },
		[&]( Scalar x, Scalar y, Scalar z ) { return rd->Evaluate( x + 0.03, y + 1.11, z + 0.19 ); },
		1e-6 );
	const bool passedZ = AllCloseOnGrid3D( 4, 3, 3, 0.07, 0.05, 0.09,
		[&]( Scalar x, Scalar y, Scalar z ) { return rd->Evaluate( x + 0.03, y + 0.11, z + 0.19 ); },
		[&]( Scalar x, Scalar y, Scalar z ) { return rd->Evaluate( x + 0.03, y + 0.11, z + 1.19 ); },
		1e-6 );
	const bool passed = passedX && passedY && passedZ;

	rd->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	else std::cout << "    FAIL: unit-period grid comparison mismatch" << std::endl;
	return passed;
}

int main()
{
	std::cout << "=== ReactionDiffusion3D Tests ===" << std::endl;
	bool allPassed = true;
	allPassed &= TestOutputRange();
	allPassed &= TestSpatialVariation();
	allPassed &= TestDeterministic();
	allPassed &= TestDifferentParameters();
	allPassed &= TestNegativeCoordinates();
	allPassed &= TestUnitPeriodicityPerAxis();
	std::cout << std::endl;
	if( allPassed ) std::cout << "ALL TESTS PASSED" << std::endl;
	else std::cout << "SOME TESTS FAILED" << std::endl;
	return allPassed ? 0 : 1;
}
