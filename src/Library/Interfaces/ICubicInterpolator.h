//////////////////////////////////////////////////////////////////////
//
//  ICubicInterpolator.h - Defines an interfaces for a cubic inter-
//    polator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 1, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ICUBIC_INTERPOLATOR_
#define ICUBIC_INTERPOLATOR_

#include "IFullInterpolator.h"

namespace RISE
{
	//! A cubic interpolator takes four control points and interpolates
	//! a value between the two middle points
	/// \sa IFullInterpolator
	template< class T >
	class ICubicInterpolator : public virtual IFullInterpolator<T>
	{
	protected:
		ICubicInterpolator(){};
		virtual ~ICubicInterpolator(){};

	public:

		//! Interpolates between two values, given all the possible values
		/// \return The interpolated value
		virtual T InterpolateValues( 
			const T& y0,									///< [in] The control point before the one we are interested in
			const T& y1,									///< [in] The first control point we are interested in
			const T& y2,									///< [in] The second control point we are interested in
			const T& y3,									///< [in] The control point after the one we are interested in
			const Scalar x									///< [in] Specifies the interpolate amount, scalar from [0..1]
			) const = 0;
	};
}

#endif
