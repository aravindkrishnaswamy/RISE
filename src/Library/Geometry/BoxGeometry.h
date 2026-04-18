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
			// Tessellates the 6 box faces, each as a (detail+1) x (detail+1) grid in face-local UV.
			// Edges between faces are NOT shared — each face gets its own vertices with the face's
			// own UV and normal.  Minimum detail = 1 (two triangles per face).
			bool TessellateToMesh( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail ) const;

			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const; 
			BoundingBox GenerateBoundingBox() const;
			inline bool DoPreHitTest( ) const { return false; };

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
