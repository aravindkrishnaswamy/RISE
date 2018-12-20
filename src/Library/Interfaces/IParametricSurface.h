//////////////////////////////////////////////////////////////////////
//
//  IParametricSurface.h - Interface to a parametric surface
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 26, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IPARAMETRIC_SURFACE_
#define IPARAMETRIC_SURFACE_

#include "IReference.h"
#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	//! A parametric surface 
	class IParametricSurface : public virtual IReference
	{
	protected:
		IParametricSurface(){};
		virtual ~IParametricSurface(){};

	public:
		//! Evalutes the surface at the given co-ordinates, returning a point on the surface
		/// \return TRUE if the given value evaluates to something on the surface, FALSE otherwise
		virtual bool Evaluate( 
			Point3& ret,							///< [out] The point on the surface
			const Scalar u,							///< [in] First co-ordinate
			const Scalar v							///< [in] Second co-ordinate
			) = 0;

		//! Sets the evaluation range of u and v
		virtual void SetRange(
			const Scalar u_start,					///< [in] Where u starts
			const Scalar u_end,						///< [in] Where u ends
			const Scalar v_start,					///< [in] Where v starts
			const Scalar v_end						///< [in] Where v ends
			) = 0;

		//! Returns the evaluation range of u and v
		virtual void GetRange(
			Scalar& u_start,						///< [in] Where u starts
			Scalar& u_end,							///< [in] Where u ends
			Scalar& v_start,						///< [in] Where v starts
			Scalar& v_end							///< [in] Where v ends
			) = 0;
	};
}

#endif