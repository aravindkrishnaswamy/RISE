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

#include <algorithm>	// std::max (BoundingBoxRootFloor)
#include <cmath>		// std::fabs / std::isfinite (same)

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
			//!
			//! UNBUILT / UNBOUNDED GUARD (adversarial review of 4b141ad3, P1-3):
			//! an empty collection reports the DEFAULT `BoundingBox()`, whose
			//! corners are +-RISE_INFINITY (== DBL_MAX) -- an unbuilt
			//! `TriangleMeshGeometryIndexed` (null BVH) and an empty
			//! `BilinearPatchGeometry` (empty tree) both do.  Summing three of
			//! those OVERFLOWS to +inf, so the floor came back INFINITE, and the
			//! one caller -- `CSGObject::AdoptCsgExitFacePayloadViaProbe` -- then
			//! built an infinite margin, a probe origin carrying a NaN component
			//! (0 * inf on any zero direction component), and a
			//! `range > maxAcceptRange` rejection that can NEVER fire, because
			//! every comparison against NaN is false.  So a bbox that is not a
			//! real, built, finite box contributes NOTHING: fall back to the same
			//! generic `NEARZERO * (1 + |o|_1)` floor `IGeometry::SelfHitRootFloor`
			//! defaults to.  That is not an under-statement in any case that
			//! matters -- there are no primitives there to gate.  (The probe carries
			//! its own `std::isfinite` guard as a second layer, for any other
			//! source of a non-finite floor.)
			static Scalar BoundingBoxRootFloor( const BoundingBox& bb, const Point3& localOrigin )
			{
				const Scalar coordScale =
					std::fabs( localOrigin.x ) + std::fabs( localOrigin.y ) + std::fabs( localOrigin.z );
				const Scalar generic = NEARZERO * ( Scalar(1) + coordScale );

				// max over the 8 corners of (|x| + |y| + |z|), taken PER AXIS so a
				// sentinel box cannot overflow the sum before it can be tested.  The
				// three axes are independent, so this is exactly the corner maximum
				// the loop it replaces computed.
				const Scalar ax = std::max( std::fabs( bb.ll.x ), std::fabs( bb.ur.x ) );
				const Scalar ay = std::max( std::fabs( bb.ll.y ), std::fabs( bb.ur.y ) );
				const Scalar az = std::max( std::fabs( bb.ll.z ), std::fabs( bb.ur.z ) );

				// 1e30 is RISE's own "effectively unbounded" coordinate sentinel
				// (Ray::RecomputeInvDir), far above any real scene coordinate and far
				// below DBL_MAX -- it catches the +-RISE_INFINITY default box without
				// rejecting anything an author could plausibly build.  The ll <= ur
				// test catches an INVERTED (empty-seeded) box as well.
				const Scalar kMaxSaneCoord = Scalar(1e30);
				const bool built =
					bb.ll.x <= bb.ur.x && bb.ll.y <= bb.ur.y && bb.ll.z <= bb.ur.z &&
					std::isfinite( ax ) && std::isfinite( ay ) && std::isfinite( az ) &&
					ax < kMaxSaneCoord && ay < kMaxSaneCoord && az < kMaxSaneCoord;
				if( !built ) {
					return generic;
				}

				// The triangle gate ADDS the vertex scale to the origin's; the
				// patch gate MAXes them.  Adding is the larger (and so the
				// safe) of the two for both.
				return NEARZERO * ( Scalar(1) + coordScale + ax + ay + az );
			}
		};
	}
}

#endif
