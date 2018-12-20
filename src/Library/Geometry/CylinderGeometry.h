//////////////////////////////////////////////////////////////////////
//
//  CylinderGeometry.h - Definition of a cylinder
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 20, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CYLINDER_GEOMETRY_
#define CYLINDER_GEOMETRY_

#include "Geometry.h"

namespace RISE
{
	namespace Implementation
	{
		class CylinderGeometry : public Geometry
		{
		protected:
			const int		m_chAxis;
			Scalar			m_dRadius;
			Scalar			m_dOVRadius;
			Scalar			m_dHeight;
			Scalar			m_dAxisMin;
			Scalar			m_dAxisMax;

			virtual ~CylinderGeometry( );

		public:
			CylinderGeometry( const int chAxis, const Scalar dRadius, const Scalar dHeight );

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
