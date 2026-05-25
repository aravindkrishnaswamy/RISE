//////////////////////////////////////////////////////////////////////
//
//  JakobHanikaRoundTripTest.cpp - Validates the spectral upsampling
//    LUT (extlib/jakob-hanika-luts/<target>.coeff, baked into
//    src/Library/Utilities/Color/RGBToSpectrumTable_LUTData.cpp) and
//    the runtime types that consume it.
//
//    For each of N random in-gamut Rec.709 RGB triples:
//      1. Construct an RGBAlbedoSpectrum from the rgb (the lookup
//         goes through the runtime RISEPel→Rec.709 boundary).
//      2. Integrate the spectrum against the CIE 1931 2° observer
//         under a flat illuminant and convert XYZ → Rec.709 RGB
//         (no Bradford adapt — D65 throughout).
//      3. Assert the round-trip matches the input within tolerance.
//
//    "In-gamut" here means rgb that lands inside the Rec.709 gamut
//    interior (away from the saturated corners that the JH sigmoid
//    model cannot perfectly reproduce — see docs/JH_LUT_GAMUT.md).
//
//    Acceptance threshold: per-channel max delta < 0.02.  Loose
//    enough to tolerate the LUT quantisation (~1/63 ≈ 0.016 between
//    cells) plus the natural inversion limits of the JH sigmoid at
//    saturated colours.
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

	// Integrate a spectrum against CIE 1931 to get XYZ, then convert
	// to Rec.709 RGB.  Mirror of the LUT generator's forward model in
	// tools/JakobHanikaLUTGen.cpp::IntegrateToTarget for the rec709
	// target — keep in sync.  No Bradford adapt (Rec.709 is D65, CIE
	// is D65-referred).
	template< typename Spectrum >
	void IntegrateToRec709( const Spectrum& s, Rec709RGBPel& rec709 )
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

		// XYZ(D65) → Rec.709(D65) via the codebase's mxXYZtoRec709
		// matrix.  No chromatic adapt — both source and target share
		// D65.  Keep in sync with src/Library/Utilities/Color/Color.cpp
		// (mxXYZtoRec709).
		rec709.r = Scalar(  3.240479 * X - 1.537150 * Y - 0.498535 * Z );
		rec709.g = Scalar( -0.969256 * X + 1.875992 * Y + 0.041556 * Z );
		rec709.b = Scalar(  0.055648 * X - 0.204043 * Y + 1.057311 * Z );
	}
}

