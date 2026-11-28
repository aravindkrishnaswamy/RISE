//////////////////////////////////////////////////////////////////////
//
//  SimpleInterpolators.h - Defines some simple interpolators, like
//  linear, cosine...
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 11, 2002
//  Tabs: 4
//  Comments: Reference-counted ISimpleInterpolator/IFullInterpolator
//    implementations. Parameter `x` is expected in [0, 1] (unclamped:
//    0 -> a, 1 -> b). Interpolate2Values() ignores the full `values`
//    vector because linear/cosine interpolation needs only *first and *second.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SIMPLE_INTERPOLATORS_
#define SIMPLE_INTERPOLATORS_

#include "../Interfaces/ISimpleInterpolator.h"
#include "../Interfaces/IFullInterpolator.h"
#include "Math3D/Constants.h"
#include "Reference.h"
#include <math.h>

namespace RISE
{
	template< class T >
	class LinearInterpolator : 
		public virtual ISimpleInterpolator<T>,
		public virtual IFullInterpolator<T>,
		public virtual Implementation::Reference
	{
	protected:
		virtual ~LinearInterpolator(){};

	public:
		T	InterpolateValues( const T a, const T b, const Scalar x ) const
		{
			return (a * (1.0-x) + b * x);
		}

		T	Interpolate2Values( 
			const typename std::vector<T>& values, 
			typename std::vector<T>::const_iterator& first, 
			typename std::vector<T>::const_iterator& second, 
			const Scalar x 
			) const
		{
			const		T& a = *first;
			const		T& b = *second;

			return (a * (1.0-x) + b * x);
		}
	};

	// C1 half-cosine easing: f(x) = (1 - cos(x * PI)) / 2, zero slope at x = 0, 1.
	template< class T >
	class CosineInterpolator : 
		public virtual ISimpleInterpolator<T>, 
		public virtual IFullInterpolator<T>,
		public virtual Implementation::Reference
	{
	protected:
		virtual ~CosineInterpolator(){};

	public:
		T	InterpolateValues( const T a, const T b, const Scalar x ) const
		{
			const Scalar		ft = x * PI;
			const Scalar		f = (1.0 - cos(ft)) * 0.5;
			return (a*(1.0-f) + b*f);
		}

		T	Interpolate2Values( 
			const typename std::vector<T>& values, 
			typename std::vector<T>::const_iterator& first, 
			typename std::vector<T>::const_iterator& second, 
			const Scalar x 
			) const
		{
			const		T& a = *first;
			const		T& b = *second;

			const Scalar		ft = x * PI;
			const Scalar		f = (1.0 - cos(ft)) * 0.5;
			return (a*(1.0-f) + b*f);
		}
	};

	typedef LinearInterpolator<Scalar>	RealLinearInterpolator;
	typedef CosineInterpolator<Scalar>	RealCosineInterpolator;
}

#endif
