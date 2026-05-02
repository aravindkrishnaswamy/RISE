//////////////////////////////////////////////////////////////////////
//
//  DisplayTransform.h - Per-channel display-transform tone curves
//  applied between the linear-radiance film and an LDR output writer.
//
//  Background: a path tracer produces linear scene radiance with
//  arbitrary dynamic range.  An LDR file format (PNG / TGA / PPM /
//  TIFF / etc.) clamps anything above 1.0 to white, throwing away
//  the highlights.  A tone curve maps the (0, +inf) radiance domain
//  smoothly into [0, 1) so highlights roll off rather than clip.
//
//  These curves are NOT physically based — they are perceptual
//  display transforms.  The path tracer's integrator output (the
//  EXR primary) is the radiometric ground truth; tone curves apply
//  only when targeting an LDR display.  Never apply to HDR formats
//  (EXR / HDR / RGBEA); doing so corrupts the archival radiance.
//
//  Curves provided:
//
//    None     — identity.  Use only when downstream is HDR or when
//               the user wants the legacy "clip at 1.0" PNG output
//               for byte-for-byte regression testing.
//
//    Reinhard — x / (1 + x).  Asymptotic-to-1; cheap; doesn't
//               compress mid-tones.  Good baseline; can look flat.
//
//    ACES     — Krzysztof Narkowicz's analytic fit to ACES RRT+ODT
//               for sRGB display.  De-facto standard PBR curve;
//               matches the proper ACES chain to ~1% at ordinary
//               luminances.  Default for new scenes.
//
//    AgX      — Troy Sobotka's modern alternative.  v1 ships the
//               scalar sigmoid-in-log form; the proper AgX with
//               primaries-aware input/output transforms is a
//               Landing-3 follow-up (depends on spectral pipeline).
//
//    Hable    — John Hable's Uncharted 2 filmic curve, normalised
//               to white-point 11.2.  Older; included for
//               compatibility with assets / look-dev tuned for it.
//
//  Negative inputs: clamped to 0 before the curve.  Pixel
//  reconstruction filters with negative side lobes (Mitchell /
//  Lanczos) can produce small negatives; ACES / Reinhard are
//  undefined for negatives.  NaN / Inf inputs collapse to 0 to
//  avoid downstream poison through to the integerisation step.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 2, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DISPLAY_TRANSFORM_
#define DISPLAY_TRANSFORM_

#include "../Utilities/Color/Color.h"
#include "../Utilities/Math3D/Math3D.h"

#include <algorithm>
#include <cmath>

namespace RISE
{
	//! Selects which display-transform curve to apply between the
	//! linear-radiance film and an LDR output writer.
	enum DISPLAY_TRANSFORM
	{
		eDisplayTransform_None     = 0,	///< Identity (legacy clip-at-1.0 behaviour)
		eDisplayTransform_Reinhard = 1,	///< x / (1 + x)
		eDisplayTransform_ACES     = 2,	///< Narkowicz ACES Filmic fit (DEFAULT)
		eDisplayTransform_AgX      = 3,	///< Sobotka scalar sigmoid-in-log
		eDisplayTransform_Hable    = 4	///< Uncharted 2 / John Hable, white-point 11.2
	};

	namespace DisplayTransforms
	{
		//! Sanitise a single channel before applying any curve.
		//! Negatives, NaN, and Inf collapse to 0; finite-positive
		//! values pass through.  Centralised so every curve gets the
		//! same gate.
		inline Scalar Sanitise( Scalar x )
		{
			// std::isfinite catches both NaN and +/-Inf; the
			// !(x > 0) clause coerces -0 / negatives to 0 without
			// branching on the sign bit.
			return ( std::isfinite( x ) && x > Scalar( 0 ) ) ? x : Scalar( 0 );
		}

		//! Identity curve.  Clamps negatives / non-finite per the
		//! shared sanitisation policy; otherwise pass-through.
		inline Scalar None( Scalar x )
		{
			return Sanitise( x );
		}

		//! Global Reinhard tone-mapper: f(x) = x / (1 + x).
		//! Asymptotic to 1; no whitepoint parameter.
		inline Scalar Reinhard( Scalar x )
		{
			const Scalar s = Sanitise( x );
			return s / ( Scalar( 1 ) + s );
		}

