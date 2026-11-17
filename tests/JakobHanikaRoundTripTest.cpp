//////////////////////////////////////////////////////////////////////
//
//  JakobHanikaRoundTripTest.cpp - Validates the spectral upsampling
//    LUT (extlib/jakob-hanika-luts/<target>.coeff, baked into
//    src/Library/Utilities/Color/RGBToSpectrumTable_LUTData.cpp) and
//    the runtime types that consume it.
//
//    Two forward models are exercised, matching the Stage C
//    convention (2026-09-02, docs/SPECTRAL_ILLUMINANT_CONVENTION.md):
//
//    REFLECTANCE (RGBAlbedoSpectrum / RGBUnboundedSpectrum) — what
//    the LUT is trained to invert:
//
//      rgb = M_XYZ→709 · ( ∫ S(λ)·D65(λ)·cmf(λ) dλ )
//                        / ( ∫ D65(λ)·ȳ(λ) dλ )
//
//    RADIANCE SOURCE (RGBIlluminantSpectrum) — what the FILM does,
//    with no illuminant factor of its own (the source carries it):
//
//      rgb = M_XYZ→709 · ( ∫ L(λ)·cmf(λ) dλ ) / ( ∫ ȳ(λ) dλ )
//
//    The D65 weights come from RGBIlluminantSpectrum::
//    ReferenceIlluminant, i.e. the production table — so if the LUT
//    generator's private copy of the SPD ever drifts from the
//    runtime's, these round-trips break.  That is deliberate.
//
//    For each of N random in-gamut Rec.709 RGB triples:
//      1. Construct an RGBAlbedoSpectrum from the rgb (the lookup
//         goes through the runtime RISEPel→Rec.709 boundary).
//      2. Apply the reflectance forward model above.
//      3. Assert the round-trip matches the input within tolerance.
//
//    "In-gamut" here means rgb that lands inside the Rec.709 gamut
//    interior.  Post-Stage-C the LUT converges on 100 % of cells
//    (max residual < 1e-4), so the residual error these checks see is
//    dominated by the LUT's grid quantisation between cells
//    (~1/63 ≈ 0.016 on the x/y axes), not by the sigmoid's
//    expressiveness.
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

	// CIE 1931 2° observer at 5nm spacing, 380-780nm.  Same data RISE
	// uses elsewhere via ColorUtils::XYZFromNM, sampled here at every
	// step (no interpolation needed since we control the sampling).
	const int kLambdaMin  = 380;
	const int kLambdaMax  = 780;
	const int kLambdaStep = 5;
	const int kN          = (kLambdaMax - kLambdaMin) / kLambdaStep + 1;

	// XYZ(D65) → Rec.709(D65) via the codebase's mxXYZtoRec709 matrix.
	// No chromatic adapt — both source and target share D65.  Keep in
	// sync with src/Library/Utilities/Color/Color.cpp (mxXYZtoRec709).
	void XYZToRec709( double X, double Y, double Z, Rec709RGBPel& rec709 )
	{
		rec709.r = Scalar(  3.240479 * X - 1.537150 * Y - 0.498535 * Z );
		rec709.g = Scalar( -0.969256 * X + 1.875992 * Y + 0.041556 * Z );
		rec709.b = Scalar(  0.055648 * X - 0.204043 * Y + 1.057311 * Z );
	}

	// REFLECTANCE forward model — the one the LUT is trained to invert.
	// Mirror of tools/JakobHanikaLUTGen.cpp::IntegrateToTarget for the
	// rec709 target; keep in sync.
	//
	//   rgb = M · ∫ S·D65·cmf dλ / ∫ D65·ȳ dλ
	//
	// The D65 weights come from the RUNTIME's table (via
	// RGBIlluminantSpectrum::ReferenceIlluminant), so a drift between
	// the generator's private SPD copy and the runtime's shows up here.
	// Its Y-normalisation cancels out of the ratio.
	template< typename Spectrum >
	void IntegrateReflectanceToRec709( const Spectrum& s, Rec709RGBPel& rec709 )
	{
		double X = 0, Y = 0, Z = 0;
		double normY = 0;

		for( int i = 0; i < kN; ++i ) {
			const Scalar lambda = Scalar(kLambdaMin) + Scalar(i * kLambdaStep);
			XYZPel obs;
			if( !ColorUtils::XYZFromNM( obs, lambda ) ) continue;
			const double illum   = double( RGBIlluminantSpectrum::ReferenceIlluminant( lambda ) );
			const double specVal = double( s( lambda ) ) * illum;

			X += specVal * obs.X;
			Y += specVal * obs.Y;
			Z += specVal * obs.Z;
			normY += illum * obs.Y;
		}
		const double inv = 1.0 / normY;
		XYZToRec709( X * inv, Y * inv, Z * inv, rec709 );
	}

	// RADIANCE forward model — exactly what the film does with a
	// spectral radiance sample.  See
	// PixelBasedSpectralIntegratingRasterizer::TakeSingleSample
	// (mYNormalization = (b-a)/k_y, k_y = ∫ȳ dλ) and FilteredFilm's
	// matching XYZ→RISEPel resolve.  NO illuminant factor: a radiance
	// source is expected to carry its own SPD shape.
	//
	//   rgb = M · ∫ L·cmf dλ / ∫ ȳ dλ
	template< typename Spectrum >
	void IntegrateRadianceToRec709( const Spectrum& s, Rec709RGBPel& rec709 )
	{
		double X = 0, Y = 0, Z = 0;
		double normY = 0;

		for( int i = 0; i < kN; ++i ) {
			const Scalar lambda = Scalar(kLambdaMin) + Scalar(i * kLambdaStep);
			XYZPel obs;
			if( !ColorUtils::XYZFromNM( obs, lambda ) ) continue;
			const double specVal = double( s( lambda ) );

			X += specVal * obs.X;
			Y += specVal * obs.Y;
			Z += specVal * obs.Z;
			normY += obs.Y;
		}
		const double inv = 1.0 / normY;
		XYZToRec709( X * inv, Y * inv, Z * inv, rec709 );
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
		// Pure grey (0.5, 0.5, 0.5).  Under the Stage C convention a
		// FLAT reflectance is exactly neutral, so the solved cell is
		// c ≈ (0, 0, 0.797) — sigmoid ≈ 0.5 at every λ.  Measured
		// max per-channel deviation 7.0e-6; tolerance 2e-3 leaves two
		// orders of margin for LUT-rebuild jitter.  (Pre-Stage-C this
		// check ran at 0.05 because flat-E training forced a sloped
		// spectrum on every grey.)
		const Rec709RGBPel input( 0.5, 0.5, 0.5 );
		RGBAlbedoSpectrum s = RGBAlbedoSpectrum::FromRGB(
			RISEPel( input ), table );
		Rec709RGBPel rt;
		IntegrateReflectanceToRec709( s, rt );
		std::printf( "    grey 0.5 -> (%.6f, %.6f, %.6f)\n", rt.r, rt.g, rt.b );
		Check( Close( rt.r, input.r, 2e-3 ) &&
		       Close( rt.g, input.g, 2e-3 ) &&
		       Close( rt.b, input.b, 2e-3 ),
		       "grey (0.5, 0.5, 0.5) round-trips within 2e-3" );
	}
	{
		// Near-white (0.95, 0.95, 0.95).  The sigmoid asymptotes at 1
		// rather than reaching it, but under the Stage C convention the
		// TARGET of a near-white cell is a near-flat spectrum, which the
		// asymptote approximates to well under a display quantum.
		// Measured max per-channel deviation 5.2e-5 — the tolerance is
		// 2e-3, tightened 50x from the pre-Stage-C 0.10 that the flat-E
		// LUT's white-corner collapse forced.
		const Rec709RGBPel input( 0.95, 0.95, 0.95 );
		RGBAlbedoSpectrum s = RGBAlbedoSpectrum::FromRGB(
			RISEPel( input ), table );
		Rec709RGBPel rt;
		IntegrateReflectanceToRec709( s, rt );
		std::printf( "    near-white 0.95 -> (%.6f, %.6f, %.6f)\n", rt.r, rt.g, rt.b );
		Check( Close( rt.r, input.r, 2e-3 ) &&
		       Close( rt.g, input.g, 2e-3 ) &&
		       Close( rt.b, input.b, 2e-3 ),
		       "near-white (0.95, 0.95, 0.95) round-trips within 2e-3" );
	}
	{
		// Pure white (1, 1, 1) — the cell the pre-Stage-C LUT could not
		// represent at all (it collapsed to 1.28e-5 at 660 nm; see
		// tests/JHWhiteGuardSpectralTest.cpp for that history).  Now it
		// is the sigmoid's asymptote and round-trips to 6.7e-5.
		const Rec709RGBPel input( 1.0, 1.0, 1.0 );
		RGBAlbedoSpectrum s = RGBAlbedoSpectrum::FromRGB(
			RISEPel( input ), table );
		Rec709RGBPel rt;
		IntegrateReflectanceToRec709( s, rt );
		std::printf( "    white 1.0 -> (%.6f, %.6f, %.6f)\n", rt.r, rt.g, rt.b );
		Check( Close( rt.r, input.r, 2e-3 ) &&
		       Close( rt.g, input.g, 2e-3 ) &&
		       Close( rt.b, input.b, 2e-3 ),
		       "white (1, 1, 1) round-trips within 2e-3" );
		Check( s.Eval( Scalar(660) ) > Scalar(0.99),
		       "white's spectrum does NOT collapse at the red end (>= 0.99 at 660nm)" );
	}
	{
		// Saturated red, bounded at 0.8 to stay in the well-
		// conditioned interior of the gamut.  Measured L2 1.6e-4;
		// tolerance 5e-3 (was 0.05).
		const Rec709RGBPel input( 0.8, 0.2, 0.2 );
		RGBAlbedoSpectrum s = RGBAlbedoSpectrum::FromRGB(
			RISEPel( input ), table );
		Rec709RGBPel rt;
		IntegrateReflectanceToRec709( s, rt );
		const double err = std::sqrt(
			(rt.r - input.r) * (rt.r - input.r) +
			(rt.g - input.g) * (rt.g - input.g) +
			(rt.b - input.b) * (rt.b - input.b) );
		Check( err < 5e-3,
		       "saturated red (0.8, 0.2, 0.2) round-trips L2 < 5e-3" );
		if( err >= 5e-3 ) {
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
		IntegrateReflectanceToRec709( s, rt );
		const double err = std::sqrt(
			(rt.r - input.r) * (rt.r - input.r) +
			(rt.g - input.g) * (rt.g - input.g) +
			(rt.b - input.b) * (rt.b - input.b) );
		sumErr += err;
		if( err > worstErr ) {
			worstErr = err;
			worstIdx = i;
		}
		if( err < 5e-3 ) {
			++withinLoose;
		}
	}
	const double meanErr = sumErr / kSamples;
	std::printf( "  %d samples, mean L2 err = %.5f, max = %.5f (idx %d), "
	             "within-5e-3 = %d/%d\n",
		kSamples, meanErr, worstErr, worstIdx, withinLoose, kSamples );
	// Measured post-Stage-C: mean 1.1e-4, max 3.5e-4.  Thresholds
	// tightened ~10x from the pre-Stage-C 0.02 / 0.05 pair, which had
	// to absorb the flat-E LUT's unconverged cells.
	Check( meanErr < 2e-3, "interior mean L2 error < 2e-3" );
	Check( withinLoose >= int(0.95 * kSamples),
	       "≥ 95% of samples within 5e-3 L2 (in-gamut acceptance)" );

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
		// Illuminant kind = RADIANCE SOURCE.  This is a real round-trip
		// through the FILM's forward model (no illuminant factor — the
		// source carries D65 itself), which is the whole point of the
		// kind: an RGB-authored light must resolve back to the authored
		// RGB, brightness included.  Pre-Stage-C this block could only
		// assert positivity, because the flat-E LUT double-applied D65
		// (sigmoid already carried a whitepoint-correcting slope) and
		// the peak-at-560 normalisation left the result ~1.1 % dim.
		//
		// Measured max per-channel deviation: white 6.7e-5, 0.5 grey
		// 3.4e-5, saturated (0.8,0.2,0.2) 5.9e-5.  Tolerance 2e-3.
		struct Case { const char* name; Rec709RGBPel rgb; };
		const Case cases[] = {
			{ "white (1,1,1)",             Rec709RGBPel( 1.0, 1.0, 1.0 ) },
			{ "0.5 grey",                  Rec709RGBPel( 0.5, 0.5, 0.5 ) },
			{ "saturated (0.8,0.2,0.2)",   Rec709RGBPel( 0.8, 0.2, 0.2 ) }
		};
		for( const Case& c : cases ) {
			RGBIlluminantSpectrum s = RGBIlluminantSpectrum::FromRGB(
				RISEPel( c.rgb ), table );
			Rec709RGBPel rt;
			IntegrateRadianceToRec709( s, rt );
			std::printf( "    illuminant %-24s -> (%.6f, %.6f, %.6f)\n",
				c.name, rt.r, rt.g, rt.b );
			Check( Close( rt.r, c.rgb.r, 2e-3 ) &&
			       Close( rt.g, c.rgb.g, 2e-3 ) &&
			       Close( rt.b, c.rgb.b, 2e-3 ),
			       "illuminant source round-trips through the film model within 2e-3" );
		}

		// The reference illuminant itself must be Y-normalised: a
		// perfectly flat unit spectrum times it resolves to Y = 1.
		// This is the constant that makes an authored-white light as
		// bright in a spectral render as in an RGB one.
		struct FlatUnit {
			Scalar operator()( Scalar lambda_nm ) const {
				return RGBIlluminantSpectrum::ReferenceIlluminant( lambda_nm );
			}
		};
		Rec709RGBPel refRt;
		IntegrateRadianceToRec709( FlatUnit(), refRt );
		std::printf( "    reference illuminant alone -> (%.6f, %.6f, %.6f)\n",
			refRt.r, refRt.g, refRt.b );
		Check( Close( refRt.r, Scalar(1), 1e-3 ) &&
		       Close( refRt.g, Scalar(1), 1e-3 ) &&
		       Close( refRt.b, Scalar(1), 1e-3 ),
		       "reference illuminant is Y-normalised (resolves to Rec.709 white, Y=1)" );
	}

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