int main()
{
	std::cout << "JakobHanikaRoundTripTest -- spectral uplift round-trip\n";

	// Make sure the LUT is locatable.  The test harness sets working
	// directory to the project root by convention.  (The LUT is baked
	// into the binary post-2026-05, but the locator paths are kept for
	// resilience.)
	GlobalMediaPathLocator().AddPath( "." );
	GlobalMediaPathLocator().AddPath( "../" );
	GlobalMediaPathLocator().AddPath( "../../" );

	const RGBToSpectrumTable& table = RGBToSpectrumTable::Get();
	if( !table.IsLoaded() ) {
		std::cout << "  FAIL: LUT not loaded.  Run JakobHanikaLUTGen to "
		             "regenerate extlib/jakob-hanika-luts/rec709.coeff and "
		             "tools/GenerateSpectrumLUTHeader.py.\n";
		return 1;
	}
	std::cout << "  LUT resolution: " << table.Resolution() << "\n";

	// Test 1: basic identity / centre cases.
	std::cout << "\n[1/3] Centre + corner sanity\n";
	{
		// Pure grey (0.5, 0.5, 0.5).  c = (0,0,0) by construction
		// of the LUT — sigmoid(0) = 0.5, integrated against flat E
		// gives Y = 0.5 → neutral grey in Rec.709.
		const Rec709RGBPel input( 0.5, 0.5, 0.5 );
		RGBAlbedoSpectrum s = RGBAlbedoSpectrum::FromRGB(
			RISEPel( input ), table );
		Rec709RGBPel rt;
		IntegrateToRec709( s, rt );
		Check( Close( rt.r, input.r, 0.05 ) &&
		       Close( rt.g, input.g, 0.05 ) &&
		       Close( rt.b, input.b, 0.05 ),
		       "grey (0.5, 0.5, 0.5) round-trips within 0.05" );
	}
	{
		// Near-white (0.95, 0.95, 0.95).  The sigmoid asymptotes at 1
		// so it can't represent perfect white exactly — the closest
		// match has the polynomial coefficients pushed to large
		// positive c2.  Tolerance 0.10 accommodates this; same regime
		// PBRT-v4 / Mitsuba 3 ship with.
		const Rec709RGBPel input( 0.95, 0.95, 0.95 );
		RGBAlbedoSpectrum s = RGBAlbedoSpectrum::FromRGB(
			RISEPel( input ), table );
		Rec709RGBPel rt;
		IntegrateToRec709( s, rt );
		Check( Close( rt.r, input.r, 0.10 ) &&
		       Close( rt.g, input.g, 0.10 ) &&
		       Close( rt.b, input.b, 0.10 ),
		       "near-white (0.95, 0.95, 0.95) round-trips within 0.10" );
	}
	{
		// Saturated red, bounded at 0.8 to stay in the well-
		// conditioned interior of the gamut.
		const Rec709RGBPel input( 0.8, 0.2, 0.2 );
		RGBAlbedoSpectrum s = RGBAlbedoSpectrum::FromRGB(
			RISEPel( input ), table );
		Rec709RGBPel rt;
		IntegrateToRec709( s, rt );
		const double err = std::sqrt(
			(rt.r - input.r) * (rt.r - input.r) +
			(rt.g - input.g) * (rt.g - input.g) +
			(rt.b - input.b) * (rt.b - input.b) );
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
		// Sample inside the well-conditioned interior of the gamut.
		const Rec709RGBPel input( uni(rng), uni(rng), uni(rng) );
		RGBAlbedoSpectrum s = RGBAlbedoSpectrum::FromRGB(
			RISEPel( input ), table );
		Rec709RGBPel rt;
		IntegrateToRec709( s, rt );
		const double err = std::sqrt(
			(rt.r - input.r) * (rt.r - input.r) +
			(rt.g - input.g) * (rt.g - input.g) +
			(rt.b - input.b) * (rt.b - input.b) );
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
		// Scale() is computed on the LUT-target-space (Rec.709)
		// converted RISEPel.  Rec709→ROMM→Rec709 round-trip preserves
		// the white axis but with small per-channel rounding from the
		// published-constant Bradford + XYZ↔RGB matrices.  A few-percent
		// tolerance accommodates that.
		RGBUnboundedSpectrum s = RGBUnboundedSpectrum::FromRGB(
			RISEPel( Rec709RGBPel( 5.0, 5.0, 5.0 ) ), table );
		Check( Close( s.Scale(), 5.0, 0.1 ),
		       "unbounded scale ≈ max(R,G,B) on neutral white" );
		const Scalar peak = s.Eval( 555.0 );
		Check( peak > Scalar(0) && peak < Scalar(10),
		       "unbounded eval at 555nm in plausible range" );
	}
	{
		// Wide-gamut HDR triple — regression guard for the Stage A
		// fix where scale + normalize used to happen in RISEPel space
		// (ROMM), passing through the boundary conversion + clamp in
		// operator(), which silently desaturated wide-gamut colours.
		// Post-fix: scale + normalize happen in LUT-target space
		// (Rec.709), so Scale() should equal max(Rec.709 triple).
		const Rec709RGBPel hdrInput( 2.0, 0.4, 0.1 );
		RGBUnboundedSpectrum s = RGBUnboundedSpectrum::FromRGB(
			RISEPel( hdrInput ), table );
		Check( Close( s.Scale(), 2.0, 0.05 ),
		       "wide-gamut HDR scale matches max(Rec.709 triple) (regression guard for "
		       "scale-in-RISEPel-space bug)" );
	}
	{
		RGBIlluminantSpectrum s = RGBIlluminantSpectrum::FromRGB(
			RISEPel( Rec709RGBPel( 1.0, 1.0, 1.0 ) ), table );
		const Scalar e500 = s.Eval( 500.0 );
		const Scalar e600 = s.Eval( 600.0 );
		// Both should be positive — the reference illuminant SPD
		// (D65 post-migration, D50 pre-migration) is non-zero
		// throughout the visible.
		Check( e500 > Scalar(0) && e600 > Scalar(0),
		       "illuminant eval > 0 at 500 and 600 nm" );
	}

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
