//////////////////////////////////////////////////////////////////////
//
//  RGBPainterSpectralRoundTripTest.cpp - Verifies that
//    UniformColorPainter (and other RGB-source leaf painters) now
//    return physically-meaningful spectral responses via
//    GetColorNM, instead of the previous luminance-proxy
//    `MaxValue(C)` (which made spectral integrators silently sample
//    materials at one scalar regardless of wavelength).
//
//    The test:
//      For each of N random in-gamut ROMM RGB triples:
//        1. Construct a UniformColorPainter with that RGB.
//        2. Sample GetColorNM at every 5nm bin from 380-780nm.
//        3. Integrate the sampled spectrum against CIE 1931 + D50.
//        4. Convert XYZ → ROMM RGB.
//        5. Assert the round-trip matches the original RGB within
//           tolerance.
//
//    This is L3.B's acceptance test.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <iostream>
#include <random>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"
#include "../src/Library/Utilities/MediaPathLocator.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IPainter.h"

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

	// Integrate painter.GetColorNM samples against CIE under a flat
	// illuminant (matching the LUT generator's forward model and the
	// spectral rasterizer's runtime integration).
	RISEPel IntegratePainterToROMM( const IPainter& painter, const RayIntersectionGeometric& ri )
	{
		double X = 0, Y = 0, Z = 0, normY = 0;
		for( int i = 0; i < 81; ++i ) {
			const Scalar lambda = Scalar(380) + Scalar(i * 5);
			XYZPel obs;
			if( !ColorUtils::XYZFromNM( obs, lambda ) ) continue;
			const Scalar specVal = painter.GetColorNM( ri, lambda );
			X += double(specVal) * obs.X;
			Y += double(specVal) * obs.Y;
			Z += double(specVal) * obs.Z;
			normY += obs.Y;
		}
		const double inv = 1.0 / normY;
		X *= inv; Y *= inv; Z *= inv;

		// Match the LUT generator's forward model exactly: Bradford
		// D65 → D50 chromatic adaptation followed by the D50→ROMM
		// matrix.  Both matrices inlined from
		// src/Library/Utilities/Color/Color.cpp (mxXYZD65toXYZD50 /
		// mxXYZD50toROMM) — keep in sync if those are retuned.
		const double Xd =  1.0479 * X + 0.0229 * Y - 0.0502 * Z;
		const double Yd =  0.0296 * X + 0.9904 * Y - 0.0171 * Z;
		const double Zd = -0.0092 * X + 0.0151 * Y + 0.7519 * Z;
		RISEPel out;
		out.r = Scalar(  1.3460 * Xd - 0.2556 * Yd - 0.0511 * Zd );
		out.g = Scalar( -0.5446 * Xd + 1.5082 * Yd + 0.0205 * Zd );
		out.b = Scalar(  0.0    * Xd + 0.0    * Yd + 1.2123 * Zd );
		return out;
	}

	RayIntersectionGeometric MakeDummyRi()
	{
		const Ray r( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		const RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( r, rs );
		ri.bHit = true;
		ri.ptCoord = Point2( 0, 0 );
		return ri;
	}
}

int main()
{
	std::cout << "RGBPainterSpectralRoundTripTest -- UniformColorPainter spectral path\n";

	GlobalMediaPathLocator().AddPath( "." );
	GlobalMediaPathLocator().AddPath( "../" );
	GlobalMediaPathLocator().AddPath( "../../" );

	const RayIntersectionGeometric ri = MakeDummyRi();

	// Test 1: GetColor still returns the exact RGB (no regression on
	// the non-spectral path).
	std::cout << "\n[1/3] GetColor unchanged on RGB path\n";
	{
		IPainter* p = nullptr;
		RISE_API_CreateUniformColorPainter( &p, RISEPel( 0.7, 0.4, 0.2 ) );
		const RISEPel c = p->GetColor( ri );
		Check( std::fabs( c.r - 0.7 ) < 1e-9 &&
		       std::fabs( c.g - 0.4 ) < 1e-9 &&
		       std::fabs( c.b - 0.2 ) < 1e-9,
		       "GetColor returns exact input RGB" );
		p->release();
	}

	// Test 2: GetColorNM returns wavelength-varying values (not a
	// single luminance constant).
	std::cout << "\n[2/3] GetColorNM is wavelength-varying\n";
	{
		IPainter* p = nullptr;
		RISE_API_CreateUniformColorPainter( &p, RISEPel( 0.8, 0.2, 0.2 ) );
		const Scalar v500 = p->GetColorNM( ri, Scalar(500) );	// short — should be low for red
		const Scalar v650 = p->GetColorNM( ri, Scalar(650) );	// long — should be high for red
		Check( v650 > v500,
		       "red painter's spectrum is higher at long λ than short λ" );
		std::printf( "    red painter: S(500)=%.3f, S(650)=%.3f\n",
			double(v500), double(v650) );
		p->release();
	}

	// Test 3: round-trip in-gamut sweep — the headline spectral
	// correctness test.
	std::cout << "\n[3/3] Random in-gamut round-trip\n";
	std::mt19937 rng( 54321 );
	std::uniform_real_distribution<double> uni( 0.05, 0.85 );
	const int kSamples = 50;
	double sumErr = 0, maxErr = 0;
	int withinTol = 0;
	for( int i = 0; i < kSamples; ++i ) {
		const RISEPel rgb( uni(rng), uni(rng), uni(rng) );
		IPainter* p = nullptr;
		RISE_API_CreateUniformColorPainter( &p, rgb );
		const RISEPel rt = IntegratePainterToROMM( *p, ri );
		p->release();

		const double err = std::sqrt(
			(rt.r - rgb.r) * (rt.r - rgb.r) +
			(rt.g - rgb.g) * (rt.g - rgb.g) +
			(rt.b - rgb.b) * (rt.b - rgb.b) );
		sumErr += err;
		maxErr  = std::max( maxErr, err );
		if( err < 0.05 ) ++withinTol;
	}
	const double meanErr = sumErr / kSamples;
	std::printf( "  %d painters, mean L2 err = %.4f, max = %.4f, within-0.05 = %d/%d\n",
		kSamples, meanErr, maxErr, withinTol, kSamples );
	Check( meanErr < 0.02,
	       "interior mean L2 error < 0.02" );
	Check( withinTol >= int(0.95 * kSamples),
	       "≥ 95% of painter spectra round-trip within 0.05 L2" );

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
