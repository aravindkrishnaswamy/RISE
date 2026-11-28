//////////////////////////////////////////////////////////////////////
//
//  RGBSpectra.h - RGB-derived spectra over RGBSigmoidPolynomial /
//    RGBToSpectrumTable (see docs/SPECTRAL_ILLUMINANT_CONVENTION.md).
//
//  The sigmoid LUT stores reflectance under D65 so that
//    rgb = M_XYZ->709 * (integral S*D65*cmf dlambda) / (integral D65*ybar dlambda)
//  reproduces the authored RGB (flat S = c maps to neutral grey).
//
//    RGBAlbedoSpectrum     - Bounded [0, 1] reflectance (material slots).
//    RGBUnboundedSpectrum  - Unbounded reflectance-shaped multiplier (>1
//                            HDR reflectance/BRDF; NOT an emitter).
//    RGBIlluminantSpectrum - Radiance source: scale * sigmoid * D65norm
//                            (lights, emissive slots, radiance maps).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RGB_SPECTRA_
#define RGB_SPECTRA_

#include "RGBSigmoidPolynomial.h"
#include "RGBToSpectrumTable.h"
#include "ColorMath.h"

namespace RISE
{
	// Bounded reflectance clamped to [0, 1]; Eval(lambda) returns the
	// D65-trained sigmoid in [0, 1].
	class RGBAlbedoSpectrum
	{
	public:
		RGBAlbedoSpectrum() {}

		static RGBAlbedoSpectrum FromRGB( const RISEPel& rgb,
		                                   const RGBToSpectrumTable& table = RGBToSpectrumTable::Get() )
		{
			RGBAlbedoSpectrum s;
			s.poly = table( rgb );
			return s;
		}

		Scalar operator()( Scalar lambda_nm ) const { return poly.Eval( lambda_nm ); }
		Scalar Eval( Scalar lambda_nm )       const { return poly.Eval( lambda_nm ); }

	private:
		RGBSigmoidPolynomial poly;
	};

	// Unbounded reflectance-shaped multiplier: scale * sigmoid(lambda), where
	// scale = max channel (NOT an emitter — carries no illuminant shape).
	// Must convert to LUTTargetPel (Rec.709) BEFORE scaling/normalizing so
	// wide-gamut colors are not clamped/desaturated inside table(rgb).
	class RGBUnboundedSpectrum
	{
	public:
		RGBUnboundedSpectrum() : scale( Scalar(1) ) {}

		static RGBUnboundedSpectrum FromRGB( const RISEPel& rgb,
		                                      const RGBToSpectrumTable& table = RGBToSpectrumTable::Get() )
		{
			RGBUnboundedSpectrum s;
			const LUTTargetPel rgb_target( rgb );
			s.scale = ColorMath::MaxValue( rgb_target );
			if( s.scale > Scalar(1e-9) ) {
				const LUTTargetPel norm(
					rgb_target.r / s.scale,
					rgb_target.g / s.scale,
					rgb_target.b / s.scale );
				s.poly = table( norm );
			} else {
				s.poly = RGBSigmoidPolynomial( 0, 0, 0 );
			}
			return s;
		}

		Scalar operator()( Scalar lambda_nm ) const { return scale * poly.Eval( lambda_nm ); }
		Scalar Eval( Scalar lambda_nm )       const { return scale * poly.Eval( lambda_nm ); }
		Scalar Scale() const { return scale; }

	private:
		RGBSigmoidPolynomial poly;
		Scalar               scale;
	};

	// Radiance source for lights, emissive slots, and radiance maps:
	//   Eval(lambda) = scale * sigmoid(lambda) * D65norm(lambda)
	// D65norm is Y-normalized (integral D65norm*ybar = integral ybar) so white
	// of scale 1 resolves to film Y = 1 and Rec.709 (1, 1, 1).  5-nm table
	// over 380-780 nm lives in the .cpp; normalizes in LUTTargetPel space.
	class RGBIlluminantSpectrum
	{
	public:
		// `scale` folds in 1/kD65YNorm so Eval stays a plain multiply chain.
		RGBIlluminantSpectrum() : scale( YNormReciprocal() ) {}

		static RGBIlluminantSpectrum FromRGB( const RISEPel& rgb,
		                                       const RGBToSpectrumTable& table = RGBToSpectrumTable::Get() )
		{
			RGBIlluminantSpectrum s;
			const LUTTargetPel rgb_target( rgb );
			const Scalar maxc = ColorMath::MaxValue( rgb_target );
			if( maxc > Scalar(1e-9) ) {
				const LUTTargetPel norm(
					rgb_target.r / maxc,
					rgb_target.g / maxc,
					rgb_target.b / maxc );
				s.poly = table( norm );
			} else {
				s.poly = RGBSigmoidPolynomial( 0, 0, 0 );
			}
			// Fold Y-normalization once here to avoid per-wavelength static loads in Eval.
			s.scale = maxc * YNormReciprocal();
			return s;
		}

		// Evaluate at lambda in nm (sigmoid * linearly-interpolated reference SPD).
		Scalar Eval( Scalar lambda_nm ) const;
		Scalar operator()( Scalar lambda_nm ) const { return Eval( lambda_nm ); }

		// Y-normalized reference illuminant (D65): integral(Ref*ybar) = integral(ybar).
		static Scalar ReferenceIlluminant( Scalar lambda_nm );

		// 1 / kD65YNorm (computed once from shipped tables in .cpp; folded into scale).
		static Scalar YNormReciprocal();

	private:
		RGBSigmoidPolynomial poly;
		Scalar               scale;
	};
}

#endif
