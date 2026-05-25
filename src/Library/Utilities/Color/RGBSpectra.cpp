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
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RGBSpectra.h"
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
	// whitepoint.  The LUT generator's forward model
	// (tools/JakobHanikaLUTGen.cpp::IntegrateToTarget for the rec709
	// target) integrates sigmoid × CIE 1931 under a flat illuminant
	// and matrix-converts XYZ(D65) → Rec709(D65) with NO Bradford
	// adapt.  The runtime, when materializing an RGB-authored
	// illuminant, multiplies that sigmoid by THIS SPD per-wavelength.
	// For the round-trip identity to hold (white RGB → spectrum
	// integrates to white) the SPD must reference the same
	// whitepoint as the LUT — D65.
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

	// Normalization factor: D65 SPD reference at 560 nm (= 100 by
	// the standard's normalization convention).  Dividing by this
	// gives a "spectrum normalized to D65" with peak ~1.0 at midband.
	const double kD65Peak = 100.0;
}

Scalar RGBIlluminantSpectrum::Eval( Scalar lambda_nm ) const
{
	const Scalar sig    = poly.Eval( lambda_nm );
	const Scalar d65val = Scalar( LookupD65( double( lambda_nm ) ) / kD65Peak );
	return scale * sig * d65val;
}
