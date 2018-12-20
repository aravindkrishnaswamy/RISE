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
