//////////////////////////////////////////////////////////////////////
//
//  EllipsoidGeometry.h - Definition of an ellipsoid
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 7, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ELLIPSOID_GEOMETRY_
#define ELLIPSOID_GEOMETRY_

#include "Geometry.h"

namespace RISE
{
	namespace Implementation
	{
		class EllipsoidGeometry : public Geometry
		{
		protected:
			Vector3			m_vRadius;
			Matrix4			Q;					// Matrix representation of ellipsoid
			Scalar			m_OVmaxRadius;		// Inverse of max radius (for fast spherical UV)

			// Marginal theta CDF for area-uniform sampling on the ellipsoid surface.
			// The naive (costheta = 1-2u) mapping is only uniform on a sphere;
			// for an ellipsoid we precompute a CDF that warps theta to account
			// for the varying area element.
			static const unsigned int THETA_CDF_SIZE = 256;
			Scalar			m_thetaCDF[THETA_CDF_SIZE + 1];

			virtual ~EllipsoidGeometry( );

		public:
			EllipsoidGeometry( const Vector3& vRadius );

			// Tessellates the ellipsoid to a triangle mesh with (detail+1) x (detail+1) vertices.
			// Same parameterization as sphere, scaled by the ellipsoid's semi-axes.
			// Note: m_vRadius stores diameters; semi-axes are m_vRadius / 2.
			bool TessellateToMesh( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail ) const;

			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const; 
			BoundingBox GenerateBoundingBox() const;
			inline bool DoPreHitTest( ) const { return true; };

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const;
			Scalar GetArea( ) const;

			SurfaceDerivatives ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
		};
	}
}

#endif
