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
			const bool		m_bCapped;		///< TRUE: closed solid (two end-cap disks); FALSE: open tube (side only)

			virtual ~CylinderGeometry( );

			// Surface-region codes returned by IntersectCappedSolid / consumed by
			// FillSurfaceNormalUV.  Only meaningful when m_bCapped is TRUE.
			enum CylSurface { SURF_SIDE = 0, SURF_CAPMIN = 1, SURF_CAPMAX = 2 };

			//! Closed-solid ray intersection (side wall + two cap disks).  Returns the
			//! nearest boundary crossing ahead of the ray (tNear/surfNear) and, when the
			//! ray enters from outside, the exit crossing (tFar/surfFar).  When the ray
			//! origin is inside the solid there is a single crossing: tFar is set to 0 and
			//! surfFar mirrors surfNear (matching the open-tube "started inside" convention).
			bool IntersectCappedSolid( const Ray& ray, Scalar& tNear, int& surfNear, Scalar& tFar, int& surfFar ) const;

			//! Fills the outward normal (and, if coord != 0, the UV) for a point known to lie
			//! on surface region `surf` of the capped cylinder.
			void FillSurfaceNormalUV( const Point3& pt, int surf, Vector3& normal, Point2* coord ) const;

		public:
			CylinderGeometry( const int chAxis, const Scalar dRadius, const Scalar dHeight, const bool capped );

			// Tessellates the cylinder side to (detail+1) x (detail+1) vertices (duplicating the
			// u=0/u=1 seam).  When m_bCapped, appends a triangle fan for each end cap so the
			// tessellation is a closed solid matching the analytic intersection.
			bool TessellateToMesh( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail ) const override;

			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const override; 
			BoundingBox GenerateBoundingBox() const override;
			inline bool DoPreHitTest( ) const override { return true; };

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const override;
			Scalar GetArea( ) const override;

			SurfaceDerivatives ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const override;

			//! IGeometry::DistanceToSurface -- EXACT for BOTH forms this class takes -- the
			//! capped solid (side wall plus two cap disks) and the open tube
			//! (side wall only), which are different surfaces and get different
			//! closed forms
			//! (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 5.2).
			bool DistanceToSurface( const Point3& ptObject, const Scalar maxDistObject, Scalar& outDist ) const override;

			//! IGeometry::SelfHitRootFloor -- the gate both
			//! CylinderGeometry::IntersectCappedSolid and the open-tube
			//! Ray{X,Y,Z}CylinderIntersection apply,
			//! `NEARZERO * (1 + |origin|_1 + radius)` (the axis passes through
			//! this frame's origin, so their axO/raO/rbO split is exactly the
			//! origin's L1).  Direction-independent.
			Scalar SelfHitRootFloor( const Point3& localOrigin, const Vector3& localDir, const Vector3& localNormal ) const override
			{
				(void)localDir; (void)localNormal;
				return NEARZERO * ( Scalar(1) +
					std::fabs( localOrigin.x ) + std::fabs( localOrigin.y ) + std::fabs( localOrigin.z ) +
					std::fabs( m_dRadius ) );
			}

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) override;
			void SetIntermediateValue( const IKeyframeParameter& val ) override;
			void RegenerateData( ) override;
		};
	}
}

#endif
