//////////////////////////////////////////////////////////////////////
//
//  WorleyNoise3DTest.cpp - Unit tests for WorleyNoise3D
//
//  Tests:
//    1. Output is in [0, 1] range
//    2. Output varies spatially (not constant)
//    3. Deterministic: same input produces same output
//    4. F2-F1 is non-negative
//    5. Jitter=0 produces regular grid pattern
//    6. Different metrics produce different outputs
//    7. Different output modes produce different outputs
//    8. Negative coordinates work correctly
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

#include "../src/Library/Noise/WorleyNoise.h"
#include "ProceduralTestHelpers.h"

using namespace RISE;
using namespace RISE::Implementation;
using ProceduralTestHelpers::IsClose;
using ProceduralTestHelpers::IsInUnitInterval;

/// Test 1: Output is always in [0, 1]
bool TestOutputRange()
{
	std::cout << "  Test 1: Output range [0,1]..." << std::endl;

	WorleyNoise3D* noise = new WorleyNoise3D( 1.0, eWorley_Euclidean, eWorley_F1 );

	bool passed = true;
	for( int i = -50; i < 50; i++ ) {
		for( int j = -50; j < 50; j++ ) {
			Scalar val = noise->Evaluate( i * 0.37, j * 0.41, i * 0.13 + j * 0.29 );
			if( !IsInUnitInterval( val ) ) {
				std::cout << "    FAIL: out of range value " << val << std::endl;
				passed = false;
				break;
			}
		}
		if( !passed ) break;
	}

	noise->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}

/// Test 2: Output varies spatially
bool TestSpatialVariation()
{
	std::cout << "  Test 2: Spatial variation..." << std::endl;

	WorleyNoise3D* noise = new WorleyNoise3D( 1.0, eWorley_Euclidean, eWorley_F1 );

	Scalar minVal = 1e20;
	Scalar maxVal = -1e20;

	for( int i = 0; i < 100; i++ ) {
		Scalar val = noise->Evaluate( i * 0.3, i * 0.7, i * 0.5 );
		if( val < minVal ) minVal = val;
		if( val > maxVal ) maxVal = val;
	}

	bool passed = (maxVal - minVal) > 0.1;
	noise->release();
	if( !passed ) {
		std::cout << "    FAIL: range too small: min=" << minVal << " max=" << maxVal << std::endl;
	} else {
		std::cout << "    PASSED (range=" << (maxVal - minVal) << ")" << std::endl;
	}
	return passed;
}

/// Test 3: Deterministic output
bool TestDeterministic()
{
	std::cout << "  Test 3: Deterministic output..." << std::endl;

	WorleyNoise3D* noise = new WorleyNoise3D( 1.0, eWorley_Euclidean, eWorley_F1 );

	bool passed = true;
	for( int i = 0; i < 20; i++ ) {
		Scalar x = i * 3.7;
		Scalar y = i * 2.1;
		Scalar z = i * 1.3;
		Scalar v1 = noise->Evaluate( x, y, z );
		Scalar v2 = noise->Evaluate( x, y, z );
		if( !IsClose( v1, v2 ) ) {
			std::cout << "    FAIL: non-deterministic at (" << x << "," << y << "," << z
				<< ") v1=" << v1 << " v2=" << v2 << std::endl;
			passed = false;
			break;
		}
	}

	noise->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}

/// Test 4: F2-F1 output is non-negative
bool TestF2MinusF1NonNeg()
{
	std::cout << "  Test 4: F2-F1 non-negative..." << std::endl;

	WorleyNoise3D* noise = new WorleyNoise3D( 1.0, eWorley_Euclidean, eWorley_F2minusF1 );

	bool passed = true;
	for( int i = -30; i < 30; i++ ) {
		for( int j = -5; j < 5; j++ ) {
			Scalar val = noise->Evaluate( i * 0.5, j * 0.7, (i+j) * 0.3 );
			if( val < -1e-10 ) {
				std::cout << "    FAIL: negative F2-F1 value " << val << std::endl;
				passed = false;
				break;
			}
		}
		if( !passed ) break;
	}

	noise->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}

