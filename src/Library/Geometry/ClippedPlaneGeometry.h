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

		public:
			ClippedPlaneGeometry( const Point3 (&vP_)[4], const bool bDoubleSided_ );

			// Geometry interface
			// Tessellates the quad as a bilinear (detail+1) x (detail+1) grid.  Corner mapping:
			// vP[0]->UV(0,0), vP[1]->UV(1,0), vP[2]->UV(1,1), vP[3]->UV(0,1).  All verts receive
			// the averaged quad normal (vNormal).  Works cleanly on planar quads; non-planar
			// quads are approximated.
			bool TessellateToMesh( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail ) const;

			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const; 
			BoundingBox GenerateBoundingBox() const;
			inline bool DoPreHitTest( ) const { return true; };

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const;
			Scalar GetArea( ) const;

			SurfaceDerivatives ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const;

			//! IGeometry::SelfHitRootFloor -- RayBilinearPatchIntersection's
			//! debt-21 gate, `NEARZERO * (1 + max(|origin|_1, max corner |.|_1))`.
			//! Direction-independent.
			Scalar SelfHitRootFloor( const Point3& localOrigin, const Vector3& localDir, const Vector3& localNormal ) const
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
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
		};
	}
}

#endif
