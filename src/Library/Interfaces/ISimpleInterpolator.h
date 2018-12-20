//////////////////////////////////////////////////////////////////////
//
//  ISimpleInterpolator.h - Defines an interfaces for a simple
//  interpolator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 11, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISIMPLE_INTERPOLATOR_
#define ISIMPLE_INTERPOLATOR_

#include "IReference.h"

namespace RISE
{
	//! A simple interpolator requires only the two values to interpolate between
	//! in order to compute the interpolated value.
	/// \sa IFullInterpolator
	template< class T >
	class ISimpleInterpolator : public virtual IReference
	{
	protected:
		ISimpleInterpolator(){};
		virtual ~ISimpleInterpolator(){};

	public:
		//! Interpolates the two values without known what the other values are
		/// \return The interpolated value
		virtual T InterpolateValues( 
			const T a,						///< [in] First value 
			const T b,						///< [in] Second value
			const Scalar x					///< [in] Interpolation factor
			) const = 0;
	};

	typedef ISimpleInterpolator<Scalar>		RealSimpleInterpolator;
}

#endif
