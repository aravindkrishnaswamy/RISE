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
			Vector3	vEdgesA[2];
			Vector3	vEdgesB[2];
			bool	bDoubleSided;

		public:
			ClippedPlaneGeometry( const Point3 (&vP_)[4], const bool bDoubleSided_ );

			// Geometry interface
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
