//////////////////////////////////////////////////////////////////////
//
//  InfinitePlaneGeometry.h - Definition of the InfinitePlaneGeometry
//  class which defines an infinite plane
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 25, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef INFINITEPLANE_GEOMETRY_
#define INFINITEPLANE_GEOMETRY_

#include "Geometry.h"

namespace RISE
{
	namespace Implementation
	{
		class InfinitePlaneGeometry : public Geometry
		{
		protected:
			virtual ~InfinitePlaneGeometry( );

			Scalar			xTile;
			Scalar			yTile;

			Scalar			OVXTile;
			Scalar			OVYTile;

		public:
			InfinitePlaneGeometry( const Scalar xTile_, const Scalar yTile_ );

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
