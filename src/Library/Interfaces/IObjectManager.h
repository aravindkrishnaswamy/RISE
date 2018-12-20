//////////////////////////////////////////////////////////////////////
//
//  IObjectManager.h - Interface to the object manager, which manages
//    all objects in a scene
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IOBJECTMANAGER_
#define IOBJECTMANAGER_

#include "IManager.h"
#include "IEnumCallback.h"
#include "../Intersection/RayIntersection.h"

namespace RISE
{
	class IObjectPriv;

	class IObjectManager : public virtual IManager<IObjectPriv>
	{
	protected:
		IObjectManager(){};
		virtual ~IObjectManager(){};

	public:
		//
		// Object specific
		//

		//! Intersects a ray with the object
		virtual void IntersectRay( 
			RayIntersection& ri,						///< [in/out] Intersection details at point of intersection if there is an intersection
			const bool bHitFrontFaces,					///< [in] Should front facing hits be processed?
			const bool bHitBackFaces,					///< [in] Should back facing hits be processed?
			const bool bComputeExitInfo					///< [in] Should exit information be computed (the ray continues until exiting the object) in addition of initial intersection information?
			) const = 0;

		//! Intersects, but performs intersection test only with object
		virtual bool IntersectShadowRay( 
			const Ray& ray,								///< [in] The ray to process the intersection from
			const Scalar dHowFar,						///< [in] Maximum distance to travel along that ray (optimization parameter)
			const bool bHitFrontFaces,					///< [in] Should we process the intersection if the element is front facing?
			const bool bHitBackFaces					///< [in] Should we process the intersection if the element is back facing?
			) const = 0;

		//! Enumerates the objects with normal interfaces
		virtual void EnumerateObjects( 
			IEnumCallback<IObject>& pFunc				///< [in] Callback functor interface that accepts normal IObject interface
			) const = 0;

		//! Enumerates the objects with their priviledged interfaces
		virtual void EnumerateObjects( 
			IEnumCallback<IObjectPriv>& pFunc			///< [in] Callback functor interface that accepts the IObjectPriv interface
			) const = 0;

		//! Tells all the objects to reset any runtime data
		virtual void ResetRuntimeData() const = 0;
	};
}

#include "IObjectPriv.h"

#endif
