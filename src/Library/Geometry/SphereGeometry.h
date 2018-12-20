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

			void GenerateMesh( );
			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const; 
			BoundingBox GenerateBoundingBox() const;
			inline bool DoPreHitTest( ) const { return false; };

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
