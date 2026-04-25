//////////////////////////////////////////////////////////////////////
//
//  TurbulenceNoise3DTest.cpp - Unit tests for TurbulenceNoise3D
//
//  Tests:
//    1. Output is non-negative (abs-value property)
//    2. Output varies spatially (not constant)
//    3. Deterministic: same input produces same output
//    4. Scale invariance: frequency doubles per octave
//    5. Single octave matches abs(interpolated noise)
//    6. Persistence affects output distribution
//    7. Zero and negative coordinates work correctly
//    8. Turbulence differs from Perlin at same coordinates
//    9. Output range is within [0,1] after normalization
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

#include "../src/Library/Noise/TurbulenceNoise.h"
#include "../src/Library/Noise/PerlinNoise.h"
#include "../src/Library/Utilities/SimpleInterpolators.h"
#include "ProceduralTestHelpers.h"
#include "StandaloneTestReporting.h"

using namespace RISE;
using namespace RISE::Implementation;
using ProceduralTestHelpers::CountDifferentSamples;
using ProceduralTestHelpers::IsClose;
using ProceduralTestHelpers::IsInUnitInterval;
using StandaloneTestReporting::PrintSection;

/// Test 1: Turbulence output is always non-negative
bool TestNonNegative()
{
	std::cout << "  Test 1: Non-negative output..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	TurbulenceNoise3D* noise = new TurbulenceNoise3D( *interp, 0.5, 6 );

	bool passed = true;
	for( int i = -50; i < 50; i++ ) {
		for( int j = -50; j < 50; j++ ) {
			Scalar val = noise->Evaluate( i * 0.37, j * 0.41, i * 0.13 + j * 0.29 );
			if( val < -1e-10 ) {
				std::cout << "    FAIL: negative value " << val
					<< " at (" << i*0.37 << "," << j*0.41 << "," << i*0.13+j*0.29 << ")" << std::endl;
				passed = false;
				break;
			}
		}
		if( !passed ) break;
	}

	noise->release();
	interp->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}

/// Test 2: Output varies spatially (not constant)
bool TestSpatialVariation()
{
	std::cout << "  Test 2: Spatial variation..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	TurbulenceNoise3D* noise = new TurbulenceNoise3D( *interp, 0.5, 4 );

	Scalar minVal = 1e20;
	Scalar maxVal = -1e20;

	for( int i = 0; i < 100; i++ ) {
		Scalar val = noise->Evaluate( i * 1.7, i * 2.3, i * 0.9 );
		if( val < minVal ) minVal = val;
		if( val > maxVal ) maxVal = val;
	}

	bool passed = (maxVal - minVal) > 0.01;
	noise->release();
	interp->release();
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

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	TurbulenceNoise3D* noise = new TurbulenceNoise3D( *interp, 0.65, 4 );

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
	interp->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}

/// Test 4: With 2 octaves (n=1 after the -1 convention), turbulence = |noise|/normFactor
/// which with persistence and single octave means turbulence = |noise(x*freq[0])| * amp[0] / amp[0] = |noise|
/// Similarly, Perlin with 2 octaves uses n=1, so perlin = noise(x*freq[0]) * amp[0]
/// Therefore turbulence = |perlin / amp[0]| = |perlin| (since amp[0]=1.0 for any persistence)
bool TestSingleOctaveMatchesAbsNoise()
{
	std::cout << "  Test 4: Single octave turbulence matches |perlin|..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	// numOctaves=2 means n=1 (one actual octave), matching the convention
	TurbulenceNoise3D* turbulence = new TurbulenceNoise3D( *interp, 0.5, 2 );
	PerlinNoise3D* perlin = new PerlinNoise3D( *interp, 0.5, 2 );

	bool passed = true;
	for( int i = 0; i < 30; i++ ) {
		Scalar x = i * 2.3 + 0.5;
		Scalar y = i * 1.7 - 3.2;
		Scalar z = i * 0.9 + 1.1;
		Scalar tVal = turbulence->Evaluate( x, y, z );
		Scalar pVal = fabs( perlin->Evaluate( x, y, z ) );
		if( !IsClose( tVal, pVal, 1e-4 ) ) {
			std::cout << "    FAIL: at (" << x << "," << y << "," << z
				<< ") turbulence=" << tVal << " |perlin|=" << pVal << std::endl;
			passed = false;
			break;
		}
	}

	turbulence->release();
	perlin->release();
	interp->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}

