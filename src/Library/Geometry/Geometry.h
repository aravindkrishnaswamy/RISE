//////////////////////////////////////////////////////////////////////
//
//  Geometry.h - Declaration of the geometry class which is what
//  things that want to be classified as geometry must extend.
//  Some functions are pure virtual, others are only virtual
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 21, 2001
//  Tabs: 4
//  Comments:  All geometry objects *MUST* be representable as 
//			   triangular meshes, see meshes for mesh properties
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef GEOMETRY_
#define GEOMETRY_

#include "../Interfaces/IGeometry.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class Geometry : public virtual IGeometry, public virtual Reference
		{
		protected:
			// Can this geometry object only be represented as a 
			// mesh ?
			bool		bOnlyAsMesh;

		//	std::vector<Triangle>		triangles;

			Geometry();
			virtual ~Geometry();

			//! Shared helper for `IGeometry::SelfHitRootFloor` on geometries
			//! that hold a COLLECTION of primitives (triangle meshes, patch
			//! trees) rather than one analytic surface.
			//!
			//! Their per-primitive gates -- RayTriangleIntersection's
			//! `NEARZERO * (1 + |origin|_1 + max vertex |.|_1)`,
			//! RayBilinearPatchIntersection's `NEARZERO * (1 + max(|origin|_1,
			//! max corner |.|_1))` -- depend on WHICH primitive is being hit,
			//! and the query names only a point.  Bound them all at once with
			//! the collection's bounding box: L1 is convex, so its maximum
			//! over a box is attained at a box CORNER and therefore dominates
			//! every contained vertex or control point.  Conservative -- never
			//! UNDER-states a primitive's own gate, which is the direction
			//! `SelfHitRootFloor`'s contract requires.
			static Scalar BoundingBoxRootFloor( const BoundingBox& bb, const Point3& localOrigin )
			{
				Scalar coordScale =
					std::fabs( localOrigin.x ) + std::fabs( localOrigin.y ) + std::fabs( localOrigin.z );
				Scalar cornerMax = Scalar(0);
				for( int c = 0; c < 8; c++ ) {
					const Scalar x = ( c & 1 ) ? bb.ur.x : bb.ll.x;
					const Scalar y = ( c & 2 ) ? bb.ur.y : bb.ll.y;
					const Scalar z = ( c & 4 ) ? bb.ur.z : bb.ll.z;
					const Scalar l1 = std::fabs( x ) + std::fabs( y ) + std::fabs( z );
					if( l1 > cornerMax ) {
						cornerMax = l1;
					}
				}
				// The triangle gate ADDS the vertex scale to the origin's; the
				// patch gate MAXes them.  Adding is the larger (and so the
				// safe) of the two for both.
				coordScale += cornerMax;
				return NEARZERO * ( Scalar(1) + coordScale );
			}
		};
	}
}

#endif
