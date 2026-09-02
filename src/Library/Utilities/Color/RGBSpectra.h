//////////////////////////////////////////////////////////////////////
//
//  RGBSpectra.h - Three flavors of RGB-derived spectra built on top
//    of RGBSigmoidPolynomial / RGBToSpectrumTable.
//
//    THE CONVENTION (Stage C, 2026-09-02 — full write-up in
//    docs/SPECTRAL_ILLUMINANT_CONVENTION.md):
//
//      The sigmoid the LUT stores is a REFLECTANCE VIEWED UNDER D65.
//      It is trained so that
//
//        rgb = M_XYZ→709 · ( ∫ S(λ)·D65(λ)·cmf(λ) dλ )
//                          / ( ∫ D65(λ)·ȳ(λ) dλ )
//
//      reproduces the authored RGB.  Consequences that callers rely
//      on: a flat S = c is EXACTLY the neutral grey (c, c, c), and
//      authored white is representable at the sigmoid's asymptote
//      (S ≈ 1 − ε) instead of collapsing at the red end.  Before
//      Stage C the LUT was trained under a flat (E) illuminant, which
//      made neither of those true.
//
//    The three kinds, and which material/light slot each belongs to:
//
//      RGBAlbedoSpectrum     - REFLECTANCE, clamped to [0, 1]; the
//                              sigmoid alone.  Use for every
//                              multiplicative slot: baseColor,
//                              sheen_color, specular tint,
//                              transmission_color, alpha, coat tint.
//                              Round-trips exactly (∫S·D65·cmf → rgb).
//
//      RGBUnboundedSpectrum  - REFLECTANCE-SHAPED but allowed to
//                              exceed 1: sigmoid of the chromaticity-
//                              normalised triple, times
//                              scale = max(R, G, B).  Use for
//                              multiplicative/throughput quantities
//                              whose magnitude can exceed unity (HDR
//                              texture reads feeding a reflectance
//                              slot, BRDF-value uplift).  NOT for a
//                              radiance source — it carries no
//                              illuminant shape, so a "white" source
//                              built from it is flat, not D65, and
//                              shifts the whitepoint.
//
//      RGBIlluminantSpectrum - RADIANCE SOURCE: scale · sigmoid · D65,
//                              with the D65 table Y-normalised so an
//                              authored-white spectrum of scale 1
//                              resolves to film Y = 1 and Rec.709
//                              (1, 1, 1).  Use for anything that
//                              EMITS: light SPDs authored as RGB,
//                              emissive material slots, environment /
//                              radiance maps, and shader ops that
//                              uplift a COMPUTED RADIANCE back into a
//                              spectrum.
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
	// lookup.  Eval(λ) returns sigmoid(c0·λ̃² + c1·λ̃ + c2) ∈ [0, 1],
	// meaning "the fraction of D65 this surface reflects at λ".  No
	// code change was needed at Stage C — the MEANING of the sigmoid
	// changed underneath (reflectance under D65 rather than under a
	// flat illuminant), not the evaluation.
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

	// Unbounded, reflectance-SHAPED: rgb may have components > 1.
	// Stored as (sigmoid_for_normalized_rgb, scale = max_channel).
	// At evaluation: scale · sigmoid(λ).  Round-trip preserves the
	// peak channel; non-peak channels round-trip through the same
	// sigmoid · scale (slight chromaticity shift at hot pixels — same
	// as PBRT-v4).
	//
	// Stage C note: no code change here either, but the kind's MEANING
	// is now explicit — this is a reflectance-shaped multiplier that
	// happens to exceed 1, NOT a radiance source.  A source authored
	// through this kind emits a flat-ish spectrum rather than a D65-
	// shaped one; the sites that do that (emissive slots, radiance
	// maps, radiance-uplifting shader ops) are slice 2's work, listed
	// in docs/SPECTRAL_ILLUMINANT_CONVENTION.md.
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

	// Illuminant / RADIANCE SOURCE: the reflectance sigmoid multiplied
	// by the reference illuminant SPD, which is what "an RGB-authored
	// light" physically means under the Stage C convention — the
	// authored RGB is the colour that light HAS, i.e. reflectance-
	// under-D65 × D65.  RISE's reference illuminant matches the LUT
	// target's whitepoint (D65 for Rec.709 post 2026-05; D50
	// historically for ROMM).  A pure-white RGB input returns the
	// reference SPD rather than a flat spectrum.
	//
	// Eval(λ) = scale · sigmoid(λ) · D65norm(λ)
	//
	// where D65norm is Y-normalised (∫D65norm·ȳ dλ = ∫ȳ dλ) so that a
	// white source of scale 1 resolves to film Y = 1 and Rec.709
	// (1, 1, 1) — matching the RGB pipe exactly.  Before Stage C the
	// SPD was peak-normalised at 560 nm, which was ~1.1 % dim.
	//
	// This is the kind every EMISSIVE slot should use: light SPDs
	// authored as RGB (`directional_light`, point/spot/omni),
	// emissive material slots, environment / radiance maps, and shader
	// ops that uplift a computed radiance.  Wiring those up is slice 2
	// (docs/SPECTRAL_ILLUMINANT_CONVENTION.md); until then several of
	// them still use RGBUnboundedSpectrum, which carries no illuminant
	// shape.
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

		// The Y-normalised reference illuminant itself (D65), i.e. the
		// factor Eval multiplies into the sigmoid.  Exposed so tests
		// and offline checks can reproduce the LUT's forward model
		// against the SAME table the runtime uses, instead of carrying
		// a fourth copy of the SPD.  ∫ReferenceIlluminant·ȳ dλ = ∫ȳ dλ,
		// so a flat unit spectrum times this resolves to film Y = 1.
		static Scalar ReferenceIlluminant( Scalar lambda_nm );

	private:
		RGBSigmoidPolynomial poly;
		Scalar               scale;
	};
}

#endif
