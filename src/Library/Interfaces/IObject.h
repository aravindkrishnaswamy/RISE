//////////////////////////////////////////////////////////////////////
//
//  IObject.h - Interface to a rasterizable object in our scene
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IOBJECT_
#define IOBJECT_

#include "IReference.h"
#include "ITransformable.h"
#include "../Utilities/BoundingBox.h"

namespace RISE
{
	class RayIntersection;
	class IMaterial;
	class Ray;

	//! An object combines both geometric and material information into a 
	//! meaningful scene element
	/// \sa IGeometry
	/// \sa IMaterial
	class IObject : public virtual IReference, public virtual IBasicTransform
	{
	protected:
		IObject(){};
		virtual ~IObject(){};

	public:
		//! Intersects a ray with the object
		virtual void IntersectRay( 
			RayIntersection& ri,						///< [in/out] Intersection details at point of intersection if there is an intersection
			const Scalar dHowFar,						///< [in] Maximum distance to travel along ray, optimization
			const bool bHitFrontFaces,					///< [in] Should front facing hits be processed?
			const bool bHitBackFaces,					///< [in] Should back facing hits be processed?
			const bool bComputeExitInfo					///< [in] Should exit information be computed (the ray continues until exiting the object) in addition of initial intersection information?
			) const = 0;

		//! Intersects, but performs intersection test only with object
		/// \return TRUE if there is an intersection, FALSE otherwise
		virtual bool IntersectRay_IntersectionOnly( 
			const Ray& ray,								///< [in] The ray to process the intersection from
			const Scalar dHowFar,						///< [in] Maximum distance to travel along that ray (optimization parameter)
			const bool bHitFrontFaces,					///< [in] Should we process the intersection if the element is front facing?
			const bool bHitBackFaces					///< [in] Should we process the intersection if the element is back facing?
			) const = 0;

		//! Is this object visible to the world?
		/// \return TRUE if the object is visible to the world, FALSE otherwise
		virtual bool IsWorldVisible() const = 0;

		//! Does this object cast shadows?
		/// \return TRUE if the object casts shadows, FALSE otherwise
		virtual bool DoesCastShadows() const = 0;

		//! Does this object receive shadows?
		/// \return TRUE if the object receive shadows, FALSE otherwise
		virtual bool DoesReceiveShadows() const = 0;

		//! Reteives the material associated to this object
		virtual const IMaterial* GetMaterial() const = 0;
		
		//! Generates a uniform random point on the object.  Needed to sample luminary geometry surfaces
		//! This function guarantees that for the same prand, the same data is returned
		virtual void UniformRandomPoint( 
			Point3* point,								///< [out] Point on the surface
			Vector3* normal,							///< [out] Normal at the point on the surface
			Point2* coord,								///< [out] Texture co-ordinate at the point on the surface
			const Point3& prand						///< [in] Variables used in point generation
			) const = 0;

		//! Returns the area of the object
		/// \return The area of the object in meters squared
		virtual Scalar GetArea( ) const = 0;

		//! Returns the bounding box of the object
		/// \return The bounding box of the object
		virtual const BoundingBox getBoundingBox() const = 0;
	};
}

#include "../Intersection/RayIntersection.h"
#include "IMaterial.h"

#endif
