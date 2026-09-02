//////////////////////////////////////////////////////////////////////
//
//  RGBSpectra.cpp - Reference illuminant SPD for
//    RGBIlluminantSpectrum.  See RGBSpectra.h.
//
//  Since 2026-05 the reference illuminant is D65 (matching the new
//  Rec.709 Linear LUT target's D65 whitepoint).  Pre-2026-05 RISE
//  used D50 (matching ROMM's whitepoint).  The illuminant must
//  match the LUT's training whitepoint so a pure-white RGB input
//  authored under "neutral" light returns an SPD whose CIE round-
//  trip lands back on the input chromaticity.  Mixing D50 SPD with
//  a D65-trained sigmoid produces a small but measurable chromatic
//  shift (~2-4% per channel across the visible) on every RGB-
//  authored illuminant / emissive painter.
//
//  Stage C (2026-09-02, docs/SPECTRAL_ILLUMINANT_CONVENTION.md)
//  changed the SPD's NORMALISATION from "peak = 1 at 560 nm" to
//  "Y = 1 through the film": the SPD is divided by
//  ∫D65·ȳ dλ / ∫ȳ dλ instead of by D65(560) = 100.  The film
//  resolves radiance as ∫L·cmf dλ / ∫ȳ dλ, so with this scaling an
//  authored-white illuminant of scale 1 lands on film luminance
//  Y = 1 — identical to what the RGB pipe produces for the same
//  authored colour.  Under the old peak normalisation it landed on
//  Y = 0.98892, a silent ~1.1 % spectral-vs-RGB brightness gap.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RGBSpectra.h"
#include "ColorUtils.h"
#include <algorithm>
#include <cmath>

using namespace RISE;

namespace
{
	// CIE Standard Illuminant D65 SPD at 5nm spacing, 380-780nm
	// (81 entries).  Source: CIE 015:2018 Table A.1.  Values
	// normalized so D65(560nm) = 100.
	//
	// Why D65, not D50: matches the Rec.709 Linear LUT target's
	// whitepoint.  Since Stage C the LUT generator's forward model
	// (tools/JakobHanikaLUTGen.cpp::IntegrateToTarget for the rec709
	// target) integrates sigmoid × THIS SPD × CIE 1931 and matrix-
	// converts XYZ(D65) → Rec709(D65) with no chromatic adapt — i.e.
	// the trained sigmoid means "reflectance under D65".  A light
	// SOURCE authored as RGB is therefore that reflectance TIMES the
	// illuminant: the runtime multiplies the sigmoid by THIS SPD
	// per-wavelength, and the product round-trips through the film
	// back to the authored RGB.  Both ends must reference the same
	// whitepoint — D65.
	//
	// This table is duplicated verbatim in tools/JakobHanikaLUTGen.cpp
	// (a deliberately standalone single-file tool that includes no RISE
	// headers).  Keep the two in sync.
	const int    kLambdaMin  = 380;
	const int    kLambdaMax  = 780;
	const int    kLambdaStep = 5;
	const int    kNLambda    = (kLambdaMax - kLambdaMin) / kLambdaStep + 1;

	const double kD65[ kNLambda ] = {
		 49.9755,  52.3118,  54.6482,  68.7015,  82.7549,  87.1204,  91.4860,  92.4589,  93.4318,  90.0570,
		 86.6823,  95.7736, 104.8650, 110.9360, 117.0080, 117.4100, 117.8120, 116.3360, 114.8610, 115.3920,
		115.9230, 112.3670, 108.8110, 109.0820, 109.3540, 108.5780, 107.8020, 106.2960, 104.7900, 106.2390,
		107.6890, 106.0470, 104.4050, 104.2250, 104.0460, 102.0230, 100.0000,  98.1671,  96.3342,  96.0611,
		 95.7880,  92.2368,  88.6856,  89.3459,  90.0062,  89.8026,  89.5991,  88.6489,  87.6987,  85.4936,
		 83.2886,  83.4939,  83.6992,  81.8630,  80.0268,  80.1207,  80.2146,  81.2462,  82.2778,  80.2810,
		 78.2842,  74.0027,  69.7213,  70.6652,  71.6091,  72.9790,  74.3490,  67.9765,  61.6040,  65.7448,
		 69.8856,  72.4863,  75.0870,  69.3398,  63.5927,  55.0054,  46.4182,  56.6118,  66.8054,  65.0941,
		 63.3828
	};

	inline double LookupD65( double lambda_nm )
	{
		if( lambda_nm < kLambdaMin || lambda_nm > kLambdaMax ) return 0.0;
		const double f = ( lambda_nm - kLambdaMin ) / double(kLambdaStep);
		const int    i0 = std::min( int( std::floor( f ) ), kNLambda - 1 );
		const int    i1 = std::min( i0 + 1, kNLambda - 1 );
		const double t  = f - double(i0);
		return kD65[i0] * (1.0 - t) + kD65[i1] * t;
	}

	// Y normalisation factor (Stage C).  The film resolves a spectral
	// radiance L to XYZ as  ∫L·cmf dλ / ∫ȳ dλ  (see
	// PixelBasedSpectralIntegratingRasterizer's mYNormalization =
	// (b-a)/k_y, and FilteredFilm's matching resolve).  Dividing the
	// raw D65 table by
	//
	//     kD65YNorm = ∫D65·ȳ dλ / ∫ȳ dλ
	//
	// therefore makes an authored-white illuminant of scale 1 land on
	// film luminance Y exactly 1, and — because the D65-normalised
	// chromaticity is the Rec.709 whitepoint — on Rec.709 (1, 1, 1)
	// after the film's un-adapted XYZ(D65)→Rec709(D65) matrix.  That is
	// the same value the RGB pipe produces for the same authored
	// colour, so spectral and RGB renders agree on brightness.
	//
	// Computed from the tables rather than hardcoded so it can never
	// drift from kD65 / the CIE observer.  Both integrals are Riemann
	// sums on the shared 5 nm grid, so the step cancels.  Value for the
	// tables above: 98.89248 (the old peak-at-560 constant was 100.0,
	// i.e. the previous convention was ~1.1 % dim).  Lazily initialised
	// on first use — a function-local static, so there is no
	// static-initialisation-order dependency on ColorUtils' CIE table
	// and no data race.
	double ComputeD65YNorm()
	{
		double num = 0.0, den = 0.0;
		for( int i = 0; i < kNLambda; ++i ) {
			const Scalar lambda = Scalar( kLambdaMin + i * kLambdaStep );
			XYZPel obs;
			if( !ColorUtils::XYZFromNM( obs, lambda ) ) continue;
			num += kD65[i] * double( obs.Y );
			den += double( obs.Y );
		}
		return ( den > 0.0 ) ? ( num / den ) : 100.0;
	}
}

Scalar RGBIlluminantSpectrum::ReferenceIlluminant( Scalar lambda_nm )
{
	static const double kD65YNorm = ComputeD65YNorm();
	return Scalar( LookupD65( double( lambda_nm ) ) / kD65YNorm );
}

Scalar RGBIlluminantSpectrum::Eval( Scalar lambda_nm ) const
{
	return scale * poly.Eval( lambda_nm ) * ReferenceIlluminant( lambda_nm );
}
