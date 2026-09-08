//////////////////////////////////////////////////////////////////////
//
//  SphereGeometry.h - Definition of a sphere
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 31, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SPHERE_GEOMETRY_
#define SPHERE_GEOMETRY_

#include "Geometry.h"

namespace RISE
{
	namespace Implementation
	{
		class SphereGeometry : public Geometry
		{
		protected:
			Scalar			m_dRadius;
			Scalar			m_dSqrRadius;
			Scalar			m_dOVRadius;

			virtual ~SphereGeometry( );

		public:
			SphereGeometry( Scalar dRadius );

			// Tessellates the sphere to a triangle mesh with (detail+1) x (detail+1) vertices.
			// `detail` is the number of segments along each natural parameter axis (theta, phi).
			// Seam vertices at u=0 / u=1 are duplicated to keep UV continuous under displacement.
			bool TessellateToMesh( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail ) const override;

			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const override; 
			BoundingBox GenerateBoundingBox() const override;
			inline bool DoPreHitTest( ) const override { return false; };

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const override;
			Scalar GetArea( ) const override;

			SurfaceDerivatives ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const override;

			//! IGeometry::DistanceToSurface -- EXACT: | |p| - R |
			//! (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 5.2).
			bool DistanceToSurface( const Point3& ptObject, const Scalar maxDistObject, Scalar& outDist ) const override;

			//! IGeometry::SelfHitRootFloor -- RaySphereIntersection's own gate,
			//! `NEARZERO * (1 + |origin|_1 + radius)`.  Direction-independent (it
			//! is a floor on the quadratic's roots, not a plane distance), so
			//! `localDir` / `localNormal` are unused.
			Scalar SelfHitRootFloor( const Point3& localOrigin, const Vector3& localDir, const Vector3& localNormal ) const override
			{
				(void)localDir; (void)localNormal;
				return NEARZERO * ( Scalar(1) +
					std::fabs( localOrigin.x ) + std::fabs( localOrigin.y ) + std::fabs( localOrigin.z ) +
					std::fabs( m_dRadius ) );
			}

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) override;
			void SetIntermediateValue( const IKeyframeParameter& val ) override;
			void RegenerateData( ) override;
		};
	}
}

#endif
