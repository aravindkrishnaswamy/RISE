//////////////////////////////////////////////////////////////////////
//
//  DiffusionApproximationExtinction.h - Simple extinction function for SSS
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2005
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DIFFUSION_APPROXIMATION_EXTINCTION_H
#define DIFFUSION_APPROXIMATION_EXTINCTION_H

#include "../../Interfaces/ISubSurfaceExtinctionFunction.h"
#include "../../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class DiffusionApproximationExtinction :
			public virtual ISubSurfaceExtinctionFunction,
			public virtual Reference
		{
		protected:
			const RISEPel absorption;
			const RISEPel scattering;
			const Scalar relative_ior;
			const Scalar g;
			const Scalar geometric_scale;

			// Precomputed values
			RISEPel s_prime;
			RISEPel t_prime;
			RISEPel alpha_prime;

			RISEPel zr;
			RISEPel zv;

			RISEPel tr;

			RISEPel ap_ov_four_pi;

		public:
			DiffusionApproximationExtinction(
				const RISEPel absorption_,
				const RISEPel scattering_,
				const Scalar relative_ior_,
				const Scalar g_,
				const Scalar geometric_scale_
				) :
				absorption( absorption_ ),
				scattering( scattering_ ),
				relative_ior( relative_ior_ ),
				g( g_ ),
				geometric_scale( geometric_scale_ )
			{
				s_prime = scattering * (1.0-g);
				t_prime = s_prime + absorption;
				alpha_prime = s_prime / t_prime;

				const RISEPel lu = RISEPel(1,1,1)/alpha_prime;
				const Scalar nu = relative_ior;
				const Scalar fresnel = -1.440/(nu*nu) + 0.710/nu + 0.668 + 0.0636*nu;
				const Scalar A = (1.0+fresnel)/(1.0-fresnel);
			//	const RISEPel D = RISEPel(1,1,1) / (3.0*t_prime);

//				zv = lu;
//				zr = zv + 4.0*A*D;
				zr = lu;
				zv = zr + 4.0*A;

				tr = ColorMath::root(2.0*absorption*t_prime);

				ap_ov_four_pi = alpha_prime / RISEPel(FOUR_PI,FOUR_PI,FOUR_PI);
			}

			virtual ~DiffusionApproximationExtinction()
			{
			}

			virtual Scalar GetMaximumDistanceForError(
				const Scalar error
			) const
			{
				if( error > 0 ) {
					return -log(error) / (ColorMath::MinValue(alpha_prime)*geometric_scale) / 1000.0;
				}

				return INFINITY;
			}

			RISEPel ComputeTotalExtinction(
				const Scalar distance
			) const
			{
				const Scalar r = distance * 1000.0 * geometric_scale;	// Convert the distance from meters to mm
				const Scalar r2 = r*r;
				const RISEPel sqr_r( r2, r2, r2 );

				const RISEPel one(1,1,1);
				const RISEPel negative(-1,-1,-1);

				RISEPel dv = ColorMath::root( sqr_r + (zv*zv) );
				RISEPel dr = ColorMath::root( sqr_r + (zr*zr) );

				const RISEPel inside_left = zr*(tr*dr+one) * ( ColorMath::exponential(negative*tr*dr)/(dr*dr*dr) );
				const RISEPel inside_right = zv*(tr*dv+one) * ( ColorMath::exponential(negative*tr*dv)/(dv*dv*dv) );

				return ap_ov_four_pi * (inside_left+inside_right);
			}
		};
	}
}

#endif
