//////////////////////////////////////////////////////////////////////
//
//  SellmeierScalarPainter.h - Analytic Sellmeier IOR formula
//
//    n²(λ) = 1 + Σᵢ (Bᵢ · λ²) / (λ² - Cᵢ),  λ in micrometres
//
//  Three-term form is the textbook standard (BK7, fused silica, etc.).
//  Schott / SCHOTT, Refractiveindex.info, and most optical-glass
//  catalogs publish coefficients in this form.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SELLMEIER_SCALAR_PAINTER_
#define SELLMEIER_SCALAR_PAINTER_

#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"
#include <cmath>

namespace RISE
{
	namespace Implementation
	{
		class SellmeierScalarPainter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		protected:
			const Scalar B1, B2, B3;
			const Scalar C1, C2, C3;
			virtual ~SellmeierScalarPainter() {}

			static constexpr Scalar kRepresentativeNm = Scalar( 587.6 );	// d-line, glass-spec convention

			//! Guard distance: skip the singularity at λ² == Cᵢ.
			//! For standard optical-glass coefficients (Cᵢ in [0.005,
			//! 105] µm²), even UV wavelengths down to 200 nm
			//! (λ² = 0.04 µm²) are far from C₁ ≈ 0.006 µm² by enough
			//! that this guard never fires.  It's defense-in-depth
			//! for hand-authored Sellmeier coefficients.
			static constexpr Scalar kSingularityEps = Scalar( 1e-9 );

			static Scalar SafeTerm( Scalar lam2, Scalar B, Scalar C )
			{
				const Scalar denom = lam2 - C;
				if( std::fabs( denom ) < kSingularityEps ) return Scalar( 0 );
				return ( B * lam2 ) / denom;
			}

			Scalar EvalAtNM( Scalar nm ) const
			{
				// Sellmeier wants λ in micrometres; nm is nanometres.
				const Scalar lam_um = nm * Scalar( 1e-3 );
				const Scalar lam2 = lam_um * lam_um;
				const Scalar t1 = SafeTerm( lam2, B1, C1 );
				const Scalar t2 = SafeTerm( lam2, B2, C2 );
				const Scalar t3 = SafeTerm( lam2, B3, C3 );
				const Scalar n2 = Scalar( 1.0 ) + t1 + t2 + t3;
				return n2 > 0 ? std::sqrt( n2 ) : Scalar( 1.0 );
			}

		public:
			SellmeierScalarPainter(
				Scalar B1_, Scalar B2_, Scalar B3_,
				Scalar C1_, Scalar C2_, Scalar C3_
				) :
				B1( B1_ ), B2( B2_ ), B3( B3_ ),
				C1( C1_ ), C2( C2_ ), C3( C3_ )
			{}

			ScalarTriple GetValuesAt(
				const RayIntersectionGeometric& /*ri*/
				) const override
			{
				return ScalarTriple( EvalAtNM( kRepresentativeNm ) );
			}

			Scalar GetValueAtNM(
				const RayIntersectionGeometric& /*ri*/,
				Scalar nm
				) const override
			{
				return EvalAtNM( nm );
			}

			bool HasPerChannelVariation() const override { return false; }
		};
	}
}

#endif
