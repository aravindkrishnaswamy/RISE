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

			void GenerateMesh( );
			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const; 
			BoundingBox GenerateBoundingBox() const;
			inline bool DoPreHitTest( ) const { return true; };

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const;
			Scalar GetArea( ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
		};
	}
}

#endif
