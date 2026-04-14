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

using namespace RISE;
using namespace RISE::Implementation;

static bool IsClose( double a, double b, double tol = 1e-6 )
{
	return fabs( a - b ) < tol;
}

bool TestOutputRange()
{
	std::cout << "  Test 1: Output range [0,1]..." << std::endl;
	// Small grid, few iterations for speed
	ReactionDiffusion3D rd( 16, 0.2, 0.1, 0.037, 0.06, 500 );
	bool passed = true;
	for( int i = 0; i < 20; i++ ) {
		for( int j = 0; j < 5; j++ ) {
			Scalar val = rd.Evaluate( i * 0.05, j * 0.05, 0.5 );
			if( val < -1e-10 || val > 1.0 + 1e-6 ) {
				std::cout << "    FAIL: out of range " << val << std::endl;
				passed = false; break;
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
	ReactionDiffusion3D rd( 16, 0.2, 0.1, 0.037, 0.06, 500 );
	Scalar minVal = 1e20, maxVal = -1e20;
	for( int i = 0; i < 50; i++ ) {
		Scalar val = rd.Evaluate( i * 0.02, i * 0.03, i * 0.01 );
		if( val < minVal ) minVal = val;
		if( val > maxVal ) maxVal = val;
	}
	bool passed = (maxVal - minVal) > 0.01;
	if( !passed ) std::cout << "    FAIL: range=" << (maxVal-minVal) << std::endl;
	else std::cout << "    PASSED (range=" << (maxVal-minVal) << ")" << std::endl;
	return passed;
}

bool TestDeterministic()
{
	std::cout << "  Test 3: Deterministic..." << std::endl;
	ReactionDiffusion3D rd( 16, 0.2, 0.1, 0.037, 0.06, 500 );
	bool passed = true;
	for( int i = 0; i < 10; i++ ) {
		Scalar x = i * 0.1, y = i * 0.07, z = i * 0.05;
		if( !IsClose( rd.Evaluate(x,y,z), rd.Evaluate(x,y,z) ) ) {
			passed = false; break;
		}
	}
	if( passed ) std::cout << "    PASSED" << std::endl;
	else std::cout << "    FAIL" << std::endl;
	return passed;
}

bool TestDifferentParameters()
{
	std::cout << "  Test 4: Different parameters produce different patterns..." << std::endl;
	// Spots pattern (f=0.037, k=0.06) vs stripes pattern (f=0.04, k=0.06)
	ReactionDiffusion3D rdA( 16, 0.2, 0.1, 0.037, 0.06, 500 );
	ReactionDiffusion3D rdB( 16, 0.2, 0.1, 0.04, 0.065, 500 );
	int differCount = 0;
	for( int i = 0; i < 20; i++ ) {
		Scalar x = i * 0.05, y = 0.5, z = 0.5;
		if( !IsClose( rdA.Evaluate(x,y,z), rdB.Evaluate(x,y,z), 1e-4 ) )
			differCount++;
	}
	bool passed = differCount > 5;
	if( !passed ) std::cout << "    FAIL: only " << differCount << "/20 differ" << std::endl;
	else std::cout << "    PASSED (" << differCount << "/20 differ)" << std::endl;
	return passed;
}

bool TestNegativeCoordinates()
{
	std::cout << "  Test 5: Negative coordinates (wrapping)..." << std::endl;
	ReactionDiffusion3D rd( 16, 0.2, 0.1, 0.037, 0.06, 500 );
	Scalar val = rd.Evaluate( -0.3, -0.5, -0.1 );
	bool passed = val >= -1e-10 && val <= 1.0 + 1e-6;
	if( passed ) std::cout << "    PASSED (val=" << val << ")" << std::endl;
	else std::cout << "    FAIL: val=" << val << std::endl;
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
	std::cout << std::endl;
	if( allPassed ) std::cout << "ALL TESTS PASSED" << std::endl;
	else std::cout << "SOME TESTS FAILED" << std::endl;
	return allPassed ? 0 : 1;
}
