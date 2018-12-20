//////////////////////////////////////////////////////////////////////
//
//  IGeometry.h - Geometry interface
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IGEOMETRY_
#define IGEOMETRY_

#include "IReference.h"
#include "IKeyframable.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Utilities/BoundingBox.h"

namespace RISE
{
	//! Geometry represents the basic geometry of a scene object
	//! It needs only to provide basic geometric intersection details
	class IGeometry : 
		public virtual IReference,
		public virtual IKeyframable
	{
	protected:
		IGeometry(){};
		virtual ~IGeometry(){};

	public:
		//! Generates a mesh and stores it in the local class 
		//! variables, all sub classes better implement this function
		virtual void GenerateMesh() = 0;
		
		//! This the most important function
		//! It asks the geometric object to intersect itself
		//! and return intersection details
		//
		//! If a sub class doesn't override this method, then
		//! it will be called here and we'll handle it
		virtual void IntersectRay( 
			RayIntersectionGeometric& ri,				///< [in/out] Receives the geometric intersection information
			const bool bHitFrontFaces,					///< [in] Should we process the intersection if the element is front facing?
			const bool bHitBackFaces,					///< [in] Should we process the intersection if the element is back facing?
			const bool bComputeExitInfo					///< [in] Should exit information be computed (the ray continues until exiting the object) in addition of initial intersection information?
			) const = 0;

		//! This function is here to help the shadow checks
		//! It asks the geometry object if the given ray
		//! will intersect the object, we don't care about
		//! where or the normal or any of that junk
		//!
		//! Similar to IntersectRay, if the sub class doesn't
		//! override this method, then it will be called here
		//! and we'll handle it
		/// \return TRUE if there is an intersection, FALSE otherwise
		virtual bool IntersectRay_IntersectionOnly(
			const Ray& ray,								///< [in] The ray to process the intersection from
			const Scalar dHowFar,						///< [in] Maximum distance to travel along that ray (optimization parameter)
			const bool bHitFrontFaces,					///< [in] Should we process the intersection if the element is front facing?
			const bool bHitBackFaces					///< [in] Should we process the intersection if the element is back facing?
			) const = 0;

		//! Generates a sphere that envelopes all the geometry
		virtual void GenerateBoundingSphere(
			Point3& ptCenter,							///< [out] Center of the bounding sphere
			Scalar& radius								///< [out] Radius of the bounding sphere
			) const = 0;

		//! Generates a box that envelopes all the geometry
		virtual BoundingBox GenerateBoundingBox(
			) const = 0;

		//! Should bounding spheres or boxes be tested against before doing a full out intersection test?
		/// \return TRUE if bounding boxes/sphere should be checked, FALSE otherwise
		virtual bool DoPreHitTest() const = 0; 

		//! Returns a uniformly random point on the surface.  Needed to sample luminary geometry surfaces
		//! This function guarantees that for the same prand, the same data is returned
		virtual void UniformRandomPoint(
			Point3* point,								///< [out] Point on the surface
			Vector3* normal,							///< [out] Normal at the point on the surface
			Point2* coord,								///< [out] Texture co-ordinate at the point on the surface
			const Point3& prand						///< [in] Variables used in point generation
			) const = 0;

		//! Gets the area of the geometry.  Needed for luminaries.
		/// \return Area of the geometry in Meters Squared
		virtual Scalar GetArea( ) const = 0;
	};
}


#endif
