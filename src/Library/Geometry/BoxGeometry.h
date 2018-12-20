//////////////////////////////////////////////////////////////////////
//
//  BoxGeometry.h - Definition of a box
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 10, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef _BOX_GEOMETRY_
#define _BOX_GEOMETRY_

#include "Geometry.h"

namespace RISE
{
	namespace Implementation
	{
		class BoxGeometry : public Geometry
		{
		protected:
			Scalar		dWidth;
			Scalar		dHeight;
			Scalar		dDepth;

			//
			// For optimization purposes...
			//
			Scalar		dOVWidth;
			Scalar		dOVHeight;
			Scalar		dOVDepth;

			Scalar		dWidthOV2;
			Scalar		dHeightOV2;
			Scalar		dDepthOV2;

			virtual ~BoxGeometry( );

		public:
			BoxGeometry( Scalar dWidth_, Scalar dHeight_, Scalar dDepth_ );

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
