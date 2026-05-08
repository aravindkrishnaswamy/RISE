//////////////////////////////////////////////////////////////////////
//
//  RGBSpectra.cpp - D50 illuminant data table for
//    RGBIlluminantSpectrum.  See RGBSpectra.h.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RGBSpectra.h"
#include <algorithm>
#include <cmath>

using namespace RISE;

namespace
{
	// CIE Standard Illuminant D50 SPD at 5nm spacing, 380-780nm
	// (81 entries).  Source: CIE 015:2018 Table A.5.  Values
	// normalized so D50(560nm) = 100.  Same table the LUT generator
	// (tools/JakobHanikaLUTGen.cpp) uses — keep in sync.
	const int    kLambdaMin  = 380;
	const int    kLambdaMax  = 780;
	const int    kLambdaStep = 5;
	const int    kNLambda    = (kLambdaMax - kLambdaMin) / kLambdaStep + 1;

	const double kD50[ kNLambda ] = {
		 23.94,  25.45,  26.96,  25.72,  24.49,  27.18,  29.87,  39.59,  49.31,  52.91,
		 56.51,  58.27,  60.03,  58.93,  57.82,  66.32,  74.82,  81.04,  87.25,  88.93,
		 90.61,  90.99,  91.37,  93.24,  95.11,  93.54,  91.96,  93.84,  95.72,  96.17,
		 96.61,  96.87,  97.13,  99.61, 102.10, 101.43, 100.75, 101.54, 102.32, 101.16,
		100.00,  98.87,  97.74,  98.33,  98.92,  96.21,  93.50,  95.59,  97.69, 98.48,
		 99.27, 99.155, 99.04,  97.38,  95.72,  97.29,  98.86,  97.26,  95.67,  96.93,
		 98.19, 100.60, 103.00, 101.07,  99.13,  93.26,  87.38,  89.49,  91.60,  92.25,
		 92.89,  84.87,  76.85,  81.68,  86.51,  89.55,  92.58,  85.40,  78.23,  67.96,
		 57.69
	};

	inline double LookupD50( double lambda_nm )
	{
		if( lambda_nm < kLambdaMin || lambda_nm > kLambdaMax ) return 0.0;
		const double f = ( lambda_nm - kLambdaMin ) / double(kLambdaStep);
		const int    i0 = std::min( int( std::floor( f ) ), kNLambda - 1 );
		const int    i1 = std::min( i0 + 1, kNLambda - 1 );
		const double t  = f - double(i0);
		return kD50[i0] * (1.0 - t) + kD50[i1] * t;
	}

	// Normalization factor: D50 reference flux at the 555 nm peak
	// of human photopic response.  Dividing by this gives a
	// "spectrum normalized to D50" with peak ~1.0 at midband.
	const double kD50Peak = 103.00;	// D50(555nm) from kD50 table
}

Scalar RGBIlluminantSpectrum::Eval( Scalar lambda_nm ) const
{
	const Scalar sig    = poly.Eval( lambda_nm );
	const Scalar d50val = Scalar( LookupD50( double( lambda_nm ) ) / kD50Peak );
	return scale * sig * d50val;
}
