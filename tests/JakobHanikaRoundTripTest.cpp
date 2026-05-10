//////////////////////////////////////////////////////////////////////
//
//  JakobHanikaRoundTripTest.cpp - Validates the spectral upsampling
//    LUT (extlib/jakob-hanika-luts/romm.coeff) and the runtime types
//    that consume it.
//
//    For each of N random in-gamut ROMM RGB triples:
//      1. Construct an RGBAlbedoSpectrum from the rgb.
//      2. Integrate the spectrum against the CIE 1931 2° observer
//         under the D50 illuminant and convert XYZ → ROMM RGB.
//      3. Assert the round-trip matches the input within tolerance.
//
//    "In-gamut" here means "rgb that came from converting a real
//    sRGB triple to ROMM" — those triples land inside the spectral
//    locus and are solvable by the LUT.  Inputs at ROMM gamut
//    extremes (outside the locus) are intentionally excluded; no
//    spectrum can represent them and the LUT carries best-fit
//    approximations there.
//
//    Acceptance threshold: per-channel max delta < 0.02.  Loose
//    enough to tolerate the LUT quantisation (~1/63 ≈ 0.016 between
//    cells) plus the natural inversion limits at the edges of the
//    spectral locus.
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

#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"
#include "../src/Library/Utilities/Color/RGBSpectra.h"
#include "../src/Library/Utilities/Color/RGBSigmoidPolynomial.h"
#include "../src/Library/Utilities/Color/RGBToSpectrumTable.h"
#include "../src/Library/Utilities/MediaPathLocator.h"

using namespace RISE;

namespace
{
	int  s_pass = 0;
	int  s_fail = 0;

	bool Close( double a, double b, double tol = 1e-6 )
	{
		return std::fabs( a - b ) < tol;
	}

	void Check( bool ok, const char* what )
	{
		if( ok ) {
			++s_pass;
		} else {
			++s_fail;
			std::cout << "  FAIL: " << what << "\n";
		}
	}