/// Test 5: Different persistence values produce different outputs
bool TestPersistenceEffect()
{
	std::cout << "  Test 5: Persistence effect..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	TurbulenceNoise3D* noiseLow = new TurbulenceNoise3D( *interp, 0.3, 6 );
	TurbulenceNoise3D* noiseHigh = new TurbulenceNoise3D( *interp, 0.9, 6 );

	const int differCount = CountDifferentSamples( 50,
		[&]( int i ) { return noiseLow->Evaluate( i * 1.3 + 0.7, i * 0.9 - 2.1, i * 2.1 + 0.3 ); },
		[&]( int i ) { return noiseHigh->Evaluate( i * 1.3 + 0.7, i * 0.9 - 2.1, i * 2.1 + 0.3 ); },
		1e-6 );

	// Different persistence values should produce different outputs
	bool passed = differCount > 25;
	noiseLow->release();
	noiseHigh->release();
	interp->release();
	if( !passed ) {
		std::cout << "    FAIL: only " << differCount << "/50 points differ between low and high persistence" << std::endl;
	} else {
		std::cout << "    PASSED (" << differCount << "/50 points differ)" << std::endl;
	}
	return passed;
}

/// Test 6: Zero persistence collapses to the single-octave result
bool TestZeroPersistenceMatchesSingleOctave()
{
	std::cout << "  Test 6: Zero persistence matches single octave..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	TurbulenceNoise3D* zeroPersistence = new TurbulenceNoise3D( *interp, 0.0, 6 );
	TurbulenceNoise3D* singleOctave = new TurbulenceNoise3D( *interp, 0.5, 2 );

	bool passed = true;
	for( int i = 0; i < 30; i++ ) {
		Scalar x = i * 1.2 + 0.1;
		Scalar y = i * 0.8 + 0.2;
		Scalar z = i * 1.5 + 0.3;
		const Scalar a = zeroPersistence->Evaluate( x, y, z );
		const Scalar b = singleOctave->Evaluate( x, y, z );
		if( !IsClose( a, b, 1e-6 ) ) {
			std::cout << "    FAIL: zeroPersistence=" << a << " singleOctave=" << b << std::endl;
			passed = false;
			break;
		}
	}

	zeroPersistence->release();
	singleOctave->release();
	interp->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}

/// Test 7: More octaves generally increases energy
bool TestOctaveEffect()
{
	std::cout << "  Test 7: Octave count effect..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	TurbulenceNoise3D* noise1 = new TurbulenceNoise3D( *interp, 0.5, 2 );   // n=1 (one octave)
	TurbulenceNoise3D* noise6 = new TurbulenceNoise3D( *interp, 0.5, 7 );   // n=6 (six octaves)

	// With normalization, more octaves doesn't necessarily mean more total energy
	// (since we divide by the sum of amplitudes). However, with more octaves the
	// noise is more "filled in" -- we test that the two produce different outputs.
	const int diffCount = CountDifferentSamples( 50,
		[&]( int i ) { return noise1->Evaluate( i * 1.1 + 0.3, i * 0.7 - 1.5, i * 1.9 + 0.8 ); },
		[&]( int i ) { return noise6->Evaluate( i * 1.1 + 0.3, i * 0.7 - 1.5, i * 1.9 + 0.8 ); },
		1e-6 );

	// Most points should differ when octave counts differ
	bool passed = diffCount > 25;
	noise1->release();
	noise6->release();
	interp->release();
	if( !passed ) {
		std::cout << "    FAIL: only " << diffCount << "/50 points differ between 1 and 6 octaves" << std::endl;
	} else {
		std::cout << "    PASSED (" << diffCount << "/50 points differ)" << std::endl;
	}
	return passed;
}

