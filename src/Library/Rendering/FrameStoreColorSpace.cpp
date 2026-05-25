//////////////////////////////////////////////////////////////////////
//
//  FrameStoreColorSpace.cpp - Implementation of output-side color
//  space conversions, transfer functions, and IEEE 754 binary16
//  half-float encode/decode for the FrameStore Render readback path.
//
//  See FrameStoreColorSpace.h for the design context and pipeline
//  ordering.
//
//  Colour-space chain rationale: rather than precomputing composite
//  Pel→{P3, BT.2020, ROMM} matrices (which would duplicate numerics
//  already maintained in Color.cpp and risk drift), we chain at runtime:
//
//      RISEPel (Rec.709 Linear post Stage B)
//        → ColorUtils::Rec709RGBtoROMMRGB           [for ROMM_Linear target]
//        OR identity                                [for sRGB_Linear target]
//        OR M_sRGB→{P3,BT2020} matrix multiply      [for P3 / BT2020 targets]
//
//  Per-pixel overhead is negligible compared to the transfer-function
//  cost.  The M_sRGB→target matrices below are well-known published
//  industry constants, NOT derived from RISE internals.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FrameStoreColorSpace.h"
#include "../Utilities/Color/ColorUtils.h"
#include "../Utilities/Color/Color.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace RISE
{
	namespace FrameStoreOutput
	{

		// ─────────────────────────────────────────────────────────────
		// Published M_sRGB → target primaries matrices (row-major 3x3)
		// ─────────────────────────────────────────────────────────────
		//
		// These are well-documented industry constants and are NOT
		// derived from RISE internals.  Both source spaces (sRGB /
		// BT.709) and target spaces (DCI-P3 D65, BT.2020 D65) share
		// the D65 white point, so no chromatic adaptation is needed
		// in these matrices.

		// sRGB / BT.709 D65 → DCI-P3 D65 (Display P3).
		// Source: computed from BT.709 and DCI-P3 D65 chromaticities
		// (both share the D65 white point so no chromatic adaptation
		// is applied).  Cross-checked against colour-science.org's
		// Python output and Apple's Display P3 reference WWDC 2017
		// Session 503; row sums are exact 1.0.  Pre-2026-05 row 2 had
		// a transcription typo in (2,1) / (2,2) (0.07239696 / 0.91052041
		// vs. canonical 0.0723974407 / 0.9105199286) — caught by the
		// L0 adversarial review.  The error was ~5e-7 (below display
		// threshold but visible to anyone cross-checking against the
		// published source).
		static const double kSRGBtoDisplayP3[9] = {
			 0.8224621560,  0.1775378440,  0.0000000000,
			 0.0331941969,  0.9668058031,  0.0000000000,
			 0.0170826307,  0.0723974407,  0.9105199286
		};

		// sRGB / BT.709 D65 → BT.2020 D65.
		// Source: ITU-R BT.2087 Annex C "Colour conversion from BT.709
		// to BT.2020 colour gamut".
		static const double kSRGBtoBT2020[9] = {
			 0.62740390,  0.32928304,  0.04331306,
			 0.06909729,  0.91954040,  0.01136231,
			 0.01639144,  0.08801331,  0.89559525
		};

		// Identity (used when target == ROMM_Linear, i.e. no conversion).
		static const double kIdentity3x3[9] = {
			1.0, 0.0, 0.0,
			0.0, 1.0, 0.0,
			0.0, 0.0, 1.0
		};

		const double* GetPelToTargetMatrix( FSColorSpace target )
		{
			// Returns the matrix that takes RISEPel (=sRGB / BT.709
			// Linear post Stage B) → the target colour space's primaries
			// (no transfer function applied).  For sRGB targets this is
			// the identity (RISEPel is already sRGB-linear).  For
			// ROMM_Linear targets, callers must use the chained
			// ConvertPelToTargetPrimaries path (this function returns
			// identity for ROMM since the ROMM matrix lives in Color.cpp).
			switch( target )
			{
				case FSColorSpace::sRGB_Linear:      return kIdentity3x3; // RISEPel IS sRGB linear (post Stage B)
				case FSColorSpace::ROMM_Linear:      return kIdentity3x3; // real conversion done in ConvertPelToTargetPrimaries
				case FSColorSpace::DisplayP3_Linear: return kSRGBtoDisplayP3;
				case FSColorSpace::BT2020_Linear:    return kSRGBtoBT2020;
			}
			return kIdentity3x3;
		}

		void ApplyMatrix3x3(
			const double* mat,
			double inR, double inG, double inB,
			double& outR, double& outG, double& outB )
		{
			// Stash inputs locally so the function is alias-safe
			// (caller may pass &outR for inR, etc.).
			const double r = inR;
			const double g = inG;
			const double b = inB;
			outR = mat[0] * r + mat[1] * g + mat[2] * b;
			outG = mat[3] * r + mat[4] * g + mat[5] * b;
			outB = mat[6] * r + mat[7] * g + mat[8] * b;
		}

		void ConvertPelToTargetPrimaries(
			FSColorSpace target,
			double pelR, double pelG, double pelB,
			double& outR, double& outG, double& outB )
		{
			// Input is RISEPel = Rec.709 / sRGB Linear (post Stage B).
			if ( target == FSColorSpace::sRGB_Linear )
			{
				// Identity — RISEPel IS sRGB linear post Stage B.
				outR = pelR;
				outG = pelG;
				outB = pelB;
				return;
			}

			if ( target == FSColorSpace::ROMM_Linear )
			{
				// Rec.709 → ROMM via the codebase's single-source-of-
				// truth `mxRec709toROMM` (Bradford D65→D50 + matrix).
				// Goes through ColorUtils so the matrix definition
				// stays maintained in one place.
				Rec709RGBPel rec709Pel;
				rec709Pel.r = pelR;
				rec709Pel.g = pelG;
				rec709Pel.b = pelB;
				const ROMMRGBPel romm = ColorUtils::Rec709RGBtoROMMRGB( rec709Pel );
				outR = romm.r;
				outG = romm.g;
				outB = romm.b;
				return;
			}

			// Stage 2 targets (DisplayP3, BT.2020): RISEPel is already
			// sRGB-linear, so apply the published sRGB→target matrix
			// directly — no first-stage conversion needed.
			ApplyMatrix3x3( GetPelToTargetMatrix( target ),
			                pelR, pelG, pelB,
			                outR, outG, outB );
		}

		// ─────────────────────────────────────────────────────────────
		// Transfer functions
		// ─────────────────────────────────────────────────────────────

		// Centralised input sanitise: collapses NaN, infinity, and
		// negative inputs to 0 the same way DisplayTransform.h does.
		// Public transfer functions all front this gate so no caller
		// can poison the encoded output by feeding through bad inputs.
		//
		// IMPORTANT: this implementation does NOT use std::isfinite /
		// std::isnan, because RISE compiles with `-ffast-math` (see
		// build/make/rise/Config.OSX) which implies `-ffinite-math-only`.
		// Under that flag, the compiler is told it may assume no Inf /
		// NaN inputs, and `std::isfinite(+Inf)` may return true.  We
		// therefore check the IEEE 754 bit pattern directly via memcpy:
		//
		//   bit  63       : sign  (set ⇒ negative or -0)
		//   bits 62..52   : exponent (all 1s ⇒ Inf or NaN)
		//   bits 51..0    : mantissa
		//
		// We reject "exponent all 1s" (catches +Inf, -Inf, all NaN
		// including signalling and quiet) and "sign bit set" (catches
		// all negatives plus -0 → 0, which is fine).  Positive finite
		// values pass through unchanged.
		static inline double Sanitise( double x )
		{
			static_assert( sizeof(double) == 8, "double must be 64-bit IEEE 754" );
			uint64_t bits;
			std::memcpy( &bits, &x, sizeof(bits) );
			constexpr uint64_t kExpMask  = 0x7FF0000000000000ULL;
			constexpr uint64_t kSignBit  = 0x8000000000000000ULL;
			if ( ( bits & kExpMask ) == kExpMask ) return 0.0;  // Inf or NaN
			if ( bits & kSignBit )                 return 0.0;  // negative or -0
			return x;
		}

		double ApplySRGBTransfer( double linear )
		{
			// Reuse the codebase's existing piecewise sRGB curve so
			// any "old PNG via FileRasterizerOutput" vs "new PNG via
			// FrameStore encoder" diff is byte-identical (the L2/L3
			// regression gate).
			return ColorUtils::SRGBTransferFunction(
				Sanitise( linear ) );
		}

		double ApplyGammaTransfer( double linear, double gamma )
		{
			const double s = Sanitise( linear );
			if ( s <= 0.0 ) return 0.0;
			return std::pow( s, 1.0 / gamma );
		}

		double ApplyPQTransfer( double linearScene )
		{
			// Sanitise + rescale: 1.0 scene-referred = 100 nits;
			// PQ domain has 1.0 = 10000 nits; so divide by 100.
			double s = Sanitise( linearScene ) * 0.01;
			if ( s > 1.0 ) s = 1.0;
			if ( s <= 0.0 ) return 0.0;

			// SMPTE ST.2084 OETF coefficients (named per the spec).
			constexpr double m1 = 0.1593017578125;     // 2610/16384/4
			constexpr double m2 = 78.84375;            // 2523/4096*128
			constexpr double c1 = 0.8359375;           // 3424/4096
			constexpr double c2 = 18.8515625;          // 2413/4096*32
			constexpr double c3 = 18.6875;             // 2392/4096*32

			const double Lm1 = std::pow( s, m1 );
			const double num = c1 + c2 * Lm1;
			const double den = 1.0 + c3 * Lm1;
			return std::pow( num / den, m2 );
		}

		double ApplyHLGTransfer( double linearScene )
		{
			// HLG OETF (BT.2100 Table 5 / ARIB STD-B67).  Input is
			// scene linear (positive); output is encoded.  BT.2100
			// defines the OETF on [0, 1] → [0, 1] but the underlying
			// formula naturally extends past 1; we don't clamp at
			// the top.  Forward + inverse round-trip exactly.
			const double s = Sanitise( linearScene );
			if ( s <= 0.0 ) return 0.0;

			constexpr double a = 0.17883277;
			constexpr double b = 0.28466892;
			constexpr double c = 0.55991073;

			if ( s <= 1.0 / 12.0 ) {
				return std::sqrt( 3.0 * s );
			}
			return a * std::log( 12.0 * s - b ) + c;
		}

		double ApplyTransfer( TransferFunction fn, double linearValue )
		{
			switch( fn )
			{
				case TransferFunction::Linear:
					// True identity: archival HDR paths
					// (RGBA32F_Linear, .hdr / EXR encoders) need
					// negative values, NaN, and Inf to pass
					// through unchanged so reconstruction-filter
					// ringing and out-of-gamut samples are
					// preserved in the file rather than silently
					// clamped to zero.  LDR-fixed targets that
					// pair with a fixed-point quantisation step do
					// their own clamping in EncodePixel's Q8/Q16.
					// See L1 adversarial review P3.
					return linearValue;
				case TransferFunction::sRGB:      return ApplySRGBTransfer( linearValue );
				case TransferFunction::PQ_ST2084: return ApplyPQTransfer( linearValue );
				case TransferFunction::HLG:       return ApplyHLGTransfer( linearValue );
				case TransferFunction::Gamma22:   return ApplyGammaTransfer( linearValue, 2.2 );
			}
			return linearValue;
		}

		double ApplyTransferInverse( TransferFunction fn, double encoded )
		{
			switch( fn )
			{
				case TransferFunction::Linear:
					// True identity inverse: matches the forward
					// pass-through above.  See L1 adversarial review P3.
					return encoded;

				case TransferFunction::sRGB:
					return ColorUtils::SRGBTransferFunctionInverse( Sanitise( encoded ) );

				case TransferFunction::PQ_ST2084: {
					// Inverse of ApplyPQTransfer.  Scales back so 1.0
					// PQ-domain (= 10000 nits) maps to 100.0 scene-
					// referred (= 100 × 100 nits).
					const double e = Sanitise( encoded );
					if ( e <= 0.0 ) return 0.0;

					constexpr double m1 = 0.1593017578125;
					constexpr double m2 = 78.84375;
					constexpr double c1 = 0.8359375;
					constexpr double c2 = 18.8515625;
					constexpr double c3 = 18.6875;

					const double em2 = std::pow( e, 1.0 / m2 );
					const double num = std::max( em2 - c1, 0.0 );
					const double den = c2 - c3 * em2;
					if ( den <= 0.0 ) return 0.0;
					const double L_pq = std::pow( num / den, 1.0 / m1 );
					return L_pq * 100.0;  // PQ-domain back to scene-referred
				}

				case TransferFunction::HLG: {
					const double e = Sanitise( encoded );
					if ( e <= 0.0 ) return 0.0;
					constexpr double a = 0.17883277;
					constexpr double b = 0.28466892;
					constexpr double c = 0.55991073;
					if ( e <= 0.5 ) {
						return ( e * e ) / 3.0;
					}
					return ( std::exp( ( e - c ) / a ) + b ) / 12.0;
				}

				case TransferFunction::Gamma22: {
					const double e = Sanitise( encoded );
					if ( e <= 0.0 ) return 0.0;
					return std::pow( e, 2.2 );
				}
			}
			return Sanitise( encoded );
		}

		// ─────────────────────────────────────────────────────────────
		// IEEE 754 binary16 (half-float) encode / decode
		// ─────────────────────────────────────────────────────────────
		//
		// Portable software encoder.  IEEE 754 binary16 layout:
		//   sign:1 | exp:5 (bias 15) | mantissa:10
		//
		// The float layout (binary32) is:
		//   sign:1 | exp:8 (bias 127) | mantissa:23
		//
		// Cases:
		//   - Zero (positive / negative): preserved.
		//   - Denormal float: encodes to half-zero (we don't bother
		//     encoding the tiny tail).
		//   - Normal float in half range: rebias exponent (127 → 15),
		//     truncate mantissa to 10 bits with round-to-nearest-even.
		//   - Float overflow into half range: emits half-infinity.
		//   - Float underflow: encodes to denormal half if representable,
		//     otherwise zero.
		//   - Inf / NaN: preserved with sign; NaN payload bits compressed.
		//
		// We don't depend on _Float16 / __fp16 / Imath::half so the
		// codebase compiles on platforms whose toolchains lack them
		// (the FrameStore L0 must build on every supported platform).

		uint16_t FloatBitsToHalf( uint32_t bits )
		{
			const uint32_t sign     = ( bits >> 16 ) & 0x8000u;
			const uint32_t exponent = ( bits >> 23 ) & 0xFFu;
			const uint32_t mantissa = bits & 0x7FFFFFu;

			// Special values: exponent == 0xFF → Inf or NaN.
			if ( exponent == 0xFFu ) {
				if ( mantissa == 0 ) {
					// +/- Inf
					return static_cast<uint16_t>( sign | 0x7C00u );
				}
				// NaN: keep sign + exponent, compress mantissa.
				// Use top mantissa bit so we never emit half-Inf
				// (which has mantissa == 0) for an input that was NaN.
				return static_cast<uint16_t>(
					sign | 0x7C00u | ( mantissa >> 13 ) | 0x0001u );
			}

			// Compute the half exponent (bias 15 vs float bias 127).
			int newExp = static_cast<int>( exponent ) - 127 + 15;

			// Overflow → half-Inf.
			if ( newExp >= 0x1F ) {
				return static_cast<uint16_t>( sign | 0x7C00u );
			}

			// Normal range.
			if ( newExp > 0 ) {
				const uint32_t halfMantissa = mantissa >> 13;
				const uint32_t roundBit = ( mantissa >> 12 ) & 0x1u;
				const uint32_t stickyBits = mantissa & 0xFFFu;
				uint16_t halfBits = static_cast<uint16_t>(
					sign | ( static_cast<uint32_t>(newExp) << 10 ) | halfMantissa );
				// Round-to-nearest-even.
				if ( roundBit && ( stickyBits || ( halfMantissa & 0x1u ) ) ) {
					halfBits = static_cast<uint16_t>( halfBits + 1 );
				}
				return halfBits;
			}

			// Subnormal range (newExp ≤ 0).  Encode if representable;
			// underflow to zero otherwise.  The "implicit 1" bit comes
			// back into the mantissa explicitly here.
			if ( newExp < -10 ) {
				// Smaller than the smallest representable subnormal.
				return static_cast<uint16_t>( sign );
			}

			const uint32_t mantissaWithImplicit = mantissa | 0x800000u;
			const int shift = 14 - newExp;  // 14..24
			uint32_t halfMantissa = mantissaWithImplicit >> shift;
			const uint32_t roundBit = ( mantissaWithImplicit >> ( shift - 1 ) ) & 0x1u;
			const uint32_t stickyBits = mantissaWithImplicit & ( ( 1u << ( shift - 1 ) ) - 1u );
			uint16_t halfBits = static_cast<uint16_t>( sign | halfMantissa );
			if ( roundBit && ( stickyBits || ( halfMantissa & 0x1u ) ) ) {
				halfBits = static_cast<uint16_t>( halfBits + 1 );
			}
			return halfBits;
		}

		uint32_t HalfToFloatBits( uint16_t h )
		{
			const uint32_t sign     = ( static_cast<uint32_t>(h) & 0x8000u ) << 16;
			const uint32_t exponent = ( h >> 10 ) & 0x1Fu;
			const uint32_t mantissa = h & 0x3FFu;

			uint32_t bits;
			if ( exponent == 0 ) {
				if ( mantissa == 0 ) {
					// +/- 0
					bits = sign;
				} else {
					// Subnormal half → normalised float.
					// Find leading-1 in mantissa, shift it to bit 10,
					// then rebias.
					//
					// Derivation: a half subnormal with mantissa M has
					// value M·2^-24.  After k left-shifts the leading
					// 1 lands at bit 10, so the normalised mantissa is
					// (M<<k)·2^-10·2^-14 = (M<<k)·2^-24 = M·2^(k-24).
					// Reading (M<<k) as 1.frac (bit 10 implicit), the
					// float exponent is therefore -24+10-k = -14-k.
					// In the loop, e = -1 - k so k = -1 - e and
					// floatExp = -14 + 1 + e + 127 = e + 114.
					//
					// (Pre-2026 commit history: this was off-by-one
					// (e+113), which silently rounded every subnormal
					// half to half its true magnitude.  Caught by the
					// FrameStoreColorMathTest 65k-pattern sweep.)
					uint32_t m = mantissa;
					int e = -1;
					while ( ( m & 0x400u ) == 0 ) {
						m <<= 1;
						e--;
					}
					m &= 0x3FFu;  // strip the (now implicit) leading 1
					const int floatExp = e + 114;
					bits = sign | ( static_cast<uint32_t>(floatExp) << 23 ) | ( m << 13 );
				}
			} else if ( exponent == 0x1Fu ) {
				// Inf or NaN.
				if ( mantissa == 0 ) {
					bits = sign | 0x7F800000u;
				} else {
					bits = sign | 0x7F800000u | ( mantissa << 13 );
				}
			} else {
				// Normal.
				const uint32_t floatExp = exponent - 15 + 127;
				bits = sign | ( floatExp << 23 ) | ( mantissa << 13 );
			}

			return bits;
		}

		// Float-typed wrappers.  These exist for callers who already
		// have a `float` value in hand; they round-trip the value via
		// memcpy.  Callers that need to distinguish NaN bit patterns
		// (e.g. tests, archival writers) should use the *Bits variants
		// directly to avoid float-typed register transfer under
		// -ffast-math, which can canonicalise NaN payloads.
		uint16_t FloatToHalf( float f )
		{
			uint32_t bits;
			std::memcpy( &bits, &f, sizeof(bits) );
			return FloatBitsToHalf( bits );
		}

		float HalfToFloat( uint16_t h )
		{
			const uint32_t bits = HalfToFloatBits( h );
			float f;
			std::memcpy( &f, &bits, sizeof(f) );
			return f;
		}

	} // namespace FrameStoreOutput
} // namespace RISE
