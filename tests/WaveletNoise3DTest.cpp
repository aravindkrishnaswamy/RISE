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

using namespace RISE;
using namespace RISE::Implementation;

static bool IsClose( double a, double b, double tol = 1e-6 )
{
	return fabs( a - b ) < tol;
}

bool TestOutputRange()
{
	std::cout << "  Test 1: Output range [0,1]..." << std::endl;
	WaveletNoise3D noise( 32, 0.5, 4 );
	bool passed = true;
	for( int i = -30; i < 30; i++ ) {
		for( int j = -5; j < 5; j++ ) {
			Scalar val = noise.Evaluate( i * 0.37, j * 0.41, (i+j) * 0.29 );
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
	WaveletNoise3D noise( 32, 0.5, 4 );
	Scalar minVal = 1e20, maxVal = -1e20;
	for( int i = 0; i < 100; i++ ) {
		Scalar val = noise.Evaluate( i * 0.7, i * 1.3, i * 0.5 );
		if( val < minVal ) minVal = val;
		if( val > maxVal ) maxVal = val;
	}
	bool passed = (maxVal - minVal) > 0.05;
	if( !passed ) std::cout << "    FAIL: range=" << (maxVal-minVal) << std::endl;
	else std::cout << "    PASSED (range=" << (maxVal-minVal) << ")" << std::endl;
	return passed;
}

bool TestDeterministic()
{
	std::cout << "  Test 3: Deterministic..." << std::endl;
	WaveletNoise3D noise( 32, 0.65, 4 );
	bool passed = true;
	for( int i = 0; i < 20; i++ ) {
		Scalar x = i * 3.7, y = i * 2.1, z = i * 1.3;
		if( !IsClose( noise.Evaluate(x,y,z), noise.Evaluate(x,y,z) ) ) {
			passed = false; break;
		}
	}
	if( passed ) std::cout << "    PASSED" << std::endl;
	else std::cout << "    FAIL" << std::endl;
	return passed;
}

bool TestTiling()
{
	std::cout << "  Test 4: Tiling periodicity..." << std::endl;
	WaveletNoise3D noise( 32, 0.65, 1 );  // 1 octave to test raw tile
	bool passed = true;
	for( int i = 0; i < 10; i++ ) {
		Scalar x = i * 2.7 + 0.5;
		Scalar y = i * 1.3 + 0.3;
		Scalar z = i * 0.9 + 0.7;
		Scalar v1 = noise.Evaluate( x, y, z );
		Scalar v2 = noise.Evaluate( x + 32, y + 32, z + 32 );
		if( !IsClose( v1, v2, 1e-4 ) ) {
			std::cout << "    FAIL: not periodic at shift 32" << std::endl;
			passed = false; break;
		}
	}
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestNegativeCoordinates()
{
	std::cout << "  Test 5: Negative coordinates..." << std::endl;
	WaveletNoise3D noise( 32, 0.65, 4 );
	Scalar val = noise.Evaluate( -10.5, -20.3, -5.7 );
	bool passed = val >= -1e-10 && val <= 1.0 + 1e-6;
	if( passed ) std::cout << "    PASSED (val=" << val << ")" << std::endl;
	else std::cout << "    FAIL: val=" << val << std::endl;
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
	allPassed &= TestNegativeCoordinates();
	std::cout << std::endl;
	if( allPassed ) std::cout << "ALL TESTS PASSED" << std::endl;
	else std::cout << "SOME TESTS FAILED" << std::endl;
	return allPassed ? 0 : 1;
}
