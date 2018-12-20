//////////////////////////////////////////////////////////////////////
//
//  ParametricSurface.h - Helper function for parametric surfaces
//    this helper class implements the range functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 26, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PARAMETRIC_SURFACE_
#define PARAMETRIC_SURFACE_

#include "../Interfaces/IParametricSurface.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class ParametricSurface : public virtual IParametricSurface, public virtual Reference
		{
		protected:
			Scalar u_start;
			Scalar u_end;
			Scalar v_start;
			Scalar v_end;

			ParametricSurface();
			virtual ~ParametricSurface();

		public:
			//! Sets the evaluation range of u and v
			virtual void SetRange(
				const Scalar u_start,					///< [in] Where u starts
				const Scalar u_end,						///< [in] Where u ends
				const Scalar v_start,					///< [in] Where v starts
				const Scalar v_end						///< [in] Where v ends
				);

			//! Returns the evaluation range of u and v
			virtual void GetRange(
				Scalar& u_start,						///< [in] Where u starts
				Scalar& u_end,							///< [in] Where u ends
				Scalar& v_start,						///< [in] Where v starts
				Scalar& v_end							///< [in] Where v ends
				);
		};
	};
}

#endif