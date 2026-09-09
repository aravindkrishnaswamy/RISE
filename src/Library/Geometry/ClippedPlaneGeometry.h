//////////////////////////////////////////////////////////////////////
//
//  ClippedPlaneGeometry.h - Definition of the ClippedPlaneGeometry
//  class which defines a clipped plane( ie. a quad)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 16, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CLIPPEDPLANE_GEOMETRY_
#define CLIPPEDPLANE_GEOMETRY_

#include "Geometry.h"
#include <algorithm>		// std::max (SelfHitRootFloor over the four corners)

namespace RISE
{
	namespace Implementation
	{
		class ClippedPlaneGeometry : public Geometry
		{
		protected:
			virtual ~ClippedPlaneGeometry( );

			Point3	vP[4];
			Vector3	vNormal;
			Vector3	vNormalA;
			Vector3	vNormalB;
			Vector3	vEdgesA[2];
			Vector3	vEdgesB[2];
			bool	bDoubleSided;

			//! ARE THE FOUR CORNERS COPLANAR?  Decided once, in
			//! RegenerateData (which the constructor and every keyframed
			//! corner edit run), because the answer is a property of the
			//! authored corners and re-deciding it per proximity query
			//! would be four cross products on a hot path
			//! (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 5.2).
			//!
			//! IT IS NOT A PEDANTIC CASE.  This geometry stores four
			//! ARBITRARY corners and traces the BILINEAR surface through
			//! them, which for non-coplanar corners is genuinely curved and
			//! has no closed-form point-to-surface distance.  A planar quad
			//! -- which is what every `rect_light` and every hand-authored
			//! panel actually is -- does, and gets it.  Anything else
			//! REFUSES rather than answering with the flat quad's distance,
			//! which could be SMALLER than the true one and so over-read
			//! contact: the one direction the signal must never fail in.
			bool	bCornersCoplanar;

			//! AND ARE THEY CONVEX?  Decided in the same place, and needed
			//! for the same reason coplanarity is: what this class TRACES
			//! is the bilinear patch through the four corners, and only for
			//! a coplanar CONVEX quad is that patch's image the polygon the
			//! corners outline.  For a coplanar DART (one corner inside the
			//! triangle of the other three) the image is a proper subset of
			//! the polygon, so the point-to-polygon form UNDER-reports over
			//! the reflex lobe -- contact painted where there is none, the
			//! one direction this signal must never fail in -- and
			//! over-reports outside.  A dart therefore REFUSES, exactly as
			//! a non-coplanar quad does.  Meaningful only when
			//! `bCornersCoplanar` (the test needs the plane basis).
			//!
			//! Every `rect_light` and every hand-authored panel is a convex
			//! planar quad, so nothing in the acceptance set is affected;
			//! this closes a case a scene CAN author, not a hypothetical.
			bool	bCornersConvex;

			//! Unit normal of that plane, and a unit in-plane basis for the
			//! point-in-polygon test.  Meaningful only when
			//! `bCornersCoplanar`.
			Vector3	vPlaneNormal;
			Vector3	vPlaneU;
			Vector3	vPlaneV;

		public:
			ClippedPlaneGeometry( const Point3 (&vP_)[4], const bool bDoubleSided_ );

			// Geometry interface
			// Tessellates the quad as a bilinear (detail+1) x (detail+1) grid.  Corner mapping:
			// vP[0]->UV(0,0), vP[1]->UV(1,0), vP[2]->UV(1,1), vP[3]->UV(0,1).  All verts receive
			// the averaged quad normal (vNormal).  Works cleanly on planar quads; non-planar
			// quads are approximated.
			bool TessellateToMesh( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail ) const override;

			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const override; 
			BoundingBox GenerateBoundingBox() const override;
			inline bool DoPreHitTest( ) const override { return true; };

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const override;
			Scalar GetArea( ) const override;

			SurfaceDerivatives ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const override;

			//! IGeometry::DistanceToSurface -- EXACT on a PLANAR **CONVEX** quad,
			//! REFUSES otherwise -- the four corners are arbitrary, so this
			//! geometry is a bilinear patch whose point-to-surface distance has
			//! no closed form unless the corners are both coplanar AND convex
			//! (which every rect_light's are).  Both are decided ONCE, in
			//! RegenerateData (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 5.2); see
			//! `bCornersConvex` for why coplanarity alone is not enough.
			bool DistanceToSurface( const Point3& ptObject, const Scalar maxDistObject, Scalar& outDist ) const override;

			//! IGeometry::SelfHitRootFloor -- RayBilinearPatchIntersection's
			//! debt-21 gate, `NEARZERO * (1 + max(|origin|_1, max corner |.|_1))`.
			//! Direction-independent.
			Scalar SelfHitRootFloor( const Point3& localOrigin, const Vector3& localDir, const Vector3& localNormal ) const override
			{
				(void)localDir; (void)localNormal;
				Scalar coordScale =
					std::fabs( localOrigin.x ) + std::fabs( localOrigin.y ) + std::fabs( localOrigin.z );
				for( int ci = 0; ci < 4; ci++ ) {
					coordScale = std::max( coordScale,
						std::fabs( vP[ci].x ) + std::fabs( vP[ci].y ) + std::fabs( vP[ci].z ) );
				}
				return NEARZERO * ( Scalar(1) + coordScale );
			}

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) override;
			void SetIntermediateValue( const IKeyframeParameter& val ) override;
			void RegenerateData( ) override;
		};
	}
}

#endif
