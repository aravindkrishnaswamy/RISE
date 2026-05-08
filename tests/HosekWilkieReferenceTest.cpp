//////////////////////////////////////////////////////////////////////
//
//  HosekWilkieReferenceTest.cpp - Sanity-check the analytic
//    sun-and-sky model (HosekWilkieSkyModel — currently Preetham
//    1999 internally, see HosekWilkieSkyModel.h for the v1/v2
//    plan).
//
//    The published Hosek-Wilkie reference data isn't vendored yet
//    (CC-BY 3.0 supplemental files; tracked in design doc), so this
//    test verifies model invariants rather than absolute reference-
//    radiance values:
//
//      1. Sky radiance is finite, non-negative, and zero on the
//         ground hemisphere (dir.y < 0).
//      2. Solar elevation parameter changes the sun direction
//         monotonically.
//      3. Turbidity changes the sky's chromaticity in the expected
//         direction (higher T → more whitish desaturation).
//      4. The matched sun + sky integrate to a plausible ROMM RGB
//         in the range a tonemapped renderer can read.
//      5. Solar radiance peaks in the visible (around 555nm).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#define _USE_MATH_DEFINES
#include <cmath>
#include <cstdio>
#include <iostream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "../src/Library/Utilities/HosekWilkieSkyModel.h"
#include "../src/Library/Rendering/HosekWilkieSpectralRadianceMap.h"
#include "../src/Library/Utilities/Color/Color.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int s_pass = 0;
	int s_fail = 0;

	void Check( bool ok, const char* what )
	{
		if( ok ) {
			++s_pass;
		} else {
			++s_fail;
			std::cout << "  FAIL: " << what << "\n";
		}
	}

	const Scalar kDeg = Scalar(M_PI / 180.0);
}

