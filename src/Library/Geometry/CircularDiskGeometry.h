//////////////////////////////////////////////////////////////////////
//
//  CircularDiskGeometry.h - Definition of the CircularDisk
//  class which defines a circular disk situated on a plane
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 24, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CIRCULARDISK_GEOMETRY_
#define CIRCULARDISK_GEOMETRY_

#include "Geometry.h"

namespace RISE
{
	namespace Implementation
	{
		class CircularDiskGeometry : public Geometry
		{
		protected:
			virtual ~CircularDiskGeometry( );

			// Assume the disk is on the same plane as a default infinite plane
			// Also assume that the center of the disk is at 0,0,0 and that the
			// disk is transformed into place
			Scalar			radius;
			const char		chAxis;
			Scalar			sqrRadius;
			Scalar			OVRadius;

		public:
			CircularDiskGeometry( const Scalar radius_, const unsigned char chAxis_ );

			// Geometry interface
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