/// Test 8: Negative coordinates work correctly
bool TestNegativeCoordinates()
{
	std::cout << "  Test 8: Negative coordinates..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	TurbulenceNoise3D* noise = new TurbulenceNoise3D( *interp, 0.5, 4 );

	bool passed = true;
	Scalar val = noise->Evaluate( -10.5, -20.3, -5.7 );
	if( !IsInUnitInterval( val ) ) {
		std::cout << "    FAIL: negative value " << val << " at negative coords" << std::endl;
		passed = false;
	}

	// Also test that it varies at negative coords
	Scalar v1 = noise->Evaluate( -10.0, -20.0, -5.0 );
	Scalar v2 = noise->Evaluate( -15.0, -25.0, -10.0 );
	if( IsClose( v1, v2, 1e-10 ) ) {
		std::cout << "    WARN: identical values at different negative coords (unlikely but possible)" << std::endl;
	}

	noise->release();
	interp->release();
	if( passed )
		std::cout << "    PASSED (val=" << val << ")" << std::endl;
	return passed;
}

/// Test 9: Turbulence differs from Perlin at same coordinates
bool TestDiffersFromPerlin()
{
	std::cout << "  Test 9: Differs from Perlin noise..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	TurbulenceNoise3D* turbulence = new TurbulenceNoise3D( *interp, 0.5, 4 );
	PerlinNoise3D* perlin = new PerlinNoise3D( *interp, 0.5, 4 );

	const int differCount = CountDifferentSamples( 50,
		[&]( int i ) { return turbulence->Evaluate( i * 1.7 + 0.5, i * 2.3 - 1.0, i * 0.9 + 2.0 ); },
		[&]( int i ) { return perlin->Evaluate( i * 1.7 + 0.5, i * 2.3 - 1.0, i * 0.9 + 2.0 ); },
		1e-6 );

	// Most points should differ (turbulence takes abs of each octave)
	bool passed = differCount > 25;
	turbulence->release();
	perlin->release();
	interp->release();
	if( !passed ) {
		std::cout << "    FAIL: only " << differCount << "/50 points differ from Perlin" << std::endl;
	} else {
		std::cout << "    PASSED (" << differCount << "/50 points differ)" << std::endl;
	}
	return passed;
}

/// Test 10: Output range is within [0, 1] after normalization
bool TestOutputRange()
{
	std::cout << "  Test 10: Output range [0,1]..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	// Test with aggressive parameters that would exceed 1.0 without normalization
	TurbulenceNoise3D* noise = new TurbulenceNoise3D( *interp, 0.9, 8 );

	bool passed = true;
	Scalar maxVal = -1e20;
	Scalar minVal = 1e20;

	for( int i = -100; i < 100; i++ ) {
		for( int j = -10; j < 10; j++ ) {
			Scalar val = noise->Evaluate( i * 0.5, j * 0.7, (i+j) * 0.3 );
			if( val > maxVal ) maxVal = val;
			if( val < minVal ) minVal = val;
			if( !IsInUnitInterval( val ) ) {
				std::cout << "    FAIL: out of range value " << val << std::endl;
				passed = false;
				break;
			}
		}
		if( !passed ) break;
	}

	noise->release();
	interp->release();
	if( passed )
		std::cout << "    PASSED (range=[" << minVal << ", " << maxVal << "])" << std::endl;
	return passed;
}

int main()
{
	std::cout << "=== TurbulenceNoise3D Tests ===" << std::endl;
	bool allPassed = true;

	PrintSection( "Exact Contract Checks" );
	allPassed &= TestNonNegative();
	allPassed &= TestSingleOctaveMatchesAbsNoise();
	allPassed &= TestZeroPersistenceMatchesSingleOctave();
	allPassed &= TestOutputRange();
	allPassed &= TestDeterministic();
	allPassed &= TestNegativeCoordinates();

	PrintSection( "Sampled-Difference Heuristics" );
	allPassed &= TestSpatialVariation();
	allPassed &= TestPersistenceEffect();
	allPassed &= TestOctaveEffect();
	allPassed &= TestDiffersFromPerlin();

	std::cout << std::endl;
	if( allPassed )
		std::cout << "ALL TESTS PASSED" << std::endl;
	else
		std::cout << "SOME TESTS FAILED" << std::endl;

	return allPassed ? 0 : 1;
}
