//////////////////////////////////////////////////////////////////////
//
//  IFullInterpolator.h - Defines an interfaces for a full interpolator
//  which takes an entire set of values and the value to interpolate
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 11, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IFULL_INTERPOLATOR_
#define IFULL_INTERPOLATOR_

#include "IReference.h"
#include <vector>

namespace RISE
{
	//! A full interpolator takes all the values and then computes the interpolated
	//! value between two points.  It can use any of the values in the series to compute
	//! this interpolated value.
	/// \sa ISimpleInterpolator
	template< class T >
	class IFullInterpolator : public virtual IReference
	{
	protected:
		IFullInterpolator(){};
		virtual ~IFullInterpolator(){};

	public:
		//! Interpolates between two values, given all the possible values
		/// \return The interpolated value
		virtual T Interpolate2Values( 
			typename std::vector<T>& values,						///< [in] A list of values in a container to be interpolated
			typename std::vector<T>::const_iterator& first,			///< [in] Iterator at the value to interpolate from
			typename std::vector<T>::const_iterator& second,		///< [in] Iterator at the value to interpolate to
			const Scalar x											///< [in] Specifies the interpolate amount, scalar from [0..1]
			) const = 0;
	};

	typedef IFullInterpolator<Scalar>		RealFullInterpolator;
}

#endif