		//! Krzysztof Narkowicz's ACES Filmic fit.
		//! https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
		inline Scalar ACES( Scalar x )
		{
			const Scalar s = Sanitise( x );
			const Scalar a = Scalar( 2.51 );
			const Scalar b = Scalar( 0.03 );
			const Scalar c = Scalar( 2.43 );
			const Scalar d = Scalar( 0.59 );
			const Scalar e = Scalar( 0.14 );
			const Scalar num = s * ( a * s + b );
			const Scalar den = s * ( c * s + d ) + e;
			const Scalar y = num / den;
			// Saturate to [0, 1].  The fit is bounded but rounding
			// can drift fractionally outside at large s.
			return std::min( std::max( y, Scalar( 0 ) ), Scalar( 1 ) );
		}

		//! AgX scalar form: sigmoid in log10(x).  This is a placeholder
		//! suitable for stand-alone display tone-mapping; the full AgX
		//! with primaries-aware input/output transforms requires the
		//! spectral pipeline and lands in Landing 3 of the PB plan.
		//!
		//! Domain mapped to ~[10^-3, 10^3] EV-like range; sigmoid
		//! parameters chosen so f(1) ≈ 0.5 (neutral white lands at
		//! display mid-grey).  The 1e-10 epsilon padding inside the
		//! log10 is solely to avoid log10(0) = -inf when Sanitise has
		//! returned 0 — it shifts f(1) by ~5e-11 from exact 0.5,
		//! well below display precision.  Don't remove without
		//! adding an explicit x==0 -> 0 short-circuit.
		inline Scalar AgX( Scalar x )
		{
			const Scalar s = Sanitise( x );
			// Pad below the log floor so Sanitise(0)=0 doesn't blow up.
			const Scalar logx = std::log10( s + Scalar( 1e-10 ) );
			// Sigmoid: 0.5 + 0.5 * tanh( a * (logx - b) ).
			// a = 1 gives a gentle slope (~1 stop per output step);
			// b = 0 anchors x=1 at the sigmoid midpoint.
			const Scalar a = Scalar( 1.0 );
			const Scalar b = Scalar( 0.0 );
			const Scalar y = Scalar( 0.5 ) +
			                 Scalar( 0.5 ) * std::tanh( a * ( logx - b ) );
			return std::min( std::max( y, Scalar( 0 ) ), Scalar( 1 ) );
		}

		//! John Hable's Uncharted 2 filmic curve.  Pre-scales the
		//! input by 2.0 (ETM exposure bias) and normalises by the
		//! curve's value at white-point 11.2.
		inline Scalar Hable( Scalar x )
		{
			const Scalar s = Sanitise( x );
			const Scalar A = Scalar( 0.15 );
			const Scalar B = Scalar( 0.50 );
			const Scalar C = Scalar( 0.10 );
			const Scalar D = Scalar( 0.20 );
			const Scalar E = Scalar( 0.02 );
			const Scalar F = Scalar( 0.30 );
			auto curve = []( Scalar v, Scalar A_, Scalar B_, Scalar C_,
			                 Scalar D_, Scalar E_, Scalar F_ ) -> Scalar {
				return ( ( v * ( A_ * v + C_ * B_ ) + D_ * E_ ) /
				         ( v * ( A_ * v + B_      ) + D_ * F_ ) ) -
				       E_ / F_;
			};
			const Scalar exposureBiased = s * Scalar( 2.0 );
			const Scalar numerator   = curve( exposureBiased, A, B, C, D, E, F );
			const Scalar denominator = curve( Scalar( 11.2 ), A, B, C, D, E, F );
			const Scalar y = numerator / denominator;
			return std::min( std::max( y, Scalar( 0 ) ), Scalar( 1 ) );
		}

		//! Apply the selected curve to all RGB channels of a RISEPel.
		//! Caller is responsible for applying exposure (multiplication
		//! by 2^EV) BEFORE this call — exposure and curve are
		//! orthogonal stages.
		inline RISEPel Apply( DISPLAY_TRANSFORM dt, const RISEPel& linear )
		{
			RISEPel out;
			switch( dt )
			{
				case eDisplayTransform_None:
					out.r = None( linear.r );
					out.g = None( linear.g );
					out.b = None( linear.b );
					break;
				case eDisplayTransform_Reinhard:
					out.r = Reinhard( linear.r );
					out.g = Reinhard( linear.g );
					out.b = Reinhard( linear.b );
					break;
				default:
				case eDisplayTransform_ACES:
					out.r = ACES( linear.r );
					out.g = ACES( linear.g );
					out.b = ACES( linear.b );
					break;
				case eDisplayTransform_AgX:
					out.r = AgX( linear.r );
					out.g = AgX( linear.g );
					out.b = AgX( linear.b );
					break;
				case eDisplayTransform_Hable:
					out.r = Hable( linear.r );
					out.g = Hable( linear.g );
					out.b = Hable( linear.b );
					break;
			}
			return out;
		}
	}
}

#endif
