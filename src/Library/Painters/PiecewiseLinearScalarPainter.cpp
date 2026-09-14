//////////////////////////////////////////////////////////////////////
//
//  PiecewiseLinearScalarPainter.cpp - DL-29's RGB-aware evaluation
//    (ComputeCachedRGB), out-of-line so PiecewiseLinearScalarPainter.h
//    stays free of the ColorUtils.h/Color.h include chain -- see the
//    header's doc comment on `cachedRGB`.
//
//  Author: Claude (debt-precision slice, DL-29)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PiecewiseLinearScalarPainter.h"
// DL-80 (docs/DEBT_LEDGER.md): Color.h and ColorUtils.h are mutually
// circular (ColorUtils.h's first line is `#include "Color.h"`;
// Color.h's SpectralPacket.h back-includes "ColorUtils.h"), so whichever
// of the two a translation unit includes FIRST wins the include-guard
// race and gets its declarations seen by the other -- entering via
// ColorUtils.h leaves ColorUtils::XYZFromNM undeclared when
// SpectralPacket.h's nested #include "ColorUtils.h" is skipped as an
// already-in-progress guard.  Every existing .cpp that needs both
// (e.g. ColorUtils.cpp itself) happens to include Color.h first; match
// that order here rather than rely on it being incidental.
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/ColorUtils.h"
#include "../Utilities/Color/RGBSpectra.h"
#include <cmath>
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

//! DL-29 forward model, run once at construction.  Uses a FIXED 5nm
//! grid over the CIE visible range [380,780], matching the convention
//! RGBSpectra.cpp's own D65 normalisation constant uses
//! (ComputeD65YNorm) -- a plain per-index sum, no trapezoidal
//! weighting, which is exact for a RATIO of two sums built the
//! identical way (the grid step cancels).  Any consistent overall
//! scale on the illuminant term also cancels in this ratio, so using
//! the Y-normalised reference (rather than a raw D65 table this file
//! would otherwise have to duplicate) gives the identical result to
//! docs/SPECTRAL_ILLUMINANT_CONVENTION.md's forward model:
//!   rgb = M_XYZ->709 . (int S.D65.cmf dλ) / (int D65.ȳ dλ)
void PiecewiseLinearScalarPainter::ComputeCachedRGB()
{
	static const Scalar kLambdaMin  = Scalar( 380 );
	static const Scalar kLambdaStep = Scalar( 5 );
	static const int    kNSteps     = 81;	// (780-380)/5 + 1

	Scalar sumX = 0, sumY = 0, sumZ = 0, sumYIllum = 0;
	for( int i = 0; i < kNSteps; ++i ) {
		const Scalar lambda = kLambdaMin + Scalar( i ) * kLambdaStep;
		XYZPel cmf;
		if( !ColorUtils::XYZFromNM( cmf, lambda ) ) {
			continue;
		}
		const Scalar illum = RGBIlluminantSpectrum::ReferenceIlluminant( lambda );
		const Scalar s = EvalAtNM( lambda );
		sumX += s * illum * cmf.X;
		sumY += s * illum * cmf.Y;
		sumZ += s * illum * cmf.Z;
		sumYIllum += illum * cmf.Y;
	}

	if( sumYIllum < Scalar( 1e-12 ) ) {
		cachedRGB = ScalarTriple( 0 );
		bHasPerChannelVariation = false;
		return;
	}

	const XYZPel xyz( sumX / sumYIllum, sumY / sumYIllum, sumZ / sumYIllum );
	const Rec709RGBPel rgb = ColorUtils::XYZtoRec709RGB( xyz );

	// Snap to exact-uniform when the three channels agree within
	// floating-point noise (see bHasPerChannelVariation's doc comment
	// in the header) -- a RELATIVE tolerance against the triple's own
	// magnitude, since an absolute one would be wrong at both very
	// small and very large physical-scalar magnitudes (this pipe
	// carries values like `scattering 1000000` alongside values like a
	// [0,1] tau curve).
	const Scalar maxAbs = std::max( std::fabs( Scalar( rgb.r ) ),
		std::max( std::fabs( Scalar( rgb.g ) ), std::fabs( Scalar( rgb.b ) ) ) );
	const Scalar spread = std::max( std::fabs( Scalar( rgb.r ) - Scalar( rgb.g ) ),
		std::max( std::fabs( Scalar( rgb.g ) - Scalar( rgb.b ) ),
		          std::fabs( Scalar( rgb.r ) - Scalar( rgb.b ) ) ) );
	// 1e-3, not a tighter ULP-level tolerance: measured directly on a
	// FLAT curve (constant 1.33 at every wavelength) through this exact
	// pipeline, R/G/B come out as 1.329884655 / 1.330028877 / 1.330070275
	// -- a ~1.4e-4 RELATIVE spread from the D65-weighted CMF integration
	// and the XYZ->Rec709 matrix not being perfectly symmetric under a
	// constant integrand (same root cause RGBAlbedoSpectrum's authored-
	// white round-trips to "1 - epsilon" rather than exactly 1, per
	// docs/SPECTRAL_ILLUMINANT_CONVENTION.md).  A curve with genuine
	// per-channel variation (e.g. this file's own colors/linear.ior-
	// shaped test fixture, 1.10 at 380nm to 1.45 at 720nm) reads a
	// ~0.15 relative spread -- three orders of magnitude clear of this
	// threshold, so there is no risk of a real gradient being folded
	// into "flat" here.
	static const Scalar kUniformRelTol = Scalar( 1e-3 );

	if( maxAbs < Scalar( 1e-12 ) || spread <= kUniformRelTol * maxAbs ) {
		cachedRGB = ScalarTriple( Scalar( rgb.r ) );
		bHasPerChannelVariation = false;
	} else {
		cachedRGB = ScalarTriple( Scalar( rgb.r ), Scalar( rgb.g ), Scalar( rgb.b ) );
		bHasPerChannelVariation = true;
	}
}
