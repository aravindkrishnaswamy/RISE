//////////////////////////////////////////////////////////////////////
//
//  TorusGeometry.h - Definition of a torus
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 12, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TORUS_GEOMETRY_
#define TORUS_GEOMETRY_

#include "Geometry.h"

namespace RISE
{
	namespace Implementation
	{
		class TorusGeometry : public Geometry
		{
		protected:
			Scalar			m_dMajorRadius;
			Scalar			m_dMinorRadius;
			Scalar			m_p0;
			Scalar			m_p1;
			Scalar			m_sqrP0;
			Scalar			m_sqrP1;

			virtual ~TorusGeometry( );

		public:
			TorusGeometry( const Scalar dMajorRadius, const Scalar dMinorRadius );

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
