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
#include <algorithm>		// std::max (SelfHitRootFloor's grazing clamp)

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

			//! IGeometry::SelfHitRootFloor -- unlike every other primitive the
			//! box's self-hit gate (DropSelfHitRoot's `onFace`) is a PLANE
			//! DISTANCE band on the face's own axis,
			//!   eps = 4*NEARZERO + 64*DBL_EPSILON*|origin.axis|,
			//! not a floor on the root.  A ray leaving `localOrigin` along
			//! `localDir` covers that plane distance after
			//! `eps / |localDir . localNormal|` of range, so the band is divided
			//! by the incidence cosine to answer in the interface's range units.
			//! The cosine is clamped at 1/20 (the same grazing clamp
			//! CSGObject's probe already used on its own rate term) so a
			//! near-tangential query returns a large but finite floor rather
			//! than infinity; a caller that cannot meet it takes whatever
			//! fallback it has.  The axis is picked by `localNormal`'s largest
			//! component -- box faces are axis-aligned in this frame, so that
			//! names the face exactly.
			Scalar SelfHitRootFloor( const Point3& localOrigin, const Vector3& localDir, const Vector3& localNormal ) const
			{
				const Scalar ax = std::fabs( localNormal.x );
				const Scalar ay = std::fabs( localNormal.y );
				const Scalar az = std::fabs( localNormal.z );
				const Scalar oAxis = ( ax >= ay && ax >= az ) ? localOrigin.x
				                   : ( ay >= az )             ? localOrigin.y
				                                              : localOrigin.z;
				constexpr Scalar kUlpFactor = 64.0 * 2.2204460492503131e-16;   // 64 * DBL_EPSILON
				const Scalar band = Scalar(4) * NEARZERO + kUlpFactor * std::fabs( oAxis );
				const Scalar cosI = std::max(
					std::fabs( localDir.x * localNormal.x + localDir.y * localNormal.y + localDir.z * localNormal.z ),
					Scalar(0.05) );
				return band / cosI;
			}

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
		};
	}
}

#endif
