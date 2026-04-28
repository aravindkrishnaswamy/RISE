//////////////////////////////////////////////////////////////////////
//
//  TreeElementProcesssor.h - Defines an interface for a set of 
//    functions for processing individual nodes in BSPTrees and Octrees
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 20, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TREE_ELEMENT_PROCESSOR_
#define TREE_ELEMENT_PROCESSOR_

#include "Interfaces/IReference.h"
#include "Intersection/RayIntersection.h"
#include "Utilities/Plane.h"

namespace RISE
{
	template< class T >
	struct TreeElementProcessor : 
		public virtual IReference
	{
		// These five functions need to specialized for EVERY type we want to use
		// Octrees or BSP trees with
		
			virtual void RayElementIntersection( RayIntersectionGeometric& ri, const T elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const = 0;
			virtual void RayElementIntersection( RayIntersection& ri, const T elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const = 0;
			virtual bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const T elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const = 0;
			virtual BoundingBox GetElementBoundingBox( const T elem ) const = 0;
			virtual bool ElementBoxIntersection( const T elem, const BoundingBox& bbox ) const = 0;
			virtual char WhichSideofPlaneIsElement( const T elem, const Plane& plane ) const = 0;

		virtual void SerializeElement( IWriteBuffer& buffer, const T elem ) const = 0;
		virtual void DeserializeElement( IReadBuffer& buffer, T& ret ) const = 0;

		//! Phase 2 (BVH float filter): if this element type is a triangle
		//! whose three vertices can be supplied in float, fill the three
		//! 3-float arrays and return true.  BVH uses this to precompute
		//! Möller-Trumbore edges per leaf primitive and run a fast float
		//! intersection filter before the expensive double-precision
		//! RayElementIntersection.  Default returns false → BVH skips
		//! the filter for this primitive type and routes every leaf hit
		//! candidate straight to the full intersection path.
		//! Non-pure so existing TreeElementProcessor subclasses (Octree,
		//! BSP, BSSRDF photon-store, etc.) remain source-compatible.
		virtual bool GetFloatTriangleVertices(
			const T /*elem*/,
			float /*v0*/[3], float /*v1*/[3], float /*v2*/[3] ) const
		{
			return false;
		}
	};
}

#endif