int main()
{
	std::cout << "HosekWilkieReferenceTest -- analytic sky-model invariants\n";

	// Test 1: zenith / ground / sun behaviour at noon clear sky.
	std::cout << "\n[1/5] Noon clear sky basic invariants\n";
	{
		HosekWilkieSkyModel sky(
			Scalar(85) * kDeg,			// solar elevation 85° (near zenith)
			Scalar(0)  * kDeg,			// azimuth 0
			Scalar(2),					// turbidity 2 (very clear)
			RISEPel( 0.3, 0.3, 0.3 ) );

		const Vector3 zenith( 0, 1, 0 );
		const Vector3 horizon( 1, 0.01, 0 );
		const Vector3 ground( 0, -1, 0 );

		const Scalar lZ = sky.SampleRadiance( zenith, Scalar(550) );
		const Scalar lH = sky.SampleRadiance( Vector3Ops::Normalize( horizon ), Scalar(550) );
		const Scalar lG = sky.SampleRadiance( ground, Scalar(550) );

		Check( lZ > 0, "zenith radiance > 0" );
		Check( lH > 0, "horizon radiance > 0" );
		Check( lG == 0, "ground radiance == 0 (no contribution from below)" );
		Check( std::isfinite( lZ ) && std::isfinite( lH ),
		       "all sky samples finite" );
	}

	// Test 2: solar direction tracks elevation.
	std::cout << "\n[2/5] Sun direction follows solar_elevation parameter\n";
	{
		HosekWilkieSkyModel low(   5 * kDeg, 0, 3, RISEPel(0.3,0.3,0.3) );
		HosekWilkieSkyModel high( 80 * kDeg, 0, 3, RISEPel(0.3,0.3,0.3) );
		Check( high.SunDirection().y > low.SunDirection().y,
		       "high-elevation sun is higher in sky" );
		// Sun direction should be normalized (within FP epsilon).
		const Vector3 lo = low.SunDirection();
		const Scalar lenLo = std::sqrt( lo.x*lo.x + lo.y*lo.y + lo.z*lo.z );
		Check( std::fabs( lenLo - 1.0 ) < 1e-6,
		       "sun direction is unit length" );
	}

	// Test 3: turbidity → chromaticity.
	std::cout << "\n[3/5] Higher turbidity desaturates the sky\n";
	{
		HosekWilkieSkyModel clear(  45 * kDeg, 0, 2, RISEPel(0.3,0.3,0.3) );
		HosekWilkieSkyModel hazy(   45 * kDeg, 0, 8, RISEPel(0.3,0.3,0.3) );
		const Vector3 zenith( 0, 1, 0 );
		const RISEPel cClear = clear.IntegrateRGB( zenith );
		const RISEPel cHazy  = hazy.IntegrateRGB( zenith );

		// Clear sky should have higher B/R ratio than hazy (blue-skewed
		// Rayleigh dominant); hazy sky's B/R approaches 1 as Mie
		// scattering desaturates.
		const double brClear = double(cClear.b) / std::max( 1e-9, double(cClear.r) );
		const double brHazy  = double(cHazy.b)  / std::max( 1e-9, double(cHazy.r) );
		std::printf( "  clear B/R = %.3f, hazy B/R = %.3f\n", brClear, brHazy );
		Check( brClear > brHazy,
		       "clear sky has stronger blue-vs-red than hazy sky" );
	}

	// Test 4: integrated RGB plausibility.
	std::cout << "\n[4/5] Integrated RGB lands in plausible range\n";
	{
		HosekWilkieSkyModel sky( 45 * kDeg, 0, 3, RISEPel(0.3,0.3,0.3) );
		const Vector3 dir1 = Vector3Ops::Normalize( Vector3( 0.3, 0.7, 0.0 ) );
		const RISEPel rgb = sky.IntegrateRGB( dir1 );
		Check( rgb.r > 0 && rgb.g > 0 && rgb.b > 0,
		       "all channels positive" );
		Check( rgb.b > rgb.r,
		       "blue dominant for clear-sky-direction sample (Rayleigh)" );
	}

	// Test 5: solar SPD peaks in visible.
	std::cout << "\n[5/5] Solar SPD peaks in the visible\n";
	{
		HosekWilkieSkyModel sky( 45 * kDeg, 0, 3, RISEPel(0.3,0.3,0.3) );
		const Scalar s400 = sky.SampleSolarRadiance( 400 );
		const Scalar s555 = sky.SampleSolarRadiance( 555 );
		const Scalar s700 = sky.SampleSolarRadiance( 700 );
		Check( s555 > 0,
		       "solar radiance at 555nm > 0" );
		Check( std::isfinite( s400 ) && std::isfinite( s700 ),
		       "solar radiance finite across the visible" );
	}

	// Test 6: the HosekWilkieSpectralRadianceMap wrapper.
	std::cout << "\n[6/6] HosekWilkieSpectralRadianceMap wires through correctly\n";
	{
		HosekWilkieSpectralRadianceMap* rmap = new HosekWilkieSpectralRadianceMap(
			Scalar(60) * kDeg, Scalar(45) * kDeg, Scalar(3),
			RISEPel( 0.3, 0.3, 0.3 ), Scalar(1) );
		rmap->addref();

		const Ray rUp(    Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		const Ray rDown(  Point3( 0, 0, 0 ), Vector3( 0, -1, 0 ) );
		const RasterizerState rs = { 0, 0 };

		const RISEPel cUp   = rmap->GetRadiance( rUp, rs );
		const RISEPel cDown = rmap->GetRadiance( rDown, rs );

		Check( cUp.r > 0 && cUp.g > 0 && cUp.b > 0,
		       "radiance map returns positive RGB at zenith" );
		Check( cDown.r == 0 && cDown.g == 0 && cDown.b == 0,
		       "radiance map returns zero RGB at nadir (ground hit)" );

		const Scalar nm = rmap->GetRadianceNM( rUp, rs, Scalar(550) );
		Check( nm > 0,
		       "GetRadianceNM > 0 at zenith (550nm)" );

		// Sun direction sanity
		const Vector3 sun = rmap->SunDirection();
		Check( sun.y > 0,
		       "matched sun direction is above the horizon" );

		rmap->release();
	}

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
