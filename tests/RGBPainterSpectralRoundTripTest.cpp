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
//      For each of N random in-gamut RISEPel (Rec.709 Linear) triples:
//        1. Construct a UniformColorPainter with that RGB.
//        2. Sample GetColorNM at every 5nm bin from 380-780nm.
//        3. Integrate the sampled REFLECTANCE against CIE 1931
//           WEIGHTED BY THE REFERENCE ILLUMINANT (D65), normalising
//           by ∫D65·ȳ dλ — the Stage C forward model
//           (docs/SPECTRAL_ILLUMINANT_CONVENTION.md).
//        4. Convert XYZ(D65) → Rec.709(D65).
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
#include "../src/Library/Utilities/Color/RGBSpectra.h"
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

	// Integrate painter.GetColorNM samples — a REFLECTANCE — against
	// CIE 1931 under the reference illuminant, matching the LUT
	// generator's rec709 forward model
	// (tools/JakobHanikaLUTGen.cpp::IntegrateToTarget):
	//
	//   rgb = M · ∫ S·D65·cmf dλ / ∫ D65·ȳ dλ
	//
	// Before Stage C (2026-09-02) this integrated under a FLAT
	// illuminant, matching the LUT's then-flat-E training.  That
	// convention could not represent white or a neutral grey (see
	// docs/SPECTRAL_ILLUMINANT_CONVENTION.md); the illuminant now lives
	// in the forward model on both ends.  D65 comes from the production
	// table via RGBIlluminantSpectrum::ReferenceIlluminant, whose own
	// Y-normalisation cancels out of the ratio.
	RISEPel IntegratePainterToRec709( const IPainter& painter, const RayIntersectionGeometric& ri )
	{
		double X = 0, Y = 0, Z = 0, normY = 0;
		for( int i = 0; i < 81; ++i ) {
			const Scalar lambda = Scalar(380) + Scalar(i * 5);
			XYZPel obs;
			if( !ColorUtils::XYZFromNM( obs, lambda ) ) continue;
			const double illum   = double( RGBIlluminantSpectrum::ReferenceIlluminant( lambda ) );
			const double specVal = double( painter.GetColorNM( ri, lambda ) ) * illum;
			X += specVal * obs.X;
			Y += specVal * obs.Y;
			Z += specVal * obs.Z;
			normY += illum * obs.Y;
		}
		const double inv = 1.0 / normY;
		X *= inv; Y *= inv; Z *= inv;

		// XYZ(D65) → Rec.709(D65) — matches the LUT generator's rec709
		// target forward model (no Bradford adapt; both spaces D65).
		// Matrix inlined from src/Library/Utilities/Color/Color.cpp
		// (mxXYZtoRec709).
		RISEPel out;
		out.r = Scalar(  3.240479 * X - 1.537150 * Y - 0.498535 * Z );
		out.g = Scalar( -0.969256 * X + 1.875992 * Y + 0.041556 * Z );
		out.b = Scalar(  0.055648 * X - 0.204043 * Y + 1.057311 * Z );
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
		const RISEPel rt = IntegratePainterToRec709( *p, ri );
		p->release();

		const double err = std::sqrt(
			(rt.r - rgb.r) * (rt.r - rgb.r) +
			(rt.g - rgb.g) * (rt.g - rgb.g) +
			(rt.b - rgb.b) * (rt.b - rgb.b) );
		sumErr += err;
		maxErr  = std::max( maxErr, err );
		if( err < 5e-3 ) ++withinTol;
	}
	const double meanErr = sumErr / kSamples;
	std::printf( "  %d painters, mean L2 err = %.5f, max = %.5f, within-5e-3 = %d/%d\n",
		kSamples, meanErr, maxErr, withinTol, kSamples );
	// Measured post-Stage-C on this seed: mean 1.1e-4, max 2.7e-4.
	// Pre-Stage-C thresholds were 0.02 / 0.05, sized for the flat-E
	// LUT's unconverged cells; with 100 % cell convergence the residual
	// is pure grid quantisation, so tighten ~10x.
	Check( meanErr < 2e-3,
	       "interior mean L2 error < 2e-3" );
	Check( withinTol >= int(0.95 * kSamples),
	       "≥ 95% of painter spectra round-trip within 5e-3 L2" );

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