/// Test 5: Jitter=0 produces regular grid pattern (F1 at cell center = 0)
bool TestZeroJitter()
{
	std::cout << "  Test 5: Zero jitter regularity..." << std::endl;

	WorleyNoise3D* noise = new WorleyNoise3D( 0.0, eWorley_Euclidean, eWorley_F1 );

	// With zero jitter, all feature points are at cell centers (0.5, 0.5, 0.5)
	// So at integer+0.5 coordinates, F1 is exactly 0, and at cell corners
	// it is sqrt(3)/2 before normalization (Euclidean F1 uses normScale=1).
	Scalar atCenter = noise->Evaluate( 0.5, 0.5, 0.5 );
	Scalar atEdge = noise->Evaluate( 0.0, 0.0, 0.0 );
	const Scalar expectedEdge = sqrt( 3.0 ) * 0.5;

	bool passed = IsClose( atCenter, 0.0, 1e-8 ) &&
		IsClose( atEdge, expectedEdge, 1e-6 ) &&
		atCenter < atEdge;
	noise->release();
	if( !passed ) {
		std::cout << "    FAIL: center=" << atCenter
			<< " edge=" << atEdge
			<< " expectedEdge=" << expectedEdge << std::endl;
	} else {
		std::cout << "    PASSED (center=" << atCenter << " edge=" << atEdge << ")" << std::endl;
	}
	return passed;
}

/// Test 6: With zero jitter, some Euclidean mode values are analytically known
bool TestZeroJitterExactOutputModes()
{
	std::cout << "  Test 6: Zero jitter exact output modes..." << std::endl;

	WorleyNoise3D* noiseF2 = new WorleyNoise3D( 0.0, eWorley_Euclidean, eWorley_F2 );
	WorleyNoise3D* noiseF2F1 = new WorleyNoise3D( 0.0, eWorley_Euclidean, eWorley_F2minusF1 );

	// At a cell center, nearest feature point is distance 0 and the second
	// nearest is one cell away, so raw F2 = 1.0. Euclidean F2 normalizes by 1.5.
	const Scalar f2AtCenter = noiseF2->Evaluate( 0.5, 0.5, 0.5 );
	const Scalar expectedF2AtCenter = 1.0 / 1.5;

	// At a cell corner, eight neighboring cell centers are tied at the same
	// distance, so raw F1 == F2 and F2-F1 = 0 exactly.
	const Scalar f2MinusF1AtCorner = noiseF2F1->Evaluate( 0.0, 0.0, 0.0 );

	const bool passed =
		IsClose( f2AtCenter, expectedF2AtCenter, 1e-6 ) &&
		IsClose( f2MinusF1AtCorner, 0.0, 1e-8 );

	noiseF2->release();
	noiseF2F1->release();
	if( !passed ) {
		std::cout << "    FAIL: f2Center=" << f2AtCenter
			<< " expectedF2Center=" << expectedF2AtCenter
			<< " f2MinusF1Corner=" << f2MinusF1AtCorner << std::endl;
	} else {
		std::cout << "    PASSED" << std::endl;
	}
	return passed;
}

