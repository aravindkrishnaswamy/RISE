//////////////////////////////////////////////////////////////////////
//
//  RGBSpectra.h - Three flavors of RGB-derived spectra built on top
//    of RGBSigmoidPolynomial / RGBToSpectrumTable:
//
//      RGBAlbedoSpectrum     - reflectance ∈ [0, 1]; sigmoid only.
//                              For baseColor, sheen_color,
//                              transmission_color, etc.
//
//      RGBUnboundedSpectrum  - radiance / illuminant value ≥ 0;
//                              sigmoid scaled by max(R, G, B).  For
//                              emissive painters and HDR EXR sources
//                              whose RGB exceeds 1.0.
//
//      RGBIlluminantSpectrum - illuminant; sigmoid · scale · D50.
//                              For light SPDs authored as an RGB
//                              colour temperature equivalent.
//
//    All three are POD-ish value types with closed-form Eval(λ).
//    Used by UniformColorPainter (eager, cached at construction)
//    and TexturePainter (sample-time, per-texel uplift).
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
	// Bounded reflectance: input rgb is clamped to [0, 1] before LUT
	// lookup.  Eval(λ) returns sigmoid(c0·λ̃² + c1·λ̃ + c2) ∈ [0, 1].
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

	// Unbounded radiance / illuminant: rgb may have components > 1.
	// Stored as (sigmoid_for_normalized_rgb, scale = max_channel).
	// At evaluation: scale · sigmoid(λ).  Round-trip preserves the
	// peak channel; non-peak channels round-trip through the same
	// sigmoid · scale (slight chromaticity shift at hot pixels — same
	// as PBRT-v4).
	//
	// IMPORTANT: scale + normalize MUST run in the LUT's target colour
	// space.  Doing the math on the raw RISEPel and passing through
	// the RISEPel-taking table overload would have the matrix-converted
	// normalized triple clamped to [0, 1] inside operator(), which
	// silently desaturates any wide-gamut colour and breaks the round-
	// trip.  We convert to Rec.709 FIRST, normalize there, and call the
	// typed table overload that skips the boundary conversion.  When
	// RISEPel == Rec709RGBPel (post Stage B) the conversion is identity.
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

	// Illuminant: an unbounded spectrum pre-multiplied by a reference
	// illuminant SPD.  RISE uses the reference illuminant matching the
	// LUT's target whitepoint (D65 for Rec.709 post 2026-05 migration;
	// D50 historically for ROMM).  A pure-white RGB input authored
	// under "neutral" light returns the reference SPD rather than a
	// flat spectrum.  Used for `directional_light` SPDs authored as
	// RGB (e.g. converting a Hosek-Wilkie integrated solar XYZ → RGB
	// back to a usable SPD).
	//
	// Eval(λ) = scale · sigmoid(λ) · refSPD(λ_normalized)
	//
	// Reference SPD is sampled at the same 5-nm spacing 380-780 nm as
	// the CIE_DATA used elsewhere in RISE; data table lives in the .cpp.
	//
	// Same in-LUT-target-space scale/normalize discipline as
	// RGBUnboundedSpectrum (see comment above).
	class RGBIlluminantSpectrum
	{
	public:
		RGBIlluminantSpectrum() : scale( Scalar(1) ) {}

		static RGBIlluminantSpectrum FromRGB( const RISEPel& rgb,
		                                       const RGBToSpectrumTable& table = RGBToSpectrumTable::Get() )
		{
			RGBIlluminantSpectrum s;
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

		// Evaluate at λ in nm.  Multiplies the sigmoid by the
		// linearly-interpolated reference SPD value at that wavelength.
		Scalar Eval( Scalar lambda_nm ) const;
		Scalar operator()( Scalar lambda_nm ) const { return Eval( lambda_nm ); }

	private:
		RGBSigmoidPolynomial poly;
		Scalar               scale;
	};
}

#endif
