//////////////////////////////////////////////////////////////////////
//
//  RGBScalarPainter.h - A scalar painter that holds three per-channel
//    scalar values (R, G, B).  Used for inline-triple material
//    parameters like `ior 1.3 1.5 2.0` where the author wants
//    RGB-channel dispersion in the RGB rendering path.
//
//  For the spectral path, the three values map to three nominal
//  wavelengths (R = 650 nm, G = 550 nm, B = 450 nm) and the
//  spectral value at an arbitrary wavelength is piecewise-linearly
//  interpolated between them.  Outside the [450, 650] nm range we
//  extrapolate via clamp-to-endpoint — the user authoring three
//  RGB-channel values isn't supplying a meaningful spectrum at the
//  edges of the visible band, and clamping is the least-surprising
//  behavior.
//
//  Authors who need a real spectrum should use
//  `PiecewiseLinearScalarPainter` or `Function1DScalarPainter`.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RGB_SCALAR_PAINTER_
#define RGB_SCALAR_PAINTER_

#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class RGBScalarPainter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		protected:
			const Scalar r;
			const Scalar g;
			const Scalar b;
			virtual ~RGBScalarPainter() {}

			//! Nominal wavelengths for the R, G, B channels (CIE-like).
			//! Used for piecewise-linear spectral interpolation.
			static constexpr Scalar kNmR = Scalar( 650.0 );
			static constexpr Scalar kNmG = Scalar( 550.0 );
			static constexpr Scalar kNmB = Scalar( 450.0 );

		public:
			RGBScalarPainter( Scalar r_, Scalar g_, Scalar b_ )
				: r( r_ ), g( g_ ), b( b_ )
			{}

			ScalarTriple GetValuesAt(
				const RayIntersectionGeometric& /*ri*/
				) const override
			{
				return ScalarTriple( r, g, b );
			}

			Scalar GetValueAtNM(
				const RayIntersectionGeometric& /*ri*/,
				Scalar nm
				) const override
			{
				// Clamp outside the nominal RGB band.
				if( nm <= kNmB ) return b;
				if( nm >= kNmR ) return r;
				// Linear interpolation between (450, B) → (550, G) → (650, R).
				if( nm <= kNmG ) {
					const Scalar t = ( nm - kNmB ) / ( kNmG - kNmB );
					return b + t * ( g - b );
				} else {
					const Scalar t = ( nm - kNmG ) / ( kNmR - kNmG );
					return g + t * ( r - g );
				}
			}

			bool HasPerChannelVariation() const override
			{
				return ! ( r == g && g == b );
			}
		};
	}
}

#endif