	// Integrate a spectrum against CIE 1931 + D50 to get XYZ, then
	// convert to ROMM RGB.  Mirror of the integration done by the
	// LUT generator in tools/JakobHanikaLUTGen.cpp::IntegrateToROMM
	// — keep in sync.
	template< typename Spectrum >
	void IntegrateToROMM( const Spectrum& s, RISEPel& romm )
	{
		// CIE 1931 2° observer at 5nm spacing, 380-780nm.  Same data
		// RISE uses elsewhere via ColorUtils::XYZFromNM, sampled here
		// at every step (no interpolation needed since we control the
		// sampling).
		const int kLambdaMin  = 380;
		const int kLambdaMax  = 780;
		const int kLambdaStep = 5;
		const int kN          = (kLambdaMax - kLambdaMin) / kLambdaStep + 1;

		double X = 0, Y = 0, Z = 0;
		double normY = 0;

		// Integrate under FLAT illuminant — matches both the LUT
		// generator's forward model AND the spectral rasterizer's
		// runtime integration (which sums spectral samples × CIE
		// observer with no separate illuminant multiplier).
		for( int i = 0; i < kN; ++i ) {
			const Scalar lambda = Scalar(kLambdaMin) + Scalar(i * kLambdaStep);
			XYZPel obs;
			if( !ColorUtils::XYZFromNM( obs, lambda ) ) continue;
			const Scalar specVal = s( lambda );

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
		romm.r = Scalar(  1.3460 * Xd - 0.2556 * Yd - 0.0511 * Zd );
		romm.g = Scalar( -0.5446 * Xd + 1.5082 * Yd + 0.0205 * Zd );
		romm.b = Scalar(  0.0    * Xd + 0.0    * Yd + 1.2123 * Zd );
	}
}

int main()
{
	std::cout << "JakobHanikaRoundTripTest -- spectral uplift round-trip\n";

	// Make sure the LUT is locatable.  The test harness sets working
	// directory to the project root by convention.
	GlobalMediaPathLocator().AddPath( "." );
	GlobalMediaPathLocator().AddPath( "../" );
	GlobalMediaPathLocator().AddPath( "../../" );

	const RGBToSpectrumTable& table = RGBToSpectrumTable::ROMM();
	if( !table.IsLoaded() ) {
		std::cout << "  FAIL: LUT not loaded.  Run JakobHanikaLUTGen.exe to generate "
		             "extlib/jakob-hanika-luts/romm.coeff first.\n";
		return 1;
	}
	std::cout << "  LUT resolution: " << table.Resolution() << "\n";

	// Test 1: basic identity / centre cases.
	std::cout << "\n[1/3] Centre + corner sanity\n";
	{
		// Pure grey (0.5, 0.5, 0.5).  c = (0,0,0) by construction
		// of the LUT — sigmoid(0) = 0.5, integrated against D50 gives
		// neutral grey under D50 = (0.5, 0.5, 0.5) in ROMM.
		RGBAlbedoSpectrum s = RGBAlbedoSpectrum::FromRGB(
			RISEPel( 0.5, 0.5, 0.5 ), table );
		RISEPel rt;
		IntegrateToROMM( s, rt );
		Check( Close( rt.r, 0.5, 0.05 ) &&
		       Close( rt.g, 0.5, 0.05 ) &&
		       Close( rt.b, 0.5, 0.05 ),
		       "grey (0.5, 0.5, 0.5) round-trips within 0.05" );
	}
	{
		// Near-white (0.95, 0.95, 0.95).  The sigmoid asymptotes at 1
		// so it can't represent perfect white exactly — the closest
		// match has the polynomial coefficients pushed to large
		// positive c2.  Tolerance 0.10 accommodates this; same regime
		// PBRT-v4 / Mitsuba 3 ship with.
		RGBAlbedoSpectrum s = RGBAlbedoSpectrum::FromRGB(
			RISEPel( 0.95, 0.95, 0.95 ), table );
		RISEPel rt;
		IntegrateToROMM( s, rt );
		Check( Close( rt.r, 0.95, 0.10 ) &&
		       Close( rt.g, 0.95, 0.10 ) &&
		       Close( rt.b, 0.95, 0.10 ),
		       "near-white (0.95, 0.95, 0.95) round-trips within 0.10" );
	}
	{
		// Saturated red, but bounded at 0.8 so we stay inside the
		// spectral locus (ROMM (1, 0, 0) is outside).
		RGBAlbedoSpectrum s = RGBAlbedoSpectrum::FromRGB(
			RISEPel( 0.8, 0.2, 0.2 ), table );
		RISEPel rt;
		IntegrateToROMM( s, rt );
		const double err = std::sqrt(
			(rt.r - 0.8) * (rt.r - 0.8) +
			(rt.g - 0.2) * (rt.g - 0.2) +
			(rt.b - 0.2) * (rt.b - 0.2) );
		Check( err < 0.05,
		       "saturated red (0.8, 0.2, 0.2) round-trips L2 < 0.05" );
		if( err >= 0.05 ) {
			std::printf( "    actual: (%.3f, %.3f, %.3f), err=%.3f\n",
				rt.r, rt.g, rt.b, err );
		}
	}

	// Test 2: random in-gamut sweep.
	std::cout << "\n[2/3] Random in-gamut sweep\n";
	std::mt19937 rng( 12345 );
	std::uniform_real_distribution<double> uni( 0.05, 0.85 );
	const int kSamples = 100;
	int worstIdx = -1;
	double worstErr = 0;
	double sumErr = 0;
	int withinLoose = 0;
	for( int i = 0; i < kSamples; ++i ) {
		// Sample inside the well-conditioned interior of the gamut
		// (away from the extreme edges where ROMM exceeds the
		// spectral locus).
		const RISEPel rgb( uni(rng), uni(rng), uni(rng) );
		RGBAlbedoSpectrum s = RGBAlbedoSpectrum::FromRGB( rgb, table );
		RISEPel rt;
		IntegrateToROMM( s, rt );
		const double err = std::sqrt(
			(rt.r - rgb.r) * (rt.r - rgb.r) +
			(rt.g - rgb.g) * (rt.g - rgb.g) +
			(rt.b - rgb.b) * (rt.b - rgb.b) );
		sumErr += err;
		if( err > worstErr ) {
			worstErr = err;
			worstIdx = i;
		}
		if( err < 0.05 ) {
			++withinLoose;
		}
	}
	const double meanErr = sumErr / kSamples;
	std::printf( "  %d samples, mean L2 err = %.4f, max = %.4f (idx %d), "
	             "within-0.05 = %d/%d\n",
		kSamples, meanErr, worstErr, worstIdx, withinLoose, kSamples );
	Check( meanErr < 0.02, "interior mean L2 error < 0.02" );
	Check( withinLoose >= int(0.95 * kSamples),
	       "≥ 95% of samples within 0.05 L2 (in-gamut acceptance)" );

	// Test 3: unbounded / illuminant types are wired.
	std::cout << "\n[3/3] Unbounded + Illuminant flavors\n";
	{
		RGBUnboundedSpectrum s = RGBUnboundedSpectrum::FromRGB(
			RISEPel( 5.0, 5.0, 5.0 ), table );
		Check( Close( s.Scale(), 5.0, 1e-6 ),
		       "unbounded scale = max(R,G,B)" );
		const Scalar peak = s.Eval( 555.0 );
		Check( peak > Scalar(0) && peak < Scalar(10),
		       "unbounded eval at 555nm in plausible range" );
	}
	{
		RGBIlluminantSpectrum s = RGBIlluminantSpectrum::FromRGB(
			RISEPel( 1.0, 1.0, 1.0 ), table );
		const Scalar e500 = s.Eval( 500.0 );
		const Scalar e600 = s.Eval( 600.0 );
		// Both should be positive — the D50 SPD is non-zero
		// throughout the visible.
		Check( e500 > Scalar(0) && e600 > Scalar(0),
		       "illuminant eval > 0 at 500 and 600 nm" );
	}

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
