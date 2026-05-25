//////////////////////////////////////////////////////////////////////
//
//  ViewTransform.cpp - Stage 1, 2, 4 pipeline application + presets.
//
//  Sequencing (Stages 3, 5, 6 are TargetFormat's responsibility):
//
//    in (linear RISEPel — Rec.709 Linear D65 post Stage B
//                          colour-space migration)
//      → multiply by 2^exposureEV                  [Stage 1]
//      → matrix3 multiply by whiteBalance          [Stage 2 — identity fast-path]
//      → apply RISEPel→target primaries matrix     [Stage 3]
//      → apply tone curve (iff applyToneCurve)     [Stage 4]
//      → out (linear, in target primaries)
//
//  Tone curve operates per channel after primaries conversion.  This
//  matches the existing FileRasterizerOutput / DisplayTransformWriter
//  flow byte-for-byte (the curve was applied to display-space values
//  there too), so PNG output through the new pipeline matches PNG
//  output through the old at the L2/L3 regression gate.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ViewTransform.h"
#include "FrameStoreColorSpace.h"

#include <cmath>

namespace RISE
{
	namespace FrameStoreOutput
	{
		ViewTransform ViewTransform::Identity()
		{
			ViewTransform xf;
			// All defaults already represent identity:
			//   exposureEV = 0, whiteBalance = identity Matrix3,
			//   toneCurve = None, toneCurveStrength = 1.
			return xf;
		}

		ViewTransform ViewTransform::ForLDRDisplay( float exposureEV, ToneCurve tc )
		{
			ViewTransform xf;
			xf.exposureEV = exposureEV;
			xf.toneCurve = tc;
			xf.toneCurveStrength = 1.0f;
			return xf;
		}

		ViewTransform ViewTransform::ForHDRDisplay( float exposureEV )
		{
			ViewTransform xf;
			xf.exposureEV = exposureEV;
			// Critically: tone curve OFF for HDR display.  The display
			// compositor (CAMetalLayer EDR, DXGI scRGB) handles tone
			// mapping; baking an SDR ACES curve here would compress the
			// highlights the user is trying to see at extended dynamic
			// range.
			xf.toneCurve = eDisplayTransform_None;
			xf.toneCurveStrength = 1.0f;
			return xf;
		}

		// Detect whiteBalance == identity by bit-comparison of the 9
		// fields.  Strict bit equality is fine here: both producers
		// (Identity() / ForLDRDisplay() / ForHDRDisplay()) use the
		// Matrix3 default constructor, which sets exact 0 / 1 values.
		// A user-set whiteBalance that's "approximately identity"
		// will fall through to the matrix-multiply path; that's the
		// safer behaviour (a 1e-15 perturbation that "should" be
		// identity gets multiplied through, not bit-tested away).
		static inline bool IsIdentityWB( const Matrix3& m )
		{
			return ( m._00 == 1.0 && m._01 == 0.0 && m._02 == 0.0
			      && m._10 == 0.0 && m._11 == 1.0 && m._12 == 0.0
			      && m._20 == 0.0 && m._21 == 0.0 && m._22 == 1.0 );
		}

		void ApplyViewTransformLinear(
			const ViewTransform& xf,
			FSColorSpace         targetSpace,
			bool                 applyToneCurve,
			double               linearPel_R,
			double               linearPel_G,
			double               linearPel_B,
			double&              outR,
			double&              outG,
			double&              outB )
		{
			// Stage 1: exposure.  Use std::ldexp to multiply by 2^EV
			// without going through pow() (faster + bit-exact).  Note
			// that EV is float in the struct but std::ldexp takes int
			// for the exponent — we want the fractional part too, so
			// we use std::pow(2.0, EV) instead.  std::pow(2.0, ...)
			// is special-cased by glibc / msvc to a fast path.
			double r = linearPel_R;
			double g = linearPel_G;
			double b = linearPel_B;

			if ( xf.exposureEV != 0.0f ) {
				const double scale = std::pow( 2.0, static_cast<double>( xf.exposureEV ) );
				r *= scale;
				g *= scale;
				b *= scale;
			}

			// Stage 2: white balance (per-channel, RISEPel-space).  Skip
			// when the matrix is exactly identity.
			if ( !IsIdentityWB( xf.whiteBalance ) ) {
				const Matrix3& m = xf.whiteBalance;
				const double rr = m._00 * r + m._01 * g + m._02 * b;
				const double gg = m._10 * r + m._11 * g + m._12 * b;
				const double bb = m._20 * r + m._21 * g + m._22 * b;
				r = rr; g = gg; b = bb;
			}

			// Stage 3: RISEPel linear → target colour space's linear primaries.
			ConvertPelToTargetPrimaries( targetSpace, r, g, b, r, g, b );

			// Stage 4: tone curve (LDR fixed targets only).
			if ( applyToneCurve && xf.toneCurve != eDisplayTransform_None )
			{
				// Apply per channel.  RISEPel is just a triple of
				// doubles and DisplayTransforms::Apply takes RISEPel
				// natively — the curve is colour-space agnostic.
				// IMPORTANT: do NOT re-pack into ROMMRGBPel here.  Pre
				// Stage B `RISEPel = ROMMRGBPel`, so the pack+unpack was
				// a free no-op.  Post Stage B `RISEPel = Rec709RGBPel`,
				// and a `ROMMRGBPel` → `RISEPel` conversion at the call
				// fires `ColorUtils::ROMMRGBtoRec709RGB` (Bradford D50→
				// D65 + matrix), then the return-value conversion fires
				// the inverse — net effect ~25 % per-channel error on
				// non-grey colours, breaking every LDR-fixed tone-curved
				// output (RGBA8_sRGB, RGB8_sRGB, BGRA8_sRGB, etc.).
				RISEPel pel;
				pel.r = r; pel.g = g; pel.b = b;
				const RISEPel curved = DisplayTransforms::Apply( xf.toneCurve, pel );

				if ( xf.toneCurveStrength >= 1.0f ) {
					r = curved.r; g = curved.g; b = curved.b;
				} else if ( xf.toneCurveStrength <= 0.0f ) {
					// Strength 0 → bypass (already done by skipping).
				} else {
					// Lerp between original and curved.
					const float t = xf.toneCurveStrength;
					const float oneMinusT = 1.0f - t;
					r = oneMinusT * r + t * curved.r;
					g = oneMinusT * g + t * curved.g;
					b = oneMinusT * b + t * curved.b;
				}
			}

			outR = r;
			outG = g;
			outB = b;
		}
	}
}
