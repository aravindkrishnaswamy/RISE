//////////////////////////////////////////////////////////////////////
//
//  PolynomialFunction2DPainter.h - A 2D Function2D painter that
//  evaluates a polynomial in normalised coordinates.
//
//  The painter operates on (x, y) = ((u − center.x) / scale.x,
//  (v − center.y) / scale.y), so for the canonical sphere UV domain
//  [0,1]² the defaults (center = 0.5,0.5  scale = 0.5,0.5) give
//  (x, y) ∈ [-1, 1]² — the natural domain for polynomials in unit
//  form.
//
//  The `type` enum selects a polynomial family:
//
//    radial_bump        f = amplitude × max(0, 1 − r²)^degree
//                       (r = sqrt(x² + y²))
//                       Compact support outside r=1.  `degree`
//                       controls boundary smoothness:
//                         1 → C⁰ kink (parabolic dome)
//                         2 → C¹ (quartic bump)
//                         3 → C² (sextic bump)
//                         …
//
//    monomial           f = amplitude × x^power_x × y^power_y
//
//    paraboloid         f = amplitude × (x² + y²)
//
//    hyperbolic_saddle  f = amplitude × (x² − y²)
//
//    monkey_saddle      f = amplitude × (x³ − 3·x·y²)
//
//    bivariate          f = amplitude × Σ a_{ij} x^i y^j
//                       for i + j ≤ degree.  Coefficients are passed
//                       as an array ordered by total-degree then by
//                       descending power of x:
//                         a₀₀,
//                         a₁₀, a₀₁,
//                         a₂₀, a₁₁, a₀₂,
//                         a₃₀, a₂₁, a₁₂, a₀₃,
//                         …
//                       (= (degree+1)·(degree+2)/2 coefficients in
//                       total).  Missing coefficients are treated as
//                       zero, so under-specification is allowed.
//
//  Implements both `IPainter` (interpolates colora→colorb by the
//  normalised polynomial value) and `IFunction2D` (the per-vertex
//  hook DisplacedGeometry calls).
//
//  Author: RISE contributors
//  Date of Birth: 2026-05-12
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef POLYNOMIAL_FUNCTION2D_PAINTER_
#define POLYNOMIAL_FUNCTION2D_PAINTER_

#include "Painter.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class PolynomialFunction2DPainter : public Painter
		{
		public:
			enum PolynomialType
			{
				eRadialBump        = 0,	///< amplitude × max(0, 1 − r²)^degree
				eMonomial          = 1,	///< amplitude × x^power_x × y^power_y
				eParaboloid        = 2,	///< amplitude × (x² + y²)
				eHyperbolicSaddle  = 3,	///< amplitude × (x² − y²)
				eMonkeySaddle      = 4,	///< amplitude × (x³ − 3·x·y²)
				eBivariate         = 5	///< amplitude × Σ a_{ij} x^i y^j
			};

		protected:
			virtual ~PolynomialFunction2DPainter();

			const IPainter&				a;
			const IPainter&				b;

			PolynomialType				type;
			Scalar						centerU;
			Scalar						centerV;
			Scalar						scaleU;	///< divisor; clamped away from zero in ctor
			Scalar						scaleV;	///< divisor; clamped away from zero in ctor
			Scalar						amplitude;
			unsigned int				degree;	///< used by radial_bump and bivariate
			unsigned int				powerX;	///< used by monomial
			unsigned int				powerY;	///< used by monomial
			std::vector<Scalar>			bivariateCoeffs;	///< used by bivariate; row-major (see header comment)

		public:
			//! Construct.  `pCoeffs` and `nCoeffs` are only consulted
			//! when `type == eBivariate`; pass nullptr / 0 otherwise.
			//! For bivariate, missing coefficients beyond `nCoeffs`
			//! are treated as zero.
			PolynomialFunction2DPainter(
				const IPainter&			cA_,
				const IPainter&			cB_,
				const PolynomialType	type_,
				const Scalar			centerU_,
				const Scalar			centerV_,
				const Scalar			scaleU_,
				const Scalar			scaleV_,
				const Scalar			amplitude_,
				const unsigned int		degree_,
				const unsigned int		powerX_,
				const unsigned int		powerY_,
				const Scalar*			pCoeffs,
				const unsigned int		nCoeffs );

			PolynomialFunction2DPainter( const PolynomialFunction2DPainter& ) = delete;
			PolynomialFunction2DPainter& operator=( const PolynomialFunction2DPainter& ) = delete;

			RISEPel	GetColor( const RayIntersectionGeometric& ri ) const;
			Scalar	GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			//! IFunction2D — `displaced_geometry` calls this per mesh vertex.
			Scalar	Evaluate( const Scalar x, const Scalar y ) const;

			//! Keyframable interface (no animated parameters in v1).
			IKeyframeParameter*	KeyframeFromParameters( const String& name, const String& value );
			void				SetIntermediateValue( const IKeyframeParameter& val );
			void				RegenerateData();
		};
	}
}

#endif
