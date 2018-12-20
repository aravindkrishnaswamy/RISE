//////////////////////////////////////////////////////////////////////
//
//  HermiteInterpolator.h - Defines a hermite interpolator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 24, 2003
//  Tabs: 4
//  Comments: From http://astronomy.swin.edu.au/~pbourke/analysis/interpolation/
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HERMITE_INTERPOLATOR_
#define HERMITE_INTERPOLATOR_

#include "../Interfaces/IFullInterpolator.h"
#include "Reference.h"

namespace RISE
{
	template< class T >
	class HermiteInterpolator : 
		public IFullInterpolator<T>,
		public virtual Implementation::Reference
	{
	protected:
		virtual ~HermiteInterpolator(){};

		const Scalar	tension;		///< 1 is high, 0 normal, -1 is low
		const Scalar	bias;			///< 0 is even, positive is towards first segment, negative towards second segment
		const Scalar	continuity;		///< 

	public:
		HermiteInterpolator( 
			const Scalar tension_,
			const Scalar bias_,
			const Scalar continuity_
			) : 
		tension( tension_ ),
		bias( bias_ ),
		continuity( continuity_ )
		{
		}

		T	Interpolate2Values( 
			typename std::vector<T>& values,
			typename std::vector<T>::const_iterator& first,
			typename std::vector<T>::const_iterator& second, 
			const Scalar mu ) const
		{
			// y0 = the point before a
			// y1 = the point a
			// y2 = the point b
			// y3 = the after b
			const T& y1 = *first;
			const T& y2 = *second;

			const T& y0 = first > values.begin() ? *(first-1) : y1;
			const T& y3 = second < (values.end()-1) ? *(second+1) : y2;

			const Scalar mu2 = mu * mu;
			const Scalar mu3 = mu2 * mu;
			const T m0 = (y1-y0)*((1-continuity)*(1+bias)*(1-tension)*0.5) +
						((y2-y1)*((1+continuity)*(1-bias)*(1-tension)*0.5));
			const T m1 = (y2-y1)*((1+continuity)*(1+bias)*(1-tension)*0.5) + 
						((y3-y2)*((1-continuity)*(1-bias)*(1-tension)*0.5));

			const Scalar a0 =  2*mu3 - 3*mu2 + 1;
			const Scalar a1 =    mu3 - 2*mu2 + mu;
			const Scalar a2 =    mu3 -   mu2;
			const Scalar a3 = -2*mu3 + 3*mu2;

			return(a0*y1+a1*m0+a2*m1+a3*y2);

		}
	};

	typedef HermiteInterpolator<Scalar>		RealHermiteInterpolator;
}

#endif

