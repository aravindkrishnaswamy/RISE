//////////////////////////////////////////////////////////////////////
//
//  IRayIntersectionModifier.h - An interface to an object
//  that is capable of taking a ray intersection and screwing
//  around with it
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 17, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IRAYINTERSECTION_MODIFIER_
#define IRAYINTERSECTION_MODIFIER_

#include "IReference.h"
#include "../Intersection/RayIntersectionGeometric.h"

namespace RISE
{
	//! Has the ability to modify intersection details
	class IRayIntersectionModifier : public virtual IReference
	{
	protected:
		IRayIntersectionModifier(){};
		virtual ~IRayIntersectionModifier(){};

	public:
		//! Modifies a ray intersection
		virtual void Modify( 
			RayIntersectionGeometric& ri				///< [in/out] The geometric intersection information to modify
			) const = 0;
	};
}

#endif