/// Test 7: Different metrics produce different outputs
bool TestDifferentMetrics()
{
	std::cout << "  Test 7: Different metrics produce different outputs..." << std::endl;

	WorleyNoise3D* noiseEuc = new WorleyNoise3D( 1.0, eWorley_Euclidean, eWorley_F1 );
	WorleyNoise3D* noiseMht = new WorleyNoise3D( 1.0, eWorley_Manhattan, eWorley_F1 );
	WorleyNoise3D* noiseChb = new WorleyNoise3D( 1.0, eWorley_Chebyshev, eWorley_F1 );

	int differCount = 0;
	for( int i = 0; i < 30; i++ ) {
		Scalar x = i * 1.7 + 0.3;
		Scalar y = i * 2.1 - 1.0;
		Scalar z = i * 0.9 + 2.0;
		Scalar vE = noiseEuc->Evaluate( x, y, z );
		Scalar vM = noiseMht->Evaluate( x, y, z );
		Scalar vC = noiseChb->Evaluate( x, y, z );
		if( !IsClose(vE, vM, 1e-4) || !IsClose(vE, vC, 1e-4) ) differCount++;
	}

	bool passed = differCount > 15;
	noiseEuc->release();
	noiseMht->release();
	noiseChb->release();
	if( !passed ) {
		std::cout << "    FAIL: only " << differCount << "/30 points differ" << std::endl;
	} else {
		std::cout << "    PASSED (" << differCount << "/30 points differ)" << std::endl;
	}
	return passed;
}

/// Test 8: Different output modes produce different outputs
bool TestDifferentOutputModes()
{
	std::cout << "  Test 8: Different output modes..." << std::endl;

	WorleyNoise3D* noiseF1 = new WorleyNoise3D( 1.0, eWorley_Euclidean, eWorley_F1 );
	WorleyNoise3D* noiseF2 = new WorleyNoise3D( 1.0, eWorley_Euclidean, eWorley_F2 );
	WorleyNoise3D* noiseF2F1 = new WorleyNoise3D( 1.0, eWorley_Euclidean, eWorley_F2minusF1 );

	int differCount = 0;
	for( int i = 0; i < 30; i++ ) {
		Scalar x = i * 1.3 + 0.5;
		Scalar y = i * 0.7 - 1.0;
		Scalar z = i * 1.9 + 0.5;
		Scalar v1 = noiseF1->Evaluate( x, y, z );
		Scalar v2 = noiseF2->Evaluate( x, y, z );
		Scalar v3 = noiseF2F1->Evaluate( x, y, z );
		if( !IsClose(v1, v2, 1e-4) || !IsClose(v1, v3, 1e-4) ) differCount++;
	}

	bool passed = differCount > 15;
	noiseF1->release();
	noiseF2->release();
	noiseF2F1->release();
	if( !passed ) {
		std::cout << "    FAIL: only " << differCount << "/30 points differ" << std::endl;
	} else {
		std::cout << "    PASSED (" << differCount << "/30 points differ)" << std::endl;
	}
	return passed;
}

/// Test 9: Negative coordinates work
bool TestNegativeCoordinates()
{
	std::cout << "  Test 9: Negative coordinates..." << std::endl;

	WorleyNoise3D* noise = new WorleyNoise3D( 1.0, eWorley_Euclidean, eWorley_F1 );

	bool passed = true;
	Scalar val = noise->Evaluate( -10.5, -20.3, -5.7 );
	if( !IsInUnitInterval( val ) ) {
		std::cout << "    FAIL: out of range at negative coords: " << val << std::endl;
		passed = false;
	}

	noise->release();
	if( passed )
		std::cout << "    PASSED (val=" << val << ")" << std::endl;
	return passed;
}

int main()
{
	std::cout << "=== WorleyNoise3D Tests ===" << std::endl;
	bool allPassed = true;

	allPassed &= TestOutputRange();
	allPassed &= TestSpatialVariation();
	allPassed &= TestDeterministic();
	allPassed &= TestF2MinusF1NonNeg();
	allPassed &= TestZeroJitter();
	allPassed &= TestZeroJitterExactOutputModes();
	allPassed &= TestDifferentMetrics();
	allPassed &= TestDifferentOutputModes();
	allPassed &= TestNegativeCoordinates();

	std::cout << std::endl;
	if( allPassed )
		std::cout << "ALL TESTS PASSED" << std::endl;
	else
		std::cout << "SOME TESTS FAILED" << std::endl;

	return allPassed ? 0 : 1;
}
